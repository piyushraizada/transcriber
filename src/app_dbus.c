/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * app_dbus.c - D-Bus service integration for single-instance enforcement
 *              and hotkey toggle support.
 *
 * This module provides a D-Bus service that allows the system hotkey
 * framework to toggle the microphone state. It also enforces single-instance
 * behavior by claiming a well-known bus name.
 *
 * Architecture:
 *   - Uses GDBus (GIO) for D-Bus session bus communication
 *   - Integrates natively with GLib/GTK main loop (no polling required)
 *   - Single-instance check via synchronous GetNameOwner call
 *   - Graceful degradation when D-Bus is unavailable
 */

#include "app_dbus.h"
#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal structures                                                 */
/* ------------------------------------------------------------------ */

/**
 * Internal D-Bus service state.
 *
 * introspection: Parsed D-Bus introspection XML (describes our interface)
 * owner_id:      Return value from g_bus_own_name() (0 = not owned)
 * callback:      Function pointer for toggle method handler
 * user_data:     User data passed to callback
 * error_msg:     Human-readable error message buffer
 */
struct _DBusService {
    GDBusNodeInfo    *introspection;
    guint             owner_id;
    dbus_toggle_callback callback;
    void             *user_data;
    char              error_msg[256];
};

/* ------------------------------------------------------------------ */
/* Static helper function declarations                                 */
/* ------------------------------------------------------------------ */

static void dbus_service_set_error(DBusService *service, const char *msg);
static GDBusNodeInfo *parse_introspection(void);

/* GDBus method handler — forward declaration for vtable below */
static void on_toggle_method(GDBusConnection       *connection,
                              const gchar           *sender,
                              const gchar           *object_path,
                              const gchar           *interface_name,
                              const gchar           *method_name,
                              GVariant              *parameters,
                              GDBusMethodInvocation *invocation,
                              gpointer               user_data);

/* D-Bus interface vtable — defined at file scope to avoid static inside function */
static const GDBusInterfaceVTable g_dbus_vtable = {
    .method_call = on_toggle_method,
};

/* GDBus name ownership callbacks */
static void on_bus_acquired(GDBusConnection *connection,
                            const gchar     *name,
                            gpointer         user_data);
static void on_name_acquired(GDBusConnection *connection,
                             const gchar     *name,
                             gpointer         user_data);
static void on_name_lost(GDBusConnection *connection,
                         const gchar     *name,
                         gpointer         user_data);

/* GDBus method handler */
static void on_toggle_method(GDBusConnection       *connection,
                             const gchar           *sender,
                             const gchar           *object_path,
                             const gchar           *interface_name,
                             const gchar           *method_name,
                             GVariant              *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer               user_data);

/* ------------------------------------------------------------------ */
/* Error management                                                    */
/* ------------------------------------------------------------------ */

/**
 * Set an error message on the D-Bus service.
 */
static void dbus_service_set_error(DBusService *service, const char *msg) {
    if (!service || !msg) return;
    g_strlcpy(service->error_msg, msg, sizeof(service->error_msg));
}

/**
 * Get the last error message from the D-Bus service.
 */
const char* dbus_service_get_error(const DBusService *service) {
    if (!service) return NULL;
    return service->error_msg;
}

/* ------------------------------------------------------------------ */
/* Introspection XML                                                   */
/* ------------------------------------------------------------------ */

/**
 * Parse the D-Bus introspection XML that describes our service interface.
 *
 * This XML defines the org.xvoice.Actions interface with a single Toggle
 * method that takes no parameters and returns no value.
 *
 * @return Parsed GDBusNodeInfo on success, NULL on failure.
 */
static GDBusNodeInfo *parse_introspection(void) {
    static const char xml[] =
        "<node>"
        "  <interface name=\"" DBUS_INTERFACE "\">"
        "    <method name=\"" DBUS_METHOD_TOGGLE "\">"
        "      <arg direction=\"out\" type=\"b\" name=\"success\"/>"
        "    </method>"
        "  </interface>"
        "</node>";

    GError *error = NULL;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, &error);
    if (error) {
        g_critical("GDBus: Failed to parse introspection XML: %s", error->message);
        g_error_free(error);
        return NULL;
    }
    return info;
}

/* ------------------------------------------------------------------ */
/* GDBus method handler                                                */
/* ------------------------------------------------------------------ */

/**
 * Handle the Toggle method call from external clients.
 *
 * When a desktop environment hotkey triggers:
 *   dbus-send --session --dest=org.xvoice.Controller \
 *     /org/xvoice/App org.xvoice.Actions.Toggle
 *
 * This handler is invoked by GDBus automatically. It calls the registered
 * toggle callback (which performs the state machine transition), then
 * sends a success reply.
 */
static void on_toggle_method(GDBusConnection       *connection,
                             const gchar           *sender,
                             const gchar           *object_path,
                             const gchar           *interface_name,
                             const gchar           *method_name,
                             GVariant              *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer               user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)method_name;
    (void)parameters;

    DBusService *service = (DBusService *)user_data;

    /* Invoke the toggle callback if registered */
    if (service && service->callback) {
        service->callback(service->user_data);
    }

    /* Send success reply with explicit return type matching introspection XML */
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", true));
}

/* ------------------------------------------------------------------ */
/* GDBus name ownership callbacks                                      */
/* ------------------------------------------------------------------ */

/**
 * Called when the D-Bus connection is acquired and our name is ready.
 * This is where we register the method handler for our interface.
 */
static void on_bus_acquired(GDBusConnection *connection,
                            const gchar     *name,
                            gpointer         user_data)
{
    (void)name;

    DBusService *service = (DBusService *)user_data;

    if (!connection || !service || !service->introspection) {
        return;
    }

    /* Get the interface definition from parsed introspection */
    GDBusInterfaceInfo *iface = service->introspection->interfaces[0];
    if (!iface) {
        g_warning("GDBus: No interface found in introspection data");
        return;
    }

    /* Register our object at /org/xvoice/App */
    GError *error = NULL;
    gboolean registered = g_dbus_connection_register_object(
        connection,
        DBUS_OBJECT_PATH,
        iface,
        &g_dbus_vtable,
        service,
        NULL,   // GDestroyNotify
        &error
    );

    if (!registered) {
        g_critical("GDBus: Failed to register object at %s: %s",
                   DBUS_OBJECT_PATH, error->message);
        g_error_free(error);
    } else {
        g_message("GDBus: Registered object at %s with interface %s",
                  DBUS_OBJECT_PATH, iface->name);
    }
}

/**
 * Called when we successfully acquire the bus name.
 */
static void on_name_acquired(GDBusConnection *connection,
                             const gchar     *name,
                             gpointer         user_data)
{
    (void)connection;
    (void)user_data;
    g_message("GDBus: Acquired bus name '%s'", name);
}

/**
 * Called when we lose the bus name (another process took it, or we released it).
 */
static void on_name_lost(GDBusConnection *connection,
                         const gchar     *name,
                         gpointer         user_data)
{
    (void)connection;
    (void)user_data;
    g_warning("GDBus: Lost bus name '%s'", name);
}

/* ------------------------------------------------------------------ */
/* Public API - Lifecycle                                              */
/* ------------------------------------------------------------------ */

/**
 * Create and initialize a D-Bus service instance.
 */
DBusService *dbus_service_create(void) {
    DBusService *service = g_new0(DBusService, 1);
    return service;
}

/**
 * Destroy a D-Bus service and release all resources.
 */
void dbus_service_destroy(DBusService *service) {
    if (!service) return;

    /* Stop the service if it's running */
    if (service->owner_id != 0) {
        dbus_service_stop(service);
    }

    /* Free introspection data */
    if (service->introspection) {
        g_dbus_node_info_unref(service->introspection);
        service->introspection = NULL;
    }

    /* Free the service structure */
    g_free(service);
}

/* ------------------------------------------------------------------ */
/* Public API - Service Control                                        */
/* ------------------------------------------------------------------ */

/**
 * Start the D-Bus service and request the bus name.
 *
 * Single-instance enforcement: Before requesting the bus name, we perform
 * a synchronous D-Bus call to org.freedesktop.DBus.GetNameOwner. If the
 * name already has an owner, another instance is running and we return
 * false so the application can exit.
 */
bool dbus_service_start(DBusService *service,
                        dbus_toggle_callback callback,
                        void *user_data)
{
    if (!service) return false;

    /* Validate callback parameter (pre-condition: callback != NULL) */
    if (!callback) {
        dbus_service_set_error(service, "Toggle callback must not be NULL");
        return false;
    }

    /* Store callback and user data */
    service->callback = callback;
    service->user_data = user_data;

    /* Parse introspection XML */
    service->introspection = parse_introspection();
    if (!service->introspection) {
        dbus_service_set_error(service, "Failed to parse D-Bus introspection XML");
        return false;
    }

    /* Check if another instance already owns the bus name (single-instance) */
    GError *call_error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &call_error);

    if (!bus) {
        /* Cannot reach session bus — D-Bus unavailable */
        if (call_error) {
            char msg[256];
            snprintf(msg, sizeof(msg), "D-Bus session bus unavailable: %s", call_error->message);
            dbus_service_set_error(service, msg);
            g_error_free(call_error);
        } else {
            dbus_service_set_error(service, "D-Bus session bus unavailable");
        }
        g_dbus_node_info_unref(service->introspection);
        service->introspection = NULL;
        return false;
    }

    /* Pass NULL for expected_return_type to skip type validation (avoids leak) */
    GVariant *owner_var = g_dbus_connection_call_sync(
        bus,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "GetNameOwner",
        g_variant_new("(s)", DBUS_BUS_NAME),
        NULL,  // expected_return_type — NULL skips type validation
        G_DBUS_CALL_FLAGS_NONE,
        -1,    // no timeout
        NULL,  // cancellable
        &call_error
    );

    g_object_unref(bus);  // done with the connection

    if (call_error != NULL) {
        /* Error calling GetNameOwner — if it's NAME_HAS_NO_OWNER, that's good */
        if (g_error_matches(call_error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER)) {
            /* No owner — we can claim the name */
            g_error_free(call_error);
        } else {
            /* Real error (bus gone, timeout, etc.) */
            char msg[256];
            snprintf(msg, sizeof(msg), "D-Bus GetNameOwner failed: %s", call_error->message);
            dbus_service_set_error(service, msg);
            g_error_free(call_error);
            g_dbus_node_info_unref(service->introspection);
            service->introspection = NULL;
            return false;
        }
    } else if (owner_var != NULL) {
        /* Name is already owned — another instance is running */
        g_variant_unref(owner_var);
        dbus_service_set_error(service, "Another instance is already running");
        g_dbus_node_info_unref(service->introspection);
        service->introspection = NULL;
        return false;
    }
    /* else: owner_var is NULL and no error — should not happen, but safe to continue */

    /* Request bus name ownership via GDBus */
    service->owner_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        DBUS_BUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_NONE,  // Do not queue, do not replace
        on_bus_acquired,
        on_name_acquired,
        on_name_lost,
        service,
        NULL  // GDestroyNotify
    );

    if (service->owner_id == 0) {
        dbus_service_set_error(service, "Failed to own D-Bus bus name");
        g_dbus_node_info_unref(service->introspection);
        service->introspection = NULL;
        return false;
    }

    g_message("GDBus: Service started, watching bus name '%s'", DBUS_BUS_NAME);
    return true;
}

/**
 * Stop the D-Bus service and release the bus name.
 */
bool dbus_service_stop(DBusService *service) {
    if (!service) return false;

    if (service->owner_id != 0) {
        g_bus_unown_name(service->owner_id);
        service->owner_id = 0;
        g_message("GDBus: Released bus name '%s'", DBUS_BUS_NAME);
    }

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
           "--dest=" DBUS_BUS_NAME " "
           DBUS_OBJECT_PATH " "
           DBUS_INTERFACE "." DBUS_METHOD_TOGGLE;
}
