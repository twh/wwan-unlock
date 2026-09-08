# wwan-unlock — clean-room FCC unlock, plus an orchestrator used only for SAR
#
# Every FCC unlock in modules/ is clean-room and contains no Lenovo code:
#   foxunlock     Foxconn T99W696, QMI-over-MBIM, FOXAP 0xE4 msg 0x5571
#   at-gtfcclock  Rolling RW101R-GL, Fibocom FM350-GL and L860R+, in the
#                 dispatcher itself with stock sha256sum
#   mbimcli       Quectel, --quectel-set-radio-state=on
#
# wwan-orch  : SAR only. Reimplements Lenovo's gated orchestrator (minus the
#              US-SIM check) and calls Lenovo's own unmodified libs
#              (vendor/lenovo/lib). Only needs -ldl. Not used for any unlock.
# foxunlock  : the Foxconn unlock. Needs libmbim.
#
# Build deps: gcc pkgconf ; foxunlock also needs libmbim-glib-dev

CC      ?= gcc

all: wwan-orch foxunlock

wwan-orch: src/wwan-orch.c
	$(CC) -O2 -Wall -Wextra -o $@ $< -ldl

foxunlock: src/foxunlock.c
	$(CC) -O2 -Wall $(shell pkg-config --cflags glib-2.0 gio-2.0 mbim-glib) \
	    -o $@ $< $(shell pkg-config --libs glib-2.0 gio-2.0 mbim-glib)

clean:
	rm -f wwan-orch foxunlock

.PHONY: all clean
