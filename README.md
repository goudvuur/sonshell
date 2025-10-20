# SonShell - an effort to "ssh into my Sony camera"

A Linux-only helper built on Sony’s official **Camera Remote SDK**. It connects to supported Sony bodies (the full SDK model list is recognised) over Wi-Fi or Ethernet, mirrors new captures straight to your workstation, and drops you into an interactive shell for remote control.

The shell can download files automatically, trigger the shutter, tweak exposure settings, start live view, run post-download hooks, and keep retrying if the camera drops offline. Everything runs from a single terminal window.

---

## Demo

https://github.com/user-attachments/assets/6146ff3b-d51c-412b-8684-bdde5c418d4d

---

## Quick Start

### Requirements
- Linux (developed on Ubuntu 24.04) with a C++17 toolchain (`gcc`, `g++`, `cmake`, `make`).
- Sony Camera Remote SDK v2.00.00 (download it from Sony and extract it somewhere convenient).
- Python 3 for the small header-generation scripts.
- `pkg-config` (or `pkgconf`) so CMake can locate GTK when linking Sony’s OpenCV bundle (omit when configuring with `-DSONSHELL_HEADLESS=ON`).
- Runtime deps: libedit, ncurses, libudev, libxml2, OpenCV 4.8 (bundled inside Sony’s SDK).

On Ubuntu/Debian you can grab the basics with:
```bash
sudo apt install autoconf libtool libudev-dev gcc g++ make cmake unzip libxml2-dev libedit-dev python3 pkg-config
```

### Build in a hurry
1. Download and extract the Sony Camera Remote SDK v2.00.00, then configure CMake while pointing `SONY_SDK_DIR` at the folder that contains `app/`:
   ```bash
   cmake -S . -B build -DSONY_SDK_DIR="$HOME/SonySDK/CrSDK_v2.00.00_20250805a_Linux64PC"
   ```
2. Compile and copy the required Sony/OpenCV shared libraries next to the binary:
   ```bash
   cmake --build build --config Release
   ```
3. Run it (start with enumeration and let SonShell pick the download folder):
   ```bash
   ./build/sonshell --dir "$PWD/photos" --keepalive 3000
   ```

### Headless builds

If you are compiling on a machine without a GUI stack, pass `-DSONSHELL_HEADLESS=ON` to the CMake configure step. This skips the OpenCV/GTK dependencies and disables the live-view `monitor` command. The binary prints a reminder at startup and any `monitor` invocation warns that the build is headless.

The build copies `libCr_*`, the adapter modules, and Sony’s OpenCV libs into `build/`. Run the binary from inside `build/` (or keep the copied `.so` files alongside it) so live view keeps working.

---

## Command-Line Options

| Option | Description |
| --- | --- |
| `--dir <path>` | Directory where downloads are stored. If omitted, files land in the working directory; providing an explicit folder is strongly recommended for sync features. |
| `--ip <addr>` | Connect directly to a camera at the given IPv4 address (e.g. `192.168.1.1`). Skipped when enumerating automatically. |
| `--mac <hex:mac>` | Optional MAC address for direct-IP connects (`aa:bb:cc:dd:ee:ff`). Used to seed the SDK’s Ethernet object. |
| `--model <name>` | Optional camera model hint for direct-IP connects (e.g. `a7r5`, `fx3`, `zve1`). Enumeration ignores this flag and always picks the first discovered device. |
| `--user <name>` | Username for cameras with Access Auth enabled. |
| `--pass <password>` | Password for Access Auth. Combine with `--user`. |
| `--cmd <path>` | Executable/script that SonShell calls for every file event (new downloads, syncs, rating changes, …). Arguments: `<path> <mode> <operation> [new] [old]`. Runs asynchronously; SonShell does not wait for completion. |
| `--keepalive <ms>` | Reconnection delay after failure or disconnect. `0` disables retry (SonShell exits on error). |
| `--verbose`, `-v` | Print detailed property-change logs and transfer progress from the SDK callbacks. |

If no `--ip` is provided SonShell enumerates available cameras and uses the first match. A fingerprint of the successful connection is cached under `~/.cache/sonshell/fp_enumerated.bin` so subsequent launches pair faster.

---

## Hook Events

When `--cmd` is provided SonShell calls the hook for every file-affecting event. The hook always receives:

```
<path> <mode> <operation> [new] [old]
```

- `path` – absolute path to the newest local copy of the file.
- `mode` – current camera operating mode resolved via the SDK. Examples:
  - `record/still/m` → still capture in manual mode.
  - `record/still/auto_plus` → still capture in Auto+ mode.
  - `record/movie/cine_ei/sq` → movie clip shot in Cine EI with S&Q enabled.
  - `playback` → events raised while browsing files on-body.
- `operation` – high-level action SonShell observed.
  - `new` – a freshly captured file copied to disk.
  - `sync` – a file mirrored during a manual/auto sync.
  - `rating` – the camera changed the star rating of a file (works wherever the SDK reports the update).
- `new` / `old` – optional values tied to the operation. For `rating` hooks SonShell now sends the current star count first, followed by the previous value. For `new`/`sync` only the `new` value is populated with the original camera path.

The hook is executed asynchronously, so long-running work should be handled internally or by delegating to background jobs.

## Scripts

The `scripts/` directory contains helper utilities that SonShell can trigger through the `--cmd` hook or that you can run manually:

- `scripts/broadcast.sh` – YAML-driven dispatcher that maps incoming hook arguments to one or more handler commands. Flags: `-v|--verbose` enables logging, `-c|--config PATH` points at an alternative YAML file, and `--help` prints usage. Pass `--` before the event payload (e.g. `broadcast.sh -v -- <path> playback rating 5 4`). Requires `python3` with `pyyaml` installed.
- `scripts/debug.sh` – Diagnostic helper that shows the received arguments in a dialog (prefers `kdialog`, `zenity`, `dialog`, or `whiptail`, falling back to `echo`). Accepts any arguments; no flags.
- `scripts/find_adb.sh` – Scans the network for Android devices listening for wireless ADB and optionally connects to the first match. Options: `-v` for verbose logs, `-s START:END` to set the port range, and `-m MIN_SCORE` to raise or lower the neighbour scoring threshold.
- `scripts/to_android.sh` – Interacts with a paired Android device (default backend: KDE Connect). Actions: `send-file` (default) copies one or more image/video files, while `notify` pushes a notification to the handset. Options: `-v` verbose mode, `-m MIN_SCORE` for neighbour selection, `-b|--backend NAME` to choose an alternate transfer backend, `-a|--action ACTION` to pick `send-file`/`notify`, `--message TEXT` for the notification body (falls back to positional text), and `-h|--help` for usage info.
- `scripts/show.sh` – Lightweight wrapper around `xdg-open` that simply opens the supplied file path (`show.sh <path>`).
- `scripts/show_single.sh` – Opens an image in the desktop’s default viewer while ensuring only one viewer window launched by the script stays open. Usage: `show_single.sh <image-file>`.
- `scripts/gmic.sh` – Applies a configurable GMIC grade (default: `tensiongreen_1`) to the provided photo, writes the processed copy next to the original, and displays it via `show_single.sh`. Usage: `gmic.sh [-v] [--no-show] [--preset <name>] [--strength <0-1|0-100>] [--pre-resize <WxH>] [--post-quality <n|none>] [--post-strip <on|off>] [--post-interlace <mode|none>] [...] <photo>`; consult the script for all adjustment flags. `-v` enables colored debug logging; `--no-show` skips opening viewer windows and prints the graded file path so the script can be chained in automation. Requires GMIC/ImageMagick.

---

## Shell Command Reference

| Command | Variants / Subcommands | What it does | Shortcut |
| --- | --- | --- | --- |
| `help`, `?` | – | Print the built-in overview of available commands. | – |
| `status` | – | Snapshot the body/lens info plus exposure, focus, and movie settings (`StatusSnapshot`). | – |
| `shoot`, `trigger` | – | Full-press the shutter (locks S1, fires, releases). | `F1` (mapped in the REPL) |
| `focus` | – | Half-press S1 long enough to autofocus, then release. | – |
| `sync` | `sync`, `sync <N>`, `sync all`, `sync on`, `sync off`, `sync stop` | `sync`/`sync <N>` downloads the newest `N` items per slot (skips existing files). `sync all` mirrors every item, preserving Sony’s DCIM/day folder layout. `sync on/off` toggles automatic downloads triggered by new captures. `sync stop` cancels an active sync after the current file finishes (sends `CancelContentsTransfer` when the body supports it). | – |
| `exposure` | `exposure show`, `mode <value>`, `iso <value>`, `aperture <f-number>`, `shutter <value>`, `comp <value>` (aliases: `sensitivity`, `f`, `fnumber`, `speed`, `compensation`, `ev`) | Inspect or change exposure parameters. Values accept friendly forms like `manual`, `auto`, `f/2.8`, `1/125`, `0.3`, or `1/3`. SonShell surfaces hints when the camera mode dial must change. | – |
| `monitor` | `monitor start`, `monitor stop` | Start/stop the OpenCV live-view window. Close it with `monitor stop`. | – |
| `record` | `record start`, `record stop` | Toggle movie recording (simulates the camera’s red button). Confirms state when possible. | – |
| `power` | `power off` | Request a remote power-down. Enable “Remote Power OFF/ON” plus “Network Standby” on the camera for best results. | – |
| `quit`, `exit` | – | Leave SonShell. Also triggered by `Ctrl+D`. | `Ctrl+D` |

Automatic downloads queue in worker threads. Newly captured files are renamed to avoid clashes (e.g. `DSC01234.JPG`, `DSC01234_1.JPG`, …) unless you run a manual `sync`, in which case the original names and folder layout are preserved.

---

## Keyboard Shortcuts
- `F1` inside the REPL: triggers `shoot` (full shutter press).
- `Ctrl+C`: cancel the current input line and repaint the prompt immediately.
- `Ctrl+D`: exit the shell (same as `quit`).

---

## Features
- Auto-connect via enumeration or direct IP, with fingerprint caching under `~/.cache/sonshell/` and optional username/password for Access Auth bodies.
- Automatic download of new captures with unique local filenames plus manual `sync` flows (`latest N` or full mirror).
- Unified hook callbacks (`--cmd`) fired on every file event (new captures, sync mirrors, edits like rating changes) with rich context including capture mode (`record/still/m`, `record/movie/cine_ei/sq`, …).
- Exposure control commands that wrap Sony’s SDK properties, including helpful mode hints when the body rejects a setting.
- Live-view streaming implemented with the SDK monitor APIs and bundled OpenCV 4.8 binaries.
- Robust REPL built on libedit: asynchronous logging, history persisted to `~/.cache/sonshell/history`, and key bindings for shutter control.
- Keepalive loop (`--keepalive`) that retries connections without manual intervention.
- Clean shutdown handling: SIGINT/SIGTERM set a global stop flag, downloads wind down gracefully, and the SDK is released once background threads exit.

---

## Supported cameras

SonShell recognises every entry in Sony’s `CrCameraDeviceModelList` (SDK v2.00.00). Common aliases like `a7r5`, `fx3`, `zve1`, and `a6000` resolve automatically when used with `--model` for direct-IP sessions. When no model is supplied SonShell simply connects to the first enumerated camera.

- ILCE-7RM4 (`a7r4`, `a7rm4`)
- ILCE-9M2 (`a9m2`, `a92`)
- ILCE-7C (`a7c`)
- ILCE-7SM3 (`a7s3`)
- ILCE-1 (`a1`)
- ILCE-7RM4A (`a7r4a`)
- DSC-RX0M2 (`rx0m2`)
- ILCE-7M4 (`a7m4`, `a74`)
- ILME-FX3 (`fx3`)
- ILME-FX30 (`fx30`)
- ILME-FX6 (`fx6`)
- ILCE-7RM5 (`a7r5`)
- ZV-E1 (`zve1`)
- ILCE-6700 (`a6700`)
- ILCE-7CM2 (`a7c2`)
- ILCE-7CR (`a7cr`)
- ILX-LR1 (`lr1`)
- MPC-2610 (`mpc2610`)
- ILCE-9M3 (`a9m3`, `a93`)
- ZV-E10M2 (`zve10m2`)
- PXW-Z200 (`pxwz200`)
- HXR-NX800 (`hxrnx800`)
- ILCE-1M2 (`a1m2`, `a12`)
- ILME-FX3A (`fx3a`)
- BRC-AM7 (`brcam7`)
- ILME-FR7 (`fr7`)
- ILME-FX2 (`fx2`)
- ILCE-6000 (`a6000` – legacy body without an explicit SDK enum)

---

## How It’s Built
- Single translation unit (`src/main.cpp`) stitches together the SDK callback interface, the REPL, and async transfer logic.
- `QuietCallback` implements `SDK::IDeviceCallback`, dispatching transfers, aggregating progress, and feeding a log queue so the shell stays responsive.
- A background input thread owns libedit; download work happens in detached worker threads; live view runs in its own thread guarded by `g_monitor_mtx`.
- Generated helper headers – `prop_names_generated.h` and `error_names_generated.h` – are produced by the Python scripts in `tools/` using Sony’s official headers so logs can spell out property/error names.
- CMake links directly against `libCr_Core.so`, `libCr_PTP_IP.so`, and Sony’s OpenCV libs, then copies those `.so` files into the build output so `./build/sonshell` runs without extra `LD_LIBRARY_PATH` tweaking.
- Persistent state (fingerprint, REPL history) lives under `~/.cache/sonshell/` and is recreated on demand.

---

## Tested cameras

- Sony α6700 (body firmware v2.00). Other models listed above share the same SDK support but have not been explicitly tested yet.

## Tested systems

- Ubuntu 24.04.3 LTS x86_64
- Ubuntu 24.04.3 LTS aarch64

---

## License
See [LICENSE](./LICENSE) for licensing details.

---

## Links
- Sony Camera Remote SDK: https://support.d-imaging.sony.co.jp/app/sdk/en/index.html

---
