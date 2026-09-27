#!/usr/bin/env bash
# Build retro-core-runner for THIS machine, for local development: the runner a
# developer hands a port or app as an explicit runner (--runner,
# RETRO_CORE_RUNNER). Linux and macOS; scripts/build-local.ps1 is Windows.
#
# Not a release. scripts/package-release.sh is the release path and its gates
# (pinned static SDL3, import allowlist, universal Mac binary, archive checks)
# do not apply here. What is kept from it, where it makes sense locally: Release
# by default, a static C++ runtime on Linux, the version stamped as "dev" and
# the commit read from git (with -dirty when tracked files are modified).
#
# SDL3 is optional, found the way CMakeLists.txt finds it (find_package, then
# pkg-config). Without it the runner cannot lend a GL context (no --gl); the
# output says which build you got. A shared SDL3 from outside the system
# library directories is copied beside the runner, which finds it through its
# RUNPATH ($ORIGIN on Linux, @loader_path on macOS).
#
# The runner is checked before its path is printed: its --version must run and
# report "game_package 1". The LAST line on stdout is always
#   RETRO_CORE_RUNNER=<absolute path>
#
# usage: scripts/build-local.sh [--debug] [--out DIR] [--build DIR]
#          [--sdl3-prefix DIR | --no-sdl] [--jobs N] [--test] [--help]
set -euo pipefail

die() { echo "build-local: $*" >&2; exit 1; }
say() { echo "build-local: $*" >&2; }

usage() {
  cat <<'EOF'
usage: scripts/build-local.sh [options]

Build retro-core-runner for this machine (local development, not a release)
and print its path last, as RETRO_CORE_RUNNER=<absolute path>.

  --debug              Debug build (default: Release)
  --out DIR            where the runner is copied
                       (default: <repo>/out/local/<platform>/)
  --build DIR          CMake build directory
                       (default: <repo>/build-local, or build-local-debug)
  --sdl3-prefix DIR    use the SDL3 installed under DIR (it must be found)
  --no-sdl             build without SDL3 (the runner then has no --gl)
  --jobs N             parallel build jobs (default: all CPUs)
  --test               run the test suite (ctest) after building
  --help               this text

<platform> is linux-x86_64, linux-arm64, macos-x86_64 or macos-arm64.
Relative paths are taken against the current directory.
EOF
}

CONFIG=Release OUT="" BUILD="" SDL3_PREFIX="" NO_SDL=0 JOBS="" RUN_TESTS=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug)       CONFIG=Debug; shift ;;
    --out)         [[ $# -ge 2 ]] || die "--out needs a directory"; OUT="$2"; shift 2 ;;
    --build)       [[ $# -ge 2 ]] || die "--build needs a directory"; BUILD="$2"; shift 2 ;;
    --sdl3-prefix) [[ $# -ge 2 ]] || die "--sdl3-prefix needs a directory"; SDL3_PREFIX="$2"; shift 2 ;;
    --no-sdl)      NO_SDL=1; shift ;;
    --jobs)        [[ $# -ge 2 ]] || die "--jobs needs a number"; JOBS="$2"; shift 2 ;;
    --test)        RUN_TESTS=1; shift ;;
    -h|--help)     usage; exit 0 ;;
    *) die "unknown argument '$1' (see --help)" ;;
  esac
done
[[ -z "${SDL3_PREFIX}" || "${NO_SDL}" == 0 ]] || die "--sdl3-prefix and --no-sdl contradict each other"
[[ -z "${JOBS}" || "${JOBS}" =~ ^[1-9][0-9]*$ ]] || die "--jobs: want a positive number, got '${JOBS}'"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$(uname -s)" in
  Linux)  OS=linux ;;
  Darwin) OS=macos ;;
  *) die "$(uname -s): this script builds on Linux and macOS; use scripts/build-local.ps1 on Windows" ;;
esac
case "$(uname -m)" in
  x86_64|amd64)  ARCH=x86_64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  *) die "$(uname -m): no platform name for this architecture" ;;
esac
PLATFORM="${OS}-${ARCH}"

# Paths from the command line are relative to where the script was run.
abspath() { mkdir -p "$1" && (cd "$1" && pwd); }
if [[ -z "${BUILD}" ]]; then
  BUILD="${ROOT}/build-local"
  if [[ "${CONFIG}" == Debug ]]; then BUILD+="-debug"; fi
fi
BUILD="$(abspath "${BUILD}")"
OUT="$(abspath "${OUT:-${ROOT}/out/local/${PLATFORM}}")"
[[ "${OUT}" != "${BUILD}" ]] || die "--out and --build must differ"
if [[ -n "${SDL3_PREFIX}" ]]; then
  [[ -d "${SDL3_PREFIX}" ]] || die "--sdl3-prefix ${SDL3_PREFIX}: no such directory"
  SDL3_PREFIX="$(cd "${SDL3_PREFIX}" && pwd)"
fi
if [[ -z "${JOBS}" ]]; then
  JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
fi

command -v cmake >/dev/null || die "cmake not found"

# ---- version stamp: "dev", and the commit with -dirty if it has changes ---
COMMIT=""
if git -C "${ROOT}" rev-parse --git-dir >/dev/null 2>&1; then
  COMMIT="$(git -C "${ROOT}" rev-parse HEAD)"
  # Tracked files only, as git describe --dirty counts them.
  git -C "${ROOT}" diff --quiet HEAD -- 2>/dev/null || COMMIT+="-dirty"
fi

# ---- configure --------------------------------------------------------------
cmake_args=(
  -S "${ROOT}" -B "${BUILD}"
  "-DCMAKE_BUILD_TYPE=${CONFIG}"
  "-DRETRO_RUNTIME_VERSION=dev"
  "-DRETRO_RUNTIME_COMMIT=${COMMIT}"   # empty: CMake reads git itself, else "unknown"
)
if command -v ninja >/dev/null; then cmake_args+=(-G Ninja); fi
case "${OS}" in
  # As the release does: no libstdc++/libgcc_s to find at run time.
  linux) cmake_args+=("-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc"
                      "-DCMAKE_INSTALL_RPATH=\$ORIGIN") ;;
  macos) cmake_args+=("-DCMAKE_INSTALL_RPATH=@loader_path") ;;
esac
if [[ "${NO_SDL}" == 1 ]]; then
  cmake_args+=(-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON)
else
  cmake_args+=(-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=OFF -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=OFF)
  if [[ -n "${SDL3_PREFIX}" ]]; then
    cmake_args+=("-DCMAKE_PREFIX_PATH=${SDL3_PREFIX}")
    SDL3_CONFIG="$(find "${SDL3_PREFIX}" -name SDL3Config.cmake -print -quit)"
    [[ -z "${SDL3_CONFIG}" ]] || cmake_args+=("-DSDL3_DIR=$(dirname "${SDL3_CONFIG}")")
  fi
fi

# A build directory configured with other SDL choices, or another generator,
# keeps them in its cache: start it afresh when the choices change.
STAMP="${BUILD}/build-local.args"
if [[ -f "${STAMP}" && "$(cat "${STAMP}")" != "$(printf '%s\n' "${cmake_args[@]}")" ]]; then
  say "configuration changed since the last build in ${BUILD}; reconfiguring from scratch"
  rm -rf "${BUILD}/CMakeCache.txt" "${BUILD}/CMakeFiles"
fi

say "${PLATFORM}, ${CONFIG}, build ${BUILD}"
cmake "${cmake_args[@]}" >&2
printf '%s\n' "${cmake_args[@]}" > "${STAMP}"
cmake --build "${BUILD}" --parallel "${JOBS}" >&2

if [[ "${RUN_TESTS}" == 1 ]]; then
  ctest --test-dir "${BUILD}" --output-on-failure --parallel "${JOBS}" >&2
fi

# ---- install into a stage (sets the RUNPATH), then copy flat into --out -----
STAGE="${BUILD}/build-local-stage"
rm -rf "${STAGE}"
cmake --install "${BUILD}" --prefix "${STAGE}" >/dev/null
RUNNER="${OUT}/retro-core-runner"
cp "${STAGE}/bin/retro-core-runner" "${RUNNER}"
chmod 0755 "${RUNNER}"
cp "${ROOT}/LICENSE" "${OUT}/LICENSE"

# ---- SDL3: none, static, system, or shared and copied beside the runner -----
# The build-tree binary still carries the build RPATH, so its SDL3 resolves to
# the library it was linked against.
BUILT="${BUILD}/retro-core-runner"
SDL_NOTE=""
sdl_lib=""
rm -f "${OUT}"/libSDL3.so* "${OUT}"/libSDL3*.dylib   # from an earlier build
case "${OS}" in
  linux)
    sdl_lib="$(ldd "${BUILT}" 2>/dev/null | awk '$1 ~ /^libSDL3\.so/ {print $1 " " $3; exit}')"
    if [[ -n "${sdl_lib}" ]]; then
      soname="${sdl_lib%% *}" path="${sdl_lib#* }"
      [[ -f "${path}" ]] || die "retro-core-runner needs ${soname}, which does not resolve"
      case "${path}" in
        /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*)
          SDL_NOTE="shared, from the system (${path})" ;;
        *)
          cp -L "${path}" "${OUT}/${soname}"
          SDL_NOTE="shared, copied beside the runner (${soname}, from ${path})" ;;
      esac
    fi
    ;;
  macos)
    sdl_lib="$(otool -L "${BUILT}" | awk '/libSDL3/ {print $1; exit}')"
    if [[ -n "${sdl_lib}" ]]; then
      name="$(basename "${sdl_lib}")" path=""
      if [[ "${sdl_lib}" == @rpath/* ]]; then
        while read -r dir; do
          if [[ -f "${dir}/${sdl_lib#@rpath/}" ]]; then path="${dir}/${sdl_lib#@rpath/}"; break; fi
        done < <(otool -l "${BUILT}" | awk '/LC_RPATH/ {f=1} f && $1 == "path" {print $2; f=0}')
      else
        path="${sdl_lib}"
      fi
      [[ -n "${path}" && -f "${path}" ]] || die "retro-core-runner needs ${sdl_lib}, which does not resolve"
      case "${path}" in
        /usr/lib/*|/System/*|/opt/homebrew/*|/usr/local/*|/opt/local/*)
          SDL_NOTE="shared, from the system or a package manager (${path})" ;;
        *)
          cp -L "${path}" "${OUT}/${name}"
          if [[ "${sdl_lib}" != @rpath/* ]]; then
            install_name_tool -change "${sdl_lib}" "@rpath/${name}" "${RUNNER}"
          fi
          SDL_NOTE="shared, copied beside the runner (${name}, from ${path})" ;;
      esac
    fi
    # An edited Mach-O must be signed again (ad hoc) or arm64 refuses to run it.
    codesign --force --sign - "${RUNNER}" >/dev/null 2>&1 || true
    ;;
esac

# ---- check the copy, then print where it is ---------------------------------
report="$("${RUNNER}" --version)" || die "${RUNNER} --version failed"
field() { sed -n "s/^$1 //p" <<<"${report}"; }
grep -qx 'game_package 1' <<<"${report}" ||
  die "${RUNNER} --version has no 'game_package 1' line"
[[ "$(field version)" == dev ]] || die "${RUNNER} says version '$(field version)', expected dev"
[[ -z "${COMMIT}" || "$(field commit)" == "${COMMIT}" ]] ||
  die "${RUNNER} says commit '$(field commit)', expected ${COMMIT}"
if [[ "$(field gl)" == 1 ]]; then
  SDL_NOTE="SDL3 ${SDL_NOTE:-linked statically}; --gl available"
else
  [[ -z "${SDL3_PREFIX}" ]] || die "--sdl3-prefix ${SDL3_PREFIX} was given, but SDL3 was not found there"
  SDL_NOTE="no SDL3; --gl (lending a GL context) unavailable"
fi

echo "build-local: ${PLATFORM} ${CONFIG} runner in ${OUT}"
echo "build-local: ${SDL_NOTE}"
echo "${report}"
echo "RETRO_CORE_RUNNER=${RUNNER}"
