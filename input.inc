// ---- Background running, input focus filter, hold-Esc fast-forward ----
// The game pauses on WM_ACTIVATEAPP(0); SubProc swallows that (see g_keeprun) so it keeps
// running when another window has focus. DirectInput is already NONEXCLUSIVE|BACKGROUND,
// so while the game is not the foreground window its keyboard/mouse reads come back empty
// (typing in another window never reaches the game).
// Esc: a short tap goes to the game as usual (pause menu). Holding Esc longer than
// FF_HOLD_MS runs the game clock FF_SCALE times faster while held: in-engine cinematics
// (player can only look around) fly by. Clock = QueryPerformanceCounter, GetTickCount,
// timeGetTime, all hot-patched.
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#pragma comment(lib, "dxguid.lib")
static const int FF_HOLD_MS = 350;
static const double FF_SCALE = 8.0;
static volatile LONG g_esc_hidden = 0;  // 1 while Esc is held and hidden from the game
static volatile LONG g_esc_tap = 0;     // reads left that report a short Esc tap to the game
static volatile LONG g_esc_tap_ev = 0;  // buffered tap events (press + release) still to inject

static bool game_focused() { return g_wnd && GetForegroundWindow() == g_wnd; }

// ---- scaled clocks ----
static CRITICAL_SECTION g_clk; static int g_clk_init = 0;
struct Clk { double scale; long long real0, fake0; };
static Clk g_qpc = {1, 0, 0}, g_tick = {1, 0, 0}, g_tgt = {1, 0, 0};
static long long clk_map(Clk& c, long long real) {
    double s = g_ff ? FF_SCALE : 1.0;
    EnterCriticalSection(&g_clk);
    if (c.real0 == 0) { c.real0 = real; c.fake0 = real; c.scale = s; }
    if (s != c.scale) { c.fake0 = c.fake0 + (long long)((real - c.real0) * c.scale); c.real0 = real; c.scale = s; }
    long long f = c.fake0 + (long long)((real - c.real0) * c.scale);
    LeaveCriticalSection(&g_clk);
    return f;
}
typedef BOOL(WINAPI* QPC_t)(LARGE_INTEGER*);
typedef DWORD(WINAPI* TICK_t)();
static QPC_t o_QPC = nullptr; static TICK_t o_Tick = nullptr, o_TGT = nullptr;
static BOOL WINAPI h_QPC(LARGE_INTEGER* p) {
    BOOL r = o_QPC(p);
    if (r && p && g_clk_init) p->QuadPart = clk_map(g_qpc, p->QuadPart);
    return r;
}
static DWORD WINAPI h_Tick() { DWORD t = o_Tick(); return g_clk_init ? (DWORD)clk_map(g_tick, t) : t; }
static DWORD WINAPI h_TGT() { DWORD t = o_TGT(); return g_clk_init ? (DWORD)clk_map(g_tgt, t) : t; }
static void clock_hook() {
    static int done = 0; if (done) return; done = 1;
    InitializeCriticalSection(&g_clk); g_clk_init = 1;
    int a = hotpatch("QueryPerformanceCounter", (void*)h_QPC, (void**)&o_QPC, "kernel32.dll");
    int b = hotpatch("GetTickCount", (void*)h_Tick, (void**)&o_Tick, "kernel32.dll");
    if (!GetModuleHandleA("winmm.dll")) LoadLibraryA("winmm.dll");
    int c = hotpatch("timeGetTime", (void*)h_TGT, (void**)&o_TGT, "winmm.dll");
    logf("clock hooks: qpc %d tick %d tgt %d", a, b, c);
}

// ---- DirectInput device hooks (shared vtable: GetDeviceState 9, GetDeviceData 10) ----
typedef HRESULT(STDMETHODCALLTYPE* GDS_t)(void*, DWORD, void*);
typedef HRESULT(STDMETHODCALLTYPE* GDD_t)(void*, DWORD, DIDEVICEOBJECTDATA*, DWORD*, DWORD);
static GDS_t o_GDS_A = nullptr, o_GDS_W = nullptr; static GDD_t o_GDD_A = nullptr, o_GDD_W = nullptr;
static HRESULT gds(GDS_t o, void* d, DWORD n, void* p) {
    HRESULT r = o(d, n, p);
    if (FAILED(r) || !p) return r;
    if (!game_focused()) { memset(p, 0, n); return r; }
    if (n == 256) {  // keyboard
        BYTE* k = (BYTE*)p;
        if (g_esc_hidden) k[DIK_ESCAPE] = 0;
        if (g_esc_tap > 0) { k[DIK_ESCAPE] = 0x80; InterlockedDecrement(&g_esc_tap); }
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE h_GDS_A(void* d, DWORD n, void* p) { return gds(o_GDS_A, d, n, p); }
static HRESULT STDMETHODCALLTYPE h_GDS_W(void* d, DWORD n, void* p) { return gds(o_GDS_W, d, n, p); }
static void* g_kbds[8]; static void* g_notkbds[8]; static int g_nk = 0, g_nnk = 0;
static bool is_kbd(void* d) {  // cached GetCapabilities (vtable slot 3) device type check
    for (int i = 0; i < g_nk; ++i) if (g_kbds[i] == d) return true;
    for (int i = 0; i < g_nnk; ++i) if (g_notkbds[i] == d) return false;
    DIDEVCAPS c = {}; c.dwSize = sizeof c;
    typedef HRESULT(STDMETHODCALLTYPE* CAPS_t)(void*, DIDEVCAPS*);
    bool k = SUCCEEDED(((CAPS_t)(*(void***)d)[3])(d, &c)) && GET_DIDEVICE_TYPE(c.dwDevType) == DI8DEVTYPE_KEYBOARD;
    if (k) { if (g_nk < 8) g_kbds[g_nk++] = d; } else if (g_nnk < 8) g_notkbds[g_nnk++] = d;
    return k;
}
static HRESULT gdd(GDD_t o, void* d, DWORD cb, DIDEVICEOBJECTDATA* buf, DWORD* inout, DWORD fl) {
    DWORD cap = inout ? *inout : 0;
    HRESULT r = o(d, cb, buf, inout, fl);
    if (FAILED(r) || !inout) return r;
    if (!game_focused()) { *inout = 0; return r; }
    if (!buf || (fl & DIGDD_PEEK) || cb < sizeof(DIDEVICEOBJECTDATA_DX3) || !is_kbd(d)) return r;
    DWORD w = 0, seq = 0;
    for (DWORD i = 0; i < *inout; ++i) {  // drop real Esc events; taps are injected below
        DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((BYTE*)buf + i * cb);
        seq = e->dwSequence;
        if (e->dwOfs == DIK_ESCAPE) continue;
        if (w != i) memmove((BYTE*)buf + w * cb, e, cb);
        ++w;
    }
    if (g_esc_tap_ev && w + 2 <= cap) {
        InterlockedExchange(&g_esc_tap_ev, 0);
        for (int k = 0; k < 2; ++k) {
            DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((BYTE*)buf + (w + k) * cb);
            memset(e, 0, cb); e->dwOfs = DIK_ESCAPE; e->dwData = k ? 0 : 0x80;
            e->dwTimeStamp = GetTickCount(); e->dwSequence = seq + 1 + k;
        }
        w += 2;
    }
    *inout = w;
    return r;
}
static HRESULT STDMETHODCALLTYPE h_GDD_A(void* d, DWORD cb, DIDEVICEOBJECTDATA* b, DWORD* io, DWORD f) { return gdd(o_GDD_A, d, cb, b, io, f); }
static HRESULT STDMETHODCALLTYPE h_GDD_W(void* d, DWORD cb, DIDEVICEOBJECTDATA* b, DWORD* io, DWORD f) { return gdd(o_GDD_W, d, cb, b, io, f); }

// Patch the shared device vtables (A and W) through throwaway keyboard devices.
static void di_hook() {
    static int done = 0; if (done) return;
    HMODULE m = GetModuleHandleA("dinput8.dll"); if (!m) return;
    typedef HRESULT(WINAPI* DI8C_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    DI8C_t create = (DI8C_t)GetProcAddress(m, "DirectInput8Create"); if (!create) return;
    done = 1;
    for (int w = 0; w < 2; ++w) {
        IUnknown* di = nullptr;
        if (FAILED(create(GetModuleHandleA(nullptr), 0x0800, w ? IID_IDirectInput8W : IID_IDirectInput8A, (void**)&di, nullptr)) || !di) continue;
        typedef HRESULT(STDMETHODCALLTYPE* CD_t)(void*, REFGUID, void**, LPUNKNOWN);
        void* dev = nullptr;
        if (SUCCEEDED(((CD_t)(*(void***)di)[3])(di, GUID_SysKeyboard, &dev, nullptr)) && dev) {
            void** vt = *(void***)dev;
            vt_set(vt, 9, w ? (void*)h_GDS_W : (void*)h_GDS_A, w ? (void**)&o_GDS_W : (void**)&o_GDS_A);
            vt_set(vt, 10, w ? (void*)h_GDD_W : (void*)h_GDD_A, w ? (void**)&o_GDD_W : (void**)&o_GDD_A);
            logf("dinput8 %s device vtable %p hooked", w ? "W" : "A", vt);
            ((IUnknown*)dev)->Release();
        }
        di->Release();
    }
}

// Esc watcher: decides tap (pass to game) vs hold (fast-forward).
// The mixer runs on real time, so fast-forwarded speech would drift: mute the level mix meanwhile.
// Only the newest mix buffer: older ones belong to unloaded levels and may be freed.
static void ff_mute(bool on) { if (o_SV && g_nmix) o_SV(g_mixbuf[(g_nmix - 1) % 8], on ? -10000 : 0); }
// Blocking cinematic flag (Snowblind.SP, found by disassembly): 0x49bbc0 starts a cinematic and
// sets byte +0x1d of the object at *0x1023308; 0x49bc60 ends it and clears it.
static int cine_active() {
    __try {
        BYTE* o = *(BYTE**)0x1023308;
        return o ? o[0x1d] != 0 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static DWORD WINAPI esc_thread(LPVOID) {
    bool was = false; DWORD t0 = 0; int cine_was = -1;
    for (;;) {
        Sleep(10);
        di_hook();
        int cine = cine_active();
        if (cine != cine_was) { logf("cinematic %d", cine); cine_was = cine; }
        bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        // Outside cinematics Esc is left alone (normal pause menu, no fast-forward).
        if (down && !was) { if (game_focused() && cine) { t0 = GetTickCount(); InterlockedExchange(&g_esc_hidden, 1); } }
        if (g_ff && !cine) { InterlockedExchange(&g_ff, 0); ff_mute(false); logf("fast-forward off (cinematic over)"); }
        if (down && g_esc_hidden && !g_ff && GetTickCount() - t0 > (DWORD)FF_HOLD_MS) { InterlockedExchange(&g_ff, 1); ff_mute(true); logf("fast-forward on"); }
        if (!down && was && g_esc_hidden) {
            if (g_ff) { InterlockedExchange(&g_ff, 0); ff_mute(false); logf("fast-forward off"); }
            else { InterlockedExchange(&g_esc_tap, 3); InterlockedExchange(&g_esc_tap_ev, 1); }
            InterlockedExchange(&g_esc_hidden, 0);
        }
        was = down;
    }
}
static void input_start() {
    static int started = 0; if (started) return; started = 1;
    clock_hook();
    CreateThread(nullptr, 0, esc_thread, nullptr, 0, nullptr);
}
