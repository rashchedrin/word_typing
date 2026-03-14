#!/usr/bin/env bash
# Cleans, rebuilds, and runs word_typing_practice_tui. Run from project root or any directory.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

rm -f build/word_typing_practice_tui
"$SCRIPT_DIR/build.sh"
exec ./build/word_typing_practice_tui
