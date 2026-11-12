// Pingu - what a penguin's trait look shows on this machine (PenguinTraitLooks.cpp), for the tests.

#pragma once

#include "CoreMinimal.h"
#include "PinguTraitTable.h"

class APenguinCharacter;

namespace PinguJesterHat
{
	struct FStats;
}

namespace PinguTraitLooks
{
	/** A penguin's trait look as this machine shows it now. */
	struct FLookStats
	{
		/** The look's trait (None: no look). */
		EPinguTrait Trait = EPinguTrait::None;
		/** It waits for its transform (the reveal is still on). */
		bool bPending = false;
		/** It is on (its pieces, materials and shape). */
		bool bApplied = false;
		/** It went on quietly (no pop, no burst, no sound: a late joiner, or a trait without a reveal). */
		bool bQuiet = false;
		/** Seconds since it went on here (the pop runs for its first 0.6 s). */
		float Age = 0.f;
		/** Pieces it put on the penguin (at most 4; Jester's cap is one), and how many show right now. */
		int32 Pieces = 0;
		int32 PiecesShown = 0;
		/** Its see-through sheen (M_Ghost) is the mesh's overlay now. */
		bool bOverlay = false;
		/** The body's shape it gives right now: the Body bone's (GetBodyShape) x the mesh's squash and stretch
		 *  (APenguinCharacter::GetTraitBodyScale: the pop's boing and the jelly). */
		FVector BodyScale = FVector::OneVector;
		/** Mesh sections it hides (all LODs): Speedster the beanie, cuff and pom; Slow Burn the pom. */
		int32 HiddenSections = 0;
		/** Mesh slots wearing a clip material (the ink outline, LOD2 / LOD3's merged slot). */
		int32 ClipMaterials = 0;
		/** Pieces casting a shadow (only from High shadows up). */
		int32 ShadowPieces = 0;
		/** Mesh slots given another material (clip materials, Glass Cannon's glossy clay). */
		int32 SwappedSlots = 0;
	};

	/** Penguin's trait look on this machine; false when it has no trait component. */
	PENGUIN_API bool GetLookStats(const APenguinCharacter* Penguin, FLookStats& Out);

	/** The trait's body shape (side, forward, up) the animation gives Penguin's Body bone right now ((1, 1, 1): none; the
	 *  transform's elastic ease included). PenguinAnimInstance scales the Body bone by it and keeps the eyes, flippers,
	 *  tail and feet in their own shape - the eyes stay round. Game thread. */
	PENGUIN_API FVector GetBodyShape(const APenguinCharacter* Penguin);

	/** Effects every look of this machine spawned so far (they only count up): Quick Draw's bolts, idle effects (sparks,
	 *  smoke, embers, glints, dust...) and transforms played with their burst and sound. */
	struct FLookCounters
	{
		int32 Bolts = 0;
		int32 IdleEffects = 0;
		int32 Transforms = 0;
		int32 QuietLooks = 0;
	};
	PENGUIN_API const FLookCounters& GetLookCounters();

	/** Jester's cap on Penguin on this machine (PinguJesterHat.h: static or on spring bones, its LOD, outline, the bells'
	 *  motion) and how many of the penguin's mesh sections the look hides (the beanie, its cuff and pom on every LOD);
	 *  false when the penguin wears no Jester look (or the cap is missing). */
	PENGUIN_API bool GetJesterHatStats(const APenguinCharacter* Penguin, PinguJesterHat::FStats& Out, int32* OutBeanieHidden = nullptr);
}
