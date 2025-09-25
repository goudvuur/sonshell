# SonShell — Developer Guide

This document explains how the cleaned Linux-only helper works so you can extend it quickly.

## High-level overview

The program connects to a Sony camera via the Camera Remote SDK, monitors for new items on the card, downloads them to a target directory, and (optionally) invokes a post-download hook. It can run in a resilient “keepalive” loop that retries on initial failure and on later disconnects.

**Core responsibilities**

* Initialize & release the Camera Remote SDK.
* Discover a camera (via enumeration) or connect directly by IP/MAC.
* Subscribe to remote transfer notifications.
* Upon new content, fetch the latest contents list, determine the newest files, and download them.
* Emit progress/diagnostic logs.
* Optionally call a user-provided command with the downloaded file path.
* Optionally keep retrying when no camera is available or after disconnects.

## Key components

### 1) Global runtime state

* **g\_downloadThreads**: fire-and-forget worker threads created when new content arrives. Joined on shutdown or reconnect.
* **g\_download\_dir**: destination directory for downloaded files.
* **g\_post\_cmd**: optional executable/script to call per downloaded file.
* **g\_keepalive**: retry interval when connection is down.
* **g\_stop / g\_shutting\_down / g\_reconnect**: atomics coordinating lifetime and reconnect behavior.

### 2) Signal handling

* `SIGINT`/`SIGTERM` set `g_stop` to trigger graceful shutdown.
* `SIGCHLD` is ignored to avoid zombies from the post-download hook.

### 3) Filesystem helpers

* `join_path`, `file_exists`, `basename_from_path`, and `unique_name` keep naming and path logic simple and POSIX-friendly.
* Fingerprint cache stored under `$HOME/.cache/sonshell` to speed up/solidify trust on subsequent connects.

### 4) Network helpers

* `ip_to_uint32` and `parse_mac` convert CLI inputs into the SDK’s formats for direct IP/MAC connections.

### 5) Post-download hook

* `run_post_cmd(path, file)`: forks and `execl`s the provided command with **one argument** (absolute path to saved file). Parent does not wait.

### 6) `QuietCallback` (SDK::IDeviceCallback)

Implements the Camera Remote SDK callback surface:

* **Connection lifecycle**: `OnConnected`, `OnDisconnected`, `OnError`, `OnWarning`. These update condition variables and atomics; `OnDisconnected` sets `g_reconnect` so the main loop will tear down and retry.
* **Property changes**: `OnPropertyChanged`/`OnLvPropertyChanged` log only when values actually change (reduces noise). Property name decoding is done via generated lookup helpers.
* **Remote transfer flow**:

  * `OnNotifyRemoteTransferContentsListChanged` fires when new items are available. It spawns a worker thread that:

    1. Gets the list of captured dates for a slot and picks the latest day.
    2. Fetches the contents list for that day.
    3. Sorts by modification timestamp (newest first) and selects the newest `addSize` items (fallback 1).
    4. For each file in each selected content record, requests a transfer via `GetRemoteTransferContentsDataFile`.
  * `OnNotifyRemoteTransferResult` signals completion (or failure) to the worker via `dl_cv`/`dl_waiting`. On success it logs a `[PHOTO]` line and triggers the post-download hook if configured.

**Threading model**

* The callback may launch download workers. Each worker synchronizes with the callback using a condition variable to wait for transfer completion notifications.
* Main thread only manages connection and retry policy; it does **not** block the callback threads.

## Connection strategy

### Direct IP vs. Enumeration

* If `--ip` is provided, we construct a synthetic `ICrCameraObjectInfo` for an Ethernet connection using the supplied IP (and optional `--mac`).
* Otherwise we call `EnumCameraObjects` and pick the first entry.

### Fingerprint

* On connect, we submit any cached fingerprint; on success we read back the new fingerprint and persist it. This avoids repeated trust prompts in some flows.

### Keepalive loop

* Start SDK → attempt connect → on failure: if `--keepalive ms` is set, sleep **ms** and try again; otherwise exit.
* While connected: loop until `g_stop` (signals) or `g_reconnect` (disconnect callback) becomes true.
* On disconnect: tear down device and enumeration objects, join worker threads, then either exit (no keepalive) or sleep **ms** and retry.

## CLI

```
--dir <path>          Directory to save files (required in most real setups)
--ip <addr>           Connect directly to camera at IPv4 (e.g., 192.168.10.184)
--mac <hex:mac>       Optional MAC (e.g., 10:20:30:40:50:60) for direct IP
--cmd <path>          Executable/script called after each download with 1 arg
--keepalive <millis>  Retry interval when offline or after disconnect
--boot-pull <N>       On startup, download the latest N files from each slot
                      (skip if file already exists locally; post-boot reverts
                      to numeric suffix naming). Runs asynchronously and is
                      cancelable with Ctrl-C.
-v, --verbose         Verbose property-change logging
```

### Examples

* Enumerate and keep trying every 2s; run a hook after each file:

```
./sonshell --dir /photos --keepalive 2000 --cmd /usr/local/bin/ingest-photo
```

* Direct IP connect, verbose, try again every 3s:

```
./sonshell --ip 192.168.1.1 --mac 10:20:30:40:50:60 --dir /photos -v --keepalive 3000
```

## Build

```
g++ -std=c++17 sony-a6700-remote-cleaned.cpp \
    -I/path/to/CrSDK/include \
    -L/path/to/CrSDK/lib -lCameraRemoteSDK \
    -lpthread -o `sonshell`
```

## 

## Interactive Prompt Commands

When SonShell is running, you'll see a `sonshell>` prompt. Supported commands:

- `shoot` — Triggers the shutter (press, brief hold, release).
- `focus` — Performs an autofocus half-press (S1 lock, then release).
- `quit` / `exit` — Cleanly shuts down the session and exits.

(Use Tab for simple completion of known commands. History is preserved per session.)
Error handling & logging

* All SDK errors are logged with hex codes and (where available) decoded names.
* Transfers log success with `[PHOTO] <name> (<size bytes>)`.
* Failed downloads log an error and the worker continues with the next item.

## Extending the tool

* **Multiple file types**: Current logic downloads all files in a content record. Filter by extension if needed.
* **Alternate naming**: Replace `unique_name` or introduce timestamp-based naming.
* **Structured logging**: Swap `std::cout` for JSON logs if another system ingests them.
* **Graceful shutdown**: Add a draining phase to wait for in-flight transfers before reconnecting.
* **Unit-testable pieces**: Extract helpers into separate translation units and add small tests.

## Common pitfalls

* **Permissions**: Ensure `--dir` is writable and your hook is executable.
* **Network**: Direct IP mode assumes camera is reachable and in the correct SDK mode.
* **Fingerprint mismatch**: If connection fails mysteriously, try deleting the cached fingerprint at `~/.cache/sonshell/fp_enumerated.bin` and reconnecting.

