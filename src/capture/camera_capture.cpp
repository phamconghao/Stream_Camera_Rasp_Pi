#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <vector>
#include <map>
#include <cstring>
#include <utility>

#include <libcamera/libcamera.h>

#include "camera_capture.h"
#include "raw_frame_pool.h"
#include "raw_frame_queue.h"

/**
 * ============================================================================
 * PIPELINE STAGE 1: Camera -> [THIS FILE] -> Raw Frame Pool/Queue -> Encoder
 * ============================================================================
 *
 * Wraps libcamera to pull YUV420 frames off the CSI camera and hand them
 * to the rest of the pipeline. This file owns no dedicated thread of its
 * own - libcamera drives everything via its internal event loop, which
 * invokes request_complete() (below) on its own thread whenever a frame
 * finishes capturing. That callback is effectively "the producer side"
 * of raw_frame_queue.
 *
 * Key libcamera concepts used here (for anyone unfamiliar with the API):
 *   - CameraManager: global registry of cameras attached to the system.
 *   - Camera: one physical camera (the CSI sensor here).
 *   - CameraConfiguration / StreamConfiguration: negotiates resolution
 *     and pixel format (640x480 YUV420 in this project) with the sensor.
 *   - FrameBufferAllocator: pre-allocates a small ring of DMA buffers
 *     that the camera driver will fill directly (zero-copy from the
 *     sensor's point of view).
 *   - Request: one "please capture a frame into this buffer" ticket.
 *     We keep a handful of Requests permanently in flight and re-queue
 *     each one right after it completes, so the camera is continuously
 *     streaming without gaps.
 *   - FrameBuffer::Plane: each buffer is described by a dma-buf file
 *     descriptor (`fd`) rather than a normal pointer, since the memory
 *     is DMA'd directly from the camera's ISP. We must mmap() it
 *     ourselves to read the bytes, then munmap() when done.
 */

using namespace libcamera;
static std::unique_ptr<CameraManager> g_camera_manager;
static std::shared_ptr<Camera> g_camera;
static std::unique_ptr<CameraConfiguration> g_config;
static std::unique_ptr<FrameBufferAllocator> g_allocator;
static Stream *g_stream;
static std::vector<std::unique_ptr<Request>> g_requests;

// Owned exclusively by this module (see camera_capture.h) - independent
// from every other thread's running flag, following the same convention
// as encoder_thread/rtp_packetizer_thread/udp_sender_thread.
static bool g_capture_running = false;

// Mirrors a plane left-right in place: reverses each row's bytes
// independently (not the whole buffer - that would rotate 180 degrees
// instead, flipping up/down too). Assumes no row stride padding beyond
// width, true for this hardware's Y/U/V planes at 1920x1080 YUV420, per
// the mmap layout confirmed above.
static void hflip_inplace(uint8_t *data, size_t width, size_t height)
{
    for (size_t row = 0; row < height; row++)
    {
        uint8_t *line = data + row * width;
        for (size_t i = 0, j = width - 1; i < j; i++, j--)
        {
            std::swap(line[i], line[j]);
        }
    }
}

static uint64_t g_frame_count = 0;
static bool g_dumped = false; // true after the one-shot debug dump below has fired

// One-time debug helper: writes the very first captured frame to disk
// as raw YUV420 bytes, useful for sanity-checking capture format/size
// with a tool like `ffplay -f rawvideo -pixel_format yuv420p -video_size
// 640x480 frame_000.yuv` without needing the rest of the pipeline.
static void dump_frame(const void *data, size_t size)
{
    FILE *fp = fopen("frame_000.yuv", "wb");

    if (!fp)
    {
        perror("fopen");
        return;
    }

    fwrite(data, 1, size, fp);

    fclose(fp);
    std::cout << "[DUMP] frame_000.yuv (" << size << " bytes)" << std::endl;
}

// libcamera invokes this on ITS OWN internal thread every time a queued
// Request finishes capturing a frame - this is the actual "producer"
// that feeds raw_frame_queue. Must stay fast: no blocking I/O other than
// the required mmap()/munmap() of camera buffers.
static void request_complete(Request *request)
{
    if (!g_capture_running)
    {
        return;
    }

    std::cout << "[CALLBACK] request complete" << std::endl;

    if (request->status() == Request::RequestCancelled)
    {
        return;
    }

    g_frame_count++;

    std::cout << "[PRODUCER] Frame = " << g_frame_count << std::endl;

    const Request::BufferMap &buffers = request->buffers();

    // Usually iterates exactly once (single Viewfinder stream = one buffer
    // per request), but written generically in case more streams are
    // added to the same Request later.
    for (auto const &pair : buffers)
    {
        FrameBuffer *buffer = pair.second;
        raw_frame_t *frame = raw_frame_pool_acquire();
        if (!frame)
        {
            // Pool exhausted: downstream (encoder) is too slow to keep
            // up. Drop this frame rather than block the camera callback.
            std::cout << "[POOL] empty" << std::endl;

            continue;
        }
        // meta_planes carries per-plane info (bytes actually used) as
        // reported by the ISP - stored in a named local (not chained
        // straight off meta.planes()[i]) since Span is returned by value;
        // binding a reference directly into a temporary's element
        // triggers GCC's -Wdangling-reference under -Werror.
        const FrameMetadata &meta = buffer->metadata();
        const auto meta_planes = meta.planes();
        std::cout << "[CAPTURE] byteused = " << meta_planes[0].bytesused << std::endl;
        
        frame->pts_us = meta.timestamp / 1000; // libcamera timestamp is in nanoseconds
        frame->sequence = meta.sequence;
        std::cout << "Plane count = " << buffer->planes().size() << std::endl;
        size_t offset = 0;

        // YUV420's 3 planes (Y, U, V) are usually sub-regions of ONE
        // underlying dma-buf allocation, sharing the same fd with each
        // plane at a different byte offset into it (confirmed on this
        // hardware: all 3 planes report the same fd, with U/V offsets
        // of ~2MB/2.5MB into it) - NOT 3 independent dma-bufs each
        // starting at their own offset 0. mmap()'s offset argument must
        // be page-aligned, and these per-plane offsets are not (e.g.
        // 2073600 isn't a multiple of 4096), so each plane can't be
        // mmap'd individually at plane.offset. Every plane sharing an
        // fd is therefore mmap'd ONCE as a whole (offset 0, spanning
        // the furthest byte any of its planes touches), and each
        // plane's actual bytes are read via pointer arithmetic
        // (base + plane.offset) into that single mapping.
        //
        // Getting this wrong doesn't fail loudly: mmap(fd, 0) still
        // succeeds for every plane (it's a valid mapping, just of the
        // wrong region), so U and V both silently ended up reading the
        // start of the Y plane's own data instead of their real chroma
        // bytes - bytes-identical U and V, decoded as a magenta/green
        // split wherever Y's "luma disguised as chroma" pushed Cb/Cr
        // away from neutral. This was the real cause of the colour-cast
        // bug (independent of, and more severe than, the VUI/colour-
        // space signalling gap fixed separately above).
        std::map<int, std::pair<void *, size_t>> fd_mappings; // fd -> (mmap base, mmap length)

        for (size_t i = 0; i < buffer->planes().size(); i++)
        {
            const FrameBuffer::Plane &plane = buffer->planes()[i];
            size_t needed = plane.offset + plane.length;
            auto it = fd_mappings.find(plane.fd.get());
            if (it == fd_mappings.end() || needed > it->second.second)
            {
                fd_mappings[plane.fd.get()] = { nullptr, needed };
            }
        }

        bool mmap_ok = true;
        for (auto &entry : fd_mappings)
        {
            void *memory = mmap(nullptr, entry.second.second, PROT_READ, MAP_SHARED, entry.first, 0);
            if (memory == MAP_FAILED)
            {
                perror("mmap");
                mmap_ok = false;
                break;
            }
            entry.second.first = memory;
        }

        if (!mmap_ok)
        {
            for (auto &entry : fd_mappings)
            {
                if (entry.second.first)
                {
                    munmap(entry.second.first, entry.second.second);
                }
            }
            raw_frame_pool_release(frame);
            frame = nullptr;
        }
        else
        {
            for (size_t i = 0; i < buffer->planes().size(); i++)
            {
                const FrameBuffer::Plane &plane = buffer->planes()[i];
                const FrameMetadata::Plane &meta_plane = meta_planes[i];
                const uint8_t *base = static_cast<const uint8_t *>(fd_mappings[plane.fd.get()].first);

                memcpy(frame->data + offset, base + plane.offset, meta_plane.bytesused);

                // Plane 0 is full-res Y (1920x1080); planes 1/2 are the
                // 4:2:0 subsampled U/V chroma planes (960x540) - fixed
                // sizes matching the hardcoded 1920x1080 YUV420 capture
                // resolution set in camera_capture_init() above.
                size_t plane_width = (i == 0) ? 1920 : 960;
                size_t plane_height = (i == 0) ? 1080 : 540;
                hflip_inplace(frame->data + offset, plane_width, plane_height);

                offset += meta_plane.bytesused;
            }

            for (auto &entry : fd_mappings)
            {
                munmap(entry.second.first, entry.second.second);
            }
        }

        if (!frame)
        {
            std::cout << "[POOL] empty" << std::endl;

            continue;
        }
        frame->size = offset;

        if (!g_dumped)
        {
            dump_frame(frame->data, frame->size);
            g_dumped = true;
        }

        std::cout << "[PRODUCER] size = " << frame->size << " seq = " << frame->sequence << std::endl;

        // Hand the frame off to encoder_thread (zero-copy: only the
        // pointer crosses the thread boundary). If encoder_thread can't
        // keep up and the queue is full, drop this frame.
        if (raw_frame_queue_push(frame) < 0)
        {
            std::cout << "[QUEUE] full" << std::endl;

            raw_frame_pool_release(frame);
        }
    }

    // Re-arm this Request immediately so the camera keeps streaming
    // continuously instead of stalling after every single frame.
    request->reuse(Request::ReuseBuffers);

    if (g_capture_running)
    {
        g_camera->queueRequest(request);
    }
}

// One-time setup: find and configure the camera, allocate buffers, and
// prepare a small pool of Requests. Must be called once before
// camera_capture_start(). Does NOT start streaming yet.
int camera_capture_init(void)
{
    std::cout << "[CAPTURE] Init CameraManager" << std::endl;

    // Create CameraManager
    g_camera_manager = std::make_unique<CameraManager>();

    // Start libcamera
    int ret = g_camera_manager->start();
    if (ret)
    {
        std::cerr << "[CAPTURE] CameraManager start failed" << std::endl;
        return -1;
    }

    // Check camera count
    if (g_camera_manager->cameras().empty())
    {
        std::cerr << "[CAPTURE] No camera found" << std::endl;
        return -1;
    }

    // Select first camera
    g_camera = g_camera_manager->cameras()[0];

    std::cout << "[CAPTURE] Camera found: " << g_camera->id() << std::endl;

    // Acquire camera
    ret = g_camera->acquire();
    if (ret)
    {
        std::cerr << "[CAPTURE] acquire failed" << std::endl;
        return -1;
    }

    std::cout << "[CAPTURE] acquire OK" << std::endl;

    // Generate config
    g_config = g_camera->generateConfiguration({ StreamRole::Viewfinder });

    if (!g_config)
    {
        std::cerr << "[CAPTURE] generateConfiguration failed" << std::endl;

        return -1;
    }

    // Set resolution - fixed 1920x1080 (FHD) YUV420 for now. Must match
    // MAX_RAW_FRAME_SIZE in raw_frame.h and the resolution assumed by
    // bcm2835_encoder. Not yet configurable at runtime.
    StreamConfiguration &cfg = g_config->at(0);
    cfg.size.width = 1920;
    cfg.size.height = 1080;
    cfg.pixelFormat = formats::YUV420;

    // Not left unset: without an explicit request here, libcamera
    // defaults StreamRole::Viewfinder to ColorSpace::Sycc (sRGB
    // primaries, Rec601 matrix, FULL range 0-255 - confirmed via
    // ColorSpace::toString() logging on this hardware). The BCM2835
    // hardware H.264 encoder (bcm2835_encoder.cpp) does not embed any
    // colour_description into the H.264 SPS it produces regardless of
    // what's requested on its V4L2 OUTPUT/CAPTURE queues (confirmed
    // empirically: ffmpeg's trace_headers bitstream filter shows
    // video_signal_type_present_flag=0 in every SPS this encoder
    // emits, even after setting V4L2_COLORSPACE_JPEG on both queues -
    // that driver simply doesn't support signalling it). With no VUI
    // colour info at all, browsers/WebRTC decoders fall back to
    // guessing - the standard guess for HD (>=720 lines) content is
    // limited-range BT.709, per H.264/H.273's own informative default
    // and every mainstream decoder's convention (ffmpeg, Chrome's
    // media pipeline, etc). Requesting Rec709 here (limited-range
    // BT.709, matching libcamera's own named preset) instead of the
    // full-range-Rec601 default makes the actual pixel bytes equal
    // what those decoders assume in the absence of VUI, which is the
    // only lever available given the encoder can't be made to signal
    // the truth explicitly. This was the root cause of the reported
    // blue/green colour-cast bug: full-range source samples decoded
    // as if limited-range (previously would have also mismatched on
    // matrix at this resolution).
    cfg.colorSpace = ColorSpace::Rec709;

    // NOTE: setting g_config->orientation (Rotate180/Rotate180Mirror/etc)
    // was tried here and had zero effect on this hardware - this sensor's
    // pipeline handler doesn't honour libcamera's orientation/Transform
    // request, so validate() silently keeps it at Identity. The
    // left-right mirror fix this camera's mounting needs is instead done
    // in software in request_complete() below (hflip_inplace), by
    // reversing each plane's rows after they're copied out of the DMA
    // buffer.

    // Validate
    CameraConfiguration::Status status = g_config->validate();
    if (status == CameraConfiguration::Invalid)
    {
        std::cerr << "[CAPTURE] Invalid configuration" << std::endl;
        return -1;
    }

    // Configure camera
    if (g_camera->configure(g_config.get()) < 0)
    {
        std::cerr << "[CAPTURE] configure failed" << std::endl;
        return -1;
    }

    // Logged (not just asserted in a comment) because libcamera's
    // validate()/configure() are free to silently adjust an unsupported
    // colorSpace request to whatever the pipeline handler actually
    // supports - worth being able to confirm from the logs that the
    // negotiated value still matches what bcm2835_encoder.cpp assumes.
    std::cout << "[CAPTURE] negotiated colorSpace = " << ColorSpace::toString(cfg.colorSpace) << std::endl;

    // Save stream pointer
    g_stream = cfg.stream();

    // Allocate the DMA buffers the driver will capture into (count is
    // decided internally by libcamera based on the pipeline handler,
    // typically a small ring like 4).
    g_allocator = std::make_unique<FrameBufferAllocator>(g_camera);
    if (g_allocator->allocate(g_stream) < 0)
    {
        std::cerr << "[CAPTURE] allocate failed" << std::endl;
        return -1;
    }

    // Register request_complete() as the callback libcamera invokes
    // (on its own thread) whenever a queued Request finishes.
    g_camera->requestCompleted.connect(request_complete);

    // Create one Request per allocated buffer and keep them all queued
    // permanently - each one is re-queued in request_complete() right
    // after it completes, so capture never stalls waiting for a new
    // Request to be created.
    const auto &buffers = g_allocator->buffers(g_stream);
    for (auto &buffer : buffers)
    {
        std::unique_ptr<Request> request = g_camera->createRequest();
        if (!request)
        {
            continue;
        }

        if (request->addBuffer(g_stream, buffer.get()) < 0)
        {
            continue;
        }

        g_requests.push_back(std::move(request));
    }

    return 0;
}

// Begin streaming: starts the camera and queues every prepared Request.
// From this point, request_complete() will start firing on libcamera's
// internal thread as frames arrive.
int camera_capture_start(void)
{
    std::cout << "[CAPTURE] start" << std::endl;
    g_capture_running = true;
    
    if (g_camera->start() < 0)
    {
        std::cerr << "[CAPTURE] start failed" << std::endl;
        return -1;
    }

    for (auto &request : g_requests)
    {
        g_camera->queueRequest(request.get());
    }

    std::cout << "[CAPTURE] Streaming..." << std::endl;

    return 0;
}

// Stop streaming. After this returns, request_complete() will no longer
// push new frames into raw_frame_queue, so it is safe for the caller to
// then wake up and join the encoder thread (see main.cpp's shutdown
// sequence: camera_capture_stop() -> encoder_thread_stop() -> ...).
void camera_capture_stop(void)
{
    std::cout << "[CAPTURE] stop" << std::endl;
    std::cout << "[STOP] begin" << std::endl;
    g_capture_running = false;
    g_camera->stop();

    // Camera::stop() synchronously cancels every in-flight Request, but
    // does not reset them to a re-queueable state. request_complete()
    // never gets to call request->reuse() for these, since it bails out
    // immediately when g_capture_running is false (set above, before
    // stop()). Without this loop, the next camera_capture_start() would
    // queueRequest() these still-cancelled Requests directly, which
    // libcamera rejects as invalid ("Request is not valid") for every
    // single one - silently breaking capture until the process restarts.
    for (auto &request : g_requests)
    {
        request->reuse(Request::ReuseBuffers);
    }

    std::cout << "[STOP] end" << std::endl;
}

// Full teardown of libcamera resources. Currently NOT called from
// main.cpp (the line is commented out there) - the process just exits
// after camera_capture_stop(), which is fine for a one-shot CLI run but
// would leak/hold camera resources if the app were ever restarted
// in-process without exiting.
void camera_capture_cleanup(void)
{
    std::cout << "[CAPTURE] cleanup" << std::endl;
    std::cout << "[CLEANUP] begin" << std::endl;
    g_camera->requestCompleted.disconnect();

    if (g_requests.size() > 0)
    {
        // Clear requests
        g_requests.clear();
    }

    if (g_allocator)
    {
        // Release camera allocator
        g_allocator.reset();
    }

    if (g_config)
    {
        // Release camera config
        g_config.reset();
    }

    if (g_camera)
    {
        // Release camera ownership
        g_camera->release();
        g_camera.reset();
    }

    if (g_camera_manager)
    {
        // Stop libcamera
        g_camera_manager->stop();
        g_camera_manager.reset();
    }

    std::cout << "[CLEANUP] end" << std::endl;
}
