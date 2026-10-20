// rdp_client.cpp - rdp_mini client (the PC you control from)
// Build: g++ -O2 -std=c++17 rdp_client.cpp -o rdp_client.exe -lgdiplus -lgdi32 -lws2_32 -lole32 -luser32
// Run:   rdp_client <ip> <port> <password>
// Note: no TLS. Trusted internal network only.

#include "rdp_common.h"

// ---------------------------------------------------------------- client
static SOCKET gSock = INVALID_SOCKET;
static std::vector<HWND> gWnd;     // one window per server monitor
static std::vector<HBITMAP> gBmp;  // latest frame per monitor
static std::mutex gMtx;

static void sendEv(int t, int a, int b = 0, int m = 0) {
    Ev e{t, a, b, m};
    sendMsg(gSock, MSG_INPUT, &e, sizeof e);
}

static int monOf(HWND w) {
    for (size_t i = 0; i < gWnd.size(); i++) if (gWnd[i] == w) return (int)i;
    return -1;
}

static void netThread() {
    Hdr h;
    while (recvAll(gSock, &h, sizeof h)) {
        if (h.len > (64u << 20)) break;
        std::vector<char> buf(h.len);
        if (h.len && !recvAll(gSock, buf.data(), (int)h.len)) break;
        if (h.type != MSG_FRAME || h.len <= 4) continue;
        int32_t idx;
        memcpy(&idx, buf.data(), 4);
        if (idx < 0 || idx >= (int)gWnd.size()) continue;
        uint32_t n = h.len - 4;
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, n);
        memcpy(GlobalLock(hg), buf.data() + 4, n);
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
            if (gBmp[idx]) DeleteObject(gBmp[idx]);
            gBmp[idx] = hb;
        }
        InvalidateRect(gWnd[idx], NULL, FALSE);
    }
    PostMessage(gWnd[0], WM_CLOSE, 0, 0);
}

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        std::lock_guard<std::mutex> lk(gMtx);
        int mi = monOf(w);
        HBITMAP cur = mi >= 0 ? gBmp[mi] : NULL;
        if (cur) {
            BITMAP bm;
            GetObject(cur, sizeof bm, &bm);
            RECT rc;
            GetClientRect(w, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ o = SelectObject(mem, cur);
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
                            (int)((int64_t)y * 65535 / (rc.bottom - 1)), monOf(w));
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
    if (!recvAll(gSock, &h, sizeof h) || h.type != MSG_INFO || h.len < 12 || h.len > 4096 || h.len % 4) {
        printf("Bad monitor info\n");
        return 1;
    }
    std::vector<int32_t> info(h.len / 4);
    if (!recvAll(gSock, info.data(), (int)h.len) || info[0] < 1 || (size_t)info[0] * 2 + 1 > info.size()) {
        printf("Bad monitor info\n");
        return 1;
    }
    int nMon = info[0];

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"RdpMini";
    RegisterClassW(&wc);
    gWnd.assign(nMon, NULL);
    gBmp.assign(nMon, NULL);
    for (int i = 0; i < nMon; i++) { // one window per server monitor
        int mw = info[1 + 2 * i], mh = info[2 + 2 * i];
        int cw = std::min(mw, 1280), ch = (int)((int64_t)mh * cw / mw);
        RECT rc = {0, 0, cw, ch};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        wchar_t title[64];
        _snwprintf(title, 64, L"rdp_mini - monitor %d (%dx%d)", i + 1, mw, mh);
        gWnd[i] = CreateWindowW(L"RdpMini", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
    }
    for (HWND w : gWnd) ShowWindow(w, SW_SHOW);
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
    if (argc != 4) {
        printf("Usage:\n  rdp_client <ip> <port> <password>\n");
        return 1;
    }
    SetProcessDPIAware();
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    Gdiplus::GdiplusStartupInput gi;
    ULONG_PTR tok;
    Gdiplus::GdiplusStartup(&tok, &gi, NULL);
    int rc = runClient(argv[1], atoi(argv[2]), argv[3]);
    Gdiplus::GdiplusShutdown(tok);
    WSACleanup();
    return rc;
}
