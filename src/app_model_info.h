/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_MODEL_INFO_H
#define APP_MODEL_INFO_H

#include <stdbool.h>

/**
 * Model metadata extracted from a GGUF Whisper model file.
 */
typedef struct {
    char model_name[64];       // Human-readable model type (e.g., "base", "small", "medium")
    char quantization[32];     // Quantization descriptor from whisper_model_type_readable
    bool multilingual;         // true if model supports multiple languages
    bool valid;                // true if the file is a valid Whisper model
    char error_message[256];   // Error details if valid is false
} ModelInfo;

/**
 * Load a Whisper model file and extract metadata.
 *
 * This function temporarily loads the model context to read metadata,
 * then immediately frees it. The caller should use the result to display
 * model information in the UI.
 *
 * @param model_path Path to the .bin GGUF model file
 * @param info Output struct to populate with metadata
 * @return true if metadata was successfully extracted, false on error
 */
bool model_info_load(const char *model_path, ModelInfo *info);

/**
 * Initialize a ModelInfo struct with default/empty values.
 */
void model_info_init(ModelInfo *info);

#endif
