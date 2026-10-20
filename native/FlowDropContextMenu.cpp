#define UNICODE
#define _UNICODE
#define NOMINMAX
#define DllCanUnloadNow FlowDropSystemDllCanUnloadNow
#define DllGetClassObject FlowDropSystemDllGetClassObject

#include <windows.h>
#include <ole2.h>
#include <oleidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>
#include <string>
#include <vector>
#undef DllCanUnloadNow
#undef DllGetClassObject

static const CLSID CLSID_FlowDropContextMenu =
    { 0x8f4e9d31, 0x4b9b, 0x4e5a, { 0x9c, 0x7e, 0x2d, 0x39, 0x0b, 0x6f, 0x4a, 0x12 } };

static long g_objects = 0;
static HINSTANCE g_instance = nullptr;

static std::wstring ModulePath()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_instance, path, ARRAYSIZE(path));
    return path;
}

static std::wstring HelperPath()
{
    std::wstring path = ModulePath();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"FlowDropOlePoc.exe";
    return path.substr(0, slash + 1) + L"FlowDropOlePoc.exe";
}

static bool LaunchHelper(const std::wstring& file)
{
    const std::wstring helper = HelperPath();
    if (GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring command = L"\"" + helper + L"\" --context-file=\"" + file + L"\"";
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

class FlowDropContextMenu final : public IShellExtInit, public IContextMenu
{
    ULONG references_ = 1;
    std::wstring file_;
    bool launched_ = false;

public:
    FlowDropContextMenu() { ++g_objects; }
    ~FlowDropContextMenu() { --g_objects; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IShellExtInit) *object = static_cast<IShellExtInit*>(this);
        else if (iid == IID_IContextMenu) *object = static_cast<IContextMenu*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG result = --references_;
        if (!result) delete this;
        return result;
    }

    HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE, IDataObject* data, HKEY) override
    {
        if (!data) return E_INVALIDARG;
        FORMATETC format = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium = {};
        if (FAILED(data->GetData(&format, &medium))) return E_FAIL;
        HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
        if (!drop) { ReleaseStgMedium(&medium); return E_FAIL; }
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, path, ARRAYSIZE(path))) file_ = path;
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        return file_.empty() ? E_FAIL : S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT first, UINT, UINT flags) override
    {
        if (!(flags & CMF_DEFAULTONLY) && !launched_ && !file_.empty()) {
            launched_ = true;
            LaunchHelper(file_);
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(LPCMINVOKECOMMANDINFO) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) override { return E_NOTIMPL; }
};

class FlowDropClassFactory final : public IClassFactory
{
    ULONG references_ = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *object = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG result = --references_; if (!result) delete this; return result; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** object) override
    {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* menu = new FlowDropContextMenu();
        const HRESULT result = menu->QueryInterface(iid, object);
        menu->Release();
        return result;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { if (lock) ++g_objects; else --g_objects; return S_OK; }
};

static std::wstring GuidString()
{
    wchar_t value[64] = {};
    StringFromGUID2(CLSID_FlowDropContextMenu, value, ARRAYSIZE(value));
    return value;
}

static HRESULT SetRegistryValue(const std::wstring& key, const wchar_t* name, const std::wstring& value)
{
    HKEY handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
        return E_FAIL;
    const LONG result = RegSetValueExW(handle, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    return result == ERROR_SUCCESS ? S_OK : E_FAIL;
}

extern "C" HRESULT __declspec(dllexport) __stdcall DllRegisterServer()
{
    const std::wstring clsid = GuidString();
    const std::wstring module = ModulePath();
    if (FAILED(SetRegistryValue(L"Software\\Classes\\CLSID\\" + clsid, nullptr, L"FlowDrop Hover Convert")))
        return E_FAIL;
    if (FAILED(SetRegistryValue(L"Software\\Classes\\CLSID\\" + clsid + L"\\InprocServer32", nullptr, module)))
        return E_FAIL;
    if (FAILED(SetRegistryValue(L"Software\\Classes\\CLSID\\" + clsid + L"\\InprocServer32", L"ThreadingModel", L"Apartment")))
        return E_FAIL;
    return SetRegistryValue(
        L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\FlowDropHover",
        nullptr, clsid);
}

extern "C" HRESULT __declspec(dllexport) __stdcall DllUnregisterServer()
{
    const std::wstring clsid = GuidString();
    RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\CLSID\\" + clsid).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\FlowDropHover");
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void)
{
    return g_objects == 0 ? S_OK : S_FALSE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** object)
{
    if (clsid != CLSID_FlowDropContextMenu) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new FlowDropClassFactory();
    const HRESULT result = factory->QueryInterface(iid, object);
    factory->Release();
    return result;
}

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
