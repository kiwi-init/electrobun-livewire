// Option A: the main process in a hidden WebView2 (Edge's V8, shipped with
// Windows 11). Native calls go through a host object (synchronous, across
// processes); messages go through postMessage.
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <cstdio>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static LARGE_INTEGER freq, start;
static double now_ms() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart - start.QuadPart) * 1000.0 / double(freq.QuadPart);
}
static std::wstring widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
static void print_line(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    std::fputs(s.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

class Native : public IDispatch {
    LONG refs = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch) { *out = static_cast<IDispatch *>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return ULONG(r); }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *n) override { *n = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *names, UINT count, LCID, DISPID *ids) override {
        for (UINT i = 0; i < count; i++) {
            if (_wcsicmp(names[i], L"Echo") == 0) ids[i] = 1;
            else { ids[i] = DISPID_UNKNOWN; return DISP_E_UNKNOWNNAME; }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS *params, VARIANT *result, EXCEPINFO *, UINT *) override {
        if (id != 1) return DISP_E_MEMBERNOTFOUND;
        if (!params || params->cArgs < 1) return DISP_E_BADPARAMCOUNT;
        if (result) { VariantInit(result); VariantCopy(result, &params->rgvarg[0]); }
        return S_OK;
    }
};

static ComPtr<ICoreWebView2Controller> controller;
static ComPtr<ICoreWebView2> webview;

int main(int argc, char **argv) {
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    if (argc < 2) { std::fprintf(stderr, "usage: wv2-host main.js\n"); return 2; }
    FILE *f = std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    std::string js;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) js.append(buf, n);
    std::fclose(f);
    std::wstring html = widen("<!doctype html><meta charset=utf-8><script>" + js + "</script>");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LPWSTR version = nullptr;
    if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) && version) {
        std::wstring line = L"VERSION WebView2 " + std::wstring(version);
        print_line(line.c_str());
        CoTaskMemFree(version);
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"wv2-engine-spike";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 200, 200, nullptr, nullptr, wc.hInstance, nullptr);

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring data = std::wstring(temp) + L"wv2-engine-spike";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([hwnd, html](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
            if (FAILED(result)) { std::fprintf(stderr, "environment failed: 0x%08lx\n", result); PostQuitMessage(1); return result; }
            return env->CreateCoreWebView2Controller(hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([html](HRESULT result, ICoreWebView2Controller *c) -> HRESULT {
                    if (FAILED(result)) { std::fprintf(stderr, "controller failed: 0x%08lx\n", result); PostQuitMessage(1); return result; }
                    controller = c;
                    controller->get_CoreWebView2(&webview);
                    VARIANT object;
                    VariantInit(&object);
                    object.vt = VT_DISPATCH;
                    object.pdispVal = new Native();
                    webview->AddHostObjectToScript(L"native", &object);
                    VariantClear(&object);
                    EventRegistrationToken token;
                    webview->add_WebMessageReceived(
                        Callback<ICoreWebView2WebMessageReceivedEventHandler>([](ICoreWebView2 *w, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                            LPWSTR message = nullptr;
                            if (FAILED(args->TryGetWebMessageAsString(&message)) || !message) return S_OK;
                            if (wcscmp(message, L"ping") == 0) {
                                w->PostWebMessageAsString(L"pong");
                            } else {
                                print_line(message);
                                if (wcsncmp(message, L"READY", 5) == 0) SetTimer(nullptr, 0, 6000, [](HWND, UINT, UINT_PTR, DWORD) { PostQuitMessage(0); });
                            }
                            CoTaskMemFree(message);
                            return S_OK;
                        }).Get(),
                        &token);
                    return webview->NavigateToString(html.c_str());
                }).Get());
        }).Get());
    if (FAILED(hr)) { std::fprintf(stderr, "CreateCoreWebView2EnvironmentWithOptions: 0x%08lx\n", hr); return 1; }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    controller = nullptr;
    webview = nullptr;
    return int(msg.wParam);
}
