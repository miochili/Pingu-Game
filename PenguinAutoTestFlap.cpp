// Pingu - Flap, the 19th ability (UPenguinMovementComponent::StartFlap, card 2026-11-04 flap). The autotest's steps
// (APenguinAutoTest::AddFlapSteps; filter flap_ + spawn_target, look_Flap for the pictures):
//
//  flap_Ground        from the ground: climbs the icicle pad's height (100 %), at 40 % of its speed (a slow climb), then falls.
//  flap_Fall          falling: the fall is caught at once (rising the very next frame), climbs 75 % of the pad's height.
//  flap_Cooldown      6 s after a use (8 until 2026-11-05); a second press meanwhile does nothing.
//  flap_Blocked       not while frozen, stunned, sucked in by a tornado, hanging in a tornado's eye (swallowed is that too).
//  flap_Ends          a hit's knockback, a jump and a landing end it.
//  flap_Scale         Featherweight / Heavyweight / Giant / Tiny: the height scales with the square of the jump scale (as a
//                     jump), the speed with it; Sticky lets go of its wall, Rubber's landing after it is its own.
//  flap_HostJudge     the host's move judge (PinguHostAuthority.h): an honest flap of a LAN joiner never refused once the
//                     host started it, the same climb without the host's flap refused.
//  look_Flap          95_flap_ground_*, 95_flap_fall_* (strips while it climbs).
//  look_FlapWind_<Low|Medium|High|Ultra>  96_flapwind_<level>_ground_*, _air_* : the wing beats' wind (APinguFX::FlapWind) per Effects setting.

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinTraitComponent.h"
#include "PinguFX.h"
#include "PinguHostAuthority.h"
#include "PinguSettings.h"
#include "PinguTraitTable.h"
#include "PinguTuning.h"

namespace PenguinAutoTestFlapPrivate
{
	FString Ok(bool bOk) { return bOk ? TEXT("ok") : TEXT("FAIL"); }

	/** What a flap step measured. */
	struct FFlapRun
	{
		bool bPressed = false;
		bool bUsed = false;
		float PressT = -1.f;
		float StartZ = 0.f;
		float PeakZ = -1.e9f;
		float PeakVz = 0.f;
		float VzBefore = 0.f;
		float VzNext = 0.f;
		int32 FramesSincePress = -1;
		bool bLanded = false;
		float ClimbStartT = -1.f;
		float ClimbEndT = -1.f;
		bool bShotHud = false;
	};

	FFlapRun& Run()
	{
		static FFlapRun State;
		return State;
	}

	/** The icicle pad's height for this penguin (the pad's speed and the jump's gravity, as StartFlap works it out). */
	float PadHeight(const UPenguinMovementComponent* Move)
	{
		const float G = FMath::Abs(Move->GetWorld()->GetGravityZ() * Move->GravityScale);
		return FMath::Square(Move->JumpPadSpeed) / (2.f * FMath::Max(G, 1.f));
	}

	void SetTraitNow(APenguinCharacter* Who, EPinguTrait Trait)
	{
		if (UPenguinTraitComponent* Traits = Who ? Who->GetTraits() : nullptr)
		{
			Traits->SetTrait(Trait, -1.f);
		}
	}
}

void APenguinAutoTest::AddFlapSteps()
{
	using namespace PenguinAutoTestFlapPrivate;

	// One measuring step: PressAt s in (after an optional drop from DropHeight cm), the flap's climb followed to its top and down.
	const auto Measure = [this](float T, float PressAt)
	{
		FFlapRun& R = Run();
		UPenguinMovementComponent* M = Move.Get();
		if (!M || !Penguin.IsValid())
		{
			return;
		}
		const float Z = static_cast<float>(Penguin->GetActorLocation().Z);
		if (!R.bPressed && T >= PressAt)
		{
			R.bPressed = true;
			R.PressT = T;
			R.VzBefore = static_cast<float>(M->Velocity.Z);
			R.StartZ = Z;
			R.bUsed = Penguin->GetAbilities()->UseAbility(0, Penguin->GetActorLocation() + FVector(0.f, 0.f, 300.f));
			R.FramesSincePress = 0;
			return;
		}
		if (!R.bPressed)
		{
			return;
		}
		++R.FramesSincePress;
		if (R.FramesSincePress == 1)
		{
			R.VzNext = static_cast<float>(M->Velocity.Z);
		}
		R.PeakZ = FMath::Max(R.PeakZ, Z);
		if (M->IsFlapping())
		{
			R.PeakVz = FMath::Max(R.PeakVz, static_cast<float>(M->Velocity.Z));
			R.ClimbEndT = T;
		}
		if (R.FramesSincePress > 5 && M->IsMovingOnGround())
		{
			R.bLanded = true;
		}
	};

	AddStep(TEXT("flap_Ground"), 2.6f,
		[this] { Run() = FFlapRun(); PrepareBrawl(3000.f, { EPenguinAbility::Flap }); },
		[this, Measure](float T) { Measure(T, 0.25f); },
		[this]
		{
			const FFlapRun& R = Run();
			const UPenguinMovementComponent* M = Move.Get();
			const float Pad = M ? PadHeight(M) : 0.f;
			const float Height = R.PeakZ - R.StartZ;
			const float Speed = R.PeakVz;
			const float PadSpeed = M ? M->JumpPadSpeed : 0.f;
			const bool bOk = R.bUsed && FMath::Abs(Height - Pad) < Pad * 0.05f && FMath::Abs(Speed - PadSpeed * 0.4f) < PadSpeed * 0.02f && R.bLanded;
			Report(FString::Printf(TEXT("RESULT flap ground: climbed %.0f cm (the icicle pad throws %.0f cm at %.0f cm/s: want 100 %%), at %.0f cm/s (want 40 %%: %.0f), ")
				TEXT("climbing %.2f s, down again %d %s"), Height, Pad, PadSpeed, Speed, PadSpeed * 0.4f, R.ClimbEndT - R.PressT, R.bLanded ? 1 : 0, *Ok(bOk)));
		});

	AddStep(TEXT("flap_Fall"), 2.6f,
		[this]
		{
			Run() = FFlapRun();
			PrepareBrawl(3000.f, { EPenguinAbility::Flap });
			Teleport(Location() + FVector(0.f, 0.f, 900.f)); // dropped from 9 m: it falls a moment first
		},
		[this, Measure](float T) { Measure(T, 0.45f); },
		[this]
		{
			const FFlapRun& R = Run();
			const UPenguinMovementComponent* M = Move.Get();
			const float Pad = M ? PadHeight(M) : 0.f;
			const float Height = R.PeakZ - R.StartZ;
			const bool bOk = R.bUsed && R.VzBefore < -500.f && R.VzNext > 0.f && FMath::Abs(Height - Pad * 0.75f) < Pad * 0.05f;
			Report(FString::Printf(TEXT("RESULT flap fall: falling at %.0f cm/s, the next frame %.0f cm/s (want rising: caught at once), climbed %.0f cm ")
				TEXT("(want 75 %% of the pad's %.0f: %.0f) %s"), R.VzBefore, R.VzNext, Height, Pad, Pad * 0.75f, *Ok(bOk)));
		});

	AddStep(TEXT("flap_Cooldown"), 0.6f,
		[this] { Run() = FFlapRun(); PrepareBrawl(3000.f, { EPenguinAbility::Flap }); },
		[this](float T)
		{
			FFlapRun& R = Run();
			if (!R.bPressed && T > 0.1f)
			{
				R.bPressed = true;
				R.bUsed = Penguin->GetAbilities()->UseAbility(0, Location());
				R.PressT = T;
			}
			if (R.bPressed && !R.bLanded && T > 0.3f)
			{
				R.bLanded = true; // (here: the second press done)
				R.VzNext = Penguin->GetAbilities()->UseAbility(0, Location()) ? 1.f : 0.f;
				R.PeakVz = Penguin->GetAbilities()->GetCooldownRemaining(0);
			}
		},
		[this]
		{
			const FFlapRun& R = Run();
			// (6 s since 2026-11-05, card build-limit-snowman; was 8)
			const float Want = UPenguinAbilityComponent::GetInfo(EPenguinAbility::Flap).Cooldown;
			const bool bOk = R.bUsed && R.VzNext < 0.5f && FMath::Abs(R.PeakVz + 0.2f - Want) < 0.15f && FMath::IsNearlyEqual(Want, 6.f);
			Report(FString::Printf(TEXT("RESULT flap cooldown: used %d, a second press 0.2 s later used %d (want 0), cooldown left %.2f s (want %.1f of %.0f, the cooldown 6) %s"),
				R.bUsed ? 1 : 0, R.VzNext > 0.5f ? 1 : 0, R.PeakVz, Want - 0.2f, Want, *Ok(bOk)));
		});

	AddStep(TEXT("flap_Blocked"), 0.4f,
		[this]
		{
			PrepareBrawl(3000.f, { EPenguinAbility::Flap });
			UPenguinMovementComponent* M = Move.Get();
			UPenguinAbilityComponent* A = Penguin->GetAbilities();
			FString Line;
			bool bAll = true;
			const auto Try = [&](const TCHAR* What)
			{
				A->SetAbilities({ EPenguinAbility::Flap }); // (a fresh cooldown)
				const bool bUsed = A->UseAbility(0, Location());
				bAll &= !bUsed && !M->IsFlapping();
				Line += FString::Printf(TEXT("%s %d, "), What, bUsed ? 1 : 0);
				Penguin->ResetForRound(Penguin->GetActorLocation());
			};
			M->SetFrozen(1.f);
			Try(TEXT("frozen"));
			M->ApplyKnockback(FVector2D(0.f, 1.f), 0.6f);
			Try(TEXT("stunned"));
			M->SetTornadoPull(Location() + FVector(300.f, 0.f, 200.f), 1.f, 600.f, 60.f);
			Try(TEXT("pulled"));
			M->SetHover(1.f);
			Try(TEXT("in a tornado's eye / swallowed"));
			A->SetAbilities({ EPenguinAbility::Flap });
			const bool bFree = A->UseAbility(0, Location());
			Report(FString::Printf(TEXT("RESULT flap blocked: %sfree again %d (want 0 for each state, 1 free) %s"), *Line, bFree ? 1 : 0, *Ok(bAll && bFree)));
		},
		[](float) {}, [this] { Penguin->ResetForRound(Penguin->GetActorLocation()); });

	AddStep(TEXT("flap_Ends"), 0.9f,
		[this] { Run() = FFlapRun(); PrepareBrawl(3000.f, { EPenguinAbility::Flap }); },
		[this](float T)
		{
			FFlapRun& R = Run();
			UPenguinMovementComponent* M = Move.Get();
			UPenguinAbilityComponent* A = Penguin->GetAbilities();
			// A flap, a knockback 0.15 s in; a flap, a jump 0.15 s in (the jump takes over).
			if (!R.bPressed && T > 0.05f)
			{
				R.bPressed = true;
				A->UseAbility(0, Location());
			}
			if (R.bPressed && R.PressT < 0.f && T > 0.2f)
			{
				R.PressT = T;
				M->ApplyKnockback(FVector2D(400.f, 300.f), 0.2f);
				R.VzBefore = M->IsFlapping() ? 1.f : 0.f; // (want 0)
			}
			if (R.PressT > 0.f && !R.bUsed && T > 0.5f)
			{
				R.bUsed = true;
				A->SetAbilities({ EPenguinAbility::Flap });
				M->ResetMovementState();
				A->UseAbility(0, Location());
				R.VzNext = M->IsFlapping() ? 1.f : 0.f; // (want 1)
				M->JumpPressed();
			}
			if (R.bUsed && R.ClimbStartT < 0.f && T > 0.56f)
			{
				R.ClimbStartT = T;
				R.PeakVz = M->IsFlapping() ? 1.f : 0.f; // (want 0: the double jump took over)
				M->JumpReleased();
			}
		},
		[this]
		{
			const FFlapRun& R = Run();
			const bool bOk = R.VzBefore < 0.5f && R.VzNext > 0.5f && R.PeakVz < 0.5f;
			Report(FString::Printf(TEXT("RESULT flap ends: flapping after a knockback %d (want 0), a new flap %d (want 1), after a jump %d (want 0) %s"),
				R.VzBefore > 0.5f ? 1 : 0, R.VzNext > 0.5f ? 1 : 0, R.PeakVz > 0.5f ? 1 : 0, *Ok(bOk)));
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});

	AddStep(TEXT("flap_Scale"), 0.3f,
		[this]
		{
			PrepareBrawl(3000.f, { EPenguinAbility::Flap });
			UPenguinMovementComponent* M = Move.Get();
			const float Pad = PadHeight(M);
			FString Line;
			bool bAll = true;
			struct FCase { const TCHAR* Name; EPinguTrait Trait; };
			const FCase Cases[] = { { TEXT("none"), EPinguTrait::None }, { TEXT("Featherweight"), EPinguTrait::Featherweight },
				{ TEXT("Heavyweight"), EPinguTrait::Heavyweight }, { TEXT("Giant"), EPinguTrait::Giant }, { TEXT("Tiny"), EPinguTrait::Tiny },
				{ TEXT("Sticky"), EPinguTrait::Sticky }, { TEXT("Rubber"), EPinguTrait::Rubber } };
			float NoneHeight = 0.f;
			for (const FCase& Case : Cases)
			{
				SetTraitNow(Penguin.Get(), Case.Trait);
				Penguin->ResetForRound(Penguin->GetActorLocation());
				// Size traits take effect through the size: give it its size now.
				M->SetSizeScale(Penguin->GetSizeTarget());
				M->ResetMovementState();
				const float JumpRatio = M->GetEffectiveJumpHeight() / FMath::Max(M->JumpHeight, 1.f);
				M->StartFlap();
				const float Height = M->GetLastFlapHeight();
				const float Want = Pad * JumpRatio * (M->WasLastFlapFromGround() ? 1.f : PinguTuning::FlapFallHeightShare);
				bAll &= FMath::Abs(Height - Want) < Want * 0.02f + 1.f;
				if (Case.Trait == EPinguTrait::None)
				{
					NoneHeight = Height;
				}
				Line += FString::Printf(TEXT("%s %.0f (want %.0f), "), Case.Name, Height, Want);
			}
			SetTraitNow(Penguin.Get(), EPinguTrait::None);
			M->SetSizeScale(1.f);
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Report(FString::Printf(TEXT("RESULT flap scale: %sas a jump scales (its jump height ratio x the pad's %.0f) %s"), *Line, Pad, *Ok(bAll && NoneHeight > 0.f)));
		},
		[](float) {}, [] {});

	AddStep(TEXT("flap_HostJudge"), 0.1f,
		[this]
		{
			// A LAN joiner's flap as the host's judge sees it, moves 60 a second: the host's StartFlap noted its climb (the same
			// NoteHostPush StartFlap makes), the owner climbs at the flap's speed to its top and falls back; then the same climb
			// with nothing noted (a faked flap).
			const UPenguinMovementComponent* M = Move.Get();
			const FPinguMoveLimits Limits = M ? M->GetHostMoveLimits() : FPinguMoveLimits();
			const float G = Limits.Gravity;
			const float Pad = M ? PadHeight(M) : 986.f;
			const float Speed = (M ? M->JumpPadSpeed : 2970.f) * PinguTuning::FlapSpeedShare;
			const auto Climb = [&](float Height, bool bNoted, bool bGround, int32 Flaps)
			{
				FPinguMoveJudge Judge;
				double Now = 500.0;
				FVector At(0.f, 0.f, 0.f);
				FPinguMoveSample Sample;
				Sample.Location = At;
				Sample.Now = Now;
				Sample.bSupported = true;
				FString Why;
				Judge.Judge(Sample, Limits, Why);
				if (!bGround)
				{
					// Off a ledge first: half a second of falling (it is in the air when it flaps).
					float Vz = 0.f;
					for (int32 Step = 0; Step < 30; ++Step)
					{
						Now += 1.0 / 60.0;
						Vz -= G * 1.6f / 60.f;
						At.Z += Vz / 60.f;
						Sample.Now = Now;
						Sample.Location = At;
						Sample.bSupported = false;
						Judge.Judge(Sample, Limits, Why);
					}
				}
				int32 Refused = 0;
				for (int32 Flap = 0; Flap < Flaps; ++Flap)
				{
					if (bNoted)
					{
						Judge.NoteHostPush(Now, At, FVector(0.f, 0.f, FMath::Sqrt(2.f * G * (Height * 1.15f + 50.f))), true, false, Limits);
					}
					const float Top = static_cast<float>(At.Z) + Height;
					float Vz = Speed;
					// (The last flap: down again after its top; an earlier one: the next flap at its top.)
					for (int32 Step = 0; Step < 180 && (Vz > 0.f || (Flap == Flaps - 1 && At.Z > Top - Height - 200.f)); ++Step)
					{
						Now += 1.0 / 60.0;
						const float Coast = FMath::Sqrt(2.f * G * FMath::Max(0.f, Top - static_cast<float>(At.Z)));
						Vz = Vz > 0.f && Coast > Speed ? Speed : Vz - G * (Vz < 0.f ? 1.6f : 1.f) / 60.f;
						At.Z += Vz / 60.f;
						Sample.Now = Now;
						Sample.Location = At;
						Sample.bSupported = false;
						Refused += Judge.Judge(Sample, Limits, Why) ? 0 : 1;
					}
				}
				return Refused;
			};
			const int32 GroundNoted = Climb(Pad, true, true, 1);
			const int32 FallNoted = Climb(Pad * 0.75f, true, false, 1);
			// Faked: flapping again and again (a pad's height each), never asked of the host. One fake flap is within what its own
			// jumps, air dash and climb could lift it (it passes); flying on does not.
			const int32 OneFake = Climb(Pad, false, true, 1);
			const int32 FlyingFake = Climb(Pad, false, true, 3);
			Report(FString::Printf(TEXT("RESULT flap host judge: an honest flap from the ground refused %d times, from a fall %d (want 0 each); without the host's ")
				TEXT("flap one climb refused %d times (within its own jumps' reach), three in a row (flying) %d times (want 1 or more) %s"), GroundNoted, FallNoted,
				OneFake, FlyingFake, *Ok(GroundNoted == 0 && FallNoted == 0 && FlyingFake > 0)));
		},
		[](float) {}, [] {});

	// look_Flap: close-ups of the beat (the stage camera beside the penguin) from the ground and while falling, and a wide one at
	// the top of the ground flap with the strip below (how high: the icicle pad's 986 cm).
	AddStep(TEXT("look_Flap"), 4.6f,
		[this]
		{
			Run() = FFlapRun();
			PrepareBrawl(3000.f, { EPenguinAbility::Flap });
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this](float T)
		{
			FFlapRun& R = Run();
			UPenguinAbilityComponent* A = Penguin->GetAbilities();
			if (!R.bShotHud && T > 0.1f)
			{
				R.bShotHud = true;
				Shot(TEXT("95_flap_hud"), true); // the HUD with the Flap icon in its slot
			}
			if (!R.bPressed && T > 0.2f)
			{
				R.bPressed = true;
				R.StartZ = static_cast<float>(Penguin->GetFeetLocation().Z);
				A->UseAbility(0, Location());
			}
			// From the ground: three close-ups in the climb (7 beats a second: each a different moment of the beat) ...
			static const float Close[] = { 0.30f, 0.37f, 0.44f, 0.52f };
			// ... a wide one at the top, then a drop from there and a flap in the fall (two close-ups).
			const float WideAt = 1.25f;
			const float SecondAt = 2.6f;
			static const float FallClose[] = { 2.68f, 2.76f };
			if (T < WideAt - 0.02f || (T > SecondAt && T < 4.4f))
			{
				if (AActor* Camera = LookCamera.Get())
				{
					// Beside it, a little ahead of the climb (a shot is written a frame or two after it is asked for).
					const FVector LookAt = Penguin->GetActorLocation() + FVector(0.f, 0.f, 15.f) + Move->Velocity * 0.04f;
					const FVector From = LookAt + FVector(-60.f, 640.f, 90.f);
					Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
			}
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Close); ++Index)
			{
				if (R.FramesSincePress < Index && T >= Close[Index])
				{
					R.FramesSincePress = Index;
					Shot(*FString::Printf(TEXT("95_flap_ground_%d"), Index));
				}
			}
			if (!R.bLanded && T >= WideAt)
			{
				R.bLanded = true;
				// The game's own camera for the wide one (it frames the penguin and the strip under it).
				if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
				{
					Stage->SetActorTickEnabled(true);
					Stage->SnapToTargets();
				}
				Report(FString::Printf(TEXT("look Flap: the wide shot at %.0f cm over the strip"), Penguin->GetFeetLocation().Z - R.StartZ));
			}
			if (R.bLanded && !R.bUsed && T >= WideAt + 0.05f)
			{
				R.bUsed = true;
				Shot(TEXT("95_flap_ground_top_wide"));
			}
			if (R.ClimbStartT < 0.f && T >= SecondAt)
			{
				if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
				{
					Stage->SetActorTickEnabled(false); // (close up again for the flap in a fall)
				}
				// Up 6 m, falling, then a flap (a fresh one: the cooldown is not what this shows).
				R.ClimbStartT = T;
				Penguin->ResetForRound(Penguin->GetActorLocation());
				A->SetAbilities({ EPenguinAbility::Flap });
				Teleport(Location() + FVector(0.f, 0.f, 600.f));
			}
			if (R.ClimbStartT > 0.f && R.ClimbEndT < 0.f && T >= SecondAt + 0.05f)
			{
				R.ClimbEndT = T;
				A->UseAbility(0, Location());
			}
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(FallClose); ++Index)
			{
				if (R.ClimbEndT > 0.f && R.PeakVz < Index + 1 && T >= FallClose[Index])
				{
					R.PeakVz = static_cast<float>(Index + 1);
					Shot(*FString::Printf(TEXT("95_flap_fall_%d"), Index));
				}
			}
		},
		[this]
		{
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			Report(TEXT("RESULT look Flap: 95_flap_ground_0..3 (the beat close up), 95_flap_ground_top_wide (the top), 95_flap_fall_0..1 (a flap in a fall) taken ok"));
		});

	// look_FlapWind_<level>: the wind of the beats (APinguFX::FlapWind) at one Effects setting: close-ups just after the press (the
	// penguin is on the ground: the snow ring), then a flap in a fall 6 m up (air only). Each puts the setting back afterwards.
	struct FWindLevel { const TCHAR* Name; EPinguQualityLevel Level; };
	static const FWindLevel WindLevels[] = { { TEXT("Low"), EPinguQualityLevel::Low }, { TEXT("Medium"), EPinguQualityLevel::Medium },
		{ TEXT("High"), EPinguQualityLevel::High }, { TEXT("Ultra"), EPinguQualityLevel::Ultra } };
	for (const FWindLevel& L : WindLevels)
	{
		const FString Name = FString::Printf(TEXT("look_FlapWind_%s"), L.Name);
		const FString Tag = FString(L.Name).ToLower();
		const EPinguQualityLevel Level = L.Level;
		AddStep(*Name, 3.4f,
			[this, Level]
			{
				Run() = FFlapRun();
				UPinguSettings* Settings = UPinguSettings::Get();
				FPinguGraphicsOptions Options = Settings->GetGraphics();
				Options.Effects = Level;
				Settings->SetGraphics(Options);
				APinguFX::SetUltraOverride(-1);
				PrepareBrawl(3000.f, { EPenguinAbility::Flap });
				if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
				{
					Stage->SetActorTickEnabled(false);
					LookCamera = Stage;
				}
			},
			[this, Tag](float T)
			{
				FFlapRun& R = Run();
				UPenguinAbilityComponent* A = Penguin->GetAbilities();
				if (AActor* Camera = LookCamera.Get())
				{
					// Ground part: fixed, low and far enough to show the penguin within 2 m of the snow with the ground under it; the air
					// part: close beside the penguin.
					FVector LookAt = Penguin->GetActorLocation() + FVector(0.f, 0.f, 10.f);
					FVector From = LookAt + FVector(-120.f, 520.f, 50.f);
					if (R.ClimbStartT < 0.f)
					{
						if (!R.bPressed)
						{
							R.StartZ = static_cast<float>(Penguin->GetFeetLocation().Z);
							R.PeakZ = static_cast<float>(Penguin->GetActorLocation().X);
						}
						LookAt = FVector(R.PeakZ, 0.f, R.StartZ + 90.f);
						From = LookAt + FVector(-70.f, 600.f, 15.f);
					}
					Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
				if (!R.bPressed && T > 0.3f)
				{
					R.bPressed = true;
					A->UseAbility(0, Location());
				}
				static const float Ground[] = { 0.33f, 0.40f, 0.47f };
				for (int32 Index = 0; Index < UE_ARRAY_COUNT(Ground); ++Index)
				{
					if (R.FramesSincePress < Index && R.bPressed && T >= Ground[Index])
					{
						R.FramesSincePress = Index;
						Shot(*FString::Printf(TEXT("96_flapwind_%s_ground_%d"), *Tag, Index));
					}
				}
				if (R.ClimbStartT < 0.f && T >= 1.6f)
				{
					R.ClimbStartT = T;
					Penguin->ResetForRound(Penguin->GetActorLocation());
					A->SetAbilities({ EPenguinAbility::Flap });
					Teleport(Location() + FVector(0.f, 0.f, 600.f));
				}
				if (R.ClimbStartT > 0.f && R.ClimbEndT < 0.f && T >= 1.65f)
				{
					R.ClimbEndT = T;
					A->UseAbility(0, Location());
				}
				static const float Air[] = { 1.75f, 1.80f, 1.86f };
				for (int32 Index = 0; Index < UE_ARRAY_COUNT(Air); ++Index)
				{
					if (R.ClimbEndT > 0.f && R.PeakVz < Index + 1 && T >= Air[Index])
					{
						R.PeakVz = static_cast<float>(Index + 1);
						Shot(*FString::Printf(TEXT("96_flapwind_%s_air_%d"), *Tag, Index));
					}
				}
			},
			[this, Tag]
			{
				if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
				{
					Stage->SetActorTickEnabled(true);
					Stage->SnapToTargets();
				}
				LookCamera.Reset();
				UPinguSettings* Settings = UPinguSettings::Get();
				FPinguGraphicsOptions Options = Settings->GetGraphics();
				Options.Effects = EPinguQualityLevel::High;
				Settings->SetGraphics(Options);
				Report(FString::Printf(TEXT("RESULT look FlapWind %s: 96_flapwind_%s_ground_0..2, _air_0..2 taken ok"), *Tag, *Tag));
			});
	}
}
