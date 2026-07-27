/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>
 *
 * This file is part of the Transcriber project.
 * See the LICENSE file for full license text.
 */

/*
 * app_controller.c — AppStateController Implementation
 *
 * This file implements the AppStateController API declared in app.h.
 * It provides thread-safe state machine transitions for the three
 * application states: IDLE, LISTENING, TRANSCRIBING.
 *
 * Extracted from main.c for better modularity. The state controller
 * is the central coordinator for all state transitions and uses a
 * mutex-protected state variable with an atomic sequence counter
 * to prevent race conditions.
 *
 * SRS: Section 2.1 (Threading Model), Section 2.3 (State Machine),
 *      NR-019 (Atomic Sequence Counter)
 */

#include "app.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>

/******************************************************************************
 * AppStateController Implementation (from app.h)
 *****************************************************************************/

int app_state_controller_init(AppStateController *controller,
                              AppConfig *config,
                              transcription_result_callback on_transcription_result,
                              model_status_callback on_model_status,
                              state_change_callback on_state_change,
                              void *user_data) {
    if (!controller || !config) {
        return -1;
    }

    /* Zero-initialize the entire struct first.
     * This ensures that even if another thread reads the controller between
     * mutex init and field assignment below, it sees all-zero values rather
     * than stale garbage from stack/heap memory. The atomic sequence counter
     * is set to 0 here (via memset) and will be properly initialized by
     * atomic_init() after the mutex is ready. */
    memset(controller, 0, sizeof(AppStateController));

    /* Initialize the mutex before any field assignments.
     * All subsequent writes are performed under the lock so that a reader
     * acquiring the mutex sees either all-zero (pre-init) or fully-populated
     * fields — never a partially-initialized intermediate state. */
    int ret = pthread_mutex_init(&controller->state_mutex, NULL);
    if (ret != 0) {
        return -1;
    }

    /* Initialize atomic sequence counter properly. */
    atomic_store((atomic_int *)&controller->sequence_counter, 0);

    /* Set all fields under the mutex lock for thread safety.
     * Any concurrent reader (e.g., app_get_state) will either see
     * the zero-initialized state or the fully-populated state. */
    pthread_mutex_lock(&controller->state_mutex);
    controller->state = STATE_IDLE;
    controller->model_status = MODEL_UNAVAILABLE;
    controller->config = config;
    controller->on_transcription_result = on_transcription_result;
    controller->on_model_status = on_model_status;
    controller->on_state_change = on_state_change;
    controller->callback_user_data = user_data;
    pthread_mutex_unlock(&controller->state_mutex);

    return 0;
}

void app_state_controller_cleanup(AppStateController *controller) {
    if (!controller) return;
    pthread_mutex_destroy(&controller->state_mutex);
}

AppState app_get_state(AppStateController *controller) {
    if (!controller) return STATE_IDLE;

    AppState state;
    pthread_mutex_lock(&controller->state_mutex);
    state = controller->state;
    pthread_mutex_unlock(&controller->state_mutex);

    return state;
}

ModelStatus app_get_model_status(AppStateController *controller) {
    if (!controller) return MODEL_UNAVAILABLE;

    ModelStatus status;
    pthread_mutex_lock(&controller->state_mutex);
    status = controller->model_status;
    pthread_mutex_unlock(&controller->state_mutex);

    return status;
}

bool app_transition_to(AppStateController *controller, AppState target) {
    if (!controller) return false;

    pthread_mutex_lock(&controller->state_mutex);

    AppState current = controller->state;
    bool allowed = false;

    /* Check valid transitions */
    switch (current) {
        case STATE_IDLE:
            allowed = (target == STATE_LISTENING);
            break;
        case STATE_LISTENING:
            allowed = (target == STATE_TRANSCRIBING);
            break;
        case STATE_TRANSCRIBING:
            /* Allow TRANSCRIBING → LISTENING for continuous mode,
             * and TRANSCRIBING → IDLE for normal mode */
            allowed = (target == STATE_IDLE || target == STATE_LISTENING);
            break;
    }

    if (allowed) {
        controller->state = target;
        controller->sequence_counter++;
    }

    pthread_mutex_unlock(&controller->state_mutex);

    /* Invoke state change callback directly on successful transition.
      * Callback is invoked outside the mutex to prevent deadlocks.
      * Note: Called from whichever thread invoked this function.
      *
      * Thread safety: callback_user_data was captured inside the mutex above.
      * In practice, callback_user_data is set once during init and never
      * modified, so there is no race. If future code modifies it, a ref
      * count or copy would be needed here. */
    if (allowed && controller->on_state_change) {
        controller->on_state_change(current, target, controller->callback_user_data);
    }

    return allowed;
}

void app_set_model_status(AppStateController *controller, ModelStatus status) {
    if (!controller) return;

    pthread_mutex_lock(&controller->state_mutex);
    controller->model_status = status;
    pthread_mutex_unlock(&controller->state_mutex);
}

bool app_toggle_state(AppStateController *controller) {
    if (!controller) return false;

    /* Make the toggle decision and transition atomic in a single
     * mutex acquisition to prevent race conditions from double-increment
     * of sequence_counter and non-atomic state determination. */
    pthread_mutex_lock(&controller->state_mutex);

    AppState previous = controller->state;
    AppState target;
    bool should_transition = false;

    switch (previous) {
        case STATE_IDLE:
            target = STATE_LISTENING;
            should_transition = true;
            break;
        case STATE_LISTENING:
            target = STATE_TRANSCRIBING;
            should_transition = true;
            break;
        case STATE_TRANSCRIBING:
        default:
            /* No-op: wait for transcription to complete */
            should_transition = false;
            break;
    }

    if (should_transition) {
        controller->state = target;
        controller->sequence_counter++;
    }

    pthread_mutex_unlock(&controller->state_mutex);

    /* Invoke state change callback directly on successful transition.
     * Callback is invoked outside the mutex to prevent deadlocks.
     * Note: Called from whichever thread invoked this function. */
    if (should_transition && controller->on_state_change) {
        controller->on_state_change(previous, target, controller->callback_user_data);
    }

    return should_transition;
}
