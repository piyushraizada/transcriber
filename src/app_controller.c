/*
 * SPDX-License-Identifier: MIT
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

/******************************************************************************
 * AppStateController Implementation (from app.h)
 *****************************************************************************/

int app_state_controller_init(AppStateController *controller,
                              AppConfig *config,
                              transcription_result_callback on_transcription_result,
                              connection_status_callback on_connection_status,
                              recording_stop_callback on_recording_stop,
                              state_change_callback on_state_change,
                              void *user_data) {
    if (!controller || !config) {
        return -1;
    }

    /* Initialize the mutex */
    int ret = pthread_mutex_init(&controller->state_mutex, NULL);
    if (ret != 0) {
        return -1;
    }

    /* Set initial state */
    controller->state = STATE_IDLE;
    controller->connection_status = CONNECTION_DISCONNECTED;
    controller->sequence_counter = 0;
    controller->config = config;
    controller->on_transcription_result = on_transcription_result;
    controller->on_connection_status = on_connection_status;
    controller->on_recording_stop = on_recording_stop;
    controller->on_state_change = on_state_change;
    controller->callback_user_data = user_data;

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

ConnectionStatus app_get_connection_status(AppStateController *controller) {
    if (!controller) return CONNECTION_DISCONNECTED;

    ConnectionStatus status;
    pthread_mutex_lock(&controller->state_mutex);
    status = controller->connection_status;
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
            allowed = (target == STATE_IDLE);
            break;
    }

    if (allowed) {
        controller->state = target;
        controller->sequence_counter++;
    }

    pthread_mutex_unlock(&controller->state_mutex);

    /* Invoke state change callback directly on successful transition.
     * Callback is invoked outside the mutex to prevent deadlocks.
     * Note: Called from whichever thread invoked this function. */
    if (allowed && controller->on_state_change) {
        controller->on_state_change(target, controller->callback_user_data);
    }

    return allowed;
}

void app_set_connection_status(AppStateController *controller, ConnectionStatus status) {
    if (!controller) return;

    pthread_mutex_lock(&controller->state_mutex);
    controller->connection_status = status;
    pthread_mutex_unlock(&controller->state_mutex);
}

bool app_toggle_state(AppStateController *controller) {
    if (!controller) return false;

    /* CR-03 fix: Make the toggle decision and transition atomic in a single
     * mutex acquisition to prevent race conditions from double-increment
     * of sequence_counter and non-atomic state determination. */
    pthread_mutex_lock(&controller->state_mutex);

    AppState target;
    bool should_transition = false;

    switch (controller->state) {
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
        controller->on_state_change(target, controller->callback_user_data);
    }

    return should_transition;
}
