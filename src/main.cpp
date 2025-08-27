#include "multicam.h"

static std::shared_ptr<Camera> camera;

int main() {
  std::cout << "Camera Detection Start..." << std::endl;

  std::unique_ptr<CameraManager> cm = std::make_unique<CameraManager>();
  int ret = cm->start();
  if (ret) {
    std::cerr << "Failed to start camera manager: " << ret << std::endl;
    return EXIT_FAILURE;
  }

  // List available cameras
  auto cameras = cm->cameras();
  if (cameras.empty()) {
    std::cout << "No cameras found!" << std::endl;
  } else {
    std::cout << "Found " << cameras.size() << " camera(s):" << std::endl;
    for (size_t i = 0; i < cameras.size(); ++i) {
      std::cout << "Camera " << i << ": " << cameras[i]->id() << std::endl;
    }
  }

  cm->stop();

  return 0;
}
