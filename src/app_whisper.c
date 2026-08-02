/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * app_whisper.c — Local Whisper transcription via whisper.cpp
 *
 * Uses the whisper.cpp library (ggml-org/whisper.cpp) for offline,
 * local speech-to-text transcription. No network connection required.
 *
 * The API surface matches the original external API implementation,
 * so the rest of the application remains unchanged.
 *
 * Key data structures:
 *   - WhisperClient: Opaque handle containing whisper context, model path,
 *     language config, GPU settings, mutex, cancellation flag, and a
 *     reference count for the loaded context to prevent use-after-free
 *     when transcriptions are in-flight during model reload/unload.
 *
 * Threading:
 *   - All public APIs are thread-safe via pthread_mutex_t
 *   - Context reference counting (atomic) ensures that in-flight
 *     transcriptions keep the whisper_context alive even if another
 *     thread unloads the model
 *   - A 'destroying' atomic flag blocks new transcriptions during cleanup
 *   - Cancellation uses an atomic_int flag checked by whisper.cpp's
 *     abort_callback during whisper_full()
 *
 * Lifecycle:
 *   1. whisper_client_create() — allocate and init client
 *   2. whisper_client_set_model_path() — configure model location
 *   3. whisper_client_load_model() (optional) — pre-load model
 *   4. whisper_transcribe() / whisper_transcribe_samples() — transcribe
 *   5. whisper_client_destroy() — signal cancellation, wait for refs to
 *     drop, free context and client
 */

#define _POSIX_C_SOURCE 200809L

#include "app_whisper.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

// whisper.cpp header
#include "whisper.h"

// Exception-safe C wrappers around whisper.cpp (implemented in app_whisper_safe.cpp)
#include "app_whisper_safe.h"

#include "app_gpu.h"

#ifdef HAVE_CUDA
#include <cuda_runtime_api.h>
#endif

/* ===================================================================
 * Constants and defaults
 * =================================================================== */

#include <limits.h>

#define MAX_PATH_LEN  PATH_MAX
#define DEFAULT_THREADS 0  // 0 = use all available threads

/* Maximum WAV data chunk size: 30 minutes of 16kHz/16-bit/mono audio (~57.6 MB) */
#define MAX_WAV_DATA_SIZE (30UL * 60UL * 16000UL * 2UL)

/* GGML magic: 0x67676d6c ("ggml" little-endian) */
#define GGML_MAGIC 0x67676d6cu
/* GGUF magic: 0x46554747 ("GGUF" as uint32 little-endian) */
#define GGUF_MAGIC 0x46554747u

/* Default fallback thread count when sysconf fails */
#define DEFAULT_FALLBACK_THREADS 4

/* Default model search directories (in order of preference).
 * SYSTEM_MODEL_DIR is defined by CMake at configure time, allowing package
 * maintainers to override the system-wide model directory via
 * -DSYSTEM_MODEL_DIR=/custom/path. Falls back to /usr/share/transcriber/models */
#ifndef SYSTEM_MODEL_DIR
#define SYSTEM_MODEL_DIR "/usr/share/transcriber/models"
#endif

static const char *DEFAULT_MODEL_DIRS[] = {
    "~/.cache/whisper",
    SYSTEM_MODEL_DIR,
    NULL
};

/* ===================================================================
 * Internal WhisperClient struct
 * =================================================================== */
struct _WhisperClient {
    struct whisper_context *ctx;       // whisper.cpp context (loaded model)
    char model_path[MAX_PATH_LEN];
    char language[16];                 // Language code: "auto", "en", "fr", etc.
    int n_threads;
    int gpu_index;                     // -3 = auto by free mem, -2 = CPU-only, >=0 = specific GPU
    int active_gpu_device;             // Actual CUDA device index after model load (-1 = CPU)
    bool flash_attention;              // Enable flash attention for reduced VRAM usage
    char error_message[256];
    int error_code;
    pthread_mutex_t mutex;
    atomic_int cancel_requested;
    atomic_bool model_loaded;          // atomic for portable cross-thread visibility
    atomic_bool model_loading;         // true while model load is in progress
    atomic_uint ctx_refcount;          // reference count for in-flight transcriptions
    atomic_bool destroying;            // true during whisper_client_destroy cleanup
    char gpu_fallback_message[512];    // Human-readable GPU fallback info (empty if no fallback)
};

/* ===================================================================
 * Helper: context reference counting
 * =================================================================== */
static void ctx_ref(WhisperClient *client) {
    if (client) {
        atomic_fetch_add(&client->ctx_refcount, 1);
    }
}

/* Decrement in-flight transcription count. Does NOT free the context —
 * that is handled by free_current_context() and whisper_client_destroy(). */
static void ctx_unref(WhisperClient *client) {
    if (client) {
        atomic_fetch_sub(&client->ctx_refcount, 1);
    }
}

/* ===================================================================
 * Cancellation callback for whisper.cpp
 * Called periodically during whisper_full() to check if transcription
 * should be aborted. Returns true to abort, false to continue.
 * =================================================================== */
static bool whisper_abort_callback(void *data) {
    atomic_int *cancel_flag = (atomic_int *)data;
    return cancel_flag && atomic_load(cancel_flag);
}

/* ===================================================================
 * Public: resolve model path with tilde expansion and directory search
 * =================================================================== */
/* Distinct return codes for path resolution:
 *   0  = path resolved and file exists
 *  -1  = path resolved but file does not exist (valid path, missing file)
 *  -2  = path resolution failed (invalid input, no HOME, etc.) */
int whisper_resolve_model_path(const char *input, char *output, size_t out_size) {
    if (!input || !output || out_size == 0) return -2;

    char resolved[MAX_PATH_LEN];

    // Check if path starts with ~
    if (input[0] == '~') {
        if (input[1] == '/' || input[1] == '\0') {
            const char *home = getenv("HOME");
            if (!home) return -2;
            snprintf(resolved, sizeof(resolved), "%s%s", home, input + 1);
        } else {
            // ~username style - not supported, return resolution failure
            return -2;
        }
    } else {
        snprintf(resolved, sizeof(resolved), "%s", input);
    }

    // Check if the resolved path exists directly
    struct stat st;
    if (stat(resolved, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(output, out_size, "%s", resolved);
        return 0;
    }

    // Simplified -- only search default dirs for bare filenames (no '/')
    if (strrchr(input, '/') == NULL) {
            const char *basename = input[0] == '~' ? input + 2 : input;
            for (size_t i = 0; DEFAULT_MODEL_DIRS[i] != NULL; i++) {
                char candidate[MAX_PATH_LEN];
                const char *dir = DEFAULT_MODEL_DIRS[i];

                // Expand tilde in directory path
                char expanded_dir[MAX_PATH_LEN];
                if (dir[0] == '~') {
                    const char *home = getenv("HOME");
                    if (!home) continue;
                    snprintf(expanded_dir, sizeof(expanded_dir), "%s%s", home, dir + 1);
                } else {
                    snprintf(expanded_dir, sizeof(expanded_dir), "%s", dir);
                }

                /* Use snprintf with explicit size to avoid truncation warning */
                int n = snprintf(candidate, sizeof(candidate), "%s/%s", expanded_dir, basename);
                if (n < 0 || (size_t)n >= sizeof(candidate)) {
                    /* Path too long, skip this candidate */
                    continue;
                }
                if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
                    snprintf(output, out_size, "%s", candidate);
                    return 0;
                }
            }
    }

    // Use the resolved path as-is (may not exist yet)
    snprintf(output, out_size, "%s", resolved);
    return -1;  // Path resolved but file does not exist
}

/* ===================================================================
 * Helper: set error message (must be called with mutex held)
 * =================================================================== */
static void set_error(WhisperClient *client, int code, const char *msg) {
    if (!client) return;
    client->error_code = code;
    if (msg) {
        snprintf(client->error_message, sizeof(client->error_message), "%s", msg);
    } else {
        client->error_message[0] = '\0';
    }
}

/* ===================================================================
 * Helper: setup transcription parameters (deduplicated)
 * =================================================================== */
static void setup_transcription_params(int n_threads_cfg, const char *lang,
                                        struct whisper_full_params *params,
                                        atomic_int *cancel_flag) {
    *params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    int threads = n_threads_cfg > 0 ? n_threads_cfg : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (threads <= 0) {
        threads = DEFAULT_FALLBACK_THREADS;
    }
    params->n_threads = threads;
    params->print_progress = false;
    params->print_realtime = false;
    params->print_special = false;
    params->translate = false;
    params->no_timestamps = true;

    // Setup cancellation callback - whisper.cpp will call this periodically
    params->abort_callback = whisper_abort_callback;
    params->abort_callback_user_data = cancel_flag;

    // Set language: "auto" or empty means auto-detect, otherwise use specified language
    if (lang[0] == '\0' || strcmp(lang, "auto") == 0) {
        params->language = NULL;  // Auto-detect
    } else {
        params->language = lang;
    }
}

/* ===================================================================
 * Helper: extract and concatenate segment text (deduplicated)
 * =================================================================== */
static char *extract_segments_text(struct whisper_context *ctx, int n_segments) {
    if (!ctx || n_segments <= 0) return NULL;

    // Single pass to calculate total length
    size_t total_len = 0;
    for (int i = 0; i < n_segments; i++) {
        const char *text = safe_whisper_full_get_segment_text(ctx, i);
        if (text) {
            total_len += strlen(text);
        }
    }

    char *full_text = g_malloc(total_len + 1);
    if (!full_text) return NULL;

    full_text[0] = '\0';
    size_t offset = 0;
    for (int i = 0; i < n_segments; i++) {
        const char *text = safe_whisper_full_get_segment_text(ctx, i);
        if (text) {
            size_t len = strlen(text);
            memcpy(full_text + offset, text, len);
            offset += len;
        }
    }
    full_text[offset] = '\0';

    // Trim trailing whitespace/newline
    size_t len = strlen(full_text);
    while (len > 0 && (full_text[len - 1] == '\n' || full_text[len - 1] == ' ')) {
        full_text[--len] = '\0';
    }

    return full_text;
}

/* ===================================================================
 * Public: Check if CUDA GPU acceleration is available
 * =================================================================== */
bool whisper_gpu_available(void) {
#ifdef HAVE_CUDA
    return true;
#else
    return false;
#endif
}

/* ===================================================================
 * Public: Check if model is loaded
 * =================================================================== */
bool whisper_client_is_model_loaded(const WhisperClient *client) {
    if (!client) return false;
    return atomic_load(&client->model_loaded);
}

/* ===================================================================
 * Public: Check if model is currently loading
 * =================================================================== */
bool whisper_client_is_loading(const WhisperClient *client) {
    if (!client) return false;
    return atomic_load(&client->model_loading);
}

/* ===================================================================
 * Internal: load whisper model with parameters
 * =================================================================== */
static bool load_model_internal(WhisperClient *client) {
    if (!client) return false;
    if (atomic_load(&client->model_loaded)) return true;

    if (client->model_path[0] == '\0') {
        set_error(client, WHISPER_ERR_MODEL_NOT_FOUND, "No model path configured");
        return false;
    }

    // Check if model file exists
    struct stat st;
    if (stat(client->model_path, &st) != 0) {
        snprintf(client->error_message, sizeof(client->error_message),
                  "Model file not found: %.200s", client->model_path);
        client->error_code = WHISPER_ERR_MODEL_NOT_FOUND;
        return false;
    }

    // Determine GPU strategy using dynamic VRAM threshold based on model size.
    // The static 2 GB minimum was too conservative for small models (base/tiny ~700 MB).
    // Now we calculate: max(512 MiB, model_size * 1.5 + 300 MiB) to match actual needs.
    size_t min_vram = gpu_min_vram_for_model((size_t)st.st_size);

    bool try_gpu = false;
    int gpu_idx = client->gpu_index;

    if (gpu_idx == GPU_INDEX_CPU_ONLY) {
        // User explicitly requested CPU-only
        try_gpu = false;
    } else if (gpu_idx == GPU_INDEX_AUTO_MEMORY) {
        // Auto-select GPU with most free memory
        int best_gpu = -1;
        size_t free_mem = 0;
        if (gpu_select_best_by_free_memory(&best_gpu, &free_mem)) {
            if (free_mem >= min_vram) {
                try_gpu = true;
                gpu_idx = best_gpu;
            } else {
                try_gpu = false;
            }
        } else {
            try_gpu = false;
        }
    } else if (gpu_idx >= 0) {
        // User specified a specific GPU
        try_gpu = whisper_gpu_available();
        if (!try_gpu) {
            try_gpu = false;
        }
    } else {
        // Fallback: treat as auto-select by free memory
        int best_gpu = -1;
        size_t free_mem = 0;
        if (gpu_select_best_by_free_memory(&best_gpu, &free_mem)) {
            if (free_mem >= min_vram) {
                try_gpu = true;
                gpu_idx = best_gpu;
            }
        }
    }

    bool gpu_loaded = false;
    int final_gpu_idx = -1;  // Actual GPU device used (-1 = CPU)

    /* Clear any previous fallback message */
    client->gpu_fallback_message[0] = '\0';

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.flash_attn = client->flash_attention;

#ifdef HAVE_CUDA
    /* Build a sorted list of GPU devices by free memory for fallback iteration.
     * When the primary GPU fails (e.g., insufficient VRAM), we try remaining GPUs
     * in descending order of available free memory before falling back to CPU. */
    struct whisper_context *ctx = NULL;
    typedef struct { int idx; size_t free_mem; } gpu_candidate;
    /* Track failed GPUs so cudaDeviceReset() is not called on them later (crashes). */
    int failed_gpus[16] = {0};
    int n_failed = 0;
    gpu_candidate candidates[16];  // Max 16 GPUs is more than sufficient
    int n_candidates = 0;

    if (try_gpu && whisper_gpu_available()) {
        int device_count = 0;
        if (gpu_get_device_count(&device_count)) {
            for (int i = 0; i < device_count && n_candidates < 16; i++) {
                size_t free_mem = 0;
                if (gpu_get_memory_info(i, &free_mem, NULL)) {
                    char dev_name[128] = "unknown";
                    gpu_get_device_name(i, dev_name, sizeof(dev_name));
                    g_log("app-whisper", G_LOG_LEVEL_DEBUG,
                          "[whisper] GPU candidate %d (%s): %.2f GiB free (threshold: %.2f GiB)",
                          i, dev_name,
                          (double)free_mem / (1024.0 * 1024.0 * 1024.0),
                          (double)min_vram / (1024.0 * 1024.0 * 1024.0));
                    candidates[n_candidates].idx = i;
                    candidates[n_candidates].free_mem = free_mem;
                    n_candidates++;
                }
            }
        }

        /* Sort candidates by free memory descending (simple insertion sort) */
        for (int i = 1; i < n_candidates; i++) {
            gpu_candidate key = candidates[i];
            int j = i - 1;
            while (j >= 0 && candidates[j].free_mem < key.free_mem) {
                candidates[j + 1] = candidates[j];
                j--;
            }
            candidates[j + 1] = key;
        }

        /* If primary GPU was specified and not at the top of the list, move it there */
        if (gpu_idx >= 0 && n_candidates > 0) {
            int pos = -1;
            for (int i = 0; i < n_candidates; i++) {
                if (candidates[i].idx == gpu_idx) { pos = i; break; }
            }
            if (pos > 0) {
                gpu_candidate tmp = candidates[0];
                candidates[0] = candidates[pos];
                candidates[pos] = tmp;
            }
        }

        /* Iterate through GPU candidates until one succeeds.
     * Track failed GPUs so we don't call cudaDeviceReset() on them later —
     * a failed whisper.cpp load can leave the CUDA context in an undefined state,
     * and resetting it causes crashes when other code (e.g., config dialog) queries
     * device properties afterward. */
        int original_gpu_idx = gpu_idx;

        for (int ci = 0; ci < n_candidates && !ctx; ci++) {
            int attempt_idx = candidates[ci].idx;
            cparams.use_gpu = true;
            cparams.gpu_device = attempt_idx;

            char gpu_name[256];
            bool is_fallback = (attempt_idx != original_gpu_idx);

            if (gpu_get_device_name(attempt_idx, gpu_name, sizeof(gpu_name))) {
                g_log("app-whisper", G_LOG_LEVEL_MESSAGE,
                       "[whisper] %sGPU device %d: %s (%.1f GB free)",
                       is_fallback ? "Trying fallback " : "Using ",
                       attempt_idx, gpu_name,
                       (double)candidates[ci].free_mem / (1024.0 * 1024.0 * 1024.0));
            } else {
                g_log("app-whisper", G_LOG_LEVEL_MESSAGE,
                       "[whisper] %sGPU device %d (%.1f GB free)",
                       is_fallback ? "Trying fallback " : "Using ",
                       attempt_idx,
                       (double)candidates[ci].free_mem / (1024.0 * 1024.0 * 1024.0));
            }

            cudaSetDevice(attempt_idx);
            ctx = safe_whisper_init_from_file_with_params(client->model_path, cparams);

            if (ctx) {
                gpu_loaded = true;
                final_gpu_idx = attempt_idx;

                /* Build fallback message for user notification */
                if (is_fallback) {
                    char orig_name[256] = "GPU";
                    char alt_name[256] = "GPU";
                    gpu_get_device_name(original_gpu_idx, orig_name, sizeof(orig_name));
                    gpu_get_device_name(attempt_idx, alt_name, sizeof(alt_name));
                    snprintf(client->gpu_fallback_message, sizeof(client->gpu_fallback_message),
                              "The configured GPU (%s) did not have enough free VRAM. "
                              "Model loaded on %s instead.", orig_name, alt_name);
                }
                break;  // Success — stop iterating
            }

            /* This GPU failed — record it so we skip cudaDeviceReset() later */
            if (n_failed < 16) {
                failed_gpus[n_failed++] = attempt_idx;
            }
            g_log("app-whisper", G_LOG_LEVEL_WARNING,
                   "[whisper] GPU device %d failed to load model, trying next...", attempt_idx);
        }
    }
#else
    struct whisper_context *ctx = NULL;
    int n_candidates = 0;
    (void)gpu_idx;  // Suppress unused warning when CUDA not available
#endif

    /* If no GPU succeeded, fall back to CPU */
    if (!ctx) {
        cparams.use_gpu = false;
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE,
               "[whisper] Falling back to CPU for model loading");

#ifdef HAVE_CUDA
        if (n_candidates > 0) {
            /* Build a list of failed GPU names for the fallback message */
            char gpu_list[256] = "";
            int listed = 0;
            for (int ci = 0; ci < n_candidates && listed < 4; ci++) {
                char name[128];
                if (!gpu_get_device_name(candidates[ci].idx, name, sizeof(name))) {
                    snprintf(name, sizeof(name), "GPU %d", candidates[ci].idx);
                }
                if (listed > 0) strncat(gpu_list, ", ", sizeof(gpu_list) - strlen(gpu_list) - 1);
                strncat(gpu_list, name, sizeof(gpu_list) - strlen(gpu_list) - 1);
                listed++;
            }
            snprintf(client->gpu_fallback_message, sizeof(client->gpu_fallback_message),
                      "No GPU had enough free VRAM to load the model (%s). Using CPU instead.", gpu_list);
        } else if (!whisper_gpu_available()) {
            snprintf(client->gpu_fallback_message, sizeof(client->gpu_fallback_message),
                      "CUDA runtime not available. Using CPU for model loading.");
        }
#endif

        ctx = safe_whisper_init_from_file_with_params(client->model_path, cparams);
    } else if (!gpu_loaded) {
        /* GPU was never attempted (e.g., gpu_index == CPU_ONLY) */
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Using CPU (no GPU)");
    }

    if (!ctx) {
        snprintf(client->error_message, sizeof(client->error_message),
                  "Failed to load whisper model: %.200s", client->model_path);
        client->error_code = WHISPER_ERR_MODEL_LOAD_FAIL;
        return false;
    }

    /* Assign context before validation so error paths can clean up. */
    client->ctx = ctx;

    /* Post-load functional validation: verify the loaded context is usable
     * by querying model properties and checking GPU memory allocation.
     * This catches cases where whisper_init_from_file_with_params returns
     * a non-NULL pointer but the underlying GPU memory is corrupted or
     * insufficient for actual inference. */
    int n_vocab = safe_whisper_n_vocab(ctx);
    if (n_vocab <= 0) {
        g_log("app-whisper", G_LOG_LEVEL_WARNING,
               "[whisper] Post-load validation failed: invalid vocab size (%d) — model may be corrupted\n",
               n_vocab);
#ifdef HAVE_CUDA
        if (gpu_loaded && final_gpu_idx >= 0) cudaSetDevice(final_gpu_idx);
#endif
        safe_whisper_free(ctx);
        client->ctx = NULL;
        snprintf(client->error_message, sizeof(client->error_message),
                  "Model loaded but post-validation failed (vocab=%d)", n_vocab);
        client->error_code = WHISPER_ERR_MODEL_VALIDATION;
        return false;
    }

    /* Verify language IDs are accessible — a quick sanity check that the
     * model's internal data structures are properly mapped in memory.
     * Note: "auto" is NOT a valid whisper.cpp language code (it's only used as
     * a user-facing config value for auto-detection). Use "en" instead which
     * exists in every Whisper model's language table. */
    int lang_en_id = safe_whisper_lang_id("en");
    if (lang_en_id < 0) {
        g_log("app-whisper", G_LOG_LEVEL_WARNING,
               "[whisper] Post-load validation failed: cannot resolve 'en' language ID\n");
#ifdef HAVE_CUDA
        if (gpu_loaded && final_gpu_idx >= 0) cudaSetDevice(final_gpu_idx);
#endif
        safe_whisper_free(ctx);
        client->ctx = NULL;
        snprintf(client->error_message, sizeof(client->error_message),
                  "Model loaded but post-validation failed (language lookup)");
        client->error_code = WHISPER_ERR_MODEL_LANG_CHECK;
        return false;
    }

    /* Store the actual active GPU device so transcription can restore
     * the CUDA context before each call. The current device can drift
     * after gpu_release_unused_devices() and multi-threaded operations. */
    client->active_gpu_device = gpu_loaded && final_gpu_idx >= 0 ? final_gpu_idx : -1;

    /* Reset refcount for new context */
    atomic_store(&client->ctx_refcount, 0);

    /* Model passed functional validation — mark as loaded. */
    atomic_store(&client->model_loaded, true);

    g_log("app-whisper", G_LOG_LEVEL_MESSAGE,
           "[whisper] Model validated successfully (vocab=%d, gpu=%s)\n",
           n_vocab, gpu_loaded ? "yes" : "no");

    /* Release CUDA contexts on GPUs not used by the loaded model.
      * whisper_init_from_file_with_params() enumerates all CUDA devices
      * internally, creating ~256 MiB contexts on each. Clean them up.
      * Skip devices that had failed load attempts — their context is corrupted. */
#ifdef HAVE_CUDA
    if (gpu_loaded && final_gpu_idx >= 0) {
        gpu_release_unused_devices_skip(final_gpu_idx, failed_gpus, n_failed);
    }
#endif

    return true;
}

/* ===================================================================
 * Internal: Helper to free current context under mutex with GPU safety
 * =================================================================== */
static void free_current_context(WhisperClient *client) {
    if (!client || !client->ctx) return;

#ifdef HAVE_CUDA
    int saved_gpu = client->active_gpu_device;
    if (saved_gpu >= 0) {
        pthread_mutex_unlock(&client->mutex);
        cudaSetDevice(saved_gpu);
        pthread_mutex_lock(&client->mutex);
    }
#endif
    safe_whisper_free(client->ctx);
    client->ctx = NULL;
}

/* ===================================================================
 * Internal: load model (wrapper with loading state tracking)
 * =================================================================== */
static bool load_model(WhisperClient *client) {
    if (!client || atomic_load(&client->model_loaded)) return true;

    // Mark as loading (atomic for cross-thread visibility)
    atomic_store(&client->model_loading, true);

    bool result = load_model_internal(client);

    // Clear loading flag
    atomic_store(&client->model_loading, false);

    return result;
}

/* ===================================================================
 * Public: Load the model synchronously (blocking call)
 *
 * @param client      WhisperClient instance
 * @param gpu_mode    GPU mode string: "auto", "cpu", or "gpu:N"
 *                    If NULL, defaults to "auto" (GPU_INDEX_AUTO_MEMORY)
 * @param flash_attention  Enable flash attention for reduced VRAM usage
 * =================================================================== */
bool whisper_client_load_model(WhisperClient *client, const char *gpu_mode, bool flash_attention) {
    if (!client) return false;

    pthread_mutex_lock(&client->mutex);

    // Parse GPU mode string to get internal index
    int gpu_index = GPU_INDEX_AUTO_MEMORY;  // Default to auto
    if (gpu_mode) {
        gpu_mode_parse(gpu_mode, &gpu_index);
    }

    // If already loaded with same GPU config and flash attention setting, skip
    if (atomic_load(&client->model_loaded) && client->gpu_index == gpu_index
            && client->flash_attention == flash_attention) {
        pthread_mutex_unlock(&client->mutex);
        return true;
    }

    // If already loaded but GPU config or flash attention differs, unload first
    if (atomic_load(&client->model_loaded)) {
        free_current_context(client);
        atomic_store(&client->model_loaded, false);
    }

    // Set the GPU index and flash attention preference
    client->gpu_index = gpu_index;
    client->flash_attention = flash_attention;

    // Load the model (atomic for cross-thread visibility)
    atomic_store(&client->model_loading, true);
    bool result = load_model_internal(client);
    atomic_store(&client->model_loading, false);

    pthread_mutex_unlock(&client->mutex);

    return result;
}

/* ===================================================================
 * Helper: read WAV file and extract PCM samples
 * =================================================================== */
/* Little-endian byte extraction helpers for portable WAV parsing */
static uint16_t le16dec(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32dec(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool read_wav_samples(const char *path, float **samples_out, int *n_samples_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Failed to open WAV file: %s\n", path);
        return false;
    }

    // Parse WAV header
    char riff[4], wave[4];
    uint16_t audio_format = 0, channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0;
    uint32_t data_size = 0;

    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) != 0) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Invalid WAV: not a RIFF file\n");
        fclose(f);
        return false;
    }
    // Skip riff_size (not needed for parsing)
    if (fseek(f, 4, SEEK_CUR) != 0) { fclose(f); return false; }
    if (fread(wave, 1, 4, f) != 4 || memcmp(wave, "WAVE", 4) != 0) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Invalid WAV: missing WAVE header\n");
        fclose(f);
        return false;
    }

    // Parse chunks to find 'fmt ' and 'data'
    while (data_size == 0) {
        char chunk_id[4];
        unsigned char buf4[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(buf4, 1, 4, f) != 4) break;
        chunk_size = le32dec(buf4);

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size >= 16) {
                unsigned char buf2[2];
                if (fread(buf2, 1, 2, f) == 2) audio_format = le16dec(buf2);
                if (fread(buf2, 1, 2, f) == 2) channels = le16dec(buf2);
                if (fread(buf4, 1, 4, f) == 4) sample_rate = le32dec(buf4);
                // Skip byte_rate (4 bytes)
                if (fseek(f, 4, SEEK_CUR) != 0) break;
                if (fread(buf2, 1, 2, f) == 2) { /* block_align, not needed */ }
                if (fread(buf2, 1, 2, f) == 2) bits_per_sample = le16dec(buf2);
                if (chunk_size > 16) {
                    fseek(f, chunk_size - 16, SEEK_CUR);
                }
            }
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            break;
        } else {
            fseek(f, (long)chunk_size, SEEK_CUR);
        }
    }

    if (data_size == 0) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Invalid WAV: no data chunk found\n");
        fclose(f);
        return false;
    }

    // Validate format - we expect 16-bit PCM, mono, 16kHz
    if (audio_format != 1) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Warning: non-PCM format (%d), attempting anyway\n", audio_format);
    }
    if (channels != 1) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Warning: %d channels, expected 1 (mono)\n", channels);
    }
    if (bits_per_sample != 16) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Warning: %d bits/sample, expected 16\n", bits_per_sample);
    }
    if (sample_rate != 16000) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Warning: %d Hz sample rate, expected 16000\n", sample_rate);
    }

    // Validate data_size to prevent excessive memory allocation from malicious WAV files.
    if ((uint64_t)data_size > MAX_WAV_DATA_SIZE) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE,
               "[whisper] WAV data chunk too large: %u bytes (max %lu) — aborting\n",
               data_size, (unsigned long)MAX_WAV_DATA_SIZE);
        fclose(f);
        return false;
    }

    // Read raw PCM data as bytes, then convert to float32
    int bytes_per_sample_actual = bits_per_sample / 8;
    if (bytes_per_sample_actual <= 0) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Invalid bits_per_sample: %d\n", bits_per_sample);
        fclose(f);
        return false;
    }

    int total_samples_in_file = data_size / bytes_per_sample_actual;
    // Handle multi-channel by only taking first channel samples
    int n_samples = (channels > 1) ? total_samples_in_file / channels : total_samples_in_file;

    if (n_samples <= 0) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] WAV file contains no valid samples\n");
        fclose(f);
        return false;
    }

    float *samples = g_new0(float, n_samples);
    if (!samples) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Memory allocation failed for samples\n");
        fclose(f);
        return false;
    }

    // Read raw PCM data as bytes to handle any bit depth correctly
    uint8_t *raw_pcm = g_malloc(data_size);
    if (!raw_pcm) {
        g_free(samples);
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Memory allocation failed for PCM buffer\n");
        fclose(f);
        return false;
    }

    size_t bytes_read = fread(raw_pcm, 1, data_size, f);
    fclose(f);

    if (bytes_read < data_size) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Warning: read %zu bytes, expected %u\n",
                bytes_read, data_size);
        n_samples = (int)(bytes_read / (bytes_per_sample_actual * channels));
    }

    // Convert to float32 normalized to [-1, 1]
    for (int i = 0; i < n_samples; i++) {
        int byte_offset = i * channels * bytes_per_sample_actual;
        if (bits_per_sample == 16) {
            int16_t val = (int16_t)(raw_pcm[byte_offset] | (raw_pcm[byte_offset + 1] << 8));
            samples[i] = (float)val / 32768.0f;
        } else if (bits_per_sample == 8) {
            samples[i] = ((float)raw_pcm[byte_offset] - 128.0f) / 128.0f;
        } else {
            // Fallback: treat as 16-bit
            int16_t val = (int16_t)(raw_pcm[byte_offset] | (raw_pcm[byte_offset + 1] << 8));
            samples[i] = (float)val / 32768.0f;
        }
    }

    g_free(raw_pcm);
    *samples_out = samples;
    *n_samples_out = n_samples;

    return true;
}

/* ===================================================================
 * Public API: whisper_response_free
 * =================================================================== */
void whisper_response_free(WhisperResponse *response) {
    if (!response) return;

    if (response->text) {
        g_free(response->text);
        response->text = NULL;
    }
    memset(response->error_message, 0, sizeof(response->error_message));
    g_free(response);
}

/* ===================================================================
 * Public API: whisper_client_create
 * =================================================================== */
WhisperClient* whisper_client_create(void) {
    WhisperClient *client = g_new0(WhisperClient, 1);
    if (!client) return NULL;

    client->ctx = NULL;
    client->model_loaded = false;
    atomic_store(&client->model_loading, false);
    atomic_store(&client->ctx_refcount, 0);
    atomic_store(&client->destroying, false);
    client->n_threads = DEFAULT_THREADS;
    client->gpu_index = GPU_INDEX_AUTO_MEMORY;  // Auto-select by free memory
    client->active_gpu_device = -1;             // -1 means CPU until model is loaded
    client->model_path[0] = '\0';
    strncpy(client->language, "auto", sizeof(client->language) - 1);
    client->language[sizeof(client->language) - 1] = '\0';
    client->error_message[0] = '\0';
    client->gpu_fallback_message[0] = '\0';
    client->error_code = 0;
    atomic_store(&client->cancel_requested, 0);

    if (pthread_mutex_init(&client->mutex, NULL) != 0) {
        g_free(client);
        return NULL;
    }
    return client;
}

/* ===================================================================
 * Public API: whisper_client_destroy
 * =================================================================== */
void whisper_client_destroy(WhisperClient* client) {
    if (!client) return;

    // Set destroying flag to block new transcriptions
    atomic_store(&client->destroying, true);
    atomic_store(&client->model_loading, false);

    // Signal cancellation to any in-flight transcription
    atomic_store(&client->cancel_requested, 1);

    pthread_mutex_lock(&client->mutex);

    // Save GPU device for cleanup while holding mutex
#ifdef HAVE_CUDA
    int saved_gpu = client->active_gpu_device;
#endif

    // Take ownership of the context pointer (set to NULL so refcount drops to 0)
    struct whisper_context *to_free = client->ctx;
    client->ctx = NULL;
    atomic_store(&client->model_loaded, false);
    atomic_store(&client->ctx_refcount, 0);

    pthread_mutex_unlock(&client->mutex);

    // Free the context outside the mutex to avoid holding lock during CUDA ops
    if (to_free) {
#ifdef HAVE_CUDA
        if (saved_gpu >= 0) {
            cudaSetDevice(saved_gpu);
        }
#endif
        safe_whisper_free(to_free);
    }

    pthread_mutex_destroy(&client->mutex);
    g_free(client);
}

/* ===================================================================
 * Public API: whisper_client_set_model_path
 * =================================================================== */
bool whisper_client_set_model_path(WhisperClient* client, const char* path) {
    if (!client || !path) return false;

    pthread_mutex_lock(&client->mutex);

    // Resolve the model path
    char resolved[MAX_PATH_LEN];
    int ret = whisper_resolve_model_path(path, resolved, sizeof(resolved));

    // Store the path (even if not found yet - it may be created later)
    strncpy(client->model_path, resolved, sizeof(client->model_path) - 1);
    client->model_path[sizeof(client->model_path) - 1] = '\0';

    // If model was previously loaded with a different path, unload it
    if (client->ctx && ret == 0) {
        // Check if path changed
        if (strcmp(client->model_path, resolved) != 0) {
            free_current_context(client);
            atomic_store(&client->model_loaded, false);
        }
    }

    pthread_mutex_unlock(&client->mutex);
    return true;
}

/* ===================================================================
 * Public API: whisper_client_set_language
 * =================================================================== */
bool whisper_client_set_language(WhisperClient* client, const char* language) {
    if (!client) return false;

    pthread_mutex_lock(&client->mutex);

    if (language && language[0] != '\0') {
        strncpy(client->language, language, sizeof(client->language) - 1);
        client->language[sizeof(client->language) - 1] = '\0';
    } else {
        strncpy(client->language, "auto", sizeof(client->language) - 1);
        client->language[sizeof(client->language) - 1] = '\0';
    }

    pthread_mutex_unlock(&client->mutex);
    return true;
}

/* ===================================================================
 * Helper: perform transcription given context and samples (no mutex held)
 * Returns WhisperResponse; caller must free it.
 * =================================================================== */
static WhisperResponse *transcribe_with_context(WhisperClient *client,
                                                 struct whisper_context *ctx,
                                                 int n_threads_cfg,
                                                 const char *lang,
                                                 int active_gpu_device,
                                                 const float *samples,
                                                 int n_samples) {
    WhisperResponse *response = g_new0(WhisperResponse, 1);
    if (!response) return NULL;

    // Setup parameters using deduplicated helper
    struct whisper_full_params params;
    setup_transcription_params(n_threads_cfg, lang, &params, &client->cancel_requested);

    // Ensure CUDA context is set to the correct GPU device before inference.
#ifdef HAVE_CUDA
    if (active_gpu_device >= 0) {
        cudaSetDevice(active_gpu_device);
    }
#endif

    int result = safe_whisper_full(ctx, params, samples, n_samples);

    if (result != 0) {
        const char *err_msg = "Unknown transcription error";
        switch (result) {
            case -1: err_msg = "Failed to compute log mel spectrogram"; break;
            case -2: err_msg = "Failed to auto-detect language"; break;
            case -3: err_msg = "Too many source language tokens"; break;
            case -4: err_msg = "whisper_decode() failed"; break;
            case -5: err_msg = "Failed to batch decode"; break;
            case -100: err_msg = "GPU out-of-memory (C++ exception during transcription)"; break;
            default: err_msg = "Transcription failed";
        }
        pthread_mutex_lock(&client->mutex);
        set_error(client, WHISPER_ERR_TRANSCRIBE_FAIL, err_msg);
        pthread_mutex_unlock(&client->mutex);
        strncpy(response->error_message, err_msg, sizeof(response->error_message) - 1);
        response->error_code = WHISPER_ERR_TRANSCRIBE_FAIL;
        return response;
    }

    // Extract text from segments
    int n_segments = safe_whisper_full_n_segments(ctx);

    if (n_segments <= 0) {
        strncpy(response->error_message, "No transcription segments produced", sizeof(response->error_message) - 1);
        response->error_code = WHISPER_ERR_NO_SEGMENTS;
        return response;
    }

    // Use deduplicated segment extraction helper
    char *full_text = extract_segments_text(ctx, n_segments);
    if (!full_text) {
        pthread_mutex_lock(&client->mutex);
        set_error(client, WHISPER_ERR_MEMORY_ALLOC, "Memory allocation failed");
        pthread_mutex_unlock(&client->mutex);
        strncpy(response->error_message, "Memory allocation failed", sizeof(response->error_message) - 1);
        response->error_code = WHISPER_ERR_MEMORY_ALLOC;
        return response;
    }

    response->text = full_text;
    response->success = true;
    response->error_code = WHISPER_ERR_OK;

    return response;
}

/* ===================================================================
 * Public API: whisper_transcribe
 * Mutex is now held only to access shared state (context, config), then
 * released before the actual transcription runs. Context reference counting
 * prevents use-after-free when another thread unloads the model.
 * =================================================================== */
WhisperResponse* whisper_transcribe(WhisperClient* client, const char* wav_path) {
    WhisperResponse *response = g_new0(WhisperResponse, 1);
    if (!response) return NULL;

    response->success = false;
    response->error_code = WHISPER_ERR_OK;
    response->text = NULL;
    response->error_message[0] = '\0';

    if (!client || !wav_path) {
        strncpy(response->error_message, "Invalid parameters", sizeof(response->error_message) - 1);
        response->error_code = WHISPER_ERR_INVALID_PARAM;
        return response;
    }

    // Check destroying flag before proceeding
    if (atomic_load(&client->destroying)) {
        strncpy(response->error_message, "Client is being destroyed", sizeof(response->error_message) - 1);
        response->error_code = WHISPER_ERR_INVALID_PARAM;
        return response;
    }

    // --- Phase 1: Lock mutex to access shared state ---
    pthread_mutex_lock(&client->mutex);

    // Reset cancel flag
    atomic_store(&client->cancel_requested, 0);

    // Load model if needed
    if (!load_model(client)) {
        snprintf(response->error_message, sizeof(response->error_message), "%s", client->error_message);
        response->error_message[sizeof(response->error_message) - 1] = '\0';
        response->error_code = client->error_code;
        pthread_mutex_unlock(&client->mutex);
        return response;
    }

    // Copy needed config while holding mutex — including GPU device index
    struct whisper_context *ctx = client->ctx;
    int n_threads_cfg = client->n_threads;
    char lang[16];
    strncpy(lang, client->language, sizeof(lang));
    lang[sizeof(lang) - 1] = '\0';
    int gpu_device = client->active_gpu_device;

    // Increment refcount to keep context alive during transcription
    ctx_ref(client);

    pthread_mutex_unlock(&client->mutex);
    // --- Mutex released: now run long operations without holding it ---

    // Read WAV samples (can take time for large files)
    float *samples = NULL;
    int n_samples = 0;
    if (!read_wav_samples(wav_path, &samples, &n_samples)) {
        pthread_mutex_lock(&client->mutex);
        set_error(client, WHISPER_ERR_WAV_READ_FAIL, "Failed to read WAV file");
        pthread_mutex_unlock(&client->mutex);
        strncpy(response->error_message, "Failed to read WAV file", sizeof(response->error_message) - 1);
        response->error_code = WHISPER_ERR_WAV_READ_FAIL;
        g_free(samples);
        ctx_unref(client);
        return response;
    }

    // Perform transcription with context ref held.
    // Use a local variable to receive the result from transcribe_with_context(),
    // then copy the data into our original 'response' struct to avoid leaking
    // the initial allocation when overwriting the pointer.
    WhisperResponse *result = transcribe_with_context(client, ctx, n_threads_cfg, lang, gpu_device, samples, n_samples);
    g_free(samples);

    if (result) {
        response->success = result->success;
        response->error_code = result->error_code;
        memcpy(response->error_message, result->error_message, sizeof(response->error_message));
        // Steal the text pointer so we don't duplicate it
        response->text = result->text;
        result->text = NULL;  // Prevent whisper_response_free from double-freeing
        whisper_response_free(result);
    }

    // Release context reference
    ctx_unref(client);

    return response;
}

/* ===================================================================
 * Public API: whisper_transcribe_samples (in-memory, no WAV file)
 * =================================================================== */
WhisperResponse* whisper_transcribe_samples(WhisperClient* client,
                                              const int16_t *samples,
                                              int n_samples) {
    WhisperResponse *response = g_new0(WhisperResponse, 1);
    if (!response) return NULL;

    response->text = NULL;
    response->error_code = WHISPER_ERR_INVALID_PARAM;
    response->success = false;
    response->error_message[0] = '\0';

    if (!client || !samples || n_samples <= 0) {
        strncpy(response->error_message, "Invalid parameters", sizeof(response->error_message) - 1);
        response->error_code = WHISPER_ERR_INVALID_PARAM;
        return response;
    }

    // Check destroying flag before proceeding
    if (atomic_load(&client->destroying)) {
        strncpy(response->error_message, "Client is being destroyed", sizeof(response->error_message) - 1);
        response->error_code = WHISPER_ERR_INVALID_PARAM;
        return response;
    }

    // --- Phase 1: Lock mutex to access shared state ---
    pthread_mutex_lock(&client->mutex);

    // Reset cancel flag
    atomic_store(&client->cancel_requested, 0);

    // Load model if needed
    if (!load_model(client)) {
        snprintf(response->error_message, sizeof(response->error_message), "%s", client->error_message);
        response->error_message[sizeof(response->error_message) - 1] = '\0';
        response->error_code = client->error_code;
        pthread_mutex_unlock(&client->mutex);
        return response;
    }

    // Copy needed config while holding mutex — including GPU device index
    struct whisper_context *ctx = client->ctx;
    int n_threads_cfg = client->n_threads;
    char lang[16];
    strncpy(lang, client->language, sizeof(lang));
    lang[sizeof(lang) - 1] = '\0';
    int gpu_device = client->active_gpu_device;

    // Increment refcount to keep context alive during transcription
    ctx_ref(client);

    pthread_mutex_unlock(&client->mutex);
    // --- Mutex released ---

    // Convert int16_t PCM to float32 samples expected by whisper.cpp
    float *float_samples = g_malloc((size_t)n_samples * sizeof(float));
    if (!float_samples) {
        pthread_mutex_lock(&client->mutex);
        set_error(client, WHISPER_ERR_MEMORY_ALLOC, "Memory allocation failed for sample conversion");
        pthread_mutex_unlock(&client->mutex);
        strncpy(response->error_message, "Memory allocation failed", sizeof(response->error_message) - 1);
        response->error_code = WHISPER_ERR_MEMORY_ALLOC;
        ctx_unref(client);
        return response;
    }

    for (int i = 0; i < n_samples; i++) {
        float_samples[i] = (float)samples[i] / 32768.0f;
    }

    // Perform transcription with context ref held.
    // Use a local variable to receive the result from transcribe_with_context(),
    // then copy the data into our original 'response' struct to avoid leaking
    // the initial allocation when overwriting the pointer.
    WhisperResponse *result = transcribe_with_context(client, ctx, n_threads_cfg, lang, gpu_device, float_samples, n_samples);
    g_free(float_samples);

    if (result) {
        response->success = result->success;
        response->error_code = result->error_code;
        memcpy(response->error_message, result->error_message, sizeof(response->error_message));
        // Steal the text pointer so we don't duplicate it
        response->text = result->text;
        result->text = NULL;  // Prevent whisper_response_free from double-freeing
        whisper_response_free(result);
    }

    // Release context reference
    ctx_unref(client);

    return response;
}

/* ===================================================================
 * Public API: whisper_transcribe_samples_with_retry
 * =================================================================== */
WhisperResponse* whisper_transcribe_samples_with_retry(WhisperClient* client,
                                                         const int16_t *samples,
                                                         int n_samples,
                                                         int max_retries) {
    if (!client || !samples || n_samples <= 0) return NULL;

    int attempts = max_retries > 0 ? max_retries + 1 : 1;
    WhisperResponse *last_response = NULL;

    for (int i = 0; i < attempts; i++) {
        WhisperResponse *response = whisper_transcribe_samples(client, samples, n_samples);
        if (!response) {
            continue;
        }

        if (response->success) {
            if (last_response) {
                whisper_response_free(last_response);
            }
            return response;
        }

        int code = response->error_code;

        // Only retry on certain error codes (5=decode error, 7=memory)
        if (code != WHISPER_ERR_TRANSCRIBE_FAIL && code != WHISPER_ERR_MEMORY_ALLOC) {
            if (last_response) {
                whisper_response_free(last_response);
            }
            last_response = response;
            break;
        }

        if (last_response) {
            whisper_response_free(last_response);
        }
        last_response = response;

        if (i == attempts - 1) break;

        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Retry %d/%d after error: %s\n",
                i + 1, max_retries, whisper_client_get_error(client));

        /* Use g_usleep for brief retry delay. Note: g_usleep wraps nanosleep()
         * on modern GLib, making it POSIX-compliant. The delay is short enough
         * (100-300ms) that it won't block the transcription thread meaningfully. */
        g_usleep(100000 * (size_t)(i + 1));  // 100ms, 200ms, ...
    }

    return last_response;
}

/* ===================================================================
 * Public API: whisper_transcribe_with_retry
 * =================================================================== */
/* Restructured retry loop to avoid unconditional extra call.
 * Free failed responses on each retry iteration to prevent
 * accumulating orphaned WhisperResponse structs when retries are exhausted. */
WhisperResponse* whisper_transcribe_with_retry(WhisperClient* client, const char* wav_path, int max_retries) {
    if (!client || !wav_path) return NULL;

    int attempts = max_retries > 0 ? max_retries + 1 : 1;
    WhisperResponse *last_response = NULL;

    for (int i = 0; i < attempts; i++) {
        WhisperResponse *response = whisper_transcribe(client, wav_path);
        if (!response) {
            continue;
        }

        if (response->success) {
            /* Free any previous failed response before returning success */
            if (last_response) {
                whisper_response_free(last_response);
            }
            return response;
        }

        int code = response->error_code;

        // Only retry on certain error codes (4=read error, 5=decode error, 7=memory)
        if (code != WHISPER_ERR_WAV_READ_FAIL && code != WHISPER_ERR_TRANSCRIBE_FAIL &&
            code != WHISPER_ERR_MEMORY_ALLOC) {
            // Non-retryable error -- free previous and return this one
            if (last_response) {
                whisper_response_free(last_response);
            }
            last_response = response;
            break;
        }

        // Free previous failed response before saving the new one
        if (last_response) {
            whisper_response_free(last_response);
        }
        last_response = response;

        if (i == attempts - 1) break;  // Last attempt

        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Retry %d/%d after error: %s\n",
                i + 1, max_retries, whisper_client_get_error(client));

        // Brief delay before retry
        g_usleep(100000 * (size_t)(i + 1));  // 100ms, 200ms, ...
    }

    return last_response;
}

/* ===================================================================
 * Public API: whisper_validate_model_file (validates model file)
 *
 * Checks file exists, is a regular file, and has a valid Whisper model
 * magic header (GGML 0x67676d6c or GGUF 0x46554747). Does NOT load the
 * full model — that would be too slow for UI validation (800MB+ models).
 * The actual model loading happens lazily on first transcription.
 * =================================================================== */
bool whisper_validate_model_file(const char* model_path) {
    if (!model_path || model_path[0] == '\0') return false;

    /* Resolve the path first — handles bare filenames like "ggml-base.bin"
     * by searching default directories (~/.cache/whisper/, etc.) and
     * expands tildes in paths like "~/.cache/whisper/ggml-base.bin" */
    char resolved_path[MAX_PATH_LEN];
    whisper_resolve_model_path(model_path, resolved_path, sizeof(resolved_path));

    struct stat st;
    if (stat(resolved_path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (st.st_size < 4) return false;  /* Too small to be a valid model */

    FILE *f = fopen(resolved_path, "rb");
    if (!f) return false;

    uint32_t magic;
    if (fread(&magic, 1, 4, f) != 4) {
        fclose(f);
        return false;
    }
    fclose(f);

    if (magic == GGML_MAGIC || magic == GGUF_MAGIC) {
        return true;
    }

    g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Invalid model magic: 0x%08x (expected GGML 0x%08x or GGUF 0x%08x)\n",
            magic, GGML_MAGIC, GGUF_MAGIC);
    return false;
}

/* ===================================================================
 * Public API: whisper_check_connection (verifies model file)
 * =================================================================== */
bool whisper_check_connection(WhisperClient* client) {
    if (!client) return false;

    pthread_mutex_lock(&client->mutex);

    if (client->model_path[0] == '\0') {
        set_error(client, WHISPER_ERR_MODEL_NOT_FOUND, "No model path configured");
        pthread_mutex_unlock(&client->mutex);
        return false;
    }

    struct stat st;
    bool exists = (stat(client->model_path, &st) == 0 && S_ISREG(st.st_mode));

    if (exists) {
        set_error(client, WHISPER_ERR_OK, "");
    } else {
        snprintf(client->error_message, sizeof(client->error_message),
                  "Model file not found: %.200s", client->model_path);
        client->error_code = WHISPER_ERR_MODEL_NOT_FOUND;
    }

    pthread_mutex_unlock(&client->mutex);
    return exists;
}

/* ===================================================================
 * Public API: whisper_client_get_error
 * =================================================================== */
const char* whisper_client_get_error(WhisperClient* client) {
    static __thread char local_buffer[256] = {0};
    if (!client) return "No client";
    pthread_mutex_lock(&client->mutex);
    snprintf(local_buffer, sizeof(local_buffer), "%s", client->error_message);
    pthread_mutex_unlock(&client->mutex);
    return local_buffer[0] != '\0' ? local_buffer : "";
}

/* ===================================================================
 * Public API: whisper_client_get_gpu_fallback_message
 * =================================================================== */
const char* whisper_client_get_gpu_fallback_message(WhisperClient* client) {
    static __thread char local_buffer[512] = {0};
    if (!client) return "";
    pthread_mutex_lock(&client->mutex);
    snprintf(local_buffer, sizeof(local_buffer), "%s", client->gpu_fallback_message);
    pthread_mutex_unlock(&client->mutex);
    return local_buffer[0] != '\0' ? local_buffer : "";
}

/* ===================================================================
 * Public API: whisper_client_cancel
 * =================================================================== */
void whisper_client_cancel(WhisperClient* client) {
    if (!client) return;
    atomic_store(&client->cancel_requested, 1);
}
