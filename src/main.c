/*
 * main.c — Application Entry Point, State Controller, Recording Lifecycle,
 *          Runtime Config Updates, Recording Completion Beep
 *
 * This file serves two purposes:
 *
 *   1. Implements the AppStateController API declared in app.h
 *      (lifecycle, state transitions, thread-safe accessors).
 *
 *   2. Provides the main() entry point that initializes all subsystems,
 *      creates the MainWindow, sets up D-Bus service, starts the GTK
 *      main loop, and handles graceful shutdown.
 *
 * Application Startup Sequence:
 *   1. Initialize GTK3 and libcurl
 *   2. Load configuration from ~/.config/transcriber/config.json
 *   3. Initialize the AppStateController
 *   4. Create the AudioRecorder with configured device
 *   5. Create the WhisperClient
 *   6. Create the MainWindow and TextWindow
 *   7. Register toggle callback (mic icon click → start/stop recording)
 *   8. Register config-changed callback (apply runtime config updates)
 *   9. Start the D-Bus service (single-instance enforcement)
 *   10. Perform initial connection check to Whisper API
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
 *   - When recording finishes (handle_enter_transcribing), an ASCII BEL
 *     character (0x07) is emitted to stderr to trigger the system beep.
 *
 * Threading:
 *   - Main thread: GTK main loop, UI updates, D-Bus message processing
 *   - Audio thread: Created by app_audio.c for PCM capture
 *   - Network thread: Created by app_whisper.c for HTTP transcription
 *
 * All cross-thread communication flows through callback function pointers
 * registered with the AppStateController, marshaled to the GTK main thread
 * via g_idle_add().
 */

#define _POSIX_C_SOURCE 199309L

#include "app.h"
#include "app_audio.h"
#include "app_whisper.h"
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
#include <curl/curl.h>

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
static gboolean volume_poll_callback(gpointer user_data);

/* Wrappers for g_idle_add (GSourceFunc signature: gboolean (*)(gpointer)) */
static gboolean on_transcription_result_idle(gpointer data) {
    on_transcription_result((const char *)data, true, NULL);
    g_free(data);
    return FALSE;
}

static gboolean on_transcription_result_error_idle(gpointer data) {
    on_transcription_result_error((const char *)data, false, NULL);
    g_free(data);
    return FALSE;
}

static gboolean on_connection_status_idle(gpointer data) {
    ConnectionStatus status = (ConnectionStatus)GPOINTER_TO_INT(data);
    on_connection_status_change(status, NULL);
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
static guint g_connection_poll_source_id = 0;
static guint g_volume_poll_source_id = 0;
static SystemTray *g_tray = NULL;
static char g_current_wav_path[PATH_MAX];
static pthread_mutex_t g_wav_path_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Periodic connection poll interval (seconds) */
#define CONNECTION_POLL_INTERVAL_SECONDS 5

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

/* MIN-012 fix: Transcription phase watchdog with constant 30s timeout (NR-019) */
static const int TRANSCRIPTION_TIMEOUT_SECONDS = 30;

static gboolean transcription_watchdog_callback(gpointer user_data) {
    (void)user_data;

    AppState state = app_get_state(&g_controller);
    if (state == STATE_TRANSCRIBING) {
        /* Force transition back to IDLE */
        app_transition_to(&g_controller, STATE_IDLE);
        if (g_text_window) {
            app_text_window_set_error(g_text_window, "Transcription timed out (30s limit)");
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
    g_transcription_watchdog_source_id = g_timeout_add_seconds(
        TRANSCRIPTION_TIMEOUT_SECONDS, transcription_watchdog_callback, NULL);
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
static gboolean volume_poll_callback(gpointer user_data) {
    (void)user_data;

    if (g_audio_recorder && g_main_window) {
        double level = audio_recorder_get_volume_level(g_audio_recorder);
        app_window_set_volume_level(g_main_window, level);
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
            fprintf(stderr, "[config] Updated audio device to: %s\n", device);
        } else {
            audio_recorder_set_device(g_audio_recorder, NULL);
            fprintf(stderr, "[config] Reset audio device to default\n");
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
        if (audio_recorder_start(g_audio_recorder, max_duration)) {
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

    /* Play a beep to signal recording is done */
    fprintf(stderr, "\a");

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
        g_strlcpy(g_current_wav_path, wav_path, sizeof(g_current_wav_path));
    } else if (g_audio_recorder) {
        const char *path = audio_recorder_get_wav_path(g_audio_recorder);
        if (path) {
            g_strlcpy(g_current_wav_path, path, sizeof(g_current_wav_path));
        }
    }
    pthread_mutex_unlock(&g_wav_path_mutex);

    /* Start transcription in a background thread */
    if (g_whisper_client && g_current_wav_path[0] != '\0') {
        const char *language = config_get_language(g_controller.config);
        whisper_client_set_language(g_whisper_client, language);

        /* MIN-012 fix: Start transcription watchdog (30s constant timeout) */
        start_transcription_watchdog();

        /* Perform transcription (blocking call in background thread) */
        /* We use g_thread_new to run this in a separate thread */
        g_thread_new("transcribe",
                      (GThreadFunc)transcribe_thread_func,
                      NULL);
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

    const char *url = config_get_whisper_url(g_controller.config);
    whisper_client_set_url(g_whisper_client, url);

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
        /* Display the text in the TextWindow */
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
 * Handle microphone toggle request.
 * Shared by both the D-Bus toggle and the icon click handler.
 *
 * ERR-014: Before starting recording, check if the Whisper server is
 * reachable. If unavailable, show an error dialog and abort.
 */
static void on_microphone_toggle(void *user_data) {
    (void)user_data;

    /* Only check server when starting recording (IDLE -> LISTENING) */
    AppState current = app_get_state(&g_controller);
    if (current == STATE_IDLE) {
        /* First check cached connection status */
        ConnectionStatus conn = app_get_connection_status(&g_controller);
        if (conn == CONNECTION_DISCONNECTED) {
            /* Do a live check to confirm if client exists */
            bool server_available = true;
            if (g_whisper_client) {
                server_available = whisper_check_connection(g_whisper_client);
            } else {
                server_available = false;
            }
            if (!server_available) {
                /* ME-06 fix: Consolidated error dialog for both cases */
                GtkWindow *parent = GTK_WINDOW(app_window_get_gtk_window(g_main_window));
                GtkDialog *dialog = GTK_DIALOG(gtk_message_dialog_new(
                    parent,
                    GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_OK,
                    "Whisper server is unavailable.\n\n"
                    "Please check that the Whisper API server is running "
                    "and reachable, then try again."));
                gtk_window_set_title(GTK_WINDOW(dialog), "Server Unavailable");
                gtk_dialog_run(dialog);
                gtk_widget_destroy(GTK_WIDGET(dialog));
                return; /* Abort — do not start recording */
            }
        }
    }

    /* Toggle the state — state_monitor_callback will handle the actual work */
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
/* Connection Check                                                    */
/* ------------------------------------------------------------------ */

/**
 * Perform an initial connection check to the Whisper API.
 * Runs in a background thread to avoid blocking the GTK main loop.
 */
static gpointer connection_check_thread_func(gpointer data) {
    (void)data;

    if (g_whisper_client) {
        bool connected = whisper_check_connection(g_whisper_client);

        ConnectionStatus status = connected ? CONNECTION_CONNECTED
                                            : CONNECTION_DISCONNECTED;

        /* Update the controller state */
        app_set_connection_status(&g_controller, status);

        /* Marshal UI update to the GTK main thread */
        g_idle_add(on_connection_status_idle,
                   GINT_TO_POINTER((int)status));
    }

    return NULL;
}

/**
 * Start the initial connection check.
 */
static void perform_initial_connection_check(void) {
    if (!g_whisper_client) return;

    const char *url = config_get_whisper_url(g_controller.config);
    whisper_client_set_url(g_whisper_client, url);

    /* Set status to CHECKING */
    app_set_connection_status(&g_controller, CONNECTION_CHECKING);
    if (g_main_window) {
        app_window_set_connection_status(g_main_window, CONNECTION_CHECKING);
    }

    /* Start the connection check in a background thread */
    g_thread_new("connection_check", connection_check_thread_func, NULL);
}

/* ------------------------------------------------------------------ */
/* Periodic Connection Polling                                         */
/* ------------------------------------------------------------------ */

/**
 * CURL write callback for periodic connection polling.
 */
static size_t poll_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)userdata;
    (void)ptr;
    return size * nmemb;
}

/**
 * Background thread function for periodic connection polling.
 * Uses a standalone CURL handle to avoid contention with the shared
 * WhisperClient handle used by the transcription thread.
 * Silently updates connection status without showing CHECKING state.
 */
static gpointer periodic_connection_poll_thread_func(gpointer data) {
    (void)data;

    if (!g_controller.config) {
        return NULL;
    }

    /* Use a standalone CURL handle — do NOT use g_whisper_client,
     * as its handle is shared with the transcription thread. */
    CURL *curl = curl_easy_init();
    if (!curl) {
        return NULL;
    }

    const char *url = config_get_whisper_url(g_controller.config);
    char base_url[512];
    /* Extract base URL by stripping the /v1/audio/transcriptions suffix */
    snprintf(base_url, sizeof(base_url), "%s", url);
    char *slash = strstr(base_url, "/v1/audio/transcriptions");
    if (slash) {
        *slash = '\0';
    }
    char full_url[600];
    snprintf(full_url, sizeof(full_url), "%s/v1/models", base_url);

    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, poll_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);

    CURLcode curl_err = curl_easy_perform(curl);
    long http_status = 0;
    if (curl_err == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    }

    bool connected = (curl_err == CURLE_OK && http_status == 200);

    curl_easy_cleanup(curl);

    ConnectionStatus status = connected ? CONNECTION_CONNECTED
                                        : CONNECTION_DISCONNECTED;

    /* Update the controller state */
    app_set_connection_status(&g_controller, status);

    /* Marshal UI update to the GTK main thread */
    g_idle_add(on_connection_status_idle,
               GINT_TO_POINTER((int)status));

    return NULL;
}

/**
 * GLib timeout callback — fires every 5 seconds to trigger a connection poll.
 */
static gboolean periodic_connection_poll_callback(gpointer user_data) {
    (void)user_data;

    /* Skip poll if libcurl handle is busy (transcription in progress)
     * to avoid concurrent use of the same CURL handle, which crashes. */
    AppState state = app_get_state(&g_controller);
    if (state == STATE_TRANSCRIBING) {
        return TRUE; /* Keep the timer alive, skip this cycle */
    }

    /* Skip poll if already in a CHECKING state (avoid redundant checks) */
    ConnectionStatus current = app_get_connection_status(&g_controller);
    if (current == CONNECTION_CHECKING) {
        return TRUE; /* Keep the timer alive, skip this cycle */
    }

    g_thread_new("connection_poll", periodic_connection_poll_thread_func, NULL);
    return TRUE; /* Keep the timer alive */
}

/**
 * Start the periodic connection polling timer.
 */
static void start_connection_polling(void) {
    if (g_connection_poll_source_id != 0) {
        return; /* Already running */
    }
    g_connection_poll_source_id = g_timeout_add_seconds(
        CONNECTION_POLL_INTERVAL_SECONDS, periodic_connection_poll_callback, NULL);
}

/**
 * Stop the periodic connection polling timer.
 */
static void stop_connection_polling(void) {
    if (g_connection_poll_source_id != 0) {
        g_source_remove(g_connection_poll_source_id);
        g_connection_poll_source_id = 0;
    }
}

/* ------------------------------------------------------------------ */
/* State Change Monitoring                                             */
/* ------------------------------------------------------------------ */

/**
 * Idle callback to monitor state changes and trigger handlers.
 * This runs in the GTK main loop and detects state transitions.
 */
static AppState g_last_monitored_state = STATE_IDLE;

static gboolean state_monitor_callback(gpointer user_data) {
    (void)user_data;

    AppState current = app_get_state(&g_controller);

    if (current != g_last_monitored_state) {
        /* State has changed — trigger appropriate handler */
        switch (current) {
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
                        g_last_monitored_state = STATE_IDLE;
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
        g_last_monitored_state = current;
    }

    return TRUE; /* Continue monitoring */
}

/* ------------------------------------------------------------------ */
/* Main Entry Point                                                    */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    /* Initialize libcurl once at startup (MAJ-002 fix) */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Initialize GTK */
    gtk_init(&argc, &argv);

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
                                  NULL) != 0) {
        curl_global_cleanup();
        return 1;
    }

    /* Create the audio recorder */
    AudioFormat fmt = audio_format_get_default();
    const char *device = config_get_audio_device(&config);
    g_audio_recorder = audio_recorder_create(&fmt);
    if (!g_audio_recorder) {
        app_state_controller_cleanup(&g_controller);
        curl_global_cleanup();
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
        curl_global_cleanup();
        return 1;
    }

    /* Set Whisper client timeouts from config */
    whisper_client_set_connect_timeout(g_whisper_client, 5);
    whisper_client_set_total_timeout(g_whisper_client, 30);

    /* Create the MainWindow */
    g_main_window = app_window_create(&config, &g_controller, g_whisper_client);
    if (!g_main_window) {
        whisper_client_destroy(g_whisper_client);
        audio_recorder_destroy(g_audio_recorder);
        app_state_controller_cleanup(&g_controller);
        curl_global_cleanup();
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

    /* Add state monitor to the GTK main loop */
    g_idle_add(state_monitor_callback, NULL);

    /* Show the main window */
    gtk_widget_show_all(GTK_WIDGET(gtk_win));

    /* Perform initial connection check */
    perform_initial_connection_check();

    /* Start periodic connection polling (every 5 seconds) */
    start_connection_polling();

    /* Start the GTK main loop */
    gtk_main();

    /* ------------------------------------------------------------------ */
    /* Shutdown                                                            */
    /* ------------------------------------------------------------------ */

    /* Stop the watchdog timer */
    stop_watchdog_timer();

    /* Stop volume polling */
    stop_volume_poll();

    /* Stop periodic connection polling */
    stop_connection_polling();

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

    /* Destroy the Whisper client */
    if (g_whisper_client) {
        whisper_client_destroy(g_whisper_client);
        g_whisper_client = NULL;
    }
    curl_global_cleanup();

    /* Destroy the audio recorder */
    if (g_audio_recorder) {
        audio_recorder_destroy(g_audio_recorder);
        g_audio_recorder = NULL;
    }

    /* Cleanup the state controller */
    app_state_controller_cleanup(&g_controller);

    return 0;
}
