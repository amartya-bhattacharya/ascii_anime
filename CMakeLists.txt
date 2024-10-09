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
)

# Add the executable target
add_executable(anime_to_ascii src/main.c)

# Include FFmpeg headers and link libraries
target_include_directories(anime_to_ascii PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(anime_to_ascii PRIVATE PkgConfig::FFMPEG)

# Set rpath to locate FFmpeg libraries at runtime (useful on Unix systems)
set_target_properties(anime_to_ascii PROPERTIES
    INSTALL_RPATH "${FFMPEG_LIBRARY_DIRS}"
    BUILD_RPATH "${FFMPEG_LIBRARY_DIRS}"
)

# Compiler flags (more resilient for older versions of FFmpeg)
target_compile_options(anime_to_ascii PRIVATE ${FFMPEG_CFLAGS_OTHER})
