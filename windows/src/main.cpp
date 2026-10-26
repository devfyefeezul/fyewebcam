// fyewebcam Windows client: connects to the phone and shows its camera.

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <cstdio>
#include <string>

#include "protocol.h"
#include "stream_client.h"

namespace {

// Palette, matching the Android app.
const COLORREF kGround = RGB(0x0F, 0x11, 0x15);
const COLORREF kPanel = RGB(0x13, 0x16, 0x1B);
const COLORREF kVideoBg = RGB(0x1B, 0x1F, 0x27);
const COLORREF kLine = RGB(0x22, 0x26, 0x2E);
const COLORREF kText = RGB(0xE8, 0xEA, 0xED);
const COLORREF kMuted = RGB(0xA3, 0xAA, 0xB6);
const COLORREF kAccent = RGB(0x2D, 0xD4, 0xBF);

struct Preset {
    const wchar_t* label;
    int width;
    int height;
};
const Preset kPresets[] = {{L"480p", 848, 480}, {L"720p", 1280, 720}, {L"1080p", 1920, 1080}};
constexpr int kFps = 30;

enum ControlId {
    kIdWifi = 100,
    kIdUsb,
    kIdHost,
    kIdPort,
    kIdConnect,
    kIdLens,
    kIdResolution,
    kIdMirror,
    kIdRotate,
    kIdFullscreen,
};
constexpr UINT_PTR kStatsTimer = 1;
constexpr int kPanelWidth = 300;  // in DIPs

struct App {
    HWND wnd = nullptr;
    HFONT font = nullptr;
    HFONT font_bold = nullptr;
    HBRUSH panel_brush = nullptr;
    HBRUSH ground_brush = nullptr;
    int dpi = 96;

    // Panel controls.
    HWND lbl_connection, wifi, usb, lbl_host, host, lbl_port, port, connect;
    HWND lbl_camera, lbl_lens, lens, lbl_res, resolution;
    HWND lbl_status, status, stats;
    HWND mirror_btn, rotate_btn, fullscreen_btn;

    StreamClient client;
    bool connected = false;
    Frame raw;
    Frame shown;
    bool has_frame = false;
    bool mirror = false;
    int rotation = 0;  // degrees clockwise: 0, 90, 180, 270

    bool fullscreen = false;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
};

App g;

int Dp(int v) { return MulDiv(v, g.dpi, 96); }

// ---------- settings (HKCU\Software\fyewebcam) ----------

const wchar_t kRegKey[] = L"Software\\fyewebcam";

std::wstring RegString(const wchar_t* name, const std::wstring& fallback) {
    wchar_t buf[256];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
        return buf;
    }
    return fallback;
}

DWORD RegDword(const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value;
    }
    return fallback;
}

void SaveSettings() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    wchar_t host[256];
    GetWindowTextW(g.host, host, 256);
    RegSetValueExW(key, L"Host", 0, REG_SZ, reinterpret_cast<const BYTE*>(host),
                   static_cast<DWORD>((wcslen(host) + 1) * sizeof(wchar_t)));
    DWORD values[3] = {
        static_cast<DWORD>(GetDlgItemInt(g.wnd, kIdPort, nullptr, FALSE)),
        static_cast<DWORD>(Button_GetCheck(g.usb) == BST_CHECKED),
        static_cast<DWORD>(ComboBox_GetCurSel(g.resolution)),
    };
    const wchar_t* names[3] = {L"Port", L"Usb", L"Resolution"};
    for (int i = 0; i < 3; ++i) {
        RegSetValueExW(key, names[i], 0, REG_DWORD, reinterpret_cast<const BYTE*>(&values[i]), sizeof(DWORD));
    }
    RegCloseKey(key);
}

// ---------- UI helpers ----------

HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0, DWORD ex_style = 0) {
    HWND h = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    return h;
}

void ApplyFonts() {
    if (g.font) DeleteObject(g.font);
    if (g.font_bold) DeleteObject(g.font_bold);
    g.font = CreateFontW(-Dp(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g.font_bold = CreateFontW(-Dp(12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    EnumChildWindows(
        g.wnd,
        [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
            return TRUE;
        },
        0);
    for (HWND h : {g.lbl_connection, g.lbl_camera, g.lbl_status}) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font_bold), TRUE);
    }
}

RECT VideoRect() {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    if (g.fullscreen) return rc;
    rc.left += Dp(kPanelWidth) + Dp(20);
    rc.top += Dp(20);
    rc.right -= Dp(20);
    rc.bottom -= Dp(20 + 36 + 12);
    if (rc.right < rc.left) rc.right = rc.left;
    if (rc.bottom < rc.top) rc.bottom = rc.top;
    return rc;
}

void Layout() {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    const int x = Dp(20);
    const int w = Dp(kPanelWidth - 40);
    const int row = Dp(30);
    HDWP dwp = BeginDeferWindowPos(24);
    auto place = [&](HWND h, int px, int py, int pw, int ph) {
        dwp = DeferWindowPos(dwp, h, nullptr, px, py, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(g.lbl_connection, x, Dp(20), w, Dp(18));
    place(g.wifi, x, Dp(44), Dp(110), Dp(24));
    place(g.usb, x + Dp(120), Dp(44), Dp(110), Dp(24));
    place(g.lbl_host, x, Dp(78), Dp(170), Dp(18));
    place(g.lbl_port, x + Dp(180), Dp(78), w - Dp(180), Dp(18));
    place(g.host, x, Dp(98), Dp(170), row);
    place(g.port, x + Dp(180), Dp(98), w - Dp(180), row);
    place(g.connect, x, Dp(140), w, Dp(36));

    place(g.lbl_camera, x, Dp(200), w, Dp(18));
    place(g.lbl_lens, x, Dp(224), w, Dp(18));
    place(g.lens, x, Dp(244), w, Dp(200));
    place(g.lbl_res, x, Dp(282), w, Dp(18));
    place(g.resolution, x, Dp(302), w, Dp(200));

    place(g.lbl_status, x, Dp(360), w, Dp(18));
    place(g.status, x, Dp(384), w, Dp(64));
    place(g.stats, x, Dp(456), w, Dp(20));

    const int bw = Dp(110);
    const int bh = Dp(36);
    const int by = rc.bottom - Dp(20) - bh;
    place(g.fullscreen_btn, rc.right - Dp(20) - bw, by, bw, bh);
    place(g.rotate_btn, rc.right - Dp(20) - bw * 2 - Dp(8), by, bw, bh);
    place(g.mirror_btn, rc.right - Dp(20) - bw * 3 - Dp(16), by, bw, bh);
    EndDeferWindowPos(dwp);
}

void ShowPanel(bool show) {
    for (HWND h : {g.lbl_connection, g.wifi, g.usb, g.lbl_host, g.host, g.lbl_port, g.port, g.connect, g.lbl_camera,
                   g.lbl_lens, g.lens, g.lbl_res, g.resolution, g.lbl_status, g.status, g.stats, g.mirror_btn,
                   g.rotate_btn, g.fullscreen_btn}) {
        ShowWindow(h, show ? SW_SHOW : SW_HIDE);
    }
}

void SetStatus(const std::wstring& text) { SetWindowTextW(g.status, text.c_str()); }

void UpdateConnectButton() {
    SetWindowTextW(g.connect, g.client.Running() ? L"Disconnect" : L"Connect");
    const bool usb = Button_GetCheck(g.usb) == BST_CHECKED;
    EnableWindow(g.host, !g.client.Running() && !usb);
    EnableWindow(g.port, !g.client.Running());
    EnableWindow(g.wifi, !g.client.Running());
    EnableWindow(g.usb, !g.client.Running());
}

void InvalidateVideo() {
    RECT rc = VideoRect();
    InvalidateRect(g.wnd, &rc, FALSE);
}

// Rotates |src| clockwise by g.rotation into |dst|.
void RotateFrame(const Frame& src, Frame* dst) {
    if (g.rotation == 0) {
        *dst = src;
        return;
    }
    const bool swap = g.rotation == 90 || g.rotation == 270;
    dst->width = swap ? src.height : src.width;
    dst->height = swap ? src.width : src.height;
    dst->bgra.resize(src.bgra.size());
    const uint32_t* in = reinterpret_cast<const uint32_t*>(src.bgra.data());
    uint32_t* out = reinterpret_cast<uint32_t*>(dst->bgra.data());
    const int w = src.width;
    const int h = src.height;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int dx, dy;
            if (g.rotation == 90) {
                dx = h - 1 - y;
                dy = x;
            } else if (g.rotation == 180) {
                dx = w - 1 - x;
                dy = h - 1 - y;
            } else {
                dx = y;
                dy = w - 1 - x;
            }
            out[dy * dst->width + dx] = in[y * w + x];
        }
    }
}

void Connect() {
    wchar_t host[256];
    GetWindowTextW(g.host, host, 256);
    BOOL ok = FALSE;
    UINT port = GetDlgItemInt(g.wnd, kIdPort, &ok, FALSE);
    const bool usb = Button_GetCheck(g.usb) == BST_CHECKED;
    if (!ok || port == 0 || port > 65535) {
        SetStatus(L"Enter a port between 1 and 65535.");
        return;
    }
    if (!usb && host[0] == L'\0') {
        SetStatus(L"Enter the IP address shown on the phone.");
        return;
    }
    const Preset& preset = kPresets[ComboBox_GetCurSel(g.resolution)];
    ConnectOptions options;
    options.host = host;
    options.port = static_cast<int>(port);
    options.usb = usb;
    options.width = preset.width;
    options.height = preset.height;
    options.fps = kFps;
    // The camera picked on the phone always wins when connecting. The PC can switch it afterwards.
    options.camera = proto::kCameraKeep;
    ComboBox_SetCurSel(g.lens, 0);
    SaveSettings();
    g.client.Start(options, g.wnd);
    UpdateConnectButton();
}

void Disconnect() {
    g.client.Stop();
    g.connected = false;
    g.has_frame = false;
    SetStatus(L"Not connected.");
    SetWindowTextW(g.stats, L"");
    UpdateConnectButton();
    InvalidateRect(g.wnd, nullptr, FALSE);
}

void ToggleFullscreen() {
    if (!g.fullscreen) {
        GetWindowPlacement(g.wnd, &g.placement);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g.wnd, MONITOR_DEFAULTTONEAREST), &mi);
        g.fullscreen = true;
        ShowPanel(false);
        SetWindowLongPtrW(g.wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowPos(g.wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        g.fullscreen = false;
        SetWindowLongPtrW(g.wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowPlacement(g.wnd, &g.placement);
        SetWindowPos(g.wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        ShowPanel(true);
    }
    InvalidateRect(g.wnd, nullptr, TRUE);
}

// ---------- painting ----------

void PaintVideo(HDC dc) {
    RECT area = VideoRect();
    HBRUSH bg = CreateSolidBrush(g.fullscreen ? RGB(0, 0, 0) : kVideoBg);
    if (!g.has_frame || g.shown.width == 0) {
        FillRect(dc, &area, bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kMuted);
        HGDIOBJ old = SelectObject(dc, g.font);
        const wchar_t* text = g.client.Running() ? L"Waiting for video..." : L"Connect to your phone to see the camera";
        DrawTextW(dc, text, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(bg);
        return;
    }

    const int aw = area.right - area.left;
    const int ah = area.bottom - area.top;
    int w = aw;
    int h = static_cast<int>(static_cast<int64_t>(aw) * g.shown.height / g.shown.width);
    if (h > ah) {
        h = ah;
        w = static_cast<int>(static_cast<int64_t>(ah) * g.shown.width / g.shown.height);
    }
    const int x = area.left + (aw - w) / 2;
    const int y = area.top + (ah - h) / 2;

    // Letterbox bars, drawn around the picture so it does not flicker.
    RECT bars[4] = {{area.left, area.top, area.right, y},
                    {area.left, y + h, area.right, area.bottom},
                    {area.left, y, x, y + h},
                    {x + w, y, area.right, y + h}};
    for (const RECT& r : bars) {
        if (r.right > r.left && r.bottom > r.top) FillRect(dc, &r, bg);
    }
    DeleteObject(bg);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = g.shown.width;
    bmi.bmiHeader.biHeight = -g.shown.height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    if (g.mirror) {
        StretchDIBits(dc, x + w - 1, y, -w, h, 0, 0, g.shown.width, g.shown.height, g.shown.bgra.data(), &bmi,
                      DIB_RGB_COLORS, SRCCOPY);
    } else {
        StretchDIBits(dc, x, y, w, h, 0, 0, g.shown.width, g.shown.height, g.shown.bgra.data(), &bmi, DIB_RGB_COLORS,
                      SRCCOPY);
    }
}

void Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g.wnd, &ps);
    RECT rc;
    GetClientRect(g.wnd, &rc);
    if (!g.fullscreen) {
        RECT panel = {0, 0, Dp(kPanelWidth), rc.bottom};
        FillRect(dc, &panel, g.panel_brush);
        RECT line = {panel.right - 1, 0, panel.right, rc.bottom};
        HBRUSH line_brush = CreateSolidBrush(kLine);
        FillRect(dc, &line, line_brush);
        DeleteObject(line_brush);

        // Ground around the video area.
        RECT video = VideoRect();
        RECT around[4] = {{panel.right, 0, rc.right, video.top},
                          {panel.right, video.bottom, rc.right, rc.bottom},
                          {panel.right, video.top, video.left, video.bottom},
                          {video.right, video.top, rc.right, video.bottom}};
        for (const RECT& r : around) {
            if (r.right > r.left && r.bottom > r.top) FillRect(dc, &r, g.ground_brush);
        }

        // Live dot next to the status heading.
        if (g.connected) {
            HBRUSH dot = CreateSolidBrush(kAccent);
            HGDIOBJ old_brush = SelectObject(dc, dot);
            HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
            const int cx = Dp(20 + 70);
            const int cy = Dp(360 + 9);
            Ellipse(dc, cx - Dp(4), cy - Dp(4), cx + Dp(4), cy + Dp(4));
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(dot);
        }
    }
    PaintVideo(dc);
    EndPaint(g.wnd, &ps);
}

// ---------- window ----------

void CreateControls() {
    g.lbl_connection = MakeControl(L"STATIC", L"CONNECTION", SS_LEFT);
    g.wifi = MakeControl(L"BUTTON", L"Wi-Fi", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, kIdWifi);
    g.usb = MakeControl(L"BUTTON", L"USB", BS_AUTORADIOBUTTON, kIdUsb);
    g.lbl_host = MakeControl(L"STATIC", L"Phone IP", SS_LEFT);
    g.lbl_port = MakeControl(L"STATIC", L"Port", SS_LEFT);
    g.host = MakeControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, kIdHost);
    g.port = MakeControl(L"EDIT", L"", ES_NUMBER | WS_TABSTOP | WS_BORDER, kIdPort);
    g.connect = MakeControl(L"BUTTON", L"Connect", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdConnect);

    g.lbl_camera = MakeControl(L"STATIC", L"CAMERA", SS_LEFT);
    g.lbl_lens = MakeControl(L"STATIC", L"Lens", SS_LEFT);
    g.lens = MakeControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdLens);
    g.lbl_res = MakeControl(L"STATIC", L"Resolution", SS_LEFT);
    g.resolution = MakeControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdResolution);

    g.lbl_status = MakeControl(L"STATIC", L"STATUS", SS_LEFT);
    g.status = MakeControl(L"STATIC", L"Not connected.", SS_LEFT);
    g.stats = MakeControl(L"STATIC", L"", SS_LEFT);

    g.mirror_btn = MakeControl(L"BUTTON", L"Mirror", BS_PUSHBUTTON | WS_TABSTOP, kIdMirror);
    g.rotate_btn = MakeControl(L"BUTTON", L"Rotate", BS_PUSHBUTTON | WS_TABSTOP, kIdRotate);
    g.fullscreen_btn = MakeControl(L"BUTTON", L"Full screen", BS_PUSHBUTTON | WS_TABSTOP, kIdFullscreen);

    ComboBox_AddString(g.lens, L"Phone's choice");
    ComboBox_AddString(g.lens, L"Back camera");
    ComboBox_AddString(g.lens, L"Front camera");
    for (const Preset& p : kPresets) {
        wchar_t label[64];
        swprintf(label, 64, L"%ls · %d fps", p.label, kFps);
        ComboBox_AddString(g.resolution, label);
    }

    // Dark themes for the common controls (Windows 10 1809 and later; ignored elsewhere).
    for (HWND h : {g.connect, g.mirror_btn, g.rotate_btn, g.fullscreen_btn}) SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
    for (HWND h : {g.lens, g.resolution, g.host, g.port}) SetWindowTheme(h, L"DarkMode_CFD", nullptr);
    // Unthemed radios so WM_CTLCOLORSTATIC can color their labels.
    for (HWND h : {g.wifi, g.usb}) SetWindowTheme(h, L"", L"");

    // Restore the last settings.
    SetWindowTextW(g.host, RegString(L"Host", L"").c_str());
    SetDlgItemInt(g.wnd, kIdPort, RegDword(L"Port", proto::kDefaultPort), FALSE);
    Button_SetCheck(RegDword(L"Usb", 0) ? g.usb : g.wifi, BST_CHECKED);
    ComboBox_SetCurSel(g.lens, 0);
    DWORD res = RegDword(L"Resolution", 1);
    ComboBox_SetCurSel(g.resolution, res < 3 ? res : 1);
    UpdateConnectButton();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.wnd = hwnd;
            g.dpi = static_cast<int>(GetDpiForWindow(hwnd));
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            CreateControls();
            ApplyFonts();
            Layout();
            SetTimer(hwnd, kStatsTimer, 1000, nullptr);
            return 0;
        }
        case WM_SIZE:
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyFonts();
            Layout();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = {Dp(760), Dp(520)};
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            const bool heading = ctl == g.lbl_connection || ctl == g.lbl_camera || ctl == g.lbl_status;
            const bool field_label = ctl == g.lbl_host || ctl == g.lbl_port || ctl == g.lbl_lens || ctl == g.lbl_res;
            SetTextColor(dc, heading || field_label ? kMuted : kText);
            SetBkColor(dc, kPanel);
            return reinterpret_cast<LRESULT>(g.panel_brush);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, kText);
            SetBkColor(dc, kGround);
            return reinterpret_cast<LRESULT>(g.ground_brush);
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            switch (id) {
                case IDOK:
                case kIdConnect:
                    if (g.client.Running()) {
                        Disconnect();
                    } else {
                        Connect();
                    }
                    return 0;
                case kIdWifi:
                case kIdUsb:
                    UpdateConnectButton();
                    return 0;
                case kIdLens:
                    if (code == CBN_SELCHANGE) {
                        const int lens = ComboBox_GetCurSel(g.lens);
                        if (g.connected && lens > 0) {
                            g.client.SendSwitchCamera(lens == 2 ? proto::kCameraFront : proto::kCameraBack);
                        }
                        SaveSettings();
                    }
                    return 0;
                case kIdResolution:
                    if (code == CBN_SELCHANGE) {
                        const Preset& p = kPresets[ComboBox_GetCurSel(g.resolution)];
                        if (g.connected) g.client.SendResolution(p.width, p.height, kFps);
                        SaveSettings();
                    }
                    return 0;
                case kIdMirror:
                    g.mirror = !g.mirror;
                    InvalidateVideo();
                    return 0;
                case kIdRotate:
                    g.rotation = (g.rotation + 90) % 360;
                    if (g.has_frame) RotateFrame(g.raw, &g.shown);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case kIdFullscreen:
                    ToggleFullscreen();
                    return 0;
            }
            break;
        }
        case WM_LBUTTONDBLCLK:
            ToggleFullscreen();
            return 0;
        case WM_TIMER:
            if (wp == kStatsTimer && g.connected) {
                int frames = 0;
                int64_t bytes = 0;
                g.client.TakeCounters(&frames, &bytes);
                wchar_t text[96];
                swprintf(text, 96, L"%d fps  ·  %.1f Mbps", frames, bytes * 8 / 1e6);
                SetWindowTextW(g.stats, text);
            }
            return 0;
        case WM_APP_STATUS:
            if (wp == g.client.Session()) SetStatus(g.client.Status());
            return 0;
        case WM_APP_CONNECTED:
            if (wp == g.client.Session()) {
                g.connected = true;
                wchar_t text[128];
                swprintf(text, 128, L"Connected.\n%d x %d at %d fps", g.client.StreamWidth(), g.client.StreamHeight(),
                         g.client.StreamFps());
                SetStatus(text);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_APP_FRAME:
            if (wp == g.client.Session() && g.client.TakeFrame(&g.raw)) {
                RotateFrame(g.raw, &g.shown);
                g.has_frame = true;
                InvalidateVideo();
            }
            return 0;
        case WM_APP_DISCONNECTED:
            if (wp == g.client.Session()) {
                std::wstring error = g.client.LastError();
                Disconnect();
                if (!error.empty()) SetStatus(error);
            }
            return 0;
        case WM_DESTROY:
            g.client.Stop();
            SaveSettings();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g.panel_brush = CreateSolidBrush(kPanel);
    g.ground_brush = CreateSolidBrush(kGround);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"fyewebcam";
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(0, L"fyewebcam", L"fyewebcam", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, MulDiv(1280, dpi, 96), MulDiv(800, dpi, 96), nullptr, nullptr, instance,
                                nullptr);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_F11 || (msg.wParam == VK_ESCAPE && g.fullscreen))) {
            ToggleFullscreen();
            continue;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    MFShutdown();
    CoUninitialize();
    WSACleanup();
    return static_cast<int>(msg.wParam);
}
