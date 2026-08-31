cmake_minimum_required(VERSION 3.15)

project(SonoBusMoLiXiuBridge LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "The MoLiXiu bridge is Windows-only.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(SonoBusMoLiXiuHook SHARED SonoBusMoLiXiuHook.cpp)
add_executable(SonoBusMoLiXiuBridge SonoBusMoLiXiuBridge.cpp)

foreach(target SonoBusMoLiXiuHook SonoBusMoLiXiuBridge)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${target})
endforeach()

target_link_libraries(SonoBusMoLiXiuHook PRIVATE user32)
