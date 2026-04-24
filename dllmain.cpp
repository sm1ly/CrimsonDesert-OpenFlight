#define _CRT_SECURE_NO_WARNINGS
#include "pch.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static uint8_t* g_trampoline = nullptr;
static uint8_t* g_patchAddr  = nullptr;

// Multiplier vector (X, Y, Z, W) - default is 1.0 (no change)
alignas(16) static float g_MultiplierVec[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
// Additive vector (X, Y, Z, W) - default is 0.0 (no change)
alignas(16) static float g_AddVec[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

static int g_AscendKey = VK_NUMPAD9;
static int g_DescendKey = VK_NUMPAD8;
static int g_ForwardKey = VK_LSHIFT;
static float g_AscendSpeed = 6.0f;
static float g_DescendSpeed = -6.0f;
static float g_ForwardMultiplier = 8.0f; 

static void WriteLog(const char* msg) {
    HANDLE h = CreateFileA("CDFlight_Context.log", FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, msg, (DWORD)strlen(msg), &wrote, nullptr);
    WriteFile(h, "\r\n", 2, &wrote, nullptr);
    CloseHandle(h);
}

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
    g_ForwardKey = GetPrivateProfileIntA("Settings", "ForwardKey", VK_LSHIFT, iniPath.c_str());
    
    char buf[32];
    GetPrivateProfileStringA("Settings", "AscendSpeed", "6.0", buf, sizeof(buf), iniPath.c_str());
    g_AscendSpeed = std::stof(buf);
    
    GetPrivateProfileStringA("Settings", "DescendSpeed", "-6.0", buf, sizeof(buf), iniPath.c_str());
    g_DescendSpeed = std::stof(buf);

    GetPrivateProfileStringA("Settings", "ForwardSpeed", "8.0", buf, sizeof(buf), iniPath.c_str());
    g_ForwardMultiplier = std::stof(buf);
}

// Dynamically scan for the AOB pattern to find the exact injection point regardless of game version!
// Pattern: 49 3B F7 0F 8C ?? ?? ?? ?? 0F 28 C6 F3 45 0F 5C C8 41 0F 58 45 00 41 0F 11 45 00
static uint8_t* FindHookAddress() {
    HMODULE hGame = GetModuleHandleA(nullptr);
    if (!hGame) return nullptr;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hGame;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t*)hGame + dos->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    uint8_t* base = (uint8_t*)hGame;

    const uint8_t pattern[] = { 
        0x49, 0x3B, 0xF7, 0x0F, 0x8C, 0x00, 0x00, 0x00, 0x00, // +0 (?? ?? ?? ??)
        0x0F, 0x28, 0xC6, 0xF3, 0x45, 0x0F, 0x5C, 0xC8,       // +9
        0x41, 0x0F, 0x58, 0x45, 0x00,                         // +17
        0x41, 0x0F, 0x11, 0x45, 0x00                          // +22  <-- We want to hook this instruction
    };
    const char* mask = "xxxxx????xxxxxxxxxxxxxxxxxx";
    size_t len = strlen(mask);

    for (DWORD i = 0; i < size - len; i++) {
        bool found = true;
        for (size_t j = 0; j < len; j++) {
            if (mask[j] != '?' && base[i + j] != pattern[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            // Found the start of the pattern. The instruction we want to hook is at +22 bytes.
            return base + i + 22;
        }
    }
    return nullptr;
}

static DWORD WINAPI KeyPollThread(LPVOID) {
    while (true) {
        if (IsGameForeground()) {
            if (GetAsyncKeyState(g_ForwardKey) & 0x8000) {
                // Умножаем X и Z вектор (скорость/дельту), чтобы лететь ровно по камере
                g_MultiplierVec[0] = g_ForwardMultiplier;
                g_MultiplierVec[2] = g_ForwardMultiplier;
            } else {
                g_MultiplierVec[0] = 1.0f;
                g_MultiplierVec[2] = 1.0f;
            }

            if (GetAsyncKeyState(g_AscendKey) & 0x8000) {
                g_AddVec[1] = g_AscendSpeed;
            } else if (GetAsyncKeyState(g_DescendKey) & 0x8000) {
                g_AddVec[1] = g_DescendSpeed;
            } else {
                g_AddVec[1] = 0.0f;
            }
        } else {
            g_MultiplierVec[0] = 1.0f;
            g_MultiplierVec[2] = 1.0f;
            g_AddVec[1] = 0.0f;
        }
        Sleep(10);
    }
    return 0;
}

static bool InstallPatch() {
    g_patchAddr = FindHookAddress();
    
    if (!g_patchAddr) {
        WriteLog("ERROR: AOB Scan failed! Could not find physics update instruction in memory.");
        return false;
    }

    char buf[256];
    sprintf_s(buf, "SUCCESS: AOB Scanner found hook address at %p", g_patchAddr);
    WriteLog(buf);

    // Ensure it's exactly the instruction we expect (41 0F 11 45 00)
    if (g_patchAddr[0] != 0x41 || g_patchAddr[1] != 0x0F || g_patchAddr[2] != 0x11 || g_patchAddr[3] != 0x45 || g_patchAddr[4] != 0x00) {
        WriteLog("ERROR: Found address does not match expected 'movups [r13], xmm0' bytes!");
        return false;
    }

    uintptr_t base = reinterpret_cast<uintptr_t>(g_patchAddr) & ~static_cast<uintptr_t>(0xFFFF);
    const uintptr_t kWindow = 0x70000000ull; 
    for (uintptr_t off = 0x10000; off < kWindow && !g_trampoline; off += 0x10000) {
        void* hi = VirtualAlloc(reinterpret_cast<void*>(base + off), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (hi) { g_trampoline = (uint8_t*)hi; break; }
        void* lo = VirtualAlloc(reinterpret_cast<void*>(base - off), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (lo) { g_trampoline = (uint8_t*)lo; break; }
    }
    
    if (!g_trampoline) {
        WriteLog("ERROR: Failed to allocate trampoline memory.");
        return false;
    }

    uint8_t* p = g_trampoline;

    // --- ELEGANT BRANCHLESS MATH HOOK ---
    // No cmp, no je, no stack alignment issues, no xmm1 trashing.
    // xmm0 = (xmm0 * g_MultiplierVec) + g_AddVec
    
    *p++ = 0x50; // push rax

    // Multiply X and Z (Forward Speed)
    *p++ = 0x48; *p++ = 0xB8; // mov rax, &g_MultiplierVec
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_MultiplierVec[0]); p += 8;
    *p++ = 0x0F; *p++ = 0x59; *p++ = 0x00; // mulps xmm0, [rax]

    // Add Y (Ascend/Descend Speed)
    *p++ = 0x48; *p++ = 0xB8; // mov rax, &g_AddVec
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_AddVec[0]); p += 8;
    *p++ = 0x0F; *p++ = 0x58; *p++ = 0x00; // addps xmm0, [rax]

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

    WriteLog("SUCCESS: Trampoline hook installed completely.");
    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        DeleteFileA("CDFlight_Context.log");
        WriteLog("--- CDFlight Open Source v3.0 (Dynamic AOB Edition) ---");
        LoadConfig();
        CreateThread(nullptr, 0, KeyPollThread, nullptr, 0, nullptr);
        InstallPatch();
    }
    return TRUE;
}
