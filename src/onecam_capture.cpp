#include "multicam.h"
#include <chrono>
#include <signal.h>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

static std::shared_ptr<Camera> camera;
static std::atomic<bool> running(true);
static std::atomic<uint32_t> frameCount(0);
static auto startTime = std::chrono::steady_clock::now();
static bool saveNextFrame = false;
static std::atomic<bool> frameSaved(false);
static uint32_t imageWidth = 0;
static uint32_t imageHeight = 0;
static std::string pixelFormat = "";

static void signalHandler(int signal) {
  if (signal == SIGINT) {
    printf("\nReceived interrupt signal, stopping...\n");
    running = false;
  }
}

// Simple function to save raw buffer directly
static void saveFrameAsRAW(const FrameBuffer *buffer, const FrameMetadata &metadata) {
  auto captureStart = std::chrono::high_resolution_clock::now();
  
  // Generate timestamp filename with resolution
  auto now = std::chrono::system_clock::now();
  auto time_t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  
  std::stringstream filename;
  filename << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
  filename << "_" << std::setfill('0') << std::setw(3) << ms.count();
  filename << "_" << imageWidth << "x" << imageHeight;
  filename << ".raw";
  
  auto processStart = std::chrono::high_resolution_clock::now();
  
  // Get the first plane
  const FrameBuffer::Plane &plane = buffer->planes()[0];
  
  // Map the buffer memory
  void *mem = mmap(NULL, plane.length, PROT_READ, MAP_SHARED, plane.fd.get(), 0);
  if (mem == MAP_FAILED) {
    printf("Failed to mmap buffer\n");
    return;
  }
  
  // Save raw buffer directly - no conversion needed!
  std::ofstream file(filename.str(), std::ios::binary);
  if (file.is_open()) {
    file.write((char *)mem, plane.length);
    file.close();
    
    auto saveEnd = std::chrono::high_resolution_clock::now();
    
    // Calculate timings
    auto captureToProcess = std::chrono::duration_cast<std::chrono::microseconds>
                           (processStart - captureStart).count();
    auto processToSave = std::chrono::duration_cast<std::chrono::microseconds>
                        (saveEnd - processStart).count();
    auto totalTime = std::chrono::duration_cast<std::chrono::microseconds>
                    (saveEnd - captureStart).count();
    
    printf("\n=== Frame Saved ===\n");
    printf("Filename: %s\n", filename.str().c_str());
    printf("Resolution: %dx%d\n", imageWidth, imageHeight);
    printf("Pixel Format: %s\n", pixelFormat.c_str());
    printf("Buffer Size: %zu bytes\n", plane.length);
    printf("Capture → Processing: %ld µs\n", captureToProcess);
    printf("Processing → Saved: %ld µs\n", processToSave);
    printf("Total time: %ld µs (%.2f ms)\n", totalTime, totalTime / 1000.0);
    printf("\nTo convert to PNG, use:\n");
    printf("ffmpeg -f rawvideo -pixel_format bgra -s %dx%d -i %s -frames:v 1 output.png\n",
           imageWidth, imageHeight, filename.str().c_str());
    printf("==================\n\n");
  }
  
  munmap(mem, plane.length);
}

static void requestComplete(Request *request) {
  if (request->status() == Request::RequestCancelled)
    return;

  frameCount++;
  
  const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();
  for (auto bufferPair : buffers) {
    FrameBuffer *buffer = bufferPair.second;
    const FrameMetadata &metadata = buffer->metadata();
    
    // Save first frame immediately
    if (saveNextFrame || frameCount == 1) {
      saveFrameAsRAW(buffer, metadata);
      saveNextFrame = false;
      frameSaved = true;
    }
    
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

  // Stop after saving first frame
  if (frameSaved) {
    running = false;
    return;
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
  
  // Don't set fixed resolution - use camera's default/maximum
  // The camera will use its highest available resolution for Viewfinder
  config->validate();
  
  // Store the actual resolution and format
  imageWidth = streamConfig.size.width;
  imageHeight = streamConfig.size.height;
  pixelFormat = streamConfig.pixelFormat.toString();
  
  printf("Using configuration: %s\n", streamConfig.toString().c_str());
  printf("Resolution: %dx%d\n", imageWidth, imageHeight);
  printf("Pixel Format: %s\n", pixelFormat.c_str());
  
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
  printf("Camera started, capturing and saving first frame...\n");
  
  startTime = std::chrono::steady_clock::now();
  
  // Queue all requests initially
  for (std::unique_ptr<Request> &request : requests) {
    camera->queueRequest(request.get());
  }
  
  // Run until frame is saved or interrupted
  auto captureStart = std::chrono::steady_clock::now();
  while (running && !frameSaved) {
    std::this_thread::sleep_for(10ms);
    
    // Timeout after 5 seconds if frame not saved
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - captureStart).count() >= 5) {
      printf("Timeout waiting for frame\n");
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
