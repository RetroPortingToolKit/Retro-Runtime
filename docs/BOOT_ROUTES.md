# Input replays

Input replays record a play session from a cold boot or a restored mid-game
anchor and replay it in a fresh runner process. `--version` reports `boot_routes 1` in a runner with this feature.
No core ABI change is required.

The recorder runs in `HostSession`, on the same input callbacks that headless
and link sessions use. It records the order of actual `input_get` calls, all
eight seats, connection flags, buttons and six signed axes. It also records
host clock calls. Playback supplies those values and refuses a changed
callback sequence. It does not sample desktop input on a timer.

Each completed field has a state checkpoint, the last CPU picture hash and a
hash of the delivered PCM samples. Cores without public state hashes have
picture/audio checks only. A mark also writes a PPM picture. GPU-only
presentation has no CPU picture to hash. Use software rendering for a route
that requires exact pictures across these two runner modes.

The file contains the initial save regions and these identities:

- The ROM, core, runner, game package and title `game.toml`.
- The core id and version, strict mode and every resolved option.
- The process's `N64LLE_` environment variables.

The initial saves are restored after load and before the first field.
Playback does not persist them to the recording's original save files.
Routes contain private player data. Keep them with private game inputs.

## Record and replay

`--record-route <new-file>` records headless input. `--route-mark FIELD:NAME`
adds a named checkpoint picture. The existing `--input-script` can supply
buttons and stick directions for scripted recording. A route file is never
overwritten.

For ordinary play through a hub, set `RETRO_RECORD_ROUTE` to a new absolute
route path and select this runner through the hub's `--runner` option. The
runner records the pads that the hub grants. Quit the game normally to seal
the movie. No launcher change is required. The headless link test exercises
the same path without a window.

`--play-route <file>` runs the movie headlessly or over the hub link. Supply the same ROM,
package, title directory and resolved configuration. Playback ignores
`--frames` and runs the movie's field count. It refuses simultaneous input
scripts, netplay, loaded states and accessories. Accessories need
their own input/state contract before they can join this format.

A compatible frontend uses link 2.5 `ReplayControl` to start or stop recording
between frames. Start captures and restores a core snapshot before recording
inputs. The optional `A` record stores that anchor and its public digest;
boot movies remain readable. Fresh playback restores initial saves and the
anchor before the first field, and checks its digest immediately. A core must
serialize all replay-relevant state for anchored playback to work. A core that
only supports same-process restores is insufficient; playback refuses or
reports divergence instead of weakening checkpoints. Anchors are limited to
64 MiB and the entire movie to 256 MiB.

`ReplayStatus` confirms Recording, Saved, Playing, TakenOver or Failed. Hosts
must wait for the response before granting another frame and display recording
status only after confirmation. Controls are unavailable with older peers,
netplay, accessories, or an active playback. Existing-file destinations refuse.
The frontend can list sealed `.route` files and start playback in a fresh runner
using `RETRO_PLAY_ROUTE`. `RETRO_ROUTE_VERDICT` selects the result path.
The hub supplies presentation and pacing. Normal completion ends the runner;
Quit writes `stopped`. Playback uses private save memory, leaving the frontend's
persistent save mappings alone, including after takeover.

Watch enables `--route-takeover` through `RETRO_ROUTE_TAKEOVER=1`. First neutral
live pads arm takeover. A later button or stick movement ends replay input
authority at the next field, without a core reset. The verdict is `taken_over`
and records that field. Checks stop there. The live grants then drive the game
normally. The frontend's mapped input/deadzone is retained, with its raw-axis
fallback of 8000 as a residual-noise guard for takeover detection only.
Headless replay has no takeover path.

## Results and core updates

`<out>/route-verdict.json` records `recorded`, `in_sync`, `diverged`, `failed`,
`stopped` or `taken_over`.
`--route-verdict <path>` selects another result file. Exit 5 means that a
checkpoint or callback sequence diverged. Exit 2 means that admission failed.
The first differing field is recorded. A picture mismatch also writes the
actual CPU picture. An identical final picture alone is not a passing replay.

Default playback requires the exact recorded core and runner binaries.
The Watch extension can use `--route-runner-reference` (or
`RETRO_ROUTE_RUNNER_REFERENCE`) to name the preserved recording runner. Its
hash must match the movie, and the verdict names both recorded and current
runner hashes. Other identities and all checked checkpoints remain exact.
`--route-allow-core-change` explicitly compares another core. The ROM, package,
runner, existing options and expected checkpoints remain unchanged. A new
option is accepted only at the new core's declared default, and the verdict
lists it under `added_default_options`. A removed option or a changed
recorded option still refuses. A passing comparison describes that route and
configuration, not every game or future core version.

## Interrupted recording

The active file is `<route>.partial`. The writer flushes each complete field.
A normal link Quit or headless Ctrl+C seals completed fields in the requested
route file. A fault leaves the partial file. An unsealed partial file refuses
normal playback.

`scripts/route_recover.py` copies the complete field prefix into a new sealed
file. It does not overwrite the source or an existing output. The normal
runner still validates the recovered file before execution. For example:

```powershell
& python .\scripts\route_recover.py 'C:\private\route.partial' 'C:\private\recovered.route'
```

The format limit is 256 MiB. Recording and parsing have bounded callback,
image and audio sizes. The checksum detects accidental file damage. It is
not a signature that authenticates another person's route.

## Validation

`route_input_roundtrip` checks all seats and axes, clocks, initial-save
restoration, state/picture/audio replay and explicit new-default handling.
`runner_routes` checks new-process playback, link-to-headless playback,
identity refusal, truncation, malformed field order and first divergence.
`tests/route_recover_test.py` checks complete-prefix recovery and refusals
without game data. The existing Runtime suite remains applicable.

`route_anchor` checks recording controls, a restored mid-game anchor,
fresh-process playback and protection of an existing movie. Runtime tests use
synthetic cores; title/core-specific file-state replay must be qualified
separately. They do not establish visual UI or physical-controller acceptance.
