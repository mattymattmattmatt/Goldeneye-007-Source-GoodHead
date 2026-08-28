#include "weapons.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>

static std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

static bool Contains(const std::string &hay, const char *needle)
{
	return hay.find(needle) != std::string::npos;
}

PositionAngle Weapons::GetOffset(const std::string &modelName)
{
	const std::string name = ToLower(modelName);

	// { forward, right, up }, { pitch, yaw, roll }
	static const std::vector<std::pair<const char *, PositionAngle>> kTable = {
		// Pistols / dual-wield staples
		{ "pp7_silenced", {{ 18.0f,  4.0f, -3.0f }, { -2.0f, 0.0f, 0.0f }} },
		{ "pp7",          {{ 18.0f,  4.0f, -3.0f }, { -2.0f, 0.0f, 0.0f }} },
		{ "dd44",         {{ 18.5f,  4.0f, -3.0f }, { -2.0f, 0.0f, 0.0f }} },
		{ "klobb",        {{ 17.5f,  4.5f, -3.5f }, { -1.5f, 0.0f, 0.0f }} },
		{ "goldengun",    {{ 19.0f,  4.0f, -3.0f }, { -1.0f, 0.0f, 0.0f }} },
		{ "cougar",       {{ 19.0f,  4.5f, -3.0f }, { -1.5f, 0.0f, 0.0f }} },
		{ "magnum",       {{ 19.0f,  4.5f, -3.0f }, { -1.5f, 0.0f, 0.0f }} },

		// SMGs
		{ "zmg",          {{ 16.5f,  5.0f, -4.0f }, { -1.0f, 0.0f, 0.0f }} },
		{ "d5k_silenced", {{ 16.5f,  5.0f, -4.0f }, { -1.0f, 0.0f, 0.0f }} },
		{ "d5k",          {{ 16.5f,  5.0f, -4.0f }, { -1.0f, 0.0f, 0.0f }} },
		{ "phantom",      {{ 16.0f,  5.0f, -4.5f }, { -1.0f, 0.0f, 0.0f }} },
		{ "kf7",          {{ 17.0f,  5.5f, -4.5f }, { -0.5f, 0.0f, 0.0f }} },
		{ "soviet",       {{ 17.0f,  5.5f, -4.5f }, { -0.5f, 0.0f, 0.0f }} },

		// Rifles
		{ "ar33",         {{ 17.5f,  5.5f, -5.0f }, { -0.5f, 0.0f, 0.0f }} },
		{ "rcp90",        {{ 16.5f,  5.5f, -5.0f }, { -0.5f, 0.0f, 0.0f }} },
		{ "rcp-90",       {{ 16.5f,  5.5f, -5.0f }, { -0.5f, 0.0f, 0.0f }} },
		{ "sniper",       {{ 18.5f,  5.0f, -5.5f }, {  0.0f,-1.5f, 0.0f }} },

		// Shotguns
		{ "autoshotgun",  {{ 15.5f,  5.0f, -4.5f }, { -1.5f,-1.0f, 0.0f }} },
		{ "shotgun",      {{ 15.5f,  5.0f, -4.5f }, { -1.5f,-1.0f, 0.0f }} },

		// Heavy / gadgets
		{ "rocket",       {{ 14.0f,  5.0f, -3.5f }, { -1.0f, 0.0f, 0.0f }} },
		{ "grenade_l",    {{ 14.0f,  5.0f, -3.0f }, { -1.0f, 0.0f, 0.0f }} },
		{ "grenadelauncher", {{ 14.0f, 5.0f, -3.0f }, { -1.0f, 0.0f, 0.0f }} },
		{ "moonraker",    {{ 16.0f,  4.5f, -3.5f }, { -1.0f, 0.0f, 0.0f }} },
		{ "laser",        {{ 17.0f,  4.0f, -3.0f }, { -1.0f, 0.0f, 0.0f }} },

		// Melee
		{ "throwing",     {{ 22.0f,  6.0f, -2.5f }, {-20.0f,-10.0f,-20.0f }} },
		{ "knife",        {{ 22.0f,  6.0f, -2.5f }, {-20.0f,-10.0f,-20.0f }} },
		{ "slapper",      {{ 14.0f,  5.0f, -8.0f }, {-30.0f, -8.0f,-25.0f }} },
		{ "fist",         {{ 14.0f,  5.0f, -8.0f }, {-30.0f, -8.0f,-25.0f }} },

		// Thrown
		{ "grenade",      {{ 16.0f,  5.0f, -4.0f }, { -8.0f, 0.0f, 0.0f }} },
		{ "mine",         {{ 16.0f,  5.0f, -4.0f }, { -8.0f, 0.0f, 0.0f }} },
		{ "detonator",    {{ 14.0f,  4.0f, -3.0f }, { -4.0f, 0.0f, 0.0f }} },
		{ "watch",        {{  8.0f,  3.0f, -6.0f }, {  0.0f, 0.0f, 0.0f }} },
	};

	for (const auto &entry : kTable)
	{
		if (Contains(name, entry.first))
			return entry.second;
	}

	// Generic N64 pistol-ish default
	return { { 18.0f, 4.5f, -3.5f }, { -1.5f, 0.0f, 0.0f } };
}

bool Weapons::IsMelee(const std::string &modelName)
{
	const std::string name = ToLower(modelName);
	return Contains(name, "knife") || Contains(name, "slapper") || Contains(name, "fist") || Contains(name, "throwing");
}

bool Weapons::IsDualWieldable(const std::string &modelName)
{
	const std::string name = ToLower(modelName);
	static const char *kDual[] = {
		"pp7", "dd44", "klobb", "zmg", "d5k", "phantom", "goldengun", "cougar", "magnum"
	};
	for (auto *key : kDual)
	{
		if (Contains(name, key))
			return true;
	}
	return false;
}

bool Weapons::IsThrowable(const std::string &modelName)
{
	const std::string name = ToLower(modelName);
	return Contains(name, "grenade") || Contains(name, "mine") || Contains(name, "throwing");
}

bool Weapons::IsWatch(const std::string &modelName)
{
	return Contains(ToLower(modelName), "watch");
}

bool Weapons::IsSniper(const std::string &modelName)
{
	const std::string name = ToLower(modelName);
	return Contains(name, "sniper") || Contains(name, "rifle");
}
