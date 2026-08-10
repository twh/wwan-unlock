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

# RW101 33f8:0301 exposes its FCC AT commands on a ttyUSB port, while Lenovo's
# unmodified worker library attempts to use its MBIM AT service.  The verified
# module preloads this transport shim only for that device.
rw101-serial.so: src/rw101-serial.c
	$(CC) -O2 -Wall -Wextra -fPIC -shared -o $@ $<

# standalone alternative (built on demand: `make foxunlock`)
foxunlock: src/foxunlock.c
	$(CC) -O2 -Wall $(shell pkg-config --cflags glib-2.0 gio-2.0 mbim-glib) \
	    -o $@ $< $(shell pkg-config --libs glib-2.0 gio-2.0 mbim-glib)

clean:
	rm -f wwan-orch foxunlock rw101-serial.so

.PHONY: all clean
