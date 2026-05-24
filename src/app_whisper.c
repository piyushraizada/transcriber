/*
 * app_whisper.c — Whisper API integration
 *
 * HTTP POST transcription, JSON parsing, connection health checks.
 * Uses libcurl for HTTP and cJSON for JSON parsing.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_whisper.h"
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <math.h>
#include <sys/random.h>  /* getrandom() for ME-02 fix */

#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

/* ===================================================================
 * Secure memory scrubbing
 * =================================================================== */
#if defined(HAVE_STRING_H) && defined(HAVE_MEMSET_S)
  #define scrub_memory(ptr, len) memset_s(ptr, len, 0, len)
#elif defined(HAVE_EXPLICIT_BZERO)
  #define scrub_memory(ptr, len) explicit_bzero(ptr, len)
#else
  /* Fallback: volatile write to prevent optimization */
  static void scrub_memory(void *ptr, size_t len) {
      volatile unsigned char *p = (volatile unsigned char *)ptr;
      while (len--) {
          *p++ = 0;
      }
  }
#endif

/* Maximum URL length */
#define MAX_URL_LEN 1024

/* Maximum model name length */
#define MAX_MODEL_LEN 256

/* Maximum language code length */
#define MAX_LANGUAGE_LEN 16

/* Default timeouts */
#define DEFAULT_CONNECT_TIMEOUT 30
#define DEFAULT_TOTAL_TIMEOUT 60

/* Maximum retry delay in seconds */
#define MAX_RETRY_DELAY 16

/* Include <unistd.h> for usleep (jitter) */
#include <unistd.h>

/* Internal WhisperClient struct */
struct _WhisperClient {
    CURL *curl;
    char url[MAX_URL_LEN];
    char model[MAX_MODEL_LEN];
    char language[MAX_LANGUAGE_LEN];
    int connect_timeout;
    int total_timeout;
    char error_message[256];
    int curl_error_code;
    pthread_mutex_t mutex;
};

/* ===================================================================
 * Response management
 * =================================================================== */

/**
 * whisper_response_free
 * Frees a WhisperResponse and scrubs sensitive data.
 *
 * @param response Response to free (may be NULL)
 */
void whisper_response_free(WhisperResponse *response) {
    if (!response) return;

    /* Scrub and free transcription text */
    if (response->text) {
        size_t len = strlen(response->text);
        scrub_memory(response->text, len);
        free(response->text);
        response->text = NULL;
    }

    /* Scrub error message */
    scrub_memory(response->error_message, sizeof(response->error_message));

    free(response);
}

/* ===================================================================
 * Internal helpers
 * =================================================================== */

/* M1-001 fix: Renamed to set_client_error_locked to make it explicit that
 * callers MUST hold client->mutex before calling this function. */
static void set_client_error_locked(WhisperClient *client, const char *msg) {
    if (!client) return;
    if (msg) {
        strncpy(client->error_message, msg, sizeof(client->error_message) - 1);
        client->error_message[sizeof(client->error_message) - 1] = '\0';
    } else {
        client->error_message[0] = '\0';
    }
}

/* Callback for receiving HTTP response data */
struct MemoryChunk {
    char *data;
    size_t size;
    size_t capacity;
};

static size_t memory_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryChunk *chunk = (struct MemoryChunk *)userp;

    size_t new_capacity = chunk->capacity + realsize + 1024;
    char *new_data = (char *)realloc(chunk->data, new_capacity);
    if (!new_data) {
        return 0;  /* Out of memory */
    }

    chunk->data = new_data;
    memcpy(chunk->data + chunk->size, contents, realsize);
    chunk->size += realsize;
    chunk->capacity = new_capacity;
    chunk->data[chunk->size] = '\0';

    return realsize;
}

/* Callback for receiving HTTP response headers */
struct HeaderChunk {
    char *data;
    size_t size;
    size_t capacity;
};

static size_t header_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct HeaderChunk *hdr = (struct HeaderChunk *)userp;

    size_t new_capacity = hdr->capacity + realsize + 1;
    char *new_data = (char *)realloc(hdr->data, new_capacity);
    if (!new_data) return 0;

    hdr->data = new_data;
    memcpy(hdr->data + hdr->size, contents, realsize);
    hdr->size += realsize;
    hdr->capacity = new_capacity;
    hdr->data[hdr->size] = '\0';

    return realsize;
}

/* Extract the base URL (scheme + host + port) from a full endpoint URL */
static void extract_base_url(const char *url, char *out, size_t out_size) {
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        snprintf(out, out_size, "%s", url);
        return;
    }
    const char *path_start = strchr(scheme_end + 3, '/');
    if (path_start) {
        size_t len = (size_t)(path_start - url);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, url, len);
        out[len] = '\0';
    } else {
        snprintf(out, out_size, "%s", url);
    }
}

static char * read_file_to_buffer(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0) {
        fclose(f);
        return NULL;
    }

    char *buffer = (char *)malloc((size_t)fsize + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buffer, 1, (size_t)fsize, f);
    fclose(f);

    buffer[read] = '\0';
    *out_size = read;
    return buffer;
}

/* ===================================================================
 * Lifecycle
 * =================================================================== */

/**
 * whisper_client_create
 * Creates and initializes a new WhisperClient.
 *
 * @return Valid WhisperClient* on success, NULL on failure
 */
WhisperClient * whisper_client_create(void) {
    /* Note: curl_global_init() is now called once in main() startup.
     * Do NOT call it here to avoid multiple initializations. */

    WhisperClient *client = (WhisperClient *)calloc(1, sizeof(WhisperClient));
    if (!client) return NULL;

    client->curl = curl_easy_init();
    if (!client->curl) {
        free(client);
        return NULL;
    }

    /* Default configuration */
    client->url[0] = '\0';
    client->model[0] = '\0';  /* Will be auto-discovered from server */
    client->language[0] = '\0';
    client->connect_timeout = DEFAULT_CONNECT_TIMEOUT;
    client->total_timeout = DEFAULT_TOTAL_TIMEOUT;
    client->error_message[0] = '\0';
    client->curl_error_code = CURLE_OK;

    pthread_mutex_init(&client->mutex, NULL);

    return client;
}

/**
 * whisper_client_destroy
 * Frees all resources associated with a WhisperClient.
 *
 * @param client Client to destroy (must not be NULL)
 */
void whisper_client_destroy(WhisperClient *client) {
    if (!client) return;


    if (client->curl) {
        curl_easy_cleanup(client->curl);
        client->curl = NULL;
    }

    /* Scrub sensitive data */
    scrub_memory(client->url, sizeof(client->url));
    scrub_memory(client->model, sizeof(client->model));
    scrub_memory(client->language, sizeof(client->language));
    scrub_memory(client->error_message, sizeof(client->error_message));

    pthread_mutex_destroy(&client->mutex);
    free(client);
}

/* ===================================================================
 * Configuration
 * =================================================================== */

/**
 * whisper_client_set_url
 * Sets the Whisper API base URL.
 *
 * @param client Client handle
 * @param url Base URL (e.g., "http://localhost:8000")
 * @return true on success
 */
bool whisper_client_set_url(WhisperClient *client, const char *url) {
    if (!client || !url) return false;

    pthread_mutex_lock(&client->mutex);
    if (strlen(url) >= MAX_URL_LEN) {
        set_client_error_locked(client, "URL too long");
        pthread_mutex_unlock(&client->mutex);
        return false;
    }
    strncpy(client->url, url, sizeof(client->url) - 1);
    client->url[sizeof(client->url) - 1] = '\0';
    pthread_mutex_unlock(&client->mutex);
    return true;
}

/**
 * whisper_client_set_language
 * Sets the transcription language.
 *
 * @param client Client handle
 * @param language ISO 639-1 code (e.g., "en"), or NULL for auto-detect
 * @return true on success
 */
bool whisper_client_set_language(WhisperClient *client, const char *language) {
    if (!client) return false;

    pthread_mutex_lock(&client->mutex);
    if (language) {
        if (strlen(language) >= MAX_LANGUAGE_LEN) {
            set_client_error_locked(client, "Language code too long");
            pthread_mutex_unlock(&client->mutex);
            return false;
        }
        strncpy(client->language, language, sizeof(client->language) - 1);
        client->language[sizeof(client->language) - 1] = '\0';
    } else {
        client->language[0] = '\0';
    }
    pthread_mutex_unlock(&client->mutex);
    return true;
}

/**
 * whisper_client_set_connect_timeout
 * Sets the connection timeout in seconds.
 *
 * @param client Client handle
 * @param timeout_sec Timeout [1, 120]
 * @return true on success
 */
bool whisper_client_set_connect_timeout(WhisperClient *client, int timeout_sec) {
    if (!client || timeout_sec < 1 || timeout_sec > 120) return false;

    pthread_mutex_lock(&client->mutex);
    client->connect_timeout = timeout_sec;
    pthread_mutex_unlock(&client->mutex);
    return true;
}

/**
 * whisper_client_set_total_timeout
 * Sets the total request timeout in seconds.
 *
 * @param client Client handle
 * @param timeout_sec Timeout [5, 300]
 * @return true on success
 */
bool whisper_client_set_total_timeout(WhisperClient *client, int timeout_sec) {
    if (!client || timeout_sec < 5 || timeout_sec > 300) return false;

    pthread_mutex_lock(&client->mutex);
    client->total_timeout = timeout_sec;
    pthread_mutex_unlock(&client->mutex);
    return true;
}

/* ===================================================================
 * Core transcription
 * =================================================================== */

/**
 * whisper_transcribe
 * Sends a WAV file to the Whisper API for transcription.
 *
 * @param client Client handle
 * @param wav_path Path to WAV file
 * @return WhisperResponse* (caller must free), or NULL on critical failure
 */
WhisperResponse * whisper_transcribe(WhisperClient *client, const char *wav_path) {
    if (!client || !wav_path || !client->url[0]) {
        /* Return error response */
        WhisperResponse *resp = (WhisperResponse *)calloc(1, sizeof(WhisperResponse));
        if (!resp) return NULL;

        if (!client) {
            strncpy(resp->error_message, "Invalid client", sizeof(resp->error_message) - 1);
        } else if (!wav_path) {
            strncpy(resp->error_message, "Invalid WAV path", sizeof(resp->error_message) - 1);
        } else {
            strncpy(resp->error_message, "URL not configured", sizeof(resp->error_message) - 1);
        }
        resp->http_status = 0;
        resp->success = false;
        return resp;
    }

    WhisperResponse *resp = (WhisperResponse *)calloc(1, sizeof(WhisperResponse));
    if (!resp) return NULL;

    /* CR-01/CR-02 fix: Copy configuration under mutex, then release before network I/O.
     * Use a per-call CURL handle to avoid sharing across threads. */
    char local_url[MAX_URL_LEN];
    char local_model[MAX_MODEL_LEN];
    char local_language[MAX_LANGUAGE_LEN];
    int local_connect_timeout;
    int local_total_timeout;

    pthread_mutex_lock(&client->mutex);
    strncpy(local_url, client->url, sizeof(local_url) - 1);
    local_url[sizeof(local_url) - 1] = '\0';
    strncpy(local_model, client->model, sizeof(local_model) - 1);
    local_model[sizeof(local_model) - 1] = '\0';
    strncpy(local_language, client->language, sizeof(local_language) - 1);
    local_language[sizeof(local_language) - 1] = '\0';
    local_connect_timeout = client->connect_timeout;
    local_total_timeout = client->total_timeout;
    pthread_mutex_unlock(&client->mutex);

    /* Build full URL from copied config */
    char full_url[MAX_URL_LEN + 64];
    snprintf(full_url, sizeof(full_url), "%s", local_url);

    /* Read WAV file */
    size_t wav_size = 0;
    char *wav_data = read_file_to_buffer(wav_path, &wav_size);
    if (!wav_data) {
        pthread_mutex_lock(&client->mutex);
        set_client_error_locked(client, "Failed to read WAV file");
        pthread_mutex_unlock(&client->mutex);
        strncpy(resp->error_message, "Failed to read WAV file", sizeof(resp->error_message) - 1);
        resp->http_status = 0;
        resp->success = false;
        return resp;
    }

    /* CR-02 fix: Create a per-call CURL handle instead of sharing client->curl */
    CURL *curl = curl_easy_init();
    if (!curl) {
        pthread_mutex_lock(&client->mutex);
        set_client_error_locked(client, "Failed to initialize CURL handle");
        pthread_mutex_unlock(&client->mutex);
        strncpy(resp->error_message, "Failed to initialize CURL handle", sizeof(resp->error_message) - 1);
        resp->http_status = 0;
        resp->success = false;
        scrub_memory(wav_data, wav_size);
        free(wav_data);
        return resp;
    }

    /* Setup libcurl for multipart POST using curl_mime API */
    curl_mime *mime = curl_mime_init(curl);

    /* Add file field */
    curl_mimepart *file_part = curl_mime_addpart(mime);
    curl_mime_name(file_part, "file");
    curl_mime_filename(file_part, "recording.wav");
    curl_mime_data(file_part, wav_data, (size_t)wav_size);

    /* MIN-010 fix: Only add model field if populated (from connection check) */
    if (local_model[0] != '\0') {
        curl_mimepart *model_part = curl_mime_addpart(mime);
        curl_mime_name(model_part, "model");
        curl_mime_data(model_part, local_model, (size_t)strlen(local_model));
    }

    /* Add language field if set */
    if (local_language[0]) {
        curl_mimepart *lang_part = curl_mime_addpart(mime);
        curl_mime_name(lang_part, "language");
        curl_mime_data(lang_part, local_language, (size_t)strlen(local_language));
    }
    /* Response buffer */
    struct MemoryChunk chunk;
    chunk.data = (char *)calloc(1, 1024);
    chunk.size = 0;
    chunk.capacity = 1024;

    /* Header buffer */
    struct HeaderChunk headers;
    headers.data = (char *)calloc(1, 1024);
    headers.size = 0;
    headers.capacity = 1024;

    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)local_connect_timeout);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)local_total_timeout);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    long http_status = 0;

    /* Network operation performed WITHOUT holding the mutex (CR-01 fix) */
    CURLcode curl_err = curl_easy_perform(curl);
    if (curl_err == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    }

    /* Cleanup mime and CURL handle */
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    /* Scrub WAV data from memory */
    scrub_memory(wav_data, wav_size);
    free(wav_data);

    /* Handle curl errors */
    if (curl_err != CURLE_OK) {
        pthread_mutex_lock(&client->mutex);
        set_client_error_locked(client, curl_easy_strerror(curl_err));
        client->curl_error_code = curl_err;
        pthread_mutex_unlock(&client->mutex);
        strncpy(resp->error_message, curl_easy_strerror(curl_err), sizeof(resp->error_message) - 1);
        resp->http_status = 0;
        resp->success = false;
        free(chunk.data);
        free(headers.data);
        return resp;
    }

    resp->http_status = (int)http_status;

    /* Handle HTTP errors */
    if (http_status != 200) {
        /* Try to parse error from vLLM JSON response */
        cJSON *error_json = cJSON_Parse(chunk.data);
        if (error_json) {
            cJSON *detail_obj = cJSON_GetObjectItemCaseSensitive(error_json, "detail");
            if (cJSON_IsString(detail_obj) && detail_obj->valuestring) {
                snprintf(resp->error_message, sizeof(resp->error_message),
                         "vLLM error: %s", detail_obj->valuestring);
            } else {
                /* Check for message field */
                cJSON *msg_obj = cJSON_GetObjectItemCaseSensitive(error_json, "message");
                if (cJSON_IsString(msg_obj) && msg_obj->valuestring) {
                    snprintf(resp->error_message, sizeof(resp->error_message),
                             "vLLM error: %s", msg_obj->valuestring);
                } else {
                    snprintf(resp->error_message, sizeof(resp->error_message),
                             "HTTP %ld", http_status);
                }
            }
            cJSON_Delete(error_json);
        } else {
            if (http_status >= 400 && http_status < 500) {
                snprintf(resp->error_message, sizeof(resp->error_message),
                         "Client error: HTTP %ld", http_status);
            } else if (http_status >= 500) {
                snprintf(resp->error_message, sizeof(resp->error_message),
                         "Server error: HTTP %ld", http_status);
            } else {
                snprintf(resp->error_message, sizeof(resp->error_message),
                         "Unexpected HTTP status: %ld", http_status);
            }
        }
        resp->success = false;
        free(chunk.data);
        free(headers.data);
        return resp;
    }

    /* Parse JSON response */
    cJSON *json = cJSON_Parse(chunk.data);
    free(chunk.data);

    if (!json) {
        pthread_mutex_lock(&client->mutex);
        set_client_error_locked(client, "Failed to parse JSON response");
        pthread_mutex_unlock(&client->mutex);
        strncpy(resp->error_message, "Invalid JSON response", sizeof(resp->error_message) - 1);
        resp->success = false;
        free(headers.data);
        return resp;
    }

    /* Extract "text" field */
    cJSON *text_obj = cJSON_GetObjectItemCaseSensitive(json, "text");
    if (cJSON_IsString(text_obj) && text_obj->valuestring) {
        resp->text = strdup(text_obj->valuestring);
        if (!resp->text) {
            pthread_mutex_lock(&client->mutex);
            set_client_error_locked(client, "Failed to allocate memory for transcription text");
            pthread_mutex_unlock(&client->mutex);
            cJSON_Delete(json);
            free(headers.data);
            return resp;
        }
        resp->success = true;
    } else {
        /* Check for error in response */
        cJSON *error_obj = cJSON_GetObjectItemCaseSensitive(json, "error");
        if (cJSON_IsObject(error_obj)) {
            cJSON *msg_obj = cJSON_GetObjectItemCaseSensitive(error_obj, "message");
            if (cJSON_IsString(msg_obj)) {
                strncpy(resp->error_message, msg_obj->valuestring, sizeof(resp->error_message) - 1);
            } else {
                strncpy(resp->error_message, "No text field in response", sizeof(resp->error_message) - 1);
            }
        } else {
            strncpy(resp->error_message, "No text field in response", sizeof(resp->error_message) - 1);
        }
        resp->success = false;
    }

    cJSON_Delete(json);
    free(headers.data);
    return resp;
}

/**
 * whisper_transcribe_with_retry
 * Transcription with exponential backoff retry.
 *
 * @param client Client handle
 * @param wav_path Path to WAV file
 * @param max_retries Max retry attempts (0 = no retry)
 * @return WhisperResponse* (caller must free)
 */
WhisperResponse * whisper_transcribe_with_retry(
    WhisperClient *client, const char *wav_path, int max_retries)
{
    if (!client || !wav_path) return NULL;

    WhisperResponse *resp = NULL;
    int attempts = 0;
    int delay = 1;  /* Initial delay in seconds */


    do {
        if (attempts > 0) {
            fprintf(stderr, "[whisper] Retry attempt %d/%d (delay %ds)...\n",
                    attempts, max_retries, delay);
        } else {
            fprintf(stderr, "[whisper] Transcribing %s...\n", wav_path);
        }

        resp = whisper_transcribe(client, wav_path);
        if (!resp) return NULL;

        /* Check if we should retry */
        bool should_retry = false;

        if (!resp->success) {
            /* Retry on 5xx server errors */
            if (resp->http_status >= 500 && resp->http_status < 600) {
                should_retry = true;
            }
            /* Retry on 408 Request Timeout */
            else if (resp->http_status == 408) {
                should_retry = true;
            }
            /* Retry on 429 Too Many Requests */
            else if (resp->http_status == 429) {
                should_retry = true;
            }
            /* Retry on connection errors (HTTP status 0) */
            else if (resp->http_status == 0) {
                should_retry = true;
            }
        }

        if (!should_retry || attempts >= max_retries) {
            break;
        }

        /* Free previous response before retry */
        whisper_response_free(resp);
        resp = NULL;

        /* Exponential backoff sleep with jitter to prevent thundering herd.
         * Jitter: add random 0-500ms to the delay using getrandom() for better randomness. */
        {
            struct timespec ts;
            ts.tv_sec = delay;
            /* ME-02 fix: Use getrandom() instead of unseeded rand() for jitter */
            unsigned jitter_ms = 0;
            ssize_t nr = getrandom(&jitter_ms, sizeof(jitter_ms), 0);
            if (nr > 0) {
                jitter_ms = jitter_ms % 500;
            }
            ts.tv_nsec = (unsigned)(jitter_ms * 1000000UL);
            nanosleep(&ts, NULL);
        }
        delay *= 2;
        if (delay > MAX_RETRY_DELAY) {
            delay = MAX_RETRY_DELAY;
        }

        attempts++;
    } while (attempts < max_retries);

    return resp;
}

/* ===================================================================
 * Connection health check
 * =================================================================== */

/**
 * whisper_check_connection
 * Checks if the Whisper API server is reachable.
 *
 * @param client Client handle
 * @return true if server responded with HTTP 200
 */
bool whisper_check_connection(WhisperClient *client) {
    if (!client || !client->url[0]) return false;

    /* Copy URL under mutex, then release before network I/O */
    char local_url[MAX_URL_LEN];
    pthread_mutex_lock(&client->mutex);
    strncpy(local_url, client->url, sizeof(local_url) - 1);
    local_url[sizeof(local_url) - 1] = '\0';
    pthread_mutex_unlock(&client->mutex);

    char base_url[MAX_URL_LEN];
    extract_base_url(local_url, base_url, sizeof(base_url));
    char full_url[MAX_URL_LEN + 32];
    snprintf(full_url, sizeof(full_url), "%s/v1/models", base_url);

    /* CR-02 fix: Use a per-call CURL handle instead of sharing client->curl */
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    struct MemoryChunk chunk;
    chunk.data = (char *)calloc(1, 1024);
    chunk.size = 0;
    chunk.capacity = 1024;

    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);  /* Short timeout for health check */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

    CURLcode curl_err = curl_easy_perform(curl);
    long http_status = 0;

    if (curl_err == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    }

    bool connected = false;

    if (curl_err == CURLE_OK && http_status == 200) {
        /* Parse JSON to verify this is a Whisper server and discover model */
        cJSON *json = cJSON_Parse(chunk.data);
        if (json) {
            cJSON *data_obj = cJSON_GetObjectItemCaseSensitive(json, "data");
            if (cJSON_IsArray(data_obj) && cJSON_GetArraySize(data_obj) > 0) {
                /* Extract the first model ID and store it */
                cJSON *first_model = cJSON_GetArrayItem(data_obj, 0);
                cJSON *id_obj = cJSON_GetObjectItemCaseSensitive(first_model, "id");
                if (cJSON_IsString(id_obj) && id_obj->valuestring) {
                    pthread_mutex_lock(&client->mutex);
                    strncpy(client->model, id_obj->valuestring, sizeof(client->model) - 1);
                    client->model[sizeof(client->model) - 1] = '\0';
                    pthread_mutex_unlock(&client->mutex);
                    connected = true;
                }
            }
            cJSON_Delete(json);
        }
    } else {
        pthread_mutex_lock(&client->mutex);
        if (curl_err != CURLE_OK) {
            set_client_error_locked(client, curl_easy_strerror(curl_err));
            client->curl_error_code = curl_err;
        } else {
            set_client_error_locked(client, "Server returned non-200 status");
        }
        pthread_mutex_unlock(&client->mutex);
    }

    free(chunk.data);
    curl_easy_cleanup(curl);
    return connected;
}


/* ===================================================================
 * Error handling
 * =================================================================== */

/**
 * whisper_client_get_error
 * Gets the last error message.
 *
 * @param client Client handle
 * @return Error message string (empty if no error)
 *
 * Thread-safe: Returns a pointer to a thread-local buffer that is
 * populated under the client mutex. The caller does not need to free
 * the returned string, but the value is only guaranteed stable until
 * the next call to whisper_client_get_error() from the same thread.
 */
const char * whisper_client_get_error(WhisperClient *client) {
    if (!client) return "Invalid client";
    static __thread char local_buffer[256] = {0};
    pthread_mutex_lock(&client->mutex);
    strncpy(local_buffer, client->error_message, sizeof(local_buffer) - 1);
    local_buffer[sizeof(local_buffer) - 1] = '\0';
    pthread_mutex_unlock(&client->mutex);
    return local_buffer;
}

/**
 * whisper_client_get_curl_error
 * Gets the libcurl error code from the last operation.
 *
 * @param client Client handle
 * @return CURLcode (CURLE_OK = 0 if no error)
 */
int whisper_client_get_curl_error(const WhisperClient *client) {
    if (!client) return CURLE_FAILED_INIT;
    return client->curl_error_code;
}

/**
 * whisper_client_get_model
 * Gets the discovered model name from the last connection check.
 *
 * @param client Client handle
 * @return Model name string (empty if not yet discovered)
 */
const char * whisper_client_get_model(const WhisperClient *client) {
    if (!client) return "";
    return client->model;
}
