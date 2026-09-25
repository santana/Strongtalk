#!/bin/sh
# Install the development tools needed to build and format the Strongtalk VM.
#
#   tools/setup-deps.sh [--dry-run]
#   make setup-deps
#
# Installs clang-format pinned to the exact version CI uses (CLANG_FORMAT_VERSION,
# default 23.1.0) so local formatting is byte-identical to `make format-check`.
# The build toolchain is only *checked* and reported on; nothing is installed
# with sudo and nothing here touches package-manager state beyond the user's
# Python, so the script is safe to rerun.

set -u

DRY_RUN=0
case "${1-}" in
  --dry-run) DRY_RUN=1 ;;
esac

CLANG_FORMAT_VERSION=${CLANG_FORMAT_VERSION:-23.1.0}
NEEDED=latest   # successfully matched the pinned version

say()   { printf 'setup-deps: %s\n' "$*"; }
warn()  { printf 'setup-deps: WARNING: %s\n' "$*" >&2; }
run()   {
  if [ "$DRY_RUN" = 1 ]; then
    printf 'setup-deps: would run: %s\n' "$*"
    return 0
  fi
  "$@"
}

CLANG_FORMAT=$(command -v clang-format 2>/dev/null || true)
if [ -n "$CLANG_FORMAT" ]; then
  LOCAL_VERSION=$("$CLANG_FORMAT" --version 2>/dev/null | sed -E 's/.*version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
  if [ "$LOCAL_VERSION" = "$CLANG_FORMAT_VERSION" ]; then
    say "clang-format $LOCAL_VERSION found at $CLANG_FORMAT (matches CI pin)."
    NEEDED=
  else
    warn "clang-format $(command -v clang-format) is $LOCAL_VERSION; CI pins $CLANG_FORMAT_VERSION."
    warn "Re-pinning via Python to keep local formatting in sync with 'make format-check'."
  fi
fi

if [ -n "$NEEDED" ]; then
  PIP=$(command -v pipx 2>/dev/null || command -v pip3 2>/dev/null || true)
  if [ -z "$PIP" ]; then
    warn "no pipx/pip3 found; run one of:"
    warn "  brew install clang-format          # macOS (version may lag the pin)"
    warn "  apt-get install clang-format       # Linux/Ubuntu (version may lag the pin)"
    warn "  python3 -m pip install --user \"clang-format==$CLANG_FORMAT_VERSION\""
    say "version $CLANG_FORMAT_VERSION is required for byte-identical formatting to CI."
    exit 1
  fi

  PIP_NAME=$(basename "$PIP")
  if [ "$PIP_NAME" = "pip3" ]; then
    PIP="python3 -m pip"
  fi
  say "installing clang-format==$CLANG_FORMAT_VERSION with $PIP ..."
  # --user keeps it out of the system-site-packages; --break-system-packages is
  # the PEP 668 flag (needed on managed Pythons, e.g. Debian/Ubuntu/Homebrew).
  run $PIP install --user --break-system-packages \
    "clang-format==$CLANG_FORMAT_VERSION" || {
    warn "'$PIP install' failed; install clang-format $CLANG_FORMAT_VERSION manually"
    warn "  and make sure its binary is on PATH."
    exit 1
  }

  if [ "$DRY_RUN" = 1 ]; then
    say "skipping the version sanity check in --dry-run mode."
    NEEDED=
  else
    CLANG_FORMAT=$(command -v clang-format 2>/dev/null || true)
    LOCAL_VERSION=$("$CLANG_FORMAT" --version 2>/dev/null | sed -E 's/.*version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/' || true)
    if [ "$LOCAL_VERSION" != "$CLANG_FORMAT_VERSION" ]; then
      warn "clang-format still not on PATH/resolvable after install (got '${LOCAL_VERSION:-nothing}')."
      warn "'$PIP install --user' may place it under ~/.local/bin; ensure that is on PATH."
      exit 1
    fi
    say "clang-format $LOCAL_VERSION is now on PATH at $CLANG_FORMAT."
  fi
fi

say "checking the build toolchain (informational, not installing):"
case "$(uname -s)" in
  Darwin)
    if command -v clang >/dev/null 2>&1 && command -v make >/dev/null 2>&1; then
      say "  compiler + make: found (clang $(clang --version | head -1))."
    else
      warn "  compiler/make missing install the Xcode Command Line Tools:"
      warn "    xcode-select --install"
    fi
    ;;
  Linux)
    if command -v make >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1; then
      say "  make + g++: found."
    else
      warn "  make/g++ missing; run:"
      warn "    sudo apt-get install -y make g++"
    fi
    ;;
  *)
    warn "  unsupported platform for toolchain hints: $(uname -s)."
    ;;
esac

if [ "$DRY_RUN" = 1 ]; then
  say "dry run complete (no changes made)."
else
  say "done."
fi