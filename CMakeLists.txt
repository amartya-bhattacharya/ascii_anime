cmake_minimum_required(VERSION 3.29)
project(anime_to_ascii C)

set(CMAKE_C_STANDARD 11)

# Add include directories for FFmpeg
include_directories(/usr/local/include)

# Add the executable
add_executable(anime_to_ascii main.c)

# Set FFmpeg library paths
set(FFMPEG_LIB_DIR /usr/local/lib)
set(FFMPEG_INCLUDE_DIR /usr/local/include)

include_directories(${FFMPEG_INCLUDE_DIR})

# Link FFmpeg libraries
target_link_libraries(anime_to_ascii
    ${FFMPEG_LIB_DIR}/libavcodec.dylib
    ${FFMPEG_LIB_DIR}/libavformat.dylib
    ${FFMPEG_LIB_DIR}/libavutil.dylib
    ${FFMPEG_LIB_DIR}/libswscale.dylib
)

# Include stb_image_write header
target_include_directories(anime_to_ascii PRIVATE ${CMAKE_SOURCE_DIR})

