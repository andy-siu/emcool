# Developing on a Mac

How to set up a Mac to build and flash the firmware, run the checks on the laptop, and serve the dashboard locally. The three parts don't depend on each other: the firmware needs ESP-IDF, and the checks and the dashboard don't.

## 1. Homebrew packages

```bash
brew install cmake ninja dfu-util python3 ccache node openssl@3
```

The checks need Node 22.13 or newer, for its built-in SQLite. Check with `node -v`.

## 2. ESP-IDF v6.1

The firmware is built with ESP-IDF v6.1. The `Makefile` and the README expect it at `~/esp/esp-idf`:

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32
```

Set up the environment in every terminal you build from:

```bash
. ~/esp/esp-idf/export.sh
```

To make that shorter, add an alias to `~/.zshrc`:

```bash
alias get_idf='. ~/esp/esp-idf/export.sh'
```

Don't source `export.sh` in every new shell, because it's slow.

## 3. Build, flash and monitor

From the repo, in a terminal where `export.sh` has been sourced:

```bash
make            # build (or: idf.py build)
make flash      # build if needed, then flash
make monitor    # serial console; Ctrl-] quits
```

- **Driver:** macOS has a built-in driver for the board's CP2104 USB-C bridge, so you don't need the Silicon Labs driver.
- **Port:** the board shows up as `/dev/cu.usbserial-*`, and `idf.py` usually finds it on its own. To pick it yourself, list the ports with `ls /dev/cu.*`, then pass it:

  ```bash
  make flash PORT=/dev/cu.usbserial-0001
  make monitor PORT=/dev/cu.usbserial-0001
  ```

- **No port showing up:** try a different cable. Many USB-C cables only charge and carry no data.
- **Build options:** after changing `sdkconfig.defaults`, delete `sdkconfig` so the build regenerates it.

## 4. Laptop checks

```bash
tools/check.sh
```

- **No ESP-IDF needed:** the checks use the Homebrew CMake, Ninja, OpenSSL 3 and Node.
- **OpenSSL:** on Apple Silicon, `host/CMakeLists.txt` finds OpenSSL 3 at `/opt/homebrew/opt/openssl@3` on its own. On an Intel Mac, point CMake at it with `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`.
- **The reference implementation:** the checks need a checkout of the reference implementation next to this repo (`../cool-eva`), or its path in `COOL_EVA_DIR`.
- **FAT image:** the full-card check mounts a real FAT image with `hdiutil`, so that step only runs on macOS.

## 5. Dashboard

```bash
python3 -m http.server -d dashboard
```

Then open `http://localhost:8000/?transport=ble` in Chrome. Safari doesn't support Web Bluetooth. [`dashboard/README.md`](../dashboard/README.md) has more about hosting it.
