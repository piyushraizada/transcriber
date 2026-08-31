/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_FILE_SEGMENTER_H
#define APP_FILE_SEGMENTER_H

/**
 * @file app_file_segmenter.h
 * @brief VAD-based segmentation of a decoded PCM buffer into transcribable windows
 *
 * This module implements the file-upload segmentation algorithm from
 * plans/audio-file-upload-transcription-spec.md (REQ-07..REQ-11):
 *
 *   - Audio is processed in fixed 30-second windows.
 *   - Within each window the first 5 seconds are ignored as a noise buffer.
 *   - The LAST silence point between seconds 5 and 30 is used as the segment
 *     boundary; the segment spans [window_start, boundary).
 *   - If no silence is found in [5s, 30s], the whole 30-second window is
 *     emitted as a single segment.
 *   - A trailing remainder shorter than 5 seconds is emitted as-is (no VAD).
 *
 * The function is synchronous and operates on a contiguous in-memory PCM
 * buffer (16kHz mono int16) — it does not use the ring buffer or spawn a
 * thread, unlike the live SilenceScanner which is coupled to a continuous
 * stream. It reuses the app_vad WebRTC VAD wrapper for frame classification.
 *
 * Threading: safe to call from any single thread; not internally
 * synchronized. Intended to run on the file-transcription background thread.
 */

#include <stdint.h>
#include <stddef.h>
#include "app_vad.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * PcmSegment — A slice of the decoded PCM buffer
 *---------------------------------------------------------------------------
 * Offsets are in samples (16kHz mono int16). The segment covers the half-open
 * range [start, end).
 */
typedef struct {
    size_t start;  ///< First sample index (inclusive)
    size_t end;    ///< One past the last sample index (exclusive)
} PcmSegment;

/**
 * Segment a decoded PCM buffer into transcribable windows.
 *
 * @param pcm        16kHz mono int16 PCM buffer (may be NULL if n_samples is 0)
 * @param n_samples  Total number of int16 samples in pcm
 * @param mode       VAD aggressiveness mode for silence detection
 * @param out_count  Receives the number of segments in the returned array
 * @return A g_new0'd array of PcmSegment, or NULL if there are no segments
 *         (in which case *out_count is 0). Caller must free with g_free().
 */
PcmSegment *file_segmenter_segment(const int16_t *pcm, size_t n_samples,
                                   VadMode mode, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* APP_FILE_SEGMENTER_H */
