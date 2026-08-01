/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * main.c — Application Entry Point, State Controller, Recording Lifecycle,
 *          Runtime Config Updates, Recording Completion Beep
 *
 * This file provides the main() entry point that initializes all subsystems,
 *      creates the MainWindow, sets up D-Bus service, starts the GTK
 *      main loop, and handles graceful shutdown.
 *
 * Application Startup Sequence:
 *   1. Initialize GTK3
 *   2. Load configuration from ~/.config/transcriber/config.json
 *   3. Initialize the AppStateController
 *   4. Create the AudioRecorder with configured device
 *   5. Create the WhisperClient
 *   6. Create the MainWindow and TextWindow
 *   7. Register toggle callback (mic icon click → start/stop recording)
 *   8. Register config-changed callback (apply runtime config updates)
 *   9. Start the D-Bus service (single-instance enforcement)
 *   10. Perform initial model availability check
 *   11. Enter the GTK main loop
 *
 * Application Shutdown Sequence:
 *   1. Stop the D-Bus service
 *   2. Stop any running animations and countdown timer
 *   3. Save window position
 *   4. Destroy MainWindow and TextWindow
 *   5. Cleanup AppStateController
 *   6. Uninitialize GTK
 *
 * Recording Lifecycle:
 *   - IDLE → LISTENING: Start audio recording, start watchdog timer,
 *     start sine wave animation, start countdown timer
 *   - LISTENING → TRANSCRIBING: Stop recording (emit beep), stop watchdog,
 *     stop animation/countdown, start transcription watchdog (30s),
 *     spawn transcription thread
 *   - TRANSCRIBING → IDLE: Stop transcription watchdog, display result,
 *     clear clipboard, return to idle state
 *
 * Runtime Config Updates:
 *   - When the user saves changes in the Configuration Dialog, the
 *     config-changed callback (on_config_changed) is invoked, which
 *     applies the new audio device to the running AudioRecorder
 *     immediately without requiring a restart.
 *
 * Recording Completion Beep:
 *   - When recording finishes (handle_enter_transcribing), a GTK window
 *     bell is used to trigger the system beep.
 *
 * Threading:
 *   - Main thread: GTK main loop, UI updates, D-Bus message processing
 *   - Audio thread: Created by app_audio.c for PCM capture
 *   - Transcription thread: Created by app_whisper.c for local whisper.cpp transcription
 *
 * All cross-thread communication flows through callback function pointers
 * registered with the AppStateController, marshaled to the GTK main thread
 * via g_idle_add().
 *
 * Encapsulation:
 *   - All application state is encapsulated in the TranscriberApp struct,
 *     eliminating file-scope static globals and improving testability.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <time.h>

#include "app.h"
#include "app_audio.h"
#include "app_silence_scanner.h"
#include "app_vad.h"
#include "app_whisper.h"
#include "whisper.h"
#include "app_config.h"
#include "app_config_dialog.h"
#include "app_clipboard.h"
#include "app_dbus.h"
#include "app_tray.h"
#include "app_window.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* File-based logging via GLib log handler                             */
/* ------------------------------------------------------------------ */

/**
 * Global flag controlling whether DEBUG/MESSAGE levels are logged.
 * Set to false by default; updated when the user toggles "Debug logs" in settings.
 * Both file and stderr log handlers check this flag to filter output consistently.
 */
static bool g_debug_logs_enabled = false;

/**
 * Stderr log handler — conditionally filters output based on debug_logs setting.
 * When debug_logs is false, only ERROR, CRITICAL, WARNING, and INFO are printed.
 * When debug_logs is true, all levels including DEBUG and MESSAGE pass through.
 */
static void transcriber_stderr_log_handler(const gchar *log_domain,
                                            GLogLevelFlags log_level,
                                            const gchar *message,
                                            gpointer user_data) {
    (void)user_data;

    /* Filter out DEBUG/MESSAGE when debug logging is disabled */
    if (!g_debug_logs_enabled &&
        !(log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_INFO))) {
        return;
    }

    const char *level;
    if (log_level & G_LOG_LEVEL_ERROR)       level = "ERROR";
    else if (log_level & G_LOG_LEVEL_CRITICAL) level = "CRITICAL";
    else if (log_level & G_LOG_LEVEL_WARNING)  level = "WARNING";
    else if (log_level & G_LOG_LEVEL_MESSAGE)  level = "MESSAGE";
    else if (log_level & G_LOG_LEVEL_INFO)     level = "INFO";
    else                                        level = "DEBUG";

    fprintf(stderr, "[%s] %s: %s\n",
            log_domain ? log_domain : "(null)",
            level, message ? message : "(null)");
}

/**
 * List of all application-specific log domains for iterating.
 */
static const char *LOG_DOMAINS[] = {
    NULL, "main", "app-audio", "app-scanner", "app-whisper",
    "app-config", "app-gpu", "app-tray", "app_window", "app-ringbuffer"
};
#define LOG_DOMAINS_COUNT (sizeof(LOG_DOMAINS) / sizeof(LOG_DOMAINS[0]))

/**
 * Flush and close the static log file handle opened by transcriber_file_log_handler().
 * Call this during application shutdown to release the file descriptor and ensure
 * all pending buffered data is written. Safe to call multiple times or before the
 * log handler has been invoked (no-op in both cases).
 */
static FILE *g_log_file_handle = NULL;

/* File-based log handler that writes g_log() messages to /tmp/transcriber.log.
 * Uses a module-level static FILE* so it can be explicitly closed during shutdown. */
static void transcriber_file_log_handler(const gchar *log_domain,
                                          GLogLevelFlags log_level,
                                          const gchar *message,
                                          gpointer user_data) {
    (void)user_data;

    /* Filter out DEBUG/MESSAGE when debug logging is disabled */
    if (!g_debug_logs_enabled &&
        !(log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_INFO))) {
        return;
    }

    if (!g_log_file_handle) {
        g_log_file_handle = fopen("/tmp/transcriber.log", "w");  /* Truncate on each startup */
        if (!g_log_file_handle) return;
        setlinebuf(g_log_file_handle);   /* Line-buffered so each message flushes immediately */
    }

    /* Format a human-readable timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    char time_str[32];
    if (localtime_r(&now, &tm_buf)) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);
    } else {
        strcpy(time_str, "???");
    }

    /* Map log level flags to a short label */
    const char *level;
    if (log_level & G_LOG_LEVEL_ERROR)       level = "ERROR";
    else if (log_level & G_LOG_LEVEL_CRITICAL) level = "CRITICAL";
    else if (log_level & G_LOG_LEVEL_WARNING)  level = "WARNING";
    else if (log_level & G_LOG_LEVEL_MESSAGE)  level = "MESSAGE";
    else if (log_level & G_LOG_LEVEL_INFO)     level = "INFO";
    else                                        level = "DEBUG";

    /* Write: [timestamp] [level] domain: message */
    fprintf(g_log_file_handle, "[%s] [%s] %s: %s\n", time_str, level,
            log_domain ? log_domain : "(null)",
            message ? message : "(null)");
}

static void close_log_file(void) {
    if (g_log_file_handle) {
        fflush(g_log_file_handle);
        fclose(g_log_file_handle);
        g_log_file_handle = NULL;
    }
    if (stderr) {
        fflush(stderr);
    }
}

/**
 * Register log handlers for every domain used by our application.
 * Both file and stderr handlers filter based on the shared g_debug_logs_enabled flag.
 */
static void register_all_log_handlers(AppConfig *config) {
    GLogLevelFlags mask =
        G_LOG_FLAG_RECURSION | G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL |
        G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_INFO | G_LOG_LEVEL_DEBUG;

    g_debug_logs_enabled = config ? config_get_debug_logs(config) : false;

    for (size_t i = 0; i < LOG_DOMAINS_COUNT; i++) {
        g_log_set_handler(LOG_DOMAINS[i], mask, transcriber_file_log_handler, NULL);
        g_log_set_handler(LOG_DOMAINS[i], mask, transcriber_stderr_log_handler, NULL);
    }
}

/**
 * Update the global debug logs flag when the user toggles "Debug logs" in settings.
 * Both file and stderr handlers check this flag on every call, so no handler
 * re-registration is needed — just flip the flag for immediate effect.
 */
static void reinstall_stderr_handlers(AppConfig *config) {
    g_debug_logs_enabled = config ? config_get_debug_logs(config) : false;
}

/* ------------------------------------------------------------------ */
/* TranscriberApp — Encapsulated Application State                     */
/* ------------------------------------------------------------------ */

/**
 * TranscriberApp — Central application state struct.
 *
 * Replaces file-scope static globals with a single encapsulated struct.
 * All callbacks and helper functions receive a pointer to this struct
 * via their user_data parameter, eliminating implicit global access.
 */
typedef struct TranscriberApp {
    /* Core subsystems */
    AppStateController controller;
    MainWindow *main_window;
    TextWindow *text_window;
    DBusService *dbus_service;
    AudioRecorder *audio_recorder;
    WhisperClient *whisper_client;
    SystemTray *tray;
    SilenceScanner *silence_scanner;

    /* Configuration (owned by the app, lifetime = app lifetime) */
    AppConfig config;

    /* Timer source IDs */
    guint watchdog_source_id;
    guint transcription_watchdog_source_id;
    guint volume_poll_source_id;

    /* WAV path for transcription (protected by wav_path_mutex) */
    char *current_wav_path;
    pthread_mutex_t wav_path_mutex;

    /* Model loading state */
    atomic_int model_loading_from_toggle;

    /* Thread handles for clean shutdown */
    GThread *transcribe_thread;
    pthread_mutex_t transcribe_thread_mutex;
    GThread *model_load_thread;
    pthread_mutex_t model_load_thread_mutex;

    /* Shutdown flag — atomic for portable cross-thread visibility */
    atomic_bool shutting_down;

    /* User explicitly clicked mic to stop recording in continuous mode.
     * When set, on_transcription_result will transition to IDLE instead
     * of restarting recording, even if continuous_dictation is enabled. */
    atomic_bool user_requested_stop;

    /* Mutex for scanner segment callback — protects concurrent transcription */
    pthread_mutex_t scanner_transcribe_mutex;

    /* Accumulated transcribed text for clipboard in continuous mode.
     * Each successful transcription appends to this buffer so the clipboard
     * always holds the full transcript, not just the last segment. Protected
     * by continuous_clipboard_mutex. */
    char *continuous_clipboard_text;
    pthread_mutex_t continuous_clipboard_mutex;
} TranscriberApp;

/* ------------------------------------------------------------------ */
/* Forward declarations for internal callbacks                         */
/* ------------------------------------------------------------------ */

static void on_transcription_result(TranscriberApp *app, const char *text, bool success);
static void on_model_status_change(TranscriberApp *app, ModelStatus status);
static void on_dbus_toggle(void *user_data);
static void on_dbus_activate(void *user_data);
static void on_config_changed(void *user_data);
static gboolean watchdog_timer_callback(gpointer user_data);
static gpointer transcribe_thread_func(gpointer data);
static gboolean volume_poll_callback(gpointer data);
static gpointer model_loading_thread_func(gpointer data);
static gboolean on_model_loaded_idle(gpointer data);
static gboolean on_model_load_failed_idle(gpointer data);
static void on_microphone_toggle(void *user_data);
static void on_state_change(TranscriberApp *app, AppState previous_state, AppState new_state);
static void show_auto_close_dialog(TranscriberApp *app, const char *title,
                                   GtkMessageType type, const char *format, ...);
static void handle_enter_listening(TranscriberApp *app, AppState previous_state);
static void handle_enter_transcribing(TranscriberApp *app, const char *wav_path);
static void on_scanner_segment(int16_t *samples, size_t count, void *user_data);
static gboolean restart_watchdog_idle(gpointer user_data);
static void start_watchdog_timer(TranscriberApp *app);
static void stop_watchdog_timer(TranscriberApp *app);
static void start_transcription_watchdog(TranscriberApp *app);
static void stop_transcription_watchdog(TranscriberApp *app);
static void start_volume_poll(TranscriberApp *app);
static void stop_volume_poll(TranscriberApp *app);
static void perform_initial_model_load(TranscriberApp *app);
static int get_transcription_timeout_seconds(TranscriberApp *app);
static void on_tray_clear(TranscriberApp *app);
static TranscriberApp *app_create(void);
static void app_destroy(TranscriberApp *app);

/* ------------------------------------------------------------------ */
/* Idle callback wrappers (GSourceFunc signature)                      */
/* ------------------------------------------------------------------ */

/**
 * Wrapper struct for idle callbacks that need both the app pointer and data.
 */
typedef struct {
    TranscriberApp *app;
    char *data;
} IdleCallbackData;

/**
 * Wrapper struct for model load failed idle callback.
 */
typedef struct {
    TranscriberApp *app;
    char *error_msg;
} ModelLoadFailedData;

static gboolean on_transcription_result_idle(gpointer data) {
    IdleCallbackData *icd = (IdleCallbackData *)data;
    TranscriberApp *app = icd->app;

    /* Bail out early if application is shutting down */
    if (app->shutting_down) {
        g_free(icd->data);
        g_free(icd);
        return FALSE;
    }
    on_transcription_result(app, icd->data ? icd->data : "", true);
    g_free(icd->data);
    g_free(icd);
    return FALSE;
}

static gboolean on_transcription_error_idle(gpointer data) {
    IdleCallbackData *icd = (IdleCallbackData *)data;
    TranscriberApp *app = icd->app;

    /* Bail out early if application is shutting down */
    if (app->shutting_down) {
        g_free(icd->data);
        g_free(icd);
        return FALSE;
    }
    on_transcription_result(app, icd->data, false);
    g_free(icd->data);
    g_free(icd);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/* Auto-close dialog timeout (seconds) */
#define DIALOG_AUTO_CLOSE_SECONDS 8

/* Volume poll interval (milliseconds) — ~10fps for smooth updates */
#define VOLUME_POLL_INTERVAL_MS 100

/* Default max recording duration (seconds) */
#define DEFAULT_MAX_DURATION_SECONDS 30

/* Minimum transcription timeout (seconds) */
#define MIN_TRANSCRIPTION_TIMEOUT_SECONDS 30

/* Maximum transcription timeout (seconds) */
#define MAX_TRANSCRIPTION_TIMEOUT_SECONDS 120

/* Maximum transcription retries on failure */
#define WHISPER_MAX_RETRIES 3

/* Volume level change threshold */
#define VOLUME_DELTA 0.05

/* ------------------------------------------------------------------ */
/* Dialog helpers                                                      */
/* ------------------------------------------------------------------ */

/* Wrapper for g_timeout_add: GSourceFunc signature requires gboolean return */
static gboolean auto_close_dialog(gpointer data) {
    gtk_widget_destroy(GTK_WIDGET(data));
    return FALSE;
}

/**
 * Show a non-modal, auto-closing error/warning dialog.
 *
 * Helper function to reduce boilerplate for the repeated GTK dialog pattern.
 * The dialog is non-modal (does not block the GTK main loop) and auto-closes
 * after DIALOG_AUTO_CLOSE_SECONDS.
 *
 * @param app         TranscriberApp instance.
 * @param title       Window title for the dialog.
 * @param type        GTK message type (GTK_MESSAGE_ERROR, GTK_MESSAGE_WARNING, etc.).
 * @param format      Printf-style format string for the message.
 * @param ...         Variable arguments for the format string.
 */
static void show_auto_close_dialog(TranscriberApp *app, const char *title,
                                   GtkMessageType type, const char *format, ...) {
    va_list args;
    char message[512];

    va_start(args, format);
    g_vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    GtkWindow *parent = app->main_window ? GTK_WINDOW(app_window_get_gtk_window(app->main_window)) : NULL;
    GtkDialog *dialog = GTK_DIALOG(gtk_message_dialog_new(
        parent,
        GTK_DIALOG_DESTROY_WITH_PARENT,
        type,
        GTK_BUTTONS_OK,
        "%s", message));
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    g_timeout_add_seconds(DIALOG_AUTO_CLOSE_SECONDS, auto_close_dialog, dialog);
    /* Show without triggering GNOME desktop notifications.
     * gtk_window_present() causes GNOME Shell to display a transient
     * notification bubble in the top panel, so we use show_all + deiconify
     * instead for a quieter experience, consistent with TextWindow behavior. */
    gtk_widget_show_all(GTK_WIDGET(dialog));
    gtk_window_deiconify(GTK_WINDOW(dialog));
}

/* ------------------------------------------------------------------ */
/* Watchdog Timer                                                      */
/* ------------------------------------------------------------------ */

/**
 * Watchdog timer callback.
 * Fires when the max recording duration is reached while in LISTENING state.
 * Transitions to TRANSCRIBING state.
 */
static gboolean watchdog_timer_callback(gpointer user_data) {
    TranscriberApp *app = (TranscriberApp *)user_data;

    AppState state = app_get_state(&app->controller);
    bool continuous = app->controller.config
        ? config_get_continuous_dictation(app->controller.config) : false;

    g_log("main", G_LOG_LEVEL_WARNING,
          "[watchdog] FIRED — state=%d continuous=%s\n",
          state, continuous ? "true" : "false");

    if (state == STATE_LISTENING) {
        /* Transition to TRANSCRIBING */
        if (app_transition_to(&app->controller, STATE_TRANSCRIBING)) {
            g_log("main", G_LOG_LEVEL_WARNING,
                  "[watchdog] Recording stopped by watchdog timer\n");
            /* Stop audio recording */
            if (app->audio_recorder) {
                audio_recorder_stop(app->audio_recorder);
            }
        }
    }

    /* Cancel the watchdog timer */
    app->watchdog_source_id = 0;
    return FALSE;
}

/**
 * Start the watchdog timer with the configured max duration.
 */
static void start_watchdog_timer(TranscriberApp *app) {
    if (app->watchdog_source_id != 0) {
        return; /* Already running */
    }

    int max_duration = app->controller.config->max_duration;
    if (max_duration <= 0) {
        max_duration = DEFAULT_MAX_DURATION_SECONDS;
    }

    /* Schedule the watchdog to fire after max_duration seconds */
    app->watchdog_source_id = g_timeout_add_seconds(max_duration,
                                                     watchdog_timer_callback,
                                                     app);
}

/**
 * Stop the watchdog timer.
 */
static void stop_watchdog_timer(TranscriberApp *app) {
    if (app->watchdog_source_id != 0) {
        g_source_remove(app->watchdog_source_id);
        app->watchdog_source_id = 0;
    }
}

/* Transcription phase watchdog timeout is now configurable via
 * the AppConfig max_duration field, scaled by a factor to allow longer
 * transcriptions for longer recordings. Minimum 30s, maximum 120s. */
static int get_transcription_timeout_seconds(TranscriberApp *app) {
    int base = app->controller.config ? app->controller.config->max_duration : DEFAULT_MAX_DURATION_SECONDS;
    /* Scale: 1.5x the recording duration, clamped to [MIN, MAX] */
    int scaled = base * 3 / 2;
    if (scaled < MIN_TRANSCRIPTION_TIMEOUT_SECONDS) scaled = MIN_TRANSCRIPTION_TIMEOUT_SECONDS;
    if (scaled > MAX_TRANSCRIPTION_TIMEOUT_SECONDS) scaled = MAX_TRANSCRIPTION_TIMEOUT_SECONDS;
    return scaled;
}

static gboolean transcription_watchdog_callback(gpointer user_data) {
    TranscriberApp *app = (TranscriberApp *)user_data;

    AppState state = app_get_state(&app->controller);
    if (state == STATE_TRANSCRIBING) {
        /* Force transition back to IDLE */
        app_transition_to(&app->controller, STATE_IDLE);
        if (app->text_window) {
            char msg[64];
            int timeout = get_transcription_timeout_seconds(app);
            snprintf(msg, sizeof(msg), "Transcription timed out (%ds limit)", timeout);
            app_text_window_set_error(app->text_window, msg);
        }
        if (app->main_window) {
            app_window_set_state(app->main_window, STATE_IDLE);
        }
    }

    app->transcription_watchdog_source_id = 0;
    return FALSE;
}

static void start_transcription_watchdog(TranscriberApp *app) {
    if (app->transcription_watchdog_source_id != 0) {
        return; /* Already running */
    }
    int timeout = get_transcription_timeout_seconds(app);
    app->transcription_watchdog_source_id = g_timeout_add_seconds(
        timeout, transcription_watchdog_callback, app);
}

static void stop_transcription_watchdog(TranscriberApp *app) {
    if (app->transcription_watchdog_source_id != 0) {
        g_source_remove(app->transcription_watchdog_source_id);
        app->transcription_watchdog_source_id = 0;
    }
}

/**
 * Idle callback to restart the watchdog timer after a scanner transcription.
 * Ensures continuous recording doesn't expire during long dictation sessions.
 */
static gboolean restart_watchdog_idle(gpointer user_data)
{
    TranscriberApp *app = (TranscriberApp *)user_data;
    g_log("main", G_LOG_LEVEL_DEBUG,
          "[watchdog] Restarting watchdog after successful scanner segment\n");
    stop_watchdog_timer(app);
    start_watchdog_timer(app);
    /* Restart the UI countdown timer so the user sees the full duration reset
     * after each successfully transcribed segment — but only in non-continuous mode.
     * In continuous mode, the countdown is not shown at all. */
    if (app->main_window) {
        bool continuous = app->controller.config
            ? config_get_continuous_dictation(app->controller.config) : false;
        if (!continuous) {
            app_window_stop_countdown(app->main_window);
            app_window_start_countdown(app->main_window);
        }
    }
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/* Volume Level Polling                                                */
/* ------------------------------------------------------------------ */

/**
 * Volume level poll callback.
 * Reads the current RMS volume from the audio recorder and updates the UI.
 * Runs at ~10fps during STATE_LISTENING.
 */
static gboolean volume_poll_callback(gpointer data) {
    TranscriberApp *app = (TranscriberApp *)data;

    if (app->audio_recorder && app->main_window) {
        double level = audio_recorder_get_volume_level(app->audio_recorder);
        /* Only update GTK widget if level changed significantly */
        if (level < 0.0) level = 0.0;
        if (level > 1.0) level = 1.0;
        double last_level = app_window_get_last_volume_level(app->main_window);
        if (level - last_level > VOLUME_DELTA ||
            last_level - level > VOLUME_DELTA ||
            last_level < 0.0) {
            app_window_set_volume_level(app->main_window, level);
            app_window_set_last_volume_level(app->main_window, level);
        }
    }

    return TRUE; /* Continue polling */
}

/**
 * Start the volume level polling timer.
 */
static void start_volume_poll(TranscriberApp *app) {
    if (app->volume_poll_source_id != 0) {
        return; /* Already running */
    }
    app->volume_poll_source_id = g_timeout_add(VOLUME_POLL_INTERVAL_MS,
                                                volume_poll_callback, app);
}

/**
 * Stop the volume level polling timer.
 */
static void stop_volume_poll(TranscriberApp *app) {
    if (app->volume_poll_source_id != 0) {
        g_source_remove(app->volume_poll_source_id);
        app->volume_poll_source_id = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Config Changes                                                      */
/* ------------------------------------------------------------------ */

/**
 * Handle config changes saved from the configuration dialog.
 * Applies runtime updates to the audio recorder (device, etc.) and
 * immediately reacts to transcription text mode changes.
 */
static void on_config_changed(void *user_data) {
    TranscriberApp *app = (TranscriberApp *)user_data;

    /* Apply audio device change */
    if (app->audio_recorder && app->controller.config) {
        const char *device = config_get_audio_device(app->controller.config);
        if (device && device[0] != '\0') {
            audio_recorder_set_device(app->audio_recorder, device);
            g_log("main", G_LOG_LEVEL_INFO,
                  "[audio] Microphone reconfigured to: %s", device);
        } else {
            audio_recorder_set_device(app->audio_recorder, NULL);
            g_log("main", G_LOG_LEVEL_INFO,
                  "[audio] Microphone reconfigured to: default");
        }
    }

    /* React to append_transcription_text setting change immediately.
     * When the user switches from append mode (true) to overwrite mode
     * (false), clear the text window so the next transcription starts fresh.
     * Only do this when not in continuous dictation mode, since continuous
     * mode manages its own text accumulation. */
    if (app->controller.config && app->text_window) {
        bool append_mode = config_get_append_transcription_text(app->controller.config);
        bool continuous = config_get_continuous_dictation(app->controller.config);
        if (!append_mode && !continuous) {
            app_text_window_clear_text(app->text_window);
        }
    }

    /* Apply debug logging setting change immediately */
    if (app->controller.config) {
        reinstall_stderr_handlers(app->controller.config);
    }
}

/**
 * Callback invoked by the silence scanner when a segment is ready for transcription.
 * This runs on the scanner thread, so we marshal to GTK main thread via g_idle_add.
 */
static void on_scanner_segment(int16_t *samples, size_t count, void *user_data)
{
    TranscriberApp *app = (TranscriberApp *)user_data;

    /* Guard against shutdown: if whisper_client is NULL, skip transcription */
    if (!app->whisper_client) {
        g_free(samples);
        return;
    }

    size_t scanner_offset = app->silence_scanner
        ? silence_scanner_get_transcribed_offset(app->silence_scanner) : 0;
    g_log("main", G_LOG_LEVEL_MESSAGE,
          "[DEBUG-scanner-segment] %zu samples (%.1fs), transcribed_offset=%zu — transcribing\n",
          count, (double)count / 16000.0, scanner_offset);

    /* Acquire mutex before calling whisper_transcribe_samples to prevent
     * concurrent access to the whisper.cpp context (not thread-safe).
     * The blocking transcription call is made inside the critical section,
     * but we minimize post-transcription work under the lock by extracting
     * the result data and freeing the response before unlocking. */
    pthread_mutex_lock(&app->scanner_transcribe_mutex);
    WhisperResponse *response = whisper_transcribe_samples(app->whisper_client, samples, (int)count);
    pthread_mutex_unlock(&app->scanner_transcribe_mutex);

    g_free(samples);  /* Free scanner-allocated samples outside mutex */

    bool is_continuous = app->controller.config
        ? config_get_continuous_dictation(app->controller.config) : false;

    if (response && response->success && response->text && response->text[0] != '\0') {
        /* Marshal success to GTK main thread */
        IdleCallbackData *icd = g_new0(IdleCallbackData, 1);
        if (!icd) {
            g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot allocate IdleCallbackData for scanner result\n");
            whisper_response_free(response);
        } else {
            icd->app = app;
            icd->data = g_strdup(response->text);
            if (!icd->data) {
                g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot strdup scanner result text\n");
                g_free(icd);
            } else {
                whisper_response_free(response);
                g_idle_add(on_transcription_result_idle, icd);

                /* Restart watchdog timer so continuous recording doesn't expire */
                g_idle_add(restart_watchdog_idle, app);
            }
        }
    } else if (response && response->success && is_continuous) {
        /* Whisper succeeded but returned empty text — treat as silence in
         * continuous mode. Do not show an error to the user. This happens
         * when whisper processes audio containing only silence or very short
         * speech segments below its detection threshold. */
        g_log("main", G_LOG_LEVEL_DEBUG,
              "[flow] Scanner transcription succeeded but empty text — skipping in continuous mode\n");
        whisper_response_free(response);
    } else {
        const char *error = "Scanner transcription returned empty result";
        if (response) {
            if (response->error_message[0] != '\0') {
                error = response->error_message;
            }
        }
        if (!error || error[0] == '\0') {
            error = whisper_client_get_error(app->whisper_client);
        }
        if (!error || error[0] == '\0') {
            error = "Scanner transcription returned empty result";
        }
        IdleCallbackData *icd = g_new0(IdleCallbackData, 1);
        if (!icd) {
            g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot allocate IdleCallbackData for scanner error\n");
            whisper_response_free(response);
        } else {
            icd->app = app;
            icd->data = g_strdup(error);
            if (!icd->data) {
                g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot strdup scanner error message\n");
                g_free(icd);
            } else {
                whisper_response_free(response);
                g_idle_add(on_transcription_error_idle, icd);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* State Transition Handlers                                           */
/* ------------------------------------------------------------------ */

/**
 * Handle transition to LISTENING state.
 * Starts audio recording and the watchdog timer.
 */
static void handle_enter_listening(TranscriberApp *app, AppState previous_state) {
    g_log("main", G_LOG_LEVEL_DEBUG, "[flow] === handle_enter_listening() entered ===\n");

    /* Clear the TextWindow buffer at the start of a new transcription session
     * if the user has disabled append mode (overwrite mode). This prevents
     * unbounded memory growth over many transcription sessions.
     *
     * For continuous dictation mode, only clear when entering from IDLE
     * (a fresh session started by the user). Do NOT clear during mid-session
     * restarts (TRANSCRIBING→LISTENING) to preserve accumulated text. */
    bool append_mode = app->controller.config ? config_get_append_transcription_text(app->controller.config) : true;
    bool continuous_dictation = app->controller.config ? config_get_continuous_dictation(app->controller.config) : false;
    if (app->text_window && !append_mode) {
        if (!continuous_dictation || previous_state == STATE_IDLE) {
            app_text_window_clear_text(app->text_window);
        }
    }

    /* Update the UI — use GTK type checks to prevent assertions on destroyed widgets.
      * This guards against desync: if a signal handler or assertion fires between
      * updating the main window and tray, both remain in valid states. */
    if (app->main_window && GTK_IS_WIDGET(app_window_get_gtk_window(app->main_window))) {
        app_window_set_state(app->main_window, STATE_LISTENING);
        /* In continuous mode, do not show the countdown timer. */
        if (continuous_dictation) {
            app_window_stop_countdown(app->main_window);
        }
    }

    /* Update tray icon (tray now sets ATTENTION status for pulse in dock) */
    if (app->tray) {
        tray_set_state(app->tray, STATE_LISTENING);
        tray_start_animation(app->tray);
    }

    /* Start audio recording */
    if (app->audio_recorder) {
        int max_duration = app->controller.config->max_duration;
        if (max_duration <= 0) max_duration = DEFAULT_MAX_DURATION_SECONDS;
        g_log("main", G_LOG_LEVEL_DEBUG,
              "[flow] Starting audio recorder (max_duration=%ds)\n", max_duration);

        /* Configure noise suppression before starting capture */
        bool ns_enabled = app->controller.config ? config_get_noise_suppression(app->controller.config) : true;
        audio_recorder_set_noise_suppression(app->audio_recorder, ns_enabled);

        if (audio_recorder_start(app->audio_recorder)) {
            g_log("main", G_LOG_LEVEL_DEBUG,
                  "[flow] Audio recording started successfully\n");
            /* Start the watchdog timer */
            start_watchdog_timer(app);
            /* Start volume level polling */
            start_volume_poll(app);

            /* Create and start silence scanner AFTER ring buffer exists.
             * In continuous mode restarts (TRANSCRIBING→LISTENING), the old
             * scanner must be stopped and destroyed first to prevent:
             *   - Concurrent scanners fighting over the same ring buffer
             *   - Stale offsets causing "Ring buffer full" spam
             *   - Scanner thread exiting while still referenced */
            if (config_get_continuous_dictation(app->controller.config)) {
                /* Stop and destroy old scanner before creating a new one.
                  * This is safe even on first entry where app->silence_scanner==NULL.
                  * IMPORTANT: Before destroying, ensure any in-flight transcription
                  * callback from the old scanner has completed. The scanner's segment
                  * callback holds scanner_transcribe_mutex during whisper.cpp calls.
                  * If we destroy the scanner while that mutex is held, pthread_join()
                  * inside silence_scanner_destroy() will block indefinitely because
                  * the GTK main thread (which runs the idle callbacks) can't release
                  * the lock until this function returns. Acquiring the mutex here
                  * ensures no in-flight callback is active before destruction. */
                if (app->silence_scanner) {
                    g_log("main", G_LOG_LEVEL_DEBUG,
                          "[flow] Destroying old silence scanner before restart\n");

                    /* Wait for any in-flight transcription callback to complete.
                      * This prevents deadlock: the scanner thread may be blocked
                      * inside on_scanner_segment holding this mutex while we try
                      * to join it via silence_scanner_destroy(). */
                    pthread_mutex_lock(&app->scanner_transcribe_mutex);
                    silence_scanner_stop(app->silence_scanner);
                    silence_scanner_destroy(app->silence_scanner);
                    app->silence_scanner = NULL;
                    pthread_mutex_unlock(&app->scanner_transcribe_mutex);
                }

                AudioRingBuffer *rb = audio_recorder_get_ring_buffer(app->audio_recorder);
                if (rb) {
                    app->silence_scanner = silence_scanner_create(
                        rb,
                        (VadMode)config_get_vad_mode(app->controller.config),
                        (int)(config_get_scanner_silence_sec(app->controller.config) * 1000.0f),
                        (int)(config_get_scanner_min_segment_sec(app->controller.config) * 1000.0f)
                    );
                    if (app->silence_scanner) {
                        silence_scanner_set_callback(app->silence_scanner,
                                                     on_scanner_segment, app);
                        if (silence_scanner_start(app->silence_scanner)) {
                            g_log("main", G_LOG_LEVEL_MESSAGE,
                                  "[main] Continuous mode — silence scanner started\n");
                        }
                    }
                }
            }
        } else {
            /* Failed to start recording — return to IDLE */
            const char *error = audio_recorder_get_error();
            if (error && error[0] != '\0') {
                if (app->text_window) {
                    app_text_window_set_error(app->text_window, error);
                }
            }
            app_transition_to(&app->controller, STATE_IDLE);
            if (app->main_window) {
                app_window_set_state(app->main_window, STATE_IDLE);
            }
        }
    } else {
        app_transition_to(&app->controller, STATE_IDLE);
        if (app->main_window) {
            app_window_set_state(app->main_window, STATE_IDLE);
        }
    }
}

/**
 * Handle transition to TRANSCRIBING state.
 * Stops audio recording and initiates transcription.
 */
static void handle_enter_transcribing(TranscriberApp *app, const char *wav_path) {
    g_log("main", G_LOG_LEVEL_DEBUG,
          "[flow] === handle_enter_transcribing() entered wav='%s' ===\n",
          wav_path ? wav_path : "(null)");

    /* Use GTK window bell instead of terminal BEL character,
     * which works reliably in modern desktop environments */
    if (app->main_window) {
        GtkWindow *gtk_win = GTK_WINDOW(app_window_get_gtk_window(app->main_window));
        GdkWindow *gdk_win = gtk_win ? gtk_widget_get_window(GTK_WIDGET(gtk_win)) : NULL;
        gdk_display_beep(gdk_win ? gdk_window_get_display(gdk_win) : gdk_display_get_default());
    }

    /* Stop the watchdog timer */
    stop_watchdog_timer(app);

    /* Stop volume polling */
    stop_volume_poll(app);

    /* Update the UI — GTK type check prevents assertion on destroyed widget */
    if (app->main_window && GTK_IS_WIDGET(app_window_get_gtk_window(app->main_window))) {
        app_window_set_state(app->main_window, STATE_TRANSCRIBING);
    }

    /* Update tray icon (tray now sets ATTENTION status for pulse in dock) */
    if (app->tray) {
        tray_stop_animation(app->tray);
        tray_set_state(app->tray, STATE_TRANSCRIBING);
    }

    /* Store the WAV path for transcription */
    pthread_mutex_lock(&app->wav_path_mutex);
    
    /* Free previous path before allocating new one */
    g_free(app->current_wav_path);
    app->current_wav_path = NULL;

    if (wav_path) {
        app->current_wav_path = g_strdup(wav_path);
    } else if (app->audio_recorder) {
        const char *path = audio_recorder_get_wav_path(app->audio_recorder);
        if (path) {
            app->current_wav_path = g_strdup(path);
        }
    }
    pthread_mutex_unlock(&app->wav_path_mutex);

    /* Start transcription in a background thread.
     * In continuous mode, the WAV path may be empty (deleted by scanner segments),
     * but the ring buffer still has audio. Allow transcription to proceed as long
     * as the whisper client exists. */
    bool continuous = app->controller.config
        ? config_get_continuous_dictation(app->controller.config) : false;
    if (app->whisper_client &&
        ((app->current_wav_path != NULL && app->current_wav_path[0] != '\0') || continuous)) {
        /* Start transcription watchdog (30s constant timeout) — disabled in continuous mode */
        if (!continuous) {
            start_transcription_watchdog(app);
        }

        /* Store thread handle for clean shutdown.
         * Join any previous transcription thread before overwriting,
         * preventing zombie threads from accumulating. */
        GThread *old_thread = NULL;
        pthread_mutex_lock(&app->transcribe_thread_mutex);
        if (app->transcribe_thread) {
            old_thread = app->transcribe_thread;
            app->transcribe_thread = NULL;
        }
        pthread_mutex_unlock(&app->transcribe_thread_mutex);

        /* Cancel the in-flight transcription OUTSIDE the mutex to prevent
         * deadlock: whisper_client_cancel() sets an atomic flag that the
         * transcription thread checks, but we must not hold the thread mutex
         * while waiting for that thread to respond. */
        if (old_thread) {
            whisper_client_cancel(app->whisper_client);
            g_thread_join(old_thread);
        }

        pthread_mutex_lock(&app->transcribe_thread_mutex);
        app->transcribe_thread = g_thread_new("transcribe",
                        (GThreadFunc)transcribe_thread_func,
                        app);
        pthread_mutex_unlock(&app->transcribe_thread_mutex);

        if (!app->transcribe_thread) {
            /* Thread creation failed — transition back to IDLE so the user
              * isn't stuck in TRANSCRIBING state forever. */
            g_log("main", G_LOG_LEVEL_ERROR,
                  "[flow] Failed to create transcription thread\n");
            if (app->text_window) {
                app_text_window_set_error(app->text_window, "Failed to start transcription thread");
            }
            app_transition_to(&app->controller, STATE_IDLE);
            if (app->main_window) {
                app_window_set_state(app->main_window, STATE_IDLE);
            }
        }
    } else {
        /* No whisper client or WAV path — return to IDLE */
        if (app->text_window) {
            app_text_window_set_error(app->text_window, "No audio file available for transcription");
        }
        app_transition_to(&app->controller, STATE_IDLE);
        if (app->main_window) {
            app_window_set_state(app->main_window, STATE_IDLE);
        }
    }
}

/**
 * Background thread function for transcription.
 */
static gpointer transcribe_thread_func(gpointer data) {
    TranscriberApp *app = (TranscriberApp *)data;

    /* Snapshot config values into local variables to prevent torn reads on
      * non-x86 architectures. The AppConfig struct may be modified by the
      * main thread (via config dialog save) while this worker thread is running. */
    char model_path_buf[512] = {0};
    char language_buf[16] = {0};
    bool is_continuous_snapshot = false;

    if (app->controller.config) {
        const char *mp = config_get_model_path(app->controller.config);
        if (mp) strncpy(model_path_buf, mp, sizeof(model_path_buf) - 1);
        const char *lang = config_get_language(app->controller.config);
        if (lang) strncpy(language_buf, lang, sizeof(language_buf) - 1);
        is_continuous_snapshot = config_get_continuous_dictation(app->controller.config);
    }

    whisper_client_set_model_path(app->whisper_client, model_path_buf);
    whisper_client_set_language(app->whisper_client, language_buf);

    /* DEBUG: Log entry state */
    g_log("main", G_LOG_LEVEL_MESSAGE,
          "[DEBUG-transcribe-thread] ENTRY continuous=%s user_requested_stop=%d\n",
          is_continuous_snapshot ? "true" : "false",
          atomic_load(&app->user_requested_stop));

    /* Stop the silence scanner first to prevent it from reading the ring buffer
     * while we extract samples. The scanner thread and this transcription thread
     * both access the ring buffer, so we must stop the scanner before extracting. */
    if (app->silence_scanner) {
        size_t offset_before_stop = silence_scanner_get_transcribed_offset(app->silence_scanner);
        silence_scanner_stop(app->silence_scanner);
        g_log("main", G_LOG_LEVEL_MESSAGE,
              "[DEBUG-transcribe-thread] Scanner stopped — transcribed_offset=%zu\n",
              offset_before_stop);
    }

    /* Stop audio recording first (capture thread must stop before ring buffer extract) */
    if (app->audio_recorder) {
        audio_recorder_stop(app->audio_recorder);
    }

    /* Extract PCM samples from ring buffer for in-memory transcription */
    int16_t *samples = NULL;
    size_t n_samples = 0;
    if (app->audio_recorder) {
        n_samples = audio_recorder_extract_samples(app->audio_recorder, &samples);

        g_log("main", G_LOG_LEVEL_MESSAGE,
              "[DEBUG-transcribe-thread] Extracted %zu samples (%.2fs) from ring buffer\n",
              n_samples, (double)n_samples / 16000.0);

        /* In continuous mode, skip already-transcribed samples.
         * The silence scanner tracks how many samples have been sent
         * for transcription. We only want the remaining untranscribed
         * portion from the ring buffer. */
        if (samples && n_samples > 0 && app->silence_scanner) {
            size_t already_transcribed = silence_scanner_get_transcribed_offset(app->silence_scanner);
            g_log("main", G_LOG_LEVEL_MESSAGE,
                  "[DEBUG-transcribe-thread] Scanner transcribed_offset=%zu, ring_buffer_total=%zu\n",
                  already_transcribed, n_samples);
            if (already_transcribed >= n_samples) {
                /* All samples were already transcribed by the scanner */
                g_log("main", G_LOG_LEVEL_MESSAGE,
                      "[DEBUG-transcribe-thread] SKIP — all %zu samples already transcribed (offset=%zu)\n",
                      n_samples, already_transcribed);
                g_free(samples);
                samples = NULL;
                n_samples = 0;
            } else if (already_transcribed > 0) {
                size_t remaining_after_skip = n_samples - already_transcribed;
                g_log("main", G_LOG_LEVEL_MESSAGE,
                      "[DEBUG-transcribe-thread] Skipping %zu already-transcribed samples, keeping %zu (%.2fs)\n",
                      already_transcribed, remaining_after_skip,
                      (double)remaining_after_skip / 16000.0);
                /* Skip the already-transcribed prefix */
                memmove(samples, samples + already_transcribed,
                        (n_samples - already_transcribed) * sizeof(int16_t));
                n_samples -= already_transcribed;
            } else {
                g_log("main", G_LOG_LEVEL_MESSAGE,
                      "[DEBUG-transcribe-thread] No offset to skip — using all %zu samples\n",
                      n_samples);
            }
        }

        /* Trim trailing silence to prevent whisper from hallucinating text
         * on trailing quiet periods. */
        if (samples && n_samples > 0) {
            size_t before_trim = n_samples;
            n_samples = audio_trim_trailing_silence(samples, n_samples, 16000);
            g_log("main", G_LOG_LEVEL_MESSAGE,
                  "[DEBUG-transcribe-thread] After trim: %zu -> %zu samples (%.2fs removed)\n",
                  before_trim, n_samples,
                  (double)(before_trim - n_samples) / 16000.0);
        }

        /* Skip transcription if remaining audio is too short.
         * Whisper hallucinates text (e.g., "Thank you") on very short
         * noise-only clips. Require at least 500ms of audio after trimming
         * to avoid spurious transcriptions. */
        if (samples && n_samples > 0) {
            int remaining_ms = (int)((double)n_samples / 16000.0 * 1000.0);
            g_log("main", G_LOG_LEVEL_MESSAGE,
                  "[DEBUG-transcribe-thread] Duration check: %d ms (threshold=500ms)\n",
                  remaining_ms);
            if (remaining_ms < 500) {
                g_log("main", G_LOG_LEVEL_MESSAGE,
                      "[DEBUG-transcribe-thread] SKIP — too short (%d ms < 500ms)\n",
                      remaining_ms);
                g_free(samples);
                samples = NULL;
                n_samples = 0;
            }
        }

        /* Verify remaining audio actually contains voice using VAD.
         * In continuous mode, the user may have stopped speaking before
         * clicking the mic, leaving only background noise in the buffer.
         * Whisper hallucinates text on noise-only audio, so we skip
         * transcription if VAD finds insufficient voice content. */
        if (samples && n_samples > 0 && app->silence_scanner) {
            VadDetector *vad = vad_detector_create(VAD_MODE_MODERATE);
            if (vad) {
                size_t frame_samples = 320;  /* 20ms at 16kHz */
                size_t n_frames = n_samples / frame_samples;
                int voice_frames = 0;
                for (size_t f = 0; f < n_frames; f++) {
                    bool frame_voice = vad_process_frame(vad,
                                                         samples + f * frame_samples,
                                                         frame_samples,
                                                         16000);
                    if (frame_voice) voice_frames++;
                }
                /* Require at least 30% of frames to be voice */
                bool has_voice = (voice_frames > (int)(n_frames * 0.3));
                vad_detector_destroy(vad);
                g_log("main", G_LOG_LEVEL_MESSAGE,
                      "[DEBUG-transcribe-thread] VAD: %d/%d frames voice (%.0f%%, threshold=30%%) has_voice=%s\n",
                      voice_frames, (int)n_frames,
                      n_frames > 0 ? ((double)voice_frames / n_frames * 100.0) : 0.0,
                      has_voice ? "true" : "false");
                if (!has_voice) {
                    g_log("main", G_LOG_LEVEL_MESSAGE,
                          "[DEBUG-transcribe-thread] SKIP — VAD rejected (%d/%d frames)\n",
                          voice_frames, (int)n_frames);
                    g_free(samples);
                    samples = NULL;
                    n_samples = 0;
                }
            }
        } else if (samples && n_samples > 0) {
            g_log("main", G_LOG_LEVEL_MESSAGE,
                  "[DEBUG-transcribe-thread] VAD check skipped (no scanner), proceeding with %zu samples\n",
                  n_samples);
        }
    }

    g_log("main", G_LOG_LEVEL_MESSAGE,
          "[DEBUG-transcribe-thread] Final: samples=%p n_samples=%zu (%.2fs)\n",
          (void*)samples, n_samples,
          n_samples > 0 ? (double)n_samples / 16000.0 : 0.0);

    WhisperResponse *response = NULL;

    if (samples && n_samples > 0) {
        /* In-memory transcription from ring buffer */
        response = whisper_transcribe_samples(app->whisper_client, samples, (int)n_samples);
        g_free(samples);
    } else {
        /* Fallback: try WAV file path if ring buffer is empty */
        pthread_mutex_lock(&app->wav_path_mutex);
        char wav_path[PATH_MAX];
        g_strlcpy(wav_path, app->current_wav_path, sizeof(wav_path));
        pthread_mutex_unlock(&app->wav_path_mutex);

        if (wav_path[0] != '\0') {
            response = whisper_transcribe_with_retry(app->whisper_client, wav_path, WHISPER_MAX_RETRIES);
        }
    }

    /* If both ring buffer and WAV path are empty, check if this is continuous
     * mode. In that case, silently skip transcription and let the app
     * transition to IDLE without showing an error. */
    /* Use snapshot instead of re-reading config (torn-read prevention) */
    if (!response) {
        if (is_continuous_snapshot) {
            /* In continuous mode, audio may have been entirely silence or noise.
             * Create a success response with empty text so the app transitions to
             * IDLE without showing an error. Using an empty string rather than NULL
             * makes the semantic meaning clear: this is a valid result with no
             * content, distinct from a failure or NULL pointer. */
            response = (WhisperResponse *)calloc(1, sizeof(WhisperResponse));
            if (response) {
                response->success = true;
                response->text = g_strdup("");  /* Empty string — silence/noise only */
            }
        } else {
            /* Non-continuous mode: show error */
            response = (WhisperResponse *)calloc(1, sizeof(WhisperResponse));
            if (response) {
                snprintf(response->error_message, sizeof(response->error_message),
                         "No audio data available for transcription");
                response->error_code = 10;
                response->success = false;
            }
        }
    }

    /* Marshal result to the GTK main thread */
    if (app->controller.on_transcription_result) {
        if (response && response->success && response->text && response->text[0] != '\0') {
            IdleCallbackData *icd = g_new0(IdleCallbackData, 1);
            if (!icd) {
                g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot allocate IdleCallbackData for transcription result\n");
            } else {
                icd->app = app;
                icd->data = g_strdup(response->text);
                if (!icd->data) {
                    g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot strdup transcription text\n");
                    g_free(icd);
                } else {
                    g_idle_add(on_transcription_result_idle, icd);
                }
            }
        } else if (response && response->success && is_continuous_snapshot) {
            /* Whisper succeeded but returned empty text — treat as silence in
             * continuous mode. Do not show an error to the user. This happens
             * when whisper processes audio containing only silence or very short
             * speech segments below its detection threshold.
             * IMPORTANT: Still marshal to GTK main thread so on_transcription_result()
             * can check user_requested_stop and transition state appropriately. */
            g_log("main", G_LOG_LEVEL_DEBUG,
                  "[flow] Transcription succeeded but empty text — skipping in continuous mode\n");
            IdleCallbackData *icd = g_new0(IdleCallbackData, 1);
            if (!icd) {
                g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot allocate IdleCallbackData for empty result\n");
            } else {
                icd->app = app;
                icd->data = g_strdup("");  /* Empty string — not NULL for safe idle callback */
                if (!icd->data) {
                    g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot strdup empty result\n");
                    g_free(icd);
                } else {
                    g_idle_add(on_transcription_result_idle, icd);
                }
            }
        } else {
            const char *error = whisper_client_get_error(app->whisper_client);
            if (!error || error[0] == '\0') {
                error = response && response->error_message[0] != '\0'
                        ? response->error_message
                        : "Transcription returned empty result";
            }
            IdleCallbackData *icd = g_new0(IdleCallbackData, 1);
            if (!icd) {
                g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot allocate IdleCallbackData for transcription error\n");
            } else {
                icd->app = app;
                icd->data = g_strdup(error);
                if (!icd->data) {
                    g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot strdup transcription error message\n");
                    g_free(icd);
                } else {
                    g_idle_add(on_transcription_error_idle, icd);
                }
            }
        }
        if (response) {
            whisper_response_free(response);
        }
    } else {
        /* Free response when callback is NULL to prevent memory leak */
        if (response) {
            whisper_response_free(response);
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Callback Implementations                                            */
/* ------------------------------------------------------------------ */

/**
 * Handle transcription result (success or error).
 * Called from the GTK main thread via g_idle_add().
 * Either text or error may be non-NULL, but not both.
 */
static void on_transcription_result(TranscriberApp *app, const char *text_or_error, bool success) {
    /* Stop transcription watchdog on result */
    stop_transcription_watchdog(app);

    /* Check continuous mode early so clipboard logic can use it below. */
    bool continuous = false;
    if (app->controller.config) {
        continuous = config_get_continuous_dictation(app->controller.config);
    }

    if (success && text_or_error) {
        /* Log the raw transcription output for debugging */
        g_log("main", G_LOG_LEVEL_DEBUG,
              "[flow] Transcription SUCCESS — appending '%.80s...'\n", text_or_error);

        /* Append or replace transcribed text in the TextWindow based on setting. */
        if (app->text_window) {
            bool append_mode = app->controller.config
                ? config_get_append_transcription_text(app->controller.config) : true;
            if (!append_mode) {
                app_text_window_clear_text(app->text_window);
            }
            app_text_window_append_text(app->text_window, text_or_error);
        }

        /* In continuous mode, accumulate all segments into a single buffer so
          * the clipboard always holds the full transcript. In normal mode, just
          * copy the current segment as before. */
        if (continuous) {
            pthread_mutex_lock(&app->continuous_clipboard_mutex);

            /* Cap accumulated clipboard text to prevent unbounded memory growth.
              * 500 KiB is more than enough for hours of continuous dictation.
              * If the buffer exceeds this limit, truncate from the front (oldest)
              * so the most recent transcription remains intact. */
            #define CONTINUOUS_CLIPBOARD_MAX_BYTES (512 * 1024)

            if (app->continuous_clipboard_text) {
                size_t current_len = strlen(app->continuous_clipboard_text);
                size_t incoming_len = strlen(text_or_error);
                if (current_len + incoming_len + 2 > CONTINUOUS_CLIPBOARD_MAX_BYTES) {
                    /* Truncate oldest text to make room for new segment.
                      * Keep at least the last quarter of existing text plus the new segment. */
                    size_t keep_from = current_len / 4;
                    memmove(app->continuous_clipboard_text,
                            app->continuous_clipboard_text + keep_from,
                            (current_len - keep_from) + 1);
                }
            }

            char *accumulated = g_strdup_printf("%s%s%s",
                app->continuous_clipboard_text ? app->continuous_clipboard_text : "",
                app->continuous_clipboard_text ? " " : "",
                text_or_error);
            if (!accumulated) {
                g_log("main", G_LOG_LEVEL_ERROR,
                      "[oom] Cannot allocate clipboard buffer — skipping copy\n");
                pthread_mutex_unlock(&app->continuous_clipboard_mutex);
            } else {
                g_free(app->continuous_clipboard_text);
                app->continuous_clipboard_text = accumulated;

                if (clipboard_is_available(NULL)) {
                    clipboard_copy_text_both(NULL, app->continuous_clipboard_text);
                }
                pthread_mutex_unlock(&app->continuous_clipboard_mutex);
            }
        } else {
            /* Normal mode: reset accumulator and copy just this segment. */
            pthread_mutex_lock(&app->continuous_clipboard_mutex);
            g_free(app->continuous_clipboard_text);
            app->continuous_clipboard_text = NULL;
            pthread_mutex_unlock(&app->continuous_clipboard_mutex);

            if (clipboard_is_available(NULL)) {
                clipboard_copy_text_both(NULL, text_or_error);
            }
        }

        /* Delete the temporary WAV file after transcription */
        if (app->audio_recorder) {
            audio_recorder_delete_wav(app->audio_recorder);
        }
    } else if (!success && text_or_error) {
        /* 'text_or_error' parameter holds the error message on failure */
        if (app->text_window) {
            app_text_window_set_error(app->text_window, text_or_error);
        }
    }

    /* Check if user explicitly requested stop (click during LISTENING in continuous mode).
     * Capture and clear the flag atomically so the next transcription cycle is unaffected. */
    bool user_stopped = atomic_exchange(&app->user_requested_stop, 0) != 0;

    AppState current_state = app_get_state(&app->controller);

    size_t final_offset = app->silence_scanner
        ? silence_scanner_get_transcribed_offset(app->silence_scanner) : 0;
    g_log("main", G_LOG_LEVEL_DEBUG,
          "[flow] transcription_result: success=%d continuous=%s user_stopped=%d "
          "current_state=%d transcribed_offset=%zu text_len=%zu\n",
          success, continuous ? "true" : "false",
          user_stopped, current_state, final_offset,
          (success && text_or_error) ? strlen(text_or_error) : 0);

    if (continuous && success && !user_stopped) {
        /* Continuous mode: restart recording after transcription completes.
         *
         * Two paths lead here:
         *   1. Scanner-initiated: recorder still running, scanner was stopped
         *      by transcribe_thread_func(). Transitioning to LISTENING triggers
         *      handle_enter_listening() which destroys the old scanner and
         *      creates a fresh one with reset offsets.
         *   2. Watchdog-initiated: recorder stopped, scanner dead.
         *      handle_enter_listening() restarts everything from scratch.
         *
         * In both cases, transitioning to LISTENING is the correct action to
         * restore the continuous recording + scanning loop. */
        g_log("main", G_LOG_LEVEL_DEBUG,
              "[flow] Continuous mode — restarting LISTENING after transcription\n");

        /* Transition back to LISTENING — this triggers handle_enter_listening()
         * which restarts audio recording and recreates the silence scanner. */
        app_transition_to(&app->controller, STATE_LISTENING);
    } else if (continuous && !success) {
        g_log("main", G_LOG_LEVEL_DEBUG,
              "[flow] Continuous mode but transcription FAILED — transitioning to IDLE\n");
    } else {
        /* Normal mode: transition back to IDLE */
        app_transition_to(&app->controller, STATE_IDLE);
        if (app->main_window) {
            app_window_set_state(app->main_window, STATE_IDLE);
        }

        /* Update tray icon (tray now sets ACTIVE status — no pulse when idle) */
        if (app->tray) {
            tray_stop_animation(app->tray);
            tray_set_state(app->tray, STATE_IDLE);
        }
    }
}

/**
 * Handle connection status change.
 * Called from the GTK main thread.
 */
static void on_model_status_change(TranscriberApp *app, ModelStatus status) {
    if (app->main_window) {
        app_window_set_model_status(app->main_window, status);
    }

    if (app->tray) {
        tray_set_model_status(app->tray, status);
    }
}

/**
 * Background thread: loads the whisper model with GPU auto-detection.
 * On success, transitions to LISTENING. On failure, shows error dialog.
 */
static gpointer model_loading_thread_func(gpointer data) {
    TranscriberApp *app = (TranscriberApp *)data;

    /* Snapshot config values into local variables to prevent torn reads on
      * non-x86 architectures. The AppConfig struct may be modified by the
      * main thread while this worker thread is running. */
    char model_path_buf[512] = {0};
    char language_buf[16] = {0};
    char gpu_mode_buf[32] = {0};
    bool flash_attention_snapshot = false;

    if (app->controller.config) {
        const char *mp = config_get_model_path(app->controller.config);
        if (mp) strncpy(model_path_buf, mp, sizeof(model_path_buf) - 1);
        const char *lang = config_get_language(app->controller.config);
        if (lang) strncpy(language_buf, lang, sizeof(language_buf) - 1);
        const char *gm = config_get_gpu_mode(app->controller.config);
        if (gm) strncpy(gpu_mode_buf, gm, sizeof(gpu_mode_buf) - 1);
        flash_attention_snapshot = config_get_flash_attention(app->controller.config);
    }

    whisper_client_set_model_path(app->whisper_client, model_path_buf);
    whisper_client_set_language(app->whisper_client, language_buf);

    /* Load the model with GPU mode from config.
      * gpu_mode can be "auto", "cpu", or "gpu:N".
      * This blocks until the model is loaded (or fails). */
    bool loaded = whisper_client_load_model(app->whisper_client, gpu_mode_buf, flash_attention_snapshot);

    /* Marshal result back to GTK main thread */
    if (loaded) {
        g_idle_add(on_model_loaded_idle, app);
    } else {
        const char *error = whisper_client_get_error(app->whisper_client);
        ModelLoadFailedData *mlfd = g_new0(ModelLoadFailedData, 1);
        if (!mlfd) {
            g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot allocate ModelLoadFailedData\n");
        } else {
            mlfd->app = app;
            mlfd->error_msg = error ? g_strdup(error) : g_strdup("Failed to load model");
            if (!mlfd->error_msg) {
                g_log("main", G_LOG_LEVEL_ERROR, "[oom] Cannot strdup model load error message\n");
                g_free(mlfd);
            } else {
                g_idle_add(on_model_load_failed_idle, mlfd);
            }
        }
    }

    return NULL;
}

/**
 * GTK idle callback: model loaded successfully — clear "WAIT" indicator.
 * If the loading was triggered by user click (lazy loading), also
 * auto-transition to LISTENING to start recording.
 */
static gboolean on_model_loaded_idle(gpointer data) {
    TranscriberApp *app = (TranscriberApp *)data;

    /* Bail out early if application is shutting down */
    if (app->shutting_down) {
        return FALSE;
    }

    /* Capture and clear the flag atomically before any potential re-entry */
    bool from_toggle = atomic_exchange(&app->model_loading_from_toggle, 0) != 0;

    /* Clear the "WAIT" overlay from the icon */
    if (app->main_window) {
        app_window_set_model_loading(app->main_window, FALSE);
    }

    /* If GPU fallback occurred, show a non-intrusive info dialog so the user knows
     * that the configured GPU was not used. This is informational only — the model
     * loaded successfully on an alternate device or CPU. */
    const char *gpu_fallback = whisper_client_get_gpu_fallback_message(app->whisper_client);
    if (gpu_fallback && gpu_fallback[0] != '\0') {
        show_auto_close_dialog(app, "GPU Fallback", GTK_MESSAGE_INFO,
            "%s\n\nThe application is fully functional.", gpu_fallback);
    }

    /* Update connection status to connected */
    app_set_model_status(&app->controller, MODEL_AVAILABLE);
    if (app->main_window) {
        app_window_set_model_status(app->main_window, MODEL_AVAILABLE);
    }
    if (app->tray) {
        tray_set_model_status(app->tray, MODEL_AVAILABLE);
    }

    /* If loading was triggered by user click, auto-transition to LISTENING */
    if (from_toggle) {
        app_toggle_state(&app->controller);
    }

    return FALSE;
}

/**
 * GTK idle callback: model loading failed — show error, clear "WAIT".
 * Falls back to disconnected state so lazy loading on first click
 * can still attempt to load the model.
 */
static gboolean on_model_load_failed_idle(gpointer data) {
    ModelLoadFailedData *mlfd = (ModelLoadFailedData *)data;
    TranscriberApp *app = mlfd->app;
    char *error_msg = mlfd->error_msg;

    /* Bail out early if application is shutting down */
    if (app->shutting_down) {
        g_free(error_msg);
        g_free(mlfd);
        return FALSE;
    }

    /* Clear the "WAIT" overlay from the icon */
    if (app->main_window) {
        app_window_set_model_loading(app->main_window, FALSE);
    }

    /* Reset connection status to disconnected */
    app_set_model_status(&app->controller, MODEL_UNAVAILABLE);
    if (app->main_window) {
        app_window_set_model_status(app->main_window, MODEL_UNAVAILABLE);
    }
    if (app->tray) {
        tray_set_model_status(app->tray, MODEL_UNAVAILABLE);
    }

    /* Show non-modal, auto-closing warning dialog */
    const char *gpu_fallback = whisper_client_get_gpu_fallback_message(app->whisper_client);
    if (gpu_fallback && gpu_fallback[0] != '\0') {
        show_auto_close_dialog(app, "Model Load Warning", GTK_MESSAGE_WARNING,
            "Failed to load Whisper model at startup.\n\n%s\n\n%s\n\n"
            "The application will attempt to load the model when you "
            "first click the microphone icon.",
            gpu_fallback, error_msg ? error_msg : "Unknown error");
    } else {
        show_auto_close_dialog(app, "Model Load Warning", GTK_MESSAGE_WARNING,
            "Failed to load Whisper model at startup.\n\n%s\n\n"
            "The application will attempt to load the model when you "
            "first click the microphone icon.", error_msg ? error_msg : "Unknown error");
    }

    g_free(error_msg);
    g_free(mlfd);
    return FALSE;
}

/**
 * Handle microphone toggle request.
 * Shared by both the D-Bus toggle and the icon click handler.
 *
 * Before starting recording, check if the local Whisper model
 * is available. If unavailable, show an error dialog and abort.
 *
 * MODEL-LOADING: On first mic click, the model is loaded lazily in a
 * background thread. The UI shows a LOADING indicator (amber circle)
 * while the model is being loaded. Once loaded, recording starts.
 * This prevents the application from crashing at startup or config
 * dialog open due to GPU initialization issues.
 *
 * STARTUP-LOADING: If the model is still loading from startup
 * (indicated by model_loading==TRUE on the MainWindow), reject the
 * click and let the user know to wait.
 */
static void on_microphone_toggle(void *user_data) {
    TranscriberApp *app = (TranscriberApp *)user_data;

    /* Only check local model when starting recording (IDLE -> LISTENING) */
    AppState current = app_get_state(&app->controller);
    const char *state_name[] = {"IDLE", "LISTENING", "TRANSCRIBING"};
    g_log("main", G_LOG_LEVEL_DEBUG,
          "[flow] on_microphone_toggle() entered, current=%s\n", state_name[current]);
    if (current == STATE_IDLE) {
        /* STARTUP-LOADING: If model is still loading from startup,
         * reject the click. The "WAIT" overlay on the icon should
         * have made this clear to the user. */
        if (app->main_window && app_window_get_model_loading(app->main_window)) {
            return;  /* Model still loading — ignore click */
        }
        /* CFG-015: Validate that the configured audio device is available */
        {
            const char *configured_device = config_get_audio_device(app->controller.config);
            bool device_valid = false;

            /* "default" is always valid — ALSA will use the system default */
            if (configured_device && g_strcmp0(configured_device, "default") == 0) {
                device_valid = true;
            } else if (configured_device && configured_device[0] != '\0') {
                /* Check if the configured device exists in the available device list */
                AudioDeviceList *devices = audio_recorder_get_device_list(app->audio_recorder, false);
                if (devices) {
                    for (gint i = 0; i < devices->count; i++) {
                        if (g_strcmp0(devices->device_names[i], configured_device) == 0) {
                            device_valid = true;
                            break;
                        }
                    }
                    audio_device_list_free(devices);
                }
            }

            if (!device_valid) {
                show_auto_close_dialog(app, "Microphone Not Available", GTK_MESSAGE_ERROR,
                    "The configured microphone is not available.\n\n"
                    "Please select a valid microphone in Transcriber Settings.");
                return; /* Abort — do not start recording */
            }
        }

        /* Check that a valid GGUF model file is configured and accessible */
        const char *model_path = config_get_model_path(app->controller.config);
        if (!model_path || model_path[0] == '\0' ||
            !config_dialog_validate_model(model_path)) {
            show_auto_close_dialog(app, "Model Not Found", GTK_MESSAGE_ERROR,
                APP_ERROR_NO_VALID_MODEL ".\n\n"
                "Please configure a valid Whisper model file in Settings.");
            return; /* Abort — do not start recording */
        }

        /* First check cached connection status */
        ModelStatus conn = app_get_model_status(&app->controller);
        if (conn == MODEL_UNAVAILABLE) {
            /* Do a live check to confirm if model is available */
            bool model_available = true;
            if (app->whisper_client) {
                model_available = whisper_check_connection(app->whisper_client);
            } else {
                model_available = false;
            }
            if (!model_available) {
                show_auto_close_dialog(app, "Model Unavailable", GTK_MESSAGE_ERROR,
                    "Local Whisper model is unavailable.\n\n"
                    "Please check that the model file exists and is "
                    "correctly configured, then try again.");
                return; /* Abort — do not start recording */
            }
        }

        /* MODEL-LOADING: If model file exists but is not yet loaded,
         * start loading in a background thread with LOADING indicator.
         * Set flag so on_model_loaded_idle auto-transitions to LISTENING. */
        if (app->whisper_client && !whisper_client_is_model_loaded(app->whisper_client) &&
            !whisper_client_is_loading(app->whisper_client)) {

            /* Mark that this load was triggered by user click,
             * so on_model_loaded_idle will auto-transition to LISTENING */
            atomic_store(&app->model_loading_from_toggle, 1);

            /* Set model path and language */
            const char *mp = config_get_model_path(app->controller.config);
            whisper_client_set_model_path(app->whisper_client, mp);
            const char *lang = config_get_language(app->controller.config);
            whisper_client_set_language(app->whisper_client, lang);

            /* Show LOADING indicator and "WAIT" overlay */
            app_set_model_status(&app->controller, MODEL_LOADING);
            if (app->main_window) {
                app_window_set_model_status(app->main_window, MODEL_LOADING);
                app_window_set_model_loading(app->main_window, TRUE);
            }
            if (app->tray) {
                tray_set_model_status(app->tray, MODEL_LOADING);
            }

            /* Store model loading thread handle for clean shutdown.
             * Join any previous model load thread before overwriting. */
            pthread_mutex_lock(&app->model_load_thread_mutex);
            if (app->model_load_thread) {
                g_thread_join(app->model_load_thread);
                app->model_load_thread = NULL;
            }
            app->model_load_thread = g_thread_new("model_loading",
                                   model_loading_thread_func, app);
            pthread_mutex_unlock(&app->model_load_thread_mutex);

            if (!app->model_load_thread) {
                /* Thread creation failed — clear WAIT overlay and show error */
                g_log("main", G_LOG_LEVEL_ERROR,
                      "[toggle] Failed to create model load thread\n");
                app_set_model_status(&app->controller, MODEL_UNAVAILABLE);
                if (app->main_window) {
                    app_window_set_model_loading(app->main_window, FALSE);
                    app_window_set_model_status(app->main_window, MODEL_UNAVAILABLE);
                }
                show_auto_close_dialog(app, "Model Load Error", GTK_MESSAGE_ERROR,
                    "Failed to start model loading thread.\n\n"
                    "Please check system resources and try again.");
                return;
            }

            return; /* Wait for loading to complete before recording */
        }

        /* If model is already loading (another click), just return */
        if (app->whisper_client && whisper_client_is_loading(app->whisper_client)) {
            return;
        }
    }

    /* When user clicks during LISTENING or TRANSCRIBING in continuous mode, set the stop flag
     * so that on_transcription_result knows this was an explicit user stop
     * and should transition to IDLE instead of restarting recording. */
    if (app->controller.config) {
        bool continuous = config_get_continuous_dictation(app->controller.config);
        if (continuous && (current == STATE_LISTENING || current == STATE_TRANSCRIBING)) {
            atomic_store(&app->user_requested_stop, 1);
        }
    }

    /* Toggle the state — on_state_change callback will handle the actual work.
      * If we reach here, either:
      * 1. Model was already loaded (normal fast path)
      * 2. User is stopping recording (LISTENING -> TRANSCRIBING)
      * 3. User is in TRANSCRIBING state (no-op) */
    app_toggle_state(&app->controller);
}

/**
 * Handle D-Bus toggle request.
 * Called when the system hotkey triggers the D-Bus ToggleMicrophone method.
 */
static void on_dbus_toggle(void *user_data) {
    on_microphone_toggle(user_data);
}

/**
 * Handle GNOME Shell Activate request from D-Bus.
 * Called when the user clicks the application icon in the GNOME Dash/Dock.
 * Presents or raises the main window to bring it to the user's attention.
 */
static void on_dbus_activate(void *user_data) {
    TranscriberApp *app = (TranscriberApp *)user_data;
    if (app && app->main_window) {
        GtkWindow *win = app_window_get_gtk_window(app->main_window);
        if (win) {
            gtk_window_present(win);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Startup Model Loading                                               */
/* ------------------------------------------------------------------ */

/**
 * Load the Whisper model at startup in a background thread.
 * The MainWindow displays a "WAIT" overlay on the mic icon until
 * loading completes. On success, "WAIT" is removed and the
 * connection indicator turns green. On failure, "WAIT" is removed,
 * the indicator turns red, and a non-blocking warning is shown.
 * The user can still attempt lazy loading on first click.
 */
static void perform_initial_model_load(TranscriberApp *app) {
    if (!app->whisper_client) return;

    /* Validate model path before attempting load */
    const char *model_path = config_get_model_path(app->controller.config);
    if (!model_path || model_path[0] == '\0') {
        /* No model configured — clear WAIT, show disconnected */
        if (app->main_window) {
            app_window_set_model_loading(app->main_window, FALSE);
        }
        app_set_model_status(&app->controller, MODEL_UNAVAILABLE);
        if (app->main_window) {
            app_window_set_model_status(app->main_window, MODEL_UNAVAILABLE);
        }
        return;
    }

    /* Set model path and language */
    whisper_client_set_model_path(app->whisper_client, model_path);
    const char *lang = config_get_language(app->controller.config);
    whisper_client_set_language(app->whisper_client, lang);

    /* Show LOADING indicator on the status bar */
    app_set_model_status(&app->controller, MODEL_LOADING);
    if (app->main_window) {
        app_window_set_model_status(app->main_window, MODEL_LOADING);
    }
    if (app->tray) {
        tray_set_model_status(app->tray, MODEL_LOADING);
    }

    /* The "WAIT" overlay is already active (set to TRUE in app_window_create).
     * Start background thread to load the model. */
    /* Store model loading thread handle for clean shutdown.
     * Join any previous model load thread before overwriting. */
    pthread_mutex_lock(&app->model_load_thread_mutex);
    if (app->model_load_thread) {
        g_thread_join(app->model_load_thread);
        app->model_load_thread = NULL;
    }
    app->model_load_thread = g_thread_new("startup_model_load",
                                         model_loading_thread_func, app);
    pthread_mutex_unlock(&app->model_load_thread_mutex);

    /* If thread creation failed, clear the WAIT overlay immediately so the UI
      * doesn't get stuck showing a loading indicator forever. */
    if (!app->model_load_thread) {
        g_log("main", G_LOG_LEVEL_ERROR,
              "[startup] Failed to create model load thread — clearing WAIT overlay\n");
        if (app->main_window) {
            app_window_set_model_loading(app->main_window, FALSE);
        }
        app_set_model_status(&app->controller, MODEL_UNAVAILABLE);
        if (app->main_window) {
            app_window_set_model_status(app->main_window, MODEL_UNAVAILABLE);
        }
        if (app->tray) {
            tray_set_model_status(app->tray, MODEL_UNAVAILABLE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* State Change Callback (direct invocation on transition)             */
/* ------------------------------------------------------------------ */

/**
 * Direct state change callback invoked by the state controller
 * on each successful transition. Replaces the polling-based
 * state_monitor_callback pattern for zero-latency response.
 *
 * This callback is invoked from the GTK main thread, as all
 * callers of app_transition_to/app_toggle_state run on that thread.
 */
/**
 * Callback invoked when the user selects "Clear Transcription" from the tray context menu.
 * Only available in IDLE state. Clears the TextWindow buffer and empties the clipboard.
 */
static void on_tray_clear(TranscriberApp *app) {
    /* Clear the transcription text window */
    if (app->text_window) {
        app_text_window_clear_text(app->text_window);
    }

    /* Clear the system clipboard */
    clipboard_clear(NULL);

    /* Reset continuous clipboard accumulator */
    pthread_mutex_lock(&app->continuous_clipboard_mutex);
    g_free(app->continuous_clipboard_text);
    app->continuous_clipboard_text = NULL;
    pthread_mutex_unlock(&app->continuous_clipboard_mutex);
}

static void on_state_change(TranscriberApp *app, AppState previous_state, AppState new_state) {
    const char *state_name[] = {"IDLE", "LISTENING", "TRANSCRIBING"};

    g_log("main", G_LOG_LEVEL_DEBUG,
          "[flow] STATE CHANGE: %s -> %s\n",
          state_name[previous_state],
          state_name[new_state]);

    switch (new_state) {
        case STATE_LISTENING:
            g_log("main", G_LOG_LEVEL_DEBUG,
                  "[flow] Entering LISTENING — starting recording + scanner\n");
            handle_enter_listening(app, previous_state);
            break;
        case STATE_TRANSCRIBING:
            /* Stop the silence scanner BEFORE stopping the recorder.
             * The scanner thread may be blocked inside whisper transcription
             * (on_scanner_segment holds app->scanner_transcribe_mutex). If we
             * don't stop it first, the scanner continues running while we try
             * to shut down recording, causing resource contention and hangs.
             * This is especially critical in continuous mode where the scanner
             * fires transcription segments during active recording. */
            if (app->silence_scanner) {
                silence_scanner_stop(app->silence_scanner);
            }

            /* Stop audio recording — pthread_join will wait for capture thread
             * to finish writing remaining data and close ALSA device. */
            if (app->audio_recorder) {
                audio_recorder_stop(app->audio_recorder);
            }
            {
                const char *path = NULL;
                if (app->audio_recorder) {
                    path = audio_recorder_get_wav_path(app->audio_recorder);
                }
                bool continuous = app->controller.config
                    ? config_get_continuous_dictation(app->controller.config) : false;
                g_log("main", G_LOG_LEVEL_DEBUG,
                      "[flow] TRANSCRIBING: wav_path='%s' continuous=%s\n",
                      path ? path : "(null)", continuous ? "true" : "false");
                /* In continuous mode, always transcribe even if WAV path is empty,
                 * since the ring buffer may contain remaining audio from the last
                 * segment that wasn't picked up by the silence scanner. The
                 * transcribe_thread_func() uses ring buffer as primary source
                 * and WAV as fallback. */
                if ((path && path[0] != '\0') || continuous) {
                    handle_enter_transcribing(app, path);
                } else {
                    g_log("main", G_LOG_LEVEL_DEBUG,
                          "[flow] No WAV path and not continuous — transitioning to IDLE\n");
                    app_transition_to(&app->controller, STATE_IDLE);
                    if (app->main_window) {
                        app_window_set_state(app->main_window, STATE_IDLE);
                    }
                }
            }
            break;
        case STATE_IDLE:
            g_log("main", G_LOG_LEVEL_DEBUG,
                  "[flow] Entering IDLE\n");
            if (app->main_window && GTK_IS_WIDGET(app_window_get_gtk_window(app->main_window))) {
                app_window_set_state(app->main_window, STATE_IDLE);
            }
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Callback wrappers for AppStateController (void *user_data signature) */
/* ------------------------------------------------------------------ */

/**
 * Wrapper to adapt on_transcription_result to the controller callback signature.
 * The controller passes user_data which is our TranscriberApp pointer.
 */
static void on_transcription_result_wrapper(const char *text, bool success, void *user_data) {
    TranscriberApp *app = (TranscriberApp *)user_data;
    on_transcription_result(app, text, success);
}

/**
 * Wrapper to adapt on_model_status_change to the controller callback signature.
 */
static void on_model_status_change_wrapper(ModelStatus status, void *user_data) {
    TranscriberApp *app = (TranscriberApp *)user_data;
    on_model_status_change(app, status);
}

/**
 * Wrapper to adapt on_state_change to the controller callback signature.
 */
static void on_state_change_wrapper(AppState previous_state, AppState new_state, void *user_data) {
    TranscriberApp *app = (TranscriberApp *)user_data;
    on_state_change(app, previous_state, new_state);
}

/* ------------------------------------------------------------------ */
/* Application Lifecycle                                               */
/* ------------------------------------------------------------------ */

/**
 * Create and initialize the TranscriberApp.
 * Returns NULL on failure.
 */
static TranscriberApp *app_create(void) {
    TranscriberApp *app = g_new0(TranscriberApp, 1);
    if (!app) return NULL;

    /* Initialize config with defaults */
    config_set_defaults(&app->config);
    config_load(&app->config);

    /* Reinstall stderr handlers now that debug_logs setting is loaded from config.
     * The initial register_all_log_handlers(NULL) used the default (no debug). */
    reinstall_stderr_handlers(&app->config);

    /* Initialize controller */
    if (app_state_controller_init(&app->controller,
                                  &app->config,
                                  on_transcription_result_wrapper,
                                  on_model_status_change_wrapper,
                                  on_state_change_wrapper,
                                  app) != 0) {
        g_free(app);
        return NULL;
    }

    /* Create audio recorder */
    AudioFormat fmt = audio_format_get_default();
    const char *device = config_get_audio_device(&app->config);
    app->audio_recorder = audio_recorder_create(&fmt);
    if (!app->audio_recorder) {
        app_state_controller_cleanup(&app->controller);
        g_free(app);
        return NULL;
    }
    if (device && device[0] != '\0') {
        audio_recorder_set_device(app->audio_recorder, device);
        g_log("main", G_LOG_LEVEL_INFO,
              "[audio] Using configured microphone: %s", device);
    } else {
        g_log("main", G_LOG_LEVEL_INFO,
              "[audio] Using default microphone");
    }

    /* Enumerate available microphones at startup for logging and validation.
     * If the configured device is not present in the enumerated list, emit a
     * WARNING to the log file so stale configurations are traceable. */
    {
        AudioDeviceList *startup_devices = audio_recorder_get_device_list(app->audio_recorder, true);
        if (startup_devices) {
            const char *configured = config_get_audio_device(&app->config);
            if (configured && configured[0] != '\0' &&
                g_strcmp0(configured, "default") != 0) {
                bool found = false;
                for (gint i = 0; i < startup_devices->count; i++) {
                    if (g_strcmp0(startup_devices->device_names[i], configured) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    g_log("main", G_LOG_LEVEL_WARNING,
                          "[audio] Configured microphone '%s' not found in available devices — "
                          "recording may fail until a valid device is selected in Settings",
                          configured);
                }
            }
            audio_device_list_free(startup_devices);
        }
    }

    /* Create Whisper client */
    app->whisper_client = whisper_client_create();
    if (!app->whisper_client) {
        audio_recorder_destroy(app->audio_recorder);
        app_state_controller_cleanup(&app->controller);
        g_free(app);
        return NULL;
    }

    /* Silence scanner will be created lazily when entering continuous mode
     * (ring buffer only exists after audio_recorder_start()). */
    app->silence_scanner = NULL;

    /* Create MainWindow */
    app->main_window = app_window_create(&app->config, &app->controller, app->whisper_client);
    if (!app->main_window) {
        whisper_client_destroy(app->whisper_client);
        audio_recorder_destroy(app->audio_recorder);
        app_state_controller_cleanup(&app->controller);
        g_free(app);
        return NULL;
    }

    /* Register callbacks on MainWindow */
    app_window_set_toggle_callback(app->main_window, on_microphone_toggle, app);
    app_window_set_clear_callback(app->main_window, (void (*)(void *))on_tray_clear, app);
    app_window_set_config_changed_callback(app->main_window, on_config_changed, app);

    /* Create TextWindow */
    GtkWindow *gtk_win = app_window_get_gtk_window(app->main_window);
    app->text_window = app_text_window_create(gtk_win);

    /* Create system tray icon */
    app->tray = tray_create();
    if (app->tray) {
        tray_set_main_window(app->tray, gtk_win);
        tray_set_toggle_callback(app->tray, on_microphone_toggle, app);
        tray_set_clear_callback(app->tray, (void (*)(void *))on_tray_clear, app);
    }

    /* Start D-Bus service */
    app->dbus_service = dbus_service_create();
    if (app->dbus_service) {
        if (!dbus_service_start(app->dbus_service, on_dbus_toggle, on_dbus_activate, app)) {
            /* Continue without D-Bus — single-instance not enforced */
            dbus_service_destroy(app->dbus_service);
            app->dbus_service = NULL;
        }
    }

    /* Initialize mutexes */
    if (pthread_mutex_init(&app->wav_path_mutex, NULL) != 0 ||
        pthread_mutex_init(&app->transcribe_thread_mutex, NULL) != 0 ||
        pthread_mutex_init(&app->model_load_thread_mutex, NULL) != 0 ||
        pthread_mutex_init(&app->scanner_transcribe_mutex, NULL) != 0 ||
        pthread_mutex_init(&app->continuous_clipboard_mutex, NULL) != 0) {
        app_destroy(app);
        return NULL;
    }

    /* Initialize atomic flags */
    atomic_store(&app->model_loading_from_toggle, 0);

    /* Initialize shutdown flag */
    app->shutting_down = false;

    return app;
}

/**
 * Destroy the TranscriberApp and free all resources.
 */
static void app_destroy(TranscriberApp *app) {
    if (!app) return;

    /* Set shutdown flag atomically to prevent idle callbacks from accessing freed resources.
      * Using atomic_store for portable cross-thread visibility. */
    atomic_store(&app->shutting_down, true);

    /* Clear clipboard content so transcription text does not persist after exit.
     * Must be called while GTK display is still valid (before windows are destroyed). */
    clipboard_clear(NULL);

    /* Stop timers */
    stop_watchdog_timer(app);
    stop_volume_poll(app);

    /* Destroy system tray icon */
    if (app->tray) {
        tray_destroy(app->tray);
        app->tray = NULL;
    }

    /* Stop D-Bus service */
    if (app->dbus_service) {
        dbus_service_stop(app->dbus_service);
        dbus_service_destroy(app->dbus_service);
        app->dbus_service = NULL;
    }

    /* Destroy TextWindow */
    if (app->text_window) {
        app_text_window_destroy(app->text_window);
        app->text_window = NULL;
    }

    /* Save window position */
    if (app->main_window) {
        app_window_save_position(app->main_window);
    }

    /* Destroy MainWindow */
    if (app->main_window) {
        app_window_destroy(app->main_window);
        app->main_window = NULL;
    }

    /* Gracefully cancel and join any in-flight transcription thread */
    if (app->whisper_client) {
        whisper_client_cancel(app->whisper_client);
    }

    /* Join the transcription thread */
    pthread_mutex_lock(&app->transcribe_thread_mutex);
    if (app->transcribe_thread) {
        g_thread_join(app->transcribe_thread);
        app->transcribe_thread = NULL;
    }
    pthread_mutex_unlock(&app->transcribe_thread_mutex);

    /* Join the model loading thread if it's still running */
    pthread_mutex_lock(&app->model_load_thread_mutex);
    if (app->model_load_thread) {
        g_thread_join(app->model_load_thread);
        app->model_load_thread = NULL;
    }
    pthread_mutex_unlock(&app->model_load_thread_mutex);

    /* Stop and destroy silence scanner BEFORE destroying the whisper client.
     * The scanner thread's on_scanner_segment() callback accesses
     * app->whisper_client. If the whisper client is destroyed first, the
     * scanner callback could access freed memory (use-after-free).
     * silence_scanner_destroy() calls pthread_join() internally, ensuring
     * the scanner thread has fully exited before the whisper client is freed. */
    if (app->silence_scanner) {
        silence_scanner_destroy(app->silence_scanner);
        app->silence_scanner = NULL;
    }

    /* Destroy Whisper client (after scanner is stopped to prevent use-after-free) */
    if (app->whisper_client) {
        whisper_client_destroy(app->whisper_client);
        app->whisper_client = NULL;
    }

    /* Destroy audio recorder */
    if (app->audio_recorder) {
        audio_recorder_destroy(app->audio_recorder);
        app->audio_recorder = NULL;
    }

    /* Cleanup state controller */
    app_state_controller_cleanup(&app->controller);

    /* Free the current WAV path */
    g_free(app->current_wav_path);

    /* Free accumulated continuous-mode clipboard text */
    g_free(app->continuous_clipboard_text);

    /* Destroy mutexes */
    pthread_mutex_destroy(&app->wav_path_mutex);
    pthread_mutex_destroy(&app->transcribe_thread_mutex);
    pthread_mutex_destroy(&app->model_load_thread_mutex);
    pthread_mutex_destroy(&app->scanner_transcribe_mutex);
    pthread_mutex_destroy(&app->continuous_clipboard_mutex);

    /* Drain any pending GTK idle callbacks that may reference 'app' before
     * freeing it. Worker threads (scanner, transcription) queue idle callbacks
     * via g_idle_add() that capture the 'app' pointer. Even after the worker
     * threads are joined, those callbacks may still be pending in the GTK main
     * loop. Processing them here ensures they execute while 'app' is still valid,
     * and their internal shutting_down checks will cause them to bail out safely.
     * Cap iterations at 500 to prevent infinite loops if a callback keeps
     * re-queuing itself — after the limit, remaining callbacks are abandoned. */
    {
        int drain_iterations = 0;
        const int max_drain_iterations = 500;
        while (g_main_context_pending(NULL) && drain_iterations < max_drain_iterations) {
            g_main_context_iteration(NULL, FALSE);
            drain_iterations++;
        }
        if (drain_iterations >= max_drain_iterations) {
            g_log("main", G_LOG_LEVEL_WARNING,
                  "[shutdown] GTK drain loop hit %d iteration limit — abandoning remaining callbacks\n",
                  max_drain_iterations);
        }
    }

    g_free(app);
}

/* ------------------------------------------------------------------ */
/* Main Entry Point                                                    */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    /* Handle --version flag before any subsystem initialization or debug
     * logging is enabled, so the output is clean (no GLib-GIO-DEBUG noise). */
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--version") == 0) {
            g_print("transcriber %s\n", APP_VERSION);
            return 0;
        }
    }

    /* Enable ALL GLib log levels (DEBUG, INFO, MESSAGE) for all domains.
     * By default, g_log() only emits ERROR/CRITICAL/WARNING. Setting this
     * environment variable before any logging occurs ensures our file handler
     * receives every message level including DEBUG and MESSAGE. */
    setenv("G_MESSAGES_DEBUG", "all", 1);

    /* Truncate/create log file BEFORE installing any handlers so that no stale
     * data from a previous run exists when the first log message is written. */
    {
        FILE *_truncate_log = fopen("/tmp/transcriber.log", "w");
        if (_truncate_log) fclose(_truncate_log);
    }

    /* Install file-based log handlers BEFORE any subsystem initialization.
     * Registers transcriber_file_log_handler for every domain used by the app
     * so all g_log() messages are captured to /tmp/transcriber.log.
     * Config is NULL here — stderr handler defaults to INFO+ only (no debug). */
    register_all_log_handlers(NULL);

    /* Verify the log handler is active by writing a startup marker.
     * This also ensures the static FILE* inside the handler has been opened,
     * so we can safely reopen stderr into the same file below. */
    g_log("main", G_LOG_LEVEL_MESSAGE, "[startup] Log handlers installed — /tmp/transcriber.log active\n");

    /* Initialize GTK early so GIO/GDBus domains are registered before we set up
     * catch-all log handlers and redirect stderr. If we register handlers or
     * reopen stderr after gtk_init(), some GLib internal messages may have
     * already been emitted to the original stderr and lost. */
    gtk_init(&argc, &argv);

    /* Re-register all log handlers AFTER GTK init. This is critical because
     * gtk_init() registers new GLib domains (GLib-GIO, GDBus, GVfs, etc.) that
     * were not present before. Without this re-registration, messages from those
     * domains fall through to the default stderr formatter and bypass our log file. */
    register_all_log_handlers(NULL);

    /* Set the default GLib log handler as a catch-all for any domain we haven't
     * explicitly registered (e.g., GModule, GIO modules). This ensures every
     * g_log() message lands in the log file regardless of its domain name. */
    g_log_set_default_handler(transcriber_file_log_handler, NULL);

    /* Redirect stderr to append into /tmp/transcriber.log so that messages from
     * libraries not using GLib logging (e.g., whisper.cpp via fprintf(stderr))
     * are also captured in the same log file. The g_log handler above opened the
     * file with "w" mode and wrote the startup marker, so reopening stderr with
     * "a" mode appends after that marker without truncating. */
    if (freopen("/tmp/transcriber.log", "a", stderr)) {
        setlinebuf(stderr);  /* Line-buffered for immediate flushing */
    }

    /* Set the default application icon name for icon theme resolution. */
    gtk_window_set_default_icon_name("redmic");

    /* Enable whisper.cpp logging to stderr so it appears in /tmp/transcriber.log.
     * This helps debug model loading failures and transcription errors. */
    // whisper_log_set(NULL, NULL);  // Previously suppressed — now enabled for debugging

    /* Create and initialize the application */
    TranscriberApp *app = app_create();
    if (!app) {
        return 1;
    }

    /* Show the main window (with "WAIT" overlay on the mic icon) */
    GtkWindow *gtk_win = app_window_get_gtk_window(app->main_window);
    gtk_widget_show_all(GTK_WIDGET(gtk_win));

    /* Load the Whisper model at startup in a background thread.
     * The window shows "WAIT" overlay on the red mic icon until
     * the model is fully loaded. Once loaded, "WAIT" is removed
     * and the connection indicator turns green. This eliminates
     * the lag when the user first clicks the mic to start recording. */
    perform_initial_model_load(app);

    /* Start the GTK main loop */
    gtk_main();

    /* Flush log file before destroying the application to ensure final messages are written. */
    close_log_file();

    /* Destroy the application and free all resources */
    app_destroy(app);

    return 0;
}
