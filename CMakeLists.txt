cmake_minimum_required(VERSION 3.16)
project(transcriber VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# Find Packages
# GTK3 (includes GDK, Pango, Cairo, ATK, GDK-Pixbuf, GLib, GObject, GIO)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0>=3.20)
pkg_check_modules(ALSA REQUIRED alsa>=1.1.0)
pkg_check_modules(CURL REQUIRED libcurl>=7.68.0)
pkg_check_modules(DBUS REQUIRED dbus-1>=1.12)

# JSON Library
pkg_check_modules(CJSON REQUIRED libcjson>=1.7.14)

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
    src/app_config.c
    src/app_config_dialog.c
    src/app_clipboard.c
)

# Embed XPM assets as static C data for GdkPixbuf loading
# gdk_pixbuf_new_from_xpm_data() accepts null-terminated char arrays
set(XPM_SOURCES)
foreach(xpm_file gear.xpm greenmic.xpm redmic.xpm)
    set(xpm_path "${CMAKE_CURRENT_SOURCE_DIR}/assets/${xpm_file}")
    string(REPLACE ".xpm" "" xpm_name "${xpm_file}")
    string(TOUPPER "${xpm_name}" xpm_upper)
    set(out_file "${CMAKE_CURRENT_BINARY_DIR}/assets/${xpm_name}.h")
    file(READ "${xpm_path}" xpm_content)
    # Strip trailing whitespace/newlines from the file content
    string(STRIP "${xpm_content}" xpm_content)
    # Ensure the last line ends with a comma for valid C array syntax
    string(REGEX REPLACE "([^,])$" "\\1," xpm_content "${xpm_content}")
    file(GENERATE
        OUTPUT "${out_file}"
        CONTENT "#ifndef EMBEDDED_${xpm_upper}_H
#define EMBEDDED_${xpm_upper}_H
static const char *${xpm_upper}_XPM[] = {
${xpm_content}
NULL
};
#endif
"
    )
    list(APPEND XPM_SOURCES "${out_file}")
endforeach()

add_executable(transcriber ${SOURCES})

# 1. ADD INCLUDE DIRECTORIES
target_include_directories(transcriber PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${GTK3_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
    ${APPINDICATOR_INCLUDE_DIRS}
)

# 2. ADD LINK LIBRARIES
target_link_libraries(transcriber PRIVATE
    ${GTK3_LIBRARIES}
    ${ALSA_LIBRARIES}
    ${CURL_LIBRARIES}
    ${DBUS_LIBRARIES}
    ${CJSON_LIBRARIES}
    ${APPINDICATOR_LIBRARIES}
    pthread
    m
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
    ${CURL_CFLAGS_OTHER}
    ${DBUS_CFLAGS_OTHER}
    ${CJSON_CFLAGS_OTHER}
    ${APPINDICATOR_CFLAGS_OTHER}
)

# LO-04 fix: Add compiler warning flags for better code quality
target_compile_options(transcriber PRIVATE -Wall -Wextra -Wpedantic)

# LO-06 fix: Optional sanitizer support for debug builds
option(ENABLE_SANITIZERS "Enable AddressSanitizer and ThreadSanitizer" OFF)
if(ENABLE_SANITIZERS)
    target_compile_options(transcriber PRIVATE -fsanitize=address,thread -fno-omit-frame-pointer)
    target_link_options(transcriber PRIVATE -fsanitize=address,thread)
endif()

if(ALSA_FOUND)
    target_compile_definitions(transcriber PRIVATE HAVE_ALSA)
endif()

# Install rules
install(TARGETS transcriber RUNTIME DESTINATION bin)
install(FILES transcriber.desktop DESTINATION share/applications)
# Install themed icon for application menu integration
install(FILES assets/redmic.xpm DESTINATION share/pixmaps RENAME redmic.xpm)

