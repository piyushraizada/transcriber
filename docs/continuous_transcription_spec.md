<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2026 Piyush Raizada <piyush.raizada@gmail.com>

  This file is part of the Transcriber project.
  See the LICENSE file for full license text.
-->

# Continuous Transcription — Reverse-Engineered Specification

## Purpose

This document reverse-engineers the continuous transcription logic from the Linux (ALSA/GTK3) implementation of Transcriber into a platform-independent specification that another AI agent can use to implement an exact copy for Windows porting.

Every algorithm, data structure, threading pattern, timing constant, and state transition described here was extracted directly from the source code in `src/`.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Audio Format Constants](#2-audio-format-constants)
3. [Configuration Parameters](#3-configuration-parameters)
4. [State Machine](#4-state-machine)
5. [Core Data Structures](#5-core-data-structures)
6. [Subsystems](#6-subsystems)
   - [6.1 Ring Buffer](#61-ring-buffer)
   - [6.2 VAD (Voice Activity Detection)](#62-vad-voice-activity-detection)
   - [6.3 Audio Recorder](#63-audio-recorder)
   - [6.4 Silence Scanner](#64-silence-scanner)
   - [6.5 Whisper Transcription Client](#65-whisper-transcription-client)
   - [6.6 State Controller](#66-state-controller)
7. [Continuous Mode Flow — Complete Sequence](#7-continuous-mode-flow--complete-sequence)
8. [Threading Model](#9-threading-model)
9. [Cross-Thread Communication Patterns](#10-cross-thread-communication-patterns)
10. [User Stop in Continuous Mode](#11-user-stop-in-continuous-mode)
11. [Final Transcription (Stop Sequence)](#12-final-transcription-stop-sequence)
12. [Watchdog Timer](#13-watchdog-timer)
13. [Error Handling and Edge Cases](#14-error-handling-and-edge-cases)
14. [Platform-Independent Abstractions for Windows Porting](#15-platform-independent-abstractions-for-windows-porting)

---

## 1. Overview

Transcriber operates in Continuous Dictation Mode using silence-based segmentation:

1. User clicks microphone → starts recording + creates a **Silence Scanner** thread
2. VAD is **disabled** in the capture thread (`vad_active = false`) — ALL frames go to ring buffer unconditionally
3. Silence Scanner runs as a dedicated background thread, scanning the ring buffer in 500ms chunks
4. When scanner detects silence threshold exceeded after minimum segment duration → extracts that segment from ring buffer → calls transcription callback
5. Transcription callback transcribes the segment via whisper.cpp → result appended to text window
6. Recording **never stops** — scanner continues scanning from where it left off
7. Loop repeats until user explicitly clicks microphone again (sets `user_requested_stop` atomic flag)

The architecture relies on a dedicated Silence Scanner thread for segmentation, while the capture thread simply streams all audio frames into a ring buffer without any voice activity filtering. This enables multiple transcriptions to occur while recording continues uninterrupted.

---

## 2. Audio Format Constants

These are FIXED and must be identical across all subsystems:

| Constant | Value | Description |
|----------|-------|-------------|
| Sample Rate | 16000 Hz | Mono audio at 16kHz |
| Sample Format | int16_t (signed 16-bit PCM) | Little-endian, native to whisper.cpp |
| Channels | 1 (Mono) | Single channel |
| Frame Size for VAD | 320 samples = 20ms | Matches WebRTC VAD expected frame sizes |
| ALSA Buffer Size | 320 samples | Each `snd_pcm_readi()` call reads 320 samples |

The 20ms frame size (320 samples at 16kHz) is critical because WebRTC VAD only accepts frames of exactly 10ms, 20ms, or 30ms. The code uses 20ms consistently.

---

## 3. Configuration Parameters

The `AppConfig` struct (`src/app_config.h`) contains these fields relevant to continuous transcription:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `continuous_dictation` | bool | false | Enables continuous mode (silence scanner segmentation) |
| `vad_mode` | int (0-3) | 2 | WebRTC VAD aggressiveness: 0=least, 3=most restrictive. Used by Silence Scanner's internal VAD instance and final transcription validation. |
| `scanner_silence_ms` | int | 1500 | Silence duration (ms) before silence scanner segments audio in continuous mode |
| `scanner_silence_ms` | int | 1500 | Silence duration (ms) before silence scanner segments audio in continuous mode |
| `scanner_min_segment_ms` | int | 2000 | Minimum segment duration (ms) before scanner will trigger transcription |
| `max_duration` | int | 30 | Maximum recording duration in seconds (watchdog timer) |
| `append_transcription_text` | bool | true | If false, clears text window at start of each session |
| `model_path` | char[] | "" | Path to Whisper GGUF model file |
| `audio_device` | char[] | "default" | Audio capture device name |
| `language` | char[] | "auto" | Language code for whisper.cpp (or "auto") |

---

## 4. State Machine

### States

```c
typedef enum {
    STATE_IDLE,          // Not recording, not transcribing
    STATE_LISTENING,     // Recording audio
    STATE_TRANSCRIBING   // Processing audio → text
} AppState;
```

### Allowed Transitions

| From | To | Trigger |
|------|----|---------|
| IDLE | LISTENING | User clicks mic (model loaded + device valid) |
| LISTENING | TRANSCRIBING | Watchdog timeout OR user click during continuous recording |
| LISTENING | IDLE | Recording start failure |
| TRANSCRIBING | IDLE | Transcription complete (final continuous result with `user_requested_stop`) |
| TRANSCRIBING | LISTENING | Continuous mode restart (NOT used — see note below) |

**Important Note**: The state machine does NOT loop through TRANSCRIBING during active continuous recording. In continuous mode:
- State stays at `STATE_LISTENING` the entire time
- Segments are transcribed in background via scanner callbacks without changing app state
- Only when user clicks mic to stop → LISTENING→TRANSCRIBING (final transcription) → IDLE

### State Controller Implementation

The `AppStateController` struct uses:
- A `pthread_mutex_t` to protect the current state value
- An atomic sequence counter (`int32_t`) for race condition prevention between threads attempting transitions simultaneously
- Callbacks: `on_transcription_result`, `on_model_status_change`, `on_state_change`

Transition function `app_transition_to(controller, target_state)`:
1. Lock mutex
2. Check if transition is allowed (see table above)
3. If allowed: set new state, increment sequence counter, store old state
4. Unlock mutex
5. Invoke `on_state_change` callback with old and new state

Toggle function `app_toggle_state(controller)`:
- IDLE → LISTENING
- LISTENING → TRANSCRIBING
- TRANSCRIBING → no-op (must explicitly transition to IDLE first)

---

## 5. Core Data Structures

### TranscriberApp (Main Application Struct)

```c
typedef struct TranscriberApp {
    AppStateController controller;      // State machine + callbacks
    AudioRecorder *audio_recorder;       // ALSA capture + WAV file writer + ring buffer
    SilenceScanner *silence_scanner;     // Continuous mode silence scanner
    WhisperClient *whisper_client;       // whisper.cpp wrapper
    MainWindow *main_window;            // GTK UI
    TextWindow *text_window;            // Transcription output window
    SystemTray *tray;                   // System tray icon

    /* Threading synchronization */
    pthread_mutex_t scanner_transcribe_mutex;  // Protects concurrent whisper access from scanner + transcription thread
    pthread_mutex_t transcribe_thread_mutex;   // Protects transcribe_thread handle
    GThread *transcribe_thread;                // Background transcription thread handle
    pthread_mutex_t wav_path_mutex;            // Protects current_wav_path
    char *current_wav_path;                    // Current WAV file path for transcription

    /* Continuous mode control */
    atomic_bool user_requested_stop;           // Set when user clicks mic during continuous LISTENING
} TranscriberApp;
```

Key synchronization primitives:
- `scanner_transcribe_mutex`: Prevents the silence scanner callback and the final transcription thread from both calling whisper.cpp simultaneously (whisper.cpp context is NOT thread-safe)
- `user_requested_stop`: Atomic flag. Set to 1 when user clicks mic during continuous LISTENING. Read/cleared atomically in `on_transcription_result()` to decide whether to continue or stop

---

## 6. Subsystems

### 6.1 Ring Buffer

**File**: [`src/app_ring_buffer.c`](src/app_ring_buffer.c:1), [`src/app_ring_buffer.h`](src/app_ring_buffer.h:1)

#### Purpose
Thread-safe circular buffer for in-memory PCM storage. Replaces the need to read from WAV files during continuous mode segmentation.

#### Internal Structure
```c
struct _AudioRingBuffer {
    int16_t *buffer;           // Circular array of samples
    size_t capacity;           // Max number of samples (90 seconds = 1,440,000)
    size_t write_pos;          // Current write position (sample index)
    size_t total_written;      // Total samples written since creation (monotonically increasing)
    pthread_mutex_t mutex;     // Protects all fields
};
```

#### Capacity Calculation
Created with 90-second capacity:
```c
ring_buffer_create(90, recorder->format.sample_rate);
// capacity = 90 * 16000 = 1,440,000 samples = ~2.88 MB (int16_t)
```

#### API

| Function | Description | Thread Safety |
|----------|-------------|---------------|
| `audio_ring_buffer_create(duration_seconds, sample_rate)` | Allocates and initializes buffer | — |
| `ring_buffer_write(rb, data, n_samples)` | Writes samples. Overwrites oldest if full (true ring behavior). Returns number of samples actually written. | Mutex-protected |
| `ring_buffer_read_all(rb, out, max_samples)` | Reads all available samples without consuming. Returns count read. | Mutex-protected |
| `ring_buffer_read_range(rb, offset, count, out)` | Reads a specific range of samples (non-consuming). Used by silence scanner to analyze chunks. | Mutex-protected |
| `ring_buffer_extract_all(rb, out)` | Extracts all samples and resets write position. Used for final transcription extraction. | Mutex-protected |
| `ring_buffer_get_available(rb)` | Returns number of samples available to read | Mutex-protected |
| `audio_ring_buffer_cleanup(rb)` | Frees buffer memory and destroys mutex | — |

#### Access Pattern
- **Writer**: ALSA capture thread calls `ring_buffer_write()` for every 320-sample frame
- **Reader (non-consuming)**: Silence scanner thread calls `ring_buffer_read_range()` to analyze chunks without removing them
- **Reader (consuming)**: Transcription thread calls `ring_buffer_extract_all()` once at the end

### 6.2 VAD (Voice Activity Detection)

**File**: [`src/app_vad.c`](src/app_vad.c:1), [`src/app_vad.h`](src/app_vad.h:1)

#### Purpose
Thin wrapper around WebRTC VAD library (`third_party/webrtc_vad/`). Determines if audio frames contain speech or silence.

#### Internal Structure
```c
struct _VadDetector {
    void *vad_inst;  // WebRtcVad::VadInst instance (opaque)
};
```

#### API

| Function | Description |
|----------|-------------|
| `vad_detector_create(mode)` | Creates VAD detector with mode 0-3. Returns NULL on failure. |
| `vad_process_frame(detector, samples, n_samples, sample_rate)` | Analyzes one frame. Returns `true` if voice detected, `false` for silence. Frame must be exactly 10ms/20ms/30ms (160/320/480 samples at 16kHz). |
| `vad_detector_destroy(detector)` | Frees VAD detector resources. |

#### VAD Modes
```c
typedef enum {
    VAD_MODE_AGGRESSIVE_LOW = 0,   // Least restrictive — detects more as voice
    VAD_MODE_MODERATE_LOW = 1,
    VAD_MODE_MODERATE = 2,         // Default
    VAD_MODE_AGGRESSIVE_HIGH = 3   // Most restrictive — only clear speech detected
} VadMode;
```

#### Usage Contexts
1. **Silence scanner thread**: Creates its own VAD detector instance. Runs VAD with majority-vote on 500ms chunks (see section 6.4).
2. **Final transcription validation**: Creates a temporary VAD detector to verify remaining audio has voice content (>30% of frames must be voice).

### 6.3 Audio Recorder

**File**: [`src/app_audio.c`](src/app_audio.c:1), [`src/app_audio.h`](src/app_audio.h:1)

#### Purpose
Manages ALSA PCM capture, WAV file writing, ring buffer integration, and VAD in the capture thread.

#### Internal Structure (Key Fields)
```c
struct _AudioRecorder {
    AudioFormat format;               // Fixed: 16kHz mono 16-bit PCM
    char device_name[256];            // ALSA device name
    FILE *wav_file;                   // Output WAV file handle
    char wav_path[PATH_MAX];          // Path to current WAV file

    /* Ring buffer */
    AudioRingBuffer *ring_buffer;     // In-memory PCM storage (created in start)

    /* Capture thread */
    pthread_t capture_thread;         // Thread handle
    bool is_recording;                // Atomic flag — set false to stop thread

    /* VAD integration */
    VadDetector *vad_detector;        // VAD instance (NULL in continuous mode — scanner handles segmentation)
    bool vad_active;                  // Always false in continuous mode — all frames go to ring buffer unconditionally
    int vad_mode;                     // VAD mode 0-3 (stored for passing to silence scanner)

    /* Volume level */
    float current_volume_level;         // RMS volume (0.0-1.0) of last frame
};
```

#### WAV File Format
- Standard RIFF/WAV header written at start of recording
- 320-byte WAV header (WAV_HEADER_SIZE = 44 bytes, but code reserves space for chunk size update)
- Data chunk contains raw int16_t PCM samples
- Header is updated at end to set correct data chunk size

#### Capture Thread Loop (`capture_thread_func`)

In continuous mode, `vad_active` is always `false`, so the capture loop writes every frame to the ring buffer unconditionally:

```
while (is_recording) {
    1. Read 320 samples from ALSA via snd_pcm_readi()
    
    2. Calculate volume level (RMS of frame)
    
    3. Write frame to WAV file
    
    4. Write ALL frames directly to ring buffer (no VAD filtering)
}
```

The silence scanner handles all segmentation logic independently, so the capture thread is a simple audio passthrough.

#### Key API Functions

| Function | Description |
|----------|-------------|
| `audio_recorder_start(recorder)` | Creates temp WAV file, creates ring buffer (90s capacity), starts capture thread |
| `audio_recorder_stop(recorder)` | Sets `is_recording=false`, joins capture thread, updates WAV header, closes file |
| `audio_recorder_configure_vad(recorder, active, mode, silence_ms)` | Configures VAD. If `active=true`, creates/destroys VadDetector. Sets vad_active flag. |
| `audio_recorder_get_ring_buffer(recorder)` | Returns pointer to ring buffer (for scanner creation) |
| `audio_recorder_extract_samples(recorder, &samples)` | Extracts all samples from ring buffer. Caller must free returned array. |
| `audio_recorder_delete_wav(recorder)` | Deletes temp WAV file after transcription |

### 6.4 Silence Scanner

**File**: [`src/app_silence_scanner.c`](src/app_silence_scanner.c:1), [`src/app_silence_scanner.h`](src/app_silence_scanner.h:1)

#### Purpose
The core of continuous mode. A dedicated thread that scans the ring buffer in 500ms chunks, runs VAD on each chunk, and triggers transcription callbacks when silence is detected after a minimum segment duration.

#### Internal Structure
```c
struct _SilenceScanner {
    AudioRingBuffer *ring_buffer;     // Reference to shared ring buffer (read-only)

    /* VAD */
    VadDetector *vad_detector;        // Scanner's own VAD instance

    /* Configuration */
    int silence_threshold_ms;         // scanner_silence_ms from config (default: 1500ms)
    int min_segment_duration_ms;      // scanner_min_segment_ms from config (default: 2000ms)

    /* Scan state — protected by mutex */
    pthread_mutex_t mutex;
    size_t scan_offset;               // Current position in ring buffer being scanned (sample index, monotonically increasing since session start)
    size_t transcribed_offset;        // Total samples already sent for transcription (monotonically increasing)
    size_t last_voice_offset;         // Last sample offset where voice was detected
    bool scanning;                    // Set false to stop scanner thread

    /* Silence detection state */
    int consecutive_silence_chunks;   // Number of consecutive silent 500ms chunks
    
    /* Segment tracking for current potential segment */
    size_t segment_start_offset;      // Start offset of current potential segment (where voice started)
    
    /* Callback — invoked on scanner thread when a segment is ready */
    void (*segment_callback)(int16_t *samples, size_t count, void *user_data);
    void *callback_user_data;
};
```

#### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| SCANNER_CHUNK_DURATION_MS | 500ms | Size of each scan chunk |
| SCANNER_CHUNK_SAMPLES | 8000 (500ms * 16kHz / 1000) | Samples per chunk at 16kHz |

#### Scanner Thread Loop (`scanner_thread_func`)

```
while (scanning && ring_buffer has new data since scan_offset) {
    1. Calculate available samples = ring_buffer_get_available() - (scan_offset % capacity)
       If available < SCANNER_CHUNK_SAMPLES, sleep briefly and continue
    
    2. Read one chunk (8000 samples = 500ms) from ring buffer at scan_offset:
       ring_buffer_read_range(ring_buffer, scan_offset, 8000, chunk_buffer)
    
    3. Run VAD on the chunk using majority-vote sub-frame analysis:
       a. Split chunk into 25 sub-frames of 320 samples each (8000/320 = 25 frames)
       b. For each sub-frame, call vad_process_frame()
       c. Count voice_frames vs total_frames
       d. chunk_is_voice = (voice_frames > total_frames / 2)  // Majority vote
    
    4. If chunk_is_voice:
       a. Reset consecutive_silence_chunks to 0
       b. Update last_voice_offset = scan_offset + SCANNER_CHUNK_SAMPLES
       c. If segment_start_offset was not set, set it to scan_offset (start of new potential segment)
    
    5. If chunk is silence:
       a. Calculate segment_duration_ms = (scan_offset - segment_start_offset) / sample_rate * 1000
       
       b. If segment_duration_ms >= min_segment_duration_ms AND consecutive_silence_chunks == 0:
          // This is the FIRST silent chunk after meeting minimum duration — start counting silence
          consecutive_silence_chunks++
       
       c. If consecutive_silence_chunks > 0:
          consecutive_silence_chunks++
          
       d. Calculate total_silence_ms = consecutive_silence_chunks * SCANNER_CHUNK_DURATION_MS (500ms each)
          
       e. If total_silence_ms >= silence_threshold_ms:
          // SILENCE THRESHOLD EXCEEDED — SEGMENT READY FOR TRANSCRIPTION
          
          i.   Calculate segment end: end_offset = last_voice_offset (trim trailing silence)
          ii.  Calculate segment start: start_offset = segment_start_offset
          iii. Segment length in samples: n = end_offset - start_offset
          iv.  Allocate output buffer and copy segment from ring_buffer_read_range()
          v.   Call segment_callback(segment_samples, n, user_data) on scanner thread
          vi.  Update transcribed_offset = end_offset
          vii. Reset: consecutive_silence_chunks = 0, segment_start_offset = not set
    
    6. scan_offset += SCANNER_CHUNK_SAMPLES (8000 samples)
}
```

#### VAD Majority-Vote Sub-Frame Analysis

The scanner does NOT run VAD on the entire 500ms chunk at once. Instead:
1. Splits each 500ms chunk into 25 sub-frames of 320 samples (20ms each)
2. Runs `vad_process_frame()` on each sub-frame individually
3. Counts how many sub-frames detected voice
4. Uses majority vote: if more than half the sub-frames are voice → chunk is considered voice

This provides smoother detection than running VAD once on 8000 samples, as it handles brief pauses within speech segments better.

#### Key API Functions

| Function | Description |
|----------|-------------|
| `silence_scanner_create(rb, vad_mode, silence_ms, min_segment_ms)` | Creates scanner with its own VAD detector initialized to given mode |
| `silence_scanner_start(scanner)` | Resets scan state, creates and starts scanner thread |
| `silence_scanner_stop(scanner)` | Sets scanning=false, joins thread. Does NOT destroy (scanner is reused). |
| `silence_scanner_set_callback(scanner, cb, user_data)` | Sets the segment callback function |
| `silence_scanner_get_transcribed_offset(scanner)` | Returns number of samples already transcribed (used to skip prefix in final transcription) |
| `silence_scanner_cleanup(scanner)` | Destroys VAD detector and frees scanner struct |

#### Segment Callback Signature

```c
typedef void (*ScannerSegmentCallback)(int16_t *samples, size_t count, void *user_data);
```

The callback receives a **newly allocated** array of `count` int16_t samples. The caller (callback implementation) must free the array with `g_free()` after use.

### 6.5 Whisper Transcription Client

**File**: [`src/app_whisper.c`](src/app_whisper.c:1), [`src/app_whisper.h`](src/app_whisper.h:1)

#### Purpose
Wrapper around whisper.cpp providing thread-safe access to the GGML model context. All public functions protect `struct whisper_context *` with a mutex since whisper.cpp is NOT thread-safe.

#### Internal Structure
```c
struct _WhisperClient {
    struct whisper_context *ctx;      // Loaded model context (NULL = not loaded)
    char model_path[MAX_PATH_LEN];    // Path to GGUF/GGML model file
    char language[16];                // Language code or "auto"
    int n_threads;                    // Thread count for transcription (0 = auto-detect)

    pthread_mutex_t mutex;            // Protects ctx and all config fields
    atomic_int cancel_requested;      // Abort flag checked by whisper.cpp callback
    bool is_loading;                  // True while model loading in progress

    int error_code;                   // Last error code (0 = no error)
    char error_message[256];          // Last error message
};
```

#### Key API Functions

| Function | Description | Threading Model |
|----------|-------------|-----------------|
| `whisper_transcribe(client, wav_path)` | Transcribes WAV file. Returns WhisperResponse*. | Locks mutex → loads model (lazy) → copies config → unlocks → runs transcription WITHOUT lock → returns result |
| `whisper_transcribe_samples(client, samples, n_samples)` | Transcribes in-memory int16_t PCM samples. Converts to float32 internally. Returns WhisperResponse*. | Same locking pattern as whisper_transcribe() |
| `whisper_client_set_model_path(client, path)` | Sets model path (locks mutex) | — |
| `whisper_client_set_language(client, lang)` | Sets language (locks mutex) | — |
| `whisper_client_is_model_loaded(client)` | Checks if ctx != NULL (locks mutex) | — |
| `whisper_client_cancel(client)` | Sets atomic cancel flag to 1 | Lock-free (atomic) |

#### WhisperResponse Structure

```c
typedef struct {
    bool success;                     // true = transcription succeeded
    char *text;                       // Transcribed text (malloc'd, caller must free via whisper_response_free())
    int error_code;                   // 0 = no error, >0 = error code
    char error_message[256];          // Human-readable error message
} WhisperResponse;
```

#### Transcription Parameters

The whisper.cpp transcription uses these fixed parameters:
- Sampling strategy: `WHISPER_SAMPLING_GREEDY`
- Threads: Config value or auto-detect (`sysconf(_SC_NPROCESSORS_ONLN)`)
- Print progress: false
- Print realtime: false
- Print special tokens: false
- Translate: false (always transcribe in source language)
- No timestamps: true

#### Sample Conversion

In `whisper_transcribe_samples()`:
```c
// Convert int16_t PCM to float32 [-1.0, 1.0] for whisper.cpp
float *float_samples = malloc(n_samples * sizeof(float));
for (int i = 0; i < n_samples; i++) {
    float_samples[i] = (float)samples[i] / 32768.0f;
}
```

#### Retry Functions

| Function | Description |
|----------|-------------|
| `whisper_transcribe_with_retry(client, wav_path, max_retries)` | Retries on error codes 4 (read), 5 (decode), 7 (memory). Exponential backoff: 100ms * attempt_number. |
| `whisper_transcribe_samples_with_retry(client, samples, n_samples, max_retries)` | Same retry logic for in-memory transcription. Retries on error codes 5 and 7 only. |

### 6.6 State Controller

**File**: [`src/app_controller.c`](src/app_controller.c:1)

#### Purpose
Provides thread-safe state machine with atomic transitions and callback notifications.

#### Transition Logic

```c
int app_transition_to(AppStateController *controller, AppState target) {
    pthread_mutex_lock(&controller->mutex);
    
    switch (controller->state) {
        case STATE_IDLE:
            if (target == STATE_LISTENING) {
                old_state = controller->state;
                controller->state = target;
                controller->sequence++;
                allowed = true;
            }
            break;
            
        case STATE_LISTENING:
            if (target == STATE_TRANSCRIBING) {
                old_state = controller->state;
                controller->state = target;
                controller->sequence++;
                allowed = true;
            }
            break;
            
        case STATE_TRANSCRIBING:
            if (target == STATE_IDLE || target == STATE_LISTENING) {
                old_state = controller->state;
                controller->state = target;
                controller->sequence++;
                allowed = true;
            }
            break;
    }
    
    pthread_mutex_unlock(&controller->mutex);
    
    if (allowed && controller->on_state_change) {
        controller->on_state_change(old_state, new_state, user_data);
    }
    
    return allowed ? 0 : -1;
}
```

---

## 7. Continuous Mode Flow — Complete Sequence

This section describes the exact sequence of operations from mic click to final stop in continuous mode.

### Phase 1: Start Recording (IDLE → LISTENING)

Triggered by user clicking microphone icon when state is IDLE.

1. **Pre-flight checks** (`on_microphone_toggle`):
   - Check if model is still loading (reject click if `model_loading == true`)
   - Validate configured audio device exists and is available
   - Validate Whisper model file exists and has valid magic header (GGML/GGUF)
   - If model not yet loaded → start background model loading thread, return

2. **State transition** (`app_toggle_state` / `on_state_change` callback):
   - Transition: IDLE → LISTENING via state controller
   - Invokes `handle_enter_listening(app)` callback

3. **Handle Enter Listening** (`handle_enter_listening`):
   a. Check append mode config. If NOT append mode → clear text window
   b. Update UI: set MainWindow to STATE_LISTENING (green mic + sine wave animation)
   c. In continuous mode → stop countdown timer (no max duration countdown shown)
   d. **Configure VAD**: Call `audio_recorder_configure_vad(recorder, false, vad_mode, 0)` with `vad_active=false` — VAD is disabled in the capture thread; silence scanner handles all segmentation
   e. **Start recording**: Call `audio_recorder_start(recorder)`:
      - Creates temp WAV file
      - Creates ring buffer: `ring_buffer_create(90, 16000)` → capacity = 1,440,000 samples
      - Starts ALSA capture thread (reads 320 samples at a time, writes to WAV + ring buffer)
   f. Start watchdog timer (`start_watchdog_timer`) — configurable max recording duration
   g. Start volume level polling
   h. **Create silence scanner**:
      - Get ring buffer: `rb = audio_recorder_get_ring_buffer(recorder)`
      - Create scanner: `silence_scanner_create(rb, vad_mode, scanner_silence_ms, scanner_min_segment_ms)`:
        * Allocates SilenceScanner struct
        * Creates VAD detector with specified mode
        * Stores config values (silence_threshold_ms, min_segment_duration_ms)
      - Set callback: `silence_scanner_set_callback(scanner, on_scanner_segment, app)`
   i. **Start silence scanner**: `silence_scanner_start(scanner)`:
      - Resets scan state (scan_offset=0, transcribed_offset=0, etc.)
      - Creates and starts scanner thread

### Phase 2: Continuous Recording + Segmentation Loop

At this point, three threads are running concurrently:

| Thread | Responsibility | Accesses |
|--------|---------------|----------|
| ALSA Capture Thread | Reads audio from device → writes to WAV file + ring buffer | Writes to ring buffer (via `ring_buffer_write`) |
| Silence Scanner Thread | Scans ring buffer in 500ms chunks → detects silence → triggers transcription | Reads from ring buffer (via `ring_buffer_read_range`, non-consuming) |
| GTK Main Thread | UI updates, state management, callback dispatch | Coordinates all threads via callbacks and mutexes |

#### Scanner Detection Cycle

The scanner thread runs this loop continuously:

```
while (scanning && new data available in ring buffer beyond scan_offset):
    1. Wait until at least SCANNER_CHUNK_SAMPLES (8000) of new data available
       If not enough → sleep 50ms, retry
    
    2. Read chunk = 8000 samples from ring_buffer at position scan_offset
    
    3. Run VAD majority-vote on chunk:
       - Split into 25 sub-frames × 320 samples each
       - Count voice frames vs total
       - chunk_is_voice = (voice_frames > 12)  // More than half
    
    4a. If VOICE detected:
        - Reset consecutive_silence_chunks = 0
        - Update last_voice_offset = scan_offset + 8000
        - Set segment_start_offset if not already set
    
    4b. If SILENCE detected AND segment meets minimum duration:
        - Increment consecutive_silence_chunks
        - total_silence_ms = consecutive_silence_chunks * 500
        
        - If total_silence_ms >= silence_threshold_ms (default 1500ms):
            // SEGMENT READY — invoke transcription callback
            
            a. segment_end = last_voice_offset  // Trim trailing silence
            b. segment_start = segment_start_offset
            c. n_samples = segment_end - segment_start
            d. Allocate buffer, copy samples via ring_buffer_read_range()
            e. Call on_scanner_segment(samples, n_samples, app)
            f. transcribed_offset = segment_end
            g. Reset consecutive_silence_chunks = 0, segment_start_offset = unset
    
    5. scan_offset += 8000
```

#### Scanner Segment Transcription (`on_scanner_segment`)

Called on the **scanner thread** when a silence-triggered segment is ready:

1. Guard check: if `app->whisper_client` is NULL → free samples, return
2. Acquire `scanner_transcribe_mutex` (prevents concurrent whisper access)
3. Call `whisper_transcribe_samples(client, samples, count)` — blocks until complete
4. Release `scanner_transcribe_mutex`
5. Free samples with `g_free()`
6. If success:
   - Allocate `IdleCallbackData` struct with duplicated text result
   - Marshal to GTK main thread via `g_idle_add(on_transcription_result_idle, icd)`
   - Restart watchdog timer via `g_idle_add(restart_watchdog_idle, app)`
7. If failure:
   - Extract error message from response or whisper client
   - Allocate `IdleCallbackData` with error text
   - Marshal to GTK main thread via `g_idle_add(on_transcription_error_idle, icd)`

#### GTK Main Thread Result Handler (`on_transcription_result`)

Called on the **GTK main thread** (marshaled from scanner callback):

1. Stop transcription watchdog timer
2. If success with text:
   - Log raw transcription output
   - Append to TextWindow: `app_text_window_append_text(text)` — adds space separator if content exists, scrolls to end, shows window
   - Copy to clipboard (PRIMARY + CLIPBOARD)
3. Check continuous mode config
4. Read and clear `user_requested_stop` flag atomically
5. **Decision**:
   - If `continuous && success && !user_stopped` → Do NOT transition to IDLE. Log message. Scanner continues running.
   - Otherwise → Transition to IDLE, update UI

### Phase 3: User Stop (LISTENING → TRANSCRIBING → IDLE)

Triggered by user clicking microphone icon while in LISTENING state during continuous mode.

1. **Mic click handler** (`on_microphone_toggle`):
   - Current state is STATE_LISTENING, continuous mode enabled
   - Set `atomic_store(&app->user_requested_stop, 1)` — signals to transcription result handler that this was an explicit user stop
   - Call `app_toggle_state()` → triggers LISTENING → TRANSCRIBING transition

2. **State controller** detects LISTENING → TRANSCRIBING:
   - Updates state, increments sequence counter
   - Invokes `on_state_change` callback with old=LISTENING, new=TRANSCRIBING

3. **Handle Enter Transcribing** (`handle_enter_transcribing`):
   a. Play system beep via GDK display
   b. Stop watchdog timer and volume polling
   c. Update UI to STATE_TRANSCRIBING (green mic, no animation)
   d. Store WAV path for transcription
   e. Start background transcription thread: `g_thread_new("transcribe", transcribe_thread_func, app)`

4. **Transcription Thread** (`transcribe_thread_func`):
   
   This is the final transcription that processes any remaining audio in the ring buffer after all scanner-segmented portions have been handled.
   
   a. Set model path and language on whisper client
   
   b. **Stop silence scanner first**: `silence_scanner_stop(app->silence_scanner)` — prevents concurrent ring buffer access
   
   c. **Stop audio recording**: `audio_recorder_stop(app->audio_recorder)` — stops capture thread, joins it
   
   d. **Extract samples from ring buffer**: `n_samples = audio_recorder_extract_samples(recorder, &samples)`
   
   e. **Skip already-transcribed prefix**:
      ```c
      size_t already_transcribed = silence_scanner_get_transcribed_offset(scanner);
      if (already_transcribed > 0 && already_transcribed < n_samples) {
          memmove(samples, samples + already_transcribed, 
                  (n_samples - already_transcribed) * sizeof(int16_t));
          n_samples -= already_transcribed;
      } else if (already_transcribed >= n_samples) {
          // All samples were already transcribed — skip final transcription
          g_free(samples);
          samples = NULL;
          n_samples = 0;
      }
      ```
   
   f. **Trim trailing silence**: `n_samples = audio_trim_trailing_silence(samples, n_samples, 16000)`
   
   g. **Validate minimum duration** (skip if < 500ms remaining):
      ```c
      int remaining_ms = n_samples / 16000.0 * 1000.0;
      if (remaining_ms < 500) { skip transcription }
      ```
   
   h. **VAD voice content validation**:
      - Create temporary VAD detector with `VAD_MODE_MODERATE`
      - Process remaining audio in 320-sample frames
      - Count voice_frames vs total_frames
      - Require at least 30% of frames to be voice: `voice_frames > n_frames * 0.3`
      - If not enough voice → skip transcription (log message)
   
   i. **Transcribe**:
      - If samples available → `whisper_transcribe_samples(client, samples, count)`
      - Fallback if ring buffer empty → `whisper_transcribe_with_retry(client, wav_path, max_retries)`
      - In continuous mode with no data (VAD filtered all) → create success response with empty text
   
   j. **Marshal result to GTK main thread** via `g_idle_add(on_transcription_result_idle, icd)`

5. **GTK Main Thread Final Result Handler**:
   
   Same as step 4 above (`on_transcription_result`), but now:
   - `user_requested_stop` was set to 1 in step 1 and read/cleared here via `atomic_exchange(&app->user_requested_stop, 0)`
   - Since `user_stopped == true`, the condition `continuous && success && !user_stopped` is **false**
   - Therefore: Transition to IDLE, update UI (red mic), stop tray animation

---

## 8. Threading Model

### Thread Inventory

| Thread | Created By | Purpose | Lifecycle |
|--------|-----------|---------|-----------|
| GTK Main Thread | `gtk_main()` | UI updates, state management, callback dispatch | Entire app lifetime |
| ALSA Capture Thread | `audio_recorder_start()` → `pthread_create` | Reads audio from device, writes WAV + ring buffer | Recording session (created/destroyed per recording) |
| Silence Scanner Thread | `silence_scanner_start()` → `pthread_create` | Scans ring buffer for silence segments | Recording session (created at start, stopped at final transcription) |
| Transcription Thread | `handle_enter_transcribing()` → `g_thread_new("transcribe", ...)` | Final transcription of remaining audio | Per-stop sequence (one-shot) |
| Model Loading Thread | `on_microphone_toggle()` or startup → `g_thread_new("model_loading", ...)` | Loads Whisper model in background | One-shot per load |

### Mutex Inventory

| Mutex | Protects | Used By |
|-------|----------|---------|
| `AppStateController.mutex` | Current state + sequence counter | Any thread calling transition functions |
| `AudioRingBuffer.mutex` | Buffer write position, total written | Capture thread (write), scanner thread (read_range), transcription thread (extract_all) |
| `SilenceScanner.mutex` | Scan offset, transcribed offset, last voice offset, silence counters | Scanner thread (owns), main thread (get_transcribed_offset) |
| `WhisperClient.mutex` | Model context + config fields | Any thread calling whisper API functions |
| `TranscriberApp.scanner_transcribe_mutex` | Prevents concurrent whisper access from scanner callback AND transcription thread | Scanner thread (callback), transcription thread |
| `TranscriberApp.transcribe_thread_mutex` | Transcription thread handle | Main thread, new transitions |
| `TranscriberApp.wav_path_mutex` | Current WAV file path | Any thread accessing wav_path |

### Atomic Variables

| Variable | Type | Purpose |
|----------|------|---------|
| `user_requested_stop` | `atomic_bool` | Set by main thread when user clicks mic during continuous LISTENING. Read/cleared by transcription result handler to decide whether to continue or stop. |
| `WhisperClient.cancel_requested` | `atomic_int` | Abort flag for whisper.cpp transcription. Checked periodically by whisper.cpp internal loop. |
| `model_loading_from_toggle` | `atomic_int` | Flag indicating model load was triggered by user click (auto-transition after load) vs startup. |

---

## 9. Cross-Thread Communication Patterns

### Pattern 1: Callback Marshaling to GTK Main Thread

Used for: Transcription results, VAD stop trigger, UI updates from worker threads

```c
// From worker thread:
IdleCallbackData *icd = g_new0(IdleCallbackData, 1);
icd->app = app;
icd->data = g_strdup(response_text_or_error);
g_idle_add(on_transcription_result_idle, icd);

// GTK main thread callback:
static gboolean on_transcription_result_idle(gpointer user_data) {
    IdleCallbackData *icd = (IdleCallbackData *)user_data;
    TranscriberApp *app = icd->app;
    
    if (app->controller.on_transcription_result) {
        app->controller.on_transcription_result(app, icd->data, true);
    }
    
    g_free(icd->data);
    g_free(icd);
    return FALSE;  // One-shot callback
}
```

### Pattern 2: Atomic Flag for Stop Signaling

Used for: `user_requested_stop` in continuous mode

Set from main thread, read/cleared atomically in result handler. No mutex needed because it's a single flag with no compound operations.

### Pattern 3: Mutex-Protected Shared Resource Access

Used for: Ring buffer (capture writes, scanner reads non-consumingly, transcription extracts), Whisper client context

Always lock → access → unlock pattern. Never hold multiple locks simultaneously to prevent deadlocks. The exception is the `scanner_transcribe_mutex` which is held during the entire whisper.cpp call because whisper.cpp itself may use internal threading.

---

## 10. User Stop in Continuous Mode

The user stop mechanism uses an atomic flag because:
1. The GTK main thread receives the mic click
2. But the actual "should I continue or stop?" decision happens inside `on_transcription_result()` which runs after each segment transcription completes
3. There's a race between the next scanner callback firing and the user clicking

The sequence:
```
[Main Thread] User clicks mic during LISTENING (continuous mode)
    ↓
atomic_store(&app->user_requested_stop, 1)
    ↓
app_toggle_state() → triggers LISTENING→TRANSCRIBING transition
    ↓
[Transcription Thread] Final transcription starts

// Meanwhile, a scanner callback might still be in-flight:
[Scanner Thread] on_scanner_segment() fires
    ↓
whisper_transcribe_samples() completes
    ↓
g_idle_add(on_transcription_result_idle) marshals to main thread
    ↓
[Main Thread] on_transcription_result() called
    ↓
user_stopped = atomic_exchange(&app->user_requested_stop, 0)  // Reads and clears flag
    ↓
if (continuous && success && !user_stopped): continue
else: transition to IDLE
```

The `atomic_exchange` is critical — it both reads the value AND clears it atomically in one operation. Without clearing, subsequent transcriptions would see a stale stop flag.

---

## 11. Final Transcription (Stop Sequence)

When user stops continuous recording, the final transcription thread performs additional processing that scanner-segmented transcriptions do not:

### Steps Unique to Final Transcription

1. **Skip already-transcribed prefix**: Uses `silence_scanner_get_transcribed_offset()` to find how many samples were already handled by the scanner. Moves remaining untranscribed portion to start of buffer.

2. **Trim trailing silence**: Calls `audio_trim_trailing_silence(samples, n_samples, sample_rate)` which:
   - Scans from end of buffer backward in 320-sample frames
   - Runs VAD on each frame
   - Stops when first voice-containing frame found (from the end)
   - Returns new count excluding trailing silence

3. **Minimum duration check**: Requires at least 500ms remaining after trimming to prevent whisper hallucinations on noise-only clips

4. **VAD voice content validation**: Creates temporary VAD detector, processes all remaining frames, requires >30% of frames to be voice. This prevents transcribing segments that are mostly background noise (which causes whisper.cpp to hallucinate text like "Thank you")

5. **Empty result handling for continuous mode**: If both ring buffer and WAV path have no data after processing → creates a success response with empty string rather than an error, so the app transitions cleanly to IDLE without showing an error dialog

---

## 12. Watchdog Timer

### Purpose
Prevents recording from running indefinitely due to VAD failure or scanner bugs.

### Configuration
- Max duration: `config->max_duration` seconds (default 30)
- Displayed as countdown timer in status bar during LISTENING state
- In continuous mode, countdown is hidden but watchdog still runs internally

### Implementation
A GLib timeout source (`g_timeout_add_seconds`) that decrements a counter every second. When counter reaches 0:
1. Calls `app_toggle_state()` → triggers LISTENING → TRANSCRIBING transition
2. The transcription process handles the rest normally

### Restart in Continuous Mode
After each successful scanner-segmented transcription, the watchdog timer is restarted via `restart_watchdog_idle`. This prevents the max duration from expiring during long continuous dictation sessions where the user is actively speaking with natural pauses.

---

## 13. Error Handling and Edge Cases

### Audio Device Errors
- If configured device not available → show error dialog, reject recording start
- ALSA errors (device busy, permission denied) → captured by `audio_recorder_start()` returning false → transition back to IDLE with error in text window

### Model Loading Failures
- Model file not found / invalid magic header → show error dialog before starting recording
- GPU initialization failure → falls back to CPU automatically via whisper.cpp auto-detection
- Model loading during startup → shows "WAIT" overlay on mic icon, rejects clicks until loaded

### Transcription Errors
- Whisper returns empty result → logged as error in text window (unless continuous mode with no data)
- Whisper decode failure (`error_code=5`) or memory allocation failure (`error_code=7`) → retried up to `WHISPER_MAX_RETRIES` times with exponential backoff (100ms * attempt_number)

### Ring Buffer Overflow
The ring buffer overwrites oldest samples when full. In continuous mode, the scanner processes data as fast as it arrives (500ms chunks). If the scanner falls behind (e.g., during long whisper.cpp processing), older audio may be overwritten — this is acceptable as a trade-off for bounded memory usage.

### Scanner Falls Behind
If transcription takes longer than 500ms, the scanner thread sleeps waiting for new data while the transcription runs under `scanner_transcribe_mutex`. This means segments are processed sequentially (not pipelined), preventing whisper.cpp concurrent access issues.

---

## 14. Platform-Independent Abstractions for Windows Porting

### Subsystems That Must Be Replaced

| Linux Component | Windows Replacement | Notes |
|----------------|--------------------|-------|
| ALSA (`snd_pcm_*`) | WASAPI (Waveout Audio Session API) or WaveIn | Use `waveInOpen`/`waveInStart` for simplicity, or WASAPI for low-latency. Must provide 16kHz mono int16_t PCM at 320-sample buffers matching WebRTC VAD frame size |
| GTK3 UI | Win32 API / WPF / WinUI | Replace MainWindow with equivalent window showing mic icon, status bar. Replace TextWindow with text editor. State updates must occur on the UI thread (message pump) |
| GLib utilities (`g_thread`, `g_idle_add`, etc.) | Windows threads + message queue or C11 atomics + condition variables | `g_idle_add()` → PostMessage() to main window's message queue. `g_thread_new()` → `_beginthreadex` or `CreateThread`. `g_timeout_add()` → `SetTimer()` or `SleepEx()` with alertable I/O |
| GLib memory (`g_malloc0`, `g_strdup`) | Standard C (`calloc`, `strdup`) | Straightforward replacement |
| D-Bus (single-instance service) | Named Mutex (`CreateMutex` with name) | For single-instance enforcement on Windows |
| System Tray (`GtkStatusIcon` / `AppIndicator`) | Shell_NotifyIcon API | Windows notification area icon |
| Clipboard (`GdkClipboard`) | OpenClipboard/SetClipboardData | Windows clipboard API for both PRIMARY and CLIPBOARD equivalents |

### Subsystems That Are Portable (No Changes Needed)

| Component | Notes |
|-----------|-------|
| WebRTC VAD (`third_party/webrtc_vad/`) | Already portable C code. Compile as-is on Windows with MSVC or MinGW |
| whisper.cpp | Already has Windows build support via CMake. Same API calls work identically |
| Ring Buffer (algorithm) | Pure C data structure using pthreads mutex → replace with `SRWLOCK` or critical section |
| Silence Scanner (algorithm) | Pure logic, no platform dependencies. Replace pthread_create + mutex |
| VAD wrapper (`app_vad.c`) | Thin layer around WebRTC VAD — fully portable |
| State Controller algorithm | Portable logic, just replace pthread_mutex with Windows synchronization primitives |
| Configuration (JSON via cJSON) | Fully portable |

### Critical Constants to Preserve

These MUST be identical in the Windows port:

```c
#define AUDIO_SAMPLE_RATE     16000       // Hz
#define AUDIO_CHANNELS        1           // Mono
#define AUDIO_SAMPLE_FORMAT   int16_t     // Signed 16-bit PCM
#define VAD_FRAME_SAMPLES     320         // 20ms at 16kHz — MUST match WebRTC VAD expectations
#define SCANNER_CHUNK_MS      500         // Scanner processes 500ms chunks
#define SCANNER_CHUNK_SAMPLES (AUDIO_SAMPLE_RATE * SCANNER_CHUNK_MS / 1000)  // = 8000
#define RING_BUFFER_DURATION_S 90         // 90 seconds capacity
#define VAD_SUB_FRAMES_PER_CHUNK 25       // 8000/320 = 25 sub-frames for majority vote
#define MIN_SEGMENT_VALIDATION_MS 500     // Skip transcription if < 500ms remaining after trim
#define VOICE_CONTENT_THRESHOLD 0.3f      // Require >30% of frames to be voice in final validation
```

### Thread Safety Primitives Mapping

| GLib/Pthread | Windows Equivalent |
|-------------|-------------------|
| `pthread_mutex_t` + lock/unlock | `SRWLOCK` + `AcquireSRWLockExclusive`/`ReleaseSRWLockExclusive` OR `CRITICAL_SECTION` + `EnterCriticalSection`/`LeaveCriticalSection` |
| `GThread *` + `g_thread_new()` | `_beginthreadex()` (preferred for CRT compatibility) or `CreateThread()` |
| `g_thread_join()` | `WaitForSingleObject(thread_handle, INFINITE)` |
| `atomic_bool` / `atomic_int` | C11 `<stdatomic.h>` works on MSVC 2019+ and MinGW. Same syntax as Linux. |
| `g_idle_add()` | `PostMessage(hwnd, WM_APP + n, wParam, lParam)` to marshal callbacks to main thread's message loop |
| `g_timeout_add(ms, cb, data)` | `SetTimer(NULL, id, ms, callback)` or a dedicated timer thread with `SleepEx()` |

### Build System Notes for Windows

- CMake already supports cross-platform builds. The existing `CMakeLists.txt` can be extended with Windows-specific targets
- whisper.cpp has its own CMake build that works on Windows (including optional CUDA)
- WebRTC VAD needs to be compiled as a static library — same source files, different compiler flags for MSVC
- Consider using vcpkg or Conan for system dependencies (cJSON, etc.)

---

## Summary Checklist for Implementation

An agent implementing the Windows port should verify these items:

- [ ] Audio capture produces 16kHz mono int16_t PCM in 320-sample (20ms) frames
- [ ] Ring buffer implementation is thread-safe with correct capacity (90s = 1,440,000 samples)
- [ ] VAD wrapper calls WebRTC VAD correctly with 20ms frames and returns bool
- [ ] Silence scanner processes ring buffer in exactly 500ms chunks using majority-vote VAD on 25 sub-frames
- [ ] Scanner segment callback transcribes via `whisper_transcribe_samples()` under mutex protection
- [ ] Transcription results marshaled to main thread (equivalent of `g_idle_add`)
- [ ] `user_requested_stop` atomic flag correctly set/cleared for continuous mode stop
- [ ] Final transcription skips already-transcribed prefix using `transcribed_offset`
- [ ] Trailing silence trimming and VAD voice content validation (>30% threshold) in final transcription
- [ ] State machine allows: IDLE→LISTENING, LISTENING→TRANSCRIBING, TRANSCRIBING→IDLE
- [ ] In continuous mode: state stays at LISTENING during active recording; only transitions to TRANSCRIBING on user stop
- [ ] Watchdog timer restarts after each successful scanner-segmented transcription
- [ ] Whisper client mutex protects model context from concurrent access

---

*End of Specification*
