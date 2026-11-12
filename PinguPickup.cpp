// Pingu - ability bubbles. A random one floats down from the sky every few seconds and every knocked-out
// penguin drops one of its abilities. Touch a bubble to swap its ability into your middle (MMB) slot.
//
// LAN: the server spawns, moves, pushes, hands out and pops the bubbles (its effects play everywhere through
// FPinguFXBroadcastScope). Clients get the bubble, its spawn time and motion snapshots and fly their copy on from
// those with the same physics; sparkles, glow trail, landing ring, bob, pop-in and blink are made on every machine.

#include "PinguPickup.h"
#include "BlockIsland.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PinguFX.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"

namespace PinguPickupPrivate
{
	// LAN: motion snapshots (the bubble is slow and clients fly it on with the same physics; these only correct drift).
	constexpr float NetUpdateRate = 30.f;        // 1/s: how often the server may send a bubble
	constexpr float FloatSendInterval = 0.1f;    // s: a snapshot this often while it floats
	constexpr float RestSendInterval = 1.f;      // s: lying down, only while it still settles
	constexpr float MaxCatchUp = 0.25f;          // s: a client flies a snapshot on by at most this
	constexpr float CorrectionFade = 12.f;       // 1/s: how fast a client blends a correction out
	constexpr float SnapDistance = 200.f;        // cm: a bigger correction is not blended (it would look like a slide)

	constexpr float Gravity = 900.f;          // cm/s^2: bubbles float down gently
	constexpr float AirDrag = 1.5f;           // 1/s, sideways
	constexpr float GroundFriction = 3.f;     // 1/s, rolling along the ground
	constexpr float OverspeedDrag = 3.f;      // 1/s, knocked down faster than FallSpeed
	constexpr float SweepScale = 0.9f;        // collision sphere radius / bubble radius
	constexpr float BounceSpeed = 250.f;      // lands faster than this: a small bounce
	constexpr float Bounciness = 0.4f;
	constexpr float RestSpeed = 40.f;         // slower than this on flat ground: lies still
	constexpr float FloorNormalZ = 0.6f;
	constexpr float FlatNormalZ = 0.96f;      // flat enough to lie on (less: it rolls off the edge)
	constexpr float Skin = 0.5f;              // gap kept to a surface after a hit
	constexpr float BobAmplitude = 6.f;       // cm
	constexpr float BobSpeed = 2.4f;          // rad/s
	constexpr float PopInTime = 0.35f;
	constexpr float BlinkTime = 3.f;
	constexpr float SkyHeight = 1000.f;       // sky drops start this far above the island top
	constexpr float SeaDropHeight = 700.f;    // the drop of a penguin that fell into the sea
	constexpr float SeaMargin = 60.f;
	constexpr float DefaultSeaLevel = -1650.f;
	constexpr float CollapseMargin = 150.f;   // sudden death: stay this far inside the crumbling edge
	constexpr float MarkerInterval = 0.45f;
	constexpr float MarkerRadius = 55.f;
	constexpr float PushCooldownTime = 0.12f;
	constexpr float DropPickupDelay = 0.5f;
	/** Every new bubble: nobody takes it for this long (2026-11-07, shared-traits). A sky drop is born high up, and with Low
	 *  Gravity for everybody a penguin up there took one in the very frame it appeared - no LAN machine (nor the joiners'
	 *  anti-cheat judge, which then saw an ability out of nowhere: "not dealt Noot") ever saw the bubble. */
	constexpr float NewPickupDelay = 0.25f;

	static bool IsRealAbility(EPenguinAbility Which)
	{
		return Which > EPenguinAbility::None && Which < EPenguinAbility::Count;
	}

	/** Bubbles are made and changed by the server (or a solo game) only. */
	static bool IsServerWorld(const UWorld* World)
	{
		return World && World->GetNetMode() != NM_Client;
	}

	/** The server's game clock: on a LAN client the replicated server time, else the world's own. */
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

	/** The ability colour, a touch brighter: the bubble is mostly a glowing rim. */
	static FLinearColor BubbleColor(EPenguinAbility Which)
	{
		const FLinearColor Base = UPenguinAbilityComponent::GetInfo(Which).Color;
		return FLinearColor((0.1f + 0.9f * Base.R) * 1.15f, (0.1f + 0.9f * Base.G) * 1.15f, (0.1f + 0.9f * Base.B) * 1.15f);
	}

	static float BobOffset(float BobTime)
	{
		return BobAmplitude * FMath::Sin(BobTime * BobSpeed);
	}

	/** Out of whatever a sweep started inside of, on the play plane. */
	static FVector PushOut(const FHitResult& Hit)
	{
		FVector Out(Hit.Normal.X, 0.f, Hit.Normal.Z);
		Out = Out.GetSafeNormal();
		if (Out.IsNearlyZero())
		{
			Out = FVector::UpVector;
		}
		return Out * (FMath::Max(Hit.PenetrationDepth, 0.f) + 1.f);
	}

	/** X range over the island where a bubble may come down: inside the bounds (inset) and, in sudden death,
	 *  inside the part that is not crumbling yet. */
	static void SafeRangeX(const ABlockIsland* Island, const FBox& Bounds, float Inset, float& OutMinX, float& OutMaxX)
	{
		OutMinX = static_cast<float>(Bounds.Min.X) + Inset;
		OutMaxX = static_cast<float>(Bounds.Max.X) - Inset;
		if (OutMinX > OutMaxX)
		{
			OutMinX = OutMaxX = static_cast<float>(Bounds.GetCenter().X); // a tiny island: its middle
		}
		if (Island->IsCollapsing())
		{
			const float CenterX = Island->GetCollapseCenterX();
			const float Keep = FMath::Max(Island->GetCollapseRadius() - CollapseMargin, 0.f);
			const float Lo = FMath::Max(OutMinX, CenterX - Keep);
			const float Hi = FMath::Min(OutMaxX, CenterX + Keep);
			if (Lo <= Hi)
			{
				OutMinX = Lo;
				OutMaxX = Hi;
			}
			else
			{
				OutMinX = OutMaxX = FMath::Clamp(CenterX, OutMinX, OutMaxX);
			}
		}
	}
}

APinguPickup::APinguPickup()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics; // after the penguins moved
	SetActorEnableCollision(false);

	// The bubble hangs off a plain root so it can bob without moving the actor (GetIconLocation follows it).
	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = SceneRoot;

	Bubble = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Bubble"));
	Bubble->SetupAttachment(SceneRoot);
	Bubble->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Bubble->SetGenerateOverlapEvents(false);
	Bubble->SetCanEverAffectNavigation(false);
	Bubble->SetCastShadow(false);
	Bubble->SetRelativeScale3D(FVector(0.01f)); // pops in (ApplyLook)

	// LAN: the server's bubbles go to every machine. The engine's movement replication stays off: the motion travels
	// in our own snapshots (Motion), which clients fly on smoothly.
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(PinguPickupPrivate::NetUpdateRate);
	SetMinNetUpdateFrequency(2.f);
}

void APinguPickup::BeginPlay()
{
	Super::BeginPlay();
	if (!HasAuthority())
	{
		if (!HomeIsland.IsValid())
		{
			HomeIsland = ABlockIsland::FindNear(GetWorld(), GetActorLocation());
		}
		if (!bHasMotion)
		{
			SimLocation = GetActorLocation(); // no snapshot yet: fly on from where it appeared
			bHasMotion = true;
		}
		SetupLook();
	}
}

void APinguPickup::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APinguPickup, Ability);
	DOREPLIFETIME(APinguPickup, SpawnTime);
	DOREPLIFETIME(APinguPickup, BobStart);
	DOREPLIFETIME(APinguPickup, TakeDelay);
	DOREPLIFETIME(APinguPickup, Motion);
	DOREPLIFETIME(APinguPickup, ReelHook);
}

double APinguPickup::GetServerNow() const
{
	return PinguPickupPrivate::ServerNow(GetWorld());
}

// ---------------------------------------------------------------------------- spawning

APinguPickup* APinguPickup::Spawn(UWorld* World, EPenguinAbility InAbility, const FVector& Location, const FVector& InVelocity)
{
	using namespace PinguPickupPrivate;
	if (!IsServerWorld(World) || !IsRealAbility(InAbility))
	{
		return nullptr;
	}
	const FVector Start(Location.X, 0.f, Location.Z);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APinguPickup* Pickup = World->SpawnActor<APinguPickup>(Start, FRotator::ZeroRotator, Params);
	if (!Pickup)
	{
		return nullptr;
	}
	Pickup->Ability = InAbility;
	Pickup->Velocity = FVector(InVelocity.X, 0.f, InVelocity.Z);
	Pickup->Bob = FMath::FRandRange(0.f, 10.f); // bubbles don't bob in step
	Pickup->BobStart = Pickup->Bob;
	Pickup->SpawnTime = ServerNow(World);
	Pickup->PickupDelay = NewPickupDelay;
	Pickup->TakeDelay = NewPickupDelay;
	Pickup->HomeIsland = ABlockIsland::FindNear(World, Start);
	Pickup->SetupLook(); // after the ability: the tint needs it
	Pickup->ApplyLook();
	Pickup->PublishMotion(0.f, true); // the first snapshot goes out with the bubble
	return Pickup;
}

APinguPickup* APinguPickup::SpawnFromSky(UWorld* World, FRandomStream& Rand)
{
	using namespace PinguPickupPrivate;
	ABlockIsland* Island = IsServerWorld(World) ? ABlockIsland::Get(World) : nullptr;
	if (!World || !Island)
	{
		return nullptr;
	}
	const FBox Bounds = Island->GetBounds();
	if (!Bounds.IsValid)
	{
		return nullptr;
	}
	const EPenguinAbility Picked = static_cast<EPenguinAbility>(Rand.RandRange(1, static_cast<int32>(EPenguinAbility::Count) - 1));
	float MinX = 0.f;
	float MaxX = 0.f;
	SafeRangeX(Island, Bounds, Island->CellSize, MinX, MaxX);
	const float TopZ = static_cast<float>(Bounds.Max.Z);
	const float Depth = static_cast<float>(Bounds.Max.Z - Bounds.Min.Z) + 200.f;
	for (int32 Attempt = 0; Attempt < 12; ++Attempt)
	{
		const float X = static_cast<float>(Rand.FRandRange(MinX, MaxX));
		// Sudden death: only well inside the part that is not crumbling.
		if (Island->IsCollapsing() && FMath::Abs(X - Island->GetCollapseCenterX()) >= Island->GetCollapseRadius() - CollapseMargin)
		{
			continue;
		}
		float GroundZ = 0.f;
		if (!Island->FindGroundBelow(FVector(X, 0.f, TopZ + 50.f), Depth, GroundZ))
		{
			continue; // a gap: the sea below
		}
		APinguPickup* Pickup = Spawn(World, Picked, FVector(X, 0.f, TopZ + SkyHeight));
		if (Pickup)
		{
			FPinguFXBroadcastScope Broadcast(Pickup);
			APinguFX::Sound(Pickup, EPinguSound::Drop, 0.45f, 0.03f);
			UE_LOG(LogPingu, Display, TEXT("[PICKUP] sky drop %s at x %.0f"), UPenguinAbilityComponent::GetInfo(Picked).Name, X);
		}
		return Pickup;
	}
	return nullptr;
}

APinguPickup* APinguPickup::SpawnDrop(UWorld* World, EPenguinAbility InAbility, const FVector& Where, bool bFellIntoSea)
{
	using namespace PinguPickupPrivate;
	if (!IsServerWorld(World) || !IsRealAbility(InAbility))
	{
		return nullptr;
	}
	APinguPickup* Pickup = nullptr;
	if (!bFellIntoSea)
	{
		// Pops out where the penguin was (nobody can grab it for a moment, so everyone sees it).
		Pickup = Spawn(World, InAbility, Where + FVector(0.f, 0.f, 20.f), FVector(FMath::FRandRange(-120.f, 120.f), 0.f, 420.f));
		if (Pickup)
		{
			Pickup->PickupDelay = DropPickupDelay;
			Pickup->TakeDelay = DropPickupDelay;
		}
	}
	else
	{
		// Fell into the sea: floats down from the sky over the nearest part of the island.
		ABlockIsland* Island = ABlockIsland::FindNear(World, Where);
		const FBox Bounds = Island ? Island->GetBounds() : FBox(ForceInit);
		if (!Island || !Bounds.IsValid)
		{
			return nullptr;
		}
		float MinX = 0.f;
		float MaxX = 0.f;
		SafeRangeX(Island, Bounds, Island->CellSize * 1.5f, MinX, MaxX);
		const float X = FMath::Clamp(static_cast<float>(Where.X), MinX, MaxX);
		Pickup = Spawn(World, InAbility, FVector(X, 0.f, static_cast<float>(Bounds.Max.Z) + SeaDropHeight));
	}
	if (Pickup)
	{
		UE_LOG(LogPingu, Display, TEXT("[PICKUP] drop %s at x %.0f"), UPenguinAbilityComponent::GetInfo(InAbility).Name, Pickup->GetActorLocation().X);
	}
	return Pickup;
}

void APinguPickup::GetAll(const UWorld* World, TArray<APinguPickup*>& Out)
{
	Out.Reset();
	if (!World)
	{
		return;
	}
	for (TActorIterator<APinguPickup> It(const_cast<UWorld*>(World)); It; ++It)
	{
		APinguPickup* Pickup = *It;
		if (IsValid(Pickup) && !Pickup->bTaken && !Pickup->IsActorBeingDestroyed())
		{
			Out.Add(Pickup);
		}
	}
}

int32 APinguPickup::CountAll(const UWorld* World)
{
	TArray<APinguPickup*> All;
	GetAll(World, All);
	return All.Num();
}

void APinguPickup::DestroyAll(UWorld* World)
{
	if (!PinguPickupPrivate::IsServerWorld(World))
	{
		return; // a client's copies go when the server's do
	}
	TArray<APinguPickup*> All;
	GetAll(World, All);
	for (APinguPickup* Pickup : All)
	{
		Pickup->bTaken = true;
		Pickup->Destroy();
	}
}

// ---------------------------------------------------------------------------- pushes

void APinguPickup::Push(const FVector& Impulse)
{
	// A shot overlapping the bubble for a few frames pushes it once. Server only: clients see it in the next snapshot.
	// A bubble on a fish hook goes where the hook goes.
	if (bTaken || PushCooldown > 0.f || !HasAuthority() || IsOnHook())
	{
		return;
	}
	Velocity += FVector(Impulse.X, 0.f, Impulse.Z);
	bResting = false;
	PushCooldown = PinguPickupPrivate::PushCooldownTime;
	Jiggle = 1.f;
	++PushSerial;
	bMotionKick = true;
}

void APinguPickup::WarpTo(const FVector& NewLocation)
{
	if (bTaken || !HasAuthority() || IsOnHook())
	{
		return;
	}
	SetActorLocation(FVector(NewLocation.X, 0.f, NewLocation.Z));
	bResting = false;
	bTrailStarted = false; // no glow streak across the jump
	bMotionKick = true;    // the clients' next snapshot is there at once (a jump this big snaps)
}

int32 APinguPickup::PushInArea(UWorld* World, const FVector& Origin, float AreaRadius, float Strength, const FVector& ConeDirection, float CosHalfAngle, float Lift)
{
	if (!PinguPickupPrivate::IsServerWorld(World))
	{
		return 0;
	}
	TArray<APinguPickup*> All;
	GetAll(World, All);
	FVector Cone(ConeDirection.X, 0.f, ConeDirection.Z);
	Cone = Cone.GetSafeNormal();
	const bool bCone = CosHalfAngle > -1.f && !Cone.IsNearlyZero();
	int32 Count = 0;
	for (APinguPickup* Pickup : All)
	{
		FVector D = Pickup->GetIconLocation() - Origin;
		D.Y = 0.f;
		const float Dist = static_cast<float>(D.Size());
		const float Reach = AreaRadius + Pickup->Radius;
		if (Dist > Reach || Pickup->IsOnHook())
		{
			continue;
		}
		if (bCone && Dist > 30.f && FVector::DotProduct(D / Dist, Cone) < CosHalfAngle)
		{
			continue;
		}
		const FVector Dir = bCone ? Cone : (Dist > 1.f ? D / Dist : FVector::UpVector);
		// Full strength in the middle, half at the edge (the lift too).
		const float Falloff = 1.f - 0.5f * FMath::Clamp(Dist / FMath::Max(Reach, 1.f), 0.f, 1.f);
		Pickup->Push(Dir * (Strength * Falloff) + FVector::UpVector * (Lift * Falloff));
		++Count;
	}
	return Count;
}

// ---------------------------------------------------------------------------- the fish hook

void APinguPickup::AttachToHook(AActor* Hook)
{
	if (!HasAuthority() || bTaken || !IsValid(Hook))
	{
		return;
	}
	// ReelHook replicates: every machine draws the bubble on its own copy of the hook from now on.
	ReelHook = Hook;
	Velocity = FVector::ZeroVector;
	bResting = false;
	bSliding = false;
	Jiggle = 1.f;
	SetActorLocation(Hook->GetActorLocation());
	ForceNetUpdate();
	FPinguFXBroadcastScope Broadcast(this);
	APinguFX::Sparkles(this, GetIconLocation(), 5, Radius * 0.6f, PinguPickupPrivate::BubbleColor(Ability));
}

void APinguPickup::ReleaseFromHook(const FVector& InVelocity)
{
	if (!HasAuthority() || !ReelHook)
	{
		return;
	}
	ReelHook = nullptr;
	Velocity = FVector(InVelocity.X, 0.f, InVelocity.Z);
	bResting = false;
	Jiggle = 1.f;
	bTrailStarted = false; // no glow streak from where the hook held it
	FVector Location = GetActorLocation();
	if (EscapeBlocks(Location)) // let go inside a block (the hook was at a wall): out on top
	{
		SetActorLocation(Location);
	}
	PublishMotion(0.f, true); // the clients fly on from here
}

bool APinguPickup::GiveTo(APenguinCharacter* Penguin)
{
	if (!HasAuthority() || bTaken || IsActorBeingDestroyed() || !IsValid(Penguin) || !Penguin->IsAlive() || Penguin->IsHidden())
	{
		return false;
	}
	Collect(Penguin); // (gone with it, hook and all)
	return bTaken;
}

// ---------------------------------------------------------------------------- look

FVector APinguPickup::GetIconLocation() const
{
	return GetActorLocation() + FVector(0.f, 0.f, PinguPickupPrivate::BobOffset(Bob));
}

float APinguPickup::GetIconScale() const
{
	const float T = Age / PinguPickupPrivate::PopInTime;
	if (T >= 1.f)
	{
		return 1.f;
	}
	if (T < 0.65f)
	{
		// Out to a little overshoot ...
		const float Grow = T / 0.65f;
		return 1.15f * (1.f - (1.f - Grow) * (1.f - Grow));
	}
	// ... and back to 1.
	const float Settle = (T - 0.65f) / 0.35f;
	return 1.15f - 0.15f * (0.5f - 0.5f * FMath::Cos(Settle * UE_PI));
}

float APinguPickup::GetOpacity() const
{
	using namespace PinguPickupPrivate;
	const float Left = Life - Age;
	if (Left >= BlinkTime)
	{
		return 1.f;
	}
	// Blinks 2.5 times a second at first, 8 times at the very end.
	const float Elapsed = FMath::Clamp(BlinkTime - Left, 0.f, BlinkTime);
	const float Cycles = 2.5f * Elapsed + (8.f - 2.5f) * Elapsed * Elapsed / (2.f * BlinkTime);
	const float Wave = 0.5f + 0.5f * FMath::Cos(Cycles * 2.f * UE_PI);
	return FMath::Lerp(0.3f, 1.f, FMath::SmoothStep(0.2f, 0.8f, Wave));
}

void APinguPickup::SetupLook()
{
	if (!Bubble)
	{
		return;
	}
	if (!bLookLoaded)
	{
		bLookLoaded = true;
		// Loaded by path like the ice shell (SM_Bubble is 100 cm across).
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FX/SM_Bubble.SM_Bubble"), nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!Mesh)
		{
			Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		}
		Bubble->SetStaticMesh(Mesh);
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Bubble.M_Bubble"), nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!Material)
		{
			Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		}
		if (Material)
		{
			BubbleMaterial = UMaterialInstanceDynamic::Create(Material, this);
			Bubble->SetMaterial(0, BubbleMaterial);
		}
	}
	// Runs again from Spawn once the ability is set, so the tint is right whichever came first.
	if (BubbleMaterial)
	{
		BubbleMaterial->SetVectorParameterValue(TEXT("Color"), PinguPickupPrivate::BubbleColor(Ability));
		AppliedOpacity = GetOpacity();
		BubbleMaterial->SetScalarParameterValue(TEXT("Opacity"), AppliedOpacity);
	}
}

void APinguPickup::ApplyLook()
{
	if (!Bubble)
	{
		return;
	}
	// Pops in, wobbles gently (more right after a bounce or a push) and bobs like the HUD icon.
	const float Wobble = 0.03f * FMath::Sin(Age * 6.5f) + 0.1f * Jiggle * FMath::Sin(Age * 24.f);
	const float Size = FMath::Max(Radius * 2.f / 100.f * GetIconScale(), 0.01f);
	Bubble->SetRelativeScale3D(FVector(Size * (1.f + Wobble), Size, Size * (1.f - Wobble)));
	Bubble->SetRelativeLocation(FVector(0.f, 0.f, PinguPickupPrivate::BobOffset(Bob)));
	const float Opacity = GetOpacity();
	if (BubbleMaterial && FMath::Abs(Opacity - AppliedOpacity) > 0.005f)
	{
		AppliedOpacity = Opacity;
		BubbleMaterial->SetScalarParameterValue(TEXT("Opacity"), Opacity);
	}
}

void APinguPickup::UpdateLook(float Dt)
{
	using namespace PinguPickupPrivate;
	if (!bLookLoaded)
	{
		SetupLook();
	}
	Jiggle = FMath::Max(0.f, Jiggle - 2.5f * Dt);
	ApplyLook();

	const FLinearColor Color = BubbleColor(Ability);
	const FVector Icon = GetIconLocation();
	// A sparkle in the ability colour now and then.
	SparkleTimer -= Dt;
	if (SparkleTimer <= 0.f)
	{
		SparkleTimer = FMath::FRandRange(0.3f, 0.65f);
		APinguFX::Sparkles(this, Icon, 1, Radius * 0.7f, Color);
	}
	if (bResting)
	{
		LastTrailPoint = Icon;
		bTrailStarted = true;
		MarkerTimer = 0.f;
		return;
	}
	// Floating: a soft glow trail ...
	if (!bTrailStarted)
	{
		LastTrailPoint = Icon;
		bTrailStarted = true;
	}
	else if (FVector::DistSquared(LastTrailPoint, Icon) > 4.f)
	{
		APinguFX::GlowTrail(this, LastTrailPoint, Icon, 24.f, Color, 0.25f, 0.35f);
		LastTrailPoint = Icon;
	}
	if (IsOnHook())
	{
		MarkerTimer = 0.f;
		return; // on its way to a flipper, not coming down anywhere
	}
	// ... and a ring on the ground where it will come down.
	MarkerTimer -= Dt;
	if (MarkerTimer <= 0.f)
	{
		MarkerTimer = MarkerInterval;
		ShowLandingMarker();
	}
}

void APinguPickup::ShowLandingMarker()
{
	using namespace PinguPickupPrivate;
	UWorld* World = GetWorld();
	APinguFX* FX = APinguFX::Get(this);
	if (!World || !FX)
	{
		return;
	}
	const ABlockIsland* Island = HomeIsland.Get();
	const FVector From = GetActorLocation();
	const FVector To(From.X, 0.f, Island ? Island->SeaLevel : DefaultSeaLevel);
	if (To.Z >= From.Z)
	{
		return;
	}
	FHitResult Hit;
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguPickupMarker), false, this);
	if (!World->SweepSingleByObjectType(Hit, From, To, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic),
		FCollisionShape::MakeSphere(Radius * SweepScale), Params) || Hit.bStartPenetrating)
	{
		return; // over the sea: nowhere to land
	}
	if (From.Z - Hit.Location.Z < 80.f)
	{
		return; // about to land anyway
	}
	// Lying on the ground and closing in on the spot.
	FPinguParticleDesc Desc;
	Desc.Mesh = EPinguFXMesh::Ring;
	Desc.Location = FVector(Hit.Location.X, 0.f, Hit.ImpactPoint.Z + 8.f);
	Desc.Rotation = FRotator(0.f, 0.f, 90.f); // SM_Ring faces the camera: lay it flat
	Desc.Size = MarkerRadius * 2.f * 1.3f;
	Desc.EndSize = MarkerRadius * 2.f * 0.6f;
	Desc.Curve = EPinguSizeCurve::Linear;
	Desc.Life = 0.5f;
	Desc.bFadeOut = true;
	Desc.Emissive = 1.2f;
	Desc.Opacity = 0.9f;
	Desc.Color = BubbleColor(Ability);
	FX->Spawn(Desc);
}

// ---------------------------------------------------------------------------- tick

void APinguPickup::Tick(float DeltaSeconds)
{
	using namespace PinguPickupPrivate;
	Super::Tick(DeltaSeconds);
	if (bTaken || IsActorBeingDestroyed())
	{
		return;
	}
	const float Dt = FMath::Min(DeltaSeconds, 0.05f);
	if (!HasAuthority())
	{
		TickClient(Dt);
		return;
	}
	Age += Dt;
	Bob += Dt;
	PickupDelay = FMath::Max(0.f, PickupDelay - Dt);
	PushCooldown = FMath::Max(0.f, PushCooldown - Dt);
	if (Age >= Life)
	{
		Pop(false);
		return;
	}
	if (!HomeIsland.IsValid())
	{
		HomeIsland = ABlockIsland::Get(GetWorld());
	}
	if (ReelHook)
	{
		if (IsValid(ReelHook))
		{
			// Reeled in by a fish hook: wherever the hook is (it hands the bubble over at its thrower's flipper).
			SetActorLocation(ReelHook->GetActorLocation());
			UpdateLook(Dt);
			return;
		}
		ReleaseFromHook(); // the hook went without letting go: float down from here
	}

	FVector Location = GetActorLocation();
	if (MoveBubble(Dt, Location))
	{
		SetActorLocation(Location);
	}

	const ABlockIsland* Island = HomeIsland.Get();
	if (GetActorLocation().Z < (Island ? Island->SeaLevel : DefaultSeaLevel) + SeaMargin)
	{
		Pop(true);
		return;
	}
	if (CanBeTaken())
	{
		if (APenguinCharacter* Toucher = FindToucher())
		{
			Collect(Toucher);
			return;
		}
	}
	PublishMotion(Dt, false);
	UpdateLook(Dt);
}

void APinguPickup::PublishMotion(float Dt, bool bForce)
{
	using namespace PinguPickupPrivate;
	if (GetNetMode() == NM_Standalone)
	{
		return; // nobody to tell
	}
	PublishTimer -= Dt;
	// At once when the motion jumped or it landed / took off; else now and then while it floats (clients fly the same
	// physics, this only corrects their drift) and while it still settles on the ground.
	const bool bNow = bForce || bMotionKick || bResting != Motion.bResting;
	const bool bMoved = !Motion.Location.Equals(GetActorLocation(), 0.5f);
	if (!bNow && (PublishTimer > 0.f || (bResting && !bMoved)))
	{
		return;
	}
	PublishTimer = bResting ? RestSendInterval : FloatSendInterval;
	bMotionKick = false;
	Motion.Location = GetActorLocation();
	Motion.Velocity = Velocity;
	Motion.ServerTime = GetServerNow();
	Motion.bResting = bResting;
	Motion.bSliding = bSliding;
	Motion.PushCount = PushSerial;
	if (bNow)
	{
		ForceNetUpdate();
	}
}

void APinguPickup::TickClient(float Dt)
{
	using namespace PinguPickupPrivate;
	// Age, bob and the take delay from the server clock: the same pop-in, bob and blink as on the server.
	Age = FMath::Max(0.f, static_cast<float>(GetServerNow() - SpawnTime));
	Bob = BobStart + Age;
	PickupDelay = FMath::Max(0.f, TakeDelay - Age);
	if (!HomeIsland.IsValid())
	{
		HomeIsland = ABlockIsland::FindNear(GetWorld(), GetActorLocation());
	}
	if (IsOnHook())
	{
		// On a fish hook: drawn on our copy of the hook (the line comes from the thrower's flipper as we see it).
		SimLocation = ReelHook->GetActorLocation();
		VisualOffset = FVector::ZeroVector;
		SetActorLocation(SimLocation);
		UpdateLook(Dt);
		return;
	}
	if (bHasMotion)
	{
		// Our own flight from the last snapshot (the same island collision as the server's), corrections fading out.
		MoveBubble(Dt, SimLocation);
		VisualOffset *= FMath::Exp(-CorrectionFade * Dt);
		if (VisualOffset.SizeSquared() < 0.01f)
		{
			VisualOffset = FVector::ZeroVector;
		}
		SetActorLocation(SimLocation + VisualOffset);
	}
	UpdateLook(Dt);
}

void APinguPickup::OnRep_Ability()
{
	SetupLook(); // the tint
}

void APinguPickup::OnRep_Motion()
{
	using namespace PinguPickupPrivate;
	const FVector Shown = GetActorLocation();
	SimLocation = Motion.Location;
	Velocity = Motion.Velocity;
	bResting = Motion.bResting;
	bSliding = Motion.bSliding;
	// The snapshot is a little old by now: fly it on by the time it took to get here.
	float Behind = FMath::Clamp(static_cast<float>(GetServerNow() - Motion.ServerTime), 0.f, MaxCatchUp);
	while (Behind > 1.e-3f)
	{
		const float StepTime = FMath::Min(Behind, 0.05f);
		MoveBubble(StepTime, SimLocation);
		Behind -= StepTime;
	}
	if (Motion.PushCount != SeenPushCount)
	{
		SeenPushCount = Motion.PushCount;
		Jiggle = bHasMotion ? 1.f : Jiggle; // pushed (not for the pushes from before we saw it)
	}
	// Keep showing it where it was and blend the correction out; the first snapshot (or a big jump) is there at once.
	VisualOffset = bHasMotion ? Shown - SimLocation : FVector::ZeroVector;
	if (VisualOffset.SizeSquared() > FMath::Square(SnapDistance))
	{
		VisualOffset = FVector::ZeroVector;
		bTrailStarted = false; // no glow streak across the jump
	}
	bHasMotion = true;
	SetActorLocation(SimLocation + VisualOffset);
}

bool APinguPickup::EscapeBlocks(FVector& InOutLocation) const
{
	const ABlockIsland* Island = HomeIsland.Get();
	if (!Island || !Island->IsSolidAt(InOutLocation))
	{
		return false;
	}
	FIntPoint Cell = Island->WorldToCell(InOutLocation);
	for (int32 Step = 0; Step < 64 && Island->IsSolidCell(Cell); ++Step)
	{
		++Cell.Y;
	}
	const float TopZ = static_cast<float>(Island->GetActorLocation().Z) + Cell.Y * Island->CellSize;
	InOutLocation.Z = TopZ + Radius + PinguPickupPrivate::BobAmplitude;
	return true;
}

bool APinguPickup::MoveBubble(float Dt, FVector& Location)
{
	using namespace PinguPickupPrivate;
	UWorld* World = GetWorld();
	if (!World || Dt <= 0.f)
	{
		return false;
	}
	const float SweepRadius = Radius * SweepScale;
	const FCollisionShape Sphere = FCollisionShape::MakeSphere(SweepRadius);
	const FCollisionObjectQueryParams WorldStatic(ECC_WorldStatic);
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguPickup), false, this);
	Location.Y = 0.f;

	if (bResting)
	{
		// An ice wall grew right under it: up on top.
		if (EscapeBlocks(Location))
		{
			bMotionKick = true;
			return true;
		}
		// Is the ground still there? The island crumbles in sudden death, bombs blow craters, ice melts.
		const float RestLift = Radius + BobAmplitude - SweepRadius; // hovers so the bob never dips into the snow
		FHitResult Ground;
		const bool bGround = World->SweepSingleByObjectType(Ground, Location + FVector(0.f, 0.f, 1.f),
			Location - FVector(0.f, 0.f, RestLift + 20.f), FQuat::Identity, WorldStatic, Sphere, Params);
		if (bGround && Ground.bStartPenetrating)
		{
			Location += PushOut(Ground);
			bResting = false;
		}
		else if (bGround && Ground.Normal.Z >= FlatNormalZ)
		{
			// Settles softly at its hover height.
			const float TargetZ = static_cast<float>(Ground.Location.Z) + RestLift;
			Location.Z += (TargetZ - Location.Z) * (1.f - FMath::Exp(-12.f * Dt));
			return true;
		}
		else
		{
			// Nothing under it any more (or only an edge: roll off it) - float down again.
			bResting = false;
			if (bGround)
			{
				Velocity = FVector(FMath::Sign(static_cast<float>(Ground.Normal.X)) * 60.f, 0.f, 0.f);
			}
		}
	}

	// Floating: gentle gravity up to a top fall speed, sideways drag (friction while rolling on the ground).
	float SpeedZ = static_cast<float>(Velocity.Z);
	if (SpeedZ > -FallSpeed)
	{
		SpeedZ = FMath::Max(SpeedZ - Gravity * Dt, -FallSpeed);
	}
	else
	{
		SpeedZ = -FallSpeed + (SpeedZ + FallSpeed) * FMath::Exp(-OverspeedDrag * Dt);
	}
	Velocity = FVector(Velocity.X * FMath::Exp(-(bSliding ? GroundFriction : AirDrag) * Dt), 0.f, SpeedZ);
	bSliding = false;

	float TimeLeft = Dt;
	for (int32 Iteration = 0; Iteration < 3 && TimeLeft > 1.e-4f; ++Iteration)
	{
		const FVector Delta = Velocity * TimeLeft;
		if (Delta.SizeSquared() < 1.e-6f)
		{
			break;
		}
		FHitResult Hit;
		if (!World->SweepSingleByObjectType(Hit, Location, Location + Delta, FQuat::Identity, WorldStatic, Sphere, Params))
		{
			Location += Delta;
			break;
		}
		if (Hit.bStartPenetrating)
		{
			// Started inside something (popped out inside a block, or a block appeared around it): step out.
			if (!EscapeBlocks(Location))
			{
				Location += PushOut(Hit);
			}
			bMotionKick = true;
			break;
		}
		FVector Normal(Hit.Normal.X, 0.f, Hit.Normal.Z);
		Normal = Normal.GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Normal = -Delta.GetSafeNormal();
		}
		Location = Hit.Location + Normal * Skin;
		TimeLeft *= 1.f - Hit.Time;
		const float Into = static_cast<float>(FVector::DotProduct(Velocity, Normal));
		if (Into >= 0.f)
		{
			continue;
		}
		if (Normal.Z > FloorNormalZ)
		{
			if (-Into > BounceSpeed)
			{
				// Landed fast: a small bounce (every machine flies its own bubble, so the puff is local).
				Velocity -= Normal * (Into * (1.f + Bounciness));
				Velocity.X *= 0.8f;
				Jiggle = 1.f;
				bMotionKick = true;
				APinguFX::Puffs(this, Hit.ImpactPoint, 2, 16.f, 80.f, Normal, 1.f);
			}
			else
			{
				Velocity -= Normal * Into; // rolls along the ground
				bSliding = true;
				if (Normal.Z >= FlatNormalZ && Velocity.SizeSquared() < FMath::Square(RestSpeed))
				{
					Velocity = FVector::ZeroVector;
					bResting = true;
					break;
				}
			}
		}
		else
		{
			// A wall or a ceiling: loses the speed into it, keeps the rest.
			Velocity -= Normal * Into;
			Jiggle = FMath::Max(Jiggle, 0.4f);
		}
	}
	Location.Y = 0.f;
	return true;
}

// ---------------------------------------------------------------------------- taking and popping

APenguinCharacter* APinguPickup::FindToucher() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	const FVector Center = GetIconLocation();
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		if (IsValid(Penguin) && Penguin->IsAlive() && !Penguin->IsKnockedOut() && !Penguin->IsHidden() && Penguin->DistanceTo(Center) < Radius)
		{
			return Penguin;
		}
	}
	return nullptr;
}

void APinguPickup::Collect(APenguinCharacter* Penguin)
{
	UPenguinAbilityComponent* Abilities = Penguin ? Penguin->GetAbilities() : nullptr;
	if (bTaken || !Abilities)
	{
		return;
	}
	bTaken = true;
	// A LAN player's new ability (and the HUD pop) reaches it through its ability component's replication.
	const int32 Slot = Abilities->GiveAbility(Ability);
	{
		FPinguFXBroadcastScope Broadcast(this);
		const FLinearColor Color = PinguPickupPrivate::BubbleColor(Ability);
		const FVector Chest = Penguin->GetAimOrigin();
		APinguFX::Sparkles(this, GetIconLocation(), 6, Radius * 0.6f, Color);
		APinguFX::Sparkles(this, Chest, 10, 40.f, Color);
		APinguFX::Ring(this, Chest, 90.f, Color, 0.3f);
		APinguFX::Sound(this, EPinguSound::Pickup, 0.9f, 0.03f);
	}
	UE_LOG(LogPingu, Display, TEXT("[PICKUP] %s took %s (slot %d)"), *Penguin->GetDisplayName(), UPenguinAbilityComponent::GetInfo(Ability).Name, Slot);
	Destroy(); // the clients' copies go with it
}

void APinguPickup::Pop(bool bSplash)
{
	using namespace PinguPickupPrivate;
	if (bTaken)
	{
		return;
	}
	bTaken = true;
	const FVector Where = GetIconLocation();
	{
		FPinguFXBroadcastScope Broadcast(this);
		if (bSplash)
		{
			const ABlockIsland* Island = HomeIsland.Get();
			APinguFX::Splash(this, FVector(Where.X, 0.f, Island ? Island->SeaLevel : DefaultSeaLevel));
			APinguFX::Sound(this, EPinguSound::Splash, 0.35f);
		}
		else
		{
			APinguFX::Sparkles(this, Where, 8, Radius * 0.8f, BubbleColor(Ability));
			APinguFX::Puffs(this, Where, 5, 18.f, 150.f, FVector::UpVector, 1.f, FLinearColor(0.85f, 0.95f, 1.f));
			APinguFX::Sound(this, EPinguSound::Shrink, 0.25f, 0.04f, 1.6f);
		}
	}
	Destroy(); // the clients' copies go with it
}
