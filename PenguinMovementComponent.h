// Pingu - platform-fighter movement.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PenguinTypes.h"
#include "PinguHostAuthority.h"
#include "PenguinMovementComponent.generated.h"

class UPenguinTraitComponent;
struct FPinguTraitNumbers;
enum class EPinguTrait : uint8;

/** What became of the jump presses (the game logs this for the player after every round, to track down lost jumps). */
struct FPenguinJumpStats
{
	int32 Presses = 0;
	int32 GroundJumps = 0;
	int32 CoyoteJumps = 0;
	int32 NearGroundJumps = 0;
	int32 AirJumps = 0;
	int32 Climbs = 0;
	int32 Dropped = 0;
	int32 DroppedStunned = 0;
	int32 DroppedFrozen = 0;
	int32 DroppedNoAirJump = 0;
	int32 DroppedOther = 0;

	int32 Jumps() const { return GroundJumps + CoyoteJumps + NearGroundJumps + AirJumps + Climbs; }
};

/** A movement effect decided on the server that the owning client of a remote player's penguin applies (see Rq17). */
UENUM()
enum class EPenguinNetEffect : uint8
{
	None,
	Knockback,
	AbilityImpulse,
	LaunchPlanar,
	Frozen,
	Slippery,
	Float,
	StartStomp,
	RefreshAirMoves,
	InputLocked,
	KnockedOut,
	Teleport,
	Nudge,
	/** The server saw the belly slide run into a penguin (Value: that slide's SlideEvents, bFlag: a hit, not a bump). */
	BellySlideEnd,
	/** StickNextLanding (a fish hook's over-head throw). */
	StickLanding,
	/** SetHover (the tornado's user: Value seconds). */
	Hover,
	/** SetTornadoPull (Location: the middle, Value: seconds, Vector: (radius, 0, eye radius)). */
	TornadoPull,
	/** SetPassThroughIsland (Location.Z: the height its feet pass, Value: seconds at most; 0 ends it). */
	PassThroughIsland,
	/** SetHoverGlide (Location: where its actor goes, Value: seconds). */
	HoverGlide,
	/** StartFlap (the Flap ability). */
	StartFlap,
};

/** One forwarded effect: the arguments of the public function that was called on the server. */
USTRUCT()
struct FPenguinNetEffect
{
	GENERATED_BODY()

	/** Counts the effects sent to this owner; the owner sends back the last one it applied. */
	UPROPERTY()
	uint8 Seq = 0;

	UPROPERTY()
	EPenguinNetEffect Kind = EPenguinNetEffect::None;

	/** Velocity / impulse / nudge (X right, Z up). */
	UPROPERTY()
	FVector Vector = FVector::ZeroVector;

	/** Teleport target. */
	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	/** Stun time, low gravity time or seconds; a teleport: 1 = a belly slide goes on. */
	UPROPERTY()
	float Value = 0.f;

	/** Cancel fall / locked / knocked out / force falling. */
	UPROPERTY()
	bool bFlag = false;
};

/**
 * The movement state that animation, FX and server gameplay read on a penguin this machine does not simulate. The owner sends
 * it to the server (FPenguinMoveSync), the server replicates it to everybody else. Small types: it goes out up to 60 times a second.
 */
USTRUCT()
struct FPenguinNetMoveState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 JumpEvents = 0;

	UPROPERTY()
	int32 AirJumpEvents = 0;

	UPROPERTY()
	int32 DashEvents = 0;

	UPROPERTY()
	int32 LandEvents = 0;

	UPROPERTY()
	int32 ClimbEvents = 0;

	UPROPERTY()
	int32 StompLandEvents = 0;

	UPROPERTY()
	int32 PadBounceEvents = 0;

	/** Belly slides started (UPenguinMovementComponent::SlideEvents). */
	UPROPERTY()
	int32 SlideEvents = 0;

	/** cm the current belly slide has gone (the server decides its hits on it). */
	UPROPERTY()
	uint16 SlideTravel = 0;

	/** cm/s */
	UPROPERTY()
	uint16 LastLandSpeed = 0;

	/** cm/s */
	UPROPERTY()
	uint16 LastStompLandSpeed = 0;

	/** Stick / keys, -127..127 */
	UPROPERTY()
	int8 MoveInputX = 0;

	UPROPERTY()
	int8 MoveInputY = 0;

	UPROPERTY()
	bool bFacingLeft = false;

	UPROPERTY()
	bool bJumpHeld = false;

	UPROPERTY()
	bool bStomping = false;

	UPROPERTY()
	bool bStompHop = false;

	UPROPERTY()
	bool bStompWindup = false;

	UPROPERTY()
	bool bFastFalling = false;

	UPROPERTY()
	bool bOnIce = false;

	UPROPERTY()
	bool bDashFromGround = false;

	/** The apex hang of a held jump is allowed (proxies extrapolate the jump arc with the right gravity). */
	UPROPERTY()
	bool bApexHang = false;

	UPROPERTY()
	bool bLastJumpFromRun = false;

	UPROPERTY()
	bool bBellySliding = false;

	UPROPERTY()
	bool bLastSlideWasSlip = false;

	/** On the belly with the small capsule (UPenguinMovementComponent::IsLyingLow). */
	UPROPERTY()
	bool bLowCapsule = false;

	/** Sticky: clinging to a wall (UPenguinMovementComponent::IsWallClinging). */
	UPROPERTY()
	bool bWallClinging = false;

	/** Rubber: bounces off the ground or a wall (UPenguinMovementComponent::BounceEvents). */
	UPROPERTY()
	int32 BounceEvents = 0;

	/** Sticky: wall jumps (UPenguinMovementComponent::WallJumpEvents). */
	UPROPERTY()
	int32 WallJumpEvents = 0;

	/** Passing through the island's blocks on the way up (Second Chance's throw: UPenguinMovementComponent::
	 *  SetPassThroughIsland): proxies extrapolate through them too. */
	UPROPERTY()
	bool bPassThroughIsland = false;

	/** Flap: the flippers beat (proxies hold the climb without gravity, every machine shows the beat). */
	UPROPERTY()
	bool bFlapping = false;
};

/** Owner -> server, unreliable, about every frame (see UPenguinMovementComponent::Rq16). */
USTRUCT()
struct FPenguinMoveSync
{
	GENERATED_BODY()

	UPROPERTY()
	FPenguinNetMoveState State;

	/** cm/s, X right, Z up */
	UPROPERTY()
	int16 VelocityX = 0;

	UPROPERTY()
	int16 VelocityZ = 0;

	/** Where the owner is (cm): the server waits for it after a server-side teleport. */
	UPROPERTY()
	int32 LocationX = 0;

	UPROPERTY()
	int32 LocationZ = 0;

	/** Out-of-order packets are dropped. */
	UPROPERTY()
	uint16 Seq = 0;

	/** The last forwarded effect the owner has applied (FPenguinNetEffect::Seq). */
	UPROPERTY()
	uint8 EffectAck = 0;
};

/** Server-authoritative status, replicated to everybody but the owner (who gets the effects themselves). End times on the server clock. */
USTRUCT()
struct FPenguinNetStatus
{
	GENERATED_BODY()

	/** Server world time the hit stun ends (in the past or 0: not stunned). */
	UPROPERTY()
	float StunEnd = 0.f;

	UPROPERTY()
	float FrozenEnd = 0.f;

	UPROPERTY()
	float SlipperyEnd = 0.f;

	UPROPERTY()
	float FloatEnd = 0.f;

	UPROPERTY()
	float LowGravityEnd = 0.f;

	/** The tornado: its user hangs still until then, a penguin it sucks in is pulled until then (proxies extrapolate with
	 *  the gravity of those states). */
	UPROPERTY()
	float HoverEnd = 0.f;

	UPROPERTY()
	float PullEnd = 0.f;

	UPROPERTY()
	int32 HitEvents = 0;

	UPROPERTY()
	float LastHitSpeed = 0.f;

	UPROPERTY()
	bool bKnockedOut = false;

	UPROPERTY()
	bool bInputLocked = false;
};

/** A forwarded effect the owner has not confirmed yet (server side). */
struct FPenguinPendingNetEffect
{
	uint8 Seq = 0;
	/** It replaced the owner's velocity or movement flags: the owner's state from before it is out of date. */
	bool bHard = false;
	/** What it added to the velocity (ability pushes): added to the owner's velocity until the owner has it too. */
	FVector VelocityDelta = FVector::ZeroVector;
	/** Real time (FPlatformTime) it was sent: every effect waits at most NetStaleTimeout for its confirmation. */
	double SentTime = 0.0;
};

/** Where the server's copy of a remote player's penguin was at a moment (real time): the host's own path. */
struct FPenguinHostPathPoint
{
	double Time = 0.0;
	FVector Location = FVector::ZeroVector;
};

/**
 * The client's moves to the server without a movement base the network cannot name. A penguin stands on the island's
 * collision boxes (and icicle pegs), which every machine makes for itself at runtime; the engine would send the position
 * relative to such a box plus a reference to it that the server cannot resolve (a NetGUID warning for every move, and a
 * position the server cannot place). Such moves carry the absolute position instead.
 */
struct FPenguinNetworkMoveData : public FCharacterNetworkMoveData
{
	virtual void ClientFillNetworkMoveData(const FSavedMove_Character& ClientMove, ENetworkMoveType MoveType) override;
};

struct FPenguinNetworkMoveDataContainer : public FCharacterNetworkMoveDataContainer
{
	FPenguinNetworkMoveDataContainer();

	FPenguinNetworkMoveData PenguinMoveData[3];
};

/**
 * Fast, snappy 2D platformer movement for the penguin, built on UCharacterMovementComponent.
 *
 * Gameplay happens on a plane: X = right, Z = up, Y is locked (a 2D game in a 3D world).
 *
 *  - Run:        high acceleration, extra-strong turn-around, quick stop.
 *  - Jump:       height/time based (JumpHeight + JumpTimeToApex). Releasing early cuts the jump,
 *                holding it gives a short hang at the apex, falling is heavier than rising.
 *  - Forgiveness: coyote time (jump shortly after leaving a ledge) and a jump buffer
 *                (press slightly before landing and the jump still comes out). Falling with the
 *                ground right below the feet, a press is a ground jump (the double jump stays).
 *                A press during a hit stun waits for the stun to end.
 *  - Double jump: resets vertical speed and lets you flip direction instantly.
 *  - Fast fall:  press down in the air after the apex.
 *  - Dash:       8 directions, one air dash per airtime, cooldown. A ground dash can be
 *                cancelled into a jump that keeps the dash speed.
 *  - Ledge grab: out of jumps next to a ledge, jump pulls you up over it.
 *  - Ice:        blocks whose collision is tagged "Ice" are slippery (little grip to speed up,
 *                turn or stop). Ice gun feet slip on every floor, far worse than ice.
 *  - Belly slide: a tap of the slide key throws the penguin on its belly: a burst along the
 *                stick (or the facing) that slows down and ends after about six blocks - twice
 *                that on ice. In the air it is a belly-flop dive that becomes the slide on
 *                landing. Jump out of it to keep the speed. A slide that has gone three blocks
 *                knocks away the penguin it runs into (the server decides: APenguinCharacter).
 *                On the belly the capsule shrinks to a ball (BellySlideHalfHeight), so a slide fits through
 *                a gap one block high. It stands up again only where there is room: under a ceiling that
 *                low the slide goes on (BellySlideCrawlSpeed, the stick turns it around) until there is.
 *
 * All state changes happen inside PerformMovement (UpdateCharacterStateBeforeMovement / Phys*).
 *
 * LAN multiplayer (client-authoritative, latency matters more than cheating at a LAN party):
 *  - The machine that controls a penguin simulates it exactly like solo play: the owning client for a player's penguin, the
 *    server for the host's own penguin and the bots. The server takes a client's position and movement mode with every
 *    move and never corrects it; the client never combines moves (our timers and event counters are not replayable).
 *  - The server copy of a remote player's penguin does not simulate (MoveAutonomous): the owner sends its velocity and
 *    movement state (Rq16), the server copy writes them into its members, and they replicate on to everybody
 *    else (NetState). Server-authoritative status (stun, freeze, slippery feet, float, low gravity, hits, knock-out, input
 *    lock) replicates as NetStatus, the size as NetSizeScale. So on every machine the members and getters tell the truth.
 *  - Effects called on the server for a remote player's penguin (ApplyKnockback, AddAbilityImpulse, LaunchPlanar,
 *    NudgeVelocity, SetFrozen, SetSlippery, SetFloat, StartStomp, RefreshAirMoves, SetInputLocked, SetKnockedOut,
 *    NetTeleportPenguin, EndBellySlideOnContact, SetHover, SetTornadoPull, SetPassThroughIsland; SetSizeScale through
 *    NetSizeScale) are applied to the server copy and
 *    sent to the owner. Until the owner confirms them, its older velocity / movement flags (and after a teleport its
 *    older positions) are ignored.
 *  - Other machines' penguins are the engine's simulated proxies (extrapolated with our gravity rules, linear smoothing).
 *  - The host's say (PinguHostAuthority.h, 2026-10-30): while a knockback's stun, a freeze, a tornado's pull or hover or a belly
 *    stomp lasts, the host simulates the copy itself (HostSimulate) and takes the owner's position again only where its own
 *    path went; every other move of the owner's is judged first (FPinguMoveJudge: height, air time, speed, Y 0, walking only
 *    on ground the host finds). A refused move: the host keeps and simulates its copy and, after a while, puts the owner back.
 *    Slides and landings the owner sends are requests (the host's cooldown, its own sight of the fall); stomp landings are the
 *    host's own.
 */
UCLASS(ClassGroup = (Penguin), meta = (BlueprintSpawnableComponent))
class PENGUIN_API UPenguinMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	UPenguinMovementComponent();

	// ------------------------------------------------------------------ Run
	/** Top running speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Run", meta = (ForceUnits = "cm/s"))
	float RunSpeed = 560.f;

	/** Acceleration toward the input speed on the ground (cm/s^2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Run")
	float GroundAcceleration = 6000.f;

	/** Acceleration when the input points against the current velocity (turning around). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Run")
	float GroundTurnAcceleration = 11000.f;

	/** Deceleration on the ground without input. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Run")
	float GroundBraking = 5000.f;

	/** Deceleration while faster than RunSpeed (after a dash or a push) and still holding that direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Run")
	float GroundOverspeedDeceleration = 1800.f;

	// ------------------------------------------------------------------ Air
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Air")
	float AirAcceleration = 4500.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Air")
	float AirTurnAcceleration = 7000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Air")
	float AirBraking = 1300.f;

	/** How fast extra speed (from a dash or a push) bleeds off in the air. Low = long momentum jumps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Air")
	float AirOverspeedDeceleration = 500.f;

	// ------------------------------------------------------------------ Jump
	/** Height of a full (held) jump. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump", meta = (ForceUnits = "cm"))
	float JumpHeight = 290.f;

	/** Time from take-off to the apex of a full jump. Smaller = snappier and heavier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump", meta = (ForceUnits = "s"))
	float JumpTimeToApex = 0.36f;

	/** Height gained by the double jump (from zero vertical speed). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump", meta = (ForceUnits = "cm"))
	float DoubleJumpHeight = 230.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump")
	int32 MaxAirJumps = 1;

	/** Upward speed is cut to this when the jump button is released early (short hop). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump")
	float JumpCutSpeed = 550.f;

	/** Jump pads (an icicle stuck in a wall): landing on one, or walking onto it, throws you up this fast - about 3.4x a
	 *  full jump (985 cm: twice the 490 cm it was at 2100) - with the double jump and the air dash back. Everybody
	 *  bounces, the thrower too. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump")
	float JumpPadSpeed = 2970.f;

	/** Gravity multiplier while falling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump")
	float FallGravityMultiplier = 1.6f;

	/** Gravity multiplier near the apex while jump is held (a little hang time). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump")
	float ApexGravityMultiplier = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump")
	float ApexSpeedThreshold = 220.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump")
	float MaxFallSpeed = 2200.f;

	/** Jump still allowed this long after walking off a ledge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump", meta = (ForceUnits = "s"))
	float CoyoteTime = 0.1f;

	/** A jump pressed this long before landing still happens on landing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump", meta = (ForceUnits = "s"))
	float JumpBufferTime = 0.13f;

	/** However short the press, letting go cuts the jump only after this long (a tap is a short hop, never a 30 cm blip). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump", meta = (ForceUnits = "s"))
	float MinJumpHoldTime = 0.035f;

	/** Falling with ground at most this far below the feet (about to land, or just stepped down a block), a jump press is a
	 *  ground jump and the double jump stays for later. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump", meta = (ForceUnits = "cm"))
	float GroundJumpReach = 60.f;

	/** A jump pressed while stunned by a hit or frozen waits (at most this long) and comes out as soon as you can move again. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump", meta = (ForceUnits = "s"))
	float StunJumpBufferTime = 0.6f;

	/** Horizontal speed (fraction of RunSpeed) given when double jumping against your current direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Jump")
	float DoubleJumpRedirect = 0.75f;

	// ------------------------------------------------------------------ Fast fall
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|FastFall")
	float FastFallStartSpeed = 1100.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|FastFall")
	float FastFallMaxSpeed = 3000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|FastFall")
	float FastFallGravityMultiplier = 2.3f;

	/** Fast fall kicks in once the vertical speed is below this (near or after the apex). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|FastFall")
	float FastFallTriggerSpeed = 250.f;

	// ------------------------------------------------------------------ Dash
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Dash")
	float DashSpeed = 1900.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Dash", meta = (ForceUnits = "s"))
	float DashDuration = 0.14f;

	/** Speed kept along the dash direction when the dash ends. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Dash")
	float DashExitSpeed = 700.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Dash", meta = (ForceUnits = "s"))
	float DashCooldown = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Dash")
	int32 MaxAirDashes = 1;

	/** Dash in 8 directions (otherwise only left/right). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Dash")
	bool bEightWayDash = true;

	/** A ground dash can be cancelled into a jump after this long. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Dash", meta = (ForceUnits = "s"))
	float DashJumpCancelTime = 0.04f;

	// ------------------------------------------------------------------ Ice
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Ice")
	FName IceTag = TEXT("Ice");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Ice")
	float IceAccelerationScale = 0.28f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Ice")
	float IceBrakingScale = 0.1f;

	/** Ice gun feet (on every floor): speeding up, turning and the slow-down above running speed get this on top of
	 *  IceAccelerationScale (0.28 x 0.43: about an eighth of the grip on snow - it takes ~1.2 m to stop by steering against). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Ice")
	float SlipperyAccelerationScale = 0.43f;

	/** Ice gun feet: the slow-down without input gets this on top of IceBrakingScale (0.1 x 0.3: let go at a run and you
	 *  glide about ten blocks - off the island unless you steer against it). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Ice")
	float SlipperyBrakingScale = 0.3f;

	/** Ice gun feet: the stick does nothing; on the ground the penguin only loses this much speed per second (cm/s^2: almost no
	 *  friction - from a run it glides about 14 s), in the air none. Speed above its running speed still wears off as usual. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Ice")
	float SlipperyGroundFriction = 40.f;

	/** Ice gun feet: the ground friction while stunned by a hit is scaled by this (a hit sends you skating). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Ice")
	float SlipperyKnockbackFrictionScale = 0.2f;

	// ------------------------------------------------------------------ Belly slide
	/** The belly slide starts this fast (a burst that slows down). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "cm/s"))
	float BellySlideSpeed = 1300.f;

	/** How far a slide goes on snow (about six blocks): its slow-down is worked out from this, BellySlideSpeed and
	 *  BellySlideExitSpeed (1300 -> 520 cm/s over 600 cm: about 0.66 s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "cm"))
	float BellySlideDistance = 600.f;

	/** Slowed down to this, the penguin gets up and runs (or brakes) again. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "cm/s"))
	float BellySlideExitSpeed = 520.f;

	/** On ice the slide slows down only this much (0.5: twice as far). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide")
	float BellySlideIceDecelerationScale = 0.5f;

	/** From the start of one slide to the next. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "s"))
	float BellySlideCooldown = 1.f;

	/** A slide that has gone this far knocks away the penguin it runs into; a shorter one only bumps into it and stops. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "cm"))
	float BellySlideHitDistance = 300.f;

	/** The slide hit's knockback (X along the slide, Y up) when it hits at BellySlideHitSpeed; it scales with the speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide")
	FVector2D BellySlideKnockback = FVector2D(1200.f, 500.f);

	/** About the speed a slide has when it reaches BellySlideHitDistance on snow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "cm/s"))
	float BellySlideHitSpeed = 1000.f;

	/** Jumping out of a slide keeps its speed, up to this (about what a dash-jump keeps). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "cm/s"))
	float BellySlideJumpSpeed = 1100.f;

	/** In the air the slide key is a belly-flop dive: forward at least this fast... */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "cm/s"))
	float BellyDiveSpeed = 850.f;

	/** ...and down at least this fast (never up: it is no recovery move). It lands as a full slide. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "cm/s"))
	float BellyDiveDropSpeed = 650.f;

	/** A slide in the air (a dive, or off a ledge) ends after this long without ground: an ordinary fall (with control)
	 *  from then on. A dive from any jump lands well before. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "s"))
	float BellySlideMaxAirTime = 0.6f;

	/** The capsule's half height on the belly (at least its radius: then it is a ball). 30 cm makes it 60 cm tall - a
	 *  slide fits through a gap one block (100 cm) high, a grown penguin's too. The feet stay where they are. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "cm"))
	float BellySlideHalfHeight = 30.f;

	/** Under a ceiling too low to stand up a slide never ends: it goes on at least this fast until there is room (the
	 *  stick turns it around, out of a dead end). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|BellySlide", meta = (ForceUnits = "cm/s"))
	float BellySlideCrawlSpeed = 450.f;

	// ------------------------------------------------------------------ Combat
	/** Horizontal slow-down while flying from a hit (low = long flights off the island). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Combat")
	float KnockbackAirDrag = 380.f;

	/** Horizontal slow-down while stunned on the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Combat")
	float KnockbackGroundFriction = 2600.f;

	/** A frozen penguin slides like an ice cube. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Combat")
	float FrozenGroundBraking = 450.f;

	/** A frozen penguin's ice cube in the air: it hardly slows down sideways (cm/s^2), and it falls like a block of the
	 *  island (ABlockIsland::FallingBlockGravity on the way up and down, ABlockIsland::FallingBlockMaxSpeed at most). */
	static constexpr float FrozenAirDrag = 60.f;

	/** Falling speed of the belly stomp. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Combat")
	float StompSpeed = 3300.f;

	/** Hop before a stomp started on the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Combat")
	float StompHopSpeed = 1050.f;

	/** The belly stomp hangs in the air this long before it slams down (a wind-up the others can react to). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Combat", meta = (ForceUnits = "s"))
	float StompWindupTime = 0.24f;

	// ------------------------------------------------------------------ Ledge grab
	/** Out of jumps next to a ledge? Pressing jump grabs it and climbs up if its top is at most this far above the feet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Climb", meta = (ForceUnits = "cm"))
	float ClimbReachUp = 150.f;

	/** How far the wall may be from the front of the penguin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Climb", meta = (ForceUnits = "cm"))
	float ClimbReachForward = 40.f;

	/** Duration of the pull-up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Climb", meta = (ForceUnits = "s"))
	float ClimbDuration = 0.3f;

	/** Ice shell float: gravity scale and the slowest you can sink while the bubble is up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Combat")
	float FloatGravityScale = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Combat")
	float FloatMaxFallSpeed = 240.f;

	/** Ability launches never push you up faster than this (they stack with jumps otherwise). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Combat")
	float MaxAbilityLaunchSpeed = 1750.f;

	// ------------------------------------------------------------------ Input
	/** X = right, Y = up. Set every frame by the owner. */
	UFUNCTION(BlueprintCallable, Category = "Penguin|Input")
	void SetMoveInput(FVector2D Input);

	UFUNCTION(BlueprintCallable, Category = "Penguin|Input")
	void JumpPressed();

	UFUNCTION(BlueprintCallable, Category = "Penguin|Input")
	void JumpReleased();

	UFUNCTION(BlueprintCallable, Category = "Penguin|Input")
	void DashPressed();

	/** The slide key: tried in the next movement update and kept for a moment (a press during a dash comes out when it
	 *  ends, a press just before the cooldown runs out still counts). */
	UFUNCTION(BlueprintCallable, Category = "Penguin|Input")
	void BellySlidePressed();

	/**
	 * Starts a belly slide right now if it can: on the ground a burst along the stick (or the facing), in the air a
	 * belly-flop dive. Only where the movement runs (RunsMovementHere). False when it can't (cooldown, no control,
	 * sliding or dashing already, climbing).
	 */
	bool TryBellySlide();

	// ------------------------------------------------------------------ State
	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	bool IsDashing() const;

	/** On the belly: sliding along the ground, or in the air after a dive or off a ledge. Right on every machine. */
	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	bool IsBellySliding() const { return bBellySliding; }

	/** cm slid along X since this slide started (the last slide's total once it is over, until the next one starts). */
	float GetBellySlideDistance() const { return SlideTravel; }
	/** +1 right, -1 left: the way the current (or last) slide goes. */
	float GetBellySlideDirection() const { return SlideDir; }
	/** Seconds until the next slide can start (on the machine that runs the movement). */
	float GetBellySlideCooldownRemaining() const { return BellySlideCooldownLeft; }
	/** The slide key was pressed in the air: the slide is a belly-flop dive until it lands. */
	bool IsBellyDiving() const { return bBellySliding && bBellyDive; }
	/** On the belly with the small capsule (BellySlideHalfHeight): sliding, or still under a ceiling too low to stand up.
	 *  Right on every machine. */
	bool IsLyingLow() const { return bLowCapsule; }
	/** The capsule's half height standing up (cm, scaled with the size): the head is twice this above the feet, also while
	 *  lying low. */
	float GetStandingHalfHeight() const;
	/** Lying low under a ceiling too low to stand up (where the movement runs: an overlap test). */
	bool IsStuckLow() const { return bLowCapsule && !HasRoomToStand(); }
	/**
	 * Server: the slide ran into a penguin (bHit: it knocked it away, else a bump). The slider stops with a little
	 * rebound, here and on the owning machine of a remote player's penguin (only that same slide: a newer one goes on).
	 */
	void EndBellySlideOnContact(bool bHit);

	/** Pulling up over a ledge (ledge grab). */
	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	bool IsClimbing() const;

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	bool IsFastFalling() const { return bFastFalling; }

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	bool IsOnIce() const { return bOnIce; }

	/** +1 facing right, -1 facing left. */
	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	float GetFacing() const { return Facing; }

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	FVector2D GetMoveInput() const { return MoveInput; }

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	int32 GetAirJumpsLeft() const { return AirJumpsLeft; }

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	int32 GetAirDashesLeft() const { return AirDashesLeft; }

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	float GetDashCooldownRemaining() const { return DashCooldownLeft; }

	float GetJumpVelocity() const { return JumpVelocity; }

	/** Clears every transient movement state (used on respawn). */
	void ResetMovementState();

	// ------------------------------------------------------------------ Combat API
	/** Replaces the velocity (X right, Y up) and takes control away for StunTime seconds. */
	void ApplyKnockback(const FVector2D& NewVelocity, float StunTime);
	/** Call right after ApplyKnockback: the flight ends with a thud where it comes down - the landing keeps only a small
	 *  part of the sideways speed (a fish hook throw lands behind its thrower instead of skidding on across the island).
	 *  Any other knockback before the landing cancels it. */
	void StickNextLanding();

	/** Frozen penguins can't act and slide like ice cubes. 0 thaws. */
	void SetFrozen(float Seconds);

	/** Belly stomp: hop (on the ground) then slam down. False if not possible right now (see CanUseAbilities). */
	bool StartStomp();

	/**
	 * Flap (the ability, PinguTuning::Flap*): the flippers beat and the penguin climbs straight up at FlapSpeedShare of the jump
	 * pad's speed until it is FlapFallHeightShare (from the ground FlapGroundHeightShare) of the pad's height above where it
	 * began - a fall is caught at once - then coasts to the top under gravity. Not while it can't use abilities, is stunned or a
	 * tornado pulls it (bFromServer: the server checked - a LAN player's own machine just flies it). On the server for a remote
	 * player's penguin it is sent to the owner, and the host's move judge allows that one climb. Ends with anything that takes
	 * over (a jump, a dash, a slide, a hit, a push up, landing, no control).
	 */
	bool StartFlap(bool bFromServer = false);
	bool IsFlapping() const { return bFlapping; }
	/** Tests: the last flap's planned height (cm) and climb speed (cm/s), and whether it began on the ground. */
	float GetLastFlapHeight() const { return LastFlapHeight; }
	float GetLastFlapSpeed() const { return FlapSpeed; }
	bool WasLastFlapFromGround() const { return bLastFlapFromGround; }

	/** Fly straight toward a point with almost no gravity for Duration (fish hook pull). */
	void LaunchPlanar(const FVector2D& NewVelocity, float LowGravityTime);

	/** No input is accepted (countdown, round over). Physics keep running. */
	void SetInputLocked(bool bLocked);

	/** Knocked out: no control at all. */
	void SetKnockedOut(bool bKO);

	/**
	 * CONTRACT (v2, LAN): teleport + new velocity for gameplay code (APenguinCharacter::NetTeleport calls this).
	 * On the server for a remote player's penguin it is also sent to the owning client (whose simulation owns the
	 * position). NewVelocity: X right, Z up. bForceFalling: leave the ground. bKeepSlide: a belly slide goes on from there
	 * (through a portal); otherwise it ends.
	 */
	void NetTeleportPenguin(const FVector& Location, const FVector& NewVelocity, bool bForceFalling, bool bKeepSlide = false);

	/** Turn toward an aim direction (abilities), unless dashing. */
	void FaceDirection(float DirectionX);

	/** Gives back the double jump and air dash (warp, ice ledge). */
	void RefreshAirMoves();

	/**
	 * Ability movement (recoil, lunge, frost jet, stomp bounce): adds Impulse (X right, Y up) to the
	 * velocity WITHOUT taking control away. With bCancelFall an upward impulse first stops any falling, so it
	 * always gives height. LowGravityTime adds a short floaty moment.
	 */
	void AddAbilityImpulse(const FVector2D& Impulse, float LowGravityTime = 0.f, bool bCancelFall = true);

	/**
	 * Adds Delta (X right, Y up) to the velocity as it is: no conditions, no mode change (a hit's nudge on a frozen
	 * penguin, the push of a status ray). Use this instead of writing Velocity on another penguin: on the server it
	 * also reaches the owner of a remote player's penguin.
	 */
	void NudgeVelocity(const FVector2D& Delta);

	/** Ice shell: sink slowly (capped fall speed) for Seconds while airborne. 0 ends it. */
	void SetFloat(float Seconds);
	bool IsFloating() const { return FloatLeft > 0.f; }

	/**
	 * The tornado's user (APinguTornado): hangs perfectly still for Seconds where it is now - in the air or on the ground: no
	 * gravity, no movement, no control (HasNoControl) and no ability (CanUseAbilities). A knockback, a freeze, a fish hook's
	 * pull or a knock-out ends it, so does 0. On the server for a remote player's penguin it reaches the owner too.
	 */
	void SetHover(float Seconds);
	bool IsHovering() const { return HoverLeft > 0.f; }
	float GetHoverRemaining() const { return HoverLeft; }
	/**
	 * Hanging in a tornado's eye (SetHover), it drifts to To (its actor's place; X and Z) in Seconds, at an even speed (two
	 * tornadoes merging: their users drift together into the giant's eye). Walls stop it on the way; the hover goes on there.
	 * Nothing without a hover; a new hover starts still. On the server for a remote player's penguin it reaches the owner too.
	 */
	void SetHoverGlide(const FVector& To, float Seconds);
	bool IsHoverGliding() const { return HoverLeft > 0.f && HoverGlideLeft > 0.f; }
	/**
	 * Sucked in by a tornado for Seconds: dragged toward Center (cm/s: PinguTuning::TornadoPullEdgeSpeed at Radius, up to
	 * TornadoPullNearSpeed near it), lifted off its feet a little, and it stops at EyeRadius from Center. No control meanwhile
	 * (HasNoControl); abilities still work (a stomp breaks it, so does any knockback, a hook's pull or a knock-out). It runs
	 * where the movement runs: on the server for a remote player's penguin it is sent to the owner. 0 ends it.
	 */
	void SetTornadoPull(const FVector& Center, float Seconds, float Radius, float EyeRadius);
	bool IsTornadoPulled() const { return PullLeft > 0.f; }
	/** The velocity the tornado's pull is after right now (where the movement runs; zero without a pull). */
	FVector GetTornadoPullVelocity() const;

	/**
	 * Second Chance's throw out of the sea: the capsule passes through the island's blocks (every ABlockIsland: their collision
	 * is ignored, floors too) until its feet are above AboveFeetZ - or it stops rising, a knockback or a knock-out comes, or
	 * MaxSeconds are over. Then the blocks count again, and should it be inside one then it is put on the nearest free spot
	 * (it never ends inside a block). Runs where the movement runs: on the server for a remote player's penguin it is sent to
	 * the owner (after the throw's knockback and teleport: the effects arrive in order); every other machine follows the
	 * owner's flag (FPenguinNetMoveState::bPassThroughIsland). 0 seconds ends it.
	 */
	void SetPassThroughIsland(float AboveFeetZ, float MaxSeconds);
	/** Passing through the island's blocks right now (SetPassThroughIsland). Right on every machine. */
	bool IsPassingThroughIsland() const { return bPassThroughIsland; }
	/** Tests: how often a pass through the island ended with the capsule inside a block and it was put on a free spot, and how
	 *  far it was moved the last time (cm). */
	int32 GetPassThroughNudges() const { return PassThroughNudges; }
	float GetLastPassThroughNudge() const { return LastPassThroughNudge; }

	/** The jump button is held right now (stomp bounce height). */
	bool IsJumpHeld() const { return bJumpHeld; }

	/** Jump presses and what became of them since the last reset (diagnostics). */
	const FPenguinJumpStats& GetJumpStats() const { return JumpStats; }
	void ResetJumpStats() { JumpStats = FPenguinJumpStats(); }

	/** On the ground, or dashing along it (a ground dash is a custom movement mode, not "walking"). */
	bool IsGroundedForAbilities() const { return IsMovingOnGround() || (IsDashing() && bDashFromGround); }

	/** Ice gun: every floor is slippery ice for Seconds (the longer of the current and the new time). */
	void SetSlippery(float Seconds);
	bool IsSlippery() const { return SlipperyLeft > 0.f; }
	float GetSlipperyRemaining() const { return SlipperyLeft; }

	/** Shrink / grow ray: big penguins run faster and jump higher, small ones are slower. */
	void SetSizeScale(float Scale);
	float GetSpeedScale() const { return SpeedScale; }

	// ------------------------------------------------------------------ the numbers in effect (Traits mode)
	// The UPROPERTYs above are BP_Penguin's tuning and never change in the game. What the movement uses is these getters:
	// the tuning with the penguin's trait on top (UPenguinTraitComponent; without a trait exactly the tuning). Right on every
	// machine: the trait replicates, so the owner's simulation and the proxies' extrapolation use the same numbers. The size
	// (GetSpeedScale, the jump scale) comes on top where the movement applies it.
	/** The trait's numbers (neutral without a trait). */
	const FPinguTraitNumbers& GetTraitNumbers() const;
	/** Air jumps after the ground jump: MaxAirJumps, or the trait's (Heavyweight 0, Featherweight 2). */
	int32 GetMaxAirJumpsNow() const;
	/** Top running speed without the size: RunSpeed x the trait. */
	float GetEffectiveRunSpeed() const;
	float GetEffectiveDashSpeed() const;
	float GetEffectiveDashExitSpeed() const;
	/** Grip on the ground (x on the ground's speeding up, turning and overspeed slow-down; on top of the ice's) and its
	 *  braking without input. */
	float GetGripScale() const;
	float GetGripBrakeScale() const;
	/** Air acceleration, turning and braking x this. */
	float GetAirControlScale() const;
	/** The trait's jump speed scale (ground jump, double jump, jump pads; the size's comes on top). */
	float GetTraitJumpScale() const;
	/** Height of a full (held) jump now, cm: the trait and the size. */
	float GetEffectiveJumpHeight() const;
	float GetEffectiveFallGravityMultiplier() const;
	/** The fastest fall (not a fast fall): MaxFallSpeed, or the trait's (Featherweight 950). */
	float GetEffectiveMaxFallSpeed() const;
	float GetEffectiveClimbReachUp() const;
	float GetEffectiveClimbReachForward() const;
	/** The slide key does something (Sticky: never). */
	bool CanBellySlideNow() const;
	/** Out of jumps next to a ledge, jump grabs it (Rubber: never). */
	bool CanGrabLedge() const;
	/** Sticky: stuck to a wall right now - it touched one in the air on the way down and hangs there (no slide, no time
	 *  limit) until the stick presses away from it, a jump, or a hit; down held it slides down at PinguTraitTuning::
	 *  StickyClingSlideSpeed (IsWallSliding). Right on every machine (it replicates with the move state). */
	bool IsWallClinging() const { return bWallClinging; }
	/** Sticky: stuck to a wall with down held - sliding down it slowly. Right on every machine (the stick replicates). */
	bool IsWallSliding() const;
	/** Sticky: the side of the wall it is stuck to (+1 right, -1 left; 0 while it is not stuck). */
	float GetClingWallDir() const { return bWallClinging ? ClingWallDir : 0.f; }
	/** Sticky: a wall right beside the penguin it can jump off (where the movement runs): OutWallDir +1 when it is on the
	 *  right. Prefers the wall it clings to, then the one the stick points at, then the one it faces. False without the trait. */
	bool FindStickyWall(float& OutWallDir) const;

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	bool IsStunned() const { return StunLeft > 0.f; }

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	bool IsFrozen() const { return FrozenLeft > 0.f; }

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	bool IsStomping() const { return bStomping || bStompHop || bStompWindup; }
	/** Hanging in the air before the slam. */
	bool IsStompWindingUp() const { return bStompWindup; }

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	bool IsKnockedOut() const { return bKnockedOut; }

	UFUNCTION(BlueprintPure, Category = "Penguin|State")
	bool IsInputLocked() const { return bInputLocked; }

	/** True when the penguin can't be controlled right now (a tornado's user hanging in its eye, or sucked in by one, too). */
	bool HasNoControl() const
	{
		return bKnockedOut || bInputLocked || FrozenLeft > 0.f || StunLeft > 0.f || bStomping || bStompHop || bStompWindup || HoverLeft > 0.f
			|| PullLeft > 0.f;
	}

	/**
	 * Abilities can be used right now (UPenguinAbilityComponent::UseAbility, on the server). Not while frozen, dizzy from a
	 * slap, knocked out, input locked (intro, countdown, round over), in our own belly stomp or hanging in our own tornado's
	 * eye (SetHover). Everything else is fine: an ordinary hit stun (the knockback flight), a dash, a belly slide, lying low,
	 * a ledge climb, the air, another penguin's tornado sucking us in.
	 * The slap's dizziness is known where the gameplay runs (the server); on a LAN client it is always off here.
	 */
	bool CanUseAbilities() const
	{
		return !bKnockedOut && !bInputLocked && FrozenLeft <= 0.f && SlapDizzyLeft <= 0.f && !bStomping && !bStompHop && !bStompWindup && HoverLeft <= 0.f;
	}

	/**
	 * Server: the slap's stun makes the penguin dizzy for Seconds - no ability at all until it is over (an ordinary hit stun
	 * leaves abilities alone). It runs down like the stun; a knock-out or a round reset ends it. Gameplay only (abilities
	 * run on the server), so it is not sent to a LAN player's own machine.
	 */
	void SetSlapDizzy(float Seconds) { SlapDizzyLeft = bKnockedOut ? 0.f : FMath::Max(SlapDizzyLeft, Seconds); }
	bool IsSlapDizzy() const { return SlapDizzyLeft > 0.f; }
	float GetSlapDizzyRemaining() const { return SlapDizzyLeft; }

	float GetStunRemaining() const { return StunLeft; }
	float GetFrozenRemaining() const { return FrozenLeft; }
	float GetAirTime() const { return TimeSinceGrounded; }

	int32 HitEvents = 0;
	float LastHitSpeed = 0.f;
	int32 StompLandEvents = 0;
	float LastStompLandSpeed = 0.f;

	/** Event counters: incremented when the event happens. Animation/FX compare against the last value they saw. */
	int32 JumpEvents = 0;
	int32 AirJumpEvents = 0;
	int32 DashEvents = 0;
	int32 LandEvents = 0;
	int32 ClimbEvents = 0;
	int32 PadBounceEvents = 0;
	float LastLandSpeed = 0.f;
	bool bLastJumpFromRun = false;
	/** Belly slides started (the slide's FX, sound and squash on every machine; the animation reads it too). */
	int32 SlideEvents = 0;
	/** The last slide started with ice gun feet: the animation slips onto the belly instead of diving onto it. */
	bool bLastSlideWasSlip = false;
	/** Rubber: bounces off the ground (every landing whose bounce - PinguTraitTuning::RubberBounceHeight of the height fallen
	 *  - leaves at RubberMinSpeed or more) and off walls (in a knockback flight). Right on every machine (the boing, the look). */
	int32 BounceEvents = 0;
	/** Rubber, where the movement runs: the last floor bounce - the height it fell from (cm, the top of the fall to the
	 *  landing) and the speed it was sent back up with (cm/s). Tests. */
	float LastBounceFallHeight = 0.f;
	float LastBounceUpSpeed = 0.f;
	/** Sticky: wall jumps (they count as jumps too: JumpEvents). Right on every machine. */
	int32 WallJumpEvents = 0;

	// ------------------------------------------------------------------ Networking (LAN)
	/**
	 * This machine runs the penguin's movement: its owning client, the server for the host's own penguin, bots and
	 * unpossessed penguins, or solo play. Elsewhere (simulated proxies, the server copy of a remote player's penguin)
	 * the members mirror the replicated state and the movement logic (jumps, timers, counters) does not run.
	 */
	bool RunsMovementHere() const;

	/** The server's copy of a remote player's penguin: its owner simulates it, effects called here are forwarded to it. */
	bool IsRemotePlayerCopy() const;

	/** Component tag of jump pads (APenguinProjectile's icicle pegs). */
	static const FName JumpPadTag;
	static bool IsJumpPad(const UPrimitiveComponent* Component);

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	/** The server: the move state and status go out as they are at the send, like the actor's own movement (a hit landed after
	 *  this component's tick - a tornado's throw, a blast - would otherwise reach the clients one update after the flight). */
	virtual void PreReplication(IRepChangedPropertyTracker& ChangedPropertyTracker) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	/** Never simulates a remote player's penguin forward on the server (its owner is authoritative for the position). */
	virtual bool ForcePositionUpdate(float DeltaTime) override;

	// ------------------------------------------------------------------ the host's say over a remote player's penguin
	/** The limits the host judges this penguin's own moves by (its tuning, size and trait; PinguHostAuthority.h). */
	FPinguMoveLimits GetHostMoveLimits() const;
	/** Server copy of a remote player's penguin: the host moves it itself right now (a stun, freeze, pull, hover, stomp, a
	 *  teleport on its way, or the owner's moves were refused). */
	bool IsHostOwned() const { return bHostOwnedNow; }
	/** cm/s a belly slide hits with: the velocity; on a remote player's copy at most what the host measured (+ room). */
	float GetSlideHitSpeed() const;
	/** Tests: the host's judge of this copy. */
	const FPinguMoveJudge& GetHostJudge() const { return HostJudge; }

protected:
	virtual void InitializeComponent() override;
	virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;
	virtual float GetGravityZ() const override;
	virtual FVector NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const override;
	virtual void PhysCustom(float DeltaTime, int32 Iterations) override;
	virtual float GetMaxSpeed() const override;
	virtual void ProcessLanded(const FHitResult& Hit, float RemainingTime, int32 Iterations) override;
	/** An ice shell's bubble pushed into a wall bounces off it (applied after the move: ShellBounceVelocityX). */
	virtual void HandleImpact(const FHitResult& Hit, float TimeSlice = 0.f, const FVector& MoveDelta = FVector::ZeroVector) override;
	/** Server copy of a remote player's penguin: keeps the move bookkeeping (time stamps, acks, the pose tick) but does not simulate. */
	virtual void MoveAutonomous(float ClientTimeStamp, float DeltaTime, uint8 CompressedFlags, const FVector& NewAccel) override;
	/** Never: a delayed move gets combined with the next one, which rewinds and replays both (see the .cpp). */
	virtual bool CanDelaySendingMove(const FSavedMovePtr& NewMove) override;
	/** Server copy of a remote player's penguin: the owner's position is taken (no error check) - unless it is a teleport or an
	 *  impossible run on the ground the host did not cause (PinguAntiCheat::AcceptMove): then the host keeps its own, and
	 *  moves the owner back after a while. */
	using UCharacterMovementComponent::ServerMoveHandleClientError;
	virtual void ServerMoveHandleClientError(float ClientTimeStamp, float DeltaTime, const FVector& Accel, const FVector& RelativeClientLocation,
		FMovementBaseInterfaceData* ClientMovementBaseInterfaceData, FName ClientBaseBoneName, uint8 ClientMovementMode) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Owner -> server: velocity and movement state of the owner's simulation (unreliable, about every frame). */
	UFUNCTION(Server, Unreliable)
	void Rq16(const FPenguinMoveSync& Sync);

	/** Server -> owner: an effect the server applied to its copy of this penguin (reliable, in order). */
	UFUNCTION(Client, Reliable)
	void Rq17(const FPenguinNetEffect& Effect);

	UFUNCTION()
	void OnRep_NetState();

	UFUNCTION()
	void OnRep_NetStatus();

	UFUNCTION()
	void OnRep_NetSizeScale();

	/** Movement state for everybody but the owner (from the owner's syncs, or the server's own simulation). */
	UPROPERTY(ReplicatedUsing = OnRep_NetState)
	FPenguinNetMoveState NetState;

	/** Server-authoritative status for everybody but the owner. */
	UPROPERTY(ReplicatedUsing = OnRep_NetStatus)
	FPenguinNetStatus NetStatus;

	/** Size (SetSizeScale) for everybody, the owner too: the character calls SetSizeScale every frame while the size animates. */
	UPROPERTY(ReplicatedUsing = OnRep_NetSizeScale)
	float NetSizeScale = 1.f;

	FPenguinNetMoveState CaptureNetState() const;
	/** bOwnerSimulated: also velocity-related flags and facing (not while server effects are waiting for the owner). */
	void RestoreNetState(const FPenguinNetMoveState& State, bool bOwnerSimulated);
	/** Simulated proxy: timers from the replicated end times, hits, knock-out, input lock. */
	void ApplyNetStatus();
	/** Server: NetState / NetStatus from the members (replication sends what changed). */
	void PublishNetState();
	/** Owning client: send the movement state when it changed (and now and then anyway). */
	void SendMoveSync(float DeltaTime);
	/** Server copy of a remote player's penguin: timers, the teleport guard, smoothing of the host's view. */
	void TickRemoteCopy(float DeltaTime);
	/** Penguins this machine does not simulate tick after their owner, so our network smoothing of the mesh comes last. */
	void UpdateNetTickOrder();
	/** Server, remote player's penguin: send an effect to the owner and remember it until the owner confirms it. */
	void ForwardEffect(FPenguinNetEffect& Effect, bool bHard, const FVector& VelocityDelta = FVector::ZeroVector);
	/** Server, remote player's penguin: ignore the owner's positions (and state) until it has arrived at Target too. */
	void ArmNetPositionGuard(const FVector& From, const FVector& Target, bool bUntilAck, uint8 AckSeq);
	void ReleaseNetPositionGuard(const TCHAR* Why);
	/** The server's clock (synchronised on clients). False on a client that does not know it yet (no game state). */
	bool GetNetServerTime(float& OutTime) const;
	/** Proxies: carry on along the replicated velocity in the custom modes (dash, climb). */
	void PhysProxyCustom(float DeltaTime);

	void RecomputeJumpPhysics();
	bool TryJump();
	void LaunchJump(bool bAirJump);
	bool TryStartDash();
	void EndDash(bool bIntoJump);
	void PhysDash(float DeltaTime, int32 Iterations);
	bool TryStartClimb();
	void PhysClimb(float DeltaTime, int32 Iterations);
	void BeginStompWindup(float Seconds);
	bool IsFloorIce() const;
	/** Clears the slide state (a jump, a dash, a hit, ... took over). The velocity is left alone. */
	void StopBellySlide();
	/** cm/s^2 on snow: makes a slide from BellySlideSpeed down to BellySlideExitSpeed take BellySlideDistance. */
	float GetBellySlideDeceleration() const;
	/** CalcVelocity while sliding: slow down along the slide (less on ice), get up at the exit speed; no steering. */
	void CalcBellySlideVelocity(float DeltaTime, bool bAir);
	/** Landing while sliding: a dive becomes the full slide, a slide off a ledge goes on with its speed. */
	void LandBellySlide();
	/** Stops the slide with a little rebound (EndBellySlideOnContact; on the owner: the slide Serial only). */
	void ApplyBellySlideContact(int32 Serial, bool bHit);
	/**
	 * The small belly capsule (bLow) or the standing one. bKeepFeet: the feet stay where they are (the middle goes down /
	 * up: where the movement runs; standing up only after HasRoomToStand). Without it the capsule changes around its
	 * middle (machines that mirror an owner's positions: they come lowered already). The mesh stays where it was.
	 */
	void SetLowCapsule(bool bLow, bool bKeepFeet);
	/** Standing up from lying low would fit here (an overlap test of the standing capsule, the feet kept). */
	bool HasRoomToStand(FVector* OutStandLocation = nullptr) const;
	/** Where the movement runs, before every move: lie low while sliding, stand up again once there is room. */
	void UpdateLowCapsule();
	/** On a jump pad (see JumpPadSpeed): up you go. Runs where the movement runs (RunsMovementHere). */
	void BounceOffJumpPad();
	/** The capsule touches an icicle stuck in the ground (APenguinProjectile::IsGroundPad): walking into it is a jump pad. */
	bool IsTouchingGroundPad() const;
	/** Falling onto walkable ground that is at most GroundJumpReach below the feet (another penguin does not count). */
	bool IsNearGroundForJump() const;
	/** A buffered jump press ran out without a jump: count it (and log why for the player). */
	void NoteDroppedJump();
	/** The penguin's trait is this one (replicated: right on every machine). */
	bool HasTraitNow(EPinguTrait Trait) const;
	/** A wall (not a penguin) right beside the capsule on side Dir (+1 right, -1 left), within Reach cm of its side (At: the
	 *  capsule there instead of where it is). */
	bool IsWallBeside(float Dir, float Reach, const FVector* At = nullptr) const;
	/** Sticky, where the movement runs, before every move: stick to a wall touched in the air on the way down (and stay stuck
	 *  until the stick presses away from it). */
	void UpdateWallCling(float DeltaSeconds, bool bControl);
	/** Sticky: a jump press next to a wall - up over a ledge in reach, else a wall jump away from it. */
	bool TryWallJump();
	/** Rubber, where the movement runs: the bounce off the ground of the landing being processed (ProcessLanded): back up to
	 *  RubberBounceHeight of the height fallen from RubberFallTopZ. */
	bool TryRubberLandingBounce(float VelocityXBefore, bool bWasStomping, bool bWasSticking);
	/** Where the movement runs, before every move: the end of a pass through the island (SetPassThroughIsland). */
	void UpdatePassThrough(float DeltaSeconds);
	/** Ends a pass through the island; bNudge (where the movement runs): a capsule inside a block goes to the nearest free spot. */
	void EndPassThrough(bool bNudge);
	/** The capsule ignores (or no longer ignores) every island's blocks when it moves. */
	void ApplyPassThroughIgnore(bool bIgnore);
	/** Rubber, where the movement runs: a wall hit in a knockback flight bounces off it (applied after the move). */
	void HandleRubberWallImpact(const FHitResult& Hit);
	/** After every move where the movement runs: a bubble's or Rubber's wall bounce, the slide's way, jump pads. */
	void PostMoveSteps(float DeltaTime);

	// The host's say over a remote player's penguin (PinguHostAuthority.h)
	/** The host's copy is in a state the host decides alone (no control: stun, freeze, pull, hover, stomp). */
	bool IsHostEffectOwned() const;
	/** The host simulates its copy for one frame (no input): the knockback flight, the ice cube, the fall of a refused penguin. */
	void HostSimulate(float DeltaTime, double Now);
	/** The host finds walkable ground right under a capsule at Location (or, Sticky, a wall beside it). */
	bool IsHostSupported(const FVector& Location) const;
	/** A jump pad (a stuck icicle) right there. */
	bool IsNearJumpPad(const FVector& Location) const;
	/** The host knows something lifts or floats the penguin right now: its height is not judged. */
	bool IsHostFreeVertical() const;
	/** Location is on the host's own path of the last moment (as long ago as the owner's ping makes its moves late). */
	bool IsNearHostPath(const FVector& Location, double Now) const;
	/** The old gate (PinguAntiCheat::AcceptMove) and the judge; false = refused (logged, counted). */
	bool JudgeOwnerMove(const FVector& ClientLoc, bool bSupported, double Now, FString& OutWhy);
	/** Refused: the host keeps its copy and simulates it from here on (until the owner is back on its path). */
	void StartHostPenalty(const FString& Why, double Now);
	/** The owner's slides, landings and stomp landings: only what the host counts (Rq16). */
	void ApplyOwnerRequests(const FPenguinMoveSync& Sync, bool bCurrent);
#if !UE_BUILD_SHIPPING
	/** Dev builds, a LAN client's own penguin: the -PinguCheat movement cheats of the net test (PinguHostAuthority::HasDevCheat). */
	void DevCheatOwnerTick(float DeltaTime);
#endif

	// Input
	FVector2D MoveInput = FVector2D::ZeroVector;
	bool bJumpHeld = false;
	float JumpBufferLeft = 0.f;
	/** Time left for a press made during a stun / freeze to wait for control (see StunJumpBufferTime). */
	float StunJumpHoldLeft = 0.f;
	/** Since the last jump took off (the jump cut waits for MinJumpHoldTime). */
	float TimeSinceJumpLaunch = 10.f;
	FPenguinJumpStats JumpStats;
	bool bDashRequested = false;
	float DashBufferLeft = 0.f;
	bool bSlideRequested = false;
	float SlideBufferLeft = 0.f;
	bool bDownHeldLastFrame = false;

	// State
	float Facing = 1.f;
	float TimeSinceGrounded = 0.f;
	bool bCoyoteAvailable = false;
	bool bRisingFromJump = false;
	bool bJumpCutDone = false;
	bool bApexHangAllowed = false;
	bool bFastFallArmed = false;
	bool bFastFalling = false;
	int32 AirJumpsLeft = 1;
	int32 AirDashesLeft = 1;
	float DashCooldownLeft = 0.f;
	float DashElapsed = 0.f;
	FVector DashDirection = FVector(1.f, 0.f, 0.f);
	bool bDashFromGround = false;
	bool bOnIce = false;

	// Belly slide (bBellySliding, SlideTravel and the counters replicate in NetState)
	bool bBellySliding = false;
	/** Started in the air: a belly-flop dive until it lands. */
	bool bBellyDive = false;
	/** +1 right, -1 left. */
	float SlideDir = 1.f;
	/** cm along X since the slide started. */
	float SlideTravel = 0.f;
	/** X at the last travel update. */
	float SlideLastX = 0.f;
	float SlideElapsed = 0.f;
	/** Seconds the slide has been in the air (see BellySlideMaxAirTime). */
	float SlideAirTime = 0.f;
	float BellySlideCooldownLeft = 0.f;
	/** The small belly capsule is on (see SetLowCapsule; replicates in NetState). */
	bool bLowCapsule = false;
	/** The standing capsule's half height without the size scale (the character's, from InitializeComponent). */
	float StandingHalfHeight = 0.f;

	// Combat state
	float StunLeft = 0.f;
	/** The slap's dizzy stun (SetSlapDizzy): no abilities while it lasts. Server gameplay only. */
	float SlapDizzyLeft = 0.f;
	float FrozenLeft = 0.f;
	float LowGravityLeft = 0.f;
	/** A ground icicle threw us up a moment ago: it does not throw again while we are still in it. */
	float GroundPadWait = 0.f;
	float FloatLeft = 0.f;
	float SlipperyLeft = 0.f;
	/** The tornado: hanging still in its eye (SetHover), or sucked in by one (SetTornadoPull: its middle, reach and eye). */
	float HoverLeft = 0.f;
	/** SetHoverGlide: where the hover drifts to, the seconds left of it, and this move's velocity (zero when not gliding). */
	FVector HoverGlideTo = FVector::ZeroVector;
	float HoverGlideLeft = 0.f;
	FVector HoverGlideVelocity = FVector::ZeroVector;
	float PullLeft = 0.f;
	FVector PullCenter = FVector::ZeroVector;
	float PullRadius = 1.f;
	float PullEye = 0.f;
	/** StickNextLanding: the next landing keeps only a little of the sideways speed. */
	bool bStickNextLanding = false;
	/** HandleImpact: the bubble hit a wall this move; after the move it flies off it at this X speed. */
	bool bShellBouncePending = false;
	float ShellBounceVelocityX = 0.f;
	/** Seconds an ability push (AddAbilityImpulse) still counts as recent: an ice shell's bubble pushed into a wall bounces. */
	float PushedLeft = 0.f;
	float SpeedScale = 1.f;
	float JumpScale = 1.f;
	/** Sticky: stuck to a wall (IsWallClinging), the side of the wall (+1 right), and the moment after a wall jump that the
	 *  stick can't steer back into the wall. */
	bool bWallClinging = false;
	float ClingWallDir = 0.f;
	float WallJumpLockLeft = 0.f;
	/** Rubber: HandleImpact hit a wall in a flight this move; after the move it flies off it at this X speed. */
	bool bRubberBouncePending = false;
	float RubberBounceVelocityX = 0.f;
	/** Rubber: the highest the capsule's middle got since it left the ground (or last bounced): the top of the fall. */
	float RubberFallTopZ = 0.f;
	bool bRubberFallTracked = false;
	/** Second Chance's throw: passing through the island's blocks (SetPassThroughIsland) until the feet are above this
	 *  height, for at most PassThroughLeft more seconds; whether the capsule ignores the islands right now; the tests' count
	 *  of nudges out of a block at its end. */
	bool bPassThroughIsland = false;
	float PassThroughAboveZ = 0.f;
	float PassThroughLeft = 0.f;
	bool bPassThroughIgnoring = false;
	int32 PassThroughNudges = 0;
	float LastPassThroughNudge = 0.f;
	float StompHopTime = 0.f;
	bool bStomping = false;
	bool bStompHop = false;
	bool bStompWindup = false;
	float StompWindupLeft = 0.f;
	/** Flap (StartFlap): flapping (climbing, then coasting: bFlapCoast), its top, speed, the seconds it may still take. */
	bool bFlapping = false;
	bool bFlapCoast = false;
	float FlapTopZ = 0.f;
	float FlapSpeed = 0.f;
	float FlapLeft = 0.f;
	float LastFlapHeight = 0.f;
	bool bLastFlapFromGround = false;
	/** Where the movement runs, before every move: the climb, the coast, the end. */
	void UpdateFlap(float DeltaSeconds);
	/** The gravity of a jump (cm/s^2, no trait or state): what the flap's height is worked out with. */
	float GetJumpGravity() const;

	// Ledge grab
	FVector ClimbStart = FVector::ZeroVector;
	FVector ClimbMid = FVector::ZeroVector;
	FVector ClimbEnd = FVector::ZeroVector;
	float ClimbElapsed = 0.f;
	float ClimbDir = 1.f;
	bool bKnockedOut = false;
	bool bInputLocked = false;

	// Derived from JumpHeight / JumpTimeToApex
	float JumpVelocity = 1600.f;
	float AirJumpVelocity = 1400.f;

	// Networking: the owning client of a player's penguin
	uint16 NetSyncSeq = 0;
	/** The last forwarded effect applied here (sent back with every sync). */
	uint8 NetEffectAck = 0;
	float NetSyncAge = 0.f;
	bool bNetSyncSent = false;
	/** An effect was just applied: tell the server at once. */
	bool bNetSyncUrgent = false;
	FPenguinMoveSync NetLastSentSync;

	// Networking: the server copy of a remote player's penguin
	uint8 NetEffectSeq = 0;
	/** Real time (FPlatformTime) of the last effect sent to the owner: the anti-cheat does not judge its moves for a while. */
	double NetLastEffectTime = -100.0;
	uint16 NetLastSyncSeq = 0;
	bool bNetHasSync = false;
	TArray<FPenguinPendingNetEffect> NetPendingEffects;
	bool bNetPositionGuard = false;
	bool bNetGuardUntilAck = false;
	uint8 NetGuardAckSeq = 0;
	FVector NetGuardFrom = FVector::ZeroVector;
	FVector NetGuardTarget = FVector::ZeroVector;
	float NetGuardTimeLeft = 0.f;
	// Networking: the host's say over a remote player's penguin (PinguHostAuthority.h)
	FPinguMoveJudge HostJudge;
	FPinguSlideGate HostSlideGate;
	FPinguLandGate HostLandGate;
	/** HostSimulate is running: RunsMovementHere() for that frame's move. */
	bool bHostSimulating = false;
	/** This frame: the host simulated its copy (the post-move steps ran there), and the frame's timers were run by the move. */
	bool bHostSimulatedThisFrame = false;
	bool bHostTimersRun = false;
	/** The host moves its copy right now (IsHostOwned), because of an effect, and whether that just ended (hand-back). */
	bool bHostOwnedNow = false;
	bool bHostOwnedByEffect = false;
	bool bHandbackPending = false;
	/** What the host's last stretch of its own moves was (the hand-back's log): its reasons, since when (real time). */
	FString HostOwnedWhy;
	double HostOwnedSince = 0.0;
	/** The owner's moves were refused: the host has it until the owner is back on the host's path (pulled back after a while). */
	bool bHostPenalty = false;
	double HostPenaltySince = -1.0;
	double HostLastPullBack = -100.0;
	/** Real time of the owner's last move; it went quiet (the host moves its penguin meanwhile). */
	double HostLastOwnerMove = -1.0;
	bool bHostQuiet = false;
	/** Where the copy was, the last second (IsNearHostPath). */
	TArray<FPenguinHostPathPoint> HostPath;
	/** The owner's counters as it last sent them (its requests: a change is a new slide / landing / stomp landing). */
	bool bOwnerCountersSeen = false;
	int32 OwnerSlideSeen = 0;
	int32 OwnerLandSeen = 0;
	int32 OwnerStompSeen = 0;
	/** A slide the host counted is on: when it began, its way so far (cm, the host's measure), X at the last measure. */
	bool bHostSlideActive = false;
	double HostSlideStart = 0.0;
	float HostSlideTravel = 0.f;
	float HostSlideLastX = 0.f;
	/** Real time of the host's own last stomp landing (a claim right after it is that landing, not a fake). */
	double HostLastStompLand = -100.0;
	/** Seconds of Second Chance's pass through the island the host started; seconds its height stays free after a lift ended. */
	float HostPassThroughLeft = 0.f;
	float HostFreeAfter = 0.f;
#if !UE_BUILD_SHIPPING
	/** Dev cheats on a LAN client's own penguin (DevCheatOwnerTick). */
	float DevCheatClock = 0.f;
	float DevFlyLeft = 0.f;
	float DevFlyDir = 1.f;
	float DevPulseClock = 0.f;
	float DevLandClock = 0.f;
	float DevHopClock = 0.f;
	float DevFlapClock = 0.f;
	int32 DevFlapsLeft = 0;
	int32 DevSlideClaims = 0;
#endif
	/** Sum of the owner's move times (the engine's ServerAccumulatedClientTimeStamp): time stamps for linear smoothing. */
	double NetClientMoveTime = 0.0;
	/** Move time not yet given to the animation (ticked once per frame). */
	float NetPendingPoseTime = 0.f;
	/** Where the copy was at the last tick (a new position from the owner starts the host's smoothing). */
	FVector NetSmoothedLocation = FVector::ZeroVector;
	bool bNetSmoothedLocationValid = false;

	// Networking: every machine
	bool bNetTickAfterOwner = false;

	/** The client -> server move data (see FPenguinNetworkMoveData). */
	FPenguinNetworkMoveDataContainer PenguinMoveDataContainer;

	/** The penguin's trait (GetTraitNumbers), found when the component starts. */
	UPROPERTY(Transient)
	TObjectPtr<UPenguinTraitComponent> TraitComponent;
	/** Looks the trait component up on the owner (InitializeComponent, BeginPlay). */
	void FindTraitComponent();
};
