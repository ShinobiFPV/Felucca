# CHORD mode and POLY6: Phase 0 notes, gap analysis and storage plan

Written against upstream `main` `a4f007c` (v1.8.1 + 1.8.2 flash fix). File references are `firmware/src/` unless
another path is given.

## 1. How the existing parts work

**Chord keys (SCL > CHORD, `chord.c`).** Two track parameters: `P_CHRD` (OFF, DIA3, DIA7, MAJ, MIN, DOM7, MAJ7, MIN7,
SUS4, POW) and `P_VOIC` (CLOSE, OPEN, INV1, INV2, +OCT). `chord_make` builds the chord on the note the key maps to
(`kb_map`: QNT, TRN, octave): DIA3 / DIA7 stack every other note of the track's ROOT + SCALE (a key off the scale
drops to the scale note below; CHR uses the major scale), the fixed shapes come from `CHORD_SHAPE`. The voicing then
moves tones by octaves (OPEN raises the 2nd tone, INV1 / INV2 raise the lowest one or two, +OCT adds the root an
octave down and drops a seventh's fifth) and sorts. At most `CHORD_MAX` = 4 notes. MONO / LEGATO / UNISON play the
root only, and kits (DRUM, SLICE) ignore it. Each source (`kb_chord[k]` per key, `mchord[]` per MIDI note) remembers
the notes it started, so a release ends exactly those. `chord_last[]` + `chord_name` ("Cm7", "F#m7b5") feed the
CHORD page drawing in `ui_graph.c`. There is no voice leading: each chord is voiced on its own.

**Arpeggiator (`seq.c` arp section).** `P_AMODE`: OFF UP DN UPDN RND ORD REPEAT DNUP UP+8 CONV DIVG PINKY THUMB WALK
CHORD; `P_ARATE` (N_DIV: 1/4 1/8 1/16 1/32 8T 16T 1/2 1/1 2BAR 4BAR), OCT 1..4, GATE, SWG, PROB, HOLD, ORD
(sorted / as played). Held notes are kept per track (`held[]`, with `held_key`/`held_dt` so a re-map of TRN / SCALE
moves them live). `arp_step` runs once per CTL block in the audio ISR: it advances `arp_pos` by the block (or by the
external clock's samples), and at `div_samples(rate)` ± swing it calls `arp_fire`, which builds the list
(`arp_list`) and picks the index (`arp_pick`). External clock: `arp_external_due` catches up whole steps from the MIDI
clock grid (`midi_arp_grid`). Everything ends in `trk_note_on` / `trk_note_off`.

**ANALOG and the render callback (`engines/src/eng_analog.c`, `felucca_engine.h`, MIT, upstream's Felucca-Engines).**
An engine is an `engine_t`: 8 parameters (`P_E0..P_E7`, EDIT 1 / 2), presets, `note_on(t, v)` and
`render(t, v, out, n, vmod)`, plus optional hooks: `amp` (own amplitude curve), `desc` (labels per mode), `block`
(once per part per block, before the voices), `keys` (own key map), `ownenv` + `done`, `mem` (per-part memory) and
`post` (once per part after the voices, on the mono sum). `render` **adds** n ≤ CTL = 32 samples (int32) per voice;
`vmod_t` carries the phase increment, the amplitude ramp amp0 → amp1, and the modulated cutoff and shape. ANALOG is
2 polyBLEP oscillators (SAW SQR TRI SIN PWM, + SYNC and SUB pairs), noise, drive, a Q12 trapezoidal SVF (LP/BP/HP)
with a soft knee. The host (`voice.c`) does the ADSR (Q24), the LFO, glide and the per-block modulation.

**Voice allocation (`voice.c`).** `NVOICE` = 8 is both the per-part voice array and the **global budget shared by all
four parts**. `voice_alloc`: the same note reuses its voice; otherwise ROTATE (round-robin) or REUSE (nearest pitch);
when the budget is full `voice_victim` takes, across all parts, the oldest released voice, else an extra UNISON voice,
else the oldest held POLY voice that isn't that part's lowest note (the bass is protected). A voice taken from
another part fades out over one block (stage 4). `engine_t.poly` caps an engine's voices (0 = 8).

**Sequencer (`seq.c`) and SONG (`song_chain.c`).** Each of the 4 tracks owns a 64-step pattern (`step_t`: up to 4
notes, TIE / REST, accent, slide, RATCH ×1..4, NUDGE, chance / conditions, 8 drum-lane hits), with its own
LEN / DIV / SWING / GATE (polymeter). Live recording quantises to swung steps and writes held lengths as TIEs.
Locks and automation are `motion_store_t` (128 records of {step, param id | LOCK bit, value}). SONG is a chain of up
to 16 sections; each names a project slot (A..D or silent) for each track and a repeat count. Before PLAY the slots in
use are copied into `chain.source[]` and the tracks read their steps / motion / timing from there.

**Undo (`ui.c`).** One level: SAVE held swaps back the last load or edit (UNDO_SOUND, UNDO_PAT, UNDO_MOT) of one
track, from a copy in `.pool`.

**Serialization (`project.c`, `upreset.c`, `storage.c`).** Flash objects have A/B sectors with a 32-byte commit
record written last (magic, seq, length, CRC), so a torn write keeps the older copy. A project (format "FUNA" /
FUN10) is one 3,840-byte object: globals, 4 × (P_COUNT byte parameters + engine + preset + 64 × 9-byte steps), the
song chain, motion, 4 packed FM6 patches, MIDI channels, the name and a hash. Older formats (FUN1..FUN9) are read
and converted, and parameters are mapped by count (new ones go just before `P_E0`). **FUN10 is full**: 13 spare bytes
(3 more per-track parameters). User presets: 32 slots in banks at 0xDC000. Settings at 0xFC000. Autosave at 0xE5000.

**Free flash.** The 1 MiB NOR: app slot up to 0x93000; store 0x97000..0xDFFFF (projects 0x97000..0x9EFFF, user
samples 0xA0000..0xDBFFF, user preset banks 0xDC000..0xDFFFF); OTA staging 0xE0000..0xE4FFF; autosave 0xE5000..0xE6FFF;
FM6 voice bank 0xE7000; settings / globals 0xFC000..0xFEFFF. felucca-bsp reserves two areas for a firmware's own
data: **XDATA0 0x93000..0x96FFF (16 KiB) and XDATA1 0xE7000..0xFBFFF**, of which 0xE8000..0xFBFFF (80 KiB) is unused.
That makes **24 free 4 KiB sectors**. Constraint: the update loader clears what looks like an update record at offset
0xF00 of each such sector, so objects there must keep bytes 0xF00..0xF07 non-record (simplest: payload ≤ 3,584 B,
ending before 0xF00).

**CPU load.** `audio.c fm1_alnk0_irq` times each half buffer (128 frames, 2.9 ms) with `fm1_ticks()` and keeps a
running average `song.cpu_q8` (1/256 of real time). MENU > SYSTEM > INFO shows it as a percentage, refreshed every
500 ms; the USB console has `cpu_pct`. `shed_check` drops voices when the load passes its limit. The optional second
core (MENU > SYSTEM > 2ND CORE, beta) can render some parts.

**Headroom (stock).** Code 63,960 B free of the 581,564 B app slot. RAM 7,308 B usable, pool 34,212 B usable.

## 2. Gap analysis

R = reuse, E = extend existing code, N = new.

### Phase 1: CHORD mode and chord sets
| Feature | | How |
| --- | --- | --- |
| A per-track CHORD mode | E | A new `P_CHRD` value `SET` (append-only list) makes `chord_make` take the chord from the current set instead of a shape. All of the existing key → note → `trk_note_on` path, `kb_chord`/`mchord` release tracking, MONO root and kit rules are reused. Works on any engine. |
| 12 chord keys + shortcut keys | E | `keyboard_block` / `kb_map` (27 keys F3..G5). In SET mode the chord keys map to chord index, not pitch. |
| Chord set data, ~100 sets | N | `tools/shintech/chordsets/*.json` → `tools/shintech/gen_chords.py` → `build/gen/chord_sets.h` (wired into build.py as the other generators are). Packed: a shared table of interval shapes (one byte per chord's shape), 3 bytes per chord, ~45 B per set → ~4.5 KB. |
| Chords up to 6 notes | E | `CHORD_MAX` 4 → 6 for SET chords (9ths, 11ths, 13ths). The 8-voice global budget still applies (see risks). |
| KEY transpose, VOICING close/open/spread, RANGE | E/N | Transpose: `P_TRANS`/`P_ROOT` exist, reuse. VOICING: replace the per-chord switch with a voicing function. RANGE: new, an octave window. |
| INVERSION / voice leading | N | A voice-leading pass: choose the inversion and octave of each chord that keeps it closest (sum of semitone moves) to the previous chord inside RANGE. Pure, host-testable C. |
| Chord name + keyboard drawing | E | `chord_name` covers triads/7ths/sus/6; extend for 9/11/13, add9, slash bass. The CHORD page in `ui_graph.c` is extended with a mini keyboard. |
| New track parameters | E | P_COUNT grows (set, style, variation, voicing, range, hold, MIDI out mode…): FUN10 has room for 3, so this forces the project format bump (storage plan). |

### Phase 2: styles and variations
| Feature | | How |
| --- | --- | --- |
| Style engine (9 × 12) | N | A per-track phrase player next to `arp_step`: its own position/index fields, firing chord-relative events (note index, velocity, length, strum offset). Ends in `trk_note_on/off` like the arp. |
| Timing, swing, external clock | R | `div_samples`, `swing_step_len`/`track_swing`, the `midi_arp_grid` catch-up of `arp_external_due`, `beat_pos`. |
| Arp styles (1–2) | R | `arp_pick` modes (UP, DN, UPDN, RND) and rates (N_DIV, includes 8T/16T); 1/32T is not in N_DIV, it will be a style's own rate. |
| Beat / strum styles (3–9) | N | Step-pattern events; strum = per-note offset in samples, up / down order. |
| Style data | N | `tools/shintech/styles/*.json` → `gen_styles.py` → `build/gen/chord_styles.h`. ~16 events × 2–3 B per variation → ~5 KB. |
| HOLD | E | `latched`/`P_AHOLD` logic exists for the arp; the CHORD HOLD reuses it with the "new chord or HOLD again releases" rule. |

### Phase 3: chord pattern sequencer
| Feature | | How |
| --- | --- | --- |
| 64 patterns × 64 steps | N | A separate store (below). A pattern is a different kind of data from `step_t`, so it does not reuse the track's pattern. |
| Step input, real-time recording | E | Reuse the step cursor/UI conventions of SEQ and `rec_note`'s quantisation; new step editor for chord/style/note/tempo fields. |
| Chaining, SONG | E | `chain_row_t` names project slots per track; a CHORD track's section needs a chord pattern number. Plan: the chord pattern number of a CHORD track per section goes in the project extension (below), so FUN10's chain layout stays as it is. |
| Copy/paste, clear, undo | E | `ui_tools.c` copy/paste and `ui.c` undo (new UNDO_CPAT kind, one pattern copy in `.pool`, 348 B). |
| Tempo changes per step | N | Sets `G_BPM` (internal clock only; ignored under external clock). |
| Pattern screen | N | New page, a row of steps with chord names. |

### Phase 4: POLY6
| Feature | | How |
| --- | --- | --- |
| Engine | N | `firmware/src/eng_poly6.c` (GPL-3.0, ours; not in the MIT Felucca-Engines submodule). One `engine_t`. |
| DCO saw + pulse/PWM + sub + noise | R | polyBLEP `osc_saw`/`osc_pulse`, `noise32` from `dsp.c`; sub = square at half phase. |
| 4-position HPF | N | One-pole HP with 0 = low-shelf boost, 1 = flat, 2/3 = rising corners. |
| 24 dB ZDF ladder with self-oscillation | N | A fixed-point TPT 4-pole ladder with a one-step feedback solve and a tanh (`TANH_Q15`) in the loop. The main CPU risk. |
| ENV amount/polarity, LFO, KYBD | R | `vmod.cutoff` already carries ENV DEST FLT + LFO DEST FLT + matrix; polarity = sign of `P_ED_FLT`. Keyboard tracking as ANALOG's KTR. |
| One ADSR for VCF + VCA, VCA gate mode | R | The host ADSR; a gate mode uses `amp` hook returning full level while gated. |
| LFO with delay | R | Track LFO + `P_LFADE` (fade-in) is the delay/attack. |
| More than 8 parameters | E | The engine has 8 EDIT values; POLY6 needs ~20. Like FM6's packed patch: a per-track POLY6 patch (~24 B) in the project extension and in user presets' extension, edited on extra EDIT pages. The 8 E-values hold the most-played controls (so locks/automation/matrix E1..E8 reach them). |
| BBD chorus I / II / I+II | N | Per part, in the engine's `post` hook: two modulated fractional delay lines (triangle LFOs, anti-phase L/R), a gentle low-pass and a little noise. Stereo: the part's mix is mono + a side signal (SPREAD path, `mix_spread`), so the chorus writes mid into the part buffer and side into the spread buffer: a small host change in `fx.c`. Delay memory ~2 × 1,024 samples × 2 B = 4 KB per part (pool or engine `mem`). |
| 4 / 8 voices | E | `engine_t.poly` is constant; a new per-track VOICES setting caps `trk_nvoice`. Note: 8 is the whole device's budget. |
| 32 presets | N | `preset_t` table + patch table. |
| Delay / reverb | R | Track sends to the global delay / reverb. |

### Integration
| | | |
| --- | --- | --- |
| Locks, automation, matrix | R | They act on track parameters by id; new CHORD/POLY6 parameters are ordinary `P_*` (ids < 128: P_COUNT is 111 today; room). |
| MIDI LEARN, Program Change | R | Parameter-id based; PC loads presets as now. |
| FX layer | R | Unchanged path. |
| USB MIDI out of chord/style notes | E | `midi_out_event` is sent today only for keys (`key_on`/`key_off`); add it for style and pattern notes on `trk_midi_ch`, plus a LOCAL setting (internal engine on/off). |
| External clock USB/TRS | R | `G_CLOCK`, `midi_arp_grid`. |
| Tests | E | `tests/run_tests.sh` + host C tests, as upstream's. |

## 3. Storage plan

**A chord pattern: 348 bytes.**

| Bytes | |
| --- | --- |
| 4 | length 1..64, chord set, key (transpose), flags |
| 8 | name |
| 64 | 16 hand-played chords: root, 12-bit interval mask, bass/voicing (4 B each) |
| 16 | 8 tempo changes: step, BPM |
| 256 | 64 steps × 4 B: chord ref + kind (chord / tie / rest / note-only) + tempo flag; style variation (0 = keep, 1..108); single note; velocity / octave |

**Bank:** 10 patterns per object (3,480 B + header ≤ 3,584 B), so **64 patterns = 7 objects × A/B = 14 sectors**. All of
it is in **XDATA1 / XDATA0, which no data uses today**; nothing existing moves.

| Object | Sectors |
| --- | --- |
| Chord pattern bank (64 × 64) | 14 |
| Project extension: for each of 4 slots + the autosave, CHORD track settings, the CHORD patterns of SONG sections, each track's POLY6 patch (≤ 700 B each), tagged with the hash of the project it belongs to | 2 |
| User preset extension: each of 32 user presets' POLY6 patch | 2 |
| User chord sets (from the web editor) | 2 |
| Spare | 4 |
| **Total** | **24 of 24** |

**Formats.** The project becomes **FUN11** ("FUNB"). Its main record keeps FUN10's layout and size (one 3,840 B
object: there is no room to grow it), holding the parameters FUN10 has; the new CHORD and POLY6 track parameters are
written to the project extension object instead (by id, so later additions need no layout change). FUN10 and every
older format still load (new parameters at defaults: CHORD off, which plays as before). Upstream Felucca refuses FUN11 (a slot shows EMPTY there) and never
reads the XDATA objects; the flash copy is left alone until saved over. User presets: `UP_VER` 4 → 6 (5 is taken by
the grid variant), older ones still read. A project extension whose tag doesn't match its project is ignored
(defaults), so a power cut between the two commits loses only the CHORD settings of that save, never the project.
