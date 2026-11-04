<p align="center"><img src="docs/logo.png" width="96" alt="Vysion logo"></p>

<h1 align="center">Vysion</h1>

<p align="center">A tiny video player for Windows that opens instantly and shows a preview of the exact second you hover on the timeline.</p>

---

## Why I built it

I built Vysion for my own daily use. The Windows **Photos** and **Movies & TV / Media Player** apps are
too slow for how I watch videos: they take a while to open, the timeline lags behind the mouse, and
there are plenty of options I never use. I wanted something that opens immediately, has a clean
minimal UI, and lets me scrub through a video by hovering instead of guessing.

Optimizing software until the improvement is measurable is what I enjoy most, so this was also an
exercise in keeping every part of the player lean: a ~550 KB native executable, no framework,
no runtime and no background services.

## Features

- **Opens instantly.** A native Win32 executable with no .NET, Electron or other runtime to start.
- **Timeline previews.** Hover the timeline (or anywhere in the band above it) to see a thumbnail of
  that exact second. Click or drag there to seek.
  - A second, muted, hidden decoder renders the previews, so playback is never interrupted.
  - Rough previews of the whole video are prepared in the background, so something shows instantly
    while the exact frame is decoded.
- **Minimal, auto-hiding UI.**
  - There's no visible title bar: in a window, just a small minimize / maximize / close box in the corner.
  - The timeline and controls appear only when the mouse is near the bottom, and fade out.
  - The previous / next arrows sit on the screen edges and appear on mouse movement.
  - In fullscreen, the close button appears only when you hover the top-right corner.
- **Folder navigation that follows Explorer.** Next / previous use the same order as the Explorer window
  you opened the video from (name, date, size, groups, ascending or descending). They also work from
  search results, and fall back to natural name order.
- **Rotate the view** (button, or `Ctrl+R` / `Ctrl+Shift+R`): turn a sideways 9:16 clip into 16:9 instantly.
  It rotates on the GPU while drawing, with no re-encoding, and the file is never touched. The previews rotate
  too, and rotation resets for the next video.
- **Three repeat modes** (one button, or `R`): don't repeat, repeat this video, or play the next video
  automatically.
- **Remembers your settings:** volume, mute, repeat mode and window size and position.
- **Plays almost anything** that FFmpeg/libmpv can decode, with hardware decoding when available.
- **Native Windows look:** the icons come from the system icon font (Segoe Fluent Icons / Segoe MDL2
  Assets), the UI is DPI-aware per monitor, and the title bar is dark.
- **Smooth fullscreen.** It uses a borderless window, so there is no black flash when switching and
  NVIDIA's overlay doesn't treat it as a game.
- **Keyboard and mouse:**

| Key / mouse | Action |
|---|---|
| Space, K, click on video | Play / pause |
| Double-click, F, F11, Enter | Fullscreen (Esc to leave) |
| Left / Right | -5 s / +5 s (Shift = 1 s, Ctrl = 30 s) |
| J / L | -10 s / +10 s |
| Up / Down, mouse wheel | Volume |
| M | Mute |
| R | Cycle repeat mode |
| Ctrl+R / Ctrl+Shift+R | Rotate view clockwise / counter-clockwise |
| N / P, PageDown / PageUp, mouse side buttons | Next / previous video in the folder |
| , / . | Previous / next frame |
| Ctrl+O / Ctrl+W | Open / close |

## Stack

| Part | Technology |
|---|---|
| Language | C++17 |
| Window, input, file dialogs | Win32 API |
| Video decoding and playback | [libmpv](https://mpv.io) (FFmpeg), loaded at runtime |
| Video rendering | OpenGL (WGL) through the libmpv render API, on a dedicated render thread |
| Timeline previews | A second libmpv instance rendering into an offscreen OpenGL framebuffer on its own thread |
| UI overlay | GDI+ drawn into premultiplied-alpha layers, uploaded as GL textures and blended over the video |
| Explorer sort order | Shell COM APIs (`IShellWindows`, `IFolderView`) |
| Settings | INI file in `%APPDATA%\Vysion` |
| Build | MinGW-w64 (cross-compiled from Linux, or MSYS2 on Windows) |
| Installer | NSIS: per-user, no admin prompt, Apps & features entry, Default apps registration |

How it fits together:

```
UI thread      window, input, mpv events, draws the overlay layers (GDI+)
render thread  owns the GL context, renders mpv + overlay layers, vsync
thumb thread   hidden muted mpv instance -> offscreen FBO -> preview cache
```

## Download and install

Download **`Vysion-Setup-x.y.z.exe`** from **Releases** and run it. It installs for your user only (no admin
prompt) into `%LOCALAPPDATA%\Programs\Vysion`. It also:
- adds a Start menu shortcut, plus a desktop shortcut if you tick it;
- adds an entry to **Settings > Apps** with a working uninstaller;
- registers Vysion in **Open with** and **Settings > Default apps**. Tick the box on the last page to open
  that settings page and make Vysion your default player.

Windows doesn't let apps make themselves the default without asking, which is why that last step is one click.
The installer isn't code-signed, so SmartScreen may show "Windows protected your PC" the first time:
click **More info > Run anyway**.

If you'd rather not install anything, there's also a portable zip: extract it anywhere and run `Vysion.exe`,
keeping the mpv DLL next to it.

For the newest codecs, you can replace the bundled DLL with a current `libmpv-2.dll` from
[shinchiro's mpv builds](https://github.com/shinchiro/mpv-winbuild-cmake/releases). Vysion loads
`libmpv-2.dll`, `mpv-2.dll`, `mpv-1.dll` or `mpv.dll`, in that order.

## Build from source

- **Linux (cross-compile):** install `g++-mingw-w64-x86-64-posix`, then run `./build.sh`
- **Windows:** in an MSYS2 UCRT64 shell with gcc, run `build.bat`

Then put a libmpv DLL next to `build/Vysion.exe`. To build the installer, run
`makensis -DVERSION=1.1.0 installer/vysion.nsi` (NSIS 3), which writes `build/Vysion-Setup-1.1.0.exe`.

## License note

libmpv and FFmpeg are licensed under the (L)GPL. Vysion loads the DLL at runtime. Release zips that include
the DLL point to its source at https://github.com/mpv-player/mpv.
