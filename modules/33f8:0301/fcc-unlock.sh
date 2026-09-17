#!/bin/sh
# Clean-room FCC unlock dispatcher for the Rolling RW101R-GL.
#   RW101R-GL, 33f8:0301
# Uses no Lenovo code: the challenge/response is computed here with stock
# sha256sum.
#
# Lenovo's libmodemauthRW101.so.1.1 maps every Rolling USB pid to device type 3
# in its `modules` table (01a8, 01a9, 0301, 0302 -> 3), and init_modemauth_srvc
# dispatches type 3 to fcc_at_modem_unlock_101r on /dev/cdc-wdm0, which runs
# event_monitor_at_101r. That is a different sequence from event_monitor_at,
# which serves the 7560 R+ (device type 2).
#
# Vendor sequence, event_monitor_at_101r, verified against the library:
#
#   ate0                      send_at_of_mm
#   at+gtfcclockgen           challenge, 8 hex chars taken from offset 0x12
#   at+gtfcclockver=0x<hex>   response, sent as a hex string
#   at+cfun=1                 send_at_of_mm
#   AT+GTFCCEFFSTATUS?        strtoul base 16 of the reply, non-zero = unlocked
#
# Every send failure is fatal to the vendor: LOGE then exit(1). There is no
# at+gtfcclockmodeunlock on this path, and no at+gtfcclockstate. The vendor does
# not check the at+gtfcclockver reply at all; the effective status read is what
# decides.
#
# We depart in one place: at+cfun=1 is not sent, because ModemManager sets the
# power state itself once this dispatcher returns 0.
#
# The response comes from compute_sha256_101r, which works on hex strings and
# does no byte swapping: StrSHA256 over the model id, its first 8 hex chars
# appended to the challenge's 8 hex chars, those 16 characters converted back to
# 8 bytes, StrSHA256 over those, and the first 8 hex chars of the result sent as
# text. The first four bytes of sha256("KHOIHGIUCCHHII") are 3df8c719, the
# vendor id hash below.
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

# if the option driver has not claimed the device, ModemManager never saw a
# ttyUSB to pass us. Bind it here and re-check: kernels before 6.18 (and the
# stable backports) do not know 33f8:01a8/01a9/0301/0302. The bundled
# 99-rw101r-serial.rules makes this persistent; this is the fallback for a
# system that does not have it installed yet.
if [ -z "$AT" ] && [ -w /sys/bus/usb-serial ]; then
  log "no AT port from ModemManager; binding the option driver"
  modprobe option >/dev/null 2>&1 || true
  if [ -d /sys/bus/usb-serial/drivers/option1 ]; then
    for PID in 01a8 01a9 0301 0302; do
      echo "33f8 $PID" > /sys/bus/usb-serial/drivers/option1/new_id 2>/dev/null || true
    done
  fi
  n=1
  while [ "$n" -le 6 ]; do
    for D in /dev/ttyUSB*; do
      [ -e "$D" ] && { AT="${D#/dev/}"; break; }
    done
    [ -n "$AT" ] && break
    sleep 0.2
    n="$((n + 1))"
  done
  [ -n "$AT" ] && log "  bound $AT; install 99-rw101r-serial.rules to make it persistent"
fi

[ -n "$AT" ] || { log "no AT port; the option driver could not be bound"; exit 2; }
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
# at all. Dell's Latitude 5540 has only the latter, which is why known values
# are kept below for firmware that publishes none.
#
# Read 133-0, as ModemManager!1491 does. On every machine anyone has examined
# the string bearing record is the first one, and no machine has been observed
# where it is not. Read from DMI sysfs rather than dmidecode, to avoid
# executing another binary.
dmi_oem_string() {
    entry='/sys/firmware/dmi/entries/133-0'
    len="$(cat "$entry/length" 2>/dev/null)" || return 1
    [ -n "$len" ] || return 1
    tail -c "+$((len + 1))" "$entry/raw" 2>/dev/null | tr '\000' '\n' | head -n 1
}

#   3df8c719 = sha256("KHOIHGIUCCHHII"), Lenovo, one id for every module
# No Dell value is listed. Dell keys the id to the WWAN card and names it
# after it (DW5823EFCCLOCK for the L860-R, DW5931EFCCLOCK for the FM350), so
# neither applies to a Rolling module, and no Dell Rolling id is known.
KNOWN_VENDOR_ID_HASHES='3df8c719'

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
at_command 'ate0' >/dev/null
i=1
for VENDOR_ID_HASH in $VENDOR_ID_HASHES; do
  while [ "$i" -le 9 ]; do
      RAW="$(at_command 'at+gtfcclockgen')"
      CHALLENGE="$(echo "$RAW" | grep -o '0x[0-9a-fA-F]\+' | awk '{print $1}')"
      if [ -n "$CHALLENGE" ]; then
          HEX="$(printf '%08x' "$CHALLENGE")"
          COMBINED="$HEX$(printf '%.8s' "$VENDOR_ID_HASH")"
          HASH="$(echo "$COMBINED" | xxd -r -p | sha256sum | cut -d ' ' -f 1)"
          at_command "at+gtfcclockver=0x$(printf '%.8s' "$HASH")" >/dev/null

          # the vendor ignores the gtfcclockver reply and reads the effective
          # status instead, requiring a non-zero value
          STATUS="$(at_command 'AT+GTFCCEFFSTATUS?')"
          VALUE="$(echo "$STATUS" | tr -d '\r' | sed 's/.*[:=] *//')"
          case "$VALUE" in
              ''|0|00|0x0) ;;
              *)  log "  FCC unlock: SUCCESS (status $STATUS)"
                  log "result rc=0"
                  exit 0 ;;
          esac
          log "  attempt $i: hash $VENDOR_ID_HASH refused, status: $STATUS"
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
