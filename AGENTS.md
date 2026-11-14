# Repository Guidelines

## Project Structure & Module Organization
WayWin now develops the Qt/C++ product and Ubuntu/Windows validation in parallel, per the user instruction on 2026-11-14. Real-platform evidence remains required for compatibility and release claims. See `docs/product.md` for implemented and outstanding features.

- `app/`: native Qt product, session worker, host settings and GUI validation reports.
- `tests/product/`: QtTest GUI, privacy, storage and process-isolation contracts.
- `cmake/ProductRuntime.cmake`: pinned native runtime build without Python.
- `tools/probe.py`: Python CLI for dependency builds, environment checks, session launches, and evidence assessment.
- `windows/probe/`: C++ Win32 test app, resources, and manifest.
- `tests/test_probe.py`: automated probe contracts.
- `cmake/`, `patches/`, `dependencies.lock.json`: build profiles, FreeRDP instrumentation, and pinned provenance.
- `docs/`: design, acceptance criteria, validation procedures, and report template; `task.md` tracks progress.
- `.cache/`, `build/`, and `out/`: ignored dependencies, binaries, and local evidence.

## Build, Test, and Development Commands
Use Python 3.10+, CMake 3.22+, and Ninja. Run from the repository root:

```sh
cmake -S . -B build/product -G Ninja -DWAYWIN_BUILD_RUNTIME=ON
cmake --build build/product --parallel 4
ctest --test-dir build/product --output-on-failure

python3 tools/probe.py --help                 # Discover CLI operations
python3 tools/probe.py build                  # Fetch, verify, patch, and build FreeRDP
python3 tools/probe.py doctor                 # Check the target Wayland environment
cmake -S . -B build/checks -G Ninja -DWAYWIN_BUILD_APP=OFF            # Configure automated checks
ctest --test-dir build/checks --output-on-failure
python3 -m unittest discover -s tests -v      # Run tests directly
```

For a native Windows build in a Visual Studio C++ environment:

```sh
cmake -S . -B build/windows-native -A x64 -DBUILD_TESTING=OFF
cmake --build build/windows-native --config Release
```

See `docs/validation.md` for platform dependencies and session commands.

## Coding Style & Naming Conventions
Use four-space indentation in Python/C++ and two spaces in CMake. Follow Python `snake_case` functions, `PascalCase` classes, and `UPPER_SNAKE_CASE` constants; C++ uses `camelCase` functions/variables. Target C++17. No formatter or linter is configured; Windows builds treat compiler warnings as errors. Preserve unified-patch whitespace and update the lockfile hash when changing the FreeRDP patch.

## Testing Guidelines
Use standard-library `unittest`, `test_*.py` files, and `test_*` methods. Add regression coverage for changed validation, privacy, provenance, and process-isolation behavior. No numerical coverage threshold is configured. Record actual platform evidence separately: macOS builds and mocks do not establish Wayland compatibility.

## Commit & Pull Request Guidelines
History uses `feat:`, `docs:`, and `chore:` subjects. Keep commits focused and update `task.md` with results. PRs should explain behavior, link relevant issues or requirement IDs, and list checks and remaining platform gaps. Include sanitized screenshots for GUI changes. Remote pushes require explicit authorization.

## Security & Agent Instructions
Keep credentials out of arguments, logs, and reports; preserve certificate validation. On this PC, follow `/Users/jujin/.codex/RTK.md` and prefix shell commands with `rtk` or `rtk proxy`. Prefer CLI checks; use computer interaction only for necessary verification with predefined exit conditions.

Product additions: windows/companion/ is the native DVC helper; protocol/ is the bounded wire contract; runtime/ is the canonical product source embedded in the FreeRDP patch. Regenerate it with tools/update_product_patch.py after synchronizing a reviewed checkout. packaging/install-linux.sh provides Ubuntu GUI installation. Keep transient images, titles, and clipboard payloads out of evidence files.
