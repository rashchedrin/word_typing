#!/usr/bin/env bash
# Builds the word_typing_practice_tui executable. Run from project root or any directory.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

cmake -S . -B build
make -C build
