CC = gcc
CFLAGS = -O2 -Iinclude -Wall -Wextra -std=gnu11
LDFLAGS = -lm -lpthread

WITH_NVML ?= 1
CUDA_HOME ?= $(firstword $(wildcard /usr/local/cuda /usr/local/cuda-12.4))
NVML_INC ?= -I$(CUDA_HOME)/targets/x86_64-linux/include
NVML_LIB ?= -L$(CUDA_HOME)/targets/x86_64-linux/lib/stubs -lnvidia-ml

ifeq ($(WITH_NVML),1)
CFLAGS += -DWITH_NVML $(NVML_INC)
LDFLAGS += $(NVML_LIB)
endif

SRC = src/main.c src/rapl.c src/gpu_energy.c
OBJ = $(SRC:.c=.o)
TARGET = bench

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean