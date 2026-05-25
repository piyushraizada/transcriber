/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_WHISPER_H
#define APP_WHISPER_H

/**
 * @file app_whisper.h
 * @brief Local Whisper transcription via whisper.cpp
 *
 * This module implements local, offline speech-to-text transcription using
 * the whisper.cpp library (ggml-org/whisper.cpp). No network connection or
 * external API is required — all processing happens on the local machine.
 *
 * Key features:
 *   - Loads GGML model files (ggml-base.bin, ggml-small.bin, etc.)
 *   - Transcribes WAV files (16kHz, mono, 16-bit PCM)
 *   - Supports language specification and auto-detection
 *   - Thread-safe with mutex-protected model access
 *   - Graceful cancellation support for long transcriptions
 *
 * The module maintains API compatibility with the previous external API
 * implementation, so the rest of the application code remains unchanged.
 * The "URL" concept is repurposed as the model path, and "connection check"
 * verifies the model file exists and can be loaded.
 *
 * Threading:
 *   - Transcription runs in a dedicated background thread
 *   - The whisper.cpp context is NOT thread-safe, so a mutex protects
 *     concurrent access during transcription
 *   - Cancellation is supported via an atomic flag checked during processing
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Section 1: Whisper Response Structure
 *---------------------------------------------------------------------------
 * Contains the parsed result from a local Whisper transcription request.
 *
 * This structure holds the transcription text extracted from whisper.cpp,
 * along with metadata about the result status. The text field is
 * dynamically allocated and must be freed using whisper_response_free()
 * when no longer needed.
 */
typedef struct {
    char* text;              ///< Transcription text (dynamically allocated, NULL on error)
    int error_code;          ///< 0 on success, non-zero error code on failure
    bool success;            ///< true if transcription succeeded
    char error_message[256]; ///< Human-readable error message (empty if success)
} WhisperResponse;

/*---------------------------------------------------------------------------
 * Section 2: Whisper Client Handle (Opaque)
 *---------------------------------------------------------------------------
 * Opaque handle to the internal Whisper client state. The actual structure
 * contains the whisper.cpp context, model path, language settings, and
 * thread synchronization primitives.
 */
typedef struct _WhisperClient WhisperClient;

/*---------------------------------------------------------------------------
 * Section 3: Initialization and Cleanup
 *---------------------------------------------------------------------------
 * Functions for creating, configuring, and destroying the Whisper client.
 */

/**
 * Create and initialize a new Whisper client instance.
 *
 * This function allocates memory for the WhisperClient struct and
 * initializes internal state. The model is NOT loaded yet — use
 * whisper_client_set_model_path() to set the model path, which will trigger
 * model loading on the first transcription.
 *
 * @return A valid WhisperClient* on success, or NULL on allocation failure.
 */
WhisperClient* whisper_client_create(void);

/**
 * Destroy a Whisper client and free all associated resources.
 *
 * This function performs complete cleanup:
 *   1. Free whisper.cpp context (if loaded)
 *   2. Free model path and language strings
 *   3. Destroy mutex
 *   4. Free the WhisperClient struct
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 */
void whisper_client_destroy(WhisperClient* client);

/*---------------------------------------------------------------------------
 * Section 4: Configuration
 *---------------------------------------------------------------------------
 * Functions for configuring the Whisper client parameters.
 */

/**
 * Set the Whisper model path.
 *
 * This function sets the path to the GGML model file. The model will be
 * loaded lazily on the first transcription request. Common model files:
 *   - ggml-tiny.bin      (~75 MB, fast, lower accuracy)
 *   - ggml-base.bin      (~140 MB, good balance)
 *   - ggml-small.bin     (~465 MB, better accuracy)
 *   - ggml-medium.bin    (~1.5 GB, high accuracy)
 *   - ggml-large-v3.bin  (~3 GB, best accuracy)
 *
 * Models can be downloaded from:
 *   https://huggingface.co/ggml-org/models
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 * @param model_path Path to the GGML model file. Must not be NULL.
 *                   The string is copied internally.
 *
 * @return true if the path was set successfully, false if invalid.
 */
bool whisper_client_set_model_path(WhisperClient* client, const char* model_path);

/*---------------------------------------------------------------------------
 * Section 5: Core Transcription API
 *---------------------------------------------------------------------------
 * The primary function for transcribing audio using local whisper.cpp.
 */

/**
 * Transcribe a WAV file using local Whisper model.
 *
 * This function loads the model (if not already loaded), processes the WAV
 * file through whisper.cpp, and returns the transcription text.
 *
 * The function performs the following steps:
 *   1. Load the GGML model (if not already loaded)
 *   2. Initialize whisper_full_params with language and threading settings
 *   3. Read the WAV file and convert to float32 samples
 *   4. Run whisper_full() for transcription
 *   5. Extract text from all segments
 *   6. Return the result in a WhisperResponse struct
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 * @param wav_path Path to the WAV file to transcribe. Must not be NULL.
 *
 * @return A pointer to a WhisperResponse struct (allocated by this function).
 *         The caller MUST free the response using whisper_response_free().
 *         Returns NULL on critical failure.
 *
 * @thread_safe This function may be called from any thread. The internal
 *              mutex protects the whisper.cpp context for concurrent access.
 */
WhisperResponse* whisper_transcribe(WhisperClient* client, const char* wav_path);

/**
 * Transcribe a WAV file with automatic retry on transient errors.
 *
 * Retry provides protection against memory allocation failures or other
 * transient issues during model loading.
 *
 * @param client         Pointer to a valid WhisperClient. Must not be NULL.
 * @param wav_path       Path to the WAV file to transcribe. Must not be NULL.
 * @param max_retries    Maximum number of retry attempts (0 = no retry).
 *
 * @return A pointer to a WhisperResponse struct (allocated by this function).
 *         The caller MUST free the response using whisper_response_free().
 */
WhisperResponse* whisper_transcribe_with_retry(WhisperClient* client, const char* wav_path, int max_retries);

/*---------------------------------------------------------------------------
 * Section 6: Connection Health Check (Model Availability)
 *---------------------------------------------------------------------------
 * "Connection check" verifies the model file exists and is accessible.
 */

/**
 * Check if the Whisper model is available.
 *
 * This function verifies that the model file specified via
 * whisper_client_set_model_path() exists and is readable. It does NOT
 * load the model — that happens lazily on first transcription.
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 * @return true if the model file exists and is readable, false otherwise.
 */
bool whisper_check_connection(WhisperClient* client);

/**
 * Validate that a file is a valid Whisper model (GGML or GGUF format).
 *
 * This function attempts to load the model using whisper.cpp's own
 * model loader (whisper_init_from_file_with_params), which handles
 * both GGML (.bin) and GGUF (.gguf) formats. The model is immediately
 * freed after validation — it is NOT kept loaded.
 *
 * This is the authoritative check for model validity, as it uses the
 * same code path that will be used during actual transcription.
 *
 * @param model_path Path to the model file to validate.
 * @return true if whisper.cpp can successfully load the model,
 *         false otherwise (file missing, corrupt, wrong format, etc.).
 *
 * @note This function is synchronous and may take several seconds
 *       for large models. Call from a background thread or idle
 *       callback, NOT from the GTK main thread during UI updates.
 */
bool whisper_validate_model_file(const char* model_path);

/*---------------------------------------------------------------------------
 * Section 7: Response Management
 *---------------------------------------------------------------------------
 */

/**
 * Free a WhisperResponse and all associated resources.
 *
 * @param response Pointer to a WhisperResponse. May be NULL (no-op).
 */
void whisper_response_free(WhisperResponse* response);

/*---------------------------------------------------------------------------
 * Section 8: Error Handling and Diagnostics
 *---------------------------------------------------------------------------
 */

/**
 * Get the last error message from the Whisper client.
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 * @return A null-terminated string containing the error message, or an
 *         empty string if no error has occurred.
 */
const char* whisper_client_get_error(WhisperClient* client);

/**
 * Get the error code from the last operation.
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 * @return The error code, or 0 if no error occurred.
 */
int whisper_client_get_error_code(const WhisperClient* client);

/**
 * Request graceful cancellation of an in-flight transcription request.
 *
 * This function sets an atomic cancellation flag that is checked during
 * transcription. When set, the transcription will abort as soon as
 * possible and return an error response.
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 *
 * @thread_safe This function is safe to call from any thread.
 */
void whisper_client_cancel(WhisperClient* client);

/*---------------------------------------------------------------------------
 * Section 11: Model Pre-loading and GPU Management
 *---------------------------------------------------------------------------
 * Functions for pre-loading the model (before first transcription) and
 * managing GPU selection. Pre-loading allows the UI to show a loading
 * indicator rather than blocking during first transcription.
 */

/**
 * Check if the whisper model is currently loaded in memory.
 *
 * @param client Pointer to a valid WhisperClient.
 * @return true if the model has been loaded, false otherwise.
 */
bool whisper_client_is_model_loaded(const WhisperClient *client);

/**
 * Check if the whisper model is currently being loaded (in progress).
 *
 * @param client Pointer to a valid WhisperClient.
 * @return true if loading is in progress, false otherwise.
 */
bool whisper_client_is_loading(const WhisperClient *client);

/**
 * Load the whisper model synchronously (blocking call).
 *
 * This function loads the model into memory so that the first
 * transcription request does not incur loading latency. It handles
 * GPU selection based on the gpu_mode parameter:
 *   - "auto": Select GPU with most free memory, fallback to CPU if insufficient
 *   - "cpu": Force CPU-only processing
 *   - "gpu:N": Use specific GPU device N, fallback to CPU on failure
 *   - NULL: Defaults to "auto"
 *
 * Safe to call from a background thread. Do NOT call from the GTK
 * main thread as this may block for several seconds.
 *
 * @param client   Pointer to a valid WhisperClient. Must not be NULL.
 * @param gpu_mode GPU mode string: "auto", "cpu", or "gpu:N" (may be NULL for auto)
 * @return true if the model was loaded successfully, false on error.
 *         Check whisper_client_get_error() for details on failure.
 *
 * @thread_safe This function is safe to call from any thread.
 */
bool whisper_client_load_model(WhisperClient *client, const char *gpu_mode);

/**
 * Unload the whisper model from memory.
 *
 * Frees the loaded model context so it can be reloaded with
 * different parameters (e.g., different GPU).
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 */
void whisper_client_unload_model(WhisperClient *client);

/*---------------------------------------------------------------------------
 * Section 9: GPU Support
 *---------------------------------------------------------------------------
 * Functions for GPU availability detection and status reporting.
 */

/**
 * Check if CUDA GPU acceleration is available at runtime.
 *
 * This function checks if the application was compiled with CUDA support
 * and if a compatible NVIDIA GPU is detected at runtime.
 *
 * @return true if GPU acceleration is available, false otherwise
 */
bool whisper_gpu_available(void);

/**
 * Get the current GPU usage status of the loaded model.
 *
 * @param client WhisperClient instance
 * @return true if the model is currently running on GPU, false if CPU
 */
bool whisper_client_is_using_gpu(const WhisperClient *client);

/**
 * Get a human-readable description of the current compute backend.
 *
 * @param client WhisperClient instance
 * @return Static string describing the backend (e.g., "GPU (CUDA)" or "CPU")
 */
const char *whisper_client_backend_description(const WhisperClient *client);

/*---------------------------------------------------------------------------
 * Section 10: Model Path Resolution
 *---------------------------------------------------------------------------
 * Utility for resolving model paths with tilde expansion and directory search.
 */

/**
 * Resolve a model path with tilde expansion and directory search.
 *
 * This function handles the following path formats:
 *   - Absolute paths (e.g., "/home/user/.cache/whisper/ggml-base.bin")
 *   - Tilde paths (e.g., "~/.cache/whisper/ggml-base.bin")
 *   - Bare filenames (e.g., "ggml-base.bin") - searched in default directories
 *
 * Default search directories:
 *   - ~/.cache/whisper/
 *   - /usr/share/transcriber/models/
 *
 * @param input Input path (may contain ~ or be a bare filename)
 * @param output Buffer for resolved absolute path
 * @param out_size Size of output buffer
 * @return 0 on success (file found), -1 if file not found (output still contains best guess)
 */
int whisper_resolve_model_path(const char *input, char *output, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* APP_WHISPER_H */
