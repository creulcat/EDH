cmake_minimum_required(VERSION 3.16)
project(Towhitch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SCS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tools/scs_sdk")
set(SCS_SDK_INCLUDE "${SCS_SDK_ROOT}/include")

if(NOT EXISTS "${SCS_SDK_INCLUDE}/scssdk_telemetry.h")
    message(FATAL_ERROR
        "SCS Telemetry SDK not found.\n"
        "Download it from https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry\n"
        "and extract so that this file exists:\n"
        "  ${SCS_SDK_INCLUDE}/scssdk_telemetry.h"
    )
endif()

add_library(Towhitch SHARED src/Towhitch.cpp)

target_include_directories(Towhitch PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${SCS_SDK_INCLUDE}"
    "${SCS_SDK_INCLUDE}/common"
    "${SCS_SDK_INCLUDE}/amtrucks"
    "${SCS_SDK_INCLUDE}/eurotrucks2"
)

if(WIN32)
    target_link_libraries(Towhitch PRIVATE winhttp shell32)
    if(MSVC)
        target_compile_definitions(Towhitch PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_options(Towhitch PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/Towhitch.def")
    else()
        target_link_options(Towhitch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Towhitch.def")
    endif()
else()
    find_package(CURL REQUIRED)
    target_link_libraries(Towhitch PRIVATE CURL::libcurl)

    set_target_properties(Towhitch PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()

# SCS plugins are named Towhitch.dll / Towhitch.so / Towhitch.dylib (no lib prefix).
set_target_properties(Towhitch PROPERTIES PREFIX "")

if(NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

set(TOWHITCH_DIST_DIR "${CMAKE_SOURCE_DIR}/dist")

add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TOWHITCH_DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:Towhitch>" "${TOWHITCH_DIST_DIR}/$<TARGET_FILE_NAME:Towhitch>"
    DEPENDS Towhitch
    COMMENT "Copy Towhitch plugin to dist/"
)

install(TARGETS Towhitch
    RUNTIME DESTINATION "."
    LIBRARY DESTINATION "."
)
