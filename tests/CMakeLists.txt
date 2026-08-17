# Each test is its own binary so a crash in one doesn't hide the others.
# GUILESS_MAIN in the sources keeps them runnable without a display.
foreach(test_name test_project test_filters test_commands test_snap test_fft)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE zrecord_core Qt6::Test)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
