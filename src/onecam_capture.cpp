#include "multicam.h"

static std::shared_ptr<Camera> camera;

static void requestComplete(Request *request) {
  // Code to follow
}

int main() {
  std::unique_ptr<CameraManager> cameraManager =
      std::make_unique<CameraManager>();
  int ret = cameraManager->start();
  if (ret) {
    fprintf(stderr, "Failed to start camera manager: %d\n", ret);
    return EXIT_FAILURE;
  }
  printf("Camera Manager Started\n");

  auto cameras = cameraManager->cameras();
  if (cameras.empty()) {
    printf("No cameras were identified on the system.\n");
    cameraManager->stop();
    return EXIT_FAILURE;
  }
  std::string cameraId = cameras[0]->id();
  camera = cameraManager->get(cameraId);
  camera->acquire();
  printf("Camera Acquired: %s\n", cameraId);

  std::unique_ptr<CameraConfiguration> config =
      camera->generateConfiguration({StreamRole::Viewfinder});
  StreamConfiguration &streamConfig = config->at(0);
  printf("Default viewfinder configuration is: %s\n",
         streamConfig.toString().c_str());
  streamConfig.size.width = 640;
  streamConfig.size.height = 480;
  config->validate();
  printf("Validated viewfinder configuration is: %s\n",
         streamConfig.toString().c_str());
  camera->configure(config.get());

  FrameBufferAllocator *allocator = new FrameBufferAllocator(camera);

  for (StreamConfiguration &cfg : *config) {
    int ret = allocator->allocate(cfg.stream());
    if (ret < 0) {
      print("Can't allocate buffers\n");
      return -ENOMEM;
    }

    size_t allocated = allocator->buffers(cfg.stream()).size();
    print("Allocated:%s\n", allocated);
  }

  Stream *stream = streamConfig.stream();
  const std::vector<std::unique_ptr<FrameBuffer>> &buffers =
      allocator->buffers(stream);
  std::vector<std::unique_ptr<Request>> requests;

  for (unsigned int i = 0; i < buffers.size(); ++i) {
    std::unique_ptr<Request> request = camera->createRequest();
    if (!request) {
      printf("Can't create request");
      return -ENOMEM;
    }

    const std::unique_ptr<FrameBuffer> &buffer = buffers[i];
    int ret = request->addBuffer(stream, buffer.get());
    if (ret < 0) {
      printf("Can't set buffer for request");
      return ret;
    }

    requests.push_back(std::move(request));

    if (request->status() == Request::RequestCancelled)
      return;

    const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();
    for (auto bufferPair : buffers) {
      FrameBuffer *buffer = bufferPair.second;
      const FrameMetadata &metadata = buffer->metadata();
      std::cout << " seq: " << std::setw(6) << std::setfill('0')
                << metadata.sequence << " bytesused: ";

      unsigned int nplane = 0;
      for (const FrameMetadata::Plane &plane : metadata.planes()) {
        std::cout << plane.bytesused;
        if (++nplane < metadata.planes().size())
          std::cout << "/";
      }

      std::cout << std::endl;
    }

    request->reuse(Request::ReuseBuffers);
  }

  camera->queueRequest(request);

  camera->requestCompleted.connect(requestComplete);

  camera->start();
  for (std::unique_ptr<Request> &request : requests) {
    camera->queueRequest(request.get());
    std::this_thread::sleep_for(3000ms);
  }

  // Clean
  camera->stop();
  allocator->free(stream);
  delete allocator;
  camera->release();
  camera.reset();
  cameraManager->stop();

  return 0;
}
