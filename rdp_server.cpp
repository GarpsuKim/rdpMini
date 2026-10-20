// rdp_server.cpp - rdp_mini server (the PC being controlled)
// Build: g++ -O2 -std=c++17 rdp_server.cpp -o rdp_server.exe -lgdiplus -lgdi32 -lws2_32 -lole32 -luser32
// Run:   rdp_server <port> <password> [allowed_ip|any] [once|auto|always]
//   allowed_ip : accept connections only from this IP ("any" = no restriction, default)
//   accept mode: once   = ask the first time per client IP (default)
//                auto   = never ask
//                always = ask on every connection
// Note: no TLS. Trusted internal network only.

#include "rdp_common.h"

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
static std::vector<RECT> gMons; // server monitors, sorted left to right
static BOOL CALLBACK enumProc(HMONITOR, HDC, LPRECT r, LPARAM lp) {
    ((std::vector<RECT>*)lp)->push_back(*r);
    return TRUE;
}
static void inject(const Ev& e) {
    INPUT in[2] = {};
    int cnt = 1;
    switch (e.t) {
    case EV_MOVE: { // a,b = 0..65535 inside monitor m -> virtual desktop coordinates
        if (e.m < 0 || e.m >= (int)gMons.size()) return;
        const RECT& r = gMons[e.m];
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw < 2 || vh < 2) return;
        int px = r.left + (int)((int64_t)std::clamp(e.a, 0, 65535) * (r.right - r.left - 1) / 65535);
        int py = r.top + (int)((int64_t)std::clamp(e.b, 0, 65535) * (r.bottom - r.top - 1) / 65535);
        in[0].type = INPUT_MOUSE;
        in[0].mi.dx = (LONG)((int64_t)(px - vx) * 65535 / (vw - 1));
        in[0].mi.dy = (LONG)((int64_t)(py - vy) * 65535 / (vh - 1));
        in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        break; }
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

static std::string gAllowIp;   // empty = any client IP
static int gAcceptMode = 0;    // 0 = once (ask the first time per IP), 1 = auto (never ask), 2 = always (ask every time)

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
        if (gAcceptMode == 1) {
            // auto: no prompt
        } else if (gAcceptMode == 2 || !trusted.count(ips)) {
            std::wstring msg = L"Remote connection request: " + std::wstring(ips.begin(), ips.end()) + L"\n\nAllow?";
            ok = MessageBoxW(NULL, msg.c_str(), L"rdp_mini",
                             MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
            if (ok) trusted.insert(ips);
        }
    }
    sendMsg(c, MSG_AUTH, &ok, 1);
    if (!ok) { printf("Auth failed or denied\n"); return; }
    printf("Client accepted\n");

    gMons.clear();
    EnumDisplayMonitors(NULL, NULL, enumProc, (LPARAM)&gMons);
    std::sort(gMons.begin(), gMons.end(), [](const RECT& a, const RECT& b) {
        return a.left != b.left ? a.left < b.left : a.top < b.top;
    });
    {   // tell the client how many monitors there are and their sizes
        std::vector<int32_t> info;
        info.push_back((int32_t)gMons.size());
        for (auto& r : gMons) { info.push_back(r.right - r.left); info.push_back(r.bottom - r.top); }
        sendMsg(c, MSG_INFO, info.data(), (uint32_t)(info.size() * 4));
    }

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
    struct Mon { int W, H; HDC mem; HBITMAP dib; HGDIOBJ old; void* bits; size_t bytes; std::vector<uint8_t> prev; };
    std::vector<Mon> mons(gMons.size());
    HDC scr = GetDC(NULL);
    for (size_t i = 0; i < mons.size(); i++) {
        Mon& m = mons[i];
        m.W = gMons[i].right - gMons[i].left;
        m.H = gMons[i].bottom - gMons[i].top;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = m.W;
        bi.bmiHeader.biHeight = -m.H; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        m.bits = nullptr;
        m.mem = CreateCompatibleDC(scr);
        m.dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &m.bits, NULL, 0);
        m.old = SelectObject(m.mem, m.dib);
        m.bytes = (size_t)m.W * m.H * 4;
        m.prev.assign(m.bytes, 0);
    }
    ULONG quality = 60;

    while (alive) {
        bool sentOk = true;
        for (size_t i = 0; i < mons.size() && sentOk; i++) {
            Mon& m = mons[i];
            BitBlt(m.mem, 0, 0, m.W, m.H, scr, gMons[i].left, gMons[i].top, SRCCOPY | CAPTUREBLT);
            if (memcmp(m.prev.data(), m.bits, m.bytes) == 0) continue; // this monitor unchanged
            memcpy(m.prev.data(), m.bits, m.bytes);
            Gdiplus::Bitmap bmp(m.W, m.H, m.W * 4, PixelFormat32bppRGB, (BYTE*)m.bits);
            IStream* st = nullptr;
            CreateStreamOnHGlobal(NULL, TRUE, &st);
            Gdiplus::EncoderParameters ep;
            ep.Count = 1;
            ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
            ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            ep.Parameter[0].NumberOfValues = 1;
            ep.Parameter[0].Value = &quality;
            if (bmp.Save(st, &jpg, &ep) == Gdiplus::Ok) {
                STATSTG ss;
                st->Stat(&ss, STATFLAG_NONAME);
                HGLOBAL hg;
                GetHGlobalFromStream(st, &hg);
                void* p = GlobalLock(hg);
                Hdr fh{MSG_FRAME, ss.cbSize.LowPart + 4}; // payload = monitor index + JPEG
                int32_t idx = (int32_t)i;
                sentOk = sendAll(c, &fh, sizeof fh) && sendAll(c, &idx, 4) &&
                         sendAll(c, p, (int)ss.cbSize.LowPart);
                GlobalUnlock(hg);
            }
            st->Release();
        }
        if (!sentOk) break;
        Sleep(60);
    }
    shutdown(c, SD_BOTH);
    rx.join();
    for (Mon& m : mons) {
        SelectObject(m.mem, m.old);
        DeleteObject(m.dib);
        DeleteDC(m.mem);
    }
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
    printf("Listening (port %d, allowed IP: %s, accept mode: %s)\n", port,
           gAllowIp.empty() ? "any" : gAllowIp.c_str(),
           gAcceptMode == 1 ? "auto" : gAcceptMode == 2 ? "always" : "once");
    for (;;) {
        sockaddr_in ca = {};
        int cl = sizeof ca;
        SOCKET c = accept(ls, (sockaddr*)&ca, &cl);
        if (c == INVALID_SOCKET) continue;
        if (!gAllowIp.empty() && gAllowIp != inet_ntoa(ca.sin_addr)) {
            printf("Rejected IP %s\n", inet_ntoa(ca.sin_addr));
            closesocket(c);
            continue;
        }
        int nd = 1;
        setsockopt(c, IPPROTO_TCP, 1 /*TCP_NODELAY*/, (char*)&nd, sizeof nd);
        serveClient(c, pw);
        closesocket(c);
    }
}

int main(int argc, char** argv) {
    bool ok = argc >= 3 && argc <= 5;
    if (ok && argc >= 4 && strcmp(argv[3], "any") && strcmp(argv[3], "*")) {
        if (inet_addr(argv[3]) == INADDR_NONE) ok = false;
        else gAllowIp = argv[3];
    }
    if (ok && argc == 5) {
        if (!strcmp(argv[4], "once")) gAcceptMode = 0;
        else if (!strcmp(argv[4], "auto")) gAcceptMode = 1;
        else if (!strcmp(argv[4], "always")) gAcceptMode = 2;
        else ok = false;
    }
    if (!ok) {
        printf("Usage:\n  rdp_server <port> <password> [allowed_ip|any] [once|auto|always]\n");
        return 1;
    }
    SetProcessDPIAware();
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    Gdiplus::GdiplusStartupInput gi;
    ULONG_PTR tok;
    Gdiplus::GdiplusStartup(&tok, &gi, NULL);
    int rc = runServer(atoi(argv[1]), argv[2]);
    Gdiplus::GdiplusShutdown(tok);
    WSACleanup();
    return rc;
}
