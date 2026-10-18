// dinput8.dll proxy for Project Snowblind: forces DISCL_NONEXCLUSIVE | DISCL_BACKGROUND on every
// DirectInput device so Alt-Tab / other windows work in borderless mode. Logs to snowblind-dinput.log
// (only if that file exists in the game dir).
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <stdarg.h>

static void logf(const char* f, ...) {
    FILE* fp = fopen("snowblind-dinput.log", "r"); if (!fp) return; fclose(fp);
    fp = fopen("snowblind-dinput.log", "a"); if (!fp) return;
    va_list a; va_start(a, f); vfprintf(fp, f, a); va_end(a); fputc('\n', fp); fclose(fp);
}
typedef HRESULT (WINAPI* PFN_DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static HMODULE g_real = nullptr;
static PFN_DI8Create g_create = nullptr;
static void load_real() {
    if (g_real) return;
    char p[MAX_PATH]; GetSystemDirectoryA(p, MAX_PATH); strcat(p, "/dinput8.dll");
    g_real = LoadLibraryA(p);
    if (g_real) g_create = (PFN_DI8Create)GetProcAddress(g_real, "DirectInput8Create");
    logf("real dinput8 %p create %p", g_real, g_create);
}
static void hook_slot(void** vt, int i, void* fn, void** orig) {
    if (*orig) return;
    DWORD old; VirtualProtect(&vt[i], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = vt[i]; vt[i] = fn; VirtualProtect(&vt[i], sizeof(void*), old, &old);
}
// IDirectInputDevice8::SetCooperativeLevel = slot 13
typedef HRESULT (WINAPI* PFN_SetCoop)(void*, HWND, DWORD);
static PFN_SetCoop o_SetCoopA = nullptr, o_SetCoopW = nullptr;
static HRESULT WINAPI h_SetCoop(PFN_SetCoop o, void* self, HWND h, DWORD f) {
    DWORD nf = (f & ~(DISCL_EXCLUSIVE | DISCL_FOREGROUND)) | DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    HRESULT hr = o(self, h, nf);
    logf("SetCooperativeLevel dev=%p hwnd=%p flags=%02x -> %02x hr=%08x", self, h, f, nf, hr);
    return hr;
}
static HRESULT WINAPI h_SetCoopA(void* s, HWND h, DWORD f) { return h_SetCoop(o_SetCoopA, s, h, f); }
static HRESULT WINAPI h_SetCoopW(void* s, HWND h, DWORD f) { return h_SetCoop(o_SetCoopW, s, h, f); }
// IDirectInput8::CreateDevice = slot 3
typedef HRESULT (WINAPI* PFN_CreateDevice)(void*, REFGUID, void**, LPUNKNOWN);
static PFN_CreateDevice o_CreateDevA = nullptr, o_CreateDevW = nullptr;
static HRESULT WINAPI h_CreateDev(PFN_CreateDevice o, void* self, REFGUID g, void** out, LPUNKNOWN u, PFN_SetCoop* oc, void* hc) {
    HRESULT hr = o(self, g, out, u);
    logf("CreateDevice guid=%08x -> %08x dev=%p", g.Data1, hr, out ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out) hook_slot(*(void***)*out, 13, hc, (void**)oc);
    return hr;
}
static HRESULT WINAPI h_CreateDevA(void* s, REFGUID g, void** o, LPUNKNOWN u) { return h_CreateDev(o_CreateDevA, s, g, o, u, &o_SetCoopA, (void*)h_SetCoopA); }
static HRESULT WINAPI h_CreateDevW(void* s, REFGUID g, void** o, LPUNKNOWN u) { return h_CreateDev(o_CreateDevW, s, g, o, u, &o_SetCoopW, (void*)h_SetCoopW); }

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, LPUNKNOWN outer) {
    load_real();
    if (!g_create) return E_FAIL;
    HRESULT hr = g_create(inst, ver, iid, out, outer);
    int w = (iid == IID_IDirectInput8W);
    logf("DirectInput8Create ver=%04x %s -> %08x", ver, w ? "W" : "A", hr);
    if (SUCCEEDED(hr) && out && *out) hook_slot(*(void***)*out, 3, w ? (void*)h_CreateDevW : (void*)h_CreateDevA, w ? (void**)&o_CreateDevW : (void**)&o_CreateDevA);
    return hr;
}
extern "C" HRESULT WINAPI DllCanUnloadNow() { load_real(); auto f = (HRESULT(WINAPI*)())GetProcAddress(g_real, "DllCanUnloadNow"); return f ? f() : S_FALSE; }
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID i, LPVOID* o) { load_real(); auto f = (HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*))GetProcAddress(g_real, "DllGetClassObject"); return f ? f(c, i, o) : E_FAIL; }
extern "C" HRESULT WINAPI DllRegisterServer() { return S_OK; }
extern "C" HRESULT WINAPI DllUnregisterServer() { return S_OK; }
extern "C" LPCDIDATAFORMAT WINAPI GetdfDIJoystick() { load_real(); auto f = (LPCDIDATAFORMAT(WINAPI*)())GetProcAddress(g_real, "GetdfDIJoystick"); return f ? f() : nullptr; }
BOOL WINAPI DllMain(HINSTANCE, DWORD r, LPVOID) { if (r == DLL_PROCESS_ATTACH) logf("dinput8 proxy loaded"); return TRUE; }
