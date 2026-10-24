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
- Neither core left 25 % headroom for 6 FM channels at this point. Decision: optimize the ymfm port further (still
  bit-exact) and use the second core too.

#### Phase 1, round 2: the port on two cores
- `bench cal` measured the CPU (~220-240 MHz, about one simple instruction a cycle). A dependent load costs 4-6
  cycles, the pool is ~25 % slower than `.bss`, and RAM code is no faster than warm XIP.
- The per-sample calls and their 12-register saves cost ~19 % of a core with no note playing. The port now renders
  in blocks (`opn2_render_n`, `opn2_generate_n`), checks `prepare` inline, and inlines the envelope / SSG-EG / LFO.
- `opn2_t` now holds the operators first and `reg[]` last (short load offsets: -8 %), each channel's operators
  together, a 64-byte op struct, and a precomputed `idle` flag.
- Channel-major blocks (`opn2_block_begin` + `opn2_block_channels`): a block's global steps first, then each channel
  over the block. So channels can render apart: `opn2_begin_n` / `opn2_channels_n` / `opn2_mix_n` split the six
  channels across the two cores (4..6 on core 2, started before the parts; 1..3 on core 1 after them).
- Every step stays bit-exact against ymfm. The test now also renders random block sizes and the two-core split.
- `.fast_text` (app.ld, build.py, main.c): hot DSP code copied to RAM at boot; it may call into XIP (`OPN2_FAR`).
  `fm1_guard.h`'s PC limit now ends at `_ft_end`. Its first version missed that, and the FM-1 crashed; a second
  crash within 30 s of boot (the console script restarting the benchmark) sent it to UBOOT, and a power cycle brought
  it back. The console scripts (`tools/shintech/`) now stop at the first reboot or lost port.
- Tried and dropped: per-operator block loops (slower here: the state stays in memory, the arrays add traffic).

| ymfm port, % of one core at full time | worst case | typical |
| --- | --- | --- |
| core 1 alone (round 1 best) | 93.6 % | 86.7 % |
| core 1 alone (channel-major blocks) | 84.1 % | 80.2 % |
| **split, 3 + 3 channels: the whole ISR (idle 13 % included)** | **54 %** | **50 %** |

- Split, worst case at full time: core 1's part 43.6 %, core 2's 35.8 %. A 60 s soak: no late buffer, no core-2
  fault, the longest half 1,997 us of 2,902 (69 %).
- **Choice: the ymfm port (BSD-3-Clause, in C), split across both cores. Nuked-OPN2 (694 %) is out.** Without the
  second core (MENU > SYSTEM > 2ND CORE OFF, or a unit that cannot run it) the port alone costs ~84 % worst case; the
  engine must then cap the sounding FM channels (Phase 2).

