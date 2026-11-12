// Pingu - the host's TRAITS list (2026-11-07, card shared-traits). A page of APinguMenu3D reached from the lobby's PICK sign
// (APenguinHUD::Action_TraitPicks): a wooden board with TRAITS, how many are ticked, every trait of the table as a toggle sign
// (EPinguMenu3DKind::Toggle, like TRAITS ON / OFF: plain when out of the bag, lit when in it; four to a row, in table order -
// traits other cards add show up by themselves), then ALL and DONE. Linus: "pls dont add gui exept where its nescesary and
// them ake it like the rest and clean" - so nothing but the toggles, in the menus' own signs.

#include "PinguMenu3D.h"
#include "Penguin.h"
#include "PenguinHUD.h"
#include "Widgets/SNullWidget.h"
#include "PinguUI.h"

#define LOCTEXT_NAMESPACE "PinguMenu3D"

namespace PinguMenu3DTraitsPrivate
{
	constexpr int32 Columns = 4;
	constexpr float ToggleWidth = 196.f;
	constexpr float ToggleHeight = 40.f;
	constexpr float GapX = 10.f;
	constexpr float GapY = 8.f;
	constexpr float Pad = 40.f;
	constexpr float Inner = Columns * ToggleWidth + (Columns - 1) * GapX;
	constexpr float Width = Inner + 2.f * Pad;
	constexpr float TitleHeight = 52.f;
	constexpr float ButtonHeight = 56.f;
}

void APinguMenu3D::BuildTraitPicks()
{
	using namespace PinguMenu3DTraitsPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	const TArray<EPinguTrait> All = PinguTraits::GetAll();
	const int32 Rows = FMath::Max(1, (All.Num() + Columns - 1) / Columns);
	const float GridHeight = Rows * ToggleHeight + (Rows - 1) * GapY;
	const float Height = 24.f + TitleHeight + 16.f + GridHeight + 24.f + ButtonHeight + 24.f;
	BeginPage(TraitPicks, EPinguMenu3DPage::TraitPicks, FVector2D(Width, Height));
	TraitPicks.DefaultFocus = TEXT("Done");
	auto Z = [Height](float FromTop) { return 0.5f * Height - FromTop; };
	const float Left = -0.5f * Inner;
	const float Right = 0.5f * Inner;
	float Y = 24.f;

	// ---- TRAITS, and on the right how many are in the bag
	AddLabel(TraitPicks, nullptr, FVector2D(Left, Z(Y + 26.f)), 36.f, [] { return LOCTEXT("TraitPicksTitle", "TRAITS"); }, Style.Snow,
		PinguMenu3D::ETextAlign::Left, 0.f, true, 10.f);
	AddLabel(TraitPicks, nullptr, FVector2D(Right, Z(Y + 26.f)), 14.f,
		[WeakHUD]
		{
			const APenguinHUD* H = WeakHUD.Get();
			return FText::Format(LOCTEXT("TraitPicksCount", "{0} OF {1} IN THE BAG"), FText::AsNumber(H ? H->GetTraitPickCount() : PinguNumTraits),
				FText::AsNumber(PinguNumTraits));
		}, Style.IceDim, PinguMenu3D::ETextAlign::Right);
	Y += TitleHeight + 16.f;

	// ---- every trait: a toggle (a click ticks it in or out of the round bag)
	for (int32 Index = 0; Index < All.Num(); ++Index)
	{
		const EPinguTrait Trait = All[Index];
		const int32 Row = Index / Columns;
		const int32 Column = Index % Columns;
		const FVector2D Center(Left + 0.5f * ToggleWidth + Column * (ToggleWidth + GapX), Z(Y + 0.5f * ToggleHeight + Row * (ToggleHeight + GapY)));
		const FText Name = FText::FromString(FString(PinguTraits::GetInfo(Trait).Name).ToUpper());
		// (Style 5: a dense grid of toggles are toggle rows - the row plank, no snow over the row above.)
		bNextRowPlank = true;
		FButton& Toggle = AddButton(TraitPicks, FName(*FString::Printf(TEXT("Trait_%s"), PinguTraits::GetInfo(Trait).IconName)), EPinguMenu3DKind::Toggle,
			Center, FVector2D(ToggleWidth, ToggleHeight), 14.f, [Name] { return Name; },
			[WeakHUD, Trait] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_ToggleTraitPick(Trait); } });
		Toggle.Selected = [WeakHUD, Trait] { const APenguinHUD* H = WeakHUD.Get(); return H && H->IsTraitPicked(Trait); };
	}
	Y += GridHeight + 24.f;

	// ---- ALL (back to every trait) and DONE
	AddButton(TraitPicks, TEXT("All"), EPinguMenu3DKind::Normal, FVector2D(Left + 90.f, Z(Y + 0.5f * ButtonHeight)), FVector2D(180.f, 48.f), 16.f,
		[] { return LOCTEXT("TraitPicksAll", "ALL"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_PickAllTraits(); } });
	AddButton(TraitPicks, TEXT("Done"), EPinguMenu3DKind::Primary, FVector2D(Right - 105.f, Z(Y + 0.5f * ButtonHeight)), FVector2D(210.f, ButtonHeight), 20.f,
		[] { return LOCTEXT("TraitPicksDone", "DONE"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_Back(); } });
	EndStatic(TraitPicks);
}

#undef LOCTEXT_NAMESPACE

// ---------------------------------------------------------------------------- its Slate screen: the input layer only

TSharedRef<SWidget> SPinguRoot::BuildTraitPicks()
{
	// (The old Slate menus have no TRAITS list: their bag is every trait.)
	if (!PinguMenu3D::IsEnabled())
	{
		return SNullWidget::NullWidget;
	}
	TSharedRef<SPinguMenu3DInput> Input = SNew(SPinguMenu3DInput).HUD(HUD);
	FirstFocus.Add(static_cast<uint8>(EPinguScreen::TraitPicks), Input);
	return MakeScreen(EPinguScreen::TraitPicks, Input, false);
}
