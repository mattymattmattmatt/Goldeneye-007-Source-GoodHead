#pragma once
#include "vector.h"
#include "sdk.h"
#include <string>

namespace Weapons
{
	// N64-style GE:S viewmodel offsets, keyed by a substring of the model name.
	// Tune in VR/weapons.txt or by editing this table after a test session.
	PositionAngle GetOffset(const std::string &modelName);
	bool IsMelee(const std::string &modelName);
	bool IsDualWieldable(const std::string &modelName);
	bool IsThrowable(const std::string &modelName);
	bool IsWatch(const std::string &modelName);
	bool IsSniper(const std::string &modelName);
}
