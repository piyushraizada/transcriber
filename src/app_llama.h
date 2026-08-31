/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_LLAMA_H
#define APP_LLAMA_H

/**
 * @file app_llama.h
 * @brief llama-server HTTP ASR client (Gemma 4 12B and compatible chat models)
 *
 * This module implements a second, network-based speech-to-text backend for
 * Transcriber: any model served by a llama.cpp `llama-server` over its
 * OpenAI-compatible HTTP API (e.g. the Gemma 4 12B Unified model with its
 * audio input via --mmproj).
 *
 * How it works:
 *   - 16 kHz mono int16 PCM (the same format the audio pipeline and the
 *     Whisper backend consume) is wrapped in a minimal in-memory WAV header,
 *     base64-encoded, and sent as an `input_audio` content part in a
 *     `POST /v1/chat/completions` request.
 *   - The server's `choices[0].message.content` string is returned as the
 *     transcription.
 *   - Results are returned in the shared WhisperResponse struct so all
 *     downstream result handling (UI marshalling, error display) is identical
 *     to the Whisper backend.
 *
 * Auto-fallback:
 *   - The `*_fallback` variants retry the same input through the local
 *     Whisper client when the llama request fails (connect error, timeout,
 *     HTTP error, parse error). A user-initiated cancellation is NOT
 *     retried, and a successful empty-text response (silence) is a valid
 *     result and never triggers a fallback.
 *
 * Threading:
 *   - Every function may be called from any thread.
 *   - Each request creates its own TCP connection (stateless), and request-
 *     scoped timeouts are enforced by a private canceller thread, so no
 *     GLib main loop is required on the calling thread.
 *   - At most one in-flight request is tracked per client for cancellation;
 *     the application serializes transcription calls with its scanner mutex,
 *     so requests from this app never overlap in practice.
 *   - llama_client_destroy() must not be called while a request from
 *     another thread is in flight (same contract as
 *     whisper_client_destroy()).
 *
 * Lifecycle:
 *   - Create with llama_client_create() (holds server URL + model alias).
 *   - URL/model can be re-pointed at runtime with llama_client_set_url() /
 *     llama_client_set_model() (e.g. when the user edits the config).
 *   - No server state is cached; there is nothing to pre-load.
 *
 * @see plans/llama-server-asr-backend-plan.md
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "app_whisper.h"  /* WhisperResponse — shared ASR result type */

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Section 0: Constants
 *---------------------------------------------------------------------------*/

/** Fixed input sample rate — the audio pipeline always delivers 16 kHz mono. */
#define LLAMA_SAMPLE_RATE 16000

/** TCP connect timeout in seconds (GSocketClient connect attempts). */
#define LLAMA_CONNECT_TIMEOUT_SEC 5

/** Total read deadline in seconds (connect + full response). A busy
 *  single-slot server queues requests; this covers queued wait plus
 *  generation of a full 30 s segment. */
#define LLAMA_READ_TIMEOUT_SEC 120

/** Max generation tokens per request. Longest input is a 30 s segment
 *  (~6 tokens/s of audio observed); 1024 is >= 8x headroom and well under
 *  the server's --n-predict limit. */
#define LLAMA_MAX_TOKENS_PER_SEGMENT 1024

/** Sanity cap on PCM samples per request (10 minutes at 16 kHz). Guards
 *  against corrupt counts, not a real limit. */
#define LLAMA_MAX_SAMPLES (LLAMA_SAMPLE_RATE * 600)

/** Caller buffer capacity for llama_list_models(): how many advertised
 *  model names the caller can receive. llama-server advertises one model
 *  in single-model mode and a handful in router mode — 8 is far above any
 *  realistic server; extra entries are dropped. */
#define LLAMA_MAX_MODELS 8

/** Max length (including NUL) of one advertised model name. */
#define LLAMA_MAX_MODEL_NAME 128

/*---------------------------------------------------------------------------
 * Section 1: Error Codes
 *---------------------------------------------------------------------------
 * Consistent, non-overlapping error codes for all llama module functions.
 * Stored in WhisperResponse.error_code on failure.
 */
typedef enum {
    LLAMA_ERR_OK                =  0,  ///< Success
    LLAMA_ERR_INVALID_PARAM     =  1,  ///< NULL/invalid argument, or unreadable/unusable WAV file
    LLAMA_ERR_CONNECT           =  2,  ///< Connect failure (refused, DNS, connect timeout, socket reset)
    LLAMA_ERR_READ_TIMEOUT      =  3,  ///< Server did not answer within LLAMA_READ_TIMEOUT_SEC
    LLAMA_ERR_HTTP              =  4,  ///< Non-2xx HTTP status
    LLAMA_ERR_RESPONSE_PARSE    =  5,  ///< Body not valid JSON / missing expected fields
    LLAMA_ERR_ALLOC             =  6,  ///< Memory allocation failure
    LLAMA_ERR_CANCELLED         =  7,  ///< Request cancelled via llama_client_cancel()
} llama_error_code;

/*---------------------------------------------------------------------------
 * Section 2: LlamaClient Handle (Opaque)
 *---------------------------------------------------------------------------
 * Opaque handle to the client state: server URL, model alias, in-flight
 * request tracking (for cancellation), and the last error message.
 */
typedef struct _LlamaClient LlamaClient;

/*---------------------------------------------------------------------------
 * Section 3: Initialization and Cleanup
 *---------------------------------------------------------------------------*/

/**
 * Create and initialize a new llama-server client.
 *
 * @param server_url Base URL, e.g. "http://127.0.0.1:8005". May be NULL or
 *                   empty — the client is created unconfigured and can be
 *                   pointed at a server later with llama_client_set_url().
 *                   Trailing slashes are normalized away.
 * @param model      Model alias as exposed by the server (--alias), e.g.
 *                   "gemma-4-12b". May be NULL (see above).
 * @return A valid LlamaClient* on success, or NULL on allocation failure.
 */
LlamaClient* llama_client_create(const char* server_url, const char* model);

/**
 * Destroy a llama client and free all associated resources.
 *
 * MUST NOT be called while a request from another thread is in flight
 * (same contract as whisper_client_destroy()). An in-flight request is
 * cancelled first.
 *
 * @param client Pointer to a valid LlamaClient. May be NULL (no-op).
 */
void llama_client_destroy(LlamaClient* client);

/*---------------------------------------------------------------------------
 * Section 4: Configuration
 *---------------------------------------------------------------------------*/

/**
 * Re-point the client at a different server URL at runtime.
 * Trailing slashes are stripped. A NULL/empty string clears the URL.
 * An overly long string is rejected (previous value kept, warning logged).
 *
 * @param client     Pointer to a valid LlamaClient. Must not be NULL.
 * @param server_url New base URL (copied internally).
 */
void llama_client_set_url(LlamaClient* client, const char* server_url);

/**
 * Change the model alias at runtime.
 * A NULL/empty string clears the model. Overly long values are rejected.
 *
 * @param client Pointer to a valid LlamaClient. Must not be NULL.
 * @param model  New model alias (copied internally).
 */
void llama_client_set_model(LlamaClient* client, const char* model);

/*---------------------------------------------------------------------------
 * Section 5: Connection Health Check
 *---------------------------------------------------------------------------*/

/**
 * Check whether the llama-server is reachable and healthy.
 *
 * Issues a `GET {server}/health` request. Returns true only on HTTP 200
 * with a body containing "ok" (llama-server's health response).
 * Safe to call from any thread; on failure the error is logged and
 * retrievable via llama_client_get_error().
 *
 * @param client Pointer to a valid LlamaClient. Must not be NULL.
 * @return true if the server reported healthy, false otherwise.
 */
bool llama_check_connection(LlamaClient* client);

/**
 * Query the models the server advertises (`GET {server}/v1/models`).
 *
 * llama-server advertises exactly one entry in single-model mode (the
 * first `--alias`, or the model name) and one entry per hosted model in
 * router (multi-model) mode. Both response shapes are accepted: the
 * llama.cpp `{"models": [...]}` key and the OpenAI-compatible
 * `{"data": [...]}` key used by gateways.
 *
 * Safe to call from any thread; on failure the error is logged and
 * retrievable via llama_client_get_error().
 *
 * @param client    Pointer to a valid LlamaClient. Must not be NULL.
 * @param names_out Caller buffer for the names, at most LLAMA_MAX_MODELS
 *                  rows of LLAMA_MAX_MODEL_NAME bytes. Must not be NULL.
 *                  Names longer than LLAMA_MAX_MODEL_NAME - 1 are
 *                  truncated.
 * @param max_names Number of rows in names_out (1..LLAMA_MAX_MODELS).
 * @return Number of advertised models on a successful HTTP 200 response
 *         (may be 0 for a valid empty list), or -1 on transport, HTTP,
 *         or JSON failure.
 */
int llama_list_models(LlamaClient* client,
                      char (*names_out)[LLAMA_MAX_MODEL_NAME],
                      int max_names);

/*---------------------------------------------------------------------------
 * Section 6: Core Transcription API
 *---------------------------------------------------------------------------*/

/**
 * Transcribe 16 kHz mono int16 PCM samples via the llama-server.
 *
 * The samples are wrapped in a minimal WAV header, base64-encoded, and sent
 * as an audio content part of a chat completion request. The prompt is
 * "Transcribe this audio.", or "Transcribe this audio in <language>." when
 * language is set to a concrete ISO 639-1 code ("auto" or empty means no
 * language hint).
 *
 * @param client    Pointer to a valid LlamaClient. Must not be NULL.
 * @param samples   Pointer to int16_t PCM samples (16 kHz, mono).
 * @param n_samples Number of samples. Must be > 0 and <= LLAMA_MAX_SAMPLES.
 * @param language  ISO 639-1 code ("en", "fr", ...), "auto", empty, or NULL
 *                 for automatic language detection by the model.
 *
 * @return A pointer to a WhisperResponse (allocated by this function).
 *         The caller MUST free it with whisper_response_free().
 *         Returns NULL only on critical allocation failure.
 *
 * Note: success with an EMPTY text is a valid result (the segment was
 * silence) — it is not an error and must not be treated as a failure.
 *
 * @thread_safe Safe to call from any thread.
 */
WhisperResponse* llama_transcribe_samples(LlamaClient* client,
                                          const int16_t* samples,
                                          int n_samples,
                                          const char* language);

/**
 * Transcribe a WAV file on disk via the llama-server.
 *
 * The file must be a 16 kHz mono 16-bit PCM WAV (the format the app's
 * recorder writes); the whole file is sent to the server as-is.
 *
 * @param client   Pointer to a valid LlamaClient. Must not be NULL.
 * @param wav_path Path to the WAV file. Must not be NULL or empty.
 * @param language Language code handling as in llama_transcribe_samples().
 *
 * @return A pointer to a WhisperResponse. Caller frees with
 *         whisper_response_free(). NULL only on critical allocation failure.
 *
 * @thread_safe Safe to call from any thread.
 */
WhisperResponse* llama_transcribe_wav(LlamaClient* client,
                                      const char* wav_path,
                                      const char* language);

/*---------------------------------------------------------------------------
 * Section 7: Transcription with Auto-Fallback to Local Whisper
 *---------------------------------------------------------------------------
 * The approved failure policy: when the llama request fails (transport,
 * timeout, HTTP, or parse error), the SAME input is re-sent through the
 * local Whisper client and that result is returned. The original llama
 * error is logged at WARNING level before the fallback. A user-initiated
 * cancellation (LLAMA_ERR_CANCELLED) is NOT retried.
 */

/**
 * Transcribe PCM with automatic fallback to the local Whisper client.
 *
 * @param client    LlamaClient for the primary attempt. Must not be NULL.
 * @param whisper   WhisperClient used ONLY for the fallback (borrowed, not
 *                  owned; it lazy-loads its model on first use). May be
 *                  NULL, in which case failures are returned as-is.
 * @param samples   16 kHz mono int16 PCM samples.
 * @param n_samples Number of samples (> 0).
 * @param language  Language code handling as in llama_transcribe_samples().
 *
 * @return A pointer to a WhisperResponse owned by the caller. If the
 *         fallback ran, this is the WHISPER response (success or failure);
 *         the llama error remains in the log and in
 *         llama_client_get_error().
 *
 * @thread_safe Safe to call from any thread.
 */
WhisperResponse* llama_transcribe_samples_fallback(LlamaClient* client,
                                                   WhisperClient* whisper,
                                                   const int16_t* samples,
                                                   int n_samples,
                                                   const char* language);

/**
 * Transcribe a WAV file with automatic fallback to the local Whisper client.
 * See llama_transcribe_samples_fallback() for the fallback semantics.
 *
 * @param client   LlamaClient for the primary attempt. Must not be NULL.
 * @param whisper  WhisperClient for the fallback (borrowed). May be NULL.
 * @param wav_path Path to a 16 kHz mono 16-bit PCM WAV file.
 * @param language Language code handling as in llama_transcribe_samples().
 *
 * @return A pointer to a WhisperResponse owned by the caller.
 *
 * @thread_safe Safe to call from any thread.
 */
WhisperResponse* llama_transcribe_wav_fallback(LlamaClient* client,
                                               WhisperClient* whisper,
                                               const char* wav_path,
                                               const char* language);

/*---------------------------------------------------------------------------
 * Section 8: Cancellation
 *---------------------------------------------------------------------------*/

/**
 * Request cancellation of an in-flight request.
 *
 * The request aborts as soon as possible and returns a response with
 * LLAMA_ERR_CANCELLED. Safe no-op when no request is in flight.
 * NOTE: the `*_fallback` wrappers deliberately do NOT retry a cancelled
 * request through Whisper.
 *
 * @param client Pointer to a valid LlamaClient. Must not be NULL.
 * @thread_safe Safe to call from any thread.
 */
void llama_client_cancel(LlamaClient* client);

/*---------------------------------------------------------------------------
 * Section 9: Error Handling and Diagnostics
 *---------------------------------------------------------------------------*/

/**
 * Get the last error message from the llama client.
 *
 * @param client Pointer to a valid LlamaClient.
 * @return A null-terminated error string (thread-local buffer), or an empty
 *         string if no error has occurred. Must NOT be freed.
 */
const char* llama_client_get_error(const LlamaClient* client);

#ifdef __cplusplus
}
#endif

#endif /* APP_LLAMA_H */
