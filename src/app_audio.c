/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * app_audio.c — Audio capture implementation using ALSA
 *
 * Records 16kHz mono 16-bit PCM to a temporary WAV file.
 * Uses ALSA for all audio capture operations.
 *
 * Key Features:
 *   - Device selection: Configurable via audio_recorder_set_device().
 *     Falls back to ALSA "default" if configured device fails to open.
 *   - Device logging: Before starting recording, logs the device name
 *     via g_log() with domain "app-audio".
 *   - Secure memory scrubbing: PCM buffers are overwritten with zeros
 *     (via scrub_memory()) before being freed to prevent swap leakage.
 *   - Thread-safe: Uses pthread_mutex for concurrent access protection.
 *   - WAV format: 44-byte RIFF header, 16kHz mono 16-bit PCM data chunk.
 */

#include "app_audio.h"
#include "app.h"  /* For UNUSED macro */
#include "app_config.h"
#include "app_vad.h"
#include "app_ring_buffer.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <math.h>

/* ALSA */
#include <alsa/asoundlib.h>

/* Secure memory scrubbing — prevents compiler optimization */
static void scrub_memory(void *ptr, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

/* ===================================================================
 * Audio sample analysis helper
 * =================================================================== */

/* RIFF WAV header size */
#define WAV_HEADER_SIZE 44

/* File-scope error string — protected by mutex for thread safety */
static char g_audio_error[512] = {0};
static pthread_mutex_t g_audio_error_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_audio_error(const char *msg) {
    pthread_mutex_lock(&g_audio_error_mutex);
    if (msg) {
        strncpy(g_audio_error, msg, sizeof(g_audio_error) - 1);
        g_audio_error[sizeof(g_audio_error) - 1] = '\0';
    } else {
        g_audio_error[0] = '\0';
    }
    pthread_mutex_unlock(&g_audio_error_mutex);
}

/* ===================================================================
 * WAV file format helpers
 * =================================================================== */

AudioFormat audio_format_get_default(void) {
    AudioFormat fmt;
    fmt.sample_rate = 16000;
    fmt.channels = 1;
    fmt.bits_per_sample = 16;
    fmt.buffer_size = 320;  /* 20ms at 16kHz — matches WebRTC VAD frame size requirement */
    return fmt;
}

/* Inline helper for little-endian 32-bit packing */
static inline void pack32(unsigned char *buf, uint32_t val) {
    buf[0] = (unsigned char)((val) & 0xFF);
    buf[1] = (unsigned char)(((val) >> 8) & 0xFF);
    buf[2] = (unsigned char)(((val) >> 16) & 0xFF);
    buf[3] = (unsigned char)(((val) >> 24) & 0xFF);
}

static bool audio_format_write_wav_header(FILE *file, const AudioFormat *format) {
    if (!file || !format) return false;

    uint32_t sample_rate = (uint32_t)format->sample_rate;
    uint16_t channels = (uint16_t)format->channels;
    uint16_t bits_per_sample = (uint16_t)format->bits_per_sample;
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);

    unsigned char header[WAV_HEADER_SIZE] = {
        'R', 'I', 'F', 'F',           /* RIFF marker */
        0, 0, 0, 0,                    /* File size - 8 (placeholder) */
        'W', 'A', 'V', 'E',           /* WAVE marker */
        'f', 'm', 't', ' ',           /* fmt chunk */
        16, 0, 0, 0,                   /* Chunk size (16 for PCM) */
        1, 0,                          /* Audio format (1 = PCM) */
        0, 0,                          /* Num channels (placeholder) */
        0, 0, 0, 0,                    /* Sample rate (placeholder) */
        0, 0, 0, 0,                    /* Byte rate (placeholder) */
        0, 0,                          /* Block align (placeholder) */
        0, 0,                          /* Bits per sample (placeholder) */
        'd', 'a', 't', 'a',           /* data chunk */
        0, 0, 0, 0                     /* Data size (placeholder) */
    };

    unsigned char tmp[4];

    pack32(tmp, (uint32_t)channels);
    header[22] = tmp[0];
    header[23] = tmp[1];

    pack32(tmp, sample_rate);
    header[24] = tmp[0];
    header[25] = tmp[1];
    header[26] = tmp[2];
    header[27] = tmp[3];

    pack32(tmp, byte_rate);
    header[28] = tmp[0];
    header[29] = tmp[1];
    header[30] = tmp[2];
    header[31] = tmp[3];

    pack32(tmp, (uint32_t)block_align);
    header[32] = tmp[0];
    header[33] = tmp[1];

    pack32(tmp, (uint32_t)bits_per_sample);
    header[34] = tmp[0];
    header[35] = tmp[1];

    size_t written = fwrite(header, 1, WAV_HEADER_SIZE, file);
    return (written == WAV_HEADER_SIZE);
}

bool audio_format_finalize_wav_header(FILE *file, gsize data_size) {
    uint32_t ds = (uint32_t)data_size;
    uint32_t riff_size = ds + 36;  /* File size minus 8 bytes for RIFF descriptor */

    unsigned char buf[4];

    /* Patch RIFF chunk size at offset 4 */
    if (fseek(file, 4, SEEK_SET) != 0) {
        return false;
    }
    pack32(buf, riff_size);
    size_t written = fwrite(buf, 1, 4, file);
    if (written != 4) {
        return false;
    }

    /* Patch data chunk size at offset 40 */
    if (fseek(file, 40, SEEK_SET) != 0) {
        return false;
    }
    pack32(buf, ds);
    written = fwrite(buf, 1, 4, file);
    if (written != 4) {
        return false;
    }

    fseek(file, 0, SEEK_END);

    return true;
}

/* ===================================================================
 * AudioRecorder struct
 * =================================================================== */

struct _AudioRecorder {
    AudioFormat format;
    gchar device[256];
    gchar wav_path[512];
    gint wav_fd;
    FILE *wav_file;
    gsize wav_data_size;
    /* wav_data_size protected by mutex to prevent data races */
    gboolean is_recording;

    pthread_t capture_thread;
    pthread_mutex_t mutex;
    /* Atomic stop flag for portable thread safety */
    atomic_int stop_flag;

    /* ALSA context */
    snd_pcm_t *alsa_pcm;

    /* Real-time RMS volume level (0.0-1.0), updated by capture thread */
    /* Protected by volume_mutex for portable thread safety (not just x86) */
    gdouble rms_volume;
    pthread_mutex_t volume_mutex;

    /* In-memory ring buffer for continuous transcription (replaces WAV file) */
    AudioRingBuffer *ring_buffer;    ///< Ring buffer, created on start
};

/* ===================================================================
 * Helper: try to open an ALSA device
 * =================================================================== */

static bool try_alsa_device(AudioRecorder *rec, const char *device_name) {
    int err;
    snd_pcm_t *pcm;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
    unsigned int rate = rec->format.sample_rate;
    unsigned int channels = rec->format.channels;
    snd_pcm_uframes_t buffer_size = rec->format.buffer_size;

    err = snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        g_log("app-audio", G_LOG_LEVEL_MESSAGE,
              "[audio] Failed to open ALSA device '%s': %s\n",
              device_name, snd_strerror(err));
        return false;
    }

    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_hw_params_alloca(&params);
    err = snd_pcm_hw_params_any(pcm, params);
    if (err < 0) {
        char msg[256];
        g_snprintf(msg, sizeof(msg),
                   "[audio] Failed to get hw params for '%s': %s",
                   device_name, snd_strerror(err));
        set_audio_error(msg);
        g_log("app-audio", G_LOG_LEVEL_MESSAGE, "%s\n", msg);
        snd_pcm_close(pcm);
        return false;
    }

    err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        char msg[256];
        g_snprintf(msg, sizeof(msg), "[audio] Failed to set access mode for '%s': %s",
                   device_name, snd_strerror(err));
        set_audio_error(msg);
        g_log("app-audio", G_LOG_LEVEL_MESSAGE, "%s\n", msg);
        snd_pcm_close(pcm);
        return false;
    }

    err = snd_pcm_hw_params_set_format(pcm, params, format);
    if (err < 0) {
        char msg[256];
        g_snprintf(msg, sizeof(msg), "[audio] Failed to set format for '%s': %s",
                   device_name, snd_strerror(err));
        set_audio_error(msg);
        g_log("app-audio", G_LOG_LEVEL_MESSAGE, "%s\n", msg);
        snd_pcm_close(pcm);
        return false;
    }

    err = snd_pcm_hw_params_set_channels(pcm, params, channels);
    if (err < 0) {
        char msg[256];
        g_snprintf(msg, sizeof(msg), "[audio] Failed to set channels for '%s': %s",
                   device_name, snd_strerror(err));
        set_audio_error(msg);
        g_log("app-audio", G_LOG_LEVEL_MESSAGE, "%s\n", msg);
        snd_pcm_close(pcm);
        return false;
    }

    unsigned int actual_rate = rate;
    err = snd_pcm_hw_params_set_rate_near(pcm, params, &actual_rate, 0);
    if (err < 0) {
        char msg[256];
        g_snprintf(msg, sizeof(msg), "[audio] Failed to set sample rate for '%s': %s",
                   device_name, snd_strerror(err));
        set_audio_error(msg);
        g_log("app-audio", G_LOG_LEVEL_MESSAGE, "%s\n", msg);
        snd_pcm_close(pcm);
        return false;
    }

    /* Validate that the actual rate matches requested 16kHz.
     * Mismatched rates produce distorted audio. */
    if (actual_rate != 16000) {
        g_log("app-audio", G_LOG_LEVEL_MESSAGE, "[audio] Device '%s' provides %u Hz instead of 16000 Hz — skipping\n",
                device_name, actual_rate);
        snd_pcm_close(pcm);
        return false;
    }

    snd_pcm_uframes_t actual_buffer = buffer_size;
    err = snd_pcm_hw_params_set_period_size_near(pcm, params, &actual_buffer, 0);
    if (err < 0) { snd_pcm_close(pcm); return false; }

    err = snd_pcm_hw_params(pcm, params);
    if (err < 0) {
        snd_pcm_close(pcm);
        return false;
    }

    rec->alsa_pcm = pcm;
    return true;
}

/* ===================================================================
 * Lifecycle: create / destroy
 * =================================================================== */

AudioRecorder *audio_recorder_create(const AudioFormat *format) {
    AudioRecorder *recorder = g_new0(AudioRecorder, 1);
    if (!recorder) return NULL;

    if (format) {
        recorder->format = *format;
    } else {
        recorder->format = audio_format_get_default();
    }

    recorder->wav_fd = -1;
    recorder->wav_file = NULL;
    recorder->wav_data_size = 0;
    recorder->is_recording = false;
    atomic_store(&recorder->stop_flag, false);
    recorder->alsa_pcm = NULL;
    recorder->rms_volume = 0.0;
    if (pthread_mutex_init(&recorder->volume_mutex, NULL) != 0 ||
        pthread_mutex_init(&recorder->mutex, NULL) != 0) {
        g_free(recorder);
        return NULL;
    }

    /* Ring buffer initialized to NULL — created on start */
    recorder->ring_buffer = NULL;

    return recorder;
}

void audio_recorder_destroy(AudioRecorder *recorder) {
    if (!recorder) return;

    if (recorder->is_recording) {
        audio_recorder_stop(recorder);
    }

    if (recorder->wav_file) {
        fclose(recorder->wav_file);
        recorder->wav_file = NULL;
    } else if (recorder->wav_fd >= 0) {
        close(recorder->wav_fd);
        recorder->wav_fd = -1;
    }

    if (recorder->wav_path[0]) {
        unlink(recorder->wav_path);
    }

    if (recorder->alsa_pcm) {
        snd_pcm_drop(recorder->alsa_pcm);
        snd_pcm_close(recorder->alsa_pcm);
        recorder->alsa_pcm = NULL;
    }

    pthread_mutex_destroy(&recorder->volume_mutex);
    pthread_mutex_destroy(&recorder->mutex);

    /* Destroy ring buffer if it was created */
    if (recorder->ring_buffer) {
        ring_buffer_destroy(recorder->ring_buffer);
        recorder->ring_buffer = NULL;
    }

    scrub_memory(recorder->wav_path, sizeof(recorder->wav_path));
    scrub_memory(recorder->device, sizeof(recorder->device));

    g_free(recorder);
}

bool audio_recorder_set_device(AudioRecorder *recorder, const char *device) {
    if (!recorder) return false;

    pthread_mutex_lock(&recorder->mutex);
    if (device) {
        strncpy(recorder->device, device, sizeof(recorder->device) - 1);
        recorder->device[sizeof(recorder->device) - 1] = '\0';
    } else {
        recorder->device[0] = '\0';
    }
    pthread_mutex_unlock(&recorder->mutex);
    return true;
}

const char *audio_recorder_get_device(const AudioRecorder *recorder) {
    if (!recorder) return "";
    return recorder->device;
}

/* ===================================================================
 * Capture thread
 * =================================================================== */

static void *capture_thread_func(void *arg) {
    AudioRecorder *recorder = (AudioRecorder *)arg;
    gsize bytes_per_frame = recorder->format.channels * (recorder->format.bits_per_sample / 8);
    gsize buffer_bytes = recorder->format.buffer_size * bytes_per_frame;

    guchar *buffer = g_malloc(buffer_bytes);
    if (!buffer) {
        set_audio_error("Failed to allocate capture buffer");
        /* Signal failure: set is_recording=false under mutex so the main
         * thread knows recording did not start. The main thread set
         * is_recording=true after pthread_create(), but the thread failed
         * before entering the capture loop. */
        pthread_mutex_lock(&recorder->mutex);
        recorder->is_recording = false;
        pthread_mutex_unlock(&recorder->mutex);
        return NULL;
    }

    /* Device validation with fallback: configured device -> "default" */
    bool device_opened = false;
    const char *last_error = NULL;

    const char *configured_device = recorder->device[0] ? recorder->device : NULL;
    if (configured_device) {
        if (try_alsa_device(recorder, configured_device)) {
            device_opened = true;
        } else {
            last_error = "Failed to open configured ALSA device";
        }
    }

    if (!device_opened) {
        if (try_alsa_device(recorder, "default")) {
            device_opened = true;
        } else {
            last_error = "Failed to open ALSA default device";
        }
    }

    if (!device_opened) {
        char msg[512];
        g_snprintf(msg, sizeof(msg),
                   "No valid audio device found. Last error: %s. "
                   "Please check your microphone configuration.",
                   last_error ? last_error : "unknown");
        set_audio_error(msg);
        /* Signal failure: set is_recording=false so the main thread knows
         * recording did not start. Without this, is_recording remains true
         * (set by main thread after pthread_create), blocking future starts. */
        pthread_mutex_lock(&recorder->mutex);
        recorder->is_recording = false;
        pthread_mutex_unlock(&recorder->mutex);
        scrub_memory(buffer, buffer_bytes);
        g_free(buffer);
        return NULL;
    }

    /* Device successfully opened.
     * is_recording is set to TRUE by the main thread under the mutex
     * in audio_recorder_start() after pthread_create() returns successfully. */

    /* Enter the ALSA read loop */
    int err;
    int iteration = 0;
    int read_error_count = 0;
    gsize total_frames = 0;

    /* Use atomic load for the stop flag check */
    while (!atomic_load(&recorder->stop_flag)) {
        err = snd_pcm_readi(recorder->alsa_pcm, buffer, recorder->format.buffer_size);
        if (err == -EPIPE) {
            snd_pcm_prepare(recorder->alsa_pcm);
            continue;  /* Loop will check stop_flag at while() condition */
        } else if (err < 0) {
            read_error_count++;
            const char *err_msg = snd_strerror(err);
            if (read_error_count > 10) {
                set_audio_error(err_msg ? err_msg : "Repeated ALSA errors");
                break;
            }
            continue;
        }

        gsize bytes_written_count = (gsize)err * bytes_per_frame;
        size_t written = fwrite(buffer, 1, (size_t)bytes_written_count, recorder->wav_file);
        if (written != (size_t)bytes_written_count) {
            set_audio_error("Failed to write audio data to WAV file");
            break;
        }
        /* Protect wav_data_size with mutex for portable thread safety. */
        pthread_mutex_lock(&recorder->mutex);
        recorder->wav_data_size += (size_t)bytes_written_count;
        pthread_mutex_unlock(&recorder->mutex);
        total_frames += (gsize)err;

        /* Calculate RMS volume level from PCM samples (16-bit signed) */
        {
            double sum_squares = 0.0;
            int16_t *samples = (int16_t *)buffer;
            int num_samples = err * recorder->format.channels;
            for (int i = 0; i < num_samples; i++) {
                double val = (double)samples[i];
                sum_squares += val * val;
            }
            double rms = (num_samples > 0) ? sqrt(sum_squares / num_samples) : 0.0;
            /* Normalize to 0.0-1.0 range (full scale for int16 is 32768) */
            double normalized = rms / 32768.0;
            if (normalized > 1.0) normalized = 1.0;
            pthread_mutex_lock(&recorder->volume_mutex);
            recorder->rms_volume = normalized;
            pthread_mutex_unlock(&recorder->volume_mutex);
        }

        /* Write all frames to ring buffer unconditionally.
         * In continuous mode the silence scanner handles segment detection.
         * Thread safety: recorder->ring_buffer is set once during
         * audio_recorder_start() before pthread_create() returns, and is
         * never modified during recording. The pthread_create() call provides
         * a happens-before guarantee, ensuring the capture thread sees the
         * initialized ring_buffer pointer without additional synchronization. */
        if (recorder->ring_buffer) {
            int num_samples = err * recorder->format.channels;
            int16_t *samples = (int16_t *)buffer;
            ring_buffer_write(recorder->ring_buffer, samples, (size_t)num_samples);
        }

        iteration++;
    }

    /* Cleanup ALSA */
    if (recorder->alsa_pcm) {
        snd_pcm_drop(recorder->alsa_pcm);
        snd_pcm_close(recorder->alsa_pcm);
        recorder->alsa_pcm = NULL;
    }

    /* Set is_recording = FALSE under mutex for thread safety.
     * The main thread only reads is_recording after pthread_join() returns,
     * which provides a happens-before guarantee. */
    pthread_mutex_lock(&recorder->mutex);
    recorder->is_recording = false;
    pthread_mutex_unlock(&recorder->mutex);

    scrub_memory(buffer, buffer_bytes);
    g_free(buffer);
    return NULL;
}

/* ===================================================================
 * Recording control
 * =================================================================== */

/* Duration enforcement is done by the watchdog timer in main.c.
 * The audio module has no concept of duration limits. */
bool audio_recorder_start(AudioRecorder *recorder) {

    /* Reset any stale error before starting a new recording */
    audio_recorder_reset_error();

    if (!recorder) {
        set_audio_error("Invalid recorder handle");
        return false;
    }

    if (recorder->is_recording) {
        set_audio_error("Already recording");
        return false;
    }

    pthread_mutex_lock(&recorder->mutex);

    GError *tmp_error = NULL;
    char *tmp_path = NULL;

    recorder->wav_fd = g_file_open_tmp("transcriber_XXXXXX", &tmp_path, &tmp_error);
    if (recorder->wav_fd < 0) {
        char msg[512];
        g_snprintf(msg, sizeof(msg),
                   "Failed to create temporary WAV file: %s", tmp_error ? tmp_error->message : "unknown error");
        set_audio_error(msg);
        if (tmp_error) {
            g_error_free(tmp_error);
        }
        pthread_mutex_unlock(&recorder->mutex);
        return false;
    }
    if (g_strlcpy(recorder->wav_path, tmp_path, sizeof(recorder->wav_path)) >= sizeof(recorder->wav_path)) {
        g_log("app-audio", G_LOG_LEVEL_ERROR, "[audio] Temporary path too long, aborting\n");
        close(recorder->wav_fd);
        recorder->wav_fd = -1;
        unlink(tmp_path);
        g_free(tmp_path);
        pthread_mutex_unlock(&recorder->mutex);
        return false;
    }
    g_free(tmp_path);
    /* Use owner-only permissions (0600) for privacy. */
    chmod(recorder->wav_path, 0600);

    recorder->wav_file = fdopen(recorder->wav_fd, "w+");
    if (!recorder->wav_file) {
        close(recorder->wav_fd);
        recorder->wav_fd = -1;
        char msg[512];
        g_snprintf(msg, sizeof(msg),
                   "Failed to open WAV file stream: %s", strerror(errno));
        set_audio_error(msg);
        pthread_mutex_unlock(&recorder->mutex);
        return false;
    }

    if (!audio_format_write_wav_header(recorder->wav_file, &recorder->format)) {
        fclose(recorder->wav_file);
        recorder->wav_file = NULL;
        unlink(recorder->wav_path);
        recorder->wav_path[0] = '\0';
        pthread_mutex_unlock(&recorder->mutex);
        return false;
    }

    recorder->wav_data_size = 0;
    atomic_store(&recorder->stop_flag, false);

    /* Create ring buffer for in-memory audio storage (90 seconds = 3x whisper segment limit) */
    if (!recorder->ring_buffer) {
        recorder->ring_buffer = ring_buffer_create(90, recorder->format.sample_rate);
        if (!recorder->ring_buffer) {
            g_log("app-audio", G_LOG_LEVEL_WARNING,
                  "[audio] Failed to create ring buffer — in-memory transcription unavailable\n");
        }
    }

    g_log("app-audio", G_LOG_LEVEL_WARNING,
          "[flow] audio_recorder_start() — created WAV '%s', ring_buffer=%p\n",
          recorder->wav_path, (void*)recorder->ring_buffer);

    int err = pthread_create(&recorder->capture_thread, NULL, capture_thread_func, recorder);
    if (err != 0) {
        char msg[512];
        g_snprintf(msg, sizeof(msg),
                   "Failed to create capture thread: %s", strerror(err));
        set_audio_error(msg);
        fclose(recorder->wav_file);
        recorder->wav_file = NULL;
        unlink(recorder->wav_path);
        recorder->wav_path[0] = '\0';
        pthread_mutex_unlock(&recorder->mutex);
        return false;
    }
    /* Do NOT detach — use pthread_join() in stop() */

    /* Set is_recording = TRUE under mutex AFTER successful thread creation.
     * This ensures the main thread and capture thread have a consistent view of
     * the recording state, protected by the mutex. */
    recorder->is_recording = true;

    pthread_mutex_unlock(&recorder->mutex);
    return true;
}

bool audio_recorder_stop(AudioRecorder *recorder) {
    if (!recorder) {
        return false;
    }

    /* Check stop_flag atomically to guard against double-stop.
     * We check is_recording under the mutex below after setting the stop flag. */

    pthread_mutex_lock(&recorder->mutex);

    /* Guard: if not recording, bail out */
    if (!recorder->is_recording) {
        pthread_mutex_unlock(&recorder->mutex);
        return false;
    }

    /* Use atomic store for portable thread safety */
    atomic_store(&recorder->stop_flag, true);

    /* Interrupt any blocking ALSA read by dropping the PCM state.
     * snd_pcm_readi() blocks indefinitely waiting for audio data from
     * the hardware. Merely setting stop_flag does not interrupt that
     * system call, causing pthread_join() below to hang forever on the
     * GTK main thread (freezing the entire UI).
     *
     * snd_pcm_drop() forces the pending read to return -EPIPE, which
     * the capture thread handles by calling snd_pcm_prepare() and then
     * re-checking stop_flag — causing a clean loop exit.
     *
     * Safe under mutex: is_recording=TRUE guarantees alsa_pcm != NULL. */
    if (recorder->alsa_pcm) {
        snd_pcm_drop(recorder->alsa_pcm);
    }

    pthread_mutex_unlock(&recorder->mutex);

    /* Use pthread_join() for clean thread termination.
     * With the PCM dropped above, this should not block indefinitely. */
    void *thread_result = NULL;
    int join_err = pthread_join(recorder->capture_thread, &thread_result);
    if (join_err != 0) {
        set_audio_error("Failed to join capture thread");
    }

    /* Finalize WAV header and close file before clearing is_recording.
     * The capture thread sets is_recording=FALSE under mutex, but we must
     * finalize the WAV file first. */
    /* Read wav_data_size under mutex to prevent data race with capture thread */
    gsize final_data_size;
    pthread_mutex_lock(&recorder->mutex);
    final_data_size = recorder->wav_data_size;
    pthread_mutex_unlock(&recorder->mutex);

    if (recorder->wav_file) {
        audio_format_finalize_wav_header(recorder->wav_file, final_data_size);
        fflush(recorder->wav_file);

        /* Close the file so data is fully flushed to disk and handle is released */
        fclose(recorder->wav_file);
        recorder->wav_file = NULL;
        recorder->wav_fd = -1;
        /* Use owner-only permissions (0600) for privacy. */
        chmod(recorder->wav_path, 0600);
    }

    return true;
}

/* ===================================================================
 * Query / status
 * =================================================================== */

const char *audio_recorder_get_wav_path(const AudioRecorder *recorder) {
    if (!recorder) return "";
    return recorder->wav_path;
}

const char *audio_recorder_get_error(void) {
    /* Lock mutex, copy to thread-local buffer, unlock, then return.
     * This ensures the returned pointer is stable regardless of concurrent writes. */
    static __thread char local_buffer[512] = {0};
    pthread_mutex_lock(&g_audio_error_mutex);
    strncpy(local_buffer, g_audio_error, sizeof(local_buffer) - 1);
    local_buffer[sizeof(local_buffer) - 1] = '\0';
    pthread_mutex_unlock(&g_audio_error_mutex);
    return local_buffer;
}

void audio_recorder_reset_error(void) {
    pthread_mutex_lock(&g_audio_error_mutex);
    g_audio_error[0] = '\0';
    pthread_mutex_unlock(&g_audio_error_mutex);
}

/**
 * Delete the WAV file from disk. Call this AFTER transcription is done.
 */
bool audio_recorder_delete_wav(AudioRecorder *recorder) {
    if (!recorder) return false;

    if (recorder->wav_path[0]) {
        unlink(recorder->wav_path);
        recorder->wav_path[0] = '\0';
        return true;
    }
    return false;
}

/* ===================================================================
 * Device listing
 * =================================================================== */

/* Try to open a device for capture to verify it's actually usable */
static bool test_capture_device(const char *device_name) {
    snd_pcm_t *pcm;
    int err = snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) return false;
    snd_pcm_close(pcm);
    return true;
}

AudioDeviceList *audio_recorder_get_device_list(const AudioRecorder *recorder) {
    UNUSED(recorder);

    void **hints = NULL;
    int err = snd_device_name_hint(-1, "pcm", &hints);
    if (err < 0) {
        goto fallback;
    }

    typedef struct { gchar *display; gchar *name; } DevEntry;
    gint max_entries = 64;
    DevEntry *entries = (DevEntry *)calloc(max_entries, sizeof(DevEntry));
    if (!entries) {
        snd_device_name_free_hint(hints);
        goto fallback;
    }
    gint n_entries = 0;

    for (void **h = hints; *h != NULL; h++) {
        char *name = snd_device_name_get_hint(*h, "NAME");
        char *desc = snd_device_name_get_hint(*h, "DESC");
        char *ioid = snd_device_name_get_hint(*h, "IOID");

        /* IOID "Input" = capture device; NULL = both input+output */
        gboolean is_capture = (ioid == NULL) || (strcmp(ioid, "Input") == 0);
        free(ioid);

        if (!is_capture || !name) {
            free(name);
            free(desc);
            continue;
        }

        /* Only show direct hardware devices (hw: prefix) — skip plugins/virtual.
         * Rationale: ALSA plugin devices (e.g., "default", "plug:", "surround:")
         * go through the ALSA plugin chain which can introduce latency, resampling,
         * or channel remapping that breaks the 16kHz mono PCM requirement for
         * whisper.cpp. Direct hw: devices provide raw hardware access with
         * predictable format. Users with PipeWire/PulseAudio should ensure their
         * hw: devices are properly routed. The "default" virtual device is still
         * available as a fallback in the config dialog device list. */
        if (strncmp(name, "hw:", 3) != 0) {
            free(name);
            free(desc);
            continue;
        }

        /* Actually test if the device can be opened for capture */
        if (!test_capture_device(name)) {
            free(name);
            free(desc);
            continue;
        }

        /* Build display name from description only (no device identifier) */
        gchar *display;
        if (desc && desc[0] != '\0') {
            /* Clean up description — remove newlines */
            char *d = desc;
            while (*d) {
                if (*d == '\n') *d = ' ';
                d++;
            }
            /* Truncate at first comma to keep name short and readable */
            char *comma = strchr(desc, ',');
            if (comma) {
                *comma = '\0';
            }
            /* Trim trailing whitespace */
            d = desc + strlen(desc) - 1;
            while (d >= desc && *d == ' ') {
                *d = '\0';
                d--;
            }
            display = g_strdup(desc);
        } else {
            display = g_strdup(name);
        }

        if (n_entries < max_entries) {
            entries[n_entries].display = display;
            entries[n_entries].name = g_strdup(name);  /* duplicate for return */
            n_entries++;
        } else {
            g_free(display);
        }
        free(name);
        free(desc);
    }

    snd_device_name_free_hint(hints);

    if (n_entries > 0) {
        AudioDeviceList *list = g_new0(AudioDeviceList, 1);
        list->display_names = g_new0(gchar *, n_entries + 1);
        list->device_names = g_new0(gchar *, n_entries + 1);
        list->count = n_entries;
        for (gint i = 0; i < n_entries; i++) {
            list->display_names[i] = entries[i].display;
            list->device_names[i] = entries[i].name;
        }
        free(entries);
        return list;
    }
    free(entries);

fallback:
    {
        AudioDeviceList *list = g_new0(AudioDeviceList, 1);
        list->display_names = g_new0(gchar *, 2);
        list->device_names = g_new0(gchar *, 2);
        list->count = 1;
        list->display_names[0] = g_strdup("Default");
        list->device_names[0] = g_strdup("default");
        return list;
    }
}

void audio_device_list_free(AudioDeviceList *list) {
    if (!list) return;
    if (list->display_names) {
        for (gint i = 0; i < list->count; i++) {
            g_free(list->display_names[i]);
        }
        g_free(list->display_names);
    }
    if (list->device_names) {
        for (gint i = 0; i < list->count; i++) {
            g_free(list->device_names[i]);
        }
        g_free(list->device_names);
    }
    g_free(list);
}

/* ===================================================================
 * Volume level
 * =================================================================== */

double audio_recorder_get_volume_level(const AudioRecorder *recorder) {
    if (!recorder) return 0.0;
    /* Use dedicated volume_mutex for portable thread safety.
      * Note: const-cast is safe here — mutex lock/unlock does not logically
      * modify the recorder state, it only provides synchronization. */
    AudioRecorder *nonconst = (AudioRecorder *)recorder;
    double vol;
    pthread_mutex_lock(&nonconst->volume_mutex);
    vol = nonconst->rms_volume;
    pthread_mutex_unlock(&nonconst->volume_mutex);
    return vol;
}

AudioRingBuffer *audio_recorder_get_ring_buffer(const AudioRecorder *recorder)
{
    if (!recorder) return NULL;
    /* Ring buffer is set during start() and never modified during recording.
     * The pthread_create() in start() provides happens-before guarantee. */
    return recorder->ring_buffer;
}

/* ===================================================================
 * Ring Buffer Extraction
 * =================================================================== */

size_t audio_recorder_extract_samples(AudioRecorder *recorder, int16_t **out_samples)
{
    if (!recorder || !out_samples || !recorder->ring_buffer) {
        if (out_samples) *out_samples = NULL;
        return 0;
    }
    return ring_buffer_extract_all(recorder->ring_buffer, out_samples);
}

/* Trim trailing silence from PCM samples.
 * Scans backwards in 20ms frames and removes frames below RMS threshold.
 * Returns the trimmed sample count. */
size_t audio_trim_trailing_silence(int16_t *samples, size_t n_samples, uint32_t sample_rate)
{
    if (!samples || n_samples == 0 || sample_rate == 0) return n_samples;

    /* Frame size: 20ms at the given sample rate */
    size_t frame_size = sample_rate / 50;  /* 16000/50 = 320 samples */
    if (frame_size == 0) frame_size = 1;

    /* Silence threshold: RMS amplitude below this is considered silence.
     * 100 out of 32768 full scale ≈ -90 dB, well below speech levels. */
    const double silence_threshold = 100.0;

    size_t remaining = n_samples;

    while (remaining >= frame_size) {
        /* Calculate RMS of the last frame */
        size_t frame_start = remaining - frame_size;
        double sum_squares = 0.0;
        for (size_t i = frame_start; i < remaining; i++) {
            double val = (double)samples[i];
            sum_squares += val * val;
        }
        double rms = sqrt(sum_squares / frame_size);

        if (rms < silence_threshold) {
            /* This frame is silence — trim it */
            remaining -= frame_size;
        } else {
            /* Found a non-silent frame — stop trimming */
            break;
        }
    }

    if (remaining < n_samples) {
        g_log("app-audio", G_LOG_LEVEL_MESSAGE,
              "[audio] Trimmed trailing silence: %zu -> %zu samples (%.1f seconds removed)\n",
              n_samples, remaining,
              (double)(n_samples - remaining) / (double)sample_rate);
    }

    return remaining;
}

