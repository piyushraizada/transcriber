# Transcriber

Transcriber is a lightweight, offline voice-to-text application for Linux desktops. It provides a system tray icon with a single-click microphone toggle that captures audio from your configured microphone, transcribes it locally using OpenAI's Whisper model via [whisper.cpp](https://github.com/ggml-org/whisper.cpp), and displays the result in a persistent text area. Transcribed text can be edited in-place and copied to the system clipboard for use in other applications. The application runs entirely offline with no external API dependencies, supports optional GPU (CUDA) acceleration, and enforces single-instance operation via D-Bus.

## Prerequisites

The following packages are required to build Transcriber from source. Install them using your distribution's package manager.

### Debian / Ubuntu

```bash
sudo apt-get install build-essential cmake pkg-config \
    libgtk-3-dev libasound2-dev libdbus-1-dev \
    libcjson-dev libayatana-appindicator3-dev \
    git
```

### Fedora / RHEL

```bash
sudo dnf install gcc gcc-c++ cmake pkgconf-pkg-config \
    gtk3-devel alsa-lib-devel dbus-devel \
    cjson-devel libayatana-appindicator3-devel \
    git
```

### Arch Linux

```bash
sudo pacman -S base-devel cmake pkgconf \
    gtk3 alsa-lib dbus cjson libayatana-appindicator \
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

- Download and compile [whisper.cpp](https://github.com/ggml-org/whisper.cpp) via CMake FetchContent
- Download the default Whisper model (`large-v3-turbo-q8_0`, ~1.1 GiB) to `~/.cache/whisper/`
- Detect CUDA (if available) and enable GPU acceleration automatically

To skip the model download during build:

```bash
cmake -DDOWNLOAD_DEFAULT_MODEL=OFF ..
```

### 3. Install (optional)

```bash
sudo make install
```

This installs the `transcriber` binary to `/usr/local/bin` and the `.desktop` file to `/usr/local/share/applications`.

## Running

Run directly from the build directory:

```bash
./transcriber
```

Or after installation:

```bash
transcriber
```

The application will appear as a microphone icon in your system tray. Click the icon to start recording, click again to stop and transcribe.

## Configuration

Configuration is stored in `~/.config/transcriber/config.json`. You can adjust settings such as:

- **Model path** — path to a GGML Whisper model file (default: `~/.cache/whisper/ggml-large-v3-turbo-q8_0.bin`)
- **Audio device** — ALSA capture device (default: system default)
- **Max session duration** — maximum recording time in seconds (default: 30, range: 5–30)

A configuration dialog is available from the system tray context menu.

## License

MIT License — see [LICENSE](LICENSE) for details.
