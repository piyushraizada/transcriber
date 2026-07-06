# Software Requirements Specification (SRS)
## Transcriber — Voice-to-Text Application

**Document Version:** 3.0
**Date:** 2026-07-06
**Author:** System Architecture Team
**Status:** Production-Ready

### Revision History

| Version | Date | Changes |
|---------|------|---------|
| 3.0 | 2026-07-06 | Major rewrite: stripped implementation and architectural design details. Retained user-facing capabilities, configuration settings reference, and behavioral requirements only. Removed threading model, API call sequences, widget internals, code snippets, test plan, and deployment procedures. |
| 2.5 | 2026-05-31 | Updated constants to match implementation: max_duration default 60s (was 30s), range 5-120 (was 5-30), vad_silence_ms range 500-5000 (was 500-3000), config key vad_silence_ms (was vad_silence_timeout_ms), VAD mode labels updated to match UI, audio buffer size 320 (was 1024), silence timeout UI uses seconds (was ms), removed Session Duration field (not in implementation), removed max_session_minutes config parameter |
| 2.4 | 2026-05-30 | Augmented transcription behavior with VAD-driven continuous segmentation: audio recording is monitored in real-time by a Voice Activity Detector that automatically segments speech at natural silence boundaries, transcribing each segment asynchronously while recording continues |

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Application Capabilities](#2-application-capabilities)
3. [Configuration Settings Reference](#3-configuration-settings-reference)
4. [Non-Functional Requirements](#4-non-functional-requirements)
5. [Error Behaviors](#5-error-behaviors)
6. [Hotkey Integration](#6-hotkey-integration)
7. [Appendix — Configuration File Example](#7-appendix--configuration-file-example)

---

## 1. Introduction

### 1.1 Purpose

This document specifies the requirements for **Transcriber**, a GTK3-based voice-to-text application for Linux desktop environments. The application captures audio from a configured microphone, performs local speech-to-text transcription using Whisper models via whisper.cpp (fully offline, no network required), and displays the transcribed text in an editable window.

### 1.2 Scope

The Transcriber application provides:
- Click-to-start recording with visual state feedback (red/green microphone icon)
- Animated sine wave visualization during active recording
- Voice Activity Detection (VAD) that automatically segments speech at natural silence boundaries
- Asynchronous transcription of each speech segment while recording continues
- Persistent, editable text area displaying accumulated transcribed text
- System clipboard integration for copying transcribed text to other applications
- Configurable model path, audio device, transcription language, GPU mode, VAD sensitivity, silence timeout, max segment duration, text append/overwrite mode, and continuous dictation toggle
- GPU (CUDA) acceleration support with automatic CPU fallback
- System tray icon with state-aware display and context menu
- Global hotkey support via D-Bus for toggling recording without clicking the window
- Configuration dialog accessible from a gear icon on the status bar

### 1.3 Definitions

| Term | Definition |
|------|------------|
| VAD | Voice Activity Detection — real-time analysis of audio to distinguish speech from silence, used to automatically segment continuous recording into transcribable clips |
| Speech Segment | A contiguous portion of recorded audio bounded by silence gaps (detected by VAD) or the maximum segment duration timer, processed as a single transcription unit |
| TextWindow | Persistent text display window attached to the application that displays transcribed text, allows in-place editing, and supports copying to the system clipboard |
| whisper.cpp | Local, offline C/C++ implementation of Whisper inference (ggml-org/whisper.cpp) |
| GGML/GGUF | Model file format used by whisper.cpp for storing speech-to-text models |

### 1.4 Application States

The application operates in three states:

- **IDLE** — Microphone icon is red. No recording or transcription active. The model may be loaded or unloaded depending on prior usage.
- **LISTENING** — Microphone icon is green with animated sine wave overlay. Audio capture is active and VAD monitors the stream for speech/silence boundaries. Speech segments are flushed to transcription asynchronously while recording continues.
- **TRANSCRIBING** — Microphone icon is green (animation stopped). Recording has ended; the final audio segment is being transcribed. After completion, text is displayed and the application returns to IDLE.

---

## 2. Application Capabilities

### 2.1 Audio Capture

- Records from the system default microphone or a user-selected ALSA capture device
- Audio format: 16 kHz sample rate, mono, 16-bit PCM (fixed for Whisper model compatibility)
- User can select available microphones through the Configuration Dialog
- Application validates that the configured audio device is available before starting recording

### 2.2 Voice Activity Detection

- Real-time VAD analyzes captured audio to detect speech and silence boundaries
- When silence persists for a configurable duration, the current speech segment is flushed for transcription
- A maximum segment duration timer serves as a fallback — if no silence is detected within the configured limit, the segment is forcibly flushed and a new one begins
- Multiple segments can be transcribed concurrently during a single recording session

### 2.3 Transcription

- Local Whisper model inference via whisper.cpp — fully offline, no network required
- Model loads lazily on first transcription request in a background thread (does not block the UI)
- GPU (CUDA) acceleration supported with automatic CPU fallback when GPU is unavailable or has insufficient memory
- User-selectable GPU mode: Auto (best free memory), CPU Only, or specific GPU device
- Transcription results are appended to (or replace) the TextWindow depending on configuration

### 2.4 User Interface

#### Main Window
- Displays red microphone icon when idle, green microphone icon during recording/transcribing
- Animated sine wave overlay visible only during LISTENING state
- Status bar with gear/settings button (left), countdown timer (center), and model availability indicator (right)
- Real-time volume level bar displayed during recording
- Non-resizable fixed-size window; position persists across sessions

#### TextWindow
- Persistent text area attached to the main window displaying transcribed text
- Supports full text editing — character input, selection, undo, line wrapping, scrolling
- Copy selected or all text to system clipboard via standard keyboard shortcuts (Ctrl+C)
- Positioned below the main window by default; repositions above if it would extend past screen bottom

#### Model Availability Indicator
- 8x8 pixel circle in the status bar:
  - **Green** — model file verified and accessible
  - **Red** — model not found or verification failed
  - **Yellow/blinking** — verification in progress
  - **Amber/solid** — model loading in background
- Clickable to trigger manual re-verification

#### System Tray Icon
- State-aware icon (red mic when idle, green mic during recording/transcribing)
- Dynamic tooltip reflecting current state:
  - "Transcriber — Ready" / "Transcriber — Model unavailable" (IDLE)
  - "Transcriber — Recording..." (LISTENING)
  - "Transcriber — Transcribing..." (TRANSCRIBING)
- Right-click context menu with items: Toggle Recording, Show Window, Quit

#### Configuration Dialog
- Modal dialog titled "Transcriber Settings" opened by clicking the gear icon on the status bar
- Positioned adjacent to the main window (right side with screen-edge awareness)
- Contains all configurable settings organized in sections: Whisper Model Path, Audio Device, Language, Max Duration, GPU Acceleration, Transcription Text Mode, Voice Activity Detection (VAD), D-Bus Hotkey Command, and Reset Window Position
- Action buttons: Save, Cancel
- "Save" validates inputs, writes to `~/.config/transcriber/config.json`, closes dialog. "Cancel" discards changes.
- Unsaved changes are discarded if closed via X button or Escape key

### 2.5 Recording Lifecycle

1. User clicks red microphone icon (or invokes D-Bus toggle / tray menu)
2. Application verifies model availability and audio device
3. Transitions to LISTENING — starts recording, shows green mic with animation
4. VAD monitors audio stream; speech segments are flushed at silence boundaries or max duration timeout
5. Each segment is transcribed asynchronously; results appear in TextWindow while recording continues
6. User clicks green microphone icon (or invokes toggle) to stop
7. Transitions to TRANSCRIBING — stops animation, processes final segment
8. Audible beep signals transcription start
9. Final text displayed, clipboard updated, returns to IDLE with red mic

### 2.6 Clipboard Integration

- System clipboard populated automatically after each transcription result
- Supports both PRIMARY selection (middle-click paste on X11) and CLIPBOARD (Ctrl+V paste)
- Plain text UTF-8 format
- Text persists in TextWindow after copying for editing and re-copying

### 2.7 Model Management

- User configures model path via Configuration Dialog (absolute, relative, or bare filename)
- Bare filenames are searched in default directories: `~/.cache/whisper/` then `/usr/share/transcriber/models/`
- Application verifies model file accessibility on startup and before recording begins
- The Browse button validates the selected model file and displays its metadata (name, quantization, multilingual support) before saving

---

## 3. Configuration Settings Reference

All settings are stored in `~/.config/transcriber/config.json`. The application auto-creates this file with defaults if it does not exist. File permissions: `600` (rw-------).

### 3.1 User-Configurable Settings

| # | Setting Name | UI Label | Type / Widget | Default | Range / Options | Description |
|---|-------------|----------|---------------|---------|-----------------|-------------|
| 1 | `audio_device` | Audio Device | Drop-down (combo box) | `"default"` | Runtime-detected ALSA capture devices; "Default" always first option | Select the audio capture device. "Default" uses the system default microphone. |
| 2 | `model_path` | Whisper Model Path | Text input + Browse button | `"ggml-large-v3-turbo-q8_0.bin"` | Any valid file path or bare filename (searched in default directories) | Path to the local GGML/GGUF Whisper model file. A "Browse..." button opens a file chooser filtered to `.bin` and `.gguf` files, defaulting to `~/.cache/whisper/`. Model metadata is displayed upon selection. |
| 3 | `max_duration` | Max Recording Duration (seconds) | Numeric input with spin buttons | `30` | 5 – 30 seconds, integer step | Maximum speech segment duration. If no silence is detected within this time, the current segment is flushed for transcription and a new one begins. |
| 4 | `vad_mode` | Sensitivity | Drop-down (combo box) | `"Moderate"` (mode 1) | Least sensitive (0), Moderate (recommended) (1), Aggressive (2), Most aggressive (only clear speech) (3) | Controls how aggressively the Voice Activity Detector distinguishes speech from background noise. Higher values reduce false positives but may miss quiet speech. |
| 5 | `scanner_silence_ms` | Auto-stop after silence | Numeric input with spin buttons | `2000` ms (displayed as 2.0 seconds) | 1000 – 10000 ms (UI: 1.0 – 10.0 s, step 0.1) | Duration of consecutive silence before the scanner triggers a segment flush and restarts recording in continuous dictation mode. |
| 6 | `gpu_mode` | GPU Acceleration | Drop-down (combo box) | `"auto"` | Auto, CPU Only, GPU N (dynamically populated with available devices) | Select the GPU acceleration mode. **Auto** selects the GPU with most free memory (minimum 2 GB threshold). **CPU Only** forces all processing to CPU. **GPU N** uses a specific NVIDIA GPU by index. Falls back to CPU if selected GPU is unavailable. Application must be restarted for GPU changes to take effect. |
| 7 | `language` | Language | Drop-down (combo box) | `"auto"` | Auto-detect plus ~90 languages (ISO 639-1 codes including English, Chinese, German, Spanish, Russian, Korean, French, Japanese, Portuguese, and many others) | Selects the input language for transcription. "Auto-detect" enables automatic language detection for multilingual input. Specific language codes improve accuracy when the input language is known. |
| 8 | `append_transcription_text` | Append Transcription Text | Check box | `true` | true / false | When enabled, new transcription results are appended to existing text in the TextWindow. When disabled (overwrite mode), the TextWindow is cleared at the start of each new recording session. |
| 9 | `continuous_dictation` | Continuous dictation (silence-triggered loop) | Check box | `true` | true / false | When enabled, silence triggers transcription and recording automatically restarts for the next segment. When disabled, recording runs until max_duration expires or you click the mic icon to stop. |
| 10 | — | Reset Window Position | Button labeled "Reset Window Position" | — | — | Resets the main window position to screen center (100, 100) on next launch. Stored internally as `window_position`. |
| 11 | — | D-Bus Hotkey Command | Read-only selectable text box + Copy button | `dbus-send --session --type=method_call --dest=org.xvoice.Controller /org/xvoice/App org.xvoice.Actions.Toggle` | — | Displays the D-Bus command for global hotkey activation. Not stored in config; shown for user reference only. Clicking "Copy" copies to clipboard (button briefly shows "Copied!"). Text is also selectable for manual copying. |

### 3.2 Fixed (Non-Configurable) Settings

| Setting | Value | Rationale |
|---------|-------|-----------|
| Audio format | 16000 Hz, 1 channel, 16-bit PCM | Required for Whisper model compatibility |
| Transcription watchdog timeout | max_duration × 1.5, clamped to [30, 120] seconds | Scales with recording length; prevents indefinite hangs |
| Retry count | 3 (with progressive backoff) | Retries transient whisper.cpp errors only |
| Minimum segment duration | 5000 ms (internal) | Scanner ignores audio segments shorter than this threshold |

### 3.3 Additional Configuration Parameters (Internal)

| Parameter | Description |
|-----------|-------------|
| `window_position` | JSON object `{"x": <int>, "y": <int>}` storing the last window position. Auto-saved on move; reset via "Reset Window Position" button in the dialog. |
| `audio_device_display_name` | Human-readable display name for the configured audio device, stored alongside `audio_device` for reference and display purposes. Not used for device selection. |
| `scanner_min_segment_ms` | Minimum segment duration (ms) before transcribing. The scanner will not send segments shorter than this to Whisper. Range: 1000-30000 ms, default: 5000. Not exposed in the Configuration Dialog. |

---

## 4. Non-Functional Requirements

### 4.1 Performance

| Metric | Target |
|--------|--------|
| Recording start latency (click to first sample) | < 200 ms |
| Transcription display latency (recording end to text update) | < 5 seconds per segment |
| Memory footprint during normal operation | < 100 MB RSS |
| CPU usage when idle | < 10% |

### 4.2 Security and Privacy

- Temporary audio files are deleted immediately after transcription completes (success or failure) — no `.wav` files persist on disk post-transcription
- Configuration file has restricted permissions (`600`)
- Audio data is never transmitted over the network; all processing occurs locally
- Memory buffers containing raw PCM audio data are securely overwritten before being freed

### 4.3 Compatibility

- Compatible with GTK3 on both X11 and Wayland display servers
- D-Bus hotkey interface works identically under X11 and Wayland
- Supports NVIDIA GPU (CUDA) acceleration when whisper.cpp was compiled with CUDA support, with automatic CPU fallback

---

## 5. Error Behaviors

### 5.1 Model Unavailable

If the Whisper model file is not found or inaccessible:
- Connection indicator turns red
- Attempting to start recording displays a modal error dialog and aborts — no recording begins, icon stays red

### 5.2 Audio Device Unavailable

If the configured audio device is not currently available:
- Modal error dialog informs user that the microphone is not available
- Recording flow is aborted; application remains in IDLE state

### 5.3 Model Load Failure

If the model file fails to load (corrupt file, insufficient GPU memory):
- Error dialog displayed with specific failure reason
- Connection indicator turns red
- Application remains in IDLE until user provides a valid model path

### 5.4 Transcription Failure

On transcription error:
- Retryable errors (file read, decode, memory) trigger up to 3 retries with progressive backoff
- Non-retryable errors (model not found, invalid parameters) fail immediately
- After exhausting retries, an error message is displayed in the TextWindow and application returns to IDLE

### 5.5 Transcription Timeout

If transcription exceeds the watchdog timeout:
- In-progress transcription is cancelled
- Error message displayed in TextWindow indicating the timeout value
- Application returns to IDLE

### 5.6 D-Bus Unavailable

If the D-Bus session bus is unavailable at startup:
- Warning logged; application continues without hotkey support
- No fallback IPC mechanism is attempted

---

## 6. Hotkey Integration

The application exposes a D-Bus method for toggling recording, which can be bound to a global keyboard shortcut in the desktop environment or window manager. The application does not register or manage global shortcuts itself — configuration is done externally by the user.

### 6.1 D-Bus Interface

| Property | Value |
|----------|-------|
| Bus Name | `org.xvoice.Controller` |
| Object Path | `/org/xvoice/App` |
| Interface | `org.xvoice.Actions` |
| Method | `Toggle` (no parameters, returns boolean) |

### 6.2 Toggle Command

```bash
dbus-send --session --type=method_call --dest=org.xvoice.Controller /org/xvoice/App org.xvoice.Actions.Toggle
```

This command can be bound to any keyboard shortcut via desktop environment tools such as `sxhkd`, `xbindkeys`, Sway, Hyprland, GNOME Keyboard Shortcuts, or KDE System Settings. The toggle behavior is identical to clicking the microphone icon in the main window.

### 6.3 Single-Instance Enforcement

The D-Bus bus name ownership (`org.xvoice.Controller`) enforces single-instance semantics — only one instance of the application can run at a time. If the `dbus-send` command is invoked when no instance is running, the D-Bus daemon returns an error (or auto-launches the application if a D-Bus activation service file is installed).

---

## 7. Appendix — Configuration File Example

```json
{
  "window_position": { "x": 100, "y": 100 },
  "model_path": "~/.cache/whisper/ggml-base.bin",
  "audio_device": "default",
  "max_duration": 60,
  "gpu_mode": "auto",
  "vad_mode": 1,
  "vad_silence_ms": 1000,
  "append_transcription_text": true
}
```
