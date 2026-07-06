/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#include "app_ring_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <glib.h>

struct _AudioRingBuffer {
    int16_t *buffer;          ///< Circular buffer storage
    size_t capacity;          ///< Maximum number of samples (set at creation, never modified)
    size_t write_pos;         ///< Next write position (0 to capacity-1)
    size_t count;             ///< Number of valid samples currently stored
    uint32_t sample_rate;     ///< Sample rate in Hz (set at creation, never modified)
    pthread_mutex_t mutex;    ///< Protects write_pos and count fields
};

AudioRingBuffer *ring_buffer_create(int duration_seconds, uint32_t sample_rate)
{
    if (duration_seconds <= 0 || sample_rate <= 0) {
        return NULL;
    }

    size_t capacity = (size_t)duration_seconds * (size_t)sample_rate;
    if (capacity == 0) {
        return NULL;
    }

    AudioRingBuffer *rb = g_malloc0(sizeof(AudioRingBuffer));
    if (!rb) {
        return NULL;
    }

    rb->buffer = g_malloc0(capacity * sizeof(int16_t));
    if (!rb->buffer) {
        g_free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->write_pos = 0;
    rb->count = 0;
    rb->sample_rate = sample_rate;
    if (pthread_mutex_init(&rb->mutex, NULL) != 0) {
        g_free(rb->buffer);
        g_free(rb);
        return NULL;
    }

    g_log("app-ringbuffer", G_LOG_LEVEL_MESSAGE,
          "[rb] Created: %d seconds, %zu samples, %.1f MB\n",
          duration_seconds, capacity,
          (double)(capacity * sizeof(int16_t)) / (1024.0 * 1024.0));

    return rb;
}

void ring_buffer_destroy(AudioRingBuffer *rb)
{
    if (!rb) return;
    pthread_mutex_destroy(&rb->mutex);
    g_free(rb->buffer);
    g_free(rb);
}

/* Bulk write: copy samples into ring buffer in two memcpy calls
   to handle wrap-around, avoiding per-sample loop overhead. */
static void ring_buffer_write_bulk(AudioRingBuffer *rb, const int16_t *samples, size_t count)
{
    size_t first_chunk = rb->capacity - rb->write_pos;
    if (first_chunk > count) {
        first_chunk = count;
    }
    memcpy(rb->buffer + rb->write_pos, samples, first_chunk * sizeof(int16_t));

    size_t second_chunk = count - first_chunk;
    if (second_chunk > 0) {
        memcpy(rb->buffer, samples + first_chunk, second_chunk * sizeof(int16_t));
    }

    rb->write_pos = (rb->write_pos + count) % rb->capacity;
    if (rb->count < rb->capacity) {
        size_t space = rb->capacity - rb->count;
        rb->count += (count < space) ? count : space;
    }
}

bool ring_buffer_write(AudioRingBuffer *rb, const int16_t *samples, size_t count)
{
    if (!rb || !samples || count == 0) return false;

    pthread_mutex_lock(&rb->mutex);
    ring_buffer_write_bulk(rb, samples, count);
    pthread_mutex_unlock(&rb->mutex);
    return true;
}

size_t ring_buffer_available(const AudioRingBuffer *rb)
{
    if (!rb) return 0;
    /* Use const-correct cast: pthread_mutex_lock takes non-const but only
      * performs synchronization (futex operations), not logical modification.
      * This is the standard pattern for const-correct mutex access in C.
      * An alternative would be to use a separate pthread_mutex_t for
      * read-only access, but that adds complexity without benefit. */
    AudioRingBuffer *nonconst = (AudioRingBuffer *)(const void *)rb;
    size_t c;
    pthread_mutex_lock(&nonconst->mutex);
    c = nonconst->count;
    pthread_mutex_unlock(&nonconst->mutex);
    return c;
}

size_t ring_buffer_capacity(const AudioRingBuffer *rb)
{
    if (!rb) return 0;
    /* Capacity is set at creation time and never modified.
      * No mutex needed for reading capacity. */
    return rb->capacity;
}

size_t ring_buffer_extract_all(AudioRingBuffer *rb, int16_t **out_samples)
{
    if (!rb || !out_samples) return 0;

    *out_samples = NULL;

    pthread_mutex_lock(&rb->mutex);

    if (rb->count == 0) {
        pthread_mutex_unlock(&rb->mutex);
        return 0;
    }

    int16_t *out = g_malloc(rb->count * sizeof(int16_t));
    if (!out) {
        pthread_mutex_unlock(&rb->mutex);
        return 0;
    }

    // Calculate the read start position
    // If buffer is full, write_pos points to oldest data
    // If buffer is not full, oldest data is at (write_pos - count) mod capacity
    size_t read_pos;
    if (rb->count >= rb->capacity) {
        read_pos = rb->write_pos;  // Full: oldest = write_pos
    } else {
        read_pos = (rb->write_pos + rb->capacity - rb->count) % rb->capacity;
    }

    // Copy in two chunks to handle wrap-around
    size_t first_chunk = rb->capacity - read_pos;
    if (first_chunk > rb->count) {
        first_chunk = rb->count;
    }
    memcpy(out, rb->buffer + read_pos, first_chunk * sizeof(int16_t));

    size_t second_chunk = rb->count - first_chunk;
    if (second_chunk > 0) {
        memcpy(out + first_chunk, rb->buffer, second_chunk * sizeof(int16_t));
    }

    size_t extracted = rb->count;

    // Reset buffer
    rb->write_pos = 0;
    rb->count = 0;

    pthread_mutex_unlock(&rb->mutex);

    *out_samples = out;
    return extracted;
}

double ring_buffer_fill_ratio(AudioRingBuffer *rb)
{
    if (!rb || rb->capacity == 0) return 0.0;
    /* Capacity is immutable after creation; count is protected by mutex. */
    size_t c;
    pthread_mutex_lock(&rb->mutex);
    c = rb->count;
    pthread_mutex_unlock(&rb->mutex);
    return (double)c / (double)rb->capacity;
}

size_t ring_buffer_read_range(AudioRingBuffer *rb,
                              size_t offset,
                              int16_t *out,
                              size_t count)
{
    if (!rb || !out || count == 0) return 0;

    /* Single critical section: read all state and perform the copy atomically
     * with respect to the writer. This eliminates the TOCTOU vulnerability
     * where the writer could advance write_pos/count between two separate
     * mutex acquisitions, causing stale bounds checks or out-of-bounds reads. */
    pthread_mutex_lock(&rb->mutex);

    size_t available = rb->count;
    if (offset >= available) {
        pthread_mutex_unlock(&rb->mutex);
        return 0;
    }
    /* Use subtraction form to avoid size_t wrap-around in offset + count */
    if (count > available - offset) {
        count = available - offset;
    }

    /* Calculate the physical read position for the oldest sample */
    size_t oldest_pos;
    if (rb->count >= rb->capacity) {
        oldest_pos = rb->write_pos;  /* Full: oldest = write_pos */
    } else {
        oldest_pos = (rb->write_pos + rb->capacity - rb->count) % rb->capacity;
    }

    /* Apply offset to get actual read position */
    size_t read_pos = (oldest_pos + offset) % rb->capacity;

    /* Copy in two chunks to handle wrap-around */
    size_t first_chunk = rb->capacity - read_pos;
    if (first_chunk > count) {
        first_chunk = count;
    }
    memcpy(out, rb->buffer + read_pos, first_chunk * sizeof(int16_t));

    size_t second_chunk = count - first_chunk;
    if (second_chunk > 0) {
        memcpy(out + first_chunk, rb->buffer, second_chunk * sizeof(int16_t));
    }

    pthread_mutex_unlock(&rb->mutex);
    return count;
}

void ring_buffer_reset(AudioRingBuffer *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->mutex);
    rb->write_pos = 0;
    rb->count = 0;
    pthread_mutex_unlock(&rb->mutex);
}
