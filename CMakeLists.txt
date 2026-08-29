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
# Find OpenSSL (Phase 22.2.3: DTLS self-signed cert + fingerprint;
# Phase 22.4 will use it for the actual DTLS handshake)
#
pkg_check_modules(
    OPENSSL
    REQUIRED
    openssl
)

#
# Find libsrtp2 (Phase 22.5: SRTP encrypt/decrypt using the keying
# material Phase 22.4's DTLS handshake exports)
#
pkg_check_modules(
    LIBSRTP2
    QUIET
    libsrtp2
)

if(NOT LIBSRTP2_FOUND)
    message(FATAL_ERROR
        "libsrtp2 development files are required to build camera_app.\n"
        "On Raspberry Pi OS/Debian install them with:\n"
        "  sudo apt update && sudo apt install -y libsrtp2-dev\n"
        "Then rerun ./build.sh."
    )
endif()

#
# Include directories
#
include_directories(
    include

    include/capture
    include/encoder
    include/decoder
    include/frame
    include/writer
    include/parser
    include/rtp
    include/common
    include/network
    include/rtsp
    include/signaling

    ${LIBCAMERA_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
    ${LIBSRTP2_INCLUDE_DIRS}
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

    src/parser/h264_nal_parser.cpp
    src/parser/sps_pps_cache.cpp

    src/rtp/rtp_packetizer.cpp
    src/rtp/rtp_packetizer_thread.cpp
    src/rtp/rtp_packet_pool.cpp
    src/rtp/rtp_packet_queue.cpp
    src/rtp/webrtc_rtp_packet_pool.cpp
    src/rtp/webrtc_rtp_packet_queue.cpp
    src/rtp/rtcp_packet.cpp
    src/rtp/rtcp_sender_thread.cpp

    src/network/udp_sender.cpp
    src/network/udp_sender_thread.cpp
    src/network/udp_receiver.cpp
    src/network/control_listener_thread.cpp

    src/rtsp/rtsp_message.cpp
    src/rtsp/rtsp_session_registry.cpp
    src/rtsp/rtsp_server.cpp
    src/rtsp/pipeline_controller.cpp

    src/signaling/signaling_server.cpp
    src/signaling/webrtc_sdp.cpp
    src/signaling/dtls_cert.cpp
    src/signaling/ice_credentials.cpp
    src/signaling/ice_candidate.cpp
    src/signaling/stun_message.cpp
    src/signaling/ice_agent.cpp
    src/signaling/dtls_handshake.cpp
    src/signaling/srtp_session.cpp
    src/signaling/webrtc_media_registry.cpp
    src/signaling/webrtc_sender_thread.cpp

    src/common/app_state.cpp
    src/common/log.cpp
    src/common/base64.cpp
    src/common/sha1.cpp
    src/common/json_lite.cpp
    src/common/hmac.cpp
    src/common/auth_failure_log.cpp
)

#
# Receiver pipeline sources (RTP receiver + hardware decoder). Separate
# executable from camera_app: no libcamera/capture/encoder dependency,
# just the receive-side network/RTP/decoder/writer stages. Reuses the
# same frame/rtp_packet pool+queue source files as camera_app (they're
# plain data structures, nothing sender-specific), and the two
# executables never link together / never run in the same process.
#
set(RECEIVER_SOURCES

    src/main_receiver.cpp

    src/decoder/decoder_thread.cpp
    src/decoder/bcm2835_decoder.cpp

    src/frame/frame_pool.cpp
    src/frame/frame_queue.cpp
    src/frame/raw_frame_pool.cpp
    src/frame/raw_frame_queue.cpp
    src/frame/encoded_frame_pool.cpp
    src/frame/encoded_frame_queue.cpp

    src/writer/yuv_writer.cpp
    src/writer/circular_h264_writer.cpp

    src/rtp/rtp_depacketizer.cpp
    src/rtp/rtp_depacketizer_thread.cpp
    src/rtp/rtp_jitter_buffer.cpp
    src/rtp/rtp_packet_pool.cpp
    src/rtp/loss_reporter_thread.cpp
    src/rtp/rtcp_packet.cpp
    src/rtp/rtcp_receiver_stats.cpp
    src/rtp/rtcp_receiver_thread.cpp

    src/network/udp_receiver.cpp
    src/network/udp_receiver_thread.cpp
    src/network/udp_sender.cpp
    src/network/control_channel.cpp

    src/common/app_state.cpp
    src/common/log.cpp
    src/common/hmac.cpp
)

#
# Executables
#
add_executable(
    camera_app
    ${SOURCES}
)

add_executable(
    camera_receiver
    ${RECEIVER_SOURCES}
)

#
# Link libraries
#
target_link_libraries(
    camera_app

    pthread

    ${LIBCAMERA_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${LIBSRTP2_LIBRARIES}
)

#
# PHASE 23.2: camera_receiver now needs OpenSSL too (common/hmac.cpp,
# used by control_channel.cpp to sign control-channel messages) -
# previously only camera_app needed it (Phase 22's DTLS/SDP work).
#
target_link_libraries(
    camera_receiver

    pthread
    ${OPENSSL_LIBRARIES}
)

#
# Compile flags from pkg-config
#
target_compile_options(
    camera_app
    PRIVATE
    ${LIBCAMERA_CFLAGS_OTHER}
)
