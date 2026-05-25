/*
 * app_config_dialog.c — Configuration Dialog UI implementation
 *
 * Modal GTK3 settings window with all configurable fields.
 */

#include "app_config_dialog.h"
#include "app_config.h"
#include "app_audio.h"
#include "app_whisper.h"
#include "app_model_info.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Default model file distributed with the application package */
#define DEFAULT_MODEL_DIR  "~/.cache/whisper"
#define DEFAULT_MODEL_FILE "ggml-large-v3-turbo-q8_0.bin"

/* Forward declaration for WhisperClient */
struct _WhisperClient;
typedef struct _WhisperClient WhisperClient;

/* ===================================================================
 * Internal dialog state
 * =================================================================== */

struct _ConfigDialog {
    GtkDialog *dialog;
    AppConfig *config;
    WhisperClient *whisper_client;

    /* Widgets */
    GtkEntry *model_path_entry;
    GtkButton *model_browse_button;
    GtkLabel *model_info_label;
    GtkComboBox *device_combo;
    /* MIN-002 fix: Removed language_combo - multilingual models only */
    GtkSpinButton *duration_spin;
    GtkCheckButton *notifications_check;
    GtkLabel *model_path_error;
    GtkLabel *duration_error;
    GtkLabel *hotkey_label;
    GtkLabel *test_status_label;

    /* Async model info loading */
    guint model_info_idle_id;  /* Track idle source so we can cancel on dialog close */
};

/* MIN-002 fix: Removed dead language_combo and g_languages array.
 * Language selection was removed - using multilingual models only. */

/* ===================================================================
 * Default model path helpers
 * =================================================================== */

/**
 * Get the full default model path, expanding ~ to $HOME.
 * Returns a statically allocated buffer (do NOT free).
 */
const char* config_dialog_get_default_model_path(void) {
    static char path[1024];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s/.cache/whisper/%s", home, DEFAULT_MODEL_FILE);
    } else {
        snprintf(path, sizeof(path), "%s/%s", DEFAULT_MODEL_DIR, DEFAULT_MODEL_FILE);
    }
    return path;
}

/**
 * Check if the default model file exists and is a regular file.
 */
bool config_dialog_default_model_exists(void) {
    const char *path = config_dialog_get_default_model_path();
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

/**
 * Validate that a path points to a valid Whisper model (GGML or GGUF).
 * Uses whisper.cpp's own model loader for authoritative validation.
 */
bool config_dialog_validate_gguf_model(const char *path) {
    if (!path || path[0] == '\0') return false;
    return whisper_validate_model_file(path);
}

/* ===================================================================
 * Validation functions
 * =================================================================== */

/**
 * config_dialog_validate_model_path
 * Validates a Whisper model path.
 *
 * @param path Model path string to validate
 * @return true if valid (non-empty, length < 512)
 */
bool config_dialog_validate_model_path(const char *path) {
    if (!path) return false;

    size_t len = strlen(path);
    if (len == 0 || len >= 512) return false;

    return true;
}

/**
 * config_dialog_validate_duration
 * Validates max recording duration.
 *
 * @param duration Duration in seconds
 * @return true if valid (5 <= duration <= 30)
 */
bool config_dialog_validate_duration(int duration) {
    return duration >= 5 && duration <= 30;
}

/**
 * config_dialog_show_error
 * Displays an error message in a label.
 *
 * @param error_label GtkLabel for error display
 * @param message Error message text
 */
void config_dialog_show_error(GtkLabel *error_label, const char *message) {
    if (!error_label) return;

    /* M6-001 fix: Use markup instead of creating/destroying CSS providers repeatedly */
    if (message && message[0]) {
        char *escaped = g_markup_escape_text(message, -1);
        char *markup = g_strdup_printf("<span foreground='red'>%s</span>", escaped);
        gtk_label_set_markup(error_label, markup);
        g_free(escaped);
        g_free(markup);
    } else {
        gtk_label_set_text(error_label, "");
    }
    gtk_widget_set_visible(GTK_WIDGET(error_label), TRUE);
}

/**
 * config_dialog_clear_error
 * Clears an error message from a label.
 *
 * @param error_label GtkLabel for error display
 */
void config_dialog_clear_error(GtkLabel *error_label) {
    if (!error_label) return;

    gtk_label_set_text(error_label, "");
    gtk_widget_set_visible(GTK_WIDGET(error_label), FALSE);
}

/* ===================================================================
 * Audio device list
 * =================================================================== */

/**
 * config_dialog_get_audio_devices
 * Queries the audio backend for available input devices.
 *
 * @param backend Audio backend to query
 * @return GtkListStore* of device names (caller must unref), or NULL
 */
GtkListStore * config_dialog_get_audio_devices(AudioBackend backend) {
    (void)backend;

    /* Two-column store: col 0 = display name, col 1 = device name */
    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);

    /* Use the audio module's device enumeration instead of duplicating ALSA logic.
     * audio_recorder_get_device_list() handles all ALSA hint enumeration, capture
     * device filtering, and open-testing internally.
     * Returns both user-friendly display names and raw ALSA device names. */
    AudioDeviceList *dev_list = audio_recorder_get_device_list(NULL);

    if (dev_list && dev_list->count > 0) {
        for (gint i = 0; i < dev_list->count; i++) {
            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                0, dev_list->display_names[i],  /* friendly name */
                1, dev_list->device_names[i],   /* ALSA device name */
                -1);
        }
        audio_device_list_free(dev_list);
    }

    return store;
}

/* MIN-002 fix: Removed language list section — language selection removed. */

/* ===================================================================
 * Hotkey command display
 * =================================================================== */

/**
 * config_dialog_get_hotkey_command
 * Returns the D-Bus hotkey command string.
 *
 * @return Static string (do NOT free)
 */
const char * config_dialog_get_hotkey_command(void) {
    return "dbus-send --session --type=method_call "
           "--dest=org.xvoice.Controller "
           "/org/xvoice/App "
           "org.xvoice.Actions.Toggle";
}

/* ===================================================================
 * Window position reset
 * =================================================================== */

/**
 * config_dialog_reset_window_position
 * Resets window position to defaults (-1, -1).
 *
 * @param config AppConfig struct to update
 */
void config_dialog_reset_window_position(struct _AppConfig *config) {
    if (!config) return;
    config->window_x = -1;
    config->window_y = -1;
}

/* ===================================================================
 * Signal callbacks
 * =================================================================== */

static void on_save_clicked(GtkButton *button, ConfigDialog *dlg) {
    (void)button;

    gboolean valid = TRUE;

    /* Validate Model Path */
    const char *model_path = gtk_entry_get_text(dlg->model_path_entry);
    
    if (!config_dialog_validate_model_path(model_path)) {
        config_dialog_show_error(dlg->model_path_error, "No valid whisper ggml file found");
        valid = FALSE;
    } else {
        /* Check file exists */
        struct stat st;
        if (stat(model_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            config_dialog_show_error(dlg->model_path_error, "No valid whisper ggml file found");
            valid = FALSE;
        } else {
            /* Validate GGUF/GGML format by attempting to load metadata */
            ModelInfo info;
            model_info_init(&info);
            if (!model_info_load(model_path, &info) || !info.valid) {
                config_dialog_show_error(dlg->model_path_error, "No valid whisper ggml file found");
                valid = FALSE;
            } else {
                config_dialog_clear_error(dlg->model_path_error);
            }
        }
    }

    /* Validate duration */
    int duration = (int)gtk_spin_button_get_value(dlg->duration_spin);
    if (!config_dialog_validate_duration(duration)) {
        config_dialog_show_error(dlg->duration_error, "Duration must be between 5 and 30 seconds");
        valid = FALSE;
    } else {
        config_dialog_clear_error(dlg->duration_error);
    }

    if (!valid) {
        return;  /* Keep dialog open */
    }

    /* Apply values to config */
    config_set_model_path(dlg->config, model_path);

    /* Get selected audio device — column 0 = display name, column 1 = device name */
    GtkTreeIter device_iter;
    GtkTreeModel *device_model = GTK_TREE_MODEL(gtk_combo_box_get_model(dlg->device_combo));
    if (gtk_combo_box_get_active_iter(dlg->device_combo, &device_iter)) {
        gchar *display_name = NULL;
        gchar *device_name = NULL;
        gtk_tree_model_get(device_model, &device_iter, 0, &display_name, 1, &device_name, -1);
        if (device_name) {
            if (g_strcmp0(device_name, "default") == 0) {
                config_set_audio_device(dlg->config, "");
            } else {
                config_set_audio_device(dlg->config, device_name);
            }
            g_free(device_name);
        }
        if (display_name) {
            config_set_audio_device_display_name(dlg->config, display_name);
            g_free(display_name);
        }
    }

    /* Set duration */
    config_set_max_duration(dlg->config, duration);

    /* Set notifications */
    gboolean notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dlg->notifications_check));
    config_set_notifications(dlg->config, notifications);

    /* Save config to file */
    config_save(dlg->config);

    /* Close dialog with response ID GTK_RESPONSE_OK */
    gtk_dialog_response(dlg->dialog, GTK_RESPONSE_OK);
}

static void on_cancel_clicked(GtkButton *button, ConfigDialog *dlg) {
    (void)button;
    gtk_dialog_response(dlg->dialog, GTK_RESPONSE_CANCEL);
}


static void on_reset_position_clicked(GtkButton *button, ConfigDialog *dlg) {
    (void)button;
    config_dialog_reset_window_position(dlg->config);
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
}

/* FIX #2: Async model info loading to avoid blocking GTK main loop */
typedef struct {
    ConfigDialog *dlg;
    char *path;
    guint idle_id;  /* Store the idle source ID so we can track/cancel it */
} ModelInfoLoadData;

static gboolean model_info_load_callback(gpointer user_data) {
    ModelInfoLoadData *data = (ModelInfoLoadData *)user_data;
    ConfigDialog *dlg = data->dlg;
    char *path = data->path;  /* Non-const to allow g_free */

    /* Safety check: verify the dialog widget is still alive before accessing it.
     * This prevents use-after-free if the dialog was closed before the callback ran. */
    if (!GTK_IS_WIDGET(GTK_WIDGET(dlg->dialog)) ||
        !gtk_widget_get_realized(GTK_WIDGET(dlg->dialog))) {
        g_free(path);
        g_free(data);
        return FALSE;
    }

    /* Load model metadata */
    ModelInfo info;
    model_info_init(&info);

    if (model_info_load(path, &info)) {
        if (info.valid) {
            /* MIN-006 fix: Escape model metadata for Pango markup safety */
            gchar *escaped_name = g_markup_escape_text(info.model_name, -1);
            gchar *escaped_quant = g_markup_escape_text(info.quantization, -1);
            const char *lang_str = info.multilingual ? "Multilingual" : "English-only";
            char markup[512];
            snprintf(markup, sizeof(markup),
                "<span size='small'><b>%s</b> — %s — %s</span>",
                escaped_name, escaped_quant, lang_str);
            gtk_label_set_markup(dlg->model_info_label, markup);
            g_free(escaped_name);
            g_free(escaped_quant);
        } else {
            /* Show user-friendly error for invalid models */
            gtk_label_set_markup(dlg->model_info_label,
                "<span foreground='red' size='small'>No valid whisper ggml file found</span>");
        }
    } else {
        gtk_label_set_markup(dlg->model_info_label,
            "<span foreground='red' size='small'>No valid whisper ggml file found</span>");
    }

    /* Clear the tracked idle ID since this callback just ran */
    if (dlg->model_info_idle_id == data->idle_id) {
        dlg->model_info_idle_id = 0;
    }

    g_free(path);
    g_free(data);
    return FALSE;  /* One-shot */
}

static void update_model_info_label(ConfigDialog *dlg, const char *path) {
    if (!dlg || !path || !dlg->model_info_label) return;

    /* Cancel any pending load for this dialog */
    if (dlg->model_info_idle_id > 0) {
        g_source_remove(dlg->model_info_idle_id);
        dlg->model_info_idle_id = 0;
    }

    /* Show loading indicator immediately */
    gtk_label_set_markup(dlg->model_info_label,
        "<span foreground='gray' style='italic'>Reading model info...</span>");
    gtk_widget_set_visible(GTK_WIDGET(dlg->model_info_label), TRUE);

    /* Schedule async metadata loading */
    ModelInfoLoadData *data = g_new0(ModelInfoLoadData, 1);
    data->dlg = dlg;
    data->path = g_strdup(path);
    data->idle_id = g_idle_add(model_info_load_callback, data);
    dlg->model_info_idle_id = data->idle_id;  /* Track so we can cancel on dialog close */
}

static void on_model_path_changed(GtkEntry *entry, ConfigDialog *dlg) {
    (void)entry;
    config_dialog_clear_error(dlg->model_path_error);

    const char *path = gtk_entry_get_text(dlg->model_path_entry);
    if (path && path[0] != '\0') {
        update_model_info_label(dlg, path);
    } else {
        gtk_label_set_markup(dlg->model_info_label, "");
        gtk_widget_set_visible(GTK_WIDGET(dlg->model_info_label), FALSE);
        config_dialog_show_error(dlg->model_path_error, "No valid whisper ggml file found");
    }
}

static void on_browse_model_clicked(GtkButton *button, ConfigDialog *dlg) {
    (void)button;

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Select Whisper Model File",
        GTK_WINDOW(dlg->dialog),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Open", GTK_RESPONSE_ACCEPT,
        NULL
    );

    /* Add filter for .bin and .gguf files */
    GtkFileFilter *model_filter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(model_filter, "*.bin");
    gtk_file_filter_add_pattern(model_filter, "*.gguf");
    gtk_file_filter_set_name(model_filter, "Whisper model files (*.bin, *.gguf)");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), model_filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all_filter);

    /* Set initial folder to ~/.cache/whisper if it exists */
    char default_dir[512];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(default_dir, sizeof(default_dir), "%s/.cache/whisper", home);
        if (access(default_dir, F_OK) == 0) {
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), default_dir);
        }
    }

    gint response = gtk_dialog_run(GTK_DIALOG(chooser));
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (filename) {
            /* Immediately validate the selected file as a valid GGUF model */
            if (config_dialog_validate_gguf_model(filename)) {
                /* Valid model — set path, clear error, and load metadata */
                config_dialog_clear_error(dlg->model_path_error);
                gtk_entry_set_text(dlg->model_path_entry, filename);
                update_model_info_label(dlg, filename);
            } else {
                /* Invalid model — show error, do NOT set the entry */
                config_dialog_show_error(dlg->model_path_error, "No valid whisper ggml file found");
                g_free(filename);
                filename = NULL;
            }
        }
    }
    gtk_widget_destroy(chooser);
}

static void on_duration_changed(GtkSpinButton *spin, ConfigDialog *dlg) {
    (void)spin;
    config_dialog_clear_error(dlg->duration_error);
}

/* MAJ-001 fix: Async model metadata loading to avoid blocking GTK main loop.
 * Previously, model_info_load() blocked the GTK thread for 5-15 seconds
 * on large models. Now uses g_idle_add() callback pattern. */
typedef struct {
    ConfigDialog *dlg;
    char *model_path;
    WhisperClient *whisper_client;
} TestModelData;

static gboolean test_model_callback(gpointer user_data) {
    TestModelData *data = (TestModelData *)user_data;
    ConfigDialog *dlg = data->dlg;
    char *model_path = data->model_path;
    WhisperClient *whisper_client = data->whisper_client;

    /* Load model metadata */
    ModelInfo info;
    model_info_init(&info);
    if (model_info_load(model_path, &info) && info.valid) {
        char status[512];
        snprintf(status, sizeof(status),
            "OK: %s (%s) — %s",
            info.model_name, info.quantization,
            info.multilingual ? "Multilingual" : "EN-only");
        gtk_label_set_text(dlg->test_status_label, status);
    } else {
        /* Fallback: just check if file exists */
        bool available = whisper_check_connection(whisper_client);
        if (available) {
            gtk_label_set_text(dlg->test_status_label, "Model file found but metadata unreadable");
        } else {
            gtk_label_set_text(dlg->test_status_label, "Model file not found");
        }
    }

    g_free(model_path);
    g_free(data);
    return FALSE;  /* One-shot */
}

static void on_test_model_clicked(GtkButton *button, ConfigDialog *dlg) {
    (void)button;

    if (!dlg->whisper_client || !dlg->test_status_label) return;

    const char *model_path = gtk_entry_get_text(dlg->model_path_entry);
    if (!model_path || model_path[0] == '\0') {
        gtk_label_set_text(dlg->test_status_label, "Enter a model path first");
        return;
    }

    whisper_client_set_model_path(dlg->whisper_client, model_path);
    gtk_label_set_text(dlg->test_status_label, "Loading model...");

    /* MAJ-001 fix: Schedule async metadata loading instead of blocking */
    TestModelData *data = g_new0(TestModelData, 1);
    data->dlg = dlg;
    data->model_path = g_strdup(model_path);
    data->whisper_client = dlg->whisper_client;
    g_idle_add(test_model_callback, data);
}

/* HI-01/HI-04 fix: Proper callback struct for timeout to reset button label.
 * Avoids type mismatch with GSourceFunc and memory leak from g_strdup. */
typedef struct {
    GtkButton *button;
} ResetLabelData;

static gboolean reset_copy_button_label_callback(gpointer user_data) {
    ResetLabelData *data = (ResetLabelData *)user_data;
    gtk_button_set_label(data->button, "Copy");
    g_free(data);
    return FALSE;  /* One-shot timeout */
}

/* MIN-003 fix: Copy hotkey command to clipboard */
static void on_copy_hotkey_clicked(GtkButton *button, ConfigDialog *dlg) {
    (void)button;
    (void)dlg;
    const char *hotkey_cmd = config_dialog_get_hotkey_command();
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clipboard, hotkey_cmd, -1);
    gtk_clipboard_store(clipboard);
    /* Briefly change button text to confirm */
    gtk_button_set_label(button, "Copied!");
    /* HI-01/HI-04 fix: Use proper callback with correct GSourceFunc signature */
    ResetLabelData *data = g_new0(ResetLabelData, 1);
    data->button = button;
    g_timeout_add(1500, reset_copy_button_label_callback, data);
}

/* ===================================================================
 * Main dialog creation and display
 * =================================================================== */

/**
 * config_dialog_show
 * Creates and shows the configuration dialog as a modal window.
 *
 * @param parent_window Parent GTK window
 * @param config AppConfig struct to edit
 * @return true if user clicked Save, false if Cancel
 */
bool config_dialog_show(GtkWindow *parent_window, struct _AppConfig *config,
                           WhisperClient *whisper_client) {
    if (!parent_window || !config) return false;


    /* MIN-004 fix: Allocate ConfigDialog on heap to avoid stack overflow */
    ConfigDialog *dlg = g_new0(ConfigDialog, 1);
    dlg->config = config;
    dlg->whisper_client = whisper_client;

    dlg->dialog = GTK_DIALOG(gtk_dialog_new_with_buttons(
        "Transcriber Settings",
        parent_window,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Save", GTK_RESPONSE_OK,
        NULL
    ));

    /* Prevent resizing for consistent layout */
    gtk_window_set_resizable(GTK_WINDOW(dlg->dialog), FALSE);

    /* Position dialog adjacent to parent window (right side, touching edge) */
    /* Must be done before showing the dialog */
    gint px, py, pw, ph;
    gtk_window_get_position(GTK_WINDOW(parent_window), &px, &py);
    gtk_window_get_size(GTK_WINDOW(parent_window), &pw, &ph);

    /* Estimate dialog size (will be refined after realization) */
    gint dw = 420;  /* Approximate dialog width */
    gint dh = 600;  /* Approximate dialog height */

    /* Position to the right of parent, vertically centered */
    int dx = px + pw + 1;  /* Touching right edge */
    int dy = py + (ph - dh) / 2;  /* Vertically centered */

    /* Ensure dialog stays on screen */
    GtkWidget *parent_widget = GTK_WIDGET(parent_window);
    GdkDisplay *display = gtk_widget_get_display(parent_widget);
    GdkWindow *parent_gdk = gtk_widget_get_window(parent_widget);
    GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, parent_gdk);
    GdkRectangle monitor_rect;
    gdk_monitor_get_geometry(monitor, &monitor_rect);
    gint screenWidth = monitor_rect.width;
    gint screenHeight = monitor_rect.height;

    if (dx + dw > screenWidth - 10) {
        /* Not enough space on right, place on left */
        dx = px - dw - 1;
    }
    if (dx < 10) {
        dx = 10;  /* Minimum margin from left edge */
    }
    if (dy < 10) {
        dy = 10;  /* Minimum margin from top */
    }
    if (dy + dh > screenHeight - 10) {
        dy = screenHeight - dh - 10;  /* Minimum margin from bottom */
    }

    gtk_window_move(GTK_WINDOW(dlg->dialog), dx, dy);

    /* Get content area */
    GtkWidget *content = gtk_dialog_get_content_area(dlg->dialog);
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_widget_set_size_request(GTK_WIDGET(content), 400, -1);

    /* Create main vbox */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(content), vbox);

    /* Separator between title and content */
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), separator, FALSE, FALSE, 6);

    /* ---- Whisper Model Path ---- */
    GtkWidget *url_label = gtk_label_new("Whisper Model Path:");
    gtk_label_set_xalign(GTK_LABEL(url_label), 0);
    gtk_box_pack_start(GTK_BOX(vbox), url_label, FALSE, FALSE, 0);

    /* Horizontal box: Entry + Browse Button */
    GtkWidget *model_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    dlg->model_path_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(dlg->model_path_entry, "ggml-base.bin");
    gtk_box_pack_start(GTK_BOX(model_box), GTK_WIDGET(dlg->model_path_entry), TRUE, TRUE, 0);

    dlg->model_browse_button = GTK_BUTTON(gtk_button_new_with_label("Browse..."));
    gtk_box_pack_start(GTK_BOX(model_box), GTK_WIDGET(dlg->model_browse_button), FALSE, FALSE, 0);

    g_signal_connect(dlg->model_browse_button, "clicked",
                     G_CALLBACK(on_browse_model_clicked), dlg);

    gtk_box_pack_start(GTK_BOX(vbox), model_box, FALSE, TRUE, 0);

    /* Model info label - displays model metadata */
    dlg->model_info_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(GTK_LABEL(dlg->model_info_label), 0);
    gtk_label_set_use_markup(dlg->model_info_label, TRUE);
    /* model_info_label visibility is managed by update_model_info_label() */
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->model_info_label), FALSE, FALSE, 0);

    dlg->model_path_error = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(GTK_LABEL(dlg->model_path_error), 0);
    /* model_path_error visibility is managed by config_dialog_show_error/clear_error */
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->model_path_error), FALSE, FALSE, 0);

    /* Determine effective model path:
     * Priority:
     * 1. Use the model path from the configuration file (if valid).
     * 2. Fall back to the built-in default model if config is empty/missing/invalid.
     * 3. Show error if neither is available. */
    const char *default_path = config_dialog_get_default_model_path();
    const char *current_path = config_get_model_path(config);
    fflush(stderr);
    bool use_default = false;
    bool has_valid_model = false;

    /* First, check if the configured path is valid */
    if (current_path && current_path[0] != '\0') {
        if (config_dialog_validate_gguf_model(current_path)) {
            /* Config has a valid model — use it */
            has_valid_model = true;
        }
        /* If config path is invalid, fall through to check default */
    }

    /* If config path is empty or invalid, try the built-in default */
    if (!has_valid_model && config_dialog_default_model_exists()) {
        use_default = true;
        has_valid_model = true;
    }

    /* Set the entry text based on decision (before connecting 'changed' signal) */
    const char *effective_path = NULL;
    if (use_default) {
        gtk_entry_set_text(dlg->model_path_entry, default_path);
        effective_path = default_path;
    } else if (has_valid_model && current_path && current_path[0] != '\0') {
        gtk_entry_set_text(dlg->model_path_entry, current_path);
        effective_path = current_path;
    } else {
        /* No valid model found — show whatever path we have and error */
        if (current_path && current_path[0] != '\0') {
            gtk_entry_set_text(dlg->model_path_entry, current_path);
        }
        config_dialog_show_error(dlg->model_path_error, "No valid whisper ggml file found");
    }

    /* Connect 'changed' signal AFTER setting initial text to avoid spurious callbacks */
    g_signal_connect(dlg->model_path_entry, "changed",
                     G_CALLBACK(on_model_path_changed), dlg);

    /* Now load model info for the effective path */
    if (effective_path && effective_path[0] != '\0') {
        update_model_info_label(dlg, effective_path);
    }

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);

    /* ---- Audio Device ---- */
    GtkWidget *device_label = gtk_label_new("Audio Device:");
    gtk_label_set_xalign(GTK_LABEL(device_label), 0);
    gtk_box_pack_start(GTK_BOX(vbox), device_label, FALSE, FALSE, 0);

    GtkListStore *device_store = config_dialog_get_audio_devices(AUDIO_BACKEND_NONE);
    dlg->device_combo = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(device_store)));
    g_object_unref(device_store);

    GtkCellRenderer *device_renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(dlg->device_combo), device_renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(dlg->device_combo), device_renderer,
                                   "text", 0, NULL);
    /* NOTE: Do NOT g_object_unref(device_renderer) — see model selector comment */

    /* Select current device — match against column 1 (device name) */
    const char *current_device = config_get_audio_device(config);
    GtkTreeIter d_iter;
    GtkTreeModel *d_model = GTK_TREE_MODEL(gtk_combo_box_get_model(dlg->device_combo));
    if (gtk_tree_model_get_iter_first(d_model, &d_iter)) {
        do {
            gchar *name = NULL;
            gtk_tree_model_get(d_model, &d_iter, 1, &name, -1);
            if (name) {
                gboolean match = FALSE;
                if (current_device && current_device[0] == '\0' && g_strcmp0(name, "default") == 0) {
                    match = TRUE;
                } else if (current_device && g_strcmp0(name, current_device) == 0) {
                    match = TRUE;
                }
                if (match) {
                    gtk_combo_box_set_active_iter(dlg->device_combo, &d_iter);
                    g_free(name);
                    break;
                }
                g_free(name);
            }
        } while (gtk_tree_model_iter_next(d_model, &d_iter));
    }

    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->device_combo), FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);

    /* Language selection removed - using multilingual models only */

    /* ---- Max Duration ---- */
    GtkWidget *dur_label = gtk_label_new("Max Recording Duration (seconds):");
    gtk_label_set_xalign(GTK_LABEL(dur_label), 0);
    gtk_box_pack_start(GTK_BOX(vbox), dur_label, FALSE, FALSE, 0);

    dlg->duration_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5, 30, 1));
    gtk_spin_button_set_value(dlg->duration_spin, (gdouble)config_get_max_duration(config));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->duration_spin), FALSE, TRUE, 0);

    dlg->duration_error = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(GTK_LABEL(dlg->duration_error), 0);
    /* duration_error visibility is managed by config_dialog_show_error/clear_error */
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->duration_error), FALSE, FALSE, 0);

    g_signal_connect(dlg->duration_spin, "value-changed", G_CALLBACK(on_duration_changed), dlg);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);

    /* ---- Notifications ---- */
    dlg->notifications_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Enable Desktop Notifications"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dlg->notifications_check),
                                 config_get_notifications(config));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->notifications_check), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);

    /* ---- Hotkey Command ---- */
    GtkWidget *hotkey_title = gtk_label_new("D-Bus Hotkey Command:");
    gtk_label_set_xalign(GTK_LABEL(hotkey_title), 0);
    PangoAttrList *hotkey_attrs = pango_attr_list_new();
    pango_attr_list_insert(hotkey_attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(hotkey_title), hotkey_attrs);
    pango_attr_list_unref(hotkey_attrs);
    gtk_box_pack_start(GTK_BOX(vbox), hotkey_title, FALSE, FALSE, 0);

    const char *hotkey_cmd = config_dialog_get_hotkey_command();
    dlg->hotkey_label = GTK_LABEL(gtk_label_new(hotkey_cmd));
    gtk_label_set_xalign(GTK_LABEL(dlg->hotkey_label), 0);
    gtk_label_set_line_wrap(dlg->hotkey_label, TRUE);
    gtk_label_set_selectable(dlg->hotkey_label, TRUE);

    /* Set monospace font */
    GtkStyleContext *ctx = gtk_widget_get_style_context(GTK_WIDGET(dlg->hotkey_label));
    gtk_style_context_add_class(ctx, "monospace");

    /* Copy button for hotkey command (MIN-003 fix) */
    GtkWidget *hotkey_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(hotkey_box), GTK_WIDGET(dlg->hotkey_label), TRUE, TRUE, 0);
    GtkWidget *copy_btn = gtk_button_new_with_label("Copy");
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_hotkey_clicked), dlg);
    gtk_box_pack_start(GTK_BOX(hotkey_box), copy_btn, FALSE, FALSE, 0);

    GtkWidget *hotkey_frame = gtk_frame_new(NULL);
    gtk_container_set_border_width(GTK_CONTAINER(hotkey_frame), 4);
    gtk_container_add(GTK_CONTAINER(hotkey_frame), hotkey_box);
    gtk_widget_set_size_request(GTK_WIDGET(hotkey_frame), -1, 80);
    gtk_box_pack_start(GTK_BOX(vbox), hotkey_frame, FALSE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);

    /* ---- Reset Window Position Button ---- */
    GtkWidget *reset_btn = gtk_button_new_with_label("Reset Window Position");
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset_position_clicked), dlg);
    gtk_box_pack_start(GTK_BOX(vbox), reset_btn, FALSE, FALSE, 0);

    /* Spacer to push buttons to bottom */
    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(vbox), spacer, TRUE, TRUE, 0);

    /* ---- Test Model Button and Status ---- */
    GtkWidget *test_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *test_btn = gtk_button_new_with_label("Test Model");
    gtk_widget_set_sensitive(test_btn, whisper_client != NULL);
    dlg->test_status_label = GTK_LABEL(gtk_label_new(""));
    g_signal_connect(test_btn, "clicked", G_CALLBACK(on_test_model_clicked), dlg);
    gtk_box_pack_start(GTK_BOX(test_box), test_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(test_box), GTK_WIDGET(dlg->test_status_label), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), test_box, FALSE, FALSE, 6);

    /* ---- Connect Save/Cancel buttons ---- */
    GtkWidget *save_btn = gtk_dialog_get_widget_for_response(dlg->dialog, GTK_RESPONSE_OK);
    GtkWidget *cancel_btn = gtk_dialog_get_widget_for_response(dlg->dialog, GTK_RESPONSE_CANCEL);

    if (save_btn) {
        g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_clicked), dlg);
    }
    if (cancel_btn) {
        g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), dlg);
    }

    /* MIN-002 fix: Ensure Escape key closes dialog */
    gtk_window_set_accept_focus(GTK_WINDOW(dlg->dialog), TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(cancel_btn));

    /* Show all widgets before running the dialog.
     * Using gtk_widget_show_all() on the dialog ensures every widget in the
     * hierarchy is visible BEFORE gtk_dialog_run() starts the event loop.
     * This avoids layout/mapping races from fragmented show/show_all calls. */
    gtk_widget_show_all(GTK_WIDGET(dlg->dialog));

    /* Run dialog */
    gint response = gtk_dialog_run(dlg->dialog);
    bool saved = (response == GTK_RESPONSE_OK);

    /* Cancel any pending model info loading before destroying the dialog.
     * This prevents use-after-free if the idle callback runs after dialog destruction. */
    if (dlg->model_info_idle_id > 0) {
        g_source_remove(dlg->model_info_idle_id);
        dlg->model_info_idle_id = 0;
    }

    /* Destroy dialog */
    gtk_widget_destroy(GTK_WIDGET(dlg->dialog));
    g_free(dlg); /* MIN-004 fix: free heap-allocated ConfigDialog */

    return saved;
}
