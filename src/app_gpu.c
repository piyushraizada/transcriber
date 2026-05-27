/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/**
 * @file app_gpu.c
 * @brief GPU discovery, memory querying, and selection implementation
 *
 * Implements runtime detection of NVIDIA GPU devices, free memory queries,
 * and intelligent GPU selection for whisper.cpp model loading.
 *
 * When compiled with CUDA support (HAVE_CUDA), uses the CUDA runtime API
 * (cuda_runtime_api.h) for device enumeration and memory queries.
 * When compiled without CUDA, all functions gracefully return CPU-only results.
 *
 * @see app_gpu.h
 * @see docs/gpu-selection-analysis.md
 */

#include "app_gpu.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_CUDA
#include <cuda_runtime_api.h>
#endif

/*---------------------------------------------------------------------------
 * Constants
 *---------------------------------------------------------------------------*/

/** Minimum free VRAM (in bytes) to consider a GPU suitable for model loading.
 *  Set to 2 GB as a conservative minimum — smaller models (tiny, base) need
 *  about 1-1.5 GB, but we leave headroom for CUDA context overhead. */
#define MIN_FREE_VRAM_BYTES ((size_t)(2UL * 1024 * 1024 * 1024))

/*---------------------------------------------------------------------------
 * Section 1: GPU Availability and Discovery
 *---------------------------------------------------------------------------*/

bool gpu_get_device_count(int *count_out)
{
    if (!count_out) return false;
    *count_out = 0;

#ifndef HAVE_CUDA
    return false;
#endif

    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess) {
        g_log("app-gpu", G_LOG_LEVEL_MESSAGE, "[gpu] cudaGetDeviceCount failed: %s\n",
                cudaGetErrorString(err));
        return false;
    }
    *count_out = count;
    return true;
}

bool gpu_get_device_name(int device_idx, char *name_out, size_t name_size)
{
    if (device_idx < 0 || !name_out || name_size == 0) return false;

#ifndef HAVE_CUDA
    return false;
#endif

    struct cudaDeviceProp prop;
    cudaError_t err = cudaGetDeviceProperties(&prop, device_idx);
    if (err != cudaSuccess) {
        g_log("app-gpu", G_LOG_LEVEL_MESSAGE, "[gpu] cudaGetDeviceProperties(%d) failed: %s\n",
                device_idx, cudaGetErrorString(err));
        return false;
    }
    snprintf(name_out, name_size, "%.*s", (int)(name_size - 1), prop.name);
    return true;
}

/*---------------------------------------------------------------------------
 * Section 2: GPU Memory Querying
 *---------------------------------------------------------------------------*/

bool gpu_get_memory_info(int device_idx, size_t *free_bytes, size_t *total_bytes)
{
    if (device_idx < 0) return false;

#ifndef HAVE_CUDA
    return false;
#endif

    /* Save current device so we can restore it */
    int current_device = 0;
    cudaGetDevice(&current_device);

    /* Set target device */
    cudaError_t err = cudaSetDevice(device_idx);
    if (err != cudaSuccess) {
        g_log("app-gpu", G_LOG_LEVEL_MESSAGE, "[gpu] cudaSetDevice(%d) failed: %s\n",
                device_idx, cudaGetErrorString(err));
        /* Restore original device */
        cudaSetDevice(current_device);
        return false;
    }

    /* Query memory */
    size_t free_mem = 0, total_mem = 0;
    err = cudaMemGetInfo(&free_mem, &total_mem);
    if (err != cudaSuccess) {
        g_log("app-gpu", G_LOG_LEVEL_MESSAGE, "[gpu] cudaMemGetInfo on device %d failed: %s\n",
                device_idx, cudaGetErrorString(err));
        cudaSetDevice(current_device);
        return false;
    }

    if (free_bytes) *free_bytes = free_mem;
    if (total_bytes) *total_bytes = total_mem;

    /* Restore original device */
    cudaSetDevice(current_device);
    return true;
}

/*---------------------------------------------------------------------------
 * Section 3: GPU Selection
 *---------------------------------------------------------------------------*/

bool gpu_release_unused_devices(int used_device_idx)
{
    if (used_device_idx < 0) return false;

#ifndef HAVE_CUDA
    return false;
#endif

    int device_count = 0;
    if (!gpu_get_device_count(&device_count)) {
        return false;
    }

    for (int i = 0; i < device_count; i++) {
        if (i != used_device_idx) {
            cudaSetDevice(i);
            cudaError_t err = cudaDeviceReset();
            if (err != cudaSuccess) {
                g_log("app-gpu", G_LOG_LEVEL_MESSAGE,
                        "[gpu] cudaDeviceReset(%d) failed: %s\n",
                        i, cudaGetErrorString(err));
            } else {
                g_log("app-gpu", G_LOG_LEVEL_DEBUG,
                        "[gpu] Released CUDA context on device %d\n", i);
            }
        }
    }

    /* Ensure the used device is the current one */
    cudaSetDevice(used_device_idx);
    return true;
}

bool gpu_select_best_by_free_memory(int *best_device_idx, size_t *free_bytes)
{
    if (!best_device_idx) return false;

    int device_count = 0;
    if (!gpu_get_device_count(&device_count)) {
        g_log("app-gpu", G_LOG_LEVEL_MESSAGE, "[gpu] No CUDA devices available\n");
        return false;
    }

    if (device_count == 0) {
        g_log("app-gpu", G_LOG_LEVEL_MESSAGE, "[gpu] No CUDA devices found\n");
        return false;
    }

    int best_idx = -1;
    size_t best_free = 0;

    for (int i = 0; i < device_count; i++) {
        /* MED-8 fix: Removed unused total_mem variable */
        size_t free_mem = 0;
        if (gpu_get_memory_info(i, &free_mem, NULL)) {
            char name[128] = {"Unknown"};
            gpu_get_device_name(i, name, sizeof(name));

            if (best_idx < 0 || free_mem > best_free) {
                best_free = free_mem;
                best_idx = i;
            }
        } else {
            g_log("app-gpu", G_LOG_LEVEL_MESSAGE, "[gpu] Failed to query memory for device %d, skipping\n", i);
        }
    }

    if (best_idx < 0) {
        g_log("app-gpu", G_LOG_LEVEL_MESSAGE, "[gpu] Failed to query memory for all devices\n");
        return false;
    }

    *best_device_idx = best_idx;
    if (free_bytes) *free_bytes = best_free;

    /* Release CUDA contexts on all non-selected devices.
     * Querying memory via cudaSetDevice + cudaMemGetInfo creates a CUDA
     * context (~256 MiB) on each device. Reset the contexts on devices
     * we won't use so they don't hold VRAM unnecessarily. */
    gpu_release_unused_devices(best_idx);

    return true;
}

bool gpu_select_with_min_free_memory(int *best_device_idx, size_t min_free_bytes)
{
    if (!best_device_idx) return false;

    int device_count = 0;
    if (!gpu_get_device_count(&device_count)) {
        return false;
    }

    if (device_count == 0) {
        return false;
    }

    int best_idx = -1;
    size_t best_free = 0;

    for (int i = 0; i < device_count; i++) {
        size_t free_mem = 0;
        if (gpu_get_memory_info(i, &free_mem, NULL)) {
            if (free_mem >= min_free_bytes && (best_idx < 0 || free_mem > best_free)) {
                best_free = free_mem;
                best_idx = i;
            }
        }
    }

    if (best_idx < 0) {
        g_log("app-gpu", G_LOG_LEVEL_MESSAGE,
                "[gpu] No GPU with >= %.1f GB free memory found",
                (double)min_free_bytes / (1024.0 * 1024.0 * 1024.0));
        return false;
    }

    *best_device_idx = best_idx;

    /* Release CUDA contexts on non-selected devices (was missing here) */
    gpu_release_unused_devices(best_idx);

    return true;
}

/*---------------------------------------------------------------------------
 * Section 4: GPU Mode String Parsing
 *---------------------------------------------------------------------------*/

bool gpu_mode_parse(const char *mode_str, int *gpu_index_out)
{
    if (!gpu_index_out) return false;

    /* Empty string defaults to auto */
    if (!mode_str || mode_str[0] == '\0') {
        *gpu_index_out = GPU_INDEX_AUTO_MEMORY;
        return true;
    }

    if (strcmp(mode_str, "auto") == 0) {
        *gpu_index_out = GPU_INDEX_AUTO_MEMORY;
        return true;
    }

    if (strcmp(mode_str, "cpu") == 0) {
        *gpu_index_out = GPU_INDEX_CPU_ONLY;
        return true;
    }

    /* Check for "gpu:N" format */
    if (strncmp(mode_str, "gpu:", 4) == 0) {
        const char *num_str = mode_str + 4;
        char *endptr = NULL;
        long val = strtol(num_str, &endptr, 10);

        /* Validate: must be a non-negative integer with no trailing chars */
        if (endptr == num_str || *endptr != '\0' || val < 0) {
            g_log("app-gpu", G_LOG_LEVEL_MESSAGE, "[gpu] Invalid GPU mode string: \"%s\"\n", mode_str);
            return false;
        }

        int device_idx = (int)val;

        /* Validate against available device count */
        int device_count = 0;
        if (gpu_get_device_count(&device_count)) {
            if (device_idx >= device_count) {
                g_log("app-gpu", G_LOG_LEVEL_MESSAGE,
                        "[gpu] GPU %d requested but only %d device(s) available, falling back to CPU",
                        device_idx, device_count);
                *gpu_index_out = GPU_INDEX_CPU_ONLY;
                return true;
            }
        }
        /* If we can't query device count (no CUDA compiled), still accept
         * the index — the whisper.cpp load will fail and fallback to CPU */

        *gpu_index_out = device_idx;
        return true;
    }

    g_log("app-gpu", G_LOG_LEVEL_MESSAGE, "[gpu] Unknown GPU mode string: \"%s\"\n", mode_str);
    return false;
}

/*---------------------------------------------------------------------------
 * Section 5: Default and Validation Helpers
 *---------------------------------------------------------------------------*/

const char *gpu_mode_get_default(void)
{
    return "auto";
}

bool gpu_mode_validate(const char *mode_str)
{
    if (!mode_str) return false;

    if (strcmp(mode_str, "auto") == 0) return true;
    if (strcmp(mode_str, "cpu") == 0) return true;

    if (strncmp(mode_str, "gpu:", 4) == 0) {
        const char *num_str = mode_str + 4;
        if (num_str[0] == '\0') return false;
        char *endptr = NULL;
        strtol(num_str, &endptr, 10);
        return (endptr != num_str && *endptr == '\0');
    }

    return false;
}
