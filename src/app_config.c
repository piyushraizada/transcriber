/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/**
 * @file app_config.c
 * @brief Configuration file management implementation
 *
 * Implements load, save, validate, and accessor functions for the
 * JSON configuration file at ~/.config/transcriber/config.json.
 *
 * @see app_config.h
 * @see SRS Section 9: Configuration (CFG-001 through CFG-014)
 */

#include "app_config.h"
#include "app_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <libgen.h>
#include <glib.h>
#include <cjson/cJSON.h>

/*---------------------------------------------------------------------------
 * Static error buffer — protected by mutex for thread safety (M5-001 fix)
 *---------------------------------------------------------------------------*/

static char g_config_error[256] = {0};
static pthread_mutex_t g_config_error_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_error(const char* msg)
{
    pthread_mutex_lock(&g_config_error_mutex);
    if (msg) {
        snprintf(g_config_error, sizeof(g_config_error), "%s", msg);
    } else {
        g_config_error[0] = '\0';
    }
    pthread_mutex_unlock(&g_config_error_mutex);
}

const char* config_get_error(void)
{
    /* HI-05 fix: Lock mutex, copy to thread-local buffer, unlock, then return. */
    static __thread char local_buffer[256] = {0};
    pthread_mutex_lock(&g_config_error_mutex);
    snprintf(local_buffer, sizeof(local_buffer), "%s", g_config_error);
    pthread_mutex_unlock(&g_config_error_mutex);
    return local_buffer;
}

/*---------------------------------------------------------------------------
 * Helper: create directory recursively (like mkdir -p)
 *---------------------------------------------------------------------------*/

static bool mkdirs(const char* path, mode_t mode)
{
    char tmp[1024];
    char* p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    /* Remove trailing slash */
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return false;
    }

    return true;
}

/*---------------------------------------------------------------------------
 * Helper: expand ~ to home directory
 *---------------------------------------------------------------------------*/

static char* expand_tilde(const char* path)
{
    const char* home = getenv("HOME");
    if (!home) {
        /* Fallback — HOME not set, return copy of original path */
        char* result = strdup(path);
        if (!result) {
            set_error("Memory allocation failed in expand_tilde (strdup)");
        }
        return result;
    }

    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        size_t home_len = strlen(home);
        size_t rest_len = strlen(path + 1);
        char* expanded = malloc(home_len + rest_len + 1);
        if (expanded) {
            memcpy(expanded, home, home_len);
            memcpy(expanded + home_len, path + 1, rest_len);
            expanded[home_len + rest_len] = '\0';
        } else {
            set_error("Memory allocation failed in expand_tilde (malloc)");
        }
        return expanded;
    }

    char* result = strdup(path);
    if (!result) {
        set_error("Memory allocation failed in expand_tilde (strdup)");
    }
    return result;
}

/*---------------------------------------------------------------------------
 * Section 2: Configuration File Path
 *---------------------------------------------------------------------------*/

char* config_get_default_directory(void)
{
    const char* xdg = getenv("XDG_CONFIG_HOME");

    /* We need HOME for the fallback path */
    const char* home = getenv("HOME");
    if (!home) {
        set_error("HOME environment variable not set");
        return NULL;
    }

    if (!xdg) {
        /* Build ~/.config/transcriber/ */
        size_t len = strlen(home) + strlen("/.config/transcriber/") + 1;
        char* dir = malloc(len);
        if (!dir) {
            set_error("Memory allocation failed");
            return NULL;
        }
        snprintf(dir, len, "%s/.config/transcriber", home);
        return dir;
    }

    /* Use $XDG_CONFIG_HOME/transcriber/ */
    size_t len = strlen(xdg) + strlen("/transcriber") + 1;
    char* dir = malloc(len);
    if (!dir) {
        set_error("Memory allocation failed");
        return NULL;
    }
    snprintf(dir, len, "%s/transcriber", xdg);
    return dir;
}

char* config_get_default_path(void)
{
    char* dir = config_get_default_directory();
    if (!dir) {
        return NULL;
    }

    size_t len = strlen(dir) + strlen("/config.json") + 1;
    char* path = malloc(len);
    if (path) {
        snprintf(path, len, "%s/config.json", dir);
    } else {
        set_error("Memory allocation failed");
    }
    free(dir);
    return path;
}

char* config_get_path(void)
{
    const char* env_path = getenv("TRANSCRIBER_CONFIG");
    if (env_path && env_path[0] != '\0') {
        return expand_tilde(env_path);
    }
    return config_get_default_path();
}

/*---------------------------------------------------------------------------
 * Section 3: Default Configuration
 *---------------------------------------------------------------------------*/

void config_set_defaults(AppConfig* config)
{
    if (!config) {
        set_error("NULL config pointer");
        return;
    }

    /* CFG-003: Default Configuration */
    strncpy(config->model_path, "ggml-large-v3-turbo-q8_0.bin", sizeof(config->model_path) - 1);
    config->model_path[sizeof(config->model_path) - 1] = '\0';

    strncpy(config->audio_device, "default", sizeof(config->audio_device) - 1);
    config->audio_device[sizeof(config->audio_device) - 1] = '\0';

    config->audio_device_display_name[0] = '\0';  /* No display name by default */

    config->max_duration = 30;  /* 30 seconds (SRS default) */

    config->window_x = 100;  /* Default position */
    config->window_y = 100;  /* Default position */

    /* GPU mode — default to "auto" (select best GPU by free memory) */
    strncpy(config->gpu_mode, gpu_mode_get_default(), sizeof(config->gpu_mode) - 1);
    config->gpu_mode[sizeof(config->gpu_mode) - 1] = '\0';

    set_error(NULL);
}

/*---------------------------------------------------------------------------
 * Section 4: Configuration File Load/Save
 *---------------------------------------------------------------------------*/

bool config_load(AppConfig* config)
{
    if (!config) {
        set_error("NULL config pointer");
        return false;
    }

    char* path = config_get_path();
    if (!path) {
        /* Cannot determine path — use defaults */
        config_set_defaults(config);
        return false;
    }

    bool result = config_load_from_path(config, path);
    free(path);
    return result;
}

bool config_load_from_path(AppConfig* config, const char* path)
{
    if (!config || !path) {
        set_error("NULL parameter");
        return false;
    }

    /* Initialize with defaults first */
    config_set_defaults(config);

    /* Check if file exists */
    FILE* fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            /* File does not exist — create it with defaults (CFG-012).
             * Return true because config has been populated with valid
             * defaults and the file has been created. The caller does not
             * need to treat this as a failure. */
            set_error(NULL);
            config_save_to_path(config, path);
            return true;
        }
        set_error("Failed to open config file");
        return false;
    }

    /* Get file size */
    /* M-004 fix: Use off_t + ftello for portable file size handling */
    fseek(fp, 0, SEEK_END);
    off_t fsize = ftello(fp);
    fseeko(fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > (off_t)(1024 * 1024)) {
        /* File too small or too large */
        fclose(fp);
        set_error("Config file has invalid size");
        return false;
    }

    /* Read file content
     * MED-12 fix: The cast (size_t)fsize is safe because fsize is bounded
     * to [1, 1MB] by the check above. On 32-bit systems, size_t is 32-bit,
     * but 1MB fits comfortably in 32 bits (max ~4GB). */
    char* content = malloc((size_t)fsize + 1);
    if (!content) {
        fclose(fp);
        set_error("Memory allocation failed");
        return false;
    }

    size_t read = fread(content, 1, (size_t)fsize, fp);
    fclose(fp);
    content[read] = '\0';

    /* Parse JSON */
    cJSON* root = cJSON_Parse(content);
    free(content);

    if (!root) {
        set_error("Failed to parse JSON config file");
        return false;
    }

    /* Extract fields with validation */
    cJSON* item;

    /* Read "model_path" from config.
     * Legacy "whisper_url" key from pre-migration config files is no longer
     * supported — users must update their config.json to use "model_path". */
    item = cJSON_GetObjectItemCaseSensitive(root, "model_path");
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(config->model_path, item->valuestring, sizeof(config->model_path) - 1);
        config->model_path[sizeof(config->model_path) - 1] = '\0';
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "audio_device");
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(config->audio_device, item->valuestring, sizeof(config->audio_device) - 1);
        config->audio_device[sizeof(config->audio_device) - 1] = '\0';
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "audio_device_display_name");
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(config->audio_device_display_name, item->valuestring, sizeof(config->audio_device_display_name) - 1);
        config->audio_device_display_name[sizeof(config->audio_device_display_name) - 1] = '\0';
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "max_duration");
    if (item && cJSON_IsNumber(item)) {
        int dur = (int)item->valueint;
        if (dur < 5) config->max_duration = 5;
        else if (dur > 30) config->max_duration = 30;
        else config->max_duration = dur;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "window_position");
    if (item && cJSON_IsObject(item)) {
        cJSON *wx = cJSON_GetObjectItemCaseSensitive(item, "x");
        cJSON *wy = cJSON_GetObjectItemCaseSensitive(item, "y");
        if (cJSON_IsNumber(wx)) config->window_x = (int)wx->valueint;
        if (cJSON_IsNumber(wy)) config->window_y = (int)wy->valueint;
    } else {
        /* Backward compatibility with flat window_x/window_y keys */
        item = cJSON_GetObjectItemCaseSensitive(root, "window_x");
        if (item && cJSON_IsNumber(item)) {
            config->window_x = (int)item->valueint;
        }
        item = cJSON_GetObjectItemCaseSensitive(root, "window_y");
        if (item && cJSON_IsNumber(item)) {
            config->window_y = (int)item->valueint;
        }
    }

    /* GPU mode */
    item = cJSON_GetObjectItemCaseSensitive(root, "gpu_mode");
    if (item && cJSON_IsString(item) && item->valuestring) {
        if (gpu_mode_validate(item->valuestring)) {
            strncpy(config->gpu_mode, item->valuestring, sizeof(config->gpu_mode) - 1);
            config->gpu_mode[sizeof(config->gpu_mode) - 1] = '\0';
        } else {
            g_log("app-config", G_LOG_LEVEL_MESSAGE, "[config] Invalid gpu_mode \"%s\", using default\n", item->valuestring);
        }
    }

    cJSON_Delete(root);
    set_error(NULL);
    return true;
}

bool config_save(const AppConfig* config)
{
    if (!config) {
        set_error("NULL config pointer");
        return false;
    }

    char* path = config_get_path();
    if (!path) {
        return false;
    }

    bool result = config_save_to_path(config, path);
    free(path);
    return result;
}

bool config_save_to_path(const AppConfig* config, const char* path)
{
    if (!config || !path) {
        set_error("NULL parameter");
        return false;
    }

    /* Create config directory if needed */
    char* path_copy = strdup(path);
    if (!path_copy) {
        set_error("Memory allocation failed");
        return false;
    }

    char* dir = dirname(path_copy);
    if (!mkdirs(dir, 0700)) {
        free(path_copy);
        set_error("Failed to create config directory");
        return false;
    }
    free(path_copy);

    /* Build JSON object */
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        set_error("Failed to create JSON object");
        return false;
    }

    cJSON_AddStringToObject(root, "model_path", config->model_path);
    cJSON_AddStringToObject(root, "audio_device", config->audio_device);
    cJSON_AddStringToObject(root, "audio_device_display_name", config->audio_device_display_name);
    cJSON_AddNumberToObject(root, "max_duration", config->max_duration);

    cJSON *window_pos = cJSON_CreateObject();
    cJSON_AddNumberToObject(window_pos, "x", config->window_x);
    cJSON_AddNumberToObject(window_pos, "y", config->window_y);
    cJSON_AddItemToObject(root, "window_position", window_pos);

    cJSON_AddStringToObject(root, "gpu_mode", config->gpu_mode);

    /* Print to string with indentation */
    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        set_error("Failed to serialize JSON");
        return false;
    }

    /* Atomic write: write to temp file, then rename */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE* fp = fopen(tmp_path, "w");
    if (!fp) {
        free(json_str);
        set_error("Failed to create temporary config file");
        return false;
    }

    if (fprintf(fp, "%s\n", json_str) < 0) {
        free(json_str);
        fclose(fp);
        unlink(tmp_path);
        set_error("Failed to write config file");
        return false;
    }
    free(json_str);

    /* Flush and sync */
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    /* Set permissions before rename (NR-008) */
    if (chmod(tmp_path, 0600) != 0) {
        unlink(tmp_path);
        set_error("Failed to set config file permissions");
        return false;
    }

    /* Atomic rename */
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        set_error("Failed to rename config file");
        return false;
    }

    set_error(NULL);
    return true;
}

/*---------------------------------------------------------------------------
 * Section 5: Configuration Validation
 *---------------------------------------------------------------------------*/

bool config_validate(const AppConfig* config)
{
    if (!config) {
        set_error("NULL config pointer");
        return false;
    }

    /* Validate model_path — must not be empty */
    if (config->model_path[0] == '\0') {
        set_error("model_path is empty");
        return false;
    }

    /* Validate max_duration — must be in [5, 30] per SRS */
    if (config->max_duration < 5 || config->max_duration > 30) {
        set_error("max_duration must be between 5 and 30 seconds");
        return false;
    }

    /* Validate window position — must be >= -1 */
    if (config->window_x < -1 || config->window_y < -1) {
        set_error("window position must be >= -1");
        return false;
    }

    set_error(NULL);
    return true;
}

/*---------------------------------------------------------------------------
 * Section 6: Field Accessors
 *---------------------------------------------------------------------------*/

bool config_set_model_path(AppConfig* config, const char* path)
{
    if (!config || !path) {
        set_error("NULL parameter");
        return false;
    }
    if (strlen(path) >= sizeof(config->model_path)) {
        set_error("Model path too long");
        return false;
    }
    strncpy(config->model_path, path, sizeof(config->model_path) - 1);
    config->model_path[sizeof(config->model_path) - 1] = '\0';
    return true;
}

const char* config_get_model_path(const AppConfig* config)
{
    if (!config) return "";
    return config->model_path;
}

bool config_set_audio_device(AppConfig* config, const char* device)
{
    if (!config) {
        set_error("NULL config");
        return false;
    }
    if (device) {
        if (strlen(device) >= sizeof(config->audio_device)) {
            set_error("device name too long");
            return false;
        }
        strncpy(config->audio_device, device, sizeof(config->audio_device) - 1);
        config->audio_device[sizeof(config->audio_device) - 1] = '\0';
    } else {
        config->audio_device[0] = '\0';
    }
    return true;
}

const char* config_get_audio_device(const AppConfig* config)
{
    if (!config) return "";
    return config->audio_device;
}

bool config_set_audio_device_display_name(AppConfig* config, const char* name)
{
    if (!config) {
        set_error("NULL config");
        return false;
    }
    if (name) {
        if (strlen(name) >= sizeof(config->audio_device_display_name)) {
            set_error("display name too long");
            return false;
        }
        strncpy(config->audio_device_display_name, name, sizeof(config->audio_device_display_name) - 1);
        config->audio_device_display_name[sizeof(config->audio_device_display_name) - 1] = '\0';
    } else {
        config->audio_device_display_name[0] = '\0';
    }
    return true;
}

const char* config_get_audio_device_display_name(const AppConfig* config)
{
    if (!config) return "";
    return config->audio_device_display_name;
}

bool config_set_max_duration(AppConfig* config, int duration)
{
    if (!config) {
        set_error("NULL config");
        return false;
    }
    if (duration < 5) duration = 5;
    if (duration > 30) duration = 30;
    config->max_duration = duration;
    return true;
}

int config_get_max_duration(const AppConfig* config)
{
    if (!config) return 30;
    return config->max_duration;
}

void config_set_window_position(AppConfig* config, int x, int y)
{
    if (!config) return;
    config->window_x = x;
    config->window_y = y;
}

int config_get_window_x(const AppConfig* config)
{
    if (!config) return -1;
    return config->window_x;
}

int config_get_window_y(const AppConfig* config)
{
    if (!config) return -1;
    return config->window_y;
}

bool config_set_gpu_mode(AppConfig* config, const char* mode)
{
    if (!config || !mode) {
        set_error("NULL parameter");
        return false;
    }
    if (!gpu_mode_validate(mode)) {
        set_error("Invalid GPU mode string");
        return false;
    }
    if (strlen(mode) >= sizeof(config->gpu_mode)) {
        set_error("GPU mode string too long");
        return false;
    }
    strncpy(config->gpu_mode, mode, sizeof(config->gpu_mode) - 1);
    config->gpu_mode[sizeof(config->gpu_mode) - 1] = '\0';
    return true;
}

const char* config_get_gpu_mode(const AppConfig* config)
{
    if (!config) return "";
    return config->gpu_mode;
}

