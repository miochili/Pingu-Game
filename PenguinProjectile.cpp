// Pingu - thrown things and the falling iceberg.
//
// LAN: only the server runs the gameplay (flight + collisions, hits, interactions, pegs, island edits, the hook's pull,
// bubbles knocked along); the effects of that gameplay go out inside FPinguFXBroadcastScope, once per event. Clients
// only draw: they fly their copy on with the replicated velocity (gravity, walls and bomb bounces included) and blend
// the server's corrections out, and they rebuild icicle pegs from replicated numbers so their own penguin (which each
// client moves itself) can stand on them. Purely visual per-frame parts (spin, glow trails, fuse sparks, trail puffs,
// the hook line from the thrower's flipper) run on every machine by itself and are never broadcast.

#include "PenguinProjectile.h"
#include "PinguGiantSnowball.h"
#include "PinguSnowman.h"
#include "PinguBuildLimit.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinMovementComponent.h"
#include "PenguinTraitComponent.h"
#include "PinguFX.h"
#include "PinguPickup.h"
#include "PinguSettings.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"

namespace PenguinProjectilePrivate
{
	static UStaticMesh* LoadMesh(const TCHAR* Path, const TCHAR* Fallback)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
		return Mesh ? Mesh : LoadObject<UStaticMesh>(nullptr, Fallback);
	}

	constexpr float PegLifetime = PinguTuning::PegLifetime; // a stuck icicle (a wall peg or a ground pad) lasts this long
	constexpr float GroundPadThickness = 16.f; // a ground pad's icicle is this thick around its line (x its size): touching it counts
	constexpr float IcicleMeshScale = 1.3f; // thickness; SM_Icicle is 100 cm long (tip +X)
	constexpr float IcicleLengthScale = 1.75f; // 175 cm long (was 130): a longer spear, and a longer jump pad in a wall
	constexpr float PegLength = 110.f;    // how far a peg (a jump pad, UPenguinMovementComponent::JumpPadSpeed) sticks out of the wall
	constexpr float PegIcicleOut = PegLength - 50.f * IcicleLengthScale; // icicle centre out of the wall: its thick end is the peg's outer end
	constexpr float PegIcicleDrop = 13.f; // icicle axis this far below the peg top (its upper surface is the peg top)
	constexpr float PegThickness = 12.f;
	constexpr float IcicleEmissive = 0.6f;  // the icicle's steady glow (a peg about to break blinks brighter)
	// The snow bomb's blast (x its size): knocked out inside the kill radius, pushed away up to the push radius; the
	// crater takes the blocks within the crater radius. Its blast zone shows the first two while the fuse burns.
	constexpr float BombKillRadius = PinguTuning::BombKillRadius;
	constexpr float BombPushRadius = PinguTuning::BombPushRadius;
	constexpr float BombCraterRadius = PinguTuning::BombCraterRadius;
	/** The blast zone's ring: this thick (cm), pulsing this often (1/s) when lit and just before it goes off. */
	constexpr float ZoneRingWidth = 12.f;
	constexpr float ZonePulseSlow = 2.2f;
	constexpr float ZonePulseFast = 9.f;
	static const TCHAR* BlastZoneMaterialPath = TEXT("/Game/FX/M_BlastZone.M_BlastZone");
	constexpr float OwnShotGrace = 0.15f;   // your own shots ignore each other while one of them is younger than this
	constexpr float BombBounce = 0.75f;     // bombs bumping into each other keep this much of the swapped speed
	constexpr float PickupPushSpeed = 700.f; // a shot flying through an ability bubble knocks it along this fast (cm/s)
	constexpr float BlastPickupPush = 1200.f;
	constexpr float HookReelSpeed = 7200.f; // a hook on its way back to the flipper (cm/s)

	// The fish hook throws what it catches over its thrower's head.
	constexpr float HookThrowBehind = PinguTuning::HookThrowBehind; // lands this far behind the thrower (x the thrower's size, 0.75 .. 1.25)
	constexpr float HookThrowClearance = 30.f;    // the thrown thing passes at least this far above the thrower's head
	constexpr float HookThrowMaxSpeedX = 3200.f;  // a faster throw (a far catch) goes higher instead
	constexpr float HookThrowMinRise = 80.f;      // even a catch from above hops up a little first
	constexpr float HookThrowMaxRise = 1500.f;
	constexpr float HookThrowStunExtra = 0.12f;   // the victim stays stunned this long past the flight
	constexpr float HookHaulMinTime = 0.25f;      // the hook rides on its victim at least this long (it may not have left the ground yet)
	constexpr float HookYankSpeed = 1700.f;       // icicles (flying or pulled out of a wall) come back at the thrower this fast
	constexpr float HookBombFuseExtra = 0.35f;    // a hooked bomb goes off this long after it lands behind the thrower
	constexpr float HookFlingLifeExtra = 0.3f;    // a flung snowball lives at least this long past its landing
	// Heavy things the fish hook yanks - a frozen penguin in its ice cube, an ice block pulled out of the island - come only
	// about halfway to the thrower (a penguin flies over its head), on a low lob, falling like a block of the island.
	constexpr float HookHeavyShare = 0.5f;
	constexpr float HookHeavyRisePerCm = 0.25f;   // the top of the lob: this much above the start per cm it comes sideways ...
	constexpr float HookHeavyMinRise = 60.f;      // ... at least this ...
	constexpr float HookHeavyMaxRise = 320.f;     // ... at most this,
	constexpr float HookHeavyClearLanding = 40.f; // and at least this far above where it comes down
	constexpr float HookHeavyGroundSearch = 1400.f; // where it comes down: the ground below there, searched this far down
	constexpr float HookIceHoldExtra = 0.5f;      // a yanked ice cube stays frozen at least this long past the lob's flight
	// The ice block pulled out of the island.
	constexpr float HookedIceSkin = 3.f;          // its box is this much smaller than a cell on each side (fits a one-block gap)
	constexpr float HookedIceMaxStep = 40.f;      // flown in steps of at most this (cm): it never skips a block
	constexpr float HookedIceHitSpeed = 750.f;    // a penguin in its way is knocked aside this fast ...
	constexpr float HookedIceHitLift = 380.f;     // ... and up,
	constexpr float HookedIceHitStun = 0.45f;     // and stunned this long
	constexpr float HookedIceLinger = 0.3f;       // landed on a block, it stays drawn this long (the island's copy reaches the clients)
	constexpr float HookedIceMaxLife = 5.f;
	constexpr float HookedIceNetErrorDecay = 12.f; // a client's correction fades at this rate (1/s)
	constexpr float HookedIceIgnoreFromCellTime = 0.5f; // a client's copy flies through the block it came out of this long
	// The gust blows ice blocks out of the island (AHookedIceBlock::BlowInArea): away from the wind's middle, falling like
	// blocks of the island - a few blocks away on the same level (about 4.5 m near the wind's middle).
	constexpr float BlownIceSpeed = 1400.f;       // away from the wind's middle (cm/s) ...
	constexpr float BlownIceEdgeShare = 0.6f;     // ... this share of it at the edge of the wind ...
	constexpr float BlownIceLift = 1100.f;        // ... plus this much straight up
	constexpr float BlownIceMinSide = 0.45f;      // right above or below the wind's middle it still goes this much sideways
	constexpr float BlownIceLandVolume = 0.3f;    // many land at once: each one's thud is quiet (and no shake)
	// The gust blows icicle pegs and ground pads out (BlowOut): the icicle flies off as the blower's, heavy (it drops).
	constexpr float BlownIcicleSpeed = 1500.f;
	constexpr float BlownIcicleMinUp = 0.3f;      // its way out points at least this much up
	// Hooking the ground pulls the thrower there.
	constexpr float HookPullSpeedPerCm = 2.2f;
	constexpr float HookPullMinSpeed = 900.f;
	constexpr float HookPullMaxSpeed = 3000.f;    // the line reaches twice as far as it used to: the pull is faster too
	constexpr float HookPullMaxLowGravity = 1.f;
	constexpr float HookPullLift = 0.18f;         // a bit of lift so the thrower lands on top rather than under the edge

	constexpr float IceGunSlipTime = PinguTuning::IceGunSlipTime; // seconds of slippery feet from an ice bolt (x the bolt's size, 0.5 .. 2)
	constexpr float IceInside = 20.f;             // a hit's ice block is this far behind the impact point

	// Frost.
	constexpr float FrozenGravity = 1400.f;       // a frozen snowball (or icicle) is heavy: it drops
	constexpr float IcicleFrostSpeed = 0.5f;      // frost slows a flying icicle to this share of its speed (once)
	constexpr float PegRefreezeQuiet = 0.3f;      // a peg refrozen less than this long ago sparkles no more (frost: every frame)
	constexpr float AreaMouth = 30.f;             // right at the mouth of a cone everything is in it (any direction)

	// Frost holds a falling iceberg in the air.
	constexpr float IcebergFrostHold = 0.9f;      // each touch of a frost breath (FreezeInArea) holds it at least this long
	constexpr float IcebergHeldSparkleTime = 0.08f; // a frosty sparkle on it this often while it hangs (x 1 / effects density)

	// A belly slide kicks bombs along.
	constexpr float BombKickSpeed = 950.f;        // at least this fast ...
	constexpr float BombKickOverSpeed = 1.2f;     // ... or this much faster than the slider
	constexpr float BombKickUp = 380.f;
	constexpr float BombKickReach = 6.f;          // the slider's capsule this close to the bomb
	constexpr float BombKickMinSlideSpeed = 50.f; // slower than this: kicked the way the slider faces
	constexpr float BombKickCooldown = 0.3f;

	// The iceberg falls (cm/s^2, cm/s): 1.6 times the gravity it had (it comes down from its 11 m in 0.56 s instead of
	// 0.71 s). Its warning before the fall is as long as ever (WarnLeft), so it stays fair.
	constexpr float IcebergGravity = 7040.f;
	constexpr float IcebergMaxFallSpeed = 8000.f;
	constexpr float IcebergDropHeight = 1100.f;   // it appears this far above where it lands
	// What it does to the penguins it meets on the way down (PressPenguins).
	constexpr float IcebergTouchMargin = 6.f;     // a capsule reaching less than this far into its box is not touched
	constexpr float IcebergCarryGap = 1.f;        // a penguin under it is kept this far under its bottom while it pushes it down ...
	constexpr float IcebergPressTolerance = 2.f;  // ... and crushed once ground is within this of where its feet would have to go
	constexpr float IcebergShoveSpeed = 380.f;    // a penguin at its side is shoved out of the way at least this fast (cm/s) ...
	constexpr float IcebergShoveHop = 180.f;      // ... with this little hop when it stood on the ground
	constexpr float IcebergShoveGap = 2.f;        // gap (cm) between the shoved penguin's capsule and the ice
	constexpr float IcebergNudgeDistance = 15.f;  // set aside less than this (cm): no shove, no bonk
	constexpr float IcebergShoveMaxDrop = 300.f;  // shoved from the ground only onto ground at most this far down (no pit, no sea)
	constexpr float IcebergPushAhead = 1.1f;      // flying sideways into a penguin, it pushes it along this much faster than itself
	// Wind blows a falling iceberg away (Blow).
	constexpr float IcebergBlowSpeed = 1300.f;    // sideways, cm/s (x the wind's strength) ...
	constexpr float IcebergBlowLift = 450.f;      // ... and this much up (it stops falling for a moment)
	constexpr float IcebergBlowMinAimX = 0.25f;   // an aimed blast more upright than this blows it away from the blast's middle
	constexpr float IcebergBlowCenterBand = 20.f; // right above the middle of the wind: it goes the way the blower faces
	constexpr float IcebergColumnInset = 2.f;     // its box is this much inside its edges against the island's cells (touching is fine)
	constexpr float IcebergSeaSurface = 50.f;     // blown off over the sea it splashes in here above SeaLevel (the sea surface)
	constexpr float IcebergNoFloe = -1.e8f;       // FloeTopZ below this: not dropped over the sea (no floe height)
	constexpr float IcebergLandingRecheck = 0.5f; // where it lands is looked up again once it moved this far sideways (cm)
	constexpr float IcebergNetSteadyX = 60.f;     // clients: a sideways speed this close to ours is the same flight (timing only)
	constexpr float IcebergNetSnap = 400.f;       // clients: a correction further than this snaps instead of fading

	/** LAN: a finished shot stays (hidden) on the server this long, so its final state - finished, right where it hit -
	 *  reaches every client before the actor goes (destroying it at once could pop it out there before it arrives). */
	constexpr float NetFinishLinger = 0.25f;
	/** Clients: how fast a correction from the server fades out (1/s), and beyond which distance it just snaps. */
	constexpr float NetErrorDecay = 12.f;
	constexpr float NetSnapDistance = 300.f;
	/** Clients: while a shot keeps its heading and speed, the part of a correction along its path is mostly timing
	 *  (when that update left the server, when this frame picked it up): it fades slowly, so fast shots fly steadily
	 *  instead of wobbling at the update rate. Beyond this much flight time it is a real error and fades fast. */
	constexpr float NetAlongDecay = 2.5f;
	constexpr float NetAlongTimingLimit = 0.06f;

	/** Fades a client's drawn offset: across the path (or after a real change) fast, along a steady path slowly. */
	static FVector FadeNetError(const FVector& Error, const FVector& Velocity, bool bSteady, float Dt)
	{
		const float Fast = FMath::Exp(-NetErrorDecay * Dt);
		const float Speed = static_cast<float>(Velocity.Size());
		if (!bSteady || Speed < 1.f)
		{
			return Error * Fast;
		}
		const FVector Dir = Velocity / Speed;
		const float Along = static_cast<float>(FVector::DotProduct(Error, Dir));
		const FVector Across = Error - Dir * Along;
		const float AlongFade = FMath::Abs(Along) > NetAlongTimingLimit * Speed ? Fast : FMath::Exp(-NetAlongDecay * Dt);
		return Dir * (Along * AlongFade) + Across * Fast;
	}

	static bool IsNetClient(const UWorld* World)
	{
		return World && World->GetNetMode() == NM_Client;
	}

	static bool IsSizeRay(EPenguinProjectileKind Kind)
	{
		return Kind == EPenguinProjectileKind::ShrinkRay || Kind == EPenguinProjectileKind::GrowRay;
	}

	/** Collision radius of a kind at size 1 (cm). */
	static float BaseRadius(EPenguinProjectileKind Kind)
	{
		switch (Kind)
		{
		case EPenguinProjectileKind::Snowball: return 24.f;
		case EPenguinProjectileKind::Icicle: return 17.f;
		case EPenguinProjectileKind::Bomb: return 30.f;
		case EPenguinProjectileKind::FishHook: return 26.f;
		case EPenguinProjectileKind::IceBolt: return 17.f;
		case EPenguinProjectileKind::ShrinkRay:
		case EPenguinProjectileKind::GrowRay: return 22.f;
		}
		return 18.f;
	}

	static float BaseMeshScale(EPenguinProjectileKind Kind);

	/** Mesh scale of a kind at size 1 (the icicle is stretched along its length, X). */
	static FVector BaseMeshScale3D(EPenguinProjectileKind Kind)
	{
		if (Kind == EPenguinProjectileKind::Icicle)
		{
			return FVector(IcicleLengthScale, IcicleMeshScale, IcicleMeshScale);
		}
		return FVector(BaseMeshScale(Kind));
	}

	/** Uniform mesh scale of a kind at size 1. */
	static float BaseMeshScale(EPenguinProjectileKind Kind)
	{
		switch (Kind)
		{
		case EPenguinProjectileKind::Snowball: return 0.55f;
		case EPenguinProjectileKind::Icicle: return IcicleMeshScale;
		case EPenguinProjectileKind::Bomb: return 0.8f;
		case EPenguinProjectileKind::FishHook: return 0.75f;
		case EPenguinProjectileKind::IceBolt: return 0.85f;
		case EPenguinProjectileKind::ShrinkRay:
		case EPenguinProjectileKind::GrowRay: return 0.45f;
		}
		return 1.f;
	}

	/** Half size of a peg box for a projectile size. */
	static FVector PegExtent(float Scale)
	{
		return FVector(PegLength * 0.5f * Scale, 60.f, PegThickness * 0.5f * Scale);
	}

	/** A penguin's capsule is in (or right at) a peg box there, on the play plane. */
	static bool IsInPegBox(const APenguinCharacter* Penguin, const FVector& Center, const FVector& Extent)
	{
		const FVector Loc = Penguin->GetActorLocation();
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		return FMath::Abs(Loc.X - Center.X) < Extent.X + Capsule->GetScaledCapsuleRadius() + 2.f
			&& FMath::Abs(Loc.Z - Center.Z) < Extent.Z + Capsule->GetScaledCapsuleHalfHeight() + 2.f;
	}

	/** This machine's particle density (lower effect settings spawn fewer trail particles). */
	static float FXDensity()
	{
		return FMath::Clamp(UPinguSettings::GetFXDensity(), 0.1f, 1.f);
	}

	/** A ball (Reach cm) at Point is in the area on the play plane: within AreaRadius of Origin and, for a cone
	 *  (CosHalfAngle > -1), inside its angle - or right at its mouth. */
	static bool InArea(const FVector& Point, float Reach, const FVector& Origin, float AreaRadius, const FVector& ConeDirection, float CosHalfAngle)
	{
		FVector D = Point - Origin;
		D.Y = 0.f;
		const float Dist = static_cast<float>(D.Size());
		if (Dist > AreaRadius + Reach)
		{
			return false;
		}
		return CosHalfAngle <= -1.f || Dist <= AreaMouth || FVector::DotProduct(D / Dist, ConeDirection) >= CosHalfAngle;
	}

	/** The segment From..To on the play plane (X, Z) runs into the box (slab test). */
	static bool SegmentHitsBox(const FVector2D& From, const FVector2D& To, const FVector2D& BoxMin, const FVector2D& BoxMax)
	{
		double Enter = 0.0;
		double Leave = 1.0;
		const FVector2D Delta = To - From;
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			const double Start = Axis == 0 ? From.X : From.Y;
			const double Step = Axis == 0 ? Delta.X : Delta.Y;
			const double Low = Axis == 0 ? BoxMin.X : BoxMin.Y;
			const double High = Axis == 0 ? BoxMax.X : BoxMax.Y;
			if (FMath::Abs(Step) < UE_KINDA_SMALL_NUMBER)
			{
				if (Start < Low || Start > High)
				{
					return false; // runs alongside the box, outside it
				}
				continue;
			}
			const double T0 = (Low - Start) / Step;
			const double T1 = (High - Start) / Step;
			Enter = FMath::Max(Enter, FMath::Min(T0, T1));
			Leave = FMath::Min(Leave, FMath::Max(T0, T1));
			if (Enter > Leave)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * A box on the play plane (X, Z) touches the area (see InArea): the circle reaches it and, for a cone, a corner of the
	 * box is inside the cone or one of a few rays across the cone (its edges, its middle and halfway between) runs into
	 * it - close enough for a breath of frost.
	 */
	static bool BoxInArea(const FVector2D& BoxMin, const FVector2D& BoxMax, const FVector& Origin, float AreaRadius, const FVector& ConeDirection, float CosHalfAngle)
	{
		const FVector2D From(Origin.X, Origin.Z);
		const FVector2D Nearest(FMath::Clamp(From.X, BoxMin.X, BoxMax.X), FMath::Clamp(From.Y, BoxMin.Y, BoxMax.Y));
		const float NearDist = static_cast<float>(FVector2D::Distance(From, Nearest));
		if (NearDist > AreaRadius)
		{
			return false;
		}
		if (CosHalfAngle <= -1.f || NearDist <= AreaMouth)
		{
			return true;
		}
		for (const FVector2D& Corner : { BoxMin, BoxMax, FVector2D(BoxMin.X, BoxMax.Y), FVector2D(BoxMax.X, BoxMin.Y) })
		{
			if (InArea(FVector(Corner.X, 0.f, Corner.Y), 0.f, Origin, AreaRadius, ConeDirection, CosHalfAngle))
			{
				return true;
			}
		}
		const FVector2D Axis = FVector2D(ConeDirection.X, ConeDirection.Z).GetSafeNormal();
		const float HalfAngle = FMath::Acos(FMath::Clamp(CosHalfAngle, -1.f, 1.f));
		for (const float Share : { -1.f, -0.5f, 0.f, 0.5f, 1.f })
		{
			const float Angle = HalfAngle * Share;
			const float CosA = FMath::Cos(Angle);
			const float SinA = FMath::Sin(Angle);
			const FVector2D Ray(Axis.X * CosA - Axis.Y * SinA, Axis.X * SinA + Axis.Y * CosA);
			if (SegmentHitsBox(From, From + Ray * AreaRadius, BoxMin, BoxMax))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * A bomb meeting the island bounces and rolls (the server and the clients' copies use the same rule, on the same
	 * island). Returns false if it started inside the ground: OutLocation steps it out and the velocity stays.
	 */
	static bool BounceBomb(const FHitResult& Hit, FVector& InOutVelocity, FVector& OutLocation, FVector& OutNormal, bool& bOutRest)
	{
		const FVector N = FVector(Hit.ImpactNormal.X, 0.f, Hit.ImpactNormal.Z).GetSafeNormal();
		OutNormal = N;
		bOutRest = false;
		if (Hit.bStartPenetrating)
		{
			// Started inside the ground (grown while touching it): step out and keep the velocity.
			const FVector Out = N.IsNearlyZero() ? FVector::UpVector : N;
			OutLocation = Hit.Location + Out * (FMath::Max(static_cast<float>(Hit.PenetrationDepth), 0.f) + 1.f);
			return false;
		}
		const FVector Normal = N * FVector::DotProduct(InOutVelocity, N);
		const FVector Tangent = InOutVelocity - Normal;
		InOutVelocity = Tangent * 0.7f - Normal * 0.42f;
		OutLocation = Hit.Location + N * 1.f;
		if (InOutVelocity.Size() < 160.f && N.Z > 0.7f)
		{
			InOutVelocity = FVector::ZeroVector;
			bOutRest = true;
		}
		return true;
	}

	static UMaterialInterface* PropMaterial()
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Prop.M_Prop"), nullptr, LOAD_Quiet | LOAD_NoWarn);
		return Material ? Material : LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}

	// ------------------------------------------------------------------ over-the-head throws (the fish hook)

	/** How a thrown thing moves through the air (all positive). */
	struct FArcPhysics
	{
		float GravityUp = 980.f;    // cm/s^2 while rising
		float GravityDown = 980.f;  // cm/s^2 while falling (penguins fall heavier than they rise)
		float MaxFallSpeed = 0.f;   // cm/s, 0 = no limit
		float DragX = 0.f;          // cm/s^2 of sideways slow-down (a stunned penguin's air drag)
	};

	/** Seconds to fall Drop cm from the apex. */
	static float ArcFallTime(const FArcPhysics& P, float Drop)
	{
		if (Drop <= 0.f)
		{
			return 0.f;
		}
		if (P.MaxFallSpeed > 0.f)
		{
			const float CapTime = P.MaxFallSpeed / P.GravityDown;
			const float CapDrop = 0.5f * P.MaxFallSpeed * CapTime;
			if (Drop > CapDrop)
			{
				return CapTime + (Drop - CapDrop) / P.MaxFallSpeed;
			}
		}
		return FMath::Sqrt(2.f * Drop / P.GravityDown);
	}

	/** Height above the start Time seconds into an arc whose apex is Rise above the start. */
	static float ArcHeight(const FArcPhysics& P, float Rise, float Time)
	{
		const float UpTime = FMath::Sqrt(2.f * Rise / P.GravityUp);
		if (Time <= UpTime)
		{
			return P.GravityUp * UpTime * Time - 0.5f * P.GravityUp * Time * Time;
		}
		const float T = Time - UpTime;
		float Fall = 0.5f * P.GravityDown * T * T;
		if (P.MaxFallSpeed > 0.f)
		{
			const float CapTime = P.MaxFallSpeed / P.GravityDown;
			if (T > CapTime)
			{
				Fall = 0.5f * P.MaxFallSpeed * CapTime + P.MaxFallSpeed * (T - CapTime);
			}
		}
		return Rise - Fall;
	}

	/** Seconds until a throw at SpeedX (slowed by the drag) has come Distance cm sideways (a huge number if never). */
	static float ArcTimeAt(const FArcPhysics& P, float SpeedX, float Distance)
	{
		if (Distance <= 0.f)
		{
			return 0.f;
		}
		if (P.DragX <= UE_KINDA_SMALL_NUMBER)
		{
			return Distance / FMath::Max(SpeedX, 1.f);
		}
		const float Disc = SpeedX * SpeedX - 2.f * P.DragX * Distance;
		return Disc < 0.f ? 1.e6f : (SpeedX - FMath::Sqrt(Disc)) / P.DragX;
	}

	/**
	 * An over-the-head throw, in coordinates along the throw (X from the start toward the thrower, Z up from the start):
	 * the lowest arc that stays above ClearZ from ClearFromX to ClearToX (the thrower's head, both sides of it) and comes
	 * down at (LandX, LandZ) - higher ones while it would be faster sideways than MaxSpeedX. False if even the highest
	 * arc tried does not clear (OutVelocity / OutTime then hold that highest one).
	 */
	static bool SolveOverheadArc(const FArcPhysics& P, float ClearFromX, float ClearToX, float ClearZ, float LandX, float LandZ,
		float MaxSpeedX, FVector2D& OutVelocity, float& OutTime)
	{
		constexpr float RiseStep = 25.f;
		// The apex is above the head and above the landing spot.
		const float FirstRise = FMath::Max3(HookThrowMinRise, ClearZ + 5.f, LandZ + 5.f);
		OutVelocity = FVector2D::ZeroVector;
		OutTime = 0.f;
		for (float Rise = FirstRise; Rise <= FMath::Max(HookThrowMaxRise, FirstRise); Rise += RiseStep)
		{
			const float UpTime = FMath::Sqrt(2.f * Rise / P.GravityUp);
			const float Time = UpTime + ArcFallTime(P, Rise - LandZ);
			if (Time <= UE_KINDA_SMALL_NUMBER)
			{
				continue;
			}
			// Sideways speed that reaches LandX in Time (the drag takes 0.5 * Drag * Time^2 off).
			const float SpeedX = (LandX + 0.5f * P.DragX * Time * Time) / Time;
			OutVelocity = FVector2D(SpeedX, P.GravityUp * UpTime);
			OutTime = Time;
			if (P.DragX > UE_KINDA_SMALL_NUMBER && SpeedX / P.DragX < Time)
			{
				continue; // the drag would stop it short (only very high arcs over a short way)
			}
			// A concave arc is lowest over the thrower at one of the two ends of the stretch it must clear.
			const bool bClears = (ClearFromX <= 0.f || ArcHeight(P, Rise, ArcTimeAt(P, SpeedX, ClearFromX)) >= ClearZ)
				&& ArcHeight(P, Rise, ArcTimeAt(P, SpeedX, ClearToX)) >= ClearZ;
			if (bClears && SpeedX <= MaxSpeedX)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * A heavy thing's lob (the fish hook yanking an ice cube or an ice block), in coordinates along the throw (X from the
	 * start toward the thrower, Z up from the start): low - its top rises HookHeavyRisePerCm per cm it goes sideways,
	 * within HookHeavyMinRise..HookHeavyMaxRise and at least HookHeavyClearLanding above the landing - coming down at
	 * (LandX, LandZ). Returns the launch velocity; OutTime the flight.
	 */
	static FVector2D SolveHeavyLob(const FArcPhysics& P, float LandX, float LandZ, float& OutTime)
	{
		const float Rise = FMath::Max(FMath::Clamp(LandX * HookHeavyRisePerCm, HookHeavyMinRise, HookHeavyMaxRise), LandZ + HookHeavyClearLanding);
		const float UpTime = FMath::Sqrt(2.f * Rise / FMath::Max(P.GravityUp, 1.f));
		OutTime = UpTime + ArcFallTime(P, Rise - LandZ);
		// Sideways speed that comes LandX in OutTime (the drag takes 0.5 * Drag * Time^2 off).
		const float SpeedX = OutTime > UE_KINDA_SMALL_NUMBER ? (FMath::Max(LandX, 0.f) + 0.5f * P.DragX * OutTime * OutTime) / OutTime : 0.f;
		return FVector2D(SpeedX, P.GravityUp * UpTime);
	}

	// ------------------------------------------------------------------ the iceberg

	/** A falling iceberg dropped by Owner passes through this penguin instead of pushing it down: its owner, an ice shell,
	 *  the round winner (ApplyPenguinHit would not knock any of them out). They end up on top of the ice when it lands
	 *  (LiftOnTop). Friendly fire: the owner's teammates are crushed too. (Its ice falling on down a lower column after
	 *  the landing is the island's falling sand: ABlockIsland::ShovePenguinsOutOfSand spares the same ones.) */
	static bool IsSparedByIceberg(const APenguinCharacter* Penguin, const APenguinCharacter* Owner)
	{
		return Penguin == Owner || Penguin->IsShielded() || Penguin->IsVictorious();
	}

	/** The falling iceberg presses a penguin against the ground: knocked out, flung out sideways. False if that did not
	 *  knock it out (an ice shell, the round winner). (Its ice falling on down a lower column knocks out the same way:
	 *  ABlockIsland's CrushUnderSand with bIcebergIce.) */
	static bool CrushPenguin(APenguinCharacter* Penguin, APenguinCharacter* Owner, float MiddleX)
	{
		const FVector Loc = Penguin->GetActorLocation();
		FPenguinHit Hit;
		Hit.Instigator = Owner;
		Hit.Source = EPenguinAbility::IcebergDrop;
		Hit.SourceSize = 1.f; // falling ice has no size of its own: whoever dropped it, it goes by the victim's size alone
		Hit.bDeadly = true;
		Hit.Reason = EPenguinKOReason::Iceberg;
		Hit.Knockback = FVector2D(Loc.X < MiddleX ? -500.f : 500.f, 300.f);
		Hit.Location = Loc;
		return Penguin->ApplyPenguinHit(Hit);
	}

	/** A penguin the new ice would trap stands on top of it instead (lifted with a little upward speed, in the air).
	 *  NetTeleport also moves a LAN player's penguin on its own machine (whose simulation owns its position). */
	static void LiftOnTop(APenguinCharacter* Penguin, float TopZ)
	{
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const FVector OldVelocity = Move ? Move->Velocity : FVector::ZeroVector;
		const float HalfHeight = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		Penguin->NetTeleport(FVector(Penguin->GetActorLocation().X, 0.f, TopZ + HalfHeight + 2.f),
			FVector(OldVelocity.X, 0.f, FMath::Max(static_cast<float>(OldVelocity.Z), 200.f)), true);
	}

	/** The iceberg's ice: clear blue - or frosty while frost holds it in the air (paler, matt, glowing a little more). */
	static void SetIcebergIceLook(UMaterialInstanceDynamic* Material, bool bFrosty)
	{
		Material->SetVectorParameterValue(TEXT("Color"), bFrosty ? FLinearColor(0.8f, 0.95f, 1.f) : FLinearColor(0.42f, 0.85f, 1.f));
		Material->SetScalarParameterValue(TEXT("Roughness"), bFrosty ? 0.5f : 0.05f);
		Material->SetScalarParameterValue(TEXT("Emissive"), bFrosty ? 0.9f : 0.45f);
	}

	// ------------------------------------------------------------------ the fish hook

	/** Where something thrown over Thrower's head comes down: on the ground there, or level with the thrower's feet. */
	static float ThrowLandingZ(const UWorld* World, const APenguinCharacter* Thrower, float LandWorldX, float HalfHeight)
	{
		const UCapsuleComponent* Capsule = Thrower->GetCapsuleComponent();
		const FVector Center = Thrower->GetActorLocation();
		const float ThrowerHalf = Capsule->GetScaledCapsuleHalfHeight();
		if (const ABlockIsland* Island = ABlockIsland::FindNear(World, Center))
		{
			float GroundZ = 0.f;
			// From a little above the head down to well below the feet (a step down behind the thrower is fine).
			if (Island->FindGroundBelow(FVector(LandWorldX, 0.f, Center.Z + ThrowerHalf + Island->CellSize), 2.f * ThrowerHalf + 6.f * Island->CellSize, GroundZ))
			{
				return GroundZ + HalfHeight;
			}
		}
		return static_cast<float>(Center.Z) - ThrowerHalf + HalfHeight;
	}

	/** Where a heavy yanked thing (HalfHeight from its middle down to its bottom) comes down at LandWorldX: on the ground
	 *  there, found from the height of the higher of its start (From) and the thrower down; over a gap (or inside a wall)
	 *  at the lower one's height - it falls on (or hits the wall) from there. */
	static float HeavyLandingZ(const UWorld* World, float LandWorldX, const FVector& From, const APenguinCharacter* Thrower, float HalfHeight)
	{
		const FVector ThrowerAt = Thrower->GetActorLocation();
		const float ThrowerBottom = static_cast<float>(ThrowerAt.Z) - Thrower->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const float FromBottom = static_cast<float>(From.Z) - HalfHeight;
		if (const ABlockIsland* Island = ABlockIsland::FindNear(World, FVector(LandWorldX, 0.f, From.Z)))
		{
			float GroundZ = 0.f;
			if (Island->FindGroundBelow(FVector(LandWorldX, 0.f, FMath::Max(FromBottom, ThrowerBottom) + 1.f), HookHeavyGroundSearch, GroundZ))
			{
				return GroundZ + HalfHeight;
			}
		}
		return FMath::Min(FromBottom, ThrowerBottom) + HalfHeight;
	}
}

// ============================================================================ projectile

APenguinProjectile::APenguinProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	SetActorEnableCollision(false);

	// LAN: spawned by the server only and sent to everyone, often (a handful of fast shots; the engine's random update
	// delay makes 100 about one update every server frame or two).
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(100.f);
	SetMinNetUpdateFrequency(30.f);
	NetPriority = 2.f;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(true);

	Line = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Line"));
	Line->SetupAttachment(Mesh);
	Line->SetUsingAbsoluteLocation(true);
	Line->SetUsingAbsoluteRotation(true);
	Line->SetUsingAbsoluteScale(true);
	Line->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Line->SetCastShadow(false);
	Line->SetVisibility(false);
}

void APenguinProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(APenguinProjectile, Kind, COND_InitialOnly);
	DOREPLIFETIME(APenguinProjectile, Shooter);
	DOREPLIFETIME(APenguinProjectile, SizeScale);
	DOREPLIFETIME(APenguinProjectile, Gravity);
	DOREPLIFETIME(APenguinProjectile, bStuck);
	DOREPLIFETIME(APenguinProjectile, bDone);
	DOREPLIFETIME(APenguinProjectile, bRetracting);
	DOREPLIFETIME(APenguinProjectile, bAnchored);
	DOREPLIFETIME(APenguinProjectile, bResting);
	DOREPLIFETIME(APenguinProjectile, bFrozenLook);
	DOREPLIFETIME(APenguinProjectile, bPegged);
	DOREPLIFETIME(APenguinProjectile, bPegExpiring);
	DOREPLIFETIME(APenguinProjectile, bGroundPad);
	DOREPLIFETIME(APenguinProjectile, PegOutX);
	DOREPLIFETIME(APenguinProjectile, PegWallX);
	DOREPLIFETIME(APenguinProjectile, PegCenterZ);
	DOREPLIFETIME(APenguinProjectile, HookedPenguin);
	DOREPLIFETIME(APenguinProjectile, HookedIce);
	DOREPLIFETIME(APenguinProjectile, TeleportCount);
	DOREPLIFETIME(APenguinProjectile, FuseTicks);
	DOREPLIFETIME(APenguinProjectile, FuseTotalTicks);
	DOREPLIFETIME_CONDITION(APenguinProjectile, BlastScale, COND_InitialOnly);
}

APenguinProjectile* APenguinProjectile::Launch(UWorld* World, EPenguinProjectileKind InKind, APenguinCharacter* InShooter, const FVector& Location, const FVector& InVelocity)
{
	// Projectiles exist only because the server (or solo game) spawned them; clients get them by replication.
	if (!World || PenguinProjectilePrivate::IsNetClient(World))
	{
		return nullptr;
	}
	FPinguFXBroadcastScope FXScope(World); // the bomb's fuse hiss
	FVector Start = Location;
	Start.Y = 0.f;
	const FTransform SpawnTransform(InVelocity.Rotation(), Start);
	APenguinProjectile* Projectile = World->SpawnActorDeferred<APenguinProjectile>(APenguinProjectile::StaticClass(), SpawnTransform, InShooter, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Projectile)
	{
		Projectile->Setup(InKind, InShooter, InVelocity);
		Projectile->FinishSpawning(SpawnTransform);
	}
	return Projectile;
}

void APenguinProjectile::Setup(EPenguinProjectileKind InKind, APenguinCharacter* InShooter, const FVector& InVelocity)
{
	Kind = InKind;
	Shooter = InShooter;
	Velocity = FVector(InVelocity.X, 0.f, InVelocity.Z);
	// The strength of the use that threw it (Traits mode; 1 otherwise): its hits carry it.
	const UPenguinAbilityComponent* ShooterAbilities = InShooter ? InShooter->GetAbilities() : nullptr;
	ShotStrength = ShooterAbilities ? ShooterAbilities->GetUseStrength() : 1.f;
	Radius = PenguinProjectilePrivate::BaseRadius(Kind);
	switch (Kind)
	{
	case EPenguinProjectileKind::Snowball:
		Gravity = 700.f;
		Life = 1.6f;
		break;
	case EPenguinProjectileKind::Icicle:
		Gravity = 0.f;
		Life = 1.6f; // slower now: keep about the same range
		break;
	case EPenguinProjectileKind::Bomb:
		Gravity = PinguTuning::BombGravity;
		Life = 4.f;
		FuseLeft = PinguTuning::BombFuse;
		FuseTotal = FuseLeft;
		FuseTicks = static_cast<uint8>(FMath::RoundToInt(PinguTuning::BombFuse * 40.f)); // (1/40 s) arrives with the spawn
		FuseTotalTicks = FuseTicks;
		BlastScale = PinguTraitTuning::GetReachScale(ShotStrength); // (1 without traits) arrives with the spawn
		APinguFX::Sound(this, EPinguSound::BombFuse, 0.5f, 0.f);
		break;
	case EPenguinProjectileKind::FishHook:
		Gravity = 0.f;
		// The line reaches HookReach at HookSpeed (further or shorter with the throw's strength: Traits mode).
		Life = HookReach / HookSpeed * PinguTraitTuning::GetReachScale(ShotStrength);
		break;
	case EPenguinProjectileKind::IceBolt:
		Gravity = 0.f;
		Life = 0.75f;
		break;
	case EPenguinProjectileKind::ShrinkRay:
	case EPenguinProjectileKind::GrowRay:
		Gravity = 0.f;
		Life = 0.9f;
		break;
	}
	// Big penguins throw big things (and small ones small things).
	const float ShooterScale = InShooter ? InShooter->GetSizeScale() : 1.f;
	if (!FMath::IsNearlyEqual(ShooterScale, 1.f, 0.01f))
	{
		SizeScale = ShooterScale;
		Radius *= ShooterScale;
	}
	SetupLook();
	if (Kind == EPenguinProjectileKind::Icicle || Kind == EPenguinProjectileKind::FishHook || Kind == EPenguinProjectileKind::IceBolt)
	{
		SetActorRotation(Velocity.Rotation());
	}
}

void APenguinProjectile::SetupLook()
{
	using namespace PenguinProjectilePrivate;
	UMaterialInterface* Material = PropMaterial();
	MeshMaterial = UMaterialInstanceDynamic::Create(Material, this);
	Spin = 0.f;
	switch (Kind)
	{
	case EPenguinProjectileKind::Snowball:
		Mesh->SetStaticMesh(LoadMesh(TEXT("/Game/FX/SM_Snowball.SM_Snowball"), TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		Spin = 720.f;
		break;
	case EPenguinProjectileKind::Icicle:
		Mesh->SetStaticMesh(LoadMesh(TEXT("/Game/FX/SM_Icicle.SM_Icicle"), TEXT("/Engine/BasicShapes/Cone.Cone")));
		if (MeshMaterial)
		{
			MeshMaterial->SetScalarParameterValue(TEXT("Emissive"), IcicleEmissive);
		}
		break;
	case EPenguinProjectileKind::Bomb:
		Mesh->SetStaticMesh(LoadMesh(TEXT("/Game/FX/SM_SnowBomb.SM_SnowBomb"), TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		Spin = 360.f;
		break;
	case EPenguinProjectileKind::FishHook:
		Mesh->SetStaticMesh(LoadMesh(TEXT("/Game/FX/SM_Fish.SM_Fish"), TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		Line->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
		if (UMaterialInstanceDynamic* LineMaterial = UMaterialInstanceDynamic::Create(Material, this))
		{
			LineMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.08f, 0.09f, 0.12f));
			Line->SetMaterial(0, LineMaterial);
		}
		Line->SetVisibility(true);
		break;
	case EPenguinProjectileKind::IceBolt:
		Mesh->SetStaticMesh(LoadMesh(TEXT("/Game/FX/SM_Shard.SM_Shard"), TEXT("/Engine/BasicShapes/Cone.Cone")));
		if (MeshMaterial)
		{
			MeshMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.55f, 0.92f, 1.f));
			MeshMaterial->SetScalarParameterValue(TEXT("Emissive"), 1.4f);
		}
		break;
	case EPenguinProjectileKind::ShrinkRay:
	case EPenguinProjectileKind::GrowRay:
		Mesh->SetStaticMesh(LoadMesh(TEXT("/Game/FX/SM_Snowball.SM_Snowball"), TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		if (MeshMaterial)
		{
			MeshMaterial->SetVectorParameterValue(TEXT("Color"), RayColor());
			MeshMaterial->SetScalarParameterValue(TEXT("Emissive"), 3.f);
		}
		break;
	}
	Mesh->SetMaterial(0, MeshMaterial);
	Mesh->SetRelativeScale3D(BaseMeshScale3D(Kind) * SizeScale);
	if (bFrozenLook)
	{
		ApplyFrozenLook(true);
	}
}

void APenguinProjectile::ApplyFrozenLook(bool bFrozen)
{
	if (!MeshMaterial)
	{
		return;
	}
	// Only bombs, snowballs and icicles get frozen, and none is tinted in SetupLook: thawing goes back to plain white
	// (and the icicle's own steady glow).
	const float PlainEmissive = Kind == EPenguinProjectileKind::Icicle ? PenguinProjectilePrivate::IcicleEmissive : 0.f;
	MeshMaterial->SetVectorParameterValue(TEXT("Color"), bFrozen ? FLinearColor(0.6f, 0.9f, 1.f) : FLinearColor::White);
	MeshMaterial->SetScalarParameterValue(TEXT("Emissive"), bFrozen ? 0.8f : PlainEmissive);
}

void APenguinProjectile::Thaw()
{
	FrozenLeft = 0.f;
	if (bFrozenLook)
	{
		bFrozenLook = false; // replicated: the clients' copies thaw too
		ApplyFrozenLook(false);
	}
}

void APenguinProjectile::BeginPlay()
{
	Super::BeginPlay();
	LastTrailPoint = GetActorLocation();
	bTrailStarted = true;
	NetLocation = GetActorLocation();
	ShownTeleportCount = TeleportCount; // warps from before we saw it are old news
	if (!HasAuthority())
	{
		// A LAN client: draw what the server spawned and catch up with everything that came with it.
		SetupLook();
		Radius = PenguinProjectilePrivate::BaseRadius(Kind) * SizeScale;
		ShownSizeScale = SizeScale;
		bShownFrozen = bFrozenLook;
		if ((Kind == EPenguinProjectileKind::Icicle || Kind == EPenguinProjectileKind::FishHook || Kind == EPenguinProjectileKind::IceBolt) && !Velocity.IsNearlyZero())
		{
			SetActorRotation(Velocity.Rotation());
		}
		ApplyState();
	}
}

FLinearColor APenguinProjectile::RayColor() const
{
	return Kind == EPenguinProjectileKind::GrowRay ? FLinearColor(0.45f, 1.f, 0.35f) : FLinearColor(0.85f, 0.4f, 1.f);
}

void APenguinProjectile::GetAll(const UWorld* World, TArray<APenguinProjectile*>& Out)
{
	Out.Reset();
	if (!World)
	{
		return;
	}
	for (TActorIterator<APenguinProjectile> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It) && !It->bDone)
		{
			Out.Add(*It);
		}
	}
}

APenguinCharacter* APenguinProjectile::GetShooter() const
{
	// Nobody once that penguin is gone (it left the game).
	APenguinCharacter* Thrower = Shooter.Get();
	return IsValid(Thrower) ? Thrower : nullptr;
}

float APenguinProjectile::GetLifeLeft() const
{
	if (bDone)
	{
		return 0.f;
	}
	if (bStuck)
	{
		return FMath::Max(StuckLeft, 0.f); // stuck in the ground, or a peg
	}
	if (Kind == EPenguinProjectileKind::Bomb)
	{
		return FrozenLeft + FMath::Max(FuseLeft, 0.f); // bombs go off instead (a frozen fuse waits first)
	}
	return FMath::Max(Life - Age, 0.f);
}

FVector APenguinProjectile::HandLocation() const
{
	const APenguinCharacter* Thrower = GetShooter();
	return Thrower ? Thrower->GetAimOrigin() : GetActorLocation();
}

bool APenguinProjectile::Reflect(APenguinCharacter* NewOwner, const FVector& Direction, float NewSpeed)
{
	if (!HasAuthority() || bDone || bStuck || bRetracting || bAnchored || HookedPenguin || HookedIce)
	{
		return false;
	}
	FPinguFXBroadcastScope FXScope(this);
	if (Kind == EPenguinProjectileKind::FishHook)
	{
		// A batted or blocked hook lets go and reels back to its own thrower (the line stays in the thrower's hand).
		bRetracting = true;
		APinguFX::Sound(this, EPinguSound::ShieldBlock, 0.8f);
		APinguFX::Sparkles(this, GetActorLocation(), 5, 25.f, FLinearColor(1.f, 0.95f, 0.6f));
		return true;
	}
	Shooter = NewOwner;
	PushedPickups.Reset(); // a new flight
	FVector Dir = Direction;
	Dir.Y = 0.f;
	Dir = Dir.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = -Velocity.GetSafeNormal();
	}
	const float Speed = NewSpeed > 0.f ? NewSpeed : FMath::Max(static_cast<float>(Velocity.Size()), 1000.f) * 1.1f;
	Velocity = Dir * Speed;
	if (Kind == EPenguinProjectileKind::Bomb)
	{
		Velocity.Z = FMath::Max(static_cast<float>(Velocity.Z), 450.f);
		FuseLeft = FMath::Max(FuseLeft, 0.7f);
		bResting = false;
	}
	Age = 0.f;
	APinguFX::Sound(this, EPinguSound::ShieldBlock, 0.8f);
	APinguFX::Sparkles(this, GetActorLocation(), 5, 25.f, FLinearColor(1.f, 0.95f, 0.6f));
	if (Kind == EPenguinProjectileKind::Icicle || Kind == EPenguinProjectileKind::FishHook || Kind == EPenguinProjectileKind::IceBolt)
	{
		SetActorRotation(Velocity.Rotation());
	}
	return true;
}

void APenguinProjectile::EmitTrail()
{
	const FVector Now = GetActorLocation();
	if (!bTrailStarted || bStuck || Kind == EPenguinProjectileKind::FishHook)
	{
		// The hook has its fishing line instead; stuck icicles don't move.
		LastTrailPoint = Now;
		bTrailStarted = true;
		return;
	}
	FLinearColor Color(0.8f, 0.9f, 1.f);
	float Width = 20.f;
	float Opacity = 0.22f;
	switch (Kind)
	{
	case EPenguinProjectileKind::Snowball:
		Color = IsFrozen() ? FLinearColor(0.5f, 0.85f, 1.f) : FLinearColor(0.8f, 0.9f, 1.f);
		break;
	case EPenguinProjectileKind::Icicle:
		Color = FLinearColor(0.45f, 0.85f, 1.f);
		Width = 14.f;
		Opacity = 0.3f;
		break;
	case EPenguinProjectileKind::Bomb:
		Color = FLinearColor(1.f, 0.62f, 0.3f);
		Width = 22.f;
		break;
	case EPenguinProjectileKind::IceBolt:
		Color = FLinearColor(0.5f, 0.9f, 1.f);
		Width = 13.f;
		Opacity = 0.32f;
		break;
	case EPenguinProjectileKind::ShrinkRay:
	case EPenguinProjectileKind::GrowRay:
		Color = RayColor();
		Width = 16.f;
		Opacity = 0.3f;
		break;
	default:
		break;
	}
	APinguFX::GlowTrail(this, LastTrailPoint, Now, Width * SizeScale, Color, Opacity, 0.28f);
	LastTrailPoint = Now;
}

void APenguinProjectile::Finish()
{
	if (bDone)
	{
		return;
	}
	// The last bit of the trail, up to where it hit, is drawn by TickVisuals (outside any effect broadcast).
	bDone = true;
	HookedPenguin = nullptr;
	HookedIce = nullptr;
	if (APinguPickup* Caught = CaughtPickup.Get())
	{
		// A hook that goes without handing its bubble over lets go of it.
		CaughtPickup.Reset();
		Caught->ReleaseFromHook(FVector::ZeroVector);
	}
	if (Peg)
	{
		Peg->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	if (GetNetMode() == NM_Standalone)
	{
		SetLifeSpan(0.05f);
	}
	else
	{
		// LAN: send the final state now (it arrives with the impact effect) and keep the actor until it got there.
		ForceNetUpdate();
		SetLifeSpan(PenguinProjectilePrivate::NetFinishLinger);
	}
}

void APenguinProjectile::UpdateLine()
{
	if (Kind != EPenguinProjectileKind::FishHook || !Line)
	{
		return;
	}
	const FVector From = HandLocation();
	const FVector To = GetActorLocation();
	const FVector Delta = To - From;
	const float Length = Delta.Size();
	if (Length < 1.f)
	{
		Line->SetVisibility(false);
		return;
	}
	Line->SetVisibility(true);
	// The engine cylinder is 100 cm tall along Z with a 50 cm radius.
	const FQuat Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Delta / Length);
	Line->SetWorldLocationAndRotation((From + To) * 0.5f, Rotation);
	Line->SetWorldScale3D(FVector(0.035f, 0.035f, Length / 100.f));
}

void APenguinProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bDone)
	{
		TickVisuals(0.f); // the last bit of the trail (once)
		return;
	}
	const float Dt = FMath::Min(DeltaSeconds, 0.05f);
	Age += Dt;
	bMovedThisFrame = false;
	if (HasAuthority())
	{
		// Every effect of this frame's gameplay (hits, bounces, breaking, explosions) also plays on the LAN clients.
		FPinguFXBroadcastScope FXScope(this);
		TickGameplay(Dt);
	}
	else
	{
		TickRemote(Dt);
	}
	TickVisuals(Dt);
}

void APenguinProjectile::TickGameplay(float Dt)
{
	UWorld* World = GetWorld();

	// ---- icicle stuck in the ground
	if (bStuck)
	{
		StuckLeft -= Dt;
		bPegExpiring = (Peg != nullptr || bGroundPad) && StuckLeft < 0.8f; // it blinks (every machine draws that itself)
		if (StuckLeft <= 0.f)
		{
			APinguFX::Shards(this, GetActorLocation(), 5, 400.f, FLinearColor(0.7f, 0.9f, 1.f));
			Finish();
		}
		return;
	}

	// ---- fish hook: pulling the thrower, riding on a thrown penguin, or reeling back in
	if (Kind == EPenguinProjectileKind::FishHook)
	{
		if (bAnchored)
		{
			AnchorLeft -= Dt;
			SetActorLocation(Anchor);
			// Let go once the thrower's capsule (big or small) has reached the hook.
			const APenguinCharacter* Thrower = GetShooter();
			if (AnchorLeft <= 0.f || !Thrower || Thrower->DistanceTo(Anchor) < 60.f)
			{
				Finish();
			}
			return;
		}
		if (HookedPenguin)
		{
			// The line stays on the penguin flying over its thrower's head until it comes down (or the throw is over).
			HaulLeft -= Dt;
			HaulAge += Dt;
			const APenguinCharacter* Victim = HookedPenguin.Get();
			const APenguinCharacter* Thrower = GetShooter();
			const UPenguinMovementComponent* VictimMove = IsValid(Victim) ? Victim->GetPenguinMovement() : nullptr;
			const bool bLanded = HaulAge > PenguinProjectilePrivate::HookHaulMinTime && VictimMove && VictimMove->IsMovingOnGround();
			if (!VictimMove || !Victim->IsAlive() || Victim->IsHidden() || !Thrower || !Thrower->IsAlive() || HaulLeft <= 0.f || bLanded)
			{
				HookedPenguin = nullptr;
				bRetracting = true;
				return;
			}
			const FVector Hold = Victim->GetAimOrigin();
			SetActorRotation((Hold - HandLocation()).Rotation());
			SetActorLocation(Hold);
			Velocity = VictimMove->Velocity;
			return;
		}
		if (HookedIce)
		{
			// The line stays on the ice block it pulled out of the island until that lands (a block of the island again).
			HaulLeft -= Dt;
			const AHookedIceBlock* Block = HookedIce.Get();
			if (!IsValid(Block) || Block->HasLanded() || HaulLeft <= 0.f || !GetShooter())
			{
				HookedIce = nullptr;
				bRetracting = true;
				return;
			}
			const FVector Hold = Block->GetCenter();
			SetActorRotation((Hold - HandLocation()).Rotation());
			SetActorLocation(Hold);
			Velocity = Block->GetVelocity();
			return;
		}
		if (bRetracting)
		{
			const FVector Hand = HandLocation();
			const FVector ToHand = Hand - GetActorLocation();
			const float Step = PenguinProjectilePrivate::HookReelSpeed * Dt;
			if (ToHand.Size() <= Step || !GetShooter())
			{
				SetActorLocation(Hand);
				HandOverPickup();
				Finish();
				return;
			}
			SetActorLocation(GetActorLocation() + ToHand.GetSafeNormal() * Step);
			SetActorRotation((-ToHand).Rotation());
			return;
		}
	}

	// ---- frozen (frost breath, ice gun): bombs stop ticking
	const bool bFrozen = FrozenLeft > 0.f;
	if (bFrozen)
	{
		FrozenLeft = FMath::Max(0.f, FrozenLeft - Dt);
		if (FrozenLeft <= 0.f)
		{
			Thaw(); // back to the normal look
		}
	}

	// ---- bomb fuse (its sparks are drawn by TickVisuals); a belly slide kicks it along
	if (Kind == EPenguinProjectileKind::Bomb)
	{
		if (!bFrozen)
		{
			FuseLeft -= Dt;
		}
		// The blast zone clock goes once round in the longest fuse it had (Reflect, FlingOverhead and Detonate change
		// FuseLeft; the clock follows from here on every machine).
		FuseTotal = FMath::Max(FuseTotal, FuseLeft);
		FuseTicks = static_cast<uint8>(FMath::Clamp(FMath::CeilToInt(FuseLeft * 40.f), 0, 255));
		FuseTotalTicks = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FuseTotal * 40.f), 1, 255));
		if (FuseLeft <= 0.f)
		{
			Explode();
			return;
		}
		KickCooldown = FMath::Max(0.f, KickCooldown - Dt);
		KickBySlide();
	}

	if (Age > Life && Kind != EPenguinProjectileKind::Bomb)
	{
		if (Kind == EPenguinProjectileKind::FishHook)
		{
			bRetracting = true;
			return;
		}
		APinguFX::Puffs(this, GetActorLocation(), 3, 16.f, 120.f);
		Finish();
		return;
	}
	if (bResting)
	{
		return;
	}

	Velocity.Z -= Gravity * Dt;
	const FVector Start = GetActorLocation();
	const FVector End = Start + Velocity * Dt;

	// ---- other projectiles: everything interacts with everything
	{
		const float Length = FMath::Max(static_cast<float>((End - Start).Size()), 0.01f);
		APenguinProjectile* Met = nullptr;
		float MetT = TNumericLimits<float>::Max();
		for (TActorIterator<APenguinProjectile> It(World); It; ++It)
		{
			APenguinProjectile* Other = *It;
			if (Other == this || !Other->IsFlying())
			{
				continue;
			}
			// Your own shots meet each other too, just not right at the flipper: while one of them is fresh they pass.
			// Size rays resize your own shots at once (as they do your pegs). Your own hook never grabs your own shots.
			const bool bOneIsRay = PenguinProjectilePrivate::IsSizeRay(Kind) != PenguinProjectilePrivate::IsSizeRay(Other->Kind);
			const bool bOneIsHook = (Kind == EPenguinProjectileKind::FishHook) != (Other->Kind == EPenguinProjectileKind::FishHook);
			if (Other->Shooter == Shooter && !bOneIsRay && (bOneIsHook || FMath::Min(Age, Other->Age) < PenguinProjectilePrivate::OwnShotGrace))
			{
				continue;
			}
			const FVector P = Other->GetActorLocation();
			const FVector Closest = FMath::ClosestPointOnSegment(P, Start, End);
			if (FVector::Dist(Closest, P) < Radius + Other->Radius)
			{
				const float T = static_cast<float>(FVector::Dist(Start, Closest)) / Length;
				if (T < MetT)
				{
					MetT = T;
					Met = Other;
				}
			}
		}
		if (Met)
		{
			SetActorLocation(Start + (End - Start) * MetT);
			ResolveCollision(this, Met);
			return;
		}
	}

	// ---- ability bubbles: knocked along, the shot keeps flying (the fish hook catches the first one instead)
	float PickupT = TNumericLimits<float>::Max();
	APinguPickup* PickupOnPath = nullptr;
	if (Kind == EPenguinProjectileKind::FishHook)
	{
		PickupOnPath = FindPickupOnPath(Start, End, PickupT);
	}
	else
	{
		PushPickupsOnPath(Start, End);
	}

	// ---- penguins (and their ice shells)
	APenguinCharacter* Victim = nullptr;
	APenguinCharacter* ShieldOwner = nullptr;
	float BestT = TNumericLimits<float>::Max();
	FVector BestPoint = End;
	const float SegmentLength = FMath::Max(static_cast<float>((End - Start).Size()), 0.01f);
	const APenguinCharacter* Thrower = GetShooter();
	// A penguin it flies through for a moment (IgnorePenguin: Ice Touch froze it and it drops past).
	IgnoreLeft = FMath::Max(IgnoreLeft - Dt, 0.f);
	const APenguinCharacter* Ignored = IgnoreLeft > 0.f ? IgnoredPenguin.Get() : nullptr;
	// An icicle's hitbox grows (or shrinks) with the throw's strength (Traits mode; 1 normally).
	const float HitRadius = Kind == EPenguinProjectileKind::Icicle ? Radius * PinguTraitTuning::GetReachScale(ShotStrength) : Radius;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		// Friendly fire: a shot hits the thrower's teammates (and their shells) like anybody else - only never its thrower.
		if (!Penguin->IsAlive() || Penguin->IsHidden() || Penguin == Thrower || Penguin == Ignored)
		{
			continue;
		}
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		const FVector Center = Penguin->GetActorLocation();

		if (Penguin->IsShielded())
		{
			const FVector Chest = Penguin->GetAimOrigin();
			const FVector Closest = FMath::ClosestPointOnSegment(Chest, Start, End);
			if (FVector::Dist(Closest, Chest) < Penguin->GetShieldRadius() + HitRadius)
			{
				const float T = FVector::Dist(Start, Closest) / SegmentLength;
				if (T < BestT)
				{
					BestT = T;
					ShieldOwner = Penguin;
					Victim = nullptr;
					BestPoint = Closest;
				}
			}
			continue;
		}

		const FVector A = Center - FVector(0.f, 0.f, HalfHeight - CapsuleRadius);
		const FVector B = Center + FVector(0.f, 0.f, HalfHeight - CapsuleRadius);
		FVector OnPath, OnCapsule;
		FMath::SegmentDistToSegmentSafe(Start, End, A, B, OnPath, OnCapsule);
		if (FVector::Dist(OnPath, OnCapsule) < HitRadius + CapsuleRadius)
		{
			const float T = FVector::Dist(Start, OnPath) / SegmentLength;
			if (T < BestT)
			{
				BestT = T;
				Victim = Penguin;
				ShieldOwner = nullptr;
				BestPoint = OnPath;
			}
		}
	}

	// ---- the island
	FHitResult Hit;
	const bool bWorldHit = SweepWorld(Start, End, Hit);
	const float WorldT = bWorldHit ? Hit.Time : TNumericLimits<float>::Max();

	if (PickupOnPath && PickupT <= BestT && PickupT <= WorldT)
	{
		// The hook catches the bubble and reels it in: its thrower gets it at the flipper.
		SetActorLocation(Start + (End - Start) * PickupT);
		CaughtPickup = PickupOnPath;
		PickupOnPath->AttachToHook(this);
		bRetracting = true;
		APinguFX::Sound(this, EPinguSound::HookHit, 0.7f);
		return;
	}
	if (ShieldOwner && BestT <= WorldT)
	{
		SetActorLocation(BestPoint);
		FVector Away = BestPoint - ShieldOwner->GetAimOrigin();
		Away.Y = 0.f;
		Reflect(ShieldOwner, Away.GetSafeNormal() * 0.6f - Velocity.GetSafeNormal() * 0.4f);
		return;
	}
	if (Victim && BestT <= WorldT)
	{
		SetActorLocation(BestPoint);
		// The victim's trait may take the shot instead (Mirror sends it back, Ice Touch freezes it, Hot Feet melts it).
		if (UPenguinTraitComponent* VictimTraits = Victim->GetTraits(); VictimTraits && VictimTraits->InterceptShot(this, BestPoint))
		{
			return;
		}
		HitPenguin(Victim, BestPoint);
		return;
	}
	if (bWorldHit)
	{
		SetActorLocation(Hit.Location);
		HitWorld(Hit);
		return;
	}

	SetActorLocation(End);
	bMovedThisFrame = true; // spin and trail puffs: TickVisuals
}

bool APenguinProjectile::SweepWorld(const FVector& Start, const FVector& End, FHitResult& OutHit) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguProjectile), false, this);
	if (Kind == EPenguinProjectileKind::Snowball || Kind == EPenguinProjectileKind::Icicle)
	{
		// Your own snowballs and icicles fly through your own pegs (throw down from a peg without breaking it).
		for (TActorIterator<APenguinProjectile> It(World); It; ++It)
		{
			if (*It != this && It->Peg && !It->bDone && It->Shooter == Shooter)
			{
				Params.AddIgnoredActor(*It);
			}
		}
	}
	return World->SweepSingleByObjectType(OutHit, Start, End, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic),
		FCollisionShape::MakeSphere(Radius), Params);
}

void APenguinProjectile::TickRemote(float Dt)
{
	using namespace PenguinProjectilePrivate;
	if (bPegCollisionPending && Peg && !PegWouldTrap(PegCenter(), PegExtent(SizeScale), true))
	{
		Peg->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		bPegCollisionPending = false;
	}
	if (bStuck)
	{
		return; // sits in the wall where the server put it
	}
	if (HookedPenguin)
	{
		// The hook rides on the penguin it throws over its thrower's head, as this machine shows that penguin.
		if (const APenguinCharacter* Victim = HookedPenguin.Get())
		{
			const FVector Hold = Victim->GetAimOrigin();
			SetActorRotation((Hold - HandLocation()).Rotation());
			SetActorLocation(Hold);
			NetLocation = Hold;
		}
		NetError = FVector::ZeroVector;
		return;
	}
	if (HookedIce)
	{
		// ... or on the ice block it pulled out of the island, as this machine shows that block.
		if (const AHookedIceBlock* Block = HookedIce.Get())
		{
			const FVector Hold = Block->GetCenter();
			SetActorRotation((Hold - HandLocation()).Rotation());
			SetActorLocation(Hold);
			NetLocation = Hold;
		}
		NetError = FVector::ZeroVector;
		return;
	}
	if (Kind == EPenguinProjectileKind::FishHook && bRetracting)
	{
		// Reels in toward the thrower's flipper as this machine sees it (the line is drawn from that flipper) and
		// waits there until the server finishes it.
		const FVector Hand = HandLocation();
		const FVector ToHand = Hand - GetActorLocation();
		const float Step = HookReelSpeed * Dt;
		if (ToHand.Size() <= Step)
		{
			SetActorLocation(Hand);
		}
		else
		{
			SetActorLocation(GetActorLocation() + ToHand.GetSafeNormal() * Step);
			SetActorRotation((-ToHand).Rotation());
		}
		NetLocation = GetActorLocation();
		NetError = FVector::ZeroVector;
		return;
	}
	if (!bAnchored && !bResting && !bNetBlocked)
	{
		// Keep flying with the replicated velocity until the next update: no stepping at the net rate.
		Velocity.Z -= Gravity * Dt;
		const FVector Start = NetLocation;
		const FVector End = Start + Velocity * Dt;
		FHitResult Hit;
		if (SweepWorld(Start, End, Hit))
		{
			if (Kind == EPenguinProjectileKind::Bomb)
			{
				// Bounces as on the server (the island is the same everywhere), so it never sinks into the ground.
				FVector Normal;
				bool bRest = false;
				BounceBomb(Hit, Velocity, NetLocation, Normal, bRest);
				bNetSteady = false;
			}
			else
			{
				// Stops at the wall until the server says what became of it (stuck, hooked, burst, ...).
				NetLocation = Hit.Location;
				bNetBlocked = true;
			}
		}
		else
		{
			NetLocation = End;
			bMovedThisFrame = true;
		}
	}
	NetError = FadeNetError(NetError, Velocity, bNetSteady && !bNetBlocked && !bAnchored && !bResting, Dt);
	SetActorLocation(NetLocation + NetError);
}

void APenguinProjectile::TickVisuals(float Dt)
{
	if (bDone)
	{
		// The last bit of the path, up to where it hit.
		if (!bFinalTrailDone)
		{
			bFinalTrailDone = true;
			EmitTrail();
		}
		return;
	}
	const float Density = PenguinProjectilePrivate::FXDensity();

	// Glow trail: a segment every frame at full effects, fewer and longer ones at lower settings.
	GlowWait -= Dt;
	if (GlowWait <= 0.f)
	{
		GlowWait = (1.f / Density - 1.f) / 60.f;
		EmitTrail();
	}

	// An icicle peg (or ground pad) about to break blinks (an ice bolt can refreeze a peg: back to its steady glow).
	if ((Peg || bGroundPad) && bPegExpiring && MeshMaterial)
	{
		MeshMaterial->SetScalarParameterValue(TEXT("Emissive"), 0.6f + 1.6f * (0.5f + 0.5f * FMath::Sin(Age * 32.f)));
		bPegBlinkShown = true;
	}
	else if (bPegBlinkShown && MeshMaterial)
	{
		MeshMaterial->SetScalarParameterValue(TEXT("Emissive"), PenguinProjectilePrivate::IcicleEmissive);
		bPegBlinkShown = false;
	}

	// The bomb's burning fuse and its blast zone.
	if (Kind == EPenguinProjectileKind::Bomb)
	{
		UpdateBlastZone(Dt);
		TrailTimer -= Dt;
		if (TrailTimer <= 0.f)
		{
			TrailTimer = 0.05f / Density;
			const FVector Tip = GetActorLocation() + GetActorRotation().RotateVector(FVector(0.f, 0.f, 30.f));
			APinguFX::Sparkles(this, Tip, 1, 5.f, FLinearColor(1.f, 0.7f, 0.2f));
		}
	}

	if (bMovedThisFrame)
	{
		switch (Kind)
		{
		case EPenguinProjectileKind::Icicle:
		case EPenguinProjectileKind::FishHook:
		case EPenguinProjectileKind::IceBolt:
			SetActorRotation(Velocity.Rotation());
			break;
		default:
			AddActorLocalRotation(FRotator(-Spin * Dt * FMath::Sign(Velocity.X + 0.01f), 0.f, 0.f));
			break;
		}

		if (Kind != EPenguinProjectileKind::Bomb)
		{
			TrailTimer -= Dt;
			if (TrailTimer <= 0.f)
			{
				TrailTimer = (Kind == EPenguinProjectileKind::Snowball ? 0.045f : 0.03f) / Density;
				if (Kind == EPenguinProjectileKind::Icicle || Kind == EPenguinProjectileKind::IceBolt)
				{
					APinguFX::Sparkles(this, GetActorLocation(), 1, 6.f, FLinearColor(0.6f, 0.9f, 1.f));
				}
				else if (PenguinProjectilePrivate::IsSizeRay(Kind))
				{
					APinguFX::Sparkles(this, GetActorLocation(), 2, 10.f * SizeScale, RayColor());
				}
				else
				{
					APinguFX::Trail(this, GetActorLocation(), Kind == EPenguinProjectileKind::Snowball ? 16.f : 12.f);
				}
			}
		}
	}
	UpdateLine();
}

void APenguinProjectile::UpdateBlastZone(float Dt)
{
	using namespace PenguinProjectilePrivate;
	const bool bFrozen = IsFrozen();
	// The clock: how far the fuse has burnt of the longest fuse the bomb had. The ring turns solid from 12 o'clock
	// clockwise over the dotted ring and is all solid when the bomb goes off; a frozen fuse stops it, a fuse batted back
	// onto 0.7 s turns it back to match. The server reads its own fuse. A LAN client reads the replicated one (in 1/40 s,
	// rounded up) and counts down between updates by itself - never past what the server sent, not at all while frozen -
	// so its ring turns smoothly and never runs ahead of the server's.
	float Fuse = 0.f;
	float FuseFull = 0.f;
	if (HasAuthority())
	{
		Fuse = FMath::Max(FuseLeft, 0.f);
		FuseFull = FMath::Max(FuseTotal, Fuse);
	}
	else
	{
		const float NetFuseTop = FuseTicks / 40.f;
		const float NetFuseBottom = FMath::Max(static_cast<int32>(FuseTicks) - 1, 0) / 40.f;
		ClientFuse = ClientFuse < 0.f ? NetFuseTop : ClientFuse - (bFrozen ? 0.f : Dt);
		ClientFuse = FMath::Clamp(ClientFuse, NetFuseBottom, NetFuseTop);
		Fuse = ClientFuse;
		FuseFull = FMath::Max(FuseTotalTicks / 40.f, Fuse);
	}
	ZoneProgress = FuseFull > 0.f ? FMath::Clamp(1.f - Fuse / FuseFull, 0.f, 1.f) : 0.f;

	if (bNoBlastZone)
	{
		return;
	}
	if (!BlastZone)
	{
		UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
		UMaterialInterface* ZoneMaterial = LoadObject<UMaterialInterface>(nullptr, BlastZoneMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!Plane || !ZoneMaterial)
		{
			bNoBlastZone = true; // the fx / niagara setup phase builds M_BlastZone
			return;
		}
		BlastZone = NewObject<UStaticMeshComponent>(this, TEXT("BlastZone"), RF_Transient);
		BlastZone->SetupAttachment(Mesh);
		BlastZone->SetUsingAbsoluteLocation(true);
		BlastZone->SetUsingAbsoluteRotation(true);
		BlastZone->SetUsingAbsoluteScale(true);
		BlastZone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		BlastZone->SetGenerateOverlapEvents(false);
		BlastZone->SetCanEverAffectNavigation(false);
		BlastZone->SetCastShadow(false);
		BlastZone->bAffectDistanceFieldLighting = false;
		BlastZone->bAffectDynamicIndirectLighting = false;
		BlastZone->bVisibleInRayTracing = false;
		BlastZone->bVisibleInReflectionCaptures = false;
		BlastZone->bVisibleInRealTimeSkyCaptures = false;
		BlastZone->SetStaticMesh(Plane);
		BlastZoneMaterial = UMaterialInstanceDynamic::Create(ZoneMaterial, this);
		BlastZone->SetMaterial(0, BlastZoneMaterial);
		BlastZone->SetTranslucentSortPriority(2);
		BlastZone->RegisterComponent();
	}
	ZoneAge += Dt;
	ZonePhase += Dt * (bFrozen ? 0.f : FMath::Lerp(ZonePulseSlow, ZonePulseFast, ZoneProgress * ZoneProgress)) * 2.f * UE_PI;
	const float Pulse = bFrozen ? 0.35f : 0.5f + 0.5f * FMath::Sin(ZonePhase);
	const float FadeIn = FMath::Clamp(ZoneAge / 0.15f, 0.f, 1.f);
	const float LastMoment = !bFrozen && Fuse < 0.3f ? 1.f + 1.5f * (0.3f - Fuse) : 1.f; // brighter just before it goes off

	const float PushRadius = BombPushRadius * SizeScale * BlastScale;
	const FVector Center = GetActorLocation();
	const float Diameter = 2.f * PushRadius / 100.f; // the engine plane is 100 cm across
	BlastZone->SetWorldLocationAndRotation(FVector(Center.X, 0.f, Center.Z), FRotator(0.f, 0.f, 90.f)); // facing the camera
	BlastZone->SetWorldScale3D(FVector(Diameter, Diameter, 1.f));
	if (BlastZoneMaterial)
	{
		BlastZoneMaterial->SetVectorParameterValue(TEXT("Color"), bFrozen ? FLinearColor(0.45f, 0.8f, 1.f) : FLinearColor(1.f, 0.55f, 0.16f));
		BlastZoneMaterial->SetVectorParameterValue(TEXT("CoreColor"), bFrozen ? FLinearColor(0.6f, 0.88f, 1.f) : FLinearColor(1.f, 0.16f, 0.1f));
		BlastZoneMaterial->SetScalarParameterValue(TEXT("Progress"), ZoneProgress);
		BlastZoneMaterial->SetScalarParameterValue(TEXT("Pulse"), Pulse);
		BlastZoneMaterial->SetScalarParameterValue(TEXT("Opacity"), FadeIn * LastMoment * (bFrozen ? 0.6f : 1.f));
		BlastZoneMaterial->SetScalarParameterValue(TEXT("Inner"), BombKillRadius / BombPushRadius);
		BlastZoneMaterial->SetScalarParameterValue(TEXT("RingWidth"), FMath::Clamp(ZoneRingWidth / PushRadius, 0.005f, 0.1f));
	}
}

// ---------------------------------------------------------------------------- LAN clients

void APenguinProjectile::OnRep_State()
{
	ApplyState();
}

void APenguinProjectile::ApplyState()
{
	using namespace PenguinProjectilePrivate;
	if (HasAuthority() || !HasActorBegunPlay())
	{
		return; // BeginPlay applies whatever came with the spawn
	}
	if (SizeScale != ShownSizeScale)
	{
		ShownSizeScale = SizeScale;
		Mesh->SetRelativeScale3D(BaseMeshScale3D(Kind) * SizeScale);
		Radius = BaseRadius(Kind) * SizeScale;
		ResizePegBox();
	}
	if (bFrozenLook != bShownFrozen)
	{
		bShownFrozen = bFrozenLook;
		ApplyFrozenLook(bFrozenLook);
	}
	if (bPegged && !Peg && !bDone)
	{
		// The same box as on the server, so our own penguin can stand on it. If our penguin is in its way (it moved
		// since the server checked), it turns solid once the penguin has left, instead of trapping it.
		bPegCollisionPending = PegWouldTrap(PegCenter(), PegExtent(SizeScale), true);
		CreatePegBox(!bPegCollisionPending);
	}
	if (bDone && !bShownDone)
	{
		bShownDone = true;
		if (!bFinalTrailDone)
		{
			bFinalTrailDone = true;
			EmitTrail();
		}
		if (Peg)
		{
			Peg->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		bPegCollisionPending = false;
		SetActorEnableCollision(false);
		SetActorHiddenInGame(true);
	}
}

void APenguinProjectile::PostNetReceiveVelocity(const FVector& NewVelocity)
{
	// Stuck, finished, hanging on, riding on a thrown penguin or reeling in: the server's velocity is stale there and the
	// copy keeps its own.
	if (bStuck || bDone || bAnchored || HookedPenguin || HookedIce || (Kind == EPenguinProjectileKind::FishHook && bRetracting))
	{
		return;
	}
	const FVector NewPlanar(NewVelocity.X, 0.f, NewVelocity.Z);
	// Same heading and about the same speed as we were flying: the next correction is (mostly) just timing.
	const float OldSpeed = static_cast<float>(Velocity.Size());
	const float NewSpeed = static_cast<float>(NewPlanar.Size());
	bNetSteady = OldSpeed > 1.f && NewSpeed > 1.f && FVector::DotProduct(Velocity, NewPlanar) > 0.95f * OldSpeed * NewSpeed
		&& FMath::Abs(NewSpeed - OldSpeed) < 0.15f * OldSpeed;
	Velocity = NewPlanar;
}

void APenguinProjectile::PostNetReceiveLocationAndRotation()
{
	using namespace PenguinProjectilePrivate;
	// The replicated rotation is never used: every machine spins / aims its own copy.
	FVector NewLocation = FRepMovement::RebaseOntoLocalOrigin(GetReplicatedMovement().Location, this);
	NewLocation.Y = 0.f;
	// Put somewhere else at once (snow warp): straight there, no blending, no trail streak across the jump. (All
	// replicated properties of an update are in before this notify runs, so a new count comes with its location.)
	const bool bTeleported = TeleportCount != ShownTeleportCount;
	ShownTeleportCount = TeleportCount;
	if (bTeleported && HasActorBegunPlay() && !bDone && !bStuck)
	{
		NetLocation = NewLocation;
		NetError = FVector::ZeroVector;
		bNetBlocked = false;
		SetActorLocation(NewLocation);
		LastTrailPoint = NewLocation;
		return;
	}
	if (HookedPenguin || HookedIce)
	{
		return; // rides on the penguin it throws or the ice block it pulls (TickRemote)
	}
	if (!HasActorBegunPlay() || bDone || bStuck)
	{
		// Not drawn yet, finished (right where it hit) or stuck in a wall: straight there.
		NetLocation = NewLocation;
		NetError = FVector::ZeroVector;
		if (!Peg)
		{
			SetActorLocation(NewLocation); // a peg's icicle is lined up with its box instead
		}
		return;
	}
	if (Kind == EPenguinProjectileKind::FishHook && bRetracting)
	{
		return; // reels in by itself toward the flipper it is drawn from
	}
	// Keep drawing it where it is and let the difference fade out over the next frames.
	NetError = GetActorLocation() - NewLocation;
	if (NetError.SizeSquared() > FMath::Square(NetSnapDistance * FMath::Max(1.f, SizeScale)))
	{
		// Too far off to blend: jump there (and don't draw a trail streak across the jump).
		NetError = FVector::ZeroVector;
		SetActorLocation(NewLocation);
		LastTrailPoint = NewLocation;
	}
	NetLocation = NewLocation;
	bNetBlocked = false;
}

// ---------------------------------------------------------------------------- hits

void APenguinProjectile::HitPenguin(APenguinCharacter* Victim, const FVector& Where)
{
	const FVector Dir = Velocity.GetSafeNormal();
	FPenguinHit Hit;
	Hit.Instigator = GetShooter();
	Hit.Location = Where;
	Hit.Strength = ShotStrength;
	switch (Kind)
	{
	case EPenguinProjectileKind::Snowball:
		Hit.Source = EPenguinAbility::Snowball;
		{
			APenguinCharacter* Thrower = GetShooter();
			UPenguinAbilityComponent* ThrowerAbilities = Thrower ? Thrower->GetAbilities() : nullptr;
			// The combo: this hit's number in its thrower's chain of hits (1 = the first), which pushes it that much harder
			// (PinguTuning::GetSnowballComboScale). The chain is the thrower's: none for a ball that came back to its own thrower
			// (batted), none on a frozen penguin's ice (that hit does not land: ApplyPenguinHit), and the number only counts once
			// the hit has landed (a shell takes it, a miss breaks the chain: UPenguinAbilityComponent::UseAbility).
			const UPenguinMovementComponent* VictimMove = Victim->GetPenguinMovement();
			const bool bChain = ThrowerAbilities && Thrower != Victim && VictimMove && !VictimMove->IsFrozen();
			const int32 ComboHit = bChain ? ThrowerAbilities->GetNextSnowballComboHit() : 1;
			const float ComboScale = PinguTuning::GetSnowballComboScale(ComboHit);
			// A grown snowball (or a frozen one) hits harder. The snowball's own size already carries the thrower's (it is
			// thrown at the thrower's size, and a size ray can change it on the way), so the hit counts no attacker size on
			// top: SourceSize 1, only the victim's size scales it again. (The combo's scale goes into the same push: ApplyPenguinHit
			// scales it on - the victim's size, the fight's growth, the traits.)
			const float Power = FMath::Clamp(SizeScale, 0.4f, 2.5f) * (FrozenLeft > 0.f ? 1.3f : 1.f);
			Hit.SourceSize = 1.f;
			Hit.Knockback = FVector2D(Dir.X * PinguTuning::SnowballPush, FMath::Max(Dir.Z * PinguTuning::SnowballPush, 0.f) + PinguTuning::SnowballLift) * Power * ComboScale;
			Hit.Stun = PinguTuning::SnowballStun * FMath::Max(1.f, Power);
			int32 LandedCombo = 1;
			if (Victim->ApplyPenguinHit(Hit) && bChain)
			{
				// The snowball is a combo: every hit gives the thrower the next one at once (a shell or a miss does not), and the
				// next one pushes harder still.
				LandedCombo = ThrowerAbilities->NoteSnowballHit();
				if (!bNoRefund) // (a snowman's snowball: the combo goes on, no snowball back - APinguSnowman)
				{
					ThrowerAbilities->RefundCooldown(EPenguinAbility::Snowball);
				}
				else
				{
					APinguSnowman::NoteShotHit(GetWorld(), this); // (its next throw comes sooner)
				}
				if (LandedCombo >= 2)
				{
					Victim->ShowComboHit(LandedCombo); // the "x3" popping at the victim (every machine's overhead widget)
				}
			}
			// The hit's look grows with the combo: a bigger burst, a rising pitch, golden glints from the third hit on.
			const float Build = FMath::Clamp((PinguTuning::GetSnowballComboScale(LandedCombo) - 1.f) / (PinguTuning::SnowballComboMax - 1.f), 0.f, 1.f);
			APinguFX::Puffs(this, Where, 8 + FMath::RoundToInt(10.f * Build), 24.f * (1.f + 0.8f * Build), 320.f * (1.f + 0.5f * Build), -Dir, 1.f);
			if (LandedCombo >= 3)
			{
				APinguFX::Sparkles(this, Where, 3 + FMath::RoundToInt(9.f * Build), 40.f * (1.f + 0.5f * Build), FLinearColor(1.f, 0.85f, 0.35f));
			}
			APinguFX::Sound(this, EPinguSound::SnowballHit, 1.f + 0.2f * Build, 0.06f, 1.f + 0.4f * Build);
		}
		Finish();
		break;
	case EPenguinProjectileKind::Icicle:
		Hit.Source = EPenguinAbility::Icicle;
		Hit.bDeadly = true;
		Hit.Reason = EPenguinKOReason::Icicle;
		Hit.Knockback = FVector2D(Dir.X * 900.f, 700.f);
		Victim->ApplyPenguinHit(Hit);
		APinguFX::Shards(this, Where, 8, 700.f, FLinearColor(0.7f, 0.9f, 1.f));
		APinguFX::Sound(this, EPinguSound::IcicleHit);
		Finish();
		break;
	case EPenguinProjectileKind::Bomb:
		Explode();
		break;
	case EPenguinProjectileKind::FishHook:
		ThrowPenguinOverhead(Victim, Where);
		break;
	case EPenguinProjectileKind::IceBolt:
		Hit.Source = EPenguinAbility::IceGun;
		Hit.Slip = PenguinProjectilePrivate::IceGunSlipTime * FMath::Clamp(SizeScale, 0.5f, 2.f) * PinguTraitTuning::GetDurationScale(ShotStrength);
		Hit.Knockback = FVector2D(Dir.X * 450.f, 180.f);
		Hit.Stun = 0.f;
		Victim->ApplyPenguinHit(Hit);
		APinguFX::Shards(this, Where, 6, 450.f, FLinearColor(0.6f, 0.92f, 1.f));
		Finish();
		break;
	case EPenguinProjectileKind::ShrinkRay:
	case EPenguinProjectileKind::GrowRay:
		Hit.Source = Kind == EPenguinProjectileKind::ShrinkRay ? EPenguinAbility::ShrinkRay : EPenguinAbility::GrowRay;
		Hit.SizeFactor = Kind == EPenguinProjectileKind::ShrinkRay ? PinguShrinkRayFactor : PinguGrowRayFactor;
		Hit.Knockback = FVector2D::ZeroVector;
		Hit.Stun = 0.f;
		Victim->ApplyPenguinHit(Hit);
		APinguFX::Sparkles(this, Where, 10, 45.f, RayColor());
		Finish();
		break;
	}
}

void APenguinProjectile::HitWorld(const FHitResult& Hit)
{
	using namespace PenguinProjectilePrivate;
	const FVector Dir = Velocity.GetSafeNormal();
	// Hit an icicle peg: rays resize it, snowballs and icicles knock it out of the wall, an ice bolt refreezes it (it
	// lasts its full time again), the fish hook pulls it out.
	if (APenguinProjectile* PegOwner = Cast<APenguinProjectile>(Hit.GetActor()))
	{
		if (PegOwner != this && PegOwner->Peg && !PegOwner->bDone)
		{
			if (Kind == EPenguinProjectileKind::ShrinkRay || Kind == EPenguinProjectileKind::GrowRay)
			{
				PegOwner->Rescale(Kind == EPenguinProjectileKind::ShrinkRay ? 0.6f : 1.6f);
				APinguFX::Sparkles(this, Hit.ImpactPoint, 8, 30.f, RayColor());
				Finish();
				return;
			}
			if (Kind == EPenguinProjectileKind::Snowball || Kind == EPenguinProjectileKind::Icicle)
			{
				PegOwner->Shatter();
				Shatter();
				return;
			}
			if (Kind == EPenguinProjectileKind::IceBolt)
			{
				PegOwner->RefreezePeg(false);
				Finish();
				return;
			}
			if (Kind == EPenguinProjectileKind::FishHook)
			{
				SetActorLocation(Hit.Location);
				YankPeg(PegOwner);
				return;
			}
		}
	}
	// Ice blocks break under (almost) every shot.
	if (HitIce(Hit))
	{
		return;
	}
	switch (Kind)
	{
	case EPenguinProjectileKind::IceBolt:
		// Ice gun: the snow it hits turns to slippery ice.
		if (ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Hit.ImpactPoint))
		{
			Island->FreezeBlocksInRadius(FVector(Hit.ImpactPoint) - FVector(Hit.ImpactNormal) * 20.f, 120.f * SizeScale);
		}
		APinguFX::Shards(this, Hit.ImpactPoint, 5, 380.f, FLinearColor(0.6f, 0.92f, 1.f));
		APinguFX::Sparkles(this, Hit.ImpactPoint, 6, 60.f, FLinearColor(0.75f, 0.95f, 1.f));
		APinguFX::Sound(this, EPinguSound::Freeze, 0.5f);
		Finish();
		break;
	case EPenguinProjectileKind::ShrinkRay:
	case EPenguinProjectileKind::GrowRay:
		APinguFX::Sparkles(this, Hit.ImpactPoint, 8, 30.f, RayColor());
		Finish();
		break;
	case EPenguinProjectileKind::Snowball:
		if (FrozenLeft > 0.f)
		{
			// A frozen snowball is a heavy ice ball: it smashes the snow block it hits too (ice: HitIce).
			if (ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Hit.ImpactPoint))
			{
				const FVector Inside = FVector(Hit.ImpactPoint) - FVector(Hit.ImpactNormal) * IceInside;
				if (Island->GetBlockAt(Inside) == EBlockKind::Snow && Island->BreakBlockAt(Inside))
				{
					APinguFX::Sound(this, EPinguSound::Crumble, 0.6f);
				}
			}
		}
		APinguFX::Puffs(this, Hit.ImpactPoint, 7, 22.f, 260.f, FVector(Hit.ImpactNormal), 1.f);
		APinguFX::Sound(this, EPinguSound::SnowballHit, 0.6f);
		Finish();
		break;
	case EPenguinProjectileKind::Icicle:
		// (An ice block it hits breaks instead: HitIce.)
		bStuck = true;
		StuckLeft = PenguinProjectilePrivate::PegLifetime;
		Thaw(); // a frost-slowed icicle sticks in without its frost (stuck, it would never thaw; a peg glows and blinks itself)
		SetActorLocation(Hit.Location + Dir * 34.f);
		APinguFX::Shards(this, Hit.ImpactPoint, 3, 350.f, FLinearColor(0.7f, 0.9f, 1.f));
		APinguFX::Sound(this, EPinguSound::IcicleHit, 0.5f);
		// Movement: an icicle in a wall becomes a peg to stand on (climb walls, save yourself over the sea); anywhere else
		// (the ground, a ceiling) it is a jump pad you walk into (the movement does the throw: UPenguinMovementComponent).
		if (FMath::Abs(Hit.ImpactNormal.X) > 0.6f)
		{
			MakePeg(Hit, FMath::Sign(static_cast<float>(Hit.ImpactNormal.X)));
		}
		else
		{
			bGroundPad = true;
		}
		break;
	case EPenguinProjectileKind::Bomb:
	{
		// Bounce and roll until the fuse runs out.
		FVector Where;
		FVector Normal;
		bool bRest = false;
		const bool bBounced = PenguinProjectilePrivate::BounceBomb(Hit, Velocity, Where, Normal, bRest);
		SetActorLocation(Where);
		if (!bBounced)
		{
			break;
		}
		if (bRest)
		{
			bResting = true;
		}
		APinguFX::Puffs(this, Hit.ImpactPoint, 2, 14.f, 100.f, Normal, 1.f);
		break;
	}
	case EPenguinProjectileKind::FishHook:
	{
		// Hooked the island (an ice block is pulled out instead: HitIce): fly there.
		APenguinCharacter* Thrower = GetShooter();
		float PullTime = 0.15f;
		if (Thrower && Thrower->GetPenguinMovement() && Thrower->IsAlive())
		{
			FVector ToHook = Hit.Location - Thrower->GetActorLocation();
			ToHook.Y = 0.f;
			const float Dist = ToHook.Size();
			FVector Dir2 = ToHook.GetSafeNormal();
			Dir2.Z += HookPullLift;
			Dir2.Normalize();
			const float Speed = FMath::Clamp(Dist * HookPullSpeedPerCm, HookPullMinSpeed, HookPullMaxSpeed);
			PullTime = FMath::Clamp(Dist / Speed, 0.15f, HookPullMaxLowGravity);
			// The movement component reaches the thrower's own machine by itself (LAN).
			Thrower->GetPenguinMovement()->LaunchPlanar(FVector2D(Dir2.X, Dir2.Z) * Speed, PullTime);
			Thrower->GetPenguinMovement()->RefreshAirMoves();
		}
		bAnchored = true;
		AnchorLeft = PullTime + 0.1f; // the line holds while the thrower flies there
		Anchor = Hit.Location;
		APinguFX::Puffs(this, Hit.ImpactPoint, 4, 16.f, 160.f, FVector(Hit.ImpactNormal), 1.f);
		APinguFX::Sound(this, EPinguSound::HookHit);
		break;
	}
	}
}

bool APenguinProjectile::HitIce(const FHitResult& Hit)
{
	using namespace PenguinProjectilePrivate;
	if (Kind == EPenguinProjectileKind::Bomb || Kind == EPenguinProjectileKind::IceBolt)
	{
		return false; // bombs bounce off (their blast breaks it), the ice gun makes ice rather than breaking it
	}
	ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Hit.ImpactPoint);
	const FVector Normal(Hit.ImpactNormal.X, 0.f, Hit.ImpactNormal.Z);
	const FVector Inside = FVector(Hit.ImpactPoint) - Normal * IceInside;
	if (!Island || Island->GetBlockAt(Inside) != EBlockKind::Ice)
	{
		return false;
	}
	const FVector Dir = Velocity.GetSafeNormal();
	if (Kind == EPenguinProjectileKind::GrowRay)
	{
		// The grow ray grows the ice: a new block on the face it hit (where nobody stands).
		const FIntPoint IceCell = Island->WorldToCell(Inside);
		const bool bSideways = FMath::Abs(Normal.X) >= FMath::Abs(Normal.Z);
		const FIntPoint NewCell = IceCell + (bSideways ? FIntPoint(Normal.X > 0.f ? 1 : -1, 0) : FIntPoint(0, Normal.Z > 0.f ? 1 : -1));
		const FVector NewCenter = Island->CellToWorld(NewCell);
		const float Half = Island->CellSize * 0.5f;
		if (!Island->IsSolidCell(NewCell) && !PegWouldTrap(NewCenter, FVector(Half, 0.f, Half), false)
			&& PinguBuildLimit::AllowsCells(Island, { NewCell }) // (not past the build limit)
			&& Island->AddBlocks({ NewCell }, EBlockKind::Ice, 0.f).Num() > 0)
		{
			APinguFX::Sparkles(this, NewCenter, 10, Half, RayColor());
			APinguFX::Sound(this, EPinguSound::Grow, 0.6f);
		}
		else
		{
			APinguFX::Sparkles(this, Hit.ImpactPoint, 8, 30.f, RayColor());
		}
		Finish();
		return true;
	}
	if (Kind == EPenguinProjectileKind::FishHook)
	{
		// The hook pulls the block out of the island, whole: heavy, it comes about halfway to the thrower and stays where
		// it lands (AHookedIceBlock). The hook rides on it until then.
		APenguinCharacter* Thrower = GetShooter();
		if (AHookedIceBlock* Block = AHookedIceBlock::Pull(Island, Inside, Thrower))
		{
			HookedIce = Block;
			HaulLeft = Block->GetFlightTime() + 0.8f;
			HaulAge = 0.f;
			SetActorLocation(Block->GetCenter());
			APinguFX::Sound(this, EPinguSound::HookHit);
			ForceNetUpdate();
			return true;
		}
		// (No thrower any more: it breaks toward the hook's way back, as it used to.)
		FVector Toward = Thrower ? Thrower->GetAimOrigin() - Inside : -Dir;
		Toward.Y = 0.f;
		if (!Island->BreakIceAt(Inside, Toward.GetSafeNormal()))
		{
			return false;
		}
		SetActorLocation(Hit.Location);
		bRetracting = true;
		APinguFX::Sound(this, EPinguSound::HookHit);
		return true;
	}
	// Snowballs, icicles and the shrink ray (it shrinks the block away) break the block and are used up.
	if (!Island->BreakIceAt(Inside, Dir))
	{
		return false;
	}
	SetActorLocation(Hit.Location);
	if (Kind == EPenguinProjectileKind::ShrinkRay)
	{
		APinguFX::Sparkles(this, Island->CellToWorld(Island->WorldToCell(Inside)), 12, Island->CellSize * 0.5f, RayColor());
		APinguFX::Sound(this, EPinguSound::Shrink, 0.6f);
		Finish();
	}
	else
	{
		Shatter();
	}
	return true;
}

APinguPickup* APenguinProjectile::FindPickupOnPath(const FVector& Start, const FVector& End, float& OutT) const
{
	OutT = TNumericLimits<float>::Max();
	TArray<APinguPickup*> Pickups;
	APinguPickup::GetAll(GetWorld(), Pickups);
	APinguPickup* Best = nullptr;
	const float Length = FMath::Max(static_cast<float>((End - Start).Size()), 0.01f);
	for (APinguPickup* Pickup : Pickups)
	{
		if (!IsValid(Pickup) || Pickup->IsOnHook())
		{
			continue;
		}
		const FVector Bubble = Pickup->GetIconLocation();
		const FVector Closest = FMath::ClosestPointOnSegment(Bubble, Start, End);
		if (FVector::Dist(Closest, Bubble) < Radius + Pickup->GetRadius())
		{
			const float T = static_cast<float>(FVector::Dist(Start, Closest)) / Length;
			if (T < OutT)
			{
				OutT = T;
				Best = Pickup;
			}
		}
	}
	return Best;
}

void APenguinProjectile::HandOverPickup()
{
	APinguPickup* Caught = CaughtPickup.Get();
	CaughtPickup.Reset();
	if (!Caught)
	{
		return;
	}
	// Reeled in to the flipper: the thrower takes it (as if it had touched it); nobody there to take it - it floats on.
	if (!Caught->GiveTo(GetShooter()))
	{
		Caught->ReleaseFromHook(FVector::ZeroVector);
	}
}

void APenguinProjectile::ThrowPenguinOverhead(APenguinCharacter* Victim, const FVector& Where)
{
	using namespace PenguinProjectilePrivate;
	APenguinCharacter* Thrower = GetShooter();
	UPenguinMovementComponent* VictimMove = Victim ? Victim->GetPenguinMovement() : nullptr;
	if (!Thrower || !VictimMove)
	{
		bRetracting = true;
		return;
	}
	if (VictimMove->IsFrozen())
	{
		YankIceCube(Victim, Where); // the ice doesn't break: the whole heavy ice cube comes, about halfway
		return;
	}
	const FVector From = Victim->GetActorLocation();
	const FVector ThrowerAt = Thrower->GetActorLocation();
	const UCapsuleComponent* ThrowerCapsule = Thrower->GetCapsuleComponent();
	const UCapsuleComponent* VictimCapsule = Victim->GetCapsuleComponent();
	const float VictimHalf = VictimCapsule->GetScaledCapsuleHalfHeight();

	// Toward the thrower and over its head (caught straight above or below it: over its back), coordinates along the throw.
	const float Dx = static_cast<float>(ThrowerAt.X - From.X);
	const float Side = FMath::Abs(Dx) > 20.f ? FMath::Sign(Dx) : -Thrower->GetFacing();
	const float ThroughX = FMath::Max(Dx * Side, 0.f);
	const float Width = ThrowerCapsule->GetScaledCapsuleRadius() + VictimCapsule->GetScaledCapsuleRadius();
	const float Behind = HookThrowBehind * FMath::Clamp(Thrower->GetSizeScale(), 0.75f, 1.25f);
	const float LandZ = ThrowLandingZ(GetWorld(), Thrower, static_cast<float>(ThrowerAt.X) + Side * Behind, VictimHalf) - static_cast<float>(From.Z);
	const float ClearZ = static_cast<float>(ThrowerAt.Z - From.Z) + ThrowerCapsule->GetScaledCapsuleHalfHeight() + VictimHalf + HookThrowClearance;

	// The victim flies with its own gravity (heavier on the way down) and, stunned all the way, the air drag of a hit.
	FArcPhysics Physics;
	Physics.GravityUp = 2.f * FMath::Max(VictimMove->JumpHeight, 1.f) / FMath::Square(FMath::Max(VictimMove->JumpTimeToApex, 0.05f));
	Physics.GravityDown = Physics.GravityUp * FMath::Max(VictimMove->GetEffectiveFallGravityMultiplier(), 0.1f);
	Physics.MaxFallSpeed = VictimMove->GetEffectiveMaxFallSpeed();
	Physics.DragX = VictimMove->KnockbackAirDrag;
	FVector2D Launch;
	float FlightTime = 0.f;
	SolveOverheadArc(Physics, ThroughX - Width, ThroughX + Width, ClearZ, ThroughX + Behind, LandZ, HookThrowMaxSpeedX, Launch, FlightTime);
	const FVector2D ThrowVelocity(Launch.X * Side, Launch.Y);

	FPenguinHit Hit;
	Hit.Instigator = Thrower;
	Hit.Source = EPenguinAbility::FishHook;
	Hit.Knockback = ThrowVelocity;
	Hit.Stun = FlightTime + HookThrowStunExtra;
	Hit.Location = Where;
	if (!Victim->ApplyPenguinHit(Hit))
	{
		bRetracting = true; // blocked (or already out): the hook lets go
		return;
	}
	// The hit scales its knockback (size, a frozen victim, ...): the throw flies exactly this arc instead. Like every
	// knockback it reaches a LAN player's own machine by itself.
	VictimMove->ApplyKnockback(ThrowVelocity, Hit.Stun);
	VictimMove->StickNextLanding(); // it comes down behind the thrower with a thud instead of skidding on
	HookedPenguin = Victim;
	HaulLeft = FlightTime + 0.1f;
	HaulAge = 0.f;
	SetActorLocation(Victim->GetAimOrigin());
	APinguFX::Puffs(this, Where, 5, 18.f, 200.f);
	APinguFX::Sound(this, EPinguSound::HookHit);
	ForceNetUpdate();
}

void APenguinProjectile::YankIceCube(APenguinCharacter* Victim, const FVector& Where)
{
	using namespace PenguinProjectilePrivate;
	APenguinCharacter* Thrower = GetShooter();
	UPenguinMovementComponent* VictimMove = Victim ? Victim->GetPenguinMovement() : nullptr;
	if (!Thrower || !VictimMove)
	{
		bRetracting = true;
		return;
	}
	const FVector From = Victim->GetActorLocation();
	const FVector ThrowerAt = Thrower->GetActorLocation();
	const float VictimHalf = Victim->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

	// Toward the thrower, about halfway (caught straight above or below it: it hops in place), coordinates along the pull.
	const float Dx = static_cast<float>(ThrowerAt.X - From.X);
	const float Side = FMath::Abs(Dx) > 20.f ? FMath::Sign(Dx) : -Thrower->GetFacing();
	const float LandX = FMath::Max(Dx * Side, 0.f) * HookHeavyShare;
	const float LandZ = HeavyLandingZ(GetWorld(), static_cast<float>(From.X) + Side * LandX, From, Thrower, VictimHalf) - static_cast<float>(From.Z);

	// The ice cube flies like a block of the island and hardly slows sideways (UPenguinMovementComponent, frozen).
	FArcPhysics Physics;
	Physics.GravityUp = ABlockIsland::FallingBlockGravity;
	Physics.GravityDown = ABlockIsland::FallingBlockGravity;
	Physics.MaxFallSpeed = ABlockIsland::FallingBlockMaxSpeed;
	Physics.DragX = UPenguinMovementComponent::FrozenAirDrag;
	float FlightTime = 0.f;
	const FVector2D Launch = SolveHeavyLob(Physics, LandX, LandZ, FlightTime);
	const FVector2D PullVelocity(Launch.X * Side, Launch.Y);

	FPenguinHit Hit;
	Hit.Instigator = Thrower;
	Hit.Source = EPenguinAbility::FishHook;
	Hit.Knockback = PullVelocity;
	Hit.Stun = FlightTime + HookThrowStunExtra;
	Hit.Location = Where;
	if (!Victim->ApplyPenguinHit(Hit) || !VictimMove->IsFrozen())
	{
		bRetracting = true; // blocked (an ice shell), or already out
		return;
	}
	// The ice holds the whole way (it shatters where it hits something: BeginIceThrow), and the cube flies exactly this
	// arc (the hit scaled its knockback by size). Both reach a LAN player's own machine by themselves.
	VictimMove->SetFrozen(FMath::Max(VictimMove->GetFrozenRemaining(), FlightTime + HookIceHoldExtra));
	VictimMove->ApplyKnockback(PullVelocity, Hit.Stun);
	VictimMove->StickNextLanding(); // it comes down with a thud (and shatters) instead of sliding on
	Victim->BeginIceThrow(FlightTime);
	HookedPenguin = Victim;
	HaulLeft = FlightTime + 0.1f;
	HaulAge = 0.f;
	SetActorLocation(Victim->GetAimOrigin());
	APinguFX::Puffs(this, Where, 5, 18.f, 200.f);
	APinguFX::Sound(this, EPinguSound::HookHit);
	ForceNetUpdate();
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s's fish hook yanks %s's ice cube %.0f cm toward it (%.2f s)"), *Thrower->GetDisplayName(),
		*Victim->GetDisplayName(), LandX, FlightTime);
}

void APenguinProjectile::FlingOverhead(APenguinCharacter* NewOwner)
{
	using namespace PenguinProjectilePrivate;
	if (!HasAuthority() || bDone || bStuck || !NewOwner)
	{
		return;
	}
	Shooter = NewOwner; // replicated: the shot is the hook thrower's now
	PushedPickups.Reset();
	Age = 0.f;
	bResting = false;
	const FVector From = GetActorLocation();
	const FVector ThrowerAt = NewOwner->GetActorLocation();
	if (Gravity <= UE_KINDA_SMALL_NUMBER)
	{
		// Straight shots (icicles) can't arc: they come straight back at the thrower and fly on past it.
		FVector Toward = NewOwner->GetAimOrigin() - From;
		Toward.Y = 0.f;
		Toward = Toward.GetSafeNormal();
		if (Toward.IsNearlyZero())
		{
			Toward = -Velocity.GetSafeNormal();
		}
		Velocity = Toward * FMath::Max(static_cast<float>(Velocity.Size()), HookYankSpeed);
		SetActorRotation(Velocity.Rotation());
		return;
	}
	// Snowballs and bombs: up over the thrower's head and down a few metres behind it.
	const UCapsuleComponent* Capsule = NewOwner->GetCapsuleComponent();
	const float Dx = static_cast<float>(ThrowerAt.X - From.X);
	const float Side = FMath::Abs(Dx) > 20.f ? FMath::Sign(Dx) : -NewOwner->GetFacing();
	const float ThroughX = FMath::Max(Dx * Side, 0.f);
	const float Width = Capsule->GetScaledCapsuleRadius() + Radius;
	const float Behind = HookThrowBehind * FMath::Clamp(NewOwner->GetSizeScale(), 0.75f, 1.25f);
	const float LandZ = ThrowLandingZ(GetWorld(), NewOwner, static_cast<float>(ThrowerAt.X) + Side * Behind, Radius) - static_cast<float>(From.Z);
	const float ClearZ = static_cast<float>(ThrowerAt.Z - From.Z) + Capsule->GetScaledCapsuleHalfHeight() + Radius + HookThrowClearance;
	FArcPhysics Physics;
	Physics.GravityUp = Gravity;
	Physics.GravityDown = Gravity;
	FVector2D Launch;
	float FlightTime = 0.f;
	SolveOverheadArc(Physics, ThroughX - Width, ThroughX + Width, ClearZ, ThroughX + Behind, LandZ, HookThrowMaxSpeedX, Launch, FlightTime);
	Velocity = FVector(Launch.X * Side, 0.f, Launch.Y);
	Life = FMath::Max(Life, FlightTime + HookFlingLifeExtra); // a long lob must not vanish in mid-air
	if (Kind == EPenguinProjectileKind::Bomb)
	{
		FuseLeft = FMath::Max(FuseLeft, FlightTime + HookBombFuseExtra); // it lands behind the thrower, then goes off
	}
}

void APenguinProjectile::YankPeg(APenguinProjectile* PegOwner)
{
	using namespace PenguinProjectilePrivate;
	APenguinCharacter* Thrower = GetShooter();
	if (Thrower && PegOwner)
	{
		// Out of the wall: the peg's icicle flies at the thrower - and on past it, it is the thrower's now.
		const FVector From = PegOwner->GetActorLocation();
		FVector Toward = Thrower->GetAimOrigin() - From;
		Toward.Y = 0.f;
		Toward = Toward.GetSafeNormal();
		if (Toward.IsNearlyZero())
		{
			Toward = -Velocity.GetSafeNormal();
		}
		if (APenguinProjectile* Yanked = Launch(GetWorld(), EPenguinProjectileKind::Icicle, Thrower, From, Toward * HookYankSpeed))
		{
			// As big as the peg was (set before it first replicates, so every machine spawns it that size).
			Yanked->SizeScale = PegOwner->SizeScale;
			Yanked->Radius = BaseRadius(EPenguinProjectileKind::Icicle) * Yanked->SizeScale;
			Yanked->Mesh->SetRelativeScale3D(BaseMeshScale3D(EPenguinProjectileKind::Icicle) * Yanked->SizeScale);
		}
	}
	if (PegOwner)
	{
		PegOwner->Shatter(); // the peg (and its box) is gone
	}
	bRetracting = true;
	APinguFX::Sound(this, EPinguSound::HookHit);
}

void APenguinProjectile::KickBySlide()
{
	using namespace PenguinProjectilePrivate;
	if (KickCooldown > 0.f || bDone)
	{
		return;
	}
	const FVector Center = GetActorLocation();
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		if (!Move || !Penguin->IsAlive() || Penguin->IsHidden() || !Move->IsBellySliding() || Penguin->DistanceTo(Center) > Radius + BombKickReach)
		{
			continue;
		}
		// Slid into it: kicked along ahead of the slider, the slider's bomb now (its blast can't knock the slider out).
		const float SlideX = static_cast<float>(Move->Velocity.X);
		const float Side = FMath::Abs(SlideX) > BombKickMinSlideSpeed ? FMath::Sign(SlideX) : Penguin->GetFacing();
		Velocity = FVector(Side * FMath::Max(FMath::Abs(SlideX) * BombKickOverSpeed, BombKickSpeed), 0.f, BombKickUp);
		Shooter = Penguin;
		PushedPickups.Reset();
		bResting = false;
		KickCooldown = BombKickCooldown;
		APinguFX::Puffs(this, Center, 4, 16.f, 180.f, FVector(-Side, 0.f, 0.3f), 1.f);
		APinguFX::Sound(this, EPinguSound::HookHit, 0.4f, 0.08f, 1.4f);
		return;
	}
}

void APenguinProjectile::WarpTo(const FVector& NewLocation)
{
	if (!HasAuthority() || bDone || bStuck)
	{
		return;
	}
	FVector Where = NewLocation;
	Where.Y = 0.f;
	SetActorLocation(Where);
	bResting = false;
	++TeleportCount; // clients snap there instead of blending the jump (PostNetReceiveLocationAndRotation)
	LastTrailPoint = Where;
	ForceNetUpdate();
}

// ---------------------------------------------------------------------------- pegs

FVector APenguinProjectile::PegCenter() const
{
	const FVector Extent = PenguinProjectilePrivate::PegExtent(SizeScale);
	return FVector(PegWallX + PegOutX * Extent.X, 0.f, PegCenterZ);
}

FVector APenguinProjectile::PegIcicleLocation() const
{
	using namespace PenguinProjectilePrivate;
	// Level, thick end sticking out, its upper surface the peg top, the tip a little into the wall.
	const FVector Extent = PegExtent(SizeScale);
	return FVector(PegWallX + PegOutX * PegIcicleOut * SizeScale, 0.f, PegCenterZ + Extent.Z - PegIcicleDrop * SizeScale);
}

FRotator APenguinProjectile::PegIcicleRotation() const
{
	return FRotator(0.f, PegOutX < 0.f ? 0.f : 180.f, 0.f);
}

bool APenguinProjectile::PegWouldTrap(const FVector& Center, const FVector& Extent, bool bOnlyLocal) const
{
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		const APenguinCharacter* Penguin = *It;
		if (!Penguin->IsAlive() || Penguin->IsHidden() || (bOnlyLocal && !Penguin->IsLocallyControlled()))
		{
			continue;
		}
		if (PenguinProjectilePrivate::IsInPegBox(Penguin, Center, Extent))
		{
			return true;
		}
	}
	return false;
}

bool APenguinProjectile::PegGrowthWouldTrap(float NewScale) const
{
	using namespace PenguinProjectilePrivate;
	// The box grows out of the wall and up and down around its height (PegCenter). Whoever its box touches already (stands
	// on it) is lifted by its own movement as before; somebody the bigger box would newly reach into would be stuck in it.
	const FVector OldExtent = PegExtent(SizeScale);
	const FVector NewExtent = PegExtent(NewScale);
	const FVector OldCenter(PegWallX + PegOutX * OldExtent.X, 0.f, PegCenterZ);
	const FVector NewCenter(PegWallX + PegOutX * NewExtent.X, 0.f, PegCenterZ);
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		const APenguinCharacter* Penguin = *It;
		if (Penguin->IsAlive() && !Penguin->IsHidden() && IsInPegBox(Penguin, NewCenter, NewExtent) && !IsInPegBox(Penguin, OldCenter, OldExtent))
		{
			return true;
		}
	}
	return false;
}

void APenguinProjectile::RefreezePeg(bool bQuietIfFresh)
{
	using namespace PenguinProjectilePrivate;
	if (!HasAuthority() || !Peg || bDone)
	{
		return;
	}
	const bool bFresh = StuckLeft > PegLifetime - PegRefreezeQuiet;
	StuckLeft = FMath::Max(StuckLeft, PegLifetime);
	bPegExpiring = false; // replicated: the blink stops on every machine
	if (bQuietIfFresh && bFresh)
	{
		return;
	}
	FPinguFXBroadcastScope FXScope(this);
	APinguFX::Sparkles(this, GetPegTop(), 10, 40.f * SizeScale, FLinearColor(0.7f, 0.95f, 1.f));
	APinguFX::Sound(this, EPinguSound::Freeze, 0.5f);
}

void APenguinProjectile::MakePeg(const FHitResult& Hit, float OutX)
{
	using namespace PenguinProjectilePrivate;
	UWorld* World = GetWorld();
	if (!World || Peg)
	{
		return;
	}
	const FVector Extent = PegExtent(SizeScale);
	const FVector Center(Hit.ImpactPoint.X + OutX * Extent.X, 0.f, Hit.ImpactPoint.Z - Extent.Z);

	// Never trap a penguin inside a new peg.
	if (PegWouldTrap(Center, Extent, false))
	{
		return;
	}
	// Replicated: every client builds the same box from these (its own penguin stands on its local copy).
	PegOutX = OutX;
	PegWallX = Hit.ImpactPoint.X;
	PegCenterZ = Center.Z;
	bPegged = true;
	CreatePegBox(true);
	if (!Peg)
	{
		bPegged = false;
		return;
	}
	StuckLeft = PegLifetime;
	APinguFX::Sparkles(this, Center, 4, 22.f, FLinearColor(0.7f, 0.93f, 1.f));
}

void APenguinProjectile::CreatePegBox(bool bCollide)
{
	if (Peg)
	{
		return;
	}
	Peg = NewObject<UBoxComponent>(this, TEXT("Peg"));
	if (!Peg)
	{
		return;
	}
	// Line the icicle up with the peg: level, thick end sticking out, tip a little into the wall.
	SetActorLocationAndRotation(PegIcicleLocation(), PegIcicleRotation());

	// Not attached to the root: a Static box can't hang off the Movable mesh, and it uses absolute transforms anyway.
	Peg->SetUsingAbsoluteLocation(true);
	Peg->SetUsingAbsoluteRotation(true);
	Peg->SetUsingAbsoluteScale(true);
	Peg->SetBoxExtent(PenguinProjectilePrivate::PegExtent(SizeScale), false);
	Peg->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	Peg->ComponentTags.Add(UPenguinMovementComponent::JumpPadTag); // whoever lands on it is thrown up (the movement does it)
	if (!bCollide)
	{
		Peg->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	Peg->SetGenerateOverlapEvents(false);
	Peg->SetCanEverAffectNavigation(false);
	// Place it BEFORE registering: once registered, a Static component refuses to move.
	Peg->SetWorldLocationAndRotation(PegCenter(), FRotator::ZeroRotator);
	Peg->SetMobility(EComponentMobility::Static); // a static base: penguins standing on it keep their tick order
	SetActorEnableCollision(true); // spawned with collision off; the mesh and the line stay NoCollision
	Peg->RegisterComponent();
}

void APenguinProjectile::ResizePegBox()
{
	if (!Peg)
	{
		return;
	}
	// A Static box can only be resized and moved while it is unregistered (its height stays).
	Peg->UnregisterComponent();
	Peg->SetBoxExtent(PenguinProjectilePrivate::PegExtent(SizeScale), false);
	Peg->SetWorldLocation(PegCenter());
	Peg->RegisterComponent();
	// Keep the icicle lined up with the resized peg.
	SetActorLocation(PegIcicleLocation());
}

void APenguinProjectile::GetGroundPadSegment(FVector& OutA, FVector& OutB, float& OutRadius) const
{
	using namespace PenguinProjectilePrivate;
	// The icicle points the way it flew (tip +X, centred on the actor): from its thick end to its tip, which is in the ground.
	const float HalfLength = 50.f * IcicleLengthScale * SizeScale;
	const FVector Forward = GetActorForwardVector();
	OutA = GetActorLocation() - Forward * HalfLength;
	OutB = GetActorLocation() + Forward * HalfLength;
	OutRadius = GroundPadThickness * SizeScale;
}

FVector APenguinProjectile::GetPegTop() const
{
	if (!Peg)
	{
		return GetActorLocation();
	}
	return Peg->GetComponentLocation() + FVector(0.f, 0.f, Peg->GetScaledBoxExtent().Z);
}

// ---------------------------------------------------------------------------- explosion

void APenguinProjectile::Explode()
{
	if (bDone)
	{
		return;
	}
	const FVector Center = GetActorLocation();
	bDone = true; // no re-entry through chain reactions
	// The blast zone's clock is full now (the zone hides with the bomb).
	ZoneProgress = 1.f;
	if (BlastZoneMaterial)
	{
		BlastZoneMaterial->SetScalarParameterValue(TEXT("Progress"), 1.f);
	}
	// (BlastScale: the throw's strength in Traits mode, 1 otherwise.)
	const float KillRadius = GetBlastKillRadius();
	const float PushRadius = GetBlastPushRadius();
	APenguinCharacter* Thrower = GetShooter();
	bool bHitSomeone = false;
	if (APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr)
	{
		GameMode->NoteBombExploded(); // (the bot matches count them: how often a bomb kills its own thrower)
	}
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		if (!Penguin->IsAlive() || Penguin->IsHidden())
		{
			continue;
		}
		const float Dist = Penguin->DistanceTo(Center);
		if (Dist > PushRadius)
		{
			continue;
		}
		FVector Dir = Penguin->GetActorLocation() - Center;
		Dir.Y = 0.f;
		Dir = Dir.GetSafeNormal();
		if (Dir.IsNearlyZero())
		{
			Dir = FVector::UpVector;
		}
		// Nobody is spared, the thrower least of all: its own bomb knocks it out inside the kill radius and pushes it up to the
		// push radius, like anybody (the shell, frozen ice and the trait multipliers work the same). A knock-out of its own is
		// a self-knock-out: APenguinCharacter::KnockOut credits it to nobody. The hit reaches the thrower's own machine by the
		// movement component's forwarding (LAN). (A bomb batted, kicked or hooked has its new owner as the thrower: whoever
		// threw it first is just a victim then.)
		FPenguinHit Hit;
		Hit.Instigator = Thrower;
		Hit.Source = EPenguinAbility::SnowBomb;
		Hit.Location = Penguin->GetActorLocation();
		Hit.Strength = ShotStrength;
		if (Dist <= KillRadius)
		{
			Hit.bDeadly = true;
			Hit.Reason = EPenguinKOReason::Bomb;
			Hit.Knockback = FVector2D(Dir.X, FMath::Max(static_cast<float>(Dir.Z), 0.4f)) * 1200.f;
		}
		else
		{
			const float Falloff = 1.f - 0.5f * (Dist - KillRadius) / (PushRadius - KillRadius);
			Hit.Knockback = FVector2D(Dir.X * PinguTuning::BombPush * Falloff, FMath::Max(Dir.Z * PinguTuning::BombPush * Falloff, 0.f) + PinguTuning::BombLift);
			Hit.Stun = PinguTuning::BombStun;
		}
		bHitSomeone |= Penguin->ApplyPenguinHit(Hit);
	}

	// A giant snowball whose surface is inside the kill radius breaks (a bomb thrown straight into one is swallowed instead:
	// APinguGiantSnowball::TouchShots).
	APinguGiantSnowball::BurstInArea(GetWorld(), Center, KillRadius, Thrower, TEXT("snow bomb"));
	// A snowman in its crater crumbles (as the blocks there go).
	APinguSnowman::BreakInArea(GetWorld(), Center, PenguinProjectilePrivate::BombCraterRadius * SizeScale * BlastScale, Thrower, TEXT("snow bomb"));

	// Everything else in the blast: other bombs go off too (chain reaction), projectiles and pegs break,
	// falling icebergs shatter.
	for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
	{
		APenguinProjectile* Other = *It;
		if (Other == this || Other->bDone || FVector::Dist(Other->GetActorLocation(), Center) > PushRadius + Other->Radius)
		{
			continue;
		}
		if (Other->Kind == EPenguinProjectileKind::Bomb)
		{
			Other->Detonate(0.12f);
		}
		else
		{
			Other->Shatter();
		}
	}
	TArray<AIcebergDrop*> Icebergs;
	for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
	{
		if (It->Overlaps(Center, PushRadius * 0.6f))
		{
			Icebergs.Add(*It);
		}
	}
	for (AIcebergDrop* Iceberg : Icebergs)
	{
		Iceberg->Shatter(); // destroyed outside the iterator
	}
	// Ability bubbles get blown away.
	APinguPickup::PushInArea(GetWorld(), Center, PushRadius * 1.3f, PenguinProjectilePrivate::BlastPickupPush);

	if (ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Center))
	{
		TArray<FVector> Removed;
		Island->RemoveBlocksInRadius(Center, PenguinProjectilePrivate::BombCraterRadius * SizeScale, &Removed); // a big crater
		for (int32 Index = 0; Index < Removed.Num() && Index < 8; ++Index)
		{
			APinguFX::Puffs(this, Removed[Index], 2, 40.f, 300.f, FVector::UpVector, 1.f, FLinearColor(0.85f, 0.9f, 1.f));
			APinguFX::Shards(this, Removed[Index], 2, 600.f, FLinearColor(0.75f, 0.82f, 0.95f));
		}
	}

	APinguFX::Explosion(this, Center, PushRadius); // its blast zone flashes at exactly this radius
	APinguFX::Shake(this, 0.6f * FMath::Clamp(SizeScale, 0.6f, 1.6f));
	APinguFX::Sound(this, EPinguSound::Explosion);
	if (bHitSomeone)
	{
		APinguFX::HitStop(this, 0.08f);
	}
	bDone = false; // let Finish() do its work
	Finish();
}

// ---------------------------------------------------------------------------- interactions

void APenguinProjectile::Push(APenguinCharacter* By, const FVector& Direction, float NewSpeed)
{
	if (!HasAuthority() || bDone)
	{
		return;
	}
	if (bStuck)
	{
		if (Peg || bGroundPad)
		{
			Shatter(); // knocked out of the wall (or the ground)
		}
		return;
	}
	if (bRetracting || bAnchored || HookedPenguin || HookedIce)
	{
		return;
	}
	Reflect(By, Direction, NewSpeed);
}

void APenguinProjectile::BlowOut(APenguinCharacter* By, const FVector& Direction)
{
	using namespace PenguinProjectilePrivate;
	if (!HasAuthority() || bDone)
	{
		return;
	}
	if (!bStuck || !(Peg || bGroundPad))
	{
		Push(By, Direction);
		return;
	}
	// Out of the surface it is stuck in: a peg sticks out of its wall along PegOutX; a ground pad's icicle points into the
	// ground (or a ceiling) with its tip. It starts just off its outer end, so it does not hit that surface at once.
	FVector Out = FVector(PegOutX, 0.f, 0.f);
	FVector Start = FVector(PegWallX + PegOutX * (PegLength * SizeScale + Radius + 2.f), 0.f, PegCenterZ);
	if (!Peg)
	{
		FVector ThickEnd;
		FVector Tip;
		float Thickness = 0.f;
		GetGroundPadSegment(ThickEnd, Tip, Thickness);
		Out = ThickEnd - Tip;
		Out.Y = 0.f;
		Out = Out.GetSafeNormal();
		if (Out.IsNearlyZero())
		{
			Out = FVector::UpVector;
		}
		Start = ThickEnd + Out * (Radius + 2.f);
	}
	Start.Y = 0.f;
	// Away from the wind - but never into the surface: that part of the way is turned round (out, then along it).
	FVector Dir(Direction.X, 0.f, Direction.Z);
	Dir = Dir.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = Out;
	}
	const float Into = static_cast<float>(FVector::DotProduct(Dir, Out));
	if (Into < 0.f)
	{
		Dir -= Out * (2.f * Into);
	}
	Dir.Z = FMath::Max(static_cast<float>(Dir.Z), BlownIcicleMinUp);
	Dir = Dir.GetSafeNormal();
	FPinguFXBroadcastScope FXScope(this);
	if (APenguinProjectile* Blown = Launch(GetWorld(), EPenguinProjectileKind::Icicle, By, Start, Dir * BlownIcicleSpeed))
	{
		// As big as the peg was, and heavy: it tumbles down (set before it first replicates, so every machine flies it so).
		Blown->SizeScale = SizeScale;
		Blown->Radius = BaseRadius(EPenguinProjectileKind::Icicle) * Blown->SizeScale;
		Blown->Mesh->SetRelativeScale3D(BaseMeshScale3D(EPenguinProjectileKind::Icicle) * Blown->SizeScale);
		Blown->Gravity = FrozenGravity;
	}
	APinguFX::Puffs(this, Start, 3, 18.f, 260.f, Dir, 0.8f, FLinearColor(0.85f, 0.95f, 1.f));
	Shatter(); // the peg (and its box) is gone from the wall
}

void APenguinProjectile::IgnorePenguin(APenguinCharacter* Penguin, float Seconds)
{
	if (!HasAuthority() || !Penguin)
	{
		return;
	}
	IgnoredPenguin = Penguin;
	IgnoreLeft = FMath::Max(Seconds, 0.f);
}

void APenguinProjectile::SetFlightVelocity(const FVector& NewVelocity)
{
	if (!HasAuthority() || bDone || bStuck)
	{
		return;
	}
	Velocity = FVector(NewVelocity.X, 0.f, NewVelocity.Z);
	bResting = false;
	if (Kind == EPenguinProjectileKind::Icicle || Kind == EPenguinProjectileKind::FishHook || Kind == EPenguinProjectileKind::IceBolt)
	{
		SetActorRotation(Velocity.Rotation());
	}
	ForceNetUpdate(); // every machine's copy flies on from here
}

void APenguinProjectile::Freeze(float Seconds)
{
	using namespace PenguinProjectilePrivate;
	if (!HasAuthority() || bDone || bStuck)
	{
		return;
	}
	// Frost breath calls this every frame: slow down, tint and sparkle only on the first freeze, then just keep it frozen.
	const bool bWasFrozen = FrozenLeft > 0.f;
	switch (Kind)
	{
	case EPenguinProjectileKind::Bomb:
		FrozenLeft = FMath::Max(FrozenLeft, Seconds);
		break;
	case EPenguinProjectileKind::Snowball:
		FrozenLeft = FMath::Max(FrozenLeft, Seconds);
		if (!bWasFrozen)
		{
			Velocity *= 0.35f;
		}
		Gravity = FMath::Max(Gravity, FrozenGravity); // a heavy ice ball now
		break;
	case EPenguinProjectileKind::Icicle:
		// Frost on a flying icicle, once: half as fast, and it drops like a frozen snowball (still deadly). Another freeze
		// changes nothing (an ice bolt never calls this on an icicle: ResolveCollision).
		if (bFrostSlowed)
		{
			return;
		}
		bFrostSlowed = true;
		FrozenLeft = Seconds;
		Velocity *= IcicleFrostSpeed;
		Gravity = FMath::Max(Gravity, FrozenGravity); // replicated: the clients' copies drop too
		break;
	case EPenguinProjectileKind::FishHook:
		bRetracting = true;
		return;
	case EPenguinProjectileKind::IceBolt:
	case EPenguinProjectileKind::ShrinkRay:
	case EPenguinProjectileKind::GrowRay:
		return; // ice bolts and rays are not bothered
	}
	if (bWasFrozen)
	{
		return;
	}
	FPinguFXBroadcastScope FXScope(this);
	bFrozenLook = true;
	ApplyFrozenLook(true);
	APinguFX::Sparkles(this, GetActorLocation(), 5, 25.f, FLinearColor(0.7f, 0.95f, 1.f));
}

void APenguinProjectile::Rescale(float Factor)
{
	if (!HasAuthority() || bDone)
	{
		return;
	}
	const float NewScale = FMath::Clamp(SizeScale * Factor, 0.35f, 3.f);
	const float Applied = NewScale / SizeScale;
	if (FMath::IsNearlyEqual(Applied, 1.f, 0.001f))
	{
		return;
	}
	// A peg never grows into a penguin (as MakePeg never builds one around a penguin): it keeps its size then. (Every
	// machine builds the peg box from the size, so the icicle keeps its size too.)
	if (Peg && Applied > 1.f && PegGrowthWouldTrap(NewScale))
	{
		return;
	}
	FPinguFXBroadcastScope FXScope(this);
	const float OldRadius = Radius;
	SizeScale = NewScale;
	Radius *= Applied;
	Mesh->SetRelativeScale3D(Mesh->GetRelativeScale3D() * Applied);
	if (bResting)
	{
		// Keep sitting on the ground: not sunk in when grown, not hovering when shrunk (no collision on the actor, no sweep needed).
		AddActorWorldOffset(FVector(0.f, 0.f, Radius - OldRadius));
	}
	ResizePegBox();
	APinguFX::Sparkles(this, GetActorLocation(), 8, 30.f * SizeScale, Applied > 1.f ? FLinearColor(0.45f, 1.f, 0.35f) : FLinearColor(0.85f, 0.4f, 1.f));
	APinguFX::Sound(this, Applied > 1.f ? EPinguSound::Grow : EPinguSound::Shrink, 0.6f);
}

void APenguinProjectile::Shatter()
{
	if (!HasAuthority() || bDone)
	{
		return;
	}
	if (Kind == EPenguinProjectileKind::Bomb)
	{
		Detonate(0.05f);
		return;
	}
	if (Kind == EPenguinProjectileKind::FishHook)
	{
		bAnchored = false;
		HookedPenguin = nullptr; // lets go of a penguin it was throwing (the throw flies on)
		HookedIce = nullptr;     // or of the ice block it was pulling (it flies on)
		bRetracting = true;
		return;
	}
	FPinguFXBroadcastScope FXScope(this);
	const FVector Where = GetActorLocation();
	if (Kind == EPenguinProjectileKind::Snowball)
	{
		APinguFX::Puffs(this, Where, 6, 20.f, 220.f);
	}
	else
	{
		APinguFX::Shards(this, Where, 5, 420.f, FLinearColor(0.7f, 0.9f, 1.f));
	}
	Finish();
}

void APenguinProjectile::Detonate(float Delay)
{
	if (!HasAuthority() || bDone || Kind != EPenguinProjectileKind::Bomb)
	{
		return;
	}
	FuseLeft = FMath::Min(FuseLeft, Delay);
	Thaw(); // a frozen bomb set off burns again - and looks it
	bResting = false;
}

void APenguinProjectile::ResolveCollision(APenguinProjectile* A, APenguinProjectile* B)
{
	auto IsRay = [](EPenguinProjectileKind K) { return PenguinProjectilePrivate::IsSizeRay(K); };
	auto Pick = [A, B](EPenguinProjectileKind K, APenguinProjectile*& Found, APenguinProjectile*& Other)
	{
		Found = A->Kind == K ? A : B;
		Other = Found == A ? B : A;
	};
	APenguinProjectile* First = nullptr;
	APenguinProjectile* Second = nullptr;

	// Rays resize whatever they meet (two rays cancel out).
	if (IsRay(A->Kind) || IsRay(B->Kind))
	{
		First = IsRay(A->Kind) ? A : B;
		Second = First == A ? B : A;
		if (IsRay(Second->Kind))
		{
			Second->Finish();
		}
		else
		{
			Second->Rescale(First->Kind == EPenguinProjectileKind::ShrinkRay ? 0.6f : 1.6f);
		}
		APinguFX::Sparkles(First, First->GetActorLocation(), 8, 30.f, First->RayColor());
		First->Finish();
		return;
	}
	// Ice bolts freeze whatever they meet (an icicle flies on: only frost slows it down).
	if (A->Kind == EPenguinProjectileKind::IceBolt || B->Kind == EPenguinProjectileKind::IceBolt)
	{
		Pick(EPenguinProjectileKind::IceBolt, First, Second);
		if (Second->Kind == EPenguinProjectileKind::IceBolt)
		{
			Second->Shatter();
		}
		else if (Second->Kind != EPenguinProjectileKind::Icicle)
		{
			Second->Freeze(2.5f);
		}
		First->Shatter();
		return;
	}
	// The fish hook grabs shots (never its own thrower's): snowballs and bombs are flung over the thrower's head, icicles
	// come straight back past it - the thrower's shots now. Two hooks let go of each other.
	if (A->Kind == EPenguinProjectileKind::FishHook || B->Kind == EPenguinProjectileKind::FishHook)
	{
		Pick(EPenguinProjectileKind::FishHook, First, Second);
		APenguinCharacter* Thrower = First->GetShooter();
		if (Second->Kind == EPenguinProjectileKind::FishHook)
		{
			Second->bRetracting = true;
		}
		else if (Thrower)
		{
			Second->FlingOverhead(Thrower);
			APinguFX::Sparkles(Second, Second->GetActorLocation(), 5, 25.f, FLinearColor(1.f, 0.95f, 0.6f));
		}
		First->bRetracting = true;
		APinguFX::Sound(First, EPinguSound::HookHit, 0.7f);
		return;
	}
	// Bombs: an icicle sets them off, a snowball knocks them along, two bombs bounce off each other.
	if (A->Kind == EPenguinProjectileKind::Bomb || B->Kind == EPenguinProjectileKind::Bomb)
	{
		Pick(EPenguinProjectileKind::Bomb, First, Second);
		if (Second->Kind == EPenguinProjectileKind::Bomb)
		{
			BounceBombs(First, Second);
			return;
		}
		if (Second->Kind == EPenguinProjectileKind::Icicle)
		{
			Second->Shatter();
			First->Detonate(0.f);
			return;
		}
		First->Velocity += Second->Velocity.GetSafeNormal() * 700.f * Second->SizeScale;
		First->Velocity.Z = FMath::Max(static_cast<float>(First->Velocity.Z), 250.f);
		First->bResting = false;
		First->Shooter = Second->Shooter;
		Second->Shatter();
		return;
	}
	// Snowballs burst on each other and on icicles; two icicles shatter each other.
	if (A->Kind == EPenguinProjectileKind::Icicle && B->Kind == EPenguinProjectileKind::Icicle)
	{
		A->Shatter();
		B->Shatter();
		return;
	}
	if (A->Kind == EPenguinProjectileKind::Snowball)
	{
		A->Shatter();
	}
	if (B->Kind == EPenguinProjectileKind::Snowball)
	{
		B->Shatter();
	}
}

void APenguinProjectile::BounceBombs(APenguinProjectile* A, APenguinProjectile* B)
{
	FVector Normal = B->GetActorLocation() - A->GetActorLocation();
	Normal.Y = 0.f;
	const float Dist = static_cast<float>(Normal.Size());
	if (Dist > 0.01f)
	{
		Normal /= Dist;
	}
	else
	{
		Normal = (A->Velocity - B->Velocity).GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Normal = FVector(1.f, 0.f, 0.f);
		}
	}

	// Swap the speeds along the line between the centres (like billiard balls), losing a little.
	const float SpeedA = static_cast<float>(FVector::DotProduct(A->Velocity, Normal));
	const float SpeedB = static_cast<float>(FVector::DotProduct(B->Velocity, Normal));
	const float Approach = SpeedA - SpeedB;
	if (Approach > 0.f)
	{
		A->Velocity += Normal * (SpeedB * PenguinProjectilePrivate::BombBounce - SpeedA);
		B->Velocity += Normal * (SpeedA * PenguinProjectilePrivate::BombBounce - SpeedB);
	}

	// Push them apart so they don't overlap (a bomb resting on the ground stays put, the other one moves).
	const float Overlap = A->Radius + B->Radius + 1.f - Dist;
	if (Overlap > 0.f)
	{
		const float ShareA = B->bResting && !A->bResting ? 1.f : 0.5f;
		A->AddActorWorldOffset(-Normal * Overlap * ShareA);
		B->AddActorWorldOffset(Normal * Overlap * (1.f - ShareA));
	}
	A->bResting = false;
	B->bResting = false;

	if (Approach > 150.f)
	{
		const FVector Contact = A->GetActorLocation() + Normal * A->Radius;
		APinguFX::Puffs(A, Contact, 3, 12.f * FMath::Max(A->SizeScale, B->SizeScale), 140.f, FVector::UpVector, 1.f);
		APinguFX::Sound(A, EPinguSound::HookHit, 0.3f, 0.08f, 1.6f);
	}
}

void APenguinProjectile::PushPickupsOnPath(const FVector& Start, const FVector& End)
{
	TArray<APinguPickup*> Pickups;
	APinguPickup::GetAll(GetWorld(), Pickups);
	if (Pickups.Num() == 0)
	{
		return;
	}
	const FVector Dir = Velocity.GetSafeNormal();
	// Slow things (a rolling bomb, a frozen snowball) only nudge the bubble.
	const float Strength = PenguinProjectilePrivate::PickupPushSpeed * SizeScale * FMath::Clamp(static_cast<float>(Velocity.Size()) / 1200.f, 0.3f, 1.f);
	for (APinguPickup* Pickup : Pickups)
	{
		if (!IsValid(Pickup) || PushedPickups.Contains(Pickup))
		{
			continue;
		}
		const FVector Bubble = Pickup->GetIconLocation();
		if (FVector::Dist(FMath::ClosestPointOnSegment(Bubble, Start, End), Bubble) < Radius + Pickup->GetRadius())
		{
			PushedPickups.Add(Pickup);
			Pickup->Push(Dir * Strength);
		}
	}
}

int32 APenguinProjectile::PushInArea(UWorld* World, APenguinCharacter* By, const FVector& Origin, float AreaRadius, const FVector& ConeDirection, float CosHalfAngle,
	bool bBlowPegsOut)
{
	if (!World || PenguinProjectilePrivate::IsNetClient(World))
	{
		return 0;
	}
	FPinguFXBroadcastScope FXScope(World);
	TArray<APenguinProjectile*> All;
	GetAll(World, All);
	int32 Count = 0;
	for (APenguinProjectile* P : All)
	{
		if (P->GetShooter() == By)
		{
			continue; // your own shots and pegs are safe
		}
		FVector D = P->GetActorLocation() - Origin;
		D.Y = 0.f;
		const float Dist = static_cast<float>(D.Size());
		if (Dist > AreaRadius + P->Radius)
		{
			continue;
		}
		const bool bCone = CosHalfAngle > -1.f;
		if (bCone && Dist > 30.f && FVector::DotProduct(D / Dist, ConeDirection) < CosHalfAngle)
		{
			continue;
		}
		const FVector Dir = bCone ? ConeDirection : (Dist > 1.f ? D / Dist : FVector::UpVector);
		if (bBlowPegsOut)
		{
			P->BlowOut(By, Dir); // pegs and pads fly off (the rest is pushed as ever)
		}
		else
		{
			P->Push(By, Dir);
		}
		++Count;
	}
	return Count;
}

int32 APenguinProjectile::FreezeInArea(UWorld* World, APenguinCharacter* By, const FVector& Origin, float AreaRadius, const FVector& ConeDirection, float CosHalfAngle, float Seconds)
{
	using namespace PenguinProjectilePrivate;
	if (!World || IsNetClient(World))
	{
		return 0;
	}
	FPinguFXBroadcastScope FXScope(World);
	TArray<APenguinProjectile*> All;
	GetAll(World, All);
	int32 Count = 0;
	for (APenguinProjectile* P : All)
	{
		if (P->Peg)
		{
			// An icicle peg - anybody's, the breather's own too (it holds up whoever stands on it) - is refrozen as an ice
			// bolt does it: anywhere along it, from the wall to its end.
			const FVector Extent = PegExtent(P->SizeScale);
			for (const float Along : { 0.f, 1.f, 2.f })
			{
				if (InArea(FVector(P->PegWallX + P->PegOutX * Extent.X * Along, 0.f, P->PegCenterZ), static_cast<float>(Extent.Z), Origin, AreaRadius, ConeDirection, CosHalfAngle))
				{
					P->RefreezePeg(true);
					++Count;
					break;
				}
			}
			continue;
		}
		if (P->GetShooter() == By || !P->IsFlying() || !InArea(P->GetActorLocation(), P->Radius, Origin, AreaRadius, ConeDirection, CosHalfAngle))
		{
			continue;
		}
		P->Freeze(Seconds);
		++Count;
	}
	// A falling iceberg the frost reaches (not the breather's own, like its own shots) hangs in the air for a moment.
	for (TActorIterator<AIcebergDrop> It(World); It; ++It)
	{
		AIcebergDrop* Iceberg = *It;
		if (Iceberg->IsFalling() && Iceberg->GetDropOwner() != By && Iceberg->IsInArea(Origin, AreaRadius, ConeDirection, CosHalfAngle))
		{
			Iceberg->Hold(IcebergFrostHold);
			++Count;
		}
	}
	return Count;
}

// ============================================================================ iceberg

AIcebergDrop::AIcebergDrop()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	SetActorEnableCollision(false);

	// LAN: spawned by the server only; every client draws the warning and the fall (it has no collision itself).
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(60.f);
	SetMinNetUpdateFrequency(30.f);
	NetPriority = 2.f;

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = SceneRoot;

	Blocks = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Blocks"));
	Blocks->SetupAttachment(SceneRoot);
	Blocks->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	Marker = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Marker"));
	Marker->SetupAttachment(SceneRoot);
	Marker->SetUsingAbsoluteLocation(true);
	Marker->SetUsingAbsoluteRotation(true);
	Marker->SetUsingAbsoluteScale(true);
	Marker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Marker->SetCastShadow(false);
}

void AIcebergDrop::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// Set before its first replication: CellSize and FloeTopZ never change; where it lands every client follows on by
	// itself from there (blocks built under it, wind blowing it sideways: UpdateLanding), as the server does.
	DOREPLIFETIME_CONDITION(AIcebergDrop, CellSize, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AIcebergDrop, SizeCells, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AIcebergDrop, BlockSpan, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AIcebergDrop, LandTopZ, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AIcebergDrop, FloeTopZ, COND_InitialOnly);
	DOREPLIFETIME(AIcebergDrop, bHeld);
}

namespace PenguinProjectilePrivate
{
	/** Where an iceberg dropped at TargetX goes (AIcebergDrop::Drop, IsDropAllowed). */
	struct FIcebergPlan
	{
		PinguTuning::FPinguIceScale IceScale;
		int32 Cells = 2;
		int32 Left = 0;
		/** The row its bottom lands on (Lowest: nothing below, a floe). */
		int32 Landing = TNumericLimits<int32>::Lowest();
	};

	static FIcebergPlan PlanIceberg(const ABlockIsland* Island, const APenguinCharacter* InOwner, float TargetX, float MakerSize)
	{
		FIcebergPlan Plan;
		// As big as its dropper is grown: 2x2 blocks, 3x3 from 2x size, 2x2 big blocks (4x4 cells) from 4x.
		Plan.IceScale = PinguTuning::GetIceScale(MakerSize > 0.f ? MakerSize : (InOwner ? InOwner->GetSizeScale() : 1.f));
		Plan.Cells = FMath::Max(Plan.IceScale.IcebergCells, 1);
		const float Cell = Island->CellSize;
		const float IslandX = Island->GetActorLocation().X;
		const float IslandZ = Island->GetActorLocation().Z;
		// Centred on the target (2 wide: the column border nearest it), on the island's cell grid (never off the cells it can hold).
		Plan.Left = FMath::Clamp(FMath::FloorToInt((TargetX - IslandX) / Cell - Plan.Cells * 0.5f + 0.5f), -30000, 30000 - Plan.Cells);
		// Where will it land? The highest block top under its columns.
		const FBox Bounds = Island->GetBounds();
		const int32 TopRow = Bounds.IsValid ? FMath::CeilToInt((Bounds.Max.Z - IslandZ) / Cell) + 2 : 12;
		const int32 BottomRow = Bounds.IsValid ? FMath::FloorToInt((Bounds.Min.Z - IslandZ) / Cell) - 1 : -12;
		for (int32 Column = Plan.Left; Column < Plan.Left + Plan.Cells; ++Column)
		{
			for (int32 Row = TopRow; Row >= BottomRow; --Row)
			{
				if (Island->IsSolidCell(FIntPoint(Column, Row)))
				{
					Plan.Landing = FMath::Max(Plan.Landing, Row + 1);
					break;
				}
			}
		}
		return Plan;
	}
}

bool AIcebergDrop::IsDropAllowed(const UWorld* World, const APenguinCharacter* InOwner, float TargetX, float TargetZ, float MakerSize, FVector* OutLandedAt)
{
	const ABlockIsland* Island = World ? ABlockIsland::FindNear(World, FVector(TargetX, 0.f, 0.f)) : nullptr;
	if (!Island)
	{
		return true;
	}
	const PenguinProjectilePrivate::FIcebergPlan Plan = PenguinProjectilePrivate::PlanIceberg(Island, InOwner, TargetX, MakerSize);
	const float Cell = Island->CellSize;
	const float IslandX = Island->GetActorLocation().X;
	const float IslandZ = Island->GetActorLocation().Z;
	float BottomZ = 0.f;
	if (Plan.Landing != TNumericLimits<int32>::Lowest())
	{
		BottomZ = IslandZ + Plan.Landing * Cell;
	}
	else
	{
		// A floe (as Drop): its top near the target, never into the sea.
		const int32 MinRow = FMath::CeilToInt((Island->SeaLevel + 120.f - IslandZ) / Cell);
		BottomZ = IslandZ + FMath::Max(FMath::RoundToInt((TargetZ - IslandZ) / Cell) - Plan.Cells, MinRow) * Cell;
	}
	const float MinX = IslandX + Plan.Left * Cell;
	const float MaxX = MinX + Plan.Cells * Cell;
	const float TopZ = BottomZ + Plan.Cells * Cell;
	if (OutLandedAt)
	{
		*OutLandedAt = FVector((MinX + MaxX) * 0.5f, 0.f, TopZ);
	}
	return PinguBuildLimit::AllowsSpan(Island, MinX, MaxX, TopZ);
}

AIcebergDrop* AIcebergDrop::Drop(UWorld* World, APenguinCharacter* InOwner, float TargetX, float TargetZ, float MakerSize)
{
	// Icebergs exist only because the server (or solo game) dropped them; clients get them by replication.
	if (!World || PenguinProjectilePrivate::IsNetClient(World))
	{
		return nullptr;
	}
	ABlockIsland* Island = ABlockIsland::FindNear(World, FVector(TargetX, 0.f, 0.f));
	if (!Island)
	{
		return nullptr;
	}
	const PenguinProjectilePrivate::FIcebergPlan Plan = PenguinProjectilePrivate::PlanIceberg(Island, InOwner, TargetX, MakerSize);
	const PinguTuning::FPinguIceScale& IceScale = Plan.IceScale;
	const int32 Cells = Plan.Cells;
	const float Cell = Island->CellSize;
	const float IslandX = Island->GetActorLocation().X;
	const float IslandZ = Island->GetActorLocation().Z;
	const int32 Left = Plan.Left;
	const int32 Landing = Plan.Landing;

	AIcebergDrop* Iceberg = World->SpawnActor<AIcebergDrop>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!Iceberg)
	{
		return nullptr;
	}
	Iceberg->DropOwner = InOwner;
	Iceberg->CellSize = Cell;
	Iceberg->SizeCells = Cells;
	Iceberg->BlockSpan = FMath::Clamp(IceScale.Span, 1, Cells);
	Iceberg->LeftColumn = Left;
	Iceberg->bHasGround = Landing != TNumericLimits<int32>::Lowest();
	// Movement: nothing below (the sea) - it stops as a floating floe with its top near the cursor.
	Iceberg->bFloe = !Iceberg->bHasGround;
	if (Iceberg->bFloe)
	{
		const int32 MinRow = FMath::CeilToInt((Island->SeaLevel + 120.f - IslandZ) / Cell);
		const int32 FloeRow = FMath::Max(FMath::RoundToInt((TargetZ - IslandZ) / Cell) - Cells, MinRow);
		Iceberg->LandTopZ = IslandZ + FloeRow * Cell;
		Iceberg->FloeTopZ = Iceberg->LandTopZ; // blown sideways over the sea, it still stops at this height
	}
	else
	{
		Iceberg->LandTopZ = IslandZ + Landing * Cell;
	}
	const float StartZ = Iceberg->LandTopZ + PenguinProjectilePrivate::IcebergDropHeight;
	Iceberg->SetActorLocation(FVector(IslandX + Left * Cell, 0.f, StartZ));
	Iceberg->NetZ = StartZ;
	Iceberg->NetX = IslandX + Left * Cell;
	Iceberg->LandingX = Iceberg->NetX;
	Iceberg->BuildLook();
	return Iceberg;
}

void AIcebergDrop::BuildLook()
{
	// Blocks: SizeCells x SizeCells cells (2x2 normally), three rows deep like the island's, relative to the bottom-left corner;
	// big blocks: one scaled-up block per 2x2 cells (two along the depth axis, 1.5 rows deep each, as the island draws them).
	if (UStaticMesh* BlockMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Environment/SM_Block.SM_Block"), nullptr, LOAD_Quiet | LOAD_NoWarn))
	{
		Blocks->SetStaticMesh(BlockMesh);
	}
	else
	{
		Blocks->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	}
	// The island's ice material (fresnel rim, inner cracks); the generic M_Block only when it is missing.
	UMaterialInterface* BlockMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Environment/M_Block_Ice.M_Block_Ice"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	if (!BlockMaterial)
	{
		BlockMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Environment/M_Block.M_Block"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	}
	if (BlockMaterial)
	{
		IceMaterial = UMaterialInstanceDynamic::Create(BlockMaterial, this);
		ApplyIceLook(); // clear ice (frosty if it already hangs when a client first sees it)
		Blocks->SetMaterial(0, IceMaterial);
	}
	const float Scale = CellSize / 100.f;
	const int32 Span = FMath::Clamp(BlockSpan, 1, FMath::Max(SizeCells, 1));
	const int32 PerSide = FMath::Max(SizeCells / Span, 1);
	const int32 DepthRows = 3;
	const int32 DepthBlocks = Span > 1 ? FMath::DivideAndRoundUp(DepthRows, Span) : DepthRows;
	const float DepthCells = static_cast<float>(DepthRows) / DepthBlocks;
	for (int32 X = 0; X < PerSide; ++X)
	{
		for (int32 Z = 0; Z < PerSide; ++Z)
		{
			for (int32 Row = 0; Row < DepthBlocks; ++Row)
			{
				const FVector Local((X + 0.5f) * Span * CellSize, (Row - (DepthBlocks - 1) * 0.5f) * DepthCells * CellSize, (Z + 0.5f) * Span * CellSize);
				Blocks->AddInstance(FTransform(FRotator::ZeroRotator, Local, FVector(Scale * Span, Scale * DepthCells, Scale * Span)));
			}
		}
	}

	// Warning ring where it will land (as wide as it is).
	Marker->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FX/SM_Ring.SM_Ring"), nullptr, LOAD_Quiet | LOAD_NoWarn));
	if (UMaterialInterface* RingMaterial = PenguinProjectilePrivate::PropMaterial())
	{
		MarkerMaterial = UMaterialInstanceDynamic::Create(RingMaterial, this);
		MarkerMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.25f, 0.1f));
		MarkerMaterial->SetScalarParameterValue(TEXT("Emissive"), 2.f);
		Marker->SetMaterial(0, MarkerMaterial);
	}
	const FVector MarkerLocation(GetActorLocation().X + GetWidth() * 0.5f, 60.f, LandTopZ + 60.f);
	Marker->SetWorldLocationAndRotation(MarkerLocation, FRotator::ZeroRotator);
	Marker->SetWorldScale3D(FVector(1.6f * SizeCells / PinguTuning::IcebergBlocks));
	ShownMarkerLocation = MarkerLocation;
}

void AIcebergDrop::UpdateMarker()
{
	// Under its middle, where it will come down (a blown iceberg's ring moves along with it).
	const FVector MarkerLocation(GetActorLocation().X + GetWidth() * 0.5f, 60.f, LandTopZ + 60.f);
	if (!MarkerLocation.Equals(ShownMarkerLocation, 0.5))
	{
		ShownMarkerLocation = MarkerLocation;
		Marker->SetWorldLocation(MarkerLocation);
	}
}

void AIcebergDrop::BeginPlay()
{
	Super::BeginPlay();
	NetZ = GetActorLocation().Z;
	NetX = GetActorLocation().X;
	LandingX = NetX;
	if (!HasAuthority())
	{
		// A LAN client: the blocks and the warning ring (the server builds them in Drop).
		BuildLook();
	}
}

void AIcebergDrop::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(DeltaSeconds, 0.05f);
	Age += Dt;

	// Blinking warning ring (every machine), as wide as the iceberg.
	const float Pulse = (1.35f + 0.35f * FMath::Sin(Age * 26.f)) * SizeCells / PinguTuning::IcebergBlocks;
	Marker->SetWorldScale3D(FVector(Pulse));

	// Held in the air by frost: frosty sparkles about it (every machine draws its own from bHeld, nothing is broadcast).
	if (bHeld)
	{
		HeldSparkleTimer -= Dt;
		if (HeldSparkleTimer <= 0.f)
		{
			HeldSparkleTimer = PenguinProjectilePrivate::IcebergHeldSparkleTime / PenguinProjectilePrivate::FXDensity();
			const float Width = GetWidth();
			const FVector Corner = GetActorLocation();
			const FVector Where(Corner.X + FMath::FRandRange(0.f, Width), 0.f, Corner.Z + FMath::FRandRange(0.f, Width));
			APinguFX::Sparkles(this, Where, 1, 22.f, FLinearColor(0.8f, 0.97f, 1.f));
		}
	}

	if (!HasAuthority())
	{
		TickRemote(Dt);
		return;
	}
	if (WarnLeft > 0.f)
	{
		WarnLeft -= Dt;
		return;
	}

	// Held by frost: it hangs right there (nothing moves, so nothing is crushed), then falls on from rest.
	if (HoldLeft > 0.f)
	{
		HoldLeft -= Dt;
		if (HoldLeft <= 0.f)
		{
			HoldLeft = 0.f;
			bHeld = false; // replicated: the clients let it fall on too
			ApplyIceLook();
			ForceNetUpdate();
		}
		return;
	}

	// Crushed and shoved penguins, broken projectiles and the landing also play on the LAN clients.
	FPinguFXBroadcastScope FXScope(this);
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), GetActorLocation());
	const float OldBottom = GetActorLocation().Z;
	const double OldLeftX = GetActorLocation().X;
	double LeftX = OldLeftX;
	// Blown by wind: it flies on sideways until a block stops it, and where it lands follows it.
	if (Island && VelocityX != 0.f)
	{
		StepSideways(Island, Dt, LeftX, OldBottom);
	}
	if (Island && FMath::Abs(LeftX - LandingX) > PenguinProjectilePrivate::IcebergLandingRecheck)
	{
		UpdateLanding(Island, LeftX, OldBottom);
	}
	VelocityZ = FMath::Max(VelocityZ - PenguinProjectilePrivate::IcebergGravity * Dt, -PenguinProjectilePrivate::IcebergMaxFallSpeed);
	float NewBottom = OldBottom + VelocityZ * Dt;
	if (VelocityZ > 0.f && Island && BoxHitsBlocks(Island, LeftX, NewBottom))
	{
		// Blown up into a block above: it stops rising there.
		NewBottom = OldBottom;
		VelocityZ = 0.f;
	}
	// Ice walls are lasting now: one raised under the falling iceberg (or anything else built there since the drop) is
	// what it lands on.
	const float BuiltTop = BlockTopBetween(LeftX, OldBottom, NewBottom);
	if (BuiltTop > LandTopZ)
	{
		LandTopZ = BuiltTop;
		bHasGround = true;
		bFloe = false;
	}
	const bool bLands = NewBottom <= LandTopZ;
	if (bLands)
	{
		NewBottom = LandTopZ;
	}
	SetActorLocation(FVector(LeftX, 0.f, NewBottom));
	UpdateMarker();

	// Penguins under it are pushed down (crushed against the ground), the ones at its side shoved away.
	PressPenguins(Island, OldBottom, NewBottom, static_cast<float>(LeftX - OldLeftX));

	// ... and whatever projectile is in its way: bombs go off, the rest breaks.
	const float MinX = static_cast<float>(LeftX);
	const float MaxX = MinX + GetWidth();
	const float SweptMinZ = FMath::Min(NewBottom, OldBottom);
	const float SweptMaxZ = FMath::Max(NewBottom, OldBottom) + GetWidth();
	TArray<APenguinProjectile*> Projectiles;
	APenguinProjectile::GetAll(GetWorld(), Projectiles);
	for (APenguinProjectile* Projectile : Projectiles)
	{
		const FVector P = Projectile->GetActorLocation();
		if (P.X > MinX && P.X < MaxX && P.Z > SweptMinZ - Projectile->GetRadius() && P.Z < SweptMaxZ)
		{
			Projectile->Shatter();
		}
	}

	if (bLands)
	{
		Land(bHasGround || bFloe);
	}
}

void AIcebergDrop::PressPenguins(const ABlockIsland* Island, float OldBottom, float NewBottom, float MovedX)
{
	using namespace PenguinProjectilePrivate;
	const float Width = GetWidth();
	const float MinX = static_cast<float>(GetActorLocation().X);
	const float MaxX = MinX + Width;
	const float MiddleX = MinX + Width * 0.5f;
	// Everything its box went through this frame.
	const float SweptMinZ = FMath::Min(NewBottom, OldBottom);
	const float SweptMaxZ = FMath::Max(NewBottom, OldBottom) + Width;
	APenguinCharacter* Dropper = DropOwner.Get();
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		if (!Penguin->IsAlive() || Penguin->IsHidden() || !Capsule || Penguin == Dropper)
		{
			continue; // your own iceberg never touches you (you end up on top of it if it lands on you)
		}
		const FVector Loc = Penguin->GetActorLocation();
		const float R = Capsule->GetScaledCapsuleRadius();
		const float H = Capsule->GetScaledCapsuleHalfHeight();
		if (Loc.X + R <= MinX + IcebergTouchMargin || Loc.X - R >= MaxX - IcebergTouchMargin || Loc.Z + H <= SweptMinZ || Loc.Z - H >= SweptMaxZ)
		{
			continue;
		}
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const FVector Velocity = Move ? Move->Velocity : FVector::ZeroVector;

		if (Loc.X < MinX || Loc.X > MaxX)
		{
			// Only its side (the middle of the penguin is outside its columns): shoved out of the way to that side, not
			// hurt - onto ground when it stood on ground (no pit, no sea), else left alone (on top of the ice once it lands).
			const float Side = Loc.X < MiddleX ? -1.f : 1.f;
			const FVector Target(Side < 0.f ? MinX - R - IcebergShoveGap : MaxX + R + IcebergShoveGap, 0.f, Loc.Z);
			const bool bOnGround = Move && Move->IsMovingOnGround();
			float GroundZ = 0.f;
			if (Island && (!Island->CanFitCapsule(Target, R, H)
				|| (bOnGround && !Island->FindGroundBelow(Target - FVector(0.f, 0.f, H - IcebergShoveGap), IcebergShoveMaxDrop, GroundZ))))
			{
				continue;
			}
			// NetTeleport also moves a LAN player's penguin on its own machine (whose simulation owns its position).
			const float Pushed = FMath::Abs(static_cast<float>(Target.X - Loc.X));
			const float Along = static_cast<float>(Velocity.X) * Side;
			// Flying sideways into it, the iceberg pushes it along ahead of itself.
			const float Ahead = VelocityX * Side > 0.f ? FMath::Abs(VelocityX) * IcebergPushAhead : 0.f;
			if (Pushed < IcebergNudgeDistance && Ahead <= 0.f)
			{
				Penguin->NetTeleport(Target, Velocity, false); // only grazing it: set aside, nothing else
				continue;
			}
			const FVector Shove(Side * FMath::Max3(Along, IcebergShoveSpeed, Ahead), 0.f, bOnGround ? IcebergShoveHop : FMath::Min(static_cast<float>(Velocity.Z), 0.f));
			Penguin->NetTeleport(Target, Shove, true);
			if (Pushed >= IcebergNudgeDistance)
			{
				APinguFX::Puffs(this, FVector(Target.X - Side * R, 0.f, Loc.Z + H * 0.5f), 3, 24.f, 160.f, FVector(Side, 0.f, 0.3f), 0.8f, FLinearColor(0.85f, 0.93f, 1.f));
				APinguFX::Sound(this, EPinguSound::SnowballHit, 0.4f, 0.08f, 0.8f);
			}
			continue;
		}

		// Its middle is within the columns. Above the iceberg's middle it is on top of it (or inside its upper half): left
		// alone - it stands on the ice once it lands. Not reached by the bottom any more (the iceberg was blown up): nothing.
		if (Loc.Z >= NewBottom + Width * 0.5f || Loc.Z + H <= NewBottom)
		{
			continue;
		}
		if (IsSparedByIceberg(Penguin, Dropper) || Spared.ContainsByPredicate([Penguin](const TWeakObjectPtr<APenguinCharacter>& Other) { return Other.Get() == Penguin; }))
		{
			continue; // it passes through them: they end up on top of the ice when it lands
		}
		// Under it: pushed down with it, kept just under its bottom - and crushed once that presses it against the ground
		// (or blocks) below. Over the sea nothing stops it: it is pushed down into the sea (the sea knocks it out).
		const float TargetZ = NewBottom - H - IcebergCarryGap;
		const float Feet = static_cast<float>(Loc.Z) - H;
		const float TargetFeet = TargetZ - H;
		if (Feet <= TargetFeet)
		{
			continue; // already low enough
		}
		float GroundZ = 0.f;
		const bool bPressed = Island && Island->FindGroundUnderCapsule(Loc, R, H, Feet - TargetFeet + IcebergPressTolerance, GroundZ)
			&& GroundZ >= TargetFeet - IcebergPressTolerance;
		if (bPressed)
		{
			if (!CrushPenguin(Penguin, Dropper, MiddleX))
			{
				Spared.Add(Penguin); // (the ice took the hit ...): it is not pushed down again
			}
			continue;
		}
		// Carried along sideways too when the iceberg is blown (unless a block is in the way there).
		FVector Carried(Loc.X + MovedX, 0.f, TargetZ);
		if (MovedX != 0.f && Island && !Island->CanFitCapsule(Carried, R, H))
		{
			Carried.X = Loc.X;
		}
		Penguin->NetTeleport(Carried, FVector(Velocity.X, 0.f, FMath::Min(VelocityZ, static_cast<float>(Velocity.Z))), true);
	}
}

void AIcebergDrop::TickRemote(float Dt)
{
	using namespace PenguinProjectilePrivate;
	// The same warning and fall as on the server, so it falls smoothly between updates.
	if (WarnLeft > 0.f)
	{
		WarnLeft -= Dt;
	}
	else if (bHeld)
	{
		// Hangs where the server holds it (no fall or flight carried on); it falls on from rest when it lets go.
		VelocityZ = 0.f;
		VelocityX = 0.f;
	}
	else
	{
		const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), GetActorLocation());
		// Blown sideways: the same flight and the same landing as on the server (our island has the same blocks).
		if (Island && VelocityX != 0.f)
		{
			StepSideways(Island, Dt, NetX, static_cast<float>(NetZ));
		}
		if (Island && FMath::Abs(NetX - LandingX) > IcebergLandingRecheck)
		{
			UpdateLanding(Island, NetX, static_cast<float>(NetZ));
		}
		VelocityZ = FMath::Max(VelocityZ - IcebergGravity * Dt, -IcebergMaxFallSpeed);
		const double OldNetZ = NetZ;
		NetZ += VelocityZ * Dt;
		if (VelocityZ > 0.f && Island && BoxHitsBlocks(Island, NetX, static_cast<float>(NetZ)))
		{
			NetZ = OldNetZ; // blown up into a block: it stops rising there
			VelocityZ = 0.f;
		}
		// Stops on blocks built under it since the drop, as the server's does (our island has the same blocks).
		LandTopZ = FMath::Max(LandTopZ, BlockTopBetween(NetX, static_cast<float>(OldNetZ), static_cast<float>(NetZ)));
	}
	if (NetZ <= LandTopZ)
	{
		NetZ = LandTopZ; // waits on the ground until the server's landing removes it
		VelocityZ = 0.f;
		VelocityX = 0.f;
	}
	// A steady flight's correction is timing: fade it slowly (no wobble at the update rate), anything else fast.
	const bool bTimingZ = bNetSteady && FMath::Abs(NetErrorZ) < NetAlongTimingLimit * FMath::Abs(VelocityZ);
	NetErrorZ *= FMath::Exp(-(bTimingZ ? NetAlongDecay : NetErrorDecay) * Dt);
	const bool bTimingX = bNetSteady && FMath::Abs(NetErrorX) < NetAlongTimingLimit * FMath::Abs(VelocityX);
	NetErrorX *= FMath::Exp(-(bTimingX ? NetAlongDecay : NetErrorDecay) * Dt);
	SetActorLocation(FVector(NetX + NetErrorX, 0.f, FMath::Max(NetZ + NetErrorZ, static_cast<double>(LandTopZ))));
	UpdateMarker();
}

void AIcebergDrop::PostNetReceiveVelocity(const FVector& NewVelocity)
{
	using namespace PenguinProjectilePrivate;
	bNetSteady = false;
	const float NewVelocityX = static_cast<float>(NewVelocity.X);
	const float NewVelocityZ = static_cast<float>(NewVelocity.Z);
	if (FMath::Abs(NewVelocityZ) > 1.f || FMath::Abs(NewVelocityX) > 1.f)
	{
		// The server's iceberg is falling (or blown along): so is ours, as fast.
		bNetSteady = VelocityZ < -1.f && NewVelocityZ < -1.f && FMath::Abs(NewVelocityZ - VelocityZ) < 0.15f * FMath::Abs(VelocityZ)
			&& FMath::Abs(NewVelocityX - VelocityX) < IcebergNetSteadyX;
		WarnLeft = 0.f;
		VelocityZ = NewVelocityZ;
		VelocityX = NewVelocityX;
	}
	else if (WarnLeft <= 0.f)
	{
		VelocityX = 0.f; // held by frost: no sideways flight carried on either
	}
}

void AIcebergDrop::PostNetReceiveLocationAndRotation()
{
	const FVector NewLocation = FRepMovement::RebaseOntoLocalOrigin(GetReplicatedMovement().Location, this);
	NetErrorZ = 0.f;
	NetErrorX = 0.f;
	if (HasActorBegunPlay())
	{
		// Keep drawing it where it is and let the difference fade out.
		NetErrorZ = static_cast<float>(GetActorLocation().Z - NewLocation.Z);
		NetErrorX = static_cast<float>(GetActorLocation().X - NewLocation.X);
		if (FMath::Abs(NetErrorZ) > PenguinProjectilePrivate::IcebergNetSnap || FMath::Abs(NetErrorX) > PenguinProjectilePrivate::IcebergNetSnap)
		{
			NetErrorZ = 0.f;
			NetErrorX = 0.f;
		}
	}
	NetZ = NewLocation.Z;
	NetX = NewLocation.X;
	SetActorLocation(FVector(NetX + NetErrorX, 0.f, NetZ + NetErrorZ));
}

float AIcebergDrop::GroundTopUnder(const ABlockIsland* Island, double LeftX, float FromBottom, float MaxDrop) const
{
	using namespace PenguinProjectilePrivate;
	float Top = TNumericLimits<float>::Lowest();
	// Every column its box reaches into (two when it sits on the column grid, three between).
	const int32 FirstColumn = Island->WorldToCell(FVector(LeftX + IcebergColumnInset, 0.f, FromBottom)).X;
	const int32 LastColumn = Island->WorldToCell(FVector(LeftX + GetWidth() - IcebergColumnInset, 0.f, FromBottom)).X;
	for (int32 Column = FirstColumn; Column <= LastColumn; ++Column)
	{
		const FVector Probe(Island->CellToWorld(FIntPoint(Column, 0)).X, 0.f, FromBottom + 1.f);
		float GroundZ = 0.f;
		if (Island->FindGroundBelow(Probe, MaxDrop, GroundZ))
		{
			Top = FMath::Max(Top, GroundZ);
		}
	}
	return Top;
}

bool AIcebergDrop::BoxHitsBlocks(const ABlockIsland* Island, double LeftX, float Bottom) const
{
	using namespace PenguinProjectilePrivate;
	const float Width = GetWidth();
	const FIntPoint Low = Island->WorldToCell(FVector(LeftX + IcebergColumnInset, 0.f, Bottom + IcebergColumnInset));
	const FIntPoint High = Island->WorldToCell(FVector(LeftX + Width - IcebergColumnInset, 0.f, Bottom + Width - IcebergColumnInset));
	for (int32 Column = Low.X; Column <= High.X; ++Column)
	{
		for (int32 Row = Low.Y; Row <= High.Y; ++Row)
		{
			if (Island->IsSolidCell(FIntPoint(Column, Row)))
			{
				return true;
			}
		}
	}
	return false;
}

void AIcebergDrop::StepSideways(const ABlockIsland* Island, float Dt, double& InOutX, float Bottom)
{
	const double NewX = InOutX + VelocityX * Dt;
	if (!BoxHitsBlocks(Island, NewX, Bottom))
	{
		InOutX = NewX;
		return;
	}
	// A block in the way: it stops flush against it, on the column grid (as close as there is room), and falls on from
	// there.
	const double IslandX = Island->GetActorLocation().X;
	const bool bRight = VelocityX > 0.f;
	VelocityX = 0.f;
	for (const double From : { NewX, InOutX })
	{
		const double Cells = (From - IslandX) / CellSize;
		const double Flush = IslandX + (bRight ? FMath::FloorToDouble(Cells) : FMath::CeilToDouble(Cells)) * CellSize;
		if (!BoxHitsBlocks(Island, Flush, Bottom))
		{
			InOutX = Flush;
			return;
		}
	}
}

void AIcebergDrop::UpdateLanding(const ABlockIsland* Island, double LeftX, float Bottom)
{
	using namespace PenguinProjectilePrivate;
	LandingX = LeftX;
	// The first ground under any of its columns, however far down.
	const FBox Bounds = Island->GetBounds();
	const float MaxDrop = Bounds.IsValid ? FMath::Max(Bottom - static_cast<float>(Bounds.Min.Z), 0.f) + CellSize : 0.f;
	const float GroundTop = MaxDrop > 0.f ? GroundTopUnder(Island, LeftX, Bottom, MaxDrop) : TNumericLimits<float>::Lowest();
	if (GroundTop > TNumericLimits<float>::Lowest())
	{
		LandTopZ = GroundTop;
		bHasGround = true;
		bFloe = false;
		return;
	}
	bHasGround = false;
	if (FloeTopZ > IcebergNoFloe)
	{
		// Dropped over the sea and still over it: a floe at its height (or right where it is, if it is lower by now).
		LandTopZ = FMath::Min(FloeTopZ, Bottom);
		bFloe = true;
		return;
	}
	// Blown off the island over the sea: it splashes in.
	LandTopZ = Island->SeaLevel + IcebergSeaSurface;
	bFloe = false;
}

float AIcebergDrop::BlockTopBetween(double LeftX, float OldBottom, float NewBottom) const
{
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), GetActorLocation());
	if (!Island || NewBottom >= OldBottom)
	{
		return TNumericLimits<float>::Lowest();
	}
	const float GroundZ = GroundTopUnder(Island, LeftX, OldBottom, OldBottom - NewBottom + 1.f);
	return GroundZ >= NewBottom ? GroundZ : TNumericLimits<float>::Lowest();
}

bool AIcebergDrop::Overlaps(const FVector& Center, float InRadius) const
{
	const float MinX = GetActorLocation().X;
	const float MaxX = MinX + GetWidth();
	const float MinZ = GetActorLocation().Z;
	const float MaxZ = MinZ + GetWidth();
	const float Cx = FMath::Clamp(static_cast<float>(Center.X), MinX, MaxX);
	const float Cz = FMath::Clamp(static_cast<float>(Center.Z), MinZ, MaxZ);
	const float Dx = static_cast<float>(Center.X) - Cx;
	const float Dz = static_cast<float>(Center.Z) - Cz;
	return Dx * Dx + Dz * Dz <= InRadius * InRadius;
}

void AIcebergDrop::Shatter()
{
	if (!HasAuthority() || IsActorBeingDestroyed())
	{
		return;
	}
	FPinguFXBroadcastScope FXScope(this);
	const FVector Center(GetActorLocation().X + GetWidth() * 0.5f, 0.f, GetActorLocation().Z + GetWidth() * 0.5f);
	APinguFX::Shards(this, Center, 16, 1000.f, FLinearColor(0.6f, 0.9f, 1.f));
	APinguFX::Puffs(this, Center, 8, 40.f, 400.f, FVector::UpVector, 1.f, FLinearColor(0.85f, 0.95f, 1.f));
	APinguFX::Sound(this, EPinguSound::Shatter);
	Destroy();
}

void AIcebergDrop::Hold(float Seconds)
{
	// Only once it falls (during the warning it is not there yet), and not once it is gone.
	if (!HasAuthority() || WarnLeft > 0.f || Seconds <= 0.f || IsActorBeingDestroyed())
	{
		return;
	}
	HoldLeft = FMath::Max(HoldLeft, Seconds);
	VelocityZ = 0.f; // it falls on from rest when it lets go (a blown one stops flying sideways too)
	VelocityX = 0.f;
	if (bHeld)
	{
		return; // frost holds it every frame of a breath: the effects only once
	}
	bHeld = true; // replicated: every client shows it frosty and keeps it hanging
	ApplyIceLook();
	ForceNetUpdate();
	FPinguFXBroadcastScope FXScope(this);
	const float Width = GetWidth();
	const FVector Center(GetActorLocation().X + Width * 0.5f, 0.f, GetActorLocation().Z + Width * 0.5f);
	APinguFX::Sparkles(this, Center, 12, Width * 0.5f, FLinearColor(0.75f, 0.95f, 1.f));
	APinguFX::Sound(this, EPinguSound::Freeze, 0.6f);
}

bool AIcebergDrop::IsHeld() const
{
	return bHeld;
}

bool AIcebergDrop::IsInArea(const FVector& Origin, float AreaRadius, const FVector& ConeDirection, float CosHalfAngle) const
{
	const FVector Corner = GetActorLocation();
	const float Width = GetWidth();
	return PenguinProjectilePrivate::BoxInArea(FVector2D(Corner.X, Corner.Z), FVector2D(Corner.X + Width, Corner.Z + Width), Origin, AreaRadius,
		ConeDirection, CosHalfAngle);
}

void AIcebergDrop::Blow(float DirectionX, float Strength)
{
	using namespace PenguinProjectilePrivate;
	// Only once it falls (during the warning it is not there yet), and not once it is gone.
	if (!HasAuthority() || WarnLeft > 0.f || Strength <= 0.f || IsActorBeingDestroyed())
	{
		return;
	}
	const float Side = DirectionX < 0.f ? -1.f : 1.f;
	// Sideways along the wind and a little up (a falling one stops falling for a moment); it falls on from there. The
	// velocity reaches the clients with the replicated movement.
	VelocityX = Side * IcebergBlowSpeed * Strength;
	VelocityZ = FMath::Max(VelocityZ, IcebergBlowLift * Strength);
	if (bHeld)
	{
		// Frost held it: the wind takes it away all the same.
		HoldLeft = 0.f;
		bHeld = false;
		ApplyIceLook();
	}
	ForceNetUpdate();
	FPinguFXBroadcastScope FXScope(this);
	const float Width = GetWidth();
	const FVector Corner = GetActorLocation();
	// Ice dust blown off its windward side, along the wind; a heavy knock of ice.
	const FVector Windward(Side < 0.f ? Corner.X + Width : Corner.X, 0.f, Corner.Z + Width * 0.5f);
	APinguFX::Puffs(this, Windward, 6, 34.f, 520.f, FVector(Side, 0.f, 0.25f), 0.6f, FLinearColor(0.85f, 0.95f, 1.f));
	APinguFX::Sparkles(this, Windward, 5, Width * 0.4f, FLinearColor(0.75f, 0.95f, 1.f));
	APinguFX::Sound(this, EPinguSound::IcicleHit, 0.8f, 0.05f, 0.6f);
}

int32 AIcebergDrop::BlowInArea(UWorld* World, APenguinCharacter* By, const FVector& Origin, float AreaRadius, const FVector& ConeDirection, float CosHalfAngle, float Strength)
{
	using namespace PenguinProjectilePrivate;
	if (!World || IsNetClient(World))
	{
		return 0;
	}
	int32 Count = 0;
	for (TActorIterator<AIcebergDrop> It(World); It; ++It)
	{
		AIcebergDrop* Iceberg = *It;
		// A falling one the wind reaches (not the blower's own, like its own shots).
		if (!Iceberg->IsFalling() || Iceberg->IsActorBeingDestroyed() || Iceberg->GetDropOwner() == By
			|| !Iceberg->IsInArea(Origin, AreaRadius, ConeDirection, CosHalfAngle))
		{
			continue;
		}
		// The way it goes: along an aimed blast (the noot) that points sideways enough; else (a gust all around, a blast
		// straight up) away from the wind's middle - right above it, the way the blower faces.
		float DirectionX = 0.f;
		if (CosHalfAngle > -1.f && FMath::Abs(static_cast<float>(ConeDirection.X)) >= IcebergBlowMinAimX)
		{
			DirectionX = static_cast<float>(ConeDirection.X);
		}
		else
		{
			const float Dx = static_cast<float>(Iceberg->GetActorLocation().X + Iceberg->GetWidth() * 0.5f - Origin.X);
			DirectionX = FMath::Abs(Dx) > IcebergBlowCenterBand ? Dx : (By ? By->GetFacing() : 1.f);
		}
		Iceberg->Blow(DirectionX, Strength);
		++Count;
	}
	return Count;
}

void AIcebergDrop::OnRep_Held()
{
	ApplyIceLook();
}

void AIcebergDrop::ApplyIceLook()
{
	if (IceMaterial)
	{
		PenguinProjectilePrivate::SetIcebergIceLook(IceMaterial, bHeld);
	}
}

void AIcebergDrop::Land(bool bOnIsland)
{
	using namespace PenguinProjectilePrivate;
	ABlockIsland* Island = bOnIsland ? ABlockIsland::FindNear(GetWorld(), GetActorLocation()) : nullptr;
	if (Island)
	{
		// Blown sideways it may have come down between two columns: its ice settles into the nearer two (they are no
		// higher than where it stopped, and free above it).
		const float IslandX = static_cast<float>(Island->GetActorLocation().X);
		LeftColumn = FMath::RoundToInt((static_cast<float>(GetActorLocation().X) - IslandX) / CellSize);
		SetActorLocation(FVector(IslandX + LeftColumn * CellSize, 0.f, GetActorLocation().Z));
	}
	const FVector Center(GetActorLocation().X + GetWidth() * 0.5f, 0.f, LandTopZ);
	if (bOnIsland)
	{
		const float TopZ = LandTopZ + GetWidth();
		// It stays as lasting ice blocks of the island (island edits reach every LAN client by themselves: the new ice
		// collides there exactly as here). No crater: it lands on top of whatever is there.
		if (Island)
		{
			const int32 BottomRow = FMath::RoundToInt((LandTopZ - Island->GetActorLocation().Z) / CellSize);
			TArray<FIntPoint> NewCells;
			for (int32 X = 0; X < SizeCells; ++X)
			{
				for (int32 Z = 0; Z < SizeCells; ++Z)
				{
					NewCells.Add(FIntPoint(LeftColumn + X, BottomRow + Z));
				}
			}
			// A grown dropper's big blocks: each 2x2 cells, on the cells' lattice from its bottom-left corner.
			TArray<FIntPoint> BigOrigins;
			for (int32 X = 0; BlockSpan > 1 && X + BlockSpan <= SizeCells; X += BlockSpan)
			{
				for (int32 Z = 0; Z + BlockSpan <= SizeCells; Z += BlockSpan)
				{
					BigOrigins.Add(FIntPoint(LeftColumn + X, BottomRow + Z));
				}
			}
			if (bFloe)
			{
				// Over the sea: a floe that stays where it stopped (as sand it would sink into the sea).
				Island->AddBlocks(NewCells, EBlockKind::Ice, 0.f, &BigOrigins);
			}
			else
			{
				// On ground: ice that behaves like sand - each column piles up on whatever is under it. It stopped on the
				// highest of its columns: the ice of a column with a lower floor (or a gap) falls on down from here, and
				// it crushes only a penguin it really comes down on (the island's falling sand: pushed down, crushed once
				// pressed against what is below - knocked out by the iceberg, never its owner). Whoever is down there when
				// the other column lands can still get out from under it; shots there break once the ice reaches them. A big
				// block is one piece of that sand: held up by any of its columns, else it falls on as one.
				Island->AddSandBlocks(NewCells, EBlockKind::Ice, DropOwner.Get(), true, &BigOrigins);
			}
		}
		// Whoever survived under it (its owner, a shielded penguin) ends up standing on top, not inside the ice.
		const float MinX = GetActorLocation().X;
		const float MaxX = MinX + GetWidth();
		for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
		{
			APenguinCharacter* Penguin = *It;
			if (!Penguin->IsAlive() || Penguin->IsHidden())
			{
				continue;
			}
			const FVector Loc = Penguin->GetActorLocation();
			const float R = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
			const float H = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			if (Loc.X + R > MinX + 2.f && Loc.X - R < MaxX - 2.f && Loc.Z + H > LandTopZ && Loc.Z - H < TopZ)
			{
				LiftOnTop(Penguin, TopZ);
			}
		}
		APinguFX::Shards(this, Center + FVector(0.f, 0.f, 60.f), 12, 900.f, FLinearColor(0.6f, 0.9f, 1.f));
		APinguFX::Ring(this, Center + FVector(0.f, 0.f, 40.f), 260.f, FLinearColor(0.8f, 0.95f, 1.f), 0.35f);
		APinguFX::Sound(this, EPinguSound::IcebergCrash);
		if (bFloe)
		{
			APinguFX::Sparkles(this, Center + FVector(0.f, 0.f, 20.f), 10, 90.f, FLinearColor(0.7f, 0.93f, 1.f));
			APinguFX::Shake(this, 0.25f);
		}
		else
		{
			APinguFX::GroundPuffs(this, Center, 2.f);
			APinguFX::Shake(this, 0.6f);
		}
	}
	else
	{
		APinguFX::Splash(this, Center);
		APinguFX::Sound(this, EPinguSound::Splash);
	}
	Destroy();
}

// ============================================================================ an ice block the fish hook pulled out

AHookedIceBlock::AHookedIceBlock()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	SetActorEnableCollision(false);

	// LAN: spawned by the server only; every client flies its own copy against its own island. It has no collision itself
	// (the island's blocks are the collision - and it is one of them again where it lands).
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(60.f);
	SetMinNetUpdateFrequency(30.f);
	NetPriority = 2.f;

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = SceneRoot;

	Blocks = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Blocks"));
	Blocks->SetupAttachment(SceneRoot);
	Blocks->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AHookedIceBlock::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(AHookedIceBlock, CellSize, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AHookedIceBlock, bBlown, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AHookedIceBlock, bTornado, COND_InitialOnly);
	DOREPLIFETIME(AHookedIceBlock, bSwirling);
}

AHookedIceBlock* AHookedIceBlock::Pull(ABlockIsland* InIsland, const FVector& InsidePoint, APenguinCharacter* InThrower)
{
	using namespace PenguinProjectilePrivate;
	UWorld* World = InIsland ? InIsland->GetWorld() : nullptr;
	if (!World || !InThrower || IsNetClient(World) || InIsland->GetBlockAt(InsidePoint) != EBlockKind::Ice)
	{
		return nullptr;
	}
	// Out of the island first (not broken): where it comes down is looked up without it.
	FIntPoint Taken;
	if (!InIsland->TakeIceBlockAt(InsidePoint, Taken))
	{
		return nullptr;
	}
	const float Cell = InIsland->CellSize;
	const FVector Start = InIsland->CellToWorld(Taken);
	const FVector ThrowerAt = InThrower->GetActorLocation();

	// About halfway to the thrower, onto the middle of a column (it is a block of that column again).
	const float Dx = static_cast<float>(ThrowerAt.X - Start.X);
	const float Side = FMath::Abs(Dx) > 20.f ? FMath::Sign(Dx) : -InThrower->GetFacing();
	const int32 LandColumn = InIsland->WorldToCell(FVector(Start.X + Dx * HookHeavyShare, 0.f, Start.Z)).X;
	const float LandWorldX = static_cast<float>(InIsland->CellToWorld(FIntPoint(LandColumn, Taken.Y)).X);
	const float LandX = FMath::Max((LandWorldX - static_cast<float>(Start.X)) * Side, 0.f);
	const float LandZ = HeavyLandingZ(World, LandWorldX, Start, InThrower, Cell * 0.5f) - static_cast<float>(Start.Z);

	// Heavy: it falls like a block of the island (no air drag).
	FArcPhysics Physics;
	Physics.GravityUp = ABlockIsland::FallingBlockGravity;
	Physics.GravityDown = ABlockIsland::FallingBlockGravity;
	Physics.MaxFallSpeed = ABlockIsland::FallingBlockMaxSpeed;
	float LobTime = 0.f;
	const FVector2D Launch = SolveHeavyLob(Physics, LandX, LandZ, LobTime);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AHookedIceBlock* Block = World->SpawnActor<AHookedIceBlock>(Start, FRotator::ZeroRotator, Params);
	if (!Block)
	{
		InIsland->AddBlocks({ Taken }, EBlockKind::Ice, 0.f); // (back where it was)
		return nullptr;
	}
	Block->Island = InIsland;
	Block->Thrower = InThrower;
	Block->CellSize = Cell;
	Block->Position = FVector2D(Start.X, Start.Z);
	Block->Velocity2D = FVector2D(Launch.X * Side, Launch.Y);
	Block->FlightTime = LobTime;
	Block->BuildLook();
	FPinguFXBroadcastScope FXScope(World);
	APinguFX::Puffs(Block, Start, 4, 26.f, 220.f, FVector(Side, 0.f, 0.4f).GetSafeNormal(), 0.8f, FLinearColor(0.85f, 0.95f, 1.f));
	APinguFX::Sound(Block, EPinguSound::IcicleHit, 0.9f, 0.05f, 0.55f); // a deep crack: it comes out whole
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s's fish hook pulls an ice block out of the island: %.0f cm toward it (%.2f s)"),
		*InThrower->GetDisplayName(), LandX, LobTime);
	return Block;
}

AHookedIceBlock* AHookedIceBlock::Blow(ABlockIsland* InIsland, const FVector& InsidePoint, APenguinCharacter* Blower, const FVector2D& InVelocity)
{
	using namespace PenguinProjectilePrivate;
	UWorld* World = InIsland ? InIsland->GetWorld() : nullptr;
	if (!World || IsNetClient(World) || InIsland->GetBlockAt(InsidePoint) != EBlockKind::Ice)
	{
		return nullptr;
	}
	// Out of the island (not broken: no shards, not counted as broken; the sand on it falls), then it flies on its own.
	FIntPoint Taken;
	if (!InIsland->TakeIceBlockAt(InsidePoint, Taken))
	{
		return nullptr;
	}
	const FVector Start = InIsland->CellToWorld(Taken);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AHookedIceBlock* Block = World->SpawnActor<AHookedIceBlock>(Start, FRotator::ZeroRotator, Params);
	if (!Block)
	{
		InIsland->AddBlocks({ Taken }, EBlockKind::Ice, 0.f); // (back where it was)
		return nullptr;
	}
	// All set before its first replication (a client starts its own copy from these).
	Block->Island = InIsland;
	Block->Thrower = Blower;
	Block->bBlown = true;
	Block->CellSize = InIsland->CellSize;
	Block->Position = FVector2D(Start.X, Start.Z);
	Block->Velocity2D = InVelocity;
	Block->BuildLook();
	FPinguFXBroadcastScope FXScope(World);
	APinguFX::Puffs(Block, Start, 2, 24.f, 200.f, FVector(InVelocity.X, 0.f, InVelocity.Y).GetSafeNormal(), 0.8f, FLinearColor(0.85f, 0.95f, 1.f));
	return Block;
}

AHookedIceBlock* AHookedIceBlock::RipOut(ABlockIsland* InIsland, const FVector& InsidePoint, APenguinCharacter* Ripper, FIntPoint* OutCell)
{
	using namespace PenguinProjectilePrivate;
	UWorld* World = InIsland ? InIsland->GetWorld() : nullptr;
	if (!World || IsNetClient(World) || InIsland->GetBlockAt(InsidePoint) != EBlockKind::Ice)
	{
		return nullptr;
	}
	// Out of the island (not broken: no shards, not counted as broken; the sand on it falls); the tornado carries it on.
	FIntPoint Taken;
	if (!InIsland->TakeIceBlockAt(InsidePoint, Taken))
	{
		return nullptr;
	}
	const FVector Start = InIsland->CellToWorld(Taken);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AHookedIceBlock* Block = World->SpawnActor<AHookedIceBlock>(Start, FRotator::ZeroRotator, Params);
	if (!Block)
	{
		InIsland->AddBlocks({ Taken }, EBlockKind::Ice, 0.f); // (back where it was)
		return nullptr;
	}
	// All set before its first replication (a client starts its own copy from these).
	Block->Island = InIsland;
	Block->Thrower = Ripper;
	Block->bBlown = true;
	Block->bTornado = true;
	Block->bSwirling = true;
	Block->CellSize = InIsland->CellSize;
	Block->Position = FVector2D(Start.X, Start.Z);
	Block->BuildLook();
	if (OutCell)
	{
		*OutCell = Taken;
	}
	FPinguFXBroadcastScope FXScope(World);
	APinguFX::Puffs(Block, Start, 3, 26.f, 220.f, FVector::UpVector, 0.9f, FLinearColor(0.85f, 0.95f, 1.f));
	return Block;
}

void AHookedIceBlock::Swirl(const FVector2D& NewCenter, const FVector2D& NewVelocity)
{
	if (!HasAuthority() || !bSwirling || bLanded)
	{
		return;
	}
	Position = NewCenter;
	Velocity2D = NewVelocity;
	SwirlAge = 0.f; // its tornado still holds it (a giant made of merged tornadoes can hold it longer than one)
	SetActorLocation(FVector(Position.X, 0.f, Position.Y));
}

void AHookedIceBlock::ThrowOut(const FVector2D& InVelocity, const TArray<TWeakObjectPtr<APenguinCharacter>>& Spare)
{
	if (!HasAuthority() || !bSwirling || bLanded)
	{
		return;
	}
	bSwirling = false;
	Velocity2D = InVelocity;
	FlightTime = 0.f;
	// The penguins the tornado threw itself fly already: this block never knocks them again.
	for (const TWeakObjectPtr<APenguinCharacter>& Penguin : Spare)
	{
		if (Penguin.IsValid())
		{
			HitAlready.AddUnique(Penguin);
		}
	}
	ForceNetUpdate(); // every machine flies it from here
}

int32 AHookedIceBlock::BlowInArea(UWorld* World, APenguinCharacter* By, const FVector& Origin, float AreaRadius, int32 MaxBlocks)
{
	using namespace PenguinProjectilePrivate;
	if (!World || IsNetClient(World) || MaxBlocks <= 0 || AreaRadius <= 0.f)
	{
		return 0;
	}
	ABlockIsland* Isle = ABlockIsland::FindNear(World, Origin);
	if (!Isle)
	{
		return 0;
	}
	// Never the floor the blower stands on (the row under its feet and a block to either side): no blowing the ground
	// away under yourself (as the gust used to break ice: the same blocks are safe).
	bool bHasSupport = false;
	FIntPoint SupportLeft(0, 0);
	FIntPoint SupportRight(0, 0);
	if (By && By->GetCapsuleComponent())
	{
		const FVector Feet = By->GetFeetLocation();
		const float FeetRadius = By->GetCapsuleComponent()->GetScaledCapsuleRadius();
		SupportLeft = Isle->WorldToCell(Feet + FVector(-FeetRadius, 0.f, -10.f)) - FIntPoint(1, 0);
		SupportRight = Isle->WorldToCell(Feet + FVector(FeetRadius, 0.f, -10.f)) + FIntPoint(1, 0);
		bHasSupport = true;
	}
	// Every ice block the wind reaches: its middle in the area, its face toward the wind's middle open and nothing solid on
	// the way there (walls shield what is behind them; the ice behind the first layer goes with the next gust). Nearest
	// first. (Once per gust: the list lives on the stack.)
	const FVector Middle(Origin.X, 0.f, Origin.Z);
	const FIntPoint MiddleCell = Isle->WorldToCell(Middle);
	const int32 Reach = FMath::CeilToInt(AreaRadius / Isle->CellSize) + 1;
	TArray<TPair<float, FIntPoint>, TInlineAllocator<64>> Found;
	for (int32 X = MiddleCell.X - Reach; X <= MiddleCell.X + Reach; ++X)
	{
		for (int32 Z = MiddleCell.Y - Reach; Z <= MiddleCell.Y + Reach; ++Z)
		{
			if (Isle->GetBlock(X, Z) != EBlockKind::Ice || (bHasSupport && Z == SupportLeft.Y && X >= SupportLeft.X && X <= SupportRight.X))
			{
				continue;
			}
			const FVector CellCenter = Isle->CellToWorld(FIntPoint(X, Z));
			const float Dx = static_cast<float>(CellCenter.X - Middle.X);
			const float Dz = static_cast<float>(CellCenter.Z - Middle.Z);
			const float Dist = FMath::Sqrt(Dx * Dx + Dz * Dz);
			if (Dist > AreaRadius)
			{
				continue;
			}
			// The wind hits the face toward its middle: that one must be open, and the way to it clear.
			const bool bAlongX = FMath::Abs(Dx) >= FMath::Abs(Dz);
			const FIntPoint Toward = bAlongX ? FIntPoint(Dx > 0.f ? X - 1 : X + 1, Z) : FIntPoint(X, Dz > 0.f ? Z - 1 : Z + 1);
			if (Isle->IsSolidCell(Toward) || !Isle->IsLineClear(Middle, Isle->CellToWorld(Toward)))
			{
				continue;
			}
			Found.Add(TPair<float, FIntPoint>(Dist, FIntPoint(X, Z)));
		}
	}
	if (Found.Num() == 0)
	{
		return 0;
	}
	Found.Sort([](const TPair<float, FIntPoint>& A, const TPair<float, FIntPoint>& B) { return A.Key < B.Key; });
	const float Facing = By ? By->GetFacing() : 1.f;
	FPinguFXBroadcastScope FXScope(World);
	int32 Blown = 0;
	for (const TPair<float, FIntPoint>& Entry : Found)
	{
		if (Blown >= MaxBlocks)
		{
			break;
		}
		const FVector CellCenter = Isle->CellToWorld(Entry.Value);
		float AwayX = static_cast<float>(CellCenter.X - Middle.X);
		float AwayZ = static_cast<float>(CellCenter.Z - Middle.Z);
		if (Entry.Key > 1.f)
		{
			AwayX /= Entry.Key;
			AwayZ /= Entry.Key;
		}
		else
		{
			AwayX = Facing;
			AwayZ = 0.f;
		}
		// Right above or below the wind's middle it still goes off to a side (away from the middle, else the way the blower
		// faces) - never straight back down onto the blower.
		if (FMath::Abs(AwayX) < BlownIceMinSide)
		{
			AwayX = (FMath::Abs(AwayX) > UE_KINDA_SMALL_NUMBER ? FMath::Sign(AwayX) : Facing) * BlownIceMinSide;
		}
		// Hardest near the wind's middle; always a lift (it is heavy: it comes down a few blocks away).
		const float Strength = FMath::Lerp(1.f, BlownIceEdgeShare, FMath::Clamp(Entry.Key / AreaRadius, 0.f, 1.f));
		const FVector2D Launch(AwayX * BlownIceSpeed * Strength, FMath::Max(AwayZ, 0.f) * BlownIceSpeed * Strength + BlownIceLift);
		Blown += Blow(Isle, CellCenter, By, Launch) ? 1 : 0;
	}
	if (Blown > 0)
	{
		// One deep knock of ice for all of them.
		APinguFX::Sound(Isle, EPinguSound::IcicleHit, 0.85f, 0.05f, 0.55f);
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s's gust blows %d ice block(s) out of the island"), By ? *By->GetDisplayName() : TEXT("wind"), Blown);
	}
	return Blown;
}

void AHookedIceBlock::BeginPlay()
{
	Super::BeginPlay();
	if (!HasAuthority())
	{
		// A LAN client: its own copy flies from here (Pull sets up the server's).
		Island = ABlockIsland::FindNear(GetWorld(), GetActorLocation());
		Position = FVector2D(GetActorLocation().X, GetActorLocation().Z);
		if (const ABlockIsland* Isle = Island.Get())
		{
			FromCell = Isle->WorldToCell(GetActorLocation());
			bIgnoreFromCell = true;
		}
		BuildLook();
	}
}

void AHookedIceBlock::BuildLook()
{
	// The island's own ability-ice block (sand ice looks the same: it is one again where it lands), as many rows deep.
	UStaticMesh* Mesh = nullptr;
	UMaterialInterface* Material = nullptr;
	int32 Rows = 3;
	if (const ABlockIsland* Isle = Island.Get())
	{
		Isle->GetAbilityIceLook(Mesh, Material);
		Rows = FMath::Max(Isle->DepthRows, 1);
	}
	if (!Mesh)
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Environment/SM_Block.SM_Block"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	}
	if (!Mesh)
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}
	Blocks->SetStaticMesh(Mesh);
	if (Material)
	{
		Blocks->SetMaterial(0, Material);
	}
	Blocks->ClearInstances();
	const float Scale = CellSize / 100.f;
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		const FVector Local(0.f, (Row - (Rows - 1) * 0.5f) * CellSize, 0.f);
		Blocks->AddInstance(FTransform(FRotator::ZeroRotator, Local, FVector(Scale)));
	}
}

bool AHookedIceBlock::TouchesBlocks(const FVector2D& Center) const
{
	using namespace PenguinProjectilePrivate;
	const ABlockIsland* Isle = Island.Get();
	if (!Isle)
	{
		return false;
	}
	const float Half = CellSize * 0.5f - HookedIceSkin;
	const FVector Origin = Isle->GetActorLocation();
	const int32 MinX = FMath::FloorToInt((Center.X - Half - Origin.X) / CellSize);
	const int32 MaxX = FMath::FloorToInt((Center.X + Half - Origin.X) / CellSize);
	const int32 MinZ = FMath::FloorToInt((Center.Y - Half - Origin.Z) / CellSize);
	const int32 MaxZ = FMath::FloorToInt((Center.Y + Half - Origin.Z) / CellSize);
	for (int32 X = MinX; X <= MaxX; ++X)
	{
		for (int32 Z = MinZ; Z <= MaxZ; ++Z)
		{
			if (Isle->IsSolidCell(FIntPoint(X, Z)) && !(bIgnoreFromCell && FromCell == FIntPoint(X, Z)))
			{
				return true;
			}
		}
	}
	return false;
}

bool AHookedIceBlock::Step(float Dt)
{
	using namespace PenguinProjectilePrivate;
	const ABlockIsland* Isle = Island.Get();
	const FVector Origin = Isle ? Isle->GetActorLocation() : FVector::ZeroVector;
	// In steps of at most HookedIceMaxStep, so it never skips a block.
	const float Fastest = FMath::Max(FMath::Abs(Velocity2D.X), FMath::Abs(Velocity2D.Y) + ABlockIsland::FallingBlockGravity * Dt);
	const int32 Steps = FMath::Clamp(FMath::CeilToInt(Fastest * Dt / HookedIceMaxStep), 1, 32);
	const float H = Dt / Steps;
	for (int32 Index = 0; Index < Steps; ++Index)
	{
		Velocity2D.Y = FMath::Max(Velocity2D.Y - ABlockIsland::FallingBlockGravity * H, -ABlockIsland::FallingBlockMaxSpeed);
		// Sideways: a wall stops it on the middle of the column it is in (it drops from there).
		if (!FMath::IsNearlyZero(Velocity2D.X))
		{
			const FVector2D Next(Position.X + Velocity2D.X * H, Position.Y);
			if (TouchesBlocks(Next))
			{
				const int32 Column = FMath::FloorToInt((Position.X - Origin.X) / CellSize);
				Position.X = static_cast<float>(Origin.X) + (Column + 0.5f) * CellSize;
				Velocity2D.X = 0.f;
			}
			else
			{
				Position.X = Next.X;
			}
		}
		// Up and down: onto a block it lands (on the middle of the row above it), into one above it stops rising.
		const FVector2D Next(Position.X, Position.Y + Velocity2D.Y * H);
		if (TouchesBlocks(Next))
		{
			const int32 Row = FMath::FloorToInt((Position.Y - Origin.Z) / CellSize);
			Position.Y = static_cast<float>(Origin.Z) + (Row + 0.5f) * CellSize;
			if (Velocity2D.Y <= 0.f)
			{
				Velocity2D = FVector2D::ZeroVector;
				return true;
			}
			Velocity2D.Y = 0.f;
		}
		else
		{
			Position.Y = Next.Y;
		}
	}
	return false;
}

void AHookedIceBlock::HitPenguins()
{
	using namespace PenguinProjectilePrivate;
	const FVector Center = GetActorLocation();
	const float Half = CellSize * 0.5f;
	ABlockIsland* Isle = Island.Get();
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		if (!Penguin->IsAlive() || Penguin->IsHidden() || Penguin == Thrower.Get())
		{
			continue; // the block never hits its own thrower
		}
		const FVector Loc = Penguin->GetActorLocation();
		const float R = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
		const float H = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		if (FMath::Abs(Loc.X - Center.X) >= Half + R - 4.f || FMath::Abs(Loc.Z - Center.Z) >= Half + H - 4.f)
		{
			continue;
		}
		// A tornado's block coming down onto a penguin is falling ice: it pushes it down with it and crushes it once it is
		// pressed against what is below (the tornado's user's knock-out) - whether or not the tornado threw that one.
		if (bTornado && Isle && Velocity2D.Y < 0.f)
		{
			bool bCrushed = false;
			const FBox Box(FVector(Center.X - Half, 0.f, Center.Z - Half), FVector(Center.X + Half, 0.f, Center.Z + Half));
			if (Isle->PressUnderFallingIce(Penguin, Box, -Velocity2D.Y, Thrower.Get(), bCrushed))
			{
				if (bCrushed)
				{
					++PenguinsCrushed;
					HitAlready.AddUnique(Penguin);
					UE_LOG(LogPingu, Display, TEXT("[GAME] a tornado's ice block came down on %s: crushed"), *Penguin->GetDisplayName());
				}
				continue;
			}
		}
		if (HitAlready.Contains(Penguin))
		{
			continue; // each penguin once
		}
		HitAlready.Add(Penguin);
		// Knocked along the block's way (dropping straight down onto it: aside, away from its middle).
		const float Way = FMath::Abs(Velocity2D.X) > 50.f ? FMath::Sign(Velocity2D.X) : (Loc.X >= Center.X ? 1.f : -1.f);
		FPenguinHit Hit;
		Hit.Instigator = Thrower.Get();
		// A blown block is a flying block of ice, not the wind itself (a gust's hit would clash with a penguin that has
		// just pushed, and bounce the blower far away): it counts as ice.
		Hit.Source = bBlown ? EPenguinAbility::IceWall : EPenguinAbility::FishHook;
		Hit.SourceSize = 1.f; // a block of ice: the same push whoever pulled or blew it (only the victim's size counts)
		Hit.Knockback = FVector2D(Way * HookedIceHitSpeed, HookedIceHitLift);
		Hit.Stun = HookedIceHitStun;
		Hit.Location = Loc;
		Penguin->ApplyPenguinHit(Hit);
	}
}

void AHookedIceBlock::Land(bool bInSea)
{
	using namespace PenguinProjectilePrivate;
	if (bLanded)
	{
		return;
	}
	bLanded = true;
	Velocity2D = FVector2D::ZeroVector;
	FPinguFXBroadcastScope FXScope(this);
	const FVector Center = GetActorLocation();
	ABlockIsland* Isle = Island.Get();
	if (bInSea || !Isle)
	{
		APinguFX::Splash(this, Center);
		APinguFX::Sound(this, EPinguSound::Splash, bBlown ? 0.5f : 1.f);
		Destroy();
		return;
	}
	// A block of the island again, as sand ice: resting on a block it is one at once (whoever stands there is moved out of
	// it), over a gap it falls on down its column. (Taken meanwhile: on top of what is there.)
	FIntPoint Cell = Isle->WorldToCell(Center);
	for (int32 Up = 0; Up < 4 && Isle->IsSolidCell(Cell); ++Up)
	{
		++Cell.Y;
	}
	const bool bOnBlock = Isle->IsSolidCell(FIntPoint(Cell.X, Cell.Y - 1));
	const bool bPlaced = Isle->AddSandBlocks({ Cell }, EBlockKind::Ice, Thrower.Get()).Num() > 0; // (falling on, it never crushes its thrower / blower)
	if (bPlaced)
	{
		// (A gust blows several at once: theirs are smaller and quieter, without a shake each.)
		const FVector Bottom(Center.X, 0.f, Center.Z - CellSize * 0.5f);
		APinguFX::Puffs(this, Bottom, bBlown ? 3 : 5, 30.f, 260.f, FVector::UpVector, 1.f, FLinearColor(0.85f, 0.95f, 1.f));
		APinguFX::Sound(this, EPinguSound::IcebergCrash, bBlown ? BlownIceLandVolume : 0.55f, 0.08f, bBlown ? 1.5f : 1.35f);
		if (!bBlown)
		{
			APinguFX::Shake(this, 0.2f);
		}
	}
	else
	{
		APinguFX::Shards(this, Center, bBlown ? 8 : 14, 800.f, FLinearColor(0.7f, 0.92f, 1.f));
		APinguFX::Sound(this, EPinguSound::Shatter, bBlown ? 0.5f : 0.8f);
	}
	UE_LOG(LogPingu, Display, TEXT("[GAME] the %s ice block came down at cell %d,%d: %s (penguins hit on the way %d)"), bBlown ? TEXT("blown") : TEXT("pulled"),
		Cell.X, Cell.Y, bPlaced ? (bOnBlock ? TEXT("a block again") : TEXT("falls on as sand")) : TEXT("no room - it broke"), HitAlready.Num());
	if (bPlaced && bOnBlock)
	{
		// It stays drawn a moment right where the island's new block is: the clients get that block before this one goes.
		const FVector Final = Isle->CellToWorld(Cell);
		Position = FVector2D(Final.X, Final.Z);
		SetActorLocation(Final);
		LingerLeft = HookedIceLinger;
		ForceNetUpdate();
		return;
	}
	Destroy();
}

void AHookedIceBlock::Tick(float DeltaSeconds)
{
	using namespace PenguinProjectilePrivate;
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(DeltaSeconds, 0.05f);
	Age += Dt;
	if (!HasAuthority())
	{
		// The same flight as on the server (our island has the same blocks); down on a block it waits for the server.
		if (bIgnoreFromCell && Age > HookedIceIgnoreFromCellTime)
		{
			bIgnoreFromCell = false; // our island has lost that block by now (or it is a new one: it counts)
		}
		if (bSwirling)
		{
			// In a tornado's wind: on along the server's last velocity (no gravity) until its next position comes.
			Position += Velocity2D * Dt;
		}
		else if (!bResting && Step(Dt))
		{
			bResting = true;
		}
		NetError *= FMath::Exp(-HookedIceNetErrorDecay * Dt);
		SetActorLocation(FVector(Position.X + NetError.X, 0.f, Position.Y + NetError.Y));
		return;
	}
	if (bSwirling)
	{
		// In a tornado's wind: it goes where the tornado puts it (Swirl). One whose tornado is gone drops after a while.
		constexpr float SwirlMaxTime = 1.5f;
		SwirlAge += Dt;
		if (SwirlAge > SwirlMaxTime)
		{
			ThrowOut(FVector2D::ZeroVector, {});
		}
		return;
	}
	if (bLanded)
	{
		LingerLeft -= Dt;
		if (LingerLeft <= 0.f)
		{
			Destroy();
		}
		return;
	}
	const bool bDown = Step(Dt);
	SetActorLocation(FVector(Position.X, 0.f, Position.Y));
	HitPenguins();
	const ABlockIsland* Isle = Island.Get();
	if (bDown)
	{
		Land(false);
	}
	else if (!Isle || Position.Y < Isle->SeaLevel + 60.f)
	{
		Land(true);
	}
	else if (Age > HookedIceMaxLife)
	{
		FPinguFXBroadcastScope FXScope(this);
		APinguFX::Shards(this, GetActorLocation(), 14, 800.f, FLinearColor(0.7f, 0.92f, 1.f));
		bLanded = true;
		Destroy();
	}
}

void AHookedIceBlock::PostNetReceiveVelocity(const FVector& NewVelocity)
{
	Velocity2D = FVector2D(NewVelocity.X, NewVelocity.Z);
}

void AHookedIceBlock::PostNetReceiveLocationAndRotation()
{
	const FVector NewLocation = FRepMovement::RebaseOntoLocalOrigin(GetReplicatedMovement().Location, this);
	const FVector2D Server(NewLocation.X, NewLocation.Z);
	NetError = FVector2D::ZeroVector;
	if (HasActorBegunPlay())
	{
		// Keep drawing it where it is and let the difference fade out.
		const FVector Shown = GetActorLocation();
		NetError = FVector2D(Shown.X, Shown.Z) - Server;
		if (NetError.Size() > 300.f)
		{
			NetError = FVector2D::ZeroVector;
		}
	}
	Position = Server;
	bResting = false; // flies on from where the server has it (down on a block, it rests again at once)
	SetActorLocation(FVector(Server.X + NetError.X, 0.f, Server.Y + NetError.Y));
}
