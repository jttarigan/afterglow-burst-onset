add_executable(ag_tests test_main.cpp test_core.cpp test_sim.cpp test_forecast.cpp test_levels.cpp test_workload.cpp
                        test_lab.cpp test_stepbench.cpp)
target_link_libraries(ag_tests PRIVATE ag_lab ag_doctest)
target_include_directories(ag_tests PRIVATE ${CMAKE_SOURCE_DIR}/lab/src)
target_compile_definitions(ag_tests PRIVATE AG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
ag_set_warnings(ag_tests)
add_test(NAME unit COMMAND ag_tests)

# R1: no float in core/.
add_test(NAME core_integer_only COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_core_numeric.py")
# E0 short form on the Mac: determinism, expected hashes, forecast exactness, snapshot round trip.
add_test(NAME selftest COMMAND aglab --mode=selftest --levels=${CMAKE_SOURCE_DIR}/bench/levels)
# Analysis scripts: schema agrees with the C++ writers; validate.py rules; e1_fit recovers known tau.
add_test(NAME analysis
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_SOURCE_DIR}/tools/analysis/tests" -t "${CMAKE_SOURCE_DIR}/tools/analysis"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(analysis PROPERTIES ENVIRONMENT "AGLAB=$<TARGET_FILE:aglab>")
