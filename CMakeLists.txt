cmake_minimum_required(VERSION 3.16)
project(transcriber VERSION 1.0.0 LANGUAGES C CXX)

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
    GIT_TAG        v1.8.1
)
# Attempt to enable CUDA GPU acceleration if CUDA toolkit is available
# Falls back gracefully to CPU-only if CUDA is not installed
# Use CUDAToolkit (modern CMake 3.9+) instead of deprecated FindCUDA
set(CMAKE_POLICY_DEFAULT_CMP0146 OLD)  # Suppress CMP0146 warning from whisper.cpp
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    set(GGML_CUDA ON CACHE BOOL "Enable CUDA GPU acceleration via GGML" FORCE)
    message(STATUS "CUDA found (v${CUDAToolkit_VERSION}) - GPU acceleration enabled for whisper.cpp")
else()
    set(GGML_CUDA OFF CACHE BOOL "CUDA not found - CPU only" FORCE)
    message(STATUS "CUDA not found - whisper.cpp will use CPU only")
endif()
set(WHISPER_OPENBLAS OFF CACHE BOOL "Disable OpenBLAS" FORCE)
set(WHISPER_BLAS OFF CACHE BOOL "Disable BLAS" FORCE)
set(WHISPER_SILERO_VAD OFF CACHE BOOL "Disable Silero VAD" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static libs only" FORCE)
FetchContent_MakeAvailable(whisper)

# Download default GGML whisper model (large-v3-turbo-q8_0) to ~/.cache/whisper/
# The application expects the default model at ~/.cache/whisper/ggml-large-v3-turbo-q8_0.bin
# (see DEFAULT_MODEL_DIR and DEFAULT_MODEL_FILE in src/app_config_dialog.c).
# The download script from whisper.cpp handles caching — it skips if the file exists.
option(DOWNLOAD_DEFAULT_MODEL "Download the default Whisper model (large-v3-turbo-q8_0, 1.1 GiB)" ON)
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
    src/app_model_info.c
    src/app_gpu.c
    src/app_config.c
    src/app_config_dialog.c
    src/app_clipboard.c
)

# Embed XPM assets as static C data for GdkPixbuf loading
# Each .xpm file is a valid C source defining: static const char *NAME_XPM[] = { ... };
# Symbols: GEAR_XPM, GREENMIC_XPM, REDMIC_XPM

add_executable(transcriber ${SOURCES})

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
target_compile_definitions(transcriber PRIVATE
    XPM_STATICALLY_EMBEDDED
    APP_VERSION="${PROJECT_VERSION}"
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
    -Werror=implicit
    -Werror=format-security
    -Werror=implicit-function-declaration
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
    install(CODE "execute_process(COMMAND ${GTK_UPDATE_ICON_CACHE} \$ENV{HOME}/.local/share/icons/hicolor/ RESULT_VARIABLE RET_VAR)")
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
