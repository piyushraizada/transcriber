/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * app_file_segmenter.c — VAD-based segmentation of decoded file PCM
 *
 * Implements the spec's batch segmentation algorithm over a finite,
 * in-memory 16kHz mono int16 buffer:
 *
 *   For each 30-second window starting at the current offset:
 *     1. Frames in the first 5 seconds (noise buffer) are classified but
 *        never used as a boundary.
 *     2. The last silence frame in [5s, 30s] marks the boundary.
 *     3. Segment = [window_start, boundary). If no silence exists in
 *        [5s, 30s], segment = the full window (clamped to remaining data).
 *   After the last full window, a trailing remainder < 5s is emitted as-is
 *   (REQ-11); a remainder >= 5s simply starts another window.
 *
 * Frame classification uses the WebRTC VAD wrapper (app_vad) with 20ms
 * (320-sample) frames at 16kHz — the same frame size the live
 * SilenceScanner uses. A single VadDetector instance is created for the
 * whole buffer and destroyed before return; WebRTC VAD is stateful, so
 * frames must (and do) flow through it in order.
 *
 * Memory: the segment array is g_new0'd and owned by the caller (g_free).
 * No other allocations are retained.
 */

#include "app_file_segmenter.h"
#include "app.h"  /* For UNUSED macro */

#include <glib.h>

/* Pipeline constants — 16kHz mono, matching the whisper.cpp input format. */
#define SEG_SAMPLE_RATE      16000u
#define SEG_FRAME_SAMPLES    320u   /* 20ms at 16kHz — WebRTC VAD frame size */
#define SEG_WINDOW_SECONDS   30
#define SEG_NOISE_BUFFER_SEC 5

#define SEG_WINDOW_SAMPLES   ((size_t)SEG_WINDOW_SECONDS * SEG_SAMPLE_RATE)
#define SEG_NOISE_SAMPLES    ((size_t)SEG_NOISE_BUFFER_SEC * SEG_SAMPLE_RATE)

/* Upper bound on segments for a 600s file at the minimum possible segment
 * length (one 30s window each) plus a tail — used only as a sanity cap on
 * the dynamic array growth. */
#define SEG_MAX_SEGMENTS     256

PcmSegment *file_segmenter_segment(const int16_t *pcm, size_t n_samples,
                                   VadMode mode, size_t *out_count) {
    if (out_count) {
        *out_count = 0;
    }
    if (!pcm || n_samples == 0) {
        return NULL;
    }
    if (!out_count) {
        return NULL;
    }

    VadDetector *vad = vad_detector_create(mode);
    if (!vad) {
        g_log("app-file-seg", G_LOG_LEVEL_WARNING,
              "[file-seg] Failed to create VAD detector — emitting single segment\n");
        /* Degraded path: no silence detection available, so the whole
         * buffer is one segment (whisper.cpp handles it in 30s chunks). */
        PcmSegment *fallback = g_new0(PcmSegment, 1);
        fallback[0].start = 0;
        fallback[0].end = n_samples;
        *out_count = 1;
        return fallback;
    }

    PcmSegment *segments = g_new0(PcmSegment, 8);
    size_t seg_count = 0;
    size_t window_start = 0;

    while (window_start < n_samples) {
        size_t window_end = window_start + SEG_WINDOW_SAMPLES;
        if (window_end > n_samples) {
            window_end = n_samples;
        }
        size_t window_len = window_end - window_start;

        /* A trailing remainder shorter than the noise buffer is transcribed
         * as-is without VAD (REQ-11). */
        if (window_len < SEG_NOISE_SAMPLES) {
            if (seg_count >= SEG_MAX_SEGMENTS) {
                break;  /* Sanity cap — should be unreachable for 600s files */
            }
            segments[seg_count].start = window_start;
            segments[seg_count].end = window_end;
            seg_count++;
            window_start = window_end;
            continue;
        }

        /* Classify every 20ms frame in the window. Record the sample offset
         * of the LAST silence frame at or after the noise buffer. */
        size_t last_silence_offset = 0;  /* 0 = none found */
        size_t frame_pos = window_start;
        while (frame_pos + SEG_FRAME_SAMPLES <= window_end) {
            bool voice = vad_process_frame(vad, pcm + frame_pos,
                                           SEG_FRAME_SAMPLES, SEG_SAMPLE_RATE);
            if (!voice && frame_pos >= window_start + SEG_NOISE_SAMPLES) {
                last_silence_offset = frame_pos;
            }
            frame_pos += SEG_FRAME_SAMPLES;
        }

        if (seg_count >= SEG_MAX_SEGMENTS) {
            break;  /* Sanity cap */
        }

        if (last_silence_offset > 0) {
            /* Cut at the last detected silence (REQ-09). */
            segments[seg_count].start = window_start;
            segments[seg_count].end = last_silence_offset;
            seg_count++;
            window_start = last_silence_offset;
        } else {
            /* No silence in [5s, 30s] — emit the whole window (REQ-10). */
            segments[seg_count].start = window_start;
            segments[seg_count].end = window_end;
            seg_count++;
            window_start = window_end;
        }
    }

    vad_detector_destroy(vad);

    if (seg_count == 0) {
        g_free(segments);
        return NULL;
    }

    *out_count = seg_count;
    return segments;
}
