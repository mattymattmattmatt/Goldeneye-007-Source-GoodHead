#include "viewport_clamp.h"
#include "sigscanner.h"
#include "game.h"

#include <Windows.h>
#include <cstdint>

namespace ViewportClamp
{
namespace
{
    // shaderapidx9.dll, CShaderAPIDx8::SetViewports, from the first clamp's
    // compare to the last clamp's store (rva 0x1A819..0x1A84A in SDK 2007).
    const char *kSignature =
        "3B F2 76 08 3B D5 7E 04 89 54 24 28 "        // cmp w,bbW / jbe / cmp bbW,0 / jle / mov w
        "3B D8 76 22 3B C5 7E 1E EB 18 "              // cmp h,bbH / jbe / cmp bbH,0 / jle / jmp
        "8B 87 5C 2B 00 00 3B F0 76 04 89 44 24 28 "  // other branch: w
        "8B 87 60 2B 00 00 3B D8 76 04 89 44 24 2C";  // other branch: h

    // Offsets of the four "jbe skip-the-clamp" opcodes within the signature.
    const int kJumpAt[4] = { 2, 14, 30, 44 };
    const uint8_t kJbe = 0x76;   // jump if below or equal: clamp only when bigger
    const uint8_t kJmp = 0xEB;   // always jump: never clamp

    int      s_state = 0;        // 0 unresolved, 1 ready, -1 unusable
    uint8_t *s_site = nullptr;
    bool     s_unclamped = false;
}

bool Available()
{
    if (s_state != 0)
        return s_state == 1;
    s_state = -1;

    HMODULE mod = GetModuleHandleA("shaderapidx9.dll");
    if (!mod)
    {
        s_state = 0;             // not loaded yet: try again next time
        return false;
    }
    const int off = SigScanner::VerifyOffset("shaderapidx9.dll", 0, kSignature, 0);
    if (off <= 0)
    {
        Game::logMsg("ViewportClamp: SetViewports clamp not found; eye targets stay limited to the window size");
        return false;
    }
    uint8_t *site = reinterpret_cast<uint8_t *>(mod) + off;
    for (int i = 0; i < 4; ++i)
    {
        if (site[kJumpAt[i]] != kJbe)
        {
            Game::logMsg("ViewportClamp: byte %d is 0x%02X, expected 0x76; not patching", i, site[kJumpAt[i]]);
            return false;
        }
    }

    // Writable once, for the life of the process, so each toggle is four plain
    // byte stores rather than four syscalls per frame.
    DWORD old = 0;
    if (!VirtualProtect(site, 48, PAGE_EXECUTE_READWRITE, &old))
    {
        Game::logMsg("ViewportClamp: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    s_site = site;
    s_state = 1;
    Game::logMsg("ViewportClamp: found SetViewports clamp at shaderapidx9+0x%X", (unsigned)off);
    return true;
}

void SetUnclamped(bool unclamped)
{
    if (unclamped == s_unclamped || !Available())
        return;
    const uint8_t op = unclamped ? kJmp : kJbe;
    for (int i = 0; i < 4; ++i)
        s_site[kJumpAt[i]] = op;
    FlushInstructionCache(GetCurrentProcess(), s_site, 48);
    s_unclamped = unclamped;
}
}
