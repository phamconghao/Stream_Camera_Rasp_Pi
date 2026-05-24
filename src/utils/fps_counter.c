#include <stdio.h>
#include <time.h>

#include "fps_counter.h"

static int frame_count = 0;
static struct timespec start_ts;
static int initialized = 0;

void fps_counter_tick()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!initialized)
    {
        start_ts = now;
        initialized = 1;
    }

    frame_count++;

    double elapsed = (now.tv_sec - start_ts.tv_sec)
                    + (now.tv_nsec - start_ts.tv_nsec) / 1e9;

    if (elapsed >= 1.0)
    {
        double fps = frame_count / elapsed;

        printf("[CAPTURE FPS] %.2f\n", fps);
        frame_count = 0;
        start_ts = now;
    }
}