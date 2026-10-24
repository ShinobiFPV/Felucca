# Building the ShinTech fork on Windows 11

> Status: **verified 2026-10-23** on Windows 11 Pro (26200) with WSL2 Ubuntu 24.04: stock v1.6.1 builds
> (`felucca.fwsc` 610,090 B), `tests/run_tests.sh` passes, and the browser emulator builds and passes
> `web/emu/emu_test.mjs`.

Upstream's [BUILDING.md](BUILDING.md) targets macOS (Docker) or native Linux x86-64. The FM-1's CPU is a JieLi
AC79 (pi32v2, not ARM), and JieLi's clang 4.0.1 toolchain ships for Linux x86-64 only, so on Windows the build runs
inside **WSL2 (Ubuntu)**. There, `tools/build.py` runs the toolchain natively and Docker is not needed.

The source tree stays on the Windows drive: `D:\Projects\ShinTech\Shinthesizer\Felucca` is
`/mnt/d/Projects/ShinTech/Shinthesizer/Felucca` in WSL. The firmware builds there in about 17 s. The full test suite
is slow on `/mnt/d` (it writes thousands of small files), so expect well over 10 minutes.

## 1. WSL2 (admin PowerShell, once)

```powershell
wsl --install -d Ubuntu-24.04
```

Reboot, then open "Ubuntu 24.04" from the Start menu and **finish its first-run setup** (the Linux user). Until that
is done, every `wsl` command hangs with no output.

## 2. Packages

```sh
sudo apt update
sudo apt install -y build-essential git curl xz-utils python3 python3-pil python3-fonttools python3-numpy \
                    python3-venv nodejs npm
```

- `build-essential`: the host `cc` for `tests/run_tests.sh` and `web/emu/native_check.c`.
- Pillow and fontTools: the UI font and icons are rasterised at build time.
- numpy: the sample tools.
- Node.js 18: the web and emulator tests.

## 3. Toolchains (in `/opt`, owned by the Linux user)

```sh
sudo mkdir -p /opt/jieli /opt/fw-AC79_AIoT_SDK && sudo chown -R "$USER" /opt/jieli /opt/fw-AC79_AIoT_SDK
cd /mnt/d/Projects/ShinTech/Shinthesizer/Felucca
tools/get_toolchain.sh /opt/jieli          # -> /opt/jieli/toolchain (jieli-linux-toolchains-20260730.1)
```

**The AC79 SDK:** the package uses only three of its files. Cloning the whole SDK from gitee failed here (the TLS
connection dropped mid-clone), so fetch just those three:

```sh
B=https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK/raw/AC79NN_SDK_V1.2.1_2023-12-13/cpu/wl82/tools
D=/opt/fw-AC79_AIoT_SDK/cpu/wl82/tools
mkdir -p $D/cfg
for f in uboot.boot cfg_tool.bin cfg/eq_cfg_hw.bin; do curl -fsSL --retry 5 -o "$D/$f" "$B/$f"; done
```

`tools/build.py` checks their SHA-256 against the tested SDK (`4e3b4c22…`, `27657995…`, `41167491…`).

**Emscripten** (for the browser emulator):

```sh
sudo git clone https://github.com/emscripten-core/emsdk.git /opt/emsdk && sudo chown -R "$USER" /opt/emsdk
/opt/emsdk/emsdk install latest && /opt/emsdk/emsdk activate latest     # emcc 6.0.13 here
```

Add this to `~/.bashrc`:

```sh
export JIELI_TOOLCHAIN=/opt/jieli/toolchain AC79_SDK=/opt/fw-AC79_AIoT_SDK
. /opt/emsdk/emsdk_env.sh >/dev/null 2>&1
```

## 4. Source

Already cloned on `D:` with the submodule (`git submodule update --init`). From scratch:

```sh
git clone --recurse-submodules git@github.com:ShinobiFPV/Felucca.git
```

**Line endings:** Git for Windows defaults to `core.autocrlf=true`, which checks the shell scripts out with CRLF,
and `sh` in WSL then fails on them. These clones (Felucca, its `firmware/engines` submodule, and Felucca-WebApp) are
set to `git config core.autocrlf false`, so they keep upstream's LF. Do the same in any fresh clone made from
Windows, or clone from inside WSL.

Remotes: `origin` = the ShinobiFPV fork; `upstream` = hugelton/Felucca, **fetch only** (its push URL is set to
`DISABLED`, so nothing can be pushed upstream by accident).

## 5. Build, test, emulate

```sh
./build.sh                      # build/felucca.bin, build/loader/ota.bin, build/felucca.fwsc
tests/run_tests.sh              # after ./build.sh
web/emu/build.sh                # after ./build.sh (it needs build/gen)
node web/emu/emu_test.mjs build/emu/felucca.wasm
python3 -m http.server -d build/emu 8790
```

Open <http://localhost:8790/> in Chrome or Edge on Windows (WSL2 forwards localhost).

The stock v1.6.1 baseline:

| | Used | Of |
| --- | --- | --- |
| Code image (XIP) | 511,132 B | 581,564 B (~70 KB free) |
| RAM `.data`+`.bss` | 60,984 B | 73,728 B |
| Pool | 328,896 B | 368,640 B (~39 KB free) |

The emulator's heavy-song benchmark renders 1 s in 25 ms on this PC: a host figure, not the device's.

## 6. Install on the FM-1 (from Windows)

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

## 7. Flashing from the command line, and the device benchmark

Felucca's CLI installer runs from Windows Python (the FM-1's USB MIDI ports are "Felucca 0" / "Felucca 1"):

```powershell
python -m venv $env:TEMP\fm1venv
& $env:TEMP\fm1venv\Scripts\pip install mido python-rtmidi
& $env:TEMP\fm1venv\Scripts\python tools\fm1_install.py --info
& $env:TEMP\fm1venv\Scripts\python tools\fm1_install.py build\shintech\felucca-chipbench.fwsc --yes
```

The OPN2 core benchmark (Phase 1) is a build of its own (`FELUCCA_CHIPBENCH=1 ./build.sh`). It is driven over the USB
serial console (COM4 here) by `tools/shintech/*.ps1` (`fm1con.ps1` sends commands; `splitm.ps1` measures the two-core
split; the `bench` command is described in `firmware/src/chipbench.c`). After a flash, wait 35 s before starting it:
two crashes in the first 30 s of a boot send the FM-1 to UBOOT (USB mass storage, 4C4A:8057), and a power cycle
brings it back.

