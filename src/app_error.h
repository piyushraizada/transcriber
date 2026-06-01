/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

#ifndef APP_ERROR_H
#define APP_ERROR_H

/**
 * @file app_error.h
 * @brief Shared thread-safe error buffer utility
 *
 * Provides a reusable pattern for thread-safe error message storage
 * across modules. Replaces the pattern of static global error buffers
 * with a shared implementation using thread-local return buffers.
 *
 * Usage:
 *   // In a .c file:
 *   #include "app_error.h"
 *   APP_ERROR_BUFFER_DEFINE(my_module);
 *
 *   void my_function(void) {
 *       my_module_set_error("something failed");
 *       ...
 *   }
 *
 *   const char *my_function_get_error(void) {
 *       return my_module_get_error();
 *   }
 */

#include <pthread.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Macro: Define a module's error buffer and accessor functions
 *---------------------------------------------------------------------------
 * Expands to:
 *   - A static char buffer protected by a pthread_mutex_t
 *   - A set_error() function for the module
 *   - A get_error() function returning a thread-local copy
 *   - A reset_error() function
 *
 * @param name Base name for generated symbols (e.g., "audio", "config")
 */
#define APP_ERROR_BUFFER_DEFINE(name) \
    static char g_##name##_error[256] = {0}; \
    static pthread_mutex_t g_##name##_error_mutex = PTHREAD_MUTEX_INITIALIZER; \
    \
    static void name##_set_error(const char *msg) \
    { \
        pthread_mutex_lock(&g_##name##_error_mutex); \
        if (msg) { \
            snprintf(g_##name##_error, sizeof(g_##name##_error), "%s", msg); \
        } else { \
            g_##name##_error[0] = '\0'; \
        } \
        pthread_mutex_unlock(&g_##name##_error_mutex); \
    } \
    \
    static const char *name##_get_error(void) \
    { \
        static __thread char local_buffer[256] = {0}; \
        pthread_mutex_lock(&g_##name##_error_mutex); \
        snprintf(local_buffer, sizeof(local_buffer), "%s", g_##name##_error); \
        pthread_mutex_unlock(&g_##name##_error_mutex); \
        return local_buffer; \
    } \
    \
    static void name##_reset_error(void) \
    { \
        pthread_mutex_lock(&g_##name##_error_mutex); \
        g_##name##_error[0] = '\0'; \
        pthread_mutex_unlock(&g_##name##_error_mutex); \
    }

#ifdef __cplusplus
}
#endif

#endif /* APP_ERROR_H */
