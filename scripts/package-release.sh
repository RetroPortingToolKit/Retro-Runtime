#!/usr/bin/env bash
# Build, check and package one platform's retro-core-runner for a release.
#
# The single source of the release steps: .github/workflows/release.yml calls
# this, and so can a person. Every check below is a gate -- it fails the
# package rather than warning (recomp-ai-rules/SHIPPING.md §2-4):
#
#   - production configuration: Release; on Linux and Windows, --gl available
#     (SDL3 built in). macOS builds without SDL3: its OpenGL stops at 4.1, so
#     it never lends a context (docs/LINK_TRANSPORTS.md §11, ruling 2);
#   - the C++ runtime and SDL3 linked statically, and the binary may import
#     only what the OS itself provides (asserted from its import table);
#   - macOS: one universal binary, both slices present and both run;
#   - the archive holds exactly the allowlisted files;
#   - the version and commit are read back out of the runner INSIDE the
#     archive, extracted to a clean directory, and must match this release;
#   - that extracted runner then runs the fake core, headless and over the link.
#
# Platforms: linux-x86_64, linux-arm64 (on that architecture), macos-universal
# (serves macos-arm64 and macos-x86_64), windows-x86_64 (MSVC environment,
# from Git Bash).
#
# Output in --out:
#   retro-runtime-<version>-<platform>.tar.gz|.zip   the runner, flat at the root
#   <archive>.sha256
#   retro-runtime-<version>-<platform>.json          this build's manifest entry,
#                                                    merged by release_manifest.py
#
# usage: scripts/package-release.sh --version 0.1.0 --platform linux-x86_64 \
#          [--sdl3-prefix DIR] [--commit SHA] [--build DIR] [--out DIR]
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
  linux-x86_64|linux-arm64) OS=linux;   SERVES="\"${PLATFORM}\""; EXE=retro-core-runner ;;
  macos-universal)          OS=macos;   SERVES='"macos-arm64", "macos-x86_64"'; EXE=retro-core-runner ;;
  windows-x86_64)           OS=windows; SERVES="\"${PLATFORM}\""; EXE=retro-core-runner.exe ;;
  *) die "--platform: want linux-x86_64, linux-arm64, macos-universal or windows-x86_64," \
         "got '${PLATFORM}'" ;;
esac
[[ -n "${COMMIT}" ]] || COMMIT="$(git -C "${ROOT}" rev-parse HEAD)"
[[ -n "${BUILD}" ]] || BUILD="${ROOT}/build-release-${PLATFORM}"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

# Paths handed to native Windows tools (cmake, the runner) must be Windows paths.
native() { if [[ "${OS}" == windows ]]; then cygpath -m "$1"; else printf '%s' "$1"; fi; }
sha256_of() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1;
              else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
size_of() { if [[ "${OS}" == macos ]]; then stat -f %z "$1"; else stat -c %s "$1"; fi; }

NAME="retro-runtime-${VERSION}-${PLATFORM}"
if [[ "${OS}" == windows ]]; then ARCHIVE_NAME="${NAME}.zip"; else ARCHIVE_NAME="${NAME}.tar.gz"; fi
ARCHIVE="${OUT}/${ARCHIVE_NAME}"

# ---- build: pinned SDL3, quoted definitions, static runtimes -------------
cmake_args=(
  -S "$(native "${ROOT}")" -B "$(native "${BUILD}")" -G Ninja
  "-DCMAKE_BUILD_TYPE=Release"
  "-DRETRO_RUNTIME_VERSION=${VERSION}"
  "-DRETRO_RUNTIME_COMMIT=${COMMIT}"
  "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON"
  "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"
)
case "${OS}" in
  linux)   cmake_args+=("-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc") ;;
  windows) cmake_args+=("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded") ;; # /MT: no CRT DLLs
  macos)   cmake_args+=("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
                        "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0"
                        "-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON") ;;
esac
# SDL3 comes from exactly --sdl3-prefix: its config directory is named, and the
# pkg-config fallback is off, so a system SDL3 cannot stand in for the pinned
# one (SHIPPING.md §3). A shared one slipping in anyway fails the imports gate.
if [[ "${OS}" != macos ]]; then
  [[ -n "${SDL3_PREFIX}" ]] || die "--sdl3-prefix is required (a static SDL3 install)"
  SDL3_PREFIX="$(cd "${SDL3_PREFIX}" && pwd)"
  SDL3_CONFIG_DIR="$(dirname "$(find "${SDL3_PREFIX}" -name SDL3Config.cmake -print -quit)")"
  [[ -f "${SDL3_CONFIG_DIR}/SDL3Config.cmake" ]] || die "no SDL3Config.cmake under ${SDL3_PREFIX}"
  cmake_args+=("-DSDL3_DIR=$(native "${SDL3_CONFIG_DIR}")")
fi
rm -rf "${BUILD}"
cmake "${cmake_args[@]}"
cmake --build "$(native "${BUILD}")"
ctest --test-dir "$(native "${BUILD}")" --output-on-failure

RUNNER="${BUILD}/${EXE}"
[[ -f "${RUNNER}" ]] || die "${RUNNER}: not built"

# ---- the binary may import only what the OS provides ---------------------
bad=""
case "${OS}" in
  linux)
    allowed='^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|ld-linux-(x86-64|aarch64)\.so\.[0-9]+)$'
    while read -r lib; do
      [[ "${lib}" =~ ${allowed} ]] || bad+=" ${lib}"
    done < <(readelf -d "${RUNNER}" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    # The newest glibc symbol version it needs is the oldest distro it runs on.
    FLOOR="$(objdump -T "${RUNNER}" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' |
             sort -t. -k1,1n -k2,2n -k3,3n -u | tail -1)"
    [[ -n "${FLOOR}" ]] || die "could not read the glibc symbol versions"
    REQUIRES="\"glibc\": \"${FLOOR}\""
    ;;
  macos)
    allowed='^(/usr/lib/libSystem\.B\.dylib|/usr/lib/libc\+\+\.1\.dylib|/System/Library/Frameworks/.*)$'
    while read -r lib; do
      [[ "${lib}" =~ ${allowed} ]] || bad+=" ${lib}"
    # Dependencies are the indented lines; a universal binary also prints one
    # unindented "<path> (architecture ...):" header per slice.
    done < <(otool -L "${RUNNER}" | grep '^[[:space:]]' | awk '{print $1}' | sort -u)
    archs="$(lipo -archs "${RUNNER}")"
    [[ " ${archs} " == *" arm64 "* && " ${archs} " == *" x86_64 "* ]] ||
      die "retro-core-runner is not universal (slices: ${archs})"
    FLOOR="$(otool -l -arch arm64 "${RUNNER}" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"
    [[ -n "${FLOOR}" ]] || die "could not read LC_BUILD_VERSION minos"
    REQUIRES="\"macos\": \"${FLOOR}\""
    ;;
  windows)
    allowed='^(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|imm32|winmm|version|setupapi|cfgmgr32|bcrypt|shlwapi|opengl32|hid|dwmapi|uxtheme|ws2_32|ntdll|userenv|rpcrt4|combase)\.dll$'
    while read -r lib; do
      lower="$(tr '[:upper:]' '[:lower:]' <<<"${lib}")"
      [[ "${lower}" =~ ${allowed} ]] || bad+=" ${lib}"
    done < <(dumpbin -nologo -dependents "$(native "${RUNNER}")" | tr -d '\r' |
             sed -n 's/^ *\([A-Za-z0-9_.-]*\.[dD][lL][lL]\)$/\1/p')
    # The tested floor: CI builds and runs on Windows Server 2022 (Windows 10
    # family). The PE subsystem version is not a real floor, so it is not used.
    REQUIRES='"windows": "10"'
    ;;
esac
[[ -z "${bad}" ]] || die "${EXE} imports more than the OS provides:${bad}" \
                         "(the C++ runtime and SDL3 must be linked statically)"

# ---- stage exactly the allowlisted files ---------------------------------
STAGE="$(mktemp -d)"
CLEAN=""
trap 'rm -rf "${STAGE}" "${CLEAN}"' EXIT
cp "${RUNNER}" "${STAGE}/${EXE}"
chmod 0755 "${STAGE}/${EXE}"
cp "${ROOT}/LICENSE" "${STAGE}/LICENSE"
files=("${EXE}" LICENSE)
if [[ "${OS}" != macos ]]; then
  [[ -f "${SDL3_PREFIX}/share/licenses/SDL3/LICENSE.txt" ]] ||
    die "SDL3's LICENSE.txt not found under ${SDL3_PREFIX}"
  mkdir -p "${STAGE}/licenses"
  cp "${SDL3_PREFIX}/share/licenses/SDL3/LICENSE.txt" "${STAGE}/licenses/SDL3.txt"
  files+=(licenses/SDL3.txt)
fi
expected_files="$(printf '%s\n' "${files[@]}" | LC_ALL=C sort)"
actual_files="$(cd "${STAGE}" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)"
[[ "${actual_files}" == "${expected_files}" ]] ||
  die "archive contents differ from the allowlist:"$'\n'"${actual_files}"

# Flat at the root, so a host extracts it straight into its runtime directory.
rm -f "${ARCHIVE}"
case "${OS}" in
  linux)   tar -C "${STAGE}" --owner=0 --group=0 --numeric-owner -czf "${ARCHIVE}" "${files[@]}" ;;
  macos)   tar -C "${STAGE}" --uid 0 --gid 0 -czf "${ARCHIVE}" "${files[@]}" ;;
  windows) (cd "${STAGE}" && 7z a -tzip -bd "$(native "${ARCHIVE}")" "${files[@]}" >/dev/null) ;;
esac

# ---- read the claims back out of the archive, in a clean directory -------
CLEAN="$(mktemp -d)"
if [[ "${OS}" == windows ]]; then
  7z x -bd "-o$(native "${CLEAN}")" "$(native "${ARCHIVE}")" >/dev/null
else
  tar -C "${CLEAN}" -xzf "${ARCHIVE}"
fi
report="$("${CLEAN}/${EXE}" --version | tr -d '\r')"
field() { sed -n "s/^$1 //p" <<<"${report}"; }
[[ "$(field version)" == "${VERSION}" ]] ||
  die "the archived runner says version '$(field version)', this release is ${VERSION}"
[[ "$(field commit)" == "${COMMIT}" ]] ||
  die "the archived runner says commit '$(field commit)', this release is ${COMMIT}"
if [[ "${OS}" == macos ]]; then
  GL=false
  [[ "$(field gl)" == "0" ]] || die "the macOS runner was meant to build without SDL3"
else
  GL=true
  [[ "$(field gl)" == "1" ]] || die "the archived runner cannot lend a GL context (built without SDL3)"
fi

# ---- and run it: headless, then over the link as a host drives it --------
FAKE="$(find "${BUILD}" -maxdepth 1 -name 'fake_core.*' ! -name '*.toml' -print -quit)"
[[ -n "${FAKE}" ]] || die "the fake core was not built"
LINK_TEST="${BUILD}/retro-core-link-test"
[[ "${OS}" == windows ]] && LINK_TEST+=".exe"
headless() { # [arch prefix...]
  "$@" "${CLEAN}/${EXE}" --core "$(native "${FAKE}")" --rom "$(native "${ROOT}/LICENSE")" \
    --frames 10 --out "$(native "${CLEAN}/smoke-headless")" | grep -q "frame rate 50/1"
}
headless || die "the archived runner failed the headless fake-core run"
if [[ "${OS}" == macos ]]; then
  # Both slices, each loading the (universal) fake core.
  headless arch -arm64 || die "the arm64 slice failed the headless fake-core run"
  headless arch -x86_64 || die "the x86_64 slice failed the headless fake-core run (Rosetta?)"
fi
"${LINK_TEST}" --runner "$(native "${CLEAN}/${EXE}")" --core "$(native "${FAKE}")" \
  --rom "$(native "${ROOT}/LICENSE")" --frames 10 --out "$(native "${CLEAN}/smoke-link")" |
  tr -d '\r' | grep -q "10 frame(s) granted and done, runner exit 0" ||
  die "the archived runner failed the linked fake-core run"

# ---- record what shipped --------------------------------------------------
SHA256="$(sha256_of "${ARCHIVE}")"
echo "${SHA256}  ${ARCHIVE_NAME}" > "${ARCHIVE}.sha256"
SIZE="$(size_of "${ARCHIVE}")"
proto="$(field link_protocol)"
file_list="$(printf '"%s", ' "${files[@]}")"
cat > "${OUT}/${NAME}.json" <<EOF
{
  "platform": "${PLATFORM}",
  "serves": [${SERVES}],
  "version": "${VERSION}",
  "commit": "${COMMIT}",
  "archive": "${ARCHIVE_NAME}",
  "sha256": "${SHA256}",
  "size": ${SIZE},
  "executable": "${EXE}",
  "files": [${file_list%, }],
  "requires": {${REQUIRES}},
  "link_protocol": {"major": ${proto%%.*}, "minor": ${proto#*.}},
  "rcore_abi": {"major": $(field rcore_abi_major), "draft_revision": $(field rcore_draft_revision)},
  "gl": ${GL}
}
EOF
echo "package-release: ${ARCHIVE}"
echo "  sha256 ${SHA256}, ${SIZE} bytes, requires {${REQUIRES}}, link ${proto}"
