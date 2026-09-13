#!/usr/bin/env bash
# =============================================================================
# build.sh — сборка и установка плагина BeatStepSync для VCV Rack 2
# Arch Linux / JACK MIDI
# =============================================================================
set -euo pipefail

# ---------------------------------------------------------------------------
# Настройки — при необходимости переопределить через переменные окружения
# ---------------------------------------------------------------------------
: "${RACK_SDK_DIR:=$HOME/Rack2SDK}"
: "${RACK_PLUGINS_DIR:=$HOME/.local/share/Rack2/plugins-lin-x64}"
: "${BUILD_JOBS:=$(nproc)}"

PLUGIN_SLUG="BeatStepSync"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Цвета
# ---------------------------------------------------------------------------
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()    { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

# ---------------------------------------------------------------------------
# Проверки окружения
# ---------------------------------------------------------------------------
check_deps() {
    info "Проверка зависимостей…"

    local missing=()
    for cmd in make g++ pkg-config jq; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        # Separate hint for jq since it's often forgotten
        if [[ " ${missing[*]} " == *" jq "* ]]; then
            warn "jq не найден — он нужен Rack SDK для чтения plugin.json"
            warn "  Установи: sudo pacman -S jq"
        fi
        local build_missing=("${missing[@]/jq/}")
        build_missing=("${build_missing[@]}")  # compact
        if [[ ${#build_missing[@]} -gt 0 && "${build_missing[*]}" != " " ]]; then
            error "Не найдены: ${missing[*]}\n  Установи: sudo pacman -S base-devel jq"
        fi
        # jq only — auto-install offer
        [[ " ${missing[*]} " == *" jq "* ]] && \
            error "Установи jq и повтори: sudo pacman -S jq"
    fi

    if [[ ! -d "$RACK_SDK_DIR" ]]; then
        error "VCV Rack 2 SDK не найден в '$RACK_SDK_DIR'\n\
  Скачай SDK с https://vcvrack.com/downloads и распакуй:\n\
    wget https://vcvrack.com/downloads/Rack-SDK-2-lin-x64.zip\n\
    unzip Rack-SDK-2-lin-x64.zip -d \$HOME/Rack2SDK\n\
  Или задай RACK_SDK_DIR=/путь/к/sdk $0"
    fi

    if [[ ! -f "$RACK_SDK_DIR/plugin.mk" ]]; then
        error "plugin.mk не найден в '$RACK_SDK_DIR' — похоже, SDK неполный."
    fi

    info "Зависимости ОК"
    info "SDK: $RACK_SDK_DIR"
}

# ---------------------------------------------------------------------------
# Сборка
# ---------------------------------------------------------------------------
build() {
    info "Сборка плагина (j$BUILD_JOBS)…"
    cd "$SCRIPT_DIR"

    RACK_DIR="$RACK_SDK_DIR" make -j"$BUILD_JOBS"

    # Rack SDK plugin.mk всегда создаёт plugin.so (не BeatStepSync.so)
    if [[ ! -f "plugin.so" ]]; then
        error "Сборка завершилась, но plugin.so не найден."
    fi
    info "Сборка успешна: plugin.so"
}

# ---------------------------------------------------------------------------
# Установка в $RACK_PLUGINS_DIR (см. настройки выше)
# ---------------------------------------------------------------------------
install_plugin() {
    local dest="$RACK_PLUGINS_DIR/$PLUGIN_SLUG"
    info "Установка в $dest …"

    mkdir -p "$dest"

    # Rack SDK собирает plugin.so — именно его копируем
    cp plugin.so    "$dest/"
    cp plugin.json  "$dest/"

    info "Установлено. Перезапусти VCV Rack 2."
}

# ---------------------------------------------------------------------------
# Очистка
# ---------------------------------------------------------------------------
clean() {
    info "Очистка…"
    cd "$SCRIPT_DIR"
    RACK_DIR="$RACK_SDK_DIR" make clean 2>/dev/null || rm -f *.so build/ -rf
    info "Готово."
}

# ---------------------------------------------------------------------------
# Точка входа
# ---------------------------------------------------------------------------
usage() {
    echo "Использование: $0 [build|install|clean|all]"
    echo "  build   — только сборка"
    echo "  install — только установка (нужен собранный .so)"
    echo "  clean   — очистить объектные файлы"
    echo "  all     — сборка + установка  (по умолчанию)"
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
