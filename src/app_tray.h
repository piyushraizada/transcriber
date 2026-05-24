/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_TRAY_H
#define APP_TRAY_H

/**
 * @file app_tray.h
 * @brief System Tray Icon — libappindicator integration for notification area presence
 *
 * This module implements a system tray (notification area) icon using
 * libappindicator-gtk3, providing the following features:
 *
 *   - State-aware icon: Red mic (IDLE) / Green mic (LISTENING/TRANSCRIBING)
 *   - Dynamic tooltip: Updates with current state and connection status
 *   - Context menu: Right-click menu with Toggle Recording, Show Window, Quit
 *
 * The tray icon is created once at application startup and destroyed at
 * shutdown. State updates are pushed from main.c after each state transition.
 *
 * Library: libayatana-appindicator3-0.1 (Ayatana fork, preferred)
 *          or libappindicator3-0.1 (original, fallback)
 * Protocol: StatusNotifierItem (freedesktop.org specification)
 *
 * @see plans/system-tray-icon.md for full design documentation
 */

#include <gtk/gtk.h>
#include <stdbool.h>
#include "app.h"  /* For AppState and ConnectionStatus enums */

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Section 1: SystemTray Handle (Opaque)
 *---------------------------------------------------------------------------
 * Opaque handle to the internal tray state. Contains the AppIndicator,
 * GtkMenu, temporary icon file paths, and callback pointers.
 *
 * Users of this module should NEVER access the internal fields directly.
 * All interaction must go through the public API functions defined below.
 */
typedef struct _SystemTray SystemTray;

/*---------------------------------------------------------------------------
 * Section 2: Lifecycle
 *---------------------------------------------------------------------------
 * Functions for creating and destroying the system tray icon.
 */

/**
 * Create and initialize the system tray icon.
 *
 * This function creates an AppIndicator with ID "transcriber", sets the
 * initial icon to the red microphone (IDLE state), and creates the
 * right-click context menu with the following items:
 *   - "Toggle Recording" — triggers the recording toggle callback
 *   - "Show Window"      — presents the main application window
 *   - "Quit"             — calls gtk_main_quit()
 *
 * The icon is initially set to "redmic" state with tooltip "Transcriber".
 * Call tray_set_state() to update the icon after creation.
 *
 * @return A valid SystemTray* on success, or NULL on failure
 *         (e.g., libappindicator not available).
 *         The caller is responsible for calling tray_destroy() when done.
 */
SystemTray *tray_create(void);

/**
 * Destroy the system tray icon and free all resources.
 *
 * This function cleans up the AppIndicator, destroys the GtkMenu,
 * and removes any temporary icon files created during operation.
 *
 * @param tray Pointer to a valid SystemTray. Must not be NULL.
 */
void tray_destroy(SystemTray *tray);

/*---------------------------------------------------------------------------
 * Section 3: State Updates
 *---------------------------------------------------------------------------
 * Functions for updating the tray icon to reflect application state changes.
 * These functions are designed to be called from the GTK main thread.
 */

/**
 * Update the tray icon and tooltip based on application state.
 *
 * Changes the icon and tooltip as follows:
 *   - STATE_IDLE:         Red mic icon, tooltip "Transcriber — Ready"
 *   - STATE_LISTENING:    Green mic icon, tooltip "Transcriber — Recording..."
 *   - STATE_TRANSCRIBING: Green mic icon, tooltip "Transcriber — Transcribing..."
 *
 * The icon is loaded from the embedded XPM arrays (same source as MainWindow)
 * and saved as a temporary PNG file for AppIndicator consumption.
 *
 * @param tray  Pointer to a valid SystemTray. Must not be NULL.
 * @param state The new AppState value.
 */
void tray_set_state(SystemTray *tray, AppState state);

/**
 * Update the tray tooltip to reflect connection status.
 *
 * When in IDLE state and the connection is disconnected, the tooltip
 * is updated to "Transcriber — Model unavailable" to alert the user.
 * When connected, the tooltip shows "Transcriber — Ready".
 *
 * This function only modifies the tooltip — it does not change the icon.
 * Call tray_set_state() for icon changes.
 *
 * @param tray   Pointer to a valid SystemTray. Must not be NULL.
 * @param status The new ConnectionStatus value.
 */
void tray_set_connection_status(SystemTray *tray, ConnectionStatus status);

/*---------------------------------------------------------------------------
 * Section 4: Window Integration
 *---------------------------------------------------------------------------
 * Functions for linking the tray icon to the main application window.
 */

/**
 * Set the main window reference for the "Show Window" menu action.
 *
 * The "Show Window" context menu item will call gtk_window_present()
 * on the provided window when activated.
 *
 * @param tray  Pointer to a valid SystemTray. Must not be NULL.
 * @param win   The GtkWindow to present when "Show Window" is clicked.
 */
void tray_set_main_window(SystemTray *tray, GtkWindow *win);

/**
 * Set the toggle callback for the "Toggle Recording" menu action.
 *
 * When the user selects "Toggle Recording" from the context menu,
 * the provided callback is invoked with the user_data argument.
 *
 * @param tray       Pointer to a valid SystemTray. Must not be NULL.
 * @param callback   Function to call when "Toggle Recording" is activated.
 * @param user_data  Opaque pointer passed to the callback.
 */
void tray_set_toggle_callback(SystemTray *tray,
                              void (*callback)(void *user_data),
                              void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* APP_TRAY_H */
