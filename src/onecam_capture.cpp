#include "multicam.h"
#include <chrono>
#include <signal.h>
#include <atomic>

static std::shared_ptr<Camera> camera;
static std::atomic<bool> running(true);
static std::atomic<uint32_t> frameCount(0);
static auto startTime = std::chrono::steady_clock::now();

static void signalHandler(int signal) {
  if (signal == SIGINT) {
    printf("\nReceived interrupt signal, stopping...\n");
    running = false;
  }
}

static void requestComplete(Request *request) {
  if (request->status() == Request::RequestCancelled)
    return;

  frameCount++;
  
  const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();
  for (auto bufferPair : buffers) {
    FrameBuffer *buffer = bufferPair.second;
    const FrameMetadata &metadata = buffer->metadata();
    
    // Print every 10th frame to reduce output
    if (frameCount % 10 == 0) {
      auto currentTime = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count();
      float fps = (frameCount * 1000.0f) / elapsed;
      printf(" seq: %06u | frames: %u | fps: %.1f | bytesused: ", 
             metadata.sequence, frameCount.load(), fps);

      unsigned int nplane = 0;
      for (const FrameMetadata::Plane &plane : metadata.planes()) {
        printf("%u", plane.bytesused);
        if (++nplane < metadata.planes().size())
          printf("/");
      }
      printf("\n");
    }
  }

  // Continue capturing if still running
  if (running) {
    request->reuse(Request::ReuseBuffers);
    camera->queueRequest(request);
  }
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
  printf("Camera Acquired: %s\n", cameraId.c_str());

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
      printf("Can't allocate buffers\n");
      return -ENOMEM;
    }

    size_t allocated = allocator->buffers(cfg.stream()).size();
    printf("Allocated: %zu\n", allocated);
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
  }


  camera->requestCompleted.connect(requestComplete);

  // Setup signal handler for graceful shutdown
  signal(SIGINT, signalHandler);
  
  camera->start();
  printf("Camera started, beginning capture (press Ctrl+C to stop)...\n");
  
  startTime = std::chrono::steady_clock::now();
  
  // Queue all requests initially
  for (std::unique_ptr<Request> &request : requests) {
    camera->queueRequest(request.get());
  }
  
  // Run until interrupted or timeout
  auto captureStart = std::chrono::steady_clock::now();
  while (running) {
    std::this_thread::sleep_for(100ms);
    
    // Auto-stop after 10 seconds if not interrupted
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - captureStart).count() >= 10) {
      running = false;
    }
  }
  
  // Calculate final statistics
  auto endTime = std::chrono::steady_clock::now();
  auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
  float avgFps = (frameCount * 1000.0f) / totalTime;
  
  printf("\nStopping capture...\n");
  printf("Captured %u frames in %.2f seconds (%.1f fps average)\n", 
         frameCount.load(), totalTime / 1000.0f, avgFps);

  // Clean up in correct order
  camera->stop();
  
  // Wait for any pending operations to complete
  std::this_thread::sleep_for(100ms);
  
  allocator->free(stream);
  delete allocator;
  camera->release();
  camera.reset();
  cameraManager->stop();
  
  printf("Cleanup complete.\n");

  return 0;
}
