// Pingu - the giant snowball (see PinguGiantSnowball.h).

#include "PinguGiantSnowball.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinTraitComponent.h"
#include "PinguFX.h"
#include "PinguPickup.h"
#include "PinguSettings.h"
#include "ProceduralMeshComponent.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"

FString APinguGiantSnowball::LastEnd;
FVector APinguGiantSnowball::LastEndAt = FVector::ZeroVector;

namespace PinguGiantSnowballPrivate
{
	using namespace PinguTuning;

	static const FLinearColor FrozenColor(0.6f, 0.86f, 1.f);
	static const FLinearColor PuffColor(0.95f, 0.97f, 1.f);
	/** The roller pats the ball every this many seconds (alternating flippers) while rolling it up. */
	constexpr float PatEvery = 0.36f;
	/** The swallowed penguins are kept hanging still this long, renewed every HoverRenew (s). */
	constexpr float HoverHold = 1.2f;
	constexpr float HoverRenew = 0.5f;
	/** It is put this far in front of its roller's capsule while it is rolled up (cm). */
	constexpr float BuildGap = 6.f;
	/** Touching: its surface this far into a penguin's capsule (cm). */
	constexpr float TouchDepth = 6.f;
	/** Clients run it on from the last motion and blend in corrections at this rate (1/s); further off than SnapDistance: a snap. */
	constexpr float BlendRate = 14.f;
	constexpr float SnapDistance = 300.f;
	/** The pop of a size hit and the squash of a wall bounce: how much, how long (s). */
	constexpr float PopAmount = 0.16f;
	constexpr float PopTime = 0.35f;
	constexpr float BounceSquash = 0.14f;
	constexpr float BounceTime = 0.3f;
	/** The rolling rumble: one every this many seconds while it rolls on the ground. */
	constexpr float RollSoundEvery = 0.55f;
	/** A block reaching more than this (cm) above its bottom in front of it is in its way: a step of one block it may climb
	 *  (TryStartClimb), else a wall. */
	constexpr float StepTolerance = 6.f;
	/** No growing or climbing into a block over it: this much room (cm) is kept above its top, checked from this angle above
	 *  its sides (degrees: a wall it rolls against is not a roof). A climb takes this long at most (s). */
	constexpr float RoofGap = 3.f;
	constexpr float RoofFromDeg = 25.f;
	constexpr float ClimbMaxTime = 2.f;
	/** The snow trail behind it while it rolls: a soft puff every TrailEvery s (/ the effects density), TrailSize x its radius. */
	constexpr float TrailEvery = 0.07f;
	constexpr float TrailSize = 0.32f;
	static const FLinearColor TrailSnowColor(0.93f, 0.96f, 1.f);
	static const FLinearColor TrailIceColor(0.75f, 0.9f, 1.f);
	/** A penguin's height at size 1 (the puppet's scale inside). */
	constexpr float PenguinHeight = 112.f;
	/** The lumpy ball: bumps (and a few dents) in its shape, darker packed patches in its snow. */
	constexpr int32 BallLumps = 16;
	constexpr int32 BallPatches = 6;
	/** Its snow: the island's snow colour a touch brighter (it is fresh, packed snow); frozen: the island's ice colour. */
	static const FLinearColor BallSnowColor(0.86f, 0.92f, 0.98f);
	static const FLinearColor BallIceColor(0.45f, 0.8f, 1.f);

	/** How far something stuck in the ball pokes out where it is now (0 .. 1): rolled round under the ball it sinks into the
	 *  snow (it never sticks through the floor), and comes out again on the way up. */
	static float StuckOutShare(const FVector& Out)
	{
		return FMath::SmoothStep(0.f, 1.f, FMath::Clamp((static_cast<float>(Out.Z) + 0.8f) / 0.55f, 0.f, 1.f));
	}

	/** Where on the ball the Index-th penguin (or other thing) sticks out, in the ball's own frame: spread round the half toward
	 *  the camera (+Y; the spin round Y keeps it there), each a little different by the ball's seed. */
	static FVector StuckDirection(int32 Seed, int32 Index, bool bPenguin)
	{
		const float Golden = 2.39996323f;
		const float Angle = (bPenguin ? 0.6f : 2.2f) + Index * Golden + (Seed % 997) * 0.013f;
		const float Toward = bPenguin ? 0.55f + 0.1f * (Index % 3) : 0.22f + 0.08f * (Index % 3);
		const float Ring = FMath::Sqrt(FMath::Max(1.f - Toward * Toward, 0.f));
		return FVector(FMath::Cos(Angle) * Ring, Toward, FMath::Sin(Angle) * Ring);
	}

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

	/** The roll-up's size (diameter) at Alpha (0 .. 1): fast at first, slowing into its full size (pats show on top). */
	static float BuildDiameter(float Alpha)
	{
		const float A = FMath::Clamp(Alpha, 0.f, 1.f);
		const float Ease = 1.f - (1.f - A) * (1.f - A);
		return FMath::Lerp(GiantBuildStartSize, GiantStartSize, Ease);
	}

	/** Distance on the play plane from a point to a penguin's capsule (its segment), minus the capsule's radius. */
	static float DistanceToCapsule(const APenguinCharacter* Penguin, const FVector& Point)
	{
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const FVector P = Penguin->GetActorLocation();
		const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 30.f;
		const float Half = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 56.f;
		const float Lo = static_cast<float>(P.Z) - (Half - Radius);
		const float Hi = static_cast<float>(P.Z) + (Half - Radius);
		const float Z = FMath::Clamp(static_cast<float>(Point.Z), Lo, Hi);
		return FVector2D(static_cast<float>(Point.X - P.X), static_cast<float>(Point.Z) - Z).Size() - Radius;
	}
}

APinguGiantSnowball::APinguGiantSnowball()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	SetActorEnableCollision(false);

	// LAN: the server spawns and runs it; clients draw it from the replicated motion and state.
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(30.f);
	NetPriority = 2.f;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = SceneRoot;
	Ball = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Ball"));
	Ball->SetupAttachment(SceneRoot);
	Ball->bUseComplexAsSimpleCollision = false;
	Ball->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Ball->SetGenerateOverlapEvents(false);
	Ball->SetCanEverAffectNavigation(false);
	Ball->SetCastShadow(true);
}

void APinguGiantSnowball::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APinguGiantSnowball, Phase);
	DOREPLIFETIME(APinguGiantSnowball, Roller);
	DOREPLIFETIME(APinguGiantSnowball, Direction);
	DOREPLIFETIME(APinguGiantSnowball, BuildStart);
	DOREPLIFETIME(APinguGiantSnowball, Diameter);
	DOREPLIFETIME(APinguGiantSnowball, bFrozen);
	DOREPLIFETIME(APinguGiantSnowball, WallHits);
	DOREPLIFETIME(APinguGiantSnowball, Inside);
	DOREPLIFETIME(APinguGiantSnowball, PopSerial);
	DOREPLIFETIME(APinguGiantSnowball, BounceSerial);
	DOREPLIFETIME(APinguGiantSnowball, Motion);
	DOREPLIFETIME(APinguGiantSnowball, LookSeed);
	DOREPLIFETIME(APinguGiantSnowball, Stuck);
}

// ---------------------------------------------------------------------------- spawning, finding

APinguGiantSnowball* APinguGiantSnowball::BeginRollUp(APenguinCharacter* InRoller, int32 InSlot, float Side)
{
	using namespace PinguGiantSnowballPrivate;
	UWorld* World = InRoller ? InRoller->GetWorld() : nullptr;
	UPenguinMovementComponent* Move = InRoller ? InRoller->GetPenguinMovement() : nullptr;
	if (!World || World->GetNetMode() == NM_Client || !Move || !ABlockIsland::FindNear(World, InRoller->GetActorLocation()))
	{
		return nullptr;
	}
	const float Dir = Side >= 0.f ? 1.f : -1.f;
	const FVector Feet = InRoller->GetFeetLocation();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APinguGiantSnowball* Giant = World->SpawnActor<APinguGiantSnowball>(Feet, FRotator::ZeroRotator, Params);
	if (!Giant)
	{
		return nullptr;
	}
	// All set before its first replication.
	Giant->Roller = InRoller;
	Giant->Slot = InSlot;
	Giant->Direction = Dir;
	Giant->BuildStart = ServerNow(World);
	Giant->Diameter = GiantStartSize;
	Giant->ShownDiameter = GiantBuildStartSize;
	Giant->Phase = EPinguGiantPhase::Building;
	Giant->LookSeed = FMath::RandRange(1, 1 << 30); // (its own lumps)
	// The roller hangs still while it pats the ball (no moving, no other ability); a hit, a freeze or a knock-out ends that and
	// with it the roll-up (TickBuild).
	Move->SetHover(GiantBuildTime + 0.3f);
	InRoller->FaceDirection(Dir);
	Giant->TickBuild(0.f);
	{
		FPinguFXBroadcastScope Broadcast(InRoller);
		APinguFX::Sound(InRoller, EPinguSound::GiantRollUp);
		APinguFX::Puffs(InRoller, Giant->GetActorLocation(), 5, 18.f, 160.f, FVector::UpVector, 1.f, PuffColor);
	}
	UE_LOG(LogPingu, Display, TEXT("[GIANT] %s rolls up a giant snowball (slot %d, rolls %s)"), *InRoller->GetDisplayName(), InSlot,
		Dir > 0.f ? TEXT("right") : TEXT("left"));
	return Giant;
}

void APinguGiantSnowball::GetAll(const UWorld* World, TArray<APinguGiantSnowball*>& Out)
{
	Out.Reset();
	if (!World)
	{
		return;
	}
	for (TActorIterator<APinguGiantSnowball> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed())
		{
			Out.Add(*It);
		}
	}
}

void APinguGiantSnowball::DestroyAll(UWorld* World)
{
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	for (TActorIterator<APinguGiantSnowball> It(World); It; ++It)
	{
		It->Destroy(); // (EndPlay gives the penguins inside back their look and collision)
	}
}

APinguGiantSnowball* APinguGiantSnowball::FindHolding(const UWorld* World, const APenguinCharacter* Penguin)
{
	TArray<APinguGiantSnowball*> All;
	GetAll(World, All);
	for (APinguGiantSnowball* Giant : All)
	{
		if (Giant->HoldsPenguin(Penguin))
		{
			return Giant;
		}
	}
	return nullptr;
}

bool APinguGiantSnowball::HoldsPenguin(const APenguinCharacter* Penguin) const
{
	return Penguin && Inside.Contains(Penguin);
}

float APinguGiantSnowball::GetAge() const
{
	return static_cast<float>(PinguGiantSnowballPrivate::ServerNow(GetWorld()) - BuildStart);
}

float APinguGiantSnowball::GetBuildAlpha() const
{
	return Phase == EPinguGiantPhase::Building ? FMath::Clamp(GetAge() / PinguTuning::GiantBuildTime, 0.f, 1.f) : 1.f;
}

int32 APinguGiantSnowball::GetShownStuck() const
{
	int32 Count = 0;
	for (const TObjectPtr<UStaticMeshComponent>& Part : StuckParts)
	{
		Count += Part && Part->IsVisible() ? 1 : 0;
	}
	return Count;
}

int32 APinguGiantSnowball::GetShownInside() const
{
	int32 Count = 0;
	for (const TObjectPtr<USkeletalMeshComponent>& Puppet : Puppets)
	{
		Count += Puppet && Puppet->IsVisible() ? 1 : 0;
	}
	return Count;
}

namespace PinguGiantSnowballPrivate
{
static bool IsInReach(const APinguGiantSnowball* Giant, const FVector& Center, float Radius, const FVector& ConeDirection, float CosHalfAngle)
{
	FVector ToBall = Giant->GetCenter() - Center;
	ToBall.Y = 0.f;
	const float Dist = static_cast<float>(ToBall.Size());
	const float R = Giant->GetRadius();
	if (Dist - R > Radius)
	{
		return false;
	}
	if (CosHalfAngle <= -1.f || ConeDirection.IsNearlyZero() || Dist <= R)
	{
		return true;
	}
	// In the cone: the nearest point of the ball (a big ball half in the cone counts).
	const FVector Nearest = Giant->GetCenter() - ToBall / Dist * R * 0.7f;
	FVector ToNear = Nearest - Center;
	ToNear.Y = 0.f;
	return FVector::DotProduct(ToNear.GetSafeNormal(), ConeDirection.GetSafeNormal()) >= CosHalfAngle;
}
}

int32 APinguGiantSnowball::BurstInArea(UWorld* World, const FVector& Center, float Radius, APenguinCharacter* By, const TCHAR* What,
	const FVector& ConeDirection, float CosHalfAngle)
{
	if (!World || World->GetNetMode() == NM_Client)
	{
		return 0;
	}
	TArray<APinguGiantSnowball*> All;
	GetAll(World, All);
	int32 Count = 0;
	for (APinguGiantSnowball* Giant : All)
	{
		if (Giant->IsRolling() && PinguGiantSnowballPrivate::IsInReach(Giant, Center, Radius, ConeDirection, CosHalfAngle))
		{
			Giant->Burst(By, What);
			++Count;
		}
	}
	return Count;
}

int32 APinguGiantSnowball::FreezeInArea(UWorld* World, const FVector& Center, float Radius, const FVector& ConeDirection, float CosHalfAngle)
{
	if (!World || World->GetNetMode() == NM_Client)
	{
		return 0;
	}
	TArray<APinguGiantSnowball*> All;
	GetAll(World, All);
	int32 Count = 0;
	for (APinguGiantSnowball* Giant : All)
	{
		if (Giant->IsRolling() && !Giant->IsFrozen() && PinguGiantSnowballPrivate::IsInReach(Giant, Center, Radius, ConeDirection, CosHalfAngle))
		{
			Giant->Freeze(TEXT("frost breath"));
			++Count;
		}
	}
	return Count;
}

// ---------------------------------------------------------------------------- the tick

void APinguGiantSnowball::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(DeltaSeconds, 0.05f);
	// Its size as drawn and as it collides: the roll-up's, or following its size (a change animates, as a penguin's).
	if (Phase == EPinguGiantPhase::Building)
	{
		ShownDiameter = PinguGiantSnowballPrivate::BuildDiameter(GetBuildAlpha());
	}
	else
	{
		ShownDiameter = FMath::FInterpTo(ShownDiameter, Diameter, Dt, PinguTuning::GiantSizeSpeed);
		if (FMath::Abs(ShownDiameter - Diameter) < 0.3f)
		{
			ShownDiameter = Diameter;
		}
	}
	if (HasAuthority())
	{
		if (Phase == EPinguGiantPhase::Building)
		{
			TickBuild(Dt);
		}
		else if (Phase == EPinguGiantPhase::Rolling)
		{
			TickRoll(Dt);
		}
		if (!IsValid(this) || IsActorBeingDestroyed())
		{
			return;
		}
	}
	UpdateCollisionOfInside();
	TickLook(Dt);
}

void APinguGiantSnowball::TickBuild(float Dt)
{
	using namespace PinguGiantSnowballPrivate;
	APenguinCharacter* Penguin = Roller.Get();
	UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	// The roll-up breaks when its penguin is hit (the hover ends), frozen, knocked out or swallowed by another giant snowball.
	if (!Penguin || !Move || !Penguin->IsAlive() || Penguin->IsHidden() || Move->IsFrozen() || !Move->IsHovering())
	{
		CancelRollUp(TEXT("broken"));
		return;
	}
	const float Age = GetAge();
	if (Age >= GiantBuildTime)
	{
		Launch();
		return;
	}
	// Pats, one flipper and then the other (every machine plays the clip from the character's replicated action).
	PatTimer -= Dt;
	if (PatTimer <= 0.f)
	{
		PatTimer = PatEvery;
		Penguin->PlayAction((PatCount++ % 2) == 0 ? EPenguinClip::Swat : EPenguinClip::SwatL, 1.5f);
	}
	// In front of its feet, on the ground there, growing out from the penguin.
	const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
	const float D = BuildDiameter(Age / GiantBuildTime);
	const FVector Feet = Penguin->GetFeetLocation();
	const float X = static_cast<float>(Feet.X) + Direction * ((Capsule ? Capsule->GetScaledCapsuleRadius() : 30.f) + D * 0.5f + BuildGap);
	float Ground = static_cast<float>(Feet.Z);
	if (const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Feet))
	{
		float Found = 0.f;
		if (Island->FindGroundBelow(FVector(X, 0.f, Feet.Z + 30.f), 400.f, Found))
		{
			Ground = Found;
		}
	}
	SetActorLocation(FVector(X, 0.f, Ground + D * 0.5f));
	VX = 0.f;
	VZ = 0.f;
	PublishMotion(true);
}

void APinguGiantSnowball::Launch()
{
	using namespace PinguGiantSnowballPrivate;
	Phase = EPinguGiantPhase::Rolling;
	RollStart = ServerNow(GetWorld());
	ShownDiameter = Diameter = GiantStartSize;
	VX = Direction * GiantRollSpeed;
	VZ = 0.f;
	bGrounded = true;
	APenguinCharacter* Penguin = Roller.Get();
	if (Penguin)
	{
		// In front of its feet at its full size (it never starts touching its roller).
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const FVector Feet = Penguin->GetFeetLocation();
		const float X = static_cast<float>(Feet.X) + Direction * ((Capsule ? Capsule->GetScaledCapsuleRadius() : 30.f) + Diameter * 0.5f + BuildGap);
		SetActorLocation(FVector(X, 0.f, FMath::Max(static_cast<float>(GetActorLocation().Z), static_cast<float>(Feet.Z) + Diameter * 0.5f)));
		if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
		{
			Move->SetHover(0.f); // free again
		}
		Penguin->PlayAction(Penguin->PickNearFlipper(EPenguinClip::Toss, EPenguinClip::TossL), 1.3f);
	}
	FPinguFXBroadcastScope Broadcast(this);
	APinguFX::Sound(this, EPinguSound::Throw, 0.8f, 0.05f, 0.7f);
	APinguFX::GroundPuffs(this, GetActorLocation() - FVector(0.f, 0.f, GetRadius()), 1.2f);
	UE_LOG(LogPingu, Display, TEXT("[GIANT] rolls off %s at %s"), Direction > 0.f ? TEXT("right") : TEXT("left"), *GetActorLocation().ToCompactString());
}

void APinguGiantSnowball::LaunchForTest()
{
	if (HasAuthority() && Phase == EPinguGiantPhase::Building)
	{
		Launch();
	}
}

void APinguGiantSnowball::SetSizeForTest(float NewDiameter)
{
	if (HasAuthority())
	{
		const float Old = ShownDiameter;
		Diameter = FMath::Clamp(NewDiameter, PinguTuning::GiantMinSize, PinguTuning::GiantSafetyMaxSize);
		ShownDiameter = Diameter;
		// Its foot stays where it was (grown into the ground it would meet the floor as a wall).
		SetActorLocation(GetActorLocation() + FVector(0.f, 0.f, (Diameter - Old) * 0.5f));
	}
}

void APinguGiantSnowball::CancelRollUp(const TCHAR* Why)
{
	using namespace PinguGiantSnowballPrivate;
	if (!HasAuthority() || Phase != EPinguGiantPhase::Building)
	{
		return;
	}
	Phase = EPinguGiantPhase::Done;
	APenguinCharacter* Penguin = Roller.Get();
	if (Penguin)
	{
		UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		if (Move && Move->IsHovering() && Penguin->IsAlive() && !Penguin->IsHidden())
		{
			Move->SetHover(0.f);
		}
		// The ability counts as used: its whole cooldown from now (Linus: "u can cancel it while making but the ability counts as
		// used").
		if (UPenguinAbilityComponent* Abilities = Penguin->GetAbilities())
		{
			Abilities->RestartCooldown(Slot);
		}
	}
	{
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Puffs(this, GetActorLocation(), 8, 16.f + ShownDiameter * 0.2f, 220.f, FVector::UpVector, 1.f, PuffColor);
		APinguFX::Flakes(this, GetActorLocation(), 6, 260.f, PuffColor);
		APinguFX::Sound(this, EPinguSound::Crumble, 0.7f, 0.05f, 1.3f);
	}
	End(Why);
}

// ---------------------------------------------------------------------------- rolling (server)

bool APinguGiantSnowball::HitsWall(const ABlockIsland* Island, float X, float Z, float R, float Dir, FVector* OutHit) const
{
	// Along its front from just above its bottom to near its top: any block there is in its way (the lowest one found). A step
	// one block high it may climb (TryStartClimb); anything else turns it back like a wall.
	const float Climb = PinguGiantSnowballPrivate::StepTolerance;
	const float Step = FMath::Clamp(R * 0.25f, 10.f, 25.f);
	for (float H = -R + Climb; H <= R * 0.92f; H += Step)
	{
		const float Half = FMath::Sqrt(FMath::Max(R * R - H * H, 0.f));
		const FVector Point(X + Dir * (Half + 3.f), 0.f, Z + H);
		if (Island->IsSolidAt(Point))
		{
			if (OutHit)
			{
				*OutHit = Point;
			}
			return true;
		}
	}
	return false;
}

bool APinguGiantSnowball::FindSupport(const ABlockIsland* Island, float X, float Z, float R, float& OutZ) const
{
	// A ball on blocks: under each point of its bottom the top of the column there, plus how high the circle is above that point
	// - the highest of these holds it (it rolls smoothly down over a ledge's corner). Columns are scanned from just above its
	// bottom downward; a block it is already in (an ice wall rising under it) holds it up from its top.
	// Bigger than a block (2026-11-05, Linus: "when its bigger than an 1 block gap it ignores 1 block gaps and rolls over them"):
	// an empty cell with a block on both sides in its row holds it like a block - it rolls over a gap one block wide.
	const float Climb = PinguGiantSnowballPrivate::StepTolerance;
	const float Top = Z - R + Climb + 1.f;
	const int32 Samples = FMath::Clamp(FMath::CeilToInt(2.f * R / 20.f), 4, 40);
	const float Cell = Island->CellSize;
	const bool bBridge = 2.f * R > PinguTuning::GiantGapBridgeSize;
	bool bFound = false;
	float Best = -1.e9f;
	for (int32 Index = 0; Index <= Samples; ++Index)
	{
		const float U = FMath::Lerp(-0.95f * R, 0.95f * R, static_cast<float>(Index) / Samples);
		const FIntPoint Start = Island->WorldToCell(FVector(X + U, 0.f, Top));
		for (int32 Down = 0; Down < 64; ++Down)
		{
			const FIntPoint C(Start.X, Start.Y - Down);
			if (Island->IsSolidCell(C)
				|| (bBridge && Island->IsSolidCell(FIntPoint(C.X - 1, C.Y)) && Island->IsSolidCell(FIntPoint(C.X + 1, C.Y))))
			{
				const float Ground = static_cast<float>(Island->CellToWorld(C).Z) + Cell * 0.5f;
				const float Held = Ground + FMath::Sqrt(FMath::Max(R * R - U * U, 0.f));
				Best = FMath::Max(Best, Held);
				bFound = true;
				break;
			}
		}
	}
	OutZ = Best;
	return bFound;
}

bool APinguGiantSnowball::HasRoom(const ABlockIsland* Island, float X, float Z, float R) const
{
	// Its upper contour (from RoofFromDeg above its sides over the top), a little outside it, every ~15 cm (a block is 100).
	using namespace PinguGiantSnowballPrivate;
	const float Out = R + RoofGap;
	const int32 Points = FMath::Clamp(FMath::CeilToInt(UE_PI * Out / 15.f), 8, 160);
	const float From = FMath::DegreesToRadians(RoofFromDeg);
	const float To = UE_PI - From;
	for (int32 Index = 0; Index <= Points; ++Index)
	{
		const float A = FMath::Lerp(From, To, static_cast<float>(Index) / Points);
		if (Island->IsSolidAt(FVector(X + FMath::Cos(A) * Out, 0.f, Z + FMath::Sin(A) * Out)))
		{
			return false;
		}
	}
	return true;
}

bool APinguGiantSnowball::TryStartClimb(const ABlockIsland* Island, float X, float Z, float R, const FVector& Hit)
{
	// Linus: "it can go up 1 block when roling but when it went up 1 it will bounce off the next 1block higher tile and roll back
	// down". Only rolling on the ground, only once in a row (bClimbReady: again after it rolled down a step), only a step exactly
	// one block above the floor it rolls on, with room on top (no second block, no roof, no wall right there).
	if (!bGrounded || !bClimbReady || bClimbing)
	{
		return false;
	}
	const float Cell = Island->CellSize;
	const float Bottom = Z - R;
	const FIntPoint StepCell = Island->WorldToCell(Hit);
	const int32 FloorRow = Island->WorldToCell(FVector(X, 0.f, Bottom - 1.f)).Y;
	if (StepCell.Y != FloorRow + 1 || Island->IsSolidCell(FIntPoint(StepCell.X, StepCell.Y + 1)))
	{
		return false;
	}
	const FVector StepMiddle = Island->CellToWorld(StepCell);
	const float TopZ = static_cast<float>(StepMiddle.Z) + Cell * 0.5f;
	if (FMath::Abs(TopZ - Cell - Bottom) > 12.f)
	{
		return false; // not on the floor in front of it (in a dip, half sunk)
	}
	if (!HasRoom(Island, X, TopZ + R, R) || HitsWall(Island, X, TopZ + R, R, Direction))
	{
		return false;
	}
	bClimbing = true;
	bClimbReady = false;
	ClimbCell = StepCell;
	ClimbFaceX = static_cast<float>(StepMiddle.X) - Direction * Cell * 0.5f;
	ClimbTopZ = TopZ;
	ClimbFloorZ = TopZ;
	ClimbTime = 0.f;
	++Climbs;
	UE_LOG(LogPingu, Display, TEXT("[GIANT] climbs a step at %s (diameter %.0f)"), *FVector(X, 0.f, Z).ToCompactString(), 2.f * R);
	return true;
}

void APinguGiantSnowball::TickClimb(float Dt, float Speed, float R, float& X, float& Z)
{
	// Up the step's face (pressed against it), then forward over its corner (its middle R from the corner) until it is on top.
	// Wanting to go forward faster than GiantClimbSpeed lets it rise, it waits for the height (a big ball rolls over the corner).
	const float Budget = PinguTuning::GiantClimbSpeed * Dt;
	const float CurDx = (ClimbFaceX - X) * Direction;
	float NewDx = CurDx - Speed * Dt;
	const float Clamped = FMath::Max(NewDx, 0.f);
	const float NeedZ = NewDx >= R ? -1.e9f : ClimbTopZ + FMath::Sqrt(FMath::Max(R * R - Clamped * Clamped, 0.f));
	float NewZ = Z;
	if (NeedZ > Z)
	{
		NewZ = FMath::Min(NeedZ, Z + Budget);
		const float H = NewZ - ClimbTopZ;
		const float Allowed = H < 0.f ? R : FMath::Sqrt(FMath::Max(R * R - H * H, 0.f));
		// Never closer to the face than that height allows, never back (it may have touched the corner a little late).
		NewDx = FMath::Max(NewDx, FMath::Min(Allowed, CurDx));
	}
	X = ClimbFaceX - Direction * NewDx;
	Z = NewZ;
	ClimbTime += Dt;
	if (NewDx <= 0.5f)
	{
		bClimbing = false;
		Z = FMath::Max(Z, ClimbTopZ + R);
	}
}

void APinguGiantSnowball::TickRoll(float Dt)
{
	using namespace PinguGiantSnowballPrivate;
	UWorld* World = GetWorld();
	const double Now = ServerNow(World);
	if (Now - RollStart > GiantMaxLife)
	{
		Burst(nullptr, TEXT("melted (too old)"));
		return;
	}
	FVector Loc = GetActorLocation();
	float X = static_cast<float>(Loc.X);
	float Z = static_cast<float>(Loc.Z);
	const float R = GetRadius();
	const ABlockIsland* Island = ABlockIsland::FindNear(World, Loc);
	const float Speed = bFrozen ? GiantFrozenSpeed : GiantRollSpeed;

	// Sideways: up a step it may climb, on along its way, or back from a wall (the GiantWallHits-th one breaks it).
	bool bClimbedNow = false;
	if (bClimbing)
	{
		if (!Island || !Island->IsSolidCell(ClimbCell) || ClimbTime > ClimbMaxTime)
		{
			bClimbing = false; // the step went (broken, melted): it just drops
		}
		else
		{
			const float X0 = X;
			const float Z0 = Z;
			TickClimb(Dt, Speed, R, X, Z);
			Rolled += FMath::Abs(X - X0);
			VX = (X - X0) / FMath::Max(Dt, 1.e-3f);
			VZ = (Z - Z0) / FMath::Max(Dt, 1.e-3f);
			bClimbedNow = true;
		}
	}
	FVector Hit = FVector::ZeroVector;
	if (bClimbedNow)
	{
	}
	else if (Island && HitsWall(Island, X, Z, R, Direction, &Hit) && !TryStartClimb(Island, X, Z, R, Hit))
	{
		++WallHits;
		++BounceSerial;
		{
			FPinguFXBroadcastScope Broadcast(this);
			const FVector Front = FVector(X + Direction * R, 0.f, Z);
			APinguFX::Sound(this, EPinguSound::GiantBounce, FMath::Clamp(0.6f + R / 200.f, 0.6f, 1.f));
			APinguFX::Puffs(this, Front, 8, 14.f + R * 0.18f, 260.f, FVector(-Direction, 0.f, 0.4f), 0.8f, PuffColor);
			APinguFX::Shake(this, FMath::Clamp(R / 600.f, 0.08f, 0.3f));
		}
		UE_LOG(LogPingu, Display, TEXT("[GIANT] wall hit %d at %s"), WallHits, *Loc.ToCompactString());
		if (WallHits >= GiantWallHits)
		{
			Burst(nullptr, TEXT("third wall hit"));
			return;
		}
		Direction = -Direction;
	}
	else if (!bClimbing)
	{
		const float Dx = Direction * Speed * Dt;
		X += Dx;
		Rolled += FMath::Abs(Dx);
		// It grows as it rolls - up to 3 tiles (bigger only from hits). A block over it (2026-11-05, Linus: "the snowball doesnt
		// die when it grows with a roof over it but just keep the same size and once hes out he grows"): it keeps its size there
		// (grown into the roof, the roof was a wall in front of it on both sides: three wall hits at once, and it broke).
		if (!bFrozen && Diameter < GiantRollMaxSize)
		{
			const float Want = FMath::Min(GiantRollMaxSize, Diameter + GiantRollGrowPerCm * FMath::Abs(Dx));
			if (!Island || HasRoom(Island, X, Z - R + Want * 0.5f, Want * 0.5f))
			{
				Diameter = Want;
			}
			else
			{
				RoofHeld += FMath::Abs(Dx);
			}
		}
	}
	if (!bClimbedNow)
	{
		VX = bClimbing ? 0.f : Direction * Speed;
	}

	// Down: on the ground, falling off a ledge, or held on the step it climbs.
	if (bClimbing)
	{
		bGrounded = true;
	}
	else
	{
		float Support = 0.f;
		const bool bSupport = Island && FindSupport(Island, X, Z, R, Support);
		if (bSupport && Support >= Z - 0.5f)
		{
			if (!bGrounded && VZ < -600.f)
			{
				FPinguFXBroadcastScope Broadcast(this);
				APinguFX::GroundPuffs(this, FVector(X, 0.f, Support - R), FMath::Clamp(R / 80.f, 0.6f, 2.f));
				APinguFX::Sound(this, EPinguSound::LandHard, 0.8f, 0.05f, 0.7f);
				APinguFX::Shake(this, FMath::Clamp(R / 500.f, 0.1f, 0.35f));
			}
			Z = Support;
			VZ = 0.f;
			bGrounded = true;
		}
		else
		{
			VZ -= GiantGravity * Dt;
			const float Next = Z + VZ * Dt;
			if (bSupport && Next <= Support)
			{
				Z = Support;
				if (VZ < -600.f)
				{
					FPinguFXBroadcastScope Broadcast(this);
					APinguFX::GroundPuffs(this, FVector(X, 0.f, Support - R), FMath::Clamp(R / 80.f, 0.6f, 2.f));
					APinguFX::Sound(this, EPinguSound::LandHard, 0.8f, 0.05f, 0.7f);
					APinguFX::Shake(this, FMath::Clamp(R / 500.f, 0.1f, 0.35f));
				}
				VZ = 0.f;
				bGrounded = true;
			}
			else
			{
				Z = Next;
				bGrounded = false;
			}
		}
	}
	// Rolled down a step (or fell): it may climb again.
	if (!bClimbReady && !bClimbing && Z - R < ClimbFloorZ - (Island ? Island->CellSize : 100.f) * 0.5f)
	{
		bClimbReady = true;
	}
	SetActorLocation(FVector(X, 0.f, Z));

	// Into the sea with everybody inside.
	const float SeaLevel = Island ? Island->SeaLevel : -1650.f;
	if (Z < SeaLevel + 60.f)
	{
		Sink();
		return;
	}

	// What it meets.
	TouchPenguins();
	if (Phase != EPinguGiantPhase::Rolling) { return; }
	TouchShots();
	if (Phase != EPinguGiantPhase::Rolling) { return; }
	TouchBubbles();
	TouchOthers();
	if (Phase != EPinguGiantPhase::Rolling) { return; }
	// A falling iceberg crushes it.
	for (TActorIterator<AIcebergDrop> It(World); It; ++It)
	{
		if (It->IsFalling() && It->GetVelocity().Z < -50.f && It->Overlaps(GetActorLocation(), GetRadius()))
		{
			Burst(It->GetDropOwner(), TEXT("iceberg"));
			return;
		}
	}

	// The penguins inside keep hanging still where they were swallowed (renewed now and then; a LAN player's own penguin too).
	HoverTimer -= Dt;
	if (HoverTimer <= 0.f)
	{
		HoverTimer = HoverRenew;
		for (const TObjectPtr<APenguinCharacter>& Each : Inside)
		{
			APenguinCharacter* Penguin = Each.Get();
			if (Penguin && Penguin->IsAlive() && Penguin->GetPenguinMovement())
			{
				Penguin->GetPenguinMovement()->SetHover(HoverHold);
			}
		}
	}
	PublishMotion(false);
}

void APinguGiantSnowball::TouchPenguins()
{
	using namespace PinguGiantSnowballPrivate;
	const FVector C = GetActorLocation();
	const float R = GetRadius();
	TArray<APenguinCharacter*> Touched;
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		if (!Penguin->IsAlive() || Penguin->IsHidden() || Inside.Contains(Penguin) || DistanceToCapsule(Penguin, C) > R - TouchDepth)
		{
			continue;
		}
		Touched.Add(Penguin);
	}
	for (APenguinCharacter* Penguin : Touched)
	{
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const UPenguinTraitComponent* Traits = Penguin->GetTraits();
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		// Hot feet melt it.
		if (Traits && Traits->HasTrait(EPinguTrait::HotFeet))
		{
			Burst(Penguin, TEXT("hot feet"));
			return;
		}
		// A belly stomp coming down on it smashes it.
		if (Move && Move->IsStomping() && Penguin->GetActorLocation().Z > C.Z)
		{
			Burst(Penguin, TEXT("belly stomp"));
			return;
		}
		// A penguin far bigger than it (grown) squashes it.
		if (Capsule && Capsule->GetScaledCapsuleHalfHeight() * 2.f > GiantCrushHeight * GetDiameter())
		{
			Burst(Penguin, TEXT("a giant penguin"));
			return;
		}
		// Ice Touch freezes it.
		if (!bFrozen && Traits && Traits->HasTrait(EPinguTrait::IceTouch))
		{
			Freeze(TEXT("Ice Touch"));
		}
		if (bFrozen || Penguin->IsShielded())
		{
			Shove(Penguin);
			continue;
		}
		Swallow(Penguin);
	}
}

void APinguGiantSnowball::Swallow(APenguinCharacter* Penguin)
{
	using namespace PinguGiantSnowballPrivate;
	Inside.Add(Penguin);
	Penguin->RememberAttacker(Roller.Get()); // (its sea is the roller's knock-out; its own ball: nobody's)
	if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
	{
		Move->SetHover(HoverHold); // hangs still where it was (no control, no ability) until it comes out
	}
	Penguin->SetActorHiddenInGame(true); // (replicates)
	Penguin->SetActorEnableCollision(false);
	NoCollision.AddUnique(Penguin);
	{
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::GiantGulp);
		APinguFX::Puffs(this, Penguin->GetActorLocation(), 8, 22.f, 240.f, FVector::UpVector, 1.f, PuffColor);
		APinguFX::Flakes(this, Penguin->GetActorLocation(), 5, 300.f, PuffColor);
	}
	++PopSerial;
	UE_LOG(LogPingu, Display, TEXT("[GIANT] swallowed %s (%d inside)"), *Penguin->GetDisplayName(), Inside.Num());
}

void APinguGiantSnowball::Shove(APenguinCharacter* Penguin)
{
	using namespace PinguGiantSnowballPrivate;
	const double Now = ServerNow(GetWorld());
	if (const double* Last = Shoved.Find(Penguin))
	{
		if (Now - *Last < GiantShoveEvery)
		{
			return;
		}
	}
	Shoved.Add(Penguin, Now);
	const float Side = FMath::Abs(Penguin->GetActorLocation().X - GetActorLocation().X) > 5.f
		? FMath::Sign(static_cast<float>(Penguin->GetActorLocation().X - GetActorLocation().X)) : Direction;
	const FVector2D Push(Side * GiantShovePush, GiantShoveLift);
	if (Penguin->IsShielded())
	{
		// The shell blocks every hit; the bubble is pushed aside all the same (the ball pushes nothing back).
		if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
		{
			Move->ApplyKnockback(Push, 0.f);
		}
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::ShieldBlock, 0.8f);
		return;
	}
	FPenguinHit Hit;
	Hit.Instigator = Roller.Get();
	Hit.Source = EPenguinAbility::GiantSnowball;
	Hit.Knockback = Push;
	Hit.Stun = GiantShoveStun;
	Hit.Location = Penguin->GetActorLocation();
	Hit.SourceSize = 1.f;
	Penguin->ApplyPenguinHit(Hit);
	FPinguFXBroadcastScope Broadcast(this);
	APinguFX::Sound(this, EPinguSound::IcicleHit, 0.7f, 0.05f, 0.7f);
	APinguFX::Sparkles(this, Penguin->GetActorLocation(), 6, 40.f, FrozenColor);
}

void APinguGiantSnowball::TouchShots()
{
	using namespace PinguGiantSnowballPrivate;
	const FVector C = GetActorLocation();
	const float R = GetRadius();
	TArray<APenguinProjectile*> Shots;
	APenguinProjectile::GetAll(GetWorld(), Shots);
	for (APenguinProjectile* Shot : Shots)
	{
		if (!IsValid(Shot) || Shot->IsActorBeingDestroyed())
		{
			continue;
		}
		const EPenguinProjectileKind Kind = Shot->GetKind();
		// Pegs and pads stay in their wall; a hook on its way back or riding on a penguin is no shot any more.
		const bool bLoose = Kind == EPenguinProjectileKind::FishHook ? Shot->IsThreat() : Shot->CanBeSucked();
		FVector D = Shot->GetActorLocation() - C;
		D.Y = 0.f;
		if (!bLoose || D.Size() > R + Shot->GetRadius())
		{
			continue;
		}
		FPinguFXBroadcastScope Broadcast(this);
		switch (Kind)
		{
		case EPenguinProjectileKind::FishHook:
			Shot->Shatter(); // nothing to hook: it lets go and reels in
			break;
		case EPenguinProjectileKind::Snowball:
			if (!bFrozen)
			{
				// Snowballs make it bigger - big ones a lot, small ones a little, a combo's more (past the rolling cap too).
				APenguinCharacter* Thrower = Shot->GetShooter();
				const UPenguinAbilityComponent* ThrowerAbilities = Thrower ? Thrower->GetAbilities() : nullptr;
				const float Combo = ThrowerAbilities ? GetSnowballComboScale(ThrowerAbilities->GetNextSnowballComboHit()) : 1.f;
				const float Grow = GiantSnowballGrow * Shot->GetSizeScale() * Combo * FMath::Max(Shot->GetStrength(), 0.f);
				AddSize(Grow, true);
				++SizeHits;
				AddStuck(StuckSnowball); // (it stays stuck in it, poking out)
				APinguFX::Sound(this, EPinguSound::SnowballHit, 0.9f, 0.05f, 0.75f);
				UE_LOG(LogPingu, Display, TEXT("[GIANT] snowball (size %.2f, combo x%.2f) grows it by %.0f cm to %.0f"), Shot->GetSizeScale(), Combo, Grow, Diameter);
			}
			Shot->Shatter();
			break;
		case EPenguinProjectileKind::ShrinkRay:
		case EPenguinProjectileKind::GrowRay:
			if (!bFrozen)
			{
				ScaleSize(Kind == EPenguinProjectileKind::GrowRay ? PinguGrowRayFactor : PinguShrinkRayFactor);
			}
			Shot->Shatter();
			break;
		case EPenguinProjectileKind::IceBolt:
			Freeze(TEXT("ice gun"));
			Shot->Shatter();
			break;
		case EPenguinProjectileKind::Bomb:
		case EPenguinProjectileKind::Icicle:
			if (bFrozen)
			{
				// Frozen it swallows nothing: an icicle breaks on it, a bomb goes off on it (and that blast breaks it).
				if (Kind == EPenguinProjectileKind::Bomb)
				{
					Shot->Detonate(0.f);
				}
				else
				{
					Shot->Shatter();
				}
				break;
			}
			{
				FStoredShot Stored;
				Stored.Kind = Kind;
				Stored.Shooter = Shot->GetShooter();
				Stored.Size = Shot->GetSizeScale();
				Items.Add(Stored);
				AddStuck(Kind == EPenguinProjectileKind::Bomb ? StuckBomb : StuckIcicle);
				APinguFX::Sound(this, EPinguSound::GiantGulp, 0.6f, 0.05f, 1.4f);
				APinguFX::Puffs(this, Shot->GetActorLocation(), 4, 14.f, 160.f, FVector::UpVector, 1.f, PuffColor);
				UE_LOG(LogPingu, Display, TEXT("[GIANT] swallowed a %s (%d items inside)"), *UEnum::GetValueAsString(Kind), GetItemsInside());
				Shot->Destroy();
			}
			break;
		default:
			break;
		}
		if (Phase != EPinguGiantPhase::Rolling)
		{
			return;
		}
	}
}

void APinguGiantSnowball::TouchBubbles()
{
	using namespace PinguGiantSnowballPrivate;
	const FVector C = GetActorLocation();
	const float R = GetRadius();
	TArray<APinguPickup*> All;
	APinguPickup::GetAll(GetWorld(), All);
	for (APinguPickup* Pickup : All)
	{
		if (!IsValid(Pickup) || Pickup->IsActorBeingDestroyed() || Pickup->IsOnHook() || Pickup->GetAbility() == EPenguinAbility::None)
		{
			continue;
		}
		FVector D = Pickup->GetActorLocation() - C;
		D.Y = 0.f;
		if (D.Size() > R + APinguPickup::BubbleRadius * 0.7f)
		{
			continue;
		}
		if (bFrozen)
		{
			Pickup->Push(D.GetSafeNormal() * 500.f + FVector(0.f, 0.f, 300.f));
			continue;
		}
		Bubbles.Add(Pickup->GetAbility());
		AddStuck(static_cast<uint8>(StuckBubble + static_cast<int32>(Pickup->GetAbility())));
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::GiantGulp, 0.6f, 0.05f, 1.6f);
		Pickup->Destroy();
	}
}

void APinguGiantSnowball::TouchOthers()
{
	TArray<APinguGiantSnowball*> All;
	GetAll(GetWorld(), All);
	for (APinguGiantSnowball* Other : All)
	{
		if (Other == this || !Other->IsRolling() || Phase != EPinguGiantPhase::Rolling)
		{
			continue;
		}
		FVector D = Other->GetActorLocation() - GetActorLocation();
		D.Y = 0.f;
		if (D.Size() > GetRadius() + Other->GetRadius() - 10.f)
		{
			continue;
		}
		// The bigger one acts (of two alike, the older one).
		const bool bWeLead = Diameter > Other->Diameter + 0.5f || (FMath::Abs(Diameter - Other->Diameter) <= 0.5f && BuildStart <= Other->BuildStart);
		if (!bWeLead)
		{
			continue;
		}
		if (bFrozen || Other->bFrozen)
		{
			// Ice swallows nothing: they bounce apart (no wall hit).
			const float Side = D.X >= 0.f ? 1.f : -1.f;
			Direction = -Side;
			Other->Direction = Side;
			++BounceSerial;
			++Other->BounceSerial;
			FPinguFXBroadcastScope Broadcast(this);
			APinguFX::Sound(this, EPinguSound::GiantBounce);
			continue;
		}
		// Two giant snowballs: the bigger takes the other in - their sizes add up as balls do (by area), everybody and everything
		// inside it comes along.
		const float NewSize = FMath::Min(FMath::Sqrt(Diameter * Diameter + Other->Diameter * Other->Diameter), PinguTuning::GiantSafetyMaxSize);
		Diameter = NewSize;
		for (const TObjectPtr<APenguinCharacter>& Each : Other->Inside)
		{
			if (Each)
			{
				Inside.AddUnique(Each);
				NoCollision.AddUnique(Each.Get());
			}
		}
		Other->Inside.Reset();
		Other->NoCollision.Reset();
		Items.Append(Other->Items);
		Bubbles.Append(Other->Bubbles);
		Other->Items.Reset();
		Other->Bubbles.Reset();
		for (const uint8 Code : Other->Stuck)
		{
			AddStuck(Code);
		}
		Other->Stuck.Reset();
		++PopSerial;
		{
			FPinguFXBroadcastScope Broadcast(this);
			APinguFX::Sound(this, EPinguSound::GiantGulp, 1.f, 0.05f, 0.7f);
			APinguFX::Puffs(this, Other->GetActorLocation(), 10, 30.f, 260.f, FVector::UpVector, 1.f, PinguGiantSnowballPrivate::PuffColor);
		}
		UE_LOG(LogPingu, Display, TEXT("[GIANT] two giant snowballs merged: %.0f cm"), Diameter);
		Other->Phase = EPinguGiantPhase::Done;
		Other->End(TEXT("merged into a bigger one"));
	}
}

void APinguGiantSnowball::AddStuck(uint8 Code)
{
	if (!HasAuthority())
	{
		return;
	}
	if (Code == StuckSnowball)
	{
		int32 Snowballs = 0;
		for (const uint8 Each : Stuck)
		{
			Snowballs += Each == StuckSnowball ? 1 : 0;
		}
		if (Snowballs >= MaxStuckSnowballs)
		{
			return;
		}
	}
	Stuck.Add(Code);
}

void APinguGiantSnowball::AddSize(float DiameterDelta, bool bPop)
{
	if (!HasAuthority())
	{
		return;
	}
	Diameter = FMath::Clamp(Diameter + DiameterDelta, PinguTuning::GiantMinSize, PinguTuning::GiantSafetyMaxSize);
	if (bPop)
	{
		++PopSerial;
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Puffs(this, GetActorLocation() + FVector(0.f, 60.f, 0.f), 10, 18.f + GetRadius() * 0.15f, 300.f, FVector::UpVector, 1.f,
			PinguGiantSnowballPrivate::PuffColor);
	}
}

void APinguGiantSnowball::ScaleSize(float Factor)
{
	if (!HasAuthority() || Factor <= 0.f)
	{
		return;
	}
	Diameter = FMath::Clamp(Diameter * Factor, PinguTuning::GiantMinSize, PinguTuning::GiantSafetyMaxSize);
	++PopSerial;
	++SizeHits;
	const bool bGrow = Factor > 1.f;
	FPinguFXBroadcastScope Broadcast(this);
	const FLinearColor Color = bGrow ? FLinearColor(0.45f, 1.f, 0.35f) : FLinearColor(0.85f, 0.4f, 1.f);
	APinguFX::Sound(this, bGrow ? EPinguSound::Grow : EPinguSound::Shrink);
	APinguFX::Sparkles(this, GetActorLocation(), 12, GetRadius() * 0.8f, Color);
	APinguFX::Ring(this, GetActorLocation(), FMath::Max(Diameter, GetDiameter()) * 0.6f, Color, 0.3f);
	UE_LOG(LogPingu, Display, TEXT("[GIANT] %s ray: %.0f cm"), bGrow ? TEXT("grow") : TEXT("shrink"), Diameter);
}

void APinguGiantSnowball::Freeze(const TCHAR* Why)
{
	if (!HasAuthority() || bFrozen || Phase == EPinguGiantPhase::Done)
	{
		return;
	}
	bFrozen = true;
	FPinguFXBroadcastScope Broadcast(this);
	APinguFX::Sound(this, EPinguSound::Freeze);
	APinguFX::IceBurst(this, GetActorLocation() + FVector(0.f, GetRadius() * 0.6f, 0.f), FMath::Max(GetDiameter() / 120.f, 0.5f) * 0.5f,
		PinguGiantSnowballPrivate::FrozenColor);
	APinguFX::Sparkles(this, GetActorLocation(), 12, GetRadius(), PinguGiantSnowballPrivate::FrozenColor);
	UE_LOG(LogPingu, Display, TEXT("[GIANT] frozen (%s)"), Why);
}

void APinguGiantSnowball::Burst(APenguinCharacter* By, const TCHAR* Why)
{
	using namespace PinguGiantSnowballPrivate;
	if (!HasAuthority())
	{
		return;
	}
	if (Phase == EPinguGiantPhase::Building)
	{
		CancelRollUp(Why);
		return;
	}
	if (Phase != EPinguGiantPhase::Rolling)
	{
		return;
	}
	Phase = EPinguGiantPhase::Done;
	const FVector C = GetActorLocation();
	const float R = GetRadius();
	{
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::GiantBurst, FMath::Clamp(0.7f + R / 300.f, 0.7f, 1.f));
		APinguFX::Puffs(this, C, 16, 24.f + R * 0.25f, 420.f + R, FVector::UpVector, 1.f, PuffColor);
		APinguFX::Flakes(this, C, 12, 500.f + R, PuffColor);
		APinguFX::Ring(this, C, R * 1.4f, FLinearColor::White, 0.35f);
		if (bFrozen)
		{
			APinguFX::IceBurst(this, C + FVector(0.f, R * 0.5f, 0.f), FMath::Max(R / 60.f, 0.6f), FrozenColor);
		}
		APinguFX::Shake(this, FMath::Clamp(0.15f + R / 500.f, 0.2f, 0.6f));
	}
	UE_LOG(LogPingu, Display, TEXT("[GIANT] burst (%s%s%s): %d penguins, %d shots, %d bubbles come out at %s"), Why, By ? TEXT(", by ") : TEXT(""),
		By ? *By->GetDisplayName() : TEXT(""), Inside.Num(), Items.Num(), Bubbles.Num(), *C.ToCompactString());
	Release(C, false);
	// What it swallowed flies out, fanned over the top (a bomb with a fresh fuse, an icicle as deadly as ever).
	UWorld* World = GetWorld();
	const int32 Count = Items.Num() + Bubbles.Num();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float T = Count > 1 ? static_cast<float>(Index) / (Count - 1) : 0.5f;
		const float Angle = FMath::DegreesToRadians(FMath::Lerp(30.f, 150.f, T));
		const FVector Dir(FMath::Cos(Angle), 0.f, FMath::Sin(Angle));
		const FVector From = C + Dir * R * 0.6f;
		if (Index < Items.Num())
		{
			const FStoredShot& Stored = Items[Index];
			if (APenguinProjectile* Shot = APenguinProjectile::Launch(World, Stored.Kind, Stored.Shooter.Get(), From, Dir * GiantItemSpeed))
			{
				if (!FMath::IsNearlyEqual(Stored.Size, Shot->GetSizeScale(), 0.01f) && Shot->GetSizeScale() > 0.f)
				{
					Shot->Rescale(Stored.Size / Shot->GetSizeScale());
				}
			}
		}
		else
		{
			APinguPickup::Spawn(World, Bubbles[Index - Items.Num()], From, Dir * 600.f);
		}
	}
	Items.Reset();
	Bubbles.Reset();
	End(Why);
}

void APinguGiantSnowball::Sink()
{
	using namespace PinguGiantSnowballPrivate;
	Phase = EPinguGiantPhase::Done;
	const FVector C = GetActorLocation();
	{
		FPinguFXBroadcastScope Broadcast(this);
		const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), C);
		const float Sea = Island ? Island->SeaLevel : -1650.f;
		APinguFX::Splash(this, FVector(C.X, 0.f, Sea + 50.f));
		APinguFX::Sound(this, EPinguSound::Splash);
	}
	UE_LOG(LogPingu, Display, TEXT("[GIANT] rolled into the sea with %d penguins inside at %s"), Inside.Num(), *C.ToCompactString());
	Release(C, true);
	End(TEXT("sea"));
}

void APinguGiantSnowball::Release(const FVector& Where, bool bSea)
{
	using namespace PinguGiantSnowballPrivate;
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Where);
	const float Sea = Island ? Island->SeaLevel : -1650.f;
	const float R = GetRadius();
	const int32 Count = Inside.Num();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		APenguinCharacter* Penguin = Inside[Index].Get();
		if (!Penguin || !Penguin->IsAlive())
		{
			continue;
		}
		UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const float CapsuleRadius = Capsule ? Capsule->GetScaledCapsuleRadius() : 30.f;
		const float Half = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 56.f;
		Penguin->SetActorHiddenInGame(false);
		Penguin->SetActorEnableCollision(true);
		if (Move)
		{
			Move->SetHover(0.f);
		}
		if (bSea)
		{
			// Down into the water where the ball went in: the sea takes them as it takes anybody (Second Chance may save one); the
			// knock-out is the roller's (its own ball: its own doing, as the snow bomb's).
			Penguin->RememberAttacker(Roller.Get());
			Penguin->NetTeleport(FVector(Where.X + (Index - (Count - 1) * 0.5f) * 40.f, 0.f, Sea + 40.f), FVector(0.f, 0.f, -300.f), true);
			continue;
		}
		// Out where it broke: side by side on its floor, popped up and apart.
		const float Spread = Count > 1 ? FMath::Clamp((Index - (Count - 1) * 0.5f) * 70.f, -R * 0.8f, R * 0.8f) : 0.f;
		FVector Spot(Where.X + Spread, 0.f, Where.Z - R + Half + 4.f);
		if (Island && !Island->CanFitCapsule(Spot, CapsuleRadius, Half))
		{
			Spot = FVector(Where.X + Spread, 0.f, Where.Z);
			if (!Island->CanFitCapsule(Spot, CapsuleRadius, Half))
			{
				Spot = FVector(Where.X, 0.f, Where.Z);
			}
		}
		const float Side = Count > 1 ? (Spread >= 0.f ? 1.f : -1.f) : -Direction;
		Penguin->NetTeleport(Spot, FVector(Side * GiantPopSide, 0.f, GiantPopUp), true);
	}
	Inside.Reset();
	NoCollision.Reset();
	Shoved.Reset();
}

void APinguGiantSnowball::End(const TCHAR* Why)
{
	LastEnd = Why;
	LastEndAt = GetActorLocation();
	Phase = EPinguGiantPhase::Done;
	Destroy();
}

void APinguGiantSnowball::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Whoever is still inside (a new round took it away, or a test): back to normal where they were.
	for (const TObjectPtr<APenguinCharacter>& Each : Inside)
	{
		APenguinCharacter* Penguin = Each.Get();
		if (!IsValid(Penguin))
		{
			continue;
		}
		if (HasAuthority())
		{
			Penguin->SetActorHiddenInGame(false);
			if (Penguin->IsAlive() && Penguin->GetPenguinMovement())
			{
				Penguin->GetPenguinMovement()->SetHover(0.f);
			}
		}
	}
	for (const TWeakObjectPtr<APenguinCharacter>& Each : NoCollision)
	{
		APenguinCharacter* Penguin = Each.Get();
		if (IsValid(Penguin) && !Penguin->IsKnockedOut())
		{
			Penguin->SetActorEnableCollision(true);
		}
	}
	NoCollision.Reset();
	Super::EndPlay(EndPlayReason);
}

void APinguGiantSnowball::PublishMotion(bool bForce)
{
	FPinguGiantMotion Next;
	const FVector Loc = GetActorLocation();
	Next.X = static_cast<float>(Loc.X);
	Next.Z = static_cast<float>(Loc.Z);
	Next.VX = Phase == EPinguGiantPhase::Rolling ? VX : 0.f;
	Next.VZ = bGrounded && !bClimbing ? 0.f : VZ;
	Next.Time = PinguGiantSnowballPrivate::ServerNow(GetWorld());
	(void)bForce;
	Motion = Next;
}

void APinguGiantSnowball::OnRep_Motion()
{
	// (TickLook runs the copy on from the newest motion.)
}

// ---------------------------------------------------------------------------- every machine

void APinguGiantSnowball::UpdateCollisionOfInside()
{
	// The penguins inside never block or touch anything (their look hides with the actor's replicated hidden flag).
	for (const TObjectPtr<APenguinCharacter>& Each : Inside)
	{
		APenguinCharacter* Penguin = Each.Get();
		if (IsValid(Penguin) && Penguin->GetActorEnableCollision())
		{
			Penguin->SetActorEnableCollision(false);
			NoCollision.AddUnique(Penguin);
		}
	}
	for (int32 Index = NoCollision.Num() - 1; Index >= 0; --Index)
	{
		APenguinCharacter* Penguin = NoCollision[Index].Get();
		if (!IsValid(Penguin))
		{
			NoCollision.RemoveAt(Index);
			continue;
		}
		if (!Inside.Contains(Penguin))
		{
			if (!Penguin->IsKnockedOut())
			{
				Penguin->SetActorEnableCollision(true);
			}
			NoCollision.RemoveAt(Index);
		}
	}
}

void APinguGiantSnowball::TickLook(float Dt)
{
	using namespace PinguGiantSnowballPrivate;
	if (!bLookReady)
	{
		bLookReady = true;
		// The island's own snow and ice (their sparkle, mottle and soft shadows; the presets' quality switches inside them).
		const auto Make = [this](const TCHAR* Path, const FLinearColor& Color, float Roughness, float Emissive) -> UMaterialInstanceDynamic*
		{
			UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
			if (!Parent)
			{
				Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Prop.M_Prop"), nullptr, LOAD_Quiet | LOAD_NoWarn);
			}
			if (!Parent)
			{
				return nullptr;
			}
			UMaterialInstanceDynamic* Made = UMaterialInstanceDynamic::Create(Parent, this);
			Made->SetVectorParameterValue(TEXT("Color"), Color);
			Made->SetScalarParameterValue(TEXT("Roughness"), Roughness);
			Made->SetScalarParameterValue(TEXT("Emissive"), Emissive);
			return Made;
		};
		SnowMaterial = Make(TEXT("/Game/Environment/M_Block_Snow.M_Block_Snow"), BallSnowColor, 0.75f, 0.f);
		IceMaterial = Make(TEXT("/Game/Environment/M_Block_Ice.M_Block_Ice"), BallIceColor, 0.08f, 0.25f);
		SeenPop = PopSerial;
		SeenBounce = BounceSerial;
	}
	// The mesh: built once, again when its size has changed a lot (the snow's grain stays the island's size) or the preset.
	{
		const int32 Detail = UPinguSettings::GetFXDensity() < 0.5f ? 0 : (APinguFX::Get(this) && APinguFX::Get(this)->UsesUltraEffects() ? 2 : 1);
		const float For = FMath::Max(ShownDiameter, 10.f);
		if (Detail != MeshDetail || MeshBuiltFor <= 0.f || For > MeshBuiltFor * 1.35f || For < MeshBuiltFor / 1.35f)
		{
			MeshDetail = Detail;
			BuildBallMesh(For);
		}
		UMaterialInterface* Want = bFrozen && IceMaterial ? IceMaterial.Get() : SnowMaterial.Get();
		if (Want && Ball->GetMaterial(0) != Want)
		{
			Ball->SetMaterial(0, Want);
		}
	}

	// Clients: run it on from the newest motion and blend in where the server has it.
	const FVector Before = GetActorLocation();
	if (!HasAuthority() && Motion.Time > 0.0)
	{
		const float Since = FMath::Clamp(static_cast<float>(ServerNow(GetWorld()) - Motion.Time), 0.f, 0.3f);
		const FVector Target(Motion.X + Motion.VX * Since, 0.f, Motion.Z + Motion.VZ * Since);
		FVector Now = Before;
		if (FVector::Dist(Now, Target) > SnapDistance || Before.IsNearlyZero())
		{
			Now = Target;
		}
		else
		{
			Now = FMath::Lerp(Now, Target, 1.f - FMath::Exp(-BlendRate * Dt));
		}
		SetActorLocation(Now);
	}

	// It turns as it rolls (the way it really goes on this machine).
	const float R = FMath::Max(GetRadius(), 5.f);
	const float Moved = static_cast<float>(GetActorLocation().X - PrevLookX);
	if (Phase == EPinguGiantPhase::Rolling && FMath::Abs(Moved) < 200.f)
	{
		SpinDeg += FMath::RadiansToDegrees(Moved / R);
		// Up a step's face it rolls on the spot: it turns on with the way up.
		const float Rose = static_cast<float>(GetActorLocation().Z - PrevLookZ);
		if (Rose > FMath::Abs(Moved) && Rose < 200.f)
		{
			SpinDeg += Direction * FMath::RadiansToDegrees((Rose - FMath::Abs(Moved)) / R);
		}
	}
	else if (Phase == EPinguGiantPhase::Building)
	{
		// Rolled up in place: it turns a little toward its way with each pat.
		SpinDeg += Direction * FMath::RadiansToDegrees(Dt * 1.2f);
	}
	PrevLookX = static_cast<float>(GetActorLocation().X);
	PrevLookZ = static_cast<float>(GetActorLocation().Z);

	// Pops (a size hit, a swallow) and bounces: a squash on every machine.
	if (SeenPop != PopSerial)
	{
		SeenPop = PopSerial;
		PopTime = 0.f;
	}
	if (SeenBounce != BounceSerial)
	{
		SeenBounce = BounceSerial;
		BounceTime = 0.f;
	}
	PopTime += Dt;
	BounceTime += Dt;
	float ScaleX = 1.f;
	float ScaleZ = 1.f;
	if (PopTime < PinguGiantSnowballPrivate::PopTime)
	{
		const float P = PopTime / PinguGiantSnowballPrivate::PopTime;
		const float Wobble = FMath::Sin(P * UE_PI * 2.5f) * (1.f - P);
		ScaleX *= 1.f + PopAmount * Wobble;
		ScaleZ *= 1.f + PopAmount * Wobble;
	}
	if (BounceTime < PinguGiantSnowballPrivate::BounceTime)
	{
		const float P = BounceTime / PinguGiantSnowballPrivate::BounceTime;
		const float Squash = FMath::Sin(P * UE_PI) * (1.f - P * 0.5f);
		ScaleX *= 1.f - BounceSquash * Squash;
		ScaleZ *= 1.f + BounceSquash * 0.7f * Squash;
	}
	if (Phase == EPinguGiantPhase::Building)
	{
		// Each pat squashes it a little (in time with the flippers).
		const float Pat = FMath::Abs(FMath::Sin(GetAge() * UE_PI / PatEvery));
		ScaleZ *= 1.f - 0.07f * (1.f - Pat);
		ScaleX *= 1.f + 0.05f * (1.f - Pat);
	}
	// Squash in the play plane, the spin inside it: the squash on the root's scale, the spin on the ball (and on everything
	// that pokes out of it). Squashed down, it keeps its foot on the ground.
	const FQuat Spin(FVector(0.f, 1.f, 0.f), FMath::DegreesToRadians(SpinDeg));
	const FVector Middle(0.f, 0.f, -R * (1.f - ScaleZ) / FMath::Max(ScaleZ, 0.1f));
	Ball->SetRelativeScale3D(FVector(R));
	Ball->SetRelativeRotation(Spin);
	Ball->SetRelativeLocation(Middle);
	SceneRoot->SetRelativeScale3D(FVector(ScaleX, 1.f, ScaleZ));

	UpdateInsideLook(Spin, Middle);
	UpdateStuckLook(Spin, Middle);
	PlayLocalSounds(Dt);
	TickTrail(Dt);
}

void APinguGiantSnowball::UpdateInsideLook(const FQuat& Spin, const FVector& Middle)
{
	using namespace PinguGiantSnowballPrivate;
	// One copy of each penguin inside (its mesh and colours - the hat tells who it is), stuck in the snow on the camera's side:
	// head and flippers out, or now and then the feet - and it turns with the ball (Linus: "with the stuff lokiign out").
	int32 Shown = 0;
	const float R = FMath::Max(GetRadius(), 5.f);
	const int32 Count = Inside.Num();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const APenguinCharacter* Penguin = Inside[Index].Get();
		const USkeletalMeshComponent* Source = Penguin ? Penguin->GetMesh() : nullptr;
		if (!Source || !Source->GetSkeletalMeshAsset())
		{
			continue;
		}
		if (Puppets.Num() <= Shown)
		{
			USkeletalMeshComponent* Made = NewObject<USkeletalMeshComponent>(this, NAME_None, RF_Transient);
			Made->SetupAttachment(SceneRoot);
			Made->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Made->SetGenerateOverlapEvents(false);
			Made->SetCanEverAffectNavigation(false);
			Made->SetCastShadow(false);
			Made->RegisterComponent();
			Puppets.Add(Made);
		}
		USkeletalMeshComponent* Puppet = Puppets[Shown];
		if (Puppet->GetSkeletalMeshAsset() != Source->GetSkeletalMeshAsset())
		{
			Puppet->SetSkeletalMeshAsset(Source->GetSkeletalMeshAsset());
		}
		for (int32 Material = 0; Material < Source->GetNumMaterials(); ++Material)
		{
			if (Puppet->GetMaterial(Material) != Source->GetMaterial(Material))
			{
				Puppet->SetMaterial(Material, Source->GetMaterial(Material));
			}
		}
		// Where on the ball (fixed on it, so it turns with it): spread round its face toward the camera.
		const FVector Out = Spin.RotateVector(StuckDirection(LookSeed, Index, true));
		const bool bFeetOut = (Index % 3) == 2;
		// As big as the ball allows (a small ball: a small copy; never bigger than the penguin).
		const float Scale = FMath::Min(FMath::Clamp(Penguin->GetSizeScale(), 0.5f, 2.f), R * 0.9f / PenguinHeight);
		const float H = PenguinHeight * Scale;
		// Head out: its feet deep inside, its head and the tips of its flippers out (38 % of it). Feet out: upside down, its feet
		// and a bit of its belly out (30 %).
		const FVector Up = bFeetOut ? -Out : Out;
		const FQuat Rot = FQuat::FindBetweenNormals(FVector::UpVector, Up) * FQuat(FVector::UpVector, FMath::DegreesToRadians(-90.f + 40.f * ((Index % 2) ? 1.f : -1.f)));
		const float Poke = StuckOutShare(Out);
		const FVector Feet = Middle + Out * (bFeetOut ? R + 0.3f * Poke * H : R - (1.f - 0.38f * Poke) * H);
		Puppet->SetRelativeLocationAndRotation(Feet, Rot);
		Puppet->SetRelativeScale3D(FVector(Scale));
		Puppet->SetVisibility(true);
		++Shown;
	}
	for (int32 Index = Shown; Index < Puppets.Num(); ++Index)
	{
		if (Puppets[Index])
		{
			Puppets[Index]->SetVisibility(false);
		}
	}
}

void APinguGiantSnowball::UpdateStuckLook(const FQuat& Spin, const FVector& Middle)
{
	using namespace PinguGiantSnowballPrivate;
	const float R = FMath::Max(GetRadius(), 5.f);
	int32 Shown = 0;
	for (int32 Index = 0; Index < Stuck.Num(); ++Index)
	{
		const uint8 Code = Stuck[Index];
		if (StuckParts.Num() <= Shown)
		{
			UStaticMeshComponent* Made = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
			Made->SetupAttachment(SceneRoot);
			Made->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Made->SetGenerateOverlapEvents(false);
			Made->SetCanEverAffectNavigation(false);
			Made->SetCastShadow(false);
			Made->RegisterComponent();
			StuckParts.Add(Made);
		}
		UStaticMeshComponent* Part = StuckParts[Shown];
		// Its look by its kind (made again only when the kind in this part changes).
		const FName Tag(*FString::Printf(TEXT("Stuck%d"), Code));
		if (!Part->ComponentHasTag(Tag))
		{
			Part->ComponentTags.Reset();
			Part->ComponentTags.Add(Tag);
			const TCHAR* MeshPath = Code == StuckIcicle ? TEXT("/Game/FX/SM_Icicle.SM_Icicle")
				: Code == StuckBomb ? TEXT("/Game/FX/SM_SnowBomb.SM_SnowBomb") : TEXT("/Game/FX/SM_Snowball.SM_Snowball");
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, MeshPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
			Part->SetStaticMesh(Mesh ? Mesh : LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
			UMaterialInterface* Prop = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Prop.M_Prop"), nullptr, LOAD_Quiet | LOAD_NoWarn);
			if (Prop)
			{
				UMaterialInstanceDynamic* Made = UMaterialInstanceDynamic::Create(Prop, this);
				FLinearColor Color = FLinearColor::White;
				float Glow = 0.f;
				if (Code == StuckIcicle)
				{
					Color = FLinearColor(0.75f, 0.93f, 1.f);
					Glow = 0.7f;
				}
				else if (Code >= StuckBubble)
				{
					Color = UPenguinAbilityComponent::GetInfo(static_cast<EPenguinAbility>(Code - StuckBubble)).Color;
					Glow = 0.8f;
				}
				else if (Code == StuckSnowball)
				{
					Color = FLinearColor(0.9f, 0.94f, 1.f); // (a little bluer than the ball: a clump of its own)
				}
				Made->SetVectorParameterValue(TEXT("Color"), Color);
				Made->SetScalarParameterValue(TEXT("Emissive"), Glow);
				Part->SetMaterial(0, Made);
			}
		}
		// Its size (cm across) and how far it sticks out of the snow (share of it out).
		const UStaticMesh* Mesh = Part->GetStaticMesh();
		const float MeshSize = Mesh ? FMath::Max(static_cast<float>(Mesh->GetBounds().BoxExtent.GetMax()) * 2.f, 1.f) : 100.f;
		const float Size = Code == StuckIcicle ? 95.f : Code == StuckBomb ? 58.f : Code == StuckSnowball ? 34.f : 44.f;
		const float Shrink = FMath::Clamp(R / 60.f, 0.5f, 1.f); // (a small ball: smaller things)
		const FVector Out = Spin.RotateVector(StuckDirection(LookSeed, Index, false));
		FVector Where;
		FQuat Rot;
		if (Code == StuckIcicle)
		{
			// Its tip out (the mesh's +X), half of it in the snow.
			Rot = FQuat::FindBetweenNormals(FVector::ForwardVector, Out);
			Where = Middle + Out * (R + Size * Shrink * (0.3f * StuckOutShare(Out) - 0.45f));
		}
		else
		{
			Rot = Spin;
			Where = Middle + Out * (R + Size * Shrink * ((Code == StuckSnowball ? 0.45f : 0.65f) * StuckOutShare(Out) - 0.5f));
		}
		Part->SetRelativeLocationAndRotation(Where, Rot);
		Part->SetRelativeScale3D(FVector(Size * Shrink / MeshSize));
		Part->SetVisibility(true);
		++Shown;
	}
	for (int32 Index = Shown; Index < StuckParts.Num(); ++Index)
	{
		if (StuckParts[Index])
		{
			StuckParts[Index]->SetVisibility(false);
		}
	}
}

void APinguGiantSnowball::BuildBallMesh(float ForDiameter)
{
	using namespace PinguGiantSnowballPrivate;
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colors;
	// An icosphere (no seams, even triangles): 642 / 2562 / 10242 vertices on Low / normal / Ultra.
	const int32 Subdivisions = MeshDetail <= 0 ? 3 : (MeshDetail == 1 ? 4 : 5);
	{
		const float T = (1.f + FMath::Sqrt(5.f)) * 0.5f;
		const FVector Corners[] = { { -1, T, 0 }, { 1, T, 0 }, { -1, -T, 0 }, { 1, -T, 0 }, { 0, -1, T }, { 0, 1, T }, { 0, -1, -T }, { 0, 1, -T },
			{ T, 0, -1 }, { T, 0, 1 }, { -T, 0, -1 }, { -T, 0, 1 } };
		for (const FVector& Corner : Corners)
		{
			Vertices.Add(Corner.GetSafeNormal());
		}
		Triangles = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
			3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 };
		for (int32 Level = 0; Level < Subdivisions; ++Level)
		{
			TMap<uint64, int32> Mid;
			const auto Midpoint = [&Vertices, &Mid](int32 A, int32 B) -> int32
			{
				const uint64 Key = A < B ? (static_cast<uint64>(A) << 32) | static_cast<uint32>(B) : (static_cast<uint64>(B) << 32) | static_cast<uint32>(A);
				if (const int32* Found = Mid.Find(Key))
				{
					return *Found;
				}
				const int32 Made = Vertices.Add(((Vertices[A] + Vertices[B]) * 0.5f).GetSafeNormal());
				Mid.Add(Key, Made);
				return Made;
			};
			TArray<int32> Next;
			Next.Reserve(Triangles.Num() * 4);
			for (int32 Tri = 0; Tri + 2 < Triangles.Num(); Tri += 3)
			{
				const int32 A = Triangles[Tri];
				const int32 B = Triangles[Tri + 1];
				const int32 C = Triangles[Tri + 2];
				const int32 AB = Midpoint(A, B);
				const int32 BC = Midpoint(B, C);
				const int32 CA = Midpoint(C, A);
				Next.Append({ A, AB, CA, B, BC, AB, C, CA, BC, AB, BC, CA });
			}
			Triangles = MoveTemp(Next);
		}
	}
	// The lumps: packed clumps (bumps) and a few dents, a gentle wobble and fine grain on top; then scaled so the mean radius
	// is 1 (its size and its collision stay what they were).
	FRandomStream Rand(LookSeed);
	struct FBlob
	{
		FVector Dir;
		float Amount;
		float Width;
	};
	TArray<FBlob> Lumps;
	for (int32 Index = 0; Index < BallLumps; ++Index)
	{
		const bool bDent = Index % 4 == 3;
		Lumps.Add({ Rand.GetUnitVector(), static_cast<float>(bDent ? -Rand.FRandRange(0.03f, 0.06f) : Rand.FRandRange(0.035f, 0.1f)), static_cast<float>(Rand.FRandRange(0.22f, 0.5f)) });
	}
	// Darker packed patches and streaks: they make the turning show from far away.
	TArray<FBlob> Patches;
	for (int32 Index = 0; Index < BallPatches; ++Index)
	{
		Patches.Add({ Rand.GetUnitVector(), static_cast<float>(Rand.FRandRange(0.28f, 0.42f)), static_cast<float>(Rand.FRandRange(0.28f, 0.48f)) });
	}
	const FVector Offset(Rand.FRandRange(0.f, 50.f), Rand.FRandRange(0.f, 50.f), Rand.FRandRange(0.f, 50.f));
	const float StreakTurns = Rand.FRandRange(2.5f, 3.5f);
	TArray<float> Radii;
	Radii.SetNum(Vertices.Num());
	float Sum = 0.f;
	for (int32 Index = 0; Index < Vertices.Num(); ++Index)
	{
		const FVector D = Vertices[Index];
		float Radius = 1.f;
		for (const FBlob& Lump : Lumps)
		{
			const float Angle = FMath::Acos(FMath::Clamp(static_cast<float>(FVector::DotProduct(D, Lump.Dir)), -1.f, 1.f));
			Radius += Lump.Amount * FMath::Exp(-FMath::Square(Angle / Lump.Width));
		}
		Radius += 0.035f * FMath::PerlinNoise3D(D * 2.2f + Offset) + 0.012f * FMath::PerlinNoise3D(D * 7.f + Offset * 2.f);
		Radii[Index] = Radius;
		Sum += Radius;
	}
	const float Mean = Sum / FMath::Max(Vertices.Num(), 1);
	float Lowest = 10.f;
	float Highest = 0.f;
	for (float& Radius : Radii)
	{
		Radius /= Mean;
		Lowest = FMath::Min(Lowest, Radius);
		Highest = FMath::Max(Highest, Radius);
	}
	LumpOut = Highest - 1.f;
	LumpIn = 1.f - Lowest;
	// The snow texture's grain at the island's size (1 UV = a block, 100 cm), laid flat toward the camera (it looks down -Y,
	// so the visible half is mapped without stretch; it turns with the ball: the spin is round Y).
	const float UVScale = ForDiameter * 0.5f / 100.f;
	Colors.SetNum(Vertices.Num());
	UVs.SetNum(Vertices.Num());
	for (int32 Index = 0; Index < Vertices.Num(); ++Index)
	{
		const FVector D = Vertices[Index];
		Vertices[Index] = D * Radii[Index];
		UVs[Index] = FVector2D(D.X * UVScale, -D.Z * UVScale);
		// Brightness (the material's AO: vertex colour R): the clumps light, the hollows darker and bluer; darker packed patches;
		// thin packed streaks winding round it.
		const float Height01 = FMath::Clamp((Radii[Index] - Lowest) / FMath::Max(Highest - Lowest, 0.01f), 0.f, 1.f);
		float Light = FMath::Lerp(0.72f, 1.f, FMath::Pow(Height01, 0.7f));
		for (const FBlob& Patch : Patches)
		{
			const float Angle = FMath::Acos(FMath::Clamp(static_cast<float>(FVector::DotProduct(D, Patch.Dir)), -1.f, 1.f));
			Light -= Patch.Amount * FMath::Exp(-FMath::Square(Angle / Patch.Width));
		}
		const float Wind = FMath::Sin(StreakTurns * FMath::Atan2(static_cast<float>(D.Z), static_cast<float>(D.X)) + 3.f * static_cast<float>(D.Y)
			+ 1.5f * FMath::PerlinNoise3D(D * 1.6f + Offset));
		Light -= 0.26f * FMath::Pow(FMath::Max(Wind, 0.f), 8.f);
		const uint8 Value = static_cast<uint8>(FMath::Clamp(Light, 0.4f, 1.f) * 255.f);
		Colors[Index] = FColor(Value, Value, Value, 255);
	}
	// Normals from the faces; front faces wound the way the engine wants (the cross product of the first two edges points in).
	Normals.Init(FVector::ZeroVector, Vertices.Num());
	for (int32 Tri = 0; Tri + 2 < Triangles.Num(); Tri += 3)
	{
		const FVector& A = Vertices[Triangles[Tri]];
		const FVector& B = Vertices[Triangles[Tri + 1]];
		const FVector& C = Vertices[Triangles[Tri + 2]];
		FVector Face = FVector::CrossProduct(B - A, C - A);
		if (FVector::DotProduct(Face, A + B + C) > 0.f)
		{
			Swap(Triangles[Tri + 1], Triangles[Tri + 2]);
		}
		else
		{
			Face = -Face;
		}
		// (Face points out now, whichever way the triangle was wound.)
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Normals[Triangles[Tri + Corner]] += Face;
		}
	}
	for (FVector& Normal : Normals)
	{
		Normal = Normal.GetSafeNormal();
	}
	Ball->ClearAllMeshSections();
	Ball->CreateMeshSection(0, Vertices, Triangles, Normals, UVs, Colors, TArray<FProcMeshTangent>(), false);
	Ball->SetCastShadow(true);
	MeshBuiltFor = ForDiameter;
	MeshVertices = Vertices.Num();
	Ball->SetMaterial(0, bFrozen && IceMaterial ? IceMaterial.Get() : SnowMaterial.Get());
}

void APinguGiantSnowball::TickTrail(float Dt)
{
	using namespace PinguGiantSnowballPrivate;
	// Linus: "the normal visual it already has with a small snow particle behind it". Only while it rolls along the ground (not
	// in a fall, not before it rolls off, gone with it); as big as the ball; fewer on lower effects settings (every machine its
	// own, never sent over the network).
	const bool bOnGround = HasAuthority() ? bGrounded : FMath::IsNearlyZero(Motion.VZ);
	if (Phase != EPinguGiantPhase::Rolling || !bOnGround)
	{
		TrailTimer = 0.f;
		return;
	}
	TrailTimer -= Dt;
	if (TrailTimer > 0.f)
	{
		return;
	}
	const float Density = FMath::Clamp(UPinguSettings::GetFXDensity(), 0.25f, 1.f);
	TrailTimer = TrailEvery / Density;
	const float R = FMath::Max(GetRadius(), 5.f);
	const FVector Foot = GetActorLocation() + FVector(-Direction * R * 0.55f, R * 0.25f, -R * 0.88f);
	APinguFX::Trail(this, Foot, R * TrailSize, bFrozen ? TrailIceColor : TrailSnowColor);
	// Now and then a little clump flicked up and back off it.
	if ((++TrailPuffs % 3) == 0)
	{
		APinguFX::Puffs(this, Foot + FVector(0.f, 0.f, R * 0.1f), 1, R * TrailSize * 0.55f, 90.f + R * 0.4f, FVector(-Direction, 0.f, 1.2f).GetSafeNormal(), 0.5f,
			bFrozen ? TrailIceColor : TrailSnowColor);
	}
}

void APinguGiantSnowball::PlayLocalSounds(float Dt)
{
	// The rumble of it rolling: every machine makes its own (no stream of sounds over the network).
	if (Phase != EPinguGiantPhase::Rolling)
	{
		return;
	}
	const bool bOnGround = HasAuthority() ? bGrounded : FMath::IsNearlyZero(Motion.VZ);
	RollSoundTimer -= Dt;
	if (RollSoundTimer <= 0.f && bOnGround)
	{
		RollSoundTimer = PinguGiantSnowballPrivate::RollSoundEvery;
		const float Size = GetDiameter() / PinguTuning::GiantStartSize;
		APinguFX::Sound(this, EPinguSound::GiantRoll, FMath::Clamp(0.35f + 0.15f * Size, 0.35f, 0.8f), 0.06f,
			FMath::Clamp(1.15f - 0.12f * Size, 0.6f, 1.15f) * (bFrozen ? 1.3f : 1.f));
	}
}
