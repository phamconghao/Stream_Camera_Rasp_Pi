#ifndef __CAMERA_CAPTURE_H__
#define __CAMERA_CAPTURE_H__

#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

/**
 * PIPELINE STAGE 1: Camera -> [camera_capture] -> Raw Frame Pool/Queue
 *
 * Public API for the libcamera-based CSI camera capture. Not a
 * dedicated thread itself - libcamera's own internal event loop drives
 * frame capture and invokes a callback (see camera_capture.cpp) that
 * pushes frames into raw_frame_queue for encoder_thread to consume.
 *
 * Call order: camera_capture_init() once, then camera_capture_start()
 * to begin streaming, camera_capture_stop() to stop, and optionally
 * camera_capture_cleanup() to fully release libcamera resources.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One-time setup: finds the camera, configures 640x480 YUV420,
 * allocates DMA buffers, and prepares Requests. Must be called before
 * camera_capture_start(). Returns 0 on success, -1 on failure.
 */
int camera_capture_init(void);

/**
 * Begin streaming. From this point, captured frames start flowing into
 * raw_frame_queue via the internal libcamera callback. Returns 0 on
 * success, -1 on failure.
 */
int camera_capture_start(void);

/**
 * Stop streaming. After this returns, no further frames will be pushed
 * into raw_frame_queue - safe to then stop/join the encoder thread.
 */
void camera_capture_stop(void);

/**
 * Fully release libcamera resources (CameraManager, Camera, allocator,
 * Requests). Not currently called in main.cpp's shutdown path.
 */
void camera_capture_cleanup(void);

#ifdef __cplusplus
}
#endif
#endif // __CAMERA_CAPTURE_H__
