#!/bin/sh
# Boot-time SAR re-apply. Reads the installed family and runs wwan-orch --sar.
# Each family's apply path compares-and-skips when the modem is already current
# (fxn: Set_RF_Files; cs24/em05-cn: md5 vs EFS; em05-g: dprconfig/commit), so
# this is a near-instant no-op on a provisioned modem.
LIB=/usr/local/lib/wwan-unlock
[ -f "$LIB/orch.conf" ] && . "$LIB/orch.conf"
[ -n "${ORCH_FAMILY:-}" ] || { logger -t wwan-sar "no ORCH_FAMILY; skipping"; exit 0; }

# Match stock configservice_lenovo, which auto-detects the modem control node.
# For the USB Quectel families (em05/cs24) stock's usbdeviceExists scans ONLY
# /dev/cdc-wdm0..3, so it effectively prefers cdc-wdm0 — probe that first there.
# Other families are PCIe/wwan-subsystem (the hw-verified fxn uses wwan0mbim0),
# so probe wwan0mbim0 first. Falls back to no -d (built-in default) if none exist.
case "$ORCH_FAMILY" in
    em05|cs24) _order="/dev/cdc-wdm0 /dev/wwan0mbim0 /dev/cdc-wdm* /dev/wwan*mbim*" ;;
    *)         _order="/dev/wwan0mbim0 /dev/cdc-wdm0 /dev/wwan*mbim* /dev/cdc-wdm*" ;;
esac
DEV=""
for d in $_order; do
    [ -e "$d" ] && { DEV="$d"; break; }
done

if [ -n "$DEV" ]; then
    "$LIB/wwan-orch" --sar --family "$ORCH_FAMILY" -d "$DEV" 2>&1 | logger -t wwan-sar
else
    "$LIB/wwan-orch" --sar --family "$ORCH_FAMILY" 2>&1 | logger -t wwan-sar
fi
exit 0
