#pragma once

class VR;

// Game events GE:S fires on the client -- kills, round start and round end --
// turned into short notices on the wrist watch (VRWatch::Notify): the kill
// feed on your wrist instead of in the corner of a screen you cannot see.
namespace VREvents
{
    // In-map frames (render thread). Registers with the engine's game event
    // manager once its events exist, and re-registers if the engine ever
    // drops the listener. Does nothing while WatchKillFeed is off.
    void Update(VR *vr);
}
