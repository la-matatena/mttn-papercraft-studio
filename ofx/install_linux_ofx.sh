#!/bin/bash
# Installs PaperCraftStudioPro.ofx.bundle for DaVinci Resolve on Linux x86-64.
#
# Copies the prebuilt bundle that sits next to this script (in the extracted
# release ZIP) into an OpenFX plugin folder Resolve scans at startup. It does
# not compile anything; the Linux binary is built by CI.
#
# Usage:
#   install_linux_ofx.sh [--user] [--dry-run] [--dest DIR]
# Flags:
#   --user      Install to ~/.local/share/OFX/Plugins instead of the
#               system-wide /usr/OFX/Plugins (no sudo needed).
#   --dry-run   Print source bundle and target folder; copy nothing.
#   --dest DIR  Install into DIR (overrides --user).
#   -h, --help  Show usage.

set -euo pipefail

readonly BUNDLE_NAME="PaperCraftStudioPro.ofx.bundle"

#######################################
# Prints the usage line.
# Arguments:
#   None
# Outputs:
#   Writes usage to stdout.
#######################################
usage() {
  echo "Usage: $(basename "$0") [--user] [--dry-run] [--dest DIR]"
}

#######################################
# Parses flags and copies the bundle into the chosen OpenFX folder.
# Globals:
#   BUNDLE_NAME
#   HOME
# Arguments:
#   Command-line flags (see file header).
# Outputs:
#   Writes progress and the install location to stdout, errors to stderr.
# Returns:
#   0 on success; exits 1 on bad flags or a missing source bundle.
#######################################
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

  # Find the nearest existing ancestor of dest_dir to decide whether the
  # install needs sudo. chmod restores the exec bit in case the ZIP
  # extractor dropped it.
  local check_dir="${dest_dir}"
  while [[ ! -e "${check_dir}" && "${check_dir}" != "/" ]]; do
    check_dir="$(dirname "${check_dir}")"
  done
  if [[ -w "${check_dir}" ]]; then
    mkdir -p "${dest_dir}"
    rm -rf "${dest_dir}/${BUNDLE_NAME}"
    cp -a "${src_bundle}" "${dest_dir}/"
    chmod 0755 "${dest_dir}/${BUNDLE_NAME}/Contents/Linux-x86-64/PaperCraftStudioPro.ofx"
  else
    echo "Installing to ${dest_dir} requires sudo..."
    sudo mkdir -p "${dest_dir}"
    sudo rm -rf "${dest_dir}/${BUNDLE_NAME}"
    sudo cp -a "${src_bundle}" "${dest_dir}/"
    sudo chmod 0755 "${dest_dir}/${BUNDLE_NAME}/Contents/Linux-x86-64/PaperCraftStudioPro.ofx"
  fi
  echo "Installed ${BUNDLE_NAME} to ${dest_dir}/${BUNDLE_NAME}"
  echo "Restart DaVinci Resolve and open Effects > Open FX > MTTN PaperCraft Studio on the Edit tab."
  echo "The Paper Fold & Unfold transition is under Effects > Open FX > Transitions > MTTN PaperCraft Studio."
}

main "$@"
