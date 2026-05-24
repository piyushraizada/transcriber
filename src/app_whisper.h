#ifndef APP_WHISPER_H
#define APP_WHISPER_H

/**
 * @file app_whisper.h
 * @brief Whisper API integration — HTTP POST transcription, JSON parsing, connection check
 *
 * This module implements the network communication layer for sending audio
 * recordings to an OpenAI-compatible Whisper API server for transcription.
 * It handles all aspects of the HTTP transaction including:
 *
 *   - Multipart/form-data encoding of WAV files (WHISPER-002, WHISPER-003)
 *   - Language parameter injection (WHISPER-005)
 *   - Connection health checks and model discovery via GET /v1/models (WHISPER-013)
 *   - JSON response parsing for text extraction (WHISPER-006, WHISPER-007)
 *   - Exponential backoff retry on transient errors (WHISPER-010)
 *   - Secure memory handling for sensitive transcription data (NR-010)
 *
 * The module uses libcurl for HTTP operations and cJSON for JSON parsing.
 * All network operations are designed to run in a dedicated network thread
 * (separate from the GTK main thread and audio thread) to prevent blocking
 * the UI during transcription requests.
 *
 * @see SRS Section 6: Whisper API Integration (WHISPER-001 through WHISPER-013)
 * @see SRS Section 4: Non-Functional Requirements (NR-002, NR-010, NR-015, NR-017, NR-018)
 * @see SRS Section 2.1: Threading Model — Network Thread
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
 * Contains the parsed result from a Whisper API transcription request.
 *
 * This structure holds the transcription text extracted from the JSON
 * response, along with metadata about the response status. The text
 * field is dynamically allocated and must be freed using
 * whisper_response_free() when no longer needed.
 *
 * The structure is created by whisper_transcribe() and returned to the
 * caller. The caller takes ownership of the response and is responsible
 * for freeing it.
 *
 * @see WHISPER-006: JSON Parsing
 * @see WHISPER-007: Text Extraction
 */
typedef struct {
    char* text;              ///< Transcription text (dynamically allocated, NULL on error)
    int http_status;         ///< HTTP response status code (0 if no response received)
    bool success;            ///< true if transcription succeeded (HTTP 200 + valid JSON)
    char error_message[256]; ///< Human-readable error message (empty if success)
} WhisperResponse;

/*---------------------------------------------------------------------------
 * Section 2: Whisper Client Handle (Opaque)
 *---------------------------------------------------------------------------
 * Opaque handle to the internal Whisper client state. The actual structure
 * contains the libcurl easy handle, connection configuration (URL, timeout),
 * and thread synchronization primitives.
 *
 * Users of this module should NEVER access the internal fields directly.
 * All interaction must go through the public API functions defined below.
 *
 * The client is designed to be reused across multiple transcription requests.
 * The libcurl handle is initialized once during whisper_client_create() and
 * reused for each request, which enables connection pooling and reduces
 * latency for subsequent requests.
 *
 * @see SRS Section 2.1: Threading Model — Network Thread
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
 * initializes the internal libcurl easy handle. The client is configured
 * with default timeout values and an empty URL.
 *
 * The function performs the following initialization steps:
 *   1. Allocate WhisperClient struct via malloc()
 *   2. Initialize libcurl easy handle via curl_easy_init()
 *   3. Set default timeout values (30s connect, 60s total)
 *   4. Initialize state fields (URL = empty, model = auto-discovered)
 *   5. Initialize mutex for thread-safe access
 *
 * @return A valid WhisperClient* on success, or NULL on allocation/curl init failure.
 *         The caller is responsible for calling whisper_client_destroy()
 *         when finished.
 *
 * @thread_safe The returned client uses a mutex for concurrent request protection.
 * @see WHISPER-009: Timeout Configuration (30s connect, 60s total)
 */
WhisperClient* whisper_client_create(void);

/**
 * Destroy a Whisper client and free all associated resources.
 *
 * This function performs a complete cleanup of the client, including:
 *   1. Cleanup libcurl easy handle via curl_easy_cleanup()
 *   2. Free any pending response data
 *   3. Scrub sensitive data (URL, tokens) from memory using explicit_bzero()
 *   4. Destroy the state mutex
 *   5. Free the WhisperClient struct via free()
 *
 * After calling this function, the client pointer is invalid and must
 * NOT be used again.
 *
 * @param client Pointer to a valid WhisperClient created by whisper_client_create().
 *               Must not be NULL.
 *
 * @pre client != NULL
 * @post client is invalid (freed)
 * @see NR-010: Memory Buffer Scrubbing
 */
void whisper_client_destroy(WhisperClient* client);

/*---------------------------------------------------------------------------
 * Section 4: Configuration
 *---------------------------------------------------------------------------
 * Functions for configuring the Whisper client connection parameters.
 */

/**
 * Set the Whisper API server URL.
 *
 * This function sets the base URL of the Whisper API server. The URL
 * should include the protocol and host (and optionally port), but should
 * NOT include the endpoint path. The endpoint path (/v1/audio/transcriptions)
 * is appended automatically by whisper_transcribe().
 *
 * Example URLs:
 *   - "http://localhost:8000"  (local Whisper server)
 *   - "https://api.openai.com" (OpenAI cloud)
 *   - "http://192.168.1.100:9000" (network Whisper server)
 *
 * The URL is stored internally and used for all subsequent transcription
 * and connection check requests.
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 * @param url    The base URL of the Whisper API server. Must not be NULL.
 *               The string is copied internally, so the caller may free
 *               the original string after this call returns.
 *
 * @return true if the URL was set successfully, false if the URL is
 *         invalid (e.g., too long, invalid format).
 *
 * @see WHISPER-001: Endpoint Configuration
 * @see CFG-005: Whisper URL Configuration
 */
bool whisper_client_set_url(WhisperClient* client, const char* url);

/**
 * Set the language for transcription.
 *
 * This function sets the language parameter that will be included in the
 * multipart/form-data request. The language is specified as an ISO 639-1
 * two-letter code (e.g., "en", "es", "fr").
 *
 * If the language is set to an empty string or NULL, the Whisper model
 * will attempt auto-detection of the spoken language.
 *
 * @param client   Pointer to a valid WhisperClient. Must not be NULL.
 * @param language The language code (ISO 639-1), or NULL for auto-detection.
 *                 The string is copied internally.
 *
 * @return true if the language was set successfully, false if invalid.
 *
 * @see WHISPER-005: Language Parameter
 * @see CFG-007: Language Configuration
 */
bool whisper_client_set_language(WhisperClient* client, const char* language);

/**
 * Set the connection timeout in seconds.
 *
 * This function sets the maximum time allowed for the connection phase
 * (TCP handshake + TLS negotiation). If the connection cannot be
 * established within this time, the request fails with a timeout error.
 *
 * @param client      Pointer to a valid WhisperClient. Must not be NULL.
 * @param timeout_sec Connection timeout in seconds (minimum 1, maximum 120).
 *
 * @return true if the timeout was set successfully, false if out of range.
 *
 * @see WHISPER-009: Timeout Configuration
 */
bool whisper_client_set_connect_timeout(WhisperClient* client, int timeout_sec);

/**
 * Set the total request timeout in seconds.
 *
 * This function sets the maximum time allowed for the entire request
 * (connection + upload + processing + download). If the request does
 * not complete within this time, libcurl aborts the operation.
 *
 * @param client      Pointer to a valid WhisperClient. Must not be NULL.
 * @param timeout_sec Total timeout in seconds (minimum 5, maximum 300).
 *
 * @return true if the timeout was set successfully, false if out of range.
 *
 * @see WHISPER-009: Timeout Configuration
 */
bool whisper_client_set_total_timeout(WhisperClient* client, int timeout_sec);

/*---------------------------------------------------------------------------
 * Section 5: Core Transcription API
 *---------------------------------------------------------------------------
 * The primary function for sending audio to the Whisper API and retrieving
 * the transcription text.
 *
 * This function performs the complete transcription workflow:
 *   1. Read the WAV file into memory
 *   2. Build a multipart/form-data request with the audio file
 *   3. POST to /v1/audio/transcriptions endpoint
 *   4. Parse the JSON response
 *   5. Extract the transcription text
 *   6. Return the result in a WhisperResponse struct
 *
 * @see WHISPER-001 through WHISPER-012: All Whisper API requirements
 */

/**
 * Send a WAV file to the Whisper API for transcription.
 *
 * This function is the core transcription operation. It reads the WAV file,
 * encodes it as multipart/form-data, and POSTs it to the Whisper API
 * endpoint. The function blocks until the transcription is complete or
 * a timeout/error occurs.
 *
 * The function performs the following steps:
 *   1. Validate the WAV file exists and is readable
 *   2. Read the entire WAV file into a memory buffer
 *   3. Build a multipart/form-data request with:
 *      - file: The WAV audio data (field name: "file")
 *      - model: Auto-discovered from server via GET /v1/models
 *      - language: The configured language (if set)
 *   4. Set the Content-Type header (multipart/form-data with boundary)
 *   5. POST to {url}/v1/audio/transcriptions
 *   6. Receive the HTTP response
 *   7. Parse the JSON response body
 *   8. Extract the "text" field from the JSON
 *   9. Populate a WhisperResponse struct with the result
 *   10. Scrub the audio buffer from memory using explicit_bzero()
 *
 * The function handles HTTP errors gracefully:
 *   - 4xx errors: Client error (bad request, unauthorized, etc.) — no retry
 *   - 5xx errors: Server error — may retry with backoff (see whisper_transcribe_with_retry())
 *   - Network errors: Connection refused, timeout — may retry with backoff
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 * @param wav_path Path to the WAV file to transcribe. Must not be NULL.
 *
 * @return A pointer to a WhisperResponse struct on success (allocated by
 *         this function). The caller MUST free the response using
 *         whisper_response_free() when done. Returns NULL on critical
 *         failure (e.g., memory allocation failure).
 *
 * @pre client != NULL && wav_path != NULL
 * @post Caller owns the response and must call whisper_response_free()
 *
 * @thread_safe This function may be called from any thread. The internal
 *              mutex protects the libcurl handle for concurrent access.
 *
 * @see WHISPER-001: Endpoint Configuration (/v1/audio/transcriptions)
 * @see WHISPER-002: Content-Type (multipart/form-data)
 * @see WHISPER-003: File Field Name ("file")
 * @see WHISPER-004: Model Parameter
 * @see WHISPER-005: Language Parameter
 * @see WHISPER-006: JSON Parsing
 * @see WHISPER-007: Text Extraction
 * @see WHISPER-008: Error Response Handling
 * @see WHISPER-012: Upload Efficiency (entire file in single POST)
 * @see NR-010: Memory Buffer Scrubbing (audio buffer after upload)
 */
WhisperResponse* whisper_transcribe(WhisperClient* client, const char* wav_path);

/**
 * Send a WAV file for transcription with automatic retry on transient errors.
 *
 * This function wraps whisper_transcribe() with exponential backoff retry
 * logic. It retries the transcription request on transient errors (5xx
 * status codes, network timeouts, connection refused) up to a maximum
 * number of attempts.
 *
 * Retry behavior:
 *   - Max attempts: Configurable (default 3)
 *   - Initial delay: 1 second
 *   - Backoff multiplier: 2x (1s, 2s, 4s, ...)
 *   - Max delay: 16 seconds
 *   - Retry conditions: HTTP 5xx, curl timeout, connection refused
 *   - No retry on: HTTP 4xx (client error), invalid JSON, memory errors
 *
 * The function returns the response from the last attempt (whether
 * successful or not). The caller can check response->success to determine
 * if the final attempt succeeded.
 *
 * @param client         Pointer to a valid WhisperClient. Must not be NULL.
 * @param wav_path       Path to the WAV file to transcribe. Must not be NULL.
 * @param max_retries    Maximum number of retry attempts (0 = no retry, 1-5 recommended).
 *
 * @return A pointer to a WhisperResponse struct (allocated by this function).
 *         The caller MUST free the response using whisper_response_free().
 *         Returns NULL on critical failure.
 *
 * @see WHISPER-010: Retry Logic
 * @see NR-015: HTTP Retry Filtering by Status Code
 */
WhisperResponse* whisper_transcribe_with_retry(WhisperClient* client, const char* wav_path, int max_retries);

/*---------------------------------------------------------------------------
 * Section 6: Connection Health Check
 *---------------------------------------------------------------------------
 * Functions for checking the Whisper API server connection status.
 *
 * The connection check uses the GET /v1/models endpoint, which is a
 * lightweight operation that verifies the server is reachable and
 * responding. On success, it also extracts the first model ID from the
 * response and stores it internally for use in transcription requests.
 * This is used at application startup and when the user
 * manually triggers a connection check via the status indicator.
 *
 * @see WHISPER-013: Connection Check and Model Discovery via GET /v1/models
 * @see FR-030: LLM Connection on Startup
 * @see FR-037: Manual Connection Check
 */

/**
 * Check the connection to the Whisper API server.
 *
 * This function sends a GET request to the /v1/models endpoint to verify
 * that the Whisper API server is reachable and responding. The function
 * does NOT perform transcription — it only checks connectivity and
 * discovers the available model.
 *
 * The function performs the following steps:
 *   1. Construct the URL: {base_url}/v1/models
 *   2. Send a GET request with a short timeout (5 seconds)
 *   3. Check the HTTP response status code
 *   4. Parse the JSON response to verify it contains a "data" array
 *   5. Extract the first model ID and store it internally
 *   6. Return the connection status
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 * @return true if the server responded with HTTP 200 and valid JSON,
 *         false if the server is unreachable or returned an error.
 *
 * @thread_safe May be called from any thread.
 *
 * @see WHISPER-013: Connection Check and Model Discovery via GET /v1/models
 * @see FR-030: LLM Connection on Startup
 */
bool whisper_check_connection(WhisperClient* client);

/*---------------------------------------------------------------------------
 * Section 7: Response Management
 *---------------------------------------------------------------------------
 * Functions for managing WhisperResponse structs.
 */

/**
 * Free a WhisperResponse and all associated resources.
 *
 * This function frees the dynamically allocated text field and the
 * response struct itself. After calling this function, the response
 * pointer is invalid and must NOT be used again.
 *
 * The function also scrubs the transcription text from memory using
 * explicit_bzero() before freeing, to prevent sensitive data from
 * lingering in memory.
 *
 * @param response Pointer to a WhisperResponse. May be NULL (no-op).
 *
 * @see NR-010: Memory Buffer Scrubbing
 */
void whisper_response_free(WhisperResponse* response);

/*---------------------------------------------------------------------------
 * Section 8: Error Handling and Diagnostics
 *---------------------------------------------------------------------------
 * Functions for retrieving error information and diagnostic data.
 */

/**
 * Get the last error message from the Whisper client.
 *
 * This function returns a human-readable error message describing the
 * last error that occurred during a transcription or connection check
 * operation. The error message includes the HTTP status code (if
 * applicable) and a description of the error.
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 * @return A null-terminated string containing the error message, or an
 *         empty string if no error has occurred. The returned string is
 *         internal to the client and must NOT be freed.
 */
const char* whisper_client_get_error(WhisperClient* client);

/**
 * Get the libcurl error code from the last operation.
 *
 * This function returns the raw libcurl error code (CURLcode) from the
 * last operation. This can be used for detailed error handling beyond
 * what is provided by whisper_client_get_error().
 *
 * @param client Pointer to a valid WhisperClient. Must not be NULL.
 * @return The libcurl error code, or CURLE_OK (0) if no error occurred.
 *         See libcurl documentation for error code definitions.
 */
int whisper_client_get_curl_error(const WhisperClient* client);

#ifdef __cplusplus
}
#endif

#endif /* APP_WHISPER_H */
