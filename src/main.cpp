#include <iostream>

#include "camera_capture.h"

int main()
{
    if (camera_capture_init() < 0)
    {
        return -1;
    }

    camera_capture_start();
    std::cout << "Press Enter to exit..." << std::endl;
    std::cin.get();
    camera_capture_stop();
    camera_capture_cleanup();

    return 0;
}