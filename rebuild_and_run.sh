#!/usr/bin/env bash
# Cleans, rebuilds, and runs word_typing_practice_tui. Run from project root or any directory.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

cmake -S . -B build
make -C build clean
make -C build
exec ./build/word_typing_practice_tui
