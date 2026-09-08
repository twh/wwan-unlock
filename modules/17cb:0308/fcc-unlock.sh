#!/bin/sh
# Clean-room FCC unlock dispatcher for the Foxconn T99W696 (17cb:0308).
# Uses no Lenovo code: foxunlock builds and sends the vendor's message itself.
#
# Vendor sequence (DPR_Fcc_unlock_service -> setFccUnlock_fxn), verified against
# /opt/fcc_lenovo/lib/libfiisdk.so.2.2.2:
#
#   Fox_Attempt() -> FoxApSetFccLockStatus() -> QMIFOXAPSetFccLockStatus(),
#   which composes service byte 0xE4 (FOXAP) and message id 0x5571 and hands
#   both to SendMessage() -> ComposeUniversalQMUXMsg().
#
#   TLV 0x01  salt (4 chars) + lowercase md5 hex (32) = 36 bytes
#   TLV 0x02  one byte, '0' to unlock
#
#   The md5 input is the firmware mcfg version with its last two dot-separated
#   fields dropped, the apps version, the IMEI, the salt, and the magic "FDE2".
#   The magic is not stored: bytes 69 67 68 55 ("ighU") are built on the stack
#   and each is put through b_char_value(), which is c ? c - 0x23 : 0.
#   Firmware versions come from FOX (0xE3) msg 0x555E, the IMEI from DMS (0x02).
#
# The US-SIM gate lives in Fox_Attempt, not in the message builder, so nothing
# is lost by not reproducing it.
#
# foxunlock is used rather than qmicli because libqmi does not know service
# 0xE4 yet; that is mobile-broadband/libqmi!473. Once it ships, this can become
# qmicli --foxap-set-fcc-authentication.
#
# MM invokes: <script> <dbus-path> <control-port> [<port>...]
TAG=fcc-unlock-foxconn
LIB=/usr/local/lib/wwan-unlock
log() { logger -t "$TAG" -- "$@"; }

[ $# -lt 2 ] && { log "too few args"; exit 1; }
shift
for P in "$@"; do
  if grep -qi MBIM "/sys/class/wwan/$P/type" 2>/dev/null || echo "$P" | grep -qi mbim; then MBIM="$P"; break; fi
done
[ -n "$MBIM" ] || { log "no MBIM port"; exit 2; }
[ -x "$LIB/foxunlock" ] || { log "foxunlock not installed"; exit 2; }

log "invoked: /dev/$MBIM (clean-room, FOXAP 0xE4 msg 0x5571)"
out=$("$LIB/foxunlock" -d "/dev/$MBIM" 2>&1); rc=$?
echo "$out" | while IFS= read -r l; do [ -n "$l" ] && log "  $l"; done
log "result rc=$rc"
exit $rc
