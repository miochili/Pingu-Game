// Pingu - the kill cam (card 2026-11-07_kill-cam). See PinguKillCam.h.

#include "PinguKillCam.h"
#include "BlockIsland.h"
#include "EngineUtils.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Net/UnrealNetwork.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PinguBuildLimit.h"
#include "PinguFX.h"
#include "PinguTraitTable.h"
#include "PinguTuning.h"

namespace PinguKillCamPrivate
{
	/** The flight simulation's step and how far ahead it looks (game seconds). */
	constexpr float SimStep = 1.f / 60.f;
	constexpr float SimHorizon = 4.f;
	/** A penguin this close above the sea surface is knocked out (APenguinCharacter::Tick). */
	constexpr float SeaKOAbove = 120.f;
	/** The camera: in, out (real seconds), the frame's room around the victim (cm), the narrowest frame, the hitter fits. */
	/** Under the winner banner: room above the kill for the banner (cm), and on both sides. */
	constexpr float BannerRoom = 700.f;
	constexpr float BannerPadding = 300.f;
	constexpr float ZoomOutTime = 0.6f;
	constexpr float ZoomHoldAfter = 0.8f;
	constexpr float HitterFitsX = 1800.f;
	constexpr float HitterFitsZ = 1000.f;
	/** A hit this recent counts its maker as the hitter of a fall (s of game time). */
	constexpr float HitterMemory = 3.f;

	/** What one ability could still do for a falling penguin (from AvailableAt s on). Every rule errs toward "saved": the slow
	 *  motion must never start while the victim could still get back (it would get more real time to react). */
	enum class ESaverKind : uint8
	{
		Hook,   // the hook grabs any block in reach and pulls it there (it flies HookSpeed after a short windup)
		Warp,   // Snow Warp: up to WarpRange onto (over) the island
		Wall,   // Ice Wall: a block under its feet anywhere inside the build limit
		Shell,  // Ice Shell: floats down slowly for ShellDuration (more time to steer back)
		Peg,    // Icicle: a peg into the cliff beside it is a jump pad back up
		Moves,  // a push of its own (recoil abilities: +1 jump; Gust: +1 jump +1 dash)
		Flap,
		None,
	};

	struct FSaver
	{
		ESaverKind Kind = ESaverKind::None;
		float AvailableAt = 0.f;
		int32 ExtraJumps = 0;
		int32 ExtraDashes = 0;
	};

	/** s: an ability's windup before it acts (a margin). */
	constexpr float AbilityWindup = 0.1f;
	/** cm: an icicle peg needs a cliff this close. */
	constexpr float PegReach = 320.f;

	FSaver SaverOf(EPenguinAbility Ability)
	{
		FSaver S;
		switch (Ability)
		{
		case EPenguinAbility::FishHook: S.Kind = ESaverKind::Hook; break;
		case EPenguinAbility::SnowWarp: S.Kind = ESaverKind::Warp; break;
		case EPenguinAbility::IceWall: S.Kind = ESaverKind::Wall; break;
		case EPenguinAbility::IceShell: S.Kind = ESaverKind::Shell; break;
		case EPenguinAbility::Icicle: S.Kind = ESaverKind::Peg; break;
		case EPenguinAbility::Gust: S.Kind = ESaverKind::Moves; S.ExtraJumps = 1; S.ExtraDashes = 1; break; // it hangs a moment, gives the air moves back
		case EPenguinAbility::FrostBreath:
		case EPenguinAbility::Noot:
		case EPenguinAbility::FlipperSlap:
		case EPenguinAbility::Snowball:
		case EPenguinAbility::IceGun:
			S.Kind = ESaverKind::Moves; S.ExtraJumps = 1; break; // recoil: a push of its own
		case EPenguinAbility::Flap: S.Kind = ESaverKind::Flap; break;
		default: break;
		}
		return S;
	}

	/** Distance from a point to a box on the play plane (X, Z). */
	float DistanceToBox(const FVector& P, const FBox& Box)
	{
		const float Dx = FMath::Max3(static_cast<float>(Box.Min.X - P.X), 0.f, static_cast<float>(P.X - Box.Max.X));
		const float Dz = FMath::Max3(static_cast<float>(Box.Min.Z - P.Z), 0.f, static_cast<float>(P.Z - Box.Max.Z));
		return FMath::Sqrt(Dx * Dx + Dz * Dz);
	}

	/** The capsule touches a block of the island (its feet, its sides, its head). */
	bool TouchesIsland(const ABlockIsland* Island, const FVector& P, float Radius, float HalfHeight)
	{
		const FVector Points[] = {
			P + FVector(0.f, 0.f, -HalfHeight - 3.f),
			P + FVector(Radius * 0.8f, 0.f, -HalfHeight + 4.f),
			P + FVector(-Radius * 0.8f, 0.f, -HalfHeight + 4.f),
			P + FVector(Radius + 4.f, 0.f, 0.f),
			P + FVector(-Radius - 4.f, 0.f, 0.f),
			P + FVector(0.f, 0.f, HalfHeight),
		};
		for (const FVector& Point : Points)
		{
			if (Island->IsSolidAt(Point))
			{
				return true;
			}
		}
		return false;
	}

	APenguinGameMode* GetGameMode(const UObject* Ctx)
	{
		const UWorld* World = Ctx ? Ctx->GetWorld() : nullptr;
		return World ? World->GetAuthGameMode<APenguinGameMode>() : nullptr;
	}

	float DeathShowTime(EPenguinKOReason Reason)
	{
		return Reason == EPenguinKOReason::Sea ? PinguKillCam::SeaDeathShowTime : APenguinCharacter::GetDeathShowTime(Reason);
	}
}

// ---------------------------------------------------------------------------- the curve

float PinguKillCam::CurveAt(float From, float Exponent, float Duration, float T)
{
	if (Duration <= KINDA_SMALL_NUMBER)
	{
		return 1.f;
	}
	const float A = FMath::Clamp(T / Duration, 0.f, 1.f);
	return FMath::Clamp(From + (1.f - From) * FMath::Pow(A, Exponent), 0.f, 1.f);
}

float PinguKillCam::GameTimeOf(float From, float Exponent, float Duration)
{
	return Duration * (From + (1.f - From) / (Exponent + 1.f));
}

void PinguKillCam::PlanCurve(float GameSeconds, float& OutFrom, float& OutExponent, float& OutDuration)
{
	const float G = FMath::Max(GameSeconds, 0.05f);
	float From = StartDilation;
	float Duration = FMath::Clamp(G * RealPerGame, RealMin, RealMax);
	const float Average = G / Duration;
	float Exponent = Average > From ? (1.f - From) / (Average - From) - 1.f : 4.f;
	if (Exponent > 4.f)
	{
		// Very little game time to show: the steepest ease-in, a shorter slow motion.
		Exponent = 4.f;
		Duration = G / (From + (1.f - From) / (Exponent + 1.f));
	}
	else if (Exponent < 1.f)
	{
		// A lot of game time: an even ramp from near-frozen, a little longer.
		Exponent = 1.f;
		Duration = G / (From + (1.f - From) / (Exponent + 1.f));
	}
	OutFrom = From;
	OutExponent = Exponent;
	OutDuration = Duration;
}

float PinguKillCam::PlanRest(float From, float Exponent, float GameSeconds)
{
	const float Per = From + (1.f - From) / (FMath::Max(Exponent, 0.1f) + 1.f);
	return FMath::Max(GameSeconds, 0.f) / FMath::Max(Per, 0.01f);
}

// ---------------------------------------------------------------------------- predictions

PinguKillCam::FFlight PinguKillCam::PredictFlight(const APenguinCharacter* Victim)
{
	using namespace PinguKillCamPrivate;
	FFlight Out;
	const UPenguinMovementComponent* Move = Victim ? Victim->GetPenguinMovement() : nullptr;
	const UWorld* World = Victim ? Victim->GetWorld() : nullptr;
	const ABlockIsland* Island = World ? ABlockIsland::FindNear(World, Victim->GetActorLocation()) : nullptr;
	if (!Move || !Island || !Victim->IsAlive() || Victim->IsHidden())
	{
		Out.bUnknown = true;
		Out.Saved = TEXT("none");
		return Out;
	}
	// Only a plain flight or fall: on the ground, frozen, hanging in a tornado (or sucked in), flapping, floating in a shell,
	// stuck to a wall, stomping, passing through the island (Second Chance's throw), dashing: not predicted.
	if (!Move->IsFalling() || Move->IsFrozen() || Move->IsHovering() || Move->IsTornadoPulled() || Move->IsFlapping() || Move->IsFloating()
		|| Move->IsWallClinging() || Move->IsStomping() || Move->IsPassingThroughIsland() || Move->IsDashing() || Victim->IsShielded())
	{
		Out.bUnknown = true;
		Out.Saved = TEXT("state");
		return Out;
	}
	// Second Chance not used yet this round: the fish throws it back.
	if (const UPenguinTraitComponent* Traits = Victim->GetTraits())
	{
		if (Traits->HasTrait(EPinguTrait::SecondChance) && Traits->GetEvents().SecondChanceAt < 0.f)
		{
			Out.Saved = TEXT("trait");
			return Out;
		}
	}
	const FBox Box = Island->GetBounds();
	if (!Box.IsValid)
	{
		Out.bUnknown = true;
		Out.Saved = TEXT("island");
		return Out;
	}

	// The numbers in effect (tuning, trait, size).
	const float WorldG = FMath::Abs(World->GetGravityZ()) * Move->GravityScale;
	const float FallMult = Move->GetEffectiveFallGravityMultiplier();
	const FVector Vel = Move->Velocity;
	// Gravity the movement uses right now against what the tuning says for this state: a trait's or hazard's gravity scale.
	float GravityRatio = 1.f;
	if (FMath::Abs(Vel.Z) > Move->ApexSpeedThreshold + 30.f && !Move->IsFastFalling() && WorldG > 1.f)
	{
		const float Expected = WorldG * (Vel.Z < 0.f ? FallMult : 1.f);
		GravityRatio = FMath::Clamp(FMath::Abs(static_cast<const UMovementComponent*>(Move)->GetGravityZ()) / FMath::Max(Expected, 1.f), 0.25f, 3.f);
	}
	const float G = WorldG * GravityRatio;
	if (G < 50.f)
	{
		Out.bUnknown = true;
		Out.Saved = TEXT("no gravity");
		return Out;
	}
	const float JumpScale = Move->GetTraitJumpScale() * FMath::Sqrt(FMath::Max(Move->GetSpeedScale(), 0.1f));
	const float AirJumpSpeed = FMath::Sqrt(2.f * WorldG * Move->DoubleJumpHeight) * JumpScale;
	const float AirSpeed = Move->GetEffectiveRunSpeed() * Move->GetSpeedScale();
	const float AirAccel = Move->AirAcceleration * Move->GetAirControlScale();
	const float MaxFall = Move->GetEffectiveMaxFallSpeed();
	const float DashSpeed = Move->GetEffectiveDashSpeed() * Move->GetSpeedScale();
	const float DashExit = Move->GetEffectiveDashExitSpeed() * Move->GetSpeedScale();
	const float FlapSpeed = PinguTuning::FlapPadSpeed * PinguTuning::FlapSpeedShare * JumpScale;
	const float FlapHeight = PinguTuning::FlapPadHeight * PinguTuning::FlapFallHeightShare * JumpScale * JumpScale;
	const float Stun = Move->GetStunRemaining();
	const UCapsuleComponent* Capsule = Victim->GetCapsuleComponent();
	const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 31.f;
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 60.f;
	const float SeaZ = Island->SeaLevel + SeaKOAbove;

	// What its abilities could still do (from when their cooldowns end).
	TArray<FSaver> Savers;
	if (const UPenguinAbilityComponent* Abilities = Victim->GetAbilities())
	{
		const TArray<EPenguinAbility>& Slots = Abilities->GetAbilities();
		for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
		{
			FSaver S = SaverOf(Slots[Slot]);
			if (S.Kind == ESaverKind::None)
			{
				continue;
			}
			S.AvailableAt = Abilities->IsReady(Slot) ? 0.f : Abilities->GetCooldownRemaining(Slot);
			if (S.AvailableAt < SimHorizon)
			{
				Savers.Add(S);
			}
		}
	}

	// Where it could stand: the top of every column (Snow Warp lands it over one).
	TArray<FVector2D> Tops;
	for (float X = static_cast<float>(Box.Min.X) + Island->CellSize * 0.5f; X < Box.Max.X; X += Island->CellSize)
	{
		float Top = 0.f;
		if (Island->FindGroundBelow(FVector(X, 0.f, Box.Max.Z + 50.f), static_cast<float>(Box.Max.Z - Box.Min.Z) + 200.f, Top))
		{
			Tops.Add(FVector2D(X, Top + HalfHeight));
		}
	}
	const auto DistanceToTop = [&Tops](const FVector& P)
	{
		float Best = TNumericLimits<float>::Max();
		for (const FVector2D& Top : Tops)
		{
			// Anywhere over the column is fine (it falls onto it).
			const float Dz = FMath::Max(0.f, static_cast<float>(Top.Y - P.Z));
			Best = FMath::Min(Best, FMath::Sqrt(FMath::Square(static_cast<float>(Top.X - P.X)) + Dz * Dz));
		}
		return Best;
	};
	FPinguBuildLimit Limit;
	const bool bLimit = PinguBuildLimit::Get(Island, Limit);
	const float ShellTime = PinguTuning::ShellDuration;
	FCollisionQueryParams SweepParams(SCENE_QUERY_STAT(PinguKillCamFlight), false, Victim);

	// Six ways back: the air jumps as soon as it falls or as late as possible (below the island's top), each steering toward the
	// island (its middle while over it), or hard left, or hard right (a pit's walls, a ledge beside it).
	bool bAllFail = true;
	float LatestSplash = 0.f;
	FVector LatestAt = Victim->GetActorLocation();
	for (int32 Policy = 0; Policy < 6 && bAllFail; ++Policy)
	{
		const bool bLate = (Policy & 1) == 1;
		const int32 Steer = Policy / 2; // 0 toward the island, 1 left, 2 right
		FVector P = Victim->GetActorLocation();
		FVector V(Vel.X, 0.f, Vel.Z);
		float StunLeft = Stun;
		int32 Jumps = Move->GetAirJumpsLeft();
		int32 Dashes = Move->GetAirDashesLeft();
		float DashCooldown = Move->GetDashCooldownRemaining();
		float DashLeft = 0.f;
		FVector DashDir = FVector::ZeroVector;
		float FlapClimbLeft = 0.f;
		bool bFlapUsed = false;
		float FloatLeft = 0.f;
		bool bShellUsed = false;
		int32 SweepCounter = 0;
		FVector SweepFrom = P;
		TArray<bool> SaverUsed;
		SaverUsed.Init(false, Savers.Num());
		bool bSaved = false;
		bool bSplash = false;
		float T = 0.f;
		for (; T < SimHorizon; T += SimStep)
		{
			const bool bControl = StunLeft <= 0.f;
			StunLeft -= SimStep;
			DashCooldown -= SimStep;
			// The abilities that came off cooldown (usable in a hit's stun too, Flap only after it).
			bool bFlapNow = false;
			const TCHAR* SavedBy = nullptr;
			for (int32 Index = 0; Index < Savers.Num() && !SavedBy; ++Index)
			{
				const FSaver& S = Savers[Index];
				if (T < S.AvailableAt)
				{
					continue;
				}
				switch (S.Kind)
				{
				case ESaverKind::Hook:
				{
					// Any block in reach, and it still hangs above the sea when the hook gets there.
					const float D = DistanceToBox(P, Box);
					const float Need = AbilityWindup + D / PinguTuning::HookSpeed;
					const float Drop = FMath::Max(0.f, static_cast<float>(-V.Z)) * Need + 0.5f * G * FallMult * Need * Need;
					if (D <= PinguTuning::HookReach && P.Z - Drop > SeaZ)
					{
						SavedBy = TEXT("hook");
					}
					break;
				}
				case ESaverKind::Warp:
					if (DistanceToTop(P) <= PinguTuning::WarpRange + 60.f)
					{
						SavedBy = TEXT("warp");
					}
					break;
				case ESaverKind::Wall:
					if (bLimit ? (P.X >= Limit.MinX - 60.f && P.X <= Limit.MaxX + 60.f && P.Z < Limit.TopZ) : DistanceToBox(P, Box) <= 450.f)
					{
						SavedBy = TEXT("ice wall");
					}
					break;
				case ESaverKind::Peg:
					if (DistanceToBox(P, Box) <= PegReach)
					{
						SavedBy = TEXT("icicle peg");
					}
					break;
				case ESaverKind::Shell:
					if (!bShellUsed && V.Z < 0.f)
					{
						bShellUsed = true;
						FloatLeft = ShellTime;
					}
					break;
				case ESaverKind::Moves:
					if (!SaverUsed[Index])
					{
						SaverUsed[Index] = true;
						Jumps += S.ExtraJumps;
						Dashes += S.ExtraDashes;
					}
					break;
				case ESaverKind::Flap:
					bFlapNow = !bFlapUsed;
					break;
				default:
					break;
				}
			}
			if (SavedBy)
			{
				bSaved = true;
				Out.Saved = SavedBy;
				break;
			}
			FloatLeft -= SimStep;
			// Where it steers: toward the island (its middle while above it).
			const float GoalX = (P.X < Box.Min.X || P.X > Box.Max.X) ? FMath::Clamp(static_cast<float>(P.X), static_cast<float>(Box.Min.X), static_cast<float>(Box.Max.X))
				: static_cast<float>(Box.GetCenter().X);
			const float Dir = Steer == 1 ? -1.f : (Steer == 2 ? 1.f : (FMath::Abs(GoalX - P.X) < 1.f ? 0.f : FMath::Sign(GoalX - static_cast<float>(P.X))));
			const bool bBelowTop = P.Z < Box.Max.Z + 50.f;
			bool bNoGravity = false;
			if (DashLeft > 0.f)
			{
				DashLeft -= SimStep;
				V = DashDir * (DashLeft > 0.f ? DashSpeed : DashExit);
				bNoGravity = DashLeft > 0.f;
			}
			else if (FlapClimbLeft > 0.f)
			{
				V.Z = FlapSpeed;
				FlapClimbLeft -= FlapSpeed * SimStep;
				bNoGravity = true;
			}
			else if (bControl && V.Z < 0.f)
			{
				// Falling: the next way back up.
				if (Jumps > 0 && (!bLate || bBelowTop))
				{
					--Jumps;
					V.Z = AirJumpSpeed;
					V.X = FMath::Lerp(static_cast<float>(V.X), Dir * AirSpeed, Move->DoubleJumpRedirect);
				}
				else if (bFlapNow && (Jumps == 0 || bBelowTop))
				{
					bFlapUsed = true;
					FlapClimbLeft = FlapHeight;
					V.Z = FlapSpeed;
				}
				else if (Dashes > 0 && DashCooldown <= 0.f && Jumps == 0)
				{
					--Dashes;
					DashLeft = Move->DashDuration;
					DashDir = FVector(Dir == 0.f ? 0.f : Dir * 0.7071f, 0.f, Dir == 0.f ? 1.f : 0.7071f);
					V = DashDir * DashSpeed;
					bNoGravity = true;
				}
			}
			if (DashLeft <= 0.f)
			{
				// Sideways: the stick (over its running speed it only wears off), or no control: the knockback's drag.
				float Vx = static_cast<float>(V.X);
				if (!bControl)
				{
					Vx = FMath::FInterpConstantTo(Vx, 0.f, SimStep, Move->KnockbackAirDrag);
				}
				else if (FMath::Abs(Vx) > AirSpeed && FMath::Sign(Vx) == Dir)
				{
					Vx = FMath::FInterpConstantTo(Vx, Dir * AirSpeed, SimStep, Move->AirOverspeedDeceleration);
				}
				else
				{
					Vx = FMath::FInterpConstantTo(Vx, Dir * AirSpeed, SimStep, AirAccel);
				}
				V.X = Vx;
			}
			if (!bNoGravity)
			{
				const bool bFloat = FloatLeft > 0.f && V.Z < 0.f;
				const float Scale = bFloat ? Move->FloatGravityScale : (V.Z < 0.f ? FallMult : (bControl && FMath::Abs(V.Z) < Move->ApexSpeedThreshold ? Move->ApexGravityMultiplier : 1.f));
				V.Z = FMath::Max(static_cast<float>(V.Z) - G * Scale * SimStep, bFloat ? -Move->FloatMaxFallSpeed : -MaxFall);
			}
			P += V * SimStep;
			// Anything else it could land on or catch (another penguin, a snowman, a giant snowball, falling ice, an ice cube):
			// a sweep of the world every other step.
			const bool bSweepNow = (++SweepCounter & 1) == 0;
			if (bSweepNow && World->SweepTestByChannel(SweepFrom, P, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius, HalfHeight), SweepParams))
			{
				bSaved = true;
				Out.Saved = TEXT("something to land on");
				break;
			}
			if (bSweepNow)
			{
				SweepFrom = P;
			}
			if (TouchesIsland(Island, P, Radius, HalfHeight))
			{
				bSaved = true;
				Out.Saved = TEXT("land");
				break;
			}
			if (P.Z < SeaZ)
			{
				bSplash = true;
				break;
			}
		}
		if (bSaved || !bSplash)
		{
			if (!bSaved)
			{
				Out.Saved = TEXT("time");
			}
			bAllFail = false;
			break;
		}
		if (T >= LatestSplash)
		{
			LatestSplash = T;
			LatestAt = P;
		}
	}
	Out.bDoomed = bAllFail;
	if (bAllFail)
	{
		// When it splashes if it does nothing at all (the earliest): the slow motion is timed from this one, so it never
		// starts after the knock-out; trying to get back only makes the splash later (the curve is planned again then).
		FVector P = Victim->GetActorLocation();
		FVector V(Vel.X, 0.f, Vel.Z);
		float T = 0.f;
		for (; T < SimHorizon && P.Z >= SeaZ; T += SimStep)
		{
			const float Rate = T < Stun ? Move->KnockbackAirDrag : Move->AirBraking;
			V.X = FMath::FInterpConstantTo(static_cast<float>(V.X), 0.f, SimStep, Rate);
			V.Z = FMath::Max(static_cast<float>(V.Z) - G * (V.Z < 0.f ? FallMult : 1.f) * SimStep, -MaxFall);
			P += V * SimStep;
		}
		Out.EarliestSplashIn = FMath::Min(T, LatestSplash);
		Out.SplashIn = LatestSplash;
		Out.SplashAt = LatestAt;
		Out.Saved = TEXT("");
	}
	return Out;
}

float PinguKillCam::PredictDeadlyHit(const APenguinCharacter* Victim, APenguinCharacter*& OutBy)
{
	OutBy = nullptr;
	const UPenguinMovementComponent* Move = Victim ? Victim->GetPenguinMovement() : nullptr;
	const UWorld* World = Victim ? Victim->GetWorld() : nullptr;
	if (!Move || !World || !Victim->IsAlive() || Victim->IsHidden() || Victim->IsShielded() || Move->IsFrozen())
	{
		return -1.f; // the shell blocks it, frozen ice takes it
	}
	const UPenguinTraitComponent* Traits = Victim->GetTraits();
	const bool bMirror = Traits && Traits->HasTrait(EPinguTrait::Mirror);
	const bool bControl = !Move->HasNoControl();
	const float Window = bControl ? ShotWindow : LongShotWindow;
	// With control it could still dodge: a jump up (if it has one), a step aside. A hit is certain only if it lands even then.
	const bool bCanJump = bControl && (Move->IsMovingOnGround() || Move->GetAirJumpsLeft() > 0);
	const float JumpSpeed = Move->IsMovingOnGround() ? Move->GetJumpVelocity() : FMath::Sqrt(2.f * FMath::Abs(World->GetGravityZ()) * Move->GravityScale * Move->DoubleJumpHeight);
	const float DodgeG = FMath::Abs(World->GetGravityZ()) * Move->GravityScale;
	const auto DodgeUp = [&](float T) { return bCanJump ? FMath::Max(0.f, JumpSpeed * T - 0.5f * DodgeG * T * T) : 0.f; };
	const auto DodgeSide = [&](float T) { return bControl ? 0.5f * Move->GroundAcceleration * T * T : 0.f; };
	// A shell it could raise at once blocks the shot.
	if (bControl && Move->CanUseAbilities())
	{
		if (const UPenguinAbilityComponent* Abilities = Victim->GetAbilities())
		{
			if (Abilities->IsReady(Abilities->FindSlot(EPenguinAbility::IceShell)))
			{
				return -1.f;
			}
		}
	}
	const UCapsuleComponent* Capsule = Victim->GetCapsuleComponent();
	const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 31.f;
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 60.f;
	const FVector Loc = Victim->GetActorLocation();
	const FVector Vel = Move->IsFalling() ? Move->Velocity : FVector(Move->Velocity.X, 0.f, 0.f);
	const float VictimG = Move->IsFalling() ? FMath::Abs(static_cast<const UMovementComponent*>(Move)->GetGravityZ()) : 0.f;
	const ABlockIsland* Island = ABlockIsland::FindNear(World, Loc);
	const auto VictimAt = [&](float T)
	{
		return Loc + Vel * T - FVector(0.f, 0.f, 0.5f * VictimG * T * T);
	};

	float Best = -1.f;
	TArray<APenguinProjectile*> Shots;
	APenguinProjectile::GetAll(World, Shots);
	for (APenguinProjectile* Shot : Shots)
	{
		if (!IsValid(Shot) || !Shot->IsThreat() || bMirror)
		{
			continue;
		}
		const EPenguinProjectileKind Kind = Shot->GetKind();
		if (Kind == EPenguinProjectileKind::Icicle && Shot->GetShooter() != Victim)
		{
			const FVector P0 = Shot->GetActorLocation();
			const FVector V = Shot->GetVelocity();
			const float ShotG = Shot->GetGravity();
			const float Reach = Shot->GetRadius();
			constexpr int32 Steps = 48;
			for (int32 Step = 1; Step <= Steps; ++Step)
			{
				const float T = Window * Step / Steps;
				const FVector P = P0 + V * T - FVector(0.f, 0.f, 0.5f * ShotG * T * T);
				const FVector D = P - VictimAt(T);
				if (FMath::Abs(D.X) <= Radius + Reach - DodgeSide(T) && FMath::Abs(D.Z) <= HalfHeight + Reach - DodgeUp(T))
				{
					if ((!Island || Island->IsLineClear(P0, P)) && (Best < 0.f || T < Best))
					{
						Best = T;
						OutBy = Shot->GetShooter();
					}
					break;
				}
			}
		}
		else if (Kind == EPenguinProjectileKind::Bomb && Shot->HasAuthority())
		{
			const float Fuse = Shot->GetLifeLeft();
			if (Fuse <= 0.f || Fuse > Window)
			{
				continue;
			}
			const FVector At = Shot->GetActorLocation() + Shot->GetVelocity() * Fuse;
			// (It may still step or jump out of it.)
			if (FVector::Dist(At, VictimAt(Fuse)) <= Shot->GetBlastKillRadius() - FMath::Max(DodgeSide(Fuse), DodgeUp(Fuse)) - 20.f && (Best < 0.f || Fuse < Best))
			{
				Best = Fuse;
				OutBy = Shot->GetShooter();
			}
		}
	}
	// A belly stomp coming down on its head.
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Other = *It;
		const UPenguinMovementComponent* OtherMove = Other && Other != Victim && Other->IsAlive() ? Other->GetPenguinMovement() : nullptr;
		if (!OtherMove || !OtherMove->IsStomping() || OtherMove->IsStompWindingUp() || OtherMove->Velocity.Z > -1000.f)
		{
			continue;
		}
		const UCapsuleComponent* OtherCapsule = Other->GetCapsuleComponent();
		const float OtherRadius = OtherCapsule ? OtherCapsule->GetScaledCapsuleRadius() : 31.f;
		const float OtherHalf = OtherCapsule ? OtherCapsule->GetScaledCapsuleHalfHeight() : 60.f;
		const FVector D = Other->GetActorLocation() - Loc;
		const float Gap = static_cast<float>(D.Z) - OtherHalf - HalfHeight;
		const float T = Gap / FMath::Max(static_cast<float>(-OtherMove->Velocity.Z), 1.f);
		if (FMath::Abs(D.X) < Radius + OtherRadius - DodgeSide(T) && Gap > -20.f && T <= Window && (Best < 0.f || T < Best))
		{
			Best = FMath::Max(T, 0.f);
			OutBy = Other;
		}
	}
	return Best;
}

double PinguKillCam::PacedSeconds(const UWorld* World)
{
	const UPinguKillCamWorld* Sub = World ? World->GetSubsystem<UPinguKillCamWorld>() : nullptr;
	return FPlatformTime::Seconds() - (Sub ? Sub->LostSeconds : 0.0);
}

// ---------------------------------------------------------------------------- world subsystem

bool UPinguKillCamWorld::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UPinguKillCamWorld::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (InWorld.GetNetMode() == NM_Client)
	{
		return; // the server's comes by replication
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	KillCam = InWorld.SpawnActor<APinguKillCam>(APinguKillCam::StaticClass(), FTransform::Identity, Params);
}

// ---------------------------------------------------------------------------- the actor

APinguKillCam::APinguKillCam()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics; // after the movement: a hit's velocity is in
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(10.f);
	SetReplicatingMovement(false);
	SetHidden(true);
}

void APinguKillCam::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APinguKillCam, Plan);
}

APinguKillCam* APinguKillCam::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	UPinguKillCamWorld* Sub = World ? World->GetSubsystem<UPinguKillCamWorld>() : nullptr;
	if (!Sub)
	{
		return nullptr;
	}
	if (!Sub->KillCam.IsValid())
	{
		for (TActorIterator<APinguKillCam> It(World); It; ++It)
		{
			Sub->KillCam = *It;
			break;
		}
	}
	return Sub->KillCam.Get();
}

void APinguKillCam::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	if (UPinguKillCamWorld* Sub = World ? World->GetSubsystem<UPinguKillCamWorld>() : nullptr)
	{
		Sub->KillCam = this;
	}
	if (World && World->GetNetMode() == NM_Client)
	{
		PostDispatchHandle = World->OnPostTickDispatch().AddUObject(this, &APinguKillCam::OnPostTickDispatch);
	}
}

void APinguKillCam::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (PostDispatchHandle.IsValid())
		{
			World->OnPostTickDispatch().Remove(PostDispatchHandle);
			PostDispatchHandle.Reset();
		}
		if (CurrentDilation < 1.f)
		{
			CurrentDilation = 1.f;
			APinguFX::SetKillCamDilation(this, 1.f);
		}
		if (APenguinCamera* Camera = APenguinCamera::FindOrSpawn(World))
		{
			Camera->SetKillCamView(FBox(ForceInit), 0.f);
		}
	}
	Super::EndPlay(EndPlayReason);
}

double APinguKillCam::Clock() const
{
	// Real time that stops while the game is paused (solo), not slowed by the time dilation.
	const UWorld* World = GetWorld();
	return World ? World->GetAudioTimeSeconds() : 0.0;
}

float APinguKillCam::GetRemaining() const
{
	return bSegmentRunning ? FMath::Max(0.f, Plan.Duration - static_cast<float>(Clock() - SegmentStart)) : 0.f;
}

bool APinguKillCam::IsFinal(const APenguinCharacter* Penguin) const
{
	if (!Penguin)
	{
		return false;
	}
	if (TestFinalVictim.IsValid())
	{
		return TestFinalVictim.Get() == Penguin;
	}
	const APenguinGameMode* GameMode = PinguKillCamPrivate::GetGameMode(this);
	return GameMode && GameMode->IsFinalVictim(Penguin);
}

void APinguKillCam::StartSegment(EPinguKillCamStage Stage, float From, float Exponent, float Duration, APenguinCharacter* Victim, APenguinCharacter* Hitter)
{
	++Plan.Serial;
	Plan.Stage = Stage;
	Plan.From = From;
	Plan.Exponent = Exponent;
	Plan.Duration = Duration;
	Plan.Victim = Victim;
	Plan.Hitter = Hitter;
	BeginLocalSegment(0.0);
	ForceNetUpdate();
}

void APinguKillCam::BeginLocalSegment(double ClockOffset)
{
	SegmentStart = Clock() - ClockOffset;
	bSegmentRunning = Plan.Stage != EPinguKillCamStage::Off && Plan.Duration > 0.f;
	if (Plan.Stage == EPinguKillCamStage::Approach || Plan.Stage == EPinguKillCamStage::Showcase)
	{
		if (FirstTriggerClock < 0.0)
		{
			FirstTriggerClock = SegmentStart;
		}
		if (ZoomVictim.Get() != Plan.Victim || (Plan.Stage == EPinguKillCamStage::Approach && LocalStage != EPinguKillCamStage::Approach)
			|| (Plan.Stage == EPinguKillCamStage::Showcase && LocalStage == EPinguKillCamStage::Off))
		{
			// A new kill: nothing of the last one's place or knock-out carries over (the first frame snaps to this victim).
			bVictimSpotKnown = false;
			bKillShown = false;
			bSnapPending = true;
		}
		if (Plan.Victim && !bVictimSpotKnown)
		{
			// (Where it is now - also when already hidden, a splash: the actor stays where it went out.)
			LastVictimSpot = Plan.Victim->GetActorLocation();
			bVictimSpotKnown = true;
		}
		ZoomVictim = Plan.Victim;
		ZoomHitter = Plan.Hitter;
		ZoomHoldLeft = 0.f;
	}
	if (Plan.Stage == EPinguKillCamStage::Showcase)
	{
		++Stats.Showcases;
		KOGameTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	}
	if (!bSegmentRunning)
	{
		FirstTriggerClock = -1.0;
	}
	UpdateCurve();
	ApplyDilation();
}

void APinguKillCam::OnRep_Plan()
{
	if (!HasActorBegunPlay())
	{
		// The actor's first replication (before BeginPlay): a plan from before we joined is not played here (the host's
		// replicated time dilation covers it). (The first plan after joining comes later: a plan equal to the defaults is
		// never sent, so this is not "the first OnRep".)
		LocalSerialSeen = Plan.Serial;
		return;
	}
	if (Plan.Serial == LocalSerialSeen)
	{
		return; // (the server only ended its segment: ours ends by itself)
	}
	LocalSerialSeen = Plan.Serial;
	if (Plan.Stage == EPinguKillCamStage::Off)
	{
		bSegmentRunning = false;
		LocalStage = EPinguKillCamStage::Off;
		ZoomHoldLeft = 0.f;
		UpdateCurve();
		ApplyDilation();
		return;
	}
	// The plan left the host a moment ago (half the ping): start that far into it.
	double Offset = 0.0;
	if (const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (const APlayerState* State = PC->PlayerState)
		{
			Offset = FMath::Clamp(State->GetPingInMilliseconds() * 0.0005, 0.0, 0.1);
		}
	}
	UE_LOG(LogPingu, Log, TEXT("[KILLCAM] client: %s from %.2fx, power %.2f, %.2f s (%.0f ms in)"), *UEnum::GetValueAsString(Plan.Stage), Plan.From, Plan.Exponent,
		Plan.Duration, Offset * 1000.0);
	BeginLocalSegment(Offset);
}

void APinguKillCam::UpdateCurve()
{
	const EPinguKillCamStage Was = LocalStage;
	if (bSegmentRunning)
	{
		LocalStage = Plan.Stage;
		const float T = static_cast<float>(Clock() - SegmentStart);
		if (T >= Plan.Duration)
		{
			bSegmentRunning = false;
			CurrentDilation = 1.f;
			if (Plan.Stage == EPinguKillCamStage::Showcase)
			{
				const UWorld* World = GetWorld();
				Stats.KOToNormalGame = World && KOGameTime >= 0.0 ? static_cast<float>(World->GetTimeSeconds() - KOGameTime) : -1.f;
				Stats.TotalReal = FirstTriggerClock >= 0.0 ? static_cast<float>(Clock() - FirstTriggerClock) : -1.f;
				Stats.AtEnd = CurrentDilation;
				ZoomHoldLeft = PinguKillCamPrivate::ZoomHoldAfter;
				LocalStage = EPinguKillCamStage::Off;
				FirstTriggerClock = -1.0;
				UE_LOG(LogPingu, Display, TEXT("[KILLCAM] normal speed again: %.2f s of game time after the knock-out (death animation %.2f), %.2f s real in all"),
					Stats.KOToNormalGame, Stats.ShowTime, Stats.TotalReal);
				if (HasAuthority())
				{
					Plan.Stage = EPinguKillCamStage::Off;
				}
			}
			else if (Plan.Stage == EPinguKillCamStage::Release)
			{
				LocalStage = EPinguKillCamStage::Off;
				FirstTriggerClock = -1.0;
				if (HasAuthority())
				{
					Plan.Stage = EPinguKillCamStage::Off;
				}
			}
			// (Approach: the curve is at 1 before the knock-out came - it waits there for it, or for PinguKillCam::LateSlack.)
		}
		else
		{
			CurrentDilation = PinguKillCam::CurveAt(Plan.From, Plan.Exponent, Plan.Duration, T);
			Stats.Lowest = FMath::Min(Stats.Lowest, CurrentDilation);
		}
	}
	else
	{
		CurrentDilation = 1.f;
		if (Was == EPinguKillCamStage::Release || Was == EPinguKillCamStage::Showcase)
		{
			LocalStage = EPinguKillCamStage::Off;
		}
	}
}

void APinguKillCam::ApplyDilation()
{
	APinguFX::SetKillCamDilation(this, CurrentDilation);
}

void APinguKillCam::OnPostTickDispatch()
{
	// A LAN client: the engine's replication of this frame may have brought the host's own (older) time dilation. Ours is
	// the same curve on our own clock: set it again before the frame's time step is taken.
	if (!bSegmentRunning && CurrentDilation >= 1.f)
	{
		return;
	}
	UpdateCurve();
	if (AWorldSettings* Settings = GetWorld() ? GetWorld()->GetWorldSettings() : nullptr)
	{
		if (!FMath::IsNearlyEqual(Settings->TimeDilation, CurrentDilation))
		{
			Settings->SetTimeDilation(CurrentDilation);
		}
	}
	ApplyDilation();
}

float APinguKillCam::OnFinalKnockOut(APenguinCharacter* Victim)
{
	using namespace PinguKillCamPrivate;
	if (!HasAuthority() || !Victim)
	{
		return 0.f;
	}
	if (Plan.Stage == EPinguKillCamStage::Showcase && bSegmentRunning)
	{
		return GetRemaining(); // (already playing: the round's second knock-out at the same moment)
	}
	const float ShowTime = DeathShowTime(Victim->GetKOReason());
	APenguinCharacter* Hitter = Victim->GetKOBy();
	if (!Hitter && Plan.Stage == EPinguKillCamStage::Approach && Plan.Victim == Victim)
	{
		Hitter = Plan.Hitter;
	}
	float From = 0.f;
	float Exponent = 0.f;
	float Duration = 0.f;
	const bool bPredicted = Plan.Stage == EPinguKillCamStage::Approach && Plan.Victim == Victim;
	if (Plan.Stage == EPinguKillCamStage::Approach || Plan.Stage == EPinguKillCamStage::Release)
	{
		// Planned again from where the curve is: exactly the death animation to go.
		From = FMath::Min(CurrentDilation, 0.9f);
		Exponent = Plan.Stage == EPinguKillCamStage::Approach ? Plan.Exponent : 2.f;
		Duration = PinguKillCam::PlanRest(From, Exponent, ShowTime);
	}
	else
	{
		++Stats.AtKnockOut;
		PinguKillCam::PlanCurve(ShowTime, From, Exponent, Duration);
	}
	Stats.ShowTime = ShowTime;
	KOWorldTime = GetWorld()->GetTimeSeconds();
	UE_LOG(LogPingu, Display, TEXT("[KILLCAM] the final knock-out: %s (%s%s), %s; from %.2fx, power %.2f, %.2f s real for its %.2f s death animation"),
		*Victim->GetDisplayName(), *UEnum::GetValueAsString(Victim->GetKOReason()), Hitter ? *FString::Printf(TEXT(" by %s"), *Hitter->GetDisplayName()) : TEXT(""),
		bPredicted ? TEXT("predicted") : TEXT("at the knock-out"), From, Exponent, Duration, ShowTime);
	StartSegment(EPinguKillCamStage::Showcase, From, Exponent, Duration, Victim, Hitter);
	return Duration;
}

void APinguKillCam::Release(const TCHAR* Why)
{
	const float From = FMath::Min(CurrentDilation, 0.99f);
	++Stats.FalseTriggers;
	UE_LOG(LogPingu, Display, TEXT("[KILLCAM] false trigger: %s (%s) - back to normal speed from %.2fx"),
		Plan.Victim ? *Plan.Victim->GetDisplayName() : TEXT("?"), Why, From);
	StartSegment(EPinguKillCamStage::Release, From, 1.f, PinguKillCam::ReleaseTime, Plan.Victim, Plan.Hitter);
}

void APinguKillCam::TickServer(float GameDt)
{
	using namespace PinguKillCamPrivate;
	UWorld* World = GetWorld();
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	APenguinGameMode* GameMode = GetGameMode(this);
	if (!World || !GameState)
	{
		return;
	}
	const EPinguPhase Phase = GameState->GetPhase();
	const bool bTest = TestFinalVictim.IsValid();
	const bool bFight = Phase == EPinguPhase::Fight || bTest;
	if (Plan.Stage != EPinguKillCamStage::Off && !bFight && Phase != EPinguPhase::RoundOver && Phase != EPinguPhase::MatchOver)
	{
		// A new round, the lobby, the menu: no slow motion left over.
		StartSegment(EPinguKillCamStage::Off, 1.f, 1.f, 0.f, nullptr, nullptr);
		return;
	}

	if (bTest)
	{
		// Practice has no round to end: the test's victim's knock-out is the final one here.
		APenguinCharacter* TestVictim = TestFinalVictim.Get();
		if (TestVictim->IsAlive())
		{
			bTestKOHandled = false;
		}
		else if (!bTestKOHandled)
		{
			bTestKOHandled = true;
			OnFinalKnockOut(TestVictim);
			return;
		}
	}

	if (Plan.Stage == EPinguKillCamStage::Approach)
	{
		APenguinCharacter* Victim = Plan.Victim;
		if (!IsValid(Victim))
		{
			Release(TEXT("the victim is gone"));
			return;
		}
		if (Victim->IsKnockedOut())
		{
			OnFinalKnockOut(Victim); // (the game mode calls it too: whichever comes first)
			return;
		}
		if (!IsFinal(Victim))
		{
			Release(TEXT("its knock-out would not end the round any more"));
			return;
		}
		const float Since = static_cast<float>(World->GetTimeSeconds() - TriggerWorldTime);
		if (Since > PredictedLatestIn + PinguKillCam::LateSlack)
		{
			Release(*FString::Printf(TEXT("no knock-out %.2f s after the predicted %.2f"), Since, PredictedGameIn));
			return;
		}
		// Still doomed? (A way back the prediction did not know of.)
		bool bStill = true;
		const TCHAR* Why = TEXT("");
		if (bShotTrigger)
		{
			APenguinCharacter* By = nullptr;
			bStill = PinguKillCam::PredictDeadlyHit(Victim, By) >= 0.f;
			Why = TEXT("the shot will miss");
			if (!bStill)
			{
				// (The shot may have knocked it into a flight that still ends in the sea.)
				const PinguKillCam::FFlight Flight = PinguKillCam::PredictFlight(Victim);
				bStill = Flight.bDoomed;
			}
		}
		else
		{
			const PinguKillCam::FFlight Flight = PinguKillCam::PredictFlight(Victim);
			bStill = Flight.bDoomed || Flight.bUnknown;
			Why = Flight.Saved;
			if (Flight.bUnknown && Victim->GetPenguinMovement() && Victim->GetPenguinMovement()->IsMovingOnGround())
			{
				bStill = false;
				Why = TEXT("landed");
			}
		}
		SavedFrames = bStill ? 0 : SavedFrames + 1;
		if (SavedFrames >= PinguKillCam::SavedFramesToRelease)
		{
			Release(*FString::Printf(TEXT("saved: %s"), Why));
		}
		return;
	}
	if (Plan.Stage != EPinguKillCamStage::Off || !bFight || (GameMode && GameMode->IsRoundEnding() && !bTest))
	{
		return;
	}

	// Off, in a fight: a knock-out coming that ends the round?
	TArray<APenguinCharacter*> Candidates;
	if (bTest)
	{
		Candidates.Add(TestFinalVictim.Get());
	}
	else if (GameMode)
	{
		for (APenguinCharacter* Penguin : GameMode->GetPenguins())
		{
			if (IsValid(Penguin) && Penguin->IsAlive() && IsFinal(Penguin))
			{
				Candidates.Add(Penguin);
			}
		}
	}
	for (APenguinCharacter* Victim : Candidates)
	{
		if (!IsValid(Victim) || !Victim->IsAlive())
		{
			continue;
		}
		APenguinCharacter* By = nullptr;
		const float HitIn = PinguKillCam::PredictDeadlyHit(Victim, By);
		float GameIn = -1.f;
		float LatestGameIn = -1.f;
		float Show = PinguKillCam::SeaDeathShowTime;
		bool bShot = false;
		FString What;
		if (HitIn >= 0.f)
		{
			GameIn = HitIn;
			bShot = true;
			Show = APenguinCharacter::GetDeathShowTime(EPenguinKOReason::Icicle);
			What = FString::Printf(TEXT("a deadly hit in %.2f s"), HitIn);
		}
		else
		{
			const PinguKillCam::FFlight Flight = PinguKillCam::PredictFlight(Victim);
			if (bTest)
			{
				UE_LOG(LogPingu, Log, TEXT("[KILLCAM] test victim at x %.0f z %.0f: doomed %d (splash in %.2f) saved by '%s'%s"), Victim->GetActorLocation().X,
					Victim->GetActorLocation().Z, Flight.bDoomed ? 1 : 0, Flight.SplashIn, Flight.Saved, Flight.bUnknown ? TEXT(", not predictable") : TEXT(""));
			}
			// The point of no return: the first frame nothing it has can get it back (logged: how long before the splash).
			if (Flight.bDoomed && DoomedVictim.Get() != Victim)
			{
				DoomedVictim = Victim;
				DoomedSplashIn = Flight.EarliestSplashIn;
			}
			else if (!Flight.bDoomed && DoomedVictim.Get() == Victim)
			{
				DoomedVictim.Reset();
			}
			// (Not while it could still drag the fall out a lot longer by trying everything: the curve would end long before the splash.)
			if (Flight.bDoomed && Flight.EarliestSplashIn <= PinguKillCam::MaxLead && Flight.SplashIn <= PinguKillCam::MaxLead + PinguKillCam::MaxStretch)
			{
				GameIn = Flight.EarliestSplashIn;
				LatestGameIn = Flight.SplashIn;
				By = Victim->GetLastAttacker(HitterMemory);
				Stats.NoReturnLead = DoomedSplashIn;
				Stats.TriggerLead = Flight.EarliestSplashIn;
				What = FString::Printf(TEXT("can't get back (no return %.2f s before the splash): the sea in %.2f s (%.2f trying everything) at x %.0f"),
					DoomedSplashIn, Flight.EarliestSplashIn, Flight.SplashIn, Flight.SplashAt.X);
			}
		}
		if (GameIn < 0.f)
		{
			continue;
		}
		float From = 0.f;
		float Exponent = 0.f;
		float Duration = 0.f;
		PinguKillCam::PlanCurve(GameIn + Show, From, Exponent, Duration);
		TriggerWorldTime = World->GetTimeSeconds();
		PredictedGameIn = GameIn;
		PredictedLatestIn = FMath::Max(GameIn, LatestGameIn);
		SavedFrames = 0;
		bShotTrigger = bShot;
		++Stats.Predicted;
		Stats.PredictedShot += bShot ? 1 : 0;
		UE_LOG(LogPingu, Display, TEXT("[KILLCAM] predicted: %s - %s%s; from %.2fx, power %.2f, %.2f s real"), *Victim->GetDisplayName(), *What,
			By ? *FString::Printf(TEXT(" (by %s)"), *By->GetDisplayName()) : TEXT(""), From, Exponent, Duration);
		StartSegment(EPinguKillCamStage::Approach, From, Exponent, Duration, Victim, By);
		return;
	}
}

void APinguKillCam::UpdateCamera(float RealDt)
{
	using namespace PinguKillCamPrivate;
	UWorld* World = GetWorld();
	APenguinCamera* Camera = World ? APenguinCamera::FindOrSpawn(World) : nullptr;
	if (!Camera)
	{
		return;
	}
	if (CameraWithPrerequisite.Get() != Camera)
	{
		// The camera frames after us in the same frame: the first slow motion frame already shows the victim.
		Camera->AddTickPrerequisiteActor(this);
		CameraWithPrerequisite = Camera;
	}
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	const EPinguPhase Phase = GameState ? GameState->GetPhase() : EPinguPhase::Menu;
	const bool bBannerTime = Phase == EPinguPhase::RoundOver || Phase == EPinguPhase::MatchOver;
	const bool bTest = TestFinalVictim.IsValid();
	ZoomHoldLeft = FMath::Max(0.f, ZoomHoldLeft - RealDt);
	if (!bBannerTime && Phase != EPinguPhase::Fight && !bTest)
	{
		// A new round, the lobby, the menu: the normal framing again.
		ZoomHoldLeft = 0.f;
		bKillShown = false;
	}
	const bool bRunning = LocalStage == EPinguKillCamStage::Approach || LocalStage == EPinguKillCamStage::Showcase;
	bKillShown |= LocalStage == EPinguKillCamStage::Showcase;
	// Zoomed in while it runs (and a moment after); under the winner banner a wider view of the kill and the winner.
	const bool bWant = bRunning || ZoomHoldLeft > 0.f || (bBannerTime && bKillShown);
	bool bSnap = false;
	if (bWant && bRunning && (ZoomBlend <= 0.f || bSnapPending))
	{
		ZoomBlend = 1.f; // the first frame of a new kill: at once (the camera snaps, then zooms in gently)
		bSnap = true;
	}
	ZoomBlend = FMath::FInterpConstantTo(ZoomBlend, bWant ? 1.f : 0.f, RealDt, 1.f / ZoomOutTime);

	// The victim's place (where it was last seen once it is gone: a splash, a death look).
	// (Once the knock-out came, a victim standing again is a respawn - practice, the lobby: not the kill any more.)
	const APenguinCharacter* ZoomTarget = ZoomVictim.Get();
	const bool bAfterKO = LocalStage == EPinguKillCamStage::Showcase || bKillShown;
	const APenguinCharacter* Victim = ZoomTarget && !(bAfterKO && ZoomTarget->IsAlive()) ? ZoomTarget : nullptr;
	if (Victim && !Victim->IsHidden())
	{
		LastVictimSpot = Victim->GetActorLocation();
		bVictimSpotKnown = true;
	}
	if (ZoomBlend <= 0.f || !bVictimSpotKnown)
	{
		Camera->SetKillCamView(FBox(ForceInit), 0.f);
		return;
	}
	FBox Frame(ForceInit);
	FVector Spot = LastVictimSpot;
	const ABlockIsland* Island = ABlockIsland::FindNear(World, Spot);
	if (Island)
	{
		Spot.Z = FMath::Max(Spot.Z, static_cast<double>(Island->SeaLevel) + 60.0); // (the splash, not under the water)
	}
	Frame += Spot;
	if (bBannerTime)
	{
		// The banner: the kill's spot and the winner (or the island's edge nearest the spot) under it, room above for the banner.
		const APenguinCharacter* Winner = nullptr;
		const int32 WinnerSlot = GameState ? GameState->GetRoundWinnerSlot() : -1;
		for (TActorIterator<APenguinCharacter> It(World); It && WinnerSlot >= 0; ++It)
		{
			if (It->GetPlayerSlot() == WinnerSlot && !It->IsHidden())
			{
				Winner = *It;
				break;
			}
		}
		if (!Winner && ZoomHitter.IsValid() && ZoomHitter->IsAlive() && !ZoomHitter->IsHidden())
		{
			Winner = ZoomHitter.Get();
		}
		if (Winner)
		{
			Frame += Winner->GetActorLocation();
		}
		else if (Island)
		{
			const FBox Box = Island->GetBounds();
			if (Box.IsValid)
			{
				Frame += FVector(FMath::Clamp(Spot.X, Box.Min.X, Box.Max.X), 0.0, Box.Max.Z);
			}
		}
		Frame += FVector(Frame.GetCenter().X, 0.0, Frame.Max.Z + BannerRoom);
		Frame = Frame.ExpandBy(FVector(BannerPadding, 0.0, 0.0));
		Camera->SetKillCamView(Frame, ZoomBlend);
		return;
	}
	if (Victim && Victim->IsAlive() && !Victim->IsHidden())
	{
		// A little ahead of a fast flight (the camera's smoothing trails it).
		FVector Ahead = Spot + Victim->GetVelocity() * 0.2;
		Ahead.Z = FMath::Max(Ahead.Z, Spot.Z - 600.0);
		Frame += Ahead;
	}
	const APenguinCharacter* Hitter = ZoomHitter.Get();
	if (Hitter && Hitter != Victim && Hitter->IsAlive() && !Hitter->IsHidden())
	{
		const FVector H = Hitter->GetActorLocation();
		// (Once the victim is gone - hidden at the end of its death look - the hitter keeps the picture from being empty.)
		const float Fit = Victim && !Victim->IsHidden() ? 1.f : 2.f;
		if (FMath::Abs(H.X - Spot.X) < HitterFitsX * Fit && FMath::Abs(H.Z - Spot.Z) < HitterFitsZ * Fit)
		{
			Frame += H;
		}
	}
	Camera->SetKillCamView(Frame, ZoomBlend, bSnap);
	bSnapPending &= !bSnap;
}

void APinguKillCam::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float RealDt = FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.25f);
	// The paced clock: the real time this frame lost to the slow motion.
	if (UPinguKillCamWorld* Sub = World->GetSubsystem<UPinguKillCamWorld>())
	{
		const AWorldSettings* Settings = World->GetWorldSettings();
		const float Applied = Settings ? Settings->GetEffectiveTimeDilation() : 1.f;
		Sub->LostSeconds += static_cast<double>(RealDt) * FMath::Clamp(1.f - Applied, 0.f, 1.f);
	}
	if (HasAuthority())
	{
		TickServer(DeltaSeconds);
	}
	UpdateCurve();
	ApplyDilation();
	UpdateCamera(RealDt);
}

void APinguKillCam::SetTestFinalVictim(APenguinCharacter* Penguin)
{
	TestFinalVictim = Penguin;
	bTestKOHandled = false;
}
