// COM entry points for the fyewebcam virtual camera. The Frame Server creates the class below,
// which is an IMFActivate that hands out the media source.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>

#include <atomic>
#include <new>
#include <string>

#include "../src/shared_frame.h"
#include "media_source.h"

namespace {

// {58E7AF15-C40C-437E-9DEA-FDD8E15057D6}, matches shared_frame::kSourceClsidString.
const CLSID kSourceClsid = {0x58e7af15, 0xc40c, 0x437e, {0x9d, 0xea, 0xfd, 0xd8, 0xe1, 0x50, 0x57, 0xd6}};

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

// IMFActivate whose attribute store is a plain MF attribute object.
class Activator final : public IMFActivate {
public:
    Activator() { ++g_objects; }

    HRESULT Initialize() { return MFCreateAttributes(attrs_.Put(), 4); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) {
            *ppv = static_cast<IMFActivate*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        auto* source = new (std::nothrow) MediaSource();
        if (!source) return E_OUTOFMEMORY;
        HRESULT hr = source->Initialize(attrs_.Get());
        if (SUCCEEDED(hr)) hr = source->QueryInterface(riid, ppv);
        source->Release();
        return hr;
    }
    STDMETHODIMP ShutdownObject() override { return S_OK; }
    STDMETHODIMP DetachObject() override { return S_OK; }

    // IMFAttributes, forwarded to the attribute store.
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attrs_->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attrs_->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attrs_->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override {
        return attrs_->Compare(o, m, r);
    }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attrs_->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attrs_->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return attrs_->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attrs_->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return attrs_->GetStringLength(k, l); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR v, UINT32 n, UINT32* l) override { return attrs_->GetString(k, v, n, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* v, UINT32* l) override {
        return attrs_->GetAllocatedString(k, v, l);
    }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* s) override { return attrs_->GetBlobSize(k, s); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* s) override { return attrs_->GetBlob(k, b, n, s); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) override { return attrs_->GetAllocatedBlob(k, b, s); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* p) override { return attrs_->GetUnknown(k, r, p); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attrs_->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return attrs_->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return attrs_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attrs_->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attrs_->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return attrs_->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attrs_->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attrs_->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return attrs_->SetBlob(k, b, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* p) override { return attrs_->SetUnknown(k, p); }
    STDMETHODIMP LockStore() override { return attrs_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return attrs_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* n) override { return attrs_->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return attrs_->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return attrs_->CopyAllItems(d); }

private:
    ~Activator() { --g_objects; }

    std::atomic<ULONG> refs_{1};
    ComPtr<IMFAttributes> attrs_;
};

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // static instance
    STDMETHODIMP_(ULONG) Release() override { return 1; }  // static instance

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* activator = new (std::nothrow) Activator();
        if (!activator) return E_OUTOFMEMORY;
        HRESULT hr = activator->Initialize();
        if (SUCCEEDED(hr)) hr = activator->QueryInterface(riid, ppv);
        activator->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            ++g_locks;
        } else {
            --g_locks;
        }
        return S_OK;
    }
};

ClassFactory g_factory;

std::wstring ClsidKey() { return std::wstring(L"Software\\Classes\\CLSID\\") + shared_frame::kSourceClsidString; }

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (clsid != kSourceClsid) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

// Registers the media source for all users (HKLM), which the Frame Server service needs. Requires
// administrator rights.
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return HRESULT_FROM_WIN32(GetLastError());

    HKEY key = nullptr;
    const std::wstring inproc = ClsidKey() + L"\\InprocServer32";
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, inproc.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    st = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path),
                        static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    const wchar_t model[] = L"Both";
    if (st == ERROR_SUCCESS) {
        st = RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(model), sizeof(model));
    }
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    const wchar_t name[] = L"fyewebcam virtual camera";
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, ClsidKey().c_str(), nullptr, REG_SZ, name, sizeof(name));
    return S_OK;
}

STDAPI DllUnregisterServer() {
    LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKey().c_str());
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(st);
}
