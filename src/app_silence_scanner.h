/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_SILENCE_SCANNER_H
#define APP_SILENCE_SCANNER_H

/**
 * @file app_silence_scanner.h
 * @brief Continuous silence scanner for continuous dictation mode
 *
 * Monitors the ring buffer for silence periods and triggers transcription
 * of audio segments when silence exceeds a configurable threshold.
 *
 * Key features:
 *   - Non-blocking: runs in its own thread, scanning 500ms chunks
 *   - Minimum segment guard: never sends segments shorter than min_segment_ms
 *   - Short silences are absorbed into surrounding audio
 *   - Only active in continuous dictation mode
 *
 * Thread model:
 *   - Scanner thread: reads ring buffer, runs VAD, triggers callbacks
 *   - GTK main thread: receives transcription results via g_idle_add
 *   - ALSA capture thread: writes to ring buffer independently
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include "app_vad.h"
#include "app_ring_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Silence Scanner Handle (Opaque)
 *---------------------------------------------------------------------------*/
typedef struct _SilenceScanner SilenceScanner;

/*---------------------------------------------------------------------------
 * Segment Ready Callback
 *---------------------------------------------------------------------------*/

/**
 * Callback invoked when a segment of audio is ready for transcription.
 * The caller must free the samples array with g_free() after use.
 */
typedef void (*scanner_segment_callback)(int16_t *samples, size_t count, void *user_data);

/*---------------------------------------------------------------------------
 * Creation and Destruction
 *---------------------------------------------------------------------------*/

/**
 * Create a silence scanner instance.
 *
 * @param rb                  Ring buffer to monitor.
 * @param mode                VAD aggressiveness mode (0-3).
 * @param silence_threshold_ms Silence duration (ms) before triggering segment extraction.
 * @param min_segment_ms      Minimum segment duration (ms) before sending to Whisper.
 * @return Valid SilenceScanner* on success, NULL on failure.
 */
SilenceScanner *silence_scanner_create(AudioRingBuffer *rb,
                                       VadMode mode,
                                       int silence_threshold_ms,
                                       int min_segment_ms);

/**
 * Destroy a silence scanner and free all resources.
 * If the scanner thread is running, it will be stopped first.
 *
 * @param scanner Pointer to a valid SilenceScanner. Must not be NULL.
 */
void silence_scanner_destroy(SilenceScanner *scanner);

/*---------------------------------------------------------------------------
 * Control
 *---------------------------------------------------------------------------*/

/**
 * Start the scanner thread.
 *
 * @param scanner Scanner instance.
 * @return true on success, false if already running or thread creation failed.
 */
bool silence_scanner_start(SilenceScanner *scanner);

/**
 * Stop the scanner thread gracefully.
 * Waits for the thread to finish its current iteration.
 *
 * @param scanner Scanner instance.
 */
void silence_scanner_stop(SilenceScanner *scanner);

/**
 * Reset scanner state for a new recording session.
 * Clears offsets and VAD state without stopping the thread.
 *
 * @param scanner Scanner instance.
 */
void silence_scanner_reset(SilenceScanner *scanner);

/*---------------------------------------------------------------------------
 * Callback Configuration
 *---------------------------------------------------------------------------*/

/**
 * Set the callback invoked when a segment is ready for transcription.
 *
 * @param scanner    Scanner instance.
 * @param callback   Callback function.
 * @param user_data  User data passed to the callback.
 */
void silence_scanner_set_callback(SilenceScanner *scanner,
                                  scanner_segment_callback callback,
                                  void *user_data);

/*---------------------------------------------------------------------------
 * Status
 *---------------------------------------------------------------------------*/

/**
 * Check if the scanner thread is currently running.
 *
 * @param scanner Scanner instance.
 * @return true if the scanner thread is active.
 */
bool silence_scanner_is_running(const SilenceScanner *scanner);

/**
 * Get the number of samples already sent for transcription.
 * This offset is relative to the oldest sample in the ring buffer
 * at the time of the last reset. Use this to skip already-transcribed
 * audio when extracting remaining samples from the ring buffer.
 *
 * @param scanner Scanner instance.
 * @return Number of samples already transcribed.
 */
size_t silence_scanner_get_transcribed_offset(const SilenceScanner *scanner);

#ifdef __cplusplus
}
#endif

#endif /* APP_SILENCE_SCANNER_H */
