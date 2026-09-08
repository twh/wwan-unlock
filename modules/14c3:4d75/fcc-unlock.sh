#!/bin/sh
# Clean-room FCC unlock dispatcher for the Fibocom FM350-GL.
#   FM350-GL, 14c3:4d75
# Uses no Lenovo code: the challenge/response is computed here with stock
# sha256sum.
#
# fccunlock_fm350_l860() is called with transport selector 1 at every call site,
# for the FM350-GL and the L860R+ alike, which resolves init_modemauth_srvc() on
# /dev/wwan0at0. The init_modemauth_srvc_mbim() branch is never selected.
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
TAG=fcc-unlock-fibocom
log() { logger -t "$TAG" -- "$@"; }

[ $# -lt 2 ] && { log "too few args"; exit 1; }
shift

# the vendor drives this module over its wwan AT port (/dev/wwan0at0)
for P in "$@"; do
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

# The vendor id hash is sha256 of the machine's "model id in bios", the first
# OEM string in an SMBIOS type 133 record. Chapter 17.4 of Fibocom's public
# FM350 AT command manual documents it: the model id "KHOIHGIUCCHHII" has NVM
# hash 0x3d,0xf8,0xc7,0x19. It is a property of the machine, not the modem, so
# it is derived here rather than carried as a constant.
#
# A machine can have more than one type 133 record: Lenovo ships a small one
# whose string table holds the id, alongside a 44 byte one that has no strings
# at all. Dell's Latitude 5540 has only the latter. So iterate every record and
# take the first that yields a string, and keep known values for firmware that
# publishes none.
dmi_oem_string() {
    for _e in /sys/firmware/dmi/entries/133-*; do
        [ -d "$_e" ] || continue
        _len="$(cat "$_e/length" 2>/dev/null)" || continue
        [ -n "$_len" ] || continue
        _s="$(tail -c "+$((_len + 1))" "$_e/raw" 2>/dev/null | tr '\000' '\n' | head -n 1)"
        [ -n "$_s" ] && { echo "$_s"; return 0; }
    done
    return 1
}

# 3df8c719 = sha256("KHOIHGIUCCHHII"), Lenovo, which Lenovo firmware publishes
# 4909b5a4 = sha256("DW5931EFCCLOCK"), from Dell's FM350 driver
# bb23be7f = sha256("DW5823EFCCLOCK"), from Dell's L860-R driver
KNOWN_VENDOR_ID_HASHES='3df8c719 4909b5a4 bb23be7f'

VENDOR_ID_HASHES=''
DEVCODE="$(dmi_oem_string)"
if [ -n "$DEVCODE" ]; then
    VENDOR_ID_HASHES="$(printf '%s' "$DEVCODE" | sha256sum | cut -c '1-8')"
    log "  derived vendor id hash $VENDOR_ID_HASHES from SMBIOS type 133"
else
    log "  no OEM string in SMBIOS type 133; trying known vendor id hashes"
fi
for KNOWN in $KNOWN_VENDOR_ID_HASHES; do
    case " $VENDOR_ID_HASHES " in
        *" $KNOWN "*) ;;
        *) VENDOR_ID_HASHES="$VENDOR_ID_HASHES $KNOWN" ;;
    esac
done

log "invoked: $DEVICE (clean-room AT challenge/response)"
i=1
for VENDOR_ID_HASH in $VENDOR_ID_HASHES; do
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
          log "  attempt $i: hash $VENDOR_ID_HASH refused, reply: $REPLY"
          break
      else
          log "  attempt $i: no challenge, reply: $RAW"
      fi
      sleep 0.5
      i="$((i + 1))"
  done
done

log "result rc=2 (no vendor id hash was accepted)"
exit 2
