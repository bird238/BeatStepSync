#!/usr/bin/env bash
# =============================================================================
# build.sh — build and install the BeatStepSync plugin for VCV Rack 2
# Arch Linux / JACK MIDI
# =============================================================================
set -euo pipefail

# ---------------------------------------------------------------------------
# Settings — override via environment variables if needed
# ---------------------------------------------------------------------------
: "${RACK_SDK_DIR:=$HOME/Rack2SDK}"
: "${RACK_PLUGINS_DIR:=$HOME/.local/share/Rack2/plugins-lin-x64}"
: "${BUILD_JOBS:=$(nproc)}"

PLUGIN_SLUG="BeatStepSync"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()    { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

# ---------------------------------------------------------------------------
# Environment checks
# ---------------------------------------------------------------------------
check_deps() {
    info "Checking dependencies..."

    local missing=()
    for cmd in make g++ pkg-config jq; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        # Separate hint for jq since it's often forgotten
        if [[ " ${missing[*]} " == *" jq "* ]]; then
            warn "jq not found -- the Rack SDK needs it to read plugin.json"
            warn "  Install: sudo pacman -S jq"
        fi
        local build_missing=("${missing[@]/jq/}")
        build_missing=("${build_missing[@]}")  # compact
        if [[ ${#build_missing[@]} -gt 0 && "${build_missing[*]}" != " " ]]; then
            error "Missing: ${missing[*]}\n  Install: sudo pacman -S base-devel jq"
        fi
        # jq only — auto-install offer
        [[ " ${missing[*]} " == *" jq "* ]] && \
            error "Install jq and retry: sudo pacman -S jq"
    fi

    if [[ ! -d "$RACK_SDK_DIR" ]]; then
        error "VCV Rack 2 SDK not found at '$RACK_SDK_DIR'\n\
  Download the SDK from https://vcvrack.com/downloads and unpack it:\n\
    wget https://vcvrack.com/downloads/Rack-SDK-2-lin-x64.zip\n\
    unzip Rack-SDK-2-lin-x64.zip -d \$HOME/Rack2SDK\n\
  Or set RACK_SDK_DIR=/path/to/sdk $0"
    fi

    if [[ ! -f "$RACK_SDK_DIR/plugin.mk" ]]; then
        error "plugin.mk not found in '$RACK_SDK_DIR' -- the SDK looks incomplete."
    fi

    info "Dependencies OK"
    info "SDK: $RACK_SDK_DIR"
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
build() {
    info "Building plugin (j$BUILD_JOBS)..."
    cd "$SCRIPT_DIR"

    RACK_DIR="$RACK_SDK_DIR" make -j"$BUILD_JOBS"

    # The Rack SDK's plugin.mk always produces plugin.so (not BeatStepSync.so)
    if [[ ! -f "plugin.so" ]]; then
        error "Build finished but plugin.so was not found."
    fi
    info "Build successful: plugin.so"
}

# ---------------------------------------------------------------------------
# Install into $RACK_PLUGINS_DIR (see settings above)
# ---------------------------------------------------------------------------
install_plugin() {
    local dest="$RACK_PLUGINS_DIR/$PLUGIN_SLUG"
    info "Installing to $dest ..."

    mkdir -p "$dest"

    # The Rack SDK builds plugin.so -- that's the file we copy
    cp plugin.so    "$dest/"
    cp plugin.json  "$dest/"

    info "Installed. Restart VCV Rack 2."
}

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
clean() {
    info "Cleaning..."
    cd "$SCRIPT_DIR"
    RACK_DIR="$RACK_SDK_DIR" make clean 2>/dev/null || rm -f *.so build/ -rf
    info "Done."
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
usage() {
    echo "Usage: $0 [build|install|clean|all]"
    echo "  build   -- build only"
    echo "  install -- install only (needs an already-built .so)"
    echo "  clean   -- remove object files"
    echo "  all     -- build + install (default)"
}

CMD="${1:-all}"

case "$CMD" in
    build)   check_deps; build ;;
    install) install_plugin ;;
    clean)   clean ;;
    all)     check_deps; build; install_plugin ;;
    -h|--help) usage ;;
    *) usage; exit 1 ;;
esac
