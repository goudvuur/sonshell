# SonShell - an effort to "ssh into my Sony camera"

A Linux-only helper built on Sony’s official **Camera Remote SDK**.
It connects to a Sony A6700 camera over Wi-Fi/Ethernet, listens for new photos, downloads them automatically, and can optionally run a script on each downloaded file.

---

## Features
- Auto-connect via **enumeration** or **direct IP/MAC**.
- Watches for new capture events and fetches the newest files.
- Saves into a chosen directory with **unique filenames**.
- **Post-download hook**: run any executable/script with the saved file path as argument.
- **Keepalive mode**: auto-retry on startup failure or after disconnects.
- Linux-only

---

## Demo

https://github.com/user-attachments/assets/6146ff3b-d51c-412b-8684-bdde5c418d4d

---

## Usage
```bash
./sonshell --dir /photos [options]
```

## Options
- `--dir <path>` : Directory to save files (required in most real setups).
- `--ip <addr>` : Connect directly by IPv4 (e.g. `192.168.1.1`).
- `--mac <hex:mac>` : Optional MAC (e.g. `10:20:30:40:50:60`) for direct IP.
- `--cmd <path>` : Executable/script to run after each download, invoked as `cmd /photos/DSC01234.JPG`
- `--keepalive <ms>` : Retry interval when offline or after disconnect.
- `--verbose` : Verbose property-change logging.
- `--user <name>` : Supply username for access-auth cameras.
- `--pass <pass>` : Supply password for access-auth cameras.

---

## Interactive Commands

Once connected, you enter the interactive **SonShell** prompt:

- `shoot` : Trigger shutter release.
- `focus` : Trigger autofocus (half-press behavior).
- `quit` or `exit` : Leave the shell and stop the program.
- `sync <N>` : Download the last N files per slot (skips existing files).
- `sync all` : Download *all* files, preserving camera folder structure.
- `sync stop` : Abort an ongoing sync session (after the current file).
- Ctrl-C / Ctrl-D exit cleanly.


## Examples
Enumerate + keep retrying every 2s, run a hook after each file:
```bash
./sonshell --dir /tmp/photos --verbose --keepalive 3000 --cmd ../scripts/show_single.sh
```

Direct IP connect, verbose logs, retry every 3s:
```bash
./sonshell --ip 192.168.1.1 --mac 10:20:30:40:50:60 --dir /tmp/photos -v --keepalive 3000
```

---

## Build
Requires Linux, g++, and the Sony Camera Remote SDK.

See [INSTALL.md](./INSTALL.md)

---

## How It Works (short version)
1. **Connect** to the camera (via IP or enumeration).
   Stores/reuses SDK **fingerprint** under `~/.cache/sonshell/`.
2. **Wait for notifications**: when the camera signals new contents,
   spawn a download thread.
3. **Download** newest files to `--dir`.
   Safe naming ensures no overwrite (`file_1.jpg`, etc.).
4. **Hook**: if `--cmd` is set, fork/exec the script with the saved path.
5. **Reconnect** on errors/disconnects if `--keepalive` is set.

---

## License
See [LICENSE](./LICENSE) for licensing details.

---

## Links
- Sony Camera Remote SDK: https://support.d-imaging.sony.co.jp/app/sdk/en/index.html

---

## Developer Documentation

### Notes
- Built on/for Ubuntu 24.04
- It uses Sony's official Camera Remote SDK (not included here).
- I leaned heavily on ChatGPT while creating this, so please don't mind the mess! ;)

### Architecture
- Interactive REPL shell using libedit, with a custom getchar (`my_getc`) that integrates log-draining and prompt refresh.
- Separate background threads:
  - **Input thread** (REPL) handles user commands.
  - **Download workers** handle file transfers.
  - **Wake pipe** mechanism used to wake REPL for new logs without clobbering the prompt.

### Sync Implementation
- `sync <N>`: downloads the last N files per slot.
- `sync all`: downloads *all* files from the camera, preserving the DCIM/day-folder structure.
- `sync stop`: aborts an in-progress sync gracefully (after the current file finishes).
- Sync skips files already present locally.

### Logging
- Each file transfer produces a single compact `[PHOTO] filename (bytes, ms)` log line.
- For large files, intermediate progress updates are logged.
- Verbosity can be toggled with `--verbose`.

### Shutdown Handling
- Ctrl-C and Ctrl-D are fixed to exit cleanly on the first press, without requiring repeats.
- Signal handler sets `g_stop` and nudges the wake pipe so the REPL loop exits immediately.
- Input thread is joined before disconnect, preventing stray prompt redraws.

### Authentication
- New `--user` and `--pass` options allow supplying credentials if the camera has **Access Auth** enabled.
- Fingerprint caching (`~/.cache/sonshell/fp_enumerated.bin`) stays empty if Access Auth is disabled — this is expected.

### Recent Changes
- Added sync commands (`sync <N>`, `sync all`, `sync stop`).
- Folder mirroring when syncing.
- Improved logging format (single `[PHOTO]` line per file).
- Fixed Ctrl-C / Ctrl-D handling.
- Added `--user` / `--pass` options for authentication.
