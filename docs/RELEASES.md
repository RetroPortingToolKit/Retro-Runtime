# Releases and runner updates

How `retro-core-runner` is released, and how a host (Retro Launcher's
`retro-hub`) finds and installs a newer one without being rebuilt. The runner
can update on its own because the link protocol and the rcore ABI are
versioned: see the README's "Versioning".

## Cutting a release

Run the `release` workflow from the Actions tab (manual only):

| Input | Meaning |
|---|---|
| `version` | Semver without a `v`. Empty: bump the newest `vX.Y.Z` tag on this branch; with no tags, the first release is `project(VERSION)` in `CMakeLists.txt`. |
| `bump` | `patch` / `minor` / `major`, when `version` is empty. |
| `prerelease` | Mark it a prerelease. `releases/latest` skips prereleases, so hosts on the default channel will not see it. |
| `publish` | Off: a dry run that builds, checks and uploads workflow artifacts only. |

Each platform is built by `scripts/package-release.sh`, which a person can also
run. (For a runner to develop against, not to ship, use
`scripts/build-local.sh` / `.ps1` instead: README, "Build locally". It needs no
version, SDL3 prefix or 7z, says `version dev`, and has none of the gates
below.) It fails the build, rather than warning, when:

- the runner cannot lend a GL context (SDL3 missing);
- the runner imports anything but the C library (SDL3, libstdc++ and libgcc
  are linked statically);
- the archive holds a file not on its allowlist;
- the runner **inside the archive**, extracted to a clean directory, reports
  a different version or commit from the release (`--version`);
- that extracted runner fails the fake core, headless or over the link.

The publish job then refuses to write a manifest when the platforms disagree
on version, commit or contracts, or when one is missing, and re-checks every
archive against its recorded hash.

## What a release contains

| Asset | What |
|---|---|
| `retro-runtime-<version>-<platform>.tar.gz` (`.zip` on Windows) | The runner, flat at the archive root: `retro-core-runner` (`.exe`), `LICENSE`, and `licenses/SDL3.txt` where SDL3 is built in. |
| `<archive>.sha256` | That archive's SHA-256. |
| `SHA256SUMS` | Every archive's SHA-256. |
| `runtime-manifest.json` | What a host reads to update. |

Platforms, 2026-09-26:

| Platform | State |
|---|---|
| Platform | Built on | Lends GL | Floor recorded |
|---|---|---|---|
| `linux-x86_64` | Ubuntu 22.04 | 4.3+, where the driver has it | `glibc`, from the binary's symbol versions |
| `linux-arm64` | Ubuntu 22.04 (arm) | 4.3+, where the driver has it | `glibc` |
| `macos-arm64`, `macos-x86_64` | macOS 14, **one universal archive** (`macos-universal`), the x86_64 slice run under Rosetta | **No.** macOS OpenGL stops at 4.1, so a `GL_COMPUTE` core runs its software path. It is built without SDL3 (ruling 2, `LINK_TRANSPORTS.md` §11) | `macos`, from `LC_BUILD_VERSION` (11.0) |
| `windows-x86_64` | Windows Server 2022, MSVC, static CRT | 4.3+, where the driver has it | `windows: "10"`, the tested floor rather than a measured one |
| `windows-arm64` | — | — | Listed as unavailable: not built yet |

A platform that cannot be served is listed with the reason, so a host can
report it instead of finding the platform silently missing.

## The manifest

The newest non-prerelease manifest is always at:

```
https://github.com/RetroPortingToolKit/Retro-Runtime/releases/latest/download/runtime-manifest.json
```

and a specific release's manifest is at `…/releases/download/v<version>/runtime-manifest.json`.

```json
{
  "schema": 1,
  "name": "retro-runtime",
  "version": "0.1.0",
  "tag": "v0.1.0",
  "commit": "<40-hex>",
  "published_utc": "2026-09-26T04:01:18Z",
  "link_protocol": { "major": 1, "minor": 0 },
  "rcore_abi": { "major": 0, "draft_revision": 5 },
  "platforms": {
    "linux-x86_64": {
      "url": "https://github.com/…/releases/download/v0.1.0/retro-runtime-0.1.0-linux-x86_64.tar.gz",
      "archive": "retro-runtime-0.1.0-linux-x86_64.tar.gz",
      "sha256": "<64-hex>",
      "size": 2449824,
      "executable": "retro-core-runner",
      "files": ["retro-core-runner", "LICENSE", "licenses/SDL3.txt"],
      "requires": { "glibc": "<newest GLIBC_ symbol version it imports>" },
                                        // or "macos": "11.0", or "windows": "10"
      "gl": true
    },
    "macos-arm64":   { "url": ".../retro-runtime-0.1.0-macos-universal.tar.gz", "gl": false, ... },
    "macos-x86_64":  { "url": ".../retro-runtime-0.1.0-macos-universal.tar.gz", "gl": false, ... },
    "windows-arm64": { "unavailable": "not built yet (docs/LINK_TRANSPORTS.md §8)" }
  }
}
```

`schema` changes only on an incompatible edit to this format. Fields may be
added, so a reader ignores fields it does not know. Every contract value was
read out of the built runner (`--version`), not typed into the workflow.

## `retro-core-runner --version`

One `key value` per line, exit 0:

```
retro-core-runner 0.1.0
version 0.1.0
commit 8afeb0648adb382df9eec21099a105668f33f3c3
link_protocol 1.0
rcore_abi_major 0
rcore_draft_revision 5
gl 1
game_package 1
describe 1
transfer_pak_seats 4
```

A build outside a release says `version dev`. `game_package 1` (2026-09-26)
means the runner takes `--package` for a generic core (`CORE_RUNNER.md`); a
runner from before it prints no such line, and `probe_runner` reports 0. It
is not yet a field of `runtime-manifest.json`; a host checks it on the
extracted binary (step 7).

`describe 1` (2026-09-26) means the runner answers `--describe` with format
version 1 (`CORE_RUNNER.md`, "--describe"); a runner from before it prints
no such line, and `probe_runner` reports 0. Like `game_package`, it is read
from the binary, not the manifest.

## The update rule, for a host

1. Fetch the manifest. If `schema` is not one the host knows, keep the
   current runner.
2. Take `platforms[<this platform>]`. If it has `unavailable`, show that reason
   and keep the bundled runner.
3. **Refuse** the update unless `link_protocol.major` equals the host's
   `kProtocolMajor`. The minor does not matter, because the session
   negotiates it. The same applies to `rcore_abi.major` against the cores the
   host runs.
4. Refuse the update if the machine is below `requires`: on Linux,
   `requires.glibc` against `gnu_get_libc_version()`; on macOS,
   `requires.macos` against the OS version.
5. Compare `version` with the installed runner's `--version`. Update only if
   it is newer (the launcher's `release_tag_cmp` orders these).
6. Download `url` and check `size` and `sha256` **before** extracting.
7. Extract into a fresh directory, run `<dir>/retro-core-runner --version`,
   and require its `version` and `commit` to match the manifest. Only then
   swap it in with a rename, keeping the previous runner for rollback.
8. Install into a writable data directory, e.g.
   `<data_dir>/runtime/<version>/`, and not beside the host executable, which
   is read-only inside an AppImage or a system install. When starting a core,
   prefer the newest installed runner whose link major matches, else the
   bundled one.
9. Never replace a runner while a session it started is running.
