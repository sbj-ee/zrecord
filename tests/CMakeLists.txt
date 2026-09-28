# Each test is its own binary so a crash in one doesn't hide the others.
# GUILESS_MAIN in the sources keeps the headless suites runnable without a
# display.
foreach(test_name test_project test_projectfile test_filters test_commands test_snap test_fft test_ringbuffer test_audioengine test_samplebuffer test_playbackmixer test_resampler test_gain test_voicechanger test_capture test_meter test_dropouts test_effects test_takefile test_recovery)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE zrecord_core Qt6::Test)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# The widget test needs a QApplication. Qt's offscreen platform gives it one
# without a display, so it runs in CI exactly as it does locally.
add_executable(test_gui test_gui.cpp)
target_link_libraries(test_gui PRIVATE zrecord_gui Qt6::Test)
add_test(NAME test_gui COMMAND test_gui)
set_tests_properties(test_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# `zrecord --version` prints exactly this build's version, with no display.
string(REGEX REPLACE "([.+])" "\\\\\\1" zrecord_version_regex "${ZRECORD_VERSION}")
add_test(NAME zrecord_version COMMAND zrecord --version)
set_tests_properties(zrecord_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^zrecord ${zrecord_version_regex}\n$"
    ENVIRONMENT "DISPLAY=;WAYLAND_DISPLAY=;QT_QPA_PLATFORM=")

# In sanitizer builds, make UBSan findings fail the test instead of just
# printing (ASan already aborts). Harmless in ordinary builds.
get_property(zrecord_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${zrecord_tests} APPEND PROPERTY ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
# A crashing QtTest binary can wedge in its own crash handler; fail instead
# of hanging the CI job.
set_property(TEST ${zrecord_tests} PROPERTY TIMEOUT 300)
