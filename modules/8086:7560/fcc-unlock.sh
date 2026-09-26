#!/bin/sh
# Clean-room FCC unlock dispatcher for the Intel L860R+.
#   L860R+, 8086:7560
# Uses no vendor code: every challenge/response is computed here with stock
# sha256sum and mbimcli.
#
# Three OEMs ship this module, and the kernel binds all of them through
# drivers/net/wwan/iosm on vendor and product alone, so one dispatcher serves
# every one. Each OEM unlocks it its own way:
#
#   Lenovo  1cf8:*                AT +GTFCCLOCK, below
#   Dell    1028:5823, 1028:3a17  Intel FCC lock over MBIM, below
#   HP      103c:8507/893b/8a53   ships no unlock at all
#
# The OEM is read from the DMI system vendor, which is where Lenovo's own
# libmodemauth.so looks: init_modemauth_srvc searches the SMBIOS manufacturer
# for "Dell" and "Lenovo". On any other vendor, HP included, both methods are
# tried in turn. Per OEM traces, with addresses, are in docs/traces/.
#
# fccunlock_fm350_l860() is called with transport selector 1 at every call site,
# for the FM350-GL and the L860R+ alike, which resolves init_modemauth_srvc() on
# /dev/wwan0at0. The init_modemauth_srvc_mbim() branch is never selected.
#
# Vendor sequence, verified against Lenovo's worker library:
#
#   at+gtfcclockgen           challenge, at_send_command_singleline
#   at+gtfcclockver=<n>       response, must reply 1
#   at+gtfcclockmodeunlock    at_send_command
#   at+cfun=1                 at_send_command
#   at+gtfcclockstate         at_send_command_singleline
#
# Every one of those is mandatory to the vendor: each failure branch logs and
# then calls exit(1). Only a gtfcclockver value other than 1 is retried rather
# than fatal. We depart from that in two places, both deliberate:
#
#   at+cfun=1 is not sent. ModemManager sets the power state itself once this
#   dispatcher returns 0.
#
#   a failure of at+gtfcclockmodeunlock or at+gtfcclockstate is not treated as
#   fatal. The unlock is already effected by the time gtfcclockver replies 1;
#   those two only complete and read back the state.
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

# Lenovo drives this module over its wwan AT port, Dell over the MBIM port;
# iosm exposes both on every machine, so collect each.
for P in "$@"; do
  TYPE="$(cat "/sys/class/wwan/$P/type" 2>/dev/null || echo "$P")"
  case "$TYPE" in
    MBIM|*mbim*) [ -n "$MBIM" ] || MBIM="$P" ;;
    AT|*at[0-9]*) [ -n "$AT" ] || AT="$P" ;;
  esac
done
[ -n "$AT" ] || [ -n "$MBIM" ] || { log "no usable control port"; exit 2; }
DEVICE="/dev/$AT"

# A modem that never answers would otherwise hang this script until
# ModemManager kills it at five seconds, and the Dell method below would never
# get to run. POSIX sh has no "read -t", so bound the read with timeout(1) when
# it is present, which it is on any coreutils system.
if command -v timeout >/dev/null 2>&1; then AT_TO='timeout 2'; else AT_TO=''; fi

at_command() {
    exec 9<>"$DEVICE"
    printf "%s\r" "$1" >&9
    answer="$($AT_TO sh -c 'read a <&9; read a <&9; printf "%s" "$a"' 2>/dev/null)"
    exec 9>&-
    printf '%s' "$answer"
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
# A machine can carry more than one type 133 record and only one holds the id.
# HP's own firmware tool tells them apart by formatted length:
# SMBIOS::ProcFCCType in WWANFirmwareFlash.dll matches type 0x85 length 5 and
# copies 14 bytes from its string table, while SMBIOS::ProcWWANConfigIDType
# matches type 0x85 length 0x2c, a structured record with no model id string.
# So walk every entry and take the first 14 character string.
dmi_oem_string() {
    for entry in /sys/firmware/dmi/entries/133-*; do
        [ -d "$entry" ] || continue
        len="$(cat "$entry/length" 2>/dev/null)" || continue
        [ -n "$len" ] || continue
        s="$(tail -c "+$((len + 1))" "$entry/raw" 2>/dev/null | tr '\000' '\n' | head -n 1)"
        [ "${#s}" = '14' ] && { printf '%s' "$s"; return 0; }
    done
    return 1
}

#   3df8c719 = sha256("KHOIHGIUCCHHII"), Lenovo, one id for every module
#   bb23be7f = sha256("DW5823EFCCLOCK"), Dell's name for this card, from its
#              L860-R driver package, and the value !1141 uses
# Dell keys the id to the WWAN card, not the machine: libmodemauth pairs
# 8086:7560 with Dell subsystem 1028:5823 (DW5823e). So no other Dell value
# can apply to this module.
KNOWN_VENDOR_ID_HASHES='3df8c719 bb23be7f'

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

# This modem hashes the challenge as a little endian u32, and reads the first
# four bytes of the digest back as one. Lenovo's libmodemauth carries one hash
# function per device and they differ only in that byte placement:
#
#   compute_sha256        MSB at the higher address -> little endian
#                         used by 8086:7560 and, via event_monitor_at, by the
#                         Rolling modules
#   compute_sha256_fm350  MSB at the lower address  -> big endian
#                         used by 14c3:4d75, which is why the upstream 14c3
#                         script needs no swap
#
# So swap both ends here. ModemManager!1141 and the write-up at
# blog.hofstede.it/replacing-lenovos-wwan-unlock-blob-with-a-100-line-bash-script
# arrived at the same little endian handling for 8086:7560 independently.
swap32() {
    printf '%s' "$1" | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/'
}

# Dell's method, from DoFccUnlock in its ModemAuthenticator.exe: CID 1 of the
# Intel Mutual Authentication service, f85d46ef-ab26-4081-9868-4d183c0a3aec,
# which libmbim implements and mbimcli exposes. A set carrying
# ResponsePresent = 0 asks for the challenge and the reply carries it; a set
# carrying ResponsePresent = 1 submits the response. HP's own WinIhvRil.dll
# confirms that split: ril_request_INTEL_FCC_LOCK_Set branches on the payload's
# first u32, 0 to generate a challenge and 1 to verify one.
#
# Dell unlocks only when the query reports a nonzero lock mode with a zero lock
# state, and goes ahead anyway when the query itself fails.
#
# mbimcli prints and parses those u32 fields as numbers while the modem hashes
# their little endian bytes, so the swap is applied at that boundary only. The
# response must be decimal: mbimcli_read_uint_from_string rejects non-digits.
unlock_dell() {
    [ -n "$MBIM" ] || { log "  no MBIM port for the Dell method"; return 1; }
    DEV="/dev/$MBIM"
    mbim() { mbimcli --device-open-proxy --device="$DEV" "$1" 2>&1; }
    challenge_of() { echo "$1" | sed -n 's/.*Challenge: *\([0-9][0-9]*\).*/\1/p'; }

    n=1
    while [ "$n" -le 3 ]; do
        STATUS="$(mbim '--intel-query-fcc-lock')"
        case "$STATUS" in
            *'FCC lock status: unlocked'*)
                log "  FCC lock is not engaged, nothing to do"
                return 0 ;;
            *'FCC lock status: locked'*)
                STATE="$(challenge_of "$STATUS")"
                if [ "$((${STATE:-0} % 256))" != '0' ]; then
                    log "  FCC lock state is set, nothing to do"
                    return 0
                fi ;;
            *) log "  could not read FCC lock state, unlocking anyway: $STATUS" ;;
        esac

        CHALLENGE="$(challenge_of "$(mbim '--intel-set-fcc-lock=0,0')")"
        if [ -z "$CHALLENGE" ]; then
            log "  attempt $n: modem returned no challenge"
            sleep 0.5; n="$((n + 1))"; continue
        fi
        COMBINED="$(swap32 "$(printf '%08x' "$CHALLENGE")")bb23be7f"
        HASH="$(echo "$COMBINED" | xxd -r -p | sha256sum | cut -d ' ' -f 1)"
        RESPONSE="$(printf '%u' "0x$(swap32 "$(printf '%.8s' "$HASH")")")"
        RESULT="$(mbim "--intel-set-fcc-lock=1,$RESPONSE")"
        case "$RESULT" in
            *'FCC lock status: unlocked'*)
                log "  FCC unlock: SUCCESS (Dell method)"
                return 0 ;;
            *) log "  attempt $n: response refused: $RESULT" ;;
        esac
        sleep 0.5
        n="$((n + 1))"
    done
    return 1
}

unlock_lenovo() {
    [ -n "$AT" ] || { log "  no AT port for the Lenovo method"; return 1; }
    i=1
    for VENDOR_ID_HASH in $VENDOR_ID_HASHES; do
      while [ "$i" -le 9 ]; do
      RAW="$(at_command 'at+gtfcclockgen')"
      CHALLENGE="$(echo "$RAW" | grep -o '0x[0-9a-fA-F]\+' | awk '{print $1}')"
      if [ -n "$CHALLENGE" ]; then
          HEX="$(swap32 "$(printf '%08x' "$CHALLENGE")")"
          COMBINED="$HEX$(printf '%.8s' "$VENDOR_ID_HASH")"
          HASH="$(echo "$COMBINED" | xxd -r -p | sha256sum | cut -d ' ' -f 1)"
          RESPONSE="$(printf '%d' "0x$(swap32 "$(printf '%.8s' "$HASH")")")"
          REPLY="$(at_command "at+gtfcclockver=$RESPONSE")"

          # the vendor does not match a response prefix: it parses the value with
          # strtoul and requires 1
          RESULT="$(echo "$REPLY" | grep -o '[0-9][0-9]*' | tail -1)"
          if [ "$RESULT" = '1' ]; then
              log "  FCC unlock: SUCCESS (Lenovo method, hash $VENDOR_ID_HASH)"
              at_command 'at+gtfcclockmodeunlock' >/dev/null
              log "  FCC lock state: $(at_command 'at+gtfcclockstate')"
              return 0
          fi
          log "  attempt $i: hash $VENDOR_ID_HASH refused, reply: $REPLY"
          break
      else
          log "  attempt $i: no challenge, reply: $RAW"
          return 1
      fi
      sleep 0.5
      i="$((i + 1))"
      done
    done
    return 1
}

SYS_VENDOR="$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null)"
case "$(echo "$SYS_VENDOR" | tr '[:upper:]' '[:lower:]')" in
    *lenovo*) METHODS='lenovo' ;;
    *dell*)   METHODS='dell' ;;
    *)        METHODS='lenovo dell' ;;
esac

log "invoked: AT=${AT:-none} MBIM=${MBIM:-none} vendor=${SYS_VENDOR:-unknown} methods='$METHODS'"
for METHOD in $METHODS; do
    if "unlock_$METHOD"; then
        log "result rc=0"
        exit 0
    fi
done

log "result rc=2 (no method unlocked the modem)"
exit 2
