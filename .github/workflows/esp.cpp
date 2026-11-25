#include <windows.h>
#include <psapi.h>
#include <cmath>
#include <cstdio>
#include <cstring>

static constexpr uintptr_t dwEntityList      = 0x2578DE0;
static constexpr uintptr_t dwLocalPlayerPawn = 0x23E6208;
static constexpr uintptr_t dwViewMatrix      = 0x23C33F0;
static constexpr uintptr_t m_pGameSceneNode  = 0x330;
static constexpr uintptr_t m_iHealth         = 0x34C;
static constexpr uintptr_t m_iTeamNum        = 0x3E7;
static constexpr uintptr_t m_lifeState       = 0x354;
static constexpr uintptr_t m_vecAbsOrigin    = 0xC8;
static constexpr uintptr_t m_hPlayerPawn     = 0x8FC;

struct Vector3 { float x, y, z; };

static HANDLE    g_hProc   = nullptr;
static uintptr_t g_client  = 0;
static int       g_screenW = 0;
static int       g_screenH = 0;

template <typename T>
static T RPM(uintptr_t addr) {
    T val{};
    ReadProcessMemory(g_hProc, reinterpret_cast<LPCVOID>(addr), &val, sizeof(T), nullptr);
    return val;
}

static DWORD FindCs2Pid() {
    HWND wnd = FindWindowA(nullptr, "Counter-Strike 2");
    if (wnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(wnd, &pid);
        if (pid) return pid;
    }
    DWORD pids[4096]; DWORD needed = 0;
    if (EnumProcesses(pids, sizeof(pids), &needed)) {
        for (DWORD i = 0; i < needed / sizeof(DWORD); i++) {
            if (!pids[i]) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pids[i]);
            if (!h) continue;
            HMODULE mods[1]; DWORD cb = 0;
            if (EnumProcessModules(h, mods, sizeof(mods), &cb)) {
                char name[MAX_PATH] = {};
                GetModuleBaseNameA(h, mods[0], name, MAX_PATH);
                if (_stricmp(name, "cs2.exe") == 0) { CloseHandle(h); return pids[i]; }
            }
            CloseHandle(h);
        }
    }
    return 0;
}

static uintptr_t GetClientBase(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return 0;
    HMODULE mods[1024]; DWORD cb = 0;
    uintptr_t base = 0;
    if (EnumProcessModules(h, mods, sizeof(mods), &cb)) {
        for (DWORD i = 0; i < cb / sizeof(HMODULE); i++) {
            char name[MAX_PATH] = {};
            GetModuleBaseNameA(h, mods[i], name, MAX_PATH);
            if (_stricmp(name, "client.dll") == 0) {
                base = reinterpret_cast<uintptr_t>(mods[i]); break;
            }
        }
    }
    CloseHandle(h);
    return base;
}

static uintptr_t ResolvePawn(uintptr_t entityList, uint32_t handle) {
    if (!handle) return 0;
    int idx = handle & 0x7FFF;
    uintptr_t chunk = RPM<uintptr_t>(entityList + (static_cast<uintptr_t>(idx >> 9) << 3) + 0x10);
    if (!chunk) return 0;
    return RPM<uintptr_t>(chunk + static_cast<uintptr_t>(idx & 0x1FF) * 0x78);
}

static bool W2S(const float* m, const Vector3& w, float& sx, float& sy) {
    float cw = m[12]*w.x + m[13]*w.y + m[14]*w.z + m[15];
    if (cw < 0.01f) return false;
    float cx = m[0]*w.x + m[1]*w.y + m[2]*w.z + m[3];
    float cy = m[4]*w.x + m[5]*w.y + m[6]*w.z + m[7];
    sx = (g_screenW / 2.0f) * (cx / cw + 1.0f);
    sy = (g_screenH / 2.0f) * (1.0f - cy / cw);
    return true;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int main() {
    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);

    printf("[*] waiting for cs2.exe...\n");
    DWORD pid = 0;
    while (!pid) { pid = FindCs2Pid(); if (!pid) Sleep(2000); }
    printf("[+] found pid=%lu\n", pid);

    g_hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!g_hProc) { printf("[-] OpenProcess failed\n"); return 1; }

    while (!g_client) { g_client = GetClientBase(pid); if (!g_client) Sleep(2000); }
    printf("[+] client.dll=0x%llX\n", (unsigned long long)g_client);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "CS2ESP";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        "CS2ESP", "", WS_POPUP,
        0, 0, g_screenW, g_screenH,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { CloseHandle(g_hProc); return 1; }

    SetLayeredWindowAttributes(hwnd, RGB(0,0,0), 255, LWA_COLORKEY);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    printf("[+] overlay active. DEL = exit\n");

    HDC hdc = GetDC(hwnd);
    bool running = true;

    while (running) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        if (!running) break;
        if (GetAsyncKeyState(VK_DELETE) & 1) break;
        if (WaitForSingleObject(g_hProc, 0) == WAIT_OBJECT_0) break;

        RECT rc = {0, 0, g_screenW, g_screenH};
        HBRUSH blk = CreateSolidBrush(RGB(0,0,0));
        FillRect(hdc, &rc, blk);
        DeleteObject(blk);

        uintptr_t localPawn = RPM<uintptr_t>(g_client + dwLocalPlayerPawn);
        int localTeam = localPawn ? RPM<int>(localPawn + m_iTeamNum) : 0;

        Vector3 localOrigin = {0,0,0};
        if (localPawn) {
            uintptr_t ls = RPM<uintptr_t>(localPawn + m_pGameSceneNode);
            if (ls) localOrigin = RPM<Vector3>(ls + m_vecAbsOrigin);
        }

        float vm[16] = {};
        ReadProcessMemory(g_hProc, reinterpret_cast<LPCVOID>(g_client + dwViewMatrix), vm, 64, nullptr);
        uintptr_t entityList = RPM<uintptr_t>(g_client + dwEntityList);

        for (int i = 0; i < 64; i++) {
            uintptr_t controller = RPM<uintptr_t>(entityList + (static_cast<uintptr_t>(i) << 5));
            if (!controller) continue;
            uint32_t pawnHandle = RPM<uint32_t>(controller + m_hPlayerPawn);
            uintptr_t pawn = ResolvePawn(entityList, pawnHandle);
            if (!pawn || pawn == localPawn) continue;
            int health = RPM<int>(pawn + m_iHealth);
            if (health <= 0) continue;
            if (RPM<uint8_t>(pawn + m_lifeState) != 0) continue;
            if (RPM<int>(pawn + m_iTeamNum) == localTeam) continue;
            uintptr_t node = RPM<uintptr_t>(pawn + m_pGameSceneNode);
            if (!node) continue;
            Vector3 feet = RPM<Vector3>(node + m_vecAbsOrigin);
            Vector3 head = {feet.x, feet.y, feet.z + 75.0f};
            float fsx, fsy, hsx, hsy;
            if (!W2S(vm, feet, fsx, fsy)) continue;
            if (!W2S(vm, head, hsx, hsy)) continue;
            float boxH = fsy - hsy;
            if (boxH < 2.0f) continue;
            float boxW = boxH * 0.4f;
            float cx = (fsx + hsx) * 0.5f;
            int left   = (int)(cx - boxW * 0.5f);
            int right  = (int)(cx + boxW * 0.5f);
            int top    = (int)hsy;
            int bottom = (int)fsy;

            HPEN pen = CreatePen(PS_SOLID, 1, RGB(255,255,255));
            HGDIOBJ op = SelectObject(hdc, pen);
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, left, top, right, bottom);
            SelectObject(hdc, ob); SelectObject(hdc, op);
            DeleteObject(pen);

            float hf = max(0.0f, min(1.0f, health / 100.0f));
            int barH = (int)(boxH * hf);
            int barX = left - 5;
            HBRUSH bgb = CreateSolidBrush(RGB(30,30,30));
            RECT rb = {barX-2, top, barX+2, bottom};
            FillRect(hdc, &rb, bgb); DeleteObject(bgb);
            HBRUSH fgb = CreateSolidBrush(RGB((int)(255*(1.0f-hf)), (int)(255*hf), 0));
            RECT rf = {barX-2, bottom-barH, barX+2, bottom};
            FillRect(hdc, &rf, fgb); DeleteObject(fgb);

            float dx = feet.x-localOrigin.x, dy = feet.y-localOrigin.y, dz = feet.z-localOrigin.z;
            float meters = std::sqrt(dx*dx+dy*dy+dz*dz)/100.0f;
            char buf[32]; std::snprintf(buf, sizeof(buf), "%.0fm hp=%d", meters, health);
            SetTextColor(hdc, RGB(255,255,255));
            SetBkMode(hdc, TRANSPARENT);
            TextOutA(hdc, left, top-14, buf, (int)std::strlen(buf));
        }
        Sleep(16);
    }

    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    CloseHandle(g_hProc);
    return 0;
}
