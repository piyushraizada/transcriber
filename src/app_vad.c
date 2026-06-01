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

#include "app_vad.h"
#include "webrtc_vad.h"
#include <glib.h>

struct _VadDetector {
    VadInst *vad_inst;
};

VadDetector *vad_detector_create(VadMode mode)
{
    VadDetector *det = g_new0(VadDetector, 1);
    if (!det) {
        return NULL;
    }

    det->vad_inst = WebRtcVad_Create();
    if (!det->vad_inst) {
        g_free(det);
        return NULL;
    }

    if (WebRtcVad_Init(det->vad_inst) != 0) {
        WebRtcVad_Free(det->vad_inst);
        g_free(det);
        return NULL;
    }

    if (WebRtcVad_set_mode(det->vad_inst, (int)mode) != 0) {
        WebRtcVad_Free(det->vad_inst);
        g_free(det);
        return NULL;
    }

    return det;
}

void vad_detector_destroy(VadDetector *detector)
{
    if (!detector) {
        return;
    }
    if (detector->vad_inst) {
        WebRtcVad_Free(detector->vad_inst);
    }
    g_free(detector);
}

bool vad_process_frame(VadDetector *detector,
                       const int16_t *audio_frame,
                       size_t frame_length,
                       uint32_t sample_rate)
{
    if (!detector || !detector->vad_inst || !audio_frame) {
        return false;
    }

    // Validate sampling rate and frame length
    if (WebRtcVad_ValidRateAndFrameLength((int)sample_rate, frame_length) != 0) {
        return false;
    }

    int result = WebRtcVad_Process(detector->vad_inst,
                                   (int)sample_rate,
                                   audio_frame,
                                   frame_length);
    return (result == 1);
}

bool vad_set_mode(VadDetector *detector, VadMode mode)
{
    if (!detector || !detector->vad_inst) {
        return false;
    }
    return (WebRtcVad_set_mode(detector->vad_inst, (int)mode) == 0);
}
