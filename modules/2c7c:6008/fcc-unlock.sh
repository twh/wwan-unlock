#!/bin/sh
# Clean-room FCC unlock dispatcher for the Quectel EM061K (2c7c:6008).
# Uses no Lenovo code: stock mbimcli sends the same message the vendor sends.
#
# Vendor sequence (DPR_Fcc_unlock_service -> setFccUnlock_cs24), verified against
# /opt/fcc_lenovo/lib/libmbimtools.so:
#
#   mbim_radio_state_set() composes two MBIM SET commands via
#   mbim_compose_command(uuid, cid, MBIM_MESSAGE_COMMAND_TYPE_SET, payload, 4):
#
#     uuid 11223344-5566-7788-99aa-bbccddeeff11  cid 1  <- the unlock
#     uuid a289cc33-bcbb-8b4f-b6b0-133ec2aae6df  cid 3  <- radio on
#
#   The first uuid is libmbim's uuid_quectel and cid 1 is
#   MBIM_CID_QUECTEL_RADIO_STATE, which is exactly what
#   --quectel-set-radio-state=on sends. The second is uuid_basic_connect with
#   MBIM_CID_BASIC_CONNECT_RADIO_STATE, which only powers the radio up;
#   ModemManager does that itself once this script returns 0, so it is omitted.
#
# The US-SIM gate lives in setFccUnlock_cs24, not in the message, so nothing is
# lost by not reproducing it.
#
# MM invokes: <script> <dbus-path> <control-port> [<port>...]
TAG=fcc-unlock-quectel
log() { logger -t "$TAG" -- "$@"; }

[ $# -lt 2 ] && { log "too few args"; exit 1; }
shift
for P in "$@"; do
  if grep -qi MBIM "/sys/class/wwan/$P/type" 2>/dev/null || echo "$P" | grep -qi mbim; then MBIM="$P"; break; fi
done
[ -n "$MBIM" ] || { log "no MBIM port"; exit 2; }
command -v mbimcli >/dev/null || { log "mbimcli not installed (libmbim-utils)"; exit 2; }

log "invoked: /dev/$MBIM (clean-room, Quectel MBIM radio state)"
out=$(mbimcli --device-open-proxy --device="/dev/$MBIM" --quectel-set-radio-state=on 2>&1); rc=$?
echo "$out" | while IFS= read -r l; do [ -n "$l" ] && log "  $l"; done
log "result rc=$rc"
exit $rc
