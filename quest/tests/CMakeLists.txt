# Host unit tests for the platform-independent parts of the Quest app (gesture state machine,
# placement maths, highlight geometry). They only need the OpenXR headers, which Gradle has already
# extracted from the openxr_loader_for_android AAR after one ./gradlew assembleDebug.
#
#   cmake -S tests -B build/host-tests && cmake --build build/host-tests && ctest --test-dir build/host-tests --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(questmirror_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT OPENXR_INCLUDE_DIR)
    file(GLOB _openxr_candidates
        "$ENV{HOME}/.gradle/caches/*/transforms/*/transformed/openxr_loader_for_android-*/prefab/modules/headers/include")
    list(SORT _openxr_candidates)
    list(REVERSE _openxr_candidates)
    list(GET _openxr_candidates 0 _openxr_first)
    set(OPENXR_INCLUDE_DIR "${_openxr_first}" CACHE PATH "Directory containing openxr/openxr.h")
endif()
if(NOT EXISTS "${OPENXR_INCLUDE_DIR}/openxr/openxr.h")
    message(FATAL_ERROR "OpenXR headers not found: run ./gradlew assembleDebug once, or pass -DOPENXR_INCLUDE_DIR=...")
endif()
message(STATUS "OpenXR headers: ${OPENXR_INCLUDE_DIR}")

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp)
add_executable(host_tests
    host_tests.cpp
    ${SRC}/grab.cpp
    ${SRC}/hand_gestures.cpp)
target_include_directories(host_tests PRIVATE ${SRC})
target_include_directories(host_tests SYSTEM PRIVATE ${OPENXR_INCLUDE_DIR})
target_compile_options(host_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Werror)

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
