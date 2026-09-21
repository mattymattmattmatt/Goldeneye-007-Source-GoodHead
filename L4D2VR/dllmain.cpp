// dllmain.cpp : Defines the entry point for the DLL application.
#include <Windows.h>
#include <exception>
#include "game.h"
#pragma comment(lib, "shell32.lib")

static void BootLog(const char *msg)
{
    char path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, path))
        return;
    lstrcatA(path, "gesvr_boot.log");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[1024];
    wsprintfA(line, "%04d-%02d-%02d %02d:%02d:%02d [d3d9] %s\r\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    DWORD written = 0;
    WriteFile(h, line, (DWORD)lstrlenA(line), &written, nullptr);
    CloseHandle(h);

    char mod[MAX_PATH];
    if (GetModuleFileNameA(GetModuleHandleA("d3d9.dll"), mod, MAX_PATH))
    {
        char *slash = strrchr(mod, '\\');
        if (slash)
        {
            lstrcpyA(slash + 1, "vrmod_log.txt");
            HANDLE h2 = CreateFileA(mod, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h2 != INVALID_HANDLE_VALUE)
            {
                WriteFile(h2, line, (DWORD)lstrlenA(line), &written, nullptr);
                CloseHandle(h2);
            }
        }
    }
}

static void LoadSibling(HMODULE self, const wchar_t *name)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    lstrcpyW(slash + 1, name);
    HMODULE m = LoadLibraryW(path);
    char msg[MAX_PATH + 32];
    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);
    wsprintfA(msg, "LoadLibrary %s -> %s", narrow, m ? "OK" : "FAIL");
    BootLog(msg);
}

DWORD WINAPI InitGESVR(HMODULE hModule)
{
    // Two copies of d3d9.dll can map (bin\ and game root). Only one Game().
    HANDLE mutex = CreateMutexA(nullptr, TRUE, "Local\\GESVR_Init");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        BootLog("InitGESVR skipped (already running in this process)");
        if (mutex)
            CloseHandle(mutex);
        return 0;
    }

    Game::InitModPaths(hModule);
    Game::logMsg("InitGESVR thread start. cmdline: %s", GetCommandLineA());
    BootLog("InitGESVR thread running");

    Sleep(500);

    if (g_Game)
    {
        Game::logMsg("Game already constructed, skipping.");
        return 0;
    }

    BootLog("Waiting for client.dll");
    bool clientReady = false;
    for (int i = 0; i < 1200; ++i)
    {
        if (GetModuleHandleA("client.dll") && GetModuleHandleA("engine.dll"))
        {
            char ready[80];
            wsprintfA(ready, "client.dll ready after %d ms", i * 50);
            BootLog(ready);
            clientReady = true;
            break;
        }
        if ((i % 40) == 0)
        {
            char wait[80];
            wsprintfA(wait, "still waiting for client.dll try=%d", i);
            BootLog(wait);
        }
        Sleep(50);
    }
    if (!clientReady)
    {
        BootLog("client.dll never loaded; not constructing Game()");
        return 0;
    }

    try
    {
        g_Game = new Game();
    }
    catch (const std::exception &e)
    {
        Game::logMsg("Game() threw: %s", e.what());
        BootLog(e.what());
    }
    catch (...)
    {
        Game::logMsg("Game() threw an unknown exception");
        BootLog("Game() unknown exception");
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule,
                      DWORD ul_reason_for_call,
                      LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        BootLog("PROCESS_ATTACH");
        LoadSibling(hModule, L"openvr_api.dll");
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitGESVR, hModule, 0, NULL);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        // Do not VR_Shutdown here (loader lock). Just stop worker threads and
        // skip compositor calls so a SteamVR "Quit" cannot WaitGetPoses forever.
        GESVR_OnProcessDetach();
        break;
    }
    return TRUE;
}
