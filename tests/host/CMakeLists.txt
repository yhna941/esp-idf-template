cmake_minimum_required(VERSION 3.20)
project(host_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59)
FetchContent_MakeAvailable(googletest)
enable_testing()
foreach(component domain application drivers configuration networking)
    add_subdirectory("../../components/${component}" "components/${component}")
    target_compile_options(${component} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
add_executable(policy_tests
    test_timed_output.cpp
    test_boot_health.cpp
    test_workers.cpp
    test_sht3x.cpp
    test_networking.cpp
    test_network_service.cpp
    test_lifecycle.cpp
    test_commands.cpp
    test_output_quiesce.cpp
    test_settings_migration.cpp)
target_compile_options(policy_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(policy_tests PRIVATE GTest::gtest_main domain application drivers configuration networking)
include(GoogleTest)
gtest_discover_tests(policy_tests)
