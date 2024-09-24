#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION

#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_truetype.h"
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <termios.h>
#include <time.h>
#include <sys/resource.h>

#define FONT_SIZE 16  // Font size for rendering
#define FONT_PATH "Topaz-8.ttf" // Path to the Topaz-8 font file

// stb_truetype font buffer
unsigned char ttf_buffer[1<<20];
stbtt_fontinfo font;
unsigned char *font_bitmap;

void init_font(const char *font_path) {
    FILE *font_file = fopen(font_path, "rb");
    if (font_file) {
        fread(ttf_buffer, 1, 1 << 20, font_file);
        fclose(font_file);
        stbtt_InitFont(&font, ttf_buffer, stbtt_GetFontOffsetForIndex(ttf_buffer, 0));
    } else {
        printf("Failed to load font: %s\n", font_path);
    }
}

// Default ASCII character set
const char *ASCII_CHARS_DEFAULT = " .:-=+*#%@";
int ascii_map_size_default = 10;

// Extended ASCII character set
const char *ASCII_CHARS_EXTENDED = " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
int ascii_map_size_extended = 70;

// Block characters for increased granularity
const char *BLOCK_CHARS = "▁▂▃▄▅▆▇█";
int block_map_size = 8;

// Add a new struct to store pixel grayscale information
typedef struct {
    int gray_value;
    int r, g, b;
} CachedPixel;

static volatile bool resized = false;
const int debug_lines = 4;  // Number of lines to print after image

// Function to handle terminal resize events
void handle_resize(int sig) {
    resized = true;  // Set a flag to indicate the terminal has resized
}

// Function to map pixel intensity to an ASCII character or block character
static inline char get_ascii_char(int r, int g, int b, const char *char_set, int char_set_size) {
    int gray = 0.299 * r + 0.587 * g + 0.114 * b;
    return char_set[(gray * (char_set_size - 1)) / 255];
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

// Modify print function to move cursor back to the beginning instead of clearing
void render_ascii_art_terminal(CachedPixel *cached_img, int img_width, int img_height, int term_rows, int term_cols, const char *char_set, int char_set_size) {
    float char_aspect_ratio = 2.0;
    int target_width, target_height;

    term_rows -= debug_lines;
    float img_aspect_ratio = (float)img_width / img_height;

    if (img_width > img_height) {
        target_width = term_cols;
        target_height = target_width / img_aspect_ratio / char_aspect_ratio;
        if (target_height > term_rows) {
            target_height = term_rows;
            target_width = target_height * img_aspect_ratio * char_aspect_ratio;
        }
    } else {
        target_height = term_rows;
        target_width = target_height * img_aspect_ratio * char_aspect_ratio;
        if (target_width > term_cols) {
            target_width = term_cols;
            target_height = target_width / img_aspect_ratio / char_aspect_ratio;
        }
    }

    // Clear terminal and move the cursor to the top before every render
    clear_terminal();
    printf("\033[H");  // Move cursor to the top before starting rendering

    for (int y = 0; y < target_height; y++) {
        for (int x = 0; x < target_width; x++) {
            int img_x = x * img_width / target_width;
            int img_y = y * img_height / target_height;

            CachedPixel pixel = cached_img[img_y * img_width + img_x];
            char ascii_char = get_ascii_char(pixel.r, pixel.g, pixel.b, char_set, char_set_size);
            print_colored_char(ascii_char, pixel.r, pixel.g, pixel.b);
        }
        printf("\033[0m\n");  // Reset color after each line
    }

    // Print the debug information at the bottom
    float original_ar = (float)img_width / img_height;
    float new_ar = (float)target_width / target_height;
    float term_ar = (float)term_cols / term_rows;

    printf("\nOriginal: %dx%d (AR: %.2f) | New: %dx%d (AR: %.2f) | Term: %dx%d (AR: %.2f)\n",
           img_width, img_height, original_ar, target_width, target_height, new_ar, term_cols, term_rows + debug_lines, term_ar);
    fflush(stdout);
}

// Function to render ASCII characters using stb_truetype
void render_ascii_to_image(unsigned char *output_img, int x, int y, char ascii_char, int img_width, int img_height, int scale, int r, int g, int b) {
    int width, height, x_offset, y_offset;
    float scale_factor = stbtt_ScaleForPixelHeight(&font, scale);

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
void render_ascii_art_file_scaled(unsigned char *img, int img_width, int img_height, const char *char_set, int char_set_size, const char *output_file, float scale_factor, int font_scale) {
    // Calculate scaled dimensions while keeping the aspect ratio correct
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
        output_img[i] = 0;      // Red
        output_img[i + 1] = 0;  // Green
        output_img[i + 2] = 0;  // Blue
        output_img[i + 3] = 255; // Alpha (fully opaque)
    }

    // Iterate through each pixel and render ASCII characters
    for (int y = 0; y < scaled_height; y += font_scale) {
        for (int x = 0; x < scaled_width; x += font_scale) {
            int img_x = (int)(x / scale_factor);
            int img_y = (int)(y / scale_factor);

            // Ensure that we're still within bounds of the input image
            if (img_x < img_width && img_y < img_height) {
                int index = (img_y * img_width + img_x) * 3;
                int r = img[index];
                int g = img[index + 1];
                int b = img[index + 2];

                char ascii_char = get_ascii_char(r, g, b, char_set, char_set_size);

                // Render the ASCII character at the correct position
                render_ascii_to_image(output_img, x, y, ascii_char, scaled_width, scaled_height, font_scale, r, g, b);
            }
        }
    }

    // Save the output image as PNG
    stbi_write_png(output_file, scaled_width, scaled_height, 4, output_img, scaled_width * 4);

    free(output_img);
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
void generate_output_filename(const char *input_filename, char *output_filename) {
    // Find the position of the last dot (.) in the input filename
    const char *dot = strrchr(input_filename, '.');
    size_t len = dot ? (size_t)(dot - input_filename) : strlen(input_filename);

    // Copy the base input filename into the output filename and append "-ascii.png"
    strncpy(output_filename, input_filename, len);
    strcpy(output_filename + len, "-ascii.png");
}

void print_memory_usage() {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
//    printf("Memory usage: %ld KB\n", usage.ru_maxrss);  // Outputs in kilobytes
    // output size in megabytes
     printf("Memory usage: %ld MB\n", usage.ru_maxrss / 1024);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <image file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    int img_width, img_height, channels;
    clock_t start_time = clock();
    unsigned char *img = stbi_load(filename, &img_width, &img_height, &channels, 3);
    if (!img) {
        printf("Failed to load image: %s\n", filename);
        return 1;
    }

//    printf("Image loaded: %s (%dx%d)\n", filename, img_width, img_height);

    clock_t end_time = clock();
    double load_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;
    printf("Image loaded in %.2f seconds\n", load_time);

    print_memory_usage();

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
            is_block = true;  // Mark as using block characters
            break;
        default:
            printf("Invalid choice, using default ASCII set.\n");
            char_set = ASCII_CHARS_DEFAULT;
            char_set_size = ascii_map_size_default;
            break;
    }

    // Let user choose output mode
    printf("Choose output mode:\n");
    printf("1. Terminal\n");
    printf("2. File\n");
    printf("Enter your choice (1/2): ");
    int output_mode;
    scanf("%d", &output_mode);

    // Set up the SIGWINCH signal handler for terminal resize
    struct sigaction sa;
    sa.sa_handler = handle_resize;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);

    int term_rows, term_cols;
    get_terminal_size(&term_rows, &term_cols);

    // Initialize cached pixel array
    CachedPixel* cached_img = cache_grayscale_values(img, img_width, img_height);

    if (output_mode == 1) { // Terminal output mode
        // Prepare terminal
        clear_terminal();
        render_ascii_art_terminal(cached_img, img_width, img_height, term_rows, term_cols, char_set, char_set_size);
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
        start_time = clock();

        char output_filename[256];
        generate_output_filename(filename, output_filename);

        // Let user choose scaling factor
        float scale_factor;
        printf("Enter a scale factor (e.g., 0.5 for half size, 1 for original size, 2 for double size): ");
        scanf("%f", &scale_factor);

        // Load the font
        init_font(FONT_PATH);

        render_ascii_art_file_scaled(img, img_width, img_height, char_set, char_set_size, output_filename, scale_factor, FONT_SIZE);

        // Profiling
        end_time = clock();
        double file_render_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;
        printf("File render time: %.2f seconds\n", file_render_time);
        printf("ASCII art saved to file: %s\n", output_filename);
        print_memory_usage();
    }

    // Free memory
    stbi_image_free(img);
    free(cached_img);

    return 0;
}
