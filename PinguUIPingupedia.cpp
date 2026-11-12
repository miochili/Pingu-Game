// Pingu - the PINGUPEDIA (from the title): every ability and every trait with their real numbers. Two tabs, ABILITIES and
// TRAITS (click them, LB / RB, or Left / Right on the list); a scrolling list on the left (hovering a row, the arrow keys
// or the stick select it); the selected entry's card on the right: its big icon, name and tags, what it does, and a grid
// of its numbers. The abilities' numbers come from UPenguinAbilityComponent::GetInfo + PinguTuning::GetAbilityStats, the
// traits' from PinguTraits::GetInfo / GetStatLines (the upside in green, the downside in red). BACK, Esc or gamepad B go
// back to the title. No hints on the screen: README_Pingu.md explains.

#include "PinguUI.h"
#include "EngineUtils.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinHUD.h"
#include "PenguinMovementComponent.h"
#include "PinguMenu3D.h"
#include "PinguTraitTable.h"
#include "PinguTuning.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Engine/World.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PinguUI"

/** The PINGUPEDIA's state: the page that shows, its selected entry, the widgets the focus and the scrolling go to. */
struct FPinguPediaState
{
	/** 0 abilities, 1 traits. */
	int32 Page = 0;
	int32 Entry = 0;
	TSharedPtr<SWidget> BackButton;
	TSharedPtr<SScrollBox> Lists[2];
	/** Every page's rows, in list order (the focus goes to them by entry). */
	TArray<TSharedPtr<SWidget>> Rows[2];
	/** The detail card (rebuilt when the selection changes) and the scroll box around it. */
	TSharedPtr<SBox> Detail;
	TSharedPtr<SScrollBox> DetailScroll;
	/** What the detail card shows (-1: nothing yet). */
	int32 ShownPage = -1;
	int32 ShownEntry = -1;
};

namespace PinguUIPingupediaPrivate
{
	/** The layout (Slate units, 1 unit = 1 pixel at 1080p): a 1180 x ~840 card fits 1080p and the 1280 x 720 test window
	 *  (there the UI is 2048 x 1152 units; any screen has at least 1000 units of height). */
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
	/** A stat line's strip: its height, the room left and right inside it and between name and number, the gaps between
	 *  two strips side by side and between two rows. */
	constexpr float StatHeight = 40.f;
	constexpr float StatPadding = 14.f;
	constexpr float StatValueGap = 12.f;
	constexpr float StatGap = 12.f;
	constexpr float StatRowGap = 8.f;
	/** The detail panel's inside: its width (the card's 1100 less the list, the gap and the panel's padding) and height. */
	constexpr float DetailWidth = CardWidth - 80.f - ListWidth - ListGap - 2.f * DetailPadding;
	constexpr float DetailRoom = BodyHeight - (DetailPadding - 4.f) - DetailPadding;
	constexpr int32 NumPages = 2;
	constexpr int32 AbilitiesPage = 0;
	constexpr int32 TraitsPage = 1;

	static int32 NumEntries(int32 Page)
	{
		return Page == TraitsPage ? PinguNumTraits : static_cast<int32>(EPenguinAbility::Count) - 1;
	}

	/** The entries in list order: the abilities' table order (Snowball first), the traits' (Heavyweight ... Jester). */
	static EPenguinAbility AbilityAt(int32 Entry)
	{
		return static_cast<EPenguinAbility>(FMath::Clamp(Entry, 0, NumEntries(AbilitiesPage) - 1) + 1);
	}

	static EPinguTrait TraitAt(int32 Entry)
	{
		return static_cast<EPinguTrait>(FMath::Clamp(Entry, 0, NumEntries(TraitsPage) - 1) + 1);
	}

	static FText EntryName(int32 Page, int32 Entry)
	{
		return Page == TraitsPage ? PinguUI::Upper(PinguTraits::GetInfo(TraitAt(Entry)).Name)
			: PinguUI::Upper(UPenguinAbilityComponent::GetInfo(AbilityAt(Entry)).Name);
	}

	/** A stat line's name and number. */
	static FSlateFontInfo StatLabelFont()
	{
		return FPinguStyle::Title(16.f);
	}

	static FSlateFontInfo StatValueFont()
	{
		return FPinguStyle::Title(24.f);
	}

	static PinguUI::FPingupediaStats& Stats()
	{
		static PinguUI::FPingupediaStats Instance;
		return Instance;
	}

	/** The list's scroll bar: a slim rounded thumb in the menus' ice colours (the engine's default is the editor's grey). */
	static const FScrollBarStyle& ScrollBarLook()
	{
		static FScrollBarStyle Look;
		static bool bMade = false;
		if (!bMade)
		{
			bMade = true;
			const FPinguStyle& Style = FPinguStyle::Get();
			FSlateBrush None;
			None.DrawAs = ESlateBrushDrawType::NoDrawType;
			const FSlateRoundedBoxBrush Track(PinguUI::WithAlpha(Style.Ink, 0.45f), 4.f);
			Look.SetHorizontalBackgroundImage(None)
				.SetVerticalBackgroundImage(None)
				.SetHorizontalTopSlotImage(Track)
				.SetHorizontalBottomSlotImage(Track)
				.SetVerticalTopSlotImage(Track)
				.SetVerticalBottomSlotImage(Track)
				.SetNormalThumbImage(FSlateRoundedBoxBrush(Style.IceDim, 4.f))
				.SetHoveredThumbImage(FSlateRoundedBoxBrush(Style.Ice, 4.f))
				.SetDraggedThumbImage(FSlateRoundedBoxBrush(Style.Snow, 4.f))
				.SetThickness(8.f);
		}
		return Look;
	}

	/** No shadows at the ends of the lists (the rows are cut off cleanly at the well's edge). */
	static const FScrollBoxStyle& ScrollBoxLook()
	{
		static FScrollBoxStyle Look;
		static bool bMade = false;
		if (!bMade)
		{
			bMade = true;
			FSlateBrush None;
			None.DrawAs = ESlateBrushDrawType::NoDrawType;
			Look.SetTopShadowBrush(None).SetBottomShadowBrush(None).SetLeftShadowBrush(None).SetRightShadowBrush(None);
		}
		return Look;
	}

	/** A small pill with a word on it (the tags, UPSIDE / DOWNSIDE). */
	static TSharedRef<SWidget> MakePill(const FText& Text, const FLinearColor& Fill, const FLinearColor& TextColor, float FontSize)
	{
		return SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SPinguShape)
				.FillColor(Fill)
				.OutlineColor(FPinguStyle::Get().Ink)
				.OutlineWidth(FPinguStyle::LineWidth)
				.Pill(true)
			]
			+ SOverlay::Slot()
			.Padding(FMargin(12.f, 3.f, 12.f, 4.f))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Text)
				.Font(FPinguStyle::Title(FontSize))
				.ColorAndOpacity(TextColor)
			];
	}

	/** An entry's icon in a square of Size: an ability's art on a dark disc in a ring of its colour (as above the heads), a
	 *  trait's hexagon badge as it is (its gold rim is part of the badge). */
	static TSharedRef<SWidget> MakeEntryIcon(const APenguinHUD* PenguinHUD, int32 Page, int32 Entry, float Size)
	{
		const FPinguStyle& Style = FPinguStyle::Get();
		if (Page == TraitsPage)
		{
			return SNew(SBox)
				.WidthOverride(Size)
				.HeightOverride(Size)
				[
					SNew(SImage)
					.Image(PenguinHUD ? PenguinHUD->GetTraitBrush(TraitAt(Entry)) : &Style.NoBrush)
				];
		}
		const EPenguinAbility Ability = AbilityAt(Entry);
		return SNew(SBox)
			.WidthOverride(Size)
			.HeightOverride(Size)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SPinguShape)
					.FillColor(Style.PanelDeep)
					.OutlineColor(UPenguinAbilityComponent::GetInfo(Ability).Color)
					.OutlineWidth(FMath::Max(2.f, FMath::RoundToFloat(Size * 0.05f)))
					.Pill(true)
				]
				+ SOverlay::Slot()
				.Padding(FMath::RoundToFloat(Size * 0.1f))
				[
					SNew(SImage)
					.Image(PenguinHUD ? PenguinHUD->GetAbilityBrush(Ability) : &Style.NoBrush)
				]
			];
	}

	/** The movement whose numbers the abilities' stat lines read (the stomp's fall speed, the icicle pad's throw): this
	 *  player's penguin, else any penguin (BP_Penguin's tuning: the title's demo has bots), else the class defaults. */
	static const UPenguinMovementComponent* FindMovement(const APenguinHUD* PenguinHUD)
	{
		if (!PenguinHUD)
		{
			return nullptr;
		}
		if (const APenguinCharacter* Own = PenguinHUD->GetHumanPenguin())
		{
			if (const UPenguinMovementComponent* OwnMove = Own->GetPenguinMovement())
			{
				return OwnMove;
			}
		}
		if (UWorld* World = PenguinHUD->GetWorld())
		{
			for (TActorIterator<APenguinCharacter> It(World); It; ++It)
			{
				if (const UPenguinMovementComponent* AnyMove = It->GetPenguinMovement())
				{
					return AnyMove;
				}
			}
		}
		return nullptr;
	}

	static FText Number(float Value)
	{
		FNumberFormattingOptions Options;
		Options.SetUseGrouping(false).SetMinimumFractionalDigits(0).SetMaximumFractionalDigits(2);
		return FText::AsNumber(Value, &Options);
	}

	/** Lengths, speeds and angles: whole numbers ("986 cm", not "985.51 cm"). */
	static FText WholeNumber(float Value)
	{
		return Number(FMath::RoundToFloat(Value));
	}

	/** A stat's value with its unit ("2.3 s", "1300 cm/s", "x0.35", "55 %", "YES"). */
	static FText FormatValue(EPinguStatKind Kind, float Value)
	{
		switch (PinguTraits::GetStatUnit(Kind))
		{
		case EPinguStatUnit::Seconds:
			return FText::Format(LOCTEXT("PingupediaUnitSeconds", "{0} s"), Number(Value));
		case EPinguStatUnit::Centimetres:
			return FText::Format(LOCTEXT("PingupediaUnitCentimetres", "{0} cm"), WholeNumber(Value));
		case EPinguStatUnit::Speed:
			return FText::Format(LOCTEXT("PingupediaUnitSpeed", "{0} cm/s"), WholeNumber(Value));
		case EPinguStatUnit::Multiplier:
			return FText::Format(LOCTEXT("PingupediaUnitMultiplier", "x{0}"), Number(Value));
		case EPinguStatUnit::Share:
			return FText::Format(LOCTEXT("PingupediaUnitShare", "{0} %"), Number(FMath::RoundToFloat(Value * 100.f)));
		case EPinguStatUnit::Degrees:
			// The table has the cone's half angle; the card shows how wide the whole cone is.
			return FText::Format(LOCTEXT("PingupediaUnitDegrees", "{0}°"), WholeNumber(2.f * Value));
		case EPinguStatUnit::Flag:
			return Value > 0.5f ? LOCTEXT("PingupediaUnitYes", "YES") : LOCTEXT("PingupediaUnitNo", "NO");
		case EPinguStatUnit::Count:
		default:
			return Number(Value);
		}
	}

	/** What a stat line is called. Most kinds have one name; a few read better with what they are on that card (the
	 *  icicle's lifetime is how long it stays stuck, Rubber's floor bounce is how high it bounces back).
	 *  bOutRaw (if given): true when the kind has no name of its own yet and its code name shows (the fit check counts
	 *  them: there must be none). */
	static FText StatLabel(EPinguStatKind Kind, int32 Page, int32 Entry, bool bUpside, bool* bOutRaw = nullptr)
	{
		if (bOutRaw)
		{
			*bOutRaw = false;
		}
		if (Page == AbilitiesPage)
		{
			switch (AbilityAt(Entry))
			{
			case EPenguinAbility::Icicle:
				if (Kind == EPinguStatKind::Duration) { return LOCTEXT("PingupediaStatIcicleStuck", "STAYS STUCK"); }
				if (Kind == EPinguStatKind::Lift) { return LOCTEXT("PingupediaStatIciclePad", "PAD THROW"); }
				if (Kind == EPinguStatKind::Speed) { return LOCTEXT("PingupediaStatIcicleSpeed", "FULL THROW"); }
				if (Kind == EPinguStatKind::Range) { return LOCTEXT("PingupediaStatIcicleTap", "TAP LOB"); }
				break;
			case EPenguinAbility::BellyStomp:
				if (Kind == EPinguStatKind::Speed) { return LOCTEXT("PingupediaStatStompSpeed", "FALL SPEED"); }
				if (Kind == EPinguStatKind::KillRadius) { return LOCTEXT("PingupediaStatStompReach", "SQUASH REACH"); }
				if (Kind == EPinguStatKind::Radius) { return LOCTEXT("PingupediaStatStompShock", "SHOCKWAVE"); }
				break;
			case EPenguinAbility::FrostBreath:
				if (Kind == EPinguStatKind::Duration) { return LOCTEXT("PingupediaStatFrostBreath", "BREATH"); }
				break;
			case EPenguinAbility::IcebergDrop:
			case EPenguinAbility::FlipperSlap:
			case EPenguinAbility::FishHook:
				if (Kind == EPinguStatKind::Range) { return LOCTEXT("PingupediaStatReach", "REACH"); }
				break;
			case EPenguinAbility::SnowBomb:
				if (Kind == EPinguStatKind::Radius) { return LOCTEXT("PingupediaStatBlast", "BLAST RADIUS"); }
				break;
			case EPenguinAbility::Noot:
				if (Kind == EPinguStatKind::Knockback) { return LOCTEXT("PingupediaStatNootClose", "CLOSE-UP PUSH"); }
				break;
			case EPenguinAbility::IceGun:
				if (Kind == EPinguStatKind::Slip) { return LOCTEXT("PingupediaStatIceFeet", "SLIPPERY FEET"); }
				break;
			case EPenguinAbility::Snowman:
				if (Kind == EPinguStatKind::Duration) { return LOCTEXT("PingupediaStatSnowmanReload", "THROWS EVERY"); }
				if (Kind == EPinguStatKind::Range) { return LOCTEXT("PingupediaStatSnowmanRange", "THROWS AS FAR AS"); }
				break;
			case EPenguinAbility::GiantSnowball:
				if (Kind == EPinguStatKind::Duration) { return LOCTEXT("PingupediaStatGiantBuild", "ROLL-UP"); }
				if (Kind == EPinguStatKind::Speed) { return LOCTEXT("PingupediaStatGiantSpeed", "ROLL SPEED"); }
				if (Kind == EPinguStatKind::Range) { return LOCTEXT("PingupediaStatGiantSize", "ROLLS UP TO"); }
				if (Kind == EPinguStatKind::Blocks) { return LOCTEXT("PingupediaStatGiantWalls", "BREAKS ON WALL HIT"); }
				break;
			case EPenguinAbility::Flap:
				if (Kind == EPinguStatKind::Range) { return LOCTEXT("PingupediaStatFlapGround", "UP FROM THE GROUND"); }
				if (Kind == EPinguStatKind::Radius) { return LOCTEXT("PingupediaStatFlapFall", "UP FROM A FALL"); }
				if (Kind == EPinguStatKind::Speed) { return LOCTEXT("PingupediaStatFlapSpeed", "CLIMB SPEED"); }
				break;
			default:
				break;
			}
		}
		else
		{
			switch (TraitAt(Entry))
			{
			case EPinguTrait::IceTouch:
				if (Kind == EPinguStatKind::Radius) { return LOCTEXT("PingupediaStatIceRadius", "ICE RADIUS"); }
				if (Kind == EPinguStatKind::Freeze) { return LOCTEXT("PingupediaStatShotsFrozen", "SHOTS FROZEN"); }
				if (Kind == EPinguStatKind::Slip) { return LOCTEXT("PingupediaStatFrozenFeet", "FROZEN FEET"); }
				if (Kind == EPinguStatKind::Grip) { return LOCTEXT("PingupediaStatIceGrip", "GRIP ON YOUR ICE"); }
				if (Kind == EPinguStatKind::Braking) { return LOCTEXT("PingupediaStatIceBraking", "BRAKING ON YOUR ICE"); }
				break;
			case EPinguTrait::Sticky:
				// Both reaches show: up and forward.
				if (Kind == EPinguStatKind::ClimbReach) { return LOCTEXT("PingupediaStatClimbReachUp", "LEDGE REACH UP"); }
				break;
			case EPinguTrait::Giant:
				if (Kind == EPinguStatKind::Blocks) { return LOCTEXT("PingupediaStatIceBroken", "ICE BROKEN"); }
				break;
			case EPinguTrait::Rubber:
				// How high a fall bounces it back up (a share of the height it fell from).
				if (Kind == EPinguStatKind::FloorBounce) { return LOCTEXT("PingupediaStatBounceHeight", "BOUNCE HEIGHT"); }
				break;
			case EPinguTrait::Gambler:
				if (Kind == EPinguStatKind::Chance) { return LOCTEXT("PingupediaStatDoubleChance", "DOUBLE CHANCE"); }
				if (Kind == EPinguStatKind::Strength) { return LOCTEXT("PingupediaStatDoubleStrength", "DOUBLE HIT"); }
				break;
			default:
				break;
			}
		}
		switch (Kind)
		{
		case EPinguStatKind::Cooldown: return LOCTEXT("PingupediaStatCooldown", "COOLDOWN");
		case EPinguStatKind::Knockback: return LOCTEXT("PingupediaStatKnockback", "KNOCKBACK");
		case EPinguStatKind::Lift: return LOCTEXT("PingupediaStatLift", "LIFT");
		case EPinguStatKind::Speed: return LOCTEXT("PingupediaStatSpeed", "SPEED");
		case EPinguStatKind::Range: return LOCTEXT("PingupediaStatRange", "RANGE");
		case EPinguStatKind::Radius: return LOCTEXT("PingupediaStatRadius", "RADIUS");
		case EPinguStatKind::KillRadius: return LOCTEXT("PingupediaStatKillRadius", "KO RADIUS");
		case EPinguStatKind::Cone: return LOCTEXT("PingupediaStatCone", "CONE");
		case EPinguStatKind::Duration: return LOCTEXT("PingupediaStatDuration", "DURATION");
		case EPinguStatKind::Freeze: return LOCTEXT("PingupediaStatFreeze", "FREEZE");
		case EPinguStatKind::Stun: return LOCTEXT("PingupediaStatStun", "STUN");
		case EPinguStatKind::Slip: return LOCTEXT("PingupediaStatSlip", "SLIP");
		case EPinguStatKind::Blocks: return LOCTEXT("PingupediaStatBlocks", "BLOCKS");
		case EPinguStatKind::Fuse: return LOCTEXT("PingupediaStatFuse", "FUSE");
		case EPinguStatKind::SizeStep: return LOCTEXT("PingupediaStatSizeStep", "SIZE PER HIT");
		case EPinguStatKind::CooldownScale: return LOCTEXT("PingupediaStatCooldownScale", "COOLDOWNS");
		case EPinguStatKind::Strength: return LOCTEXT("PingupediaStatStrength", "STRENGTH");
		case EPinguStatKind::KnockbackTaken: return LOCTEXT("PingupediaStatKnockbackTaken", "KNOCKBACK TAKEN");
		case EPinguStatKind::KnockbackDealt: return LOCTEXT("PingupediaStatKnockbackDealt", "KNOCKBACK DEALT");
		case EPinguStatKind::RunSpeed: return LOCTEXT("PingupediaStatRunSpeed", "RUN SPEED");
		case EPinguStatKind::DashSpeed: return LOCTEXT("PingupediaStatDashSpeed", "DASH SPEED");
		case EPinguStatKind::Jump: return LOCTEXT("PingupediaStatJump", "JUMP SPEED");
		case EPinguStatKind::AirJumps: return LOCTEXT("PingupediaStatAirJumps", "AIR JUMPS");
		case EPinguStatKind::FallGravity: return LOCTEXT("PingupediaStatFallGravity", "FALL GRAVITY");
		case EPinguStatKind::FallSpeed: return LOCTEXT("PingupediaStatFallSpeed", "MAX FALL SPEED");
		case EPinguStatKind::Size: return LOCTEXT("PingupediaStatSize", "SIZE");
		case EPinguStatKind::Grip: return LOCTEXT("PingupediaStatGrip", "GRIP");
		case EPinguStatKind::Braking: return LOCTEXT("PingupediaStatBraking", "BRAKING");
		case EPinguStatKind::AirControl: return LOCTEXT("PingupediaStatAirControl", "AIR CONTROL");
		case EPinguStatKind::ClimbReach: return LOCTEXT("PingupediaStatClimbReach", "LEDGE REACH");
		case EPinguStatKind::LedgeGrab: return LOCTEXT("PingupediaStatLedgeGrab", "LEDGE GRAB");
		case EPinguStatKind::BellySlide: return LOCTEXT("PingupediaStatBellySlide", "BELLY SLIDE");
		case EPinguStatKind::FreezeImmune: return LOCTEXT("PingupediaStatFreezeImmune", "FREEZE PROOF");
		case EPinguStatKind::Abilities: return LOCTEXT("PingupediaStatAbilities", "ABILITIES");
		case EPinguStatKind::Rescues: return LOCTEXT("PingupediaStatRescues", "RESCUES");
		case EPinguStatKind::WallBounce: return LOCTEXT("PingupediaStatWallBounce", "WALL BOUNCE");
		case EPinguStatKind::FloorBounce: return LOCTEXT("PingupediaStatFloorBounce", "FLOOR BOUNCE");
		case EPinguStatKind::Chance: return LOCTEXT("PingupediaStatChance", "CHANCE");
		case EPinguStatKind::FizzleChance: return LOCTEXT("PingupediaStatFizzleChance", "FIZZLE CHANCE");
		case EPinguStatKind::CooldownCut: return LOCTEXT("PingupediaStatCooldownCut", "COOLDOWN CUT");
		case EPinguStatKind::Recharge: return LOCTEXT("PingupediaStatRecharge", "RECHARGE");
		case EPinguStatKind::Cling: return LOCTEXT("PingupediaStatCling", "WALL CLING");
		case EPinguStatKind::MeltTime: return LOCTEXT("PingupediaStatMeltTime", "MELT TIME");
		case EPinguStatKind::SwapDelay: return LOCTEXT("PingupediaStatSwapDelay", "SWAP DELAY");
		case EPinguStatKind::ClimbReachForward: return LOCTEXT("PingupediaStatClimbReachForward", "LEDGE REACH FORWARD");
		case EPinguStatKind::ClingFallSpeed: return LOCTEXT("PingupediaStatClingFallSpeed", "WALL SLIDE");
		case EPinguStatKind::WallJump: return LOCTEXT("PingupediaStatWallJump", "WALL JUMP");
		case EPinguStatKind::ReturnSpeed: return LOCTEXT("PingupediaStatReturnSpeed", "SHOT BACK SPEED");
		case EPinguStatKind::RescueThrow: return LOCTEXT("PingupediaStatRescueThrow", "THROWN BACK TO THE ISLAND");
		case EPinguStatKind::MeltReach: return LOCTEXT("PingupediaStatMeltReach", "MELTS ICE WITHIN");
		case EPinguStatKind::ComboStep: return LOCTEXT("PingupediaStatComboStep", "PUSH PER COMBO HIT");
		case EPinguStatKind::ComboMax: return LOCTEXT("PingupediaStatComboMax", "COMBO PUSH CAP");
		case EPinguStatKind::Gravity: return LOCTEXT("PingupediaStatGravity", "GRAVITY");
		case EPinguStatKind::KnockbackGrowth: return LOCTEXT("PingupediaStatKnockbackGrowth", "KNOCKBACK GROWTH A SECOND");
		case EPinguStatKind::DamageStep: return LOCTEXT("PingupediaStatDamageStep", "FURTHER PER HIT TAKEN");
		case EPinguStatKind::Recoil: return LOCTEXT("PingupediaStatRecoil", "PUSHED BACK BY YOUR HITS");
		case EPinguStatKind::EdgeKnockOut: return LOCTEXT("PingupediaStatEdgeKnockOut", "PAST THE SIDES: OUT");
		default:
			if (bOutRaw)
			{
				*bOutRaw = true;
			}
			return FText::FromString(PinguTraits::GetStatKindName(Kind));
		}
	}

	/** How wide a text is in a font (Slate units); 0 before Slate can measure. */
	static float TextWidth(const FText& Text, const FSlateFontInfo& Font)
	{
		if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
		{
			return 0.f;
		}
		return static_cast<float>(FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, Font).X);
	}

	/** A stat line's strip: the name on the left, the number on the right. */
	static TSharedRef<SWidget> MakeStatCell(const FText& Label, const FText& Value, const FLinearColor& ValueColor)
	{
		const FPinguStyle& Style = FPinguStyle::Get();
		return SNew(SBox)
			.HeightOverride(StatHeight)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SPinguShape)
					.FillColor(PinguUI::WithAlpha(Style.Row, 0.6f))
					.Radius(8.f)
				]
				+ SOverlay::Slot()
				.Padding(FMargin(StatPadding, 0.f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(Label)
						.Font(StatLabelFont())
						.ColorAndOpacity(Style.Ice)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(StatValueGap, 0.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Text(Value)
						.Font(StatValueFont())
						.ColorAndOpacity(ValueColor)
					]
				]
			];
	}

	/** Stat lines two to a row, in their order. A line whose name and number don't fit half a row ("THROWN BACK TO THE
	 *  ISLAND") gets a whole row; the line before it then stays alone in its half. */
	static TSharedRef<SWidget> MakeStatGrid(const TArray<FPinguStatLine>& Lines, int32 Page, int32 Entry, const FLinearColor& ValueColor)
	{
		constexpr float HalfWidth = 0.5f * (DetailWidth - StatGap);
		// The rows: one or two lines each.
		TArray<TArray<int32>> GridRows;
		bool bOpenHalf = false;
		for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
		{
			const FPinguStatLine& Line = Lines[LineIndex];
			const float Needed = 2.f * StatPadding + StatValueGap + TextWidth(StatLabel(Line.Kind, Page, Entry, Line.bUpside), StatLabelFont())
				+ TextWidth(FormatValue(Line.Kind, Line.Value), StatValueFont());
			if (Needed > HalfWidth)
			{
				GridRows.Add({ LineIndex });
				GridRows.Last().Add(INDEX_NONE); // (a whole row)
				bOpenHalf = false;
			}
			else if (bOpenHalf)
			{
				GridRows.Last().Add(LineIndex);
				bOpenHalf = false;
			}
			else
			{
				GridRows.Add({ LineIndex });
				bOpenHalf = true;
			}
		}

		TSharedRef<SVerticalBox> Grid = SNew(SVerticalBox);
		for (int32 RowIndex = 0; RowIndex < GridRows.Num(); ++RowIndex)
		{
			const TArray<int32>& RowLines = GridRows[RowIndex];
			TSharedRef<SHorizontalBox> GridRow = SNew(SHorizontalBox);
			const bool bWhole = RowLines.Num() == 2 && RowLines[1] == INDEX_NONE;
			for (int32 Column = 0; Column < (bWhole ? 1 : 2); ++Column)
			{
				TSharedRef<SWidget> Cell = SNew(SSpacer);
				if (RowLines.IsValidIndex(Column) && Lines.IsValidIndex(RowLines[Column]))
				{
					const FPinguStatLine& Line = Lines[RowLines[Column]];
					Cell = MakeStatCell(StatLabel(Line.Kind, Page, Entry, Line.bUpside), FormatValue(Line.Kind, Line.Value), ValueColor);
				}
				GridRow->AddSlot()
				.FillWidth(1.f)
				.Padding(bWhole ? 0.f : (Column > 0 ? 0.5f * StatGap : 0.f), 0.f, bWhole ? 0.f : (Column == 0 ? 0.5f * StatGap : 0.f), 0.f)
				[
					Cell
				];
			}
			Grid->AddSlot()
			.AutoHeight()
			.Padding(0.f, RowIndex > 0 ? StatRowGap : 0.f, 0.f, 0.f)
			[
				GridRow
			];
		}
		return Grid;
	}

	/** A wrapped paragraph (what an ability does, a trait's upside / downside). It wraps at a fixed width (not the width it
	 *  got last frame), so a card's height is known before it shows: the fit check measures all of them. */
	static TSharedRef<SWidget> MakeParagraph(const FString& Text)
	{
		return SNew(STextBlock)
			.Text(FText::FromString(Text))
			.Font(FPinguStyle::Body(20.f))
			.ColorAndOpacity(FPinguStyle::Get().Snow)
			.WrapTextAt(DetailWidth - 8.f);
	}

	/** The selected entry's card: big icon, name and tags, then what it does and its numbers. */
	static TSharedRef<SWidget> BuildDetail(const APenguinHUD* PenguinHUD, int32 Page, int32 Entry)
	{
		const FPinguStyle& Style = FPinguStyle::Get();
		TSharedRef<SHorizontalBox> Tags = SNew(SHorizontalBox);
		auto AddTag = [&Tags](const FText& Text, const FLinearColor& Fill, const FLinearColor& TextColor)
		{
			Tags->AddSlot().AutoWidth().Padding(Tags->NumSlots() > 0 ? 8.f : 0.f, 0.f, 0.f, 0.f)
			[
				MakePill(Text, Fill, TextColor, 15.f)
			];
		};
		TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
		if (Page == TraitsPage)
		{
			const EPinguTrait Trait = TraitAt(Entry);
			const FPinguTraitInfo& Info = PinguTraits::GetInfo(Trait);
			AddTag(LOCTEXT("PingupediaTagTrait", "TRAIT"), Info.Color, Style.Ink);
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
				const TArray<FPinguStatLine>& SideLines = bUp ? Ups : Downs;
				Body->AddSlot()
				.AutoHeight()
				.Padding(0.f, 16.f, 0.f, 0.f)
				.HAlign(HAlign_Left)
				[
					bUp ? MakePill(LOCTEXT("PingupediaUpside", "UPSIDE"), Style.Green, Style.Ink, 16.f)
						: MakePill(LOCTEXT("PingupediaDownside", "DOWNSIDE"), Style.Red, Style.Snow, 16.f)
				];
				Body->AddSlot()
				.AutoHeight()
				.Padding(0.f, 8.f, 0.f, 0.f)
				[
					MakeParagraph(bUp ? Info.Upside : Info.Downside)
				];
				if (SideLines.Num() > 0)
				{
					Body->AddSlot()
					.AutoHeight()
					.Padding(0.f, 10.f, 0.f, 0.f)
					[
						MakeStatGrid(SideLines, Page, Entry, bUp ? Style.Green : Style.RedHot)
					];
				}
			}
		}
		else
		{
			const EPenguinAbility Ability = AbilityAt(Entry);
			const FPenguinAbilityInfo& Info = UPenguinAbilityComponent::GetInfo(Ability);
			if (UPenguinAbilityComponent::IsDeadly(Ability))
			{
				AddTag(LOCTEXT("PingupediaTagDeadly", "KNOCKS OUT"), Style.Red, Style.Snow);
			}
			if (UPenguinAbilityComponent::IsKnockback(Ability))
			{
				AddTag(LOCTEXT("PingupediaTagKnockback", "KNOCKBACK"), Style.Ice, Style.Ink);
			}
			TArray<FPinguStatLine> Lines;
			PinguTuning::GetAbilityStats(Ability, FindMovement(PenguinHUD), Lines);
			Body->AddSlot()
			.AutoHeight()
			.Padding(0.f, 20.f, 0.f, 0.f)
			[
				MakeParagraph(Info.Description)
			];
			Body->AddSlot()
			.AutoHeight()
			.Padding(0.f, 20.f, 0.f, 0.f)
			[
				MakeStatGrid(Lines, Page, Entry, Style.Snow)
			];
		}

		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakeEntryIcon(PenguinHUD, Page, Entry, BigIcon)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(24.f, 0.f, 0.f, 0.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(EntryName(Page, Entry))
						.Font(FPinguStyle::Title(44.f, 3))
						.ColorAndOpacity(Style.Snow)
						.ShadowOffset(FVector2D(0.f, 3.f))
						.ShadowColorAndOpacity(PinguUI::WithAlpha(Style.Ink, 0.6f))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
					[
						Tags
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Body
			];
	}

	/** The detail card shows Page / Entry (rebuilt only when that changed), from its top. */
	static void ShowDetail(FPinguPediaState& Pedia, const APenguinHUD* PenguinHUD)
	{
		if (!Pedia.Detail.IsValid() || (Pedia.ShownPage == Pedia.Page && Pedia.ShownEntry == Pedia.Entry))
		{
			return;
		}
		Pedia.ShownPage = Pedia.Page;
		Pedia.ShownEntry = Pedia.Entry;
		Pedia.Detail->SetContent(BuildDetail(PenguinHUD, Pedia.Page, Pedia.Entry));
		if (Pedia.DetailScroll.IsValid())
		{
			Pedia.DetailScroll->SetScrollOffset(0.f);
		}
	}
}

const PinguUI::FPingupediaStats& PinguUI::GetPingupediaStats()
{
	return PinguUIPingupediaPrivate::Stats();
}

FText PinguUI::PingupediaEntryName(int32 Page, int32 Entry)
{
	return PinguUIPingupediaPrivate::EntryName(Page, Entry);
}

FText PinguUI::PingupediaStatLabel(EPinguStatKind Kind, int32 Page, int32 Entry, bool bUpside, bool* bOutRaw)
{
	return PinguUIPingupediaPrivate::StatLabel(Kind, Page, Entry, bUpside, bOutRaw);
}

FText PinguUI::PingupediaFormatValue(EPinguStatKind Kind, float Value)
{
	return PinguUIPingupediaPrivate::FormatValue(Kind, Value);
}

const UPenguinMovementComponent* PinguUI::PingupediaFindMovement(const APenguinHUD* PenguinHUD)
{
	return PinguUIPingupediaPrivate::FindMovement(PenguinHUD);
}

PinguUI::FPingupediaStats& PinguUI::MutablePingupediaStats()
{
	return PinguUIPingupediaPrivate::Stats();
}

TSharedRef<SWidget> SPinguRoot::BuildPingupedia()
{
	using namespace PinguUIPingupediaPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();

	// The 3D PINGUPEDIA (PinguMenu3DPedia.cpp): the input layer only; the cards' fit numbers come from the 3D page's own
	// layout (APinguMenu3D::MeasurePediaCards) when it is first built.
	if (PinguMenu3D::IsEnabled())
	{
		TSharedRef<SPinguMenu3DInput> Input = SNew(SPinguMenu3DInput).HUD(HUD);
		FirstFocus.Add(static_cast<uint8>(EPinguScreen::Pingupedia), Input);
		return MakeScreen(EPinguScreen::Pingupedia, Input, false);
	}
	Pedia = MakeShared<FPinguPediaState>();

	// ---- the tabs: ABILITIES 16 | TRAITS 17 (orange: the page that shows)
	TSharedRef<SHorizontalBox> TabStrip = SNew(SHorizontalBox);
	for (int32 PageIndex = 0; PageIndex < NumPages; ++PageIndex)
	{
		const FText TabText = PageIndex == TraitsPage
			? FText::Format(LOCTEXT("PingupediaTabTraits", "TRAITS {0}"), FText::AsNumber(NumEntries(TraitsPage)))
			: FText::Format(LOCTEXT("PingupediaTabAbilities", "ABILITIES {0}"), FText::AsNumber(NumEntries(AbilitiesPage)));
		TabStrip->AddSlot().AutoWidth().Padding(PageIndex > 0 ? 8.f : 0.f, 0.f, 0.f, 0.f)
		[
			SNew(SPinguButton)
			.Kind(EPinguButtonKind::Segment)
			.Width(TabWidth)
			.Height(FPinguStyle::SmallHeight)
			.FontSize(20.f)
			.Text(TabText)
			.HUD(HUD)
			.IsSelected_Lambda([this, PageIndex]() { return Pedia.IsValid() && Pedia->Page == PageIndex; })
			.OnClicked_Lambda([this, PageIndex]() { if (Pedia.IsValid()) { SelectPingupediaPage(PageIndex, Pedia->Entry, false); } })
		];
	}

	// ---- the two lists (only the page that shows is visible; both have the same size, so nothing jumps)
	TSharedRef<SOverlay> Lists = SNew(SOverlay);
	int32 Counts[NumPages] = { 0, 0 };
	for (int32 PageIndex = 0; PageIndex < NumPages; ++PageIndex)
	{
		TSharedRef<SScrollBox> List = SNew(SScrollBox)
			.Style(&ScrollBoxLook())
			.ScrollBarStyle(&ScrollBarLook())
			.ScrollBarThickness(FVector2D(8.f, 8.f))
			.ScrollBarPadding(FMargin(6.f, 2.f, 0.f, 2.f))
			.NavigationDestination(EDescendantScrollDestination::IntoView)
			.NavigationScrollPadding(RowGap)
			.Visibility_Lambda([this, PageIndex]() { return Pedia.IsValid() && Pedia->Page == PageIndex ? EVisibility::Visible : EVisibility::Collapsed; });
		Pedia->Lists[PageIndex] = List;
		const int32 Count = NumEntries(PageIndex);
		for (int32 EntryIndex = 0; EntryIndex < Count; ++EntryIndex)
		{
			TSharedPtr<SPinguButton> RowButton;
			List->AddSlot()
			.Padding(0.f, EntryIndex > 0 ? RowGap : 0.f, 0.f, 0.f)
			[
				SAssignNew(RowButton, SPinguButton)
				.Kind(EPinguButtonKind::Row)
				.Height(RowHeight)
				.HAlign(HAlign_Fill)
				.ContentPadding(FMargin(0.f))
				.HUD(HUD)
				// Left / Right on the list switch the tabs (the row keeps its place).
				.OnStep_Lambda([this, PageIndex](int32 Direction)
				{
					if (Pedia.IsValid())
					{
						SelectPingupediaPage((PageIndex + Direction + NumPages) % NumPages, Pedia->Entry, true);
					}
				})
				.OnClicked_Lambda([this, PageIndex, EntryIndex]() { SelectPingupediaPage(PageIndex, EntryIndex, false); })
				// Hover or the keys put the focus on a row: that selects it. A row the keys reach is scrolled into view; one
				// the mouse is on is in view already (scrolling it would move the next row under the pointer).
				.OnFocused_Lambda([this, PageIndex, EntryIndex]()
				{
					if (!Pedia.IsValid() || Pedia->Page != PageIndex)
					{
						return;
					}
					if (Pedia->Entry != EntryIndex)
					{
						SelectPingupediaPage(PageIndex, EntryIndex, false);
					}
					const TSharedPtr<SWidget> FocusedRow = Pedia->Rows[PageIndex].IsValidIndex(EntryIndex) ? Pedia->Rows[PageIndex][EntryIndex] : nullptr;
					if (FocusedRow.IsValid() && !FocusedRow->IsHovered() && Pedia->Lists[PageIndex].IsValid())
					{
						Pedia->Lists[PageIndex]->ScrollDescendantIntoView(FocusedRow, true, EDescendantScrollDestination::IntoView, RowGap);
					}
				})
				[
					SNew(SOverlay)
					// The selected entry: a gold rim (it stays when the focus moves to a tab or BACK).
					+ SOverlay::Slot()
					[
						SNew(SPinguShape)
						.Visibility_Lambda([this, PageIndex, EntryIndex]()
						{
							return Pedia.IsValid() && Pedia->Page == PageIndex && Pedia->Entry == EntryIndex ? EVisibility::HitTestInvisible : EVisibility::Hidden;
						})
						.FillColor(PinguUI::WithAlpha(Style.Gold, 0.08f))
						.OutlineColor(Style.Gold)
						.OutlineWidth(FPinguStyle::LineWidth)
						.Radius(FPinguStyle::RowRadius)
					]
					+ SOverlay::Slot()
					.Padding(FMargin(8.f, 0.f, 12.f, 0.f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							MakeEntryIcon(HUD.Get(), PageIndex, EntryIndex, RowIcon)
						]
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(12.f, 0.f, 0.f, 0.f)
						[
							SNew(STextBlock)
							.Text(EntryName(PageIndex, EntryIndex))
							.Font(FPinguStyle::Title(20.f))
							.ColorAndOpacity(Style.Snow)
						]
					]
				]
			];
			Pedia->Rows[PageIndex].Add(RowButton);
		}
		Counts[PageIndex] = Pedia->Rows[PageIndex].Num();
		Lists->AddSlot()
		[
			List
		];
	}
	PinguUI::FPingupediaStats& PediaStats = PinguUI::MutablePingupediaStats();
	PediaStats.Abilities = Counts[AbilitiesPage];
	PediaStats.Traits = Counts[TraitsPage];
	// Every card must fit the detail panel without scrolling: measure all of them once (the menu screenshots log the
	// tallest).
	PediaStats.CardRoom = DetailRoom;
	PediaStats.TallestCard = 0.f;
	PediaStats.RawLabels = 0;
	PediaStats.RawLabelNames.Reset();
	// Every kind of stat has a name of its own, used on a card yet or not (a new kind shows up here first).
	for (int32 KindIndex = 0; KindIndex < static_cast<int32>(EPinguStatKind::Count); ++KindIndex)
	{
		bool bRaw = false;
		StatLabel(static_cast<EPinguStatKind>(KindIndex), AbilitiesPage, 0, true, &bRaw);
		if (bRaw)
		{
			++PediaStats.RawLabels;
			PediaStats.RawLabelNames += FString::Printf(TEXT("%s%s"), PediaStats.RawLabelNames.IsEmpty() ? TEXT("") : TEXT(" "),
				PinguTraits::GetStatKindName(static_cast<EPinguStatKind>(KindIndex)));
		}
	}
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
	{
		for (int32 PageIndex = 0; PageIndex < NumPages; ++PageIndex)
		{
			for (int32 EntryIndex = 0; EntryIndex < NumEntries(PageIndex); ++EntryIndex)
			{
				// Every stat line has a name of its own (no code names like "ClimbReachForward" on a card).
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
					StatLabel(Line.Kind, PageIndex, EntryIndex, Line.bUpside, &bRaw);
					if (bRaw)
					{
						++PediaStats.RawLabels;
						PediaStats.RawLabelNames += FString::Printf(TEXT("%s%s"), PediaStats.RawLabelNames.IsEmpty() ? TEXT("") : TEXT(" "), PinguTraits::GetStatKindName(Line.Kind));
					}
				}
				const TSharedRef<SWidget> Card = BuildDetail(HUD.Get(), PageIndex, EntryIndex);
				Card->SlatePrepass(1.f);
				const float CardHeight = static_cast<float>(Card->GetDesiredSize().Y);
				if (CardHeight > PediaStats.TallestCard)
				{
					PediaStats.TallestCard = CardHeight;
					PediaStats.TallestName = EntryName(PageIndex, EntryIndex).ToString();
				}
			}
		}
	}

	// ---- the detail card (filled by SelectPingupediaPage / OnPingupediaShown)
	TSharedRef<SScrollBox> DetailScroll = SNew(SScrollBox)
		.Style(&ScrollBoxLook())
		.ScrollBarStyle(&ScrollBarLook())
		.ScrollBarThickness(FVector2D(8.f, 8.f))
		+ SScrollBox::Slot()
		[
			SAssignNew(Pedia->Detail, SBox)
		];
	Pedia->DetailScroll = DetailScroll;

	TSharedRef<SPinguButton> Back = SNew(SPinguButton)
		.HUD(HUD)
		.Kind(EPinguButtonKind::Ghost)
		.Width(200.f)
		.Height(FPinguStyle::SmallHeight)
		.FontSize(20.f)
		.Text(LOCTEXT("PingupediaBack", "BACK"))
		.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Back(); } });
	Pedia->BackButton = Back;

	auto MakeWell = [&Style](const TSharedRef<SWidget>& Content, const FMargin& Padding)
	{
		return SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SPinguShape)
				.FillColor(PinguUI::WithAlpha(Style.PanelDeep, 0.75f))
				.OutlineColor(Style.Ink)
				.OutlineWidth(FPinguStyle::LineWidth)
				.Radius(FPinguStyle::RowRadius + 4.f)
			]
			+ SOverlay::Slot()
			.Padding(Padding)
			[
				Content
			];
	};

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		// The title, the tabs on the right.
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				MakeTitle(LOCTEXT("PingupediaTitle", "PINGUPEDIA"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				TabStrip
			]
		]
		// The list | the selected entry.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(ListWidth)
				.HeightOverride(BodyHeight)
				[
					MakeWell(Lists, FMargin(WellPadding))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(ListGap, 0.f, 0.f, 0.f)
			[
				SNew(SBox)
				.HeightOverride(BodyHeight)
				[
					MakeWell(DetailScroll, FMargin(DetailPadding, DetailPadding - 4.f, DetailPadding, DetailPadding))
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 20.f, 0.f, 0.f)
		.HAlign(HAlign_Left)
		[
			Back
		];

	ShowDetail(*Pedia, HUD.Get());
	return MakeScreen(EPinguScreen::Pingupedia,
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			MakeCard(Box, FMargin(40.f, 24.f, 40.f, 28.f), CardWidth)
		]);
}

TSharedPtr<SWidget> SPinguRoot::GetPingupediaFocus() const
{
	if (!Pedia.IsValid())
	{
		return nullptr;
	}
	// The selected entry's row (row 0 on a fresh visit), else BACK.
	const TArray<TSharedPtr<SWidget>>& PageRows = Pedia->Rows[FMath::Clamp(Pedia->Page, 0, 1)];
	return PageRows.IsValidIndex(Pedia->Entry) && PageRows[Pedia->Entry].IsValid() ? PageRows[Pedia->Entry] : Pedia->BackButton;
}

void SPinguRoot::OnPingupediaShown()
{
	using namespace PinguUIPingupediaPrivate;
	if (APinguMenu3D* Menu3D = PinguMenu3D::IsEnabled() && HUD.IsValid() ? APinguMenu3D::Get(HUD.Get()) : nullptr)
	{
		Menu3D->SelectPingupediaPage(AbilitiesPage, 0, false); // a fresh visit starts at the top
		return;
	}
	if (!Pedia.IsValid())
	{
		return;
	}
	// A fresh visit starts at the top: the abilities, the first one. The card is built again (the numbers of the penguins'
	// movement were not there yet when the UI was built).
	Pedia->Page = AbilitiesPage;
	Pedia->Entry = 0;
	Pedia->ShownPage = -1;
	for (const TSharedPtr<SScrollBox>& List : Pedia->Lists)
	{
		if (List.IsValid())
		{
			List->SetScrollOffset(0.f);
		}
	}
	ShowDetail(*Pedia, HUD.Get());
}

bool SPinguRoot::HandlePingupediaKey(const FKeyEvent& InKeyEvent)
{
	using namespace PinguUIPingupediaPrivate;
	const FKey Key = InKeyEvent.GetKey();
	if (Key != EKeys::Gamepad_LeftShoulder && Key != EKeys::Gamepad_RightShoulder)
	{
		return false;
	}
	const int32 Direction = Key == EKeys::Gamepad_RightShoulder ? 1 : -1;
	if (APinguMenu3D* Menu3D = PinguMenu3D::IsEnabled() && HUD.IsValid() ? APinguMenu3D::Get(HUD.Get()) : nullptr)
	{
		int32 PediaPage = 0;
		int32 PediaEntry = 0;
		Menu3D->GetPingupediaView(PediaPage, PediaEntry);
		if (HUD.IsValid())
		{
			HUD->PlayUISound(false);
		}
		Menu3D->SelectPingupediaPage((PediaPage + Direction + NumPages) % NumPages, PediaEntry, true);
		return true;
	}
	if (!Pedia.IsValid())
	{
		return false;
	}
	// LB / RB: the other tab (two tabs: either one flips), the focus on the same place in its list.
	if (HUD.IsValid())
	{
		HUD->PlayUISound(false);
	}
	SelectPingupediaPage((Pedia->Page + Direction + NumPages) % NumPages, Pedia->Entry, true);
	return true;
}

void SPinguRoot::SelectPingupediaPage(int32 Page, int32 Entry, bool bFocus)
{
	using namespace PinguUIPingupediaPrivate;
	if (APinguMenu3D* Menu3D = PinguMenu3D::IsEnabled() && HUD.IsValid() ? APinguMenu3D::Get(HUD.Get()) : nullptr)
	{
		Menu3D->SelectPingupediaPage(Page, Entry, bFocus);
		return;
	}
	if (!Pedia.IsValid())
	{
		return;
	}
	const int32 OldPage = Pedia->Page;
	const int32 NewPage = FMath::Clamp(Page, 0, NumPages - 1);
	const int32 NewEntry = FMath::Clamp(Entry, 0, NumEntries(NewPage) - 1);
	// A row of the page that goes away must not keep the focus (its list collapses): it moves to the new page's row.
	const bool bOldListFocused = OldPage != NewPage && Pedia->Lists[OldPage].IsValid() && Pedia->Lists[OldPage]->HasUserFocusedDescendants(0);
	Pedia->Page = NewPage;
	Pedia->Entry = NewEntry;
	ShowDetail(*Pedia, HUD.Get());

	const TSharedPtr<SWidget> Row = Pedia->Rows[NewPage].IsValidIndex(NewEntry) ? Pedia->Rows[NewPage][NewEntry] : nullptr;
	if (!Row.IsValid())
	{
		return;
	}
	// The new page's list shows its selected row (it scrolls once the list is laid out).
	if ((OldPage != NewPage || bFocus) && Pedia->Lists[NewPage].IsValid())
	{
		Pedia->Lists[NewPage]->ScrollDescendantIntoView(Row, false, EDescendantScrollDestination::IntoView, RowGap);
	}
	if (bFocus || bOldListFocused)
	{
		// The row shows after the next layout pass: the focus moves there from Tick.
		PendingFocus = Row;
		PendingFocusUntil = PinguUI::Now() + 0.5;
	}
}

bool SPinguRoot::GetPingupediaView(int32& OutPage, int32& OutEntry) const
{
	if (const APinguMenu3D* Menu3D = PinguMenu3D::IsEnabled() && HUD.IsValid() ? APinguMenu3D::Find(HUD->GetWorld()) : nullptr)
	{
		return Menu3D->GetPingupediaView(OutPage, OutEntry);
	}
	OutPage = Pedia.IsValid() ? Pedia->Page : -1;
	OutEntry = Pedia.IsValid() ? Pedia->Entry : -1;
	if (!Pedia.IsValid())
	{
		return false;
	}
	const TSharedPtr<SScrollBox>& List = Pedia->Lists[FMath::Clamp(Pedia->Page, 0, 1)];
	const TArray<TSharedPtr<SWidget>>& PageRows = Pedia->Rows[FMath::Clamp(Pedia->Page, 0, 1)];
	if (!List.IsValid() || !PageRows.IsValidIndex(Pedia->Entry) || !PageRows[Pedia->Entry].IsValid())
	{
		return false;
	}
	const FSlateRect ListRect = List->GetPaintSpaceGeometry().GetLayoutBoundingRect();
	const FSlateRect RowRect = PageRows[Pedia->Entry]->GetPaintSpaceGeometry().GetLayoutBoundingRect();
	return RowRect.GetSize().Y > 1.f && RowRect.Top >= ListRect.Top - 1.f && RowRect.Bottom <= ListRect.Bottom + 1.f;
}

#undef LOCTEXT_NAMESPACE
