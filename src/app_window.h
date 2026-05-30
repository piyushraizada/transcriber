/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_WINDOW_H
#define APP_WINDOW_H

/******************************************************************************
 * app_window.h — MainWindow, TextWindow, Sine Wave Animation, Countdown Timer,
 *                Status Bar, Config Callback, TextWindow Screen Boundary Awareness
 *
 * This module implements all GTK3 UI components for the Transcriber application:
 *
 *   1. MainWindow       — The primary application window hosting the microphone
 *                          icon, sine wave animation overlay, and status bar.
 *   2. TextWindow       — A transient utility window for displaying and editing
 *                          transcribed text. Positioned below MainWindow by
 *                          default; if it would extend past the bottom of the
 *                          display monitor, it is positioned above instead.
 *   3. Sine Wave Animation — Cairo-driven animation rendered on a GtkDrawingArea
 *                          overlay during STATE_LISTENING only. Rendered in
 *                          blue (RGB 0,0,1) with a line width of 6.0 pixels.
 *                          Hidden during STATE_IDLE and STATE_TRANSCRIBING.
 *   4. Countdown Timer  — GtkLabel centered in the status bar that counts down
 *                          from max_duration to 0 during STATE_LISTENING.
 *                          Hidden during all other states.
 *   5. Status Bar       — A 16-pixel bar at the bottom of MainWindow containing
 *                          the gear/settings button (left), countdown timer
 *                          (center), and connection indicator (right).
 *   6. Config Callback  — Invoked when the Configuration Dialog saves changes,
 *                          allowing the main application to apply runtime config
 *                          updates (e.g., audio device selection).
 *
 * Window Architecture
 * ===================
 *
 * MainWindow Layout (vertical GtkBox):
 *   ┌────────────────────────────────┐
 *   │    GtkDrawingArea (icon)       │  ← Microphone icon rendered here
 *   │    GtkDrawingArea (animation)  │  ← Sine wave overlay (transparent bg)
 *   └────────────────────────────────┘
 *   ┌────────────────────────────────┐
 *   │ [⚙]        [30]          [●] │  ← Status bar (16px height)
 *   └────────────────────────────────┘
 *
 *   • The microphone icon is rendered via GdkPixbuf from embedded XPM data
 *     (redmic.xpm or greenmic.xpm) onto the icon drawing area.
 *   • The sine wave is drawn on a separate drawing area stacked on top,
 *     with a transparent background so the mic icon shows through.
 *     Color: blue (RGB 0,0,1), line width: 6.0 pixels.
 *   • The status bar is a fixed 16-pixel GtkBox with:
 *     - Left:  GtkButton with gear.xpm icon → opens Config Dialog
 *     - Center: GtkLabel countdown timer (hidden when not recording)
 *     - Right: GtkDrawingArea with 8x8 colored circle (connection status)
 *
 * MainWindow Properties:
 *   • Created via gtk_window_new(GTK_WINDOW_TOPLEVEL)
 *   • Fixed size: width = XPM width, height = XPM height + 16 (status bar)
 *   • Non-resizable: gtk_window_set_resizable(window, FALSE)
 *   • Type hint: GDK_WINDOW_TYPE_HINT_UTILITY
 *   • Title: "Transcriber"
 *
 * TextWindow Properties:
 *   • Independent GTK window (GTK_WINDOW_TOPLEVEL)
 *   • Transient for MainWindow: gtk_window_set_transient_for()
 *   • Type hint: GDK_WINDOW_TYPE_HINT_UTILITY
 *   • Resizable: TRUE
 *   • Contains a GtkTextView for full text editing (Ctrl+C/V/A/X, undo, etc.)
 *   • Positioned 10px below MainWindow via gtk_window_move()
 *   • Screen boundary awareness: if below position would extend past the
 *     bottom of the display monitor, positioned above MainWindow instead.
 *   • "Close" (X) hides the window (gtk_widget_hide), preserving content
 *   • Re-shown when new transcription text arrives
 *
 * Sine Wave Animation:
 *   • Rendered on a GtkDrawingArea using Cairo context.
 *   • Timer-driven at 30fps via g_timeout_add(33, ...).
 *   • Draws a continuous sine wave that scrolls left to right.
 *   • Transparent background (no clear/fill before drawing the wave).
 *   • Color: blue (RGB 0,0,1), line width: 6.0 pixels.
 *   • Visible only during STATE_LISTENING (hidden via gtk_widget_hide).
 *
 * Countdown Timer:
 *   • GtkLabel centered in the status bar between expanders.
 *   • Counts down from max_duration (config) to 0, updating every 1 second.
 *   • Visible only during STATE_LISTENING.
 *   • Resets to full max_duration on each recording start.
 *
 * Config Callback:
 *   • Registered via app_window_set_config_changed_callback().
 *   • Invoked when the Configuration Dialog saves changes (GTK_RESPONSE_OK).
 *   • Allows the main application to apply runtime config updates,
 *     e.g., updating the audio recorder device without restart.
 *
 * SRS Traceability
 * =================
 *   Section 2.4.1 (MainWindow GTK3 Design),
 *   Section 2.4.2 (TextWindow GTK3 Design),
 *   UI-001 through UI-027 (all UI requirements),
 *   FR-001 through FR-014, FR-018 through FR-019,
 *   FR-038 (Countdown Timer), FR-039 (Recording Beep),
 *   FR-040 (Runtime Config), FR-042 (TextWindow Screen Boundaries).
 *****************************************************************************/

#include <gtk/gtk.h>
#include <cairo.h>
#include "app.h"
#include "app_whisper.h"

/******************************************************************************
 * Forward Declarations
 *****************************************************************************/
struct _AppStateController;

/******************************************************************************
 * MainWindow — Primary Application Window Opaque Handle
 *
 * This struct encapsulates all GTK widgets and state needed for the main
 * application window. It is allocated and initialized by
 * app_window_create() and destroyed by app_window_destroy().
 *
 * The struct is intentionally opaque — callers interact with it only via
 * the public API functions declared below. This encapsulation ensures that
 * widget lifecycle and signal handlers are managed correctly.
 *
 * Key Internal Widgets:
 *   window            — The GtkWindow (TOPLEVEL, UTILITY hint).
 *   main_box          — Vertical GtkBox containing icon area and status bar.
 *   icon_area         — GtkDrawingArea where the mic icon is rendered.
 *   animation_area    — GtkDrawingArea for the sine wave overlay.
 *   status_bar        — 16px GtkBox (gear button left, indicator right).
 *   gear_button       — GtkButton with gear.xpm → opens Config Dialog.
 *   indicator_area    — GtkDrawingArea for the 8x8 connection status circle.
 *
 * Animation State:
 *   animation_source_id — GLib timeout source ID (0 = not running).
 *   wave_phase          — Current phase offset of the sine wave (radians).
 *   current_icon        — Pointer to embedded XPM array currently displayed.
 *
 * SRS: Section 2.4.1, UI-001, UI-002, UI-018
 *****************************************************************************/
typedef struct _MainWindow MainWindow;

/******************************************************************************
 * TextWindow — Transcription Text Display Opaque Handle
 *
 * This struct encapsulates the transient text window and its GtkTextView.
 * It is allocated and initialized by app_text_window_create().
 *
 * Key Internal Widgets:
 *   window    — The GtkWindow (TOPLEVEL, UTILITY hint, transient for MainWindow).
 *   text_view — GtkTextView providing full editing capabilities.
 *   buffer    — GtkTextBuffer holding the transcribed text.
 *
 * Lifecycle:
 *   • Created once on first transcription result.
 *   • "delete-event" hides the window (does NOT destroy it).
 *   • When new transcription arrives, the text buffer is appended and
 *     the window is shown and raised.
 *   • Destroyed only during application shutdown.
 *
 * SRS: Section 2.4.2, UI-006 through UI-010, UI-016, UI-017
 *****************************************************************************/
typedef struct _TextWindow TextWindow;

/******************************************************************************
 * Public API — MainWindow Lifecycle
 *****************************************************************************/

/* app_window_create — Create and initialize the MainWindow.
 *
 * Constructs the entire MainWindow widget hierarchy:
 *   1. Creates the GtkWindow with UTILITY type hint and fixed size.
 *   2. Creates the icon drawing area sized to the XPM dimensions.
 *   3. Creates the animation drawing area (same size, transparent).
 *   4. Creates the 16px status bar with gear button and indicator.
 *   5. Packs everything into a vertical GtkBox.
 *   6. Connects signal handlers:
 *      - button-press-event on icon area → toggle recording
 *      - clicked on gear button → open config dialog
 *      - button-press-event on indicator → manual connection check
 *      - delete-event on window → save position and quit
 *   7. Renders the initial redmic.xpm icon.
 *   8. Positions the window from config (or defaults to 100,100).
 *
 * The window is NOT shown automatically — call gtk_widget_show() or
 * gtk_window_present() after creation.
 *
 * Parameters:
 *   config     — Pointer to the shared AppConfig for window position.
 *   controller — Pointer to the AppStateController for state queries.
 *
 * Returns: A newly allocated MainWindow handle, or NULL on failure.
 *
 * SRS: Section 2.4.1, FR-001, FR-002, UI-001, UI-002, UI-018
 */
MainWindow *app_window_create(AppConfig *config,
                               AppStateController *controller,
                               WhisperClient *whisper_client);

/* app_window_destroy — Destroy the MainWindow and free all resources.
 *
 * Stops the sine wave animation if running, disconnects signal handlers,
 * and frees the MainWindow struct. The underlying GtkWindow is also
 * destroyed (gtk_widget_destroy).
 *
 * Parameters:
 *   win — The MainWindow handle returned by app_window_create().
 *
 * SRS: DEPLOY-004 (Application Launch lifecycle)
 */
void app_window_destroy(MainWindow *win);

/* app_window_get_gtk_window — Obtain the underlying GtkWindow pointer.
 *
 * Returns the GtkWindow * for use with GTK functions such as
 * gtk_window_present(), gtk_window_get_screen(), etc.
 *
 * Parameters:
 *   win — The MainWindow handle.
 *
 * Returns: The GtkWindow pointer (never NULL for a valid MainWindow).
 */
GtkWindow *app_window_get_gtk_window(MainWindow *win);

/******************************************************************************
 * Public API — MainWindow State Updates
 *
 * These functions are called by the state controller (or callbacks from
 * the Audio/Transcription threads) to update the UI in response to state changes.
 * All functions are designed to be called from the Presentation Thread
 * (GTK main context).
 *****************************************************************************/

/* app_window_set_state — Update the MainWindow UI for a new application state.
 *
 * Changes the microphone icon and sine wave animation based on state:
 *   • STATE_IDLE:         Show redmic.xpm, stop animation.
 *   • STATE_LISTENING:    Show greenmic.xpm, start animation.
 *   • STATE_TRANSCRIBING: Show greenmic.xpm, stop animation.
 *
 * The icon is rendered by loading the appropriate embedded XPM array into
 * a GdkPixbuf and queueing a redraw on the icon drawing area.
 *
 * Parameters:
 *   win    — The MainWindow handle.
 *   state  — The new AppState value.
 *
 * SRS: FR-005, FR-006, FR-010, FR-011, UI-001, UI-003, UI-004, UI-005
 */
void app_window_set_state(MainWindow *win, AppState state);

/* app_window_set_model_status — Update the connection status indicator.
 *
 * Changes the color of the 8x8 circle in the status bar:
 *   • MODEL_UNAVAILABLE → Red (#FF0000)
 *   • MODEL_AVAILABLE    → Green (#00FF00)
 *   • MODEL_CHECKING     → Yellow (#FFFF00, blinking via timer)
 *
 * Queues a redraw on the indicator drawing area.
 *
 * Parameters:
 *   win    — The MainWindow handle.
 *   status — The new ModelStatus value.
 *
 * SRS: UI-021, UI-022, FR-030, FR-037
 */
void app_window_set_model_status(MainWindow *win,
                                      ModelStatus status);

/* app_window_start_animation — Start the sine wave animation.
 *
 * Creates a GLib timeout source (33ms ≈ 30fps) that increments the
 * wave phase and queues a redraw on the animation drawing area.
 * The animation is a continuous sine wave drawn with Cairo:
 *   - Transparent background (no clear before drawing).
 *   - Medium line width (2.0 pixels).
 *   - Color: white with slight transparency (alpha 0.7).
 *
 * If an animation is already running, this is a no-op.
 *
 * Parameters:
 *   win — The MainWindow handle.
 *
 * SRS: FR-010, UI-004, UI-019, UI-020
 */
void app_window_start_animation(MainWindow *win);

/* app_window_stop_animation — Stop the sine wave animation.
 *
 * Destroys the GLib timeout source and clears the animation drawing area.
 * If no animation is running, this is a no-op.
 *
 * Parameters:
 *   win — The MainWindow handle.
 *
 * SRS: FR-011, FR-011a, UI-005
 */
void app_window_stop_animation(MainWindow *win);

/* app_window_start_countdown — Start the recording countdown timer.
 *
 * Reads max_duration from the AppConfig and starts a 1-second interval
 * timer that decrements the displayed count. The label is shown in the
 * status bar between the gear button and the connection indicator.
 *
 * Parameters:
 *   win — The MainWindow handle.
 */
void app_window_start_countdown(MainWindow *win);

/* app_window_stop_countdown — Stop the recording countdown timer.
 *
 * Stops the timer and hides the countdown label.
 *
 * Parameters:
 *   win — The MainWindow handle.
 */
void app_window_stop_countdown(MainWindow *win);

/* app_window_save_position — Save the current window position to config.
 *
 * Reads the current window position via gtk_window_get_position() and
 * writes the coordinates to the AppConfig struct. The config is then
 * persisted to disk by the config module.
 *
 * Typically called on window close (delete-event handler).
 *
 * Parameters:
 *   win — The MainWindow handle.
 *
 * SRS: FR-002, CFG-004
 */
void app_window_save_position(MainWindow *win);

/* app_window_set_toggle_callback — Register a callback for mic icon clicks.
 *
 * When the user clicks the microphone icon, the provided callback is invoked
 * with the user_data argument. This allows the main application to handle
 * the toggle (start/stop recording, update UI, etc.).
 *
 * Parameters:
 *   win       — The MainWindow handle.
 *   callback  — Function to call on mic icon click.
 *   user_data — Opaque pointer passed to the callback.
 */
void app_window_set_toggle_callback(MainWindow *win,
                                    void (*callback)(void *user_data),
                                    void *user_data);

/* app_window_set_config_changed_callback — Register a callback for config saves.
 *
 * When the user saves the configuration dialog, the provided callback is invoked
 * with the user_data argument. This allows the main application to apply runtime
 * config changes (e.g., update the audio recorder device).
 *
 * Parameters:
 *   win       — The MainWindow handle.
 *   callback  — Function to call when config is saved.
 *   user_data — Opaque pointer passed to the callback.
 */
void app_window_set_config_changed_callback(MainWindow *win,
                                            void (*callback)(void *user_data),
                                            void *user_data);

/******************************************************************************
 * Public API — TextWindow Lifecycle
 *****************************************************************************/

/* app_text_window_create — Create and initialize the TextWindow.
 *
 * Constructs the TextWindow as a transient utility window:
 *   1. Creates GtkWindow (TOPLEVEL, resizable, UTILITY hint).
 *   2. Sets transient parent to the provided MainWindow's GtkWindow.
 *   3. Creates a GtkTextView with:
 *      - Line wrapping enabled (GTK_WRAP_WORD_CHAR).
 *      - Editable (allows in-place editing of transcribed text).
 *      - Undo support enabled.
 *      - Vertical scrollbar for long text.
 *   4. Connects "delete-event" to hide (not destroy) the window.
 *   5. Positions the window 10px below the MainWindow.
 *
 * The window is created in a hidden state — it is shown only when
 * new transcription text arrives via app_text_window_append_text().
 *
 * Parameters:
 *   main_window_gtk — The GtkWindow * of the MainWindow (parent).
 *
 * Returns: A newly allocated TextWindow handle, or NULL on failure.
 *
 * SRS: Section 2.4.2, UI-006, UI-007, UI-016, UI-017
 */
TextWindow *app_text_window_create(GtkWindow *main_window_gtk);

/* app_text_window_destroy — Destroy the TextWindow and free all resources.
 *
 * Destroys the GtkWindow, GtkTextView, and GtkTextBuffer, then frees
 * the TextWindow struct. Call this during application shutdown.
 *
 * Parameters:
 *   tw — The TextWindow handle returned by app_text_window_create().
 *
 * SRS: DEPLOY-004 (Application Launch lifecycle)
 */
void app_text_window_destroy(TextWindow *tw);

/* app_text_window_append_text — Append transcribed text and show the window.
 *
 * Appends the provided text to the GtkTextBuffer, followed by a newline.
 * If the window is currently hidden, it is shown and raised.
 * The view is scrolled to the end so the latest text is visible.
 *
 * This function is the primary entry point for displaying transcription
 * results. It is called from the transcription_result_callback, which
 * is marshaled to the Presentation Thread.
 *
 * Parameters:
 *   tw    — The TextWindow handle.
 *   text  — NULL-terminated UTF-8 text to append.
 *
 * SRS: FR-012, FR-014, FR-033, UI-006, UI-008
 */
void app_text_window_append_text(TextWindow *tw, const char *text);

/* app_text_window_set_error — Display an error message in the TextWindow.
 *
 * Similar to append_text but prefixes the message with "ERROR: " and
 * uses a red text tag if available. Shows and raises the window.
 *
 * Parameters:
 *   tw     — The TextWindow handle.
 *   error  — NULL-terminated UTF-8 error message.
 *
 * SRS: ERR-003, ERR-005, ERR-010
 */
void app_text_window_set_error(TextWindow *tw, const char *error);

/* app_text_window_clear_text — Clear all text from the TextWindow buffer.
 *
 * Removes all content from the GtkTextBuffer to prevent unbounded memory
 * growth over many transcription sessions. Shows and raises the window.
 *
 * Parameters:
 *   tw — The TextWindow handle.
 *
 * SRS: Memory management best practice.
 */
void app_text_window_clear_text(TextWindow *tw);

/* MIN-001 fix: Removed unused app_text_window_get_text() and app_text_window_is_visible(). */

/* app_window_set_volume_level — Update the volume level bar.
 *
 * Sets the current value of the volume level bar to the provided level
 * (0.0 to 1.0). The bar uses GTK's "success" markup when the level is
 * in a good range, "warning" when too low, and "alert" when clipping.
 *
 * Parameters:
 *   win    — The MainWindow handle.
 *   level  — Volume level in range [0.0, 1.0].
 */
void app_window_set_volume_level(MainWindow *win, double level);

/* MED-9 fix: Get/set the last volume level for threshold-based updates.
 * These are used by the volume poll callback in main.c to avoid
 * unnecessary GTK widget redraws. */
double app_window_get_last_volume_level(MainWindow *win);
void app_window_set_last_volume_level(MainWindow *win, double level);

/* app_window_set_model_loading — Update the model loading indicator.
 *
 * When model_loading is TRUE, the icon draw callback overlays "WAIT" text
 * in bold black on top of the microphone icon to inform the user that
 * the system is not yet ready for dictation.
 *
 * When model_loading is set to FALSE, the "WAIT" overlay is removed and
 * the icon displays normally. This should be called once the Whisper model
 * has finished loading at startup.
 *
 * Parameters:
 *   win          — The MainWindow handle.
 *   model_loading — TRUE to show "WAIT" overlay, FALSE to remove it.
 */
void app_window_set_model_loading(MainWindow *win, bool model_loading);

/* app_window_get_model_loading — Query the model loading indicator.
 *
 * Returns TRUE if the "WAIT" overlay is currently being displayed,
 * FALSE if the model has finished loading (or failed to load).
 *
 * Parameters:
 *   win — The MainWindow handle.
 *
 * Returns: TRUE if model is still loading, FALSE otherwise.
 */
bool app_window_get_model_loading(MainWindow *win);

#endif /* APP_WINDOW_H */
