# retro-core-runner

The child process that runs an rcore core for the Retro frontend. Design:
`HOST_LIFECYCLE.md` (process model, states) and `CORE_ABI.md` (the contract).
This page covers what exists and how it is checked.

**Status, 2026-09-25:** two modes. **Headless** is described here. **Link**
(`--link`) is how the hub runs a core in its own window; see `CORE_LINK.md`.

## What it does today

1. **Loads the core and computes its identity.** It opens the library once,
   hashes it (SHA-256) through that open file, and loads the same file
   (`/proc/self/fd/N` on Linux). The printed `identity` names the code that
   actually runs. It then resolves `rcore_entry` and refuses a core that will
   not serve `RCORE_ABI_MAJOR`.
2. **Checks the sidecar** (`<stem>.rcore.toml`) field for field against the
   library's own `rcore_core_info`: ABI major, id, version, library file name,
   platforms, capabilities, `state_compat_id`. A missing sidecar, an unknown
   key or section, or any disagreement is a refusal that names both values.
   A `game_package` core whose sidecar carries `[title]` is refused too
   (`CORE_ABI.md`, "Per-title versus generic cores").
   The draft revision is reported, not enforced: the append-only rule and
   `struct_size` handle an older core. The same rule for a NEWER core
   (2026-10-01): a capability bit the library declares that this runner
   cannot name, or a name in the sidecar it does not know, is logged on
   stderr as `WARN: ... (newer than this runner's rev N); ignored` and left
   out of the comparison -- the core runs without that feature offered. The
   capabilities this runner does know are still compared both ways, so a
   known one missing from either side refuses as before. The `core:` line
   prints the library's capability word in full as hex, unknown bits
   included; no --version or --describe record names bits.

   **The game package** (`--package <library>`, 2026-09-26). A core
   declaring `game_package` is generic: its title's generated code is a
   separate library, passed as `rcore_load_params.package_path`. The rule,
   headless and `--link` alike, all exit 2 before `init`:
   - a `game_package` core without `--package` is refused;
   - `--package` for a core without `game_package` is refused;
   - a `--package` that is not a readable file is refused.

   The runner hashes the package (SHA-256) and prints
   `package: <path> sha256 <hex>` after the core's `identity` line, so over
   the link it lands in `runner.log`. The core opens the package itself, so
   this hash names the file as the runner read it, not a handle the core
   loaded through (unlike the core's own identity).
3. **Hosts the session** (`src/runner/host_session.*`), the part every mode
   shares:
   - declared options with their defaults, overrides on top; an undeclared
     key from the core is a fault (exit 3);
   - host-owned save memory filled with each region's `erase_value`, then
     from its file, and written back at unload;
   - `wall_clock_us`, real time offline;
   - a lent GL context: a hidden SDL3 window's, when built with SDL3.

   Frames, audio, input and events go to a `Sink`: files in headless mode,
   the hub's shared memory once the link exists.
4. **Headless mode** runs N frames and writes what n64lle's parity gate
   compares. Its command line and outputs match n64lle's `rcore_probe`
   (`--core --package --rom --title-dir --out --frames --load-state --tpak1-rom
   --tpak1-save --tpak1-rtc --gl --strict --no-seats --replay-at --opt
   --input-script --list-options`). The Transfer Pak flags go up to seat 4
   (`--tpakN-rom`, `--tpakN-save`, `--tpakN-rtc`, N = 1-4; 2026-09-27): each
   seat with a cartridge gets an `n64.transfer_pak` binding at slot 0, and its
   save regions are `tpakN` and `tpakN.rtc`. `--input-script F:buttons,...` holds
   `a b z l r start dup ddown dleft dright` and, since 2026-09-29, `cup cdown
   cleft cright`: the right stick at full deflection (+y up), where n64lle
   reads its C buttons.

   **Data accessories** (`--vruN`, N = 1-4; 2026-10-01, rcore rev 7,
   `CORE_ABI.md` "Accessory data"). `--vruN` plugs the VRU microphone, the
   `n64.vru` data accessory, into seat N-1, slot 0, with no content: an
   `rcore_accessory_binding` whose `content_path` is NULL, after the Transfer
   Paks in `rcore_load_params.accessories`. Headless and `--link` alike it is
   refused (exit 2, naming the core and the type) unless the core declares
   `accessory_data` and lists `n64.vru` in `accessory_types()` with that seat
   and slot in its masks -- this is the first thing the runner reads
   `accessory_types()` for. A `NETPLAY` type is also refused in a netplay
   session (`--net-*`): its bytes are not replicated to the peers yet.
   - **The bound seat reads no pad.** `input_get` for that seat answers
     `connected = 0`, decided in `HostSession` on the one input path every
     mode shares, so neither the headless script nor a hub's grant can leak a
     pad into a port the console reads as the VRU alone.
   - **`--accessory-script <file>`** feeds the accessory headless: one
     `frame<TAB>seat<TAB>slot<TAB>bytes` line per message (`#` lines and
     blank lines skipped), each one part of the named frame's sequence --
     what `accessory_poll` hands the core during that frame, in file order,
     and on a `--replay-at` second pass the same again. A line for a
     (seat, slot) nothing is plugged into is refused before the core runs.
     Everything the core sends back through `accessory_notify` prints to
     stderr as `ACCESSORY_NOTIFY <seat> <slot> <bytes>`, in order, as it
     happens.
   - **Queues** (`host_session.*`): per (seat, slot), what arrived before a
     frame began is that frame's sequence; `accessory_poll` hands it out one
     message per call, returns 0 at the end and rewinds, so a drain repeated
     within the frame sees the same sequence; the next frame boundary drops
     what the core did not take. Over the link the hub's `AccessoryData`
     messages are the arrivals (`CORE_LINK.md`, 2.1), and `accessory_notify`
     goes to the hub as `AccessoryNotify` the moment the core calls it.
5. **Savestates** (`runner/state_keeper.*`, 2026-09-26). The runner writes
   and checks the savestate envelope (`OVERLAY.md`, `CORE_ABI.md`
   "Savestates"). It holds every identity the load rule compares: the core's
   hash, the package's, the content's, the accessories', and the NETPLAY
   options.
   - **Link mode** serves 1.1's `SaveState` / `LoadState` between frames
     (`CORE_LINK.md`). It logs `state: save|load <path>: ok, N bytes` or the
     reason to `runner.log`. It hashes the content on a background thread
     from the start, so the first save does not wait for it.
   - **`--load-state`**, headless and at link start, checks an envelope by
     the load rule. A refusal is exit 2, naming the first mismatch. A file
     without the envelope's magic is a bare core state, as n64lle's gates
     write them, and goes to the core unchecked as before. Headless prints
     `state: loaded <path> (envelope|bare, N bytes)`.

5. **Describes a core** (`--describe`, 2026-09-26) for a host building a
   settings page: `--describe --core <library> [--package <library>]`, no
   `--rom`. See "--describe" below.

`--version` prints the release version, commit, link protocol and rcore ABI
compiled in, whether `--gl` is available, `game_package 1` (this runner
takes `--package`), `describe 1` (this runner answers `--describe`),
`transfer_pak_seats 4` (it takes `--tpak1-rom` to `--tpak4-rom`; a runner
without the line takes seat 1 only), `netplay 0|1` and `accessory_data 1`
(it takes `--vru1` to `--vru4` and speaks link 2.1's accessory messages; a
runner without the line takes neither), and exits 0 (`RELEASES.md`).

Exit codes:

| Code | Meaning |
|---|---|
| 0 | ok |
| 1 | a frame failed, or the core reported a FAULT |
| 2 | refused before the core ran: arguments, load, ABI, manifest or game package |
| 3 | the core broke the contract: an undeclared option key |

## `--describe`

What a core declares, printed without a ROM and without a session. The runner
loads the library and checks its sidecar exactly as for a run (a refusal is
exit 2, message on stderr, nothing on stdout). It then reads `options()` and
`input_descriptors()`, which `rcore.h` declares "before load" and which the
session already reads before `init`. It calls neither `init` nor `load`, and
it does not refuse an `OWNS_LOOP` core, because nothing is driven.

`--package` gets the same refusals as for a run (a core without
`game_package` refuses one; it must be a readable file). A `game_package`
core is described without one too: a package reaches a core only through
`load()`, so it cannot change what is declared before it.

Stdout is UTF-8, one record per line, fields separated by one TAB, and
nothing else:

```
describe	1
core	<core_id>	<core_version>	<platforms>
option	<key>	<type>	<flags>	<has_default>	<default>	<int_min>	<int_max>	<label>	<description>
value	<key>	<one enum value>
input	<button>	<axis>	<axis_direction>	<label>
accessory	<id>	<label>	<flags>	<seat_mask>	<slot_mask>
```

- Every field escapes `\` as `\\`, TAB as `\t`, LF as `\n` and CR as `\r`, so a
  record is always one line. A NULL C string is the empty field.
- `<type>` is `enum`, `bool`, `int` or `string`; a type this runner does not
  know prints as its number.
- `<flags>` is a comma-separated subset of `restart,netplay,developer`, in
  that order, or `-` for none.
- `<has_default>` is `1` when `default_value` is non-NULL, else `0` (unset:
  the core applies its own default); `<default>` is the value or empty.
- `<int_min>` and `<int_max>` are decimal int64, printed for every type.
- `value` records follow their enum option, one per entry of its
  NULL-terminated `values`, in declared order.
- `<button>` is the `RCORE_PAD_*` bit in decimal (0 for an axis),
  `<axis>` is as declared (`RCORE_AXIS_*` + 1, or 0), and `<axis_direction>`
  is a signed decimal.
- `accessory` records (2026-10-01, with `accessory_data 1` in `--version`)
  are the core's `accessory_types()`, in declared order: `<flags>` is a
  comma-separated subset of `content,save,netplay`, in that order, or `-`;
  `<seat_mask>` and `<slot_mask>` are lowercase hex without a prefix. A host
  offers a seat's pak dropdown from these (the Transfer Pak, the VRU) rather
  than guessing what the core takes where.
- Options, inputs and accessories appear in declared order. `describe 1` is
  the format's version; a new record TYPE does not change it, so a reader
  skips a record whose first field it does not know.

## How it is checked

Because the command line matches, n64lle's own gate grades this runner with no
change:

```sh
RCORE_PROBE=build/retro-core-runner \
  <port>/n64lle/tools/rust_parity/core_parity.sh check  <port>/build-release/<title>_core.so
RCORE_PROBE=build/retro-core-runner \
  <port>/n64lle/tools/rust_parity/core_parity.sh replay <port>/build-release/<title>_core.so
```

**Measured 2026-09-25** on `pokemonstadium_core.so` built clean from n64lle
`ffa84cfc`. The runner and that build's own `rcore_probe` ran all 10 `check`
scenarios and both `replay` scenarios. Every artifact of every run is
byte-identical between the two hosts: `summary.txt`, `state_hash.tsv` (every
row), `shot.ppm`, `events.tsv` and `core.log`. Both replay checks pass.

The golden comparison itself reads DIFF for both hosts alike. The default
golden set is newer than that core by 105 commits, so it is a stale-golden
result, not a runner one. Grading against current goldens needs a core built
from the matching n64lle commit.

That comparison caught one runner bug before commit: sign-extended guest
addresses were truncated in `events.tsv`.

Also checked: a manifest with a capability removed, a changed id, an unknown
key, and no manifest at all are each refused with exit 2, naming the field or
line. Under ctest since 2026-10-01 (`runner_manifest_*`,
`tests/manifest_test.cmake`): `fake_future_core`, the plain core declaring
bit 40 with a sidecar naming it `future_feature`, runs headless, over the
link and under `--describe` with the two warnings on stderr; and a sidecar
naming a known capability the library lacks (`deterministic`), or lacking
one it has (`savestate`), is refused.

`--package` (2026-09-26) is checked by ctest only, on `fake_pkg_core` (the
fake core built with `game_package`, sidecar without `[title]`) and
`tests/fake_package.txt`: it runs headless and over the link with the
package's hash logged, and each refusal above exits 2
(`runner_package_*`, `tests/package_test.cmake`). No real generic core has
run through it yet.

`--describe` (2026-09-26) is checked by ctest on both fake cores, whose
options and inputs cover every type and flag, NULL defaults and
descriptions, and a TAB, LF, CR and backslash inside a field. Each case
compares stdout byte for byte (`runner_describe_*`,
`tests/describe_test.cmake`); `fake_core` declares `n64.vru`, so its
`accessory` record is in the comparison, and `fake_pkg_core` declares none.

**Accessory data** (2026-10-01) is checked by ctest only, on `fake_core`,
which drains `accessory_poll` at every frame boundary, logs each message
with its frame, echoes it back through `accessory_notify`, drains a second
time to check the sequence repeats, and logs the bound seat's `connected`
flag (`runner_accessory_*`, `tests/accessory_test.cmake`): a message named
for frame 3 is seen in frame 3 and no other, headless from a script and over
the link from `retro-core-link-test --accessory-send`; the echoes come back
in order; a `--replay-at` second pass sees the same messages in the same
frames; seat 0 reads `connected=0` in every frame although both the headless
sink and the link test put a controller there (the mask was mutated out once
to confirm both tests fail without it); and `--vru1` on a core without the
type is refused, headless and over the link. The netplay refusal is not
under ctest, because the suite builds without recomp-net. No real core has
run with a VRU through this runner yet: n64lle's `n64.vru` is the hub's and
the core's next step. **Run by hand 2026-09-26** on n64lle's generic
`n64lle_core.so` (0.374.0, sidecar beside it): exit 0, 76 options, 16 enum
values and 16 inputs, every record with its field count.

## Netplay (headless, 2026-09-29)

`runner/net_session.*` binds recomp-net's `rb_driver` once, for every core
(rev 4; the core's slots map onto `RNetRbHost` as CORE_ABI.md §Netplay
tables). Built when CMake is given `-DRETRO_RUNTIME_RECOMP_NET_DIR=<recomp-net
checkout>`; `--version` then says `netplay 1`, otherwise `netplay 0` and every
`--net-*` flag is refused.

```
--net-slot N --net-slots N [--net-occupied MASK] [--net-delay N]
[--net-prediction N] [--net-session ID] --net-epoch UNIX_SECONDS
--net-bind HOST:PORT [--net-peer HOST:PORT | --net-relay HOST:PORT]
[--net-content LINE ...]
```

- **Transport.** Seat 0 hosts: with more than two seats it is recomp-net's
  LAN hub (the host relays every guest's datagrams to the others), with two
  a direct pair. Every other seat dials the host (`--net-peer`). With
  `--net-relay` every peer dials that address instead (the lobby server's
  relay).
- **What reaches the core.** `input_get` answers from the published rows,
  through the core's rev 6 codec (or a generic packing, refused for a core
  whose inputs ride axes a row cannot carry). `wall_clock_us` is the epoch
  plus the frame number at a nominal 60 Hz. `RCORE_INIT_NETPLAY` is set.
- **Identity.** The build fingerprint is the runner's netplay version and the
  core's sha256; the content fingerprint is the seat count, occupancy, epoch,
  the ROM's and package's sha256, every NETPLAY option's value, each Transfer
  Pak's ROM and save sha256, and any `--net-content` line. Each line is logged
  (`runner_netplay: IDENT content ...`). A difference is refused at sim 0.
- **The loop** admits one live or one replayed frame at a time (INCREMENTAL):
  `run_frame` or `run_frame_resim`, then `finish`. After `--frames` live frames
  it asks the driver to drain; either peer's drain ends the match on both.
  `NETPLAY_DONE` reports live and replayed frames, episodes, desyncs, the
  confirmed watermark and `state_hash` at the target frame.
- **Measured** (n64lle's generic core, Pokemon Stadium, separate processes on
  loopback, 600 frames): two seats at delay 0, 6 episodes, identical
  `state_hash` at frame 599, 0 desyncs; a forced-mispredict peer, 198
  episodes, 0 desyncs; three seats with seat 0 relaying, identical on all
  three; a different settled epoch refused (`mod_set_mismatch`).
- **Through the hub link** (`--link` with `--net-*`): the hub grants frames
  with the local player's pad in seat 0 (`LaunchSpec::extra_args` carries the
  flags); each grant is answered by the next LIVE frame, any replays running
  silently first, so the network paces the hub. Savestate requests are
  refused during a match. A match that ends (refused, a player gone) exits 4
  with the reason.
- **Refused:** a `NETPLAY` data accessory (`--vruN`) in a netplay session,
  exit 2 before `init` -- its bytes are not replicated to the peers yet
  (`CORE_ABI.md`, "Accessory data").
- **Not built:** NAT traversal, a spectator seat, and play over a real network.

## Not built yet

- **The savestate envelope** and its refuse-on-mismatch rule.
- **Windows and macOS testing on real machines.** Both build and pass the
  ctest suite in CI, and Windows also passes it under Wine. macOS loads a core
  from a private copy, because it has no `/proc/self/fd`
  (`LINK_TRANSPORTS.md` §7). No one has run a real core on either OS yet.
- **`OWNS_LOOP` cores** (`frame_boundary`). Only `RUN_FRAME` cores are driven.
