#!/bin/sh
# SPDX-License-Identifier: MIT
#
# wwan-unlock installer.
#
#   sudo ./install.sh                 # detect the modem, install matching module
#   sudo ./install.sh --id 17cb:0308  # force a specific module
#   ./install.sh --list               # list bundled modules (no root needed)
#   ./install.sh --detect             # just report what hardware is present
#   sudo ./install.sh --no-sar        # unlock only, skip Lenovo's SAR tool
#   sudo ./install.sh --sar-only      # re-apply SAR only (gate-free)
#   sudo ./install.sh --uninstall     # remove everything this installed
#
# Installs:
#   /usr/local/lib/wwan-unlock/<helper>          compiled C helpers (if any)
#   /etc/ModemManager/fcc-unlock.d/<id>          the dispatcher ModemManager runs
#
# Any pre-existing fcc-unlock.d entry is backed up to <id>.orig once.

set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
MODDIR="$SRC/modules"
LIBDIR="/usr/local/lib/wwan-unlock"
FCCDIR="/etc/ModemManager/fcc-unlock.d"

die()  { echo "error: $*" >&2; exit 1; }
info() { echo "$*"; }

need_root() { [ "$(id -u)" = 0 ] || die "run with sudo"; }

# ---- hardware detection ----------------------------------------------------
# Emit every <vendor>:<device> present on PCI and USB, lowercase.
present_ids() {
    for d in /sys/bus/pci/devices/*; do
        [ -r "$d/vendor" ] || continue
        v=$(cut -c3- < "$d/vendor"); p=$(cut -c3- < "$d/device")
        echo "$v:$p"
    done 2>/dev/null
    for d in /sys/bus/usb/devices/*; do
        [ -r "$d/idVendor" ] || continue
        echo "$(cat "$d/idVendor"):$(cat "$d/idProduct")"
    done 2>/dev/null
}

list_modules() {
    for m in "$MODDIR"/*/; do
        [ -f "$m/module.conf" ] || continue
        # shellcheck disable=SC1091
        ( . "$m/module.conf"
          printf '  %-12s %-38s [%s]\n' "$MODULE_IDS" "$MODULE_NAME" "$MODULE_STATUS" )
    done
}

# Find a bundled module matching hardware that is actually present.
detect_module() {
    _ids=$(present_ids)
    for m in "$MODDIR"/*/; do
        [ -f "$m/module.conf" ] || continue
        _id=$(basename "$m")
        _mids=$( . "$m/module.conf"; echo "$MODULE_IDS" )
        for _cand in $_id $_mids; do
            if echo "$_ids" | grep -qix "$_cand"; then echo "$_id"; return 0; fi
        done
    done
    return 1
}

# Upstream ModemManager ships fcc-unlock scripts in an "available" dir; enabling
# one is a symlink into the active dir. Prefer these when they exist: they are
# maintained and tested by the ModemManager project, not by us.
MM_AVAIL=""
for d in /usr/share/ModemManager/fcc-unlock.available.d \
         /usr/lib/ModemManager/fcc-unlock.available.d; do
    [ -d "$d" ] && MM_AVAIL="$d" && break
done

# Echo the upstream script that covers <id> (exact match, then vendor-wide), or
# nothing. ModemManager itself matches vendor:device first, then vendor.
upstream_for() {
    _id="$1"; _vid="${_id%%:*}"
    [ -n "$MM_AVAIL" ] || return 1
    [ -e "$MM_AVAIL/$_id" ] && { echo "$MM_AVAIL/$_id"; return 0; }
    [ -e "$MM_AVAIL/$_vid" ] && { echo "$MM_AVAIL/$_vid"; return 0; }
    return 1
}

enable_upstream() {
    _id="$1"; _src="$2"
    need_root
    mkdir -p "$FCCDIR"
    _dst="$FCCDIR/$(basename "$_src")"
    if [ -e "$_dst" ] && [ ! -L "$_dst" ] && [ ! -e "$_dst.orig" ]; then
        cp -a "$_dst" "$_dst.orig"; info "backed up existing $_dst -> .orig"
    fi
    ln -sf "$_src" "$_dst"
    info "enabled upstream ModemManager unlock: $_dst -> $_src"
    systemctl restart ModemManager
    info "done. ModemManager's own unlock for $_id is now active."
}

# ---- Lenovo runtime libraries (bundled unmodified; see vendor/lenovo/NOTICE.md)
# wwan-orch loads these at runtime. We ship ONLY the libraries and SAR tables —
# NOT Lenovo's gated orchestrator binaries, which wwan-orch replaces (minus the
# US-SIM gate). Nothing here is modified.
FCC_LENOVO=/opt/fcc_lenovo
VENDOR="$SRC/vendor/lenovo"

install_lenovo_runtime() {
    [ -d "$VENDOR" ] || die "missing vendor/lenovo runtime"
    mkdir -p "$FCC_LENOVO/lib"
    cp -f "$VENDOR"/lib/*.so* "$FCC_LENOVO/lib/"
    tar -xzf "$VENDOR/sar_config_files.tar.gz" -C "$FCC_LENOVO/" 2>/dev/null || true
    cp -f "$VENDOR/LICENSE-Lenovo.txt" "$VENDOR/ThirdPartyNotices.txt" \
          "$VENDOR/NOTICE.md" "$FCC_LENOVO/" 2>/dev/null || true
    # EM05-G DPR band tables: extract the embedded DPRConfig.xml from Lenovo's own
    # (unmodified) configservice_lenovo. It is a single contiguous XML blob; we carve
    # it out by content markers so no offset is hard-coded. Only used by EM05-G SAR.
    if [ -f "$VENDOR/configservice_lenovo" ]; then
        python3 - "$VENDOR/configservice_lenovo" "$FCC_LENOVO/DPRConfig.xml" <<'PY' 2>/dev/null || true
import sys
d = open(sys.argv[1], "rb").read()
s = d.find(b'<?xml version="1.0" encoding="utf-8"?>\r\n<COLLECTION')
e = d.find(b'</COLLECTION>', s)
if s >= 0 and e > s:
    open(sys.argv[2], "wb").write(d[s:e+len(b'</COLLECTION>')] + b'\r\n')
PY
    fi
    chmod -R a+rX "$FCC_LENOVO"
    info "installed Lenovo runtime libs -> $FCC_LENOVO (unmodified; vendor/lenovo/NOTICE.md)"
}

# Gate-free SAR via our orchestrator + Lenovo's own Set_RF_Files. Runs once now and
# installs a boot-time oneshot so it re-applies on every boot (like Lenovo's own).
run_sar() {
    _fam="$1"
    info ""
    info "applying RF/SAR via wwan-orch (US-SIM gate skipped)..."
    sleep 6
    # Auto-detect the modem control node like stock configservice_lenovo does
    # (usbdeviceExists). USB Quectel families (em05/cs24) prefer cdc-wdm0 — stock
    # scans only /dev/cdc-wdm* — others prefer the wwan-subsystem node.
    case "$_fam" in
        em05|cs24) _order="/dev/cdc-wdm0 /dev/wwan0mbim0 /dev/cdc-wdm* /dev/wwan*mbim*" ;;
        *)         _order="/dev/wwan0mbim0 /dev/cdc-wdm0 /dev/wwan*mbim* /dev/cdc-wdm*" ;;
    esac
    _sardev=""
    for _d in $_order; do
        [ -e "$_d" ] && { _sardev="$_d"; break; }
    done
    if [ -n "$_sardev" ]; then
        "$LIBDIR/wwan-orch" --sar --family "$_fam" -d "$_sardev" 2>&1 | sed 's/^/  /' || \
            info "  (SAR step returned nonzero — usually 'already current' or no table)"
    else
        "$LIBDIR/wwan-orch" --sar --family "$_fam" 2>&1 | sed 's/^/  /' || \
            info "  (SAR step returned nonzero — usually 'already current' or no table)"
    fi
    printf 'ORCH_FAMILY=%s\n' "$_fam" > "$LIBDIR/orch.conf"
    install -m0644 "$SRC/systemd/wwan-sar.service" /etc/systemd/system/wwan-sar.service
    systemctl daemon-reload
    systemctl enable wwan-sar.service >/dev/null 2>&1 || true
    info "  enabled wwan-sar.service (re-applies SAR each boot)"
}

remove_lenovo_runtime() {
    if [ -f /etc/systemd/system/wwan-sar.service ]; then
        systemctl disable wwan-sar.service >/dev/null 2>&1 || true
        rm -f /etc/systemd/system/wwan-sar.service
        systemctl daemon-reload
        info "removed wwan-sar.service"
    fi
    [ -d "$FCC_LENOVO" ] && { rm -rf "$FCC_LENOVO"; info "removed $FCC_LENOVO"; }
}

# ---- actions ---------------------------------------------------------------
do_uninstall() {
    need_root
    remove_lenovo_runtime
    for m in "$MODDIR"/*/; do
        [ -f "$m/module.conf" ] || continue
        _mids=$( . "$m/module.conf"; echo "$MODULE_IDS" )
        for _id in $(basename "$m") $_mids; do
            if [ -e "$FCCDIR/$_id" ] || [ -L "$FCCDIR/$_id" ]; then
                rm -f "$FCCDIR/$_id"; info "removed $FCCDIR/$_id"
                [ -e "$FCCDIR/$_id.orig" ] && { mv "$FCCDIR/$_id.orig" "$FCCDIR/$_id"
                                                info "restored original $FCCDIR/$_id"; }
            fi
        done
    done
    rm -rf "$LIBDIR" && info "removed $LIBDIR"
    systemctl restart ModemManager 2>/dev/null || true
    info "done."
    exit 0
}

install_module() {
    _id="$1"; _dir="$MODDIR/$_id"
    [ -f "$_dir/module.conf" ] || die "no bundled module for '$_id' (try --list)"
    # shellcheck disable=SC1091
    . "$_dir/module.conf"

    info "module : $MODULE_NAME"
    info "id     : $MODULE_IDS"
    info "status : $MODULE_STATUS"
    [ "$MODULE_STATUS" = "verified" ] || cat <<EOF

  !! This module is UNVERIFIED. It was derived from analysis but has never been
  !! run on real hardware by the maintainers. It may not work, and a failed FCC
  !! unlock leaves the radio disabled. Continue only if you can recover.
EOF
    need_root

    # build + install the gateless orchestrator and Lenovo's runtime libraries.
    # (The standalone foxunlock tool is never built here — it is a separate
    #  `make foxunlock` deliverable, independent of the installer.)
    if [ -n "${MODULE_ORCH_FAMILY:-}" ]; then
        command -v cc >/dev/null || die "cc not found (apt install build-essential)"
        mkdir -p "$LIBDIR"
        info "building wwan-orch ..."
        ( cd "$SRC" && make -s wwan-orch ) || die "build failed for wwan-orch"
        install -m0755 "$SRC/wwan-orch" "$LIBDIR/wwan-orch"
        install -m0755 "$SRC/tools/wwan-sar-boot.sh" "$LIBDIR/wwan-sar-boot.sh"
        info "installed $LIBDIR/wwan-orch"
        install_lenovo_runtime
    fi

    # install dispatcher
    mkdir -p "$FCCDIR"
    if [ -e "$FCCDIR/$_id" ] && [ ! -e "$FCCDIR/$_id.orig" ]; then
        cp -a "$FCCDIR/$_id" "$FCCDIR/$_id.orig"
        info "backed up existing entry -> $FCCDIR/$_id.orig"
    fi
    for _one in ${MODULE_IDS:-$_id}; do
        if [ -e "$FCCDIR/$_one" ] && [ ! -e "$FCCDIR/$_one.orig" ]; then
            cp -a "$FCCDIR/$_one" "$FCCDIR/$_one.orig"
            info "backed up existing entry -> $FCCDIR/$_one.orig"
        fi
        install -m0755 "$_dir/$MODULE_DISPATCHER" "$FCCDIR/$_one"
        info "installed $FCCDIR/$_one"
    done

    info "restarting ModemManager (full port re-probe, ~30s; not a hang)..."
    systemctl restart ModemManager

    # ---- RF/SAR: gate-free, via wwan-orch + Lenovo's own Set_RF_Files ------
    if [ "${MODULE_LENOVO_SAR:-}" = "yes" ] && [ "${opt_no_sar:-}" != "1" ] \
       && [ -n "${MODULE_ORCH_FAMILY:-}" ]; then
        run_sar "$MODULE_ORCH_FAMILY"
    fi

    cat <<EOF

done. The unlock now runs automatically whenever the modem powers on
(boot, resume, modem reset).

verify with:
  journalctl -t fcc-unlock-foxconn -b
  mmcli -m any | grep -iE 'state:|power state'
EOF
}

# Install support for one id, choosing the best available source:
#   1. upstream ModemManager script, if it covers the id  (maintained + tested)
#   2. our bundled module                                 (verified or not)
# Pass --prefer-ours to invert (1) and (2) for a given id.
provision() {
    _id="$1"
    if [ "${2:-}" != "prefer-ours" ] && _up=$(upstream_for "$_id"); then
        info "id $_id: ModemManager already ships an unlock ($(basename "$_up"))"
        enable_upstream "$_id" "$_up"
        return 0
    fi
    if [ -f "$MODDIR/$_id/module.conf" ]; then
        install_module "$_id"
        return 0
    fi
    # our module absent but upstream exists and we skipped it: fall back to upstream
    if _up=$(upstream_for "$_id"); then
        enable_upstream "$_id" "$_up"; return 0
    fi
    die "no unlock available for '$_id' (not upstream, no bundled module)"
}

# Report disposition for everything we could act on this system.
detect_all() {
    _ids=$(present_ids)
    _found=0
    for m in "$MODDIR"/*/; do
        [ -f "$m/module.conf" ] || continue
        _id=$(basename "$m")
        if echo "$_ids" | grep -qix "$_id"; then
            src="bundled module"; up=$(upstream_for "$_id") && src="upstream MM + bundled module"
            info "  $_id  -> $src"; _found=1
        fi
    done
    # upstream-covered cards we don't ship a module for
    for id in $_ids; do
        [ -f "$MODDIR/$id/module.conf" ] && continue
        if up=$(upstream_for "$id"); then info "  $id  -> upstream MM ($(basename "$up"))"; _found=1; fi
    done
    [ "$_found" = 1 ] || info "  (nothing this tool can act on was detected)"
}

# ---- argument handling -----------------------------------------------------
# --no-sar anywhere on the line skips SAR. Filter it out WITHOUT re-splitting or
# glob-expanding the remaining arguments (rotate the positional params, quoted).
opt_no_sar=0
_argc=$#
while [ "$_argc" -gt 0 ]; do
    _a="$1"; shift; _argc=$((_argc - 1))
    if [ "$_a" = "--no-sar" ]; then opt_no_sar=1; else set -- "$@" "$_a"; fi
done

case "${1:-}" in
    --list)      info "bundled modules:"; list_modules; exit 0 ;;
    --detect)    info "detected WWAN unlock options:"; detect_all; exit 0 ;;
    --sar-only)  need_root
                 [ -f "$LIBDIR/orch.conf" ] && . "$LIBDIR/orch.conf"
                 [ -n "${ORCH_FAMILY:-}" ] || die "no module installed yet (run install first)"
                 install_lenovo_runtime; run_sar "$ORCH_FAMILY"; exit 0 ;;
    --uninstall) do_uninstall ;;
    --id)        [ -n "${2:-}" ] || die "--id needs an argument"
                 provision "$2" "${3:-}" ;;
    --prefer-ours) [ -n "${2:-}" ] || die "--prefer-ours needs an id"
                 provision "$2" prefer-ours ;;
    "")          if id=$(detect_module); then
                     provision "$id"
                 elif id=$(present_ids | while read x; do upstream_for "$x" >/dev/null && { echo "$x"; break; }; done) && [ -n "$id" ]; then
                     info "detected $id (upstream-covered)"; provision "$id"
                 else
                     echo "no supported modem detected." >&2
                     echo "bundled modules:" >&2; list_modules >&2
                     echo "force one with: sudo $0 --id <vendor:device>" >&2
                     exit 1
                 fi ;;
    -h|--help)   sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)           die "unknown option '$1' (try --help)" ;;
esac
