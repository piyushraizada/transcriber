/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/**
 * @file app_llama.c
 * @brief llama-server HTTP ASR client implementation
 *
 * Implements the llama-server (OpenAI-compatible) transcription backend:
 * in-memory WAV construction, base64 payload, HTTP/1.1 requests via GLib
 * GSocketClient (one connection per request), request-scoped timeouts via a
 * private canceller thread (no GLib main loop required on the calling
 * thread), response parsing, and the auto-fallback wrappers that retry a
 * failed request through the local Whisper client.
 *
 * See app_llama.h for the public contract and threading rules.
 * @see plans/llama-server-asr-backend-plan.md
 */

#include "app_llama.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

#include <glib.h>
#include <glib/gbase64.h>
#include <gio/gio.h>
#include <cjson/cJSON.h>

/*---------------------------------------------------------------------------
 * Internal LlamaClient struct
 *---------------------------------------------------------------------------*/

#define LLAMA_MAX_URL_LEN 256
#define LLAMA_MAX_MODEL_LEN 128
#define LLAMA_MAX_ERROR_LEN 256
#define LLAMA_WAV_HEADER_SIZE 44

struct _LlamaClient {
    pthread_mutex_t mutex;        ///< Protects server_url, model, inflight, error state
    char server_url[LLAMA_MAX_URL_LEN];  ///< Base URL, no trailing slash
    char model[LLAMA_MAX_MODEL_LEN];     ///< Model alias on the server
    GList *inflight;              ///< Strong refs to GCancellables of in-flight requests
    char error_message[LLAMA_MAX_ERROR_LEN];
    int error_code;
};

/*---------------------------------------------------------------------------
 * Error message helper
 *---------------------------------------------------------------------------*/

static void set_client_error(LlamaClient *client, int code, const char *msg)
{
    if (!client) return;
    pthread_mutex_lock(&client->mutex);
    client->error_code = code;
    if (msg) {
        snprintf(client->error_message, sizeof(client->error_message), "%s", msg);
    } else {
        client->error_message[0] = '\0';
    }
    pthread_mutex_unlock(&client->mutex);
}

static void fill_response_error(WhisperResponse *response, int code, const char *detail)
{
    if (!response) return;
    response->success = false;
    response->error_code = code;
    response->text = NULL;
    snprintf(response->error_message, sizeof(response->error_message), "%s",
             detail ? detail : "unknown error");
}

/*---------------------------------------------------------------------------
 * Little-endian encode/decode helpers (portable WAV handling)
 *---------------------------------------------------------------------------*/

static void le16enc(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void le32enc(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint16_t le16dec(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32dec(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*---------------------------------------------------------------------------
 * Request-scoped timeout enforcement
 *
 * Worker threads do not run a GLib main loop, so g_timeout_add() cannot be
 * used here. Instead a private canceller thread sleeps in short increments
 * until either the read deadline passes (cancels the GCancellable) or the
 * request signals completion (wakes via condvar, returns promptly).
 *---------------------------------------------------------------------------*/

typedef struct {
    GCancellable *cancellable;
    atomic_int timed_out;         ///< Set by the canceller thread on deadline expiry
    pthread_mutex_t lock;         ///< Protects done
    pthread_cond_t cond;          ///< Signaled when the request finishes
    int done;
} LlamaTimeoutCtx;

static gpointer llama_timeout_thread(gpointer user_data)
{
    LlamaTimeoutCtx *ctx = (LlamaTimeoutCtx *)user_data;
    gint64 deadline = g_get_monotonic_time()
                     + (gint64)LLAMA_READ_TIMEOUT_SEC * G_TIME_SPAN_SECOND;

    pthread_mutex_lock(&ctx->lock);
    while (!ctx->done) {
        if (g_get_monotonic_time() >= deadline) {
            atomic_store(&ctx->timed_out, 1);
            pthread_mutex_unlock(&ctx->lock);
            g_cancellable_cancel(ctx->cancellable);
            return NULL;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        ts.tv_sec  += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&ctx->cond, &ctx->lock, &ts);
    }
    pthread_mutex_unlock(&ctx->lock);
    return NULL;
}

static bool llama_timeout_ctx_init(LlamaTimeoutCtx *ctx,
                                   GCancellable *cancellable,
                                   GThread **thread_out)
{
    ctx->cancellable = cancellable;
    atomic_store(&ctx->timed_out, 0);
    ctx->done = 0;
    if (pthread_mutex_init(&ctx->lock, NULL) != 0) return false;
    if (pthread_cond_init(&ctx->cond, NULL) != 0) {
        pthread_mutex_destroy(&ctx->lock);
        return false;
    }
    GThread *thread = g_thread_new("llama-timeout", llama_timeout_thread, ctx);
    if (!thread) {
        pthread_cond_destroy(&ctx->cond);
        pthread_mutex_destroy(&ctx->lock);
        return false;
    }
    *thread_out = thread;
    return true;
}

static void llama_timeout_ctx_fini(LlamaTimeoutCtx *ctx, GThread *thread)
{
    pthread_mutex_lock(&ctx->lock);
    ctx->done = 1;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
    g_thread_join(thread);
    pthread_cond_destroy(&ctx->cond);
    pthread_mutex_destroy(&ctx->lock);
}

/*---------------------------------------------------------------------------
 * HTTP core
 *---------------------------------------------------------------------------*/

typedef struct {
    int status_code;              ///< 0 when no HTTP response was received
    char *body;                   ///< Caller frees with g_free(); NULL on transport error
    llama_error_code error;       ///< LLAMA_ERR_OK when an HTTP response was received
    bool timed_out;               ///< Read deadline hit (vs. user cancellation)
} LlamaHttpResult;

static void llama_http_result_free(LlamaHttpResult *r)
{
    if (!r) return;
    if (r->body) {
        g_free(r->body);
        r->body = NULL;
    }
}

/**
 * Perform one HTTP/1.1 request (GET or POST) with Connection: close and
 * read the full response until EOF.
 *
 * On transport failure: out->status_code == 0, out->error set to the
 * corresponding llama_error_code. On any HTTP response: out->status_code
 * holds the status; out->error is LLAMA_ERR_HTTP for non-2xx.
 */
static void llama_http_request(LlamaClient *client,
                               const char *url,
                               const char *method,
                               const char *path,
                               const char *body,
                               gsize body_len,
                               LlamaHttpResult *out)
{
    out->status_code = 0;
    out->body = NULL;
    out->error = LLAMA_ERR_OK;
    out->timed_out = false;

    GCancellable *cancellable = g_cancellable_new();
    if (!cancellable) {
        out->error = LLAMA_ERR_ALLOC;
        return;
    }

    LlamaTimeoutCtx tctx;
    GThread *timer_thread = NULL;
    if (!llama_timeout_ctx_init(&tctx, cancellable, &timer_thread)) {
        g_object_unref(cancellable);
        out->error = LLAMA_ERR_ALLOC;
        return;
    }

    /* Register as in-flight (strong ref so llama_client_cancel() is always
     * safe; removed on every exit path). */
    pthread_mutex_lock(&client->mutex);
    GList *slot = g_list_append(client->inflight, g_object_ref(cancellable));
    client->inflight = slot;
    pthread_mutex_unlock(&client->mutex);

    char *uri = g_strdup_printf("%s%s", url, path);
    GError *gerr = NULL;
    GSocketClient *sc = g_socket_client_new();
    g_socket_client_set_timeout(sc, LLAMA_CONNECT_TIMEOUT_SEC);

    /* Split the URI for the Host header and the effective port.
     * This GLib build's connect_to_uri() takes an explicit default port,
     * used when the URI omits one: the explicit URI port if present, else
     * the scheme default. */
    char *u_scheme = NULL;
    char *u_host = NULL;
    int u_port = 0;
    char hostport[LLAMA_MAX_URL_LEN + 16];
    if (g_uri_split_network(uri, 0, &u_scheme, &u_host, &u_port, NULL) && u_host) {
        if (u_port > 0) {
            g_snprintf(hostport, sizeof(hostport), "%s:%d", u_host, u_port);
        } else {
            g_snprintf(hostport, sizeof(hostport), "%s", u_host);
        }
    } else {
        g_snprintf(hostport, sizeof(hostport), "localhost");
    }
    int dport = u_port;
    if (dport <= 0) {
        dport = (u_scheme && strcmp(u_scheme, "https") == 0) ? 443 : 80;
    }
    g_free(u_scheme);
    g_free(u_host);

    GSocketConnection *conn =
        g_socket_client_connect_to_uri(sc, uri, (guint16)dport, cancellable, &gerr);

    /* The GSocketClient timeout (the connect budget) is inherited by the
     * connected socket as its I/O timeout. Raise it to the read deadline:
     * a long transcription response can legitimately take far longer than
     * the connect budget to arrive, and the canceller thread above still
     * enforces LLAMA_READ_TIMEOUT_SEC independently. */
    if (conn) {
        GSocket *sock = g_socket_connection_get_socket(conn);
        if (sock) {
            g_socket_set_timeout(sock, LLAMA_READ_TIMEOUT_SEC);
        }
    }

    if (!conn) {
        if (atomic_load(&tctx.timed_out)) {
            out->error = LLAMA_ERR_READ_TIMEOUT;
        } else if (gerr && gerr->code == G_IO_ERROR_TIMED_OUT) {
            out->error = LLAMA_ERR_CONNECT;
        } else if (gerr && gerr->code == G_IO_ERROR_CANCELLED) {
            out->error = LLAMA_ERR_CANCELLED;
        } else {
            out->error = LLAMA_ERR_CONNECT;
        }
        g_log("app-llama", G_LOG_LEVEL_MESSAGE,
              "[llama] Connect to %s failed: %s\n", uri,
              gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        goto cleanup;
    }

    /* Write the request. */
    GString *req = g_string_new(NULL);
    if (body && body_len > 0) {
        g_string_append_printf(req,
            "%s %s HTTP/1.1\r\nHost: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            method, path, hostport, body_len);
        g_string_append_len(req, body, body_len);
    } else {
        g_string_append_printf(req,
            "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
            method, path, hostport);
    }

    GOutputStream *out_stream =
        G_OUTPUT_STREAM(g_io_stream_get_output_stream(G_IO_STREAM(conn)));
    g_clear_error(&gerr);
    gsize n_written = 0;
    /* This GLib's write_all() returns gboolean success; the byte count
     * comes out via the n_written out-parameter. */
    if (!g_output_stream_write_all(out_stream, (const guint8 *)req->str,
                                   req->len, &n_written, cancellable, &gerr) ||
        n_written != (gsize)req->len) {
        if (atomic_load(&tctx.timed_out)) {
            out->error = LLAMA_ERR_READ_TIMEOUT;
        } else if (gerr && gerr->code == G_IO_ERROR_CANCELLED) {
            out->error = LLAMA_ERR_CANCELLED;
        } else if (gerr && gerr->code == G_IO_ERROR_TIMED_OUT) {
            out->error = LLAMA_ERR_READ_TIMEOUT;
        } else {
            out->error = LLAMA_ERR_CONNECT;
        }
        g_log("app-llama", G_LOG_LEVEL_MESSAGE,
              "[llama] Write to %s failed: %s\n", uri,
              gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        g_string_free(req, TRUE);
        goto cleanup;
    }
    g_string_free(req, TRUE);

    /* Read the response until EOF (Connection: close). */
    GByteArray *ba = g_byte_array_new();
    GInputStream *in_stream =
        G_INPUT_STREAM(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
    uint8_t tmp[8192];
    bool read_ok = true;
    /* This GLib's read_all() returns gboolean success; the byte count
     * comes out via the n_bytes out-parameter. n_bytes == 0 signals EOF. */
    for (;;) {
        gsize n_bytes = 0;
        g_clear_error(&gerr);
        if (!g_input_stream_read_all(in_stream, tmp, sizeof(tmp), &n_bytes,
                                     cancellable, &gerr)) {
            read_ok = false;
            break;
        }
        if (n_bytes == 0) break;  /* EOF */
        g_byte_array_append(ba, tmp, n_bytes);
    }

    if (!read_ok) {
        if (atomic_load(&tctx.timed_out)) {
            out->error = LLAMA_ERR_READ_TIMEOUT;
        } else if (gerr && gerr->code == G_IO_ERROR_CANCELLED) {
            out->error = LLAMA_ERR_CANCELLED;
        } else if (gerr && gerr->code == G_IO_ERROR_TIMED_OUT) {
            /* The socket I/O timeout (LLAMA_READ_TIMEOUT_SEC) fired. */
            out->error = LLAMA_ERR_READ_TIMEOUT;
        } else {
            out->error = LLAMA_ERR_CONNECT;
        }
        g_log("app-llama", G_LOG_LEVEL_MESSAGE,
              "[llama] Read from %s failed: %s\n", uri,
              gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        g_byte_array_free(ba, TRUE);
        goto cleanup;
    }

    /* NUL-terminate, then parse the status line and split headers/body.
     * Each branch below frees ba exactly once (struct + data). */
    const uint8_t nul_byte = 0;
    g_byte_array_append(ba, &nul_byte, 1);
    char *data = (char *)ba->data;
    int status = 0;
    if (strncmp(data, "HTTP/", 5) == 0) {
        char *sp = strchr(data + 5, ' ');
        if (sp) status = (int)strtol(sp + 1, NULL, 10);
    }
    if (status < 100 || status > 599) {
        out->error = LLAMA_ERR_RESPONSE_PARSE;
        out->body = g_strndup(data, ba->len - 1);
        g_byte_array_free(ba, TRUE);
    } else {
        char *sep = strstr(data, "\r\n\r\n");
        size_t sep_len = 4;
        if (!sep) {
            sep = strstr(data, "\n\n");
            sep_len = 2;
        }
        out->status_code = status;
        if (sep) {
            size_t body_off = (size_t)(sep - data) + sep_len;
            out->body = g_strndup(data + body_off, ba->len - 1 - body_off);
            if (!out->body) {
                out->error = LLAMA_ERR_ALLOC;
            }
        } else {
            out->body = g_strdup("");
            out->error = LLAMA_ERR_RESPONSE_PARSE;
        }
        g_byte_array_free(ba, TRUE);
    }

    if (out->error == LLAMA_ERR_OK && out->status_code != 200) {
        out->error = LLAMA_ERR_HTTP;
    }

cleanup:
    if (conn) g_object_unref(conn);
    g_object_unref(sc);
    g_free(uri);

    /* Clear the in-flight slot (our strong ref). slot is NULL only if
     * g_list_append() OOM'd at registration, in which case nothing was
     * added to the list. */
    pthread_mutex_lock(&client->mutex);
    if (slot && client->inflight == slot) {
        g_object_unref(slot->data);
        client->inflight = g_list_delete_link(client->inflight, slot);
    }
    pthread_mutex_unlock(&client->mutex);

    /* Stop the canceller thread BEFORE dropping the last cancellable ref. */
    llama_timeout_ctx_fini(&tctx, timer_thread);
    g_object_unref(cancellable);
}

/*---------------------------------------------------------------------------
 * WAV helpers
 *---------------------------------------------------------------------------*/

/**
 * Fill a minimal 44-byte RIFF/WAVE header for 16-bit mono PCM data.
 */
static void build_wav_header(uint8_t *hdr, uint32_t data_size)
{
    memcpy(hdr + 0, "RIFF", 4);
    le32enc(hdr + 4, 36 + data_size);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    le32enc(hdr + 16, 16);                       /* fmt chunk size */
    le16enc(hdr + 20, 1);                        /* audio format: PCM */
    le16enc(hdr + 22, 1);                        /* channels: mono */
    le32enc(hdr + 24, LLAMA_SAMPLE_RATE);        /* sample rate */
    le32enc(hdr + 28, LLAMA_SAMPLE_RATE * 2);    /* byte rate */
    le16enc(hdr + 32, 2);                        /* block align */
    le16enc(hdr + 34, 16);                       /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    le32enc(hdr + 40, data_size);
}

/**
 * Verify that a file on disk is a 16 kHz mono 16-bit PCM WAV.
 *
 * The whole file is sent to the server as-is, so no sample extraction is
 * performed here — only header validation. On failure, *detail (if non-NULL)
 * receives a short human-readable reason.
 */
static bool validate_wav_file(const char *path, char *detail, size_t detail_size)
{
    char *data = NULL;
    gsize len = 0;
    if (detail && detail_size > 0) detail[0] = '\0';

    if (!g_file_get_contents(path, &data, &len, NULL)) {
        if (detail) snprintf(detail, detail_size, "cannot read WAV file: %s", path);
        return false;
    }

    if (len < LLAMA_WAV_HEADER_SIZE ||
        memcmp(data, "RIFF", 4) != 0 ||
        memcmp(data + 8, "WAVE", 4) != 0) {
        if (detail) snprintf(detail, detail_size, "not a RIFF/WAVE file: %s", path);
        g_free(data);
        return false;
    }

    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    bool found_fmt = false, found_data = false;
    size_t off = 12;

    while (off + 8 <= len) {
        const uint8_t *p = (const uint8_t *)data + off;
        char id[5] = {0};
        memcpy(id, p, 4);
        uint32_t csize = le32dec(p + 4);
        if (off + 8 + csize > len) {
            if (detail) snprintf(detail, detail_size, "truncated WAV chunk in: %s", path);
            g_free(data);
            return false;
        }
        if (strcmp(id, "fmt ") == 0) {
            if (csize < 16) {
                if (detail) snprintf(detail, detail_size, "invalid fmt chunk in: %s", path);
                g_free(data);
                return false;
            }
            channels = le16dec(p + 10);
            rate     = le32dec(p + 12);
            bits     = le16dec(p + 22);
            found_fmt = true;
        } else if (strcmp(id, "data") == 0) {
            found_data = true;
            break;
        }
        off += 8 + csize + (csize & 1);  /* chunks are word-aligned */
    }

    g_free(data);

    if (!found_fmt || !found_data) {
        if (detail) snprintf(detail, detail_size, "WAV missing fmt/data chunk: %s", path);
        return false;
    }
    if (rate != LLAMA_SAMPLE_RATE || channels != 1 || bits != 16) {
        if (detail) {
            snprintf(detail, detail_size,
                     "WAV is %u Hz / %u ch / %u bit, expected %d kHz mono 16-bit: %s",
                     (unsigned)rate, (unsigned)channels, (unsigned)bits,
                     LLAMA_SAMPLE_RATE / 1000, path);
        }
        return false;
    }
    return true;
}

/*---------------------------------------------------------------------------
 * Transcription core (shared by the PCM and WAV-file entry points)
 *---------------------------------------------------------------------------*/

/**
 * Send ready-to-send WAV bytes to the llama-server and parse the response.
 *
 * Returns a caller-owned WhisperResponse on every path except allocation
 * failure of the response struct itself (NULL). On failure the response
 * carries a llama_error_code and a human-readable message; the client's
 * last-error state is updated to match.
 */
static WhisperResponse *transcribe_wav_bytes(LlamaClient *client,
                                             const uint8_t *wav,
                                             size_t wav_len,
                                             const char *language)
{
    WhisperResponse *response = g_new0(WhisperResponse, 1);
    if (!response) return NULL;
    response->success = false;
    response->text = NULL;
    response->error_message[0] = '\0';

    char url[LLAMA_MAX_URL_LEN] = {0};
    char model[LLAMA_MAX_MODEL_LEN] = {0};
    char *b64 = NULL;
    char *body = NULL;
    char *prompt_buf = NULL;
    const char *prompt = "Transcribe this audio.";
    char *text = NULL;
    LlamaHttpResult http;
    http.status_code = 0;
    http.body = NULL;
    http.error = LLAMA_ERR_OK;
    http.timed_out = false;

    /* Snapshot configuration under the client mutex. */
    pthread_mutex_lock(&client->mutex);
    snprintf(url, sizeof(url), "%s", client->server_url);
    snprintf(model, sizeof(model), "%s", client->model);
    pthread_mutex_unlock(&client->mutex);

    if (url[0] == '\0' || model[0] == '\0') {
        const char *why = url[0] == '\0' ? "server URL not configured"
                                         : "model not configured";
        fill_response_error(response, LLAMA_ERR_INVALID_PARAM, why);
        set_client_error(client, LLAMA_ERR_INVALID_PARAM, why);
        goto fail;
    }

    /* Prompt: bake the language setting in when it is not "auto" (D5). */
    if (language && language[0] != '\0' && strncmp(language, "auto", 5) != 0) {
        char lang_prompt[64];
        g_snprintf(lang_prompt, sizeof(lang_prompt),
                   "Transcribe this audio in %s.", language);
        prompt_buf = g_strdup(lang_prompt);
        if (!prompt_buf) {
            fill_response_error(response, LLAMA_ERR_ALLOC, "memory allocation failed");
            set_client_error(client, LLAMA_ERR_ALLOC, "memory allocation failed");
            goto fail;
        }
        prompt = prompt_buf;
    }

    /* Build the request JSON. Ownership rule: each successful
     * cJSON_AddItemToObject() transfers ownership into the parent, so on
     * failure the only dangling object is the most recently created one
     * that was not yet attached — delete exactly that. */
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = NULL;
    cJSON *msg = NULL;
    cJSON *content = NULL;
    cJSON *text_part = NULL;
    cJSON *audio_part = NULL;
    cJSON *audio_obj = NULL;

#define JSON_FAIL(reason) do { \
        fill_response_error(response, LLAMA_ERR_ALLOC, reason); \
        set_client_error(client, LLAMA_ERR_ALLOC, reason); \
        goto fail; \
    } while (0)

    if (!root) JSON_FAIL("memory allocation failed");

    if (!cJSON_AddStringToObject(root, "model", model) ||
        !cJSON_AddNumberToObject(root, "temperature", 0) ||
        !cJSON_AddNumberToObject(root, "max_tokens", LLAMA_MAX_TOKENS_PER_SEGMENT) ||
        !cJSON_AddBoolToObject(root, "stream", false)) {
        JSON_FAIL("memory allocation failed");
    }

    messages = cJSON_AddArrayToObject(root, "messages");
    if (!messages) JSON_FAIL("memory allocation failed");

    msg = cJSON_CreateObject();
    if (!msg ||
        !cJSON_AddStringToObject(msg, "role", "user") ||
        !(content = cJSON_CreateArray()) ||
        !cJSON_AddItemToObject(msg, "content", content) ||
        !cJSON_AddItemToArray(messages, msg)) {
        if (msg) cJSON_Delete(msg);
        JSON_FAIL("memory allocation failed");
    }

    text_part = cJSON_CreateObject();
    if (!text_part ||
        !cJSON_AddStringToObject(text_part, "type", "text") ||
        !cJSON_AddStringToObject(text_part, "text", prompt)) {
        if (text_part) cJSON_Delete(text_part);
        JSON_FAIL("memory allocation failed");
    }
    if (!cJSON_AddItemToArray(content, text_part)) {
        cJSON_Delete(text_part);
        JSON_FAIL("memory allocation failed");
    }

    b64 = g_base64_encode(wav, wav_len);
    if (!b64) JSON_FAIL("memory allocation failed");

    audio_obj = cJSON_CreateObject();
    if (!audio_obj ||
        !cJSON_AddStringToObject(audio_obj, "data", b64) ||
        !cJSON_AddStringToObject(audio_obj, "format", "wav")) {
        if (audio_obj) cJSON_Delete(audio_obj);
        JSON_FAIL("memory allocation failed");
    }

    audio_part = cJSON_CreateObject();
    if (!audio_part ||
        !cJSON_AddStringToObject(audio_part, "type", "input_audio")) {
        if (audio_part) cJSON_Delete(audio_part);
        JSON_FAIL("memory allocation failed");
    }
    if (!cJSON_AddItemToObject(audio_part, "input_audio", audio_obj)) {
        cJSON_Delete(audio_obj);
        JSON_FAIL("memory allocation failed");
    }
    if (!cJSON_AddItemToArray(content, audio_part)) {
        cJSON_Delete(audio_part);
        JSON_FAIL("memory allocation failed");
    }

    body = cJSON_PrintUnformatted(root);
    if (!body) JSON_FAIL("memory allocation failed");
    cJSON_Delete(root);  /* body now owns the serialized data */

#undef JSON_FAIL

    /* Send the request. */
    llama_http_request(client, url, "POST", "/v1/chat/completions",
                       body, strlen(body), &http);

    if (http.error != LLAMA_ERR_OK) {
        char detail[LLAMA_MAX_ERROR_LEN];
        switch (http.error) {
        case LLAMA_ERR_CONNECT:
            snprintf(detail, sizeof(detail),
                     "cannot connect to llama-server at %.200s", url);
            break;
        case LLAMA_ERR_READ_TIMEOUT:
            snprintf(detail, sizeof(detail),
                     "llama-server at %.190s timed out (no response in %d s)",
                     url, LLAMA_READ_TIMEOUT_SEC);
            break;
        case LLAMA_ERR_CANCELLED:
            snprintf(detail, sizeof(detail), "transcription request cancelled");
            break;
        default:
            snprintf(detail, sizeof(detail), "request to llama-server at %.200s failed", url);
            break;
        }
        fill_response_error(response, http.error, detail);
        set_client_error(client, http.error, detail);
        g_log("app-llama", G_LOG_LEVEL_WARNING, "[llama] %s\n", detail);
        goto fail;
    }

    if (http.status_code != 200) {
        char detail[LLAMA_MAX_ERROR_LEN];
        if (http.body && http.body[0] != '\0') {
            snprintf(detail, sizeof(detail),
                     "llama-server %.60s returned HTTP %d: %.150s",
                     url, http.status_code, http.body);
        } else {
            snprintf(detail, sizeof(detail),
                     "llama-server %.200s returned HTTP %d", url, http.status_code);
        }
        fill_response_error(response, LLAMA_ERR_HTTP, detail);
        set_client_error(client, LLAMA_ERR_HTTP, detail);
        g_log("app-llama", G_LOG_LEVEL_WARNING, "[llama] %s\n", detail);
        goto fail;
    }

    /* Parse the chat completion response. */
    cJSON *resp = cJSON_Parse(http.body ? http.body : "");
    if (!resp) {
        fill_response_error(response, LLAMA_ERR_RESPONSE_PARSE,
                            "llama-server response is not valid JSON");
        set_client_error(client, LLAMA_ERR_RESPONSE_PARSE,
                         "llama-server response is not valid JSON");
        g_log("app-llama", G_LOG_LEVEL_WARNING,
              "[llama] response is not valid JSON: %.200s\n",
              http.body ? http.body : "(null body)");
        goto fail;
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(resp, "choices");
    cJSON *choice0 = (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0)
                    ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice0
                    ? cJSON_GetObjectItemCaseSensitive(choice0, "message") : NULL;
    cJSON *content_item = message
                         ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;

    if (content_item && cJSON_IsString(content_item) && content_item->valuestring) {
        text = g_strdup(content_item->valuestring);
    } else if (content_item && cJSON_IsArray(content_item)) {
        /* Defensive: content as an array of parts — concatenate text parts. */
        GString *buf = g_string_new(NULL);
        const cJSON *part = NULL;
        cJSON_ArrayForEach(part, content_item) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(part, "text");
            if (cJSON_IsString(t) && t->valuestring) {
                g_string_append(buf, t->valuestring);
            }
        }
        text = g_string_free(buf, FALSE);
        if (!text) text = g_strdup("");
    } else {
        cJSON_Delete(resp);
        fill_response_error(response, LLAMA_ERR_RESPONSE_PARSE,
                            "llama-server response missing choices[0].message.content");
        set_client_error(client, LLAMA_ERR_RESPONSE_PARSE,
                         "llama-server response missing choices[0].message.content");
        g_log("app-llama", G_LOG_LEVEL_WARNING,
              "[llama] response missing message content: %.200s\n",
              http.body ? http.body : "(null body)");
        goto fail;
    }
    cJSON_Delete(resp);

    if (!text) {
        fill_response_error(response, LLAMA_ERR_ALLOC, "memory allocation failed");
        set_client_error(client, LLAMA_ERR_ALLOC, "memory allocation failed");
        goto fail;
    }

    /* Success. An empty string is a VALID result (silence), not a failure. */
    response->text = text;
    text = NULL;  /* ownership transferred to the response */
    response->success = true;
    response->error_code = LLAMA_ERR_OK;
    response->error_message[0] = '\0';
    set_client_error(client, LLAMA_ERR_OK, NULL);

    double seconds = ((double)wav_len - LLAMA_WAV_HEADER_SIZE) / 2.0 / LLAMA_SAMPLE_RATE;
    g_log("app-llama", G_LOG_LEVEL_MESSAGE,
          "[llama] Transcribed %.1f s audio via %s (model=%s): %zu chars, HTTP %d\n",
          seconds, url, model, strlen(response->text), http.status_code);
    goto fail;

fail:
    if (b64) g_free(b64);
    if (body) g_free(body);
    if (prompt_buf) g_free(prompt_buf);
    if (text) g_free(text);
    llama_http_result_free(&http);
    return response;
}

/*---------------------------------------------------------------------------
 * Public API: initialization and cleanup
 *---------------------------------------------------------------------------*/

LlamaClient *llama_client_create(const char *server_url, const char *model)
{
    LlamaClient *client = g_new0(LlamaClient, 1);
    if (!client) return NULL;

    if (pthread_mutex_init(&client->mutex, NULL) != 0) {
        g_free(client);
        return NULL;
    }
    client->server_url[0] = '\0';
    client->model[0] = '\0';
    client->inflight = NULL;
    client->error_code = LLAMA_ERR_OK;
    client->error_message[0] = '\0';

    llama_client_set_url(client, server_url);
    llama_client_set_model(client, model);
    return client;
}

void llama_client_destroy(LlamaClient *client)
{
    if (!client) return;

    llama_client_cancel(client);

    pthread_mutex_lock(&client->mutex);
    GList *l = client->inflight;
    while (l) {
        GList *next = l->next;
        g_object_unref(l->data);
        g_list_free_1(l);
        l = next;
    }
    client->inflight = NULL;
    pthread_mutex_unlock(&client->mutex);

    pthread_mutex_destroy(&client->mutex);
    g_free(client);
}

/*---------------------------------------------------------------------------
 * Public API: configuration
 *---------------------------------------------------------------------------*/

void llama_client_set_url(LlamaClient *client, const char *server_url)
{
    if (!client) return;

    pthread_mutex_lock(&client->mutex);
    if (server_url && server_url[0] != '\0' &&
        strlen(server_url) < sizeof(client->server_url)) {
        snprintf(client->server_url, sizeof(client->server_url), "%s", server_url);
        size_t len = strlen(client->server_url);
        while (len > 0 && client->server_url[len - 1] == '/') {
            client->server_url[--len] = '\0';
        }
    } else {
        if (server_url && server_url[0] != '\0') {
            g_log("app-llama", G_LOG_LEVEL_WARNING,
                  "[llama] server URL too long (%zu chars), keeping previous value\n",
                  strlen(server_url));
        }
        client->server_url[0] = '\0';
    }
    pthread_mutex_unlock(&client->mutex);
}

void llama_client_set_model(LlamaClient *client, const char *model)
{
    if (!client) return;

    pthread_mutex_lock(&client->mutex);
    if (model && model[0] != '\0' &&
        strlen(model) < sizeof(client->model)) {
        snprintf(client->model, sizeof(client->model), "%s", model);
    } else {
        if (model && model[0] != '\0') {
            g_log("app-llama", G_LOG_LEVEL_WARNING,
                  "[llama] model name too long (%zu chars), keeping previous value\n",
                  strlen(model));
        }
        client->model[0] = '\0';
    }
    pthread_mutex_unlock(&client->mutex);
}

/*---------------------------------------------------------------------------
 * Public API: connection health check
 *---------------------------------------------------------------------------*/

bool llama_check_connection(LlamaClient *client)
{
    if (!client) return false;

    char url[LLAMA_MAX_URL_LEN];
    pthread_mutex_lock(&client->mutex);
    snprintf(url, sizeof(url), "%s", client->server_url);
    pthread_mutex_unlock(&client->mutex);

    if (url[0] == '\0') {
        set_client_error(client, LLAMA_ERR_INVALID_PARAM, "server URL not configured");
        return false;
    }

    LlamaHttpResult http;
    http.status_code = 0;
    http.body = NULL;
    http.error = LLAMA_ERR_OK;
    http.timed_out = false;

    llama_http_request(client, url, "GET", "/health", NULL, 0, &http);

    bool ok = (http.error == LLAMA_ERR_OK &&
               http.status_code == 200 &&
               http.body != NULL &&
               strstr(http.body, "ok") != NULL);

    if (!ok) {
        char detail[LLAMA_MAX_ERROR_LEN];
        if (http.error == LLAMA_ERR_CONNECT) {
            snprintf(detail, sizeof(detail),
                     "cannot connect to llama-server at %.190s (is it running?)", url);
        } else if (http.error == LLAMA_ERR_READ_TIMEOUT) {
            snprintf(detail, sizeof(detail),
                     "llama-server at %.200s timed out", url);
        } else if (http.error == LLAMA_ERR_CANCELLED) {
            snprintf(detail, sizeof(detail), "health check cancelled");
        } else if (http.error != LLAMA_ERR_OK) {
            snprintf(detail, sizeof(detail), "health check of %.200s failed", url);
        } else if (http.status_code != 200) {
            snprintf(detail, sizeof(detail),
                     "llama-server %.190s returned HTTP %d on /health",
                     url, http.status_code);
        } else {
            snprintf(detail, sizeof(detail),
                     "llama-server %.200s did not report healthy", url);
        }
        set_client_error(client, http.error != LLAMA_ERR_OK ? http.error : LLAMA_ERR_HTTP,
                         detail);
        g_log("app-llama", G_LOG_LEVEL_MESSAGE, "[llama] %s\n", detail);
    } else {
        set_client_error(client, LLAMA_ERR_OK, NULL);
        g_log("app-llama", G_LOG_LEVEL_MESSAGE,
              "[llama] llama-server at %s is healthy\n", url);
    }

    llama_http_result_free(&http);
    return ok;
}

int llama_list_models(LlamaClient *client,
                      char (*names_out)[LLAMA_MAX_MODEL_NAME],
                      int max_names)
{
    if (!client || !names_out || max_names < 1 ||
        max_names > LLAMA_MAX_MODELS) {
        if (client) {
            set_client_error(client, LLAMA_ERR_INVALID_PARAM,
                             "invalid arguments");
        }
        return -1;
    }

    char url[LLAMA_MAX_URL_LEN];
    pthread_mutex_lock(&client->mutex);
    snprintf(url, sizeof(url), "%s", client->server_url);
    pthread_mutex_unlock(&client->mutex);

    if (url[0] == '\0') {
        set_client_error(client, LLAMA_ERR_INVALID_PARAM, "server URL not configured");
        return -1;
    }

    LlamaHttpResult http;
    http.status_code = 0;
    http.body = NULL;
    http.error = LLAMA_ERR_OK;
    http.timed_out = false;

    llama_http_request(client, url, "GET", "/v1/models", NULL, 0, &http);

    if (http.error != LLAMA_ERR_OK || http.status_code != 200 ||
        http.body == NULL) {
        char detail[LLAMA_MAX_ERROR_LEN];
        if (http.error == LLAMA_ERR_CONNECT) {
            snprintf(detail, sizeof(detail),
                     "cannot connect to llama-server at %.190s (is it running?)", url);
        } else if (http.error == LLAMA_ERR_READ_TIMEOUT) {
            snprintf(detail, sizeof(detail),
                     "timed out while reading the model list from %.190s", url);
        } else if (http.status_code != 0 && http.status_code != 200) {
            snprintf(detail, sizeof(detail),
                     "llama-server %.190s returned HTTP %d on /v1/models",
                     url, http.status_code);
        } else {
            snprintf(detail, sizeof(detail),
                     "llama-server %.200s returned an empty /v1/models response", url);
        }
        set_client_error(client,
                         http.error != LLAMA_ERR_OK ? http.error : LLAMA_ERR_HTTP,
                         detail);
        llama_http_result_free(&http);
        return -1;
    }

    /* Parse: llama.cpp answers {"models": [...]}; OpenAI-compatible
     * gateways answer {"data": [...]}. Each entry carries the name under
     * "name" (llama.cpp also mirrors it in "model"). */
    cJSON *root = cJSON_Parse(http.body);
    llama_http_result_free(&http);
    if (!root) {
        set_client_error(client, LLAMA_ERR_RESPONSE_PARSE,
                         "invalid JSON in /v1/models response");
        return -1;
    }

    int count = 0;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_GetObjectItemCaseSensitive(root, "data");
    }
    if (cJSON_IsArray(arr)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, arr) {
            if (count >= max_names) break;  /* Caller buffer full — drop the rest. */
            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (!cJSON_IsString(name) || name->valuestring == NULL ||
                name->valuestring[0] == '\0') {
                name = cJSON_GetObjectItemCaseSensitive(item, "model");
            }
            if (!cJSON_IsString(name) || name->valuestring == NULL ||
                name->valuestring[0] == '\0') {
                continue;  /* Entry without a usable name — skip. */
            }
            g_strlcpy(names_out[count], name->valuestring,
                      LLAMA_MAX_MODEL_NAME);
            count++;
        }
    }
    cJSON_Delete(root);

    g_log("app-llama", G_LOG_LEVEL_MESSAGE,
          "[llama] server at %s advertises %d model(s)\n", url, count);
    return count;
}

/*---------------------------------------------------------------------------
 * Public API: core transcription
 *---------------------------------------------------------------------------*/

WhisperResponse *llama_transcribe_samples(LlamaClient *client,
                                          const int16_t *samples,
                                          int n_samples,
                                          const char *language)
{
    if (!client || !samples || n_samples <= 0 ||
        (size_t)n_samples > LLAMA_MAX_SAMPLES) {
        WhisperResponse *response = g_new0(WhisperResponse, 1);
        if (response) {
            fill_response_error(response, LLAMA_ERR_INVALID_PARAM,
                                "invalid parameters (need client, samples, 0 < n_samples <= "
                                "max)");
            set_client_error(client, LLAMA_ERR_INVALID_PARAM,
                             "invalid parameters for llama_transcribe_samples");
        }
        return response;
    }

    size_t data_size = (size_t)n_samples * sizeof(int16_t);
    uint8_t *wav = g_malloc(LLAMA_WAV_HEADER_SIZE + data_size);
    if (!wav) {
        WhisperResponse *response = g_new0(WhisperResponse, 1);
        if (response) {
            fill_response_error(response, LLAMA_ERR_ALLOC, "memory allocation failed");
            set_client_error(client, LLAMA_ERR_ALLOC, "memory allocation failed");
        }
        return response;
    }

    build_wav_header(wav, (uint32_t)data_size);
    memcpy(wav + LLAMA_WAV_HEADER_SIZE, samples, data_size);

    WhisperResponse *response =
        transcribe_wav_bytes(client, wav, LLAMA_WAV_HEADER_SIZE + data_size, language);
    g_free(wav);
    return response;
}

WhisperResponse *llama_transcribe_wav(LlamaClient *client,
                                      const char *wav_path,
                                      const char *language)
{
    if (!client || !wav_path || wav_path[0] == '\0') {
        WhisperResponse *response = g_new0(WhisperResponse, 1);
        if (response) {
            fill_response_error(response, LLAMA_ERR_INVALID_PARAM,
                                "invalid parameters (need client and non-empty wav_path)");
            set_client_error(client, LLAMA_ERR_INVALID_PARAM,
                             "invalid parameters for llama_transcribe_wav");
        }
        return response;
    }

    char detail[LLAMA_MAX_ERROR_LEN];
    if (!validate_wav_file(wav_path, detail, sizeof(detail))) {
        WhisperResponse *response = g_new0(WhisperResponse, 1);
        if (response) {
            fill_response_error(response, LLAMA_ERR_INVALID_PARAM, detail);
            set_client_error(client, LLAMA_ERR_INVALID_PARAM, detail);
            g_log("app-llama", G_LOG_LEVEL_MESSAGE, "[llama] %s\n", detail);
        }
        return response;
    }

    char *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(wav_path, &data, &len, NULL)) {
        WhisperResponse *response = g_new0(WhisperResponse, 1);
        if (response) {
            snprintf(detail, sizeof(detail), "cannot read WAV file: %.220s", wav_path);
            fill_response_error(response, LLAMA_ERR_INVALID_PARAM, detail);
            set_client_error(client, LLAMA_ERR_INVALID_PARAM, detail);
        }
        return response;
    }

    WhisperResponse *response = transcribe_wav_bytes(client, (const uint8_t *)data,
                                                     len, language);
    g_free(data);
    return response;
}

/*---------------------------------------------------------------------------
 * Public API: transcription with auto-fallback to local Whisper
 *---------------------------------------------------------------------------*/

WhisperResponse *llama_transcribe_samples_fallback(LlamaClient *client,
                                                   WhisperClient *whisper,
                                                   const int16_t *samples,
                                                   int n_samples,
                                                   const char *language)
{
    WhisperResponse *response =
        llama_transcribe_samples(client, samples, n_samples, language);

    if (response && response->success) {
        return response;
    }
    if (response && response->error_code == LLAMA_ERR_CANCELLED) {
        /* User-initiated cancel: do not retry through Whisper. */
        return response;
    }

    const char *llama_err = (response && response->error_message[0] != '\0')
                           ? response->error_message
                           : "NULL response from llama backend";
    g_log("app-llama", G_LOG_LEVEL_WARNING,
          "[llama] transcription failed (%s) — falling back to local Whisper\n",
          llama_err);

    if (!whisper) {
        return response;  /* no fallback target available */
    }

    WhisperResponse *whisper_response =
        whisper_transcribe_samples(whisper, samples, n_samples);
    if (whisper_response) {
        if (response) whisper_response_free(response);
        return whisper_response;  /* success or failure — Whisper's result wins */
    }

    g_log("app-llama", G_LOG_LEVEL_ERROR,
          "[llama] Whisper fallback also failed: %s\n",
          whisper_client_get_error(whisper) ? whisper_client_get_error(whisper)
                                            : "unknown error");
    return response;  /* return the original llama error */
}

WhisperResponse *llama_transcribe_wav_fallback(LlamaClient *client,
                                               WhisperClient *whisper,
                                               const char *wav_path,
                                               const char *language)
{
    WhisperResponse *response = llama_transcribe_wav(client, wav_path, language);

    if (response && response->success) {
        return response;
    }
    if (response && response->error_code == LLAMA_ERR_CANCELLED) {
        return response;
    }

    const char *llama_err = (response && response->error_message[0] != '\0')
                           ? response->error_message
                           : "NULL response from llama backend";
    g_log("app-llama", G_LOG_LEVEL_WARNING,
          "[llama] transcription failed (%s) — falling back to local Whisper\n",
          llama_err);

    if (!whisper) {
        return response;
    }

    WhisperResponse *whisper_response =
        whisper_transcribe_with_retry(whisper, wav_path, 0);
    if (whisper_response) {
        if (response) whisper_response_free(response);
        return whisper_response;
    }

    g_log("app-llama", G_LOG_LEVEL_ERROR,
          "[llama] Whisper fallback also failed: %s\n",
          whisper_client_get_error(whisper) ? whisper_client_get_error(whisper)
                                            : "unknown error");
    return response;
}

/*---------------------------------------------------------------------------
 * Public API: cancellation
 *---------------------------------------------------------------------------*/

void llama_client_cancel(LlamaClient *client)
{
    if (!client) return;

    pthread_mutex_lock(&client->mutex);
    GList *l = client->inflight;
    while (l) {
        g_cancellable_cancel((GCancellable *)l->data);
        l = l->next;
    }
    pthread_mutex_unlock(&client->mutex);
}

/*---------------------------------------------------------------------------
 * Public API: error diagnostics
 *---------------------------------------------------------------------------*/

const char *llama_client_get_error(const LlamaClient *client)
{
    static __thread char local_buffer[LLAMA_MAX_ERROR_LEN] = {0};
    if (!client) return "";

    LlamaClient *c = (LlamaClient *)(intptr_t)client;
    pthread_mutex_lock(&c->mutex);
    snprintf(local_buffer, sizeof(local_buffer), "%s", c->error_message);
    pthread_mutex_unlock(&c->mutex);
    return local_buffer;
}
