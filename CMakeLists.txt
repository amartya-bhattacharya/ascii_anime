cmake_minimum_required(VERSION 3.16)
project(anime_to_ascii C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find PkgConfig itself
find_package(PkgConfig REQUIRED)

# Find FFmpeg libraries and includes
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)

# Add the executable target
add_executable(anime_to_ascii src/main.c)

# Link using the imported targets created by pkg_check_modules
target_link_libraries(anime_to_ascii PRIVATE
        PkgConfig::AVCODEC
        PkgConfig::AVFORMAT
        PkgConfig::AVUTIL
        PkgConfig::SWSCALE
        m
)

# Compiler flags
target_compile_options(anime_to_ascii PRIVATE "-Wall" "-Wextra")