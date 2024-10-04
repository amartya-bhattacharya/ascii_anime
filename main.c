#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <termios.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <pthread.h>
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_truetype.h"
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#define FONT_SIZE 16  // Font size for rendering
#define FONT_PATH "Topaz-8.ttf" // Path to the Topaz-8 font file

// stb_truetype font buffer
unsigned char ttf_buffer[1<<20];
stbtt_fontinfo font;

// Prototypes
typedef struct {
    int gray_value;
    int r, g, b;
} CachedPixel;

void get_terminal_size(int *rows, int *cols);
CachedPixel* cache_grayscale_values(unsigned char *img, int img_width, int img_height);
void render_ascii_art_terminal(CachedPixel *cached_img, int img_width, int img_height, int term_rows, int term_cols, const char *char_set, int char_set_size);

// Default ASCII character set
const char *ASCII_CHARS_DEFAULT = " .:-=+*#%@";
int ascii_map_size_default = 10;

// Extended ASCII character set
const char *ASCII_CHARS_EXTENDED = " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
int ascii_map_size_extended = 70;

// Block characters for increased granularity
const char *BLOCK_CHARS = "▁▂▃▄▅▆▇█";
int block_map_size = 8;

// Constants for buffering
#define FRAME_BUFFER_SIZE 5

// Frame buffer and related data
typedef struct {
    AVFrame *frame;
    CachedPixel *cached_img;
    int is_ready;
} FrameBuffer;

FrameBuffer frame_buffer[FRAME_BUFFER_SIZE];

int buffer_write_index = 0;
int buffer_read_index = 0;

// Synchronization primitives
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_cond = PTHREAD_COND_INITIALIZER;

// Flags for thread control
volatile bool is_running = true;


// Function to get a formatted timestamp
void print_timestamp(const char *message) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    printf("[%ld.%06ld] %s\n", tv.tv_sec, tv.tv_usec, message);
    fflush(stdout);
}


int init_font(const char *font_path) {
    print_timestamp("Initializing font...");
    FILE *font_file = fopen(font_path, "rb");
    if (font_file) {
        fread(ttf_buffer, 1, 1 << 20, font_file);
        fclose(font_file);
        stbtt_InitFont(&font, ttf_buffer, stbtt_GetFontOffsetForIndex(ttf_buffer, 0));
    } else {
        fprintf(stderr, "Error: Failed to load font file.\n");
        return 1;
    }
    return 0;
}

// Initialize FFmpeg and open the input video file
void initialize_ffmpeg(const char *filename, AVFormatContext **pFormatContext, AVCodecContext **pCodecContext, int *video_stream_index) {
    print_timestamp("Initializing FFmpeg...");
    avformat_open_input(pFormatContext, filename, NULL, NULL);
    avformat_find_stream_info(*pFormatContext, NULL);

    *video_stream_index = -1;
    for (int i = 0; i < (*pFormatContext)->nb_streams; i++) {
        if ((*pFormatContext)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            *video_stream_index = i;
            break;
        }
    }

    if (*video_stream_index == -1) {
        printf("No video stream found\n");
        return;
    }

    AVCodecParameters *codec_params = (*pFormatContext)->streams[*video_stream_index]->codecpar;
    AVCodec *codec = (AVCodec *)avcodec_find_decoder(codec_params->codec_id);

    if (!codec) {
        printf("Failed to find codec\n");
        return;
    }

    *pCodecContext = avcodec_alloc_context3(codec);
    if (!*pCodecContext) {
        printf("Failed to allocate codec context\n");
        return;
    }

    if (avcodec_parameters_to_context(*pCodecContext, codec_params) < 0) {
        printf("Failed to copy codec parameters to context\n");
        return;
    }

    if (avcodec_open2(*pCodecContext, codec, NULL) < 0) {
        printf("Failed to open codec\n");
        return;
    }
}

// Producer thread function: Decodes and buffers frames
void *frame_producer(void *args) {
    print_timestamp("Frame producer started.");
    AVFormatContext *pFormatContext = ((AVFormatContext **)args)[0];
    AVCodecContext *pCodecContext = ((AVCodecContext **)args)[1];
    int video_stream_index = *((int *)args + 2);

    struct SwsContext *sws_ctx = sws_getContext(
        pCodecContext->width, pCodecContext->height, pCodecContext->pix_fmt,
        pCodecContext->width, pCodecContext->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    if (!sws_ctx) {
        fprintf(stderr, "Failed to initialize the SWS context\n");
        return NULL;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "Failed to allocate packet\n");
        return NULL;
    }

    while (is_running && av_read_frame(pFormatContext, packet) >= 0) {
        if (packet->stream_index == video_stream_index) {
            if (avcodec_send_packet(pCodecContext, packet) == 0) {
                AVFrame *frame = av_frame_alloc();
                if (!frame) {
                    fprintf(stderr, "Failed to allocate memory for frame\n");
                    continue;
                }

                if (avcodec_receive_frame(pCodecContext, frame) == 0) {
                    print_timestamp("Frame decoded successfully.");
                    // Convert the frame to RGB
                    AVFrame *rgb_frame = av_frame_alloc();
                    uint8_t *buffer = (uint8_t *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height, 32));
                    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer, AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height, 32);

                    sws_scale(sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0, pCodecContext->height, rgb_frame->data, rgb_frame->linesize);

                    // Lock the buffer and write frame to it
                    pthread_mutex_lock(&buffer_mutex);
                    print_timestamp("Producer: Waiting for an empty buffer slot...");

                    while (frame_buffer[buffer_write_index].is_ready) {
                        pthread_cond_wait(&buffer_cond, &buffer_mutex);
                    }

                    print_timestamp("Producer: Writing frame to buffer.");

                    // Cache the grayscale values and set frame as ready
                    frame_buffer[buffer_write_index].cached_img = cache_grayscale_values(rgb_frame->data[0], pCodecContext->width, pCodecContext->height);
                    frame_buffer[buffer_write_index].frame = frame;
                    frame_buffer[buffer_write_index].is_ready = 1;

                    buffer_write_index = (buffer_write_index + 1) % FRAME_BUFFER_SIZE;

                    pthread_cond_signal(&buffer_cond);
                    pthread_mutex_unlock(&buffer_mutex);

                    av_frame_free(&rgb_frame);
                    av_free(buffer);
                } else {
                    av_frame_free(&frame);
                }
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    sws_freeContext(sws_ctx);
    print_timestamp("Frame producer finished.");
    return NULL;
}

// Consumer thread function: Renders frames to terminal
void *frame_consumer(void *args) {
    print_timestamp("Frame consumer started.");
    int pCodecContext_width = ((int *)args)[0];
    int pCodecContext_height = ((int *)args)[1];
    double fps = *((double *)args + 2);

    struct timespec start_time, current_time;

    while (is_running) {
        pthread_mutex_lock(&buffer_mutex);
        print_timestamp("Consumer: Waiting for a ready frame...");
        while (!frame_buffer[buffer_read_index].is_ready) {
            pthread_cond_wait(&buffer_cond, &buffer_mutex);
        }

        print_timestamp("Consumer: Rendering frame.");

        // Get the frame from the buffer
        CachedPixel *cached_img = frame_buffer[buffer_read_index].cached_img;
        int term_rows, term_cols;
        get_terminal_size(&term_rows, &term_cols);

        // Render the frame
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        render_ascii_art_terminal(cached_img, pCodecContext_width, pCodecContext_height, term_rows, term_cols, ASCII_CHARS_DEFAULT, ascii_map_size_default);

        // Calculate real-time FPS
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double elapsed_time_ms = (current_time.tv_sec - start_time.tv_sec) * 1000.0 + (current_time.tv_nsec - start_time.tv_nsec) / 1e6;
        double delay_ms = (1000.0 / fps) - elapsed_time_ms;

        if (delay_ms > 0) {
            usleep(delay_ms * 1000);
        }

        // Mark frame as consumed and move to the next one
        frame_buffer[buffer_read_index].is_ready = 0;
        buffer_read_index = (buffer_read_index + 1) % FRAME_BUFFER_SIZE;

        pthread_cond_signal(&buffer_cond);
        pthread_mutex_unlock(&buffer_mutex);
    }
    print_timestamp("Frame consumer finished.");
    return NULL;
}

static volatile bool resized = false;
const int debug_lines = 4;  // Number of lines to print after image

// Function to handle terminal resize events
void handle_resize(int sig) {
    resized = true;  // Set a flag to indicate the terminal has resized
}

// Function to get terminal size
void get_terminal_size(int *rows, int *cols) {
    struct winsize w;
    ioctl(STDIN_FILENO, TIOCGWINSZ, &w);
    *rows = w.ws_row;
    *cols = w.ws_col;
}

// Function to print colored ASCII characters with a black background
static inline void print_colored_char(char ascii_char, int r, int g, int b) {
    printf("\033[48;2;0;0;0m\033[38;2;%d;%d;%dm%c", r, g, b, ascii_char);
}

// Function to clear the terminal
void clear_terminal() {
    printf("\033[2J\033[H");  // Clear the terminal and move the cursor to the top
    fflush(stdout);
}

// Function to initialize the cached pixel array
CachedPixel* cache_grayscale_values(unsigned char *img, int img_width, int img_height) {
    CachedPixel *cached_img = (CachedPixel *)malloc(img_width * img_height * sizeof(CachedPixel));

    for (int y = 0; y < img_height; y++) {
        for (int x = 0; x < img_width; x++) {
            int index = (y * img_width + x) * 3;
            int r = img[index];
            int g = img[index + 1];
            int b = img[index + 2];

            cached_img[y * img_width + x].r = r;
            cached_img[y * img_width + x].g = g;
            cached_img[y * img_width + x].b = b;
            cached_img[y * img_width + x].gray_value = 0.299 * r + 0.587 * g + 0.114 * b;
        }
    }

    return cached_img;
}

// Modify print function to move cursor back to the beginning instead of clearing
//void render_ascii_art_terminal(CachedPixel *cached_img, int img_width, int img_height, int term_rows, int term_cols, const char *char_set, int char_set_size) {
//    float char_aspect_ratio = 2.0;
//
//    // Precompute scaled dimensions once and reuse in loops
//    term_rows -= debug_lines;
//    float img_aspect_ratio = (float)img_width / img_height;
//
//    int target_width = term_cols;
//    int target_height = target_width / img_aspect_ratio / char_aspect_ratio;
//    if (target_height > term_rows) {
//        target_height = term_rows;
//        target_width = target_height * img_aspect_ratio * char_aspect_ratio;
//    }
//
//    // Clear terminal and move the cursor to the top before every render
//    clear_terminal();
//    printf("\033[H");
//
//    // Use the precomputed grayscale value from CachedPixel
//    for (int y = 0; y < target_height; y++) {
//        for (int x = 0; x < target_width; x++) {
//            int img_x = x * img_width / target_width;
//            int img_y = y * img_height / target_height;
//
//            CachedPixel pixel = cached_img[img_y * img_width + img_x];
//
//            // Use precomputed grayscale value instead of calling get_ascii_char
//            int gray = pixel.gray_value;
//            char ascii_char = char_set[(gray * (char_set_size - 1)) / 255];
//            print_colored_char(ascii_char, pixel.r, pixel.g, pixel.b);
//        }
//        printf("\033[0m\n");  // Reset color after each line
//    }
//
//    // Print debug info
//    float original_ar = (float)img_width / img_height;
//    float new_ar = (float)target_width / target_height;
//    float term_ar = (float)term_cols / term_rows;
//
//    printf("\nOriginal: %dx%d (AR: %.2f) | New: %dx%d (AR: %.2f) | Term: %dx%d (AR: %.2f)\n",
//           img_width, img_height, original_ar, target_width, target_height, new_ar, term_cols, term_rows + debug_lines, term_ar);
//    fflush(stdout);
//}

// Optimized terminal rendering to avoid full clears
void render_ascii_art_terminal(CachedPixel *cached_img, int img_width, int img_height, int term_rows, int term_cols, const char *char_set, int char_set_size) {
    float char_aspect_ratio = 2.0;
    term_rows -= debug_lines;
    float img_aspect_ratio = (float)img_width / img_height;

    int target_width = term_cols;
    int target_height = target_width / img_aspect_ratio / char_aspect_ratio;
    if (target_height > term_rows) {
        target_height = term_rows;
        target_width = target_height * img_aspect_ratio * char_aspect_ratio;
    }

    printf("\033[H");  // Move cursor to the top
    for (int y = 0; y < target_height; y++) {
        for (int x = 0; x < target_width; x++) {
            int img_x = x * img_width / target_width;
            int img_y = y * img_height / target_height;

            CachedPixel pixel = cached_img[img_y * img_width + img_x];
            int gray = pixel.gray_value;
            char ascii_char = char_set[(gray * (char_set_size - 1)) / 255];
            print_colored_char(ascii_char, pixel.r, pixel.g, pixel.b);
        }
        printf("\033[0m\n");
    }

    // Print debugging information (aspect ratios)
    float original_ar = (float)img_width / img_height;
    float new_ar = (float)target_width / target_height;
    printf("\nOriginal: %dx%d (AR: %.2f) | New: %dx%d (AR: %.2f)\n", img_width, img_height, original_ar, target_width, target_height, new_ar);
    fflush(stdout);
}


// Helper function to render ASCII characters using stb_truetype
void render_ascii_to_image(unsigned char *output_img, int x, int y, char ascii_char, int img_width, int img_height, int r, int g, int b) {
    int width, height, x_offset, y_offset;
    float scale_factor = stbtt_ScaleForPixelHeight(&font, FONT_SIZE);
    unsigned char *bitmap = stbtt_GetCodepointBitmap(&font, 0, scale_factor, ascii_char, &width, &height, &x_offset, &y_offset);

    // Render the ASCII character with the foreground color (r, g, b) on a black background
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            int output_x = x + j;
            int output_y = y + i;
            int output_index = (output_y * img_width + output_x) * 4;

            // Check for out-of-bounds writes (improved for both width and height)
            if (output_x >= 0 && output_x < img_width && output_y >= 0 && output_y < img_height) {
                if (bitmap[i * width + j]) {
                    output_img[output_index] = r;       // Red channel
                    output_img[output_index + 1] = g;   // Green channel
                    output_img[output_index + 2] = b;   // Blue channel
                    output_img[output_index + 3] = 255; // Alpha (fully opaque)
                }
            }
        }
    }
    stbtt_FreeBitmap(bitmap, NULL);
}

// Function to render ASCII art to a PNG file with scaling, black background, and colored ASCII characters
void render_ascii_art_file_scaled(CachedPixel *cached_img, int img_width, int img_height, const char *char_set, int char_set_size, const char *output_file, float scale_factor, int font_scale) {
    // Precompute scaled dimensions once and reuse in loops
    int scaled_width = (int)(img_width * scale_factor);
    int scaled_height = (int)(img_height * scale_factor);

    // Allocate memory for the output image (RGBA)
    size_t output_size = scaled_width * scaled_height * 4;
    unsigned char *output_img = (unsigned char *)malloc(output_size);
    if (!output_img) {
        printf("Failed to allocate memory for output image.\n");
        return;
    }

    // Fill the entire output image with black (for background)
    for (size_t i = 0; i < output_size; i += 4) {
        output_img[i] = 0;     // Red
        output_img[i + 1] = 0; // Green
        output_img[i + 2] = 0; // Blue
        output_img[i + 3] = 255; // Alpha (fully opaque)
    }

    // Iterate through each pixel and render ASCII characters
    for (int y = 0; y < scaled_height; y += font_scale) {
        for (int x = 0; x < scaled_width; x += font_scale) {
            int img_x = (int)(x / scale_factor);
            int img_y = (int)(y / scale_factor);

            // Ensure that we're still within bounds of the input image
            if (img_x < img_width && img_y < img_height) {
                CachedPixel pixel = cached_img[img_y * img_width + img_x];
                int r = pixel.r;
                int g = pixel.g;
                int b = pixel.b;

                // Use precomputed grayscale value from CachedPixel
                int gray = pixel.gray_value;

                // Render the ASCII character at the correct position
                char ascii_char = char_set[(gray * (char_set_size - 1)) / 255];
                if (ascii_char != ' ') {
                    render_ascii_to_image(output_img, x, y, ascii_char, scaled_width, scaled_height, r, g, b);
                }
            }
        }
    }

    // Save the output image as PNG
    stbi_write_png(output_file, scaled_width, scaled_height, 4, output_img, scaled_width * 4);

    free(output_img);
}

void render_ascii_art_file_txt(CachedPixel *cached_img, int img_width, int img_height, const char *char_set, int char_set_size, const char *output_file, int term_rows, int term_cols) {
    float char_aspect_ratio = 2.0;

    // Adjust terminal height to account for debug information
    term_rows -= debug_lines;
    float img_aspect_ratio = (float)img_width / img_height;

    int target_width = term_cols;
    int target_height = target_width / img_aspect_ratio / char_aspect_ratio;
    if (target_height > term_rows) {
        target_height = term_rows;
        target_width = target_height * img_aspect_ratio * char_aspect_ratio;
    }

    // Open the output file for writing
    FILE *file = fopen(output_file, "w");
    if (!file) {
        printf("Failed to create output file: %s\n", output_file);
        return;
    }

    // Write the ASCII art to the text file using the same logic as terminal rendering
    for (int y = 0; y < target_height; y++) {
        for (int x = 0; x < target_width; x++) {
            int img_x = x * img_width / target_width;
            int img_y = y * img_height / target_height;

            CachedPixel pixel = cached_img[img_y * img_width + img_x];

            // Use precomputed grayscale value from CachedPixel
            int gray = pixel.gray_value;
            char ascii_char = char_set[(gray * (char_set_size - 1)) / 255];

            // Write the ASCII character to the file
            fputc(ascii_char, file);
        }
        fputc('\n', file);  // Newline after each row
    }

    fclose(file);
    printf("ASCII art saved to text file: %s\n", output_file);
}

void render_video_to_terminal(AVFormatContext *pFormatContext, AVCodecContext *pCodecContext, int video_stream_index) {
    struct SwsContext *sws_ctx = sws_getContext(
        pCodecContext->width, pCodecContext->height, pCodecContext->pix_fmt,
        pCodecContext->width, pCodecContext->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    if (!sws_ctx) {
        fprintf(stderr, "Failed to initialize the SWS context\n");
        return;
    }

    AVFrame *frame = av_frame_alloc();
    AVFrame *rgb_frame = av_frame_alloc();
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height, 32);
    uint8_t *buffer = (uint8_t *)av_malloc(num_bytes * sizeof(uint8_t));
    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer, AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height, 32);

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "Failed to allocate memory for packet\n");
        return;
    }

    double fps = av_q2d(pFormatContext->streams[video_stream_index]->r_frame_rate);
    printf("Original video FPS: %.2f\n", fps);
    fflush(stdout);

    struct timeval start_time, current_time, frame_start;
    double elapsed_time = 0;
    gettimeofday(&start_time, NULL);

    while (av_read_frame(pFormatContext, packet) >= 0) {
        if (packet->stream_index == video_stream_index) {
            if (avcodec_send_packet(pCodecContext, packet) == 0) {
                while (avcodec_receive_frame(pCodecContext, frame) == 0) {
                    gettimeofday(&frame_start, NULL);  // Mark start time of frame rendering

                    sws_scale(sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0, pCodecContext->height, rgb_frame->data, rgb_frame->linesize);

                    CachedPixel *cached_img = cache_grayscale_values(rgb_frame->data[0], pCodecContext->width, pCodecContext->height);
                    int term_rows, term_cols;
                    get_terminal_size(&term_rows, &term_cols);

                    render_ascii_art_terminal(cached_img, pCodecContext->width, pCodecContext->height, term_rows, term_cols, ASCII_CHARS_DEFAULT, ascii_map_size_default);

                    free(cached_img);

                    // Calculate elapsed time since the start of frame processing
                    gettimeofday(&current_time, NULL);
                    elapsed_time = (current_time.tv_sec - frame_start.tv_sec) * 1000.0;  // Convert to milliseconds
                    elapsed_time += (current_time.tv_usec - frame_start.tv_usec) / 1000.0;  // Convert microseconds to milliseconds

                    // Calculate delay to sync with the original frame rate
                    double expected_frame_time = 1000.0 / fps;  // Expected time per frame in milliseconds
                    if (elapsed_time < expected_frame_time) {
                        usleep((expected_frame_time - elapsed_time) * 1000);  // Sleep to maintain FPS
                    }
                }
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_free(buffer);
    sws_freeContext(sws_ctx);
}

void render_video_to_terminal_with_buffering(AVFormatContext *pFormatContext, AVCodecContext *pCodecContext, int video_stream_index) {
    // Set up SwsContext for RGB conversion
    struct SwsContext *sws_ctx = sws_getContext(pCodecContext->width, pCodecContext->height, pCodecContext->pix_fmt,
                                                pCodecContext->width, pCodecContext->height, AV_PIX_FMT_RGB24,
                                                SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        fprintf(stderr, "Failed to initialize the SWS context\n");
        return;
    }

    // Set up frame buffering
    int buffer_size = 5;
    AVFrame *frames[buffer_size];
    CachedPixel *cached_frames[buffer_size];
    for (int i = 0; i < buffer_size; ++i) {
        frames[i] = av_frame_alloc();
        cached_frames[i] = NULL;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "Failed to allocate packet\n");
        return;
    }

    double fps = av_q2d(pFormatContext->streams[video_stream_index]->r_frame_rate);
    double frame_duration_ms = 1000.0 / fps;

    // Timing variables
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    int frame_index = 0;
    while (av_read_frame(pFormatContext, packet) >= 0) {
        if (packet->stream_index == video_stream_index) {
            if (avcodec_send_packet(pCodecContext, packet) == 0) {
                while (avcodec_receive_frame(pCodecContext, frames[frame_index]) == 0) {
                    // Convert to RGB and cache grayscale values
                    AVFrame *rgb_frame = av_frame_alloc();
                    uint8_t *buffer = (uint8_t *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height, 32));
                    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer, AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height, 32);
                    sws_scale(sws_ctx, (uint8_t const * const *)frames[frame_index]->data, frames[frame_index]->linesize, 0, pCodecContext->height,
                              rgb_frame->data, rgb_frame->linesize);

                    cached_frames[frame_index] = cache_grayscale_values(rgb_frame->data[0], pCodecContext->width, pCodecContext->height);

                    // Update frame index for circular buffer
                    frame_index = (frame_index + 1) % buffer_size;

                    // Free resources used for this frame after caching
                    av_frame_free(&rgb_frame);
                    av_free(buffer);
                }
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    sws_freeContext(sws_ctx);

    // Render frames in buffer
    for (int i = 0; i < buffer_size; ++i) {
        if (cached_frames[i]) {
            // Render the frame from the cache
            int term_rows, term_cols;
            get_terminal_size(&term_rows, &term_cols);
            render_ascii_art_terminal(cached_frames[i], pCodecContext->width, pCodecContext->height, term_rows, term_cols, ASCII_CHARS_DEFAULT, ascii_map_size_default);

            // Sleep to maintain frame rate
//            clock_gettime(CLOCK_MONOTONIC, &current_time);
//            double elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000.0 + (current_time.tv_nsec - start_time.tv_nsec) / 1e6;
//            double delay = frame_duration_ms - elapsed_ms;
//            if (delay > 0) {
//                usleep(delay * 1000);
//            }

            // Update start time for the next frame
            clock_gettime(CLOCK_MONOTONIC, &start_time);
        }
    }

    // Free all cached frames
    for (int i = 0; i < buffer_size; ++i) {
        if (cached_frames[i]) {
            free(cached_frames[i]);
        }
        av_frame_free(&frames[i]);
    }
}


// Function to configure terminal to non-blocking input
void set_nonblocking_input() {
    struct termios tty;
    tcgetattr(STDIN_FILENO, &tty);
    tty.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    tty.c_cc[VMIN] = 0;  // Minimum number of characters for non-blocking
    tty.c_cc[VTIME] = 1;  // Timeout for non-blocking input
    tcsetattr(STDIN_FILENO, TCSANOW, &tty);
}

// Function to reset terminal input mode
void reset_input_mode() {
    struct termios tty;
    tcgetattr(STDIN_FILENO, &tty);
    tty.c_lflag |= ICANON | ECHO;  // Enable canonical mode and echo
    tcsetattr(STDIN_FILENO, TCSANOW, &tty);
}

// Function to generate the output filename by appending "-ascii.png" to the input filename
void generate_output_filename(const char *input_filename, char *output_filename, const char *extension) {
    // Copy the input filename up to (but not including) the last dot if it exists
    snprintf(output_filename, 256, "%.*s-ascii.%s",
             (int)(strrchr(input_filename, '.') ? strrchr(input_filename, '.') - input_filename : strlen(input_filename)),
             input_filename, extension);
}

void print_memory_usage() {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    printf("Memory usage: %ld MB\n", usage.ru_maxrss / 1024);
}

int is_video_file(const char *filename) {
    // List of common video extensions
    const char *video_extensions[] = {".mp4", ".avi", ".mkv", ".mov", ".flv", ".webm", NULL};
    const char *dot = strrchr(filename, '.');
    if (dot) {
        for (int i = 0; video_extensions[i] != NULL; i++) {
            if (strcasecmp(dot, video_extensions[i]) == 0) {
                return 1; // It's a video file
            }
        }
    }
    return 0; // Not a video file
}

int main(int argc, char *argv[]) {
    CachedPixel *cached_img = NULL;
    unsigned char *img = NULL;

    if (argc < 2) {
        printf("Usage: %s <image file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];

    // Check if the input is a video file
    if (is_video_file(filename)) {
        AVFormatContext *pFormatContext = NULL;
        AVCodecContext *pCodecContext = NULL;
        int video_stream_index = -1;

        initialize_ffmpeg(filename, &pFormatContext, &pCodecContext, &video_stream_index);
        if (video_stream_index == -1) {
            fprintf(stderr, "Failed to find video stream.\n");
            return -1;
        }

        // Retrieve original frame rate from the video
        double fps = av_q2d(pFormatContext->streams[video_stream_index]->r_frame_rate);
        printf("Original video FPS: %.2f\n", fps);
        fflush(stdout);

        // Create producer and consumer threads
        pthread_t producer_thread, consumer_thread;

        void *producer_args[] = {pFormatContext, pCodecContext, &video_stream_index};
        void *consumer_args[] = {&pCodecContext->width, &pCodecContext->height, &fps};

        pthread_create(&producer_thread, NULL, frame_producer, producer_args);
        pthread_create(&consumer_thread, NULL, frame_consumer, consumer_args);

        // Join threads
        pthread_join(producer_thread, NULL);
        pthread_join(consumer_thread, NULL);

        // Cleanup FFmpeg resources
        avcodec_free_context(&pCodecContext);
        avformat_close_input(&pFormatContext);

        return 0;
    }

    // If it's not a video
    int img_width, img_height, channels;

    // Load the image
    img = stbi_load(filename, &img_width, &img_height, &channels, 3);
    if (!img) {
        fprintf(stderr, "Error: Failed to load image: %s\n", filename);
        return 1;
    }

    // Initialize cached pixel array
    cached_img = cache_grayscale_values(img, img_width, img_height);
    if (!cached_img) {
        fprintf(stderr, "Error: Failed to cache grayscale values.\n");
        stbi_image_free(img);
        return 1;
    }

    // Let user choose character set for rendering
    int choice;
    printf("Choose character set for rendering:\n");
    printf("1. Default ASCII ( .:-=+*#%%@ )\n");
    printf("2. Extended ASCII ( . .. :;; IIl .... @ etc.)\n");
    printf("3. Block characters ( ▁▂▃▄▅▆▇█ )\n");
    printf("Enter your choice (1/2/3): ");
    scanf("%d", &choice);

    const char *char_set;
    int char_set_size;
    bool is_block = false;

    switch (choice) {
        case 1:
            char_set = ASCII_CHARS_DEFAULT;
            char_set_size = ascii_map_size_default;
            break;
        case 2:
            char_set = ASCII_CHARS_EXTENDED;
            char_set_size = ascii_map_size_extended;
            break;
        case 3:
            char_set = BLOCK_CHARS;
            char_set_size = block_map_size;
//            is_block = true;  // Mark as using block characters
            break;
        default:
            fprintf(stderr, "Error: Invalid choice for character set.\n");
            free(cached_img);
            stbi_image_free(img);
            return 1;
    }

    // Let user choose output mode
    int output_mode;
    printf("Choose output mode:\n");
    printf("1. Terminal\n");
    printf("2. PNG\n");
    printf("3. TXT\n");
    printf("Enter your choice (1/2/3): ");
    scanf("%d", &output_mode);

    if (output_mode != 1 && output_mode != 2 && output_mode != 3) {
        fprintf(stderr, "Error: Invalid output mode. Must be 1, 2, or 3.\n");
        return 1;
    }

    if (output_mode == 1) { // Terminal output mode
        // Prepare terminal
        int term_rows, term_cols;
        get_terminal_size(&term_rows, &term_cols);
        clear_terminal();

        // Initial render
        render_ascii_art_terminal(cached_img, img_width, img_height, term_rows, term_cols, char_set, char_set_size);

        // Set up for resizing
        struct sigaction sa;
        sa.sa_handler = handle_resize;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGWINCH, &sa, NULL);
        set_nonblocking_input();

        // Main loop to handle live re-rendering on terminal resize or 'q' press
        while (true) {
            if (resized) {
                get_terminal_size(&term_rows, &term_cols);
                render_ascii_art_terminal(cached_img, img_width, img_height, term_rows, term_cols, char_set, char_set_size);
                resized = false;
            }

            // Check if 'q' has been pressed to quit
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0 && c == 'q') {
                break;
            }

//            usleep(100000);  // Sleep for 100ms to avoid excessive CPU usage
        }

        reset_input_mode();
        print_memory_usage();
    } else if (output_mode == 2) {  // File output mode
        // Profiling
        clock_t start_time = clock();

        // Generate the output filename
        char output_filename[256];
        generate_output_filename(filename, output_filename, "png");

        // Load the font
        init_font(FONT_PATH);

        // Let user choose scaling factor
        float scale_factor;
        printf("Enter a scale factor (e.g., 0.5 for half size, 1 for original size, 2 for double size): ");
        scanf("%f", &scale_factor);

        if (scale_factor <= 0) {
            fprintf(stderr, "Error: Invalid scale factor. Must be greater than 0.\n");
            free(cached_img);
            stbi_image_free(img);
            return 1;
        }

        render_ascii_art_file_scaled(cached_img, img_width, img_height, char_set, char_set_size, output_filename, scale_factor, FONT_SIZE);

        // Profiling
        clock_t end_time = clock();
        double file_render_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;
        printf("File render time: %.2f seconds\n", file_render_time);
        printf("ASCII art saved to file: %s\n", output_filename);
        print_memory_usage();
    } else if (output_mode == 3) {
        char output_filename[256];
        generate_output_filename(filename, output_filename, "txt");

        int term_rows = 0;
        int term_cols = 0;
        get_terminal_size(&term_rows, &term_cols);

    render_ascii_art_file_txt(cached_img, img_width, img_height, char_set, char_set_size, output_filename, term_rows, term_cols);
    print_memory_usage();
    }

    // Free memory
    free(cached_img);
    stbi_image_free(img);

    return 0;
}
