cmake_minimum_required(VERSION 3.10)

# Set the project name
project(libcamera_example)

# Set C++ standard (libcamera requires C++17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Find required packages
find_package(PkgConfig REQUIRED)

# Find libcamera using pkg-config
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# Add include directories
include_directories(${LIBCAMERA_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR}/inc)

# Add library directories
link_directories(${LIBCAMERA_LIBRARY_DIRS})

# Add compile flags
add_compile_options(${LIBCAMERA_CFLAGS_OTHER})

# Create executables
# main executable (camera list)
add_executable(main src/main.cpp)
target_link_libraries(main ${LIBCAMERA_LIBRARIES})

# onecam_capture executable
add_executable(onecam_capture src/onecam_capture.cpp)
target_link_libraries(onecam_capture ${LIBCAMERA_LIBRARIES})

# onecam_frame executable
add_executable(onecam_frame src/onecam_frame.cpp)
target_link_libraries(onecam_frame ${LIBCAMERA_LIBRARIES})

# simple_cam executable (with event_loop)
add_executable(simple_cam src/simple_cam.cpp src/event_loop.cpp)
target_link_libraries(simple_cam ${LIBCAMERA_LIBRARIES})

# Optional: Set some useful compiler flags for all executables
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(main PRIVATE -Wall -Wextra)
    target_compile_options(onecam_capture PRIVATE -Wall -Wextra)
    target_compile_options(onecam_frame PRIVATE -Wall -Wextra)
    target_compile_options(simple_cam PRIVATE -Wall -Wextra)
endif()
