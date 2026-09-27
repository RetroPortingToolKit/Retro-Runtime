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
   `struct_size` handle an older core.

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
   --input-script --list-options`).
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

`--version` prints the release version, commit, link protocol and rcore ABI
compiled in, whether `--gl` is available, and `game_package 1` (this runner
takes `--package`), and exits 0 (`RELEASES.md`).

Exit codes:

| Code | Meaning |
|---|---|
| 0 | ok |
| 1 | a frame failed, or the core reported a FAULT |
| 2 | refused before the core ran: arguments, load, ABI, manifest or game package |
| 3 | the core broke the contract: an undeclared option key |

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
line.

`--package` (2026-09-26) is checked by ctest only, on `fake_pkg_core` (the
fake core built with `game_package`, sidecar without `[title]`) and
`tests/fake_package.txt`: it runs headless and over the link with the
package's hash logged, and each refusal above exits 2
(`runner_package_*`, `tests/package_test.cmake`). No real generic core has
run through it yet.

## Not built yet

- **Netplay** (rev 4: the runner binds recomp-net's `rb_driver`).
- **The savestate envelope** and its refuse-on-mismatch rule.
- **Windows and macOS testing on real machines.** Both build and pass the
  ctest suite in CI, and Windows also passes it under Wine. macOS loads a core
  from a private copy, because it has no `/proc/self/fd`
  (`LINK_TRANSPORTS.md` §7). No one has run a real core on either OS yet.
- **`OWNS_LOOP` cores** (`frame_boundary`). Only `RUN_FRAME` cores are driven.
