cmake_minimum_required(VERSION 3.20)
project(stratus LANGUAGES CXX VERSION 0.1.0)

# Zero mandatory third-party dependencies. A clean machine with a C++20 compiler and
# CMake must be able to configure and build this repository with no network access.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# stratus_core: everything that is not a main(). Pure logic lives in headers so
# that the tests can exercise it without linking a socket stack.
# ---------------------------------------------------------------------------
add_library(stratus_core STATIC
    src/net.cpp
    src/httpd.cpp
)
target_include_directories(stratus_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(stratus_core PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(stratus_core PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
foreach(tool worker proxy loadgen autoscaler cost)
    add_executable(stratus-${tool} src/${tool}_main.cpp)
    target_link_libraries(stratus-${tool} PRIVATE stratus_core)
endforeach()

# ---------------------------------------------------------------------------
# Tests: an in-repo assert runner, registered with CTest. No GoogleTest, no Catch2.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(stratus-tests tests/test_main.cpp)
target_link_libraries(stratus-tests PRIVATE stratus_core)
target_include_directories(stratus-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

# One CTest entry per test case, so a failure names the case instead of the binary.
set(STRATUS_TEST_CASES
    mandelbrot_deterministic
    mandelbrot_interior_point_saturates
    mandelbrot_iteration_budget
    mandelbrot_size_scaling
    http_request_line_parsing
    http_query_parameters
    http_missing_query_parameter
    http_bad_query_parameter
    http_response_serialisation
    metrics_counter_exposition
    metrics_gauge_and_labels
    metrics_histogram_buckets
    prom_parse_counter_with_labels
    prom_parse_skips_comments
    prom_aggregate_across_targets
    scaling_scales_up_above_target
    scaling_respects_cooldown
    scaling_clamps_to_bounds
    scaling_dead_band_holds
    scaling_scale_down_needs_longer_calm
    cost_per_unit_of_work
    cost_rejects_unpriced_input
    stats_percentiles
    stats_empty_sample
    backend_list_parsing
    utilisation_from_counter_delta
)
foreach(case ${STRATUS_TEST_CASES})
    add_test(NAME ${case} COMMAND stratus-tests ${case})
endforeach()

# ---------------------------------------------------------------------------
# demo target: builds everything, then runs the local end to end demonstration.
# ---------------------------------------------------------------------------
if(WIN32)
    set(STRATUS_DEMO_CMD powershell -NoProfile -ExecutionPolicy Bypass -File
        ${CMAKE_CURRENT_SOURCE_DIR}/demo.ps1)
else()
    set(STRATUS_DEMO_CMD bash ${CMAKE_CURRENT_SOURCE_DIR}/demo.sh)
endif()
add_custom_target(demo
    COMMAND ${STRATUS_DEMO_CMD}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running the Stratus local demonstration")
add_dependencies(demo stratus-worker stratus-proxy stratus-loadgen stratus-autoscaler stratus-cost)
