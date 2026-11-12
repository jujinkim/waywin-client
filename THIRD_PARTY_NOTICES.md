# Third-party components

WayWin-authored code is Apache-2.0; see `LICENSE`.

The feasibility build downloads FreeRDP 3.33.1 at commit
`65bcca1913a28cbdd7eb913599f692bd2b707991` from
https://github.com/FreeRDP/FreeRDP. FreeRDP and WinPR retain their upstream
copyright notices and Apache-2.0 license. The SDL3 RAIL file being patched is
copyright 2026 Ibrahim Sevinc. The WayWin observer patch is maintained separately
in `patches/freerdp-3.33.1-probe.patch`; the original source is not vendored here.

SDL3 and SDL3_ttf are supplied by the build system's packages under their zlib
licenses. Font, crypto, JSON, OS and optional media dependencies retain their own
licenses. Keep package copyright files and upstream notices when preparing a
redistributable binary. The current build manifest inventories key versions;
it is not a complete release SBOM or a license audit.

Qt is the approved future manager dependency and is not linked by the current
probe. Product packaging, complete transitive license inventory and distribution
review remain stage-3 work. No product `.deb` or Windows helper installer has been
published.
