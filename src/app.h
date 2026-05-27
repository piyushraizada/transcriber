/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_H
#define APP_H

/******************************************************************************
 * app.h — Central State Controller, Shared Types, and Callbacks
 *
 * This is the primary header for the Transcriber project. It defines:
 *
 *   1. The application finite-state machine (AppState enum).
 *   2. The central configuration structure (AppConfig) shared by all modules.
 *   3. Callback typedefs used to communicate between threads and modules.
 *   4. The central state controller (AppStateController) that coordinates
 *      thread-safe transitions between IDLE, LISTENING, and TRANSCRIBING.
 *
 * Threading Model
 * ===============
 * The application runs three concurrent threads:
 *
 *   • Presentation Thread  — GTK main loop, UI rendering, D-Bus polling.
 *   • Audio Thread         — Real-time PCM capture written to a WAV file.
 *   • Transcription Thread — Local whisper.cpp model processing (offline, no network).
 *
 * All state transitions flow through the AppStateController, which uses a
 * mutex-protected state variable and an atomic sequence counter to prevent
 * race conditions (e.g., watchdog timeout and user stop firing simultaneously).
 *
 * State Machine
 * =============
 *   STATE_IDLE ──► STATE_LISTENING ──► STATE_TRANSCRIBING ──► STATE_IDLE
 *
 *   • IDLE:           Mic icon is red. Audio thread suspended. Transcription idle.
 *   • LISTENING:      Mic icon is green with sine wave animation. Audio thread
 *                     actively capturing PCM into a temporary WAV file. A 30-second
 *                     (configurable) watchdog timer auto-transitions to TRANSCRIBING.
 *   • TRANSCRIBING:   Sine wave stops. Audio file closed. Transcription thread
 *                     runs local whisper.cpp model. On completion, text is displayed,
 *                     clipboard is populated, temp file is deleted, and state returns to IDLE.
 *
 * SRS Traceability
 * =================
 *   Section 2.1 (Threading Model), Section 2.3 (State Machine),
 *   FR-006 (Toggle Behavior), FR-007 (Recording Duration),
 *   FR-034 (Manual Stop), NR-019 (Atomic Sequence Counter).
 *****************************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include "app_config.h"  /* For AppConfig definition */

/******************************************************************************
 * Centralized Error Messages
 *
 * L-002 fix: Shared error strings to avoid duplication across modules.
 *****************************************************************************/
#define APP_ERROR_NO_VALID_MODEL  "No valid whisper ggml file found"

/******************************************************************************
 * Forward Declarations
 *
 * These GTK types are used throughout the project but we avoid including
 * <gtk/gtk.h> here to keep this header lightweight and prevent circular
 * dependencies. Each module that needs GTK types includes <gtk/gtk.h> itself.
 *****************************************************************************/
typedef struct _GtkWindow GtkWindow;
typedef struct _GtkTextView GtkTextView;

/******************************************************************************
 * AppState — Finite State Machine Enumerations
 *
 * Represents the current operational state of the application. Only three
 * valid states exist, forming a unidirectional cycle:
 *
 *   IDLE → LISTENING → TRANSCRIBING → IDLE → ...
 *
 * The state is protected by a mutex in the AppStateController struct.
 * Transitions are initiated by:
 *   • User clicking the microphone icon (Presentation Thread)
 *   • D-Bus Toggle method call (Presentation Thread, via g_unix_fd_add)
 *   • Watchdog timer expiration (Presentation Thread, via g_timeout_add)
 *
 * SRS: Section 2.3, FR-006, FR-007, FR-034
 *****************************************************************************/
typedef enum {
    STATE_IDLE,           /* Microphone idle — red icon, no recording */
    STATE_LISTENING,      /* Actively capturing audio — green icon + sine wave */
    STATE_TRANSCRIBING    /* Running local whisper.cpp model — green icon, no animation */
} AppState;

/******************************************************************************
 * ModelStatus — Local Whisper Model Availability Indicator
 *
 * Tracks the availability of the local Whisper model. Displayed as
 * an 8x8 pixel circle in the MainWindow status bar (right side).
 *
 *   • CONNECTED (green)    — Model file verified and accessible
 *   • DISCONNECTED (red)   — Model file not found or not yet checked
 *   • CHECKING (yellow)    — Model check in progress (blinking)
 *   • LOADING (amber)      — Model is being loaded into GPU/CPU memory
 *
 * The indicator is clickable: clicking while DISCONNECTED triggers a manual
 * model verification check (WHISPER-013). Clicking while CONNECTED is ignored.
 *
 * SRS: UI-021, UI-022, UI-023, WHISPER-013, FR-037
 *****************************************************************************/
typedef enum {
    MODEL_UNAVAILABLE,  /* Red — no connection or check failed */
    MODEL_AVAILABLE,     /* Green — successful connection verified */
    MODEL_CHECKING,      /* Yellow/blinking — check in progress */
    MODEL_LOADING        /* Amber/orange — model being loaded */
} ModelStatus;

/******************************************************************************
 * AppConfig — Central Configuration Structure
 *
 * Holds all user-configurable settings loaded from
 * ~/.config/transcriber/config.json on startup. This struct is the single
 * source of truth for configuration and is shared read-only across all
 * modules after initialization.
 *
 * Fields:
 *   model_path     — Path to the local whisper.cpp model file (.bin/.gguf).
 *                     Default: "ggml-large-v3-turbo-q8_0.bin"
 *                     SRS: CFG-005, WHISPER-001
 *
 *   audio_device   — Named audio capture device or "default".
 *                     Default: "default"
 *                     SRS: CFG-006, CFG-010, AUD-002
 *
 *   max_duration   — Maximum recording duration in seconds (5–30).
 *                     Default: 30
 *                     SRS: CFG-014, FR-024
 *
 *   window_x, window_y — Persisted MainWindow position.
 *                     Default: (100, 100)
 *                     SRS: CFG-004, FR-002
 *
 * Immutability:
 *   After app_config_load() populates this struct, it is treated as
 *   read-only by all worker threads. When the user saves new settings
 *   via the Configuration Dialog, the struct is updated under a mutex
 *   and the new values take effect on the next transcription session.
 *
 * SRS: Section 9 (Configuration Management)
 *****************************************************************************/
/* AppConfig is defined in app_config.h to avoid duplicate typedef.
 * Include app_config.h for the full definition. */

/******************************************************************************
 * Callback Typedefs — Cross-Thread Communication
 *
 * Because the application runs multiple threads (Presentation, Audio, Transcription),
 * direct function calls between threads are unsafe. Instead, we use callback
 * function pointers that are invoked in the correct thread context.
 *
 * All callbacks are designed to be GTK-main-thread-safe, meaning they may
 * safely manipulate GTK widgets.
 *
 * SRS: Section 2.1 (Threading Model)
 *****************************************************************************/

/* transcription_result_callback — Invoked by the Transcription Thread when
 * the local whisper.cpp model completes transcription (success or failure).
 *
 * Parameters:
 *   text       — NULL-terminated UTF-8 transcribed text on success,
 *                or an error message string on failure.
 *   success    — true if transcription succeeded, false on error.
 *   user_data  — Opaque pointer (typically the MainWindow pointer).
 *
 * This callback is marshaled to the Presentation Thread so it can safely
 * update the TextWindow, populate the clipboard, and change the mic icon.
 *
 * SRS: FR-012, FR-014, WHISPER-006, WHISPER-007 */
typedef void (*transcription_result_callback)(const char *text,
                                              bool success,
                                              void *user_data);

/* model_status_callback — Invoked when the model availability check
 * completes (either automatic on startup or manual via clicking
 * the status indicator).
 *
 * Parameters:
 *   status     — The new ModelStatus value.
 *   user_data  — Opaque pointer (typically the MainWindow pointer).
 *
 * Marshaled to the Presentation Thread to update the status bar indicator.
 *
 * SRS: UI-021, UI-022, UI-023, WHISPER-013, FR-037 */
typedef void (*model_status_callback)(ModelStatus status,
                                           void *user_data);

/* state_change_callback — Invoked directly by the state controller when
 * a state transition completes (inside app_transition_to / app_toggle_state).
 *
 * Replaces the polling-based state_monitor_callback pattern with direct
 * callback invocation on transition, eliminating latency and CPU overhead.
 *
 * Parameters:
 *   new_state  — The AppState value after the transition.
 *   user_data  — Opaque pointer passed through from controller init.
 *
 * IMPORTANT: This callback is invoked from whichever thread called
 * app_transition_to() or app_toggle_state(). For GTK-main-thread safety,
 * callers on non-GTK threads should marshal via g_idle_add().
 *
 * SRS: Section 2.1 (Threading Model), Section 2.3 (State Machine) */
typedef void (*state_change_callback)(AppState new_state,
                                       void *user_data);

/******************************************************************************
 * AppStateController — Thread-Safe State Machine
 *
 * This struct is the central coordinator for all state transitions. It
 * contains:
 *
 *   • A mutex protecting the current state variable.
 *   • An atomic sequence counter to prevent duplicate transitions
 *     (e.g., watchdog and user stop both firing at the same time).
 *   • The current AppState and ModelStatus.
 *   • A pointer to the shared AppConfig.
 *   • Callback function pointers for cross-thread communication.
 *
 * Usage:
 *   1. On startup, call app_state_controller_init() to initialize the
 *      mutex, set state to IDLE, and register callbacks.
 *   2. To transition state, call app_transition_to() with the target state.
 *      The controller checks sequence validity and updates atomically.
 *   3. To read the current state, call app_get_state() (thread-safe).
 *   4. On shutdown, call app_state_controller_cleanup() to destroy the mutex.
 *
 * Atomic Sequence Counter:
 *   Each transition increments a monotonically increasing sequence number.
 *   If two threads attempt to transition simultaneously (e.g., watchdog
 *   timeout and user click), only the first increment succeeds. The loser
 *   detects a stale sequence and aborts its transition. This eliminates
 *   the race condition described in SRS Section 2.3.
 *
 * SRS: Section 2.1, Section 2.3, NR-019
 *****************************************************************************/
typedef struct {
    /* Current application state, protected by state_mutex. */
    AppState state;

    /* Current model availability status, protected by state_mutex. */
    ModelStatus model_status;

    /* Mutex for all state transitions. Every read or write of state or
     * model_status must hold this mutex. */
    pthread_mutex_t state_mutex;

    /* Atomic sequence counter for preventing duplicate transitions.
     * Incremented on each successful state change. If a thread reads
     * a sequence number, performs work, and then attempts to transition
     * but finds the sequence has advanced, it knows another thread won
     * the race and aborts. */
    _Atomic int64_t sequence_counter;

    /* Pointer to the shared, read-only configuration.
     * All modules read from this; only app_config.c writes to it. */
    AppConfig *config;

    /* Callback invoked when transcription completes (success or failure).
     * Called from the Transcription Thread but marshaled to the Presentation Thread. */
    transcription_result_callback on_transcription_result;

    /* Callback invoked when connection status changes.
     * Called from the Transcription Thread but marshaled to the Presentation Thread. */
    model_status_callback on_model_status;

    /* Callback invoked directly on state transition.
     * Replaces the polling-based state_monitor_callback pattern.
     * Called from whichever thread invoked app_transition_to/app_toggle_state. */
    state_change_callback on_state_change;

    /* Opaque user data passed to all callbacks.
     * Typically set to the MainWindow GtkWindow pointer. */
    void *callback_user_data;
} AppStateController;

/******************************************************************************
 * Public API — State Controller Lifecycle
 *****************************************************************************/

/* app_state_controller_init — Initialize the state controller.
 *
 * Sets the initial state to STATE_IDLE, connection status to
 * MODEL_UNAVAILABLE, initializes the mutex and sequence counter,
 * and stores the provided config and callback pointers.
 *
 * Parameters:
 *   controller — Pointer to an uninitialized AppStateController struct.
 *   config     — Pointer to the shared AppConfig (read-only after init).
 *   on_transcription_result — Callback for transcription results.
  *   on_model_status    — Callback for connection status changes.
  *   on_state_change         — Callback invoked on each state transition.
  *   user_data               — Opaque pointer passed to all callbacks.
  *
  * Returns: 0 on success, -1 if mutex initialization fails.
  *
  * SRS: Section 2.1, Section 2.3 */
 int app_state_controller_init(AppStateController *controller,
                        AppConfig *config,
                        transcription_result_callback on_transcription_result,
                        model_status_callback on_model_status,
                        state_change_callback on_state_change,
                        void *user_data);

/* app_state_controller_cleanup — Destroy the state controller's mutex.
 *
 * Call this during application shutdown to free the mutex resource.
 * After this call, the controller struct is invalid.
 *
 * Parameters:
 *   controller — Pointer to the initialized AppStateController.
 *
 * SRS: Section 2.3 */
void app_state_controller_cleanup(AppStateController *controller);

/* app_get_state — Thread-safe read of the current application state.
 *
 * Acquires the mutex, reads the state, releases the mutex.
 * Safe to call from any thread.
 *
 * Parameters:
 *   controller — Pointer to the initialized AppStateController.
 *
 * Returns: The current AppState enum value.
 *
 * SRS: Section 2.3 */
AppState app_get_state(AppStateController *controller);

/* app_get_model_status — Thread-safe read of connection status.
 *
 * Parameters:
 *   controller — Pointer to the initialized AppStateController.
 *
 * Returns: The current ModelStatus enum value.
 *
 * SRS: UI-021, UI-022 */
ModelStatus app_get_model_status(AppStateController *controller);

/* app_transition_to — Request a state transition.
 *
 * Attempts to transition the state machine to the target state. The
 * transition is only allowed if:
 *   1. The target state is a valid next state from the current state.
 *   2. No other thread has already advanced the sequence counter.
 *
 * Valid transitions:
 *   IDLE → LISTENING
 *   LISTENING → TRANSCRIBING
 *   TRANSCRIBING → IDLE
 *
 * Parameters:
 *   controller — Pointer to the initialized AppStateController.
 *   target     — The desired target state.
 *
 * Returns: true if the transition was accepted, false if rejected
 *          (invalid transition or lost race condition).
 *
 * SRS: Section 2.3, NR-019 */
bool app_transition_to(AppStateController *controller, AppState target);

/* app_set_model_status — Thread-safe update of connection status.
 *
 * Parameters:
 *   controller — Pointer to the initialized AppStateController.
 *   status     — The new ModelStatus value.
 *
 * SRS: UI-021, UI-022, WHISPER-013 */
void app_set_model_status(AppStateController *controller,
                               ModelStatus status);

/* app_toggle_state — Convenience function to advance the state machine.
 *
 * This is the primary entry point for the microphone click handler and
 * the D-Bus Toggle method. Behavior:
 *   • If current state is IDLE → transition to LISTENING.
 *   • If current state is LISTENING → transition to TRANSCRIBING.
 *   • If current state is TRANSCRIBING → no-op (wait for completion).
 *
 * Parameters:
 *   controller — Pointer to the initialized AppStateController.
 *
 * Returns: true if a transition was initiated, false if no-op.
 *
 * SRS: FR-006, FR-036, HK-002 */
bool app_toggle_state(AppStateController *controller);

#endif /* APP_H */
