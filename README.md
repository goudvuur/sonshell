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
- Cleaned, Linux-only code (no Windows ifdefs, simpler logging).

---

## Usage
```bash
./sonshell --dir /photos [options]
```

### Options
- `--dir <path>` : Directory to save files (required in most real setups).
- `--ip <addr>` : Connect directly by IPv4 (e.g. `192.168.1.1`).
- `--mac <hex:mac>` : Optional MAC (e.g. `10:20:30:40:50:60`) for direct IP.
- `--cmd <path>` : Executable/script to run after each download, invoked as
  `cmd /photos/DSC01234.JPG`
- `--keepalive <ms>` : Retry interval when offline or after disconnect.
- `--boot-pull <N>` : On startup, download the latest **N** files from each slot.
   * During boot, files already present locally are **skipped**.
   * After boot, new incoming files always use **numeric suffixes** if needed.
   * Boot downloads run asynchronously and can be cancelled with Ctrl-C.
- `-v`, `--verbose` : Verbose property-change logging.

### Examples
Enumerate + keep retrying every 2s, run a hook after each file:
```bash
./sonshell --dir /tmp/photos --verbose --keepalive 3000 --cmd ../scripts/show_single.sh
```

Direct IP connect, verbose logs, retry every 3s:
```bash
./sonshell --ip 192.168.1.1 --mac 10:20:30:40:50:60 --dir /tmp/photos -v --keepalive 3000
```

---

## Interactive Commands

Once connected, you enter the interactive **SonShell** prompt:

- `shoot` : Trigger shutter release.
- `focus` : Trigger autofocus (half-press behavior).
- `quit` or `exit` : Leave the shell and stop the program.

---

## Build
Requires Linux, g++, and the Sony Camera Remote SDK.

See [INSTALL.md](./INSTALL.md)

or (untested)

```bash
g++ -std=c++17 sonshell.cpp \
    -I/path/to/CrSDK/include \
    -L/path/to/CrSDK/lib -lCameraRemoteSDK \
    -lpthread -o sonshell
```

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

## Developer Notes
- Built on/for Ubuntu 24.04
- It uses Sony's official Camera Remote SDK (not included here).
- See [DOCS.md](./DOCS.md) for a deep dive into the internals.
- I leaned heavily on ChatGPT while creating this, so please don't mind the mess! ;)

---

## Links
- Sony Camera Remote SDK: https://support.d-imaging.sony.co.jp/app/sdk/en/index.html
- See [LICENSE](./LICENSE) for licensing details.

