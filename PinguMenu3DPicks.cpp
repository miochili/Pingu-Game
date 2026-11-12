// Pingu - the card lobby's ability picks (2026-11-07, card ability-picks; Linus: "make it so u can also make it so everyone can
// chose his abilitys but default is random", "pls dont add gui exept where its nescesary and them ake it like the rest and
// clean"). One row under the cards: the host's ABILITIES: RANDOM / CHOOSE sign (a joiner reads it, only when CHOOSE), and with
// CHOOSE your three slots, each "RANDOM" until you pick. A slot clicked opens a small grid under the row: RANDOM and every
// ability's icon (one your other slots hold is dimmed: never the same twice); a click picks it and closes the grid. Only the
// existing controls (AddButton, AddDecoSlab / AddDecoIcon as the PINGUPEDIA's ability icons); PinguAbilityPicks.h for the rules.

#include "PinguMenu3D.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinHUD.h"
#include "PinguAbilityPicks.h"
#include "PinguUI.h"

#define LOCTEXT_NAMESPACE "PinguMenu3D"

namespace PinguMenu3DPicksPrivate
{
	constexpr float RowGap = 18.f;      // under the cards
	constexpr float RowHeight = 46.f;
	constexpr float ToggleWidth = 236.f;
	constexpr float SlotWidth = 150.f;
	constexpr float SlotGap = 8.f;
	constexpr float ToggleGap = 16.f;
	constexpr float GridGap = 12.f;     // under the row
	constexpr float Cell = 52.f;
	constexpr float CellGap = 8.f;
	constexpr int32 Columns = 11;       // RANDOM takes two cells, then the 19 icons
	constexpr float CellIcon = 44.f;

	static int32 GridCells()
	{
		return 2 + static_cast<int32>(EPenguinAbility::Count) - 1;
	}
	static float GridHeight()
	{
		const int32 Rows = (GridCells() + Columns - 1) / Columns;
		return Rows * Cell + (Rows - 1) * CellGap;
	}
	static FText AbilityWord(EPenguinAbility Ability)
	{
		return Ability == EPenguinAbility::None ? LOCTEXT("PicksRandom", "RANDOM") : PinguUI::Upper(UPenguinAbilityComponent::GetInfo(Ability).Name);
	}
}

float APinguMenu3D::GetPicksHeight() const
{
	using namespace PinguMenu3DPicksPrivate;
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return 0.f;
	}
	const bool bChoose = PenguinHUD->IsChooseAbilitiesOn();
	if (!bChoose && !PenguinHUD->HasMatchAuthority())
	{
		return 0.f; // (a joiner with RANDOM: nothing new to see)
	}
	return RowGap + RowHeight + (bChoose && PickerSlot >= 0 ? GridGap + GridHeight() : 0.f);
}

FString APinguMenu3D::GetPicksKey() const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return FString();
	}
	FString Key = FString::Printf(TEXT("picks%d%d%d:"), PenguinHUD->HasMatchAuthority() ? 1 : 0, PenguinHUD->IsChooseAbilitiesOn() ? 1 : 0, PickerSlot);
	for (const EPenguinAbility Ability : PenguinHUD->GetOwnPicks())
	{
		Key += FString::Printf(TEXT("%d,"), static_cast<int32>(Ability));
	}
	return Key;
}

void APinguMenu3D::AddPicksRows(FPageParts& Parts, float Top)
{
	using namespace PinguMenu3DPicksPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return;
	}
	const bool bAuthority = PenguinHUD->HasMatchAuthority();
	const bool bChoose = PenguinHUD->IsChooseAbilitiesOn();
	if (!bChoose)
	{
		PickerSlot = -1;
	}
	if (GetPicksHeight() <= 0.f)
	{
		return;
	}
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	TWeakObjectPtr<APinguMenu3D> WeakThis(this);
	const float RowZ = Top - RowGap - 0.5f * RowHeight;
	const float RowWidth = ToggleWidth + (bChoose ? ToggleGap + 3.f * SlotWidth + 2.f * SlotGap : 0.f);
	float X = -0.5f * RowWidth;

	// The switch (the host / solo: a toggle like TRAITS; a joiner reads the host's CHOOSE).
	const FVector2D ToggleCenter(X + 0.5f * ToggleWidth, RowZ);
	if (bAuthority)
	{
		FButton& Toggle = AddButton(Parts, TEXT("PicksMode"), EPinguMenu3DKind::Toggle, ToggleCenter, FVector2D(ToggleWidth, RowHeight), 15.f,
			[WeakHUD]
			{
				const APenguinHUD* H = WeakHUD.Get();
				return H && H->IsChooseAbilitiesOn() ? LOCTEXT("PicksChoose", "ABILITIES: CHOOSE") : LOCTEXT("PicksRandomMode", "ABILITIES: RANDOM");
			},
			[WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_ToggleChooseAbilities(); } });
		Toggle.Selected = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->IsChooseAbilitiesOn(); };
	}
	else
	{
		AddLabel(Parts, nullptr, FVector2D(ToggleCenter.X, RowZ - 7.5f), 15.f, [] { return LOCTEXT("PicksChooseRead", "ABILITIES: CHOOSE"); },
			Style.IceDim, PinguMenu3D::ETextAlign::Center, ToggleWidth, true, 6.f);
	}
	if (!bChoose)
	{
		return;
	}
	X += ToggleWidth + ToggleGap;

	// Your three slots: the pick's name, or RANDOM (a sign's word sits between its nails: no icon beside it). A click opens (or closes) the grid for that slot.
	const TArray<EPenguinAbility> Picks = PenguinHUD->GetOwnPicks();
	for (int32 Slot = 0; Slot < PinguPicks::NumSlots; ++Slot)
	{
		const EPenguinAbility Ability = Picks.IsValidIndex(Slot) ? Picks[Slot] : EPenguinAbility::None;
		const FVector2D Center(X + 0.5f * SlotWidth, RowZ);
		FButton& SlotButton = AddButton(Parts, FName(*FString::Printf(TEXT("PickSlot_%d"), Slot)), EPinguMenu3DKind::Segment, Center,
			FVector2D(SlotWidth, RowHeight), 13.f, [Ability] { return PinguMenu3DPicksPrivate::AbilityWord(Ability); },
			[WeakThis, Slot]
			{
				if (APinguMenu3D* Menu = WeakThis.Get())
				{
					Menu->PickerSlot = Menu->PickerSlot == Slot ? -1 : Slot;
				}
			});
		const int32 OpenSlot = PickerSlot;
		SlotButton.Selected = [OpenSlot, Slot] { return OpenSlot == Slot; };
		SlotButton.bInstant = true;
		X += SlotWidth + SlotGap;
	}
	if (PickerSlot < 0 || PickerSlot >= PinguPicks::NumSlots)
	{
		return;
	}

	// The grid for the open slot: RANDOM (two cells wide), then every ability; one your other slots hold is dimmed.
	const int32 OpenSlot = PickerSlot;
	const float GridWidth = Columns * Cell + (Columns - 1) * CellGap;
	const float GridTop = Top - RowGap - RowHeight - GridGap;
	int32 CellIndex = 0;
	for (int32 Value = 0; Value < static_cast<int32>(EPenguinAbility::Count); ++Value)
	{
		const EPenguinAbility Ability = static_cast<EPenguinAbility>(Value);
		const int32 Span = Ability == EPenguinAbility::None ? 2 : 1;
		const int32 Row = CellIndex / Columns;
		const int32 Column = CellIndex % Columns;
		const float Width = Span * Cell + (Span - 1) * CellGap;
		const FVector2D Center(-0.5f * GridWidth + Column * (Cell + CellGap) + 0.5f * Width, GridTop - Row * (Cell + CellGap) - 0.5f * Cell);
		CellIndex += Span;
		bool bTaken = false;
		for (int32 Other = 0; Other < Picks.Num(); ++Other)
		{
			bTaken |= Other != OpenSlot && Ability != EPenguinAbility::None && Picks[Other] == Ability;
		}
		const bool bCurrent = Picks.IsValidIndex(OpenSlot) && Picks[OpenSlot] == Ability;
		FButton& CellButton = AddButton(Parts, FName(*FString::Printf(TEXT("PickCell_%d_%d"), OpenSlot, Value)), EPinguMenu3DKind::Segment, Center,
			FVector2D(Width, Cell), 13.f,
			[Ability] { return Ability == EPenguinAbility::None ? PinguMenu3DPicksPrivate::AbilityWord(Ability) : FText::GetEmpty(); },
			[WeakHUD, WeakThis, OpenSlot, Ability]
			{
				APenguinHUD* H = WeakHUD.Get();
				if (H && H->Action_SetPick(OpenSlot, Ability))
				{
					if (APinguMenu3D* Menu = WeakThis.Get())
					{
						Menu->PickerSlot = -1;
					}
				}
			});
		CellButton.bInstant = true;
		CellButton.Selected = [bCurrent] { return bCurrent; };
		CellButton.Enabled = [bTaken] { return !bTaken; };
		if (Ability != EPenguinAbility::None)
		{
			const float Rim = 2.f;
			AddDecoSlab(Parts, CellButton.Body, FVector2D::ZeroVector, FVector2D(CellIcon, CellIcon), 0.5f * CellIcon, 3.f,
				bTaken ? Style.Disabled : UPenguinAbilityComponent::GetInfo(Ability).Color, Style.Ink, 2.f);
			AddDecoSlab(Parts, CellButton.Body, FVector2D::ZeroVector, FVector2D(CellIcon - 2.f * Rim, CellIcon - 2.f * Rim), 0.5f * CellIcon - Rim, 2.f,
				PinguMenu3D::WoodEdge * 2.f, PinguMenu3D::WoodEdge, 3.f);
			const FSlateBrush* Brush = PenguinHUD->GetAbilityBrush(Ability);
			if (UTexture* Texture = Brush ? Cast<UTexture>(Brush->GetResourceObject()) : nullptr)
			{
				AddDecoIcon(Parts, CellButton.Body, FVector2D::ZeroVector, CellIcon - 8.f, Texture, bTaken ? FLinearColor(0.3f, 0.3f, 0.3f, 1.f) : FLinearColor::White, 4.f);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
