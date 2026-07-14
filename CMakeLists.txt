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
    include/encoder
    include/frame
    include/writer
    include/parser
    include/rtp
    include/common
    include/network

    ${LIBCAMERA_INCLUDE_DIRS}
)

#
# Source files
#
set(SOURCES

    src/main.cpp

    src/capture/camera_capture.cpp
    
    src/encoder/encoder_thread.cpp
    src/encoder/bcm2835_encoder.cpp

    src/frame/frame_pool.cpp
    src/frame/frame_queue.cpp
    src/frame/raw_frame_pool.cpp
    src/frame/raw_frame_queue.cpp
    src/frame/encoded_frame_pool.cpp
    src/frame/encoded_frame_queue.cpp

    src/writer/h264_writer.cpp

    src/parser/h264_nal_parser.cpp

    src/rtp/rtp_packetizer.cpp
    src/rtp/rtp_packetizer_thread.cpp
    src/rtp/rtp_packet_pool.cpp
    src/rtp/rtp_packet_queue.cpp

    src/network/udp_sender.cpp
    src/network/udp_sender_thread.cpp

    src/common/app_state.cpp
    src/common/log.cpp
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