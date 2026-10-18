// rdp_mini.cpp - Minimal LAN remote control (Windows <-> Windows, mouse only)
// Build: g++ -O2 -std=c++17 rdp_mini.cpp -o rdp_mini.exe -lgdiplus -lgdi32 -lws2_32 -lole32 -luser32
// Run:
//   controlled PC  : rdp_mini server 7070 password
//   controlling PC : rdp_mini client 192.168.0.10 7070 password
// Note: no TLS. Trusted internal network only.

#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
using std::min;
using std::max;
#include <objidl.h>
#include <gdiplus.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma pack(push, 1)
struct Hdr { uint8_t type; uint32_t len; };
#pragma pack(pop)
struct Ev { int32_t t, a, b; };
enum { MSG_AUTH = 1, MSG_FRAME = 2, MSG_INPUT = 3 };
enum { EV_MOVE = 1, EV_BTN = 2, EV_WHEEL = 3 };

static bool sendAll(SOCKET s, const void* p, int n) {
    const char* c = (const char*)p;
    while (n > 0) { int r = send(s, c, n, 0); if (r <= 0) return false; c += r; n -= r; }
    return true;
}
static bool recvAll(SOCKET s, void* p, int n) {
    char* c = (char*)p;
    while (n > 0) { int r = recv(s, c, n, 0); if (r <= 0) return false; c += r; n -= r; }
    return true;
}
static bool sendMsg(SOCKET s, uint8_t type, const void* p, uint32_t len) {
    Hdr h{type, len};
    return sendAll(s, &h, sizeof h) && (len == 0 || sendAll(s, p, (int)len));
}

static bool getJpegClsid(CLSID* out) {
    UINT n = 0, sz = 0;
    Gdiplus::GetImageEncodersSize(&n, &sz);
    if (!sz) return false;
    std::vector<char> buf(sz);
    auto* ci = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(n, sz, ci);
    for (UINT i = 0; i < n; i++)
        if (!wcscmp(ci[i].MimeType, L"image/jpeg")) { *out = ci[i].Clsid; return true; }
    return false;
}

// ---------------------------------------------------------------- server
static void inject(const Ev& e) {
    INPUT in[2] = {};
    int cnt = 1;
    switch (e.t) {
    case EV_MOVE:
        in[0].type = INPUT_MOUSE;
        in[0].mi.dx = std::clamp(e.a, 0, 65535);
        in[0].mi.dy = std::clamp(e.b, 0, 65535);
        in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        break;
    case EV_BTN: {
        DWORD f = (DWORD)e.a;
        if (f != MOUSEEVENTF_LEFTDOWN && f != MOUSEEVENTF_LEFTUP &&
            f != MOUSEEVENTF_RIGHTDOWN && f != MOUSEEVENTF_RIGHTUP &&
            f != MOUSEEVENTF_MIDDLEDOWN && f != MOUSEEVENTF_MIDDLEUP) return;
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = f;
        break; }
    case EV_WHEEL:
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_WHEEL;
        in[0].mi.mouseData = (DWORD)e.a;
        break;
    default: return;
    }
    SendInput(cnt, in, sizeof(INPUT));
}

static void serveClient(SOCKET c, const std::string& pw) {
    Hdr h;
    if (!recvAll(c, &h, sizeof h) || h.type != MSG_AUTH || h.len > 256) return;
    std::string got(h.len, '\0');
    if (h.len && !recvAll(c, &got[0], (int)h.len)) return;
    static std::set<std::string> trusted; // client IPs accepted once (cleared when the server restarts)
    uint8_t ok = (got == pw);
    if (ok) { // password correct: ask the server user, unless this IP was already accepted
        sockaddr_in pa = {};
        int pl = sizeof pa;
        getpeername(c, (sockaddr*)&pa, &pl);
        std::string ips = inet_ntoa(pa.sin_addr);
        if (!trusted.count(ips)) {
            std::wstring msg = L"Remote connection request: " + std::wstring(ips.begin(), ips.end()) + L"\n\nAllow?";
            ok = MessageBoxW(NULL, msg.c_str(), L"rdp_mini",
                             MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
            if (ok) trusted.insert(ips);
        }
    }
    sendMsg(c, MSG_AUTH, &ok, 1);
    if (!ok) { printf("Auth failed or denied\n"); return; }
    printf("Client accepted\n");

    std::atomic<bool> alive(true);
    std::thread rx([&] {
        Hdr h;
        while (recvAll(c, &h, sizeof h)) {
            if (h.len > 1024) break;
            Ev e;
            if (h.type == MSG_INPUT && h.len == sizeof e) {
                if (!recvAll(c, &e, sizeof e)) break;
                inject(e);
            } else {
                std::vector<char> skip(h.len);
                if (h.len && !recvAll(c, skip.data(), (int)h.len)) break;
            }
        }
        alive = false;
    });

    CLSID jpg;
    getJpegClsid(&jpg);
    int VX = GetSystemMetrics(SM_XVIRTUALSCREEN), VY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int W = GetSystemMetrics(SM_CXVIRTUALSCREEN), H = GetSystemMetrics(SM_CYVIRTUALSCREEN); // all monitors
    HDC scr = GetDC(NULL), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    size_t bytes = (size_t)W * H * 4;
    std::vector<uint8_t> prev(bytes, 0);
    ULONG quality = 60;

    while (alive) {
        BitBlt(mem, 0, 0, W, H, scr, VX, VY, SRCCOPY | CAPTUREBLT);
        if (memcmp(prev.data(), bits, bytes) != 0) {
            memcpy(prev.data(), bits, bytes);
            Gdiplus::Bitmap bmp(W, H, W * 4, PixelFormat32bppRGB, (BYTE*)bits);
            IStream* st = nullptr;
            CreateStreamOnHGlobal(NULL, TRUE, &st);
            Gdiplus::EncoderParameters ep;
            ep.Count = 1;
            ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
            ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            ep.Parameter[0].NumberOfValues = 1;
            ep.Parameter[0].Value = &quality;
            bool sent = true;
            if (bmp.Save(st, &jpg, &ep) == Gdiplus::Ok) {
                STATSTG ss;
                st->Stat(&ss, STATFLAG_NONAME);
                HGLOBAL hg;
                GetHGlobalFromStream(st, &hg);
                void* p = GlobalLock(hg);
                sent = sendMsg(c, MSG_FRAME, p, ss.cbSize.LowPart);
                GlobalUnlock(hg);
            }
            st->Release();
            if (!sent) break;
        }
        Sleep(60);
    }
    shutdown(c, SD_BOTH);
    rx.join();
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);
    printf("Client disconnected\n");
}

static int runServer(int port, const std::string& pw) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof one);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)port);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 1) != 0) {
        printf("Failed to open port %d\n", port);
        return 1;
    }
    printf("Listening (port %d)\n", port);
    for (;;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        int nd = 1;
        setsockopt(c, IPPROTO_TCP, 1 /*TCP_NODELAY*/, (char*)&nd, sizeof nd);
        serveClient(c, pw);
        closesocket(c);
    }
}

// ---------------------------------------------------------------- client
static SOCKET gSock = INVALID_SOCKET;
static HWND gWnd;
static HBITMAP gBmp = NULL;
static std::mutex gMtx;

static void sendEv(int t, int a, int b = 0) {
    Ev e{t, a, b};
    sendMsg(gSock, MSG_INPUT, &e, sizeof e);
}

static void netThread() {
    Hdr h;
    while (recvAll(gSock, &h, sizeof h)) {
        if (h.len > (64u << 20)) break;
        std::vector<char> buf(h.len);
        if (h.len && !recvAll(gSock, buf.data(), (int)h.len)) break;
        if (h.type != MSG_FRAME) continue;
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, h.len);
        memcpy(GlobalLock(hg), buf.data(), h.len);
        GlobalUnlock(hg);
        IStream* st = nullptr;
        CreateStreamOnHGlobal(hg, TRUE, &st);
        HBITMAP hb = NULL;
        {
            Gdiplus::Bitmap bmp(st);
            bmp.GetHBITMAP(Gdiplus::Color(0, 0, 0), &hb);
        }
        st->Release();
        if (!hb) continue;
        {
            std::lock_guard<std::mutex> lk(gMtx);
            if (gBmp) DeleteObject(gBmp);
            gBmp = hb;
        }
        InvalidateRect(gWnd, NULL, FALSE);
    }
    PostMessage(gWnd, WM_CLOSE, 0, 0);
}

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        std::lock_guard<std::mutex> lk(gMtx);
        if (gBmp) {
            BITMAP bm;
            GetObject(gBmp, sizeof bm, &bm);
            RECT rc;
            GetClientRect(w, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ o = SelectObject(mem, gBmp);
            SetStretchBltMode(dc, HALFTONE);
            StretchBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(mem, o);
            DeleteDC(mem);
        }
        EndPaint(w, &ps);
        return 0; }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        RECT rc;
        GetClientRect(w, &rc);
        if (rc.right > 1 && rc.bottom > 1) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            sendEv(EV_MOVE, (int)((int64_t)x * 65535 / (rc.right - 1)),
                            (int)((int64_t)y * 65535 / (rc.bottom - 1)));
        }
        return 0; }
    case WM_LBUTTONDOWN: SetCapture(w); sendEv(EV_BTN, MOUSEEVENTF_LEFTDOWN); return 0;
    case WM_LBUTTONUP:   ReleaseCapture(); sendEv(EV_BTN, MOUSEEVENTF_LEFTUP); return 0;
    case WM_RBUTTONDOWN: SetCapture(w); sendEv(EV_BTN, MOUSEEVENTF_RIGHTDOWN); return 0;
    case WM_RBUTTONUP:   ReleaseCapture(); sendEv(EV_BTN, MOUSEEVENTF_RIGHTUP); return 0;
    case WM_MBUTTONDOWN: SetCapture(w); sendEv(EV_BTN, MOUSEEVENTF_MIDDLEDOWN); return 0;
    case WM_MBUTTONUP:   ReleaseCapture(); sendEv(EV_BTN, MOUSEEVENTF_MIDDLEUP); return 0;
    case WM_MOUSEWHEEL:
        sendEv(EV_WHEEL, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static int runClient(const char* ip, int port, const std::string& pw) {
    gSock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr(ip);
    a.sin_port = htons((u_short)port);
    if (connect(gSock, (sockaddr*)&a, sizeof a) != 0) { printf("Connection failed\n"); return 1; }
    int nd = 1;
    setsockopt(gSock, IPPROTO_TCP, 1 /*TCP_NODELAY*/, (char*)&nd, sizeof nd);
    sendMsg(gSock, MSG_AUTH, pw.data(), (uint32_t)pw.size());
    Hdr h;
    uint8_t ok = 0;
    if (!recvAll(gSock, &h, sizeof h) || h.len != 1 || !recvAll(gSock, &ok, 1) || !ok) {
        printf("Auth failed or connection denied\n");
        return 1;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"RdpMini";
    RegisterClassW(&wc);
    gWnd = CreateWindowW(L"RdpMini", L"rdp_mini", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    std::thread t(netThread);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    shutdown(gSock, SD_BOTH);
    closesocket(gSock);
    t.join();
    return 0;
}

int main(int argc, char** argv) {
    SetProcessDPIAware();
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    Gdiplus::GdiplusStartupInput gi;
    ULONG_PTR tok;
    Gdiplus::GdiplusStartup(&tok, &gi, NULL);
    int rc = 1;
    if (argc == 4 && !strcmp(argv[1], "server"))
        rc = runServer(atoi(argv[2]), argv[3]);
    else if (argc == 5 && !strcmp(argv[1], "client"))
        rc = runClient(argv[2], atoi(argv[3]), argv[4]);
    else
        printf("Usage:\n  rdp_mini server <port> <password>\n  rdp_mini client <ip> <port> <password>\n");
    Gdiplus::GdiplusShutdown(tok);
    WSACleanup();
    return rc;
}
