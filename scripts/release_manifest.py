#!/usr/bin/env python3
"""Merge each platform's package entry into runtime-manifest.json.

The manifest is what a host (Retro Launcher) fetches to update its runner; see
docs/RELEASES.md for the format and the update rule. It is published as a
release asset, so the newest one is always at

    https://github.com/<repo>/releases/latest/download/runtime-manifest.json

Every entry was written by scripts/package-release.sh from the runner inside
its own archive. This refuses to write a manifest when they disagree about the
version, the commit or the contracts, or when an expected platform is missing:
one release is one runtime, whatever it was built on.

usage: release_manifest.py --version 0.1.0 --tag v0.1.0 --repo OWNER/NAME \
           --expect linux-x86_64,linux-arm64 --out DIR ENTRY.json...
"""

import argparse
import datetime
import json
import pathlib
import sys

SCHEMA = 1

# Platforms a host may ask for that this release cannot serve, and why. A host
# reads the reason instead of finding the platform silently absent.
UNAVAILABLE = {
    "windows-x86_64": "the link has no Windows transport yet (docs/CORE_LINK.md)",
    "macos-arm64": "the link has no macOS transport yet (docs/CORE_LINK.md)",
    "macos-x86_64": "the link has no macOS transport yet (docs/CORE_LINK.md)",
}

# Fields every platform of one release must agree on.
SHARED = ("version", "commit", "link_protocol", "rcore_abi")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--expect", required=True, help="comma-separated platforms")
    ap.add_argument("--out", required=True)
    ap.add_argument("entries", nargs="+")
    a = ap.parse_args()

    def refuse(msg: str) -> int:
        print(f"release_manifest: {msg}", file=sys.stderr)
        return 1

    entries = [json.loads(pathlib.Path(p).read_text()) for p in a.entries]
    by_platform = {}
    for e in entries:
        if e["platform"] in by_platform:
            return refuse(f"two entries for {e['platform']}")
        by_platform[e["platform"]] = e

    expected = {p for p in a.expect.split(",") if p}
    if set(by_platform) != expected:
        return refuse(f"platforms {sorted(by_platform)} but expected {sorted(expected)}")
    if e_ver := [p for p, e in by_platform.items() if e["version"] != a.version]:
        return refuse(f"{e_ver} are not version {a.version}")
    first = entries[0]
    for e in entries[1:]:
        for k in SHARED:
            if e[k] != first[k]:
                return refuse(f"{k}: {first['platform']} has {first[k]!r}, "
                              f"{e['platform']} has {e[k]!r}")

    base = f"https://github.com/{a.repo}/releases/download/{a.tag}"
    platforms = {}
    for name in sorted(by_platform):
        e = by_platform[name]
        platforms[name] = {
            "url": f"{base}/{e['archive']}",
            "archive": e["archive"],
            "sha256": e["sha256"],
            "size": e["size"],
            "executable": e["executable"],
            "files": e["files"],
            "requires": e["requires"],
            "gl": e["gl"],
        }
    for name, why in UNAVAILABLE.items():
        if name not in platforms:
            platforms[name] = {"unavailable": why}

    manifest = {
        "schema": SCHEMA,
        "name": "retro-runtime",
        "version": a.version,
        "tag": a.tag,
        "commit": first["commit"],
        "published_utc": datetime.datetime.now(datetime.timezone.utc)
                                  .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "link_protocol": first["link_protocol"],
        "rcore_abi": first["rcore_abi"],
        "platforms": platforms,
    }

    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "runtime-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    sums = "".join(f"{by_platform[p]['sha256']}  {by_platform[p]['archive']}\n"
                   for p in sorted(by_platform))
    (out / "SHA256SUMS").write_text(sums)
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
