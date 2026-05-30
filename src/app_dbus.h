/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_DBUS_H
#define APP_DBUS_H

/**
 * @file app_dbus.h
 * @brief D-Bus Interface — org.xvoice.Controller service for global hotkey toggle
 *
 * This module implements the D-Bus integration described in Section 8 of
 * the SRS. It provides a D-Bus service that allows external applications
 * (such as desktop environment hotkey configurators) to toggle the
 * microphone recording state via a D-Bus method call.
 *
 * The D-Bus service specification:
 *   - Bus Name: org.xvoice.Controller (session bus)
 *   - Object Path: /org/xvoice/App
 *   - Interface: org.xvoice.Actions
 *   - Method: Toggle (no parameters, no return value)
 *
 * When the Toggle method is called, the application performs
 * the same action as clicking the microphone icon:
 *   - If IDLE: Start recording (transition to LISTENING state)
 *   - If LISTENING: Stop recording and start transcription (transition to TRANSCRIBING)
 *   - If TRANSCRIBING: Wait for transcription to complete, then return to IDLE
 *
 * The D-Bus module operates as follows:
 *   1. On startup: Request the bus name "org.xvoice.Controller" on the
 *      session bus. If another instance already owns the name, the new
 *      instance exits (single-instance enforcement).
 *   2. Register the Toggle method handler.
 *   3. Monitor the D-Bus connection for incoming method calls.
 *   4. On method call: Invoke the toggle callback in the GTK main thread.
 *   5. On shutdown: Release the bus name and close the connection.
 *
 * The module uses GDBus (GIO) for D-Bus operations. GDBus integrates
 * natively with GLib's main loop, allowing D-Bus messages to be processed
 * without a separate thread or polling timer.
 *
 * @see SRS Section 8: Hotkey Integration (HK-001 through HK-005)
 * @see SRS Section 2.5: Hotkey Event Flow — D-Bus Integration
 * @see FR-036: OS-Configured Hotkey Toggle
 * @see NR-020: X11 and Wayland Hotkey Support
 */

#include <stdbool.h>

/* Forward declarations — avoid circular dependencies */
struct _AppStateController;

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Section 1: D-Bus Service Constants
 *---------------------------------------------------------------------------
 * D-Bus service identification constants. These define the bus name,
 * object path, interface name, and method name for the D-Bus service.
 *
 * These constants match the D-Bus interface specification in SRS Section 8.2.
 */

/** D-Bus well-known bus name requested by the application. */
#define DBUS_BUS_NAME        "org.xvoice.Controller"

/** D-Bus object path exported by the application. */
#define DBUS_OBJECT_PATH     "/org/xvoice/App"

/** D-Bus interface name. */
#define DBUS_INTERFACE       "org.xvoice.Actions"

/** D-Bus method name for toggling the microphone. */
#define DBUS_METHOD_TOGGLE   "Toggle"

/** GNOME Shell D-Bus interface for dock/dash integration. */
#define DBUS_SHELL_INTERFACE "org.gnome.Shell.Application"

/** GNOME Shell Activate method name. */
#define DBUS_METHOD_ACTIVATE "Activate"

/*---------------------------------------------------------------------------
 * Section 2: D-Bus Service Handle (Opaque)
 *---------------------------------------------------------------------------
 * Opaque handle to the internal D-Bus service state. The actual structure
 * contains the GDBus introspection data, the bus name owner ID from
 * g_bus_own_name(), and the toggle callback function pointer.
 *
 * Users of this module should NEVER access the internal fields directly.
 * All interaction must go through the public API functions defined below.
 *
 * The D-Bus service is initialized once at application startup and
 * cleaned up at application exit. The service runs in the GTK main
 * thread (presentation thread) via GDBus's native GLib main loop
 * integration.
 *
 * @see SRS Section 2.1: Threading Model — Presentation Thread handles D-Bus
 */
typedef struct _DBusService DBusService;

/*---------------------------------------------------------------------------
 * Section 3: Toggle Callback Typedef
 *---------------------------------------------------------------------------
 * Callback function type invoked when the Toggle D-Bus method
 * is called by an external client.
 */

/**
 * Callback function invoked when the Toggle D-Bus method is called.
 *
 * This callback is invoked in the GTK main thread when an external client
 * (e.g., dbus-send command triggered by a desktop environment hotkey) calls
 * the Toggle method. The callback should perform the same action
 * as clicking the microphone icon in the UI.
 *
 * @param user_data User-provided data pointer (set during dbus_service_start()).
 */
typedef void (*dbus_toggle_callback)(void* user_data);

/**
 * Callback function invoked when the GNOME Shell Activate D-Bus method is called.
 *
 * This callback is invoked in the GTK main thread when GNOME Shell (or another
 * desktop environment) calls org.gnome.Shell.Application.Activate, typically
 * when the user clicks the application icon in the dock/dash. The callback
 * should present or raise the application window.
 *
 * @param user_data User-provided data pointer (set during dbus_service_start()).
 */
typedef void (*dbus_activate_callback)(void* user_data);

/*---------------------------------------------------------------------------
 * Section 4: Service Initialization and Lifecycle
 *---------------------------------------------------------------------------
 * Functions for starting and stopping the D-Bus service.
 */

/**
 * Create and initialize a D-Bus service instance.
 *
 * This function allocates and initializes the DBusService struct. The
 * service is NOT yet active — you must call dbus_service_start() to
 * request the bus name and register the method handler.
 *
 * @return A valid DBusService* on success, or NULL on allocation failure.
 *         The caller is responsible for calling dbus_service_destroy()
 *         when finished.
 */
DBusService* dbus_service_create(void);

/**
 * Destroy a D-Bus service and release all resources.
 *
 * This function performs the following cleanup steps:
 *   1. Stop the D-Bus service (if running) — release bus name via g_bus_unown_name()
 *   2. Free the GDBus introspection data
 *   3. Free the DBusService struct
 *
 * @param service Pointer to a valid DBusService. Must not be NULL.
 */
void dbus_service_destroy(DBusService* service);

/**
 * Start the D-Bus service and request the bus name.
 *
 * This function activates the D-Bus service by performing the following
 * steps:
 *   1. Check if another instance already owns the bus name via a synchronous
 *      D-Bus call to org.freedesktop.DBus.GetNameOwner
 *   2. If name is already owned: log a warning and return false
 *   3. Register for bus name ownership via g_bus_own_name() with callbacks:
 *      - on_bus_acquired: Register the Toggle and Activate method handlers
 *      - on_name_acquired: Log success
 *      - on_name_lost: Log warning
 *   4. GDBus integrates with the default GLib main context automatically,
 *      so no polling timer or GSource wrapper is needed
 *
 * @param service    Pointer to a valid DBusService. Must not be NULL.
 * @param callback   The callback function to invoke on Toggle calls.
 * @param activate_cb The callback function to invoke on GNOME Shell Activate calls.
 * @param user_data  User data pointer passed to the callbacks.
 *
 * @return true if the service started successfully (bus name acquired),
 *         false if:
 *         - D-Bus session bus is unavailable (HK-005)
 *         - Another instance owns the bus name (single-instance conflict)
 *         - Introspection XML parsing fails
 *
 * @pre service != NULL && callback != NULL
 *
 * @see HK-003: Documentation (single-instance via bus name)
 * @see HK-005: D-Bus Session Bus Unavailable
 * @see SRS Section 8.3: Application Behavior (The Listener)
 */
bool dbus_service_start(
    DBusService* service,
    dbus_toggle_callback callback,
    dbus_activate_callback activate_cb,
    void* user_data);

/**
 * Stop the D-Bus service and release the bus name.
 *
 * This function gracefully stops the D-Bus service by:
 *   1. Releasing the bus name via g_bus_unown_name()
 *
 * After calling this function, the service is no longer listening for
 * D-Bus method calls. The service can be restarted by calling
 * dbus_service_start() again.
 *
 * @param service Pointer to a valid DBusService. Must not be NULL.
 * @return true if the service stopped successfully, false on error.
 */
bool dbus_service_stop(DBusService* service);

/*---------------------------------------------------------------------------
 * Section 5: Service Status and Diagnostics
 *---------------------------------------------------------------------------
 * Functions for checking the D-Bus service status and retrieving
 * diagnostic information.
 */

/**
 * Get the last error message from the D-Bus service.
 *
 * @param service Pointer to a valid DBusService. May be NULL (returns
 *                module-level error).
 * @return A null-terminated string containing the error message, or an
 *         empty string if no error has occurred. The returned string is
 *         internal and must NOT be freed.
 */
const char* dbus_service_get_error(const DBusService* service);

/*---------------------------------------------------------------------------
 * Section 6: D-Bus Command String Generation
 *---------------------------------------------------------------------------
 * Functions for generating the dbus-send command string for documentation.
 */

/**
 * Get the dbus-send command string for toggling the microphone.
 *
 * This function returns the complete dbus-send command that users can
 * configure in their desktop environment's hotkey settings. The command
 * is formatted for easy copy-pasting into terminal or hotkey configuration.
 *
 * The returned string is:
 *   "dbus-send --session --dest=org.xvoice.Controller "
 *   "  --type=method_call /org/xvoice/App "
 *   "  org.xvoice.Actions.Toggle"
 *
 * @return A statically allocated string containing the dbus-send command.
 *         The caller must NOT free the returned string.
 *
 * @see FR-026: Hotkey Command Display
 * @see SRS Section 8.4: Hotkey Configuration (The Trigger)
 */
const char* dbus_get_toggle_command(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DBUS_H */
