#include "game.h"
#include <Windows.h>
#include <iostream>
#include <unordered_map>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <mutex>
#include <initializer_list>

#include "sdk.h"
#include "vr.h"
#include "hooks.h"
#include "offsets.h"
#include "sigscanner.h"

static std::mutex logMutex;
static char g_ModDir[MAX_PATH] = {};
static char g_LogPath[MAX_PATH] = {};
using tCreateInterface = void* (__cdecl*)(const char* name, int* returnCode);

void Game::InitModPaths(HMODULE hModule)
{
    if (g_ModDir[0])
        return;

    char path[MAX_PATH] = {};
    if (!hModule)
        hModule = GetModuleHandleA("d3d9.dll");
    if (hModule)
        GetModuleFileNameA(hModule, path, MAX_PATH);
    if (!path[0])
        GetModuleFileNameA(nullptr, path, MAX_PATH);

    char *slash = strrchr(path, '\\');
    if (slash)
        *slash = '\0';

    strncpy_s(g_ModDir, path, _TRUNCATE);
    snprintf(g_LogPath, MAX_PATH, "%s\\vrmod_log.txt", g_ModDir);
}

const char *Game::ModDir()
{
    if (!g_ModDir[0])
        InitModPaths(nullptr);
    return g_ModDir;
}

// === Utility: Retry module load with logging ===
static HMODULE GetModuleWithRetry(const char* dllname, int maxTries = 1200, int delayMs = 50)
{
    for (int i = 0; i < maxTries; ++i)
    {
        HMODULE handle = GetModuleHandleA(dllname);
        if (handle)
            return handle;

        if ((i % 20) == 0)
            Game::logMsg("Waiting for module to load: %s (attempt %d)", dllname, i + 1);
        Sleep(delayMs);
    }

    Game::logMsg("[ERROR] Failed to load module after retrying: %s (no popup)", dllname);
    return nullptr;
}

// === Utility: Safe interface fetch with static cache ===
static void* GetInterfaceQuiet(const char* dllname, const char* interfacename)
{
    static std::unordered_map<std::string, void*> cache;

    std::string key = std::string(dllname) + "::" + interfacename;
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;

    HMODULE mod = GetModuleHandleA(dllname);
    if (!mod)
        return nullptr;

    auto CreateInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(mod, "CreateInterface"));
    if (!CreateInterface)
        return nullptr;

    int returnCode = 0;
    void* iface = CreateInterface(interfacename, &returnCode);
    if (!iface)
        return nullptr;

    cache[key] = iface;
    Game::logMsg("Interface %s::%s OK", dllname, interfacename);
    return iface;
}

static void* GetInterfaceSafe(const char* dllname, const char* interfacename)
{
    void* iface = GetInterfaceQuiet(dllname, interfacename);
    if (!iface)
        Game::logMsg("Interface not found: %s::%s", dllname, interfacename);
    return iface;
}

// === Game Constructor ===
Game::Game()
{
    Game::logMsg("Game() constructing. mod dir: %s", Game::ModDir());

    m_BaseClient = reinterpret_cast<uintptr_t>(GetModuleWithRetry("client.dll"));
    m_BaseEngine = reinterpret_cast<uintptr_t>(GetModuleWithRetry("engine.dll"));
    m_BaseMaterialSystem = reinterpret_cast<uintptr_t>(GetModuleWithRetry("MaterialSystem.dll"));
    m_BaseServer = reinterpret_cast<uintptr_t>(GetModuleWithRetry("server.dll"));
    m_BaseVgui2 = reinterpret_cast<uintptr_t>(GetModuleWithRetry("vgui2.dll"));

    auto tryIface = [](const char *dll, std::initializer_list<const char *> names) -> void * {
        for (const char *name : names)
        {
            void *iface = GetInterfaceSafe(dll, name);
            if (iface)
                return iface;
        }
        return nullptr;
    };

    m_ClientEntityList = static_cast<IClientEntityList*>(tryIface("client.dll", { "VClientEntityList003" }));
    m_EngineTrace = static_cast<IEngineTrace*>(tryIface("engine.dll", { "EngineTraceClient003", "EngineTraceClient004" }));
    m_EngineClient = static_cast<IEngineClient*>(tryIface("engine.dll", { "VEngineClient013", "VEngineClient014", "VEngineClient015" }));
    m_EngineVGui = static_cast<IEngineVGui*>(tryIface("engine.dll", { "VEngineVGui001" }));
    m_MaterialSystem = static_cast<IMaterialSystem*>(tryIface("MaterialSystem.dll", { "VMaterialSystem079", "VMaterialSystem080", "VMaterialSystem081" }));
    m_ModelInfo = static_cast<IModelInfo*>(tryIface("engine.dll", { "VModelInfoClient004", "VModelInfoClient003" }));
    m_ModelRender = static_cast<IModelRender*>(tryIface("engine.dll", { "VEngineModel016", "VEngineModel015" }));
    m_VguiInput = static_cast<IInput*>(tryIface("vgui2.dll", { "VGUI_InputInternal001" }));
    m_VguiSurface = static_cast<ISurface*>(tryIface("vguimatsurface.dll", { "VGUI_Surface031", "VGUI_Surface030" }));
    m_BaseClientDll = static_cast<IBaseClientDLL*>(tryIface("client.dll", { "VClient017", "VClient016", "VClient018", "VClient015" }));

    m_Offsets = new Offsets();
    m_VR = new VR(this);
    m_Hooks = new Hooks(this);

    m_Initialized = true;

    Game::logMsg("GoldenEye: Source VR initialized.");
}

// === Fallback Interface ===
void* Game::GetInterface(const char* dllname, const char* interfacename)
{
    Game::logMsg("Fallback GetInterface called for %s::%s", dllname, interfacename);
    return GetInterfaceSafe(dllname, interfacename);
}

// === Thread-safe Log Message with Timestamp ===
void Game::logMsg(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(logMutex);

    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    char timebuf[20] = {};
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now_c));

    printf("[%s] ", timebuf);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");

    if (!g_LogPath[0])
        InitModPaths(nullptr);

    // Both handles are opened once and kept open, flushed per line. This used
    // to fopen/fwrite/fclose TWO files on every call -- six file opens per
    // frame once per-frame tracing was on, all of it synchronous I/O on the
    // render thread inside PresentEx. Flushing keeps the log crash-complete
    // without paying for the open/close.
    static FILE *s_modLog = nullptr;
    static FILE *s_bootLog = nullptr;
    static bool s_opened = false;
    if (!s_opened)
    {
        s_opened = true;
        // Both files used to grow without end (16 MB after a few days of
        // testing). Past 8 MB at the start of a session the old one is kept
        // once, as *.old, and a fresh one started.
        auto rotate = [](const char *path) {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad) &&
                (fad.nFileSizeHigh != 0 || fad.nFileSizeLow > 8u * 1024u * 1024u))
            {
                char old[MAX_PATH];
                snprintf(old, sizeof(old), "%s.old", path);
                MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);
            }
        };
        const char *modPath = g_LogPath[0] ? g_LogPath : "vrmod_log.txt";
        rotate(modPath);
        s_modLog = fopen(modPath, "a");
        char tempLog[MAX_PATH] = {};
        if (GetTempPathA(MAX_PATH, tempLog))
        {
            strncat_s(tempLog, "gesvr_boot.log", _TRUNCATE);
            rotate(tempLog);
            s_bootLog = fopen(tempLog, "a");
        }
    }

    auto writeLog = [&](FILE *file) {
        if (!file)
            return;
        fprintf(file, "[%s] ", timebuf);
        va_list args2;
        va_start(args2, fmt);
        vfprintf(file, fmt, args2);
        va_end(args2);
        fprintf(file, "\n");
        fflush(file);
    };

    writeLog(s_modLog);
    writeLog(s_bootLog);
}

// === Error Message ===
void Game::errorMsg(const char* msg)
{
    logMsg("[ERROR] %s (no popup)", msg);
}

bool Game::IsGameUIVisible()
{
    if (!m_EngineVGui)
        return false;
    bool vis = false;
    __try
    {
        vis = m_EngineVGui->IsGameUIVisible();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vis = false;
    }
    return vis;
}

bool Game::IsInMap()
{
    if (!m_EngineClient)
        return false;
    bool ingame = false;
    __try
    {
        ingame = m_EngineClient->IsInGame();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ingame = false;
    }
    return ingame;
}

// === Entity Access ===
CBaseEntity* Game::GetClientEntity(int entityIndex)
{
    if (!m_ClientEntityList)
        return nullptr;

    return static_cast<CBaseEntity*>(m_ClientEntityList->GetClientEntity(entityIndex));
}

// === Network Name Utility ===
char* Game::getNetworkName(uintptr_t* entity)
{
    if (!entity)
        return nullptr;

    uintptr_t* vtable = reinterpret_cast<uintptr_t*>(*(entity + 0x8));
    if (!vtable)
        return nullptr;

    uintptr_t* getClientClassFn = reinterpret_cast<uintptr_t*>(*(vtable + 0x8));
    if (!getClientClassFn)
        return nullptr;

    uintptr_t* clientClass = reinterpret_cast<uintptr_t*>(*(getClientClassFn + 0x1));
    if (!clientClass)
        return nullptr;

    char* name = reinterpret_cast<char*>(*(clientClass + 0x8));
    int classID = static_cast<int>(*(clientClass + 0x10));

    Game::logMsg("[NetworkClass] ID: %d, Name: %s", classID, name ? name : "nullptr");
    return name;
}

// === Commands ===
void Game::ClientCmd(const char* szCmdString)
{
    if (m_EngineClient)
        m_EngineClient->ClientCmd(szCmdString);
}

void Game::ClientCmd_Unrestricted(const char* szCmdString)
{
    // SDK 2007's VEngineClient013 may not expose ClientCmd_Unrestricted at
    // the L4D2 vtable slot, so always go through ClientCmd.
    ClientCmd(szCmdString);
}

