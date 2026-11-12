// Pingu - the host's TRAITS list (2026-11-07, card shared-traits; Linus: "make it so traits are for all the same", picked "A1":
// a random trait each round, from a list the host ticks). The lobby's PICK sign (next to TRAITS) opens the TraitPicks screen
// (APinguMenu3D's page: PinguMenu3DTraits.cpp); every trait of the table is a toggle there, all ticked at first. The list is
// the host's match setting (FPinguMatchSettings::TraitsOff, replicated): APenguinGameMode::SetTraitsOff takes it (lobby only),
// the round bag deals only ticked traits (PinguTraits::DrawShared). A LAN client never opens it (the host decides).

#include "PenguinHUD.h"
#include "Penguin.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PinguMenu3D.h"

namespace PenguinHUDTraitsPrivate
{
	/** The menu screenshots' made-up list: a few traits unticked, so the picture shows both looks. */
	static uint64 ShotTraitsOff()
	{
		uint64 Off = 0;
		for (const EPinguTrait Trait : { EPinguTrait::HotFeet, EPinguTrait::Gambler, EPinguTrait::HighGravity })
		{
			Off |= uint64(1) << static_cast<int32>(Trait);
		}
		return Off;
	}
}

void APenguinHUD::Action_TraitPicks()
{
	// The host / solo player, in the lobby, with the 3D menus (the old Slate lobby has no list: its bag is every trait).
	if (!HasMatchAuthority() || GetPhase() != EPinguPhase::Lobby || !PinguMenu3D::IsEnabled())
	{
		return;
	}
	PushScreen(EPinguScreen::TraitPicks);
}

bool APenguinHUD::IsTraitPicked(EPinguTrait Trait) const
{
	if (bMenuShots)
	{
		return !PinguTraits::IsOffIn(PenguinHUDTraitsPrivate::ShotTraitsOff(), Trait);
	}
	return GetMatchSettings().IsTraitAllowed(Trait);
}

int32 APenguinHUD::GetTraitPickCount() const
{
	int32 Count = 0;
	for (const EPinguTrait Trait : PinguTraits::GetAll())
	{
		Count += IsTraitPicked(Trait) ? 1 : 0;
	}
	return Count;
}

void APenguinHUD::Action_ToggleTraitPick(EPinguTrait Trait)
{
	APenguinGameMode* GameMode = bMenuShots || !HasMatchAuthority() ? nullptr : GetPenguinGameMode();
	if (!GameMode || !PinguTraits::IsReal(Trait))
	{
		return;
	}
	// (The last ticked one stays ticked: the game mode refuses an empty list.)
	const uint64 Bit = uint64(1) << static_cast<int32>(Trait);
	GameMode->SetTraitsOff(GetMatchSettings().TraitsOff ^ Bit);
}

void APenguinHUD::Action_PickAllTraits()
{
	if (APenguinGameMode* GameMode = bMenuShots || !HasMatchAuthority() ? nullptr : GetPenguinGameMode())
	{
		GameMode->SetTraitsOff(0);
	}
}
