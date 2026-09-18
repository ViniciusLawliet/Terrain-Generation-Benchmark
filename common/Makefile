CC = gcc
AR = ar
CFLAGS = -O3 -Iinclude -fopenmp -Wall -Wextra -std=c11

SRC = src/perlin.c src/obj_export.c
OBJ = $(SRC:.c=.o)
LIB = libcommon.a

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $(LIB) $(OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(LIB)

.PHONY: all clean