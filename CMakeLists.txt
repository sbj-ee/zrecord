cmake_minimum_required(VERSION 3.16)
project(zrecord LANGUAGES CXX VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

# Everything that isn't a widget lives in a library so the tests can link the
# real code rather than a copy of it. The GUI classes (MainWindow, TrackPanel)
# stay in the executable -- they need a display, so they're exercised by
# running the app, not by unit tests.
add_library(zrecord_core STATIC
    src/AudioEngine.cpp
    src/Filters.cpp
    src/Fft.cpp
    src/AudioFileWriter.cpp
    src/AudioFileReader.cpp
    src/Project.cpp
    src/Snap.cpp
    src/Commands.cpp
    src/ProjectFile.cpp
)

target_include_directories(zrecord_core PUBLIC src)

target_link_libraries(zrecord_core PUBLIC
    Qt6::Widgets
    PkgConfig::PORTAUDIO
    PkgConfig::SNDFILE
)

# The widgets live in a library too, so the GUI tests can drive the real
# classes rather than a stand-in. Only main() stays in the executable.
add_library(zrecord_gui STATIC
    src/MainWindow.cpp
    src/TrackPanel.cpp
)

target_link_libraries(zrecord_gui PUBLIC zrecord_core)

add_executable(zrecord src/main.cpp)

target_link_libraries(zrecord PRIVATE zrecord_gui)

enable_testing()
add_subdirectory(tests)

install(TARGETS zrecord RUNTIME DESTINATION bin)
install(FILES packaging/zrecord.desktop DESTINATION share/applications)

set(CPACK_PACKAGE_NAME "zrecord")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "stevebj.ee@gmail.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Qt6 multi-track audio recorder/editor with live filters and voice effects")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "stevebj.ee@gmail.com")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS YES)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_GENERATOR "DEB")

include(CPack)
