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

AGENT_LABEL := com.fullstackgabe.pro-bridge
AGENT_PLIST := $(HOME)/Library/LaunchAgents/$(AGENT_LABEL).plist

# Starts pro-bridge at login and restarts it if it exits.
install-agent: pro-bridge
	mkdir -p $(HOME)/Library/LaunchAgents
	sed -e 's|@BINARY@|$(CURDIR)/pro-bridge|' -e 's|@LOG@|$(HOME)/Library/Logs/pro-bridge.log|' -e 's|@LABEL@|$(AGENT_LABEL)|' launchd.plist.in > $(AGENT_PLIST)
	launchctl bootout gui/$$(id -u) $(AGENT_PLIST) 2>/dev/null || true
	launchctl bootstrap gui/$$(id -u) $(AGENT_PLIST)

uninstall-agent:
	launchctl bootout gui/$$(id -u) $(AGENT_PLIST) 2>/dev/null || true
	rm -f $(AGENT_PLIST)

.PHONY: install-agent uninstall-agent
