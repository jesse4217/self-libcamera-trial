cmake_minimum_required(VERSION 3.10)

# Set the project name
project(libcamera_example)

# Set C++ standard (libcamera requires C++17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)

# Find libcamera using pkg-config
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# Add include directories
include_directories(${LIBCAMERA_INCLUDE_DIRS})

# Add library directories
link_directories(${LIBCAMERA_LIBRARY_DIRS})

# Add compile flags
add_compile_options(${LIBCAMERA_CFLAGS_OTHER})

# Create the executable
add_executable(camera_list main.cpp)

# Link against libcamera
target_link_libraries(camera_list ${LIBCAMERA_LIBRARIES})

# Optional: Set some useful compiler flags
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(camera_list PRIVATE -Wall -Wextra)
endif()
