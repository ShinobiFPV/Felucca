# ShinTech CHORD changelog

ShinTech's fork of Felucca with a chord-synth mode (CHORD: chord sets on the keys, styles with variations, a chord
pattern sequencer) and a six-voice-style poly engine (POLY6), for personal use. Branch `chord` of
`ShinobiFPV/Felucca`, based on upstream Felucca `main` (v1.8.1 + the 1.8.2 flash fix, `a4f007c`). Upstream's own
release notes are in its wiki. Projects that use the new features will not open on upstream Felucca.

## Unreleased

### Phase 0: build, baseline, gap analysis

- Branch `chord` from `upstream/main` in the existing ShinobiFPV forks (Felucca and Felucca-WebApp), checked out as
  worktrees `Felucca-Chord` and `Felucca-WebApp-Chord`, apart from Genesynth (`shintech`). Upstream remotes stay
  fetch-only.
- BUILDING-SHINTECH.md: the Windows 11 / WSL2 build, verified. Stock 1.8.1 builds (`felucca.fwsc` 610,286 B), all host
  tests pass.
- The browser emulator (`web/emu/`), removed upstream in `d483929`, is restored from `d483929^` unchanged, with its
  licence (DotGothic16, OFL), its credits (X0X; the emulator contributions of spinkham and ugai) and its stage in
  `tests/run_tests.sh`. It builds against 1.8.1 and passes its own test.
