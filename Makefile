SDL_PREFIX := $(shell brew --prefix sdl3)
CFLAGS := -O2 -Wall -Wextra -std=c11 -I$(SDL_PREFIX)/include
LDFLAGS := -L$(SDL_PREFIX)/lib -Wl,-rpath,$(SDL_PREFIX)/lib -lSDL3 -lz

pro-bridge: src/main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: pro-bridge
	./pro-bridge

clean:
	rm -f pro-bridge

.PHONY: run clean
