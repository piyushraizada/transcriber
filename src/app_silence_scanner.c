/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/**
 * @file app_silence_scanner.c
 * @brief Continuous silence scanner for continuous dictation mode
 *
 * Monitors the ring buffer for silence periods and triggers transcription
 * of audio segments when silence exceeds a configurable threshold.
 */

#include "app_silence_scanner.h"
#include "app_ring_buffer.h"
#include "app_vad.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* Scanner scans 500ms chunks at a time */
#define SCANNER_CHUNK_MS 500

/* Default sample rate — matches whisper.cpp requirements */
#define SCANNER_SAMPLE_RATE 16000

/* Samples per scan chunk: 500ms * 16000Hz = 8000 samples */
#define SCANNER_CHUNK_SAMPLES ((size_t)(SCANNER_CHUNK_MS * SCANNER_SAMPLE_RATE / 1000))

/* Sleep between scan iterations (ms) */
#define SCANNER_SLEEP_US 200000

struct _SilenceScanner {
    AudioRingBuffer *ring_buffer;
    VadDetector *vad_detector;

    VadMode vad_mode;            /* VAD mode, preserved for reset */

    /*
     * The scanner tracks two logical positions within the ring buffer's
     * available data:
     *   - transcribed_offset: samples already sent for transcription
     *                         (relative to oldest sample in ring buffer)
     *   - scan_offset:        current evaluation position
     *
     * Both are relative to the "start of current session" which is the
     * oldest sample in the ring buffer at the time of the last reset.
     * As the ring buffer accumulates data, these offsets advance.
     */
    size_t transcribed_offset;   /* Samples already sent for transcription */
    size_t scan_offset;          /* Current evaluation position */
    size_t last_voice_offset;    /* End of last voice chunk (for trimming trailing silence) */

    /* Frame-based silence tracking */
    int consecutive_silence_chunks;  /* Number of consecutive silence chunks */

    int silence_threshold_ms;    /* Silence duration to trigger segment check */
    int min_segment_ms;          /* Minimum segment duration before transcribing */

    bool running;
    pthread_t thread;
    atomic_int stop_flag;

    /* Callback for delivering segments */
    scanner_segment_callback callback;
    void *callback_user_data;

    pthread_mutex_t mutex;       /* Protects callback, offsets, and running state */
};

/* ===================================================================
 * Scanner thread function
 * =================================================================== */
static void *scanner_thread_func(void *arg)
{
    SilenceScanner *scanner = (SilenceScanner *)arg;
    int16_t *chunk = g_malloc0(SCANNER_CHUNK_SAMPLES * sizeof(int16_t));
    if (!chunk) return NULL;

    g_log("app-scanner", G_LOG_LEVEL_MESSAGE,
          "[scanner] Thread started\n");

    while (!atomic_load(&scanner->stop_flag)) {
        size_t available;
        size_t read_count;
        bool is_voice = false;

        /* Get available samples in ring buffer */
        available = ring_buffer_available(scanner->ring_buffer);

        /* Calculate how many samples we should scan this iteration */
        size_t target_scan = scanner->scan_offset + SCANNER_CHUNK_SAMPLES;

        /* Ring buffer wrap detection: if the buffer is full and the scanner
         * hasn't caught up, old unscanned data has been overwritten by new
         * writes. Reset offsets to start fresh from the current write position.
         * Only triggers when the buffer is truly full (available >= capacity),
         * NOT when we simply haven't accumulated enough data yet for the next
         * chunk (that case is handled by the "not enough data" check below). */
        {
            size_t cap = ring_buffer_capacity(scanner->ring_buffer);
            if (available >= cap && scanner->scan_offset < available) {
                pthread_mutex_lock(&scanner->mutex);
                scanner->scan_offset = 0;
                scanner->transcribed_offset = 0;
                scanner->last_voice_offset = 0;
                scanner->consecutive_silence_chunks = 0;
                pthread_mutex_unlock(&scanner->mutex);
                g_log("app-scanner", G_LOG_LEVEL_MESSAGE,
                      "[scanner] Ring buffer full — resetting offsets\n");
                g_usleep(SCANNER_SLEEP_US);
                continue;
            }
        }

        /* If not enough data yet, wait and retry */
        if (available < target_scan) {
            g_usleep(SCANNER_SLEEP_US);
            continue;
        }

        /* Read the chunk at scan_offset */
        read_count = ring_buffer_read_range(scanner->ring_buffer,
                                            scanner->scan_offset,
                                            chunk,
                                            SCANNER_CHUNK_SAMPLES);

        if (read_count < SCANNER_CHUNK_SAMPLES) {
            g_usleep(SCANNER_SLEEP_US);
            continue;
        }

        /* Calculate current segment duration since last transcribed offset */
        size_t seg_scan, seg_transcribed;
        pthread_mutex_lock(&scanner->mutex);
        seg_scan = scanner->scan_offset;
        seg_transcribed = scanner->transcribed_offset;
        pthread_mutex_unlock(&scanner->mutex);
        size_t seg_samples = (seg_scan > seg_transcribed) ? (seg_scan - seg_transcribed) : 0;
        double seg_ms_d = (double)seg_samples / SCANNER_SAMPLE_RATE * 1000.0;
        int seg_ms = (seg_ms_d > (double)INT_MAX) ? INT_MAX : (int)seg_ms_d;

        /* Run VAD on this chunk — subdivide into smaller 20ms frames for
         * more accurate detection. WebRTC VAD is designed for 20ms frames.
         * A 500ms chunk may contain mixed voice/silence, and processing it
         * as one large frame can produce false negatives on speech.
         *
         * Use majority-vote: require >50% of sub-frames to be voice. This is
         * robust against background noise (e.g., AC humming) which may cause
         * occasional false-positive voice frames, while still reliably detecting
         * genuine speech which would have most frames classified as voice. */
        {
            /* Hold mutex across all vad_process_frame() calls to prevent use-after-free.
             * silence_scanner_reset() destroys and recreates vad_detector under the same
             * mutex, so holding it here ensures the detector remains valid for the full
             * duration of VAD processing. Each frame takes ~microseconds, so contention
             * is negligible. */
            size_t frame_samples = 320;  /* 20ms at 16kHz */
            size_t n_frames = SCANNER_CHUNK_SAMPLES / frame_samples;  /* 25 frames */
            int voice_frames = 0;
            pthread_mutex_lock(&scanner->mutex);
            VadDetector *vad_snapshot = scanner->vad_detector;
            for (size_t f = 0; f < n_frames; f++) {
                size_t off = f * frame_samples;
                bool frame_voice = vad_process_frame(vad_snapshot,
                                                     chunk + off,
                                                     frame_samples,
                                                     SCANNER_SAMPLE_RATE);
                if (frame_voice) {
                    voice_frames++;
                }
            }
            pthread_mutex_unlock(&scanner->mutex);
            /* Require majority of frames to be voice (>50%) */
            is_voice = (voice_frames > (int)(n_frames / 2));
        }

        if (is_voice) {
            /* Voice detected — reset silence counter and record voice end position */
            pthread_mutex_lock(&scanner->mutex);
            scanner->consecutive_silence_chunks = 0;
            scanner->last_voice_offset = scanner->scan_offset + SCANNER_CHUNK_SAMPLES;
            pthread_mutex_unlock(&scanner->mutex);
        } else if (seg_ms >= scanner->min_segment_ms) {
            /* Silence detected AND minimum segment duration has elapsed —
             * increment counter. Only count silence after min_segment_ms to
             * prevent early silence within the first N seconds from triggering
             * transcription at exactly the minimum boundary. */
            int silence_chunks;
            pthread_mutex_lock(&scanner->mutex);
            scanner->consecutive_silence_chunks++;
            silence_chunks = scanner->consecutive_silence_chunks;
            pthread_mutex_unlock(&scanner->mutex);

            /* Check if silence duration exceeds threshold */
            int silence_ms = silence_chunks * SCANNER_CHUNK_MS;
            if (silence_ms >= scanner->silence_threshold_ms) {
                /* Silence threshold exceeded — extract and send for transcription.
                 * Use last_voice_offset to trim trailing silence so Whisper doesn't
                 * transcribe the silence padding as words like "Thank you". */
                size_t seg_end;
                pthread_mutex_lock(&scanner->mutex);
                seg_end = scanner->last_voice_offset;
                pthread_mutex_unlock(&scanner->mutex);
                if (seg_end <= seg_transcribed) {
                    /* Entire segment was silence — skip, but still advance offset. */
                    pthread_mutex_lock(&scanner->mutex);
                    scanner->transcribed_offset = seg_scan;
                    scanner->consecutive_silence_chunks = 0;
                    pthread_mutex_unlock(&scanner->mutex);
                    g_usleep(SCANNER_SLEEP_US);
                    continue;
                }
                size_t trim_samples = seg_end - seg_transcribed;
                int16_t *segment = g_malloc(trim_samples * sizeof(int16_t));
                if (segment) {
                    size_t actual = ring_buffer_read_range(scanner->ring_buffer,
                                                          seg_transcribed,
                                                          segment,
                                                          trim_samples);

                    if (actual > 0) {
                        scanner_segment_callback cb;
                        void *user_data;

                        pthread_mutex_lock(&scanner->mutex);
                        cb = scanner->callback;
                        user_data = scanner->callback_user_data;
                        /* Advance transcribed_offset past this segment */
                        scanner->transcribed_offset = seg_scan;
                        /* Reset silence counter so the next segment needs its own
                         * silence period after min_segment_ms to trigger. Without
                         * this reset, the accumulated silence_chunks from the
                         * previous segment would cause every new segment to fire
                         * immediately at the min_segment boundary. */
                        scanner->consecutive_silence_chunks = 0;
                        pthread_mutex_unlock(&scanner->mutex);

                        g_log("app-scanner", G_LOG_LEVEL_MESSAGE,
                              "[scanner] Segment ready: %zu samples (%.1fs), "
                              "silence_chunks=%d, trimmed_end=%zu, transcribed_offset -> %zu\n",
                              actual, (double)actual / SCANNER_SAMPLE_RATE,
                              silence_chunks, seg_end, seg_end);

                        if (cb) {
                            cb(segment, actual, user_data);
                        } else {
                            g_free(segment);
                        }
                    } else {
                        g_free(segment);
                    }
                }
            }
        }

        /* Advance scan offset */
        pthread_mutex_lock(&scanner->mutex);
        scanner->scan_offset += SCANNER_CHUNK_SAMPLES;
        pthread_mutex_unlock(&scanner->mutex);
    }

    g_free(chunk);
    /* Thread exit is expected when:
     *   - transcribe_thread_func() stops the scanner before extracting samples
     *   - handle_enter_listening() destroys the old scanner on restart
     *   - application shutdown
     * Log at DEBUG level to avoid alarming users. */
    g_log("app-scanner", G_LOG_LEVEL_DEBUG,
          "[scanner] Scanner thread exiting\n");

    /* Signal that scanner is done */
    pthread_mutex_lock(&scanner->mutex);
    scanner->running = false;
    pthread_mutex_unlock(&scanner->mutex);

    g_log("app-scanner", G_LOG_LEVEL_MESSAGE,
          "[scanner] Thread exited\n");
    return NULL;
}

/* ===================================================================
 * Public API
 * =================================================================== */

SilenceScanner *silence_scanner_create(AudioRingBuffer *rb,
                                       VadMode mode,
                                       int silence_threshold_ms,
                                       int min_segment_ms)
{
    if (!rb) return NULL;
    if (silence_threshold_ms < 500 || silence_threshold_ms > 10000) return NULL;
    if (min_segment_ms < 1000 || min_segment_ms > 30000) return NULL;

    SilenceScanner *scanner = g_malloc0(sizeof(SilenceScanner));
    if (!scanner) return NULL;

    scanner->ring_buffer = rb;
    scanner->vad_mode = mode;
    scanner->vad_detector = vad_detector_create(mode);
    if (!scanner->vad_detector) {
        g_free(scanner);
        return NULL;
    }

    scanner->silence_threshold_ms = silence_threshold_ms;
    scanner->min_segment_ms = min_segment_ms;
    scanner->transcribed_offset = 0;
    scanner->scan_offset = 0;
    scanner->last_voice_offset = 0;
    scanner->consecutive_silence_chunks = 0;
    scanner->running = false;
    atomic_store(&scanner->stop_flag, false);
    scanner->callback = NULL;
    scanner->callback_user_data = NULL;

    if (pthread_mutex_init(&scanner->mutex, NULL) != 0) {
        vad_detector_destroy(scanner->vad_detector);
        g_free(scanner);
        return NULL;
    }

    g_log("app-scanner", G_LOG_LEVEL_MESSAGE,
          "[scanner] Created: silence=%dms, min_segment=%dms, vad_mode=%d\n",
          silence_threshold_ms, min_segment_ms, mode);

    return scanner;
}

void silence_scanner_destroy(SilenceScanner *scanner)
{
    if (!scanner) return;

    if (silence_scanner_is_running(scanner)) {
        silence_scanner_stop(scanner);
    }

    pthread_mutex_destroy(&scanner->mutex);
    vad_detector_destroy(scanner->vad_detector);
    g_free(scanner);
}

bool silence_scanner_start(SilenceScanner *scanner)
{
    if (!scanner) return false;

    pthread_mutex_lock(&scanner->mutex);
    if (scanner->running) {
        pthread_mutex_unlock(&scanner->mutex);
        return false;
    }
    pthread_mutex_unlock(&scanner->mutex);

    /* Reset state for new session */
    silence_scanner_reset(scanner);

    pthread_mutex_lock(&scanner->mutex);
    scanner->running = true;
    atomic_store(&scanner->stop_flag, false);
    pthread_mutex_unlock(&scanner->mutex);

    if (pthread_create(&scanner->thread, NULL, scanner_thread_func, scanner) != 0) {
        pthread_mutex_lock(&scanner->mutex);
        scanner->running = false;
        pthread_mutex_unlock(&scanner->mutex);
        g_log("app-scanner", G_LOG_LEVEL_ERROR,
              "[scanner] Failed to create scanner thread\n");
        return false;
    }

    g_log("app-scanner", G_LOG_LEVEL_MESSAGE,
          "[scanner] Started (thread %lu)\n", (unsigned long)scanner->thread);
    return true;
}

void silence_scanner_stop(SilenceScanner *scanner)
{
    if (!scanner) return;

    bool was_running;
    pthread_mutex_lock(&scanner->mutex);
    was_running = scanner->running;
    pthread_mutex_unlock(&scanner->mutex);

    if (!was_running) {
        return;
    }

    atomic_store(&scanner->stop_flag, true);

    pthread_join(scanner->thread, NULL);

    pthread_mutex_lock(&scanner->mutex);
    scanner->running = false;
    pthread_mutex_unlock(&scanner->mutex);
}

void silence_scanner_reset(SilenceScanner *scanner)
{
    if (!scanner) return;

    /* Reset VAD detector and all state under mutex to prevent use-after-free.
     * The scanner thread snapshots vad_detector under the mutex before use,
     * so holding the mutex here ensures the thread sees either the old or
     * new detector atomically, never a dangling pointer. */
    pthread_mutex_lock(&scanner->mutex);
    vad_detector_destroy(scanner->vad_detector);
    scanner->vad_detector = vad_detector_create(scanner->vad_mode);
    scanner->transcribed_offset = 0;
    scanner->scan_offset = 0;
    scanner->consecutive_silence_chunks = 0;
    pthread_mutex_unlock(&scanner->mutex);

    g_log("app-scanner", G_LOG_LEVEL_MESSAGE,
          "[scanner] Reset for new session\n");
}

void silence_scanner_set_callback(SilenceScanner *scanner,
                                  scanner_segment_callback callback,
                                  void *user_data)
{
    if (!scanner) return;
    pthread_mutex_lock(&scanner->mutex);
    scanner->callback = callback;
    scanner->callback_user_data = user_data;
    pthread_mutex_unlock(&scanner->mutex);
}

bool silence_scanner_is_running(const SilenceScanner *scanner)
{
    if (!scanner) return false;
    bool running;
    SilenceScanner *nonconst = (SilenceScanner *)(const void *)scanner;
    pthread_mutex_lock(&nonconst->mutex);
    running = nonconst->running;
    pthread_mutex_unlock(&nonconst->mutex);
    return running;
}

size_t silence_scanner_get_transcribed_offset(const SilenceScanner *scanner)
{
    if (!scanner) return 0;
    size_t offset;
    SilenceScanner *nonconst = (SilenceScanner *)(const void *)scanner;
    pthread_mutex_lock(&nonconst->mutex);
    offset = nonconst->transcribed_offset;
    pthread_mutex_unlock(&nonconst->mutex);
    return offset;
}
