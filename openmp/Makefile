CC = gcc
CFLAGS = -O3 -march=native -funroll-loops -fopenmp -Wall -Wextra -std=c11 -Iinclude -I../common/include
LDFLAGS = -fopenmp -L../common -lcommon -lm

SRC = src/main.c src/marching_cubes.c
OBJ = $(SRC:.c=.o)
TARGET = terrain_gen_openmp

all: ../common/libcommon.a $(TARGET)

../common/libcommon.a:
	$(MAKE) -C ../common

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean