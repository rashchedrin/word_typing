# Word Typing Practice

This repository contains two standalone versions of the same typing trainer:

- `index.html`: browser-based version with inline HTML, CSS, and JavaScript.
- `main.cpp`: C++ terminal UI version for local terminal use.

## Features

Both versions provide the same core functionality:

- Custom practice word.
- Three mistake modes:
  - allow mistakes and let the user backspace,
  - reject the wrong character,
  - erase the whole current attempt.
- Optional mistake feedback.
- Optional sound.
- Optional hidden-text practice mode.
- Live timer.
- Statistics for completed words, completion rate, accuracy, and best/median/last speed.

## Browser Version

Open `index.html` in a browser.

The browser app is self-contained and does not require any build step.

## C++ TUI Version

Build with CMake:

```bash
cmake -S . -B build
cmake --build build
```

Run:

```bash
./build/word_typing_practice_tui
```

Controls:

- `Tab`: switch focus between word editing and practice input
- `Ctrl+A`: cycle mistake action
- `Ctrl+S`: toggle sound
- `Ctrl+F`: toggle mistake feedback
- `Ctrl+T`: toggle hide text
- `Ctrl+C`: quit

The TUI uses local Linux audio tools when available for sound playback.
