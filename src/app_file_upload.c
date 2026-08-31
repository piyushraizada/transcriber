/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * app_file_upload.c — File Upload, Validation & Transcription Pipeline
 *
 * This file implements the file-based transcription workflow:
 * - File format detection using FFmpeg (any container/codec FFmpeg can
 *   decode — no extension allowlist; FFmpeg errors are surfaced to the
 *   user)
 * - File size validation (max 1GB); duration validation (2s-600s) is
 *   enforced after decode, since container duration metadata is not
 *   always present
 * - FFmpeg decode + resample to 16kHz mono 16-bit PCM
 * - Background transcription pipeline (FileUploadJob): VAD segmentation,
 *   per-segment whisper.cpp transcription, incremental output file writes,
 *   cancellation with partial-file cleanup
 *
 * Threading:
 *   - file_upload_validate() / file_upload_decode_to_pcm() are synchronous
 *     and run on the GTK main thread (local-file probes are fast).
 *   - file_upload_run_pipeline() runs on a dedicated GThread. It guards
 *     whisper calls with the caller-supplied whisper_mutex (the same mutex
 *     used by live/continuous transcription) and reports progress via the
 *     job callback, which the caller marshals to the GTK thread.
 */

#include "app_file_upload.h"
#include "app.h"
#include "app_whisper.h"
#include "app_llama.h"
#include "app_file_segmenter.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* FFmpeg includes */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

/*
 * Private helper functions
 *****************************************************************************/

static gboolean validate_file_exists(const gchar *filepath) {
    struct stat st;
    if (stat(filepath, &st) != 0) {
        return FALSE;
    }
    
    /* Check if it's a regular file */
    if (!S_ISREG(st.st_mode)) {
        return FALSE;
    }
    
    /* Check if file is readable */
    if (access(filepath, R_OK) != 0) {
        return FALSE;
    }
    
    return TRUE;
}

/*
 * probe_audio_file — Probe a file with FFmpeg to confirm it is decodable
 * audio, and (best-effort) extract its duration.
 *
 * Format support is defined by FFmpeg itself: any container/codec FFmpeg
 * can open that contains an audio stream is accepted — there is no
 * extension allowlist. On failure the human-readable FFmpeg error
 * (av_strerror) is stored in *out_error so the caller can show it to the
 * user.
 *
 * Duration is best-effort: many containers (e.g. CAF files produced by
 * streaming recorders) carry no duration metadata. When unknown,
 * *out_duration is left at 0.0 and the file is still valid — the true
 * duration is computed from the decoded sample count in the pipeline.
 *
 * @param filepath: Path to the file to probe
 * @param out_duration: Receives duration in seconds, or 0.0 if unknown
 * @param out_error: Receives a g_strdup'd error message on failure (the
 *                   caller frees it with g_free()); set to NULL on success
 * @return TRUE if the file is decodable audio, FALSE otherwise
 */
static gboolean probe_audio_file(const gchar *filepath, double *out_duration,
                                 gchar **out_error) {
    AVFormatContext *fmt_ctx = NULL;

    *out_duration = 0.0;
    *out_error = NULL;

    int ret = avformat_open_input(&fmt_ctx, filepath, NULL, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        *out_error = g_strdup_printf("FFmpeg: %s", errbuf);
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Probe failed for '%s': %s\n", filepath, errbuf);
        return FALSE;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        *out_error = g_strdup_printf("FFmpeg: %s", errbuf);
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Probe failed for '%s': %s\n", filepath, errbuf);
        avformat_close_input(&fmt_ctx);
        return FALSE;
    }

    int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO,
                                        -1, -1, NULL, 0);
    if (audio_idx < 0) {
        *out_error = g_strdup("No audio stream found in file");
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] No audio stream found in: %s\n", filepath);
        avformat_close_input(&fmt_ctx);
        return FALSE;
    }

    /* Best-effort duration: stream duration is in the stream's own
     * time_base (e.g. 1/144000 for MP3), NOT AV_TIME_BASE — rescale it.
     * Fall back to the container duration (already in AV_TIME_BASE). If
     * neither is available the duration stays 0.0 (unknown). */
    AVStream *audio_stream = fmt_ctx->streams[audio_idx];
    AVRational time_base = { 1, AV_TIME_BASE };
    int64_t dur_tb = (audio_stream->duration > 0)
        ? av_rescale_q(audio_stream->duration, audio_stream->time_base, time_base)
        : (fmt_ctx->duration > 0 ? fmt_ctx->duration : 0);
    if (dur_tb > 0) {
        *out_duration = (double)dur_tb / (double)AV_TIME_BASE;
    }

    avformat_close_input(&fmt_ctx);
    return TRUE;
}

/*
 * Public API Functions
 *****************************************************************************/

int file_upload_init(FileUploadContext *ctx, const gchar *filepath,
                     void (*on_validation_complete)(FileUploadContext *ctx, gboolean success, const gchar *error),
                     void *user_data) {
    if (!ctx || !filepath) {
        return -1;
    }
    
    /* Zero-initialize the struct */
    memset(ctx, 0, sizeof(FileUploadContext));
    
    /* Copy the file path */
    ctx->filepath = g_strdup(filepath);
    if (!ctx->filepath) {
        return -1;
    }
    
    /* Extract filename from path */
    gchar *basename = g_path_get_basename(filepath);
    ctx->filename = basename;
    
    /* Set up callbacks */
    ctx->on_validation_complete = on_validation_complete;
    ctx->user_data = user_data;
    
    return 0;
}

int file_upload_validate(FileUploadContext *ctx) {
    if (!ctx || !ctx->filepath) {
        return -1;
    }
    
    /* Step 1: Check if file exists and is readable */
    if (!validate_file_exists(ctx->filepath)) {
        ctx->is_valid = FALSE;
        ctx->error_message = g_strdup("File not found or not readable");
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Validation failed: %s\n", ctx->error_message);

        if (ctx->on_validation_complete) {
            ctx->on_validation_complete(ctx, FALSE, ctx->error_message);
        }
        return -1;
    }

    /* Step 2: Check file size (max 1GB = 1073741824 bytes) */
    struct stat st;
    if (stat(ctx->filepath, &st) != 0) {
        ctx->is_valid = FALSE;
        ctx->error_message = g_strdup("Could not get file size");
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Validation failed: %s\n", ctx->error_message);

        if (ctx->on_validation_complete) {
            ctx->on_validation_complete(ctx, FALSE, ctx->error_message);
        }
        return -1;
    }

    if (st.st_size > 1073741824) {
        ctx->is_valid = FALSE;
        ctx->error_message = g_strdup("File too large (max 1GB)");
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Validation failed: %s\n", ctx->error_message);

        if (ctx->on_validation_complete) {
            ctx->on_validation_complete(ctx, FALSE, ctx->error_message);
        }
        return -1;
    }

    ctx->file_size = st.st_size;

    /* Step 3: Probe with FFmpeg — the file must be decodable and contain
     * an audio stream. Format support is whatever FFmpeg supports (no
     * extension allowlist); on failure the FFmpeg error is stored in
     * ctx->error_message so it can be shown to the user.
     *
     * Duration is best-effort here: containers without duration metadata
     * (e.g. CAF files from streaming recorders) leave ctx->duration at
     * 0.0. The authoritative 2-600 s check runs in the pipeline after
     * decode, where the true duration is known from the sample count. */
    {
        gchar *probe_error = NULL;
        if (!probe_audio_file(ctx->filepath, &ctx->duration, &probe_error)) {
            ctx->is_valid = FALSE;
            ctx->error_message = probe_error ? probe_error
                                             : g_strdup("Could not read audio file");
            g_log("app-file", G_LOG_LEVEL_WARNING,
                  "[file] Validation failed: %s\n", ctx->error_message);

            if (ctx->on_validation_complete) {
                ctx->on_validation_complete(ctx, FALSE, ctx->error_message);
            }
            return -1;
        }
    }

    /* All validations passed */
    ctx->is_valid = TRUE;
    ctx->error_message = NULL;

    if (ctx->duration > 0.0) {
        g_log("app-file", G_LOG_LEVEL_INFO,
              "[file] Validation successful: %s (%.1fs, %zu bytes)\n",
              ctx->filename, ctx->duration, ctx->file_size);
    } else {
        g_log("app-file", G_LOG_LEVEL_INFO,
              "[file] Validation successful: %s (duration unknown, %zu bytes)\n",
              ctx->filename, ctx->file_size);
    }
    
    if (ctx->on_validation_complete) {
        ctx->on_validation_complete(ctx, TRUE, NULL);
    }
    
    return 0;
}

void file_upload_cleanup(FileUploadContext *ctx) {
    if (!ctx) {
        return;
    }
    
    /* Remove temporary file if it exists. Must happen before freeing
     * ctx->temp_filepath to avoid a use-after-free. Uses POSIX unlink()
     * (from <unistd.h>) since glib/gfileutils.h is not included here. */
    if (ctx->temp_filepath && g_file_test(ctx->temp_filepath, G_FILE_TEST_EXISTS)) {
        unlink(ctx->temp_filepath);
    }

    /* Free allocated strings */
    g_free(ctx->filepath);
    g_free(ctx->filename);
    g_free(ctx->temp_filepath);
    g_free(ctx->error_message);

    memset(ctx, 0, sizeof(FileUploadContext));
}

FileUploadResult file_upload_get_result(FileUploadContext *ctx) {
    FileUploadResult result;
    memset(&result, 0, sizeof(result));
    if (!ctx) {
        return result;
    }

    result.success = ctx->is_valid;
    result.error_message = ctx->error_message;
    result.duration = ctx->duration;
    result.file_size = ctx->file_size;

    return result;
}

const gchar *file_upload_get_error(FileUploadError error) {
    switch (error) {
        case FILE_UPLOAD_ERROR_FILE_NOT_FOUND:
            return "File not found";
        case FILE_UPLOAD_ERROR_INVALID_FORMAT:
            return "Unsupported audio format";
        case FILE_UPLOAD_ERROR_FILE_TOO_LARGE:
            return "File too large (max 1GB)";
        case FILE_UPLOAD_ERROR_FILE_TOO_SMALL:
            return "File too short (min 2s)";
        case FILE_UPLOAD_ERROR_PERMISSION_DENIED:
            return "Permission denied";
        case FILE_UPLOAD_ERROR_READ_FAILED:
            return "Failed to read file";
        case FILE_UPLOAD_ERROR_PCM_EXTRACTION_FAILED:
            return "Failed to extract PCM audio";
        case FILE_UPLOAD_ERROR_TEMP_FILE_CREATION_FAILED:
            return "Failed to create temporary file";
        case FILE_UPLOAD_ERROR_NONE:
        default:
            return "No error";
    }
}

/*
 * Decode + resample
 *****************************************************************************/

/* whisper.cpp input format — fixed, not configurable */
#define DECODE_TARGET_RATE   16000

/* Grow a PCM buffer to at least `needed` samples. Returns the (possibly
 * reallocated) buffer, or NULL on allocation failure. */
static int16_t *pcm_buffer_grow(int16_t **buf, size_t *cap, size_t needed) {
    if (needed <= *cap && *buf) {
        return *buf;
    }
    size_t new_cap = *cap ? *cap : 16000;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    int16_t *nb = g_realloc(*buf, new_cap * sizeof(int16_t));
    if (!nb) {
        return NULL;
    }
    *buf = nb;
    *cap = new_cap;
    return nb;
}

int file_upload_decode_to_pcm(FileUploadContext *ctx,
                              int16_t **out_samples, size_t *out_count,
                              gchar **out_error) {
    if (!ctx || !ctx->filepath || !out_samples || !out_count) {
        return -FILE_UPLOAD_ERROR_READ_FAILED;
    }
    *out_samples = NULL;
    *out_count = 0;
    *out_error = NULL;

    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec = NULL;
    SwrContext *swr = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    int16_t *pcm = NULL;
    size_t pcm_cap = 0;
    size_t pcm_count = 0;
    int result = -FILE_UPLOAD_ERROR_PCM_EXTRACTION_FAILED;

    int ret = avformat_open_input(&fmt_ctx, ctx->filepath, NULL, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        *out_error = g_strdup_printf("FFmpeg: %s", errbuf);
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Decode failed: could not open '%s': %s\n",
              ctx->filepath, errbuf);
        return -FILE_UPLOAD_ERROR_READ_FAILED;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        *out_error = g_strdup_printf("FFmpeg: %s", errbuf);
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Decode failed: no stream info in '%s': %s\n",
              ctx->filepath, errbuf);
        goto cleanup;
    }

    int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO,
                                        -1, -1, NULL, 0);
    if (audio_idx < 0) {
        *out_error = g_strdup("No audio stream found in file");
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Decode failed: no audio stream in '%s'\n", ctx->filepath);
        result = -FILE_UPLOAD_ERROR_INVALID_FORMAT;
        goto cleanup;
    }

    AVStream *stream = fmt_ctx->streams[audio_idx];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        *out_error = g_strdup_printf(
            "No decoder available for codec '%s' in this FFmpeg build",
            avcodec_get_name(stream->codecpar->codec_id));
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Decode failed: no decoder for codec id %d in '%s'\n",
              (int)stream->codecpar->codec_id, ctx->filepath);
        goto cleanup;
    }
    codec = avcodec_alloc_context3(decoder);
    if (!codec) {
        *out_error = g_strdup("Out of memory");
        g_log("app-file", G_LOG_LEVEL_WARNING, "[file] Decode failed: OOM\n");
        goto cleanup;
    }
    ret = avcodec_parameters_to_context(codec, stream->codecpar);
    if (ret >= 0) {
        ret = avcodec_open2(codec, NULL, NULL);
    }
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        *out_error = g_strdup_printf("FFmpeg: %s", errbuf);
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Decode failed: could not open codec for '%s': %s\n",
              ctx->filepath, errbuf);
        goto cleanup;
    }

    /* Resampler: source layout/rate/format -> 16kHz mono S16 */
    AVChannelLayout out_layout;
    if (av_channel_layout_from_string(&out_layout, "mono") < 0) {
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Decode failed: could not build mono channel layout\n");
        goto cleanup;
    }
    if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, DECODE_TARGET_RATE,
                            &codec->ch_layout, codec->sample_fmt,
                            codec->sample_rate, 0, NULL) < 0 || !swr) {
        av_channel_layout_uninit(&out_layout);
        *out_error = g_strdup("Could not create audio resampler");
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Decode failed: could not create resampler for '%s'\n", ctx->filepath);
        goto cleanup;
    }
    av_channel_layout_uninit(&out_layout);
    if (swr_init(swr) < 0) {
        *out_error = g_strdup("Audio resampler initialization failed");
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Decode failed: resampler init failed for '%s'\n", ctx->filepath);
        goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        *out_error = g_strdup("Out of memory");
        g_log("app-file", G_LOG_LEVEL_WARNING, "[file] Decode failed: OOM\n");
        goto cleanup;
    }

    /* Start with no capacity — pcm_buffer_grow() doubles from 16000 samples
     * as data arrives. (Pre-sizing from the probed duration is unsafe here:
     * the buffer pointer must be NULL until the first allocation.) */
    pcm_cap = 0;

    while ((ret = av_read_frame(fmt_ctx, packet)) >= 0) {
        if (packet->stream_index != audio_idx) {
            av_packet_unref(packet);
            continue;
        }
        avcodec_send_packet(codec, packet);
        av_packet_unref(packet);

        while ((ret = avcodec_receive_frame(codec, frame)) == 0) {
            int max_out = (int)swr_get_out_samples(swr, frame->nb_samples) + 16;
            int16_t *conv_buf = g_malloc((size_t)max_out * sizeof(int16_t));
            if (!conv_buf) {
                *out_error = g_strdup("Out of memory");
                g_log("app-file", G_LOG_LEVEL_WARNING, "[file] Decode failed: OOM\n");
                goto cleanup;
            }
            uint8_t *out_ptr = (uint8_t *)conv_buf;
            int converted = swr_convert(swr, &out_ptr, max_out,
                                        (const uint8_t **)frame->extended_data,
                                        frame->nb_samples);
            av_frame_unref(frame);
            if (converted < 0) {
                *out_error = g_strdup("Audio resampling failed mid-stream");
                g_log("app-file", G_LOG_LEVEL_WARNING,
                      "[file] Decode failed: resample error for '%s'\n", ctx->filepath);
                g_free(conv_buf);
                goto cleanup;
            }
            if (converted > 0) {
                if (!pcm_buffer_grow(&pcm, &pcm_cap, pcm_count + (size_t)converted)) {
                    *out_error = g_strdup("Out of memory");
                    g_log("app-file", G_LOG_LEVEL_WARNING, "[file] Decode failed: OOM\n");
                    g_free(conv_buf);
                    goto cleanup;
                }
                memcpy(pcm + pcm_count, conv_buf, (size_t)converted * sizeof(int16_t));
                pcm_count += (size_t)converted;
            }
            g_free(conv_buf);
        }
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            *out_error = g_strdup_printf("FFmpeg: %s", errbuf);
            g_log("app-file", G_LOG_LEVEL_WARNING,
                  "[file] Decode failed: decoder error for '%s': %s\n",
                  ctx->filepath, errbuf);
            goto cleanup;
        }
    }
    if (ret < 0 && ret != AVERROR_EOF) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        *out_error = g_strdup_printf("FFmpeg: %s", errbuf);
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Decode failed: read error for '%s': %s\n",
              ctx->filepath, errbuf);
        goto cleanup;
    }

    /* Flush the decoder */
    avcodec_send_packet(codec, NULL);
    while ((ret = avcodec_receive_frame(codec, frame)) == 0) {
        int max_out = (int)swr_get_out_samples(swr, frame->nb_samples) + 16;
        int16_t *conv_buf = g_malloc((size_t)max_out * sizeof(int16_t));
        if (!conv_buf) {
            *out_error = g_strdup("Out of memory");
            g_log("app-file", G_LOG_LEVEL_WARNING, "[file] Decode failed: OOM\n");
            goto cleanup;
        }
        uint8_t *out_ptr = (uint8_t *)conv_buf;
        int converted = swr_convert(swr, &out_ptr, max_out,
                                    (const uint8_t **)frame->extended_data,
                                    frame->nb_samples);
        av_frame_unref(frame);
        if (converted < 0) {
            *out_error = g_strdup("Audio resampling failed mid-stream");
            g_free(conv_buf);
            goto cleanup;
        }
        if (converted > 0) {
            if (!pcm_buffer_grow(&pcm, &pcm_cap, pcm_count + (size_t)converted)) {
                *out_error = g_strdup("Out of memory");
                g_free(conv_buf);
                goto cleanup;
            }
            memcpy(pcm + pcm_count, conv_buf, (size_t)converted * sizeof(int16_t));
            pcm_count += (size_t)converted;
        }
        g_free(conv_buf);
    }

    if (pcm_count == 0) {
        *out_error = g_strdup("No audio samples could be decoded from file");
        g_log("app-file", G_LOG_LEVEL_WARNING,
              "[file] Decode failed: no PCM samples decoded from '%s'\n", ctx->filepath);
        goto cleanup;
    }

    *out_samples = pcm;
    *out_count = pcm_count;
    pcm = NULL;  /* ownership transferred */
    result = 0;

cleanup:
    if (pcm) {
        g_free(pcm);
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    if (swr) {
        swr_free(&swr);
    }
    if (codec) {
        avcodec_free_context(&codec);
    }
    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }
    return result;
}

/*
 * Transcription pipeline (FileUploadJob)
 *****************************************************************************/

struct FileUploadJob {
    WhisperClient *whisper_client;   /* not owned */
    LlamaClient *llama_client;       /* not owned; may be NULL */
    pthread_mutex_t *whisper_mutex;  /* not owned */
    gchar *input_path;               /* owned copy */
    gchar *output_path;              /* owned copy */
    char asr_backend[16];            /* snapshot: "whisper" or "llama" */
    char language[16];               /* snapshot: configured language */
    FILE *out_file;                  /* opened by create, closed by pipeline */
    double total_duration;
    atomic_bool cancel_requested;
    void (*on_progress)(double percent, const char *status, void *user_data);
    void *user_data;
};

FileUploadJob *file_upload_job_create(const FileUploadJobConfig *cfg) {
    if (!cfg || !cfg->whisper_client || !cfg->whisper_mutex ||
        !cfg->input_path || !cfg->output_path) {
        return NULL;
    }

    FileUploadJob *job = g_new0(FileUploadJob, 1);
    if (!job) {
        return NULL;
    }
    job->whisper_client = cfg->whisper_client;
    job->llama_client = cfg->llama_client;
    job->whisper_mutex = cfg->whisper_mutex;

    /* Snapshot the active backend and language at job creation so every
     * segment of this job is consistent, even if the config changes
     * mid-job. Empty backend string defaults to "whisper". */
    if (cfg->asr_backend[0] != '\0') {
        strncpy(job->asr_backend, cfg->asr_backend, sizeof(job->asr_backend) - 1);
        job->asr_backend[sizeof(job->asr_backend) - 1] = '\0';
    } else {
        strcpy(job->asr_backend, "whisper");
    }
    if (cfg->language[0] != '\0') {
        strncpy(job->language, cfg->language, sizeof(job->language) - 1);
        job->language[sizeof(job->language) - 1] = '\0';
    }

    job->input_path = g_strdup(cfg->input_path);
    job->output_path = g_strdup(cfg->output_path);
    job->total_duration = cfg->total_duration;
    job->on_progress = cfg->on_progress;
    job->user_data = cfg->user_data;
    atomic_init(&job->cancel_requested, false);

    job->out_file = fopen(cfg->output_path, "w");
    if (!job->out_file) {
        g_log("app-file", G_LOG_LEVEL_ERROR,
              "[file] Could not open output file '%s': %s\n",
              cfg->output_path, strerror(errno));
        g_free(job->input_path);
        g_free(job->output_path);
        g_free(job);
        return NULL;
    }

    return job;
}

void file_upload_job_destroy(FileUploadJob *job) {
    if (!job) {
        return;
    }
    /* Defensive: the pipeline normally closes the file first. If the thread
     * never ran (e.g. g_thread_new failed), close it here. */
    if (job->out_file) {
        fclose(job->out_file);
        job->out_file = NULL;
    }
    g_free(job->input_path);
    g_free(job->output_path);
    g_free(job);
}

void file_upload_job_request_cancel(FileUploadJob *job) {
    if (!job) {
        return;
    }
    atomic_store(&job->cancel_requested, true);
}

bool file_upload_job_is_cancelled(const FileUploadJob *job) {
    if (!job) {
        return false;
    }
    /* const-cast is safe — atomic_load does not logically modify state. */
    return atomic_load(&((FileUploadJob *)job)->cancel_requested);
}

void *file_upload_run_pipeline(void *arg) {
    FileUploadJob *job = (FileUploadJob *)arg;
    if (!job) {
        return NULL;
    }

    int16_t *pcm = NULL;
    size_t n_samples = 0;
    PcmSegment *segments = NULL;
    size_t seg_count = 0;
    bool cancelled = false;
    bool write_failed = false;
    bool decode_failed = false;

    /* ---- Step 1: Decode + resample to 16kHz mono PCM ---- */
    gchar *decode_error = NULL;
    {
        FileUploadContext vctx;
        if (file_upload_init(&vctx, job->input_path, NULL, NULL) == 0) {
            if (file_upload_decode_to_pcm(&vctx, &pcm, &n_samples, &decode_error) != 0) {
                decode_failed = true;
            }
            file_upload_cleanup(&vctx);
        } else {
            decode_failed = true;
        }
    }

    if (decode_failed) {
        if (job->out_file) {
            fclose(job->out_file);
            job->out_file = NULL;
        }
        unlink(job->output_path);
        if (job->on_progress) {
            if (decode_error) {
                gchar *msg = g_strdup_printf(
                    "Error: could not decode audio file — %s", decode_error);
                job->on_progress(0.0, msg, job->user_data);
                g_free(msg);
            } else {
                job->on_progress(0.0, "Error: could not decode audio file",
                                 job->user_data);
            }
        }
        g_free(decode_error);
        return NULL;
    }

    /* ---- Step 2: Duration check (authoritative) ----
     * Container metadata is not trusted for duration (e.g. CAF files from
     * streaming recorders carry none), so the 2-600 s limits are enforced
     * here, from the decoded sample count. */
    {
        double duration = (double)n_samples / DECODE_TARGET_RATE;
        if (duration < 2.0 || duration > 600.0) {
            gchar *msg = (duration < 2.0)
                ? g_strdup_printf("Error: file too short (min 2s) — got %.1fs",
                                  duration)
                : g_strdup_printf("Error: file too long (max 600s) — got %.1fs",
                                  duration);
            g_free(pcm);
            if (job->out_file) {
                fclose(job->out_file);
                job->out_file = NULL;
            }
            unlink(job->output_path);
            if (job->on_progress) {
                job->on_progress(0.0, msg, job->user_data);
            }
            g_free(msg);
            return NULL;
        }
        job->total_duration = duration;
    }

    /* ---- Step 3: VAD segmentation ---- */
    segments = file_segmenter_segment(pcm, n_samples, VAD_MODE_MODERATE, &seg_count);
    if (!segments || seg_count == 0) {
        g_free(pcm);
        if (job->out_file) {
            fclose(job->out_file);
            job->out_file = NULL;
        }
        unlink(job->output_path);
        if (job->on_progress) {
            job->on_progress(0.0, "Error: no audio segments found", job->user_data);
        }
        return NULL;
    }

    g_log("app-file", G_LOG_LEVEL_INFO,
          "[file] Transcribing '%s': %zu segment(s), %.1fs total\n",
          job->input_path, seg_count, (double)n_samples / DECODE_TARGET_RATE);

    /* ---- Step 4: Per-segment transcription with incremental writes ---- */
    size_t processed = 0;
    for (size_t i = 0; i < seg_count; i++) {
        if (atomic_load(&job->cancel_requested)) {
            cancelled = true;
            break;
        }

        int n = (int)(segments[i].end - segments[i].start);
        WhisperResponse *resp = NULL;

        /* Dispatch to the job's snapshot backend. The llama path uses the
         * fallback wrapper, which re-sends the same segment through local
         * Whisper on any hard failure (a cancellation is not retried). The
         * whole dispatch stays under whisper_mutex so file transcription
         * never races live/continuous transcription. */
        pthread_mutex_lock(job->whisper_mutex);
        if (strcmp(job->asr_backend, "llama") == 0 && job->llama_client) {
            resp = llama_transcribe_samples_fallback(job->llama_client,
                                                     job->whisper_client,
                                                     pcm + segments[i].start, n,
                                                     job->language);
        } else {
            resp = whisper_transcribe_samples(job->whisper_client,
                                              pcm + segments[i].start, n);
        }
        pthread_mutex_unlock(job->whisper_mutex);

        if (resp && resp->success && resp->text) {
            size_t text_len = strlen(resp->text);
            if (fwrite(resp->text, 1, text_len, job->out_file) != text_len ||
                fwrite("\n", 1, 1, job->out_file) != 1) {
                g_log("app-file", G_LOG_LEVEL_ERROR,
                      "[file] Failed to write segment %zu to '%s' — aborting job\n",
                      i + 1, job->output_path);
                write_failed = true;
                if (resp) whisper_response_free(resp);
                break;
            }
            fflush(job->out_file);
        } else {
            /* Per-segment failure: WARN log only, continue (spec REQ-13). */
            g_log("app-file", G_LOG_LEVEL_WARNING,
                  "[file] Segment %zu/%zu transcription failed: %s — continuing\n",
                  i + 1, seg_count,
                  resp ? resp->error_message : "unknown error");
        }
        if (resp) {
            whisper_response_free(resp);
        }

        processed += segments[i].end - segments[i].start;
        double pct = (n_samples > 0) ? (double)processed / (double)n_samples : 1.0;
        if (job->on_progress) {
            job->on_progress(pct, "Transcribing…", job->user_data);
        }
    }

    g_free(segments);
    g_free(pcm);

    /* ---- Step 5: Finalize ---- */
    if (job->out_file) {
        fclose(job->out_file);
        job->out_file = NULL;
    }

    if (cancelled) {
        /* Cancellation leaves no partial file (spec Workflow 2). */
        unlink(job->output_path);
        if (job->on_progress) {
            job->on_progress(0.0, "Cancelled", job->user_data);
        }
    } else if (write_failed) {
        unlink(job->output_path);
        if (job->on_progress) {
            job->on_progress(0.0, "Error: could not write output file", job->user_data);
        }
    } else {
        if (job->on_progress) {
            job->on_progress(1.0, "Transcription Complete", job->user_data);
        }
    }

    return NULL;
}
