# Building the ShinTech CHORD fork on Windows 11

> Status: **verified 2026-11-11** on Windows 11 Pro (26200) with WSL2 Ubuntu 24.04: stock upstream `main` (v1.8.1 +
> the 1.8.2 flash-recognition fix, `a4f007c`) builds (`felucca.fwsc` 610,286 B in 17 s), `tests/run_tests.sh` passes, and
> the browser emulator (restored, see 5) builds and passes `web/emu/emu_test.mjs`.

Upstream's [BUILDING.md](BUILDING.md) targets macOS (Docker) or native Linux x86-64. The FM-1's CPU is a JieLi
AC79 (pi32v2, not ARM), and JieLi's clang 4.0.1 toolchain ships for Linux x86-64 only, so on Windows the build runs
inside **WSL2 (Ubuntu)**. There, `tools/build.py` runs the toolchain natively and Docker is not needed.

## Where the tree is

The `ShinobiFPV/Felucca` fork holds two ShinTech lines, each on its own branch:

| Branch | What | Working tree |
| --- | --- | --- |
| `shintech` | Genesynth, the 16BIT fork (based on v1.6.1) | `D:\Projects\ShinTech\Shinthesizer\Felucca` |
| `chord` | this fork: CHORD mode and the POLY6 engine (based on upstream `main`, 1.8.1) | `D:\Projects\ShinTech\Shinthesizer\Felucca-Chord` (a `git worktree` of the same clone) |

The web editor is the same: `Felucca-WebApp` (`shintech`) and `Felucca-WebApp-Chord` (`chord`, a worktree).

GitHub allows one fork of a repository per account, so the two lines share the fork and differ by branch.
`chord` was branched from `upstream/main` with `git worktree add -b chord ../Felucca-Chord upstream/main`, then
`git branch --unset-upstream`, so a push goes to `origin` (the fork). In WSL the tree is
`/mnt/d/Projects/ShinTech/Shinthesizer/Felucca-Chord`.

## 1. WSL2 (admin PowerShell, once)

```powershell
wsl --install -d Ubuntu-24.04
```

Reboot, then open "Ubuntu 24.04" from the Start menu and **finish its first-run setup** (the Linux user). Until that
is done, every `wsl` command hangs with no output. (`wsl` may print "Failed to detect nested virtualization
support" on this PC: harmless.)

## 2. Packages

```sh
sudo apt update
sudo apt install -y build-essential git curl xz-utils python3 python3-pil python3-fonttools python3-numpy \
                    python3-venv nodejs npm
```

- `build-essential`: the host `cc` for `tests/run_tests.sh` and `web/emu/native_check.c`.
- Pillow and fontTools: the UI font and icons are rasterised at build time.
- Python 3 also runs the CHORD data generators (Phase 1 on); they need nothing outside the standard library.
- Node.js 18: the web and emulator tests.

## 3. Toolchains (in `/opt`, owned by the Linux user)

```sh
sudo mkdir -p /opt/jieli /opt/fw-AC79_AIoT_SDK && sudo chown -R "$USER" /opt/jieli /opt/fw-AC79_AIoT_SDK
cd /mnt/d/Projects/ShinTech/Shinthesizer/Felucca-Chord
tools/get_toolchain.sh /opt/jieli          # -> /opt/jieli/toolchain
```

**The AC79 SDK:** the package uses only three of its files. Cloning the whole SDK from gitee failed here (the TLS
connection dropped mid-clone), so fetch just those three:

```sh
B=https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK/raw/AC79NN_SDK_V1.2.1_2023-12-13/cpu/wl82/tools
D=/opt/fw-AC79_AIoT_SDK/cpu/wl82/tools
mkdir -p $D/cfg
for f in uboot.boot cfg_tool.bin cfg/eq_cfg_hw.bin; do curl -fsSL --retry 5 -o "$D/$f" "$B/$f"; done
```

`tools/build.py` checks their SHA-256 against the tested SDK.

**Emscripten** (for the browser emulator):

```sh
sudo git clone https://github.com/emscripten-core/emsdk.git /opt/emsdk && sudo chown -R "$USER" /opt/emsdk
/opt/emsdk/emsdk install latest && /opt/emsdk/emsdk activate latest     # emcc 6.0.13 here
```

The environment: add to `~/.bashrc` for interactive shells,

```sh
export JIELI_TOOLCHAIN=/opt/jieli/toolchain AC79_SDK=/opt/fw-AC79_AIoT_SDK
. /opt/emsdk/emsdk_env.sh >/dev/null 2>&1
```

but Ubuntu's `.bashrc` returns early in a non-interactive shell, so **`wsl bash -c '...'` from PowerShell does not
see it**. Put the two lines at the start of the command instead (as every example below does).

## 4. Source

```sh
git clone --recurse-submodules git@github.com:ShinobiFPV/Felucca.git
git -C Felucca checkout chord
```

Upstream 1.8 has **two** submodules: `firmware/engines` (Felucca-Engines, MIT) and `firmware/bsp` (felucca-bsp,
MPL-2.0, the hardware layer that was `firmware/hal/`). In a worktree or a clone without them, run
`git submodule update --init` once; the build stops with "firmware/bsp is empty" otherwise.

**Line endings:** Git for Windows defaults to `core.autocrlf=true`, which checks the shell scripts out with CRLF,
and `sh` in WSL then fails on them. This clone is set to `git config core.autocrlf false` (worktrees share it). Do
the same in any fresh clone made from Windows, or clone from inside WSL.

**The submodules have their own config**, and a worktree gets fresh submodule checkouts, so set it there too
and check them out again; otherwise the Felucca-Engines stage of the tests fails (`run.sh: set: Illegal option -`
and a golden-render diff on every line):

```powershell
foreach ($s in 'firmware/engines','firmware/bsp') { git -C $s config core.autocrlf false; git -C $s rm -rq --cached .; git -C $s reset -q --hard }
```

Remotes: `origin` = the ShinobiFPV fork; `upstream` = hugelton/Felucca, **fetch only** (its push URL is
`DISABLED`, so nothing can be pushed upstream by accident).

## 5. Build, test, emulate

From PowerShell:

```powershell
wsl -d Ubuntu-24.04 bash -c 'export JIELI_TOOLCHAIN=/opt/jieli/toolchain AC79_SDK=/opt/fw-AC79_AIoT_SDK; . /opt/emsdk/emsdk_env.sh >/dev/null 2>&1; cd /mnt/d/Projects/ShinTech/Shinthesizer/Felucca-Chord && ./build.sh'
```

or, inside a WSL shell in the tree:

```sh
./build.sh                      # build/felucca.bin, build/loader/ota.bin, build/felucca.fwsc  (~17 s)
tests/run_tests.sh              # after ./build.sh; slow on /mnt/d (thousands of small files)
sh web/emu/build.sh             # after ./build.sh (it needs build/gen) -> build/emu
node web/emu/emu_test.mjs build/emu/felucca.wasm
python3 -m http.server -d build/emu 8790
```

Open <http://localhost:8790/> in Chrome or Edge on Windows (WSL2 forwards localhost).

**The browser emulator is restored in this fork.** Upstream removed `web/emu/` (and the desktop / libretro ports)
on 2026-11-04 in `d483929` "Scope Felucca to FM-1 only". That commit touched only the ports and the docs, so the
emulator as it was just before (`d483929^`) builds and passes its own tests against today's firmware unchanged. This
fork keeps it (with its DotGothic16 font licence, its X0X credit and its stage in `tests/run_tests.sh`), because
CHORD mode is tested in it before anything is flashed. It is a development tool here as it was upstream.

The stock baseline (upstream `main`, `a4f007c`):

| | Used | Of | Free |
| --- | --- | --- | --- |
| Code image (XIP app slot, `fm1pkg_make.APP_SLOT`) | 517,604 B | 581,564 B | 63,960 B |
| RAM `.data`+`.bss` (build keeps >= 4 KiB spare) | 62,324 B | 73,728 B | 11,404 B (7,308 usable) |
| Pool (build keeps >= 8 KiB spare) | 326,236 B | 368,640 B | 42,404 B (34,212 usable) |

The emulator's heavy-song benchmark renders 1 s in 15.5 ms on this PC (1.5 % of real time): a host figure, not
the device's. The device's own figure is MENU > SYSTEM > INFO > CPU.

## 6. Install on the FM-1 (from Windows)

The FM-1's USB MIDI goes to Windows, not WSL, so install from Chrome or Edge on Windows:

```sh
python3 web/make_site.py build/felucca.fwsc dev ~/felucca-site
cd ~/felucca-site && python3 -m http.server 8000
```

Then open <http://localhost:8000/webapp/installer/> in Windows Chrome. Web MIDI needs a secure context, and
`localhost` counts as one.

Or from Windows Python (the FM-1's USB MIDI ports are "Felucca 0" / "Felucca 1"):

```powershell
python -m venv $env:TEMP\fm1venv
& $env:TEMP\fm1venv\Scripts\pip install mido python-rtmidi
& $env:TEMP\fm1venv\Scripts\python tools\fm1_install.py --info
& $env:TEMP\fm1venv\Scripts\python tools\fm1_install.py build\felucca.fwsc
```

**Before installing anything:** back up your projects and user presets with the stock Felucca build (the web
editor's backup), and keep the official firmware file at hand: the installer's *Return to official V15* takes you
back. Recovery from a failed install needs [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
After a flash, two crashes in the first 30 s of a boot send the FM-1 to UBOOT (USB mass storage, 4C4A:8057); a power
cycle brings it back.
