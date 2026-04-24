#define _CRT_SECURE_NO_WARNINGS
#include "pch.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

static uint8_t* g_trampoline = nullptr;
static uint8_t* g_patchAddr  = nullptr;

// Vector: (X, Y, Z, W)
alignas(16) static float g_MultiplierVec[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
alignas(16) static float g_AddVec[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

static int g_AscendKey = VK_NUMPAD9;
static int g_DescendKey = VK_NUMPAD8;
static int g_ForwardKey = VK_LSHIFT;

// Настройки скоростей (ИНИ)
static float g_AscendSpeed = 5.0f;     
static float g_DescendSpeed = -5.0f;   
static float g_ForwardMultiplier = 4.0f; 

static bool g_HookInstalled = false;

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

    // Если INI файла нет - создаем его с дефолтными значениями
    if (GetFileAttributesA(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringA("Settings", "AscendKey", "105", iniPath.c_str());   // Numpad 9
        WritePrivateProfileStringA("Settings", "DescendKey", "104", iniPath.c_str());  // Numpad 8
        WritePrivateProfileStringA("Settings", "ForwardKey", "16", iniPath.c_str());   // Shift
        WritePrivateProfileStringA("Settings", "AscendSpeed", "5.0", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "DescendSpeed", "-5.0", iniPath.c_str());
        WritePrivateProfileStringA("Settings", "ForwardSpeed", "4.0", iniPath.c_str());
        WriteLog("Created default CDFlight.ini");
    }

    g_AscendKey = GetPrivateProfileIntA("Settings", "AscendKey", 105, iniPath.c_str());
    g_DescendKey = GetPrivateProfileIntA("Settings", "DescendKey", 104, iniPath.c_str());
    g_ForwardKey = GetPrivateProfileIntA("Settings", "ForwardKey", 16, iniPath.c_str());
    
    char buf[32];
    GetPrivateProfileStringA("Settings", "AscendSpeed", "5.0", buf, sizeof(buf), iniPath.c_str());
    g_AscendSpeed = std::stof(buf);
    
    GetPrivateProfileStringA("Settings", "DescendSpeed", "-5.0", buf, sizeof(buf), iniPath.c_str());
    g_DescendSpeed = std::stof(buf);

    GetPrivateProfileStringA("Settings", "ForwardSpeed", "4.0", buf, sizeof(buf), iniPath.c_str());
    g_ForwardMultiplier = std::stof(buf);

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

    const uint8_t pattern[] = { 
        0x41, 0x0F, 0x58, 0x45, 0x00, 
        0x41, 0x0F, 0x11, 0x45, 0x00  
    };
    
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
    
    if (!g_patchAddr) {
        WriteLog("ERROR: AOB Scan failed! Could not find physics DELTA instruction.");
        return false;
    }

    char buf[256];
    sprintf_s(buf, "SUCCESS: Hook DELTA address found at %p", g_patchAddr);
    WriteLog(buf);

    uintptr_t base = reinterpret_cast<uintptr_t>(g_patchAddr) & ~static_cast<uintptr_t>(0xFFFF);
    const uintptr_t kWindow = 0x70000000ull; 
    
    for (uintptr_t off = 0; off < kWindow; off += 0x10000) {
        void* hi = VirtualAlloc(reinterpret_cast<void*>(base + off), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (hi) { g_trampoline = (uint8_t*)hi; break; }
        void* lo = VirtualAlloc(reinterpret_cast<void*>(base - off), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (lo) { g_trampoline = (uint8_t*)lo; break; }
    }
    
    if (!g_trampoline) return false;

    uint8_t* p = g_trampoline;

    *p++ = 0x0F; *p++ = 0x28; *p++ = 0xC6;                         
    *p++ = 0xF3; *p++ = 0x45; *p++ = 0x0F; *p++ = 0x5C; *p++ = 0xC8; 
    
    *p++ = 0x50; 

    *p++ = 0x48; *p++ = 0xB8; 
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_MultiplierVec[0]); p += 8;
    *p++ = 0x0F; *p++ = 0x59; *p++ = 0x00; 

    *p++ = 0x48; *p++ = 0xB8; 
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_AddVec[0]); p += 8;
    *p++ = 0x0F; *p++ = 0x58; *p++ = 0x00; 

    *p++ = 0x58; 

    *p++ = 0xE9; 
    int32_t relBack = static_cast<int32_t>((g_patchAddr + 8) - (p + 4));
    *reinterpret_cast<int32_t*>(p) = relBack;

    DWORD oldProt;
    VirtualProtect(g_patchAddr, 8, PAGE_EXECUTE_READWRITE, &oldProt);
    g_patchAddr[0] = 0xE9; 
    int32_t relFwd = static_cast<int32_t>(g_trampoline - (g_patchAddr + 5));
    *reinterpret_cast<int32_t*>(g_patchAddr + 1) = relFwd;
    
    g_patchAddr[5] = 0x90; 
    g_patchAddr[6] = 0x90; 
    g_patchAddr[7] = 0x90; 
    
    VirtualProtect(g_patchAddr, 8, oldProt, &oldProt);

    g_HookInstalled = true;
    return true;
}

static DWORD WINAPI KeyPollThread(LPVOID) {
    while (!g_HookInstalled) {
        if (IsGameForeground()) InstallPatch();
        Sleep(1000);
    }

    while (true) {
        if (IsGameForeground()) {
            
            // --- ГОРИЗОНТАЛЬ (Shift) ---
            if (GetAsyncKeyState(g_ForwardKey) & 0x8000) {
                g_MultiplierVec[0] = g_ForwardMultiplier; // X
                g_MultiplierVec[2] = g_ForwardMultiplier; // Z
            } else {
                g_MultiplierVec[0] = 1.0f;
                g_MultiplierVec[2] = 1.0f;
            }

            // --- ВЕРТИКАЛЬ (Взлет / Спуск) ---
            // ГЕНИАЛЬНЫЙ МУВ: если мы хотим лететь вверх/вниз, 
            // мы ставим множитель Y в 0.0f (отключаем гравитацию)
            // и прибавляем чистую скорость полета!
            if (GetAsyncKeyState(g_AscendKey) & 0x8000) {
                g_MultiplierVec[1] = 0.0f; // Отключаем гравитацию
                g_AddVec[1] = g_AscendSpeed; // Летим четко вверх
            } else if (GetAsyncKeyState(g_DescendKey) & 0x8000) {
                g_MultiplierVec[1] = 0.0f; // Отключаем гравитацию
                g_AddVec[1] = g_DescendSpeed; // Летим четко вниз
            } else {
                // Ничего не нажато - возвращаем обычную физику
                g_MultiplierVec[1] = 1.0f; 
                g_AddVec[1] = 0.0f;
            }

        } else {
            g_MultiplierVec[0] = 1.0f;
            g_MultiplierVec[1] = 1.0f;
            g_MultiplierVec[2] = 1.0f;
            g_AddVec[1] = 0.0f;
        }
        Sleep(10);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        DeleteFileA("CDFlight_Context.log");
        WriteLog("--- CDFlight Open Source v8.0 (Anti-Gravity Edition) ---");
        LoadConfig();
        CreateThread(nullptr, 0, KeyPollThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
