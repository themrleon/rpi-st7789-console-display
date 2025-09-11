#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <bcm2835.h>
#include <time.h>
#include <signal.h>
#include <stddef.h>
#include <sys/time.h>

// GPU acceleration headers
#include <bcm_host.h>
#include <interface/vmcs_host/vc_dispmanx.h>
#include <interface/vctypes/vc_image_types.h>

// Display dimensions
#define WIDTH 320
#define HEIGHT 170
#define DISPLAY_SIZE (WIDTH * HEIGHT)
#define DISPLAY_BYTES (DISPLAY_SIZE * 2)

// Display offset - ADJUSTED FOR YOUR DISPLAY
#define COL_OFFSET 0
#define ROW_OFFSET 35

// Interlacing settings - COMPILE-TIME CONFIGURATION
#define INTERLACE_ENABLED 0  // Set to 1 to enable interlacing, 0 to disable
#define INTERLACE_EVERY 2    // Every Nth line will be black (2 = every other line)

// GPIO pins
#define DC_PIN RPI_GPIO_P1_18  // GPIO 24
#define RST_PIN RPI_GPIO_P1_22 // GPIO 25
#define CS_PIN RPI_GPIO_P1_24  // GPIO 8 (CE0)

// SPI settings
#define SPI_SPEED 32000000  // 32 MHz

// Update settings
#define CHANGE_THRESHOLD 5    // Percentage of pixels that must change to trigger update
#define MIN_UPDATE_REGION 10  // Minimum region size to update

// Global variables
volatile sig_atomic_t keep_running = 1;
DISPMANX_DISPLAY_HANDLE_T display_handle = 0;
DISPMANX_RESOURCE_HANDLE_T resource_handle = 0;
VC_RECT_T rect;

// Previous frame buffer
uint16_t *prev_frame = NULL;

// Function prototypes
void init_gpio(void);
void init_spi(void);
void init_display(void);
void write_command(uint8_t cmd);
void write_data(uint8_t data);
void write_data_len(const uint8_t *data, uint32_t len);
void set_window(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end);
int init_gpu_resources(void);
void display_framebuffer_smart_update(void);
void cleanup(void);
void signal_handler(int sig);
uint16_t fix_color_format(uint16_t color);
int detect_changed_regions(uint16_t *current_frame, uint16_t *update_mask);
void update_changed_regions(uint16_t *current_frame, uint16_t *update_mask);
void apply_interlacing(uint16_t *frame);
void update_interlaced_regions(uint16_t *current_frame, uint16_t *update_mask);

// Signal handler for clean exit
void signal_handler(int sig) {
    keep_running = 0;
}

// Fix color format for 16-bit RGB565
uint16_t fix_color_format(uint16_t color) {
    // Simple byte swap
    return ((color & 0xFF) << 8) | (color >> 8);
}

// Apply interlacing to frame - every other line becomes black
void apply_interlacing(uint16_t *frame) {
    #if INTERLACE_ENABLED
    for (int y = 0; y < HEIGHT; y++) {
        if (y % INTERLACE_EVERY == 1) {  // Make every other line black
            for (int x = 0; x < WIDTH; x++) {
                frame[y * WIDTH + x] = 0x0000;  // Black
            }
        }
    }
    #endif
}

// Initialize GPIO
void init_gpio(void) {
    if (!bcm2835_init()) {
        printf("Failed to initialize BCM2835\n");
        exit(1);
    }
    
    bcm2835_gpio_fsel(DC_PIN, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(RST_PIN, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_fsel(CS_PIN, BCM2835_GPIO_FSEL_OUTP);
    
    bcm2835_gpio_write(CS_PIN, HIGH);
}

// Initialize SPI with optimal settings
void init_spi(void) {
    bcm2835_spi_begin();
    bcm2835_spi_setBitOrder(BCM2835_SPI_BIT_ORDER_MSBFIRST);
    bcm2835_spi_setDataMode(BCM2835_SPI_MODE0);
    bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_16);
    bcm2835_spi_chipSelect(BCM2835_SPI_CS0);
    bcm2835_spi_setChipSelectPolarity(BCM2835_SPI_CS0, LOW);
}

// Write command to display
void write_command(uint8_t cmd) {
    bcm2835_gpio_write(DC_PIN, LOW);
    bcm2835_gpio_write(CS_PIN, LOW);
    bcm2835_spi_transfer(cmd);
    bcm2835_gpio_write(CS_PIN, HIGH);
}

// Write data to display (single byte)
void write_data(uint8_t data) {
    bcm2835_gpio_write(DC_PIN, HIGH);
    bcm2835_gpio_write(CS_PIN, LOW);
    bcm2835_spi_transfer(data);
    bcm2835_gpio_write(CS_PIN, HIGH);
}

// Write multiple data bytes
void write_data_len(const uint8_t *data, uint32_t len) {
    bcm2835_gpio_write(DC_PIN, HIGH);
    bcm2835_gpio_write(CS_PIN, LOW);
    
    // Transfer data
    bcm2835_spi_writenb((char*)data, len);
    
    bcm2835_gpio_write(CS_PIN, HIGH);
}

// Set display window for a specific region
void set_window(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end) {
    // Apply offsets
    x_start += COL_OFFSET;
    x_end += COL_OFFSET;
    y_start += ROW_OFFSET;
    y_end += ROW_OFFSET;
    
    // Column address set
    write_command(0x2A);
    write_data(x_start >> 8);
    write_data(x_start & 0xFF);
    write_data(x_end >> 8);
    write_data(x_end & 0xFF);
    
    // Row address set
    write_command(0x2B);
    write_data(y_start >> 8);
    write_data(y_start & 0xFF);
    write_data(y_end >> 8);
    write_data(y_end & 0xFF);
    
    // Memory write
    write_command(0x2C);
}

// Initialize display with optimized command sequence
void init_display(void) {
    // Reset display
    bcm2835_gpio_write(RST_PIN, LOW);
    bcm2835_delay(100);
    bcm2835_gpio_write(RST_PIN, HIGH);
    bcm2835_delay(100);
    
    // Send initialization commands
    write_command(0x01);  // SWRESET
    bcm2835_delay(120);
    
    write_command(0x11);  // Sleep Out
    bcm2835_delay(120);
    
    write_command(0x3A);  // Color Mode
    write_data(0x55);     // 16-bit (RGB565)
    
    // MADCTL - Try different values
    write_command(0x36);  // MADCTL
    write_data(0x60);     // 270° rotation (landscape) - MY=1,MX=1,MV=1
    
    write_command(0x21);  // Display Inversion On
    
    // Set column address with proper window
    write_command(0x2A);
    write_data(COL_OFFSET >> 8);
    write_data(COL_OFFSET & 0xFF);
    write_data((COL_OFFSET + WIDTH - 1) >> 8);
    write_data((COL_OFFSET + WIDTH - 1) & 0xFF);
    
    // Set row address with proper window
    write_command(0x2B);
    write_data(ROW_OFFSET >> 8);
    write_data(ROW_OFFSET & 0xFF);
    write_data((ROW_OFFSET + HEIGHT - 1) >> 8);
    write_data((ROW_OFFSET + HEIGHT - 1) & 0xFF);
    
    write_command(0x29);  // Display ON
    bcm2835_delay(100);
    
    // Clear display to check alignment
    set_window(0, 0, WIDTH-1, HEIGHT-1);
    uint16_t black = 0x0000;
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        write_data(black >> 8);
        write_data(black & 0xFF);
    }
}

// Initialize GPU resources
int init_gpu_resources(void) {
    bcm_host_init();
    
    // Get display size
    DISPMANX_MODEINFO_T display_info;
    display_handle = vc_dispmanx_display_open(0);
    if (display_handle == 0) {
        printf("Failed to open display\n");
        return 0;
    }
    
    if (vc_dispmanx_display_get_info(display_handle, &display_info) != 0) {
        printf("Failed to get display info\n");
        return 0;
    }
    
    printf("Display size: %dx%d\n", display_info.width, display_info.height);
    
    #if INTERLACE_ENABLED
    printf("Interlacing: ENABLED (every %d lines)\n", INTERLACE_EVERY);
    #else
    printf("Interlacing: DISABLED\n");
    #endif
    
    // Create resource
    uint32_t vc_image_ptr;
    resource_handle = vc_dispmanx_resource_create(
        VC_IMAGE_RGB565,
        WIDTH, 
        HEIGHT, 
        &vc_image_ptr
    );
    
    if (resource_handle == 0) {
        printf("Failed to create dispmanx resource\n");
        return 0;
    }
    
    // Set up rectangle
    vc_dispmanx_rect_set(&rect, 0, 0, WIDTH, HEIGHT);
    
    // Allocate previous frame buffer
    prev_frame = malloc(DISPLAY_BYTES);
    if (!prev_frame) {
        printf("Failed to allocate previous frame buffer\n");
        return 0;
    }
    memset(prev_frame, 0, DISPLAY_BYTES);
    
    return 1;
}

// Detect changed regions between frames
int detect_changed_regions(uint16_t *current_frame, uint16_t *update_mask) {
    int changed_pixels = 0;
    
    // Simple diff - mark changed pixels
    for (int i = 0; i < DISPLAY_SIZE; i++) {
        if (current_frame[i] != prev_frame[i]) {
            update_mask[i] = 1;
            changed_pixels++;
        } else {
            update_mask[i] = 0;
        }
    }
    
    // Calculate change percentage
    float change_percent = (changed_pixels * 100.0f) / DISPLAY_SIZE;
    
    // If too many changes, just update the whole screen
    if (change_percent > CHANGE_THRESHOLD) {
        return 1; // Full update
    }
    
    return 0; // Partial update
}

// Update only the changed regions with interlacing support
void update_changed_regions(uint16_t *current_frame, uint16_t *update_mask) {
    // For simplicity, we'll find the bounding box of changed areas
    // and update rectangular regions
    
    int min_x = WIDTH, max_x = 0;
    int min_y = HEIGHT, max_y = 0;
    int changed_areas = 0;
    
    // Find bounding box of changes
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int idx = y * WIDTH + x;
            if (update_mask[idx]) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                changed_areas++;
            }
        }
    }
    
    // If we found a reasonable changed area, update it
    if (changed_areas > MIN_UPDATE_REGION && 
        (max_x - min_x) > 2 && (max_y - min_y) > 2) {
        
        // Set window to changed region
        set_window(min_x, min_y, max_x, max_y);
        
        // Extract and send only the changed region
        int region_width = max_x - min_x + 1;
        int region_height = max_y - min_y + 1;
        int region_size = region_width * region_height;
        
        uint16_t *region_buffer = malloc(region_size * 2);
        if (!region_buffer) {
            // Fallback to full update if memory allocation fails
            set_window(0, 0, WIDTH-1, HEIGHT-1);
            write_data_len((uint8_t*)current_frame, DISPLAY_BYTES);
            return;
        }
        
        // Copy changed region
        for (int y = min_y; y <= max_y; y++) {
            for (int x = min_x; x <= max_x; x++) {
                int src_idx = y * WIDTH + x;
                int dst_idx = (y - min_y) * region_width + (x - min_x);
                region_buffer[dst_idx] = current_frame[src_idx];
            }
        }
        
        // Send region data
        write_data_len((uint8_t*)region_buffer, region_size * 2);
        free(region_buffer);
        
        printf("Partial update: Region %d,%d to %d,%d (%d pixels)\n", 
               min_x, min_y, max_x, max_y, region_size);
    } else if (changed_areas > 0) {
        // Small changes, do full update for simplicity
        set_window(0, 0, WIDTH-1, HEIGHT-1);
        write_data_len((uint8_t*)current_frame, DISPLAY_BYTES);
    }
    // Else: no changes, no update needed
}

// Update interlaced regions - optimized for speed
void update_interlaced_regions(uint16_t *current_frame, uint16_t *update_mask) {
    #if INTERLACE_ENABLED
    // For interlaced mode, we update non-black lines only
    int min_x = WIDTH, max_x = 0;
    int min_y = HEIGHT, max_y = 0;
    int changed_areas = 0;
    
    // Find bounding box of changes, skipping black lines
    for (int y = 0; y < HEIGHT; y++) {
        // Skip black lines in interlaced mode
        if (y % INTERLACE_EVERY == 1) continue;
        
        for (int x = 0; x < WIDTH; x++) {
            int idx = y * WIDTH + x;
            if (update_mask[idx]) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                changed_areas++;
            }
        }
    }
    
    // If we found a reasonable changed area, update it
    if (changed_areas > MIN_UPDATE_REGION && 
        (max_x - min_x) > 2 && (max_y - min_y) > 2) {
        
        // Set window to changed region
        set_window(min_x, min_y, max_x, max_y);
        
        // Extract and send only the changed region
        int region_width = max_x - min_x + 1;
        int region_height = max_y - min_y + 1;
        int region_size = region_width * region_height;
        
        uint16_t *region_buffer = malloc(region_size * 2);
        if (!region_buffer) {
            // Fallback to full update if memory allocation fails
            set_window(0, 0, WIDTH-1, HEIGHT-1);
            write_data_len((uint8_t*)current_frame, DISPLAY_BYTES);
            return;
        }
        
        // Copy changed region
        for (int y = min_y; y <= max_y; y++) {
            for (int x = min_x; x <= max_x; x++) {
                int src_idx = y * WIDTH + x;
                int dst_idx = (y - min_y) * region_width + (x - min_x);
                region_buffer[dst_idx] = current_frame[src_idx];
            }
        }
        
        // Send region data
        write_data_len((uint8_t*)region_buffer, region_size * 2);
        free(region_buffer);
        
        printf("Interlaced partial update: Region %d,%d to %d,%d (%d pixels)\n", 
               min_x, min_y, max_x, max_y, region_size);
    } else if (changed_areas > 0) {
        // Small changes, do full update for simplicity
        set_window(0, 0, WIDTH-1, HEIGHT-1);
        write_data_len((uint8_t*)current_frame, DISPLAY_BYTES);
    }
    // Else: no changes, no update needed
    #else
    update_changed_regions(current_frame, update_mask);
    #endif
}

// Smart display function with partial updates and interlacing
void display_framebuffer_smart_update(void) {
    printf("Smart display with partial updates");
    #if INTERLACE_ENABLED
    printf(" and interlacing");
    #endif
    printf("...\n");
    
    struct timespec start_time, current_time;
    long frame_count = 0;
    long total_frames = 0;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    // Allocate buffers
    uint16_t *current_frame = malloc(DISPLAY_BYTES);
    uint16_t *update_mask = malloc(DISPLAY_SIZE * sizeof(uint16_t));
    
    if (!current_frame || !update_mask) {
        printf("Failed to allocate buffers\n");
        free(current_frame);
        free(update_mask);
        return;
    }
    
    while (keep_running) {
        // GPU-accelerated snapshot
        if (vc_dispmanx_snapshot(display_handle, resource_handle, 0) != 0) {
            printf("Dispmanx snapshot failed\n");
            break;
        }
        
        // Read data from GPU resource
        if (vc_dispmanx_resource_read_data(resource_handle, &rect, current_frame, WIDTH * 2) != 0) {
            printf("Failed to read resource data\n");
            break;
        }
        
        // Apply color correction
        for (int i = 0; i < DISPLAY_SIZE; i++) {
            current_frame[i] = fix_color_format(current_frame[i]);
        }
        
        // Apply interlacing if enabled
        apply_interlacing(current_frame);
        
        // Detect changed regions
        int full_update = detect_changed_regions(current_frame, update_mask);
        
        if (full_update) {
            // Full screen update
            set_window(0, 0, WIDTH-1, HEIGHT-1);
            write_data_len((uint8_t*)current_frame, DISPLAY_BYTES);
            printf("Full update\n");
        } else {
            // Partial update of changed regions
            update_interlaced_regions(current_frame, update_mask);
        }
        
        // Update previous frame
        memcpy(prev_frame, current_frame, DISPLAY_BYTES);
        
        frame_count++;
        total_frames++;
        
        // FPS reporting every 30 frames
        if (frame_count % 30 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            long elapsed_time = (current_time.tv_sec - start_time.tv_sec) * 1000000000 + 
                               (current_time.tv_nsec - start_time.tv_nsec);
            
            if (elapsed_time >= 500000000) {  // Report every 0.5 seconds for better responsiveness
                float fps = frame_count * 1000000000.0f / elapsed_time;
                printf("FPS: %.1f (Total: %ld)\n", fps, total_frames);
                frame_count = 0;
                clock_gettime(CLOCK_MONOTONIC, &start_time);
            }
        }
        
        // Small sleep to prevent 100% CPU usage
        usleep(2000);  // Reduced sleep for higher FPS
    }
    
    free(current_frame);
    free(update_mask);
}

// Cleanup resources
void cleanup(void) {
    printf("Cleaning up resources...\n");
    
    // Clean up GPU resources
    if (resource_handle != 0) {
        vc_dispmanx_resource_delete(resource_handle);
    }
    
    if (display_handle != 0) {
        vc_dispmanx_display_close(display_handle);
    }
    
    // Free previous frame buffer
    if (prev_frame) {
        free(prev_frame);
    }
    
    bcm_host_deinit();
    bcm2835_spi_end();
    bcm2835_close();
}

// Main function
int main(int argc, char *argv[]) {
    printf("Smart Partial Update Display with GPU Acceleration\n");
    printf("Display dimensions: %dx%d\n", WIDTH, HEIGHT);
    
    // Set up signal handler for clean exit
    signal(SIGINT, signal_handler);
    
    init_gpio();
    init_spi();
    
    printf("Initializing display...\n");
    init_display();
    printf("Display initialized\n");
    
    printf("Initializing GPU resources...\n");
    if (!init_gpu_resources()) {
        printf("Failed to initialize GPU resources\n");
        cleanup();
        return 1;
    }
    printf("GPU resources initialized\n");
    
    printf("Starting smart display with partial updates...\n");
    printf("Press Ctrl+C to exit\n");
    
    display_framebuffer_smart_update();
    
    cleanup();
    printf("Exited cleanly\n");
    return 0;
}
