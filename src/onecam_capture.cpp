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
static uint32_t imageStride = 0;
static std::string pixelFormat = "";
static uint32_t bitsPerPixel = 0;
static uint32_t numPlanes = 0;

static void signalHandler(int signal) {
  if (signal == SIGINT) {
    printf("\nReceived interrupt signal, stopping...\n");
    running = false;
  }
}

// Function to detect pixel format details
static void detectFormatDetails(const StreamConfiguration &config) {
  pixelFormat = config.pixelFormat.toString();
  imageWidth = config.size.width;
  imageHeight = config.size.height;
  imageStride = config.stride;
  
  // Estimate number of planes based on format (since planesCount() doesn't exist)
  std::string formatStr = pixelFormat;
  if (formatStr.find("YUV420") != std::string::npos ||
      formatStr.find("YV12") != std::string::npos) {
    numPlanes = 3;  // Y, U, V planes
  } else if (formatStr.find("NV12") != std::string::npos ||
             formatStr.find("NV21") != std::string::npos) {
    numPlanes = 2;  // Y plane + UV plane
  } else {
    numPlanes = 1;  // Most RGB formats are single plane
  }
  
  // Determine bits per pixel based on format
  std::string formatStr = pixelFormat;
  if (formatStr.find("XRGB8888") != std::string::npos ||
      formatStr.find("ARGB8888") != std::string::npos ||
      formatStr.find("XBGR8888") != std::string::npos) {
    bitsPerPixel = 32;
  } else if (formatStr.find("RGB888") != std::string::npos ||
             formatStr.find("BGR888") != std::string::npos) {
    bitsPerPixel = 24;
  } else if (formatStr.find("YUYV") != std::string::npos ||
             formatStr.find("UYVY") != std::string::npos) {
    bitsPerPixel = 16;
  } else if (formatStr.find("YUV420") != std::string::npos ||
             formatStr.find("NV12") != std::string::npos) {
    bitsPerPixel = 12;  // 1.5 bytes per pixel on average
  } else {
    bitsPerPixel = 8;  // Default assumption
  }
}

// Function to get appropriate ffmpeg pixel format
static std::string getFFmpegPixelFormat(const std::string &libcameraFormat) {
  if (libcameraFormat.find("XRGB8888") != std::string::npos) return "bgr0";
  if (libcameraFormat.find("XBGR8888") != std::string::npos) return "rgb0";
  if (libcameraFormat.find("ARGB8888") != std::string::npos) return "bgra";
  if (libcameraFormat.find("YUYV") != std::string::npos) return "yuyv422";
  if (libcameraFormat.find("UYVY") != std::string::npos) return "uyvy422";
  if (libcameraFormat.find("YUV420") != std::string::npos) return "yuv420p";
  if (libcameraFormat.find("NV12") != std::string::npos) return "nv12";
  return "bgra";  // Default fallback
}

// Improved function to save raw buffer with proper stride handling
static void saveFrameAsRAW(const FrameBuffer *buffer, const FrameMetadata &) {
  auto captureStart = std::chrono::high_resolution_clock::now();
  
  // Generate timestamp filename with detailed format info
  auto now = std::chrono::system_clock::now();
  auto time_t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  
  std::stringstream filename;
  filename << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
  filename << "_" << std::setfill('0') << std::setw(3) << ms.count();
  filename << "_" << imageWidth << "x" << imageHeight;
  filename << "_" << pixelFormat;
  filename << ".raw";
  
  auto processStart = std::chrono::high_resolution_clock::now();
  
  // Handle buffer mapping - map all planes in one go if they share the same fd
  const std::vector<FrameBuffer::Plane> &planes = buffer->planes();
  
  printf("\n=== Buffer Debug Info ===\n");
  printf("Number of planes: %zu\n", planes.size());
  
  // Map the first plane (main buffer)
  const FrameBuffer::Plane &plane0 = planes[0];
  void *mem = mmap(NULL, plane0.length, PROT_READ, MAP_SHARED, plane0.fd.get(), 0);
  if (mem == MAP_FAILED) {
    printf("Failed to mmap buffer: %s\n", strerror(errno));
    return;
  }
  
  // Debug: Print plane information
  for (size_t i = 0; i < planes.size(); ++i) {
    printf("Plane %zu: offset=%u, length=%u\n", i, planes[i].offset, planes[i].length);
  }
  
  // Save different data based on number of planes and stride
  std::ofstream file(filename.str(), std::ios::binary);
  if (file.is_open()) {
    if (imageStride == imageWidth * (bitsPerPixel / 8)) {
      // No padding, can save directly
      printf("No stride padding detected, saving full buffer\n");
      file.write((char *)mem, plane0.length);
    } else {
      // Has stride padding, save row by row
      printf("Stride padding detected (stride=%u, expected=%u)\n", 
             imageStride, imageWidth * (bitsPerPixel / 8));
      
      uint8_t *data = (uint8_t *)mem;
      uint32_t bytesPerRow = imageWidth * (bitsPerPixel / 8);
      
      for (uint32_t row = 0; row < imageHeight; ++row) {
        file.write((char *)(data + row * imageStride), bytesPerRow);
      }
    }
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
    printf("Stride: %u bytes\n", imageStride);
    printf("Pixel Format: %s\n", pixelFormat.c_str());
    printf("Bits per pixel: %u\n", bitsPerPixel);
    printf("Number of planes: %u\n", numPlanes);
    printf("Buffer length: %u bytes\n", plane0.length);
    printf("Actual image size: %u bytes\n", imageWidth * imageHeight * (bitsPerPixel / 8));
    printf("Capture → Processing: %ld µs\n", captureToProcess);
    printf("Processing → Saved: %ld µs\n", processToSave);
    printf("Total time: %ld µs (%.2f ms)\n", totalTime, totalTime / 1000.0);
    
    // Generate correct ffmpeg command
    std::string ffmpegFormat = getFFmpegPixelFormat(pixelFormat);
    printf("\nTo convert to PNG, use:\n");
    printf("ffmpeg -f rawvideo -pixel_format %s -s %dx%d -i %s output.png\n",
           ffmpegFormat.c_str(), imageWidth, imageHeight, filename.str().c_str());
    printf("==================\n\n");
  }
  
  munmap(mem, plane0.length);
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
  
  // Detect and store detailed format information
  detectFormatDetails(streamConfig);
  
  printf("Using configuration: %s\n", streamConfig.toString().c_str());
  printf("Resolution: %dx%d\n", imageWidth, imageHeight);
  printf("Stride: %u bytes\n", imageStride);
  printf("Pixel Format: %s\n", pixelFormat.c_str());
  printf("Bits per pixel: %u\n", bitsPerPixel);
  printf("Number of planes: %u\n", numPlanes);
  printf("Expected bytes per row: %u\n", imageWidth * (bitsPerPixel / 8));
  printf("Actual stride: %u\n", imageStride);
  
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
