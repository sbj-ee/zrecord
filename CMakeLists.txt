cmake_minimum_required(VERSION 3.16)
project(zrecord LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

add_executable(zrecord
    src/main.cpp
    src/MainWindow.cpp
    src/AudioEngine.cpp
    src/Filters.cpp
    src/AudioFileWriter.cpp
    src/WaveformView.cpp
)

target_link_libraries(zrecord PRIVATE
    Qt6::Widgets
    PkgConfig::PORTAUDIO
    PkgConfig::SNDFILE
)
