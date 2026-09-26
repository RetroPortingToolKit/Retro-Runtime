# The hub ↔ runner link

How the hub runs a core in its own window. The core runs in
`retro-core-runner` (`CORE_RUNNER.md`), a child process; this link is how the
two talk. The design is `HOST_LIFECYCLE.md` §4; this page is what was built.
Code: `corelink/` (shared, with the per-OS transport in `transport_*.cpp`),
`runner/runner_link.cpp` (runner side), and Retro Launcher's `src/hub/hub_play.*`
(hub side).

**Status, 2026-09-26: Linux, macOS and Windows.** The transport table below is
Linux's. macOS and Windows use the transports in `LINK_TRANSPORTS.md`; the
protocol is the same on all three.

## Transport

| Channel | What | Owner |
|---|---|---|
| control | `SOCK_SEQPACKET` socketpair, one message per packet; the runner's end at fd 3 | — |
| shared region | one `memfd` at fd 4: header, 3 frame slots of 1024×1024 RGBA, an audio ring | **hub** creates it |
| save memory | one `memfd` per save region, sent to the hub with `SCM_RIGHTS` | runner creates it (sizes are known only after `load()`); **hub** fills and persists it |

Everything the hub needs after a runner crash is memory the hub holds: the
picture, the queued audio, and every save region. EOF on the socket is how the
hub learns the runner died.

## Versioning

The runner is released and updated separately from the hosts that start it
(`Retro-Runtime`), so the link is versioned the way `rcore.h` is:

- **Protocol major.** Host and runner must agree exactly. The host writes its
  major into the shared header, and the runner refuses a mismatch before
  anything else, naming both: "protocol major 2 from the host, 1 in this
  runner -- update whichever is older" (exit 2).
- **Protocol minor.** Only append-only edits: a new message type, or a field
  at the end of a message or of the shared header. The host states its minor
  in the shared header and the runner states its own in Hello. The session
  speaks the lower of the two, and neither side sends anything the other's
  minor does not know.

**Known defect (2026-09-26): the minor rule is not implemented.**
- `as_msg` accepts only a packet of exactly `sizeof(M)`, and every sender
  sends `sizeof(M)` whatever the session's minor.
- So a field appended in a minor release would make an older peer silently
  drop that whole message.
- Before the first minor bump, either senders must size messages by the
  session's minor, or `as_msg` must accept a longer packet and ignore the tail
  (`LINK_TRANSPORTS.md` §10).

**1.0** (2026-09-25) is the layout described on this page. It resets the
unreleased development counter, which had reached 3.

## A session

```
hub                                    runner
 spawn (argv = headless flags + --link)
                           <-- Hello        identity: sha256, id, version, caps
                                            (the core's; not the package's)
                                            set options, lend GL, init, load
                           <-- SaveRegions  one memfd per region
 map, fill (erase value, then file)
 SavesFilled + seats before frame 1 -->
                                            adopt saves, load state if asked
                           <-- Ready
 Grant(frame k, every seat's pad) -->       run_frame; picture -> shared slot
                           <-- FrameDone(k)
 ...
 Quit -->                                   unload, deinit, exit 0
```

- **A game package rides in argv.** `LaunchSpec::package` becomes
  `--package <path>` (a `game_package` core needs it; any other refuses it,
  exit 2 before `Hello`). Hello is unchanged in 1.0 and carries only the
  core's hash; the runner prints the package's hash to `runner.log`, and a
  hub that needs it as data hashes the file it passed. `retro-core-link-test
  --package` drives it, and `probe_runner` reads `game_package` from
  `--version` (`RunnerVersion::game_package`, 0 for a runner from before it).
  It reads `describe` the same way (`RunnerVersion::describe`): whether the
  runner answers `--describe`, which is not a link session (`CORE_RUNNER.md`).
- **Input rides inside each Grant**, so the contract's "identical within one
  frame" holds by construction.
- **SavesFilled carries the seats as they stand before frame 1.** A core may
  read input outside a frame, and a seat's `connected` flag is guest-visible.
  Without this, n64lle reading the controllers while `unserialize()` restored a
  state saw "no controller" over the link but "port 1 connected" headless, and
  every savestate scenario diverged. That was found by the byte comparison
  below and is fixed in protocol version 2. The general rule is now in
  `CORE_ABI.md`.
- **Frames go through a lock-free triple buffer.** The runner owns the back
  slot, the hub owns the front slot, and one atomic `middle` with a fresh bit is
  exchanged between them. Neither side waits, and neither can touch the slot
  the other is using.
- **Audio** is an SPSC ring. If the hub stops draining it (paused), the runner
  drops samples and counts them rather than blocking.
- **One grant is outstanding at a time.** Pausing is simply not granting.

## Pacing

While the core produces audio, the hub paces grants by it, granting while
fewer than 60 ms are queued in its SDL audio stream. The audio is what the
player hears, so it is the clock that must not drift. With no audio yet, or no
audio device, the hub paces by the core's stated frame rate
(`set_frame_rate`, rev 5, carried in the shared header). Only a core that
states neither falls back to a fixed 60 Hz.

## Direct mode

`retro-hub --run-core <core> [--package <shim>] --rom <image> [--title-dir D]
[--tpak1-rom GB --tpak1-save SAV] [--opt key=value ...] [--no-gl]` boots
straight into the core and exits when the player closes it. This is the shape a
standalone release takes (`HOST_LIFECYCLE.md` §3). `<core>` is a per-title core
(`<title>_core.so`) or a generic one with the `game_package` capability (such as
`n64lle_core.so`), which then needs `--package`, the title's generated-code
package; `--title-dir` then defaults to the shim's directory.

- **Files:** `<stem>` is the shim's stem when `--package` is given, else the
  core's.
  - session logs go to `<data_dir>/sessions/<stem>/` (`runner.log`,
    `core.log`, `events.tsv`);
  - saves go to `<data_dir>/saves/<stem>/<region>.sav`, except where named by
    a flag.
- **Runner:** `RETRO_CORE_RUNNER` wins; otherwise the newest compatible of the
  bundled runner (beside `retro-hub`) and `<data_dir>/runtime/<version>/`. The
  source is logged. With no usable runner, or `--package` and a runner without
  `game_package 1`, the window shows the error and the hub exits 1. A background
  runtime update applies from the next launch, never mid-session.
- **Menu:** the guide button, Esc or F1 open the paused quick menu (Resume,
  Close game). F11 toggles fullscreen.
- **Input:**
  - Gamepads fill seats 0–3 in the order SDL lists them.
  - With none attached, the keyboard is port 1: arrows = D-pad, X/Z/C/S = the
    face buttons, Q/E = shoulders, Shift = Z/L2, IJKL = left stick,
    Enter = Start.
- **Faults:** a runner exit the player did not ask for shows the exit code,
  the runner's reason, FAULT and DISPATCH_MISS events, and the last 40 lines
  of `runner.log`. Saves are written before that screen appears. The runner's
  own `--package` refusals (exit 2) appear on this screen.

## How it was checked (2026-09-25)

All on `pokemonstadium_core.so` built clean from n64lle `ffa84cfc`.

- **The link reproduces the machine exactly.** `retro-core-link-test`
  drives a core through `CoreLink` the way the hub does and writes headless
  mode's artifacts. Through n64lle's `core_parity.sh`, all 10 scenarios give
  artifacts byte-identical to that build's own `rcore_probe`: `summary.txt`,
  every `state_hash.tsv` row, `shot.ppm` taken from shared memory,
  `events.tsv` and `core.log`. That includes the Transfer Pak and
  input-script scenarios.
- **The hub runs it.** `retro-hub --run-core` ran on SDL's offscreen video
  and dummy audio drivers for 25 s: about 1,380 frames at about 60 fps under
  audio pacing. When the hub was killed, the runner saw EOF, unloaded the core
  (`RUN_DONE fields=1383`) and exited, and no runner was left behind.
- **Saves survive a crash.** The runner was `SIGKILL`ed 763 frames into a
  Transfer Pak session. The hub reported exit -9 and rewrote the pak's save
  from its own mapping afterwards.
- **Frame-rate pacing** (rev 5, protocol v3). `tests/rcore_fake_core.c` is a
  silent core that states 50/1 after `load()`. The hub, offscreen with dummy
  audio, ran it for 10.07 s of wall time including start-up: 468 frames,
  about 50 fps. The 60 Hz fallback would have run about 565. After the
  shared-header change, `boot300_default` and `slot02_tpak` through the link
  are still byte-identical to `rcore_probe`.
- **Not checked:** the picture on a real screen, sound, controller feel and
  the menus. No one has looked at them. That verdict is Alex's.

## Not built yet

- **Per-title options, accessories and save choice** from the title page.
  Library launch itself exists: see `CORE_LIBRARY.md`.
- **Netplay through the runner** (rev 4).
- **Savestates from the quick menu,** with the envelope.
- **Options UI.** Options come only from `--opt`.
- **Accessory binding UI, per-seat remapping, hot-plug.**
- **The hub on macOS and Windows.** Retro Launcher builds `hub_play.cpp` only on
  Linux.
