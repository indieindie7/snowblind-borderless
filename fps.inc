// ---- FPS graph (port of AdventMod's ModFpsGraph) and HUD tweaks ----
// The last 240 frame times as bars in the top-left corner, with the current, average and worst
// frame rate. Green line = 60 fps (16.7 ms), yellow = 30; the graph's top is 50 ms. F10 toggles it.
// No Present/EndScene hook (Steam overlay safety): a frame is one keyboard GetDeviceState call
// (input.inc), timed on the real clock, and the graph is a click-through layered window on top.
static const int FPS_N = 240, FPS_W = 360, FPS_H = 90;
static float g_ft[FPS_N]; static int g_fh = 0, g_fc = 0; static long long g_flast = 0;
static volatile LONG g_fps_on = 1;
static long long real_100ns() { FILETIME f; GetSystemTimePreciseAsFileTime(&f); return ((long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static void fps_tick() {
    long long t = real_100ns();
    if (g_flast) {
        float d = (float)(t - g_flast) / 1e7f;
        if (d < 0.002f) return;  // a second poll in the same frame
        g_ft[g_fh] = d; g_fh = (g_fh + 1) % FPS_N; if (g_fc < FPS_N) ++g_fc;
    }
    g_flast = t;
}
static void fps_draw(Graphics& g) {
    float x0 = 4, y0 = 4, bw = (float)FPS_W / FPS_N, sum = 0, worst = 0;
    SolidBrush panel(Color(150, 0, 0, 0)); g.FillRectangle(&panel, 0.0f, 0.0f, (REAL)FPS_W + 8, (REAL)FPS_H + 26);
    SolidBrush gr(Color(220, 60, 220, 80)), ye(Color(220, 240, 210, 40)), re(Color(220, 240, 60, 50));
    for (int i = 0; i < g_fc; ++i) {
        float t = g_ft[(g_fh - g_fc + i + FPS_N) % FPS_N]; sum += t; if (t > worst) worst = t;
        float h = min(t / 0.05f, 1.0f) * FPS_H;
        g.FillRectangle(t <= 1.0f / 58 ? &gr : t <= 1.0f / 29 ? &ye : &re, x0 + (FPS_N - g_fc + i) * bw, y0 + FPS_H - h, bw, h);
    }
    Pen l60(Color(200, 60, 220, 80), 1), l30(Color(200, 240, 210, 40), 1);
    float y60 = y0 + FPS_H - (1.0f / 60 / 0.05f) * FPS_H, y30 = y0 + FPS_H - (1.0f / 30 / 0.05f) * FPS_H;
    g.DrawLine(&l60, x0, y60, x0 + FPS_W, y60); g.DrawLine(&l30, x0, y30, x0 + FPS_W, y30);
    static wchar_t line[128] = L""; static long long shown = 0; long long now = real_100ns();
    if (now - shown > 2500000 && g_fc) {  // numbers update 4x a second so they can be read
        shown = now; float cur = g_ft[(g_fh - 1 + FPS_N) % FPS_N], avg = sum / g_fc;
        swprintf(line, 128, L"%3.0f fps   avg %3.0f   worst %3.0f   (%.1f ms)   F10 hides", 1 / cur, 1 / avg, 1 / worst, cur * 1000);
    }
    FontFamily ff(L"Consolas"); Font f(&ff, 13, FontStyleRegular, UnitPixel); SolidBrush tx(Color(255, 235, 235, 235));
    g.DrawString(line, -1, &f, PointF(x0, y0 + FPS_H + 4), &tx);
}
static bool hud_tweaks(bool last);
// Windows keeps a per-app mute in the volume mixer; Snowblind.SP had one stored (meter 10-18: signal 0.48, mute 1,
// the "no sound in levels"). Our own process's audio session is un-muted while it runs; "keep-mute" opts out.
static void unmute_session() {
    static IAudioSessionManager* sm = nullptr; static int bad = 0;
    if (bad || GetFileAttributesA("keep-mute") != INVALID_FILE_ATTRIBUTES) return;
    if (!sm) {
        IMMDeviceEnumerator* e = nullptr; IMMDevice* d = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e))) { bad = 1; return; }
        if (SUCCEEDED(e->GetDefaultAudioEndpoint(eRender, eConsole, &d))) { d->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, (void**)&sm); d->Release(); }
        e->Release(); if (!sm) return;
    }
    ISimpleAudioVolume* v = nullptr; BOOL m = FALSE;
    if (SUCCEEDED(sm->GetSimpleAudioVolume(nullptr, FALSE, &v)) && v) {
        if (SUCCEEDED(v->GetMute(&m)) && m) { v->SetMute(FALSE, nullptr); logf("audio session was muted in the Windows mixer -> un-muted"); }
        v->Release();
    }
}
static void map_poll(); static void hud_probe(); static int g_probe_in = -1;
static DWORD WINAPI fps_thread(LPVOID) {
    const int W = FPS_W + 8, H = FPS_H + 26;
    WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = L"SBFps"; RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SBFps", L"", WS_POPUP,
                             0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) return 0;
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    HBITMAP dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0); ReleaseDC(nullptr, sdc);
    if (!dib) return 0;
    SelectObject(mdc, dib);
    bool shown = false, f10 = false; int hud_try = 0, tick = 0;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        if (hud_try >= 0) { ++hud_try; if (hud_tweaks(hud_try >= 1200) || hud_try >= 1200) hud_try = -1; }  // up to 2 minutes (the menu exe is another build: no match there)
        map_poll();
        if (g_probe_in > 0 && --g_probe_in == 0) hud_probe();
        if (++tick % 10 == 0) unmute_session();
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        Sleep(100);
        bool fg = g_wnd && GetForegroundWindow() == g_wnd;
        bool k = fg && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (k && !f10) InterlockedExchange(&g_fps_on, !g_fps_on);
        f10 = k;
        bool live = g_flast && real_100ns() - g_flast < 5000000;  // the game drew a frame in the last half second
        bool want = g_fps_on && fg && live && !g_freeze && g_fc > 0;
        if (!want) { if (shown) { ShowWindow(w, SW_HIDE); shown = false; } continue; }
        memset(bits, 0, W * H * 4);
        { Bitmap b(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits); Graphics g(&b); g.SetTextRenderingHint(TextRenderingHintAntiAlias); fps_draw(g); }
        RECT r; GetWindowRect(g_wnd, &r);
        POINT dst = {r.left + 8, r.top + 8}, src = {0, 0}; SIZE sz = {W, H};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(w, nullptr, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
        if (!shown) { SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); shown = true; }
    }
}
// Aiming reticle off (static RE, Snowblind.SP): the HUD class hudplayerweaponreticle builds its art in
// Init = vtable[1] (vtable 0xfcea18, slot 0xfcea1c = 0x53f780). Pointing the slot at a "ret 4" stub
// (0x536320) leaves the reticle unbuilt. A file "show-reticle" in the game folder keeps it.
// The exe is still packed when the window appears, so this is retried from the graph thread until the code is there.
static void radar_off(); static void ammo_move();
static bool hud_tweaks(bool last) {
    if (GetFileAttributesA("show-reticle") != INVALID_FILE_ATTRIBUTES) { logf("reticle kept (show-reticle)"); return true; }
    __try {
        DWORD* slot = (DWORD*)0xfcea1c; const BYTE* stub = (const BYTE*)0x536320;
        if (*slot != 0x53f780 || stub[0] != 0xc2 || stub[1] != 0x04 || stub[2] != 0x00) { if (last) logf("reticle: slot %08lx stub %02x %02x %02x, left alone", *slot, stub[0], stub[1], stub[2]); return false; }
        DWORD old; if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return true;
        *slot = 0x536320; VirtualProtect(slot, 4, old, &old);
        // 0x53f600 (called from the per-frame update, vtable[25]) uses the art at this+0x34 that Init would have built:
        // crash c0000005 at 0x53f623 (user, 10-18). It returns "ret 4" too, so it becomes one.
        BYTE* fn = (BYTE*)0x53f600;
        if (fn[0] == 0x56 && fn[1] == 0x8b && fn[2] == 0xf1 && VirtualProtect(fn, 3, PAGE_EXECUTE_READWRITE, &old)) {
            fn[0] = 0xc2; fn[1] = 0x04; fn[2] = 0x00; VirtualProtect(fn, 3, old, &old); FlushInstructionCache(GetCurrentProcess(), fn, 3);
        } else logf("reticle: update guard not applied (%02x %02x %02x)", fn[0], fn[1], fn[2]);
        logf("reticle off");
        radar_off();
        ammo_move();
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (last) logf("reticle: patch failed"); return last; }
    return true;
}
static void fps_start() {
    static int started = 0; if (started) return; started = 1;
    CreateThread(nullptr, 0, fps_thread, nullptr, 0, nullptr);
}
// ---- mission-1 HUD off (user, 10-18: radar, health/ammo counters and subtitles come only from mission 2 on, lore-wise) ----
// Same trick as the reticle: each HUD component class builds its art in Init = vtable[1]; while the HUD is off the
// slots point at the ret-4 stub, so components created on that map's load stay empty. Methods that use the unbuilt
// art without a null test get "ret" stubs too, and the radar is never added (the game's own "hideradar" branch).
// All from static RE (Sonnet, %TEMP%\sbdump\hud_re.md Round 2). Everything is swapped back when the HUD is on.
struct HudSlot { DWORD slot, init; const char* name; };
static const HudSlot g_hudslots[] = {
    {0xfce584, 0x53bc10, "hudmeterbar"}, {0xfceadc, 0x53fd20, "hudprimaryweapongroup"}, {0xfcebc4, 0x5407a0, "hudsecondaryweapongroup"},
    {0xfcf384, 0x547210, "hudweaponicon"}, {0xfcdc5c, 0x533dd0, "hudinteger"}};
struct HudPatch { DWORD at; BYTE exp[3]; int nexp; BYTE neu[3]; int n; BYTE save[3]; bool saved; };
#define RET4 {0xc2, 0x04, 0x00}, 3
#define RET0 {0xc3}, 1
static HudPatch g_hudpatch[] = {
    {0x53ce8d, {0x74, 0x1d}, 2, {0xeb}, 1},  // HUDPlayerBaseSegment: "if (!hideradar) AddChild(map icons)" -> always skip
    {0x53b9e0, {0x56, 0x8b, 0xf1}, 3, RET0}, {0x53ba40, {0x56, 0x8b, 0xf1}, 3, RET0}, {0x53b370, {0x8b, 0x54, 0x24}, 3, RET4},  // meter bar
    {0x53b3b0, {0x83, 0xec, 0x10}, 3, RET0}, {0x53b2b0, {0x83, 0xec, 0x40}, 3, RET4},
    {0x53fc50, {0x53, 0x56, 0x8b}, 3, RET4},  // primary weapon group update
    {0x540670, {0x6a, 0xff, 0x68}, 3, RET4}, {0x540650, {0x56, 0x8b, 0xf1}, 3, RET0},  // secondary weapon group
    {0x533a60, {0x6a, 0xff}, 2, RET4}, {0x5339d0, {0x83, 0xec, 0x10}, 3, RET4}, {0x533890, {0x57}, 1, RET4}, {0x533950, {0x83, 0xec, 0x10}, 3, RET0}};  // integer
static bool hud_poke(BYTE* at, const BYTE* b, int n) {
    DWORD old; if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, b, n); VirtualProtect(at, n, old, &old); FlushInstructionCache(GetCurrentProcess(), at, n); return true;
}
static void hud_set(bool off) {
    int n = 0, skip = 0;
    for (auto& s : g_hudslots) {
        __try {
            DWORD* p = (DWORD*)s.slot; DWORD want = off ? 0x536320 : s.init;
            if (*p != s.init && *p != 0x536320) { ++skip; continue; }  // not this build
            if (*p == want) continue;
            if (hud_poke((BYTE*)p, (const BYTE*)&want, 4)) ++n;
        } __except (EXCEPTION_EXECUTE_HANDLER) { ++skip; }
    }
    for (auto& h : g_hudpatch) {
        __try {
            BYTE* at = (BYTE*)h.at;
            if (off && !h.saved) {
                if (memcmp(at, h.exp, h.nexp)) { ++skip; continue; }
                memcpy(h.save, at, h.n); if (hud_poke(at, h.neu, h.n)) { h.saved = true; ++n; }
            } else if (!off && h.saved && !memcmp(at, h.neu, h.n)) { if (hud_poke(at, h.save, h.n)) { h.saved = false; ++n; } }
        } __except (EXCEPTION_EXECUTE_HANDLER) { ++skip; }
    }
    if (n || skip) logf("mission HUD %s (%d changes, %d skipped)", off ? "off" : "on", n, skip);
}
// Current map (static RE, hud_re.md Round 2): MS = *(*(*0x1023358) + 0x18) (accessor 0x4b69a0); MS+0 map list L
// (count at L+0xc, WORD string ids behind the pointer at L+0x14); MS+0x24 current index; MS+0x20 state (3 = loaded).
// (MS+0x2c is LoadMap's variant/language id: it read "english".) Ids resolve through the name table.
static bool id_str(DWORD id, char* out, int cap) {
    out[0] = 0; if (!id) return false;
    DWORD t = rd(rd(0x10332b4) + 0x4010), p = t ? rd(t + id * 8) : 0;
    return p > 0x10000 && str_at((const char*)p, out, cap);
}
// HUD off on the first map of the list (mission 1, lore: the radar and counters come with mission 2), or on any
// map named in "snowblind-nohud.txt" (one per line). A file "keep-hud" keeps it everywhere.
static void map_poll() {
    DWORD ms = rd(rd(rd(0x1023358)) + 0x18); if (!ms) return;
    DWORD L = rd(ms), cnt = L ? rd(L + 0xc) : 0, idx = rd(ms + 0x24), ids = L ? rd(L + 0x14) : 0;
    if (!ids || !cnt || cnt > 256 || idx >= cnt) return;
    char nm[64]; if (!id_str(rdw(ids + idx * 2), nm, sizeof nm)) return;
    if (!strcmp(nm, g_map)) return;
    strcpy_s(g_map, nm);
    char first[64] = ""; id_str(rdw(ids), first, sizeof first);
    bool off = idx == 0; FILE* f = nullptr;
    if (!fopen_s(&f, "snowblind-nohud.txt", "r") && f) {
        char l[96]; while (fgets(l, sizeof l, f)) { l[strcspn(l, "\r\n")] = 0; if (l[0] && !_stricmp(l, nm)) off = true; }
        fclose(f);
    }
    if (GetFileAttributesA("keep-hud") != INVALID_FILE_ATTRIBUTES) off = false;
    logf("map \"%s\" (index %lu of %lu, state %lu, first \"%s\") -> HUD %s", nm, idx, cnt, rd(ms + 0x20), first, off ? "off (disabled)" : "on");
    g_probe_in = 100;  // 10 s after the map is known: dump the ammo panel objects
    // hud_set(off) is off for now: loading a level crashed with it (user, 10-18); the HUD gets a redesign instead.
}
// Radar gone on every map (user, 10-18: "no more radar, radar is bad mechanics"): HUDPlayerBaseSegment init
// does "if (!hideradar) AddChild(map icons)"; the je at 0x53ce8d becomes a jmp, the game's own hideradar path.
// A file "show-radar" keeps it.
static void radar_off() {
    if (GetFileAttributesA("show-radar") != INVALID_FILE_ATTRIBUTES) { logf("radar kept (show-radar)"); return; }
    static const BYTE jmp = 0xeb;
    __try {
        BYTE* at = (BYTE*)0x53ce8d;
        if (at[0] == 0x74 && at[1] == 0x1d && hud_poke(at, &jmp, 1)) logf("radar off");
        else logf("radar: bytes %02x %02x, left alone", at[0], at[1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf("radar: patch failed"); }
}
// Probe for the HUD redesign (user, 10-18: ammo counter to the bottom-left). Positions are art markers, not config
// (hud_re.md Round 3), so the live objects are dumped: every heap object whose vtable is the primary weapon group,
// a hudinteger or a hudweaponicon, with its first 0x80 bytes as hex + float, and the group's children (+0x14 count, +0x1c data).
static void hud_dump(DWORD o, const char* what) {
    logf("probe %s @%08lx", what, o);
    for (int off = 0; off < 0x80; off += 16) {
        char line[200]; int n = sprintf_s(line, "  +%02x", off);
        for (int k = 0; k < 16; k += 4) { DWORD v = rd(o + off + k); float f; memcpy(&f, &v, 4);
            n += sprintf_s(line + n, sizeof line - n, (fabsf(f) > 1e-3f && fabsf(f) < 1e5f) ? " %08lx(%.3g)" : " %08lx", v, f); }
        logf("%s", line);
    }
}
static void hud_probe() {
    static const struct { DWORD vt; const char* n; } V[] = {{0xfcead8, "primarygroup"}, {0xfcdc58, "hudinteger"}, {0xfcf380, "weaponicon"}};
    int found = 0; MEMORY_BASIC_INFORMATION mi; BYTE* a = (BYTE*)0x10000;
    while (found < 40 && VirtualQuery(a, &mi, sizeof mi) && (DWORD)a < 0x7fff0000) {
        if (mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & PAGE_GUARD)) {
            DWORD* p = (DWORD*)mi.BaseAddress; size_t n = mi.RegionSize / 4;
            __try {
                for (size_t i = 0; i < n && found < 40; ++i) for (auto& v : V) if (p[i] == v.vt) {
                    DWORD o = (DWORD)(p + i); hud_dump(o, v.n); ++found;
                    if (v.vt == 0xfcead8) { DWORD c = rd(o + 0x14), d = rd(o + 0x1c);
                        for (DWORD j = 0; j < c && j < 8; ++j) logf("  child %lu: %08lx %08lx (vt %08lx)", j, rd(d + j * 8), rd(d + j * 8 + 4), rd(rd(d + j * 8 + 4))); }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        a = (BYTE*)mi.BaseAddress + mi.RegionSize;
    }
    logf("probe: %d HUD objects", found);
}
// Ammo counter to the bottom-left (user, 10-18). The primary weapon group (vtable 0xfcead8, Init = slot 0xfceadc =
// 0x53fd20, "ret 4") builds its children from art markers; each hudinteger child (vt 0xfcdc58) copies its marker to
// +0x54 and hands it to a text renderer r = [child+0x44], which keeps its own copy at r+0x80 (used when the number
// changes) and baked quads at [r+0x44] (count r+0x40, 24-byte verts x,y,z,colour,u,v in 0..1 screen units:
// x = (px+320)/640, y = (240-py)/480). The wrapper runs the real Init, then shifts all three. Verified live 10-18.
// A file "keep-ammo" leaves the counter in the top-right panel.
static const float AMMO_DX = -470.f, AMMO_DY = -350.f;
typedef int (__thiscall* PgInit)(void*, void*);
static PgInit g_pg_init = (PgInit)0x53fd20;
static void ammo_shift(DWORD c) {
    float* p = (float*)(c + 0x54); p[0] += AMMO_DX; p[1] += AMMO_DY;
    DWORD r = rd(c + 0x44); if (!r) return;
    float* q = (float*)(r + 0x80); q[0] += AMMO_DX; q[1] += AMMO_DY;
    DWORD n = rd(r + 0x40), vb = rd(r + 0x44);
    for (DWORD i = 0; vb && i < n && i < 256; ++i) { float* v = (float*)(vb + i * 24); v[0] += AMMO_DX / 640.f; v[1] -= AMMO_DY / 480.f; }
}
static int __fastcall pg_init_hook(void* self, void*, void* arg) {
    int ret = g_pg_init(self, arg);
    __try {
        DWORD o = (DWORD)self, c = rd(o + 0x14), d = rd(o + 0x1c), n = 0;
        for (DWORD j = 0; j < c && j < 16; ++j) { DWORD e = rd(d + j * 8 + 4); if (e && rd(e) == 0xfcdc58) { ammo_shift(e); ++n; } }
        static int logged = 0; if (logged++ < 3) logf("ammo: group %08lx, %lu counters moved", o, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf("ammo: shift failed"); }
    return ret;
}
static void ammo_move() {
    if (GetFileAttributesA("keep-ammo") != INVALID_FILE_ATTRIBUTES) { logf("ammo kept (keep-ammo)"); return; }
    __try {
        DWORD* slot = (DWORD*)0xfceadc; DWORD want = (DWORD)&pg_init_hook;
        if (*slot == 0x53fd20 && hud_poke((BYTE*)slot, (const BYTE*)&want, 4)) logf("ammo: group Init hooked");
        else logf("ammo: slot %08lx, left alone", *slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf("ammo: hook failed"); }
}
