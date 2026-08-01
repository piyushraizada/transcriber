/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_AUDIO_H
#define APP_AUDIO_H

/**
 * @file app_audio.h
 * @brief Audio capture module — ALSA backend
 *
 * This module implements the audio capture pipeline using ALSA for
 * recording PCM audio at 16kHz/mono/16-bit.
 *
 * The module manages temporary WAV file creation, ensuring
 * secure file naming and proper cleanup. All audio buffers are scrubbed
 * from memory after use to prevent sensitive audio data from lingering
 * in memory.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <glib.h>
#include "app_ring_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Section 1: Audio Configuration Structure
 *---------------------------------------------------------------------------
 * Defines the fixed audio capture parameters.
 *
 * These values are NOT configurable by the user — they are fixed to ensure
 * compatibility with whisper.cpp, which expects 16kHz mono PCM input.
 */
typedef struct {
    uint32_t sample_rate;      ///< Sample rate in Hz — fixed at 16000
    uint32_t channels;         ///< Number of channels — fixed at 1/mono
    uint16_t bits_per_sample;  ///< Bit depth — fixed at 16
    uint32_t buffer_size;      ///< PCM buffer size in frames — 320 (20ms at 16kHz, matches WebRTC VAD)
} AudioFormat;

/*---------------------------------------------------------------------------
 * Section 2: Audio Recorder Handle (Opaque)
 *---------------------------------------------------------------------------
 * Opaque handle to the internal audio recorder state.
 */
typedef struct _AudioRecorder AudioRecorder;

/*---------------------------------------------------------------------------
 * Section 3: Initialization and Cleanup
 *---------------------------------------------------------------------------
 * Functions for creating, initializing, and destroying the audio recorder.
 */

/**
 * Create and initialize a new audio recorder instance.
 *
 * @param format Audio format parameters (pass NULL for defaults)
 * @return A valid AudioRecorder* on success, or NULL on allocation failure.
 */
AudioRecorder *audio_recorder_create(const AudioFormat *format);

/**
 * Destroy an audio recorder and free all associated resources.
 *
 * @param recorder Pointer to a valid AudioRecorder. Must not be NULL.
 */
void audio_recorder_destroy(AudioRecorder *recorder);

/*---------------------------------------------------------------------------
 * Section 3: Device Configuration
 *---------------------------------------------------------------------------
 * Functions for configuring the audio input device.
 */

/**
 * Set the audio device name.
 *
 * @param recorder AudioRecorder handle
 * @param device Device name (e.g., "default", "hw:0,0")
 * @return true on success, false on failure
 */
bool audio_recorder_set_device(AudioRecorder *recorder, const char *device);

/**
 * Enable or disable noise suppression for the next recording session.
 *
 * Call this before audio_recorder_start() to configure whether RNNoise-based
 * background noise removal should be applied during capture. The setting is
 * evaluated when start() creates the internal processing handle.
 *
 * @param recorder AudioRecorder handle
 * @param enabled  true = enable noise suppression, false = disable
 */
void audio_recorder_set_noise_suppression(AudioRecorder *recorder, bool enabled);

/**
 * Get the current audio device name.
 *
 * @param recorder AudioRecorder handle
 * @return Device name string (empty string = system default)
 */
const char *audio_recorder_get_device(const AudioRecorder *recorder);

/*---------------------------------------------------------------------------
 * Section 4: Recording Control
 *---------------------------------------------------------------------------
 * Functions for starting and stopping audio recording.
 */

/**
 * Start audio recording to a temporary WAV file and ring buffer.
 *
 * @param recorder      Pointer to a valid AudioRecorder. Must not be NULL.
 * @return true if recording started successfully, false on failure.
 *
 * @note Duration enforcement is handled externally by the application's
 *       watchdog timer. In continuous mode, the silence scanner handles
 *       automatic segment detection and transcription.
 */
bool audio_recorder_start(AudioRecorder *recorder);

/**
 * Stop the current audio recording and finalize the WAV file.
 *
 * @param recorder Pointer to a valid AudioRecorder. Must not be NULL.
 * @return true if recording stopped successfully, false if not recording.
 */
bool audio_recorder_stop(AudioRecorder *recorder);

/*---------------------------------------------------------------------------
 * Section 5: WAV File Management
 *---------------------------------------------------------------------------
 * Functions for accessing and managing the temporary WAV file.
 */

/**
 * Get the path to the current WAV file.
 *
 * @param recorder Pointer to a valid AudioRecorder. Must not be NULL.
 * @return A null-terminated string containing the WAV file path, or empty
 *         string if no recording is active.
 */
const char *audio_recorder_get_wav_path(const AudioRecorder *recorder);

/**
 * Get the current RMS volume level (0.0 to 1.0).
 *
 * This value is updated in real-time by the capture thread during recording.
 * It represents the root-mean-square amplitude of the most recent PCM buffer,
 * normalized to the full range of 16-bit signed samples (32768).
 *
 * Thread-safe: protected by volume_mutex in the AudioRecorder struct.
 * Callers should treat this as a snapshot that may be slightly stale.
 *
 * @param recorder Pointer to a valid AudioRecorder. Must not be NULL.
 * @return RMS volume level in range [0.0, 1.0], or 0.0 if not recording.
 */
double audio_recorder_get_volume_level(const AudioRecorder *recorder);

/*---------------------------------------------------------------------------
 * Section 6: Audio Format Utilities
 *---------------------------------------------------------------------------
 * Functions for working with the fixed audio format parameters.
 */

/**
 * Get the fixed audio format configuration.
 *
 * @return An AudioFormat struct with the fixed parameters.
 */
AudioFormat audio_format_get_default(void);

/*---------------------------------------------------------------------------
 * Section 7: Error Handling and Diagnostics
 *---------------------------------------------------------------------------
 * Functions for retrieving error information and diagnostic data.
 */

/**
 * Get the last error message from the audio recorder.
 *
 * @return A null-terminated string containing the error message, or an
 *         empty string if no error has occurred.
 */
const char *audio_recorder_get_error(void);

/**
 * Clear the current error message.
 */
void audio_recorder_reset_error(void);

/*---------------------------------------------------------------------------
 * Section 8: Device Listing
 *---------------------------------------------------------------------------
 * Functions for listing available audio input devices.
 */

/**
 * Holds a list of audio capture devices with both user-friendly display names
 * and internal ALSA device names.
 *
 * Each entry has:
 *   - display_names[i]: Human-readable name (e.g., "Built-in Microphone (hw:0,0)")
 *   - device_names[i]:  ALSA device identifier (e.g., "hw:0,0")
 */
typedef struct {
    gchar **display_names;  ///< User-friendly names (NULL-terminated, caller owns)
    gchar **device_names;   ///< ALSA device names (NULL-terminated, caller owns)
    gint count;             ///< Number of devices
} AudioDeviceList;

/**
 * Get the list of available ALSA capture devices.
 *
 * @param recorder Pointer to a valid AudioRecorder (used for backend info).
 * @return A newly allocated AudioDeviceList*, or NULL on error.
 *         The caller must free the result using audio_device_list_free().
 */
AudioDeviceList *audio_recorder_get_device_list(const AudioRecorder *recorder, bool log_enumeration);

/**
 * Free a device list returned by audio_recorder_get_device_list().
 *
 * @param list Pointer to the AudioDeviceList struct. May be NULL (no-op).
 */
void audio_device_list_free(AudioDeviceList *list);

/**
 * Delete the WAV file from disk. Call this AFTER transcription is done.
 */
bool audio_recorder_delete_wav(AudioRecorder *recorder);

/*---------------------------------------------------------------------------
 * Section 11: Ring Buffer Access
 *---------------------------------------------------------------------------
 * Extract captured audio from the in-memory ring buffer for transcription.
 */

/**
 * Extract all PCM samples from the ring buffer into a contiguous array.
 *
 * The returned buffer is allocated with g_malloc() and must be freed by the
 * caller using g_free(). Data is ordered from oldest to newest sample.
 * After extraction, the ring buffer is emptied.
 *
 * @param recorder      AudioRecorder handle
 * @param out_samples   Output: pointer to allocated int16_t array (set by caller to NULL)
 * @return Number of samples extracted, or 0 if ring buffer unavailable/empty
 */
size_t audio_recorder_extract_samples(AudioRecorder *recorder, int16_t **out_samples);

/**
 * Trim trailing silence from a PCM sample buffer.
 *
 * Scans backwards from the end of the buffer in 20ms frames (320 samples
 * at 16kHz) and removes any frames whose RMS amplitude falls below the
 * silence threshold. This prevents whisper from receiving trailing silence
 * (e.g., the 1-second silence period that triggers VAD auto-stop) which
 * can cause hallucinated transcriptions.
 *
 * The samples array is modified in-place by reducing the sample count.
 * The caller is responsible for freeing the buffer.
 *
 * @param samples      Pointer to int16_t PCM samples (16kHz, mono).
 * @param n_samples    Initial number of samples.
 * @param sample_rate  Sample rate in Hz (must be 16000).
 * @return Number of non-silence samples remaining (<= n_samples).
 */
size_t audio_trim_trailing_silence(int16_t *samples, size_t n_samples, uint32_t sample_rate);

/**
 * Get the ring buffer associated with this recorder.
 * Returns NULL if the recorder has no ring buffer (not started yet).
 * The ring buffer is owned by the recorder — do not free it.
 */
AudioRingBuffer *audio_recorder_get_ring_buffer(const AudioRecorder *recorder);

#ifdef __cplusplus
}
#endif

#endif /* APP_AUDIO_H */
