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
 * When the ToggleMicrophone method is called, the application performs
 * the same action as clicking the microphone icon:
 *   - If IDLE: Start recording (transition to LISTENING state)
 *   - If LISTENING: Stop recording and start transcription (transition to TRANSCRIBING)
 *   - If TRANSCRIBING: Wait for transcription to complete, then return to IDLE
 *
 * The D-Bus module operates as follows:
 *   1. On startup: Request the bus name "org.xvoice.Controller" on the
 *      session bus. If another instance already owns the name, the new
 *      instance exits (single-instance enforcement).
 *   2. Register the ToggleMicrophone method handler.
 *   3. Monitor the D-Bus connection for incoming method calls.
 *   4. On method call: Invoke the toggle callback in the GTK main thread.
 *   5. On shutdown: Release the bus name and close the connection.
 *
 * The module uses libdbus-1 (the reference D-Bus C library) for low-level
 * D-Bus operations. The D-Bus main loop integration is handled via a
 * GSource wrapper that integrates the D-Bus file descriptor into the GTK
 * main loop, allowing D-Bus messages to be processed without a separate
 * thread.
 *
 * @see SRS Section 8: Hotkey Integration (HK-001 through HK-005)
 * @see SRS Section 2.5: Hotkey Event Flow — D-Bus Integration
 * @see FR-036: OS-Configured Hotkey Toggle
 * @see NR-020: X11 and Wayland Hotkey Support
 */

#include <dbus-1.0/dbus/dbus.h>
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

/*---------------------------------------------------------------------------
 * Section 2: D-Bus Service Handle (Opaque)
 *---------------------------------------------------------------------------
 * Opaque handle to the internal D-Bus service state. The actual structure
 * contains the DBusConnection pointer, the bus name owner ID, the GSource
 * ID for main loop integration, and the toggle callback function pointer.
 *
 * Users of this module should NEVER access the internal fields directly.
 * All interaction must go through the public API functions defined below.
 *
 * The D-Bus service is initialized once at application startup and
 * cleaned up at application exit. The service runs in the GTK main
 * thread (presentation thread) via GSource integration.
 *
 * @see SRS Section 2.1: Threading Model — Presentation Thread handles D-Bus
 */
typedef struct _DBusService DBusService;

/*---------------------------------------------------------------------------
 * Section 3: Toggle Callback Typedef
 *---------------------------------------------------------------------------
 * Callback function type invoked when the ToggleMicrophone D-Bus method
 * is called by an external client.
 */

/**
 * Callback function invoked when the ToggleMicrophone D-Bus method is called.
 *
 * This callback is invoked in the GTK main thread when an external client
 * (e.g., dbus-send command triggered by a desktop environment hotkey) calls
 * the ToggleMicrophone method. The callback should perform the same action
 * as clicking the microphone icon in the UI.
 *
 * @param user_data User-provided data pointer (set during dbus_service_start()).
 */
typedef void (*dbus_toggle_callback)(void* user_data);

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
 *   1. Stop the D-Bus service (if running) — release bus name
 *   2. Remove the GSource from the GTK main loop
 *   3. Close the D-Bus connection
 *   4. Free the DBusService struct
 *
 * @param service Pointer to a valid DBusService. Must not be NULL.
 */
void dbus_service_destroy(DBusService* service);

/**
 * Start the D-Bus service and request the bus name.
 *
 * This function activates the D-Bus service by performing the following
 * steps:
 *   1. Connect to the D-Bus session bus via dbus_bus_get()
 *   2. Request the well-known name "org.xvoice.Controller" via
 *      dbus_bus_request_name() with flags:
 *         - DBUS_NAME_FLAG_REPLACE_EXISTING: Replace any existing owner
 *           (enforces single-instance behavior)
 *   3. If name acquisition fails (another instance owns it):
 *      Log a warning and return false (the application should exit)
 *   4. Register the ToggleMicrophone method handler
 *   5. Create a GSource for the D-Bus file descriptor and attach it
 *      to the GTK main context
 *   6. Set the toggle callback function
 *
 * @param service    Pointer to a valid DBusService. Must not be NULL.
 * @param callback   The callback function to invoke on ToggleMicrophone calls.
 * @param user_data  User data pointer passed to the callback.
 *
 * @return true if the service started successfully (bus name acquired),
 *         false if:
 *         - D-Bus session bus is unavailable (HK-005)
 *         - Another instance owns the bus name (single-instance conflict)
 *         - GSource creation fails
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
    void* user_data);

/**
 * Stop the D-Bus service and release the bus name.
 *
 * This function gracefully stops the D-Bus service by:
 *   1. Removing the GSource from the GTK main loop
 *   2. Releasing the bus name via dbus_bus_release_name()
 *   3. Closing the D-Bus connection via dbus_connection_close()
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
 * Check if the D-Bus service is currently active.
 *
 * @deprecated Unused public function — no external callers.
 *             Diagnostic function with no call sites.
 *
 * @param service Pointer to a valid DBusService. Must not be NULL.
 * @return true if the service is active (bus name acquired and listening),
 *         false otherwise.
 */
bool dbus_service_is_active(const DBusService* service);

/**
 * Check if the D-Bus session bus is available.
 *
 * @deprecated Unused public function — no external callers.
 *             Could be used for pre-flight checks but has no call sites.
 *
 * This function attempts to connect to the D-Bus session bus to verify
 * that it is available. This is a lightweight check that does NOT request
 * a bus name or register any handlers.
 *
 * @return true if the session bus is available, false if unavailable.
 *
 * @see HK-005: D-Bus Session Bus Unavailable
 */
bool dbus_session_bus_available(void);

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
 * Section 6: D-Bus Message Processing
 *---------------------------------------------------------------------------
 * Functions for integrating D-Bus message processing with the GTK main loop.
 *
 * These functions are used internally to create a GSource that monitors
 * the D-Bus file descriptor and dispatches incoming messages. The GSource
 * is attached to the GTK main context, allowing D-Bus messages to be
 * processed in the GTK main thread without a separate D-Bus thread.
 */

/**
 * Process pending D-Bus messages.
 *
 * This function reads and dispatches any pending D-Bus messages from the
 * connection. It should be called from the GTK main loop via the GSource
 * callback when the D-Bus file descriptor becomes readable.
 *
 * This function is typically called internally by the GSource callback
 * and does NOT need to be called by application code.
 *
 * @param service Pointer to a valid DBusService. Must not be NULL.
 * @return true if messages were processed successfully, false on error.
 */
bool dbus_service_process_messages(DBusService* service);

/*---------------------------------------------------------------------------
 * Section 7: D-Bus Command String Generation
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

/* MAJ-002/MIN-001 fix: Removed unused dbus_get_toggle_command_formatted()
 * and dbus_another_instance_running() declarations. */

#ifdef __cplusplus
}
#endif

#endif /* APP_DBUS_H */
