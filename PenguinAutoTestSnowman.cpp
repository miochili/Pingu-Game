// Pingu - the snowman (APinguSnowman, the 18th ability, 2026-11-03). The autotest's steps (APenguinAutoTest::AddSnowmanSteps;
// filter snowman_ + spawn_target, the pictures look_Snowman):
//
//  look_SnowmanBuild    the ability used: a snowman stands in front of the penguin after its quick build (90_snowman_build_1..4,
//                       90_snowman_hud_icon).
//  look_SnowmanLook     its look (2026-11-04 card snowman-look) on Ultra and on Low: a close-up, three-quarter, next to its builder
//                       for scale, at play distance with the game's own camera (it throws at the dummy); its height and its
//                       triangles on each preset (91_snowman_look_*).
//  snowman_Forever      no lifetime: still standing 15 s later, and it never moves.
//  snowman_Throw        it throws at the dummy (never at its builder next to it), every SnowmanReload s (90_snowman_throw).
//  snowman_Teammate     the dummy in its builder's team: no throw.
//  snowman_Combo        two hits in a row: the builder's combo goes 1 -> 2 ("x2" on the dummy), the builder's snowball, cooling
//                       down, is not given back (90_snowman_combo).
//  snowman_GiantGrow    its snowball into a Giant Snowball grows it.
//  snowman_Unlimited    (2026-11-05) four built in a row by one penguin: all four stand, one per tile (96_snowman_tiles).
//  snowman_TakenTile    (2026-11-05) the 2nd on the same spot goes one tile further, the 3rd is refused (cooldown unspent, a
//                       "no" puff).
//  snowman_Air          (2026-11-05) in mid-air: refused, cooldown unspent, a "no" puff, nothing built.
//  snowman_TileGone     (2026-11-05; was snowman_Falls) the block under it goes: it breaks.
//  snowman_FastAfterHit (2026-11-05) a hit: its next throw SnowmanHitReload s after the last; a miss: SnowmanReload.
//  snowman_Many         (2026-11-05) twelve snowmen on Low throwing at the dummy: the frame time against none.
//  snowman_Break<case>  snowball, icicle, ice bolt, fish hook, bomb, slap, noot, frost, stomp, slide, hot feet, giant snowball,
//                       tornado, iceberg: each breaks it (90_snowman_break).
//  snowman_Rays         the grow ray (fired by the dummy: its builder's own rays pass) makes it bigger, the shrink ray smaller, and past half its size it melts.
//  snowman_OwnShots     (2026-11-05) own snowball, icicle, ice bolt, grow and shrink ray fly through its builder's snowman.
//  snowman_OwnBombBody  an own bomb's flying body passes through it.
//  snowman_OwnMany      two of one builder's snowmen: the first one's balls fly through the second.
//  snowman_OwnBreakNoot / _OwnBreakBomb  the builder's noot and a bomb's blast still break it.
//  snowman_TeamShot     a teammate's snowball breaks it.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinBot.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PinguGiantSnowball.h"
#include "PinguBuildLimit.h"
#include "PinguSettings.h"
#include "PinguSnowman.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "ShaderCompiler.h"
#include "HAL/PlatformTime.h"

namespace PenguinAutoTestSnowmanPrivate
{
	using namespace PinguTuning;

	struct FRun
	{
		TWeakObjectPtr<APinguSnowman> Snowman;
		bool bBuilt = false;
		bool bSeen = false;
		bool bGone = false;
		float BuiltAt = -1.f;
		float GoneAt = -1.f;
		FString End;
		float Values[8] = { -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f };
		int32 Counts[6] = { 0, 0, 0, 0, 0, 0 };
		bool Flags[6] = { false, false, false, false, false, false };
		int32 Shots = 0;
	};

	/** look_SnowmanLook: a preset switch settles this long (the shaders it compiles at most LookShaderWait s). */
	constexpr float LookSettle = 0.8f;
	constexpr float LookShaderWait = 30.f;

	static const TCHAR* Ok(bool b) { return b ? TEXT("ok") : TEXT("FAIL"); }

	static void Watch(FRun& R, float T)
	{
		if (R.Snowman.IsValid())
		{
			R.bSeen = true;
			return;
		}
		if (R.bSeen && !R.bGone)
		{
			R.bGone = true;
			R.GoneAt = T;
			R.End = APinguSnowman::LastEnd;
		}
	}

	static void SetTrait(APenguinCharacter* Who, EPinguTrait Trait)
	{
		if (UPenguinTraitComponent* Traits = Who ? Who->GetTraits() : nullptr)
		{
			Traits->SetTrait(Trait, -1.f);
		}
	}

	/** A snowman built by Who toward Side, standing at once (the build's time is spent before it is checked). */
	static APinguSnowman* BuildFor(APenguinCharacter* Who, float Side)
	{
		FVector Feet;
		return Who && APinguSnowman::FindSpot(Who, Side, Feet) ? APinguSnowman::Build(Who, Feet, Side) : nullptr;
	}
}

void APenguinAutoTest::AddSnowmanSteps()
{
	using namespace PenguinAutoTestSnowmanPrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	const auto CleanUp = [this, Base]()
	{
		VirtualInput = FVector2D::ZeroVector;
		ClearAbilityLeftovers();
		Penguin->CancelAbilityKeys();
		SetTrait(Penguin.Get(), EPinguTrait::None);
		Penguin->SetTeam(-1);
		Penguin->ResetForRound(Penguin->GetActorLocation());
		if (Target.IsValid())
		{
			Target->SetTeam(-1);
			SetTrait(Target.Get(), EPinguTrait::None);
			if (SavedTargetAbilities.Num() > 0)
			{
				Target->GetAbilities()->SetAbilities(SavedTargetAbilities);
				SavedTargetAbilities.Reset();
			}
			Target->ResetForRound(Target->GetActorLocation().Z < -500.f ? Base + FVector(1500.f, 0.f, 0.f) : Target->GetActorLocation());
		}
		if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
		{
			Stage->SetActorTickEnabled(true);
			Stage->SnapToTargets();
		}
		LookCamera.Reset();
		if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
		{
			Mode->SetActorTickEnabled(true);
		}
	};
	const auto HoldCamera = [this]()
	{
		if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
		{
			Stage->SetActorTickEnabled(false);
			LookCamera = Stage;
		}
		if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
		{
			Mode->SetActorTickEnabled(false); // (no practice respawn, no sky drops)
		}
	};
	const auto LookAt = [this](const FVector& At, float Distance, float Up)
	{
		if (AActor* Camera = LookCamera.Get())
		{
			const FVector From = At + FVector(0.f, Distance, Up);
			Camera->SetActorLocationAndRotation(From, (At - From).Rotation());
		}
	};

	// ---- look_SnowmanBuild: the ability (as players, bots and tests use it): a snowman stands in front of the penguin, built in a
	// moment; the HUD icon over the head first.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("look_SnowmanBuild"), 2.f,
			[this, Run, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowman, EPenguinAbility::Snowball, EPenguinAbility::Noot });
				HoldCamera();
			},
			[this, Run, LookAt](float T)
			{
				FRun& R = *Run;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
				LookAt(Penguin->GetActorLocation() + FVector(60.f, 0.f, 10.f), T < 0.3f ? 700.f : 460.f, 40.f);
				if (R.Shots == 0 && T > 0.2f)
				{
					Shot(TEXT("90_snowman_hud_icon"), true);
					++R.Shots;
				}
				if (!R.bBuilt && T > 0.35f)
				{
					R.bBuilt = Penguin->ActivateAbility(0);
					R.BuiltAt = T;
					R.Snowman = APinguSnowman::FindOf(GetWorld(), Penguin.Get());
					R.Values[0] = R.Snowman.IsValid() ? static_cast<float>(R.Snowman->GetFeet().X - Penguin->GetActorLocation().X) : -1.f;
					R.Values[1] = R.Snowman.IsValid() ? static_cast<float>(R.Snowman->GetFeet().Z) : -1.f;
					// (On a tile's middle: 50 cm into its 100 cm column.)
					const ABlockIsland* Flat = R.Snowman.IsValid() ? ABlockIsland::FindNear(GetWorld(), R.Snowman->GetFeet()) : nullptr;
					R.Values[2] = Flat ? FMath::Fmod(static_cast<float>(R.Snowman->GetFeet().X - Flat->GetActorLocation().X) + 100000.f, Flat->CellSize) : -1.f;
				}
				static const float Times[] = { 0.08f, 0.18f, 0.3f, 0.6f };
				if (R.bBuilt && R.Shots >= 1 && R.Shots <= 4 && T - R.BuiltAt > Times[R.Shots - 1])
				{
					Shot(*FString::Printf(TEXT("90_snowman_build_%d"), R.Shots));
					++R.Shots;
				}
				if (R.Snowman.IsValid() && T - R.BuiltAt > 0.6f)
				{
					R.Flags[0] |= R.Snowman->IsStanding();
					R.Counts[0] = R.Snowman->GetShownParts();
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const float Cooldown = Penguin->GetAbilities()->GetCooldownRemaining(0);
				const bool bOk = R.bBuilt && R.Values[0] > 40.f && R.Values[0] < 160.f && FMath::Abs(R.Values[2] - 50.f) < 1.f && FMath::Abs(R.Values[1]) < 3.f && R.Flags[0]
					&& R.Counts[0] >= 9 && Cooldown > 3.f && R.Shots == 5;
				Report(FString::Printf(TEXT("RESULT snowman build: used %d, standing %.0f cm in front of its builder (want 40 - 160: the next tile) at %.0f cm into its tile (want 50: the middle) on the floor (z %.0f), built %d, %d parts drawn (want 9 or more: balls, face, buttons, arms, hat, scarf), cooldown %.1f s, %d of 5 pictures: %s"),
					R.bBuilt ? 1 : 0, R.Values[0], R.Values[2], R.Values[1], R.Flags[0] ? 1 : 0, R.Counts[0], Cooldown, R.Shots, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- look_SnowmanLook (card 2026-11-04_snowman-look): its look on Ultra, then on Low - close-up, three-quarter, next to its
	// builder, at play distance (the game's camera, the dummy in reach: it throws). Its height (the top of its bounds over its feet,
	// the arms down) and its triangles on each preset.
	{
		struct FLook
		{
			TWeakObjectPtr<APinguSnowman> Snowman;
			FPinguGraphicsOptions Before;
			bool bPresetSet = false;
			int32 Stage = 0;
			float At = 0.f;
			int32 Shots = 0;
			int32 ThrowsAt = 0;
			float ThrownAt = -1.f;
			bool bGameCamera = false;
			float Height[2] = { -1.f, -1.f };
			float Width[2] = { -1.f, -1.f };
			int32 Triangles[2] = { 0, 0 };
			int32 Parts[2] = { 0, 0 };
		};
		const TSharedRef<FLook> Look = MakeShared<FLook>();
		AddStep(TEXT("look_SnowmanLook"), 14.f + 2.f * LookShaderWait,
			[this, Look, HoldCamera]
			{
				*Look = FLook();
				if (UPinguSettings* Settings = UPinguSettings::Get())
				{
					Look->Before = Settings->GetGraphics();
					Look->bPresetSet = true;
					Settings->ApplyPreset(EPinguGraphicsPreset::Ultra);
				}
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
				HoldCamera();
				Look->Snowman = BuildFor(Penguin.Get(), 1.f);
			},
			[this, Look, LookAt](float T)
			{
				FLook& L = *Look;
				APinguSnowman* Snowman = L.Snowman.Get();
				UPinguSettings* Settings = UPinguSettings::Get();
				if (!Snowman || !Settings)
				{
					StepTime = 1000.f;
					return;
				}
				const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
				const auto Ready = [&L, T, bCompiling]() { return T > L.At + LookSettle && (!bCompiling || T > L.At + LookShaderWait); };
				const auto Next = [&L, T](int32 Stage) { L.Stage = Stage; L.At = T; };
				const FVector Feet = Snowman->GetFeet();
				const FVector Body = Feet + FVector(0.f, 0.f, 70.f);
				const auto Place = [this](const FVector& From, const FVector& To)
				{
					if (AActor* Camera = LookCamera.Get())
					{
						Camera->SetActorLocationAndRotation(From, (To - From).Rotation());
					}
				};
				const auto Measure = [&L, Snowman, Feet](int32 Slot)
				{
					const FBox Box = Snowman->GetComponentsBoundingBox(true);
					L.Height[Slot] = static_cast<float>(Box.Max.Z - Feet.Z);
					L.Width[Slot] = static_cast<float>(Box.Max.X - Box.Min.X);
					L.Triangles[Slot] = Snowman->GetLookTriangles();
					L.Parts[Slot] = Snowman->GetShownParts();
				};
				const auto GameCamera = [this](bool bOn)
				{
					if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
					{
						Stage->SetActorTickEnabled(bOn);
						if (bOn)
						{
							Stage->SnapToTargets();
						}
					}
				};
				const TCHAR* Preset = L.Stage < 5 ? TEXT("ultra") : TEXT("low");
				const int32 Slot = L.Stage < 5 ? 0 : 1;
				switch (L.Stage)
				{
				case 0: // Ultra (or Low from stage 5 on): a close-up from the front
				case 5:
					LookAt(Body, 400.f, 10.f);
					if (Ready() && Snowman->IsStanding())
					{
						Measure(Slot);
						Shot(*FString::Printf(TEXT("91_snowman_look_close_%s"), Preset));
						++L.Shots;
						Next(L.Stage + 1);
					}
					break;
				case 1: // three-quarter, a little from above (the balls sunk into each other, the hat, the scarf)
				case 6:
					Place(Body + FVector(260.f, 290.f, 100.f), Body + FVector(0.f, 0.f, 6.f));
					if (T > L.At + 0.3f)
					{
						Shot(*FString::Printf(TEXT("91_snowman_look_side_%s"), Preset));
						++L.Shots;
						Next(L.Stage + 1);
					}
					break;
				case 2: // next to its builder, for scale
				case 7:
				{
					const FVector Pair = (Feet + Penguin->GetFeetLocation()) * 0.5f + FVector(0.f, 0.f, 60.f);
					LookAt(Pair, 520.f, 20.f);
					if (T > L.At + 0.3f)
					{
						Shot(*FString::Printf(TEXT("91_snowman_look_scale_%s"), Preset));
						++L.Shots;
						L.bGameCamera = false;
						Next(L.Stage + 1);
					}
					break;
				}
				case 3: // at play distance (the game's camera), a snowball on its way
				case 8:
					if (!L.bGameCamera)
					{
						// (a moment after the last picture: it is written at the end of its frame) The dummy in reach and the
						// game's camera: the play-distance picture once it has thrown.
						if (T > L.At + 0.25f)
						{
							ResetTarget(Penguin->GetActorLocation() + FVector(720.f, 0.f, 0.f));
							L.ThrowsAt = Snowman->GetThrows();
							GameCamera(true);
							L.bGameCamera = true;
							L.At = T;
						}
						break;
					}
					if (L.ThrownAt < 0.f && Snowman->GetThrows() > L.ThrowsAt)
					{
						L.ThrownAt = T; // (the picture a moment later: the ball on its way)
					}
					if (L.ThrownAt >= 0.f && T > L.ThrownAt + 0.22f)
					{
						L.ThrownAt = -1.f;
						Shot(*FString::Printf(TEXT("91_snowman_look_play_%s"), Preset));
						++L.Shots;
						GameCamera(false);
						ResetTarget(Penguin->GetActorLocation() + FVector(2400.f, 0.f, 0.f));
						Next(L.Stage + 1);
					}
					else if (T > L.At + 6.f)
					{
						Next(L.Stage + 1); // (no throw: the result says so)
						GameCamera(false);
					}
					break;
				case 4: // Low
					if (T > L.At + 0.3f)
					{
						Settings->ApplyPreset(EPinguGraphicsPreset::Low);
						Next(5);
					}
					break;
				default:
					if (T > L.At + 0.3f)
					{
						StepTime = 1000.f; // done (the last picture is written)
					}
					break;
				}
			},
			[this, Look, CleanUp]
			{
				const FLook& L = *Look;
				if (UPinguSettings* Settings = UPinguSettings::Get(); Settings && L.bPresetSet)
				{
					if (L.Before.Preset == EPinguGraphicsPreset::Custom)
					{
						Settings->SetGraphics(L.Before);
					}
					else
					{
						Settings->ApplyPreset(L.Before.Preset);
					}
				}
				// Linus: "overall height a bit shorter" - the card wants 15 - 20 % under the first look's 165 cm (hat included); Low
				// cheap, Ultra no more than a few thousand triangles more.
				const bool bOk = L.Snowman.IsValid() && L.Shots == 8 && L.Height[0] > 110.f && L.Height[0] < 145.f && FMath::Abs(L.Height[1] - L.Height[0]) < 3.f
					&& L.Triangles[1] > 0 && L.Triangles[1] <= 8000 && L.Triangles[0] >= L.Triangles[1] && L.Triangles[0] <= 24000 && L.Parts[0] >= 9 && L.Parts[1] == L.Parts[0];
				Report(FString::Printf(TEXT("RESULT snowman look: %d of 8 pictures, height %.0f cm on Ultra / %.0f on Low (want 110 - 145, the first look 165), %.0f cm wide, %d / %d triangles (want Low at most 8000, Ultra at most 24000), %d / %d parts drawn (want 9 or more): %s"),
					L.Shots, L.Height[0], L.Height[1], L.Width[0], L.Triangles[0], L.Triangles[1], L.Parts[0], L.Parts[1], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_Forever: no lifetime - still standing 15 s later, never moved (no enemy near it: the dummy is far away).
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_Forever"), 15.5f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
				Run->Snowman = BuildFor(Penguin.Get(), 1.f);
				Run->Values[0] = Run->Snowman.IsValid() ? static_cast<float>(Run->Snowman->GetFeet().X) : 0.f;
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (R.Snowman.IsValid())
				{
					R.Values[1] = FMath::Max(R.Values[1], static_cast<float>(FMath::Abs(R.Snowman->GetFeet().X - R.Values[0])));
				}
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Snowman.IsValid() && !R.bGone && R.Values[1] < 0.5f;
				Report(FString::Printf(TEXT("RESULT snowman forever: standing after 15 s %d, moved %.1f cm (want 0): %s"), R.Snowman.IsValid() ? 1 : 0, FMath::Max(R.Values[1], 0.f), Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_Throw / snowman_Teammate: the dummy 6 m away; its builder right next to it. It throws at the dummy every
	// SnowmanReload s - or, the dummy on its builder's team, never. (Since 2026-11-05 a hit makes the next throw come sooner: here
	// every ball flies through the dummy, so none hits and the normal reload shows.)
	for (int32 Variant = 0; Variant < 2; ++Variant)
	{
		const bool bTeam = Variant == 1;
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(bTeam ? TEXT("snowman_Teammate") : TEXT("snowman_Throw"), 4.2f,
			[this, Run, bTeam, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(680.f, { EPenguinAbility::Snowball });
				if (bTeam && Target.IsValid())
				{
					Penguin->SetTeam(2);
					Target->SetTeam(2);
				}
				HoldCamera();
				Run->Snowman = BuildFor(Penguin.Get(), 1.f);
			},
			[this, Run, bTeam, LookAt](float T)
			{
				FRun& R = *Run;
				APinguSnowman* Snowman = R.Snowman.Get();
				if (!Snowman)
				{
					return;
				}
				LookAt(Snowman->GetFeet() + FVector(260.f, 0.f, 80.f), 1000.f, 80.f);
				// The dummy kept where it is (the hits push it away).
				if (Target.IsValid() && Target->GetPenguinMovement() && Target->GetPenguinMovement()->IsMovingOnGround()
					&& FMath::Abs(Target->GetActorLocation().X - (Location().X + 680.f)) > 60.f)
				{
					Target->ResetForRound(Location() + FVector(680.f, 0.f, 0.f));
				}
				const int32 Throws = Snowman->GetThrows();
				if (Throws > R.Counts[0])
				{
					// (Values: 0 / 1 the times of the first two throws)
					if (R.Counts[0] == 0) { R.Values[0] = T; }
					if (R.Counts[0] == 1) { R.Values[1] = T; }
					R.Counts[0] = Throws;
					R.Flags[0] |= Snowman->GetLastTarget() == Penguin.Get();
					R.Flags[1] |= Snowman->GetLastTarget() == Target.Get();
					R.Values[2] = T;
				}
				if (R.Values[2] > 0.f)
				{
					// (A miss: the new ball flies through the dummy.)
					TArray<APenguinProjectile*> Balls;
					APenguinProjectile::GetAll(GetWorld(), Balls);
					for (APenguinProjectile* Ball : Balls)
					{
						if (Ball && Ball->GetKind() == EPenguinProjectileKind::Snowball && Target.IsValid())
						{
							Ball->IgnorePenguin(Target.Get(), 3.f);
						}
					}
					R.Values[2] = -1.f;
				}
				if (!bTeam && R.Shots == 0 && R.Counts[0] >= 1 && T > R.Values[0] + 0.12f)
				{
					Shot(TEXT("90_snowman_throw"));
					++R.Shots;
				}
			},
			[this, Run, bTeam, CleanUp]
			{
				const FRun& R = *Run;
				bool bOk = false;
				if (bTeam)
				{
					bOk = R.Counts[0] == 0;
					Report(FString::Printf(TEXT("RESULT snowman teammate: %d throws at its builder's teammate (want 0): %s"), R.Counts[0], Ok(bOk)));
				}
				else
				{
					const float Interval = R.Values[1] > 0.f ? R.Values[1] - R.Values[0] : -1.f;
					const float First = R.Values[0] - 0.f;
					bOk = R.Counts[0] >= 2 && !R.Flags[0] && R.Flags[1] && FMath::Abs(Interval - SnowmanReload) < 0.1f && First < SnowmanBuildTime + SnowmanFirstThrow + 0.2f;
					Report(FString::Printf(TEXT("RESULT snowman throw: %d throws, the first after %.2f s (want about %.2f), then every %.2f s (want %.2f), at the dummy %d, at its builder %d (want 0): %s"),
						R.Counts[0], First, SnowmanBuildTime + SnowmanFirstThrow, Interval, SnowmanReload, R.Flags[1] ? 1 : 0, R.Flags[0] ? 1 : 0, Ok(bOk)));
				}
				CleanUp();
			});
	}

	// ---- snowman_Combo: two of its hits in a row are a combo of its builder's (the second one "x2"), and they never give its
	// builder's snowball back: thrown just before, it keeps cooling down.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_Combo"), 3.6f,
			[this, Run, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(520.f, { EPenguinAbility::Snowball });
				HoldCamera();
				Run->Snowman = BuildFor(Penguin.Get(), 1.f);
				Run->Counts[1] = Target.IsValid() && Target->GetPenguinMovement() ? Target->GetPenguinMovement()->HitEvents : 0;
			},
			[this, Run, LookAt](float T)
			{
				FRun& R = *Run;
				APinguSnowman* Snowman = R.Snowman.Get();
				APenguinCharacter* Dummy = Target.Get();
				if (!Snowman || !Dummy)
				{
					return;
				}
				LookAt(Dummy->GetActorLocation(), 700.f, 60.f);
				// Our own snowball thrown straight up just before its first hit: cooling down while the snowman hits.
				if (!R.Flags[0] && T > 0.9f)
				{
					R.Flags[0] = true;
					Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(0.f, 0.f, 500.f));
					Penguin->ActivateAbility(0);
				}
				const int32 Hits = Dummy->GetPenguinMovement() ? Dummy->GetPenguinMovement()->HitEvents - R.Counts[1] : 0;
				const UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
				if (Hits >= 1 && R.Values[0] < 0.f)
				{
					R.Values[0] = Abilities->GetCooldownRemaining(0); // (our snowball: not given back)
					R.Counts[2] = Abilities->GetSnowballComboHits();
				}
				if (Hits >= 2 && R.Values[1] < 0.f)
				{
					R.Counts[3] = Abilities->GetSnowballComboHits();
					R.Counts[4] = Dummy->GetComboPopCount();
					R.Values[1] = T;
				}
				if (R.Values[1] > 0.f && R.Shots == 0 && T > R.Values[1] + 0.05f)
				{
					Shot(TEXT("90_snowman_combo"));
					++R.Shots;
				}
				// The dummy back in place after each hit (on the floor, in range).
				if (Dummy->GetPenguinMovement() && Dummy->GetPenguinMovement()->IsMovingOnGround() && FMath::Abs(Dummy->GetActorLocation().X - (Location().X + 520.f)) > 60.f)
				{
					Dummy->ResetForRound(Location() + FVector(520.f, 0.f, 0.f));
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Flags[0] && R.Values[0] > 0.05f && R.Counts[2] == 1 && R.Counts[3] == 2 && R.Counts[4] == 2;
				Report(FString::Printf(TEXT("RESULT snowman combo: after its 1st hit our snowball still cools down %.2f s (want > 0: not given back), the combo %d, after its 2nd hit %d (want 1, 2), \"x%d\" on the dummy: %s"),
					R.Values[0], R.Counts[2], R.Counts[3], R.Counts[4], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_GiantGrow: a Giant Snowball rolling at it between it and the dummy: its snowball grows the ball.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_GiantGrow"), 2.6f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(800.f, { EPenguinAbility::Snowball });
				Run->Snowman = BuildFor(Penguin.Get(), 1.f);
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.Flags[0] && T > 0.3f && Target.IsValid())
				{
					R.Flags[0] = true;
					if (APinguGiantSnowball* Giant = APinguGiantSnowball::BeginRollUp(Target.Get(), 0, -1.f))
					{
						Giant->LaunchForTest();
					}
				}
				TArray<APinguGiantSnowball*> Giants;
				APinguGiantSnowball::GetAll(GetWorld(), Giants);
				if (Giants.Num() > 0)
				{
					R.Counts[0] = FMath::Max(R.Counts[0], Giants[0]->GetSizeHits());
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				Report(FString::Printf(TEXT("RESULT snowman giant grow: the Giant Snowball grown by %d of its snowballs (want 1 or more): %s"), R.Counts[0], Ok(R.Counts[0] >= 1)));
				CleanUp();
			});
	}

	// ---- snowman_Unlimited (2026-11-05; was snowman_OnePer): no limit per penguin - four built in a row (the penguin a tile on each
	// time) all stand, each on its own tile. Picture: several snowmen, one per tile.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_Unlimited"), 2.2f,
			[this, Run, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowman });
				HoldCamera();
			},
			[this, Run, LookAt, Base](float T)
			{
				FRun& R = *Run;
				static const float BuildAt[] = { 0.1f, 0.35f, 0.6f, 0.85f };
				if (R.Counts[0] < 4 && T > BuildAt[R.Counts[0]])
				{
					// A tile further right each time (the first spot: the next tile in front, as for a player).
					Teleport(Base + FVector(-200.f + 100.f * R.Counts[0] + 50.f, 0.f, 0.f));
					Penguin->GetAbilities()->GiveAbility(EPenguinAbility::Snowman); // (ready again)
					Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
					R.Counts[1] += Penguin->ActivateAbility(0) ? 1 : 0;
					++R.Counts[0];
				}
				LookAt(Base + FVector(0.f, 0.f, 20.f), 900.f, 120.f);
				if (R.Counts[0] == 4 && R.Shots == 0 && T > 1.6f)
				{
					Teleport(Base + FVector(-450.f, 0.f, 0.f)); // (out of the picture's middle)
					Shot(TEXT("96_snowman_tiles"));
					++R.Shots;
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				TArray<APinguSnowman*> All;
				APinguSnowman::GetAll(GetWorld(), All);
				TSet<FIntPoint> Tiles;
				int32 Ours = 0;
				for (const APinguSnowman* Snowman : All)
				{
					if (Snowman->GetBuilder() == Penguin.Get())
					{
						++Ours;
						Tiles.Add(Snowman->GetTile());
					}
				}
				const bool bOk = R.Counts[1] == 4 && Ours == 4 && Tiles.Num() == 4 && R.Shots == 1;
				Report(FString::Printf(TEXT("RESULT snowman unlimited: built %d of 4, standing %d (want 4), on %d different tiles (want 4): %s"),
					R.Counts[1], Ours, Tiles.Num(), Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_TakenTile (2026-11-05; Linus: "make it so the snowman gets placed next to the one already placed with max reach of
	// 1 snowman further"): from one spot, the 1st on the tile in front, the 2nd one tile further on, the 3rd refused (nothing built,
	// the ability still ready, a "no" puff).
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_TakenTile"), 1.2f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowman });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				static const float BuildAt[] = { 0.2f, 0.45f, 0.7f };
				if (R.Counts[0] < 3 && T > BuildAt[R.Counts[0]])
				{
					Penguin->GetAbilities()->GiveAbility(EPenguinAbility::Snowman); // (ready again)
					Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
					const int32 RefusalsBefore = PinguBuildLimit::GetRefusals();
					const bool bUsed = Penguin->ActivateAbility(0);
					TArray<APinguSnowman*> All;
					APinguSnowman::GetAll(GetWorld(), All);
					R.Counts[1 + R.Counts[0]] = bUsed ? 1 : 0;
					if (bUsed && All.Num() > 0)
					{
						// (Values 0 / 1: the 1st / 2nd one's X)
						float NewestX = -1.f;
						for (const APinguSnowman* Snowman : All)
						{
							const float X = static_cast<float>(Snowman->GetFeet().X);
							bool bSeenBefore = false;
							for (int32 Index = 0; Index < R.Counts[0]; ++Index)
							{
								bSeenBefore |= FMath::IsNearlyEqual(R.Values[Index], X, 1.f);
							}
							NewestX = bSeenBefore ? NewestX : X;
						}
						R.Values[R.Counts[0]] = NewestX;
					}
					if (R.Counts[0] == 2)
					{
						R.Counts[4] = PinguBuildLimit::GetRefusals() - RefusalsBefore;
						R.Counts[5] = All.Num();
						R.Values[3] = Penguin->GetAbilities()->GetCooldownRemaining(0);
						R.Flags[0] = Penguin->GetAbilities()->IsReady(0);
					}
					++R.Counts[0];
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const float Step = R.Values[1] - R.Values[0];
				const bool bOk = R.Counts[1] == 1 && R.Counts[2] == 1 && R.Counts[3] == 0 && FMath::IsNearlyEqual(Step, 100.f, 1.f) && R.Counts[4] == 1
					&& R.Counts[5] == 2 && R.Flags[0] && R.Values[3] <= 0.f;
				Report(FString::Printf(TEXT("RESULT snowman taken tile: 1st built %d, 2nd built %d %.0f cm further on (want 100: the next tile), 3rd built %d (want 0), \"no\" puffs %d (want 1), snowmen %d (want 2), the ability ready after the refusal %d (cooldown %.2f s, want 0): %s"),
					R.Counts[1], R.Counts[2], Step, R.Counts[3], R.Counts[4], R.Counts[5], R.Flags[0] ? 1 : 0, R.Values[3], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_Air (2026-11-05; Linus: "nothing happens in the air"): used 3 m up in a jump: nothing built, the ability still
	// ready (no cooldown), a "no" puff.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_Air"), 0.8f,
			[this, Run, Base]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowman });
				Teleport(Base + FVector(0.f, 0.f, 300.f));
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.Flags[0] && T > 0.1f)
				{
					R.Flags[0] = true;
					R.Values[0] = static_cast<float>(Penguin->GetFeetLocation().Z);
					const int32 RefusalsBefore = PinguBuildLimit::GetRefusals();
					Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
					R.Flags[1] = Penguin->ActivateAbility(0);
					R.Counts[0] = PinguBuildLimit::GetRefusals() - RefusalsBefore;
					R.Values[1] = Penguin->GetAbilities()->GetCooldownRemaining(0);
					R.Flags[2] = Penguin->GetAbilities()->IsReady(0);
				}
				TArray<APinguSnowman*> All;
				APinguSnowman::GetAll(GetWorld(), All);
				R.Counts[1] = FMath::Max(R.Counts[1], All.Num());
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Flags[0] && !R.Flags[1] && R.Counts[0] == 1 && R.Counts[1] == 0 && R.Flags[2] && R.Values[1] <= 0.f && R.Values[0] > 150.f;
				Report(FString::Printf(TEXT("RESULT snowman air: used at %.0f cm over the floor: built %d (want 0), snowmen %d (want 0), \"no\" puffs %d (want 1), ready %d with cooldown %.2f s (want 0): %s"),
					R.Values[0], R.Flags[1] ? 1 : 0, R.Counts[1], R.Counts[0], R.Flags[2] ? 1 : 0, R.Values[1], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_TileGone (2026-11-05; was snowman_Falls: it fell onto the floor below): built on a one-block platform, the block
	// under it broken - it breaks.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_TileGone"), 2.f,
			[this, Run, Base]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
				{
					TArray<FIntPoint> Cells;
					const FIntPoint First = Flat->WorldToCell(Base + FVector(-150.f, 0.f, -12.f));
					for (int32 Column = 0; Column < 5; ++Column)
					{
						Cells.Add(FIntPoint(First.X + Column, First.Y));
					}
					Flat->AddBlocks(Cells, EBlockKind::Snow, 0.f);
					Flat->FlushEdits();
				}
				Teleport(Base + FVector(0.f, 0.f, 110.f));
				Penguin->ResetForRound(Penguin->GetActorLocation());
				Penguin->SetInputLocked(false);
			},
			[this, Run, Base](float T)
			{
				FRun& R = *Run;
				if (!R.Snowman.IsValid() && !R.Flags[0] && T > 0.3f)
				{
					R.Flags[0] = true;
					R.Snowman = BuildFor(Penguin.Get(), 1.f);
					R.Values[0] = R.Snowman.IsValid() ? static_cast<float>(R.Snowman->GetFeet().Z) : -1.f;
				}
				if (R.Snowman.IsValid() && !R.Flags[1] && T > 0.8f)
				{
					R.Flags[1] = true;
					R.Values[2] = T;
					if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
					{
						Flat->BreakBlockAt(R.Snowman->GetFeet() + FVector(0.f, 0.f, -50.f));
					}
				}
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = FMath::Abs(R.Values[0] - 100.f) < 3.f && R.bGone && R.End == TEXT("its tile went") && R.GoneAt - R.Values[2] < 0.1f;
				Report(FString::Printf(TEXT("RESULT snowman tile gone: built at z %.0f (want 100: on the platform), broken %d (%s; want its tile went) %.2f s after its block went: %s"),
					R.Values[0], R.bGone ? 1 : 0, *R.End, R.GoneAt - R.Values[2], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_FastAfterHit (2026-11-05; Linus: "it also shots a bit faster when it hit someone"): the dummy 3.7 m away. Its
	// 1st ball hits: the 2nd throw comes SnowmanHitReload s after the 1st. The 2nd ball is taken away in flight (a miss): the 3rd
	// comes SnowmanReload s after the 2nd.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_FastAfterHit"), 4.2f,
			[this, Run, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(520.f, { EPenguinAbility::Snowball });
				HoldCamera();
				Run->Snowman = BuildFor(Penguin.Get(), 1.f);
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				APinguSnowman* Snowman = R.Snowman.Get();
				APenguinCharacter* Dummy = Target.Get();
				if (!Snowman || !Dummy)
				{
					return;
				}
				const int32 Throws = Snowman->GetThrows();
				if (Throws > R.Counts[0] && R.Counts[0] < 3)
				{
					R.Values[R.Counts[0]] = T; // (0, 1, 2: the times of the first three throws)
					R.Counts[0] = Throws;
					if (R.Counts[0] == 2)
					{
						R.Counts[1] = Snowman->GetHits(); // (hits before the 2nd throw)
						R.Values[3] = T; // (take the 2nd ball away soon)
					}
				}
				if (R.Values[3] > 0.f && T > R.Values[3] + 0.05f)
				{
					TArray<APenguinProjectile*> Balls;
					APenguinProjectile::GetAll(GetWorld(), Balls);
					for (APenguinProjectile* Ball : Balls)
					{
						if (Ball && Ball->GetKind() == EPenguinProjectileKind::Snowball)
						{
							Ball->Destroy();
						}
					}
					R.Values[3] = -1.f;
				}
				R.Counts[2] = Snowman->GetQuickThrows();
				// The dummy back in place after each hit (on the floor, in range).
				if (Dummy->GetPenguinMovement() && Dummy->GetPenguinMovement()->IsMovingOnGround() && FMath::Abs(Dummy->GetActorLocation().X - (Location().X + 520.f)) > 60.f)
				{
					Dummy->ResetForRound(Location() + FVector(520.f, 0.f, 0.f));
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const float Quick = R.Values[1] > 0.f ? R.Values[1] - R.Values[0] : -1.f;
				const float Normal = R.Values[2] > 0.f ? R.Values[2] - R.Values[1] : -1.f;
				const bool bOk = R.Counts[0] >= 3 && R.Counts[1] >= 1 && FMath::Abs(Quick - SnowmanHitReload) < 0.06f && FMath::Abs(Normal - SnowmanReload) < 0.06f && R.Counts[2] >= 1;
				Report(FString::Printf(TEXT("RESULT snowman fast after a hit: its 1st ball hit %d, the 2nd throw %.2f s after the 1st (want %.2f), the 2nd missed, the 3rd %.2f s after it (want %.2f), quicker throws %d: %s"),
					R.Counts[1], Quick, SnowmanHitReload, Normal, SnowmanReload, R.Counts[2], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_Many (2026-11-05, the card's performance check): on Low, 1 s with none, then twelve snowmen (four builders' worth
	// of tiles in a row) throwing at the dummy for 3 s: the real frame time against none. A measurement (want under 1.5x none, and
	// all twelve standing).
	{
		struct FMany
		{
			FPinguGraphicsOptions Before;
			bool bPresetSet = false;
			bool bBuilt = false;
			int32 Built = 0;
			double Last = 0.0;
			double Sum[2] = { 0.0, 0.0 };
			int32 Frames[2] = { 0, 0 };
			int32 Throws = 0;
			bool bShot = false;
		};
		const TSharedRef<FMany> Many = MakeShared<FMany>();
		AddStep(TEXT("snowman_Many"), 4.6f,
			[this, Many, HoldCamera]
			{
				*Many = FMany();
				if (UPinguSettings* Settings = UPinguSettings::Get())
				{
					Many->Before = Settings->GetGraphics();
					Many->bPresetSet = true;
					Settings->ApplyPreset(EPinguGraphicsPreset::Low);
				}
				SpawnTarget();
				PrepareBrawl(-500.f, { EPenguinAbility::Snowball }); // (the dummy in the middle of the row: every one of them throws)
				HoldCamera();
				Many->Last = FPlatformTime::Seconds();
			},
			[this, Many, LookAt, Base](float T)
			{
				FMany& M = *Many;
				const double Now = FPlatformTime::Seconds();
				const double Frame = Now - M.Last;
				M.Last = Now;
				LookAt(Base + FVector(-550.f, 0.f, 40.f), 1500.f, 150.f);
				if (!M.bBuilt && T > 1.1f)
				{
					// Twelve tiles left of us: our penguin on each in turn, building on the tile in front (toward the dummy).
					M.bBuilt = true;
					for (int32 Index = 0; Index < 12; ++Index)
					{
						Teleport(Base + FVector(-1150.f + 100.f * Index, 0.f, 0.f));
						M.Built += BuildFor(Penguin.Get(), 1.f) ? 1 : 0;
					}
					Teleport(Base + FVector(-1400.f, 0.f, 0.f));
					return;
				}
				// (Frames 0: none, 1 s; 1: twelve, from 0.5 s after they were built.)
				if (T > 0.1f && T < 1.1f)
				{
					M.Sum[0] += Frame;
					++M.Frames[0];
				}
				else if (T > 1.6f)
				{
					M.Sum[1] += Frame;
					++M.Frames[1];
				}
				if (Target.IsValid() && Target->GetPenguinMovement() && Target->GetPenguinMovement()->IsMovingOnGround()
					&& FMath::Abs(Target->GetActorLocation().X - (Base.X - 500.f)) > 60.f)
				{
					Target->ResetForRound(Base + FVector(-500.f, 0.f, 0.f));
				}
				if (!M.bShot && T > 4.f)
				{
					M.bShot = true;
					Shot(TEXT("96_snowman_many_low"));
				}
			},
			[this, Many, CleanUp]
			{
				FMany& M = *Many;
				TArray<APinguSnowman*> All;
				APinguSnowman::GetAll(GetWorld(), All);
				for (const APinguSnowman* Snowman : All)
				{
					M.Throws += Snowman->GetThrows();
				}
				if (M.bPresetSet)
				{
					if (UPinguSettings* Settings = UPinguSettings::Get())
					{
						if (M.Before.Preset == EPinguGraphicsPreset::Custom)
						{
							Settings->SetGraphics(M.Before);
						}
						else
						{
							Settings->ApplyPreset(M.Before.Preset);
						}
					}
				}
				const double None = M.Frames[0] > 0 ? 1000.0 * M.Sum[0] / M.Frames[0] : -1.0;
				const double Twelve = M.Frames[1] > 0 ? 1000.0 * M.Sum[1] / M.Frames[1] : -1.0;
				const bool bOk = M.Built == 12 && All.Num() == 12 && None > 0.0 && Twelve > 0.0 && Twelve < None * 1.5 + 2.0;
				Report(FString::Printf(TEXT("RESULT snowman many (Low): %d of 12 built, %d standing, %d throws in 3 s, frame %.2f ms with none, %.2f ms with twelve (want under 1.5x + 2 ms): %s"),
					M.Built, All.Num(), M.Throws, None, Twelve, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_Break<case>: everything that breaks it. It stands 1.7 m right of our penguin (built by the dummy 2.5 m right of
	// us, facing left); the case's cause comes at it.
	enum class ECause : uint8 { Snowball, Icicle, IceBolt, FishHook, Bomb, Slap, Noot, Frost, Stomp, Slide, HotFeet, Giant, Tornado, Iceberg };
	struct FCause
	{
		ECause Cause;
		const TCHAR* Name;
		const TCHAR* WantEnd;
		EPenguinAbility Ability;
	};
	static const FCause Causes[] = {
		{ ECause::Snowball, TEXT("Snowball"), TEXT("snowball"), EPenguinAbility::Snowball },
		{ ECause::Icicle, TEXT("Icicle"), TEXT("icicle"), EPenguinAbility::Snowball },
		{ ECause::IceBolt, TEXT("IceBolt"), TEXT("ice bolt"), EPenguinAbility::Snowball },
		{ ECause::FishHook, TEXT("FishHook"), TEXT("fish hook"), EPenguinAbility::Snowball },
		{ ECause::Bomb, TEXT("Bomb"), TEXT("snow bomb"), EPenguinAbility::Snowball },
		{ ECause::Slap, TEXT("Slap"), TEXT("flipper slap"), EPenguinAbility::FlipperSlap },
		{ ECause::Noot, TEXT("Noot"), TEXT("noot"), EPenguinAbility::Noot },
		{ ECause::Frost, TEXT("Frost"), TEXT("frost breath"), EPenguinAbility::FrostBreath },
		{ ECause::Stomp, TEXT("Stomp"), TEXT("belly stomp"), EPenguinAbility::BellyStomp },
		{ ECause::Slide, TEXT("Slide"), TEXT("belly slide"), EPenguinAbility::Snowball },
		{ ECause::HotFeet, TEXT("HotFeet"), TEXT("hot feet"), EPenguinAbility::Snowball },
		{ ECause::Giant, TEXT("Giant"), TEXT("giant snowball"), EPenguinAbility::Snowball },
		{ ECause::Tornado, TEXT("Tornado"), TEXT("tornado"), EPenguinAbility::Gust },
		{ ECause::Iceberg, TEXT("Iceberg"), TEXT("iceberg"), EPenguinAbility::Snowball },
	};
	for (const FCause& Cause : Causes)
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		const bool bShotStep = Cause.Cause == ECause::Slap;
		AddStep(*FString::Printf(TEXT("snowman_Break%s"), Cause.Name), 2.4f,
			[this, Run, &Cause, HoldCamera, bShotStep]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(250.f, { Cause.Ability });
				if (bShotStep)
				{
					HoldCamera();
				}
				if (Target.IsValid())
				{
					SavedTargetAbilities = Target->GetAbilities()->GetAbilities();
					Run->Snowman = BuildFor(Target.Get(), -1.f);
					// Its builder away from it (out of the way of what comes at it).
					Target->ResetForRound(Location() + FVector(1600.f, 0.f, 0.f));
				}
				if (Cause.Cause == ECause::HotFeet)
				{
					SetTrait(Penguin.Get(), EPinguTrait::HotFeet);
				}
			},
			[this, Run, &Cause, LookAt, bShotStep](float T)
			{
				FRun& R = *Run;
				APinguSnowman* Snowman = R.Snowman.Get();
				if (Snowman && bShotStep)
				{
					LookAt(Snowman->GetFeet() + FVector(-60.f, 0.f, 80.f), 650.f, 50.f);
				}
				if (Snowman && !R.Flags[0] && T > 0.6f)
				{
					R.Flags[0] = true;
					const FVector Mid = Snowman->GetFeet() + FVector(0.f, 0.f, 80.f);
					const FVector From = Penguin->GetAimOrigin() + FVector(40.f, 0.f, 0.f);
					const FVector Dir = (Mid - From).GetSafeNormal();
					switch (Cause.Cause)
					{
					case ECause::Snowball: APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Penguin.Get(), From, Dir * 2100.f); break;
					case ECause::Icicle: APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Icicle, Penguin.Get(), From, Dir * 2600.f); break;
					case ECause::IceBolt: APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::IceBolt, Penguin.Get(), From, Dir * 2600.f); break;
					case ECause::FishHook: APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::FishHook, Penguin.Get(), From, Dir * APenguinProjectile::HookSpeed); break;
					case ECause::Bomb:
						if (APenguinProjectile* Bomb = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin.Get(), Snowman->GetFeet() + FVector(-90.f, 0.f, 30.f), FVector::ZeroVector))
						{
							Bomb->Detonate(0.f);
						}
						Teleport(Location() + FVector(-500.f, 0.f, 0.f)); // (out of the blast)
						break;
					case ECause::Slap:
					case ECause::Noot:
					case ECause::Frost:
					case ECause::Tornado:
						Penguin->SetAimPoint(Mid);
						R.Flags[1] = Penguin->ActivateAbility(0);
						break;
					case ECause::Stomp:
						Teleport(Snowman->GetFeet() + FVector(-20.f, 0.f, 320.f));
						Penguin->SetAimPoint(Snowman->GetFeet());
						R.Flags[1] = Penguin->ActivateAbility(0);
						break;
					case ECause::Slide:
						VirtualInput = FVector2D(1.f, 0.f);
						Penguin->BellySlide();
						break;
					case ECause::HotFeet:
						VirtualInput = FVector2D(1.f, 0.f);
						break;
					case ECause::Giant:
						if (APinguGiantSnowball* Giant = APinguGiantSnowball::BeginRollUp(Penguin.Get(), 0, 1.f))
						{
							Giant->LaunchForTest();
						}
						break;
					case ECause::Iceberg:
						AIcebergDrop::Drop(GetWorld(), Penguin.Get(), static_cast<float>(Snowman->GetFeet().X), 0.f);
						break;
					}
				}
				if (bShotStep && R.Flags[0] && R.Shots == 0 && T > 0.64f)
				{
					Shot(TEXT("90_snowman_break"));
					++R.Shots;
				}
				Watch(R, T);
			},
			[this, Run, &Cause, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.bGone && R.End == Cause.WantEnd && APinguSnowman::FindOf(GetWorld(), Target.Get()) == nullptr;
				Report(FString::Printf(TEXT("RESULT snowman break %s: broken %d (%s; want %s) %.2f s after it came: %s"), Cause.Name, R.bGone ? 1 : 0, *R.End, Cause.WantEnd,
					R.GoneAt - 0.6f, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- bot_SnowmanUse: a bot with the ability and us 6 m away on its level builds one (facing us), and only one.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("bot_SnowmanUse"), 6.f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(600.f, { EPenguinAbility::Snowball });
				if (Target.IsValid() && Target->GetBot())
				{
					Target->SetTeam(-1);
					Penguin->SetTeam(-1);
					SavedTargetAbilities = Target->GetAbilities()->GetAbilities();
					Target->GetAbilities()->SetAbilities({ EPenguinAbility::Snowman });
					Target->GetBot()->SetBotEnabled(true, 2);
				}
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				Teleport(FVector(Location().X, 0.f, 62.f)); // (we stay put)
				if (const APinguSnowman* Snowman = APinguSnowman::FindOf(GetWorld(), Target.Get()))
				{
					if (R.Values[0] < 0.f)
					{
						R.Values[0] = T;
						R.Values[1] = static_cast<float>(Location().X - Snowman->GetFeet().X);
					}
				}
				TArray<APinguSnowman*> All;
				APinguSnowman::GetAll(GetWorld(), All);
				R.Counts[0] = FMath::Max(R.Counts[0], All.Num());
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				if (Target.IsValid() && Target->GetBot())
				{
					Target->GetBot()->SetBotEnabled(false);
				}
				const bool bOk = R.Values[0] > 0.f && R.Counts[0] == 1;
				Report(FString::Printf(TEXT("RESULT bot snowman use: built one after %.1f s, %.0f cm from us, snowmen at most %d (want 1): %s"), R.Values[0], R.Values[1], R.Counts[0], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_Rays: the grow ray makes it bigger (x1.297, its snowballs with it), the shrink ray smaller; past half its size it
	// melts away.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_Rays"), 3.f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
				Run->Snowman = BuildFor(Penguin.Get(), 1.f);
				Teleport(Location() + FVector(-300.f, 0.f, 0.f));
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				APinguSnowman* Snowman = R.Snowman.Get();
				static const float FireAt[] = { 0.4f, 0.9f, 1.4f, 1.9f, 2.4f };
				static const bool bGrow[] = { true, false, false, false, false };
				if (Snowman && R.Counts[0] < 5 && T > FireAt[R.Counts[0]])
				{
					R.Values[R.Counts[0]] = Snowman->GetSize(); // (its size before each ray)
					const FVector Mid = Snowman->GetFeet() + FVector(0.f, 0.f, 80.f * Snowman->GetSize());
					const FVector From = Mid - FVector(120.f, 0.f, 0.f);
					APenguinProjectile::Launch(GetWorld(), bGrow[R.Counts[0]] ? EPenguinProjectileKind::GrowRay : EPenguinProjectileKind::ShrinkRay, Target.IsValid() ? Target.Get() : Penguin.Get(), From,
						FVector(2300.f, 0.f, 0.f));
					++R.Counts[0];
				}
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				// 1 -> 1.297 (grow) -> 0.917 -> 0.648 -> 0.458: melted at the 4th shrink.
				const bool bOk = FMath::IsNearlyEqual(R.Values[1], PinguGrowRayFactor, 0.01f) && FMath::IsNearlyEqual(R.Values[2], PinguGrowRayFactor * PinguShrinkRayFactor, 0.01f)
					&& R.bGone && R.End == TEXT("shrunk away");
				Report(FString::Printf(TEXT("RESULT snowman rays: its size 1 -> %.3f (grow) -> %.3f -> %.3f -> %.3f (shrinks), then %s (want melted past %.2f): %s"),
					R.Values[1], R.Values[2], R.Values[3], R.Values[4], R.bGone ? *R.End : TEXT("still there"), SnowmanMinSize, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_OwnShots (2026-11-05, Linus: "cant hit ur own snow mans with projectiles"): a snowball, icicle, ice bolt and the
	// grow and shrink rays of its own builder fly through it: it stands on, unchanged in size.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_OwnShots"), 3.f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
				Run->Snowman = BuildFor(Penguin.Get(), 1.f);
				Teleport(Location() + FVector(-300.f, 0.f, 0.f));
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				APinguSnowman* Snowman = R.Snowman.Get();
				static const EPenguinProjectileKind Kinds[] = { EPenguinProjectileKind::Snowball, EPenguinProjectileKind::Icicle, EPenguinProjectileKind::IceBolt,
					EPenguinProjectileKind::GrowRay, EPenguinProjectileKind::ShrinkRay };
				if (Snowman && R.Counts[0] < 5 && T > 0.3f + 0.4f * R.Counts[0])
				{
					const FVector Mid = Snowman->GetFeet() + FVector(0.f, 0.f, 80.f);
					const FVector From = Mid - FVector(150.f, 0.f, 0.f);
					APenguinProjectile::Launch(GetWorld(), Kinds[R.Counts[0]], Penguin.Get(), From, FVector(2300.f, 0.f, 0.f));
					++R.Counts[0];
				}
				if (!Snowman && R.Counts[0] > 0 && !R.bGone)
				{
					R.bGone = true;
					R.End = APinguSnowman::LastEnd;
				}
				if (Snowman)
				{
					R.Values[0] = Snowman->GetSize();
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Counts[0] == 5 && !R.bGone && R.Snowman.IsValid() && FMath::IsNearlyEqual(R.Values[0], 1.f, 0.001f);
				Report(FString::Printf(TEXT("RESULT snowman own shots: %d of 5 own shots (snowball, icicle, ice bolt, grow ray, shrink ray) flew through it, broken %d (%s; want 0), its size %.3f (want 1): %s"),
					R.Counts[0], R.bGone ? 1 : 0, *R.End, R.Values[0], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_OwnBombBody: an own bomb's flying body passes through the snowman (no blast on it, no break).
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		const TSharedRef<TWeakObjectPtr<APenguinProjectile>> Bomb = MakeShared<TWeakObjectPtr<APenguinProjectile>>();
		AddStep(TEXT("snowman_OwnBombBody"), 0.6f,
			[this, Run, Bomb]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
				Run->Snowman = BuildFor(Penguin.Get(), 1.f);
				Teleport(Location() + FVector(-300.f, 0.f, 0.f));
				if (APinguSnowman* Snowman = Run->Snowman.Get())
				{
					*Bomb = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin.Get(), Snowman->GetFeet() + FVector(-200.f, 0.f, 90.f), FVector(2000.f, 0.f, 0.f));
				}
			},
			[Run, Bomb](float T)
			{
				static_cast<void>(T);
				FRun& R = *Run;
				if (const APenguinProjectile* Flying = Bomb->Get())
				{
					R.Values[0] = FMath::Max(R.Values[0], static_cast<float>(Flying->GetActorLocation().X));
				}
				if (R.Snowman.IsValid())
				{
					R.Values[1] = static_cast<float>(R.Snowman->GetFeet().X);
				}
				else if (R.Values[1] > -1.f && !R.bGone)
				{
					R.bGone = true;
					R.End = APinguSnowman::LastEnd;
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = !R.bGone && R.Snowman.IsValid() && R.Values[0] > R.Values[1] + 40.f;
				Report(FString::Printf(TEXT("RESULT snowman own bomb body: the bomb flew to x %.0f past the snowman at x %.0f, broken %d (%s; want 0): %s"),
					R.Values[0], R.Values[1], R.bGone ? 1 : 0, *R.End, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_OwnMany: its builder's snowmen don't break each other: two in a row, the first one's balls fly on through the
	// second one toward the dummy.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		const TSharedRef<TWeakObjectPtr<APinguSnowman>> Second = MakeShared<TWeakObjectPtr<APinguSnowman>>();
		AddStep(TEXT("snowman_OwnMany"), 3.6f,
			[this, Run, Second]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(680.f, { EPenguinAbility::Snowball });
				Run->Snowman = BuildFor(Penguin.Get(), 1.f);
				*Second = BuildFor(Penguin.Get(), 1.f); // (one tile further, in the first one's line of fire)
			},
			[this, Run, Second](float T)
			{
				static_cast<void>(T);
				FRun& R = *Run;
				if (Target.IsValid() && Target->GetPenguinMovement() && Target->GetPenguinMovement()->IsMovingOnGround()
					&& FMath::Abs(Target->GetActorLocation().X - (Location().X + 680.f)) > 60.f)
				{
					Target->ResetForRound(Location() + FVector(680.f, 0.f, 0.f));
				}
				if (const APinguSnowman* First = R.Snowman.Get())
				{
					R.Counts[0] = First->GetThrows();
				}
				R.Flags[0] |= R.Snowman.IsValid() && Second->IsValid();
				R.Counts[1] = (R.Snowman.IsValid() ? 1 : 0) + (Second->IsValid() ? 1 : 0);
				if (R.Flags[0] && R.Counts[1] < 2 && !R.bGone)
				{
					R.bGone = true;
					R.End = APinguSnowman::LastEnd;
				}
			},
			[this, Run, Second, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Flags[0] && R.Counts[1] == 2 && !R.bGone && R.Counts[0] >= 2 && Second->IsValid() && R.Snowman.IsValid()
					&& Second->Get()->GetFeet().X > R.Snowman->GetFeet().X + 50.f;
				Report(FString::Printf(TEXT("RESULT snowman own many: two standing %d, still %d at the end, the first one threw %d times through the second, broken %d (%s; want 0): %s"),
					R.Flags[0] ? 1 : 0, R.Counts[1], R.Counts[0], R.bGone ? 1 : 0, *R.End, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_OwnBreakNoot / _OwnBreakBomb: the area things still break the owner's own snowman (the noot; a bomb's blast).
	for (int32 Variant = 0; Variant < 2; ++Variant)
	{
		const bool bBomb = Variant == 1;
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(bBomb ? TEXT("snowman_OwnBreakBomb") : TEXT("snowman_OwnBreakNoot"), 2.f,
			[this, Run, bBomb]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { bBomb ? EPenguinAbility::Snowball : EPenguinAbility::Noot });
				Run->Snowman = BuildFor(Penguin.Get(), 1.f);
			},
			[this, Run, bBomb](float T)
			{
				FRun& R = *Run;
				APinguSnowman* Snowman = R.Snowman.Get();
				if (Snowman && !R.Flags[0] && T > 0.6f)
				{
					R.Flags[0] = true;
					if (bBomb)
					{
						if (APenguinProjectile* Bomb = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin.Get(), Snowman->GetFeet() + FVector(60.f, 0.f, 30.f), FVector::ZeroVector))
						{
							Bomb->Detonate(0.f);
						}
						Teleport(Location() + FVector(-500.f, 0.f, 0.f)); // (out of the blast)
					}
					else
					{
						Penguin->SetAimPoint(Snowman->GetFeet() + FVector(0.f, 0.f, 80.f));
						R.Flags[1] = Penguin->ActivateAbility(0);
					}
				}
				Watch(R, T);
			},
			[this, Run, bBomb, CleanUp]
			{
				const FRun& R = *Run;
				const TCHAR* Want = bBomb ? TEXT("snow bomb") : TEXT("noot");
				const bool bOk = R.bGone && R.End == Want && (bBomb || R.Flags[1]);
				Report(FString::Printf(TEXT("RESULT snowman own %s: its builder's own snowman broken %d (%s; want %s): %s"), bBomb ? TEXT("bomb blast") : TEXT("noot"),
					R.bGone ? 1 : 0, *R.End, Want, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- snowman_TeamShot: a teammate's snowball breaks the snowman like an enemy's (only its builder's own shots pass).
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("snowman_TeamShot"), 2.4f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(250.f, { EPenguinAbility::Snowball });
				if (Target.IsValid())
				{
					Penguin->SetTeam(2);
					Target->SetTeam(2);
					SavedTargetAbilities = Target->GetAbilities()->GetAbilities();
					Run->Snowman = BuildFor(Target.Get(), -1.f);
					Target->ResetForRound(Location() + FVector(1600.f, 0.f, 0.f));
				}
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				APinguSnowman* Snowman = R.Snowman.Get();
				if (Snowman && !R.Flags[0] && T > 0.6f)
				{
					R.Flags[0] = true;
					const FVector Mid = Snowman->GetFeet() + FVector(0.f, 0.f, 80.f);
					const FVector From = Penguin->GetAimOrigin() + FVector(40.f, 0.f, 0.f);
					APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Penguin.Get(), From, (Mid - From).GetSafeNormal() * 2100.f);
				}
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.bGone && R.End == TEXT("snowball");
				Report(FString::Printf(TEXT("RESULT snowman team shot: a teammate's snowball broke it %d (%s; want snowball): %s"), R.bGone ? 1 : 0, *R.End, Ok(bOk)));
				CleanUp();
			});
	}
}
