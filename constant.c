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

// Dispmanx headers with proper paths
#include <bcm_host.h>
#include <interface/vmcs_host/vc_dispmanx.h>
#include <interface/vctypes/vc_image_types.h>

// Display dimensions
#define WIDTH 320
#define HEIGHT 170
#define DISPLAY_SIZE (WIDTH * HEIGHT)
#define DISPLAY_BYTES (DISPLAY_SIZE * 2)

// Display offset - ADJUST FOR YOUR DISPLAY
#define COL_OFFSET 0
#define ROW_OFFSET 35

// FPS counter option - SET TO 1 TO ENABLE, 0 TO DISABLE
#define SHOW_FPS 1

// Interlacing option - SET TO 1 TO ENABLE, 0 TO DISABLE
#define INTERLACE_ENABLED 0
#define INTERLACE_EVERY 2  // Every Nth line will be black (2 = every other line)

// GPIO pins
#define DC_PIN RPI_GPIO_P1_18  // GPIO 24
#define RST_PIN RPI_GPIO_P1_22 // GPIO 25
#define CS_PIN RPI_GPIO_P1_24  // GPIO 8 (CE0)

// SPI settings
#define SPI_SPEED 32000000  // 32 MHz

// Global variables
volatile sig_atomic_t keep_running = 1;
DISPMANX_DISPLAY_HANDLE_T display_handle = 0;
DISPMANX_RESOURCE_HANDLE_T resource_handle = 0;
VC_RECT_T rect;

// Function prototypes
void init_gpio(void);
void init_spi(void);
void init_display(void);
void write_command(uint8_t cmd);
void write_data_dma(const uint8_t *data, uint32_t len);
void set_window(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end);
int init_dispmanx(void);
void display_framebuffer_dispmanx(void);
void cleanup(void);
void signal_handler(int sig);
uint16_t fix_color_format(uint16_t color);
void apply_interlacing(uint16_t *buffer);

// Signal handler for clean exit
void signal_handler(int sig) {
    keep_running = 0;
}

// Fix color format for 16-bit RGB565
uint16_t fix_color_format(uint16_t color) {
    return ((color & 0xFF) << 8) | (color >> 8);
}

// Apply interlacing to buffer - every other line becomes black
void apply_interlacing(uint16_t *buffer) {
    #if INTERLACE_ENABLED
    for (int y = 0; y < HEIGHT; y++) {
        if (y % INTERLACE_EVERY == 1) {  // Make every other line black
            for (int x = 0; x < WIDTH; x++) {
                buffer[y * WIDTH + x] = 0x0000;  // Black
            }
        }
    }
    #endif
}

// Write data to display (single byte)
void write_data(uint8_t data) {
    bcm2835_gpio_write(DC_PIN, HIGH);
    bcm2835_gpio_write(CS_PIN, LOW);
    bcm2835_spi_transfer(data);
    bcm2835_gpio_write(CS_PIN, HIGH);
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

// Write multiple data bytes using DMA
void write_data_dma(const uint8_t *data, uint32_t len) {
    bcm2835_gpio_write(DC_PIN, HIGH);
    bcm2835_gpio_write(CS_PIN, LOW);
    
    // Transfer data using DMA
    bcm2835_spi_writenb((char*)data, len);
    
    bcm2835_gpio_write(CS_PIN, HIGH);
}

// Set display window with offset support
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

// Initialize display with optimized command sequence and offset support
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
    
    write_command(0x3A); // Color Mode
    write_data(0x55);    // 16-bit color (RGB565)
    
    // MADCTL - Memory Data Access Control
    write_command(0x36);
    write_data(0x60); // MV=1, MX=1, MY=0 (270° rotation)
    
    write_command(0x21);  // Display Inversion On
    
    // Set column address with offset support
    write_command(0x2A);
    write_data(COL_OFFSET >> 8);
    write_data(COL_OFFSET & 0xFF);
    write_data((COL_OFFSET + WIDTH - 1) >> 8);
    write_data((COL_OFFSET + WIDTH - 1) & 0xFF);
    
    // Set row address with offset support
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

// Initialize Dispmanx with 16-bit format only
int init_dispmanx(void) {
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
    printf("Display offset: COL=%d, ROW=%d\n", COL_OFFSET, ROW_OFFSET);
    
    #if INTERLACE_ENABLED
    printf("Interlacing: ENABLED (every %d lines)\n", INTERLACE_EVERY);
    #else
    printf("Interlacing: DISABLED\n");
    #endif
    
    // Create resource with 16-bit format only
    uint32_t vc_image_ptr;
    resource_handle = vc_dispmanx_resource_create(
        VC_IMAGE_RGB565,  // 16-bit format
        WIDTH, 
        HEIGHT, 
        &vc_image_ptr
    );
    
    if (resource_handle == 0) {
        printf("Failed to create dispmanx resource\n");
        return 0;
    }
    
    printf("Using 16-bit RGB565 format\n");
    
    // Set up rectangle
    vc_dispmanx_rect_set(&rect, 0, 0, WIDTH, HEIGHT);
    
    return 1;
}

// Display framebuffer using Dispmanx with 16-bit handling
void display_framebuffer_dispmanx(void) {
    printf("Displaying framebuffer using Dispmanx with 16-bit color...\n");
    
    // Set window to full screen once with offset support
    set_window(0, 0, WIDTH-1, HEIGHT-1);
    
    // Create buffers for display data
    uint16_t *dispmanx_buffer = malloc(DISPLAY_SIZE * sizeof(uint16_t));
    uint16_t *display_buffer = malloc(DISPLAY_SIZE * sizeof(uint16_t));
    
    if (!dispmanx_buffer || !display_buffer) {
        printf("Error allocating display buffers\n");
        free(dispmanx_buffer);
        free(display_buffer);
        return;
    }
    
    #if SHOW_FPS
    struct timespec start_time, current_time;
    long frame_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    #endif
    
    while (keep_running) {
        // Take snapshot of the display (GPU accelerated)
        if (vc_dispmanx_snapshot(display_handle, resource_handle, 0) != 0) {
            printf("Dispmanx snapshot failed\n");
            break;
        }
        
        // Read data from GPU resource
        if (vc_dispmanx_resource_read_data(resource_handle, &rect, dispmanx_buffer, WIDTH * 2) != 0) {
            printf("Failed to read resource data\n");
            break;
        }
        
        // Apply color correction
        for (int i = 0; i < DISPLAY_SIZE; i++) {
            display_buffer[i] = fix_color_format(dispmanx_buffer[i]);
        }
        
        // Apply interlacing if enabled
        apply_interlacing(display_buffer);
        
        // Send data to SPI display
        write_data_dma((uint8_t*)display_buffer, DISPLAY_SIZE * 2);
        
        #if SHOW_FPS
        frame_count++;
        
        // FPS reporting
        if (frame_count % 60 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            long elapsed_time = (current_time.tv_sec - start_time.tv_sec) * 1000000000 + 
                               (current_time.tv_nsec - start_time.tv_nsec);
            
            if (elapsed_time >= 1000000000) {
                float fps = frame_count * 1000000000.0f / elapsed_time;
                printf("FPS: %.1f\n", fps);
                frame_count = 0;
                clock_gettime(CLOCK_MONOTONIC, &start_time);
            }
        }
        #endif
        
        // Small sleep to prevent 100% CPU usage
        usleep(5000);
    }
    
    free(dispmanx_buffer);
    free(display_buffer);
}

// Cleanup resources
void cleanup(void) {
    printf("Cleaning up resources...\n");
    
    // Clean up Dispmanx resources
    if (resource_handle != 0) {
        vc_dispmanx_resource_delete(resource_handle);
    }
    
    if (display_handle != 0) {
        vc_dispmanx_display_close(display_handle);
    }
    
    bcm_host_deinit();
    
    bcm2835_spi_end();
    bcm2835_close();
}

// Main function
int main(int argc, char *argv[]) {
    printf("Initializing ST7789 display with 16-bit color handling...\n");
    printf("Display dimensions: %dx%d\n", WIDTH, HEIGHT);
    #if SHOW_FPS
    printf("FPS counter: ENABLED\n");
    #else
    printf("FPS counter: DISABLED\n");
    #endif
    
    // Set up signal handler for clean exit
    signal(SIGINT, signal_handler);
    
    init_gpio();
    init_spi();
    
    printf("Initializing display...\n");
    init_display();
    printf("Display initialized\n");
    
    printf("Initializing Dispmanx...\n");
    if (!init_dispmanx()) {
        printf("Failed to initialize Dispmanx\n");
        cleanup();
        return 1;
    }
    printf("Dispmanx initialized\n");
    
    printf("Starting framebuffer display...\n");
    printf("Press Ctrl+C to exit\n");
    
    display_framebuffer_dispmanx();
    
    cleanup();
    printf("Exited cleanly\n");
    return 0;
}
