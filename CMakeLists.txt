cmake_minimum_required(VERSION 3.29)
project(anime_to_ascii C)

set(CMAKE_C_STANDARD 11)

# Find the FFmpeg packages using pkg-config if available
find_package(PkgConfig REQUIRED)

# Find the necessary FFmpeg packages using pkg-config
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
    libavdevice
)

# Include FFmpeg directories
include_directories(${FFMPEG_INCLUDE_DIRS})

# Add the executable target
add_executable(anime_to_ascii main.c)

# Link FFmpeg libraries to the target
target_link_libraries(anime_to_ascii
    PkgConfig::FFMPEG
)

# Set rpath to locate FFmpeg libraries at runtime (useful on Unix systems)
set_target_properties(anime_to_ascii PROPERTIES
    INSTALL_RPATH "/usr/local/lib"
    BUILD_RPATH "/usr/local/lib"
)

# Set additional compiler flags if necessary for FFmpeg or other dependencies
target_compile_options(anime_to_ascii PRIVATE ${FFMPEG_CFLAGS_OTHER})
