// Pingu - the snowman (see PinguSnowman.h).

#include "PinguSnowman.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PinguBuildLimit.h"
#include "PinguFX.h"
#include "PinguGiantSnowball.h"
#include "PinguSettings.h"
#include "PinguTornado.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"

FString APinguSnowman::LastEnd;

namespace PinguSnowmanPrivate
{
	using namespace PinguTuning;

	/** The snow: the Giant Snowball's (the island's M_Block_Snow); coal and sticks on the penguin's clay; the scarf's stripes. */
	static const FLinearColor SnowColor(0.86f, 0.92f, 0.98f);
	static const FLinearColor CoalColor(0.018f, 0.018f, 0.022f);
	static const FLinearColor CarrotColor(1.f, 0.45f, 0.08f);
	static const FLinearColor StickColor(0.15f, 0.08f, 0.035f);
	static const FLinearColor StripeColor(0.75f, 0.79f, 0.84f);
	static const FLinearColor PuffColor(0.95f, 0.97f, 1.f);
	/** Its three balls (size 1, cm; Linus 2026-11-04: "make the spheres be inside eachother so it looks like it has some real
	 *  wheight ... overall height a bit shorter"): radius across, squashed this much up and down, how high their middles are over
	 *  its feet. Each sinks about a third of its height into the one below (16 / 13 cm); the bottom one sits flat on the ground
	 *  and spreads a little there. 122 cm to the head's top (was 148), 136 with the beanie (was 165). */
	constexpr float BallRadius[3] = { 38.f, 27.f, 20.5f };
	constexpr float BallSquash[3] = { 0.84f, 0.93f, 0.96f };
	constexpr float BallHeight[3] = { 29.f, 70.f, 102.f };
	/** Where each ball's collar runs (the packed snow pressed into the seam with the ball below: its latitude, -1 bottom .. 1 top). */
	constexpr float CollarAt[3] = { 0.f, -0.6f, -0.68f };
	/** Its parts' materials. */
	enum EMaterialSlot : int32 { MatSnow, MatCoal, MatStick, MatCarrot, MatFelt, MatWool, MatStripe, MatPom, MatCount };
	/** The build: each ball pops up over this part of SnowmanBuildTime (0 .. 1 of it). */
	constexpr float BallFrom[3] = { 0.f, 0.25f, 0.5f };
	constexpr float BallTo[3] = { 0.55f, 0.75f, 1.f };
	/** The throw's swing of its arm (s) and the head's turn toward its target. */
	constexpr float SwingTime = 0.32f;
	constexpr float HeadTurnDeg = 35.f;
	/** A snowball it throws flies this fast (the snowball's own speed). */
	constexpr float ThrowSpeed = SnowballSpeed;
	/** It falls as the island's falling blocks (ABlockIsland: FallingBlockGravity / FallingBlockMaxSpeed). */
	constexpr float FallGravity = 7500.f;
	constexpr float FallMaxSpeed = 3000.f;
	/** A penguin is touching it when its capsule reaches this far into its body (cm). */
	constexpr float TouchDepth = 4.f;

	static double ServerNow(const UWorld* World)
	{
		if (!World)
		{
			return 0.0;
		}
		if (World->GetNetMode() == NM_Client)
		{
			if (const AGameStateBase* GameState = World->GetGameState())
			{
				return GameState->GetServerWorldTimeSeconds();
			}
		}
		return World->GetTimeSeconds();
	}

	/** An ease-out with a little overshoot (a ball popping up). */
	static float Pop(float T)
	{
		const float A = FMath::Clamp(T, 0.f, 1.f);
		const float C = 1.70158f;
		return 1.f + (C + 1.f) * FMath::Pow(A - 1.f, 3.f) + C * FMath::Pow(A - 1.f, 2.f);
	}

	static float DistanceToCapsule(const APenguinCharacter* Penguin, const FVector& A, const FVector& B)
	{
		// Closest distance between the snowman's body line (A..B) and the penguin's capsule line, on the play plane.
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 30.f;
		const float Half = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 56.f;
		const FVector P = Penguin->GetActorLocation();
		const FVector C(P.X, 0.f, P.Z - (Half - Radius));
		const FVector D(P.X, 0.f, P.Z + (Half - Radius));
		FVector X1, X2;
		FMath::SegmentDistToSegmentSafe(FVector(A.X, 0.f, A.Z), FVector(B.X, 0.f, B.Z), C, D, X1, X2);
		return static_cast<float>(FVector::Dist(X1, X2)) - Radius;
	}
}

APinguSnowman::APinguSnowman()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	SetActorEnableCollision(false);
	// LAN: the host's; clients draw it from its replicated state (its place only changes in a fall).
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(10.f);
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = SceneRoot;
}

void APinguSnowman::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APinguSnowman, Builder);
	DOREPLIFETIME(APinguSnowman, SizeScale);
	DOREPLIFETIME(APinguSnowman, BuildStart);
	DOREPLIFETIME(APinguSnowman, ThrowSerial);
	DOREPLIFETIME(APinguSnowman, ThrowSide);
	DOREPLIFETIME(APinguSnowman, Facing);
	DOREPLIFETIME(APinguSnowman, LookSeed);
}

// ---------------------------------------------------------------------------- building, finding

bool APinguSnowman::FindSpot(const APenguinCharacter* InBuilder, float Side, FVector& OutFeet, FVector* OutRefusedAt, FString* OutWhy)
{
	using namespace PinguSnowmanPrivate;
	const UWorld* World = InBuilder ? InBuilder->GetWorld() : nullptr;
	const ABlockIsland* Island = World ? ABlockIsland::FindNear(World, InBuilder->GetActorLocation()) : nullptr;
	const auto Refuse = [OutRefusedAt, OutWhy](const FVector& At, const TCHAR* Why)
	{
		if (OutRefusedAt)
		{
			*OutRefusedAt = At;
		}
		if (OutWhy)
		{
			*OutWhy = Why;
		}
		return false;
	};
	if (!Island)
	{
		return Refuse(InBuilder ? InBuilder->GetActorLocation() : FVector::ZeroVector, TEXT("no island"));
	}
	const FVector Feet = InBuilder->GetFeetLocation();
	const float Dir = Side >= 0.f ? 1.f : -1.f;
	const int32 Step = Dir > 0.f ? 1 : -1;
	const float Cell = Island->CellSize;
	const FIntPoint FeetCell = Island->WorldToCell(FVector(Feet.X, 0.f, Feet.Z + 40.f));
	const FVector InFront(Feet.X + Dir * Cell, 0.f, Feet.Z + 40.f);
	// Only from the ground (2026-11-05, Linus: "nothing happens in the air").
	// (A LAN joiner's penguin on the host: its replicated place says it, whatever mode the host's copy is in.)
	const UPenguinMovementComponent* Move = InBuilder->GetPenguinMovement();
	float GroundUnder = 0.f;
	const bool bOnGround = (Move && Move->IsMovingOnGround())
		|| (Island->FindGroundBelow(Feet + FVector(0.f, 0.f, 5.f), 40.f, GroundUnder) && Feet.Z - GroundUnder < 30.f);
	if (!bOnGround)
	{
		return Refuse(InFront, TEXT("in the air"));
	}
	// A tile of a column: its top block with room for the body above it, from one block up (a step) to SnowmanMaxDropBlocks down.
	// Island blocks and players' ice alike: everything on the island's grid is a tile.
	const auto TileIn = [Island, &FeetCell](int32 Column, FIntPoint& OutTile)
	{
		for (int32 Row = FeetCell.Y; Row >= FeetCell.Y - 1 - SnowmanMaxDropBlocks; --Row)
		{
			if (!Island->IsSolidCell(FIntPoint(Column, Row)))
			{
				continue;
			}
			// Room for its body (SnowmanHeight: the two cells above).
			const int32 RoomCells = FMath::CeilToInt(SnowmanHeight / Island->CellSize);
			for (int32 Up = 1; Up <= RoomCells; ++Up)
			{
				if (Island->IsSolidCell(FIntPoint(Column, Row + Up)))
				{
					return false;
				}
			}
			OutTile = FIntPoint(Column, Row);
			return true;
		}
		return false;
	};
	const auto FeetOn = [Island, Cell](const FIntPoint& OnTile)
	{
		const FVector Middle = Island->CellToWorld(OnTile);
		return FVector(Middle.X, 0.f, Middle.Z + Cell * 0.5f);
	};
	// The tile in front of it; a wall or a gap there: its own tile.
	FIntPoint Chosen;
	if (!TileIn(FeetCell.X + Step, Chosen) && !TileIn(FeetCell.X, Chosen))
	{
		return Refuse(InFront, TEXT("no tile"));
	}
	// Taken: the next tile on (in its facing), never further.
	if (FindOnTile(World, Island, Chosen))
	{
		const FVector TakenFeet = FeetOn(Chosen);
		FIntPoint Next;
		if (!TileIn(Chosen.X + Step, Next))
		{
			return Refuse(TakenFeet + FVector(Dir * Cell, 0.f, 40.f), TEXT("the tile is taken, no tile next to it"));
		}
		if (FindOnTile(World, Island, Next))
		{
			return Refuse(FeetOn(Next) + FVector(0.f, 0.f, 40.f), TEXT("the tile and the next one are taken"));
		}
		Chosen = Next;
	}
	const FVector Spot = FeetOn(Chosen);
	// The build limit (everything standing in the world: PinguBuildLimit).
	const float Half = SnowmanBodyRadius;
	if (!PinguBuildLimit::AllowsSpan(Island, static_cast<float>(Spot.X) - Half, static_cast<float>(Spot.X) + Half, static_cast<float>(Spot.Z) + SnowmanHeight))
	{
		return Refuse(Spot + FVector(0.f, 0.f, 60.f), TEXT("past the build limit"));
	}
	OutFeet = Spot;
	return true;
}

APinguSnowman* APinguSnowman::FindOnTile(const UWorld* World, const ABlockIsland* Island, const FIntPoint& OnTile)
{
	TArray<APinguSnowman*> All;
	GetAll(World, All);
	for (APinguSnowman* Snowman : All)
	{
		if (Snowman->bHasTile && Snowman->Tile == OnTile && ABlockIsland::FindNear(World, Snowman->GetFeet()) == Island)
		{
			return Snowman;
		}
	}
	return nullptr;
}

void APinguSnowman::NoteShotHit(const UWorld* World, const APenguinProjectile* Shot)
{
	using namespace PinguSnowmanPrivate;
	TArray<APinguSnowman*> All;
	GetAll(World, All);
	for (APinguSnowman* Snowman : All)
	{
		if (!Snowman->OwnThrows.Contains(Shot))
		{
			continue;
		}
		++Snowman->Hits;
		// Its next throw SnowmanHitReload s after the last one (not later than it would come anyway).
		const double Since = World->GetTimeSeconds() - Snowman->LastThrowTime;
		const float Sooner = FMath::Max(0.f, SnowmanHitReload - static_cast<float>(Since));
		if (Sooner < Snowman->ThrowTimer)
		{
			Snowman->ThrowTimer = Sooner;
			Snowman->bQuickNext = true;
		}
		UE_LOG(LogPingu, Log, TEXT("[SNOWMAN] %s's snowman hit: its next throw in %.2f s"), Snowman->Builder.Get() ? *Snowman->Builder->GetDisplayName() : TEXT("?"), Snowman->ThrowTimer);
		return;
	}
}

APinguSnowman* APinguSnowman::Build(APenguinCharacter* InBuilder, const FVector& Feet, float Side)
{
	using namespace PinguSnowmanPrivate;
	UWorld* World = InBuilder ? InBuilder->GetWorld() : nullptr;
	if (!World || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	// (As many as its builder likes since 2026-11-05: one per tile, FindSpot.)
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APinguSnowman* Snowman = World->SpawnActor<APinguSnowman>(Feet, FRotator::ZeroRotator, Params);
	if (!Snowman)
	{
		return nullptr;
	}
	Snowman->Builder = InBuilder;
	Snowman->BuildStart = ServerNow(World);
	Snowman->Facing = Side >= 0.f ? 1.f : -1.f;
	Snowman->ThrowSide = Snowman->Facing;
	Snowman->LookSeed = FMath::RandRange(1, 1 << 30); // (its own lumps, the same on every machine)
	Snowman->ThrowTimer = SnowmanBuildTime + SnowmanFirstThrow;
	if (const ABlockIsland* Island = ABlockIsland::FindNear(World, Feet))
	{
		// The tile it stands on (the block under its feet): gone, it breaks.
		Snowman->Tile = Island->WorldToCell(Feet - FVector(0.f, 0.f, Island->CellSize * 0.5f));
		Snowman->bHasTile = true;
	}
	{
		FPinguFXBroadcastScope Broadcast(InBuilder);
		APinguFX::Sound(InBuilder, EPinguSound::SnowmanBuild);
		APinguFX::GroundPuffs(InBuilder, Feet, 1.f);
		APinguFX::Puffs(InBuilder, Feet + FVector(0.f, 0.f, 60.f), 8, 22.f, 220.f, FVector::UpVector, 1.f, PuffColor);
		APinguFX::Sparkles(InBuilder, Feet + FVector(0.f, 0.f, 90.f), 6, 50.f, FLinearColor(0.8f, 0.92f, 1.f));
	}
	InBuilder->PlayAction(InBuilder->PickNearFlipper(EPenguinClip::Swat, EPenguinClip::SwatL), 1.6f);
	UE_LOG(LogPingu, Display, TEXT("[SNOWMAN] %s builds a snowman at %s"), *InBuilder->GetDisplayName(), *Feet.ToCompactString());
	return Snowman;
}

void APinguSnowman::GetAll(const UWorld* World, TArray<APinguSnowman*>& Out)
{
	Out.Reset();
	if (!World)
	{
		return;
	}
	for (TActorIterator<APinguSnowman> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed() && !It->bEnded)
		{
			Out.Add(*It);
		}
	}
}

APinguSnowman* APinguSnowman::FindOf(const UWorld* World, const APenguinCharacter* InBuilder)
{
	TArray<APinguSnowman*> All;
	GetAll(World, All);
	for (APinguSnowman* Snowman : All)
	{
		if (InBuilder && Snowman->GetBuilder() == InBuilder)
		{
			return Snowman;
		}
	}
	return nullptr;
}

void APinguSnowman::DestroyAll(UWorld* World)
{
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	for (TActorIterator<APinguSnowman> It(World); It; ++It)
	{
		It->Destroy();
	}
}

int32 APinguSnowman::BreakInArea(UWorld* World, const FVector& Center, float Radius, APenguinCharacter* By, const TCHAR* What,
	const FVector& ConeDirection, float CosHalfAngle)
{
	if (!World || World->GetNetMode() == NM_Client)
	{
		return 0;
	}
	TArray<APinguSnowman*> All;
	GetAll(World, All);
	int32 Count = 0;
	for (APinguSnowman* Snowman : All)
	{
		if (Snowman->DistanceToBody(Center) > Radius)
		{
			continue;
		}
		if (CosHalfAngle > -1.f && !ConeDirection.IsNearlyZero())
		{
			// In the cone: its middle (or its head) inside it, or the center right at it.
			const FVector Mid = Snowman->GetFeet() + FVector(0.f, 0.f, PinguSnowmanPrivate::BallHeight[1] * Snowman->GetSize());
			FVector To = Mid - Center;
			To.Y = 0.f;
			const bool bAt = To.Size() < PinguTuning::SnowmanBodyRadius * Snowman->GetSize() * 1.5f;
			if (!bAt && FVector::DotProduct(To.GetSafeNormal(), ConeDirection.GetSafeNormal()) < CosHalfAngle)
			{
				continue;
			}
		}
		Snowman->Break(By, What);
		++Count;
	}
	return Count;
}

float APinguSnowman::GetAge() const
{
	return static_cast<float>(PinguSnowmanPrivate::ServerNow(GetWorld()) - BuildStart);
}

bool APinguSnowman::IsStanding() const
{
	return GetAge() >= PinguTuning::SnowmanBuildTime;
}

FVector APinguSnowman::GetThrowPoint() const
{
	using namespace PinguSnowmanPrivate;
	return GetFeet() + FVector(ThrowSide * (BallRadius[1] + 18.f), 0.f, BallHeight[2]) * SizeScale;
}

float APinguSnowman::DistanceToBody(const FVector& Point) const
{
	using namespace PinguSnowmanPrivate;
	// A line from its bottom ball's middle to its head's, as thick as its bottom ball (a bit generous at the head: easy to hit).
	const FVector A = GetFeet() + FVector(0.f, 0.f, BallHeight[0] * SizeScale);
	const FVector B = GetFeet() + FVector(0.f, 0.f, BallHeight[2] * SizeScale);
	const FVector P(Point.X, 0.f, Point.Z);
	const FVector Closest = FMath::ClosestPointOnSegment(P, FVector(A.X, 0.f, A.Z), FVector(B.X, 0.f, B.Z));
	return static_cast<float>(FVector::Dist(P, Closest)) - PinguTuning::SnowmanBodyRadius * SizeScale;
}

int32 APinguSnowman::GetShownParts() const
{
	int32 Count = 0;
	for (const UProceduralMeshComponent* Part : { Balls.IsValidIndex(0) ? Balls[0].Get() : nullptr, Balls.IsValidIndex(1) ? Balls[1].Get() : nullptr,
		Balls.IsValidIndex(2) ? Balls[2].Get() : nullptr, Face.Get(), Buttons.Get(), Arms.IsValidIndex(0) ? Arms[0].Get() : nullptr,
		Arms.IsValidIndex(1) ? Arms[1].Get() : nullptr, Hat.Get(), Scarf.Get() })
	{
		Count += Part && Part->IsVisible() && Part->GetNumSections() > 0 ? 1 : 0;
	}
	return Count;
}

// ---------------------------------------------------------------------------- the tick

void APinguSnowman::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(DeltaSeconds, 0.05f);
	if (HasAuthority() && !bEnded)
	{
		TickServer(Dt);
		if (bEnded || !IsValid(this) || IsActorBeingDestroyed())
		{
			return;
		}
	}
	TickLook(Dt);
}

void APinguSnowman::TickServer(float Dt)
{
	using namespace PinguSnowmanPrivate;
	APenguinCharacter* Owner_ = Builder.Get();
	if (!Owner_)
	{
		Break(nullptr, TEXT("its builder left"));
		return;
	}
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), GetActorLocation());
	TickFall(Dt, Island);
	if (bEnded)
	{
		return;
	}
	TouchShots();
	if (bEnded)
	{
		return;
	}
	TouchThings();
	if (bEnded)
	{
		return;
	}
	// Throws, once built (and not while falling).
	ThrowTimer -= Dt;
	if (ThrowTimer <= 0.f && IsStanding() && FallSpeed <= 0.f)
	{
		TryThrow();
	}
}

void APinguSnowman::TickFall(float Dt, const ABlockIsland* Island)
{
	using namespace PinguSnowmanPrivate;
	static_cast<void>(Dt);
	if (!Island)
	{
		return;
	}
	const FVector Feet = GetActorLocation();
	// A block where it stands (an ice wall raised under it, falling ice landed in it): crushed.
	if (Island->IsSolidAt(Feet + FVector(0.f, 0.f, BallHeight[0] * SizeScale)))
	{
		Break(nullptr, TEXT("a block came where it stands"));
		return;
	}
	// Its tile gone (broken, melted, fallen away, sunk): it breaks (2026-11-05; it used to fall on like a block).
	const FIntPoint Under = bHasTile ? Tile : Island->WorldToCell(Feet - FVector(0.f, 0.f, Island->CellSize * 0.5f));
	if (!Island->IsSolidCell(Under))
	{
		Break(nullptr, TEXT("its tile went"));
		return;
	}
	FallSpeed = 0.f;
}

void APinguSnowman::TouchShots()
{
	using namespace PinguSnowmanPrivate;
	TArray<APenguinProjectile*> Shots;
	APenguinProjectile::GetAll(GetWorld(), Shots);
	for (APenguinProjectile* Shot : Shots)
	{
		if (!IsValid(Shot) || Shot->IsActorBeingDestroyed() || OwnThrows.Contains(Shot))
		{
			continue;
		}
		// Everything its builder shoots flies through it (2026-11-05, Linus: "cant hit ur own snow mans with projectiles"): snowballs of
		// every size, icicle, ice bolt, hook, rays - and the balls of the builder's other snowmen (they count as the builder's), so
		// the snowmen of one penguin never break each other. A bomb's body passes too; its blast still breaks it (BreakInArea).
		// A teammate's shot is not the builder's: it breaks it like an enemy's.
		if (Builder && Shot->GetShooter() == Builder.Get())
		{
			continue;
		}
		const EPenguinProjectileKind Kind = Shot->GetKind();
		// Flying (or lying) shots and a hook on its way out; pegs and pads stay where they are.
		const bool bLoose = Kind == EPenguinProjectileKind::FishHook ? Shot->IsThreat() : Shot->CanBeSucked();
		if (!bLoose || DistanceToBody(Shot->GetActorLocation()) > Shot->GetRadius())
		{
			continue;
		}
		APenguinCharacter* By = Shot->GetShooter();
		FPinguFXBroadcastScope Broadcast(this);
		switch (Kind)
		{
		case EPenguinProjectileKind::ShrinkRay:
		case EPenguinProjectileKind::GrowRay:
			Shot->Shatter();
			Resize(Kind == EPenguinProjectileKind::GrowRay ? PinguGrowRayFactor : PinguShrinkRayFactor, By);
			break;
		case EPenguinProjectileKind::Bomb:
			Shot->Detonate(0.f); // (its blast breaks it: APenguinProjectile::Explode -> BreakInArea)
			Break(By, TEXT("snow bomb"));
			break;
		default:
			Shot->Shatter(); // (a hook lets go and reels in)
			Break(By, Kind == EPenguinProjectileKind::Snowball ? TEXT("snowball") : Kind == EPenguinProjectileKind::Icicle ? TEXT("icicle")
				: Kind == EPenguinProjectileKind::IceBolt ? TEXT("ice bolt") : TEXT("fish hook"));
			break;
		}
		if (bEnded)
		{
			return;
		}
	}
}

void APinguSnowman::TouchThings()
{
	using namespace PinguSnowmanPrivate;
	UWorld* World = GetWorld();
	const FVector A = GetFeet() + FVector(0.f, 0.f, BallHeight[0] * SizeScale);
	const FVector B = GetFeet() + FVector(0.f, 0.f, BallHeight[2] * SizeScale);
	const float Body = SnowmanBodyRadius * SizeScale;
	// Penguins: a slide, a stomp or hot feet breaks it (anybody else walks past it).
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		if (!Penguin->IsAlive() || Penguin->IsHidden() || DistanceToCapsule(Penguin, A, B) > Body - TouchDepth)
		{
			continue;
		}
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const UPenguinTraitComponent* Traits = Penguin->GetTraits();
		if (Move && Move->IsBellySliding())
		{
			Break(Penguin, TEXT("belly slide"));
			return;
		}
		if (Move && Move->IsStomping())
		{
			Break(Penguin, TEXT("belly stomp"));
			return;
		}
		if (Traits && Traits->HasTrait(EPinguTrait::HotFeet))
		{
			Break(Penguin, TEXT("hot feet"));
			return;
		}
	}
	// A Giant Snowball rolling into it.
	TArray<APinguGiantSnowball*> Giants;
	APinguGiantSnowball::GetAll(World, Giants);
	for (const APinguGiantSnowball* Giant : Giants)
	{
		if (Giant->IsRolling() && DistanceToBody(Giant->GetCenter()) < Giant->GetRadius())
		{
			Break(Giant->GetRoller(), TEXT("giant snowball"));
			return;
		}
	}
	// A tornado reaching it rips it apart.
	TArray<APinguTornado*> Tornadoes;
	APinguTornado::GetAll(World, Tornadoes);
	for (const APinguTornado* Tornado : Tornadoes)
	{
		if (!Tornado->HasThrown() && DistanceToBody(Tornado->GetCenter()) < Tornado->GetRadius())
		{
			Break(Tornado->GetUser(), TEXT("tornado"));
			return;
		}
	}
	// Falling icebergs and falling ice crush it.
	for (TActorIterator<AIcebergDrop> It(World); It; ++It)
	{
		if (It->IsFalling() && It->GetVelocity().Z < -50.f && It->Overlaps((A + B) * 0.5f, Body + (B.Z - A.Z) * 0.5f))
		{
			Break(It->GetDropOwner(), TEXT("iceberg"));
			return;
		}
	}
	if (const ABlockIsland* Island = ABlockIsland::FindNear(World, A))
	{
		float Bottom = 0.f;
		if (Island->GetFallingSandBottom(static_cast<float>(A.X) - Body, static_cast<float>(A.X) + Body, Bottom) && Bottom < B.Z + BallRadius[2] * SizeScale
			&& Bottom > GetFeet().Z)
		{
			Break(nullptr, TEXT("falling ice"));
			return;
		}
	}
}

APenguinCharacter* APinguSnowman::FindTarget() const
{
	using namespace PinguSnowmanPrivate;
	const APenguinCharacter* Owner_ = Builder.Get();
	const FVector From = GetFeet() + FVector(0.f, 0.f, BallHeight[2] * SizeScale);
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), From);
	APenguinCharacter* Best = nullptr;
	float BestDistance = SnowmanRange;
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		// Never its builder or the builder's teammates.
		if (Penguin == Owner_ || !Penguin->IsAlive() || Penguin->IsHidden() || (Owner_ && Owner_->IsTeammate(Penguin)))
		{
			continue;
		}
		const float Distance = static_cast<float>(FVector::Dist(From, Penguin->GetAimOrigin()));
		if (Distance > BestDistance || (Island && !Island->IsLineClear(From, Penguin->GetAimOrigin())))
		{
			continue;
		}
		Best = Penguin;
		BestDistance = Distance;
	}
	return Best;
}

void APinguSnowman::TryThrow()
{
	using namespace PinguSnowmanPrivate;
	APenguinCharacter* Target = FindTarget();
	if (!Target)
	{
		ThrowTimer = 0.15f; // (look again soon)
		return;
	}
	ThrowTimer = SnowmanReload;
	if (bQuickNext)
	{
		++QuickThrows; // (this one came sooner: the last one hit)
		bQuickNext = false;
	}
	LastThrowTime = GetWorld()->GetTimeSeconds();
	const float Side = Target->GetActorLocation().X >= GetFeet().X ? 1.f : -1.f;
	ThrowSide = Side;
	const FVector From = GetThrowPoint();
	// Led for the ball's flight (its sideways speed, a share of its up / down), lifted against the ball's gravity.
	const FVector Aim = Target->GetAimOrigin();
	const float Time = FMath::Clamp(static_cast<float>(FVector::Dist(From, Aim)) / ThrowSpeed, 0.05f, 1.f);
	const FVector V = Target->GetVelocity();
	const FVector Led = Aim + FVector(V.X * Time, 0.f, V.Z * Time * 0.4f);
	APenguinProjectile* Shot = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Builder.Get(), From, (Led - From).GetSafeNormal() * ThrowSpeed);
	if (!Shot)
	{
		return;
	}
	if (!FMath::IsNearlyEqual(SizeScale, Shot->GetSizeScale(), 0.01f) && Shot->GetSizeScale() > 0.f)
	{
		Shot->Rescale(SizeScale / Shot->GetSizeScale()); // (its size, not its builder's)
	}
	Shot->SetNoRefund(true);
	FVector Velocity = (Led - From) / Time;
	Velocity.Z += 0.5f * Shot->GetGravity() * Time;
	Velocity.Y = 0.f;
	Shot->SetFlightVelocity(Velocity);
	if (Builder.Get())
	{
		Shot->IgnorePenguin(Builder.Get(), 0.3f); // (it never hits its builder standing next to it)
	}
	OwnThrows.RemoveAll([](const TWeakObjectPtr<APenguinProjectile>& Each) { return !Each.IsValid(); });
	OwnThrows.Add(Shot);
	++ThrowSerial;
	++Throws;
	LastTarget = Target;
	FPinguFXBroadcastScope Broadcast(this);
	APinguFX::Sound(this, EPinguSound::SnowmanThrow);
	UE_LOG(LogPingu, Log, TEXT("[SNOWMAN] %s's snowman throws at %s"), Builder.Get() ? *Builder->GetDisplayName() : TEXT("?"), *Target->GetDisplayName());
}

void APinguSnowman::Resize(float Factor, APenguinCharacter* By)
{
	using namespace PinguSnowmanPrivate;
	if (!HasAuthority() || bEnded)
	{
		return;
	}
	const float Wanted = SizeScale * Factor;
	const bool bGrow = Factor > 1.f;
	{
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, bGrow ? EPinguSound::Grow : EPinguSound::Shrink);
		APinguFX::Sparkles(this, GetFeet() + FVector(0.f, 0.f, 70.f * SizeScale), 10, 50.f * SizeScale,
			bGrow ? FLinearColor(0.45f, 1.f, 0.35f) : FLinearColor(0.85f, 0.4f, 1.f));
	}
	if (Wanted < SnowmanMinSize * 0.99f)
	{
		Break(By, TEXT("shrunk away"));
		return;
	}
	SizeScale = FMath::Min(Wanted, SnowmanMaxSize);
}

void APinguSnowman::Break(APenguinCharacter* By, const TCHAR* Why)
{
	using namespace PinguSnowmanPrivate;
	if (!HasAuthority() || bEnded)
	{
		return;
	}
	const FVector Mid = GetFeet() + FVector(0.f, 0.f, 70.f * SizeScale);
	{
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::SnowmanCrumble);
		APinguFX::Puffs(this, Mid, 12, 26.f * SizeScale, 360.f, FVector::UpVector, 1.f, PuffColor);
		APinguFX::Flakes(this, Mid, 10, 420.f, PuffColor);
		APinguFX::Shards(this, Mid + FVector(0.f, 30.f, 40.f * SizeScale), 3, 300.f, CarrotColor); // (its carrot flies off)
		if (const APenguinCharacter* Who = Builder.Get())
		{
			APinguFX::Shards(this, Mid + FVector(0.f, 30.f, 22.f * SizeScale), 3, 260.f, Who->GetPlayerColor()); // (and bits of its scarf)
		}
		APinguFX::GroundPuffs(this, GetFeet(), 1.2f * SizeScale);
	}
	UE_LOG(LogPingu, Display, TEXT("[SNOWMAN] %s's snowman breaks (%s%s%s)"), Builder.Get() ? *Builder->GetDisplayName() : TEXT("?"), Why, By ? TEXT(", by ") : TEXT(""),
		By ? *By->GetDisplayName() : TEXT(""));
	End(Why);
}

void APinguSnowman::End(const TCHAR* Why)
{
	bEnded = true;
	LastEnd = Why;
	Destroy();
}

// ---------------------------------------------------------------------------- the look (every machine)
// 2026-11-04 card snowman-look (Linus: "The snowman looks unfinished ... make everything about it more detaild ... look more like a
// penguinstyle or the rest of ghe abilitys"). Every part is a mesh made here from its replicated LookSeed and Facing (every machine
// makes the same snowman; no assets to import): three lumpy, hand-packed snow balls on the island's snow material, each sunk into
// the one below with a packed collar in the seam, the bottom one settled flat on the ground; coal eyes, smile and buttons and stick
// arms on the penguin's clay; a carrot on the beak's clay; a beanie like the penguin's (felt crown, ribbed wool cuff, pompom) and a
// knitted scarf with a striped tail, both in its builder's beanie colours. Fewer triangles on Low.

namespace PinguSnowmanPrivate
{
	/** One mesh section being made. Triangles face away from the point given with them. */
	struct FMeshPart
	{
		TArray<FVector> V;
		TArray<int32> I;
		TArray<FVector2D> UV;
		TArray<FColor> C;

		int32 Add(const FVector& P, const FVector2D& InUV, uint8 Light = 255)
		{
			UV.Add(InUV);
			C.Add(FColor(Light, Light, Light, 255));
			return V.Add(P);
		}

		void Tri(int32 A, int32 B, int32 Third, const FVector& Inside)
		{
			// The engine's front faces: the cross product of the first two edges points into the mesh.
			const FVector Cross = FVector::CrossProduct(V[B] - V[A], V[Third] - V[A]);
			const FVector Mid = (V[A] + V[B] + V[Third]) / 3.f;
			if (FVector::DotProduct(Cross, Inside - Mid) >= 0.f)
			{
				I.Append({ A, B, Third });
			}
			else
			{
				I.Append({ A, Third, B });
			}
		}
	};

	/** An icosphere's unit directions and triangles (no seams, even triangles). */
	static void Icosphere(int32 Subdivisions, TArray<FVector>& Dirs, TArray<int32>& Tris)
	{
		const float G = (1.f + FMath::Sqrt(5.f)) * 0.5f;
		const FVector Corners[] = { { -1, G, 0 }, { 1, G, 0 }, { -1, -G, 0 }, { 1, -G, 0 }, { 0, -1, G }, { 0, 1, G }, { 0, -1, -G }, { 0, 1, -G },
			{ G, 0, -1 }, { G, 0, 1 }, { -G, 0, -1 }, { -G, 0, 1 } };
		Dirs.Reset();
		for (const FVector& Corner : Corners)
		{
			Dirs.Add(Corner.GetSafeNormal());
		}
		Tris = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
			3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 };
		for (int32 Level = 0; Level < Subdivisions; ++Level)
		{
			TMap<uint64, int32> Mids;
			const auto Midpoint = [&Dirs, &Mids](int32 A, int32 B) -> int32
			{
				const uint64 Key = A < B ? (static_cast<uint64>(A) << 32) | static_cast<uint32>(B) : (static_cast<uint64>(B) << 32) | static_cast<uint32>(A);
				if (const int32* Found = Mids.Find(Key))
				{
					return *Found;
				}
				const int32 Made = Dirs.Add(((Dirs[A] + Dirs[B]) * 0.5f).GetSafeNormal());
				Mids.Add(Key, Made);
				return Made;
			};
			TArray<int32> Next;
			Next.Reserve(Tris.Num() * 4);
			for (int32 Tri = 0; Tri + 2 < Tris.Num(); Tri += 3)
			{
				const int32 A = Tris[Tri];
				const int32 B = Tris[Tri + 1];
				const int32 C = Tris[Tri + 2];
				const int32 AB = Midpoint(A, B);
				const int32 BC = Midpoint(B, C);
				const int32 CA = Midpoint(C, A);
				Next.Append({ A, AB, CA, B, BC, AB, C, CA, BC, AB, BC, CA });
			}
			Tris = MoveTemp(Next);
		}
	}

	/** One snow ball's shape (from the seed): packed clumps and a few dents, flat hand pats, a gentle wobble, the collar. */
	struct FBallShape
	{
		struct FBlob
		{
			FVector Dir;
			float Amount;
			float Width;
		};
		int32 Which = 0;
		TArray<FBlob> Lumps;
		TArray<FBlob> Pats;
		TArray<FBlob> Patches;
		FVector Offset = FVector::ZeroVector;
		float CollarPhase = 0.f;

		void Make(int32 InWhich, int32 Seed)
		{
			Which = InWhich;
			FRandomStream Rand(Seed);
			for (int32 Index = 0; Index < 12; ++Index)
			{
				const bool bDent = Index % 4 == 3;
				Lumps.Add({ Rand.GetUnitVector(), static_cast<float>(bDent ? -Rand.FRandRange(0.02f, 0.04f) : Rand.FRandRange(0.025f, 0.065f)),
					static_cast<float>(Rand.FRandRange(0.25f, 0.55f)) });
			}
			// Flat pats where hands pressed it (on its sides, not under it).
			for (int32 Index = 0; Index < 6; ++Index)
			{
				FVector Dir = Rand.GetUnitVector();
				Dir.Z *= 0.6f;
				Pats.Add({ Dir.GetSafeNormal(), static_cast<float>(Rand.FRandRange(0.035f, 0.055f)), 0.f });
			}
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Patches.Add({ Rand.GetUnitVector(), static_cast<float>(Rand.FRandRange(0.06f, 0.12f)), static_cast<float>(Rand.FRandRange(0.3f, 0.5f)) });
			}
			Offset = FVector(Rand.FRandRange(0.f, 50.f), Rand.FRandRange(0.f, 50.f), Rand.FRandRange(0.f, 50.f));
			CollarPhase = Rand.FRandRange(0.f, 20.f);
		}

		/** Its radius toward D (a unit direction; about 1). */
		float RadiusAt(const FVector& D) const
		{
			float Radius = 1.f;
			for (const FBlob& Lump : Lumps)
			{
				const float Angle = FMath::Acos(FMath::Clamp(static_cast<float>(FVector::DotProduct(D, Lump.Dir)), -1.f, 1.f));
				Radius += Lump.Amount * FMath::Exp(-FMath::Square(Angle / Lump.Width));
			}
			Radius += 0.03f * FMath::PerlinNoise3D(D * 2.4f + Offset) + 0.012f * FMath::PerlinNoise3D(D * 7.f + Offset * 2.f);
			if (Which > 0)
			{
				// The collar: packed snow pressed round the seam with the ball below, thicker in places.
				const float Ring = FMath::Exp(-FMath::Square((static_cast<float>(D.Z) - CollarAt[Which]) / 0.12f));
				const float Around = FMath::Atan2(static_cast<float>(D.Y), static_cast<float>(D.X));
				Radius += Ring * (0.09f + 0.035f * FMath::PerlinNoise1D(Around * 1.6f + CollarPhase));
			}
			for (const FBlob& Pat : Pats)
			{
				const float Dot = static_cast<float>(FVector::DotProduct(D, Pat.Dir));
				if (Dot > 0.8f)
				{
					Radius = FMath::Min(Radius, (1.f + 0.02f - Pat.Amount) / Dot);
				}
			}
			return Radius;
		}

		/** Its surface toward D, around its middle (cm, size 1): squashed; the bottom ball flat on the ground and spread there. */
		FVector Surface(const FVector& D) const
		{
			const float R = BallRadius[Which] * RadiusAt(D);
			FVector P(D.X * R, D.Y * R, D.Z * R * BallSquash[Which]);
			if (Which == 0)
			{
				const float Ground = -BallHeight[0];
				const float Near = 1.f - FMath::SmoothStep(0.f, 14.f, static_cast<float>(P.Z) - Ground);
				P.X *= 1.f + 0.1f * Near;
				P.Y *= 1.f + 0.1f * Near;
				P.Z = FMath::Max(static_cast<float>(P.Z), Ground - 1.f);
			}
			return P;
		}
	};

	/** How far P (cm, around its feet) is into ball Which, in its own radii (1 = on its surface). */
	static float InBall(const FVector& P, int32 Which)
	{
		const FVector Q = (P - FVector(0.f, 0.f, BallHeight[Which])) / FVector(BallRadius[Which], BallRadius[Which], BallRadius[Which] * BallSquash[Which]);
		return static_cast<float>(Q.Size());
	}

	/** A snow ball: its shape, its shading in the vertex colour (the snow material's AO): hollows, packed patches, the shadow in
	 *  the seams and on the ground. UVs: flat toward the camera at the island's grain (1 UV = 100 cm). */
	static void MakeBall(FMeshPart& Part, const FBallShape& Shape, int32 Subdivisions)
	{
		const int32 Which = Shape.Which;
		TArray<FVector> Dirs;
		TArray<int32> Tris;
		Icosphere(Subdivisions, Dirs, Tris);
		for (const FVector& D : Dirs)
		{
			const FVector P = Shape.Surface(D);
			const FVector W = P + FVector(0.f, 0.f, BallHeight[Which]);
			const float Radius = Shape.RadiusAt(D);
			float Light = FMath::Lerp(0.8f, 1.f, FMath::Clamp((Radius - 0.95f) / 0.13f, 0.f, 1.f));
			for (const FBallShape::FBlob& Patch : Shape.Patches)
			{
				const float Angle = FMath::Acos(FMath::Clamp(static_cast<float>(FVector::DotProduct(D, Patch.Dir)), -1.f, 1.f));
				Light -= Patch.Amount * FMath::Exp(-FMath::Square(Angle / Patch.Width));
			}
			if (Which < 2)
			{
				Light -= 0.34f * (1.f - FMath::SmoothStep(1.f, 1.5f, InBall(W, Which + 1))); // (under the ball above)
			}
			if (Which > 0)
			{
				Light -= 0.16f * (1.f - FMath::SmoothStep(1.f, 1.25f, InBall(W, Which - 1))); // (the seam under the collar)
			}
			else
			{
				Light -= 0.24f * (1.f - FMath::SmoothStep(0.f, 12.f, static_cast<float>(W.Z))); // (on the ground)
			}
			Light -= 0.1f * FMath::Max(-static_cast<float>(D.Z), 0.f);
			const uint8 Value = static_cast<uint8>(FMath::Clamp(Light, 0.4f, 1.f) * 255.f);
			Part.Add(P, FVector2D(W.X, -W.Z) / 100.f, Value);
		}
		for (int32 Tri = 0; Tri + 2 < Tris.Num(); Tri += 3)
		{
			Part.Tri(Tris[Tri], Tris[Tri + 1], Tris[Tri + 2], FVector::ZeroVector);
		}
	}

	/** A lumpy round thing (the pompom, the scarf's knot): Radius, Noise how lumpy. */
	static void MakeLump(FMeshPart& Part, const FVector& Center, float Radius, float Noise, int32 Subdivisions, const FVector& Seed, float UVScale)
	{
		TArray<FVector> Dirs;
		TArray<int32> Tris;
		Icosphere(Subdivisions, Dirs, Tris);
		const int32 Base = Part.V.Num();
		for (const FVector& D : Dirs)
		{
			const float R = Radius * (1.f + Noise * FMath::PerlinNoise3D(D * 3.f + Seed) + Noise * 0.4f * FMath::PerlinNoise3D(D * 8.f + Seed));
			Part.Add(Center + D * R, FVector2D(D.X, -D.Z) * Radius * UVScale);
		}
		for (int32 Tri = 0; Tri + 2 < Tris.Num(); Tri += 3)
		{
			Part.Tri(Base + Tris[Tri], Base + Tris[Tri + 1], Base + Tris[Tri + 2], Center);
		}
	}

	/** A lump of coal: a jagged little stone, faceted, half pressed flat against the snow (Out: away from the snow). */
	static void MakeCoal(FMeshPart& Part, const FVector& Center, const FVector& Out, float Radius, FRandomStream& Rand)
	{
		TArray<FVector> Dirs;
		TArray<int32> Tris;
		Icosphere(0, Dirs, Tris);
		const FQuat Spin(Rand.GetUnitVector(), Rand.FRandRange(0.f, 2.f * UE_PI));
		TArray<FVector> Points;
		for (const FVector& D : Dirs)
		{
			FVector P = Spin.RotateVector(D) * Radius * Rand.FRandRange(0.78f, 1.12f);
			P -= Out * FVector::DotProduct(P, Out) * 0.35f;
			Points.Add(Center + P);
		}
		for (int32 Tri = 0; Tri + 2 < Tris.Num(); Tri += 3)
		{
			// (its own corners per face: flat facets)
			const int32 A = Part.Add(Points[Tris[Tri]], FVector2D(0.f, 0.f));
			const int32 B = Part.Add(Points[Tris[Tri + 1]], FVector2D(0.1f, 0.f));
			const int32 C = Part.Add(Points[Tris[Tri + 2]], FVector2D(0.f, 0.1f));
			Part.Tri(A, B, C, Center);
		}
	}

	/** A tube along Spine: an ellipse Radii.X across (along Side = the spine's direction x Ref) by Radii.Y, Sides round; closed
	 *  ends (a rounded cap) where asked; bLoop: the spine is a closed ring. UVs in cm x UVScale. */
	static void MakeTube(FMeshPart& Part, const TArray<FVector>& Spine, const TArray<FVector2D>& Radii, int32 Sides, const FVector& Ref,
		bool bCapStart, bool bCapEnd, float UVScale, bool bLoop = false)
	{
		const int32 Count = Spine.Num();
		if (Count < 2 || Radii.Num() != Count)
		{
			return;
		}
		const int32 Base = Part.V.Num();
		const int32 Row = Sides + 1;
		TArray<FVector> Along;
		float Length = 0.f;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector Prev = bLoop ? Spine[(Index - 1 + Count) % Count] : Spine[FMath::Max(Index - 1, 0)];
			const FVector Next = bLoop ? Spine[(Index + 1) % Count] : Spine[FMath::Min(Index + 1, Count - 1)];
			const FVector Dir = (Next - Prev).GetSafeNormal();
			Along.Add(Dir);
			FVector Side = FVector::CrossProduct(Dir, Ref).GetSafeNormal();
			if (Side.IsNearlyZero())
			{
				Side = FVector::CrossProduct(Dir, FVector::XAxisVector).GetSafeNormal();
			}
			const FVector Normal = FVector::CrossProduct(Side, Dir);
			if (Index > 0)
			{
				Length += static_cast<float>(FVector::Dist(Spine[Index], Spine[Index - 1]));
			}
			const float Round = UE_PI * (Radii[Index].X + Radii[Index].Y);
			for (int32 Around = 0; Around <= Sides; ++Around)
			{
				const float Angle = 2.f * UE_PI * Around / Sides;
				const FVector P = Spine[Index] + Side * FMath::Cos(Angle) * Radii[Index].X + Normal * FMath::Sin(Angle) * Radii[Index].Y;
				Part.Add(P, FVector2D(Round * Around / Sides, Length) * UVScale);
			}
		}
		const int32 Spans = bLoop ? Count : Count - 1;
		for (int32 Index = 0; Index < Spans; ++Index)
		{
			const int32 NextIndex = (Index + 1) % Count;
			const FVector Inside = (Spine[Index] + Spine[NextIndex]) * 0.5f;
			for (int32 Around = 0; Around < Sides; ++Around)
			{
				const int32 A = Base + Index * Row + Around;
				const int32 C = Base + NextIndex * Row + Around;
				Part.Tri(A, A + 1, C, Inside);
				Part.Tri(A + 1, C + 1, C, Inside);
			}
		}
		const auto Cap = [&Part, &Spine, &Radii, &Along, Base, Row, Sides](int32 Index, float Outward)
		{
			const float Bulge = FMath::Min(Radii[Index].X, Radii[Index].Y) * 0.6f;
			const FVector Tip = Spine[Index] + Along[Index] * Outward * Bulge;
			const FVector Inside = Spine[Index] - Along[Index] * Outward * FMath::Max(Bulge, 0.05f);
			const int32 Center = Part.Add(Tip, FVector2D(0.f, 0.f));
			for (int32 Around = 0; Around < Sides; ++Around)
			{
				Part.Tri(Base + Index * Row + Around, Base + Index * Row + Around + 1, Center, Inside);
			}
		};
		if (!bLoop && bCapStart)
		{
			Cap(0, -1.f);
		}
		if (!bLoop && bCapEnd)
		{
			Cap(Count - 1, 1.f);
		}
	}

	/** Normals (smooth where corners are shared) and tangents along the UVs' U. */
	static void Finish(const FMeshPart& Part, TArray<FVector>& OutNormals, TArray<FProcMeshTangent>& OutTangents)
	{
		TArray<FVector> Normals;
		TArray<FVector> Tangents;
		Normals.Init(FVector::ZeroVector, Part.V.Num());
		Tangents.Init(FVector::ZeroVector, Part.V.Num());
		for (int32 Index = 0; Index + 2 < Part.I.Num(); Index += 3)
		{
			const int32 A = Part.I[Index];
			const int32 B = Part.I[Index + 1];
			const int32 C = Part.I[Index + 2];
			const FVector E1 = Part.V[B] - Part.V[A];
			const FVector E2 = Part.V[C] - Part.V[A];
			const FVector Out = -FVector::CrossProduct(E1, E2); // (the winding's cross product points in)
			const FVector2D D1 = Part.UV[B] - Part.UV[A];
			const FVector2D D2 = Part.UV[C] - Part.UV[A];
			const double Det = D1.X * D2.Y - D2.X * D1.Y;
			const FVector Tangent = FMath::Abs(Det) > 1.e-9 ? (E1 * D2.Y - E2 * D1.Y) / Det : FVector::ZeroVector;
			for (const int32 Corner : { A, B, C })
			{
				Normals[Corner] += Out;
				Tangents[Corner] += Tangent.GetSafeNormal();
			}
		}
		OutNormals.SetNum(Part.V.Num());
		OutTangents.SetNum(Part.V.Num());
		for (int32 Index = 0; Index < Part.V.Num(); ++Index)
		{
			const FVector Normal = Normals[Index].GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			FVector Tangent = Tangents[Index] - Normal * FVector::DotProduct(Tangents[Index], Normal);
			if (!Tangent.Normalize())
			{
				Tangent = FVector::CrossProduct(Normal, FMath::Abs(Normal.Z) < 0.9f ? FVector::UpVector : FVector::XAxisVector).GetSafeNormal();
			}
			OutNormals[Index] = Normal;
			OutTangents[Index] = FProcMeshTangent(Tangent, false);
		}
	}

	/** Part into Mesh's Section with Material; its triangles. */
	static int32 Upload(UProceduralMeshComponent* Mesh, int32 Section, const FMeshPart& Part, UMaterialInterface* Material)
	{
		if (!Mesh || Part.I.Num() == 0)
		{
			return 0;
		}
		TArray<FVector> Normals;
		TArray<FProcMeshTangent> Tangents;
		Finish(Part, Normals, Tangents);
		Mesh->CreateMeshSection(Section, Part.V, Part.I, Normals, Part.UV, Part.C, Tangents, false);
		Mesh->SetMaterial(Section, Material);
		return Part.I.Num() / 3;
	}

	/** A direction from Around (degrees from the front, +Y, toward +X) and Up (degrees). */
	static FVector Toward(float AroundDeg, float UpDeg)
	{
		const float Around = FMath::DegreesToRadians(AroundDeg);
		const float Up = FMath::DegreesToRadians(UpDeg);
		return FVector(FMath::Sin(Around) * FMath::Cos(Up), FMath::Cos(Around) * FMath::Cos(Up), FMath::Sin(Up));
	}

	/** A beanie colour off the penguin (its beanie's felt / wool instances: APenguinCharacter::ApplyBeanieColor). */
	static bool ReadBeanieColor(const APenguinCharacter* Penguin, const TCHAR* Piece, FLinearColor& Out)
	{
		const USkeletalMeshComponent* PenguinMesh = Penguin ? Penguin->GetMesh() : nullptr;
		if (!PenguinMesh)
		{
			return false;
		}
		const TArray<FName> Names = PenguinMesh->GetMaterialSlotNames();
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Names[Index].ToString().Contains(Piece, ESearchCase::IgnoreCase))
			{
				if (UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(PenguinMesh->GetMaterial(Index)))
				{
					Out = Dynamic->K2_GetVectorParameterValue(TEXT("Color"));
					return true;
				}
			}
		}
		return false;
	}
}

void APinguSnowman::MakeLook()
{
	using namespace PinguSnowmanPrivate;
	bLookMade = true;
	UMaterialInterface* Prop = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Prop.M_Prop"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	if (!Prop)
	{
		Prop = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}
	// The island's snow, the penguin's own clay, felt and wool (M_Prop in their colour if one is missing).
	const auto MakeMaterial = [this, Prop](const TCHAR* Path, const FLinearColor& Color, bool bSetColor, float Roughness) -> UMaterialInterface*
	{
		UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
		const bool bFallback = !Parent;
		Parent = Parent ? Parent : Prop;
		if (!Parent)
		{
			return nullptr;
		}
		if (!bSetColor && !bFallback && Roughness < 0.f)
		{
			return Parent;
		}
		UMaterialInstanceDynamic* Made = UMaterialInstanceDynamic::Create(Parent, this);
		if (bSetColor || bFallback)
		{
			Made->SetVectorParameterValue(TEXT("Color"), Color);
		}
		if (Roughness >= 0.f)
		{
			Made->SetScalarParameterValue(TEXT("Roughness"), Roughness);
		}
		return Made;
	};
	Materials.SetNum(MatCount);
	Materials[MatSnow] = MakeMaterial(TEXT("/Game/Environment/M_Block_Snow.M_Block_Snow"), SnowColor, true, 0.75f);
	if (UMaterialInstanceDynamic* SnowMade = Cast<UMaterialInstanceDynamic>(Materials[MatSnow]))
	{
		SnowMade->SetScalarParameterValue(TEXT("Emissive"), 0.f);
	}
	Materials[MatCoal] = MakeMaterial(TEXT("/Game/Penguin/Materials/MI_PenguinHQ_Black.MI_PenguinHQ_Black"), CoalColor, true, -1.f);
	Materials[MatStick] = MakeMaterial(TEXT("/Game/Penguin/Materials/MI_PenguinHQ_Black.MI_PenguinHQ_Black"), StickColor, true, 0.85f);
	Materials[MatCarrot] = MakeMaterial(TEXT("/Game/Penguin/Materials/MI_PenguinHQ_Orange.MI_PenguinHQ_Orange"), CarrotColor, false, -1.f);
	Materials[MatFelt] = MakeMaterial(TEXT("/Game/Penguin/Materials/MI_PenguinHQ_Felt.MI_PenguinHQ_Felt"), FLinearColor(0.32f, 0.012f, 0.014f), true, -1.f);
	Materials[MatWool] = MakeMaterial(TEXT("/Game/Penguin/Materials/MI_PenguinHQ_Wool.MI_PenguinHQ_Wool"), FLinearColor(0.45f, 0.012f, 0.014f), true, -1.f);
	Materials[MatStripe] = MakeMaterial(TEXT("/Game/Penguin/Materials/MI_PenguinHQ_Wool.MI_PenguinHQ_Wool"), StripeColor, true, -1.f);
	Materials[MatPom] = MakeMaterial(TEXT("/Game/Penguin/Materials/MI_PenguinHQ_Pom.MI_PenguinHQ_Pom"), FLinearColor(0.9f, 0.9f, 0.9f), false, -1.f);

	const auto MakePart = [this](USceneComponent* Parent, bool bShadow) -> UProceduralMeshComponent*
	{
		UProceduralMeshComponent* Part = NewObject<UProceduralMeshComponent>(this, NAME_None, RF_Transient);
		Part->SetupAttachment(Parent);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetGenerateOverlapEvents(false);
		Part->SetCanEverAffectNavigation(false);
		Part->SetCastShadow(bShadow);
		Part->RegisterComponent();
		return Part;
	};
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Balls.Add(MakePart(SceneRoot, true));
	}
	Head = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	Head->SetupAttachment(SceneRoot);
	Head->RegisterComponent();
	Face = MakePart(Head, false);
	Buttons = MakePart(Balls[1], false);
	Arms.Add(MakePart(SceneRoot, true));
	Arms.Add(MakePart(SceneRoot, true));
	Hat = MakePart(Head, true);
	Scarf = MakePart(Head, true);
	SeenThrow = ThrowSerial;
}

void APinguSnowman::UpdateColors()
{
	using namespace PinguSnowmanPrivate;
	// Its beanie and scarf: its builder's beanie felt and cuff (a darker and a brighter shade of its colour).
	const APenguinCharacter* Who = Builder.Get();
	FLinearColor Felt = FLinearColor(0.32f, 0.012f, 0.014f);
	FLinearColor Cuff = FLinearColor(0.45f, 0.012f, 0.014f);
	if (Who)
	{
		if (!ReadBeanieColor(Who, TEXT("felt"), Felt))
		{
			Felt = Who->GetPlayerColor() * 0.35f;
		}
		if (!ReadBeanieColor(Who, TEXT("wool"), Cuff))
		{
			Cuff = Felt * 1.4f;
		}
	}
	else if (ShownFelt.A > 0.f)
	{
		return; // (its builder gone: it keeps its colours)
	}
	Felt.A = 1.f;
	Cuff.A = 1.f;
	if (Felt.Equals(ShownFelt) && Cuff.Equals(ShownCuff))
	{
		return;
	}
	ShownFelt = Felt;
	ShownCuff = Cuff;
	if (UMaterialInstanceDynamic* FeltMade = Materials.IsValidIndex(MatFelt) ? Cast<UMaterialInstanceDynamic>(Materials[MatFelt]) : nullptr)
	{
		FeltMade->SetVectorParameterValue(TEXT("Color"), Felt);
	}
	if (UMaterialInstanceDynamic* WoolMade = Materials.IsValidIndex(MatWool) ? Cast<UMaterialInstanceDynamic>(Materials[MatWool]) : nullptr)
	{
		WoolMade->SetVectorParameterValue(TEXT("Color"), Cuff);
	}
}

void APinguSnowman::MakeMeshes(int32 Detail)
{
	using namespace PinguSnowmanPrivate;
	MeshDetail = Detail;
	MeshSeed = LookSeed * 2 + (Facing >= 0.f ? 1 : 0);
	const float Dir = Facing >= 0.f ? 1.f : -1.f;
	const int32 Subdivisions = Detail <= 0 ? 3 : 4;
	const int32 Sides = Detail <= 0 ? 5 : (Detail == 1 ? 7 : 9);
	const float FabricUV = 1.f / 5.f;
	const float ClayUV = 1.f / 10.f;
	FBallShape Shapes[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Shapes[Index].Make(Index, LookSeed + 7919 * (Index + 1));
	}
	for (UProceduralMeshComponent* Part : { Balls[0].Get(), Balls[1].Get(), Balls[2].Get(), Face.Get(), Buttons.Get(), Arms[0].Get(), Arms[1].Get(), Hat.Get(), Scarf.Get() })
	{
		Part->ClearAllMeshSections();
	}
	FRandomStream Rand(LookSeed + 17);

	// ---- the balls
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FMeshPart Ball;
		MakeBall(Ball, Shapes[Index], Subdivisions);
		Upload(Balls[Index], 0, Ball, Materials[MatSnow]);
	}

	// ---- the face (on its head, the front toward the camera, +Y; it turns as a whole): coal eyes, a coal smile, a carrot - a
	// stubby one with a rounded tip, on the beak's orange clay
	{
		FMeshPart Coal;
		FMeshPart Nose;
		const auto OnHead = [&Shapes](float AroundDeg, float UpDeg, float Into) -> FVector
		{
			return Shapes[2].Surface(Toward(AroundDeg, UpDeg)) * Into;
		};
		for (const float EyeSide : { -1.f, 1.f })
		{
			MakeCoal(Coal, OnHead(25.f * EyeSide, 12.f, 0.99f), Toward(25.f * EyeSide, 12.f), 2.9f, Rand);
		}
		for (int32 Index = 0; Index < 5; ++Index)
		{
			const float Around = -22.f + 11.f * Index;
			const float Up = -19.f + 7.f * FMath::Square(Around / 22.f);
			MakeCoal(Coal, OnHead(Around, Up, 0.99f), Toward(Around, Up), 1.6f, Rand);
		}
		TArray<FVector> Spine;
		TArray<FVector2D> Radii;
		const FVector Root = OnHead(0.f, -3.f, 0.8f);
		const int32 Rings = Detail <= 0 ? 5 : 8;
		for (int32 Index = 0; Index <= Rings; ++Index)
		{
			const float T = static_cast<float>(Index) / Rings;
			Spine.Add(Root + FVector(0.f, 4.f + 13.f * T, -2.4f * T * T));
			// Thick at its root, a few grooves (the carrot's rings), a rounded tip.
			const float Ring = FMath::Exp(-FMath::Square((FMath::Frac(T * 3.f + 0.5f) - 0.5f) / 0.12f));
			const float Groove = Detail <= 0 || T < 0.15f || T > 0.9f ? 1.f : 1.f - 0.12f * Ring;
			const float Radius = FMath::Lerp(3.1f, 0.9f, FMath::Pow(T, 1.2f)) * Groove;
			Radii.Add(FVector2D(Radius, Radius));
		}
		MakeTube(Nose, Spine, Radii, Sides + 1, FVector::UpVector, true, true, ClayUV);
		Upload(Face, 0, Coal, Materials[MatCoal]);
		Upload(Face, 1, Nose, Materials[MatCarrot]);
	}

	// ---- three coal buttons down its middle ball's front
	{
		FMeshPart Coal;
		const float Ups[3] = { 34.f, 8.f, -18.f };
		const float Sizes[3] = { 3.1f, 2.9f, 2.7f };
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FVector Out = Toward(Dir * 2.f, Ups[Index]);
			MakeCoal(Coal, Shapes[1].Surface(Out) * 0.99f, Out, Sizes[Index], Rand);
		}
		Upload(Buttons, 0, Coal, Materials[MatCoal]);
	}

	// ---- the stick arms (built along +Z from inside the shoulder; a twig on the upper side, two at the end)
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const float ArmSide = Index == 0 ? -1.f : 1.f;
		const float Up = -ArmSide; // (turned out to its side, local +X ends up on top for the left arm, -X for the right one)
		FMeshPart Stick;
		const int32 Rings = Detail <= 0 ? 4 : 6;
		const int32 Round = FMath::Max(Sides - 1, 4);
		const auto Branch = [&Stick, Detail, Round](const FVector& From, const FVector& To, float Bend, float RootRadius, float TipRadius, int32 Count)
		{
			TArray<FVector> Spine;
			TArray<FVector2D> Radii;
			const FVector Across = FVector::CrossProduct((To - From).GetSafeNormal(), FVector::YAxisVector).GetSafeNormal();
			for (int32 Ring = 0; Ring <= Count; ++Ring)
			{
				const float T = static_cast<float>(Ring) / Count;
				Spine.Add(FMath::Lerp(From, To, T) + Across * Bend * FMath::Sin(UE_PI * T));
				const float Radius = FMath::Lerp(RootRadius, TipRadius, T) * (Detail > 0 && Count > 2 && Ring == Count / 2 ? 1.15f : 1.f); // (a knot)
				Radii.Add(FVector2D(Radius, Radius));
			}
			MakeTube(Stick, Spine, Radii, Round, FVector::YAxisVector, false, true, 1.f / 10.f);
		};
		Branch(FVector(0.f, 0.f, -8.f), FVector(0.f, 0.f, 44.f), 1.6f * Up, 1.9f, 1.05f, Rings);
		Branch(FVector(Up * 0.4f, 0.f, 24.f), FVector(Up * 7.f, 0.f, 34.f), -0.6f * Up, 1.f, 0.4f, 2);
		Branch(FVector(0.f, 0.f, 42.f), FVector(Up * 4.5f, 0.f, 50.f), 0.f, 0.85f, 0.35f, 2);
		Branch(FVector(0.f, 0.f, 42.f), FVector(-Up * 3.5f, 0.f, 49.f), 0.f, 0.75f, 0.3f, 2);
		Upload(Arms[Index], 0, Stick, Materials[MatStick]);
	}

	// ---- the beanie (the penguin's: a felt crown, a ribbed wool cuff, a pompom), slouched a little away from its front
	{
		const float RimZ = 0.5f * BallRadius[2] * BallSquash[2];
		const float RimRadius = 20.4f;
		const float Lean = -Dir * 3.f;
		const float CrownHigh = 14.5f;
		FMeshPart Crown;
		{
			TArray<FVector> Spine;
			TArray<FVector2D> Radii;
			const int32 Rows = Detail <= 0 ? 6 : 10;
			for (int32 Index = 0; Index <= Rows; ++Index)
			{
				const float T = static_cast<float>(Index) / Rows;
				const float Angle = T * UE_PI * 0.5f;
				Spine.Add(FVector(Lean * T * T, 0.f, RimZ + 1.f + CrownHigh * FMath::Sin(Angle)));
				const float Radius = FMath::Max(RimRadius * 0.97f * FMath::Pow(FMath::Cos(Angle), 0.8f) * (1.f + 0.05f * FMath::Sin(UE_PI * T)), 0.3f);
				Radii.Add(FVector2D(Radius, Radius));
			}
			MakeTube(Crown, Spine, Radii, Detail <= 0 ? 12 : (Detail == 1 ? 18 : 24), FVector::XAxisVector, false, true, FabricUV);
		}
		FMeshPart Cuff;
		{
			TArray<FVector> Spine;
			TArray<FVector2D> Radii;
			constexpr int32 Ribs = 22;
			const int32 Count = Ribs * (Detail <= 0 ? 2 : 4);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const float Angle = 2.f * UE_PI * Index / Count;
				Spine.Add(FVector(FMath::Cos(Angle) * RimRadius, FMath::Sin(Angle) * RimRadius, RimZ + 1.5f));
				const float Rib = FMath::Abs(FMath::Sin(Ribs * Angle * 0.5f));
				Radii.Add(FVector2D(2.3f + 0.9f * Rib, 4.f));
			}
			MakeTube(Cuff, Spine, Radii, Sides, FVector::UpVector, false, false, FabricUV, true);
			for (FVector2D& KnitUV : Cuff.UV)
			{
				KnitUV = FVector2D(KnitUV.Y, KnitUV.X); // (the knit's ribs up and down, as on the penguin's cuff)
			}
		}
		FMeshPart Pom;
		MakeLump(Pom, FVector(Lean, 0.f, RimZ + 1.f + CrownHigh + 4.f), 6.f, 0.14f, Detail <= 0 ? 1 : 2, FVector(static_cast<float>(LookSeed % 97), 3.f, 7.f), FabricUV);
		Upload(Hat, 0, Crown, Materials[MatFelt]);
		Upload(Hat, 1, Cuff, Materials[MatWool]);
		Upload(Hat, 2, Pom, Materials[MatPom]);
	}

	// ---- the scarf: a knitted band round its neck (over the seam), a knot on its front, a tail hanging down its middle ball with
	// two white stripes and a fringe
	{
		FMeshPart Wool;
		FMeshPart Stripes;
		const auto OnNeck = [&Shapes](const FVector& Around) -> FVector
		{
			return Shapes[2].Surface((Around * 0.79f + FVector(0.f, 0.f, -0.61f)).GetSafeNormal());
		};
		{
			const int32 Count = Detail <= 0 ? 20 : 32;
			TArray<FVector> Spine;
			TArray<FVector2D> Radii;
			const float Phase = Rand.FRandRange(0.f, 6.f);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const float Angle = 2.f * UE_PI * Index / Count;
				const FVector Around(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);
				const FVector Neck = OnNeck(Around);
				const float Out = static_cast<float>(FVector2D(Neck.X, Neck.Y).Size()) + 1.6f;
				Spine.Add(Around * Out + FVector(0.f, 0.f, Neck.Z + 1.1f * FMath::Sin(2.f * Angle + Phase)));
				Radii.Add(FVector2D(3.f, 4.6f));
			}
			MakeTube(Wool, Spine, Radii, Sides, FVector::UpVector, false, false, FabricUV, true);
		}
		// The knot, on its front a little toward its back side.
		const FVector KnotDir = FVector(-Dir * 0.42f, 0.91f, 0.f).GetSafeNormal();
		const FVector KnotNeck = OnNeck(KnotDir);
		const FVector Knot = KnotDir * (FVector2D(KnotNeck.X, KnotNeck.Y).Size() + 4.f) + FVector(0.f, 0.f, KnotNeck.Z - 0.5f);
		MakeLump(Wool, Knot, 4.6f, 0.1f, Detail <= 0 ? 1 : 2, FVector(5.f, static_cast<float>(LookSeed % 89), 1.f), FabricUV);
		// The tail: down over its middle ball's front (in the head's frame the middle ball's middle is BallHeight[1] - [2] lower).
		const FVector MiddleBall(0.f, 0.f, BallHeight[1] - BallHeight[2]);
		TArray<FVector> Tail;
		const float TailUps[5] = { 0.62f, 0.42f, 0.2f, -0.02f, -0.22f };
		Tail.Add(Knot + FVector(0.f, 1.f, -2.f));
		for (int32 Index = 0; Index < 5; ++Index)
		{
			FVector Out(-Dir * (0.36f + 0.03f * Index), 0.f, TailUps[Index]);
			Out.Y = FMath::Sqrt(FMath::Max(1.f - FMath::Square(static_cast<float>(Out.X)) - FMath::Square(static_cast<float>(Out.Z)), 0.05f));
			Out = Out.GetSafeNormal();
			Tail.Add(MiddleBall + Shapes[1].Surface(Out) + Out * 2.f);
		}
		// Sampled smoothly (a Catmull-Rom curve through the points), in segments: wool, a stripe, wool, a stripe, wool.
		const auto At = [&Tail](float T) -> FVector
		{
			const float Scaled = FMath::Clamp(T, 0.f, 1.f) * (Tail.Num() - 1);
			const int32 Index = FMath::Min(FMath::FloorToInt(Scaled), Tail.Num() - 2);
			const float F = Scaled - Index;
			const FVector P0 = Tail[FMath::Max(Index - 1, 0)];
			const FVector P1 = Tail[Index];
			const FVector P2 = Tail[Index + 1];
			const FVector P3 = Tail[FMath::Min(Index + 2, Tail.Num() - 1)];
			return 0.5f * ((2.f * P1) + (-P0 + P2) * F + (2.f * P0 - 5.f * P1 + 4.f * P2 - P3) * F * F + (-P0 + 3.f * P1 - 3.f * P2 + P3) * F * F * F);
		};
		const float Cuts[6] = { 0.f, 0.42f, 0.53f, 0.7f, 0.81f, 1.f };
		for (int32 Segment = 0; Segment < 5; ++Segment)
		{
			TArray<FVector> Points;
			TArray<FVector2D> Sizes;
			const int32 Steps = Detail <= 0 ? 2 : 4;
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const float T = FMath::Lerp(Cuts[Segment], Cuts[Segment + 1], static_cast<float>(Step) / Steps);
				Points.Add(At(T));
				Sizes.Add(FVector2D(FMath::Lerp(3.4f, 4.2f, T), 1.f)); // (wider toward its end; flat)
			}
			MakeTube(Segment % 2 == 1 ? Stripes : Wool, Points, Sizes, Sides, FVector::YAxisVector, Segment == 0, Segment == 4, FabricUV);
		}
		// The fringe: little tassels off its end.
		if (Detail > 0)
		{
			const FVector End = At(1.f);
			const FVector Down = (At(1.f) - At(0.95f)).GetSafeNormal();
			const FVector Across = FVector::CrossProduct(Down, FVector::YAxisVector).GetSafeNormal();
			for (int32 Index = 0; Index < 4; ++Index)
			{
				const FVector From = End + Across * (-3.f + 2.f * Index);
				const TArray<FVector> Points = { From, From + Down * 2.f + Across * 0.2f, From + Down * 4.f + Across * (0.3f * (Index - 1.5f)) };
				const TArray<FVector2D> Sizes = { FVector2D(0.7f, 0.7f), FVector2D(0.6f, 0.6f), FVector2D(0.45f, 0.45f) };
				MakeTube(Wool, Points, Sizes, 4, FVector::YAxisVector, false, true, FabricUV);
			}
		}
		Upload(Scarf, 0, Wool, Materials[MatWool]);
		Upload(Scarf, 1, Stripes, Materials[MatStripe]);
	}
}

void APinguSnowman::TickLook(float Dt)
{
	using namespace PinguSnowmanPrivate;
	if (!bLookMade)
	{
		MakeLook();
	}
	// Its meshes: made again when the effects setting's detail changes (or its seed arrives).
	{
		const APinguFX* FX = APinguFX::Get(this);
		const int32 Detail = UPinguSettings::GetFXDensity() < 0.5f ? 0 : (FX && FX->UsesUltraEffects() ? 2 : 1);
		if (Detail != MeshDetail || MeshSeed != LookSeed * 2 + (Facing >= 0.f ? 1 : 0))
		{
			MakeMeshes(Detail);
		}
	}
	UpdateColors();
	ShownSize = FMath::FInterpTo(ShownSize, SizeScale, Dt, 9.f);
	const float S = ShownSize;
	const float Build = FMath::Clamp(GetAge() / SnowmanBuildTime, 0.f, 1.f);
	// The throw: the arm on the target's side swings up and over, the head turns there.
	if (SeenThrow != ThrowSerial)
	{
		SeenThrow = ThrowSerial;
		SwingTime = 0.f;
	}
	SwingTime += Dt;
	ArmSwing = SwingTime < PinguSnowmanPrivate::SwingTime ? FMath::Sin(UE_PI * SwingTime / PinguSnowmanPrivate::SwingTime) : 0.f;
	const float WantTurn = (SwingTime < 1.2f ? ThrowSide : Facing) * HeadTurnDeg;
	HeadTurn = FMath::FInterpTo(HeadTurn, WantTurn, Dt, 8.f);

	// The balls pop up one after another while it is built (each grows from where it rests on the one below).
	float BallScale[3];
	float BallMiddle[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const float T = (Build - BallFrom[Index]) / FMath::Max(BallTo[Index] - BallFrom[Index], 0.01f);
		BallScale[Index] = Build >= 1.f ? 1.f : FMath::Max(Pop(T), 0.f);
		const float Rest = Index == 0 ? 0.f : BallHeight[Index] - BallRadius[Index] * BallSquash[Index];
		BallMiddle[Index] = (Rest + (BallHeight[Index] - Rest) * BallScale[Index]) * S;
		if (Balls.IsValidIndex(Index) && Balls[Index])
		{
			Balls[Index]->SetRelativeLocation(FVector(0.f, 0.f, BallMiddle[Index]));
			Balls[Index]->SetRelativeScale3D(FVector(FMath::Max(BallScale[Index] * S, 0.001f)));
			Balls[Index]->SetVisibility(BallScale[Index] * S > 0.03f);
		}
	}
	Head->SetRelativeLocation(FVector(0.f, 0.f, BallMiddle[2]));
	Head->SetRelativeScale3D(FVector(FMath::Max(BallScale[2], 0.001f) * S));
	const bool bHeadShown = BallScale[2] > 0.05f;
	// The face turns toward its side (and toward its target while it throws).
	Face->SetRelativeRotation(FRotator(0.f, -HeadTurn, 0.f));
	Face->SetVisibility(bHeadShown);
	Hat->SetVisibility(bHeadShown);
	Scarf->SetVisibility(bHeadShown);
	// The arms: sticks out of its middle ball, up and out; the throwing one swings over its head.
	for (int32 Index = 0; Index < Arms.Num(); ++Index)
	{
		const float Side = Index == 0 ? -1.f : 1.f;
		const bool bThrowing = Side == ThrowSide;
		const float Lift = 22.f + (bThrowing ? 110.f * ArmSwing : 0.f);
		const float Angle = FMath::DegreesToRadians(90.f - Lift);
		const FVector Out(Side * FMath::Sin(Angle), 0.15f, FMath::Cos(Angle));
		const FVector Shoulder(Side * BallRadius[1] * 0.8f * S, 0.f, BallMiddle[1] + 6.f * S);
		Arms[Index]->SetRelativeLocation(Shoulder);
		Arms[Index]->SetRelativeRotation(FQuat::FindBetweenNormals(FVector::UpVector, Out.GetSafeNormal()));
		Arms[Index]->SetRelativeScale3D(FVector(S * FMath::Max(BallScale[1], 0.01f)));
		Arms[Index]->SetVisibility(BallScale[1] > 0.5f);
	}
}

int32 APinguSnowman::GetLookTriangles() const
{
	int32 Count = 0;
	for (const UProceduralMeshComponent* Part : { Balls.IsValidIndex(0) ? Balls[0].Get() : nullptr, Balls.IsValidIndex(1) ? Balls[1].Get() : nullptr,
		Balls.IsValidIndex(2) ? Balls[2].Get() : nullptr, Face.Get(), Buttons.Get(), Arms.IsValidIndex(0) ? Arms[0].Get() : nullptr,
		Arms.IsValidIndex(1) ? Arms[1].Get() : nullptr, Hat.Get(), Scarf.Get() })
	{
		if (!Part || !Part->IsVisible())
		{
			continue;
		}
		UProceduralMeshComponent* Mutable = const_cast<UProceduralMeshComponent*>(Part);
		for (int32 Section = 0; Section < Part->GetNumSections(); ++Section)
		{
			if (const FProcMeshSection* Made = Mutable->GetProcMeshSection(Section))
			{
				Count += Made->ProcIndexBuffer.Num() / 3;
			}
		}
	}
	return Count;
}
