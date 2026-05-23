#ifndef __V4L2_CAPTURE_H__
#define __V4L2_CAPTURE_H__

#include <stddef.h>

int capture_init(const char *device);
int capture_start();
int capture_get_frame(void **data, size_t *size, int *index);
void capture_release_frame(int index);
void capture_stop();
void capture_close();

#endif // __V4L2_CAPTURE_H__
