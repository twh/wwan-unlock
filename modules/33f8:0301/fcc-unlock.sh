#!/bin/sh
# Clean-room FCC unlock dispatcher for the Rolling RW101R-GL.
#   RW101R-GL, 33f8:0301
# Uses no Lenovo code: the challenge/response is computed here with stock
# sha256sum.
#
# The vendor drives all five ids through one code path (fccunlock_rw101), which
# resolves only init_modemauth_srvc -- there is no MBIM variant -- and lets the
# library locate the AT port itself, so the sequence is identical for all of them.
#
# Vendor sequence, verified against Lenovo's worker library:
#
#   at+gtfcclockgen           challenge, at_send_command_singleline
#   at+gtfcclockver=<n>       response, must reply 1
#   at+gtfcclockmodeunlock    best effort, vendor only logs a failure
#   at+cfun=1                 best effort, likewise
#   at+gtfcclockstate         best effort, state read back
#
# The response is compute_sha256(): sha256 over the 14-byte vendor key, then
# sha256 over four bytes of that digest followed by four bytes of challenge,
# sending the first four bytes of the result as a decimal. The first four bytes
# of sha256("KHOIHGIUCCHHII") are 3df8c719, the vendor id hash below.
#
# The US-SIM gate lives in the DPR_Fcc_unlock_service caller, not in this
# sequence, so nothing is lost by not reproducing it.
#
# MM invokes: <script> <dbus-path> <control-port> [<port>...]
TAG=fcc-unlock-rolling
log() { logger -t "$TAG" -- "$@"; }

[ $# -lt 2 ] && { log "too few args"; exit 1; }
shift

# this firmware answers the FCC challenge on its serial AT port; the wwan AT
# service returns ERROR for at+gtfcclockgen on 33f8:0301
for P in "$@"; do
  echo "$P" | grep -q '^ttyUSB' && { AT="$P"; break; }
done
# fall back to a wwan AT port for the variants that expose one
[ -n "$AT" ] || for P in "$@"; do
  grep -q AT "/sys/class/wwan/$P/type" 2>/dev/null || echo "$P" | grep -qi AT && { AT="$P"; break; }
done
[ -n "$AT" ] || { log "no AT port"; exit 2; }
DEVICE="/dev/$AT"

at_command() {
    exec 9<>"$DEVICE"
    printf "%s\r" "$1" >&9
    read answer <&9
    read answer <&9
    echo "$answer"
    exec 9>&-
}

VENDOR_ID_HASH='3df8c719'

log "invoked: $DEVICE (clean-room AT challenge/response)"
i=1
while [ "$i" -le 9 ]; do
    RAW="$(at_command 'at+gtfcclockgen')"
    CHALLENGE="$(echo "$RAW" | grep -o '0x[0-9a-fA-F]\+' | awk '{print $1}')"
    if [ -n "$CHALLENGE" ]; then
        HEX="$(printf '%08x' "$CHALLENGE")"
        COMBINED="$HEX$(printf '%.8s' "$VENDOR_ID_HASH")"
        HASH="$(echo "$COMBINED" | xxd -r -p | sha256sum | cut -d ' ' -f 1)"
        RESPONSE="$(printf '%d' "0x$(printf '%.8s' "$HASH")")"
        REPLY="$(at_command "at+gtfcclockver=$RESPONSE")"

        # the vendor does not match a response prefix: it parses the value with
        # strtoul and requires 1
        RESULT="$(echo "$REPLY" | grep -o '[0-9][0-9]*' | tail -1)"
        if [ "$RESULT" = '1' ]; then
            log "  FCC unlock: SUCCESS"
            at_command 'at+gtfcclockmodeunlock' >/dev/null
            at_command 'at+cfun=1' >/dev/null
            log "  FCC lock state: $(at_command 'at+gtfcclockstate')"
            log "result rc=0"
            exit 0
        fi
        log "  attempt $i: unlock refused, reply: $REPLY"
    else
        log "  attempt $i: no challenge, reply: $RAW"
    fi
    sleep 0.5
    i="$((i + 1))"
done

log "result rc=2 (no successful unlock after 9 attempts)"
exit 2
