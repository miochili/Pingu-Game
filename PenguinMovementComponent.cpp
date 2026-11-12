// Pingu - platform-fighter movement.

#include "PenguinMovementComponent.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PenguinGameState.h"
#include "PinguAntiCheat.h"
#include "PinguHostAuthority.h"
#include "PinguTraitTable.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/ReplicatedState.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformTime.h"
#include "PinguKillCam.h"
#include "Net/UnrealNetwork.h"

namespace PenguinMovementPrivate
{
	constexpr float DefaultWorldGravity = 980.f;
	constexpr float DownThreshold = -0.6f;
	/** Ability pushes with a smaller upward part do not lift a standing penguin off the ground (see AddAbilityImpulse). */
	constexpr float MinGroundLiftSpeed = 300.f;

	// ---- belly slide
	/** A slide press waits this long for its chance (as a dash press does). */
	constexpr float SlideBufferTime = 0.1f;
	/** No slide lasts longer than this, whatever the floor (ice stretches one to about 1.3 s). */
	constexpr float SlideMaxTime = 3.f;
	/** cm/s^2 a slide loses in the air (a dive, off a ledge): the belly keeps its speed. */
	constexpr float SlideAirDrag = 200.f;
	/** cm/s: a slide that bumps into a penguin before it may hit bounces back this fast (a hit stops it dead). */
	constexpr float SlideBumpRebound = 120.f;
	/** StickNextLanding: share of the sideways speed a thrown penguin keeps when it comes down. */
	constexpr float StickLandingKeep = 0.2f;
	/** A ground icicle throws you again only after this long (by then the throw has carried you far out of it). */
	constexpr float GroundPadRearm = 0.5f;
	/** An ice shell's bubble that hits a wall (normal at least this sideways) bounces off it with ShellBounceKeep of its
	 *  speed: when it was pushed (a gust, a noot, another shell) in the last ShellPushMemory seconds and still goes at
	 *  least ShellBounceMinSpeed, or anyway when it goes faster than it can run. Otherwise it is only walking into the wall. */
	constexpr float ShellBounceWallNormal = 0.7f;
	constexpr float ShellBounceRunShare = 1.05f;
	constexpr float ShellBounceKeep = 0.7f;
	constexpr float ShellBounceMinSpeed = 150.f;
	constexpr float ShellPushMemory = 1.f;
	/** AddAbilityImpulse: a sideways part at least this big counts as a push (for the bounce above). */
	constexpr float ShellPushMinSide = 100.f;
	/** Sticky: a wall this close to the side of the capsule (cm) is one to stick to or jump off ... */
	constexpr float StickyWallReach = 12.f;
	/** ... and after a wall jump the stick can't steer back into the wall for this long (s). */
	constexpr float WallJumpSteerLock = 0.18f;
	/** Sticky on a wall: the stick this far away from it (of a full push) lets go of it; meanwhile it is pressed against the
	 *  wall this fast (cm/s: it touches it, the wall stops it there). */
	constexpr float StickyLetGoInput = 0.3f;
	constexpr float StickyClingHug = 120.f;
	/** Second Chance's pass through the island: a capsule inside a block when it ends goes to the nearest free spot - rings
	 *  this far apart (cm), at most this many. */
	constexpr float PassThroughNudgeStep = 20.f;
	constexpr int32 PassThroughNudgeRings = 80;

	// ---- LAN play (see the class comment)
	/** The owner sends its movement state at most this often while only the velocity, the position or the stick change... */
	constexpr float NetSyncInterval = 1.f / 60.f;
	/** ...at once when an event, a flag or an effect confirmation is new, and at least this often anyway (packets get lost). */
	constexpr float NetSyncKeepAlive = 0.25f;
	/** Moved this far on the server since the owner's last move (a respawn): the owner's older positions are ignored... */
	constexpr float NetRelocateDistance = 50.f;
	/** ...until the owner is there too, or for this long (the owner is authoritative for its position after all). */
	constexpr float NetGuardTimeout = 0.4f;
	/** A teleport forwarded with NetTeleportPenguin waits for the owner's confirmation, at most this long. */
	constexpr float NetTeleportGuardTimeout = 1.f;
	/** The owner's state is ignored at most this long while an effect waits for its confirmation (in case one never arrives). */
	constexpr float NetStaleTimeout = 1.f;
	/** Replicated end times are rewritten only when they move by more than this (not a new value every frame). */
	constexpr float NetEndTimeTolerance = 0.02f;
	constexpr int32 NetMaxPendingEffects = 32;
	/** SetHoverGlide never drifts faster than this (cm/s; a wall in the way does not make it rush at the end). */
	constexpr float HoverGlideMaxSpeed = 4000.f;

	// ---- the host's say over a remote player's penguin (PinguHostAuthority.h)
	/** cm below the feet the host still finds the ground (a walking penguin is about 2 cm above it). */
	constexpr float HostFloorReach = 40.f;
	/** The owner's position counts again (after the host had the penguin) within this many cm of the host's own path (+ its
	 *  speed x HostPathSpeedTime: a fast flight is further off for the same few frames of lag; a tornado's throw at 2900 cm/s was
	 *  167-186 cm off for an honest client on the laptop, 2026-11-04) ... */
	constexpr float HostPathTolerance = 150.f;
	constexpr float HostPathSpeedTime = 0.05f;
	/** ... of the last this many seconds plus 1.5 x the owner's ping (its moves are that late), at most HostPathMaxAge. */
	constexpr double HostPathMinAge = 0.2;
	constexpr double HostPathMaxAge = 0.5;
	/** Refused this long in a row: the owner is put back where the host has it (and again every HostPullBackRepeat s). */
	constexpr double HostPullBackAfter = 0.5;
	constexpr double HostPullBackRepeat = 1.0;
	/** No move from the owner for this long: the host moves its penguin meanwhile (a quiet joiner never hangs in the air). */
	constexpr double HostQuietTime = 0.5;
	/** A jump pad this close (cm) to a rising penguin explains the rise. */
	constexpr float HostPadReach = 260.f;
	/** After a lift (low gravity, a shell's float, a pass through the island) its height stays unjudged this long (s). */
	constexpr float HostFreeAfterTime = 0.4f;
	/** A slide's way (cm) the owner may claim beyond the host's own measure (the slide began before its sync came). */
	constexpr float HostSlideTravelRoom = 80.f;
	/** cm/s the host's measure of a slide's speed may be passed (the hit's strength). */
	constexpr float HostSlideSpeedRoom = 150.f;

	/** A comes after B (sequence numbers wrap around). */
	bool SeqAfter(uint8 A, uint8 B)
	{
		return static_cast<int8>(static_cast<uint8>(A - B)) > 0;
	}

	bool SeqAfter16(uint16 A, uint16 B)
	{
		return static_cast<int16>(static_cast<uint16>(A - B)) > 0;
	}

	int8 QuantizeAxis(float Value)
	{
		return static_cast<int8>(FMath::Clamp(FMath::RoundToInt(Value * 127.f), -127, 127));
	}

	int16 QuantizeSpeed(double Value)
	{
		return static_cast<int16>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(Value)), -32767, 32767));
	}

	/** A speed or a distance that is never negative (cm/s, cm). */
	uint16 QuantizeLandSpeed(float Value)
	{
		return static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(Value), 0, 65535));
	}

	int32 QuantizeLocation(double Value)
	{
		return FMath::RoundToInt(static_cast<float>(Value));
	}

	/** Everything but the stick: a change here goes to the server at once. */
	bool SameEventsAndFlags(const FPenguinNetMoveState& A, const FPenguinNetMoveState& B)
	{
		return A.JumpEvents == B.JumpEvents && A.AirJumpEvents == B.AirJumpEvents && A.DashEvents == B.DashEvents
			&& A.LandEvents == B.LandEvents && A.ClimbEvents == B.ClimbEvents && A.StompLandEvents == B.StompLandEvents
			&& A.PadBounceEvents == B.PadBounceEvents && A.SlideEvents == B.SlideEvents
			&& A.bBellySliding == B.bBellySliding && A.bLastSlideWasSlip == B.bLastSlideWasSlip && A.bLowCapsule == B.bLowCapsule
			&& A.LastLandSpeed == B.LastLandSpeed && A.LastStompLandSpeed == B.LastStompLandSpeed
			&& A.bFacingLeft == B.bFacingLeft && A.bJumpHeld == B.bJumpHeld && A.bStomping == B.bStomping && A.bStompHop == B.bStompHop
			&& A.bStompWindup == B.bStompWindup && A.bFastFalling == B.bFastFalling && A.bOnIce == B.bOnIce
			&& A.bDashFromGround == B.bDashFromGround && A.bApexHang == B.bApexHang && A.bLastJumpFromRun == B.bLastJumpFromRun
			&& A.bWallClinging == B.bWallClinging && A.BounceEvents == B.BounceEvents && A.WallJumpEvents == B.WallJumpEvents
			&& A.bPassThroughIsland == B.bPassThroughIsland && A.bFlapping == B.bFlapping;
	}

	/** Server: a status timer as an end time on the server clock (rewritten only when it really moves). */
	void PublishEndTime(float& End, float Left, float Now)
	{
		if (Left > 0.f)
		{
			const float NewEnd = Now + Left;
			if (FMath::Abs(NewEnd - End) > NetEndTimeTolerance)
			{
				End = NewEnd;
			}
		}
		else if (End > Now + NetEndTimeTolerance)
		{
			End = 0.f; // ended early (thawed by a hit, a reset)
		}
	}
}

FPenguinNetworkMoveDataContainer::FPenguinNetworkMoveDataContainer()
{
	NewMoveData = &PenguinMoveData[0];
	PendingMoveData = &PenguinMoveData[1];
	OldMoveData = &PenguinMoveData[2];
}

void FPenguinNetworkMoveData::ClientFillNetworkMoveData(const FSavedMove_Character& ClientMove, ENetworkMoveType MoveType)
{
	FCharacterNetworkMoveData::ClientFillNetworkMoveData(ClientMove, MoveType);
#if !UE_BUILD_SHIPPING
	// The net test's cheating joiner: -PinguCheat=speed says it is in the air while it runs (once the host's ground speed limit
	// ran only for "walking"), -PinguCheat=fakey moves 3 m off the play plane.
	if (PinguHostAuthority::HasDevCheat(TEXT("speed")) && MovementMode == static_cast<uint8>(MOVE_Walking))
	{
		MovementMode = static_cast<uint8>(MOVE_Falling);
	}
	if (PinguHostAuthority::HasDevCheat(TEXT("fakey")))
	{
		Location.Y += 300.f;
	}
#endif
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	const UObject* BaseComponent = MovementBase;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	const UObject* BaseOwner = MovementBasePhysicsObjectOwner.Get();
	const bool bNameable = (!BaseComponent || BaseComponent->IsSupportedForNetworking()) && (!BaseOwner || BaseOwner->IsSupportedForNetworking());
	if (bNameable || !ClientMove.CharacterOwner)
	{
		return;
	}
	MovementBasePhysicsObjectOwner = nullptr;
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	MovementBase = nullptr;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	MovementBaseBoneName = NAME_None;
	Location = FRepMovement::RebaseOntoZeroOrigin(ClientMove.SavedLocation, ClientMove.CharacterOwner->GetCharacterMovement());
	Acceleration = ClientMove.Acceleration;
}

UPenguinMovementComponent::UPenguinMovementComponent()
{
	bWantsInitializeComponent = true;
	SetNetworkMoveDataContainer(PenguinMoveDataContainer);

	// 2D plane: X right, Z up, Y locked at the origin.
	bConstrainToPlane = true;
	bSnapToPlaneAtStart = true;
	SetPlaneConstraintNormal(FVector(0.f, 1.f, 0.f));

	// Facing is handled by the character (mesh yaw), the capsule never rotates - not even on a turning base.
	bOrientRotationToMovement = false;
	bUseControllerDesiredRotation = false;
	bIgnoreBaseRotation = true;
	RotationRate = FRotator::ZeroRotator;

	MaxWalkSpeed = RunSpeed;
	MaxAcceleration = GroundAcceleration;
	BrakingDecelerationWalking = GroundBraking;
	BrakingDecelerationFalling = AirBraking;
	GroundFriction = 8.f;
	AirControl = 1.f;
	AirControlBoostMultiplier = 0.f;
	AirControlBoostVelocityThreshold = 0.f;
	FallingLateralFriction = 0.f;
	bApplyGravityWhileJumping = true;

	// Block world: flat-bottomed floor checks so block edges feel solid, no auto step-up onto blocks.
	bUseFlatBaseForFloorChecks = true;
	PerchRadiusThreshold = 0.f;
	PerchAdditionalHeight = 0.f;
	MaxStepHeight = 20.f;
	SetWalkableFloorAngle(50.f);
	bCanWalkOffLedges = true;
	LedgeCheckThreshold = 4.f;

	// Not the engine's crouch: the belly slide lowers the capsule itself (SetLowCapsule), with our own LAN state.
	NavAgentProps.bCanCrouch = false;
	NavAgentProps.bCanJump = true;

	bRunPhysicsWithNoController = true;
	bEnablePhysicsInteraction = false;

	// LAN play: a player's penguin is simulated by its owner. The server takes the owner's position and movement mode with
	// every move (ServerMoveHandleClientError: no error check, the client position is used) and never sends a correction.
	SetIsReplicatedByDefault(true); // our replicated state and RPCs
	bIgnoreClientMovementErrorChecksAndCorrection = true;
	bServerAcceptClientAuthoritativePosition = true;
	bClientIgnoreMovementCorrections = true;
	// Other machines' penguins are the engine's simulated proxies. Linear smoothing only offsets the mesh location (the character
	// turns the mesh for the facing) and catches up within one update; a jump of more than a couple of penguins is a teleport.
	NetworkSmoothingMode = ENetworkSmoothingMode::Linear;
	NetworkMaxSmoothUpdateDistance = 180.f;
	NetworkNoSmoothUpdateDistance = 300.f;

	RecomputeJumpPhysics();
}

void UPenguinMovementComponent::InitializeComponent()
{
	Super::InitializeComponent();
	if (CharacterOwner && CharacterOwner->GetCapsuleComponent() && !bLowCapsule)
	{
		StandingHalfHeight = CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	}
	RecomputeJumpPhysics();
	FindTraitComponent();
	AirJumpsLeft = GetMaxAirJumpsNow();
	AirDashesLeft = MaxAirDashes;
}

#if WITH_EDITOR
void UPenguinMovementComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RecomputeJumpPhysics();
}
#endif

void UPenguinMovementComponent::RecomputeJumpPhysics()
{
	const float H = FMath::Max(JumpHeight, 1.f);
	const float T = FMath::Max(JumpTimeToApex, 0.05f);
	const float G = 2.f * H / (T * T); // cm/s^2 needed for that height/time

	float WorldGravity = PenguinMovementPrivate::DefaultWorldGravity;
	if (const UWorld* World = GetWorld())
	{
		WorldGravity = FMath::Max(FMath::Abs(World->GetGravityZ()), 1.f);
	}

	GravityScale = G / WorldGravity;
	JumpVelocity = 2.f * H / T;
	JumpZVelocity = JumpVelocity;
	AirJumpVelocity = FMath::Sqrt(2.f * G * FMath::Max(DoubleJumpHeight, 1.f));

	MaxWalkSpeed = RunSpeed;
	MaxAcceleration = GroundAcceleration;
}

// ---------------------------------------------------------------------------- input

void UPenguinMovementComponent::SetMoveInput(FVector2D Input)
{
	MoveInput.X = FMath::Clamp(Input.X, -1.f, 1.f);
	MoveInput.Y = FMath::Clamp(Input.Y, -1.f, 1.f);
}

void UPenguinMovementComponent::JumpPressed()
{
	bJumpHeld = true;
	JumpBufferLeft = JumpBufferTime;
	StunJumpHoldLeft = StunJumpBufferTime;
	++JumpStats.Presses;
}

void UPenguinMovementComponent::JumpReleased()
{
	bJumpHeld = false;
}

void UPenguinMovementComponent::DashPressed()
{
	bDashRequested = true;
}

void UPenguinMovementComponent::BellySlidePressed()
{
	bSlideRequested = true;
}

bool UPenguinMovementComponent::IsDashing() const
{
	return MovementMode == MOVE_Custom && CustomMovementMode == static_cast<uint8>(EPenguinCustomMode::Dash);
}

bool UPenguinMovementComponent::IsClimbing() const
{
	return MovementMode == MOVE_Custom && CustomMovementMode == static_cast<uint8>(EPenguinCustomMode::Climb);
}

void UPenguinMovementComponent::ResetMovementState()
{
	if (IsDashing())
	{
		SetMovementMode(MOVE_Falling);
	}
	Velocity = FVector::ZeroVector;
	JumpBufferLeft = 0.f;
	StunJumpHoldLeft = 0.f;
	DashBufferLeft = 0.f;
	bDashRequested = false;
	DashCooldownLeft = 0.f;
	StopBellySlide();
	SetLowCapsule(false, false); // standing again where the caller puts it (a respawn)
	SlideBufferLeft = 0.f;
	bSlideRequested = false;
	BellySlideCooldownLeft = 0.f;
	SlideTravel = 0.f;
	bFastFalling = false;
	bFastFallArmed = false;
	bRisingFromJump = false;
	bJumpCutDone = false;
	bApexHangAllowed = false;
	AirJumpsLeft = GetMaxAirJumpsNow();
	AirDashesLeft = MaxAirDashes;
	bWallClinging = false;
	ClingWallDir = 0.f;
	WallJumpLockLeft = 0.f;
	bRubberBouncePending = false;
	bRubberFallTracked = false;
	EndPassThrough(false);
	StunLeft = 0.f;
	SlapDizzyLeft = 0.f;
	FrozenLeft = 0.f;
	LowGravityLeft = 0.f;
	FloatLeft = 0.f;
	SlipperyLeft = 0.f;
	HoverLeft = 0.f;
	HoverGlideLeft = 0.f;
	PullLeft = 0.f;
	PushedLeft = 0.f;
	bStickNextLanding = false;
	bStomping = false;
	bStompHop = false;
	bStompWindup = false;
	bKnockedOut = false;
	if (IsClimbing())
	{
		SetMovementMode(MOVE_Falling);
	}
}

// ---------------------------------------------------------------------------- per-move logic

void UPenguinMovementComponent::UpdateCharacterStateBeforeMovement(float DeltaSeconds)
{
	if (!RunsMovementHere())
	{
		// A simulated proxy (SimulateMovement calls this too): the members mirror the replicated state, nothing runs here.
		Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
		return;
	}
	bHostTimersRun |= bHostSimulating; // (the host's simulation of a remote player's copy: its timers run here this frame)

	const bool bGrounded = IsMovingOnGround();

	DashCooldownLeft = FMath::Max(0.f, DashCooldownLeft - DeltaSeconds);
	BellySlideCooldownLeft = FMath::Max(0.f, BellySlideCooldownLeft - DeltaSeconds);
	StunLeft = FMath::Max(0.f, StunLeft - DeltaSeconds);
	SlapDizzyLeft = FMath::Max(0.f, SlapDizzyLeft - DeltaSeconds);
	FrozenLeft = FMath::Max(0.f, FrozenLeft - DeltaSeconds);
	LowGravityLeft = FMath::Max(0.f, LowGravityLeft - DeltaSeconds);
	FloatLeft = FMath::Max(0.f, FloatLeft - DeltaSeconds);
	SlipperyLeft = FMath::Max(0.f, SlipperyLeft - DeltaSeconds);
	PushedLeft = FMath::Max(0.f, PushedLeft - DeltaSeconds);
	WallJumpLockLeft = FMath::Max(0.f, WallJumpLockLeft - DeltaSeconds);
	TimeSinceJumpLaunch += DeltaSeconds;
	HoverLeft = FMath::Max(0.f, HoverLeft - DeltaSeconds);
	PullLeft = FMath::Max(0.f, PullLeft - DeltaSeconds);

	// The tornado's user hangs perfectly still in its eye (no gravity either: GetGravityZ, NewFallVelocity) - or drifts at an
	// even speed to its place in a giant tornado's eye (SetHoverGlide: what is left of the way over what is left of the time).
	HoverGlideVelocity = FVector::ZeroVector;
	if (HoverLeft > 0.f && HoverGlideLeft > 0.f && UpdatedComponent)
	{
		FVector Way = HoverGlideTo - UpdatedComponent->GetComponentLocation();
		Way.Y = 0.f;
		HoverGlideVelocity = (Way / FMath::Max(HoverGlideLeft, DeltaSeconds)).GetClampedToMaxSize(PenguinMovementPrivate::HoverGlideMaxSpeed);
		if (IsMovingOnGround())
		{
			HoverGlideVelocity.Z = 0.f;
		}
		HoverGlideLeft = FMath::Max(0.f, HoverGlideLeft - DeltaSeconds);
	}
	else if (HoverLeft <= 0.f)
	{
		HoverGlideLeft = 0.f;
	}
	if (HoverLeft > 0.f)
	{
		Velocity = HoverGlideVelocity;
	}
	// Sucked in by a tornado: off the ground as soon as the pull wants to lift us (the funnel takes us up).
	if (PullLeft > 0.f && !bKnockedOut && IsMovingOnGround() && GetTornadoPullVelocity().Z > 60.f)
	{
		SetMovementMode(MOVE_Falling);
	}

	// Belly stomp: the hop tops out, hangs for a moment (wind-up), then slams down.
	if (bStompHop)
	{
		StompHopTime += DeltaSeconds;
		if ((IsFalling() && Velocity.Z < 350.f) || StompHopTime > 0.3f)
		{
			bStompHop = false;
			BeginStompWindup(StompWindupTime * 0.6f);
		}
	}
	if (bFlapping)
	{
		UpdateFlap(DeltaSeconds);
	}
	if (bStompWindup)
	{
		StompWindupLeft -= DeltaSeconds;
		Velocity.X = FMath::FInterpTo(static_cast<float>(Velocity.X), 0.f, DeltaSeconds, 10.f);
		Velocity.Z = FMath::FInterpTo(static_cast<float>(Velocity.Z), 0.f, DeltaSeconds, 14.f);
		if (StompWindupLeft <= 0.f)
		{
			bStompWindup = false;
			bStomping = true;
			Velocity = FVector(0.f, 0.f, -StompSpeed);
			SetMovementMode(MOVE_Falling);
		}
	}
	const bool bControl = !HasNoControl();

	// The belly slide is over once control is gone (a hit, frozen, a stomp, knocked out, input locked) or after too long
	// in the air (a dive or a slide off a ledge falls normally from then on).
	if (bBellySliding)
	{
		SlideElapsed += DeltaSeconds;
		SlideAirTime = bGrounded ? 0.f : SlideAirTime + DeltaSeconds;
		// (Under a ceiling too low to stand up it goes on as long as it takes to get out.)
		if (!bControl || SlideAirTime > BellySlideMaxAirTime || (SlideElapsed > PenguinMovementPrivate::SlideMaxTime && !IsStuckLow()))
		{
			StopBellySlide();
		}
	}

	if (bGrounded)
	{
		TimeSinceGrounded = 0.f;
		bCoyoteAvailable = true;
		bRisingFromJump = false;
		bFastFalling = false;
		bFastFallArmed = false;
		AirJumpsLeft = GetMaxAirJumpsNow();
		AirDashesLeft = MaxAirDashes;
		bOnIce = IsFloorIce() || SlipperyLeft > 0.f || GetTraitNumbers().bAllFloorsIce; // icy feet slip on everything (Ice Rink: everybody)
	}
	else
	{
		TimeSinceGrounded += DeltaSeconds;
		bOnIce = false;
	}

	// Facing follows the stick; a dash or a belly slide locks it.
	if (bControl && !IsDashing() && !bBellySliding && FMath::Abs(MoveInput.X) > 0.2f)
	{
		Facing = FMath::Sign(MoveInput.X);
	}

	// Dash (buffered for a moment so a press during cooldown/landing still comes out).
	if (bDashRequested)
	{
		bDashRequested = false;
		DashBufferLeft = 0.1f;
	}
	if (DashBufferLeft > 0.f)
	{
		if (bControl && TryStartDash())
		{
			DashBufferLeft = 0.f;
		}
		else
		{
			DashBufferLeft = FMath::Max(0.f, DashBufferLeft - DeltaSeconds);
		}
	}

	// Belly slide (buffered the same way; a press during a dash waits for its end).
	if (bSlideRequested)
	{
		bSlideRequested = false;
		SlideBufferLeft = PenguinMovementPrivate::SlideBufferTime;
	}
	if (SlideBufferLeft > 0.f)
	{
		if (bControl && TryBellySlide())
		{
			SlideBufferLeft = 0.f;
		}
		else if (!IsDashing())
		{
			SlideBufferLeft = FMath::Max(0.f, SlideBufferLeft - DeltaSeconds);
		}
	}

	// Jump (buffered). The buffer does not run out during a dash, so the jump comes out right after it. A press during a hit
	// stun or while frozen waits as well (up to StunJumpBufferTime) and comes out the moment the penguin can move again.
	if (JumpBufferLeft > 0.f)
	{
		const bool bWaitingForControl = !bControl && (StunLeft > 0.f || FrozenLeft > 0.f) && !bKnockedOut && !bInputLocked;
		if (bControl && TryJump())
		{
			JumpBufferLeft = 0.f;
			StunJumpHoldLeft = 0.f;
		}
		else if (IsDashing())
		{
			// comes out when the dash ends
		}
		else if (bWaitingForControl && StunJumpHoldLeft > 0.f)
		{
			StunJumpHoldLeft = FMath::Max(0.f, StunJumpHoldLeft - DeltaSeconds);
		}
		else
		{
			JumpBufferLeft = FMath::Max(0.f, JumpBufferLeft - DeltaSeconds);
			if (JumpBufferLeft <= 0.f)
			{
				NoteDroppedJump();
			}
		}
	}

	// Down only matters in the air (fast fall); on the ground it does nothing.
	const bool bDownHeld = MoveInput.Y < PenguinMovementPrivate::DownThreshold;
	if (IsFalling())
	{
		// Variable jump height: letting go early cuts the upward speed (not before MinJumpHoldTime: however short the tap -
		// even a press and release that arrive in the same frame - it is a proper short hop).
		if (bRisingFromJump && !bJumpHeld && !bJumpCutDone && TimeSinceJumpLaunch >= MinJumpHoldTime && Velocity.Z > JumpCutSpeed)
		{
			Velocity.Z = JumpCutSpeed;
			bJumpCutDone = true;
		}
		if (Velocity.Z <= 0.f)
		{
			bRisingFromJump = false;
		}

		// Fast fall: a fresh press of down in the air arms it, it fires once we are near/after the apex.
		if (bDownHeld && !bDownHeldLastFrame)
		{
			bFastFallArmed = true;
		}
		if (!bDownHeld)
		{
			bFastFallArmed = false;
		}
		// (Not while stuck to a wall: down slides down it there - UpdateWallCling.)
		if (bControl && bFastFallArmed && !bFastFalling && !bWallClinging && Velocity.Z < FastFallTriggerSpeed)
		{
			bFastFalling = true;
			Velocity.Z = FMath::Min(Velocity.Z, -FastFallStartSpeed);
		}
	}
	bDownHeldLastFrame = bDownHeld;

	// Sticky: stick to a wall touched on the way down (after the jump: a wall jump this frame lets go of it).
	UpdateWallCling(DeltaSeconds, !HasNoControl());

	// Rubber: the top of the fall - the highest the capsule's middle got since it left the ground (a jump this frame
	// counts from where it took off; TryRubberLandingBounce starts the next one at the bounce).
	if (IsFalling() && UpdatedComponent)
	{
		const float Z = static_cast<float>(UpdatedComponent->GetComponentLocation().Z);
		RubberFallTopZ = bRubberFallTracked ? FMath::Max(RubberFallTopZ, Z) : Z;
		bRubberFallTracked = true;
	}
	else
	{
		bRubberFallTracked = false;
	}

	// Second Chance's throw: through the island's blocks until the feet are over the highest one.
	UpdatePassThrough(DeltaSeconds);

	// On the belly the capsule is small (a slide fits through a gap one block high); up again once there is room.
	UpdateLowCapsule();

	Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
}

bool UPenguinMovementComponent::TryJump()
{
	if (!CharacterOwner)
	{
		return false;
	}
	if (IsStuckLow())
	{
		return false; // lying under a ceiling too low to stand up: the press waits (buffer) until it is out
	}

	bool bFromGround = IsMovingOnGround();

	if (IsDashing())
	{
		if (!(bDashFromGround && DashElapsed >= DashJumpCancelTime))
		{
			return false; // air dash: the buffered jump comes out when the dash ends
		}
		// Dash-jump: jump out of a ground dash and keep most of its speed.
		const float KeepX = DashDirection.X * FMath::Max(GetEffectiveDashExitSpeed(), GetEffectiveDashSpeed() * 0.6f);
		EndDash(true);
		Velocity.X = KeepX;
		bFromGround = true;
	}

	if (bFromGround)
	{
		if (bBellySliding)
		{
			// Jump out of a belly slide: its speed comes along (as much as a dash-jump keeps, at most).
			const float Along = FMath::Max(0.f, static_cast<float>(Velocity.X) * SlideDir);
			Velocity.X = SlideDir * FMath::Min(Along, BellySlideJumpSpeed);
		}
		++JumpStats.GroundJumps;
		LaunchJump(false);
		return true;
	}

	if (IsFalling() && bCoyoteAvailable && TimeSinceGrounded <= CoyoteTime && !bRisingFromJump)
	{
		++JumpStats.CoyoteJumps; // coyote time
		LaunchJump(false);
		return true;
	}

	// Falling with the ground right below the feet (landing in a moment, or just walked down a block): a ground jump, as if
	// we had landed already. Spending the double jump here would leave nothing for the press that was meant for it.
	if (IsFalling() && !bRisingFromJump && Velocity.Z <= 50.f && IsNearGroundForJump())
	{
		++JumpStats.NearGroundJumps;
		RefreshAirMoves();
		LaunchJump(false);
		return true;
	}

	// Sticky next to a wall: up over a ledge in reach, else a wall jump (the double jump stays for later).
	if (IsFalling() && TryWallJump())
	{
		return true;
	}

	// Out of jumps right next to a ledge: grab it and pull yourself up.
	if (IsFalling() && AirJumpsLeft <= 0 && TryStartClimb())
	{
		++JumpStats.Climbs;
		return true;
	}

	if (IsFalling() && AirJumpsLeft > 0)
	{
		--AirJumpsLeft;
		++JumpStats.AirJumps;
		LaunchJump(true);
		return true;
	}
	return false;
}

bool UPenguinMovementComponent::IsNearGroundForJump() const
{
	if (!CharacterOwner || !UpdatedComponent || GroundJumpReach <= 0.f)
	{
		return false;
	}
	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	if (!Capsule)
	{
		return false;
	}
	// The same floor test the landing uses, only reaching further down.
	FFindFloorResult Floor;
	const float Reach = GroundJumpReach + MAX_FLOOR_DIST;
	ComputeFloorDist(UpdatedComponent->GetComponentLocation(), Reach, Reach, Floor, Capsule->GetScaledCapsuleRadius());
	if (!Floor.bBlockingHit || !Floor.IsWalkableFloor() || Floor.GetDistanceToFloor() > GroundJumpReach)
	{
		return false;
	}
	const AActor* FloorActor = Floor.HitResult.GetActor();
	return !FloorActor || !FloorActor->IsA<APawn>(); // no jumping off somebody's head
}

void UPenguinMovementComponent::NoteDroppedJump()
{
	++JumpStats.Dropped;
	const TCHAR* Reason = TEXT("");
	if (StunLeft > 0.f)
	{
		++JumpStats.DroppedStunned;
		Reason = TEXT("stunned by a hit");
	}
	else if (FrozenLeft > 0.f)
	{
		++JumpStats.DroppedFrozen;
		Reason = TEXT("frozen");
	}
	else if (IsFalling() && AirJumpsLeft <= 0)
	{
		++JumpStats.DroppedNoAirJump;
		Reason = TEXT("in the air with the double jump used up");
	}
	else
	{
		++JumpStats.DroppedOther;
		Reason = IsClimbing() ? TEXT("climbing a ledge") : (IsStomping() ? TEXT("belly stomp") : (bInputLocked ? TEXT("input locked") : TEXT("other")));
	}
	if (CharacterOwner && CharacterOwner->IsPlayerControlled())
	{
		UE_LOG(LogPingu, Log, TEXT("[JUMPS] a jump press was dropped: %s (movement mode %d, stun %.2f s, frozen %.2f s, air jumps %d, speed %.0f / %.0f)"),
			Reason, static_cast<int32>(MovementMode), StunLeft, FrozenLeft, AirJumpsLeft, Velocity.X, Velocity.Z);
	}
}

void UPenguinMovementComponent::LaunchJump(bool bAirJump)
{
	StopBellySlide(); // a jump (or the double jump out of a dive) ends the slide
	Velocity.Z = (bAirJump ? AirJumpVelocity : JumpVelocity) * JumpScale * GetTraitJumpScale();

	if (bAirJump)
	{
		// The double jump lets you change direction instantly.
		if (FMath::Abs(MoveInput.X) > 0.3f && Velocity.X * MoveInput.X <= 0.f)
		{
			Velocity.X = MoveInput.X * GetEffectiveRunSpeed() * DoubleJumpRedirect;
		}
		++AirJumpEvents;
	}
	else
	{
		bLastJumpFromRun = FMath::Abs(Velocity.X) > GetEffectiveRunSpeed() * 0.35f;
		++JumpEvents;
	}

	bCoyoteAvailable = false;
	bRisingFromJump = true;
	bJumpCutDone = false;
	bApexHangAllowed = true;
	bFastFalling = false;
	bFastFallArmed = false;
	TimeSinceJumpLaunch = 0.f;
	SetMovementMode(MOVE_Falling);
}

// ---------------------------------------------------------------------------- dash

bool UPenguinMovementComponent::TryStartDash()
{
	if (!CharacterOwner || IsDashing() || DashCooldownLeft > 0.f || IsStuckLow())
	{
		return false; // (lying under a ceiling too low to stand up: the press waits until it is out)
	}
	const bool bGrounded = IsMovingOnGround();
	if (!bGrounded && AirDashesLeft <= 0)
	{
		return false;
	}

	FVector2D Dir = MoveInput;
	if (!bEightWayDash)
	{
		Dir.Y = 0.f;
	}
	if (bGrounded && Dir.Y < 0.f)
	{
		Dir.Y = 0.f;
	}
	if (Dir.SizeSquared() < 0.1f)
	{
		Dir = FVector2D(Facing, 0.f);
	}

	// Snap to 8 directions.
	const float Step = UE_PI / 4.f;
	const float Angle = FMath::RoundToFloat(FMath::Atan2(Dir.Y, Dir.X) / Step) * Step;
	DashDirection = FVector(FMath::Cos(Angle), 0.f, FMath::Sin(Angle));
	if (FMath::Abs(DashDirection.X) < UE_KINDA_SMALL_NUMBER) { DashDirection.X = 0.f; }
	if (FMath::Abs(DashDirection.Z) < UE_KINDA_SMALL_NUMBER) { DashDirection.Z = 0.f; }
	DashDirection.Normalize();

	if (!bGrounded)
	{
		--AirDashesLeft;
	}
	bDashFromGround = bGrounded;
	DashElapsed = 0.f;
	DashCooldownLeft = DashCooldown;
	bFastFalling = false;
	bFastFallArmed = false;
	bRisingFromJump = false;
	if (FMath::Abs(DashDirection.X) > 0.1f)
	{
		Facing = FMath::Sign(DashDirection.X);
	}
	StopBellySlide(); // a dash out of a slide (or a dive)
	++DashEvents;
	SetMovementMode(MOVE_Custom, static_cast<uint8>(EPenguinCustomMode::Dash));
	return true;
}

void UPenguinMovementComponent::EndDash(bool bIntoJump)
{
	if (!IsDashing())
	{
		return;
	}

	FVector Exit = DashDirection * GetEffectiveDashExitSpeed();
	if (Exit.Z > 0.f)
	{
		Exit.Z *= 0.55f; // upward dashes don't fling you to the moon
	}
	Velocity = Exit;

	if (bIntoJump)
	{
		SetMovementMode(MOVE_Falling);
		return;
	}

	FFindFloorResult Floor;
	FindFloor(UpdatedComponent->GetComponentLocation(), Floor, false);
	if (Floor.IsWalkableFloor() && Floor.GetDistanceToFloor() < 10.f && DashDirection.Z <= 0.01f)
	{
		SetMovementMode(MOVE_Walking);
	}
	else
	{
		SetMovementMode(MOVE_Falling);
	}
}

void UPenguinMovementComponent::PhysCustom(float DeltaTime, int32 Iterations)
{
	if (!RunsMovementHere())
	{
		// A simulated proxy in the dash / climb mode (MoveSmooth calls this): the dash and climb state live on the owner.
		PhysProxyCustom(DeltaTime);
		return;
	}
	if (CustomMovementMode == static_cast<uint8>(EPenguinCustomMode::Dash))
	{
		PhysDash(DeltaTime, Iterations);
		return;
	}
	if (CustomMovementMode == static_cast<uint8>(EPenguinCustomMode::Climb))
	{
		PhysClimb(DeltaTime, Iterations);
		return;
	}
	Super::PhysCustom(DeltaTime, Iterations);
}

void UPenguinMovementComponent::PhysProxyCustom(float DeltaTime)
{
	if (DeltaTime < MIN_TICK_TIME || !UpdatedComponent)
	{
		return;
	}
	// Carry on along the replicated velocity until the next update (which also ends the mode).
	const FVector Delta = Velocity * DeltaTime;
	if (Delta.IsNearlyZero())
	{
		return;
	}
	FHitResult Hit(1.f);
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, false);
	}
}

bool UPenguinMovementComponent::TryStartClimb()
{
	UWorld* World = GetWorld();
	if (!CharacterOwner || !UpdatedComponent || !World || IsStomping() || !CanGrabLedge())
	{
		return false;
	}
	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	if (!Capsule)
	{
		return false;
	}
	const float Radius = Capsule->GetScaledCapsuleRadius();
	float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	FVector Loc = UpdatedComponent->GetComponentLocation();
	if (bLowCapsule)
	{
		// Lying low (a dive): the climb is worked out for the standing capsule - it gets up as it grabs the ledge.
		if (!HasRoomToStand(&Loc))
		{
			return false;
		}
		HalfHeight = GetStandingHalfHeight();
	}
	const float FeetZ = static_cast<float>(Loc.Z) - HalfHeight;
	// Toward the wall: the stick if it is held, otherwise where we face.
	const float Dir = FMath::Abs(MoveInput.X) > 0.2f ? FMath::Sign(MoveInput.X) : Facing;
	const ECollisionChannel Channel = UpdatedComponent->GetCollisionObjectType();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguClimb), false, CharacterOwner);

	// 1) A wall right in front of us, somewhere between the knees and the top of the body.
	float WallX = 0.f;
	bool bWall = false;
	for (const float Up : { -0.45f, 0.f, 0.6f })
	{
		FHitResult WallHit;
		const FVector Start(Loc.X, 0.f, Loc.Z + Up * HalfHeight);
		const FVector End = Start + FVector(Dir * (Radius + GetEffectiveClimbReachForward()), 0.f, 0.f);
		if (World->LineTraceSingleByChannel(WallHit, Start, End, Channel, Params) && FMath::Abs(WallHit.ImpactNormal.X) > 0.7f)
		{
			WallX = static_cast<float>(WallHit.ImpactPoint.X);
			bWall = true;
			break;
		}
	}
	if (!bWall)
	{
		return false;
	}

	// 2) The top of that wall, within reach above our feet.
	FHitResult TopHit;
	const float ProbeX = WallX + Dir * 12.f;
	const float ReachUp = GetEffectiveClimbReachUp();
	const FVector TopStart(ProbeX, 0.f, FeetZ + ReachUp + 10.f);
	const FVector TopEnd(ProbeX, 0.f, FeetZ - 15.f);
	if (!World->LineTraceSingleByChannel(TopHit, TopStart, TopEnd, Channel, Params) || TopHit.bStartPenetrating || TopHit.ImpactNormal.Z < 0.7f)
	{
		return false;
	}
	const float TopZ = static_cast<float>(TopHit.ImpactPoint.Z);
	if (TopZ - FeetZ > ReachUp || TopZ < FeetZ - 10.f)
	{
		return false;
	}

	// 3) Room to rise up next to the wall and to stand on top just past the edge.
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Radius, HalfHeight);
	const FVector RiseLoc(Loc.X, 0.f, TopZ + HalfHeight + 3.f);
	const FVector StandLoc(WallX + Dir * (Radius + 6.f), 0.f, TopZ + HalfHeight + 3.f);
	if (World->OverlapBlockingTestByChannel(RiseLoc, FQuat::Identity, Channel, Shape, Params)
		|| World->OverlapBlockingTestByChannel(StandLoc, FQuat::Identity, Channel, Shape, Params))
	{
		return false;
	}

	if (IsDashing())
	{
		EndDash(true);
	}
	SetLowCapsule(false, true); // up from the belly (there is room: checked above)
	ClimbStart = Loc;
	ClimbMid = RiseLoc;
	ClimbEnd = StandLoc;
	ClimbElapsed = 0.f;
	ClimbDir = Dir;
	Facing = Dir;
	Velocity = FVector::ZeroVector;
	bRisingFromJump = false;
	bJumpCutDone = true;
	bApexHangAllowed = false;
	bFastFalling = false;
	bFastFallArmed = false;
	bCoyoteAvailable = false;
	StopBellySlide(); // a dive that grabs a ledge
	++ClimbEvents;
	SetMovementMode(MOVE_Custom, static_cast<uint8>(EPenguinCustomMode::Climb));
	return true;
}

void UPenguinMovementComponent::PhysClimb(float DeltaTime, int32 Iterations)
{
	if (DeltaTime < MIN_TICK_TIME || !UpdatedComponent)
	{
		return;
	}
	ClimbElapsed += DeltaTime;
	const float T = FMath::Clamp(ClimbElapsed / FMath::Max(ClimbDuration, 0.05f), 0.f, 1.f);
	// Pull up along the wall first, then roll over the edge.
	FVector Target;
	if (T < 0.6f)
	{
		Target = FMath::Lerp(ClimbStart, ClimbMid, FMath::InterpEaseOut(0.f, 1.f, T / 0.6f, 2.f));
	}
	else
	{
		Target = FMath::Lerp(ClimbMid, ClimbEnd, FMath::InterpEaseInOut(0.f, 1.f, (T - 0.6f) / 0.4f, 2.f));
	}
	// The path was checked when the grab started (room to rise next to the wall and to stand on top), so no sweep:
	// a sweep would snag on the wall we are sliding up against.
	const FVector OldLocation = UpdatedComponent->GetComponentLocation();
	UpdatedComponent->SetWorldLocation(Target, false, nullptr, ETeleportType::None);
	Velocity = (Target - OldLocation) / DeltaTime;

	if (T >= 1.f)
	{
		Velocity = FVector(ClimbDir * 160.f, 0.f, 0.f);
		FFindFloorResult Floor;
		FindFloor(UpdatedComponent->GetComponentLocation(), Floor, false);
		SetMovementMode(Floor.IsWalkableFloor() ? MOVE_Walking : MOVE_Falling);
	}
}

void UPenguinMovementComponent::PhysDash(float DeltaTime, int32 Iterations)
{
	if (DeltaTime < MIN_TICK_TIME)
	{
		return;
	}

	float Remaining = DeltaTime;
	while (IsDashing() && Remaining >= MIN_TICK_TIME && Iterations < MaxSimulationIterations)
	{
		++Iterations;
		const float Step = FMath::Min(Remaining, FMath::Max(DashDuration - DashElapsed, MIN_TICK_TIME));
		Remaining -= Step;
		DashElapsed += Step;

		// Full speed, easing out over the last 30%.
		const float Alpha = FMath::Clamp(DashElapsed / FMath::Max(DashDuration, 0.01f), 0.f, 1.f);
		const float DashEase = Alpha < 0.7f ? 1.f : FMath::Lerp(1.f, 0.55f, (Alpha - 0.7f) / 0.3f);
		Velocity = DashDirection * GetEffectiveDashSpeed() * DashEase;

		const FVector Delta = Velocity * Step;
		FHitResult Hit(1.f);
		SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);

		if (Hit.IsValidBlockingHit())
		{
			// Dashing diagonally down into the floor turns into a ground dash.
			if (DashDirection.Z < 0.f && IsWalkable(Hit))
			{
				DashDirection = FVector(FMath::Sign(DashDirection.X), 0.f, 0.f);
				if (DashDirection.IsNearlyZero())
				{
					EndDash(false);
					break;
				}
			}
			HandleImpact(Hit, Step, Delta);
			SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
			if ((Hit.Normal | DashDirection) < -0.85f)
			{
				EndDash(false); // head-on into a wall
				break;
			}
		}

		if (DashElapsed >= DashDuration - UE_KINDA_SMALL_NUMBER)
		{
			EndDash(false);
			break;
		}
	}

	if (!IsDashing() && Remaining >= MIN_TICK_TIME)
	{
		StartNewPhysics(Remaining, Iterations);
	}
}

// ---------------------------------------------------------------------------- ice

const FName UPenguinMovementComponent::JumpPadTag(TEXT("PinguJumpPad"));

bool UPenguinMovementComponent::IsJumpPad(const UPrimitiveComponent* Component)
{
	return Component && Component->ComponentHasTag(JumpPadTag);
}

void UPenguinMovementComponent::BounceOffJumpPad()
{
	if (!CharacterOwner || bKnockedOut)
	{
		return;
	}
	// The pad throws you, not the jump button: no jump cut, no apex hang - and your air moves are back, so pads chain.
	StopBellySlide();
	bStomping = false;
	bStompHop = false;
	bStompWindup = false;
	bFastFalling = false;
	bFastFallArmed = false;
	bRisingFromJump = false;
	bJumpCutDone = true;
	bApexHangAllowed = false;
	bCoyoteAvailable = false;
	LowGravityLeft = 0.f;
	Velocity.Y = 0.f;
	Velocity.Z = JumpPadSpeed * JumpScale * GetTraitJumpScale();
	TimeSinceJumpLaunch = 0.f;
	AirJumpsLeft = GetMaxAirJumpsNow();
	AirDashesLeft = MaxAirDashes;
	SetMovementMode(MOVE_Falling);
	++PadBounceEvents; // the boing (sound, sparkles, stretch) on every machine, from the replicated counter
}

bool UPenguinMovementComponent::IsTouchingGroundPad() const
{
	const UCapsuleComponent* Capsule = CharacterOwner ? CharacterOwner->GetCapsuleComponent() : nullptr;
	if (!Capsule || !GetWorld())
	{
		return false;
	}
	TArray<APenguinProjectile*> All;
	APenguinProjectile::GetAll(GetWorld(), All);
	if (All.Num() == 0)
	{
		return false;
	}
	// The capsule as its axis line (the ends of its round caps' centres) and its radius.
	const FVector Center = Capsule->GetComponentLocation();
	const float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
	const float AxisHalf = FMath::Max(Capsule->GetScaledCapsuleHalfHeight() - CapsuleRadius, 0.f);
	const FVector Bottom = Center - FVector(0.f, 0.f, AxisHalf);
	const FVector Top = Center + FVector(0.f, 0.f, AxisHalf);
	for (const APenguinProjectile* Projectile : All)
	{
		if (!Projectile || !Projectile->IsGroundPad())
		{
			continue;
		}
		FVector PadA;
		FVector PadB;
		float PadRadius = 0.f;
		Projectile->GetGroundPadSegment(PadA, PadB, PadRadius);
		FVector OnCapsule;
		FVector OnPad;
		FMath::SegmentDistToSegmentSafe(Bottom, Top, PadA, PadB, OnCapsule, OnPad);
		if (FVector::DistSquared(OnCapsule, OnPad) <= FMath::Square(CapsuleRadius + PadRadius))
		{
			return true;
		}
	}
	return false;
}

bool UPenguinMovementComponent::IsFloorIce() const
{
	if (!CurrentFloor.IsWalkableFloor())
	{
		return false;
	}
	const UPrimitiveComponent* FloorComponent = CurrentFloor.HitResult.GetComponent();
	return FloorComponent && FloorComponent->ComponentHasTag(IceTag);
}

// ---------------------------------------------------------------------------- belly slide

bool UPenguinMovementComponent::TryBellySlide()
{
	if (!CharacterOwner || !UpdatedComponent || !RunsMovementHere() || HasNoControl() || bBellySliding || IsDashing() || IsClimbing()
		|| BellySlideCooldownLeft > 0.f || !CanBellySlideNow())
	{
		return false;
	}
	const bool bGround = IsMovingOnGround();
	if (!bGround && !IsFalling())
	{
		return false;
	}
	// Along the stick when it points sideways, else the way we face.
	const float Dir = FMath::Abs(MoveInput.X) > 0.2f ? FMath::Sign(MoveInput.X) : Facing;
	SlideDir = Dir;
	Facing = Dir;
	bBellySliding = true;
	bBellyDive = !bGround;
	SlideTravel = 0.f;
	SlideLastX = static_cast<float>(UpdatedComponent->GetComponentLocation().X);
	SlideElapsed = 0.f;
	SlideAirTime = 0.f;
	BellySlideCooldownLeft = BellySlideCooldown;
	bLastSlideWasSlip = SlipperyLeft > 0.f; // icy feet slip out from under it
	bRisingFromJump = false;
	bApexHangAllowed = false;
	bFastFalling = false;
	bFastFallArmed = false;
	// Faster already (a push the same way): the slide starts from that (the extra bleeds off fast: CalcBellySlideVelocity).
	const float Along = static_cast<float>(Velocity.X) * Dir;
	if (bGround)
	{
		Velocity.X = Dir * FMath::Max(BellySlideSpeed, Along);
	}
	else
	{
		// Belly-flop: forward and down at once. Never up (no extra way back to the island); it lands as a full slide, a
		// double jump or an air dash gets out of it.
		Velocity.X = Dir * FMath::Max(BellyDiveSpeed, Along);
		Velocity.Z = FMath::Min(static_cast<float>(Velocity.Z), -BellyDiveDropSpeed);
		LowGravityLeft = 0.f;
	}
	Velocity.Y = 0.f;
	++SlideEvents;
	return true;
}

void UPenguinMovementComponent::StopBellySlide()
{
	bFlapping = false;        // (whatever takes over from a slide - a jump, a dash, a hit, a push up, a teleport - ends a flap too)
	bHostSlideActive = false; // (a remote player's copy: the slide the host counted is over too)
	bBellySliding = false;
	bBellyDive = false;
	SlideAirTime = 0.f;
}

float UPenguinMovementComponent::GetBellySlideDeceleration() const
{
	// v^2 = v0^2 - 2 a d: the slow-down that takes BellySlideSpeed down to BellySlideExitSpeed over BellySlideDistance.
	const float Start = FMath::Max(BellySlideSpeed, 1.f);
	const float Exit = FMath::Clamp(BellySlideExitSpeed, 0.f, Start * 0.95f);
	return (Start * Start - Exit * Exit) / (2.f * FMath::Max(BellySlideDistance, 10.f));
}

void UPenguinMovementComponent::CalcBellySlideVelocity(float DeltaTime, bool bAir)
{
	// No steering: the stick only matters again once the penguin is up (or jumps / dashes out of it).
	float Speed = FMath::Max(0.f, static_cast<float>(Velocity.X) * SlideDir);
	if (bAir)
	{
		Speed = FMath::Max(0.f, Speed - PenguinMovementPrivate::SlideAirDrag * DeltaTime);
	}
	else
	{
		// On ice - an ice floor, or icy feet from the ice gun - a slide goes twice as far.
		const bool bIcy = IsFloorIce() || SlipperyLeft > 0.f;
		float Deceleration = GetBellySlideDeceleration() * (bIcy ? BellySlideIceDecelerationScale : 1.f);
		if (Speed > BellySlideSpeed)
		{
			// A push on top (a slap's lunge, a recoil) bleeds off fast: it never turns into a glide across the island.
			Deceleration += GroundOverspeedDeceleration;
		}
		Speed = FMath::Max(0.f, Speed - Deceleration * DeltaTime);
	}
	if (!bAir && Speed <= BellySlideExitSpeed && bLowCapsule && !HasRoomToStand())
	{
		// Under a ceiling too low to stand up the slide goes on (at least crawling) until it is out; the stick turns it
		// around (out of a dead end).
		if (FMath::Abs(MoveInput.X) > 0.2f && FMath::Sign(MoveInput.X) != SlideDir)
		{
			SlideDir = FMath::Sign(MoveInput.X);
			Facing = SlideDir;
			Speed = 0.f;
		}
		Speed = FMath::Max(Speed, BellySlideCrawlSpeed);
		Velocity.X = SlideDir * Speed;
		Velocity.Y = 0.f;
		return;
	}
	Velocity.X = SlideDir * Speed;
	Velocity.Y = 0.f;
	if (!bAir && Speed <= BellySlideExitSpeed)
	{
		StopBellySlide(); // back on its feet: from here on it is running or braking (a wall ends it the same way)
	}
}

void UPenguinMovementComponent::LandBellySlide()
{
	SlideAirTime = 0.f;
	const float Along = FMath::Max(0.f, static_cast<float>(Velocity.X) * SlideDir);
	if (bBellyDive)
	{
		bBellyDive = false;
		Velocity.X = SlideDir * FMath::Max(Along, BellySlideSpeed); // the belly-flop lands as a full slide
	}
	else if (Along <= BellySlideExitSpeed)
	{
		StopBellySlide(); // slid off a ledge and lost its speed in the air: lands on its feet
	}
}

void UPenguinMovementComponent::EndBellySlideOnContact(bool bHit)
{
	if (!CharacterOwner || !bBellySliding)
	{
		return;
	}
	const int32 Serial = SlideEvents;
	ApplyBellySlideContact(Serial, bHit);
	if (IsRemotePlayerCopy())
	{
		// The owner's slide stops too - that slide only: one it started since then goes on.
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::BellySlideEnd;
		Effect.Value = static_cast<float>(Serial);
		Effect.bFlag = bHit;
		ForwardEffect(Effect, true);
	}
}

void UPenguinMovementComponent::ApplyBellySlideContact(int32 Serial, bool bHit)
{
	if (!bBellySliding || SlideEvents != Serial)
	{
		return;
	}
	StopBellySlide();
	// A hit gave its speed to the penguin it knocked away; a bump bounces back a little.
	Velocity.X = bHit ? 0.f : -SlideDir * PenguinMovementPrivate::SlideBumpRebound;
	Velocity.Y = 0.f;
}

// ---------------------------------------------------------------------------- lying low (the belly capsule)

float UPenguinMovementComponent::GetStandingHalfHeight() const
{
	const UCapsuleComponent* Capsule = CharacterOwner ? CharacterOwner->GetCapsuleComponent() : nullptr;
	if (!Capsule)
	{
		return StandingHalfHeight;
	}
	const float Unscaled = StandingHalfHeight > 0.f ? StandingHalfHeight : Capsule->GetUnscaledCapsuleHalfHeight();
	return Unscaled * Capsule->GetShapeScale();
}

bool UPenguinMovementComponent::HasRoomToStand(FVector* OutStandLocation) const
{
	const UWorld* World = GetWorld();
	const UCapsuleComponent* Capsule = CharacterOwner ? CharacterOwner->GetCapsuleComponent() : nullptr;
	if (!bLowCapsule || !World || !Capsule || !UpdatedComponent)
	{
		if (OutStandLocation && UpdatedComponent)
		{
			*OutStandLocation = UpdatedComponent->GetComponentLocation();
		}
		return true;
	}
	// The standing capsule on the same feet (the engine's UnCrouch test, a hair taller so it never starts touching).
	const float Inflation = UE_KINDA_SMALL_NUMBER * 10.f;
	const float StandHalf = GetStandingHalfHeight();
	const FVector Here = UpdatedComponent->GetComponentLocation();
	FVector Stand = Here + FVector(0.f, 0.f, StandHalf - Capsule->GetScaledCapsuleHalfHeight());
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguStandUp), false, CharacterOwner);
	FCollisionResponseParams Response;
	InitCollisionParams(Params, Response);
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), StandHalf + Inflation);
	const ECollisionChannel Channel = UpdatedComponent->GetCollisionObjectType();
	bool bBlocked = World->OverlapBlockingTestByChannel(Stand, FQuat::Identity, Channel, Shape, Params, Response);
	if (bBlocked && IsMovingOnGround() && CurrentFloor.bBlockingHit && CurrentFloor.FloorDist > Inflation)
	{
		// Something just overhead: try right on the floor.
		Stand.Z -= CurrentFloor.FloorDist - Inflation;
		bBlocked = World->OverlapBlockingTestByChannel(Stand, FQuat::Identity, Channel, Shape, Params, Response);
	}
	if (!bBlocked && OutStandLocation)
	{
		*OutStandLocation = Stand;
	}
	return !bBlocked;
}

void UPenguinMovementComponent::SetLowCapsule(bool bLow, bool bKeepFeet)
{
	UCapsuleComponent* Capsule = CharacterOwner ? CharacterOwner->GetCapsuleComponent() : nullptr;
	if (bLow == bLowCapsule || !Capsule || !UpdatedComponent)
	{
		return;
	}
	if (StandingHalfHeight <= 0.f)
	{
		StandingHalfHeight = Capsule->GetUnscaledCapsuleHalfHeight(); // (standing: bLowCapsule is still false here)
	}
	const float Radius = Capsule->GetUnscaledCapsuleRadius();
	const float LowHalf = FMath::Clamp(BellySlideHalfHeight, Radius, StandingHalfHeight);
	const float Adjust = StandingHalfHeight - LowHalf;
	const float ScaledAdjust = Adjust * Capsule->GetShapeScale();

	FVector StandLocation = UpdatedComponent->GetComponentLocation();
	if (!bLow && bKeepFeet && !HasRoomToStand(&StandLocation))
	{
		return; // no room to get up here
	}
	bLowCapsule = bLow;
	Capsule->SetCapsuleSize(Radius, bLow ? LowHalf : StandingHalfHeight);
	if (bKeepFeet && ScaledAdjust > 0.f)
	{
		// The feet stay where they are: the middle goes down onto them, or up (the room was checked: no sweep).
		const FVector Delta = bLow ? FVector(0.f, 0.f, -ScaledAdjust) : StandLocation - UpdatedComponent->GetComponentLocation();
		UpdatedComponent->MoveComponent(Delta, UpdatedComponent->GetComponentQuat(), bLow, nullptr, MOVECOMP_NoFlags, ETeleportType::TeleportPhysics);
	}
	// The mesh stays where it was: the engine's crouch offsets (the mesh base the character's tick and the network
	// smoothing build on).
	if (bLow)
	{
		CharacterOwner->OnStartCrouch(Adjust, ScaledAdjust);
	}
	else
	{
		CharacterOwner->OnEndCrouch(Adjust, ScaledAdjust);
	}
	if (!bKeepFeet && (CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy
		|| (IsNetMode(NM_ListenServer) && CharacterOwner->GetRemoteRole() == ROLE_AutonomousProxy)))
	{
		// A mirrored penguin: its lowered (raised) positions come with the state, before or after it - the smoothing offset
		// takes the mesh base's step back (as the engine does for a crouch), so the mesh never jumps.
		if (FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character())
		{
			ClientData->MeshTranslationOffset -= FVector(0.f, 0.f, bLow ? ScaledAdjust : -ScaledAdjust);
			ClientData->OriginalMeshTranslationOffset = ClientData->MeshTranslationOffset;
		}
	}
	bForceNextFloorCheck = true;
}

void UPenguinMovementComponent::UpdateLowCapsule()
{
	if (bBellySliding)
	{
		SetLowCapsule(true, true);
		return;
	}
	if (!bLowCapsule)
	{
		return;
	}
	SetLowCapsule(false, true); // up again - only where there is room
	if (bLowCapsule && !HasNoControl() && IsMovingOnGround())
	{
		// Still under a ceiling too low to stand up (a hit or a bump ended the slide in there): back on the belly, crawling
		// on (CalcBellySlideVelocity) the way the stick points, or the way it faces. No new slide (no slide event, no hit).
		bBellySliding = true;
		bBellyDive = false;
		SlideDir = FMath::Abs(MoveInput.X) > 0.2f ? FMath::Sign(MoveInput.X) : Facing;
		Facing = SlideDir;
		SlideElapsed = 0.f;
		SlideAirTime = 0.f;
		SlideTravel = 0.f;
		SlideLastX = static_cast<float>(UpdatedComponent->GetComponentLocation().X);
	}
}

// ---------------------------------------------------------------------------- physics overrides

void UPenguinMovementComponent::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	if (!HasValidData() || HasAnimRootMotion() || DeltaTime < MIN_TICK_TIME || IsDashing() || !RunsMovementHere())
	{
		return;
	}

	const bool bAir = IsFalling();
	const float AccelNorm = FMath::Max(GetMaxAcceleration(), 1.f);
	const float In = FMath::Clamp(Acceleration.X / AccelNorm, -1.f, 1.f);
	const bool bHasInput = FMath::Abs(In) > 0.05f;
	float Vx = Velocity.X;

	// The tornado: its user does not move at all (but for a giant's drift); a penguin it sucks in goes where the pull takes it
	// (Z: NewFallVelocity).
	if (HoverLeft > 0.f && !bKnockedOut)
	{
		Velocity = HoverGlideVelocity;
		return;
	}
	if (PullLeft > 0.f && !bKnockedOut)
	{
		Velocity.X = FMath::FInterpTo(Vx, static_cast<float>(GetTornadoPullVelocity().X), DeltaTime, PinguTuning::TornadoPullResponse);
		Velocity.Y = 0.f;
		return;
	}

	if (HasNoControl())
	{
		float Rate;
		if (bStomping || bStompHop)
		{
			Rate = 9000.f;
		}
		else if (FrozenLeft > 0.f)
		{
			Rate = bAir ? FrozenAirDrag : (bOnIce ? FrozenGroundBraking * 0.3f : FrozenGroundBraking);
		}
		else if (StunLeft > 0.f || bKnockedOut)
		{
			Rate = bAir ? KnockbackAirDrag : KnockbackGroundFriction;
			if (!bAir && !bKnockedOut && SlipperyLeft > 0.f)
			{
				Rate *= SlipperyKnockbackFrictionScale; // icy feet: a hit sends you skating
			}
		}
		else
		{
			Rate = bAir ? AirBraking : GroundBraking; // input locked (countdown, round over)
		}
		Velocity.X = FMath::FInterpConstantTo(Vx, 0.f, DeltaTime, Rate);
		Velocity.Y = 0.f;
		return;
	}

	if (bBellySliding)
	{
		CalcBellySlideVelocity(DeltaTime, bAir);
		return;
	}
	if (bAir && WallJumpLockLeft > 0.f)
	{
		// Sticky's wall jump: for a moment the stick can't steer back into the wall (the jump reads as one).
		Velocity.Y = 0.f;
		return;
	}
	if (bAir && bWallClinging)
	{
		// Sticky on a wall: it hangs there, pressed against it (the stick pressing away let go of it: UpdateWallCling).
		Velocity.X = ClingWallDir * PenguinMovementPrivate::StickyClingHug;
		Velocity.Y = 0.f;
		return;
	}

	if (SlipperyLeft > 0.f && !bKnockedOut)
	{
		// Ice gun feet: the stick does nothing (no walking, no steering in the air); the penguin slides on with its momentum and
		// hardly any friction. Jumping, dashing and the belly slide still work. Speed above its running speed (a dash's, a
		// push's) wears off as it does on any floor, so a long slide never outruns what the host's move judge allows.
		const float SlipRunSpeed = GetEffectiveRunSpeed() * SpeedScale;
		if (FMath::Abs(Vx) > SlipRunSpeed)
		{
			Vx = FMath::FInterpConstantTo(Vx, FMath::Sign(Vx) * SlipRunSpeed, DeltaTime, bAir ? AirOverspeedDeceleration : GroundOverspeedDeceleration);
		}
		else if (!bAir)
		{
			Vx = FMath::FInterpConstantTo(Vx, 0.f, DeltaTime, SlipperyGroundFriction);
		}
		Velocity.X = Vx;
		Velocity.Y = 0.f;
		return;
	}

	float Target = In * GetEffectiveRunSpeed() * SpeedScale;
#if !UE_BUILD_SHIPPING
	if (PinguHostAuthority::HasDevCheat(TEXT("speed")) && CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_AutonomousProxy)
	{
		Target *= 6.f; // the net test's cheating joiner: six times its speed (3360 cm/s: faster than any dash)
	}
#endif
	float Rate;
	if (bHasInput)
	{
		if (Vx * In < 0.f)
		{
			Rate = bAir ? AirTurnAcceleration : GroundTurnAcceleration;
		}
		else if (FMath::Abs(Vx) > FMath::Abs(Target))
		{
			Rate = bAir ? AirOverspeedDeceleration : GroundOverspeedDeceleration;
		}
		else
		{
			Rate = bAir ? AirAcceleration : GroundAcceleration;
		}
		if (!bAir && bOnIce)
		{
			Rate *= IceAccelerationScale;
			if (SlipperyLeft > 0.f)
			{
				Rate *= SlipperyAccelerationScale; // frozen feet: far worse than an ice floor (any floor)
			}
		}
		Rate *= bAir ? GetAirControlScale() : GetGripScale(); // the trait's grip (on top of the ice's)
	}
	else
	{
		Rate = bAir ? AirBraking : GroundBraking;
		if (!bAir && bOnIce)
		{
			Rate *= IceBrakingScale;
			if (SlipperyLeft > 0.f)
			{
				Rate *= SlipperyBrakingScale; // let go and you glide on and on
			}
		}
		Rate *= bAir ? GetAirControlScale() : GetGripBrakeScale();
	}
	Vx = FMath::FInterpConstantTo(Vx, Target, DeltaTime, Rate);

	Velocity.X = Vx;
	Velocity.Y = 0.f;
}

float UPenguinMovementComponent::GetGravityZ() const
{
	// Simulated proxies extrapolate with this too: the flags and timers it reads are replicated.
	float G = Super::GetGravityZ();
#if !UE_BUILD_SHIPPING
	if (DevFlyLeft > 0.f && MovementMode == MOVE_Falling && Velocity.Z <= 0.f)
	{
		return 0.f; // the net test's cheating joiner (-PinguCheat=fly): gravity 0 once it no longer rises
	}
#endif
	if (MovementMode == MOVE_Falling)
	{
		if (HoverLeft > 0.f && !bKnockedOut)
		{
			G = 0.f; // the tornado's user hangs in its eye
		}
		else if (PullLeft > 0.f && !bKnockedOut)
		{
			G *= PinguTuning::TornadoPullGravity; // sucked up into a tornado (where the movement runs the pull steers it anyway)
		}
		else if (FrozenLeft > 0.f && !bKnockedOut)
		{
			G = -ABlockIsland::FallingBlockGravity; // an ice cube falls like a block of the island, up and down alike
		}
		else if (bWallClinging && !bKnockedOut)
		{
			G = 0.f; // Sticky hangs on its wall (down held: it slides at a steady speed - NewFallVelocity)
		}
		else if (bStompWindup)
		{
			G = 0.f; // hanging in the air before the slam
		}
		else if (bFlapping && !bFlapCoast)
		{
			G = 0.f; // flapping up at an even speed (UpdateFlap; proxies hold the climb too)
		}
		else if (bStomping)
		{
			G *= 2.5f;
		}
		else if (LowGravityLeft > 0.f)
		{
			G *= 0.12f;
		}
		else if (FloatLeft > 0.f && Velocity.Z < 0.f)
		{
			G *= FloatGravityScale;
		}
		else if (bFastFalling)
		{
			G *= FastFallGravityMultiplier;
		}
		else if (bApexHangAllowed && bJumpHeld && StunLeft <= 0.f && FMath::Abs(Velocity.Z) < ApexSpeedThreshold)
		{
			G *= ApexGravityMultiplier;
		}
		else if (Velocity.Z < 0.f)
		{
			G *= GetEffectiveFallGravityMultiplier();
		}
		// Low / High Gravity (Traits mode: everybody's): all of it, rising and falling (not an ice cube's fall - a falling block's).
		if (FrozenLeft <= 0.f || bKnockedOut)
		{
			G *= GetTraitNumbers().GravityScale;
		}
	}
	return G;
}

FVector UPenguinMovementComponent::NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const
{
	FVector Result = Super::NewFallVelocity(InitialVelocity, Gravity, DeltaTime);
	if (HoverLeft > 0.f && !bKnockedOut)
	{
		Result = HoverGlideVelocity; // perfectly still in the tornado's eye (or drifting into a giant's)
		return Result;
	}
	if (PullLeft > 0.f && !bKnockedOut && RunsMovementHere())
	{
		// Sucked in: up or down toward the tornado's middle (a proxy only falls with the pull's low gravity).
		Result.Z = FMath::FInterpTo(static_cast<float>(InitialVelocity.Z), static_cast<float>(GetTornadoPullVelocity().Z), DeltaTime, PinguTuning::TornadoPullResponse);
		return Result;
	}
	if (bStomping)
	{
		Result.Z = -StompSpeed;
		return Result;
	}
	if (bWallClinging && !bKnockedOut && FrozenLeft <= 0.f)
	{
		// Sticky on a wall: it hangs there; down held, it slides down it slowly (proxies too: the flag and the stick replicate).
		Result.Z = IsWallSliding() ? -PinguTraitTuning::StickyClingSlideSpeed : 0.f;
		return Result;
	}
	float Limit = bFastFalling ? FastFallMaxSpeed : GetEffectiveMaxFallSpeed();
	if (FrozenLeft > 0.f && !bKnockedOut)
	{
		Limit = ABlockIsland::FallingBlockMaxSpeed; // an ice cube: as fast as a falling block
	}
	else if (FloatLeft > 0.f)
	{
		Limit = FMath::Min(Limit, FloatMaxFallSpeed);
	}
	Result.Z = FMath::Max(Result.Z, -Limit);
	return Result;
}

float UPenguinMovementComponent::GetMaxSpeed() const
{
	if (IsDashing())
	{
		return GetEffectiveDashSpeed();
	}
	if (MovementMode == MOVE_Walking || MovementMode == MOVE_NavWalking || MovementMode == MOVE_Falling)
	{
#if !UE_BUILD_SHIPPING
		if (PinguHostAuthority::HasDevCheat(TEXT("speed")) && CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_AutonomousProxy)
		{
			return GetEffectiveRunSpeed() * SpeedScale * 6.f; // (CalcVelocity: the net test's cheating joiner)
		}
#endif
		return GetEffectiveRunSpeed() * SpeedScale;
	}
	return Super::GetMaxSpeed();
}

void UPenguinMovementComponent::ProcessLanded(const FHitResult& Hit, float RemainingTime, int32 Iterations)
{
	if (!RunsMovementHere())
	{
		// Only the owner counts landings (they replicate from there).
		Super::ProcessLanded(Hit, RemainingTime, Iterations);
		return;
	}
	LastLandSpeed = FMath::Max(0.f, -Velocity.Z);
	++LandEvents;
	bFlapping = false;
	const bool bWasStomping = bStomping;
	const bool bWasSticking = bStickNextLanding;
	const float VelocityXBefore = static_cast<float>(Velocity.X);
	if (bStomping)
	{
		bStomping = false;
		LastStompLandSpeed = LastLandSpeed;
		++StompLandEvents;
		if (bHostSimulating)
		{
			HostLastStompLand = PinguKillCam::PacedSeconds(GetWorld()); // the host's own stomp landing of a remote player's penguin: its slam
		}
	}
	if (bHostSimulating)
	{
		HostLandGate.NoteLanded(); // (the owner's report of this landing is not another one)
	}
	LowGravityLeft = 0.f;
	bRisingFromJump = false;
	bJumpCutDone = false;
	bApexHangAllowed = false;
	bFastFalling = false;
	bFastFallArmed = false;
	if (bBellySliding)
	{
		LandBellySlide(); // before the walking physics of the rest of this move
	}
	if (bStickNextLanding)
	{
		bStickNextLanding = false;
		Velocity.X *= PenguinMovementPrivate::StickLandingKeep; // thrown: it comes down with a thud right there
	}
	Super::ProcessLanded(Hit, RemainingTime, Iterations);
	if (IsJumpPad(Hit.GetComponent()) && !bKnockedOut)
	{
		BounceOffJumpPad();
		return;
	}
	TryRubberLandingBounce(VelocityXBefore, bWasStomping, bWasSticking);
}

void UPenguinMovementComponent::HandleImpact(const FHitResult& Hit, float TimeSlice, const FVector& MoveDelta)
{
	Super::HandleImpact(Hit, TimeSlice, MoveDelta);
	HandleRubberWallImpact(Hit);
	using namespace PenguinMovementPrivate;
	// An ice shell is a bubble: pushed into a wall (a gust, a noot, another shell) it bounces off instead of stopping
	// dead. Running into a wall in a shell is just that.
	const float NormalX = static_cast<float>(Hit.ImpactNormal.X);
	if (FloatLeft <= 0.f || bKnockedOut || bShellBouncePending || !RunsMovementHere() || FMath::Abs(NormalX) < ShellBounceWallNormal)
	{
		return;
	}
	const float IntoWall = -static_cast<float>(Velocity.X) * FMath::Sign(NormalX);
	const bool bPushedIntoWall = PushedLeft > 0.f && IntoWall > ShellBounceMinSpeed;
	if (!bPushedIntoWall && IntoWall <= GetEffectiveRunSpeed() * SpeedScale * ShellBounceRunShare)
	{
		return;
	}
	bShellBouncePending = true;
	ShellBounceVelocityX = FMath::Sign(NormalX) * IntoWall * ShellBounceKeep;
}

void UPenguinMovementComponent::HandleRubberWallImpact(const FHitResult& Hit)
{
	using namespace PinguTraitTuning;
	// Rubber: knocked flying (stunned, or faster sideways than RubberFlightSpeed) into a wall, it bounces off it. Bouncy (Traits
	// mode, everybody's) does the same with its own share (FPinguTraitNumbers::FlightWallBounce).
	const float NormalX = static_cast<float>(Hit.ImpactNormal.X);
	const float BouncyKeep = GetTraitNumbers().FlightWallBounce;
	const bool bRubber = HasTraitNow(EPinguTrait::Rubber);
	if (bRubberBouncePending || bShellBouncePending || bKnockedOut || FrozenLeft > 0.f || !RunsMovementHere()
		|| FMath::Abs(NormalX) < PenguinMovementPrivate::ShellBounceWallNormal || (!bRubber && BouncyKeep <= 0.f))
	{
		return;
	}
	const float IntoWall = -static_cast<float>(Velocity.X) * FMath::Sign(NormalX);
	const bool bFlight = StunLeft > 0.f || FMath::Abs(static_cast<float>(Velocity.X)) > RubberFlightSpeed;
	if (!bFlight || IntoWall < RubberMinSpeed)
	{
		return;
	}
	bRubberBouncePending = true;
	RubberBounceVelocityX = FMath::Sign(NormalX) * IntoWall * (bRubber ? RubberWallKeep : BouncyKeep);
}

// ---------------------------------------------------------------------------- combat

void UPenguinMovementComponent::ApplyKnockback(const FVector2D& NewVelocity, float StunTime)
{
	if (bKnockedOut || !CharacterOwner)
	{
		return;
	}
	StopBellySlide();
	bStomping = false;
	bStompHop = false;
	bStompWindup = false;
	bFastFalling = false;
	bFastFallArmed = false;
	bRisingFromJump = false;
	bApexHangAllowed = false;
	LowGravityLeft = 0.f;
	FloatLeft = 0.f;
	HoverLeft = 0.f; // knocked out of a tornado's eye, or thrown out of one
	PullLeft = 0.f;
	bWallClinging = false; // knocked off its wall (Sticky: no cling while stunned)
	EndPassThrough(true);  // a hit on the way up through the island: the blocks count again (out of one it was in)

	bStickNextLanding = false; // a new flight (StickNextLanding may follow for this one)
	FVector2D V = NewVelocity;
	if ((IsMovingOnGround() || IsDashing()) && V.Y < 250.f && V.Size() > 400.f)
	{
		V.Y = 250.f; // pop off the ground so the hit reads
	}
	Velocity = FVector(V.X, 0.f, V.Y);
	if (IsDashing() || V.Y > 1.f || !IsMovingOnGround())
	{
		SetMovementMode(MOVE_Falling);
	}
	StunLeft = FMath::Max(StunLeft, StunTime);
	// You keep your double jump and air dash to fight your way back.
	AirJumpsLeft = GetMaxAirJumpsNow();
	AirDashesLeft = MaxAirDashes;
	if (FMath::Abs(V.X) > 50.f)
	{
		Facing = -FMath::Sign(V.X); // flung backwards, looking at whoever hit you
	}
	LastHitSpeed = V.Size();
	++HitEvents;

	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::Knockback;
		Effect.Vector = FVector(NewVelocity.X, 0.f, NewVelocity.Y);
		Effect.Value = StunTime;
		ForwardEffect(Effect, true);
	}
}

void UPenguinMovementComponent::StickNextLanding()
{
	if (bKnockedOut || !CharacterOwner)
	{
		return;
	}
	bStickNextLanding = true;
	if (IsRemotePlayerCopy())
	{
		// The owner runs the flight and the landing (after the knockback just sent: the effects arrive in order).
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::StickLanding;
		ForwardEffect(Effect, false); // nothing changes until the landing (the knockback before it is the hard part)
	}
}

void UPenguinMovementComponent::SetFrozen(float Seconds)
{
	if (bKnockedOut)
	{
		return;
	}
	FrozenLeft = FMath::Max(0.f, Seconds);
	if (FrozenLeft > 0.f)
	{
		StopBellySlide(); // an ice cube now (it keeps sliding, as one)
		HoverLeft = 0.f;  // a frozen tornado user drops out of its eye (a frozen penguin sucked in stays sucked in: an ice cube)
		bStomping = false;
		bStompHop = false;
		bStompWindup = false;
		bFastFalling = false;
		bFastFallArmed = false;
		if (IsDashing())
		{
			Velocity *= 0.3f;
			SetMovementMode(MOVE_Falling);
		}
	}

	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::Frozen;
		Effect.Value = Seconds;
		ForwardEffect(Effect, FrozenLeft > 0.f); // freezing stops a stomp or a dash; a thaw changes nothing the owner simulates
	}
}

bool UPenguinMovementComponent::StartStomp()
{
	// The same rule as every ability (CanUseAbilities): an ordinary hit stun does not stop it - a stomp in the knockback
	// flight takes over from it. On a LAN player's own machine (the server's StartStomp effect) the server has checked already.
	if (!CharacterOwner || !CanUseAbilities())
	{
		return false;
	}
	if (IsDashing())
	{
		SetMovementMode(MOVE_Falling);
	}
	StopBellySlide();
	bFastFalling = false;
	bFastFallArmed = false;
	bRisingFromJump = false;
	bApexHangAllowed = false;
	JumpBufferLeft = 0.f;
	PullLeft = 0.f; // a stomp breaks out of a tornado's pull
	if (IsMovingOnGround())
	{
		bStompHop = true;
		StompHopTime = 0.f;
		bCoyoteAvailable = false;
		Velocity = FVector(Velocity.X * 0.3f, 0.f, StompHopSpeed);
		SetMovementMode(MOVE_Falling);
	}
	else
	{
		if (IsClimbing())
		{
			SetMovementMode(MOVE_Falling);
		}
		BeginStompWindup(StompWindupTime);
		Velocity = FVector(Velocity.X * 0.3f, 0.f, 160.f); // a little hitch up before the hang
	}

	// Started by an ability on the server; a remote player's own machine simulates the hop, the slam and the landing.
	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::StartStomp;
		ForwardEffect(Effect, true);
	}
	return true;
}

float UPenguinMovementComponent::GetJumpGravity() const
{
	const UWorld* World = GetWorld();
	const float WorldGravity = World ? FMath::Abs(World->GetGravityZ()) : PenguinMovementPrivate::DefaultWorldGravity;
	return FMath::Max(WorldGravity * GravityScale * GetTraitNumbers().GravityScale, 100.f); // (Low / High Gravity: the trait's too)
}

bool UPenguinMovementComponent::StartFlap(bool bFromServer)
{
	// The rule of every ability (CanUseAbilities: not frozen, out, locked, dizzy, stomping, hanging in a tornado's eye or
	// swallowed), and not stunned or sucked in by a tornado either (no beating flippers out of those).
	if (!CharacterOwner || !UpdatedComponent || (!bFromServer && (!CanUseAbilities() || StunLeft > 0.f || PullLeft > 0.f)))
	{
		return false;
	}
	using namespace PinguTuning;
	const bool bGround = IsGroundedForAbilities();
	StopBellySlide();
	bFastFalling = false;
	bFastFallArmed = false;
	bRisingFromJump = false;
	bJumpCutDone = true;
	bApexHangAllowed = false;
	bCoyoteAvailable = false;
	bWallClinging = false;
	LowGravityLeft = 0.f;
	// As a jump: the size and the trait's jump scale the speed, their square the height.
	const float Jump = JumpScale * GetTraitJumpScale();
	const float G = GetJumpGravity();
	const float PadHeight = FMath::Square(JumpPadSpeed) / (2.f * G);
	LastFlapHeight = PadHeight * (bGround ? FlapGroundHeightShare : FlapFallHeightShare) * Jump * Jump;
	bLastFlapFromGround = bGround;
	FlapSpeed = FMath::Max(JumpPadSpeed * FlapSpeedShare * Jump, 100.f);
	FlapTopZ = static_cast<float>(UpdatedComponent->GetComponentLocation().Z) + LastFlapHeight;
	FlapLeft = LastFlapHeight / FlapSpeed + FlapExtraTime;
	bFlapping = true;
	bFlapCoast = false;
	Velocity.Z = FlapSpeed; // a fall is caught at once
	Velocity.Y = 0.f;
	SetMovementMode(MOVE_Falling); // (also out of a dash or a ledge climb)

	if (IsRemotePlayerCopy())
	{
		// The owner flies it; the host's judge allows this one climb (a ballistic arc a little above its top).
		HostJudge.NoteHostPush(PinguKillCam::PacedSeconds(GetWorld()), UpdatedComponent->GetComponentLocation(),
			FVector(0.f, 0.f, FMath::Sqrt(2.f * G * (LastFlapHeight * 1.15f + 50.f))), true, false, GetHostMoveLimits());
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::StartFlap;
		ForwardEffect(Effect, true);
	}
	return true;
}

void UPenguinMovementComponent::UpdateFlap(float DeltaSeconds)
{
	FlapLeft -= DeltaSeconds;
	if (HasNoControl() || FlapLeft <= 0.f || !IsFalling() || !UpdatedComponent)
	{
		bFlapping = false;
		return;
	}
	if (bFlapCoast)
	{
		bFlapping = Velocity.Z > 0.f; // the coast to the top: over when it no longer rises
		return;
	}
	// Climb at the flap's speed; the last bit (once that speed would carry it there under gravity) it coasts up to the top.
	const float Remaining = FlapTopZ - static_cast<float>(UpdatedComponent->GetComponentLocation().Z);
	const float Coast = FMath::Sqrt(2.f * GetJumpGravity() * FMath::Max(Remaining, 0.f));
	if (Coast <= FlapSpeed)
	{
		bFlapCoast = true;
		Velocity.Z = Coast;
		return;
	}
	Velocity.Z = FlapSpeed;
}

void UPenguinMovementComponent::BeginStompWindup(float Seconds)
{
	bStompWindup = true;
	StompWindupLeft = FMath::Max(Seconds, 0.01f);
	if (MovementMode != MOVE_Falling)
	{
		SetMovementMode(MOVE_Falling);
	}
}

void UPenguinMovementComponent::LaunchPlanar(const FVector2D& NewVelocity, float LowGravityTime)
{
	if (bKnockedOut || !CharacterOwner)
	{
		return;
	}
	StopBellySlide();
	bStomping = false;
	bStompHop = false;
	bStompWindup = false;
	bFastFalling = false;
	bFastFallArmed = false;
	bRisingFromJump = false;
	bApexHangAllowed = false;
	bCoyoteAvailable = false;
	HoverLeft = 0.f; // a hook's pull takes us out of a tornado (its eye, or its suck)
	PullLeft = 0.f;
	Velocity = FVector(NewVelocity.X, 0.f, NewVelocity.Y);
	SetMovementMode(MOVE_Falling);
	LowGravityLeft = FMath::Max(LowGravityLeft, LowGravityTime);
	if (FMath::Abs(NewVelocity.X) > 50.f)
	{
		Facing = FMath::Sign(NewVelocity.X);
	}

	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::LaunchPlanar;
		Effect.Vector = FVector(NewVelocity.X, 0.f, NewVelocity.Y);
		Effect.Value = LowGravityTime;
		ForwardEffect(Effect, true);
	}
}

void UPenguinMovementComponent::SetSizeScale(float Scale)
{
	// The sizes a penguin can have (PinguMinSize .. PinguMaxSize), with a little room.
	const float S = FMath::Clamp(Scale, 0.3f, PinguMaxSize * 1.05f);
	SpeedScale = FMath::Pow(S, PinguTuning::SizeSpeedPower); // 2x size -> 1.23x speed, 8x -> 1.87x
	JumpScale = FMath::Pow(S, PinguTuning::SizeJumpPower);   // 2x size -> 1.19x jump speed (1.41x height), 8x -> 1.68x (2.83x height)
	if (CharacterOwner && CharacterOwner->HasAuthority())
	{
		NetSizeScale = S; // every machine, the owner too (a replicated value instead of an RPC per frame while the size animates)
	}
}

void UPenguinMovementComponent::AddAbilityImpulse(const FVector2D& Impulse, float LowGravityTime, bool bCancelFall)
{
	if (bKnockedOut || FrozenLeft > 0.f || !CharacterOwner || !UpdatedComponent)
	{
		return;
	}
	const FVector VelocityBefore = Velocity;
	const float Side = static_cast<float>(Impulse.X);
	float Up = static_cast<float>(Impulse.Y);
	if (FMath::Abs(Side) >= PenguinMovementPrivate::ShellPushMinSide)
	{
		PushedLeft = PenguinMovementPrivate::ShellPushMemory; // an ice shell's bubble pushed into a wall now bounces off it
	}
	// A small upward part while standing (a slap aimed a little up, the edge of a bomb blast) only scoots you along the
	// ground: a 1 cm hop would count as airborne, and the next jump press would come out as the double jump.
	if (bCancelFall && Up > 0.f && Up < PenguinMovementPrivate::MinGroundLiftSpeed && IsGroundedForAbilities())
	{
		Up = 0.f;
	}
	if (IsDashing())
	{
		EndDash(Up > 1.f); // a sideways push lands back on the ground instead of a fake one-frame fall
	}
	if (Up > 1.f)
	{
		StopBellySlide(); // launched off the belly; a sideways push (recoil) just adds to the slide
	}
	FVector NewVelocity = Velocity;
	NewVelocity.X += Side;
	NewVelocity.Y = 0.f;
	if (Up > 0.f)
	{
		// Small upward parts (a mostly sideways push) don't stop a fall; real launches do.
		const bool bCancel = bCancelFall && Up > 150.f;
		const float CurrentZ = static_cast<float>(NewVelocity.Z);
		const float Base = bCancel ? FMath::Max(CurrentZ, 0.f) : CurrentZ;
		NewVelocity.Z = FMath::Min(Base + Up, FMath::Max(MaxAbilityLaunchSpeed, Up));
	}
	else
	{
		NewVelocity.Z += Up;
	}
	Velocity = NewVelocity;
	if (Up > 1.f)
	{
		bCoyoteAvailable = false; // a jump right after the launch must not replace it with a ground jump
		if (MovementMode != MOVE_Falling)
		{
			SetMovementMode(MOVE_Falling);
		}
	}
	// An ability launch is not a jump: letting go of jump must not cut it short.
	bRisingFromJump = false;
	bApexHangAllowed = false;
	bFastFalling = false;
	bFastFallArmed = false;
	LowGravityLeft = FMath::Max(LowGravityLeft, LowGravityTime);

	if (IsRemotePlayerCopy())
	{
		// The owner applies it to its own velocity. Until then the server's estimate is the owner's last velocity plus this push
		// (the frost jet adds one every frame, based on the velocity it sees here).
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::AbilityImpulse;
		Effect.Vector = FVector(Impulse.X, 0.f, Impulse.Y);
		Effect.Value = LowGravityTime;
		Effect.bFlag = bCancelFall;
		ForwardEffect(Effect, false, Velocity - VelocityBefore);
	}
}

void UPenguinMovementComponent::NudgeVelocity(const FVector2D& Delta)
{
	const FVector Push(Delta.X, 0.f, Delta.Y);
	Velocity += Push;
	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::Nudge;
		Effect.Vector = Push;
		ForwardEffect(Effect, false, Push);
	}
}

void UPenguinMovementComponent::SetKnockedOut(bool bKO)
{
	bKnockedOut = bKO;
	if (bKO)
	{
		StopBellySlide();
		bStomping = false;
		bStompHop = false;
		bStompWindup = false;
		FrozenLeft = 0.f;
		StunLeft = 0.f;
		SlapDizzyLeft = 0.f;
		LowGravityLeft = 0.f;
		FloatLeft = 0.f;
		SlipperyLeft = 0.f;
		HoverLeft = 0.f;
		PullLeft = 0.f;
		bWallClinging = false;
		EndPassThrough(true);
		if (IsDashing())
		{
			SetMovementMode(MOVE_Falling);
		}
		// Out: the host no longer judges or moves a remote player's penguin (its next life starts afresh).
		bHostPenalty = false;
		bHandbackPending = false;
		bHostQuiet = false;
		HostJudge.Reset();
	}

	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::KnockedOut;
		Effect.bFlag = bKO;
		ForwardEffect(Effect, bKO);
	}
}

void UPenguinMovementComponent::SetInputLocked(bool bLocked)
{
	if (bInputLocked == bLocked)
	{
		return;
	}
	bInputLocked = bLocked;
	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::InputLocked;
		Effect.bFlag = bLocked;
		ForwardEffect(Effect, false);
	}
}

void UPenguinMovementComponent::RefreshAirMoves()
{
	AirJumpsLeft = GetMaxAirJumpsNow();
	AirDashesLeft = MaxAirDashes;
	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::RefreshAirMoves;
		ForwardEffect(Effect, false);
	}
}

void UPenguinMovementComponent::SetFloat(float Seconds)
{
	FloatLeft = FMath::Max(0.f, Seconds);
	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::Float;
		Effect.Value = Seconds;
		ForwardEffect(Effect, false);
	}
}

void UPenguinMovementComponent::SetHover(float Seconds)
{
	if (bKnockedOut || !CharacterOwner)
	{
		return;
	}
	if (HoverLeft <= 0.f)
	{
		HoverGlideLeft = 0.f; // a new hover starts still (a longer one goes on drifting where it drifted)
	}
	HoverLeft = FMath::Max(0.f, Seconds);
	if (HoverLeft > 0.f)
	{
		// Everything that moved us stops: we hang right here (GetGravityZ, CalcVelocity, NewFallVelocity keep it so).
		StopBellySlide();
		bStomping = false;
		bStompHop = false;
		bStompWindup = false;
		bFastFalling = false;
		bFastFallArmed = false;
		bRisingFromJump = false;
		bApexHangAllowed = false;
		JumpBufferLeft = 0.f;
		LowGravityLeft = 0.f;
		PullLeft = 0.f;
		if (IsDashing() || IsClimbing())
		{
			SetMovementMode(MOVE_Falling);
		}
		Velocity = FVector::ZeroVector;
	}
	if (IsRemotePlayerCopy())
	{
		// The owner hangs still itself (its simulation owns the position); until it does, its older velocity is out of date.
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::Hover;
		Effect.Value = Seconds;
		ForwardEffect(Effect, HoverLeft > 0.f);
	}
}

void UPenguinMovementComponent::SetHoverGlide(const FVector& To, float Seconds)
{
	if (bKnockedOut || !CharacterOwner || HoverLeft <= 0.f)
	{
		return;
	}
	HoverGlideTo = FVector(To.X, 0.f, To.Z);
	HoverGlideLeft = FMath::Max(0.f, Seconds);
	// Up off the ground when its place is higher (a hover in the air has no gravity: it drifts straight there).
	if (HoverGlideLeft > 0.f && IsMovingOnGround() && UpdatedComponent && HoverGlideTo.Z > UpdatedComponent->GetComponentLocation().Z + 5.f)
	{
		SetMovementMode(MOVE_Falling);
	}
	if (IsRemotePlayerCopy())
	{
		// The owner drifts itself (its simulation owns the position: its moves bring the copy along).
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::HoverGlide;
		Effect.Location = HoverGlideTo;
		Effect.Value = Seconds;
		ForwardEffect(Effect, true);
	}
}

void UPenguinMovementComponent::SetTornadoPull(const FVector& Center, float Seconds, float Radius, float EyeRadius)
{
	if (bKnockedOut || !CharacterOwner)
	{
		return;
	}
	PullLeft = FMath::Max(0.f, Seconds);
	PullCenter = FVector(Center.X, 0.f, Center.Z);
	PullRadius = FMath::Max(Radius, 1.f);
	PullEye = FMath::Max(EyeRadius, 0.f);
	if (PullLeft > 0.f)
	{
		// Out of whatever we were doing: the wind has us now (abilities still work).
		StopBellySlide();
		bStomping = false;
		bStompHop = false;
		bStompWindup = false;
		bFastFalling = false;
		bFastFallArmed = false;
		bRisingFromJump = false;
		bApexHangAllowed = false;
		if (IsDashing() || IsClimbing())
		{
			SetMovementMode(MOVE_Falling);
		}
	}
	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::TornadoPull;
		Effect.Location = PullCenter;
		Effect.Value = Seconds;
		Effect.Vector = FVector(PullRadius, 0.f, PullEye);
		ForwardEffect(Effect, PullLeft > 0.f);
	}
}

FVector UPenguinMovementComponent::GetTornadoPullVelocity() const
{
	using namespace PinguTuning;
	if (PullLeft <= 0.f || !UpdatedComponent)
	{
		return FVector::ZeroVector;
	}
	FVector To = PullCenter - UpdatedComponent->GetComponentLocation();
	To.Y = 0.f;
	const float Dist = static_cast<float>(To.Size());
	// Faster the nearer the middle; it slows down into the eye (round the tornado's user, never into it).
	const float Near = 1.f - FMath::Clamp(Dist / PullRadius, 0.f, 1.f);
	const float Room = Dist - PullEye;
	constexpr float ArriveRate = 7.f; // 1/s: the last part of the way to the eye
	const float Speed = FMath::Min(FMath::Lerp(TornadoPullEdgeSpeed, TornadoPullNearSpeed, Near), FMath::Max(Room, 0.f) * ArriveRate);
	FVector Out = Dist > 1.f ? To / Dist * Speed : FVector::ZeroVector;
	// Up into the funnel a little (less and less near the eye).
	Out.Z += TornadoPullLift * FMath::Clamp(Room / 200.f, 0.f, 1.f);
	return Out;
}

void UPenguinMovementComponent::SetSlippery(float Seconds)
{
	SlipperyLeft = FMath::Max(SlipperyLeft, Seconds);
	if (IsRemotePlayerCopy())
	{
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::Slippery;
		Effect.Value = Seconds;
		ForwardEffect(Effect, false);
	}
}

void UPenguinMovementComponent::NetTeleportPenguin(const FVector& Location, const FVector& NewVelocity, bool bForceFalling, bool bKeepSlide)
{
	if (!CharacterOwner || !UpdatedComponent)
	{
		return;
	}
	const FVector From = UpdatedComponent->GetComponentLocation();
	if (!bKeepSlide)
	{
		StopBellySlide(); // moved somewhere else (a warp, lifted out of new ice): no slide goes on from there
	}
	CharacterOwner->SetActorLocation(ConstrainLocationToPlane(Location), false, nullptr, ETeleportType::TeleportPhysics);
	if (bBellySliding)
	{
		SlideLastX = static_cast<float>(UpdatedComponent->GetComponentLocation().X); // the jump is no distance slid
	}
	bRubberFallTracked = false; // (Rubber: a fall starts here, not where it was)
	Velocity = FVector(NewVelocity.X, 0.f, NewVelocity.Z);
	if (bForceFalling && MovementMode != MOVE_Falling)
	{
		SetMovementMode(MOVE_Falling); // also ends a dash or a climb
	}

	if (IsRemotePlayerCopy())
	{
		// The owner moves itself there; its moves from before it got this are ignored until it confirms.
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::Teleport;
		Effect.Location = Location;
		Effect.Vector = NewVelocity;
		Effect.bFlag = bForceFalling;
		Effect.Value = bKeepSlide ? 1.f : 0.f;
		ForwardEffect(Effect, true);
		ArmNetPositionGuard(From, UpdatedComponent->GetComponentLocation(), true, Effect.Seq);
	}
}

void UPenguinMovementComponent::FaceDirection(float DirectionX)
{
	// Not while dashing or sliding: the penguin keeps looking the way it goes.
	if (FMath::Abs(DirectionX) > 0.05f && !IsDashing() && !bBellySliding)
	{
		Facing = FMath::Sign(DirectionX);
	}
}

// ---------------------------------------------------------------------------- networking: roles

bool UPenguinMovementComponent::IsRemotePlayerCopy() const
{
	// The server's copy of a penguin possessed by a remote player (the host's own penguin is locally controlled).
	return CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_Authority && CharacterOwner->GetRemoteRole() == ROLE_AutonomousProxy
		&& CharacterOwner->GetController() != nullptr && !CharacterOwner->IsLocallyControlled();
}

bool UPenguinMovementComponent::RunsMovementHere() const
{
	// (The host simulating its copy of a remote player's penguin, HostSimulate, runs the movement here for that frame.)
	return !CharacterOwner || bHostSimulating || (CharacterOwner->GetLocalRole() != ROLE_SimulatedProxy && !IsRemotePlayerCopy());
}

bool UPenguinMovementComponent::GetNetServerTime(float& OutTime) const
{
	OutTime = 0.f;
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	if (const AGameStateBase* GameState = World->GetGameState())
	{
		OutTime = static_cast<float>(GameState->GetServerWorldTimeSeconds());
		return true;
	}
	// Without a game state a client cannot know the server's clock yet (the server's own clock is its world time).
	OutTime = static_cast<float>(World->GetTimeSeconds());
	return !CharacterOwner || CharacterOwner->HasAuthority();
}

void UPenguinMovementComponent::UpdateNetTickOrder()
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !PrimaryComponentTick.bCanEverTick || !OwnerActor->PrimaryActorTick.bCanEverTick)
	{
		return;
	}
	const bool bAfterOwner = !IsNetMode(NM_Standalone) && !RunsMovementHere();
	if (bAfterOwner)
	{
		// The character sets the mesh location and yaw in its Tick. On a penguin we only mirror, the engine's network smoothing
		// of the mesh location (SimulatedTick / the listen server's SmoothClientPosition) has to come after that, or it is lost.
		// Asserted every tick (cheap): a re-registration of our tick functions would add the engine's opposite order again.
		bNetTickAfterOwner = true;
		OwnerActor->PrimaryActorTick.RemovePrerequisite(this, PrimaryComponentTick);
		PrimaryComponentTick.AddPrerequisite(OwnerActor, OwnerActor->PrimaryActorTick);
		return;
	}
	if (!bNetTickAfterOwner)
	{
		return;
	}
	// Simulated here (again): move first, then the character reads the result (the engine's normal order, bTickBeforeOwner).
	bNetTickAfterOwner = false;
	PrimaryComponentTick.RemovePrerequisite(OwnerActor, OwnerActor->PrimaryActorTick);
	if (bTickBeforeOwner)
	{
		OwnerActor->PrimaryActorTick.AddPrerequisite(this, PrimaryComponentTick);
	}
}

// ---------------------------------------------------------------------------- networking: tick

void UPenguinMovementComponent::BeginPlay()
{
	Super::BeginPlay();
	FindTraitComponent();
	UpdateNetTickOrder();
}

// ---------------------------------------------------------------------------- the numbers in effect (Traits mode)

void UPenguinMovementComponent::FindTraitComponent()
{
	if (!TraitComponent)
	{
		const APenguinCharacter* Penguin = Cast<APenguinCharacter>(GetOwner());
		TraitComponent = Penguin ? Penguin->GetTraits() : nullptr;
	}
}

const FPinguTraitNumbers& UPenguinMovementComponent::GetTraitNumbers() const
{
	const UPenguinTraitComponent* Traits = TraitComponent.Get();
	return Traits ? Traits->GetNumbers() : PinguTraits::GetNeutralNumbers();
}

int32 UPenguinMovementComponent::GetMaxAirJumpsNow() const
{
	const int32 TraitAirJumps = GetTraitNumbers().AirJumps;
	return TraitAirJumps >= 0 ? TraitAirJumps : MaxAirJumps;
}

float UPenguinMovementComponent::GetEffectiveRunSpeed() const
{
	return RunSpeed * GetTraitNumbers().RunScale;
}

float UPenguinMovementComponent::GetEffectiveDashSpeed() const
{
	return DashSpeed * GetTraitNumbers().DashScale;
}

float UPenguinMovementComponent::GetEffectiveDashExitSpeed() const
{
	return DashExitSpeed * GetTraitNumbers().DashScale;
}

float UPenguinMovementComponent::GetGripScale() const
{
	return GetTraitNumbers().GripAccelScale;
}

float UPenguinMovementComponent::GetGripBrakeScale() const
{
	return GetTraitNumbers().GripBrakeScale;
}

float UPenguinMovementComponent::GetAirControlScale() const
{
	return GetTraitNumbers().AirControlScale;
}

float UPenguinMovementComponent::GetTraitJumpScale() const
{
	return GetTraitNumbers().JumpScale;
}

float UPenguinMovementComponent::GetEffectiveJumpHeight() const
{
	// The jump speed scales (the size's and the trait's) with the same rising gravity: the height goes with their square.
	return JumpHeight * FMath::Square(JumpScale * GetTraitJumpScale());
}

float UPenguinMovementComponent::GetEffectiveFallGravityMultiplier() const
{
	return FallGravityMultiplier * GetTraitNumbers().FallGravityScale;
}

float UPenguinMovementComponent::GetEffectiveMaxFallSpeed() const
{
	const float TraitLimit = GetTraitNumbers().MaxFallSpeed;
	return TraitLimit > 0.f ? TraitLimit : MaxFallSpeed;
}

float UPenguinMovementComponent::GetEffectiveClimbReachUp() const
{
	return ClimbReachUp * GetTraitNumbers().ClimbUpScale;
}

float UPenguinMovementComponent::GetEffectiveClimbReachForward() const
{
	return ClimbReachForward * GetTraitNumbers().ClimbForwardScale;
}

bool UPenguinMovementComponent::CanBellySlideNow() const
{
	return GetTraitNumbers().bCanBellySlide;
}

bool UPenguinMovementComponent::CanGrabLedge() const
{
	return GetTraitNumbers().bCanGrabLedge;
}

bool UPenguinMovementComponent::HasTraitNow(EPinguTrait Trait) const
{
	const UPenguinTraitComponent* Traits = TraitComponent.Get();
	return Traits && Traits->HasTrait(Trait);
}

// ---------------------------------------------------------------------------- Sticky: wall cling and wall jump

bool UPenguinMovementComponent::IsWallBeside(float Dir, float Reach, const FVector* At) const
{
	UWorld* World = GetWorld();
	const UCapsuleComponent* Capsule = CharacterOwner ? CharacterOwner->GetCapsuleComponent() : nullptr;
	if (!World || !Capsule || !UpdatedComponent || FMath::Abs(Dir) < 0.5f)
	{
		return false;
	}
	// The ledge grab's wall test: a line out of the side of the body at the knees, the middle and the chest.
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const FVector Loc = At ? *At : UpdatedComponent->GetComponentLocation();
	const ECollisionChannel Channel = UpdatedComponent->GetCollisionObjectType();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguWallCling), false, CharacterOwner);
	for (const float Up : { -0.45f, 0.f, 0.6f })
	{
		FHitResult WallHit;
		const FVector Start(Loc.X, 0.f, Loc.Z + Up * HalfHeight);
		const FVector End = Start + FVector(Dir * (Radius + Reach), 0.f, 0.f);
		if (World->LineTraceSingleByChannel(WallHit, Start, End, Channel, Params) && FMath::Abs(WallHit.ImpactNormal.X) > 0.7f
			&& WallHit.ImpactNormal.X * Dir < 0.f && !Cast<APawn>(WallHit.GetActor()))
		{
			return true;
		}
	}
	return false;
}

bool UPenguinMovementComponent::FindStickyWall(float& OutWallDir) const
{
	using namespace PenguinMovementPrivate;
	OutWallDir = 0.f;
	if (!HasTraitNow(EPinguTrait::Sticky))
	{
		return false;
	}
	const float StickDir = FMath::Abs(MoveInput.X) > 0.3f ? FMath::Sign(MoveInput.X) : 0.f;
	for (const float Dir : { bWallClinging ? ClingWallDir : 0.f, StickDir, Facing, -Facing })
	{
		if (Dir != 0.f && IsWallBeside(Dir, StickyWallReach))
		{
			OutWallDir = Dir;
			return true;
		}
	}
	return false;
}

bool UPenguinMovementComponent::IsWallSliding() const
{
	return bWallClinging && MoveInput.Y < PenguinMovementPrivate::DownThreshold;
}

void UPenguinMovementComponent::UpdateWallCling(float DeltaSeconds, bool bControl)
{
	using namespace PenguinMovementPrivate;
	const bool bWasClinging = bWallClinging;
	bWallClinging = false;
	if (!HasTraitNow(EPinguTrait::Sticky) || IsMovingOnGround())
	{
		ClingWallDir = 0.f;
		return;
	}
	// Any wall it touches in the air holds it: on the way down (a jump rises on past a wall and sticks at its top), not in a
	// stun, a freeze, a tornado or an input lock (no control), a stomp, a fast fall, a dash, a slide, a climb, or right after
	// a wall jump. No slide and no time limit: it hangs there (CalcVelocity, NewFallVelocity) until the stick presses away
	// from the wall - then it lets go (a wall on the other side may still take it). Down held: it slides down slowly. Up does
	// nothing; a jump is the wall jump (or the climb over a ledge in reach: TryWallJump).
	if (!bControl || !IsFalling() || bFastFalling || bBellySliding || IsStomping() || WallJumpLockLeft > 0.f
		|| (Velocity.Z > 0.f && !bWasClinging))
	{
		return;
	}
	const float Stick = FMath::Abs(MoveInput.X) > StickyLetGoInput ? FMath::Sign(MoveInput.X) : 0.f;
	float Dir = 0.f;
	for (const float Side : { bWasClinging ? ClingWallDir : 0.f, Stick, Facing, -Facing })
	{
		if (Side != 0.f && Side != -Stick && IsWallBeside(Side, StickyWallReach))
		{
			Dir = Side;
			break;
		}
	}
	if (Dir == 0.f)
	{
		return;
	}
	ClingWallDir = Dir;
	bWallClinging = true;
	Facing = Dir; // it looks at the wall it hangs on (a jump grabs a ledge above it that way)
	bRisingFromJump = false;
	bApexHangAllowed = false;
	Velocity.Z = IsWallSliding() ? -PinguTraitTuning::StickyClingSlideSpeed : 0.f;
}

bool UPenguinMovementComponent::TryWallJump()
{
	float WallDir = 0.f;
	if (!IsFalling() || !FindStickyWall(WallDir))
	{
		return false;
	}
	// A ledge in reach (Sticky reaches much further up): pull up over it.
	if (TryStartClimb())
	{
		++JumpStats.Climbs;
		return true;
	}
	// Off the wall: a full jump away from it, and the double jump back.
	LaunchJump(false);
	Velocity.X = -WallDir * PinguTraitTuning::StickyWallJumpSide;
	Facing = -WallDir;
	AirJumpsLeft = GetMaxAirJumpsNow();
	bWallClinging = false;
	ClingWallDir = 0.f;
	WallJumpLockLeft = PenguinMovementPrivate::WallJumpSteerLock;
	++WallJumpEvents;
	++JumpStats.AirJumps;
	return true;
}

// ---------------------------------------------------------------------------- Rubber: bounces

bool UPenguinMovementComponent::TryRubberLandingBounce(float VelocityXBefore, bool bWasStomping, bool bWasSticking)
{
	using namespace PinguTraitTuning;
	// Rubber bounces off the ground after every fall: back up to RubberBounceHeight of the height it fell from (the top of
	// the fall to here - whatever the falling gravity and the top falling speed made of the way down; the bounce rises with
	// the plain gravity of a jump), as long as that bounce leaves at RubberMinSpeed or more - a smaller one settles. Not a
	// stomp's slam, a fish hook's throw, a belly-flop dive, an ice cube; and not when a jump was pressed for this landing
	// (it jumps).
	const bool bTracked = bRubberFallTracked;
	bRubberFallTracked = false;
	if (!HasTraitNow(EPinguTrait::Rubber) || bKnockedOut || bWasStomping || bWasSticking || bBellySliding || FrozenLeft > 0.f
		|| !bTracked || !UpdatedComponent || (JumpBufferLeft > 0.f && !HasNoControl()))
	{
		return false;
	}
	const float LandZ = static_cast<float>(UpdatedComponent->GetComponentLocation().Z);
	const float FallHeight = FMath::Max(RubberFallTopZ - LandZ, 0.f);
	const float RiseGravity = FMath::Abs(Super::GetGravityZ());
	const float Up = FMath::Sqrt(2.f * RiseGravity * RubberBounceHeight * FallHeight);
	if (Up < RubberMinSpeed)
	{
		return false;
	}
	LastBounceFallHeight = FallHeight;
	LastBounceUpSpeed = Up;
	Velocity.X = VelocityXBefore;
	Velocity.Y = 0.f;
	Velocity.Z = Up;
	SetMovementMode(MOVE_Falling);
	// The next fall's top is the top of this bounce.
	RubberFallTopZ = LandZ;
	bRubberFallTracked = true;
	// Like leaving the ground: the air moves are back, and a jump right after the bounce is a ground jump (coyote time).
	AirJumpsLeft = GetMaxAirJumpsNow();
	AirDashesLeft = MaxAirDashes;
	bCoyoteAvailable = true;
	TimeSinceGrounded = 0.f;
	bRisingFromJump = false;
	bJumpCutDone = true;
	bApexHangAllowed = false;
	++BounceEvents;
	return true;
}

// ---------------------------------------------------------------------------- Second Chance: through the island on the way up

void UPenguinMovementComponent::SetPassThroughIsland(float AboveFeetZ, float MaxSeconds)
{
	if (!CharacterOwner || !UpdatedComponent)
	{
		return;
	}
	if (MaxSeconds > 0.f && !bKnockedOut)
	{
		bPassThroughIsland = true;
		PassThroughAboveZ = AboveFeetZ;
		PassThroughLeft = MaxSeconds;
		ApplyPassThroughIgnore(true);
	}
	else
	{
		EndPassThrough(RunsMovementHere());
	}
	if (IsRemotePlayerCopy())
	{
		HostPassThroughLeft = MaxSeconds > 0.f ? MaxSeconds : 0.f; // (its height is not judged meanwhile: IsHostFreeVertical)
		// The owner moves its penguin: it passes through there (it comes after the throw's knockback and teleport).
		FPenguinNetEffect Effect;
		Effect.Kind = EPenguinNetEffect::PassThroughIsland;
		Effect.Location = FVector(0.f, 0.f, AboveFeetZ);
		Effect.Value = MaxSeconds;
		ForwardEffect(Effect, false);
	}
}

void UPenguinMovementComponent::UpdatePassThrough(float DeltaSeconds)
{
	if (!bPassThroughIsland || !CharacterOwner || !UpdatedComponent)
	{
		return;
	}
	PassThroughLeft -= DeltaSeconds;
	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	const float Feet = static_cast<float>(UpdatedComponent->GetComponentLocation().Z) - (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.f);
	// Over the highest block: the blocks count again (nothing can be in the way up there). It stopped rising before that, or it
	// took too long: they count again too, and out of one it is in.
	if (bKnockedOut || Feet > PassThroughAboveZ || Velocity.Z <= 0.f || PassThroughLeft <= 0.f)
	{
		EndPassThrough(true);
	}
}

void UPenguinMovementComponent::EndPassThrough(bool bNudge)
{
	using namespace PenguinMovementPrivate;
	const bool bWasPassing = bPassThroughIsland || bPassThroughIgnoring;
	bPassThroughIsland = false;
	PassThroughLeft = 0.f;
	ApplyPassThroughIgnore(false);
	if (!bWasPassing || !bNudge || !CharacterOwner || !UpdatedComponent || !RunsMovementHere())
	{
		return;
	}
	// Never left inside a block: the nearest spot where the capsule fits (rings round it, straight up first).
	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	const FVector Here = UpdatedComponent->GetComponentLocation();
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Here);
	if (!Capsule || !Island)
	{
		return;
	}
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	if (Island->CanFitCapsule(Here, Radius, HalfHeight))
	{
		return;
	}
	static const FVector2D Ways[] = { FVector2D(0.f, 1.f), FVector2D(-1.f, 0.f), FVector2D(1.f, 0.f), FVector2D(-0.7071f, 0.7071f),
		FVector2D(0.7071f, 0.7071f), FVector2D(0.f, -1.f), FVector2D(-0.7071f, -0.7071f), FVector2D(0.7071f, -0.7071f) };
	for (int32 Ring = 1; Ring <= PassThroughNudgeRings; ++Ring)
	{
		const float Distance = Ring * PassThroughNudgeStep;
		for (const FVector2D& Way : Ways)
		{
			const FVector Spot = Here + FVector(Way.X * Distance, 0.f, Way.Y * Distance);
			if (Island->CanFitCapsule(Spot, Radius, HalfHeight))
			{
				CharacterOwner->SetActorLocation(ConstrainLocationToPlane(Spot), false, nullptr, ETeleportType::TeleportPhysics);
				++PassThroughNudges;
				LastPassThroughNudge = Distance;
				UE_LOG(LogPingu, Log, TEXT("%s: through the island's blocks no more, but inside one: put %.0f cm away at %s"),
					*GetNameSafe(CharacterOwner.Get()), Distance, *Spot.ToCompactString());
				return;
			}
		}
	}
}

void UPenguinMovementComponent::ApplyPassThroughIgnore(bool bIgnore)
{
	UWorld* World = GetWorld();
	if (bPassThroughIgnoring == bIgnore || !UpdatedPrimitive || !World)
	{
		return;
	}
	bPassThroughIgnoring = bIgnore;
	for (TActorIterator<ABlockIsland> It(World); It; ++It)
	{
		UpdatedPrimitive->IgnoreActorWhenMoving(*It, bIgnore);
	}
}

void UPenguinMovementComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// The owner simulates these itself (and gets the server's effects as RPCs).
	DOREPLIFETIME_CONDITION(UPenguinMovementComponent, NetState, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(UPenguinMovementComponent, NetStatus, COND_SkipOwner);
	DOREPLIFETIME(UPenguinMovementComponent, NetSizeScale);
}

void UPenguinMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	const bool bNetGame = CharacterOwner && !IsNetMode(NM_Standalone);
	bHostSimulatedThisFrame = false;
	if (bNetGame)
	{
		UpdateNetTickOrder();
		if (!RunsMovementHere())
		{
			if (CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
			{
				// Before the engine extrapolates the proxy: gravity reads these flags and timers.
				RestoreNetState(NetState, true);
				ApplyNetStatus();
			}
			else
			{
				TickRemoteCopy(DeltaTime);
			}
			if (!bHostSimulatedThisFrame)
			{
				TimeSinceGrounded = IsMovingOnGround() ? 0.f : TimeSinceGrounded + DeltaTime;
			}
		}
#if !UE_BUILD_SHIPPING
		else if (CharacterOwner->GetLocalRole() == ROLE_AutonomousProxy && CharacterOwner->IsLocallyControlled())
		{
			DevCheatOwnerTick(DeltaTime); // (dev builds: the net test's cheating joiner, -PinguCheat=fly / slide / ...)
		}
#endif
	}

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Network smoothing (which the engine runs only for penguins this machine does not simulate) just moved the mesh back
	// to the feet-origin place: a death look that turns the mesh about the body's middle puts its place back.
	if (bNetGame && !RunsMovementHere())
	{
		if (APenguinCharacter* TickedPenguin = Cast<APenguinCharacter>(CharacterOwner))
		{
			TickedPenguin->ReapplyDeathMeshLocation();
		}
	}

	// (The host's simulation of a remote player's copy ran them after its move already.)
	if (!bHostSimulatedThisFrame)
	{
		PostMoveSteps(DeltaTime);
	}

	if (!bNetGame || !CharacterOwner)
	{
		return;
	}
	if (CharacterOwner->HasAuthority())
	{
		PublishNetState();
	}
	else if (CharacterOwner->GetLocalRole() == ROLE_AutonomousProxy && CharacterOwner->IsLocallyControlled())
	{
		SendMoveSync(DeltaTime);
	}
}

void UPenguinMovementComponent::PostMoveSteps(float DeltaTime)
{
	// The shell's bubble bounced off a wall during the move (HandleImpact): it flies back off it now.
	if (bShellBouncePending)
	{
		bShellBouncePending = false;
		if (RunsMovementHere())
		{
			Velocity.X = ShellBounceVelocityX;
			Velocity.Y = 0.f;
		}
	}
	// Rubber hit a wall in a knockback flight (HandleImpact): it bounces back off it now.
	if (bRubberBouncePending)
	{
		bRubberBouncePending = false;
		if (RunsMovementHere() && !bKnockedOut)
		{
			Velocity.X = RubberBounceVelocityX;
			Velocity.Y = 0.f;
			Facing = -FMath::Sign(RubberBounceVelocityX); // still looking back the way it came
			++BounceEvents;
		}
	}

	// How far the belly slide has gone (the server's hit check reads it: it goes out with the move state).
	if (CharacterOwner && UpdatedComponent && bBellySliding && RunsMovementHere())
	{
		const float SlideX = static_cast<float>(UpdatedComponent->GetComponentLocation().X);
		SlideTravel += FMath::Abs(SlideX - SlideLastX);
		SlideLastX = SlideX;
	}

	// Walked onto a jump pad (from a ledge at its height): up as well.
	if (CharacterOwner && RunsMovementHere() && IsMovingOnGround() && CurrentFloor.IsWalkableFloor()
		&& IsJumpPad(CurrentFloor.HitResult.GetComponent()))
	{
		BounceOffJumpPad();
	}

	// Walked (jumped, slid, fell) into an icicle stuck in the ground: the same throw straight up. Once per icicle visit: the
	// wait keeps it from throwing again every frame while the capsule is still in it.
	GroundPadWait = FMath::Max(GroundPadWait - DeltaTime, 0.f);
	if (CharacterOwner && RunsMovementHere() && !bKnockedOut && GroundPadWait <= 0.f && IsTouchingGroundPad())
	{
		GroundPadWait = PenguinMovementPrivate::GroundPadRearm;
		BounceOffJumpPad();
	}
}

void UPenguinMovementComponent::PreReplication(IRepChangedPropertyTracker& ChangedPropertyTracker)
{
	Super::PreReplication(ChangedPropertyTracker);
	// The engine gathers the actor's ReplicatedMovement at the send; NetState / NetStatus were captured in our tick, before the
	// actors that tick after us this frame (a tornado's throw, a projectile's hit) changed the penguin. Captured again here, a
	// client gets the flight and the hit that started it (HitEvents, stun, knock-out) in the same update.
	if (CharacterOwner && CharacterOwner->HasAuthority() && !IsNetMode(NM_Standalone))
	{
		PublishNetState();
	}
}

bool UPenguinMovementComponent::IsHostEffectOwned() const
{
	// No control at all, and the host decided it: the owner's simulation would do just what the host's does.
	return !bKnockedOut && (StunLeft > 0.f || FrozenLeft > 0.f || PullLeft > 0.f || HoverLeft > 0.f || bStomping || bStompHop || bStompWindup);
}

void UPenguinMovementComponent::TickRemoteCopy(float DeltaTime)
{
	using namespace PenguinMovementPrivate;
	const double Now = PinguKillCam::PacedSeconds(GetWorld());

	// Who moves the copy this frame: the host itself while an effect of its own has the penguin (a stun, a freeze, a pull, a
	// hover, a stomp), a teleport is on its way to the owner, the owner's moves were refused, or the owner went quiet.
	const bool bWasEffectOwned = bHostOwnedByEffect;
	bHostOwnedByEffect = IsHostEffectOwned();
	if (bHostOwnedByEffect)
	{
		const FString Reasons = FString::Printf(TEXT("%s%s%s%s%s"), StunLeft > 0.f ? TEXT("stun ") : TEXT(""), FrozenLeft > 0.f ? TEXT("frozen ") : TEXT(""),
			PullLeft > 0.f ? TEXT("pull ") : TEXT(""), HoverLeft > 0.f ? TEXT("hover ") : TEXT(""), IsStomping() ? TEXT("stomp ") : TEXT(""));
		if (!bWasEffectOwned)
		{
			HostOwnedSince = Now;
			HostOwnedWhy.Reset();
		}
		if (!HostOwnedWhy.Contains(Reasons))
		{
			HostOwnedWhy += Reasons;
		}
	}
	if (bWasEffectOwned && !bHostOwnedByEffect && !bKnockedOut)
	{
		bHandbackPending = true; // the owner's next move must be on the host's path
	}
	if (!bHostQuiet && !bKnockedOut && HostLastOwnerMove >= 0.0 && Now - HostLastOwnerMove > HostQuietTime)
	{
		bHostQuiet = true;
		++PinguHostAuthority::Stats().Silences;
		UE_LOG(LogPingu, Log, TEXT("[ANTICHEAT] %s: no move from its owner for %.1f s, the host moves it meanwhile"),
			*GetNameSafe(CharacterOwner.Get()), Now - HostLastOwnerMove);
	}
	bHostOwnedNow = !bKnockedOut && (bHostOwnedByEffect || bNetPositionGuard || bHostPenalty || bHostQuiet);

	if (bHostOwnedNow)
	{
		HostSimulate(DeltaTime, Now); // (its move runs the timers below)
	}
	if (!bHostTimersRun)
	{
		// Server gameplay reads these on the copy (stun, frozen, ...): they run down here as they do on the owner.
		DashCooldownLeft = FMath::Max(0.f, DashCooldownLeft - DeltaTime);
		BellySlideCooldownLeft = FMath::Max(0.f, BellySlideCooldownLeft - DeltaTime);
		StunLeft = FMath::Max(0.f, StunLeft - DeltaTime);
		SlapDizzyLeft = FMath::Max(0.f, SlapDizzyLeft - DeltaTime);
		FrozenLeft = FMath::Max(0.f, FrozenLeft - DeltaTime);
		LowGravityLeft = FMath::Max(0.f, LowGravityLeft - DeltaTime);
		FloatLeft = FMath::Max(0.f, FloatLeft - DeltaTime);
		SlipperyLeft = FMath::Max(0.f, SlipperyLeft - DeltaTime);
		HoverLeft = FMath::Max(0.f, HoverLeft - DeltaTime); // (no ability while it hangs in its tornado: CanUseAbilities)
		PullLeft = FMath::Max(0.f, PullLeft - DeltaTime);
		TimeSinceJumpLaunch += DeltaTime;
	}
	bHostTimersRun = false;
	HostPassThroughLeft = FMath::Max(0.f, HostPassThroughLeft - DeltaTime);
	const bool bLifted = LowGravityLeft > 0.f || FloatLeft > 0.f || HostPassThroughLeft > 0.f;
	HostFreeAfter = bLifted ? HostFreeAfterTime : FMath::Max(0.f, HostFreeAfter - DeltaTime);

	if (bNetPositionGuard)
	{
		NetGuardTimeLeft -= DeltaTime;
		if (NetGuardTimeLeft <= 0.f)
		{
			// Server code moved a remote player's penguin and its owner never followed: SetActorLocation instead of NetTeleport?
			// (Or the owner ignores the host: its moves are judged again from here, against where the host has it.)
			UE_LOG(LogPingu, Warning, TEXT("%s was moved on the server (%s -> %s) but its owner did not follow: use APenguinCharacter::NetTeleport"),
				*GetNameSafe(CharacterOwner.Get()), *NetGuardFrom.ToCompactString(), *NetGuardTarget.ToCompactString());
			ReleaseNetPositionGuard(TEXT("the owner did not get there in time"));
		}
	}

	// Every effect waits at most NetStaleTimeout for the owner's confirmation (an owner that never confirms one gets no free
	// pass: its moves are judged all along, and the host has its penguin while the effect's stun or freeze lasts).
	const int32 PendingBefore = NetPendingEffects.Num();
	NetPendingEffects.RemoveAll([Now](const FPenguinPendingNetEffect& Pending) { return Now - Pending.SentTime > NetStaleTimeout; });
	if (NetPendingEffects.Num() < PendingBefore)
	{
		UE_LOG(LogPingu, Warning, TEXT("%s: the owner has not confirmed %d movement effect(s) for %.1f s, using its state again"),
			*GetNameSafe(CharacterOwner.Get()), PendingBefore - NetPendingEffects.Num(), NetStaleTimeout);
	}

	// The animation of a remote player's penguin only advances with its moves on the server (the engine sets the mesh to
	// bOnlyAllowAutonomousTickPose for remote players): what came in since the last frame, in one go.
	if (NetPendingPoseTime > 0.f && CharacterOwner->GetMesh() && !CharacterOwner->IsPlayingRootMotion())
	{
		TickCharacterPose(FMath::Min(NetPendingPoseTime, 0.25f));
	}
	NetPendingPoseTime = 0.f;

	// A new position from the owner's moves: a new transform time stamp for the clients' linear smoothing (the owner's move
	// time, like the engine does for remote players), and on a listen server the host's own view of it is smoothed (not the
	// host's own simulation: that moves smoothly already).
	if (UpdatedComponent)
	{
		const FVector NewLocation = UpdatedComponent->GetComponentLocation();
		if (bNetSmoothedLocationValid && !NewLocation.Equals(NetSmoothedLocation, 0.01))
		{
			ServerLastTransformUpdateTimeStamp = static_cast<float>(NetClientMoveTime);
			if (IsNetMode(NM_ListenServer) && !bHostSimulatedThisFrame)
			{
				const FQuat CopyRotation = UpdatedComponent->GetComponentQuat();
				SmoothCorrection(NetSmoothedLocation, CopyRotation, NewLocation, CopyRotation);
			}
		}
		NetSmoothedLocation = NewLocation;
		bNetSmoothedLocationValid = true;

		// The host's own path (the hand-back compares the owner's position with it), and the way of a slide it counted.
		HostPath.Add({ Now, NewLocation });
		while (HostPath.Num() > 1 && HostPath[0].Time < Now - 1.0)
		{
			HostPath.RemoveAt(0, EAllowShrinking::No);
		}
		if (bHostSlideActive)
		{
			const float SlideX = static_cast<float>(NewLocation.X);
			HostSlideTravel += FMath::Abs(SlideX - HostSlideLastX);
			HostSlideLastX = SlideX;
		}
	}
}

void UPenguinMovementComponent::HostSimulate(float DeltaTime, double Now)
{
	if (!CharacterOwner || !UpdatedComponent || DeltaTime <= 0.f)
	{
		return;
	}
	// From the owner's dash or ledge climb (their timers are the owner's): the host's no-control physics take over from here.
	if (IsDashing() || IsClimbing())
	{
		SetMovementMode(MOVE_Falling);
	}
	// No input: the host's copy only has what the host gave it (a stun, a freeze and a pull leave no control anyway).
	Acceleration = FVector::ZeroVector;
	bHostSimulating = true;
	bHostTimersRun = false;
	PerformMovement(DeltaTime);
	PostMoveSteps(DeltaTime); // (bounces, jump pads: as on the owner)
	bHostSimulating = false;
	bHostSimulatedThisFrame = true;
	// The owner's next move must not look like a relocation by server code (MoveAutonomous), and the clients' smoothing gets
	// time stamps that go on with the host's own simulation.
	LastUpdateLocation = UpdatedComponent->GetComponentLocation();
	LastUpdateVelocity = Velocity;
	NetClientMoveTime += DeltaTime;
	HostJudge.FollowHost(Now, UpdatedComponent->GetComponentLocation(), Velocity, IsMovingOnGround(), GetHostMoveLimits());
	HostLandGate.Observe(static_cast<float>(UpdatedComponent->GetComponentLocation().Z), IsMovingOnGround());
}

void UPenguinMovementComponent::MoveAutonomous(float ClientTimeStamp, float DeltaTime, uint8 CompressedFlags, const FVector& NewAccel)
{
	if (!IsRemotePlayerCopy())
	{
		Super::MoveAutonomous(ClientTimeStamp, DeltaTime, CompressedFlags, NewAccel);
		return;
	}
	if (!HasValidData())
	{
		return;
	}
	// The owner has simulated this move already. ServerMoveHandleClientError (right after this) takes its position and movement
	// mode; simulating here with inputs we never got (jump presses, the buffer, the dash) would only be wrong. What stays: the
	// move time, the acceleration, and the pose time (the engine ticks a remote player's animation with its moves; we do it
	// once per frame in TickRemoteCopy, the owner sends a move every frame of its own). While the host simulates the copy
	// itself, its own frames are the move time (HostSimulate).
	if (!bHostOwnedNow)
	{
		NetClientMoveTime += DeltaTime;
	}
	NetPendingPoseTime += DeltaTime;
	Acceleration = NewAccel.GetClampedToMaxSize(GetMaxAcceleration());

	// Moved here by server code since the owner's last move (a respawn, an old-style teleport): the owner's moves from before
	// it heard about that would put the copy back (and into the sea). Wait until the owner is there too.
	if (UpdatedComponent)
	{
		const FVector Here = UpdatedComponent->GetComponentLocation();
		if (FVector::DistSquared(Here, LastUpdateLocation) > FMath::Square(PenguinMovementPrivate::NetRelocateDistance))
		{
			ArmNetPositionGuard(LastUpdateLocation, Here, false, 0);
		}
	}
	bServerAcceptClientAuthoritativePosition = !bNetPositionGuard;
}

void UPenguinMovementComponent::ServerMoveHandleClientError(float ClientTimeStamp, float DeltaTime, const FVector& Accel, const FVector& RelativeClientLocation,
	FMovementBaseInterfaceData* ClientMovementBaseInterfaceData, FName ClientBaseBoneName, uint8 ClientMovementMode)
{
	using namespace PenguinMovementPrivate;
	if (!IsRemotePlayerCopy() || !UpdatedComponent)
	{
		Super::ServerMoveHandleClientError(ClientTimeStamp, DeltaTime, Accel, RelativeClientLocation, ClientMovementBaseInterfaceData, ClientBaseBoneName,
			ClientMovementMode);
		return;
	}
	// The owner moves its own penguin (the LAN model); the host decides what it can't do (PinguHostAuthority.h).
	const double Now = PinguKillCam::PacedSeconds(GetWorld());
	PinguHostAuthority::FStats& Numbers = PinguHostAuthority::Stats();
	if (Numbers.FirstMoveTime < 0.0)
	{
		Numbers.FirstMoveTime = Now;
	}
	HostLastOwnerMove = Now;
	if (bHostQuiet)
	{
		// Back after a quiet while (the host moved its penguin meanwhile): it counts again once it is on the host's path.
		bHostQuiet = false;
		if (!bHostPenalty)
		{
			bHostPenalty = true;
			HostPenaltySince = Now;
		}
	}

	// Where the owner says it is, in the world (our moves carry absolute positions unless the base can be named).
	FVector ClientLoc = RelativeClientLocation;
	if (MovementBaseUtility::UseRelativeLocation(ClientMovementBaseInterfaceData))
	{
		MovementBaseUtility::TransformLocationToWorld(ClientMovementBaseInterfaceData, ClientBaseBoneName, RelativeClientLocation, ClientLoc);
	}
	else
	{
		ClientLoc = FRepMovement::RebaseOntoLocalOrigin(ClientLoc, this);
	}
	if (ClientLoc.ContainsNaN())
	{
		PinguAntiCheat::NoteIgnored(CharacterOwner, TEXT("a move"), TEXT("not a number"));
		bServerAcceptClientAuthoritativePosition = false;
		Super::ServerMoveHandleClientError(ClientTimeStamp, DeltaTime, Accel, RelativeClientLocation, ClientMovementBaseInterfaceData, ClientBaseBoneName,
			ClientMovementMode);
		bServerAcceptClientAuthoritativePosition = !bNetPositionGuard;
		return;
	}
	// The play plane: Y is always 0 (a penguin off it could not be hit).
	const FVector OnPlane = ConstrainLocationToPlane(ClientLoc);
	if (FMath::Abs(OnPlane.Y - ClientLoc.Y) > 1.0)
	{
		++Numbers.YClamped;
		PinguAntiCheat::NoteIgnored(CharacterOwner, TEXT("a move off the play plane"), FString::Printf(TEXT("y %.0f: put back on 0"), ClientLoc.Y));
	}
	ClientLoc = OnPlane;

	const FVector Here = UpdatedComponent->GetComponentLocation();
	const bool bSupported = IsHostSupported(ClientLoc);
	bool bTake = false;
	FString Why;
	if (bKnockedOut || bNetPositionGuard)
	{
		// Out (it is not judged), or a teleport on its way (the guard keeps the host's position until the owner is there too).
		bool bIgnored = false;
		PinguAntiCheat::AcceptMove(CharacterOwner, Here, ClientLoc, false, true, false, Now, bIgnored);
		bTake = bKnockedOut && bServerAcceptClientAuthoritativePosition;
	}
	else if (IsHostEffectOwned())
	{
		// The host has it (a stun, a freeze, a pull, a hover, a stomp): the owner's moves only count again after it.
		bool bIgnored = false;
		PinguAntiCheat::AcceptMove(CharacterOwner, Here, ClientLoc, false, true, false, Now, bIgnored);
	}
	else if (bHostPenalty)
	{
		// The host took it over (refused moves): back as soon as the owner is on the host's path and its move is fine.
		if (IsNearHostPath(ClientLoc, Now) && JudgeOwnerMove(ClientLoc, bSupported, Now, Why))
		{
			bHostPenalty = false;
			bTake = true;
			UE_LOG(LogPingu, Log, TEXT("[ANTICHEAT] %s: its owner is back on the host's path (%.1f s)"), *GetNameSafe(CharacterOwner.Get()),
				Now - HostPenaltySince);
		}
		else
		{
			++Numbers.IgnoredWhileTaken; // (still not where the host has it: ignored like a refused move)
		}
		if (!bTake && Now - HostPenaltySince >= HostPullBackAfter - 0.001 && Now - HostLastPullBack >= HostPullBackRepeat)
		{
			// Refused a while: the owner is told to be where the host has it (an honest machine hardly ever gets here).
			HostLastPullBack = Now;
			++Numbers.PullBacks;
			const double LastEffect = NetLastEffectTime;
			NetTeleportPenguin(Here, Velocity, true);
			NetLastEffectTime = LastEffect; // (our own pull-back is no push that excuses fast moves)
			UE_LOG(LogPingu, Log, TEXT("[ANTICHEAT] %s: put back where the host has it (%s)"), *GetNameSafe(CharacterOwner.Get()), *Here.ToCompactString());
		}
	}
	else
	{
		bTake = true;
		if (bHandbackPending || bHostOwnedByEffect)
		{
			// The host just had it (a knockback flight, an ice cube ...): the owner must have gone the same way.
			bHandbackPending = false;
			bHostOwnedByEffect = false; // (handed back now: the next tick does not hand it back again)
			++Numbers.Handovers;
			if (!IsNearHostPath(ClientLoc, Now))
			{
				++Numbers.HandoverMisses;
				bTake = false;
				Why = FString::Printf(TEXT("not where the host's knockback / freeze / pull took it (%.0f cm off)"), FVector::Dist(ClientLoc, Here));
				UE_LOG(LogPingu, Log, TEXT("[ANTICHEAT] %s hand-back: the host had it %.2f s (%s), host at %s going %s, owner at %s, ping %.0f ms"),
					*GetNameSafe(CharacterOwner.Get()), Now - HostOwnedSince, *HostOwnedWhy, *Here.ToCompactString(), *Velocity.ToCompactString(),
					*ClientLoc.ToCompactString(), CharacterOwner->GetPlayerState() ? CharacterOwner->GetPlayerState()->GetPingInMilliseconds() : -1.f);
				PinguAntiCheat::NoteIgnored(CharacterOwner, TEXT("a move"), Why);
			}
		}
		if (bTake && !JudgeOwnerMove(ClientLoc, bSupported, Now, Why))
		{
			bTake = false;
		}
		if (!bTake)
		{
			StartHostPenalty(Why, Now);
		}
	}

	if (!bTake)
	{
		// This move: the host's position stays (the move is still acknowledged).
		bServerAcceptClientAuthoritativePosition = false;
		Super::ServerMoveHandleClientError(ClientTimeStamp, DeltaTime, Accel, RelativeClientLocation, ClientMovementBaseInterfaceData, ClientBaseBoneName,
			ClientMovementMode);
		bServerAcceptClientAuthoritativePosition = !bNetPositionGuard;
		return;
	}
	bServerAcceptClientAuthoritativePosition = true;
	Super::ServerMoveHandleClientError(ClientTimeStamp, DeltaTime, Accel, RelativeClientLocation, ClientMovementBaseInterfaceData, ClientBaseBoneName,
		ClientMovementMode);
	// Taken - on the play plane, and walking only on ground the host finds there.
	const FVector Taken = UpdatedComponent->GetComponentLocation();
	if (FMath::Abs(Taken.Y - OnPlane.Y) > 0.5)
	{
		UpdatedComponent->SetWorldLocation(ConstrainLocationToPlane(Taken), false);
		LastUpdateLocation = UpdatedComponent->GetComponentLocation();
	}
	if (MovementMode == MOVE_Walking && !bSupported)
	{
		SetMovementMode(MOVE_Falling);
	}
	HostLandGate.Observe(static_cast<float>(ClientLoc.Z), bSupported);
}

bool UPenguinMovementComponent::JudgeOwnerMove(const FVector& ClientLoc, bool bSupported, double Now, FString& OutWhy)
{
	PinguHostAuthority::FStats& Numbers = PinguHostAuthority::Stats();
	// The old gate first (a jump across the island, a run far faster than a dash; it logs and counts itself) ...
	bool bIgnored = false;
	const bool bPushed = Now - NetLastEffectTime < 4.0;
	if (!PinguAntiCheat::AcceptMove(CharacterOwner, UpdatedComponent->GetComponentLocation(), ClientLoc, bSupported, false, bPushed, Now, bIgnored))
	{
		++Numbers.Refused;
		OutWhy = TEXT("a jump or a run no penguin can do");
		return false;
	}
	// ... then the host's idea of what the penguin can do (height, air time, sideways speed).
	FPinguMoveSample Sample;
	Sample.Now = Now;
	Sample.Location = ClientLoc;
	Sample.bSupported = bSupported;
	Sample.bFreeVertical = IsHostFreeVertical();
	Sample.bNearPad = IsNearJumpPad(ClientLoc);
	if (!HostJudge.Judge(Sample, GetHostMoveLimits(), OutWhy))
	{
		++Numbers.Refused;
		PinguAntiCheat::NoteIgnored(CharacterOwner, TEXT("a move"), OutWhy);
		return false;
	}
	return true;
}

void UPenguinMovementComponent::StartHostPenalty(const FString& Why, double Now)
{
	if (bHostPenalty)
	{
		return;
	}
	bHostPenalty = true;
	HostPenaltySince = Now;
	++PinguHostAuthority::Stats().Penalties;
	// The host's copy goes on from here by itself: with a speed it could have had (the owner's last one may be made up).
	const FPinguMoveLimits Limits = GetHostMoveLimits();
	const float Side = FMath::Max(Limits.SideSpeed, HostJudge.GetSideBudget());
	Velocity.X = FMath::Clamp(static_cast<float>(Velocity.X), -Side, Side);
	Velocity.Y = 0.f;
	Velocity.Z = FMath::Min(static_cast<float>(Velocity.Z), FMath::Max(0.f, FMath::Sqrt(2.f * Limits.Gravity * Limits.CreditHeight)));
	if (IsDashing() || IsClimbing() || (MovementMode == MOVE_Walking && !IsHostSupported(UpdatedComponent->GetComponentLocation())))
	{
		SetMovementMode(MOVE_Falling);
	}
	UE_LOG(LogPingu, Log, TEXT("[ANTICHEAT] host took over %s: %s"), *GetNameSafe(CharacterOwner.Get()), *Why);
}

bool UPenguinMovementComponent::IsHostSupported(const FVector& Location) const
{
	using namespace PenguinMovementPrivate;
	const UCapsuleComponent* Capsule = CharacterOwner ? CharacterOwner->GetCapsuleComponent() : nullptr;
	if (!Capsule)
	{
		return false;
	}
	FFindFloorResult Floor;
	ComputeFloorDist(Location, HostFloorReach, HostFloorReach, Floor, Capsule->GetScaledCapsuleRadius());
	if (Floor.bBlockingHit && Floor.IsWalkableFloor() && Floor.FloorDist <= HostFloorReach)
	{
		return true;
	}
	// Sticky hangs on walls: a wall right beside it holds it up as well.
	return HasTraitNow(EPinguTrait::Sticky) && (IsWallBeside(1.f, StickyWallReach + 8.f, &Location) || IsWallBeside(-1.f, StickyWallReach + 8.f, &Location));
}

bool UPenguinMovementComponent::IsNearJumpPad(const FVector& Location) const
{
	TArray<APenguinProjectile*> All;
	APenguinProjectile::GetAll(GetWorld(), All);
	for (const APenguinProjectile* Projectile : All)
	{
		if (!Projectile || FVector::DistSquared(Projectile->GetActorLocation(), Location) > FMath::Square(PenguinMovementPrivate::HostPadReach))
		{
			continue;
		}
		// An icicle stuck in the ground, or one stuck in a wall (its peg throws whoever lands on it).
		TArray<UActorComponent*> Pads = Projectile->GetComponentsByTag(UPrimitiveComponent::StaticClass(), JumpPadTag);
		if (Projectile->IsGroundPad() || Pads.Num() > 0)
		{
			return true;
		}
	}
	return false;
}

bool UPenguinMovementComponent::IsHostFreeVertical() const
{
	return LowGravityLeft > 0.f || FloatLeft > 0.f || HostPassThroughLeft > 0.f || HostFreeAfter > 0.f;
}

bool UPenguinMovementComponent::IsNearHostPath(const FVector& Location, double Now) const
{
	using namespace PenguinMovementPrivate;
	if (!UpdatedComponent)
	{
		return false;
	}
	// As far back as the owner's moves are late (its ping), and a little more.
	double Ping = 0.0;
	if (const APlayerState* Player = CharacterOwner ? CharacterOwner->GetPlayerState() : nullptr)
	{
		Ping = Player->GetPingInMilliseconds() / 1000.0;
	}
	const double Age = FMath::Clamp(HostPathMinAge + 1.5 * Ping, HostPathMinAge, HostPathMaxAge);
	const float Tolerance = (HostPathTolerance + static_cast<float>(Velocity.Size()) * HostPathSpeedTime) * FMath::Sqrt(FMath::Max(NetSizeScale, 1.f));
	const float ToleranceSquared = FMath::Square(Tolerance);
	if (FVector::DistSquared(Location, UpdatedComponent->GetComponentLocation()) <= ToleranceSquared)
	{
		return true;
	}
	for (int32 Index = HostPath.Num() - 1; Index >= 0 && HostPath[Index].Time >= Now - Age; --Index)
	{
		if (FVector::DistSquared(Location, HostPath[Index].Location) <= ToleranceSquared)
		{
			return true;
		}
	}
	return false;
}

FPinguMoveLimits UPenguinMovementComponent::GetHostMoveLimits() const
{
	FPinguMoveLimits Limits;
	const UWorld* World = GetWorld();
	const float WorldGravity = World ? FMath::Abs(World->GetGravityZ()) : PenguinMovementPrivate::DefaultWorldGravity;
	// (Low / High Gravity: all of the penguin's gravity x the trait's; the heights its lifts reach go with 1 / that.)
	const float TraitGravity = FMath::Max(GetTraitNumbers().GravityScale, 0.05f);
	Limits.Gravity = FMath::Max(WorldGravity * GravityScale * TraitGravity, 100.f);
	Limits.ApexGravityScale = FMath::Clamp(ApexGravityMultiplier, 0.05f, 1.f);
	Limits.ApexSpeed = ApexSpeedThreshold;
	Limits.SlowestFall = 0.6f * FMath::Min(GetEffectiveMaxFallSpeed(), MaxFallSpeed);
	// Its own lifts: a held jump, an air jump, an air dash (straight up, then its exit speed), a ledge climb; the highest.
	const float Jump = JumpScale * GetTraitJumpScale();
	const float DashHeight = GetEffectiveDashSpeed() * DashDuration + FMath::Square(GetEffectiveDashExitSpeed()) / (2.f * Limits.Gravity);
	const float Highest = FMath::Max(FMath::Max(GetEffectiveJumpHeight(), DoubleJumpHeight * Jump * Jump) / TraitGravity,
		FMath::Max(DashHeight, GetEffectiveClimbReachUp() * FMath::Max(NetSizeScale, 1.f)));
	Limits.CreditHeight = Highest * 1.15f + 40.f;
	Limits.Credits = GetMaxAirJumpsNow() + MaxAirDashes + 2;
	Limits.PadSpeed = JumpPadSpeed * Jump;
	Limits.BounceShare = HasTraitNow(EPinguTrait::Rubber) ? PinguTraitTuning::RubberBounceHeight : 0.f;
	const float OwnFastest = FMath::Max(FMath::Max(GetEffectiveRunSpeed() * SpeedScale, GetEffectiveDashSpeed()),
		FMath::Max(BellySlideSpeed, FMath::Max(BellySlideJumpSpeed, BellyDiveSpeed)));
	Limits.SideSpeed = OwnFastest * 1.25f + 150.f;
	return Limits;
}

float UPenguinMovementComponent::GetSlideHitSpeed() const
{
	const float Claimed = FMath::Abs(static_cast<float>(Velocity.X));
	if (!IsRemotePlayerCopy() || HostPath.Num() < 2)
	{
		return Claimed;
	}
	// The host's own measure: how fast the copy went over its last tenth of a second.
	const FPenguinHostPathPoint& Last = HostPath.Last();
	int32 Index = HostPath.Num() - 2;
	while (Index > 0 && Last.Time - HostPath[Index].Time < 0.1)
	{
		--Index;
	}
	const double Span = Last.Time - HostPath[Index].Time;
	const float Measured = Span > 0.01 ? static_cast<float>(FMath::Abs(Last.Location.X - HostPath[Index].Location.X) / Span) : Claimed;
	return FMath::Min(Claimed, Measured + PenguinMovementPrivate::HostSlideSpeedRoom);
}

bool UPenguinMovementComponent::CanDelaySendingMove(const FSavedMovePtr& /*NewMove*/)
{
	// A delayed move is combined with the next one: the engine rewinds the penguin to the start of the first move and simulates
	// both again in one go. Our jump buffer, timers and event counters are not made for replays (a jump would get lost), and the
	// server takes our position anyway. On a LAN every move simply goes out at once.
	return false;
}

bool UPenguinMovementComponent::ForcePositionUpdate(float DeltaTime)
{
	// The engine simulates a client forward when its moves stop for a moment (and then rejects its older moves). The owner of
	// a remote player's penguin is authoritative for its position: the copy waits where it is (a quiet owner: the host
	// simulates it itself, TickRemoteCopy).
	if (IsRemotePlayerCopy())
	{
		return false;
	}
	return Super::ForcePositionUpdate(DeltaTime);
}

// ---------------------------------------------------------------------------- networking: state

FPenguinNetMoveState UPenguinMovementComponent::CaptureNetState() const
{
	using namespace PenguinMovementPrivate;
	FPenguinNetMoveState Out;
	Out.JumpEvents = JumpEvents;
	Out.AirJumpEvents = AirJumpEvents;
	Out.DashEvents = DashEvents;
	Out.LandEvents = LandEvents;
	Out.ClimbEvents = ClimbEvents;
	Out.StompLandEvents = StompLandEvents;
	Out.PadBounceEvents = PadBounceEvents;
	Out.SlideEvents = SlideEvents;
	Out.SlideTravel = QuantizeLandSpeed(SlideTravel);
	Out.LastLandSpeed = QuantizeLandSpeed(LastLandSpeed);
	Out.LastStompLandSpeed = QuantizeLandSpeed(LastStompLandSpeed);
	Out.MoveInputX = QuantizeAxis(static_cast<float>(MoveInput.X));
	Out.MoveInputY = QuantizeAxis(static_cast<float>(MoveInput.Y));
	Out.bFacingLeft = Facing < 0.f;
	Out.bJumpHeld = bJumpHeld;
	Out.bStomping = bStomping;
	Out.bStompHop = bStompHop;
	Out.bStompWindup = bStompWindup;
	Out.bFastFalling = bFastFalling;
	Out.bOnIce = bOnIce;
	Out.bDashFromGround = bDashFromGround;
	Out.bApexHang = bApexHangAllowed;
	Out.bLastJumpFromRun = bLastJumpFromRun;
	Out.bBellySliding = bBellySliding;
	Out.bLastSlideWasSlip = bLastSlideWasSlip;
	Out.bLowCapsule = bLowCapsule;
	Out.bWallClinging = bWallClinging;
	Out.BounceEvents = BounceEvents;
	Out.WallJumpEvents = WallJumpEvents;
	Out.bPassThroughIsland = bPassThroughIsland;
	Out.bFlapping = bFlapping;
	return Out;
}

void UPenguinMovementComponent::RestoreNetState(const FPenguinNetMoveState& InState, bool bOwnerSimulated)
{
	// Event counters and what goes with them: the character plays the FX / animation when a counter moves.
	JumpEvents = InState.JumpEvents;
	AirJumpEvents = InState.AirJumpEvents;
	DashEvents = InState.DashEvents;
	LandEvents = InState.LandEvents;
	ClimbEvents = InState.ClimbEvents;
	StompLandEvents = InState.StompLandEvents;
	PadBounceEvents = InState.PadBounceEvents;
	SlideEvents = InState.SlideEvents;
	BounceEvents = InState.BounceEvents;
	WallJumpEvents = InState.WallJumpEvents;
	SlideTravel = InState.SlideTravel;
	bLastSlideWasSlip = InState.bLastSlideWasSlip;
	LastLandSpeed = InState.LastLandSpeed;
	LastStompLandSpeed = InState.LastStompLandSpeed;
	bLastJumpFromRun = InState.bLastJumpFromRun;
	MoveInput = FVector2D(InState.MoveInputX / 127.f, InState.MoveInputY / 127.f);
	bJumpHeld = InState.bJumpHeld;
	bOnIce = InState.bOnIce;
	// The capsule always follows the owner's (its positions are the owner's, lowered or not).
	SetLowCapsule(InState.bLowCapsule, false);
	if (bOwnerSimulated)
	{
		Facing = InState.bFacingLeft ? -1.f : 1.f;
		bStomping = InState.bStomping;
		bStompHop = InState.bStompHop;
		bStompWindup = InState.bStompWindup;
		bFastFalling = InState.bFastFalling;
		bDashFromGround = InState.bDashFromGround;
		bApexHangAllowed = InState.bApexHang;
		bBellySliding = InState.bBellySliding;
		bWallClinging = InState.bWallClinging; // (proxies extrapolate the hang / slow slide on the wall: NewFallVelocity)
		// Second Chance's throw through the island: proxies extrapolate through its blocks too (positions are the owner's).
		bPassThroughIsland = InState.bPassThroughIsland;
		ApplyPassThroughIgnore(bPassThroughIsland);
		bFlapping = InState.bFlapping;
		if (bBellySliding)
		{
			SlideDir = Facing; // a slide locks the facing to its direction
		}
	}
}

void UPenguinMovementComponent::ApplyNetStatus()
{
	float Now = 0.f;
	if (GetNetServerTime(Now))
	{
		StunLeft = FMath::Max(0.f, NetStatus.StunEnd - Now);
		FrozenLeft = FMath::Max(0.f, NetStatus.FrozenEnd - Now);
		SlipperyLeft = FMath::Max(0.f, NetStatus.SlipperyEnd - Now);
		FloatLeft = FMath::Max(0.f, NetStatus.FloatEnd - Now);
		LowGravityLeft = FMath::Max(0.f, NetStatus.LowGravityEnd - Now);
		HoverLeft = FMath::Max(0.f, NetStatus.HoverEnd - Now);
		PullLeft = FMath::Max(0.f, NetStatus.PullEnd - Now);
	}
	HitEvents = NetStatus.HitEvents;
	LastHitSpeed = NetStatus.LastHitSpeed;
	bKnockedOut = NetStatus.bKnockedOut;
	bInputLocked = NetStatus.bInputLocked;
}

void UPenguinMovementComponent::PublishNetState()
{
	using namespace PenguinMovementPrivate;
	NetState = CaptureNetState();
	float Now = 0.f;
	GetNetServerTime(Now);
	PublishEndTime(NetStatus.StunEnd, StunLeft, Now);
	PublishEndTime(NetStatus.FrozenEnd, FrozenLeft, Now);
	PublishEndTime(NetStatus.SlipperyEnd, SlipperyLeft, Now);
	PublishEndTime(NetStatus.FloatEnd, FloatLeft, Now);
	PublishEndTime(NetStatus.LowGravityEnd, LowGravityLeft, Now);
	PublishEndTime(NetStatus.HoverEnd, HoverLeft, Now);
	PublishEndTime(NetStatus.PullEnd, PullLeft, Now);
	NetStatus.HitEvents = HitEvents;
	NetStatus.LastHitSpeed = LastHitSpeed;
	NetStatus.bKnockedOut = bKnockedOut;
	NetStatus.bInputLocked = bInputLocked;
}

void UPenguinMovementComponent::OnRep_NetState()
{
	// Right away (the character may tick before us this frame), and again every tick (a local call may have changed a member).
	if (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		RestoreNetState(NetState, true);
	}
}

void UPenguinMovementComponent::OnRep_NetStatus()
{
	if (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		ApplyNetStatus();
	}
}

void UPenguinMovementComponent::OnRep_NetSizeScale()
{
	if (!CharacterOwner || !CharacterOwner->HasAuthority())
	{
		SetSizeScale(NetSizeScale);
	}
}

// ---------------------------------------------------------------------------- networking: owner -> server

void UPenguinMovementComponent::SendMoveSync(float DeltaTime)
{
	using namespace PenguinMovementPrivate;
	FPenguinMoveSync Sync;
	Sync.State = CaptureNetState();
	Sync.VelocityX = QuantizeSpeed(Velocity.X);
	Sync.VelocityZ = QuantizeSpeed(Velocity.Z);
	if (UpdatedComponent)
	{
		const FVector Here = UpdatedComponent->GetComponentLocation();
		Sync.LocationX = QuantizeLocation(Here.X);
		Sync.LocationZ = QuantizeLocation(Here.Z);
	}
	Sync.EffectAck = NetEffectAck;
#if !UE_BUILD_SHIPPING
	if (PinguHostAuthority::HasDevCheat(TEXT("slide")))
	{
		// The net test's cheating joiner: a new belly slide in every sync, far along and fast (the host counts one a cooldown).
		const APenguinGameState* GameState = APenguinGameState::Get(this);
		if (GameState && GameState->GetPhase() == EPinguPhase::Fight)
		{
			++DevSlideClaims;
			Sync.State.SlideEvents = SlideEvents + DevSlideClaims;
			Sync.State.bBellySliding = true;
			Sync.State.SlideTravel = 1000;
			Sync.VelocityX = Facing < 0.f ? -3000 : 3000;
		}
	}
#endif

	NetSyncAge += DeltaTime;
	const FPenguinMoveSync& Last = NetLastSentSync;
	const bool bNews = !bNetSyncSent || bNetSyncUrgent || Sync.EffectAck != Last.EffectAck || !SameEventsAndFlags(Sync.State, Last.State);
	const bool bMoved = Sync.VelocityX != Last.VelocityX || Sync.VelocityZ != Last.VelocityZ || Sync.LocationX != Last.LocationX
		|| Sync.LocationZ != Last.LocationZ || Sync.State.MoveInputX != Last.State.MoveInputX || Sync.State.MoveInputY != Last.State.MoveInputY;
	if (!bNews && !(bMoved && NetSyncAge >= NetSyncInterval) && NetSyncAge < NetSyncKeepAlive)
	{
		return;
	}
	Sync.Seq = ++NetSyncSeq;
	Rq16(Sync);
	NetLastSentSync = Sync;
	NetSyncAge = 0.f;
	bNetSyncSent = true;
	bNetSyncUrgent = false;
}

void UPenguinMovementComponent::Rq16_Implementation(const FPenguinMoveSync& Sync)
{
	using namespace PenguinMovementPrivate;
	if (!IsRemotePlayerCopy())
	{
		return;
	}
	if (bNetHasSync && !SeqAfter16(Sync.Seq, NetLastSyncSeq))
	{
		return; // older than one we already have (unreliable packets can overtake each other)
	}
	bNetHasSync = true;
	NetLastSyncSeq = Sync.Seq;

	// Effects the owner has applied by now are done; the rest is still on its way to it.
	NetPendingEffects.RemoveAll([&Sync](const FPenguinPendingNetEffect& Pending) { return !SeqAfter(Pending.Seq, Sync.EffectAck); });
	bool bHardPending = false;
	FVector PendingVelocity = FVector::ZeroVector;
	for (const FPenguinPendingNetEffect& Pending : NetPendingEffects)
	{
		bHardPending |= Pending.bHard;
		PendingVelocity += Pending.VelocityDelta;
	}

	// After a teleport on the server the owner's positions count again once it is there too.
	if (bNetPositionGuard)
	{
		if (bNetGuardUntilAck)
		{
			if (!SeqAfter(NetGuardAckSeq, Sync.EffectAck))
			{
				ReleaseNetPositionGuard(TEXT("the owner teleported"));
			}
		}
		else
		{
			const FVector OwnerAt(static_cast<float>(Sync.LocationX), static_cast<float>(NetGuardTarget.Y), static_cast<float>(Sync.LocationZ));
			if (FVector::DistSquared(OwnerAt, NetGuardTarget) < FVector::DistSquared(OwnerAt, NetGuardFrom))
			{
				ReleaseNetPositionGuard(TEXT("the owner arrived"));
			}
		}
	}

	// Counters, the stick and the jump button always (the stomp bounce reads it); the velocity and the movement flags only once
	// the owner has applied every effect that changed them here (no rubber banding from an older state), and not while the
	// host moves the penguin itself. What the host decides stays the host's (ApplyOwnerRequests): the stomp, slides, landings.
	const bool bHostHasIt = !bKnockedOut && (IsHostEffectOwned() || bHostPenalty || bHostQuiet);
	const bool bCurrent = !bHardPending && !bNetPositionGuard && !bHostHasIt;
	const int32 HostStompLands = StompLandEvents;
	const float HostStompLandSpeed = LastStompLandSpeed;
	const bool bHostStomping = bStomping;
	const bool bHostStompHop = bStompHop;
	const bool bHostStompWindup = bStompWindup;
	const int32 HostLands = LandEvents;
	const float HostLandSpeed = LastLandSpeed;
	const int32 HostSlides = SlideEvents;
	RestoreNetState(Sync.State, bCurrent);
	StompLandEvents = HostStompLands;
	LastStompLandSpeed = HostStompLandSpeed;
	bStomping = bHostStomping;
	bStompHop = bHostStompHop;
	bStompWindup = bHostStompWindup;
	LandEvents = HostLands;
	LastLandSpeed = HostLandSpeed;
	SlideEvents = HostSlides;
	ApplyOwnerRequests(Sync, bCurrent);
	if (bCurrent)
	{
		// (A made-up speed only goes as far as the penguin could go: the slide's hit and the abilities read it.)
		const FPinguMoveLimits Limits = GetHostMoveLimits();
		const float Side = FMath::Max(Limits.SideSpeed, HostJudge.GetSideBudget());
		const float Up = FMath::Max(Limits.PadSpeed * 1.1f, FMath::Sqrt(2.f * Limits.Gravity * Limits.CreditHeight));
		Velocity = FVector(FMath::Clamp(static_cast<float>(Sync.VelocityX), -Side, Side), 0.f,
			FMath::Clamp(static_cast<float>(Sync.VelocityZ), -(FastFallMaxSpeed + StompSpeed), Up)) + PendingVelocity;
		UpdateComponentVelocity();
	}
}

void UPenguinMovementComponent::ApplyOwnerRequests(const FPenguinMoveSync& Sync, bool bCurrent)
{
	using namespace PenguinMovementPrivate;
	PinguHostAuthority::FStats& Numbers = PinguHostAuthority::Stats();
	const double Now = PinguKillCam::PacedSeconds(GetWorld());
	const FPenguinNetMoveState& Claim = Sync.State;
	if (!bOwnerCountersSeen)
	{
		// The first sync: where the owner's counters stand (no request yet).
		bOwnerCountersSeen = true;
		OwnerSlideSeen = Claim.SlideEvents;
		OwnerLandSeen = Claim.LandEvents;
		OwnerStompSeen = Claim.StompLandEvents;
	}

	// The stomp's landing is the host's own (its simulation of the stomp): the owner's count is only looked at.
	if (Claim.StompLandEvents != OwnerStompSeen)
	{
		OwnerStompSeen = Claim.StompLandEvents;
		if (Now - HostLastStompLand > 0.6)
		{
			++Numbers.StompClaimsIgnored;
		}
	}

	// A landing (the Giant's ice breaking reads it): only after the host saw the penguin in the air, at most as fast as that fall.
	if (Claim.LandEvents != OwnerLandSeen)
	{
		OwnerLandSeen = Claim.LandEvents;
		float Speed = 0.f;
		if (!bHostOwnedNow && UpdatedComponent
			&& HostLandGate.Request(static_cast<float>(UpdatedComponent->GetComponentLocation().Z), Claim.LastLandSpeed, GetHostMoveLimits(),
				FastFallGravityMultiplier, FastFallStartSpeed, Speed))
		{
			LandEvents = Claim.LandEvents;
			LastLandSpeed = Speed;
			LowGravityLeft = 0.f; // landed: a launch's low gravity ends, as in ProcessLanded on the owner
		}
		else if (!bHostOwnedNow)
		{
			++Numbers.LandsRefused;
		}
	}

	// A new belly slide: it counts with the host's own cooldown, and only while the penguin could slide.
	if (Claim.SlideEvents != OwnerSlideSeen)
	{
		OwnerSlideSeen = Claim.SlideEvents;
		const bool bCanSlide = !bKnockedOut && !bHostOwnedNow && !HasNoControl() && CanBellySlideNow();
		if (HostSlideGate.Request(Now, bCanSlide, BellySlideCooldown))
		{
			++Numbers.SlidesAccepted;
			SlideEvents = Claim.SlideEvents;
			bHostSlideActive = true;
			HostSlideStart = Now;
			HostSlideTravel = 0.f;
			HostSlideLastX = UpdatedComponent ? static_cast<float>(UpdatedComponent->GetComponentLocation().X) : 0.f;
		}
		else
		{
			++Numbers.SlidesRefused;
		}
	}
	// The slide the host counted ends when the owner's does, after its longest time, or when the penguin loses control.
	if (bHostSlideActive && ((bCurrent && !Claim.bBellySliding) || Now - HostSlideStart > SlideMaxTime + 0.5 || HasNoControl() || bKnockedOut))
	{
		bHostSlideActive = false;
	}
	bBellySliding = bHostSlideActive;
	if (bBellySliding)
	{
		SlideDir = Facing; // a slide locks the facing to its direction
		SlideTravel = FMath::Min(static_cast<float>(Claim.SlideTravel), HostSlideTravel + HostSlideTravelRoom);
	}
	else
	{
		SlideTravel = 0.f;
	}
}

// ---------------------------------------------------------------------------- networking: server -> owner

void UPenguinMovementComponent::ForwardEffect(FPenguinNetEffect& Effect, bool bHard, const FVector& VelocityDelta)
{
	Effect.Seq = ++NetEffectSeq;
	NetLastEffectTime = PinguKillCam::PacedSeconds(GetWorld());
	if (NetPendingEffects.Num() >= PenguinMovementPrivate::NetMaxPendingEffects)
	{
		NetPendingEffects.RemoveAt(0);
	}
	FPenguinPendingNetEffect Pending;
	Pending.Seq = Effect.Seq;
	Pending.bHard = bHard;
	Pending.VelocityDelta = VelocityDelta;
	Pending.SentTime = NetLastEffectTime;
	NetPendingEffects.Add(Pending);
	// The host's judge of the owner's moves knows of the push (a flight may go high and fast; a knockback gives the air moves
	// back). The velocity is the copy's after the effect: the host's own numbers.
	if (UpdatedComponent)
	{
		const double Now = NetLastEffectTime;
		const FVector Here = UpdatedComponent->GetComponentLocation();
		switch (Effect.Kind)
		{
		case EPenguinNetEffect::Knockback:
		case EPenguinNetEffect::LaunchPlanar:
			HostJudge.NoteHostPush(Now, Here, Velocity, true, Effect.Kind == EPenguinNetEffect::Knockback, GetHostMoveLimits());
			break;
		case EPenguinNetEffect::AbilityImpulse:
		case EPenguinNetEffect::Nudge:
			HostJudge.NoteHostPush(Now, Here, VelocityDelta, false, false, GetHostMoveLimits());
			break;
		case EPenguinNetEffect::RefreshAirMoves:
			HostJudge.NoteHostPush(Now, Here, FVector::ZeroVector, false, true, GetHostMoveLimits());
			break;
		default:
			break;
		}
	}
	Rq17(Effect);
	if (CharacterOwner)
	{
		CharacterOwner->ForceNetUpdate(); // the others see a hit / freeze at once
	}
}

void UPenguinMovementComponent::Rq17_Implementation(const FPenguinNetEffect& Effect)
{
	// The same public functions the server called: here (not the server) they only apply.
	const FVector2D Planar(Effect.Vector.X, Effect.Vector.Z);
#if !UE_BUILD_SHIPPING
	// The net test's cheating joiner: -PinguCheat=noknock confirms knockbacks, freezes, pulls and launches but ignores them;
	// -PinguCheat=noack applies every effect but never confirms one (below).
	if (PinguHostAuthority::HasDevCheat(TEXT("noknock")) && (Effect.Kind == EPenguinNetEffect::Knockback || Effect.Kind == EPenguinNetEffect::Frozen
		|| Effect.Kind == EPenguinNetEffect::LaunchPlanar || Effect.Kind == EPenguinNetEffect::TornadoPull || Effect.Kind == EPenguinNetEffect::Nudge
		|| Effect.Kind == EPenguinNetEffect::StickLanding))
	{
		NetEffectAck = Effect.Seq;
		bNetSyncUrgent = true;
		return;
	}
#endif
	switch (Effect.Kind)
	{
	case EPenguinNetEffect::Knockback: ApplyKnockback(Planar, Effect.Value); break;
	case EPenguinNetEffect::AbilityImpulse: AddAbilityImpulse(Planar, Effect.Value, Effect.bFlag); break;
	case EPenguinNetEffect::LaunchPlanar: LaunchPlanar(Planar, Effect.Value); break;
	case EPenguinNetEffect::Frozen: SetFrozen(Effect.Value); break;
	case EPenguinNetEffect::Slippery: SetSlippery(Effect.Value); break;
	case EPenguinNetEffect::Float: SetFloat(Effect.Value); break;
	case EPenguinNetEffect::StartStomp: StartStomp(); break;
	case EPenguinNetEffect::StartFlap: StartFlap(true); break;
	case EPenguinNetEffect::RefreshAirMoves: RefreshAirMoves(); break;
	case EPenguinNetEffect::InputLocked: SetInputLocked(Effect.bFlag); break;
	case EPenguinNetEffect::KnockedOut: SetKnockedOut(Effect.bFlag); break;
	case EPenguinNetEffect::Teleport: NetTeleportPenguin(Effect.Location, Effect.Vector, Effect.bFlag, Effect.Value > 0.5f); break;
	case EPenguinNetEffect::Nudge: NudgeVelocity(Planar); break;
	case EPenguinNetEffect::BellySlideEnd: ApplyBellySlideContact(FMath::RoundToInt(Effect.Value), Effect.bFlag); break;
	case EPenguinNetEffect::StickLanding: StickNextLanding(); break;
	case EPenguinNetEffect::Hover: SetHover(Effect.Value); break;
	case EPenguinNetEffect::HoverGlide: SetHoverGlide(Effect.Location, Effect.Value); break;
	case EPenguinNetEffect::TornadoPull: SetTornadoPull(Effect.Location, Effect.Value, static_cast<float>(Effect.Vector.X), static_cast<float>(Effect.Vector.Z)); break;
	case EPenguinNetEffect::PassThroughIsland: SetPassThroughIsland(static_cast<float>(Effect.Location.Z), Effect.Value); break;
	default: break;
	}
#if !UE_BUILD_SHIPPING
	if (PinguHostAuthority::HasDevCheat(TEXT("noack")))
	{
		return; // (once, the host stopped judging the moves of an owner that had not confirmed a push)
	}
#endif
	NetEffectAck = Effect.Seq;
	bNetSyncUrgent = true; // the server ignores our velocity and flags until it hears this
}

void UPenguinMovementComponent::ArmNetPositionGuard(const FVector& From, const FVector& Target, bool bUntilAck, uint8 AckSeq)
{
	using namespace PenguinMovementPrivate;
	bNetPositionGuard = true;
	bNetGuardUntilAck = bUntilAck;
	NetGuardAckSeq = AckSeq;
	NetGuardFrom = From;
	NetGuardTarget = Target;
	NetGuardTimeLeft = bUntilAck ? NetTeleportGuardTimeout : NetGuardTimeout;
	bServerAcceptClientAuthoritativePosition = false; // ServerMoveHandleClientError keeps our position (and still acks the moves)
	LastUpdateLocation = Target;                       // this move is known now
	// The host's judge goes on from the new place (the jump is no speed; its air moves are back, as after a warp).
	const FPinguMoveLimits Limits = GetHostMoveLimits();
	const double Now = PinguKillCam::PacedSeconds(GetWorld());
	HostJudge.ForgetPath();
	HostJudge.FollowHost(Now, Target, Velocity, false, Limits);
	HostJudge.NoteHostPush(Now, Target, FVector::ZeroVector, false, true, Limits);
	HostPath.Reset();
	bHandbackPending = false;
}

void UPenguinMovementComponent::ReleaseNetPositionGuard(const TCHAR* Why)
{
	if (!bNetPositionGuard)
	{
		return;
	}
	bNetPositionGuard = false;
	bServerAcceptClientAuthoritativePosition = true;
	UE_LOG(LogPingu, Verbose, TEXT("%s: using the owner's position again (%s)"), *GetNameSafe(CharacterOwner.Get()), Why);
}

// ---------------------------------------------------------------------------- dev builds: the net test's cheating joiner

#if !UE_BUILD_SHIPPING
void UPenguinMovementComponent::DevCheatOwnerTick(float DeltaTime)
{
	// What a cheating joiner's own machine could do with its penguin (-PinguCheat=...; never in Shipping). The host must not
	// follow any of it (PinguHostAuthority.h; checked by the net test's netcheatmove / netcheatcombat runs).
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	const bool bFight = GameState && GameState->GetPhase() == EPinguPhase::Fight && !bKnockedOut && UpdatedComponent;
	if (!bFight)
	{
		DevFlyLeft = 0.f;
		DevCheatClock = 0.f;
		return;
	}
	DevCheatClock += DeltaTime;
	// fly: every 6 s a hop off toward the nearer sea, then 4 s of hanging in the air (gravity 0) while it drifts out there
	// (longer than all its jumps could keep it up: the host's judge allows about 3.9 s).
	if (PinguHostAuthority::HasDevCheat(TEXT("fly")))
	{
		if (DevFlyLeft <= 0.f && DevCheatClock >= 6.f)
		{
			DevCheatClock = 0.f;
			DevFlyLeft = 4.f;
			const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), UpdatedComponent->GetComponentLocation());
			const float MiddleX = Island ? static_cast<float>(Island->GetBounds().GetCenter().X) : 0.f;
			DevFlyDir = UpdatedComponent->GetComponentLocation().X >= MiddleX ? 1.f : -1.f;
			Velocity.Z = 900.f;
			SetMovementMode(MOVE_Falling);
			UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] dev cheat: flying out over the sea (%s)"), DevFlyDir > 0.f ? TEXT("right") : TEXT("left"));
		}
		if (DevFlyLeft > 0.f)
		{
			DevFlyLeft -= DeltaTime;
			Velocity.X = DevFlyDir * 1200.f;
			if (Velocity.Z < 0.f)
			{
				Velocity.Z = 0.f;
			}
			if (IsMovingOnGround())
			{
				SetMovementMode(MOVE_Falling);
			}
		}
	}
	// noack: every 2 s a 15 m jump along the island (the host once stopped judging an owner that did not confirm a push).
	DevHopClock += DeltaTime;
	if (PinguHostAuthority::HasDevCheat(TEXT("noack")) && DevHopClock >= 2.f)
	{
		DevHopClock = 0.f;
		const FVector From = UpdatedComponent->GetComponentLocation();
		CharacterOwner->SetActorLocation(From + FVector(From.X > 0.f ? -1500.f : 1500.f, 0.f, 200.f), false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] dev cheat: jumped our own penguin from x %.0f to x %.0f"), From.X, CharacterOwner->GetActorLocation().X);
	}
	// flap: every 6 s three flaps of its own 0.9 s apart (flying up), with no Flap ability and nothing asked of the host (one
	// flap is within its own jumps' reach; the host must refuse the rest of the climb).
	DevFlapClock += DeltaTime;
	if (PinguHostAuthority::HasDevCheat(TEXT("flap")) && (DevFlapClock >= 6.f || (DevFlapClock >= 0.9f && DevFlapsLeft > 0)))
	{
		DevFlapsLeft = DevFlapClock >= 6.f ? 2 : DevFlapsLeft - 1;
		DevFlapClock = 0.f;
		StartFlap(true);
		UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] dev cheat: a flap of our own (no ability)"));
	}
	// stomp / giant: a stomp landing every 0.5 s, a hard landing every 0.3 s (neither happened).
	DevPulseClock += DeltaTime;
	if (PinguHostAuthority::HasDevCheat(TEXT("stomp")) && DevPulseClock >= 0.5f)
	{
		DevPulseClock = 0.f;
		++StompLandEvents;
		LastStompLandSpeed = StompSpeed;
	}
	DevLandClock += DeltaTime;
	if (PinguHostAuthority::HasDevCheat(TEXT("giant")) && DevLandClock >= 0.3f)
	{
		DevLandClock = 0.f;
		++LandEvents;
		LastLandSpeed = 3000.f;
	}
}
#endif
