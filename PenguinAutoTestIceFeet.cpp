// Pingu - ice feet (the ice gun's frozen feet; card 2026-11-05_ice-feet-slide). The autotest's steps
// (APenguinAutoTest::AddIceFeetSteps; filter icefeet_ + spawn_target, look_IceFeetSlide for the picture):
//
//  icefeet_NoWalk    the stick held for a second does nothing: not a step of walking.
//  icefeet_Slide     at running speed with the stick pushed the other way: it slides on at nearly the same speed (friction 40 cm/s^2).
//  icefeet_Edge      slides on over the island's end and falls (no stopping at the edge).
//  icefeet_Jump      a jump works, it keeps its speed in the air and slides on after landing.
//  icefeet_Knockback a hit's knockback works as normal (flies, ends stunned).
//  icefeet_Ends      after its time the stick works again (walking from rest).
//  icefeet_Bot       a bot (the dummy with the brain on) sliding at an edge with ground beyond it: jumps, does not steer.
//  icefeet_HostJudge the host's move judge (PinguHostAuthority.h): a long slide at run speed, and one from a dash's speed, never refused.
//  look_IceFeetSlide 98_icefeet_slide_*: a sliding penguin from the side.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinBot.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PinguHostAuthority.h"

namespace PenguinAutoTestIceFeetPrivate
{
	FString Ok(bool bOk) { return bOk ? TEXT("ok") : TEXT("FAIL"); }

	struct FIceRun
	{
		bool bStarted = false;
		bool bJumped = false;
		bool bLanded = false;
		bool bAirborne = false;
		bool bFell = false;
		float StartX = 0.f;
		float MinV = 1.e9f;
		float MaxAbsV = 0.f;
		float AirVMin = 1.e9f;
		float AirVMax = -1.e9f;
		float MaxZ = -1.e9f;
		float EndV = 0.f;
		float EndedAt = -1.f;
		int32 JumpsAtStart = 0;
		int32 Shots = 0;
	};

	FIceRun& State()
	{
		static FIceRun Data;
		return Data;
	}
}

void APenguinAutoTest::AddIceFeetSteps()
{
	using namespace PenguinAutoTestIceFeetPrivate;

	// The same start for the slide steps: at X, grounded after 0.3 s, then pushed to a speed.
	const auto Prepare = [this](float X, float SlipTime)
	{
		State() = FIceRun();
		PrepareBrawl(-1800.f, {});
		Teleport(FVector(X, 0.f, 62.f));
		Penguin->SetSlippery(SlipTime); // (the character: ice chunks on the feet too)
	};

	AddStep(TEXT("icefeet_NoWalk"), 1.4f,
		[this, Prepare] { Prepare(FlatIslandX - 300.f, 6.f); },
		[this](float T)
		{
			FIceRun& R = State();
			if (T > 0.3f)
			{
				if (!R.bStarted)
				{
					R.bStarted = true;
					R.StartX = static_cast<float>(Location().X);
				}
				VirtualInput = FVector2D(1.f, 0.f); // walking right, the whole time
				R.MaxAbsV = FMath::Max(R.MaxAbsV, FMath::Abs(static_cast<float>(Move->Velocity.X)));
			}
		},
		[this]
		{
			const FIceRun& R = State();
			VirtualInput = FVector2D::ZeroVector;
			const float Moved = static_cast<float>(Location().X) - R.StartX;
			Report(FString::Printf(TEXT("RESULT icefeet walk: the stick held right for 1.1 s on ice feet: moved %.1f cm, top speed %.1f cm/s (want under 5 cm, 5 cm/s), slippery %d %s"),
				Moved, R.MaxAbsV, Move->IsSlippery() ? 1 : 0, *Ok(FMath::Abs(Moved) < 5.f && R.MaxAbsV < 5.f && Move->IsSlippery())));
		});

	AddStep(TEXT("icefeet_Slide"), 2.6f,
		[this, Prepare] { Prepare(FlatIslandX - 1500.f, 6.f); },
		[this](float T)
		{
			FIceRun& R = State();
			if (T > 0.3f && !R.bStarted)
			{
				R.bStarted = true;
				R.StartX = static_cast<float>(Location().X);
				Move->Velocity.X = 500.f;
			}
			if (R.bStarted)
			{
				VirtualInput = FVector2D(-1.f, 0.f); // trying to stop / turn back
				R.MinV = FMath::Min(R.MinV, static_cast<float>(Move->Velocity.X));
				R.EndV = static_cast<float>(Move->Velocity.X);
			}
		},
		[this]
		{
			const FIceRun& R = State();
			VirtualInput = FVector2D::ZeroVector;
			const float Way = static_cast<float>(Location().X) - R.StartX;
			// 2.3 s at 500 minus a little: about 1100 cm; losing at most 40 x 2.3 = 92 cm/s.
			const bool bOk = R.MinV > 380.f && Way > 1000.f && Move->IsMovingOnGround();
			Report(FString::Printf(TEXT("RESULT icefeet slide: pushed to 500 cm/s, the stick pulled the other way for 2.3 s: slowest %.0f cm/s, at the end %.0f, slid %.0f cm (want over 380 and over 1000) %s"),
				R.MinV, R.EndV, Way, *Ok(bOk)));
		});

	AddStep(TEXT("icefeet_Edge"), 3.2f,
		[this, Prepare] { Prepare(FlatIslandX + 2000.f, 6.f); },
		[this](float T)
		{
			FIceRun& R = State();
			if (T > 0.3f && !R.bStarted)
			{
				R.bStarted = true;
				Move->Velocity.X = 500.f;
			}
			if (R.bStarted)
			{
				VirtualInput = FVector2D(-1.f, 0.f); // the stick against it: no help
				R.bFell |= Location().Z < -150.f;
			}
		},
		[this]
		{
			const FIceRun& R = State();
			VirtualInput = FVector2D::ZeroVector;
			Report(FString::Printf(TEXT("RESULT icefeet edge: from 500 cm/s towards the island's end, the stick pulled back: fell off %d (z %.0f; want 1: a slide does not stop at the edge) %s"),
				R.bFell ? 1 : 0, Location().Z, *Ok(R.bFell)));
		});

	AddStep(TEXT("icefeet_Jump"), 2.8f,
		[this, Prepare] { Prepare(FlatIslandX - 1500.f, 6.f); },
		[this](float T)
		{
			FIceRun& R = State();
			if (T > 0.3f && !R.bStarted)
			{
				R.bStarted = true;
				R.StartX = static_cast<float>(Location().X);
				Move->Velocity.X = 500.f;
				R.JumpsAtStart = Move->JumpEvents;
			}
			if (!R.bStarted)
			{
				return;
			}
			VirtualInput = FVector2D(-1.f, 0.f);
			if (!R.bJumped && T > 0.5f)
			{
				R.bJumped = true;
				Move->JumpPressed();
			}
			if (R.bJumped && T > 0.8f)
			{
				Move->JumpReleased();
			}
			const float V = static_cast<float>(Move->Velocity.X);
			if (!Move->IsMovingOnGround())
			{
				R.bAirborne = true;
				R.AirVMin = FMath::Min(R.AirVMin, V);
				R.AirVMax = FMath::Max(R.AirVMax, V);
				R.MaxZ = FMath::Max(R.MaxZ, static_cast<float>(Location().Z));
			}
			else if (R.bAirborne)
			{
				R.bLanded = true;
			}
			R.EndV = V;
		},
		[this]
		{
			const FIceRun& R = State();
			VirtualInput = FVector2D::ZeroVector;
			Move->JumpReleased();
			const bool bOk = Move->JumpEvents > R.JumpsAtStart && R.MaxZ > 62.f + 120.f && R.AirVMin > 420.f && R.bLanded && R.EndV > 300.f;
			Report(FString::Printf(TEXT("RESULT icefeet jump: jumps %d, rose to %.0f (from 62; want over 182), speed in the air %.0f .. %.0f cm/s (want over 420: kept), landed %d, still sliding at %.0f cm/s after it (want over 300) %s"),
				Move->JumpEvents - R.JumpsAtStart, R.MaxZ, R.AirVMin, R.AirVMax, R.bLanded ? 1 : 0, R.EndV, *Ok(bOk)));
		});

	AddStep(TEXT("icefeet_Knockback"), 1.8f,
		[this, Prepare] { Prepare(FlatIslandX - 1500.f, 6.f); },
		[this](float T)
		{
			FIceRun& R = State();
			if (T > 0.3f && !R.bStarted)
			{
				R.bStarted = true;
				R.StartX = static_cast<float>(Location().X);
				Move->ApplyKnockback(FVector2D(900.f, 800.f), 0.3f);
			}
			if (R.bStarted)
			{
				R.MaxAbsV = FMath::Max(R.MaxAbsV, static_cast<float>(Move->Velocity.X));
				R.MaxZ = FMath::Max(R.MaxZ, static_cast<float>(Location().Z));
			}
		},
		[this]
		{
			const FIceRun& R = State();
			const float Way = static_cast<float>(Location().X) - R.StartX;
			const bool bOk = R.MaxAbsV > 700.f && R.MaxZ > 62.f + 60.f && Way > 400.f;
			Report(FString::Printf(TEXT("RESULT icefeet knockback: a hit of 900 cm/s: top speed %.0f, rose to %.0f, went %.0f cm (want over 700, over 122, over 400) %s"),
				R.MaxAbsV, R.MaxZ, Way, *Ok(bOk)));
		});

	AddStep(TEXT("icefeet_Ends"), 3.6f,
		[this, Prepare] { Prepare(FlatIslandX - 1500.f, 1.3f); },
		[this](float T)
		{
			FIceRun& R = State();
			VirtualInput = FVector2D(1.f, 0.f); // walking right all the time
			if (T > 0.2f && Move->IsSlippery())
			{
				R.MaxAbsV = FMath::Max(R.MaxAbsV, FMath::Abs(static_cast<float>(Move->Velocity.X))); // slippery: it stands still
			}
			if (R.EndedAt < 0.f && !Move->IsSlippery() && T > 0.5f)
			{
				R.EndedAt = T;
			}
			R.EndV = static_cast<float>(Move->Velocity.X);
		},
		[this]
		{
			const FIceRun& R = State();
			VirtualInput = FVector2D::ZeroVector;
			const bool bOk = R.MaxAbsV < 5.f && R.EndedAt > 1.2f && R.EndedAt < 1.5f && R.EndV > Move->RunSpeed * 0.9f;
			Report(FString::Printf(TEXT("RESULT icefeet ends: 1.3 s of ice feet, the stick held right: top speed while slippery %.1f (want under 5), it ended at %.2f s (want 1.2 - 1.5), then it walks again: %.0f cm/s at the end (want about %.0f) %s"),
				R.MaxAbsV, R.EndedAt, R.EndV, Move->RunSpeed, *Ok(bOk)));
		});

	AddStep(TEXT("icefeet_Bot"), 3.6f,
		[this]
		{
			State() = FIceRun();
			PrepareBrawl(-1800.f, {});
			Teleport(FVector(FlatIslandX + 1200.f, 0.f, 62.f)); // the player on the far side of the gap (the brain goes for it: the same way)
			// A gap in the strip: 3 cells wide, ground on the far side (what a slide could be hopped over).
			if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f)))
			{
				for (const float Along : { -350.f, -250.f, -150.f })
				{
					for (int32 Row = 0; Row < 12; ++Row)
					{
						Flat->BreakBlockAt(FVector(FlatIslandX + Along, 0.f, 50.f - 100.f * Row), false);
					}
				}
				Flat->RebuildAfterEdits();
			}
			if (Target.IsValid() && Target->GetBot())
			{
				ResetTarget(FVector(FlatIslandX - 1400.f, 0.f, 62.f));
				Target->GetAbilities()->SetAbilities(TArray<EPenguinAbility>()); // (no shot, no recoil that would push it off its slide)
				Target->GetPenguinMovement()->SetSlippery(6.f);
				State().JumpsAtStart = Target->GetPenguinMovement()->JumpEvents;
				Target->GetBot()->SetBotEnabled(true, 1);
			}
		},
		[this](float T)
		{
			FIceRun& R = State();
			UPenguinMovementComponent* BotMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (!BotMove)
			{
				return;
			}
			if (T > 0.3f && !R.bStarted)
			{
				R.bStarted = true;
				BotMove->Velocity.X = 500.f;
			}
			if (R.bStarted && T < 0.8f)
			{
				R.MinV = FMath::Min(R.MinV, static_cast<float>(BotMove->Velocity.X)); // the brain pulling it back: nothing
			}
			R.bFell |= Target->GetActorLocation().Z < -150.f;
		},
		[this]
		{
			const FIceRun& R = State();
			const UPenguinMovementComponent* BotMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			const float BotX = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) : 0.f;
			if (Target.IsValid() && Target->GetBot())
			{
				Target->GetBot()->SetBotEnabled(false);
			}
			const int32 Jumps = BotMove ? BotMove->JumpEvents - R.JumpsAtStart : -1;
			const bool bOk = BotMove && Jumps >= 1 && R.MinV > 420.f && !R.bFell && BotX > FlatIslandX - 150.f;
			Report(FString::Printf(TEXT("RESULT icefeet bot: a bot sliding at 500 cm/s towards a 3-cell gap: jumps %d (want 1 or more), slowest speed in its first 0.5 s %.0f (want over 420: it did not walk against it), fell %d (want 0), ended at %.0f beyond the gap (want over %.0f) %s"),
				Jumps, R.MinV, R.bFell ? 1 : 0, BotX - FlatIslandX, -150.f, *Ok(bOk)));
		});

	AddStep(TEXT("icefeet_HostJudge"), 0.1f,
		[this]
		{
			// A LAN joiner's slide as the host's judge sees it, moves 60 a second: (1) a slide at the full running speed for
			// 6 s; (2) a slide from a dash's speed that wears off to the running speed first (the movement's overspeed rule),
			// then glides: nothing refused; (3) the same slide at 6x the speed it could have: refused (the judge still works).
			const UPenguinMovementComponent* M = Move.Get();
			const FPinguMoveLimits Limits = M ? M->GetHostMoveLimits() : FPinguMoveLimits();
			const float RunV = M ? M->GetEffectiveRunSpeed() * M->GetSpeedScale() : 560.f;
			const float DashV = M ? M->GetEffectiveDashSpeed() : 1600.f;
			const float Over = M ? M->GroundOverspeedDeceleration : 2000.f;
			const float Friction = M ? M->SlipperyGroundFriction : 40.f;
			const auto Slide = [&](float StartV, float Scale)
			{
				FPinguMoveJudge Judge;
				double Now = 800.0;
				FVector At = FVector::ZeroVector;
				FPinguMoveSample Sample;
				Sample.Location = At;
				Sample.Now = Now;
				Sample.bSupported = true;
				FString Why;
				Judge.Judge(Sample, Limits, Why);
				float V = StartV;
				int32 Refused = 0;
				for (int32 Step = 0; Step < 360; ++Step)
				{
					V = V > RunV ? FMath::Max(RunV, V - Over / 60.f) : FMath::Max(0.f, V - Friction / 60.f);
					Now += 1.0 / 60.0;
					At.X += V * Scale / 60.f;
					Sample.Now = Now;
					Sample.Location = At;
					Refused += Judge.Judge(Sample, Limits, Why) ? 0 : 1;
				}
				return Refused;
			};
			const int32 AtRun = Slide(RunV, 1.f);
			const int32 FromDash = Slide(DashV, 1.f);
			const int32 Faked = Slide(RunV, 6.f);
			Report(FString::Printf(TEXT("RESULT icefeet host judge: a 6 s slide at running speed refused %d times, one from dash speed %d (want 0 each); the same slide at 6x speed %d (want 1 or more) %s"),
				AtRun, FromDash, Faked, *Ok(AtRun == 0 && FromDash == 0 && Faked > 0)));
		},
		[](float) {}, [] {});

	// look_IceFeetSlide: the stage camera beside the sliding penguin (three pictures while it glides).
	AddStep(TEXT("look_IceFeetSlide"), 2.2f,
		[this, Prepare]
		{
			Prepare(FlatIslandX - 1500.f, 6.f);
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this](float T)
		{
			FIceRun& R = State();
			if (T > 0.3f && !R.bStarted)
			{
				R.bStarted = true;
				Move->Velocity.X = 450.f;
			}
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Penguin->GetActorLocation() + FVector(0.f, 0.f, 15.f) + Move->Velocity * 0.04f;
				const FVector From = LookAt + FVector(-60.f, 640.f, 90.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			static const float At[] = { 0.7f, 1.2f, 1.7f };
			if (R.Shots < UE_ARRAY_COUNT(At) && T >= At[R.Shots])
			{
				Shot(*FString::Printf(TEXT("98_icefeet_slide_%d"), R.Shots));
				++R.Shots;
			}
		},
		[this] { VirtualInput = FVector2D::ZeroVector; });
}
