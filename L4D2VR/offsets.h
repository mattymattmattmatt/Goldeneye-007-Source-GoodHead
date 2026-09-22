#pragma once
#include "sigscanner.h"
#include "game.h"
#include <vector>

struct Offset
{
	std::string moduleName;
	int offset = 0;
	int address = 0;
	std::string signature;
	int sigOffset = 0;
	bool optional = false;
	bool found = false;

	Offset() = default;

	Offset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0, bool optional = false)
		: Offset(std::move(moduleName), currentOffset, std::vector<std::string>{ std::move(signature) }, sigOffset, optional)
	{
	}

	Offset(std::string moduleName, int currentOffset, std::vector<std::string> signatures, int sigOffset = 0, bool optional = false)
	{
		this->moduleName = std::move(moduleName);
		this->offset = currentOffset;
		this->sigOffset = sigOffset;
		this->optional = optional;
		this->signature = signatures.empty() ? std::string() : signatures.front();

		int resolved = -1;
		for (const auto &sig : signatures)
		{
			int newOffset = SigScanner::VerifyOffset(this->moduleName, currentOffset, sig, sigOffset);
			if (newOffset == 0)
			{
				resolved = this->offset;
				this->signature = sig;
				break;
			}
			if (newOffset > 0)
			{
				resolved = newOffset;
				this->signature = sig;
				break;
			}
		}

		if (resolved < 0)
		{
			if (!optional)
			{
				Game::logMsg("Required signature not found: %s (%s)", this->signature.c_str(), this->moduleName.c_str());
			}
			else
			{
				Game::logMsg("Optional signature not found: %s (%s)", this->signature.c_str(), this->moduleName.c_str());
			}
			this->found = false;
			this->address = 0;
			return;
		}

		this->offset = resolved;
		this->found = true;
		this->address = reinterpret_cast<uintptr_t>(GetModuleHandleA(this->moduleName.c_str())) + this->offset;
		Game::logMsg("Resolved %s!%s -> 0x%X", this->moduleName.c_str(), this->signature.substr(0, 24).c_str(), this->offset);
	}
};

// GoldenEye: Source ships on Source SDK Base 2007 (Orange Box).
// Signatures are listed with several candidates so a GE:S rebuild still resolves.
class Offsets
{
public:
	// ---- client.dll (GE:S game code) ----
	Offset RenderView = {
		"client.dll", 0,
		std::vector<std::string>{
			// GE:S CViewRender vtable[6], ret 12: 55 8B EC 81 EC 1C 01 00 00 53 57 8B F9
			"55 8B EC 81 EC 1C 01 00 00 53 57 8B F9 89 7D E4"
		},
		0, false
	};

	// CViewRender::Render(vrect_t*) — vtable[5], ret 4. Fires whenever a 3D view is drawn.
	Offset ViewRenderRender = {
		"client.dll", 0,
		std::vector<std::string>{
			"55 8B EC 81 EC 98 00 00 00 53 56 57 6A 04"
		},
		0, true
	};

	Offset g_pClientMode = {
		"client.dll", 0,
		std::vector<std::string>{
			"8B 0D ? ? ? ? 8B 01 5D FF 60",
			"8B 0D ? ? ? ? 8B 01 FF 50",
			"8B 0D ? ? ? ? 8B",
			"89 04 B5 ? ? ? ? E8"
		},
		2, false
	};

	Offset CalcViewModelView = {
		"client.dll", 0,
		std::vector<std::string>{
			// C_BaseViewModel::CalcViewModelView(owner, eyePos, eyeAngles) ret 12
			"55 8B EC 83 EC 18 F3 0F 10 05 ? ? ? ? 8B 45 10 53 8B 5D 0C 56 8B F1"
		},
		0, true
	};

	// C_BaseViewModel::FormatViewModelAttachment(int, matrix3x4_t&), vtable[183],
	// ret 8. Found by comparing C_BaseAnimating's vtable (an empty 'ret 8'
	// there) with C_BaseViewModel's: MatrixGetColumn / ::FormatViewModelAttachment
	// / MatrixSetColumn, exactly the SDK source. C_PredictedViewModel inherits it.
	Offset FormatViewModelAttachment = {
		"client.dll", 0,
		std::vector<std::string>{
			"55 8B EC 83 EC 0C 8D 45 F4 50 6A 03 FF 75 0C E8 ? ? ? ? 8D 45 F4 6A 00 50 E8"
		},
		0, true
	};

	Offset CreateMove = {
		"client.dll", 0,
		std::vector<std::string>{
			"55 8B EC 56 57 8B 7D 08 8B F1 8B 0D",
			"55 8B EC A1 ? ? ? ? 83 EC 0C 83 78 30 00 56 8B 75 0C 57 8B F9",
			"55 8B EC 83 EC 0C 53 56 8B 75 0C 57 8B F9"
		},
		0, true
	};

	Offset WriteUsercmd = {
		"client.dll", 0,
		std::vector<std::string>{
			"55 8B EC 8B 4D 08 8B 45 0C 53",
			"55 8B EC A1 ? ? ? ? 83 78 30 00 53 8B 5D 10 56 57",
			"55 8B EC A1 ? ? ? ? 83 78 30 00 53 8B 5D 0C 56 57"
		},
		0, true
	};

	Offset WriteUsercmdDeltaToBuffer = {
		"client.dll", 0,
		std::vector<std::string>{
			"55 8B EC 83 EC 60 0F 57 C0 8B 55 0C",
			"55 8B EC 83 EC ? 53 56 8B 75 10 8B D9"
		},
		0, true
	};

	Offset g_pppInput = {
		"client.dll", 0,
		std::vector<std::string>{
			"8B 0D ? ? ? ? 8B 01 8B 50 58 FF E2",
			"8B 0D ? ? ? ? 8B 01 8B 50 68 FF E2"
		},
		2, true
	};

	Offset AdjustEngineViewport = {
		"client.dll", 0,
		std::vector<std::string>{
			"55 8B EC 8B 0D ? ? ? ? 85 C9 74 17"
		},
		0, true
	};

	Offset PrePushRenderTarget = {
		"client.dll", 0,
		std::vector<std::string>{
			"55 8B EC 8B C1 56 8B 75 08 8B 0E 89 08 8B 56 04 89"
		},
		0, true
	};

	Offset DrawModelExecute = {
		"engine.dll", 0,
		std::vector<std::string>{
			"55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 56 8B 75 08 57 8B",
			"55 8B EC 83 EC 0C 53 8B 5D 08 56 57 8B F9"
		},
		0, true
	};

	Offset VGui_Paint = {
		"engine.dll", 0,
		std::vector<std::string>{
			"55 8B EC E8 ? ? ? ? 8B 10 8B C8 8B 52 38",
			"55 8B EC 83 EC 08 56 E8 ? ? ? ? 8B",
			"55 8B EC 8B 0D ? ? ? ? 85 C9 74 ? 8B 01",
			"55 8B EC 56 8B 75 08 83 E6 03"
		},
		0, true
	};

	// ---- server.dll (listen-server host, required for local MP) ----
	Offset ReadUserCmd = {
		"server.dll", 0,
		std::vector<std::string>{
			"55 8B EC 53 8B 5D 10 56 57 8B 7D 0C 53",
			"55 8B EC 53 8B 5D 10 8B 4D 08 56 57"
		},
		0, true
	};

	Offset ProcessUsercmds = {
		"server.dll", 0,
		std::vector<std::string>{
			"55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08",
			"55 8B EC 83 EC 08 53 8B 5D 08 56 57 8B F1",
			"55 8B EC B8 ? ? ? ? E8 ? ? ? ? 0F 57 C0 53 56 57"
		},
		0, true
	};

	Offset CBaseEntity_entindex = {
		"server.dll", 0,
		std::vector<std::string>{
			"8B 41 1C 85 C0 75 01 C3 8B 0D ? ? ? ? 2B 41 58 C1 F8 04 C3 CC",
			"8B 41 28 85 C0 75 01 C3 8B 0D ? ? ? ? 2B 41 58 C1 F8 04 C3 CC CC CC CC CC CC CC CC CC CC CC 55"
		},
		0, true
	};

	Offset EyePosition = {
		"server.dll", 0,
		std::vector<std::string>{
			"55 8B EC 56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8B 45 08 F3"
		},
		0, true
	};

	Offset Weapon_ShootPosition = {
		"server.dll", 0,
		std::vector<std::string>{
			"55 8B EC 8B 01 8B 90 ? ? ? ? 56 8B 75 08 56 FF D2 8B C6 5E 5D C2 04 00"
		},
		0, true
	};

	Offset Weapon_ShootPositionClient = {
		"client.dll", 0,
		std::vector<std::string>{
			"55 8B EC 8B 01 8B 90 ? ? ? ? 56 8B 75 08 56 FF D2 8B C6 5E 5D C2 04 00"
		},
		0, true
	};

	// ---- materialsystem.dll (SDK 2007, VMaterialSystem080) ----
	Offset GetRenderTarget = {
		"materialsystem.dll", 0,
		std::vector<std::string>{
			"83 79 4C 00",
			"8B 41 4C 85 C0"
		},
		0, true
	};

	Offset Viewport = {
		"materialsystem.dll", 0,
		std::vector<std::string>{
			"55 8B EC 83 EC 28 8B C1",
			"55 8B EC 8B 45 0C 53 8B 5D"
		},
		0, true
	};

	Offset GetViewport = {
		"materialsystem.dll", 0,
		std::vector<std::string>{
			"55 8B EC 8B 41 4C 8B 49 40 8D 04 C0 83 7C 81 ? ?"
		},
		0, true
	};

	Offset PushRenderTargetAndViewport = {
		"materialsystem.dll", 0,
		std::vector<std::string>{
			// SDK 2007 thiscall, 6 stack args, no frame pointer (ret 0x18)
			"83 EC 24 8B 44 24 28 8B 54 24 30 53 8B D9",
			"55 8B EC 83 EC 24 8B 45 08 8B 55 10 89",
			"55 8B EC 83 EC 24 8B 45 08 8B 4D 0C"
		},
		0, false
	};

	Offset PopRenderTargetAndViewport = {
		"materialsystem.dll", 0,
		std::vector<std::string>{
			// SDK 2007: cmp [ecx+4C],0 / je / add [ecx+4C],-1 / tail-call vtable
			"83 79 4C 00 74 0E 83 41 4C FF",
			"56 8B F1 83 7E 4C 00",
			"55 8B EC 56 8B F1 83 7E 4C 00"
		},
		0, false
	};

	// L4D2 leftovers kept optional so a future GE:S melee pass can reuse them
	Offset ClientFireTerrorBullets = { "client.dll", 0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10", 0, true };
	Offset ServerFireTerrorBullets = { "server.dll", 0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10", 0, true };
	Offset TestMeleeSwingClient = { "client.dll", 0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 53 56 8B 75 08 57 8B D9 E8 ? ? ? ? 8B", 0, true };
	Offset TestMeleeSwingServer = { "server.dll", 0, "24 FF D2 5B 5F 5E C3", 20, true };
	Offset DoMeleeSwingServer = { "server.dll", 0, "55 8B EC 83 EC 3C 53 56 8B F1 E8 ? ? ? ? 8B D8 85", 0, true };
	Offset StartMeleeSwingServer = { "server.dll", 0, "55 8B EC 53 56 8B F1 8B 86 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? 8B", 0, true };
	Offset PrimaryAttackServer = { "server.dll", 0, "56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 8B 87 ? ? ? ? 83 F8 FF", 0, true };
	Offset ItemPostFrameServer = { "server.dll", 0, "56 57 8B F1 E8 ? ? ? ? 8B CE E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 53", 0, true };
	Offset GetPrimaryAttackActivity = { "server.dll", 0, "55 8B EC 53 8B 5D 08 56 57 8B BB ? ? ? ?", 0, true };
	Offset GetActiveWeapon = { "server.dll", 0, "55 8B EC 8B 45 0C 56 8B 75 08 50 56 E8 ? ? ? ? 84 C0 74 47 8B", -64, true };
	Offset GetMeleeWeaponInfo = { "server.dll", 0, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3", 0, true };
	Offset GetMeleeWeaponInfoClient = { "client.dll", 0, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3", 0, true };
	Offset IsSplitScreen = { "client.dll", 0, "33 C0 83 3D ? ? ? ? ? 0F 9D C0", 0, true };
};
