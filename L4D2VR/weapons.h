#pragma once
#include "vector.h"
#include "sdk.h"
#include <string>

namespace Weapons
{
	// Where each weapon sits in the hand: the built-in table, keyed by a
	// substring of the model name, unless VR/weapons.txt has an entry for it.
	// { forward, right, up } is the controller's position relative to the
	// model's anchor; { pitch, yaw, roll } turns the model about the hand.
	PositionAngle GetOffset(const std::string &modelName);

	// Per-weapon entries tuned in the headset with the numpad (VR::
	// ProcessTuneKeys) and saved to VR/weapons.txt. Keyed by the viewmodel's
	// file name without "v_" and ".mdl", e.g. "dd44", "slappers".
	std::string Key(const std::string &modelName);
	void SetOverride(const std::string &key, const PositionAngle &pose);
	void ClearOverride(const std::string &key);
	bool HasOverride(const std::string &key);
	bool LoadOverrides(const char *path);
	bool SaveOverrides(const char *path);
	bool IsMelee(const std::string &modelName);
	bool IsDualWieldable(const std::string &modelName);
	bool IsThrowable(const std::string &modelName);
	bool IsWatch(const std::string &modelName);
	bool IsSniper(const std::string &modelName);
}
