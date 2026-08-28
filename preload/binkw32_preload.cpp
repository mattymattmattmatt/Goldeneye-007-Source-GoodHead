// Tiny binkw32.dll stand-in. Source loads this from bin\ (engine.dll import)
// long before shaderapidx9 asks for d3d9.dll. We LoadLibrary our d3d9 by
// absolute path so Windows will reuse it instead of SysWOW64\d3d9.dll.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

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
    char line[512];
    wsprintfA(line, "%04d-%02d-%02d %02d:%02d:%02d [bink] %s\r\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    DWORD written = 0;
    WriteFile(h, line, (DWORD)lstrlenA(line), &written, nullptr);
    CloseHandle(h);
}

static void TryLoad(const wchar_t *path)
{
    HMODULE mod = LoadLibraryW(path);
    char msg[MAX_PATH + 64];
    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);
    wsprintfA(msg, "%s -> %s", narrow, mod ? "OK" : "FAIL");
    BootLog(msg);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(hModule);
    BootLog("proxy attached");

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(hModule, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash)
        return TRUE;

    // Load the real Bink from this folder by absolute path so forwards resolve
    // even if Steam locked the process DLL search to System32.
    lstrcpyW(slash + 1, L"binkw32_orig.dll");
    TryLoad(path);

    // Only load ONE d3d9.dll. Prefer this folder (bin\), then parent of hl2.exe.
    if (!GetModuleHandleA("d3d9.dll"))
    {
        lstrcpyW(slash + 1, L"d3d9.dll");
        TryLoad(path);
    }
    if (!GetModuleHandleA("d3d9.dll"))
    {
        *slash = 0;
        wchar_t *parent = wcsrchr(path, L'\\');
        if (parent)
        {
            lstrcpyW(parent + 1, L"d3d9.dll");
            TryLoad(path);
        }
    }

    return TRUE;
}
