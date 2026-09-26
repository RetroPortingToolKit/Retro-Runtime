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
run. It fails the build, rather than warning, when:

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
| `retro-runtime-<version>-<platform>.tar.gz` | The runner, flat at the archive root: `retro-core-runner`, `LICENSE`, `licenses/SDL3.txt`. |
| `….tar.gz.sha256` | That archive's SHA-256. |
| `SHA256SUMS` | Every archive's SHA-256. |
| `runtime-manifest.json` | What a host reads to update. |

Platforms, 2026-09-26:

| Platform | State |
|---|---|
| `linux-x86_64` | Built on Ubuntu 22.04. |
| `linux-arm64` | Built on Ubuntu 22.04 (arm). |
| `windows-x86_64` | **Not available.** The link has no Windows transport (`CORE_LINK.md`). |
| `macos-arm64`, `macos-x86_64` | **Not available.** No macOS transport: macOS has no `memfd` and no `SOCK_SEQPACKET` for `AF_UNIX`. |

The manifest lists the unavailable platforms with that reason, so a host can
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
      "gl": true
    },
    "windows-x86_64": { "unavailable": "the link has no Windows transport yet (docs/CORE_LINK.md)" }
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
```

A build outside a release says `version dev`.

## The update rule, for a host

1. Fetch the manifest. If `schema` is not one the host knows, keep the
   current runner.
2. Take `platforms[<this platform>]`. If it has `unavailable`, show that reason
   and keep the bundled runner.
3. **Refuse** the update unless `link_protocol.major` equals the host's
   `kProtocolMajor`. The minor does not matter, because the session
   negotiates it. The same applies to `rcore_abi.major` against the cores the
   host runs.
4. On Linux, refuse the update if `requires.glibc` is newer than
   `gnu_get_libc_version()`.
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
