#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <vector>

#include <libcamera/libcamera.h>

#include "camera_capture.h"

using namespace libcamera;
static std::unique_ptr<CameraManager> g_camera_manager;
static std::shared_ptr<Camera> g_camera;
static std::unique_ptr<CameraConfiguration> g_config;
static std::unique_ptr<FrameBufferAllocator> g_allocator;
static Stream *g_stream;
static std::vector<std::unique_ptr<Request>> g_requests;
static bool g_running = false;
static uint64_t g_frame_count = 0;
// static std::mutex g_camera_mutex;

static void request_complete(Request *request)
{
    if (!g_running)
    {
        return;
    }

    std::cout << "[CALLBACK] request complete" << std::endl;

    if (request->status() == Request::RequestCancelled)
    {
        return;
    }

    g_frame_count++;

    std::cout << "[CAPTURE] Frame #" << g_frame_count << std::endl;

    const Request::BufferMap &buffers = request->buffers();

    for (auto const &pair : buffers)
    {
        FrameBuffer *buffer = pair.second;
        const FrameMetadata &meta = buffer->metadata();
        std::cout << "[CAPTURE] byteused = " << meta.planes()[0].bytesused << std::endl;
    }

    // Requeue requests
    request->reuse(Request::ReuseBuffers);

    if (g_running)
    {
        g_camera->queueRequest(request);
    }
}

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

    // Set resolution
    StreamConfiguration &cfg = g_config->at(0);
    cfg.size.width = 640;
    cfg.size.height = 480;
    cfg.pixelFormat = formats::YUV420;

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

    // Save stream pointer
    g_stream = cfg.stream();

    // Allocator buffers
    g_allocator = std::make_unique<FrameBufferAllocator>(g_camera);
    if (g_allocator->allocate(g_stream) < 0)
    {
        std::cerr << "[CAPTURE] allocate failed" << std::endl;
        return -1;
    }

    // Connect callback
    g_camera->requestCompleted.connect(request_complete);

    // Create requests
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

int camera_capture_start(void)
{
    std::cout << "[CAPTURE] start" << std::endl;
    g_running = true;
    
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

void camera_capture_stop(void)
{
    std::cout << "[CAPTURE] stop" << std::endl;
    std::cout << "[STOP] begin" << std::endl;
    g_running = false;
    g_camera->stop();
    std::cout << "[STOP] end" << std::endl;
}

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