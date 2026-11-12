// Pingu - the traits' gameplay (UPenguinTraitComponent's effect hooks). Server only: every function here runs where the
// gameplay runs (the server or a solo game); what it changes reaches the LAN clients through the replicated trait events,
// the penguin's own replication, the island's replication and the movement component's forwarded effects. Effects
// (puffs, sounds) go out inside FPinguFXBroadcastScope.
//
// What lives where:
//   - the numbers every trait has (cooldown, strength, knockback, movement, base size) are read straight from the table
//     by the abilities, ApplyPenguinHit and the movement's effective getters;
//   - this file is the rest: a use's strength (Gambler rolls it), Jester's swaps, Leech's cooldown cut, the shots a trait
//     takes (Mirror, Ice Touch, Hot Feet), Second Chance's fish, and the server tick (Ice Touch's ice and touches, Hot
//     Feet's melting, Giant's landings).
//   - the movement traits that need the owner's simulation (Sticky's cling and wall jump, Rubber's bounces) are in
//     UPenguinMovementComponent: every machine reads the replicated trait there.

#include "PenguinTraitComponent.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguFX.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

/** The effects' own state (one per penguin and trait: a new trait makes a new one). */
struct FPinguTraitEffectsState
{
	/** Gambler's rolls and Jester's picks (seeded from the trait's serial and the penguin's slot: a test replays the same). */
	FRandomStream Rand;
	bool bRandSeeded = false;
	/** Tests: Gambler's rolls forced - 0 every roll a fizzle, 1 a double, -1 random (SetForcedGamble). */
	int32 ForcedGamble = -1;

	// ---- Ice Touch
	/** Seconds until the snow under the feet may freeze again. */
	float FloorFreezeWait = 0.f;
	/** Penguins it gave frozen feet, and when (world time): each at most once per IceTouchFeetRepeat. */
	TArray<TPair<TWeakObjectPtr<APenguinCharacter>, double>> FeetTouches;

	// ---- Hot Feet
	/** The block it stands on, and how long it has stood on it. */
	bool bMeltCellValid = false;
	FIntPoint MeltCell = FIntPoint::ZeroValue;
	float MeltTime = 0.f;
	float SteamWait = 0.f;
	/** Seconds until it may melt the next touched ice block / peg. */
	float TouchMeltWait = 0.f;

	// ---- Second Chance: how high its feet got after the fish's throw, and whether it touched ground since (the log of a
	// second fall)
	float RescueHighestFeet = -1.e9f;
	bool bRescueLanded = false;

	// ---- Giant
	bool bLandSeen = false;
	int32 SeenLandEvents = 0;

	// ---- Jester
	struct FPendingSwap
	{
		int32 Slot = INDEX_NONE;
		EPenguinAbility Used = EPenguinAbility::None;
		double At = 0.0;
	};
	TArray<FPendingSwap> Swaps;
};

namespace PenguinTraitEffectsPrivate
{
	using namespace PinguTraitTuning;

	const FLinearColor SteamColor(0.9f, 0.9f, 0.93f);
	const FLinearColor EmberColor(1.f, 0.45f, 0.1f);
	const FLinearColor FrostColor(0.6f, 0.92f, 1.f);
	const FLinearColor MirrorColor(0.85f, 0.9f, 1.f);
	const FLinearColor LeechColor(0.6f, 0.08f, 0.28f);
	const FLinearColor FishColor(0.3f, 0.6f, 1.f);

	/** Ice Touch: two capsules this close (cm, surface to surface) touch. */
	constexpr float IceTouchContact = 6.f;
	/** Hot Feet: a block under the feet steams this often while it heats up (s). */
	constexpr float HotFeetSteamInterval = 0.3f;
	/** Giant: the ice it breaks on a landing is in the row under its feet, its footprint plus this much on each side (cm). */
	constexpr float GiantBreakMargin = 25.f;
	/** Second Chance: the throw starts this far above the sea level (cm; the sea check is at SeaLevel + 120) ... */
	constexpr float SecondChanceStartAboveSea = 130.f;
	/** ... and in sudden death it aims only at columns at least this many cells inside the crumbling edge (none left: any). */
	constexpr float SecondChanceCollapseMargin = 2.f;
	/** It passes through the island's blocks at most this long after it would be over the highest one (s: a safety net). */
	constexpr float SecondChancePassSpare = 0.4f;

	static FPinguTraitEffectsState& GetOrMake(TSharedPtr<FPinguTraitEffectsState>& State)
	{
		if (!State.IsValid())
		{
			State = MakeShared<FPinguTraitEffectsState>();
		}
		return *State;
	}

	/** The flow clock the replicated trait events are on (APenguinGameState::GetFlowTime; the world's clock without one). */
	static double FlowNow(const UObject* Ctx)
	{
		if (const APenguinGameState* GameState = APenguinGameState::Get(Ctx))
		{
			return GameState->GetFlowTime();
		}
		const UWorld* World = Ctx ? Ctx->GetWorld() : nullptr;
		return World ? World->GetTimeSeconds() : 0.0;
	}

	static double WorldNow(const UObject* Ctx)
	{
		const UWorld* World = Ctx ? Ctx->GetWorld() : nullptr;
		return World ? World->GetTimeSeconds() : 0.0;
	}

	/** Distance (cm) between a standing capsule's axis and a block's square (play plane); 0 inside. */
	static float CapsuleToCell(const FVector& Center, float AxisHalf, const FVector& CellCenter, float HalfCell)
	{
		const float Dx = FMath::Max3(static_cast<float>(CellCenter.X - HalfCell - Center.X), 0.f, static_cast<float>(Center.X - (CellCenter.X + HalfCell)));
		const float Dz = FMath::Max3(static_cast<float>(CellCenter.Z - HalfCell - (Center.Z + AxisHalf)), 0.f,
			static_cast<float>((Center.Z - AxisHalf) - (CellCenter.Z + HalfCell)));
		return FMath::Sqrt(Dx * Dx + Dz * Dz);
	}

	/** The block the penguin stands on: under the middle of its feet, else under one side of them. */
	static bool FindStandCell(const ABlockIsland* Island, const APenguinCharacter* Penguin, FIntPoint& OutCell)
	{
		const FVector Below = Penguin->GetFeetLocation() - FVector(0.f, 0.f, Island->CellSize * 0.5f);
		const float Side = FMath::Max(Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius() - 4.f, 0.f);
		for (const float Dx : { 0.f, -Side, Side })
		{
			const FIntPoint Cell = Island->WorldToCell(Below + FVector(Dx, 0.f, 0.f));
			if (Island->IsSolidCell(Cell))
			{
				OutCell = Cell;
				return true;
			}
		}
		return false;
	}

	/** A puff of steam rising off something hot (and a hiss). */
	static void Steam(const UObject* Ctx, const FVector& Where, int32 Count, bool bSound)
	{
		APinguFX::Puffs(Ctx, Where, Count, 26.f, 160.f, FVector::UpVector, 0.6f, SteamColor);
		APinguFX::Sparkles(Ctx, Where, FMath::Max(1, Count / 2), 30.f, EmberColor);
		if (bSound)
		{
			APinguFX::Sound(Ctx, EPinguSound::Melt, 0.7f);
		}
	}

	// ------------------------------------------------------------------ Jester
	static void TickJester(UPenguinTraitComponent& Traits, FPinguTraitEffectsState& S, APenguinCharacter* Penguin)
	{
		if (S.Swaps.Num() == 0)
		{
			return;
		}
		UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
		const double Now = WorldNow(Penguin);
		for (int32 Index = 0; Index < S.Swaps.Num();)
		{
			if (Now < S.Swaps[Index].At)
			{
				++Index;
				continue;
			}
			const FPinguTraitEffectsState::FPendingSwap Swap = S.Swaps[Index];
			S.Swaps.RemoveAt(Index);
			// A bubble changed the slot meanwhile: that one stays.
			if (!Abilities || Abilities->GetAbility(Swap.Slot) != Swap.Used)
			{
				continue;
			}
			// A random other ability: none of the ones the penguin has (the one used included).
			TArray<EPenguinAbility> Choices;
			for (int32 Value = 1; Value < static_cast<int32>(EPenguinAbility::Count); ++Value)
			{
				const EPenguinAbility Candidate = static_cast<EPenguinAbility>(Value);
				if (!Abilities->GetAbilities().Contains(Candidate))
				{
					Choices.Add(Candidate);
				}
			}
			if (Choices.Num() == 0)
			{
				continue;
			}
			const EPenguinAbility New = Choices[S.Rand.RandRange(0, Choices.Num() - 1)];
			Abilities->ReplaceAbility(Swap.Slot, New); // its cooldown carries over
			++Traits.GetMutableEvents().JesterSwaps;
			++Traits.GetMutableCounters().Swaps;
			{
				FPinguFXBroadcastScope Broadcast(Penguin);
				APinguFX::Sparkles(Penguin, Penguin->GetAimOrigin() + FVector(0.f, 0.f, 50.f * Penguin->GetSizeScale()), 6, 40.f, PinguTraits::GetInfo(EPinguTrait::Jester).Color);
			}
			UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Jester): slot %d %s -> %s"), *Penguin->GetDisplayName(), Swap.Slot,
				UPenguinAbilityComponent::GetInfo(Swap.Used).Name, UPenguinAbilityComponent::GetInfo(New).Name);
			Traits.OnLookStateChanged();
			Penguin->ForceNetUpdate();
		}
	}

	// ------------------------------------------------------------------ Ice Touch
	static void TickIceTouch(UPenguinTraitComponent& Traits, FPinguTraitEffectsState& S, APenguinCharacter* Penguin, float DeltaTime)
	{
		UWorld* World = Penguin->GetWorld();
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		// The snow under its feet turns to ice (and then it stands on its own ice: it slips).
		S.FloorFreezeWait -= DeltaTime;
		if (Move->IsMovingOnGround() && S.FloorFreezeWait <= 0.f)
		{
			S.FloorFreezeWait = IceTouchFloorInterval;
			if (ABlockIsland* Island = ABlockIsland::FindNear(World, Penguin->GetActorLocation()))
			{
				const FVector Below = Penguin->GetFeetLocation() - FVector(0.f, 0.f, Island->CellSize * 0.5f);
				const int32 Frozen = Island->FreezeBlocksInRadius(Below, IceTouchFloorRadius * FMath::Max(Penguin->GetSizeScale(), 1.f));
				if (Frozen > 0)
				{
					Traits.GetMutableCounters().IceCells += Frozen;
					FPinguFXBroadcastScope Broadcast(Penguin);
					APinguFX::Sparkles(Penguin, Penguin->GetFeetLocation(), 4, 40.f, FrostColor);
					APinguFX::Sound(Penguin, EPinguSound::Freeze, 0.25f, 0.08f, 1.4f);
				}
			}
		}

		// Whoever it touches gets frozen feet (each one at most every IceTouchFeetRepeat seconds). An ice shell keeps it off,
		// Hot Feet can't be frozen.
		const double Now = WorldNow(Penguin);
		S.FeetTouches.RemoveAll([Now](const TPair<TWeakObjectPtr<APenguinCharacter>, double>& Entry)
		{
			return !Entry.Key.IsValid() || Now - Entry.Value >= IceTouchFeetRepeat;
		});
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const FVector Center = Penguin->GetActorLocation();
		const float Radius = Capsule->GetScaledCapsuleRadius();
		const float AxisHalf = FMath::Max(Capsule->GetScaledCapsuleHalfHeight() - Radius, 0.f);
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			APenguinCharacter* Other = *It;
			if (Other == Penguin || !Other->IsAlive() || Other->IsHidden() || Other->IsShielded()
				|| (Other->GetTraits() && Other->GetTraits()->IsFreezeImmune())
				|| S.FeetTouches.ContainsByPredicate([Other](const TPair<TWeakObjectPtr<APenguinCharacter>, double>& Entry) { return Entry.Key.Get() == Other; }))
			{
				continue;
			}
			const UCapsuleComponent* OtherCapsule = Other->GetCapsuleComponent();
			const float OtherRadius = OtherCapsule->GetScaledCapsuleRadius();
			const float OtherAxisHalf = FMath::Max(OtherCapsule->GetScaledCapsuleHalfHeight() - OtherRadius, 0.f);
			const FVector OtherCenter = Other->GetActorLocation();
			FVector OnSelf;
			FVector OnOther;
			FMath::SegmentDistToSegmentSafe(Center - FVector(0.f, 0.f, AxisHalf), Center + FVector(0.f, 0.f, AxisHalf),
				OtherCenter - FVector(0.f, 0.f, OtherAxisHalf), OtherCenter + FVector(0.f, 0.f, OtherAxisHalf), OnSelf, OnOther);
			OnSelf.Y = OnOther.Y = 0.f;
			if (FVector::Dist(OnSelf, OnOther) > Radius + OtherRadius + IceTouchContact)
			{
				continue;
			}
			S.FeetTouches.Add(TPair<TWeakObjectPtr<APenguinCharacter>, double>(Other, Now));
			Other->SetSlippery(IceTouchFeetTime); // the ice gun's frozen feet (its effects go out by themselves)
			UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Ice Touch) gave %s frozen feet"), *Penguin->GetDisplayName(), *Other->GetDisplayName());
		}
	}

	// ------------------------------------------------------------------ Hot Feet
	static void TickHotFeet(UPenguinTraitComponent& Traits, FPinguTraitEffectsState& S, APenguinCharacter* Penguin, float DeltaTime)
	{
		UWorld* World = Penguin->GetWorld();
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		ABlockIsland* Island = ABlockIsland::FindNear(World, Penguin->GetActorLocation());
		if (!Island)
		{
			return;
		}
		const float HalfCell = Island->CellSize * 0.5f;

		// The block it stands on heats up and melts after HotFeetMeltTime (snow, rock or ice - never the deep rock).
		FIntPoint StandCell;
		if (Move->IsMovingOnGround() && FindStandCell(Island, Penguin, StandCell))
		{
			if (!S.bMeltCellValid || S.MeltCell != StandCell)
			{
				S.bMeltCellValid = true;
				S.MeltCell = StandCell;
				S.MeltTime = 0.f;
				S.SteamWait = 0.f;
			}
			const EBlockKind Kind = Island->GetBlock(StandCell.X, StandCell.Y);
			if (Kind != EBlockKind::DeepRock && Kind != EBlockKind::None)
			{
				S.MeltTime += DeltaTime;
				const FVector Top = Island->CellToWorld(StandCell) + FVector(0.f, 0.f, HalfCell);
				S.SteamWait -= DeltaTime;
				if (S.SteamWait <= 0.f && S.MeltTime < HotFeetMeltTime)
				{
					// It heats up: steam, more as it gets close (the warning before the floor goes).
					S.SteamWait = HotFeetSteamInterval;
					FPinguFXBroadcastScope Broadcast(Penguin);
					APinguFX::Puffs(Penguin, Top, 1 + FMath::FloorToInt(3.f * S.MeltTime / HotFeetMeltTime), 18.f, 120.f, FVector::UpVector, 0.7f, SteamColor);
				}
				if (S.MeltTime >= HotFeetMeltTime)
				{
					const FVector Middle = Island->CellToWorld(StandCell);
					if (Island->BreakBlockAt(Middle))
					{
						++Traits.GetMutableCounters().MeltedBlocks;
						FPinguFXBroadcastScope Broadcast(Penguin);
						Steam(Penguin, Top, 6, true);
						UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Hot Feet) melted the block it stood on (%d, %d) after %.2f s"), *Penguin->GetDisplayName(),
							StandCell.X, StandCell.Y, S.MeltTime);
					}
					S.bMeltCellValid = false;
				}
			}
		}
		else
		{
			S.bMeltCellValid = false; // off the ground (or onto another block): it starts again
		}

		// Ice it touches (walls, icebergs, frozen snow beside it - not the floor: that one melts as above), icicle pegs and
		// ground pads (not its own), one at a time.
		S.TouchMeltWait -= DeltaTime;
		if (S.TouchMeltWait > 0.f)
		{
			return;
		}
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const FVector Center = Penguin->GetActorLocation();
		const float Radius = Capsule->GetScaledCapsuleRadius();
		const float AxisHalf = FMath::Max(Capsule->GetScaledCapsuleHalfHeight() - Radius, 0.f);
		const float FeetZ = static_cast<float>(Penguin->GetFeetLocation().Z);
		const float Reach = Radius + HotFeetTouchReach;
		const FIntPoint Low = Island->WorldToCell(Center - FVector(Reach, 0.f, Reach + AxisHalf));
		const FIntPoint High = Island->WorldToCell(Center + FVector(Reach, 0.f, Reach + AxisHalf));
		float BestDistance = TNumericLimits<float>::Max();
		FIntPoint BestCell = FIntPoint::ZeroValue;
		for (int32 X = Low.X; X <= High.X; ++X)
		{
			for (int32 Z = Low.Y; Z <= High.Y; ++Z)
			{
				if (Island->GetBlock(X, Z) != EBlockKind::Ice)
				{
					continue;
				}
				const FVector CellCenter = Island->CellToWorld(FIntPoint(X, Z));
				if (static_cast<float>(CellCenter.Z) + HalfCell <= FeetZ + 5.f)
				{
					continue; // under the feet: the floor
				}
				const float Distance = CapsuleToCell(Center, AxisHalf, CellCenter, HalfCell);
				if (Distance < Reach && Distance < BestDistance)
				{
					BestDistance = Distance;
					BestCell = FIntPoint(X, Z);
				}
			}
		}
		if (BestDistance < TNumericLimits<float>::Max())
		{
			const FVector Middle = Island->CellToWorld(BestCell);
			if (Island->BreakBlockAt(Middle))
			{
				S.TouchMeltWait = HotFeetTouchInterval;
				++Traits.GetMutableCounters().MeltedBlocks;
				FPinguFXBroadcastScope Broadcast(Penguin);
				Steam(Penguin, Middle, 5, true);
				UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Hot Feet) melted the ice it touched (%d, %d)"), *Penguin->GetDisplayName(), BestCell.X, BestCell.Y);
				return;
			}
		}
		TArray<APenguinProjectile*> Projectiles;
		APenguinProjectile::GetAll(World, Projectiles);
		for (APenguinProjectile* Shot : Projectiles)
		{
			if (Shot->GetShooter() == Penguin || (!Shot->HasPeg() && !Shot->IsGroundPad()))
			{
				continue;
			}
			bool bTouches = false;
			if (Shot->HasPeg())
			{
				bTouches = Penguin->DistanceTo(Shot->GetPegTop()) < HotFeetTouchReach + 10.f
					|| Penguin->DistanceTo(Shot->GetActorLocation()) < HotFeetTouchReach + Shot->GetRadius();
			}
			else
			{
				FVector PadA;
				FVector PadB;
				float PadRadius = 0.f;
				Shot->GetGroundPadSegment(PadA, PadB, PadRadius);
				FVector OnSelf;
				FVector OnPad;
				FMath::SegmentDistToSegmentSafe(Center - FVector(0.f, 0.f, AxisHalf), Center + FVector(0.f, 0.f, AxisHalf), PadA, PadB, OnSelf, OnPad);
				bTouches = FVector::Dist(OnSelf, OnPad) < Reach + PadRadius;
			}
			if (bTouches)
			{
				S.TouchMeltWait = HotFeetTouchInterval;
				const FVector Where = Shot->GetActorLocation();
				const TCHAR* What = Shot->HasPeg() ? TEXT("peg") : TEXT("pad");
				FPinguFXBroadcastScope Broadcast(Penguin);
				Shot->Shatter();
				Steam(Penguin, Where, 4, true);
				UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Hot Feet) melted an icicle %s"), *Penguin->GetDisplayName(), What);
				return;
			}
		}
	}

	// ------------------------------------------------------------------ Second Chance
	/** After the fish's throw: how high it got, whether it came down on something (the log of a second fall). */
	static void TickSecondChance(FPinguTraitEffectsState& S, APenguinCharacter* Penguin)
	{
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		S.RescueHighestFeet = FMath::Max(S.RescueHighestFeet, static_cast<float>(Penguin->GetFeetLocation().Z));
		S.bRescueLanded |= Move->IsMovingOnGround();
	}

	// ------------------------------------------------------------------ Giant
	static void TickGiant(FPinguTraitEffectsState& S, APenguinCharacter* Penguin, bool bActive)
	{
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		if (!S.bLandSeen)
		{
			S.bLandSeen = true;
			S.SeenLandEvents = Move->LandEvents; // (landings from before it was a giant are not its)
			return;
		}
		if (Move->LandEvents == S.SeenLandEvents)
		{
			return;
		}
		S.SeenLandEvents = Move->LandEvents;
		if (!bActive || Move->LastLandSpeed < GiantBreakLandSpeed || Move->IsStomping())
		{
			return; // (a belly stomp smashes its own crater)
		}
		ABlockIsland* Island = ABlockIsland::FindNear(Penguin->GetWorld(), Penguin->GetActorLocation());
		if (!Island)
		{
			return;
		}
		// The ice in the row right under its feet, nearest first, up to GiantBreakBlocks.
		const FVector Feet = Penguin->GetFeetLocation();
		const float Side = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius() + GiantBreakMargin;
		const int32 Row = Island->WorldToCell(Feet - FVector(0.f, 0.f, Island->CellSize * 0.5f)).Y;
		const int32 MinX = Island->WorldToCell(Feet - FVector(Side, 0.f, 0.f)).X;
		const int32 MaxX = Island->WorldToCell(Feet + FVector(Side, 0.f, 0.f)).X;
		TArray<FIntPoint> Ice;
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			if (Island->GetBlock(X, Row) == EBlockKind::Ice)
			{
				Ice.Add(FIntPoint(X, Row));
			}
		}
		Ice.Sort([Island, &Feet](const FIntPoint& A, const FIntPoint& B)
		{
			return FMath::Abs(Island->CellToWorld(A).X - Feet.X) < FMath::Abs(Island->CellToWorld(B).X - Feet.X);
		});
		int32 Broken = 0;
		FPinguFXBroadcastScope Broadcast(Penguin);
		for (const FIntPoint& Cell : Ice)
		{
			if (Broken >= GiantBreakBlocks)
			{
				break;
			}
			Broken += Island->BreakIceAt(Island->CellToWorld(Cell), FVector(0.f, 0.f, -1.f)) ? 1 : 0;
		}
		if (Broken > 0)
		{
			APinguFX::Shake(Penguin, 0.2f);
			UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Giant) landed at %.0f cm/s and broke %d ice blocks"), *Penguin->GetDisplayName(), Move->LastLandSpeed, Broken);
		}
	}

	// ------------------------------------------------------------------ Second Chance
	/** What the fish aims at: the middle of what still stands of the island, the island's flat ground nearest that middle,
	 *  and the island's highest block (the throw goes over it). */
	struct FRescueTarget
	{
		/** The middle between the outermost columns with ground (world X). */
		float MidX = 0.f;
		/** Where it should come down (X) and the top of the ground there: the point nearest MidX that is at least a block in
		 *  from the ends of a level stretch of ground (3 columns or more with the same top: never the edge of a platform or
		 *  of a gap); without one, the column with ground nearest MidX. */
		float LandX = 0.f;
		float LandTopZ = 0.f;
		/** The outermost columns with ground (their middles). */
		float MinX = 0.f;
		float MaxX = 0.f;
		/** The top of the island's highest block (anywhere, crumbling or not). */
		float HighestTopZ = 0.f;
	};

	/** The island's columns with ground (in sudden death with bStandingOnly only those at least SecondChanceCollapseMargin
	 *  cells inside the crumbling edge: they still stand when it comes down). False: none. */
	static bool FindRescueTarget(const ABlockIsland* Island, bool bStandingOnly, FRescueTarget& Out)
	{
		const FBox Bounds = Island->GetBounds();
		if (!Bounds.IsValid)
		{
			return false;
		}
		const float CellSize = Island->CellSize;
		const float SearchTop = static_cast<float>(Bounds.Max.Z) + 10.f;
		const float SearchDepth = static_cast<float>(Bounds.Max.Z - Bounds.Min.Z) + 2.f * CellSize;
		const int32 FirstCell = Island->WorldToCell(FVector(Bounds.Min.X + 0.5f * CellSize, 0.f, Bounds.Min.Z + 0.5f * CellSize)).X;
		const int32 LastCell = Island->WorldToCell(FVector(Bounds.Max.X - 0.5f * CellSize, 0.f, Bounds.Min.Z + 0.5f * CellSize)).X;
		const bool bOnlyStanding = bStandingOnly && Island->IsCollapsing();
		struct FColumn
		{
			int32 Cell = 0;
			float X = 0.f;
			float Top = 0.f;
		};
		TArray<FColumn, TInlineAllocator<64>> Columns;
		for (int32 CellX = FirstCell; CellX <= LastCell; ++CellX)
		{
			const float X = static_cast<float>(Island->CellToWorld(FIntPoint(CellX, 0)).X);
			if (bOnlyStanding && FMath::Abs(X - Island->GetCollapseCenterX()) >= Island->GetCollapseRadius() - SecondChanceCollapseMargin * CellSize)
			{
				continue;
			}
			float Top = 0.f;
			if (Island->FindGroundBelow(FVector(X, 0.f, SearchTop), SearchDepth, Top))
			{
				Columns.Add({ CellX, X, Top });
			}
		}
		if (Columns.Num() == 0)
		{
			return false;
		}
		Out.MinX = Columns[0].X;
		Out.MaxX = Columns.Last().X;
		Out.MidX = 0.5f * (Out.MinX + Out.MaxX);
		Out.HighestTopZ = static_cast<float>(Bounds.Max.Z);
		// Level stretches (neighbouring columns, the same top): the point nearest the middle at least a block in from both ends.
		bool bFlat = false;
		float BestDistance = TNumericLimits<float>::Max();
		for (int32 First = 0; First < Columns.Num();)
		{
			int32 Last = First;
			while (Last + 1 < Columns.Num() && Columns[Last + 1].Cell == Columns[Last].Cell + 1 && FMath::IsNearlyEqual(Columns[Last + 1].Top, Columns[First].Top, 1.f))
			{
				++Last;
			}
			if (Last - First >= 2)
			{
				const float X = FMath::Clamp(Out.MidX, Columns[First + 1].X, Columns[Last - 1].X);
				if (FMath::Abs(X - Out.MidX) < BestDistance)
				{
					BestDistance = FMath::Abs(X - Out.MidX);
					Out.LandX = X;
					Out.LandTopZ = Columns[First].Top;
					bFlat = true;
				}
			}
			First = Last + 1;
		}
		if (!bFlat)
		{
			const FColumn* Nearest = &Columns[0];
			for (const FColumn& Column : Columns)
			{
				if (FMath::Abs(Column.X - Out.MidX) < FMath::Abs(Nearest->X - Out.MidX))
				{
					Nearest = &Column;
				}
			}
			Out.LandX = Nearest->X;
			Out.LandTopZ = Nearest->Top;
		}
		return true;
	}

	/** Seconds a fall of Height (cm) takes with Gravity (cm/s^2) from rest, never faster than MaxSpeed (cm/s). */
	static float FallTimeFor(float Height, float Gravity, float MaxSpeed)
	{
		if (Height <= 0.f)
		{
			return 0.f;
		}
		const float TimeToMax = MaxSpeed / FMath::Max(Gravity, 1.f);
		const float HeightToMax = 0.5f * Gravity * TimeToMax * TimeToMax;
		return Height <= HeightToMax ? FMath::Sqrt(2.f * Height / FMath::Max(Gravity, 1.f)) : TimeToMax + (Height - HeightToMax) / FMath::Max(MaxSpeed, 1.f);
	}

	/** How far along X (cm) something thrown sideways at Speed gets while it slows down at Decel (cm/s^2) for Time seconds
	 *  (never back the other way); OutSpeed: what is left of the speed. */
	static float Coast(float Speed, float Time, float Decel, float& OutSpeed)
	{
		if (Decel > 0.f && Speed <= Decel * Time)
		{
			OutSpeed = 0.f;
			return Speed * Speed / (2.f * Decel);
		}
		OutSpeed = Speed - Decel * Time;
		return Speed * Time - 0.5f * Decel * Time * Time;
	}

	/** How far along X the fish's throw at Speed sideways gets before its middle is down at the landing height: held like a
	 *  hit on the way up (the knockback's air drag), then the air braking of a stick let go on the way down. */
	static float RescueReach(float Speed, float RiseTime, float RiseDrag, float FallTime, float FallBraking)
	{
		float AtTop = 0.f;
		const float Up = Coast(Speed, RiseTime, RiseDrag, AtTop);
		float Left = 0.f;
		return Up + Coast(AtTop, FallTime, FallBraking, Left);
	}
}

// ---------------------------------------------------------------------------- the hooks

void UPenguinTraitComponent::ResetEffects()
{
	EffectsState.Reset(); // a new trait (or none): pending swaps, timers and touches are gone
}

void UPenguinTraitComponent::TickEffects(float DeltaTime)
{
	using namespace PenguinTraitEffectsPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	if (!Move || !Penguin->IsAlive() || Penguin->IsHidden())
	{
		return;
	}
	FPinguTraitEffectsState& S = GetOrMake(EffectsState);
	if (!S.bRandSeeded)
	{
		S.bRandSeeded = true;
		S.Rand.Initialize(static_cast<int32>(State.Serial) * 7919 + Penguin->GetPlayerSlot() * 104729 + 12345);
	}
	TickJester(*this, S, Penguin);
	TickSecondChance(S, Penguin);
	// What the trait does to the island and the others: once the penguin has changed (the reveal's transform), and only in
	// the fight (not while the intro, the countdown or the round's end hold everybody still).
	const bool bActive = IsTransformed() && !Move->IsInputLocked();
	switch (State.Trait)
	{
	case EPinguTrait::Giant:
		TickGiant(S, Penguin, bActive);
		break;
	case EPinguTrait::IceTouch:
		if (bActive)
		{
			TickIceTouch(*this, S, Penguin, DeltaTime);
		}
		break;
	case EPinguTrait::HotFeet:
		if (bActive)
		{
			TickHotFeet(*this, S, Penguin, DeltaTime);
		}
		break;
	case EPinguTrait::DeadlyEdges:
		// Past the island's left / right side (as it was at the round's start, plus a margin): out at once, however high - no
		// flying back (Linus 2026-11-07: "edges kill"). Only in the fight (the round's end and the intro hold everybody; practice
		// has no round trait - only the autotest sets one there).
		if (bActive)
		{
			const APenguinGameState* GameState = APenguinGameState::Get(Penguin);
			if (GameState && GameState->HasDeadlyEdges() && (GameState->GetPhase() == EPinguPhase::Fight || GameState->GetPhase() == EPinguPhase::Practice))
			{
				const FVector2D Edges = GameState->GetDeadlyEdges();
				const float X = static_cast<float>(Penguin->GetActorLocation().X);
				if (X < Edges.X || X > Edges.Y)
				{
					++Counters.EdgeKnockOuts;
					const float Side = X < Edges.X ? -1.f : 1.f;
					UE_LOG(LogPingu, Display, TEXT("[TRAITS] %s (Deadly Edges) went past the %s edge at x %.0f (edges %.0f / %.0f)"), *Penguin->GetDisplayName(),
						Side < 0.f ? TEXT("left") : TEXT("right"), X, Edges.X, Edges.Y);
					Penguin->KnockOut(EPenguinKOReason::Edge, nullptr, FVector2D(Side * 900.f, 1100.f));
				}
			}
		}
		break;
	default:
		break;
	}
}

void UPenguinTraitComponent::OnKnockbackTaken(APenguinCharacter* By, const FVector2D& Push)
{
	using namespace PenguinTraitEffectsPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	if (!IsServer() || !Penguin)
	{
		return;
	}
	// Damage %: every hit makes the next ones send it further (the number shows under the penguin on every machine).
	if (State.Trait == EPinguTrait::DamagePercent && IsTransformed())
	{
		const int32 Step = FMath::RoundToInt(DamageStep * 100.f);
		Events.DamagePercent = static_cast<uint16>(FMath::Min<int32>(Events.DamagePercent + Step, FMath::RoundToInt(DamageMax * 100.f)));
		++Counters.DamageHits;
		Penguin->ForceNetUpdate();
		UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Damage %%) took a hit: %d %%"), *Penguin->GetDisplayName(), Events.DamagePercent);
	}
	// Recoil (By's trait): the hit pushes By back with half of it - away from us, a little up. Not its own hits, not a frozen or
	// knocked-out attacker.
	UPenguinTraitComponent* ByTraits = By && By != Penguin ? By->GetTraits() : nullptr;
	UPenguinMovementComponent* ByMove = By ? By->GetPenguinMovement() : nullptr;
	if (ByTraits && ByMove && ByTraits->HasTrait(EPinguTrait::Recoil) && ByTraits->IsTransformed() && By->IsAlive() && !By->IsHidden()
		&& !ByMove->IsFrozen() && FMath::Abs(Push.X) >= RecoilMinPush)
	{
		const float Away = FMath::Sign(By->GetActorLocation().X - Penguin->GetActorLocation().X) != 0.f
			? FMath::Sign(static_cast<float>(By->GetActorLocation().X - Penguin->GetActorLocation().X)) : -FMath::Sign(Push.X);
		const float Speed = FMath::Min(FMath::Abs(Push.X) * RecoilShare, RecoilMax);
		ByMove->ApplyKnockback(FVector2D(Away * Speed, RecoilLift), 0.f); // reaches a remote owner by itself
		++ByTraits->Events.Recoils;
		++ByTraits->Counters.RecoilPushes;
		ByTraits->Counters.LastRecoilSpeed = Speed;
		By->ForceNetUpdate();
		FPinguFXBroadcastScope Broadcast(By);
		APinguFX::Puffs(By, By->GetFeetLocation(), 4, 20.f * By->GetSizeScale(), 220.f, FVector(-Away, 0.f, 0.3f).GetSafeNormal(), 1.f);
		UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Recoil) hit %s: pushed back at %.0f cm/s"), *By->GetDisplayName(), *Penguin->GetDisplayName(), Speed);
	}
}

float UPenguinTraitComponent::RollUseStrength(EPenguinAbility Ability)
{
	using namespace PenguinTraitEffectsPrivate;
	if (!HasAnyTrait() || !IsServer())
	{
		return 1.f;
	}
	++Counters.Uses;
	if (State.Trait != EPinguTrait::Gambler)
	{
		return GetNumbers().Strength;
	}
	// Gambler: twice as strong, or nothing at all.
	FPinguTraitEffectsState& S = GetOrMake(EffectsState);
	if (!S.bRandSeeded)
	{
		S.bRandSeeded = true;
		const APenguinCharacter* Penguin = GetPenguin();
		S.Rand.Initialize(static_cast<int32>(State.Serial) * 7919 + (Penguin ? Penguin->GetPlayerSlot() : 0) * 104729 + 12345);
	}
	const float Roll = S.Rand.FRand();
	const bool bDouble = S.ForcedGamble >= 0 ? S.ForcedGamble != 0 : Roll < GambleDoubleChance;
	Events.bGambleDouble = bDouble;
	// The slot comes with OnAbilityUsed, right after (the same frame: both reach every machine together). A roll whose
	// ability never comes out leaves it unknown, and the overhead icon stays as it was.
	Events.GambleSlot = 255;
	Events.GambleAbility = static_cast<uint8>(Ability);
	++Events.GambleSerial;
	if (bDouble)
	{
		++Counters.Doubles;
	}
	else
	{
		++Counters.Fizzles;
	}
	if (const APenguinCharacter* Penguin = GetPenguin())
	{
		UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Gambler): %s %s (%d doubles, %d fizzles)"), *Penguin->GetDisplayName(),
			UPenguinAbilityComponent::GetInfo(Ability).Name, bDouble ? TEXT("doubled") : TEXT("fizzled"), Counters.Doubles, Counters.Fizzles);
	}
	OnLookStateChanged(); // (a listen server gets no notify of its own)
	if (AActor* OwnerActor = GetOwner())
	{
		OwnerActor->ForceNetUpdate();
	}
	return bDouble ? GambleDoubleStrength : 0.f;
}

void UPenguinTraitComponent::OnAbilityUsed(int32 Slot, EPenguinAbility Ability, float Strength)
{
	using namespace PenguinTraitEffectsPrivate;
	if (State.Trait == EPinguTrait::Gambler && IsServer() && Slot >= 0 && Slot < 3)
	{
		// Gambler: which slot the roll was for (its overhead icon crumbles to dust on a fizzle, shines on a double).
		Events.GambleSlot = static_cast<uint8>(Slot);
		Events.GambleAbility = static_cast<uint8>(Ability);
		if (AActor* OwnerActor = GetOwner())
		{
			OwnerActor->ForceNetUpdate();
		}
		return;
	}
	if (State.Trait != EPinguTrait::Jester || !IsServer() || Slot == INDEX_NONE)
	{
		return;
	}
	// Jester: a moment after the use the slot turns into another ability (its cooldown carries over). The delay lets the
	// use show (its use count, the card flash) with the ability it was.
	FPinguTraitEffectsState& S = GetOrMake(EffectsState);
	S.Swaps.RemoveAll([Slot](const FPinguTraitEffectsState::FPendingSwap& Swap) { return Swap.Slot == Slot; });
	FPinguTraitEffectsState::FPendingSwap Swap;
	Swap.Slot = Slot;
	Swap.Used = Ability;
	Swap.At = WorldNow(this) + JesterSwapDelay;
	S.Swaps.Add(Swap);
}

void UPenguinTraitComponent::SetForcedGamble(int32 Mode)
{
	using namespace PenguinTraitEffectsPrivate;
	if (IsServer())
	{
		GetOrMake(EffectsState).ForcedGamble = Mode < 0 ? -1 : (Mode == 0 ? 0 : 1);
	}
}

void UPenguinTraitComponent::OnHitLanded(APenguinCharacter* Victim)
{
	using namespace PenguinTraitEffectsPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	if (State.Trait != EPinguTrait::Leech || !IsServer() || !Penguin || !Victim || Victim == Penguin || !Penguin->IsAlive())
	{
		return;
	}
	// Leech: every hit it lands takes LeechCooldownCut off all its cooldowns.
	if (UPenguinAbilityComponent* Abilities = Penguin->GetAbilities())
	{
		Abilities->ReduceCooldowns(LeechCooldownCut);
	}
	++Events.LeechCuts; // (replicated: the joiners' anti-cheat judge counts the cuts it allows)
	Counters.LeechSeconds += LeechCooldownCut;
	FPinguFXBroadcastScope Broadcast(Penguin);
	APinguFX::Sparkles(Penguin, Penguin->GetAimOrigin(), 6, 45.f * Penguin->GetSizeScale(), LeechColor);
	APinguFX::Sparkles(Penguin, Victim->GetAimOrigin(), 4, 35.f * Victim->GetSizeScale(), LeechColor);
	UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Leech) hit %s: cooldowns -%.1f s (%.1f s so far)"), *Penguin->GetDisplayName(), *Victim->GetDisplayName(),
		LeechCooldownCut, Counters.LeechSeconds);
}

bool UPenguinTraitComponent::InterceptShot(APenguinProjectile* Shot, const FVector& Where)
{
	using namespace PenguinTraitEffectsPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	if (!HasAnyTrait() || !IsServer() || !Shot || !Penguin)
	{
		return false;
	}
	const EPenguinProjectileKind Kind = Shot->GetKind();
	switch (State.Trait)
	{
	case EPinguTrait::Mirror:
	{
		// The first shot every MirrorRecharge seconds goes back to its thrower, faster (never a fish hook).
		const double Now = FlowNow(this);
		if (Kind == EPenguinProjectileKind::FishHook || Now < Events.MirrorReadyAt)
		{
			return false;
		}
		const APenguinCharacter* Thrower = Shot->GetShooter();
		FVector Back = Thrower && Thrower != Penguin && Thrower->IsAlive() ? Thrower->GetAimOrigin() - Where : -Shot->GetVelocity();
		Back.Y = 0.f;
		const float Speed = FMath::Max(static_cast<float>(Shot->GetVelocity().Size()), 600.f) * MirrorSpeed;
		FPinguFXBroadcastScope Broadcast(Penguin);
		if (!Shot->Reflect(Penguin, Back, Speed))
		{
			return false;
		}
		Events.MirrorReadyAt = static_cast<float>(Now + MirrorRecharge);
		++Counters.Reflections;
		APinguFX::Ring(Penguin, Penguin->GetAimOrigin(), 70.f * Penguin->GetSizeScale(), MirrorColor, 0.25f);
		APinguFX::Sparkles(Penguin, Where, 8, 35.f, MirrorColor);
		APinguFX::Sound(Penguin, EPinguSound::ShieldBlock, 0.9f, 0.05f, 1.35f);
		UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Mirror) sent a shot back to %s (%d so far)"), *Penguin->GetDisplayName(),
			Thrower ? *Thrower->GetDisplayName() : TEXT("nobody"), Counters.Reflections);
		OnLookStateChanged();
		Penguin->ForceNetUpdate();
		return true;
	}
	case EPinguTrait::IceTouch:
	{
		// Shots that touch it freeze and drop (they fly past it for a moment). Ice bolts and rays are not bothered by frost.
		if (Shot->IsFrozen() || (Kind != EPenguinProjectileKind::Snowball && Kind != EPenguinProjectileKind::Icicle
			&& Kind != EPenguinProjectileKind::Bomb && Kind != EPenguinProjectileKind::FishHook))
		{
			return false;
		}
		FPinguFXBroadcastScope Broadcast(Penguin);
		Shot->Freeze(IceTouchShotFreeze); // (a fish hook lets go and reels back in)
		if (Kind != EPenguinProjectileKind::FishHook)
		{
			const FVector Velocity = Shot->GetVelocity();
			Shot->SetFlightVelocity(FVector(Velocity.X * IceTouchShotDropKeepX, 0.f, IceTouchShotDropZ));
			Shot->IgnorePenguin(Penguin, IceTouchShotIgnoreTime);
		}
		APinguFX::Sparkles(Penguin, Where, 8, 30.f, FrostColor);
		APinguFX::Sound(Penguin, EPinguSound::Freeze, 0.6f, 0.06f, 1.2f);
		UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Ice Touch) froze a shot that touched it"), *Penguin->GetDisplayName());
		return true;
	}
	case EPinguTrait::HotFeet:
	{
		// A frozen snowball or icicle melts away on it. (A frozen bomb thaws and goes off as ever.)
		if (!Shot->IsFrozen() || (Kind != EPenguinProjectileKind::Snowball && Kind != EPenguinProjectileKind::Icicle))
		{
			return false;
		}
		FPinguFXBroadcastScope Broadcast(Penguin);
		Shot->Shatter();
		Steam(Penguin, Where, 5, true);
		UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Hot Feet) melted a frozen shot"), *Penguin->GetDisplayName());
		return true;
	}
	default:
		return false;
	}
}

bool UPenguinTraitComponent::TrySecondChance()
{
	using namespace PenguinTraitEffectsPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	if (State.Trait != EPinguTrait::SecondChance || !IsServer() || !Move || !Penguin->IsAlive())
	{
		return false;
	}
	const double Now = FlowNow(this);
	if (Events.SecondChanceAt >= 0.f)
	{
		// Thrown already this round. For a moment after the throw the sea leaves it alone (a LAN owner's older positions
		// may still show it down there); after that the sea is the sea.
		const double Since = Now - static_cast<double>(Events.SecondChanceAt);
		if (Since < SecondChanceGrace)
		{
			return true;
		}
		const FPinguTraitEffectsState* S = EffectsState.Get();
		UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Second Chance): back in the sea %.2f s after the fish (no second rescue; its feet got up to z %.0f, landed since %d)"),
			*Penguin->GetDisplayName(), Since, S ? S->RescueHighestFeet : 0.f, S && S->bRescueLanded ? 1 : 0);
		return false;
	}

	// The fish throws it high over the island - its feet SecondChanceApexAbove over the highest block at the top of the throw -
	// toward the middle of what still stands of the island, at most SecondChanceMaxAngle off straight up, as fast sideways as
	// it takes to come down on the level ground nearest that middle (never a platform's edge). On the way up it passes through
	// the island's blocks (under an overhang, through a column) until its feet are over the highest one. Too far out for the
	// steepest angle allowed: thrown at that angle anyway (short of the island it comes down in the sea). Without an island:
	// straight up.
	const FVector Location = Penguin->GetActorLocation();
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Location);
	const float SeaLevel = Island ? Island->SeaLevel : -1650.f;
	const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const FVector Start(Location.X, 0.f, FMath::Max(static_cast<float>(Location.Z), SeaLevel + SecondChanceStartAboveSea));
	const float StartX = static_cast<float>(Start.X);
	const float StartZ = static_cast<float>(Start.Z);
	// The gravity of the way up (a jump's: BP_Penguin's JumpHeight over JumpTimeToApex) and of the way down (x the falling
	// multiplier, at most the top falling speed). The throw holds it like a hit until the top (the knockback's air drag
	// sideways, no steering); then the stick is its own - planned for it let go (the air braking).
	const float Gravity = 2.f * FMath::Max(Move->JumpHeight, 1.f) / FMath::Square(FMath::Max(Move->JumpTimeToApex, 0.05f));
	const float FallGravity = Gravity * FMath::Max(Move->GetEffectiveFallGravityMultiplier(), 0.1f);
	const float MaxFall = FMath::Max(Move->GetEffectiveMaxFallSpeed(), 100.f);
	const float RiseDrag = Move->KnockbackAirDrag;
	const float FallBraking = Move->AirBraking * Move->GetAirControlScale();

	FRescueTarget Target;
	// (Sudden death: a column that will still stand; none left - any column at all.)
	const bool bAimed = Island && (FindRescueTarget(Island, true, Target) || FindRescueTarget(Island, false, Target));
	const float HighestTop = bAimed ? Target.HighestTopZ : StartZ;
	const float ApexMid = HighestTop + SecondChanceApexAbove + HalfHeight;
	const float Up = FMath::Sqrt(2.f * Gravity * FMath::Max(ApexMid - StartZ, 100.f));
	const float RiseTime = Up / Gravity;
	float SideSpeed = 0.f;
	float Side = 0.f;
	float Reach = 0.f;
	float MaxReach = 0.f;
	bool bTooFar = false;
	if (bAimed)
	{
		const float Want = FMath::Abs(Target.LandX - StartX);
		Side = Target.LandX >= StartX ? 1.f : -1.f;
		const float FallTime = FallTimeFor(ApexMid - (Target.LandTopZ + HalfHeight), FallGravity, MaxFall);
		const float MaxSide = Up * FMath::Tan(FMath::DegreesToRadians(SecondChanceMaxAngle));
		MaxReach = RescueReach(MaxSide, RiseTime, RiseDrag, FallTime, FallBraking);
		if (MaxReach <= Want)
		{
			SideSpeed = MaxSide; // as far as it goes
		}
		else
		{
			// The speed that comes down right on the middle (the reach grows with the speed: halve the range).
			float Low = 0.f;
			float High = MaxSide;
			for (int32 Step = 0; Step < 32; ++Step)
			{
				const float Mid = 0.5f * (Low + High);
				if (RescueReach(Mid, RiseTime, RiseDrag, FallTime, FallBraking) < Want)
				{
					Low = Mid;
				}
				else
				{
					High = Mid;
				}
			}
			SideSpeed = 0.5f * (Low + High);
		}
		Reach = RescueReach(SideSpeed, RiseTime, RiseDrag, FallTime, FallBraking);
		// Short of the island's nearest ground (out past one of its ends): it comes down in the sea.
		const bool bOver = StartX >= Target.MinX && StartX <= Target.MaxX;
		const float NearX = Side > 0.f ? Target.MinX : Target.MaxX;
		bTooFar = !bOver && Reach < FMath::Abs(NearX - StartX) - 0.5f * Island->CellSize;
	}
	const FVector Throw(Side * SideSpeed, 0.f, Up);
	Events.SecondChanceAt = static_cast<float>(Now);
	++Counters.SecondChances;
	Counters.RescueStart = Start;
	Counters.RescueThrow = Throw;
	Counters.RescueMidX = bAimed ? Target.MidX : StartX;
	Counters.RescueLandX = bAimed ? Target.LandX : StartX;
	Counters.RescueHighestTop = HighestTop;
	Counters.bRescueAimed = bAimed;
	Counters.bRescueTooFar = bTooFar;
	// The ice of a frozen penguin goes; the throw (a stomp, a fast fall or a slide end with it) holds it until the top; it
	// starts just above the water, through the island's blocks until its feet are over the highest one. Each reaches a LAN
	// player's own machine in this order (which moves its penguin); the position guard covers that machine's older moves.
	if (Move->IsFrozen())
	{
		Move->SetFrozen(0.f);
	}
	Move->ApplyKnockback(FVector2D(Throw.X, Throw.Z), RiseTime);
	Penguin->NetTeleport(Start, Throw, true);
	Move->RefreshAirMoves();
	if (bAimed)
	{
		Move->SetPassThroughIsland(HighestTop + SecondChancePassClear, RiseTime + SecondChancePassSpare);
		// It comes down with a thud where it lands (the sideways speed it still has would slide it off a platform: the hook
		// throw's landing, StickNextLanding - a hit before then cancels it).
		Move->StickNextLanding();
	}
	if (Side != 0.f)
	{
		Penguin->FaceDirection(Side); // looking at the island (a knock looks back the way it came)
	}
	FPinguTraitEffectsState& S = GetOrMake(EffectsState);
	S.RescueHighestFeet = -1.e9f;
	S.bRescueLanded = false;
	{
		FPinguFXBroadcastScope Broadcast(Penguin);
		const FVector Surface(Start.X, 0.f, SeaLevel + 40.f);
		APinguFX::Splash(Penguin, Surface);
		APinguFX::Puffs(Penguin, Surface, 8, 30.f, 420.f, FVector::UpVector, 0.5f, FLinearColor(0.85f, 0.95f, 1.f));
		APinguFX::Sparkles(Penguin, Start, 10, 60.f, FishColor);
		APinguFX::Sound(Penguin, EPinguSound::Splash);
		APinguFX::Sound(Penguin, EPinguSound::Jump, 0.8f, 0.05f, 0.8f);
	}
	UE_LOG(LogPingu, Display, TEXT("[TRAITS] %s (Second Chance): a fish throws it out of the sea at x %.0f: %.0f cm/s up, %.0f sideways (%.1f deg off straight up; aiming at x %.0f, the level ground nearest the middle x %.0f of the island x %.0f to %.0f, top %.0f; over its highest block %.0f; reach %.0f cm, %.0f at %.0f deg%s)"),
		*Penguin->GetDisplayName(), Start.X, Throw.Z, Throw.X, FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(Throw.X), Throw.Z)), Target.LandX, Target.MidX,
		Target.MinX, Target.MaxX, Target.LandTopZ, HighestTop, Reach, MaxReach, SecondChanceMaxAngle,
		!bAimed ? TEXT(", no island to aim for") : (bTooFar ? TEXT(": too far out, it falls short into the sea") : TEXT("")));
	OnLookStateChanged();
	Penguin->ForceNetUpdate();
	return true;
}

void UPenguinTraitComponent::OnTransformed()
{
	// The trait's resting size (Tiny, Giant): size rays stack on it and wear off back to it.
	if (APenguinCharacter* Penguin = GetPenguin())
	{
		Penguin->SetBaseSize(GetNumbers().BaseSize);
		// Ice Rink: the island's snow turns to ice (the first penguin's transform does it; the rest find it frozen). The grip is
		// every penguin's own (bAllFloorsIce: rock slides too). The island is new every round.
		if (State.Trait == EPinguTrait::IceRink)
		{
			if (ABlockIsland* Island = ABlockIsland::Get(Penguin->GetWorld()))
			{
				const FBox Bounds = Island->GetBounds();
				const float Radius = static_cast<float>((Bounds.Max - Bounds.Min).Size()) + Island->CellSize;
				const int32 Frozen = Island->FreezeBlocksInRadius(Bounds.GetCenter(), Radius);
				Counters.RinkCells += Frozen;
				if (Frozen > 0)
				{
					FPinguFXBroadcastScope Broadcast(Penguin);
					APinguFX::Sound(Penguin, EPinguSound::Freeze, 0.6f, 0.05f, 0.8f);
					UE_LOG(LogPingu, Display, TEXT("[TRAITS] Ice Rink: %d snow blocks of the island turned to ice"), Frozen);
				}
			}
		}
	}
}
