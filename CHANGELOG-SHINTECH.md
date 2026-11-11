# ShinTech CHORD changelog

ShinTech's fork of Felucca with a chord-synth mode (CHORD: chord sets on the keys, styles with variations, a chord
pattern sequencer) and a six-voice-style poly engine (POLY6), for personal use. Branch `chord` of
`ShinobiFPV/Felucca`, based on upstream Felucca `main` (v1.8.1 + the 1.8.2 flash fix, `a4f007c`). Upstream's own
release notes are in its wiki. Projects that use the new features will not open on upstream Felucca.

## Unreleased

### Phase 1: CHORD mode and chord sets

- **Chord set data.** 100 original chord sets in 12 genres (POP 9, SYNTHWAVE 8, HOUSE 9, TECHNO 8, R&B 8, NEO SOUL 9,
  JAZZ 9, LO-FI 8, CINEMATIC 8, FUNK 8, GOSPEL 8, MINIMAL 8), 12 chords each, written as chord symbols in
  `tools/shintech/chordsets/<genre>.json` ("Fmaj9", "Dm7/G", optionally "open" / "spread" and an inversion "i1").
  `tools/shintech/gen_chords.py` checks them and writes `build/gen/chord_sets.h` at build time: 26 distinct shapes, 3
  bytes a chord, 5,056 B. Edit the JSON and rebuild; no C to touch.
- `firmware/src/chordset.c`: a set's chord (root, shape, slash bass, default voicing and inversion), and a chord's
  name from its notes (9ths, 11ths, 13ths, altered, sus, 6/9, slash): "Fmaj9", "E7#9", "Am/G". Names use sharps.
- **Voicing** (`chordset.c cs_voice`): RANGE, an octave window the chord's lowest note always stays in (so KEY moves
  a chord inside it, never by octaves); VOICING CLOSE / OPEN (drop 2) / SPREAD (drop 2 and 4; a triad 1-5-10) / AUTO
  (the chord's own); INVERSION 0..3 or AUTO, voice leading: the inversion closest to the chord before (over ii V I vi IV
  it moves the voices 70 semitones where root position moves 426); BASS, the slash bass or root an octave below.
- Tests: `tests/chordset_test.c` and `tests/shintech/test_chordsets.py` (the symbol grammar, the generator's
  checks, the table made from today's data, and each of the 1,200 factory chords' firmware name read back to its
  notes), in `tests/run_tests.sh`.

### Phase 0: build, baseline, gap analysis

- Branch `chord` from `upstream/main` in the existing ShinobiFPV forks (Felucca and Felucca-WebApp), checked out as
  worktrees `Felucca-Chord` and `Felucca-WebApp-Chord`, apart from Genesynth (`shintech`). Upstream remotes stay
  fetch-only.
- BUILDING-SHINTECH.md: the Windows 11 / WSL2 build, verified. Stock 1.8.1 builds (`felucca.fwsc` 610,286 B), all host
  tests pass.
- The browser emulator (`web/emu/`), removed upstream in `d483929`, is restored from `d483929^` unchanged, with its
  licence (DotGothic16, OFL), its credits (X0X; the emulator contributions of spinkham and ugai) and its stage in
  `tests/run_tests.sh`. It builds against 1.8.1 and passes its own test.
