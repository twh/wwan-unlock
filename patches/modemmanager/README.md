# ModemManager patches

Fixes to ModemManager itself, kept here until they are merged upstream. They
are not part of the unlock and `install.sh` does not apply them.

## 0001-broadband-modem-mbim-don-t-re-request-automatic-regi.patch

**Symptom:** on an Intel L860R+ (`8086:7560`), every radio power on (boot,
resume, modem reset) waits about 60 s before connecting, with
`registration in network failed: Network timeout` in the journal, after which
a retry registers instantly.

**Cause:** at the register step ModemManager sends an MBIM
`register-state` set (`automatic`) while the modem is still in its first
network search. Firmware `18601.5001.00.01.01.32_GC` registers on its own and
says so through indications, but never answers the set, so ModemManager waits
out its 60 s command timeout and treats that as a failed registration.

**Fix:** keep the register mode the modem reports, and skip the set when
automatic registration with no data class change is requested and the modem
already reports automatic mode.

**Result**, ThinkPad X13 Gen 3, modem reset to connected: 115 s before, 53 s
after.

Written against ModemManager 1.24.0; not yet submitted upstream.

### Building it on Debian 13

```sh
mkdir mm && cd mm
apt-get source modemmanager
sudo apt-get build-dep modemmanager
cd modemmanager-1.24.0
cp /path/to/wwan-unlock/patches/modemmanager/0001-*.patch debian/patches/
ls debian/patches/0001-broadband-*.patch | xargs -n1 basename >> debian/patches/series
QUILT_PATCHES=debian/patches quilt push -a
dch --local +wwanunlock "Skip the redundant MBIM register state set (L860R+ 60s stall)"
DEB_BUILD_OPTIONS="nocheck nodoc" dpkg-buildpackage -b -us -uc
sudo apt install ../modemmanager_*wwanunlock*_amd64.deb ../libmm-glib0_*wwanunlock*_amd64.deb
```

Any later Debian update of `modemmanager` replaces the local build and brings
the stall back. `sudo apt-mark hold modemmanager libmm-glib0` prevents that,
at the cost of also holding back security fixes until you rebuild.
