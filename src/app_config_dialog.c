/*
 * app_config_dialog.c — Configuration Dialog UI implementation
 *
 * Modal GTK3 settings window with all configurable fields.
 */

#include "app_config_dialog.h"
#include "app_config.h"
#include "app_audio.h"
#include "app_whisper.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <alsa/asoundlib.h>

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
    GtkEntry *url_entry;
    GtkComboBox *device_combo;
    GtkComboBox *language_combo;
    GtkSpinButton *duration_spin;
    GtkCheckButton *notifications_check;
    GtkLabel *url_error;
    GtkLabel *duration_error;
    GtkLabel *hotkey_label;
    GtkLabel *test_status_label;
};

/* ===================================================================
 * Static language list
 * =================================================================== */

/* Language entries: display name -> ISO 639-1 code */
typedef struct {
    const char *display;
    const char *code;
} LanguageEntry;

static const LanguageEntry g_languages[] = {
    {"Auto-detect", ""},
    {"English", "en"},
    {"Spanish", "es"},
    {"French", "fr"},
    {"German", "de"},
    {"Italian", "it"},
    {"Portuguese", "pt"},
    {"Russian", "ru"},
    {"Japanese", "ja"},
    {"Chinese", "zh"},
    {"Korean", "ko"},
    {"Dutch", "nl"},
    {"Polish", "pl"},
    {"Turkish", "tr"},
    {"Vietnamese", "vi"},
};

static const int g_language_count = sizeof(g_languages) / sizeof(g_languages[0]);

/* ===================================================================
 * Validation functions
 * =================================================================== */

/**
 * config_dialog_validate_url
 * Validates a Whisper API URL.
 *
 * @param url URL string to validate
 * @return true if valid (starts with http:// or https://, length < 512)
 */
bool config_dialog_validate_url(const char *url) {
    if (!url) return false;

    size_t len = strlen(url);
    if (len == 0 || len >= 512) return false;

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }

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
 * Helper: populate combo box with strings
 * =================================================================== */

static GtkListStore * create_string_liststore(const char **items, int count) {
    GtkListStore *store = gtk_list_store_new(1, G_TYPE_STRING);

    for (int i = 0; i < count; i++) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, items[i], -1);
    }

    return store;
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

    /* Use ALSA hint API to enumerate capture devices */
    void **hints = NULL;
    int err = snd_device_name_hint(-1, "pcm", &hints);
    if (err >= 0 && hints) {
        for (void **h = hints; *h != NULL; h++) {
            char *name = snd_device_name_get_hint(*h, "NAME");
            char *desc = snd_device_name_get_hint(*h, "DESC");
            char *ioid = snd_device_name_get_hint(*h, "IOID");

            /* IOID "Input" = capture; NULL = both input+output */
            gboolean is_capture = (ioid == NULL) || (strcmp(ioid, "Input") == 0);
            free(ioid);

            if (!is_capture || !name) {
                free(name);
                free(desc);
                continue;
            }

            /* Only show direct hardware devices (hw: prefix) — skip plugins/virtual */
            if (strncmp(name, "hw:", 3) != 0) {
                free(name);
                free(desc);
                continue;
            }

            /* Test if device can actually be opened for capture */
            snd_pcm_t *pcm;
            int test_err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_CAPTURE, 0);
            if (test_err < 0) {
                free(name);
                free(desc);
                continue;
            }
            snd_pcm_close(pcm);

            /* Build clean display name from DESC: take first line, strip ", USB Audio" suffix */
            gchar *display;
            if (desc && desc[0] != '\0') {
                char *d = desc;
                while (*d) {
                    if (*d == '\n') { *d = '\0'; break; }
                    d++;
                }
                /* Remove trailing ", USB Audio" */
                char *comma = strstr(desc, ", USB Audio");
                if (comma) *comma = '\0';
                display = g_strdup(desc);
            } else {
                display = g_strdup(name);
            }
            free(desc);

            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, display, 1, name, -1);
            g_free(display);
            free(name);
        }
        snd_device_name_free_hint(hints);
    }

    /* Add "Default" as the last entry */
    GtkTreeIter iter;
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, 0, "Default", 1, "default", -1);

    return store;
}

/* ===================================================================
 * Language list
 * =================================================================== */

/**
 * config_dialog_get_language_list
 * Returns a GtkListStore with supported languages.
 *
 * @return GtkListStore* (static, do NOT unref)
 */
GtkListStore * config_dialog_get_language_list(void) {
    /* HI-02 fix: Return a newly allocated store. Caller is responsible for
     * calling g_object_unref() when done to avoid memory leaks. */
    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);

    for (int i = 0; i < g_language_count; i++) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
                           0, g_languages[i].display,
                           1, g_languages[i].code,
                           -1);
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
    (void)button;

    gboolean valid = TRUE;

    /* Validate URL */
    const char *url = gtk_entry_get_text(dlg->url_entry);
    if (!config_dialog_validate_url(url)) {
        config_dialog_show_error(dlg->url_error, "URL must start with http:// or https://");
        valid = FALSE;
    } else {
        config_dialog_clear_error(dlg->url_error);
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
    config_set_whisper_url(dlg->config, url);

    /* Get selected audio device — column 1 = device name */
    GtkTreeIter device_iter;
    GtkTreeModel *device_model = GTK_TREE_MODEL(gtk_combo_box_get_model(dlg->device_combo));
    if (gtk_combo_box_get_active_iter(dlg->device_combo, &device_iter)) {
        gchar *device_name = NULL;
        gtk_tree_model_get(device_model, &device_iter, 1, &device_name, -1);
        if (device_name) {
            if (g_strcmp0(device_name, "default") == 0) {
                config_set_audio_device(dlg->config, "");
            } else {
                config_set_audio_device(dlg->config, device_name);
            }
            g_free(device_name);
        }
    }

    /* Get selected language */
    GtkTreeIter lang_iter;
    if (gtk_combo_box_get_active_iter(dlg->language_combo, &lang_iter)) {
        GtkTreeModel *lang_model = GTK_TREE_MODEL(gtk_combo_box_get_model(dlg->language_combo));
        gchar *lang_code = NULL;
        gtk_tree_model_get(lang_model, &lang_iter, 1, &lang_code, -1);
        if (lang_code) {
            config_set_language(dlg->config, lang_code);
            g_free(lang_code);
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

static void on_url_changed(GtkEntry *entry, ConfigDialog *dlg) {
    (void)entry;
    config_dialog_clear_error(dlg->url_error);
}

static void on_duration_changed(GtkSpinButton *spin, ConfigDialog *dlg) {
    (void)spin;
    config_dialog_clear_error(dlg->duration_error);
}

static void on_test_connection_clicked(GtkButton *button, ConfigDialog *dlg) {
    (void)button;

    if (!dlg->whisper_client || !dlg->test_status_label) return;

    const char *url = gtk_entry_get_text(dlg->url_entry);
    if (!url || url[0] == '\0') {
        gtk_label_set_text(dlg->test_status_label, "Enter a URL first");
        return;
    }

    whisper_client_set_url(dlg->whisper_client, url);
    gtk_label_set_text(dlg->test_status_label, "Testing connection...");

    bool connected = whisper_check_connection(dlg->whisper_client);
    if (connected) {
        gtk_label_set_text(dlg->test_status_label, "Connection successful");
    } else {
        gtk_label_set_text(dlg->test_status_label, "Connection failed");
    }
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

    /* ---- Whisper Server URL ---- */
    GtkWidget *url_label = gtk_label_new("Whisper Server URL:");
    gtk_label_set_xalign(GTK_LABEL(url_label), 0);
    gtk_box_pack_start(GTK_BOX(vbox), url_label, FALSE, FALSE, 0);

    dlg->url_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_text(dlg->url_entry, config_get_whisper_url(config));
    gtk_entry_set_placeholder_text(dlg->url_entry, "http://localhost:8000");
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->url_entry), FALSE, TRUE, 0);

    dlg->url_error = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(GTK_LABEL(dlg->url_error), 0);
    gtk_widget_set_no_show_all(GTK_WIDGET(dlg->url_error), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(dlg->url_error), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->url_error), FALSE, FALSE, 0);

    g_signal_connect(dlg->url_entry, "changed", G_CALLBACK(on_url_changed), dlg);

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
    GtkWidget *lang_label = gtk_label_new("Language:");
    gtk_label_set_xalign(GTK_LABEL(lang_label), 0);
    gtk_box_pack_start(GTK_BOX(vbox), lang_label, FALSE, FALSE, 0);

    GtkListStore *lang_store = config_dialog_get_language_list();
    dlg->language_combo = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(lang_store)));
    g_object_unref(lang_store); /* MAJ-005 fix: free ListStore after assigning model */

    GtkCellRenderer *lang_renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(dlg->language_combo), lang_renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(dlg->language_combo), lang_renderer,
                                   "text", 0, NULL);

    /* Select current language */
    const char *current_lang = config_get_language(config);
    GtkTreeIter l_iter;
    GtkTreeModel *l_model = GTK_TREE_MODEL(gtk_combo_box_get_model(dlg->language_combo));
    if (gtk_tree_model_get_iter_first(l_model, &l_iter)) {
        do {
            gchar *code = NULL;
            gtk_tree_model_get(l_model, &l_iter, 1, &code, -1);
            if (code) {
                if (!current_lang || current_lang[0] == '\0') {
                    if (code[0] == '\0') {
                        gtk_combo_box_set_active_iter(dlg->language_combo, &l_iter);
                        g_free(code);
                        break;
                    }
                } else if (g_strcmp0(code, current_lang) == 0) {
                    gtk_combo_box_set_active_iter(dlg->language_combo, &l_iter);
                    g_free(code);
                    break;
                }
                g_free(code);
            }
        } while (gtk_tree_model_iter_next(l_model, &l_iter));
    }

    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->language_combo), FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);

    /* ---- Max Duration ---- */
    GtkWidget *dur_label = gtk_label_new("Max Recording Duration (seconds):");
    gtk_label_set_xalign(GTK_LABEL(dur_label), 0);
    gtk_box_pack_start(GTK_BOX(vbox), dur_label, FALSE, FALSE, 0);

    dlg->duration_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5, 30, 1));
    gtk_spin_button_set_value(dlg->duration_spin, (gdouble)config_get_max_duration(config));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(dlg->duration_spin), FALSE, TRUE, 0);

    dlg->duration_error = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(GTK_LABEL(dlg->duration_error), 0);
    gtk_widget_set_no_show_all(GTK_WIDGET(dlg->duration_error), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(dlg->duration_error), FALSE);
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

    /* ---- Test Connection Button and Status ---- */
    GtkWidget *test_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *test_btn = gtk_button_new_with_label("Test Connection");
    gtk_widget_set_sensitive(test_btn, whisper_client != NULL);
    dlg->test_status_label = GTK_LABEL(gtk_label_new(""));
    g_signal_connect(test_btn, "clicked", G_CALLBACK(on_test_connection_clicked), dlg);
    gtk_box_pack_start(GTK_BOX(test_box), test_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(test_box), GTK_WIDGET(dlg->test_status_label), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), test_box, FALSE, FALSE, 6);

    /* ---- Connect Save/Cancel buttons ---- */
    GtkWidget *save_btn = gtk_dialog_get_widget_for_response(dlg->dialog, GTK_RESPONSE_OK);
    GtkWidget *cancel_btn = gtk_dialog_get_widget_for_response(dlg->dialog, GTK_RESPONSE_CANCEL);

    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_clicked), dlg);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), dlg);

    /* MIN-002 fix: Ensure Escape key closes dialog */
    gtk_window_set_accept_focus(GTK_WINDOW(dlg->dialog), TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(cancel_btn));

    /* Show all widgets */
    gtk_widget_show_all(GTK_WIDGET(dlg->dialog));

    /* Run dialog */
    gint response = gtk_dialog_run(dlg->dialog);
    bool saved = (response == GTK_RESPONSE_OK);

    /* Destroy dialog */
    gtk_widget_destroy(GTK_WIDGET(dlg->dialog));
    g_free(dlg); /* MIN-004 fix: free heap-allocated ConfigDialog */

    return saved;
}
