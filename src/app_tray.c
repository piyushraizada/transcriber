/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/**
 * @file app_tray.c
 * @brief System Tray Icon — libappindicator integration for notification area presence
 *
 * This module implements the system tray (notification area) icon for the
 * Transcriber application. It uses libappindicator (Ayatana fork preferred,
 * with fallback to original libappindicator) to create a StatusNotifierItem
 * that appears in the desktop environment's system tray / notification area.
 *
 * Features:
 *   - State-aware icon (red mic for IDLE, green mic for LISTENING/TRANSCRIBING)
 *   - Dynamic tooltip reflecting current state and connection status
 *   - Right-click context menu with Toggle Recording, Show Window, Quit
 *
 * Icon Handling:
 *   The embedded XPM arrays (from CMake) are converted to GdkPixbuf, then saved
 *   as temporary PNG files in the system temp directory for AppIndicator
 *   consumption. These temp files are cleaned up in tray_destroy().
 *
 * Context Menu:
 *   Built with GtkMenu (GTK3). Items are:
 *     - "Toggle Recording" — calls the registered toggle callback
 *     - "Show Window"      — presents the main GtkWindow
 *     - "Quit"             — calls gtk_main_quit()
 *
 * @see app_tray.h for public API documentation
 * @see plans/system-tray-icon.md for full design documentation
 */

#define _POSIX_C_SOURCE 200809L

#include "app_tray.h"
#include "app.h"

#include <glib.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <libayatana-appindicator/app-indicator.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>

/* Embedded XPM assets */
#include "assets/greenmic.xpm"
#include "assets/redmic.xpm"

/* ------------------------------------------------------------------ */
/* Internal constants                                                  */
/* ------------------------------------------------------------------ */

/** Reverse-DNS style indicator ID for uniqueness. */
#define TRAY_ID "com.example.transcriber"

/** Tooltip strings per state. */
#define TOOLTIP_IDLE_CONNECTED    "Transcriber — Ready"
#define TOOLTIP_IDLE_DISCONNECTED "Transcriber — Model unavailable"
#define TOOLTIP_LISTENING         "Transcriber — Recording..."
#define TOOLTIP_TRANSCRIBING      "Transcriber — Transcribing..."
#define TOOLTIP_LOADING           "Transcriber — Loading model..."

/** Temp file base names (combined with g_get_tmp_dir() at runtime). */
#define TRAY_ICON_IDLE_NAME     "transcriber_tray_idle"
#define TRAY_ICON_LISTENING_NAME "transcriber_tray_listening"

/* ------------------------------------------------------------------ */
/* Internal structures                                                 */
/* ------------------------------------------------------------------ */

struct _SystemTray {
    AppIndicator *indicator;      /* The AppIndicator handle */
    GtkMenu *menu;                /* Right-click context menu */
    GtkWindow *main_window;       /* Reference to main window for "Show" */
    AppState current_state;       /* Current application state */
    ModelStatus model_status; /* Current connection status */
    /* Toggle callback — invoked when "Toggle Recording" is selected */
    void (*on_toggle)(void *user_data);
    void *toggle_user_data;
    /* Dynamic temp icon directory and file paths */
    char icon_dir[PATH_MAX];
    char icon_idle_path[PATH_MAX];
    char icon_listening_path[PATH_MAX];
    /* Icon names for app_indicator_set_icon() (basename without extension) */
    char icon_idle_name[64];
    char icon_listening_name[64];
    /* Track which temp icon files were created for cleanup */
    bool icon_idle_created;
    bool icon_listening_created;
    /* Sine wave animation state */
    guint animation_source_id;    /* Timer source ID (0 = not running) */
    guint animation_frame;        /* Current frame index */
    char anim_icon_paths[8][PATH_MAX];  /* Animated frame file paths */
    char anim_icon_names[8][64];   /* Animated frame icon names */
    GdkPixbuf *anim_pixbufs[8];   /* Pre-loaded pixbufs for each frame (avoids disk I/O in timer) */
    guint num_anim_frames;        /* Number of animation frames created */
    bool anim_icons_created;      /* TRUE if animation frames were generated */
};

/* ------------------------------------------------------------------ */
/* Static helper function declarations                                 */
/* ------------------------------------------------------------------ */

static GdkPixbuf *load_xpm_for_tray(const char *filename);
static gboolean save_pixbuf_as_png(GdkPixbuf *pixbuf, const char *path);
static void ensure_icon_files(SystemTray *tray);
static void update_tray_icon(SystemTray *tray);
static void update_tray_tooltip(SystemTray *tray);
static void build_context_menu(SystemTray *tray);

/* Menu item callbacks */
static void on_menu_toggle(GtkMenuItem *item, gpointer user_data);
static void on_menu_show(GtkMenuItem *item, gpointer user_data);
static void on_menu_quit(GtkMenuItem *item, gpointer user_data);

/* Animation helpers */
static void ensure_animation_frames(SystemTray *tray);
static gboolean animation_tick_tray(gpointer user_data);
static void set_dock_icon_from_pixbuf(GtkWindow *win, GdkPixbuf *pixbuf);

/* ------------------------------------------------------------------ */
/* Icon loading and PNG conversion                                     */
/* ------------------------------------------------------------------ */

/**
 * Load an XPM icon into a GdkPixbuf using the embedded arrays.
 * Falls back to loading from the assets/ directory on disk.
 */
static GdkPixbuf *load_xpm_for_tray(const char *filename) {
    GdkPixbuf *pixbuf = NULL;

#ifdef XPM_STATICALLY_EMBEDDED
    if (strcmp(filename, "greenmic.xpm") == 0) {
        pixbuf = gdk_pixbuf_new_from_xpm_data(GREENMIC_XPM);
    } else if (strcmp(filename, "redmic.xpm") == 0) {
        pixbuf = gdk_pixbuf_new_from_xpm_data(REDMIC_XPM);
    }
#else
    /* Fallback: load from disk */
    char path[PATH_MAX];
    /* Try relative to executable */
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        char *dir = dirname(exe_path);
        snprintf(path, sizeof(path), "%s/assets/%s", dir, filename);
        pixbuf = gdk_pixbuf_new_from_file(path, NULL);
    }
    /* Try relative to current directory */
    if (!pixbuf) {
        snprintf(path, sizeof(path), "./assets/%s", filename);
        pixbuf = gdk_pixbuf_new_from_file(path, NULL);
    }
#endif

    return pixbuf;
}

/**
 * Save a GdkPixbuf as a PNG file to the given path.
 *
 * @return TRUE on success, FALSE on failure (with error logged to stderr).
 */
static gboolean save_pixbuf_as_png(GdkPixbuf *pixbuf, const char *path) {
    GError *error = NULL;
    gboolean ok = gdk_pixbuf_save(pixbuf, path, "png", &error, NULL);
    if (!ok) {
        /* CR-02 fix: 'error' may be NULL if gdk_pixbuf_save fails without setting GError */
        g_warning("[tray] Failed to save icon to %s: %s",
                  path, error ? error->message : "unknown error");
        g_error_free(error);
    }
    return ok;
}

/**
 * Ensure the temporary PNG icon files exist on disk.
 * Converts embedded XPM -> GdkPixbuf -> PNG for AppIndicator use.
 * Icons are saved in the tray->icon_dir directory with names like
 * "transcriber_tray_idle.png" so app_indicator_set_icon() can find them
 * via the icon theme path set by app_indicator_new_with_path().
 */
static void ensure_icon_files(SystemTray *tray) {
    /* Create IDLE icon (red mic) */
    if (!tray->icon_idle_created) {
        GdkPixbuf *pixbuf = load_xpm_for_tray("redmic.xpm");
        if (pixbuf && save_pixbuf_as_png(pixbuf, tray->icon_idle_path)) {
            tray->icon_idle_created = true;
        }
        if (pixbuf) {
            g_object_unref(pixbuf);
        }
    }

    /* Create LISTENING/TRANSCRIBING icon (green mic) */
    if (!tray->icon_listening_created) {
        GdkPixbuf *pixbuf = load_xpm_for_tray("greenmic.xpm");
        if (pixbuf && save_pixbuf_as_png(pixbuf, tray->icon_listening_path)) {
            tray->icon_listening_created = true;
        }
        if (pixbuf) {
            g_object_unref(pixbuf);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Context menu                                                        */
/* ------------------------------------------------------------------ */

/**
 * Build the right-click context menu for the tray icon.
 */
static void build_context_menu(SystemTray *tray) {
    tray->menu = GTK_MENU(gtk_menu_new());

    /* "Toggle Recording" menu item */
    GtkWidget *toggle_item = gtk_menu_item_new_with_label("Toggle Recording");
    g_signal_connect_swapped(toggle_item, "activate",
                             G_CALLBACK(on_menu_toggle), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), toggle_item);

    /* "Show Window" menu item */
    GtkWidget *show_item = gtk_menu_item_new_with_label("Show Window");
    g_signal_connect_swapped(show_item, "activate",
                             G_CALLBACK(on_menu_show), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), show_item);

    /* Separator */
    GtkWidget *separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), separator);

    /* "Quit" menu item */
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect_swapped(quit_item, "activate",
                             G_CALLBACK(on_menu_quit), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), quit_item);

    /* Show all menu items */
    gtk_widget_show_all(GTK_WIDGET(tray->menu));

    /* Attach menu to the indicator */
    app_indicator_set_menu(tray->indicator, GTK_MENU(tray->menu));
}

/* ------------------------------------------------------------------ */
/* Menu item callback implementations                                  */
/* ------------------------------------------------------------------ */

static void on_menu_toggle(GtkMenuItem *item, gpointer user_data) {
    UNUSED(item);
    SystemTray *tray = (SystemTray *)user_data;
    if (tray->on_toggle) {
        tray->on_toggle(tray->toggle_user_data);
    }
}

static void on_menu_show(GtkMenuItem *item, gpointer user_data) {
    UNUSED(item);
    SystemTray *tray = (SystemTray *)user_data;
    if (tray->main_window) {
        gtk_window_present(tray->main_window);
    }
}

static void on_menu_quit(GtkMenuItem *item, gpointer user_data) {
    UNUSED(item);
    UNUSED(user_data);
    gtk_main_quit();
}

/* ------------------------------------------------------------------ */
/* Icon and tooltip updates                                            */
/* ------------------------------------------------------------------ */

/**
 * Update the tray icon based on current state.
 * Uses icon names (not full paths) with the custom icon theme path
 * set by app_indicator_new_with_path().
 */
static void update_tray_icon(SystemTray *tray) {
    ensure_icon_files(tray);

    const char *icon_name = NULL;
    switch (tray->current_state) {
        case STATE_LISTENING:
        case STATE_TRANSCRIBING:
            if (tray->icon_listening_created) {
                icon_name = tray->icon_listening_name;
            }
            break;
        case STATE_IDLE:
        default:
            if (tray->icon_idle_created) {
                icon_name = tray->icon_idle_name;
            }
            break;
    }

    if (icon_name) {
        app_indicator_set_icon(tray->indicator, icon_name);
    }
}

/**
 * Update the tray tooltip and label based on current state and connection status.
 */
static void update_tray_tooltip(SystemTray *tray) {
    const char *tooltip;
    AppIndicatorStatus status;

    switch (tray->current_state) {
        case STATE_LISTENING:
            tooltip = TOOLTIP_LISTENING;
            /* Use ACTIVE status so the dock icon stays still, allowing the
             * sine wave animation frames to be visible without pulse/bounce. */
            status = APP_INDICATOR_STATUS_ACTIVE;
            break;
        case STATE_TRANSCRIBING:
            tooltip = TOOLTIP_TRANSCRIBING;
            /* ACTIVE status — icon remains steady during transcription. */
            status = APP_INDICATOR_STATUS_ACTIVE;
            break;
        case STATE_IDLE:
        default:
            if (tray->model_status == MODEL_LOADING) {
                tooltip = TOOLTIP_LOADING;
            } else if (tray->model_status == MODEL_AVAILABLE) {
                tooltip = TOOLTIP_IDLE_CONNECTED;
            } else {
                tooltip = TOOLTIP_IDLE_DISCONNECTED;
            }
            /* Normal active status when idle — no pulse. */
            status = APP_INDICATOR_STATUS_ACTIVE;
            break;
    }

    /* Set the label (tooltip) on the indicator.
     * The third argument (secondary_label) is NULL — we only need the main label. */
    app_indicator_set_label(tray->indicator, tooltip, NULL);

    /* Set status: ACTIVE for normal display, ATTENTION for pulse/bounce in dock.
     * This is what makes the GNOME dock icon visibly indicate recording state. */
    app_indicator_set_status(tray->indicator, status);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

SystemTray *tray_create(void) {
    SystemTray *tray = calloc(1, sizeof(SystemTray));
    if (!tray) {
        return NULL;
    }

    tray->current_state = STATE_IDLE;
    tray->model_status = MODEL_UNAVAILABLE;
    tray->main_window = NULL;
    tray->on_toggle = NULL;
    tray->toggle_user_data = NULL;
    tray->icon_idle_created = false;
    tray->icon_listening_created = false;
    tray->animation_source_id = 0;
    tray->animation_frame = 0;
    tray->num_anim_frames = 0;
    tray->anim_icons_created = false;
    for (guint i = 0; i < 8; i++) {
        tray->anim_pixbufs[i] = NULL;
    }

    /* Create a temp directory for icon files so app_indicator_new_with_path()
     * can find them. This is required because app_indicator_set_icon() expects
     * an icon name (not a full path) and searches the icon theme path. */
    snprintf(tray->icon_dir, sizeof(tray->icon_dir),
             "%s/transcriber_icons_%d", g_get_tmp_dir(), getpid());
    mkdir(tray->icon_dir, 0755);

    /* Build icon file paths within the temp directory */
    g_snprintf(tray->icon_idle_path, sizeof(tray->icon_idle_path),
               "%s/%s.png", tray->icon_dir, TRAY_ICON_IDLE_NAME);
    g_snprintf(tray->icon_listening_path, sizeof(tray->icon_listening_path),
               "%s/%s.png", tray->icon_dir, TRAY_ICON_LISTENING_NAME);

    /* Icon names (basename without extension) for app_indicator_set_icon() */
    snprintf(tray->icon_idle_name, sizeof(tray->icon_idle_name), "%s", TRAY_ICON_IDLE_NAME);
    snprintf(tray->icon_listening_name, sizeof(tray->icon_listening_name), "%s", TRAY_ICON_LISTENING_NAME);

    /* Build animation frame paths (after icon_dir is set) */
    for (guint i = 0; i < 8; i++) {
        g_snprintf(tray->anim_icon_paths[i], sizeof(tray->anim_icon_paths[i]),
                   "%s/transcriber_anim_%d.png", tray->icon_dir, i);
        g_snprintf(tray->anim_icon_names[i], sizeof(tray->anim_icon_names[i]),
                   "transcriber_anim_%d", i);
    }

    /* Create the icon PNG files BEFORE app_indicator_new_with_path().
     * The AppIndicator constructor attempts to load the initial icon
     * immediately. If the file doesn't exist, internal GTK widgets end up
     * in an invalid (NULL) state, causing a cascade of G_IS_OBJECT and
     * GTK_IS_CELL_RENDERER assertion failures followed by a segfault. */
    ensure_icon_files(tray);

    /* Create the AppIndicator with custom icon theme path */
    tray->indicator = app_indicator_new_with_path(TRAY_ID, tray->icon_idle_name,
                                           APP_INDICATOR_CATEGORY_APPLICATION_STATUS,
                                           tray->icon_dir);
    if (!tray->indicator) {
        g_log("app-tray", G_LOG_LEVEL_MESSAGE, "[tray] Failed to create AppIndicator\n");
        rmdir(tray->icon_dir);
        free(tray);
        return NULL;
    }

    /* Build the context menu */
    build_context_menu(tray);

    /* Set initial icon and status */
    update_tray_icon(tray);
    update_tray_tooltip(tray);

    return tray;
}

void tray_destroy(SystemTray *tray) {
    if (!tray) return;

    /* Stop animation timer if running */
    if (tray->animation_source_id != 0) {
        g_source_remove(tray->animation_source_id);
        tray->animation_source_id = 0;
    }

    /* Remove temporary icon files */
    if (tray->icon_idle_created) {
        unlink(tray->icon_idle_path);
    }
    if (tray->icon_listening_created) {
        unlink(tray->icon_listening_path);
    }

    /* Free pre-loaded animation pixbufs */
    for (guint i = 0; i < tray->num_anim_frames; i++) {
        if (tray->anim_pixbufs[i]) {
            g_object_unref(tray->anim_pixbufs[i]);
            tray->anim_pixbufs[i] = NULL;
        }
    }

    /* Remove animation frame files */
    for (guint i = 0; i < tray->num_anim_frames; i++) {
        unlink(tray->anim_icon_paths[i]);
    }

    /* Remove temporary icon directory */
    rmdir(tray->icon_dir);

    /* Free GTK resources */
    if (tray->menu) {
        gtk_widget_destroy(GTK_WIDGET(tray->menu));
    }

    /* Free the indicator (this is a GObject) */
    if (tray->indicator) {
        g_object_unref(tray->indicator);
    }

    free(tray);
}

void tray_set_state(SystemTray *tray, AppState state) {
    if (!tray) return;

    tray->current_state = state;
    update_tray_icon(tray);
    update_tray_tooltip(tray);
}

void tray_set_model_status(SystemTray *tray, ModelStatus status) {
    if (!tray) return;

    tray->model_status = status;
    /* Only update tooltip if in IDLE state (connection matters most then) */
    if (tray->current_state == STATE_IDLE) {
        update_tray_tooltip(tray);
    }
}

void tray_set_main_window(SystemTray *tray, GtkWindow *win) {
    if (!tray) return;
    tray->main_window = win;
}

void tray_set_toggle_callback(SystemTray *tray,
                              void (*callback)(void *user_data),
                              void *user_data) {
    if (!tray) return;
    tray->on_toggle = callback;
    tray->toggle_user_data = user_data;
}

/* ------------------------------------------------------------------ */
/* Sine wave animation on dock icon                                    */
/* ------------------------------------------------------------------ */

/**
 * Animation constants — match the MainWindow sine wave for visual consistency.
 */
#define TRAY_ANIM_FRAMES       8
#define TRAY_ANIM_INTERVAL_MS  33  /* ~30 fps */
#define TRAY_WAVE_AMPLITUDE    20.0
#define TRAY_WAVE_FREQUENCY    0.05
#define TRAY_WAVE_COLOR_R      0.165
#define TRAY_WAVE_COLOR_G      0.655
#define TRAY_WAVE_COLOR_B      0.259
#define TRAY_WAVE_COLOR_A      0.7
#define TRAY_WAVE_LINE_WIDTH   6.0

/**
 * Timer callback for the tray icon sine wave animation.
 * Cycles through pre-generated frames, updating both tray and window (dock) icon.
 */
static gboolean animation_tick_tray(gpointer user_data) {
    SystemTray *tray = (SystemTray *)user_data;
    tray->animation_frame = (tray->animation_frame + 1) % tray->num_anim_frames;
    app_indicator_set_icon(tray->indicator, tray->anim_icon_names[tray->animation_frame]);
    /* Update the window (dock) icon with the current animation frame.
     * Use pre-loaded pixbuf to avoid slow disk I/O on every timer tick.
     * Use set_dock_icon_from_pixbuf() which sets _NET_WM_ICON via Xlib,
     * ensuring the dock sees the animated frames. */
    if (tray->main_window && tray->anim_pixbufs[tray->animation_frame]) {
        set_dock_icon_from_pixbuf(tray->main_window, tray->anim_pixbufs[tray->animation_frame]);
    }
    return TRUE; /* Continue the timer */
}

/**
 * Set the dock/taskbar icon from a GdkPixbuf by writing the _NET_WM_ICON
 * X11 property directly. This is required because gtk_window_set_icon()
 * may be cached or ignored by some desktop environments (e.g., GNOME Shell
 * dock) for already-running applications.
 */
static void set_dock_icon_from_pixbuf(GtkWindow *win, GdkPixbuf *pixbuf) {
    GdkWindow *gdk_win = gtk_widget_get_window(GTK_WIDGET(win));
    if (!gdk_win || !GDK_IS_X11_WINDOW(gdk_win)) return;

    Window xwin = gdk_x11_window_get_xid(gdk_win);
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_window_get_display(gdk_win));
    Atom net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", True);
    if (net_wm_icon == None) return;

    int w = gdk_pixbuf_get_width(pixbuf);
    int h = gdk_pixbuf_get_height(pixbuf);
    int n_channels = gdk_pixbuf_get_n_channels(pixbuf);
    guint8 *pixels = gdk_pixbuf_get_pixels(pixbuf);
    guint rowstride = gdk_pixbuf_get_rowstride(pixbuf);

    /* _NET_WM_ICON format: [width, height, pixel_data...] as CARDINAL (32-bit) values.
     * Each pixel is a single 32-bit value in ARGB format: 0xAARRGGBB. */
    long *data = g_new(long, 2 + w * h);
    gint i, j, k = 0;
    data[k++] = w;
    data[k++] = h;
    for (i = 0; i < h; i++) {
        for (j = 0; j < w; j++) {
            guint8 *p = pixels + i * rowstride + j * n_channels;
            if (n_channels == 4) {
                data[k++] = ((long)p[3] << 24) | ((long)p[0] << 16) | ((long)p[1] << 8) | (long)p[2];
            } else {
                /* 3 channels (RGB) — assume fully opaque */
                data[k++] = 0xFF000000UL | ((long)p[0] << 16) | ((long)p[1] << 8) | (long)p[2];
            }
        }
    }

    XChangeProperty(dpy, xwin, net_wm_icon, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)data, k);
    XFlush(dpy);
    g_free(data);
}

/**
 * Pre-generate animation frames by compositing a sine wave onto the green mic.
 * Each frame uses a different phase offset of the sine wave.
 */
static void ensure_animation_frames(SystemTray *tray) {
    if (tray->anim_icons_created) {
        return;
    }

    /* Load the green mic as base */
    GdkPixbuf *base = load_xpm_for_tray("greenmic.xpm");
    if (!base) {
        return;
    }

    int w = gdk_pixbuf_get_width(base);
    int h = gdk_pixbuf_get_height(base);
    guint n_channels = gdk_pixbuf_get_n_channels(base); /* 3 or 4 */

    /* Phase increment per frame: full cycle over TRAY_ANIM_FRAMES frames */
    double phase_inc = (2.0 * G_PI) / TRAY_ANIM_FRAMES;
    guint frames_created = 0;

    for (guint f = 0; f < TRAY_ANIM_FRAMES; f++) {
        double phase = f * phase_inc;

        /* Create a mutable copy of the base icon */
        GdkPixbuf *frame = gdk_pixbuf_copy(base);
        guint8 *fp = gdk_pixbuf_get_pixels(frame);
        guint f_rowstride = gdk_pixbuf_get_rowstride(frame);

        /* Draw sine wave onto the frame pixels */
        double center_y = h / 2.0;
        guint wave_half = (guint)(TRAY_WAVE_LINE_WIDTH / 2);

        for (int x = 0; x < w; x++) {
            double y = center_y + TRAY_WAVE_AMPLITUDE * sin(TRAY_WAVE_FREQUENCY * x + phase);

            /* Draw line segment around the sine wave */
            for (int dy = -(int)wave_half; dy <= (int)wave_half; dy++) {
                int py = (int)y + dy;
                if (py >= 0 && py < h) {
                    guint offset = py * f_rowstride + x * n_channels;
                    /* Apply wave color with alpha blending over existing pixel */
                    guint8 r = fp[offset];
                    guint8 g = fp[offset + 1];
                    guint8 b = fp[offset + 2];
                    double a = TRAY_WAVE_COLOR_A;
                    fp[offset]     = (guint8)(r * (1.0 - a) + TRAY_WAVE_COLOR_R * 255.0 * a);
                    fp[offset + 1] = (guint8)(g * (1.0 - a) + TRAY_WAVE_COLOR_G * 255.0 * a);
                    fp[offset + 2] = (guint8)(b * (1.0 - a) + TRAY_WAVE_COLOR_B * 255.0 * a);
                }
            }
        }

        /* Save frame to temp PNG (needed for tray icon via app_indicator_set_icon) */
        if (save_pixbuf_as_png(frame, tray->anim_icon_paths[f])) {
            /* Pre-load the pixbuf into memory to avoid disk I/O on every timer tick.
             * gdk_pixbuf_copy() creates a new reference we own. */
            tray->anim_pixbufs[frames_created] = gdk_pixbuf_copy(frame);
            frames_created++;
        }
        g_object_unref(frame);
    }

    g_object_unref(base);

    if (frames_created > 0) {
        tray->num_anim_frames = frames_created;
        tray->anim_icons_created = true;
    }
}

/**
 * Start the sine wave animation on the tray icon.
 * Pre-generates frames then starts the timer.
 */
void tray_start_animation(SystemTray *tray) {
    if (!tray) return;

    /* Don't start if already running */
    if (tray->animation_source_id != 0) {
        return;
    }

    ensure_animation_frames(tray);
    if (tray->num_anim_frames == 0) {
        return; /* No frames generated, skip animation */
    }

    tray->animation_frame = 0;
    /* Show first frame immediately on both tray and window (dock) icon.
     * Use pre-loaded pixbuf + set_dock_icon_from_pixbuf() for dock visibility. */
    app_indicator_set_icon(tray->indicator, tray->anim_icon_names[0]);
    if (tray->main_window && tray->anim_pixbufs[0]) {
        set_dock_icon_from_pixbuf(tray->main_window, tray->anim_pixbufs[0]);
    }
    tray->animation_source_id = g_timeout_add(TRAY_ANIM_INTERVAL_MS,
                                               animation_tick_tray,
                                               tray);
}

/**
 * Stop the sine wave animation on the tray icon.
 * Stops the timer and reverts to the static green mic icon.
 */
void tray_stop_animation(SystemTray *tray) {
    if (!tray) return;

    if (tray->animation_source_id != 0) {
        g_source_remove(tray->animation_source_id);
        tray->animation_source_id = 0;
    }

    /* Revert to static green mic on both tray and window (dock) icon */
    if (tray->icon_listening_created) {
        app_indicator_set_icon(tray->indicator, tray->icon_listening_name);
    }
    if (tray->main_window) {
        GdkPixbuf *green = gdk_pixbuf_new_from_file(tray->icon_listening_path, NULL);
        if (green) {
            set_dock_icon_from_pixbuf(tray->main_window, green);
            g_object_unref(green);
        }
    }
}
