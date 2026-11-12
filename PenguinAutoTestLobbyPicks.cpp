// Pingu - lobbytest: the ability picks (2026-11-07, card ability-picks; PinguAbilityPicks.h, PinguMenu3DPicks.cpp). Before the
// second match: ABILITIES: RANDOM -> CHOOSE by a real press of the lobby's switch, slot 1 opened (the icon grid), Snowball
// picked, slot 3 opened, Flap picked, Snowball refused for slot 2 (never twice); shots Lobby\lobby_picks_empty, _grid, _filled.
// In that match's fight your loadout must be Snowball, a random third ability, Flap.

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinHUD.h"
#include "PinguAbilityPicks.h"
#include "PinguMenu3D.h"
#include "GameFramework/PlayerController.h"

namespace PinguLobbyPicksPrivate
{
	static FName CellId(int32 Slot, EPenguinAbility Ability)
	{
		return FName(*FString::Printf(TEXT("PickCell_%d_%d"), Slot, static_cast<int32>(Ability)));
	}
}

bool APinguLobbyTest::UpdatePicksStage(APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check)
{
	using namespace PinguLobbyPicksPrivate;
	APinguMenu3D* Menu = APinguMenu3D::Get(LobbyHUD);
	if (!Menu)
	{
		Check(false, TEXT("ability picks: no 3D menu in the lobby"));
		return true;
	}
	const float Since = StageTime - PicksAt;
	const auto Press = [Menu](FName Id) { return Menu->FocusButton(Id) && Menu->Accept(); };
	const auto NextSub = [this]() { ++PicksSubStep; PicksAt = StageTime; };
	switch (PicksSubStep)
	{
	case 0: // the host's switch: RANDOM (the default in tests) -> CHOOSE
		Check(!LobbyHUD->IsChooseAbilitiesOn(), TEXT("ability picks: the lobby starts at ABILITIES: RANDOM"));
		Check(Press(TEXT("PicksMode")), TEXT("ability picks: the ABILITIES switch is on the lobby board and presses"));
		NextSub();
		return false;
	case 1:
		if (Since > 0.8f)
		{
			const TArray<EPenguinAbility> Picks = LobbyHUD->GetOwnPicks();
			Check(LobbyHUD->IsChooseAbilitiesOn() && Picks.Num() == 3 && !Picks.ContainsByPredicate([](EPenguinAbility A) { return A != EPenguinAbility::None; })
				&& Menu->FocusButton(TEXT("PickSlot_0")) && Menu->FocusButton(TEXT("PickSlot_2")),
				TEXT("ability picks: CHOOSE on, three slots on the board, all RANDOM"));
			Shot(TEXT("lobby_picks_empty"));
			NextSub();
		}
		return false;
	case 2:
		if (Since > 0.4f)
		{
			Press(TEXT("PickSlot_0"));
			NextSub();
		}
		return false;
	case 3:
		if (Since > 0.8f)
		{
			Check(Menu->FocusButton(CellId(0, EPenguinAbility::Snowball)) && Menu->FocusButton(CellId(0, EPenguinAbility::None)),
				TEXT("ability picks: slot 1 opens the grid (RANDOM and the icons)"));
			Menu->FocusButton(CellId(0, EPenguinAbility::Snowball));
			Shot(TEXT("lobby_picks_grid"));
			NextSub();
		}
		return false;
	case 4:
		if (Since > 0.4f)
		{
			Menu->Accept();
			NextSub();
		}
		return false;
	case 5:
		if (Since > 0.8f)
		{
			Press(TEXT("PickSlot_2"));
			NextSub();
		}
		return false;
	case 6:
		if (Since > 0.8f)
		{
			// Snowball is slot 1's: dimmed in slot 3's grid (not focusable), refused by the action.
			Check(!Menu->FocusButton(CellId(2, EPenguinAbility::Snowball)) && !LobbyHUD->Action_SetPick(2, EPenguinAbility::Snowball),
				TEXT("ability picks: an ability another slot holds can't be picked again"));
			Press(CellId(2, EPenguinAbility::Flap));
			NextSub();
		}
		return false;
	case 7:
		if (Since > 0.8f)
		{
			const TArray<EPenguinAbility> Picks = LobbyHUD->GetOwnPicks();
			const bool bOk = Picks.Num() == 3 && Picks[0] == EPenguinAbility::Snowball && Picks[1] == EPenguinAbility::None && Picks[2] == EPenguinAbility::Flap;
			Check(bOk && !Menu->FocusButton(CellId(2, EPenguinAbility::Noot)), FString::Printf(TEXT("ability picks: picked %s (want Snowball, random, Flap), the grid closed"),
				*PinguPicks::Describe(PinguPicks::ToBytes(Picks))));
			Menu->FocusButton(TEXT("PickSlot_2"));
			Shot(TEXT("lobby_picks_filled"));
			NextSub();
		}
		return false;
	default:
		return Since > 0.4f;
	}
}

void APinguLobbyTest::CheckPicksDealt(APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check)
{
	const APenguinCharacter* Penguin = LobbyHUD && LobbyHUD->PlayerOwner ? Cast<APenguinCharacter>(LobbyHUD->PlayerOwner->GetPawn()) : nullptr;
	const UPenguinAbilityComponent* Abilities = Penguin ? Penguin->GetAbilities() : nullptr;
	if (!Abilities)
	{
		Check(false, TEXT("ability picks dealt: no penguin of yours in the fight"));
		return;
	}
	const TArray<EPenguinAbility>& Loadout = Abilities->GetAbilities();
	const bool bOk = Loadout.Num() == 3 && Loadout[0] == EPenguinAbility::Snowball && Loadout[2] == EPenguinAbility::Flap && Loadout[1] != EPenguinAbility::None
		&& Loadout[1] != EPenguinAbility::Snowball && Loadout[1] != EPenguinAbility::Flap;
	Check(bOk, FString::Printf(TEXT("ability picks dealt: your loadout %s (want Snowball, a random other, Flap)"),
		*PinguPicks::Describe(PinguPicks::ToBytes(Loadout))));
}
