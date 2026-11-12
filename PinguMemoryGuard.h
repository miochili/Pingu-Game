// Pingu - the guard against memory trainers (2026-11-05, card host-cheat-fix). Linus's friend used "PinguTrainer.exe": an
// outside program that finds our objects' properties by their reflected names in the running game and writes them about
// every 50 ms (Read/WriteProcessMemory; no hook, no RPC). As host it overwrote the loadout ("Abilities"), zeroed the
// cooldowns ("CooldownEnds") and changed the movement tuning (RunSpeed, MaxAirJumps, GravityScale, JumpZVelocity, ...).
//
// What stops it now (server / solo; protects solo, the host and everybody the host plays with):
//  - The ability component keeps a hidden, masked shadow of the loadout and the cooldown ends (UPenguinAbilityComponent::
//    GuardShadow): an outside write is undone every frame and before replication, IsReady goes by the shadow. The reflected
//    names of those arrays are meaningless now (Vq, Tz, ...), so the trainer does not even find them.
//  - This guard: every frame before the actors tick, each penguin's movement tuning (every editable number of
//    UPenguinMovementComponent, plus the engine's GravityScale / JumpZVelocity / MaxWalkSpeed / MaxAcceleration that are
//    derived from it) is compared with the class defaults (the archetype: what BP_Penguin sets) and put back when it differs.
//    Nothing in the game changes these numbers at runtime (traits and sizes go through their own scales), so any difference is
//    an outside write.
// Off in the editor (PIE: Linus may tweak a penguin in the details panel) and with the dev switch -PinguCheat=noguard.
// What it can't stop: a trainer that finds the shadow (no name, masked: it needs real reverse engineering) or patches our
// code; joiners still judge what the host replicates (FPinguCheatJudge).
#pragma once

#include "CoreMinimal.h"

class UWorld;
class APenguinCharacter;

namespace PinguMemoryGuard
{
	/** One frame of the guard (and of the dev trainer, -PinguCheat=trainer) for World. Called before the actors tick. */
	PENGUIN_API void TickWorld(UWorld* World);
	/** Movement numbers put back since the start (tests). */
	PENGUIN_API int32 GetMovementRestores();
	/** How many numbers of a penguin's movement the guard watches (tests). */
	PENGUIN_API int32 GetWatchedCount();
	/** The guard is on in this process (not in the editor, not -PinguCheat=noguard). */
	PENGUIN_API bool IsActive();
#if !UE_BUILD_SHIPPING
	/** Tests: the movement guard on even in the editor (the autotest's own check). */
	PENGUIN_API void SetForcedOn(bool bOn);
	/** The dev trainer: what PinguTrainer.exe wrote, done to the local player's penguin of World (or Target) right now -
	 *  loadout Snow Bomb / Ice Gun / Noot, every cooldown 0, run speed x2, 4 air jumps, half gravity. */
	PENGUIN_API void DevTrainerWrite(UWorld* World, APenguinCharacter* Target = nullptr);
#endif
}
