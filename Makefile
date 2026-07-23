# wwan-unlock — clean-room gateless orchestrator + standalone helper
#
# wwan-orch  : primary. Reimplements Lenovo's gated orchestrator (minus the US-SIM
#              check) and calls Lenovo's own unmodified libs (vendor/lenovo/lib).
#              Only needs -ldl.
# foxunlock  : standalone, FULLY clean-room FCC unlock for the Foxconn T99W696
#              (no vendor libs at runtime). Optional alternative. Needs libmbim.
#
# Build deps: gcc pkgconf ; foxunlock also needs libmbim-glib-dev

CC      ?= gcc

all: wwan-orch

wwan-orch: src/wwan-orch.c
	$(CC) -O2 -Wall -Wextra -o $@ $< -ldl

# standalone alternative (built on demand: `make foxunlock`)
foxunlock: src/foxunlock.c
	$(CC) -O2 -Wall $(shell pkg-config --cflags glib-2.0 gio-2.0 mbim-glib) \
	    -o $@ $< $(shell pkg-config --libs glib-2.0 gio-2.0 mbim-glib)

clean:
	rm -f wwan-orch foxunlock

.PHONY: all clean
