#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
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
//#include <omp.h>
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_truetype.h"
#include "stb_image_resize2.h"
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>


volatile bool terminated = false; // Track if the program is being terminated

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
#define BUFFER_POOL_SIZE 10

// Frame pool, buffer pool, and packet pool to be reused
AVFrame *frame_pool[BUFFER_POOL_SIZE];
uint8_t *buffer_pool[BUFFER_POOL_SIZE];

// Frame buffer and related data
typedef struct {
    AVFrame *frame;
    CachedPixel *cached_img;
    int is_ready;
} FrameBuffer;

#define NUM_BUFFERS 2  // Number of buffers for double buffering
FrameBuffer frame_buffer[NUM_BUFFERS][BUFFER_POOL_SIZE];

int buffer_write_index = 0;
int buffer_read_index = 0;

CachedPixel *cached_image_pool[BUFFER_POOL_SIZE];

// Struct for passing arguments to the producer thread
typedef struct {
    AVFormatContext *pFormatContext;
    AVCodecContext *pCodecContext;
    int video_stream_index;
} ProducerArgs;

// Struct for passing arguments to the consumer thread
typedef struct {
    int pCodecContext_width;
    int pCodecContext_height;
    double fps;
} ConsumerArgs;

// Synchronization primitives
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_cond = PTHREAD_COND_INITIALIZER;

// Flags for thread control
volatile bool is_running = true;
volatile bool is_done = false; // Added to indicate that producer is done

// Profiling variables
struct timespec producer_start_time, producer_end_time;
double producer_total_time = 0.0;
double producer_read_frame_total_time = 0.0;
double producer_send_packet_total_time = 0.0;
double producer_receive_frame_total_time = 0.0;
double producer_convert_frame_total_time = 0.0;
double producer_cache_total_time = 0.0;
int producer_frame_count = 0;

struct timespec consumer_start_time, consumer_end_time;
double consumer_total_time = 0.0;
double consumer_lock_wait_total = 0.0;
double consumer_render_total = 0.0;
double consumer_buffer_update_total = 0.0;
int consumer_frame_count = 0;

// Function to get a formatted timestamp
void print_timestamp(const char *message) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    printf("[%ld.%06d] %s\n", tv.tv_sec, tv.tv_usec, message);
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

int init_ffmpeg(const char *filename, AVFormatContext **pFormatContext, AVCodecContext **pCodecContext, int *video_stream_index) {
    // Initialize FFmpeg and open the input video file
    avformat_network_init();
    print_timestamp("Initializing FFmpeg...");

    if (avformat_open_input(pFormatContext, filename, NULL, NULL) != 0) {
        fprintf(stderr, "Could not open video file: %s\n", filename);
        return -1;
    }

    if (avformat_find_stream_info(*pFormatContext, NULL) < 0) {
        fprintf(stderr, "Could not find stream information.\n");
        avformat_close_input(pFormatContext);
        return -1;
    }

    // Find the video stream
    for (int i = 0; i < (*pFormatContext)->nb_streams; i++) {
        if ((*pFormatContext)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            *video_stream_index = i;
            break;
        }
    }

    if (*video_stream_index == -1) {
        fprintf(stderr, "Failed to find video stream.\n");
        avformat_close_input(pFormatContext);
        return -1;
    }

    // Find and set up the codec context
    AVCodecParameters *codec_params = (*pFormatContext)->streams[*video_stream_index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        fprintf(stderr, "Failed to find codec.\n");
        avformat_close_input(pFormatContext);
        return -1;
    }

    *pCodecContext = avcodec_alloc_context3(codec);
    if (!*pCodecContext) {
        fprintf(stderr, "Failed to allocate codec context.\n");
        avformat_close_input(pFormatContext);
        return -1;
    }

    if (avcodec_parameters_to_context(*pCodecContext, codec_params) < 0) {
        fprintf(stderr, "Failed to copy codec parameters to context.\n");
        avcodec_free_context(pCodecContext);
        avformat_close_input(pFormatContext);
        return -1;
    }

    if (avcodec_open2(*pCodecContext, codec, NULL) < 0) {
        fprintf(stderr, "Failed to open codec.\n");
        avcodec_free_context(pCodecContext);
        avformat_close_input(pFormatContext);
        return -1;
    }

    return 0;  // Successful initialization
}

static volatile bool resized = false;
const int debug_lines = 4;  // Number of lines to print after image

// Function to handle terminal resize events
void handle_resize(int sig) {
    (void)sig;  // Mark the parameter as unused to suppress the warning
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
void cache_grayscale_values(const unsigned char *img, int img_width, int img_height, CachedPixel *cached_img) {
    #pragma omp parallel for
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
}


// Modify print function to move cursor back to the beginning instead of clearing
void render_ascii_art_terminal(CachedPixel *cached_img, int img_width, int img_height, int term_rows, int term_cols, const char *char_set, int char_set_size) {
    static double total_render_time = 0.0;
    static int frame_count = 0;

    float char_aspect_ratio = 2.0;


    // Precompute scaled dimensions once and reuse in loops
    term_rows -= debug_lines;
    float img_aspect_ratio = (float)img_width / img_height;

    int target_width = term_cols;
    int target_height = target_width / img_aspect_ratio / char_aspect_ratio;
    if (target_height > term_rows) {
        target_height = term_rows;
        target_width = target_height * img_aspect_ratio * char_aspect_ratio;
    }

    // Clear terminal and move the cursor to the top before every render
//    clear_terminal();   // causes flickering
    printf("\033[H");

    // Use the precomputed grayscale value from CachedPixel
    for (int y = 0; y < target_height; y++) {
        for (int x = 0; x < target_width; x++) {
            int img_x = x * img_width / target_width;
            int img_y = y * img_height / target_height;

            CachedPixel pixel = cached_img[img_y * img_width + img_x];

            // Use precomputed grayscale value instead of calling get_ascii_char
            int gray = pixel.gray_value;
            char ascii_char = char_set[(gray * (char_set_size - 1)) / 255];
            print_colored_char(ascii_char, pixel.r, pixel.g, pixel.b);
        }
        printf("\033[0m\n");  // Reset color after each line
    }

    // Print debug info
    float original_ar = (float)img_width / img_height;
    float new_ar = (float)target_width / target_height;

    printf("\nOriginal: %dx%d (AR: %.2f) | New: %dx%d (AR: %.2f) | Term: %dx%d",
               img_width, img_height, original_ar, target_width, target_height, new_ar, term_cols, term_rows + debug_lines);

//    fflush(stdout);   // might have to put this back and pass in the fps stuff in params for image
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
    // Ensure cached_img is not NULL
    if (!cached_img) {
        printf("Error: Cached image is NULL.\n");
        return;
    }

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
    // Ensure cached_img is not NULL
    if (!cached_img) {
        printf("Error: Cached image is NULL.\n");
        return;
    }

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

void *frame_producer(void *args) {
    ProducerArgs *prod_args = (ProducerArgs *)args;

    AVFormatContext *pFormatContext = prod_args->pFormatContext;
    AVCodecContext *pCodecContext = prod_args->pCodecContext;
    int video_stream_index = prod_args->video_stream_index;

    struct SwsContext *sws_ctx = sws_getContext(
        pCodecContext->width, pCodecContext->height, pCodecContext->pix_fmt,
        pCodecContext->width, pCodecContext->height, AV_PIX_FMT_RGB24,
        SWS_FAST_BILINEAR, NULL, NULL, NULL
    );

    if (!sws_ctx) {
        print_timestamp("Failed to initialize the SWS context");
        pthread_exit(NULL);
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        print_timestamp("Failed to allocate packet");
        sws_freeContext(sws_ctx);
        pthread_exit(NULL);
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        print_timestamp("Failed to allocate frame");
        av_packet_free(&packet);
        sws_freeContext(sws_ctx);
        pthread_exit(NULL);
    }

    int pool_index = 0; // Index for using frames and buffers from the pool
    int current_buffer = 0; // 0 for Buffer A, 1 for Buffer B

    // Granular profiling
    struct timespec read_frame_start, read_frame_end;
    struct timespec send_packet_start, send_packet_end;
    struct timespec receive_frame_start, receive_frame_end;
    struct timespec convert_frame_start, convert_frame_end;
    struct timespec cache_start, cache_end;

    double read_frame_total_time = 0.0;
    double send_packet_total_time = 0.0;
    double receive_frame_total_time = 0.0;
    double convert_frame_total_time = 0.0;
    double cache_total_time = 0.0;


    while (is_running && !terminated) {
        clock_gettime(CLOCK_MONOTONIC, &producer_start_time);  // Start profiling

        clock_gettime(CLOCK_MONOTONIC, &read_frame_start);

        int ret = av_read_frame(pFormatContext, packet);

        clock_gettime(CLOCK_MONOTONIC, &read_frame_end);
        read_frame_total_time += (read_frame_end.tv_sec - read_frame_start.tv_sec) + (read_frame_end.tv_nsec - read_frame_start.tv_nsec) / 1e9;

        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // End of file reached, no more packets to read
                break;
            } else {
                fprintf(stderr, "Error reading frame: %s\n", av_err2str(ret));
            }
            break; // Exit the loop if there's an error or EOF
        }

        if (packet->stream_index == video_stream_index) {
            clock_gettime(CLOCK_MONOTONIC, &send_packet_start);

            ret = avcodec_send_packet(pCodecContext, packet);

            clock_gettime(CLOCK_MONOTONIC, &send_packet_end);
            send_packet_total_time += (send_packet_end.tv_sec - send_packet_start.tv_sec) + (send_packet_end.tv_nsec - send_packet_start.tv_nsec) / 1e9;

            if (ret < 0) {
                fprintf(stderr, "Error sending packet to decoder: %s\n", av_err2str(ret));
                av_packet_unref(packet);
                continue;
            }

            while ((ret = avcodec_receive_frame(pCodecContext, frame)) == 0) {
                clock_gettime(CLOCK_MONOTONIC, &receive_frame_start);

                // Use pre-allocated RGB frame from the pool
                AVFrame *rgb_frame = frame_pool[pool_index];
                uint8_t *buffer = buffer_pool[pool_index];

                // Convert the frame to RGB
                clock_gettime(CLOCK_MONOTONIC, &convert_frame_start);

                av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer, AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height, 32);
                sws_scale(sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0, pCodecContext->height, rgb_frame->data, rgb_frame->linesize);

                clock_gettime(CLOCK_MONOTONIC, &convert_frame_end);
                convert_frame_total_time += (convert_frame_end.tv_sec - convert_frame_start.tv_sec) + (convert_frame_end.tv_nsec - convert_frame_start.tv_nsec) / 1e9;

                // Lock the buffer and write frame to it
                pthread_mutex_lock(&buffer_mutex);

                while (frame_buffer[current_buffer][buffer_write_index].is_ready) {
                    pthread_cond_wait(&buffer_cond, &buffer_mutex);
                }

                // Cache grayscale values
                clock_gettime(CLOCK_MONOTONIC, &cache_start);

                frame_buffer[current_buffer][buffer_write_index].cached_img = cached_image_pool[pool_index];
                cache_grayscale_values(rgb_frame->data[0], pCodecContext->width, pCodecContext->height, frame_buffer[current_buffer][buffer_write_index].cached_img);

                clock_gettime(CLOCK_MONOTONIC, &cache_end);
                cache_total_time += (cache_end.tv_sec - cache_start.tv_sec) + (cache_end.tv_nsec - cache_start.tv_nsec) / 1e9;

                frame_buffer[current_buffer][buffer_write_index].frame = frame_pool[pool_index];
                frame_buffer[current_buffer][buffer_write_index].is_ready = 1;

                buffer_write_index = (buffer_write_index + 1) % BUFFER_POOL_SIZE;

                // Switch the buffer after filling all slots
                if (buffer_write_index == 0) {
                    current_buffer = 1 - current_buffer;
                }

                pthread_cond_signal(&buffer_cond);
                pthread_mutex_unlock(&buffer_mutex);

                pool_index = (pool_index + 1) % BUFFER_POOL_SIZE;

                clock_gettime(CLOCK_MONOTONIC, &receive_frame_end);
                receive_frame_total_time += (receive_frame_end.tv_sec - receive_frame_start.tv_sec) + (receive_frame_end.tv_nsec - receive_frame_start.tv_nsec) / 1e9;

            }

            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                fprintf(stderr, "Error receiving frame from decoder: %s\n", av_err2str(ret));
            }
        }
        av_packet_unref(packet);

        clock_gettime(CLOCK_MONOTONIC, &producer_end_time);  // End profiling
        producer_total_time += (producer_end_time.tv_sec - producer_start_time.tv_sec) + (producer_end_time.tv_nsec - producer_start_time.tv_nsec) / 1e9;
        producer_frame_count++;
    }

    pthread_mutex_lock(&buffer_mutex);
    is_done = true; // Set the flag to indicate producer is done
    pthread_cond_broadcast(&buffer_cond); // Notify all waiting consumers
    pthread_mutex_unlock(&buffer_mutex);

    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(sws_ctx);

    // Store granular profiling results
    producer_read_frame_total_time = read_frame_total_time;
    producer_send_packet_total_time = send_packet_total_time;
    producer_receive_frame_total_time = receive_frame_total_time;
    producer_convert_frame_total_time = convert_frame_total_time;
    producer_cache_total_time = cache_total_time;

    pthread_exit(NULL);
}

// Consumer thread function: Renders frames to terminal
void *frame_consumer(void *args) {
    ConsumerArgs *cons_args = (ConsumerArgs *)args;
    int pCodecContext_width = cons_args->pCodecContext_width;
    int pCodecContext_height = cons_args->pCodecContext_height;

    struct timespec previous_time, current_time;
    double total_elapsed_time = 0.0;
    int frame_count = 0;
    const int fps_calculation_window = 10;  // Calculate FPS every 10 frames

    // Profiling variables for different consumer stages
    double lock_wait_total = 0.0;
    double render_total = 0.0;
    double buffer_update_total = 0.0;

    // Start time for FPS calculation
    clock_gettime(CLOCK_MONOTONIC, &previous_time);

    int current_buffer = 0; // 0 for Buffer A, 1 for Buffer B

    while (is_running && !terminated) {
        struct timespec stage_start, stage_end;

        // Stage 1: Lock and wait for frame availability
        clock_gettime(CLOCK_MONOTONIC, &stage_start);

        pthread_mutex_lock(&buffer_mutex);

        // Wait until there's a frame ready to consume or the producer signals completion
        while (!frame_buffer[current_buffer][buffer_read_index].is_ready && !is_done) {
            pthread_cond_wait(&buffer_cond, &buffer_mutex);
        }

        // If producer is done and there are no more frames left, exit the loop
        if (is_done && !frame_buffer[current_buffer][buffer_read_index].is_ready) {
            pthread_mutex_unlock(&buffer_mutex);
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &stage_end);
        lock_wait_total += (stage_end.tv_sec - stage_start.tv_sec) + (stage_end.tv_nsec - stage_start.tv_nsec) / 1e9;

        // Stage 2: Render the frame
        clock_gettime(CLOCK_MONOTONIC, &stage_start);

        // Consume the frame
        CachedPixel *cached_img = frame_buffer[current_buffer][buffer_read_index].cached_img;

        // Render the frame to the terminal
        int term_rows, term_cols;
        get_terminal_size(&term_rows, &term_cols);

        render_ascii_art_terminal(cached_img, pCodecContext_width, pCodecContext_height, term_rows, term_cols,
                                  ASCII_CHARS_DEFAULT, ascii_map_size_default);

        clock_gettime(CLOCK_MONOTONIC, &stage_end);
        render_total += (stage_end.tv_sec - stage_start.tv_sec) + (stage_end.tv_nsec - stage_start.tv_nsec) / 1e9;

        // Stage 3: Update buffer indices
        clock_gettime(CLOCK_MONOTONIC, &stage_start);

        // Mark frame as consumed and update the read index
        frame_buffer[current_buffer][buffer_read_index].is_ready = 0;
        buffer_read_index = (buffer_read_index + 1) % BUFFER_POOL_SIZE;

        // Switch buffer after consuming all slots
        if (buffer_read_index == 0) {
            current_buffer = 1 - current_buffer;
        }

        pthread_cond_signal(&buffer_cond);
        pthread_mutex_unlock(&buffer_mutex);

        clock_gettime(CLOCK_MONOTONIC, &stage_end);
        buffer_update_total += (stage_end.tv_sec - stage_start.tv_sec) + (stage_end.tv_nsec - stage_start.tv_nsec) / 1e9;

        // End time for FPS calculation
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double frame_elapsed_time =
                (current_time.tv_sec - previous_time.tv_sec) + (current_time.tv_nsec - previous_time.tv_nsec) / 1e9;

        previous_time = current_time;

        // Accumulate total elapsed time and count frames
        total_elapsed_time += frame_elapsed_time;
        frame_count++;

        // Profiling
        consumer_frame_count++;

        // Every `fps_calculation_window` frames, calculate and display FPS
        if (frame_count % fps_calculation_window == 0) {
            double avg_fps = fps_calculation_window / total_elapsed_time;
            double avg_frame_delay = (total_elapsed_time / fps_calculation_window) * 1000.0;  // in milliseconds

            printf(" | FPS: %.2f | Frame delay: %.2f ms\n", avg_fps, avg_frame_delay);
            fflush(stdout);

            // Reset for the next calculation window
            total_elapsed_time = 0.0;
            frame_count = 0;
        }
    }

    // Update global profiling variables for consumer stages
    consumer_lock_wait_total += lock_wait_total;
    consumer_render_total += render_total;
    consumer_buffer_update_total += buffer_update_total;

    pthread_exit(NULL);
}



void print_profiling_results() {
    if (producer_frame_count > 0) {
        printf("Average Producer Time per Frame: %.6f seconds\n", producer_total_time / producer_frame_count);
        printf("Producer Profiling Breakdown:\n");
        printf(" - Average Read Frame Time per Frame: %.6f seconds\n", producer_read_frame_total_time / producer_frame_count);
        printf(" - Average Send Packet Time per Frame: %.6f seconds\n", producer_send_packet_total_time / producer_frame_count);
        printf(" - Average Receive Frame Time per Frame: %.6f seconds\n", producer_receive_frame_total_time / producer_frame_count);
        printf(" - Average Convert Frame Time per Frame: %.6f seconds\n", producer_convert_frame_total_time / producer_frame_count);
        printf(" - Average Cache Time per Frame: %.6f seconds\n", producer_cache_total_time / producer_frame_count);
    } else {
        printf("No frames produced.\n");
    }

    if (consumer_frame_count > 0) {
        printf("Average Consumer Time per Frame: %.6f seconds\n", consumer_total_time / consumer_frame_count);
        printf("Consumer Profiling Breakdown:\n");
        printf(" - Average Lock & Wait Time per Frame: %.6f seconds\n", consumer_lock_wait_total / consumer_frame_count);
        printf(" - Average Render Time per Frame: %.6f seconds\n", consumer_render_total / consumer_frame_count);
        printf(" - Average Buffer Update Time per Frame: %.6f seconds\n", consumer_buffer_update_total / consumer_frame_count);
    } else {
        printf("No frames consumed.\n");
    }
}

// Signal handler function
void handle_sigint(int sig) {
    (void)sig;  // Suppress unused parameter warning
    terminated = true;
    print_profiling_results();  // Print whatever profiling data we have so far
    // Optionally add a clean exit here if needed
    exit(0);
}

void setup_signal_handler() {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error setting up SIGINT handler");
        exit(1);
    }
}

void process_video(const char *filename) {
    AVFormatContext *pFormatContext = NULL;
    AVCodecContext *pCodecContext = NULL;
    int video_stream_index = -1;

    // Initialize FFmpeg and open the input video file
    if (init_ffmpeg(filename, &pFormatContext, &pCodecContext, &video_stream_index) != 0) {
        // Initialization failed, exit the function
        return;
    }

    // Print additional video information
    const char *extension = strrchr(filename, '.');
    if (!extension) extension = "unknown";

    // Prepare render context for video
    double fps = av_q2d(pFormatContext->streams[video_stream_index]->r_frame_rate);
    double frame_delay = 1000.0 / fps;

    clear_terminal();
    printf("Video Extension: %s\n", extension);
    printf("Target FPS: %.2f\n", fps);
    printf("Frame Time (ms): %.2f\n", frame_delay);
    printf("Input Pixel Format: %s\n", av_get_pix_fmt_name(pCodecContext->pix_fmt));
    fflush(stdout);

    // Create producer and consumer threads
    pthread_t producer_thread, consumer_thread;

    // Prepare arguments for the producer and consumer
    ProducerArgs producer_args = {
        .pFormatContext = pFormatContext,
        .pCodecContext = pCodecContext,
        .video_stream_index = video_stream_index
    };

    ConsumerArgs consumer_args = {
        .pCodecContext_width = pCodecContext->width,
        .pCodecContext_height = pCodecContext->height,
        .fps = fps
    };

    // Allocate frames and buffers for the pool
    for (int i = 0; i < BUFFER_POOL_SIZE; ++i) {
        // Frame allocation
        frame_pool[i] = av_frame_alloc();
        if (!frame_pool[i]) {
            fprintf(stderr, "Failed to allocate frame for pool\n");
            exit(1);
        }

        // Buffer allocation
        buffer_pool[i] = (uint8_t *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height, 32));
        if (!buffer_pool[i]) {
            fprintf(stderr, "Failed to allocate buffer for pool\n");
            exit(1);
        }

        // Cached image pool allocation
        cached_image_pool[i] = (CachedPixel *)malloc(pCodecContext->width * pCodecContext->height * sizeof(CachedPixel));
        if (!cached_image_pool[i]) {
            fprintf(stderr, "Failed to allocate cached image for pool\n");
            exit(1);
        }
    }

    // Create producer and consumer threads
    pthread_create(&producer_thread, NULL, frame_producer, &producer_args);
    pthread_create(&consumer_thread, NULL, frame_consumer, &consumer_args);

    // Wait for producer and consumer threads to finish
    pthread_join(producer_thread, NULL);
    pthread_join(consumer_thread, NULL);

    // Print profiling results after both threads finish
    print_profiling_results();

    // Cleanup FFmpeg resources
    avcodec_free_context(&pCodecContext);
    avformat_close_input(&pFormatContext);

    // Free frame pool and buffer pool
    for (int i = 0; i < BUFFER_POOL_SIZE; ++i) {
        if (frame_pool[i]) {
            av_frame_free(&frame_pool[i]);
            frame_pool[i] = NULL; // Set pointer to NULL after freeing
        }

        if (buffer_pool[i]) {
            av_free(buffer_pool[i]);
            buffer_pool[i] = NULL; // Set pointer to NULL after freeing
        }

        if (cached_image_pool[i]) {
            free(cached_image_pool[i]);
            cached_image_pool[i] = NULL; // Set pointer to NULL after freeing
        }
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
    setup_signal_handler();
    CachedPixel *cached_img = NULL;
    unsigned char *img = NULL;

    if (argc < 2) {
        printf("Usage: %s <image file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];

    // Check if the input is a video file
    if (is_video_file(filename)) {
        process_video(filename);  // Call the simplified video processing function
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
//    cached_img = cache_grayscale_values(img, img_width, img_height);
//    if (!cached_img) {
//        fprintf(stderr, "Error: Failed to cache grayscale values.\n");
//        stbi_image_free(img);
//        return 1;
//    }

    // Let user choose character set for rendering
    char input_buffer[10];
    int choice = 0;
    printf("Choose character set for rendering:\n");
    printf("1. Default ASCII ( .:-=+*#%%@ )\n");
    printf("2. Extended ASCII ( . .. :;; IIl .... @ etc.)\n");
    printf("3. Block characters ( ▁▂▃▄▅▆▇█ )\n");
    printf("Enter your choice (1/2/3): ");

    if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
        choice = (int)strtol(input_buffer, NULL, 10);
    }

    const char *char_set;
    int char_set_size;
//    bool is_block = false;

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
    int output_mode = 0;
    printf("Choose output mode:\n");
    printf("1. Terminal\n");
    printf("2. PNG\n");
    printf("3. TXT\n");
    printf("Enter your choice (1/2/3): ");

    if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
        output_mode = (int)strtol(input_buffer, NULL, 10);
    }

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
//        render_ascii_art_terminal(cached_img, img_width, img_height, term_rows, term_cols, char_set, char_set_size);

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
//                render_ascii_art_terminal(cached_img, img_width, img_height, term_rows, term_cols, char_set, char_set_size);
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
        float scale_factor = 1;
        printf("Enter a scale factor (e.g., 0.5 for half size, 1 for original size, 2 for double size): ");

        if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
            scale_factor = strtof(input_buffer, NULL);
        }

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
    } else {
        char output_filename[256];
        generate_output_filename(filename, output_filename, "txt");

        int term_rows = 0;
        int term_cols = 0;
        get_terminal_size(&term_rows, &term_cols);

        render_ascii_art_file_txt(cached_img, img_width, img_height, char_set, char_set_size, output_filename,
                                  term_rows, term_cols);
        print_memory_usage();
    }

    // Free memory
    free(cached_img);
    stbi_image_free(img);

    return 0;
}
