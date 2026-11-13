#!/usr/bin/env bash
#
# Builds the PaperCraft OpenFX plugin and runs both C++ test suites:
#   - ofx/papercraft_ofx_test.cc: mock OpenFX host that dlopen()s the built
#     plugin binary (PAPERCRAFT_OFX_PATH);
#   - ofx/papercraft_kernel_test.cc: direct unit tests linked against
#     ofx/papercraft_kernel.cc.
#
# The plugin is built the same way as for release: on macOS through
# Install_PaperCraft_OpenFX_macOS.command --build-only, on Linux with the g++
# flags used by .github/workflows/ci-release.yml. Tests compile against real
# GoogleTest when found (pkg-config gtest_main, or headers plus libgtest in
# /usr, /usr/local, /opt/homebrew), else against the tiny local stand-in in
# ofx/testing/gtest_lite. CI always uses real GoogleTest.
#
# Usage: ofx/run_cpp_tests.sh [--lite] [-h|--help]
#   --lite     Force the gtest_lite stand-in even if GoogleTest is installed.
#   -h|--help  Print usage.
# Environment: CXX overrides the compiler (default clang++ on macOS, g++
# elsewhere).
# Exits 0 when both suites pass, non-zero on a build or test failure.

set -euo pipefail

# Filled in by main() and find_gtest().
REPO_ROOT=""
OFX_DIR=""
BUILD_DIR=""
CXX_BIN=""
GTEST_MODE=""          # "real" or "lite".
GTEST_FLAGS=()         # Extra compile/link flags for real GoogleTest.

#######################################
# Prints usage.
# Outputs:
#   Writes usage to stdout.
#######################################
usage() {
  echo "Usage: $(basename "$0") [--lite] [-h|--help]"
}

#######################################
# Removes the scratch build directory on exit.
# Globals:
#   BUILD_DIR
#######################################
cleanup() {
  if [[ -n "${BUILD_DIR}" ]]; then
    rm -rf "${BUILD_DIR}"
  fi
}

#######################################
# Builds the plugin binary into BUILD_DIR and prints its path.
# Globals:
#   OFX_DIR, BUILD_DIR, CXX_BIN
# Outputs:
#   Writes the binary path to stdout; build logs go to stderr.
# Returns:
#   0 on success, non-zero if the build fails.
#######################################
build_plugin() {
  if [[ "$(uname -s)" == "Darwin" ]]; then
    # Reuse the installer so the bundle gets the exact release flags.
    local bundle="${BUILD_DIR}/bundle/PaperCraftStudioPro.ofx.bundle"
    "${OFX_DIR}/Install_PaperCraft_OpenFX_macOS.command" \
      --build-only "${BUILD_DIR}/bundle" >&2
    echo "${bundle}/Contents/MacOS/PaperCraftStudioPro.ofx"
    return 0
  fi
  # Same flags as the "Build Linux OpenFX binary" CI step.
  "${CXX_BIN}" -std=c++17 -O3 -fPIC -shared -fvisibility=hidden \
    -DPAPERCRAFT_STANDALONE_OFX \
    "-I${OFX_DIR}" "-I${OFX_DIR}/openfx_include" \
    "-Wl,--version-script=${OFX_DIR}/openfx_sym.lds" \
    "${OFX_DIR}/papercraft_kernel.cc" "${OFX_DIR}/papercraft_ofx_plugin.cc" \
    -o "${BUILD_DIR}/PaperCraftStudioPro.ofx" >&2
  chmod 0755 "${BUILD_DIR}/PaperCraftStudioPro.ofx"
  echo "${BUILD_DIR}/PaperCraftStudioPro.ofx"
}

#######################################
# Looks for an installed GoogleTest (headers plus gtest and gtest_main libs).
# Globals:
#   GTEST_MODE, GTEST_FLAGS (set on success)
# Returns:
#   0 if GoogleTest was found, 1 otherwise.
#######################################
find_gtest() {
  if command -v pkg-config >/dev/null 2>&1 &&
     pkg-config --exists gtest_main 2>/dev/null; then
    # Word splitting is intended: pkg-config prints a flag list.
    # shellcheck disable=SC2207
    GTEST_FLAGS=($(pkg-config --cflags --libs gtest_main) -lpthread)
    GTEST_MODE="real"
    return 0
  fi
  local prefix
  for prefix in /usr /usr/local /opt/homebrew; do
    if [[ -f "${prefix}/include/gtest/gtest.h" ]] &&
       ls "${prefix}"/lib*/libgtest_main.* "${prefix}"/lib/*/libgtest_main.* \
         >/dev/null 2>&1; then
      GTEST_FLAGS=("-isystem" "${prefix}/include" "-L${prefix}/lib"
                   -lgtest -lgtest_main -lpthread)
      GTEST_MODE="real"
      return 0
    fi
  done
  return 1
}

#######################################
# Compiles one test binary against the selected GoogleTest flavor.
# Globals:
#   OFX_DIR, CXX_BIN, GTEST_MODE, GTEST_FLAGS
# Arguments:
#   $1: output binary path.
#   $@: remaining arguments are the test sources.
# Outputs:
#   Compiler diagnostics on stderr.
# Returns:
#   0 on success, the compiler's status otherwise.
#######################################
compile_test() {
  local out="$1"
  shift
  local -a cmd=("${CXX_BIN}" -std=c++17 -O2 -DPAPERCRAFT_STANDALONE_OFX)
  if [[ "${GTEST_MODE}" == "lite" ]]; then
    cmd+=("-I${OFX_DIR}/testing/gtest_lite")
  fi
  cmd+=("-I${OFX_DIR}" "-I${OFX_DIR}/openfx_include" "$@")
  if [[ "${GTEST_MODE}" == "lite" ]]; then
    cmd+=("${OFX_DIR}/testing/gtest_lite/gtest_lite_main.cc" -lpthread)
  else
    cmd+=("${GTEST_FLAGS[@]}")
  fi
  if [[ "$(uname -s)" != "Darwin" ]]; then
    cmd+=(-ldl)
  fi
  "${cmd[@]}" -o "${out}"
}

#######################################
# Parses flags, builds the plugin, then compiles and runs both suites.
# Arguments:
#   Command-line flags (see file header).
# Outputs:
#   Build and test logs on stdout/stderr.
# Returns:
#   0 when both suites pass; exits 1 on bad flags or any failure.
#######################################
main() {
  local force_lite="false"
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --lite)
        force_lite="true"
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        echo "Unknown option: $1" >&2
        usage >&2
        exit 1
        ;;
    esac
  done

  OFX_DIR="$(cd "$(dirname "$0")" && pwd)"
  REPO_ROOT="$(dirname "${OFX_DIR}")"
  if [[ -n "${CXX:-}" ]]; then
    CXX_BIN="${CXX}"
  elif [[ "$(uname -s)" == "Darwin" ]]; then
    CXX_BIN="clang++"
  else
    CXX_BIN="g++"
  fi
  if ! command -v "${CXX_BIN}" >/dev/null 2>&1; then
    echo "Compiler not found: ${CXX_BIN}" >&2
    exit 1
  fi

  GTEST_MODE="lite"
  if [[ "${force_lite}" != "true" ]] && find_gtest; then
    echo "Using GoogleTest: ${GTEST_FLAGS[*]}"
  else
    echo "Using gtest_lite stand-in (ofx/testing/gtest_lite)."
  fi

  BUILD_DIR="$(mktemp -d)"
  trap cleanup EXIT

  echo "==> Building plugin binary"
  local plugin_path
  plugin_path="$(build_plugin)"
  if [[ ! -f "${plugin_path}" ]]; then
    echo "Plugin binary missing: ${plugin_path}" >&2
    exit 1
  fi

  echo "==> Compiling C++ tests"
  compile_test "${BUILD_DIR}/papercraft_ofx_test" \
    "${OFX_DIR}/papercraft_ofx_test.cc"
  compile_test "${BUILD_DIR}/papercraft_kernel_test" \
    "${OFX_DIR}/papercraft_kernel_test.cc" "${OFX_DIR}/papercraft_kernel.cc"

  local failed=0
  echo "==> Running papercraft_kernel_test"
  "${BUILD_DIR}/papercraft_kernel_test" || failed=1
  echo "==> Running papercraft_ofx_test"
  (cd "${REPO_ROOT}" &&
     PAPERCRAFT_OFX_PATH="${plugin_path}" "${BUILD_DIR}/papercraft_ofx_test") ||
    failed=1

  if [[ "${failed}" -ne 0 ]]; then
    echo "C++ tests FAILED." >&2
    exit 1
  fi
  echo "All C++ tests passed (${GTEST_MODE} GoogleTest)."
}

main "$@"
