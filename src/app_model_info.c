/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * app_model_info.c - GGUF Whisper model metadata extraction
 *
 * Parses the GGUF file header directly to extract metadata (model name,
 * quantization type, multilingual support) WITHOUT loading the full
 * whisper model context. This avoids the expensive and potentially
 * crash-prone whisper_init_from_file_with_params() call.
 *
 * The GGUF format stores all metadata as key-value pairs in the file
 * header, which is typically only a few KB. By parsing this directly,
 * we avoid loading the entire model (which can be GBs) just to read
 * a few string attributes.
 *
 * GGUF spec: https://github.com/ggml-org/ggml/blob/master/docs/gguf.md
 */

#include "app_model_info.h"

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

/* Forward declaration - implemented in app_whisper.c */
extern int whisper_resolve_model_path(const char *input, char *output, size_t out_size);

/* ===================================================================
 * GGUF constants and types
 * =================================================================== */

/* GGUF magic bytes: "GGUF" in little-endian */
#define GGUF_MAGIC 0x46554747u

/* GGML magic bytes: The file stores "ggml" as bytes 6c 6d 67 67.
 * When read as little-endian uint32 via read_u32_le, this becomes 0x67676d6c. */
#define GGML_MAGIC 0x67676d6cu

/* GGUF value types */
#define GGUF_TYPE_UINT8   0
#define GGUF_TYPE_INT8    1
#define GGUF_TYPE_UINT16  2
#define GGUF_TYPE_INT16   3
#define GGUF_TYPE_UINT32  4
#define GGUF_TYPE_INT32   5
#define GGUF_TYPE_FLOAT32 6
#define GGUF_TYPE_BOOL    7
#define GGUF_TYPE_STRING  8
#define GGUF_TYPE_ARRAY   9
#define GGUF_TYPE_UINT64  10
#define GGUF_TYPE_INT64   11
#define GGUF_TYPE_FLOAT64 12

/* file_type values for quantization */
static const char *ftype_to_quant_string(int ftype) {
    switch (ftype) {
        case 0:  return "F32";
        case 1:  return "F16";
        case 2:  return "Q4_0";
        case 3:  return "Q4_1";
        case 5:  return "Q4_1 (some F16)";
        case 6:  return "Q5_0";
        case 7:  return "Q5_1";
        case 8:  return "Q8_0";
        case 10: return "Q8_0 (some Q6_K)";
        case 12: return "Q4_K_S";
        case 13: return "Q4_K_M";
        case 14: return "Q4_K_L";
        case 15: return "Q5_K_S";
        case 16: return "Q5_K_M";
        case 17: return "Q5_K_L";
        case 18: return "Q6_K";
        case 19: return "IQ1_S";
        case 20: return "IQ1_M";
        case 21: return "Q2_K_S";
        case 22: return "Q2_K";
        case 23: return "Q2_K_M";
        case 24: return "Q2_K_L";
        case 25: return "Q3_K_S";
        case 26: return "Q3_K_M";
        case 27: return "Q3_K_L";
        case 28: return "Q4_0 (via Q4_K)";
        case 29: return "TQ1_0";
        case 30: return "TQ2_0";
        default: return "unknown";
    }
}

/* ===================================================================
 * Little-endian byte reading helpers
 * =================================================================== */
static uint32_t read_u32_le(const unsigned char *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static uint64_t read_u64_le(const unsigned char *buf) {
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= ((uint64_t)buf[i] << (i * 8));
    }
    return val;
}

/* ===================================================================
 * Public API
 * =================================================================== */

void model_info_init(ModelInfo *info) {
    if (!info) return;
    info->model_name[0] = '\0';
    info->quantization[0] = '\0';
    info->multilingual = false;
    info->valid = false;
    info->error_message[0] = '\0';
}

/* HIGH-7 fix: model_info_load() now returns false on I/O errors and true
 * only when metadata was successfully parsed. Callers should check the
 * return value first, then check info.valid for format validation. */
bool model_info_load(const char *model_path, ModelInfo *info) {
    if (!model_path || !info) return false;

    model_info_init(info);

    /* Resolve the path (handle tilde expansion and directory search) */
    char resolved[PATH_MAX];
    int path_result = whisper_resolve_model_path(model_path, resolved, sizeof(resolved));
    /* LOW-17 fix: Use distinct return codes for better error messages */
    if (path_result == -2) {
        /* Path resolution failed (invalid input, no HOME, etc.) */
        snprintf(info->error_message, sizeof(info->error_message),
            "Invalid model path: %s", model_path);
        return false;
    }
    if (path_result != 0) {
        /* Path resolved but file does not exist */
        snprintf(info->error_message, sizeof(info->error_message),
            "Model file not found: %s", model_path);
        return false;
    }

    /* Open the file */
    FILE *f = fopen(resolved, "rb");
    if (!f) {
        snprintf(info->error_message, sizeof(info->error_message),
            "Cannot open model file: %.200s", resolved);
        return false;
    }

    /* Read magic to determine format */
    unsigned char magic_buf[4];
    if (fread(magic_buf, 1, 4, f) != 4) {
        snprintf(info->error_message, sizeof(info->error_message),
            "Failed to read model file header");
        fclose(f);
        return false;
    }
    uint32_t magic = read_u32_le(magic_buf);

    if (magic == GGML_MAGIC) {
        /* ----------------------------------------------------------------
         * Parse GGML format (old Whisper model binary format)
         * Header: magic(4) + hparams(44) = 48 bytes total
         * Hparams: n_vocab, n_audio_ctx, n_audio_state, n_audio_head,
         *          n_audio_layer, n_text_ctx, n_text_state, n_text_head,
         *          n_text_layer, n_mels, ftype  (each uint32)
         * ---------------------------------------------------------------- */
        unsigned char hparams[44];
        if (fread(hparams, 1, 44, f) != 44) {
            snprintf(info->error_message, sizeof(info->error_message),
                "Failed to read GGML hparams");
            fclose(f);
            return false;
        }

        uint32_t n_vocab = read_u32_le(hparams + 0);
        uint32_t n_audio_layer = read_u32_le(hparams + 16);
        uint32_t ftype_raw = read_u32_le(hparams + 40);

        /* Extract quantization version factor - same as whisper.cpp */
        const uint32_t GGML_QNT_VERSION_FACTOR = 1000;
        uint32_t qntvr = ftype_raw / GGML_QNT_VERSION_FACTOR;
        int file_type = (int)(ftype_raw % GGML_QNT_VERSION_FACTOR);
        (void)qntvr;

        /* Determine model type from n_audio_layer */
        char model_name_buf[64] = "whisper";
        const char *mver = "";
        if (n_audio_layer == 4) {
            strcpy(model_name_buf, "tiny");
        } else if (n_audio_layer == 6) {
            strcpy(model_name_buf, "base");
        } else if (n_audio_layer == 12) {
            strcpy(model_name_buf, "small");
        } else if (n_audio_layer == 24) {
            strcpy(model_name_buf, "medium");
        } else if (n_audio_layer == 32) {
            strcpy(model_name_buf, "large");
            if (n_vocab == 51866) {
                mver = " v3";
            }
        }

        /* Multilingual detection: vocab > 51864 indicates multilingual */
        bool is_multilingual = (n_vocab >= 51865);

        snprintf(info->model_name, sizeof(info->model_name), "%s%s", model_name_buf, mver);
        snprintf(info->quantization, sizeof(info->quantization), "%s",
                 ftype_to_quant_string(file_type));
        info->multilingual = is_multilingual;
        info->valid = true;

        fclose(f);
        return true;
    }

    if (magic != GGUF_MAGIC) {
        snprintf(info->error_message, sizeof(info->error_message),
            "Not a valid model file (bad magic: 0x%08x)", magic);
        fclose(f);
        return false;
    }

    /* ----------------------------------------------------------------
     * Parse GGUF header
     * ---------------------------------------------------------------- */

    /* Read version (4) + tensor_count (8) + metadata_kv_count (8) = 20 more bytes */
    unsigned char header[20];
    if (fread(header, 1, 20, f) != 20) {
        snprintf(info->error_message, sizeof(info->error_message),
            "Failed to read GGUF header");
        fclose(f);
        return true;
    }

    uint64_t tensor_count = read_u64_le(header + 4);
    uint64_t metadata_kv_count = read_u64_le(header + 4 + 8);

    (void)tensor_count; /* Not needed for metadata extraction */

    /* Params we'll extract from metadata */
    char architecture[64] = {0};
    char model_type[64] = {0};
    int file_type = -1;
    bool is_multilingual = false;
    bool found_architecture = false;
    bool found_model_type = false;

    /* Read each metadata key-value pair */
    for (uint64_t i = 0; i < metadata_kv_count; i++) {
        /* Read key length (uint64) + key data */
        uint64_t key_len = 0;
        unsigned char len_buf[8];
        if (fread(len_buf, 1, 8, f) != 8) {
            break; /* Truncated file */
        }
        key_len = read_u64_le(len_buf);

        /* Sanity check: key shouldn't be absurdly large */
        if (key_len > 1024) {
            /* Corrupted or unsupported format */
            break;
        }

        char key[1024];
        if (key_len > 0) {
            if (fread(key, 1, key_len, f) != key_len) {
                break;
            }
        }
        key[key_len] = '\0';

        /* Read value type (uint32) */
        unsigned char type_buf[4];
        if (fread(type_buf, 1, 4, f) != 4) {
            break;
        }
        uint32_t val_type = read_u32_le(type_buf);

        /* Read value based on type */
        if (strcmp(key, "general.architecture") == 0 && val_type == GGUF_TYPE_STRING) {
            uint64_t str_len = 0;
            if (fread(len_buf, 1, 8, f) != 8) break;
            str_len = read_u64_le(len_buf);
            if (str_len < sizeof(architecture)) {
                if (fread(architecture, 1, str_len, f) != str_len) break;
                architecture[str_len] = '\0';
                found_architecture = true;
            } else {
                fseek(f, (long)str_len, SEEK_CUR);
            }
        } else if (strcmp(key, "general.type") == 0 && val_type == GGUF_TYPE_STRING) {
            uint64_t str_len = 0;
            if (fread(len_buf, 1, 8, f) != 8) break;
            str_len = read_u64_le(len_buf);
            if (str_len < sizeof(model_type)) {
                if (fread(model_type, 1, str_len, f) != str_len) break;
                model_type[str_len] = '\0';
                found_model_type = true;
            } else {
                fseek(f, (long)str_len, SEEK_CUR);
            }
        } else if (strcmp(key, "general.file_type") == 0 &&
                   (val_type == GGUF_TYPE_INT32 || val_type == GGUF_TYPE_UINT32)) {
            unsigned char val_buf[4];
            if (fread(val_buf, 1, 4, f) != 4) break;
            file_type = (int)read_u32_le(val_buf);
        } else if (strcmp(key, "whisper.is_multilingual") == 0 && val_type == GGUF_TYPE_BOOL) {
            unsigned char val_buf[1];
            if (fread(val_buf, 1, 1, f) != 1) break;
            is_multilingual = (val_buf[0] != 0);
        } else {
            /* Skip unknown value */
            switch (val_type) {
                case GGUF_TYPE_UINT8:
                case GGUF_TYPE_INT8:
                    fseek(f, 1, SEEK_CUR);
                    break;
                case GGUF_TYPE_UINT16:
                case GGUF_TYPE_INT16:
                    fseek(f, 2, SEEK_CUR);
                    break;
                case GGUF_TYPE_UINT32:
                case GGUF_TYPE_INT32:
                case GGUF_TYPE_FLOAT32:
                    fseek(f, 4, SEEK_CUR);
                    break;
                case GGUF_TYPE_UINT64:
                case GGUF_TYPE_INT64:
                case GGUF_TYPE_FLOAT64:
                    fseek(f, 8, SEEK_CUR);
                    break;
                case GGUF_TYPE_BOOL:
                    fseek(f, 1, SEEK_CUR);
                    break;
                case GGUF_TYPE_STRING: {
                    uint64_t str_len = 0;
                    if (fread(len_buf, 1, 8, f) != 8) goto done_reading;
                    str_len = read_u64_le(len_buf);
                    fseek(f, (long)str_len, SEEK_CUR);
                    break;
                }
                case GGUF_TYPE_ARRAY: {
                    /* Skip array type (4 bytes) + count (8 bytes) */
                    fseek(f, 4 + 8, SEEK_CUR);
                    /* Can't easily skip array content without knowing element type,
                     * so we just break out of the loop for now */
                    goto done_reading;
                }
                default:
                    /* Unknown type - can't reliably skip, stop parsing */
                    goto done_reading;
            }
        }
    }

done_reading:
    fclose(f);

    /* Validate that this is a whisper model */
    if (!found_architecture || strcmp(architecture, "whisper") != 0) {
        if (!found_architecture) {
            snprintf(info->error_message, sizeof(info->error_message),
                "Missing 'general.architecture' metadata");
        } else {
            snprintf(info->error_message, sizeof(info->error_message),
                "Not a Whisper model (architecture: %s)", architecture);
        }
        return true;
    }

    /* Populate model name */
    if (found_model_type && model_type[0]) {
        snprintf(info->model_name, sizeof(info->model_name), "%s", model_type);
    } else {
        snprintf(info->model_name, sizeof(info->model_name), "whisper");
    }

    /* Populate quantization string */
    if (file_type >= 0) {
        snprintf(info->quantization, sizeof(info->quantization), "%s",
                 ftype_to_quant_string(file_type));
    } else {
        snprintf(info->quantization, sizeof(info->quantization), "unknown");
    }

    info->multilingual = is_multilingual;
    info->valid = true;
    info->error_message[0] = '\0';

    return true;
}
