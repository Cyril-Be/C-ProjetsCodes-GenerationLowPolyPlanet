# Makefile — Génération 2D de Planète Low Poly
# Nécessite SDL2 : sudo apt install libsdl2-dev

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c99 $(shell sdl2-config --cflags)
LDFLAGS = $(shell sdl2-config --libs) -lm
TARGET  = planet
SRC     = src/main.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) planet.bmp
