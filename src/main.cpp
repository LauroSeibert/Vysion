// Vysion - a tiny, fast video player for Windows.
// Win32 + OpenGL + libmpv (loaded at runtime), GDI+ for the overlay controls.
//
// Threads:
//   UI thread      - window, input, mpv events, draws the control overlay into a bitmap (GDI+)
//   render thread  - owns the GL context of the main window, renders mpv + overlay, vsync
//   thumb thread   - second (hidden, muted) mpv instance that renders timeline previews into an FBO

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <exdisp.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <GL/gl.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mpv/client.h"
#include "mpv/render_gl.h"

// ---------------------------------------------------------------------------------------------
// libmpv, loaded dynamically so any libmpv build (libmpv-2.dll / mpv-2.dll / mpv-1.dll) works.
// ---------------------------------------------------------------------------------------------
struct MpvApi {
    decltype(&mpv_create) create;
    decltype(&mpv_initialize) initialize;
    decltype(&mpv_terminate_destroy) terminate_destroy;
    decltype(&mpv_set_option_string) set_option_string;
    decltype(&mpv_command) command;
    decltype(&mpv_command_async) command_async;
    decltype(&mpv_set_property) set_property;
    decltype(&mpv_set_property_async) set_property_async;
    decltype(&mpv_get_property) get_property;
    decltype(&mpv_observe_property) observe_property;
    decltype(&mpv_wait_event) wait_event;
    decltype(&mpv_set_wakeup_callback) set_wakeup_callback;
    decltype(&mpv_render_context_create) rc_create;
    decltype(&mpv_render_context_set_update_callback) rc_set_update_callback;
    decltype(&mpv_render_context_update) rc_update;
    decltype(&mpv_render_context_render) rc_render;
    decltype(&mpv_render_context_report_swap) rc_report_swap;
    decltype(&mpv_render_context_free) rc_free;
} M;

static bool LoadMpv(const std::wstring& dir) {
    const wchar_t* names[] = {L"libmpv-2.dll", L"mpv-2.dll", L"mpv-1.dll", L"mpv.dll"};
    HMODULE h = nullptr;
    for (auto n : names) {
        h = LoadLibraryExW((dir + n).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (h) break;
    }
    if (!h) return false;
#define L_(f, n) M.f = (decltype(M.f))(void*)GetProcAddress(h, n); if (!M.f) return false;
    L_(create, "mpv_create") L_(initialize, "mpv_initialize") L_(terminate_destroy, "mpv_terminate_destroy")
    L_(set_option_string, "mpv_set_option_string") L_(command, "mpv_command") L_(command_async, "mpv_command_async")
    L_(set_property, "mpv_set_property") L_(set_property_async, "mpv_set_property_async")
    L_(get_property, "mpv_get_property") L_(observe_property, "mpv_observe_property")
    L_(wait_event, "mpv_wait_event") L_(set_wakeup_callback, "mpv_set_wakeup_callback")
    L_(rc_create, "mpv_render_context_create") L_(rc_set_update_callback, "mpv_render_context_set_update_callback")
    L_(rc_update, "mpv_render_context_update") L_(rc_render, "mpv_render_context_render")
    L_(rc_report_swap, "mpv_render_context_report_swap") L_(rc_free, "mpv_render_context_free")
#undef L_
    return true;
}

// ---------------------------------------------------------------------------------------------
// Minimal GL loader (things beyond OpenGL 1.1)
// ---------------------------------------------------------------------------------------------
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#define GL_CLAMP_TO_EDGE_ 0x812F
#define GL_FRAMEBUFFER_ 0x8D40
#define GL_COLOR_ATTACHMENT0_ 0x8CE0
#define GL_ARRAY_BUFFER_ 0x8892
#define GL_ELEMENT_ARRAY_BUFFER_ 0x8893
#define GL_PIXEL_UNPACK_BUFFER_ 0x88EC
#define GL_PIXEL_PACK_BUFFER_ 0x88EB
#define GL_TEXTURE0_ 0x84C0

typedef void (APIENTRY* PFN_UseProgram)(GLuint);
typedef void (APIENTRY* PFN_BindBuffer)(GLenum, GLuint);
typedef void (APIENTRY* PFN_BindVertexArray)(GLuint);
typedef void (APIENTRY* PFN_ActiveTexture)(GLenum);
typedef void (APIENTRY* PFN_BindSampler)(GLuint, GLuint);
typedef void (APIENTRY* PFN_GenFramebuffers)(GLsizei, GLuint*);
typedef void (APIENTRY* PFN_BindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY* PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY* PFN_DeleteFramebuffers)(GLsizei, const GLuint*);
typedef BOOL (WINAPI* PFN_SwapInterval)(int);

static void* GLProc(void*, const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    if (!p || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1) {
        static HMODULE gl = LoadLibraryW(L"opengl32.dll");
        p = (void*)GetProcAddress(gl, name);
    }
    return p;
}
template <class T> static T GLF(const char* n) { return (T)GLProc(nullptr, n); }

static bool SetupPixelFormat(HDC dc) {
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(dc, &pfd);
    return pf && SetPixelFormat(dc, pf, &pfd);
}

// ---------------------------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------------------------
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring FromUtf8(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}
static std::wstring FormatTime(double t, bool hours) {
    if (!(t >= 0)) t = 0;
    long s = (long)t;
    wchar_t b[32];
    if (hours) swprintf(b, 32, L"%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(b, 32, L"%02ld:%02ld", s / 60, s % 60);
    return b;
}
// Same, with milliseconds (for the trim handles): 1:23.456 or 1:02:03.456
static std::wstring FormatTimeMs(double t, bool hours) {
    if (!(t >= 0)) t = 0;
    long long ms = (long long)std::llround(t * 1000);
    long s = (long)(ms / 1000);
    wchar_t b[40];
    if (hours) swprintf(b, 40, L"%ld:%02ld:%02ld.%03d", s / 3600, (s / 60) % 60, s % 60, (int)(ms % 1000));
    else swprintf(b, 40, L"%ld:%02ld.%03d", s / 60, s % 60, (int)(ms % 1000));
    return b;
}
extern UINT gDpiForMetrics;
static int SysMetric(int idx) {
    typedef int(WINAPI * Fn)(int, UINT);
    static Fn f = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    return f ? f(idx, gDpiForMetrics) : GetSystemMetrics(idx);
}
static const wchar_t* kVideoExt[] = {L".mp4", L".mkv", L".webm", L".avi", L".mov", L".m4v", L".wmv", L".flv",
                                     L".mpg", L".mpeg", L".ts", L".m2ts", L".mts", L".3gp", L".ogv", L".vob",
                                     L".gif", L".hevc", L".mxf", L".divx", L".f4v", L".rmvb", L".asf"};
static bool IsVideoFile(const std::wstring& name) {
    const wchar_t* ext = PathFindExtensionW(name.c_str());
    for (auto e : kVideoExt) if (_wcsicmp(ext, e) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------------------------
// Settings (persisted in %APPDATA%\Vysion\settings.ini)
// ---------------------------------------------------------------------------------------------
struct Settings {
    double volume = 100;
    bool mute = false;
    int repeat = 2;
    RECT rect = {CW_USEDEFAULT, CW_USEDEFAULT, 0, 0};
    bool maximized = false;
    std::wstring path;

    void Init() {
        wchar_t* appdata = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
            std::wstring root = appdata;
            std::wstring dir = root + L"\\Vysion";
            CoTaskMemFree(appdata);
            CreateDirectoryW(dir.c_str(), nullptr);
            path = dir + L"\\settings.ini";
            // carry over settings from the old name
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                CopyFileW((root + L"\\LitePlayer\\settings.ini").c_str(), path.c_str(), TRUE);
        }
    }
    void Load() {
        if (path.empty()) return;
        wchar_t b[64];
        GetPrivateProfileStringW(L"player", L"volume", L"100", b, 64, path.c_str());
        volume = std::clamp(_wtof(b), 0.0, 100.0);
        mute = GetPrivateProfileIntW(L"player", L"mute", 0, path.c_str()) != 0;
        repeat = std::clamp((int)GetPrivateProfileIntW(L"player", L"repeat", 2, path.c_str()), 0, 2);
        int x = GetPrivateProfileIntW(L"window", L"x", INT_MIN, path.c_str());
        int y = GetPrivateProfileIntW(L"window", L"y", INT_MIN, path.c_str());
        int w = GetPrivateProfileIntW(L"window", L"w", 0, path.c_str());
        int h = GetPrivateProfileIntW(L"window", L"h", 0, path.c_str());
        maximized = GetPrivateProfileIntW(L"window", L"maximized", 0, path.c_str()) != 0;
        if (x != INT_MIN && y != INT_MIN && w > 200 && h > 150) {
            RECT r = {x, y, x + w, y + h};
            // make sure it is still on a monitor
            if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) rect = r;
        }
    }
    void SavePlayer() {
        if (path.empty()) return;
        wchar_t b[64];
        swprintf(b, 64, L"%.0f", volume);
        WritePrivateProfileStringW(L"player", L"volume", b, path.c_str());
        WritePrivateProfileStringW(L"player", L"mute", mute ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"player", L"repeat", repeat == 0 ? L"0" : (repeat == 1 ? L"1" : L"2"), path.c_str());
    }
    void SaveWindow() {
        if (path.empty()) return;
        auto put = [&](const wchar_t* k, int v) {
            wchar_t b[32]; swprintf(b, 32, L"%d", v);
            WritePrivateProfileStringW(L"window", k, b, path.c_str());
        };
        put(L"x", rect.left); put(L"y", rect.top);
        put(L"w", rect.right - rect.left); put(L"h", rect.bottom - rect.top);
        put(L"maximized", maximized ? 1 : 0);
    }
};

// ---------------------------------------------------------------------------------------------
// Thumbnails
// ---------------------------------------------------------------------------------------------
struct Thumb {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // top-down BGRA
};
using ThumbPtr = std::shared_ptr<const Thumb>;

static const int kCoarseCount = 72;    // keyframe previews spread over the file (instant hover)
static const size_t kExactCap = 400;   // exact per-second previews kept in memory

struct ThumbShared {
    std::mutex m;
    std::condition_variable cv;
    bool quit = false;
    // requests from UI
    std::string newPath;      // non-empty -> load this file
    bool hasNewPath = false;
    int baseWidth = 200;      // preview width in physical pixels (dpi scaled)
    int reqSec = -1;          // exact second wanted
    // results
    int generation = 0;       // incremented per file
    bool enabled = false;     // file has video + thumbnails work
    double duration = 0;
    ThumbPtr coarse[kCoarseCount];
    std::map<int, ThumbPtr> exact;
    std::deque<int> exactOrder;
};

// ---------------------------------------------------------------------------------------------
// Global app state
// ---------------------------------------------------------------------------------------------
enum Hit { H_NONE, H_VIDEO, H_BAR, H_PLAY, H_PREV, H_NEXT, H_TIMELINE, H_SCRUB, H_MUTE, H_VOLUME, H_FULLSCREEN,
           H_REPEAT, H_ROTATE, H_MIN, H_MAX, H_CLOSE, H_EXITFS, H_TRIM, H_TRIM_A, H_TRIM_B, H_TRIM_SAVE };

enum Repeat { REPEAT_OFF = 0, REPEAT_ONE = 1, REPEAT_NEXT = 2 };

struct Layout {
    float s = 1;           // dpi scale
    int cw = 0, ch = 0;    // client size
    // caption buttons: a small box in the top-right corner (always in a window, on activity in fullscreen)
    int btnW = 0, btnH = 0, dragH = 0;
    bool topShown = false;
    RECT topBox{}, minBtn{}, maxBtn{}, closeBtn{}, exitFsBtn{};
    // previous / next video: chevrons on the left and right edge
    bool sidesShown = false;
    RECT prev{}, next{};
    // bottom controls
    int trackL = 0, trackR = 0, trackY = 0, rowY = 0, barTop = 0;
    RECT timeline{}, play{}, mute{}, volume{}, rotate{}, repeat{}, fullscreen{};
    RECT trim{}, trimSave{};   // pen button; "Save as copy" (trim mode only)
    bool trimSaveWide = true;  // room for the full label
    bool hidePlay = false;     // narrow window in trim mode: the save button takes the play button's place
    int trimAX = 0, trimBX = 0;   // x of the two trim handles
    int volL = 0, volR = 0, volY = 0;
    RECT scrub{};          // big area above the timeline that also scrubs
    int ovH = 0;           // bottom overlay height
    int thumbW = 0, thumbH = 0;
};

// One overlay layer handed from the UI thread to the render thread.
struct Overlay {
    std::vector<uint32_t> px;
    int x = 0, y = 0, w = 0, h = 0, version = 0;
    bool visible = false;
    float alpha = 1;  // fade
};
enum { OV_BOTTOM, OV_TOP, OV_LEFT, OV_RIGHT, OV_COUNT };

struct App {
    HWND hwnd = nullptr;
    HINSTANCE inst = nullptr;
    mpv_handle* mpv = nullptr;
    Settings cfg;
    ULONG_PTR gdipToken = 0;

    // playback state (UI thread)
    std::wstring file;
    double timePos = 0, duration = 0;
    bool paused = false, hasFile = false, idle = true;
    double volume = 100;
    bool mute = false;
    std::wstring errorText;

    // UI state
    // what is shown: bottom controls (only near the bottom), side arrows (on mouse activity),
    // caption buttons (always in a window; only while hovered in fullscreen)
    bool showBottom = true, showSides = false, showTop = true;
    float alpha[OV_COUNT] = {1, 1, 0, 0};
    DWORD fadeTick = 0;
    bool recentActivity = false;   // mouse moved in the last ~2 s
    DWORD flashUntil = 0;          // bottom forced visible until this tick (file switch, keys)
    bool cursorHidden = false;
    bool mouseInside = false;
    bool active = true;
    Hit hover = H_NONE;
    Hit drag = H_NONE;
    Hit pressed = H_NONE;
    int mouseX = -1, mouseY = -1;
    double dragTime = 0;
    bool fullscreen = false;
    WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
    LONG savedStyle = 0;
    UINT dpi = 96;
    ThumbPtr shownThumb;
    int shownThumbGen = -1;
    DWORD lastSeekTick = 0;
    std::wstring toast;
    int toastRight = 0;            // >0: tooltip sits on the button row, ending at this x (next to its button)
    int rotate = 0;                // extra view rotation in degrees (0/90/180/270), display only
    bool eof = false;              // at the end of the file (keep-open)
    bool eofHandled = false;
    int repeat = REPEAT_NEXT;

    // trimming ("Save as copy" without re-encoding; the start snaps back to a keyframe)
    bool trimMode = false;
    double trimA = 0, trimB = 0;   // chosen start / end (seconds)
    double trimKeyA = -1;          // where the copy will really start (keyframe at or before trimA); -1 = finding out
    int trimProbeGen = 0;          // discards stale keyframe answers
    bool trimBusy = false;         // saving
    int rotateBeforeTrim = 0;      // the view rotation to bring back when trimming ends

    // overlays -> render thread
    std::mutex rm;
    std::condition_variable rcv;
    bool rQuit = false, rMpvDirty = false, rRedraw = true, rReady = false;
    Overlay ov[OV_COUNT];
    std::thread renderThread;

    ThumbShared th;
    std::thread thumbThread;

    std::atomic<bool> mpvWakePending{false};
} g;
UINT gDpiForMetrics = 96;

#define WM_APP_MPV (WM_APP + 1)
#define WM_APP_THUMB (WM_APP + 2)
#define WM_APP_NEXT (WM_APP + 3)
#define WM_APP_SNAPSHOT (WM_APP + 4)
#define WM_APP_REGISTER (WM_APP + 5)
#define WM_APP_TRIM (WM_APP + 6)
#define TIMER_HIDE 1
#define TIMER_SAVE 2
#define TIMER_TOAST 3
#define TIMER_FADE 4
#define TIMER_FLASH 5

static void ComposeOverlay();
static void ShowToast(const std::wstring& text, int anchorRight);
static bool OpenSibling(int dir, bool wrap);
static void RequestRender() {
    { std::lock_guard<std::mutex> l(g.rm); g.rRedraw = true; }
    g.rcv.notify_one();
}

// ---------------------------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------------------------
static void OnRenderUpdate(void*) {
    { std::lock_guard<std::mutex> l(g.rm); g.rMpvDirty = true; }
    g.rcv.notify_one();
}

struct GLOverlay {
    GLuint tex = 0;
    int version = -1, x = 0, y = 0, w = 0, h = 0;
    bool visible = false;
    float alpha = 1;
};

static void RenderThreadMain() {
    HDC dc = GetDC(g.hwnd);
    HGLRC glrc = wglCreateContext(dc);
    wglMakeCurrent(dc, glrc);
    if (auto si = GLF<PFN_SwapInterval>("wglSwapIntervalEXT")) si(1);
    auto glUseProgram_ = GLF<PFN_UseProgram>("glUseProgram");
    auto glBindBuffer_ = GLF<PFN_BindBuffer>("glBindBuffer");
    auto glBindVertexArray_ = GLF<PFN_BindVertexArray>("glBindVertexArray");
    auto glActiveTexture_ = GLF<PFN_ActiveTexture>("glActiveTexture");
    auto glBindSampler_ = GLF<PFN_BindSampler>("glBindSampler");
    auto glBindFramebuffer_ = GLF<PFN_BindFramebuffer>("glBindFramebuffer");

    mpv_opengl_init_params glInit{GLProc, nullptr, nullptr};
    const char* apiType = MPV_RENDER_API_TYPE_OPENGL;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)apiType},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context* rctx = nullptr;
    if (M.rc_create(&rctx, g.mpv, params) < 0) rctx = nullptr;
    if (rctx) M.rc_set_update_callback(rctx, OnRenderUpdate, nullptr);
    {
        std::lock_guard<std::mutex> l(g.rm);
        g.rReady = true;
    }
    g.rcv.notify_all();

    GLOverlay lay[OV_COUNT];
    for (auto& t : lay) glGenTextures(1, &t.tex);

    // called with g.rm held
    auto sync = [&](const Overlay& o, GLOverlay& t) {
        t.visible = o.visible && !o.px.empty() && o.alpha > 0.003f;
        t.x = o.x; t.y = o.y; t.alpha = o.alpha;
        if (!t.visible || t.version == o.version) return;
        if (glActiveTexture_) glActiveTexture_(GL_TEXTURE0_);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        if (glBindBuffer_) glBindBuffer_(GL_PIXEL_UNPACK_BUFFER_, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, o.w, o.h, 0, GL_BGRA, GL_UNSIGNED_BYTE, o.px.data());
        t.version = o.version; t.w = o.w; t.h = o.h;
    };
    auto drawQuad = [&](const GLOverlay& t) {
        glBindTexture(GL_TEXTURE_2D, t.tex);
        float x0 = (float)t.x, y0 = (float)t.y, x1 = x0 + t.w, y1 = y0 + t.h;
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x0, y0);
        glTexCoord2f(1, 0); glVertex2f(x1, y0);
        glTexCoord2f(1, 1); glVertex2f(x1, y1);
        glTexCoord2f(0, 1); glVertex2f(x0, y1);
        glEnd();
    };

    for (;;) {
        bool mpvDirty;
        {
            std::unique_lock<std::mutex> l(g.rm);
            g.rcv.wait(l, [] { return g.rQuit || g.rMpvDirty || g.rRedraw; });
            if (g.rQuit) break;
            mpvDirty = g.rMpvDirty;
            g.rMpvDirty = false;
            g.rRedraw = false;
            for (int i = 0; i < OV_COUNT; i++) sync(g.ov[i], lay[i]);
        }
        if (!rctx) { glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); SwapBuffers(dc); continue; }
        if (mpvDirty) M.rc_update(rctx);

        RECT rc;
        GetClientRect(g.hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        if (w <= 0 || h <= 0) continue;

        if (glBindFramebuffer_) glBindFramebuffer_(GL_FRAMEBUFFER_, 0);
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        mpv_opengl_fbo fbo{0, w, h, 0};
        int flip = 1;
        mpv_render_param rp[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                                 {MPV_RENDER_PARAM_FLIP_Y, &flip},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
        M.rc_render(rctx, rp);

        bool anyLayer = false;
        for (auto& t : lay) anyLayer |= t.visible && t.w > 0;
        if (anyLayer) {
            // restore a sane fixed-function state after mpv
            if (glUseProgram_) glUseProgram_(0);
            if (glBindVertexArray_) glBindVertexArray_(0);
            if (glBindBuffer_) { glBindBuffer_(GL_ARRAY_BUFFER_, 0); glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER_, 0); }
            if (glActiveTexture_) glActiveTexture_(GL_TEXTURE0_);
            if (glBindSampler_) glBindSampler_(0, 0);
            glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
            glDisable(GL_STENCIL_TEST);
            glViewport(0, 0, w, h);
            glMatrixMode(GL_PROJECTION); glLoadIdentity();
            glOrtho(0, w, h, 0, -1, 1);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied alpha from GDI+
            glEnable(GL_TEXTURE_2D);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glColor4f(1, 1, 1, 1);
            for (auto& t : lay) {
                if (!t.visible || t.w <= 0) continue;
                glColor4f(t.alpha, t.alpha, t.alpha, t.alpha);  // premultiplied: scale everything
                drawQuad(t);
            }
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_BLEND);
        }
        SwapBuffers(dc);
        M.rc_report_swap(rctx);
    }
    if (rctx) M.rc_free(rctx);
    for (auto& t : lay) glDeleteTextures(1, &t.tex);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(glrc);
    ReleaseDC(g.hwnd, dc);
}

// ---------------------------------------------------------------------------------------------
// Thumbnail thread: hidden mpv instance rendering into an offscreen FBO
// ---------------------------------------------------------------------------------------------
static std::atomic<bool> gThumbFrame{false};
static std::atomic<unsigned> gThumbFrameCount{0};
static void OnThumbUpdate(void*) {
    gThumbFrame = true;
    gThumbFrameCount++;
    g.th.cv.notify_all();
}

static void ThumbThreadMain() {
    ThumbShared& T = g.th;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g.inst;
    wc.lpszClassName = L"VysionThumbGL";
    wc.style = CS_OWNDC;
    RegisterClassW(&wc);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, g.inst, nullptr);
    HDC dc = GetDC(hw);
    SetupPixelFormat(dc);
    HGLRC glrc = wglCreateContext(dc);
    wglMakeCurrent(dc, glrc);
    auto glGenFramebuffers_ = GLF<PFN_GenFramebuffers>("glGenFramebuffers");
    auto glBindFramebuffer_ = GLF<PFN_BindFramebuffer>("glBindFramebuffer");
    auto glFramebufferTexture2D_ = GLF<PFN_FramebufferTexture2D>("glFramebufferTexture2D");
    auto glDeleteFramebuffers_ = GLF<PFN_DeleteFramebuffers>("glDeleteFramebuffers");
    auto glBindBuffer_ = GLF<PFN_BindBuffer>("glBindBuffer");
    if (!glGenFramebuffers_ || !glBindFramebuffer_ || !glFramebufferTexture2D_) {
        wglMakeCurrent(nullptr, nullptr); wglDeleteContext(glrc); ReleaseDC(hw, dc); DestroyWindow(hw);
        return;
    }

    mpv_handle* t = M.create();
    auto opt = [&](const char* k, const char* v) { M.set_option_string(t, k, v); };
    opt("config", "no"); opt("terminal", "no"); opt("msg-level", "all=no");
    opt("vo", "libmpv"); opt("ao", "null"); opt("audio", "no"); opt("sid", "no");
    opt("pause", "yes"); opt("keep-open", "always"); opt("idle", "yes");
    opt("osc", "no"); opt("ytdl", "no"); opt("load-scripts", "no"); opt("input-default-bindings", "no");
    opt("cache", "no"); opt("hr-seek-framedrop", "yes");
    if (M.set_option_string(t, "hwdec", "auto-copy-safe") < 0) opt("hwdec", "no");
    opt("vd-lavc-skiploopfilter", "all"); opt("vd-lavc-fast", "yes"); opt("vd-lavc-threads", "4");
    opt("scale", "bilinear"); opt("dscale", "bilinear"); opt("cscale", "bilinear");
    opt("correct-downscaling", "no"); opt("linear-downscaling", "no"); opt("sigmoid-upscaling", "no");
    opt("deband", "no"); opt("dither-depth", "no"); opt("video-sync", "audio");
    opt("background", "color"); opt("background-color", "#000000");
    M.initialize(t);

    mpv_opengl_init_params glInit{GLProc, nullptr, nullptr};
    const char* apiType = MPV_RENDER_API_TYPE_OPENGL;
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, (void*)apiType},
                                 {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context* rctx = nullptr;
    if (M.rc_create(&rctx, t, params) < 0) rctx = nullptr;
    if (rctx) M.rc_set_update_callback(rctx, OnThumbUpdate, nullptr);

    GLuint fbo = 0, tex = 0;
    glGenFramebuffers_(1, &fbo);
    glGenTextures(1, &tex);
    int tw = 0, tht = 0;
    bool fileOk = false;
    int prefillPos = 0;
    std::vector<int> prefillOrder;
    double dur = 0;
    int gen = 0;

    auto aborted = [&]() {
        std::lock_guard<std::mutex> l(T.m);
        return T.quit || T.hasNewPath;
    };
    // wait for a specific mpv event (or failure); returns event id seen, or -1 on timeout/abort
    auto waitFor = [&](std::initializer_list<int> ids, double timeout, std::function<void()> tick) -> int {
        DWORD start = GetTickCount();
        for (;;) {
            mpv_event* e = M.wait_event(t, 0.02);
            if (e->event_id != MPV_EVENT_NONE)
                for (int id : ids) if (e->event_id == id) return id;
            if (aborted()) return -1;
            if (tick) tick();
            if (GetTickCount() - start > (DWORD)(timeout * 1000)) return -1;
        }
    };

    auto renderFrame = [&]() -> ThumbPtr {
        // PLAYBACK_RESTART can arrive slightly before the VO has the new frame. Wait for a frame
        // update that happens after the restart (or a short timeout), then draw; if yet another
        // update lands right after drawing, draw again so we never keep a stale frame.
        auto waitNewer = [&](unsigned since, int ms) {
            std::unique_lock<std::mutex> l(T.m);
            return T.cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return gThumbFrameCount.load() != since || T.quit; });
        };
        unsigned c0 = gThumbFrameCount.load();
        waitNewer(c0, 120);
        gThumbFrame = false;
        unsigned c1 = gThumbFrameCount.load();
        M.rc_update(rctx);
        mpv_opengl_fbo f{(int)fbo, tw, tht, 0};
        int flip = 0, block = 0;
        mpv_render_param rp[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &f},
                                 {MPV_RENDER_PARAM_FLIP_Y, &flip},
                                 {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
        M.rc_render(rctx, rp);
        auto th = std::make_shared<Thumb>();
        th->w = tw; th->h = tht;
        th->px.resize((size_t)tw * tht);
        glBindFramebuffer_(GL_FRAMEBUFFER_, fbo);
        if (glBindBuffer_) glBindBuffer_(GL_PIXEL_PACK_BUFFER_, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        if (waitNewer(c1, 40)) {
            M.rc_update(rctx);
            M.rc_render(rctx, rp);
        }
        glBindFramebuffer_(GL_FRAMEBUFFER_, fbo);
        glReadPixels(0, 0, tw, tht, GL_BGRA, GL_UNSIGNED_BYTE, th->px.data());
        glBindFramebuffer_(GL_FRAMEBUFFER_, 0);
        for (auto& p : th->px) p |= 0xFF000000u;
        return th;
    };

    for (;;) {
        std::string path;
        int req = -1;
        bool doPrefill = false;
        {
            std::unique_lock<std::mutex> l(T.m);
            T.cv.wait(l, [&] {
                return T.quit || T.hasNewPath || (fileOk && (T.reqSec >= 0 || prefillPos < (int)prefillOrder.size()));
            });
            if (T.quit) break;
            if (T.hasNewPath) {
                path = T.newPath;
                T.hasNewPath = false;
                T.reqSec = -1;
                T.enabled = false;
                T.exact.clear(); T.exactOrder.clear();
                for (auto& c : T.coarse) c.reset();
                gen = ++T.generation;
            } else if (T.reqSec >= 0) {
                req = T.reqSec;
                T.reqSec = -1;
                if (T.exact.count(req)) req = -1;
            } else {
                doPrefill = true;
            }
        }
        if (!rctx) continue;

        if (!path.empty()) {
            fileOk = false;
            prefillOrder.clear();
            prefillPos = 0;
            // let the main player grab the disk/CPU first so the first frame shows up fast
            Sleep(250);
            if (aborted()) continue;
            const char* cmd[] = {"loadfile", path.c_str(), nullptr};
            M.command(t, cmd);
            int r = waitFor({MPV_EVENT_FILE_LOADED, MPV_EVENT_END_FILE}, 15, nullptr);
            if (r != MPV_EVENT_FILE_LOADED) continue;
            // video dimensions arrive with the first VIDEO_RECONFIG
            int64_t dw = 0, dh = 0;
            DWORD start = GetTickCount();
            while ((dw <= 0 || dh <= 0) && GetTickCount() - start < 4000 && !aborted()) {
                M.wait_event(t, 0.02);
                M.get_property(t, "dwidth", MPV_FORMAT_INT64, &dw);
                M.get_property(t, "dheight", MPV_FORMAT_INT64, &dh);
            }
            dur = 0;
            M.get_property(t, "duration", MPV_FORMAT_DOUBLE, &dur);
            if (dw <= 0 || dh <= 0 || dur <= 0) continue;
            int base;
            { std::lock_guard<std::mutex> l(T.m); base = T.baseWidth; }
            if (dw >= dh) { tw = base; tht = (int)std::lround((double)base * dh / dw); }
            else { tht = (int)std::lround(base * 0.8); tw = (int)std::lround((double)tht * dw / dh); }
            tw = std::max(tw, 16); tht = std::max(tht, 16);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, tht, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindFramebuffer_(GL_FRAMEBUFFER_, fbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D, tex, 0);
            glBindFramebuffer_(GL_FRAMEBUFFER_, 0);
            // coarse prefill order: halving strides so the whole range gets covered early
            std::vector<bool> used(kCoarseCount, false);
            for (int step = kCoarseCount; step >= 1; step /= 2)
                for (int i = 0; i < kCoarseCount; i += step)
                    if (!used[i]) { used[i] = true; prefillOrder.push_back(i); }
            for (int i = 0; i < kCoarseCount; i++) if (!used[i]) prefillOrder.push_back(i);
            {
                std::lock_guard<std::mutex> l(T.m);
                T.enabled = true;
                T.duration = dur;
            }
            fileOk = true;
            continue;
        }
        if (!fileOk) continue;

        if (req >= 0) {
            // exact preview for one second; newer requests pre-empt the running seek
            int target = req;
            double ts = std::min((double)target + 0.5, std::max(0.0, dur - 0.05));
            char tbuf[64];
            auto issue = [&]() {
                snprintf(tbuf, sizeof tbuf, "%.3f", ts);
                gThumbFrame = false;
                const char* cmd[] = {"seek", tbuf, "absolute+exact", nullptr};
                M.command_async(t, 0, cmd);
            };
            issue();
            int r = -1;
            DWORD seekStart = GetTickCount();
            for (;;) {
                r = waitFor({MPV_EVENT_PLAYBACK_RESTART}, 6, [&] {
                    std::lock_guard<std::mutex> l(T.m);
                    if (T.reqSec >= 0 && T.reqSec != target && !T.exact.count(T.reqSec)) {
                        target = T.reqSec;
                        T.reqSec = -1;
                        ts = std::min((double)target + 0.5, std::max(0.0, dur - 0.05));
                        issue();
                    }
                });
                if (r != MPV_EVENT_PLAYBACK_RESTART) break;
                // a restart from an older (pre-empted) seek may arrive first: check where we landed
                double pos = -1;
                M.get_property(t, "time-pos", MPV_FORMAT_DOUBLE, &pos);
                if (std::fabs(pos - ts) < 0.6 || GetTickCount() - seekStart > 6000) break;
            }
            if (r != MPV_EVENT_PLAYBACK_RESTART) continue;
            ThumbPtr img = renderFrame();
#ifdef DEBUG_THUMB
            { double pos=-1; M.get_property(t, "time-pos", MPV_FORMAT_DOUBLE, &pos);
              FILE* f=fopen("thumb.log","a"); fprintf(f,"exact target=%d ts=%.3f pos=%.3f\n",target,ts,pos); fclose(f); }
#endif
            {
                std::lock_guard<std::mutex> l(T.m);
                if (T.generation != gen) continue;
                if (!T.exact.count(target)) {
                    T.exact[target] = img;
                    T.exactOrder.push_back(target);
                    while (T.exactOrder.size() > kExactCap) {
                        T.exact.erase(T.exactOrder.front());
                        T.exactOrder.pop_front();
                    }
                }
            }
            PostMessageW(g.hwnd, WM_APP_THUMB, 0, 0);
        } else if (doPrefill) {
            int idx = prefillOrder[prefillPos++];
            double ts = (idx + 0.5) * dur / kCoarseCount;
            char tbuf[64];
            snprintf(tbuf, sizeof tbuf, "%.3f", ts);
            gThumbFrame = false;
            const char* cmd[] = {"seek", tbuf, "absolute+keyframes", nullptr};
            M.command_async(t, 0, cmd);
            int r = waitFor({MPV_EVENT_PLAYBACK_RESTART}, 4, nullptr);
            if (r != MPV_EVENT_PLAYBACK_RESTART) continue;
            ThumbPtr img = renderFrame();
            {
                std::lock_guard<std::mutex> l(T.m);
                if (T.generation != gen) continue;
                T.coarse[idx] = img;
            }
            PostMessageW(g.hwnd, WM_APP_THUMB, 0, 0);
        }
    }
    if (rctx) M.rc_free(rctx);
    M.terminate_destroy(t);
    if (glDeleteFramebuffers_) glDeleteFramebuffers_(1, &fbo);
    glDeleteTextures(1, &tex);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(glrc);
    ReleaseDC(hw, dc);
    DestroyWindow(hw);
}

// Pick the best preview available for time t (exact second, otherwise nearest coarse one).
static ThumbPtr LookupThumb(double t, bool* exactOut) {
    std::lock_guard<std::mutex> l(g.th.m);
    *exactOut = false;
    if (!g.th.enabled || g.th.duration <= 0) return nullptr;
    int sec = (int)std::floor(std::max(0.0, t));
    auto it = g.th.exact.find(sec);
    if (it != g.th.exact.end()) { *exactOut = true; return it->second; }
    int idx = std::clamp((int)(t / g.th.duration * kCoarseCount), 0, kCoarseCount - 1);
    for (int d = 0; d < kCoarseCount; d++) {
        if (idx - d >= 0 && g.th.coarse[idx - d]) return g.th.coarse[idx - d];
        if (idx + d < kCoarseCount && g.th.coarse[idx + d]) return g.th.coarse[idx + d];
    }
    return nullptr;
}
static void RequestThumb(double t) {
    int sec = (int)std::floor(std::max(0.0, t));
    {
        std::lock_guard<std::mutex> l(g.th.m);
        if (!g.th.enabled || g.th.exact.count(sec)) return;
        g.th.reqSec = sec;
    }
    g.th.cv.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Layout + hit testing
// ---------------------------------------------------------------------------------------------
static Layout ComputeLayout() {
    Layout L;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    L.cw = rc.right; L.ch = rc.bottom;
    L.s = g.dpi / 96.0f;
    float s = L.s;
    auto S = [&](float v) { return (int)std::lround(v * s); };
    int cw = L.cw, ch = L.ch;

    L.btnW = S(46); L.btnH = S(32); L.dragH = S(32);
    L.topShown = g.showTop;
    L.closeBtn = {cw - L.btnW, 0, cw, L.btnH};
    if (g.fullscreen) {
        L.exitFsBtn = {cw - 2 * L.btnW, 0, cw - L.btnW, L.btnH};
        L.topBox = {L.exitFsBtn.left, 0, cw, L.btnH};
    } else {
        L.maxBtn = {cw - 2 * L.btnW, 0, cw - L.btnW, L.btnH};
        L.minBtn = {cw - 3 * L.btnW, 0, cw - 2 * L.btnW, L.btnH};
        L.topBox = {L.minBtn.left, 0, cw, L.btnH};
    }

    int aw = S(36), ah = S(128);
    L.sidesShown = g.showSides;
    L.prev = {0, ch / 2 - ah / 2, aw, ch / 2 + ah / 2};
    L.next = {cw - aw, ch / 2 - ah / 2, cw, ch / 2 + ah / 2};

    L.rowY = ch - S(28);
    L.trackY = ch - S(70);
    L.trackL = S(24);
    L.trackR = std::max(L.trackL + S(20), cw - S(24));
    L.timeline = {L.trackL - S(8), L.trackY - S(10), L.trackR + S(8), L.trackY + S(10)};
    L.barTop = L.timeline.top;
    int cy = L.rowY;
    L.play = {cw / 2 - S(20), cy - S(18), cw / 2 + S(20), cy + S(18)};
    L.mute = {S(14), cy - S(16), S(14) + S(34), cy + S(16)};
    L.volume = {L.mute.right + S(2), cy - S(12), L.mute.right + S(2) + S(88), cy + S(12)};
    L.volL = L.volume.left + S(5); L.volR = L.volume.right - S(5); L.volY = cy;
    L.fullscreen = {cw - S(14) - S(34), cy - S(16), cw - S(14), cy + S(16)};
    L.repeat = {L.fullscreen.left - S(6) - S(34), cy - S(16), L.fullscreen.left - S(6), cy + S(16)};
    L.rotate = {L.repeat.left - S(6) - S(34), cy - S(16), L.repeat.left - S(6), cy + S(16)};
    L.trim = {L.rotate.left - S(6) - S(34), cy - S(16), L.rotate.left - S(6), cy + S(16)};
    if (g.trimMode) {
        int right = L.trim.left - S(8);
        int room = right - (L.play.right + S(10));
        if (room < S(64)) {   // too narrow: take the play button's place (clicking the video still pauses)
            L.hidePlay = true;
            room = right - (L.volume.right + S(10));
        }
        int w = std::clamp(room, S(64), S(118));
        L.trimSaveWide = w >= S(110);
        L.trimSave = {right - w, cy - S(15), right, cy + S(15)};
    }

    // Preview size, keep in sync with what the thumb thread renders
    L.thumbW = S(200);
    L.thumbH = S(160);
    L.ovH = g.hasFile ? std::min(ch, ch - L.trackY + S(22) + L.thumbH) : ch;
    L.ovH = std::max(L.ovH, 1);
    // the whole band where the preview appears also works as a (big) timeline
    L.scrub = {0, std::max(L.btnH, L.trackY - S(16) - L.thumbH), cw, L.barTop};
    if (g.duration > 0) {
        auto X = [&](double t) { return L.trackL + (int)std::lround((L.trackR - L.trackL) * std::clamp(t / g.duration, 0.0, 1.0)); };
        L.trimAX = X(g.trimA); L.trimBX = X(g.trimB);
    }
    return L;
}
static bool In(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
// Finished playing (keep-open leaves us paused on the last frame).
static bool AtEnd() {
    return g.hasFile && g.duration > 0 && (g.eof || (g.paused && g.timePos >= g.duration - 0.5));
}
static Hit HitTestRaw(const Layout& L, int x, int y, bool bottom, bool sides, bool top) {
    if (x < 0 || y < 0 || x >= L.cw || y >= L.ch) return H_NONE;
    if (top && In(L.topBox, x, y)) {
        if (In(L.closeBtn, x, y)) return H_CLOSE;
        if (g.fullscreen) return H_EXITFS;
        if (In(L.maxBtn, x, y)) return H_MAX;
        return H_MIN;
    }
    if (sides && In(L.prev, x, y)) return H_PREV;
    if (sides && In(L.next, x, y)) return H_NEXT;
    if (!bottom) return H_VIDEO;
    if (g.trimMode && g.duration > 0 && y >= L.trackY - (int)(26 * L.s) && y <= L.trackY + (int)(12 * L.s)) {
        int da = std::abs(x - L.trimAX), db = std::abs(x - L.trimBX), grab = (int)(9 * L.s);
        if (da <= grab || db <= grab) {
            if (da == db) return x < L.trimAX ? H_TRIM_A : H_TRIM_B;   // handles on top of each other
            return da < db ? H_TRIM_A : H_TRIM_B;
        }
    }
    if (y >= L.barTop) {
        if (In(L.timeline, x, y)) return H_TIMELINE;
        if (g.trimMode && In(L.trimSave, x, y)) return H_TRIM_SAVE;
        if (!L.hidePlay && In(L.play, x, y)) return H_PLAY;
        if (In(L.mute, x, y)) return H_MUTE;
        if (In(L.volume, x, y)) return H_VOLUME;
        if (In(L.repeat, x, y)) return H_REPEAT;
        if (!g.trimMode && In(L.rotate, x, y)) return H_ROTATE;
        if (g.hasFile && In(L.trim, x, y)) return H_TRIM;
        if (In(L.fullscreen, x, y)) return H_FULLSCREEN;
        return H_BAR;
    }
    if (g.hasFile && g.duration > 0 && In(L.scrub, x, y)) return H_SCRUB;
    return H_VIDEO;
}
static Hit HitTest(const Layout& L, int x, int y) { return HitTestRaw(L, x, y, g.showBottom, g.showSides, g.showTop); }
static bool IsTl(Hit h) { return h == H_TIMELINE || h == H_SCRUB; }
static double TimeAtX(const Layout& L, int x) {
    if (g.duration <= 0 || L.trackR <= L.trackL) return 0;
    double f = std::clamp((double)(x - L.trackL) / (L.trackR - L.trackL), 0.0, 1.0);
    return f * g.duration;
}

// ---------------------------------------------------------------------------------------------
// Overlay drawing (GDI+ into premultiplied BGRA buffers)
// ---------------------------------------------------------------------------------------------
using namespace Gdiplus;
static const Color kAccent(255, 76, 160, 255);
static const Color kTrim = kAccent;   // trim selection / handles: same blue as the progress bar

static void RoundRectPath(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    r = std::min(r, std::min(w, h) / 2);
    if (r <= 0.5f) { p.AddRectangle(RectF(x, y, w, h)); return; }
    REAL d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}
static void FillRound(Graphics& gr, const Brush& b, REAL x, REAL y, REAL w, REAL h, REAL r) {
    GraphicsPath p;
    RoundRectPath(p, x, y, w, h, r);
    gr.FillPath(&b, &p);
}
static const FontFamily* UiFont() {
    static FontFamily* segoe = new FontFamily(L"Segoe UI");
    return segoe->IsAvailable() ? segoe : FontFamily::GenericSansSerif();
}
static void InitGraphics(Graphics& gr) {
    gr.SetSmoothingMode(SmoothingModeAntiAlias);
    gr.SetPixelOffsetMode(PixelOffsetModeHalf);
    gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    gr.SetCompositingQuality(CompositingQualityHighSpeed);
}

// Windows' own icon font (the one Photos / Media Player use). Segoe Fluent Icons on Windows 11,
// Segoe MDL2 Assets on Windows 10. Same code points in both. Returns null if neither exists,
// in which case the hand-drawn icons below are used.
static const FontFamily* IconFont() {
    static const FontFamily* f = nullptr;
    static bool init = false;
    if (!init) {
        init = true;
        for (const wchar_t* name : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
            auto* ff = new FontFamily(name);
            if (ff->IsAvailable()) { f = ff; break; }
            delete ff;
        }
    }
    return f;
}
enum : wchar_t {
    G_PLAY = 0xE768, G_PAUSE = 0xE769, G_MUTE = 0xE74F, G_VOL0 = 0xE992, G_VOL1 = 0xE993, G_VOL2 = 0xE994, G_VOL3 = 0xE995,
    G_REPEAT_ALL = 0xE8EE, G_REPEAT_ONE = 0xE8ED, G_NEXT = 0xE893, G_ROTATE = 0xE7AD, G_FULLSCREEN = 0xE740, G_BACKTOWINDOW = 0xE73F,
    G_CHEVRON_LEFT = 0xE76B, G_CHEVRON_RIGHT = 0xE76C, G_EDIT = 0xE70F,
    G_MINIMIZE = 0xE921, G_MAXIMIZE = 0xE922, G_RESTORE = 0xE923, G_CLOSE = 0xE8BB,
};
// Draws one glyph of size px (pixels), centred on (cx, cy) by its actual ink bounds.
// ref: centre using another glyph's bounds (keeps e.g. the speaker still while the volume waves change)
static bool DrawGlyph(Graphics& gr, wchar_t ch, REAL cx, REAL cy, REAL px, const Color& c, wchar_t ref = 0) {
    const FontFamily* ff = IconFont();
    if (!ff) return false;
    px = std::round(px);   // whole-pixel sizes let the font's hinting snap edges to the pixel grid
    GraphicsPath path;
    wchar_t str[2] = {ch, 0};
    path.AddString(str, 1, ff, FontStyleRegular, px, PointF(0, 0), StringFormat::GenericTypographic());
    RectF b;
    if (ref) {
        GraphicsPath rp;
        wchar_t rs[2] = {ref, 0};
        rp.AddString(rs, 1, ff, FontStyleRegular, px, PointF(0, 0), StringFormat::GenericTypographic());
        rp.GetBounds(&b);
    } else {
        path.GetBounds(&b);
    }
    if (b.Width <= 0 || b.Height <= 0) return false;
    // Drawn as hinted text rather than a filled outline: grid-fitting keeps strokes on whole pixels
    // (a filled path ignores hinting, so edges fall between pixels and look soft).
    REAL dx = std::round(cx - (b.X + b.Width / 2)), dy = std::round(cy - (b.Y + b.Height / 2));
    Font f(ff, px, FontStyleRegular, UnitPixel);
    SolidBrush br(c);
    TextRenderingHint old = gr.GetTextRenderingHint();
    gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    gr.DrawString(str, 1, &f, PointF(dx, dy), StringFormat::GenericTypographic(), &br);
    gr.SetTextRenderingHint(old);
    return true;
}

// Thin line icons (fallback). (cx, cy) is the icon centre in bitmap coordinates.
static void IconRepeat(Graphics& gr, Pen& pen, SolidBrush& fill, REAL cx, REAL cy, REAL s, int mode) {
    if (mode == REPEAT_NEXT) {
        // "play next": a small play triangle followed by a bar, with a list line under it
        PointF tri[3] = {PointF(cx - 7 * s, cy - 6 * s), PointF(cx + 3 * s, cy), PointF(cx - 7 * s, cy + 6 * s)};
        gr.DrawPolygon(&pen, tri, 3);
        gr.DrawLine(&pen, cx + 6 * s, cy - 6 * s, cx + 6 * s, cy + 6 * s);
        return;
    }
    // loop: rounded rectangle with two arrow heads
    REAL w = 18 * s, h = 12 * s, x = cx - w / 2, y = cy - h / 2, r = 3.5f * s;
    GraphicsPath p;
    p.AddLine(x + r, y, x + w - r - 2 * s, y);
    p.StartFigure();
    p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    p.AddLine(x + w, y + r, x + w, y + h - r);
    p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    p.AddLine(x + w - r, y + h, x + r + 2 * s, y + h);
    p.StartFigure();
    p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    p.AddLine(x, y + h - r, x, y + r);
    p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    gr.DrawPath(&pen, &p);
    REAL a = 3.0f * s;
    gr.DrawLine(&pen, x + w - r - 2 * s - a, y - a, x + w - r - 2 * s, y);          // top arrow (pointing right)
    gr.DrawLine(&pen, x + w - r - 2 * s - a, y + a, x + w - r - 2 * s, y);
    gr.DrawLine(&pen, x + r + 2 * s + a, y + h - a, x + r + 2 * s, y + h);          // bottom arrow (pointing left)
    gr.DrawLine(&pen, x + r + 2 * s + a, y + h + a, x + r + 2 * s, y + h);
    if (mode == REPEAT_ONE) {
        Font f(UiFont(), std::round(8 * s), FontStyleBold, UnitPixel);
        StringFormat c; c.SetAlignment(StringAlignmentCenter); c.SetLineAlignment(StringAlignmentCenter);
        gr.DrawString(L"1", -1, &f, RectF(cx - 6 * s, cy - 6 * s, 12 * s, 12 * s), &c, &fill);
    } else {  // off: slash through it
        gr.DrawLine(&pen, cx - 8 * s, cy + 7 * s, cx + 8 * s, cy - 7 * s);
    }
}

static void DrawBottomIcons(Graphics& gr, const Layout& L, int oy) {
    float s = L.s;
    SolidBrush white(Color(255, 255, 255, 255));
    SolidBrush hoverBg(Color(36, 255, 255, 255));
    Pen pen(Color(255, 255, 255, 255), 1.3f * s);
    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
    auto hoverRect = [&](const RECT& r, Hit h) {
        if (g.hover == h) FillRound(gr, hoverBg, (REAL)r.left, (REAL)(r.top - oy), (REAL)(r.right - r.left), (REAL)(r.bottom - r.top), 2 * s);
    };
    auto C = [&](const RECT& r, REAL& cx, REAL& cy) { cx = (r.left + r.right) / 2.0f; cy = (r.top + r.bottom) / 2.0f - oy; };
    REAL cx, cy;

    const Color W(255, 255, 255, 255);
    const REAL gs = 16 * s;  // glyph size

    // play / pause
    if (!L.hidePlay) hoverRect(L.play, H_PLAY);
    C(L.play, cx, cy);
    if (L.hidePlay) {
    } else if (DrawGlyph(gr, g.paused || !g.hasFile ? G_PLAY : G_PAUSE, cx, cy, 18 * s, W)) {
    } else if (g.paused || !g.hasFile) {
        PointF pts[3] = {PointF(cx - 5 * s, cy - 8 * s), PointF(cx + 8 * s, cy), PointF(cx - 5 * s, cy + 8 * s)};
        gr.DrawPolygon(&pen, pts, 3);
    } else {
        gr.DrawLine(&pen, cx - 4 * s, cy - 8 * s, cx - 4 * s, cy + 8 * s);
        gr.DrawLine(&pen, cx + 4 * s, cy - 8 * s, cx + 4 * s, cy + 8 * s);
    }

    // speaker
    hoverRect(L.mute, H_MUTE);
    C(L.mute, cx, cy);
    wchar_t vg = g.mute ? G_MUTE : g.volume <= 0 ? G_VOL0 : g.volume < 34 ? G_VOL1 : g.volume < 67 ? G_VOL2 : G_VOL3;
    if (DrawGlyph(gr, vg, cx, cy, gs, W, G_VOL3)) {
    } else {
        cx -= 3 * s;
        PointF pts[6] = {PointF(cx - 7 * s, cy - 3.5f * s), PointF(cx - 3 * s, cy - 3.5f * s), PointF(cx + 2 * s, cy - 8 * s),
                         PointF(cx + 2 * s, cy + 8 * s), PointF(cx - 3 * s, cy + 3.5f * s), PointF(cx - 7 * s, cy + 3.5f * s)};
        gr.DrawPolygon(&pen, pts, 6);
        if (g.mute || g.volume <= 0) {
            gr.DrawLine(&pen, cx + 6 * s, cy - 3.5f * s, cx + 12 * s, cy + 3.5f * s);
            gr.DrawLine(&pen, cx + 12 * s, cy - 3.5f * s, cx + 6 * s, cy + 3.5f * s);
        } else {
            gr.DrawArc(&pen, cx - 1 * s, cy - 5 * s, 10 * s, 10 * s, -50, 100);
            if (g.volume > 50) gr.DrawArc(&pen, cx - 4 * s, cy - 9 * s, 18 * s, 18 * s, -50, 100);
        }
    }

    // volume slider
    {
        bool hv = g.hover == H_VOLUME || g.drag == H_VOLUME;
        REAL vh = 2 * s;
        REAL vl = (REAL)L.volL, vr = (REAL)L.volR, vy = L.volY - oy - vh / 2;
        FillRound(gr, SolidBrush(Color(80, 255, 255, 255)), vl, vy, vr - vl, vh, vh / 2);
        double vf = g.mute ? 0 : std::clamp(g.volume / 100.0, 0.0, 1.0);
        REAL vx = vl + (REAL)((vr - vl) * vf);
        if (vx > vl) FillRound(gr, SolidBrush(kAccent), vl, vy, vx - vl, vh, vh / 2);
        REAL r = (hv ? 6.0f : 4.5f) * s;
        gr.FillEllipse(&white, vx - r, L.volY - oy - r, r * 2, r * 2);
    }

    // trim (pen): highlighted while trimming
    if (g.hasFile) {
        if (g.trimMode) FillRound(gr, SolidBrush(Color(g.hover == H_TRIM ? 150 : 115, 76, 160, 255)), (REAL)L.trim.left,
                                  (REAL)(L.trim.top - oy), (REAL)(L.trim.right - L.trim.left), (REAL)(L.trim.bottom - L.trim.top), 2 * s);
        else hoverRect(L.trim, H_TRIM);
        C(L.trim, cx, cy);
        Color pc = W;   // white on the blue tint while trimming
        if (!DrawGlyph(gr, G_EDIT, cx, cy, gs, pc)) {
            Pen pp(pc, 1.3f * s);
            pp.SetStartCap(LineCapRound); pp.SetEndCap(LineCapRound);
            gr.DrawLine(&pp, cx - 6 * s, cy + 6 * s, cx + 5 * s, cy - 5 * s);
            gr.DrawLine(&pp, cx - 7 * s, cy + 7 * s, cx - 4 * s, cy + 7 * s);
        }
    }

    // "Save as copy"
    if (g.trimMode) {
        const RECT& r = L.trimSave;
        bool hv = g.hover == H_TRIM_SAVE && !g.trimBusy;
        Color bg = g.trimBusy ? Color(90, 76, 160, 255) : hv ? Color(255, 116, 182, 255) : kTrim;
        REAL bx = (REAL)r.left, by = (REAL)(r.top - oy), bw = (REAL)(r.right - r.left), bh = (REAL)(r.bottom - r.top);
        FillRound(gr, SolidBrush(bg), bx, by, bw, bh, bh / 2);
        Font f(UiFont(), std::round(12 * s), FontStyleBold, UnitPixel);
        StringFormat c; c.SetAlignment(StringAlignmentCenter); c.SetLineAlignment(StringAlignmentCenter);
        c.SetFormatFlags(StringFormatFlagsNoWrap);
        const wchar_t* t = g.trimBusy ? L"Saving…" : L.trimSaveWide ? L"Save as copy" : L"Save";
        SolidBrush ink(Color(255, 255, 255, 255));
        gr.DrawString(t, -1, &f, RectF(bx, by, bw, bh), &c, &ink);
    }

    // rotate view
    if (!g.trimMode) hoverRect(L.rotate, H_ROTATE);
    C(L.rotate, cx, cy);
    if (!DrawGlyph(gr, G_ROTATE, cx, cy, gs, g.trimMode ? Color(90, 255, 255, 255) : W)) {
        gr.DrawArc(&pen, cx - 7 * s, cy - 7 * s, 14 * s, 14 * s, -60, 290);
        gr.DrawLine(&pen, cx + 3.5f * s, cy - 6 * s, cx + 7 * s, cy - 7.5f * s);
        gr.DrawLine(&pen, cx + 3.5f * s, cy - 6 * s, cx + 4.5f * s, cy - 2 * s);
    }

    hoverRect(L.repeat, H_REPEAT);
    C(L.repeat, cx, cy);
    {
        // off = dimmed repeat icon (like Photos), one = repeat-one, next = next-track
        wchar_t rg = g.repeat == REPEAT_ONE ? G_REPEAT_ONE : g.repeat == REPEAT_NEXT ? G_NEXT : G_REPEAT_ALL;
        Color rc = g.repeat == REPEAT_OFF ? Color(115, 255, 255, 255) : W;
        if (!DrawGlyph(gr, rg, cx, cy, gs, rc)) IconRepeat(gr, pen, white, cx, cy, s, g.repeat);
    }

    // fullscreen: diagonal arrows (outward), or pointing inward when already fullscreen
    hoverRect(L.fullscreen, H_FULLSCREEN);
    C(L.fullscreen, cx, cy);
    if (!DrawGlyph(gr, g.fullscreen ? G_BACKTOWINDOW : G_FULLSCREEN, cx, cy, gs, W)) {
        REAL a = 7 * s, b = 2 * s, h = 4 * s;
        for (int d = -1; d <= 1; d += 2) {   // d = -1: bottom-left, +1: top-right
            REAL ox = cx + d * a, oy2 = cy - d * a;   // outer corner
            REAL ix = cx + d * b, iy = cy - d * b;    // inner point
            gr.DrawLine(&pen, ix, iy, ox, oy2);
            if (!g.fullscreen) {
                gr.DrawLine(&pen, ox, oy2, ox - d * h, oy2);
                gr.DrawLine(&pen, ox, oy2, ox, oy2 + d * h);
            } else {
                gr.DrawLine(&pen, ix, iy, ix + d * h, iy);
                gr.DrawLine(&pen, ix, iy, ix, iy - d * h);
            }
        }
    }
}

static void ComposeBottom(const Layout& L, std::vector<uint32_t>& buf, int W, int H) {
    float s = L.s;
    int oy = L.ch - H;  // client y of row 0
    Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)buf.data());
    Graphics gr(&bmp);
    InitGraphics(gr);

    // bottom gradient for legibility
    int gradH = std::min(H, (int)(L.ch - L.trackY + 96 * s));
    LinearGradientBrush grad(Point(0, H - gradH - 1), Point(0, H), Color(0, 0, 0, 0), Color(185, 0, 0, 0));
    gr.FillRectangle(&grad, 0, H - gradH, W, gradH);

    Font font(UiFont(), std::round(13 * s), FontStyleRegular, UnitPixel);
    Font fontSmall(UiFont(), std::round(12 * s), FontStyleRegular, UnitPixel);
    Font fontBold(UiFont(), std::round(12 * s), FontStyleBold, UnitPixel);
    SolidBrush white(Color(255, 255, 255, 255));
    SolidBrush soft(Color(215, 255, 255, 255));
    SolidBrush dim(Color(110, 255, 255, 255));
    SolidBrush accent(kAccent);
    StringFormat center; center.SetAlignment(StringAlignmentCenter); center.SetLineAlignment(StringAlignmentCenter);
    StringFormat leftFmt; leftFmt.SetAlignment(StringAlignmentNear); leftFmt.SetLineAlignment(StringAlignmentCenter);
    leftFmt.SetFormatFlags(StringFormatFlagsNoWrap); leftFmt.SetTrimming(StringTrimmingEllipsisCharacter);
    StringFormat rightFmt; rightFmt.SetAlignment(StringAlignmentFar); rightFmt.SetLineAlignment(StringAlignmentCenter);
    rightFmt.SetFormatFlags(StringFormatFlagsNoWrap);

    if (!g.hasFile) {
        std::wstring msg = g.errorText.empty() ? L"Drop a video here or press Ctrl+O" : g.errorText;
        RectF r(0, 0, (REAL)W, (REAL)(L.barTop - oy));
        gr.DrawString(msg.c_str(), -1, &font, r, &center, &dim);
    }

    // timeline (thin)
    bool hovTl = (IsTl(g.hover) || g.drag == H_TIMELINE);
    REAL th = (hovTl ? 3.0f : 2.0f) * s;
    REAL ty = L.trackY - oy - th / 2;
    REAL tl = (REAL)L.trackL, tr = (REAL)L.trackR;
    FillRound(gr, SolidBrush(Color(75, 255, 255, 255)), tl, ty, tr - tl, th, th / 2);
    // at the end mpv reports the last frame's start time, which is short of the duration
    double cur = (g.drag == H_TIMELINE) ? g.dragTime : (AtEnd() ? g.duration : g.timePos);
    double frac = g.duration > 0 ? std::clamp(cur / g.duration, 0.0, 1.0) : 0;
    REAL px = tl + (REAL)((tr - tl) * frac);
    if (hovTl && g.duration > 0 && g.drag != H_TIMELINE) {
        REAL hx = (REAL)std::clamp(g.mouseX, L.trackL, L.trackR);
        if (hx > px) FillRound(gr, SolidBrush(Color(60, 255, 255, 255)), px, ty, hx - px, th, th / 2);
    }
    if (g.trimMode && g.duration > 0) {
        // selection band; a fainter band shows the extra part kept because the copy starts at a keyframe
        REAL ax = (REAL)L.trimAX, bx = (REAL)L.trimBX, bandH = 4 * s, by = L.trackY - oy - bandH / 2;
        if (g.trimKeyA >= 0 && g.trimKeyA < g.trimA - 0.0005) {
            REAL kx = tl + (REAL)((tr - tl) * std::clamp(g.trimKeyA / g.duration, 0.0, 1.0));
            FillRound(gr, SolidBrush(Color(110, 76, 160, 255)), kx, by, std::max(1.0f, ax - kx), bandH, 0);
            Pen tick(Color(200, 76, 160, 255), 1.0f * s);
            gr.DrawLine(&tick, kx, by - 4 * s, kx, by + bandH + 4 * s);
        }
        if (bx > ax) FillRound(gr, SolidBrush(kTrim), ax, by, bx - ax, bandH, 0);
    } else if (px > tl) FillRound(gr, accent, tl, ty, px - tl, th, th / 2);
    if (!g.trimMode) {   // no playhead knob while trimming: it only gets in the way of the handles
        REAL kr = (hovTl ? 8.0f : 7.0f) * s;
        REAL ky = L.trackY - oy;
        SolidBrush knobFill(Color(255, 24, 24, 24));
        gr.FillEllipse(&knobFill, px - kr, ky - kr, kr * 2, kr * 2);
        Pen ring(kAccent, 2.0f * s);
        gr.DrawEllipse(&ring, px - kr + 1 * s, ky - kr + 1 * s, kr * 2 - 2 * s, kr * 2 - 2 * s);
    }

    // trim handles with their exact times above them
    bool hours = g.duration >= 3600;
    if (g.trimMode && g.duration > 0) {
        // whole, even pixel sizes so the handles' edges sit exactly on pixels
        auto even = [](REAL v) { return 2 * std::round(v / 2); };
        REAL hw = even(4 * s), hh = even(24 * s), hy = (REAL)(L.trackY - oy) - hh / 2;
        REAL labelH = std::round(20 * s), ly = std::round(hy - labelH - 4 * s);
        RectF la, lbb;
        std::wstring ta = FormatTimeMs(g.trimA, hours), tb = FormatTimeMs(g.trimB, hours);
        gr.MeasureString(ta.c_str(), -1, &fontSmall, PointF(0, 0), &la);
        gr.MeasureString(tb.c_str(), -1, &fontSmall, PointF(0, 0), &lbb);
        REAL wa = la.Width + 12 * s, wb = lbb.Width + 12 * s;
        REAL xa = std::clamp((REAL)L.trimAX - wa / 2, 2 * s, W - wa - 2 * s);
        REAL xb = std::clamp((REAL)L.trimBX - wb / 2, 2 * s, W - wb - 2 * s);
        if (xb < xa + wa + 4 * s) {   // keep the labels apart when the handles are close
            REAL mid = (xa + wa + xb) / 2;
            xa = std::max(2 * s, mid - 2 * s - wa);
            xb = std::min(W - wb - 2 * s, mid + 2 * s);
        }
        xa = std::round(xa); xb = std::round(xb); wa = std::round(wa); wb = std::round(wb);
        auto handle = [&](int x, Hit h, REAL lx, REAL lw, const std::wstring& t) {
            bool act = g.hover == h || g.drag == h;
            REAL w = act ? hw + even(2 * s) : hw;
            REAL o = std::max(1.0f, std::round(s));   // dark outline keeps it visible on bright video
            FillRound(gr, SolidBrush(Color(255, 24, 24, 24)), x - w / 2 - o, hy - o, w + 2 * o, hh + 2 * o, 2 * s);
            FillRound(gr, SolidBrush(act ? Color(255, 255, 255, 255) : kTrim), x - w / 2, hy, w, hh, 2 * s);
            FillRound(gr, SolidBrush(Color(act ? 235 : 200, 20, 20, 20)), lx, ly, lw, labelH, labelH / 2);
            gr.DrawString(t.c_str(), -1, &fontSmall, RectF(lx, ly, lw, labelH), &center, act ? &white : &soft);
        };
        handle(L.trimAX, H_TRIM_A, xa, wa, ta);
        handle(L.trimBX, H_TRIM_B, xb, wb, tb);

        // length of the copy, and where it really starts if that's earlier
        double start = g.trimKeyA >= 0 ? g.trimKeyA : g.trimA;
        wchar_t info[160];
        if (g.trimKeyA >= 0 && g.trimKeyA < g.trimA - 0.0005)
            swprintf(info, 160, L"%.3f s · starts at keyframe %ls", g.trimB - start, FormatTimeMs(start, hours).c_str());
        else
            swprintf(info, 160, L"%.3f s", g.trimB - start);
        REAL y = (REAL)(L.trackY - oy) + 9 * s;
        gr.DrawString(info, -1, &fontSmall, RectF(tl + 90 * s, y, std::max(0.0f, tr - tl - 180 * s), 16 * s), &center, &soft);
    }

    // times under the two ends of the timeline
    {
        REAL y = (REAL)(L.trackY - oy) + 9 * s, hgt = 16 * s;
        std::wstring a = FormatTime(cur, hours), b = FormatTime(g.duration, hours);
        gr.DrawString(a.c_str(), -1, &fontSmall, RectF(tl, y, 120 * s, hgt), &leftFmt, &soft);
        gr.DrawString(b.c_str(), -1, &fontSmall, RectF(tr - 120 * s, y, 120 * s, hgt), &rightFmt, &soft);
    }

    DrawBottomIcons(gr, L, oy);

    if (!g.toast.empty()) {
        RectF lb;
        gr.MeasureString(g.toast.c_str(), -1, &font, PointF(0, 0), &lb);
        REAL tw = lb.Width + 24 * s, thh = 30 * s;
        REAL tx, tyy;
        if (g.toastRight > 0) {   // to the left of its button, on the button row
            tx = std::max(4 * s, (REAL)g.toastRight - 8 * s - tw);
            tyy = (REAL)(L.rowY - oy) - thh / 2;
        } else {
            tx = (W - tw) / 2;
            tyy = (REAL)(L.trackY - oy) - 110 * s;
        }
        if (tyy < 0) tyy = 0;
        FillRound(gr, SolidBrush(Color(225, 20, 20, 20)), tx, tyy, tw, thh, 2 * s);
        gr.DrawString(g.toast.c_str(), -1, &font, RectF(tx, tyy, tw, thh), &center, &white);
    }

    // hover preview above the timeline
    if (hovTl && g.duration > 0) {
        int mx = g.mouseX;
        double ht = TimeAtX(L, mx);
        std::wstring label = FormatTime(ht, hours);
        REAL cxp = (REAL)std::clamp(mx, L.trackL, L.trackR);
        ThumbPtr tp = g.shownThumb;
        REAL labelH = 22 * s;
        REAL bottom = (REAL)(L.trackY - oy) - 16 * s;
        if (tp) {
            REAL bw = (REAL)L.thumbW, bh = (REAL)L.thumbH;
            bool sideways = g.rotate == 90 || g.rotate == 270;
            REAL iw = (REAL)(sideways ? tp->h : tp->w), ih = (REAL)(sideways ? tp->w : tp->h);
            REAL sc = std::min(bw / iw, bh / ih);
            REAL dw = iw * sc, dh = ih * sc;
            REAL x0 = std::clamp(cxp - dw / 2, 6 * s, W - dw - 6 * s);
            REAL y0 = std::max(0.0f, bottom - dh);
            Bitmap tb0(tp->w, tp->h, tp->w * 4, PixelFormat32bppRGB, (BYTE*)tp->px.data());
            std::unique_ptr<Bitmap> rotated;
            Bitmap* tbp = &tb0;
            if (g.rotate) {   // previews follow the view rotation
                rotated.reset(tb0.Clone(0, 0, tp->w, tp->h, PixelFormat32bppRGB));
                rotated->RotateFlip(g.rotate == 90 ? Rotate90FlipNone : g.rotate == 180 ? Rotate180FlipNone : Rotate270FlipNone);
                tbp = rotated.get();
            }
            Bitmap& tb = *tbp;
            GraphicsPath clip;
            RoundRectPath(clip, x0, y0, dw, dh, 3 * s);
            gr.SetClip(&clip);
            gr.SetInterpolationMode(sc > 0.99f && sc < 1.01f ? InterpolationModeNearestNeighbor : InterpolationModeBilinear);
            gr.DrawImage(&tb, RectF(x0, y0, dw, dh), 0, 0, iw, ih, UnitPixel);
            gr.ResetClip();
            Pen border(Color(170, 255, 255, 255), 1.0f * s);
            gr.DrawPath(&border, &clip);
            RectF lb;
            gr.MeasureString(label.c_str(), -1, &fontBold, PointF(0, 0), &lb);
            REAL lw = lb.Width + 14 * s;
            REAL lx = x0 + (dw - lw) / 2, ly = y0 + dh - labelH - 5 * s;
            FillRound(gr, SolidBrush(Color(200, 0, 0, 0)), lx, ly, lw, labelH, labelH / 2);
            gr.DrawString(label.c_str(), -1, &fontBold, RectF(lx, ly, lw, labelH), &center, &white);
        } else {
            RectF lb;
            gr.MeasureString(label.c_str(), -1, &fontBold, PointF(0, 0), &lb);
            REAL lw = lb.Width + 16 * s;
            REAL lx = std::clamp(cxp - lw / 2, 6 * s, W - lw - 6 * s), ly = bottom - labelH - 2 * s;
            FillRound(gr, SolidBrush(Color(225, 30, 30, 30)), lx, ly, lw, labelH, 6 * s);
            gr.DrawString(label.c_str(), -1, &fontBold, RectF(lx, ly, lw, labelH), &center, &white);
        }
    }
}

// Caption buttons in the top-right corner (min / max / close, or exit-fullscreen / close).
static void ComposeTopBox(const Layout& L, std::vector<uint32_t>& buf, int W, int H) {
    float s = L.s;
    Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)buf.data());
    Graphics gr(&bmp);
    InitGraphics(gr);
    SolidBrush bg(Color(255, 0, 0, 0));
    gr.FillRectangle(&bg, 0, 0, W, H);
    int ox = L.topBox.left;
    Color glyph = g.active || g.fullscreen ? Color(255, 255, 255, 255) : Color(150, 255, 255, 255);
    auto button = [&](const RECT& r, Hit h, int kind) {
        REAL x = (REAL)(r.left - ox), w = (REAL)(r.right - r.left);
        bool hov = g.hover == h, prs = hov && g.pressed == h;
        if (hov) {
            Color c = kind == 0 ? Color(255, 196, 43, 28) : Color(prs ? 50 : 32, 255, 255, 255);
            if (kind == 0 && prs) c = Color(255, 160, 35, 24);
            SolidBrush b(c);
            gr.FillRectangle(&b, x, 0.0f, w, (REAL)H);
        }
        REAL cx = x + w / 2, cy = H / 2.0f, a = 5 * s;
        static const wchar_t glyphs[] = {G_CLOSE, G_MINIMIZE, G_MAXIMIZE, G_RESTORE, G_BACKTOWINDOW};
        if (DrawGlyph(gr, glyphs[kind], cx, cy, (kind == 4 ? 12 : 10) * s, hov && kind == 0 ? Color(255, 255, 255, 255) : glyph)) return;
        Pen pen(glyph, 1.0f * s);
        gr.SetSmoothingMode(kind == 0 || kind == 4 ? SmoothingModeAntiAlias : SmoothingModeNone);
        switch (kind) {
        case 0: gr.DrawLine(&pen, cx - a, cy - a, cx + a, cy + a); gr.DrawLine(&pen, cx + a, cy - a, cx - a, cy + a); break;
        case 1: gr.DrawLine(&pen, cx - a, cy, cx + a, cy); break;
        case 2: gr.DrawRectangle(&pen, cx - a, cy - a, 2 * a, 2 * a); break;
        case 3: {
            REAL o = 2 * s;
            gr.DrawRectangle(&pen, cx - a, cy - a + o, 2 * a - o, 2 * a - o);
            gr.DrawLine(&pen, cx - a + o, cy - a, cx + a, cy - a);
            gr.DrawLine(&pen, cx + a, cy - a, cx + a, cy + a - o);
            break;
        }
        case 4: {  // exit fullscreen: arrows pointing inward
            REAL b = 1.5f * s, aa = 6 * s, hh = 3.5f * s;
            for (int d = -1; d <= 1; d += 2) {
                REAL oxx = cx + d * aa, oyy = cy - d * aa, ix = cx + d * b, iy = cy - d * b;
                gr.DrawLine(&pen, ix, iy, oxx, oyy);
                gr.DrawLine(&pen, ix, iy, ix + d * hh, iy);
                gr.DrawLine(&pen, ix, iy, ix, iy - d * hh);
            }
            break;
        }
        }
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
    };
    button(L.closeBtn, H_CLOSE, 0);
    if (g.fullscreen) {
        button(L.exitFsBtn, H_EXITFS, 4);
    } else {
        button(L.maxBtn, H_MAX, IsZoomed(g.hwnd) ? 3 : 2);
        button(L.minBtn, H_MIN, 1);
    }
}

// Chevron for previous (dir -1) / next (dir +1) video.
static void ComposeSide(const Layout& L, std::vector<uint32_t>& buf, int W, int H, int dir) {
    float s = L.s;
    Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)buf.data());
    Graphics gr(&bmp);
    InitGraphics(gr);
    bool hov = g.hover == (dir < 0 ? H_PREV : H_NEXT);
    // flush with the screen edge: the outer corners are pushed off-bitmap, inner ones barely rounded
    REAL r = 2 * s, pw = (REAL)W, ph = (REAL)H, x = 0, y = 0;
    // backdrop only while hovered; otherwise just the chevron (its shadow keeps it readable)
    if (hov) FillRound(gr, SolidBrush(Color(56, 255, 255, 255)), dir < 0 ? -r : 0, y, pw + r, ph, r);
    Pen pen(Color(255, 255, 255, 255), 1.5f * s);
    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
    REAL cx = x + pw / 2, cy = H / 2.0f, a = 5 * s, b = 10 * s;
    // soft shadow keeps the white chevron readable on bright video now that the backdrop is light
    if (DrawGlyph(gr, dir < 0 ? G_CHEVRON_LEFT : G_CHEVRON_RIGHT, cx + 1 * s, cy + 1 * s, 20 * s, Color(110, 0, 0, 0))) {
        DrawGlyph(gr, dir < 0 ? G_CHEVRON_LEFT : G_CHEVRON_RIGHT, cx, cy, 20 * s, Color(255, 255, 255, 255));
        return;
    }
    // "<" for previous, ">" for next
    PointF pts[3] = {PointF(cx - dir * a, cy - b), PointF(cx + dir * a, cy), PointF(cx - dir * a, cy + b)};
    gr.DrawLines(&pen, pts, 3);
}

static void ComposeOverlay() {
    Layout L = ComputeLayout();
    if (L.cw <= 0 || L.ch <= 0) return;
    int W = L.cw;

    std::vector<uint32_t> bb((size_t)W * L.ovH, 0);
    ComposeBottom(L, bb, W, L.ovH);

    int tw = L.topBox.right - L.topBox.left, th = L.btnH;
    std::vector<uint32_t> tb;
    bool topVis = tw > 0 && th > 0 && th <= L.ch;
    if (topVis) { tb.assign((size_t)tw * th, 0); ComposeTopBox(L, tb, tw, th); }

    int sw = L.prev.right - L.prev.left, sh = L.prev.bottom - L.prev.top;
    bool sideVis = g.hasFile && sw > 0 && sh > 0;
    std::vector<uint32_t> lb, rb;
    if (sideVis) {
        lb.assign((size_t)sw * sh, 0); ComposeSide(L, lb, sw, sh, -1);
        rb.assign((size_t)sw * sh, 0); ComposeSide(L, rb, sw, sh, +1);
    }
    {
        std::lock_guard<std::mutex> l(g.rm);
        auto put = [&](int i, std::vector<uint32_t>& px, int x, int y, int w, int h, bool vis) {
            Overlay& o = g.ov[i];
            o.px.swap(px);
            o.x = x; o.y = y; o.w = w; o.h = h;
            o.version++;
            o.visible = vis;
            o.alpha = g.alpha[i];
        };
        put(OV_BOTTOM, bb, 0, L.ch - L.ovH, W, L.ovH, true);
        put(OV_TOP, tb, L.topBox.left, 0, tw, th, topVis);
        put(OV_LEFT, lb, L.prev.left, L.prev.top, sw, sh, sideVis);
        put(OV_RIGHT, rb, L.next.left, L.next.top, sw, sh, sideVis);
        g.rRedraw = true;
    }
    g.rcv.notify_one();
}

static bool BottomActiveHit(Hit h) {
    return h == H_TIMELINE || h == H_SCRUB || h == H_PLAY || h == H_MUTE || h == H_VOLUME || h == H_REPEAT || h == H_ROTATE ||
           h == H_FULLSCREEN || h == H_BAR || h == H_TRIM || h == H_TRIM_A || h == H_TRIM_B || h == H_TRIM_SAVE;
}
// Fade animation (UI thread timer). Fade in fast, fade out slower.
static void PushAlphas() {
    {
        std::lock_guard<std::mutex> l(g.rm);
        for (int i = 0; i < OV_COUNT; i++) g.ov[i].alpha = g.alpha[i];
        g.rRedraw = true;
    }
    g.rcv.notify_one();
}
static void StepFade() {
    bool want[OV_COUNT] = {g.showBottom, g.showTop, g.showSides, g.showSides};
    DWORD now = GetTickCount();
    float dt = std::min(100.0f, (float)(now - g.fadeTick));
    g.fadeTick = now;
    bool moving = false;
    for (int i = 0; i < OV_COUNT; i++) {
        float target = want[i] ? 1.0f : 0.0f;
        float step = dt / (want[i] ? 110.0f : 280.0f);
        float a = g.alpha[i];
        a = a < target ? std::min(target, a + step) : std::max(target, a - step);
        g.alpha[i] = a;
        if (a != target) moving = true;
    }
    PushAlphas();
    if (!moving) KillTimer(g.hwnd, TIMER_FADE);
}
static void StartFade() {
    g.fadeTick = GetTickCount() - 16;
    SetTimer(g.hwnd, TIMER_FADE, 15, nullptr);
    StepFade();
}
// Decide what should be visible from the mouse position / activity, then fade towards it.
static void UpdateVisibility() {
    Layout L = ComputeLayout();
    bool in = g.mouseInside && g.mouseX >= 0 && g.mouseY >= 0;
    int zone = std::max(L.ovH, (int)(160 * L.s));
    Hit raw = in ? HitTestRaw(L, g.mouseX, g.mouseY, true, true, true) : H_NONE;
    bool nearBottom = in && g.mouseY >= L.ch - zone && raw != H_PREV && raw != H_NEXT;
    bool flashing = (LONG)(g.flashUntil - GetTickCount()) > 0;

    bool ended = AtEnd() && g.repeat != REPEAT_ONE;   // finished and not looping: keep the timeline up
    bool bottom = !g.hasFile || ended || g.drag != H_NONE || flashing || !g.toast.empty() || g.trimMode ||
                  (nearBottom && g.recentActivity) || (nearBottom && BottomActiveHit(raw));
    bool sides = g.hasFile && in && (g.recentActivity || raw == H_PREV || raw == H_NEXT);
    bool top = !g.fullscreen || (in && In(L.topBox, g.mouseX, g.mouseY)) || g.pressed != H_NONE;

    if (bottom != g.showBottom || sides != g.showSides || top != g.showTop) {
        g.showBottom = bottom; g.showSides = sides; g.showTop = top;
        if (in) g.hover = HitTest(L, g.mouseX, g.mouseY);
        ComposeOverlay();
        StartFade();
    }
}
static void PokeActivity() {  // the mouse moved
    g.cursorHidden = false;
    g.recentActivity = true;
    SetTimer(g.hwnd, TIMER_HIDE, 2000, nullptr);
    UpdateVisibility();
}
static void FlashBottom(DWORD ms = 2000) {  // show the timeline for a moment, then fade out
    g.flashUntil = GetTickCount() + ms;
    SetTimer(g.hwnd, TIMER_FLASH, ms + 10, nullptr);
    UpdateVisibility();
}
static void ShowToast(const std::wstring& text, int anchorRight = 0) {
    g.toast = text;
    g.toastRight = anchorRight;
    SetTimer(g.hwnd, TIMER_TOAST, 1800, nullptr);
    ComposeOverlay();
    UpdateVisibility();
}

// ---------------------------------------------------------------------------------------------
// Player actions
// ---------------------------------------------------------------------------------------------
static void Cmd(std::initializer_list<const char*> args) {
    std::vector<const char*> v(args);
    v.push_back(nullptr);
    M.command_async(g.mpv, 0, v.data());
}
static void UpdateTitle() {
    std::wstring t = L"Vysion";
    if (!g.file.empty()) t = std::wstring(PathFindFileNameW(g.file.c_str())) + L" - Vysion";
    SetWindowTextW(g.hwnd, t.c_str());
}
static void OpenFile(const std::wstring& pathIn) {
    std::wstring path = pathIn;
    {
        wchar_t lp[32768];
        DWORD n = GetLongPathNameW(pathIn.c_str(), lp, 32768);  // expand 8.3 names
        if (n > 0 && n < 32768) path = lp;
    }
    g.file = path;
    g.eofHandled = false;
    if (!g.trimBusy) { g.trimMode = false; g.trimProbeGen++; g.rotateBeforeTrim = 0; }
    if (g.rotate) { g.rotate = 0; Cmd({"set", "video-rotate", "0"}); }
    g.eof = false;
    FlashBottom(2000);
    g.errorText.clear();
    g.timePos = 0;
    g.duration = 0;
    g.shownThumb.reset();
    std::string u = ToUtf8(path);
    Cmd({"loadfile", u.c_str(), "replace"});
    Cmd({"set", "pause", "no"});
    {
        std::lock_guard<std::mutex> l(g.th.m);
        g.th.newPath = u;
        g.th.hasNewPath = true;
        g.th.enabled = false;
        g.th.baseWidth = (int)std::lround(200 * g.dpi / 96.0);
    }
    g.th.cv.notify_all();
    UpdateTitle();
    ComposeOverlay();
}
static void TogglePause() {
    if (!g.hasFile) return;
    int p = g.paused ? 0 : 1;
    M.set_property(g.mpv, "pause", MPV_FORMAT_FLAG, &p);
}
static void SeekRel(double secs) {
    if (!g.hasFile) return;
    char b[32]; snprintf(b, sizeof b, "%.3f", secs);
    Cmd({"seek", b, "relative+exact"});
}
static void SeekAbs(double t, bool exact) {
    if (!g.hasFile) return;
    char b[32]; snprintf(b, sizeof b, "%.3f", t);
    Cmd({"seek", b, exact ? "absolute+exact" : "absolute+keyframes"});
}
static void SetVolume(double v) {
    v = std::clamp(v, 0.0, 100.0);
    g.volume = v;
    M.set_property(g.mpv, "volume", MPV_FORMAT_DOUBLE, &v);
    if (g.mute && v > 0) { int m = 0; M.set_property(g.mpv, "mute", MPV_FORMAT_FLAG, &m); }
    ComposeOverlay();
}
static void ApplyRepeat() {
    Cmd({"set", "loop-file", g.repeat == REPEAT_ONE ? "inf" : "no"});
}
static void CycleRepeat() {
    g.repeat = (g.repeat + 1) % 3;
    ApplyRepeat();
    g.cfg.repeat = g.repeat;
    g.cfg.SavePlayer();
    static const wchar_t* names[] = {L"Don't repeat", L"Repeat this video", L"Play next video automatically"};
    ShowToast(names[g.repeat], ComputeLayout().repeat.left);
}
// Rotate the picture for viewing only (done on the GPU while drawing; the file is untouched).
static void SetRotation(int deg) {
    g.rotate = ((deg % 360) + 360) % 360;
    char b[8];
    snprintf(b, sizeof b, "%d", g.rotate);
    Cmd({"set", "video-rotate", b});
    if (g.hasFile) ShowToast(g.rotate ? L"Rotated " + std::to_wstring(g.rotate) + L"\u00B0" : L"Original orientation", ComputeLayout().rotate.left);
    ComposeOverlay();
}
static void ToggleMute() {
    int m = g.mute ? 0 : 1;
    if (!m && g.volume <= 0) SetVolume(30);
    M.set_property(g.mpv, "mute", MPV_FORMAT_FLAG, &m);
}
static void ToggleFullscreen() {
    HWND h = g.hwnd;
    if (!g.fullscreen) {
        g.savedPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(h, &g.savedPlacement);
        g.savedStyle = GetWindowLongW(h, GWL_STYLE);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, (g.savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        g.fullscreen = true;
        // One pixel taller than the monitor: a window that exactly covers the screen with a GL
        // swap chain gets promoted to "fullscreen exclusive / independent flip", which causes the
        // black flash on entering fullscreen and makes the NVIDIA overlay treat us as a game.
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top + 1, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        g.fullscreen = false;
        SetWindowLongW(h, GWL_STYLE, g.savedStyle);
        SetWindowPlacement(h, &g.savedPlacement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    ComposeOverlay();
}
// The order the files are shown in the Explorer window the video was opened from (whatever sort /
// grouping the user picked there). Looks at every open Explorer window showing this folder (or a
// search/library view containing the file) and returns its items in view order.
static bool ExplorerOrder(const std::wstring& folder, const std::wstring& file, std::vector<std::wstring>& out) {
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw))) || !sw) return false;
    std::wstring folderNoSlash = folder;
    while (folderNoSlash.size() > 3 && (folderNoSlash.back() == L'\\' || folderNoSlash.back() == L'/')) folderNoSlash.pop_back();
    long n = 0;
    sw->get_Count(&n);
    int bestScore = 0;
    std::vector<std::wstring> best;
    for (long i = 0; i < n; i++) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        IDispatch* d = nullptr;
        if (sw->Item(v, &d) != S_OK || !d) continue;
        IServiceProvider* sp = nullptr;
        IShellBrowser* sb = nullptr;
        IShellView* sv = nullptr;
        IFolderView* fv = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&sp))) &&
            SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) &&
            SUCCEEDED(sb->QueryActiveShellView(&sv)) && SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv)))) {
            std::wstring fpath;
            IPersistFolder2* pf = nullptr;
            if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&pf))) && pf) {
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(pf->GetCurFolder(&pidl)) && pidl) {
                    wchar_t buf[32768];
                    if (SHGetPathFromIDListEx(pidl, buf, 32768, GPFIDL_DEFAULT)) fpath = buf;
                    CoTaskMemFree(pidl);
                }
                pf->Release();
            }
            while (fpath.size() > 3 && fpath.back() == L'\\') fpath.pop_back();
            bool same = !fpath.empty() && _wcsicmp(fpath.c_str(), folderNoSlash.c_str()) == 0;
            // same folder, or a virtual view (search results, library) that may contain the file
            if (same || fpath.empty()) {
                IShellItemArray* arr = nullptr;
                if (SUCCEEDED(fv->Items(SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER, IID_PPV_ARGS(&arr))) && arr) {
                    DWORD cnt = 0;
                    arr->GetCount(&cnt);
                    std::vector<std::wstring> list;
                    bool hasFile = false;
                    for (DWORD k = 0; k < cnt && k < 100000; k++) {
                        IShellItem* it = nullptr;
                        if (FAILED(arr->GetItemAt(k, &it)) || !it) continue;
                        PWSTR nm = nullptr;
                        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &nm)) && nm) {
                            std::wstring pth = nm;
                            CoTaskMemFree(nm);
                            bool isCur = _wcsicmp(pth.c_str(), file.c_str()) == 0;
                            if (isCur) hasFile = true;
                            if (isCur || IsVideoFile(pth)) list.push_back(pth);
                        }
                        it->Release();
                    }
                    int score = hasFile ? (same ? 2 : 1) : 0;
                    if (score > bestScore) { bestScore = score; best.swap(list); }
                    arr->Release();
                }
            }
        }
        if (fv) fv->Release();
        if (sv) sv->Release();
        if (sb) sb->Release();
        if (sp) sp->Release();
        d->Release();
    }
    sw->Release();
    if (!bestScore) return false;
    out.swap(best);
    return true;
}

// When Vysion is the default app ("Always use this app"), Explorer uses the key
// HKCU\Software\Classes\Applications\<exe> for the file type. Give it a plain video-file icon
// and an empty TypeOverlay, so thumbnails don't get the app icon stamped in their corner.
static void SetRegString(HKEY root, const std::wstring& sub, const wchar_t* name, const std::wstring& val, bool& changed) {
    HKEY k;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    wchar_t cur[1024];
    DWORD type = 0, size = sizeof(cur);
    bool same = RegQueryValueExW(k, name, nullptr, &type, (BYTE*)cur, &size) == ERROR_SUCCESS && type == REG_SZ &&
                std::wstring(cur, size / sizeof(wchar_t)).c_str() == val;
    if (!same) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
        changed = true;
    }
    RegCloseKey(k);
}
static void RegisterFileTypeLook() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring app = std::wstring(L"Software\\Classes\\Applications\\") + PathFindFileNameW(exe);
    bool changed = false;
    SetRegString(HKEY_CURRENT_USER, app, L"TypeOverlay", L"", changed);
    SetRegString(HKEY_CURRENT_USER, app, L"FriendlyAppName", L"Vysion", changed);
    SetRegString(HKEY_CURRENT_USER, app + L"\\DefaultIcon", nullptr, std::wstring(L"\"") + exe + L"\",1", changed);
    if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Remembered order so next/previous keep following Explorer's order even after its window is closed.
static std::vector<std::wstring> gOrder;
static bool OrderContains(const std::vector<std::wstring>& v, const std::wstring& f) {
    for (auto& x : v) if (_wcsicmp(x.c_str(), f.c_str()) == 0) return true;
    return false;
}
static void SnapshotExplorerOrder() {
    if (g.file.empty()) return;
    std::wstring folder = g.file.substr(0, g.file.find_last_of(L"\\/") + 1);
    std::vector<std::wstring> v;
    if (ExplorerOrder(folder, g.file, v)) gOrder.swap(v);
}

// ---------------------------------------------------------------------------------------------
// Trimming: a bundled ffmpeg.exe (stream copy only) cuts without re-encoding. That can only start
// at a keyframe, so the start snaps back to the keyframe at or before the chosen time; the timeline
// shows that. Runs on worker threads; results come back as WM_APP_TRIM.
// ---------------------------------------------------------------------------------------------
struct TrimResult {
    int kind = 0;           // 1 = keyframe found, 2 = save finished
    int gen = 0;
    double keyA = -1;
    bool ok = false;
    std::wstring path;      // saved copy
};
static std::wstring ExeDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe;
    return d.substr(0, d.find_last_of(L"\\/") + 1);
}
static std::wstring FfmpegPath() { return ExeDir() + L"ffmpeg.exe"; }
// Runs a command line without a console window; returns the exit code (-1 if it couldn't start).
static int RunHidden(const std::wstring& cmdline, std::string* out) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    BOOL started = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                  nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!started) { CloseHandle(rd); return -1; }
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n)
        if (out && out->size() < (1u << 20)) out->append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}
static std::wstring Secs(double t) { wchar_t b[32]; swprintf(b, 32, L"%.3f", std::max(0.0, t)); return b; }

// Where will a copy starting at `a` really begin? Asks ffmpeg for the first video packet it would copy:
// its timestamp is relative to `a` (zero or negative), so keyframe = a + pts.
static void ProbeKeyframe(double a) {
    int gen = ++g.trimProbeGen;
    if (a <= 0.0005) { g.trimKeyA = 0; return; }
    g.trimKeyA = -1;
    std::wstring file = g.file, ff = FfmpegPath();
    HWND hwnd = g.hwnd;
    std::thread([=] {
        std::string out;
        int rc = RunHidden(L"\"" + ff + L"\" -hide_banner -v error -nostdin -ss " + Secs(a) + L" -i \"" + file +
                           L"\" -map 0:v:0 -c copy -frames:v 1 -avoid_negative_ts disabled -f framecrc -", &out);
        auto* r = new TrimResult;
        r->kind = 1; r->gen = gen; r->keyA = a;   // no video / no answer: the cut starts where asked
        long long num = 0, den = 0;
        const char* tb = strstr(out.c_str(), "#tb 0:");
        if (rc == 0 && tb && sscanf(tb, "#tb 0: %lld/%lld", &num, &den) == 2 && num > 0 && den > 0) {
            size_t pos = 0;
            while (pos < out.size()) {   // first packet line: "0, dts, pts, duration, size, crc"
                size_t e = out.find('\n', pos);
                std::string line = out.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
                pos = e == std::string::npos ? out.size() : e + 1;
                long long dts = 0, pts = 0;
                int idx = 0;
                if (!line.empty() && line[0] != '#' && sscanf(line.c_str(), "%d, %lld, %lld", &idx, &dts, &pts) == 3) {
                    double off = (double)pts * num / den;
                    if (off <= 0.0005 && off > -600) r->keyA = std::max(0.0, a + off);
                    break;
                }
            }
        }
        if (!PostMessageW(hwnd, WM_APP_TRIM, 0, (LPARAM)r)) delete r;
    }).detach();
}
static void SetTrimMode(bool on) {
    if (on && (!g.hasFile || g.duration <= 0)) return;
    if (on == g.trimMode) return;
    if (on && GetFileAttributesW(FfmpegPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        ShowToast(L"Trimming needs ffmpeg.exe next to Vysion.exe");
        return;
    }
    g.trimMode = on;
    if (on) {
        // trimming works on the file as it is: show the original orientation, and pause
        g.rotateBeforeTrim = g.rotate;
        if (g.rotate) { g.rotate = 0; Cmd({"set", "video-rotate", "0"}); }
        if (!g.paused) TogglePause();
        g.trimA = 0; g.trimB = g.duration;
        ProbeKeyframe(g.trimA);
        ShowToast(L"Drag the handles, or press I / O at the playhead");
    } else {
        g.trimProbeGen++;
        if (g.rotateBeforeTrim && !g.rotate) {   // bring back the rotation the user had
            g.rotate = g.rotateBeforeTrim;
            char b[8];
            snprintf(b, sizeof b, "%d", g.rotate);
            Cmd({"set", "video-rotate", b});
        }
        g.rotateBeforeTrim = 0;
    }
    ComposeOverlay();
    UpdateVisibility();
}
static void SetTrimPoint(bool start, double t) {
    if (!g.trimMode) return;
    const double minLen = 0.05;
    t = std::clamp(t, 0.0, g.duration);
    if (start) {
        g.trimA = std::min(t, std::max(0.0, g.trimB - minLen));
        ProbeKeyframe(g.trimA);
    } else {
        g.trimB = std::max(t, std::min(g.duration, g.trimA + minLen));
    }
    ComposeOverlay();
}
// "name (trim).ext", or "name (trim 2).ext" ... if taken
static std::wstring TrimOutputPath(const std::wstring& in, const wchar_t* forceExt = nullptr) {
    size_t slash = in.find_last_of(L"\\/");
    size_t dot = in.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) dot = in.size();
    std::wstring base = in.substr(0, dot), ext = forceExt ? std::wstring(forceExt) : in.substr(dot);
    for (int i = 1; i < 1000; i++) {
        std::wstring p = base + (i == 1 ? std::wstring(L" (trim)") : L" (trim " + std::to_wstring(i) + L")") + ext;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    }
    return base + L" (trim copy)" + ext;
}
static void SaveTrimmedCopy() {
    if (!g.trimMode || g.trimBusy || g.file.empty()) return;
    g.trimBusy = true;
    std::wstring in = g.file, out = TrimOutputPath(in), mkv = TrimOutputPath(in, L".mkv"), ff = FfmpegPath();
    double a = g.trimA, b = g.trimB;
    bool whole = b >= g.duration - 0.0005;
    HWND hwnd = g.hwnd;
    std::thread([=] {
        std::wstring head = L"\"" + ff + L"\" -hide_banner -v error -nostdin -y" + (a > 0.0005 ? L" -ss " + Secs(a) : L"") +
                            (whole ? L"" : L" -to " + Secs(b)) + L" -i \"" + in + L"\"";
        auto tail = [](const std::wstring& o) { return L" -c copy -avoid_negative_ts make_zero -map_metadata 0 \"" + o + L"\""; };
        // everything first (subtitles too); if the container refuses a stream, just video + audio;
        // if this format can't be written at all, Matroska takes almost anything
        std::wstring path = out;
        int rc = RunHidden(head + L" -map 0 -dn" + tail(out), nullptr);
        if (rc != 0) rc = RunHidden(head + L" -map 0:v? -map 0:a?" + tail(out), nullptr);
        if (rc != 0) {
            DeleteFileW(out.c_str());
            path = mkv;
            rc = RunHidden(head + L" -map 0 -dn" + tail(mkv), nullptr);
        }
        auto* r = new TrimResult;
        r->kind = 2; r->ok = rc == 0; r->path = path;
        if (!r->ok) DeleteFileW(path.c_str());
        if (!PostMessageW(hwnd, WM_APP_TRIM, 0, (LPARAM)r)) delete r;
    }).detach();
    ComposeOverlay();
}
static void OnTrimResult(TrimResult* r) {
    if (r->kind == 1) {
        if (r->gen == g.trimProbeGen && g.trimMode) { g.trimKeyA = r->keyA; ComposeOverlay(); }
    } else if (r->kind == 2) {
        g.trimBusy = false;
        if (r->ok) {
            SetTrimMode(false);
            ShowToast(L"Saved " + std::wstring(PathFindFileNameW(r->path.c_str())));
        } else {
            ShowToast(L"Couldn't save the trimmed copy");
        }
        ComposeOverlay();
        UpdateVisibility();
    }
    delete r;
}

// Next/previous video, in the order Explorer shows the folder (falls back to name order).
static bool OpenSibling(int dir, bool wrap) {
    if (g.file.empty()) return false;
    size_t slash = g.file.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    std::wstring folder = g.file.substr(0, slash + 1);

    // 1) live Explorer order, 2) the order remembered earlier, 3) natural name order
    std::vector<std::wstring> files;
    if (ExplorerOrder(folder, g.file, files)) {
        gOrder = files;
    } else if (OrderContains(gOrder, g.file)) {
        files = gOrder;
    } else {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((folder + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (f == INVALID_HANDLE_VALUE) { ShowToast(L"Can't read this folder"); return false; }
        std::wstring cur = g.file.substr(slash + 1);
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (IsVideoFile(fd.cFileName) || _wcsicmp(fd.cFileName, cur.c_str()) == 0) files.push_back(fd.cFileName);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
        for (auto& x : files) x = folder + x;
    }
    int count = (int)files.size();
    int idx = -1;
    for (int i = 0; i < count; i++) if (_wcsicmp(files[i].c_str(), g.file.c_str()) == 0) { idx = i; break; }
    if (count == 0 || (count == 1 && idx == 0)) { ShowToast(L"No other videos in this folder"); return false; }
    int n = idx < 0 ? (dir > 0 ? 0 : count - 1) : idx + dir;
    if (n < 0 || n >= count) {
        if (!wrap) { ShowToast(dir > 0 ? L"Last video in this folder" : L"First video in this folder"); return false; }
        n = (n + count) % count;
    }
    OpenFile(files[n]);
    return true;
}
static void ShowOpenDialog() {
    wchar_t buf[32768] = {};
    OPENFILENAMEW of = {sizeof(of)};
    of.hwndOwner = g.hwnd;
    of.lpstrFilter = L"Videos\0*.mp4;*.mkv;*.webm;*.avi;*.mov;*.m4v;*.wmv;*.flv;*.mpg;*.mpeg;*.ts;*.m2ts;*.mts;*.3gp;*.ogv;*.gif\0All files\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = 32768;
    of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&of)) OpenFile(buf);
}

// ---------------------------------------------------------------------------------------------
// mpv events (UI thread)
// ---------------------------------------------------------------------------------------------
static void OnMpvWakeup(void*) {
    if (!g.mpvWakePending.exchange(true)) PostMessageW(g.hwnd, WM_APP_MPV, 0, 0);
}
static void HandleMpvEvents() {
    g.mpvWakePending = false;
    bool redraw = false;
    for (;;) {
        mpv_event* e = M.wait_event(g.mpv, 0);
        if (e->event_id == MPV_EVENT_NONE) break;
        switch (e->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto* p = (mpv_event_property*)e->data;
            std::string n = p->name;
            if (n == "time-pos") {
                double v = p->format == MPV_FORMAT_DOUBLE ? *(double*)p->data : 0;
                // only redraw when the visible state changes (second or progress pixel)
                Layout L = ComputeLayout();
                int w = std::max(1, L.trackR - L.trackL);
                bool changed = (long)v != (long)g.timePos ||
                               (g.duration > 0 && (int)(v / g.duration * w) != (int)(g.timePos / g.duration * w));
                g.timePos = v;
                if (changed && (g.showBottom || g.alpha[OV_BOTTOM] > 0)) redraw = true;
            } else if (n == "duration") {
                g.duration = p->format == MPV_FORMAT_DOUBLE ? *(double*)p->data : 0;
                redraw = true;
            } else if (n == "pause") {
                g.paused = p->format == MPV_FORMAT_FLAG && *(int*)p->data;
                UpdateVisibility();

                redraw = true;
            } else if (n == "volume") {
                if (p->format == MPV_FORMAT_DOUBLE) g.volume = *(double*)p->data;
                g.cfg.volume = g.volume;
                SetTimer(g.hwnd, TIMER_SAVE, 800, nullptr);
                redraw = true;
            } else if (n == "mute") {
                g.mute = p->format == MPV_FORMAT_FLAG && *(int*)p->data;
                g.cfg.mute = g.mute;
                SetTimer(g.hwnd, TIMER_SAVE, 800, nullptr);
                redraw = true;
            } else if (n == "eof-reached") {
                bool eof = p->format == MPV_FORMAT_FLAG && *(int*)p->data;
                if (eof != g.eof) { g.eof = eof; redraw = true; UpdateVisibility(); }
                if (!eof) g.eofHandled = false;
                else if (!g.eofHandled && g.hasFile && g.drag == H_NONE && g.repeat == REPEAT_NEXT) {
                    g.eofHandled = true;
                    PostMessageW(g.hwnd, WM_APP_NEXT, 0, 0);  // don't re-enter mpv from inside the event loop
                }
            } else if (n == "idle-active") {
                g.idle = p->format == MPV_FORMAT_FLAG && *(int*)p->data;
                if (g.idle) { g.hasFile = false; redraw = true; }
            }
            break;
        }
        case MPV_EVENT_FILE_LOADED:
            g.hasFile = true;
            g.errorText.clear();
            redraw = true;
            break;
        case MPV_EVENT_END_FILE: {
            auto* ef = (mpv_event_end_file*)e->data;
            if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                g.hasFile = false;
                g.errorText = L"Can't play this file";
                redraw = true;
            }
            break;
        }
        default: break;
        }
    }
    if (redraw) ComposeOverlay();
}

// ---------------------------------------------------------------------------------------------
// Mouse handling
// ---------------------------------------------------------------------------------------------
static void UpdateHoverPreview(const Layout& L) {
    if ((IsTl(g.hover) || g.drag == H_TIMELINE) && g.duration > 0) {
        double t = TimeAtX(L, g.mouseX);
        bool exact = false;
        ThumbPtr tp = LookupThumb(t, &exact);
        if (tp) g.shownThumb = tp;
        if (!exact) RequestThumb(t);
    }
}
// Arrow normally, hand over buttons, left-right arrows over the trim handles.
static void ApplyCursor() {
    bool sizer = g.hover == H_TRIM_A || g.hover == H_TRIM_B || g.drag == H_TRIM_A || g.drag == H_TRIM_B;
    bool button = g.drag == H_NONE && (g.hover == H_PLAY || g.hover == H_PREV || g.hover == H_NEXT || g.hover == H_MUTE ||
                  g.hover == H_FULLSCREEN || g.hover == H_REPEAT || g.hover == H_ROTATE || g.hover == H_TRIM ||
                  g.hover == H_TRIM_SAVE || g.hover == H_MIN || g.hover == H_MAX || g.hover == H_CLOSE || g.hover == H_EXITFS);
    SetCursor(g.cursorHidden ? nullptr : LoadCursor(nullptr, sizer ? IDC_SIZEWE : button ? IDC_HAND : IDC_ARROW));
}
static void OnMouseMove(int x, int y) {
    bool moved = (x != g.mouseX || y != g.mouseY);
    g.mouseX = x; g.mouseY = y;
    if (!g.mouseInside) {
        g.mouseInside = true;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g.hwnd, 0};
        TrackMouseEvent(&tme);
    }
    if (moved) PokeActivity();
    Layout L = ComputeLayout();
    Hit old = g.hover;
    g.hover = HitTest(L, x, y);
    if (g.drag == H_TIMELINE) {
        g.dragTime = TimeAtX(L, x);
        DWORD now = GetTickCount();
        if (now - g.lastSeekTick > 40) { SeekAbs(g.dragTime, false); g.lastSeekTick = now; }
    } else if (g.drag == H_VOLUME) {
        double f = std::clamp((double)(x - L.volL) / std::max(1, L.volR - L.volL), 0.0, 1.0);
        SetVolume(f * 100);
    } else if (g.drag == H_TRIM_A || g.drag == H_TRIM_B) {
        double t = TimeAtX(L, x);
        bool start = g.drag == H_TRIM_A;
        if (start) { g.trimA = std::clamp(t, 0.0, std::max(0.0, g.trimB - 0.05)); g.trimKeyA = -1; }
        else g.trimB = std::clamp(t, std::min(g.duration, g.trimA + 0.05), g.duration);
        DWORD now = GetTickCount();   // show the frame under the handle
        if (now - g.lastSeekTick > 60) { SeekAbs(start ? g.trimA : g.trimB, true); g.lastSeekTick = now; }
    }
    UpdateHoverPreview(L);
    if (old != g.hover) ApplyCursor();
    if (moved || old != g.hover) ComposeOverlay();
}
static void OnLButtonDown(int x, int y) {
    Layout L = ComputeLayout();
    Hit h = HitTest(L, x, y);
    PokeActivity();
    switch (h) {
    case H_VIDEO: TogglePause(); break;
    case H_PLAY: if (g.hasFile) TogglePause(); else ShowOpenDialog(); break;
    case H_PREV: OpenSibling(-1, true); break;
    case H_NEXT: OpenSibling(+1, true); break;
    case H_MUTE: ToggleMute(); break;
    case H_REPEAT: CycleRepeat(); break;
    case H_ROTATE: SetRotation(g.rotate + 90); break;
    case H_FULLSCREEN: ToggleFullscreen(); break;
    case H_TRIM: SetTrimMode(!g.trimMode); break;
    case H_TRIM_SAVE: SaveTrimmedCopy(); break;
    case H_TRIM_A: case H_TRIM_B:
        g.drag = h;
        SetCapture(g.hwnd);
        if (!g.paused) TogglePause();   // hold still while picking the frame
        break;
    case H_MIN: case H_MAX: case H_CLOSE: case H_EXITFS:
        g.pressed = h;
        SetCapture(g.hwnd);
        break;
    case H_SCRUB:
    case H_TIMELINE:
        if (g.duration > 0) {
            g.drag = H_TIMELINE;
            SetCapture(g.hwnd);
            g.dragTime = TimeAtX(L, x);
            SeekAbs(g.dragTime, false);
            g.lastSeekTick = GetTickCount();
        }
        break;
    case H_VOLUME: {
        g.drag = H_VOLUME;
        SetCapture(g.hwnd);
        double f = std::clamp((double)(x - L.volL) / std::max(1, L.volR - L.volL), 0.0, 1.0);
        SetVolume(f * 100);
        break;
    }
    default: break;
    }
    ComposeOverlay();
}
static void OnLButtonUp(int x, int y) {
    if (g.pressed != H_NONE) {
        Hit p = g.pressed;
        g.pressed = H_NONE;
        ReleaseCapture();
        Layout L = ComputeLayout();
        if (HitTest(L, x, y) == p) {
            switch (p) {
            case H_CLOSE: PostMessageW(g.hwnd, WM_CLOSE, 0, 0); break;
            case H_MIN: ShowWindow(g.hwnd, SW_MINIMIZE); break;
            case H_MAX: ShowWindow(g.hwnd, IsZoomed(g.hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
            case H_EXITFS: ToggleFullscreen(); break;
            default: break;
            }
        }
        g.hover = H_NONE;
        ComposeOverlay();
        return;
    }
    if (g.drag == H_TIMELINE) {
        Layout L = ComputeLayout();
        g.dragTime = TimeAtX(L, x);
        g.timePos = g.dragTime;
        SeekAbs(g.dragTime, true);
    }
    if (g.drag == H_TRIM_A || g.drag == H_TRIM_B) {
        bool start = g.drag == H_TRIM_A;
        SetTrimPoint(start, start ? g.trimA : g.trimB);
        SeekAbs(start ? g.trimA : g.trimB, true);
    }
    if (g.drag != H_NONE) {
        g.drag = H_NONE;
        ReleaseCapture();
        ComposeOverlay();
    }
}

// ---------------------------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------------------------
static void SaveWindowState() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (g.fullscreen) wp = g.savedPlacement;
    else GetWindowPlacement(g.hwnd, &wp);
    g.cfg.rect = wp.rcNormalPosition;
    g.cfg.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    g.cfg.SaveWindow();
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_MPV: HandleMpvEvents(); return 0;
    case WM_APP_NEXT: OpenSibling(+1, false); return 0;
    case WM_APP_SNAPSHOT: SnapshotExplorerOrder(); return 0;
    case WM_APP_REGISTER: RegisterFileTypeLook(); return 0;
    case WM_APP_TRIM: OnTrimResult((TrimResult*)lp); return 0;
    case WM_APP_THUMB:
        if (IsTl(g.hover) || g.drag == H_TIMELINE) {
            Layout L = ComputeLayout();
            UpdateHoverPreview(L);
            ComposeOverlay();
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        RequestRender();
        return 0;
    }
    case WM_SIZE:
        if (g.mpv) ComposeOverlay();
        RequestRender();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize = {360, 220};
        return 0;
    }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        gDpiForMetrics = g.dpi;
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ComposeOverlay();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            ApplyCursor();
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_NCMOUSEMOVE:
        if (g.hover != H_NONE) { g.hover = H_NONE; ComposeOverlay(); }
        break;
    case WM_MOUSELEAVE:
        g.mouseInside = false;
        g.hover = H_NONE;
        ComposeOverlay();
        UpdateVisibility();
        return 0;
    case WM_LBUTTONDOWN: SetFocus(h); OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONUP: OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONDBLCLK: {
        Layout L = ComputeLayout();
        Hit hh = HitTest(L, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hh == H_VIDEO) {
            TogglePause();  // undo the pause toggle from the first click
            ToggleFullscreen();
        } else {
            OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != h && (g.drag != H_NONE || g.pressed != H_NONE)) {
            g.drag = H_NONE; g.pressed = H_NONE; ComposeOverlay();
        }
        return 0;
    case WM_NCCALCSIZE:
        // drop the standard caption (we draw a taller one ourselves) but keep the resize borders
        if (wp && !g.fullscreen) {
            auto* p = (NCCALCSIZE_PARAMS*)lp;
            int top = p->rgrc[0].top;
            LRESULT r = DefWindowProcW(h, msg, wp, lp);
            p->rgrc[0].top = top;
            if (IsZoomed(h)) p->rgrc[0].top += SysMetric(SM_CYFRAME) + SysMetric(SM_CXPADDEDBORDER);
            return r;
        }
        break;
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(h, msg, wp, lp);
        if (hit != HTCLIENT || g.fullscreen) return hit;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        Layout L = ComputeLayout();
        if (!IsZoomed(h)) {
            int b = SysMetric(SM_CYFRAME) + SysMetric(SM_CXPADDEDBORDER);
            if (pt.y < b) return pt.x < b * 2 ? HTTOPLEFT : (pt.x >= L.cw - b * 2 ? HTTOPRIGHT : HTTOP);
        }
        // invisible title bar: the top strip drags the window (except the caption buttons)
        if (pt.y < L.dragH && !In(L.topBox, pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ACTIVATE:
        g.active = LOWORD(wp) != WA_INACTIVE;
        if (g.mpv) ComposeOverlay();
        break;
    case WM_XBUTTONUP:
        OpenSibling(GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? +1 : -1, true);
        return TRUE;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        SetVolume(g.volume + (d > 0 ? 5 : -5));
        FlashBottom();
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_HIDE) {
            KillTimer(h, TIMER_HIDE);
            g.recentActivity = false;
            UpdateVisibility();
            bool overUi = g.hover != H_VIDEO && g.hover != H_NONE;
            if (g.mouseInside && g.hasFile && !overUi && g.drag == H_NONE) { g.cursorHidden = true; SetCursor(nullptr); }
        } else if (wp == TIMER_FADE) {
            StepFade();
        } else if (wp == TIMER_FLASH) {
            KillTimer(h, TIMER_FLASH);
            UpdateVisibility();
        } else if (wp == TIMER_TOAST) {
            KillTimer(h, TIMER_TOAST);
            g.toast.clear();
            ComposeOverlay();
            UpdateVisibility();
        } else if (wp == TIMER_SAVE) {
            KillTimer(h, TIMER_SAVE);
            g.cfg.SavePlayer();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        switch (wp) {
        case VK_SPACE: case 'K': TogglePause(); break;
        case VK_LEFT: SeekRel(shift ? -1 : (ctrl ? -30 : -5)); break;
        case VK_RIGHT: SeekRel(shift ? 1 : (ctrl ? 30 : 5)); break;
        case 'J': SeekRel(-10); break;
        case 'L': SeekRel(10); break;
        case VK_UP: SetVolume(g.volume + 5); break;
        case VK_DOWN: SetVolume(g.volume - 5); break;
        case 'M': ToggleMute(); break;
        case 'R':
            if (ctrl) { if (!g.trimMode) SetRotation(g.rotate + (shift ? -90 : 90)); }
            else CycleRepeat();
            break;
        case 'F': case VK_F11: ToggleFullscreen(); break;
        case VK_RETURN: if (msg == WM_SYSKEYDOWN || !ctrl) ToggleFullscreen(); break;
        case VK_ESCAPE:
            if (g.trimMode) SetTrimMode(false);
            else if (g.fullscreen) ToggleFullscreen();
            break;
        case 'E': if (ctrl) SetTrimMode(!g.trimMode); break;
        case 'I': if (g.trimMode) SetTrimPoint(true, g.timePos); break;   // start at the playhead
        case 'S': if (ctrl) SaveTrimmedCopy(); break;
        case VK_NEXT: case 'N': OpenSibling(+1, true); break;
        case VK_PRIOR: case 'P': OpenSibling(-1, true); break;
        case VK_HOME: SeekAbs(0, true); break;
        case VK_OEM_PERIOD: Cmd({"frame-step"}); break;
        case VK_OEM_COMMA: Cmd({"frame-back-step"}); break;
        case 'O':
            if (ctrl) ShowOpenDialog();
            else SetTrimPoint(false, g.timePos);   // trim end at the playhead
            break;
        case 'W': if (ctrl) PostMessageW(h, WM_CLOSE, 0, 0); break;
        case VK_F4: if (msg == WM_SYSKEYDOWN) PostMessageW(h, WM_CLOSE, 0, 0); break;
        default:
            if (msg == WM_SYSKEYDOWN) return DefWindowProcW(h, msg, wp, lp);
            return 0;
        }
        if (wp != VK_ESCAPE && wp != VK_NEXT && wp != VK_PRIOR && wp != 'N' && wp != 'P') FlashBottom();
        return 0;
    }
    case WM_SYSCHAR: if (wp == VK_RETURN) return 0; break;
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp;
        wchar_t buf[MAX_PATH * 4];
        if (DragQueryFileW(d, 0, buf, MAX_PATH * 4)) OpenFile(buf);
        DragFinish(d);
        SetForegroundWindow(h);
        return 0;
    }
    case WM_CLOSE: {
        SaveWindowState();
        g.cfg.SavePlayer();
        // stop audio right away (the rest of the shutdown takes a moment)
        int yes = 1;
        M.set_property(g.mpv, "pause", MPV_FORMAT_FLAG, &yes);
        const char* stop[] = {"stop", nullptr};
        M.command(g.mpv, stop);
        ShowWindow(h, SW_HIDE);
        DestroyWindow(h);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int nShow) {
    g.inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Per-monitor DPI awareness (Win10+), with fallback
    {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        typedef BOOL(WINAPI * SetCtx)(HANDLE);
        if (auto f = (SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext")) f((HANDLE)-4);
        else SetProcessDPIAware();
    }

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    if (!LoadMpv(dir)) {
        MessageBoxW(nullptr, L"libmpv was not found.\n\nPut libmpv-2.dll (or mpv-2.dll / mpv-1.dll) next to Vysion.exe.",
                    L"Vysion", MB_ICONERROR);
        return 1;
    }

    g.cfg.Init();
    g.cfg.Load();
    g.volume = g.cfg.volume;
    g.mute = g.cfg.mute;
    g.repeat = g.cfg.repeat;

    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g.gdipToken, &gsi, nullptr);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_OWNDC | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"VysionWindow";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1100, h = 650;
    if (g.cfg.rect.left != CW_USEDEFAULT) {
        x = g.cfg.rect.left; y = g.cfg.rect.top;
        w = g.cfg.rect.right - g.cfg.rect.left; h = g.cfg.rect.bottom - g.cfg.rect.top;
    }
    g.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Vysion", WS_OVERLAPPEDWINDOW, x, y, w, h,
                             nullptr, nullptr, inst, nullptr);
    {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        typedef UINT(WINAPI * GetDpi)(HWND);
        if (auto f = (GetDpi)(void*)GetProcAddress(u, "GetDpiForWindow")) g.dpi = f(g.hwnd);
        gDpiForMetrics = g.dpi;
        if (g.cfg.rect.left == CW_USEDEFAULT && g.dpi != 96)
            SetWindowPos(g.hwnd, nullptr, 0, 0, MulDiv(w, g.dpi, 96), MulDiv(h, g.dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
    }
    // apply the custom frame (WM_NCCALCSIZE) right away
    SetWindowPos(g.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

    HDC dc = GetDC(g.hwnd);
    SetupPixelFormat(dc);
    ReleaseDC(g.hwnd, dc);

    // mpv
    g.mpv = M.create();
    auto opt = [](const char* k, const char* v) { M.set_option_string(g.mpv, k, v); };
    opt("config", "no"); opt("terminal", "no"); opt("msg-level", "all=no");
    opt("vo", "libmpv"); opt("hwdec", "auto-safe");
    opt("keep-open", "yes"); opt("idle", "yes");
    opt("osc", "no"); opt("osd-level", "0"); opt("ytdl", "no"); opt("load-scripts", "no");
    opt("input-default-bindings", "no"); opt("input-vo-keyboard", "no");
    opt("volume-max", "100");
    opt("hr-seek", "default");
    opt("background", "color"); opt("background-color", "#000000");
    char vb[32]; snprintf(vb, sizeof vb, "%.0f", g.volume);
    opt("volume", vb);
    opt("mute", g.mute ? "yes" : "no");
    opt("loop-file", g.repeat == REPEAT_ONE ? "inf" : "no");
    if (M.initialize(g.mpv) < 0) {
        MessageBoxW(nullptr, L"Could not initialise libmpv.", L"Vysion", MB_ICONERROR);
        return 1;
    }
    M.observe_property(g.mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    M.observe_property(g.mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    M.observe_property(g.mpv, 0, "pause", MPV_FORMAT_FLAG);
    M.observe_property(g.mpv, 0, "volume", MPV_FORMAT_DOUBLE);
    M.observe_property(g.mpv, 0, "mute", MPV_FORMAT_FLAG);
    M.observe_property(g.mpv, 0, "idle-active", MPV_FORMAT_FLAG);
    M.observe_property(g.mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
    M.set_wakeup_callback(g.mpv, OnMpvWakeup, nullptr);

    ShowWindow(g.hwnd, g.cfg.maximized ? SW_SHOWMAXIMIZED : nShow);

    g.renderThread = std::thread(RenderThreadMain);
    {
        std::unique_lock<std::mutex> l(g.rm);
        g.rcv.wait(l, [] { return g.rReady; });
    }
    g.thumbThread = std::thread(ThumbThreadMain);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        wchar_t full[MAX_PATH * 4];
        GetFullPathNameW(argv[1], MAX_PATH * 4, full, nullptr);
        OpenFile(full);
        PostMessageW(g.hwnd, WM_APP_SNAPSHOT, 0, 0);  // after the first frames are on their way
    } else {
        ComposeOverlay();
    }
    LocalFree(argv);
    PostMessageW(g.hwnd, WM_APP_REGISTER, 0, 0);  // low priority, after startup

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    // shutdown: render context must go before the mpv handle
    { std::lock_guard<std::mutex> l(g.rm); g.rQuit = true; }
    g.rcv.notify_all();
    { std::lock_guard<std::mutex> l(g.th.m); g.th.quit = true; }
    g.th.cv.notify_all();
    if (g.renderThread.joinable()) g.renderThread.join();
    if (g.thumbThread.joinable()) g.thumbThread.join();
    M.terminate_destroy(g.mpv);
    Gdiplus::GdiplusShutdown(g.gdipToken);
    return 0;
}
