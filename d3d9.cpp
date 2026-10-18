#include <string.h>
// snowblind-borderless: d3d9.dll proxy that turns Project: Snowblind's fullscreen
// into a borderless window covering the monitor. Drop next to Snowblind.exe.
// Only Direct3DCreate9 is wrapped (CreateDevice + a vtable hook on Reset);
// every other export is forwarded to the system d3d9.dll.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static HMODULE g_real = nullptr;
static HWND g_wnd = nullptr;
static int g_log = 0;

static void logf(const char* f, ...) {
    if (!g_log) return;
    char b[512]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a);
    FILE* fp = fopen("snowblind-borderless.log", "a");
    if (fp) { fputs(b, fp); fputc('\n', fp); fclose(fp); }
}

static HMODULE real() {
    if (!g_real) {
        g_log = GetFileAttributesA("snowblind-borderless.log") != INVALID_FILE_ATTRIBUTES;
        // Chain-load the community Widescreen Patch (its d3d9.dll renamed to
        // d3d9_widescreen.dll next to the game; it loads the system d3d9 itself).
        g_real = LoadLibraryA("d3d9_widescreen.dll");
        logf("d3d9_widescreen.dll -> %p", g_real);
        if (!g_real) {
            char p[MAX_PATH]; GetSystemDirectoryA(p, MAX_PATH); strcat_s(p, "\\d3d9.dll");
            g_real = LoadLibraryA(p);
        }
    }
    return g_real;
}

// ---- window subclass: log focus traffic, keep the game from re-raising itself while inactive ----
// ---- IAT hooks on the game's user32 imports: the game pulls focus back whenever it
// loses it (Alt-Tab bounces straight back), so focus-grabbing calls are ignored while
// the app is inactive. ----
static int g_appactive = 1;
static unsigned g_iatlog = 0;
static void modname(void* addr, char* out, size_t n) {
    HMODULE m = nullptr; MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof mbi) && mbi.AllocationBase) m = (HMODULE)mbi.AllocationBase;
    char path[MAX_PATH] = "?"; if (m) GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, 92); base = base ? base + 1 : path;
    _snprintf(out, n, "%s+%x", base, (unsigned)((BYTE*)addr - (BYTE*)m));
}
static void trace(const char* why) {
    void* fr[12]; USHORT n = CaptureStackBackTrace(1, 12, fr, nullptr);
    char line[512] = ""; size_t len = 0;
    for (USHORT i = 0; i < n && len < 440; ++i) { char m[64]; modname(fr[i], m, sizeof m); len += _snprintf(line + len, sizeof line - len, " %s", m); }
    logf("%s stack:%s", why, line);
}
static unsigned g_calllog = 0;
static int blocked(const char* fn) {
    if (g_appactive) { if (g_calllog < 30) { ++g_calllog; trace(fn); } return 0; }
    if (g_iatlog < 40) { ++g_iatlog; logf("%s while inactive -> ignored", fn); trace(fn); }
    return 1;
}
static BOOL (WINAPI* o_SetForegroundWindow)(HWND) = nullptr;
static HWND (WINAPI* o_SetActiveWindow)(HWND) = nullptr;
static HWND (WINAPI* o_SetFocus)(HWND) = nullptr;
static BOOL (WINAPI* o_BringWindowToTop)(HWND) = nullptr;
static BOOL (WINAPI* o_ShowWindow)(HWND, int) = nullptr;
static BOOL (WINAPI* o_SetWindowPos)(HWND, HWND, int, int, int, int, UINT) = nullptr;
static BOOL WINAPI h_SetForegroundWindow(HWND h) { return blocked("SetForegroundWindow") ? TRUE : o_SetForegroundWindow(h); }
static HWND WINAPI h_SetActiveWindow(HWND h) { return blocked("SetActiveWindow") ? h : o_SetActiveWindow(h); }
static HWND WINAPI h_SetFocus(HWND h) { return blocked("SetFocus") ? h : o_SetFocus(h); }
static BOOL WINAPI h_BringWindowToTop(HWND h) { return blocked("BringWindowToTop") ? TRUE : o_BringWindowToTop(h); }
static BOOL WINAPI h_ShowWindow(HWND h, int c) { return blocked("ShowWindow") ? TRUE : o_ShowWindow(h, c); }
static BOOL WINAPI h_SetWindowPos(HWND h, HWND a, int x, int y, int w, int hh, UINT f) {
    if (blocked("SetWindowPos")) f |= SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (a == HWND_TOPMOST) { logf("SetWindowPos HWND_TOPMOST -> HWND_NOTOPMOST"); a = HWND_NOTOPMOST; }
    return o_SetWindowPos(h, a, x, y, w, hh, f);
}
struct IatEntry { const char* name; void* hook; void** orig; };
// SetWindowsHookEx: log every hook the game installs; refuse keyboard hooks (WH_KEYBOARD=2, WH_KEYBOARD_LL=13)
// that would eat Alt+Tab. Returns a fake handle so the game thinks it succeeded.
static HHOOK (WINAPI* o_SetWindowsHookExA)(int, HOOKPROC, HINSTANCE, DWORD) = nullptr;
static HHOOK (WINAPI* o_SetWindowsHookExW)(int, HOOKPROC, HINSTANCE, DWORD) = nullptr;
static HHOOK hookex(const char* fn, HHOOK (WINAPI* o)(int, HOOKPROC, HINSTANCE, DWORD), int id, HOOKPROC pr, HINSTANCE m, DWORD tid) {
    char mn[64]; modname((void*)pr, mn, sizeof mn);
    logf("%s id=%d proc=%s mod=%p thread=%u", fn, id, mn, m, tid); trace(fn);
    if (id == WH_KEYBOARD || id == WH_KEYBOARD_LL) { logf("  -> keyboard hook refused (fake handle)"); return (HHOOK)0x51; }
    return o(id, pr, m, tid);
}
static HHOOK WINAPI h_SetWindowsHookExA(int id, HOOKPROC pr, HINSTANCE m, DWORD tid) { return hookex("SetWindowsHookExA", o_SetWindowsHookExA, id, pr, m, tid); }
static HHOOK WINAPI h_SetWindowsHookExW(int id, HOOKPROC pr, HINSTANCE m, DWORD tid) { return hookex("SetWindowsHookExW", o_SetWindowsHookExW, id, pr, m, tid); }
static IatEntry g_iat[] = {
    { "SetWindowsHookExA", (void*)h_SetWindowsHookExA, (void**)&o_SetWindowsHookExA },
    { "SetWindowsHookExW", (void*)h_SetWindowsHookExW, (void**)&o_SetWindowsHookExW },
    { "SetForegroundWindow", (void*)h_SetForegroundWindow, (void**)&o_SetForegroundWindow },
    { "SetActiveWindow", (void*)h_SetActiveWindow, (void**)&o_SetActiveWindow },
    { "SetFocus", (void*)h_SetFocus, (void**)&o_SetFocus },
    { "BringWindowToTop", (void*)h_BringWindowToTop, (void**)&o_BringWindowToTop },
    { "ShowWindow", (void*)h_ShowWindow, (void**)&o_ShowWindow },
    { "SetWindowPos", (void*)h_SetWindowPos, (void**)&o_SetWindowPos },
};
// Hot-patch the user32 exports (mov edi,edi prologue + 5 padding bytes): catches every
// caller in the process, including the DRM-unpacked game image that has no normal IAT.
static int hotpatch(const char* name, void* hook, void** orig, const char* mod = "user32.dll") {
    BYTE* f = (BYTE*)GetProcAddress(GetModuleHandleA(mod), name);
    if (!f) { logf("%s: not found", name); return 0; }
    for (int hops = 0; hops < 4 && f[0] == 0xFF && f[1] == 0x25; ++hops) f = **(BYTE***)(f + 2);  // follow jmp [imm32] forwarders
    if (f[0] == 0xB8 ||  // win32u syscall stub: mov eax,imm32 (5 bytes, relocatable) -> trampoline hook
        (f[0] == 0x55 && f[1] == 0x8B && f[2] == 0xEC && f[3] == 0x6A && f[4] == 0xFF)) {  // push ebp; mov ebp,esp; push -1 (DSOAL)
        static BYTE* pool = nullptr; static int used = 0;
        if (!pool) pool = (BYTE*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!pool) return 0;
        BYTE* tr = pool + used; used += 16;
        memcpy(tr, f, 5); tr[5] = 0xE9; *(DWORD*)(tr + 6) = (DWORD)((f + 5) - (tr + 10));
        DWORD old2;
        if (!VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &old2)) return 0;
        f[0] = 0xE9; *(DWORD*)(f + 1) = (DWORD)((BYTE*)hook - (f + 5));
        VirtualProtect(f, 5, old2, &old2);
        FlushInstructionCache(GetCurrentProcess(), f, 5);
        *orig = tr;
        logf("%s: syscall stub hooked at %p", name, f);
        return 1;
    }
    if (f[0] != 0x8B || f[1] != 0xFF) { logf("%s: prologue %02x %02x, skipped", name, f[0], f[1]); return 0; }
    for (int i = -5; i < 0; ++i) if (f[i] != 0xCC && f[i] != 0x90) { logf("%s: no padding, skipped", name); return 0; }
    DWORD old;
    if (!VirtualProtect(f - 5, 7, PAGE_EXECUTE_READWRITE, &old)) return 0;
    f[-5] = 0xE9; *(DWORD*)(f - 4) = (DWORD)((BYTE*)hook - f);
    f[0] = 0xEB; f[1] = 0xF9;
    VirtualProtect(f - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f - 5, 7);
    *orig = f + 2;
    return 1;
}

// LEVEL SOUND FIX. The built-in mixer streams level audio (music + effects) through one
// 11776-byte DirectSound buffer (flags CTRLVOLUME|CTRLPAN|CTRLFREQUENCY|GETCURRENTPOSITION2|
// LOCSOFTWARE...), fills it with real samples, but only ever calls SetVolume(-10000) on it,
// so on modern Windows levels are silent while menu/Bink streams play. The mix already carries
// the game's own volume settings, so that buffer is kept at 0 dB.
// Path: LoadLibraryA("dsound") -> DirectSoundCreate(8) -> IDirectSound::CreateSoundBuffer
// (vtable slot 3) -> IDirectSoundBuffer::SetVolume (slot 15).
typedef HRESULT(STDMETHODCALLTYPE* SV_t)(void*, LONG);
static SV_t o_SV = nullptr;
static void* g_mixbuf[8]; static int g_nmix = 0;
static bool is_mix(void* b) { for (int i = 0; i < g_nmix; ++i) if (g_mixbuf[i] == b) return true; return false; }
static HRESULT STDMETHODCALLTYPE h_SV(void* b, LONG v) {
    bool mix = is_mix(b);
    static int n = 0; if (mix && v != 0 && n++ < 10) logf("mix buffer %p SetVolume %ld -> 0", b, v);
    return o_SV(b, mix ? 0 : v);
}
static void vt_set(void** vt, int i, void* h, void** o) {
    DWORD old; if (!VirtualProtect(&vt[i], 4, PAGE_EXECUTE_READWRITE, &old)) return;
    *o = vt[i]; vt[i] = h; VirtualProtect(&vt[i], 4, old, &old);
}
typedef HRESULT(STDMETHODCALLTYPE* CSB_t)(void*, const void*, void**, void*);
static CSB_t o_CSB = nullptr;
static HRESULT STDMETHODCALLTYPE h_CSB(void* ds, const void* desc, void** pb, void* u) {
    HRESULT r = o_CSB(ds, desc, pb, u);
    if (r != 0 || !pb || !*pb || !desc) return r;
    DWORD fl = ((const DWORD*)desc)[1], sz = ((const DWORD*)desc)[2];
    if (fl & 1) return r;  // primary buffer
    if (!o_SV) { void** vt = *(void***)*pb; vt_set(vt, 15, (void*)h_SV, (void**)&o_SV); logf("dsound buffer vtable %p hooked", vt); }
    if (sz == 11776 && (fl & 0x40e0) == 0x40e0) {
        g_mixbuf[g_nmix % 8] = *pb; if (g_nmix < 8) ++g_nmix; else g_mixbuf[0] = *pb;
        logf("mix buffer %p (flags %08x)", *pb, (unsigned)fl);
        if (o_SV) o_SV(*pb, 0);
    }
    return r;
}
static void hook_ds(void* ds) {
    if (!ds || o_CSB) return;
    void** vt = *(void***)ds;
    vt_set(vt, 3, (void*)h_CSB, (void**)&o_CSB);
    logf("dsound vtable %p: CreateSoundBuffer hooked", vt);
}
typedef HRESULT(WINAPI* DSC_t)(const GUID*, void**, void*);
static DSC_t o_DSC = nullptr, o_DSC8 = nullptr;
static HRESULT WINAPI h_DSC(const GUID* g, void** pp, void* u) {
    HRESULT r = o_DSC(g, pp, u);
    if (r == 0 && pp) hook_ds(*pp); return r;
}
static HRESULT WINAPI h_DSC8(const GUID* g, void** pp, void* u) {
    HRESULT r = o_DSC8(g, pp, u);
    if (r == 0 && pp) hook_ds(*pp); return r;
}
static void ds_hook() {
    static int done = 0; if (done) return;
    if (!GetModuleHandleA("dsound.dll")) return;
    int n = hotpatch("DirectSoundCreate", (void*)h_DSC, (void**)&o_DSC, "dsound.dll") +
            hotpatch("DirectSoundCreate8", (void*)h_DSC8, (void**)&o_DSC8, "dsound.dll");
    logf("dsound hooks: %d", n);
    done = 1;
    // The game may already own its device: patch the shared vtable through a throwaway object.
    if (!o_CSB && o_DSC8) { void* ds = nullptr; if (h_DSC8(nullptr, &ds, nullptr) == 0 && ds) ((IUnknown*)ds)->Release(); }
}

typedef HMODULE(WINAPI* LLA_t)(LPCSTR);
static LLA_t o_LLA = nullptr;
static HMODULE WINAPI h_LLA(LPCSTR n) {
    HMODULE m = o_LLA(n);
    if (n && m && strstr(n, "sound") || (n && m && strstr(n, "SOUND"))) { logf("LoadLibraryA(%s)", n); ds_hook(); }
    return m;
}
static void ll_hook() {
    int n = hotpatch("LoadLibraryA", (void*)h_LLA, (void**)&o_LLA, "kernelbase.dll");
    logf("LoadLibraryA hook: %d", n);
}
static void iat_hook() {
    static int done = 0; if (done) return; done = 1;
    int n = 0;
    for (IatEntry& e : g_iat) n += hotpatch(e.name, e.hook, e.orig);
    logf("hotpatched %d user32 functions", n);
}

static WNDPROC g_oldproc = nullptr;
static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // The game crashes (c0000005 at Snowblind.exe+0xa36f65) when it is told it lost focus,
    // so in borderless mode it never hears about activation changes at all: it keeps
    // running in the background and never pauses or minimizes.
    switch (m) {
    case WM_ACTIVATEAPP: {
        g_appactive = (int)w; HWND fg = GetForegroundWindow(); char cls[64] = "?", ttl[64] = "";
        if (fg) { GetClassNameA(fg, cls, sizeof cls); GetWindowTextA(fg, ttl, sizeof ttl); }
        logf("WM_ACTIVATEAPP %d other-thread=%u fg=%p '%s' \"%s\" our style=%08x ex=%08x (hidden)", (int)w, (unsigned)l, fg, cls, ttl,
             (unsigned)GetWindowLongA(h, GWL_STYLE), (unsigned)GetWindowLongA(h, GWL_EXSTYLE));
        if (w) trace("activateapp");
        break; }
    case WM_ACTIVATE: logf("WM_ACTIVATE %d min=%d", (int)LOWORD(w), (int)HIWORD(w)); break;
    case WM_SETFOCUS: logf("WM_SETFOCUS"); break;
    case WM_KILLFOCUS: logf("WM_KILLFOCUS"); break;
    case WM_TIMER: if (w == 0x5b) { ds_hook(); if (GetWindowLongA(h, GWL_EXSTYLE) & WS_EX_TOPMOST) { logf("timer: topmost set by game -> cleared"); o_SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); } return 0; } break;
    case WM_SYSKEYDOWN: if (w == VK_TAB) logf("WM_SYSKEYDOWN alt+tab"); break;
    case WM_SYSCOMMAND: logf("WM_SYSCOMMAND %04x", (unsigned)(w & 0xfff0)); break;
    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS* wp = (WINDOWPOS*)l;
        if (!g_appactive && GetForegroundWindow() != h) {
            logf("WINDOWPOSCHANGING while inactive flags=%04x after=%p -> blocked", wp->flags, wp->hwndInsertAfter);
            wp->flags |= SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        }
        break; }
    }
    return CallWindowProcA(g_oldproc, h, m, w, l);
}
static void subclass(HWND h) {
    if (g_oldproc || !h) return;
    g_oldproc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)SubProc);
    SetTimer(h, 0x5b, 500, nullptr);
    logf("subclassed %p (old proc %p)", h, g_oldproc);
}

static void borderless(HWND h, UINT* w, UINT* hh) {
    if (!h) return;
    HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi }; GetMonitorInfoA(m, &mi);
    RECT r = mi.rcMonitor;
    SetWindowLongA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    SetWindowLongA(h, GWL_EXSTYLE, 0);
    SetWindowPos(h, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    logf("window %p -> %ld,%ld %ldx%ld (backbuffer %ux%u)", h, r.left, r.top,
         r.right - r.left, r.bottom - r.top, w ? *w : 0, hh ? *hh : 0);
    subclass(h);
    iat_hook();
}

static void fix_pp(D3DPRESENT_PARAMETERS* pp, HWND focus) {
    if (!pp) return;
    HWND h = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    g_wnd = h;
    logf("pp windowed=%d %ux%u hz=%u fmt=%d interval=%08x", pp->Windowed, pp->BackBufferWidth,
         pp->BackBufferHeight, pp->FullScreen_RefreshRateInHz, pp->BackBufferFormat,
         pp->PresentationInterval);
    if (!pp->BackBufferWidth || !pp->BackBufferHeight) {
        RECT r; GetClientRect(h, &r);
        pp->BackBufferWidth = r.right; pp->BackBufferHeight = r.bottom;
    }
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    if (pp->BackBufferFormat == D3DFMT_R5G6B5 || pp->BackBufferFormat == D3DFMT_X1R5G5B5)
        pp->BackBufferFormat = D3DFMT_X8R8G8B8;  // windowed must match the desktop
    borderless(h, &pp->BackBufferWidth, &pp->BackBufferHeight);
}

// ---- IDirect3DDevice9::Reset vtable hook (slot 16) ----
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static Reset_t g_reset = nullptr;
static HRESULT STDMETHODCALLTYPE Reset_hook(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    logf("Reset");
    fix_pp(pp, g_wnd);
    return g_reset(d, pp);
}
// ---- IDirect3DDevice9::Present (slot 17): frame counter for the log ----
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static Present_t g_present = nullptr;
static unsigned g_frames = 0;
static HRESULT STDMETHODCALLTYPE Present_hook(IDirect3DDevice9* d, const RECT* s, const RECT* t, HWND w, const RGNDATA* r) {
    if ((++g_frames % 200) == 1) logf("present #%u active=%d", g_frames, GetForegroundWindow() == g_wnd);
    return g_present(d, s, t, w, r);
}
// EndScene (slot 42) and IDirect3DSwapChain9::Present (slot 3): extra counters, log only
typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t g_endscene = nullptr; static unsigned g_ends = 0;
static HRESULT STDMETHODCALLTYPE EndScene_hook(IDirect3DDevice9* d) {
    if ((++g_ends % 200) == 1) logf("endscene #%u", g_ends);
    return g_endscene(d);
}
typedef HRESULT(STDMETHODCALLTYPE* SCPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
static SCPresent_t g_scpresent = nullptr; static unsigned g_scframes = 0;
static HRESULT STDMETHODCALLTYPE SCPresent_hook(IDirect3DSwapChain9* c, const RECT* s, const RECT* t, HWND w, const RGNDATA* r, DWORD f) {
    if ((++g_scframes % 200) == 1) logf("swapchain present #%u", g_scframes);
    return g_scpresent(c, s, t, w, r, f);
}
static void hook_slot(void** vt, int i, void* fn, void** saved) {
    DWORD old;
    VirtualProtect(&vt[i], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *saved = vt[i]; vt[i] = fn;
    VirtualProtect(&vt[i], sizeof(void*), old, &old);
}
static void hook_reset(IDirect3DDevice9* d) {
    if (g_reset || !d) return;
    void** vt = *(void***)d;
    hook_slot(vt, 16, (void*)Reset_hook, (void**)&g_reset);
    // Only Reset is hooked: the Present/EndScene/swapchain hooks collided with Steam's
    // gameoverlayrenderer.dll (crash at Snowblind.exe+0xa36f65 on device re-creation).
}

// ---- IDirect3D9 wrapper: only CreateDevice changes, the rest forwards ----
struct D3D9W : IDirect3D9 {
    IDirect3D9* o;
    D3D9W(IDirect3D9* p) : o(p) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override { return o->QueryInterface(r, p); }
    ULONG STDMETHODCALLTYPE AddRef() override { return o->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = o->Release(); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* p) override { return o->RegisterSoftwareDevice(p); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return o->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* i) override { return o->GetAdapterIdentifier(a, f, i); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT f) override { return o->GetAdapterModeCount(a, f); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* d) override { return o->EnumAdapterModes(a, f, m, d); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* d) override { return o->GetAdapterDisplayMode(a, d); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT bf, BOOL w) override { return o->CheckDeviceType(a, t, af, bf, w); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE rt, D3DFORMAT cf) override { return o->CheckDeviceFormat(a, t, af, u, rt, cf); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m, DWORD* q) override { return o->CheckDeviceMultiSampleType(a, t, f, w, m, q); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) override { return o->CheckDepthStencilMatch(a, t, af, rf, df); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d) override { return o->CheckDeviceFormatConversion(a, t, s, d); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9* c) override { return o->GetDeviceCaps(a, t, c); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override { return o->GetAdapterMonitor(a); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT a, D3DDEVTYPE t, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) override {
        logf("CreateDevice focus=%p flags=%08lx", focus, flags);
        fix_pp(pp, focus);
        HRESULT hr = o->CreateDevice(a, t, focus, flags, pp, out);
        logf("CreateDevice -> %08lx", hr);
        if (SUCCEEDED(hr)) hook_reset(*out);
        return hr;
    }
};

extern "C" {
IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    typedef IDirect3D9* (WINAPI* fn)(UINT);
    fn f = (fn)GetProcAddress(real(), "Direct3DCreate9");
    IDirect3D9* d = f ? f(sdk) : nullptr;
    logf("Direct3DCreate9(%u) -> %p", sdk, d);
    ds_hook();
    return d ? new D3D9W(d) : nullptr;
}
HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    typedef HRESULT(WINAPI* fn)(UINT, IDirect3D9Ex**);
    fn f = (fn)GetProcAddress(real(), "Direct3DCreate9Ex");
    return f ? f(sdk, out) : E_NOTIMPL;
}
#define FWD(name, ret, args, call) \
    ret WINAPI name args { typedef ret(WINAPI* fn) args; \
        fn f = (fn)GetProcAddress(real(), #name); return f ? f call : (ret)0; }
FWD(D3DPERF_BeginEvent, int, (DWORD c, LPCWSTR n), (c, n))
FWD(D3DPERF_EndEvent, int, (void), ())
FWD(D3DPERF_GetStatus, DWORD, (void), ())
FWD(D3DPERF_QueryRepeatFrame, BOOL, (void), ())
FWD(D3DPERF_SetMarker, void, (DWORD c, LPCWSTR n), (c, n))
FWD(D3DPERF_SetOptions, void, (DWORD o), (o))
FWD(D3DPERF_SetRegion, void, (DWORD c, LPCWSTR n), (c, n))
FWD(DebugSetMute, void, (void), ())
FWD(DebugSetLevel, void, (void), ())
FWD(Direct3DShaderValidatorCreate9, void*, (void), ())
FWD(PSGPError, void, (void* a, int b, unsigned long c), (a, b, c))
FWD(PSGPSampleTexture, void, (void* a, int b, unsigned long c, unsigned long d, void* e), (a, b, c, d, e))
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); ds_hook(); ll_hook(); }
    else if (r == DLL_PROCESS_DETACH && g_real) FreeLibrary(g_real);
    return TRUE;
}
