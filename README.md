# Transcriber

Transcriber is a lightweight, offline voice-to-text application for Linux desktops. It allows users to capture audio from their microphone and transcribe it into text locally on their own machine, ensuring privacy and removing the need for an internet connection.

### Core Functionality
*   **Continuous VAD-Driven Transcription:** Records audio continuously and automatically segments speech at natural silence boundaries using Voice Activity Detection (VAD). Each speech segment is transcribed asynchronously while recording continues, enabling seamless, hands-free transcription.
*   **Voice Capture:** Start and stop recording audio via a microphone icon in the main window or a system tray icon.
*   **Local Transcription:** Uses OpenAI's Whisper model via [whisper.cpp](https://github.com/ggml-org/whisper.cpp) to perform speech-to-text processing entirely offline.
*   **Text Management:** Transcribed text is displayed in a persistent, editable text area and can be copied to the system clipboard.
*   **Global Control:** Supports global hotkeys via D-Bus, allowing users to toggle recording without needing the application window in focus.

### Technical Highlights
*   **Tech Stack:** Written in C using GTK3 for the GUI and ALSA (`libasound`) for audio capture.
*   **Performance:** Supports NVIDIA GPU (CUDA) acceleration for faster transcription, with an automatic fallback to CPU.
*   **Architecture:** Multi-threaded design (Presentation, Audio, and Transcription threads) to ensure a responsive user interface.
*   **User Experience:** Features a real-time volume level bar, sine wave animation during recording, and a model availability status indicator.

## Prerequisites

The following packages are required to build Transcriber from source. Install them using your distribution's package manager.

### Debian / Ubuntu

```bash
sudo apt-get install build-essential cmake pkg-config \
    libgtk-3-dev libasound2-dev \
    libcjson-dev libayatana-appindicator3-dev \
    git
```

### Fedora / RHEL

```bash
sudo dnf install gcc gcc-c++ cmake pkgconf-pkg-config \
    gtk3-devel alsa-lib-devel \
    cjson-devel libayatana-appindicator3-devel \
    git
```

### Arch Linux

```bash
sudo pacman -S base-devel cmake pkgconf \
    gtk3 alsa-lib cjson libayatana-appindicator \
    git
```

### Optional: GPU Acceleration

For NVIDIA GPU acceleration, install the CUDA toolkit:

```bash
# Debian / Ubuntu
sudo apt-get install nvidia-cuda-toolkit

# Fedora
sudo dnf install cuda-toolkit

# Arch Linux
sudo pacman -S cuda
```

If CUDA is not installed, the build will automatically fall back to CPU-only mode.

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

- Download and compile [whisper.cpp](https://github.com/ggml-org/whisper.cpp) (pinned to tag `v1.8.1`) via CMake FetchContent
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
| `ENABLE_BLAS` | `ON` | Enable BLAS/OpenBLAS acceleration for whisper.cpp CPU inference |
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

The application will appear as a microphone icon in your system tray. Click the icon to start continuous recording. A Voice Activity Detector (VAD) monitors the audio stream in real-time and automatically segments speech at natural silence boundaries, transcribing each segment asynchronously while recording continues. Click the icon again or use the global hotkey to stop recording and trigger the final transcription of any remaining audio.

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

A configuration dialog is available from the system tray context menu or the gear icon in the main window's status bar.

## Global Hotkey

Transcriber exposes a D-Bus method for toggling recording, which can be bound to a global hotkey in your desktop environment:

```bash
dbus-send --session --type=method_call --dest=org.xvoice.Controller /org/xvoice/App org.xvoice.Actions.Toggle
```

Configure this command as a custom shortcut in your desktop environment's keyboard settings (e.g., GNOME Settings → Keyboard → Custom Shortcuts). The D-Bus activation service file (`org.xvoice.Controller.service`) also enables the application to autolaunch from the dock when no instance is running.

## Third-Party Software

Transcriber bundles the following third-party components:

### WebRTC VAD (Voice Activity Detection)

Voice activity detection is provided by the **WebRTC VAD** library, packaged and maintained by **[CPUImage](https://github.com/cpuimage/webrtc_vad)**. The original implementation is part of Google's WebRTC project. The source code resides in `third_party/webrtc_vad/` and is distributed under the **BSD-3-Clause** license. Full license text is available in [`third_party/webrtc_vad/LICENSE`](third_party/webrtc_vad/LICENSE).

## License

Apache License 2.0 — see [LICENSE](LICENSE) for details.
