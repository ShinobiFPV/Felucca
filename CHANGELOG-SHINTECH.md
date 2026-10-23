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
