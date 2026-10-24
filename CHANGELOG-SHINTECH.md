# ShinTech fork changelog

The 16BIT groovebox fork of Felucca (OPN2, PSG and PCM8 engines). Based on upstream Felucca v1.6.1
(`63f88ec`). Upstream's own release notes are in its wiki.

## Unreleased

### Phase 0: build, baseline, decisions
- Forked hugelton/Felucca and hugelton/Felucca-WebApp to ShinobiFPV; upstream remotes are fetch-only.
- Added BUILDING-SHINTECH.md: the Windows 11 / WSL2 build, verified. Stock v1.6.1 builds (felucca.fwsc 610,090 B),
  all host tests pass, and the browser emulator builds and passes its own test.
- Decisions: keep 4 tracks (16BIT layout T1 OPN2, T2 OPN2, T3 PSG, T4 PCM8). One emulated chip driven by register
  writes, each voice owning a chip channel. Chip voices count against their chip's channels, not the global
  8-voice budget. Locks, automation and MIDI LEARN cover the 8 macro values, as with FM6.

### Phase 1: core selection and benchmark
- `firmware/src/chips/opn2.c`: a C port of ymfm's YM2612 path (BSD-3-Clause). It is bit-exact against ymfm itself
  (`tests/opn2_ymfm_test.cpp`: 8 seeds x 400,000 samples of random register traffic on both ports).
- `firmware/src/chips/ym3438.{c,h}`: Nuked-OPN2 1.0.12, vendored unmodified (LGPL-2.1-or-later). It's only built
  into the benchmark firmware. `chips/shim/string.h` supplies its one libc header on the target.
- `FELUCCA_CHIPBENCH=1` builds the device benchmark (`firmware/src/chipbench.c`, the `bench` console command). It
  is off in every other build, and the default build is unchanged (all host tests pass).
- Host benchmark, 6 channels with SSG-EG and LFO: Nuked 1,022 ns per native sample, the ymfm port 370 ns (2.8x).
  With the DAC on: 1,134 vs 340 ns (3.3x).
- `tests/run_shintech.sh`: the fork's own host tests.
- The ymfm port is faster and still bit-exact: register values cached at prepare, the PM phase step recomputed only
  when the LFO value moves, a 1024-entry sine table, silent operators skipped, and clock + output fused per channel
  (`opn2_render`). Host: 370 -> 265 ns per native sample. A channel ymfm doesn't hear (channel 6 under the DAC) must
  not be computed, because its output updates its feedback memory; the exactness test caught that.
- Benchmark v2/v3: duty in 32nds of each block, the script one write a block, `bench c1` (the second core),
  `bench load 1` (typical patches). Flashed with `tools/fm1_install.py` from a Windows venv.

#### Device results (FM-1, v1.6.1 base, idle 13 % CPU with the HALL reverb; % of one core at full time)

| Core | Load | DAC off | DAC on |
| --- | --- | --- | --- |
| Nuked-OPN2 1.0.12 | worst case (6 ch, SSG-EG + AM + PM everywhere) | 694 % | 697 % |
| ymfm port, first | worst case | 124.8 % | 119.2 % |
| ymfm port, optimized | worst case | 96.9 % | 92.4 % |
| ymfm port, optimized | typical (no SSG-EG/AM, vibrato on 2 ch) | 95.6 % | |
| ymfm port, optimized, on core 2 | worst case | 91.6 % | |
| ymfm port, optimized, on core 2 | typical | 90.9 % | |

- FX sends fed with the chip: +1 % total (the chorus, delay and reverb buses always run).
- Core 2 on this unit: stable for 2+ minutes (no bad job reads, no stalls, no late audio, wait max 542 us). Felucca's
  dual-core split is synchronous (core 1 waits for core 2 each block), so the chip on core 2 still counts against the
  block deadline.
- Neither core leaves 25 % headroom for 6 FM channels: decision pending (see the Phase 1 report).

