cmake_minimum_required(VERSION 3.5.0)
project(scrcpy)

set(CMAKE_CXX_STANDARD 17)

add_library(scrcpy SHARED napi_init.cpp scrcpy_client.cpp)
target_link_libraries(scrcpy PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libhilog_ndk.z.so
    libnative_window.so
    libnative_media_core.so
    libnative_media_codecbase.so
    libnative_media_vdec.so)
