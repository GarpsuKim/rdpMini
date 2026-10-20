// rdp_common.h - shared by rdp_server.cpp and rdp_client.cpp
#pragma once

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
struct Ev { int32_t t, a, b, m; }; // m = monitor index (mouse move)
enum { MSG_AUTH = 1, MSG_FRAME = 2, MSG_INPUT = 3, MSG_INFO = 4 };
enum { EV_MOVE = 1, EV_BTN = 2, EV_WHEEL = 3 };

static inline bool sendAll(SOCKET s, const void* p, int n) {
    const char* c = (const char*)p;
    while (n > 0) { int r = send(s, c, n, 0); if (r <= 0) return false; c += r; n -= r; }
    return true;
}
static inline bool recvAll(SOCKET s, void* p, int n) {
    char* c = (char*)p;
    while (n > 0) { int r = recv(s, c, n, 0); if (r <= 0) return false; c += r; n -= r; }
    return true;
}
static inline bool sendMsg(SOCKET s, uint8_t type, const void* p, uint32_t len) {
    Hdr h{type, len};
    return sendAll(s, &h, sizeof h) && (len == 0 || sendAll(s, p, (int)len));
}
