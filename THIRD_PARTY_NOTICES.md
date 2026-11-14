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

The Qt/C++ product dynamically links Qt 6 Core, Widgets, Network and Concurrent
under the applicable Qt LGPLv3/GPLv3/commercial terms, and QtKeychain under its
Modified BSD license. The legacy probe does not link Qt. Preserve the system
packages' license texts and provide the required relinking/source information
when distributing binaries. The additional FreeRDP product patch is maintained
in `patches/freerdp-3.33.1-product.patch` under Apache-2.0; upstream copyrights
remain unchanged. Product-specific build manifests record both patch hashes.

Complete transitive license inventory and distribution review remain release
work. No product `.deb` or Windows helper installer has been published.
