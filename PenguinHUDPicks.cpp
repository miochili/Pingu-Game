// Pingu - the HUD's side of the ability picks (2026-11-07, card ability-picks; PinguAbilityPicks.h): the lobby's ABILITIES
// RANDOM / CHOOSE switch and your own three picks.

#include "PenguinHUD.h"
#include "Penguin.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinPlayerState.h"
#include "PinguAbilityPicks.h"
#include "GameFramework/PlayerController.h"

bool APenguinHUD::IsChooseAbilitiesOn() const
{
	return GetMatchSettings().bChooseAbilities;
}

void APenguinHUD::Action_ToggleChooseAbilities()
{
	// The host (and solo) only, in the lobby: the match settings replicate; a joiner's lobby shows the host's choice.
	if (APenguinGameMode* GameMode = GetPenguinGameMode())
	{
		GameMode->SetChooseAbilities(!IsChooseAbilitiesOn());
	}
}

TArray<EPenguinAbility> APenguinHUD::GetOwnPicks() const
{
	const APenguinPlayerState* State = PlayerOwner ? PlayerOwner->GetPlayerState<APenguinPlayerState>() : nullptr;
	return PinguPicks::ToAbilities(State ? State->GetShownPicks() : TArray<uint8>());
}

bool APenguinHUD::Action_SetPick(int32 Slot, EPenguinAbility Ability)
{
	APenguinPlayerState* State = PlayerOwner ? PlayerOwner->GetPlayerState<APenguinPlayerState>() : nullptr;
	if (!State || Slot < 0 || Slot >= PinguPicks::NumSlots || !IsChooseAbilitiesOn())
	{
		return false;
	}
	TArray<EPenguinAbility> Picks = GetOwnPicks();
	if (Ability != EPenguinAbility::None)
	{
		for (int32 Other = 0; Other < Picks.Num(); ++Other)
		{
			if (Other != Slot && Picks[Other] == Ability)
			{
				return false; // (one of your other slots holds it: never the same ability twice)
			}
		}
	}
	Picks[Slot] = Ability;
	State->RequestPicks(PinguPicks::ToBytes(Picks));
	UE_LOG(LogPingu, Display, TEXT("[UI] ability pick %d: %s"), Slot + 1, *PinguPicks::Describe(PinguPicks::ToBytes(Picks)));
	return true;
}
