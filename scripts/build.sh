#!/bin/bash

set -e

echo "========================================="
echo "Building camera application"
echo "========================================="

mkdir -p build
gcc \
-fsanitize=address \
-g \
-Wall \
-Wextra \
-Iinclude \
src/main.c \
src/capture/v4l2_capture.c \
src/capture/capture_thread.c \
src/network/client_thread.c \
src/queue/frame_queue.c \
src/shared/shared_frame.c \
src/shared/frame_pool.c \
src/stream/stream_thread.c \
src/utils/signal_handler.c \
-o build/camera_app \
-lpthread

echo ""
echo "Build success!"
echo ""

echo "Binary:"
echo "build/camera_app"
