#ifndef APP_CLIPBOARD_H
#define APP_CLIPBOARD_H

/**
 * @file app_clipboard.h
 * @brief GTK3 Clipboard Integration — Copy transcription text to system clipboard
 *
 * This module implements the clipboard integration described in Section 4
 * (FR-015, FR-016) and Section 7 (UI-008 through UI-010) of the SRS. It
 * provides functions for copying the transcription text to the system
 * clipboard using the GTK3 clipboard API.
 *
 * The module supports two clipboard targets:
 *   - GDK_SELECTION_CLIPBOARD: The standard system clipboard (Ctrl+C/Ctrl+V)
 *   - GDK_SELECTION_PRIMARY: The primary selection (middle-click paste on X11)
 *
 * On X11, both selections are populated simultaneously to ensure maximum
 * compatibility with user paste habits. On Wayland, only the CLIPBOARD
 * selection is available (PRIMARY selection is not supported by the
 * Wayland protocol).
 *
 * The clipboard module is minimal and focused — it provides a single
 * function to copy text to the clipboard, and optional functions for
 * checking clipboard availability and clearing the clipboard.
 *
 * @see SRS Section 4: Functional Requirements (FR-015, FR-016)
 * @see SRS Section 7: UI Requirements (UI-008 through UI-010)
 * @see SRS Section 2.4.3: Clipboard Integration
 */

#include <gtk/gtk.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Section 1: Clipboard Copy Operations
 *---------------------------------------------------------------------------
 * Core functions for copying text to the clipboard.
 */

/**
 * Copy text to the system clipboard (CLIPBOARD selection).
 *
 * This function copies the provided text to the standard system clipboard
 * (GDK_SELECTION_CLIPBOARD). The text can be pasted using Ctrl+V in any
 * application.
 *
 * The function uses gtk_clipboard_set_text() which takes ownership of the
 * text data. The GTK clipboard API handles the text encoding (UTF-8) and
 * memory management internally.
 *
 * @param display A GdkDisplay* for the clipboard. Pass NULL to use the
 *                default display.
 * @param text    The text to copy. Must be a valid UTF-8 string. Must not
 *                be NULL. The string is copied internally.
 *
 * @return true if the text was copied successfully, false if the clipboard
 *         is unavailable (should not happen in normal operation).
 *
 * @note This function is non-blocking and does not own the clipboard.
 *       The text remains in the clipboard until overwritten by another
 *       copy operation or the application exits.
 *
 * @see FR-015: Copy to Clipboard
 * @see FR-016: Clipboard Format (plain text, UTF-8)
 * @see UI-009: Clipboard API
 */
bool clipboard_copy_text(GdkDisplay* display, const char* text);

/**
 * Copy text to both CLIPBOARD and PRIMARY selections.
 *
 * This function copies the provided text to both the standard clipboard
 * (Ctrl+V paste) and the primary selection (middle-click paste on X11).
 * This ensures maximum compatibility with user paste habits on X11.
 *
 * On Wayland, only the CLIPBOARD selection is populated (PRIMARY is not
 * supported by the Wayland protocol). The function detects the display
 * protocol and behaves appropriately.
 *
 * @param display A GdkDisplay* for the clipboard. Pass NULL to use the
 *                default display.
 * @param text    The text to copy. Must be a valid UTF-8 string. Must not
 *                be NULL.
 *
 * @return true if the text was copied to at least one selection successfully,
 *         false if both operations failed.
 *
 * @see FR-015: Copy to Clipboard
 * @see UI-009: Clipboard API
 * @see SRS Section 2.4.3: Clipboard Integration (PRIMARY and CLIPBOARD)
 */
bool clipboard_copy_text_both(GdkDisplay* display, const char* text);

/**
 * Copy the transcription text from a GtkTextView to the clipboard.
 *
 * This convenience function retrieves the text content from a GtkTextView
 * widget and copies it to the clipboard. This is the primary function
 * used when the user triggers the "Copy" action.
 *
 * The function performs the following steps:
 *   1. Get the GtkTextBuffer from the GtkTextView
 *   2. Get the full text range from the buffer
 *   3. Copy the text to both CLIPBOARD and PRIMARY selections
 *   4. Return the result
 *
 * @param text_view Pointer to a GtkTextView containing the transcription text.
 *                  Must not be NULL.
 * @return true if the text was copied successfully, false on error.
 *
 * @see FR-015: Copy to Clipboard
 * @see FR-017: Text Area Persistence on Copy (text remains in view after copy)
 */
bool clipboard_copy_from_text_view(GtkTextView* text_view);

/**
 * Clear the clipboard content.
 *
 * This function clears the clipboard content by setting an empty string.
 * This is useful for cleaning up sensitive data before application exit.
 *
 * @param display A GdkDisplay* for the clipboard. Pass NULL for default.
 * @return true if the clipboard was cleared, false on error.
 *
 * @see NR-010: Memory Buffer Scrubbing (clipboard cleanup on exit)
 */
bool clipboard_clear(GdkDisplay* display);

/*---------------------------------------------------------------------------
 * Section 3: Clipboard Utilities
 *---------------------------------------------------------------------------
 * Utility functions for clipboard operations.
 */

/**
 * Check if the clipboard is available and functional.
 *
 * This function attempts to create a GtkClipboard object for the
 * CLIPBOARD selection. If successful, the clipboard is available.
 *
 * @param display A GdkDisplay* for the clipboard. Pass NULL for default.
 * @return true if the clipboard is available, false if unavailable
 *         (should not happen in normal operation).
 */
bool clipboard_is_available(GdkDisplay* display);

/**
 * Get the GtkClipboard object for the CLIPBOARD selection.
 *
 * This function returns a GtkClipboard* for the standard clipboard.
 * The caller takes a reference to the clipboard and is responsible for
 * calling g_object_unref() when done.
 *
 * @param display A GdkDisplay* for the clipboard. Pass NULL for default.
 * @return A newly referenced GtkClipboard*, or NULL on error.
 *         The caller must call g_object_unref() when done.
 */
GtkClipboard* clipboard_get_clipboard(GdkDisplay* display);

/**
 * Get the GtkClipboard object for the PRIMARY selection.
 *
 * This function returns a GtkClipboard* for the primary selection.
 * The caller takes a reference to the clipboard and is responsible for
 * calling g_object_unref() when done.
 *
 * On Wayland, this function may return NULL or a non-functional clipboard,
 * as the PRIMARY selection is not supported by the Wayland protocol.
 *
 * @param display A GdkDisplay* for the clipboard. Pass NULL for default.
 * @return A newly referenced GtkClipboard*, or NULL on error / Wayland.
 *         The caller must call g_object_unref() when done.
 */
GtkClipboard* clipboard_get_primary(GdkDisplay* display);

/*---------------------------------------------------------------------------
 * Section 4: Error Handling
 *---------------------------------------------------------------------------
 * Functions for retrieving error information.
 */

/**
 * Get the last error message from the clipboard module.
 *
 * @return A null-terminated string containing the error message, or an
 *         empty string if no error has occurred. The returned string is
 *         static and must NOT be freed.
 */
const char* clipboard_get_error(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CLIPBOARD_H */
