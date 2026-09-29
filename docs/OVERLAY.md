# The play overlay — `retro_overlay`

What a host draws over a running core: an FPS readout, a TURBO marker, a volume
meter, toasts, and the save-state browser. Code: `overlay/` (the library),
`state/` (the savestate envelope it lists), and the runner's savestate path
(`runner/state_keeper.*`, link 1.1 in `CORE_LINK.md`).

**Status, 2026-09-26: built, and used by Retro Launcher's hub** (branch
`feat/runtime-overlay`, `src/hub/hub_play.cpp`). Checked as described at the
end of this page. Nobody has yet played with it on a real screen with a real
controller; that verdict is Alex's.

## Why it lives in the runtime

- **Cores never draw it.** A core owns no window (`rcore.h`). The overlay is
  composited after the core has produced its picture. It is not in the frame
  the core submitted, not in a savestate, and not in anything a netplay peer
  sees.
- **Hosts do not each re-implement it.** psxrecomp (`host_osd.c`,
  `psx_savestate_menu.c`), snesrecomp (`snes_osd.c`,
  `snes_savestate_menu.c`) and n64lle's own host (`host_turbo.rs`,
  `host_savestate_menu.rs`) each carried a copy. Their headers already argued
  against "25 copies". Under rcore every core runs behind the same host, so the
  one copy lives here.
- **It looks the same for every core.** It uses the same 8x8 font those three
  projects share (`font8x8_basic`), OSD text at 2x in translucent dark boxes,
  and the browser's layout and colours (n64lle's, which were psxrecomp's). The
  save-state legend draws face buttons by **position**, never one console's
  letters.

It depends on the C++ library and `rcore.h` only: no SDL, no GL, no clock. The
host passes the time in, feeds it events, and draws the images it hands back.

## Pieces

| Piece | Where | When |
|---|---|---|
| FPS | top left | while the host's "show FPS" is on |
| Toasts | top left, under FPS | about 2 s after something happened ("Slot 3 saved") |
| `>> TURBO` | top right, gold | while the host runs the core faster than its rate |
| Volume meter | right edge, centred | 1.5 s after the volume changes |
| Save states | centre | while the browser is open |

**FPS counts emulated frames**, the frames the core finished, not presents.
Under turbo the core runs several frames per present, and the reading should
say how fast the machine is running. The reading is the last 64 frames divided
by the time they span. It is not a mean of 1/dt: that reads high whenever
frames arrive unevenly. For example, 15 ms and 25 ms alternating gives a 1/dt
mean of 53.3 fps, where the true rate is 50. The reading starts afresh on a gap
over 0.5 s, when turbo starts or stops, and when the host calls `restart_fps()`
(the hub does on resuming from a pause).

## Drawing it (the host's side)

```cpp
for (const Layer& l : osd.layers(now_ns))   // + browser.layer() when open
    draw(texture_for(l.id, l.image, l.revision), place(l, win_w, win_h));
```

- **One texture per `Layer::id`.** Re-upload only when `Layer::revision`
  changes. The images are RGBA8 (the same byte order as an
  `RCORE_PIXEL_RGBA8` frame) with straight alpha.
- **Draw at `place()`, nearest-filtered.** `place()` puts every layer in the
  same spot on every host:
  - Corner and edge layers use integer scale `1 + height / 1800` (1 at 1080p,
    2 at 4K), inset 8 units per scale step.
  - The browser uses the largest integer scale that fits 90% of the window
    (2x at 1080p).
- It is window chrome. It does not scale or move with the letterboxed game
  picture.

## The save-state browser

Twelve slots per title: `<dir>/slot01.rstate` … `slot12.rstate`. Each is an
envelope (below). The **runner** writes and checks them. The browser lists
them, draws the panel, and turns input into **requests**. The host carries a
request to the runner and reports back with `finish(ok, detail)`.

| Input | Pad (by position) | Keyboard |
|---|---|---|
| open / close | SELECT + R1 (the psxrecomp / snesrecomp / n64lle chord), or the host's hotkey | host's hotkey (the hub: F7) |
| choose a slot | D-pad, left stick (held: 350 ms, then every 90 ms) | Up / Down; 1–9, 0, -, = jump to 1–12 |
| load | SOUTH | Enter |
| save | NORTH | S |
| back | EAST, Start | Esc, Backspace |

- **Pads are read physically**, not through the player's bindings, so a remap
  cannot strand the menu.
- **A request blocks the browser** until it is answered. While it waits, the
  panel says so ("SAVING SLOT 03...").
- **A save stays open.** The thumbnail appearing in its row is the player's
  evidence that it was written.
- **A good load closes the browser** and toasts.
- **A refused load stays open** and shows the runner's reason. Hashes are
  shortened to 12 digits on the panel; the full sentence is in the runner's
  log.
- **Slots from another core or build are marked** "OTHER CORE" or "OTHER
  BUILD", from the envelope's header against the link's Hello. The mark is a
  hint. The runner's load rule decides.
- **`InputGuard`**: buttons held when the browser closes (the chord, an Enter)
  stay out of the game until they are released.

## The envelope

`state/state_envelope.hpp` implements `CORE_ABI.md` "Savestates". The file is
laid out as:

- magic `RCSTATE\0` and a version;
- a text header of `key=value` lines;
- a 160x120 RGBA8 thumbnail;
- the core's bytes.

The header records the rcore ABI major, core id, core file SHA-256,
`state_compat_id`, game package SHA-256, content SHA-256, every accessory
binding, every `NETPLAY` option's value, and the core bytes' size and SHA-256.
For display it also records the frame number and time. Unknown header keys are
ignored, so a later version can add fields.

**The load rule** is `check_state()`. It follows the order `CORE_ABI.md`
gives, stops at the first mismatch, and names both values:

1. ABI major, core id.
2. `state_compat_id` if either side has one, otherwise the core file hash.
3. Package (only for a core with no `state_compat_id`: `CORE_ABI.md`
   "Savestates"), then content.
4. Accessories, then options.
5. The bytes' hash (a corrupt file).

A refused state stays on disk.

**The thumbnail is inside the envelope.** n64lle kept a sidecar file so that a
picture could never stand in the way of state compatibility. Here the envelope
is the host's format and the load rule ignores the picture. One file is
simpler to copy and sync. The runner takes the picture from the last frame it
published to the hub.

**Bare states still load where they always did.** A file without the magic
goes to the core unchecked through headless `--load-state` and the link's
launch-time `--load-state` (n64lle's gates write bare states). The browser's
requests accept only envelopes.

## The hub's hotkeys

These are the hub's choices, listed here because every core gets them:

| Key | Does |
|---|---|
| F3 | show / hide FPS (also a setting: "Show FPS", saved in `<data_dir>/play.ini`) |
| Tab (held) | turbo; sound is dropped while it runs |
| + / - (`=` `-` or keypad) | volume, 10% steps, meter on the right (saved in `play.ini`) |
| F7, or SELECT + R1 | save states |
| Esc, F1, Guide | the pause menu, which also has Save states, Show FPS and Volume |

- The recomp-ui family binds FPS to F. In the hub F drives C-Left (the TFGH
  right stick), so FPS is F3.
- Turbo holds on key events and drops when the window loses focus.
- Nothing here is bound on a pad except the browser's chord and the existing
  Guide.

## How it was checked (2026-09-26)

- **`retro-overlay-test`** (ctest `overlay`) checks:
  - the FPS method, including uneven arrival and turbo;
  - layer lifetimes and revisions, and `place()`;
  - the browser: chord edges, auto-repeat, requests, refusals, marks;
  - `InputGuard`;
  - the envelope: round trip, escaping, every load-rule mismatch in order,
    corrupt bytes, truncation.

  `--dump DIR` writes each layer composited over a picture, for a person to
  look at.
- **`link_savestates`** (ctest, fake core):
  - over the link, a state saved after frame 4 and loaded after frame 8 makes
    the last picture 6;
  - the same state in a session with other content is refused, naming both
    hashes, and the session runs on;
  - headless `--load-state` accepts the envelope.
- **The hub's real `PlaySession`** was driven by a scratch harness (not in any
  repo) on SDL's offscreen driver with injected key events, and each step's
  framebuffer was read back:
  - with the fake core: F3, Tab, `-`, F7, S, Enter;
  - with n64lle 0.375.0 running Pokémon Stadium (US 1.0, `pokemonstadium_game.so`):
    - the FPS readout, TURBO marker and volume meter drew in place;
    - the browser opened over the dimmed game;
    - a 2.97 MB state saved in about 40 ms, with the game's own frame as its
      thumbnail, and loaded back;
    - the session's `events.tsv` was empty (no miss, bridge or fault).
- **Not checked:** a real screen, a real controller (the SELECT + R1 chord,
  stick repeat), sound and volume by ear, and the settings page's new section
  as drawn. Those are Alex's to judge.
