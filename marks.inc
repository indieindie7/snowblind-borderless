// ---- Playtest marks (same format as github.com/indieindie7/playtest-marks, ported from the Goobi brawler) ----
// F9 in game: the frame is frozen (game clock stopped, level audio muted) and a mark editor opens over it,
// Lightshot style: toolbar top right (Pen, Line, Arrow, Box, Oval, Marker, Text, Persp, Undo, Redo, Clear,
// colour swatches, "..." colour picker), mouse wheel = size, Ctrl+Z / Ctrl+Y undo/redo. Type the note in the
// box at the bottom: Enter saves, Esc cancels. Saved: marks\NNN.png (clean frame), marks\NNN_sketch.png
// (with the markup, only if something was drawn), one JSON line in marks\marks.jsonl with the game state
// (level, player position, cinematic flag). A file dropped on the game or the mark editor is copied to
// marks\inbox and named in the mark ("dropped"). marks\typing.lock exists while the editor is open, so a
// deploy script waits. Off unless a "marks" folder exists in the game folder.
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <commdlg.h>
#include <math.h>
#include <vector>
#include <string>
#include <algorithm>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
using namespace Gdiplus;

enum MTool { T_PEN, T_LINE, T_ARROW, T_BOX, T_OVAL, T_MARKER, T_TEXT, T_PERSP, T_PICK, T_COUNT };
static const wchar_t* MTOOL_NAME[T_COUNT] = {L"Pen", L"Line", L"Arrow", L"Box", L"Oval", L"Marker", L"Text", L"Persp", L"Pick"};
static const COLORREF SWATCH[8] = {RGB(255, 40, 40), RGB(255, 220, 0), RGB(40, 220, 60), RGB(0, 220, 255),
                                   RGB(50, 90, 255), RGB(255, 0, 220), RGB(255, 255, 255), RGB(0, 0, 0)};
struct MStroke { int tool; COLORREF c; float size; std::vector<POINT> pts; std::wstring text; };
struct MGuide { bool on = false; float vx = 0.5f, vy = 0.42f; bool has2 = false; float v2x = 0.9f; int lines = 16; };
static struct MarkUi {
    HWND game = nullptr, box = nullptr, edit = nullptr, tedit = nullptr;
    WNDPROC editproc = nullptr, teditproc = nullptr;
    Bitmap* shot = nullptr; int w = 0, h = 0;
    std::vector<MStroke> strokes, redo;
    bool drawing = false, dragvp = false; MStroke cur;
    int tool = T_ARROW; COLORREF col = RGB(255, 40, 40); float size = 4;
    MGuide guide; POINT tpos = {};
    int done = 0;  // 1 = save, 2 = cancel
    int n = 0; std::string dropped; std::wstring status;
    HFONT font = nullptr;
} M;
static char g_mark_drop[MAX_PATH * 2] = {}; static volatile LONG g_mark_drop_pending = 0;
static HWND g_toast = nullptr; static DWORD g_toast_t = 0; static std::wstring g_toast_text;

// ---- game state (Snowblind.SP addresses; reads are guarded) ----
static int mark_cine() {
    __try { BYTE* o = *(BYTE**)0x1023308; return o ? o[0x1d] != 0 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Player position (unverified): GetPlayer() at 0x5c1650 = first entry of the list headed at *0x10338d0, pos at +0x40.
static bool mark_player(float* o) {
    __try {
        DWORD s = *(DWORD*)0x10338d0; if (!s) return false;
        DWORD n = *(DWORD*)s; if (n == s || !n) return false;
        DWORD pl = *(DWORD*)(n + 8); if (!pl) return false;
        const float* p = (const float*)(pl + 0x40);
        for (int i = 0; i < 3; ++i) { o[i] = p[i]; if (!(o[i] > -1e6f && o[i] < 1e6f)) return false; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// d3d9.cpp blocks SetFocus process-wide (the game steals focus); the mark editor uses the real one.
static void mfocus(HWND h) { if (o_SetFocus) o_SetFocus(h); else SetFocus(h); }
// Object list probe (static RE, unverified): circular std::list headed at *0x10337cc, node +0 next, +8 object,
// object pos float[3] at +0x40. Name candidates: ref-counted string handles at obj+0x3c / obj+0x80 (chars at *h + 6).
// Each mark writes marks\probe.txt with every candidate, so the real name field can be picked from live data.
static bool str_at(const char* p, char* out, int cap) {
    __try {
        int i = 0;
        for (; i < cap - 1 && p[i]; ++i) { if (p[i] < 0x20 || p[i] > 0x7e) return false; out[i] = p[i]; }
        out[i] = 0; return i >= 2 && !p[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static DWORD rd(DWORD a) { __try { return *(DWORD*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static bool rdf(DWORD a, float* o) {
    __try { for (int i = 0; i < 3; ++i) { o[i] = ((float*)a)[i]; if (!(o[i] > -1e6f && o[i] < 1e6f)) return false; } return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void probe_name(DWORD obj, int off, FILE* fp, char* best) {
    DWORD h = rd(obj + off); char s[72];
    const DWORD tries[4] = {h, h + 6, rd(h) + 6, rd(h)};
    for (int k = 0; k < 4; ++k) if (tries[k] > 0x10000 && str_at((const char*)tries[k], s, sizeof s)) {
        if (fp) fprintf(fp, " +%02x/%d=\"%s\"", off, k, s);
        if (best && !best[0]) strcpy(best, s);
    }
}
struct MObj { float p[3]; char name[72]; float d; };
static WORD rdw(DWORD a) { __try { return *(WORD*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
// Object name (static RE): obj+0xb0 is a WORD id into the interned string table
// *(*(0x10332b4)+0x4010), 8 bytes per entry, entry +0 = char*; id 0 = unnamed (the by-name lookup 0x5bdd30
// compares this word). Fallback: the ref-counted string at obj+0x3c (chars at buffer + 6).
static bool name_of(DWORD obj, char* out, int cap) {
    out[0] = 0;
    DWORD id = rdw(obj + 0xb0);
    if (id) { DWORD t = rd(rd(0x10332b4) + 0x4010), p = t ? rd(t + id * 8) : 0; if (p > 0x10000 && str_at((const char*)p, out, cap)) return true; }
    DWORD h = rd(obj + 0x3c); if (h > 0x10000 && str_at((const char*)(h + 6), out, cap)) return true;
    out[0] = 0; return false;
}
// Pick tool: every object projected with the camera of the frozen frame (static RE):
// cam = *(*(0x10379f0)+0xfb7c), view 4x4 at cam+0x88, projection at cam+0xc8, row-major, row vectors (D3D).
struct PObj { float p[3]; char name[72]; float sx, sy, w; };
static std::vector<PObj> g_pobj; static std::vector<int> g_picked; static int g_hover = -1;
static bool rdm(DWORD a, float* m) { __try { memcpy(m, (const void*)a, 64); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static void vmul(const float* v, const float* m, float* o) { for (int j = 0; j < 4; ++j) o[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j]; }
static char g_map[64] = "";  // current map name (fps.inc map_poll)
static float g_cam[6];  // first-person camera pos + angles at the last capture (written into the mark)
static void pick_capture(int W, int H) {
    g_pobj.clear(); g_picked.clear(); g_hover = -1; memset(g_cam, 0, sizeof g_cam);
    // Camera = the object "birthed firstpersoncamera_001" (live read 10-18): pos at +0x40, then three angles
    // (roll?, pitch, yaw in radians) at +0x4c. Z is up, yaw 0 looks down +Y. FOV: 90 deg across the 4:3 frame
    // (fitted on a screenshot; the 640x480 image is stretched to the window).
    DWORD head = rd(0x10337cc), n = head ? rd(head) : 0; int total = 0; bool cam = false;
    std::vector<DWORD> all;
    for (int guard = 0; head && n && n != head && guard < 5000; ++guard, n = rd(n)) {
        DWORD obj = rd(n + 8); if (!obj) continue; ++total; all.push_back(obj);
        char nm[72]; if (!cam && name_of(obj, nm, sizeof nm) && strstr(nm, "firstpersoncamera")) cam = rdf(obj + 0x40, g_cam) && rdf(obj + 0x4c, g_cam + 3);
    }
    if (!cam) { logf("pick: no camera (%d objects)", total); return; }
    float cp = cosf(g_cam[4]), sp = sinf(g_cam[4]), cy = cosf(g_cam[5]), sy = sinf(g_cam[5]);
    float f[3] = {-sy * cp, cy * cp, sp}, r[3] = {cy, sy, 0}, u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const float tx = 1.0f, ty = 0.75f;  // tan(hfov/2), tan(vfov/2)
    for (DWORD obj : all) {
        PObj o = {}; if (!rdf(obj + 0x40, o.p)) continue;
        float d[3] = {o.p[0] - g_cam[0], o.p[1] - g_cam[1], o.p[2] - g_cam[2]};
        float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2]; if (z < 1.0f) continue;  // behind the camera or at the eye
        float x = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / z / tx, y = (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / z / ty;
        if (fabsf(x) > 1.02f || fabsf(y) > 1.02f) continue;
        o.sx = (x * 0.5f + 0.5f) * W; o.sy = (0.5f - y * 0.5f) * H; o.w = z;
        name_of(obj, o.name, sizeof o.name);
        g_pobj.push_back(o);
    }
    logf("pick: %d objects, %d on screen, cam %.1f %.1f %.1f ang %.3f %.3f %.3f", total, (int)g_pobj.size(), g_cam[0], g_cam[1], g_cam[2], g_cam[3], g_cam[4], g_cam[5]);
}
static int pick_at(int x, int y) {  // nearest on-screen object within 40 px, the closer one on ties
    int best = -1; float bd = 40.0f * 40.0f;
    for (int i = 0; i < (int)g_pobj.size(); ++i) {
        float dx = g_pobj[i].sx - x, dy = g_pobj[i].sy - y, d = dx * dx + dy * dy;
        if (d < bd - 1 || (best >= 0 && fabsf(d - bd) <= 1 && g_pobj[i].w < g_pobj[best].w)) { bd = d; best = i; }
    }
    return best;
}
static int mark_objects(MObj* out, int cap, const float* me) {  // nearest objects to the player, also dumps marks\probe.txt
    FILE* fp = fopen("marks\\probe.txt", "w");
    DWORD head = rd(0x10337cc), n = head ? rd(head) : 0; int cnt = 0, total = 0;
    for (int guard = 0; head && n && n != head && guard < 5000; ++guard, n = rd(n)) {
        DWORD obj = rd(n + 8); if (!obj) continue; ++total;
        MObj o = {}; if (!rdf(obj + 0x40, o.p)) continue;
        if (fp) fprintf(fp, "%08lx vt %08lx pos %.1f %.1f %.1f", obj, rd(obj), o.p[0], o.p[1], o.p[2]);
        name_of(obj, o.name, sizeof o.name);
        if (fp) fprintf(fp, " id %u \"%s\"\n",(unsigned)rdw(obj + 0xb0), o.name);
        if (me) { float dx = o.p[0] - me[0], dy = o.p[1] - me[1], dz = o.p[2] - me[2]; o.d = sqrtf(dx * dx + dy * dy + dz * dz); }
        if (cnt < cap) out[cnt++] = o;
        else { int far_ = 0; for (int i = 1; i < cap; ++i) if (out[i].d > out[far_].d) far_ = i; if (o.d < out[far_].d) out[far_] = o; }
    }
    if (fp) { fprintf(fp, "total %d\n", total); fclose(fp); }
    for (int i = 0; i < cnt; ++i) for (int j = i + 1; j < cnt; ++j) if (out[j].d < out[i].d) { MObj t = out[i]; out[i] = out[j]; out[j] = t; }
    return cnt;
}
// ---- small helpers ----
static int mark_next() {
    int n = 0; FILE* fp = fopen("marks\\marks.jsonl", "r");
    if (fp) { char l[8192]; while (fgets(l, sizeof l, fp)) { const char* p = strstr(l, "\"n\":"); if (p) { int v = atoi(p + 4); if (v > n) n = v; } } fclose(fp); }
    return n + 1;
}
static bool png_encoder(CLSID* id) {
    UINT n = 0, sz = 0; GetImageEncodersSize(&n, &sz); if (!sz) return false;
    ImageCodecInfo* ci = (ImageCodecInfo*)malloc(sz); GetImageEncoders(n, sz, ci);
    bool ok = false; for (UINT i = 0; i < n; ++i) if (!wcscmp(ci[i].MimeType, L"image/png")) { *id = ci[i].Clsid; ok = true; }
    free(ci); return ok;
}
static bool save_png(Bitmap* b, const char* path) {
    CLSID id; if (!b || !png_encoder(&id)) return false;
    wchar_t wp[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH); return b->Save(wp, &id) == Ok;
}
static Bitmap* grab(HWND h, int* w, int* hh) {
    RECT r; if (!GetWindowRect(h, &r)) return nullptr;
    *w = r.right - r.left; *hh = r.bottom - r.top; if (*w <= 0 || *hh <= 0) return nullptr;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr); HBITMAP bm = CreateCompatibleBitmap(scr, *w, *hh);
    HGDIOBJ old = SelectObject(mem, bm); BitBlt(mem, 0, 0, *w, *hh, scr, r.left, r.top, SRCCOPY); SelectObject(mem, old);
    Bitmap* tmp = Bitmap::FromHBITMAP(bm, nullptr); Bitmap* b = tmp ? tmp->Clone(0, 0, *w, *hh, PixelFormat32bppARGB) : nullptr;
    delete tmp; DeleteObject(bm); DeleteDC(mem); ReleaseDC(nullptr, scr); return b;
}
static void json_w(FILE* fp, const wchar_t* s) {  // UTF-8 JSON string
    fputc('"', fp);
    for (; *s; ++s) {
        wchar_t c = *s;
        if (c == '"' || c == '\\') { fputc('\\', fp); fputc((char)c, fp); }
        else if (c < 0x20) fprintf(fp, "\\u%04x", (unsigned)c);
        else { char u[8]; int k = WideCharToMultiByte(CP_UTF8, 0, &c, 1, u, sizeof u, nullptr, nullptr); fwrite(u, 1, k, fp); }
    }
    fputc('"', fp);
}
static void json_a(FILE* fp, const char* s) { wchar_t w[MAX_PATH * 2]; MultiByteToWideChar(CP_ACP, 0, s, -1, w, MAX_PATH * 2); json_w(fp, w); }
static Color gc(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// ---- drawing ----
static void draw_label(Graphics& g, const PObj& o, bool strong) {
    FontFamily ff(L"Segoe UI"); Font f(&ff, 14, FontStyleBold, UnitPixel);
    Pen ring(strong ? Color(255, 255, 220, 0) : Color(255, 0, 220, 255), 2);
    g.DrawEllipse(&ring, o.sx - 7, o.sy - 7, 14.0f, 14.0f);
    wchar_t t[96]; swprintf(t, 96, L"%hs", o.name[0] ? o.name : "(unnamed)");
    SolidBrush sh(Color(200, 0, 0, 0)), br(strong ? Color(255, 255, 220, 0) : Color(255, 0, 220, 255));
    PointF p(o.sx + 10, o.sy - 9), p2(p.X + 1, p.Y + 1);
    g.DrawString(t, -1, &f, p2, &sh); g.DrawString(t, -1, &f, p, &br);
}
static void draw_picks(Graphics& g, bool live) {
    if (live && M.tool == T_PICK) {
        SolidBrush dot(Color(170, 0, 220, 255));
        for (auto& o : g_pobj) g.FillEllipse(&dot, o.sx - 3, o.sy - 3, 6.0f, 6.0f);
        if (g_hover >= 0) draw_label(g, g_pobj[g_hover], false);
    }
    for (int i : g_picked) draw_label(g, g_pobj[i], true);
}
static void draw_stroke(Graphics& g, const MStroke& s) {
    if (s.pts.empty()) return;
    if (s.tool == T_TEXT) {
        FontFamily ff(L"Segoe UI"); Font f(&ff, 10 + s.size * 3, FontStyleBold, UnitPixel);
        SolidBrush sh(Color(160, 0, 0, 0)), br(gc(s.c));
        PointF p((REAL)s.pts[0].x, (REAL)s.pts[0].y), p2(p.X + 2, p.Y + 2);
        g.DrawString(s.text.c_str(), -1, &f, p2, &sh); g.DrawString(s.text.c_str(), -1, &f, p, &br); return;
    }
    bool mk = s.tool == T_MARKER;
    Pen pen(gc(s.c, mk ? 100 : 255), mk ? s.size * 4 : s.size);
    pen.SetLineJoin(LineJoinRound); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
    POINT a = s.pts.front(), b = s.pts.back();
    switch (s.tool) {
    case T_PEN: case T_MARKER:
        if (s.pts.size() == 1) { SolidBrush br(gc(s.c, mk ? 100 : 255)); REAL r = pen.GetWidth() / 2; g.FillEllipse(&br, a.x - r, a.y - r, 2 * r, 2 * r); }
        else { std::vector<Point> p; for (auto& q : s.pts) p.push_back(Point(q.x, q.y)); g.DrawLines(&pen, p.data(), (INT)p.size()); }
        break;
    case T_LINE: g.DrawLine(&pen, a.x, a.y, b.x, b.y); break;
    case T_ARROW: {
        g.DrawLine(&pen, a.x, a.y, b.x, b.y);
        double ang = atan2((double)(b.y - a.y), (double)(b.x - a.x)), L = 10 + s.size * 3;
        for (int k = -1; k <= 1; k += 2) g.DrawLine(&pen, (REAL)b.x, (REAL)b.y, (REAL)(b.x - L * cos(ang + k * 0.45)), (REAL)(b.y - L * sin(ang + k * 0.45)));
        break; }
    case T_BOX: g.DrawRectangle(&pen, min(a.x, b.x), min(a.y, b.y), abs(b.x - a.x), abs(b.y - a.y)); break;
    case T_OVAL: g.DrawEllipse(&pen, min(a.x, b.x), min(a.y, b.y), abs(b.x - a.x), abs(b.y - a.y)); break;
    }
}
static void draw_guide(Graphics& g, int w, int h) {
    const MGuide& G = M.guide; if (!G.on) return;
    Pen faint(Color(110, 255, 255, 255), 1), hor(Color(220, 255, 220, 0), 2);
    REAL vx = G.vx * w, vy = G.vy * h;
    g.DrawRectangle(&faint, 0, 0, w - 1, h - 1);
    g.DrawLine(&hor, 0.0f, vy, (REAL)w, vy);
    auto fan = [&](REAL x) {
        for (int i = 0; i < G.lines; ++i) {
            double a = 3.14159265 * 2 * i / G.lines;
            g.DrawLine(&faint, x, vy, (REAL)(x + cos(a) * w * 2), (REAL)(vy + sin(a) * w * 2));
        }
        SolidBrush br(Color(230, 255, 220, 0)); g.FillEllipse(&br, x - 6, vy - 6, 12.0f, 12.0f);
    };
    fan(vx); if (G.has2) fan(G.v2x * w);
}
// Toolbar buttons, measured from the top right corner.
static const int BW = 58, BH = 26, GAP = 4;
enum { B_TOOL = 0, B_UNDO = T_COUNT, B_REDO, B_CLEAR, B_SW0, B_COLOR = B_SW0 + 8, B_COUNT };
static RECT btn_rect(int i) {
    int row = i < T_COUNT ? 0 : i < B_SW0 ? 1 : 2, col = i < T_COUNT ? i : i < B_SW0 ? i - T_COUNT : i - B_SW0;
    int cols = T_COUNT, x0 = M.w - GAP - cols * (BW + GAP);
    int bw = row == 2 ? (BW * cols + GAP * (cols - 1) - 8 * GAP) / 9 : BW;
    RECT r = {x0 + col * (bw + GAP), GAP + row * (BH + GAP), 0, 0}; r.right = r.left + bw; r.bottom = r.top + BH; return r;
}
static int btn_at(int x, int y) { for (int i = 0; i < B_COUNT; ++i) { RECT r = btn_rect(i); POINT p = {x, y}; if (PtInRect(&r, p)) return i; } return -1; }
static void draw_ui(Graphics& g) {
    FontFamily ff(L"Segoe UI"); Font f(&ff, 13, FontStyleRegular, UnitPixel), fb(&ff, 15, FontStyleBold, UnitPixel);
    StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush bg(Color(200, 25, 25, 30)), sel(Color(230, 70, 110, 200)), txt(Color(255, 235, 235, 235));
    Pen edge(Color(255, 255, 255, 255), 2);
    for (int i = 0; i < B_COUNT; ++i) {
        RECT r = btn_rect(i); RectF rf((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
        if (i >= B_SW0 && i < B_COLOR) {
            SolidBrush sw(gc(SWATCH[i - B_SW0])); g.FillRectangle(&sw, rf);
            if (SWATCH[i - B_SW0] == M.col) g.DrawRectangle(&edge, rf);
            continue;
        }
        bool on = (i < T_COUNT && i == M.tool) || (i == T_PERSP && M.guide.on && M.tool != T_PERSP && false);
        g.FillRectangle(on ? &sel : &bg, rf);
        const wchar_t* t = i < T_COUNT ? MTOOL_NAME[i] : i == B_UNDO ? L"Undo" : i == B_REDO ? L"Redo" : i == B_CLEAR ? L"Clear" : L"...";
        if (i == B_COLOR) { SolidBrush cb(gc(M.col)); RectF in(rf.X + 3, rf.Y + 3, rf.Width - 6, rf.Height - 6); g.FillRectangle(&cb, in); }
        else g.DrawString(t, -1, &f, rf, &sf, &txt);
    }
    // Hint line top left, note label bottom.
    wchar_t hint[512];
    swprintf(hint, 512, L"MARK %d   %s   size %.0f%s   |   wheel size, Ctrl+Z / Ctrl+Y, drop a file to attach   |   Enter sends, Esc cancels%s%s",
             M.n, MTOOL_NAME[M.tool], M.size, M.tool == T_PERSP ? L" (drag vanishing point, right click 2nd VP, wheel lines)" : L"",
             M.dropped.empty() ? L"" : L"   |   file attached", M.status.empty() ? L"" : (L"   |   " + M.status).c_str());
    RectF hr(8, 6, (REAL)max(200, btn_rect(0).left - 16), 60);
    SolidBrush sh(Color(200, 0, 0, 0)); RectF hr2 = hr; hr2.Offset(1, 1);
    g.DrawString(hint, -1, &f, hr2, nullptr, &sh); g.DrawString(hint, -1, &f, hr, nullptr, &txt);
}
static void paint(HDC dc) {
    Bitmap back(M.w, M.h, PixelFormat32bppARGB); Graphics g(&back);
    g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    if (M.shot) g.DrawImage(M.shot, 0, 0, M.w, M.h);
    draw_guide(g, M.w, M.h);
    for (auto& s : M.strokes) draw_stroke(g, s);
    if (M.drawing) draw_stroke(g, M.cur);
    draw_picks(g, true);
    draw_ui(g);
    Graphics out(dc); out.DrawImage(&back, 0, 0, M.w, M.h);
}
static void redraw() { if (M.box) InvalidateRect(M.box, nullptr, FALSE); }

// ---- text tool ----
static void text_commit(bool keep) {
    if (!M.tedit) return;
    int len = GetWindowTextLengthW(M.tedit); std::wstring t(len, L'\0');
    if (len) GetWindowTextW(M.tedit, &t[0], len + 1);
    if (keep && len) { MStroke s; s.tool = T_TEXT; s.c = M.col; s.size = M.size; s.pts.push_back(M.tpos); s.text = t; M.strokes.push_back(s); M.redo.clear(); }
    HWND e = M.tedit; M.tedit = nullptr; DestroyWindow(e);
    mfocus(M.edit); redraw();
}
static LRESULT CALLBACK TEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) { text_commit(true); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { text_commit(false); return 0; }
    if (m == WM_CHAR && (w == '\r' || w == 27)) return 0;
    if (m == WM_KILLFOCUS && M.tedit == h) { LRESULT r = CallWindowProcW(M.teditproc, h, m, w, l); text_commit(true); return r; }
    if (m == WM_LBUTTONDOWN && GetFocus() != h) mfocus(h);
    return CallWindowProcW(M.teditproc, h, m, w, l);
}
static void text_start(POINT p) {
    text_commit(true);
    M.tpos = p;
    int fs = (int)(10 + M.size * 3);
    M.tedit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_BORDER, p.x, p.y, 400, fs + 10, M.box, nullptr, nullptr, nullptr);
    HFONT f = CreateFontW(-fs, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(M.tedit, WM_SETFONT, (WPARAM)f, TRUE);
    M.teditproc = (WNDPROC)SetWindowLongPtrW(M.tedit, GWLP_WNDPROC, (LONG_PTR)TEditProc);
    mfocus(M.tedit);
}

// ---- actions ----
static void undo() { if (!M.strokes.empty()) { M.redo.push_back(M.strokes.back()); M.strokes.pop_back(); redraw(); } }
static void redo_() { if (!M.redo.empty()) { M.strokes.push_back(M.redo.back()); M.redo.pop_back(); redraw(); } }
static void pick_colour() {
    static COLORREF custom[16]; CHOOSECOLORW cc = {}; cc.lStructSize = sizeof cc; cc.hwndOwner = M.box;
    cc.rgbResult = M.col; cc.lpCustColors = custom; cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) M.col = cc.rgbResult;
    mfocus(M.edit); redraw();
}
static void attach(const char* src) {
    CreateDirectoryA("marks\\inbox", nullptr);
    const char* name = strrchr(src, '\\'); name = name ? name + 1 : src;
    char dst[MAX_PATH * 2]; sprintf(dst, "marks\\inbox\\%03d_%s", M.n, name);
    if (CopyFileA(src, dst, FALSE)) { M.dropped = dst; for (auto& c : M.dropped) if (c == '\\') c = '/'; M.status = L"attached"; }
    else M.status = L"could not copy the file";
    redraw();
}
static LRESULT CALLBACK MarkEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    // The EDIT control focuses itself on a click through the hooked (blocked) SetFocus: refocus with the real one.
    if (m == WM_LBUTTONDOWN && GetFocus() != h) { if (M.tedit) text_commit(true); mfocus(h); }
    if (m == WM_KEYDOWN && w == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) { M.done = 1; return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { M.done = 2; return 0; }
    if (m == WM_KEYDOWN && ctrl && w == 'Z') { undo(); return 0; }
    if (m == WM_KEYDOWN && ctrl && w == 'Y') { redo_(); return 0; }
    if (m == WM_CHAR && (w == '\r' || w == 27 || w == 0x1a || w == 0x19)) return 0;
    return CallWindowProcW(M.editproc, h, m, w, l);
}
static LRESULT CALLBACK MarkBoxProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    int x = (short)LOWORD(l), y = (short)HIWORD(l);
    switch (m) {
    case WM_CLOSE: M.done = 2; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(dc); EndPaint(h, &ps); return 0; }
    case WM_CTLCOLOREDIT: { SetTextColor((HDC)w, RGB(240, 240, 240)); SetBkColor((HDC)w, RGB(30, 30, 34)); static HBRUSH br = CreateSolidBrush(RGB(30, 30, 34)); return (LRESULT)br; }
    case WM_DROPFILES: {
        char p[MAX_PATH * 2]; if (DragQueryFileA((HDROP)w, 0, p, sizeof p)) attach(p);
        DragFinish((HDROP)w); return 0; }
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1;
        if (M.tool == T_PERSP) M.guide.lines = max(4, min(64, M.guide.lines + d * 2));
        else M.size = (float)max(1, min(40, (int)M.size + d));
        redraw(); return 0; }
    case WM_LBUTTONDOWN: {
        int b = btn_at(x, y);
        if (b >= 0) {
            if (b < T_COUNT) {
                if (b == T_PERSP && M.tool == T_PERSP) M.guide.on = !M.guide.on;  // clicking Persp again hides the guide
                else { M.tool = b; if (b == T_PERSP) M.guide.on = true; }
            }
            else if (b == B_UNDO) undo(); else if (b == B_REDO) redo_();
            else if (b == B_CLEAR) { M.strokes.clear(); M.redo.clear(); M.guide.on = false; }
            else if (b < B_COLOR) M.col = SWATCH[b - B_SW0];
            else { pick_colour(); return 0; }
            text_commit(true); mfocus(M.edit); redraw(); return 0;
        }
        POINT p = {x, y};
        if (M.tool == T_PICK) {
            int i = pick_at(x, y);
            if (i >= 0) { auto it = std::find(g_picked.begin(), g_picked.end(), i); if (it == g_picked.end()) g_picked.push_back(i); else g_picked.erase(it); }
            mfocus(M.edit); redraw(); return 0;
        }
        if (M.tool == T_TEXT) { text_start(p); return 0; }
        if (M.tool == T_PERSP) { M.dragvp = true; M.guide.vx = (float)x / M.w; M.guide.vy = (float)y / M.h; SetCapture(h); redraw(); return 0; }
        M.drawing = true; M.cur = MStroke(); M.cur.tool = M.tool; M.cur.c = M.col; M.cur.size = M.size;
        M.cur.pts.push_back(p); if (M.tool != T_PEN && M.tool != T_MARKER) M.cur.pts.push_back(p);
        SetCapture(h); redraw(); return 0; }
    case WM_MOUSEMOVE:
        if (M.tool == T_PICK) { int i = pick_at(x, y); if (i != g_hover) { g_hover = i; redraw(); } }
        if (M.dragvp) { M.guide.vx = (float)x / M.w; M.guide.vy = (float)y / M.h; redraw(); }
        else if (M.drawing) { POINT p = {x, y}; if (M.cur.tool == T_PEN || M.cur.tool == T_MARKER) M.cur.pts.push_back(p); else M.cur.pts.back() = p; redraw(); }
        return 0;
    case WM_LBUTTONUP:
        if (M.dragvp) { M.dragvp = false; ReleaseCapture(); mfocus(M.edit); }
        if (M.drawing) { M.drawing = false; ReleaseCapture(); M.strokes.push_back(M.cur); M.redo.clear(); mfocus(M.edit); redraw(); }
        return 0;
    case WM_RBUTTONDOWN:
        if (M.tool == T_PERSP) { if (M.guide.has2) M.guide.has2 = false; else { M.guide.has2 = true; M.guide.v2x = (float)x / M.w; } redraw(); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- toast ("mark 12 sent") ----
static LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
        HBRUSH br = CreateSolidBrush(RGB(25, 25, 30)); FillRect(dc, &r, br); DeleteObject(br);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(240, 240, 240)); SelectObject(dc, M.font);
        DrawTextW(dc, g_toast_text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE); EndPaint(h, &ps); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void toast(const wchar_t* t) {
    if (g_toast) DestroyWindow(g_toast);
    g_toast_text = t; RECT gr; GetWindowRect(M.game, &gr);
    int w = 320, h = 40;
    g_toast = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"SBMarkToast", L"", WS_POPUP,
                              gr.left + (gr.right - gr.left - w) / 2, gr.top + 40, w, h, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    SetLayeredWindowAttributes(g_toast, 0, 220, LWA_ALPHA);
    ShowWindow(g_toast, SW_SHOWNOACTIVATE); g_toast_t = GetTickCount();
}

// ---- the mark ----
static void mix_mute(bool on) { if (o_SV && g_nmix) o_SV(g_mixbuf[(g_nmix - 1) % 8], on ? -10000 : 0); }
static void markup_json(FILE* fp) {
    static const char* hex = "0123456789abcdef"; (void)hex;
    fprintf(fp, ", \"markup\": {\"size\": [%d, %d], \"texts\": [", M.w, M.h);
    bool first = true;
    for (auto& s : M.strokes) if (s.tool == T_TEXT) { if (!first) fputc(',', fp); first = false; json_w(fp, s.text.c_str()); }
    fprintf(fp, "], \"strokes\": [");
    first = true;
    for (auto& s : M.strokes) {
        if (s.tool == T_TEXT) continue;
        POINT a = s.pts.front(), b = s.pts.back();
        if (!first) fputc(',', fp); first = false;
        // Same wording as the Goobi marks: tool, colour, from/to, here in screen pixels of "size".
        fprintf(fp, "\"%ls #%02x%02x%02x from (%ld,%ld) to (%ld,%ld)\"", MTOOL_NAME[s.tool], GetRValue(s.c), GetGValue(s.c), GetBValue(s.c), a.x, a.y, b.x, b.y);
    }
    fprintf(fp, "], \"picked\": [");
    first = true;
    for (int i : g_picked) {
        const PObj& o = g_pobj[i]; if (!first) fputc(',', fp); first = false;
        fprintf(fp, "{\"name\": "); json_a(fp, o.name);
        fprintf(fp, ", \"pos\": [%.1f, %.1f, %.1f], \"screen\": [%.0f, %.0f]}", o.p[0], o.p[1], o.p[2], o.sx, o.sy);
    }
    fprintf(fp, "], \"camera\": [%.1f, %.1f, %.1f, %.4f, %.4f, %.4f]", g_cam[0], g_cam[1], g_cam[2], g_cam[3], g_cam[4], g_cam[5]);
    fprintf(fp, ", \"map\": "); json_a(fp, g_map);
    if (M.guide.on) {
        fprintf(fp, ", \"perspective\": {\"horizon_y\": \"%.2f\", \"vp1_x\": \"%.2f\", \"vp2_x\": ", M.guide.vy, M.guide.vx);
        if (M.guide.has2) fprintf(fp, "\"%.2f\"", M.guide.v2x); else fprintf(fp, "null");
        fprintf(fp, ", \"lines\": %d}", M.guide.lines);
    }
    fprintf(fp, "}");
}
static void mark_take(const char* drop) {
    M.n = mark_next(); M.dropped.clear(); M.status.clear(); M.strokes.clear(); M.redo.clear(); M.drawing = M.dragvp = false; M.done = 0;
    InterlockedExchange(&g_freeze, 1); mix_mute(true);
    Sleep(40);  // let the frame settle on screen before the grab
    delete M.shot; M.shot = grab(M.game, &M.w, &M.h);
    if (!M.shot) { InterlockedExchange(&g_freeze, 0); mix_mute(false); return; }
    char shot[64]; sprintf(shot, "marks\\%03d.png", M.n);
    bool shot_ok = save_png(M.shot, shot);
    int cine = mark_cine();
    float pp[3]; bool has_pp = mark_player(pp);
    pick_capture(M.w, M.h);
    MObj near_[12]; int nnear = mark_objects(near_, 12, has_pp ? pp : nullptr);
    FILE* lk = fopen("marks\\typing.lock", "w"); if (lk) { fputs("writing a mark", lk); fclose(lk); }
    RECT gr; GetWindowRect(M.game, &gr);
    M.box = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES, L"SBMarkBox", L"Mark", WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                            gr.left, gr.top, M.w, M.h, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    int ew = min(M.w - 40, 900), eh = 64;
    M.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                             (M.w - ew) / 2, M.h - eh - 24, ew, eh, M.box, nullptr, nullptr, nullptr);
    SendMessageW(M.edit, WM_SETFONT, (WPARAM)M.font, TRUE);
    M.editproc = (WNDPROC)SetWindowLongPtrW(M.edit, GWLP_WNDPROC, (LONG_PTR)MarkEditProc);
    if (drop) attach(drop);
    ClipCursor(nullptr);
    if (o_SetForegroundWindow) o_SetForegroundWindow(M.box); else SetForegroundWindow(M.box);
    mfocus(M.edit);
    MSG msg;
    while (!M.done && GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    text_commit(true);
    int len = GetWindowTextLengthW(M.edit); std::wstring note(len, L'\0'); if (len) GetWindowTextW(M.edit, &note[0], len + 1);
    bool sketch = !M.strokes.empty() || M.guide.on || !g_picked.empty(), ok = false;
    char sk[64]; sprintf(sk, "marks\\%03d_sketch.png", M.n);
    if (M.done == 1 && (len > 0 || sketch || !M.dropped.empty())) {
        if (sketch) {  // second picture with the markup, without the toolbar
            Bitmap b(M.w, M.h, PixelFormat32bppARGB); Graphics g(&b); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            g.DrawImage(M.shot, 0, 0, M.w, M.h); draw_guide(g, M.w, M.h); for (auto& s : M.strokes) draw_stroke(g, s);
            draw_picks(g, false); sketch = save_png(&b, sk);
        }
        FILE* fp = fopen("marks\\marks.jsonl", "ab");
        if (fp) {
            SYSTEMTIME t; GetLocalTime(&t);
            fprintf(fp, "{\"n\": %d, \"note\": ", M.n); json_w(fp, note.c_str());
            fprintf(fp, ", \"game\": \"snowblind\", \"time\": \"%04d-%02d-%02d %02d:%02d:%02d\"", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            fprintf(fp, ", \"cinematic\": %s", cine < 0 ? "null" : cine ? "true" : "false");
            if (nnear) {
                fprintf(fp, ", \"nearby\": [");
                for (int i = 0; i < nnear; ++i) fprintf(fp, "%s{\"name\": \"%s\", \"pos\": [%.1f, %.1f, %.1f], \"dist\": %.1f}", i ? ", " : "", near_[i].name, near_[i].p[0], near_[i].p[1], near_[i].p[2], near_[i].d);
                fprintf(fp, "]");
            }
            if (has_pp) fprintf(fp, ", \"player_pos\": [%.1f, %.1f, %.1f]", pp[0], pp[1], pp[2]);
            if (shot_ok) fprintf(fp, ", \"shot\": \"marks/%03d.png\"", M.n);
            if (sketch) { fprintf(fp, ", \"sketch\": \"marks/%03d_sketch.png\"", M.n); markup_json(fp); }
            if (!M.dropped.empty()) { fprintf(fp, ", \"dropped\": "); json_a(fp, M.dropped.c_str()); }
            fprintf(fp, "}\n"); ok = fclose(fp) == 0;
        }
        logf("mark %d %s", M.n, ok ? "saved" : "FAILED");
    } else if (shot_ok) DeleteFileA(shot);  // cancelled: drop the unused screenshot
    DestroyWindow(M.box); M.box = M.edit = nullptr;
    DeleteFileA("marks\\typing.lock");
    InterlockedExchange(&g_freeze, 0); mix_mute(false);
    if (o_SetForegroundWindow) o_SetForegroundWindow(M.game); else SetForegroundWindow(M.game);
    wchar_t tt[64]; swprintf(tt, 64, M.done == 1 ? (ok ? L"mark %d sent" : L"MARK %d FAILED") : L"mark %d cancelled", M.n); toast(tt);
}
// Called from SubProc (game thread): a file dropped on the game window opens a mark for it.
static bool mark_drop(HDROP d) {
    char p[MAX_PATH * 2];
    if (!M.game || !DragQueryFileA(d, 0, p, sizeof p)) return false;
    strcpy(g_mark_drop, p); InterlockedExchange(&g_mark_drop_pending, 1); return true;
}
static DWORD WINAPI mark_thread(LPVOID p) {
    M.game = (HWND)p;
    ULONG_PTR tok; GdiplusStartupInput in; GdiplusStartup(&tok, &in, nullptr);
    M.font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    WNDCLASSW wc = {}; wc.lpfnWndProc = MarkBoxProc; wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = L"SBMarkBox"; wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    RegisterClassW(&wc);
    wc.lpfnWndProc = ToastProc; wc.lpszClassName = L"SBMarkToast"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&wc);
    DeleteFileA("marks\\typing.lock");  // leftover from a crash
    bool was = false;
    for (;;) {
        Sleep(30);
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_toast && GetTickCount() - g_toast_t > 1500) { DestroyWindow(g_toast); g_toast = nullptr; }
        bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !was && GetForegroundWindow() == M.game) mark_take(nullptr);
        else if (g_mark_drop_pending) { InterlockedExchange(&g_mark_drop_pending, 0); char d[MAX_PATH * 2]; strcpy(d, g_mark_drop); mark_take(d); }
        was = down;
        if (!IsWindow(M.game)) break;
    }
    return 0;
}
static void mark_start(HWND game) {
    static int started = 0; if (started || !game) return;
    DWORD a = GetFileAttributesA("marks");
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;
    started = 1; DragAcceptFiles(game, TRUE);
    CreateThread(nullptr, 0, mark_thread, game, 0, nullptr);
    logf("marks on (F9, drop a file on the window)");
}
