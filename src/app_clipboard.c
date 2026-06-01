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
 * @see app_clipboard.h
 * @see SRS Section 4: Functional Requirements (FR-015, FR-016)
 * @see SRS Section 7: UI Requirements (UI-008 through UI-010)
 */

#include "app_clipboard.h"
#include "app.h"  /* For UNUSED macro */

#include <pthread.h>
#include <string.h>

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

/* Removed unused clipboard_copy_from_text_view(), clipboard_clear(),
 * clipboard_get_clipboard(), and clipboard_get_primary(). */

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
