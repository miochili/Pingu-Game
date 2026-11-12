// Pingu - Smash-style camera.

#include "PenguinCamera.h"
#include "BlockIsland.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PinguSettings.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/App.h"

namespace PenguinCameraPrivate
{
	/** The kill cam's frame: room around the victim (and hitter) in X and Z, cm, and the narrowest picture. */
	constexpr float KillCamPaddingX = 450.f;
	constexpr float KillCamPaddingZ = 300.f;
	constexpr float KillCamMinWidth = 1500.f;
	/** The kill cam's snap starts this much further out than its frame. */
	constexpr float KillCamSnapWide = 1.3f;

	// Critically damped spring (same idea as Unity's SmoothDamp).
	static float SmoothDamp(float Current, float Target, float& Velocity, float SmoothTime, float Dt)
	{
		SmoothTime = FMath::Max(0.0001f, SmoothTime);
		const float Omega = 2.f / SmoothTime;
		const float X = Omega * Dt;
		const float Exp = 1.f / (1.f + X + 0.48f * X * X + 0.235f * X * X * X);
		const float Change = Current - Target;
		const float Temp = (Velocity + Omega * Change) * Dt;
		Velocity = (Velocity - Omega * Temp) * Exp;
		float Output = Target + (Change + Temp) * Exp;
		if ((Target - Current > 0.f) == (Output > Target))
		{
			Output = Target;
			Velocity = 0.f;
		}
		return Output;
	}

	// How much further a grown penguin reaches out from its middle than a normal one (X: sideways, Z: up and down) - its
	// scaled capsule minus the unscaled one, so 0 at normal size (and for a shrunk one: the normal margins are plenty). An
	// 8x penguin is about 9 m tall: its head is ~390 cm further above its middle than a normal penguin's.
	/** Following stops only once the island's frame needs this share less than the furthest view (no flicking in and out). */
	constexpr float FollowHysteresis = 0.03f;

	static FVector GrownReach(const APenguinCharacter* Penguin)
	{
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		if (!Capsule)
		{
			return FVector::ZeroVector;
		}
		return FVector(FMath::Max(0.f, Capsule->GetScaledCapsuleRadius() - Capsule->GetUnscaledCapsuleRadius()), 0.f,
			FMath::Max(0.f, Capsule->GetScaledCapsuleHalfHeight() - Capsule->GetUnscaledCapsuleHalfHeight()));
	}
}

APenguinCamera::APenguinCamera()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics; // after the penguins moved

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	RootComponent = Camera;
	Camera->FieldOfView = FieldOfView;
	Camera->bConstrainAspectRatio = false;
	Camera->PostProcessSettings.bOverride_VignetteIntensity = true;
	Camera->PostProcessSettings.VignetteIntensity = 0.28f;
	Camera->PostProcessSettings.bOverride_BloomIntensity = true;
	Camera->PostProcessSettings.BloomIntensity = 0.35f;
	// Platformers read best without motion blur.
	Camera->PostProcessSettings.bOverride_MotionBlurAmount = true;
	Camera->PostProcessSettings.MotionBlurAmount = 0.f;

	// LAN: the server tells each client's player controller to look through the server's camera; that reference only
	// resolves on the client when the camera replicates. Its movement does not: every machine frames by itself.
	// (Nothing of it changes, so this rate only decides how soon a joining client gets it.)
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(10.f);
}

APenguinCamera* APenguinCamera::FindOrSpawn(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<APenguinCamera> It(World); It; ++It)
	{
		return *It;
	}
	// A client waits for the server's camera (a second, local one would get the shakes and swoops the other shows).
	if (!World->IsGameWorld() || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<APenguinCamera>(FVector(0.f, 3000.f, 400.f), FRotator(-8.f, -90.f, 0.f), Params);
}

void APenguinCamera::BeginPlay()
{
	Super::BeginPlay();
	ClaimLocalView();
}

void APenguinCamera::ClaimLocalView()
{
	// The player controller looks through the shared camera when it starts; on a client the camera can replicate in
	// after that. Only a view nobody set up yet (none, or the controller itself) is taken over.
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() != NM_Client)
	{
		return;
	}
	const UGameInstance* GameInstance = World->GetGameInstance();
	APlayerController* PlayerController = GameInstance ? GameInstance->GetFirstLocalPlayerController(World) : nullptr;
	if (PlayerController)
	{
		const AActor* ViewTarget = PlayerController->GetViewTarget();
		if (!ViewTarget || ViewTarget == PlayerController)
		{
			PlayerController->SetViewTarget(this);
		}
	}
}

float APenguinCamera::GetUserDistanceScale() const
{
	const APenguinGameState* PenguinState = APenguinGameState::Get(this);
	const EPinguPhase Phase = PenguinState ? PenguinState->GetPhase() : EPinguPhase::Menu;
	const bool bMatch = Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown || Phase == EPinguPhase::Fight
		|| Phase == EPinguPhase::RoundOver || Phase == EPinguPhase::Practice;
	const UPinguSettings* Settings = UPinguSettings::Get();
	return bMatch && Settings ? Settings->GetCameraDistance() : 1.f;
}

void APenguinCamera::AddTrauma(float Amount)
{
	Trauma = FMath::Clamp(Trauma + Amount, 0.f, 1.f);
}

void APenguinCamera::FocusOn(const FVector& Location, float Duration, float ZoomScale)
{
	FocusOverride = Location;
	FocusOverrideLeft = FMath::Max(Duration, 0.01f);
	FocusOverrideDuration = FocusOverrideLeft;
	FocusOverrideZoom = FMath::Clamp(ZoomScale, 0.2f, 1.5f);
}

void APenguinCamera::SetKillCamView(const FBox& Frame, float Blend, bool bSnap)
{
	KillCamFrame = Frame;
	KillCamBlend = Frame.IsValid ? FMath::Clamp(Blend, 0.f, 1.f) : 0.f;
	bKillCamSnap |= bSnap && KillCamBlend > 0.f;
}

void APenguinCamera::SnapToTargets(float HoldSeconds)
{
	bInitialized = false;
	bFollowPointValid = false; // (a new round: nothing knocked flying to hold for, nothing to glide from)
	GlideLeft = 0.f;
	SnapHoldLeft = FMath::Max(SnapHoldLeft, HoldSeconds);
}

bool APenguinCamera::GetIslandFrame(FBox& OutFrame)
{
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (!Island || (Island->Layout != EIslandLayout::Arena && Island->Layout != EIslandLayout::Lobby))
	{
		return false;
	}
	const FBox Bounds = Island->GetBounds();
	if (!Bounds.IsValid)
	{
		return false;
	}
	// The ground: the lowest and highest places to stand (once per island - blown-away blocks don't move the camera).
	if (GroundGeneration != Island->GetGeneration())
	{
		GroundGeneration = Island->GetGeneration();
		GroundMinZ = static_cast<float>(Bounds.Max.Z);
		GroundMaxZ = static_cast<float>(Bounds.Min.Z);
		for (const FVector& Stand : Island->GetSpreadSpawnLocations(12))
		{
			GroundMinZ = FMath::Min(GroundMinZ, static_cast<float>(Stand.Z));
			GroundMaxZ = FMath::Max(GroundMaxZ, static_cast<float>(Stand.Z));
		}
		if (GroundMinZ > GroundMaxZ)
		{
			GroundMinZ = GroundMaxZ = static_cast<float>(Bounds.Max.Z);
		}
	}
	// The whole island from end to end, from a little under its lowest ground to well above its highest block. A small
	// island (fewer than four penguins) keeps less room around it, so the camera comes in close.
	const float MarginScale = FMath::Clamp(Island->GetArenaScale(), MinIslandMarginScale, 1.f);
	OutFrame = FBox(FVector(Bounds.Min.X - IslandMargin.X * MarginScale, 0.f, GroundMinZ - IslandMarginBelow * MarginScale),
		FVector(Bounds.Max.X + IslandMargin.X * MarginScale, 0.f, FMath::Max(static_cast<float>(Bounds.Max.Z), GroundMaxZ) + IslandMargin.Y * MarginScale));
	if (Island->Layout == EIslandLayout::Lobby)
	{
		// The lobby's little island: close in, the name tags' room above the heads, and a band of sea below the island for
		// the lobby's buttons (they sit at the bottom of the screen).
		OutFrame = FBox(FVector(Bounds.Min.X - LobbyMargin.X, 0.f, GroundMinZ - LobbyMarginBelow),
			FVector(Bounds.Max.X + LobbyMargin.X, 0.f, FMath::Max(static_cast<float>(Bounds.Max.Z), GroundMaxZ) + LobbyMargin.Y));
	}
	// Penguins close beside or above it widen the frame; further away they are off screen. A penguin knocked flying does
	// not (the camera would sway out after every big hit and back): until it lands again it only gets its edge marker.
	const FBox Follow(OutFrame.Min - FVector(FollowMargin.X, 0.f, 200.f), OutFrame.Max + FVector(FollowMargin.X, 0.f, FollowMargin.Y));
	for (auto FlungIt = Flung.CreateIterator(); FlungIt; ++FlungIt)
	{
		if (!FlungIt.Key().IsValid())
		{
			FlungIt.RemoveCurrent();
		}
	}
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		const APenguinCharacter* Penguin = *It;
		if (!IsValid(Penguin) || Penguin->IsKnockedOut() || Penguin->IsHidden())
		{
			continue;
		}
		if (IsFlung(Penguin))
		{
			continue;
		}
		// A grown penguin counts with its whole body (shrink / grow rays, up to 8x: about 9 m tall standing): it widens the
		// frame when any of it comes close, and its head and feet stay in the picture. At normal size this is the old
		// framing exactly (Reach is 0). The camera never goes further out than MaxFrameDistance (Tick).
		const FVector Location = Penguin->GetActorLocation();
		const FVector Reach = PenguinCameraPrivate::GrownReach(Penguin);
		const FBox Near(Follow.Min - Reach, Follow.Max + Reach);
		if (Near.IsInsideOrOn(FVector(Location.X, 0.f, Location.Z)))
		{
			OutFrame += FVector(Location.X - 80.f - Reach.X, 0.f, Location.Z - 120.f - Reach.Z);
			OutFrame += FVector(Location.X + 80.f + Reach.X, 0.f, Location.Z + 200.f + Reach.Z);
		}
	}
	return true;
}

APenguinCharacter* APenguinCamera::FindLocalPenguin() const
{
	UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const APlayerController* PlayerController = GameInstance ? GameInstance->GetFirstLocalPlayerController(World) : nullptr;
	APenguinCharacter* Penguin = PlayerController ? Cast<APenguinCharacter>(PlayerController->GetPawn()) : nullptr;
	if (!IsValid(Penguin) || Penguin->IsKnockedOut() || Penguin->IsHidden() || Penguin->GetActorLocation().Z < IgnoreBelowZ)
	{
		return nullptr;
	}
	// The title menu's demo and the tests' bot matches (the player's penguin plays by itself there): the middle of everybody.
	const APenguinGameState* PenguinState = APenguinGameState::Get(this);
	if (PenguinState && (PenguinState->GetPhase() == EPinguPhase::Menu || PenguinState->IsMatchTest()))
	{
		return nullptr;
	}
	return Penguin;
}

bool APenguinCamera::GetFollowTarget(FBox& OutBody, bool& bOutFlung)
{
	bOutFlung = false;
	if (const APenguinCharacter* Own = FindLocalPenguin())
	{
		// All of it (a grown one is big): the edge keep holds for its head and its feet.
		const UCapsuleComponent* Capsule = Own->GetCapsuleComponent();
		const FVector Location = Own->GetActorLocation();
		const FVector Half = Capsule ? FVector(Capsule->GetScaledCapsuleRadius(), 0.f, Capsule->GetScaledCapsuleHalfHeight()) : FVector(40.f, 0.f, 60.f);
		OutBody = FBox(FVector(Location.X, 0.f, Location.Z) - Half, FVector(Location.X, 0.f, Location.Z) + Half);
		bOutFlung = IsFlung(Own);
		FollowPenguin = Own;
		return true;
	}
	FollowPenguin.Reset();
	// Nobody of our own (knocked out, watching, the demo): the middle of the penguins still in, not knocked flying, not falling
	// into the sea.
	FBox Group(ForceInit);
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		const APenguinCharacter* Penguin = *It;
		if (!IsValid(Penguin) || Penguin->IsKnockedOut() || Penguin->IsHidden() || Penguin->GetActorLocation().Z < IgnoreBelowZ || IsFlung(Penguin))
		{
			continue;
		}
		const FVector Location = Penguin->GetActorLocation();
		Group += FVector(Location.X, 0.f, Location.Z);
	}
	if (!Group.IsValid)
	{
		return false;
	}
	const FVector Middle = Group.GetCenter();
	OutBody = FBox(Middle, Middle);
	return true;
}

bool APenguinCamera::GetViewRect(FBox2D& OutRect) const
{
	if (ViewRectOverride.bIsValid)
	{
		OutRect = ViewRectOverride;
		return true;
	}
	UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const APlayerController* PlayerController = GameInstance ? GameInstance->GetFirstLocalPlayerController(World) : nullptr;
	if (!PlayerController)
	{
		return false;
	}
	int32 SizeX = 0;
	int32 SizeY = 0;
	PlayerController->GetViewportSize(SizeX, SizeY);
	if (SizeX < 2 || SizeY < 2)
	{
		return false;
	}
	// The four corners of the screen (top left, top right, bottom left, bottom right) on the play plane.
	const FVector2D Screen[4] = { FVector2D(0.0, 0.0), FVector2D(SizeX, 0.0), FVector2D(0.0, SizeY), FVector2D(SizeX, SizeY) };
	FVector Corner[4];
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FVector Origin;
		FVector Direction;
		if (!PlayerController->DeprojectScreenPositionToWorld(static_cast<float>(Screen[Index].X), static_cast<float>(Screen[Index].Y), Origin, Direction)
			|| FMath::Abs(Direction.Y) < KINDA_SMALL_NUMBER)
		{
			return false;
		}
		const double Along = -Origin.Y / Direction.Y;
		if (Along <= 0.0)
		{
			return false;
		}
		Corner[Index] = Origin + Direction * Along;
	}
	// The camera looks down a little: what it sees of the plane is a little wider at the top. The rectangle inside it.
	OutRect = FBox2D(FVector2D(FMath::Max(Corner[0].X, Corner[2].X), FMath::Max(Corner[2].Z, Corner[3].Z)),
		FVector2D(FMath::Min(Corner[1].X, Corner[3].X), FMath::Min(Corner[0].Z, Corner[1].Z)));
	OutRect.bIsValid = OutRect.Min.X < OutRect.Max.X && OutRect.Min.Y < OutRect.Max.Y;
	return OutRect.bIsValid;
}

bool APenguinCamera::GetSharedFrameRect(FBox2D& OutRect) const
{
	if (!LastIslandFrame.IsValid)
	{
		return false;
	}
	OutRect = FBox2D(FVector2D(LastIslandFrame.Min.X, LastIslandFrame.Min.Z), FVector2D(LastIslandFrame.Max.X, LastIslandFrame.Max.Z));
	return true;
}

bool APenguinCamera::IsFlung(const APenguinCharacter* Penguin)
{
	const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
	bool& bFlung = Flung.FindOrAdd(Penguin);
	if (!Move || Move->IsMovingOnGround() || Move->IsClimbing())
	{
		bFlung = false; // landed (or hanging on a ledge): it counts again
	}
	else if (Move->IsStunned() || (!Move->IsDashing() && FMath::Abs(Penguin->GetVelocity().X) > FlungSpeed))
	{
		bFlung = true; // hit in the air or knocked off the ground: stunned, or flying sideways faster than a dash
	}
	return bFlung;
}

void APenguinCamera::Tick(float DeltaSeconds)
{
	using namespace PenguinCameraPrivate;
	Super::Tick(DeltaSeconds);

	// Real time so the camera keeps gliding through hit-stop / slow motion.
	const float Dt = FMath::Clamp(static_cast<float>(FApp::GetDeltaTime()), 0.0001f, 0.1f);
	Camera->FieldOfView = FieldOfView;
	if (SnapHoldLeft > 0.f)
	{
		SnapHoldLeft -= Dt;
		bInitialized = false;
	}
	ClaimLocalView();

	// The island first (arena rounds, the lobby, the menu demo): all of it stays in the picture, penguins flung far away
	// go off screen (the HUD marks them at the edge). The practice course: the penguins, Smash-style.
	FBox IslandFrame(ForceInit);
	const bool bIslandFirst = GetIslandFrame(IslandFrame);
	FBox Targets(ForceInit);
	if (bIslandFirst)
	{
		Targets = IslandFrame;
	}
	else
	{
		for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
		{
			const APenguinCharacter* Penguin = *It;
			if (!IsValid(Penguin) || Penguin->IsKnockedOut() || Penguin->IsHidden())
			{
				continue;
			}
			const FVector Location = Penguin->GetActorLocation();
			if (Location.Z < IgnoreBelowZ)
			{
				continue;
			}
			const FVector Velocity = Penguin->GetVelocity();
			const FVector Ahead = Location + FVector(FMath::Clamp(Velocity.X * LookAheadTime, -MaxLookAhead, MaxLookAhead), 0.f, 0.f);
			Targets += Location;
			Targets += Ahead;
			// A grown penguin's head and feet too (nothing extra at normal size: the padding covers a normal penguin).
			const FVector Reach = GrownReach(Penguin);
			Targets += Location - Reach;
			Targets += Location + Reach;
		}
		if (Targets.IsValid)
		{
			LastTargets = Targets;
		}
		else if (LastTargets.IsValid)
		{
			Targets = LastTargets;
		}
		else
		{
			Targets = FBox(FVector(-100.f, 0.f, 0.f), FVector(100.f, 0.f, 100.f));
		}

		// Keep a good part of the island in the picture, so you see where you can go.
		if (const ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
		{
			const FBox IslandBox = Island->GetBounds();
			const double CenterX = Targets.GetCenter().X;
			if (IslandBox.IsValid && CenterX > IslandBox.Min.X - 2500.0 && CenterX < IslandBox.Max.X + 2500.0)
			{
				const FVector Center = Targets.GetCenter();
				const float MinX = FMath::Lerp(static_cast<float>(Center.X), static_cast<float>(IslandBox.Min.X), IslandFraming);
				const float MaxX = FMath::Lerp(static_cast<float>(Center.X), static_cast<float>(IslandBox.Max.X), IslandFraming);
				const float TopZ = FMath::Lerp(static_cast<float>(Center.Z), static_cast<float>(IslandBox.Max.Z), IslandFraming * 0.5f);
				Targets += FVector(MinX, 0.f, Center.Z);
				Targets += FVector(MaxX, 0.f, Center.Z);
				Targets += FVector(Center.X, 0.f, TopZ);
			}
		}
	}

	float Aspect = 16.f / 9.f;
	if (GEngine && GEngine->GameViewport)
	{
		FVector2D ViewportSize;
		GEngine->GameViewport->GetViewportSize(ViewportSize);
		if (ViewportSize.X > 1.f && ViewportSize.Y > 1.f)
		{
			Aspect = ViewportSize.X / ViewportSize.Y;
		}
	}

	const FVector Center = Targets.GetCenter();
	const FVector Extent = Targets.GetExtent();
	// (The island's frame has its margins built in.)
	const float Width = 2.f * Extent.X + (bIslandFirst ? 0.f : 2.f * FramePadding.X);
	const float Height = 2.f * Extent.Z + (bIslandFirst ? 0.f : 2.f * FramePadding.Y);
	const float TanHalfH = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(FieldOfView, 5.f, 170.f) * 0.5f));
	const float TanHalfV = TanHalfH / FMath::Max(Aspect, 0.1f);
	const float Needed = FMath::Max(Width * 0.5f / TanHalfH, Height * 0.5f / TanHalfV);
	// An arena never further out than MaxFrameDistance: one that needs more is followed (below).
	float TargetDistance = bIslandFirst ? FMath::Clamp(static_cast<float>(Needed), MinDistance, MaxFrameDistance)
		: FMath::Clamp(static_cast<float>(Needed), MinDistance, MaxDistance);
	// The player's camera distance (settings, this machine only; 2026-10-29): in a match nearer or further than the framing
	// wants. Nearer than the island needs: the view follows this machine's penguin inside the island's frame (below). The
	// title's demo and the lobby keep their framing (the menus are laid out on it).
	const float UserDistance = GetUserDistanceScale();
	TargetDistance *= UserDistance;
	float TargetX = static_cast<float>(Center.X);
	float TargetZ = bIslandFirst ? static_cast<float>(Center.Z) : FMath::Max(static_cast<float>(Center.Z) + FocusHeightOffset, MinFocusZ);
	LastIslandFrame = bIslandFirst ? IslandFrame : FBox(ForceInit);

	// ---- Following: the island does not fit. The view stays inside the island's frame (pan limits: no more sea past an
	// end than the frame shows) and follows this machine's penguin there - or, with none, the middle of the penguins.
	// (Once following, it only stops when the frame fits with a little room to spare: no flicking in and out at the edge.)
	const bool bWantFollow = bIslandFirst && (Needed > MaxFrameDistance * (FollowBlend > 0.5f ? 1.f - PenguinCameraPrivate::FollowHysteresis : 1.f)
		|| UserDistance < 0.97f);
	FollowBlend = bInitialized ? FMath::FInterpConstantTo(FollowBlend, bWantFollow ? 1.f : 0.f, Dt, 1.f / FMath::Max(FollowBlendTime, 0.01f))
		: (bWantFollow ? 1.f : 0.f);
	FBox FollowBody(ForceInit);
	bool bFollowFlung = false;
	const bool bHasFollow = bIslandFirst && GetFollowTarget(FollowBody, bFollowFlung);
	if (bHasFollow)
	{
		// The pan limits: the frame minus the view's half-size at the furthest distance (where one axis fits: its middle).
		const float LimitHalfW = TargetDistance * TanHalfH;
		const float LimitHalfH = TargetDistance * TanHalfV;
		const FVector2D LimitMin(IslandFrame.Min.X + LimitHalfW, IslandFrame.Min.Z + LimitHalfH);
		const FVector2D LimitMax(IslandFrame.Max.X - LimitHalfW, IslandFrame.Max.Z - LimitHalfH);
		const FVector2D FrameMiddle(Center.X, Center.Z);
		const auto Limit = [&](double Value, int32 Axis)
		{
			return LimitMin[Axis] > LimitMax[Axis] ? FrameMiddle[Axis] : FMath::Clamp(Value, LimitMin[Axis], LimitMax[Axis]);
		};
		const FVector BodyMiddle = FollowBody.GetCenter();
		const FVector2D Raw(BodyMiddle.X, BodyMiddle.Z);
		// Knocked flying: the followed point holds (the camera does not chase it out over the sea; the edge keep still holds).
		const FVector2D Want = bFollowFlung && bFollowPointValid ? FollowPoint : Raw;
		const FVector2D NewPoint(Limit(Want.X, 0), Limit(Want.Y, 1));
		// The followed point jumped (a warp, a respawn, back from a flight, somebody else to follow): a quick glide there
		// instead of trailing (or being dragged by the lag box).
		if (bInitialized && bFollowPointValid && FollowBlend > 0.f && FVector2D::Distance(NewPoint, FollowPoint) > GlideJumpDistance)
		{
			GlideLeft = GlideTime;
			GlideFrom = FVector2D(FocusX, FocusZ);
		}
		FollowPoint = NewPoint;
		bFollowPointValid = true;
		TargetX = FMath::Lerp(TargetX, static_cast<float>(FollowPoint.X), FollowBlend);
		TargetZ = FMath::Lerp(TargetZ, static_cast<float>(FollowPoint.Y), FollowBlend);
	}
	else if (!bIslandFirst)
	{
		FollowPenguin.Reset();
	}

	// Knock-out swoop.
	if (FocusOverrideLeft > 0.f)
	{
		FocusOverrideLeft -= Dt;
		const float T = 1.f - FocusOverrideLeft / FMath::Max(FocusOverrideDuration, 0.01f);
		const float Blend = FMath::Sin(FMath::Clamp(T, 0.f, 1.f) * UE_PI); // in and back out
		FVector Focus = FocusOverride;
		if (bIslandFirst)
		{
			// Swoop toward a penguin knocked out far away or deep in the sea, but keep the island in the picture.
			Focus.X = FMath::Clamp(Focus.X, IslandFrame.Min.X, IslandFrame.Max.X);
			Focus.Z = FMath::Clamp(Focus.Z, static_cast<double>(GroundMinZ) - 150.0, FMath::Max(IslandFrame.Max.Z, static_cast<double>(GroundMinZ)));
		}
		TargetX = FMath::Lerp(TargetX, static_cast<float>(Focus.X), Blend);
		TargetZ = FMath::Lerp(TargetZ, static_cast<float>(Focus.Z) + FocusHeightOffset * 0.5f, Blend);
		TargetDistance = FMath::Lerp(TargetDistance, FMath::Max(MinDistance * 0.55f, TargetDistance * FocusOverrideZoom), Blend);
	}

	// The kill cam (APinguKillCam): in on the round's last victim (and its hitter, when both fit), eased in and out.
	if (KillCamBlend > 0.f && KillCamFrame.IsValid)
	{
		const FVector KillCenter = KillCamFrame.GetCenter();
		const FVector KillExtent = KillCamFrame.GetExtent();
		const float KillWidth = FMath::Max(2.f * static_cast<float>(KillExtent.X) + 2.f * KillCamPaddingX, KillCamMinWidth);
		const float KillHeight = 2.f * static_cast<float>(KillExtent.Z) + 2.f * KillCamPaddingZ;
		const float KillNeeded = FMath::Max(KillWidth * 0.5f / TanHalfH, KillHeight * 0.5f / TanHalfV);
		const float KillDistance = FMath::Clamp(KillNeeded, MinDistance * 0.55f, TargetDistance);
		const float Ease = FMath::SmoothStep(0.f, 1.f, KillCamBlend);
		TargetX = FMath::Lerp(TargetX, static_cast<float>(KillCenter.X), Ease);
		TargetZ = FMath::Lerp(TargetZ, static_cast<float>(KillCenter.Z) + FocusHeightOffset * 0.3f, Ease);
		TargetDistance = FMath::Lerp(TargetDistance, KillDistance, Ease);
	}

	if (!bInitialized)
	{
		FocusX = TargetX;
		FocusZ = TargetZ;
		Distance = TargetDistance;
		FocusVelocityX = FocusVelocityZ = DistanceVelocity = 0.f;
		GlideLeft = 0.f;
		bInitialized = true;
	}
	if (bKillCamSnap)
	{
		// The kill cam's first frame: there at once, a little wider (the smoothing zooms in from it).
		bKillCamSnap = false;
		FocusX = TargetX;
		FocusZ = TargetZ;
		Distance = TargetDistance * KillCamSnapWide;
		FocusVelocityX = FocusVelocityZ = DistanceVelocity = 0.f;
		GlideLeft = 0.f;
	}
	// The title menu covers the left ~40% of the screen: show the demo on the right.
	const APenguinGameState* PenguinState = APenguinGameState::Get(this);
	const bool bMenu = PenguinState && PenguinState->GetPhase() == EPinguPhase::Menu;
	MenuShift = FMath::FInterpTo(MenuShift, bMenu ? 1.f : 0.f, Dt, 3.f);
	const float Shift = MenuShift * Distance * TanHalfH * 0.42f;
	TargetX -= Shift;

	if (GlideLeft > 0.f && FollowBlend > 0.f)
	{
		// Gliding to a point that jumped: there in GlideTime, eased in and out.
		GlideLeft = FMath::Max(0.f, GlideLeft - Dt);
		const float Ease = FMath::SmoothStep(0.f, 1.f, 1.f - GlideLeft / FMath::Max(GlideTime, 0.01f));
		FocusX = FMath::Lerp(static_cast<float>(GlideFrom.X), TargetX, Ease);
		FocusZ = FMath::Lerp(static_cast<float>(GlideFrom.Y), TargetZ, Ease);
		FocusVelocityX = FocusVelocityZ = 0.f;
	}
	else
	{
		GlideLeft = 0.f;
		FocusX = SmoothDamp(FocusX, TargetX, FocusVelocityX, FMath::Lerp(HorizontalSmoothTime, FollowSmoothTime, FollowBlend), Dt);
		FocusZ = SmoothDamp(FocusZ, TargetZ, FocusVelocityZ, FMath::Lerp(VerticalSmoothTime, FollowSmoothTime, FollowBlend), Dt);
	}
	Distance = SmoothDamp(Distance, TargetDistance, DistanceVelocity, ZoomSmoothTime, Dt);

	// Following: never too far behind the followed point, and the followed penguin never near the screen's edge (dragged
	// along at once). Not during a knock-out swoop.
	if (bHasFollow && FollowBlend > 0.f && FocusOverrideLeft <= 0.f && KillCamBlend <= 0.f)
	{
		const FVector2D HalfView(Distance * TanHalfH, Distance * TanHalfV);
		FVector2D Kept(FocusX + Shift, FocusZ); // (without the title menu's shift)
		if (GlideLeft <= 0.f)
		{
			Kept.X = FMath::Clamp(Kept.X, FollowPoint.X - FollowMaxLag.X * HalfView.X, FollowPoint.X + FollowMaxLag.X * HalfView.X);
			Kept.Y = FMath::Clamp(Kept.Y, FollowPoint.Y - FollowMaxLag.Y * HalfView.Y, FollowPoint.Y + FollowMaxLag.Y * HalfView.Y);
		}
		const FVector2D Room((1.f - FollowEdgeKeep.X) * HalfView.X, (1.f - FollowEdgeKeep.Y) * HalfView.Y);
		const FVector2D BodyMin(FollowBody.Min.X, FollowBody.Min.Z);
		const FVector2D BodyMax(FollowBody.Max.X, FollowBody.Max.Z);
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			double Lo = BodyMax[Axis] - Room[Axis];
			double Hi = BodyMin[Axis] + Room[Axis];
			if (Lo > Hi)
			{
				Lo = Hi = 0.5 * (BodyMin[Axis] + BodyMax[Axis]); // too big to keep that far in: its middle in the middle
			}
			Kept[Axis] = FMath::Clamp(Kept[Axis], Lo, Hi);
		}
		FocusX = FMath::Lerp(FocusX, static_cast<float>(Kept.X) - Shift, FollowBlend);
		FocusZ = FMath::Lerp(FocusZ, static_cast<float>(Kept.Y), FollowBlend);
	}
	FocusShift = Shift;

	// Shake (trauma squared feels natural: small hits barely move it).
	ShakeTime += Dt;
	Trauma = FMath::Max(0.f, Trauma - TraumaDecay * Dt);
	const float Amount = Trauma * Trauma;
	const float DistanceScale = Distance / 3000.f;
	const FVector ShakeMove(
		FMath::PerlinNoise1D(ShakeTime * 23.f) * ShakeOffset * Amount * DistanceScale,
		0.f,
		FMath::PerlinNoise1D(ShakeTime * 23.f + 57.3f) * ShakeOffset * Amount * DistanceScale);
	const float Roll = FMath::PerlinNoise1D(ShakeTime * 17.f + 131.f) * ShakeRoll * Amount;

	const FRotator Rotation(Pitch, -90.f, Roll);
	const FVector FocusPoint(FocusX, 0.f, FocusZ);
	SetActorLocationAndRotation(FocusPoint - FRotator(Pitch, -90.f, 0.f).Vector() * Distance + ShakeMove, Rotation);
}
