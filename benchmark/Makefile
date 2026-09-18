CC = gcc
CFLAGS = -O2 -Iinclude -Wall -Wextra -std=gnu11
LDFLAGS = -lm

WITH_NVML ?= 1

CUDA_HOME ?= $(firstword $(wildcard /usr/local/cuda /usr/local/cuda-* ))
NVML_INC ?= -I$(CUDA_HOME)/targets/x86_64-linux/include

NVML_RUNTIME ?= $(shell ldconfig -p 2>/dev/null | awk '/libnvidia-ml\.so\.1/{print $$NF; exit}')
ifeq ($(strip $(NVML_RUNTIME)),)
  NVML_LIB ?= -l:libnvidia-ml.so.1
else
  NVML_LIB ?= $(NVML_RUNTIME)
endif

ifeq ($(WITH_NVML),1)
CFLAGS += -DWITH_NVML $(NVML_INC)
LDFLAGS += $(NVML_LIB)
endif

SRC = src/main.c src/rapl.c src/gpu_energy.c
OBJ = $(SRC:.c=.o)
TARGET = bench
PROBE = nvml_probe

all: $(TARGET)
ifeq ($(WITH_NVML),1)
all: $(PROBE)
endif

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

$(PROBE): src/nvml_probe.o
	$(CC) $< -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) src/nvml_probe.o $(TARGET) $(PROBE)

.PHONY: all clean