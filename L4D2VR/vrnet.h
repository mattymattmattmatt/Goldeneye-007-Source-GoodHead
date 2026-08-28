#pragma once
#include "usercmd.h"
#include "vector.h"

// Pack a VR right-hand pose into unused CUserCmd fields so a listen-server
// host can reconstruct other VR players' gun origin/angles.
//
// Signal: tick_count is stored negated (0 is forced to 1 first).
// Flat clients never do this, so they keep working on a VR-hosted server.
//
// Payload:
//   mousedx/dy     = controller pitch/yaw * 10
//   command_number = original + ((roll+180)/2)*10_000_000
//                    negated if the hand is swinging fast (melee cue)
//   viewangles.z   = controller pos.x
//   upmove         = controller pos.y
//   viewangles.x   = mix(controller pos.z, original pitch)
//
// After the engine reads the cmd we restore look-angles, then — if the
// player is firing — overwrite viewangles with the controller so GE:S
// hitscan follows the gun without L4D2's FireTerrorBullets hook.

namespace VRNet
{
	constexpr int kMaxPlayers = 33; // 1..32, slot 0 unused

	inline bool SlotOk(int i)
	{
		return i > 0 && i < kMaxPlayers;
	}

	struct Pose
	{
		Vector pos;
		QAngle ang;
		bool swinging = false;
	};

	inline int SafeTick(int tick)
	{
		if (tick <= 0)
			tick = 1;
		return tick;
	}

	inline void Encode(CUserCmd *to, const Pose &pose)
	{
		to->tick_count = -SafeTick(to->tick_count);

		int originalCommandNum = to->command_number;
		to->mousedx = static_cast<short>(pose.ang.x * 10.0f);
		to->mousedy = static_cast<short>(pose.ang.y * 10.0f);

		int rollEncoding = ((static_cast<int>(pose.ang.z) + 180) / 2) * 10000000;
		to->command_number = originalCommandNum + rollEncoding;
		if (pose.swinging)
			to->command_number *= -1;

		to->viewangles.z = pose.pos.x;
		to->upmove = pose.pos.y;

		const float xAngle = to->viewangles.x;
		int encodedAngle = static_cast<int>((xAngle + 360.0f) * 10.0f);
		int encoding = static_cast<int>(pose.pos.z * 10.0f) * 10000;
		encoding += encoding < 0 ? -encodedAngle : encodedAngle;
		to->viewangles.x = static_cast<float>(encoding);
	}

	inline bool Decode(CUserCmd *move, Pose &out)
	{
		if (move->tick_count >= 0)
			return false;

		move->tick_count *= -1;

		out.swinging = false;
		if (move->command_number < 0)
		{
			move->command_number *= -1;
			out.swinging = true;
		}

		out.ang.x = static_cast<float>(move->mousedx) / 10.0f;
		out.ang.y = static_cast<float>(move->mousedy) / 10.0f;
		out.pos.x = move->viewangles.z;
		out.pos.y = move->upmove;

		int rollEncoding = move->command_number / 10000000;
		move->command_number -= rollEncoding * 10000000;
		out.ang.z = static_cast<float>(rollEncoding * 2 - 180);

		int decodedZInt = static_cast<int>(move->viewangles.x / 10000);
		float decodedAngle = abs((move->viewangles.x - (decodedZInt * 10000)) / 10.0f);
		decodedAngle -= 360.0f;
		out.pos.z = static_cast<float>(decodedZInt) / 10.0f;

		move->viewangles.x = decodedAngle;
		move->viewangles.z = 0;
		move->upmove = 0;
		return true;
	}
}
