#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/**
 * @file app_config.h
 * @brief Configuration file management — Load, save, and validate JSON configuration
 *
 * This module implements the configuration persistence layer described in
 * Section 9 of the SRS. It manages a JSON configuration file located at
 * `~/.config/transcriber/config.json` (or the path specified by the
 * TRANSCRIBER_CONFIG environment variable for testing).
 *
 * The configuration file stores all user-configurable settings including:
 *   - Whisper API server URL
 *   - Audio device selection
 *   - Language preferences
 *   - Maximum recording duration
 *   - Window position (x, y coordinates)
 *   - Notification preferences
 *
 * The module handles:
 *   - JSON file parsing with cJSON library
 *   - Automatic config directory creation (~/.config/transcriber/)
 *   - Default configuration generation on first run (CFG-012)
 *   - Config file validation and error recovery
 *   - Secure file permissions (0600 — owner read/write only) (NR-008)
 *
 * @see SRS Section 9: Configuration (CFG-001 through CFG-014)
 * @see SRS Section 4: Non-Functional Requirements (NR-008, NR-014)
 * @see SRS Section 9.4: Fixed (Non-Configurable) Settings
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Section 1: Configuration Structure
 *---------------------------------------------------------------------------
 * Contains all user-configurable application settings. This structure is
 * loaded from the JSON config file on startup and saved when the user
 * modifies settings via the Configuration Dialog.
 *
 * The structure matches the AppConfig typedef defined in app.h. This
 * header provides the authoritative documentation for each field.
 *
 * @see CFG-002: File Format (JSON)
 * @see SRS Section 9.2: Configurable Settings
 */
typedef struct _AppConfig {
    /* Whisper API Settings */
    char whisper_url[512];       ///< Whisper API base URL (e.g., "http://localhost:8000")
                                 ///< @see CFG-005: Whisper URL
                                 ///< @see WHISPER-001: Endpoint Configuration

    char language[16];           ///< Language code (ISO 639-1, e.g., "en", "es", "")
                                 ///< Empty string means auto-detect.
                                 ///< @see CFG-007: Language

    /* Audio Settings */
    char audio_device[256];      ///< Audio device name (backend-specific)
                                 ///< Examples: "default" (ALSA), "alsa_input.pci-..." (PulseAudio)
                                 ///< Empty string means system default.
                                 ///< @see CFG-006: Audio Device
                                 ///< @see AUD-008a: Microphone Selection

    int max_duration;            ///< Maximum recording duration in seconds (5-30)
                                 ///< @see CFG-014: Max Recording Duration
                                 ///< @see FR-007a: Maximum Session Limit

    /* Window Settings */
    int window_x;                ///< Window X position (screen coordinates, -1 = center)
                                 ///< @see CFG-004: Window Position

    int window_y;                ///< Window Y position (screen coordinates, -1 = center)
                                 ///< @see CFG-004: Window Position

    /* UI Settings */
    bool enable_notifications;   ///< Enable desktop notifications for critical errors
                                 ///< @see CFG-009: Notification
                                 ///< @see ERR-006: Desktop Notifications
} AppConfig;

/*---------------------------------------------------------------------------
 * Section 2: Configuration File Path
 *---------------------------------------------------------------------------
 * Functions for determining and managing the configuration file path.
 */

/**
 * Get the default configuration directory path.
 *
 * This function returns the path to the configuration directory, which
 * is `~/.config/transcriber/` by default. The directory is determined
 * as follows:
 *   1. Check XDG_CONFIG_HOME environment variable (if set, use $XDG_CONFIG_HOME/transcriber/)
 *   2. Otherwise, use ~/.config/transcriber/
 *
 * The returned string is dynamically allocated and must be freed by the
 * caller using free().
 *
 * @return A dynamically allocated string containing the config directory path,
 *         or NULL on allocation failure.
 *
 * @see CFG-001: File Location (~/.config/transcriber/config.json)
 */
char* config_get_default_directory(void);

/**
 * Get the default configuration file path.
 *
 * This function returns the full path to the configuration file, which
 * is `~/.config/transcriber/config.json` by default. The path is
 * constructed by appending "config.json" to the config directory path.
 *
 * The returned string is dynamically allocated and must be freed by the
 * caller using free().
 *
 * @return A dynamically allocated string containing the config file path,
 *         or NULL on allocation failure.
 *
 * @see CFG-001: File Location
 */
char* config_get_default_path(void);

/**
 * Get the configuration file path to use (from environment or default).
 *
 * This function checks the TRANSCRIBER_CONFIG environment variable. If
 * set, it returns the path from the environment variable (useful for
 * testing). Otherwise, it returns the default config file path.
 *
 * The returned string is dynamically allocated and must be freed by the
 * caller using free().
 *
 * @return A dynamically allocated string containing the config file path,
 *         or NULL on allocation failure.
 */
char* config_get_path(void);

/*---------------------------------------------------------------------------
 * Section 3: Default Configuration
 *---------------------------------------------------------------------------
 * Functions for obtaining the default configuration values.
 *
 * The default configuration is used when:
 *   - The config file does not exist (first run) — CFG-012
 *   - A config field is missing from the JSON — uses default for that field
 *   - The config file is corrupted — falls back to full defaults
 *
 * @see CFG-003: Default Configuration
 * @see CFG-012: Auto-Create Configuration File
 */

/**
 * Initialize an AppConfig struct with default values.
 *
 * This function sets all fields in the AppConfig struct to their
 * default values as specified in the SRS. The defaults are:
 *   - whisper_url: "http://localhost:8080/v1/audio/transcriptions"
 *   - language: "en" (English)
 *   - audio_device: "system_default" (system default microphone)
 *   - max_duration: 30 (30 seconds)
 *   - window_x: 100 (default horizontal position)
 *   - window_y: 100 (default vertical position)
 *   - enable_notifications: true
 *
 * @param config Pointer to an AppConfig struct to initialize. Must not be NULL.
 *
 * @see CFG-003: Default Configuration
 */
void config_set_defaults(AppConfig* config);

/*---------------------------------------------------------------------------
 * Section 4: Configuration File Load/Save
 *---------------------------------------------------------------------------
 * Functions for loading and saving the JSON configuration file.
 *
 * The load function handles missing files gracefully (creates defaults)
 * and recovers from corrupted files (logs error, uses defaults).
 * The save function ensures atomic writes via temporary file + rename
 * to prevent corruption on power loss.
 *
 * @see CFG-011: Load on Startup
 * @see CFG-027: Save Configuration
 * @see NR-014: Configuration Persistence
 */

/**
 * Load configuration from the JSON file.
 *
 * This function attempts to load the configuration from the JSON file
 * at the default path (~/.config/transcriber/config.json). The function
 * performs the following steps:
 *   1. Check if the config file exists
 *   2. If not exists: Initialize config with defaults, create directory,
 *      and save the default config (CFG-012: Auto-Create)
 *   3. If exists: Read and parse the JSON file using cJSON
 *   4. Validate each field and apply defaults for missing/invalid values
 *   5. Return the loaded configuration
 *
 * If the JSON file is corrupted or contains invalid data, the function
 * logs an error message and falls back to default values for the
 * affected fields. The application continues to function with partial
 * or full defaults.
 *
 * @param config Pointer to an AppConfig struct to populate. Must not be NULL.
 * @return true if the configuration was loaded successfully (file exists
 *         and is valid), false if:
 *         - File does not exist (defaults created — this is NOT an error)
 *         - File is corrupted (defaults used for invalid fields)
 *         - Memory allocation failure
 *
 * @note A return value of false does NOT mean the config is unusable.
 *       The config struct will contain valid values (defaults) even on
 *       failure. The return value indicates whether the file was loaded
 *       successfully, not whether the config is valid.
 *
 * @see CFG-011: Load on Startup
 * @see CFG-012: Auto-Create Configuration File
 * @see ERR-001: File System Errors
 */
bool config_load(AppConfig* config);

/**
 * Load configuration from a specific file path.
 *
 * This function is identical to config_load() but allows specifying a
 * custom file path. This is useful for testing or for loading alternate
 * configuration profiles.
 *
 * @param config Pointer to an AppConfig struct to populate. Must not be NULL.
 * @param path   Path to the JSON config file. Must not be NULL.
 * @return true if loaded successfully, false on error (defaults used).
 */
bool config_load_from_path(AppConfig* config, const char* path);

/**
 * Save configuration to the JSON file.
 *
 * This function writes the configuration to the JSON file at the default
 * path. The function performs an atomic write to prevent corruption:
 *   1. Create a temporary file in the config directory (config.json.tmp)
 *   2. Write the JSON data to the temporary file
 *   3. Set file permissions to 0600 (owner read/write only)
 *   4. Flush and close the temporary file
 *   5. Rename the temporary file to config.json (atomic on same filesystem)
 *
 * If any step fails, the original config file is preserved (not corrupted).
 * The temporary file is cleaned up on error.
 *
 * @param config Pointer to an AppConfig struct to save. Must not be NULL.
 * @return true if the configuration was saved successfully, false if:
 *         - Config directory cannot be created
 *         - Temporary file creation fails
 *         - JSON serialization fails
 *         - File write fails
 *         - Rename fails
 *
 * @see CFG-027: Save Configuration
 * @see NR-008: Configuration Security (0600 permissions)
 * @see NR-014: Configuration Persistence
 */
bool config_save(const AppConfig* config);

/**
 * Save configuration to a specific file path.
 *
 * This function is identical to config_save() but allows specifying a
 * custom file path. This is useful for testing or for saving alternate
 * configuration profiles.
 *
 * @param config Pointer to an AppConfig struct to save. Must not be NULL.
 * @param path   Path to the JSON config file. Must not be NULL.
 * @return true if saved successfully, false on error.
 */
bool config_save_to_path(const AppConfig* config, const char* path);

/*---------------------------------------------------------------------------
 * Section 5: Configuration Validation
 *---------------------------------------------------------------------------
 * Functions for validating configuration field values.
 */

/**
 * Validate a configuration struct and report any issues.
 *
 * This function checks each field in the AppConfig struct for validity
 * and reports any issues. Validation rules:
 *   - whisper_url: Must be a valid URL (starts with http:// or https://)
 *   - language: Must be empty (auto) or valid ISO 639-1 code (2 letters)
 *   - audio_device: Any non-empty string is valid (backend validates at runtime)
 *   - max_duration: Must be in range [5, 30]
 *   - window_x, window_y: Must be >= -1 (-1 means center)
 *
 * @param config Pointer to an AppConfig struct to validate. Must not be NULL.
 * @return true if all fields are valid, false if any field is invalid.
 *         Invalid fields are NOT modified — the caller can use
 *         config_get_error() to determine which field failed validation.
 */
bool config_validate(const AppConfig* config);

/*---------------------------------------------------------------------------
 * Section 6: Field Accessors (Convenience Functions)
 *---------------------------------------------------------------------------
 * Getter and setter functions for individual configuration fields.
 * These provide a clean API for the configuration dialog to update
 * individual fields without manipulating the struct directly.
 */

/**
 * Set the Whisper API URL.
 * @param config Pointer to AppConfig. Must not be NULL.
 * @param url    URL string. Copied internally. Must not be NULL.
 * @return true if valid, false if URL is too long or invalid format.
 */
bool config_set_whisper_url(AppConfig* config, const char* url);

/**
 * Get the Whisper API URL.
 * @param config Pointer to AppConfig. Must not be NULL.
 * @return The whisper_url string (internal, must NOT be freed or modified).
 */
const char* config_get_whisper_url(const AppConfig* config);

/**
 * Set the language code.
 * @param config  Pointer to AppConfig. Must not be NULL.
 * @param language Language code (ISO 639-1) or empty string for auto-detect.
 *                 Copied internally. Must not be NULL.
 * @return true if valid, false if language code is invalid.
 */
bool config_set_language(AppConfig* config, const char* language);

/**
 * Get the language code.
 * @param config Pointer to AppConfig. Must not be NULL.
 * @return The language string (internal, must NOT be freed or modified).
 */
const char* config_get_language(const AppConfig* config);

/**
 * Set the audio device name.
 * @param config  Pointer to AppConfig. Must not be NULL.
 * @param device  Audio device name. Copied internally. May be NULL or empty
 *                to use system default.
 * @return true if valid, false if device name is too long.
 */
bool config_set_audio_device(AppConfig* config, const char* device);

/**
 * Get the audio device name.
 * @param config Pointer to AppConfig. Must not be NULL.
 * @return The audio_device string (internal, must NOT be freed or modified).
 */
const char* config_get_audio_device(const AppConfig* config);

/**
 * Set the maximum recording duration.
 * @param config     Pointer to AppConfig. Must not be NULL.
 * @param duration   Duration in seconds. Must be in range [5, 30].
 * @return true if valid, false if out of range.
 */
bool config_set_max_duration(AppConfig* config, int duration);

/**
 * Get the maximum recording duration.
 * @param config Pointer to AppConfig. Must not be NULL.
 * @return The max_duration value in seconds.
 */
int config_get_max_duration(const AppConfig* config);

/**
 * Set the window position.
 * @param config Pointer to AppConfig. Must not be NULL.
 * @param x      X coordinate (-1 to center).
 * @param y      Y coordinate (-1 to center).
 */
void config_set_window_position(AppConfig* config, int x, int y);

/**
 * Get the window X position.
 * @param config Pointer to AppConfig. Must not be NULL.
 * @return The window_x value.
 */
int config_get_window_x(const AppConfig* config);

/**
 * Get the window Y position.
 * @param config Pointer to AppConfig. Must not be NULL.
 * @return The window_y value.
 */
int config_get_window_y(const AppConfig* config);

/**
 * Set the notification enable flag.
 * @param config  Pointer to AppConfig. Must not be NULL.
 * @param enable  true to enable desktop notifications, false to disable.
 */
void config_set_notifications(AppConfig* config, bool enable);

/**
 * Get the notification enable flag.
 * @param config Pointer to AppConfig. Must not be NULL.
 * @return true if notifications are enabled, false otherwise.
 */
bool config_get_notifications(const AppConfig* config);

/*---------------------------------------------------------------------------
 * Section 7: Error Handling and Diagnostics
 *---------------------------------------------------------------------------
 * Functions for retrieving error information.
 */

/**
 * Get the last error message from the config module.
 *
 * @return A null-terminated string containing the error message, or an
 *         empty string if no error has occurred. The returned string is
 *         static and must NOT be freed.
 */
const char* config_get_error(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
