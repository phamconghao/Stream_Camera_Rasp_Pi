cmake_minimum_required(VERSION 3.16)

project(camera_stream)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#
# Warnings
#
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

#
# Find libcamera
#
find_package(PkgConfig REQUIRED)

pkg_check_modules(
    LIBCAMERA
    REQUIRED
    libcamera
)

#
# Include directories
#
include_directories(
    include

    include/capture
    include/frame

    ${LIBCAMERA_INCLUDE_DIRS}
)

#
# Source files
#
set(SOURCES

    src/main.cpp

    src/capture/camera_capture.cpp

    src/frame/frame_pool.cpp
    src/frame/frame_queue.cpp
    src/frame/raw_frame_pool.cpp
    src/frame/raw_frame_queue.cpp
)

#
# Executable
#
add_executable(
    camera_app
    ${SOURCES}
)

#
# Link libraries
#
target_link_libraries(
    camera_app

    pthread

    ${LIBCAMERA_LIBRARIES}
)

#
# Compile flags from pkg-config
#
target_compile_options(
    camera_app
    PRIVATE
    ${LIBCAMERA_CFLAGS_OTHER}
)