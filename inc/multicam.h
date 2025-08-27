#ifndef MULTICAM_H
#define MULTICAM_H

// Standard C headers
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

// Standard C++ headers
#include <iostream>
#include <iomanip>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <fstream>
#include <sstream>

// libcamera headers
#include <libcamera/libcamera.h>

using namespace libcamera;
using namespace std::chrono_literals;

#endif // MULTICAM_H