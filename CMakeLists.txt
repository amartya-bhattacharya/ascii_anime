cmake_minimum_required(VERSION 3.29)
project(anime_to_ascii C)

set(CMAKE_C_STANDARD 11)

# Add FFmpeg include directory (assuming FFmpeg is installed in /usr/local)
include_directories(/usr/local/include)

# Find the FFmpeg packages using pkg-config if available
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
    libavdevice
)

# Add the executable target
add_executable(anime_to_ascii main.c)

# Link FFmpeg libraries to the target
target_link_libraries(anime_to_ascii
    PkgConfig::FFMPEG
)

# Set rpath so that the linked FFmpeg libraries can be found at runtime
set_target_properties(anime_to_ascii PROPERTIES INSTALL_RPATH "/usr/local/lib")
