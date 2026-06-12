#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

#include <libcamera/libcamera.h>

#include "camera_capture.h"

using namespace libcamera;
static std::unique_ptr<CameraManager> g_camera_manager;
static std::shared_ptr<Camera> g_camera;
static bool g_running = false;

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

    return 0;
}

int camera_capture_start(void)
{
    std::cout << "[CAPTURE] start" << std::endl;
    g_running = true;

    return 0;
}

void camera_capture_stop(void)
{
    std::cout << "[CAPTURE] stop" << std::endl;
    g_running = false;
}

void camera_capture_cleanup(void)
{
    std::cout << "[CAPTURE] cleanup" << std::endl;

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
}