/*
 * SPDX-License-Identifier: MIT
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

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Section 1: Audio Backend Enum
 *---------------------------------------------------------------------------
 * Represents the audio capture backend. Only ALSA is supported.
 */
typedef enum {
    AUDIO_BACKEND_ALSA,  ///< ALSA — direct hardware access
    AUDIO_BACKEND_NONE   ///< No backend available — recording disabled
} AudioBackend;

/*---------------------------------------------------------------------------
 * Section 2: Audio Configuration Structure
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
    uint32_t buffer_size;      ///< PCM buffer size in frames — 1024
} AudioFormat;

/*---------------------------------------------------------------------------
 * Section 3: Audio Recorder Handle (Opaque)
 *---------------------------------------------------------------------------
 * Opaque handle to the internal audio recorder state.
 */
typedef struct _AudioRecorder AudioRecorder;

/*---------------------------------------------------------------------------
 * Section 4: Initialization and Cleanup
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
 * Section 5: Device Configuration
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
 * Get the current audio device name.
 *
 * @param recorder AudioRecorder handle
 * @return Device name string (empty string = system default)
 */
const char *audio_recorder_get_device(const AudioRecorder *recorder);

/**
 * Get the currently selected audio backend.
 *
 * @param recorder Pointer to a valid AudioRecorder. Must not be NULL.
 * @return The selected AudioBackend enum value.
 */
AudioBackend audio_recorder_get_backend(const AudioRecorder *recorder);

/*---------------------------------------------------------------------------
 * Section 6: Recording Control
 *---------------------------------------------------------------------------
 * Functions for starting and stopping audio recording.
 */

/**
 * Start audio recording to a temporary WAV file.
 *
 * @param recorder      Pointer to a valid AudioRecorder. Must not be NULL.
 * @return true if recording started successfully, false on failure.
 *
 * @note Duration enforcement is handled by the watchdog timer in main.c,
 *       not by this function. The audio module has no concept of duration limits.
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
 * Section 7: WAV File Management
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
 * Thread-safe: the field is volatile and accessed via simple load/store.
 * On x86/x64, double-width loads are atomic. Callers should treat this as
 * a snapshot that may be slightly stale.
 *
 * @param recorder Pointer to a valid AudioRecorder. Must not be NULL.
 * @return RMS volume level in range [0.0, 1.0], or 0.0 if not recording.
 */
double audio_recorder_get_volume_level(const AudioRecorder *recorder);

/*---------------------------------------------------------------------------
 * Section 8: Audio Format Utilities
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
 * Section 9: Error Handling and Diagnostics
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
 * Section 10: Device Listing
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
AudioDeviceList *audio_recorder_get_device_list(const AudioRecorder *recorder);

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

#ifdef __cplusplus
}
#endif

#endif /* APP_AUDIO_H */
