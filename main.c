#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <termios.h>
#include <fcntl.h>

const char *ASCII_CHARS = " .:-=+*#%@";
int ascii_map_size = 10;
static volatile bool resized = false;
const int debug_lines = 3;  // Adjust this to match the number of extra lines you print after the image


// Function to handle the terminal resize event
void handle_resize(int sig) {
    resized = true;  // Set a flag to indicate the terminal has resized
}

// Function to map pixel intensity to an ASCII character
char get_ascii_char(int r, int g, int b) {
    int gray = 0.299 * r + 0.587 * g + 0.114 * b;
    if (gray == 0) return ' ';
    return ASCII_CHARS[(gray * (ascii_map_size - 1)) / 255];
}

// Function to get terminal size
void get_terminal_size(int *rows, int *cols) {
    struct winsize w;
    ioctl(STDIN_FILENO, TIOCGWINSZ, &w);
    *rows = w.ws_row;
    *cols = w.ws_col;
}

// Function to print colored ASCII characters with a black background
void print_colored_char(char ascii_char, int r, int g, int b) {
    printf("\033[48;2;0;0;0m\033[38;2;%d;%d;%dm%c", r, g, b, ascii_char);
}

// Function to clear the terminal
void clear_terminal() {
    printf("\033[2J\033[H");  // Clear the terminal and move the cursor to the top
}

// Function to render the image as ASCII art in the terminal
void render_ascii_art(unsigned char *img, int img_width, int img_height, int term_rows, int term_cols) {
    float char_aspect_ratio = 2.0;
    int target_width, target_height;

    // Adjust terminal height to account for debug information and avoid scrolling
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

    // Clear the terminal for re-drawing
    clear_terminal();

    // Clear the terminal for re-drawing
//    printf("\033[2J\033[H");

    for (int y = 0; y < target_height; y++) {
        for (int x = 0; x < target_width; x++) {
            int img_x = x * img_width / target_width;
            int img_y = y * img_height / target_height;

            int index = (img_y * img_width + img_x) * 3;
            int r = img[index];
            int g = img[index + 1];
            int b = img[index + 2];

            char ascii_char = get_ascii_char(r, g, b);
            print_colored_char(ascii_char, r, g, b);
        }
        printf("\033[0m\n");  // Reset color after each line
    }

    float original_ar = (float)img_width / img_height;
    float new_ar = (float)target_width / target_height;
    float term_ar = (float)term_cols / term_rows;

    printf("\nOriginal: %dx%d (AR: %.2f) | New: %dx%d (AR: %.2f) | Term: %dx%d (AR: %.2f)\n",
           img_width, img_height, original_ar, target_width, target_height, new_ar, term_cols, term_rows, term_ar);
}

// Function to configure terminal to non-blocking input with proper signal handling
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

    // Set up the SIGWINCH signal handler for terminal resize
    struct sigaction sa;
    sa.sa_handler = handle_resize;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);

    int term_rows, term_cols;
    get_terminal_size(&term_rows, &term_cols);

    // Initial render
    render_ascii_art(img, img_width, img_height, term_rows, term_cols);

    // Set the terminal to non-blocking input mode
    set_nonblocking_input();

    // Main loop to handle live re-rendering on terminal resize or 'q' press
    while (true) {
        if (resized) {
            get_terminal_size(&term_rows, &term_cols);
            render_ascii_art(img, img_width, img_height, term_rows, term_cols);
            resized = false;
        }

        // Check if 'q' has been pressed to quit
        char c;
        if (read(STDIN_FILENO, &c, 1) > 0 && c == 'q') {
            break;
        }

        usleep(100000);  // Sleep for 100ms to avoid excessive CPU usage
    }

    // Reset terminal input mode before exiting
    reset_input_mode();

    stbi_image_free(img);
    return 0;
}
