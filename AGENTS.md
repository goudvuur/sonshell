# Repository Guidelines

## Project Structure & Module Organization
- `src/main.cpp` is the entire runtime (SDK bridge, REPL, live view, hooks); keep additions small and well-fenced.
- Root CMake glue (`CMakeLists.txt`, `INSTALL.md`) defines the build; `build*/` stays untracked while automation lives in `scripts/`.
- The Python generators in `tools/` refresh the SDK-derived headers next to `main.cpp`; rerun them whenever Sony ships new enums.
- You can find the Sony SDK API in the `docs/CrSDK_v2.00.00_api` subdirectory.
- You can find a Sony CLI sample/reference app in the `docs/CrSDK_v2.00.00_app` subdirectory

## Build, Test & Development Commands
- Configure with `cmake -S . -B build -DSONY_SDK_DIR="/abs/path/CrSDK_v2.00.00_20250805a_Linux64PC"` (use `~/Projects/a6700/sdk/CrSDK_v2.00.00_20250805a_Linux64PC` when unset) and add `-DSONSHELL_HEADLESS=ON` if GTK/OpenCV are absent.
- Build via `cmake --build build --config Release`; rebuild after every edit so compile errors surface immediately.
- Run `./build/sonshell --sync-dir "$PWD/photos" --keepalive 3000` (optionally `--cmd scripts/broadcast.sh`) and regenerate helper tables with the Python scripts in `tools/`.

## Coding Style & Naming Conventions
- Stick to C++17, two-space indentation, same-line braces, explicit `std::` qualifiers, and RAII wrappers—no raw SDK ownership.
- Functions use `snake_case`, globals stay `g_foo`, constants are `kFoo`; prefer `<filesystem>` paths and `std::chrono` durations over ad-hoc strings.
- Include only what you use, forward-declare SDK types when practical, and reserve comments for non-obvious coordination.

## Testing Guidelines
- Every feature needs a deterministic smoke path (documented in `docs/` and ideally scripted in `scripts/`) until a proper automated suite exists.
- Wire new tests into CTest (`ctest --test-dir build`) and keep fixtures under `build/tests_data` to avoid bloating git history.
- Exercise hook-heavy flows with `./build/sonshell --cmd scripts/broadcast.sh --sync-dir /tmp/captures` and attach the resulting logs to the PR.

## Commit & Pull Request Guidelines
- Write imperative, subsystem-leading summaries (`monitor: guard empty live view`) and add short bullets for multi-part commits.
- Reference scripts/docs whenever CLI flags or environment contracts (e.g., `SONY_SDK_DIR`) change, call out required hardware/firmware, and include repro steps plus screenshots/logs for UI or live-view tweaks.

## Security & Configuration Tips
- Never commit Sony SDK archives, proprietary `.so` files, or cached camera fingerprints; contributors should set `SONY_SDK_DIR` locally.
- Scrub credentials/serials from examples, and remind reporters to sanitize `~/.cache/sonshell/` before sharing logs.

## Hardware Input Mapping
- YAML shortcut support defaults to `~/.config/sonshell/input-map.yaml` (override with `--input-map PATH`) and maps `/dev/input/eventX` devices to commands such as `KEY_LEFT: button dpad left`.
- Bindings fire on key-down through the REPL dispatcher, so keep them idempotent and document any workflow dependencies.
- Ensure contributors can read the chosen input node without sudo (udev rule or group membership) before relying on a shortcut.
