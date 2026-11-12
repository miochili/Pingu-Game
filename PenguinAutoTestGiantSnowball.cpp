// Pingu - the giant snowball (APinguGiantSnowball, the 17th ability, 2026-10-29). The autotest's steps
// (APenguinAutoTest::AddGiantSnowballSteps; filter giant_ + spawn_target, the bots' bot_Giant, the pictures look_Giant):
//
//  giant_Build          the ability used (no key): the penguin stands still 1.5 s (walking does nothing), the ball grows to 1
//                       tile and rolls off by itself toward the aim; the cooldown runs from the launch.
//  look_GiantBuild      the roll-up up close (81_giant_build_1..4) and the HUD icon over the head (81_giant_hud_icon).
//  look_GiantSnowball   the lumpy ball and what pokes out of it (3 penguins, snowballs, an icicle, a bomb, a bubble) close and far,
//                       3 tiles, a roll strip, the GPU with / without an 8-tile ball, frozen (86_giant_look_*).
//  giant_Cancel         the key let go after 0.6 s: it crumbles, the full cooldown runs, the penguin moves again.
//  giant_HoldKey        the key held past 1.5 s: it rolls off by itself; the late release does nothing.
//  giant_RollGrowSea    rolled the length of the strip: its speed, 1 -> 2 -> 3 tiles by distance and never past 3, off the
//                       end into the sea (82_giant_roll_1tile / _2tiles / _3tiles).
//  giant_JumpOver       a 3-tile ball rolling at our penguin: a real jump + double jump clears it (not swallowed).
//  giant_Bounce         between two walls: back from the 1st and 2nd, broken by the 3rd; it swallows its own roller on the
//                       way back, who pops out where it broke (83_giant_bounce).
//  giant_StepUp(Big)    a 1-tile and a 3-tile ball turn back from a one-block step up (a wall hit; it never rolls uphill).
//  giant_StepDown       one rolled off a one-block platform drops down and rolls on (87_giant_step_up).
//  giant_SwallowRelease the dummy swallowed (hidden, no collision, hanging still, drawn inside), let out where it broke
//                       (84_giant_inside).
//  giant_SeaInside      the dummy swallowed, rolled off the edge: it is out (sea), our knock-out.
//  giant_SnowballSizes  a normal, a big (x2) and a small (x0.5) snowball grow it by 30, 60 and 15 cm.
//  giant_PastCap        at 3 tiles a snowball still grows it; the safety cap (8 tiles) holds.
//  giant_Rays           the grow ray x1.297, the shrink ray x0.707; shrunk to the smallest (half a tile) its penguin stays in.
//  giant_IceGun         an ice bolt freezes it: faster, it shoves the dummy instead of swallowing it (85_giant_frozen).
//  giant_Frost          the frost breath freezes it.
//  giant_BombSwallow    a bomb thrown straight into it and an ability bubble in its way are swallowed and come out when it
//                       breaks.
//  giant_BombBlast      a bomb going off next to it breaks it.
//  giant_Noot / giant_Slap / giant_Stomp / giant_Iceberg / giant_HotFeet   each breaks it.
//  giant_IceTouch       an Ice Touch penguin freezes it and is shoved, not swallowed.
//  giant_Shell          a penguin in its ice shell is pushed aside, not swallowed.
//  giant_Merge          two giant snowballs meet: the bigger takes the other in.
//  bot_GiantDodge       a bot jumps over one rolling at it.
//  bot_GiantUse         a bot with the ability rolls one at its target.

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
#include "PinguPickup.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "DynamicRHI.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"

namespace PenguinAutoTestGiantPrivate
{
	using namespace PinguTuning;

	/** One step's state. */
	struct FRun
	{
		TWeakObjectPtr<APinguGiantSnowball> Ball;
		bool bUsed = false;
		bool bLaunched = false;
		bool bGone = false;
		bool bSeen = false;
		float UseAt = -1.f;
		float LaunchAt = -1.f;
		float GoneAt = -1.f;
		FString End;
		FVector EndAt = FVector::ZeroVector;
		float X0 = 0.f;
		float Drift = 0.f;
		bool bHoverSeen = false;
		float FirstSize = -1.f;
		float LaunchSize = -1.f;
		float Cooldown = -1.f;
		float Values[8] = { -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f };
		int32 Counts[4] = { 0, 0, 0, 0 };
		bool Flags[8] = { false, false, false, false, false, false, false, false };
		int32 Shots = 0;
	};

	/** The giant snowball in the world (the first one), or null. */
	static APinguGiantSnowball* FindBall(const UWorld* World)
	{
		TArray<APinguGiantSnowball*> All;
		APinguGiantSnowball::GetAll(World, All);
		return All.Num() > 0 ? All[0] : nullptr;
	}

	static int32 CountBalls(const UWorld* World)
	{
		TArray<APinguGiantSnowball*> All;
		APinguGiantSnowball::GetAll(World, All);
		return All.Num();
	}

	/** A ball rolled by Roller toward Side, rolling at once (the roll-up skipped). */
	static APinguGiantSnowball* RollNow(APenguinCharacter* Roller, float Side)
	{
		APinguGiantSnowball* Ball = APinguGiantSnowball::BeginRollUp(Roller, 0, Side);
		if (Ball)
		{
			Ball->LaunchForTest();
		}
		return Ball;
	}

	/** Watches the ball: notes when it went and why. */
	static void Watch(FRun& R, float T)
	{
		if (R.Ball.IsValid())
		{
			R.bSeen = true;
			return;
		}
		if (R.bSeen && !R.bGone)
		{
			R.bGone = true;
			R.GoneAt = T;
			R.End = APinguGiantSnowball::LastEnd;
			R.EndAt = APinguGiantSnowball::LastEndAt;
		}
	}

	static const TCHAR* Ok(bool b) { return b ? TEXT("ok") : TEXT("FAIL"); }

	/** The flat strip at At, the top block of the floor under it and that floor's top (z). Null if there is none. */
	static ABlockIsland* FloorUnder(UWorld* World, const FVector& At, FIntPoint& OutFloor, float& OutFloorZ)
	{
		ABlockIsland* Flat = ABlockIsland::FindNear(World, At);
		if (!Flat || !Flat->FindGroundBelow(FVector(At.X, 0.f, At.Z + 300.f), 2000.f, OutFloorZ))
		{
			return nullptr;
		}
		OutFloor = Flat->WorldToCell(FVector(At.X, 0.f, OutFloorZ - 1.f));
		return Flat;
	}

	static void SetTrait(APenguinCharacter* Who, EPinguTrait Trait)
	{
		if (UPenguinTraitComponent* Traits = Who ? Who->GetTraits() : nullptr)
		{
			Traits->SetTrait(Trait, -1.f);
		}
	}
}

void APenguinAutoTest::AddGiantSnowballSteps()
{
	using namespace PenguinAutoTestGiantPrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	// Everything back as it was after a step (the dummy too).
	const auto CleanUp = [this, Base]()
	{
		VirtualInput = FVector2D::ZeroVector;
		ClearAbilityLeftovers();
		Penguin->CancelAbilityKeys();
		SetTrait(Penguin.Get(), EPinguTrait::None);
		Penguin->ResetForRound(Penguin->GetActorLocation());
		if (Target.IsValid())
		{
			if (Target->GetBot())
			{
				Target->GetBot()->SetBotEnabled(false);
			}
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
	// The stage camera held for pictures (no practice respawn either).
	const auto HoldCamera = [this]()
	{
		if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
		{
			Stage->SetActorTickEnabled(false);
			LookCamera = Stage;
		}
		if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
		{
			Mode->SetActorTickEnabled(false);
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

	// ---- giant_Build: the ability used (as bots and tests use it: no key): 1.5 s rolling it up, standing still (walking
	// does nothing), then it rolls off by itself; the cooldown runs from the launch.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_Build"), 2.4f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::GiantSnowball });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
				// Walking left all along: nothing while rolling it up, away from the ball after.
				VirtualInput = FVector2D(-1.f, 0.f);
				if (!R.bUsed && T > 0.2f)
				{
					R.bUsed = Penguin->ActivateAbility(0);
					R.UseAt = T;
					R.X0 = static_cast<float>(Penguin->GetActorLocation().X);
					R.Ball = FindBall(GetWorld());
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (Ball && Ball->IsBuilding())
				{
					R.Drift = FMath::Max(R.Drift, FMath::Abs(static_cast<float>(Penguin->GetActorLocation().X) - R.X0));
					R.bHoverSeen |= Move->IsHovering();
					if (R.FirstSize < 0.f)
					{
						R.FirstSize = Ball->GetDiameter();
					}
				}
				if (Ball && Ball->IsRolling() && !R.bLaunched)
				{
					R.bLaunched = true;
					R.LaunchAt = T - R.UseAt;
					R.LaunchSize = Ball->GetDiameter();
					R.Cooldown = Penguin->GetAbilities()->GetCooldownRemaining(0);
					R.Values[0] = Ball->GetDirection();
					R.Values[1] = static_cast<float>(Ball->GetActorLocation().X);
				}
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const float Walked = R.X0 - static_cast<float>(Penguin->GetActorLocation().X);
				const bool bOk = R.bUsed && R.LaunchAt > 1.45f && R.LaunchAt < 1.6f && R.Drift < 3.f && R.bHoverSeen && R.FirstSize > 10.f && R.FirstSize < 40.f
					&& FMath::IsNearlyEqual(R.LaunchSize, GiantStartSize, 1.f) && R.Cooldown > 7.8f && R.Cooldown < 8.05f && R.Values[0] > 0.f
					&& R.Values[1] > R.X0 && Walked > 50.f;
				Report(FString::Printf(TEXT("RESULT giant build: used %d, rolled off %.2f s after the use (want 1.5), drifted %.1f cm while rolling it up (want 0), hanging still %d, %.0f cm at the start -> %.0f at the launch (want about 18 -> 100), cooldown %.2f s at the launch (want 8), rolls %s, walked %.0f cm away after: %s"),
					R.bUsed ? 1 : 0, R.LaunchAt, R.Drift, R.bHoverSeen ? 1 : 0, R.FirstSize, R.LaunchSize, R.Cooldown, R.Values[0] > 0.f ? TEXT("right (the aim's side)") : TEXT("LEFT"), Walked, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- look_GiantBuild: the roll-up up close, four pictures; and the HUD's icon over the head first.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("look_GiantBuild"), 2.6f,
			[this, Run, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::GiantSnowball, EPenguinAbility::Snowball, EPenguinAbility::Noot });
				HoldCamera();
			},
			[this, Run, LookAt](float T)
			{
				FRun& R = *Run;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
				LookAt(Penguin->GetActorLocation() + FVector(80.f, 0.f, -10.f), T < 0.3f ? 700.f : 420.f, 40.f);
				if (R.Shots == 0 && T > 0.2f)
				{
					++R.Shots;
					Shot(TEXT("81_giant_hud_icon"), true);
				}
				if (!R.bUsed && T > 0.35f)
				{
					R.bUsed = Penguin->ActivateAbility(0);
					R.UseAt = T;
				}
				static const float Times[] = { 0.3f, 0.7f, 1.1f, 1.48f };
				if (R.bUsed && R.Shots >= 1 && R.Shots <= 4 && T - R.UseAt > Times[R.Shots - 1])
				{
					Shot(*FString::Printf(TEXT("81_giant_build_%d"), R.Shots));
					++R.Shots;
				}
			},
			[this, Run, CleanUp]
			{
				Report(FString::Printf(TEXT("RESULT giant build look: used %d, %d of 5 pictures: %s"), Run->bUsed ? 1 : 0, Run->Shots, Ok(Run->bUsed && Run->Shots == 5)));
				CleanUp();
			});
	}

	// ---- look_GiantSnowball (2026-10-29, Linus: "give it a lumpy snow surface", "with the stuff lokiign out"): the lumpy
	// packed-snow ball up close at 1 tile; three penguins, two snowballs, an icicle, a bomb and an ability bubble swallowed and
	// poking out of it (close and at match distance); 3 tiles close; a roll strip at match distance; the GPU with and without an
	// 8-tile ball in view; frozen. Shots 86_giant_look_*.
	{
		struct FLook
		{
			TWeakObjectPtr<APinguGiantSnowball> Ball;
			TArray<TWeakObjectPtr<APenguinCharacter>> Extras;
			int32 Shots = 0;
			int32 Fired = 0;
			float GpuWith = 0.f;
			int32 FramesWith = 0;
			float GpuWithout = 0.f;
			int32 FramesWithout = 0;
			int32 Vertices = 0;
			float LumpOut = 0.f;
			float LumpIn = 0.f;
			int32 ShownInside = 0;
			int32 ShownStuck = 0;
			bool bHidden = false;
		};
		const TSharedRef<FLook> L = MakeShared<FLook>();
		AddStep(TEXT("look_GiantSnowball"), 10.4f,
			[this, L, HoldCamera]
			{
				*L = FLook();
				SpawnTarget();
				PrepareBrawl(250.f, { EPenguinAbility::Snowball });
				// Two more penguins in its way (other hats: who is inside shows).
				for (int32 Index = 0; Index < 2; ++Index)
				{
					FActorSpawnParameters Params;
					Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					const FVector At = Location() + FVector(450.f + 200.f * Index, 0.f, 0.f);
					if (APenguinCharacter* Extra = GetWorld()->SpawnActor<APenguinCharacter>(Penguin->GetClass(), At, FRotator::ZeroRotator, Params))
					{
						Extra->SetPlayerSlot(3 + Index, Index == 0 ? TEXT("Dummy 3") : TEXT("Dummy 4"));
						Extra->ResetForRound(At);
						Extra->SetInputLocked(false);
						L->Extras.Add(Extra);
					}
				}
				HoldCamera();
			},
			[this, L, LookAt](float T)
			{
				FLook& R = *L;
				if (!R.Ball.IsValid() && R.Shots == 0 && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					APinguPickup::Spawn(GetWorld(), EPenguinAbility::Noot, Location() + FVector(900.f, 0.f, 60.f));
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (!Ball)
				{
					return;
				}
				const FVector C = Ball->GetActorLocation();
				// Shots into it from behind: two snowballs, an icicle, a bomb.
				static const float FireAt[] = { 0.5f, 0.8f, 1.1f, 1.4f };
				static const EPenguinProjectileKind FireKind[] = { EPenguinProjectileKind::Snowball, EPenguinProjectileKind::Snowball,
					EPenguinProjectileKind::Icicle, EPenguinProjectileKind::Bomb };
				if (R.Fired < 4 && T > FireAt[R.Fired])
				{
					const FVector From = C - FVector(Ball->GetRadius() + 50.f, 0.f, 0.f);
					APenguinProjectile::Launch(GetWorld(), FireKind[R.Fired], Penguin.Get(), From, FVector(1200.f, 0.f, 0.f));
					++R.Fired;
				}
				const auto Take = [this, &R](const TCHAR* Name)
				{
					Shot(Name);
					++R.Shots;
				};
				if (T < 4.2f)
				{
					LookAt(C, T < 3.4f ? 420.f + Ball->GetDiameter() * 1.5f : (T < 3.95f ? 500.f + Ball->GetDiameter() * 2.5f : 2300.f), T < 3.95f ? 40.f : 160.f);
				}
				if (R.Shots == 0 && T > 0.35f) { Take(TEXT("86_giant_look_1tile")); }
				if (R.Shots == 1 && T > 3.8f)
				{
					R.ShownInside = Ball->GetShownInside();
					R.ShownStuck = Ball->GetShownStuck();
					Take(TEXT("86_giant_look_stuff_close"));
				}
				if (R.Shots == 2 && T > 4.1f) { Take(TEXT("86_giant_look_stuff_far")); }
				if (R.Shots == 3 && T > 4.2f)
				{
					Ball->SetSizeForTest(PinguTuning::GiantRollMaxSize);
					++R.Shots;
				}
				if (R.Shots >= 4 && T >= 4.2f && T < 6.f)
				{
					LookAt(C, T < 4.9f ? 1000.f : 2300.f, T < 4.9f ? 60.f : 200.f);
				}
				if (R.Shots == 4 && T > 4.7f) { Take(TEXT("86_giant_look_3tiles")); }
				if (R.Shots == 5 && T > 5.f) { Take(TEXT("86_giant_look_roll_1")); }
				if (R.Shots == 6 && T > 5.4f) { Take(TEXT("86_giant_look_roll_2")); }
				if (R.Shots == 7 && T > 5.8f) { Take(TEXT("86_giant_look_roll_3")); }
				// The cost: an 8-tile ball with everything in it in view, then the same with the ball hidden.
				if (R.Shots == 8 && T > 6.f)
				{
					Ball->SetSizeForTest(PinguTuning::GiantSafetyMaxSize);
					R.Vertices = Ball->GetMeshVertices();
					R.LumpOut = Ball->GetLumpOut();
					R.LumpIn = Ball->GetLumpIn();
					++R.Shots;
				}
				if (T >= 6.f && T < 8.6f)
				{
					LookAt(C, 3200.f, 300.f);
				}
				if (T > 6.4f && T < 7.4f)
				{
					R.GpuWith += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
					++R.FramesWith;
				}
				if (T > 7.4f && !R.bHidden)
				{
					R.bHidden = true;
					Ball->SetActorHiddenInGame(true);
				}
				if (T > 7.6f && T < 8.6f)
				{
					R.GpuWithout += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
					++R.FramesWithout;
				}
				if (R.Shots == 9 && T > 8.6f)
				{
					Ball->SetActorHiddenInGame(false);
					Ball->SetSizeForTest(150.f);
					Ball->Freeze(TEXT("test"));
					++R.Shots;
				}
				if (T >= 8.6f)
				{
					LookAt(C, 700.f, 50.f);
				}
				if (R.Shots == 10 && T > 9.4f) { Take(TEXT("86_giant_look_frozen")); }
			},
			[this, L, CleanUp]
			{
				const FLook& R = *L;
				const float With = R.FramesWith > 0 ? R.GpuWith / R.FramesWith : -1.f;
				const float Without = R.FramesWithout > 0 ? R.GpuWithout / R.FramesWithout : -1.f;
				const bool bOk = R.Shots == 11 && R.Vertices > 600 && R.LumpOut > 0.04f && R.LumpIn > 0.01f && R.ShownInside == 3 && R.ShownStuck >= 4;
				Report(FString::Printf(TEXT("RESULT giant look: %d of 11 pictures, lumpy mesh %d vertices (lumps %.0f %% out, %.0f %% in), poking out: %d penguins (want 3), %d other things (want 4 or more: snowballs, an icicle, a bomb, a bubble): %s"),
					R.Shots, R.Vertices, R.LumpOut * 100.f, R.LumpIn * 100.f, R.ShownInside, R.ShownStuck, Ok(bOk)));
				Report(FString::Printf(TEXT("RESULT giant look gpu: %.2f ms with an 8-tile ball and its stuff in view, %.2f ms without (%d / %d frames; a measurement)"),
					With, Without, R.FramesWith, R.FramesWithout));
				for (const TWeakObjectPtr<APenguinCharacter>& Extra : R.Extras)
				{
					if (Extra.IsValid())
					{
						Extra->Destroy();
					}
				}
				APinguPickup::DestroyAll(GetWorld());
				CleanUp();
			});
	}

	// ---- giant_Cancel: the key let go after 0.6 s: it crumbles, the full cooldown runs, the penguin moves again.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_Cancel"), 2.f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::GiantSnowball });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
				if (!R.bUsed && T > 0.2f)
				{
					R.bUsed = true;
					R.UseAt = T;
					Penguin->SetAbilityKeyDown(0, true);
				}
				if (R.bUsed && !R.Flags[0] && T > 0.5f)
				{
					R.Flags[0] = true;
					R.Ball = FindBall(GetWorld());
					R.Flags[1] = R.Ball.IsValid() && R.Ball->IsBuilding();
				}
				if (R.Flags[0] && !R.Flags[2] && T > 0.8f)
				{
					R.Flags[2] = true;
					Penguin->SetAbilityKeyDown(0, false);
				}
				if (R.Flags[2] && !R.Flags[3] && T > 0.85f)
				{
					R.Flags[3] = true;
					R.Cooldown = Penguin->GetAbilities()->GetCooldownRemaining(0);
					R.Values[0] = static_cast<float>(Penguin->GetActorLocation().X);
				}
				Watch(R, T);
				if (R.Flags[3] && T > 1.2f)
				{
					VirtualInput = FVector2D(-1.f, 0.f);
					R.Flags[4] |= Move->IsHovering();
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const float Walked = R.Values[0] - static_cast<float>(Penguin->GetActorLocation().X);
				const bool bReady = Penguin->GetAbilities()->IsReady(0);
				const bool bOk = R.Flags[1] && R.bGone && R.End == TEXT("let go") && CountBalls(GetWorld()) == 0 && R.Cooldown > 7.8f && R.Cooldown < 8.05f
					&& !bReady && !R.Flags[4] && Walked > 50.f;
				Report(FString::Printf(TEXT("RESULT giant cancel: rolling up %d, after the release gone %d (%s), cooldown %.2f s (want 8: it counts as used), ready %d (want 0), walked %.0f cm after: %s"),
					R.Flags[1] ? 1 : 0, R.bGone ? 1 : 0, *R.End, R.Cooldown, bReady ? 1 : 0, Walked, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_HoldKey: the key held past the roll-up: it rolls off by itself at 1.5 s; the late release does nothing.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_HoldKey"), 2.6f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::GiantSnowball });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(-400.f, 0.f, 0.f)); // (to the left this time)
				if (!R.bUsed && T > 0.2f)
				{
					R.bUsed = true;
					R.UseAt = T;
					Penguin->SetAbilityKeyDown(0, true);
				}
				if (R.bUsed && !R.Ball.IsValid() && !R.bLaunched)
				{
					R.Ball = FindBall(GetWorld());
				}
				if (R.Ball.IsValid() && R.Ball->IsRolling() && !R.bLaunched)
				{
					R.bLaunched = true;
					R.LaunchAt = T - R.UseAt;
					R.Values[0] = R.Ball->GetDirection();
				}
				if (R.bUsed && !R.Flags[0] && T > 2.1f)
				{
					R.Flags[0] = true;
					Penguin->SetAbilityKeyDown(0, false);
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bStill = R.Ball.IsValid() && R.Ball->IsRolling();
				const bool bOk = R.bLaunched && R.LaunchAt > 1.45f && R.LaunchAt < 1.6f && bStill && R.Values[0] < 0.f;
				Report(FString::Printf(TEXT("RESULT giant hold key: rolled off %.2f s after the press (want 1.5), to the %s, still rolling after the late release %d: %s"),
					R.LaunchAt, R.Values[0] < 0.f ? TEXT("left (the aim's side)") : TEXT("RIGHT"), bStill ? 1 : 0, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_RollGrowSea: from the strip's left end to its right end and off it: speed, growth by distance, the 3-tile cap,
	// the sea. Pictures at 1, 2 and 3 tiles.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_RollGrowSea"), 26.f,
			[this, Run, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::GiantSnowball });
				Teleport(FVector(FlatIslandX - 2800.f, 0.f, 62.f));
				ResetTarget(FVector(FlatIslandX - 2960.f, 0.f, 62.f)); // (behind us: out of its way)
				Penguin->ResetForRound(Penguin->GetActorLocation());
				Penguin->SetInputLocked(false);
				Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::GiantSnowball });
				HoldCamera();
			},
			[this, Run, LookAt](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					R.UseAt = T;
					R.X0 = R.Ball.IsValid() ? static_cast<float>(R.Ball->GetActorLocation().X) : 0.f;
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (Ball)
				{
					const float D = Ball->GetDiameter();
					const float Rolled = Ball->GetRolled();
					LookAt(Ball->GetActorLocation(), 700.f + D * 2.f, 60.f);
					// (Values: 0 the size after 10 m, 1 after 20 m, 2 the most it ever had, 3 / 4 X at 2 s / 6 s for the speed)
					if (R.Values[0] < 0.f && Rolled >= 1000.f) { R.Values[0] = Ball->GetTargetDiameter(); }
					if (R.Values[1] < 0.f && Rolled >= 2000.f) { R.Values[1] = Ball->GetTargetDiameter(); }
					R.Values[2] = FMath::Max(R.Values[2], Ball->GetTargetDiameter());
					if (R.Values[3] < 0.f && T - R.UseAt > 2.f) { R.Values[3] = static_cast<float>(Ball->GetActorLocation().X); }
					if (R.Values[4] < 0.f && T - R.UseAt > 6.f) { R.Values[4] = static_cast<float>(Ball->GetActorLocation().X); }
					if (R.Shots == 0 && T - R.UseAt > 0.4f) { Shot(TEXT("82_giant_roll_1tile")); ++R.Shots; }
					if (R.Shots == 1 && D >= 199.f) { Shot(TEXT("82_giant_roll_2tiles")); ++R.Shots; }
					if (R.Shots == 2 && D >= 299.f) { Shot(TEXT("82_giant_roll_3tiles")); ++R.Shots; }
				}
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const float Speed = R.Values[4] > 0.f && R.Values[3] > 0.f ? (R.Values[4] - R.Values[3]) / 4.f : -1.f;
				const bool bOk = R.bUsed && FMath::Abs(Speed - GiantRollSpeed) < 10.f && FMath::Abs(R.Values[0] - 200.f) < 6.f
					&& FMath::Abs(R.Values[1] - GiantRollMaxSize) < 2.f && R.Values[2] <= GiantRollMaxSize + 0.5f && R.bGone && R.End == TEXT("sea")
					&& R.Shots == 3 && Target->IsAlive();
				Report(FString::Printf(TEXT("RESULT giant roll: %.0f cm/s (want %.0f), %.0f cm after 10 m (want 200), %.0f after 20 m (want %.0f), at most %.0f (want %.0f: never past 3 tiles by rolling), ended %d (%s) %.1f s after the launch at %s, %d of 3 pictures: %s"),
					Speed, GiantRollSpeed, R.Values[0], R.Values[1], GiantRollMaxSize, R.Values[2], GiantRollMaxSize, R.bGone ? 1 : 0, *R.End, R.GoneAt - R.UseAt,
					*R.EndAt.ToCompactString(), R.Shots, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_JumpOver: a 3-tile ball rolling at our penguin - a real jump and the double jump at its top clear it (Linus:
	// "never get to big that u cant jump over it normal sized").
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_JumpOver"), 4.f,
			[this, Run, Base]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(1500.f, { EPenguinAbility::Snowball });
				Run->Values[0] = 1.e9f; // the least room between our feet and its top while over it
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f && Target.IsValid())
				{
					R.Ball = RollNow(Target.Get(), -1.f);
					if (R.Ball.IsValid())
					{
						R.Ball->SetSizeForTest(GiantRollMaxSize);
					}
					R.bUsed = R.Ball.IsValid();
					R.UseAt = T;
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (!Ball)
				{
					return;
				}
				const FVector C = Ball->GetActorLocation();
				const float Rad = Ball->GetRadius();
				const FVector Feet = Penguin->GetFeetLocation();
				const float Gap = static_cast<float>((C.X - Rad) - (Feet.X + 30.f));
				// Run at it, jump when it is close, jump again at the top of the first jump.
				VirtualInput = FVector2D(1.f, 0.f);
				if (!R.Flags[0] && Gap < 200.f && Move->IsMovingOnGround())
				{
					R.Flags[0] = true;
					Penguin->PressJump();
				}
				if (R.Flags[0] && !R.Flags[1] && Move->Velocity.Z < 400.f && !Move->IsMovingOnGround())
				{
					R.Flags[1] = true;
					Penguin->ReleaseJump();
					Penguin->PressJump();
				}
				if (FMath::Abs(Feet.X - C.X) < Rad)
				{
					const float Dx = static_cast<float>(Feet.X - C.X);
					R.Values[0] = FMath::Min(R.Values[0], static_cast<float>(Feet.Z - C.Z) - FMath::Sqrt(FMath::Max(Rad * Rad - Dx * Dx, 0.f)));
				}
				R.Flags[2] |= Penguin->IsHidden() || Ball->HoldsPenguin(Penguin.Get());
				R.Values[1] = static_cast<float>(Feet.X - C.X);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				Penguin->ReleaseJump();
				const bool bOk = R.bUsed && R.Flags[0] && !R.Flags[2] && R.Values[1] > 0.f && Penguin->IsAlive();
				Report(FString::Printf(TEXT("RESULT giant jump over: a %.0f cm ball, jumped %d, double jumped %d, swallowed %d (want 0), least room between our feet and its surface below them %.0f cm, on its far side %d: %s"),
					GiantRollMaxSize, R.Flags[0] ? 1 : 0, R.Flags[1] ? 1 : 0, R.Flags[2] ? 1 : 0, R.Values[0], R.Values[1] > 0.f ? 1 : 0, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_Bounce: walls 4 m either side of the dummy that rolls it: back from the 1st and the 2nd, broken by the 3rd. On
	// its way back it swallows its own roller, who pops out where it broke.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_Bounce"), 9.f,
			[this, Run, Base, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(0.f, { EPenguinAbility::Snowball });
				Teleport(Base + FVector(1500.f, 0.f, 0.f));
				ResetTarget(Base);
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
				{
					for (const float Side : { -1.f, 1.f })
					{
						const FIntPoint Foot = Flat->WorldToCell(Base + FVector(Side * 450.f, 0.f, -12.f));
						Flat->AddBlocks({ Foot, FIntPoint(Foot.X, Foot.Y + 1), FIntPoint(Foot.X, Foot.Y + 2), FIntPoint(Foot.X, Foot.Y + 3) }, EBlockKind::Rock, 0.f);
					}
					Flat->FlushEdits();
				}
				HoldCamera();
			},
			[this, Run, LookAt, Base](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f && Target.IsValid())
				{
					R.Ball = RollNow(Target.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					R.UseAt = T;
				}
				LookAt(Base + FVector(0.f, 0.f, 80.f), 1500.f, 120.f);
				if (APinguGiantSnowball* Ball = R.Ball.Get())
				{
					R.Counts[0] = Ball->GetWallHits();
					R.Flags[0] |= Ball->HoldsPenguin(Target.Get());
					if (R.Shots == 0 && Ball->GetWallHits() >= 1)
					{
						Shot(TEXT("83_giant_bounce"));
						++R.Shots;
					}
				}
				Watch(R, T);
				if (R.bGone && !R.Flags[1] && T > R.GoneAt + 0.4f && Target.IsValid())
				{
					R.Flags[1] = true;
					R.Flags[2] = !Target->IsHidden() && Target->IsAlive() && Target->GetActorEnableCollision();
					R.Values[0] = FVector::Dist2D(FVector(Target->GetActorLocation().X, Target->GetActorLocation().Z, 0.f), FVector(R.EndAt.X, R.EndAt.Z, 0.f));
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.bUsed && R.Counts[0] == 2 && R.bGone && R.End == TEXT("third wall hit") && R.Flags[0] && R.Flags[2] && R.Values[0] < 400.f;
				Report(FString::Printf(TEXT("RESULT giant bounce: walls hit %d before the last (want 2), ended %d (%s, want the third wall hit) after %.1f s, swallowed its own roller %d, which came out %d (%.0f cm from where it broke): %s"),
					R.Counts[0], R.bGone ? 1 : 0, *R.End, R.GoneAt - R.UseAt, R.Flags[0] ? 1 : 0, R.Flags[2] ? 1 : 0, R.Values[0], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_StepUp / giant_StepUpBig / giant_StepDown: a platform one block high starts 4 m right of the roller. Since
	// 2026-11-05 (Linus: "it can go up 1 block when roling") a 1-tile and a 3-tile ball rolling at it both climb it and roll on
	// (no wall hit, its foot up one block); before (2026-10-29) any step up was a wall. One rolled off the platform's left end
	// drops down onto the floor and rolls on (no wall hit). Shot 87_giant_step_up (the 1-tile ball half way up).
	for (int32 Variant = 0; Variant < 3; ++Variant)
	{
		const TCHAR* StepName = Variant == 0 ? TEXT("giant_StepUp") : Variant == 1 ? TEXT("giant_StepUpBig") : TEXT("giant_StepDown");
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(StepName, 3.4f,
			[this, Run, Base, Variant, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(-1500.f, { EPenguinAbility::Snowball });
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
				{
					// The platform: one block on the floor from 4 m to 12 m right of Base.
					TArray<FIntPoint> Cells;
					const FIntPoint First = Flat->WorldToCell(Base + FVector(450.f, 0.f, -12.f));
					for (int32 Column = 0; Column < 8; ++Column)
					{
						Cells.Add(FIntPoint(First.X + Column, First.Y));
					}
					Flat->AddBlocks(Cells, EBlockKind::Snow, 0.f);
					Flat->FlushEdits();
				}
				if (Variant == 2)
				{
					// The roller on the platform, rolling left off its end.
					Teleport(Base + FVector(550.f, 0.f, 100.f));
					Penguin->ResetForRound(Penguin->GetActorLocation());
					Penguin->SetInputLocked(false);
				}
				HoldCamera();
			},
			[this, Run, Base, Variant, LookAt](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.15f)
				{
					R.Ball = RollNow(Penguin.Get(), Variant == 2 ? -1.f : 1.f);
					if (R.Ball.IsValid() && Variant == 1)
					{
						R.Ball->SetSizeForTest(PinguTuning::GiantRollMaxSize);
					}
					R.bUsed = R.Ball.IsValid();
					R.Values[0] = -1.e9f; // the highest its foot got (cm above the floor, z 0)
					R.Values[1] = 1.e9f;  // the lowest
				}
				LookAt(Base + FVector(300.f, 0.f, 120.f), 1300.f, 150.f);
				if (APinguGiantSnowball* Ball = R.Ball.Get())
				{
					const float Foot = static_cast<float>(Ball->GetActorLocation().Z) - Ball->GetRadius();
					if (T > 0.3f)
					{
						R.Values[0] = FMath::Max(R.Values[0], Foot);
						R.Values[1] = FMath::Min(R.Values[1], Foot);
					}
					R.Counts[0] = Ball->GetWallHits();
					R.Counts[1] = Ball->GetClimbs();
					R.Values[2] = Ball->GetDirection();
					R.Values[3] = static_cast<float>(Ball->GetActorLocation().X - Base.X);
					if (Variant == 0 && R.Shots == 0 && Ball->IsClimbing() && Foot > 45.f)
					{
						Shot(TEXT("87_giant_step_up"));
						++R.Shots;
					}
				}
				Watch(R, T);
			},
			[this, Run, Variant, StepName, CleanUp]
			{
				const FRun& R = *Run;
				bool bOk = R.bUsed && !R.bGone;
				FString Want;
				if (Variant < 2)
				{
					// Up the step and on: one climb, no wall hit, still rolling right, its foot one block up, past the step's face.
					bOk &= R.Counts[1] == 1 && R.Counts[0] == 0 && R.Values[2] > 0.f && R.Values[0] > 95.f && R.Values[0] < 106.f && R.Values[3] > 600.f;
					Want = TEXT("one climb, no wall hit, on right, its foot up to about 100");
				}
				else
				{
					// Off the platform's end: down onto the floor, rolling on left, no wall hit.
					bOk &= R.Counts[0] == 0 && R.Values[2] < 0.f && R.Values[1] < 3.f && R.Values[0] > 95.f && R.Values[3] < 300.f;
					Want = TEXT("down from the platform (foot from 100 to 0), on left, no wall hit");
				}
				Report(FString::Printf(TEXT("RESULT %s: climbs %d, wall hits %d, rolling %s, at %.0f cm, its foot %.0f .. %.0f cm above the floor (want: %s): %s"), StepName, R.Counts[1], R.Counts[0],
					R.Values[2] > 0.f ? TEXT("right") : TEXT("left"), R.Values[3], R.Values[1], R.Values[0], *Want, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_StepBounce (2026-11-05, Linus: "it can go up 1 block when roling but when it went up 1 it will bounce off the next
	// 1block higher tile and roll back down"): a staircase right of the roller (a step one block high from 3 m, a second block on
	// top from 7 m) and one block-high step 4 m to its left. The ball climbs the first, bounces off the second (one wall hit),
	// rolls back down, and - having rolled down a step - climbs the left one: 2 climbs, its foot never 2 blocks up.
	// Shots 99_giant_climb (half way up the first step), 99_giant_step_bounce (turned at the second).
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_StepBounce"), 9.f,
			[this, Run, Base, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(-1500.f, { EPenguinAbility::Snowball });
				FIntPoint Floor;
				float FloorZ = 0.f;
				if (ABlockIsland* Flat = FloorUnder(GetWorld(), Base, Floor, FloorZ))
				{
					TArray<FIntPoint> Cells;
					for (int32 Column = 3; Column <= 9; ++Column)
					{
						Cells.Add(FIntPoint(Floor.X + Column, Floor.Y + 1));
						if (Column >= 7)
						{
							Cells.Add(FIntPoint(Floor.X + Column, Floor.Y + 2));
						}
					}
					for (int32 Column = -7; Column <= -4; ++Column)
					{
						Cells.Add(FIntPoint(Floor.X + Column, Floor.Y + 1));
					}
					Flat->AddBlocks(Cells, EBlockKind::Snow, 0.f);
					Flat->FlushEdits();
				}
				Run->Values[4] = FloorZ;
				HoldCamera();
			},
			[this, Run, Base, LookAt](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.15f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					R.Values[0] = -1.e9f; // the highest its foot got above the floor
					R.Values[1] = 1.e9f;  // the lowest after the bounce
					// The roller out of its way (on the floor right of the staircase).
					Teleport(Base + FVector(1600.f, 0.f, 0.f));
				}
				LookAt(Base + FVector(250.f, 0.f, 200.f), 2500.f, 150.f);
				if (APinguGiantSnowball* Ball = R.Ball.Get())
				{
					const float Foot = static_cast<float>(Ball->GetActorLocation().Z) - Ball->GetRadius() - R.Values[4];
					if (T > 0.3f)
					{
						R.Values[0] = FMath::Max(R.Values[0], Foot);
					}
					if (Ball->GetWallHits() >= 1)
					{
						R.Values[1] = FMath::Min(R.Values[1], Foot);
					}
					R.Counts[0] = Ball->GetWallHits();
					R.Counts[1] = Ball->GetClimbs();
					R.Values[2] = Ball->GetDirection();
					R.Values[3] = static_cast<float>(Ball->GetActorLocation().X - Base.X);
					if (!R.Flags[0] && Ball->IsClimbing() && Foot > 45.f)
					{
						R.Flags[0] = true;
						Shot(TEXT("99_giant_climb"));
						++R.Shots;
					}
					if (!R.Flags[1] && Ball->GetWallHits() >= 1)
					{
						R.Flags[1] = true;
						Shot(TEXT("99_giant_step_bounce"));
						++R.Shots;
					}
				}
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.bUsed && !R.bGone && R.Counts[1] == 2 && R.Counts[0] == 1 && R.Values[0] > 95.f && R.Values[0] < 106.f
					&& R.Values[1] < 5.f && R.Values[2] < 0.f && R.Values[3] < -300.f;
				Report(FString::Printf(TEXT("RESULT giant_StepBounce: climbs %d (want 2), wall hits %d (want 1: the second step), its foot up to %.0f cm (want about 100, never 200), down to %.0f after the bounce (want 0), rolling %s at %.0f cm (want left, past the left step), gone %d: %s"),
					R.Counts[1], R.Counts[0], R.Values[0], R.Values[1], R.Values[2] > 0.f ? TEXT("right") : TEXT("left"), R.Values[3], R.bGone ? 1 : 0, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_Roof (2026-11-05, Linus: "make it so the snowball doesnt die when it grows with a roof over it but just keep the
	// same size and once hes out he grows"): a roof two blocks over the floor from 3 m to 9 m right of the roller. A ball of 1.7
	// tiles rolls under it: it grows until its top nearly touches the roof, keeps that size under it (no wall hit, never gone)
	// and grows on once it is out. Before, it grew into the roof, which then was a wall on both sides: three wall hits, broken.
	// Shots 99_giant_roof_in (under the roof), 99_giant_roof_out (out, bigger).
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_Roof"), 6.6f,
			[this, Run, Base, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(-1500.f, { EPenguinAbility::Snowball });
				FIntPoint Floor;
				float FloorZ = 0.f;
				if (ABlockIsland* Flat = FloorUnder(GetWorld(), Base, Floor, FloorZ))
				{
					TArray<FIntPoint> Cells;
					for (int32 Column = 3; Column <= 8; ++Column)
					{
						Cells.Add(FIntPoint(Floor.X + Column, Floor.Y + 3));
					}
					Flat->AddBlocks(Cells, EBlockKind::Snow, 0.f);
					Flat->FlushEdits();
					const float Cell = Flat->CellSize;
					Run->Values[5] = static_cast<float>(Flat->CellToWorld(FIntPoint(Floor.X + 3, Floor.Y + 3)).X) - Cell * 0.5f;
					Run->Values[6] = static_cast<float>(Flat->CellToWorld(FIntPoint(Floor.X + 8, Floor.Y + 3)).X) + Cell * 0.5f;
					Run->Values[7] = static_cast<float>(Flat->CellToWorld(FIntPoint(Floor.X + 3, Floor.Y + 3)).Z) - Cell * 0.5f - FloorZ;
				}
				Run->Values[4] = FloorZ;
				HoldCamera();
			},
			[this, Run, Base, LookAt](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.15f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					Teleport(Base + FVector(-1000.f, 0.f, 0.f));
					if (R.Ball.IsValid())
					{
						R.Ball->SetSizeForTest(170.f);
					}
					R.Values[0] = 0.f;  // the biggest it was under the roof
					R.Values[1] = -1.f; // its size once out
					R.Values[2] = 0.f;  // at the end
				}
				// The roof, and the ball once it is out (both in the picture).
				const float BallX = R.Ball.IsValid() ? static_cast<float>(R.Ball->GetActorLocation().X - Base.X) : 600.f;
				LookAt(Base + FVector(FMath::Max(600.f, (600.f + BallX) * 0.5f), 0.f, 150.f), 2200.f, 120.f);
				if (APinguGiantSnowball* Ball = R.Ball.Get())
				{
					const float X = static_cast<float>(Ball->GetActorLocation().X);
					const float D = Ball->GetDiameter();
					if (X > R.Values[5] && X < R.Values[6])
					{
						R.Values[0] = FMath::Max(R.Values[0], D);
						if (!R.Flags[0] && X > (R.Values[5] + R.Values[6]) * 0.5f)
						{
							R.Flags[0] = true;
							Shot(TEXT("99_giant_roof_in"));
							++R.Shots;
						}
					}
					if (R.Values[1] < 0.f && X > R.Values[6] + D)
					{
						R.Values[1] = D;
					}
					R.Values[2] = D;
					R.Values[3] = Ball->GetRoofHeld();
					R.Counts[0] = Ball->GetWallHits();
					if (!R.Flags[1] && T > 6.3f)
					{
						R.Flags[1] = true;
						Shot(TEXT("99_giant_roof_out"));
						++R.Shots;
					}
				}
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const float Room = R.Values[7];
				const bool bOk = R.bUsed && !R.bGone && R.Counts[0] == 0 && R.Values[0] > 175.f && R.Values[0] <= Room && R.Values[3] > 200.f
					&& R.Values[1] > 0.f && R.Values[2] > R.Values[0] + 15.f;
				Report(FString::Printf(TEXT("RESULT giant_Roof: under a roof %.0f cm over the floor it got up to %.0f cm (want 175 .. %.0f), rolled %.0f cm without growing (want > 200), wall hits %d (want 0), gone %d (%s; want 0), out of it %.0f, at the end %.0f cm (want 15+ more than under it): %s"),
					Room, R.Values[0], Room, R.Values[3], R.Counts[0], R.bGone ? 1 : 0, *R.End, R.Values[1], R.Values[2], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_GapBig / giant_GapSmall (2026-11-05, Linus: "when its bigger than an 1 block gap it ignores 1 block gaps and
	// rolls over them"): the floor 4 m right of the roller is cut out one block wide, all the way down. A 1.5-tile ball rolls over
	// it (its foot never below the floor, no wall hit); a ball still under 1.05 tiles there drops into it. Shots 99_giant_gap_big, 99_giant_gap_small.
	for (int32 Variant = 0; Variant < 2; ++Variant)
	{
		const bool bBig = Variant == 0;
		const TCHAR* StepName = bBig ? TEXT("giant_GapBig") : TEXT("giant_GapSmall");
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(StepName, 4.f,
			[this, Run, Base, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(-1500.f, { EPenguinAbility::Snowball });
				FIntPoint Floor;
				float FloorZ = 0.f;
				if (ABlockIsland* Flat = FloorUnder(GetWorld(), Base, Floor, FloorZ))
				{
					for (int32 Down = 0; Down < 8; ++Down)
					{
						Flat->BreakBlockAt(Flat->CellToWorld(FIntPoint(Floor.X + 4, Floor.Y - Down)), false);
					}
					Flat->FlushEdits();
					Run->Values[5] = static_cast<float>(Flat->CellToWorld(FIntPoint(Floor.X + 4, Floor.Y)).X);
					Run->Flags[2] = !Flat->IsSolidCell(FIntPoint(Floor.X + 4, Floor.Y)) && Flat->IsSolidCell(FIntPoint(Floor.X + 3, Floor.Y))
						&& Flat->IsSolidCell(FIntPoint(Floor.X + 5, Floor.Y));
				}
				Run->Values[4] = FloorZ;
				HoldCamera();
			},
			[this, Run, Base, LookAt, bBig](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.15f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					Teleport(Base + FVector(-1000.f, 0.f, 0.f));
					if (R.Ball.IsValid())
					{
						R.Ball->SetSizeForTest(bBig ? 150.f : 55.f); // it grows on the way: about 95 cm at the gap
					}
					R.Values[0] = 0.f;    // its size over the gap
					R.Values[1] = 1.e9f;  // the lowest its foot got
					R.Values[3] = -1.e9f; // the furthest it got right of the gap's middle
				}
				LookAt(Base + FVector(400.f, 0.f, 60.f), 1100.f, 120.f);
				if (APinguGiantSnowball* Ball = R.Ball.Get())
				{
					const float X = static_cast<float>(Ball->GetActorLocation().X);
					const float Foot = static_cast<float>(Ball->GetActorLocation().Z) - Ball->GetRadius() - R.Values[4];
					R.Values[1] = FMath::Min(R.Values[1], Foot);
					R.Values[3] = FMath::Max(R.Values[3], X - R.Values[5]);
					R.Counts[0] = Ball->GetWallHits();
					if (FMath::Abs(X - R.Values[5]) < 20.f)
					{
						R.Values[0] = Ball->GetDiameter();
					}
					if (!R.Flags[0] && (bBig ? FMath::Abs(X - R.Values[5]) < 10.f : Foot < -20.f))
					{
						R.Flags[0] = true;
						Shot(bBig ? TEXT("99_giant_gap_big") : TEXT("99_giant_gap_small"));
						++R.Shots;
					}
				}
				Watch(R, T);
			},
			[this, Run, StepName, bBig, CleanUp]
			{
				const FRun& R = *Run;
				bool bOk = R.bUsed && R.Flags[2];
				if (bBig)
				{
					bOk &= !R.bGone && R.Counts[0] == 0 && R.Values[1] > -5.f && R.Values[3] > 300.f && R.Values[0] > 105.f;
				}
				else
				{
					// Into the gap, not over it (nearly as wide as the gap, it then knocks between its walls: as before 2026-11-05).
					bOk &= R.Values[1] < -20.f && R.Values[3] < 100.f;
				}
				Report(FString::Printf(TEXT("RESULT %s: a one-block gap made %d, %.0f cm wide over it, its foot down to %.0f cm (want %s), got %.0f cm past the gap, wall hits %d, gone %d (%s): %s"),
					StepName, R.Flags[2] ? 1 : 0, R.Values[0], R.Values[1], bBig ? TEXT("never below the floor, on 3 m past it, no wall hit") : TEXT("into the gap: below -20, never on past it"),
					R.Values[3], R.Counts[0], R.bGone ? 1 : 0, *R.End, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- look_GiantTrail (2026-10-29, Linus: "the normal visual it already has with a small snow particle behind it"): the soft
	// snow trail it kicks up while it rolls, at 1 tile and at 3 tiles; none before it rolls off. Shots 89_giant_trail_1tile / _3tiles.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("look_GiantTrail"), 2.6f,
			[this, Run, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(-1500.f, { EPenguinAbility::Snowball });
				HoldCamera();
			},
			[this, Run, LookAt](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = APinguGiantSnowball::BeginRollUp(Penguin.Get(), 0, 1.f);
					R.bUsed = R.Ball.IsValid();
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (!Ball)
				{
					return;
				}
				// Still rolled up: no trail yet. Then off it goes.
				if (!R.Flags[0] && T > 0.5f)
				{
					R.Flags[0] = true;
					R.Counts[0] = Ball->GetTrailPuffs();
					Ball->LaunchForTest();
				}
				LookAt(Ball->GetActorLocation(), 500.f + Ball->GetDiameter() * 2.2f, 40.f);
				if (R.Shots == 0 && T > 1.1f)
				{
					R.Counts[1] = Ball->GetTrailPuffs();
					Shot(TEXT("89_giant_trail_1tile"));
					++R.Shots;
					Ball->SetSizeForTest(PinguTuning::GiantRollMaxSize);
				}
				if (R.Shots == 1 && T > 2.2f)
				{
					R.Counts[2] = Ball->GetTrailPuffs();
					Shot(TEXT("89_giant_trail_3tiles"));
					++R.Shots;
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.bUsed && R.Counts[0] == 0 && R.Counts[1] >= 3 && R.Counts[2] > R.Counts[1] && R.Shots == 2;
				Report(FString::Printf(TEXT("RESULT giant trail: puffs while rolled up %d (want 0), after 0.6 s of rolling %d, 1.1 s later %d (want more), %d of 2 pictures: %s"),
					R.Counts[0], R.Counts[1], R.Counts[2], R.Shots, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_SwallowRelease: the dummy 4 m ahead is swallowed (hidden, no collision, hanging still, drawn inside), then the
	// ball breaks and lets it out where it broke.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_SwallowRelease"), 3.f,
			[this, Run, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(400.f, { EPenguinAbility::Snowball });
				HoldCamera();
			},
			[this, Run, LookAt](float T)
			{
				FRun& R = *Run;
				APenguinCharacter* Dummy = Target.Get();
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					R.UseAt = T;
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (Ball)
				{
					LookAt(Ball->GetActorLocation(), 520.f, 30.f);
				}
				if (Ball && Dummy && T > 1.7f && !R.Flags[0])
				{
					R.Flags[0] = true;
					const UPenguinMovementComponent* DummyMove = Dummy->GetPenguinMovement();
					R.Flags[1] = Ball->HoldsPenguin(Dummy) && Dummy->IsHidden() && Dummy->IsAlive() && !Dummy->GetActorEnableCollision()
						&& DummyMove && DummyMove->IsHovering();
					R.Counts[0] = Ball->GetShownInside();
					Shot(TEXT("84_giant_inside"));
					++R.Shots;
				}
				if (Ball && T > 2.1f && !R.Flags[2])
				{
					R.Flags[2] = true;
					Ball->Burst(Penguin.Get(), TEXT("test"));
				}
				Watch(R, T);
				if (R.bGone && Dummy && !R.Flags[3] && T > R.GoneAt + 0.3f)
				{
					R.Flags[3] = true;
					const UPenguinMovementComponent* DummyMove = Dummy->GetPenguinMovement();
					R.Flags[4] = !Dummy->IsHidden() && Dummy->IsAlive() && Dummy->GetActorEnableCollision() && DummyMove && !DummyMove->IsHovering();
					R.Values[0] = FMath::Abs(static_cast<float>(Dummy->GetActorLocation().X - R.EndAt.X));
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Flags[1] && R.Counts[0] == 1 && R.End == TEXT("test") && R.Flags[4] && R.Values[0] < 250.f;
				Report(FString::Printf(TEXT("RESULT giant swallow: the dummy inside (hidden, no collision, hanging still, alive) %d, drawn inside %d of 1, let out where it broke %d (%.0f cm from its middle): %s"),
					R.Flags[1] ? 1 : 0, R.Counts[0], R.Flags[4] ? 1 : 0, R.Values[0], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_SeaInside: the dummy swallowed near the strip's end, rolled off it: out in the sea, our knock-out.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_SeaInside"), 8.f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(0.f, { EPenguinAbility::Snowball });
				Teleport(FVector(FlatIslandX + 2000.f, 0.f, 62.f));
				ResetTarget(FVector(FlatIslandX + 2450.f, 0.f, 62.f));
				Penguin->ResetForRound(Penguin->GetActorLocation());
				Penguin->SetInputLocked(false);
				if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
				{
					Mode->SetActorTickEnabled(false); // (no practice respawn before we look)
				}
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					R.UseAt = T;
				}
				if (R.Ball.IsValid())
				{
					R.Flags[0] |= R.Ball->HoldsPenguin(Target.Get());
				}
				Watch(R, T);
				if (Target.IsValid() && Target->IsKnockedOut() && !R.Flags[1])
				{
					R.Flags[1] = true;
					R.Values[0] = T;
					R.Flags[2] = Target->GetKOReason() == EPenguinKOReason::Sea;
					R.Flags[3] = Target->GetKOBy() == Penguin.Get();
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Flags[0] && R.End == TEXT("sea") && R.Flags[1] && R.Flags[2] && R.Flags[3];
				Report(FString::Printf(TEXT("RESULT giant sea: the dummy swallowed %d, the ball ended %d (%s), the dummy out %d (sea %d, our knock-out %d): %s"),
					R.Flags[0] ? 1 : 0, R.bGone ? 1 : 0, *R.End, R.Flags[1] ? 1 : 0, R.Flags[2] ? 1 : 0, R.Flags[3] ? 1 : 0, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_SnowballSizes: snowballs of every size work on it: a normal one +30 cm, a big one (x2: a grown penguin's) +60, a
	// small one (x0.5: a shrunk penguin's) +15.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_SnowballSizes"), 2.4f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (!Ball)
				{
					return;
				}
				static const float Sizes[] = { 1.f, 2.f, 0.5f };
				for (int32 Index = 0; Index < 3; ++Index)
				{
					const float At = 0.3f + Index * 0.6f;
					if (R.Counts[0] == Index && T > At)
					{
						// Thrown from just behind it (its size before the hit, then 0.25 s later what it grew by - less the rolling's).
						R.Values[Index * 2] = Ball->GetTargetDiameter();
						R.Values[6] = Ball->GetRolled();
						const FVector From = Ball->GetActorLocation() - FVector(Ball->GetRadius() + 40.f, 0.f, 0.f);
						if (APenguinProjectile* Snowball = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Penguin.Get(), From, FVector(2100.f, 0.f, 0.f)))
						{
							if (Sizes[Index] != 1.f)
							{
								Snowball->Rescale(Sizes[Index]);
							}
						}
						++R.Counts[0];
					}
					if (R.Counts[1] == Index && R.Counts[0] > Index && T > At + 0.25f)
					{
						R.Values[Index * 2 + 1] = Ball->GetTargetDiameter() - R.Values[Index * 2] - GiantRollGrowPerCm * (Ball->GetRolled() - R.Values[6]);
						++R.Counts[1];
					}
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Counts[1] == 3 && FMath::Abs(R.Values[1] - GiantSnowballGrow) < 2.f && FMath::Abs(R.Values[3] - 2.f * GiantSnowballGrow) < 3.f
					&& FMath::Abs(R.Values[5] - 0.5f * GiantSnowballGrow) < 2.f;
				Report(FString::Printf(TEXT("RESULT giant snowball sizes: a normal snowball grew it %.1f cm (want %.0f), a big one (x2) %.1f (want %.0f), a small one (x0.5) %.1f (want %.0f): %s"),
					R.Values[1], GiantSnowballGrow, R.Values[3], 2.f * GiantSnowballGrow, R.Values[5], 0.5f * GiantSnowballGrow, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_PastCap: at 3 tiles (the rolling cap) a snowball still grows it (Linus: "snowballs can push it past the limit");
	// the technical cap (8 tiles) holds.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_PastCap"), 1.6f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					if (R.Ball.IsValid())
					{
						R.Ball->SetSizeForTest(GiantRollMaxSize);
					}
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (!Ball)
				{
					return;
				}
				if (!R.Flags[0] && T > 0.4f)
				{
					R.Flags[0] = true;
					R.Values[0] = Ball->GetTargetDiameter();
					const FVector From = Ball->GetActorLocation() - FVector(Ball->GetRadius() + 40.f, 0.f, 0.f);
					APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Penguin.Get(), From, FVector(2100.f, 0.f, 0.f));
				}
				if (R.Flags[0] && !R.Flags[1] && T > 0.8f)
				{
					R.Flags[1] = true;
					R.Values[1] = Ball->GetTargetDiameter();
					R.Values[3] = Ball->GetDiameter();
					// Far past it all at once: the safety cap.
					Ball->AddSize(5000.f, true);
					R.Values[2] = Ball->GetTargetDiameter();
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = FMath::IsNearlyEqual(R.Values[0], GiantRollMaxSize, 0.5f) && R.Values[1] > GiantRollMaxSize + GiantSnowballGrow - 2.f
					&& R.Values[3] > GiantRollMaxSize + 10.f && FMath::IsNearlyEqual(R.Values[2], GiantSafetyMaxSize, 0.5f);
				Report(FString::Printf(TEXT("RESULT giant past the cap: %.0f cm (the rolling cap) + a snowball = %.0f (drawn %.0f; want more than %.0f), pushed far past it: %.0f (want the safety cap %.0f): %s"),
					R.Values[0], R.Values[1], R.Values[3], GiantRollMaxSize, R.Values[2], GiantSafetyMaxSize, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_Rays: the grow ray x1.297 and the shrink ray x0.707 (a penguin's steps); shrunk to its smallest, the dummy it
	// holds stays inside.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_Rays"), 3.f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(450.f, { EPenguinAbility::Snowball });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (!Ball)
				{
					return;
				}
				const auto Fire = [this, Ball](EPenguinProjectileKind Kind)
				{
					const FVector From = Ball->GetActorLocation() - FVector(Ball->GetRadius() + 40.f, 0.f, 0.f);
					APenguinProjectile::Launch(GetWorld(), Kind, Penguin.Get(), From, FVector(2300.f, 0.f, 0.f));
				};
				// grow at 0.3, shrink at 0.8; then (the dummy inside by then) four shrinks from 2.0
				// (what rolling added in between is taken off: GiantRollGrowPerCm per cm)
				if (R.Counts[0] == 0 && T > 0.3f) { R.Values[0] = Ball->GetTargetDiameter(); R.Values[6] = Ball->GetRolled(); Fire(EPenguinProjectileKind::GrowRay); ++R.Counts[0]; }
				if (R.Counts[0] == 1 && T > 0.6f) { R.Values[1] = Ball->GetTargetDiameter() - GiantRollGrowPerCm * (Ball->GetRolled() - R.Values[6]); ++R.Counts[0]; }
				if (R.Counts[0] == 2 && T > 0.8f) { R.Values[2] = Ball->GetTargetDiameter(); R.Values[6] = Ball->GetRolled(); Fire(EPenguinProjectileKind::ShrinkRay); ++R.Counts[0]; }
				if (R.Counts[0] == 3 && T > 1.1f) { R.Values[3] = Ball->GetTargetDiameter() - GiantRollGrowPerCm * (Ball->GetRolled() - R.Values[6]); ++R.Counts[0]; }
				if (R.Counts[0] >= 4 && R.Counts[0] < 8 && T > 2.f + (R.Counts[0] - 4) * 0.25f)
				{
					R.Flags[0] |= Ball->HoldsPenguin(Target.Get());
					Fire(EPenguinProjectileKind::ShrinkRay);
					++R.Counts[0];
				}
				if (R.Counts[0] == 8 && T > 2.85f)
				{
					R.Values[4] = Ball->GetTargetDiameter();
					R.Flags[1] = Ball->HoldsPenguin(Target.Get()) && Target->IsHidden();
					++R.Counts[0];
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const float Grow = R.Values[0] > 0.f ? R.Values[1] / R.Values[0] : 0.f;
				const float Shrink = R.Values[2] > 0.f ? R.Values[3] / R.Values[2] : 0.f;
				// (the rolling adds a little in between: 0.1 cm per cm in 0.3 s = about 8 cm)
				const bool bOk = FMath::Abs(Grow - PinguGrowRayFactor) < 0.02f && FMath::Abs(Shrink - PinguShrinkRayFactor) < 0.02f && R.Flags[0]
					&& R.Values[4] >= GiantMinSize - 0.5f && R.Values[4] < GiantMinSize + 6.f && R.Flags[1];
				Report(FString::Printf(TEXT("RESULT giant rays: the grow ray x%.3f (want x%.3f), the shrink ray x%.3f (want x%.3f), four more shrinks: %.0f cm (want the smallest, %.0f), the dummy inside before %d and after %d: %s"),
					Grow, PinguGrowRayFactor, Shrink, PinguShrinkRayFactor, R.Values[4], GiantMinSize, R.Flags[0] ? 1 : 0, R.Flags[1] ? 1 : 0, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_IceGun: an ice bolt freezes it - it slides faster and shoves the dummy instead of swallowing it.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_IceGun"), 3.6f,
			[this, Run, HoldCamera]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(1100.f, { EPenguinAbility::Snowball });
				HoldCamera();
			},
			[this, Run, LookAt](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					R.Counts[1] = Target.IsValid() && Target->GetPenguinMovement() ? Target->GetPenguinMovement()->HitEvents : 0;
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (!Ball)
				{
					return;
				}
				LookAt(Ball->GetActorLocation(), 600.f, 40.f);
				if (!R.Flags[0] && T > 0.3f)
				{
					R.Flags[0] = true;
					const FVector From = Ball->GetActorLocation() - FVector(Ball->GetRadius() + 40.f, 0.f, 0.f);
					APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::IceBolt, Penguin.Get(), From, FVector(2600.f, 0.f, 0.f));
				}
				if (R.Flags[0] && !R.Flags[1] && Ball->IsFrozen())
				{
					R.Flags[1] = true;
					R.Values[0] = static_cast<float>(Ball->GetActorLocation().X);
					R.Values[1] = T;
				}
				if (R.Flags[1] && R.Values[2] < 0.f && T > R.Values[1] + 1.f)
				{
					R.Values[2] = (static_cast<float>(Ball->GetActorLocation().X) - R.Values[0]) / (T - R.Values[1]);
				}
				if (R.Flags[1] && R.Shots == 0 && T > R.Values[1] + 0.4f)
				{
					Shot(TEXT("85_giant_frozen"));
					++R.Shots;
				}
				R.Flags[2] |= Ball->HoldsPenguin(Target.Get());
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const int32 Hits = Target.IsValid() && Target->GetPenguinMovement() ? Target->GetPenguinMovement()->HitEvents - R.Counts[1] : 0;
				const bool bOk = R.Flags[1] && FMath::Abs(R.Values[2] - GiantFrozenSpeed) < 15.f && !R.Flags[2] && Hits >= 1 && !Target->IsHidden();
				Report(FString::Printf(TEXT("RESULT giant ice gun: frozen %d, sliding %.0f cm/s (want %.0f), the dummy swallowed %d (want 0), shoved %d x: %s"),
					R.Flags[1] ? 1 : 0, R.Values[2], GiantFrozenSpeed, R.Flags[2] ? 1 : 0, Hits, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_Frost: the frost breath freezes it too.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_Frost"), 1.6f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::FrostBreath });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
				}
				if (R.Ball.IsValid() && !R.Flags[0] && T > 0.3f)
				{
					R.Flags[0] = true;
					Penguin->SetAimPoint(R.Ball->GetActorLocation());
					R.Flags[1] = Penguin->ActivateAbility(0);
				}
				R.Flags[2] |= R.Ball.IsValid() && R.Ball->IsFrozen();
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				Report(FString::Printf(TEXT("RESULT giant frost: breathed %d, frozen %d: %s"), R.Flags[1] ? 1 : 0, R.Flags[2] ? 1 : 0, Ok(R.Flags[1] && R.Flags[2])));
				CleanUp();
			});
	}

	// ---- giant_BombSwallow: a bomb thrown straight into it and an ability bubble in its way are swallowed; broken, it throws
	// both out again.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_BombSwallow"), 2.6f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
				if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
				{
					Mode->SetActorTickEnabled(false); // (no sky drops: the bubbles are counted)
				}
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					if (R.Ball.IsValid())
					{
						APinguPickup::Spawn(GetWorld(), EPenguinAbility::IceShell, R.Ball->GetActorLocation() + FVector(260.f, 0.f, 0.f));
					}
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (Ball && !R.Flags[0] && T > 0.3f)
				{
					R.Flags[0] = true;
					const FVector From = Ball->GetActorLocation() - FVector(Ball->GetRadius() + 50.f, 0.f, -20.f);
					APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin.Get(), From, FVector(900.f, 0.f, 100.f));
				}
				if (Ball && !R.Flags[1] && T > 1.8f)
				{
					R.Flags[1] = true;
					R.Counts[0] = Ball->GetItemsInside();
					R.Flags[2] = Ball->IsRolling();
					Ball->Burst(nullptr, TEXT("test"));
				}
				if (R.Flags[1] && !R.Flags[3] && T > 1.95f)
				{
					R.Flags[3] = true;
					TArray<APenguinProjectile*> Shots;
					APenguinProjectile::GetAll(GetWorld(), Shots);
					for (const APenguinProjectile* Shot : Shots)
					{
						R.Counts[1] += Shot->GetKind() == EPenguinProjectileKind::Bomb ? 1 : 0;
					}
					R.Counts[2] = APinguPickup::CountAll(GetWorld());
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Counts[0] == 2 && R.Flags[2] && R.Counts[1] == 1 && R.Counts[2] == 1;
				Report(FString::Printf(TEXT("RESULT giant bomb swallow: inside %d things (want 2: the bomb, the bubble), still rolling %d, broken: %d bombs and %d bubbles out again (want 1 and 1): %s"),
					R.Counts[0], R.Flags[2] ? 1 : 0, R.Counts[1], R.Counts[2], Ok(bOk)));
				APinguPickup::DestroyAll(GetWorld());
				CleanUp();
			});
	}

	// ---- the things that break it, one step each.
	// giant_BombBlast: a bomb going off just behind it (its surface 60 cm from the bomb).
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_BombBlast"), 2.f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
				Teleport(Location() + FVector(-400.f, 0.f, 0.f)); // (out of the blast)
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f && Target.IsValid())
				{
					ResetTarget(Location() + FVector(400.f, 0.f, 0.f));
					R.Ball = RollNow(Target.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					R.UseAt = T;
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (Ball && !R.Flags[0] && T > 0.9f)
				{
					R.Flags[0] = true;
					const FVector C = Ball->GetActorLocation();
					if (APenguinProjectile* Bomb = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin.Get(),
						FVector(C.X - Ball->GetRadius() - 60.f, 0.f, 34.f), FVector::ZeroVector))
					{
						Bomb->Detonate(0.f);
					}
					R.Values[0] = T;
				}
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.bGone && R.End == TEXT("snow bomb") && R.GoneAt - R.Values[0] < 0.3f;
				Report(FString::Printf(TEXT("RESULT giant bomb blast: broken %d (%s) %.2f s after the bomb was set off: %s"), R.bGone ? 1 : 0, *R.End, R.GoneAt - R.Values[0], Ok(bOk)));
				CleanUp();
			});
	}
	// giant_Noot / giant_Slap: our own ball, rolling away from us, at point blank.
	for (const EPenguinAbility Ability : { EPenguinAbility::Noot, EPenguinAbility::FlipperSlap })
	{
		const bool bNoot = Ability == EPenguinAbility::Noot;
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(bNoot ? TEXT("giant_Noot") : TEXT("giant_Slap"), 1.6f,
			[this, Run, Ability]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball, Ability });
			},
			[this, Run, bNoot](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					R.UseAt = T;
				}
				if (R.Ball.IsValid() && !R.Flags[0] && T > (bNoot ? 0.45f : 0.25f))
				{
					R.Flags[0] = true;
					Penguin->SetAimPoint(R.Ball->GetActorLocation());
					R.Values[0] = static_cast<float>(R.Ball->GetActorLocation().X - Penguin->GetActorLocation().X) - R.Ball->GetRadius();
					R.Flags[1] = Penguin->ActivateAbility(1);
				}
				Watch(R, T);
			},
			[this, Run, bNoot, CleanUp]
			{
				const FRun& R = *Run;
				const TCHAR* Want = bNoot ? TEXT("noot") : TEXT("flipper slap");
				const bool bOk = R.Flags[1] && R.bGone && R.End == Want;
				Report(FString::Printf(TEXT("RESULT giant %s: used %d with its surface %.0f cm away, broken %d (%s): %s"), Want, R.Flags[1] ? 1 : 0, R.Values[0], R.bGone ? 1 : 0, *R.End, Ok(bOk)));
				CleanUp();
			});
	}
	// giant_Stomp: a belly stomp from right above it.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_Stomp"), 2.f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(-400.f, { EPenguinAbility::BellyStomp });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f && Target.IsValid())
				{
					R.Ball = RollNow(Target.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
				}
				APinguGiantSnowball* Ball = R.Ball.Get();
				if (Ball && !R.Flags[0])
				{
					// Hanging over it until we stomp.
					const FVector C = Ball->GetActorLocation();
					Teleport(FVector(C.X + 20.f, 0.f, C.Z + Ball->GetRadius() + 260.f));
					if (T > 0.5f)
					{
						R.Flags[0] = true;
						R.Flags[1] = Penguin->ActivateAbility(0);
					}
				}
				R.Flags[2] |= Ball && Ball->HoldsPenguin(Penguin.Get());
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Flags[1] && R.bGone && R.End == TEXT("belly stomp") && !R.Flags[2];
				Report(FString::Printf(TEXT("RESULT giant stomp: stomped %d, broken %d (%s), swallowed us %d: %s"), R.Flags[1] ? 1 : 0, R.bGone ? 1 : 0, *R.End, R.Flags[2] ? 1 : 0, Ok(bOk)));
				CleanUp();
			});
	}
	// giant_Iceberg: an iceberg dropped where it will be.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_Iceberg"), 2.4f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(-500.f, { EPenguinAbility::Snowball });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f && Target.IsValid())
				{
					R.Ball = RollNow(Target.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
					if (R.Ball.IsValid())
					{
						// It warns for 0.45 s and falls for about 0.55: by then the ball is 2.6 m further.
						const FVector C = R.Ball->GetActorLocation();
						R.Flags[0] = AIcebergDrop::Drop(GetWorld(), Penguin.Get(), static_cast<float>(C.X) + 255.f, 0.f) != nullptr;
					}
				}
				Watch(R, T);
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.Flags[0] && R.bGone && R.End == TEXT("iceberg");
				Report(FString::Printf(TEXT("RESULT giant iceberg: dropped %d, broken %d (%s): %s"), R.Flags[0] ? 1 : 0, R.bGone ? 1 : 0, *R.End, Ok(bOk)));
				CleanUp();
			});
	}
	// giant_HotFeet / giant_IceTouch / giant_Shell: our penguin standing in its way.
	enum class EStand : uint8 { HotFeet, IceTouch, Shell };
	for (const EStand Stand : { EStand::HotFeet, EStand::IceTouch, EStand::Shell })
	{
		const TCHAR* Name = Stand == EStand::HotFeet ? TEXT("giant_HotFeet") : Stand == EStand::IceTouch ? TEXT("giant_IceTouch") : TEXT("giant_Shell");
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(Name, 1.8f,
			[this, Run, Stand]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(-450.f, { Stand == EStand::Shell ? EPenguinAbility::IceShell : EPenguinAbility::Snowball });
				if (Stand != EStand::Shell)
				{
					SetTrait(Penguin.Get(), Stand == EStand::HotFeet ? EPinguTrait::HotFeet : EPinguTrait::IceTouch);
				}
				Run->Counts[0] = Move->HitEvents;
			},
			[this, Run, Stand](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f && Target.IsValid())
				{
					R.Ball = RollNow(Target.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
				}
				if (Stand == EStand::Shell && !R.Flags[0] && T > 0.75f)
				{
					R.Flags[0] = true;
					Penguin->SetAimPoint(Penguin->GetAimOrigin());
					Penguin->ActivateAbility(0);
				}
				R.Flags[1] |= Penguin->IsHidden() || (R.Ball.IsValid() && R.Ball->HoldsPenguin(Penguin.Get()));
				R.Flags[2] |= R.Ball.IsValid() && R.Ball->IsFrozen();
				R.Values[0] = FMath::Max(R.Values[0], static_cast<float>(Move->Velocity.X));
				Watch(R, T);
			},
			[this, Run, Stand, Name, CleanUp]
			{
				const FRun& R = *Run;
				bool bOk = !R.Flags[1] && Penguin->IsAlive();
				if (Stand == EStand::HotFeet)
				{
					bOk &= R.bGone && R.End == TEXT("hot feet");
				}
				else
				{
					// Frozen (Ice Touch) or held off by the shell: pushed aside, the ball rolls on.
					bOk &= (Stand != EStand::IceTouch || R.Flags[2]) && R.Values[0] > 300.f && !R.bGone;
				}
				Report(FString::Printf(TEXT("RESULT %s: swallowed us %d (want 0), the ball frozen %d, broken %d (%s), pushed us at %.0f cm/s: %s"), Name, R.Flags[1] ? 1 : 0,
					R.Flags[2] ? 1 : 0, R.bGone ? 1 : 0, *R.End, R.Values[0], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- giant_Merge: two meet head on - the bigger (the older of two alike) takes the other in, their sizes added as balls'.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("giant_Merge"), 2.6f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(900.f, { EPenguinAbility::Snowball });
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.1f && Target.IsValid())
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					APinguGiantSnowball* Other = RollNow(Target.Get(), -1.f);
					R.bUsed = R.Ball.IsValid() && Other;
				}
				TArray<APinguGiantSnowball*> All;
				APinguGiantSnowball::GetAll(GetWorld(), All);
				if (All.Num() == 2)
				{
					R.Values[0] = All[0]->GetTargetDiameter();
					R.Values[1] = All[1]->GetTargetDiameter();
				}
				if (All.Num() == 1 && R.bUsed && R.Values[2] < 0.f)
				{
					R.Values[2] = All[0]->GetTargetDiameter();
				}
				R.Counts[0] = All.Num();
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const float Want = FMath::Sqrt(R.Values[0] * R.Values[0] + R.Values[1] * R.Values[1]);
				const bool bOk = R.bUsed && R.Counts[0] == 1 && FMath::Abs(R.Values[2] - Want) < 6.f;
				Report(FString::Printf(TEXT("RESULT giant merge: %.0f and %.0f cm met, %d left of them, %.0f cm (want about %.0f): %s"), R.Values[0], R.Values[1], R.Counts[0], R.Values[2], Want, Ok(bOk)));
				CleanUp();
			});
	}

	// ---- bot_GiantDodge: a bot jumps over one rolling at it.
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("bot_GiantDodge"), 4.f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(700.f, { EPenguinAbility::Snowball });
				if (Target.IsValid() && Target->GetBot())
				{
					Target->SetTeam(-1);
					Penguin->SetTeam(-1);
					SavedTargetAbilities = Target->GetAbilities()->GetAbilities();
					Target->GetAbilities()->SetAbilities({ EPenguinAbility::IceWall });
					Target->GetBot()->SetBotEnabled(true, 1);
				}
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				if (!R.bUsed && T > 0.2f)
				{
					R.Ball = RollNow(Penguin.Get(), 1.f);
					R.bUsed = R.Ball.IsValid();
				}
				Teleport(FVector(Location().X, 0.f, 62.f)); // (we stay put)
				if (R.Ball.IsValid() && Target.IsValid())
				{
					R.Flags[0] |= R.Ball->HoldsPenguin(Target.Get());
					R.Values[0] = static_cast<float>(Target->GetActorLocation().X - R.Ball->GetActorLocation().X);
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bOk = R.bUsed && !R.Flags[0] && Target->IsAlive();
				Report(FString::Printf(TEXT("RESULT bot giant dodge: swallowed %d (want 0), the bot %.0f cm from the ball's middle at the end (behind it: < 0): %s"),
					R.Flags[0] ? 1 : 0, R.Values[0], Ok(bOk)));
				CleanUp();
			});
	}

	// ---- bot_GiantUse: a bot with the ability rolls one at its target (us, 7 m away on the flat strip).
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(TEXT("bot_GiantUse"), 6.f,
			[this, Run]
			{
				*Run = FRun();
				SpawnTarget();
				PrepareBrawl(700.f, { EPenguinAbility::Snowball });
				if (Target.IsValid() && Target->GetBot())
				{
					Target->SetTeam(-1);
					Penguin->SetTeam(-1);
					SavedTargetAbilities = Target->GetAbilities()->GetAbilities();
					Target->GetAbilities()->SetAbilities({ EPenguinAbility::GiantSnowball });
					Target->GetBot()->SetBotEnabled(true, 2);
				}
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				Teleport(FVector(Location().X, 0.f, 62.f)); // (we stay put)
				TArray<APinguGiantSnowball*> All;
				APinguGiantSnowball::GetAll(GetWorld(), All);
				for (const APinguGiantSnowball* Ball : All)
				{
					if (Ball->GetRoller() == Target.Get() && !R.bUsed)
					{
						R.bUsed = true;
						R.Values[0] = T;
						R.Values[1] = Ball->GetDirection();
						R.Values[2] = static_cast<float>(Location().X - Target->GetActorLocation().X);
					}
				}
				if (R.bUsed && T > R.Values[0] + 1.6f && Target.IsValid() && Target->GetBot())
				{
					Target->GetBot()->SetBotEnabled(false);
				}
			},
			[this, Run, CleanUp]
			{
				const FRun& R = *Run;
				const bool bToward = R.Values[1] * R.Values[2] > 0.f;
				const bool bOk = R.bUsed && bToward;
				Report(FString::Printf(TEXT("RESULT bot giant use: rolled one %d after %.1f s, toward us %d: %s"), R.bUsed ? 1 : 0, R.Values[0], bToward ? 1 : 0, Ok(bOk)));
				CleanUp();
			});
	}
}
