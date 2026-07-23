# Lenovo WWAN runtime libraries — redistributed, UNMODIFIED

Everything in this directory is **Lenovo's software, copied bit-for-bit unmodified**
from Lenovo's `lenovo-wwan-unlock` package. It is **not** part of this project's
clean-room work and is **not** covered by this project's MIT license.

- `lib/` — the runtime libraries that do the actual FCC unlock and SAR config
  (`libfiisdk`, `libmbimtools`, `libconfigservice*`, `libmodemauth*`)
- `sar_config_files.tar.gz` — Lenovo's per-chassis RF/SAR tables
- `LICENSE-Lenovo.txt` — the governing license
- `ThirdPartyNotices.txt` — third-party components (Fibocom is BSD-3-Clause, etc.)

## What is deliberately NOT here

Lenovo's FCC orchestrator `DPR_Fcc_unlock_service` is **not** bundled. Its SAR
counterpart `configservice_lenovo` **is** bundled, but **only as a data source**:
it is the sole place Lenovo ships the EM05-G DPR band tables (a `DPRConfig.xml` blob
embedded in the binary), which the installer extracts at install time. It is shipped
**unmodified** and is **never executed** by this project — `wwan-orch` replaces its
logic. Both orchestrators are the only pieces that contain the US-SIM country gate;
this project reimplements that logic in its own clean-room `wwan-orch`, which loads
the libraries below and calls the same functions **without** the gate. So the gate is
never invoked and nothing is patched.

## Why redistributing the libraries is allowed

The Lenovo license explicitly permits it, and forbids only modification:

> "Lenovo … hereby grants a royalty free license to **use and distribute** the
> Software … You may **not modify** the Software's binary application or libraries …"
> — `LICENSE-Lenovo.txt`

These libraries are shipped exactly as published. `wwan-orch` `dlopen()`s them and
calls their public functions (`ModuleConnect`, `Fox_Attempt`, `Set_RF_Files`, …) —
using them, not modifying them.

Provenance: Lenovo `lenovo-wwan-unlock`, revision per `ThirdPartyNotices.txt`.
