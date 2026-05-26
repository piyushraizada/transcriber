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
 */

#define _POSIX_C_SOURCE 199309L

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
/* Forward declarations for internal callbacks */
static void on_transcription_result(const char *text, bool success, void *user_data);
static void on_transcription_result_error(const char *error, bool success, void *user_data);
static void on_connection_status_change(ConnectionStatus status, void *user_data);
static void on_recording_stop(const char *wav_path, void *user_data);
static void on_dbus_toggle(void *user_data);
static void on_config_changed(void *user_data);
static gboolean watchdog_timer_callback(gpointer user_data);
static gpointer transcribe_thread_func(gpointer data);
static gboolean volume_poll_callback(gpointer data);
static gpointer model_loading_thread_func(gpointer data);
static gboolean on_model_loaded_idle(gpointer data);
static gboolean on_model_load_failed_idle(gpointer data);

/* H-002 fix: Shutdown flag — declared early so idle callbacks can reference it */
static volatile bool g_shutting_down = false;

/* Wrappers for g_idle_add (GSourceFunc signature: gboolean (*)(gpointer)) */
static gboolean on_transcription_result_idle(gpointer data) {
    /* H-002 fix: Bail out early if application is shutting down */
    if (g_shutting_down) {
        g_free(data);
        return FALSE;
    }
    on_transcription_result((const char *)data, true, NULL);
    g_free(data);
    return FALSE;
}

static gboolean on_transcription_result_error_idle(gpointer data) {
    /* H-002 fix: Bail out early if application is shutting down */
    if (g_shutting_down) {
        g_free(data);
        return FALSE;
    }
    on_transcription_result_error((const char *)data, false, NULL);
    g_free(data);
    return FALSE;
}

/* Wrapper for g_timeout_add: GSourceFunc signature requires gboolean return */
static gboolean auto_close_dialog(gpointer data) {
    gtk_widget_destroy(GTK_WIDGET(data));
    return FALSE;
}


/* ------------------------------------------------------------------ */
/* Global application state (accessible from callbacks)                */
/* ------------------------------------------------------------------ */

static AppStateController g_controller;
static MainWindow *g_main_window = NULL;
static TextWindow *g_text_window = NULL;
static DBusService *g_dbus_service = NULL;
static AudioRecorder *g_audio_recorder = NULL;
static WhisperClient *g_whisper_client = NULL;
static guint g_watchdog_source_id = 0;
static guint g_transcription_watchdog_source_id = 0;
static guint g_volume_poll_source_id = 0;
static SystemTray *g_tray = NULL;
static char g_current_wav_path[PATH_MAX];
/* TRUE when model loading was triggered by user click (lazy loading).
 * Causes on_model_loaded_idle to auto-transition to LISTENING on success.
 * FALSE when model loading was triggered at startup (just clear WAIT). */
/* CRIT-1 fix: Use atomic_bool for defense-in-depth against potential
 * cross-thread access if future refactoring moves idle callbacks. */
static atomic_int g_model_loading_from_toggle = 0;
static pthread_mutex_t g_wav_path_mutex = PTHREAD_MUTEX_INITIALIZER;

/* HI-04 fix: Store transcription thread handle for clean shutdown */
static GThread *g_transcribe_thread = NULL;
static pthread_mutex_t g_transcribe_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

/* H-001 fix: Store model loading thread handle for clean shutdown */
static GThread *g_model_load_thread = NULL;
static pthread_mutex_t g_model_load_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Volume poll interval (milliseconds) — ~10fps for smooth updates */
#define VOLUME_POLL_INTERVAL_MS 100

/* ------------------------------------------------------------------ */
/* Watchdog Timer                                                      */
/* ------------------------------------------------------------------ */

/**
 * Watchdog timer callback.
 * Fires when the max recording duration is reached while in LISTENING state.
 * Transitions to TRANSCRIBING state.
 */
static gboolean watchdog_timer_callback(gpointer user_data) {
    (void)user_data;

    AppState state = app_get_state(&g_controller);
    if (state == STATE_LISTENING) {
        /* Transition to TRANSCRIBING */
        if (app_transition_to(&g_controller, STATE_TRANSCRIBING)) {
            /* Stop audio recording */
            if (g_audio_recorder) {
                audio_recorder_stop(g_audio_recorder);
            }
        }
    }

    /* Cancel the watchdog timer */
    g_watchdog_source_id = 0;
    return FALSE;
}

/**
 * Start the watchdog timer with the configured max duration.
 */
static void start_watchdog_timer(void) {
    if (g_watchdog_source_id != 0) {
        return; /* Already running */
    }

    int max_duration = g_controller.config->max_duration;
    if (max_duration <= 0) {
        max_duration = 30; /* Default 30 seconds */
    }

    /* Schedule the watchdog to fire after max_duration seconds */
    g_watchdog_source_id = g_timeout_add_seconds(max_duration,
                                                  watchdog_timer_callback,
                                                  NULL);
}

/* LOW-16 fix: Transcription phase watchdog timeout is now configurable via
 * the AppConfig max_duration field, scaled by a factor to allow longer
 * transcriptions for longer recordings. Minimum 30s, maximum 120s. */
static int get_transcription_timeout_seconds(void) {
    int base = g_controller.config ? g_controller.config->max_duration : 30;
    /* Scale: 1.5x the recording duration, clamped to [30, 120] */
    int scaled = base * 3 / 2;
    if (scaled < 30) scaled = 30;
    if (scaled > 120) scaled = 120;
    return scaled;
}

static gboolean transcription_watchdog_callback(gpointer user_data) {
    (void)user_data;

    AppState state = app_get_state(&g_controller);
    if (state == STATE_TRANSCRIBING) {
        /* Force transition back to IDLE */
        app_transition_to(&g_controller, STATE_IDLE);
        if (g_text_window) {
            char msg[64];
            int timeout = get_transcription_timeout_seconds();
            snprintf(msg, sizeof(msg), "Transcription timed out (%ds limit)", timeout);
            app_text_window_set_error(g_text_window, msg);
        }
        if (g_main_window) {
            app_window_set_state(g_main_window, STATE_IDLE);
        }
    }

    g_transcription_watchdog_source_id = 0;
    return FALSE;
}

static void start_transcription_watchdog(void) {
    if (g_transcription_watchdog_source_id != 0) {
        return; /* Already running */
    }
    int timeout = get_transcription_timeout_seconds();
    g_transcription_watchdog_source_id = g_timeout_add_seconds(
        timeout, transcription_watchdog_callback, NULL);
}

static void stop_transcription_watchdog(void) {
    if (g_transcription_watchdog_source_id != 0) {
        g_source_remove(g_transcription_watchdog_source_id);
        g_transcription_watchdog_source_id = 0;
    }
}

/**
 * Stop the watchdog timer.
 */
static void stop_watchdog_timer(void) {
    if (g_watchdog_source_id != 0) {
        g_source_remove(g_watchdog_source_id);
        g_watchdog_source_id = 0;
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
/* MIN-007 fix: Threshold-based volume level updates to reduce unnecessary
 * GTK widget redraws. Only update when level changes by more than VOLUME_DELTA. */
#define VOLUME_DELTA 0.05

static gboolean volume_poll_callback(gpointer user_data) {
    (void)user_data;

    if (g_audio_recorder && g_main_window) {
        double level = audio_recorder_get_volume_level(g_audio_recorder);
        /* Only update GTK widget if level changed significantly */
        if (level < 0.0) level = 0.0;
        if (level > 1.0) level = 1.0;
        double last_level = app_window_get_last_volume_level(g_main_window);
        if (level - last_level > VOLUME_DELTA ||
            last_level - level > VOLUME_DELTA ||
            last_level < 0.0) {
            app_window_set_volume_level(g_main_window, level);
            app_window_set_last_volume_level(g_main_window, level);
        }
    }

    return TRUE; /* Continue polling */
}

/**
 * Start the volume level polling timer.
 */
static void start_volume_poll(void) {
    if (g_volume_poll_source_id != 0) {
        return; /* Already running */
    }
    g_volume_poll_source_id = g_timeout_add(VOLUME_POLL_INTERVAL_MS,
                                             volume_poll_callback, NULL);
}

/**
 * Stop the volume level polling timer.
 */
static void stop_volume_poll(void) {
    if (g_volume_poll_source_id != 0) {
        g_source_remove(g_volume_poll_source_id);
        g_volume_poll_source_id = 0;
    }
}

/**
 * Handle config changes saved from the configuration dialog.
 * Applies runtime updates to the audio recorder (device, etc.).
 */
static void on_config_changed(void *user_data) {
    (void)user_data;
    if (g_audio_recorder && g_controller.config) {
        const char *device = config_get_audio_device(g_controller.config);
        if (device && device[0] != '\0') {
            audio_recorder_set_device(g_audio_recorder, device);
        } else {
            audio_recorder_set_device(g_audio_recorder, NULL);
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
static void handle_enter_listening(void) {

    /* Update the UI */
    if (g_main_window) {
        app_window_set_state(g_main_window, STATE_LISTENING);
    }

    /* Update tray icon */
    if (g_tray) {
        tray_set_state(g_tray, STATE_LISTENING);
    }

    /* Start audio recording */
    if (g_audio_recorder) {
        int max_duration = g_controller.config->max_duration;
        if (max_duration <= 0) max_duration = 30;
        if (audio_recorder_start(g_audio_recorder)) {
            /* Start the watchdog timer */
            start_watchdog_timer();
            /* Start volume level polling */
            start_volume_poll();
        } else {
            /* Failed to start recording — return to IDLE */
            const char *error = audio_recorder_get_error();
            if (error && error[0] != '\0') {
                if (g_text_window) {
                    app_text_window_set_error(g_text_window, error);
                }
            }
            app_transition_to(&g_controller, STATE_IDLE);
            if (g_main_window) {
                app_window_set_state(g_main_window, STATE_IDLE);
            }
        }
    } else {
        app_transition_to(&g_controller, STATE_IDLE);
        if (g_main_window) {
            app_window_set_state(g_main_window, STATE_IDLE);
        }
    }
}

/**
 * Handle transition to TRANSCRIBING state.
 * Stops audio recording and initiates transcription.
 */
static void handle_enter_transcribing(const char *wav_path) {

    /* LO-05 fix: Use GTK window bell instead of terminal BEL character,
     * which works reliably in modern desktop environments */
    if (g_main_window) {
        GtkWindow *gtk_win = GTK_WINDOW(app_window_get_gtk_window(g_main_window));
        GdkWindow *gdk_win = gtk_win ? gtk_widget_get_window(GTK_WIDGET(gtk_win)) : NULL;
        gdk_display_beep(gdk_win ? gdk_window_get_display(gdk_win) : gdk_display_get_default());
    }

    /* Stop the watchdog timer */
    stop_watchdog_timer();

    /* Stop volume polling */
    stop_volume_poll();

    /* Update the UI */
    if (g_main_window) {
        app_window_set_state(g_main_window, STATE_TRANSCRIBING);
    }

    /* Update tray icon */
    if (g_tray) {
        tray_set_state(g_tray, STATE_TRANSCRIBING);
    }

    /* Store the WAV path for transcription */
    pthread_mutex_lock(&g_wav_path_mutex);
    if (wav_path) {
        size_t len = strlen(wav_path);
        if (len >= sizeof(g_current_wav_path)) {
            g_log("main", G_LOG_LEVEL_WARNING,
                  "[main] WAV path truncated: %.100s... (len=%zu, max=%zu)\n",
                  wav_path, len, sizeof(g_current_wav_path) - 1);
        }
        g_strlcpy(g_current_wav_path, wav_path, sizeof(g_current_wav_path));
    } else if (g_audio_recorder) {
        const char *path = audio_recorder_get_wav_path(g_audio_recorder);
        if (path) {
            size_t len = strlen(path);
            if (len >= sizeof(g_current_wav_path)) {
                g_log("main", G_LOG_LEVEL_WARNING,
                      "[main] WAV path truncated: %.100s... (len=%zu, max=%zu)\n",
                      path, len, sizeof(g_current_wav_path) - 1);
            }
            g_strlcpy(g_current_wav_path, path, sizeof(g_current_wav_path));
        }
    }
    pthread_mutex_unlock(&g_wav_path_mutex);

    /* Start transcription in a background thread */
    if (g_whisper_client && g_current_wav_path[0] != '\0') {
        /* MIN-012 fix: Start transcription watchdog (30s constant timeout) */
        start_transcription_watchdog();

        /* HI-04 fix: Store thread handle for clean shutdown */
        /* Perform transcription (blocking call in background thread) */
        pthread_mutex_lock(&g_transcribe_thread_mutex);
        g_transcribe_thread = g_thread_new("transcribe",
                       (GThreadFunc)transcribe_thread_func,
                       NULL);
        pthread_mutex_unlock(&g_transcribe_thread_mutex);
    } else {
        /* No whisper client or WAV path — return to IDLE */
        if (g_text_window) {
            app_text_window_set_error(g_text_window, "No audio file available for transcription");
        }
        app_transition_to(&g_controller, STATE_IDLE);
        if (g_main_window) {
            app_window_set_state(g_main_window, STATE_IDLE);
        }
    }
}

/**
 * Background thread function for transcription.
 */
static gpointer transcribe_thread_func(gpointer data) {
    (void)data;

    const char *model_path = config_get_model_path(g_controller.config);
    whisper_client_set_model_path(g_whisper_client, model_path);

    /* Copy WAV path under mutex */
    pthread_mutex_lock(&g_wav_path_mutex);
    char wav_path[PATH_MAX];
    g_strlcpy(wav_path, g_current_wav_path, sizeof(wav_path));
    pthread_mutex_unlock(&g_wav_path_mutex);


    /* Perform transcription with retry */
    WhisperResponse *response = whisper_transcribe_with_retry(g_whisper_client,
                                                wav_path,
                                                3); /* 3 retries */

    /* Marshal result to the GTK main thread */
    if (g_controller.on_transcription_result) {
        if (response && response->success && response->text && response->text[0] != '\0') {
            g_idle_add(on_transcription_result_idle, g_strdup(response->text));
        } else {
            const char *error = whisper_client_get_error(g_whisper_client);
            if (!error || error[0] == '\0') {
                error = response && response->error_message[0] != '\0'
                        ? response->error_message
                        : "Transcription returned empty result";
            }
            g_idle_add(on_transcription_result_error_idle, g_strdup(error));
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
 * Handle transcription result (success).
 * Called from the GTK main thread via g_idle_add().
 */
static void on_transcription_result(const char *text, bool success, void *user_data) {
    (void)user_data;

    /* MIN-012 fix: Stop transcription watchdog on result */
    stop_transcription_watchdog();

    if (success && text) {
        /* Append transcribed text to the TextWindow */
        if (g_text_window) {
            app_text_window_append_text(g_text_window, text);
        }

        /* Copy to clipboard (both PRIMARY and CLIPBOARD) */
        if (clipboard_is_available(NULL)) {
            clipboard_copy_text_both(NULL, text);
        }

        /* Delete the temporary WAV file after transcription */
        if (g_audio_recorder) {
            audio_recorder_delete_wav(g_audio_recorder);
        }
    }

    /* Transition back to IDLE */
    app_transition_to(&g_controller, STATE_IDLE);
    if (g_main_window) {
        app_window_set_state(g_main_window, STATE_IDLE);
    }

    /* Update tray icon */
    if (g_tray) {
        tray_set_state(g_tray, STATE_IDLE);
    }
}

/**
 * Handle transcription result (error).
 * Called from the GTK main thread via g_idle_add().
 */
static void on_transcription_result_error(const char *error, bool success, void *user_data) {
    (void)success;
    (void)user_data;

    /* MIN-012 fix: Stop transcription watchdog on error */
    stop_transcription_watchdog();

    if (error) {
        if (g_text_window) {
            app_text_window_set_error(g_text_window, error);
        }
    }

    /* Transition back to IDLE */
    app_transition_to(&g_controller, STATE_IDLE);
    if (g_main_window) {
        app_window_set_state(g_main_window, STATE_IDLE);
    }

    /* Update tray icon */
    if (g_tray) {
        tray_set_state(g_tray, STATE_IDLE);
    }
}

/**
 * Handle connection status change.
 * Called from the GTK main thread.
 */
static void on_connection_status_change(ConnectionStatus status, void *user_data) {
    (void)user_data;

    if (g_main_window) {
        app_window_set_connection_status(g_main_window, status);
    }

    if (g_tray) {
        tray_set_connection_status(g_tray, status);
    }
}

/**
 * Handle recording stop.
 * Called when audio recording completes (either by watchdog or user stop).
 */
static void on_recording_stop(const char *wav_path, void *user_data) {
    (void)user_data;

    /* Transition to TRANSCRIBING */
    if (app_get_state(&g_controller) == STATE_LISTENING) {
        handle_enter_transcribing(wav_path);
    }
}

/**
 * Background thread: loads the whisper model with GPU auto-detection.
 * On success, transitions to LISTENING. On failure, shows error dialog.
 */
static gpointer model_loading_thread_func(gpointer data) {
    (void)data;

    /* Set model path from config */
    const char *model_path = config_get_model_path(g_controller.config);
    whisper_client_set_model_path(g_whisper_client, model_path);

    /* Load the model with GPU mode from config.
     * gpu_mode can be "auto", "cpu", or "gpu:N".
     * This blocks until the model is loaded (or fails). */
    const char *gpu_mode = config_get_gpu_mode(g_controller.config);
    bool loaded = whisper_client_load_model(g_whisper_client, gpu_mode);

    /* Marshal result back to GTK main thread */
    if (loaded) {
        g_idle_add(on_model_loaded_idle, NULL);
    } else {
        const char *error = whisper_client_get_error(g_whisper_client);
        g_idle_add(on_model_load_failed_idle,
                   error ? g_strdup(error) : g_strdup("Failed to load model"));
    }

    return NULL;
}

/**
 * GTK idle callback: model loaded successfully — clear "WAIT" indicator.
 * If the loading was triggered by user click (lazy loading), also
 * auto-transition to LISTENING to start recording.
 */
static gboolean on_model_loaded_idle(gpointer data) {
    (void)data;

    /* Capture and clear the flag atomically before any potential re-entry */
    bool from_toggle = atomic_exchange(&g_model_loading_from_toggle, 0) != 0;

    /* Clear the "WAIT" overlay from the icon */
    if (g_main_window) {
        app_window_set_model_loading(g_main_window, FALSE);
    }

    /* Update connection status to connected */
    app_set_connection_status(&g_controller, CONNECTION_CONNECTED);
    if (g_main_window) {
        app_window_set_connection_status(g_main_window, CONNECTION_CONNECTED);
    }
    if (g_tray) {
        tray_set_connection_status(g_tray, CONNECTION_CONNECTED);
    }

    /* If loading was triggered by user click, auto-transition to LISTENING */
    if (from_toggle) {
        app_toggle_state(&g_controller);
    }

    return FALSE;
}

/**
 * GTK idle callback: model loading failed — show error, clear "WAIT".
 * Falls back to disconnected state so lazy loading on first click
 * can still attempt to load the model.
 */
static gboolean on_model_load_failed_idle(gpointer data) {
    char *error_msg = (char *)data;

    /* Clear the "WAIT" overlay from the icon */
    if (g_main_window) {
        app_window_set_model_loading(g_main_window, FALSE);
    }

    /* Reset connection status to disconnected */
    app_set_connection_status(&g_controller, CONNECTION_DISCONNECTED);
    if (g_main_window) {
        app_window_set_connection_status(g_main_window, CONNECTION_DISCONNECTED);
    }
    if (g_tray) {
        tray_set_connection_status(g_tray, CONNECTION_DISCONNECTED);
    }

    /* M-005 fix: Show non-modal, auto-closing warning dialog.
     * Using gtk_window_present() + response signal instead of gtk_dialog_run()
     * to avoid blocking the GTK main loop. The dialog auto-closes after 8 seconds. */
    {
        GtkWindow *parent = GTK_WINDOW(app_window_get_gtk_window(g_main_window));
        GtkDialog *dialog = GTK_DIALOG(gtk_message_dialog_new(
            parent,
            GTK_DIALOG_DESTROY_WITH_PARENT,  /* Non-modal — does not block main loop */
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_OK,
            "Failed to load Whisper model at startup.\n\n%s\n\n"
            "The application will attempt to load the model when you "
            "first click the microphone icon.", error_msg ? error_msg : "Unknown error"));
        gtk_window_set_title(GTK_WINDOW(dialog), "Model Load Warning");
        /* Auto-close after 8 seconds */
        g_timeout_add_seconds(8, auto_close_dialog, dialog);
        /* Present without blocking */
        gtk_window_present(GTK_WINDOW(dialog));
    }

    g_free(error_msg);
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
    (void)user_data;

    /* HI-03 fix: Only check local model when starting recording (IDLE -> LISTENING) */
    AppState current = app_get_state(&g_controller);
    if (current == STATE_IDLE) {
        /* STARTUP-LOADING: If model is still loading from startup,
         * reject the click. The "WAIT" overlay on the icon should
         * have made this clear to the user. */
        if (g_main_window && app_window_get_model_loading(g_main_window)) {
            return;  /* Model still loading — ignore click */
        }
        /* CFG-015: Validate that the configured audio device is available */
        {
            const char *configured_device = config_get_audio_device(g_controller.config);
            bool device_valid = false;

            /* "default" is always valid — ALSA will use the system default */
            if (configured_device && g_strcmp0(configured_device, "default") == 0) {
                device_valid = true;
            } else if (configured_device && configured_device[0] != '\0') {
                /* Check if the configured device exists in the available device list */
                AudioDeviceList *devices = audio_recorder_get_device_list(g_audio_recorder);
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
                /* HIGH-4 fix: Non-modal, auto-closing dialog to avoid blocking GTK main loop */
                GtkWindow *parent = GTK_WINDOW(app_window_get_gtk_window(g_main_window));
                GtkDialog *dialog = GTK_DIALOG(gtk_message_dialog_new(
                    parent,
                    GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_OK,
                    "The configured microphone is not available.\n\n"
                    "Please select a valid microphone in Transcriber Settings."));
                gtk_window_set_title(GTK_WINDOW(dialog), "Microphone Not Available");
                g_timeout_add_seconds(8, auto_close_dialog, dialog);
                gtk_window_present(GTK_WINDOW(dialog));
                return; /* Abort — do not start recording */
            }
        }

        /* Check that a valid GGUF model file is configured and accessible */
        const char *model_path = config_get_model_path(g_controller.config);
        if (!model_path || model_path[0] == '\0' ||
            !config_dialog_validate_gguf_model(model_path)) {
            /* HIGH-4 fix: Non-modal, auto-closing dialog */
            GtkWindow *parent = GTK_WINDOW(app_window_get_gtk_window(g_main_window));
            GtkDialog *dialog = GTK_DIALOG(gtk_message_dialog_new(
                parent,
                GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_OK,
                APP_ERROR_NO_VALID_MODEL ".\n\n"
                "Please configure a valid Whisper model file in Settings."));
            gtk_window_set_title(GTK_WINDOW(dialog), "Model Not Found");
            g_timeout_add_seconds(8, auto_close_dialog, dialog);
            gtk_window_present(GTK_WINDOW(dialog));
            return; /* Abort — do not start recording */
        }

        /* First check cached connection status */
        ConnectionStatus conn = app_get_connection_status(&g_controller);
        if (conn == CONNECTION_DISCONNECTED) {
            /* Do a live check to confirm if model is available */
            bool model_available = true;
            if (g_whisper_client) {
                model_available = whisper_check_connection(g_whisper_client);
            } else {
                model_available = false;
            }
            if (!model_available) {
                /* HIGH-4 fix: Non-modal, auto-closing dialog */
                GtkWindow *parent = GTK_WINDOW(app_window_get_gtk_window(g_main_window));
                GtkDialog *dialog = GTK_DIALOG(gtk_message_dialog_new(
                    parent,
                    GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_OK,
                    "Local Whisper model is unavailable.\n\n"
                    "Please check that the model file exists and is "
                    "correctly configured, then try again."));
                gtk_window_set_title(GTK_WINDOW(dialog), "Model Unavailable");
                g_timeout_add_seconds(8, auto_close_dialog, dialog);
                gtk_window_present(GTK_WINDOW(dialog));
                return; /* Abort — do not start recording */
            }
        }

        /* MODEL-LOADING: If model file exists but is not yet loaded,
         * start loading in a background thread with LOADING indicator.
         * Set flag so on_model_loaded_idle auto-transitions to LISTENING. */
        if (g_whisper_client && !whisper_client_is_model_loaded(g_whisper_client) &&
            !whisper_client_is_loading(g_whisper_client)) {

            /* Mark that this load was triggered by user click,
             * so on_model_loaded_idle will auto-transition to LISTENING */
            atomic_store(&g_model_loading_from_toggle, 1);

            /* Set model path */
            const char *model_path = config_get_model_path(g_controller.config);
            whisper_client_set_model_path(g_whisper_client, model_path);

            /* Show LOADING indicator and "WAIT" overlay */
            app_set_connection_status(&g_controller, CONNECTION_LOADING);
            if (g_main_window) {
                app_window_set_connection_status(g_main_window, CONNECTION_LOADING);
                app_window_set_model_loading(g_main_window, TRUE);
            }
            if (g_tray) {
                tray_set_connection_status(g_tray, CONNECTION_LOADING);
            }

            /* H-001 fix: Store model loading thread handle for clean shutdown */
            pthread_mutex_lock(&g_model_load_thread_mutex);
            g_model_load_thread = g_thread_new("model_loading",
                                  model_loading_thread_func, NULL);
            pthread_mutex_unlock(&g_model_load_thread_mutex);

            return; /* Wait for loading to complete before recording */
        }

        /* If model is already loading (another click), just return */
        if (g_whisper_client && whisper_client_is_loading(g_whisper_client)) {
            return;
        }
    }

    /* Toggle the state — on_state_change callback will handle the actual work.
     * If we reach here, either:
     * 1. Model was already loaded (normal fast path)
     * 2. User is stopping recording (LISTENING -> TRANSCRIBING)
     * 3. User is in TRANSCRIBING state (no-op) */
    app_toggle_state(&g_controller);
}

/**
 * Handle D-Bus toggle request.
 * Called when the system hotkey triggers the D-Bus ToggleMicrophone method.
 */
static void on_dbus_toggle(void *user_data) {
    on_microphone_toggle(user_data);
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
static void perform_initial_model_load(void) {
    if (!g_whisper_client) return;

    /* Validate model path before attempting load */
    const char *model_path = config_get_model_path(g_controller.config);
    if (!model_path || model_path[0] == '\0') {
        /* No model configured — clear WAIT, show disconnected */
        if (g_main_window) {
            app_window_set_model_loading(g_main_window, FALSE);
        }
        app_set_connection_status(&g_controller, CONNECTION_DISCONNECTED);
        if (g_main_window) {
            app_window_set_connection_status(g_main_window, CONNECTION_DISCONNECTED);
        }
        return;
    }

    /* Set model path */
    whisper_client_set_model_path(g_whisper_client, model_path);

    /* Show LOADING indicator on the status bar */
    app_set_connection_status(&g_controller, CONNECTION_LOADING);
    if (g_main_window) {
        app_window_set_connection_status(g_main_window, CONNECTION_LOADING);
    }
    if (g_tray) {
        tray_set_connection_status(g_tray, CONNECTION_LOADING);
    }

    /* The "WAIT" overlay is already active (set to TRUE in app_window_create).
      * Start background thread to load the model. */
    /* H-001 fix: Store model loading thread handle for clean shutdown */
    pthread_mutex_lock(&g_model_load_thread_mutex);
    g_model_load_thread = g_thread_new("startup_model_load",
                                       model_loading_thread_func, NULL);
    pthread_mutex_unlock(&g_model_load_thread_mutex);
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
static void on_state_change(AppState new_state, void *user_data) {
    (void)user_data;

    switch (new_state) {
        case STATE_LISTENING:
            handle_enter_listening();
            break;
        case STATE_TRANSCRIBING:
            if (g_audio_recorder) {
                audio_recorder_stop(g_audio_recorder);
            }
            {
                const char *path = NULL;
                if (g_audio_recorder) {
                    path = audio_recorder_get_wav_path(g_audio_recorder);
                }
                if (path && path[0] != '\0') {
                    handle_enter_transcribing(path);
                } else {
                    app_transition_to(&g_controller, STATE_IDLE);
                    if (g_main_window) {
                        app_window_set_state(g_main_window, STATE_IDLE);
                    }
                }
            }
            break;
        case STATE_IDLE:
            if (g_main_window) {
                app_window_set_state(g_main_window, STATE_IDLE);
            }
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Main Entry Point                                                    */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {

    /* Initialize GTK */
    gtk_init(&argc, &argv);

    /* Suppress whisper.cpp library internal log output */
    whisper_log_set(NULL, NULL);

    /* Load configuration */
    AppConfig config;
    config_set_defaults(&config);

    config_load(&config);

    /* Initialize the state controller */
    if (app_state_controller_init(&g_controller,
                                  &config,
                                  on_transcription_result,
                                  on_connection_status_change,
                                  on_recording_stop,
                                  on_state_change,
                                  NULL) != 0) {
        return 1;
    }

    /* Create the audio recorder */
    AudioFormat fmt = audio_format_get_default();
    const char *device = config_get_audio_device(&config);
    g_audio_recorder = audio_recorder_create(&fmt);
    if (!g_audio_recorder) {
        app_state_controller_cleanup(&g_controller);
        return 1;
    }

    /* Set the configured audio device on the recorder */
    if (device && device[0] != '\0') {
        audio_recorder_set_device(g_audio_recorder, device);
    }

    /* Create the Whisper client */
    g_whisper_client = whisper_client_create();
    if (!g_whisper_client) {
        audio_recorder_destroy(g_audio_recorder);
        app_state_controller_cleanup(&g_controller);
        return 1;
    }

    /* Create the MainWindow */
    g_main_window = app_window_create(&config, &g_controller, g_whisper_client);
    if (!g_main_window) {
        whisper_client_destroy(g_whisper_client);
        audio_recorder_destroy(g_audio_recorder);
        app_state_controller_cleanup(&g_controller);
        return 1;
    }

    /* Register the toggle callback for mic icon clicks */
    app_window_set_toggle_callback(g_main_window, on_microphone_toggle, NULL);

    /* Register the config-changed callback to apply runtime config updates */
    app_window_set_config_changed_callback(g_main_window, on_config_changed, NULL);

    /* Create the TextWindow */
    GtkWindow *gtk_win = app_window_get_gtk_window(g_main_window);
    g_text_window = app_text_window_create(gtk_win);

    /* Create the system tray icon */
    g_tray = tray_create();
    if (g_tray) {
        tray_set_main_window(g_tray, gtk_win);
        tray_set_toggle_callback(g_tray, on_microphone_toggle, NULL);
    }

    /* Start the D-Bus service */
    g_dbus_service = dbus_service_create();
    if (g_dbus_service) {
        if (!dbus_service_start(g_dbus_service, on_dbus_toggle, NULL)) {
            /* Continue without D-Bus — single-instance not enforced */
            dbus_service_destroy(g_dbus_service);
            g_dbus_service = NULL;
        }
    }

    /* Show the main window (with "WAIT" overlay on the mic icon) */
    gtk_widget_show_all(GTK_WIDGET(gtk_win));

    /* Load the Whisper model at startup in a background thread.
     * The window shows "WAIT" overlay on the red mic icon until
     * the model is fully loaded. Once loaded, "WAIT" is removed
     * and the connection indicator turns green. This eliminates
     * the lag when the user first clicks the mic to start recording. */
    perform_initial_model_load();

    /* Start the GTK main loop */
    gtk_main();

    /* ------------------------------------------------------------------ */
    /* Shutdown                                                            */
    /* ------------------------------------------------------------------ */

    /* H-002 fix: Set shutdown flag to prevent idle callbacks from accessing freed resources */
    g_shutting_down = true;

    /* Stop the watchdog timer */
    stop_watchdog_timer();

    /* Stop volume polling */
    stop_volume_poll();

    /* Destroy the system tray icon */
    if (g_tray) {
        tray_destroy(g_tray);
        g_tray = NULL;
    }

    /* Stop the D-Bus service */
    if (g_dbus_service) {
        dbus_service_stop(g_dbus_service);
        dbus_service_destroy(g_dbus_service);
        g_dbus_service = NULL;
    }

    /* Destroy the TextWindow */
    if (g_text_window) {
        app_text_window_destroy(g_text_window);
        g_text_window = NULL;
    }

    /* Save window position */
    if (g_main_window) {
        app_window_save_position(g_main_window);
    }

    /* Destroy the MainWindow */
    if (g_main_window) {
        app_window_destroy(g_main_window);
        g_main_window = NULL;
    }

    /* HI-04 fix: Gracefully cancel and join any in-flight transcription thread
     * before destroying the client. This prevents use-after-free during shutdown. */
    if (g_whisper_client) {
        whisper_client_cancel(g_whisper_client);
    }

    /* CRIT-2 fix: Join the transcription thread. The whisper_client_cancel()
     * above sets the cancel flag which whisper.cpp checks periodically during
     * whisper_full(). If the thread is stuck (e.g., in a long GPU operation),
     * the join may block. In practice, the cancel callback is checked every
     * encoder/decoder step, so this should complete within a few seconds. */
    pthread_mutex_lock(&g_transcribe_thread_mutex);
    if (g_transcribe_thread) {
        g_thread_join(g_transcribe_thread);
        g_transcribe_thread = NULL;
    }
    pthread_mutex_unlock(&g_transcribe_thread_mutex);

    /* H-001 fix: Join the model loading thread if it's still running */
    pthread_mutex_lock(&g_model_load_thread_mutex);
    if (g_model_load_thread) {
        g_thread_join(g_model_load_thread);
        g_model_load_thread = NULL;
    }
    pthread_mutex_unlock(&g_model_load_thread_mutex);

    if (g_whisper_client) {
        whisper_client_destroy(g_whisper_client);
        g_whisper_client = NULL;
    }

    /* Destroy the audio recorder */
    if (g_audio_recorder) {
        audio_recorder_destroy(g_audio_recorder);
        g_audio_recorder = NULL;
    }

    /* Cleanup the state controller */
    app_state_controller_cleanup(&g_controller);

    return 0;
}
