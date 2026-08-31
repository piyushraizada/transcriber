/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * app_file_upload.h — File Upload, Validation & Transcription Pipeline
 *
 * This file provides the complete file-based transcription workflow:
 * - Audio format detection via FFmpeg (any container/codec FFmpeg can
 *   decode — no extension allowlist; FFmpeg errors are surfaced to the
 *   user)
 * - File size validation (max 1GB)
 * - Duration validation (min 2s, max 600s) — enforced after decode, since
 *   container duration metadata is not always present
 * - FFmpeg decode + resample to 16kHz mono 16-bit PCM
 * - Background transcription pipeline (FileUploadJob) that segments the
 *   decoded PCM with VAD silence detection and transcribes each segment
 *   with whisper.cpp, appending results to the output file incrementally
 *
 * Threading: file_upload_validate() and file_upload_decode_to_pcm() are
 * synchronous and intended to run on the GTK main thread (local-file probes
 * are fast). file_upload_run_pipeline() is a GThread entry point and must
 * NOT be called from the GTK main thread. The job's on_progress callback
 * fires on the pipeline thread — the caller must marshal it to the GTK
 * thread (e.g. via g_idle_add).
 *
 * Uses FFmpeg (libavformat/libavcodec/libswresample) for probing, decoding,
 * and resampling.
 */

#ifndef APP_FILE_UPLOAD_H
#define APP_FILE_UPLOAD_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>

/*
 * FileUploadContext — Context for file upload operations
 *
 * Tracks the state and metadata for a single file upload operation.
 */
typedef struct FileUploadContext FileUploadContext;

struct FileUploadContext {
    /* File metadata */
    gchar *filepath;           /* Original file path */
    gchar *filename;           /* Just the filename */
    gsize file_size;           /* File size in bytes */
    double duration;           /* Duration in seconds */
    
    /* Validation state */
    gboolean is_valid;         /* Whether file passed all validation checks */
    gchar *error_message;      /* Error message if validation failed */
    
    /* Temporary file for segmentation */
    gchar *temp_filepath;      /* Temporary file path for PCM extraction */
    
    /* Progress tracking */
    gboolean is_processing;    /* Whether file is currently being processed */
    
    /* Callbacks */
    void (*on_validation_complete)(FileUploadContext *ctx, gboolean success, const gchar *error);
    void *user_data;           /* User data for callbacks */
};

/*
 * FileUploadResult — Result of file validation
 *
 * Contains the outcome of file validation checks. Returned BY VALUE —
 * the error_message pointer aliases ctx->error_message and is valid
 * until file_upload_cleanup() is called.
 */
typedef struct {
    gboolean success;           /* Whether validation succeeded */
    gchar *error_message;      /* Error message if validation failed */
    double duration;           /* Duration in seconds if valid */
    gsize file_size;           /* File size in bytes if valid */
} FileUploadResult;

/* Forward declaration — opaque whisper client handle (see app_whisper.h).
 * Declared with the same struct tag so the pointer types are compatible
 * without including app_whisper.h (avoids an include cycle). */
typedef struct _WhisperClient WhisperClient;

/* Forward declaration — opaque llama-server ASR client handle (see
 * app_llama.h). Same tag as app_llama.h so the pointer types are
 * compatible without including it (avoids an include cycle). */
typedef struct _LlamaClient LlamaClient;

/*
 * FileUploadError — Categories of upload errors
 *
 * Used for consistent error reporting and recovery.
 */
typedef enum {
    FILE_UPLOAD_ERROR_NONE,
    FILE_UPLOAD_ERROR_FILE_NOT_FOUND,
    FILE_UPLOAD_ERROR_INVALID_FORMAT,
    FILE_UPLOAD_ERROR_FILE_TOO_LARGE,
    FILE_UPLOAD_ERROR_FILE_TOO_SMALL,
    FILE_UPLOAD_ERROR_PERMISSION_DENIED,
    FILE_UPLOAD_ERROR_READ_FAILED,
    FILE_UPLOAD_ERROR_PCM_EXTRACTION_FAILED,
    FILE_UPLOAD_ERROR_TEMP_FILE_CREATION_FAILED,
    FILE_UPLOAD_ERROR_COUNT
} FileUploadError;

/*
 * Public API Functions
 *
 * These functions provide the interface for file upload and validation.
 */

/*
 * file_upload_init — Initialize file upload context
 *
 * @param ctx: Pointer to FileUploadContext to initialize
 * @param filepath: Path to the file to upload
 * @param on_validation_complete: Callback for validation completion
 * @param user_data: User data for callback
 * @return: 0 on success, negative error code on failure
 */
int file_upload_init(FileUploadContext *ctx, const gchar *filepath,
                     void (*on_validation_complete)(FileUploadContext *ctx, gboolean success, const gchar *error),
                     void *user_data);

/*
 * file_upload_validate — Validate uploaded file
 *
 * Checks:
 * - File exists and is readable
 * - File size is within limits (max 1GB)
 * - File is decodable by FFmpeg and contains an audio stream (any format
 *   FFmpeg supports — no extension allowlist). On failure, ctx->error_message
 *   holds the human-readable FFmpeg error for display to the user.
 *
 * Duration is probed best-effort: ctx->duration is 0.0 when the container
 * carries no duration metadata. The authoritative 2-600 s duration check is
 * performed in file_upload_run_pipeline() after decode.
 *
 * @param ctx: FileUploadContext to validate
 * @return: 0 on success, negative error code on failure
 */
int file_upload_validate(FileUploadContext *ctx);

/*
 * file_upload_cleanup — Clean up file upload resources
 *
 * Removes temporary files and frees allocated memory.
 *
 * @param ctx: FileUploadContext to clean up
 */
void file_upload_cleanup(FileUploadContext *ctx);

/*
 * file_upload_get_result — Get validation result
 *
 * @param ctx: FileUploadContext to get result from
 * @return: FileUploadResult by value. success is FALSE and error_message
 *          is NULL if ctx is NULL. error_message aliases ctx->error_message
 *          (valid until file_upload_cleanup()).
 */
FileUploadResult file_upload_get_result(FileUploadContext *ctx);

/*
 * file_upload_decode_to_pcm — Decode and resample the audio file to PCM
 *
 * Decodes the audio stream of ctx->filepath using FFmpeg and resamples it
 * to 16kHz mono 16-bit PCM (the format whisper.cpp requires). The output
 * buffer is g_malloc'd and must be freed by the caller with g_free().
 *
 * @param ctx: Validated FileUploadContext
 * @param out_samples: Receives the g_malloc'd int16 PCM buffer
 * @param out_count: Receives the number of int16 samples
 * @param out_error: Receives a g_strdup'd human-readable error (the FFmpeg
 *                   message where applicable) on failure; the caller frees
 *                   it with g_free(). Set to NULL on success.
 * @return: 0 on success, negative error code on failure
 */
int file_upload_decode_to_pcm(FileUploadContext *ctx,
                              int16_t **out_samples, size_t *out_count,
                              gchar **out_error);

/*
 * FileUploadJob — A running file transcription operation
 *
 * Owns the output file handle and cancellation state for one transcription
 * run. Created on the GTK main thread, consumed by the pipeline thread
 * (file_upload_run_pipeline), and destroyed on the GTK main thread after
 * the pipeline thread has exited.
 */
typedef struct FileUploadJob FileUploadJob;

/*
 * FileUploadJobConfig — Parameters for creating a FileUploadJob
 *
 * whisper_mutex must be the same mutex the rest of the app uses around
 * whisper calls (scanner_transcribe_mutex in main.c) so file transcription
 * never races live/continuous transcription.
 */
typedef struct {
    WhisperClient *whisper_client;   /* Client to transcribe with (not owned) */
    LlamaClient *llama_client;       /* Second ASR backend (not owned); NULL
                                        when unavailable — the whisper backend
                                        is then always used */
    pthread_mutex_t *whisper_mutex;  /* Mutex guarding transcription calls
                                        (not owned). Also held around the
                                        llama HTTP dispatch, so file
                                        transcription never races live/
                                        continuous transcription. */
    const char *input_path;          /* Source audio file (not owned) */
    const char *output_path;         /* Destination .txt file (not owned) */
    double total_duration;           /* Seconds, for progress reporting */
    char asr_backend[16];            /* "whisper" or "llama" — snapshot of the
                                        active backend at job creation; every
                                        segment of this job uses it */
    char language[16];               /* Configured language; passed to the
                                        llama prompt builder (auto/empty means
                                        no language instruction) */
    void (*on_progress)(double percent, const char *status, void *user_data);
    void *user_data;
} FileUploadJobConfig;

/*
 * file_upload_job_create — Create a job and open the output file
 *
 * @param cfg: Job configuration (all pointers must remain valid)
 * @return: Job handle, or NULL if the output file could not be opened
 */
FileUploadJob *file_upload_job_create(const FileUploadJobConfig *cfg);

/*
 * file_upload_job_destroy — Free the job (output file must be closed first)
 *
 * @param job: Job handle (NULL is safe)
 */
void file_upload_job_destroy(FileUploadJob *job);

/*
 * file_upload_job_request_cancel — Signal the pipeline to stop
 *
 * Thread-safe. The pipeline checks the flag between segments; the caller
 * should also call whisper_client_cancel() to abort an in-flight inference.
 *
 * @param job: Job handle (NULL is safe)
 */
void file_upload_job_request_cancel(FileUploadJob *job);

/*
 * file_upload_job_is_cancelled — Query the cancellation flag
 *
 * @param job: Job handle
 * @return: true if cancellation was requested
 */
bool file_upload_job_is_cancelled(const FileUploadJob *job);

/*
 * file_upload_run_pipeline — GThread entry point for file transcription
 *
 * Decodes the input to 16kHz mono PCM, segments it with VAD-based silence
 * detection, transcribes each segment with whisper.cpp, and appends the
 * text to the output file after each segment. On cancellation the partial
 * output file is deleted. Progress is reported via the job's callback
 * (which the caller is responsible for marshaling to the GTK thread).
 *
 * @param arg: FileUploadJob*
 * @return: Always NULL
 */
void *file_upload_run_pipeline(void *arg);

/*
 * file_upload_get_error — Get error for FileUploadError enum
 *
 * @param error: FileUploadError to get message for
 * @return: Static error message string
 */
const gchar *file_upload_get_error(FileUploadError error);

#endif /* APP_FILE_UPLOAD_H */
