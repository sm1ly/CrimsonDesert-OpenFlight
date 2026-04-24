#define _CRT_SECURE_NO_WARNINGS
#include "pch.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

// CD 1.04.02 target RVA for velocity/position integration
static const uint32_t kPatchRVA = 0x38524BC;
static const uint8_t  kExpected[5] = { 0x41, 0x0F, 0x11, 0x45, 0x00 }; // movups [r13], xmm0

static uint8_t* g_trampoline = nullptr;
static uint8_t* g_patchAddr  = nullptr;

// Aligned array for addps instruction (X, Y, Z, W)
alignas(16) static float g_BoostVec[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static volatile int32_t g_BoostActive = 0;

static int g_AscendKey = VK_NUMPAD9;
static int g_DescendKey = VK_NUMPAD8;
static float g_AscendSpeed = 1.5f;
static float g_DescendSpeed = -1.5f;

static bool IsGameForeground() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

static void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA("CDFlight.asi"), path, MAX_PATH);
    std::string iniPath = path;
    size_t pos = iniPath.find_last_of("\\/");
    if (pos != std::string::npos) {
        iniPath = iniPath.substr(0, pos) + "\\CDFlight.ini";
    }

    g_AscendKey = GetPrivateProfileIntA("Settings", "AscendKey", VK_NUMPAD9, iniPath.c_str());
    g_DescendKey = GetPrivateProfileIntA("Settings", "DescendKey", VK_NUMPAD8, iniPath.c_str());
    
    char buf[32];
    GetPrivateProfileStringA("Settings", "AscendSpeed", "1.5", buf, sizeof(buf), iniPath.c_str());
    g_AscendSpeed = std::stof(buf);
    
    GetPrivateProfileStringA("Settings", "DescendSpeed", "-1.5", buf, sizeof(buf), iniPath.c_str());
    g_DescendSpeed = std::stof(buf);
}

static DWORD WINAPI KeyPollThread(LPVOID) {
    while (true) {
        if (IsGameForeground()) {
            if (GetAsyncKeyState(g_AscendKey) & 0x8000) {
                g_BoostVec[1] = g_AscendSpeed;
                g_BoostActive = 1;
            } else if (GetAsyncKeyState(g_DescendKey) & 0x8000) {
                g_BoostVec[1] = g_DescendSpeed;
                g_BoostActive = 1;
            } else {
                g_BoostVec[1] = 0.0f;
                g_BoostActive = 0;
            }
        } else {
            g_BoostVec[1] = 0.0f;
            g_BoostActive = 0;
        }
        Sleep(10);
    }
    return 0;
}

static bool InstallPatch() {
    HMODULE hGame = GetModuleHandleA(nullptr);
    if (!hGame) return false;
    g_patchAddr = reinterpret_cast<uint8_t*>(hGame) + kPatchRVA;

    if (memcmp(g_patchAddr, kExpected, 5) != 0) return false;

    uintptr_t base = reinterpret_cast<uintptr_t>(g_patchAddr) & ~static_cast<uintptr_t>(0xFFFF);
    const uintptr_t kWindow = 0x70000000ull; 
    for (uintptr_t off = 0x10000; off < kWindow && !g_trampoline; off += 0x10000) {
        void* hi = VirtualAlloc(reinterpret_cast<void*>(base + off), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (hi) { g_trampoline = (uint8_t*)hi; break; }
        void* lo = VirtualAlloc(reinterpret_cast<void*>(base - off), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (lo) { g_trampoline = (uint8_t*)lo; break; }
    }
    if (!g_trampoline) return false;

    uint8_t* p = g_trampoline;

    // Check if boost is active
    *p++ = 0x50; // push rax
    
    *p++ = 0x48; *p++ = 0xB8; // mov rax, &g_BoostActive
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_BoostActive); p += 8;
    
    *p++ = 0x83; *p++ = 0x38; *p++ = 0x00; // cmp dword ptr [rax], 0
    *p++ = 0x74; *p++ = 0x0D; // je skip (13 bytes forward)

    // Add boost to xmm0
    *p++ = 0x48; *p++ = 0xB8; // mov rax, &g_BoostVec
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_BoostVec[0]); p += 8;
    
    *p++ = 0x0F; *p++ = 0x58; *p++ = 0x00; // addps xmm0, [rax]

    // skip:
    *p++ = 0x58; // pop rax

    // Original instruction: movups [r13], xmm0
    *p++ = 0x41; *p++ = 0x0F; *p++ = 0x11; *p++ = 0x45; *p++ = 0x00;

    // Jump back
    *p++ = 0xE9; // jmp rel32
    intptr_t relBack = (g_patchAddr + 5) - (p + 4);
    *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>(relBack); p += 4;

    intptr_t relFwd = g_trampoline - (g_patchAddr + 5);
    DWORD oldProt;
    VirtualProtect(g_patchAddr, 5, PAGE_EXECUTE_READWRITE, &oldProt);
    g_patchAddr[0] = 0xE9;
    *reinterpret_cast<int32_t*>(g_patchAddr + 1) = static_cast<int32_t>(relFwd);
    VirtualProtect(g_patchAddr, 5, oldProt, &oldProt);

    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LoadConfig();
        CreateThread(nullptr, 0, KeyPollThread, nullptr, 0, nullptr);
        InstallPatch();
    }
    return TRUE;
}
