# ShinTech CHORD changelog

ShinTech's fork of Felucca with a chord-synth mode (CHORD: chord sets on the keys, styles with variations, a chord
pattern sequencer) and a six-voice-style poly engine (POLY6), for personal use. Branch `chord` of
`ShinobiFPV/Felucca`, based on upstream Felucca `main` (v1.8.1 + the 1.8.2 flash fix, `a4f007c`). Upstream's own
release notes are in its wiki. Projects that use the new features will not open on upstream Felucca.

## Unreleased

### Phase 4: POLY6

- **POLY6, a six-voice-style analogue polysynth engine** (`firmware/src/eng_poly6.c`, GPL-3.0, engine 15), after DRUM in
  the engine list (PRESETS, its ENG knob, the EDIT layer's 14th white key, the editor). Per voice: one DCO (a
  band-limited saw and pulse from one phase), PWM (PW by hand, the LFO through LFO DEST SHP, the envelope through ENV DEST
  SHP), a square sub an octave down, noise; a 24 dB / octave zero-delay-feedback ladder (four trapezoidal poles, the
  feedback solved per sample, a soft clip in the loop: it self-oscillates at the top of RES and thins the bass as RES
  rises), KYBD tracking (127: one to one). Over the part, as on the original's signal path: AC coupling, the 4-position
  HPF (BOOST: a low shelf up, FLAT, two rising corners) and the **BBD chorus**: one delay line read at two taps a triangle
  LFO sweeps in opposite directions left and right (I 0.513 Hz, II 0.863 Hz over 1.66 .. 5.35 ms; I+II 9.75 Hz, 3.0 ..
  3.6 ms), its input and output roll-off and a little hiss, true stereo through the part's side path (a host change:
  fx.c mix_part / mix_spread take its two sides at full width while the chorus is on).
- **Controls:** EDIT 1 DCO: WAVE (SAW PULSE BOTH NONE) PW SUB NOISE; EDIT 2 VCF: CUT RES KYBD HPF; EDIT > POLY6:
  CHORUS (OFF I II I+II), VOICES (4ENV 8ENV 4GATE 8GATE: 4 voices by default, 8 the option; GATE: the VCA follows the
  key, the ADSR the filter only), the LFO's RATE and DELAY (its fade-in). The rest is the track's, as for every engine:
  one ADSR for the VCF (ENV DEST FLT: amount and, by its sign, polarity) and the VCA, the LFO's routings (PIT: the DCO's
  LFO, FLT: the VCF's, SHP: PWM), LEVEL, the sends to Felucca's delay and reverb, locks, automation, the matrix, MIDI
  LEARN (CC74 CUT, CC71 RES as ANALOG).
- **32 original presets:** pads (WARM PAD, GLASS PAD, SLOW SWEEP, DARK PAD), strings, brass, poly and house stabs, a
  rave chord, house organ, percussive organ, e-piano, house piano, clav, plucks, harp, bell keys, basses (FAT, SUB, ACID,
  PULSE, SEQ), leads, WHISTLE, NOISE SWEEP, WIND; each with its own chorus, voices and LFO (POLY6_X), categories for
  PRESETS > LIST, favourites (settings bytes no firmware used).
- **Formats:** two track parameters BBD and VOX (117, 118: P_E0 119, P_COUNT 127, the most parameters ids allow) in the
  project extension. A project with a POLY6 track is FUNB. **User presets:** a record that uses the fork (POLY6, or CHRD
  SET) is written as version 6 (the same layout as 4): upstream Felucca shows it empty and keeps its bytes; every other
  record stays version 4, as upstream reads it. Engine 15 is POLY6 in every build: a slot a build leaves out (SLICE 13,
  ADPCM 14) holds a silent engine nobody can pick, so stored engine numbers never shift.
- **CPU (an estimate):** this host has no instruction counter; POLY6 renders in 2.3 x ANALOG's host time a voice (2.4 x
  with the chorus). Scaled by ANALOG's known device cost that is about **28 % of the device for 4 voices with the chorus
  and 54 % for 8** (the shed check drops voices past the limit). MENU > SYSTEM > INFO > CPU on the FM-1 is the figure to
  trust.
- Tests: `tests/poly6_test.c` (the ladder against four TPT poles' response at three cutoffs, RES's peak, self-oscillation
  at the cutoff, bounded at every corner; the chorus's delay range and rate for I / II / I+II, left and right opposite,
  an impulse out of the left tap at its delay; stereo on / off; VOICES 4 / 8; GATE; every preset sounds with no DC or
  clipping; the cost; demos in `build/poly6_demo`). Upstream tests that pinned the engine count, its display order, the
  EDIT layer's keys, P_COUNT, the user preset versions and MIDI LEARN's id gap follow the new layout. Cost: 4.6 KB of
  code; its chorus lives in the part's engine memory (no new RAM).

### Phase 3: chord pattern sequencer

- **64 chord patterns, up to 64 steps each** (the full count the storage plan allowed), one bank for the whole device
  (as user presets are, not a project's). A pattern: LEN 1..64 steps of DIV (1/4 by default; any of N_DIV), the chord
  set and KEY it was written with, NEXT (LOOP, or the pattern that follows it), up to 16 chords played by hand, and per
  step a chord (REST, TIE, chord 1..12 of the set, hand chord H1..H16), a STYLE (KEEP, OFF, or a style and variation),
  a single note, and a tempo (KEEP, 40..240 BPM). `firmware/src/cpat.c`.
- **Playing:** a new track parameter `CPAT` (OFF, 1..64; id 116, P_E0 117, P_COUNT 125, in the project extension) names
  the pattern a CHRD SET track plays while the transport runs: from step 1 with PLAY, its chords voiced and led as the
  keys' are, to the style when the step's (or the track's) STYLE is on, else held as a key holds them (the ARP takes
  them), every note to MIDI OUT on the track's channel. A step's STYLE sets STYLE / VAR from it on; a tempo sets BPM
  (internal clock only); a single note plays one step long. At its end NEXT follows, its own set and KEY applied. CPAT
  may be locked or automated (a lock switches the pattern from that step), and the external clock steps it as it does
  the sequencer.
- **SONG:** each section plays the chord pattern its slot's project was saved with (CPAT there), from its step 1 with
  the section; a silent lane plays none.
- **Recording:** with the track armed and the transport running (no song), a chord key goes into the step playing (or
  the next, past its half) with STYLE / VAR as they are then; the steps that start while it is held become TIEs, the
  first after its release a REST. C5..G5's white keys write their note. While a track records its chord pattern, the
  same keys no longer record into its own steps.
- **Pages:** SEQ > (after AUTOMATION) **CHORD PAT** (CPAT, STEP, CHORD, STYLE), **PAT STEP** (STEP, NOTE, TEMPO),
  **PAT SETUP** (LEN, DIV, NEXT, INPUT SET / HAND), **PAT TOOLS** (ON STEP / PAT, COPY, PASTE, CLEAR: pick with the knob,
  OCT+ does it); CHRD SET tracks only. The panel shows the cursor step's chord name, the eight steps around it as named
  cells (the cursor, a STYLE change, a note, the step playing marked) and the whole pattern as a strip. The PRESETS knob
  moves the cursor. HELP has a hint for each.
- **Step entry** (armed, stopped, on those pages): a chord key writes its chord at the cursor and moves on; C5..G5 write
  their note (the cursor stays); INPUT HAND: the keys play their own notes and a chord held on them, when let go, is
  stored as a hand chord (root and intervals, voiced later like the set's) and written at the cursor. Each edit, key
  and tool is one undo (SAVE held: UNDO_CPAT; held again: redo).
- **Storage:** the bank is 7 flash objects of 10 patterns (3,284 B each), A / B, in 14 sectors 0xEA000..0xF7FFF of
  felucca-bsp's FL_XDATA1 (no data used them; bytes 0xF00.. stay erased). A RAM cache (12 patterns, in `.pool`) holds
  what the tracks may play next and what the pages edit, filled by the main loop (`cpat_store.c` cpat_poll); an edit is
  written back 2 s after it, only when the music is quiet (stopped, no key, nothing sounding: as the autosave), never
  while playing (an erase silences the audio). A power cut during a write leaves the pattern as it was.
- Tests: `tests/cpat_test.c` (exact chords at their steps within a block, TIE / REST, notes, STYLE, tempo, hand chords,
  NEXT, CPAT changed, the cache, STOP, recording, SONG sections, the pages, knobs, step entry, HAND input, tools, undo /
  redo), `tests/cpat_store_test.c` (all 64 through a power cycle, an object's other patterns kept, nothing else written,
  a power cut at each of a write's programs, a damaged newest copy, writes only when quiet). Renders: `chord_pat`,
  `pat_tools`. Cost: 9.4 KB of code, 64 B of RAM, 4.9 KB of pool.
- Not yet: hand chords are entered step by step (INPUT HAND), not recorded live; patterns are not in the editor's
  backup (Phase 5).

### Phase 2: styles and variations

- **Styles.** SCL > CHORD > CHORD SET > VOICING > **STYLE** (CHRD SET only): `STYLE` OFF / ARP1 ARP2 (arpeggios) /
  BEAT1 BEAT2 BEAT3 (the whole chord in rhythms: syncopations, triplets, offbeats and stabs) / STRM1..STRM4 (strummed
  phrases: simple, rhythmic, and a variation of each), `VAR` 1..12, `CHOLD` (HOLD) and `STRUM` (AUTO: the
  variation's own, or 1..60 ms between a strum's notes). With a style on, a chord key (or MIDI IN note) hands its
  voiced chord (SET, KEY, VOICING, INVERSION, RANGE, BASS, voice leading as in Phase 1) to the track's style player
  (`firmware/src/style.c`) instead of sounding it; the player loops the variation on the chord. Any engine but a kit.
  - The first chord starts the loop at its step 1 at once. Another chord while it plays changes the chord in place:
    the old notes end, and what still sounds of the events at that point (a pad, a long stab: half a step of it left
    or more) strikes again on the new chord, unstrummed. Letting go of the chord's key stops it.
  - **HOLD** (`CHOLD`, or F#3): the style plays on after the key is let go, until another chord takes over or HOLD is
    pressed again. With STYLE OFF, HOLD holds the chord itself the same way.
  - **Tempo, swing, clock:** the steps follow BPM, the track's and the global SWING (straight rates; triplets play
    straight), and the external clock (USB / TRS) the way the ARP does: its pulses step it (6 a 1/16), its Stop holds
    it. PLAY restarts a running style at step 1 with the sequencer.
  - Every note a style plays goes to MIDI OUT on the track's channel (the chord key itself sends nothing then).
  - **A3 / B3** VARIATION - / + (round the twelve), **F#3** HOLD: the shortcut keys Phase 1 kept for them.
  - The STYLE page shows the variation's name, the style and its rate over its loop drawn as steps (each event as long
    as it sounds, as high as the chord note it plays; accents bright; a tick a step, the beats brighter) and the step
    playing. HELP has its hint.
- **Style data:** 9 styles x 12 variations, all original, as text: `tools/shintech/styles/<n>-<code>.json`, each
  variation a loop of steps (rate 1/2 .. 1/32, 4T 8T 16T 32T) of events relative to the chord: "<step> <notes>
  <velocity> <length> [d|u]", notes A (all) B (bass) U (all but the bass), the n-th from the bottom or top, or the arp's
  next UP DOWN UP/DOWN RANDOM over 1..3 octaves; ' and , move an octave. `tools/shintech/gen_styles.py` checks them and
  writes `build/gen/chord_styles.h` (642 events, 4.5 KB). Edit the JSON and rebuild.
- Four new track parameters (ids 112..115: STYLE, VAR, CHOLD, STRUM; P_E0 116, P_COUNT 124), in the project extension
  like Phase 1's (a project using them is FUNB); kept on sound loads, lockable, automatable, MIDI-learnable. The
  built-in editor's tables and `web/EDITOR_PROTOCOL.md` have them. User preset records hold them already (143 slots).
- Tests: `tests/style_test.c` (notes at their steps within one block at 90 / 120 / 150 BPM, swing, triplets, strums
  up and down and the STRUM knob, lengths, release, HOLD, a chord changed on the go, VAR on the go, STYLE OFF, PLAY,
  the external clock's pulses and Stop, MIDI IN, HOLD with STYLE OFF, and all 108 variations over two bars with MIDI
  OUT balanced and nothing left sounding), `tests/shintech/test_styles.py` (the grammar, the generator's checks, the
  factory set's coverage, today's table); the UI renders draw `chord_style`. Cost: 8.8 KB of code (4.5 KB the data),
  128 B of RAM, 680 B of pool.

### Phase 1: CHORD mode and chord sets

- **CHORD mode on the keys.** SCL > CHORD, KNOB 1 to **SET** (after POW) on any engine but a kit. Then:
  - **C4..B4** play the twelve chords of the set, voiced as CHORD SET / VOICING say, each led from the chord before
    (chordset.c cs_voice). A release ends exactly the notes its key started.
  - **F3..B3** are shortcuts, silent: F SET -, G SET + (round the 100), G# VOICING (CLOSE OPEN SPRD AUTO), A# BASS
    on / off; F# HOLD and A / B VARIATION - / + are kept for the styles (Phase 2) and do nothing yet.
  - **C5..G5**: the white keys play their own note over the chord; C#5 / D#5 KEY - / + (round the twelve).
  - **MIDI IN** plays chord n % 12 of the set for note n (C of any octave: chord 1). MONO / LEGATO / UNISON play the
    chord's root. MIDI OUT sends every note on the track's channel, as the chord keys always did. The arp takes a SET
    chord as the notes it played.
- **Pages.** SCL now goes SCL > CHORD > **CHORD SET** (SET, KEY, BASS) > **VOICING** (VOICING, INVERSION, RANGE), the
  last two only while CHRD is SET (they leave for SCL when it no longer is). The SET knob names the set by its genre
  and number ("HSE2"); the page shows the chord's name ("Dm9", "G/B": chordset.c cs_name) and the set's name over the
  keys it sounds (up to four octaves for a wide voicing), and before any chord the set's first. HELP has a hint for
  both.
- Tests: `tests/chordmode_test.c` (exact voicings and voice leading on the real keys, BASS, every shortcut, single
  notes, MIDI IN / OUT, MONO, kits, the pages and their order); the UI renders sweep both pages' values and draw two
  new screens (`chord_set`, `chord_voicing`). Cost: 9.3 KB of code (5 KB the chord sets), 224 B of RAM.

- **Project format FUNB and the project extension.** Six new track parameters for CHORD mode, before the engine
  values as upstream adds them: SET, KEY, CVOIC (the voicing: CLOSE OPEN SPRD AUTO; not VOIC, the chord keys' label),
  INV (AUTO ROOT 1ST 2ND 3RD), RNG (C2..C6, default G3) and BASS, ids 106..111, plus P_CHRD's new value SET (P_COUNT
  120, P_E0 112). `web/EDITOR_PROTOCOL.md` and the built-in editor's tables have them. They are kept on sound loads (as
  the chord keys) and may be locked or automated.
  - FUN10 has no room for them (one spare byte), so a project keeps them in an **extension record** in two new flash
    sectors no data used before (0xE8000 / 0xE9000, felucca-bsp's FL_XDATA1, A/B). Nothing existing moved.
  - A project that uses none of the new features is still written as **FUN10 ("FUNA") exactly as upstream 1.8
    writes it**, and upstream Felucca opens it. One that uses them (CHRD SET, a CHORD value off its default, a lock
    on one) is written as **FUNB**: FUN10's layout with the magic "FUNB", the motion ids' P_COUNT at byte 67, the
    extension record's digest in bit 7 of 32 FM6 patch bytes. **Upstream Felucca does not open a FUNB project**: the
    slot shows EMPTY there and its flash is left alone.
  - Extension records are found by their content's digest; a save writes the extension before the project and keeps
    every record a stored project or the autosave names, so a power cut between the two writes leaves the old project
    with its own CHORD setup. A FUNB whose record is missing (a backup restored: backups do not carry the extension
    yet, see Phase 5) loads with the CHORD values at their defaults; the project itself is never refused for it.
  - Older projects (FUN10 of 1.2 .. 1.8 and every earlier format) load as before, CHORD off. User presets need no
    change (their records map 143 parameters by count both ways); their version bump comes with POLY6 (Phase 4).
  - Cost: 2,132 B of code, 7.2 KB of pool (the store mirrored, and its compaction copy).
  - Tests: `tests/funb_test.c` (FUN10 byte layout when nothing is new, FUNB round trips, missing / damaged
    records, byte 67's ids, the store, 2,000 random projects), `tests/funb_save_test.c` (the firmware's own save,
    load, rename, erase and autosave flows on simulated flash: power-offs, a cut between the two writes, the extension
    erased or its newest copy damaged), `tests/storage_test.c` (the new sectors, nothing else touched, 0xF00..0xF07
    left erased). Upstream tests that pinned P_E0 106 / P_COUNT 114 or the object list now check the fork's layout.

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
