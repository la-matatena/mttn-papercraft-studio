#!/usr/bin/env bash
#
# Runs every formatter and linter that enforces the repository style guide
# (docs/developer_guide.md, "Code style and documentation conventions").
# CI runs it in check mode; developers run it before committing.
#
# Usage:
#   tools/lint.sh             Check only; exits 1 if anything needs fixing.
#   tools/lint.sh --fix       Apply the formatters, then run the linters.
#   tools/lint.sh --pins pip  Print the pinned pip packages (used by CI).
#   tools/lint.sh --pins npm  Print the pinned npm packages (used by CI).
#
# Checks (all first-party sources; vendored ofx/openfx_include/ is skipped):
#   - Python (*.py):            pyink (pyproject.toml) and pylint (.pylintrc).
#   - C++ (ofx/*.cc, ofx/*.h):  clang-format (.clang-format).
#   - Shell (ofx/*.command, ofx/*.sh, tools/*.sh): shellcheck.
#   - CSS (site/*.css):         prettier (.prettierrc.json, .prettierignore).
#
# The tools are dev-only (the product itself has no dependencies) and must be
# on PATH. ${REPO_ROOT}/.venv-dev/bin and .venv-dev/node/node_modules/.bin
# are added to PATH automatically when they exist; see print_install_help for
# the exact pinned install commands. Formatter output can change between
# versions, so a version mismatch is reported as a warning.

set -euo pipefail

# Pinned tool versions. CI installs exactly these through `--pins`.
readonly PYINK_VERSION="26.5.1"
readonly PYLINT_VERSION="4.1.3"
readonly CLANG_FORMAT_VERSION="23.1.5"
readonly SHELLCHECK_PY_VERSION="0.11.0.1"
readonly SHELLCHECK_VERSION="0.11.0"  # Binary shipped by shellcheck-py.
readonly PRETTIER_VERSION="3.10.1"
readonly NPM_REGISTRY="https://registry.npmjs.org/"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT

#######################################
# Prints the pinned packages for one package manager.
# Arguments:
#   $1: "pip" or "npm".
# Outputs:
#   Space-separated package specs on stdout.
# Returns:
#   0 on success, 1 for an unknown package manager.
#######################################
print_pins() {
  case "$1" in
    pip)
      echo "pyink==${PYINK_VERSION} pylint==${PYLINT_VERSION}" \
        "clang-format==${CLANG_FORMAT_VERSION}" \
        "shellcheck-py==${SHELLCHECK_PY_VERSION}"
      ;;
    npm)
      echo "prettier@${PRETTIER_VERSION}"
      ;;
    *)
      echo "Unknown package manager for --pins: $1 (use pip or npm)" >&2
      return 1
      ;;
  esac
}

#######################################
# Prints how to install the pinned tools into the git-ignored .venv-dev/.
# Outputs:
#   Install commands on stderr.
#######################################
print_install_help() {
  cat >&2 <<EOF
Install the pinned lint tools (dev-only, git-ignored) from the repo root:
  python3 -m venv .venv-dev
  .venv-dev/bin/pip install $(print_pins pip)
  npm install --prefix .venv-dev/node --registry ${NPM_REGISTRY} $(print_pins npm)
tools/lint.sh picks up .venv-dev/bin and .venv-dev/node/node_modules/.bin
automatically.
EOF
}

#######################################
# Verifies that the needed tools are on PATH and warns about version
# mismatches. Only tools for languages that have files are required, so the
# script also works in trees without site/ (e.g. the Complete Repo ZIP).
# Globals:
#   PY_FILES, CPP_FILES, SH_FILES, CSS_FILES
# Outputs:
#   Missing-tool errors and version warnings on stderr.
# Returns:
#   0 when all needed tools are present, 1 otherwise.
#######################################
check_tools() {
  local -a needed=()
  if [[ ${#PY_FILES[@]} -gt 0 ]]; then
    needed+=(pyink pylint)
  fi
  if [[ ${#CPP_FILES[@]} -gt 0 ]]; then
    needed+=(clang-format)
  fi
  if [[ ${#SH_FILES[@]} -gt 0 ]]; then
    needed+=(shellcheck)
  fi
  if [[ ${#CSS_FILES[@]} -gt 0 ]]; then
    needed+=(prettier)
  fi
  if [[ ${#needed[@]} -eq 0 ]]; then
    return 0
  fi
  local missing=0
  local tool
  for tool in "${needed[@]}"; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
      echo "Missing tool: ${tool}" >&2
      missing=1
    fi
  done
  if [[ "${missing}" -ne 0 ]]; then
    print_install_help
    return 1
  fi
  for tool in "${needed[@]}"; do
    case "${tool}" in
      pyink) warn_version "${tool}" "${PYINK_VERSION}" ;;
      pylint) warn_version "${tool}" "${PYLINT_VERSION}" ;;
      clang-format) warn_version "${tool}" "${CLANG_FORMAT_VERSION}" ;;
      shellcheck) warn_version "${tool}" "${SHELLCHECK_VERSION}" ;;
      prettier) warn_version "${tool}" "${PRETTIER_VERSION}" ;;
      *) ;;
    esac
  done
}

#######################################
# Warns when a tool's --version output does not mention the pinned version.
# Arguments:
#   $1: tool name (also the command that is run with --version).
#   $2: pinned version.
# Outputs:
#   A warning on stderr on mismatch.
#######################################
warn_version() {
  local output
  output="$("$1" --version 2>&1 || true)"
  # Match the version as a whole token so 3.10.1 does not match 3.10.10.
  if ! grep -Eq "(^|[^0-9.])${2//./\\.}([^0-9.]|$)" <<<"${output}"; then
    echo "Warning: $1 is not the pinned ${2}; results may differ from CI." >&2
  fi
}

#######################################
# Lists candidate source files relative to REPO_ROOT, one per line.
# Uses git (tracked plus untracked, not ignored) when available, and falls
# back to find (e.g. in the Complete Repo ZIP, which has no .git).
# Globals:
#   REPO_ROOT
# Outputs:
#   File paths on stdout.
#######################################
list_repo_files() {
  if git -C "${REPO_ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "${REPO_ROOT}" ls-files -co --exclude-standard
  else
    (cd "${REPO_ROOT}" && find . -type f \
      -not -path './.git/*' -not -path '*/node_modules/*' \
      -not -path './.venv*' -not -path './venv/*' \
      -not -path './build/*' -not -path './dist/*' -not -path './release/*' |
      sed 's|^\./||')
  fi
}

#######################################
# Sorts repository files into one array per language.
# Globals:
#   PY_FILES, CPP_FILES, SH_FILES, CSS_FILES (set)
#######################################
collect_files() {
  PY_FILES=()
  CPP_FILES=()
  SH_FILES=()
  CSS_FILES=()
  local path
  while IFS= read -r path; do
    # Deleted-but-tracked files are listed by git; skip them.
    [[ -f "${REPO_ROOT}/${path}" ]] || continue
    case "${path}" in
      ofx/openfx_include/*) ;;  # Vendored OpenFX SDK headers.
      *.py) PY_FILES+=("${path}") ;;
      ofx/*.cc | ofx/*.h) CPP_FILES+=("${path}") ;;
      ofx/*.command | ofx/*.sh | tools/*.sh) SH_FILES+=("${path}") ;;
      site/*.css) CSS_FILES+=("${path}") ;;
      *) ;;
    esac
  done < <(list_repo_files | LC_ALL=C sort)
}

#######################################
# Runs one named step and records a failure without stopping the script.
# Globals:
#   FAILED_STEPS (appended on failure)
# Arguments:
#   $1: step label.
#   $@: remaining arguments are the command to run.
# Outputs:
#   Step banner plus the command's own output.
#######################################
run_step() {
  local label="$1"
  shift
  echo "==> ${label}"
  if ! "$@"; then
    FAILED_STEPS+=("${label}")
  fi
}

#######################################
# Applies or checks every formatter, then runs the linters.
# Globals:
#   REPO_ROOT, PY_FILES, CPP_FILES, SH_FILES, CSS_FILES, FAILED_STEPS
# Arguments:
#   $1: "true" to apply formatters, "false" to only check.
# Outputs:
#   Tool output on stdout/stderr and a final summary.
# Returns:
#   0 when every step passed, 1 otherwise.
#######################################
run_checks() {
  local fix="$1"
  FAILED_STEPS=()
  cd "${REPO_ROOT}"

  if [[ ${#PY_FILES[@]} -gt 0 ]]; then
    if [[ "${fix}" == "true" ]]; then
      run_step "pyink (format)" pyink --quiet "${PY_FILES[@]}"
    else
      run_step "pyink (check)" pyink --check --diff --quiet "${PY_FILES[@]}"
    fi
    run_step "pylint" pylint --rcfile=.pylintrc --score=no "${PY_FILES[@]}"
  fi

  if [[ ${#CPP_FILES[@]} -gt 0 ]]; then
    if [[ "${fix}" == "true" ]]; then
      run_step "clang-format (format)" clang-format -i "${CPP_FILES[@]}"
    else
      run_step "clang-format (check)" \
        clang-format --dry-run --Werror "${CPP_FILES[@]}"
    fi
  fi

  if [[ ${#SH_FILES[@]} -gt 0 ]]; then
    run_step "shellcheck" shellcheck "${SH_FILES[@]}"
  fi

  if [[ ${#CSS_FILES[@]} -gt 0 ]]; then
    if [[ "${fix}" == "true" ]]; then
      run_step "prettier (format)" prettier --log-level warn --write \
        "${CSS_FILES[@]}"
    else
      run_step "prettier (check)" prettier --log-level warn --check \
        "${CSS_FILES[@]}"
    fi
  fi

  if [[ ${#FAILED_STEPS[@]} -gt 0 ]]; then
    echo "Lint FAILED: ${FAILED_STEPS[*]}" >&2
    if [[ "${fix}" != "true" ]]; then
      echo "Formatting issues can be fixed with: tools/lint.sh --fix" >&2
    fi
    return 1
  fi
  echo "All lint checks passed."
}

#######################################
# Parses flags, prepares PATH, and runs the checks.
# Arguments:
#   Command-line flags (see file header).
# Outputs:
#   Tool output and a summary on stdout/stderr.
# Returns:
#   0 when every check passes; exits 1 on bad flags, missing tools, or any
#   failed check.
#######################################
main() {
  local fix="false"
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --fix)
        fix="true"
        shift
        ;;
      --pins)
        print_pins "${2:-}"
        exit $?
        ;;
      -h | --help)
        sed -n '2,/^$/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
        exit 0
        ;;
      *)
        echo "Unknown option: $1 (see tools/lint.sh --help)" >&2
        exit 1
        ;;
    esac
  done

  # Prefer the repo-local, git-ignored tool installs when present.
  local dir
  for dir in "${REPO_ROOT}/.venv-dev/node/node_modules/.bin" \
    "${REPO_ROOT}/.venv-dev/bin"; do
    if [[ -d "${dir}" ]]; then
      PATH="${dir}:${PATH}"
    fi
  done
  export PATH

  collect_files
  check_tools || exit 1
  run_checks "${fix}" || exit 1
}

main "$@"
