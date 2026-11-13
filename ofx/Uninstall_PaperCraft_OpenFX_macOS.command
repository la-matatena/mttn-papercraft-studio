#!/bin/bash
# Removes MTTN PaperCraft Studio from DaVinci Resolve on macOS: the OpenFX
# bundle (system and --user installs) and the optional .drfx Fusion templates.
#
# Double-click it in Finder or run it from a shell. It lists everything it
# will delete first, and uses sudo only for paths the user cannot write.
#
# Usage:
#   Uninstall_PaperCraft_OpenFX_macOS.command [--dry-run]
# Flags:
#   --dry-run   List what would be removed, delete nothing.
#   -h, --help  Show usage.

set -euo pipefail

readonly BUNDLE_NAME="PaperCraftStudioPro.ofx.bundle"
# Template folder to remove (searched under each Fusion/Templates root).
readonly TEMPLATE_DIR_NAME="MTTN PaperCraft Studio"

dry_run="false"
case "${1:-}" in
  --dry-run) dry_run="true" ;;
  "") ;;
  -h|--help) echo "Usage: $(basename "$0") [--dry-run]"; exit 0 ;;
  *) echo "Usage: $(basename "$0") [--dry-run]" >&2; exit 1 ;;
esac

# Collect every path to delete: the OpenFX bundle in the system and user
# plugin folders, then template folders and stray PaperCraft .drfx files in
# both Fusion template roots. `-prune` stops find from descending into a
# matched template folder, which is removed as a whole.
targets=()
for dir in "/Library/OFX/Plugins" "${HOME}/Library/OFX/Plugins"; do
  [[ -e "${dir}/${BUNDLE_NAME}" ]] && targets+=("${dir}/${BUNDLE_NAME}")
done
for tpl in "${HOME}/Library/Application Support/Blackmagic Design/DaVinci Resolve/Fusion/Templates" \
           "/Library/Application Support/Blackmagic Design/DaVinci Resolve/Fusion/Templates"; do
  [[ -d "${tpl}" ]] || continue
  while IFS= read -r found; do
    targets+=("${found}")
  done < <(find "${tpl}" \( -type d -name "${TEMPLATE_DIR_NAME}" -prune \) -o \
                         \( -type f -iname "*PaperCraft*.drfx" \) | sort)
done

if [[ ${#targets[@]} -eq 0 ]]; then
  echo "MTTN PaperCraft Studio is not installed. Nothing to remove."
  exit 0
fi

echo "Removing:"
printf '  %s\n' "${targets[@]}"
[[ "${dry_run}" == "true" ]] && { echo "[dry-run] Nothing was deleted."; exit 0; }

# Delete each target, escalating to sudo only when its parent is not
# writable (e.g. /Library/OFX/Plugins).
for path in "${targets[@]}"; do
  if [[ -w "$(dirname "${path}")" ]]; then
    rm -rf "${path}"
  else
    echo "Removing ${path} requires administrator privileges..."
    sudo rm -rf "${path}"
  fi
done
echo "Done. Restart DaVinci Resolve to unload MTTN PaperCraft Studio."
echo "Clips that already use the effects will show them as missing; remove them from your timelines."
