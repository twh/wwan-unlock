#!/bin/sh
# ModemManager FCC-unlock dispatcher for the verified RW101R-GL 33f8:0301 USB
# configuration.  Keep MBIM as the vendor worker's control port, but route its
# FCC AT commands through the serial port exposed by this firmware.
FAMILY=rw101
TAG=fcc-unlock-rolling
LIB=/usr/local/lib/wwan-unlock
SHIM="$LIB/rw101-serial.so"
log() { logger -t "$TAG" -- "$@"; }

[ $# -lt 2 ] && { log "too few args"; exit 1; }
MBIM=
AT=
shift
for P in "$@"; do
  if [ -z "$MBIM" ] &&
    { grep -qi MBIM "/sys/class/wwan/$P/type" 2>/dev/null ||
      echo "$P" | grep -qiE 'mbim|^cdc-wdm'; }; then
    MBIM="$P"
  fi
  if [ -z "$AT" ] && echo "$P" | grep -qi '^ttyUSB'; then
    AT="$P"
  fi
done
[ -n "$MBIM" ] || { log "no MBIM port"; exit 2; }
[ -n "$AT" ] || { log "no AT port"; exit 2; }
[ -x "$LIB/wwan-orch" ] || { log "wwan-orch not installed"; exit 2; }
[ -r "$SHIM" ] || { log "RW101 serial transport not installed"; exit 2; }

log "invoked: /dev/$MBIM via /dev/$AT (family $FAMILY)"
out=$(WWAN_UNLOCK_MBIM_PORT="/dev/$MBIM" \
  WWAN_UNLOCK_AT_PORT="/dev/$AT" \
  LD_PRELOAD="$SHIM" \
  "$LIB/wwan-orch" --family "$FAMILY" -d "/dev/$MBIM" 2>&1); rc=$?
echo "$out" | while IFS= read -r l; do [ -n "$l" ] && log "  $l"; done
log "result rc=$rc"
exit $rc
