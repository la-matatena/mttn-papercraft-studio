#!/bin/bash
# Compiles, signs, and installs PaperCraftStudioPro.ofx.bundle for macOS.

set -euo pipefail

readonly BUNDLE_NAME="PaperCraftStudioPro.ofx.bundle"
readonly BINARY_NAME="PaperCraftStudioPro.ofx"
TMP_DIR=""

usage() {
  echo "Usage: $(basename "$0") [--user] [--dry-run] [--build-only DIR]"
}

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
  local bundle_dir="${tmp_dir}/${BUNDLE_NAME}"
  mkdir -p "${bundle_dir}/Contents/MacOS" "${bundle_dir}/Contents/Resources"
  cp "${src_dir}/Info.plist" "${bundle_dir}/Contents/Info.plist"
  cp "${src_dir}/openfx_include/LICENSE" "${bundle_dir}/Contents/Resources/LICENSE"

  local -a common_flags=(
    -std=c++17 -O3 -fPIC -bundle -fvisibility=hidden -mmacosx-version-min=11.0
    -DPAPERCRAFT_STANDALONE_OFX "-I${src_dir}" "-I${src_dir}/openfx_include"
    -Wl,-exported_symbol,_OfxGetNumberOfPlugins -Wl,-exported_symbol,_OfxGetPlugin
  )
  local -a srcs=("${src_dir}/papercraft_kernel.cc" "${src_dir}/papercraft_ofx_plugin.cc")
  local bin_out="${bundle_dir}/Contents/MacOS/${BINARY_NAME}"
  if clang++ "${common_flags[@]}" -arch x86_64 "${srcs[@]}" -o "${tmp_dir}/x86_64.ofx" 2>/dev/null && \
     clang++ "${common_flags[@]}" -arch arm64 "${srcs[@]}" -o "${tmp_dir}/arm64.ofx" 2>/dev/null; then
    lipo -create -output "${bin_out}" "${tmp_dir}/x86_64.ofx" "${tmp_dir}/arm64.ofx"
  else
    clang++ "${common_flags[@]}" "${srcs[@]}" -o "${bin_out}"
  fi
  chmod 0755 "${bin_out}"
  command -v xattr >/dev/null 2>&1 && xattr -cr "${bundle_dir}" || true
  command -v codesign >/dev/null 2>&1 && codesign --force --deep --sign - "${bundle_dir}" || true

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
}

main "$@"
