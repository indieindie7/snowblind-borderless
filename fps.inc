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
static void map_poll();
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
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (last) logf("reticle: patch failed"); return last; }
    return true;
}
static void fps_start() {
    static int started = 0; if (started) return; started = 1;
    CreateThread(nullptr, 0, fps_thread, nullptr, 0, nullptr);
}
// ---- mission-1 HUD off (user, 10-18: radar, health/ammo counters and subtitles come only from mission 2 on, lore-wise) ----
// Same trick as the reticle: each HUD component class builds its art in Init = vtable[1]; while the current map
// is listed in "snowblind-nohud.txt" (one map name per line; default: the first map of the mission list) the slots
// point at the ret-4 stub, so components created on that map's load stay empty. Classes from static RE (Sonnet, hud_re.md).
struct HudSlot { DWORD slot, init; const char* name; };
static const HudSlot g_hudslots[] = {
    {0xfce584, 0x53bc10, "hudmeterbar"},  {0xfcd3cc, 0x52f930, "hudcirclemeter"}, {0xfcd8fc, 0x5319d0, "hudcurvedmeter"},
    {0xfceadc, 0x53fd20, "hudprimaryweapongroup"}, {0xfcebc4, 0x5407a0, "hudsecondaryweapongroup"}, {0xfcf384, 0x547210, "hudweaponicon"},
    {0xfcdc5c, 0x533dd0, "hudinteger"}, {0xfcf05c, 0x5450b0, "hudtextpopup"}, {0xfce84c, 0x53d270, "radar?"}};
static void hud_set(bool off) {
    int n = 0;
    for (auto& s : g_hudslots) {
        __try {
            DWORD* p = (DWORD*)s.slot; DWORD want = off ? 0x536320 : s.init;
            if (*p != s.init && *p != 0x536320) continue;  // not this build
            if (*p == want) continue;
            DWORD old; if (VirtualProtect(p, 4, PAGE_READWRITE, &old)) { *p = want; VirtualProtect(p, 4, old, &old); ++n; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (n) logf("mission HUD %s (%d classes)", off ? "off" : "on", n);
}
// Current map (static RE): MissionState = *(*0x1023358) + 0x18; +0x24 map index, +0x2c WORD string id of the
// map name; +0 map list (count at +0xc, WORD ids at +0x14). Ids resolve through the name table (name_of).
static bool id_str(DWORD id, char* out, int cap) {
    out[0] = 0; if (!id) return false;
    DWORD t = rd(rd(0x10332b4) + 0x4010), p = t ? rd(t + id * 8) : 0;
    return p > 0x10000 && str_at((const char*)p, out, cap);
}
static void map_poll() {
    DWORD gs = rd(rd(0x1023358)); if (!gs) return;
    DWORD ms = gs + 0x18, idx = rd(ms + 0x24); char nm[64];
    if (!id_str(rdw(ms + 0x2c), nm, sizeof nm)) return;
    if (!strcmp(nm, g_map)) return;
    strcpy_s(g_map, nm);
    DWORD list = rd(ms); char first[64] = "";
    if (list && !id_str(rdw(list + 0x14), first, sizeof first)) id_str(rdw(rd(list + 0x14)), first, sizeof first);  // ids inline, or behind a pointer
    bool off = false; FILE* f = nullptr;
    if (!fopen_s(&f, "snowblind-nohud.txt", "r") && f) {
        char l[96]; while (fgets(l, sizeof l, f)) { l[strcspn(l, "\r\n")] = 0; if (l[0] && !_stricmp(l, nm)) off = true; }
        fclose(f);
    }  // no default yet: the field read as "english" on mission 1 (10-18), so it is not the map name
    logf("map \"%s\" (index %lu, first \"%s\") -> HUD %s", nm, idx, first, off ? "off" : "on");
    hud_set(off);
}
