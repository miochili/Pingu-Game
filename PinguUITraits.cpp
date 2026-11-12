// Pingu - the traits' reveal (Traits mode): at the start of a round a card above every penguin shows its new trait (its
// badge and its name), then flies into it - and the penguin changes (its look: PenguinTraitLooks.cpp).
//
// Painted from the replicated trait state (UPenguinTraitComponent: the trait and the flow time its reveal starts) and the
// flow clock on every machine: no multicasts, a LAN client sees the same reveal at the same moment, one that joins in the
// middle of it sees the rest, and one that joins later sees no cards (the look goes on quietly). No trait icon shows
// outside the reveal.
//
// Timeline (seconds after the trait's RevealStart; UPenguinTraitComponent's constants):
//   0 ... 3.0      the cards pop in (0.2 s each, one after the other from left to right) and show
//   3.0 ... 3.45   each card flies into its penguin (eased in, on a little arc, shrinking), a flash ring where it lands
//   3.45 ...       the penguin changes (PenguinTraitLooks.cpp); cards further right fly and land a few hundredths later
// The cards stand above the penguins' ability icons; one that would overlap a card further left moves up a row (two rows
// at most). Text never scales: only the badge pops and flies, the name fades.
// With the round's one trait (everybody the same, the normal case) there are no cards: only the trait's logo pops in big near
// the top, shows until 3.0 s and flies into the top-left corner icon by 3.45 s (PaintSharedBanner, SPinguTraitCorner).

#include "PinguUI.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PenguinMovementComponent.h"
#include "PenguinTraitComponent.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "PinguWoodStyle.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "Styling/WidgetStyle.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Layout/SBox.h"

#define LOCTEXT_NAMESPACE "PinguUI"

namespace PinguUITraitsPrivate
{
	static PinguUI::FTraitRevealStats& MutableRevealStats()
	{
		static PinguUI::FTraitRevealStats Stats;
		return Stats;
	}

	/** Badge size of a local player's card and of everybody else's (Slate units = pixels at 1080p). */
	constexpr float LocalBadge = 72.f;
	constexpr float OtherBadge = 56.f;
	/** The name's font size on a local player's card and on everybody else's. */
	constexpr float LocalFont = 20.f;
	constexpr float OtherFont = 17.f;
	/** Room between the ability icons and the card, between the badge and its name, and between two rows of cards. */
	constexpr float CardGap = 8.f;
	constexpr float NameGap = 1.f;
	constexpr float RowGap = 6.f;
	constexpr int32 MaxRows = 2;
	/** Cards keep this far inside the picture, this far from an off-screen marker (and its team ring), and this far
	 *  apart along an edge. */
	constexpr float ScreenMargin = 8.f;
	constexpr float MarkerClearance = 6.f;
	constexpr float EdgeGap = 6.f;
	/** The name pill's padding. */
	constexpr float PillPadX = 10.f;
	constexpr float PillPadY = 2.f;
	/** A card pops in this long, this much after the card left of it. */
	constexpr float PopTime = 0.2f;
	constexpr float PopStagger = 0.04f;
	/** Each card flies this much after the one left of it (PenguinTraitLooks.cpp changes the penguins in the same order). */
	constexpr float FlyStagger = 0.03f;
	/** The badge's size where it lands, the flash ring's life and largest size there. */
	constexpr float LandSize = 14.f;
	constexpr float FlashTime = 0.32f;
	constexpr float FlashSize = 120.f;
	/** How high the flight's arc rises over the straight line (Slate units). */
	constexpr float ArcHeight = 46.f;

	/** The round's one trait at round start (card 2026-11-07_trait-logo-wood-tiling; Linus: "make the trait that shows have just
	 *  the trait logo no planks or text with it"): only its badge, LogoSize big at its biggest, its centre LogoHeight of the
	 *  picture's height from the top; it pops in over LogoPopTime, then flies into the top-left corner icon (SPinguTraitCorner). */
	constexpr float LogoSize = 208.f;
	constexpr float LogoHeight = 0.25f;
	constexpr float LogoPopTime = 0.3f;

	/** The corner icon's size in Slate units (pixels at 1080p; the UI scale follows the window) and its distance from the corner. */
	constexpr float CornerSize = 72.f;
	constexpr float CornerMargin = 20.f;
	/** A trait of the viewer's own fades in this long once the penguins have changed (the cards' flight is over). */
	constexpr float CornerFadeIn = 0.35f;

	/** Where the corner icon stands, in absolute (desktop) space, painted or not: the logo's flight ends exactly on it. Every
	 *  machine paints its own corner, so this is local UI state (no replication). */
	struct FCornerPlace
	{
		FSlateRect Absolute;
		bool bKnown = false;
	};
	static FCornerPlace& CornerPlace()
	{
		static FCornerPlace Place;
		return Place;
	}

	/** 0 -> 1 with a little overshoot (a card popping in). */
	static float BackOut(float P)
	{
		P = FMath::Clamp(P, 0.f, 1.f);
		constexpr float C1 = 1.9f;
		constexpr float C3 = C1 + 1.f;
		return 1.f + C3 * FMath::Pow(P - 1.f, 3.f) + C1 * FMath::Pow(P - 1.f, 2.f);
	}

	/** The top of a penguin's head standing up (as the overhead icons stand on it). */
	static FVector StandingHeadTop(const APenguinCharacter* Penguin)
	{
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 56.f;
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const float Standing = Move ? FMath::Max(Move->GetStandingHalfHeight(), HalfHeight) : HalfHeight;
		return Penguin->GetActorLocation() + FVector(0.f, 0.f, 2.f * Standing - HalfHeight);
	}

	/** Rank of the penguin among those whose reveal starts with its own, left to right in the world (0 = leftmost). */
	static int32 GetRank(const APenguinCharacter* Penguin, float RevealStart, const TArray<const APenguinCharacter*>& All)
	{
		const double X = Penguin->GetActorLocation().X;
		int32 Rank = 0;
		for (const APenguinCharacter* Other : All)
		{
			if (Other == Penguin || FMath::Abs(Other->GetTraits()->GetState().RevealStart - RevealStart) > 0.05f)
			{
				continue;
			}
			const double OtherX = Other->GetActorLocation().X;
			Rank += (OtherX < X || (OtherX == X && Other->GetPlayerSlot() < Penguin->GetPlayerSlot())) ? 1 : 0;
		}
		return Rank;
	}
}

/**
 * The trait cards above the penguins and their flight into them (see the top of this file). Painted directly (no child
 * widgets) over the whole game HUD; never takes a click.
 */
class SPinguTraitReveal : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguTraitReveal)
	{
		_Visibility = EVisibility::HitTestInvisible;
	}
		SLATE_ARGUMENT(TWeakObjectPtr<APenguinHUD>, HUD)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		HUD = InArgs._HUD;
	}

protected:
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override { return FVector2D::ZeroVector; }
	virtual bool ComputeVolatility() const override { return true; }
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	/** The screen's edge a penguin out of the picture is beyond (its marker sits there). */
	enum class EEdge : uint8
	{
		Left,
		Right,
		Top,
		Bottom,
	};

	/** One penguin's card this frame. */
	struct FCard
	{
		const APenguinCharacter* Penguin = nullptr;
		EPinguTrait Trait = EPinguTrait::None;
		float Age = 0.f;
		int32 Rank = 0;
		bool bLocal = false;
		/** Where the card's bottom middle wants to stand (above the icons) and where the card flies to: the penguin's chest,
		 *  or its marker at the screen's edge (Slate units). */
		FVector2f Anchor = FVector2f::ZeroVector;
		FVector2f Target = FVector2f::ZeroVector;
		float Badge = 0.f;
		float Font = 0.f;
		FString Name;
		FVector2f NameSize = FVector2f::ZeroVector;
		/** The card's size, where it stands (its rectangle) and its row above the icons (0 = right above them). */
		FVector2f Size = FVector2f::ZeroVector;
		FSlateRect Rect;
		int32 Row = 0;
		/** Its penguin is out of the picture: the card stands at that edge of the screen, next to the penguin's marker
		 *  (SPinguOverheadAbilities' off-screen markers). */
		bool bEdge = false;
		EEdge Edge = EEdge::Left;
	};

	/** A circle of Diameter round Center: Fill inside, a ring of Ring width in RingColor (0 / transparent: none). */
	void PaintCircle(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center, float Diameter,
		float Ring, const FLinearColor& RingColor, const FLinearColor& Fill) const;
	/** The text's width in a font (measured once per name and size). */
	FVector2f MeasureName(const FString& Name, const FSlateFontInfo& Font, float FontSize) const;
	/** Deadly Edges (the round's trait): a thin red line where each deadly side is, the world beyond it tinted red. */
	int32 PaintDeadlyEdges(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const APlayerController* Viewer,
		const FLinearColor& Fade) const;
	/** The round's one trait (everybody's): only its logo (no plank, no words; card 2026-11-07_trait-logo-wood-tiling) instead of
	 *  a card per penguin; it pops in big near the top with the reveal, shows for CardsTime, then flies into the top-left corner
	 *  icon (SPinguTraitCorner), landing at TransformAt as the penguins change. */
	int32 PaintSharedBanner(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, EPinguTrait Trait, float Age,
		const FWidgetStyle& InWidgetStyle, const FLinearColor& Fade) const;

	TWeakObjectPtr<APenguinHUD> HUD;
	/** A circle brush of 100 units (drawn scaled to any size). */
	mutable FSlateRoundedBoxBrush CircleBrush{ FLinearColor::White, 50.f, FLinearColor::White, 2.f };
	mutable FSlateRoundedBoxBrush PillBrush{ FLinearColor::White, 10.f, FLinearColor::White, 2.f };
	mutable TMap<FString, FVector2f> NameSizes;
};

void SPinguTraitReveal::PaintCircle(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center,
	float Diameter, float Ring, const FLinearColor& RingColor, const FLinearColor& Fill) const
{
	if (Diameter < 0.5f)
	{
		return;
	}
	const float Scale = Diameter / 100.f;
	const FVector2f Box(100.f, 100.f);
	CircleBrush.OutlineSettings.Color = FSlateColor(RingColor);
	CircleBrush.OutlineSettings.Width = Ring > 0.f ? Ring / Scale : 0.f;
	FSlateDrawElement::MakeBox(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(Box, FSlateLayoutTransform(Scale, Center - Box * (0.5f * Scale))),
		&CircleBrush, ESlateDrawEffect::None, Fill);
}

FVector2f SPinguTraitReveal::MeasureName(const FString& Name, const FSlateFontInfo& Font, float FontSize) const
{
	const FString Key = FString::Printf(TEXT("%s|%.0f"), *Name, FontSize);
	if (const FVector2f* Known = NameSizes.Find(Key))
	{
		return *Known;
	}
	FVector2f Size(Name.Len() * FontSize * 0.6f, FontSize * 1.3f);
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
	{
		const FVector2f Measured = FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Name, Font);
		Size = FVector2f(Measured.X, FMath::Max(Measured.Y, FontSize * 1.2f));
	}
	NameSizes.Add(Key, Size);
	return Size;
}

int32 SPinguTraitReveal::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace PinguUITraitsPrivate;
	PinguUI::FTraitRevealStats& Stats = MutableRevealStats();
	Stats.Cards = 0;
	Stats.Flying = 0;
	Stats.Flashes = 0;
	Stats.Rows = 0;
	Stats.bOverlap = false;
	Stats.EdgeCards = 0;
	Stats.Markers = 0;
	Stats.OverlapPairs = 0;
	Stats.CardsOverMarkers = 0;
	Stats.CardsOutside = 0;
	Stats.Names.Reset();
	Stats.Frame = GFrameCounter;

	Stats.bBanner = false;
	Stats.BannerAge = -1.f;
	Stats.BannerName.Reset();
	Stats.BannerLines = 0;
	Stats.bBannerOutside = false;
	Stats.EdgeLines = 0;
	Stats.EdgeLeftX = -1.f;
	Stats.EdgeRightX = -1.f;

	const APenguinHUD* PenguinHUD = HUD.Get();
	const UWorld* World = PenguinHUD ? PenguinHUD->GetWorld() : nullptr;
	const APlayerController* Viewer = World ? World->GetFirstPlayerController() : nullptr;
	if (!Viewer)
	{
		return LayerId;
	}
	// 2026-11-07 (shared-traits): Deadly Edges' red sides; and with the round's one trait (everybody's) one banner instead of a
	// card per penguin (a test that deals different traits by slot still gets the cards).
	const APenguinGameState* SharedState = APenguinGameState::Get(World);
	const EPinguTrait RoundTrait = SharedState ? SharedState->GetRoundTrait() : EPinguTrait::None;
	const int32 BaseLayer = PaintDeadlyEdges(OutDrawElements, AllottedGeometry, LayerId, Viewer, InWidgetStyle.GetColorAndOpacityTint());
	// Every penguin whose reveal is on now (it has a trait with a reveal, it is in, it shows).
	TArray<const APenguinCharacter*> Revealing;
	float SharedAge = -1.e6f;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		const APenguinCharacter* Penguin = *It;
		const UPenguinTraitComponent* Traits = IsValid(Penguin) ? Penguin->GetTraits() : nullptr;
		if (Traits && Traits->HasAnyTrait() && Traits->GetState().RevealStart >= 0.f)
		{
			Revealing.Add(Penguin);
			if (Traits->GetTrait() == RoundTrait)
			{
				SharedAge = FMath::Max(SharedAge, Traits->GetRevealAge());
			}
		}
	}
	if (PinguTraits::IsReal(RoundTrait))
	{
		return SharedAge > -1.e5f ? PaintSharedBanner(OutDrawElements, AllottedGeometry, BaseLayer, RoundTrait, SharedAge, InWidgetStyle,
			InWidgetStyle.GetColorAndOpacityTint()) : BaseLayer;
	}
	LayerId = BaseLayer;
	if (Revealing.Num() == 0)
	{
		return LayerId;
	}
	// The local players' own penguins (bigger cards, seen through their own view).
	TMap<const APenguinCharacter*, const APlayerController*> LocalPenguins;
	if (const UGameInstance* GameInstance = World->GetGameInstance())
	{
		for (const ULocalPlayer* LocalPlayer : GameInstance->GetLocalPlayers())
		{
			const APlayerController* Controller = LocalPlayer ? LocalPlayer->GetPlayerController(World) : nullptr;
			if (const APenguinCharacter* Penguin = Controller ? Cast<APenguinCharacter>(Controller->GetPawn()) : nullptr)
			{
				LocalPenguins.Add(Penguin, Controller);
			}
		}
	}

	const float PixelsPerUnit = AllottedGeometry.GetAccumulatedLayoutTransform().GetScale();
	const float UnitsPerPixel = PixelsPerUnit > UE_KINDA_SMALL_NUMBER ? 1.f / PixelsPerUnit : 1.f;
	const FLinearColor Fade = InWidgetStyle.GetColorAndOpacityTint();
	const FPinguStyle& Style = FPinguStyle::Get();
	constexpr float FlyAt = UPenguinTraitComponent::CardsTime;
	constexpr float LandAt = UPenguinTraitComponent::TransformAt;

	// ---- the cards of this reveal and where they want to stand
	const FVector2f Area = AllottedGeometry.GetLocalSize();
	// The off-screen markers (SPinguOverheadAbilities::PaintOffscreenMarkers): every penguin in the game out of the picture
	// has one at the screen's edge. No card may cover one.
	constexpr float MarkerSlack = 28.f; // (the markers' own slack: a penguin this far past the edge still counts as in)
	const float MarkerInset = SPinguOverheadAbilities::MarkerEdge + SPinguOverheadAbilities::MarkerSize * 0.5f;
	const float MarkerReach = SPinguOverheadAbilities::MarkerSize * 0.5f + SPinguOverheadAbilities::TeamRingGap + SPinguOverheadAbilities::TeamRingWidth
		+ MarkerClearance;
	const auto FindMarker = [&](const APenguinCharacter* Penguin, FVector2f& OutCentre, EEdge& OutEdge) -> bool
	{
		FVector2D Pixels;
		if (!Viewer->ProjectWorldLocationToScreen(Penguin->GetActorLocation(), Pixels, false))
		{
			return false;
		}
		const FVector2f Where(static_cast<float>(Pixels.X) * UnitsPerPixel, static_cast<float>(Pixels.Y) * UnitsPerPixel);
		if (Where.X >= -MarkerSlack && Where.X <= Area.X + MarkerSlack && Where.Y >= -MarkerSlack && Where.Y <= Area.Y + MarkerSlack)
		{
			return false; // in the picture: no marker
		}
		OutCentre = FVector2f(FMath::Clamp(Where.X, MarkerInset, Area.X - MarkerInset), FMath::Clamp(Where.Y, MarkerInset, Area.Y - MarkerInset));
		const float OutX = FMath::Max(-Where.X, Where.X - Area.X);
		const float OutY = FMath::Max(-Where.Y, Where.Y - Area.Y);
		OutEdge = OutX >= OutY ? (Where.X < 0.f ? EEdge::Left : EEdge::Right) : (Where.Y < 0.f ? EEdge::Top : EEdge::Bottom);
		return true;
	};
	TArray<FSlateRect> Markers;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		const APenguinCharacter* Penguin = *It;
		FVector2f Centre;
		EEdge Edge;
		if (IsValid(Penguin) && !Penguin->IsKnockedOut() && !Penguin->IsHidden() && FindMarker(Penguin, Centre, Edge))
		{
			Markers.Add(FSlateRect(Centre.X - MarkerReach, Centre.Y - MarkerReach, Centre.X + MarkerReach, Centre.Y + MarkerReach));
		}
	}

	TArray<FCard> Cards;
	for (const APenguinCharacter* Penguin : Revealing)
	{
		const UPenguinTraitComponent* Traits = Penguin->GetTraits();
		if (Penguin->IsKnockedOut() || Penguin->IsHidden())
		{
			continue;
		}
		const float RevealStart = Traits->GetState().RevealStart;
		const float Age = Traits->GetRevealAge();
		const int32 Rank = GetRank(Penguin, RevealStart, Revealing);
		const float Landed = LandAt + Rank * FlyStagger;
		if (Age < 0.f || Age > Landed + FlashTime)
		{
			continue; // not yet (the round's banner), or over: no trait icon outside the reveal
		}
		const APlayerController* const* LocalController = LocalPenguins.Find(Penguin);
		const APlayerController* Controller = LocalController ? *LocalController : Viewer;
		FVector2D HeadPixels;
		FVector2D ChestPixels;
		if (!Controller->ProjectWorldLocationToScreen(StandingHeadTop(Penguin) + FVector(0.f, 0.f, SPinguOverheadAbilities::HeadClearance), HeadPixels, false)
			|| !Controller->ProjectWorldLocationToScreen(Penguin->GetAimOrigin(), ChestPixels, false))
		{
			continue;
		}
		FCard& Card = Cards.AddDefaulted_GetRef();
		Card.Penguin = Penguin;
		Card.Trait = Traits->GetTrait();
		Card.Age = Age;
		Card.Rank = Rank;
		Card.bLocal = LocalController != nullptr;
		Card.Badge = Card.bLocal ? LocalBadge : OtherBadge;
		Card.Font = Card.bLocal ? LocalFont : OtherFont;
		Card.Name = FString(Traits->GetInfo().Name).ToUpper();
		Card.NameSize = MeasureName(Card.Name, FPinguStyle::Title(Card.Font, 2), Card.Font);
		Card.Size = FVector2f(FMath::Max(Card.Badge, Card.NameSize.X + 2.f * PillPadX), Card.Badge + NameGap + Card.NameSize.Y + 2.f * PillPadY);
		// Above the penguin's ability icons (they stand on the head point; a team match rings them).
		const float IconScale = Card.bLocal ? 1.f : SPinguOverheadAbilities::OtherIconScale;
		const float IconsHigh = SPinguOverheadAbilities::GetIconsHeight(IconScale, PenguinHUD->GetPenguinTeamRing(Penguin) >= 0);
		Card.Anchor = FVector2f(static_cast<float>(HeadPixels.X) * UnitsPerPixel, static_cast<float>(HeadPixels.Y) * UnitsPerPixel - IconsHigh - CardGap);
		Card.Target = FVector2f(static_cast<float>(ChestPixels.X) * UnitsPerPixel, static_cast<float>(ChestPixels.Y) * UnitsPerPixel);
		// Out of the picture: the card goes to that edge, and flies to the penguin's marker there.
		FVector2f MarkerCentre;
		if (FindMarker(Penguin, MarkerCentre, Card.Edge))
		{
			Card.bEdge = true;
			Card.Target = MarkerCentre;
			// (A column at the left / right edge keeps the cards' height above the heads; a row at the top / bottom lines
			// them up by where the markers are.)
			Card.Anchor.X = MarkerCentre.X;
		}
		else if (Area.X > 2.f * Card.Size.X && Area.Y > 3.f * Card.Size.Y)
		{
			// At the screen's edge but still in the picture: kept inside it.
			Card.Anchor.X = FMath::Clamp(Card.Anchor.X, Card.Size.X * 0.5f + ScreenMargin, Area.X - Card.Size.X * 0.5f - ScreenMargin);
			Card.Anchor.Y = FMath::Clamp(Card.Anchor.Y, Card.Size.Y + ScreenMargin, Area.Y - ScreenMargin);
		}
	}
	if (Cards.Num() == 0)
	{
		return LayerId;
	}

	// ---- layout. First the edge cards: along each edge (just inside the markers), in the order of their penguins
	// there, each one clear of the one before; a column / row that does not fit gets a second one further in.
	const float EdgeInset = SPinguOverheadAbilities::MarkerEdge + SPinguOverheadAbilities::MarkerSize + SPinguOverheadAbilities::TeamRingGap
		+ SPinguOverheadAbilities::TeamRingWidth + MarkerClearance;
	TArray<FSlateRect> Placed;
	float LeftColumn = 0.f; // how far in the left / right columns reach (the top / bottom rows stay out of them)
	float RightColumn = Area.X;
	for (const EEdge Edge : { EEdge::Left, EEdge::Right, EEdge::Top, EEdge::Bottom })
	{
		TArray<FCard*> Line;
		for (FCard& Card : Cards)
		{
			if (Card.bEdge && Card.Edge == Edge)
			{
				Line.Add(&Card);
			}
		}
		if (Line.Num() == 0)
		{
			continue;
		}
		const bool bColumn = Edge == EEdge::Left || Edge == EEdge::Right;
		// Along the edge: Y for a column, X for a row.
		Line.Sort([bColumn](const FCard& A, const FCard& B) { return bColumn ? A.Anchor.Y < B.Anchor.Y : A.Anchor.X < B.Anchor.X; });
		const float From = bColumn ? ScreenMargin : LeftColumn + EdgeGap;
		const float To = bColumn ? Area.Y - ScreenMargin : RightColumn - EdgeGap;
		float Inward = EdgeInset; // how far in from the edge this column / row starts
		int32 First = 0;
		while (First < Line.Num())
		{
			// As many as fit into this column / row (at least one).
			int32 Last = First;
			float Length = bColumn ? Line[First]->Size.Y : Line[First]->Size.X;
			float Thick = bColumn ? Line[First]->Size.X : Line[First]->Size.Y;
			while (Last + 1 < Line.Num())
			{
				const float Next = bColumn ? Line[Last + 1]->Size.Y : Line[Last + 1]->Size.X;
				if (Length + EdgeGap + Next > To - From)
				{
					break;
				}
				Length += EdgeGap + Next;
				Thick = FMath::Max(Thick, bColumn ? Line[Last + 1]->Size.X : Line[Last + 1]->Size.Y);
				++Last;
			}
			// Where each wants to be: in a column standing at the height a card over its penguin would (above the heads),
			// the ones above pushed up by the ones below, then the column pulled down into the picture; in a row centred on
			// its marker, pushed along by the one before, then the row pulled back in.
			TArray<float> Starts;
			for (int32 Index = First; Index <= Last; ++Index)
			{
				const FCard& Card = *Line[Index];
				Starts.Add(bColumn ? Card.Anchor.Y - Card.Size.Y : Card.Anchor.X - Card.Size.X * 0.5f);
			}
			const auto PushForward = [&]()
			{
				float Cursor = From;
				for (int32 Index = First; Index <= Last; ++Index)
				{
					float& Start = Starts[Index - First];
					Start = FMath::Max(Start, Cursor);
					Cursor = Start + (bColumn ? Line[Index]->Size.Y : Line[Index]->Size.X) + EdgeGap;
				}
			};
			const auto PushBack = [&]()
			{
				float Limit = To;
				for (int32 Index = Last; Index >= First; --Index)
				{
					float& Start = Starts[Index - First];
					Start = FMath::Min(Start, Limit - (bColumn ? Line[Index]->Size.Y : Line[Index]->Size.X));
					Limit = Start - EdgeGap;
				}
			};
			if (bColumn)
			{
				PushBack();
				PushForward();
			}
			else
			{
				PushForward();
				PushBack();
			}
			for (int32 Index = First; Index <= Last; ++Index)
			{
				FCard& Card = *Line[Index];
				const float Start = Starts[Index - First];
				float Left = 0.f;
				float Top = 0.f;
				switch (Edge)
				{
				case EEdge::Left: Left = Inward; Top = Start; break;
				case EEdge::Right: Left = Area.X - Inward - Card.Size.X; Top = Start; break;
				case EEdge::Top: Left = Start; Top = Inward; break;
				default: Left = Start; Top = Area.Y - Inward - Card.Size.Y; break;
				}
				Card.Rect = FSlateRect(Left, Top, Left + Card.Size.X, Top + Card.Size.Y);
				Placed.Add(Card.Rect);
			}
			Inward += Thick + EdgeGap;
			First = Last + 1;
		}
		if (Edge == EEdge::Left)
		{
			LeftColumn = Inward;
		}
		else if (Edge == EEdge::Right)
		{
			RightColumn = Area.X - Inward;
		}
	}

	// Then the cards over the penguins in the picture, left to right: right above the icons, or a row higher, or to one
	// side - the first place that covers no other card and no marker and stays inside the picture (else the one that
	// covers least).
	const auto OverlapArea = [](const FSlateRect& A, const FSlateRect& B)
	{
		const float W = FMath::Min(A.Right, B.Right) - FMath::Max(A.Left, B.Left);
		const float H = FMath::Min(A.Bottom, B.Bottom) - FMath::Max(A.Top, B.Top);
		return W > 0.f && H > 0.f ? W * H : 0.f;
	};
	const auto Outside = [&Area](const FSlateRect& R)
	{
		return R.Left < 0.f || R.Top < 0.f || R.Right > Area.X || R.Bottom > Area.Y;
	};
	TArray<FCard*> InPicture;
	for (FCard& Card : Cards)
	{
		if (!Card.bEdge)
		{
			InPicture.Add(&Card);
		}
	}
	InPicture.Sort([](const FCard& A, const FCard& B) { return A.Anchor.X < B.Anchor.X; });
	for (FCard* Card : InPicture)
	{
		float Best = -1.f;
		bool bFound = false;
		for (int32 Shift = 0; Shift < 5 && !bFound; ++Shift)
		{
			// Shifts: none, a half card right, left, a whole card right, left.
			const float Dx = Shift == 0 ? 0.f : (Shift % 2 == 1 ? 1.f : -1.f) * static_cast<float>((Shift + 1) / 2) * (Card->Size.X * 0.5f + RowGap);
			for (int32 Row = 0; Row < MaxRows + 1 && !bFound; ++Row)
			{
				const float Bottom = Card->Anchor.Y - Row * (Card->Size.Y + RowGap);
				const float Left = Card->Anchor.X + Dx - Card->Size.X * 0.5f;
				const FSlateRect Rect(Left, Bottom - Card->Size.Y, Left + Card->Size.X, Bottom);
				float Cover = Outside(Rect) ? 1.e7f : 0.f;
				for (const FSlateRect& Other : Placed)
				{
					Cover += OverlapArea(Rect, Other);
				}
				for (const FSlateRect& Marker : Markers)
				{
					Cover += OverlapArea(Rect, Marker);
				}
				if (Best < 0.f || Cover < Best)
				{
					Best = Cover;
					Card->Rect = Rect;
					Card->Row = Row;
				}
				bFound = Cover <= 0.f;
			}
		}
		Placed.Add(Card->Rect);
	}

	// What the tests check, over the cards showing as cards: two of them overlapping, one covering a marker, one not
	// wholly in the picture.
	const auto ShowsAsCard = [](const FCard& Card)
	{
		return Card.Age >= Card.Rank * PopStagger && Card.Age < FlyAt + Card.Rank * FlyStagger;
	};
	for (int32 Index = 0; Index < Cards.Num(); ++Index)
	{
		const FCard& Card = Cards[Index];
		if (!ShowsAsCard(Card))
		{
			continue;
		}
		Stats.EdgeCards += Card.bEdge ? 1 : 0;
		Stats.CardsOutside += Outside(Card.Rect) ? 1 : 0;
		for (const FSlateRect& Marker : Markers)
		{
			if (OverlapArea(Card.Rect, Marker) > 0.f)
			{
				++Stats.CardsOverMarkers;
				break;
			}
		}
		for (int32 Other = Index + 1; Other < Cards.Num(); ++Other)
		{
			if (ShowsAsCard(Cards[Other]) && OverlapArea(Card.Rect, Cards[Other].Rect) > 0.f)
			{
				++Stats.OverlapPairs;
			}
		}
	}
	Stats.bOverlap = Stats.OverlapPairs > 0;
	Stats.Markers = Markers.Num();
	// Painted left to right (the names in screen order).
	Cards.Sort([](const FCard& A, const FCard& B) { return A.Rect.Left < B.Rect.Left; });

	// ---- paint: each card's glow, badge and name pill; flying badges; landing flashes
	int32 Layer = LayerId;
	PillBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
	PillBrush.OutlineSettings.Width = FPinguStyle::LineWidth;
	for (const FCard& Card : Cards)
	{
		const UPenguinTraitComponent* Traits = Card.Penguin->GetTraits();
		const FPinguTraitInfo& Info = Traits->GetInfo();
		FLinearColor TraitColor = Info.Color;
		TraitColor.A = 1.f;
		const FSlateBrush* Badge = PenguinHUD->GetTraitBrush(Card.Trait);
		// (A trait without an icon yet draws no badge at all - never an empty disc, docs/UI_STYLE.md.)
		const bool bBadge = Badge && Badge->GetDrawType() != ESlateBrushDrawType::NoDrawType && Badge->GetResourceObject() != nullptr;
		const float FlyStart = FlyAt + Card.Rank * FlyStagger;
		const float Landed = LandAt + Card.Rank * FlyStagger;
		const FSlateRect& Rect = Card.Rect;
		const float CardMiddle = 0.5f * (Rect.Left + Rect.Right);
		const FVector2f BadgeCentre(CardMiddle, Rect.Top + Card.Badge * 0.5f);
		if (Card.Age < Card.Rank * PopStagger)
		{
			continue; // laid out already (the others keep their places), not popped in yet
		}

		if (Card.Age < FlyStart)
		{
			// The card: pops in (the badge with a little overshoot, the name fading and sliding up into place).
			const float Pop = FMath::Clamp((Card.Age - Card.Rank * PopStagger) / PopTime, 0.f, 1.f);
			const float Grow = BackOut(Pop);
			const float Alpha = FMath::Clamp(Pop * 2.f, 0.f, 1.f);
			const FLinearColor CardFade = Fade.CopyWithNewOpacity(Fade.A * Alpha);
			++Stats.Cards;
			Stats.Rows = FMath::Max(Stats.Rows, Card.bEdge ? 1 : Card.Row + 1);
			Stats.Names.Add(Info.Name);
			// A soft glow in the trait's colour behind the badge, breathing slowly.
			const float Breath = 0.5f + 0.5f * FMath::Sin(Card.Age * 4.f + Card.Rank);
			for (int32 Step = 0; bBadge && Step < 3; ++Step)
			{
				const float Diameter = Card.Badge * Grow * (1.5f - 0.17f * Step + 0.06f * Breath);
				PaintCircle(OutDrawElements, AllottedGeometry, Layer, BadgeCentre, Diameter, 0.f, FLinearColor::Transparent,
					PinguUI::WithAlpha(TraitColor, 0.13f + 0.05f * Step) * CardFade);
			}
			if (bBadge)
			{
				const float Size = Card.Badge * Grow;
				const FVector2f Box(Size, Size);
				FSlateDrawElement::MakeBox(OutDrawElements, Layer + 1, AllottedGeometry.ToPaintGeometry(Box, FSlateLayoutTransform(1.f, BadgeCentre - Box * 0.5f)),
					Badge, ESlateDrawEffect::None, Badge->GetTint(InWidgetStyle) * CardFade);
			}
			// The name on a dark pill under the badge, its corner on a whole pixel (crisp text that never scales).
			const FVector2f PillSize(FMath::CeilToFloat(Card.NameSize.X + 2.f * PillPadX), FMath::CeilToFloat(Card.NameSize.Y + 2.f * PillPadY));
			const float Slide = PinguUI::SnapUnits(6.f * (1.f - Alpha));
			const float LeftPixels = FMath::RoundToFloat((CardMiddle - PillSize.X * 0.5f) * PixelsPerUnit);
			const float TopPixels = FMath::RoundToFloat((Rect.Top + Card.Badge + NameGap + Slide) * PixelsPerUnit);
			const FVector2f PillTopLeft(LeftPixels * UnitsPerPixel, TopPixels * UnitsPerPixel);
			PillBrush.OutlineSettings.Color = FSlateColor(TraitColor * CardFade);
			// (The UI's wood, 2026-11-07: PinguWoodStyle.h.)
			PinguWood::FPlankLook PillLook;
			PillLook.Cut = PinguWood::ECut::Pill;
			PillLook.Opacity = CardFade.A;
			PillLook.Seed = static_cast<uint32>(Card.Trait) * 31u + 5u;
			if (!PinguWood::PaintPlank(OutDrawElements, Layer + 2, AllottedGeometry, PillTopLeft, PillSize, PillLook))
			{
				FSlateDrawElement::MakeBox(OutDrawElements, Layer + 2, AllottedGeometry.ToPaintGeometry(PillSize, FSlateLayoutTransform(1.f, PillTopLeft)),
					&PillBrush, ESlateDrawEffect::None, PinguUI::WithAlpha(Style.Ink, 0.86f) * CardFade);
			}
			FSlateDrawElement::MakeText(OutDrawElements, Layer + 3,
				AllottedGeometry.ToPaintGeometry(Card.NameSize, FSlateLayoutTransform(1.f, PillTopLeft + FVector2f(PillPadX, PillPadY))),
				Card.Name, PinguWood::OnWoodFont(FPinguStyle::Title(Card.Font, 2)), ESlateDrawEffect::None, FMath::Lerp(TraitColor, FLinearColor::White, 0.7f) * CardFade);
			Layer += 4;
		}
		else if (Card.Age < Landed)
		{
			// The flight: eased in along a little arc from the card to the penguin's chest, shrinking; the name fades at once.
			const float P = FMath::Clamp((Card.Age - FlyStart) / (Landed - FlyStart), 0.f, 1.f);
			const float Ease = P * P;
			const FVector2f Where = FMath::Lerp(BadgeCentre, Card.Target, Ease) - FVector2f(0.f, ArcHeight * FMath::Sin(UE_PI * P));
			const float Size = FMath::Lerp(Card.Badge, LandSize, Ease);
			++Stats.Flying;
			Stats.Names.Add(Info.Name);
			if (bBadge)
			{
				PaintCircle(OutDrawElements, AllottedGeometry, Layer, Where, Size * 1.45f, 0.f, FLinearColor::Transparent, PinguUI::WithAlpha(TraitColor, 0.28f) * Fade);
				const FVector2f Box(Size, Size);
				FSlateDrawElement::MakeBox(OutDrawElements, Layer + 1, AllottedGeometry.ToPaintGeometry(Box, FSlateLayoutTransform(1.f, Where - Box * 0.5f)),
					Badge, ESlateDrawEffect::None, Badge->GetTint(InWidgetStyle) * Fade);
			}
			const float NameAlpha = FMath::Clamp(1.f - P / 0.3f, 0.f, 1.f);
			if (NameAlpha > 0.f)
			{
				const FLinearColor NameFade = Fade.CopyWithNewOpacity(Fade.A * NameAlpha);
				const FVector2f PillSize(FMath::CeilToFloat(Card.NameSize.X + 2.f * PillPadX), FMath::CeilToFloat(Card.NameSize.Y + 2.f * PillPadY));
				const float LeftPixels = FMath::RoundToFloat((CardMiddle - PillSize.X * 0.5f) * PixelsPerUnit);
				const float TopPixels = FMath::RoundToFloat((Rect.Top + Card.Badge + NameGap) * PixelsPerUnit);
				const FVector2f PillTopLeft(LeftPixels * UnitsPerPixel, TopPixels * UnitsPerPixel);
				PillBrush.OutlineSettings.Color = FSlateColor(TraitColor * NameFade);
				// (The UI's wood, 2026-11-07: PinguWoodStyle.h.)
				PinguWood::FPlankLook PillLook;
				PillLook.Cut = PinguWood::ECut::Pill;
				PillLook.Opacity = NameFade.A;
				PillLook.Seed = static_cast<uint32>(Card.Trait) * 31u + 5u;
				if (!PinguWood::PaintPlank(OutDrawElements, Layer + 2, AllottedGeometry, PillTopLeft, PillSize, PillLook))
				{
					FSlateDrawElement::MakeBox(OutDrawElements, Layer + 2, AllottedGeometry.ToPaintGeometry(PillSize, FSlateLayoutTransform(1.f, PillTopLeft)),
						&PillBrush, ESlateDrawEffect::None, PinguUI::WithAlpha(Style.Ink, 0.86f) * NameFade);
				}
				FSlateDrawElement::MakeText(OutDrawElements, Layer + 3,
					AllottedGeometry.ToPaintGeometry(Card.NameSize, FSlateLayoutTransform(1.f, PillTopLeft + FVector2f(PillPadX, PillPadY))),
					Card.Name, PinguWood::OnWoodFont(FPinguStyle::Title(Card.Font, 2)), ESlateDrawEffect::None, FMath::Lerp(TraitColor, FLinearColor::White, 0.7f) * NameFade);
			}
			Layer += 4;
		}
		else
		{
			// Landed: a flash ring in the trait's colour bursts out of the chest (the penguin changes now).
			const float P = FMath::Clamp((Card.Age - Landed) / FlashTime, 0.f, 1.f);
			const float Ease = PinguUI::EaseOut(P);
			const float Alpha = FMath::Square(1.f - P);
			++Stats.Flashes;
			PaintCircle(OutDrawElements, AllottedGeometry, Layer, Card.Target, FMath::Lerp(LandSize, FlashSize, Ease), FMath::Lerp(6.f, 1.5f, P),
				PinguUI::WithAlpha(FMath::Lerp(TraitColor, FLinearColor::White, 0.3f), Alpha) * Fade, FLinearColor(0.f, 0.f, 0.f, 0.001f));
			if (P < 0.5f)
			{
				PaintCircle(OutDrawElements, AllottedGeometry, Layer + 1, Card.Target, FMath::Lerp(LandSize * 2.f, LandSize * 3.5f, P * 2.f), 0.f,
					FLinearColor::Transparent, PinguUI::WithAlpha(FLinearColor::White, 0.55f * (1.f - P * 2.f)) * Fade);
			}
			Layer += 2;
		}
	}
	return Layer > LayerId ? Layer - 1 : LayerId;
}

int32 SPinguTraitReveal::PaintDeadlyEdges(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer,
	const APlayerController* Viewer, const FLinearColor& Fade) const
{
	PinguUI::FTraitRevealStats& Stats = PinguUITraitsPrivate::MutableRevealStats();
	const APenguinGameState* GameState = APenguinGameState::Get(Viewer);
	const ABlockIsland* Island = GameState && GameState->HasDeadlyEdges() ? ABlockIsland::Get(Viewer->GetWorld()) : nullptr;
	if (!Island)
	{
		return Layer;
	}
	// Minimal and clean (Linus): one thin red line per deadly side through the whole picture, a soft glow on its inner side, and
	// the world beyond it faintly red. Projected every frame (the camera moves).
	const float PixelsPerUnit = AllottedGeometry.GetAccumulatedLayoutTransform().GetScale();
	const float UnitsPerPixel = PixelsPerUnit > UE_KINDA_SMALL_NUMBER ? 1.f / PixelsPerUnit : 1.f;
	const FVector2f Area = AllottedGeometry.GetLocalSize();
	const FBox Bounds = Island->GetBounds();
	const float LowZ = Island->SeaLevel;
	const float HighZ = static_cast<float>(Bounds.Max.Z) + 3000.f;
	const FVector2D Edges = GameState->GetDeadlyEdges();
	const FLinearColor Red(0.95f, 0.12f, 0.1f);
	const FSlateBrush* White = FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox"));
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const float X = static_cast<float>(Side == 0 ? Edges.X : Edges.Y);
		FVector2D Low;
		FVector2D High;
		if (!Viewer->ProjectWorldLocationToScreen(FVector(X, 0.f, LowZ), Low, false) || !Viewer->ProjectWorldLocationToScreen(FVector(X, 0.f, HighZ), High, false))
		{
			continue;
		}
		const FVector2f A(static_cast<float>(Low.X) * UnitsPerPixel, static_cast<float>(Low.Y) * UnitsPerPixel);
		const FVector2f B(static_cast<float>(High.X) * UnitsPerPixel, static_cast<float>(High.Y) * UnitsPerPixel);
		// (The line runs through the whole picture: extended to its top and bottom.)
		const FVector2f Dir = (B - A).GetSafeNormal();
		if (FMath::Abs(Dir.Y) < 0.2f)
		{
			continue;
		}
		const FVector2f Top = A + Dir * ((0.f - A.Y) / Dir.Y);
		const FVector2f Bottom = A + Dir * ((Area.Y - A.Y) / Dir.Y);
		const float MidX = 0.5f * (Top.X + Bottom.X);
		if (MidX < -40.f || MidX > Area.X + 40.f)
		{
			continue; // out of the picture
		}
		// The tint beyond the line (to the screen's edge), a glow of a few bands on the line's inner side, then the line.
		if (White)
		{
			const float From = Side == 0 ? 0.f : MidX;
			const float To = Side == 0 ? MidX : Area.X;
			if (To - From > 1.f)
			{
				FSlateDrawElement::MakeBox(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(FVector2f(To - From, Area.Y), FSlateLayoutTransform(FVector2f(From, 0.f))),
					White, ESlateDrawEffect::None, PinguUI::WithAlpha(Red, 0.18f) * Fade);
			}
			constexpr float BandWidth = 5.f;
			for (int32 Band = 0; Band < 4; ++Band)
			{
				const float Left = Side == 0 ? MidX + Band * BandWidth : MidX - (Band + 1) * BandWidth;
				FSlateDrawElement::MakeBox(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(FVector2f(BandWidth, Area.Y), FSlateLayoutTransform(FVector2f(Left, 0.f))),
					White, ESlateDrawEffect::None, PinguUI::WithAlpha(Red, 0.12f * (4 - Band) / 4.f) * Fade);
			}
		}
		TArray<FVector2f> Points = { Top, Bottom };
		FSlateDrawElement::MakeLines(OutDrawElements, Layer + 1, AllottedGeometry.ToPaintGeometry(), Points, ESlateDrawEffect::None,
			PinguUI::WithAlpha(Red, 0.9f) * Fade, true, 3.f);
		++Stats.EdgeLines;
		if (Side == 0)
		{
			Stats.EdgeLeftX = MidX;
		}
		else
		{
			Stats.EdgeRightX = MidX;
		}
	}
	return Layer + 2;
}

int32 SPinguTraitReveal::PaintSharedBanner(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, EPinguTrait Trait,
	float Age, const FWidgetStyle& InWidgetStyle, const FLinearColor& Fade) const
{
	using namespace PinguUITraitsPrivate;
	PinguUI::FTraitRevealStats& Stats = MutableRevealStats();
	constexpr float FlyAt = UPenguinTraitComponent::CardsTime;
	constexpr float LandAt = UPenguinTraitComponent::TransformAt;
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD || Age < 0.f || Age >= LandAt)
	{
		return Layer; // not yet, or landed: from LandAt on the corner icon shows it (SPinguTraitCorner)
	}
	// Only the logo (card 2026-11-07_trait-logo-wood-tiling; Linus: "just the trait logo no planks or text with it"): no plank,
	// no name, no upside / downside (the trait LIST and the PINGUPEDIA keep those). It pops in big and crisp near the top (the
	// badge textures are 1024 px), shows until the cards' time is up, then flies into the top-left corner and turns into the
	// corner icon the round keeps. Painted from the replicated reveal age: the same on the host, every LAN client and solo.
	const FPinguTraitInfo& Info = PinguTraits::GetInfo(Trait);
	const FVector2f Area = AllottedGeometry.GetLocalSize();
	const float Big = FMath::Min(LogoSize, 0.3f * Area.Y);
	const FVector2f Home(0.5f * Area.X, FMath::Max(LogoHeight * Area.Y, 0.5f * Big + 24.f));
	// Where it lands: the corner icon's place (absolute, from its last paint) in this widget's space; before the corner ever
	// painted, where its box will stand (CornerMargin from the corner).
	FVector2f CornerCentre(CornerMargin + 0.5f * CornerSize, CornerMargin + 0.5f * CornerSize);
	float CornerSide = CornerSize;
	if (CornerPlace().bKnown)
	{
		const FSlateRect& Abs = CornerPlace().Absolute;
		const FVector2f TopLeftLocal = AllottedGeometry.AbsoluteToLocal(FVector2f(Abs.Left, Abs.Top));
		const FVector2f BottomRightLocal = AllottedGeometry.AbsoluteToLocal(FVector2f(Abs.Right, Abs.Bottom));
		CornerCentre = 0.5f * (TopLeftLocal + BottomRightLocal);
		CornerSide = FMath::Max(BottomRightLocal.X - TopLeftLocal.X, 1.f);
	}
	// Pop: a little overshoot from 0.55x, fading in over the first half; flight: eased in and out, shrinking to the corner's size.
	const float Pop = BackOut(FMath::Clamp(Age / LogoPopTime, 0.f, 1.f));
	const float Alpha = FMath::Clamp(Age / LogoPopTime * 2.f, 0.f, 1.f);
	const float FlyRaw = FMath::Clamp((Age - FlyAt) / (LandAt - FlyAt), 0.f, 1.f);
	const float Fly = FlyRaw * FlyRaw * (3.f - 2.f * FlyRaw);
	const float Side = FMath::Lerp(Big * FMath::Lerp(0.55f, 1.f, Pop), CornerSide, Fly);
	const FVector2f Centre = FMath::Lerp(Home, CornerCentre, Fly);
	const FLinearColor LogoFade = Fade.CopyWithNewOpacity(Fade.A * Alpha);
	const FVector2f Box(Side, Side);
	const FVector2f TopLeft = Centre - Box * 0.5f;
	Stats.bBanner = true;
	Stats.BannerAge = Age;
	Stats.BannerName = Info.Name;
	Stats.BannerLines = 0; // (words drawn with it: none)
	{
		// (Absolute, like the corner's own rect, so a check can see the flight end on it.)
		const FVector2f AbsTopLeft = AllottedGeometry.LocalToAbsolute(TopLeft);
		const FVector2f AbsBottomRight = AllottedGeometry.LocalToAbsolute(TopLeft + Box);
		Stats.BannerRect = FSlateRect(AbsTopLeft.X, AbsTopLeft.Y, AbsBottomRight.X, AbsBottomRight.Y);
	}
	Stats.bBannerOutside = TopLeft.X < 0.f || TopLeft.Y < 0.f || TopLeft.X + Side > Area.X || TopLeft.Y + Side > Area.Y;
	Stats.Names.Add(Info.Name);
	const FSlateBrush* Badge = PenguinHUD->GetTraitBrush(Trait);
	if (Badge && Badge->GetDrawType() != ESlateBrushDrawType::NoDrawType && Badge->GetResourceObject() != nullptr)
	{
		// The same crisp drop shadow as the corner icon (the badge's own shape in dark, a little lower), scaled with it.
		const float ShadowDrop = 2.f * Side / CornerSize;
		FSlateDrawElement::MakeBox(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(Box, FSlateLayoutTransform(1.f, TopLeft + FVector2f(0.f, ShadowDrop))),
			Badge, ESlateDrawEffect::None, FLinearColor(0.f, 0.f, 0.f, 0.45f) * LogoFade);
		FSlateDrawElement::MakeBox(OutDrawElements, Layer + 1, AllottedGeometry.ToPaintGeometry(Box, FSlateLayoutTransform(1.f, TopLeft)),
			Badge, ESlateDrawEffect::None, Badge->GetTint(InWidgetStyle) * LogoFade);
	}
	// (No badge art: nothing is drawn - never a disc or a letter, docs/UI_STYLE.md; the stats still name the trait.)
	return Layer + 2;
}

// ---------------------------------------------------------------------------- the corner icon

namespace PinguUITraitsPrivate
{
	static PinguUI::FTraitCornerStats& MutableCornerStats()
	{
		static PinguUI::FTraitCornerStats Stats;
		return Stats;
	}

	// (CornerSize, CornerMargin, CornerFadeIn: at the top of the file - the round-start logo flies onto the corner icon.)
}

/**
 * The viewer's current trait as its badge alone in the top-left corner, the whole round (Linus 2026-11-07: "just use the icon and
 * move it in the top left corner during the game so all can see it"). No plate behind it: the badge texture is 1024 px, so it is
 * drawn crisp; a soft dark shadow keeps it readable. Derived from the replicated trait state, so it is the same on host, LAN
 * clients and solo. Never takes a click.
 */
class SPinguTraitCorner : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguTraitCorner)
	{
		_Visibility = EVisibility::HitTestInvisible;
	}
		SLATE_ARGUMENT(TWeakObjectPtr<APenguinHUD>, HUD)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs) { HUD = InArgs._HUD; }

protected:
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override
	{
		return FVector2D(PinguUITraitsPrivate::CornerSize, PinguUITraitsPrivate::CornerSize);
	}
	virtual bool ComputeVolatility() const override { return true; }
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		using namespace PinguUITraitsPrivate;
		PinguUI::FTraitCornerStats& Stats = MutableCornerStats();
		Stats.bShown = false;
		Stats.Trait = EPinguTrait::None;
		Stats.Frame = GFrameCounter;
		// (Where the box stands, shown or not: the round-start logo flies onto it.)
		CornerPlace().Absolute = AllottedGeometry.GetLayoutBoundingRect();
		CornerPlace().bKnown = true;
		const APenguinHUD* PenguinHUD = HUD.Get();
		const UWorld* World = PenguinHUD ? PenguinHUD->GetWorld() : nullptr;
		const APlayerController* Viewer = World ? World->GetFirstPlayerController() : nullptr;
		if (!Viewer)
		{
			return LayerId;
		}
		// The viewer's own penguin's trait; with none (out of the round), the round's one trait if it has one.
		EPinguTrait Trait = EPinguTrait::None;
		float Age = -1.f;
		if (const APenguinCharacter* Own = Cast<APenguinCharacter>(Viewer->GetPawn()))
		{
			const UPenguinTraitComponent* Traits = Own->GetTraits();
			if (Traits && Traits->HasAnyTrait() && Traits->GetState().RevealStart >= 0.f)
			{
				Trait = Traits->GetTrait();
				Age = Traits->GetRevealAge();
			}
		}
		if (!PinguTraits::IsReal(Trait))
		{
			const APenguinGameState* GameState = APenguinGameState::Get(World);
			const EPinguTrait RoundTrait = GameState ? GameState->GetRoundTrait() : EPinguTrait::None;
			if (PinguTraits::IsReal(RoundTrait))
			{
				for (TActorIterator<APenguinCharacter> It(World); It; ++It)
				{
					const UPenguinTraitComponent* Traits = IsValid(*It) ? (*It)->GetTraits() : nullptr;
					if (Traits && Traits->GetTrait() == RoundTrait && Traits->GetState().RevealStart >= 0.f)
					{
						Trait = RoundTrait;
						Age = FMath::Max(Age, Traits->GetRevealAge());
					}
				}
			}
		}
		// The round's one trait: its logo has just flown in and landed here (SPinguTraitReveal::PaintSharedBanner), so the icon is
		// there at once; a trait of the viewer's own (the cards fly into the penguins) fades in.
		const APenguinGameState* RoundState = APenguinGameState::Get(World);
		const bool bLanded = RoundState && PinguTraits::IsReal(Trait) && RoundState->GetRoundTrait() == Trait;
		const float Shown = bLanded ? (Age >= UPenguinTraitComponent::TransformAt ? 1.f : 0.f)
			: FMath::Clamp((Age - UPenguinTraitComponent::TransformAt) / CornerFadeIn, 0.f, 1.f);
		if (!PinguTraits::IsReal(Trait) || Shown <= 0.f)
		{
			return LayerId;
		}
		const FLinearColor Fade = InWidgetStyle.GetColorAndOpacityTint().CopyWithNewOpacity(InWidgetStyle.GetColorAndOpacityTint().A * Shown);
		const FVector2f Box(CornerSize, CornerSize);
		const FVector2f Origin = FVector2f::ZeroVector;
		const FSlateBrush* Badge = PenguinHUD->GetTraitBrush(Trait);
		if (Badge && Badge->GetDrawType() != ESlateBrushDrawType::NoDrawType && Badge->GetResourceObject() != nullptr)
		{
			// A crisp small drop shadow: the badge's own shape in dark, 2 units lower (no blurry disc behind it).
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(Box, FSlateLayoutTransform(1.f, Origin + FVector2f(0.f, 2.f))),
				Badge, ESlateDrawEffect::None, FLinearColor(0.f, 0.f, 0.f, 0.45f) * Fade);
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(Box, FSlateLayoutTransform(1.f, Origin)),
				Badge, ESlateDrawEffect::None, Badge->GetTint(InWidgetStyle) * Fade);
		}
		// (No badge art for this trait yet: nothing is drawn - never an empty disc, style 5 / docs/UI_STYLE.md; the stats still say
		// which trait the corner stands for, so the checks follow the trait, not the art.)
		Stats.bShown = true;
		Stats.Trait = Trait;
		Stats.Size = CornerSize;
		Stats.Rect = AllottedGeometry.GetLayoutBoundingRect();
		return LayerId + 3;
	}

private:
	TWeakObjectPtr<APenguinHUD> HUD;
	mutable FSlateRoundedBoxBrush CircleBrush{ FLinearColor::White, 50.f, FLinearColor::White, 0.f };
};

TSharedRef<SWidget> PinguUI::MakeTraitCorner(const TWeakObjectPtr<APenguinHUD>& HUD)
{
	return SNew(SBox).HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(PinguUITraitsPrivate::CornerMargin)
		[
			SNew(SPinguTraitCorner).HUD(HUD)
		];
}

const PinguUI::FTraitCornerStats& PinguUI::GetTraitCornerStats()
{
	return PinguUITraitsPrivate::MutableCornerStats();
}

TSharedRef<SWidget> PinguUI::MakeTraitReveal(const TWeakObjectPtr<APenguinHUD>& HUD)
{
	return SNew(SPinguTraitReveal).HUD(HUD);
}

const PinguUI::FTraitRevealStats& PinguUI::GetTraitRevealStats()
{
	return PinguUITraitsPrivate::MutableRevealStats();
}

#undef LOCTEXT_NAMESPACE
