/*
 * SPDX-License-Identifier: MIT
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
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE  /* For usleep on glibc >= 2.37 */

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

#include "app_gpu.h"

/* ===================================================================
 * Constants and defaults
 * =================================================================== */

#include <limits.h>

#define MAX_PATH_LEN  PATH_MAX
#define DEFAULT_THREADS 0  // 0 = use all available threads
#define GPU_INDEX_AUTO    -1  // Auto-detect GPU
#define GPU_INDEX_CPU_ONLY -2 // Force CPU-only

/* Default model search directories (in order of preference) */
static const char *DEFAULT_MODEL_DIRS[] = {
    "~/.cache/whisper",
    "/usr/share/transcriber/models",
    NULL
};

/* ===================================================================
 * Internal WhisperClient struct
 * =================================================================== */
struct _WhisperClient {
    struct whisper_context *ctx;       // whisper.cpp context (loaded model)
    char model_path[MAX_PATH_LEN];
    int n_threads;
    int gpu_index;                     // -1 = auto-detect, -2 = CPU-only, >=0 = specific GPU
    char error_message[256];
    int error_code;
    pthread_mutex_t mutex;
    atomic_int cancel_requested;
    bool model_loaded;
    bool model_loading;                // true while model load is in progress
    bool using_gpu;                    // true if model is running on GPU
};

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
/* M-006 fix: Distinct return codes for path resolution:
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
            // ~username style - not supported, treat as literal
            snprintf(resolved, sizeof(resolved), "%s", input);
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

    // ME-01 fix: Simplified -- only search default dirs for bare filenames (no '/')
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
 * Public: Get GPU usage status
 * =================================================================== */
bool whisper_client_is_using_gpu(const WhisperClient *client) {
    if (!client) return false;
    return client->using_gpu;
}

/* ===================================================================
 * Public: Get backend description string
 * =================================================================== */
const char *whisper_client_backend_description(const WhisperClient *client) {
    if (!client || !client->model_loaded) return "Not loaded";
    return client->using_gpu ? "GPU (CUDA)" : "CPU";
}

/* ===================================================================
 * Public: Check if model is loaded
 * =================================================================== */
bool whisper_client_is_model_loaded(const WhisperClient *client) {
    if (!client) return false;
    return client->model_loaded;
}

/* ===================================================================
 * Public: Check if model is currently loading
 * =================================================================== */
bool whisper_client_is_loading(const WhisperClient *client) {
    if (!client) return false;
    return client->model_loading;
}

/* ===================================================================
 * Public: Unload model from memory
 * =================================================================== */
void whisper_client_unload_model(WhisperClient *client) {
    if (!client) return;

    pthread_mutex_lock(&client->mutex);
    if (client->ctx) {
        whisper_free(client->ctx);
        client->ctx = NULL;
    }
    client->model_loaded = false;
    client->model_loading = false;
    client->using_gpu = false;
    pthread_mutex_unlock(&client->mutex);
}

/* ===================================================================
 * Internal: load whisper model with parameters
 * =================================================================== */
static bool load_model_internal(WhisperClient *client) {
    if (!client) return false;
    if (client->model_loaded) return true;

    if (client->model_path[0] == '\0') {
        set_error(client, 1, "No model path configured");
        return false;
    }

    // Check if model file exists
    struct stat st;
    if (stat(client->model_path, &st) != 0) {
        snprintf(client->error_message, sizeof(client->error_message),
                 "Model file not found: %.200s", client->model_path);
        client->error_code = 2;
        return false;
    }

    // Determine GPU strategy
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
            // Check if free memory meets minimum threshold (2 GB)
            if (free_mem >= ((size_t)(2UL * 1024 * 1024 * 1024))) {
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
        // Legacy auto-detect (GPU_INDEX_AUTO = -1): try first GPU
        try_gpu = whisper_gpu_available();
        gpu_idx = 0; // Default to first GPU for legacy auto-detect
    }

    bool gpu_loaded = false;

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = try_gpu;

    // Set GPU device if we have a specific index
#ifdef HAVE_CUDA
    if (try_gpu && gpu_idx >= 0) {
        cparams.gpu_device = gpu_idx;
    }
#endif

    struct whisper_context *ctx = NULL;

    if (try_gpu) {
        ctx = whisper_init_from_file_with_params(client->model_path, cparams);

        if (ctx) {
            gpu_loaded = true;
        } else {
            cparams.use_gpu = false;
            ctx = whisper_init_from_file_with_params(client->model_path, cparams);
        }
    } else {
        ctx = whisper_init_from_file_with_params(client->model_path, cparams);
    }

    if (!ctx) {
        snprintf(client->error_message, sizeof(client->error_message),
                 "Failed to load whisper model: %.200s", client->model_path);
        client->error_code = 3;
        return false;
    }

    client->ctx = ctx;
    client->model_loaded = true;
    client->using_gpu = gpu_loaded;


    return true;
}

/* ===================================================================
 * Internal: load model (wrapper with loading state tracking)
 * =================================================================== */
static bool load_model(WhisperClient *client) {
    if (!client || client->model_loaded) return true;

    // Mark as loading
    client->model_loading = true;

    bool result = load_model_internal(client);

    // Clear loading flag
    client->model_loading = false;

    return result;
}

/* ===================================================================
 * Public: Load the model synchronously (blocking call)
 *
 * @param client      WhisperClient instance
 * @param gpu_mode    GPU mode string: "auto", "cpu", or "gpu:N"
 *                    If NULL, defaults to "auto" (GPU_INDEX_AUTO_MEMORY)
 * =================================================================== */
bool whisper_client_load_model(WhisperClient *client, const char *gpu_mode) {
    if (!client) return false;

    pthread_mutex_lock(&client->mutex);

    // Parse GPU mode string to get internal index
    int gpu_index = GPU_INDEX_AUTO_MEMORY;  // Default to auto
    if (gpu_mode) {
        gpu_mode_parse(gpu_mode, &gpu_index);
    }

    // If already loaded with same GPU config, skip
    if (client->model_loaded && client->gpu_index == gpu_index) {
        pthread_mutex_unlock(&client->mutex);
        return true;
    }

    // If already loaded but GPU config differs, unload first
    if (client->model_loaded) {
        if (client->ctx) {
            whisper_free(client->ctx);
            client->ctx = NULL;
        }
        client->model_loaded = false;
        client->using_gpu = false;
    }

    // Set the GPU index preference
    client->gpu_index = gpu_index;

    // Load the model
    client->model_loading = true;
    bool result = load_model_internal(client);
    client->model_loading = false;

    pthread_mutex_unlock(&client->mutex);

    return result;
}

/* ===================================================================
 * Helper: read WAV file and extract PCM samples
 * =================================================================== */
/* ME-02 fix: Little-endian byte extraction helpers for portable WAV parsing */
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

    // Read PCM data and convert to float32
    int n_samples = data_size / (bits_per_sample / 8);
    int bytes_per_sample_actual = bits_per_sample / 8;

    // Handle multi-channel by only taking first channel samples
    if (channels > 1) {
        n_samples = n_samples / channels;
    }

    float *samples = (float *)calloc(n_samples, sizeof(float));
    if (!samples) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Memory allocation failed for samples\n");
        fclose(f);
        return false;
    }

    // Read raw PCM data
    int16_t *pcm = (int16_t *)malloc(data_size);
    if (!pcm) {
        free(samples);
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Memory allocation failed for PCM buffer\n");
        fclose(f);
        return false;
    }

    size_t read_count = fread(pcm, bytes_per_sample_actual * channels, n_samples * channels, f);
    fclose(f);

    if (read_count < (size_t)(n_samples * channels)) {
        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Warning: read %zu samples, expected %d\n",
                read_count, n_samples * channels);
        n_samples = read_count / channels;
    }

    // Convert to float32 normalized to [-1, 1]
    for (int i = 0; i < n_samples; i++) {
        if (bits_per_sample == 16) {
            samples[i] = (float)pcm[i * channels] / 32768.0f;
        } else if (bits_per_sample == 8) {
            samples[i] = ((float)pcm[i * channels] - 128.0f) / 128.0f;
        } else {
            samples[i] = (float)pcm[i * channels] / 32768.0f;
        }
    }

    free(pcm);
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
        free(response->text);
        response->text = NULL;
    }
    memset(response->error_message, 0, sizeof(response->error_message));
    free(response);
}

/* ===================================================================
 * Public API: whisper_client_create
 * =================================================================== */
WhisperClient* whisper_client_create(void) {
    WhisperClient *client = (WhisperClient *)calloc(1, sizeof(WhisperClient));
    if (!client) return NULL;

    client->ctx = NULL;
    client->model_loaded = false;
    client->model_loading = false;
    client->n_threads = DEFAULT_THREADS;
    client->gpu_index = GPU_INDEX_AUTO;  // Auto-detect by default
    client->model_path[0] = '\0';
    client->error_message[0] = '\0';
    client->error_code = 0;
    atomic_store(&client->cancel_requested, 0);

    pthread_mutex_init(&client->mutex, NULL);
    return client;
}

/* ===================================================================
 * Public API: whisper_client_destroy
 * =================================================================== */
void whisper_client_destroy(WhisperClient* client) {
    if (!client) return;

    pthread_mutex_lock(&client->mutex);
    if (client->ctx) {
        whisper_free(client->ctx);
        client->ctx = NULL;
    }
    client->model_loaded = false;
    client->model_loading = false;
    pthread_mutex_unlock(&client->mutex);

    pthread_mutex_destroy(&client->mutex);
    memset(client, 0, sizeof(WhisperClient));
    free(client);
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
            whisper_free(client->ctx);
            client->ctx = NULL;
            client->model_loaded = false;
        }
    }

    pthread_mutex_unlock(&client->mutex);
    return true;
}

/* ===================================================================
 * Public API: whisper_transcribe
 * HI-01 fix: Mutex is now held only to access shared state (context,
 * config), then released before the actual transcription runs. This
 * prevents blocking whisper_check_connection(), whisper_client_set_model_path(),
 * and whisper_client_destroy() during long transcription operations.
 * =================================================================== */
WhisperResponse* whisper_transcribe(WhisperClient* client, const char* wav_path) {
    WhisperResponse *response = (WhisperResponse *)calloc(1, sizeof(WhisperResponse));
    if (!response) return NULL;

    response->success = false;
    response->error_code = 0;
    response->text = NULL;
    response->error_message[0] = '\0';

    if (!client || !wav_path) {
        strncpy(response->error_message, "Invalid parameters", sizeof(response->error_message) - 1);
        response->error_code = 1;
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

    // Copy needed config while holding mutex
    struct whisper_context *ctx = client->ctx;
    int n_threads_cfg = client->n_threads;

    pthread_mutex_unlock(&client->mutex);
    // --- Mutex released: now run long operations without holding it ---

    // Read WAV samples (can take time for large files)
    float *samples = NULL;
    int n_samples = 0;
    if (!read_wav_samples(wav_path, &samples, &n_samples)) {
        pthread_mutex_lock(&client->mutex);
        set_error(client, 4, "Failed to read WAV file");
        pthread_mutex_unlock(&client->mutex);
        strncpy(response->error_message, "Failed to read WAV file", sizeof(response->error_message) - 1);
        response->error_code = 4;
        return response;
    }

    // Setup transcription parameters
    struct whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    // Configure parameters
    params.n_threads = n_threads_cfg > 0 ? n_threads_cfg : (int)sysconf(_SC_NPROCESSORS_ONLN);
    params.print_progress = false;
    params.print_realtime = false;
    params.print_special = false;
    params.translate = false;
    params.no_timestamps = true;

    // Setup cancellation callback - whisper.cpp will call this periodically
    // to check if transcription should be aborted
    params.abort_callback = whisper_abort_callback;
    params.abort_callback_user_data = &client->cancel_requested;

    // Always auto-detect language
    params.language = NULL;

    // Run transcription WITHOUT holding the mutex
    int result = whisper_full(ctx, params, samples, n_samples);

    free(samples);
    samples = NULL;

    if (result != 0) {
        const char *err_msg = "Unknown transcription error";
        switch (result) {
            case -1: err_msg = "Failed to compute log mel spectrogram"; break;
            case -2: err_msg = "Failed to auto-detect language"; break;
            case -3: err_msg = "Too many source language tokens"; break;
            case -4: err_msg = "whisper_decode() failed"; break;
            case -5: err_msg = "Failed to batch decode"; break;
            default: err_msg = "Transcription failed";
        }
        pthread_mutex_lock(&client->mutex);
        set_error(client, 5, err_msg);
        pthread_mutex_unlock(&client->mutex);
        strncpy(response->error_message, err_msg, sizeof(response->error_message) - 1);
        response->error_code = 5;
        return response;
    }

    // Extract text from segments
    int n_segments = whisper_full_n_segments(ctx);
    if (n_segments <= 0) {
        strncpy(response->error_message, "No transcription segments produced", sizeof(response->error_message) - 1);
        response->error_code = 6;
        return response;
    }

    // Concatenate all segments - single pass with memcpy for O(n) performance
    size_t total_len = 0;
    for (int i = 0; i < n_segments; i++) {
        const char *text = whisper_full_get_segment_text(ctx, i);
        if (text) {
            total_len += strlen(text);
        }
    }

    char *full_text = (char *)malloc(total_len + 1);
    if (!full_text) {
        pthread_mutex_lock(&client->mutex);
        set_error(client, 7, "Memory allocation failed");
        pthread_mutex_unlock(&client->mutex);
        strncpy(response->error_message, "Memory allocation failed", sizeof(response->error_message) - 1);
        response->error_code = 7;
        return response;
    }

    full_text[0] = '\0';
    size_t offset = 0;
    for (int i = 0; i < n_segments; i++) {
        const char *text = whisper_full_get_segment_text(ctx, i);
        if (text) {
            size_t len = strlen(text);
            memcpy(full_text + offset, text, len);
            offset += len;
        }
    }
    full_text[offset] = '\0';

    // Trim trailing whitespace/newline
    size_t len = strlen(full_text);
    while (len > 0 && (full_text[len-1] == '\n' || full_text[len-1] == ' ')) {
        full_text[--len] = '\0';
    }

    response->text = full_text;
    response->success = true;
    response->error_code = 0;

    return response;
}

/* ===================================================================
 * Public API: whisper_transcribe_with_retry
 * =================================================================== */
/* ME-03 fix: Restructured retry loop to avoid unconditional extra call. */
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
            return response;
        }

        int code = response->error_code;

        // Only retry on certain error codes (4=read error, 5=decode error, 7=memory)
        if (code != 4 && code != 5 && code != 7) {
            // Non-retryable error -- return immediately
            last_response = response;
            break;
        }

        // Save as last response in case this is the final attempt
        last_response = response;

        if (i == attempts - 1) break;  // Last attempt

        g_log("app-whisper", G_LOG_LEVEL_MESSAGE, "[whisper] Retry %d/%d after error: %s\n",
                i + 1, max_retries, whisper_client_get_error(client));

        // Brief delay before retry
        usleep(100000 * (i + 1));  // 100ms, 200ms, ...
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

    /* GGML magic: 0x67676d6c ("ggml" little-endian) */
    #define GGML_MAGIC 0x67676d6cu
    /* GGUF magic: 0x46554747 ("GGUF" as uint32 little-endian) */
    #define GGUF_MAGIC 0x46554747u

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
        set_error(client, 1, "No model path configured");
        pthread_mutex_unlock(&client->mutex);
        return false;
    }

    struct stat st;
    bool exists = (stat(client->model_path, &st) == 0 && S_ISREG(st.st_mode));

    if (exists) {
        set_error(client, 0, "");
    } else {
        snprintf(client->error_message, sizeof(client->error_message),
                 "Model file not found: %.200s", client->model_path);
        client->error_code = 2;
    }

    pthread_mutex_unlock(&client->mutex);
    return exists;
}

/* ===================================================================
 * Public API: whisper_client_get_error
 * =================================================================== */
const char* whisper_client_get_error(WhisperClient* client) {
    if (!client) return "No client";
    return client->error_message[0] != '\0' ? client->error_message : "";
}

/* ===================================================================
 * Public API: whisper_client_get_error_code (returns error_code)
 * =================================================================== */
int whisper_client_get_error_code(const WhisperClient* client) {
    if (!client) return -1;
    return client->error_code;
}

/* ===================================================================
 * Public API: whisper_client_cancel
 * =================================================================== */
void whisper_client_cancel(WhisperClient* client) {
    if (!client) return;
    atomic_store(&client->cancel_requested, 1);
}
