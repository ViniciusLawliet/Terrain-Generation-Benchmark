NVCC = nvcc
CC = gcc

CUDA_ARCH = -arch=sm_61

NVCC_FLAGS = -O3 $(CUDA_ARCH) -Iinclude -I../common/include
CC_FLAGS = -O3 -Iinclude -I../common/include -fopenmp -Wall -Wextra -std=c11
LDFLAGS = -Xcompiler -fopenmp -L../common -lcommon -lm

SRC_C = src/main.c
SRC_CU = src/kernels.cu
OBJ_C = $(SRC_C:.c=.o)
OBJ_CU = $(SRC_CU:.cu=.o)
TARGET = terrain_gen_cuda

all: ../common/libcommon.a $(TARGET)

../common/libcommon.a:
	$(MAKE) -C ../common

$(TARGET): $(OBJ_C) $(OBJ_CU)
	$(NVCC) $(OBJ_C) $(OBJ_CU) -o $(TARGET) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CC_FLAGS) -c $< -o $@

src/%.o: src/%.cu
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

clean:
	rm -f $(OBJ_C) $(OBJ_CU) $(TARGET)

.PHONY: all clean