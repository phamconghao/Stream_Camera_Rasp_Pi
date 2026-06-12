#ifndef __CAMERA_CAPTURE_H__
#define __CAMERA_CAPTURE_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize camera subsystem
 */
int camera_capture_init(void);

/**
 * Start streaming
 */
int camera_capture_start(void);

/**
 * Stop streaming
 */
void camera_capture_stop(void);

/**
 * Cleanup resources
 */
void camera_capture_cleanup(void);

#ifdef __cplusplus
}
#endif
#endif // __CAMERA_CAPTURE_H__