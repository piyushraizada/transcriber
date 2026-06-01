/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_RING_BUFFER_H
#define APP_RING_BUFFER_H

/**
 * @file app_ring_buffer.h
 * @brief Thread-safe ring buffer for in-memory PCM audio storage
 *
 * Replaces temporary WAV file I/O for continuous transcription mode.
 * Audio captured from ALSA is written directly into this ring buffer
 * in main memory, eliminating filesystem latency.
 *
 * Memory consumption:
 *   16kHz mono 16-bit = 32 KB/second
 *   30-second buffer  = ~960 KB
 *   60-second buffer  = ~1.92 MB
 *   5-minute buffer   = ~9.6 MB
 *
 * The buffer operates as a circular (ring) buffer: when the write
 * pointer reaches the end, it wraps to the beginning, overwriting
 * the oldest data. This allows indefinite recording with bounded
 * memory usage.
 *
 * Thread-safety:
 *   - Writer: Audio capture thread (single producer)
 *   - Reader: Transcription thread (single consumer, only after
 *     capture thread stops)
 *   - Protected by pthread_mutex for correctness
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Ring Buffer Handle (Opaque)
 *---------------------------------------------------------------------------*/
typedef struct _AudioRingBuffer AudioRingBuffer;

/*---------------------------------------------------------------------------
 * Creation and Destruction
 *---------------------------------------------------------------------------*/

/**
 * Create a ring buffer with the given capacity in seconds.
 *
 * @param duration_seconds Maximum audio duration to retain (e.g., 300 = 5 min).
 * @param sample_rate      Sample rate in Hz (must be 16000 for whisper.cpp).
 * @return Valid AudioRingBuffer* on success, NULL on failure.
 */
AudioRingBuffer *ring_buffer_create(int duration_seconds, uint32_t sample_rate);

/**
 * Destroy a ring buffer and free all resources.
 *
 * @param rb Pointer to a valid AudioRingBuffer. Must not be NULL.
 */
void ring_buffer_destroy(AudioRingBuffer *rb);

/*---------------------------------------------------------------------------
 * Write Operations (called from capture thread)
 *---------------------------------------------------------------------------*/

/**
 * Write PCM samples into the ring buffer.
 *
 * If the buffer is full, the oldest data is overwritten (circular behavior).
 *
 * @param rb       Ring buffer instance.
 * @param samples  Pointer to int16_t PCM samples.
 * @param count    Number of samples to write.
 * @return true on success, false on error.
 */
bool ring_buffer_write(AudioRingBuffer *rb, const int16_t *samples, size_t count);

/*---------------------------------------------------------------------------
 * Read / Extraction Operations (called after capture stops)
 *---------------------------------------------------------------------------*/

/**
 * Get the total number of samples currently available in the buffer.
 *
 * @param rb Ring buffer instance.
 * @return Number of int16_t samples available for reading.
 */
size_t ring_buffer_available(const AudioRingBuffer *rb);

/**
 * Get the total capacity of the buffer in samples.
 *
 * @param rb Ring buffer instance.
 * @return Maximum number of int16_t samples the buffer can hold.
 */
size_t ring_buffer_capacity(const AudioRingBuffer *rb);

/**
 * Extract all available samples from the ring buffer into a contiguous array.
 *
 * The returned buffer is allocated with g_malloc() and must be freed by the
 * caller using g_free(). The data is ordered from oldest to newest sample.
 *
 * After extraction, the ring buffer is emptied (write/read pointers reset).
 *
 * @param rb            Ring buffer instance.
 * @param out_samples   Output: pointer to allocated int16_t array (set by caller to NULL).
 * @return Number of samples extracted, or 0 on error.
 */
size_t ring_buffer_extract_all(AudioRingBuffer *rb, int16_t **out_samples);

/*---------------------------------------------------------------------------
 * Range Read Operations (non-consuming, for silence scanner)
 *---------------------------------------------------------------------------*/

/**
 * Read a range of samples from the ring buffer without consuming them.
 *
 * offset is relative to the oldest sample (0 = oldest). The data is copied
 * into the caller-provided buffer. Does NOT modify the ring buffer state.
 *
 * @param rb       Ring buffer instance.
 * @param offset   Offset from oldest sample (0-based).
 * @param out      Caller-allocated output buffer.
 * @param count    Number of samples to read.
 * @return Number of samples actually read (may be less than count if offset+count exceeds available data).
 */
size_t ring_buffer_read_range(const AudioRingBuffer *rb,
                              size_t offset,
                              int16_t *out,
                              size_t count);

/**
 * Reset the ring buffer, discarding all stored data.
 * Useful when starting a new recording session.
 *
 * @param rb Ring buffer instance.
 */
void ring_buffer_reset(AudioRingBuffer *rb);

/*---------------------------------------------------------------------------
 * Diagnostic
 *---------------------------------------------------------------------------*/

/**
 * Get the fill level as a fraction of capacity (0.0 to 1.0).
 *
 * @param rb Ring buffer instance.
 * @return Fill ratio, or 0.0 if NULL.
 */
double ring_buffer_fill_ratio(const AudioRingBuffer *rb);

#ifdef __cplusplus
}
#endif

#endif /* APP_RING_BUFFER_H */
