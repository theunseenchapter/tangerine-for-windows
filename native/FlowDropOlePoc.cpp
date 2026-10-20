#define UNICODE
#define _UNICODE
#include <windows.h>
#include <ole2.h>
#include <oleidl.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <vector>
#include <string>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <memory>
#include <climits>
#include <shellscalingapi.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

static constexpr UINT WM_APP_SHOW_WHEEL = WM_APP + 1;
static constexpr UINT WM_APP_HIDE_WHEEL = WM_APP + 2;
static constexpr UINT_PTR TIMER_INPUT = 1;
static constexpr UINT_PTR TIMER_ANIM = 2;

// ---------------------------------------------------------------------------
// Design tokens.  All sizes are expressed at 96 DPI and scaled once.
// The visible flower is 160 px across; the extra margin on every side belongs
// to the per-pixel-alpha surface and holds the soft shadow.
// ---------------------------------------------------------------------------
static constexpr int WHEEL_SIZE_96 = 192;
static constexpr float WHEEL_RADIUS_96 = 80.0f;
static constexpr float HUB_RADIUS_96 = 21.0f;
static constexpr float PETAL_INNER_RADIUS_96 = 27.0f;
static constexpr float PETAL_GAP_DEGREES = 3.6f;
static constexpr float PETAL_SWEEP_DEGREES = 45.0f - PETAL_GAP_DEGREES;
static constexpr float PI_F = 3.14159265359f;
static constexpr ULONGLONG OPEN_MS = 160;      // fade/scale-in duration
static constexpr float HOVER_EASE = 0.34f;     // per 16 ms frame
static constexpr float OPEN_START_SCALE = 0.90f;

struct Theme {
    Color face, faceUnavailable, border, ink, accentTop, accentBottom, accentInk, shadow;
};
static const Theme kDark = {
    Color(214, 36, 37, 42), Color(70, 36, 37, 42), Color(70, 255, 255, 255),
    Color(255, 236, 237, 241), Color(255, 255, 160, 82), Color(255, 245, 120, 40),
    Color(255, 255, 255, 255), Color(255, 0, 0, 0)
};
static const Theme kLight = {
    Color(222, 252, 252, 254), Color(78, 252, 252, 254), Color(60, 20, 22, 30),
    Color(255, 38, 40, 48), Color(255, 255, 160, 82), Color(255, 245, 120, 40),
    Color(255, 255, 255, 255), Color(255, 30, 20, 10)
};

static HWND g_window;
static bool g_dragging;
static bool g_leftButtonDown;
static bool g_shift;
static bool g_shiftActivated;
static bool g_cancelled;
static bool g_oleInteractionObserved;
static bool g_verbose;
static int g_hover = -1;
static bool g_supported[8] = {};
static float g_mix[8] = {};
static float g_scale = 1.0f;
static int g_left = INT_MIN, g_top = INT_MIN;
static ULONGLONG g_openStart;
static ULONG_PTR g_gdiplusToken;
static std::vector<std::wstring> g_files;
static std::wstring g_dragLabel = L"CONVERT";
static HHOOK g_mouseHook;
static HHOOK g_keyboardHook;
static bool g_previewMode;
static std::wstring g_sourceFormatLabel = L"FILE";

struct OutputFormat {
    const wchar_t* label;
    const wchar_t* extension;
    const wchar_t* mime;
};

// Document sources use the first five slots; image sources use the final
// three slots. Unsupported slots remain visible as structure but cannot be
// selected.
static constexpr std::array<OutputFormat, 8> kFormats = {{
    { L"PDF",  L".pdf",  L"" },
    { L"DOCX", L".docx", L"" },
    { L"XLSX", L".xlsx", L"" },
    { L"PPTX", L".pptx", L"" },
    { L"TXT",  L".txt",  L"" },
    { L"JPEG", L".jpg",  L"image/jpeg" },
    { L"BMP",  L".bmp",  L"image/bmp" },
    { L"TIFF", L".tiff", L"image/tiff" }
}};

static void RenderLayeredWheel();

static const Theme& CurrentTheme()
{
    DWORD light = 0, size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS) {
        light = 0;
    }
    return light ? kLight : kDark;
}

static float WheelScale() { return g_scale; }

static int WheelSize()
{
    return static_cast<int>(std::lround(WHEEL_SIZE_96 * g_scale));
}

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

static void ResetAnimationState()
{
    for (float& m : g_mix) m = 0.0f;
    g_hover = -1;
}

static void HideWheel(const wchar_t* reason)
{
    if (g_window && IsWindowVisible(g_window)) {
        Log(L"Wheel hidden: %s", reason);
        KillTimer(g_window, TIMER_ANIM);
        ShowWindow(g_window, SW_HIDE);
    }
    g_left = g_top = INT_MIN;
    ResetAnimationState();
}

static void StartAnimation()
{
    if (g_window) SetTimer(g_window, TIMER_ANIM, 16, nullptr);
}

static float OpenProgress()
{
    const ULONGLONG elapsed = GetTickCount64() - g_openStart;
    if (elapsed >= OPEN_MS) return 1.0f;
    const float t = static_cast<float>(elapsed) / static_cast<float>(OPEN_MS);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

static void SetHover(int hover)
{
    if (hover == g_hover) return;
    g_hover = hover;
    StartAnimation();
}

static int EncoderClsid(const wchar_t* mime, CLSID* clsid)
{
    UINT count = 0;
    UINT bytes = 0;
    GetImageEncodersSize(&count, &bytes);
    if (!bytes) return 0;
    std::vector<BYTE> buffer(bytes);
    auto* encoders = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    GetImageEncoders(count, bytes, encoders);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, mime) == 0) {
            *clsid = encoders[i].Clsid;
            return 1;
        }
    }
    return 0;
}

static const wchar_t* FormatName(int index)
{
    return index >= 0 && index < static_cast<int>(kFormats.size())
        ? kFormats[index].label : L"FILE";
}

static bool IsDocumentFile(const std::wstring& file)
{
    const wchar_t* extension = PathFindExtensionW(file.c_str());
    if (!extension) return false;
    return _wcsicmp(extension, L".pdf") == 0 ||
        _wcsicmp(extension, L".docx") == 0 ||
        _wcsicmp(extension, L".xlsx") == 0 ||
        _wcsicmp(extension, L".pptx") == 0 ||
        _wcsicmp(extension, L".txt") == 0;
}

static std::wstring FindLibreOffice()
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
    for (const auto& candidate : candidates) {
        if (PathFileExistsW(candidate.c_str())) return candidate;
    }
    return L"";
}

static bool IsSameFormat(const std::wstring& file, int format)
{
    const wchar_t* extension = PathFindExtensionW(file.c_str());
    if (!extension || format < 0 || format >= static_cast<int>(kFormats.size())) return false;
    return _wcsicmp(extension, kFormats[format].extension) == 0 ||
        (format == 5 && _wcsicmp(extension, L".jpeg") == 0);
}

static bool RunLibreOffice(const std::wstring& input, int format, const std::wstring& output)
{
    const std::wstring soffice = FindLibreOffice();
    if (soffice.empty()) {
        Log(L"LibreOffice was not found; set FLOWDROP_LIBREOFFICE or bundle libreoffice\\program\\soffice.exe");
        return false;
    }
    const wchar_t* filter = kFormats[format].label;
    wchar_t outputDirectory[MAX_PATH] = {};
    StringCchCopyW(outputDirectory, ARRAYSIZE(outputDirectory), output.c_str());
    PathRemoveFileSpecW(outputDirectory);
    wchar_t generated[MAX_PATH] = {};
    StringCchCopyW(generated, ARRAYSIZE(generated), input.c_str());
    PathRemoveExtensionW(generated);
    StringCchCatW(generated, ARRAYSIZE(generated), kFormats[format].extension);
    std::wstring command = L"\"" + soffice + L"\" --headless --convert-to ";
    command += filter;
    command += L" --outdir \"" + std::wstring(outputDirectory) + L"\"";
    command += L" \"" + input + L"\"";

    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        Log(L"Could not start LibreOffice (error=%lu)", GetLastError());
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0 || !PathFileExistsW(generated)) return false;
    if (!MoveFileExW(generated, output.c_str(), 0)) {
        Log(L"Could not move LibreOffice output to %s (error=%lu)",
            output.c_str(), GetLastError());
        return false;
    }
    return true;
}

static bool ConvertFile(const std::wstring& input, int format)
{
    if (format < 0 || format >= static_cast<int>(kFormats.size()) ||
        !kFormats[format].extension[0] || IsSameFormat(input, format)) return false;
    const auto& outputFormat = kFormats[format];
    wchar_t stem[MAX_PATH];
    StringCchCopyW(stem, ARRAYSIZE(stem), input.c_str());
    PathRemoveExtensionW(stem);
    // Originals and earlier conversions are never overwritten.
    wchar_t output[MAX_PATH];
    StringCchPrintfW(output, ARRAYSIZE(output), L"%s-converted%s", stem, outputFormat.extension);
    for (int n = 2; PathFileExistsW(output) && n < 1000; ++n) {
        StringCchPrintfW(output, ARRAYSIZE(output), L"%s-converted-%d%s", stem, n, outputFormat.extension);
    }
    if (PathFileExistsW(output)) return false;

    if (!outputFormat.mime[0]) return RunLibreOffice(input, format, output);
    CLSID encoder;
    if (!EncoderClsid(outputFormat.mime, &encoder)) return false;
    Bitmap bitmap(input.c_str(), FALSE);
    if (bitmap.GetLastStatus() != Ok) return false;
    return bitmap.Save(output, &encoder, nullptr) == Ok;
}

static bool ExtractFiles(IDataObject* data, std::vector<std::wstring>& files)
{
    FORMATETC format = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = {};
    if (FAILED(data->GetData(&format, &medium))) return false;

    HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (!drop) {
        ReleaseStgMedium(&medium);
        return false;
    }
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
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

static std::wstring HubLabelForFile(const std::wstring& file)
{
    if (file.empty()) return L"CONVERT";
    std::wstring name = PathFindFileNameW(file.c_str());
    const size_t extension = name.find_last_of(L'.');
    if (extension != std::wstring::npos) name.resize(extension);
    if (name.empty()) return L"CONVERT";
    // A concise, single line label remains readable in the 40 px hub.  The
    // original filename stays available in the console drag log.
    if (name.size() > 7) name = name.substr(0, 6) + L"…";
    return name;
}

static const wchar_t* SourceFormatName(const std::wstring& file)
{
    const wchar_t* extension = PathFindExtensionW(file.c_str());
    if (!extension) return L"FILE";
    if (_wcsicmp(extension, L".jpg") == 0 || _wcsicmp(extension, L".jpeg") == 0) return L"JPEG";
    if (_wcsicmp(extension, L".png") == 0) return L"PNG";
    if (_wcsicmp(extension, L".gif") == 0) return L"GIF";
    if (_wcsicmp(extension, L".bmp") == 0) return L"BMP";
    if (_wcsicmp(extension, L".tif") == 0 || _wcsicmp(extension, L".tiff") == 0) return L"TIFF";
    if (_wcsicmp(extension, L".webp") == 0) return L"WEBP";
    if (_wcsicmp(extension, L".heic") == 0) return L"HEIC";
    if (_wcsicmp(extension, L".avif") == 0) return L"AVIF";
    if (_wcsicmp(extension, L".pdf") == 0) return L"PDF";
    if (_wcsicmp(extension, L".docx") == 0) return L"DOCX";
    if (_wcsicmp(extension, L".xlsx") == 0) return L"XLSX";
    if (_wcsicmp(extension, L".pptx") == 0) return L"PPTX";
    if (_wcsicmp(extension, L".txt") == 0) return L"TXT";
    return L"FILE";
}

static void AddPetalPath(GraphicsPath& path, int index)
{
    const float centerAngle = -90.0f + index * 45.0f;
    const float halfSweep = PETAL_SWEEP_DEGREES / 2.0f;
    const float startAngle = centerAngle - halfSweep;
    const float endAngle = centerAngle + halfSweep;
    const float scale = WheelScale();
    const float wheelSize = static_cast<float>(WheelSize());
    const PointF center(wheelSize / 2.0f, wheelSize / 2.0f);
    const float outer = WHEEL_RADIUS_96 * scale;
    const float inner = PETAL_INNER_RADIUS_96 * scale;
    const float radians = PI_F / 180.0f;
    const auto pointAt = [&](float radius, float angle) {
        return PointF(center.X + radius * std::cos(angle * radians),
            center.Y + radius * std::sin(angle * radians));
    };
    const PointF innerStart = pointAt(inner, centerAngle - halfSweep * 0.62f);
    const PointF outerStart = pointAt(outer - 1.0f * scale, startAngle);
    const PointF outerEnd = pointAt(outer - 1.0f * scale, endAngle);
    const PointF innerEnd = pointAt(inner, centerAngle + halfSweep * 0.62f);

    path.StartFigure();
    path.AddBezier(innerStart,
        pointAt(inner + 10.0f * scale, centerAngle - halfSweep * 0.94f),
        pointAt(outer - 19.0f * scale, centerAngle - halfSweep * 1.10f), outerStart);
    path.AddBezier(outerStart,
        pointAt(outer + 4.0f * scale, centerAngle - halfSweep * 0.60f),
        pointAt(outer + 4.0f * scale, centerAngle + halfSweep * 0.60f), outerEnd);
    path.AddBezier(outerEnd,
        pointAt(outer - 19.0f * scale, centerAngle + halfSweep * 1.10f),
        pointAt(inner + 10.0f * scale, centerAngle + halfSweep * 0.94f), innerEnd);
    path.AddBezier(innerEnd,
        pointAt(inner - 1.5f * scale, centerAngle + halfSweep * 0.35f),
        pointAt(inner - 1.5f * scale, centerAngle - halfSweep * 0.35f), innerStart);
    path.CloseFigure();
}

// One set of petal paths per DPI scale; painting and hit testing share it, so
// the visible region and the selectable region cannot drift apart.
static std::array<std::unique_ptr<GraphicsPath>, 8> g_petals;
static float g_petalScale = 0.0f;

static const GraphicsPath& Petal(int index)
{
    if (g_petalScale != g_scale) {
        for (auto& p : g_petals) p.reset();
        g_petalScale = g_scale;
    }
    if (!g_petals[index]) {
        g_petals[index] = std::make_unique<GraphicsPath>();
        AddPetalPath(*g_petals[index], index);
    }
    return *g_petals[index];
}

static bool IsSupportedImageFile(const std::wstring& file)
{
    const wchar_t* extension = PathFindExtensionW(file.c_str());
    if (!extension) return false;
    return _wcsicmp(extension, L".jpg") == 0 ||
        _wcsicmp(extension, L".jpeg") == 0 ||
        _wcsicmp(extension, L".png") == 0 ||
        _wcsicmp(extension, L".bmp") == 0 ||
        _wcsicmp(extension, L".gif") == 0 ||
        _wcsicmp(extension, L".tif") == 0 ||
        _wcsicmp(extension, L".tiff") == 0;
}

static void SetSupportedFormats(const std::wstring& file)
{
    for (bool& supported : g_supported) supported = false;
    g_sourceFormatLabel = SourceFormatName(file);
    const bool image = IsSupportedImageFile(file);
    const bool document = IsDocumentFile(file);
    if (!image && !document) return;
    const bool libreOfficeAvailable = document && !FindLibreOffice().empty();
    for (int i = 0; i < static_cast<int>(kFormats.size()); ++i) {
        CLSID encoder;
        const bool imageOutput = kFormats[i].mime[0] != L'\0';
        const bool documentOutput = !imageOutput;
        const bool available = image
            ? imageOutput && EncoderClsid(kFormats[i].mime, &encoder) != 0
            : documentOutput && libreOfficeAvailable;
        g_supported[i] = available && !IsSameFormat(file, i);
    }
}

static bool HasSupportedFormat()
{
    for (bool supported : g_supported) {
        if (supported) return true;
    }
    return false;
}

// Before Explorer has entered our HWND there is no IDataObject to inspect.
// Show only formats the installed native engine can actually encode, then
// replace this provisional set as soon as DragEnter supplies the real file.
static void SetProvisionalNativeFormats()
{
    for (bool& supported : g_supported) supported = false;
    for (int i = 0; i < static_cast<int>(kFormats.size()); ++i) {
        CLSID encoder;
        g_supported[i] = (kFormats[i].mime[0] && EncoderClsid(kFormats[i].mime, &encoder) != 0) ||
            (!kFormats[i].mime[0] && !FindLibreOffice().empty());
    }
    g_sourceFormatLabel = L"FILE";
    g_dragLabel = L"CONVERT";
}

static int FormatAt(POINTL point)
{
    RECT rect;
    GetWindowRect(g_window, &rect);
    const PointF local(
        static_cast<REAL>(point.x - rect.left),
        static_cast<REAL>(point.y - rect.top));
    for (int i = 0; i < 8; ++i) {
        if (!g_supported[i]) continue;
        if (Petal(i).IsVisible(local)) return i;
    }
    return -1;
}

// Returns true when the surface moved or was just shown.
static bool PlaceWheel(POINTL point)
{
    const POINT cursor = { point.x, point.y };
    HMONITOR hmon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor = { sizeof(monitor) };
    GetMonitorInfoW(hmon, &monitor);
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        g_scale = static_cast<float>(dpiX) / 96.0f;
    }
    const RECT& workArea = monitor.rcWork;
    const int wheelSize = WheelSize();
    // Keep the cursor in the hub; a native OLE target gets DragLeave if the
    // window jumps away from the pointer.
    const int desiredLeft = static_cast<int>(point.x) - wheelSize / 2;
    const int desiredTop = static_cast<int>(point.y) - wheelSize / 2;
    const int maxLeft = static_cast<int>(workArea.right) - wheelSize;
    const int maxTop = static_cast<int>(workArea.bottom) - wheelSize;
    const int left = desiredLeft < workArea.left ? workArea.left
        : (desiredLeft > maxLeft ? maxLeft : desiredLeft);
    const int top = desiredTop < workArea.top ? workArea.top
        : (desiredTop > maxTop ? maxTop : desiredTop);

    const bool wasVisible = IsWindowVisible(g_window) != FALSE;
    if (wasVisible && left == g_left && top == g_top) return false;
    if (!wasVisible) {
        ResetAnimationState();
        g_openStart = GetTickCount64();
        StartAnimation();
    }
    g_left = left;
    g_top = top;
    SetWindowPos(g_window, HWND_TOPMOST, left, top, wheelSize, wheelSize,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

static void UpdateWheelFromInput()
{
    if (!g_window) return;
    const bool dragging = g_leftButtonDown;
    if (g_cancelled || (!dragging && !g_dragging) ||
        (!g_shiftActivated && !g_dragging)) {
        if (!dragging) g_oleInteractionObserved = false;
        PostMessageW(g_window, WM_APP_HIDE_WHEEL, 0, 0);
        return;
    }
    POINT cursor;
    if (!g_dragging && dragging && g_shiftActivated && !g_oleInteractionObserved) {
        SetProvisionalNativeFormats();
    }
    // Once OLE has entered, the wheel stays anchored so a petal can be reached.
    if (!g_dragging && HasSupportedFormat() && GetCursorPos(&cursor)) {
        PostMessageW(g_window, WM_APP_SHOW_WHEEL, static_cast<WPARAM>(cursor.x),
            static_cast<LPARAM>(cursor.y));
    }
}

static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION &&
        (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
        if (message == WM_LBUTTONDOWN) {
            g_leftButtonDown = true;
            g_shiftActivated = false;
            g_oleInteractionObserved = false;
            g_cancelled = false;
        }
        if (message == WM_LBUTTONUP) {
            g_leftButtonDown = false;
            g_shiftActivated = false;
            g_cancelled = false;
        }
        UpdateWheelFromInput();
    }
    return CallNextHookEx(g_mouseHook, code, message, data);
}

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
        message == WM_KEYUP || message == WM_SYSKEYUP)) {
        const auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        if (key->vkCode == VK_SHIFT || key->vkCode == VK_LSHIFT || key->vkCode == VK_RSHIFT) {
            g_shift = down;
            if (down && g_leftButtonDown) g_shiftActivated = true;
        }
        // Escape cancels the wheel until the button is released.
        if (key->vkCode == VK_ESCAPE && down && g_leftButtonDown) g_cancelled = true;
        UpdateWheelFromInput();
    }
    return CallNextHookEx(g_keyboardHook, code, message, data);
}

static Color Mix(const Color& a, const Color& b, float t)
{
    const auto lerp = [&](BYTE x, BYTE y) {
        return static_cast<BYTE>(std::lround(x + (y - x) * t));
    };
    return Color(lerp(a.GetA(), b.GetA()), lerp(a.GetR(), b.GetR()),
        lerp(a.GetG(), b.GetG()), lerp(a.GetB(), b.GetB()));
}

static Color WithAlpha(const Color& c, BYTE alpha)
{
    return Color(alpha, c.GetR(), c.GetG(), c.GetB());
}

static void PaintWheel(Graphics& graphics, const Theme& theme, float openT)
{
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);
    // Grayscale AA: ClearType cannot be used on a per-pixel-alpha surface.
    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

    const float scale = WheelScale();
    const float wheelSize = static_cast<float>(WheelSize());
    const float hubRadius = HUB_RADIUS_96 * scale;
    const PointF center(wheelSize / 2.0f, wheelSize / 2.0f);

    const float s = OPEN_START_SCALE + (1.0f - OPEN_START_SCALE) * openT;
    graphics.TranslateTransform(center.X, center.Y);
    graphics.ScaleTransform(s, s);
    graphics.TranslateTransform(-center.X, -center.Y);

    // Shadow lives only outside the silhouette so the translucent face stays
    // clean and the desktop shows through undistorted.
    GraphicsPath silhouette;
    for (int i = 0; i < 8; ++i) AddPetalPath(silhouette, i);
    GraphicsPath hubDisc;
    hubDisc.AddEllipse(center.X - hubRadius, center.Y - hubRadius, hubRadius * 2, hubRadius * 2);
    graphics.SetClip(&silhouette, CombineModeExclude);
    graphics.TranslateTransform(0.0f, 3.0f * scale);
    for (int k = 6; k >= 1; --k) {
        Pen soft(WithAlpha(theme.shadow, static_cast<BYTE>(5 + (7 - k))), k * 2.4f * scale);
        soft.SetLineJoin(LineJoinRound);
        graphics.DrawPath(&soft, &silhouette);
    }
    graphics.TranslateTransform(0.0f, -3.0f * scale);
    graphics.ResetClip();

    Font font(L"Segoe UI", 9.5f * scale, FontStyleBold, UnitPixel);
    StringFormat centered;
    centered.SetAlignment(StringAlignmentCenter);
    centered.SetLineAlignment(StringAlignmentCenter);

    for (int i = 0; i < 8; ++i) {
        const GraphicsPath& petal = Petal(i);
        const float m = g_mix[i];
        const Color base = g_supported[i] ? theme.face : theme.faceUnavailable;
        Color fill = base;
        if (g_supported[i] && m > 0.001f) {
            fill = Mix(base, Color(244, 255, 150, 70), m);
        }
        SolidBrush brush(fill);
        graphics.FillPath(&brush, &petal);
        Pen border(Mix(theme.border, Color(190, 255, 190, 140), m), 1.0f * scale);
        graphics.DrawPath(&border, &petal);

        // Blank petals are structure only; just supported encoders get labels.
        if (!g_supported[i]) continue;
        const float angle = (-90.0f + i * 45.0f) * PI_F / 180.0f;
        const float labelRadius = 53.0f * scale;
        const PointF at(center.X + labelRadius * std::cos(angle),
            center.Y + labelRadius * std::sin(angle));
        SolidBrush text(Mix(theme.ink, theme.accentInk, m));
        graphics.DrawString(FormatName(i), -1, &font,
            RectF(at.X - 24.0f * scale, at.Y - 9.0f * scale, 48.0f * scale, 18.0f * scale),
            &centered, &text);
    }

    // Hub: accent gradient disc with a hairline ring.
    LinearGradientBrush hubBrush(
        PointF(center.X, center.Y - hubRadius), PointF(center.X, center.Y + hubRadius),
        theme.accentTop, theme.accentBottom);
    graphics.FillPath(&hubBrush, &hubDisc);
    Pen ring(Color(110, 255, 255, 255), 1.0f * scale);
    graphics.DrawPath(&ring, &hubDisc);

    Font fileFont(L"Segoe UI", 7.5f * scale, FontStyleBold, UnitPixel);
    SolidBrush fileText(theme.accentInk);
    std::wstring hubText = g_sourceFormatLabel == L"FILE"
        ? L"CONVERT"
        : g_sourceFormatLabel + L" \x2192";
    graphics.DrawString(hubText.c_str(), -1, &fileFont,
        RectF(center.X - hubRadius + 3.0f * scale, center.Y - 7.0f * scale,
            hubRadius * 2.0f - 6.0f * scale, 14.0f * scale),
        &centered, &fileText);
}

// Draws straight into the premultiplied-alpha DIB used by UpdateLayeredWindow,
// so transparency is real per-pixel alpha, not a color key.
static void RenderLayeredWheel()
{
    if (!g_window || !IsWindowVisible(g_window)) return;
    const int size = WheelSize();
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    ZeroMemory(pixels, static_cast<size_t>(size) * size * 4);
    const float openT = OpenProgress();
    {
        Bitmap surface(size, size, size * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
        Graphics graphics(&surface);
        PaintWheel(graphics, CurrentTheme(), openT);
    }

    RECT rect;
    GetWindowRect(g_window, &rect);
    POINT destination = { rect.left, rect.top };
    POINT source = { 0, 0 };
    SIZE surfaceSize = { size, size };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255.0f * openT)), AC_SRC_ALPHA };
    UpdateLayeredWindow(g_window, screen, &destination, &surfaceSize, memory,
        &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

// Advances open and hover easing. Returns true while anything still moves.
static bool TickAnimation()
{
    bool active = OpenProgress() < 1.0f;
    for (int i = 0; i < 8; ++i) {
        const float target = (i == g_hover && g_supported[i]) ? 1.0f : 0.0f;
        const float delta = target - g_mix[i];
        if (std::fabs(delta) > 0.01f) {
            g_mix[i] += delta * HOVER_EASE;
            active = true;
        } else {
            g_mix[i] = target;
        }
    }
    return active;
}

class DropTarget final : public IDropTarget {
    ULONG references_ = 1;
    IDataObject* data_ = nullptr;

public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG result = --references_;
        if (!result) delete this;
        return result;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override
    {
        if (!effect) return E_INVALIDARG;
        g_oleInteractionObserved = true;
        g_shift = (keys & MK_SHIFT) != 0;
        if (!g_shiftActivated && !g_shift) {
            Log(L"DragEnter ignored: Shift activation required");
            g_oleInteractionObserved = false;
            HideWheel(L"Shift required");
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        if (data_) data_->Release();
        data_ = data;
        data_->AddRef();
        g_files.clear();
        ExtractFiles(data, g_files);
        g_dragLabel = g_files.empty() ? L"CONVERT" : HubLabelForFile(g_files.front());
        SetSupportedFormats(g_files.empty() ? L"" : g_files.front());
        if (!HasSupportedFormat()) {
            Log(L"No native conversion formats available for %s", g_dragLabel.c_str());
            g_dragging = false;
            HideWheel(L"unsupported input");
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        Log(L"Conversion options ready for %s (JPEG=%d BMP=%d TIFF=%d)",
            g_sourceFormatLabel.c_str(), g_supported[1], g_supported[5], g_supported[7]);
        g_dragging = true;
        PlaceWheel(point);
        SetHover(FormatAt(point));
        *effect = g_hover >= 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        RenderLayeredWheel();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD* effect) override
    {
        if (!effect) return E_INVALIDARG;
        g_shift = (keys & MK_SHIFT) != 0;
        if (!g_shiftActivated && !g_shift) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        const int hover = FormatAt(point);
        if (hover != g_hover) {
            SetHover(hover);
            RenderLayeredWheel();
        }
        *effect = g_hover >= 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        Log(L"DragLeave");
        if (data_) {
            data_->Release();
            data_ = nullptr;
        }
        g_dragging = false;
        for (bool& supported : g_supported) supported = false;
        g_sourceFormatLabel = L"FILE";
        g_dragLabel = L"CONVERT";
        HideWheel(L"DragLeave");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override
    {
        if (!effect) return E_INVALIDARG;
        g_shift = (keys & MK_SHIFT) != 0;
        const int selected = g_cancelled ? -1 : FormatAt(point);
        g_hover = selected;
        std::vector<std::wstring> files;
        bool extracted = ExtractFiles(data, files);
        Log(L"Drop shift=%d hover=%d files=%zu", g_shift, g_hover, files.size());
        if (extracted && selected >= 0) {
            Log(L"Conversion selected: %s", FormatName(selected));
            bool convertedAll = true;
            for (const auto& file : files) {
                const bool convertible = IsSupportedImageFile(file) || IsDocumentFile(file);
                const bool converted = convertible && ConvertFile(file, selected);
                convertedAll = convertedAll && converted;
                Log(L"  %s | Output: %s | Status: %s", file.c_str(), FormatName(selected),
                    converted ? L"Converted successfully"
                              : (convertible ? L"Conversion failed" : L"Unsupported input format"));
            }
            *effect = convertedAll ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        } else {
            *effect = DROPEFFECT_NONE;
        }
        if (data_) {
            data_->Release();
            data_ = nullptr;
        }
        g_dragging = false;
        for (bool& supported : g_supported) supported = false;
        g_sourceFormatLabel = L"FILE";
        HideWheel(selected >= 0 ? L"drop completed" : L"drop outside petal");
        return S_OK;
    }
};

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_APP_SHOW_WHEEL) {
        if (!g_shiftActivated && !g_dragging) {
            HideWheel(L"activation not confirmed");
            return 0;
        }
        POINTL point = { static_cast<LONG>(wParam), static_cast<LONG>(lParam) };
        const bool wasVisible = IsWindowVisible(window) != FALSE;
        if (PlaceWheel(point)) {
            if (!wasVisible) Log(L"Wheel shown x=%ld y=%ld", point.x, point.y);
            RenderLayeredWheel();
        }
        return 0;
    }
    if (message == WM_APP_HIDE_WHEEL && !g_dragging) {
        const bool dragging = g_leftButtonDown;
        if (g_cancelled) HideWheel(L"cancelled");
        else if (!dragging) HideWheel(L"left button released");
        else if (!g_shiftActivated) HideWheel(L"Shift required");
        return 0;
    }
    if (message == WM_TIMER) {
        if (wParam == TIMER_ANIM) {
            if (!IsWindowVisible(window)) { KillTimer(window, TIMER_ANIM); return 0; }
            const bool active = TickAnimation();
            RenderLayeredWheel();
            if (!active) KillTimer(window, TIMER_ANIM);
        } else {
            UpdateWheelFromInput();
        }
        return 0;
    }
    if (message == WM_DPICHANGED) return 0;
    if (message == WM_NCHITTEST) {
        POINT cursor;
        GetCursorPos(&cursor);
        RECT rect;
        GetWindowRect(window, &rect);
        const PointF local(static_cast<REAL>(cursor.x - rect.left),
            static_cast<REAL>(cursor.y - rect.top));
        const float hubRadius = HUB_RADIUS_96 * WheelScale();
        const float center = WheelSize() / 2.0f;
        const float x = local.X - center;
        const float y = local.Y - center;
        if (x * x + y * y <= hubRadius * hubRadius) return HTCLIENT;
        for (int i = 0; i < 8; ++i) {
            if (!g_supported[i]) continue;
            if (Petal(i).IsVisible(local)) return HTCLIENT;
        }
        return HTTRANSPARENT;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DESTROY) {
        RevokeDragDrop(window);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    g_verbose = wcsstr(GetCommandLineW(), L"--console") != nullptr;
    if (g_verbose) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        Log(L"FlowDrop OLE drop-target POC (verbose)");
    }
    wchar_t executablePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, executablePath, ARRAYSIZE(executablePath));
    g_previewMode = wcsstr(GetCommandLineW(), L"--preview-wheel") != nullptr ||
        wcsstr(executablePath, L".preview.exe") != nullptr;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    GdiplusStartupInput startup;
    GdiplusStartup(&g_gdiplusToken, &startup, nullptr);

    WNDCLASSW windowClass = {};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = L"FlowDropOlePoc";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);

    g_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        windowClass.lpszClassName, L"FlowDrop OLE POC", WS_POPUP,
        500, 300, WHEEL_SIZE_96, WHEEL_SIZE_96, nullptr, nullptr, instance, nullptr);
    ShowWindow(g_window, SW_HIDE);
    if (!g_previewMode) SetTimer(g_window, TIMER_INPUT, 50, nullptr);
    UpdateWindow(g_window);

    auto* target = new DropTarget();
    HRESULT registered = RegisterDragDrop(g_window, target);
    Log(L"RegisterDragDrop: 0x%08X", registered);
    target->Release();
    if (FAILED(registered)) return 1;

    if (g_previewMode) {
        SetProvisionalNativeFormats();
        g_dragLabel = L"SUNSET";
        POINT cursor;
        GetCursorPos(&cursor);
        PlaceWheel({ cursor.x, cursor.y });
        RenderLayeredWheel();
        Log(L"Preview wheel shown. Close the process to exit preview mode.");
    } else {
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
        Log(L"Input observers: mouse=%s keyboard=%s", g_mouseHook ? L"OK" : L"FAILED",
            g_keyboardHook ? L"OK" : L"FAILED");
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
    GdiplusShutdown(g_gdiplusToken);
    OleUninitialize();
    return 0;
}
