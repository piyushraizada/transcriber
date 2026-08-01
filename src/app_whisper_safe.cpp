/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/**
 * @file app_whisper_safe.cpp
 * @brief Exception-safe C wrappers around whisper.cpp API calls
 *
 * whisper.cpp (C++) can throw std::runtime_error and std::bad_alloc during
 * GPU model loading, CUDA buffer allocation, or context destruction. When
 * called from C code, these exceptions propagate across the extern "C"
 * boundary and cause abort() / SIGABRT crash.
 *
 * This file provides try/catch wrappers for every whisper.cpp function that
 * our C code calls, converting exceptions into safe return values:
 *   - NULL for pointer-returning functions (init)
 *   - 0/-1 for integer-returning functions (n_vocab, lang_id, segments)
 *   - -1 for error-code functions (whisper_full)
 *   - no-op for void functions (free)
 *
 * Additionally, safe_whisper_init_from_file_with_params installs a temporary
 * SIGABRT handler that longjmps out of whisper_init when GGML triggers an
 * assertion (e.g., CUDA OOM during buffer allocation). This allows the caller's
 * multi-GPU fallback loop to continue trying other devices instead of crashing.
 */

#include "app_whisper_safe.h"

#include <whisper.h>

#include <exception>
#include <stdexcept>
#include <cstdio>
#include <csignal>
#include <csetjmp>

namespace {

void log_exception(const char *func_name, const std::exception &e) {
    fprintf(stderr, "[whisper-safe] %s: caught exception: %s\n", func_name, e.what());
}

void log_unknown_exception(const char *func_name) {
    fprintf(stderr, "[whisper-safe] %s: caught unknown exception\n", func_name);
}

/* SIGABRT handler for GGML assertion interception.
 * When whisper_init triggers a GGML_ASSERT (e.g., CUDA OOM), ggml calls abort()
 * which sends SIGABRT. This handler longjmps back to the setjmp point in
 * safe_whisper_init_from_file_with_params, allowing graceful fallback. */

} // anonymous namespace

extern "C" {

static jmp_buf g_abort_jump_buffer;
static volatile sig_atomic_t g_abort_handler_installed = 0;

static void sigabrt_handler(int /*signum*/) {
    fprintf(stderr, "[whisper-safe] Caught SIGABRT during model load — falling back\n");
    longjmp(g_abort_jump_buffer, 1);
}

struct whisper_context *safe_whisper_init_from_file_with_params(
        const char *path_model, struct whisper_context_params params) {
    /* Install SIGABRT handler to catch GGML assertions (e.g., CUDA OOM).
     * ggml calls abort() on assertion failure instead of returning an error.
     * Our handler longjmps out, allowing the caller's GPU fallback loop to continue. */
    struct sigaction old_sa;
    struct sigaction sa;
    sa.sa_handler = sigabrt_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGABRT, &sa, &old_sa) != 0) {
        fprintf(stderr, "[whisper-safe] Failed to install SIGABRT handler\n");
    }
    g_abort_handler_installed = 1;

    int jumped = setjmp(g_abort_jump_buffer);
    if (jumped) {
        /* Returned from longjmp after GGML abort */
        g_abort_handler_installed = 0;
        sigaction(SIGABRT, &old_sa, NULL);
        return nullptr;
    }

    try {
        struct whisper_context *result = whisper_init_from_file_with_params(path_model, params);
        g_abort_handler_installed = 0;
        sigaction(SIGABRT, &old_sa, NULL);
        return result;
    } catch (const std::bad_alloc &e) {
        log_exception("whisper_init_from_file_with_params", e);
        g_abort_handler_installed = 0;
        sigaction(SIGABRT, &old_sa, NULL);
        return nullptr;
    } catch (const std::runtime_error &e) {
        log_exception("whisper_init_from_file_with_params", e);
        g_abort_handler_installed = 0;
        sigaction(SIGABRT, &old_sa, NULL);
        return nullptr;
    } catch (const std::exception &e) {
        log_exception("whisper_init_from_file_with_params", e);
        g_abort_handler_installed = 0;
        sigaction(SIGABRT, &old_sa, NULL);
        return nullptr;
    } catch (...) {
        log_unknown_exception("whisper_init_from_file_with_params");
        g_abort_handler_installed = 0;
        sigaction(SIGABRT, &old_sa, NULL);
        return nullptr;
    }
}

void safe_whisper_free(struct whisper_context *ctx) {
    if (!ctx) return;
    try {
        whisper_free(ctx);
    } catch (const std::exception &e) {
        log_exception("whisper_free", e);
    } catch (...) {
        log_unknown_exception("whisper_free");
    }
}

int safe_whisper_full(struct whisper_context *ctx,
                      struct whisper_full_params params,
                      const float *samples, int n_samples) {
    try {
        return whisper_full(ctx, params, samples, n_samples);
    } catch (const std::bad_alloc &e) {
        log_exception("whisper_full", e);
        return -100;  // Distinct sentinel: GPU OOM / C++ exception (avoids collision with native -1..-5)
    } catch (const std::runtime_error &e) {
        log_exception("whisper_full", e);
        return -100;
    } catch (const std::exception &e) {
        log_exception("whisper_full", e);
        return -100;
    } catch (...) {
        log_unknown_exception("whisper_full");
        return -100;
    }
}

int safe_whisper_n_vocab(struct whisper_context *ctx) {
    try {
        return whisper_n_vocab(ctx);
    } catch (const std::exception &e) {
        log_exception("whisper_n_vocab", e);
        return 0;
    } catch (...) {
        log_unknown_exception("whisper_n_vocab");
        return 0;
    }
}

int safe_whisper_lang_id(const char *lang) {
    try {
        return whisper_lang_id(lang);
    } catch (const std::exception &e) {
        log_exception("whisper_lang_id", e);
        return -1;
    } catch (...) {
        log_unknown_exception("whisper_lang_id");
        return -1;
    }
}

int safe_whisper_full_n_segments(struct whisper_context *ctx) {
    try {
        return whisper_full_n_segments(ctx);
    } catch (const std::exception &e) {
        log_exception("whisper_full_n_segments", e);
        return 0;
    } catch (...) {
        log_unknown_exception("whisper_full_n_segments");
        return 0;
    }
}

const char *safe_whisper_full_get_segment_text(struct whisper_context *ctx, int i) {
    try {
        return whisper_full_get_segment_text(ctx, i);
    } catch (const std::exception &e) {
        log_exception("whisper_full_get_segment_text", e);
        return nullptr;
    } catch (...) {
        log_unknown_exception("whisper_full_get_segment_text");
        return nullptr;
    }
}

} // extern "C"
