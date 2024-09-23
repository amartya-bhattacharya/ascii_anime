#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <termios.h>
#include <fcntl.h>

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

// Adaptive sleep duration function
void adaptive_sleep(int *current_sleep, bool recently_resized) {
    if (recently_resized) {
        *current_sleep = *current_sleep > 20000 ? *current_sleep - 10000 : *current_sleep;
    } else {
        *current_sleep = *current_sleep < 200000 ? *current_sleep + 10000 : *current_sleep;
    }
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
    printf("\033[3J");  // Clear the entire terminal scrollback buffer
    printf("\033[H");   // Move the cursor to the top left
    printf("\033[2J");  // Clear the entire screen
    fflush(stdout);     // Ensure the commands are flushed to the terminal
}

// Modify print function to move cursor back to the beginning instead of clearing
void render_ascii_art(CachedPixel *cached_img, int img_width, int img_height, int term_rows, int term_cols, const char *char_set, int char_set_size) {
    float char_aspect_ratio = 2.0;
    int target_width, target_height;

    term_rows -= debug_lines;  // Reduce available rows for rendering
    float img_aspect_ratio = (float)img_width / img_height;

    // Calculate the target dimensions to preserve aspect ratio
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

    // Render the ASCII art
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

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <image file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    int img_width, img_height, channels;

    unsigned char *img = stbi_load(filename, &img_width, &img_height, &channels, 3);
    if (!img) {
        printf("Failed to load image: %s\n", filename);
        return 1;
    }

    printf("Image loaded: %s (%dx%d)\n", filename, img_width, img_height);

    // Let user choose character set for rendering
    int choice;
    printf("Choose character set for rendering:\n");
    printf("1. Default ASCII ( .:-=+*#%%@ )\n");
    printf("2. Extended ASCII ( . .. :;; IIl .... @ etc.)\n");
    printf("3. Block characters ( ▁▂▃▄▅▆▇█ ) [broken right now..]\n");
    printf("Enter your choice (1/2/3): ");
    scanf("%d", &choice);

    const char *char_set;
    int char_set_size;

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
            break;
        default:
            printf("Invalid choice, using default ASCII set.\n");
            char_set = ASCII_CHARS_DEFAULT;
            char_set_size = ascii_map_size_default;
            break;
    }

    // Set up the SIGWINCH signal handler for terminal resize
    struct sigaction sa;
    sa.sa_handler = handle_resize;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);

    int term_rows, term_cols;
    get_terminal_size(&term_rows, &term_cols);

    // Cache pixel values
    CachedPixel* cached_img = cache_grayscale_values(img, img_width, img_height);

    // Initial render
    clear_terminal();
    render_ascii_art(cached_img, img_width, img_height, term_rows, term_cols, char_set, char_set_size);

    // Set the terminal to non-blocking input mode
    set_nonblocking_input();

    // Main loop to handle live re-rendering on terminal resize or 'q' press
    while (true) {
        if (resized) {
            get_terminal_size(&term_rows, &term_cols);
            clear_terminal();
            render_ascii_art(cached_img, img_width, img_height, term_rows, term_cols, char_set, char_set_size);
            resized = false;
        }


        // Check if 'q' has been pressed to quit
        char c;
        if (read(STDIN_FILENO, &c, 1) > 0 && c == 'q') {
            break;
        }

//        adaptive_sleep(&sleep_duration, resized);
//        usleep(1);  // Sleep for 100ms to avoid excessive CPU usage
    }

    // Reset terminal input mode before exiting
    reset_input_mode();

    stbi_image_free(img);
    return 0;
}
