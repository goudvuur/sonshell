# SonyShell - an effort to "ssh into my Sony DSLR"

# Sony A6700 Remote Downloader

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
./sony-remote --dir /photos [options]
```

### Options
- `--dir <path>` : Directory to save files (required in most real setups).
- `--ip <addr>` : Connect directly by IPv4 (e.g. `192.168.10.184`).
- `--mac <hex:mac>` : Optional MAC (e.g. `10:32:2c:2a:1a:6d`) for direct IP.
- `--cmd <path>` : Executable/script to run after each download, invoked as
  `cmd /photos/DSC01234.JPG`
- `--keepalive <ms>` : Retry interval when offline or after disconnect.
- `-v`, `--verbose` : Verbose property-change logging.

### Examples
Enumerate + keep retrying every 2s, run a hook after each file:
```bash
./sony-remote --dir /photos --keepalive 2000 --cmd /usr/local/bin/ingest-photo
```

Direct IP connect, verbose logs, retry every 3s:
```bash
./sony-remote --ip 192.168.10.184 --mac 10:32:2c:2a:1a:6d --dir /photos -v --keepalive 3000
```

---

## Build
Requires Linux, g++, and the Sony Camera Remote SDK.

```bash
g++ -std=c++17 sony-a6700-remote-cleaned.cpp \
    -I/path/to/CrSDK/include \
    -L/path/to/CrSDK/lib -lCameraRemoteSDK \
    -lpthread -o sony-remote
```

---

## How It Works (short version)
1. **Connect** to the camera (via IP or enumeration).
   Stores/reuses SDK **fingerprint** under `~/.cache/sonyshell/`.
2. **Wait for notifications**: when the camera signals new contents,
   spawn a download thread.
3. **Download** newest files to `--dir`.
   Safe naming ensures no overwrite (`file_1.jpg`, etc.).
4. **Hook**: if `--cmd` is set, fork/exec the script with the saved path.
5. **Reconnect** on errors/disconnects if `--keepalive` is set.

---

## Developer Notes
- Core behavior is driven by `QuietCallback` (an `IDeviceCallback` impl).
- Download workers use threads + condition variables to sync progress.
- Logging is plain `std::cout`/`std::cerr` with `std::endl` flushing.
- Minimal globals, coordinated by atomics for stop/reconnect flags.
- See [DOCS](./DOCS) for a deep dive into the internals.

---

## Personal Notes
- Built on/for Ubuntu 24.04
- It uses Sony's official Camera Remote SDK. You can search it or download it here: https://support.d-imaging.sony.co.jp/app/sdk/en/index.html
- I leaned heavily on ChatGPT while creating this, so please don't mind the mess! ;)

---

## Links
- Sony Camera Remote SDK: https://support.d-imaging.sony.co.jp/app/sdk/en/index.html
- See `LICENSE` for licensing details.

