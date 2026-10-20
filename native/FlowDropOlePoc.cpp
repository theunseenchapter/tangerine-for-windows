// FlowDrop Quick Convert  -  radial wheel edition
//
// Build (MSVC, Developer Command Prompt):
//   cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE FlowDrop.cpp /link /SUBSYSTEM:WINDOWS
//
// Usage
//   Drag mode   : start dragging a file, press Shift -> a wheel opens under the cursor.
//                 Flick toward a format (or scroll the mouse wheel to rotate), release to convert.
//   Context mode: FlowDrop.exe --context-file="C:\path\file.png"
//                 Wheel opens at the cursor. Click / Enter to convert, arrows or scroll to rotate, Esc to cancel.

#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <algorithm>
using std::min;
using std::max;

#include <windows.h>
#include <mmsystem.h>
#include <ole2.h>
#include <oleidl.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <shellscalingapi.h>
#include <vector>
#include <string>
#include <array>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <climits>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Gdiplus;

// ---------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------
static constexpr UINT  WM_APP_SHOW_CARD = WM_APP + 1;
static constexpr UINT  WM_APP_HIDE_CARD = WM_APP + 2;
static constexpr UINT  WM_APP_DONE      = WM_APP + 3;
static constexpr float PI               = 3.14159265359f;

// Wheel geometry (logical pixels, scaled by DPI at runtime)
static constexpr float WIN_SIZE  = 460.f;   // window edge
static constexpr float R_DISC    = 190.f;   // glass disc
static constexpr float R_TRACK   = 180.f;   // outer indicator track
static constexpr float R_OUT     = 162.f;   // segment outer radius
static constexpr float R_IN      = 84.f;    // segment inner radius
static constexpr float R_HUB     = 66.f;    // centre hub
static constexpr float SEG_GAP   = 3.f;
static constexpr float SEG_INSET = 4.f;     // corner rounding

// ---------------------------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------------------------
struct Theme {
    Color panelTop, panelBottom, rim, segA, segB, hub, hubEdge;
    Color ink, muted, accentA, accentB, onAccent, shadow, good, bad;
};

static const Theme kDark = {
    Color(245, 36, 38, 48),  Color(245, 19, 20, 27),  Color(70, 255, 255, 255),
    Color(255, 52, 55, 68),  Color(255, 40, 42, 53),  Color(255, 14, 15, 20),  Color(45, 255, 255, 255),
    Color(255, 245, 246, 249), Color(255, 148, 154, 168),
    Color(255, 255, 128, 64),  Color(255, 255, 66, 112), Color(255, 255, 255, 255),
    Color(140, 0, 0, 0), Color(255, 66, 210, 130), Color(255, 242, 86, 86)
};
static const Theme kLight = {
    Color(248, 255, 255, 255), Color(248, 236, 239, 247), Color(60, 30, 36, 60),
    Color(255, 236, 239, 247), Color(255, 225, 229, 240), Color(255, 250, 251, 254), Color(45, 30, 36, 60),
    Color(255, 28, 32, 44), Color(255, 108, 115, 132),
    Color(255, 255, 112, 48), Color(255, 244, 60, 110), Color(255, 255, 255, 255),
    Color(90, 40, 48, 90), Color(255, 34, 170, 100), Color(255, 226, 64, 64)
};
static Theme g_theme = kDark;

// ---------------------------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------------------------
struct OutputFormat {
    const wchar_t* label;
    const wchar_t* extension;
    const wchar_t* mime;   // empty = document format handled by LibreOffice
    const wchar_t* hint;   // short caption shown under the label
};

static constexpr std::array<OutputFormat, 10> kFormats = {{
    { L"JPG",  L".jpg",  L"image/jpeg", L"Photo"      },
    { L"PNG",  L".png",  L"image/png",  L"Lossless"   },
    { L"WEBP", L".webp", L"image/webp", L"Web"        },
    { L"AVIF", L".avif", L"image/avif", L"Next-gen"   },
    { L"BMP",  L".bmp",  L"image/bmp",  L"Bitmap"     },
    { L"GIF",  L".gif",  L"image/gif",  L"256 colors" },
    { L"TIFF", L".tiff", L"image/tiff", L"Archive"    },
    { L"PDF",  L".pdf",  L"",           L"Document"   },
    { L"DOCX", L".docx", L"",           L"Word"       },
    { L"TXT",  L".txt",  L"",           L"Plain text" }
}};

enum class Phase { Idle, Busy, Success, Failed };

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
static HWND      g_window = nullptr;
static HHOOK     g_mouseHook = nullptr;
static HHOOK     g_keyboardHook = nullptr;
static ULONG_PTR g_gdiplusToken = 0;
static FontFamily* g_fontFamily = nullptr;

static std::vector<std::wstring> g_files;       // context mode files
static std::vector<int>          g_items;       // format indices shown on the wheel
static int   g_selected = -1;                   // index into g_items
static bool  g_wheelOverride = false;           // selection came from scroll / keys
static POINT g_lastPt = { INT_MIN, INT_MIN };
static int   g_wheelAccum = 0;

static bool  g_leftDown = false;
static bool  g_dragging = false;
static bool  g_shift = false;
static bool  g_shiftConfirmed = false;
static bool  g_cancelled = false;
static bool  g_verbose = false;
static bool  g_contextMode = false;
static bool  g_quitPending = false;
static POINT g_downPt = {};

static Phase  g_phase = Phase::Idle;
static double g_phaseTime = 0.0;
static double g_startTime = 0.0;

static std::wstring g_source = L"FILE";
static std::wstring g_filename;

// window / animation
static bool  g_visible = false;     // logically open
static bool  g_shown = false;       // window actually on screen (includes fade-out)
static int   g_winX = 0, g_winY = 0, g_winSize = 0;
static float g_scale = 1.0f;
static float g_alpha = 0.f;
static float g_pop = 0.8f, g_popVel = 0.f;
static float g_pulse = 0.f;
static float g_knob = -90.f, g_knobAlpha = 0.f;
static float g_spin = 0.f;
static std::array<float, 16> g_hoverAnim = {};
static double g_itemsStart = 0.0;
static double g_lastFrame = 0.0;

// back buffer
static HDC     g_memDC = nullptr;
static HBITMAP g_dib = nullptr;
static HGDIOBJ g_oldBmp = nullptr;
static void*   g_pixels = nullptr;
static int     g_surfaceSize = 0;

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------
static void Log(const wchar_t* format, ...)
{
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(buffer, ARRAYSIZE(buffer), format, args);
    va_end(args);
    if (g_verbose) wprintf(L"%s\n", buffer);
    OutputDebugStringW(buffer);
    OutputDebugStringW(L"\n");
}

static double Now()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
}

static float Clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
static float EaseOutCubic(float t) { const float u = 1.0f - t; return 1.0f - u * u * u; }
static float Smooth(float cur, float target, float rate, float dt)
{
    return cur + (target - cur) * (1.0f - std::exp(-rate * dt));
}

static Color Mix(const Color& a, const Color& b, float t)
{
    auto m = [&](int x, int y) { return static_cast<BYTE>(x + (y - x) * t + 0.5f); };
    return Color(m(a.GetA(), b.GetA()), m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
}
static Color Fade(const Color& c, float k)
{
    return Color(static_cast<BYTE>(c.GetA() * Clamp(k, 0.f, 1.f) + 0.5f), c.GetR(), c.GetG(), c.GetB());
}
static PointF Polar(float r, float deg)
{
    const float a = deg * PI / 180.0f;
    return PointF(r * std::cos(a), r * std::sin(a));
}

static Theme CurrentTheme()
{
    DWORD light = 0, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light ? kLight : kDark;
}

// ---------------------------------------------------------------------------------------------
// File type helpers
// ---------------------------------------------------------------------------------------------
static bool IsImage(const std::wstring& file)
{
    const wchar_t* ext = PathFindExtensionW(file.c_str());
    if (!ext) return false;
    static const wchar_t* extensions[] = {
        L".jpg", L".jpeg", L".png", L".webp", L".avif", L".bmp", L".gif", L".tif", L".tiff", L".heic"
    };
    for (const auto* e : extensions) if (_wcsicmp(ext, e) == 0) return true;
    return false;
}

static bool IsDocument(const std::wstring& file)
{
    const wchar_t* ext = PathFindExtensionW(file.c_str());
    if (!ext) return false;
    static const wchar_t* extensions[] = { L".pdf", L".docx", L".xlsx", L".pptx", L".txt" };
    for (const auto* e : extensions) if (_wcsicmp(ext, e) == 0) return true;
    return false;
}

static std::wstring SourceFormat(const std::wstring& file)
{
    const wchar_t* ext = PathFindExtensionW(file.c_str());
    if (!ext || !*ext) return L"FILE";
    std::wstring s = ext + 1;
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    if (s == L"JPEG") s = L"JPG";
    if (s == L"TIF") s = L"TIFF";
    if (s.size() > 5) s = L"FILE";
    return s;
}

static std::wstring ShortName(const std::wstring& file)
{
    std::wstring name = PathFindFileNameW(file.c_str());
    if (name.size() > 35) name = name.substr(0, 31) + L"...";
    return name;
}

static bool SameFormat(const std::wstring& file, int format)
{
    const wchar_t* ext = PathFindExtensionW(file.c_str());
    if (!ext || format < 0 || format >= static_cast<int>(kFormats.size())) return false;
    if (_wcsicmp(ext, kFormats[format].extension) == 0) return true;
    if (_wcsicmp(ext, L".jpeg") == 0 && wcscmp(kFormats[format].label, L"JPG") == 0) return true;
    if (_wcsicmp(ext, L".tif") == 0 && wcscmp(kFormats[format].label, L"TIFF") == 0) return true;
    return false;
}

static int EncoderClsid(const wchar_t* mime, CLSID* clsid)
{
    UINT count = 0, bytes = 0;
    GetImageEncodersSize(&count, &bytes);
    if (!bytes) return 0;
    std::vector<BYTE> buffer(bytes);
    auto* encoders = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    GetImageEncoders(count, bytes, encoders);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, mime) == 0) { *clsid = encoders[i].Clsid; return 1; }
    }
    return 0;
}

static std::wstring LibreOfficePath()
{
    wchar_t value[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"FLOWDROP_LIBREOFFICE", value, ARRAYSIZE(value));
    if (length && length < ARRAYSIZE(value) && PathFileExistsW(value)) return value;
    wchar_t executable[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    PathRemoveFileSpecW(executable);
    const std::wstring base = executable;
    const std::wstring candidates[] = {
        base + L"\\libreoffice\\program\\soffice.exe",
        base + L"\\tools\\libreoffice\\program\\soffice.exe",
        L"C:\\Program Files\\LibreOffice\\program\\soffice.exe",
        L"C:\\Program Files (x86)\\LibreOffice\\program\\soffice.exe"
    };
    for (const auto& c : candidates) if (PathFileExistsW(c.c_str())) return c;
    return L"";
}

// ---------------------------------------------------------------------------------------------
// Conversion (runs on a worker thread so the drag source never freezes)
// ---------------------------------------------------------------------------------------------
static std::wstring OutputPath(const std::wstring& input, const wchar_t* extension)
{
    wchar_t stem[MAX_PATH] = {};
    StringCchCopyW(stem, ARRAYSIZE(stem), input.c_str());
    PathRemoveExtensionW(stem);
    wchar_t output[MAX_PATH] = {};
    StringCchPrintfW(output, ARRAYSIZE(output), L"%s-converted%s", stem, extension);
    for (int n = 2; PathFileExistsW(output) && n < 1000; ++n)
        StringCchPrintfW(output, ARRAYSIZE(output), L"%s-converted-%d%s", stem, n, extension);
    return output;
}

static bool ConvertDocument(const std::wstring& input, int format, const std::wstring& output)
{
    const std::wstring office = LibreOfficePath();
    if (office.empty()) return false;

    // Convert inside a private temp folder so we can never overwrite an existing sibling file.
    wchar_t temp[MAX_PATH] = {};
    if (!GetTempPathW(ARRAYSIZE(temp), temp)) return false;
    wchar_t dir[MAX_PATH] = {};
    StringCchPrintfW(dir, ARRAYSIZE(dir), L"%sflowdrop-%lu-%llu", temp,
        GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
    if (!CreateDirectoryW(dir, nullptr)) return false;

    std::wstring filter = kFormats[format].extension + 1;
    if (wcscmp(kFormats[format].label, L"TXT") == 0) filter = L"txt:Text";

    std::wstring command = L"\"" + office + L"\" --headless --norestore --convert-to " + filter;
    command += L" --outdir \"" + std::wstring(dir) + L"\" \"" + input + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    bool ok = false;
    if (CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        if (WaitForSingleObject(process.hProcess, 180000) == WAIT_TIMEOUT)
            TerminateProcess(process.hProcess, 1);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        wchar_t stem[MAX_PATH] = {};
        StringCchCopyW(stem, ARRAYSIZE(stem), PathFindFileNameW(input.c_str()));
        PathRemoveExtensionW(stem);
        const std::wstring generated = std::wstring(dir) + L"\\" + stem + kFormats[format].extension;
        ok = exitCode == 0 && PathFileExistsW(generated.c_str()) &&
            MoveFileExW(generated.c_str(), output.c_str(), MOVEFILE_COPY_ALLOWED) != FALSE;
        DeleteFileW(generated.c_str());
    }
    RemoveDirectoryW(dir);
    return ok;
}

static bool ConvertFile(const std::wstring& input, int format)
{
    if (format < 0 || format >= static_cast<int>(kFormats.size()) || SameFormat(input, format)) return false;
    const std::wstring output = OutputPath(input, kFormats[format].extension);
    if (PathFileExistsW(output.c_str())) return false;
    if (!kFormats[format].mime[0]) return ConvertDocument(input, format, output);

    CLSID encoder;
    if (!EncoderClsid(kFormats[format].mime, &encoder)) return false;
    Bitmap bitmap(input.c_str(), FALSE);
    if (bitmap.GetLastStatus() != Ok) return false;

    if (wcscmp(kFormats[format].mime, L"image/jpeg") == 0) {
        // JPEG has no alpha: flatten onto white instead of getting black backgrounds.
        const int w = static_cast<int>(bitmap.GetWidth());
        const int h = static_cast<int>(bitmap.GetHeight());
        Bitmap flat(w, h, PixelFormat32bppRGB);
        {
            Graphics g(&flat);
            g.Clear(Color(255, 255, 255, 255));
            g.DrawImage(&bitmap, 0, 0, w, h);
        }
        ULONG quality = 92;
        EncoderParameters params = {};
        params.Count = 1;
        params.Parameter[0].Guid = EncoderQuality;
        params.Parameter[0].Type = EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        params.Parameter[0].Value = &quality;
        return flat.Save(output.c_str(), &encoder, &params) == Ok;
    }
    return bitmap.Save(output.c_str(), &encoder, nullptr) == Ok;
}

static void StartConversion(std::vector<std::wstring> files, int format)
{
    g_phase = Phase::Busy;
    g_phaseTime = Now();
    std::thread([files = std::move(files), format]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = !files.empty();
        for (const auto& f : files) {
            if (!(IsImage(f) || IsDocument(f)) || !ConvertFile(f, format)) ok = false;
        }
        CoUninitialize();
        PostMessageW(g_window, WM_APP_DONE, ok ? 1 : 0, 0);
    }).detach();
}

// ---------------------------------------------------------------------------------------------
// Wheel items + selection
// ---------------------------------------------------------------------------------------------
static void SetFile(const std::wstring& file)
{
    g_items.clear();
    g_source = SourceFormat(file);
    g_filename = ShortName(file);
    const bool image = IsImage(file);
    const bool document = IsDocument(file);
    const bool office = document && !LibreOfficePath().empty();
    for (int i = 0; i < static_cast<int>(kFormats.size()); ++i) {
        if (SameFormat(file, i)) continue;
        CLSID encoder;
        const bool imageOutput = kFormats[i].mime[0] != L'\0';
        bool ok = false;
        if (image) ok = imageOutput && EncoderClsid(kFormats[i].mime, &encoder);
        else if (document) ok = !imageOutput && office;
        if (ok) g_items.push_back(i);
    }
    g_selected = -1;
    g_wheelOverride = false;
    g_lastPt = { INT_MIN, INT_MIN };
    g_hoverAnim.fill(0.f);
    g_itemsStart = Now();
}

static void SetSelected(int s)
{
    if (s != g_selected) { g_selected = s; g_pulse = 1.0f; }
}

// -1 = hub (dead zone), -2 = outside the disc, otherwise the segment index
static int IndexAt(POINT p)
{
    const int n = static_cast<int>(g_items.size());
    if (!n || !g_winSize) return -2;
    const float half = g_winSize * 0.5f;
    const float dx = (p.x - (g_winX + half)) / g_scale;
    const float dy = (p.y - (g_winY + half)) / g_scale;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d > R_DISC) return -2;
    if (d < R_HUB) return -1;
    float a = std::atan2(dy, dx) + PI * 0.5f;          // 0 = top, clockwise
    a = std::fmod(a, 2.0f * PI);
    if (a < 0) a += 2.0f * PI;
    const float step = 2.0f * PI / n;
    return static_cast<int>(std::floor((a + step * 0.5f) / step)) % n;
}

static void SelectFromPoint(POINT p)
{
    if (p.x == g_lastPt.x && p.y == g_lastPt.y) return;   // only react to real movement
    g_lastPt = p;
    const int idx = IndexAt(p);
    if (idx >= 0) { SetSelected(idx); g_wheelOverride = false; }
    else if (idx == -2) { SetSelected(-1); g_wheelOverride = false; }
    else if (!g_wheelOverride) SetSelected(-1);
}

static void StepSelection(int delta)
{
    const int n = static_cast<int>(g_items.size());
    if (!n) return;
    const int next = g_selected < 0 ? (delta > 0 ? 0 : n - 1) : (((g_selected + delta) % n) + n) % n;
    g_wheelOverride = true;
    SetSelected(next);
}

// ---------------------------------------------------------------------------------------------
// Window placement / lifetime
// ---------------------------------------------------------------------------------------------
static void EnsureSurface(int size)
{
    if (g_dib && g_surfaceSize == size) return;
    if (g_dib) { SelectObject(g_memDC, g_oldBmp); DeleteObject(g_dib); g_dib = nullptr; }
    if (!g_memDC) {
        HDC screen = GetDC(nullptr);
        g_memDC = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_pixels, nullptr, 0);
    g_oldBmp = SelectObject(g_memDC, g_dib);
    g_surfaceSize = size;
}

static void PlaceCard(POINT point)
{
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = { sizeof(info) };
    GetMonitorInfoW(monitor, &info);
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        g_scale = static_cast<float>(dpiX) / 96.0f;

    const int size = static_cast<int>(WIN_SIZE * g_scale + 0.5f);
    int left = point.x - size / 2;
    int top = point.y - size / 2;
    left = std::max(static_cast<int>(info.rcWork.left), std::min(left, static_cast<int>(info.rcWork.right) - size));
    top = std::max(static_cast<int>(info.rcWork.top), std::min(top, static_cast<int>(info.rcWork.bottom) - size));
    g_winX = left; g_winY = top; g_winSize = size;
    EnsureSurface(size);

    g_visible = true;
    if (!g_shown) {
        g_theme = CurrentTheme();
        g_alpha = 0.f; g_pop = 0.8f; g_popVel = 0.f;
        g_pulse = 0.f; g_knobAlpha = 0.f;
        g_hoverAnim.fill(0.f);
        g_selected = -1; g_wheelOverride = false;
        g_lastPt = { INT_MIN, INT_MIN };
        g_wheelAccum = 0;
        g_itemsStart = Now();
        g_lastFrame = Now();
        g_shown = true;
    }
    SetWindowPos(g_window, HWND_TOPMOST, left, top, size, size, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void HideCard(const wchar_t* reason)
{
    if (!g_visible) return;
    Log(L"Card hidden: %s", reason);
    g_visible = false;     // fade-out is finished in FrameTick
}

static void FinishHide()
{
    ShowWindow(g_window, SW_HIDE);
    g_shown = false;
    g_items.clear();
    g_selected = -1;
    g_hoverAnim.fill(0.f);
    if (g_quitPending) PostQuitMessage(0);
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------
static void TextC(Graphics& g, const wchar_t* s, float cx, float cy, float w, float size,
    const Color& color, FontStyle style = FontStyleRegular)
{
    if (!g_fontFamily) return;
    Font font(g_fontFamily, size, style, UnitPixel);
    SolidBrush brush(color);
    StringFormat fmt(StringFormatFlagsNoWrap);
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(s, -1, &font, RectF(cx - w * 0.5f, cy - size, w, size * 2.0f), &fmt, &brush);
}

static void RenderCard()
{
    if (!g_window || !g_shown || !g_winSize) return;
    const int W = g_winSize;
    const Theme& T = g_theme;
    const int n = static_cast<int>(g_items.size());

    Bitmap surface(W, W, W * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(g_pixels));
    Graphics g(&surface);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    const float s = g_scale * Clamp(g_pop, 0.5f, 1.2f);
    g.TranslateTransform(W * 0.5f, W * 0.5f);
    g.ScaleTransform(s, s);

    // --- soft shadow --------------------------------------------------------------------------
    for (int k = 15; k >= 1; --k) {
        const float a = T.shadow.GetA() * std::pow(1.0f - k / 16.0f, 2.0f) * 0.22f;
        SolidBrush b(Color(static_cast<BYTE>(a), T.shadow.GetR(), T.shadow.GetG(), T.shadow.GetB()));
        const float r = R_DISC + k;
        g.FillEllipse(&b, -r, -r + 7.0f, r * 2.0f, r * 2.0f);
    }

    // --- glass disc -----------------------------------------------------------------------------
    {
        LinearGradientBrush bg(PointF(0, -R_DISC), PointF(0, R_DISC), T.panelTop, T.panelBottom);
        g.FillEllipse(&bg, -R_DISC, -R_DISC, R_DISC * 2.0f, R_DISC * 2.0f);
        Pen rim(T.rim, 1.0f);
        g.DrawEllipse(&rim, -R_DISC, -R_DISC, R_DISC * 2.0f, R_DISC * 2.0f);
        Pen track(Fade(T.rim, 0.55f), 2.0f);
        g.DrawEllipse(&track, -R_TRACK, -R_TRACK, R_TRACK * 2.0f, R_TRACK * 2.0f);
    }

    // --- scroll indicator (smooth orbiting knob + arc) -------------------------------------------
    const float step = n > 0 ? 360.0f / n : 360.0f;
    if (g_knobAlpha > 0.01f && n > 0) {
        const float sweep = std::min(step * 0.7f, 56.0f);
        Pen arc(Fade(T.accentA, g_knobAlpha), 3.5f);
        arc.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
        g.DrawArc(&arc, -R_TRACK, -R_TRACK, R_TRACK * 2.0f, R_TRACK * 2.0f, g_knob - sweep * 0.5f, sweep);
        const PointF kp = Polar(R_TRACK, g_knob);
        const Color kc = Mix(T.accentA, T.accentB, 0.5f);
        for (int k = 3; k >= 1; --k) {
            SolidBrush glow(Fade(kc, 0.10f * g_knobAlpha * (4 - k)));
            const float r = 4.5f + 3.0f * k;
            g.FillEllipse(&glow, kp.X - r, kp.Y - r, r * 2.0f, r * 2.0f);
        }
        SolidBrush dot(Fade(kc, g_knobAlpha));
        g.FillEllipse(&dot, kp.X - 4.5f, kp.Y - 4.5f, 9.0f, 9.0f);
    }

    // --- segments -------------------------------------------------------------------------------
    const double since = Now() - g_itemsStart;
    for (int i = 0; i < n; ++i) {
        const OutputFormat& f = kFormats[g_items[i]];
        const float h = Clamp(g_hoverAnim[i], 0.f, 1.f);
        const float e = EaseOutCubic(Clamp(static_cast<float>(since * 1000.0 - i * 35.0) / 280.0f, 0.f, 1.f));
        if (e <= 0.f) continue;

        const float centerDeg = -90.0f + i * step;
        const float ri = R_IN;
        const float roFull = R_OUT + 10.0f * h;
        const float ro = ri + 8.0f + (roFull - ri - 8.0f) * e;
        const float rm = (ri + ro) * 0.5f;
        const float angInset = (SEG_GAP * 0.5f + SEG_INSET) / rm * 180.0f / PI;
        const float start = centerDeg - step * 0.5f + angInset;
        const float sweep = std::max(step - 2.0f * angInset, 6.0f);
        const float rO = ro - SEG_INSET, rI = ri + SEG_INSET;

        GraphicsPath path;
        path.AddArc(RectF(-rO, -rO, rO * 2.0f, rO * 2.0f), start, sweep);
        path.AddArc(RectF(-rI, -rI, rI * 2.0f, rI * 2.0f), start + sweep, -sweep);
        path.CloseFigure();

        if (h > 0.02f) {
            for (int k = 1; k <= 3; ++k) {
                Pen glow(Fade(T.accentA, 0.15f * h / k), SEG_INSET * 2.0f + 7.0f * k);
                glow.SetLineJoin(LineJoinRound);
                g.DrawPath(&glow, &path);
            }
        }

        const PointF p1 = Polar(rI, centerDeg), p2 = Polar(rO, centerDeg);
        LinearGradientBrush br(p1, p2, Mix(T.segA, T.accentA, h), Mix(T.segB, T.accentB, h));
        Pen edge(&br, SEG_INSET * 2.0f);
        edge.SetLineJoin(LineJoinRound);
        g.FillPath(&br, &path);
        g.DrawPath(&edge, &path);

        const PointF lp = Polar(rm, centerDeg);
        TextC(g, f.label, lp.X, lp.Y - 6.0f - 1.5f * h, 96.0f, 15.0f + 2.0f * h,
            Fade(Mix(T.ink, T.onAccent, h), e), FontStyleBold);
        TextC(g, f.hint, lp.X, lp.Y + 12.0f, 96.0f, 9.5f,
            Fade(Mix(T.muted, Color(210, 255, 255, 255), h), e));
    }

    // --- hub -------------------------------------------------------------------------------------
    {
        const float hr = R_HUB * (1.0f + 0.05f * g_pulse);
        SolidBrush hubBrush(T.hub);
        g.FillEllipse(&hubBrush, -hr, -hr, hr * 2.0f, hr * 2.0f);
        Pen hubEdge(T.hubEdge, 1.0f);
        g.DrawEllipse(&hubEdge, -hr, -hr, hr * 2.0f, hr * 2.0f);

        const float rr = hr + 6.0f;
        if (g_phase == Phase::Busy) {
            Pen track(Fade(T.rim, 0.8f), 2.5f);
            g.DrawEllipse(&track, -rr, -rr, rr * 2.0f, rr * 2.0f);
            Pen spin(T.accentA, 3.0f);
            spin.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
            g.DrawArc(&spin, -rr, -rr, rr * 2.0f, rr * 2.0f, g_spin, 110.0f);
        } else if (g_phase == Phase::Success || g_phase == Phase::Failed) {
            Pen ring(g_phase == Phase::Success ? T.good : T.bad, 2.5f);
            g.DrawEllipse(&ring, -rr, -rr, rr * 2.0f, rr * 2.0f);
        } else if (g_knobAlpha > 0.01f) {
            Pen ring(Fade(Mix(T.accentA, T.accentB, 0.5f), 0.9f * g_knobAlpha), 2.0f);
            g.DrawEllipse(&ring, -rr, -rr, rr * 2.0f, rr * 2.0f);
        }

        const wchar_t* target = g_selected >= 0 && g_selected < n ? kFormats[g_items[g_selected]].label : L"";
        if (g_phase == Phase::Success || g_phase == Phase::Failed) {
            const bool ok = g_phase == Phase::Success;
            Pen mark(ok ? T.good : T.bad, 4.5f);
            mark.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
            mark.SetLineJoin(LineJoinRound);
            if (ok) {
                const PointF pts[3] = { PointF(-12, -9), PointF(-4, -1), PointF(12, -17) };
                g.DrawLines(&mark, pts, 3);
            } else {
                g.DrawLine(&mark, PointF(-8, -17), PointF(8, -1));
                g.DrawLine(&mark, PointF(8, -17), PointF(-8, -1));
            }
            TextC(g, ok ? L"Done" : L"Failed", 0, 15, 100, 17, ok ? T.good : T.bad, FontStyleBold);
            TextC(g, ok ? L"Saved next to original" : L"Couldn't convert", 0, 37, 108, 9, T.muted);
        } else if (g_phase == Phase::Busy) {
            TextC(g, L"CONVERTING", 0, -26, 100, 9.5f, T.muted, FontStyleBold);
            TextC(g, target, 0, -2, 100, 24, T.accentA, FontStyleBold);
            TextC(g, g_filename.c_str(), 0, 26, 112, 10, T.muted);
        } else if (g_selected >= 0) {
            const std::wstring from = g_source + L"  \u2192";
            TextC(g, from.c_str(), 0, -28, 100, 9.5f, T.muted, FontStyleBold);
            TextC(g, target, 0, -4, 100, 25, T.accentA, FontStyleBold);
            TextC(g, g_filename.c_str(), 0, 26, 112, 10, T.muted);
        } else if (n > 0) {
            TextC(g, L"CONVERT FROM", 0, -28, 100, 9.5f, T.muted, FontStyleBold);
            TextC(g, g_source.c_str(), 0, -4, 100, 25, T.ink, FontStyleBold);
            TextC(g, g_filename.c_str(), 0, 26, 112, 10, T.muted);
        } else {
            TextC(g, L"FLOWDROP", 0, -28, 100, 9.5f, T.accentA, FontStyleBold);
            TextC(g, L"Drop", 0, -4, 100, 25, T.ink, FontStyleBold);
            TextC(g, L"to convert", 0, 26, 100, 10, T.muted);
        }
    }

    POINT dst = { g_winX, g_winY }, src = { 0, 0 };
    SIZE size = { W, W };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, static_cast<BYTE>(255.0f * Clamp(g_alpha, 0.f, 1.f)), AC_SRC_ALPHA };
    UpdateLayeredWindow(g_window, nullptr, &dst, &size, g_memDC, &src, 0, &blend, ULW_ALPHA);
}

// ---------------------------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------------------------
static void Tick(float dt)
{
    g_alpha = Smooth(g_alpha, g_visible ? 1.0f : 0.0f, g_visible ? 16.0f : 20.0f, dt);

    // spring for the "pop" scale (slight overshoot = alive feeling)
    const float target = g_visible ? 1.0f : 0.92f;
    const float k = 420.0f, c = 26.0f;
    g_popVel += (k * (target - g_pop) - c * g_popVel) * dt;
    g_pop += g_popVel * dt;

    for (int i = 0; i < static_cast<int>(g_hoverAnim.size()); ++i)
        g_hoverAnim[i] = Smooth(g_hoverAnim[i], i == g_selected ? 1.0f : 0.0f, 20.0f, dt);

    g_pulse = Smooth(g_pulse, 0.0f, 9.0f, dt);
    g_spin += dt * 430.0f;
    if (g_spin > 360.f) g_spin -= 360.f;

    const int n = static_cast<int>(g_items.size());
    const bool has = g_selected >= 0 && n > 0;
    g_knobAlpha = Smooth(g_knobAlpha, has ? 1.0f : 0.0f, 14.0f, dt);
    if (has) {
        const float tgt = -90.0f + g_selected * (360.0f / n);
        if (g_knobAlpha < 0.05f) {
            g_knob = tgt;                                    // appear exactly on the segment
        } else {
            float d = std::fmod(tgt - g_knob, 360.0f);       // glide along the shortest way round
            if (d > 180.f) d -= 360.f;
            if (d < -180.f) d += 360.f;
            g_knob += d * (1.0f - std::exp(-18.0f * dt));
        }
    }

    if ((g_phase == Phase::Success || g_phase == Phase::Failed) && Now() - g_phaseTime > 0.85) {
        g_phase = Phase::Idle;
        if (g_contextMode) g_quitPending = true;
        HideCard(L"done");
    }
}

static void FrameTick()
{
    const double now = Now();
    const double elapsed = now - g_lastFrame;
    if (elapsed < 1.0 / 90.0) return;
    g_lastFrame = now;
    Tick(static_cast<float>(std::min(elapsed, 0.05)));
    if (!g_visible && g_alpha < 0.03f) { FinishHide(); return; }
    RenderCard();
}

// ---------------------------------------------------------------------------------------------
// OLE drop target
// ---------------------------------------------------------------------------------------------
static bool ExtractFiles(IDataObject* data, std::vector<std::wstring>& files)
{
    FORMATETC format = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = {};
    if (FAILED(data->GetData(&format, &medium))) return false;
    HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (!drop) { ReleaseStgMedium(&medium); return false; }
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring file(length + 1, L'\0');
        DragQueryFileW(drop, i, file.data(), length + 1);
        file.resize(length);
        files.push_back(std::move(file));
    }
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    return !files.empty();
}

class DropTarget final : public IDropTarget {
    ULONG references_ = 1;
    IDataObject* data_ = nullptr;

    void ReleaseData() { if (data_) { data_->Release(); data_ = nullptr; } }
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --references_; if (!r) delete this; return r; }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override
    {
        if (!effect) return E_INVALIDARG;
        *effect = DROPEFFECT_NONE;
        g_shift = (keys & MK_SHIFT) != 0;
        if (g_phase != Phase::Idle) return S_OK;
        if (!g_shift && !g_shiftConfirmed) { HideCard(L"shift required"); return S_OK; }

        ReleaseData();
        data_ = data; data_->AddRef();
        std::vector<std::wstring> files;
        if (!ExtractFiles(data, files)) return S_OK;
        SetFile(files.front());
        if (g_items.empty()) { HideCard(L"unsupported input"); return S_OK; }

        g_dragging = true;
        const POINT p = { point.x, point.y };
        if (!g_visible) PlaceCard(p);
        SelectFromPoint(p);
        *effect = g_selected >= 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD* effect) override
    {
        if (!effect) return E_INVALIDARG;
        g_shift = (keys & MK_SHIFT) != 0;
        if (!g_dragging || (!g_shift && !g_shiftConfirmed)) { *effect = DROPEFFECT_NONE; return S_OK; }
        SelectFromPoint({ point.x, point.y });
        *effect = g_selected >= 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        ReleaseData();
        g_dragging = false;
        HideCard(L"drag left");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override
    {
        if (!effect) return E_INVALIDARG;
        g_shift = (keys & MK_SHIFT) != 0;
        SelectFromPoint({ point.x, point.y });
        const int selected = g_cancelled ? -1 : g_selected;

        std::vector<std::wstring> files;
        bool ok = ExtractFiles(data, files) && selected >= 0 && selected < static_cast<int>(g_items.size())
            && g_phase == Phase::Idle;
        if (ok) for (const auto& f : files) if (!(IsImage(f) || IsDocument(f))) ok = false;

        ReleaseData();
        g_dragging = false;
        if (ok) {
            *effect = DROPEFFECT_COPY;
            StartConversion(std::move(files), g_items[selected]);   // returns immediately
        } else {
            *effect = DROPEFFECT_NONE;
            HideCard(L"drop cancelled");
        }
        return S_OK;
    }
};

// ---------------------------------------------------------------------------------------------
// Input hooks
// ---------------------------------------------------------------------------------------------
static void TriggerCheck(POINT p)
{
    if (g_contextMode || !g_leftDown || g_cancelled || !g_shiftConfirmed ||
        g_visible || g_dragging || g_phase != Phase::Idle) return;
    const int thr = GetSystemMetrics(SM_CXDRAG) + 2;
    const int dx = p.x - g_downPt.x, dy = p.y - g_downPt.y;
    if (dx * dx + dy * dy > thr * thr)
        PostMessageW(g_window, WM_APP_SHOW_CARD, static_cast<WPARAM>(static_cast<LONG_PTR>(p.x)),
            static_cast<LPARAM>(p.y));
}

static void ConfirmContext()
{
    if (g_selected < 0 || g_selected >= static_cast<int>(g_items.size())) {
        PostQuitMessage(0);
        return;
    }
    StartConversion(g_files, g_items[g_selected]);
}

static LRESULT CALLBACK MouseHook(int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION) {
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        if (message == WM_MOUSEWHEEL) {
            if (g_visible && (g_dragging || g_contextMode) && g_phase == Phase::Idle && !g_items.empty()) {
                g_wheelAccum += static_cast<short>(HIWORD(ms->mouseData));
                const int steps = g_wheelAccum / WHEEL_DELTA;
                if (steps) {
                    g_wheelAccum -= steps * WHEEL_DELTA;
                    StepSelection(-steps);                    // scroll down = clockwise
                }
                return 1;                                     // keep the page underneath from scrolling
            }
        } else if (g_contextMode) {
            if (g_visible && g_phase == Phase::Idle) {
                if (message == WM_MOUSEMOVE) {
                    SelectFromPoint(ms->pt);
                } else if (message == WM_LBUTTONUP && Now() - g_startTime > 0.35) {
                    SelectFromPoint(ms->pt);
                    ConfirmContext();
                }
            }
        } else {
            if (message == WM_LBUTTONDOWN) {
                g_leftDown = true;
                g_cancelled = false;
                g_downPt = ms->pt;
                g_shiftConfirmed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            } else if (message == WM_LBUTTONUP) {
                g_leftDown = false;
                g_shiftConfirmed = false;
                PostMessageW(g_window, WM_APP_HIDE_CARD, 0, 0);
            } else if (message == WM_MOUSEMOVE) {
                TriggerCheck(ms->pt);
            }
        }
    }
    return CallNextHookEx(g_mouseHook, code, message, data);
}

static LRESULT CALLBACK KeyboardHook(int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
        message == WM_KEYUP || message == WM_SYSKEYUP)) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;

        if (key->vkCode == VK_SHIFT || key->vkCode == VK_LSHIFT || key->vkCode == VK_RSHIFT) {
            g_shift = down;
            if (down && g_leftDown && !g_contextMode) {
                g_shiftConfirmed = true;
                POINT cursor;
                if (GetCursorPos(&cursor)) TriggerCheck(cursor);
            }
        }
        if (key->vkCode == VK_ESCAPE && down) {
            if (g_contextMode) { if (g_phase == Phase::Idle) PostQuitMessage(0); }
            else if (g_leftDown) {
                g_cancelled = true;
                PostMessageW(g_window, WM_APP_HIDE_CARD, 0, 0);
            }
        }
        if (g_contextMode && g_visible && g_phase == Phase::Idle && down) {
            switch (key->vkCode) {
            case VK_LEFT: case VK_UP:    StepSelection(-1); return 1;
            case VK_RIGHT: case VK_DOWN: StepSelection(+1); return 1;
            case VK_RETURN:              ConfirmContext();  return 1;
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, code, message, data);
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_APP_SHOW_CARD:
        if (g_leftDown && !g_visible && g_phase == Phase::Idle) {
            PlaceCard({ static_cast<LONG>(static_cast<LONG_PTR>(wParam)), static_cast<LONG>(lParam) });
            RenderCard();
        }
        return 0;
    case WM_APP_HIDE_CARD:
        if (!g_dragging && g_phase == Phase::Idle) HideCard(L"input ended");
        return 0;
    case WM_APP_DONE:
        g_phase = wParam ? Phase::Success : Phase::Failed;
        g_phaseTime = Now();
        g_pulse = 1.0f;
        return 0;
    case WM_NCHITTEST:   return HTCLIENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DESTROY:
        RevokeDragDrop(window);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    g_verbose = wcsstr(GetCommandLineW(), L"--console") != nullptr;

    const std::wstring commandLine = GetCommandLineW();
    const std::wstring contextToken = L"--context-file=\"";
    const size_t contextStart = commandLine.find(contextToken);
    if (contextStart != std::wstring::npos) {
        const size_t valueStart = contextStart + contextToken.size();
        const size_t valueEnd = commandLine.find(L'"', valueStart);
        if (valueEnd != std::wstring::npos) {
            g_contextMode = true;
            g_files.push_back(commandLine.substr(valueStart, valueEnd - valueStart));
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    OleInitialize(nullptr);
    GdiplusStartupInput startup;
    GdiplusStartup(&g_gdiplusToken, &startup, nullptr);

    g_fontFamily = new FontFamily(L"Segoe UI Variable Display");
    if (!g_fontFamily->IsAvailable()) { delete g_fontFamily; g_fontFamily = new FontFamily(L"Segoe UI"); }

    int exitCode = 0;
    WNDCLASSW windowClass = {};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = L"FlowDropQuickConvert";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);

    g_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        windowClass.lpszClassName, L"FlowDrop Quick Convert", WS_POPUP,
        0, 0, 100, 100, nullptr, nullptr, instance, nullptr);
    ShowWindow(g_window, SW_HIDE);

    auto* target = new DropTarget();
    const HRESULT registered = RegisterDragDrop(g_window, target);
    target->Release();
    if (FAILED(registered)) exitCode = 1;

    if (!exitCode && g_contextMode) {
        SetFile(g_files.front());
        if (g_items.empty()) exitCode = 2;
    }

    if (!exitCode) {
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, nullptr, 0);
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, nullptr, 0);

        if (g_contextMode) {
            POINT cursor;
            GetCursorPos(&cursor);
            g_startTime = Now();
            PlaceCard(cursor);
            g_itemsStart = Now();
            RenderCard();
        }

        MSG msg;
        bool running = true;
        while (running) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, g_shown ? 4 : INFINITE, QS_ALLINPUT);
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { running = false; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (running && g_shown) FrameTick();
        }
    }

    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
    if (g_dib) { SelectObject(g_memDC, g_oldBmp); DeleteObject(g_dib); }
    if (g_memDC) DeleteDC(g_memDC);
    delete g_fontFamily;
    GdiplusShutdown(g_gdiplusToken);
    OleUninitialize();
    timeEndPeriod(1);
    return exitCode;
}
