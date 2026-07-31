cmake_minimum_required(VERSION 3.16)
project(transcriber VERSION 2.0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# Find Packages
# GTK3 (includes GDK, Pango, Cairo, ATK, GDK-Pixbuf, GLib, GObject, GIO)
# Note: GIO (GDBus) is pulled in by gtk+-3.0, so no separate dbus-1 dependency needed
pkg_check_modules(GTK3 REQUIRED gtk+-3.0>=3.20)
pkg_check_modules(ALSA REQUIRED alsa>=1.1.0)

# JSON Library (still needed for config file parsing)
pkg_check_modules(CJSON REQUIRED libcjson>=1.7.14)

# Local Whisper via whisper.cpp (ggml-org/whisper.cpp)
# Uses FetchContent to download and build whisper.cpp as a static library.
# The whisper library provides local, offline speech-to-text transcription
# without requiring any external API or network connection.
# Models are downloaded separately (e.g., ggml-base.bin, ggml-small.bin)
# and placed in ~/.cache/whisper/ or specified via config.
set(FETCHCONTENT_QUIET OFF)
include(FetchContent)
FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG        v1.9.1
    # Pin to specific commit for reproducible builds.
    # Tag v1.9.1 points to this commit as of 2026-07-30.
    # Update this SHA when upgrading whisper.cpp.
    GIT_SHALLOW    ON
)
# Attempt to enable CUDA GPU acceleration if CUDA toolkit is available
# Falls back gracefully to CPU-only if CUDA is not installed
# Use CUDAToolkit (modern CMake 3.9+) instead of deprecated FindCUDA
# CMP0146: CMake 3.27+ changed CUDA language handling. whisper.cpp v1.9.1
# expects the old behavior. Set to OLD to suppress warnings. Review and
# remove this override when upgrading whisper.cpp to a version compatible
# with the new CMP0146 policy.
set(CMAKE_POLICY_DEFAULT_CMP0146 OLD)
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    set(GGML_CUDA ON CACHE BOOL "Enable CUDA GPU acceleration via GGML" FORCE)
    message(STATUS "CUDA found (v${CUDAToolkit_VERSION}) - GPU acceleration enabled for whisper.cpp")
else()
    set(GGML_CUDA OFF CACHE BOOL "CUDA not found - CPU only" FORCE)
    message(STATUS "CUDA not found - whisper.cpp will use CPU only")
endif()
# BLAS/OpenBLAS acceleration for whisper.cpp CPU inference.
# Enabled by default for better CPU transcription performance.
# Disable if you encounter build issues or prefer smaller binary size.
option(ENABLE_BLAS "Enable BLAS/OpenBLAS acceleration for whisper.cpp" ON)
if(ENABLE_BLAS)
    set(WHISPER_OPENBLAS ON CACHE BOOL "Enable OpenBLAS if available" FORCE)
    set(WHISPER_BLAS ON CACHE BOOL "Enable BLAS if available" FORCE)
else()
    set(WHISPER_OPENBLAS OFF CACHE BOOL "Disable OpenBLAS" FORCE)
    set(WHISPER_BLAS OFF CACHE BOOL "Disable BLAS" FORCE)
endif()
set(WHISPER_SILERO_VAD OFF CACHE BOOL "Disable Silero VAD" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static libs only" FORCE)
FetchContent_MakeAvailable(whisper)

# Download default GGML whisper model (large-v3-turbo-q8_0) to ~/.cache/whisper/
# The application expects the default model at ~/.cache/whisper/ggml-large-v3-turbo-q8_0.bin
# (see DEFAULT_MODEL_DIR and DEFAULT_MODEL_FILE in src/app_config_dialog.c).
# The download script from whisper.cpp handles caching — it skips if the file exists.
option(DOWNLOAD_DEFAULT_MODEL "Download the default Whisper model (large-v3-turbo-q8_0, 1.1 GiB)" OFF)
if(DOWNLOAD_DEFAULT_MODEL AND whisper_SOURCE_DIR)
    set(DEFAULT_MODEL_DIR "$ENV{HOME}/.cache/whisper")
    file(MAKE_DIRECTORY "${DEFAULT_MODEL_DIR}")
    add_custom_target(download-default-model
        COMMAND "${whisper_SOURCE_DIR}/models/download-ggml-model.sh"
            "large-v3-turbo-q8_0"
            "${DEFAULT_MODEL_DIR}"
        COMMENT "Downloading default Whisper model (large-v3-turbo-q8_0, 1.1 GiB) to ${DEFAULT_MODEL_DIR}..."
        USES_TERMINAL
    )
endif()

# System Tray Icon (libappindicator for StatusNotifierItem support)
# Try Ayatana fork first (modern, actively maintained), then fall back to original
# Note: pkg-config name is "ayatana-appindicator3-0.1" not "libayatana-appindicator3-1"
pkg_check_modules(APPINDICATOR ayatana-appindicator3-0.1)
if(NOT APPINDICATOR_FOUND)
    pkg_check_modules(APPINDICATOR libappindicator3-0.1)
endif()
if(NOT APPINDICATOR_FOUND)
    message(FATAL_ERROR "ayatana-appindicator3-0.1 or libappindicator3-0.1 is required for system tray icon support")
endif()

# Source files
set(SOURCES
    src/app_controller.c
    src/app_window.c
    src/app_dbus.c
    src/app_tray.c
    src/main.c
    src/app_audio.c
    src/app_whisper.c
    src/app_whisper_safe.cpp
    src/app_model_info.c
    src/app_gpu.c
    src/app_config.c
    src/app_config_dialog.c
    src/app_clipboard.c
    src/app_vad.c
    src/app_ring_buffer.c
    src/app_silence_scanner.c
)

# Determine the install prefix for assets directory
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr/local" CACHE PATH "Install prefix" FORCE)
endif()

# WebRTC VAD (Voice Activity Detection) — by CPUImage / Google WebRTC project
# Provides real-time voice activity detection for continuous transcription.
# Licensed under BSD-3-Clause. See third_party/webrtc_vad/LICENSE.
set(WEBRTC_VAD_SOURCES
    third_party/webrtc_vad/webrtc_vad.c
    third_party/webrtc_vad/vad_core.c
    third_party/webrtc_vad/vad_filterbank.c
    third_party/webrtc_vad/vad_gmm.c
    third_party/webrtc_vad/vad_sp.c
    third_party/webrtc_vad/signal_processing_library.c
)

# Embed XPM assets as static C data for GdkPixbuf loading
# Each .xpm file is a valid C source defining: static const char *NAME_XPM[] = { ... };
# Symbols: GEAR_XPM, GREENMIC_XPM, REDMIC_XPM

add_executable(transcriber ${SOURCES} ${WEBRTC_VAD_SOURCES})

# 1. ADD INCLUDE DIRECTORIES
target_include_directories(transcriber PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${GTK3_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
    ${APPINDICATOR_INCLUDE_DIRS}
    # whisper.cpp headers via FetchContent
    ${whisper_SOURCE_DIR}/include
    ${whisper_SOURCE_DIR}/examples
    # WebRTC VAD headers
    third_party/webrtc_vad/include
)

# 2. ADD LINK LIBRARIES
target_link_libraries(transcriber PRIVATE
    ${GTK3_LIBRARIES}
    ${ALSA_LIBRARIES}
    ${CJSON_LIBRARIES}
    ${APPINDICATOR_LIBRARIES}
    whisper
    pthread
    m
    X11
)

# 3. DEFINITIONS

# System-wide model directory — configurable by package maintainers via
# -DSYSTEM_MODEL_DIR=/custom/path. Defaults to <prefix>/share/transcriber/models.
if(NOT SYSTEM_MODEL_DIR)
    set(SYSTEM_MODEL_DIR "${CMAKE_INSTALL_PREFIX}/share/transcriber/models")
endif()

target_compile_definitions(transcriber PRIVATE
    XPM_STATICALLY_EMBEDDED
    APP_VERSION="${PROJECT_VERSION}"
    ASSET_DIR_PATH="${CMAKE_INSTALL_PREFIX}/share/transcriber/assets"
    SYSTEM_MODEL_DIR="${SYSTEM_MODEL_DIR}"
)

# 3a. ADD CFLAGS_OTHER (e.g., -isystem flags from pkg-config)
target_compile_options(transcriber PRIVATE
    ${GTK3_CFLAGS_OTHER}
    ${ALSA_CFLAGS_OTHER}
    ${APPINDICATOR_CFLAGS_OTHER}
)

# Add WHISPER_CPP definition so app_whisper.c knows it's using local whisper
target_compile_definitions(transcriber PRIVATE WHISPER_CPP_LOCAL)

# LO-04 fix: Add compiler warning flags for better code quality
# MIN-008 fix: Treat critical warning categories as errors in CI builds
target_compile_options(transcriber PRIVATE -Wall -Wextra -Wpedantic
    -Werror=return-type
    -Werror=format-security
)
# Enable buffer overflow detection in release builds
target_compile_options(transcriber PRIVATE -D_FORTIFY_SOURCE=2)

# CR-01 fix: Separate sanitizer options (ASan and TSan are mutually exclusive)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(ENABLE_ASAN)
    target_compile_options(transcriber PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(transcriber PRIVATE -fsanitize=address)
elseif(ENABLE_TSAN)
    target_compile_options(transcriber PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(transcriber PRIVATE -fsanitize=thread)
endif()

# Link Time Optimization for release builds
option(ENABLE_LTO "Enable Link Time Optimization (LTO)" OFF)
if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
    if(lto_supported)
        set_property(TARGET transcriber PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "LTO enabled for release build optimization")
    else()
        message(WARNING "LTO requested but not supported: ${lto_error}")
    endif()
endif()

if(ALSA_FOUND)
    target_compile_definitions(transcriber PRIVATE HAVE_ALSA)
endif()

# CUDA GPU acceleration support
if(CUDAToolkit_FOUND)
    target_link_libraries(transcriber PRIVATE cudart)
    target_compile_definitions(transcriber PRIVATE HAVE_CUDA)
    message(STATUS "Linking CUDA runtime for GPU acceleration")
endif()

# Install rules
install(TARGETS transcriber RUNTIME DESTINATION bin)
install(FILES transcriber.desktop DESTINATION share/applications)
# Install D-Bus activation service file for GNOME dock integration.
# This enables DBusActivatable=true in the .desktop file, so GNOME Shell
# recognizes that clicking the dock icon activates the existing instance
# (via the org.xvoice.Controller D-Bus name) rather than launching a new one.
install(FILES org.xvoice.Controller.service DESTINATION share/dbus-1/services)
# Install themed icons for application menu integration and runtime fallback
# When XPM_STATICALLY_EMBEDDED is undefined, the application loads these from disk
install(FILES assets/redmic.xpm   DESTINATION share/pixmaps RENAME redmic.xpm)
install(FILES assets/greenmic.xpm DESTINATION share/pixmaps RENAME greenmic.xpm)
install(FILES assets/gear.xpm     DESTINATION share/pixmaps RENAME gear.xpm)
# Install PNG icons in hicolor theme for reliable GNOME dock/dash resolution
install(FILES assets/hicolor/redmic.png  DESTINATION share/icons/hicolor/apps RENAME redmic.png)
install(FILES assets/hicolor/greenmic.png DESTINATION share/icons/hicolor/apps RENAME greenmic.png)
install(FILES assets/hicolor/gear.png    DESTINATION share/icons/hicolor/apps RENAME gear.png)
# Update the icon cache after installing PNG icons so the theme picks them up
find_program(GTK_UPDATE_ICON_CACHE gtk-update-icon-cache)
if(GTK_UPDATE_ICON_CACHE)
    install(CODE "execute_process(COMMAND ${GTK_UPDATE_ICON_CACHE} \${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/ RESULT_VARIABLE RET_VAR)")
endif()

# Custom target to build a .deb package with bundled Whisper turbo model
# Usage: make build-deb  (builds transcriber_<version>_<arch>.deb in project root)
# The deb package includes the binary, desktop file, D-Bus service, icons,
# and the Whisper large-v3-turbo-q8_0 model (~1.1 GiB).
add_custom_target(build-deb
    COMMAND ${CMAKE_COMMAND} -E echo "Building .deb package..."
    COMMAND bash ${CMAKE_SOURCE_DIR}/packaging/build-deb.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Creating .deb package with bundled Whisper turbo model"
    USES_TERMINAL
)

# Custom target to uninstall files installed by CMake 'make install'
# Usage: make uninstall  (removes all files from /usr/local)
add_custom_target(uninstall
    COMMAND bash ${CMAKE_SOURCE_DIR}/packaging/build-deb.sh --uninstall
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Removing files installed by 'make install'"
    USES_TERMINAL
)
