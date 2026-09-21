#pragma once
#include <string>

class VR;

// What the wrist watch shows. -1 means "not known" throughout: the numbers
// come from reading the game's networked variables, and any of them can fail
// to resolve on a given build of GE:S.
struct WatchStats
{
    int health = -1;
    int armor = -1;
    int maxHealth = -1;        // only if the game networks it
    int maxArmor = -1;
    int clip = -1;             // -1 also for weapons without a clip
    int reserve = -1;
    int timeLeft = -1;         // seconds left in a timed round
    std::string weaponModel;   // viewmodel path, e.g. models/weapons/dd44/v_dd44.mdl

    bool operator==(const WatchStats &o) const
    {
        return health == o.health && armor == o.armor && maxHealth == o.maxHealth &&
               maxArmor == o.maxArmor && clip == o.clip && reserve == o.reserve &&
               timeLeft == o.timeLeft && weaponModel == o.weaponModel;
    }
};

// Q-branch wrist watch on the off hand: GoldenEye 64 health (left) and armour
// (right) arcs round a green screen with weapon, ammo and round time.
//
// Like the settings panel it is its own SteamVR overlay, drawn with GDI on a
// worker thread and handed over as a PNG, so the game never knows it exists.
// It is pinned to the off-hand controller (device-relative, so it follows the
// hand with no lag) and turned each frame to face the headset.
namespace VRWatch
{
    void Init(VR *vr);
    void Update();     // in-map frame, render thread
    void Hide();

    // Shared with the offline preview: the display name for a viewmodel path.
    std::wstring WeaponName(const std::string &viewmodel);
}
