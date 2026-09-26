# Unlock traces

One file per card and OEM tool: the complete unlock routine, traced from the
OEM binary's entry point to the bytes the modem receives, with the addresses
each step was read at. A dispatcher is written from these, and every place it
departs from the OEM is listed at the end of the trace with the reason.

The rule for a trace: every branch reachable from the routine's entry, every
callee that decides what goes on the wire, every log string resolved. Nothing
carried over from another card or another OEM's tool.

[oem-packages.md](oem-packages.md) inventories every vendor package examined,
including the ones that turned out to carry no unlock at all.

## Recorded

| PCI id | Card | OEM tool | Trace |
|---|---|---|---|
| `8086:7560` | Intel L860R+ | Lenovo `libmodemauth.so` | [8086-7560-lenovo-l860r.md](8086-7560-lenovo-l860r.md) |
| `8086:7560` | Dell DW5823e | Dell `ModemAuthenticator.exe` | [8086-7560-dell-dw5823e.md](8086-7560-dell-dw5823e.md) |
| `8086:7560` | HP variants | HP packages: none ships an unlock | [8086-7560-hp.md](8086-7560-hp.md) |
| `03f0:09c8`, and `14c3:4d75` on Dell and HP | HP DRMR-H01 / WNC M80 | HP `WWANModemAuthenticator.exe` | [hp-wwanmodemauthenticator.md](hp-wwanmodemauthenticator.md) |

`8086:7560` is complete: all three OEMs that ship the module are accounted for,
and the dispatcher covers Lenovo and Dell with their own sequences.

## Not yet recorded to this standard

| PCI id | Card | OEM tool to trace | What is needed |
|---|---|---|---|
| `14c3:4d75` | Fibocom FM350-GL, Rolling RW350R-GL | Lenovo `libmodemauth.so`, `event_monitor_at_fm350` | Full trace like the L860R+ one; the existing notes predate the current standard |
| `14c3:4d75` | Dell DW5931e | Dell `IntelWWANModemAuthenticator.exe` | `4909b5a4` now has a binary source via HP's shared authenticator; Dell's own tool still to be read |
| `14c0:4d75` | Dell DW5933e | Dell `WWANModemAuthenticator.exe` | Traced when the script was written, hardware confirmed; write up to this standard |
| `8086:7360` | Intel XMM7360 | Lenovo, plus HP sp148979 for context | In-tree script uses XMM RPC and Lenovo's key; HP ships no unlock |
| `105b:e0f5`, `105b:e0f9` | Dell DW5932e | Dell, Foxconn magic string scheme | Traced when the script was written; write up |
| `17cb:0308` | Foxconn T99W696 | Lenovo `libfiisdk.so.2.2.2` | Findings exist in `T99W696-FCC-unlock-findings.md`; fold into this format |
| `33f8:*` | Rolling RW101R-GL | Lenovo `libmodemauthRW101.so.1.1`, `event_monitor_at_101r` | Partially traced; the dispatcher's verdict handling needs correcting first |
| RW350 | Rolling RW350 | Lenovo `libmodemauth.so.1.1` | Which device case it resolves to is unestablished |
| `1eac:*`, `2c7c:*` | Quectel | Lenovo `libmbimtools.so` | Mechanism known, trace not written up |

## Tools

[tools/](tools/) holds the scripts these traces were produced with:
`elfdis.py` for the Lenovo ELF libraries, `mainwalk.py` for walking
`DPR_Fcc_unlock_service`'s dispatch with a module code fixed, and `pe.py` for
the Windows binaries, which does `.pdata` function bounds, a resilient linear
sweep, cross references and a reachability walk.
