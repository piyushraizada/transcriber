/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_CONFIG_DIALOG_H
#define APP_CONFIG_DIALOG_H

/**
 * @file app_config_dialog.h
 * @brief Configuration Dialog UI — Modal settings window with all configurable fields
 *
 * This module implements the Configuration Dialog described in Section 7.3
 * and FR-019 of the SRS. The dialog is a modal GTK3 window that allows the
 * user to modify all configurable application settings:
 *
 *   - Whisper model path (text entry)
 *   - Audio device selection (combo box dropdown)
 *   - Language selection (combo box dropdown — ISO 639-1 codes)
 *   - Maximum recording duration (spin button — 5 to 30 seconds)
 *   - Window position reset (button)
 *   - D-Bus hotkey command display (read-only label)
 *
 * The dialog is launched as a modal window when the user clicks the gear
 * icon button on the MainWindow status bar. The dialog blocks interaction
 * with the main window until the user clicks "Save" or "Cancel".
 *
 * Dialog Behavior:
 *   - Launch: Created on-demand when gear button is clicked (FR-019)
 *   - Modal: Blocks the main window while open
 *   - Save: Validates inputs, updates config, saves to file, destroys dialog
 *   - Cancel: Discards changes, destroys dialog without saving
 *   - Window Position Reset: Resets window position to defaults (-1, -1)
 *
 * @see SRS Section 7.3: Configuration Dialog (FR-019 through FR-028)
 * @see SRS Section 9: Configuration (CFG-001 through CFG-014)
 * @see SRS Section 8: Hotkey Integration (FR-026)
 */

#include <gtk/gtk.h>
#include <stdbool.h>
#include "app_audio.h"  /* For AudioBackend enum */

/* Forward declarations */
struct _AppConfig;
#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Section 1: Config Dialog Handle (Opaque)
 *---------------------------------------------------------------------------
 * Opaque handle to the internal configuration dialog state. The actual
 * structure contains the GTK dialog window, all child widgets (entries,
 * combo boxes, spin buttons, buttons), and the AppConfig struct being
 * edited.
 *
 * Users of this module should NEVER access the internal fields directly.
 * All interaction must go through the public API functions defined below.
 *
 * The dialog is created on-demand and destroyed when closed (Save or
 * Cancel). It is NOT kept hidden and reused — each launch creates a
 * fresh dialog with current config values.
 *
 * @see FR-019: Configuration Dialog — Modal Launch Behavior
 */
typedef struct _ConfigDialog ConfigDialog;

/*---------------------------------------------------------------------------
 * Section 2: Dialog Creation and Display
 *---------------------------------------------------------------------------
 * Functions for creating and showing the configuration dialog.
 */

/**
 * Create and show the configuration dialog (modal).
 *
 * This function creates a new configuration dialog populated with the
 * current configuration values, and displays it as a modal window
 * transient to the main window. The function blocks until the dialog
 * is closed (Save or Cancel).
 *
 * The function performs the following steps:
 *   1. Create a GTK Dialog window with title "Transcriber Settings"
 *   2. Set the dialog as modal and transient to the parent window
 *   3. Populate all widgets with current config values
 *   4. Query available audio devices for the dropdown
 *   5. Display the D-Bus hotkey command in a read-only label
 *   7. Run the dialog modal loop (blocks until Save/Cancel)
 *   8. If Save: Validate inputs, update config, save to file
 *   9. Destroy the dialog window and free resources
 *  10. Return the result
 *
 * The dialog window has the following properties:
 *   - Title: "Transcriber Settings"
 *   - Modal: true (blocks parent window)
 *   - Resizable: false (fixed size)
 *   - Destroy with parent: true
 *   - Default size: ~400x500 pixels (auto-sized by GTK based on content)
 *
 * @param parent_window The parent GTK window (MainWindow). Must not be NULL.
 *                      The dialog is transient to this window.
 * @param config        Pointer to the AppConfig struct to edit. The dialog
 *                      reads current values from this struct and writes
 *                      new values on Save. Must not be NULL.
 * @return true if the user clicked "Save" and configuration was updated,
 *         false if the user clicked "Cancel" or the dialog was closed
 *         via window manager (X button).
 *
 * @pre parent_window != NULL && config != NULL
 *
 * @note This function BLOCKS until the dialog is closed. It should be
 *       called from the GTK main thread (presentation thread).
 *
 * @see FR-019: Configuration Dialog — Modal Launch Behavior
 * @see FR-027: Save Configuration
 * @see FR-028: Cancel Configuration
 */
bool config_dialog_show(GtkWindow* parent_window, struct _AppConfig* config);

/*---------------------------------------------------------------------------
 * Section 3: Audio Device Population
 *---------------------------------------------------------------------------
 * Functions for populating the audio device dropdown with available devices.
 */

/**
 * Get the list of available audio devices for the config dialog dropdown.
 *
 * This function queries the audio backend for a list of available input
 * devices (microphones) and returns them as a GtkListStore suitable for
 * use with a GtkComboBox. The list includes a special "Default" entry
 * at the top, followed by the detected devices.
 *
 * The GtkListStore has a single column of type G_TYPE_STRING containing
 * the device names. The caller takes ownership of the GtkListStore and
 * is responsible for_unref()ing it.
 *
 * @param backend The AudioBackend to query. If AUDIO_BACKEND_NONE, returns
 *                an empty list.
 * @return A newly referenced GtkListStore* containing device names, or
 *         NULL on error. The caller must call g_object_unref() when done.
 *
 * @see FR-020: Microphone Selection
 * @see AUD-008a: Microphone Selection Support
 */
GtkListStore* config_dialog_get_audio_devices(AudioBackend backend);

/* MIN-002 fix: Removed Section 4 (Language List) — language selection removed. */

/*---------------------------------------------------------------------------
 * Section 5: D-Bus Hotkey Display
 *---------------------------------------------------------------------------
 * Functions for displaying the D-Bus hotkey command in the dialog.
 */

/**
 * Get the D-Bus hotkey command string for display in the config dialog.
 *
 * This function returns the dbus-send command that the user can run from
 * the terminal to toggle the microphone via the D-Bus interface. The
 * command is displayed in a read-only label in the configuration dialog.
 *
 * The returned string is formatted as:
 *   dbus-send --session --dest=org.xvoice.Controller \
 *             --type=method_call /org/xvoice/App \
 *             org.xvoice.Actions.Toggle
 *
 * The string is wrapped in a GtkLabel with monospace font for easy
 * copy-pasting.
 *
 * @return A statically allocated string containing the dbus-send command.
 *         The caller must NOT free the returned string.
 *
 * @see FR-026: Hotkey Command Display
 * @see SRS Section 8.2: D-Bus Interface Specification
 */
const char* config_dialog_get_hotkey_command(void);

/*---------------------------------------------------------------------------
 * Section 7: Input Validation
 *---------------------------------------------------------------------------
 * Functions for validating user input in the dialog fields.
 */

/**
 * Validate the Whisper model path field.
 *
 * @param path The model path string to validate.
 * @return true if valid (non-empty, length < 512),
 *         false otherwise.
 */
bool config_dialog_validate_model_path(const char* path);

/**
 * Validate that a path points to a valid Whisper GGUF model file.
 *
 * This function checks that the file exists, is a regular file, and
 * can be loaded as a valid Whisper GGUF model by attempting to read
 * its metadata.
 *
 * @param path The full path to the model file to validate.
 * @return true if the file exists and is a valid Whisper GGUF model,
 *         false otherwise.
 *
 * @note This function performs a synchronous model metadata load and
 *       should NOT be called from the GTK main thread for large models.
 *       Use this for quick validation (e.g., before starting recording).
 */
bool config_dialog_validate_gguf_model(const char* path);

/**
 * Get the default model file path (~/.cache/whisper/ggml-large-v3-turbo-q8_0.bin).
 *
 * @return A statically allocated string (do NOT free).
 */
const char* config_dialog_get_default_model_path(void);

/**
 * Check if the default model file exists.
 *
 * @return true if the default model file exists and is a regular file.
 */
bool config_dialog_default_model_exists(void);

/**
 * Validate the max duration field.
 *
 * @param duration The duration in seconds.
 * @return true if valid (5 <= duration <= 30), false otherwise.
 */
bool config_dialog_validate_duration(int duration);

/**
 * Show a validation error message in the dialog.
 *
 * This function displays an error message below the invalid field using
 * a GtkLabel with red text. The error message is cleared when the user
 * starts typing in the field.
 *
 * @param error_label Pointer to the GtkLabel widget for error display.
 * @param message     The error message to display.
 */
void config_dialog_show_error(GtkLabel* error_label, const char* message);

/**
 * Clear a validation error message.
 *
 * @param error_label Pointer to the GtkLabel widget for error display.
 */
void config_dialog_clear_error(GtkLabel* error_label);

/*---------------------------------------------------------------------------
 * Section 8: Window Position Reset
 *---------------------------------------------------------------------------
 * Function for the "Reset Window Position" button.
 */

/**
 * Reset the window position in the config struct to defaults.
 *
 * This function sets the window_x and window_y fields in the config
 * struct to -1, which indicates the window should be centered on screen
 * on the next launch.
 *
 * This is called when the user clicks the "Reset Window Position" button
 * in the configuration dialog.
 *
 * @param config Pointer to the AppConfig struct to update. Must not be NULL.
 *
 * @see FR-025: Window Position Reset
 * @see CFG-004: Window Position
 */
void config_dialog_reset_window_position(struct _AppConfig* config);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_DIALOG_H */
