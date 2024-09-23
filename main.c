#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

// ASCII character set used for grayscale mapping (inverted for proper brightness mapping)
const char *ASCII_CHARS = " .:-=+*#%@";
int ascii_map_size = 10;  // Number of ASCII characters

// Function to map pixel intensity to an ASCII character
char get_ascii_char(int r, int g, int b) {
    // Convert RGB to grayscale using weighted average
    int gray = 0.299 * r + 0.587 * g + 0.114 * b;
    // Inverted grayscale mapping: @ for bright, space for black
    if (gray == 0) return ' ';  // Map pure black (RGB 0,0,0) to space
    return ASCII_CHARS[(gray * (ascii_map_size - 1)) / 255];
}

// Function to get terminal size
void get_terminal_size(int *rows, int *cols) {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    *rows = w.ws_row - 2;   // Subtract 2 for the prompt and status bar
    *cols = w.ws_col;
}

// Function to print colored ASCII characters using ANSI escape codes with a black background
void print_colored_char(char ascii_char, int r, int g, int b) {
    // Print the ASCII character in the RGB color with a black background using ANSI escape codes
    printf("\033[48;2;0;0;0m\033[38;2;%d;%d;%dm%c", r, g, b, ascii_char);  // Black background, colored foreground
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <image file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    int width, height, channels;

    // Load the image using stb_image
    unsigned char *img = stbi_load(filename, &width, &height, &channels, 3);  // Force 3 channels (RGB)
    if (!img) {
        printf("Failed to load image: %s\n", filename);
        return 1;
    }

    printf("Image loaded: %s (%dx%d)\n", filename, width, height);

    // Get terminal size
    int term_rows, term_cols;
    get_terminal_size(&term_rows, &term_cols);

    // Adjust for character aspect ratio (2:1 height to width)
    float char_aspect_ratio = 2.0;

    // Variables to store the target width and height
    int target_width, target_height;

    // Calculate aspect ratio of the image
    float img_aspect_ratio = (float)width / height;

    // Landscape orientation (width > height)
    if (width > height) {
        // Cap the width at terminal width
        target_width = term_cols;
        target_height = target_width / img_aspect_ratio / char_aspect_ratio;
        // Ensure height does not exceed terminal height
        if (target_height > term_rows) {
            target_height = term_rows;
            target_width = target_height * img_aspect_ratio * char_aspect_ratio;
        }
    }
    // Portrait orientation (height > width)
    else {
        // Cap the height at terminal height
        target_height = term_rows;
        target_width = target_height * img_aspect_ratio * char_aspect_ratio;
        // Ensure width does not exceed terminal width
        if (target_width > term_cols) {
            target_width = term_cols;
            target_height = target_width / img_aspect_ratio / char_aspect_ratio;
        }
    }

    // Pre-calculate scaling factors to avoid redundant calculations
    float x_scale = (float)width / target_width;
    float y_scale = (float)height / target_height;

    // Render the colored ASCII art directly to the terminal
    for (int y = 0; y < target_height; y++) {
        for (int x = 0; x < target_width; x++) {
            // Scale the coordinates to match the original image size
            int img_x = x * width / target_width;
            int img_y = y * height / target_height;

            // Get pixel data
            int index = (img_y * width + img_x) * 3;
            int r = img[index];
            int g = img[index + 1];
            int b = img[index + 2];

            // Map the pixel to an ASCII character
            char ascii_char = get_ascii_char(r, g, b);

            // Print the ASCII character in the pixel's color with a black background
            print_colored_char(ascii_char, r, g, b);
        }
        printf("\033[0m\n");  // Reset color and background at the end of each line
    }

    // Calculate aspect ratios
    float original_ar = (float)width / height;
    float new_ar = (float)target_width / target_height;
    float term_ar = (float)term_cols / term_rows;

    // Debug printout
    printf("\nOriginal: %dx%d (AR: %.2f) | New: %dx%d (AR: %.2f) | Term: %dx%d (AR: %.2f)\n",
           width, height, original_ar, target_width, target_height, new_ar, term_cols, term_rows, term_ar);

    // Free the image memory
    stbi_image_free(img);

    return 0;
}
