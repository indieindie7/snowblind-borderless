// ---- Playtest marks (same format as github.com/indieindie7/playtest-marks) ----
// F9 in game: screenshot of the game window to marks\NNN.png, then a small note box.
// Enter saves one JSON line to marks\\marks.jsonl, Esc cancels. While the box is open
// marks\\typing.lock exists so a deploy script waits. Off unless a "marks" folder exists.
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
static int g_mark_n = 0;
static HWND g_mark_box = nullptr, g_mark_edit = nullptr;
static WNDPROC g_mark_editproc = nullptr;
static int g_mark_done = 0;  // 1 = save, 2 = cancel

static int mark_next() {
    int n = 0; FILE* fp = fopen("marks\\marks.jsonl", "r");
    if (fp) { char l[4096]; while (fgets(l, sizeof l, fp)) { const char* p = strstr(l, "\"n\":"); if (p) { int v = atoi(p + 4); if (v > n) n = v; } } fclose(fp); }
    return n + 1;
}
static bool png_encoder(CLSID* id) {
    UINT n = 0, sz = 0; Gdiplus::GetImageEncodersSize(&n, &sz); if (!sz) return false;
    Gdiplus::ImageCodecInfo* ci = (Gdiplus::ImageCodecInfo*)malloc(sz); Gdiplus::GetImageEncoders(n, sz, ci);
    bool ok = false; for (UINT i = 0; i < n; ++i) if (!wcscmp(ci[i].MimeType, L"image/png")) { *id = ci[i].Clsid; ok = true; }
    free(ci); return ok;
}
static bool mark_shot(HWND h, const char* path) {
    RECT r; if (!GetWindowRect(h, &r)) return false;
    int w = r.right - r.left, hh = r.bottom - r.top; if (w <= 0 || hh <= 0) return false;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr); HBITMAP bm = CreateCompatibleBitmap(scr, w, hh);
    HGDIOBJ old = SelectObject(mem, bm); BitBlt(mem, 0, 0, w, hh, scr, r.left, r.top, SRCCOPY); SelectObject(mem, old);
    bool ok = false; CLSID id;
    if (png_encoder(&id)) { Gdiplus::Bitmap b(bm, nullptr); wchar_t wp[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH); ok = b.Save(wp, &id) == Gdiplus::Ok; }
    DeleteObject(bm); DeleteDC(mem); ReleaseDC(nullptr, scr); return ok;
}
static void json_str(FILE* fp, const wchar_t* s) {  // UTF-8 JSON string
    fputc('"', fp);
    for (; *s; ++s) {
        wchar_t c = *s;
        if (c == '"' || c == '\\') { fputc('\\', fp); fputc((char)c, fp); }
        else if (c < 0x20) fprintf(fp, "\\u%04x", (unsigned)c);
        else { char u[8]; int k = WideCharToMultiByte(CP_UTF8, 0, &c, 1, u, sizeof u, nullptr, nullptr); fwrite(u, 1, k, fp); }
    }
    fputc('"', fp);
}
static LRESULT CALLBACK MarkEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) { g_mark_done = 1; return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { g_mark_done = 2; return 0; }
    if (m == WM_CHAR && (w == '\r' || w == 27)) return 0;
    return CallWindowProcW(g_mark_editproc, h, m, w, l);
}
static LRESULT CALLBACK MarkBoxProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { g_mark_done = 2; return 0; }
    if (m == WM_CTLCOLOREDIT) { SetTextColor((HDC)w, RGB(240, 240, 240)); SetBkColor((HDC)w, RGB(30, 30, 34)); static HBRUSH br = CreateSolidBrush(RGB(30, 30, 34)); return (LRESULT)br; }
    return DefWindowProcW(h, m, w, l);
}
static void mark_take(HWND game) {
    int n = mark_next(); char shot[64]; sprintf(shot, "marks\\%03d.png", n);
    bool shot_ok = mark_shot(game, shot);
    FILE* lk = fopen("marks\\typing.lock", "w"); if (lk) fclose(lk);
    RECT gr; GetWindowRect(game, &gr);
    int bw = 720, bh = 150, x = gr.left + (gr.right - gr.left - bw) / 2, y = gr.top + (gr.bottom - gr.top) / 3;
    wchar_t title[64]; swprintf(title, 64, L"Mark %d  (Enter = save, Esc = cancel)", n);
    g_mark_done = 0;
    g_mark_box = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"SBMarkBox", title, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                 x, y, bw, bh, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    RECT cr; GetClientRect(g_mark_box, &cr);
    g_mark_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                  6, 6, cr.right - 12, cr.bottom - 12, g_mark_box, nullptr, nullptr, nullptr);
    HFONT f = CreateFontW(-20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(g_mark_edit, WM_SETFONT, (WPARAM)f, TRUE);
    g_mark_editproc = (WNDPROC)SetWindowLongPtrW(g_mark_edit, GWLP_WNDPROC, (LONG_PTR)MarkEditProc);
    ClipCursor(nullptr);
    if (o_SetForegroundWindow) o_SetForegroundWindow(g_mark_box); else SetForegroundWindow(g_mark_box);
    SetFocus(g_mark_edit);
    MSG msg;
    while (!g_mark_done && GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    int len = GetWindowTextLengthW(g_mark_edit);
    wchar_t* note = (wchar_t*)calloc(len + 1, sizeof(wchar_t)); GetWindowTextW(g_mark_edit, note, len + 1);
    DestroyWindow(g_mark_box); DeleteObject(f); g_mark_box = g_mark_edit = nullptr;
    if (g_mark_done == 1 && len > 0) {
        FILE* fp = fopen("marks\\marks.jsonl", "ab");
        if (fp) {
            SYSTEMTIME t; GetLocalTime(&t);
            fprintf(fp, "{\"n\": %d, \"note\": ", n); json_str(fp, note);
            fprintf(fp, ", \"game\": \"snowblind\", \"time\": \"%04d-%02d-%02d %02d:%02d:%02d\"", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            if (shot_ok) fprintf(fp, ", \"shot\": \"marks/%03d.png\"", n);
            fprintf(fp, "}\n"); fclose(fp);
        }
        logf("mark %d saved", n);
    } else if (shot_ok) DeleteFileA(shot);  // cancelled: drop the unused screenshot
    free(note);
    DeleteFileA("marks\\typing.lock");
    if (o_SetForegroundWindow) o_SetForegroundWindow(game); else SetForegroundWindow(game);
}
static DWORD WINAPI mark_thread(LPVOID p) {
    HWND game = (HWND)p;
    ULONG_PTR tok; Gdiplus::GdiplusStartupInput in; Gdiplus::GdiplusStartup(&tok, &in, nullptr);
    WNDCLASSW wc = {}; wc.lpfnWndProc = MarkBoxProc; wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = L"SBMarkBox"; wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 34)); wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
    RegisterClassW(&wc);
    DeleteFileA("marks\\typing.lock");  // leftover from a crash
    bool was = false;
    for (;;) {
        Sleep(30);
        bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !was && GetForegroundWindow() == game) mark_take(game);
        was = down;
        if (!IsWindow(game)) break;
    }
    return 0;
}
static void mark_start(HWND game) {
    static int started = 0; if (started || !game) return;
    DWORD a = GetFileAttributesA("marks");
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;
    started = 1; CreateThread(nullptr, 0, mark_thread, game, 0, nullptr);
    logf("marks on (F9)");
}
