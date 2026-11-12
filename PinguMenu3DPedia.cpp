// Pingu - the 3D menu's PINGUPEDIA page (PinguMenu3D.h): today's Slate screen (PinguUIPingupedia.cpp) as 3D - the plane, the two
// tabs as segment slabs, the list on the left as a dark well with a window of row slabs (the wheel, Up / Down and the stick
// scroll it; a slim scroll bar at its right), the selected entry's card on the right: its big icon (a textured quad), its
// name in 3D letters, its tags as pills, and - flat on the card's face (SPinguMenu3DOverlay) - what it does and its numbers in
// row-coloured stat strips. Flat text there because a paragraph in Text3D would be hundreds of glyph meshes rebuilt on every
// selection; the strips and the panel are still 3D, and the text follows the panel's turn. The fit check (every card inside its
// panel, PinguUI::FPingupediaStats) is made from the same layout, measured for every entry once.

#include "PinguMenu3D.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinHUD.h"
#include "PinguTraitTable.h"
#include "PinguTuning.h"
#include "PinguUI.h"
#include "PinguWoodStyle.h"
#include "Engine/Texture.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "Text3DComponent.h"

#define LOCTEXT_NAMESPACE "PinguMenu3D"

namespace PinguMenu3DPediaPrivate
{
	/** The layout (page units, as the Slate card's: a 1180 x 800 card). */
	constexpr float CardWidth = 1180.f;
	constexpr float BodyHeight = 640.f;
	constexpr float ListWidth = 360.f;
	constexpr float ListGap = 24.f;
	constexpr float WellPadding = 8.f;
	constexpr float RowHeight = 52.f;
	constexpr float RowGap = 4.f;
	constexpr float RowIcon = 40.f;
	constexpr float BigIcon = 112.f;
	constexpr float DetailPadding = 24.f;
	constexpr float TabWidth = 216.f;
	constexpr float StatHeight = 40.f;
	constexpr float StatPadding = 14.f;
	/** The room between a stat's name and its number (22 since 2026-10-26: at 12 KNOCKBACK TAKEN touched x0.52 on the screen; 30 made GIANT's card too tall). */
	constexpr float StatValueGap = 22.f;
	constexpr float StatGap = 12.f;
	constexpr float StatRowGap = 8.f;
	constexpr float DetailWidth = CardWidth - 80.f - ListWidth - ListGap - 2.f * DetailPadding;
	constexpr float DetailRoom = BodyHeight - (DetailPadding - 4.f) - DetailPadding;
	constexpr int32 NumPages = 2;
	constexpr int32 AbilitiesPage = 0;
	constexpr int32 TraitsPage = 1;
	constexpr float CardHeight = 24.f + 52.f + 16.f + BodyHeight + 20.f + 44.f + 28.f;
	/** The list's window: whole rows that fit the well. */
	constexpr int32 ListRows = static_cast<int32>((BodyHeight - 2.f * WellPadding + RowGap) / (RowHeight + RowGap));
	/** The parts of the card stand out of its well by this much. */
	constexpr float CardRelief = 3.f;

	static int32 NumEntries(int32 Page)
	{
		return Page == TraitsPage ? PinguNumTraits : static_cast<int32>(EPenguinAbility::Count) - 1;
	}

	static EPenguinAbility AbilityAt(int32 Entry)
	{
		return static_cast<EPenguinAbility>(FMath::Clamp(Entry, 0, NumEntries(AbilitiesPage) - 1) + 1);
	}

	static EPinguTrait TraitAt(int32 Entry)
	{
		return static_cast<EPinguTrait>(FMath::Clamp(Entry, 0, NumEntries(TraitsPage) - 1) + 1);
	}

	static FSlateFontInfo StatLabelFont() { return FPinguStyle::Title(16.f); }
	static FSlateFontInfo StatValueFont() { return FPinguStyle::Title(24.f); }
	static FSlateFontInfo ParagraphFont() { return FPinguStyle::Body(20.f); }

	static TSharedPtr<FSlateFontMeasure> Measurer()
	{
		if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
		{
			return nullptr;
		}
		return FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	}

	/** How wide a text is in a font (page units; 0 before Slate can measure). */
	static float TextWidth(const FText& Text, const FSlateFontInfo& Font)
	{
		const TSharedPtr<FSlateFontMeasure> Measure = Measurer();
		return Measure.IsValid() ? static_cast<float>(Measure->Measure(Text, Font).X) : 0.f;
	}

	/** A paragraph broken into lines of at most Width (the overlay draws exactly these lines); its height. */
	static FString WrapParagraph(const FString& Text, float Width, float& OutHeight)
	{
		const TSharedPtr<FSlateFontMeasure> Measure = Measurer();
		const FSlateFontInfo Font = ParagraphFont();
		const float LineHeight = Measure.IsValid() ? static_cast<float>(Measure->GetMaxCharacterHeight(Font)) : 26.f;
		TArray<FString> Words;
		Text.ParseIntoArray(Words, TEXT(" "), true);
		TArray<FString> Lines;
		FString Line;
		for (const FString& Word : Words)
		{
			const FString Tried = Line.IsEmpty() ? Word : Line + TEXT(" ") + Word;
			const float Tried_Width = Measure.IsValid() ? static_cast<float>(Measure->Measure(Tried, Font).X) : 8.f * Tried.Len();
			if (Tried_Width > Width && !Line.IsEmpty())
			{
				Lines.Add(Line);
				Line = Word;
			}
			else
			{
				Line = Tried;
			}
		}
		if (!Line.IsEmpty())
		{
			Lines.Add(Line);
		}
		OutHeight = LineHeight * FMath::Max(Lines.Num(), 1);
		return FString::Join(Lines, TEXT("\n"));
	}

	/** The rows of a stat grid: one or two lines each (a line whose name and number don't fit half a row gets a whole row). */
	static TArray<TArray<int32>> GridRows(const TArray<FPinguStatLine>& Lines, int32 Page, int32 Entry)
	{
		constexpr float HalfWidth = 0.5f * (DetailWidth - StatGap);
		TArray<TArray<int32>> Rows;
		bool bOpenHalf = false;
		for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
		{
			const FPinguStatLine& Line = Lines[LineIndex];
			const float Needed = 2.f * StatPadding + StatValueGap + TextWidth(PinguUI::PingupediaStatLabel(Line.Kind, Page, Entry, Line.bUpside), StatLabelFont())
				+ TextWidth(PinguUI::PingupediaFormatValue(Line.Kind, Line.Value), StatValueFont());
			if (Needed > HalfWidth)
			{
				Rows.Add({ LineIndex, INDEX_NONE });
				bOpenHalf = false;
			}
			else if (bOpenHalf)
			{
				Rows.Last().Add(LineIndex);
				bOpenHalf = false;
			}
			else
			{
				Rows.Add({ LineIndex });
				bOpenHalf = true;
			}
		}
		return Rows;
	}
}

// ============================================================================ building

void APinguMenu3D::AddEntryIcon(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, float Size, int32 PediaPageIndex, int32 Entry, float Protrude)
{
	using namespace PinguMenu3DPediaPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (PediaPageIndex == TraitsPage)
	{
		// A trait's hexagon badge as it is (its gold rim is part of the badge).
		const FSlateBrush* Brush = PenguinHUD ? PenguinHUD->GetTraitBrush(TraitAt(Entry)) : nullptr;
		if (UTexture* Texture = Brush ? Cast<UTexture>(Brush->GetResourceObject()) : nullptr)
		{
			AddDecoIcon(Parts, Parent, Pos, Size, Texture, FLinearColor::White, Protrude);
		}
		return;
	}
	// An ability's art on a dark disc in a ring of its colour (as above the heads): the ring is a disc in the colour under a
	// smaller dark disc.
	const EPenguinAbility Ability = AbilityAt(Entry);
	const float Rim = FMath::Max(2.f, FMath::RoundToFloat(Size * 0.05f));
	AddDecoSlab(Parts, Parent, Pos, FVector2D(Size, Size), 0.5f * Size, 4.f, UPenguinAbilityComponent::GetInfo(Ability).Color, Style.Ink, Protrude);
	AddDecoSlab(Parts, Parent, Pos, FVector2D(Size - 2.f * Rim, Size - 2.f * Rim), 0.5f * Size - Rim, 3.f, PinguMenu3D::WoodEdge * 2.f, PinguMenu3D::WoodEdge, Protrude + 1.f);
	const FSlateBrush* Brush = PenguinHUD ? PenguinHUD->GetAbilityBrush(Ability) : nullptr;
	if (UTexture* Texture = Brush ? Cast<UTexture>(Brush->GetResourceObject()) : nullptr)
	{
		AddDecoIcon(Parts, Parent, Pos, Size - 2.f * FMath::RoundToFloat(Size * 0.1f), Texture, FLinearColor::White, Protrude + 2.f);
	}
}

void APinguMenu3D::BuildPingupedia()
{
	using namespace PinguMenu3DPediaPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	TWeakObjectPtr<APinguMenu3D> WeakThis(this);
	constexpr float Inner = CardWidth - 80.f;
	constexpr float Left = -0.5f * Inner;
	constexpr float Right = 0.5f * Inner;
	BeginPage(Pingupedia, EPinguMenu3DPage::Pingupedia, FVector2D(CardWidth, CardHeight));
	auto Z = [](float FromTop) { return 0.5f * CardHeight - FromTop; };
	float Y = 24.f;

	// ---- the title, the tabs on the right: ABILITIES 16 | TRAITS 17
	AddLabel(Pingupedia, nullptr, FVector2D(Left, Z(Y + 26.f)), 42.f, [] { return LOCTEXT("PingupediaTitle3D", "PINGUPEDIA"); }, Style.Snow, PinguMenu3D::ETextAlign::Left, 0.f, true, 10.f);
	for (int32 PageIndex = 0; PageIndex < NumPages; ++PageIndex)
	{
		const FText TabText = PageIndex == TraitsPage
			? FText::Format(LOCTEXT("PingupediaTabTraits3D", "TRAITS {0}"), FText::AsNumber(NumEntries(TraitsPage)))
			: FText::Format(LOCTEXT("PingupediaTabAbilities3D", "ABILITIES {0}"), FText::AsNumber(NumEntries(AbilitiesPage)));
		const float X = Right - (NumPages - 1 - PageIndex) * (TabWidth + 8.f) - 0.5f * TabWidth;
		FButton& Tab = AddButton(Pingupedia, FName(*FString::Printf(TEXT("PediaTab%d"), PageIndex)), EPinguMenu3DKind::Segment, FVector2D(X, Z(Y + 26.f)), FVector2D(TabWidth, 44.f), 16.f,
			[TabText] { return TabText; }, [WeakThis, PageIndex] { if (WeakThis.IsValid()) { WeakThis->SelectPingupediaPage(PageIndex, WeakThis->PediaEntry, false); } });
		Tab.Selected = [WeakThis, PageIndex] { return WeakThis.IsValid() && WeakThis->PediaPage == PageIndex; };
	}
	Y += 52.f + 16.f;

	// ---- the list's well (a window of rows; the page that shows), its scroll bar; the detail's well
	const float WellTop = Z(Y);
	// (The list's well is one clean dark slab, not loose planks: the rows scroll over it - 2026-10-27, Linus: "make the pingupedia
	// scroll ... way cleaner".)
	AddDecoWood(Pingupedia, nullptr, FVector2D(Left + 0.5f * ListWidth, WellTop - 0.5f * BodyHeight), FVector2D(ListWidth, BodyHeight), 8.f, 4.f,
		PinguMenu3D::WoodWell, PinguMenu3D::WoodWellLip, 1.f, 0.35f, false);
	const float DetailLeft = Left + ListWidth + ListGap;
	const float DetailWellWidth = Right - DetailLeft;
	AddDecoWood(Pingupedia, nullptr, FVector2D(DetailLeft + 0.5f * DetailWellWidth, WellTop - 0.5f * BodyHeight), FVector2D(DetailWellWidth, BodyHeight), 8.f, 4.f,
		PinguMenu3D::WoodWell, PinguMenu3D::WoodWellLip, 1.f, 0.55f);
	Pingupedia.ListWindow = ListRows;
	Pingupedia.ListStep = RowHeight + RowGap;
	Pingupedia.ListTop = WellTop - WellPadding - 0.5f * RowHeight;
	Pingupedia.ListCount = NumEntries(AbilitiesPage);
	PediaListRoot = Pingupedia.Root;
	const float RowWidth = ListWidth - 2.f * WellPadding - 14.f;
	const float RowX = Left + WellPadding + 0.5f * RowWidth;
	for (int32 PageIndex = 0; PageIndex < NumPages; ++PageIndex)
	{
		for (int32 EntryIndex = 0; EntryIndex < NumEntries(PageIndex); ++EntryIndex)
		{
			const FText Name = PinguUI::PingupediaEntryName(PageIndex, EntryIndex);
			auto RowVisible = [WeakThis, PageIndex, EntryIndex]
			{
				if (!WeakThis.IsValid() || WeakThis->PediaPage != PageIndex)
				{
					return false;
				}
				const FPageParts& P = WeakThis->Pingupedia;
				return EntryIndex >= P.ListScroll && EntryIndex < P.ListScroll + P.ListWindow;
			};
			const int32 ButtonIndex = Pingupedia.Buttons.Num();
			{
				FButton& Row = AddButton(Pingupedia, FName(*FString::Printf(TEXT("Entry%d_%d"), PageIndex, EntryIndex)), EPinguMenu3DKind::ListRow,
					FVector2D(RowX, Pingupedia.ListTop - EntryIndex * Pingupedia.ListStep), FVector2D(RowWidth, RowHeight), 16.f, [Name] { return Name; },
					[WeakThis, PageIndex, EntryIndex] { if (WeakThis.IsValid()) { WeakThis->SelectPingupediaPage(PageIndex, EntryIndex, false); } });
				Row.LabelPad = 8.f + RowIcon + 12.f;
				PlaceLabel(Row, -0.5f * RowWidth + Row.LabelPad);
				Row.ListId = PageIndex;
				Row.ListIndex = EntryIndex;
				Row.Visible = RowVisible;
				Row.Selected = [WeakThis, PageIndex, EntryIndex] { return WeakThis.IsValid() && WeakThis->PediaPage == PageIndex && WeakThis->PediaEntry == EntryIndex; };
				// Hover or the keys on a row select it; Left / Right on the list switch the tabs (the row keeps its place).
				Row.OnHot = [WeakThis, PageIndex, EntryIndex]
				{
					if (WeakThis.IsValid() && WeakThis->PediaPage == PageIndex && WeakThis->PediaEntry != EntryIndex)
					{
						WeakThis->SelectPingupediaPage(PageIndex, EntryIndex, false);
					}
				};
				Row.OnStep = [WeakThis, PageIndex](int32 Direction)
				{
					if (WeakThis.IsValid())
					{
						WeakThis->SelectPingupediaPage((PageIndex + Direction + NumPages) % NumPages, WeakThis->PediaEntry, true);
					}
				};
			}
			const int32 FirstDeco = Pingupedia.Decos.Num();
			AddEntryIcon(Pingupedia, Pingupedia.Buttons[ButtonIndex].Body, FVector2D(-0.5f * RowWidth + 8.f + 0.5f * RowIcon, 0.f), RowIcon, PageIndex, EntryIndex, 1.f);
			for (int32 Index = FirstDeco; Index < Pingupedia.Decos.Num(); ++Index)
			{
				Pingupedia.Decos[Index].Visible = RowVisible;
			}
		}
	}
	// The scroll bar: a slim track down the well's right, a thumb as long as the window's share.
	const float TrackX = Left + ListWidth - WellPadding - 4.f;
	const float TrackHeight = BodyHeight - 2.f * WellPadding;
	PediaScrollTrack = AddDecoSlab(Pingupedia, nullptr, FVector2D(TrackX, WellTop - 0.5f * BodyHeight), FVector2D(6.f, TrackHeight), 3.f, 2.f,
		PinguMenu3D::WoodEdge * 2.f, PinguMenu3D::WoodEdge, 1.5f);
	PediaScrollThumb = AddDecoSlab(Pingupedia, nullptr, FVector2D(TrackX, WellTop - WellPadding - 0.5f * TrackHeight * 0.5f), FVector2D(8.f, 0.5f * TrackHeight), 4.f, 3.f,
		Style.Gold, PinguMenu3D::WoodEdge, 2.5f);
	Y += BodyHeight + 20.f;

	// ---- BACK
	AddButton(Pingupedia, TEXT("Back"), EPinguMenu3DKind::Ghost, FVector2D(Left + 100.f, Z(Y + 22.f)), FVector2D(200.f, 44.f), 16.f,
		[] { return LOCTEXT("PingupediaBack3D", "BACK"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_Back(); } });

	// The detail card's root: the well's inside, top left (the card's parts are dynamic: rebuilt per entry).
	PediaDetailRoot = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	PediaDetailRoot->SetupAttachment(Pingupedia.Root);
	PediaDetailRoot->RegisterComponent();
	PediaDetailRoot->SetRelativeLocation(FVector(-1.f, DetailLeft + DetailPadding, WellTop - (DetailPadding - 4.f)));
	EndStatic(Pingupedia);
	MeasurePediaCards();
}

namespace PinguMenu3DPediaPrivate
{
	/** What a card is made of, laid out from the top of the detail's inside (x right, y down): computed once for the measure
	 *  and once for the build. */
	struct FCardPlan
	{
		struct FParagraph
		{
			float Y = 0.f;
			float Height = 0.f;
			FString Lines;
		};
		struct FPillAt
		{
			float Y = 0.f;
			FText Text;
			FLinearColor Fill;
			FLinearColor TextColor;
		};
		struct FStatAt
		{
			float X = 0.f;
			float Y = 0.f;
			float Width = 0.f;
			FText Label;
			FText Value;
			FLinearColor ValueColor;
		};
		TArray<FParagraph> Paragraphs;
		TArray<FPillAt> Pills;
		TArray<FStatAt> Stats;
		TArray<FPillAt> Tags;
		float Height = 0.f;
	};

	static void PlanGrid(FCardPlan& Plan, const TArray<FPinguStatLine>& Lines, int32 Page, int32 Entry, const FLinearColor& ValueColor, float& Y)
	{
		const TArray<TArray<int32>> Rows = GridRows(Lines, Page, Entry);
		for (int32 RowIndex = 0; RowIndex < Rows.Num(); ++RowIndex)
		{
			const TArray<int32>& RowLines = Rows[RowIndex];
			const bool bWhole = RowLines.Num() == 2 && RowLines[1] == INDEX_NONE;
			Y += RowIndex > 0 ? StatRowGap : 0.f;
			for (int32 Column = 0; Column < (bWhole ? 1 : 2); ++Column)
			{
				if (!RowLines.IsValidIndex(Column) || !Lines.IsValidIndex(RowLines[Column]))
				{
					continue;
				}
				const FPinguStatLine& Line = Lines[RowLines[Column]];
				FCardPlan::FStatAt& Stat = Plan.Stats.AddDefaulted_GetRef();
				Stat.Width = bWhole ? DetailWidth : 0.5f * (DetailWidth - StatGap);
				Stat.X = Column == 0 ? 0.f : 0.5f * (DetailWidth + StatGap);
				Stat.Y = Y;
				Stat.Label = PinguUI::PingupediaStatLabel(Line.Kind, Page, Entry, Line.bUpside);
				Stat.Value = PinguUI::PingupediaFormatValue(Line.Kind, Line.Value);
				Stat.ValueColor = ValueColor;
			}
			Y += StatHeight;
		}
	}

	static FCardPlan PlanCard(const APenguinHUD* PenguinHUD, int32 Page, int32 Entry)
	{
		const FPinguStyle& Style = FPinguStyle::Get();
		FCardPlan Plan;
		float Y = BigIcon; // the icon, the name and the tags take the first BigIcon of height
		if (Page == TraitsPage)
		{
			const EPinguTrait Trait = TraitAt(Entry);
			const FPinguTraitInfo& Info = PinguTraits::GetInfo(Trait);
			Plan.Tags.Add({ 0.f, LOCTEXT("PingupediaTagTrait3D", "TRAIT"), Info.Color, Style.Ink });
			TArray<FPinguStatLine> Lines;
			PinguTraits::GetStatLines(Trait, Lines);
			TArray<FPinguStatLine> Ups;
			TArray<FPinguStatLine> Downs;
			for (const FPinguStatLine& Line : Lines)
			{
				(Line.bUpside ? Ups : Downs).Add(Line);
			}
			// Upside (green), then downside (red): a pill, the sentence, its numbers.
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const bool bUp = Side == 0;
				Y += 16.f;
				Plan.Pills.Add({ Y, bUp ? LOCTEXT("PingupediaUpside3D", "UPSIDE") : LOCTEXT("PingupediaDownside3D", "DOWNSIDE"), bUp ? Style.Green : Style.Red, bUp ? Style.Ink : Style.Snow });
				Y += 26.f + 8.f;
				FCardPlan::FParagraph& Paragraph = Plan.Paragraphs.AddDefaulted_GetRef();
				Paragraph.Y = Y;
				Paragraph.Lines = WrapParagraph(bUp ? Info.Upside : Info.Downside, DetailWidth - 8.f, Paragraph.Height);
				Y += Paragraph.Height;
				const TArray<FPinguStatLine>& SideLines = bUp ? Ups : Downs;
				if (SideLines.Num() > 0)
				{
					Y += 10.f;
					PlanGrid(Plan, SideLines, Page, Entry, bUp ? Style.Green : Style.RedHot, Y);
				}
			}
		}
		else
		{
			const EPenguinAbility Ability = AbilityAt(Entry);
			const FPenguinAbilityInfo& Info = UPenguinAbilityComponent::GetInfo(Ability);
			if (UPenguinAbilityComponent::IsDeadly(Ability))
			{
				Plan.Tags.Add({ 0.f, LOCTEXT("PingupediaTagDeadly3D", "KNOCKS OUT"), Style.Red, Style.Snow });
			}
			if (UPenguinAbilityComponent::IsKnockback(Ability))
			{
				Plan.Tags.Add({ 0.f, LOCTEXT("PingupediaTagKnockback3D", "KNOCKBACK"), Style.Ice, Style.Ink });
			}
			TArray<FPinguStatLine> Lines;
			PinguTuning::GetAbilityStats(Ability, PinguUI::PingupediaFindMovement(PenguinHUD), Lines);
			Y += 20.f;
			FCardPlan::FParagraph& Paragraph = Plan.Paragraphs.AddDefaulted_GetRef();
			Paragraph.Y = Y;
			Paragraph.Lines = WrapParagraph(Info.Description, DetailWidth - 8.f, Paragraph.Height);
			Y += Paragraph.Height;
			Y += 20.f;
			PlanGrid(Plan, Lines, Page, Entry, Style.Snow, Y);
		}
		Plan.Height = Y;
		return Plan;
	}
}

void APinguMenu3D::MeasurePediaCards()
{
	using namespace PinguMenu3DPediaPrivate;
	// Every card must fit the detail panel without scrolling: all of them measured once (the menu screenshots log the tallest);
	// every stat kind has a name of its own.
	PinguUI::FPingupediaStats& PediaStats = PinguUI::MutablePingupediaStats();
	PediaStats.Abilities = NumEntries(AbilitiesPage);
	PediaStats.Traits = NumEntries(TraitsPage);
	PediaStats.CardRoom = DetailRoom;
	PediaStats.TallestCard = 0.f;
	PediaStats.RawLabels = 0;
	PediaStats.RawLabelNames.Reset();
	for (int32 KindIndex = 0; KindIndex < static_cast<int32>(EPinguStatKind::Count); ++KindIndex)
	{
		bool bRaw = false;
		PinguUI::PingupediaStatLabel(static_cast<EPinguStatKind>(KindIndex), AbilitiesPage, 0, true, &bRaw);
		if (bRaw)
		{
			++PediaStats.RawLabels;
			PediaStats.RawLabelNames += FString::Printf(TEXT("%s%s"), PediaStats.RawLabelNames.IsEmpty() ? TEXT("") : TEXT(" "), PinguTraits::GetStatKindName(static_cast<EPinguStatKind>(KindIndex)));
		}
	}
	if (!Measurer().IsValid())
	{
		return;
	}
	for (int32 PageIndex = 0; PageIndex < NumPages; ++PageIndex)
	{
		for (int32 EntryIndex = 0; EntryIndex < NumEntries(PageIndex); ++EntryIndex)
		{
			TArray<FPinguStatLine> Lines;
			if (PageIndex == TraitsPage)
			{
				PinguTraits::GetStatLines(TraitAt(EntryIndex), Lines);
			}
			else
			{
				PinguTuning::GetAbilityStats(AbilityAt(EntryIndex), nullptr, Lines);
			}
			for (const FPinguStatLine& Line : Lines)
			{
				bool bRaw = false;
				PinguUI::PingupediaStatLabel(Line.Kind, PageIndex, EntryIndex, Line.bUpside, &bRaw);
				if (bRaw)
				{
					++PediaStats.RawLabels;
					PediaStats.RawLabelNames += FString::Printf(TEXT("%s%s"), PediaStats.RawLabelNames.IsEmpty() ? TEXT("") : TEXT(" "), PinguTraits::GetStatKindName(Line.Kind));
				}
			}
			const FCardPlan Plan = PlanCard(HUD.Get(), PageIndex, EntryIndex);
			if (Plan.Height > PediaStats.TallestCard)
			{
				PediaStats.TallestCard = Plan.Height;
				PediaStats.TallestName = PinguUI::PingupediaEntryName(PageIndex, EntryIndex).ToString();
			}
		}
	}
	UE_LOG(LogPingu, Log, TEXT("[MENU3D] PINGUPEDIA cards measured: tallest %.0f of %.0f (%s), %d raw labels"), PediaStats.TallestCard, PediaStats.CardRoom,
		*PediaStats.TallestName, PediaStats.RawLabels);
}

void APinguMenu3D::RebuildPediaDetail(FPageParts& Parts)
{
	using namespace PinguMenu3DPediaPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	if (!PediaDetailRoot)
	{
		return;
	}
	ClearRows(Parts);
	PediaShownPage = PediaPage;
	PediaShownEntry = PediaEntry;
	const FCardPlan Plan = PlanCard(HUD.Get(), PediaPage, PediaEntry);
	USceneComponent* Root = PediaDetailRoot;
	// The icon, the name and the tags.
	AddEntryIcon(Parts, Root, FVector2D(0.5f * BigIcon, -0.5f * BigIcon), BigIcon, PediaPage, PediaEntry, CardRelief);
	const float NameX = BigIcon + 24.f;
	const FText Name = PinguUI::PingupediaEntryName(PediaPage, PediaEntry);
	AddLabel(Parts, Root, FVector2D(NameX, -40.f), 35.f, [Name] { return Name; }, Style.Snow, PinguMenu3D::ETextAlign::Left, DetailWidth - NameX, true, CardRelief);
	float TagX = NameX;
	for (const FCardPlan::FPillAt& Tag : Plan.Tags)
	{
		TagX += AddPill(Parts, Root, FVector2D(TagX, -84.f), 30.f, 15.f, Tag.Text, PinguMenu3D::WoodRow * 0.9f, PinguMenu3D::TagTextColor(Tag.Fill), CardRelief) + 8.f;
	}
	// The pills, the paragraphs (flat on the card) and the stat strips (slabs with flat text).
	for (const FCardPlan::FPillAt& Pill : Plan.Pills)
	{
		AddPill(Parts, Root, FVector2D(0.f, -(Pill.Y + 13.f)), 28.f, 14.f, Pill.Text, PinguMenu3D::WoodRow * 0.9f, PinguMenu3D::TagTextColor(Pill.Fill), CardRelief);
	}
	for (const FCardPlan::FParagraph& Paragraph : Plan.Paragraphs)
	{
		const FString Lines = Paragraph.Lines;
		// (Style 5, 2026-11-07: each paragraph on its own plank, so no line runs across the seams of the detail's loose planks;
		// a tall paragraph takes the banner plank (one board), docs/UI_STYLE.md.)
		{
			constexpr float PadX = 12.f;
			constexpr float PadY = 6.f;
			const FVector2D PlankSize(DetailWidth - 8.f + 2.f * PadX, Paragraph.Height + 2.f * PadY);
			const int32 Plank = AddDecoWood(Parts, Root, FVector2D(0.5f * (DetailWidth - 8.f), -(Paragraph.Y + 0.5f * Paragraph.Height)), PlankSize, 6.f, 3.f,
				PinguMenu3D::WoodRow, PinguMenu3D::WoodRowLip, 0.5f * CardRelief, 0.6f, false);
			if (PlankSize.Y > 1.25f * PinguWood::BodyDesign(PinguWood::EClass::Row))
			{
				Parts.Decos[Plank].Slab.SetWoodImage(static_cast<uint8>(PinguWood::ECut::Banner), static_cast<uint8>(PinguWood::EState::Normal));
			}
		}
		AddFlat(Parts, Root, 0.f, -Paragraph.Y, DetailWidth - 8.f, Paragraph.Height, [Lines] { return FText::FromString(Lines); }, false, 20.f, Style.Snow, 0);
	}
	for (const FCardPlan::FStatAt& Stat : Plan.Stats)
	{
		const int32 Strip = AddDecoWood(Parts, Root, FVector2D(Stat.X + 0.5f * Stat.Width, -(Stat.Y + 0.5f * StatHeight)), FVector2D(Stat.Width, StatHeight), 6.f, 3.f,
			PinguMenu3D::WoodRow, PinguMenu3D::WoodRowLip, CardRelief, 0.7f);
		USceneComponent* StripRoot = Parts.Decos[Strip].Root;
		const FText Label = Stat.Label;
		const FText Value = Stat.Value;
		AddFlat(Parts, StripRoot, -0.5f * Stat.Width + StatPadding, 0.5f * StatHeight, Stat.Width - 2.f * StatPadding, StatHeight, [Label] { return Label; }, true, 16.f, Style.Ice, 0);
		AddFlat(Parts, StripRoot, -0.5f * Stat.Width + StatPadding, 0.5f * StatHeight, Stat.Width - 2.f * StatPadding, StatHeight, [Value] { return Value; }, true, 24.f, Stat.ValueColor, 2);
	}
	UE_LOG(LogPingu, Log, TEXT("[MENU3D] PINGUPEDIA card %d/%d built: %.0f high (%d stats, %d paragraphs)"), PediaPage, PediaEntry, Plan.Height, Plan.Stats.Num(), Plan.Paragraphs.Num());
}

void APinguMenu3D::UpdatePingupedia(FPageParts& Parts)
{
	using namespace PinguMenu3DPediaPrivate;
	if (PediaShownPage != PediaPage || PediaShownEntry != PediaEntry)
	{
		RebuildPediaDetail(Parts);
	}
	// The scroll bar's thumb: as long as the window's share of the list, where the window is.
	if (Parts.Decos.IsValidIndex(PediaScrollThumb) && Parts.Decos.IsValidIndex(PediaScrollTrack) && Parts.Decos[PediaScrollThumb].Root && Parts.Decos[PediaScrollTrack].Root)
	{
		FDeco& Thumb = Parts.Decos[PediaScrollThumb];
		const FDeco& Track = Parts.Decos[PediaScrollTrack];
		const float TrackHeight = Track.Slab.Height;
		const float Share = FMath::Clamp(static_cast<float>(Parts.ListWindow) / FMath::Max(Parts.ListCount, 1), 0.1f, 1.f);
		const float ThumbHeight = FMath::Max(TrackHeight * Share, 24.f);
		const float Travel = TrackHeight - ThumbHeight;
		const float At = Parts.ListCount > Parts.ListWindow ? static_cast<float>(Parts.ListScroll) / (Parts.ListCount - Parts.ListWindow) : 0.f;
		const float TopZ = static_cast<float>(Track.Root->GetRelativeLocation().Z) + 0.5f * TrackHeight;
		const FVector Want(Thumb.Root->GetRelativeLocation().X, Track.Root->GetRelativeLocation().Y, TopZ - At * Travel - 0.5f * ThumbHeight);
		if (!Want.Equals(Thumb.Root->GetRelativeLocation(), 0.1f) || !FMath::IsNearlyEqual(Thumb.Slab.Height, ThumbHeight, 0.5f))
		{
			Thumb.Root->SetRelativeLocation(Want);
			Thumb.Slab.SetShape(8.f, ThumbHeight, 4.f, 3.f);
		}
	}
}

void APinguMenu3D::SelectPingupediaPage(int32 InPage, int32 Entry, bool bFocus)
{
	using namespace PinguMenu3DPediaPrivate;
	const int32 NewPage = FMath::Clamp(InPage, 0, NumPages - 1);
	const int32 NewEntry = FMath::Clamp(Entry, 0, NumEntries(NewPage) - 1);
	const bool bPageChanged = NewPage != PediaPage;
	PediaPage = NewPage;
	PediaEntry = NewEntry;
	if (!Pingupedia.bBuilt)
	{
		return;
	}
	Pingupedia.ListCount = NumEntries(NewPage);
	Pingupedia.ActiveList = NewPage;
	// The list shows the selected row (the new page's rows take the window).
	ScrollListTo(Pingupedia, NewEntry);
	if (Page == EPinguMenu3DPage::Pingupedia && (bFocus || bPageChanged))
	{
		FocusButton(FName(*FString::Printf(TEXT("Entry%d_%d"), NewPage, NewEntry)));
	}
}

bool APinguMenu3D::GetPingupediaView(int32& OutPage, int32& OutEntry) const
{
	OutPage = PediaPage;
	OutEntry = PediaEntry;
	return Pingupedia.bBuilt && PediaEntry >= Pingupedia.ListScroll && PediaEntry < Pingupedia.ListScroll + Pingupedia.ListWindow;
}

#undef LOCTEXT_NAMESPACE
