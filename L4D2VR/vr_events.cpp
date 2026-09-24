#include "vr_events.h"
#include "vr.h"
#include "vr_watch.h"
#include <Windows.h>
#include "sdk.h"
#include "game.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

// Engine interfaces by raw vtable slot. Each slot was read off the code in
// this engine.dll (Source SDK Base 2007), and each function is checked against
// its first bytes before the first call, so a different engine build turns
// the kill feed off instead of calling the wrong thing:
//   IGameEventManager2 ("GAMEEVENTSMANAGER002", CGameEventManager):
//     3 AddListener(listener, name, serverSide)   ret 0Ch
//     4 FindListener(listener, name)              ret 8
//   IGameEventListener2: 0 destructor, 1 FireGameEvent(event) -- the manager
//     calls [vtable+4] with the event (CGameEventManager::FireEventIntern).
//   IGameEvent (CGameEvent, a KeyValues wrapper): 1 GetName, 6 GetInt(key,
//     def), 8 GetString(key, def).
//   IVEngineClient (CEngineClient): 8 GetPlayerInfo(ent, player_info_t *),
//     which copies 0x84 bytes, name first; 9 GetPlayerForUserID(userid).
namespace VREvents
{
static VR *g_vr = nullptr;

template <typename R, typename... A>
static R VCall(void *obj, int slot, A... a)
{
    typedef R(__thiscall * F)(void *, A...);
    return reinterpret_cast<F>((*reinterpret_cast<void ***>(obj))[slot])(obj, a...);
}

// Does the code at fn start with these bytes ("??" matches anything)?
static bool CodeMatches(const void *fn, const char *pattern)
{
    __try
    {
        const unsigned char *p = static_cast<const unsigned char *>(fn);
        for (const char *s = pattern; *s;)
        {
            if (*s == ' ')
            {
                ++s;
                continue;
            }
            if (s[0] == '?')
            {
                ++p;
                s += 2;
                continue;
            }
            char hex[3] = { s[0], s[1], 0 };
            if (*p != (unsigned char)strtoul(hex, nullptr, 16))
                return false;
            ++p;
            s += 2;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void *SlotFn(void *obj, int slot)
{
    __try
    {
        return (*reinterpret_cast<void ***>(obj))[slot];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Weapon names: GE:S puts the weapon's print name in the event ("#GE_KF7Soviet"),
// a token its HUD looks up in resource/gesource_english.txt. So do we.
// ---------------------------------------------------------------------------
static std::map<std::string, std::wstring> g_tokens;
static bool g_tokensLoaded = false;

static void LoadTokens()
{
    g_tokensLoaded = true;
    char path[MAX_PATH] = {};
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client || !GetModuleFileNameA(client, path, MAX_PATH))
        return;
    char *bin = strrchr(path, '\\');                       // ...\gesource\bin\client.dll
    if (bin) *bin = 0;
    bin = strrchr(path, '\\');                             // ...\gesource\bin
    if (bin) *bin = 0;
    strncat_s(path, "\\resource\\gesource_english.txt", _TRUNCATE);
    FILE *f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return;
    std::wstring text;
    wchar_t buf[4096];
    size_t n;
    while ((n = fread(buf, sizeof(wchar_t), 4096, f)) > 0)
        text.append(buf, n);
    fclose(f);
    // "token"  "value" pairs, one per line; comments and blocks are skipped.
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        size_t q1 = line.find(L'"'), q2 = q1 == std::wstring::npos ? q1 : line.find(L'"', q1 + 1);
        size_t q3 = q2 == std::wstring::npos ? q2 : line.find(L'"', q2 + 1);
        size_t q4 = q3 == std::wstring::npos ? q3 : line.find(L'"', q3 + 1);
        if (q4 == std::wstring::npos)
            continue;
        const std::wstring key = line.substr(q1 + 1, q2 - q1 - 1);
        if (key.compare(0, 3, L"GE_") != 0)
            continue;
        std::string k;
        for (wchar_t ch : key) k += (char)tolower(ch & 0x7F);
        g_tokens[k] = line.substr(q3 + 1, q4 - q3 - 1);
    }
    Game::logMsg("Kill feed: %d GE:S names loaded from %s", (int)g_tokens.size(), path);
}

static std::wstring Upper(std::wstring s)
{
    if (!s.empty())
        CharUpperBuffW(&s[0], (DWORD)s.size());
    return s;
}

static std::wstring WeaponDisplayName(const char *weapon)
{
    if (!weapon || !*weapon || !strcmp(weapon, "self") || !strcmp(weapon, "world"))
        return L"";
    if (!strncmp(weapon, "-TD-", 4))          // a trap's own death message
        return L"TRAP";
    std::string k = weapon[0] == '#' ? weapon + 1 : weapon;
    for (char &ch : k) ch = (char)tolower((unsigned char)ch);
    if (!g_tokensLoaded)
        LoadTokens();
    auto it = g_tokens.find(k);
    if (it != g_tokens.end())
        return Upper(it->second);
    // Not a token: tidy the raw name (weapon_pp7 -> PP7).
    if (!k.compare(0, 7, "weapon_")) k = k.substr(7);
    if (!k.compare(0, 3, "ge_")) k = k.substr(3);
    std::wstring out;
    for (char ch : k) out += (ch == '_') ? L' ' : (wchar_t)toupper((unsigned char)ch);
    return out;
}

// ---------------------------------------------------------------------------
// Player names
// ---------------------------------------------------------------------------
static int g_namesState = 0;   // 0 unchecked, 1 verified, -1 not this engine

static bool NamesUsable()
{
    if (g_namesState == 0)
    {
        g_namesState = -1;
        void *engine = g_vr && g_vr->m_Game ? g_vr->m_Game->m_EngineClient : nullptr;
        if (engine && CodeMatches(SlotFn(engine, 8), "8B 44 24 04 83 E8 01 3B 05") &&
            CodeMatches(SlotFn(engine, 9), "8B 0D ?? ?? ?? ?? 85 C9 75"))
            g_namesState = 1;
        Game::logMsg("Kill feed: engine player lookups %s",
                     g_namesState == 1 ? "verified (GetPlayerInfo 8, GetPlayerForUserID 9)" : "NOT recognised -- no names");
    }
    return g_namesState == 1;
}

// Entity index and raw name for a user ID; SEH-guarded, the engine's memory.
static bool LookupPlayer(void *engine, int userid, int &ent, char (&name)[33])
{
    __try
    {
        char info[0x84 + 64] = {};
        ent = VCall<int>(engine, 9, userid);
        if (ent <= 0)
            return false;
        if (!VCall<bool>(engine, 8, ent, (void *)info))
            return false;
        memcpy(name, info, 32);
        name[32] = 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct Player
{
    int ent = 0;
    std::wstring name;
};

static Player FindPlayer(int userid)
{
    Player p;
    if (userid <= 0 || !NamesUsable())
        return p;
    char raw[33] = {};
    if (!LookupPlayer(g_vr->m_Game->m_EngineClient, userid, p.ent, raw))
        return p;
    wchar_t wide[64] = {};
    MultiByteToWideChar(CP_UTF8, 0, raw, -1, wide, 64);
    p.name = Upper(wide);
    return p;
}

// ---------------------------------------------------------------------------
// The listener
// ---------------------------------------------------------------------------
struct EventData
{
    char name[32] = {};
    int userid = 0, attacker = 0, headshot = 0;
    char weapon[64] = {};
    int roundcount = 0, winnerid = 0, teamid = 0, isfinal = 0;
};

static bool ReadEvent(void *ev, EventData &d)
{
    __try
    {
        const char *n = VCall<const char *>(ev, 1);
        if (!n)
            return false;
        strncpy_s(d.name, n, _TRUNCATE);
        if (!strcmp(d.name, "player_death"))
        {
            d.userid = VCall<int>(ev, 6, "userid", 0);
            d.attacker = VCall<int>(ev, 6, "attacker", 0);
            d.headshot = VCall<int>(ev, 6, "headshot", 0);
            const char *w = VCall<const char *>(ev, 8, "weapon", "");
            strncpy_s(d.weapon, w ? w : "", _TRUNCATE);
        }
        else if (!strcmp(d.name, "round_start"))
        {
            d.roundcount = VCall<int>(ev, 6, "roundcount", 0);
        }
        else if (!strcmp(d.name, "round_end"))
        {
            d.roundcount = VCall<int>(ev, 6, "roundcount", 0);
            d.winnerid = VCall<int>(ev, 6, "winnerid", 0);
            d.teamid = VCall<int>(ev, 6, "teamid", 0);
            d.isfinal = VCall<int>(ev, 6, "isfinal", 0);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static ULONGLONG g_youUntil = 0;   // a notice about you is showing: others wait

static void OnEvent(const EventData &d)
{
    if (!g_vr || !g_vr->m_Game || !g_vr->m_Game->m_EngineClient)
        return;
    const int local = g_vr->m_Game->m_EngineClient->GetLocalPlayer();
    const ULONGLONG now = GetTickCount64();

    if (!strcmp(d.name, "player_death"))
    {
        const Player victim = FindPlayer(d.userid);
        const Player killer = FindPlayer(d.attacker);
        const std::wstring weapon = WeaponDisplayName(d.weapon);
        const bool suicide = d.attacker == 0 || d.attacker == d.userid || !strcmp(d.weapon, "self");
        if (victim.ent == local && local > 0)
        {
            if (suicide || killer.name.empty())
                VRWatch::Notify(L"YOU DIED", weapon, 2, 4000, true);
            else
                VRWatch::Notify(L"KILLED BY", killer.name, 2, 4000, true);
            g_youUntil = now + 2500;
        }
        else if (killer.ent == local && local > 0 && !suicide)
        {
            VRWatch::Notify(d.headshot ? L"HEADSHOT!" : L"YOU KILLED", victim.name, 1, 4000, true);
            g_youUntil = now + 2500;
        }
        else if (now >= g_youUntil && !victim.name.empty())
        {
            if (suicide || killer.name.empty())
                VRWatch::Notify(victim.name, weapon.empty() ? L"DIED" : weapon, 0, 3000, false);
            else
                VRWatch::Notify(killer.name, L"KILLED " + victim.name, 0, 3000, false);
        }
    }
    else if (!strcmp(d.name, "round_start"))
    {
        wchar_t head[32];
        swprintf(head, 32, d.roundcount > 0 ? L"ROUND %d" : L"NEW ROUND", d.roundcount);
        VRWatch::Notify(head, L"GO!", 0, 3500, false);
    }
    else if (!strcmp(d.name, "round_end"))
    {
        std::wstring detail;
        int kind = 0;
        if (d.teamid == 2)
            detail = L"MI6 WIN";
        else if (d.teamid == 3)
            detail = L"JANUS WIN";
        else if (d.winnerid > 0)
        {
            const Player w = FindPlayer(d.winnerid);
            if (w.ent == local && local > 0)
            {
                detail = L"YOU WIN";
                kind = 1;
            }
            else if (!w.name.empty())
                detail = w.name + L" WINS";
        }
        VRWatch::Notify(d.isfinal ? L"MATCH OVER" : L"ROUND OVER", detail, kind, 5000, true);
    }

    static int s_logged = 0;
    if (s_logged < 40)
    {
        ++s_logged;
        Game::logMsg("Kill feed event %s user=%d attacker=%d weapon='%s' round=%d winner=%d team=%d final=%d",
                     d.name, d.userid, d.attacker, d.weapon, d.roundcount, d.winnerid, d.teamid, d.isfinal);
    }
}

class WatchListener
{
public:
    virtual ~WatchListener() {}
    virtual void FireGameEvent(void *event)   // IGameEventListener2 slot 1
    {
        if (!event || !g_vr || !g_vr->m_WatchKillFeed)
            return;
        EventData d;
        if (ReadEvent(event, d))
            OnEvent(d);
    }
};
static WatchListener g_listener;

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
static void *g_manager = nullptr;
static int g_managerState = 0;   // 0 unchecked, 1 verified, -1 unusable
static bool g_registered = false;
static const char *kEvents[] = { "player_death", "round_start", "round_end" };

static bool Manager()
{
    if (g_managerState != 0)
        return g_managerState == 1;
    g_managerState = -1;
    typedef void *(*CreateInterfaceFn)(const char *, int *);
    HMODULE engine = GetModuleHandleA("engine.dll");
    auto create = engine ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")) : nullptr;
    void *mgr = create ? create("GAMEEVENTSMANAGER002", nullptr) : nullptr;
    if (mgr && CodeMatches(SlotFn(mgr, 3), "56 57 8B 7C 24 10 85 FF 8B F1 74") &&
        CodeMatches(SlotFn(mgr, 4), "8B 44 24 08 57 50 8B F9 E8"))
    {
        g_manager = mgr;
        g_managerState = 1;
    }
    Game::logMsg("Kill feed: game event manager %s", g_managerState == 1 ? "verified (AddListener 3, FindListener 4)"
                                                                         : "NOT recognised -- no watch kill feed");
    return g_managerState == 1;
}

static bool AddListeners(void *mgr, int &added)
{
    __try
    {
        added = 0;
        for (const char *e : kEvents)
            if (VCall<bool>(mgr, 3, (void *)&g_listener, e, false))
                ++added;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool StillListening(void *mgr)
{
    __try
    {
        return VCall<bool>(mgr, 4, (void *)&g_listener, "player_death");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void Update(VR *vr)
{
    g_vr = vr;
    if (!vr || !vr->m_WatchKillFeed || !vr->m_ShowWristHUD)
        return;
    static ULONGLONG s_next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < s_next)
        return;
    s_next = now + 2000;
    if (!Manager())
        return;
    if (g_registered && StillListening(g_manager))
        return;
    int added = 0;
    if (!AddListeners(g_manager, added))
    {
        g_managerState = -1;
        Game::logMsg("Kill feed: AddListener faulted -- kill feed off");
        return;
    }
    g_registered = added > 0;
    static int s_logged = 0;
    if (s_logged < 6)
    {
        ++s_logged;
        Game::logMsg("Kill feed: listening to %d of %d game events", added, (int)(sizeof(kEvents) / sizeof(kEvents[0])));
    }
}
} // namespace VREvents
