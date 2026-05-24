/*
 * app_dbus.c - D-Bus service integration for single-instance enforcement
 *              and hotkey toggle support.
 *
 * This module provides a D-Bus service that allows the system hotkey
 * framework to toggle the microphone state. It also enforces single-instance
 * behavior by claiming a well-known bus name.
 *
 * Architecture:
 *   - Uses libdbus-1 for D-Bus session bus communication
 *   - Integrates with GTK main loop via GSource wrapper around D-Bus file descriptor
 *   - Exponential backoff retry for bus connection
 *   - Graceful degradation when D-Bus is unavailable
 */

#define _POSIX_C_SOURCE 199309L

#include "app_dbus.h"
#include <dbus/dbus.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Internal constants                                                  */
/* ------------------------------------------------------------------ */

#define DBUS_RETRY_MAX_ATTEMPTS 5
#define DBUS_RETRY_INITIAL_DELAY_MS 100
#define DBUS_RETRY_MAX_DELAY_MS 1000

/* ------------------------------------------------------------------ */
/* Internal structures                                                 */
/* ------------------------------------------------------------------ */

/**
 * Internal D-Bus service state.
 *
 * connection:    DBusConnection* to the session bus (NULL if not connected)
 * owner_id:      Unique name owner ID from dbus_bus_request_name()
 * callback:      Function pointer for toggle method handler
 * user_data:     User data passed to callback
 * source:        GSource for integrating D-Bus FD into GTK main loop
 * source_funcs:  GSourceFuncs table for the GSource
 * error_msg:     Human-readable error message buffer
 */
struct _DBusService {
    DBusConnection *connection;
    int owner_id;
    dbus_toggle_callback callback;
    void *user_data;
    GSource *source;
    char error_msg[256];
};

/* ------------------------------------------------------------------ */
/* Static helper function declarations                                 */
/* ------------------------------------------------------------------ */

/* Renamed from dbus_set_error to avoid conflict with D-Bus library's dbus_set_error() */
static void dbus_service_set_error(DBusService *service, const char *msg);
static gboolean dbus_setup_main_loop_integration(DBusService *service);
static gboolean dbus_dispatch_timeout(gpointer user_data);
static void dbus_handle_method_call(DBusService *service, DBusMessage *msg);
static void dbus_send_error_reply(DBusService *service, DBusMessage *msg, const char *error_name, const char *error_msg);

/* ------------------------------------------------------------------ */
/* Error management                                                    */
/* ------------------------------------------------------------------ */

/**
 * Set an error message on the D-Bus service.
 * Renamed from dbus_set_error to avoid name conflict with D-Bus library.
 */
static void dbus_service_set_error(DBusService *service, const char *msg) {
    if (!service || !msg) return;
    g_strlcpy(service->error_msg, msg, sizeof(service->error_msg));
}

/**
 * Get the last error message from the D-Bus service.
 * Matches header declaration: const char* dbus_service_get_error(const DBusService* service);
 */
const char* dbus_service_get_error(const DBusService *service) {
    if (!service) return NULL;
    return service->error_msg;
}

/* ------------------------------------------------------------------ */
/* D-Bus timeout-based integration with GLib main loop                 */
/* ------------------------------------------------------------------ */

/**
 * Periodic dispatch timeout - polls D-Bus for messages.
 * Uses a simple timeout-based approach to avoid dbus_connection_get_unix_fd()
 * which can crash in some environments.
 */
static gboolean dbus_dispatch_timeout(gpointer user_data) {
    DBusService *service = (DBusService *)user_data;
    if (!service || !service->connection) {
        return FALSE;
    }

    /* Use read_write (NOT read_write_dispatch) so messages remain in the
     * queue for dbus_connection_pop_message() to consume.
     * read_write_dispatch() would internally dispatch messages via slots,
     * bypassing our pop-based handler. */
    dbus_connection_read_write(service->connection, 0);
    dbus_service_process_messages(service);

    return TRUE; /* keep repeating */
}

/**
 * Set up D-Bus integration with the GLib main loop using a periodic timeout.
 *
 * Returns TRUE on success, FALSE on failure.
 */
static gboolean dbus_setup_main_loop_integration(DBusService *service) {
    if (!service || !service->connection) {
        return FALSE;
    }

    /* Set up a periodic dispatch timeout (100ms interval) */
    service->source = g_timeout_source_new(100);
    if (service->source) {
        g_source_set_callback(service->source, dbus_dispatch_timeout, service, NULL);
        g_source_set_priority(service->source, G_PRIORITY_HIGH);
        g_source_attach(service->source, NULL);
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* D-Bus method call handling                                          */
/* ------------------------------------------------------------------ */

/**
 * Send an error reply to a D-Bus method call.
 */
static void dbus_send_error_reply(DBusService *service, DBusMessage *msg, const char *error_name, const char *error_msg) {
    if (!service || !msg) return;

    DBusMessage *reply = dbus_message_new_error(msg, error_name, error_msg);
    if (reply) {
        dbus_connection_send(service->connection, reply, NULL);
        dbus_message_unref(reply);
    }
}

/**
 * Handle a D-Bus method call message.
 * Currently only handles the ToggleMicrophone method.
 */
static void dbus_handle_method_call(DBusService *service, DBusMessage *msg) {
    if (!service || !msg) return;

    /* Check if this is the ToggleMicrophone method */
    const char *member = dbus_message_get_member(msg);
    if (!member) return;

    fprintf(stderr, "[dbus] Method call received: %s\n", member);

    if (strcmp(member, DBUS_METHOD_TOGGLE) == 0) {
        fprintf(stderr, "[dbus] ToggleMicrophone invoked — calling callback\n");
        /* Invoke the toggle callback if registered */
        if (service->callback) {
            service->callback(service->user_data);
        } else {
            fprintf(stderr, "[dbus] WARNING: No toggle callback registered!\n");
        }

        /* Send a success reply */
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(service->connection, reply, NULL);
            dbus_message_unref(reply);
        }
    } else {
        fprintf(stderr, "[dbus] Unknown method: %s — sending error reply\n", member);
        /* Unknown method - send error */
        dbus_send_error_reply(service, msg,
                              "org.xvoice.Actions.Error.UnknownMethod",
                              "Unknown method called");
    }
}

/* ------------------------------------------------------------------ */
/* Public API - Message Processing                                     */
/* ------------------------------------------------------------------ */

/**
 * Process pending D-Bus messages.
 * Matches header declaration: bool dbus_service_process_messages(DBusService* service);
 */
bool dbus_service_process_messages(DBusService *service) {
    if (!service || !service->connection) {
        return false;
    }

    /* Pop and process all pending messages from the D-Bus connection.
     * Note: We use pop_message rather than dispatch because the caller
     * (dbus_dispatch_timeout) already called read_write to pull data
     * off the socket. Messages are now queued internally and ready to pop. */
    DBusMessage *msg;
    bool processed = false;

    while ((msg = dbus_connection_pop_message(service->connection))) {
        if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
            dbus_handle_method_call(service, msg);
            processed = true;
        }
        dbus_message_unref(msg);
    }

    return processed;
}

/* ------------------------------------------------------------------ */
/* Public API - Lifecycle                                              */
/* ------------------------------------------------------------------ */

/**
 * Create and initialize a D-Bus service instance.
 * Matches header declaration: DBusService* dbus_service_create(void);
 */
DBusService *dbus_service_create(void) {
    DBusService *service = (DBusService *)calloc(1, sizeof(DBusService));
    if (!service) {
        return NULL;
    }

    service->connection = NULL;
    service->owner_id = -1;
    service->callback = NULL;
    service->user_data = NULL;
    service->source = NULL;
    service->error_msg[0] = '\0';

    return service;
}

/**
 * Destroy a D-Bus service and release all resources.
 */
void dbus_service_destroy(DBusService *service) {
    if (!service) return;

    /* Stop the service if it's running */
    if (service->connection) {
        dbus_service_stop(service);
    }

    /* Free the service structure */
    free(service);
}

/* ------------------------------------------------------------------ */
/* Public API - Service Control                                        */
/* ------------------------------------------------------------------ */

/**
 * Start the D-Bus service and request the bus name.
 * Matches header declaration:
 *   bool dbus_service_start(DBusService* service, dbus_toggle_callback callback, void* user_data);
 */
bool dbus_service_start(DBusService *service, dbus_toggle_callback callback, void *user_data) {
    if (!service) return false;

    /* Store callback and user data */
    service->callback = callback;
    service->user_data = user_data;

    DBusError err;
    dbus_error_init(&err);

    /* Connect to the session bus */
    service->connection = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "D-Bus session bus connection failed: %s", err.message);
        dbus_service_set_error(service, msg);
        dbus_error_free(&err);
        return false;
    }

    /* Request the well-known bus name */
    service->owner_id = dbus_bus_request_name(
        service->connection,
        DBUS_BUS_NAME,
        DBUS_NAME_FLAG_DO_NOT_QUEUE,
        &err
    );

    if (dbus_error_is_set(&err)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "D-Bus name request failed: %s", err.message);
        dbus_service_set_error(service, msg);
        dbus_error_free(&err);
        /* Do NOT close shared connection - just clear pointer */
        service->connection = NULL;
        return false;
    }

    /* Check if we successfully acquired the name */
    if (service->owner_id == DBUS_REQUEST_NAME_REPLY_EXISTS ||
        service->owner_id == DBUS_REQUEST_NAME_REPLY_IN_QUEUE) {
        /* Another instance is already running */
        dbus_service_set_error(service, "Another instance is already running");
        dbus_bus_release_name(service->connection, DBUS_BUS_NAME, NULL);
        /* Do NOT close shared connection - just clear pointer */
        service->connection = NULL;
        service->owner_id = -1;
        return false;
    }

    /* Set up D-Bus integration with GLib main loop */
    if (!dbus_setup_main_loop_integration(service)) {
        dbus_service_set_error(service, "Failed to set up D-Bus main loop integration");
        dbus_bus_release_name(service->connection, DBUS_BUS_NAME, NULL);
        /* Do NOT close shared connection - just clear pointer */
        service->connection = NULL;
        service->owner_id = -1;
        return false;
    }

    return true;
}

/**
 * Stop the D-Bus service and release the bus name.
 * Matches header declaration: bool dbus_service_stop(DBusService* service);
 */
bool dbus_service_stop(DBusService *service) {
    if (!service) return false;

    /* Remove the GSource from the main loop */
    if (service->source) {
        g_source_destroy(service->source);
        g_source_unref(service->source);
        service->source = NULL;
    }

    /* Release the bus name using dbus_bus_release_name */
    if (service->connection && service->owner_id != -1) {
        dbus_bus_release_name(service->connection, DBUS_BUS_NAME, NULL);
        service->owner_id = -1;
    }

    /* Do NOT close shared connections obtained via dbus_bus_get().
     * Just release the bus name and clear the pointer. */
    service->connection = NULL;

    return true;
}

/* ------------------------------------------------------------------ */
/* Public API - Status Checks                                          */
/* ------------------------------------------------------------------ */

/**
 * Check if the D-Bus service is currently active.
 * Matches header declaration: bool dbus_service_is_active(const DBusService* service);
 */
bool dbus_service_is_active(const DBusService *service) {
    if (!service) return false;
    return (service->connection != NULL && service->owner_id != -1);
}

/**
 * Check if the D-Bus session bus is available.
 */
bool dbus_session_bus_available(void) {
    DBusError err;
    dbus_error_init(&err);

    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return false;
    }

    /* Do NOT close shared connection — conn was obtained via dbus_bus_get()
     * which returns a shared connection owned by libdbus. Closing it would
     * affect other users and may cause crashes. See also dbus_service_start(). */
    (void)conn;
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API - Command String Generation                              */
/* ------------------------------------------------------------------ */

/**
 * Get the dbus-send command string for toggling the microphone.
 */
const char *dbus_get_toggle_command(void) {
    return "dbus-send --session --type=method_call "
           "--dest=org.xvoice.Controller "
           "/org/xvoice/App "
           "org.xvoice.Actions.Toggle";
}

/**
 * Get a multi-line formatted version of the dbus-send command.
 */
const char *dbus_get_toggle_command_formatted(void) {
    return "dbus-send --session "
           "--dest=org.xvoice.Controller "
           "--type=method_call "
           "/org/xvoice/App "
           "org.xvoice.Actions.Toggle";
}

/* ------------------------------------------------------------------ */
/* Public API - Single-Instance Enforcement                            */
/* ------------------------------------------------------------------ */

/**
 * Check if another instance of the application is already running.
 */
bool dbus_another_instance_running(void) {
    DBusError err;
    dbus_error_init(&err);

    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return false;
    }

    /* Try to request the bus name */
    int reply = dbus_bus_request_name(
        conn,
        DBUS_BUS_NAME,
        DBUS_NAME_FLAG_DO_NOT_QUEUE,
        &err
    );

    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        /* Do NOT close shared connection */
        (void)conn;
        return false;
    }

    bool another_running = (reply == DBUS_REQUEST_NAME_REPLY_EXISTS ||
                            reply == DBUS_REQUEST_NAME_REPLY_IN_QUEUE);

    /* Release the name if we got it - use dbus_bus_release_name */
    if (!another_running) {
        dbus_bus_release_name(conn, DBUS_BUS_NAME, NULL);
    }

    /* Do NOT close shared connection */
    (void)conn;
    return another_running;
}
