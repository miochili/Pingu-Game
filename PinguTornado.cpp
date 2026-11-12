// Pingu - the tornado (the Gust ability): a short whirlwind round its user that sucks everything in and throws it out.

#include "PinguTornado.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguFX.h"
#include "PinguPickup.h"
#include "PinguPortal.h"
#include "PinguSettings.h"
#include "PinguTuning.h"
#include "Algo/Sort.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"

namespace PinguTornadoPrivate
{
	using namespace PinguTuning;

	/** Area abilities reach a normal penguin's middle 30 cm further than its capsule (they test the capsule's surface). */
	constexpr float NormalPenguinRadius = 30.f;
	/** Walls block it like every area ability: a line from the eye (or the user's head) to the other's middle or upper body. */
	constexpr float LineUpperBody = 0.95f;
	/** It stays this long after its end while its last wisps fade (no gameplay then). */
	constexpr float LingerTime = 0.15f;
	/** Things circling the eye close in on their circle in this long (s) and rise this far above the eye (cm, x size) - shots
	 *  a little, ice blocks round the user's head - on an ellipse this flat (its height / its width). */
	constexpr float OrbitCloseTime = 0.3f;
	constexpr float ShotRaise = 40.f;
	constexpr float IceRaise = 120.f;
	constexpr float OrbitFlatten = 0.55f;
	constexpr float IceSpinSpeed = 650.f;
	constexpr float MaxSteerSpeed = 3200.f;  // a shot is steered to its spot on the circle at most this fast
	/** Ability bubbles: drawn in with one push toward the eye (it takes about this long to get there), then thrown out. */
	constexpr float BubbleInTime = 0.4f;
	constexpr float BubbleThrowPush = 1500.f;
	constexpr float BubbleThrowLift = 550.f;
	/** A ripped ice block right above or below the eye still goes off this much sideways (never straight back down). */
	constexpr float IceMinSide = 0.45f;
	/** Shots fly out at least this much upward. */
	constexpr float ShotMinUp = 0.15f;
	/** A falling iceberg this close (cm) over a giant's eye is blown the way its first user faces (as the gust's). */
	constexpr float IcebergCenterBand = 20.f;

	// ---- a giant (merged) tornado's shape: Grown = its width / its users' size (2 for two plain ones)
	/** Its funnel is this share of its extra width taller (the foot stays at its users' feet), its foot - and its eye, and the
	 *  rings things circle in - this share wider. */
	constexpr float GrownHeightShare = 0.7f;
	constexpr float GrownFootShare = 0.5f;
	/** A giant's funnel widens to its new size about this fast after a merge (1/s, the interpolation's speed; 10 until
	 *  2026-10-27: the giant now pops up, see MergePop*). */
	constexpr float GrowSpeed = 20.f;
	/** The merge's flare of the giant's funnel and bands fades out over this long (s). */
	constexpr float MergeFlashTime = 0.35f;
	/** Tests and pieces: a giant has this many times the debris (at most) and the snow puffs. */
	constexpr int32 MaxDebrisTimes = 3;
	constexpr int32 MaxDustTimes = 2;

	// ---- the merge's look (2026-10-27, "make the animation when 2 nados combine way better"; all cosmetic, every machine
	// from the replicated drift and merges: the gameplay's timing is untouched)
	/** While two drift together each funnel bends toward where they meet: its top leans this share of the way there (its foot
	 *  stays at its eye; the lean builds up over the first MergeLeanBuild of the drift), at most this many funnel-top radii. */
	constexpr float MergeLeanShare = 0.55f;
	constexpr float MergeLeanBuild = 0.3f;
	constexpr float MergeLeanMaxTops = 1.2f;
	/** And it spins up: x (1 + this) by the end of the drift. */
	constexpr float MergeSpinUp = 1.4f;
	/** Squash and stretch of the giant when it pops up: its height x (1 + MergePopHeight x P), its width x (1 - MergePopWidth
	 *  x P), P = -cos(2 pi t / MergePopPeriod) x exp(-t / MergePopDecay): squashed wide at once, stretched tall at 0.15 s. */
	constexpr float MergePopHeight = 0.35f;
	constexpr float MergePopWidth = 0.22f;
	constexpr float MergePopPeriod = 0.3f;
	constexpr float MergePopDecay = 0.12f;
	constexpr float MergePopTime = 0.6f;
	/** After a merge its funnel widens to its new size this much faster. */
	constexpr float MergeGrowSpeed = 20.f;
	/** The swirl: snow spiralling in where they meet (the lead's), MergeSwirlTurns turns in all over the drift, closing in to
	 *  MergeSwirlClose of its start; flung out on a spiral over MergeSwirlFling when they are one. */
	constexpr int32 SwirlFull = 30;
	constexpr int32 SwirlLow = 12;
	constexpr float MergeSwirlTurns = 1.7f;
	constexpr float MergeSwirlClose = 0.25f;
	constexpr float MergeSwirlFling = 0.5f;
	/** Two shock rings of wind go out flat from the giant when they become one (at its foot and half way up). */
	constexpr int32 MergeRingCount = 2;
	constexpr int32 RingSegmentsFull = 18;
	constexpr int32 RingSegmentsLow = 10;
	constexpr float MergeRingTime = 0.42f;
	constexpr float MergeRingDepth = 0.45f; // a ring's circle toward the camera and away (Y) x this
	constexpr float MergeRingTilt = 0.18f;  // seen a little from above: its front lower than its back
	/** The merge sound starts with the drift (its thump lands as they become one); a drift seen this late gets none. */
	constexpr float MergeSoundLatest = 0.5f;

	// ---- the look (cm at size 1; the funnel stands on the eye)
	constexpr float FunnelBottom = -95.f;   // below the eye (about the user's feet)
	constexpr float FunnelTop = 250.f;
	constexpr float FunnelBottomRadius = 35.f;
	constexpr float FunnelTopRadius = 185.f;
	constexpr float FunnelDepth = 0.55f;    // its circles toward the camera and away (Y) x this
	constexpr float BandTilt = 0.22f;       // a band's circle seen a little from above: its front lower than its back
	constexpr float BandDepth = 0.12f;      // the bands stay near the play plane (their ribbons face the camera)
	constexpr float BandArc = 4.6f;         // each wind band goes this far round (radians)
	constexpr int32 BandLayersFull = 9;
	constexpr int32 BandSegmentsFull = 7;
	constexpr int32 BandLayersLow = 6;
	constexpr int32 BandSegmentsLow = 4;
	constexpr int32 DustFull = 16;           // (10 / 4 before 2026-10-27: the skirt round its foot is wider now)
	constexpr int32 DustLow = 6;
	constexpr int32 DebrisUltra = 26;
	constexpr int32 DebrisFull = 16;
	constexpr int32 DebrisLow = 6;
	constexpr float SpinRate = 9.f;         // rad/s at full speed (the wind-down slows it)
	constexpr float GrowTime = 0.12f;       // it forms this fast
	constexpr float InflowEvery = 0.022f;   // a streak of the inflow this often (x 1 / effects density) while it sucks
	static const FLinearColor BandColor(0.82f, 0.92f, 1.f);
	static const FLinearColor DustColor(0.92f, 0.96f, 1.f);
	static const FLinearColor DebrisColor(0.75f, 0.92f, 1.f);
	static const FLinearColor MergeColor(0.8f, 0.94f, 1.f);
	static const TCHAR* PuffMeshPath = TEXT("/Game/FX/SM_Puff.SM_Puff");
	static const TCHAR* FlakeMeshPath = TEXT("/Game/FX/SM_Flake.SM_Flake");
	static const TCHAR* GlowMeshPath = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
	static const TCHAR* SphereMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	static const TCHAR* FXMaterialPath = TEXT("/Game/FX/M_FX.M_FX");
	static const TCHAR* TrailMaterialPath = TEXT("/Game/FX/M_Trail.M_Trail");
	// ---- its shape (2026-10-27): a living twister, not a cone
	/** Its axis sways this far at the top (cm x size, x sqrt of a giant's growth). */
	constexpr float SwayAmplitude = 55.f;
	/** Its own tilt: its top stands off its eye up to LeanTilt x SwayAmplitude (x size; never less than LeanMin of that), and the
	 *  tilt wanders by up to its own share of that while it lives. */
	constexpr float LeanTilt = 1.3f;
	constexpr float LeanMin = 0.25f;   // (26 in the first pass: it still read as a cone)
	/** The skirt's puffs go round this far toward the camera and away (x their ring). */
	constexpr float SkirtDepth = 0.8f;
	/** The bits caught in it rise this many funnel heights a second. */
	constexpr float DebrisRise = 0.55f;
	/** The glass mesh: rings up its height x sides round it (Low fewer). */
	constexpr int32 GlassRingsFull = 14;
	constexpr int32 GlassSidesFull = 28;
	constexpr int32 GlassRingsLow = 10;
	constexpr int32 GlassSidesLow = 16;
	constexpr int32 GlassUpdateEvery = 3;
	/** Ultra's 3D swirls: layers of strands (inner to outer: how many, how far out x the funnel's radius, how fast they turn
	 *  x its spin), each strand this many pieces, really round it in depth (x its radius toward the camera and away). */
	constexpr int32 StrandLayersUltra = 3;
	constexpr int32 StrandsPerLayer[StrandLayersUltra] = { 8, 10, 12 };
	constexpr float StrandLayerScale[StrandLayersUltra] = { 0.72f, 0.93f, 1.12f };
	constexpr float StrandLayerSpin[StrandLayersUltra] = { 1.7f, 1.15f, 0.75f };
	constexpr int32 StrandSegments = 20;
	constexpr float StrandDepth = 0.9f;
	/** Strands are drawn wider when the camera is further than this (cm x size), up to x StrandViewBoostMax. */
	constexpr float StrandViewDistance = 1400.f;
	constexpr float StrandViewBoostMax = 2.5f;
	static const TCHAR* GhostMaterialPath = TEXT("/Game/FX/M_Ghost.M_Ghost");
	/** The twister's glass: see-through, densest where it faces the camera and fading out to nothing at its outline (so the
	 *  swirls draw its edge, not a hard rim), x the vertex colour's alpha (its top and foot fade). Tools/ue_setup_project.py
	 *  phase "tornado"; M_Ghost if it is missing. */
	static const TCHAR* GlassMaterialPath = TEXT("/Game/FX/M_TornadoGlass.M_TornadoGlass");
	constexpr float GlassOpacity = 0.5f;
	static const FLinearColor FunnelColor(0.78f, 0.9f, 1.f);
	constexpr float FunnelOpacity = 0.45f;   // M_Ghost: x 0.25 in the middle of the cone, up to x 1 at its rim
	constexpr int32 NumCustomData = 5; // R, G, B, emissive, opacity (as APinguFX's particles)

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

	static UInstancedStaticMeshComponent* MakeLookComponent(AActor* Owner, USceneComponent* Parent, const TCHAR* Name)
	{
		UInstancedStaticMeshComponent* Instances = Owner->CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
		Instances->SetupAttachment(Parent);
		Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Instances->SetGenerateOverlapEvents(false);
		Instances->SetCanEverAffectNavigation(false);
		Instances->SetCastShadow(false);
		// Wind lights nothing: out of Lumen, distance fields, ray tracing and the captures (as APinguFX's particles).
		Instances->bAffectDistanceFieldLighting = false;
		Instances->bAffectDynamicIndirectLighting = false;
		Instances->bVisibleInRayTracing = false;
		Instances->bVisibleInReflectionCaptures = false;
		Instances->bVisibleInRealTimeSkyCaptures = false;
		Instances->NumCustomDataFloats = NumCustomData;
		return Instances;
	}

	static float Smooth01(float X)
	{
		const float T = FMath::Clamp(X, 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}

	static float DistXZ(const FVector& A, const FVector& B)
	{
		return static_cast<float>(FVector::Dist(FVector(A.X, 0.f, A.Z), FVector(B.X, 0.f, B.Z)));
	}

	/** A giant made of two tornadoes (sizes and growths A and B): the biggest user's size, and its growth - the two widths added
	 *  up (at most the widest the biggest reach allows) over that size. */
	static void MergedShape(float SizeA, float GrownA, float SizeB, float GrownB, float& OutSize, float& OutGrown)
	{
		OutSize = FMath::Max(FMath::Max(SizeA, SizeB), 0.1f);
		const float Width = FMath::Min(SizeA * GrownA + SizeB * GrownB, OutSize * TornadoMergeMaxRadius / TornadoRadius);
		OutGrown = FMath::Max(Width / OutSize, 1.f);
	}

	/** Who hangs side by side in a tornado's eye at Eye until HoverFor from now - the ones still hanging in one (a knockback or a
	 *  freeze took the others out), in their order along X, TornadoMergeEyeGap between their capsules; GlideTime > 0: each drifts
	 *  to its place in that long (UPenguinMovementComponent::SetHoverGlide; on a LAN player's penguin it reaches its owner). */
	static void PlaceInEye(TArray<APenguinCharacter*> Who, const FVector& Eye, float GlideTime, float HoverFor)
	{
		Who.RemoveAll([](const APenguinCharacter* Penguin)
		{
			const UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
			return !Penguin || !Penguin->IsAlive() || !Move || !Move->IsHovering() || !Penguin->GetCapsuleComponent();
		});
		Algo::Sort(Who, [](const APenguinCharacter* A, const APenguinCharacter* B) { return A->GetActorLocation().X < B->GetActorLocation().X; });
		float Total = TornadoMergeEyeGap * static_cast<float>(FMath::Max(Who.Num() - 1, 0));
		for (const APenguinCharacter* Penguin : Who)
		{
			Total += 2.f * Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
		}
		float X = static_cast<float>(Eye.X) - Total * 0.5f;
		for (APenguinCharacter* Penguin : Who)
		{
			const float Half = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
			X += Half;
			const FVector Spot(X, 0.f, Eye.Z);
			X += Half + TornadoMergeEyeGap;
			UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
			if (Move->GetHoverRemaining() < HoverFor - 0.02f)
			{
				Move->SetHover(HoverFor);
			}
			if (GlideTime > 0.f)
			{
				// Its chest goes to the spot (the eye is at the chests).
				const FVector Chest = Penguin->GetAimOrigin();
				Move->SetHoverGlide(Penguin->GetActorLocation() + FVector(Spot.X - Chest.X, 0.f, Spot.Z - Chest.Z), GlideTime);
			}
		}
	}

	static FString NamesOf(const TArray<TObjectPtr<APenguinCharacter>>& Who)
	{
		FString Out;
		for (const TObjectPtr<APenguinCharacter>& Penguin : Who)
		{
			if (Penguin)
			{
				Out += (Out.IsEmpty() ? TEXT("") : TEXT(" + ")) + Penguin->GetDisplayName();
			}
		}
		return Out.IsEmpty() ? FString(TEXT("a")) : Out;
	}
}

APinguTornado::APinguTornado()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics; // its pull and steering are in place before the penguins and shots move
	SetActorEnableCollision(false);

	// LAN: spawned by the server only; every client draws it from the replicated numbers (its eye only moves in a merge,
	// and every machine works that out from the replicated drift).
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(10.f);
	NetPriority = 2.f;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = SceneRoot;
	Bands = PinguTornadoPrivate::MakeLookComponent(this, SceneRoot, TEXT("Bands"));
	Bands->SetTranslucentSortPriority(-1); // (soft glow ribbons, drawn like the effects' trails)
	Dust = PinguTornadoPrivate::MakeLookComponent(this, SceneRoot, TEXT("Dust"));
	Funnel = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Funnel"));
	Funnel->SetupAttachment(SceneRoot);
	Funnel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Funnel->SetGenerateOverlapEvents(false);
	Funnel->SetCanEverAffectNavigation(false);
	Funnel->SetCastShadow(false);
	Funnel->bAffectDistanceFieldLighting = false;
	Funnel->bAffectDynamicIndirectLighting = false;
	Funnel->bVisibleInRayTracing = false;
	Funnel->bVisibleInReflectionCaptures = false;
	Funnel->bVisibleInRealTimeSkyCaptures = false;
	Funnel->SetVisibility(false);
	Debris = PinguTornadoPrivate::MakeLookComponent(this, SceneRoot, TEXT("Debris"));
	Swirl = PinguTornadoPrivate::MakeLookComponent(this, SceneRoot, TEXT("Swirl"));
	Rings = PinguTornadoPrivate::MakeLookComponent(this, SceneRoot, TEXT("Rings"));
	Rings->SetTranslucentSortPriority(-1);
	Strands = PinguTornadoPrivate::MakeLookComponent(this, SceneRoot, TEXT("Strands"));
	Strands->SetTranslucentSortPriority(-1);
	LookRand.Initialize(0x70A4D0);
}

void APinguTornado::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(APinguTornado, User, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(APinguTornado, StartTime, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(APinguTornado, LookSeed, COND_InitialOnly);
	// (These change when it merges with another one.)
	DOREPLIFETIME(APinguTornado, Users);
	DOREPLIFETIME(APinguTornado, Center);
	DOREPLIFETIME(APinguTornado, CenterFrom);
	DOREPLIFETIME(APinguTornado, MoveStart);
	DOREPLIFETIME(APinguTornado, MoveTime);
	DOREPLIFETIME(APinguTornado, Radius);
	DOREPLIFETIME(APinguTornado, SizeScale);
	DOREPLIFETIME(APinguTornado, Grown);
	DOREPLIFETIME(APinguTornado, ThrowTime);
	DOREPLIFETIME(APinguTornado, EndTime);
	DOREPLIFETIME(APinguTornado, Merges);
	DOREPLIFETIME(APinguTornado, bMergeLead);
}

APinguTornado* APinguTornado::Spawn(APenguinCharacter* InUser, float InStrength, float ReachScale)
{
	using namespace PinguTornadoPrivate;
	UWorld* World = InUser ? InUser->GetWorld() : nullptr;
	if (!World || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	const FVector Chest = InUser->GetAimOrigin();
	const FVector Eye(Chest.X, 0.f, Chest.Z);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APinguTornado* Tornado = World->SpawnActor<APinguTornado>(Eye, FRotator::ZeroRotator, Params);
	if (!Tornado)
	{
		return nullptr;
	}
	// All set before its first replication (every client draws it from these).
	const double Now = ServerNow(World);
	Tornado->User = InUser;
	Tornado->Users = { InUser };
	Tornado->UserEyes = { Eye };
	Tornado->Center = Eye;
	Tornado->CenterFrom = Eye;
	Tornado->MoveStart = Now;
	Tornado->MoveTime = 0.f;
	Tornado->SizeScale = InUser->GetSizeScale();
	Tornado->Grown = 1.f;
	Tornado->FalloffRadius = TornadoRadius * Tornado->SizeScale;
	Tornado->Radius = Tornado->FalloffRadius * FMath::Max(ReachScale, 0.1f);
	Tornado->Strength = FMath::Max(InStrength, 0.f);
	Tornado->StartTime = Now;
	Tornado->LookSeed = FMath::RandRange(1, MAX_int32 - 1); // (its own look: two cast in the same frame differ too)
	Tornado->ThrowTime = Now + TornadoSuckTime;
	Tornado->EndTime = Now + TornadoTime;
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s's tornado: reach %.0f cm, strength x%.2f, at %s"), *InUser->GetDisplayName(), Tornado->Radius, Tornado->Strength,
		*Eye.ToCompactString());
	// The island's ice in reach leaves the island at once (it swirls from the next frame on); the user spins in its eye.
	// (Another tornado whose reach overlaps this one is found on its first tick: after the ability has hung its user up.)
	Tornado->bIceRipped = true;
	Tornado->RipIce(TornadoIceBlocks);
	InUser->PlayAction(EPenguinClip::Spin, 1.2f);
	return Tornado;
}

void APinguTornado::GetAll(const UWorld* World, TArray<APinguTornado*>& Out)
{
	Out.Reset();
	if (!World)
	{
		return;
	}
	for (TActorIterator<APinguTornado> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed())
		{
			Out.Add(*It);
		}
	}
}

void APinguTornado::DestroyAll(UWorld* World)
{
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	for (TActorIterator<APinguTornado> It(World); It; ++It)
	{
		It->Destroy();
	}
}

APenguinCharacter* APinguTornado::GetUser() const
{
	return User.Get();
}

bool APinguTornado::IsUser(const APenguinCharacter* Penguin) const
{
	return Penguin && Users.Contains(Penguin);
}

FVector APinguTornado::GetCenter() const
{
	using namespace PinguTornadoPrivate;
	if (MoveTime > 0.f)
	{
		const float Along = static_cast<float>((ServerNow(GetWorld()) - MoveStart) / MoveTime);
		if (Along < 1.f)
		{
			return FMath::Lerp(FVector(CenterFrom), FVector(Center), Smooth01(Along));
		}
	}
	return FVector(Center);
}

float APinguTornado::GetAge() const
{
	return FMath::Max(0.f, static_cast<float>(PinguTornadoPrivate::ServerNow(GetWorld()) - StartTime));
}

bool APinguTornado::HasThrown() const
{
	return HasAuthority() ? bThrown : StartTime > 0.0 && PinguTornadoPrivate::ServerNow(GetWorld()) >= ThrowTime;
}

float APinguTornado::GetShareAt(float Distance) const
{
	using namespace PinguTuning;
	return FMath::Lerp(TornadoPushCenterShare, TornadoPushEdgeShare, FMath::Clamp(Distance / FMath::Max(FalloffRadius, 1.f), 0.f, 1.f));
}

float APinguTornado::GetPushAt(float Distance) const
{
	return PinguTuning::NootPointBlankPush * GetShareAt(Distance) * Strength;
}

APenguinCharacter* APinguTornado::GetCreditFor(const FVector& At) const
{
	using namespace PinguTornadoPrivate;
	// A plain tornado's is its user's; a giant's: whose own tornado started nearest (it came out of that one's wind).
	APenguinCharacter* Best = User.Get();
	float BestDistance = TNumericLimits<float>::Max();
	for (int32 Index = 0; Index < Users.Num(); ++Index)
	{
		APenguinCharacter* Each = Users[Index].Get();
		if (!IsValid(Each))
		{
			continue;
		}
		const float Distance = DistXZ(UserEyes.IsValidIndex(Index) ? UserEyes[Index] : Each->GetActorLocation(), At);
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = Each;
		}
	}
	return Best;
}

int32 APinguTornado::GetIceHeld() const
{
	int32 Count = 0;
	for (const FHeldIce& Held : HeldIce)
	{
		const AHookedIceBlock* Block = Held.Block.Get();
		Count += Block && Block->IsSwirling() ? 1 : 0;
	}
	return Count;
}

float APinguTornado::GetSpread() const
{
	return FMath::Max(SizeScale, 0.2f) * (1.f + PinguTornadoPrivate::GrownFootShare * (FMath::Max(Grown, 1.f) - 1.f));
}

float APinguTornado::GetEyeRadius() const
{
	return PinguTuning::TornadoEyeRadius * GetSpread();
}

void APinguTornado::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Gone early (a new round): what it held falls where it is, the pulls end. (One taken in by another one handed all of that
	// over first.)
	if (HasAuthority() && EndPlayReason == EEndPlayReason::Destroyed && !bAbsorbed)
	{
		for (const FHeldIce& Held : HeldIce)
		{
			if (AHookedIceBlock* Block = Held.Block.Get())
			{
				Block->ThrowOut(FVector2D::ZeroVector, {});
			}
		}
		if (!bThrown)
		{
			for (const TWeakObjectPtr<APenguinCharacter>& Weak : Pulled)
			{
				APenguinCharacter* Penguin = Weak.Get();
				UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
				if (Move && Move->IsTornadoPulled())
				{
					Move->SetTornadoPull(FVector(Center), 0.f, Radius, 0.f);
				}
			}
		}
	}
	HeldIce.Reset();
	Super::EndPlay(EndPlayReason);
}

void APinguTornado::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(DeltaSeconds, 0.05f);
	if (HasAuthority())
	{
		// What it does to the world is seen (and heard) on every LAN machine.
		FPinguFXBroadcastScope Broadcast(this);
		TickGameplay(Dt);
	}
	if (!IsActorBeingDestroyed())
	{
		TickLook(Dt);
	}
}

// ---------------------------------------------------------------------------- gameplay (server)

bool APinguTornado::Reaches(const APenguinCharacter* Other) const
{
	using namespace PinguTornadoPrivate;
	if (!IsValid(Other) || IsUser(Other) || !Other->IsAlive() || Other->IsHidden())
	{
		return false;
	}
	const UPenguinMovementComponent* Move = Other->GetPenguinMovement();
	if (!Move || Move->IsHovering())
	{
		return false; // another tornado's user, anchored in its own eye
	}
	const FVector Eye = GetCenter();
	if (Other->DistanceTo(Eye) > Radius - NormalPenguinRadius)
	{
		return false;
	}
	// Walls shield what is behind them (its ice may be ripped out: then it reaches the penguin behind it too).
	const ABlockIsland* Walls = ABlockIsland::FindNear(GetWorld(), Eye);
	if (!Walls)
	{
		return true;
	}
	// From the chests and heads of its users hanging in it (as every area ability); a giant also from its eye (its users hang
	// side by side round it).
	bool bFromUsers = false;
	for (const TObjectPtr<APenguinCharacter>& Each : Users)
	{
		const APenguinCharacter* Penguin = Each.Get();
		const UPenguinMovementComponent* UserMove = Penguin ? Penguin->GetPenguinMovement() : nullptr;
		if (Penguin && Penguin->IsAlive() && UserMove && UserMove->IsHovering())
		{
			bFromUsers = true;
			if (UPenguinAbilityComponent::HasClearLine(Walls, Penguin, Other))
			{
				return true;
			}
		}
	}
	if (bFromUsers && Merges == 0)
	{
		return false;
	}
	const FVector OtherMiddle = Other->GetActorLocation();
	const FVector OtherUpper = OtherMiddle + FVector(0.f, 0.f, LineUpperBody * Other->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	return Walls->IsLineClear(Eye, OtherMiddle) || Walls->IsLineClear(Eye, OtherUpper);
}

FVector2D APinguTornado::OrbitPoint(const FVector& Eye, float Phase, float StartRadius, float OrbitRadius, float Raise, float SpinSpeed, float At) const
{
	using namespace PinguTornadoPrivate;
	// From where it was when the wind took it (Phase, StartRadius round the eye) onto the circle round the eye, closing in over
	// OrbitCloseTime, while it is swept round (faster as it closes in: SpinSpeed along its circle).
	const float Close = Smooth01(At / OrbitCloseTime);
	const float Ring = FMath::Lerp(StartRadius, OrbitRadius, Close);
	const float Angle = Phase + SpinSpeed / FMath::Max(OrbitRadius, 50.f) * At * Close;
	const float Flat = FMath::Lerp(1.f, OrbitFlatten, Close);
	return FVector2D(Eye.X + Ring * FMath::Cos(Angle), Eye.Z + Raise * Close + Ring * Flat * FMath::Sin(Angle));
}

void APinguTornado::TickGameplay(float Dt)
{
	using namespace PinguTornadoPrivate;
	if (bAbsorbed)
	{
		return;
	}
	const double Now = ServerNow(GetWorld());
	if (!bIceRipped)
	{
		bIceRipped = true; // (Spawn rips it out right away)
		RipIce(TornadoIceBlocks);
	}
	// Two tornadoes whose reaches overlap drift together, then the lead takes the other in.
	if (APinguTornado* Partner = MergeWith.Get())
	{
		if (bMergeLead && Now >= MergeAt)
		{
			Absorb(Partner, Now);
		}
	}
	else
	{
		MergeWith.Reset();
		bMergeLead = false;
		TryLink(Now);
	}
	if (!bThrown)
	{
		if (Now < ThrowTime)
		{
			Suck(Dt);
		}
		else
		{
			ThrowOut();
		}
	}
	if (Now >= EndTime + LingerTime)
	{
		Destroy();
	}
}

bool APinguTornado::CanMerge(double Now) const
{
	return !bAbsorbed && !MergeWith.IsValid() && !IsActorBeingDestroyed() && Now < EndTime - PinguTuning::TornadoMergeMinLeft;
}

void APinguTornado::TryLink(double Now)
{
	using namespace PinguTornadoPrivate;
	if (!CanMerge(Now))
	{
		return;
	}
	TArray<APinguTornado*> All;
	GetAll(GetWorld(), All);
	const FVector Eye = GetCenter();
	APinguTornado* Nearest = nullptr;
	float NearestDistance = TNumericLimits<float>::Max();
	for (APinguTornado* Other : All)
	{
		if (Other == this || !Other->CanMerge(Now))
		{
			continue;
		}
		// Their reaches overlap: they suck each other in.
		const float Distance = DistXZ(Eye, Other->GetCenter());
		if (Distance < Radius + Other->Radius && Distance < NearestDistance)
		{
			NearestDistance = Distance;
			Nearest = Other;
		}
	}
	if (!Nearest)
	{
		return;
	}
	// The bigger one takes the other in (of two alike, the older one).
	const bool bWeLead = Radius > Nearest->Radius + 1.f || (FMath::Abs(Radius - Nearest->Radius) <= 1.f && StartTime <= Nearest->StartTime);
	if (bWeLead)
	{
		BeginMerge(Nearest, Now);
	}
	else
	{
		Nearest->BeginMerge(this, Now);
	}
}

void APinguTornado::DriftTo(const FVector& To, float Seconds, double Now)
{
	CenterFrom = GetCenter();
	Center = FVector(To.X, 0.f, To.Z);
	MoveStart = Now;
	MoveTime = FMath::Max(Seconds, 0.f);
}

void APinguTornado::RepullAll(const FVector& Eye, float Reach, double Now)
{
	using namespace PinguTornadoPrivate;
	const float Left = FMath::Max(static_cast<float>(ThrowTime - Now), 0.f) + 0.05f;
	for (const TWeakObjectPtr<APenguinCharacter>& Weak : Pulled)
	{
		APenguinCharacter* Penguin = Weak.Get();
		UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
		if (Move && Move->IsTornadoPulled() && !IsUser(Penguin))
		{
			Move->SetTornadoPull(Eye, Left, Reach, GetEyeRadius());
		}
	}
}

void APinguTornado::PlaceUsers(const FVector& Eye, float GlideTime, double Now)
{
	TArray<APenguinCharacter*> Who;
	for (const TObjectPtr<APenguinCharacter>& Each : Users)
	{
		Who.Add(Each.Get());
	}
	PinguTornadoPrivate::PlaceInEye(Who, Eye, GlideTime, FMath::Max(static_cast<float>(EndTime - Now), 0.f));
}

void APinguTornado::BeginMerge(APinguTornado* Other, double Now)
{
	using namespace PinguTornadoPrivate;
	if (!Other || Other == this)
	{
		return;
	}
	// They meet between their eyes, nearer the bigger one's (weighted by their reaches).
	const FVector EyeA = GetCenter();
	const FVector EyeB = Other->GetCenter();
	const float WeightA = FMath::Max(Radius, 1.f);
	const float WeightB = FMath::Max(Other->Radius, 1.f);
	FVector Meet = (EyeA * WeightA + EyeB * WeightB) / (WeightA + WeightB);
	Meet.Y = 0.f;
	// Neither throws on its own before the merge; the giant sucks on after it, then throws, and lasts until the later of the two
	// would have ended plus a little.
	const double At = Now + TornadoMergeDriftTime;
	const double Throw = At + TornadoMergeSuckTime;
	const double End = FMath::Max(FMath::Max(EndTime, Other->EndTime) + TornadoMergeExtraTime, Throw + TornadoMergeMinWindDown);
	for (APinguTornado* Each : { this, Other })
	{
		Each->MergeWith = Each == this ? Other : this;
		Each->bMergeLead = Each == this;
		Each->MergeAt = At;
		Each->ThrowTime = Throw;
		Each->EndTime = End;
		Each->DriftTo(Meet, TornadoMergeDriftTime, Now);
		Each->ForceNetUpdate();
	}
	// What the two pull in goes where they will be one; everybody hanging in them drifts there in their hover, side by side.
	const float Reach = FMath::Min(Radius + Other->Radius, TornadoMergeMaxRadius);
	RepullAll(Meet, Reach, Now);
	Other->RepullAll(Meet, Reach, Now);
	TArray<APenguinCharacter*> Who;
	for (const TObjectPtr<APenguinCharacter>& Each : Users)
	{
		Who.AddUnique(Each.Get());
	}
	for (const TObjectPtr<APenguinCharacter>& Each : Other->Users)
	{
		Who.AddUnique(Each.Get());
	}
	PlaceInEye(Who, Meet, TornadoMergeDriftTime, static_cast<float>(End - Now));
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s's and %s's tornadoes (%.0f cm apart) suck each other in: one in %.2f s at %s, reach %.0f cm"), *NamesOf(Users),
		*NamesOf(Other->Users), DistXZ(EyeA, EyeB), TornadoMergeDriftTime, *Meet.ToCompactString(), Reach);
}

void APinguTornado::Absorb(APinguTornado* Other, double Now)
{
	using namespace PinguTornadoPrivate;
	MergeWith.Reset();
	bMergeLead = false;
	MergeAt = 0.0;
	if (!Other || Other == this || Other->bAbsorbed)
	{
		return;
	}
	// The drift is over: the eye stays where they met.
	const FVector Eye(Center.X, 0.f, Center.Z);
	CenterFrom = Center;
	MoveStart = Now;
	MoveTime = 0.f;
	const FString OtherNames = NamesOf(Other->Users);

	// Reach and push: the two added up (the push's falloff stretched over the new reach).
	Radius = FMath::Min(Radius + Other->Radius, TornadoMergeMaxRadius);
	FalloffRadius = FMath::Min(FalloffRadius + Other->FalloffRadius, TornadoMergeMaxRadius);
	Strength = FMath::Min(Strength + Other->Strength, TornadoMergeMaxStrength);
	float NewSize = SizeScale;
	float NewGrown = Grown;
	MergedShape(SizeScale, Grown, Other->SizeScale, Other->Grown, NewSize, NewGrown);
	SizeScale = NewSize;
	Grown = NewGrown;

	// Its users, and where each one's own tornado started (the credit for what it throws).
	for (int32 Index = 0; Index < Other->Users.Num(); ++Index)
	{
		APenguinCharacter* Each = Other->Users[Index].Get();
		if (Each && !Users.Contains(Each))
		{
			Users.Add(Each);
			UserEyes.Add(Other->UserEyes.IsValidIndex(Index) ? Other->UserEyes[Index] : Each->GetActorLocation());
		}
	}

	// What both hold now circles the giant's eye: each thing closes in on its ring from where it is.
	const float AgeNow = GetAge();
	SuckedShots.Append(Other->SuckedShots);
	for (FHeldShot& Held : SuckedShots)
	{
		if (const APenguinProjectile* Shot = Held.Shot.Get())
		{
			const FVector At = Shot->GetActorLocation();
			Held.Phase = FMath::Atan2(static_cast<float>(At.Z - Eye.Z), static_cast<float>(At.X - Eye.X));
			Held.StartRadius = FMath::Max(DistXZ(At, Eye), 1.f);
			Held.StartAge = AgeNow;
		}
	}
	HeldIce.Append(Other->HeldIce);
	for (FHeldIce& Held : HeldIce)
	{
		if (const AHookedIceBlock* Block = Held.Block.Get())
		{
			const FVector At = Block->GetCenter();
			Held.Phase = FMath::Atan2(static_cast<float>(At.Z - Eye.Z), static_cast<float>(At.X - Eye.X));
			Held.StartRadius = FMath::Max(DistXZ(At, Eye), 1.f);
			Held.StartAge = AgeNow;
			Held.Last = FVector2D(At.X, At.Z);
		}
	}
	for (const TWeakObjectPtr<APinguPickup>& Bubble : Other->SuckedBubbles)
	{
		SuckedBubbles.AddUnique(Bubble);
	}
	for (const TWeakObjectPtr<AIcebergDrop>& Iceberg : Other->DrawnIcebergs)
	{
		DrawnIcebergs.AddUnique(Iceberg);
	}
	Throws.Append(Other->Throws);
	RippedCells.Append(Other->RippedCells);
	IceRipped += Other->IceRipped;
	ShotsThrown += Other->ShotsThrown;
	// The penguins either one is pulling in now (one thrown already can be sucked in again); never a user.
	for (const TWeakObjectPtr<APenguinCharacter>& Weak : Other->Pulled)
	{
		Pulled.AddUnique(Weak);
	}
	Pulled.RemoveAll([this, &Eye](const TWeakObjectPtr<APenguinCharacter>& Weak)
	{
		APenguinCharacter* Penguin = Weak.Get();
		UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
		if (Move && IsUser(Penguin) && Move->IsTornadoPulled())
		{
			Move->SetTornadoPull(Eye, 0.f, Radius, 0.f);
		}
		return !Move || !Move->IsTornadoPulled() || IsUser(Penguin);
	});

	// The other one goes (what it held is ours now: its end hands nothing back).
	Other->bAbsorbed = true;
	Other->MergeWith.Reset();
	Other->HeldIce.Reset();
	Other->Pulled.Reset();
	Other->SuckedShots.Reset();
	Other->Destroy();

	// It sucks again until its throw (set when they started drifting), then throws as usual.
	Merges = static_cast<uint8>(FMath::Min(static_cast<int32>(Merges) + 1, 255));
	bThrown = false;
	ThrowTime = FMath::Max(ThrowTime, Now + 0.05);
	EndTime = FMath::Max(EndTime, ThrowTime + TornadoMergeMinWindDown);
	// More of the island's ice in its bigger reach; the pulls toward its eye with its reach; its users hang on in its eye.
	RipIce(TornadoMergeIceBlocks);
	RepullAll(Eye, Radius, Now);
	PlaceUsers(Eye, 0.f, Now);
	ForceNetUpdate();
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s's tornado takes in %s's: one giant tornado (%d merged) at %s, reach %.0f cm, push x%.2f, falloff over %.0f cm, %d user(s), throws in %.2f s, ends in %.2f s, holds %d ice block(s)"),
		*NamesOf(TArray<TObjectPtr<APenguinCharacter>>{ User }), *OtherNames, static_cast<int32>(Merges), *Eye.ToCompactString(), Radius, Strength, FalloffRadius, Users.Num(),
		static_cast<float>(ThrowTime - Now), static_cast<float>(EndTime - Now), GetIceHeld());
}

void APinguTornado::RipIce(int32 MaxBlocks)
{
	using namespace PinguTornadoPrivate;
	const FVector Eye = GetCenter();
	ABlockIsland* Isle = ABlockIsland::FindNear(GetWorld(), Eye);
	if (!Isle || IceRipped >= MaxBlocks)
	{
		return;
	}
	// Never the floor its users stand on (the row under their feet and a block to either side).
	struct FSupport
	{
		FIntPoint Left;
		FIntPoint Right;
	};
	TArray<FSupport, TInlineAllocator<4>> Supports;
	for (const TObjectPtr<APenguinCharacter>& Each : Users)
	{
		const APenguinCharacter* Penguin = Each.Get();
		if (Penguin && Penguin->GetCapsuleComponent())
		{
			const FVector Feet = Penguin->GetFeetLocation();
			const float FeetRadius = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
			Supports.Add(FSupport{ Isle->WorldToCell(Feet + FVector(-FeetRadius, 0.f, -10.f)) - FIntPoint(1, 0), Isle->WorldToCell(Feet + FVector(FeetRadius, 0.f, -10.f)) + FIntPoint(1, 0) });
		}
	}
	// Every ice block it reaches: its middle in reach, its face toward the eye open and nothing solid on the way there (walls
	// shield what is behind them: one layer). Nearest first, until it has ripped MaxBlocks out.
	const FIntPoint EyeCell = Isle->WorldToCell(Eye);
	const int32 Cells = FMath::CeilToInt(Radius / Isle->CellSize) + 1;
	TArray<TPair<float, FIntPoint>, TInlineAllocator<64>> Found;
	for (int32 X = EyeCell.X - Cells; X <= EyeCell.X + Cells; ++X)
	{
		for (int32 Z = EyeCell.Y - Cells; Z <= EyeCell.Y + Cells; ++Z)
		{
			if (Isle->GetBlock(X, Z) != EBlockKind::Ice
				|| Supports.ContainsByPredicate([X, Z](const FSupport& Support) { return Z == Support.Left.Y && X >= Support.Left.X && X <= Support.Right.X; }))
			{
				continue;
			}
			const FVector CellMiddle = Isle->CellToWorld(FIntPoint(X, Z));
			const float Dx = static_cast<float>(CellMiddle.X - Eye.X);
			const float Dz = static_cast<float>(CellMiddle.Z - Eye.Z);
			const float Dist = FMath::Sqrt(Dx * Dx + Dz * Dz);
			if (Dist > Radius)
			{
				continue;
			}
			const bool bAlongX = FMath::Abs(Dx) >= FMath::Abs(Dz);
			const FIntPoint Toward = bAlongX ? FIntPoint(Dx > 0.f ? X - 1 : X + 1, Z) : FIntPoint(X, Dz > 0.f ? Z - 1 : Z + 1);
			if (Isle->IsSolidCell(Toward) || !Isle->IsLineClear(Eye, Isle->CellToWorld(Toward)))
			{
				continue;
			}
			Found.Add(TPair<float, FIntPoint>(Dist, FIntPoint(X, Z)));
		}
	}
	Found.Sort([](const TPair<float, FIntPoint>& A, const TPair<float, FIntPoint>& B) { return A.Key < B.Key; });
	const float AgeNow = GetAge();
	int32 RippedNow = 0;
	for (const TPair<float, FIntPoint>& Entry : Found)
	{
		if (IceRipped >= MaxBlocks)
		{
			break;
		}
		FIntPoint Taken;
		const FVector CellMiddle = Isle->CellToWorld(Entry.Value);
		if (AHookedIceBlock* Block = AHookedIceBlock::RipOut(Isle, CellMiddle, GetCreditFor(CellMiddle), &Taken))
		{
			const FVector At = Block->GetCenter();
			FHeldIce& Held = HeldIce.AddDefaulted_GetRef();
			Held.Block = Block;
			Held.Phase = FMath::Atan2(static_cast<float>(At.Z - Eye.Z), static_cast<float>(At.X - Eye.X));
			Held.StartRadius = FMath::Max(Entry.Key, 1.f);
			Held.StartAge = AgeNow;
			Held.Last = FVector2D(At.X, At.Z);
			RippedCells.Add(Taken);
			++IceRipped;
			++RippedNow;
		}
	}
	if (RippedNow > 0)
	{
		APinguFX::Sound(this, EPinguSound::IcicleHit, 0.85f, 0.05f, 0.55f); // one deep crack of ice for all of them
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s's tornado rips %d ice block(s) out of the island"), *NamesOf(Users), RippedNow);
	}
}

void APinguTornado::Suck(float Dt)
{
	using namespace PinguTornadoPrivate;
	UWorld* World = GetWorld();
	const float Age = GetAge();
	const float Left = FMath::Max(static_cast<float>(ThrowTime - ServerNow(World)), 0.f);
	// Things circle the eye (it drifts while two merge); penguins are pulled where it is going (where two will be one).
	const FVector Eye = GetCenter();
	const FVector PullTo(Center.X, 0.f, Center.Z);
	const float Spread = GetSpread();

	// Penguins: each one it reaches is pulled toward the eye (once: the pull runs where its movement runs).
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Other = *It;
		if (Pulled.Contains(Other) || !Reaches(Other))
		{
			continue;
		}
		if (UPenguinMovementComponent* Move = Other->GetPenguinMovement())
		{
			// (Until the throw, and a frame more: the throw ends it.)
			Move->SetTornadoPull(PullTo, Left + 0.05f, Radius, GetEyeRadius());
			Pulled.Add(Other);
			UE_LOG(LogPingu, Display, TEXT("[GAME] %s is sucked into %s's tornado from %.0f cm"), *Other->GetDisplayName(), *NamesOf(Users),
				DistXZ(Other->GetActorLocation(), Eye));
		}
	}

	// Shots (never its users' own): they circle the eye, and they never hit its users meanwhile.
	TArray<APenguinProjectile*> Shots;
	APenguinProjectile::GetAll(World, Shots);
	for (APenguinProjectile* Shot : Shots)
	{
		if (!Shot->CanBeSucked() || IsUser(Shot->GetShooter()))
		{
			continue;
		}
		FHeldShot* Held = SuckedShots.FindByPredicate([Shot](const FHeldShot& Entry) { return Entry.Shot.Get() == Shot; });
		const FVector At = Shot->GetActorLocation();
		if (!Held)
		{
			const float Dist = DistXZ(At, Eye);
			if (Dist > Radius + Shot->GetRadius())
			{
				continue;
			}
			Held = &SuckedShots.AddDefaulted_GetRef();
			Held->Shot = Shot;
			Held->Phase = FMath::Atan2(static_cast<float>(At.Z - Eye.Z), static_cast<float>(At.X - Eye.X));
			Held->StartRadius = FMath::Max(Dist, 1.f);
			Held->StartAge = Age;
		}
		for (const TObjectPtr<APenguinCharacter>& Each : Users)
		{
			if (Each)
			{
				Shot->IgnorePenguin(Each.Get(), Left + 0.4f);
			}
		}
		const FVector2D Spot = OrbitPoint(Eye, Held->Phase, Held->StartRadius, TornadoOrbitRadius * Spread, ShotRaise * Spread, TornadoSpinSpeed, Age - Held->StartAge + Dt);
		FVector2D Way(Spot.X - At.X, Spot.Y - At.Z);
		Way /= FMath::Max(Dt, 1.e-3f);
		if (Way.Size() > MaxSteerSpeed)
		{
			Way = Way.GetSafeNormal() * MaxSteerSpeed;
		}
		// (Plus what its own gravity takes off this frame: it gets to its spot.)
		Shot->SetFlightVelocity(FVector(Way.X, 0.f, Way.Y + Shot->GetGravity() * Dt));
	}

	// Ability bubbles: one push toward the eye each (they float in).
	TArray<APinguPickup*> Bubbles;
	APinguPickup::GetAll(World, Bubbles);
	for (APinguPickup* Bubble : Bubbles)
	{
		if (SuckedBubbles.Contains(Bubble) || Bubble->IsOnHook())
		{
			continue;
		}
		FVector To = PullTo - Bubble->GetIconLocation();
		To.Y = 0.f;
		const float Dist = static_cast<float>(To.Size());
		if (Dist > Radius + Bubble->GetRadius())
		{
			continue;
		}
		SuckedBubbles.Add(Bubble);
		const float Room = FMath::Max(Dist - TornadoOrbitRadius * Spread, 0.f);
		Bubble->Push(To.GetSafeNormal() * (Room / BubbleInTime) + FVector(0.f, 0.f, 300.f));
	}

	// Falling icebergs (not its users' own): drawn toward the eye once.
	for (TActorIterator<AIcebergDrop> It(World); It; ++It)
	{
		AIcebergDrop* Iceberg = *It;
		if (!Iceberg->IsFalling() || Iceberg->IsActorBeingDestroyed() || DrawnIcebergs.Contains(Iceberg) || IsUser(Iceberg->GetDropOwner())
			|| !Iceberg->IsInArea(Eye, Radius, FVector::ZeroVector, -2.f))
		{
			continue;
		}
		DrawnIcebergs.Add(Iceberg);
		const float MiddleX = (Iceberg->GetLeftX() + Iceberg->GetRightX()) * 0.5f;
		if (FMath::Abs(MiddleX - static_cast<float>(PullTo.X)) > 20.f)
		{
			Iceberg->Blow(static_cast<float>(PullTo.X) - MiddleX, TornadoIcebergPull);
		}
	}

	// The island's ice it ripped out: round its users' heads.
	for (FHeldIce& Held : HeldIce)
	{
		AHookedIceBlock* Block = Held.Block.Get();
		if (!Block || !Block->IsSwirling())
		{
			continue;
		}
		const FVector2D Spot = OrbitPoint(Eye, Held.Phase, Held.StartRadius, TornadoIceOrbitRadius * Spread, IceRaise * Spread, IceSpinSpeed, Age - Held.StartAge + Dt);
		const FVector2D Velocity2D = (Spot - Held.Last) / FMath::Max(Dt, 1.e-3f);
		Held.Last = Spot;
		Block->Swirl(Spot, Velocity2D);
	}
}

void APinguTornado::ThrowOut()
{
	using namespace PinguTornadoPrivate;
	bThrown = true;
	UWorld* World = GetWorld();
	APenguinCharacter* FirstUser = User.Get();
	const float Facing = FirstUser ? FirstUser->GetFacing() : 1.f;
	const FVector Eye = GetCenter();

	// The pulls end first (a penguin the throw no longer reaches, or one whose hit clashes, is free again).
	for (const TWeakObjectPtr<APenguinCharacter>& Weak : Pulled)
	{
		APenguinCharacter* Penguin = Weak.Get();
		UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
		if (Move && Move->IsTornadoPulled())
		{
			Move->SetTornadoPull(Eye, 0.f, Radius, 0.f);
		}
	}
	// (One that had thrown before a merge pulls in again from scratch.)
	Pulled.Reset();

	// Penguins: a normal hit each (its push x its strength and the traits' rule, the fight's growth, sizes: in
	// ApplyPenguinHit), the nearer the middle the harder - a giant's hit is the user's whose own tornado started nearest.
	TArray<TWeakObjectPtr<APenguinCharacter>> Thrown;
	int32 ThrownNow = 0;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Other = *It;
		if (!Reaches(Other))
		{
			continue;
		}
		FVector D = Other->GetActorLocation() - Eye;
		D.Y = 0.f;
		const float Dist = static_cast<float>(D.Size());
		FVector Dir = Dist > 1.f ? D / Dist : FVector(Facing, 0.f, 0.f);
		Dir.Z = FMath::Max(static_cast<float>(Dir.Z), TornadoMinUp);
		Dir.Normalize();
		const float Along = FMath::Clamp(Dist / FMath::Max(FalloffRadius, 1.f), 0.f, 1.f);
		const UPenguinMovementComponent* OtherMove = Other->GetPenguinMovement();
		const float Frozen = OtherMove && OtherMove->IsFrozen() ? TornadoFrozenBonus : 1.f;
		APenguinCharacter* By = GetCreditFor(Other->GetActorLocation());
		FThrowRecord Record;
		Record.Penguin = Other;
		Record.By = By;
		Record.Distance = Dist;
		Record.Share = GetShareAt(Dist);
		const FVector2D Push = FVector2D(Dir.X, Dir.Z) * NootPointBlankPush * Record.Share * Frozen;
		Record.Knockback = Push + FVector2D(0.f, FMath::Lerp(TornadoLiftCenter, TornadoLiftEdge, Along));
		if (Other->IsShielded() && !Other->IsBraced())
		{
			// The shell blocks the hit, but the wind blows the bubble along (no stun: it can steer back).
			Record.bShell = true;
			if (UPenguinMovementComponent* ShellMove = Other->GetPenguinMovement())
			{
				ShellMove->AddAbilityImpulse(Push * Strength * ShellWindShare + FVector2D(0.f, ShellWindLift));
			}
			Other->RememberAttacker(By); // blown into the sea, it was the tornado's user's knock-out
			APinguFX::Sparkles(this, Other->GetAimOrigin(), 6, 60.f, FLinearColor(0.6f, 0.95f, 1.f));
			APinguFX::Sound(this, EPinguSound::ShieldBlock, 0.5f, 0.06f, 1.3f);
		}
		else
		{
			FPenguinHit Hit;
			Hit.Instigator = By;
			Hit.Source = EPenguinAbility::Gust;
			Hit.Strength = Strength;
			Hit.Knockback = Record.Knockback;
			Hit.Stun = FMath::Lerp(TornadoStunCenter, TornadoStunEdge, Along);
			Hit.Location = Other->GetActorLocation();
			Record.bHit = Other->ApplyPenguinHit(Hit);
		}
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s's tornado throws %s from %.0f cm: x%.2f of the noot's push x%.2f, knock (%.0f, %.0f)%s%s"),
			*NamesOf(Users), *Other->GetDisplayName(), Dist, Record.Share, Strength, Record.Knockback.X, Record.Knockback.Y,
			Record.bShell ? TEXT(" - its shell took it") : (Record.bHit ? TEXT("") : TEXT(" - no hit")),
			Merges > 0 && By ? *FString::Printf(TEXT(" - %s's hit"), *By->GetDisplayName()) : TEXT(""));
		Throws.Add(Record);
		Thrown.Add(Other);
		++ThrownNow;
	}

	// Shots: out as a user's own; icicle pegs and ground pads in reach are torn out and fly on as its icicles.
	TArray<APenguinProjectile*> Shots;
	APenguinProjectile::GetAll(World, Shots);
	int32 ShotsNow = 0;
	for (APenguinProjectile* Shot : Shots)
	{
		if (IsUser(Shot->GetShooter()))
		{
			continue; // its users' own shots, pegs and pads stay
		}
		FVector D = Shot->GetActorLocation() - Eye;
		D.Y = 0.f;
		const float Dist = static_cast<float>(D.Size());
		const bool bHeld = SuckedShots.ContainsByPredicate([Shot](const FHeldShot& Entry) { return Entry.Shot.Get() == Shot; });
		if (!bHeld && Dist > Radius + Shot->GetRadius())
		{
			continue;
		}
		APenguinCharacter* By = GetCreditFor(Shot->GetActorLocation());
		FVector Dir = Dist > 1.f ? D / Dist : FVector::UpVector;
		if (Shot->HasPeg() || Shot->IsGroundPad())
		{
			Shot->BlowOut(By, Dir);
			continue;
		}
		if (!Shot->CanBeSucked())
		{
			continue;
		}
		Dir.Z = FMath::Max(static_cast<float>(Dir.Z), ShotMinUp);
		Dir.Normalize();
		const float Speed = FMath::Lerp(TornadoShotThrowCenter, TornadoShotThrowEdge, FMath::Clamp(Dist / FMath::Max(FalloffRadius, 1.f), 0.f, 1.f));
		Shot->Push(By, Dir, Speed);
		++ShotsThrown;
		++ShotsNow;
	}
	const int32 SuckedNow = SuckedShots.Num();

	// Ability bubbles, falling icebergs, other penguins' portals (never its users' own).
	APinguPickup::PushInArea(World, Eye, Radius, BubbleThrowPush, FVector::ZeroVector, -2.f, BubbleThrowLift);
	if (Merges == 0)
	{
		AIcebergDrop::BlowInArea(World, FirstUser, Eye, Radius, FVector::UpVector, -2.f, TornadoIcebergStrength);
		APinguPortal::CloseInArea(World, FirstUser, Eye, Radius);
	}
	else
	{
		for (TActorIterator<AIcebergDrop> It(World); It; ++It)
		{
			AIcebergDrop* Iceberg = *It;
			if (!Iceberg->IsFalling() || Iceberg->IsActorBeingDestroyed() || IsUser(Iceberg->GetDropOwner()) || !Iceberg->IsInArea(Eye, Radius, FVector::UpVector, -2.f))
			{
				continue;
			}
			const float Dx = (Iceberg->GetLeftX() + Iceberg->GetRightX()) * 0.5f - static_cast<float>(Eye.X);
			Iceberg->Blow(FMath::Abs(Dx) > IcebergCenterBand ? Dx : Facing, TornadoIcebergStrength);
		}
		TArray<APinguPortal*> Portals;
		APinguPortal::GetAll(World, Portals);
		for (APinguPortal* Portal : Portals)
		{
			if (IsUser(Portal->GetPortalOwner()))
			{
				continue;
			}
			for (int32 End = 0; End < 2; ++End)
			{
				if (DistXZ(Portal->GetEnd(End), Eye) <= Radius + APinguPortal::EndHalfWidth)
				{
					UE_LOG(LogPingu, Display, TEXT("[GAME] a giant tornado blows out %s's portal"), Portal->GetPortalOwner() ? *Portal->GetPortalOwner()->GetDisplayName() : TEXT("nobody"));
					Portal->Close();
					break;
				}
			}
		}
	}

	// The ice it ripped out: away from the eye and up, heavy - it comes down as falling ice.
	int32 IceNow = 0;
	for (const FHeldIce& Held : HeldIce)
	{
		AHookedIceBlock* Block = Held.Block.Get();
		if (!Block || !Block->IsSwirling())
		{
			continue;
		}
		const FVector At = Block->GetCenter();
		float AwayX = static_cast<float>(At.X - Eye.X);
		float AwayZ = static_cast<float>(At.Z - Eye.Z);
		const float Length = FMath::Sqrt(AwayX * AwayX + AwayZ * AwayZ);
		if (Length > 1.f)
		{
			AwayX /= Length;
			AwayZ /= Length;
		}
		else
		{
			AwayX = Facing;
			AwayZ = 0.f;
		}
		if (FMath::Abs(AwayX) < IceMinSide)
		{
			AwayX = (FMath::Abs(AwayX) > UE_KINDA_SMALL_NUMBER ? FMath::Sign(AwayX) : Facing) * IceMinSide;
		}
		Block->ThrowOut(FVector2D(AwayX * TornadoIceThrowSpeed, FMath::Max(AwayZ, 0.f) * TornadoIceThrowSpeed * 0.5f + TornadoIceThrowLift), Thrown);
		++IceNow;
	}
	HeldIce.Reset();

	for (const TObjectPtr<APenguinCharacter>& Each : Users)
	{
		if (Each)
		{
			Each->PlayAction(EPenguinClip::Spin, 2.f);
		}
	}
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s's tornado throws out %d penguin(s), %d shot(s), %d ice block(s) (shots sucked %d, bubbles %d, icebergs %d%s)"),
		*NamesOf(Users), ThrownNow, ShotsNow, IceNow, SuckedNow, SuckedBubbles.Num(), DrawnIcebergs.Num(),
		Merges > 0 ? *FString::Printf(TEXT("; a giant of %d tornadoes, push x%.2f, reach %.0f cm"), Merges + 1, Strength, Radius) : TEXT(""));
	// (What it threw is gone from its wind: one that merges after its throw sucks in from scratch.)
	SuckedShots.Reset();
	SuckedBubbles.Reset();
	DrawnIcebergs.Reset();
}

// ---------------------------------------------------------------------------- the look (every machine)

void APinguTornado::BuildLook()
{
	using namespace PinguTornadoPrivate;
	if (!bLookBuilt)
	{
		// (Merges before this machine saw it - a LAN client meeting a giant - get no flash.)
		ShownGrown = FMath::Max(Grown, 1.f);
		MergesShown = Merges;
	}
	// Its own shape (2026-10-29, "is the nado a fixed animation always the same ? if yes make it so it has a random tilt"): rolled
	// from its replicated seed (the same on every machine), again for each merge (the giant pops up with a fresh look).
	{
		FRandomStream Roll(LookSeed ^ (static_cast<int32>(Merges) * 7919));
		for (float& Phase : ShapePhase)
		{
			Phase = Roll.FRandRange(0.f, 2.f * UE_PI);
		}
		LeanBase = (Roll.FRand() < 0.5f ? -1.f : 1.f) * Roll.FRandRange(LeanMin, 1.f);
		LeanWander = Roll.FRandRange(0.25f, 0.55f);
		LeanWanderRate[0] = Roll.FRandRange(0.6f, 1.4f);
		LeanWanderRate[1] = Roll.FRandRange(1.5f, 2.6f);
		LeanWanderPhase[0] = Roll.FRandRange(0.f, 2.f * UE_PI);
		LeanWanderPhase[1] = Roll.FRandRange(0.f, 2.f * UE_PI);
		CurveStrength = Roll.FRandRange(0.45f, 1.05f);
		CurveWaves = Roll.FRandRange(1.1f, 1.9f);
		CurveSpeed = (Roll.FRand() < 0.75f ? 1.f : -1.f) * Roll.FRandRange(1.6f, 2.8f);
		BulgeSize = Roll.FRandRange(0.13f, 0.24f);
		BulgeWaves = Roll.FRandRange(1.3f, 2.2f);
		BulgeSpeed = Roll.FRandRange(0.35f, 0.7f);
		SpinScale = Roll.FRandRange(0.85f, 1.2f);
		TurnScale = Roll.FRandRange(0.85f, 1.15f);
		DepthSway = Roll.FRandRange(0.2f, 0.5f);
	}
	bLookBuilt = true;
	const bool bLow = UPinguSettings::GetFXDensity() < 0.5f;
	const APinguFX* Effects = APinguFX::Get(this);
	const bool bUltra = Effects && Effects->UsesUltraEffects();
	// Ultra: the 3D swirl strands instead of the bands.
	BandLayers = bUltra ? 0 : bLow ? BandLayersLow : BandLayersFull;
	BandSegments = bLow ? BandSegmentsLow : BandSegmentsFull;
	NumBands = BandLayers * BandSegments;
	StrandLayers = bUltra ? StrandLayersUltra : 0;
	NumStrands = 0;
	for (int32 Layer = 0; Layer < StrandLayers; ++Layer)
	{
		NumStrands += StrandsPerLayer[Layer] * StrandSegments;
	}
	const int32 WantRings = bLow ? GlassRingsLow : GlassRingsFull;
	const int32 WantSides = bLow ? GlassSidesLow : GlassSidesFull;
	if (WantRings != GlassRings || WantSides != GlassSides)
	{
		GlassRings = WantRings;
		GlassSides = WantSides;
		bGlassMade = false;
	}
	// A giant swirls with the snow and bits of all the tornadoes it took in.
	const int32 Pieces = 1 + static_cast<int32>(Merges);
	NumDust = (bLow ? DustLow : DustFull) * FMath::Min(Pieces, MaxDustTimes);
	NumDebris = (bLow ? DebrisLow : bUltra ? DebrisUltra : DebrisFull) * FMath::Min(Pieces, MaxDebrisTimes);
	NumSwirl = bLow ? SwirlLow : SwirlFull;
	RingSegments = bLow ? RingSegmentsLow : RingSegmentsFull;

	UMaterialInterface* FXMaterial = LoadObject<UMaterialInterface>(nullptr, FXMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
	UMaterialInterface* TrailMaterial = LoadObject<UMaterialInterface>(nullptr, TrailMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
	const auto LoadMesh = [](const TCHAR* Path, const TCHAR* Fallback)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
		return Mesh ? Mesh : LoadObject<UStaticMesh>(nullptr, Fallback, nullptr, LOAD_Quiet | LOAD_NoWarn);
	};
	const FTransform Hidden(FRotator::ZeroRotator, FVector::ZeroVector, FVector(0.001f));
	struct FPart
	{
		UInstancedStaticMeshComponent* Instances;
		UStaticMesh* Mesh;
		UMaterialInterface* Material;
		int32 Count;
	};
	const FPart Parts[] = {
		{ Bands, LoadMesh(GlowMeshPath, GlowMeshPath), TrailMaterial ? TrailMaterial : FXMaterial, NumBands },
		{ Dust, LoadMesh(PuffMeshPath, SphereMeshPath), FXMaterial, NumDust },
		{ Debris, LoadMesh(FlakeMeshPath, SphereMeshPath), FXMaterial, NumDebris },
		{ Swirl, LoadMesh(FlakeMeshPath, SphereMeshPath), FXMaterial, NumSwirl },
		{ Rings, LoadMesh(GlowMeshPath, GlowMeshPath), TrailMaterial ? TrailMaterial : FXMaterial, MergeRingCount * RingSegments },
		{ Strands, LoadMesh(GlowMeshPath, GlowMeshPath), TrailMaterial ? TrailMaterial : FXMaterial, NumStrands },
	};
	// The glass funnel's material (its mesh is made on the first frame it shows).
	if (Funnel && !FunnelMaterial)
	{
		UMaterialInterface* Glass = LoadObject<UMaterialInterface>(nullptr, GlassMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
		bSoftGlass = Glass != nullptr;
		if (UMaterialInterface* Ghost = Glass ? Glass : LoadObject<UMaterialInterface>(nullptr, GhostMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn))
		{
			FunnelMaterial = UMaterialInstanceDynamic::Create(Ghost, this);
			FunnelMaterial->SetVectorParameterValue(TEXT("Color"), FunnelColor);
			FunnelMaterial->SetScalarParameterValue(TEXT("Opacity"), 0.f);
			Funnel->SetMaterial(0, FunnelMaterial);
		}
	}
	for (const FPart& Part : Parts)
	{
		if (!Part.Instances)
		{
			continue;
		}
		Part.Instances->SetStaticMesh(Part.Mesh);
		if (Part.Material)
		{
			Part.Instances->SetMaterial(0, Part.Material);
		}
		Part.Instances->SetNumCustomDataFloats(NumCustomData);
		Part.Instances->ClearInstances();
		TArray<FTransform> Start;
		Start.Init(Hidden, Part.Count);
		Part.Instances->AddInstances(Start, false, false, false);
	}
}

void APinguTornado::ShowMergeFlash(const FVector& Eye)
{
	using namespace PinguTornadoPrivate;
	// Two have just become one: a deep double whoosh, wind bursting out from the eye, snow and glitter flung round; the giant's
	// own funnel and bands flare bright for a moment (TickLook: MergeFlashAt).
	const float S = FMath::Max(SizeScale, 0.2f);
	const float Wide = FMath::Max(Grown, 1.f);
	const double Now = ServerNow(GetWorld());
	MergeFlashAt = Now;
	// The merge sound (started with the drift) has its thump now; without it (a drift seen too late) the old double whoosh.
	if (DriftShownFor >= 0.0 && Now - DriftShownFor < 1.0 && MergeSounds > 0)
	{
		APinguFX::Sound(this, EPinguSound::Gust, 0.55f, 0.04f, 0.6f);
	}
	else
	{
		APinguFX::Sound(this, EPinguSound::Gust, 1.f, 0.04f, 0.55f);
		APinguFX::Sound(this, EPinguSound::StompWhoosh, 1.f, 0.04f, 0.6f);
	}
	const float Big = FMath::Sqrt(Wide);
	const FVector FootAt = Eye + FVector(0.f, 0.f, FunnelBottom * S);
	APinguFX::Puffs(this, Eye, 6, 22.f * S, 900.f, FVector::UpVector, 1.f, MergeColor);
	// Snow sprayed out both ways along the ground from its foot.
	APinguFX::Puffs(this, FootAt, 7, 26.f * S * Big, 1300.f * Big, FVector(1.f, 0.f, 0.2f).GetSafeNormal(), 0.35f, MergeColor);
	APinguFX::Puffs(this, FootAt, 7, 26.f * S * Big, 1300.f * Big, FVector(-1.f, 0.f, 0.2f).GetSafeNormal(), 0.35f, MergeColor);
	APinguFX::Flakes(this, Eye + FVector(0.f, 0.f, 80.f * S), 28, 1400.f * Big, FLinearColor(0.85f, 0.95f, 1.f));
	APinguFX::Sparkles(this, Eye + FVector(0.f, 0.f, 120.f * S), 18, 160.f * S * Wide, FLinearColor(0.8f, 0.97f, 1.f));
	APinguFX::Shake(this, 0.3f * FMath::Min(Big, 1.3f));
	if (APinguFX* FX = APinguFX::Get(this))
	{
		// A pinwheel of wind: streaks flung out on a spiral (turned the way the funnel spins), from all up its body.
		const int32 Count = FMath::Max(12, FMath::RoundToInt(40.f * UPinguSettings::GetFXDensity()));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float Angle = 2.f * UE_PI * (Index + LookRand.FRand() * 0.6f) / Count;
			const FVector Out(FMath::Cos(Angle), 0.f, FMath::Sin(Angle));
			const FVector Round(-Out.Z, 0.f, Out.X);
			const FVector Way = (Out + Round * 0.75f).GetSafeNormal();
			const FVector From = Eye + Out * 90.f * S * Big + FVector(0.f, 0.f, LookRand.FRandRange(0.f, 160.f) * S * Big);
			SpawnStreak(FX, From, Way * LookRand.FRandRange(1600.f, 2400.f) * Big, LookRand.FRandRange(0.3f, 0.42f), 0.65f);
		}
	}
}

void APinguTornado::TickMergeLook(const FVector& Eye, double Now, float S, float Wide, float Tall, float Foot)
{
	using namespace PinguTornadoPrivate;
	float Data[NumCustomData];
	const float Big = FMath::Sqrt(Wide);
	const FTransform Hidden(FRotator::ZeroRotator, FVector::ZeroVector, FVector(0.001f));
	const bool bDrifting = MoveTime > 0.f && Now < MoveStart + MoveTime;
	const float DriftU = bDrifting ? FMath::Clamp(static_cast<float>((Now - MoveStart) / MoveTime), 0.f, 1.f) : 0.f;
	const float SinceMerge = MergeFlashAt >= 0.0 ? static_cast<float>(Now - MergeFlashAt) : -1.f;

	// The swirl (the lead's): snow spiralling in round where the two meet, closing in and speeding up over the drift; when
	// they are one it is flung out on a spiral and fades.
	const bool bSwirlIn = bDrifting && bMergeLead && SwirlStartRadius > 0.f;
	const bool bSwirlOut = !bSwirlIn && SinceMerge >= 0.f && SinceMerge < MergeSwirlFling && SwirlStartRadius > 0.f;
	const bool bShowSwirl = bSwirlIn || bSwirlOut;
	if (NumSwirl > 0 && (bShowSwirl || SwirlShown > 0))
	{
		TransformScratch.SetNum(NumSwirl, EAllowShrinking::No);
		// (Local to the eye: during the drift the lead's eye is not yet where they meet.)
		const FVector Meet = FVector(Center.X, 0.f, Center.Z) - Eye;
		const float Fling = bSwirlOut ? SinceMerge / MergeSwirlFling : 0.f;
		for (int32 Index = 0; Index < NumSwirl; ++Index)
		{
			if (!bShowSwirl)
			{
				TransformScratch[Index] = Hidden;
				continue;
			}
			const float U = (Index + 0.5f) / NumSwirl;
			const float Level = FMath::Frac(U * 2.7f + 0.1f);       // how high up the column it goes round
			const float Phase = 2.f * UE_PI * FMath::Frac(U * 7.3f);
			const float Wider = 0.7f + 0.6f * Level;                // wider higher up, like the funnel
			const float Turned = MergeSwirlTurns * (0.4f * DriftU + 0.6f * DriftU * DriftU); // it speeds up
			float Across;
			float Angle;
			float Size = (24.f + 14.f * FMath::Frac(U * 5.1f)) * S * Big;
			float Opacity;
			if (bSwirlIn)
			{
				Across = SwirlStartRadius * Wider * (1.f - (1.f - MergeSwirlClose) * Smooth01(DriftU));
				Angle = Phase + 2.f * UE_PI * Turned;
				Size *= Smooth01(DriftU / 0.25f);
				Opacity = Smooth01(DriftU / 0.2f);
			}
			else
			{
				const float Out = 1.f - FMath::Exp(-SinceMerge / 0.12f);
				Across = SwirlStartRadius * Wider * MergeSwirlClose + FunnelTopRadius * Wide * S * 2.6f * Out;
				Angle = Phase + 2.f * UE_PI * (MergeSwirlTurns + 0.6f * (1.f - FMath::Exp(-SinceMerge / 0.15f)));
				Size *= 1.f - Fling;
				Opacity = 1.f - Fling * Fling;
			}
			const float H = (FunnelBottom + 40.f + FunnelTop * Tall * 0.85f * Level) * S;
			const FVector Local = Meet + FVector(Across * FMath::Cos(Angle), Across * FMath::Sin(Angle) * FunnelDepth, H - Across * 0.3f * FMath::Sin(Angle));
			TransformScratch[Index] = FTransform(FRotator(Angle * 90.f, Index * 37.f, Angle * 60.f), Local, FVector(FMath::Max(Size, 0.01f) / 100.f));
			Data[0] = DebrisColor.R;
			Data[1] = DebrisColor.G;
			Data[2] = DebrisColor.B;
			Data[3] = 1.6f;
			Data[4] = Opacity;
			Swirl->SetCustomData(Index, MakeArrayView(Data, NumCustomData), false);
		}
		Swirl->BatchUpdateInstancesTransforms(0, TransformScratch, false, false, true);
		APinguFX::MarkInstancesChanged(Swirl);
		SwirlShown = bShowSwirl ? NumSwirl : 0;
	}

	// The shock rings: two flat rings of wind going out from the giant as it pops up (at its foot, and half way up a moment
	// later), seen a little from above, fading as they go.
	const int32 NumRingPieces = MergeRingCount * RingSegments;
	const bool bShowRings = SinceMerge >= 0.f && SinceMerge < MergeRingTime + 0.06f;
	if (NumRingPieces > 0 && (bShowRings || RingsShown > 0))
	{
		TransformScratch.SetNum(NumRingPieces, EAllowShrinking::No);
		int32 Shown = 0;
		for (int32 Which = 0; Which < MergeRingCount; ++Which)
		{
			const float T = SinceMerge - 0.05f * Which;
			const float F = T / MergeRingTime;
			const bool bOn = bShowRings && F >= 0.f && F < 1.f;
			const float Ease = 1.f - FMath::Pow(1.f - FMath::Clamp(F, 0.f, 1.f), 3.f);
			const float Across = FMath::Lerp(Which == 0 ? 70.f * Foot : 110.f * Wide, Which == 0 ? 560.f : 760.f, Ease) * S * Big;
			const float H = Which == 0 ? (FunnelBottom + 12.f) * S : (FunnelBottom + (FunnelTop * Tall - FunnelBottom) * 0.45f) * S;
			const float Width = (Which == 0 ? 34.f : 24.f) * S * Big * FMath::Max(1.f - F, 0.f);
			const auto Point = [Across, H](float Angle)
			{
				return FVector(Across * FMath::Cos(Angle), Across * FMath::Sin(Angle) * MergeRingDepth, H - Across * MergeRingTilt * FMath::Sin(Angle));
			};
			for (int32 Piece = 0; Piece < RingSegments; ++Piece)
			{
				const int32 Slot = Which * RingSegments + Piece;
				if (!bOn)
				{
					TransformScratch[Slot] = Hidden;
					continue;
				}
				++Shown;
				const float A0 = 2.f * UE_PI * Piece / RingSegments;
				const float A1 = 2.f * UE_PI * (Piece + 1) / RingSegments;
				const FVector P0 = Point(A0);
				const FVector P1 = Point(A1);
				const FVector Segment = P1 - P0;
				const float Length = FMath::Max(static_cast<float>(Segment.Size()), 1.f);
				const FQuat Turn = FQuat::FindBetweenNormals(FVector::UpVector, Segment / Length);
				TransformScratch[Slot] = FTransform(Turn, (P0 + P1) * 0.5f, FVector(FMath::Max(Width, 0.01f) / 100.f, FMath::Max(Width, 0.01f) / 100.f, Length * 1.08f / 100.f));
				const float Front = FMath::Sin((A0 + A1) * 0.5f);
				Data[0] = BandColor.R;
				Data[1] = BandColor.G;
				Data[2] = BandColor.B;
				Data[3] = 2.2f;
				Data[4] = FMath::Min(0.85f * FMath::Pow(1.f - F, 1.5f) * (0.55f + 0.45f * Front), 1.f);
				Rings->SetCustomData(Slot, MakeArrayView(Data, NumCustomData), false);
			}
		}
		Rings->BatchUpdateInstancesTransforms(0, TransformScratch, false, false, true);
		APinguFX::MarkInstancesChanged(Rings);
		RingsShown = Shown;
	}
}

void APinguTornado::TickLook(float Dt)
{
	using namespace PinguTornadoPrivate;
	if (StartTime <= 0.0)
	{
		return; // (a LAN client before its numbers arrived)
	}
	if (!bLookBuilt)
	{
		BuildLook();
	}
	const double Now = ServerNow(GetWorld());
	const FVector Eye = GetCenter();
	if (!GetActorLocation().Equals(Eye, 0.1))
	{
		SetActorLocation(Eye); // (it drifts while two merge)
	}
	const float Age = GetAge();
	LookAge = Age;
	const float S = FMath::Max(SizeScale, 0.2f);
	APinguFX* FX = APinguFX::Get(this);
	const float Density = UPinguSettings::GetFXDensity();

	// Two have just become one: the flash, and more pieces swirling; the funnel widens to its new size.
	if (Merges > MergesShown)
	{
		MergesShown = Merges;
		++MergeFlashes;
		ShowMergeFlash(Eye);
		BuildLook();
	}
	ShownGrown = FMath::FInterpTo(ShownGrown, FMath::Max(Grown, 1.f), Dt, GrowSpeed);
	const float Wide = ShownGrown;
	const float Foot = 1.f + GrownFootShare * (Wide - 1.f);
	const float Big = FMath::Sqrt(Wide); // (the pieces' sizes)

	// The merge (cosmetic, from the replicated drift and merges): while two drift together each funnel bends toward where
	// they meet and spins up; the lead starts the merge sound (its thump lands as they become one). The giant pops up:
	// squashed wide at once, stretched tall a moment later, settling (PopHeight / PopWidth).
	const bool bDrifting = MoveTime > 0.f && Now < MoveStart + MoveTime;
	const float DriftU = bDrifting ? FMath::Clamp(static_cast<float>((Now - MoveStart) / MoveTime), 0.f, 1.f) : 0.f;
	FVector2D Lean = FVector2D::ZeroVector;
	if (bDrifting)
	{
		const FVector Toward = FVector(Center.X, 0.f, Center.Z) - Eye;
		Lean = FVector2D(Toward.X, Toward.Z) * MergeLeanShare * Smooth01(DriftU / MergeLeanBuild);
		const float MaxLean = MergeLeanMaxTops * FunnelTopRadius * Wide * S;
		const float LeanSize = static_cast<float>(Lean.Size());
		Lean *= LeanSize > MaxLean ? MaxLean / LeanSize : 1.f;
		if (DriftShownFor != MoveStart)
		{
			DriftShownFor = MoveStart;
			// The swirl's pieces start out round both eyes (the lead's own way to where they meet, half again).
			SwirlStartRadius = FMath::Clamp(DistXZ(FVector(CenterFrom), FVector(Center)) * 1.5f, 200.f * S, 1000.f * S);
			if (bMergeLead && DriftU < MergeSoundLatest)
			{
				++MergeSounds;
				APinguFX::Sound(this, EPinguSound::TornadoMerge, 1.f, 0.f, 1.f);
			}
		}
	}
	ShownLean = static_cast<float>(Lean.Size());
	const float PopT = MergeFlashAt >= 0.0 ? static_cast<float>(Now - MergeFlashAt) : -1.f;
	float PopHeight = 1.f;
	float PopWidth = 1.f;
	if (PopT >= 0.f && PopT < MergePopTime)
	{
		const float P = -FMath::Cos(2.f * UE_PI * PopT / MergePopPeriod) * FMath::Exp(-PopT / MergePopDecay);
		PopHeight = 1.f + MergePopHeight * P;
		PopWidth = 1.f - MergePopWidth * P;
	}
	ShownPop = PopHeight;
	const float Tall = (1.f + GrownHeightShare * (Wide - 1.f)) * PopHeight;
	const float WideNow = Wide * PopWidth; // (the funnel's own width: popping)

	// The start: a whoosh as the wind gathers; the throw: the burst at the edge of its reach, a heavier whoosh.
	if (!bStartShown)
	{
		bStartShown = true;
		APinguFX::Sound(this, EPinguSound::Gust, 1.f, 0.05f, 0.7f);
		APinguFX::Sound(this, EPinguSound::StompWhoosh, 0.55f, 0.05f, 0.8f);
	}
	const bool bSucking = Now < ThrowTime;
	if (!bSucking && BurstShownFor != ThrowTime)
	{
		BurstShownFor = ThrowTime;
		APinguFX::Sound(this, EPinguSound::Gust, 1.f, 0.05f, Merges > 0 ? 0.85f : 1.1f);
		APinguFX::Sound(this, EPinguSound::StompWhoosh, 0.8f, 0.05f, Merges > 0 ? 0.9f : 1.2f);
		APinguFX::Puffs(this, Eye, FMath::RoundToInt(8.f * Big), 28.f * S, 1400.f * Big, FVector::UpVector, 1.f, FLinearColor(0.9f, 0.96f, 1.f));
		APinguFX::Flakes(this, Eye, FMath::RoundToInt(12.f * Wide), 900.f * Big, FLinearColor(0.85f, 0.95f, 1.f));
		APinguFX::Shake(this, 0.25f * FMath::Min(Big, 1.6f));
		if (FX)
		{
			// Wind streaks flung out all round.
			const int32 Count = FMath::Max(8, FMath::RoundToInt(28.f * Density * Big));
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const float Angle = 2.f * UE_PI * (Index + LookRand.FRand() * 0.6f) / Count;
				const FVector Out(FMath::Cos(Angle), 0.f, FMath::Sin(Angle));
				SpawnStreak(FX, Eye + Out * 120.f * S * Foot, Out * LookRand.FRandRange(1700.f, 2400.f) * Big, LookRand.FRandRange(0.3f, 0.45f), 0.5f);
			}
		}
	}

	// How much of it there is: it forms fast, sucks at full strength, then winds down after the throw (spins out, rises, fades).
	const float Grow = Smooth01(Age / GrowTime);
	const float After = FMath::Max(static_cast<float>(Now - ThrowTime), 0.f);
	const float WindDown = 1.f - Smooth01(After / FMath::Max(static_cast<float>(EndTime + LingerTime - ThrowTime), 0.05f));
	const float Amount = Grow * WindDown;
	LookAmount = Amount;
	// A merge flares the giant's funnel and bands bright for a moment (fading out over MergeFlashTime).
	const float Glare = MergeFlashAt >= 0.0 ? 1.f - Smooth01(static_cast<float>(Now - MergeFlashAt) / MergeFlashTime) : 0.f;
	const float SpinUp = (1.f + MergeSpinUp * DriftU) * (PopT >= 0.f ? 1.f + 0.8f * FMath::Exp(-PopT / 0.2f) : 1.f);
	const float Spin = SpinRate * SpinScale * (0.35f + 0.65f * WindDown) * (bSucking ? 1.f : 1.25f) * SpinUp / Big;
	SpinAngle += Spin * Dt;
	const float Flare = 1.f + 0.6f * Smooth01(After / 0.3f); // the throw flings the funnel's top out
	const float Rise = 60.f * Tall * Smooth01(After / 0.5f);

	// The inflow while it sucks: wind streaks and snowflakes swept in from the edge of its reach (it shows how far it pulls).
	if (FX && bSucking)
	{
		const FVector Meet(Center.X, 0.f, Center.Z);
		PuffTimer -= Dt;
		const float Every = InflowEvery / FMath::Max(Density, 0.15f) / Big;
		while (PuffTimer <= 0.f)
		{
			PuffTimer += Every;
			if (bDrifting && LookRand.FRand() < 0.6f)
			{
				// Two drifting together: wind and snow torn from this funnel toward where they meet (they suck each other in),
				// curling the way the swirl there turns.
				const FVector Toward = (Meet - Eye).GetSafeNormal();
				const FVector Way = Toward.IsNearlyZero() ? FVector::UpVector : Toward;
				const FVector Curl(-Way.Z, 0.f, Way.X);
				const FVector From = Eye + FVector(Way.X * LookRand.FRandRange(0.f, 160.f), LookRand.FRandRange(-30.f, 30.f), LookRand.FRandRange(-40.f, 240.f) * Tall) * S;
				const FVector Velocity = Way * LookRand.FRandRange(1500.f, 2200.f) + Curl * LookRand.FRandRange(300.f, 700.f) + FVector(0.f, 0.f, 120.f);
				if (LookRand.FRand() < 0.35f)
				{
					FPinguParticleDesc D;
					D.Mesh = EPinguFXMesh::Flake;
					D.Location = From;
					D.Velocity = Velocity * 0.8f;
					D.Size = LookRand.FRandRange(10.f, 16.f) * S * Big;
					D.EndSize = D.Size * 0.4f;
					D.Curve = EPinguSizeCurve::Linear;
					D.Life = LookRand.FRandRange(0.22f, 0.32f);
					D.Color = FLinearColor(0.9f, 0.96f, 1.f);
					D.bFadeOut = true;
					D.Spin = FVector(0.f, 0.f, 720.f);
					FX->Spawn(D);
				}
				else
				{
					SpawnStreak(FX, From, Velocity, LookRand.FRandRange(0.22f, 0.32f), 0.55f);
				}
				continue;
			}
			const float Angle = LookRand.FRandRange(-0.25f * UE_PI, 1.25f * UE_PI); // mostly above the eye (the ground hides the rest)
			const float From = Radius * LookRand.FRandRange(0.7f, 1.f);
			const FVector Start = Eye + FVector(FMath::Cos(Angle), 0.f, FMath::Sin(Angle)) * From + FVector(0.f, LookRand.FRandRange(-40.f, 40.f), 0.f);
			const float Life = LookRand.FRandRange(0.3f, 0.42f);
			const FVector In = (Eye - Start) / Life * 0.75f;
			const FVector Round = FVector(-FMath::Sin(Angle), 0.f, FMath::Cos(Angle)) * 300.f * Big;
			if (LookRand.FRand() < 0.3f)
			{
				FPinguParticleDesc D;
				D.Mesh = EPinguFXMesh::Flake;
				D.Location = Start;
				D.Velocity = FVector(In.X + Round.X, 0.f, In.Z + Round.Z);
				D.Size = LookRand.FRandRange(10.f, 16.f) * Big;
				D.EndSize = D.Size * 0.5f;
				D.Curve = EPinguSizeCurve::Linear;
				D.Life = Life;
				D.Color = FLinearColor(0.88f, 0.95f, 1.f);
				D.bFadeOut = true;
				D.Spin = FVector(0.f, 0.f, 540.f);
				FX->Spawn(D);
			}
			else
			{
				SpawnStreak(FX, Start, FVector(In.X + Round.X, 0.f, In.Z + Round.Z), Life, 0.42f);
			}
		}
	}

	// The shape (2026-10-27, "make the cone of the nado not so perfectly like a cone"): a living twister, not a cone. Its axis
	// sways and bends along its height (slow waves, more at the top; a merge's lean on top of that), its radius is uneven round
	// it and up it, its top is ragged, and all of it changes slowly over time - the same on every machine (the waves' phases come
	// from its start time, their clock is its age). The bands, the glass, the Ultra strands and the bits all follow it.
	const FVector2D LeanNow = Lean;
	const float SwayAmp = SwayAmplitude * S * Big;
	const auto Axis = [this, Age, SwayAmp, LeanNow, S, Big](float U)
	{
		const float Bend = FMath::Pow(U, 1.6f);
		// An S: one wave and a half up its height, travelling up, plus a slow lean of the whole funnel to one side.
		// Its own tilt, wandering smoothly (two slow waves of its own), and its own S curve travelling up it.
		const float Tilt = LeanTilt * (LeanBase + LeanWander * (0.6f * FMath::Sin(LeanWanderRate[0] * Age + LeanWanderPhase[0]) + 0.4f * FMath::Sin(LeanWanderRate[1] * Age + LeanWanderPhase[1])));
		const float X = SwayAmp * (CurveStrength * FMath::Sin(2.f * UE_PI * CurveWaves * U - CurveSpeed * Age + ShapePhase[0]) * FMath::Sqrt(U) + Tilt * Bend)
			+ 8.f * S * Big * FMath::Sin(5.f * Age + 6.f * U + ShapePhase[2]) * U + static_cast<float>(LeanNow.X) * Bend;
		const float Y = SwayAmp * DepthSway * FMath::Sin(1.7f * Age + 2.7f * U + ShapePhase[3]) * U;
		return FVector(X, Y, static_cast<float>(LeanNow.Y) * Bend);
	};
	const auto Rag = [](float U) { return Smooth01((U - 0.72f) / 0.28f); };
	const auto Uneven = [this, Age, Rag](float U, float Angle)
	{
		// Bulges that drift up it (about 20 % of its radius), lobes round it, the ragged top.
		return 1.f + BulgeSize * FMath::Sin(2.f * UE_PI * (BulgeWaves * U - BulgeSpeed * Age) + ShapePhase[4]) + 0.08f * FMath::Sin(3.f * Angle + 0.8f * Age + 3.5f * U + ShapePhase[5])
			+ Rag(U) * (0.1f * FMath::Sin(5.f * Angle + 2.3f * Age + ShapePhase[6]) + 0.05f * FMath::Sin(8.f * Angle - 3.f * Age + ShapePhase[7]));
	};
	const float FormScale = S * (0.35f + 0.65f * Grow);
	ShownTopShift = static_cast<float>(Axis(1.f).X);
	// Its radius up its height - a trumpet: a narrow neck at the ground flaring into its wide top (concave sides, not a V) - and
	// the height of a point U of the way up.
	const auto Profile = [Foot, WideNow, Flare, FormScale](float U)
	{
		return FMath::Lerp(FunnelBottomRadius * Foot, FunnelTopRadius * WideNow * Flare, FMath::Pow(U, 2.1f)) * FormScale;
	};
	const auto HeightAt = [Tall, Rise, S](float U)
	{
		return (FMath::Lerp(FunnelBottom, FunnelTop * Tall, U) + Rise * U) * S;
	};
	const float FullHeight = (FunnelTop * Tall - FunnelBottom + Rise) * S;
	const auto RimLift = [this, Age, Rag, FullHeight](float U, float Angle)
	{
		return Rag(U) * 0.05f * FullHeight * FMath::Sin(4.f * Angle + 1.1f * Age + ShapePhase[7]);
	};

	// The funnel's wind bands (not on Ultra: its strands instead): arcs of glowing ribbon going round the twisting axis, each
	// layer a little different in reach and height; the front of every band (toward the camera, +Y) bright, the back faint.
	float Data[NumCustomData];
	TransformScratch.SetNum(NumBands, EAllowShrinking::No);
	for (int32 Layer = 0; Layer < BandLayers; ++Layer)
	{
		const float U = BandLayers > 1 ? FMath::Clamp(Layer / static_cast<float>(BandLayers - 1) + 0.03f * FMath::Sin(Age * 2.f + Layer * 1.7f), 0.f, 1.f) : 0.f;
		const float H = HeightAt(U);
		const float Ring = Profile(U);
		const FVector Mid = Axis(U);
		const float Start = SpinAngle * (1.3f - 0.4f * U) + Layer * 1.9f;
		const float Arc = BandArc * (0.85f + 0.3f * FMath::Frac(Layer * 0.37f + 0.2f));
		const auto Point = [&Uneven, &RimLift, Ring, H, U, Mid](float Angle)
		{
			const float R = Ring * Uneven(U, Angle);
			return Mid + FVector(R * FMath::Cos(Angle), R * FMath::Sin(Angle) * BandDepth, H + RimLift(U, Angle) - R * BandTilt * FMath::Sin(Angle));
		};
		for (int32 Piece = 0; Piece < BandSegments; ++Piece)
		{
			const int32 Slot = Layer * BandSegments + Piece;
			const float A0 = Start + Arc * Piece / BandSegments;
			const float A1 = Start + Arc * (Piece + 1) / BandSegments;
			const FVector P0 = Point(A0);
			const FVector P1 = Point(A1);
			const FVector Segment = P1 - P0;
			const float Length = FMath::Max(static_cast<float>(Segment.Size()), 1.f);
			const FQuat Turn = FQuat::FindBetweenNormals(FVector::UpVector, Segment / Length);
			const float Head = (Piece + 1.f) / BandSegments; // the band thickens toward its leading end
			const float Width = (12.f + 0.22f * Ring / S) * S * (0.5f + 0.5f * Head) * Amount;
			TransformScratch[Slot] = FTransform(Turn, (P0 + P1) * 0.5f, FVector(FMath::Max(Width, 0.01f) / 100.f, FMath::Max(Width, 0.01f) / 100.f, Length * 1.06f / 100.f));
			const float Front = FMath::Sin((A0 + A1) * 0.5f);
			Data[0] = BandColor.R;
			Data[1] = BandColor.G;
			Data[2] = BandColor.B;
			Data[3] = 0.9f + 2.5f * Glare;
			Data[4] = FMath::Min((0.6f + 0.35f * Front) * (0.6f + 0.4f * Head) * Amount * (1.f + 0.6f * Glare), 1.f);
			Bands->SetCustomData(Slot, MakeArrayView(Data, NumCustomData), false);
		}
	}
	if (NumBands > 0)
	{
		Bands->BatchUpdateInstancesTransforms(0, TransformScratch, false, false, true);
		APinguFX::MarkInstancesChanged(Bands);
	}

	// Ultra: 3D swirls - layers of thin twisting streaks spiralling up round the axis at their own speeds (the inner ones fast and
	// bluish, the outer ones slow and white), really round it in depth (the back ones behind its users, faint; the front ones
	// bright), soft at both ends, each one fading in and out on its own.
	TransformScratch.SetNum(NumStrands, EAllowShrinking::No);
	int32 Slot = 0;
	// Seen from far (the match's camera) the thin strands get wider, so it still reads as a twister (this machine's camera).
	float ViewBoost = 1.f;
	if (NumStrands > 0)
	{
		const APlayerController* Viewer = GetWorld()->GetFirstPlayerController();
		if (Viewer && Viewer->PlayerCameraManager)
		{
			ViewBoost = FMath::Clamp(static_cast<float>(FVector::Dist(Viewer->PlayerCameraManager->GetCameraLocation(), Eye)) / (StrandViewDistance * S), 1.f, StrandViewBoostMax);
		}
	}
	for (int32 Layer = 0; Layer < StrandLayers && NumStrands > 0; ++Layer)
	{
		const float LayerScale = StrandLayerScale[Layer];
		const float Turn0 = SpinAngle * StrandLayerSpin[Layer];
		const int32 Count = StrandsPerLayer[Layer];
		for (int32 Strand = 0; Strand < Count; ++Strand)
		{
			const float Seed = FMath::Frac(Strand * 0.618f + Layer * 0.33f + ShapePhase[0] * 0.1f);
			const float U0 = 0.02f + 0.12f * FMath::Frac(Seed * 3.7f);
			const float U1 = 0.72f + 0.28f * FMath::Frac(Seed * 5.3f);
			const float Turns = (1.25f + 0.3f * Layer) * (0.85f + 0.3f * FMath::Frac(Seed * 7.1f)) * TurnScale;
			const float Phase = 2.f * UE_PI * Strand / Count + Layer * 0.7f + ShapePhase[1];
			const float Live = 0.55f + 0.45f * FMath::Sin(0.9f * Age + Seed * 11.f);
			const float Thick = (2.5f + 1.f * Layer) * S * Big;
			const FLinearColor Tint = FMath::Lerp(FLinearColor(0.62f, 0.82f, 1.f), FLinearColor(0.93f, 0.97f, 1.f), Layer / FMath::Max(StrandLayers - 1.f, 1.f));
			const auto Point = [&](float Along)
			{
				const float U = FMath::Lerp(U0, U1, Along);
				const float Angle = Phase + Turn0 + 2.f * UE_PI * Turns * Along;
				const float R = Profile(U) * Uneven(U, Angle) * LayerScale;
				return Axis(U) + FVector(R * FMath::Cos(Angle), R * FMath::Sin(Angle) * StrandDepth, HeightAt(U) + RimLift(U, Angle) - R * 0.08f * FMath::Sin(Angle));
			};
			for (int32 Piece = 0; Piece < StrandSegments; ++Piece, ++Slot)
			{
				const float S0 = Piece / static_cast<float>(StrandSegments);
				const float S1 = (Piece + 1) / static_cast<float>(StrandSegments);
				const FVector P0 = Point(S0);
				const FVector P1 = Point(S1);
				const FVector Segment = P1 - P0;
				// Each piece lies across the view (its depth only places it: the back ones behind the users): a piece turned
				// toward the camera would show its round end as a blob (M_Trail is made for pieces across the view).
				const FVector Flat(Segment.X, Segment.Y * 0.08f, Segment.Z);
				const float Length = FMath::Max(static_cast<float>(Flat.Size()), 1.f);
				const FQuat Turn = FQuat::FindBetweenNormals(FVector::UpVector, Flat / Length);
				const float Ends = FMath::Pow(FMath::Sin(UE_PI * (S0 + S1) * 0.5f), 0.7f); // soft, thin ends
				// As wide as the bands were (they read from the match's distance too), wider where the funnel is.
				const float Local = Profile(FMath::Lerp(U0, U1, (S0 + S1) * 0.5f));
				const float Width = (Thick + 0.07f * Local) * (0.35f + 0.65f * Ends) * (0.5f + 0.5f * Live) * Amount * ViewBoost;
				TransformScratch[Slot] = FTransform(Turn, (P0 + P1) * 0.5f, FVector(FMath::Max(Width, 0.01f) / 100.f, FMath::Max(Width, 0.01f) / 100.f, Length * 1.02f / 100.f));
				const float Front = (P0.Y + P1.Y) * 0.5f / FMath::Max(Local, 1.f); // -1 back .. 1 front
				// (Where it turns round the side, heading toward the camera or away, it is a little fainter.)
				const float Sideways = 0.6f + 0.4f * FMath::Clamp(Length / FMath::Max(static_cast<float>(Segment.Size()), 1.f), 0.f, 1.f);
				Data[0] = Tint.R;
				Data[1] = Tint.G;
				Data[2] = Tint.B;
				Data[3] = 0.9f + 0.3f * Layer + 2.f * Glare;
				// The back ones faint (behind its users), the front ones bright - but thin over its users at the bottom (they stay
				// readable inside).
				const float Facing = FMath::Clamp(0.5f + 0.5f * Front, 0.f, 1.f);
				const float Low = FMath::Lerp(0.35f, 1.f, Smooth01((FMath::Lerp(U0, U1, (S0 + S1) * 0.5f) - 0.05f) / 0.4f));
				Data[4] = FMath::Min((0.1f + 0.75f * Facing * Facing * FMath::Lerp(Low, 1.f, 1.f - Facing)) * Ends * Live * Sideways * Amount * (1.f + 0.6f * Glare), 1.f);
				Strands->SetCustomData(Slot, MakeArrayView(Data, NumCustomData), false);
			}
		}
	}
	if (NumStrands > 0)
	{
		Strands->BatchUpdateInstancesTransforms(0, TransformScratch, false, false, true);
		APinguFX::MarkInstancesChanged(Strands);
	}

	// The glassy funnel inside: a mesh of rings round the twisting axis (uneven, ragged at its top), rebuilt every frame.
	const float Top = FunnelTopRadius * WideNow * Flare * S * (0.35f + 0.65f * Grow);
	const float Height = FullHeight;
	ShownTopRadius = Top;
	ShownHeight = Height;
	if (Funnel && FunnelMaterial && GlassRings > 1 && GlassSides > 2)
	{
		const bool bShow = Amount > 0.02f;
		Funnel->SetVisibility(bShow);
		if (bShow)
		{
			const int32 Across = GlassSides + 1;
			GlassVertices.SetNum(GlassRings * Across, EAllowShrinking::No);
			GlassNormals.SetNum(GlassRings * Across, EAllowShrinking::No);
			GlassColors.SetNum(GlassRings * Across, EAllowShrinking::No);
			for (int32 Ring = 0; Ring < GlassRings; ++Ring)
			{
				const float U = Ring / static_cast<float>(GlassRings - 1);
				const FVector Mid = Axis(U);
				const float H = HeightAt(U);
				const float R = Profile(U);
				const float Slope = (Profile(FMath::Min(U + 0.05f, 1.f)) - Profile(FMath::Max(U - 0.05f, 0.f))) / FMath::Max(HeightAt(FMath::Min(U + 0.05f, 1.f)) - HeightAt(FMath::Max(U - 0.05f, 0.f)), 1.f);
				for (int32 Side = 0; Side < Across; ++Side)
				{
					const float Angle = 2.f * UE_PI * Side / GlassSides;
					const float Here = R * Uneven(U, Angle);
					const FVector Out(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);
					GlassVertices[Ring * Across + Side] = Mid + FVector(Here * Out.X, Here * Out.Y, H + RimLift(U, Angle));
					GlassNormals[Ring * Across + Side] = FVector(Out.X, Out.Y, -Slope).GetSafeNormal();
					// Fading out at its foot and its ragged top, and thin round its users.
					const float Fade = Smooth01(U / 0.12f) * (1.f - Smooth01((U - 0.78f) / 0.22f)) * FMath::Lerp(0.45f, 1.f, Smooth01((U - 0.1f) / 0.35f));
					GlassColors[Ring * Across + Side] = FColor(255, 255, 255, static_cast<uint8>(FMath::Clamp(Fade, 0.f, 1.f) * 255.f));
				}
			}
			if (!bGlassMade)
			{
				bGlassMade = true;
				TArray<int32> Triangles;
				TArray<FVector2D> UVs;
				for (int32 Ring = 0; Ring < GlassRings; ++Ring)
				{
					for (int32 Side = 0; Side < Across; ++Side)
					{
						UVs.Add(FVector2D(Side / static_cast<float>(GlassSides), Ring / static_cast<float>(GlassRings - 1)));
					}
				}
				for (int32 Ring = 0; Ring + 1 < GlassRings; ++Ring)
				{
					for (int32 Side = 0; Side < GlassSides; ++Side)
					{
						const int32 A = Ring * Across + Side;
						const int32 B = A + 1;
						const int32 C = A + Across;
						const int32 D = C + 1;
						Triangles.Append({ A, C, B, B, C, D });
					}
				}
				Funnel->CreateMeshSection(0, GlassVertices, Triangles, GlassNormals, UVs, GlassColors, TArray<FProcMeshTangent>(), false);
				Funnel->SetMaterial(0, FunnelMaterial);
			}
			else
			{
				// Each new shape is a buffer upload (~0.13 ms GPU on the laptop at every frame): below Ultra only every
				// GlassUpdateEvery frames while it just whirls (its waves are slow), every frame while it forms or merges.
				const bool bBusy = Age < GrowTime * 2.f || bDrifting || PopT >= 0.f && PopT < MergePopTime;
				if (NumStrands > 0 || bBusy || ++GlassSkipped >= GlassUpdateEvery)
				{
					GlassSkipped = 0;
					Funnel->UpdateMeshSection(0, GlassVertices, GlassNormals, TArray<FVector2D>(), GlassColors, TArray<FProcMeshTangent>());
				}
			}
			FunnelMaterial->SetScalarParameterValue(TEXT("Opacity"), (bSoftGlass ? GlassOpacity : FunnelOpacity) * Amount * (1.f + 1.2f * Glare));
		}
	}

	// The skirt: snow and bits whirling round its foot, wider than the funnel, at their own speeds and heights.
	TransformScratch.SetNum(NumDust, EAllowShrinking::No);
	for (int32 Index = 0; Index < NumDust; ++Index)
	{
		const float U = (Index + 0.5f) / FMath::Max(NumDust, 1);
		const float Angle = SpinAngle * (1.1f + 0.9f * FMath::Frac(U * 5.3f)) + 2.f * UE_PI * U;
		const float Ring = (FunnelBottomRadius * Foot + 35.f * Foot + 85.f * Foot * FMath::Frac(U * 3.7f) + 20.f * FMath::Sin(Age * 6.f + Index)) * S * (0.4f + 0.6f * Grow);
		const FVector Local = Axis(0.f) + FVector(Ring * FMath::Cos(Angle), Ring * FMath::Sin(Angle) * SkirtDepth, (FunnelBottom + 8.f + 45.f * FMath::Frac(U * 3.3f)) * S);
		const float Size = (16.f + 14.f * FMath::Frac(U * 7.7f)) * S * Big * Amount;
		TransformScratch[Index] = FTransform(FRotator(0.f, FMath::RadiansToDegrees(Angle), 0.f), Local, FVector(FMath::Max(Size, 0.01f) / 100.f));
		Data[0] = DustColor.R;
		Data[1] = DustColor.G;
		Data[2] = DustColor.B;
		Data[3] = 0.1f;
		Data[4] = Amount;
		Dust->SetCustomData(Index, MakeArrayView(Data, NumCustomData), false);
	}
	if (NumDust > 0)
	{
		Dust->BatchUpdateInstancesTransforms(0, TransformScratch, false, false, true);
		APinguFX::MarkInstancesChanged(Dust);
	}

	// Snow and ice bits caught in it: they spiral up round the twisting funnel (and start again at its foot, shrinking away at
	// both ends), fast and a little further out.
	TransformScratch.SetNum(NumDebris, EAllowShrinking::No);
	for (int32 Index = 0; Index < NumDebris; ++Index)
	{
		const float U = (Index + 0.5f) / FMath::Max(NumDebris, 1);
		const float Along = FMath::Frac(U * 2.3f + 0.17f + Age * DebrisRise);
		const float H = HeightAt(Along) + (30.f * (1.f - Along) + 20.f * Along + Rise * 0.5f * Along) * S;
		const float Angle = SpinAngle * 2.1f + 2.f * UE_PI * FMath::Frac(U * 7.1f);
		const float Ring = (Profile(Along) + (40.f + 10.f * Along) * FormScale) * Uneven(Along, Angle);
		const FVector Local = Axis(Along) + FVector(Ring * FMath::Cos(Angle), Ring * FMath::Sin(Angle) * FunnelDepth, H - Ring * BandTilt * FMath::Sin(Angle));
		const float Size = 15.f * S * Big * Amount * (0.7f + 0.3f * FMath::Frac(U * 13.f)) * FMath::Min(FMath::Sin(UE_PI * Along) * 3.f, 1.f);
		TransformScratch[Index] = FTransform(FRotator(Age * 400.f + Index * 40.f, Age * 300.f, Index * 25.f), Local, FVector(FMath::Max(Size, 0.01f) / 100.f));
		Data[0] = DebrisColor.R;
		Data[1] = DebrisColor.G;
		Data[2] = DebrisColor.B;
		Data[3] = 0.4f;
		Data[4] = Amount;
		Debris->SetCustomData(Index, MakeArrayView(Data, NumCustomData), false);
	}
	if (NumDebris > 0)
	{
		Debris->BatchUpdateInstancesTransforms(0, TransformScratch, false, false, true);
		APinguFX::MarkInstancesChanged(Debris);
	}

	TickMergeLook(Eye, Now, S, Wide, Tall, Foot);
}

void APinguTornado::SpawnStreak(APinguFX* FX, const FVector& From, const FVector& Velocity, float Life, float Opacity)
{
	using namespace PinguTornadoPrivate;
	const float Speed = static_cast<float>(Velocity.Size());
	if (!FX || Speed < 1.f)
	{
		return;
	}
	// A soft glowing segment along its way (the effects' Glow mesh: its +Z turned along the velocity), fading as it goes.
	const float Scale = FMath::Max(SizeScale, 0.5f) * FMath::Sqrt(FMath::Max(ShownGrown, 1.f));
	FPinguParticleDesc D;
	D.Mesh = EPinguFXMesh::Glow;
	D.Location = From;
	D.Velocity = Velocity;
	D.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Velocity / Speed).Rotator();
	D.Length = FMath::Clamp(Speed * 0.09f, 50.f, 200.f) * Scale;
	D.Size = LookRand.FRandRange(9.f, 15.f) * Scale;
	D.EndSize = D.Size * 0.3f;
	D.Curve = EPinguSizeCurve::Linear;
	D.Life = Life;
	D.Color = FLinearColor(BandColor.R * 1.2f, BandColor.G * 1.2f, BandColor.B * 1.2f);
	D.Emissive = 0.6f;
	D.Opacity = Opacity;
	D.bFadeOut = true;
	FX->Spawn(D);
}
