// Pingu - ability picks (2026-11-07, card ability-picks; Linus: "make it so u can also make it so everyone can chose his abilitys
// but default is random", "B2 (everyone has to chose his own in the lobby and random defaults)").
//
// The host's lobby setting ABILITIES: RANDOM / CHOOSE (FPinguMatchSettings::bChooseAbilities, saved in UPinguSettings). With
// CHOOSE every person picks up to three abilities for their three slots in the lobby (APenguinPlayerState::GetPicks; a slot left
// at RANDOM is rolled each round as before, never a copy of a picked one). Bots always roll. Picks can only change in the lobby,
// so they hold for the whole match. The round's deal uses them: solo APenguinGameMode::SetupRound, LAN PinguFair::Deal (the picks
// are part of the reveal, so every joiner recomputes the same loadouts and its judge checks them: PinguFair::CheckReveal).
// The trait Same Abilities overrides the picks: everybody gets one shared roll of three that round.

#pragma once

#include "CoreMinimal.h"
#include "PenguinTypes.h"
#include "PinguTraitTable.h"

class APenguinCharacter;
class UWorld;

namespace PinguPicks
{
	/** Slots a person can pick for. */
	constexpr int32 NumSlots = 3;

	/** True for nothing (no picks) or NumSlots entries, each a real ability or None (random), no ability twice. */
	PENGUIN_API bool IsValid(const TArray<uint8>& Picks);
	/** Any entry is a real ability. */
	PENGUIN_API bool HasAny(const TArray<uint8>& Picks);
	/** NumSlots entries (missing ones None). */
	PENGUIN_API TArray<EPenguinAbility> ToAbilities(const TArray<uint8>& Picks);
	PENGUIN_API TArray<uint8> ToBytes(const TArray<EPenguinAbility>& Picks);
	/** "Snowball, random, Flap" (logs). */
	PENGUIN_API FString Describe(const TArray<uint8>& Picks);

	/**
	 * A round's loadout of NumAbilities (a trait's): with no picks exactly UPenguinAbilityComponent::RollLoadout (the same rolls
	 * as ever). With picks: three slots keep their picks in place and roll the rest from the abilities nobody picked for this
	 * penguin; two (Second Chance: slots 0 and 2) take the first two picks in slot order, rolled up to two.
	 */
	PENGUIN_API TArray<EPenguinAbility> MakeLoadout(FRandomStream& Rand, int32 NumAbilities, const TArray<uint8>& Picks);
	/** Same Abilities: the round's one shared roll of three, cut to NumAbilities as RollLoadout lays two out (slots 0 and 2). */
	PENGUIN_API TArray<EPenguinAbility> FitShared(const TArray<EPenguinAbility>& Shared, int32 NumAbilities);
	/** Any of these traits makes the round's loadouts one shared roll (Same Abilities). */
	PENGUIN_API bool IsSameAbilitiesRound(const TArray<EPinguTrait>& Traits);

	/** The match's setting (the host's: replicated in the game state's settings). */
	PENGUIN_API bool IsChooseOn(const UObject* WorldContext);
	/** A penguin's picks for a match round: its person's (CHOOSE on), nothing for bots and with RANDOM. */
	PENGUIN_API TArray<uint8> GetPicksFor(const APenguinCharacter* Penguin);

	/** The host's saved choice (UPinguSettings), read when the lobby opens; automated runs: off unless -PinguChoose. */
	PENGUIN_API bool LoadChooseSetting();
	PENGUIN_API void SaveChooseSetting(bool bOn);
}
