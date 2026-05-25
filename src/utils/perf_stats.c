#include <stdio.h>
#include <time.h>

#include "perf_stats.h"

static int sent = 0;
static int dropped = 0;
static struct timespec start_ts;
static int initialized = 0;

static double elapsed_sec(struct timespec *a, struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) + (b->tv_nsec - a->tv_nsec) / 1e9;
}

void perf_frame_sent(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!initialized)
    {
        start_ts = now;
        initialized = 1;
    }

    sent++;

    double elapsed = elapsed_sec(&start_ts, &now);

    if (elapsed >= 1.0)
    {
        printf("[PERF] sent = %d dropped = %d\n", sent, dropped);
        sent = 0;
        dropped = 0;
        start_ts = now;
    }
}

void perf_frame_dropped(void)
{
    dropped++;
}