#define _CRT_SECURE_NO_WARNINGS
#include "pch.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

static const uint32_t kPatchRVA = 0x38524BC;
static const uint8_t  kExpected[5] = { 0x41, 0x0F, 0x11, 0x45, 0x00 };

static uint8_t* g_trampoline = nullptr;
static uint8_t* g_patchAddr  = nullptr;

alignas(16) static float g_BoostVec[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static volatile int32_t g_BoostActive = 0;

static uintptr_t g_rbxBuffer[1024];
static volatile int32_t g_rbxIndex = 0;
static uintptr_t g_PlayerContext = 0;

static int g_AscendKey = VK_NUMPAD9;
static int g_DescendKey = VK_NUMPAD8;
static int g_ForwardKey = VK_LSHIFT;
static float g_AscendSpeed = 4.0f;
static float g_DescendSpeed = -4.0f;
static float g_ForwardSpeed = 10.0f; 

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

static uintptr_t FindUserActorPtr() {
    HMODULE hGame = GetModuleHandleA(nullptr);
    if (!hGame) return 0;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hGame;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t*)hGame + dos->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    uint8_t* base = (uint8_t*)hGame;
    const char* sig = "\x48\x8B\x0D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x41\xB0\x01\x48\x8B\x53\x08";
    const char* mask = "xxx????x????xxxxxxx";
    size_t len = strlen(mask);
    for (DWORD i = 0; i < size - len; i++) {
        bool found = true;
        for (size_t j = 0; j < len; j++) {
            if (mask[j] != '?' && base[i + j] != (uint8_t)sig[j]) { found = false; break; }
        }
        if (found) {
            uint32_t rel = *(uint32_t*)(base + i + 3);
            return (uintptr_t)(base + i + 7 + rel);
        }
    }
    return 0;
}

static uintptr_t GetKliff() {
    static uintptr_t userActorPtr = 0;
    if (!userActorPtr) userActorPtr = FindUserActorPtr();
    if (!userActorPtr || IsBadReadPtr((void*)userActorPtr, 8)) return 0;
    
    __try {
        uintptr_t userActor = *reinterpret_cast<uintptr_t*>(userActorPtr);
        if (!userActor || IsBadReadPtr((void*)userActor, 0xE0)) return 0;
        
        uintptr_t kliff = *reinterpret_cast<uintptr_t*>(userActor + 0xD0);
        if (kliff == 0xFFFFFFFFFFFFFFFFull || kliff == 0) {
            kliff = *reinterpret_cast<uintptr_t*>(userActor + 0xD8);
        }
        if (kliff == 0xFFFFFFFFFFFFFFFFull) return 0;
        return kliff;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
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
    GetPrivateProfileStringA("Settings", "AscendSpeed", "4.0", buf, sizeof(buf), iniPath.c_str());
    g_AscendSpeed = std::stof(buf);
    
    GetPrivateProfileStringA("Settings", "DescendSpeed", "-4.0", buf, sizeof(buf), iniPath.c_str());
    g_DescendSpeed = std::stof(buf);

    GetPrivateProfileStringA("Settings", "ForwardSpeed", "10.0", buf, sizeof(buf), iniPath.c_str());
    g_ForwardSpeed = std::stof(buf);
}

static DWORD WINAPI KeyPollThread(LPVOID) {
    while (true) {
        if (IsGameForeground()) {
            bool active = false;
            float targetX = 0.0f;
            float targetY = 0.0f;
            float targetZ = 0.0f;

            if (GetAsyncKeyState(g_AscendKey) & 0x8000) {
                targetY = g_AscendSpeed;
                active = true;
            } else if (GetAsyncKeyState(g_DescendKey) & 0x8000) {
                targetY = g_DescendSpeed;
                active = true;
            }

            if (GetAsyncKeyState(g_ForwardKey) & 0x8000) {
                uintptr_t kliff = GetKliff();
                if (kliff) {
                    for (int i = 0; i < 1024; i++) {
                        uintptr_t ctx = g_rbxBuffer[i];
                        if (!ctx) continue;
                        __try {
                            bool found = false;
                            for (int j = 0; j < 0x300; j += 8) {
                                if (*(uintptr_t*)(ctx + j) == kliff) {
                                    found = true;
                                    break;
                                }
                            }
                            if (found) {
                                g_PlayerContext = ctx;
                                break;
                            }
                        } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
                    }
                }

                if (g_PlayerContext) {
                    __try {
                        float fwdX = *(float*)(g_PlayerContext + 0x7C);
                        float fwdZ = *(float*)(g_PlayerContext + 0x80);
                        targetX = -fwdX * g_ForwardSpeed;
                        targetZ = -fwdZ * g_ForwardSpeed;
                        active = true;
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        g_PlayerContext = 0; 
                    }
                }
            }

            if (active) {
                g_BoostVec[0] = targetX;
                g_BoostVec[1] = targetY;
                g_BoostVec[2] = targetZ;
                g_BoostActive = 1;
            } else {
                g_BoostActive = 0;
            }
        } else {
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

    // --- RING BUFFER CAPTURE ---
    *p++ = 0x9C; // pushfq
    *p++ = 0x50; // push rax
    *p++ = 0x51; // push rcx
    *p++ = 0x52; // push rdx

    *p++ = 0x48; *p++ = 0xB8; // mov rax, &g_rbxIndex
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_rbxIndex); p += 8;
    *p++ = 0x8B; *p++ = 0x08; // mov ecx, [rax]
    *p++ = 0x89; *p++ = 0xCA; // mov edx, ecx
    *p++ = 0x81; *p++ = 0xE2; *p++ = 0xFF; *p++ = 0x03; *p++ = 0x00; *p++ = 0x00; // and edx, 1023
    *p++ = 0xFF; *p++ = 0xC1; // inc ecx
    *p++ = 0x89; *p++ = 0x08; // mov [rax], ecx

    *p++ = 0x48; *p++ = 0xB8; // mov rax, &g_rbxBuffer
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_rbxBuffer[0]); p += 8;
    *p++ = 0x48; *p++ = 0x89; *p++ = 0x1C; *p++ = 0xD0; // mov [rax + rdx*8], rbx

    *p++ = 0x5A; // pop rdx
    *p++ = 0x59; // pop rcx
    *p++ = 0x58; // pop rax
    *p++ = 0x9D; // popfq
    // --- END RING BUFFER ---

    // Original instruction FIRST: movups [r13], xmm0
    // This allows the engine to write the delta position naturally!
    *p++ = 0x41; *p++ = 0x0F; *p++ = 0x11; *p++ = 0x45; *p++ = 0x00;

    // NOW WE CHECK IF WE NEED TO OVERRIDE THE NEWLY WRITTEN POSITION!
    // This completely bypasses modifying xmm0 and destroying flags or other math.
    *p++ = 0x9C; // pushfq
    *p++ = 0x50; // push rax

    *p++ = 0x48; *p++ = 0xB8; // mov rax, &g_BoostActive
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_BoostActive); p += 8;
    
    *p++ = 0x83; *p++ = 0x38; *p++ = 0x00; // cmp dword ptr [rax], 0
    *p++ = 0x74; *p++ = 0x16; // je skip (22 bytes forward)

    // Load original newly written XYZW from [r13] to xmm0
    *p++ = 0x41; *p++ = 0x0F; *p++ = 0x10; *p++ = 0x45; *p++ = 0x00; // movups xmm0, [r13]

    // Load boost vector
    *p++ = 0x48; *p++ = 0xB8; // mov rax, &g_BoostVec
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(&g_BoostVec[0]); p += 8;

    // ADD the boost directly to the position delta! (Safe addps, W is 0.0 so W remains unchanged)
    *p++ = 0x0F; *p++ = 0x58; *p++ = 0x00; // addps xmm0, [rax]

    // Write the boosted position back to [r13]
    *p++ = 0x41; *p++ = 0x0F; *p++ = 0x11; *p++ = 0x45; *p++ = 0x00; // movups [r13], xmm0

    // skip:
    *p++ = 0x58; // pop rax
    *p++ = 0x9D; // popfq

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
        DeleteFileA("CDFlight_Context.log");
        LoadConfig();
        CreateThread(nullptr, 0, KeyPollThread, nullptr, 0, nullptr);
        InstallPatch();
    }
    return TRUE;
}
