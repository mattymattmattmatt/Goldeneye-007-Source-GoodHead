#pragma once

#include <cstdint>
#include <array>
#include <string>
#include "vrnet.h"
#include <cstdarg>
#include <Windows.h>

#include "vector.h"

// === Forward Declarations for Engine Interfaces ===
class IClientEntityList;
class IEngineTrace;
class IEngineClient;
class IEngineVGui;
class IMaterialSystem;
class IBaseClientDLL;
class IModelInfo;
class IModelRender;
class IMaterial;
class IInput;
class ISurface;
class CBaseEntity;
class C_BasePlayer;
struct model_t;

// === Forward Declarations for Internal Systems ===
class Game;
class Offsets;
class VR;
class Hooks;

// === Global Game Instance ===
inline Game* g_Game = nullptr;

// === Per-Player VR State ===
struct Player
{
    C_BasePlayer* pPlayer = nullptr;
    bool isUsingVR = false;

    Vector controllerPos = { 0.f, 0.f, 0.f };
    QAngle controllerAngle = { 0.f, 0.f, 0.f };
    QAngle prevControllerAngle = { 0.f, 0.f, 0.f };

    bool isMeleeing = false;
    bool isNewSwing = false;
};

// === Main Game System ===
class Game
{
public:
    // === Engine Interfaces ===
    IClientEntityList* m_ClientEntityList = nullptr;
    IEngineTrace* m_EngineTrace = nullptr;
    IEngineClient* m_EngineClient = nullptr;
    IEngineVGui* m_EngineVGui = nullptr;
    IMaterialSystem* m_MaterialSystem = nullptr;
    IBaseClientDLL* m_BaseClientDll = nullptr;
    IModelInfo* m_ModelInfo = nullptr;
    IModelRender* m_ModelRender = nullptr;
    IInput* m_VguiInput = nullptr;
    ISurface* m_VguiSurface = nullptr;

    // === Module Base Addresses ===
    uintptr_t m_BaseEngine = 0;
    uintptr_t m_BaseClient = 0;
    uintptr_t m_BaseServer = 0;
    uintptr_t m_BaseMaterialSystem = 0;
    uintptr_t m_BaseVgui2 = 0;

    // === Internal Systems ===
    Offsets* m_Offsets = nullptr;
    VR* m_VR = nullptr;
    Hooks* m_Hooks = nullptr;

    // === State Flags ===
    bool m_Initialized = false;
    bool m_PerformingMelee = false;
    int m_CurrentUsercmdID = -1;

    // === Player VR State (listen-server / local MP) ===
    std::array<Player, VRNet::kMaxPlayers> m_PlayersVRInfo;
    bool m_EnableNetVR = true;

    // === Weapon / Viewmodel State ===
    bool m_IsMeleeWeaponActive = false;
    bool m_SwitchedWeapons = false;
    model_t* m_ArmsModel = nullptr;
    IMaterial* m_ArmsMaterial = nullptr;
    bool m_CachedArmsModel = false;
    std::string m_ActiveWeaponModel;

    // === Constructor ===
    Game();

    // === Interface Utilities ===
    void* GetInterface(const char* dllname, const char* interfacename);
    CBaseEntity* GetClientEntity(int entityIndex);
    char* getNetworkName(uintptr_t* entity);

    // === Command Execution ===
    void ClientCmd(const char* szCmdString);
    void ClientCmd_Unrestricted(const char* szCmdString);

    bool IsGameUIVisible();
    bool IsInMap();

    // === Logging ===
    static void InitModPaths(HMODULE hModule);
    static const char* ModDir();
    static void logMsg(const char* fmt, ...);
    static void errorMsg(const char* msg);
};

// OpenVR helpers used from DXVK Present/CreateDevice so Theater dies even if Game() is late.
void GESVR_ClaimSteamVRScene();
void GESVR_SubmitBackBufferFallback();
void GESVR_OnProcessDetach();

// === Logging Macros (Debug Only) ===
#ifdef _DEBUG
#define LOG(fmt, ...) Game::logMsg("[LOG] " fmt, ##__VA_ARGS__)
#define ERR(msg) Game::errorMsg("[ERROR] " msg)
#else
#define LOG(fmt, ...)
#define ERR(msg)
#endif
