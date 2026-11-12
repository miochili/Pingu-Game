// Pingu - the host's say over a LAN joiner's own penguin (PinguHostAuthority.h, card 2026-10-30_host-authority). The autotest's
// steps (APenguinAutoTest::AddHostAuthoritySteps; filter hostauth_; pure logic, no LAN: the host's simulation of a joiner's
// knockback, the hand-back and the pull-back are checked by the LAN steps netcheatmove / netcheatcombat and nettest):
//
//  hostauth_Limits      the limits the host judges our own penguin by: a jump, the air moves, the fastest run / dash / slide.
//  hostauth_LegalMoves  made-up honest moves at 60 a second, with network jitter: walking, a jump + double jump + air dash, a walk
//                       off a ledge and a long fall, a jump pad, a knockback flight the host gave, ground dashes, a Rubber bounce,
//                       a low-gravity launch, Sticky on a wall: nothing refused.
//  hostauth_Cheats      the same judge on cheats: hanging in the air (gravity 0) refused within its jumps' worth of air time,
//                       a jump far higher than its jump, a steady climb into the sky, a run at 6x its speed, a push's speed kept
//                       for good; the host's slide cooldown (a new slide every sync: about one a cooldown counts) and landings
//                       (only after the host saw it in the air, at most as fast as the fall).

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PinguHostAuthority.h"
#include "Math/RandomStream.h"

namespace PenguinAutoTestHostAuthorityPrivate
{
	FString Ok(bool bOk) { return bOk ? TEXT("ok") : TEXT("FAIL"); }

	/** A made-up joiner: its own physics (as the movement: rising gravity, apex hang, heavier fall), and the host's judge. */
	struct FFakeOwner
	{
		FPinguMoveJudge Judge;
		FPinguMoveLimits Limits;
		double Now = 1000.0;
		/** The host's clock of the last move that arrived (moves arrive late and in bunches: never earlier than the last). */
		double Arrived = 1000.0;
		FVector Location = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		int32 Moves = 0;
		int32 Refused = 0;
		double FirstRefused = -1.0;
		FString FirstWhy;
		FRandomStream* Jitter = nullptr;

		bool Send(bool bSupported, bool bPad = false, bool bFree = false)
		{
			const double Late = Jitter ? Jitter->FRandRange(0.f, 0.08f) : 0.0;
			Arrived = FMath::Max(Arrived, Now + Late);
			FPinguMoveSample Sample;
			Sample.Now = Arrived;
			Sample.Location = Location;
			Sample.bSupported = bSupported;
			Sample.bNearPad = bPad;
			Sample.bFreeVertical = bFree;
			FString Why;
			++Moves;
			const bool bTaken = Judge.Judge(Sample, Limits, Why);
			if (!bTaken)
			{
				++Refused;
				if (FirstRefused < 0.0)
				{
					FirstRefused = Now;
					FirstWhy = Why;
				}
			}
			return bTaken;
		}

		/** One frame of a fall / jump (no ground below until z 0). */
		void Fly(float Dt, bool bHeld = true, float FallGravityScale = 1.6f, float MaxFall = 2200.f)
		{
			float G = Limits.Gravity;
			if (Velocity.Z < 0.f)
			{
				G *= FallGravityScale;
			}
			else if (bHeld && FMath::Abs(Velocity.Z) < Limits.ApexSpeed)
			{
				G *= Limits.ApexGravityScale;
			}
			Velocity.Z = FMath::Max(static_cast<float>(Velocity.Z) - G * Dt, -MaxFall);
			Location += Velocity * Dt;
			Now += Dt;
		}

		/** Down to the ground (z 0) and stood there for a moment. */
		void LandAndStand(float Dt)
		{
			while (Location.Z > 0.f)
			{
				Fly(Dt);
				Send(Location.Z <= 0.f);
			}
			Location.Z = 0.f;
			Velocity = FVector::ZeroVector;
			for (int32 Frame = 0; Frame < 10; ++Frame)
			{
				Now += Dt;
				Send(true);
			}
		}
	};
}

void APenguinAutoTest::AddHostAuthoritySteps()
{
	using namespace PenguinAutoTestHostAuthorityPrivate;
	constexpr float Frame = 1.f / 60.f;

	AddStep(TEXT("hostauth_Limits"), 0.1f,
		[this]
		{
			const APenguinCharacter* Own = Penguin.Get();
			const UPenguinMovementComponent* Move = Own ? Own->GetPenguinMovement() : nullptr;
			if (!Move)
			{
				Report(TEXT("RESULT hostauth limits: no penguin of ours FAIL"));
				return;
			}
			const FPinguMoveLimits Limits = Move->GetHostMoveLimits();
			const bool bOk = Limits.CreditHeight >= Move->JumpHeight && Limits.CreditHeight <= 600.f && Limits.Credits == Move->MaxAirJumps + Move->MaxAirDashes + 2
				&& Limits.SideSpeed >= Move->DashSpeed && Limits.SideSpeed <= 3000.f && Limits.Gravity > 3000.f && Limits.SlowestFall > 300.f;
			Report(FString::Printf(TEXT("RESULT hostauth limits: gravity %.0f, one lift %.0f cm (a jump is %.0f), %d lifts in the air, sideways %.0f cm/s (a dash is %.0f), ")
				TEXT("slowest fall %.0f, pad %.0f %s"), Limits.Gravity, Limits.CreditHeight, Move->JumpHeight, Limits.Credits, Limits.SideSpeed, Move->DashSpeed,
				Limits.SlowestFall, Limits.PadSpeed, *Ok(bOk)));
		},
		[](float) {}, [] {});

	AddStep(TEXT("hostauth_LegalMoves"), 0.1f,
		[this]
		{
			const APenguinCharacter* Own = Penguin.Get();
			const UPenguinMovementComponent* Move = Own ? Own->GetPenguinMovement() : nullptr;
			FPinguMoveLimits Limits = Move ? Move->GetHostMoveLimits() : FPinguMoveLimits();
			FRandomStream Rand(2030);
			FFakeOwner Owner;
			Owner.Limits = Limits;
			Owner.Jitter = &Rand;
			const float G = Limits.Gravity;
			const auto JumpSpeed = [G](float Height) { return FMath::Sqrt(2.f * G * Height); };

			// Walking for a second.
			for (int32 Step = 0; Step < 60; ++Step)
			{
				Owner.Location.X += 560.f * Frame;
				Owner.Now += Frame;
				Owner.Send(true);
			}
			// A held jump, the double jump at its top, an air dash straight up at the next top, down again.
			Owner.Velocity = FVector(300.f, 0.f, JumpSpeed(Move ? Move->JumpHeight : 290.f));
			while (Owner.Velocity.Z > 0.f)
			{
				Owner.Fly(Frame);
				Owner.Send(false);
			}
			Owner.Velocity.Z = JumpSpeed(Move ? Move->DoubleJumpHeight : 230.f);
			while (Owner.Velocity.Z > 0.f)
			{
				Owner.Fly(Frame);
				Owner.Send(false);
			}
			for (float Dash = 0.f; Dash < (Move ? Move->DashDuration : 0.14f); Dash += Frame)
			{
				Owner.Location.Z += (Move ? Move->DashSpeed : 1900.f) * Frame;
				Owner.Now += Frame;
				Owner.Send(false);
			}
			Owner.Velocity.Z = Move ? Move->DashExitSpeed : 700.f;
			Owner.LandAndStand(Frame);
			const int32 AfterJumps = Owner.Refused;
			// Off a ledge (it stood at z 0) and a long fall (2 s) into the sea.
			Owner.Velocity = FVector(560.f, 0.f, 0.f);
			for (int32 Step = 0; Step < 120; ++Step)
			{
				Owner.Fly(Frame);
				Owner.Send(false);
			}
			// (In the sea: the host puts it back on the island - a host teleport the judge follows.)
			Owner.Location.Z = 0.f;
			Owner.Velocity = FVector::ZeroVector;
			Owner.Judge.ForgetPath();
			Owner.Judge.FollowHost(Owner.Arrived, Owner.Location, FVector::ZeroVector, true, Limits);
			Owner.LandAndStand(Frame);
			const int32 AfterFall = Owner.Refused;
			// A jump pad: thrown up at its speed, down again.
			Owner.Velocity = FVector(0.f, 0.f, Limits.PadSpeed);
			do
			{
				Owner.Fly(Frame, false);
				Owner.Send(false, Owner.Location.Z < 200.f);
			} while (Owner.Location.Z > 0.f);
			Owner.LandAndStand(Frame);
			const int32 AfterPad = Owner.Refused;
			// The host knocks it away (2500 sideways, 1200 up): the flight, air drag, down again.
			Owner.Velocity = FVector(2500.f, 0.f, 1200.f);
			Owner.Judge.NoteHostPush(Owner.Arrived, Owner.Location, Owner.Velocity, true, true, Limits);
			do
			{
				Owner.Velocity.X = FMath::Max(0.f, static_cast<float>(Owner.Velocity.X) - 380.f * Frame);
				Owner.Fly(Frame, false);
				Owner.Send(false);
			} while (Owner.Location.Z > 0.f);
			Owner.LandAndStand(Frame);
			const int32 AfterKnock = Owner.Refused;
			// Ground dashes, one a cooldown: 0.14 s at dash speed, then its exit speed.
			for (int32 Dash = 0; Dash < 6; ++Dash)
			{
				for (float T = 0.f; T < 0.45f; T += Frame)
				{
					Owner.Location.X += (T < 0.14f ? 1900.f : 700.f) * Frame;
					Owner.Now += Frame;
					Owner.Send(true);
				}
			}
			// A low-gravity launch (a fish hook's pull: the host knows of it) far up, then the fall.
			Owner.Velocity = FVector(1200.f, 0.f, 1700.f);
			for (int32 Step = 0; Step < 50; ++Step)
			{
				Owner.Velocity.Z -= G * 0.12f * Frame;
				Owner.Location += Owner.Velocity * Frame;
				Owner.Now += Frame;
				Owner.Send(false, false, true);
			}
			Owner.LandAndStand(Frame);
			const int32 AfterLaunch = Owner.Refused;

			// Rubber: a fall from 900 cm bounces back to 70 % of it.
			FFakeOwner Rubber;
			Rubber.Limits = Limits;
			Rubber.Limits.BounceShare = 0.7f;
			Rubber.Location.Z = 600.f; // standing on a high ledge, then off it: 900 cm down to the ground at z -300
			Rubber.Send(true);
			while (Rubber.Location.Z > -300.f)
			{
				Rubber.Fly(Frame);
				Rubber.Send(false);
			}
			Rubber.Location.Z = -300.f;
			Rubber.Send(true);
			Rubber.Velocity = FVector(0.f, 0.f, JumpSpeed(0.7f * 900.f));
			do
			{
				Rubber.Fly(Frame, false);
				Rubber.Send(false);
			} while (Rubber.Location.Z > -300.f);

			// Sticky on a wall for 3 s (the host finds the wall: supported), a wall jump up, again.
			FFakeOwner Sticky;
			Sticky.Limits = Limits;
			Sticky.Location.Z = 800.f;
			Sticky.Send(true);
			for (int32 Step = 0; Step < 180; ++Step)
			{
				Sticky.Now += Frame;
				Sticky.Send(true);
			}

			const int32 Total = Owner.Refused + Rubber.Refused + Sticky.Refused;
			Report(FString::Printf(TEXT("RESULT hostauth legal moves: %d moves (walk, jump + double jump + air dash, a 2 s fall, a pad, a knockback flight, ")
				TEXT("ground dashes, a low-gravity launch, a Rubber bounce, Sticky on a wall; arriving up to 80 ms late): %d refused (jumps %d, fall %d, pad %d, ")
				TEXT("knock %d, launch %d, Rubber %d, Sticky %d; want 0)%s %s"), Owner.Moves + Rubber.Moves + Sticky.Moves, Total, AfterJumps, AfterFall - AfterJumps,
				AfterPad - AfterFall, AfterKnock - AfterPad, AfterLaunch - AfterKnock, Rubber.Refused, Sticky.Refused,
				Total > 0 ? *FString::Printf(TEXT(", first: %s"), *(Owner.FirstWhy.IsEmpty() ? Rubber.FirstWhy : Owner.FirstWhy)) : TEXT(""), *Ok(Total == 0)));
		},
		[](float) {}, [] {});

	AddStep(TEXT("hostauth_Cheats"), 0.1f,
		[this]
		{
			const APenguinCharacter* Own = Penguin.Get();
			const UPenguinMovementComponent* Move = Own ? Own->GetPenguinMovement() : nullptr;
			const FPinguMoveLimits Limits = Move ? Move->GetHostMoveLimits() : FPinguMoveLimits();
			const float G = Limits.Gravity;
			const float JumpSpeed = FMath::Sqrt(2.f * G * (Move ? Move->JumpHeight : 290.f));
			const auto Fresh = [&Limits]
			{
				FFakeOwner Owner;
				Owner.Limits = Limits;
				Owner.Send(true);
				return Owner;
			};

			// Gravity 0: a jump, then it hangs where it got to (drifting out over the sea) for 8 s.
			FFakeOwner Hover = Fresh();
			const double HoverStart = Hover.Now;
			Hover.Velocity = FVector(600.f, 0.f, JumpSpeed);
			for (int32 Step = 0; Step < 480; ++Step)
			{
				Hover.Velocity.Z = Hover.Velocity.Z > 0.f ? FMath::Max(0.f, static_cast<float>(Hover.Velocity.Z) - G * Frame) : 0.f;
				Hover.Location += Hover.Velocity * Frame;
				Hover.Now += Frame;
				Hover.Send(false);
			}
			const double HoverCaught = Hover.FirstRefused >= 0.0 ? Hover.FirstRefused - HoverStart : -1.0;
			Report(FString::Printf(TEXT("RESULT hostauth hover: gravity 0 after a jump, 8 s: refused after %.1f s (want within 5 s), %d of %d moves refused (%s) %s"),
				HoverCaught, Hover.Refused, Hover.Moves, *Hover.FirstWhy, *Ok(HoverCaught > 0.0 && HoverCaught <= 5.0 && Hover.Refused > 200)));

			// A jump twice as fast as its own (four times as high).
			FFakeOwner High = Fresh();
			High.Velocity = FVector(0.f, 0.f, JumpSpeed * 2.f);
			while (High.Velocity.Z > 0.f)
			{
				High.Fly(Frame);
				High.Send(false);
			}
			// A steady climb into the sky (500 cm/s for 4 s).
			FFakeOwner Climb = Fresh();
			for (int32 Step = 0; Step < 240; ++Step)
			{
				Climb.Location.Z += 500.f * Frame;
				Climb.Now += Frame;
				Climb.Send(false);
			}
			Report(FString::Printf(TEXT("RESULT hostauth jumps: twice its jump speed refused %d times (%s), a climb at 500 cm/s refused %d times after %.1f s (want both 1 or more) %s"),
				High.Refused, *High.FirstWhy, Climb.Refused, Climb.FirstRefused >= 0.0 ? Climb.FirstRefused - 1000.0 : -1.0, *Ok(High.Refused > 0 && Climb.Refused > 0)));

			// Running at 6x its speed (3360 cm/s) on the ground; a push's 3000 cm/s kept for good.
			FFakeOwner Fast = Fresh();
			for (int32 Step = 0; Step < 120; ++Step)
			{
				Fast.Location.X += 3360.f * Frame;
				Fast.Now += Frame;
				Fast.Send(true);
			}
			FFakeOwner Kept = Fresh();
			Kept.Judge.NoteHostPush(Kept.Now, Kept.Location, FVector(3000.f, 0.f, 0.f), true, false, Limits);
			int32 EarlyRefused = 0;
			for (int32 Step = 0; Step < 360; ++Step)
			{
				Kept.Location.X += 3000.f * Frame;
				Kept.Now += Frame;
				const bool bTaken = Kept.Send(true);
				EarlyRefused += Step < 60 && !bTaken ? 1 : 0;
			}
			Report(FString::Printf(TEXT("RESULT hostauth speed: 6x its run refused %d times (first after %.2f s), a push's speed taken at first (%d refused in its first second, want 0) ")
				TEXT("but not kept for 6 s (%d refused, want 1 or more) %s"), Fast.Refused, Fast.FirstRefused >= 0.0 ? Fast.FirstRefused - 1000.0 : -1.0, EarlyRefused, Kept.Refused,
				*Ok(Fast.Refused > 0 && Fast.FirstRefused - 1000.0 < 0.6 && EarlyRefused == 0 && Kept.Refused > 0)));

			// Belly slides: a "new slide" in every sync (60 a second) for 5 s; the host counts one a cooldown.
			FPinguSlideGate Slides;
			const float Cooldown = Move ? Move->BellySlideCooldown : 1.f;
			for (int32 Step = 0; Step < 300; ++Step)
			{
				Slides.Request(2000.0 + Step * Frame, true, Cooldown);
			}
			const int32 Most = FMath::FloorToInt(5.0 / (Cooldown - FPinguSlideGate::Slack)) + 1;
			FPinguSlideGate Honest;
			int32 HonestTaken = 0;
			for (int32 Slide = 0; Slide < 5; ++Slide)
			{
				HonestTaken += Honest.Request(3000.0 + Slide * (Cooldown + 0.02) + (Slide % 2 ? 0.05 : -0.05), true, Cooldown) ? 1 : 0;
			}
			const bool bStunned = Honest.Request(4000.0, false, Cooldown);
			Report(FString::Printf(TEXT("RESULT hostauth slides: 300 claimed in 5 s, %d counted (want %d at most), 5 honest ones a cooldown apart with lag %d counted (want 5), ")
				TEXT("one while stunned %d (want 0) %s"), Slides.Accepted, Most, HonestTaken, bStunned ? 1 : 0,
				*Ok(Slides.Accepted <= Most && Slides.Accepted >= 4 && HonestTaken == 5 && !bStunned)));

			// Landings: 20 claimed while it stands, then a real 30 cm hop claimed at 5000 cm/s.
			FPinguLandGate Lands;
			int32 Faked = 0;
			float Speed = 0.f;
			for (int32 Claim = 0; Claim < 20; ++Claim)
			{
				Lands.Observe(0.f, true);
				Faked += Lands.Request(0.f, 3000.f, Limits, Move ? Move->FastFallGravityMultiplier : 2.3f, Move ? Move->FastFallStartSpeed : 1100.f, Speed) ? 1 : 0;
			}
			Lands.Observe(30.f, false);
			const bool bHop = Lands.Request(0.f, 5000.f, Limits, Move ? Move->FastFallGravityMultiplier : 2.3f, Move ? Move->FastFallStartSpeed : 1100.f, Speed);
			Report(FString::Printf(TEXT("RESULT hostauth landings: 20 claimed standing, %d counted (want 0); a 30 cm hop claimed at 5000 cm/s counted %d at %.0f cm/s (want 1, under 2100) %s"),
				Faked, bHop ? 1 : 0, Speed, *Ok(Faked == 0 && bHop && Speed < 2100.f && Speed > 1000.f)));
		},
		[](float) {}, [] {});
}
