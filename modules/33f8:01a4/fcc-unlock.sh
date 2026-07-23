#!/bin/sh
# ModemManager fcc-unlock dispatcher: calls wwan-orch --family rw101, the gateless
# orchestrator that invokes Lenovo's own libraries with the US-SIM gate omitted.
# MM invokes: <script> <dbus-path> <control-port> [<port>...]
FAMILY=rw101
TAG=fcc-unlock-rolling
LIB=/usr/local/lib/wwan-unlock
log() { logger -t "$TAG" -- "$@"; }

[ $# -lt 2 ] && { log "too few args"; exit 1; }
shift
for P in "$@"; do
  if grep -qi MBIM "/sys/class/wwan/$P/type" 2>/dev/null || echo "$P" | grep -qi mbim; then MBIM="$P"; break; fi
done
[ -n "$MBIM" ] || { log "no MBIM port"; exit 2; }
[ -x "$LIB/wwan-orch" ] || { log "wwan-orch not installed"; exit 2; }

log "invoked: /dev/$MBIM (family $FAMILY)"
out=$("$LIB/wwan-orch" --family "$FAMILY" -d "/dev/$MBIM" 2>&1); rc=$?
echo "$out" | while IFS= read -r l; do [ -n "$l" ] && log "  $l"; done
log "result rc=$rc"
exit $rc
