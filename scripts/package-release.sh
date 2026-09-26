#!/usr/bin/env bash
# Build, check and package one platform's retro-core-runner for a release.
#
# The single source of the release steps: .github/workflows/release.yml calls
# this, and so can a person. Every check below is a gate -- it fails the
# package rather than warning (recomp-ai-rules/SHIPPING.md §2-4):
#
#   - production configuration: Release, and --gl available (SDL3 required);
#   - libstdc++/libgcc and SDL3 linked statically, and the binary may import
#     only the C library (asserted from its NEEDED entries);
#   - the archive holds exactly the allowlisted files;
#   - the version and commit are read back out of the runner INSIDE the
#     archive, extracted to a clean directory, and must match this release;
#   - that extracted runner then runs the fake core, headless and over the link.
#
# Output in --out:
#   retro-runtime-<version>-<platform>.tar.gz   the runner, flat at the root
#   retro-runtime-<version>-<platform>.tar.gz.sha256
#   retro-runtime-<version>-<platform>.json     this platform's manifest entry,
#                                               merged by release_manifest.py
#
# usage: scripts/package-release.sh --version 0.1.0 --platform linux-x86_64 \
#          --sdl3-prefix DIR [--commit SHA] [--build DIR] [--out DIR]
set -euo pipefail

die() { echo "package-release: $*" >&2; exit 1; }

VERSION="" PLATFORM="" SDL3_PREFIX="" COMMIT="" BUILD="" OUT="dist"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)     VERSION="$2"; shift 2 ;;
    --platform)    PLATFORM="$2"; shift 2 ;;
    --sdl3-prefix) SDL3_PREFIX="$2"; shift 2 ;;
    --commit)      COMMIT="$2"; shift 2 ;;
    --build)       BUILD="$2"; shift 2 ;;
    --out)         OUT="$2"; shift 2 ;;
    *) die "unknown argument $1" ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -n "${VERSION}" ]] || die "--version is required"
[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.+-][A-Za-z0-9.+-]*)?$ ]] ||
  die "--version ${VERSION}: expected semver without a leading v"
case "${PLATFORM}" in
  linux-x86_64|linux-arm64) ;;
  windows-*|macos-*)
    die "${PLATFORM}: retro-core-runner does not build there yet -- the link has no" \
        "transport but Linux's (memfd, SOCK_SEQPACKET, SCM_RIGHTS; docs/CORE_LINK.md)" ;;
  *) die "--platform: want linux-x86_64 or linux-arm64, got '${PLATFORM}'" ;;
esac
[[ -n "${COMMIT}" ]] || COMMIT="$(git -C "${ROOT}" rev-parse HEAD)"
[[ -n "${BUILD}" ]] || BUILD="${ROOT}/build-release-${PLATFORM}"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

NAME="retro-runtime-${VERSION}-${PLATFORM}"
ARCHIVE="${OUT}/${NAME}.tar.gz"

# ---- build: pinned SDL3, quoted definitions, static C++ runtime ----------
# SDL3 comes from exactly --sdl3-prefix: its config directory is named, and the
# pkg-config fallback is off, so a system SDL3 cannot stand in for the pinned
# one (SHIPPING.md §3). A shared one slipping in anyway fails the NEEDED gate.
[[ -n "${SDL3_PREFIX}" ]] || die "--sdl3-prefix is required (a static SDL3 install)"
SDL3_PREFIX="$(cd "${SDL3_PREFIX}" && pwd)"
SDL3_CONFIG_DIR="$(dirname "$(find "${SDL3_PREFIX}" -name SDL3Config.cmake -print -quit)")"
[[ -f "${SDL3_CONFIG_DIR}/SDL3Config.cmake" ]] || die "no SDL3Config.cmake under ${SDL3_PREFIX}"
cmake_args=(
  -S "${ROOT}" -B "${BUILD}" -G Ninja
  "-DCMAKE_BUILD_TYPE=Release"
  "-DRETRO_RUNTIME_VERSION=${VERSION}"
  "-DRETRO_RUNTIME_COMMIT=${COMMIT}"
  "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc"
  "-DSDL3_DIR=${SDL3_CONFIG_DIR}"
  "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON"
  "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"
)
rm -rf "${BUILD}"
cmake "${cmake_args[@]}"
cmake --build "${BUILD}"
ctest --test-dir "${BUILD}" --output-on-failure

RUNNER="${BUILD}/retro-core-runner"
[[ -x "${RUNNER}" ]] || die "${RUNNER}: not built"

# ---- the binary may import only the C library ----------------------------
allowed_needed='^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|ld-linux-(x86-64|aarch64)\.so\.[0-9]+)$'
bad=""
while read -r lib; do
  [[ "${lib}" =~ ${allowed_needed} ]] || bad+=" ${lib}"
done < <(readelf -d "${RUNNER}" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
[[ -z "${bad}" ]] || die "retro-core-runner imports more than the C library:${bad}" \
                         "(SDL3 and libstdc++ must be linked statically)"
# The newest glibc symbol version it needs is the oldest distro it runs on.
GLIBC_MIN="$(objdump -T "${RUNNER}" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' |
             sort -t. -k1,1n -k2,2n -k3,3n -u | tail -1)"
[[ -n "${GLIBC_MIN}" ]] || die "could not read the glibc symbol versions"

# ---- stage exactly the allowlisted files ---------------------------------
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}" "${CLEAN:-}"' EXIT
install -m 0755 "${RUNNER}" "${STAGE}/retro-core-runner"
install -m 0644 "${ROOT}/LICENSE" "${STAGE}/LICENSE"
mkdir -p "${STAGE}/licenses"
[[ -f "${SDL3_PREFIX}/share/licenses/SDL3/LICENSE.txt" ]] ||
  die "SDL3's LICENSE.txt not found under ${SDL3_PREFIX}"
install -m 0644 "${SDL3_PREFIX}/share/licenses/SDL3/LICENSE.txt" "${STAGE}/licenses/SDL3.txt"

expected_files=$'LICENSE\nlicenses/SDL3.txt\nretro-core-runner'
actual_files="$(cd "${STAGE}" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)"
[[ "${actual_files}" == "${expected_files}" ]] ||
  die "archive contents differ from the allowlist:"$'\n'"${actual_files}"

# Flat at the root, so a host extracts it straight into its runtime directory.
tar -C "${STAGE}" --owner=0 --group=0 --numeric-owner -czf "${ARCHIVE}" \
  retro-core-runner LICENSE licenses

# ---- read the claims back out of the archive, in a clean directory -------
CLEAN="$(mktemp -d)"
tar -C "${CLEAN}" -xzf "${ARCHIVE}"
report="$("${CLEAN}/retro-core-runner" --version)"
field() { sed -n "s/^$1 //p" <<<"${report}"; }
[[ "$(field version)" == "${VERSION}" ]] ||
  die "the archived runner says version '$(field version)', this release is ${VERSION}"
[[ "$(field commit)" == "${COMMIT}" ]] ||
  die "the archived runner says commit '$(field commit)', this release is ${COMMIT}"
[[ "$(field gl)" == "1" ]] ||
  die "the archived runner cannot lend a GL context (built without SDL3)"

# ---- and run it: headless, then over the link as a host drives it --------
FAKE="${BUILD}/fake_core.so"
"${CLEAN}/retro-core-runner" --core "${FAKE}" --rom "${ROOT}/LICENSE" --frames 10 \
  --out "${CLEAN}/smoke-headless" | grep -q "frame rate 50/1" ||
  die "the archived runner failed the headless fake-core run"
"${BUILD}/retro-core-link-test" --runner "${CLEAN}/retro-core-runner" --core "${FAKE}" \
  --rom "${ROOT}/LICENSE" --frames 10 --out "${CLEAN}/smoke-link" |
  grep -q "10 frame(s) granted and done, runner exit 0" ||
  die "the archived runner failed the linked fake-core run"

# ---- record what shipped --------------------------------------------------
SHA256="$(sha256sum "${ARCHIVE}" | cut -d' ' -f1)"
echo "${SHA256}  ${NAME}.tar.gz" > "${ARCHIVE}.sha256"
SIZE="$(stat -c %s "${ARCHIVE}")"
proto="$(field link_protocol)"
cat > "${OUT}/${NAME}.json" <<EOF
{
  "platform": "${PLATFORM}",
  "version": "${VERSION}",
  "commit": "${COMMIT}",
  "archive": "${NAME}.tar.gz",
  "sha256": "${SHA256}",
  "size": ${SIZE},
  "executable": "retro-core-runner",
  "files": ["retro-core-runner", "LICENSE", "licenses/SDL3.txt"],
  "requires": {"glibc": "${GLIBC_MIN}"},
  "link_protocol": {"major": ${proto%%.*}, "minor": ${proto#*.}},
  "rcore_abi": {"major": $(field rcore_abi_major), "draft_revision": $(field rcore_draft_revision)},
  "gl": true
}
EOF
echo "package-release: ${ARCHIVE}"
echo "  sha256 ${SHA256}, ${SIZE} bytes, glibc >= ${GLIBC_MIN}, link ${proto}"
