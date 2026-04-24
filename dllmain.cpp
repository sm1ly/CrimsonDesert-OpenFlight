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
static float g_AscendMultiplier = 2.0f;    // Ускорение взлета (дельта Y * это)
static float g_DescendMultiplier = -1.5f;  // Ускорение падения (мягче, чтобы не улетать под текстуры)
static float g_ForwardMultiplier = 4.0f;   // Ускорение вперед (понижено с 8.0 для безопасности)

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

    g_AscendKey = GetPrivateProfileIntA("Settings", "AscendKey", VK_NUMPAD9, iniPath.c_str());
    g_DescendKey = GetPrivateProfileIntA("Settings", "DescendKey", VK_NUMPAD8, iniPath.c_str());
    g_ForwardKey = GetPrivateProfileIntA("Settings", "ForwardKey", VK_LSHIFT, iniPath.c_str());
    
    char buf[32];
    GetPrivateProfileStringA("Settings", "AscendSpeed", "2.0", buf, sizeof(buf), iniPath.c_str());
    g_AscendMultiplier = std::stof(buf);
    
    GetPrivateProfileStringA("Settings", "DescendSpeed", "-1.5", buf, sizeof(buf), iniPath.c_str());
    g_DescendMultiplier = std::stof(buf);

    GetPrivateProfileStringA("Settings", "ForwardSpeed", "4.0", buf, sizeof(buf), iniPath.c_str());
    g_ForwardMultiplier = std::stof(buf);
}

static uint8_t* FindHookAddress() {
    HMODULE hGame = GetModuleHandleA(nullptr);
    if (!hGame) return nullptr;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hGame;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t*)hGame + dos->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    uint8_t* base = (uint8_t*)hGame;

    // Ищем железный паттерн: addps xmm0, [r13]; movups [r13], xmm0
    // Это финальное сложение и запись. Мы отсчитаем от него НАЗАД 8 байт, чтобы найти movaps.
    const uint8_t pattern[] = { 
        0x41, 0x0F, 0x58, 0x45, 0x00, // addps xmm0, xmmword ptr [r13+0]
        0x41, 0x0F, 0x11, 0x45, 0x00  // movups xmmword ptr [r13+0], xmm0
    };
    const char* mask = "xxxxx+xxxx+"; // '+' значит точное совпадение, но мы юзаем 'x'
    
    for (DWORD i = 0; i < size - 10; i++) {
        if (base[i] == 0x41 && base[i+1] == 0x0F && base[i+2] == 0x58 && base[i+3] == 0x45 && base[i+4] == 0x00 &&
            base[i+5] == 0x41 && base[i+6] == 0x0F && base[i+7] == 0x11 && base[i+8] == 0x45 && base[i+9] == 0x00) {
            
            // Нашли! Теперь проверяем, что за 8 байт ДО этого лежит 'movaps xmm0, xmm6; subss xmm9, xmm8'
            uint8_t* target = base + i - 8;
            if (target[0] == 0x0F && target[1] == 0x28 && target[2] == 0xC6 &&
                target[3] == 0xF3 && target[4] == 0x45 && target[5] == 0x0F && target[6] == 0x5C && target[7] == 0xC8) {
                return target; // Это идеальная точка для хука (movaps)
            }
        }
    }
    return nullptr;
}

static bool InstallPatch() {
    g_patchAddr = FindHookAddress();
    
    if (!g_patchAddr) {
        WriteLog("ERROR: AOB Scan failed! Could not find physics DELTA instruction in memory.");
        return false;
    }

    char buf[256];
    sprintf_s(buf, "SUCCESS: AOB Scanner found hook DELTA address at %p", g_patchAddr);
    WriteLog(buf);

    uintptr_t base = reinterpret_cast<uintptr_t>(g_patchAddr) & ~static_cast<uintptr_t>(0xFFFF);
    const uintptr_t kWindow = 0x70000000ull; 
    
    for (uintptr_t off = 0; off < kWindow; off += 0x10000) {
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

    // --- DELTA MULTIPLIER HOOK ---
    // Оригинальные 8 байт:
    *p++ = 0x0F; *p++ = 0x28; *p++ = 0xC6;                         // movaps xmm0, xmm6
    *p++ = 0xF3; *p++ = 0x45; *p++ = 0x0F; *p++ = 0x5C; *p++ = 0xC8; // subss xmm9, xmm8
    
    *p++ = 0x50; // push rax

    // Умножаем ВЕСЬ вектор X,Y,Z (дельта скорости) на g_MultiplierVec
    // Это позволяет безопасно ускоряться вперед и вверх/вниз без телепортов
    *p++ = 0x48; *p++ = 0xB8; // mov rax, &g_MultiplierVec
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_MultiplierVec[0]); p += 8;
    *p++ = 0x0F; *p++ = 0x59; *p++ = 0x00; // mulps xmm0, [rax]

    // Аддитивная страховка (если нужно просто подтолкнуть чуть вверх при нулевой дельте Y)
    *p++ = 0x48; *p++ = 0xB8; // mov rax, &g_AddVec
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_AddVec[0]); p += 8;
    *p++ = 0x0F; *p++ = 0x58; *p++ = 0x00; // addps xmm0, [rax]

    *p++ = 0x58; // pop rax

    // Прыжок обратно к: addps xmm0, [r13]
    *p++ = 0xE9; // jmp rel32
    int32_t relBack = static_cast<int32_t>((g_patchAddr + 8) - (p + 4));
    *reinterpret_cast<int32_t*>(p) = relBack;

    // --- INJECT JUMP ---
    DWORD oldProt;
    VirtualProtect(g_patchAddr, 8, PAGE_EXECUTE_READWRITE, &oldProt);
    g_patchAddr[0] = 0xE9; // jmp
    int32_t relFwd = static_cast<int32_t>(g_trampoline - (g_patchAddr + 5));
    *reinterpret_cast<int32_t*>(g_patchAddr + 1) = relFwd;
    
    g_patchAddr[5] = 0x90; // nop
    g_patchAddr[6] = 0x90; // nop
    g_patchAddr[7] = 0x90; // nop
    
    VirtualProtect(g_patchAddr, 8, oldProt, &oldProt);

    WriteLog("SUCCESS: Trampoline hook installed completely on DELTA point.");
    g_HookInstalled = true;
    return true;
}

static DWORD WINAPI KeyPollThread(LPVOID) {
    // Ждем, пока игра загрузится в память (в обход пакеров/шифрования)
    while (!g_HookInstalled) {
        if (IsGameForeground()) {
            InstallPatch();
        }
        Sleep(1000);
    }

    while (true) {
        if (IsGameForeground()) {
            
            // X и Z (Горизонтальное перемещение)
            if (GetAsyncKeyState(g_ForwardKey) & 0x8000) {
                g_MultiplierVec[0] = g_ForwardMultiplier; // X
                g_MultiplierVec[2] = g_ForwardMultiplier; // Z
            } else {
                g_MultiplierVec[0] = 1.0f;
                g_MultiplierVec[2] = 1.0f;
            }

            // Y (Вертикальное перемещение)
            if (GetAsyncKeyState(g_AscendKey) & 0x8000) {
                g_MultiplierVec[1] = g_AscendMultiplier; // Умножаем дельту Y вверх
                g_AddVec[1] = 0.5f; // Легкий пинок, если дельта была нулевой
            } else if (GetAsyncKeyState(g_DescendKey) & 0x8000) {
                g_MultiplierVec[1] = g_DescendMultiplier; // Умножаем дельту Y вниз (отрицательно)
                g_AddVec[1] = -0.5f; // Пинок вниз
            } else {
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
        WriteLog("--- CDFlight Open Source v6.0 (Delayed AOB + Safe Y-Multiplier Edition) ---");
        LoadConfig();
        CreateThread(nullptr, 0, KeyPollThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
