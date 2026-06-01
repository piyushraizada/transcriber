/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 *
 * This module uses WebRTC VAD (Voice Activity Detection) by CPUImage / Google.
 * See third_party/webrtc_vad/LICENSE for the BSD-3-Clause license.
 */

#ifndef APP_VAD_H
#define APP_VAD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * VAD Aggressiveness Mode
 *---------------------------------------------------------------------------
 * Higher modes are more restrictive in reporting speech:
 *   0 = Least aggressive (most sensitive, more false positives)
 *   1 = Moderate
 *   2 = Aggressive
 *   3 = Most aggressive (most restrictive, fewer false positives)
 *---------------------------------------------------------------------------*/
typedef enum {
    VAD_MODE_LEAST_RESTRICTIVE = 0,  // Most sensitive, catches quiet speech
    VAD_MODE_MODERATE          = 1,
    VAD_MODE_AGGRESSIVE        = 2,
    VAD_MODE_MOST_RESTRICTIVE  = 3,  // Most restrictive, only clear speech
} VadMode;

/*---------------------------------------------------------------------------
 * VAD Instance Handle (Opaque)
 *---------------------------------------------------------------------------*/
typedef struct _VadDetector VadDetector;

/*---------------------------------------------------------------------------
 * Initialization and Cleanup
 *---------------------------------------------------------------------------*/

/**
 * Create and initialize a new VAD detector instance.
 *
 * @param mode Aggressiveness mode for voice detection.
 * @return A valid VadDetector* on success, or NULL on failure.
 */
VadDetector *vad_detector_create(VadMode mode);

/**
 * Destroy a VAD detector and free all associated resources.
 *
 * @param detector Pointer to a valid VadDetector. Must not be NULL.
 */
void vad_detector_destroy(VadDetector *detector);

/*---------------------------------------------------------------------------
 * VAD Processing
 *---------------------------------------------------------------------------*/

/**
 * Process an audio frame and determine if it contains voice activity.
 *
 * Supports sampling rates: 8000, 16000, 32000 Hz.
 * Frame lengths must correspond to 10ms, 20ms, or 30ms at the given rate.
 *
 * @param detector      VAD detector instance.
 * @param audio_frame   Pointer to PCM audio data (int16_t samples).
 * @param frame_length  Number of samples in the frame.
 * @param sample_rate   Sampling rate in Hz (8000, 16000, or 32000).
 * @return true if voice is detected, false if silence or error.
 */
bool vad_process_frame(VadDetector *detector,
                       const int16_t *audio_frame,
                       size_t frame_length,
                       uint32_t sample_rate);

/*---------------------------------------------------------------------------
 * Mode Configuration
 *---------------------------------------------------------------------------*/

/**
 * Change the VAD aggressiveness mode at runtime.
 *
 * @param detector  VAD detector instance.
 * @param mode      New aggressiveness mode.
 * @return true on success, false on failure.
 */
bool vad_set_mode(VadDetector *detector, VadMode mode);

#ifdef __cplusplus
}
#endif

#endif  // APP_VAD_H
