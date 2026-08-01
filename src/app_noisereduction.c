/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * app_noisereduction.c — RNNoise-based noise suppression wrapper
 *
 * Captures audio at RNNoise's native 48kHz rate and downsamples the
 * denoised output to 16kHz for the rest of the pipeline.
 * Pipeline per 10ms frame:
 *   480 int16_t @ 48kHz -> float (raw scale, NO normalization)
 *     -> rnnoise_process_frame()
 *     -> FIR low-pass (31-tap Hamming sinc, cutoff 7kHz)
 *     -> decimate by 3 -> 160 int16_t @ 16kHz
 *
 * IMPORTANT: RNNoise expects raw int16-scale samples — the official
 * demo (examples/rnnoise_demo.c) feeds int16 values directly as floats.
 * Do NOT normalize the input to [-1,1] and do NOT rescale the output;
 * doing so turns the entire signal into digital silence (verified
 * numerically: input RMS 8513.6 -> output RMS 0.0000).
 *
 * Thread safety: Each NoiseReductionHandle has its own RNNoise state —
 * no shared mutable state between threads. Safe to use from the capture
 * thread exclusively.
 */

#include "app_noisereduction.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* RNNoise headers from FetchContent */
#include "rnnoise.h"

/*---------------------------------------------------------------------------
 * Constants
 *---------------------------------------------------------------------------*/

#define NS_CAPTURE_RATE     48000   /* RNNoise native rate */
#define NS_FRAME_SIZE       480     /* 10ms at 48kHz = one RNNoise frame */
#define NS_OUT_RATE         16000   /* Pipeline rate (WAV, ring buffer, whisper) */
#define NS_DECIMATE_FACTOR  3       /* 48000 / 16000 */
#define NS_OUT_FRAME_SIZE   (NS_FRAME_SIZE / NS_DECIMATE_FACTOR)  /* 160 */
#define NS_FIR_TAPS         31      /* Low-pass filter length @48kHz */
#define NS_FIR_CUTOFF       7000.f  /* Hz; output Nyquist is 8kHz */

/*---------------------------------------------------------------------------
 * NoiseReductionHandle struct
 *---------------------------------------------------------------------------*/

struct _NoiseReductionHandle {
    DenoiseState *rnnoise_state;    /* RNNoise internal state (opaque) */
    float *fir_coeffs;              /* 31 Hamming-windowed sinc taps */
    float *x_full;                  /* [30 history + 480 current] floats for FIR */
};

/*---------------------------------------------------------------------------
 * FIR design: Hamming-windowed sinc low-pass, cutoff NS_FIR_CUTOFF @48kHz
 *---------------------------------------------------------------------------*/

static void design_lowpass_fir(float *h, int taps, float cutoff_hz, float rate_hz)
{
    const float fc = cutoff_hz / rate_hz;  /* normalized cutoff */
    float sum = 0.f;
    const int M = taps - 1;

    for (int n = 0; n < taps; n++) {
        float m = (float)n - (float)M * 0.5f;
        float sinc = (m == 0.f) ? (2.f * fc)
                                : (float)(sin(2.0 * M_PI * fc * m) / (M_PI * m));
        /* Hamming window */
        float w = 0.54f - 0.46f * (float)cos(2.0 * M_PI * (double)n / (double)M);
        h[n] = sinc * w;
        sum += h[n];
    }
    /* Normalize so DC gain is 1.0 */
    for (int n = 0; n < taps; n++) {
        h[n] /= sum;
    }
}

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/

NoiseReductionHandle *noise_reduction_create(uint32_t sample_rate)
{
    /* RNNoise only processes at its native 48kHz rate */
    if (sample_rate != NS_CAPTURE_RATE) {
        return NULL;
    }

    NoiseReductionHandle *handle = calloc(1, sizeof(NoiseReductionHandle));
    if (!handle) return NULL;

    handle->rnnoise_state = rnnoise_create(NULL);
    if (!handle->rnnoise_state) {
        free(handle);
        return NULL;
    }

    handle->fir_coeffs = calloc(NS_FIR_TAPS, sizeof(float));
    if (!handle->fir_coeffs) {
        rnnoise_destroy(handle->rnnoise_state);
        free(handle);
        return NULL;
    }
    design_lowpass_fir(handle->fir_coeffs, NS_FIR_TAPS, NS_FIR_CUTOFF, NS_CAPTURE_RATE);

    /* 30 samples of filter history + 480 current samples */
    handle->x_full = calloc((NS_FIR_TAPS - 1) + NS_FRAME_SIZE, sizeof(float));
    if (!handle->x_full) {
        free(handle->fir_coeffs);
        rnnoise_destroy(handle->rnnoise_state);
        free(handle);
        return NULL;
    }

    return handle;
}

void noise_reduction_destroy(NoiseReductionHandle *handle)
{
    if (!handle) return;

    if (handle->x_full) {
        /* Scrub buffers to avoid leaking audio data */
        volatile float *p = handle->x_full;
        size_t n = (NS_FIR_TAPS - 1) + NS_FRAME_SIZE;
        while (n--) *p++ = 0.f;
        free(handle->x_full);
    }
    if (handle->fir_coeffs) {
        volatile float *p = handle->fir_coeffs;
        size_t n = NS_FIR_TAPS;
        while (n--) *p++ = 0.f;
        free(handle->fir_coeffs);
    }

    if (handle->rnnoise_state) {
        rnnoise_destroy(handle->rnnoise_state);
    }

    memset(handle, 0, sizeof(NoiseReductionHandle));
    free(handle);
}

bool noise_reduction_process_frame(NoiseReductionHandle *handle,
                                    const int16_t *input,
                                    int16_t *output,
                                    size_t n_samples)
{
    if (!handle || !input || !output || n_samples != NS_FRAME_SIZE) {
        return false;
    }

    /* Step 1: int16 -> float at RAW scale (RNNoise expects int16 range,
     * NOT normalized to [-1,1] — see module doc). Copy into the current
     * region of x_full, after the 30-sample FIR history. */
    float *cur = handle->x_full + (NS_FIR_TAPS - 1);
    for (size_t i = 0; i < NS_FRAME_SIZE; i++) {
        cur[i] = (float)input[i];
    }

    /* Step 2: RNNoise denoising, in-place (input fully consumed into the
     * frame analysis before synthesis writes the output). */
    (void)rnnoise_process_frame(handle->rnnoise_state, cur, cur);

    /* Step 3: FIR low-pass + decimate by 3 (480 -> 160 samples @16kHz).
     * Output sample j is the filter response at position j*3, so the
     * convolution window spans x_full[j*3 .. j*3 + TAPS-1]. */
    for (int j = 0; j < NS_OUT_FRAME_SIZE; j++) {
        const float *win = handle->x_full + j * NS_DECIMATE_FACTOR;
        float acc = 0.f;
        for (int k = 0; k < NS_FIR_TAPS; k++) {
            acc += handle->fir_coeffs[k] * win[k];
        }
        if (acc > 32767.f) acc = 32767.f;
        else if (acc < -32768.f) acc = -32768.f;
        output[j] = (int16_t)(acc + (acc >= 0 ? 0.5f : -0.5f));
    }

    /* Step 4: Persist the last 30 denoised samples as FIR history for the
     * next frame (filter spans frame boundaries). */
    memmove(handle->x_full, handle->x_full + NS_FRAME_SIZE,
            (NS_FIR_TAPS - 1) * sizeof(float));

    return true;
}
