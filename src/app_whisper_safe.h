/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_WHISPER_SAFE_H
#define APP_WHISPER_SAFE_H

/**
 * @file app_whisper_safe.h
 * @brief Exception-safe C wrappers around whisper.cpp API calls
 *
 * whisper.cpp is a C++ library that can throw std::runtime_error and
 * std::bad_alloc internally (e.g., during CUDA OOM or GPU buffer allocation).
 * Calling these functions directly from C code results in abort() / crash
 * because C cannot catch C++ exceptions.
 *
 * This module provides extern "C" wrapper functions that wrap every
 * whisper.cpp call in try/catch, converting exceptions into safe return
 * values (NULL pointers or error codes) that C code can handle gracefully.
 */

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque forward declaration — matches whisper.h struct whisper_context */
struct whisper_context;
struct whisper_context_params;
struct whisper_full_params;

/**
 * Safe wrapper for whisper_init_from_file_with_params.
 * Catches std::bad_alloc, std::runtime_error, and any other exception.
 * Returns NULL on exception instead of crashing.
 */
struct whisper_context *safe_whisper_init_from_file_with_params(
    const char *path_model, struct whisper_context_params params);

/**
 * Safe wrapper for whisper_free.
 * Catches exceptions during context destruction (GPU cleanup).
 */
void safe_whisper_free(struct whisper_context *ctx);

/**
 * Safe wrapper for whisper_full.
 * Returns -1 on exception instead of crashing mid-transcription.
 */
int safe_whisper_full(struct whisper_context *ctx,
                      struct whisper_full_params params,
                      const float *samples, int n_samples);

/**
 * Safe wrapper for whisper_n_vocab.
 * Returns 0 on exception (same as invalid vocab indicator).
 */
int safe_whisper_n_vocab(struct whisper_context *ctx);

/**
 * Safe wrapper for whisper_lang_id.
 * Returns -1 on exception (same as "language not found").
 */
int safe_whisper_lang_id(const char *lang);

/**
 * Safe wrapper for whisper_full_n_segments.
 * Returns 0 on exception.
 */
int safe_whisper_full_n_segments(struct whisper_context *ctx);

/**
 * Safe wrapper for whisper_full_get_segment_text.
 * Returns NULL on exception.
 */
const char *safe_whisper_full_get_segment_text(struct whisper_context *ctx, int i);

#ifdef __cplusplus
}
#endif

#endif /* APP_WHISPER_SAFE_H */
