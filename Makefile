# Makefile for partial.c and constant.c

# Compiler and flags
CC = gcc
CFLAGS = -O3 -march=armv6 -mtune=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard -ffast-math \
         -O3 -ffast-math -march=native -mtune=native -flto -fomit-frame-pointer \
         -funroll-loops -fno-signed-zeros -fno-trapping-math -fassociative-math

# Libraries
LIBS = -lbcm2835 -lrt -L/opt/vc/lib -lbcm_host -lvcos -lvchiq_arm

# Include directories
INCLUDES = -I/opt/vc/include -I/opt/vc/include/interface/vmcs_host/ -I/opt/vc/include/interface/vcos/pthreads

# Targets
TARGETS = partial constant

# Default target
all: $(TARGETS)

# Build partial from partial.c
partial: partial.c
	$(CC) $(CFLAGS) $(INCLUDES) partial.c -o partial $(LIBS)

# Build constant from constant.c
constant: constant.c
	$(CC) $(CFLAGS) $(INCLUDES) constant.c -o constant $(LIBS)

# Clean - remove executables
clean:
	rm -f $(TARGETS)

# Force rebuild
rebuild: clean all

.PHONY: all clean rebuild
