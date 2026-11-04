cmake_minimum_required(VERSION 3.22)
project(vrscreen LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Khronos OpenXR loader + headers, provided as a prefab package by the
# org.khronos.openxr:openxr_loader_for_android AAR.
find_package(OpenXR REQUIRED CONFIG)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(vrscreen SHARED
    main.cpp
    xr_app.cpp
    grab.cpp
    hand_gestures.cpp
    hand_tracker.cpp
    status_panel.cpp
    bitmap_font.cpp
    stream_client.cpp
    video_decoder.cpp
    mediacodec_source.cpp)

target_compile_definitions(vrscreen PRIVATE XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_OPENGL_ES)
# OpenXR structs are initialised as `T x{XR_TYPE_...}`; the remaining members are zeroed on purpose.
target_compile_options(vrscreen PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Werror=return-type)

# NativeActivity looks up ANativeActivity_onCreate, which only the glue references.
target_link_options(vrscreen PRIVATE -u ANativeActivity_onCreate)

target_link_libraries(vrscreen PRIVATE
    native_app_glue
    OpenXR::openxr_loader
    android
    EGL
    GLESv3
    mediandk
    log)
