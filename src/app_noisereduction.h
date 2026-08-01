/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_NOISEREDUCTION_H
#define APP_NOISEREDUCTION_H

/**
 * @file app_noisereduction.h
 * @brief RNNoise-based noise suppression wrapper module
 *
 * This module provides a thin C wrapper around the RNNoise library.
 * The ALSA device is opened at RNNoise's native 48kHz rate; each
 * 480-sample (10ms) frame is denoised in place, then low-pass filtered
 * and decimated by 3 to 16kHz, which is the rate the rest of the
 * pipeline (WAV, ring buffer, whisper) expects.
 *
 * The pipeline per 10ms frame:
 *   1. 480 int16_t @ 48kHz -> 480 float (raw int16 scale, no normalization)
 *   2. rnnoise_process_frame() (RNNoise native frame size)
 *   3. 31-tap Hamming-windowed sinc low-pass (cutoff 7kHz) + decimate
 *      by 3 -> 160 int16_t @ 16kHz
 *
 * The 16kHz output is what must be written to the WAV file and ring
 * buffer — the format/sample rate fields elsewhere in the app stay 16kHz.
 *
 * Thread safety: Each NoiseReductionHandle has its own RNNoise state — no
 * shared mutable state between threads. Safe to use from the capture
 * thread exclusively.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * NoiseReductionHandle (Opaque)
 *---------------------------------------------------------------------------*/

typedef struct _NoiseReductionHandle NoiseReductionHandle;

/**
 * Create a new noise reduction handle.
 * Initializes internal RNNoise state and processing buffers.
 *
 * @param sample_rate  Input sample rate in Hz (must be 48000 — RNNoise
 *                     native rate).
 * @return Valid handle on success, NULL on failure.
 */
NoiseReductionHandle *noise_reduction_create(uint32_t sample_rate);

/**
 * Destroy a noise reduction handle and free all resources.
 *
 * @param handle Pointer to NoiseReductionHandle. May be NULL (no-op).
 */
void noise_reduction_destroy(NoiseReductionHandle *handle);

/**
 * Process a single frame of audio for noise suppression.
 *
 * The input buffer is read and the denoised output is written to the
 * output buffer. The input must contain exactly 480 int16_t samples
 * (10ms at 48kHz — one RNNoise frame). The output buffer receives
 * exactly 160 int16_t samples (10ms at 16kHz).
 *
 * NOTE: RNNoise expects raw int16-scale input (the official demo feeds
 * int16 values directly as floats). Do NOT normalize the input to
 * [-1,1] and do NOT rescale the output — doing so turns the signal
 * into digital silence.
 *
 * @param handle     NoiseReductionHandle from noise_reduction_create().
 * @param input      Input PCM samples (int16_t, 48kHz mono). Must not be NULL.
 * @param output     Output buffer for denoised samples (int16_t, 16kHz
 *                   mono, 160 samples). May equal input.
 * @param n_samples  Number of input samples (must be 480 = RNNoise frame
 *                   size at 48kHz).
 * @return true on success, false on error (e.g., bad parameters).
 */
bool noise_reduction_process_frame(NoiseReductionHandle *handle,
                                    const int16_t *input,
                                    int16_t *output,
                                    size_t n_samples);

#ifdef __cplusplus
}
#endif

#endif /* APP_NOISEREDUCTION_H */
