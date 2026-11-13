#!/bin/bash
# Compiles, signs, and installs PaperCraftStudioPro.ofx.bundle for macOS.
#
# Builds papercraft_kernel.cc + papercraft_ofx_plugin.cc with clang++ into a
# universal (x86_64 + arm64) OpenFX bundle, ad-hoc signs it, and copies it to
# the OpenFX plugin folder that DaVinci Resolve scans at startup. Double-click
# it in Finder (.command files open in Terminal) or run it from a shell. It
# works both from the repo's ofx/ folder and from a release folder where the
# sources live in OpenFX_Source/.
#
# Usage:
#   Install_PaperCraft_OpenFX_macOS.command [--user] [--dry-run]
#                                           [--build-only DIR]
# Flags:
#   --user            Install to ~/Library/OFX/Plugins instead of the
#                     system-wide /Library/OFX/Plugins (no sudo needed).
#   --dry-run         Print source dir, target dir and compiler flags; build
#                     nothing.
#   --build-only DIR  Build the bundle into DIR instead of an OpenFX folder
#                     (developer builds, e.g. ./dist).
#   -h, --help        Show usage.
# Requirements: Xcode Command Line Tools (clang++, lipo, codesign).

set -euo pipefail

readonly BUNDLE_NAME="PaperCraftStudioPro.ofx.bundle"
readonly BINARY_NAME="PaperCraftStudioPro.ofx"
# Build scratch directory; global so the EXIT trap can remove it.
TMP_DIR=""

#######################################
# Prints the usage line.
# Arguments:
#   None
# Outputs:
#   Writes usage to stdout.
#######################################
usage() {
  echo "Usage: $(basename "$0") [--user] [--dry-run] [--build-only DIR]"
}

#######################################
# Parses flags, builds the bundle and installs it.
# Globals:
#   BUNDLE_NAME
#   BINARY_NAME
#   TMP_DIR (set; removed by the EXIT trap)
#   HOME
# Arguments:
#   Command-line flags (see file header).
# Outputs:
#   Writes progress and the install location to stdout, errors to stderr.
# Returns:
#   0 on success; exits 1 on bad flags, missing clang++ or build failure.
#######################################
main() {
  local mode="system"
  local dry_run="false"
  local build_only_dir=""
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --user)
        mode="user"
        shift
        ;;
      --dry-run)
        dry_run="true"
        shift
        ;;
      --build-only)
        build_only_dir="${2:-}"
        [[ -n "${build_only_dir}" ]] || { usage; exit 1; }
        shift 2
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

  local script_dir
  script_dir="$(cd "$(dirname "$0")" && pwd)"
  # Repo layout: sources next to this script. Release ZIP layout: sources in
  # an OpenFX_Source/ subfolder.
  local src_dir="${script_dir}"
  if [[ -f "${script_dir}/OpenFX_Source/papercraft_ofx_plugin.cc" ]]; then
    src_dir="${script_dir}/OpenFX_Source"
  fi
  local dest_dir="/Library/OFX/Plugins"
  if [[ -n "${build_only_dir}" ]]; then
    dest_dir="${build_only_dir}"
  elif [[ "${mode}" == "user" ]]; then
    dest_dir="${HOME}/Library/OFX/Plugins"
  fi
  if [[ "${dry_run}" == "true" ]]; then
    echo "[dry-run] Source directory: ${src_dir}"
    echo "[dry-run] Target OpenFX directory: ${dest_dir}/${BUNDLE_NAME}"
    echo "[dry-run] Compiler flags: clang++ -std=c++17 -O3 -fPIC -bundle -fvisibility=hidden -DPAPERCRAFT_STANDALONE_OFX -Wl,-exported_symbol,_OfxGetNumberOfPlugins -Wl,-exported_symbol,_OfxGetPlugin"
    exit 0
  fi
  if ! command -v clang++ >/dev/null 2>&1; then
    echo "clang++ not found. Run: xcode-select --install" >&2
    exit 1
  fi

  TMP_DIR="$(mktemp -d)"
  local tmp_dir="${TMP_DIR}"
  trap '[[ -n "${TMP_DIR:-}" ]] && rm -rf "${TMP_DIR}"' EXIT
  # Standard OpenFX macOS bundle layout: Contents/{Info.plist,MacOS,Resources}.
  local bundle_dir="${tmp_dir}/${BUNDLE_NAME}"
  mkdir -p "${bundle_dir}/Contents/MacOS" "${bundle_dir}/Contents/Resources"
  cp "${src_dir}/Info.plist" "${bundle_dir}/Contents/Info.plist"
  cp "${src_dir}/openfx_include/LICENSE" "${bundle_dir}/Contents/Resources/LICENSE"

  # -bundle builds a loadable Mach-O bundle; hidden visibility plus the two
  # -exported_symbol flags export only the OpenFX entry points, so internal
  # C++ symbols cannot clash with Resolve's own libraries.
  local -a common_flags=(
    -std=c++17 -O3 -fPIC -bundle -fvisibility=hidden -mmacosx-version-min=11.0
    -DPAPERCRAFT_STANDALONE_OFX "-I${src_dir}" "-I${src_dir}/openfx_include"
    -Wl,-exported_symbol,_OfxGetNumberOfPlugins -Wl,-exported_symbol,_OfxGetPlugin
  )
  local -a srcs=("${src_dir}/papercraft_kernel.cc" "${src_dir}/papercraft_ofx_plugin.cc")
  local bin_out="${bundle_dir}/Contents/MacOS/${BINARY_NAME}"
  local build_log="${tmp_dir}/universal_build.log"
  # Try a universal binary (Intel + Apple silicon) first. If either slice
  # fails (e.g. an SDK without one architecture), fall back to a native-only
  # build and show the universal build errors only if that also fails.
  if clang++ "${common_flags[@]}" -arch x86_64 "${srcs[@]}" -o "${tmp_dir}/x86_64.ofx" 2>"${build_log}" && \
     clang++ "${common_flags[@]}" -arch arm64 "${srcs[@]}" -o "${tmp_dir}/arm64.ofx" 2>>"${build_log}"; then
    lipo -create -output "${bin_out}" "${tmp_dir}/x86_64.ofx" "${tmp_dir}/arm64.ofx"
  else
    if ! clang++ "${common_flags[@]}" "${srcs[@]}" -o "${bin_out}"; then
      echo "Universal build stderr:" >&2
      cat "${build_log}" >&2
      exit 1
    fi
  fi
  chmod 0755 "${bin_out}"
  # Strip quarantine attributes and ad-hoc sign so Gatekeeper lets Resolve
  # load the bundle. Both steps are best effort.
  command -v xattr >/dev/null 2>&1 && xattr -cr "${bundle_dir}" || true
  command -v codesign >/dev/null 2>&1 && codesign --force --deep --sign - "${bundle_dir}" || true

  # Find the nearest existing ancestor of dest_dir to decide whether the
  # install needs sudo.
  local check_dir="${dest_dir}"
  while [[ ! -e "${check_dir}" && "${check_dir}" != "/" ]]; do
    check_dir="$(dirname "${check_dir}")"
  done
  if [[ -w "${check_dir}" ]]; then
    mkdir -p "${dest_dir}"
    rm -rf "${dest_dir}/${BUNDLE_NAME}"
    cp -R "${bundle_dir}" "${dest_dir}/"
  else
    echo "Installing to ${dest_dir} requires administrator privileges..."
    sudo mkdir -p "${dest_dir}"
    sudo rm -rf "${dest_dir}/${BUNDLE_NAME}"
    sudo cp -R "${bundle_dir}" "${dest_dir}/"
  fi
  echo "Installed ${BUNDLE_NAME} to ${dest_dir}/${BUNDLE_NAME}"
  echo "Restart DaVinci Resolve (Blackmagic Design website build; Mac App Store sandbox blocks third-party OFX)."
  echo "Find Paper Cutout FX on the Edit tab under Effects > Open FX > MTTN PaperCraft Studio."
  echo "Find the Paper Fold & Unfold transition under Effects > Open FX > Transitions > MTTN PaperCraft Studio."
}

main "$@"
