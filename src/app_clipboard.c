/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/**
 * @file app_clipboard.c
 * @brief GTK3 Clipboard Integration implementation
 *
 * Implements copy-to-clipboard operations using the GTK3 clipboard API,
 * supporting both CLIPBOARD and PRIMARY selections.
 *
 * Wayland note: the compositor refuses clipboard ownership changes
 * (zwp_data_device set_selection) from clients whose window does not
 * have keyboard focus. Transcriptions triggered via the global hotkey
 * therefore fail to reach the clipboard in-process — the user's editor
 * holds focus and the write is silently dropped by the compositor
 * (verified on GNOME/Mutter 46). The copy is routed through a momentary
 * X11 helper child process (this same binary re-executed with
 * --clipboard-x11-helper under GDK_BACKEND=x11): Xwayland clients are
 * not subject to the focus gate, and gnome-shell bridges the X11
 * selection to the Wayland clipboard and persists it after the helper
 * exits.
 *
 * @see app_clipboard.h
 * @see SRS Section 4: Functional Requirements (FR-015, FR-016)
 * @see SRS Section 7: UI Requirements (UI-008 through UI-010)
 */

#include "app_clipboard.h"
#include "app.h"  /* For UNUSED macro */

#include <gtk/gtk.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

/*---------------------------------------------------------------------------
 * Static error buffer — protected by mutex for thread safety
 *---------------------------------------------------------------------------*/

static char g_clipboard_error[256] = {0};
static pthread_mutex_t g_clipboard_error_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_error(const char* msg)
{
    pthread_mutex_lock(&g_clipboard_error_mutex);
    if (msg) {
        snprintf(g_clipboard_error, sizeof(g_clipboard_error), "%s", msg);
    } else {
        g_clipboard_error[0] = '\0';
    }
    pthread_mutex_unlock(&g_clipboard_error_mutex);
}

const char* clipboard_get_error(void)
{
    /* Lock mutex, copy to thread-local buffer, unlock, then return. */
    static __thread char local_buffer[256] = {0};
    pthread_mutex_lock(&g_clipboard_error_mutex);
    snprintf(local_buffer, sizeof(local_buffer), "%s", g_clipboard_error);
    pthread_mutex_unlock(&g_clipboard_error_mutex);
    return local_buffer;
}

/*---------------------------------------------------------------------------
 * Section 1: Clipboard Copy Operations
 *---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------
 * Wayland focus-policy workaround (X11 helper bridge)
 *---------------------------------------------------------------------------*/

/* Seconds the helper keeps its event loop spinning so the X11 clipboard
 * agent can fetch and persist the selection before the helper exits. */
#define CLIPBOARD_HELPER_SETTLE_SEC 2

/**
 * True when the default GDK display is Wayland (GTK3 has no public backend
 * query; the registered GType name is the reliable runtime check).
 */
static gboolean session_uses_wayland(void)
{
    GdkDisplay *dpy = gdk_display_get_default();
    if (!dpy) {
        return FALSE;
    }
    return strstr(G_OBJECT_TYPE_NAME(dpy), "Wayland") != NULL;
}

/**
 * Spawn "transcriber --clipboard-x11-helper" with text delivered on stdin.
 * The child copies the text through Xwayland, bypassing the Wayland
 * compositor's "clipboard writes only from the focused window" policy.
 *
 * @return true if the child was spawned (its exit status is not checked).
 */
static bool spawn_x11_clipboard_helper(const char* text)
{
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len < 0) {
        set_error("readlink(/proc/self/exe) failed");
        return false;
    }
    exe_path[len] = '\0';

    char *argv[] = { exe_path, (char *)"--clipboard-x11-helper", NULL };

    char **env = g_get_environ();
    env = g_environ_setenv(env, "GDK_BACKEND", "x11", TRUE);

    gint child_stdin = -1;
    GError *err = NULL;
    const bool spawned = g_spawn_async_with_pipes(
        NULL, argv, env,
        (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_DEFAULT),
        NULL, NULL, NULL, &child_stdin, NULL, NULL, &err);
    g_strfreev(env);

    if (!spawned || child_stdin < 0) {
        set_error(err ? err->message : "clipboard helper spawn failed");
        g_clear_error(&err);
        return false;
    }

    /* Blocking write on the pipe: segments are far below the 64 KiB pipe
     * buffer, so this never blocks the GTK main thread in practice. */
    size_t total = strlen(text);
    size_t sent = 0;
    while (sent < total) {
        ssize_t n = write(child_stdin, text + sent, total - sent);
        if (n <= 0) {
            break;
        }
        sent += (size_t)n;
    }
    close(child_stdin);  /* EOF tells the helper the text is complete */

    set_error(NULL);
    return true;
}

int clipboard_run_x11_helper(void)
{
    /* Read the text from stdin until EOF (parent closes the pipe). */
    GString *text = g_string_new(NULL);
    char buf[4096];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        g_string_append_len(text, buf, n);
    }

    gdk_set_allowed_backends("x11");
    g_setenv("GDK_BACKEND", "x11", TRUE);
    if (!gtk_init_check(NULL, NULL)) {
        g_string_free(text, TRUE);
        return 1;  /* No X11/Xwayland available — nothing we can do. */
    }

    GdkDisplay *dpy = gdk_display_get_default();
    GtkClipboard *clipboard = gtk_clipboard_get_for_display(dpy, GDK_SELECTION_CLIPBOARD);
    if (!clipboard) {
        g_string_free(text, TRUE);
        return 1;
    }
    gtk_clipboard_set_text(clipboard, text->str, -1);
    gtk_clipboard_store(clipboard);

    GtkClipboard *primary = gtk_clipboard_get_for_display(dpy, GDK_SELECTION_PRIMARY);
    if (primary) {
        gtk_clipboard_set_text(primary, text->str, -1);
    }

    /* Stay alive briefly so the X11 clipboard agent can request and cache
     * the selection contents; after that the text survives our exit. */
    g_timeout_add_seconds(CLIPBOARD_HELPER_SETTLE_SEC, (GSourceFunc)gtk_main_quit, NULL);
    gtk_main();

    g_string_free(text, TRUE);
    return 0;
}

bool clipboard_copy_text(GdkDisplay* display, const char* text)
{
    UNUSED(display); /* not required for CLIPBOARD selection */
    if (!text) {
        set_error("NULL text parameter");
        return false;
    }

    GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!clipboard) {
        set_error("Failed to get CLIPBOARD selection");
        return false;
    }

    gtk_clipboard_set_text(clipboard, text, -1);
    gtk_clipboard_store(clipboard);
    /* Do NOT unref: gtk_clipboard_get() returns a shared singleton */

    set_error(NULL);
    return true;
}

bool clipboard_copy_text_both(GdkDisplay* display, const char* text)
{
    UNUSED(display); /* not required for clipboard operations */
    if (!text) {
        set_error("NULL text parameter");
        return false;
    }

    bool clipboard_ok = clipboard_copy_text(display, text);

    /* On Wayland the in-process write above is silently dropped unless our
     * window holds keyboard focus (compositor policy). The X11 helper
     * covers the unfocused case; the X11 clipboard agent bridges the
     * result to the Wayland clipboard. */
    if (session_uses_wayland()) {
        if (!spawn_x11_clipboard_helper(text)) {
            g_warning("[clipboard] X11 helper spawn failed: %s", clipboard_get_error());
        }
    }

    /* Try PRIMARY selection first — it works on X11 and may work on XWayland.
     * Only skip if gtk_clipboard_get() returns NULL (native Wayland). */
    GtkClipboard* primary = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
    if (primary) {
        gtk_clipboard_set_text(primary, text, -1);
        /* Do NOT unref: shared singleton from gtk_clipboard_get() */
    }
    /* PRIMARY unavailable (native Wayland) — not fatal, CLIPBOARD was already set */

    set_error(NULL);
    return clipboard_ok;
}

/*---------------------------------------------------------------------------
 * Clipboard Clear — remove application content on shutdown
 *---------------------------------------------------------------------------*/

bool clipboard_clear(GdkDisplay* display)
{
    UNUSED(display); /* not required for clipboard operations */

    bool cleared = true;

    /* Clear CLIPBOARD selection */
    GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (clipboard) {
        gtk_clipboard_set_text(clipboard, "", -1);
        /* Do NOT store: we want the empty text to be cleared immediately on
         * exit without persisting. Calling store() here would keep an empty
         * string in the clipboard manager after process exit. */
    } else {
        cleared = false;
    }

    /* Clear PRIMARY selection if available */
    GtkClipboard* primary = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
    if (primary) {
        gtk_clipboard_set_text(primary, "", -1);
    }
    /* PRIMARY unavailable on native Wayland — not an error */

    /* On Wayland the shutdown-time clear needs the X11 helper as well:
     * a stored X11 selection keeps persisting in the clipboard agent
     * even after this process exits, so clearing must re-own it. */
    if (session_uses_wayland()) {
        if (!spawn_x11_clipboard_helper("")) {
            g_warning("[clipboard] X11 helper spawn failed during clear: %s",
                      clipboard_get_error());
        }
    }

    set_error(NULL);
    return cleared;
}

/*---------------------------------------------------------------------------
 * Section 3: Clipboard Utilities
 *---------------------------------------------------------------------------*/

bool clipboard_is_available(GdkDisplay* display)
{
    UNUSED(display); /* Reserved for future display-specific clipboard logic */
    GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!clipboard) {
        set_error("Clipboard is unavailable");
        return false;
    }

    /* Do NOT unref: shared singleton from gtk_clipboard_get() */
    set_error(NULL);
    return true;
}
