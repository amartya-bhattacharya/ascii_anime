cmake_minimum_required(VERSION 3.29)
project(anime_to_ascii C)

set(CMAKE_C_STANDARD 11)

# Manually find FFmpeg libraries
find_library(AVCODEC_LIBRARY avcodec REQUIRED)
find_library(AVFORMAT_LIBRARY avformat REQUIRED)
find_library(AVUTIL_LIBRARY avutil REQUIRED)
find_library(SWSCALE_LIBRARY swscale REQUIRED)

# Manually find FFmpeg includes
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
    PATHS /usr/local/include /usr/include /opt/homebrew/include
    REQUIRED)

# Add the executable target
add_executable(anime_to_ascii src/main.c)

# Include FFmpeg headers
target_include_directories(anime_to_ascii PRIVATE ${FFMPEG_INCLUDE_DIR})

# Link FFmpeg libraries
target_link_libraries(anime_to_ascii PRIVATE
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWSCALE_LIBRARY}
)

# Set rpath to locate FFmpeg libraries at runtime (useful on Unix systems)
set_target_properties(anime_to_ascii PROPERTIES
    INSTALL_RPATH "/usr/local/lib;/opt/homebrew/lib"
    BUILD_RPATH "/usr/local/lib;/opt/homebrew/lib"
)

# Compiler flags (more resilient for older versions of FFmpeg)
target_compile_options(anime_to_ascii PRIVATE "-Wall" "-Wextra")
