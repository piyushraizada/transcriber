/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * app_config_dialog.c — Configuration Dialog UI implementation
 *
 * Modal GTK3 settings window with all configurable fields.
 */

#include "app_config_dialog.h"
#include "app.h"
#include "app_config.h"
#include "app_audio.h"
#include "app_whisper.h"
#include "app_model_info.h"
#include "app_gpu.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Default model file distributed with the application package */
#define DEFAULT_MODEL_DIR  "~/.cache/whisper"
#define DEFAULT_MODEL_FILE "ggml-large-v3-turbo-q8_0.bin"

/* ===================================================================
 * Internal dialog state
 * =================================================================== */

struct _ConfigDialog {
    GtkDialog *dialog;
    AppConfig *config;
    /* Widgets */
    GtkEntry *model_path_entry;
    GtkButton *model_browse_button;
    GtkLabel *model_info_label;
    GtkComboBox *device_combo;
    GtkComboBox *language_combo;
    GtkComboBox *gpu_mode_combo;
    GtkCheckButton *flash_attention_checkbox;
    GtkSpinButton *duration_spin;
    GtkCheckButton *append_text_checkbox;
    /* VAD controls */
    GtkComboBox *vad_mode_combo;
    GtkComboBox   *vad_silence_combo;
    GtkSpinButton *min_segment_spin;
    GtkCheckButton *continuous_dictation_checkbox;
    GtkCheckButton *debug_logs_checkbox;
    GtkLabel *model_path_error;
    GtkLabel *duration_error;
    GtkLabel *hotkey_label;
    /* Async model info loading */
    guint model_info_idle_id;  /* Track idle source so we can cancel on dialog close */
};

/* Language selection removed - using multilingual models only. */

/* ===================================================================
 * Default model path helpers
 * =================================================================== */

/**
 * Get the full default model path, expanding ~ to $HOME.
 * Returns a statically allocated buffer (do NOT free).
 */
const char* config_dialog_get_default_model_path(void) {
    static __thread char path[1024];
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
bool config_dialog_validate_model(const char *path) {
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
    return duration >= 5 && duration <= 120;
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

    /* Use markup instead of creating/destroying CSS providers repeatedly */
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
 * Queries ALSA for available input devices.
 *
 * @return GtkListStore* of device names (caller must unref), or NULL
 */
GtkListStore * config_dialog_get_audio_devices(void) {

    /* Two-column store: col 0 = display name, col 1 = device name */
    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);

    /* Always add "Default (system microphone)" as the first option.
     * This maps to ALSA's virtual "default" device, which resolves through
     * the ALSA plugin chain (plug → dsnoop → actual hw:* device).
     * It is the config default value and is always valid at runtime
     * (validated at main.c toggle handler and used as fallback in capture thread). */
    {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            0, "Default (system microphone)",
            1, "default",
            -1);
    }

    /* Use the audio module's device enumeration instead of duplicating ALSA logic.
     * audio_recorder_get_device_list() handles all ALSA hint enumeration, capture
     * device filtering, and open-testing internally.
     * Returns both user-friendly display names and raw ALSA device names. */
    AudioDeviceList *dev_list = audio_recorder_get_device_list(NULL, false);

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
    UNUSED(button);

    gboolean valid = TRUE;

    /* Validate Model Path */
    const char *model_path = gtk_entry_get_text(dlg->model_path_entry);
    
    if (!config_dialog_validate_model_path(model_path)) {
        config_dialog_show_error(dlg->model_path_error, APP_ERROR_NO_VALID_MODEL);
        valid = FALSE;
    } else {
        /* Check file exists using lstat() to detect symlinks.
         * Reject symlinks to prevent symlink attacks where a malicious
         * user could redirect model loading to an arbitrary file. */
        struct stat st;
        if (lstat(model_path, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            config_dialog_show_error(dlg->model_path_error, APP_ERROR_NO_VALID_MODEL);
            valid = FALSE;
        } else {
            /* Validate GGUF/GGML format by attempting to load metadata
             * model_info_load returns false on I/O errors,
             * so we check return value first, then info.valid for format. */
            ModelInfo info;
            model_info_init(&info);
            if (!model_info_load(model_path, &info)) {
                /* I/O error — file not found, cannot open, etc. */
                config_dialog_show_error(dlg->model_path_error, APP_ERROR_NO_VALID_MODEL);
                valid = FALSE;
            } else if (!info.valid) {
                /* File opened but not a valid Whisper model */
                config_dialog_show_error(dlg->model_path_error, APP_ERROR_NO_VALID_MODEL);
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
        g_free(display_name);  /* Display name no longer stored — it was write-only */
        if (device_name) {
            if (g_strcmp0(device_name, "default") == 0) {
                config_set_audio_device(dlg->config, "");
            } else {
                config_set_audio_device(dlg->config, device_name);
            }
            g_free(device_name);
        }
    }

    /* Set duration */
    config_set_max_duration(dlg->config, duration);

    /* Set GPU mode — column 0 = display name, column 1 = mode value */
    if (dlg->gpu_mode_combo) {
        GtkTreeIter gpu_iter;
        GtkTreeModel *gpu_model = GTK_TREE_MODEL(gtk_combo_box_get_model(dlg->gpu_mode_combo));
        if (gtk_combo_box_get_active_iter(dlg->gpu_mode_combo, &gpu_iter)) {
            gchar *mode_val = NULL;
            gtk_tree_model_get(gpu_model, &gpu_iter, 1, &mode_val, -1);
            if (mode_val) {
                config_set_gpu_mode(dlg->config, mode_val);
                g_free(mode_val);
            }
        }
    }

    /* Set flash attention from checkbox */
    config_set_flash_attention(dlg->config,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dlg->flash_attention_checkbox)));

    /* Set append transcription text mode from checkbox */
    config_set_append_transcription_text(dlg->config,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dlg->append_text_checkbox)));

    /* Save VAD settings */
    config_set_vad_mode(dlg->config, gtk_combo_box_get_active(dlg->vad_mode_combo));
    {
        int silence_idx = gtk_combo_box_get_active(dlg->vad_silence_combo);
        static const float kSilenceValues[] = {0.5f, 1.0f, 1.5f, 2.0f};
        if (silence_idx >= 0 && (unsigned)silence_idx <= G_N_ELEMENTS(kSilenceValues)) {
            config_set_scanner_silence_sec(dlg->config, kSilenceValues[silence_idx]);
        }
    }
    config_set_scanner_min_segment_sec(dlg->config,
        (float)gtk_spin_button_get_value(dlg->min_segment_spin));
    config_set_continuous_dictation(dlg->config,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dlg->continuous_dictation_checkbox)));
    config_set_debug_logs(dlg->config,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dlg->debug_logs_checkbox)));
    /* Save Language */
    if (dlg->language_combo) {
        GtkTreeIter lang_iter;
        GtkTreeModel *lang_model = GTK_TREE_MODEL(gtk_combo_box_get_model(dlg->language_combo));
        if (gtk_combo_box_get_active_iter(dlg->language_combo, &lang_iter)) {
            gchar *lang_code = NULL;
            gtk_tree_model_get(lang_model, &lang_iter, 1, &lang_code, -1);
            if (lang_code) {
                config_set_language(dlg->config, lang_code);
                g_free(lang_code);
            }
        }
    }

    /* Save config to file */
    config_save(dlg->config);

    /* Close dialog with response ID GTK_RESPONSE_OK */
    gtk_dialog_response(dlg->dialog, GTK_RESPONSE_OK);
}

static void on_cancel_clicked(GtkButton *button, ConfigDialog *dlg) {
    UNUSED(button);
    gtk_dialog_response(dlg->dialog, GTK_RESPONSE_CANCEL);
}


static void on_reset_position_clicked(GtkButton *button, ConfigDialog *dlg) {
    UNUSED(button);
    config_dialog_reset_window_position(dlg->config);
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
}

/* Async model info loading to avoid blocking GTK main loop */
typedef struct {
    ConfigDialog *dlg;
    char *path;
    guint idle_id;  /* Store the idle source ID so we can track/cancel it */
} ModelInfoLoadData;

static void model_info_load_data_free(gpointer user_data) {
    ModelInfoLoadData *data = (ModelInfoLoadData *)user_data;
    g_free(data->path);
    g_free(data);
}

static gboolean model_info_load_callback(gpointer user_data) {
    ModelInfoLoadData *data = (ModelInfoLoadData *)user_data;
    ConfigDialog *dlg = data->dlg;
    char *path = data->path;

    /* Safety check: verify the dialog widget is still alive before accessing it.
     * This prevents use-after-free if the dialog was closed before the callback ran. */
    if (!GTK_IS_WIDGET(GTK_WIDGET(dlg->dialog)) ||
        !gtk_widget_get_realized(GTK_WIDGET(dlg->dialog))) {
        return FALSE;  /* destroy notify frees data and path */
    }

    /* Load model metadata */
    ModelInfo info;
    model_info_init(&info);

    /* Check return value first, then info.valid */
    if (model_info_load(path, &info) && info.valid) {
        /* Escape model metadata for Pango markup safety */
        /* Truncate to prevent markup buffer overflow from malformed GGUF metadata */
        gchar *escaped_name = g_markup_escape_text(info.model_name, -1);
        gchar *escaped_quant = g_markup_escape_text(info.quantization, -1);
        if (strlen(escaped_name) > 120) escaped_name[120] = '\0';
        if (strlen(escaped_quant) > 40) escaped_quant[40] = '\0';
        const char *lang_str = info.multilingual ? "Multilingual" : "English-only";
        char markup[256];
        g_snprintf(markup, sizeof(markup),
            "<span size='small'><b>%s</b> - %s - %s</span>",
            escaped_name, escaped_quant, lang_str);
        gtk_label_set_markup(dlg->model_info_label, markup);
        g_free(escaped_name);
        g_free(escaped_quant);
    } else {
        /* I/O error or invalid model — show error */
        gtk_label_set_markup(dlg->model_info_label,
            "<span foreground='red' size='small'>No valid whisper ggml file found</span>");
    }

    /* Clear the tracked idle ID since this callback just ran */
    if (dlg->model_info_idle_id == data->idle_id) {
        dlg->model_info_idle_id = 0;
    }

    return FALSE;  /* One-shot; destroy notify frees data and path */
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
    data->idle_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, model_info_load_callback, data,
                                    model_info_load_data_free);
    dlg->model_info_idle_id = data->idle_id;  /* Track so we can cancel on dialog close */
}

static void on_model_path_changed(GtkEntry *entry, ConfigDialog *dlg) {
    UNUSED(entry);
    config_dialog_clear_error(dlg->model_path_error);

    const char *path = gtk_entry_get_text(dlg->model_path_entry);
    if (path && path[0] != '\0') {
        update_model_info_label(dlg, path);
    } else {
        gtk_label_set_markup(dlg->model_info_label, "");
        gtk_widget_set_visible(GTK_WIDGET(dlg->model_info_label), FALSE);
        config_dialog_show_error(dlg->model_path_error, APP_ERROR_NO_VALID_MODEL);
    }
}

static void on_browse_model_clicked(GtkButton *button, ConfigDialog *dlg) {
    UNUSED(button);

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
            if (config_dialog_validate_model(filename)) {
                /* Valid model — set path, clear error, and load metadata */
                config_dialog_clear_error(dlg->model_path_error);
                gtk_entry_set_text(dlg->model_path_entry, filename);
                update_model_info_label(dlg, filename);
            } else {
                /* Invalid model — show error, do NOT set the entry */
                config_dialog_show_error(dlg->model_path_error, APP_ERROR_NO_VALID_MODEL);
                g_free(filename);
                filename = NULL;
            }
        }
    }
    gtk_widget_destroy(chooser);
}

/**
 * Update flash attention checkbox sensitivity based on GPU mode.
 * Flash attention only works with NVIDIA CUDA GPUs — disable it when CPU mode is selected.
 */
static void update_flash_attention_sensitivity(ConfigDialog *dlg) {
    if (!dlg || !dlg->flash_attention_checkbox || !dlg->gpu_mode_combo) return;

    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(gtk_combo_box_get_model(dlg->gpu_mode_combo));
    bool is_cpu = false;

    if (gtk_combo_box_get_active_iter(dlg->gpu_mode_combo, &iter)) {
        gchar *mode_val = NULL;
        gtk_tree_model_get(model, &iter, 1, &mode_val, -1);
        if (mode_val) {
            is_cpu = (g_strcmp0(mode_val, "cpu") == 0);
            g_free(mode_val);
        }
    }

    gtk_widget_set_sensitive(GTK_WIDGET(dlg->flash_attention_checkbox), !is_cpu);
    if (!is_cpu && dlg->flash_attention_checkbox) {
        /* Re-enable the checkbox so user can toggle it */
    } else if (is_cpu) {
        /* Force off when CPU mode is active to avoid confusion on save */
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dlg->flash_attention_checkbox), FALSE);
    }
}

static void on_gpu_mode_changed(GtkComboBox *combo, ConfigDialog *dlg) {
    UNUSED(combo);
    update_flash_attention_sensitivity(dlg);
}

static void on_duration_changed(GtkSpinButton *spin, ConfigDialog *dlg) {
    UNUSED(spin);
    config_dialog_clear_error(dlg->duration_error);
}


/* Async model metadata loading to avoid blocking GTK main loop.
 * Previously, model_info_load() blocked the GTK thread for 5-15 seconds
 * on large models. Now uses g_idle_add() callback pattern. */
/* Proper callback struct for timeout to reset button label.
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

/* Copy hotkey command to clipboard */
static void on_copy_hotkey_clicked(GtkButton *button, ConfigDialog *dlg) {
    UNUSED(button);
    UNUSED(dlg);
    const char *hotkey_cmd = config_dialog_get_hotkey_command();
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clipboard, hotkey_cmd, -1);
    gtk_clipboard_store(clipboard);
    /* Briefly change button text to confirm */
    gtk_button_set_label(button, "Copied!");
    /* Use proper callback with correct GSourceFunc signature */
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
bool config_dialog_show(GtkWindow *parent_window, struct _AppConfig *config) {
    if (!parent_window || !config) return false;


    /* Allocate ConfigDialog on heap to avoid stack overflow */
    ConfigDialog *dlg = g_new0(ConfigDialog, 1);
    dlg->config = config;
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
        if (config_dialog_validate_model(current_path)) {
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
        config_dialog_show_error(dlg->model_path_error, APP_ERROR_NO_VALID_MODEL);
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

    GtkListStore *device_store = config_dialog_get_audio_devices();
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

    /* ---- Language ---- */
    {
        GtkWidget *lang_label = gtk_label_new("Language:");
        gtk_label_set_xalign(GTK_LABEL(lang_label), 0);
        gtk_box_pack_start(GTK_BOX(vbox), lang_label, FALSE, FALSE, 0);

        /* Language options: display name -> code */
        struct {
            const char *display;
            const char *code;
        } languages[] = {
            { "Auto-detect", "auto" },
            { "English", "en" },
            { "Chinese (中文)", "zh" },
            { "German (Deutsch)", "de" },
            { "Spanish (Español)", "es" },
            { "Russian (Русский)", "ru" },
            { "Korean (한국어)", "ko" },
            { "French (Français)", "fr" },
            { "Japanese (日本語)", "ja" },
            { "Portuguese (Português)", "pt" },
            { "Turkish (Türkçe)", "tr" },
            { "Polish (Polski)", "pl" },
            { "Catalan (Català)", "ca" },
            { "Dutch (Nederlands)", "nl" },
            { "Arabic (العربية)", "ar" },
            { "Swedish (Svenska)", "sv" },
            { "Italian (Italiano)", "it" },
            { "Indonesian (Bahasa)", "id" },
            { "Hindi (हिन्दी)", "hi" },
            { "Finnish (Suomi)", "fi" },
            { "Vietnamese (Tiếng Việt)", "vi" },
            { "Hebrew (עברית)", "he" },
            { "Ukrainian (Українська)", "uk" },
            { "Greek (Ελληνικά)", "el" },
            { "Malay (Bahasa Melayu)", "ms" },
            { "Czech (Čeština)", "cs" },
            { "Romanian (Română)", "ro" },
            { "Danish (Dansk)", "da" },
            { "Hungarian (Magyar)", "hu" },
            { "Tamil (தமிழ்)", "ta" },
            { "Norwegian (Norsk)", "no" },
            { "Thai (ไทย)", "th" },
            { "Urdu (اردو)", "ur" },
            { "HR Croatian (Hrvatski)", "hr" },
            { "Bengali (বাংলা)", "bn" },
            { "Lithuanian (Lietuvių)", "lt" },
            { "Punjabi (ਪੰਜਾਬੀ)", "pa" },
            { "Latvian (Latviešu)", "lv" },
            { "Burmese (မြန်မာ)", "my" },
            { "Belarusian (Беларуская)", "be" },
            { "Assamese (অসমীয়া)", "as" },
            { "Slovenian (Slovenščina)", "sl" },
            { "Tagalog (Filipino)", "tl" },
            { "Slovak (Slovenčina)", "sk" },
            { "Macedonian (Македонски)", "mk" },
            { "Montenegrin (Crnogorski)", "mn" },
            { "Bosnian (Bosanski)", "bs" },
            { "Kazakh (Қазақ)", "kk" },
            { "Azerbaijani (Azərbaycan)", "az" },
            { "Sinhala (සිංහල)", "si" },
            { "Khmer (ខ្មែរ)", "km" },
            { "Shona (ChiShona)", "sn" },
            { "Yoruba (Yorùbá)", "yo" },
            { "Somali (Soomaaliga)", "so" },
            { "Afrikaans (Afrikaans)", "af" },
            { "Occitan (Occitan)", "oc" },
            { "Malayalam (മലയാളം)", "ml" },
            { "Maltese (Malti)", "mt" },
            { "Sanskrit (संस्कृत)", "sa" },
            { "Luxembourgish (Lëtzebuergesch)", "lb" },
            { "Mongolian (Монгол)", "mg" },
            { "Marathi (मराठी)", "mr" },
            { "Nepali (नेपाली)", "ne" },
            { "Oromo (Afaan Oromoo)", "om" },
            { "Pashto (پښتو)", "ps" },
            { "Albanian (Shqip)", "sq" },
            { "Serbian (Српски)", "sr" },
            { "Gujarati (ગુજરાતી)", "gu" },
            { "Kannada (ಕನ್ನಡ)", "kn" },
            { "Estonian (Eesti)", "et" },
            { "Basque (Euskara)", "eu" },
            { "Icelandic (Íslenska)", "is" },
            { "Armenian (Հայերեն)", "hy" },
            { "Nepali (नेपाली)", "ne" },
            { "Javanese (Basa Jawa)", "jv" },
            { "Sanjo (ᱥᱟᱱᱰᱤ)", "sd" },
            { "Sundanese (Basa Sunda)", "su" },
            { "Welsh (Cymraeg)", "cy" },
            { NULL, NULL }
        };

        GtkListStore *lang_store = GTK_LIST_STORE(gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING));
        GtkTreeIter iter;
        for (int i = 0; languages[i].display != NULL; i++) {
            gtk_list_store_append(lang_store, &iter);
            gtk_list_store_set(lang_store, &iter,
                0, languages[i].display,
                1, languages[i].code,
                -1);
        }

        dlg->language_combo = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(lang_store)));
        g_object_unref(lang_store);

        GtkCellRenderer *lang_renderer = gtk_cell_renderer_text_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(dlg->language_combo), lang_renderer, TRUE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(dlg->language_combo), lang_renderer,
                                       "text", 0, NULL);

        /* Select current language from config */
        const char *current_lang = config_get_language(config);
        GtkTreeModel *lang_model = GTK_TREE_MODEL(gtk_combo_box_get_model(dlg->language_combo));
        if (gtk_tree_model_get_iter_first(lang_model, &iter)) {
            do {
                gchar *code = NULL;
                gtk_tree_model_get(lang_model, &iter, 1, &code, -1);
                if (code && g_strcmp0(code, current_lang) == 0) {
                    gtk_combo_box_set_active_iter(dlg->language_combo, &iter);
                    g_free(code);
                    break;
                }
                g_free(code);
            } while (gtk_tree_model_iter_next(lang_model, &iter));
        }

        gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->language_combo), FALSE, FALSE, 0);

        GtkWidget *lang_help = gtk_label_new(
            "Select the input language for transcription. Use Auto-detect for multilingual input.");
        gtk_label_set_xalign(GTK_LABEL(lang_help), 0);
        gtk_widget_set_opacity(lang_help, 0.6);
        gtk_label_set_line_wrap(GTK_LABEL(lang_help), TRUE);
        gtk_box_pack_start(GTK_BOX(vbox), lang_help, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
    }

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

    /* ---- GPU Acceleration ---- */
    {
        GtkWidget *gpu_label = gtk_label_new("GPU Acceleration:");
        gtk_label_set_xalign(GTK_LABEL(gpu_label), 0);
        gtk_box_pack_start(GTK_BOX(vbox), gpu_label, FALSE, FALSE, 0);

        /* Build GPU mode options dynamically */
        GtkListStore *gpu_store = GTK_LIST_STORE(gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING));

        /* Option 0: Auto (select best GPU by free memory) */
        GtkTreeIter iter;
        gtk_list_store_append(gpu_store, &iter);
        gtk_list_store_set(gpu_store, &iter,
            0, "Auto (select best GPU by free memory)",
            1, "auto",
            -1);

        /* Option 1: CPU Only */
        gtk_list_store_append(gpu_store, &iter);
        gtk_list_store_set(gpu_store, &iter,
            0, "CPU Only",
            1, "cpu",
            -1);

        /* Options 2+: Individual GPUs (if CUDA available) */
        int gpu_count = 0;
        gpu_get_device_count(&gpu_count);
        for (int i = 0; i < gpu_count; i++) {
            char display[256];
            char mode[32];
            char name[128] = {"Unknown"};
            gpu_get_device_name(i, name, sizeof(name));
            snprintf(display, sizeof(display), "GPU %d: %s", i, name);
            snprintf(mode, sizeof(mode), "gpu:%d", i);
            gtk_list_store_append(gpu_store, &iter);
            gtk_list_store_set(gpu_store, &iter,
                0, display,
                1, mode,
                -1);
        }

        dlg->gpu_mode_combo = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(gpu_store)));
        g_object_unref(gpu_store);

        GtkCellRenderer *gpu_renderer = gtk_cell_renderer_text_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(dlg->gpu_mode_combo), gpu_renderer, TRUE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(dlg->gpu_mode_combo), gpu_renderer,
                                       "text", 0, NULL);

        /* Select current GPU mode from config */
        const char *current_gpu_mode = config_get_gpu_mode(config);
        GtkTreeModel *gpu_model = GTK_TREE_MODEL(gtk_combo_box_get_model(dlg->gpu_mode_combo));
        if (gtk_tree_model_get_iter_first(gpu_model, &iter)) {
            do {
                gchar *mode_val = NULL;
                gtk_tree_model_get(gpu_model, &iter, 1, &mode_val, -1);
                if (mode_val && current_gpu_mode && g_strcmp0(mode_val, current_gpu_mode) == 0) {
                    gtk_combo_box_set_active_iter(dlg->gpu_mode_combo, &iter);
                    g_free(mode_val);
                    break;
                }
                g_free(mode_val);
            } while (gtk_tree_model_iter_next(gpu_model, &iter));
        }


        gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->gpu_mode_combo), FALSE, TRUE, 0);

        /* Connect signal to update flash attention sensitivity when GPU mode changes */
        g_signal_connect(dlg->gpu_mode_combo, "changed",
                         G_CALLBACK(on_gpu_mode_changed), dlg);

        GtkWidget *gpu_restart_label = gtk_label_new("Restart the application for GPU changes to take effect.");
        gtk_label_set_xalign(GTK_LABEL(gpu_restart_label), 0);
        gtk_widget_set_opacity(gpu_restart_label, 0.6);
        gtk_box_pack_start(GTK_BOX(vbox), gpu_restart_label, FALSE, FALSE, 0);

        dlg->flash_attention_checkbox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Flash Attention (reduces GPU memory usage)"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dlg->flash_attention_checkbox),
                                      config_get_flash_attention(config));
        gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->flash_attention_checkbox), FALSE, FALSE, 0);

        GtkWidget *flash_help = gtk_label_new(
            "Only works with NVIDIA CUDA GPU. Reduces VRAM usage\n"
            "while maintaining transcription accuracy. Requires restart.");
        gtk_label_set_xalign(GTK_LABEL(flash_help), 0);
        gtk_widget_set_opacity(flash_help, 0.6);
        gtk_box_pack_start(GTK_BOX(vbox), flash_help, FALSE, FALSE, 0);

        /* Set initial sensitivity based on current GPU mode */
        update_flash_attention_sensitivity(dlg);

        gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
    }

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);

    /* ---- Transcription Text Mode ---- */
    {
        GtkWidget *text_mode_label = gtk_label_new("Transcription Text Mode:");
        gtk_label_set_xalign(GTK_LABEL(text_mode_label), 0);
        gtk_box_pack_start(GTK_BOX(vbox), text_mode_label, FALSE, FALSE, 0);

        dlg->append_text_checkbox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Append new transcriptions to existing text"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dlg->append_text_checkbox),
                                     config_get_append_transcription_text(config));
        gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->append_text_checkbox), FALSE, FALSE, 0);

        GtkWidget *text_mode_help = gtk_label_new(
            "When enabled, new text is added below previous transcriptions.\n"
            "When disabled, the text window is cleared before each new transcription.");
        gtk_label_set_xalign(GTK_LABEL(text_mode_help), 0);
        gtk_widget_set_opacity(text_mode_help, 0.6);
        gtk_label_set_line_wrap(GTK_LABEL(text_mode_help), TRUE);
        gtk_box_pack_start(GTK_BOX(vbox), text_mode_help, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
    }

    /* ---- Silence Scanner Settings (VAD-based segment detection) ---- */
    {
            GtkWidget *vad_title = gtk_label_new("Voice Activity Detection (VAD):");
            gtk_label_set_xalign(GTK_LABEL(vad_title), 0);
            PangoAttrList *vad_attrs = pango_attr_list_new();
            pango_attr_list_insert(vad_attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
            gtk_label_set_attributes(GTK_LABEL(vad_title), vad_attrs);
            pango_attr_list_unref(vad_attrs);
            gtk_box_pack_start(GTK_BOX(vbox), vad_title, FALSE, FALSE, 0);
    
            // VAD Mode (sensitivity)
        GtkWidget *vad_mode_label = gtk_label_new("  Sensitivity:");
        gtk_label_set_xalign(GTK_LABEL(vad_mode_label), 0);
        gtk_box_pack_start(GTK_BOX(vbox), vad_mode_label, FALSE, FALSE, 0);

        dlg->vad_mode_combo = GTK_COMBO_BOX(gtk_combo_box_text_new());
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dlg->vad_mode_combo), "0", "Least sensitive (catches quiet speech)");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dlg->vad_mode_combo), "1", "Moderate (recommended)");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dlg->vad_mode_combo), "2", "Aggressive");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dlg->vad_mode_combo), "3", "Most aggressive (only clear speech)");
        gtk_combo_box_set_active(dlg->vad_mode_combo, config_get_vad_mode(config));
        gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->vad_mode_combo), FALSE, FALSE, 0);

        // Scanner silence duration — controls when silence scanner triggers transcription
        GtkWidget *scanner_silence_label = gtk_label_new("  Silence threshold:");
        gtk_label_set_xalign(GTK_LABEL(scanner_silence_label), 0);
        gtk_box_pack_start(GTK_BOX(vbox), scanner_silence_label, FALSE, FALSE, 0);

        dlg->vad_silence_combo = GTK_COMBO_BOX(gtk_combo_box_text_new());
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dlg->vad_silence_combo), "0.5", "0.5 sec");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dlg->vad_silence_combo), "1.0", "1.0 sec");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dlg->vad_silence_combo), "1.5", "1.5 sec");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dlg->vad_silence_combo), "2.0", "2.0 sec");
        {
            float silence_val = config_get_scanner_silence_sec(config);
            static const float kSilenceValues[] = {0.5f, 1.0f, 1.5f, 2.0f};
            int best_idx = 1; /* default to 1.0 */
            for (size_t i = 0; i < G_N_ELEMENTS(kSilenceValues); i++) {
                if (fabsf(silence_val - kSilenceValues[i]) < 0.05f) {
                    best_idx = i;
                    break;
                }
            }
            gtk_combo_box_set_active(dlg->vad_silence_combo, best_idx);
        }
        gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->vad_silence_combo), FALSE, FALSE, 0);

        // Minimum segment duration — shortest audio before sending to Whisper
        GtkWidget *min_seg_label = gtk_label_new("  Minimum segment length:");
        gtk_label_set_xalign(GTK_LABEL(min_seg_label), 0);
        gtk_box_pack_start(GTK_BOX(vbox), min_seg_label, FALSE, FALSE, 0);

        GtkWidget *min_seg_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        dlg->min_segment_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1.0, 30.0, 0.5));
        gtk_spin_button_set_value(dlg->min_segment_spin,
                                  (gdouble)config_get_scanner_min_segment_sec(config));
        gtk_box_pack_start(GTK_BOX(min_seg_box), GTK_WIDGET(dlg->min_segment_spin), FALSE, FALSE, 0);

        GtkWidget *min_seg_unit = gtk_label_new("seconds");
        gtk_box_pack_start(GTK_BOX(min_seg_box), min_seg_unit, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), min_seg_box, FALSE, FALSE, 0);

        // Continuous dictation checkbox
        dlg->continuous_dictation_checkbox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Continuous dictation (silence-triggered loop)"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dlg->continuous_dictation_checkbox),
                                     config_get_continuous_dictation(config));
        gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->continuous_dictation_checkbox), FALSE, FALSE, 0);

        // Debug logs checkbox
        dlg->debug_logs_checkbox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Debug logs (enable verbose logging)"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dlg->debug_logs_checkbox),
                                     config_get_debug_logs(config));
        gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->debug_logs_checkbox), FALSE, FALSE, 0);

        GtkWidget *dictation_help = gtk_label_new(
            "When enabled, silence triggers transcription and recording restarts.\n"
            "When disabled, recording runs until timer expires or you click the mic, then stops.");
        gtk_label_set_xalign(GTK_LABEL(dictation_help), 0);
        gtk_widget_set_opacity(dictation_help, 0.6);
        gtk_label_set_line_wrap(GTK_LABEL(dictation_help), TRUE);
        gtk_box_pack_start(GTK_BOX(vbox), dictation_help, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
    }

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

    /* Copy button for hotkey command */
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

    /* ---- Connect Save/Cancel buttons ---- */
    GtkWidget *save_btn = gtk_dialog_get_widget_for_response(dlg->dialog, GTK_RESPONSE_OK);
    GtkWidget *cancel_btn = gtk_dialog_get_widget_for_response(dlg->dialog, GTK_RESPONSE_CANCEL);

    if (save_btn) {
        g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_clicked), dlg);
    }
    if (cancel_btn) {
        g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), dlg);
    }

    /* Ensure Escape key closes dialog */
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
    g_free(dlg); /* Free heap-allocated ConfigDialog */

    return saved;
}
