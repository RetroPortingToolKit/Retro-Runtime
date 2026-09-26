# Retro-Runtime

The pieces shared by every host that runs an **rcore core**: the contract, the
link between a host and the process that runs a core, and that process itself.
Retro Launcher (`retro-hub`) consumes this repository. A standalone release of
a title is `retro-hub` in Direct mode, plus `retro-core-runner`, plus the
title's core.

| Path | What |
|---|---|
| `include/rcore/rcore.h` | The host ↔ core contract. A core is a shared library exporting one symbol, `rcore_entry`. Draft revision 5, `RCORE_ABI_MAJOR 0`. |
| `corelink/` | The host ↔ runner link: its protocol, and `retro_corelink`, the client a host embeds. |
| `runner/` | `retro-core-runner`, the child process that loads a core and runs it: headless, or linked to a host. Also `retro_core_support`, the sidecar-manifest reader and core loader hosts use. |
| `tests/rcore_fake_core.c` | The smallest core, for tests. |
| `docs/` | `HOST_LIFECYCLE.md` (the design), `CORE_ABI.md` (the contract), `CORE_LINK.md` (the link), `CORE_RUNNER.md` (the runner), `RELEASES.md` (releases and runner updates). |

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

- SDL3 is optional: the runner uses it only to lend a core a GL context
  (`--gl`).
- The link and the runner are **Linux only** until the link has a Windows
  transport. `retro_rcore` and `retro_core_support` build everywhere.

A host pulls this in with `add_subdirectory()` and links `retro_rcore`,
`retro_core_support` and `retro_corelink`. A host that already found SDL3 can
pass `-DRETRO_RUNTIME_SDL3_TARGET=<target>`.

## Releases

The `release` workflow (manual) publishes `retro-core-runner` per platform,
plus a `runtime-manifest.json` that hosts poll to update the runner:

```
https://github.com/RetroPortingToolKit/Retro-Runtime/releases/latest/download/runtime-manifest.json
```

Linux x86_64 and arm64 today. Windows and macOS are listed as unavailable
until the link has a transport there. See `docs/RELEASES.md` for the gates,
the manifest format, and the rule a host follows to update.
`retro-core-runner --version` reports the version, commit and contracts that
were compiled in.

## Versioning

Three contracts, each with a major that must match and append-only minors:

| Contract | Where | Current |
|---|---|---|
| rcore ABI | `RCORE_ABI_MAJOR` / `RCORE_DRAFT_REVISION` | 0 (draft), revision 5 |
| Link protocol | `kProtocolMajor` / `kProtocolMinor` | 1.0 |
| Sidecar manifest | `abi_major`, `draft_revision` | follows the ABI |

The runner can therefore update separately from hosts and cores:
- A newer runner handles older cores, because the ABI is append-only
  (`struct_size`).
- Any host speaking the same link major works, and the session uses the lower
  of the two minors.

## History

This repository was split out of Retro Launcher on 2026-09-25, with its
history.
