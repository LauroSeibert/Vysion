#!/bin/sh
# Cross-compile from Linux with mingw-w64 (posix threads). On Windows use build.bat (MSYS2/mingw) instead.
set -e
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-windres res/app.rc -O coff -o build/app.res
x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -s -municode -mwindows -Isrc src/main.cpp build/app.res \
  -o build/Vysion.exe -static -static-libgcc -static-libstdc++ \
  -lopengl32 -lgdiplus -lshlwapi -lcomdlg32 -ldwmapi -lole32 -loleaut32 -luuid -lshell32 -luser32 -lgdi32
ls -la build/Vysion.exe
