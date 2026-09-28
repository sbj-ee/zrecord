cmake_minimum_required(VERSION 3.16)
project(zrecord LANGUAGES CXX VERSION 1.1.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Version string. Untagged builds get a development suffix so a .deb or an
# About box can't be mistaken for the release: 1.1.1+g<short sha>.
option(ZRECORD_RELEASE "Release build: plain version, no development suffix" OFF)
set(ZRECORD_VERSION "${PROJECT_VERSION}")
if(NOT ZRECORD_RELEASE)
    find_package(Git QUIET)
    set(zrecord_on_tag FALSE)
    set(zrecord_sha "")
    if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match --match "v${PROJECT_VERSION}" HEAD
                        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                        RESULT_VARIABLE zrecord_tag_rc OUTPUT_QUIET ERROR_QUIET)
        if(zrecord_tag_rc EQUAL 0)
            set(zrecord_on_tag TRUE)
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                        OUTPUT_VARIABLE zrecord_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        # Re-run this when HEAD moves, so the suffix follows new commits.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
        execute_process(COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
                        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                        OUTPUT_VARIABLE zrecord_head_ref OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(zrecord_head_ref AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/${zrecord_head_ref}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/.git/${zrecord_head_ref}")
        endif()
    endif()
    if(NOT zrecord_on_tag)
        if(zrecord_sha)
            set(ZRECORD_VERSION "${PROJECT_VERSION}+g${zrecord_sha}")
        else()
            set(ZRECORD_VERSION "${PROJECT_VERSION}+dev")
        endif()
    endif()
endif()
message(STATUS "zrecord version: ${ZRECORD_VERSION}")
configure_file(cmake/zrecord_version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/zrecord_version.h" @ONLY)

find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
# Resampling imported audio to the project rate (Debian: libsamplerate0-dev).
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)

# Everything that isn't a widget lives in a library so the tests can link the
# real code rather than a copy of it. The GUI classes (MainWindow, TrackPanel)
# stay in the executable -- they need a display, so they're exercised by
# running the app, not by unit tests.
add_library(zrecord_core STATIC
    src/AudioEngine.cpp
    src/Filters.cpp
    src/Capture.cpp
    src/Meter.cpp
    src/Dropouts.cpp
    src/TakeFile.cpp
    src/Effects.cpp
    src/Fft.cpp
    src/AudioFileWriter.cpp
    src/AudioFileReader.cpp
    src/Project.cpp
    src/Snap.cpp
    src/Commands.cpp
    src/ProjectFile.cpp
    src/Recovery.cpp
    src/SampleBuffer.cpp
    src/PlaybackMixer.cpp
    src/Resampler.cpp
    src/Gain.cpp
    src/VoiceChanger.cpp
)

target_include_directories(zrecord_core PUBLIC src)

target_link_libraries(zrecord_core PUBLIC
    Qt6::Widgets
    PkgConfig::PORTAUDIO
    PkgConfig::SNDFILE
    PkgConfig::SAMPLERATE
)

# The widgets live in a library too, so the GUI tests can drive the real
# classes rather than a stand-in. Only main() stays in the executable.
add_library(zrecord_gui STATIC
    src/MainWindow.cpp
    src/TrackPanel.cpp
    src/NormalizeDialog.cpp
    src/VoiceChangerDialog.cpp
    src/PeakMeter.cpp
    src/TrackEffectsDialog.cpp
)

target_link_libraries(zrecord_gui PUBLIC zrecord_core)
target_include_directories(zrecord_gui PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_executable(zrecord src/main.cpp)

target_link_libraries(zrecord PRIVATE zrecord_gui)

enable_testing()
add_subdirectory(tests)

install(TARGETS zrecord RUNTIME DESTINATION bin)
install(FILES packaging/zrecord.desktop DESTINATION share/applications)

# /usr/share/doc/zrecord/copyright in Debian's machine-readable format,
# generated from LICENSE so the two can't drift: the MIT text minus its
# title and copyright line, each line indented, blank lines as " .".
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" zrecord_license_lines)
set(ZRECORD_LICENSE_BODY "")
foreach(line IN LISTS zrecord_license_lines)
    if(line MATCHES "^MIT License$" OR line MATCHES "^Copyright ")
        continue()
    endif()
    if(line STREQUAL "")
        if(ZRECORD_LICENSE_BODY STREQUAL "")
            continue() # no leading paragraph separator
        endif()
        string(APPEND ZRECORD_LICENSE_BODY " .\n")
    else()
        string(APPEND ZRECORD_LICENSE_BODY " ${line}\n")
    endif()
endforeach()
string(REGEX REPLACE "\n$" "" ZRECORD_LICENSE_BODY "${ZRECORD_LICENSE_BODY}")
configure_file(cmake/debian-copyright.in "${CMAKE_CURRENT_BINARY_DIR}/copyright" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/copyright" DESTINATION share/doc/zrecord)

set(CPACK_PACKAGE_NAME "zrecord")
set(CPACK_PACKAGE_VERSION "${ZRECORD_VERSION}")
set(CPACK_DEBIAN_PACKAGE_VERSION "${ZRECORD_VERSION}")
set(CPACK_PACKAGE_CONTACT "Stephen B. Johnson <stevebj.ee@gmail.com>")
set(CPACK_PACKAGE_VENDOR "Stephen B. Johnson")
# MIT. A .deb has no license control field; the license travels in
# /usr/share/doc/zrecord/copyright (installed above).
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Qt6 multi-track audio recorder/editor with live filters and voice effects")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Stephen B. Johnson <stevebj.ee@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS YES)
# The mic volume slider drives PipeWire/PulseAudio through pactl; without it
# the slider does nothing (recording itself is unaffected).
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pulseaudio-utils")
configure_file(cmake/CPackProjectConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/CPackProjectConfig.cmake" @ONLY)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/CPackProjectConfig.cmake")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_GENERATOR "DEB")

include(CPack)
