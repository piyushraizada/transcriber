/*
 * SPDX-License-Identifier: MIT
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
 *     bell is used to trigger the system beep (LO-05 fix).
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

#define _POSIX_C_SOURCE 200809L

#include "app.h"
#include "app_audio.h"
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

    /* Configuration (owned by the app, lifetime = app lifetime) */
    AppConfig config;

    /* Timer source IDs */
    guint watchdog_source_id;
    guint transcription_watchdog_source_id;
    guint volume_poll_source_id;

    /* WAV path for transcription (protected by wav_path_mutex) */
    char current_wav_path[PATH_MAX];
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
static void on_state_change(TranscriberApp *app, AppState new_state);
static void show_auto_close_dialog(TranscriberApp *app, const char *title,
                                   GtkMessageType type, const char *format, ...);
static void handle_enter_listening(TranscriberApp *app);
static void handle_enter_transcribing(TranscriberApp *app, const char *wav_path);
static void start_watchdog_timer(TranscriberApp *app);
static void stop_watchdog_timer(TranscriberApp *app);
static void start_transcription_watchdog(TranscriberApp *app);
static void stop_transcription_watchdog(TranscriberApp *app);
static void start_volume_poll(TranscriberApp *app);
static void stop_volume_poll(TranscriberApp *app);
static void perform_initial_model_load(TranscriberApp *app);
static int get_transcription_timeout_seconds(TranscriberApp *app);

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

    /* H-002 fix: Bail out early if application is shutting down */
    if (app->shutting_down) {
        g_free(icd->data);
        g_free(icd);
        return FALSE;
    }
    on_transcription_result(app, icd->data, true);
    g_free(icd->data);
    g_free(icd);
    return FALSE;
}

static gboolean on_transcription_error_idle(gpointer data) {
    IdleCallbackData *icd = (IdleCallbackData *)data;
    TranscriberApp *app = icd->app;

    /* H-002 fix: Bail out early if application is shutting down */
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
    if (state == STATE_LISTENING) {
        /* Transition to TRANSCRIBING */
        if (app_transition_to(&app->controller, STATE_TRANSCRIBING)) {
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

/* LOW-16 fix: Transcription phase watchdog timeout is now configurable via
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
 * Applies runtime updates to the audio recorder (device, etc.).
 */
static void on_config_changed(void *user_data) {
    TranscriberApp *app = (TranscriberApp *)user_data;
    if (app->audio_recorder && app->controller.config) {
        const char *device = config_get_audio_device(app->controller.config);
        if (device && device[0] != '\0') {
            audio_recorder_set_device(app->audio_recorder, device);
        } else {
            audio_recorder_set_device(app->audio_recorder, NULL);
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
static void handle_enter_listening(TranscriberApp *app) {
    /* Clear the TextWindow buffer at the start of a new transcription session
     * if the user has disabled append mode (overwrite mode). This prevents
     * unbounded memory growth over many transcription sessions. */
    if (app->text_window && !config_get_append_transcription_text(app->controller.config)) {
        app_text_window_clear_text(app->text_window);
    }

    /* Update the UI */
    if (app->main_window) {
        app_window_set_state(app->main_window, STATE_LISTENING);
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
        if (audio_recorder_start(app->audio_recorder)) {
            /* Start the watchdog timer */
            start_watchdog_timer(app);
            /* Start volume level polling */
            start_volume_poll(app);
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
    /* LO-05 fix: Use GTK window bell instead of terminal BEL character,
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

    /* Update the UI */
    if (app->main_window) {
        app_window_set_state(app->main_window, STATE_TRANSCRIBING);
    }

    /* Update tray icon (tray now sets ATTENTION status for pulse in dock) */
    if (app->tray) {
        tray_stop_animation(app->tray);
        tray_set_state(app->tray, STATE_TRANSCRIBING);
    }

    /* Store the WAV path for transcription */
    pthread_mutex_lock(&app->wav_path_mutex);
    if (wav_path) {
        size_t len = strlen(wav_path);
        if (len >= sizeof(app->current_wav_path)) {
            g_log("main", G_LOG_LEVEL_WARNING,
                  "[main] WAV path truncated: %.100s... (len=%zu, max=%zu)\n",
                  wav_path, len, sizeof(app->current_wav_path) - 1);
        }
        g_strlcpy(app->current_wav_path, wav_path, sizeof(app->current_wav_path));
    } else if (app->audio_recorder) {
        const char *path = audio_recorder_get_wav_path(app->audio_recorder);
        if (path) {
            size_t len = strlen(path);
            if (len >= sizeof(app->current_wav_path)) {
                g_log("main", G_LOG_LEVEL_WARNING,
                      "[main] WAV path truncated: %.100s... (len=%zu, max=%zu)\n",
                      path, len, sizeof(app->current_wav_path) - 1);
            }
            g_strlcpy(app->current_wav_path, path, sizeof(app->current_wav_path));
        }
    }
    pthread_mutex_unlock(&app->wav_path_mutex);

    /* Start transcription in a background thread */
    if (app->whisper_client && app->current_wav_path[0] != '\0') {
        /* MIN-012 fix: Start transcription watchdog (30s constant timeout) */
        start_transcription_watchdog(app);

        /* HI-04 fix: Store thread handle for clean shutdown.
         * Memory leak fix: Join any previous transcription thread before
         * overwriting the handle, preventing zombie threads from accumulating. */
        pthread_mutex_lock(&app->transcribe_thread_mutex);
        if (app->transcribe_thread) {
            /* Cancel the in-flight transcription to free its resources */
            whisper_client_cancel(app->whisper_client);
            g_thread_join(app->transcribe_thread);
            app->transcribe_thread = NULL;
        }
        app->transcribe_thread = g_thread_new("transcribe",
                        (GThreadFunc)transcribe_thread_func,
                        app);
        pthread_mutex_unlock(&app->transcribe_thread_mutex);
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

    const char *model_path = config_get_model_path(app->controller.config);
    whisper_client_set_model_path(app->whisper_client, model_path);

    /* Copy WAV path under mutex */
    pthread_mutex_lock(&app->wav_path_mutex);
    char wav_path[PATH_MAX];
    g_strlcpy(wav_path, app->current_wav_path, sizeof(wav_path));
    pthread_mutex_unlock(&app->wav_path_mutex);

    /* Perform transcription with retry */
    WhisperResponse *response = whisper_transcribe_with_retry(app->whisper_client,
                                                 wav_path,
                                                 WHISPER_MAX_RETRIES);

    /* Marshal result to the GTK main thread */
    if (app->controller.on_transcription_result) {
        if (response && response->success && response->text && response->text[0] != '\0') {
            IdleCallbackData *icd = g_new0(IdleCallbackData, 1);
            icd->app = app;
            icd->data = g_strdup(response->text);
            g_idle_add(on_transcription_result_idle, icd);
        } else {
            const char *error = whisper_client_get_error(app->whisper_client);
            if (!error || error[0] == '\0') {
                error = response && response->error_message[0] != '\0'
                        ? response->error_message
                        : "Transcription returned empty result";
            }
            IdleCallbackData *icd = g_new0(IdleCallbackData, 1);
            icd->app = app;
            icd->data = g_strdup(error);
            g_idle_add(on_transcription_error_idle, icd);
        }
        if (response) {
            whisper_response_free(response);
        }
    } else {
        /* ME-05 fix: Free response when callback is NULL to prevent memory leak */
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
static void on_transcription_result(TranscriberApp *app, const char *text, bool success) {
    /* MIN-012 fix: Stop transcription watchdog on result */
    stop_transcription_watchdog(app);

    if (success && text) {
        /* Append transcribed text to the TextWindow */
        if (app->text_window) {
            app_text_window_append_text(app->text_window, text);
        }

        /* Copy to clipboard (both PRIMARY and CLIPBOARD).
         * This function is invoked from the GTK main thread via
         * g_idle_add() in transcribe_thread_func(), so GTK clipboard
         * operations are safe here without additional marshaling. */
        if (clipboard_is_available(NULL)) {
            clipboard_copy_text_both(NULL, text);
        }

        /* Delete the temporary WAV file after transcription */
        if (app->audio_recorder) {
            audio_recorder_delete_wav(app->audio_recorder);
        }
    } else if (!success && text) {
        /* 'text' parameter holds the error message on failure */
        if (app->text_window) {
            app_text_window_set_error(app->text_window, text);
        }
    }

    /* Transition back to IDLE */
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

    /* Set model path from config */
    const char *model_path = config_get_model_path(app->controller.config);
    whisper_client_set_model_path(app->whisper_client, model_path);

    /* Load the model with GPU mode from config.
     * gpu_mode can be "auto", "cpu", or "gpu:N".
     * This blocks until the model is loaded (or fails). */
    const char *gpu_mode = config_get_gpu_mode(app->controller.config);
    bool loaded = whisper_client_load_model(app->whisper_client, gpu_mode);

    /* Marshal result back to GTK main thread */
    if (loaded) {
        g_idle_add(on_model_loaded_idle, app);
    } else {
        const char *error = whisper_client_get_error(app->whisper_client);
        ModelLoadFailedData *mlfd = g_new0(ModelLoadFailedData, 1);
        mlfd->app = app;
        mlfd->error_msg = error ? g_strdup(error) : g_strdup("Failed to load model");
        g_idle_add(on_model_load_failed_idle, mlfd);
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

    /* H-002 fix: Bail out early if application is shutting down */
    if (app->shutting_down) {
        return FALSE;
    }

    /* Capture and clear the flag atomically before any potential re-entry */
    bool from_toggle = atomic_exchange(&app->model_loading_from_toggle, 0) != 0;

    /* Clear the "WAIT" overlay from the icon */
    if (app->main_window) {
        app_window_set_model_loading(app->main_window, FALSE);
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

    /* H-002 fix: Bail out early if application is shutting down */
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
    show_auto_close_dialog(app, "Model Load Warning", GTK_MESSAGE_WARNING,
        "Failed to load Whisper model at startup.\n\n%s\n\n"
        "The application will attempt to load the model when you "
        "first click the microphone icon.", error_msg ? error_msg : "Unknown error");

    g_free(error_msg);
    g_free(mlfd);
    return FALSE;
}

/**
 * Handle microphone toggle request.
 * Shared by both the D-Bus toggle and the icon click handler.
 *
 * ERR-014: Before starting recording, check if the local Whisper model
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

    /* HI-03 fix: Only check local model when starting recording (IDLE -> LISTENING) */
    AppState current = app_get_state(&app->controller);
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
                AudioDeviceList *devices = audio_recorder_get_device_list(app->audio_recorder);
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

            /* Set model path */
            const char *mp = config_get_model_path(app->controller.config);
            whisper_client_set_model_path(app->whisper_client, mp);

            /* Show LOADING indicator and "WAIT" overlay */
            app_set_model_status(&app->controller, MODEL_LOADING);
            if (app->main_window) {
                app_window_set_model_status(app->main_window, MODEL_LOADING);
                app_window_set_model_loading(app->main_window, TRUE);
            }
            if (app->tray) {
                tray_set_model_status(app->tray, MODEL_LOADING);
            }

            /* H-001 fix: Store model loading thread handle for clean shutdown.
             * Memory leak fix: Join any previous model load thread before overwriting. */
            pthread_mutex_lock(&app->model_load_thread_mutex);
            if (app->model_load_thread) {
                g_thread_join(app->model_load_thread);
                app->model_load_thread = NULL;
            }
            app->model_load_thread = g_thread_new("model_loading",
                                  model_loading_thread_func, app);
            pthread_mutex_unlock(&app->model_load_thread_mutex);

            return; /* Wait for loading to complete before recording */
        }

        /* If model is already loading (another click), just return */
        if (app->whisper_client && whisper_client_is_loading(app->whisper_client)) {
            return;
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

    /* Set model path */
    whisper_client_set_model_path(app->whisper_client, model_path);

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
    /* H-001 fix: Store model loading thread handle for clean shutdown.
     * Memory leak fix: Join any previous model load thread before overwriting. */
    pthread_mutex_lock(&app->model_load_thread_mutex);
    if (app->model_load_thread) {
        g_thread_join(app->model_load_thread);
        app->model_load_thread = NULL;
    }
    app->model_load_thread = g_thread_new("startup_model_load",
                                        model_loading_thread_func, app);
    pthread_mutex_unlock(&app->model_load_thread_mutex);
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
static void on_state_change(TranscriberApp *app, AppState new_state) {
    switch (new_state) {
        case STATE_LISTENING:
            handle_enter_listening(app);
            break;
        case STATE_TRANSCRIBING:
            if (app->audio_recorder) {
                audio_recorder_stop(app->audio_recorder);
            }
            {
                const char *path = NULL;
                if (app->audio_recorder) {
                    path = audio_recorder_get_wav_path(app->audio_recorder);
                }
                if (path && path[0] != '\0') {
                    handle_enter_transcribing(app, path);
                } else {
                    app_transition_to(&app->controller, STATE_IDLE);
                    if (app->main_window) {
                        app_window_set_state(app->main_window, STATE_IDLE);
                    }
                }
            }
            break;
        case STATE_IDLE:
            if (app->main_window) {
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
static void on_state_change_wrapper(AppState new_state, void *user_data) {
    TranscriberApp *app = (TranscriberApp *)user_data;
    on_state_change(app, new_state);
}

/* ------------------------------------------------------------------ */
/* Application Lifecycle                                               */
/* ------------------------------------------------------------------ */

/**
 * Create and initialize the TranscriberApp.
 * Returns NULL on failure.
 */
static TranscriberApp *app_create(void) {
    TranscriberApp *app = calloc(1, sizeof(TranscriberApp));
    if (!app) return NULL;

    /* Initialize config with defaults */
    config_set_defaults(&app->config);
    config_load(&app->config);

    /* Initialize controller */
    if (app_state_controller_init(&app->controller,
                                  &app->config,
                                  on_transcription_result_wrapper,
                                  on_model_status_change_wrapper,
                                  on_state_change_wrapper,
                                  app) != 0) {
        free(app);
        return NULL;
    }

    /* Create audio recorder */
    AudioFormat fmt = audio_format_get_default();
    const char *device = config_get_audio_device(&app->config);
    app->audio_recorder = audio_recorder_create(&fmt);
    if (!app->audio_recorder) {
        app_state_controller_cleanup(&app->controller);
        free(app);
        return NULL;
    }
    if (device && device[0] != '\0') {
        audio_recorder_set_device(app->audio_recorder, device);
    }

    /* Create Whisper client */
    app->whisper_client = whisper_client_create();
    if (!app->whisper_client) {
        audio_recorder_destroy(app->audio_recorder);
        app_state_controller_cleanup(&app->controller);
        free(app);
        return NULL;
    }

    /* Create MainWindow */
    app->main_window = app_window_create(&app->config, &app->controller, app->whisper_client);
    if (!app->main_window) {
        whisper_client_destroy(app->whisper_client);
        audio_recorder_destroy(app->audio_recorder);
        app_state_controller_cleanup(&app->controller);
        free(app);
        return NULL;
    }

    /* Register callbacks on MainWindow */
    app_window_set_toggle_callback(app->main_window, on_microphone_toggle, app);
    app_window_set_config_changed_callback(app->main_window, on_config_changed, app);

    /* Create TextWindow */
    GtkWindow *gtk_win = app_window_get_gtk_window(app->main_window);
    app->text_window = app_text_window_create(gtk_win);

    /* Create system tray icon */
    app->tray = tray_create();
    if (app->tray) {
        tray_set_main_window(app->tray, gtk_win);
        tray_set_toggle_callback(app->tray, on_microphone_toggle, app);
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
    pthread_mutex_init(&app->wav_path_mutex, NULL);
    pthread_mutex_init(&app->transcribe_thread_mutex, NULL);
    pthread_mutex_init(&app->model_load_thread_mutex, NULL);

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

    /* H-002 fix: Set shutdown flag to prevent idle callbacks from accessing freed resources */
    app->shutting_down = true;

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

    /* HI-04 fix: Gracefully cancel and join any in-flight transcription thread */
    if (app->whisper_client) {
        whisper_client_cancel(app->whisper_client);
    }

    /* CRIT-2 fix: Join the transcription thread */
    pthread_mutex_lock(&app->transcribe_thread_mutex);
    if (app->transcribe_thread) {
        g_thread_join(app->transcribe_thread);
        app->transcribe_thread = NULL;
    }
    pthread_mutex_unlock(&app->transcribe_thread_mutex);

    /* H-001 fix: Join the model loading thread if it's still running */
    pthread_mutex_lock(&app->model_load_thread_mutex);
    if (app->model_load_thread) {
        g_thread_join(app->model_load_thread);
        app->model_load_thread = NULL;
    }
    pthread_mutex_unlock(&app->model_load_thread_mutex);

    /* Destroy Whisper client */
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

    /* Destroy mutexes */
    pthread_mutex_destroy(&app->wav_path_mutex);
    pthread_mutex_destroy(&app->transcribe_thread_mutex);
    pthread_mutex_destroy(&app->model_load_thread_mutex);

    free(app);
}

/* ------------------------------------------------------------------ */
/* Main Entry Point                                                    */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    /* Initialize GTK */
    gtk_init(&argc, &argv);

    /* Set the default application icon name for icon theme resolution. */
    gtk_window_set_default_icon_name("redmic");

    /* Suppress whisper.cpp library internal log output */
    whisper_log_set(NULL, NULL);

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

    /* Destroy the application and free all resources */
    app_destroy(app);

    return 0;
}
