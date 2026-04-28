// CDFlight v8.18
// Open Source Flight Mod for Crimson Desert
// Core AOB & Inline Assembly Logic by sm1ly/Middle
// Smooth Ramping Physics concept adapted from EnhancedFlight by Bambozu

#define _CRT_SECURE_NO_WARNINGS
#include "pch.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

static uint8_t* g_trampoline = nullptr;
static uint8_t* g_patchAddr  = nullptr;

alignas(16) static float g_MultiplierVec[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
alignas(16) static float g_AddVec[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

static int g_AscendKey = VK_NUMPAD9;
static int g_DescendKey = VK_NUMPAD8;
static int g_ForwardKey = VK_LSHIFT;
static float g_AscendSpeed = 5.0f;     
static float g_DescendSpeed = -5.0f;   
static float g_ForwardMultiplier = 8.0f; 

static float g_AscendRampUpMs = 300.0f;
static float g_AscendRampDownMs = 200.0f;
static float g_DescendRampUpMs = 300.0f;
static float g_DescendRampDownMs = 200.0f;
static float g_ForwardRampUpMs = 500.0f;
static float g_ForwardRampDownMs = 300.0f;

static bool g_HookInstalled = false;

struct SharedData {
    float ForwardX;
    float ForwardZ;
    float OverrideVelX;
    float OverrideVelZ;
    uint32_t DoOverride;
};
static SharedData* g_Shared = nullptr;

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

    if (GetFileAttributesA(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringA("Settings", "AscendKey", "105", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "DescendKey", "104", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "ForwardKey", "16", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "AscendSpeed", "5.0", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "DescendSpeed", "-5.0", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "ForwardSpeed", "8.0", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "AscendRampUpMs", "300.0", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "AscendRampDownMs", "200.0", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "DescendRampUpMs", "300.0", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "DescendRampDownMs", "200.0", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "ForwardRampUpMs", "500.0", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "ForwardRampDownMs", "300.0", iniPath.c_str());
    }

    g_AscendKey = GetPrivateProfileIntA("Settings", "AscendKey", 105, iniPath.c_str());
    g_DescendKey = GetPrivateProfileIntA("Settings", "DescendKey", 104, iniPath.c_str());
    g_ForwardKey = GetPrivateProfileIntA("Settings", "ForwardKey", 16, iniPath.c_str());
    
    char buf[32];
    GetPrivateProfileStringA("Settings", "AscendSpeed", "5.0", buf, sizeof(buf), iniPath.c_str());
    g_AscendSpeed = std::stof(buf);
    
    GetPrivateProfileStringA("Settings", "DescendSpeed", "-5.0", buf, sizeof(buf), iniPath.c_str());
    g_DescendSpeed = std::stof(buf);

    GetPrivateProfileStringA("Settings", "ForwardSpeed", "8.0", buf, sizeof(buf), iniPath.c_str());
    g_ForwardMultiplier = std::stof(buf);

    GetPrivateProfileStringA("Settings", "AscendRampUpMs", "300.0", buf, sizeof(buf), iniPath.c_str());
    g_AscendRampUpMs = std::stof(buf);
    GetPrivateProfileStringA("Settings", "AscendRampDownMs", "200.0", buf, sizeof(buf), iniPath.c_str());
    g_AscendRampDownMs = std::stof(buf);

    GetPrivateProfileStringA("Settings", "DescendRampUpMs", "300.0", buf, sizeof(buf), iniPath.c_str());
    g_DescendRampUpMs = std::stof(buf);
    GetPrivateProfileStringA("Settings", "DescendRampDownMs", "200.0", buf, sizeof(buf), iniPath.c_str());
    g_DescendRampDownMs = std::stof(buf);

    GetPrivateProfileStringA("Settings", "ForwardRampUpMs", "500.0", buf, sizeof(buf), iniPath.c_str());
    g_ForwardRampUpMs = std::stof(buf);
    GetPrivateProfileStringA("Settings", "ForwardRampDownMs", "300.0", buf, sizeof(buf), iniPath.c_str());
    g_ForwardRampDownMs = std::stof(buf);

    char logBuf[256];
    sprintf_s(logBuf, "Config loaded: Fwd=%.1f, Asc=%.1f, Dsc=%.1f (INI Path: %s)", 
              g_ForwardMultiplier, g_AscendSpeed, g_DescendSpeed, iniPath.c_str());
    WriteLog(logBuf);
}

static uint8_t* FindHookAddress() {
    HMODULE hGame = GetModuleHandleA(nullptr);
    if (!hGame) return nullptr;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hGame;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t*)hGame + dos->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    uint8_t* base = (uint8_t*)hGame;
    for (DWORD i = 0; i < size - 10; i++) {
        if (base[i] == 0x41 && base[i+1] == 0x0F && base[i+2] == 0x58 && base[i+3] == 0x45 && base[i+4] == 0x00 &&
            base[i+5] == 0x41 && base[i+6] == 0x0F && base[i+7] == 0x11 && base[i+8] == 0x45 && base[i+9] == 0x00) {
            uint8_t* target = base + i - 8;
            if (target[0] == 0x0F && target[1] == 0x28 && target[2] == 0xC6 &&
                target[3] == 0xF3 && target[4] == 0x45 && target[5] == 0x0F && target[6] == 0x5C && target[7] == 0xC8) {
                return target;
            }
        }
    }
    return nullptr;
}

static bool InstallPatch() {
    g_patchAddr = FindHookAddress();
    if (!g_patchAddr) { return false; }

    uintptr_t base = reinterpret_cast<uintptr_t>(g_patchAddr) & ~static_cast<uintptr_t>(0xFFFF);
    for (uintptr_t off = 0; off < 0x70000000ull; off += 0x10000) {
        void* hi = VirtualAlloc(reinterpret_cast<void*>(base + off), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (hi) { g_trampoline = (uint8_t*)hi; break; }
        void* lo = VirtualAlloc(reinterpret_cast<void*>(base - off), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (lo) { g_trampoline = (uint8_t*)lo; break; }
    }
    if (!g_trampoline) return false;

    g_Shared = (SharedData*)(g_trampoline + 0x800);

    uint8_t* p = g_trampoline;

    // ORIGINAL BYTES
    *p++ = 0x0F; *p++ = 0x28; *p++ = 0xC6;                         
    *p++ = 0xF3; *p++ = 0x45; *p++ = 0x0F; *p++ = 0x5C; *p++ = 0xC8; 
    
    *p++ = 0x50; // push rax

    // IsGrounded check: We check [RBX+0x120] == 0.0f
    // Since jumps = 0.0f, and glide = 0.0f, we need to filter jumps.
    // Jump filter: check [RBX+0x118] > 0.6f (0x3F19999A). If it is, it's a glide (0.866). If it's <= 0.6 (0.5 on ground/jump), it skips.
    *p++ = 0x81; *p++ = 0xBB; 
    *reinterpret_cast<int32_t*>(p) = 0x118; p += 4;
    *reinterpret_cast<int32_t*>(p) = 0x3F19999A; p += 4;

    // JLE skip_boost (0x7E)
    *p++ = 0x7E; 
    uint8_t* jle_offset = p; 
    *p++ = 0x00; 

    // --- We RETURN to ADDING TO XMM0 but doing it without modifying Coordinates directly! ---
    // If setting RBX+0xC0 doesn't work, it means the physics engine IGNORES RBX+0xC0 changes made here,
    // OR it overwrites them immediately. But xmm0 is the position delta, which definitely works (because Ascend/Descend works).
    // So we MUST use AddVec on xmm0!
    // But we need g_AddVec[0] and g_AddVec[2] to be the calculated forward vector * speed.
    // The C++ thread calculates this using ForwardX and ForwardZ.

    // mov eax, [rbx+0x80] (Forward X)
    *p++ = 0x8B; *p++ = 0x83; 
    *reinterpret_cast<int32_t*>(p) = 0x80; p += 4;
    // mov [g_Shared->ForwardX], eax
    *p++ = 0x89; *p++ = 0x05;
    *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>((uint8_t*)&g_Shared->ForwardX - (p + 4)); p += 4;

    // mov eax, [rbx+0x88] (Forward Z)
    *p++ = 0x8B; *p++ = 0x83; 
    *reinterpret_cast<int32_t*>(p) = 0x88; p += 4;
    // mov [g_Shared->ForwardZ], eax
    *p++ = 0x89; *p++ = 0x05;
    *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>((uint8_t*)&g_Shared->ForwardZ - (p + 4)); p += 4;

    // apply AddVec to xmm0 (Ascend/Descend and Forward X/Z)
    *p++ = 0x48; *p++ = 0xB8; 
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_AddVec[0]); p += 8;
    *p++ = 0x0F; *p++ = 0x58; *p++ = 0x00; // addps xmm0, [rax]

    // skip_boost label
    *jle_offset = static_cast<uint8_t>(p - (jle_offset + 1));

    *p++ = 0x58; // pop rax

    *p++ = 0xE9; 
    int32_t relBack = static_cast<int32_t>((g_patchAddr + 8) - (p + 4));
    *reinterpret_cast<int32_t*>(p) = relBack;

    DWORD oldProt;
    VirtualProtect(g_patchAddr, 8, PAGE_EXECUTE_READWRITE, &oldProt);
    g_patchAddr[0] = 0xE9; 
    int32_t relFwd = static_cast<int32_t>(g_trampoline - (g_patchAddr + 5));
    *reinterpret_cast<int32_t*>(g_patchAddr + 1) = relFwd;
    g_patchAddr[5] = 0x90; g_patchAddr[6] = 0x90; g_patchAddr[7] = 0x90; 
    VirtualProtect(g_patchAddr, 8, oldProt, &oldProt);

    g_HookInstalled = true;
    return true;
}

static DWORD WINAPI KeyPollThread(LPVOID) {
    while (!g_HookInstalled) {
        if (IsGameForeground()) InstallPatch();
        Sleep(1000);
    }

    ULONGLONG lastTime = GetTickCount64();

    float currentForward = 0.0f;
    float currentAscend = 0.0f;
    float currentDescend = 0.0f;

    while (true) {
        ULONGLONG now = GetTickCount64();
        float dt = (float)(now - lastTime);
        lastTime = now;
        
        if (dt > 100.0f) dt = 100.0f; // Prevent huge jumps if thread stalled

        if (IsGameForeground() && g_Shared) {
            
            // Forward Ramp
            bool fwdDown = (GetAsyncKeyState(g_ForwardKey) & 0x8000) != 0;
            float targetForward = fwdDown ? g_ForwardMultiplier : 0.0f;
            
            if (currentForward < targetForward) {
                currentForward += g_ForwardMultiplier * (dt / g_ForwardRampUpMs);
                if (currentForward > targetForward) currentForward = targetForward;
            } else if (currentForward > targetForward) {
                currentForward -= g_ForwardMultiplier * (dt / g_ForwardRampDownMs);
                if (currentForward < targetForward) currentForward = targetForward;
            }

            g_AddVec[0] = g_Shared->ForwardX * (-currentForward * 0.05f);
            g_AddVec[2] = g_Shared->ForwardZ * (-currentForward * 0.05f);

            // Ascend Ramp
            bool ascDown = (GetAsyncKeyState(g_AscendKey) & 0x8000) != 0;
            float targetAscend = ascDown ? g_AscendSpeed : 0.0f;
            
            if (currentAscend < targetAscend) {
                currentAscend += g_AscendSpeed * (dt / g_AscendRampUpMs);
                if (currentAscend > targetAscend) currentAscend = targetAscend;
            } else if (currentAscend > targetAscend) {
                currentAscend -= g_AscendSpeed * (dt / g_AscendRampDownMs);
                if (currentAscend < targetAscend) currentAscend = targetAscend;
            }

            // Descend Ramp (g_DescendSpeed is negative)
            bool descDown = (GetAsyncKeyState(g_DescendKey) & 0x8000) != 0;
            float targetDescend = descDown ? g_DescendSpeed : 0.0f;
            
            if (currentDescend > targetDescend) { // going more negative
                currentDescend += g_DescendSpeed * (dt / g_DescendRampUpMs);
                if (currentDescend < targetDescend) currentDescend = targetDescend;
            } else if (currentDescend < targetDescend) { // going back to 0
                currentDescend -= g_DescendSpeed * (dt / g_DescendRampDownMs); 
                if (currentDescend > targetDescend) currentDescend = targetDescend;
            }

            g_AddVec[1] = currentAscend + currentDescend;
            
        } else {
            currentForward = 0.0f;
            currentAscend = 0.0f;
            currentDescend = 0.0f;
            g_AddVec[0] = 0.0f;
            g_AddVec[1] = 0.0f;
            g_AddVec[2] = 0.0f;
        }
        Sleep(10);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        DeleteFileA("CDFlight_Context.log");
        WriteLog("--- CDFlight Open Source v8.18 (Smooth Ramping Physics) ---");
        LoadConfig();
        CreateThread(nullptr, 0, KeyPollThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
