#!/bin/bash
# Installs PaperCraftStudioPro.ofx.bundle for DaVinci Resolve on Linux x86-64.

set -euo pipefail

readonly BUNDLE_NAME="PaperCraftStudioPro.ofx.bundle"

usage() {
  echo "Usage: $(basename "$0") [--user] [--dry-run] [--dest DIR]"
}

main() {
  local mode="system"
  local dry_run="false"
  local custom_dest=""
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
      --dest)
        custom_dest="${2:-}"
        [[ -n "${custom_dest}" ]] || { usage; exit 1; }
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
  local src_bundle="${script_dir}/${BUNDLE_NAME}"
  local dest_dir="/usr/OFX/Plugins"
  if [[ -n "${custom_dest}" ]]; then
    dest_dir="${custom_dest}"
  elif [[ "${mode}" == "user" ]]; then
    dest_dir="${HOME}/.local/share/OFX/Plugins"
  fi

  if [[ "${dry_run}" == "true" ]]; then
    echo "[dry-run] Source bundle: ${src_bundle}"
    echo "[dry-run] Target OpenFX directory: ${dest_dir}/${BUNDLE_NAME}"
    exit 0
  fi

  if [[ ! -d "${src_bundle}" ]]; then
    echo "Missing ${src_bundle}. Run from the extracted PaperCraft release folder." >&2
    exit 1
  fi

  chmod 0755 "${src_bundle}/Contents/Linux-x86-64/PaperCraftStudioPro.ofx"
  local check_dir="${dest_dir}"
  while [[ ! -e "${check_dir}" && "${check_dir}" != "/" ]]; do
    check_dir="$(dirname "${check_dir}")"
  done
  if [[ -w "${check_dir}" ]]; then
    mkdir -p "${dest_dir}"
    rm -rf "${dest_dir}/${BUNDLE_NAME}"
    cp -a "${src_bundle}" "${dest_dir}/"
  else
    echo "Installing to ${dest_dir} requires sudo..."
    sudo mkdir -p "${dest_dir}"
    sudo rm -rf "${dest_dir}/${BUNDLE_NAME}"
    sudo cp -a "${src_bundle}" "${dest_dir}/"
  fi
  echo "Installed ${BUNDLE_NAME} to ${dest_dir}/${BUNDLE_NAME}"
  echo "Restart DaVinci Resolve and open Effects > Open FX / Generators / Video Transitions > MTTN PaperCraft Studio on the Edit tab."
}

main "$@"
