#include "weapons.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cstdio>

// Numpad-tuned entries. Read on the render thread, loaded on the config
// thread, so everything goes through the lock.
static std::mutex g_overrideMtx;
static std::map<std::string, PositionAngle> g_overrides;

static std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

static bool Contains(const std::string &hay, const char *needle)
{
	return hay.find(needle) != std::string::npos;
}

std::string Weapons::Key(const std::string &modelName)
{
	std::string base = ToLower(modelName);
	const size_t slash = base.find_last_of("/\\");
	if (slash != std::string::npos)
		base = base.substr(slash + 1);
	if (base.compare(0, 2, "v_") == 0)
		base = base.substr(2);
	const size_t dot = base.find('.');
	if (dot != std::string::npos)
		base = base.substr(0, dot);
	return base;
}

void Weapons::SetOverride(const std::string &key, const PositionAngle &pose)
{
	std::lock_guard<std::mutex> lk(g_overrideMtx);
	g_overrides[key] = pose;
}

void Weapons::ClearOverride(const std::string &key)
{
	std::lock_guard<std::mutex> lk(g_overrideMtx);
	g_overrides.erase(key);
}

bool Weapons::HasOverride(const std::string &key)
{
	std::lock_guard<std::mutex> lk(g_overrideMtx);
	return g_overrides.count(key) != 0;
}

// Lines of "key = forward right up pitch yaw roll"; # starts a comment.
bool Weapons::LoadOverrides(const char *path)
{
	std::ifstream in(path);
	if (!in)
		return false;
	std::map<std::string, PositionAngle> loaded;
	std::string line;
	while (std::getline(in, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = line.substr(0, eq);
		key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c) != 0; }), key.end());
		std::istringstream vals(line.substr(eq + 1));
		PositionAngle p{};
		if (!key.empty() && (vals >> p.position.x >> p.position.y >> p.position.z >> p.angle.x >> p.angle.y >> p.angle.z))
			loaded[ToLower(key)] = p;
	}
	std::lock_guard<std::mutex> lk(g_overrideMtx);
	g_overrides.swap(loaded);
	return true;
}

bool Weapons::SaveOverrides(const char *path)
{
	std::map<std::string, PositionAngle> copy;
	{
		std::lock_guard<std::mutex> lk(g_overrideMtx);
		copy = g_overrides;
	}
	std::ofstream out(path, std::ios::trunc);
	if (!out)
		return false;
	out << "# GE:S VR: where each weapon sits in your hand, tuned in the headset with\n"
	       "# the numpad (free aim, VR Settings > Weapons > Adjust position).\n"
	       "# key = forward right up pitch yaw roll\n"
	       "# Delete a line to go back to the built-in position for that weapon.\n";
	char buf[160];
	for (const auto &kv : copy)
	{
		const PositionAngle &p = kv.second;
		snprintf(buf, sizeof(buf), "%s = %.2f %.2f %.2f %.1f %.1f %.1f\n", kv.first.c_str(),
		         p.position.x, p.position.y, p.position.z, p.angle.x, p.angle.y, p.angle.z);
		out << buf;
	}
	return true;
}

PositionAngle Weapons::GetOffset(const std::string &modelName)
{
	{
		std::lock_guard<std::mutex> lk(g_overrideMtx);
		const std::string key = Key(modelName);
		auto it = g_overrides.find(key);
		// The throwing knife (v_tknife) has the hunting knife's arms: until it
		// is tuned itself, it borrows the knife's position. Tuning it saves its
		// own "tknife" line.
		if (it == g_overrides.end() && key == "tknife")
			it = g_overrides.find("knife");
		if (it != g_overrides.end())
			return it->second;
	}
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
		{ "zmg",          {{ 16.5f,  4.5f, -4.0f }, { -1.0f, 0.0f, 0.0f }} },
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

		// Melee and thrown: arm rigs, placed by their hand bone (R_FK_Hand_jnt)
		// in free aim, so these are small nudges and twists of the hand.
		// Tuned in the headset with the numpad (2026-09-22). First match wins:
		// "tknife" and the named mines must stay above "knife" and "mine"
		// (the throwing knife lives in models/weapons/knife/, the mines in
		// models/weapons/mines/).
		{ "tknife",       {{  5.0f,  0.0f,  4.5f }, { -12.5f, 15.0f,   2.5f }} },
		{ "throwing",     {{  5.0f,  0.0f,  4.5f }, { -12.5f, 15.0f,   2.5f }} },
		{ "knife",        {{  4.0f, -1.5f,  4.5f }, { -12.5f, 15.0f,   2.5f }} },
		{ "slapper",      {{  0.0f, -3.5f,  5.5f }, { -55.0f,  2.5f,  25.0f }} },
		{ "fist",         {{  0.0f,  0.0f,  0.0f }, {   0.0f,  0.0f,   0.0f }} },
		{ "proximitymine",{{  4.0f, -0.5f,  7.0f }, {  -8.0f, -7.5f, -25.0f }} },
		{ "remotemine",   {{  3.5f,  0.5f,  5.5f }, {  -0.5f,-12.5f, -47.5f }} },
		{ "timedmine",    {{  5.5f,  0.5f,  3.5f }, {  -0.5f, -2.5f, -20.0f }} },
		{ "grenade",      {{  5.0f, -1.5f,  3.5f }, {  -8.0f, 15.0f,  17.5f }} },
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
