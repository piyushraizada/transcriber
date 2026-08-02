# Transcriber

Transcriber is a lightweight, offline voice-to-text application for Linux desktops. It allows users to capture audio from their microphone and transcribe it into text locally on their own machine, ensuring privacy and removing the need for an internet connection.

### Core Functionality
*   **Continuous VAD-Driven Transcription:** Records audio continuously and automatically segments speech at natural silence boundaries using Voice Activity Detection (VAD). Each speech segment is transcribed asynchronously while recording continues, enabling seamless, hands-free transcription.
*   **Voice Capture:** Start and stop recording audio via a microphone icon in the main window or a system tray icon.
*   **Clear Transcription:** Right-click the microphone icon (when idle) to clear all transcribed text from the text window, clipboard, and internal buffer. Also available as "Clear Transcription" in the system tray context menu.
*   **Local Transcription:** Uses OpenAI's Whisper model via [whisper.cpp](https://github.com/ggml-org/whisper.cpp) to perform speech-to-text processing entirely offline.
*   **Text Management:** Transcribed text is displayed in a persistent, editable text area and can be copied to the system clipboard. Text is automatically cleared from the clipboard on application exit to prevent stale data persisting after shutdown.
*   **Global Control:** Supports global hotkeys via D-Bus, allowing users to toggle recording without needing the application window in focus.

### Technical Highlights
*   **Tech Stack:** Written in C using GTK3 for the GUI and ALSA (`libasound`) for audio capture.
*   **Performance:** Supports NVIDIA GPU (CUDA) acceleration for faster transcription, with an automatic fallback to CPU.
*   **Architecture:** Multi-threaded design (Presentation, Audio, and Transcription threads) to ensure a responsive user interface.
*   **User Experience:** Features a real-time volume level bar, sine wave animation during recording, and a model availability status indicator.

## Prerequisites

Transcriber depends on system libraries (hard requirements) and optional acceleration libraries that improve performance. CMake auto-detects all installed libraries and enables them automatically — no manual configuration flags are needed when they are present.

### Hard Requirements

These packages **must** be installed before building. The build will fail without them.

| Package | Purpose |
|---------|---------|
| **GCC / G++** (≥9) or Clang | C/C++ compiler toolchain |
| **CMake** (≥3.16) | Build system |
| **pkg-config** | Dependency discovery |
| **GTK3** (≥3.20) | UI framework (includes GLib, GDK, GIO/GDBus) |
| **ALSA** (≥1.1.0) | Audio capture from microphone |
| **cJSON** (≥1.7.14) | JSON configuration file parsing |
| **OpenBLAS** | CPU matrix operation acceleration for whisper.cpp inference |
| **libayatana-appindicator3** or **libappindicator3** | System tray icon support |
| **Git** | Downloads whisper.cpp and RNNoise sources during build |

#### Install Commands

**Debian / Ubuntu:**
```bash
sudo apt-get install build-essential cmake pkg-config \
    libgtk-3-dev libasound2-dev \
    libcjson-dev libopenblas-dev libayatana-appindicator3-dev \
    git
```

**Fedora / RHEL:**
```bash
sudo dnf install gcc gcc-c++ cmake pkgconf-pkg-config \
    gtk3-devel alsa-lib-devel \
    cjson-devel openblas-devel libayatana-appindicator3-devel \
    git
```

**Arch Linux:**
```bash
sudo pacman -S base-devel cmake pkgconf \
    gtk3 alsa-lib cjson openblas libayatana-appindicator \
    git
```

### Optional: Performance Acceleration Libraries

These packages are **not required** for a successful build. CMake detects them and enables acceleration automatically when present. Install any or all of these to improve transcription speed.

| Package | When Needed | Impact | Source |
|---------|-------------|--------|--------|
| **CUDA toolkit** | NVIDIA GPU available — single or multi-GPU | Major: 5–10× faster transcription on GPU vs CPU | [NVIDIA CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit) |
| **cuBLAS** (included with CUDA) | Any CUDA build — used automatically for matrix multiply ops on GPU | Included in CUDA toolkit; no separate install needed | Bundled with CUDA |
| **NCCL** | Multiple NVIDIA GPUs — enables cross-GPU communication for parallel inference | Only useful with 2+ GPUs; ignored on single-GPU systems | [NVIDIA NCCL](https://developer.nvidia.com/nccl) |
| **OpenMP** | Multi-threaded CPU inference | Usually included with GCC/Clang; no extra install needed | Bundled with compiler |

> **Note:** whisper.cpp v1.9.1 uses cuBLAS (not cuDNN) for GPU matrix operations. cuDNN is not supported and will not be used even if installed.

#### Install Commands

**Debian / Ubuntu — CUDA + NCCL:**
```bash
# CUDA toolkit (includes cuBLAS automatically)
sudo apt-get install nvidia-cuda-toolkit

# NCCL for multi-GPU support (optional, only needed with 2+ GPUs)
sudo apt-get install libnccl-dev
```

**Fedora — CUDA + NCCL:**
```bash
sudo dnf install cuda-toolkit nccl-devel
```

**Arch Linux — CUDA + NCCL:**
```bash
sudo pacman -S cuda nccl
```

Without any optional acceleration libraries, CPU inference uses OpenBLAS for accelerated matrix operations (OpenBLAS is a hard requirement). CUDA and NCCL remain optional for GPU users.

### Verifying Detection

After running `cmake`, check the output to confirm which acceleration backends were detected:

```bash
mkdir build && cd build
cmake ..
```

Look for these lines in the CMake output:

- `"CUDA found (v...) - GPU acceleration enabled"` — CUDA/cuBLAS active ✅
- `"Including BLAS backend"` — OpenBLAS/BLAS active ✅
- `"OpenMP ... found"` — Multi-threaded CPU support ✅
- `"NCCL: ...libnccl.so"` — Multi-GPU support ✅

If a library you installed is not detected, verify the `-dev` package was installed (not just the runtime) and re-run `cmake` in a clean build directory.

## Building from Source

### 1. Clone the repository

```bash
git clone https://github.com/piyushraizada/transcriber.git
cd transcriber
```

### 2. Configure and build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The build process will:

- Download and compile [whisper.cpp](https://github.com/ggml-org/whisper.cpp) (pinned to tag `v1.9.1`) via CMake FetchContent
- Detect CUDA (if available) and enable GPU acceleration automatically

#### Download the Whisper model

The default Whisper model (`large-v3-turbo-q8_0`, ~1.1 GiB) is **not** downloaded automatically during build. The simplest way to obtain it is the helper script, which downloads to `models/` by default:

```bash
./packaging/download-model.sh
```

Alternatively, you can enable the CMake download target (which places the model in `~/.cache/whisper/`) by configuring with `-DDOWNLOAD_DEFAULT_MODEL=ON`:

```bash
cmake -DDOWNLOAD_DEFAULT_MODEL=ON ..
make download-default-model
```

You can also download any GGML/GGUF Whisper model manually and configure its path in the application settings.

#### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `DOWNLOAD_DEFAULT_MODEL` | `OFF` | Create the `download-default-model` target (downloads model to `~/.cache/whisper/`) |
| `ENABLE_ASAN` | `OFF` | Enable AddressSanitizer for memory error detection |
| `ENABLE_TSAN` | `OFF` | Enable ThreadSanitizer for data race detection |
| `ENABLE_LTO` | `OFF` | Enable Link Time Optimization for release builds |

> **Note:** `ENABLE_ASAN` and `ENABLE_TSAN` are mutually exclusive.

### 3. Install (optional)

```bash
sudo make install
```

This installs:
- `transcriber` binary to `/usr/local/bin`
- Desktop entry to `/usr/local/share/applications/transcriber.desktop`
- D-Bus activation service to `/usr/local/share/dbus-1/services/org.xvoice.Controller.service`
- Hicolor PNG icons to `/usr/local/share/icons/hicolor/apps/`
- XPM pixmap icons to `/usr/local/share/pixmaps/`

### Alternative: Build a Debian Package

To build a `.deb` package with the bundled Whisper model:

```bash
./packaging/build-deb.sh
```

Options:
- `--download-model` — Force download of the Whisper model (~1.1 GiB)
- `--cuda` — Enable CUDA GPU acceleration in the build
- `--uninstall` — Remove files installed by `make install`

To download the model separately:

```bash
./packaging/download-model.sh [output_directory]
```

The model is downloaded from Hugging Face (`ggml-org/models`) and stored in `models/` by default.

## Running

Run directly from the build directory:

```bash
./transcriber
```

Or after installation:

```bash
transcriber
```

The application will appear as a microphone icon in your system tray and as a small floating window with a red/green mic drawing.

### Visual Indicators

- **Red mic** — Idle (ready to record)
- **Green mic + sine wave animation** — Actively recording
- **Green mic (static)** — Transcribing audio to text
- **"WAIT" overlay on red mic** — Model loading in background on first use. Clicks are ignored until the model finishes loading.

### Interactions

- **Left-click** the mic icon (main window or system tray) — start/stop recording
- **Right-click** the mic icon (main window, when idle / red) — clear all transcribed text from the text window and clipboard
- **System tray context menu** — right-click for "Toggle Recording", "Clear Transcription" (idle only), "Show Window", and "Quit"

A Voice Activity Detector (VAD) monitors the audio stream in real-time and automatically segments speech at natural silence boundaries, transcribing each segment asynchronously while recording continues. Click the icon again or use the global hotkey to stop recording and trigger the final transcription of any remaining audio.

## Configuration

Configuration is stored in `~/.config/transcriber/config.json`. You can adjust settings such as:

- **Model path** — path to a GGML/GGUF Whisper model file (default: `~/.cache/whisper/ggml-large-v3-turbo-q8_0.bin`)
- **Audio device** — ALSA capture device (default: system default)
- **Max duration** — maximum segment duration in seconds before forcing transcription during continuous speech (default: 30, range: 5–30)
- **Continuous dictation** — enable or disable the silence-triggered recording/transcription loop (default: `true`)
- **VAD mode** — aggressiveness level as an integer from 0 to 3, where 0 is least aggressive (most sensitive) and 3 is most aggressive (most restrictive; default: 1, moderate)
- **Silence threshold** — silence duration in seconds before the scanner segments audio for transcription. The config dialog offers 0.5, 1.0, 1.5, and 2.0 sec (default: 1.0 sec); the raw `config.json` value is clamped to the range 1.0–10.0 sec, so hand-edited values outside the dialog choices are accepted but snapped to the nearest option when the dialog is opened.
- **Scanner min segment** — minimum audio segment length in seconds before sending to Whisper (default: 5 sec, range: 1–30 sec)
- **Append transcription text** — when `true`, new transcriptions are appended to existing text; when `false`, the text window is cleared at the start of each session (default: `true`)
- **Language** — transcription language as `"auto"` (auto-detect) or a 2-letter ISO 639-1 code (e.g., `"en"`, `"fr"`); default: `auto`
- **GPU mode** — `auto`, `cpu`, or `gpu:N` for specific GPU selection
- **Flash attention** — when `true`, enables whisper.cpp flash attention to reduce GPU VRAM usage (no effect in CPU-only mode; default: `true`)
- **Noise suppression** — when `true`, applies RNNoise-based automatic noise reduction to the audio stream during capture for cleaner transcription (default: `true`)

A configuration dialog is available from the system tray context menu ("Show Window" → gear icon) or directly via the gear button in the main window's status bar (bottom-left corner).

## Global Hotkey

Transcriber exposes a D-Bus method for toggling recording, which can be bound to a global hotkey in your desktop environment:

```bash
dbus-send --session --type=method_call --dest=org.xvoice.Controller /org/xvoice/App org.xvoice.Actions.Toggle
```

Configure this command as a custom shortcut in your desktop environment's keyboard settings (e.g., GNOME Settings → Keyboard → Custom Shortcuts). The D-Bus activation service file (`org.xvoice.Controller.service`) also enables the application to autolaunch from the dock when no instance is running.

## Troubleshooting

### Log File

All application logs are written to `/tmp/transcriber.log` on each startup (truncated at launch). Use this file to diagnose crashes, transcription failures, GPU initialization errors, or audio device issues. The log includes timestamps, severity levels, and module-specific tags for easy filtering.

`MESSAGE`, `INFO`, `WARNING`, `ERROR`, and `CRITICAL` messages are always written. Verbose `DEBUG`-level messages are only written when the **Debug logs** option is enabled in Settings (or when `debug_logs` is set to `true` in `~/.config/transcriber/config.json`):

```bash
# View the full log
cat /tmp/transcriber.log

# Filter errors only
grep ERROR /tmp/transcriber.log
```

### Common Issues

- **Model not found** — Ensure a valid GGML/GGUF model file exists at the configured path. Download one using `./packaging/download-model.sh`.
- **GPU out of memory** — Enable flash attention in settings (reduces VRAM usage), or switch GPU mode to `cpu` for CPU-only inference.
- **No audio captured** — Verify the configured ALSA device is available with `arecord -L`. Select a valid device in the settings dialog.

## Third-Party Software

Transcriber bundles the following third-party components:

### WebRTC VAD (Voice Activity Detection)

Voice activity detection is provided by the **WebRTC VAD** library, packaged and maintained by **[CPUImage](https://github.com/cpuimage/webrtc_vad)**. The original implementation is part of Google's WebRTC project. The source code resides in `third_party/webrtc_vad/` and is distributed under the **BSD-3-Clause** license. Full license text is available in [`third_party/webrtc_vad/LICENSE`](third_party/webrtc_vad/LICENSE).

### RNNoise (Noise Suppression)

Automatic noise reduction is provided by the **[RNNoise](https://github.com/xiph/rnnoise)** library from Xiph.org. RNNoise uses a deep neural network to suppress background noise in real-time, improving transcription quality in noisy environments. The library is fetched via CMake FetchContent (pinned to tag `v0.2`) and is distributed under the **BSD-3-Clause** license.

## License

Apache License 2.0 — see [LICENSE](LICENSE) for details.
