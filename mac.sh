#!/usr/bin/env bash
# ---------------------------------------------------------------------------
#  Compile et lance ClaudeDesk sur macOS.
#    ./mac.sh                 Debug : compile, lance les tests, puis l'application
#    ./mac.sh build           Debug : compile + tests seulement
#    ./mac.sh release         Release : compile puis lance
#    ./mac.sh release build   Release : compile seulement → build/mac-release/ClaudeDesk
#  Prérequis (Homebrew) : brew install cmake ninja sdl3 nlohmann-json freetype
# ---------------------------------------------------------------------------
set -euo pipefail
cd "$(dirname "$0")"

preset=mac-debug
if [[ "${1:-}" == "release" ]]; then
    preset=mac-release
    shift
fi

for tool in cmake ninja; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[ERREUR] $tool introuvable. Installer les dépendances :"
        echo "  brew install cmake ninja sdl3 nlohmann-json freetype"
        exit 1
    fi
done

[[ -f "build/$preset/build.ninja" ]] || cmake --preset "$preset"
cmake --build --preset "$preset"
[[ "$preset" == mac-debug ]] && ctest --preset mac-debug

[[ "${1:-}" == "build" ]] && exit 0
exec "build/$preset/ClaudeDesk"
