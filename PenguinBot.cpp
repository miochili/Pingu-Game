// Pingu - computer-controlled penguins.

#include "PenguinBot.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinGameState.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PinguGiantSnowball.h"
#include "PinguSnowman.h"
#include "PinguPickup.h"
#include "PinguPortal.h"
#include "PinguRoundHazards.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace PenguinBotPrivate
{
	/** The giant snowball: used at a target on our level (within GiantMaxDz) between these distances, at this chance per think;
	 *  one rolling at us is jumped when its front is this close (cm). */
	constexpr float GiantMinRange = 150.f;
	constexpr float GiantMaxRange = 1300.f;
	constexpr float GiantMaxDz = 110.f;
	constexpr float GiantUseChance = 0.35f;
	constexpr float GiantJumpGap = 200.f;
	// The fish hook reaches APenguinProjectile::HookReach; bots throw it from a bit inside that.
	constexpr float HookMinRange = 200.f;
	constexpr float HookBotReach = 0.93f;       // share of the hook's reach
	constexpr float HookUseChance = 0.6f;
	// With the sea (or a gap) behind us the hook throws its catch over our head into it: always worth it.
	constexpr float HookSeaCheckBehind = 450.f;
	// The noot is a shotgun: only worth it up close (point blank: 3x a gust, little at its range of 480).
	constexpr float NootBotRange = 230.f;       // centre distance, x our size
	constexpr float NootBotMaxDz = 150.f;
	/** A bot builds a snowman only while fewer than this many of its own stand. */
	constexpr int32 BotMaxSnowmen = 2;
	static int32 CountSnowmenOf(const UWorld* World, const APenguinCharacter* Self)
	{
		TArray<APinguSnowman*> All;
		APinguSnowman::GetAll(World, All);
		int32 Count = 0;
		for (const APinguSnowman* Snowman : All)
		{
			Count += Snowman->GetBuilder() == Self ? 1 : 0;
		}
		return Count;
	}
	// Ice walls stay until broken.
	constexpr float IceWallDodgeChance = 0.3f;  // (lasting walls clutter the island: build them less often)
	constexpr float IceShootChance = 0.6f;      // shoot away the ice that blocks the line of sight
	constexpr float IceSightRange = 1000.f;
	constexpr float StuckBreakTime = 0.3f;      // stuck against ice this long: break it
	// From our chest to the middle of an ice block (x our size): the slap and noot break ice about this far, the tornado rips
	// it out (one layer) from this far.
	constexpr float SlapIceReach = 170.f;
	constexpr float NootIceReach = 260.f;
	constexpr float TornadoIceReach = 450.f;
	// The tornado (the Gust slot): only with somebody in its reach (chest to chest, x our size) it would throw toward the sea
	// - the edge of the island at most TornadoSeaReach beyond it the way it flies - or with two in reach. Never alone.
	constexpr float TornadoBotRange = 480.f;
	constexpr float TornadoSeaReach = 900.f;
	constexpr float TornadoSeaReachInAir = 1400.f; // one already in the air flies further
	// The snow bomb knocks out and pushes its own thrower too: a throw has to end outside our own blast, and every live bomb's
	// blast is left (ours at once, others' after the reaction time).
	constexpr float BombBodyRadius = 30.f;       // a bomb's own radius (x its size, APenguinProjectile): it goes off where it touches
	constexpr float BombThrowMargin = 90.f;      // a throw ends this far outside our own push radius (the bomb rolls and bounces on)
	constexpr float BombFleeMargin = 40.f;       // a live bomb's blast counts this much bigger ...
	constexpr float BombFleeHold = 80.f;         // ... and once we run from it, we run on until we are this much further out
	constexpr float BombFleeMaxFuse = 2.5f;      // a bomb with longer to go (frozen) is not run from yet
	constexpr float BombFleeDashShare = 0.55f;   // a dash when the run takes longer than this share of the fuse
	constexpr float BombFleeDashRoom = 100.f;    // ... and the way out is free this far past the run
	constexpr int32 FallingIceRows = 8;         // look this many blocks up for sand ice about to fall on us
	// Belly slide attack: the target this far away (centre to centre, x our size) - the slide has gone its three blocks when
	// it gets there and still has the speed to knock it away - on our level, with open ground for the whole slide.
	constexpr float SlideAttackMinRange = 420.f;
	constexpr float SlideAttackMaxRange = 700.f;
	constexpr float SlideAttackMaxDz = 60.f;        // the target at most this much higher ...
	constexpr float SlideAttackMaxBelow = 130.f;    // ... or this much lower (one block down: the slide goes over the edge)
	constexpr float SlideAttackClearAhead = 700.f; // a whole slide (about 6 blocks) and a little more
	constexpr float SlideAttackPastTarget = 150.f; // flat and open up to this far past the target
	constexpr int32 SlideAttackMaxDrop = 4;        // past that, ground at most this many blocks down
	constexpr float SlideAttackChance = 0.8f;  // x aggression, per look (every 0.25 - 0.6 s while in range)
	// Gaps one block high: a belly slide fits through (the capsule is 60 cm tall on the belly), standing up we don't.
	constexpr int32 LowGapLookCells = 3;          // look this many columns ahead for one
	constexpr int32 LowGapMaxCells = 20;          // a longer tunnel (we can't see its end) counts as a dead end
	constexpr float LowGapSlideDistance = 160.f;  // slide when the ceiling's face is this close to our front (cm)
	constexpr float LowGapJumpDistance = 25.f;    // one we can't go through: jump over it (or turn away) this close
	constexpr int32 LowGapMaxDrop = 4;            // past the far end the slide may go on: ground at most this many blocks down
	constexpr float LowGapCommitExtra = 1.f;      // s on top of crawling the whole tunnel before we think again
	constexpr float LowGapStartGrace = 0.3f;      // s for the slide press to come out
	constexpr float CrawlStuckTime = 0.35f;       // crawling without moving this long: a dead end, turn around
	// Portals (Snow Warp). Only a penguin's middle counts: it goes through when its middle comes into an end's oval.
	constexpr float PortalTripMinJump = 150.f;    // our place jumped this far between two thinks, from one end into the other ...
	constexpr float PortalTripMargin = 150.f;     // ... (ends grown by this much: we move up to ~140 cm between thinks): a trip
	constexpr float PortalLoopWindow = 6.f;       // two trips through the same portal this close together (s): a loop ...
	constexpr float PortalAvoidTime = 10.f;       // ... so we leave that portal alone this long
	constexpr float PortalStandMargin = 20.f;     // we stand in this end (just came out of it): not in our way
	constexpr float PortalMinGain = 300.f;        // a portal is worth it when it saves at least this much way to the goal
	constexpr float PortalShortcutReach = 800.f;  // we go out of our way to an end at most this far ...
	constexpr float PortalShortcutMinGoal = 700.f; // ... and only for a goal further than this
	constexpr int32 PortalExitMaxDrop = 8;        // the far end must have ground at most this many blocks under it
	constexpr float PortalEdgeKeepAway = 260.f;   // ... and be this far inside a deadly edge (Deadly Edges round)
	constexpr float PortalFallLoopReach = 60.f;   // an exit above its entry within the entry's half-width + this: falling out
	                                              // of it drops us right back in
	constexpr float PortalAirLook = 0.6f;         // in the air: our flight this many seconds ahead ...
	constexpr float PortalAirStep = 0.05f;        // ... sampled this often ...
	constexpr float PortalAirMargin = 15.f;       // ... against ends grown by this much
	constexpr float PortalRunUpTime = 0.3f;       // too close to an end to jump it: back off this long, then jump it ...
	constexpr float PortalRunUpRoom = 120.f;      // ... if no end we don't want is this close behind us
	constexpr float PortalOnWayGain = 40.f;       // an end right on our way: in when it saves this much (no detour)
	// Going in after standing around (UPenguinBotComponent::AggroIdleTime): how long it lasts per difficulty, how close it
	// gets, how much more eager it is.
	constexpr float AggroTime[3] = { 4.5f, 5.5f, 6.5f };
	constexpr float AggroMeleeRange = 90.f;       // close enough for the slap, stomp, noot, frost breath (x our size)
	constexpr float AggroRangedRange = 320.f;     // a shot from close by
	constexpr float AggroSlideRange = 480.f;      // nothing else: the belly slide (it needs 420 - 700 cm)
	constexpr float AggroEagerness = 0.7f;        // this share of the rest up to 1 is added to the aggression
	constexpr float AggroStallTime = 1.f;         // going in this long without one attack: the belly slide instead
	constexpr float AggroHitWindow = 0.3f;        // a penguin whose last attacker was us this recently: we landed a hit
	constexpr float AggroRetry = 1.f;             // no enemy to go for: look again after this long
	// Snow Warp: UPenguinAbilityComponent's warp goes at most WarpRange (700 cm, PenguinAbilities.cpp) toward the aim; the
	// bots warp only where that ends over ground at most this many blocks down.
	constexpr float WarpBotRange = 700.f;
	constexpr int32 WarpMaxDropCells = 4;
	// The icicle charges while it is held (UPenguinAbilityComponent::ChargeAndThrow): the longer the hold the faster and straighter
	// it flies, and a tap is only a weak 2 m lob - so the bots hold it for real: a short hold at point blank, half a second
	// just past it, a full second (2600 cm/s, perfectly straight) from a few metres.
	constexpr float IcicleBotRange = 1400.f;      // chest to chest
	constexpr float IcicleBotPointBlank = 300.f;  // closer: a short hold (no time to dodge it anyway)
	constexpr float IcicleBotQuickMin = 0.15f;    // a point-blank hold's charge, from ...
	constexpr float IcicleBotQuickMax = 0.3f;     // ... to
	constexpr float IcicleBotFullRange = 900.f;   // from here on a full charge
	constexpr float IcicleBotMidCharge = 0.5f;    // just past point blank (more toward the full range)
	constexpr float IcicleRecoveryHold = 0.3f;
	/** Flap off the island: first (before the air jumps) when this far (cm) below the top of the column we head for, and at
	 *  least this likely whatever the bot's skill (a fall it can survive). */
	constexpr float FlapFirstBelow = 250.f;
	constexpr float FlapRecoveryChance = 0.85f;
	/** Flap in a fight: up to a target this much (cm) higher, on the ground, now and then. */
	constexpr float FlapChaseUp = 350.f;    // off the island: no time for a long charge (the aim allows for the drop)
	// Traits mode (UPenguinTraitComponent). Without a trait none of this changes anything.
	constexpr int32 GapCellsNormal = 4;           // the widest gap (cells) a normal penguin jumps at (jump + double jump)
	constexpr int32 GapCellsHeavy = 3;            // Heavyweight / Giant: never a long gap (no double jump, or a huge body)
	constexpr int32 GapCellsMax = 5;              // IsEdgeAhead finds landings at most this far
	constexpr float SpeedsterEdgeStick = 0.6f;    // slippery fast feet: no full speed toward an edge
	constexpr float HotFeetIdleLimit = 0.55f;     // s on one block before we step off it (it melts after 1 s; thinks come
	                                              // every 0.06 - 0.11 s, so we are off it within about 0.7 s)
	constexpr float IceTouchRangeScale = 0.5f;    // Ice Touch: closer - its touch gives frozen feet
	constexpr float GlassCannonDodgeBonus = 0.2f; // Glass Cannon: knocked 1.5x as far, so it dodges more
	constexpr float MirrorReadyDodgeCut = 0.3f;   // Mirror ready: the first shot goes back anyway, so it dodges less
	constexpr float SecondChanceRecoveryScale = 0.5f; // Second Chance unused: a fish saves the first fall - fewer abilities spent
	constexpr float RubberBounceTime = 0.5f;      // Rubber: this long after a landing that bounced us up, no chained jumps
	constexpr float StickyHangMax = 0.2f;         // Sticky: this long on a wall, then we jump on (into the wall) or let go ...
	constexpr float StickyLetGoTime = 0.35f;      // ... pressing away from it this long (clear of it: it would hold us again)

	/** The tornado is worth it now: somebody it reaches (in TornadoBotRange of our chest, x our size, no wall between) that
	 *  its throw - away from us - sends toward the edge of the island (the sea), or two it reaches. */
	bool TornadoWorthIt(const APenguinCharacter* Self, const ABlockIsland* Island)
	{
		const UWorld* World = Self ? Self->GetWorld() : nullptr;
		const FBox Bounds = Island ? Island->GetBounds() : FBox(ForceInit);
		if (!World || !Bounds.IsValid)
		{
			return false;
		}
		const FVector From = Self->GetAimOrigin();
		const float Reach = TornadoBotRange * Self->GetSizeScale();
		int32 InReach = 0;
		bool bTowardSea = false;
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			const APenguinCharacter* Other = *It;
			if (Other == Self || !Other->IsAlive() || Other->IsHidden() || FVector::Dist(Other->GetAimOrigin(), From) > Reach
				|| !UPenguinAbilityComponent::HasClearLine(Island, Self, Other))
			{
				continue;
			}
			++InReach;
			const float Way = Other->GetActorLocation().X >= From.X ? 1.f : -1.f;
			const float ToEdge = Way > 0.f ? static_cast<float>(Bounds.Max.X - Other->GetActorLocation().X) : static_cast<float>(Other->GetActorLocation().X - Bounds.Min.X);
			const UPenguinMovementComponent* OtherMove = Other->GetPenguinMovement();
			const bool bInAir = OtherMove && !OtherMove->IsMovingOnGround();
			bTowardSea |= ToEdge < (bInAir ? TornadoSeaReachInAir : TornadoSeaReach);
		}
		return bTowardSea || InReach >= 2;
	}

	/** The penguin's trait (None without one, or without the component). */
	EPinguTrait TraitOf(const APenguinCharacter* Penguin)
	{
		const UPenguinTraitComponent* Traits = Penguin ? Penguin->GetTraits() : nullptr;
		return Traits ? Traits->GetTrait() : EPinguTrait::None;
	}

	/** The blast scale of a snow bomb this penguin throws (Traits mode: the throw's reach, sqrt of its strength; a Gambler's
	 *  double is the worst case). 1 without a trait. */
	float ExpectedBlastScale(const APenguinCharacter* Penguin)
	{
		const UPenguinTraitComponent* Traits = Penguin ? Penguin->GetTraits() : nullptr;
		if (!Traits)
		{
			return 1.f;
		}
		const float Strength = Traits->GetTrait() == EPinguTrait::Gambler ? PinguTraitTuning::GambleDoubleStrength : Traits->GetNumbers().Strength;
		return PinguTraitTuning::GetReachScale(Strength);
	}

	/** How high the trait lets the penguin get up a wall (jump, air jumps, ledge grab), compared with a normal penguin: 1
	 *  without a trait. Heavyweight (no double jump, a lower jump) about 0.57, Featherweight (triple jump) about 1.34. */
	float TraitClimbRatio(const APenguinCharacter* Penguin)
	{
		const UPenguinMovementComponent* Movement = Penguin ? Penguin->GetPenguinMovement() : nullptr;
		if (!Movement || TraitOf(Penguin) == EPinguTrait::None)
		{
			return 1.f;
		}
		// (Low / High Gravity: the same jump speed goes 1 / gravity as high.)
		const float JumpHeights = FMath::Square(Movement->GetTraitJumpScale()) / FMath::Max(Movement->GetTraitNumbers().GravityScale, 0.1f);
		const float Normal = Movement->JumpHeight + Movement->DoubleJumpHeight * Movement->MaxAirJumps + Movement->ClimbReachUp;
		const float Now = (Movement->JumpHeight + Movement->DoubleJumpHeight * Movement->GetMaxAirJumpsNow()) * JumpHeights
			+ (Movement->CanGrabLedge() ? Movement->GetEffectiveClimbReachUp() : 0.f);
		return Normal > 1.f ? Now / Normal : 1.f;
	}

	bool IsSolid(const ABlockIsland* Island, int32 Column, int32 Row)
	{
		return Island->IsSolidCell(FIntPoint(Column, Row));
	}

	/** A block in one of the MaxCells cells right under Cell. */
	bool HasGroundUnder(const ABlockIsland* Island, const FIntPoint& Cell, int32 MaxCells)
	{
		for (int32 Down = 1; Down <= MaxCells; ++Down)
		{
			if (IsSolid(Island, Cell.X, Cell.Y - Down))
			{
				return true;
			}
		}
		return false;
	}

	/** Floor right under every column from FromColumn to ToColumn (both included) in Row: a walk with no gap in it. */
	bool HasFloorBetween(const ABlockIsland* Island, int32 FromColumn, int32 ToColumn, int32 Row)
	{
		const int32 Step = ToColumn >= FromColumn ? 1 : -1;
		for (int32 Column = FromColumn; (ToColumn - Column) * Step >= 0; Column += Step)
		{
			if (!HasGroundUnder(Island, FIntPoint(Column, Row), 1))
			{
				return false;
			}
		}
		return true;
	}

	/** Sudden death: X is on the part of the island that crumbles (or soon will). */
	bool IsCrumblingAt(const ABlockIsland* Island, float X, float Margin)
	{
		return Island->IsCollapsing() && FMath::Abs(X - Island->GetCollapseCenterX()) > Island->GetCollapseRadius() - Margin;
	}

	/** A penguin coming out of a portal end at Point lands on the island: room there, ground not far below, not crumbling,
	 *  not at a deadly edge (Deadly Edges round). */
	bool IsSafePortalExit(const APenguinCharacter* Self, const ABlockIsland* Island, const FVector& Point)
	{
		const FIntPoint Cell = Island->WorldToCell(Point);
		if (Island->IsSolidCell(Cell) || !HasGroundUnder(Island, Cell, PortalExitMaxDrop)
			|| IsCrumblingAt(Island, static_cast<float>(Point.X), 150.f))
		{
			return false;
		}
		const APenguinGameState* EdgeState = Self ? APenguinGameState::Get(Self) : nullptr;
		if (EdgeState && EdgeState->HasDeadlyEdges())
		{
			const FVector2D Edges = EdgeState->GetDeadlyEdges();
			return Point.X > Edges.X + PortalEdgeKeepAway && Point.X < Edges.Y - PortalEdgeKeepAway;
		}
		return true;
	}

	/** Out is (about) straight above In: whoever falls out of Out drops right back into In - and out of Out again. */
	bool IsFallLoop(const FVector& In, const FVector& Out)
	{
		return Out.Z > In.Z + APinguPortal::EndHalfHeight
			&& FMath::Abs(static_cast<float>(Out.X - In.X)) < APinguPortal::EndHalfWidth + PortalFallLoopReach;
	}

	/** The portal's ends are known (a LAN client gets them a moment after the portal itself). */
	bool HasEnds(const APinguPortal* Portal)
	{
		return !(Portal->GetEnd(0).IsZero() && Portal->GetEnd(1).IsZero());
	}

	/** How much shorter the way from From to Goal is through a portal (walk to its end In, come out at Out). */
	float PortalGain(const FVector& From, const FVector& In, const FVector& Out, const FVector& Goal)
	{
		return static_cast<float>(FVector::Dist(From, Goal)) - (FMath::Abs(static_cast<float>(In.X - From.X)) + static_cast<float>(FVector::Dist(Out, Goal)));
	}

	// ---- size (shrink / grow rays: 0.5x .. 8x, an 8x penguin is about 9 m tall)

	/** How much higher a grown penguin's middle is than a normal one's on the same spot (0 at normal size or smaller). The
	 *  checks below were written for a normal penguin's middle: they compare from Middle - this. */
	float GrownHalfHeight(const APenguinCharacter* Penguin)
	{
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		return Capsule ? FMath::Max(0.f, Capsule->GetScaledCapsuleHalfHeight() - Capsule->GetUnscaledCapsuleHalfHeight()) : 0.f;
	}

	/** Rows of wall we jump right at (5 for a normal penguin). A grown penguin jumps higher (2x size: 1.41x the height, 8x:
	 *  2.83x - UPenguinMovementComponent::SetSizeScale); a small one stays at 5 (it tries, the second jump and the ledge
	 *  grab do the rest, as before). A trait changes it as far as it changes the climb (TraitClimbRatio). */
	int32 JumpableRows(const APenguinCharacter* Penguin)
	{
		return FMath::FloorToInt(5.f * TraitClimbRatio(Penguin) * FMath::Sqrt(FMath::Max(1.f, Penguin->GetSizeScale())) + 0.25f);
	}

	/** Rows a capsule this tall needs to fit in, with ~5 cm to spare (a normal penguin standing: 2, on the belly: 1; 8x: 10). */
	int32 RowsToFit(float Height, float Cell)
	{
		return FMath::FloorToInt((Height + 5.f) / FMath::Max(Cell, 1.f)) + 1;
	}

	/** Height of our belly capsule (it is clamped to at least the radius, SetLowCapsule, and scales with the penguin). */
	float BellyHeight(const APenguinCharacter* Penguin, const UPenguinMovementComponent* Movement)
	{
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		if (!Capsule || !Movement)
		{
			return 60.f;
		}
		return 2.f * FMath::Max(Movement->BellySlideHalfHeight, Capsule->GetUnscaledCapsuleRadius()) * Capsule->GetShapeScale();
	}

	/** The size a penguin is heading for (the rays stack on that). It is only known where the penguin is simulated or on the
	 *  server; elsewhere its current size. */
	float HeadingSize(const APenguinCharacter* Penguin)
	{
		return Penguin->HasAuthority() || Penguin->IsLocallySimulated() ? Penguin->GetSizeTarget() : Penguin->GetSizeScale();
	}

	/** One more step of a size ray on ourselves stays inside the size limits (past one it knocks us out: 8x pops, under 0.5x
	 *  we vanish). Strict, without APenguinCharacter::ChangeSize's slack: we never go near the edge on purpose. */
	bool SelfRayIsSafe(const APenguinCharacter* Self, bool bGrow)
	{
		const float Size = bGrow ? FMath::Max(Self->GetSizeScale(), HeadingSize(Self)) : FMath::Min(Self->GetSizeScale(), HeadingSize(Self));
		const float Wanted = Size * (bGrow ? PinguGrowRayFactor : PinguShrinkRayFactor);
		return Wanted < PinguMaxSize && Wanted > PinguMinSize;
	}

	/** A grow ray hit pops this penguin (it is at the biggest size already): a knock-out. */
	bool GrowRayPops(const APenguinCharacter* Penguin)
	{
		return HeadingSize(Penguin) * PinguGrowRayFactor > PinguMaxSize * (1.f + PinguSizeLimitSlack);
	}
}

namespace PenguinBotPrivate
{
	/** TrySlideAttack: how far the looks got (see UPenguinBotComponent::LogSlideStats). */
	static int32 SlideLooks[6] = { 0, 0, 0, 0, 0, 0 };
}

void UPenguinBotComponent::LogSlideStats(const TCHAR* Label)
{
	using namespace PenguinBotPrivate;
	UE_LOG(LogPingu, Display, TEXT("[BOTS] %s: belly slide looks %d, standing ready %d, in range %d, on our level %d, dice %d, open ground (slid) %d"),
		Label, SlideLooks[0], SlideLooks[1], SlideLooks[2], SlideLooks[3], SlideLooks[4], SlideLooks[5]);
	for (int32& Count : SlideLooks)
	{
		Count = 0;
	}
}

// ============================================================================ controller

APenguinBotController::APenguinBotController()
{
}

void APenguinBotController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
}

// ============================================================================ brain

UPenguinBotComponent::UPenguinBotComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

APenguinCharacter* UPenguinBotComponent::GetPenguin() const
{
	return Cast<APenguinCharacter>(GetOwner());
}

void UPenguinBotComponent::SetBotEnabled(bool bEnabled, int32 InDifficulty)
{
	bBotEnabled = bEnabled;
	Difficulty = FMath::Clamp(InDifficulty, 0, 2);
	switch (Difficulty)
	{
	case 0: Skill = { 0.45f, 16.f, 0.35f, 0.15f, 0.75f, false }; break;
	case 2: Skill = { 0.14f, 4.f, 0.9f, 0.75f, 1.f, true }; break;
	default: Skill = { 0.28f, 9.f, 0.6f, 0.45f, 0.95f, true }; break;
	}
	SetComponentTickEnabled(bEnabled);
	APenguinCharacter* Self = GetPenguin();
	Rand.Initialize(Self ? static_cast<int32>(GetTypeHash(Self->GetName())) : 7);
	Target.Reset();
	Move = FVector2D::ZeroVector;
	if (Self)
	{
		if (UPenguinMovementComponent* Movement = Self->GetPenguinMovement())
		{
			// Our inputs must reach the movement component before it simulates this frame.
			Movement->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
		}
		// Let go of the controls - where this machine drives the penguin (a remote player's input is its owner's).
		if (!bEnabled && Self->IsLocallySimulated())
		{
			Self->ApplyVirtualMoveInput(FVector2D::ZeroVector);
			Self->ReleaseJump();
		}
	}
	bJumpDown = false;
	PickupGoal.Reset();
	GapDeadline = -1.f;
	TunnelDeadline = -1.f;
	CrawlStuckTimer = 0.f;
	bHasThinkLocation = false;
	PortalMemory.Reset();
	PortalTrips = 0;
	PortalRunUpUntil = -1.f;
	bHasLastGoal = false;
	bAggressive = false;
	IdleTime = 0.f;
	AggroLeft = 0.f;
	AggroCount = 0;
	SeenHitEvents = -1;
	LastAggroUpdate = -1.f;
}

void UPenguinBotComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	APenguinCharacter* Self = GetPenguin();
	// Only where the penguin is simulated: a LAN client's copy of a server bot (or of another player) is not ours to drive.
	if (!bBotEnabled || !Self || !Self->IsLocallySimulated() || !Self->IsAlive() || Self->IsHidden())
	{
		if (Self && (!Self->IsAlive() || Self->IsHidden()))
		{
			// Out (or waiting to come back): the idle time starts over when we are in again.
			bAggressive = false;
			IdleTime = 0.f;
		}
		return;
	}
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!Movement)
	{
		return;
	}
	const float Dt = FMath::Min(DeltaTime, 0.1f);

	if (Movement->IsInputLocked())
	{
		Move = FVector2D::ZeroVector;
		Self->ApplyVirtualMoveInput(Move);
		if (bJumpDown)
		{
			Self->ReleaseJump();
			bJumpDown = false;
		}
		// Look at somebody while waiting for GO (standing still for it is no idling).
		IdleTime = 0.f;
		bAggressive = false;
		ChooseTarget(Self);
		if (const APenguinCharacter* T = Target.Get())
		{
			Self->SetAimPoint(T->GetAimOrigin());
		}
		return;
	}

	ThinkTimer -= Dt;
	TargetTimer -= Dt;
	AbilityTimer -= Dt;
	DodgeCooldown -= Dt;
	SlideTimer -= Dt;
	if (ThinkTimer <= 0.f)
	{
		const float Elapsed = Rand.FRandRange(0.06f, 0.11f);
		ThinkTimer = Elapsed;
		ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Self->GetActorLocation());
		Think(Self, Island, Elapsed);
		// In the air every way of thinking (chase, recovery, dodge) may fly us into a portal end we don't want: past it.
		SteerClearOfPortals(Self, Island);
	}
	HandleStickyWall(Self, Dt);

	if (Movement->IsSlippery() && !Movement->IsKnockedOut() && !Movement->IsBellySliding())
	{
		// Ice feet: the stick does nothing, so none is wasted (the vertical stick stays: it starts a belly slide). All it can do
		// is jump: when the slide heads for an edge with ground beyond it, hop over the gap (the jump keeps the speed).
		Move.X = 0.f;
		const float Vx = static_cast<float>(Movement->Velocity.X);
		if (Movement->IsMovingOnGround() && FMath::Abs(Vx) > 150.f)
		{
			if (const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Self->GetActorLocation()))
			{
				float GapCells = 0.f;
				if (IsEdgeAhead(Self, Island, Vx, GapCells, FMath::Abs(Vx) * 0.2f + 40.f) && GapCells > 0.f)
				{
					Jump(0.3f);
				}
			}
		}
	}

	Self->ApplyVirtualMoveInput(Move);
	if (bJumpDown)
	{
		JumpHold -= Dt;
		if (JumpHold <= 0.f)
		{
			Self->ReleaseJump();
			bJumpDown = false;
		}
	}
}

void UPenguinBotComponent::Jump(float HoldTime)
{
	APenguinCharacter* Self = GetPenguin();
	if (!Self || bJumpDown || bRubberBouncing)
	{
		return; // (Rubber: a landing bounced us back up - a press now would only burn the double jump)
	}
	// Lying under a ceiling too low to stand up a press only waits and comes out on the way out: none in there.
	if (const UPenguinMovementComponent* Movement = Self->GetPenguinMovement())
	{
		if (Movement->IsStuckLow())
		{
			return;
		}
	}
	Self->PressJump();
	bJumpDown = true;
	JumpHold = HoldTime;
}

void UPenguinBotComponent::ChooseTarget(APenguinCharacter* Self)
{
	// (A teammate is never a target: we play for the team - friendly fire is on, so our hits would hurt it - and the team
	// may have changed since.)
	if ((TargetTimer > 0.f || bAggressive) && Target.IsValid() && Target->IsAlive() && !Target->IsHidden() && !Self->IsTeammate(Target.Get()))
	{
		return; // (going in: on the one we picked until it ends)
	}
	if (bAggressive)
	{
		// Our pick is gone (out, or a teammate now): the next one, or back to normal.
		if (APenguinCharacter* Next = ChooseAggroTarget(Self, ABlockIsland::FindNear(GetWorld(), Self->GetActorLocation())))
		{
			Target = Next;
			return;
		}
		bAggressive = false;
		IdleTime = 0.f;
	}
	TargetTimer = Rand.FRandRange(1.5f, 3.f);
	APenguinCharacter* Best = nullptr;
	float BestScore = TNumericLimits<float>::Max();
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Other = *It;
		if (Other == Self || !Other->IsAlive() || Other->IsHidden() || Self->IsTeammate(Other))
		{
			continue;
		}
		float Score = FVector::Dist(Other->GetActorLocation(), Self->GetActorLocation());
		if (Other == Target.Get())
		{
			Score *= 0.8f; // stick with the current target a bit
		}
		Score *= Rand.FRandRange(0.85f, 1.15f);
		if (Score < BestScore)
		{
			BestScore = Score;
			Best = Other;
		}
	}
	Target = Best;
}

void UPenguinBotComponent::Think(APenguinCharacter* Self, ABlockIsland* Island, float Dt)
{
	ChooseTarget(Self);
	if (!Island)
	{
		Move = FVector2D::ZeroVector;
		return;
	}
	if (const APenguinCharacter* T = Target.Get())
	{
		Self->SetAimPoint(T->GetAimOrigin());
	}
	// An icicle charging: the aim it will be let go with - on the target as it moves (led for that charge), or the spot it
	// was meant for.
	if (const UPenguinAbilityComponent* OwnAbilities = Self->GetAbilities(); OwnAbilities && OwnAbilities->IsChargeKeyHeld())
	{
		const APenguinCharacter* Aimed = Target.Get();
		Self->SetAimPoint(bIcicleLeadsTarget && Aimed ? IcicleAim(Self, Aimed, IcicleChargePower) : IcicleFixedAim);
	}
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	const float Now = GetWorld()->GetTimeSeconds();
	// Rubber: a landing that bounced us back up (Jump ignores presses while it does; off the island Recover jumps anyway).
	if (Movement->LandEvents != LastLandEvents)
	{
		LastLandEvents = Movement->LandEvents;
		LastLandTime = Now;
	}
	bRubberBouncing = PenguinBotPrivate::TraitOf(Self) == EPinguTrait::Rubber && Movement->IsFalling() && Movement->Velocity.Z > 0.f
		&& Now - LastLandTime < PenguinBotPrivate::RubberBounceTime;
	// Through a portal since the last think? (Drops a gap jump or a low passage begun on the other side.)
	TrackPortalTrips(Self, Now);
	// Fighting, or standing around long enough to go in?
	UpdateAggro(Self, Island);
	// Mid gap jump: fly on to the far side (off-stage checks would turn us back halfway and drop us in).
	if (GapDeadline > 0.f)
	{
		const FVector Loc = Self->GetActorLocation();
		const float Feet = static_cast<float>(Self->GetFeetLocation().Z);
		if (Movement->IsMovingOnGround() || Now > GapDeadline || Feet < GapLandingTop - 60.f)
		{
			GapDeadline = -1.f; // landed, too late, or fell short: normal logic (recovery) again
		}
		else
		{
			const float Dir = FMath::Sign(GapLandingX - static_cast<float>(Loc.X));
			Move = FVector2D(Dir, 0.f);
			if (Movement->Velocity.Z < 0.f && Feet < GapLandingTop + 40.f && Movement->GetAirJumpsLeft() > 0)
			{
				Jump(0.3f);
			}
			return;
		}
	}
	// Sliding through a gap one block high (or crawling out from under a low ceiling): on until we are out - no jumps,
	// dodges or abilities in there.
	if (UpdateLowPassage(Self, Island, Dt))
	{
		return;
	}
	if (IsOffStage(Self, Island))
	{
		Recover(Self, Island);
		return;
	}
	if (AvoidGiantSnowball(Self, Island) || Dodge(Self, Island) || AvoidFallingIce(Self, Island))
	{
		return;
	}
	Navigate(Self, Island, Dt);
	if (TunnelDeadline > 0.f)
	{
		return; // Navigate just slid us into a low gap
	}
	if (!bFleeingBlast && TrySlideAttack(Self, Island))
	{
		NoteFight(false); // (a slide attack is an attack)
		return;
	}
	if (AbilityTimer <= 0.f)
	{
		if (UseAbilities(Self, Island))
		{
			if (!bLastUseNoAttack)
			{
				NoteFight(false);
			}
			AbilityTimer = Skill.Reaction + Rand.FRandRange(0.15f, 0.5f) / FMath::Max(CurrentAggression(), 0.1f);
		}
		else
		{
			AbilityTimer = Rand.FRandRange(0.05f, 0.15f) + Skill.Reaction * 0.3f;
		}
	}
}

void UPenguinBotComponent::HandleStickyWall(APenguinCharacter* Self, float Dt)
{
	const UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (StickyLetGoLeft > 0.f)
	{
		// Letting go: away from the wall for a moment (a stick let go next to it would stick us right back).
		StickyLetGoLeft -= Dt;
		Move.X = StickyLetGoDir;
		StickyHangTime = 0.f;
		return;
	}
	if (!Movement || !Movement->IsWallClinging())
	{
		StickyHangTime = 0.f;
		return;
	}
	StickyHangTime += Dt;
	const float WallDir = Movement->GetClingWallDir();
	if (StickyHangTime < PenguinBotPrivate::StickyHangMax || WallDir == 0.f)
	{
		return; // (a moment on it: maybe the think jumps on by itself)
	}
	if (FMath::Abs(Move.X) > 0.3f && FMath::Sign(Move.X) == WallDir)
	{
		Jump(0.3f); // our way is into the wall: up over its ledge when it is in reach, else a wall jump (and back to it)
	}
	else
	{
		// Nowhere into it: let go (pressing away from the wall).
		StickyLetGoLeft = PenguinBotPrivate::StickyLetGoTime;
		StickyLetGoDir = -WallDir;
		Move.X = StickyLetGoDir;
	}
}

bool UPenguinBotComponent::IsOffStage(const APenguinCharacter* Self, const ABlockIsland* Island) const
{
	const FVector Feet = Self->GetFeetLocation() + FVector(0.f, 0.f, 5.f);
	// (A grown penguin stands on a wider foot: its capsule can rest on an edge with its middle out over the sea.)
	const float Wide = 45.f * FMath::Max(1.f, Self->GetSizeScale());
	float GroundZ = 0.f;
	for (const float Offset : { 0.f, -Wide, Wide })
	{
		if (Island->FindGroundBelow(Feet + FVector(Offset, 0.f, 0.f), 2500.f, GroundZ))
		{
			return false;
		}
	}
	return true;
}

void UPenguinBotComponent::Recover(APenguinCharacter* Self, ABlockIsland* Island)
{
	const FVector Loc = Self->GetActorLocation();
	const FBox Bounds = Island->GetBounds();
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!Bounds.IsValid)
	{
		Move = FVector2D::ZeroVector;
		return;
	}
	// Heights are judged from where a normal penguin's middle would be (a grown one's middle is far above its feet).
	const float Grown = PenguinBotPrivate::GrownHalfHeight(Self);
	const float LocZ = static_cast<float>(Loc.Z) - Grown;

	// The nearest column with a floor on top, not much higher than we can climb.
	float BestX = Bounds.GetCenter().X;
	float BestTop = Bounds.Max.Z;
	float BestScore = TNumericLimits<float>::Max();
	const float Cell = Island->CellSize;
	for (int32 Step = -18; Step <= 18; ++Step)
	{
		const float X = (FMath::FloorToFloat(Loc.X / Cell) + Step + 0.5f) * Cell;
		float Top = 0.f;
		if (!Island->FindGroundBelow(FVector(X, 0.f, Bounds.Max.Z + 50.f), Bounds.Max.Z - Bounds.Min.Z + 200.f, Top))
		{
			continue;
		}
		const float Score = FMath::Abs(X - Loc.X) + FMath::Max(0.f, Top - LocZ) * 1.3f;
		if (Score < BestScore)
		{
			BestScore = Score;
			BestX = X;
			BestTop = Top;
		}
	}

	const float Dir = FMath::Sign(BestX - Loc.X);
	Move = FVector2D(Dir, 0.f);
	const bool bBelowGoal = LocZ < BestTop + 80.f;
	bRubberBouncing = false; // (off the island every jump counts)
	// Sticky: the island's wall holds it (no slide). A jump at once: up over the ledge when it is in reach, else a wall jump
	// (away and up; the double jump comes back with it), then the stick toward the wall again: the next touch sticks higher.
	const EPinguTrait Trait = PenguinBotPrivate::TraitOf(Self);
	if (Trait == EPinguTrait::Sticky && Movement->IsWallClinging())
	{
		Jump(0.3f);
		return;
	}
	// Second Chance not used yet this round: a fish throws us back out of the sea anyway - fewer abilities spent on it.
	const UPenguinTraitComponent* Traits = Self->GetTraits();
	const bool bFishLeft = Trait == EPinguTrait::SecondChance && Traits && Traits->GetEvents().SecondChanceAt < 0.f;
	// Flap: the best way back up - it catches the fall and climbs 7 m. First when we are well below the island's top (the
	// air jumps would not reach), else after them like any recovery ability (UseRecoveryAbility).
	const UPenguinAbilityComponent* OwnAbilities = Self->GetAbilities();
	const bool bFlapReady = OwnAbilities && OwnAbilities->IsReady(OwnAbilities->FindSlot(EPenguinAbility::Flap));
	if (bFlapReady && Movement->IsFalling() && Movement->Velocity.Z < 150.f && !Movement->IsFlapping() && LocZ < BestTop - PenguinBotPrivate::FlapFirstBelow
		&& Rand.FRand() < FMath::Max(Skill.Recovery, PenguinBotPrivate::FlapRecoveryChance))
	{
		FireAbility(Self, EPenguinAbility::Flap, FVector(BestX, 0.f, BestTop + 90.f));
		return;
	}
	if (Movement->IsFalling() && Movement->Velocity.Z < 150.f && !Movement->IsFlapping())
	{
		if (Movement->GetAirJumpsLeft() > 0 && (bBelowGoal || FMath::Abs(BestX - Loc.X) > 350.f))
		{
			Jump(0.3f);
		}
		else if (Movement->GetAirDashesLeft() > 0 && Movement->GetDashCooldownRemaining() <= 0.f && bBelowGoal)
		{
			Move = FVector2D(Dir * 0.7f, 0.7f);
			Self->ApplyVirtualMoveInput(Move);
			Self->PressDash();
		}
		else if (Rand.FRand() < Skill.Recovery * (bFishLeft ? PenguinBotPrivate::SecondChanceRecoveryScale : 1.f))
		{
			UseRecoveryAbility(Self, Island, FVector(BestX, 0.f, BestTop + 90.f));
		}
	}
}

bool UPenguinBotComponent::UseRecoveryAbility(APenguinCharacter* Self, const ABlockIsland* Island, const FVector& Goal)
{
	const UPenguinAbilityComponent* Abilities = Self->GetAbilities();
	if (!Abilities)
	{
		return false;
	}
	const FVector Loc = Self->GetActorLocation();
	// Goal is where a normal penguin's middle would be over the ground: a grown one is that much taller.
	const float Grown = PenguinBotPrivate::GrownHalfHeight(Self);
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::Flap)))
	{
		return FireAbility(Self, EPenguinAbility::Flap, Goal); // (the stick steers it over the island meanwhile: Recover)
	}
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::SnowWarp)))
	{
		return FireAbility(Self, EPenguinAbility::SnowWarp, Goal + FVector(0.f, 0.f, Grown));
	}
	// The hook pulls us to the ground it hits - but an ice block it would just pull out of the island.
	const FVector HookAim = Goal - FVector(0.f, 0.f, 60.f);
	const bool bIceThere = Island && Island->GetBlockAt(HookAim - FVector(0.f, 0.f, Island->CellSize * 0.5f)) == EBlockKind::Ice;
	if (!bIceThere && Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::FishHook))
		&& FVector::Dist(Loc, HookAim) < APenguinProjectile::HookReach * PenguinBotPrivate::HookBotReach)
	{
		return FireAbility(Self, EPenguinAbility::FishHook, HookAim);
	}
	const float Dir = FMath::Sign(Goal.X - Loc.X);
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::IceWall)))
	{
		// Just in front of us and under our feet (80 cm out and 130 down at normal size; past a grown one's belly). Past the
		// build limit (far out at the side): the next way back.
		const float Out = 50.f + FMath::Max(30.f, Self->GetCapsuleComponent()->GetScaledCapsuleRadius());
		if (FireAbility(Self, EPenguinAbility::IceWall, Loc + FVector(Dir * Out, 0.f, -130.f - Grown)))
		{
			return true;
		}
	}
	// Every ability is a movement tool. Recoil ones aim down and away from the island, lunges aim at it. The tornado holds
	// us in the air for a second and gives the double jump and air dash back (Recover jumps on after it).
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::Gust)) && Self->GetPenguinMovement() && Self->GetPenguinMovement()->GetAirJumpsLeft() == 0)
	{
		return FireAbility(Self, EPenguinAbility::Gust, Goal);
	}
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::FrostBreath)))
	{
		return FireAbility(Self, EPenguinAbility::FrostBreath, Loc + FVector(-Dir * 110.f, 0.f, -200.f));
	}
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::Noot)))
	{
		return FireAbility(Self, EPenguinAbility::Noot, Loc + FVector(-Dir * 120.f, 0.f, -200.f));
	}
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::FlipperSlap)))
	{
		return FireAbility(Self, EPenguinAbility::FlipperSlap, Loc + FVector(Dir * 150.f, 0.f, 130.f));
	}
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::Icicle)))
	{
		// It sticks into the cliff below us as a jump pad that throws us back up (no time for a long charge: a quick throw).
		return FireAbility(Self, EPenguinAbility::Icicle, Loc + FVector(Dir * 300.f, 0.f, -120.f), PenguinBotPrivate::IcicleRecoveryHold);
	}
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::Snowball)))
	{
		return FireAbility(Self, EPenguinAbility::Snowball, Loc + FVector(-Dir * 40.f, 0.f, -200.f));
	}
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::IceGun)))
	{
		return FireAbility(Self, EPenguinAbility::IceGun, Loc + FVector(-Dir * 40.f, 0.f, -200.f));
	}
	if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::IceShell)))
	{
		return FireAbility(Self, EPenguinAbility::IceShell, Loc);
	}
	return false;
}

float UPenguinBotComponent::StopLookahead(const APenguinCharacter* Self, float Direction) const
{
	const UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!Movement)
	{
		return 45.f;
	}
	const float Speed = FMath::Max(0.f, static_cast<float>(Movement->Velocity.X) * FMath::Sign(Direction));
	// (A trait's grip on top: Speedster's slippery feet steer back far slower; 1 without a trait.)
	float Brake = Movement->GroundTurnAcceleration * Movement->GetGripScale();
	if (Movement->IsOnIce())
	{
		Brake *= Movement->IceAccelerationScale;
		if (Movement->IsSlippery())
		{
			Brake *= Movement->SlipperyAccelerationScale; // ice gun feet brake even worse
		}
	}
	// One think interval (plus a frame of input lag) of travel, plus the braking distance.
	return FMath::Max(45.f, Speed * 0.15f + Speed * Speed / (2.f * FMath::Max(Brake, 100.f)));
}

bool UPenguinBotComponent::IsEdgeAhead(const APenguinCharacter* Self, const ABlockIsland* Island, float Direction, float& OutGapCells,
	float Lookahead, FIntPoint* OutLanding) const
{
	OutGapCells = 0.f;
	const FVector Feet = Self->GetFeetLocation();
	const FIntPoint FeetCell = Island->WorldToCell(Feet + FVector(0.f, 0.f, 5.f));
	const int32 GroundRow = FeetCell.Y - 1;
	const int32 Dir = Direction > 0.f ? 1 : -1;
	const FIntPoint Next = Island->WorldToCell(FVector(Feet.X + Dir * Lookahead, 0.f, Feet.Z + 5.f));
	if (Next.X == FeetCell.X && Island->IsSolidCell(FIntPoint(FeetCell.X, GroundRow)))
	{
		return false; // still solid under us and ahead
	}
	// (Next.X == FeetCell.X with nothing under the feet: we already hang over the edge - treat it as the edge.)
	if (Island->IsSolidCell(FIntPoint(Next.X, FeetCell.Y)))
	{
		return false; // a wall, not an edge
	}
	for (int32 Down = 0; Down <= 2; ++Down)
	{
		if (Island->IsSolidCell(FIntPoint(Next.X, GroundRow - Down)))
		{
			return false; // flat or a small step down
		}
	}
	// It's an edge: is there ground to jump to at about our height?
	for (int32 K = 1; K <= 5; ++K)
	{
		const int32 Column = Next.X + Dir * K;
		for (int32 Row = GroundRow - 1; Row <= GroundRow + 1; ++Row)
		{
			if (Island->IsSolidCell(FIntPoint(Column, Row)) && !Island->IsSolidCell(FIntPoint(Column, Row + 1)))
			{
				OutGapCells = static_cast<float>(K);
				if (OutLanding)
				{
					*OutLanding = FIntPoint(Column, Row);
				}
				return true;
			}
		}
	}
	// Or a lower floor we could drop onto?
	for (int32 Down = 3; Down <= 7; ++Down)
	{
		if (Island->IsSolidCell(FIntPoint(Next.X, GroundRow - Down)))
		{
			OutGapCells = -1.f;
			return true;
		}
	}
	return true;
}

int32 UPenguinBotComponent::WallHeightAhead(const APenguinCharacter* Self, const ABlockIsland* Island, float Direction) const
{
	const FVector Feet = Self->GetFeetLocation();
	// Look ~20 cm past the front of the capsule (radius 30 x size): a grown penguin's centre never gets within 50 cm of a wall.
	const float Probe = 20.f + 30.f * FMath::Max(1.f, Self->GetSizeScale());
	const FIntPoint Cell = Island->WorldToCell(FVector(Feet.X + FMath::Sign(Direction) * Probe, 0.f, Feet.Z + 10.f));
	// Counted up to one row more than we jump right at (6 at normal size; a grown penguin jumps higher walls).
	const int32 MaxHeight = PenguinBotPrivate::JumpableRows(Self) + 1;
	int32 Height = 0;
	while (Height < MaxHeight && Island->IsSolidCell(FIntPoint(Cell.X, Cell.Y + Height)))
	{
		++Height;
	}
	return Height;
}

float UPenguinBotComponent::PreferredRange(const APenguinCharacter* Self) const
{
	if (!Self->GetAbilities())
	{
		return 300.f;
	}
	bool bMeleeReady = false;
	bool bRangedReady = false;
	ReadyAttacks(Self, bMeleeReady, bRangedReady);
	// Glass Cannon is knocked 1.5x as far: it keeps its distance when it can shoot. Ice Touch comes closer (its touch freezes
	// feet).
	const EPinguTrait Trait = PenguinBotPrivate::TraitOf(Self);
	if (Trait == EPinguTrait::GlassCannon && bRangedReady)
	{
		return 480.f;
	}
	const float Range = bMeleeReady ? 110.f : (bRangedReady ? 480.f : 360.f);
	return Trait == EPinguTrait::IceTouch ? Range * PenguinBotPrivate::IceTouchRangeScale : Range;
}

void UPenguinBotComponent::ReadyAttacks(const APenguinCharacter* Self, bool& bMeleeReady, bool& bRangedReady) const
{
	bMeleeReady = false;
	bRangedReady = false;
	const UPenguinAbilityComponent* Abilities = Self->GetAbilities();
	if (!Abilities)
	{
		return;
	}
	for (int32 Slot = 0; Slot < Abilities->GetAbilities().Num(); ++Slot)
	{
		if (!Abilities->IsReady(Slot))
		{
			continue;
		}
		switch (Abilities->GetAbility(Slot))
		{
		case EPenguinAbility::FlipperSlap:
		case EPenguinAbility::Gust:
		case EPenguinAbility::FrostBreath:
		case EPenguinAbility::BellyStomp:
		case EPenguinAbility::Noot:
			bMeleeReady = true;
			break;
		case EPenguinAbility::Snowball:
		case EPenguinAbility::Icicle:
		case EPenguinAbility::SnowBomb:
		case EPenguinAbility::FishHook:
		case EPenguinAbility::IcebergDrop:
		case EPenguinAbility::IceGun:
		case EPenguinAbility::ShrinkRay:
			bRangedReady = true;
			break;
		default:
			break;
		}
	}
}

int32 UPenguinBotComponent::MaxGapCells(const APenguinCharacter* Self) const
{
	using namespace PenguinBotPrivate;
	const EPinguTrait Trait = TraitOf(Self);
	const UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (Trait == EPinguTrait::None || !Movement)
	{
		return GapCellsNormal;
	}
	// How far a jump carries compared with a normal penguin's: the running speed times the time in the air (the rise, and
	// the fall with its own gravity), the size's speed and jump on top; no double jump carries less, a triple jump more.
	const FPinguTraitNumbers& N = Movement->GetTraitNumbers();
	const float Fall = FMath::Max(Movement->FallGravityMultiplier, 0.1f);
	const float AirNormal = 1.f + 1.f / FMath::Sqrt(Fall);
	// (Low / High Gravity: every second in the air goes with 1 / gravity.)
	const float AirNow = (1.f + 1.f / FMath::Sqrt(FMath::Max(Fall * N.FallGravityScale, 0.1f))) / FMath::Max(N.GravityScale, 0.1f);
	const int32 AirJumps = Movement->GetMaxAirJumpsNow();
	const float AirJumpShare = AirJumps <= 0 ? 0.7f : (AirJumps >= 2 ? 1.15f : 1.f);
	const float SizeJump = FMath::Pow(FMath::Max(Self->GetSizeScale(), 0.1f), 0.25f);
	const float Reach = N.RunScale * Movement->GetSpeedScale() * Movement->GetTraitJumpScale() * SizeJump * AirNow / AirNormal * AirJumpShare;
	int32 Cells = FMath::Clamp(FMath::FloorToInt(GapCellsNormal * Reach + 0.25f), 1, GapCellsMax);
	if (Trait == EPinguTrait::Heavyweight || Trait == EPinguTrait::Giant)
	{
		Cells = FMath::Min(Cells, GapCellsHeavy);
	}
	return Cells;
}

bool UPenguinBotComponent::IsMirrorReady(const APenguinCharacter* Self) const
{
	const UPenguinTraitComponent* Traits = Self->GetTraits();
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	return Traits && GameState && Traits->HasTrait(EPinguTrait::Mirror) && GameState->GetFlowTime() >= Traits->GetEvents().MirrorReadyAt;
}

bool UPenguinBotComponent::AvoidMeltingBlock(const APenguinCharacter* Self, const ABlockIsland* Island, float Dt, float& Desired)
{
	using namespace PenguinBotPrivate;
	const UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!Island || !Movement || TraitOf(Self) != EPinguTrait::HotFeet || !Movement->IsMovingOnGround())
	{
		HotFeetTime = 0.f;
		HotFeetWay = 0.f;
		return false;
	}
	const FIntPoint Under = Island->WorldToCell(Self->GetFeetLocation() - FVector(0.f, 0.f, Island->CellSize * 0.5f));
	if (Under != HotFeetCell)
	{
		HotFeetCell = Under;
		HotFeetTime = 0.f;
		HotFeetWay = 0.f;
		return false;
	}
	HotFeetTime += Dt;
	if (HotFeetTime < HotFeetIdleLimit)
	{
		return false;
	}
	// Off it: the way we wanted to go (or wander), else the other way - never toward an edge (the edge checks in Navigate
	// would only turn us back, and we would dither on the melting block); kept until we are off it. Edges both ways (a
	// pillar): a hop.
	const auto Unsafe = [this, Self, Island](float Way)
	{
		float Gap = 0.f;
		return IsEdgeAhead(Self, Island, Way, Gap, FMath::Max(60.f, StopLookahead(Self, Way)));
	};
	if (HotFeetWay == 0.f || Unsafe(HotFeetWay))
	{
		float Way = FMath::Abs(Desired) > 0.3f ? FMath::Sign(Desired) : (FMath::Abs(WanderDir) > 0.1f ? FMath::Sign(WanderDir) : 1.f);
		if (Unsafe(Way))
		{
			Way = -Way;
		}
		if (Unsafe(Way))
		{
			Jump(0.25f);
			Desired = 0.f;
			HotFeetWay = 0.f;
			return true;
		}
		HotFeetWay = Way;
	}
	Desired = HotFeetWay;
	return true;
}

void UPenguinBotComponent::Navigate(APenguinCharacter* Self, ABlockIsland* Island, float Dt)
{
	const APenguinCharacter* T = Target.Get();
	const FVector Loc = Self->GetActorLocation();
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	float Desired = 0.f;
	// Where we are heading, when we head somewhere (chasing the target, going for a bubble): portals are judged by it.
	FVector Goal = FVector::ZeroVector;
	bool bHasGoal = false;

	if (T)
	{
		const FVector TLoc = T->GetActorLocation();
		const float Dx = TLoc.X - Loc.X;
		// (Heights from where a normal penguin's middle would be: a grown one's is far above its feet - a giant on our level
		// is not "on a platform above us", and a giant bot does not take a penguin on a platform for one below it.)
		const float Dz = (static_cast<float>(TLoc.Z) - PenguinBotPrivate::GrownHalfHeight(T))
			- (static_cast<float>(Loc.Z) - PenguinBotPrivate::GrownHalfHeight(Self));
		// Going in (after standing around): close to what we attack with, no backing off (only to the belly slide's distance
		// when that is all we have), no wandering.
		bool bSlideOnly = false;
		const float Range = bAggressive ? AggroRange(Self, bSlideOnly) : PreferredRange(Self);
		if (FMath::Abs(Dx) > Range + (bAggressive ? 30.f : 60.f))
		{
			Desired = FMath::Sign(Dx);
			Goal = TLoc;
			bHasGoal = true;
		}
		else if (bAggressive && !bSlideOnly)
		{
			// In reach: melee leans in a little (the slap and the stomp want contact) and holds still right on it; a shot
			// holds still where it is.
			const bool bMeleeReach = Range < PenguinBotPrivate::AggroRangedRange * 0.5f * FMath::Max(Self->GetSizeScale(), 1.f);
			Desired = bMeleeReach && FMath::Abs(Dx) > Range * 0.5f ? FMath::Sign(Dx) * 0.35f : 0.f;
		}
		else if (Range > 250.f && FMath::Abs(Dx) < Range - (bAggressive ? 60.f : 140.f))
		{
			Desired = -FMath::Sign(Dx); // ranged kit: back off a little (going in with the slide only: to its distance)
		}
		else if (bAggressive)
		{
			Desired = 0.f; // the belly slide's distance: wait for the slide
		}
		else
		{
			WanderTimer -= Dt;
			if (WanderTimer <= 0.f)
			{
				WanderTimer = Rand.FRandRange(0.35f, 1.1f);
				const float Roll = Rand.FRand();
				WanderDir = Roll < 0.35f ? -1.f : (Roll < 0.65f ? 0.f : 1.f);
			}
			Desired = WanderDir * 0.6f;
		}

		// Up onto the target's platform.
		if (Dz > 160.f && FMath::Abs(Dx) < 280.f && Movement->IsMovingOnGround())
		{
			Jump(0.35f);
		}
		if (Dz > 120.f && Movement->IsFalling() && Movement->Velocity.Z < 60.f && Movement->GetAirJumpsLeft() > 0 && FMath::Abs(Dx) < 400.f)
		{
			Jump(0.3f);
		}
	}
	else
	{
		WanderTimer -= Dt;
		if (WanderTimer <= 0.f)
		{
			WanderTimer = Rand.FRandRange(0.8f, 2.f);
			WanderDir = Rand.FRand() < 0.5f ? -1.f : 1.f;
		}
		Desired = WanderDir * 0.7f;
	}

	// An ability bubble close by: grab it (the edge, wall and sudden-death checks below still keep us safe).
	if (const APinguPickup* Pickup = ChoosePickup(Self, Island))
	{
		const FVector Bubble = Pickup->GetIconLocation();
		const float PDx = static_cast<float>(Bubble.X - Loc.X);
		// (From as far below our head as a normal penguin's middle: a giant reaches a bubble high up without a jump.)
		const float PDz = static_cast<float>(Bubble.Z - Loc.Z) - PenguinBotPrivate::GrownHalfHeight(Self);
		Desired = FMath::Abs(PDx) > 20.f ? FMath::Sign(PDx) : 0.f;
		Goal = Bubble;
		bHasGoal = true;
		// Up to the platform it lies on, or jump into it in the air.
		if (PDz > 110.f && FMath::Abs(PDx) < 260.f && Movement->IsMovingOnGround())
		{
			Jump(PDz > 220.f ? 0.35f : 0.2f);
		}
		if (PDz > 80.f && Movement->IsFalling() && Movement->Velocity.Z < 60.f && Movement->GetAirJumpsLeft() > 0 && FMath::Abs(PDx) < 300.f)
		{
			Jump(0.3f);
		}
	}

	// A bomb about to go off with us in its blast (our own too): out of it first, whatever else we wanted.
	if (AvoidBlast(Self, Island, Desired))
	{
		bHasGoal = false;
	}

	// Sudden death: get off the part of the island that is crumbling, toward the middle.
	if (Island && Island->IsCollapsing())
	{
		const float FromCenter = static_cast<float>(Loc.X) - Island->GetCollapseCenterX();
		if (FMath::Abs(FromCenter) > FMath::Max(Island->GetCollapseRadius() - 230.f, 50.f))
		{
			Desired = -FMath::Sign(FromCenter);
			bHasGoal = false; // (just away from the crumbling part: no portal is worth it)
		}
	}
	// The round's hazard (PinguRoundHazards.h): off crumbling blocks, out of a rain ring, against the wind near an edge.
	if (APinguRoundHazards::SteerBot(Self, Island, Desired))
	{
		bHasGoal = false;
	}
	// Deadly Edges (the round's trait, 2026-11-07): never toward a deadly side once close to it - back in instead.
	if (const APenguinGameState* EdgeState = APenguinGameState::Get(Self))
	{
		if (EdgeState->HasDeadlyEdges())
		{
			const FVector2D Edges = EdgeState->GetDeadlyEdges();
			constexpr float EdgeKeepAway = 260.f;
			if ((Desired < 0.f && Loc.X < Edges.X + EdgeKeepAway) || (Desired > 0.f && Loc.X > Edges.Y - EdgeKeepAway))
			{
				Desired = -FMath::Sign(Desired) * 0.6f;
				bHasGoal = false;
			}
		}
	}

	// Portals: through one that takes us closer to the goal, over the others (they would throw us somewhere else).
	LastGoal = Goal;
	bHasLastGoal = bHasGoal;
	HandlePortals(Self, Island, Desired, bHasGoal ? &Goal : nullptr);

	// Hot Feet: the block we stand on melts after a second - off it before then.
	AvoidMeltingBlock(Self, Island, Dt, Desired);

	// Walls and edges.
	bool bPressedOnPurpose = false; // against a low gap's ceiling, waiting for the slide: no stuck hop
	if (FMath::Abs(Desired) > 0.1f)
	{
		const float Dir = FMath::Sign(Desired);
		if (Movement->IsMovingOnGround())
		{
			// Speedster: fast, slippery feet - never at full speed toward an edge (a gap we jump is taken at full speed below).
			if (PenguinBotPrivate::TraitOf(Self) == EPinguTrait::Speedster && FMath::Abs(Desired) > PenguinBotPrivate::SpeedsterEdgeStick)
			{
				float NearGap = 0.f;
				if (IsEdgeAhead(Self, Island, Dir, NearGap, 2.f * StopLookahead(Self, Dir)) && !(NearGap > 0.f && NearGap <= MaxGapCells(Self)))
				{
					Desired = Dir * PenguinBotPrivate::SpeedsterEdgeStick;
				}
			}
			float Gap = 0.f;
			FIntPoint Landing(0, 0);
			// Look far enough ahead to stop in time at our speed (on ice that is a long way).
			if (IsEdgeAhead(Self, Island, Dir, Gap, StopLookahead(Self, Dir), &Landing))
			{
				const bool bTargetBeyond = T && FMath::Sign(T->GetActorLocation().X - Loc.X) == Dir;
				const bool bTargetBelow = T && T->GetActorLocation().Z < Loc.Z - 150.f;
				if (Gap > 0.f && Gap <= static_cast<float>(MaxGapCells(Self)) && (bTargetBeyond || !T || bFleeingBlast))
				{
					// Take off at the edge itself, not early, and commit to the far side.
					float NearGap = 0.f;
					if (IsEdgeAhead(Self, Island, Dir, NearGap, 45.f))
					{
						Jump(0.35f);
						const FVector LandingCenter = Island->CellToWorld(Landing);
						GapLandingX = static_cast<float>(LandingCenter.X);
						GapLandingTop = static_cast<float>(LandingCenter.Z) + Island->CellSize * 0.5f;
						GapDeadline = GetWorld()->GetTimeSeconds() + 1.2f;
					}
					Desired = Dir;
				}
				else if (Gap < 0.f && bTargetBelow && bTargetBeyond)
				{
					Desired = Dir; // drop down to the lower floor
				}
				else
				{
					// Don't walk off: brake (steer back) while we still have speed.
					const float Vx = static_cast<float>(Movement->Velocity.X);
					Desired = FMath::Abs(Vx) > 50.f && Vx * Dir > 0.f ? -Dir : 0.f;
					WanderDir = -Dir;
				}
			}
			const int32 Wall = WallHeightAhead(Self, Island, Dir);
			const int32 Jumpable = PenguinBotPrivate::JumpableRows(Self); // 5 rows at normal size, more for a grown penguin
			// A gap too low to stand in (one block high for a normal penguin; a giant fits under nothing lower than itself):
			// standing up we would just run into its ceiling (and hop there for ever).
			FLowGap LowGap;
			const bool bLowGap = Wall == 0 && FMath::Sign(Desired) == Dir && FindLowGapAhead(Self, Island, Dir, LowGap);
			if (bLowGap)
			{
				bPressedOnPurpose = true;
				if (LowGap.bPassable)
				{
					// Through on the belly: the slide fits, and under the ceiling it crawls on until it is out. Not ready
					// yet (the slide's cooldown, a jump still held): on up to it, it will be.
					if (LowGap.Distance < PenguinBotPrivate::LowGapSlideDistance && StartLowPassage(Self, Island, Dir, LowGap))
					{
						Desired = Dir;
					}
				}
				else if (LowGap.Distance < PenguinBotPrivate::LowGapJumpDistance)
				{
					// A dead end (or a slide that would go on into the sea): over the top of it like a wall, break an ice
					// ceiling, or turn away from it.
					if (LowGap.OverHeight <= Jumpable)
					{
						Jump(LowGap.OverHeight >= 3 ? 0.35f : 0.22f);
					}
					else if (!BreakIceAhead(Self, Island, Dir))
					{
						Desired = 0.f;
						WanderDir = -Dir;
					}
				}
			}
			// Ice walls stay until broken: one too high to jump (or ice we keep running into, say the half of a wall left
			// above a broken block) gets broken instead.
			else if ((Wall > Jumpable || StuckTimer > PenguinBotPrivate::StuckBreakTime) && BreakIceAhead(Self, Island, Dir))
			{
				StuckTimer = 0.f;
			}
			else if (Wall > 0 && Wall <= Jumpable)
			{
				Jump(Wall >= 3 ? 0.35f : 0.22f);
			}
		}
		else if (Movement->Velocity.Z < 0.f && Movement->GetAirJumpsLeft() > 0 && WallHeightAhead(Self, Island, Dir) > 0)
		{
			Jump(0.3f); // didn't clear the wall yet
		}
		else if (Movement->IsFalling() && Movement->GetAirJumpsLeft() <= 0 && Movement->CanGrabLedge() && WallHeightAhead(Self, Island, Dir) > 0)
		{
			Jump(0.1f); // out of jumps: grab the ledge and climb up (Rubber can't)
		}
	}
	else if (Movement->IsOnIce() && Movement->IsMovingOnGround() && FMath::Abs(static_cast<float>(Movement->Velocity.X)) > 50.f)
	{
		// Stick let go but still sliding on ice: brake if the slide would take us over an edge into the sea.
		// (Off ice the normal braking stops us within ~30 cm anyway.)
		const float CoastVx = static_cast<float>(Movement->Velocity.X);
		const float CoastDir = FMath::Sign(CoastVx);
		float CoastGap = 0.f;
		if (IsEdgeAhead(Self, Island, CoastDir, CoastGap, StopLookahead(Self, CoastDir)) && CoastGap >= 0.f)
		{
			Desired = -CoastDir;
		}
	}

	// Stuck against something? Hop.
	if (bPressedOnPurpose)
	{
		StuckTimer = 0.f;
	}
	else if (FMath::Abs(Desired) > 0.3f && Movement->IsMovingOnGround())
	{
		StuckTimer = FVector::Dist(Loc, LastLocation) < 2.f ? StuckTimer + Dt : 0.f;
		if (StuckTimer > 0.45f)
		{
			Jump(0.35f);
			StuckTimer = 0.f;
		}
	}
	LastLocation = Loc;

	Move = FVector2D(FMath::Clamp(Desired, -1.f, 1.f), 0.f);
}

const APinguPickup* UPenguinBotComponent::ChoosePickup(const APenguinCharacter* Self, const ABlockIsland* Island)
{
	TArray<APinguPickup*> Pickups;
	APinguPickup::GetAll(GetWorld(), Pickups);
	const UPenguinAbilityComponent* Abilities = Self->GetAbilities();
	if (Pickups.Num() == 0 || !Island || !Abilities)
	{
		PickupGoal.Reset();
		return nullptr;
	}
	const FVector Loc = Self->GetActorLocation();
	// Bolder bots go further for them.
	const float Reach = 450.f + 650.f * Skill.Aggression;
	const APinguPickup* Best = nullptr;
	float BestScore = TNumericLimits<float>::Max();
	for (const APinguPickup* Pickup : Pickups)
	{
		const FVector Bubble = Pickup->GetIconLocation();
		const float Dx = FMath::Abs(static_cast<float>(Bubble.X - Loc.X));
		const float Dz = static_cast<float>(Bubble.Z - Loc.Z);
		if (Dx > Reach || Dz > 450.f || Dz < -500.f || Pickup->IsOnHook())
		{
			continue; // too far, or on somebody's fish hook already
		}
		// Not over the sea, not on ground that is about to crumble away.
		float GroundZ = 0.f;
		if (!Island->FindGroundBelow(Bubble, 3000.f, GroundZ))
		{
			continue;
		}
		if (Island->IsCollapsing() && FMath::Abs(static_cast<float>(Bubble.X) - Island->GetCollapseCenterX()) > Island->GetCollapseRadius() - 200.f)
		{
			continue;
		}
		// Something we already have and can use right now is worth nothing; one on cooldown is a recharge.
		const int32 OwnSlot = Abilities->FindSlot(Pickup->GetAbility());
		if (OwnSlot != INDEX_NONE && Abilities->IsReady(OwnSlot))
		{
			continue;
		}
		float Score = Dx + FMath::Max(Dz, 0.f) * 1.2f + (OwnSlot != INDEX_NONE ? 300.f : 0.f);
		if (Pickup == PickupGoal.Get())
		{
			Score *= 0.8f; // stick with it
		}
		if (Score < BestScore)
		{
			BestScore = Score;
			Best = Pickup;
		}
	}
	PickupGoal = Best;
	return Best;
}

bool UPenguinBotComponent::HasLineOfSight(const FVector& From, const FVector& To) const
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguBotSight), false, GetOwner());
	return !GetWorld()->LineTraceTestByObjectType(From, To, FCollisionObjectQueryParams(ECC_WorldStatic), Params);
}

bool UPenguinBotComponent::IceBlocksSight(const ABlockIsland* Island, const FVector& From, const FVector& To, FVector& OutIceSpot) const
{
	if (!Island)
	{
		return false;
	}
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguBotIce), false, GetOwner());
	if (!GetWorld()->LineTraceSingleByObjectType(Hit, From, To, FCollisionObjectQueryParams(ECC_WorldStatic), Params)
		|| FVector::Dist(From, Hit.ImpactPoint) > PenguinBotPrivate::IceSightRange)
	{
		return false;
	}
	const FVector Inside = FVector(Hit.ImpactPoint) - FVector(Hit.ImpactNormal) * 20.f;
	if (Island->GetBlockAt(Inside) != EBlockKind::Ice)
	{
		return false;
	}
	OutIceSpot = Island->CellToWorld(Island->WorldToCell(Inside));
	return true;
}

bool UPenguinBotComponent::FireAtIce(APenguinCharacter* Self, const FVector& IceSpot)
{
	const UPenguinAbilityComponent* Abilities = Self->GetAbilities();
	if (!Abilities)
	{
		return false;
	}
	auto Ready = [Abilities](EPenguinAbility Ability) { return Abilities->IsReady(Abilities->FindSlot(Ability)); };
	const float Size = Self->GetSizeScale();
	const float Dist = static_cast<float>(FVector::Dist(Self->GetAimOrigin(), IceSpot));
	// Up close: the flipper (the block in front), the noot (the nearest ones in its cone), the tornado (the ones round us,
	// ripped out) - each within the reach of its ice breaking.
	if (Dist < PenguinBotPrivate::SlapIceReach * Size && Ready(EPenguinAbility::FlipperSlap))
	{
		return FireAbility(Self, EPenguinAbility::FlipperSlap, IceSpot);
	}
	if (Dist < PenguinBotPrivate::NootIceReach * Size && Ready(EPenguinAbility::Noot))
	{
		return FireAbility(Self, EPenguinAbility::Noot, IceSpot);
	}
	if (Dist < PenguinBotPrivate::TornadoIceReach * Size && Ready(EPenguinAbility::Gust))
	{
		return FireAbility(Self, EPenguinAbility::Gust, IceSpot);
	}
	// Shots break the block they hit (the hook pulls it out).
	for (const EPenguinAbility Shot : { EPenguinAbility::Snowball, EPenguinAbility::Icicle, EPenguinAbility::FishHook })
	{
		if (Ready(Shot))
		{
			return FireAbility(Self, Shot, IceSpot);
		}
	}
	if (Ready(EPenguinAbility::ShrinkRay) && Dist > (UPenguinAbilityComponent::SelfCastRadius + 20.f) * Size)
	{
		return FireAbility(Self, EPenguinAbility::ShrinkRay, IceSpot);
	}
	// Warping into ice shatters it (and takes us through), a bomb blows it up (from far enough: our own blast knocks us
	// out too). (Not into ice over the sea: we would fall in after it.)
	if (Ready(EPenguinAbility::SnowWarp) && WarpLandsSafely(Self, nullptr, IceSpot))
	{
		return FireAbility(Self, EPenguinAbility::SnowWarp, IceSpot);
	}
	if (Ready(EPenguinAbility::SnowBomb) && BombThrowIsSafe(Self, ABlockIsland::FindNear(GetWorld(), Self->GetActorLocation()), IceSpot))
	{
		return FireAbility(Self, EPenguinAbility::SnowBomb, IceSpot);
	}
	return false;
}

bool UPenguinBotComponent::BreakIceAhead(APenguinCharacter* Self, const ABlockIsland* Island, float Direction)
{
	if (!Island)
	{
		return false;
	}
	// The lowest ice block in our way, from the feet up to the head (a one-block gap is too low to pass).
	const FVector Feet = Self->GetFeetLocation();
	const float Probe = 20.f + 30.f * FMath::Max(1.f, Self->GetSizeScale());
	const float Height = 2.f * Self->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	for (float Up = 10.f; Up < Height + Island->CellSize * 0.5f; Up += Island->CellSize)
	{
		const FVector At(Feet.X + FMath::Sign(Direction) * Probe, 0.f, Feet.Z + FMath::Min(Up, Height - 10.f));
		const EBlockKind Block = Island->GetBlockAt(At);
		if (Block == EBlockKind::Ice)
		{
			return FireAtIce(Self, Island->CellToWorld(Island->WorldToCell(At)));
		}
	}
	return false;
}

bool UPenguinBotComponent::AvoidFallingIce(APenguinCharacter* Self, const ABlockIsland* Island)
{
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!Island || !Movement || !Movement->IsMovingOnGround() || !Island->IsSandFalling())
	{
		return false;
	}
	// Sand ice (an iceberg's) falls down its column: is the first block over our head ice?
	const FVector Loc = Self->GetActorLocation();
	const float Radius = Self->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float HeadZ = static_cast<float>(Loc.Z) + Self->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5.f;
	auto IceOverhead = [Island, HeadZ](float X)
	{
		const FIntPoint Head = Island->WorldToCell(FVector(X, 0.f, HeadZ));
		for (int32 Up = 0; Up < PenguinBotPrivate::FallingIceRows; ++Up)
		{
			const EBlockKind Block = Island->GetBlock(Head.X, Head.Y + Up);
			if (Block != EBlockKind::None)
			{
				return Block == EBlockKind::Ice; // anything else above shelters us
			}
		}
		return false;
	};
	const float Here = static_cast<float>(Loc.X);
	const float Onward = Movement->Velocity.X >= 0.f ? 1.f : -1.f;
	float Dir = 0.f;
	float BlockX = 0.f;
	if (Island->IsSandFallingOver(Here - Radius, Here + Radius, static_cast<float>(Loc.Z), &BlockX))
	{
		// A block shaking or on its way down over us: away from its middle (on, if it is right over us).
		Dir = FMath::Abs(BlockX - Here) < 10.f ? Onward : (BlockX < Here ? 1.f : -1.f);
	}
	else
	{
		const bool bLeft = IceOverhead(Here - Radius + 2.f);
		const bool bRight = IceOverhead(Here + Radius - 2.f);
		if (!bLeft && !bRight)
		{
			return false;
		}
		// Out from under it, toward the side without ice overhead.
		Dir = bLeft != bRight ? (bLeft ? 1.f : -1.f) : Onward;
	}
	// Not over an edge into the sea.
	float Gap = 0.f;
	if (IsEdgeAhead(Self, Island, Dir, Gap, StopLookahead(Self, Dir)) && Gap >= 0.f)
	{
		Dir = -Dir;
	}
	Move = FVector2D(Dir, 0.f);
	return true;
}

bool UPenguinBotComponent::TrySlideAttack(APenguinCharacter* Self, const ABlockIsland* Island)
{
	using namespace PenguinBotPrivate;
	const APenguinCharacter* T = Target.Get();
	const UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!T || !Island || !Movement || SlideTimer > 0.f || !Movement->CanBellySlideNow())
	{
		return false; // (Sticky never slides)
	}
	++SlideLooks[0];
	if (bJumpDown || !Movement->IsMovingOnGround() || Movement->IsBellySliding() || Movement->HasNoControl()
		|| Movement->GetBellySlideCooldownRemaining() > 0.f || T->IsShielded())
	{
		return false;
	}
	++SlideLooks[1];
	const float Dx = static_cast<float>(T->GetActorLocation().X - Self->GetActorLocation().X);
	const float Size = Self->GetSizeScale();
	const float Dz = static_cast<float>(T->GetFeetLocation().Z - Self->GetFeetLocation().Z);
	if (FMath::Abs(Dx) < SlideAttackMinRange * Size || FMath::Abs(Dx) > SlideAttackMaxRange * Size)
	{
		return false;
	}
	++SlideLooks[2];
	if (Dz > SlideAttackMaxDz || Dz < -SlideAttackMaxBelow)
	{
		return false;
	}
	++SlideLooks[3];
	SlideTimer = Rand.FRandRange(0.25f, 0.6f);
	// (Going in: every look may slide, as far as our skill goes.)
	if (Rand.FRand() > CurrentAggression() * (bAggressive ? 1.f : SlideAttackChance))
	{
		return false;
	}
	++SlideLooks[4];
	// Up to the target (and a little past it): flat ground or a step down with nothing in the way. Beyond that (a slide
	// that misses goes on) ground somewhere below or a wall that stops it - never a slide off the island into the sea.
	const float Dir = FMath::Sign(Dx);
	const FIntPoint FeetCell = Island->WorldToCell(Self->GetFeetLocation() + FVector(0.f, 0.f, 5.f));
	const int32 Columns = FMath::CeilToInt(SlideAttackClearAhead * Size / Island->CellSize);
	const int32 ToTarget = FMath::CeilToInt((FMath::Abs(Dx) + SlideAttackPastTarget) / Island->CellSize);
	// Rows the sliding belly fills: 1 at normal size; a grown penguin's slide would get stuck under a ceiling lower than that.
	const int32 BellyRows = RowsToFit(BellyHeight(Self, Movement), Island->CellSize);
	for (int32 K = 1; K <= Columns; ++K)
	{
		const int32 Column = FeetCell.X + (Dir > 0.f ? K : -K);
		bool bBlocked = false;
		for (int32 Up = 0; Up < BellyRows && !bBlocked; ++Up)
		{
			bBlocked = Island->IsSolidCell(FIntPoint(Column, FeetCell.Y + Up));
		}
		if (K <= ToTarget)
		{
			const bool bGround = Island->IsSolidCell(FIntPoint(Column, FeetCell.Y - 1)) || Island->IsSolidCell(FIntPoint(Column, FeetCell.Y - 2));
			if (!bGround || bBlocked)
			{
				return false;
			}
			continue;
		}
		if (bBlocked)
		{
			break; // a wall ends the slide here
		}
		bool bGroundBelow = false;
		for (int32 Down = 1; Down <= SlideAttackMaxDrop && !bGroundBelow; ++Down)
		{
			bGroundBelow = Island->IsSolidCell(FIntPoint(Column, FeetCell.Y - Down));
		}
		if (!bGroundBelow)
		{
			return false;
		}
	}
	// No portal end on the slide's way that we don't want (it would throw us somewhere else).
	{
		TArray<APinguPortal*> Portals;
		APinguPortal::GetAll(GetWorld(), Portals);
		const FVector SlideFrom = Self->GetActorLocation();
		for (const APinguPortal* Portal : Portals)
		{
			for (int32 End = 0; End < 2 && HasEnds(Portal); ++End)
			{
				const FVector In = Portal->GetEnd(End);
				const float Ahead = static_cast<float>(In.X - SlideFrom.X) * Dir;
				if (Ahead > 0.f && Ahead < SlideAttackClearAhead * Size + APinguPortal::EndHalfWidth
					&& FMath::Abs(static_cast<float>(In.Z - SlideFrom.Z)) < APinguPortal::EndHalfHeight + 40.f
					&& !WantsPortal(Self, Island, Portal, End, nullptr, false))
				{
					return false;
				}
			}
		}
	}
	++SlideLooks[5];
	Move = FVector2D(Dir, 0.f);
	Self->ApplyVirtualMoveInput(Move); // the slide goes along the stick
	Self->BellySlide();
	SlideTimer = Rand.FRandRange(2.5f, 5.f);
	return true;
}

FVector UPenguinBotComponent::Lead(const APenguinCharacter* T, const FVector& From, float ProjectileSpeed, float Gravity) const
{
	const FVector Aim = T->GetAimOrigin();
	float Time = FVector::Dist(From, Aim) / FMath::Max(ProjectileSpeed, 1.f);
	FVector Predicted = Aim;
	if (Skill.bLead)
	{
		const FVector V = T->GetVelocity();
		for (int32 Iteration = 0; Iteration < 2; ++Iteration)
		{
			Predicted = Aim + FVector(V.X, 0.f, V.Z * 0.4f) * Time;
			Time = FVector::Dist(From, Predicted) / FMath::Max(ProjectileSpeed, 1.f);
		}
	}
	Predicted.Z += 0.5f * Gravity * Time * Time;
	return Predicted;
}

FVector UPenguinBotComponent::ApplyAimError(const FVector& From, const FVector& To) const
{
	FVector Delta = To - From;
	const float Angle = FMath::DegreesToRadians(const_cast<FRandomStream&>(Rand).FRandRange(-Skill.AimError, Skill.AimError));
	const float C = FMath::Cos(Angle);
	const float S = FMath::Sin(Angle);
	return From + FVector(Delta.X * C - Delta.Z * S, 0.f, Delta.X * S + Delta.Z * C);
}

bool UPenguinBotComponent::FireAbility(APenguinCharacter* Self, EPenguinAbility Ability, const FVector& Aim, float ChargeHold, bool bLeadTarget)
{
	UPenguinAbilityComponent* Abilities = Self->GetAbilities();
	// (An empty slot - Second Chance's slot 1 - is no ability.)
	const int32 Slot = Abilities && Ability != EPenguinAbility::None ? Abilities->FindSlot(Ability) : INDEX_NONE;
	if (Slot == INDEX_NONE || !Abilities->IsReady(Slot))
	{
		return false;
	}
	// The build limit (PinguBuildLimit, 2026-11-05): a placement the host would refuse (past the top of the screen or the side
	// room; a snowman without a free tile) is never tried - no ledges above the screen, no refused tries.
	if (!Abilities->IsPlacementAllowed(Ability, Aim))
	{
		return false;
	}
	Self->SetAimPoint(Aim);
	if (UPenguinAbilityComponent::IsChargedAbility(Ability))
	{
		// The icicle: held for ChargeHold, then let go by itself (on a LAN client through our own machine, like a player).
		const float Hold = ChargeHold >= 0.f ? ChargeHold : UPenguinAbilityComponent::IcicleFullChargeTime;
		IcicleChargePower = UPenguinAbilityComponent::GetIciclePower(Hold);
		bIcicleLeadsTarget = bLeadTarget;
		IcicleAimErrorDeg = Rand.FRandRange(-Skill.AimError, Skill.AimError);
		// A fixed spot is aimed at for this charge's drop and lift (a short hold's arc is steep).
		IcicleFixedAim = bLeadTarget ? Aim : IcicleAimAt(Self, Aim, IcicleChargePower);
		if (bLeadTarget)
		{
			if (const APenguinCharacter* T = Target.Get())
			{
				Self->SetAimPoint(IcicleAim(Self, T, IcicleChargePower));
			}
		}
		return Abilities->ChargeAndThrow(Slot, Hold);
	}
	return Self->ActivateAbility(Slot);
}

namespace PenguinBotPrivate
{
	/** Where to aim so that an icicle, which points TiltDegrees above the aim (its lift and this throw's aim error), flies at
	 *  Led (the spot with the drop already allowed for). */
	static FVector IcicleAimToward(const APenguinCharacter* Self, const FVector& Led, float TiltDegrees)
	{
		const FVector From = Self->GetAimOrigin();
		const FVector Delta(Led.X - From.X, 0.f, Led.Z - From.Z);
		const float Length = static_cast<float>(Delta.Size());
		if (Length < 1.f)
		{
			return Led;
		}
		const float Side = FMath::Abs(Delta.X) > 1.e-3 ? FMath::Sign(static_cast<float>(Delta.X)) : Self->GetFacing();
		const float Angle = FMath::Atan2(static_cast<float>(Delta.Z), FMath::Abs(static_cast<float>(Delta.X))) - FMath::DegreesToRadians(TiltDegrees);
		return From + FVector(Side * FMath::Cos(Angle), 0.f, FMath::Sin(Angle)) * Length;
	}
}

FVector UPenguinBotComponent::IcicleAim(const APenguinCharacter* Self, const APenguinCharacter* T, float Power) const
{
	float Speed = PinguTuning::IcicleSpeed;
	float Gravity = 0.f;
	float LiftDegrees = 0.f;
	UPenguinAbilityComponent::GetIcicleThrow(Power, Speed, Gravity, LiftDegrees);
	// Led for its speed and drop; the weak throws point LiftDegrees above where they are aimed, so aim that much lower (and off
	// by this throw's aim error).
	return PenguinBotPrivate::IcicleAimToward(Self, Lead(T, Self->GetAimOrigin(), Speed, Gravity), LiftDegrees + IcicleAimErrorDeg);
}

FVector UPenguinBotComponent::IcicleAimAt(const APenguinCharacter* Self, const FVector& Where, float Power) const
{
	float Speed = PinguTuning::IcicleSpeed;
	float Gravity = 0.f;
	float LiftDegrees = 0.f;
	UPenguinAbilityComponent::GetIcicleThrow(Power, Speed, Gravity, LiftDegrees);
	// A spot that stays: only the drop of the flight (the time the straight line takes) to allow for.
	const float Time = static_cast<float>(FVector::Dist(Self->GetAimOrigin(), Where)) / FMath::Max(Speed, 1.f);
	FVector Raised = Where;
	Raised.Z += 0.5f * Gravity * Time * Time;
	return PenguinBotPrivate::IcicleAimToward(Self, Raised, LiftDegrees + IcicleAimErrorDeg);
}

float UPenguinBotComponent::IcicleCharge(float Distance)
{
	using namespace PenguinBotPrivate;
	if (Distance < IcicleBotPointBlank)
	{
		return Rand.FRandRange(IcicleBotQuickMin, IcicleBotQuickMax);
	}
	const float Share = FMath::Clamp((Distance - IcicleBotPointBlank) / (IcicleBotFullRange - IcicleBotPointBlank), 0.f, 1.f);
	return FMath::Lerp(IcicleBotMidCharge, 1.f, Share);
}

bool UPenguinBotComponent::UseAbilities(APenguinCharacter* Self, ABlockIsland* Island)
{
	const APenguinCharacter* T = Target.Get();
	const UPenguinAbilityComponent* Abilities = Self->GetAbilities();
	bLastUseNoAttack = false;
	if (!T || !Abilities || Rand.FRand() > CurrentAggression())
	{
		return false;
	}
	const UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	const UPenguinMovementComponent* TargetMovement = T->GetPenguinMovement();
	const FVector From = Self->GetAimOrigin();
	const FVector To = T->GetAimOrigin();
	const float Dist = FVector::Dist(From, To);
	const float Dx = To.X - From.X;
	const float Dz = To.Z - From.Z;

	// Lasting ice walls block shots: one between us and the target is shot away first (now and then).
	const bool bSight = HasLineOfSight(From, To);
	FVector IceSpot = FVector::ZeroVector;
	const bool bShootIce = !bSight && Rand.FRand() < PenguinBotPrivate::IceShootChance && IceBlocksSight(Island, From, To, IceSpot);
	// Walls shield penguins from the slap, gust, noot and frost breath: those only at a target they can reach.
	const bool bReach = UPenguinAbilityComponent::HasClearLine(Island, Self, T);

	// A frozen target's ice takes the first hit that smashes ice and frees it, and a snowball only bursts on it: no slap, stomp,
	// snowball, icicle or bomb at it then (wind blows the ice cube away - that is fine).
	const bool bFrozenTarget = TargetMovement && TargetMovement->IsFrozen();
	// The slap, frost breath, gust, stomp and noot reach further on a bigger penguin (x its size, PenguinAbilities.cpp) - and
	// a giant's chest is metres above a normal penguin's: their ranges below grow with us (1 at normal size).
	const float Size = Self->GetSizeScale();
	// The size rays go off on ourselves when aimed this close to our chest (DoSizeRay's self-cast).
	const float SelfCastDistance = (UPenguinAbilityComponent::SelfCastRadius + 20.f) * Size;

	TArray<int32> Order;
	for (int32 Slot = 0; Slot < Abilities->GetAbilities().Num(); ++Slot)
	{
		Order.Insert(Slot, Rand.RandRange(0, Order.Num()));
	}
	for (const int32 Slot : Order)
	{
		if (!Abilities->IsReady(Slot))
		{
			continue;
		}
		const EPenguinAbility Ability = Abilities->GetAbility(Slot);
		if (Ability == EPenguinAbility::None || (bFrozenTarget && !bShootIce && (UPenguinAbilityComponent::IsIceSmashing(Ability) || UPenguinAbilityComponent::BouncesOffIce(Ability))))
		{
			continue; // (an empty slot: Second Chance)
		}
		switch (Ability)
		{
		case EPenguinAbility::Snowball:
			if (Dist < 1000.f && bSight)
			{
				return FireAbility(Self, Ability, ApplyAimError(From, Lead(T, From, 2100.f, 700.f)));
			}
			if (bShootIce)
			{
				return FireAbility(Self, Ability, IceSpot);
			}
			break;
		case EPenguinAbility::Icicle:
			if (Dist < PenguinBotPrivate::IcicleBotRange && bSight)
			{
				// Held as long as the distance needs (a short hold at point blank, a full second from a few metres), aimed at
				// the target as it moves until it is let go.
				const float Hold = IcicleCharge(Dist) * UPenguinAbilityComponent::IcicleFullChargeTime + Rand.FRandRange(0.f, 0.05f);
				return FireAbility(Self, Ability, T->GetAimOrigin(), Hold, true);
			}
			if (bShootIce)
			{
				return FireAbility(Self, Ability, IceSpot);
			}
			break;
		case EPenguinAbility::FishHook:
			if (Dist > PenguinBotPrivate::HookMinRange && Dist < APenguinProjectile::HookReach * PenguinBotPrivate::HookBotReach && bSight)
			{
				// It throws the catch over our head: with the sea behind us that is a ring-out, always worth a try (not a frozen
				// target: its heavy ice cube only comes halfway).
				float GroundZ = 0.f;
				const FVector Behind = Self->GetActorLocation() - FVector(FMath::Sign(Dx) * PenguinBotPrivate::HookSeaCheckBehind, 0.f, 0.f);
				const bool bSeaBehind = !bFrozenTarget && Island && !Island->FindGroundBelow(Behind, 2500.f, GroundZ);
				if (bSeaBehind || Rand.FRand() < PenguinBotPrivate::HookUseChance)
				{
					return FireAbility(Self, Ability, ApplyAimError(From, Lead(T, From, APenguinProjectile::HookSpeed, 0.f)));
				}
			}
			break;
		case EPenguinAbility::SnowBomb:
			if (Dist > 220.f && Dist < 850.f)
			{
				FVector Aim = T->GetFeetLocation();
				if (Skill.bLead)
				{
					Aim += FVector(T->GetVelocity().X * 0.7f, 0.f, 0.f);
				}
				if (FMath::Abs(Aim.X - From.X) < 45.f)
				{
					break; // straight below us: the bomb would just drop at our own feet
				}
				if (!BombThrowIsSafe(Self, Island, Aim))
				{
					break; // our own blast knocks us out too: only a throw that ends well away from us
				}
				return FireAbility(Self, Ability, Aim);
			}
			break;
		case EPenguinAbility::IcebergDrop:
			if (FMath::Abs(Dx) < 950.f && FMath::Abs(Dx) > 150.f && TargetMovement && TargetMovement->IsMovingOnGround())
			{
				const FVector Aim = T->GetActorLocation() + FVector(Skill.bLead ? T->GetVelocity().X * 0.55f : 0.f, 0.f, 0.f);
				return FireAbility(Self, Ability, Aim);
			}
			break;
		case EPenguinAbility::FlipperSlap:
			if (Dist < 150.f * Size && bReach)
			{
				return FireAbility(Self, Ability, To + FVector(0.f, 0.f, 25.f));
			}
			break;
		case EPenguinAbility::FrostBreath:
			if (Dist < 360.f * Size && bSight && bReach)
			{
				return FireAbility(Self, Ability, To);
			}
			break;
		case EPenguinAbility::Gust:
			// The tornado: only with somebody in its reach that it would throw toward the sea (or two of them) - never alone.
			if (Dist < PenguinBotPrivate::TornadoBotRange * Size && bReach && PenguinBotPrivate::TornadoWorthIt(Self, Island))
			{
				return FireAbility(Self, Ability, To);
			}
			break;
		case EPenguinAbility::BellyStomp:
			if ((Movement->IsFalling() && FMath::Abs(Dx) < 100.f * Size && Dz < -60.f * Size)
				|| (Movement->IsMovingOnGround() && Dist < 130.f * Size && FMath::Abs(Dz) < 60.f * Size))
			{
				return FireAbility(Self, Ability, To);
			}
			break;
		case EPenguinAbility::SnowWarp:
			if (Dist > 900.f && TargetMovement && TargetMovement->IsMovingOnGround() && Rand.FRand() < 0.3f)
			{
				// The warp stops at its range on the way to a far aim: only if that spot is over ground (not over the sea
				// between two parts of the island).
				const FVector WarpAim = T->GetActorLocation() + FVector(-FMath::Sign(Dx) * 220.f, 0.f, 60.f);
				if (WarpLandsSafely(Self, Island, WarpAim))
				{
					return FireAbility(Self, Ability, WarpAim);
				}
			}
			break;
		case EPenguinAbility::Noot:
			// A shotgun: point blank it rings them out, at range it is next to nothing.
			if (Dist < PenguinBotPrivate::NootBotRange * Size && FMath::Abs(Dz) < PenguinBotPrivate::NootBotMaxDz * FMath::Max(1.f, Size) && bReach)
			{
				return FireAbility(Self, Ability, To);
			}
			break;
		case EPenguinAbility::IceGun:
			if (Dist < 950.f && bSight && TargetMovement && !TargetMovement->IsSlippery())
			{
				return FireAbility(Self, Ability, ApplyAimError(From, Lead(T, From, 2600.f, 0.f)));
			}
			break;
		case EPenguinAbility::ShrinkRay:
			// Always at the target, never on ourselves: small penguins are light (easier to knock into the sea), one at the
			// smallest size vanishes (a knock-out), and a giant shrinks back toward harmless.
			if (Dist < 850.f && bSight)
			{
				const FVector RayAim = ApplyAimError(From, Lead(T, From, 2300.f, 0.f));
				// Too close and the ray goes off on ourselves.
				if (FVector::Dist(From, RayAim) > SelfCastDistance)
				{
					return FireAbility(Self, Ability, RayAim);
				}
			}
			// The shrink ray shrinks ice away too.
			if (bShootIce && FVector::Dist(From, IceSpot) > SelfCastDistance)
			{
				return FireAbility(Self, Ability, IceSpot);
			}
			break;
		case EPenguinAbility::GrowRay:
			// A target at the biggest size already pops from one more hit: a knock-out. Otherwise never at somebody else
			// (bigger is heavier, faster and reaches further) - not even one close to 8x: it would only make it stronger.
			if (PenguinBotPrivate::GrowRayPops(T) && Dist < 850.f && bSight)
			{
				const FVector RayAim = ApplyAimError(From, Lead(T, From, 2300.f, 0.f));
				if (FVector::Dist(From, RayAim) > SelfCastDistance)
				{
					return FireAbility(Self, Ability, RayAim);
				}
			}
			// Grow yourself: heavier, faster, longer reach - only up to about 1.7x, and never a step past the size limit
			// (past 8x we would pop).
			if (FMath::Max(Self->GetSizeScale(), Self->GetSizeTarget()) < 1.6f && PenguinBotPrivate::SelfRayIsSafe(Self, true) && Dist < 800.f)
			{
				bLastUseNoAttack = true; // (on ourselves: no attack)
				return FireAbility(Self, Ability, Self->GetAimOrigin());
			}
			break;
		case EPenguinAbility::Snowman:
			// Built when an enemy is a few metres away on about our level and fewer than BotMaxSnowmen of ours stand (any number may
			// stand since 2026-11-05; a bot keeps it to a couple): the snowman faces it and throws at it while we fight on.
			if (Movement->IsMovingOnGround() && Dist > 250.f && Dist < PinguTuning::SnowmanRange && FMath::Abs(Dz) < 200.f
				&& PenguinBotPrivate::CountSnowmenOf(GetWorld(), Self) < PenguinBotPrivate::BotMaxSnowmen && Rand.FRand() < 0.5f)
			{
				return FireAbility(Self, Ability, To);
			}
			break;
		case EPenguinAbility::Flap:
			// Up to a target far above (on a high ledge), from the ground, now and then; otherwise kept for falls (Recover).
			if (Movement->IsMovingOnGround() && Dz > PenguinBotPrivate::FlapChaseUp && FMath::Abs(Dx) < 500.f && Rand.FRand() < 0.3f)
			{
				bLastUseNoAttack = true; // (a way up: no attack)
				return FireAbility(Self, Ability, To);
			}
			break;
		case EPenguinAbility::GiantSnowball:
			// Rolled at a target on our level a few metres in front, now and then (the roll-up holds us still for 1.5 s).
			if (GiantSnowballWorthIt(Self, T, Island) && Rand.FRand() < PenguinBotPrivate::GiantUseChance)
			{
				return FireAbility(Self, Ability, To);
			}
			break;
		default:
			break; // Ice Wall and Ice Shell are used defensively (Dodge / Recover)
		}
	}
	return false;
}

bool UPenguinBotComponent::GiantSnowballWorthIt(const APenguinCharacter* Self, const APenguinCharacter* T, const ABlockIsland* Island) const
{
	const UPenguinMovementComponent* Movement = Self ? Self->GetPenguinMovement() : nullptr;
	const UPenguinMovementComponent* TargetMovement = T ? T->GetPenguinMovement() : nullptr;
	if (!Movement || !TargetMovement || !Island || !Movement->IsMovingOnGround())
	{
		return false;
	}
	const FVector From = Self->GetActorLocation();
	const FVector To = T->GetActorLocation();
	const float Dx = static_cast<float>(To.X - From.X);
	if (FMath::Abs(Dx) < PenguinBotPrivate::GiantMinRange || FMath::Abs(Dx) > PenguinBotPrivate::GiantMaxRange
		|| FMath::Abs(To.Z - From.Z) > PenguinBotPrivate::GiantMaxDz)
	{
		return false;
	}
	// No wall on the way at the ball's height (it would only bounce back at us).
	const float Dir = FMath::Sign(Dx);
	const float Feet = static_cast<float>(Self->GetFeetLocation().Z);
	for (float X = 120.f; X < FMath::Abs(Dx); X += 50.f)
	{
		if (Island->IsSolidAt(FVector(From.X + Dir * X, 0.f, Feet + 50.f)))
		{
			return false;
		}
	}
	return true;
}

bool UPenguinBotComponent::AvoidGiantSnowball(APenguinCharacter* Self, const ABlockIsland* Island)
{
	UPenguinMovementComponent* Movement = Self ? Self->GetPenguinMovement() : nullptr;
	if (!Movement)
	{
		return false;
	}
	TArray<APinguGiantSnowball*> Giants;
	APinguGiantSnowball::GetAll(GetWorld(), Giants);
	const FVector Loc = Self->GetActorLocation();
	const float Radius = Self->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float Half = Self->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	for (const APinguGiantSnowball* Giant : Giants)
	{
		if (!Giant->IsRolling())
		{
			continue;
		}
		const FVector C = Giant->GetCenter();
		const float R = Giant->GetRadius();
		const float Rel = static_cast<float>(Loc.X - C.X);
		const float Gap = FMath::Abs(Rel) - R - Radius;
		// Only one on our level, and not one we are already over.
		// (In the air over it - jumping it - it is still ours to steer over.)
		const bool bOverIt = !Movement->IsMovingOnGround() && FMath::Abs(Rel) < R + Radius + 150.f && Loc.Z - Half < C.Z + R + 400.f;
		if (!bOverIt && (Loc.Z - Half > C.Z + R + 20.f || Loc.Z + Half < C.Z - R || Gap > PenguinBotPrivate::GiantJumpGap + 400.f))
		{
			continue;
		}
		const bool bComing = FMath::Sign(Rel) == Giant->GetDirection();
		if (!bComing)
		{
			// It rolls away from us (or we are over it, past its middle): keep away from it - on across in the air, back on the ground.
			if ((Gap < 80.f && Movement->IsMovingOnGround()) || (!Movement->IsMovingOnGround() && Gap < 120.f))
			{
				Move = FVector2D(FMath::Sign(Rel), 0.f);
				return true;
			}
			continue;
		}
		if (Movement->IsMovingOnGround())
		{
			// Jump when it is about a quarter of a second away (closing at its speed and ours), or GiantJumpGap.
			const float Closing = FMath::Max(0.f, static_cast<float>(-FMath::Sign(Rel) * Movement->Velocity.X)) + (Giant->IsFrozen() ? PinguTuning::GiantFrozenSpeed : PinguTuning::GiantRollSpeed);
			if (Gap > FMath::Max(PenguinBotPrivate::GiantJumpGap, Closing * 0.22f))
			{
				continue; // not yet
			}
			if (Rand.FRand() > Skill.Dodge + 0.3f)
			{
				return false; // (a slow bot gets caught now and then)
			}
			// Up and over toward its far side.
			Move = FVector2D(-FMath::Sign(Rel), 0.f);
			Jump(0.35f);
			UE_LOG(LogPingu, Log, TEXT("[BOT] %s jumps a giant snowball (%.0f cm away, %.0f cm across)"), *Self->GetDisplayName(), Gap, Giant->GetDiameter());
			return true;
		}
		// In the air over it: run on across, and jump again on the way down while its top is still above our feet.
		Move = FVector2D(-FMath::Sign(Rel), 0.f);
		if (Movement->Velocity.Z < 400.f && Movement->GetAirJumpsLeft() > 0 && Loc.Z - Half < C.Z + R + 60.f && FMath::Abs(Rel) < R + 220.f)
		{
			if (bJumpDown)
			{
				Self->ReleaseJump(); // (the first jump's key: let go, then the double jump)
				bJumpDown = false;
			}
			Jump(0.3f);
		}
		return true;
	}
	return false;
}

bool UPenguinBotComponent::Dodge(APenguinCharacter* Self, ABlockIsland* Island)
{
	if (DodgeCooldown > 0.f)
	{
		return false;
	}
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	const UPenguinAbilityComponent* Abilities = Self->GetAbilities();
	const FVector Loc = Self->GetActorLocation();
	// Glass Cannon is knocked 1.5x as far: it dodges more. A ready Mirror sends the next shot back anyway (not a hook): it
	// dodges shots less.
	const float DodgeChance = Skill.Dodge + (PenguinBotPrivate::TraitOf(Self) == EPinguTrait::GlassCannon ? PenguinBotPrivate::GlassCannonDodgeBonus : 0.f);
	const bool bMirrorReady = IsMirrorReady(Self);

	TArray<APenguinProjectile*> Projectiles;
	APenguinProjectile::GetAll(GetWorld(), Projectiles);
	for (const APenguinProjectile* Projectile : Projectiles)
	{
		// Friendly fire is on: a teammate's shot hurts like anybody's, so it is dodged too. (Our own shots are left out.)
		if (!Projectile->IsThreat() || Projectile->GetShooter() == Self)
		{
			continue;
		}
		const FVector Rel = Loc - Projectile->GetActorLocation();
		const FVector V = Projectile->GetVelocity();
		const float Speed2 = V.SizeSquared();
		if (Speed2 < 1.f)
		{
			continue;
		}
		const float Time = FVector::DotProduct(Rel, V) / Speed2;
		if (Time < 0.f || Time > 0.45f)
		{
			continue;
		}
		const FVector Miss = Projectile->GetActorLocation() + V * Time - Loc;
		// (A grown penguin is a bigger target: a shot at its head or feet hits it too.)
		if (Miss.Size() > 85.f + PenguinBotPrivate::GrownHalfHeight(Self))
		{
			continue;
		}
		DodgeCooldown = 0.5f;
		const bool bReflected = bMirrorReady && Projectile->GetKind() != EPenguinProjectileKind::FishHook;
		if (Rand.FRand() > DodgeChance - (bReflected ? PenguinBotPrivate::MirrorReadyDodgeCut : 0.f))
		{
			return false;
		}
		if (Abilities && Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::IceShell)))
		{
			return FireAbility(Self, EPenguinAbility::IceShell, Loc);
		}
		if (Abilities && Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::IceWall)) && Rand.FRand() < PenguinBotPrivate::IceWallDodgeChance
			&& FireAbility(Self, EPenguinAbility::IceWall, Loc - Rel.GetSafeNormal() * 150.f))
		{
			return true;
		}
		if (Movement->IsMovingOnGround() && FMath::Abs(V.Z) < FMath::Abs(V.X))
		{
			Jump(0.25f);
			return true;
		}
		if (Movement->GetAirDashesLeft() > 0 || Movement->IsMovingOnGround())
		{
			const float Side = FMath::Abs(V.X) > FMath::Abs(V.Z) ? 0.f : -FMath::Sign(V.X + 0.01f);
			float EdgeGap = 0.f;
			if (Side != 0.f && Movement->IsMovingOnGround() && IsEdgeAhead(Self, Island, Side, EdgeGap, 250.f)
				&& (EdgeGap == 0.f || EdgeGap > static_cast<float>(MaxGapCells(Self))))
			{
				Jump(0.25f); // a dash that way would end in the sea
				return true;
			}
			Move = FVector2D(Side, FMath::Abs(V.X) > FMath::Abs(V.Z) ? 1.f : 0.f);
			Self->ApplyVirtualMoveInput(Move);
			Self->PressDash();
			return true;
		}
		return false;
	}

	// Icebergs about to land on us (anybody's but ours, a teammate's too): run. (Sides: our capsule's radius + 15 cm, 45 at
	// normal size; height: from a normal penguin's middle on our spot.)
	const float IcebergSide = 15.f + FMath::Max(30.f, Self->GetCapsuleComponent()->GetScaledCapsuleRadius());
	const float IcebergBelow = static_cast<float>(Loc.Z) - 100.f - PenguinBotPrivate::GrownHalfHeight(Self);
	for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
	{
		const AIcebergDrop* Iceberg = *It;
		if (Iceberg->GetDropOwner() == Self)
		{
			continue; // our own iceberg never crushes us
		}
		if (Loc.X > Iceberg->GetLeftX() - IcebergSide && Loc.X < Iceberg->GetRightX() + IcebergSide && Iceberg->GetActorLocation().Z > IcebergBelow)
		{
			DodgeCooldown = 0.2f;
			if (Rand.FRand() > DodgeChance + 0.25f)
			{
				return false;
			}
			if (Abilities && Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::IceShell)) && Iceberg->IsFalling())
			{
				return FireAbility(Self, EPenguinAbility::IceShell, Loc);
			}
			const float Center = (Iceberg->GetLeftX() + Iceberg->GetRightX()) * 0.5f;
			float Gap = 0.f;
			float Dir = Loc.X < Center ? -1.f : 1.f;
			if (Movement->IsMovingOnGround() && IsEdgeAhead(Self, Island, Dir, Gap, StopLookahead(Self, Dir)))
			{
				if (Gap > 0.f && Gap <= static_cast<float>(MaxGapCells(Self)))
				{
					Jump(0.35f); // over the gap and away
				}
				else if (Gap >= 0.f)
				{
					Dir = -Dir; // the sea is that way: run the other way
				}
			}
			Move = FVector2D(Dir, 0.f);
			return true;
		}
	}
	return false;
}

// ============================================================================ the snow bomb's blast

FVector UPenguinBotComponent::PredictBombEnd(const ABlockIsland* Island, const APenguinCharacter* Ignore, const FVector& From, const FVector& Velocity,
	float Gravity, float Radius, float Fuse) const
{
	const UWorld* World = GetWorld();
	if (!Island || !World || Velocity.SizeSquared() < 1.f)
	{
		return From; // at rest (or frozen in the air): it goes off where it is
	}
	// Every penguin it could touch on the way (it goes off in the first one: a direct hit).
	TArray<const APenguinCharacter*, TInlineAllocator<12>> Penguins;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		if (*It != Ignore && It->IsAlive() && !It->IsHidden())
		{
			Penguins.Add(*It);
		}
	}
	constexpr float Step = 1.f / 60.f;
	const int32 Steps = FMath::Clamp(FMath::CeilToInt(Fuse / Step), 1, 180);
	FVector Pos = From;
	FVector Vel = Velocity;
	for (int32 Index = 0; Index < Steps; ++Index)
	{
		Vel.Z -= Gravity * Step;
		const FVector Next = Pos + Vel * Step;
		// Its body (a ball) against the blocks: the middle and the four points on its rim.
		if (Island->IsSolidAt(Next) || Island->IsSolidAt(Next + FVector(Radius, 0.f, 0.f)) || Island->IsSolidAt(Next - FVector(Radius, 0.f, 0.f))
			|| Island->IsSolidAt(Next + FVector(0.f, 0.f, Radius)) || Island->IsSolidAt(Next - FVector(0.f, 0.f, Radius)))
		{
			return Pos;
		}
		for (const APenguinCharacter* Penguin : Penguins)
		{
			if (Penguin->DistanceTo(Next) < Radius)
			{
				return Next;
			}
		}
		Pos = Next;
		if (Pos.Z < Island->SeaLevel)
		{
			break; // into the sea
		}
	}
	return Pos;
}

bool UPenguinBotComponent::BombThrowIsSafe(const APenguinCharacter* Self, const ABlockIsland* Island, const FVector& Aim) const
{
	using namespace PenguinBotPrivate;
	if (!Island)
	{
		return false;
	}
	FVector Spawn;
	FVector Velocity;
	bool bDropAtFeet = false;
	UPenguinAbilityComponent::GetBombThrow(Self->GetAimOrigin(), Aim, Self->GetFacing(), Spawn, Velocity, bDropAtFeet);
	if (bDropAtFeet)
	{
		return false; // on our own feet
	}
	const float Size = Self->GetSizeScale(); // (the bomb is as big as its thrower)
	const FVector End = PredictBombEnd(Island, Self, Spawn, Velocity, PinguTuning::BombGravity, BombBodyRadius * Size, PinguTuning::BombFuse);
	const float Blast = PinguTuning::BombPushRadius * Size * ExpectedBlastScale(Self);
	return Self->DistanceTo(End) > Blast + BombThrowMargin;
}

float UPenguinBotComponent::FreeRunDistance(const APenguinCharacter* Self, const ABlockIsland* Island, float Direction, float MaxDistance) const
{
	using namespace PenguinBotPrivate;
	const float Cell = FMath::Max(Island->CellSize, 1.f);
	const int32 Dir = Direction > 0.f ? 1 : -1;
	const FIntPoint FeetCell = Island->WorldToCell(Self->GetFeetLocation() + FVector(0.f, 0.f, 5.f));
	const int32 Jumpable = JumpableRows(Self);
	const int32 MaxGap = MaxGapCells(Self);
	const int32 Cells = FMath::CeilToInt(MaxDistance / Cell);
	int32 Row = FeetCell.Y; // the row we stand in, column by column (a step up or down changes it)
	int32 GapRun = 0;
	for (int32 K = 1; K <= Cells; ++K)
	{
		const int32 Column = FeetCell.X + Dir * K;
		// A wall: climbed if it is low enough.
		int32 Up = 0;
		while (Up <= Jumpable && IsSolid(Island, Column, Row + Up))
		{
			++Up;
		}
		if (Up > Jumpable)
		{
			return static_cast<float>(K - 1) * Cell;
		}
		Row += Up;
		// The floor: right under us, or a few blocks down; with none it is a gap column.
		int32 Down = 0;
		while (Down < 8 && !IsSolid(Island, Column, Row - 1 - Down))
		{
			++Down;
		}
		if (Down >= 8)
		{
			++GapRun;
			if (GapRun > MaxGap)
			{
				return static_cast<float>(K - GapRun) * Cell; // a gap too wide: the way ends at its near side
			}
			continue;
		}
		GapRun = 0;
		Row -= Down;
	}
	// Still in a gap where we stopped looking: the island ends there as far as we know.
	return GapRun > 0 ? static_cast<float>(FMath::Max(Cells - GapRun, 0)) * Cell : MaxDistance;
}

bool UPenguinBotComponent::AvoidBlast(APenguinCharacter* Self, const ABlockIsland* Island, float& Desired)
{
	using namespace PenguinBotPrivate;
	bFleeingBlast = false;
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	const UWorld* World = GetWorld();
	if (!Island || !Movement || !World)
	{
		return false;
	}
	const float Now = World->GetTimeSeconds();
	TArray<APenguinProjectile*> Projectiles;
	APenguinProjectile::GetAll(World, Projectiles);
	for (auto It = BombsSeen.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	// The bomb that goes off first with us in its blast (the blast as its zone shows it, a margin more).
	const UPenguinAbilityComponent* OwnAbilities = Self->GetAbilities();
	const float ShieldLeft = Self->IsShielded() && OwnAbilities ? OwnAbilities->GetShieldRemaining() : 0.f;
	const APenguinProjectile* Urgent = nullptr;
	FVector UrgentEnd = FVector::ZeroVector;
	float UrgentLeft = TNumericLimits<float>::Max();
	float UrgentRadius = 0.f;
	for (const APenguinProjectile* Bomb : Projectiles)
	{
		if (Bomb->GetKind() != EPenguinProjectileKind::Bomb || !Bomb->IsFlying())
		{
			continue;
		}
		const float Left = FMath::Max(Bomb->GetLifeLeft(), 0.f);
		if (Left > BombFleeMaxFuse || ShieldLeft > Left + 0.2f)
		{
			continue; // (not yet - or our shell is still up when it goes off: it holds the blast)
		}
		// Our own we know about at once; another's we see for a moment first (the easier the bot, the longer).
		const float Seen = BombsSeen.FindOrAdd(Bomb, Now);
		if (Bomb->GetShooter() != Self && Now - Seen < Skill.Reaction)
		{
			continue;
		}
		const float Radius = Bomb->GetBlastPushRadius() + BombFleeMargin + (FleeBomb.Get() == Bomb ? BombFleeHold : 0.f);
		const FVector End = PredictBombEnd(Island, Bomb->GetShooter(), Bomb->GetActorLocation(), Bomb->GetVelocity(), Bomb->GetGravity(), Bomb->GetRadius(), Left);
		if (Self->DistanceTo(End) <= Radius && Left < UrgentLeft)
		{
			Urgent = Bomb;
			UrgentEnd = End;
			UrgentLeft = Left;
			UrgentRadius = Radius;
		}
	}
	if (!Urgent)
	{
		FleeBomb.Reset();
		return false;
	}

	// How far out we have to get (sideways: the blast is a circle, and our capsule is long), and which way.
	const FVector Loc = Self->GetActorLocation();
	const UCapsuleComponent* Capsule = Self->GetCapsuleComponent();
	const float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
	const float Half = FMath::Max(Capsule->GetScaledCapsuleHalfHeight() - CapsuleRadius, 0.f);
	const float Dz = FMath::Max(0.f, FMath::Abs(static_cast<float>(UrgentEnd.Z - Loc.Z)) - Half);
	const float Reach = FMath::Sqrt(FMath::Max(UrgentRadius * UrgentRadius - Dz * Dz, 0.f)) + CapsuleRadius + 10.f;
	const float Dx = static_cast<float>(Loc.X - UrgentEnd.X);
	float Away = FMath::Abs(Dx) > 15.f ? FMath::Sign(Dx) : 0.f;
	if (Away == 0.f)
	{
		// Right under or over it: the way we were running from it, else the way with more room.
		Away = FleeBomb.Get() == Urgent && FleeDir != 0.f ? FleeDir
			: (FreeRunDistance(Self, Island, 1.f, Reach) >= FreeRunDistance(Self, Island, -1.f, Reach) ? 1.f : -1.f);
	}
	const float NeedAway = FMath::Max(Reach - FMath::Abs(Dx), 0.f); // out the way we are on
	const float NeedOther = FMath::Abs(Dx) + Reach;                  // through the middle and out the far side
	const float Speed = FMath::Max(Movement->GetEffectiveRunSpeed(), 100.f);
	const float FreeAway = FreeRunDistance(Self, Island, Away, NeedAway + BombFleeDashRoom);
	float Dir = Away;
	float Need = NeedAway;
	float Free = FreeAway;
	if (FreeAway < NeedAway)
	{
		// The island ends (or a wall stands) that way, so we cannot get out of the blast on that side. The shell holds it (the
		// best there is); else through the middle and on past the bomb: the push, if it still reaches us, then sends us the
		// way of the island instead of off its end, and once past the bomb's middle the kill zone is behind us in a moment.
		const float FreeOther = FreeRunDistance(Self, Island, -Away, NeedOther + BombFleeDashRoom);
		const UPenguinAbilityComponent* Abilities = Self->GetAbilities();
		if (Abilities && Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::IceShell)) && FireAbility(Self, EPenguinAbility::IceShell, Loc))
		{
			FleeBomb = Urgent;
			FleeDir = 0.f;
			bFleeingBlast = true;
			Desired = 0.f; // the shell holds the blast: stay put in it
			return true;
		}
		const float PastTheMiddle = FMath::Abs(Dx) + Urgent->GetBlastKillRadius() + CapsuleRadius + 30.f;
		if (FreeOther >= FMath::Min(NeedOther, PastTheMiddle))
		{
			Dir = -Away;
			Need = NeedOther;
			Free = FreeOther;
		}
	}
	FleeBomb = Urgent;
	FleeDir = Dir;
	bFleeingBlast = true;
	Desired = Dir;
	// Too slow on foot: a dash (only along a way that is free well past the run).
	if (Need / Speed > UrgentLeft * BombFleeDashShare && Free >= Need + BombFleeDashRoom && Movement->GetDashCooldownRemaining() <= 0.f
		&& (Movement->IsMovingOnGround() || Movement->GetAirDashesLeft() > 0))
	{
		Self->ApplyVirtualMoveInput(FVector2D(Dir, 0.f));
		Self->PressDash();
	}
	return true;
}

// ============================================================================ gaps one block high

bool UPenguinBotComponent::FindLowGapAhead(const APenguinCharacter* Self, const ABlockIsland* Island, float Direction, FLowGap& OutGap) const
{
	using namespace PenguinBotPrivate;
	OutGap = FLowGap();
	const UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!Island || !Movement || FMath::Abs(Direction) < 0.1f)
	{
		return false;
	}
	const float Cell = Island->CellSize;
	// Rows we need to stand in: 2 for a normal penguin (so a ceiling right above our feet row is a gap one block high). A
	// shrunk penguin is shorter than a block (1: it walks through standing up); a grown one needs more (an 8x giant, about
	// 9 m tall, 10), and any ceiling lower than that stops it.
	const int32 StandRows = RowsToFit(2.f * Movement->GetStandingHalfHeight(), Cell);
	if (StandRows <= 1)
	{
		return false;
	}
	const int32 Dir = Direction > 0.f ? 1 : -1;
	const FVector Feet = Self->GetFeetLocation();
	const float FrontX = static_cast<float>(Feet.X) + Dir * Self->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FIntPoint Front = Island->WorldToCell(FVector(FrontX, 0.f, Feet.Z + 5.f));
	const int32 Row = Front.Y; // the row our feet stand in: the gap's row

	// The first column ahead with floor under it and a ceiling over this row lower than we are tall (a wall or an edge
	// before it is the business of WallHeightAhead / IsEdgeAhead). (Columns can be negative - left of the island's middle -
	// so a found flag, not INDEX_NONE (-1), says whether there is one.) CeilingRows: rows from our feet row up to the
	// ceiling's underside (1 at normal size).
	int32 CeilingX = 0;
	int32 CeilingRows = 0;
	bool bHasCeiling = false;
	for (int32 K = 0; K <= LowGapLookCells && !bHasCeiling; ++K)
	{
		const int32 Column = Front.X + Dir * K;
		if (IsSolid(Island, Column, Row) || !IsSolid(Island, Column, Row - 1))
		{
			return false;
		}
		for (int32 Up = 1; Up < StandRows && !bHasCeiling; ++Up)
		{
			if (IsSolid(Island, Column, Row + Up))
			{
				CeilingX = Column;
				CeilingRows = Up;
				bHasCeiling = true;
			}
		}
	}
	if (!bHasCeiling)
	{
		return false;
	}
	const float FaceX = static_cast<float>(Island->CellToWorld(FIntPoint(CeilingX, Row)).X) - Dir * Cell * 0.5f;
	OutGap.Distance = (FaceX - FrontX) * Dir;
	// To jump over it instead, the ceiling's stack is like a wall this many rows high (our feet row counted). (Counted up to
	// a little more than we jump: 8 at normal size.)
	const int32 MaxOver = JumpableRows(Self) + 3;
	int32 Over = CeilingRows + 1;
	while (Over < MaxOver && IsSolid(Island, CeilingX, Row + Over))
	{
		++Over;
	}
	OutGap.OverHeight = Over;
	// Taller than two rows (a grown penguin, about 2.2x and up): the belly slide and the crawl out below only know gaps one
	// block high, and a slide this big would not fit under one anyway - so it is never a way through: over the top, break
	// the ice, or turn away (Navigate).
	if (StandRows > 2)
	{
		return true;
	}

	// Along under the ceiling to where there is room to stand again (or a hole to drop into).
	int32 Length = 0;
	int32 ExitX = 0;
	bool bHasExit = false;
	for (int32 K = 0; K < LowGapMaxCells && !bHasExit; ++K)
	{
		const int32 Column = CeilingX + Dir * K;
		if (IsSolid(Island, Column, Row))
		{
			return true; // a dead end
		}
		if (!IsSolid(Island, Column, Row + 1))
		{
			ExitX = Column; // room to stand up
			bHasExit = true;
		}
		else if (!IsSolid(Island, Column, Row - 1))
		{
			// A hole in the floor under the ceiling: we drop into it - fine if there is ground not far down.
			if (!HasGroundUnder(Island, FIntPoint(Column, Row - 1), LowGapMaxDrop))
			{
				return true;
			}
			ExitX = Column;
			bHasExit = true;
		}
		else
		{
			++Length;
		}
	}
	if (!bHasExit)
	{
		return true; // longer than we can see: treated as a dead end
	}
	// A short gap leaves the slide still fast at the far end, and it goes on (about six blocks from where it started, twice
	// that on ice): up to there flat ground or a few blocks down, or a wall that stops it - never on into the sea.
	const float Reach = (Movement->BellySlideDistance + Cell) * FMath::Max(Self->GetSizeScale(), 1.f) * (Movement->IsOnIce() ? 2.f : 1.f);
	const int32 ReachX = Front.X + Dir * FMath::CeilToInt(Reach / Cell);
	for (int32 Column = ExitX; (ReachX - Column) * Dir >= 0; Column += Dir)
	{
		if (IsSolid(Island, Column, Row))
		{
			break;
		}
		if (!HasGroundUnder(Island, FIntPoint(Column, Row), LowGapMaxDrop))
		{
			return true;
		}
	}
	// The belly capsule is clamped to at least the radius (SetLowCapsule) and scales with the penguin: a grown one may not fit.
	const float BellyHalf = FMath::Max(Movement->BellySlideHalfHeight, Self->GetCapsuleComponent()->GetUnscaledCapsuleRadius());
	const bool bBellyFits = 2.f * BellyHalf * Self->GetCapsuleComponent()->GetShapeScale() < Cell - 4.f;
	OutGap.Length = Length;
	// (Sticky can't slide: never a way through for it - over the top or away. Tiny fits standing: no low gap at all, above.)
	OutGap.bPassable = bBellyFits && Movement->CanBellySlideNow();
	return true;
}

bool UPenguinBotComponent::StartLowPassage(APenguinCharacter* Self, const ABlockIsland* Island, float Direction, const FLowGap& Gap)
{
	using namespace PenguinBotPrivate;
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!Movement || !Island || !Movement->CanBellySlideNow())
	{
		return false;
	}
	const float Dir = Direction >= 0.f ? 1.f : -1.f;
	// Already sliding that way (a slide attack): it just goes on through.
	const bool bSlidingThere = Movement->IsBellySliding() && Movement->GetBellySlideDirection() * Dir > 0.f;
	if (!bSlidingThere)
	{
		if (bJumpDown || Movement->IsBellySliding() || !Movement->IsMovingOnGround() || Movement->IsDashing() || Movement->IsClimbing()
			|| Movement->HasNoControl() || Movement->GetBellySlideCooldownRemaining() > 0.f)
		{
			return false;
		}
		Move = FVector2D(Dir, 0.f);
		Self->ApplyVirtualMoveInput(Move); // the slide goes along the stick
		Self->BellySlide();
	}
	const float Now = GetWorld()->GetTimeSeconds();
	TunnelDir = Dir;
	TunnelStartTime = Now;
	TunnelDeadline = Now + LowGapCommitExtra + (Gap.Length + 3) * Island->CellSize / FMath::Max(Movement->BellySlideCrawlSpeed, 100.f);
	CrawlStuckTimer = 0.f;
	return true;
}

bool UPenguinBotComponent::UpdateLowPassage(APenguinCharacter* Self, const ABlockIsland* Island, float Dt)
{
	using namespace PenguinBotPrivate;
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!Movement || !Island)
	{
		TunnelDeadline = -1.f;
		return false;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	const bool bLow = Movement->IsLyingLow();
	const bool bStarting = Now < TunnelStartTime + LowGapStartGrace; // (the slide press comes out a frame or two later)
	if (TunnelDeadline > 0.f && (Now > TunnelDeadline || Movement->HasNoControl() || Movement->IsFalling()
		|| (!bLow && !Movement->IsBellySliding() && !bStarting)))
	{
		TunnelDeadline = -1.f; // out and standing, knocked about, off a ledge, or too long: think as usual again
	}
	// Lying under a ceiling too low to stand up with no plan (a hit or a bump stopped the slide in there, a slide attack
	// went in, the plan ran out): crawl out - the way we want to go if that way leads out, else the other way.
	if (TunnelDeadline <= 0.f && bLow && Movement->IsMovingOnGround() && !Movement->HasNoControl() && Movement->IsStuckLow())
	{
		float Preferred = Movement->GetBellySlideDirection();
		if (const APenguinCharacter* T = Target.Get())
		{
			const float ToTarget = static_cast<float>(T->GetActorLocation().X - Self->GetActorLocation().X);
			if (FMath::Abs(ToTarget) > 50.f)
			{
				Preferred = FMath::Sign(ToTarget);
			}
		}
		int32 Cells = 0;
		const float Way = CrawlDirection(Self, Island, Preferred, Cells);
		TunnelDir = Way != 0.f ? Way : (Preferred >= 0.f ? 1.f : -1.f);
		TunnelStartTime = Now;
		TunnelDeadline = Now + LowGapCommitExtra + (Cells + 1) * Island->CellSize / FMath::Max(Movement->BellySlideCrawlSpeed, 100.f);
		CrawlStuckTimer = 0.f;
	}
	if (TunnelDeadline <= 0.f)
	{
		return false;
	}
	// Crawling without getting anywhere (a dead end - say an ice wall built at the far end since we went in): the other way
	// (the stick turns a crawl around).
	const FVector Loc = Self->GetActorLocation();
	if (bLow && Movement->IsMovingOnGround() && !bStarting)
	{
		CrawlStuckTimer = FVector::Dist(Loc, LastLocation) < 2.f ? CrawlStuckTimer + Dt : 0.f;
		if (CrawlStuckTimer > CrawlStuckTime)
		{
			int32 Cells = 0;
			const float Way = CrawlDirection(Self, Island, -TunnelDir, Cells);
			TunnelDir = Way != 0.f ? Way : -TunnelDir;
			TunnelStartTime = Now;
			TunnelDeadline = Now + LowGapCommitExtra + (Cells + 1) * Island->CellSize / FMath::Max(Movement->BellySlideCrawlSpeed, 100.f);
			CrawlStuckTimer = 0.f;
		}
	}
	LastLocation = Loc;
	StuckTimer = 0.f;
	Move = FVector2D(TunnelDir, 0.f);
	return true;
}

float UPenguinBotComponent::CrawlDirection(const APenguinCharacter* Self, const ABlockIsland* Island, float Preferred, int32& OutCells) const
{
	using namespace PenguinBotPrivate;
	OutCells = 0;
	if (!Island)
	{
		return 0.f;
	}
	const FIntPoint Here = Island->WorldToCell(Self->GetFeetLocation() + FVector(0.f, 0.f, 5.f));
	// Rows we need to stand up (2 at normal size: room to stand is no block right above our feet row; a grown penguin that
	// slid under a higher ceiling needs all of its height free).
	const UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	const int32 StandRows = Movement ? FMath::Max(2, RowsToFit(2.f * Movement->GetStandingHalfHeight(), Island->CellSize)) : 2;
	auto RoomToStand = [Island, &Here, StandRows](int32 Column)
	{
		for (int32 Up = 1; Up < StandRows; ++Up)
		{
			if (IsSolid(Island, Column, Here.Y + Up))
			{
				return false;
			}
		}
		return true;
	};
	// Columns to the way out on each side (right, left): room to stand, or a hole to drop into; a wall first: none.
	int32 ToExit[2] = { INDEX_NONE, INDEX_NONE };
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const int32 Step = Side == 0 ? 1 : -1;
		for (int32 K = 1; K <= LowGapMaxCells; ++K)
		{
			const int32 Column = Here.X + Step * K;
			if (IsSolid(Island, Column, Here.Y))
			{
				break;
			}
			if (RoomToStand(Column) || !IsSolid(Island, Column, Here.Y - 1))
			{
				// Room to stand, or a hole to drop into: a way out only with ground not far down (never into the sea).
				if (HasGroundUnder(Island, FIntPoint(Column, Here.Y), LowGapMaxDrop + 1))
				{
					ToExit[Side] = K;
				}
				break;
			}
		}
	}
	const int32 PreferredSide = Preferred < 0.f ? 1 : 0;
	const int32 Pick = ToExit[PreferredSide] != INDEX_NONE ? PreferredSide : 1 - PreferredSide;
	if (ToExit[Pick] == INDEX_NONE)
	{
		return 0.f;
	}
	OutCells = ToExit[Pick];
	return Pick == 0 ? 1.f : -1.f;
}

// ============================================================================ portals

void UPenguinBotComponent::TrackPortalTrips(const APenguinCharacter* Self, float Now)
{
	using namespace PenguinBotPrivate;
	const FVector Loc = Self->GetActorLocation();
	const float Moved = static_cast<float>(FVector::Dist(Loc, ThinkLocation));
	// (Forget the portals that closed.)
	PortalMemory.RemoveAllSwap([](const FPortalMemory& Memory) { return !Memory.Portal.IsValid(); });
	if (bHasThinkLocation && Moved > PortalTripMinJump)
	{
		TArray<APinguPortal*> Portals;
		APinguPortal::GetAll(GetWorld(), Portals);
		for (const APinguPortal* Portal : Portals)
		{
			if (!HasEnds(Portal))
			{
				continue;
			}
			// From near one end to near the other, about as far as they are apart (not just a fast move past a short portal).
			const float Span = static_cast<float>(FVector::Dist(Portal->GetEnd(0), Portal->GetEnd(1)));
			const bool bTrip = Moved > Span * 0.5f
				&& ((Portal->IsInEnd(0, ThinkLocation, PortalTripMargin) && Portal->IsInEnd(1, Loc, PortalTripMargin))
					|| (Portal->IsInEnd(1, ThinkLocation, PortalTripMargin) && Portal->IsInEnd(0, Loc, PortalTripMargin)));
			if (!bTrip)
			{
				continue;
			}
			// Through a portal (or our own warp): a gap jump or a low passage we began belongs to the other side.
			GapDeadline = -1.f;
			TunnelDeadline = -1.f;
			PortalRunUpUntil = -1.f;
			StuckTimer = 0.f;
			LastLocation = Loc;
			++PortalTrips;
			// Twice through this portal close together: back and forth (a loop) - leave it alone for a while.
			FPortalMemory* Memory = PortalMemory.FindByPredicate([Portal](const FPortalMemory& Entry) { return Entry.Portal.Get() == Portal; });
			if (!Memory)
			{
				FPortalMemory& Added = PortalMemory.AddDefaulted_GetRef();
				Added.Portal = Portal;
				Memory = &Added;
			}
			if (Now - Memory->LastTrip < PortalLoopWindow)
			{
				Memory->AvoidUntil = Now + PortalAvoidTime;
				UE_LOG(LogPingu, Verbose, TEXT("[BOT] %s looped through %s: leaves it alone for %.0f s"), *Self->GetName(), *Portal->GetName(), PortalAvoidTime);
			}
			Memory->LastTrip = Now;
			break;
		}
	}
	ThinkLocation = Loc;
	bHasThinkLocation = true;
}

bool UPenguinBotComponent::IsPortalAvoided(const APinguPortal* Portal) const
{
	const float Now = GetWorld()->GetTimeSeconds();
	const FPortalMemory* Memory = PortalMemory.FindByPredicate([Portal](const FPortalMemory& Entry) { return Entry.Portal.Get() == Portal; });
	return Memory && Now < Memory->AvoidUntil;
}

bool UPenguinBotComponent::WantsPortal(const APenguinCharacter* Self, const ABlockIsland* Island, const APinguPortal* Portal, int32 End,
	const FVector* Goal, bool bRecovering, bool bOnTheWay) const
{
	using namespace PenguinBotPrivate;
	if (!Island || !Portal || !HasEnds(Portal) || IsPortalAvoided(Portal))
	{
		return false;
	}
	const FVector In = Portal->GetEnd(End);
	const FVector Out = Portal->GetEnd(1 - End);
	if (IsFallLoop(In, Out) || !IsSafePortalExit(Self, Island, Out))
	{
		return false;
	}
	if (bRecovering)
	{
		return true; // off the island: anywhere safe on it beats the sea
	}
	return Goal && PortalGain(Self->GetActorLocation(), In, Out, *Goal) > (bOnTheWay ? PortalOnWayGain : PortalMinGain);
}

void UPenguinBotComponent::HandlePortals(APenguinCharacter* Self, const ABlockIsland* Island, float& Desired, const FVector* Goal)
{
	using namespace PenguinBotPrivate;
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!Island || !Movement)
	{
		return;
	}
	TArray<APinguPortal*> Portals;
	APinguPortal::GetAll(GetWorld(), Portals);
	if (Portals.Num() == 0)
	{
		return;
	}
	const FVector Loc = Self->GetActorLocation();
	const float Now = GetWorld()->GetTimeSeconds();
	const float HalfWidth = APinguPortal::EndHalfWidth;
	const float HalfHeight = APinguPortal::EndHalfHeight;

	// A shortcut: an end on our level (we walk right into it), not far, nothing in between, whose other end comes out on
	// the island much closer to the goal (and is no portal we just looped through).
	if (Goal && Movement->IsMovingOnGround() && FVector::Dist(Loc, *Goal) > PortalShortcutMinGoal)
	{
		float BestGain = PortalMinGain;
		float BestDir = 0.f;
		const int32 FeetRow = Island->WorldToCell(Self->GetFeetLocation() + FVector(0.f, 0.f, 5.f)).Y;
		for (const APinguPortal* Portal : Portals)
		{
			for (int32 End = 0; End < 2 && HasEnds(Portal); ++End)
			{
				const FVector In = Portal->GetEnd(End);
				const FVector Out = Portal->GetEnd(1 - End);
				const float Dx = static_cast<float>(In.X - Loc.X);
				if (Portal->IsInEnd(End, Loc, PortalStandMargin) || FMath::Abs(Dx) > PortalShortcutReach
					|| FMath::Abs(static_cast<float>(In.Z - Loc.Z)) > HalfHeight - 10.f)
				{
					continue;
				}
				const float Gain = PortalGain(Loc, In, Out, *Goal);
				// (Only an end we can walk to: floor all the way, so no gap or open air in between.)
				if (Gain > BestGain && WantsPortal(Self, Island, Portal, End, Goal, false)
					&& HasFloorBetween(Island, Island->WorldToCell(Loc).X, Island->WorldToCell(In).X, FeetRow) && HasLineOfSight(Loc, In))
				{
					BestGain = Gain;
					BestDir = FMath::Sign(Dx);
				}
			}
		}
		if (BestDir != 0.f)
		{
			Desired = BestDir;
			PortalRunUpUntil = -1.f;
		}
	}
	if (!Movement->IsMovingOnGround() || Movement->IsLyingLow())
	{
		PortalRunUpUntil = -1.f;
		return; // (in the air or on the belly we can't hop over one anyway: SteerClearOfPortals looks after the air)
	}
	// Backing off for a run-up at an end too close to jump: on until the run-up is done (no turning back and forth).
	if (Now < PortalRunUpUntil)
	{
		Desired = PortalRunUpDir;
		return;
	}
	if (FMath::Abs(Desired) < 0.1f)
	{
		return;
	}

	// An end right on our way: on into it if it helps, else over it. A full jump clears an end on our level when we take off
	// early enough (the faster we go, the earlier); our middle has to stay out of its oval.
	const float Dir = FMath::Sign(Desired);
	const float Speed = FMath::Max(0.f, static_cast<float>(Movement->Velocity.X) * Dir);
	const float TakeOffNear = HalfWidth + 20.f + Speed * 0.09f;
	const float TakeOffFar = TakeOffNear + FMath::Max(140.f, Speed * 0.15f);
	for (const APinguPortal* Portal : Portals)
	{
		for (int32 End = 0; End < 2 && HasEnds(Portal); ++End)
		{
			const FVector In = Portal->GetEnd(End);
			const float Ahead = static_cast<float>(In.X - Loc.X) * Dir;
			if (Ahead <= 0.f || Ahead > TakeOffFar || Portal->IsInEnd(End, Loc, PortalStandMargin)
				|| FMath::Abs(static_cast<float>(In.Z - Loc.Z)) >= HalfHeight)
			{
				continue; // behind us, still far off, the one we stand in, or we pass above or below it
			}
			if (WantsPortal(Self, Island, Portal, End, Goal, false, true))
			{
				continue; // it takes us closer (a short one just a little: it is on our way anyway): in we go
			}
			// Over it only with ground to land on behind it (an end at the island's edge: we would jump into the sea).
			const FIntPoint Feet = Island->WorldToCell(Self->GetFeetLocation() + FVector(0.f, 0.f, 5.f));
			const int32 LastColumn = Island->WorldToCell(FVector(In.X + Dir * (HalfWidth + 250.f), 0.f, Loc.Z)).X;
			const int32 Step = Dir > 0.f ? 1 : -1;
			bool bLanding = true;
			for (int32 Column = Feet.X; (LastColumn - Column) * Step >= 0 && bLanding; Column += Step)
			{
				bLanding = IsSolid(Island, Column, Feet.Y) || HasGroundUnder(Island, FIntPoint(Column, Feet.Y), 3);
			}
			if (!bLanding)
			{
				// Neither through it nor over it: stay out of it (stand, or the way back when we only wandered).
				Desired = 0.f;
				WanderDir = -Dir;
			}
			else if (Ahead >= TakeOffNear)
			{
				Jump(0.35f);
			}
			else
			{
				// Too close to clear it: back off a little and take a run at it - unless an end we don't want is right
				// behind us (we would back into that one): then we stand and wait.
				bool bRoomBehind = true;
				for (const APinguPortal* Other : Portals)
				{
					for (int32 OtherEnd = 0; OtherEnd < 2 && bRoomBehind && HasEnds(Other); ++OtherEnd)
					{
						const FVector Behind = Other->GetEnd(OtherEnd);
						const float Back = static_cast<float>(Loc.X - Behind.X) * Dir;
						bRoomBehind = !(Back > 0.f && Back < HalfWidth + PortalRunUpRoom && FMath::Abs(static_cast<float>(Behind.Z - Loc.Z)) < HalfHeight
							&& !Other->IsInEnd(OtherEnd, Loc, PortalStandMargin) && !WantsPortal(Self, Island, Other, OtherEnd, Goal, false, true));
					}
				}
				if (bRoomBehind)
				{
					PortalRunUpDir = -Dir;
					PortalRunUpUntil = Now + PortalRunUpTime;
					Desired = -Dir;
				}
				else
				{
					Desired = 0.f;
					WanderDir = -Dir;
				}
			}
			return;
		}
	}
}

void UPenguinBotComponent::SteerClearOfPortals(APenguinCharacter* Self, const ABlockIsland* Island)
{
	using namespace PenguinBotPrivate;
	UPenguinMovementComponent* Movement = Self->GetPenguinMovement();
	if (!Island || !Movement || !Movement->IsFalling() || GapDeadline > 0.f || TunnelDeadline > 0.f || Movement->HasNoControl())
	{
		return; // (a committed gap jump flies on; in a tumble the stick does nothing)
	}
	TArray<APinguPortal*> Portals;
	APinguPortal::GetAll(GetWorld(), Portals);
	if (Portals.Num() == 0)
	{
		return;
	}
	const bool bRecovering = IsOffStage(Self, Island);
	const FVector Loc = Self->GetActorLocation();
	const FVector Velocity = Movement->Velocity;
	// (The movement's own gravity - traits, the fall's extra pull - through the engine's public getter.)
	const float Gravity = static_cast<const UMovementComponent*>(Movement)->GetGravityZ();
	const FVector* Goal = bHasLastGoal ? &LastGoal : nullptr;
	// Where our flight goes in the next moment (no stick): the first unwanted end it goes into, and when.
	for (float T = PortalAirStep; T <= PortalAirLook + KINDA_SMALL_NUMBER; T += PortalAirStep)
	{
		const FVector At = Loc + Velocity * T + FVector(0.f, 0.f, 0.5f * Gravity * T * T);
		for (const APinguPortal* Portal : Portals)
		{
			for (int32 End = 0; End < 2 && HasEnds(Portal); ++End)
			{
				if (!Portal->IsInEnd(End, At, PortalAirMargin) || Portal->IsInEnd(End, Loc)
					|| WantsPortal(Self, Island, Portal, End, Goal, bRecovering))
				{
					continue; // (the one we are in right now: we just came out of it)
				}
				// Past it on the side we are already on (or the side we fly to).
				const float FromMiddle = static_cast<float>(Loc.X - Portal->GetEnd(End).X);
				float Away = FMath::Abs(FromMiddle) > 5.f ? FMath::Sign(FromMiddle) : FMath::Sign(static_cast<float>(Velocity.X));
				if (Away == 0.f)
				{
					Away = Target.IsValid() ? FMath::Sign(static_cast<float>(Target->GetActorLocation().X - Loc.X)) : 1.f;
					Away = Away == 0.f ? 1.f : Away;
				}
				// (No jump: up is often where the portal's other end is - a Snow Warp straight up.)
				Move = FVector2D(Away, 0.f);
				return;
			}
		}
	}
}

// ============================================================================ going in (after standing around)

float UPenguinBotComponent::CurrentAggression() const
{
	return bAggressive ? Skill.Aggression + (1.f - Skill.Aggression) * PenguinBotPrivate::AggroEagerness : Skill.Aggression;
}

float UPenguinBotComponent::AggroRange(const APenguinCharacter* Self, bool& bOutSlideOnly) const
{
	using namespace PenguinBotPrivate;
	bool bMelee = false;
	bool bRanged = false;
	ReadyAttacks(Self, bMelee, bRanged);
	// Nothing ready, or going in a while without one attack (the tornado not worth it, no line of sight ...): the slide.
	const bool bStalled = AggroElapsed > AggroStallTime && AggroAttacks == 0;
	bOutSlideOnly = bStalled || (!bMelee && !bRanged);
	const float Size = Self->GetSizeScale();
	if (bOutSlideOnly)
	{
		return AggroSlideRange * Size;
	}
	if (bMelee)
	{
		return AggroMeleeRange * Size;
	}
	return AggroRangedRange;
}

void UPenguinBotComponent::NoteFight(bool bLandedHit)
{
	IdleTime = 0.f;
	if (bAggressive && !bLandedHit)
	{
		++AggroAttacks;
	}
	if (bLandedHit && bAggressive)
	{
		bAggressive = false; // it worked: back to normal
		TargetTimer = 0.f;
	}
}

APenguinCharacter* UPenguinBotComponent::ChooseAggroTarget(const APenguinCharacter* Self, const ABlockIsland* Island) const
{
	using namespace PenguinBotPrivate;
	if (!Island)
	{
		return nullptr;
	}
	// Whoever leads the match (most round wins, at least one) is the one to stop.
	const APenguinGameState* State = APenguinGameState::Get(Self);
	int32 MostWins = 0;
	if (State)
	{
		for (const FPinguScore& Row : State->GetScores())
		{
			MostWins = FMath::Max(MostWins, Row.Wins);
		}
	}
	FVector2D Edges(-1.e9f, 1.e9f);
	if (State && State->HasDeadlyEdges())
	{
		Edges = State->GetDeadlyEdges();
	}
	APenguinCharacter* Best = nullptr;
	float BestScore = TNumericLimits<float>::Max();
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Other = *It;
		if (Other == Self || !Other->IsAlive() || Other->IsHidden() || Self->IsTeammate(Other))
		{
			continue;
		}
		// Reachable: over ground (not flying out over the sea), not at a deadly edge.
		const FVector Where = Other->GetActorLocation();
		float GroundZ = 0.f;
		if (!Island->FindGroundBelow(Where, 2500.f, GroundZ) || Where.X < Edges.X + PortalEdgeKeepAway || Where.X > Edges.Y - PortalEdgeKeepAway)
		{
			continue;
		}
		float Score = static_cast<float>(FVector::Dist(Where, Self->GetActorLocation()));
		if (State && MostWins > 0)
		{
			for (const FPinguScore& Row : State->GetScores())
			{
				if (Row.Penguin == Other && Row.Wins == MostWins)
				{
					Score *= 0.6f; // the leader
				}
			}
		}
		if (Score < BestScore)
		{
			BestScore = Score;
			Best = Other;
		}
	}
	return Best;
}

void UPenguinBotComponent::UpdateAggro(APenguinCharacter* Self, const ABlockIsland* Island)
{
	using namespace PenguinBotPrivate;
	// Game time since the last update (a think's own interval comes out in whole frames, a little longer than it says).
	const float Now = GetWorld()->GetTimeSeconds();
	const float Step = LastAggroUpdate >= 0.f ? FMath::Clamp(Now - LastAggroUpdate, 0.f, 0.25f) : 0.f;
	LastAggroUpdate = Now;
	// Host-side only: hits are known on the server (a LAN client's brain - the net test - plays as before).
	if (!Self->HasAuthority())
	{
		bAggressive = false;
		IdleTime = 0.f;
		return;
	}
	// Hit (our hit counter moved).
	if (const UPenguinMovementComponent* Movement = Self->GetPenguinMovement())
	{
		if (SeenHitEvents >= 0 && Movement->HitEvents != SeenHitEvents)
		{
			NoteFight(false);
		}
		SeenHitEvents = Movement->HitEvents;
	}
	// Landed a hit (somebody's last attacker is us, just now).
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		if (*It != Self && It->IsAlive() && It->GetLastAttacker(AggroHitWindow) == Self)
		{
			NoteFight(true);
			break;
		}
	}
	if (bAggressive)
	{
		AggroLeft -= Step;
		AggroElapsed += Step;
		if (AggroLeft <= 0.f)
		{
			bAggressive = false; // a few seconds and nothing: back to normal (and another 15 s before the next go)
			IdleTime = 0.f;
			TargetTimer = 0.f;
		}
		return;
	}
	IdleTime += Step;
	if (IdleTime < AggroIdleTime)
	{
		return;
	}
	APenguinCharacter* Pick = ChooseAggroTarget(Self, Island);
	if (!Pick)
	{
		IdleTime = AggroIdleTime - AggroRetry; // nobody to go for right now: look again in a moment
		return;
	}
	bAggressive = true;
	AggroLeft = AggroTime[FMath::Clamp(Difficulty, 0, 2)];
	AggroElapsed = 0.f;
	AggroAttacks = 0;
	++AggroCount;
	Target = Pick;
	AbilityTimer = FMath::Min(AbilityTimer, Skill.Reaction);
	SlideTimer = FMath::Min(SlideTimer, Skill.Reaction);
	WanderTimer = 0.f;
	UE_LOG(LogPingu, Verbose, TEXT("[BOT] %s stood around for %.0f s: goes in on %s"), *Self->GetName(), AggroIdleTime, *Pick->GetName());
}

bool UPenguinBotComponent::WarpLandsSafely(const APenguinCharacter* Self, const ABlockIsland* Island, const FVector& Aim) const
{
	using namespace PenguinBotPrivate;
	const FVector Start = Self->GetActorLocation();
	if (!Island)
	{
		Island = ABlockIsland::FindNear(GetWorld(), Start);
	}
	if (!Island)
	{
		return false;
	}
	// Where the warp ends: toward the aim, at most its range (closer when there is no room there: we can't tell that here).
	FVector Delta = Aim - Start;
	Delta.Y = 0.f;
	if (Delta.Size() > WarpBotRange)
	{
		Delta = Delta.GetSafeNormal() * WarpBotRange;
	}
	const FVector Arrival = Start + Delta;
	if (IsCrumblingAt(Island, static_cast<float>(Arrival.X), 150.f))
	{
		return false;
	}
	const FIntPoint Cell = Island->WorldToCell(Arrival);
	// Into a block: ice shatters around us (then the ground under it counts), anything else puts us on top of it.
	if (Island->IsSolidCell(Cell) && Island->GetBlock(Cell.X, Cell.Y) != EBlockKind::Ice)
	{
		return true;
	}
	return HasGroundUnder(Island, Cell, WarpMaxDropCells);
}
