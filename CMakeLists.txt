cmake_minimum_required(VERSION 3.16)
project(zrecord LANGUAGES CXX VERSION 0.1.0)

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
    src/AudioFileReader.cpp
    src/Project.cpp
    src/Commands.cpp
    src/TrackPanel.cpp
    src/ProjectFile.cpp
)

target_link_libraries(zrecord PRIVATE
    Qt6::Widgets
    PkgConfig::PORTAUDIO
    PkgConfig::SNDFILE
)

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
