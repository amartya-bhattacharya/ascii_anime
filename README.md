# ASCII Image Generator Demo

This project converts images (and videos) into ASCII art! Options for default or extended ASCII set. block chars WIP.
Options to render directly to terminal, or output to a png/txt file. Output saved as `<input>-ascii.png/.txt`.

### Dependencies
- **FFmpeg**: Used for video processing. Install via:
  - macOS: `brew install ffmpeg`
  - Ubuntu: `sudo apt install ffmpeg`
  - Windows: Download from the [official site](https://ffmpeg.org/download.html)

### Build the Executable
To build the project:

```sh
cmake -S . -B build
cmake --build build
```

Executable will be located at `build/anime_to_ascii`.

### Usage

```shell
./build/anime_to_ascii <input_file>
```

Rendering options:
- Default ASCII set
- Extended ASCII set
- Block characters (WIP)

Output options:
- Live render to terminal
- Save to a PNG file
  - Upscale or downscale the image
- Save to a TXT file

Press `q` to quit.
