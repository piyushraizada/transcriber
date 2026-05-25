#ifndef APP_GPU_H
#define APP_GPU_H

/**
 * @file app_gpu.h
 * @brief GPU discovery, memory querying, and selection utilities
 *
 * This module provides functions to discover NVIDIA GPU devices, query their
 * free/total memory, and select the best GPU for model loading based on
 * available VRAM. It is built on top of the CUDA runtime API when available
 * (HAVE_CUDA defined at compile time), and gracefully degrades to CPU-only
 * reporting when CUDA is not compiled in.
 *
 * Key features:
 *   - Discover available NVIDIA GPU devices at runtime
 *   - Query free and total memory per device
 *   - Select GPU with most free memory (for "Auto" mode)
 *   - Parse user-facing GPU mode strings into internal GPU indices
 *   - Get human-readable GPU device names
 *
 * GPU Mode Strings (used in config.json and UI):
 *   - "auto"     → Select GPU with most free memory, fallback to CPU
 *   - "cpu"      → Force CPU-only processing
 *   - "gpu:0"    → Use specific GPU device 0
 *   - "gpu:1"    → Use specific GPU device 1
 *   - etc.
 *
 * Internal GPU Index Constants (used in app_whisper.c):
 *   - GPU_INDEX_AUTO_MEMORY  (-3) → Auto-select by free memory
 *   - GPU_INDEX_CPU_ONLY     (-2) → Force CPU
 *   - GPU_INDEX_AUTO         (-1) → Legacy auto (first GPU)
 *   - >= 0                   → Specific GPU device index
 *
 * Thread Safety:
 *   - All functions are thread-safe (no internal mutable state)
 *   - CUDA runtime calls are thread-safe per NVIDIA documentation
 *
 * @see docs/gpu-selection-analysis.md
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * GPU Index Constants
 *---------------------------------------------------------------------------
 * These match the constants in app_whisper.c and are used to communicate
 * GPU selection preferences between the config layer and the whisper client.
 */

#define GPU_INDEX_AUTO_MEMORY  -3  ///< Auto-select GPU with most free memory
#define GPU_INDEX_CPU_ONLY     -2  ///< Force CPU-only processing
#define GPU_INDEX_AUTO         -1  ///< Legacy auto (use first GPU)

/*---------------------------------------------------------------------------
 * Section 1: GPU Availability and Discovery
 *---------------------------------------------------------------------------
 * Functions to check if CUDA support is compiled in and discover available
 * GPU devices at runtime.
 */

/**
 * Check if the application was compiled with CUDA support.
 *
 * This is a compile-time check. Returns true if HAVE_CUDA is defined,
 * false otherwise. Does NOT check if a GPU is actually present at runtime.
 *
 * @return true if CUDA support is compiled in, false otherwise
 */
bool gpu_cuda_compiled_in(void);

/**
 * Discover the number of available NVIDIA GPU devices at runtime.
 *
 * When CUDA is compiled in, this calls cudaDeviceGetCount() to get the
 * actual number of CUDA-capable devices. When CUDA is not compiled in,
 * returns 0.
 *
 * @param count_out Output parameter set to the number of available GPUs
 * @return true if the query succeeded, false on error or CUDA not available
 */
bool gpu_get_device_count(int *count_out);

/**
 * Get the human-readable name of a GPU device.
 *
 * Queries the CUDA device properties for the given index and copies the
 * device name into the output buffer. When CUDA is not compiled in or
 * the device index is invalid, returns false.
 *
 * @param device_idx GPU device index (0-based)
 * @param name_out   Output buffer for device name
 * @param name_size  Size of name_out buffer
 * @return true if the name was retrieved successfully, false otherwise
 */
bool gpu_get_device_name(int device_idx, char *name_out, size_t name_size);

/*---------------------------------------------------------------------------
 * Section 2: GPU Memory Querying
 *---------------------------------------------------------------------------
 * Functions to query free and total memory on a specific GPU device.
 * Uses cudaMemGetInfo() after setting the active device.
 */

/**
 * Query free and total memory for a specific GPU device.
 *
 * Sets the active CUDA device to device_idx, then calls cudaMemGetInfo()
 * to get the free and total memory. The active device is restored after
 * the query.
 *
 * When CUDA is not compiled in, returns false.
 *
 * @param device_idx    GPU device index (0-based)
 * @param free_bytes    Output parameter for free memory in bytes (can be NULL)
 * @param total_bytes   Output parameter for total memory in bytes (can be NULL)
 * @return true if the query succeeded, false otherwise
 */
bool gpu_get_memory_info(int device_idx, size_t *free_bytes, size_t *total_bytes);

/*---------------------------------------------------------------------------
 * Section 3: GPU Selection
 *---------------------------------------------------------------------------
 * Functions to select the best GPU based on various criteria.
 */

/**
 * Select the GPU device with the most free memory.
 *
 * Iterates over all available GPU devices, queries free memory for each,
 * and selects the one with the highest free memory value. If no GPUs are
 * available or all queries fail, returns false.
 *
 * The selected device index is written to best_device_idx. The free memory
 * of the selected device is written to free_bytes (if not NULL).
 *
 * @param best_device_idx Output parameter for the selected device index
 * @param free_bytes      Output parameter for free memory on selected device (can be NULL)
 * @return true if a GPU was successfully selected, false if no GPUs available
 */
bool gpu_select_best_by_free_memory(int *best_device_idx, size_t *free_bytes);

/**
 * Select a GPU device with at least the specified amount of free memory.
 *
 * Similar to gpu_select_best_by_free_memory(), but only returns a device
 * that has at least min_free_bytes available. If no device meets the
 * threshold, returns false.
 *
 * @param best_device_idx Output parameter for the selected device index
 * @param min_free_bytes  Minimum free memory required in bytes
 * @return true if a suitable GPU was found, false otherwise
 */
bool gpu_select_with_min_free_memory(int *best_device_idx, size_t min_free_bytes);

/*---------------------------------------------------------------------------
 * Section 4: GPU Mode String Parsing
 *---------------------------------------------------------------------------
 * Functions to parse the user-facing GPU mode strings from config into
 * internal GPU index values.
 */

/**
 * Parse a GPU mode string into an internal GPU index.
 *
 * Accepts the following formats:
 *   - "auto"     → Returns GPU_INDEX_AUTO_MEMORY (-3)
 *   - "cpu"      → Returns GPU_INDEX_CPU_ONLY (-2)
 *   - "gpu:N"    → Returns N (where N >= 0)
 *   - ""         → Returns GPU_INDEX_AUTO_MEMORY (-3) (default to auto)
 *
 * For "gpu:N" mode, the function validates that N is within the range
 * of available devices. If N is out of range, returns GPU_INDEX_CPU_ONLY
 * with a warning logged to stderr.
 *
 * @param mode_str        Input string from config (e.g., "auto", "cpu", "gpu:0")
 * @param gpu_index_out   Output parameter for the parsed GPU index
 * @return true if parsing succeeded, false if the string is invalid
 */
bool gpu_mode_parse(const char *mode_str, int *gpu_index_out);

/**
 * Format a GPU index back into a human-readable mode string.
 *
 * Reverse of gpu_mode_parse(). Useful for displaying the current setting
 * in the UI or logging.
 *
 * @param gpu_index   Internal GPU index value
 * @param mode_out    Output buffer for the mode string
 * @param mode_size   Size of mode_out buffer
 */
void gpu_mode_format(int gpu_index, char *mode_out, size_t mode_size);

/*---------------------------------------------------------------------------
 * Section 5: Default and Validation Helpers
 *---------------------------------------------------------------------------
 * Helper functions for configuration defaults and validation.
 */

/**
 * Get the default GPU mode string.
 *
 * @return Static string "auto" (do NOT free)
 */
const char *gpu_mode_get_default(void);

/**
 * Validate a GPU mode string.
 *
 * Checks if the mode string is one of the accepted formats:
 * "auto", "cpu", or "gpu:N" where N is a non-negative integer.
 *
 * @param mode_str Input string to validate
 * @return true if valid, false otherwise
 */
bool gpu_mode_validate(const char *mode_str);

/**
 * Get a description of the GPU mode for display/logging.
 *
 * @param mode_str GPU mode string
 * @return Static description string (do NOT free)
 */
const char *gpu_mode_description(const char *mode_str);

#ifdef __cplusplus
}
#endif

#endif /* APP_GPU_H */
