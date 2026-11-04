@echo off
rem Build on Windows with MSYS2 (pacman -S mingw-w64-ucrt-x86_64-gcc), run from a UCRT64 shell or with g++ on PATH.
windres res\app.rc -O coff -o build\app.res || exit /b 1
g++ -std=c++17 -O2 -s -municode -mwindows -Isrc src\main.cpp build\app.res -o build\Vysion.exe -static -lopengl32 -lgdiplus -lshlwapi -lcomdlg32 -ldwmapi -lole32 -loleaut32 -luuid -lshell32 -luser32 -lgdi32
