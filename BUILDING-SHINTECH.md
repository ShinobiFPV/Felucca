# Building the ShinTech fork on Windows 11

> Status: **draft, not yet verified end to end.** Steps 1–2 need an admin shell and a reboot.
> Everything after them will be checked off here once it has actually run.

Upstream's [BUILDING.md](BUILDING.md) targets macOS (Docker) or native Linux x86-64. The FM-1's CPU is a JieLi
AC79 (pi32v2, not ARM), and JieLi's clang 4.0.1 toolchain ships for Linux x86-64 only, so on Windows the build runs
inside **WSL2 (Ubuntu)**. There, `tools/build.py` runs the toolchain natively and Docker is not needed.

The source tree stays on the Windows drive (`D:\Projects\ShinTech\Shinthesizer\Felucca`, i.e.
`/mnt/d/Projects/ShinTech/Shinthesizer/Felucca` in WSL). Builds on `/mnt/d` are slower than on WSL's own disk, but
they work, and the tree stays visible to Windows tools.

## 1. Install WSL2 (admin PowerShell, once)

```powershell
wsl --install -d Ubuntu-24.04
```

Reboot when it asks, then open "Ubuntu 24.04" from the Start menu and create the Linux user.

## 2. Packages (in Ubuntu)

```sh
sudo apt update
sudo apt install -y build-essential git curl xz-utils python3 python3-pil python3-fonttools python3-numpy \
                    python3-venv nodejs npm
```

- `build-essential`: the host `cc` that `tests/run_tests.sh` and `web/emu/native_check.c` use.
- Pillow and fontTools: the build rasterises the UI font and icons.
- numpy: the sample tools (`tools/gen_samples.py`, `tools/sampleio.py`).
- Node.js: the web tests (optional).

The command-line installer (optional; the web installer works from Windows Chrome) also needs
`pip install mido python-rtmidi` in a venv.

## 3. JieLi toolchain

```sh
cd /mnt/d/Projects/ShinTech/Shinthesizer/Felucca
tools/get_toolchain.sh          # -> ~/.jieli/toolchain
```

## 4. JieLi AC79 SDK (three of its files go into every package)

```sh
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
```

`tools/build.py` checks their SHA-256 against the tested SDK version.

## 5. Source

Already cloned on `D:` with the submodule (`git submodule update --init`). From scratch:

```sh
git clone --recurse-submodules git@github.com:ShinobiFPV/Felucca.git
```

Remotes: `origin` = the ShinobiFPV fork; `upstream` = hugelton/Felucca, **fetch only** (its push URL is set to
`DISABLED`, so nothing can be pushed upstream by accident).

## 6. Build

```sh
./build.sh                      # build/felucca.bin, build/loader/ota.bin, build/felucca.fwsc
```

## 7. Tests

```sh
tests/run_tests.sh              # after ./build.sh; AC79_SDK as for the build
```

## 8. Browser emulator

```sh
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
. ~/emsdk/emsdk_env.sh
web/emu/build.sh                # after ./build.sh (needs build/gen)
python3 -m http.server -d build/emu 8790
```

Open <http://localhost:8790/> in Chrome or Edge on Windows. WSL2 forwards localhost.

## 9. Install on the FM-1 (from Windows)

The FM-1's USB MIDI goes to Windows, not WSL, so install from Chrome or Edge on Windows:

```sh
python3 web/make_site.py build/felucca.fwsc dev ~/felucca-site
cd ~/felucca-site && python3 -m http.server 8000
```

Then open <http://localhost:8000/webapp/installer/> in Windows Chrome. Web MIDI needs a secure context, and
`localhost` counts as one.

**Before installing anything:** back up your projects and user presets with the stock Felucca build (the web
editor's backup), and keep the official firmware file at hand: the installer's *Return to official V15* takes you
back. Recovery from a failed install needs [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
