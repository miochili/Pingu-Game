// Pingu - automated movement check.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PinguMenu3D.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinAbilities.h"
#include "PenguinProjectile.h"
#include "PinguPickup.h"
#include "PinguPortal.h"
#include "PinguTornado.h"
#include "PinguGiantSnowball.h"
#include "PinguSnowman.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "ShaderCompiler.h"
#include "PenguinBot.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PenguinInputConfig.h"
#include "PenguinPlayerController.h"
#include "PinguSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "GameFramework/PlayerController.h"
#include "Misc/App.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Layout/Children.h"
#include "Input/Events.h"
#include "PenguinCamera.h"
#include "PinguFX.h"
#include "StaticMeshResources.h"

APenguinAutoTest::APenguinAutoTest()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
}

void APenguinAutoTest::BeginPlay()
{
	Super::BeginPlay();

	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		Penguin = *It;
		break;
	}
	if (!Penguin.IsValid() || !Penguin->GetPenguinMovement())
	{
		Report(TEXT("FAIL no penguin in the level"));
		Finish();
		return;
	}
	Move = Penguin->GetPenguinMovement();
	// Our virtual input must be applied before the movement component ticks.
	Move->PrimaryComponentTick.AddPrerequisite(this, PrimaryActorTick);

	// A long flat strip far away for clean measurements.
	const FTransform FlatTransform(FVector(FlatIslandX, 0.f, 0.f));
	if (ABlockIsland* Flat = GetWorld()->SpawnActorDeferred<ABlockIsland>(ABlockIsland::StaticClass(), FlatTransform))
	{
		Flat->Layout = EIslandLayout::Flat;
		Flat->bBuildBackdrop = false;
		Flat->FinishSpawning(FlatTransform);
	}

	Report(FString::Printf(TEXT("start: run %.0f, jump v %.0f, gravity scale %.2f, capsule %.0f/%.0f, clips %d"),
		Move->RunSpeed, Move->GetJumpVelocity(), Move->GravityScale,
		Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius(), Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
		Penguin->GetNumLoadedClips()));
	BuildScript();
	// -PinguAutoTestOnly=look_,matrix_: only the steps whose names contain one of these (a quick look at some of them).
	FString Only;
	// (Not stopping at the commas: the whole list.)
	if (FParse::Value(FCommandLine::Get(), TEXT("PinguAutoTestOnly="), Only, false) && !Only.IsEmpty())
	{
		TArray<FString> Patterns;
		Only.ParseIntoArray(Patterns, TEXT(","), true);
		Steps.RemoveAll([&Patterns](const FStep& Step)
		{
			for (const FString& Pattern : Patterns)
			{
				if (Step.Name.Contains(Pattern))
				{
					return false;
				}
			}
			return true;
		});
		Report(FString::Printf(TEXT("only the steps with %s in their name: %d"), *Only, Steps.Num()));
	}
}

void APenguinAutoTest::AddStep(const TCHAR* Name, float Duration, TFunction<void()> Start, TFunction<void(float)> Update, TFunction<void()> End)
{
	FStep Step;
	Step.Name = Name;
	Step.Duration = Duration;
	Step.Start = MoveTemp(Start);
	Step.Update = MoveTemp(Update);
	Step.End = MoveTemp(End);
	Steps.Add(MoveTemp(Step));
}

FVector APenguinAutoTest::Location() const
{
	return Penguin.IsValid() ? Penguin->GetActorLocation() : FVector::ZeroVector;
}

void APenguinAutoTest::Teleport(const FVector& Where)
{
	if (!Penguin.IsValid())
	{
		return;
	}
	Move->ResetMovementState();
	Move->StopMovementImmediately();
	Penguin->SetActorLocation(Where, false, nullptr, ETeleportType::TeleportPhysics);
	Move->SetMovementMode(MOVE_Falling);
}

void APenguinAutoTest::Shot(const TCHAR* Name, bool bWithUI)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ClaudeLogs/Shots") / FString(Name) + TEXT(".png"));
	// The 3D menus' and the lobby's shots (73_, 74_, 75_) always with the UI: their words and tags are drawn by the Slate text
	// overlay since 2026-10-26 - without it the signs and tags show no text, not what Linus sees.
	const FString Prefix = FString(Name).Left(3);
	const bool bUI = bWithUI || Prefix == TEXT("73_") || Prefix == TEXT("74_") || Prefix == TEXT("75_");
	FScreenshotRequest::RequestScreenshot(Path, bUI, false);
	Report(FString::Printf(TEXT("screenshot %s at %s"), Name, *Location().ToString()));
}

void APenguinAutoTest::Report(const FString& Message) const
{
	UE_LOG(LogPingu, Display, TEXT("[AUTOTEST] %s"), *Message);
}

void APenguinAutoTest::DumpStatusVisuals(const APenguinCharacter* Who, const TCHAR* Label) const
{
	if (!Who)
	{
		return;
	}
	TArray<UStaticMeshComponent*> Comps;
	Who->GetComponents<UStaticMeshComponent>(Comps);
	for (const UStaticMeshComponent* Comp : Comps)
	{
		const UMaterialInterface* Material = Comp->GetMaterial(0);
		const UStaticMesh* MeshAsset = Comp->GetStaticMesh();
		Report(FString::Printf(TEXT("DIAG %s %s: mesh %s, material %s (blend %d), render %d, visible %d, registered %d, render state %d, at %s, scale %s, bounds %.0f"),
			Label, *Comp->GetName(), MeshAsset ? *MeshAsset->GetName() : TEXT("none"), Material ? *Material->GetName() : TEXT("none"),
			Material ? static_cast<int32>(Material->GetBlendMode()) : -1, Comp->ShouldRender() ? 1 : 0, Comp->IsVisible() ? 1 : 0,
			Comp->IsRegistered() ? 1 : 0, Comp->IsRenderStateCreated() ? 1 : 0, *Comp->GetComponentLocation().ToString(),
			*Comp->GetComponentScale().ToString(), Comp->Bounds.SphereRadius));
	}
}

void APenguinAutoTest::ResetAir()
{
	StartZ = Location().Z;
	PeakZ = StartZ;
	PeakTime = -1.f;
	LandTime = -1.f;
	bLeftGround = false;
}

void APenguinAutoTest::TrackAir(float T)
{
	const FVector L = Location();
	if (!Move->IsMovingOnGround())
	{
		bLeftGround = true;
	}
	if (L.Z > PeakZ)
	{
		PeakZ = L.Z;
		PeakTime = T;
	}
	if (bLeftGround && LandTime < 0.f && Move->IsMovingOnGround())
	{
		LandTime = T;
	}
}

void APenguinAutoTest::Finish()
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	Report(TEXT("done"));
	PinguEndTestRun(this, TEXT("PinguAutoTest"));
}

void APenguinAutoTest::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bFinished || !Penguin.IsValid() || !Move.IsValid())
	{
		return;
	}

	++Frames;
	StepTime += DeltaSeconds;
	if (StepIndex < 0 || StepTime >= Steps[StepIndex].Duration)
	{
		if (StepIndex >= 0 && Steps[StepIndex].End)
		{
			Steps[StepIndex].End();
		}
		++StepIndex;
		StepTime = 0.f;
		bShotDone = false;
		bFlag = false;
		MarkA = -1.f;
		MarkB = -1.f;
		Value = 0.f;
		if (StepIndex >= Steps.Num())
		{
			Finish();
			return;
		}
		Report(FString::Printf(TEXT("step %s"), *Steps[StepIndex].Name));
		if (Steps[StepIndex].Start)
		{
			Steps[StepIndex].Start();
		}
	}
	if (Steps[StepIndex].Update)
	{
		Steps[StepIndex].Update(StepTime);
	}
	Penguin->ApplyVirtualMoveInput(VirtualInput);
}

void APenguinAutoTest::BuildScript()
{
	const FVector FlatStart(FlatIslandX - 1500.f, 0.f, 62.f);
	const float Run = Move->RunSpeed;

	// ---- wait for shaders so the screenshots show the real materials
	AddStep(TEXT("warmup"), 240.f, nullptr,
		[this](float T)
		{
			const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
			if (!bCompiling && T > 1.f)
			{
				StepTime = 1000.f; // finish this step
			}
		},
		[this] { Report(FString::Printf(TEXT("warmup done after %d frames"), Frames)); });

	// ---- main island look
	AddStep(TEXT("settle"), 1.6f, [this] { VirtualInput = FVector2D::ZeroVector; }, nullptr,
		[this] { Shot(TEXT("01_spawn")); Report(FString::Printf(TEXT("spawn location %s grounded %d"), *Location().ToString(), Move->IsMovingOnGround())); });

	AddStep(TEXT("main_run_left"), 0.42f, [this] { VirtualInput = FVector2D(-1.f, 0.f); },
		[this](float T) { if (T > 0.3f && !bShotDone) { Shot(TEXT("02_run")); bShotDone = true; } },
		[this] { VirtualInput = FVector2D::ZeroVector; });

	AddStep(TEXT("main_jump"), 1.2f, [this] { Move->JumpPressed(); },
		[this](float T) { if (T > 0.2f && !bShotDone) { Shot(TEXT("03_jump")); bShotDone = true; } if (T > 0.6f) { Move->JumpReleased(); } },
		[this] { Move->JumpReleased(); });

	// ---- measurements on the flat strip
	AddStep(TEXT("teleport_flat"), 0.9f, [this, FlatStart] { Teleport(FlatStart); }, nullptr, nullptr);

	AddStep(TEXT("accelerate"), 0.9f, [this] { VirtualInput = FVector2D(1.f, 0.f); },
		[this, Run](float T) { if (MarkA < 0.f && Move->Velocity.X >= 0.95f * Run) { MarkA = T; } },
		[this, Run] { Report(FString::Printf(TEXT("RESULT accelerate: 95%% of %.0f cm/s after %.3f s"), Run, MarkA)); });

	AddStep(TEXT("brake"), 0.6f, [this] { VirtualInput = FVector2D::ZeroVector; StartX = Location().X; },
		[this](float T) { if (MarkA < 0.f && FMath::Abs(Move->Velocity.X) < 5.f) { MarkA = T; Value = Location().X - StartX; } },
		[this] { Report(FString::Printf(TEXT("RESULT brake: stopped after %.3f s over %.0f cm"), MarkA, Value)); });

	AddStep(TEXT("turnaround"), 1.0f, [this] { VirtualInput = FVector2D(1.f, 0.f); },
		[this, Run](float T)
		{
			if (T > 0.45f && MarkA < 0.f) { MarkA = T; VirtualInput = FVector2D(-1.f, 0.f); }
			if (MarkA > 0.f && MarkB < 0.f && Move->Velocity.X <= -0.95f * Run) { MarkB = T - MarkA; }
		},
		[this] { VirtualInput = FVector2D::ZeroVector; Report(FString::Printf(TEXT("RESULT turnaround: full speed right to full speed left in %.3f s"), MarkB)); });

	AddStep(TEXT("settle2"), 0.5f, nullptr, nullptr, nullptr);

	AddStep(TEXT("full_jump"), 1.3f, [this] { ResetAir(); Move->JumpPressed(); },
		[this](float T) { TrackAir(T); },
		[this] { Move->JumpReleased(); Report(FString::Printf(TEXT("RESULT full jump: height %.0f cm, apex after %.3f s, airtime %.3f s"), PeakZ - StartZ, PeakTime, LandTime)); });

	AddStep(TEXT("short_hop"), 1.0f, [this] { ResetAir(); Move->JumpPressed(); },
		[this](float T) { if (T > 0.03f) { Move->JumpReleased(); } TrackAir(T); },
		[this] { Report(FString::Printf(TEXT("RESULT short hop (tap): height %.0f cm, airtime %.3f s"), PeakZ - StartZ, LandTime)); });

	AddStep(TEXT("double_jump"), 1.7f, [this] { ResetAir(); Move->JumpPressed(); },
		[this](float T)
		{
			TrackAir(T);
			if (!bFlag && T > 0.34f) { Move->JumpReleased(); Move->JumpPressed(); bFlag = true; }
			if (T > 0.3f && T < 0.5f && !bShotDone && bFlag && T > 0.4f) { Shot(TEXT("04_double_jump")); bShotDone = true; }
			if (T > 1.1f) { Move->JumpReleased(); }
		},
		[this] { Move->JumpReleased(); Report(FString::Printf(TEXT("RESULT double jump: total height %.0f cm, airtime %.3f s, air jumps left %d"), PeakZ - StartZ, LandTime, Move->GetAirJumpsLeft())); });

	AddStep(TEXT("ground_dash"), 0.9f, [this] { StartX = Location().X; Move->DashPressed(); },
		[this](float T)
		{
			if (T > 0.07f && !bShotDone) { Shot(TEXT("05_dash")); bShotDone = true; }
			if (Move->IsDashing()) { MarkA = T; Value = Location().X - StartX; }
		},
		[this] { Report(FString::Printf(TEXT("RESULT ground dash: %.0f cm in %.3f s, cooldown left %.2f"), Value, MarkA, Move->GetDashCooldownRemaining())); });

	AddStep(TEXT("air_dash_up"), 1.5f, [this] { ResetAir(); Move->JumpPressed(); },
		[this](float T)
		{
			TrackAir(T);
			if (!bFlag && T > 0.3f) { VirtualInput = FVector2D(0.7f, 0.7f); Move->DashPressed(); bFlag = true; }
			if (T > 0.36f) { VirtualInput = FVector2D::ZeroVector; }
			if (T > 0.55f) { Move->JumpReleased(); }
		},
		[this] { Move->JumpReleased(); Report(FString::Printf(TEXT("RESULT jump + up-right air dash: height %.0f cm, airtime %.3f s, air dashes left %d"), PeakZ - StartZ, LandTime, Move->GetAirDashesLeft())); });

	AddStep(TEXT("teleport_flat2"), 0.7f, [this, FlatStart] { Teleport(FlatStart); }, nullptr, nullptr);

	// The belly slide is gone: holding down while running changes nothing (no crouch, no speed boost).
	AddStep(TEXT("no_slide"), 1.2f, [this] { VirtualInput = FVector2D(1.f, 0.f); MarkA = 0.f; },
		[this](float T)
		{
			if (T > 0.35f) { VirtualInput = FVector2D(1.f, -1.f); }
			if (T > 0.4f)
			{
				bFlag |= Move->IsCrouching() || Move->IsBellySliding();
				Value = FMath::Max(Value, FMath::Abs(static_cast<float>(Move->Velocity.X)));
				MarkA = FMath::Max(MarkA, Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
			}
			if (T > 0.6f && !bShotDone) { Shot(TEXT("06_down_while_running")); bShotDone = true; }
		},
		[this] { VirtualInput = FVector2D::ZeroVector; Report(FString::Printf(TEXT("RESULT no slide: slid or crouched %d, top speed holding down %.0f, capsule half height %.0f"), bFlag ? 1 : 0, Value, MarkA)); });

	AddStep(TEXT("fast_fall"), 1.4f, [this] { VirtualInput = FVector2D::ZeroVector; ResetAir(); Move->JumpPressed(); },
		[this](float T)
		{
			TrackAir(T);
			if (T > 0.3f) { Move->JumpReleased(); }
			if (T > 0.38f && T < 0.9f) { VirtualInput = FVector2D(0.f, -1.f); } else { VirtualInput = FVector2D::ZeroVector; }
			Value = FMath::Min(Value, Move->Velocity.Z);
			if (Move->IsFastFalling()) { bFlag = true; }
		},
		[this] { Report(FString::Printf(TEXT("RESULT fast fall: triggered %d, max fall speed %.0f, airtime %.3f s (height %.0f)"), bFlag ? 1 : 0, -Value, LandTime, PeakZ - StartZ)); });

	AddStep(TEXT("coyote"), 2.2f, [this] { Teleport(FVector(FlatIslandX + 2650.f, 0.f, 62.f)); },
		[this](float T)
		{
			if (T > 0.4f && MarkA < 0.f) { VirtualInput = FVector2D(1.f, 0.f); MarkA = T; }
			if (MarkA > 0.f && !bFlag && !Move->IsMovingOnGround() && T > MarkA + 0.1f) { bFlag = true; MarkB = T; }
			if (bFlag && MarkB > 0.f && T > MarkB + 0.07f && Value == 0.f) { Move->JumpPressed(); Value = 1.f; }
			if (Value == 1.f && Move->Velocity.Z > 900.f) { Value = 2.f; }
			if (T > MarkB + 0.4f && MarkB > 0.f) { Move->JumpReleased(); VirtualInput = FVector2D::ZeroVector; }
		},
		[this] { Move->JumpReleased(); VirtualInput = FVector2D::ZeroVector; Report(FString::Printf(TEXT("RESULT coyote jump 0.07 s after leaving the ledge: %s"), Value == 2.f ? TEXT("jumped") : TEXT("NO JUMP"))); });

	// A player hammering jump while running: every press on the ground (or with the double jump left) must jump,
	// also across the seams between snow and ice blocks (separate collision boxes).
	AddStep(TEXT("walk_jump_presses"), 4.2f,
		[this, FlatStart]
		{
			Teleport(FlatStart);
			if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FlatStart))
			{
				for (const float Along : { 350.f, 800.f, 1300.f, 1700.f })
				{
					Flat->FreezeBlocksInRadius(FVector(FlatStart.X + Along, 0.f, -50.f), 90.f, false);
				}
				Flat->RebuildAfterEdits();
			}
			VirtualInput = FVector2D(1.f, 0.f);
			PressTimes.Reset();
			PressShouldJump.Reset();
			PressCounters.Reset();
			bJumpKeyDown = false;
		},
		[this](float T)
		{
			if (bJumpKeyDown && T > PressTimes.Last() + 0.06f)
			{
				Move->JumpReleased();
				bJumpKeyDown = false;
			}
			// A tap every 0.31 s, whatever the penguin is doing.
			const int32 Wanted = T > 0.25f && T < 3.8f ? 1 + FMath::FloorToInt((T - 0.25f) / 0.31f) : PressTimes.Num();
			if (!bJumpKeyDown && PressTimes.Num() < Wanted)
			{
				PressShouldJump.Add(Move->IsMovingOnGround() || Move->GetAirJumpsLeft() > 0);
				PressCounters.Add(Move->JumpEvents + Move->AirJumpEvents);
				PressTimes.Add(T);
				Move->JumpPressed();
				bJumpKeyDown = true;
			}
		},
		[this]
		{
			Move->JumpReleased();
			bJumpKeyDown = false;
			VirtualInput = FVector2D::ZeroVector;
			// A press that should have jumped did so if the counters went up before the next press.
			int32 Should = 0;
			int32 Ignored = 0;
			const int32 Total = Move->JumpEvents + Move->AirJumpEvents;
			for (int32 Index = 0; Index < PressTimes.Num(); ++Index)
			{
				if (!PressShouldJump[Index])
				{
					continue;
				}
				++Should;
				const int32 Next = Index + 1 < PressCounters.Num() ? PressCounters[Index + 1] : Total;
				Ignored += Next > PressCounters[Index] ? 0 : 1;
			}
			Report(FString::Printf(TEXT("RESULT walk jump presses: %d presses, %d should jump, %d ignored"), PressTimes.Num(), Should, Ignored));
		});

	AddJumpForgivenessSteps(FlatStart);

	// ---- back on the test island
	AddStep(TEXT("teleport_ice"), 0.8f, [this] { Teleport(FVector(-1390.f, 0.f, 62.f)); }, nullptr, nullptr);

	// Ice: turning around takes much longer than on snow (and never flops you onto your belly any more).
	AddStep(TEXT("ice_turn"), 1.8f, [this] { VirtualInput = FVector2D(1.f, 0.f); StartZ = 0.f; },
		[this](float T)
		{
			if (Move->IsOnIce()) { bFlag = true; }
			if (T > 0.35f && MarkA < 0.f) { MarkA = T; Value = Move->Velocity.X; VirtualInput = FVector2D(-1.f, 0.f); }
			if (MarkA > 0.f && MarkB < 0.f && Move->Velocity.X < 0.f) { MarkB = T - MarkA; }
			if (MarkA > 0.f && Move->IsBellySliding()) { StartZ = 1.f; }
			if (MarkA > 0.f && T > MarkA + 0.2f && !bShotDone) { Shot(TEXT("08_ice_turn")); bShotDone = true; }
		},
		[this]
		{
			VirtualInput = FVector2D::ZeroVector;
			Report(FString::Printf(TEXT("RESULT ice: on ice %d, speed %.0f when reversing, moving the other way after %.2f s, belly slip %d"),
				bFlag ? 1 : 0, Value, MarkB, StartZ > 0.f ? 1 : 0));
			StartZ = 0.f;
		});

	AddStep(TEXT("teleport_platforms"), 0.8f, [this] { Teleport(FVector(-50.f, 0.f, 62.f)); }, nullptr, nullptr);

	AddStep(TEXT("double_jump_onto_platform"), 2.0f, [this] { Move->JumpPressed(); },
		[this](float T)
		{
			// straight up first (platform B is up-left), then drift left after the double jump
			VirtualInput = (T > 0.42f && T < 0.9f) ? FVector2D(-0.5f, 0.f) : FVector2D::ZeroVector;
			if (!bFlag && T > 0.33f) { Move->JumpReleased(); Move->JumpPressed(); bFlag = true; }
			if (T > 0.8f) { Move->JumpReleased(); }
			if (T > 1.3f && !bShotDone) { Shot(TEXT("09_platform")); bShotDone = true; }
		},
		[this] { Report(FString::Printf(TEXT("RESULT platform: ended at %s grounded %d (platform top is z = 400)"), *Location().ToString(), Move->IsMovingOnGround())); });

	// ---- round 2: animation in both directions (the old clips only looked right running right)
	AddStep(TEXT("teleport_flat3"), 0.8f, [this, FlatStart] { Teleport(FlatStart); }, nullptr, nullptr);
	AddStep(TEXT("anim_run_right"), 1.3f, [this] { VirtualInput = FVector2D(1.f, 0.f); },
		[this](float T)
		{
			if (T > 0.6f && MarkA < 0.f) { Shot(TEXT("10_run_right_a")); MarkA = T; }
			if (T > 0.68f && MarkB < 0.f) { Shot(TEXT("10_run_right_b")); MarkB = T; }
			if (T > 0.76f && !bShotDone) { Shot(TEXT("10_run_right_c")); bShotDone = true; }
		},
		[this] { Report(FString::Printf(TEXT("RESULT run right: speed %.0f facing %.0f"), Move->Velocity.X, Move->GetFacing())); VirtualInput = FVector2D::ZeroVector; });
	AddStep(TEXT("anim_run_left"), 1.3f, [this] { VirtualInput = FVector2D(-1.f, 0.f); },
		[this](float T)
		{
			if (T > 0.6f && MarkA < 0.f) { Shot(TEXT("11_run_left_a")); MarkA = T; }
			if (T > 0.68f && MarkB < 0.f) { Shot(TEXT("11_run_left_b")); MarkB = T; }
			if (T > 0.76f && !bShotDone) { Shot(TEXT("11_run_left_c")); bShotDone = true; }
		},
		[this] { Report(FString::Printf(TEXT("RESULT run left: speed %.0f facing %.0f"), Move->Velocity.X, Move->GetFacing())); VirtualInput = FVector2D::ZeroVector; });
	AddStep(TEXT("anim_walk"), 1.0f, [this] { VirtualInput = FVector2D(0.45f, 0.f); },
		[this](float T) { if (T > 0.7f && !bShotDone) { Shot(TEXT("12_walk")); bShotDone = true; } },
		[this] { Report(FString::Printf(TEXT("RESULT walk: speed %.0f"), Move->Velocity.X)); VirtualInput = FVector2D::ZeroVector; });
	AddStep(TEXT("anim_skid"), 1.0f, [this] { VirtualInput = FVector2D(1.f, 0.f); },
		[this](float T)
		{
			if (T > 0.5f) { VirtualInput = FVector2D(-1.f, 0.f); }
			if (T > 0.56f && !bShotDone) { Shot(TEXT("13_skid")); bShotDone = true; }
		},
		[this] { VirtualInput = FVector2D::ZeroVector; });
	AddStep(TEXT("eyes"), 1.4f, [this] { VirtualInput = FVector2D::ZeroVector; },
		[this](float T)
		{
			const FVector Chest = Penguin->GetAimOrigin();
			Penguin->SetAimPoint(T < 0.7f ? Chest + FVector(-300.f, 0.f, 260.f) : Chest + FVector(320.f, 0.f, -200.f));
			if (T > 0.5f && MarkA < 0.f) { Shot(TEXT("14_eyes_up_left")); MarkA = T; }
			if (T > 1.2f && !bShotDone) { Shot(TEXT("15_eyes_down_right")); bShotDone = true; }
		},
		nullptr);
	AddFacingStep();

	// ---- round 2: every ability against a dummy penguin
	AddStep(TEXT("spawn_target"), 0.6f, [this] { SpawnTarget(); }, nullptr, nullptr);
	AddAbilityStep(EPenguinAbility::Snowball, 400.f, 1.4f);
	AddAbilityStep(EPenguinAbility::Icicle, 500.f, 1.4f);
	AddAbilityStep(EPenguinAbility::FlipperSlap, 110.f, 1.4f);
	AddAbilityStep(EPenguinAbility::FrostBreath, 260.f, 1.4f);
	AddAbilityStep(EPenguinAbility::FishHook, 600.f, 1.6f);
	AddAbilityStep(EPenguinAbility::BellyStomp, 0.f, 1.8f);
	AddAbilityStep(EPenguinAbility::IceShell, 400.f, 1.2f);
	AddAbilityStep(EPenguinAbility::SnowWarp, 500.f, 1.2f);
	AddAbilityStep(EPenguinAbility::IceWall, 250.f, 1.4f);
	AddAbilityStep(EPenguinAbility::IcebergDrop, 400.f, 2.4f);

	AddAbilityStep(EPenguinAbility::Noot, 250.f, 1.4f);
	AddAbilityStep(EPenguinAbility::IceGun, 500.f, 1.4f);
	AddAbilityStep(EPenguinAbility::ShrinkRay, 500.f, 1.6f);
	AddAbilityStep(EPenguinAbility::GrowRay, 400.f, 1.6f); // aimed at ourselves: we grow
	AddInteractionSteps();
	AddBrawlSteps();
	AddIcebergPressSteps();
	AddIslandLooksSteps(); // the blocks keep their look when blocks go (PenguinAutoTestIslandLooks.cpp)
	AddMapEdgeSteps(); // the arena's ends step down to the sea (PenguinAutoTestMapEdges.cpp)
	AddIceScaleSteps(); // the ice wall solid at once, a grown penguin's bigger ice and big blocks (PenguinAutoTestIceScale.cpp)
	AddSlideGapSteps();
	AddBotNavSteps();
	AddBotAggroSteps(); // idle bots go in, portals without loops (PenguinAutoTestBots.cpp)
	AddTeamSteps();
	AddStunAndIceShieldSteps();
	AddSnowballSteps(); // the snowball combo and its bounce off a frozen penguin's ice: PenguinAutoTestSnowball.cpp
	AddHookIceSteps();
	AddMatrixSteps();
	AddTornadoSteps(); // the tornado (the Gust slot): PenguinAutoTestTornado.cpp
	AddIceFeetLookStep();
	AddPortalLookStep();
	AddEffectsLookStep(false);
	AddEffectsLookStep(true);
	AddBackgroundLookStep();
	AddBackdropPresetStep(); // the background on High and Low: PenguinAutoTestBackdrop.cpp
	AddCooldownLookStep();
	AddNootHornLookStep();
	AddShellBubbleLookStep();
	AddTornadoLookStep(); // (PenguinAutoTestTornado.cpp)
	AddNoOutlineStep(); // no ink outline on the penguins, the horn, the cap (PenguinAutoTestOutline.cpp)
	AddMenu3DLookStep(); // the 3D title and the lobby's 3D controls (PenguinAutoTestMenu3D.cpp)
	AddMenu3DScreensLookStep(); // every other 3D page at 1920x1080 (PenguinAutoTestScreens3D.cpp)
	AddIcicleThrowSteps();
	AddIcicleFacingStep(); // the penguin faces its aim while it charges (PenguinAutoTestIcicleFacing.cpp)
	AddStunLookStep();
	AddHookIceLookStep();
	AddDeathLookStep();
	AddSizeKnockbackSteps();
	AddPickupSteps();
	AddGiantSnowballSteps(); // the giant snowball, the 17th ability (PenguinAutoTestGiantSnowball.cpp)
	AddSnowmanSteps(); // the snowman, the 18th ability (PenguinAutoTestSnowman.cpp)
	AddAbilityTuningSteps(); // the Snowman cooldown, the Icicle melt (PenguinAutoTestAbilityTuning.cpp)
	AddAntiCheatSteps(); // the LAN anti-cheat's logic (PenguinAutoTestAntiCheat.cpp)
	AddAbilityPicksSteps(); // the ability picks' deal and judge (PenguinAutoTestAbilityPicks.cpp)
	AddHostAuthoritySteps(); // the host's judge of a joiner's own moves (PenguinAutoTestHostAuthority.cpp)
	AddFlapSteps(); // Flap, the 19th ability (PenguinAutoTestFlap.cpp)
	AddIceFeetSteps(); // ice feet: a slide (PenguinAutoTestIceFeet.cpp)
	AddKillCamSteps(); // the round-winning knock-out in slow motion (PenguinAutoTestKillCam.cpp)

	// The loadout is truly random (2026-11-05): no forced deadly / knockback ability; every ability dealt about equally often.
	AddStep(TEXT("loadout_rule"), 0.1f,
		[this]
		{
			FRandomStream Rand(1234);
			int32 Bad = 0;
			int32 NoKiller = 0;
			int32 WithDeadly = 0;
			TMap<EPenguinAbility, int32> Counts;
			constexpr int32 Rolls = 6000;
			for (int32 Roll = 0; Roll < Rolls; ++Roll)
			{
				const TArray<EPenguinAbility> Loadout = UPenguinAbilityComponent::RollAbilities(Rand, 3);
				int32 Deadly = 0;
				int32 Knockback = 0;
				TSet<EPenguinAbility> Distinct;
				for (const EPenguinAbility Ability : Loadout)
				{
					Distinct.Add(Ability);
					++Counts.FindOrAdd(Ability);
					Deadly += UPenguinAbilityComponent::IsDeadly(Ability) ? 1 : 0;
					Knockback += UPenguinAbilityComponent::IsKnockback(Ability) ? 1 : 0;
				}
				Bad += (Loadout.Num() != 3 || Distinct.Num() != 3 || Distinct.Contains(EPenguinAbility::None)) ? 1 : 0;
				NoKiller += (Deadly == 0 && Knockback < 2) ? 1 : 0;
				WithDeadly += Deadly > 0 ? 1 : 0;
			}
			int32 Least = MAX_int32;
			int32 Most = 0;
			for (int32 Value = 1; Value < static_cast<int32>(EPenguinAbility::Count); ++Value)
			{
				const int32 Count = Counts.FindRef(static_cast<EPenguinAbility>(Value));
				Least = FMath::Min(Least, Count);
				Most = FMath::Max(Most, Count);
			}
			const float Mean = Rolls * 3.f / (static_cast<int32>(EPenguinAbility::Count) - 1);
			Report(FString::Printf(TEXT("RESULT loadout: %d rolls, %d not 3 different abilities (want 0), %d without a deadly ability or 2 knockback ones (want some: no forced killer), %d with a deadly one, every ability %d to %d times (mean %.0f; want each above a third of it) %s"),
				Rolls, Bad, NoKiller, WithDeadly, Least, Most, Mean, (Bad == 0 && NoKiller > Rolls / 20 && Least > Mean / 3.f) ? TEXT("ok") : TEXT("FAIL")));
		},
		nullptr, nullptr);

	// ---- every ability doubles as movement (target parked far away)
	AddMoveStep(EPenguinAbility::Snowball, FVector(0.f, 0.f, -300.f), 380.f, 0.2f, 1.1f);     // throw down in the air: hop
	AddMoveStep(EPenguinAbility::FlipperSlap, FVector(300.f, 0.f, 150.f), 0.f, 0.25f, 1.1f);  // lunge
	AddMoveStep(EPenguinAbility::FrostBreath, FVector(-60.f, 0.f, -300.f), 0.f, 0.25f, 1.4f); // frost jet
	AddMoveStep(EPenguinAbility::IceShell, FVector(0.f, 0.f, 0.f), 520.f, 0.1f, 1.2f);        // slow float
	AddMoveStep(EPenguinAbility::BellyStomp, FVector(0.f, 0.f, 0.f), 300.f, 0.1f, 1.6f);      // bounce off the slam
	AddPegStep();
	AddFloeStep();
	AddClimbStep();
	AddCollapseStep();
	AddHudCleanupSteps(); // the trait badge in the corner, the feed and FPS without plates (PenguinAutoTestHudCleanup.cpp)
	AddWoodTilingStep(); // the menus' planks never show the same grain in line (PenguinAutoTestWood.cpp)
	AddSelfBombSteps(); // the snow bomb knocks out and pushes its own thrower too - no more bomb jump (PenguinAutoTestSelfBomb.cpp)

	AddAbilityStep(EPenguinAbility::SnowBomb, 420.f, 2.2f); // it blows a hole in the strip
	AddTraitSteps(); // Traits mode (PenguinAutoTestTraits.cpp): the deal, every trait's numbers, their looks
	AddJesterHatStep(); // Jester's felt cap on High / Ultra (spring bones) / Low (PenguinAutoTestJester.cpp)
	AddTeamLookStep(); // last: it turns the course into an arena
	AddIslandSizeSteps(); // ... and after it, on that arena: the island's sizes, the camera, Snow Warp on screen (PenguinAutoTestIsland.cpp)
	AddLobbyIslandLookStep(); // the lobby island, five penguins on it (PenguinAutoTestLobbyIsland.cpp); ends in practice again
	AddMenuClickStep(); // the mouse on the 3D title and the lobby island at two sizes (PenguinAutoTestClicks.cpp); ends in practice again
	AddMenuScreensClickStep(); // the mouse and the keys on the other 3D pages (PenguinAutoTestScreens3D.cpp)
	AddBuildLimitSteps(); // the build limit on an arena (PenguinAutoTestBuildLimit.cpp; it turns the main island into one: last)
	AddRoundHazardSteps(); // the round hazards, on that arena too (PenguinAutoTestHazards.cpp)
}

void APenguinAutoTest::AddMoveStep(EPenguinAbility Ability, const FVector& AimOffset, float StartHeight, float FireTime, float Duration)
{
	const FString Name = FString(UPenguinAbilityComponent::GetInfo(Ability).Name).Replace(TEXT(" "), TEXT(""));
	AddStep(*FString::Printf(TEXT("move_%s"), *Name), Duration,
		[this, Ability, StartHeight]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			Teleport(Base + FVector(0.f, 0.f, StartHeight));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ Ability });
			bFlag = false;
			bShotDone = false;
			StartX = static_cast<float>(Penguin->GetActorLocation().X);
			StartZ = static_cast<float>(Penguin->GetActorLocation().Z);
			PeakZ = -1.e9f;
			Value = 0.f;   // Z when fired
			MarkA = 0.f;   // biggest |X| offset after firing
			MarkB = 0.f;   // fastest fall after firing (cm/s)
			MoveLowZ = 1.e9f;
			MoveBounceZ = -1.e9f;
		},
		[this, Name, AimOffset, FireTime](float T)
		{
			const FVector Loc = Penguin->GetActorLocation();
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + AimOffset);
			if (T > FireTime && !bFlag)
			{
				bFlag = true;
				Value = static_cast<float>(Loc.Z);
				const bool bUsed = Penguin->ActivateAbility(0);
				Report(FString::Printf(TEXT("move %s used: %d"), *Name, bUsed ? 1 : 0));
			}
			if (bFlag)
			{
				PeakZ = FMath::Max(PeakZ, static_cast<float>(Loc.Z));
				MarkA = FMath::Max(MarkA, FMath::Abs(static_cast<float>(Loc.X) - StartX));
				MarkB = FMath::Max(MarkB, static_cast<float>(-Move->Velocity.Z));
				if (Loc.Z < MoveLowZ)
				{
					MoveLowZ = static_cast<float>(Loc.Z);
					MoveBounceZ = MoveLowZ;
				}
				MoveBounceZ = FMath::Max(MoveBounceZ, static_cast<float>(Loc.Z));
			}
			if (bFlag && T > FireTime + 0.3f && !bShotDone)
			{
				Shot(*FString::Printf(TEXT("30_move_%s"), *Name));
				bShotDone = true;
			}
		},
		[this, Name]
		{
			Report(FString::Printf(TEXT("RESULT move %s: rose %.0f cm above the firing point, bounced %.0f cm above its lowest point, moved %.0f cm sideways, fastest fall %.0f cm/s, alive %d"),
				*Name, PeakZ - Value, MoveBounceZ - MoveLowZ, MarkA, MarkB, Penguin->IsAlive() ? 1 : 0));
		});
}

void APenguinAutoTest::AddInteractionSteps()
{
	// A bomb going off sets off the bomb next to it. The step ends before B's own 1.3 s fuse could run out,
	// so B is only gone if the chain reaction set it off.
	AddStep(TEXT("interact_BombChain"), 1.0f,
		[this]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			Teleport(Base + FVector(-600.f, 0.f, 0.f));
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Target.Get(), Base + FVector(200.f, 0.f, 60.f), FVector::ZeroVector);
			TestBombB = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Target.Get(), Base + FVector(360.f, 0.f, 60.f), FVector::ZeroVector);
			bFlag = false;
		},
		[this](float T)
		{
			if (T > 0.4f && !bFlag && TestBombA.IsValid())
			{
				bFlag = true;
				TestBombA->Detonate(0.f);
			}
		},
		[this]
		{
			const bool bSecondGone = !TestBombB.IsValid() || !TestBombB->IsFlying();
			Report(FString::Printf(TEXT("RESULT interact BombChain: second bomb went off %d"), bSecondGone ? 1 : 0));
		});

	// Frost breath freezes a lobbed bomb: its fuse stops.
	AddStep(TEXT("interact_FrostBomb"), 2.0f,
		[this]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			Teleport(Base);
			ResetTarget(Base + FVector(700.f, 0.f, 0.f));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::FrostBreath });
			TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Target.Get(), Base + FVector(260.f, 0.f, 120.f), FVector(-150.f, 0.f, 200.f));
			bFlag = false;
		},
		[this](float T)
		{
			if (TestBombA.IsValid())
			{
				Penguin->SetAimPoint(TestBombA->GetActorLocation());
			}
			if (T > 0.3f && !bFlag)
			{
				bFlag = true;
				Report(FString::Printf(TEXT("frost at bomb used: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
		},
		[this]
		{
			// Unfrozen it would have exploded after 1.3 s.
			const bool bStillThere = TestBombA.IsValid() && TestBombA->IsFlying();
			Report(FString::Printf(TEXT("RESULT interact FrostBomb: frozen bomb still ticking %d, caster alive %d"), bStillThere ? 1 : 0, Penguin->IsAlive() ? 1 : 0));
			if (TestBombA.IsValid())
			{
				TestBombA->Destroy();
			}
		});

	// Frost breath turns an incoming snowball into a slow, heavy ice ball - it must not stop dead in mid-air.
	AddStep(TEXT("interact_FrostSnowball"), 0.6f,
		[this]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			Teleport(Base);
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::FrostBreath });
			TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Target.Get(), Base + FVector(650.f, 0.f, 40.f), FVector(-2100.f, 0.f, 120.f));
			bFlag = false;
			MarkA = -1.f; // share of the throw speed left 0.2 s into the breath
			MarkB = 0.f;  // frozen then
		},
		[this](float T)
		{
			if (!bFlag)
			{
				bFlag = true;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
				Report(FString::Printf(TEXT("frost at snowball used: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
			if (MarkA < 0.f && T > 0.2f)
			{
				const bool bThere = TestBombA.IsValid() && TestBombA->IsFlying();
				MarkA = bThere ? FMath::Abs(static_cast<float>(TestBombA->GetVelocity().X)) / 2100.f : 0.f;
				MarkB = bThere && TestBombA->IsFrozen() ? 1.f : 0.f;
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT interact FrostSnowball: snowball kept %.0f%% of its speed (want about 35), frozen %d"),
				FMath::Max(MarkA, 0.f) * 100.f, MarkB > 0.f ? 1 : 0));
		});

	// Two bombs flying at each other bounce apart (they neither pass through nor glue together).
	AddStep(TEXT("interact_BombBump"), 0.9f,
		[this]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			Teleport(Base + FVector(-600.f, 0.f, 0.f));
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Target.Get(), Base + FVector(100.f, 0.f, 200.f), FVector(700.f, 0.f, 300.f));
			TestBombB = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin.Get(), Base + FVector(500.f, 0.f, 200.f), FVector(-700.f, 0.f, 300.f));
			bFlag = false;
		},
		[this](float T)
		{
			// A was thrown to the right: after the bump it flies left.
			if (TestBombA.IsValid() && TestBombA->GetVelocity().X < -100.f)
			{
				bFlag = true;
			}
		},
		[this]
		{
			const bool bBoth = TestBombA.IsValid() && TestBombB.IsValid();
			const float Ax = bBoth ? static_cast<float>(TestBombA->GetActorLocation().X) : 0.f;
			const float Bx = bBoth ? static_cast<float>(TestBombB->GetActorLocation().X) : 0.f;
			Report(FString::Printf(TEXT("RESULT interact BombBump: bounced apart %d (A turned back %d, A is %.0f cm left of B)"),
				bBoth && bFlag && Bx - Ax > 60.f ? 1 : 0, bFlag ? 1 : 0, Bx - Ax));
		});

	// Your own shots meet each other too: your icicle sets off your own bomb (after the short grace at the flipper).
	AddStep(TEXT("interact_OwnIcicleBomb"), 1.0f,
		[this]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			Teleport(Base);
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Icicle });
			// Our own bomb, lying on the strip in front of us with a long fuse left.
			TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin.Get(), Base + FVector(560.f, 0.f, 30.f), FVector::ZeroVector);
			bFlag = false;
		},
		[this](float T)
		{
			if (TestBombA.IsValid())
			{
				Penguin->SetAimPoint(TestBombA->GetActorLocation());
			}
			if (T > 0.3f && !bFlag)
			{
				bFlag = true;
				Report(FString::Printf(TEXT("icicle at own bomb used: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
		},
		[this]
		{
			// The fuse is 1.3 s: gone after 1 s only if our icicle set it off.
			const bool bGone = !TestBombA.IsValid() || !TestBombA->IsFlying();
			Report(FString::Printf(TEXT("RESULT interact OwnIcicleBomb: own bomb went off early %d, caster alive %d"), bGone ? 1 : 0, Penguin->IsAlive() ? 1 : 0));
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});

	// Your own grow ray grows your own bomb (a Bopl-style combo: a bigger bomb, a bigger blast).
	AddStep(TEXT("interact_GrowOwnBomb"), 0.9f,
		[this]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			Teleport(Base);
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::GrowRay });
			TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin.Get(), Base + FVector(300.f, 0.f, 40.f), FVector::ZeroVector);
			bFlag = false;
		},
		[this](float T)
		{
			if (TestBombA.IsValid())
			{
				Penguin->SetAimPoint(TestBombA->GetActorLocation());
			}
			if (T > 0.3f && !bFlag)
			{
				bFlag = true;
				Report(FString::Printf(TEXT("grow ray at own bomb used: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT interact GrowOwnBomb: bomb size %.2f (want 1.60), still there %d"),
				TestBombA.IsValid() ? TestBombA->GetSizeScale() : 0.f, TestBombA.IsValid() && TestBombA->IsFlying() ? 1 : 0));
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});
}

void APenguinAutoTest::PrepareBrawl(float TargetOffset, const TArray<EPenguinAbility>& Abilities)
{
	ClearAbilityLeftovers();
	APinguPickup::DestroyAll(GetWorld());
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	Teleport(Base);
	ResetTarget(Base + FVector(TargetOffset, 0.f, 0.f));
	Penguin->ResetForRound(Penguin->GetActorLocation());
	Penguin->SetInputLocked(false);
	Penguin->CancelAbilityKeys();
	Penguin->FaceDirection(1.f);
	if (Abilities.Num() > 0)
	{
		Penguin->GetAbilities()->SetAbilities(Abilities);
	}
	VirtualInput = FVector2D::ZeroVector;
	const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
	TargetHitsAtStart = TargetMove ? TargetMove->HitEvents : 0;
	TargetVelAtHit = FVector::ZeroVector;
	SlidesAtStart = Move->SlideEvents;
	TimeA = -1.f;
	TimeB = -1.f;
	bKeyDone = false;
	CountA = 0;
	CountB = 0;
	HighZ = -1.e9f;
	StartX = static_cast<float>(Location().X);
	const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
	BrokenAtStart = Flat ? Flat->GetBrokenBlocks() : 0;
}

int32 APenguinAutoTest::CountIce(float FromX, float Width, int32 Rows, int32* OutTemporary) const
{
	if (OutTemporary)
	{
		*OutTemporary = 0;
	}
	const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
	if (!Flat)
	{
		return 0;
	}
	// z 50: the row right above the strip's floor (its top is at z 0).
	const FIntPoint First = Flat->WorldToCell(FVector(FromX, 0.f, 50.f));
	const FIntPoint Last = Flat->WorldToCell(FVector(FromX + Width, 0.f, 50.f));
	int32 Count = 0;
	for (int32 X = First.X; X <= Last.X; ++X)
	{
		for (int32 Row = 0; Row < Rows; ++Row)
		{
			const FIntPoint Cell(X, First.Y + Row);
			if (Flat->GetBlock(Cell.X, Cell.Y) == EBlockKind::Ice)
			{
				++Count;
				if (OutTemporary && Flat->IsTemporaryCell(Cell))
				{
					++*OutTemporary;
				}
			}
		}
	}
	return Count;
}

void APenguinAutoTest::AddBrawlSteps()
{
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	// ---- Ctrl: the belly slide. About six blocks over the snow in about two thirds of a second, then up again.
	AddStep(TEXT("move_BellySlide"), 1.6f,
		[this] { PrepareBrawl(2400.f, {}); },
		[this](float T)
		{
			if (!bFlag && T > 0.4f)
			{
				bFlag = true;
				StartX = static_cast<float>(Location().X);
				TimeA = T;
				Penguin->BellySlide();
				return;
			}
			if (!bFlag)
			{
				return;
			}
			if (Move->IsBellySliding())
			{
				MarkB = FMath::Max(MarkB, FMath::Abs(static_cast<float>(Move->Velocity.X)));
				Value = static_cast<float>(Location().X) - StartX;
			}
			else if (TimeB < 0.f && Move->SlideEvents != SlidesAtStart)
			{
				TimeB = T - TimeA; // the slide is over: back on its feet
			}
			if (T > 0.75f && !bShotDone)
			{
				Shot(TEXT("36_move_BellySlide"));
				bShotDone = true;
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT move BellySlide: slides %d, slid %.0f cm in %.2f s (want about 600 cm in 0.66 s), top speed %.0f cm/s, at rest %.0f cm from the start"),
				Move->SlideEvents - SlidesAtStart, Value, TimeB, MarkB, static_cast<float>(Location().X) - StartX));
		});

	// ---- A slide that has gone three blocks knocks the penguin it runs into away (along the slide, a little up) and stops.
	AddStep(TEXT("interact_SlideHit"), 1.6f,
		[this] { PrepareBrawl(450.f, {}); Value = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) : 0.f; },
		[this](float T)
		{
			if (!bFlag && T > 0.4f)
			{
				bFlag = true;
				StartX = static_cast<float>(Location().X);
				Penguin->BellySlide();
				return;
			}
			const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (bFlag && TargetMove && TimeA < 0.f && TargetMove->HitEvents != TargetHitsAtStart)
			{
				TimeA = T;
				MarkA = static_cast<float>(Location().X) - StartX;
				TargetVelAtHit = TargetMove->Velocity;
			}
			if (TimeA > 0.f && T > TimeA + 0.1f && !bShotDone)
			{
				Shot(TEXT("37_interact_SlideHit"));
				bShotDone = true;
			}
		},
		[this]
		{
			const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			Report(FString::Printf(TEXT("RESULT interact SlideHit: hits %d after sliding %.0f cm (want 1, after 300 or more), the dummy sent off at %.0f cm/s along and %.0f up, dummy moved %.0f cm, slider still sliding %d"),
				TargetMove ? TargetMove->HitEvents - TargetHitsAtStart : 0, MarkA, TargetVelAtHit.X, TargetVelAtHit.Z,
				Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) - Value : 0.f, Move->IsBellySliding() ? 1 : 0));
		});

	// ---- Running into somebody before three blocks is only a bump: no hit, the slide stops and bounces back a little.
	AddStep(TEXT("interact_SlideBump"), 1.2f,
		[this] { PrepareBrawl(250.f, {}); Value = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) : 0.f; },
		[this](float T)
		{
			if (!bFlag && T > 0.4f)
			{
				bFlag = true;
				StartX = static_cast<float>(Location().X);
				Penguin->BellySlide();
				return;
			}
			if (bFlag && TimeA < 0.f && Move->SlideEvents != SlidesAtStart && !Move->IsBellySliding())
			{
				TimeA = T;
				MarkA = static_cast<float>(Location().X) - StartX; // where it stopped
				MarkB = static_cast<float>(Move->Velocity.X);      // bounced back: below 0
			}
		},
		[this]
		{
			const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			Report(FString::Printf(TEXT("RESULT interact SlideBump: hits %d (want 0), the slide stopped after %.0f cm, slider speed then %.0f cm/s (bounced back: below 0), dummy moved %.0f cm"),
				TargetMove ? TargetMove->HitEvents - TargetHitsAtStart : 0, MarkA, MarkB,
				Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) - Value : 0.f));
		});

	// ---- A slide kicks a lying snow bomb along ahead of it (the bomb is the slider's now: its blast spares the slider).
	AddStep(TEXT("interact_SlideKicksBomb"), 1.2f,
		[this, Base]
		{
			PrepareBrawl(2400.f, {});
			TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Target.Get(), Base + FVector(380.f, 0.f, 30.f), FVector::ZeroVector);
			MarkA = 0.f;
		},
		[this](float T)
		{
			if (!bFlag && T > 0.35f)
			{
				bFlag = true;
				Penguin->BellySlide();
			}
			if (TestBombA.IsValid())
			{
				MarkA = FMath::Max(MarkA, static_cast<float>(TestBombA->GetVelocity().X));
				CountA = TestBombA->GetShooter() == Penguin.Get() ? 1 : CountA;
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT interact SlideKicksBomb: bomb kicked to %.0f cm/s (want 950 or more), now the slider's %d, slider alive %d"),
				MarkA, CountA, Penguin->IsAlive() ? 1 : 0));
			if (TestBombA.IsValid())
			{
				TestBombA->Destroy();
			}
		});

	// (Ice feet: the icefeet_ steps in PenguinAutoTestIceFeet.cpp - the old move_IceFeet measured walking up to speed on ice feet.)

	// ---- Size rays on a key: held, the ray is used on yourself (one size step, nothing shot); tapped, it shoots on the release.
	AddStep(TEXT("ability_GrowRayHold"), 1.3f,
		[this] { PrepareBrawl(600.f, { EPenguinAbility::GrowRay }); },
		[this](float T)
		{
			if (Target.IsValid())
			{
				Penguin->SetAimPoint(Target->GetAimOrigin()); // a tap would shoot the dummy
			}
			if (!bFlag && T > 0.2f)
			{
				bFlag = true;
				TimeA = T;
				Penguin->SetAbilityKeyDown(0, true);
			}
			if (bFlag && TimeB < 0.f && Penguin->GetSizeTarget() > 1.01f)
			{
				TimeB = T - TimeA;
			}
			if (bFlag && !bKeyDone && T > 1.f)
			{
				bKeyDone = true;
				Penguin->SetAbilityKeyDown(0, false); // the release does nothing more
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT ability GrowRayHold: own size target %.2f (want 1.30) %.2f s after the press (want 0.45), uses %d (want 1), dummy size target %.2f (want 1: nothing shot)"),
				Penguin->GetSizeTarget(), TimeB, Penguin->GetAbilities()->GetUseCount(0), Target.IsValid() ? Target->GetSizeTarget() : 0.f));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			if (Target.IsValid())
			{
				Target->ResetForRound(Target->GetActorLocation());
			}
		});

	AddStep(TEXT("ability_ShrinkRayTap"), 1.2f,
		[this] { PrepareBrawl(600.f, { EPenguinAbility::ShrinkRay }); },
		[this](float T)
		{
			if (Target.IsValid())
			{
				Penguin->SetAimPoint(Target->GetAimOrigin());
			}
			if (!bFlag && T > 0.2f)
			{
				bFlag = true;
				Penguin->SetAbilityKeyDown(0, true);
			}
			if (bFlag && !bKeyDone && T > 0.35f)
			{
				bKeyDone = true;
				CountA = Penguin->GetAbilities()->GetUseCount(0); // used while still held? (must not be)
				Penguin->SetAbilityKeyDown(0, false);
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT ability ShrinkRayTap: uses while held %d (want 0), uses after the release %d (want 1), own size target %.2f (want 1), dummy size target %.2f (want 0.71)"),
				CountA, Penguin->GetAbilities()->GetUseCount(0), Penguin->GetSizeTarget(), Target.IsValid() ? Target->GetSizeTarget() : 0.f));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			if (Target.IsValid())
			{
				Target->ResetForRound(Target->GetActorLocation());
			}
		});

	// ---- The noot is a shotgun: three times a gust standing inside the other penguin, little at the edge of its reach.
	for (const float Offset : { 40.f, 200.f, 400.f })
	{
		AddStep(*FString::Printf(TEXT("ability_Noot_%.0fcm"), Offset), 0.9f,
			[this, Offset] { PrepareBrawl(Offset, { EPenguinAbility::Noot }); },
			[this](float T)
			{
				if (!bFlag && T > 0.25f)
				{
					bFlag = true;
					Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f)); // level, along the strip
					Penguin->ActivateAbility(0);
				}
				const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
				if (bFlag && TargetMove && TargetMove->HitEvents != TargetHitsAtStart)
				{
					MarkB = FMath::Max(MarkB, FMath::Abs(static_cast<float>(TargetMove->Velocity.X)));
					Value = FMath::Max(Value, static_cast<float>(TargetMove->Velocity.Z));
				}
			},
			[this, Offset]
			{
				const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
				Report(FString::Printf(TEXT("RESULT ability Noot at %.0f cm: hits %d, the dummy flew off at %.0f cm/s sideways and %.0f up, dummy KO %d"),
					Offset, TargetMove ? TargetMove->HitEvents - TargetHitsAtStart : 0, FMath::Max(MarkB, 0.f), Value,
					Target.IsValid() && Target->IsKnockedOut() ? 1 : 0));
			});
	}

	// ---- The fish hook reaches twice as far and throws the penguin it catches right over your head.
	AddStep(TEXT("interact_HookFar"), 2.6f,
		[this] { PrepareBrawl(2500.f, { EPenguinAbility::FishHook }); },
		[this](float T)
		{
			if (!bFlag && Target.IsValid())
			{
				Penguin->SetAimPoint(Target->GetAimOrigin());
			}
			if (!bFlag && T > 0.25f)
			{
				bFlag = true;
				TimeA = T;
				Report(FString::Printf(TEXT("hook thrown 25 blocks: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
			const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (bFlag && TargetMove && TimeB < 0.f && TargetMove->HitEvents != TargetHitsAtStart)
			{
				TimeB = T - TimeA; // caught
			}
			if (Target.IsValid() && TimeB >= 0.f)
			{
				HighZ = FMath::Max(HighZ, static_cast<float>(Target->GetActorLocation().Z));
			}
			if (TimeB >= 0.f && T > TimeA + TimeB + 0.45f && !bShotDone)
			{
				Shot(TEXT("38_interact_HookFar"));
				bShotDone = true;
			}
		},
		[this]
		{
			const float Behind = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X - Location().X) : 0.f;
			Report(FString::Printf(TEXT("RESULT interact HookFar: caught %d after %.2f s, the dummy ended %.0f cm from the thrower (below 0: thrown over to the other side), flew %.0f cm above the thrower, dummy KO %d"),
				TimeB >= 0.f ? 1 : 0, TimeB, Behind, HighZ > -1.e8f ? HighZ - static_cast<float>(Location().Z) : 0.f,
				Target.IsValid() && Target->IsKnockedOut() ? 1 : 0));
		});

	// ---- The hook grabs things too: an ability bubble is reeled in and taken.
	AddStep(TEXT("interact_HookBubble"), 2.2f,
		[this, Base]
		{
			PrepareBrawl(2400.f, { EPenguinAbility::FishHook, EPenguinAbility::Snowball, EPenguinAbility::Icicle });
			TestPickup = APinguPickup::Spawn(GetWorld(), EPenguinAbility::Noot, Base + FVector(1400.f, 0.f, 40.f));
		},
		[this](float T)
		{
			if (!bFlag && TestPickup.IsValid())
			{
				Penguin->SetAimPoint(TestPickup->GetIconLocation());
			}
			if (!bFlag && T > 0.6f)
			{
				bFlag = true;
				TimeA = T;
				Report(FString::Printf(TEXT("hook thrown at the bubble: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
			if (bFlag && TimeB < 0.f && !TestPickup.IsValid())
			{
				TimeB = T - TimeA;
			}
		},
		[this]
		{
			const UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			Report(FString::Printf(TEXT("RESULT interact HookBubble: the bubble reeled in and taken %d after %.2f s, middle slot now Noot %d"),
				TestPickup.IsValid() ? 0 : 1, TimeB, Abilities->GetAbility(UPenguinAbilityComponent::PickupSlot) == EPenguinAbility::Noot ? 1 : 0));
		});

	// ---- Ice walls stay until broken, one block per hit: a snowball breaks exactly one.
	AddStep(TEXT("island_IceWallBreak"), 2.0f,
		[this] { PrepareBrawl(2400.f, { EPenguinAbility::IceWall, EPenguinAbility::Snowball }); },
		[this, Base](float T)
		{
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			if (!bFlag && T > 0.2f)
			{
				bFlag = true;
				Penguin->SetAimPoint(Base + FVector(300.f, 0.f, 0.f));
				Report(FString::Printf(TEXT("ice wall raised: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
			if (bFlag && !bKeyDone && T > 1.f && Flat)
			{
				bKeyDone = true;
				CountA = CountIce(static_cast<float>(Base.X) + 100.f, 400.f, 6, &CountB);
				BrokenAtStart = Flat->GetBrokenBlocks();
				Penguin->SetAimPoint(Flat->CellToWorld(Flat->WorldToCell(FVector(Base.X + 300.f, 0.f, 150.f)))); // the wall's middle
				Report(FString::Printf(TEXT("snowball at the wall: %d"), Penguin->ActivateAbility(1) ? 1 : 0));
			}
			if (bKeyDone && T > 1.4f && !bShotDone)
			{
				Shot(TEXT("39_island_IceWallBreak"));
				bShotDone = true;
			}
		},
		[this, Base]
		{
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			Report(FString::Printf(TEXT("RESULT island IceWallBreak: wall blocks %d (want 3), temporary %d (want 0: it stays), a snowball broke %d (want 1), %d left"),
				CountA, CountB, Flat ? Flat->GetBrokenBlocks() - BrokenAtStart : 0, CountIce(static_cast<float>(Base.X) + 100.f, 400.f, 6, nullptr)));
		});

	// ---- Iceberg ice falls like sand: down its column, stacking, falling again when the block under it breaks.
	AddStep(TEXT("island_SandStack"), 2.0f,
		[this, Base]
		{
			PrepareBrawl(2400.f, {});
			Teleport(Base + FVector(-500.f, 0.f, 0.f)); // out of the way
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			SandColumn = Flat ? Flat->WorldToCell(FVector(Base.X + 300.f, 0.f, 50.f)) : FIntPoint::ZeroValue;
			if (Flat)
			{
				Flat->AddSandBlocks({ FIntPoint(SandColumn.X, SandColumn.Y + 4) }, EBlockKind::Ice); // four blocks up in the air
			}
		},
		[this, Base](float T)
		{
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			if (!Flat)
			{
				return;
			}
			const bool bBottom = Flat->GetBlock(SandColumn.X, SandColumn.Y) == EBlockKind::Ice;
			const bool bSecond = Flat->GetBlock(SandColumn.X, SandColumn.Y + 1) == EBlockKind::Ice;
			if (TimeA < 0.f && bBottom)
			{
				TimeA = T; // the first block is down on the floor
			}
			if (!bFlag && T > 0.6f)
			{
				bFlag = true;
				TimeB = T;
				Flat->AddSandBlocks({ FIntPoint(SandColumn.X, SandColumn.Y + 5) }, EBlockKind::Ice);
				return;
			}
			if (bFlag && !bKeyDone && MarkA < 0.f && bSecond)
			{
				MarkA = T - TimeB; // the second one stacked on the first
			}
			if (bFlag && !bKeyDone && T > 1.2f)
			{
				bKeyDone = true;
				TimeB = T;
				CountA = Flat->BreakIceAt(Flat->CellToWorld(SandColumn)) ? 1 : 0; // the bottom one breaks: the top one comes down
				return;
			}
			if (bKeyDone && MarkB < 0.f && bBottom && !bSecond)
			{
				MarkB = T - TimeB;
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT island SandStack: first block down after %.2f s (want 0.33), the second stacked on it after %.2f s, bottom broken %d, the top one fell into its place after %.2f s (shakes 0.25 s first: want about 0.4)"),
				TimeA, MarkA, CountA, MarkB));
		});

	// ---- Sand coming down right onto a penguin (its middle under the block) pushes it down and crushes it against the
	// floor: knocked out (reason Crushed), never stuck inside the ice while it is in.
	AddStep(TEXT("island_SandOnPenguin"), 1.4f,
		[this, Base]
		{
			PrepareBrawl(2400.f, {});
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			SandColumn = Flat ? Flat->WorldToCell(FVector(Location().X, 0.f, 50.f)) : FIntPoint::ZeroValue;
			if (Flat)
			{
				// Right under the middle of the column (the strip's base is on a column's edge).
				Teleport(FVector(Flat->CellToWorld(SandColumn).X, 0.f, Base.Z));
				Flat->AddSandBlocks({ FIntPoint(SandColumn.X, SandColumn.Y + 5) }, EBlockKind::Ice);
			}
			MarkA = static_cast<float>(Location().Z);
		},
		[this, Base](float T)
		{
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			if (Penguin->IsAlive())
			{
				CountB += Flat && Flat->IsSolidAt(Location()) ? 1 : 0; // frames inside a block while still in
				return;
			}
			if (TimeA < 0.f)
			{
				TimeA = T; // crushed
				CountA = Penguin->GetKOReason() == EPenguinKOReason::Crushed ? 1 : 0;
			}
		},
		[this, Base]
		{
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			const bool bDown = Flat && Flat->GetBlock(SandColumn.X, SandColumn.Y) == EBlockKind::Ice;
			const bool bOk = bDown && TimeA > 0.f && CountA == 1 && CountB == 0;
			Report(FString::Printf(TEXT("RESULT island SandOnPenguin: the block came down %d, penguin crushed %d after %.2f s (want about 0.32), reason crushed %d, frames inside a block while in %d (want 0): %s"),
				bDown ? 1 : 0, TimeA > 0.f ? 1 : 0, TimeA, CountA, CountB, bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	// ---- The iceberg no longer blows a crater: it lands as four blocks (2 x 2) of that sand ice.
	AddStep(TEXT("island_IcebergSand"), 2.6f,
		[this] { PrepareBrawl(2400.f, { EPenguinAbility::IcebergDrop }); },
		[this, Base](float T)
		{
			if (!bFlag && T > 0.2f)
			{
				bFlag = true;
				Penguin->SetAimPoint(Base + FVector(450.f, 0.f, 0.f));
				Report(FString::Printf(TEXT("iceberg dropped: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
			if (T > 2.f && !bShotDone)
			{
				Shot(TEXT("40_island_IcebergSand"));
				bShotDone = true;
			}
		},
		[this, Base]
		{
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			Report(FString::Printf(TEXT("RESULT island IcebergSand: ice blocks %d (want 4), blocks broken %d (want 0), sand still falling %d"),
				CountIce(static_cast<float>(Base.X) + 150.f, 600.f, 4, nullptr), Flat ? Flat->GetBrokenBlocks() - BrokenAtStart : 0,
				Flat && Flat->IsSandFalling() ? 1 : 0));
		});
}

void APenguinAutoTest::AddIcebergPressSteps()
{
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	// ---- A penguin right under a falling iceberg (its middle within the iceberg's columns) is pushed down with it - just
	// under its bottom - and crushed once that presses it against the floor: knocked out there (reason Iceberg), not before.
	AddStep(TEXT("brawl_IcebergCarry"), 2.4f,
		[this, Base]
		{
			SpawnTarget();
			PrepareBrawl(2400.f, {});
			// Ours (its owner is never touched): over the strip 600 cm right of our penguin, the dummy put under it in the air.
			TestIceberg = AIcebergDrop::Drop(GetWorld(), Penguin.Get(), static_cast<float>(Base.X) + 600.f, 0.f);
			Value = -1.f;
		},
		[this](float T)
		{
			APenguinCharacter* Dummy = Target.Get();
			const AIcebergDrop* Berg = TestIceberg.Get();
			if (!Dummy)
			{
				return;
			}
			const float HalfHeight = Dummy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			if (Berg && !bFlag && Berg->IsFalling() && Berg->GetActorLocation().Z < 800.f)
			{
				bFlag = true;
				Dummy->NetTeleport(FVector((Berg->GetLeftX() + Berg->GetRightX()) * 0.5f, 0.f, 560.f), FVector::ZeroVector, true);
				return;
			}
			if (!bFlag)
			{
				return;
			}
			if (Dummy->IsAlive())
			{
				// Carried: in the air, its head just under the iceberg's bottom (the iceberg keeps it 1 cm under; its own fall
				// at the iceberg's speed may take it up to a frame's fall further) while both go down.
				const float Gap = Berg ? static_cast<float>(Berg->GetActorLocation().Z - (Dummy->GetActorLocation().Z + HalfHeight)) : 1000.f;
				const UPenguinMovementComponent* DummyMove = Dummy->GetPenguinMovement();
				if (Gap >= -1.f && Gap <= 80.f && DummyMove && !DummyMove->IsMovingOnGround())
				{
					++CountA;
					HighZ = FMath::Max(HighZ, static_cast<float>(-DummyMove->Velocity.Z)); // its fastest way down
					if (CountA == 3)
					{
						Shot(TEXT("54_iceberg_carry"));
					}
				}
				return;
			}
			if (TimeA < 0.f)
			{
				TimeA = T;
				Value = static_cast<float>(Dummy->GetActorLocation().Z) - HalfHeight; // its feet when crushed
				CountB = Dummy->GetKOReason() == EPenguinKOReason::Iceberg ? 1 : 0;
			}
		},
		[this]
		{
			const bool bOk = CountA >= 3 && TimeA > 0.f && Value >= -5.f && Value < 15.f && CountB == 1;
			Report(FString::Printf(TEXT("RESULT iceberg carry: carried down under it %d frames (want 3 or more), down at up to %.0f cm/s (the iceberg falls 2000-4000), knocked out %d with its feet %.0f cm above the floor (want under 15: pressed onto it), reason iceberg %d: %s"),
				CountA, HighZ, TimeA > 0.f ? 1 : 0, Value, CountB, bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	// ---- A penguin only at the iceberg's side (its middle 5 cm outside, its capsule 25 cm in) is shoved out of the way:
	// not hurt, never inside the ice, and the iceberg lands as its four blocks.
	AddStep(TEXT("brawl_IcebergSide"), 2.2f,
		[this, Base]
		{
			SpawnTarget();
			PrepareBrawl(2400.f, {});
			TestIceberg = AIcebergDrop::Drop(GetWorld(), Penguin.Get(), static_cast<float>(Base.X) + 600.f, 0.f);
			if (const AIcebergDrop* Berg = TestIceberg.Get())
			{
				MarkA = Berg->GetLeftX();
				ResetTarget(FVector(Berg->GetRightX() + 5.f, 0.f, Base.Z));
			}
			Value = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) : 0.f;
		},
		[this](float)
		{
			const AIcebergDrop* Berg = TestIceberg.Get();
			if (Target.IsValid() && !Target->IsAlive())
			{
				CountB = 1;
			}
			if (Berg && !bShotDone && Berg->IsFalling() && Berg->GetActorLocation().Z < 130.f)
			{
				bShotDone = true;
				Shot(TEXT("54_iceberg_side"));
			}
		},
		[this, Base]
		{
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			const float Shoved = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) - Value : 0.f;
			const bool bInside = Flat && Target.IsValid() && Flat->IsSolidAt(Target->GetActorLocation());
			const int32 Ice = CountIce(MarkA, 200.f, 2, nullptr);
			const bool bOk = CountB == 0 && Shoved >= 20.f && !bInside && Ice == 4;
			Report(FString::Printf(TEXT("RESULT iceberg side: dummy knocked out %d (want 0), shoved %.0f cm to the right (want 20 or more), inside a block %d (want 0), the iceberg's ice blocks %d (want 4): %s"),
				CountB, Shoved, bInside ? 1 : 0, Ice, bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	// ---- Falling sand ice: the dummy right under a block is crushed (reason Crushed); our penguin with its middle 5 cm
	// beside another falling block is shoved aside - not hurt, never inside.
	AddStep(TEXT("island_SandCrush"), 1.4f,
		[this, Base]
		{
			SpawnTarget();
			PrepareBrawl(650.f, {});
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			if (!Flat || !Target.IsValid())
			{
				return;
			}
			SandColumn = Flat->WorldToCell(FVector(Target->GetActorLocation().X, 0.f, 50.f));
			const FIntPoint Beside(SandColumn.X - 3, SandColumn.Y);
			// Our penguin's middle 5 cm left of that column (its capsule 25 cm into it).
			Teleport(FVector(Flat->CellToWorld(Beside).X - Flat->CellSize * 0.5f - 5.f, 0.f, Base.Z));
			StartX = static_cast<float>(Location().X);
			Flat->AddSandBlocks({ FIntPoint(SandColumn.X, SandColumn.Y + 5), FIntPoint(Beside.X, Beside.Y + 5) }, EBlockKind::Ice);
		},
		[this, Base](float T)
		{
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			if (Penguin->IsAlive())
			{
				CountB += Flat && Flat->IsSolidAt(Location()) ? 1 : 0;
			}
			else
			{
				MarkB = 1.f; // our penguin went out
			}
			if (Target.IsValid() && !Target->IsAlive() && TimeA < 0.f)
			{
				TimeA = T;
				CountA = Target->GetKOReason() == EPenguinKOReason::Crushed ? 1 : 0;
			}
		},
		[this, Base]
		{
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			const bool bDown = Flat && Flat->GetBlock(SandColumn.X, SandColumn.Y) == EBlockKind::Ice;
			const bool bBesideDown = Flat && Flat->GetBlock(SandColumn.X - 3, SandColumn.Y) == EBlockKind::Ice;
			const float Shoved = StartX - static_cast<float>(Location().X);
			const bool bOk = TimeA > 0.f && CountA == 1 && bDown && MarkB < 0.f && Shoved >= 15.f && CountB == 0 && bBesideDown;
			Report(FString::Printf(TEXT("RESULT island SandCrush: dummy under the block crushed %d after %.2f s (want about 0.32), reason crushed %d, its block on the floor %d; beside the other one our penguin knocked out %d (want 0), shoved %.0f cm left (want 15 or more), frames inside a block %d (want 0), that block on the floor %d: %s"),
				TimeA > 0.f ? 1 : 0, TimeA, CountA, bDown ? 1 : 0, MarkB > 0.f ? 1 : 0, Shoved, CountB, bBesideDown ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	// ---- (2026-10-20) An iceberg coming down with one column on a ledge stops there; the ice of its other column, over a
	// drop, falls on down like sand and crushes only a penguin it really comes down on. It used to knock out whoever was
	// under that column the very moment the other column landed, however far above them the ice still was, so nobody
	// could get out from under it. The ledge: three rock blocks under the iceberg's left column (it is dropped at
	// Base.X + 600: columns Base.X + 500 .. 700), so it stops with its bottom 300 cm up; the ice of its right column falls
	// on from there to the floor (onto the head of a penguin standing there after about 0.22 s).
	struct FOverhangState
	{
		float ColumnMinX = 0.f;    // the overhanging (right) column's left side
		float LandT = -1.f;        // the iceberg stopped on the ledge (it is gone: its ice is blocks and falling sand now)
		float IceAtLand = -1.f;    // the overhanging ice's bottom then ...
		float HeadAtLand = 0.f;    // ... and the top of the head of the penguin under it
		float MiddleAtLand = 0.f;  // that penguin's middle then, X from the overhanging column's left side
		bool bInAtLand = false;    // that penguin was still in then
		float LastHead = 0.f;      // the head's top in the last frame it was in
		float KOT = -1.f;          // knocked out
		float IceAtKO = -1.f;      // the falling ice's bottom then
		float ReachT = -1.f;       // escape: the ice came down past the height of the head where it stood
		float ClearAtReach = 0.f;  // escape: how far (cm) its capsule was out of the column then (negative: still in it)
		int32 Inside = 0;          // frames inside a block while in
	};
	const TSharedRef<FOverhangState> Overhang = MakeShared<FOverhangState>();
	const auto BuildOverhang = [this, Base, Overhang](APenguinCharacter* Dropper)
	{
		*Overhang = FOverhangState();
		ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
		if (!Flat)
		{
			return;
		}
		const FIntPoint Ledge = Flat->WorldToCell(FVector(Base.X + 550.f, 0.f, 50.f));
		Flat->AddBlocks({ Ledge, FIntPoint(Ledge.X, Ledge.Y + 1), FIntPoint(Ledge.X, Ledge.Y + 2) }, EBlockKind::Rock, 0.f);
		SandColumn = FIntPoint(Ledge.X + 1, Ledge.Y); // the overhanging column, on the floor
		Overhang->ColumnMinX = static_cast<float>(Flat->CellToWorld(SandColumn).X) - Flat->CellSize * 0.5f;
		TestIceberg = AIcebergDrop::Drop(GetWorld(), Dropper, static_cast<float>(Base.X) + 600.f, 0.f);
	};
	// Every frame: when the iceberg stopped (it is gone then) and where the overhanging column's ice is.
	const auto TrackOverhang = [this, Base, Overhang](const APenguinCharacter* Under, float T, float& OutIceBottom) -> bool
	{
		const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
		if (!Flat || !Under)
		{
			return false;
		}
		const bool bIce = Flat->GetFallingSandBottom(Overhang->ColumnMinX, Overhang->ColumnMinX + Flat->CellSize, OutIceBottom);
		if (Overhang->LandT < 0.f && !TestIceberg.IsValid())
		{
			const FVector Loc = Under->GetActorLocation();
			Overhang->LandT = T;
			Overhang->IceAtLand = bIce ? OutIceBottom : -1.f;
			Overhang->HeadAtLand = static_cast<float>(Loc.Z) + Under->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			Overhang->MiddleAtLand = static_cast<float>(Loc.X) - Overhang->ColumnMinX;
			Overhang->bInAtLand = Under->IsAlive();
		}
		return bIce;
	};

	// (b) The dummy stays put under the overhanging column: still in when the iceberg stops on the ledge, knocked out
	// (reason Iceberg: it is the iceberg's ice) only when the falling ice reaches its head.
	AddStep(TEXT("brawl_IcebergOverhang"), 2.2f,
		[this, BuildOverhang]
		{
			SpawnTarget();
			PrepareBrawl(650.f, {}); // the dummy under the middle of the overhanging column; we (we drop it) far left of the ledge
			BuildOverhang(Penguin.Get());
		},
		[this, Base, Overhang, TrackOverhang](float T)
		{
			APenguinCharacter* Dummy = Target.Get();
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			float IceBottom = -1.f;
			const bool bIce = TrackOverhang(Dummy, T, IceBottom);
			if (!Dummy || !Flat)
			{
				return;
			}
			if (Dummy->IsAlive())
			{
				const FVector Loc = Dummy->GetActorLocation();
				Overhang->LastHead = static_cast<float>(Loc.Z) + Dummy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
				Overhang->Inside += Flat->IsSolidAt(Loc) ? 1 : 0;
				// On its way down: the iceberg's left column on the ledge, the right column's ice coming down, the dummy under it.
				if (Overhang->LandT >= 0.f && !bShotDone && bIce && IceBottom < Overhang->LastHead + 100.f)
				{
					bShotDone = true;
					Shot(TEXT("54_iceberg_overhang"));
				}
				return;
			}
			if (Overhang->KOT < 0.f)
			{
				Overhang->KOT = T;
				Overhang->IceAtKO = bIce ? IceBottom : -1.f;
				CountA = Dummy->GetKOReason() == EPenguinKOReason::Iceberg ? 1 : 0;
			}
		},
		[this, Overhang]
		{
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			// How far the ice had to fall onto the head, and how long that takes it from rest (the sand's fall).
			const float Gap = Overhang->IceAtLand - Overhang->HeadAtLand;
			const float Expected = Gap > 0.f ? FMath::Sqrt(2.f * Gap / ABlockIsland::FallingBlockGravity) : 0.f;
			const float Wait = Overhang->KOT >= 0.f && Overhang->LandT >= 0.f ? Overhang->KOT - Overhang->LandT : -1.f;
			// The ice's bottom against the head's top when it was knocked out: at most a frame's fall into it.
			const float Reach = Overhang->IceAtKO - Overhang->LastHead;
			const bool bDown = Flat && Flat->GetBlock(SandColumn.X, SandColumn.Y) == EBlockKind::Ice && Flat->GetBlock(SandColumn.X, SandColumn.Y + 1) == EBlockKind::Ice;
			const int32 Ice = CountIce(Overhang->ColumnMinX - 50.f, 100.f, 5, nullptr);
			const bool bOk = Overhang->bInAtLand && Gap > 150.f && Wait >= Expected - 0.05f && Wait <= Expected + 0.1f && Overhang->IceAtKO >= 0.f
				&& Reach <= 1.f && Reach >= -60.f && CountA == 1 && Overhang->Inside == 0 && bDown && Ice == 4;
			Report(FString::Printf(TEXT("RESULT iceberg overhang: stopped on the ledge at %.2f s with the dummy under its other column still in %d (want 1: it used to be knocked out right then), the ice %.0f cm above its head then; "
				"knocked out %d %.2f s after that (want about %.2f: the ice's fall onto its head), the ice's bottom %.0f cm from its head's top then (want -60 .. 1: it really came down on it), reason iceberg %d, "
				"frames inside a block %d (want 0), that column's ice on the floor %d, the iceberg's ice blocks %d (want 4): %s"),
				Overhang->LandT, Overhang->bInAtLand ? 1 : 0, Gap, Overhang->KOT >= 0.f ? 1 : 0, Wait, Expected, Reach, CountA, Overhang->Inside, bDown ? 1 : 0, Ice,
				bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	// (a) Our penguin under the overhanging column runs out from under it right when the iceberg stops on the ledge (the
	// dummy dropped it): the ice comes down behind it, it stays in.
	AddStep(TEXT("brawl_IcebergOverhangEscape"), 2.2f,
		[this, Base, Overhang, BuildOverhang]
		{
			SpawnTarget();
			PrepareBrawl(100.f, {}); // the dummy (it drops the iceberg) left of the ledge
			BuildOverhang(Target.Get());
			Teleport(FVector(Overhang->ColumnMinX + 50.f, 0.f, Base.Z)); // our penguin under the middle of the overhanging column
		},
		[this, Base, Overhang, TrackOverhang](float T)
		{
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			const bool bLandedBefore = Overhang->LandT >= 0.f;
			float IceBottom = -1.f;
			const bool bIce = TrackOverhang(Penguin.Get(), T, IceBottom);
			if (!Flat)
			{
				return;
			}
			if (!bLandedBefore && Overhang->LandT >= 0.f)
			{
				VirtualInput = FVector2D(1.f, 0.f); // now: out from under it to the right (away from the ledge)
			}
			if (!Penguin->IsAlive())
			{
				Overhang->KOT = Overhang->KOT < 0.f ? T : Overhang->KOT;
				return;
			}
			const FVector Loc = Location();
			Overhang->Inside += Flat->IsSolidAt(Loc) ? 1 : 0;
			if (Overhang->LandT >= 0.f && Overhang->ReachT < 0.f && bIce && IceBottom <= Overhang->HeadAtLand)
			{
				Overhang->ReachT = T;
				Overhang->ClearAtReach = static_cast<float>(Loc.X) - Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius() - (Overhang->ColumnMinX + Flat->CellSize);
				Shot(TEXT("54_iceberg_overhang_escape"));
			}
		},
		[this, Overhang]
		{
			VirtualInput = FVector2D::ZeroVector;
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			const float Width = Flat ? Flat->CellSize : 100.f;
			const bool bUnder = Overhang->bInAtLand && Overhang->MiddleAtLand > 0.f && Overhang->MiddleAtLand < Width;
			const float Reached = Overhang->ReachT >= 0.f ? Overhang->ReachT - Overhang->LandT : -1.f;
			const bool bDown = Flat && Flat->GetBlock(SandColumn.X, SandColumn.Y) == EBlockKind::Ice && Flat->GetBlock(SandColumn.X, SandColumn.Y + 1) == EBlockKind::Ice;
			const bool bOk = bUnder && Overhang->KOT < 0.f && Penguin->IsAlive() && Overhang->ReachT >= 0.f && Overhang->Inside == 0 && bDown;
			Report(FString::Printf(TEXT("RESULT iceberg overhang escape: under the overhanging column when the iceberg stopped on the ledge %d (its middle %.0f cm into the %.0f cm column: that used to knock it out right then), "
				"then ran: knocked out %d (want 0), the ice came down past its head's height %.2f s after the landing with its capsule %.0f cm out of the column, that column's ice on the floor %d, "
				"frames inside a block %d (want 0): %s"),
				bUnder ? 1 : 0, Overhang->MiddleAtLand, Width, Overhang->KOT >= 0.f || !Penguin->IsAlive() ? 1 : 0, Reached, Overhang->ClearAtReach, bDown ? 1 : 0, Overhang->Inside,
				bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}

int32 APenguinAutoTest::CountPortals(const APenguinCharacter* PortalOwner, APinguPortal** OutFirst) const
{
	TArray<APinguPortal*> All;
	APinguPortal::GetAll(GetWorld(), All);
	int32 Count = 0;
	for (APinguPortal* Portal : All)
	{
		if (Portal->GetPortalOwner() == PortalOwner)
		{
			if (OutFirst && Count == 0)
			{
				*OutFirst = Portal;
			}
			++Count;
		}
	}
	return Count;
}

void APenguinAutoTest::MatrixCheck(const TCHAR* Label, bool bOk, const FString& Detail)
{
	++MatrixChecks;
	MatrixPassed += bOk ? 1 : 0;
	Report(FString::Printf(TEXT("RESULT matrix %s: %s (%s)"), Label, bOk ? TEXT("ok") : TEXT("FAIL"), *Detail));
}

void APenguinAutoTest::AddMatrixSteps()
{
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	// An ice wall of Rows blocks standing on the strip's floor in the column at X (lasting: the next step's fresh strip
	// clears it).
	const auto IceWall = [this](float X, int32 Rows)
	{
		ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
		if (!Flat)
		{
			return;
		}
		const FIntPoint Foot = Flat->WorldToCell(FVector(X, 0.f, 50.f));
		TArray<FIntPoint> Cells;
		for (int32 Row = 0; Row < Rows; ++Row)
		{
			Cells.Add(FIntPoint(Foot.X, Foot.Y + Row));
		}
		Flat->AddBlocks(Cells, EBlockKind::Ice, 0.f);
	};
	const auto TargetHits = [this]() -> int32
	{
		const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
		return TargetMove ? TargetMove->HitEvents - TargetHitsAtStart : -1;
	};
	const auto Broken = [this]() -> int32
	{
		const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
		return Flat ? Flat->GetBrokenBlocks() - BrokenAtStart : 0;
	};

	// ---- Pushes clash: a penguin that has just used a push (Flipper Slap, the tornado, Noot) is not moved by another push for a
	// moment - the pusher bounces off it instead.
	AddStep(TEXT("matrix_NootOnNoot"), 1.0f,
		[this]
		{
			PrepareBrawl(150.f, { EPenguinAbility::Noot });
			if (Target.IsValid())
			{
				Target->GetAbilities()->SetAbilities({ EPenguinAbility::Noot });
				Value = static_cast<float>(Target->GetActorLocation().X);
			}
		},
		[this](float T)
		{
			if (bFlag || T < 0.3f || !Target.IsValid())
			{
				return;
			}
			bFlag = true;
			// The dummy honks away from us (it braces); we honk at the dummy.
			Target->SetAimPoint(Target->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
			CountA = Target->ActivateAbility(0) ? 1 : 0;
			Penguin->SetAimPoint(Target->GetAimOrigin());
			CountB = Penguin->ActivateAbility(0) ? 1 : 0;
			MarkA = Target->IsBraced() ? 1.f : 0.f;
			MarkB = static_cast<float>(Move->Velocity.X);
		},
		[this, TargetHits]
		{
			const int32 Hits = TargetHits();
			MatrixCheck(TEXT("Noot x Noot"), CountA && CountB && Hits == 0 && MarkB < -700.f,
				FString::Printf(TEXT("both honked %d/%d, dummy braced %d, dummy hit %d (want 0), our speed right after %.0f cm/s (our kick and the clash: about -980)"),
					CountA, CountB, MarkA > 0.f ? 1 : 0, Hits, MarkB));
		});

	AddStep(TEXT("matrix_SlapOnSlap"), 1.0f,
		[this]
		{
			PrepareBrawl(120.f, { EPenguinAbility::FlipperSlap });
			if (Target.IsValid())
			{
				Target->GetAbilities()->SetAbilities({ EPenguinAbility::FlipperSlap });
			}
		},
		[this](float T)
		{
			if (bFlag || T < 0.3f || !Target.IsValid())
			{
				return;
			}
			bFlag = true;
			Target->SetAimPoint(Target->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
			CountA = Target->ActivateAbility(0) ? 1 : 0;
			Penguin->SetAimPoint(Target->GetAimOrigin());
			CountB = Penguin->ActivateAbility(0) ? 1 : 0;
			MarkA = static_cast<float>(Target->GetPenguinMovement()->Velocity.X);
			MarkB = static_cast<float>(Move->Velocity.X);
		},
		[this, TargetHits]
		{
			const int32 Hits = TargetHits();
			MatrixCheck(TEXT("Flipper Slap x Flipper Slap"), CountA && CountB && Hits == 0,
				FString::Printf(TEXT("both slapped %d/%d, dummy hit %d (want 0), dummy speed %.0f (its own lunge), our speed %.0f (our lunge less the clash)"),
					CountA, CountB, Hits, MarkA, MarkB));
		});

	// ---- Two tornadoes merge: two penguins 3 m apart that start one at the same moment suck each other in - they drift
	// together in their hovers and become one giant tornado (twice the reach) with both of them side by side in its eye; it
	// never sucks in or throws either of them (PenguinAutoTestTornado.cpp tornado_Merge measures the giant itself).
	{
		struct FClash
		{
			FVector OursAt = FVector::ZeroVector;
			FVector TheirsAt = FVector::ZeroVector;
			int32 Tornadoes = -1;
			int32 Users = 0;
			float Reach = 0.f;
			float Gap = -1.f;
			float GapWant = 0.f;
			float OursDrift = 0.f;
			float TheirsDrift = 0.f;
			float UpDown = 0.f;
			int32 HoverFrames = 0;
			int32 NotHovering = 0;
		};
		const TSharedRef<FClash> Clash = MakeShared<FClash>();
		AddStep(TEXT("matrix_TornadoOnTornado"), 2.f,
			[this, Clash]
			{
				*Clash = FClash();
				PrepareBrawl(300.f, { EPenguinAbility::Gust });
				OwnHitsAtStart = Move->HitEvents;
				if (Target.IsValid())
				{
					Target->GetAbilities()->SetAbilities({ EPenguinAbility::Gust });
				}
			},
			[this, Clash](float T)
			{
				if (!Target.IsValid())
				{
					return;
				}
				if (!bFlag && T > 0.3f)
				{
					bFlag = true;
					TimeA = T;
					CountA = Target->ActivateAbility(0) ? 1 : 0;
					CountB = Penguin->ActivateAbility(0) ? 1 : 0;
					Clash->OursAt = Location();
					Clash->TheirsAt = Target->GetActorLocation();
					return;
				}
				if (!bFlag)
				{
					return;
				}
				const float Since = T - TimeA;
				// Both hang in the eye until the giant's end (0.5 s after their own would have ended).
				if (Since > 0.05f && Since < PinguTuning::TornadoTime + PinguTuning::TornadoMergeExtraTime - 0.05f)
				{
					++Clash->HoverFrames;
					Clash->NotHovering += Move->IsHovering() && Target->GetPenguinMovement()->IsHovering() ? 0 : 1;
					Clash->UpDown = FMath::Max(Clash->UpDown, static_cast<float>(FMath::Max(FMath::Abs(Location().Z - Clash->OursAt.Z), FMath::Abs(Target->GetActorLocation().Z - Clash->TheirsAt.Z))));
				}
				if (Clash->Tornadoes < 0 && Since > PinguTuning::TornadoMergeDriftTime + 0.15f)
				{
					TArray<APinguTornado*> All;
					APinguTornado::GetAll(GetWorld(), All);
					Clash->Tornadoes = All.Num();
					if (All.Num() > 0)
					{
						Clash->Users = All[0]->GetUsers().Num();
						Clash->Reach = All[0]->GetRadius();
					}
					Clash->Gap = static_cast<float>(Target->GetActorLocation().X - Location().X);
					Clash->GapWant = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius() + Target->GetCapsuleComponent()->GetScaledCapsuleRadius() + PinguTuning::TornadoMergeEyeGap;
					Clash->OursDrift = static_cast<float>(Location().X - Clash->OursAt.X);
					Clash->TheirsDrift = static_cast<float>(Clash->TheirsAt.X - Target->GetActorLocation().X);
				}
			},
			[this, Clash, TargetHits]
			{
				const int32 Hits = TargetHits();
				const int32 OwnHits = Move->HitEvents - OwnHitsAtStart;
				const float DriftWant = (300.f - Clash->GapWant) * 0.5f;
				MatrixCheck(TEXT("Tornado x Tornado"), CountA && CountB && Hits == 0 && OwnHits == 0 && Clash->Tornadoes == 1 && Clash->Users == 2
					&& FMath::IsNearlyEqual(Clash->Reach, 2.f * PinguTuning::TornadoRadius, 1.f) && FMath::Abs(Clash->Gap - Clash->GapWant) < 10.f
					&& FMath::Abs(Clash->OursDrift - DriftWant) < 10.f && FMath::Abs(Clash->TheirsDrift - DriftWant) < 10.f && Clash->UpDown < 2.f && Clash->HoverFrames > 60 && Clash->NotHovering == 0,
					FString::Printf(TEXT("both started one %d/%d, merged into %d tornado (want 1) with %d users (want 2), reach %.0f cm (want %.0f), each drifted toward the other %.0f / %.0f cm "
						"(want %.0f) to %.0f cm apart (want %.0f), up or down %.2f cm, both hung in its eye on %d of %d frames (want all), the dummy hit %d (want 0), we were hit %d (want 0)"),
						CountA, CountB, Clash->Tornadoes, Clash->Users, Clash->Reach, 2.f * PinguTuning::TornadoRadius, Clash->OursDrift, Clash->TheirsDrift, DriftWant, Clash->Gap, Clash->GapWant,
						Clash->UpDown, Clash->HoverFrames - Clash->NotHovering, Clash->HoverFrames, Hits, OwnHits));
			});
	}

	// ---- Walls block the area abilities: a three-block ice wall between us and the dummy shields it (its ice may break).
	// (Not the tornado: it rips the ice wall out - below.)
	struct FWallCase
	{
		EPenguinAbility Ability;
		float TargetOffset;
		float StandOffset;
	};
	for (const FWallCase& Case : { FWallCase{ EPenguinAbility::Noot, 250.f, 0.f },
		FWallCase{ EPenguinAbility::FrostBreath, 250.f, 0.f }, FWallCase{ EPenguinAbility::FlipperSlap, 235.f, 65.f } })
	{
		const FString Name = UPenguinAbilityComponent::GetInfo(Case.Ability).Name;
		AddStep(*FString::Printf(TEXT("matrix_Wall_%s"), *Name.Replace(TEXT(" "), TEXT(""))), 1.2f,
			[this, Case, IceWall, Base]
			{
				PrepareBrawl(Case.TargetOffset, { Case.Ability });
				if (Case.StandOffset != 0.f)
				{
					Teleport(Base + FVector(Case.StandOffset, 0.f, 0.f));
				}
				IceWall(static_cast<float>(Base.X) + 125.f, 3);
				Value = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) : 0.f;
			},
			[this](float T)
			{
				if (Target.IsValid() && Target->GetPenguinMovement()->IsFrozen())
				{
					CountB = 1;
				}
				if (bFlag || T < 0.3f || !Target.IsValid())
				{
					return;
				}
				bFlag = true;
				Penguin->SetAimPoint(Target->GetAimOrigin());
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
			},
			[this, Name, TargetHits, Broken]
			{
				const int32 Hits = TargetHits();
				const float Moved = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) - Value : 0.f;
				MatrixCheck(*FString::Printf(TEXT("Ice Wall x %s"), *Name), CountA && Hits == 0 && CountB == 0 && FMath::Abs(Moved) < 20.f,
					FString::Printf(TEXT("used %d, dummy behind the wall hit %d / frozen %d (want 0), moved %.0f cm, wall blocks broken %d"),
						CountA, Hits, CountB, Moved, Broken()));
			});
	}

	// The tornado's ice blocks in the world now, not landed yet (returned): circling it (OutSwirling) and thrown out flying
	// away from X (OutAway).
	const auto CountTornadoIce = [this](float FromX, int32& OutSwirling, int32& OutAway) -> int32
	{
		OutSwirling = 0;
		OutAway = 0;
		int32 Count = 0;
		for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
		{
			if (It->IsFromTornado() && !It->HasLanded())
			{
				++Count;
				OutSwirling += It->IsSwirling() ? 1 : 0;
				OutAway += !It->IsSwirling() && It->GetVelocity().X * (It->GetCenter().X - FromX) > 0.f ? 1 : 0;
			}
		}
		return Count;
	};
	// Our penguin's tornado (the newest), or null.
	const auto OurTornado = [this]() -> const APinguTornado*
	{
		const APinguTornado* Found = nullptr;
		for (TActorIterator<APinguTornado> It(GetWorld()); It; ++It)
		{
			if (It->GetUser() == Penguin.Get())
			{
				Found = *It;
			}
		}
		return Found;
	};
	// Ice blocks left in the wall's column (rows 0..2 above the strip's floor at X).
	const auto WallLeft = [this](float X) -> int32
	{
		const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
		if (!Flat)
		{
			return -1;
		}
		const FIntPoint Foot = Flat->WorldToCell(FVector(X, 0.f, 50.f));
		int32 Left = 0;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			Left += Flat->GetBlock(Foot.X, Foot.Y + Row) == EBlockKind::Ice ? 1 : 0;
		}
		return Left;
	};

	// ---- An ice wall shields the penguin behind it from the tornado - until the tornado has ripped the wall's ice out: the
	// three-block wall between us and the dummy loses its two lower blocks (the top one faces the eye with its bottom, over the
	// block under it: it stays - one layer per tornado), and the dummy behind it, in the open now, is sucked in and thrown.
	// Nothing breaks.
	AddStep(TEXT("matrix_Wall_Tornado"), 1.6f,
		[this, IceWall, Base]
		{
			PrepareBrawl(250.f, { EPenguinAbility::Gust });
			IceWall(static_cast<float>(Base.X) + 125.f, 3);
		},
		[this, OurTornado](float T)
		{
			if (bFlag || T < 0.3f || !Target.IsValid())
			{
				return;
			}
			bFlag = true;
			CountA = Penguin->ActivateAbility(0) ? 1 : 0;
			if (const APinguTornado* Tornado = OurTornado())
			{
				CountB = Tornado->GetIceRipped();
			}
		},
		[this, Base, TargetHits, Broken, WallLeft]
		{
			const int32 Hits = TargetHits();
			const int32 Left = WallLeft(static_cast<float>(Base.X) + 125.f);
			MatrixCheck(TEXT("Ice Wall x Tornado"), CountA && CountB == 2 && Left == 1 && Hits >= 1 && Broken() == 0,
				FString::Printf(TEXT("used %d, wall blocks ripped out %d (want 2: the lower ones), left in the wall %d (want 1: the top one), the dummy behind it thrown %d (want 1 or more), "
					"blocks broken %d (want 0)"), CountA, CountB, Left, Hits, Broken()));
		});

	// ---- The tornado rips an ice wall out: a wall two blocks high 2.5 m to our right goes into the wind - both blocks circle
	// us (up over our right shoulder) - and is thrown out away from us, to the right; heavy, it lands as the strip's ice again a
	// few metres on. Nothing breaks.
	AddStep(TEXT("matrix_TornadoRipsIceWall"), 3.f,
		[this, IceWall, Base]
		{
			PrepareBrawl(2400.f, { EPenguinAbility::Gust });
			IceWall(static_cast<float>(Base.X) + 250.f, 2);
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
				const FVector LookAt = Base + FVector(380.f, 0.f, 150.f);
				const FVector From = LookAt + FVector(0.f, 1500.f, 120.f);
				Stage->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			MarkA = 0.f; // thrown out away from us (the most at once)
		},
		[this, Base, CountTornadoIce](float T)
		{
			int32 Swirling = 0;
			int32 Away = 0;
			CountTornadoIce(static_cast<float>(Base.X), Swirling, Away);
			CountB = FMath::Max(CountB, Swirling);
			MarkA = FMath::Max(MarkA, static_cast<float>(Away));
			if (!bFlag && T > 0.3f)
			{
				bFlag = true;
				TimeA = T;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
			}
			if (TimeA > 0.f && !bShotDone && T > TimeA + 0.35f)
			{
				bShotDone = true;
				Shot(TEXT("52_tornado_rips_wall")); // the wall's blocks circling us
			}
		},
		[this, Base, Broken, WallLeft, CountTornadoIce]
		{
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const float WallX = static_cast<float>(Base.X) + 250.f;
			const int32 Left = WallLeft(WallX);
			int32 Swirling = 0;
			int32 Away = 0;
			const int32 StillFlying = CountTornadoIce(static_cast<float>(Base.X), Swirling, Away);
			// (Where they fly depends on where they are on their circle at the throw: away from us, either side.)
			const int32 Landed = CountIce(static_cast<float>(Base.X) - 1500.f, 3000.f, 4, nullptr) - Left;
			MatrixCheck(TEXT("Tornado x Ice Wall"), CountA && CountB == 2 && FMath::RoundToInt(MarkA) == 2 && Left == 0 && Broken() == 0 && Landed == 2 && StillFlying == 0,
				FString::Printf(TEXT("used %d, blocks circling us %d (want 2), thrown out away from us %.0f (want 2), left in the wall %d (want 0), broken %d (want 0), "
					"the strip's ice again elsewhere %d (want 2), still in the air at the end %d (want 0)"),
					CountA, CountB, MarkA, Left, Broken(), Landed, StillFlying));
		});

	// ---- The tornado sucks the light things in and throws them out: an ability bubble up right of us floats in toward us,
	// then flies off (away and up); the dummy's icicle ground pad is torn out of the snow at the throw and flies on as our
	// icicle; the dummy's portal blows out like a candle - ours stays.
	{
		struct FLight
		{
			float StartDist = 0.f;
			float Closer = 0.f;
			bool bThrowSeen = false;
			FVector ThrowAt = FVector::ZeroVector;
		};
		const TSharedRef<FLight> Light = MakeShared<FLight>();
		AddStep(TEXT("matrix_TornadoBubblePadPortal"), 2.2f,
			[this, Base, Light]
			{
				*Light = FLight();
				PrepareBrawl(2400.f, { EPenguinAbility::Gust });
				TestPickup = APinguPickup::Spawn(GetWorld(), EPenguinAbility::ShrinkRay, Base + FVector(250.f, 0.f, 250.f));
				if (Target.IsValid())
				{
					TestPortal = APinguPortal::Open(GetWorld(), Target.Get(), Base + FVector(420.f, 0.f, 0.f), Base + FVector(1300.f, 0.f, 0.f));
				}
				APinguPortal::Open(GetWorld(), Penguin.Get(), Base + FVector(-150.f, 0.f, 230.f), Base + FVector(-1150.f, 0.f, 0.f));
				MarkA = 0.f; // the bubble: farthest to the right of where it was at the throw ...
				MarkB = 0.f; // ... and highest above it
			},
			[this, Base, Light](float T)
			{
				// The dummy's icicle straight down into the snow 3.2 m left of us: a ground pad.
				if (CountA == 0 && T > 0.1f)
				{
					CountA = 1;
					APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Icicle, Target.Get(), FVector(Base.X - 320.f, 0.f, Base.Z + 60.f), FVector(0.f, 0.f, -1900.f));
				}
				const FVector Eye = Penguin->GetAimOrigin();
				if (CountA == 1 && T > 0.8f)
				{
					CountA = 2;
					for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
					{
						CountB |= It->IsGroundPad() && It->GetShooter() == Target.Get() ? 1 : 0; // the pad is there
					}
					CountB |= CountPortals(Target.Get(), nullptr) == 1 && CountPortals(Penguin.Get(), nullptr) == 1 ? 2 : 0;
					if (TestPickup.IsValid())
					{
						Light->StartDist = static_cast<float>(FVector::Dist(TestPickup->GetIconLocation(), Eye));
					}
					CountB |= Penguin->ActivateAbility(0) ? 4 : 0;
					TimeA = T;
				}
				if (TimeA > 0.f)
				{
					const float Since = T - TimeA;
					if (TestPickup.IsValid())
					{
						const FVector Bubble = TestPickup->GetIconLocation();
						if (Since < PinguTuning::TornadoSuckTime)
						{
							Light->Closer = FMath::Max(Light->Closer, Light->StartDist - static_cast<float>(FVector::Dist(Bubble, Eye)));
						}
						else
						{
							if (!Light->bThrowSeen)
							{
								Light->bThrowSeen = true;
								Light->ThrowAt = Bubble;
							}
							MarkA = FMath::Max(MarkA, static_cast<float>(Bubble.X - Light->ThrowAt.X));
							MarkB = FMath::Max(MarkB, static_cast<float>(Bubble.Z - Light->ThrowAt.Z));
						}
					}
					for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
					{
						if (It->GetKind() == EPenguinProjectileKind::Icicle && It->GetShooter() == Penguin.Get() && It->IsFlying() && It->GetGravity() > 0.f)
						{
							CountB |= 8; // the pad torn out: our icicle now, falling
						}
					}
				}
			},
			[this, Light]
			{
				int32 PadsLeft = 0;
				for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
				{
					PadsLeft += It->IsGroundPad() && It->GetShooter() == Target.Get() ? 1 : 0;
				}
				const int32 TheirPortals = CountPortals(Target.Get(), nullptr);
				const int32 OurPortals = CountPortals(Penguin.Get(), nullptr);
				const bool bUsed = (CountB & 4) != 0;
				MatrixCheck(TEXT("Tornado x Bubble"), bUsed && Light->Closer > 60.f && MarkA > 150.f && MarkB > 5.f,
					FString::Printf(TEXT("used %d, the bubble drawn %.0f cm closer to us while it sucked (want over 60), then thrown %.0f cm away (want over 150) and %.0f cm up (want over 5)"),
						bUsed ? 1 : 0, Light->Closer, MarkA, MarkB));
				MatrixCheck(TEXT("Tornado x Ground Pad"), bUsed && (CountB & 1) && PadsLeft == 0 && (CountB & 8),
					FString::Printf(TEXT("the dummy's pad there before %d, left after %d (want 0), flying on as our falling icicle %d (want 1)"), CountB & 1, PadsLeft, (CountB >> 3) & 1));
				MatrixCheck(TEXT("Tornado x Portal"), bUsed && (CountB & 2) && TheirPortals == 0 && OurPortals == 1,
					FString::Printf(TEXT("both portals open before %d, the dummy's after %d (want 0: blown out), ours after %d (want 1)"), (CountB >> 1) & 1, TheirPortals, OurPortals));
				APinguPortal::CloseAll(GetWorld());
				APinguPickup::DestroyAll(GetWorld());
			});
	}

	// ---- Ice shells: two bubbles bounce off each other; a bubble blocks a warp; a bubble blown into a wall bounces off.
	AddStep(TEXT("matrix_ShellOnShell"), 1.0f,
		[this]
		{
			PrepareBrawl(150.f, { EPenguinAbility::IceShell });
			if (Target.IsValid())
			{
				Target->GetAbilities()->SetAbilities({ EPenguinAbility::IceShell });
				Value = static_cast<float>(Target->GetActorLocation().X - Location().X);
			}
			MarkA = 0.f;
			MarkB = 0.f;
		},
		[this](float T)
		{
			if (!Target.IsValid())
			{
				return;
			}
			if (!bFlag && T > 0.3f)
			{
				bFlag = true;
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
				CountB = Target->ActivateAbility(0) ? 1 : 0;
			}
			if (bFlag)
			{
				MarkA = FMath::Min(MarkA, static_cast<float>(Move->Velocity.X));
				MarkB = FMath::Max(MarkB, static_cast<float>(Target->GetPenguinMovement()->Velocity.X));
			}
		},
		[this]
		{
			const float Gap = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X - Location().X) : 0.f;
			MatrixCheck(TEXT("Ice Shell x Ice Shell"), CountA && CountB && MarkA < -400.f && MarkB > 400.f && Gap > Value + 60.f,
				FString::Printf(TEXT("both shelled %d/%d, bounced apart at %.0f / %.0f cm/s (about -700 / +700), gap %.0f -> %.0f cm"),
					CountA, CountB, MarkA, MarkB, Value, Gap));
		});

	AddStep(TEXT("matrix_ShellBlocksWarp"), 1.0f,
		[this]
		{
			PrepareBrawl(600.f, { EPenguinAbility::SnowWarp });
			if (Target.IsValid())
			{
				Target->GetAbilities()->SetAbilities({ EPenguinAbility::IceShell });
			}
		},
		[this](float T)
		{
			if (!Target.IsValid())
			{
				return;
			}
			if (!bFlag && T > 0.2f)
			{
				bFlag = true;
				CountA = Target->ActivateAbility(0) ? 1 : 0;
			}
			if (bFlag && !bKeyDone && T > 0.35f)
			{
				bKeyDone = true;
				Penguin->SetAimPoint(Target->GetActorLocation());
				CountB = Penguin->ActivateAbility(0) ? 1 : 0;
			}
			if (bKeyDone && MarkA < 0.f && T > 0.5f)
			{
				MarkA = static_cast<float>(Target->GetActorLocation().X - Location().X);
			}
		},
		[this]
		{
			MatrixCheck(TEXT("Ice Shell x Snow Warp"), CountA && CountB && MarkA > 100.f && MarkA < 300.f,
				FString::Printf(TEXT("dummy shelled %d, warped %d, we stopped %.0f cm short of the shelled dummy (want about 150; without a shell: 0)"),
					CountA, CountB, MarkA));
		});

	AddStep(TEXT("matrix_ShellOffWall"), 1.8f,
		[this, Base]
		{
			// The dummy's tornado from 2 m sucks our bubble in toward it, then throws it back into a wall behind us (rock: it
			// would rip an ice wall out).
			PrepareBrawl(200.f, { EPenguinAbility::IceShell });
			if (Target.IsValid())
			{
				Target->GetAbilities()->SetAbilities({ EPenguinAbility::Gust });
			}
			if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f)))
			{
				const FIntPoint Foot = Flat->WorldToCell(FVector(Base.X - 160.f, 0.f, 50.f));
				Flat->AddBlocks({ Foot, FIntPoint(Foot.X, Foot.Y + 1), FIntPoint(Foot.X, Foot.Y + 2) }, EBlockKind::Rock, 0.f);
			}
			MarkA = 0.f; // fastest toward the wall
			MarkB = 0.f; // fastest away from it afterwards
		},
		[this](float T)
		{
			if (!Target.IsValid())
			{
				return;
			}
			if (!bFlag && T > 0.3f)
			{
				bFlag = true;
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
			}
			if (bFlag && !bKeyDone && T > 0.35f)
			{
				bKeyDone = true;
				CountB = Target->ActivateAbility(0) ? 1 : 0; // its tornado sucks our bubble in, then throws it left, into the wall
			}
			if (bKeyDone)
			{
				const float Vx = static_cast<float>(Move->Velocity.X);
				MarkA = FMath::Min(MarkA, Vx);
				if (MarkA < -300.f)
				{
					MarkB = FMath::Max(MarkB, Vx);
				}
			}
		},
		[this]
		{
			MatrixCheck(TEXT("Ice Shell x Ice Wall"), CountA && CountB && MarkA < -500.f && MarkB > 150.f,
				FString::Printf(TEXT("shelled %d, the dummy's tornado %d, thrown at %.0f cm/s toward the wall, bounced back at %.0f cm/s (0.7x the speed it hit with; no bounce: 0)"),
					CountA, CountB, MarkA, MarkB));
		});

	// ---- Snow Warp's portal: one end where the warp began, one where it ended; whatever goes into one end comes out of the
	// other with the speed it had (penguins, shots, bombs, ability bubbles); the next warp moves it.
	AddStep(TEXT("matrix_PortalOpened"), 1.6f,
		[this] { PrepareBrawl(2400.f, { EPenguinAbility::SnowWarp }); },
		[this](float T)
		{
			if (!bFlag && T > 0.3f)
			{
				bFlag = true;
				PortalEndA = Location();
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(500.f, 0.f, 0.f));
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
				TimeA = T;
				return;
			}
			if (bFlag && MarkA < 0.f && T > TimeA + 0.05f)
			{
				APinguPortal* Portal = nullptr;
				CountB = CountPortals(Penguin.Get(), &Portal);
				ArriveX = static_cast<float>(Location().X);
				MarkA = Portal ? static_cast<float>(FVector::Dist(Portal->GetEnd(0), PortalEndA)) : 999.f;
				MarkB = Portal ? static_cast<float>(FVector::Dist(Portal->GetEnd(1), Location())) : 999.f;
			}
		},
		[this]
		{
			// A second later we still stand where we arrived: standing in our own portal's end does not send us back.
			const float Moved = FMath::Abs(static_cast<float>(Location().X) - ArriveX);
			MatrixCheck(TEXT("Snow Warp portal"), CountA && CountB == 1 && MarkA < 40.f && MarkB < 60.f && Moved < 100.f,
				FString::Printf(TEXT("warped %d to %.0f cm, portals %d (want 1), its ends %.0f / %.0f cm from where we stood and arrived, moved %.0f cm since (want 0: not sent back)"),
					CountA, ArriveX - StartX, CountB, MarkA, MarkB, Moved));
		});

	AddStep(TEXT("matrix_PortalMoves"), 1.8f,
		[this] { PrepareBrawl(2400.f, { EPenguinAbility::SnowWarp }); },
		[this](float T)
		{
			if (!bFlag && T > 0.3f)
			{
				bFlag = true;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
				return;
			}
			if (bFlag && !bKeyDone && T > 1.0f)
			{
				// Warp again (the cooldown waived): the old portal closes, the new one starts where we stood now.
				bKeyDone = true;
				PortalEndA = Location();
				Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::SnowWarp });
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
				CountA += Penguin->ActivateAbility(0) ? 1 : 0;
			}
		},
		[this]
		{
			APinguPortal* Portal = nullptr;
			const int32 Portals = CountPortals(Penguin.Get(), &Portal);
			const float FromNew = Portal ? static_cast<float>(FVector::Dist(Portal->GetEnd(0), PortalEndA)) : 999.f;
			MatrixCheck(TEXT("Snow Warp x Snow Warp"), CountA == 2 && Portals == 1 && FromNew < 40.f,
				FString::Printf(TEXT("warped %d times (want 2), portals %d (want 1: the old one closed), the new one begins %.0f cm from where the second warp did"),
					CountA, Portals, FromNew));
		});

	// Shots and bombs: into the near end, out of the far one (3 m higher), flying on as fast.
	struct FPortalShotCase
	{
		EPenguinProjectileKind Kind;
		const TCHAR* Name;
		float LaunchZ;
		FVector Velocity;
		float EndZ;
	};
	for (const FPortalShotCase& Case : {
		FPortalShotCase{ EPenguinProjectileKind::Snowball, TEXT("Snowball"), 62.f, FVector(2100.f, 0.f, 0.f), 62.f },
		FPortalShotCase{ EPenguinProjectileKind::Icicle, TEXT("Icicle"), 62.f, FVector(1900.f, 0.f, 0.f), 62.f },
		FPortalShotCase{ EPenguinProjectileKind::Bomb, TEXT("Snow Bomb"), 150.f, FVector(1100.f, 0.f, 250.f), 160.f },
		FPortalShotCase{ EPenguinProjectileKind::IceBolt, TEXT("Ice Gun"), 62.f, FVector(2000.f, 0.f, 0.f), 62.f },
		FPortalShotCase{ EPenguinProjectileKind::ShrinkRay, TEXT("Shrink Ray"), 62.f, FVector(2600.f, 0.f, 0.f), 62.f } })
	{
		AddStep(*FString::Printf(TEXT("matrix_Portal_%s"), *FString(Case.Name).Replace(TEXT(" "), TEXT(""))), 0.9f,
			[this, Case, Base]
			{
				PrepareBrawl(2400.f, {});
				PortalEndA = FVector(Base.X + 400.f, 0.f, Case.EndZ);
				PortalEndB = FVector(Base.X + 1400.f, 0.f, Case.EndZ + 300.f);
				TestPortal = APinguPortal::Open(GetWorld(), Target.Get(), PortalEndA, PortalEndB);
				TestBombA = APenguinProjectile::Launch(GetWorld(), Case.Kind, Penguin.Get(), FVector(Base.X + 100.f, 0.f, Case.LaunchZ), Case.Velocity);
			},
			[this](float)
			{
				if (TestBombA.IsValid() && CountB == 0 && FVector::Dist(TestBombA->GetActorLocation(), PortalEndB) < 120.f)
				{
					CountB = 1; // came out of the far end
					Value = static_cast<float>(TestBombA->GetVelocity().X);
				}
			},
			[this, Case]
			{
				MatrixCheck(*FString::Printf(TEXT("%s x Snow Warp portal"), Case.Name), CountB == 1 && Value > 0.6f * static_cast<float>(Case.Velocity.X),
					FString::Printf(TEXT("came out of the far end %d (want 1) flying %.0f cm/s (went in at %.0f)"), CountB, Value, Case.Velocity.X));
			});
	}

	AddStep(TEXT("matrix_PortalPenguin"), 1.5f,
		[this, Base]
		{
			PrepareBrawl(2400.f, {});
			PortalEndA = FVector(Base.X + 250.f, 0.f, Location().Z);
			PortalEndB = FVector(Base.X + 1300.f, 0.f, Location().Z);
			TestPortal = APinguPortal::Open(GetWorld(), Target.Get(), PortalEndA, PortalEndB);
		},
		[this](float T)
		{
			VirtualInput = FVector2D(T > 0.2f ? 1.f : 0.f, 0.f); // walk right, into the near end
			if (CountB == 0 && FMath::Abs(static_cast<float>(Location().X - PortalEndB.X)) < 80.f)
			{
				CountB = 1;
				TimeA = T;
			}
		},
		[this]
		{
			VirtualInput = FVector2D::ZeroVector;
			MatrixCheck(TEXT("Snow Warp portal x a penguin"), CountB == 1,
				FString::Printf(TEXT("walked in and came out of the far end %d (want 1), at %.0f cm now"), CountB, Location().X - StartX));
		});

	AddStep(TEXT("matrix_PortalSlide"), 1.4f,
		[this, Base]
		{
			PrepareBrawl(2400.f, {});
			PortalEndA = FVector(Base.X + 300.f, 0.f, Location().Z);
			PortalEndB = FVector(Base.X + 1300.f, 0.f, Location().Z);
			TestPortal = APinguPortal::Open(GetWorld(), Target.Get(), PortalEndA, PortalEndB);
		},
		[this](float T)
		{
			if (!bFlag && T > 0.3f)
			{
				bFlag = true;
				Penguin->BellySlide();
				return;
			}
			if (bFlag && CountB == 0 && FMath::Abs(static_cast<float>(Location().X - PortalEndB.X)) < 80.f)
			{
				CountB = Move->IsBellySliding() ? 1 : 2; // came out still sliding (2: out, but not sliding any more)
			}
		},
		[this]
		{
			const float Beyond = static_cast<float>(Location().X - PortalEndB.X);
			MatrixCheck(TEXT("Belly Slide x Snow Warp portal"), CountB == 1 && Beyond > 60.f,
				FString::Printf(TEXT("slid in and came out of the far end still sliding %d (want 1), went on %.0f cm past it"), CountB, Beyond));
		});

	AddStep(TEXT("matrix_PortalBubble"), 1.0f,
		[this, Base]
		{
			PrepareBrawl(2400.f, {});
			PortalEndA = FVector(Base.X + 420.f, 0.f, 110.f);
			PortalEndB = FVector(Base.X + 1300.f, 0.f, 260.f);
			TestPortal = APinguPortal::Open(GetWorld(), Target.Get(), PortalEndA, PortalEndB);
			TestPickup = APinguPickup::Spawn(GetWorld(), EPenguinAbility::Gust, FVector(Base.X + 330.f, 0.f, 110.f), FVector(350.f, 0.f, 0.f));
		},
		[this](float)
		{
			if (TestPickup.IsValid() && CountB == 0 && FVector::Dist(TestPickup->GetActorLocation(), PortalEndB) < 120.f)
			{
				CountB = 1;
			}
		},
		[this]
		{
			MatrixCheck(TEXT("Snow Warp portal x an ability bubble"), CountB == 1,
				FString::Printf(TEXT("the bubble came out of the far end %d (want 1)"), CountB));
		});

	// ---- Frost: slows an incoming icicle (it drops), refreezes icicle pads, holds a falling iceberg in the air.
	AddStep(TEXT("matrix_FrostSlowsIcicle"), 0.8f,
		[this] { PrepareBrawl(2400.f, { EPenguinAbility::FrostBreath }); },
		[this, Base](float T)
		{
			if (!bFlag && T > 0.3f)
			{
				bFlag = true;
				// The dummy's icicle flies at us above head height; we breathe frost up at it.
				TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Icicle, Target.Get(), Base + FVector(380.f, 0.f, 140.f), FVector(-1900.f, 0.f, 0.f));
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 130.f));
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
				return;
			}
			if (bFlag && MarkA < 0.f && TestBombA.IsValid() && TestBombA->IsFrozen())
			{
				MarkA = FMath::Abs(static_cast<float>(TestBombA->GetVelocity().X));
				MarkB = TestBombA->GetGravity();
			}
		},
		[this]
		{
			MatrixCheck(TEXT("Frost Breath x Icicle"), CountA && MarkA > 700.f && MarkA < 1200.f && MarkB > 1000.f,
				FString::Printf(TEXT("breathed %d, the dummy's icicle slowed to %.0f cm/s (want 950: half), gravity %.0f (want 1400: it drops)"),
					CountA, MarkA, MarkB));
		});

	AddStep(TEXT("matrix_FrostRefreezesPad"), 5.0f,
		[this, Base]
		{
			PrepareBrawl(2400.f, { EPenguinAbility::Icicle, EPenguinAbility::FrostBreath });
			if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
			{
				const FIntPoint Foot = Flat->WorldToCell(Base + FVector(320.f, 0.f, 10.f));
				TArray<FIntPoint> Wall;
				for (int32 Row = 0; Row < 5; ++Row)
				{
					Wall.Add(FIntPoint(Foot.X, Foot.Y + Row));
				}
				Flat->AddBlocks(Wall, EBlockKind::Snow, 8.f);
			}
		},
		[this, Base](float T)
		{
			if (!bFlag && T > 0.25f)
			{
				bFlag = true;
				Penguin->SetAimPoint(Base + FVector(320.f, 0.f, 170.f));
				Penguin->ActivateAbility(0);
				return;
			}
			if (!TestBombA.IsValid())
			{
				for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
				{
					if (It->HasPeg())
					{
						TestBombA = *It;
						CountB = 1;
						break;
					}
				}
				return;
			}
			if (!bKeyDone && TestBombA->GetLifeLeft() < 8.5f) // (a stuck icicle lasts 10 s)
			{
				bKeyDone = true;
				MarkA = TestBombA->GetLifeLeft();
				TimeA = T;
				Penguin->SetAimPoint(TestBombA->GetPegTop());
				CountA = Penguin->ActivateAbility(1) ? 1 : 0;
			}
			if (bKeyDone && MarkB < 0.f && T > TimeA + 0.5f)
			{
				MarkB = TestBombA->GetLifeLeft();
			}
		},
		[this]
		{
			MatrixCheck(TEXT("Frost Breath x Icicle pad"), CountB && CountA && MarkB > 9.f,
				FString::Printf(TEXT("pad made %d, its time %.1f s -> %.1f s half a second after the breath (want its full 10 again: over 9), breathed %d"),
					CountB, MarkA, MarkB, CountA));
		});

	AddStep(TEXT("matrix_FrostHoldsIceberg"), 3.6f,
		[this, Base]
		{
			PrepareBrawl(2400.f, { EPenguinAbility::FrostBreath });
			TestIceberg = AIcebergDrop::Drop(GetWorld(), Target.Get(), static_cast<float>(Base.X) + 220.f, 0.f);
		},
		[this](float T)
		{
			AIcebergDrop* Berg = TestIceberg.Get();
			if (!Berg)
			{
				if (bFlag && TimeB < 0.f)
				{
					TimeB = T; // landed (the ice is island blocks now)
				}
				return;
			}
			const float BottomZ = static_cast<float>(Berg->GetActorLocation().Z);
			if (!bFlag && Berg->IsFalling() && BottomZ < 380.f)
			{
				bFlag = true;
				TimeA = T;
				Penguin->SetAimPoint(FVector(Berg->GetLeftX() + 100.f, 0.f, BottomZ + 100.f));
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
			}
			if (bFlag && Berg->IsHeld())
			{
				CountB = 1;
			}
			if (bFlag && Value == 0.f && T > TimeA + 0.45f)
			{
				Value = BottomZ; // held: it should not move from here for a while
			}
			if (bFlag && MarkA < 0.f && T > TimeA + 0.85f)
			{
				MarkA = FMath::Abs(BottomZ - Value);
			}
		},
		[this]
		{
			MatrixCheck(TEXT("Frost Breath x Iceberg Drop"), CountA && CountB && MarkA >= 0.f && MarkA < 5.f && TimeB > 0.f,
				FString::Printf(TEXT("breathed %d, iceberg held %d, it moved %.0f cm in 0.4 s while held (want 0), it landed afterwards %d"),
					CountA, CountB, MarkA, TimeB > 0.f ? 1 : 0));
		});

	// ---- Wind blows a falling iceberg away sideways (gust: all around, away from its middle; noot: along the honk): it
	// flies off right, lands as its four blocks over there, nothing where it was dropped. The dummy's iceberg (not ours).
	for (const bool bNoot : { false, true })
	{
		AddStep(bNoot ? TEXT("matrix_NootBlowsIceberg") : TEXT("matrix_TornadoBlowsIceberg"), 3.f,
			[this, Base, bNoot]
			{
				SpawnTarget();
				PrepareBrawl(2400.f, { bNoot ? EPenguinAbility::Noot : EPenguinAbility::Gust });
				// Its columns 100-300 cm right of our penguin (24800-25000), well clear of it.
				TestIceberg = AIcebergDrop::Drop(GetWorld(), Target.Get(), static_cast<float>(Base.X) + 200.f, 0.f);
				Value = TestIceberg.IsValid() ? TestIceberg->GetLeftX() : 0.f;
				HighZ = 0.f; // the fastest it flew sideways
			},
			[this, bNoot](float T)
			{
				const AIcebergDrop* Berg = TestIceberg.Get();
				if (!Berg)
				{
					if (bFlag && TimeB < 0.f)
					{
						TimeB = T; // landed (island blocks now)
					}
					return;
				}
				const float BottomZ = static_cast<float>(Berg->GetActorLocation().Z);
				// (The tornado as soon as it falls: it throws half a second later - the iceberg must still be in the air then.)
				if (!bFlag && Berg->IsFalling() && (!bNoot || BottomZ < 450.f))
				{
					bFlag = true;
					TimeA = T;
					MarkA = Berg->GetLeftX();
					Penguin->SetAimPoint(FVector((Berg->GetLeftX() + Berg->GetRightX()) * 0.5f, 0.f, BottomZ + 100.f));
					CountA = Penguin->ActivateAbility(0) ? 1 : 0;
				}
				if (bFlag)
				{
					MarkB = Berg->GetLeftX();
					HighZ = FMath::Max(HighZ, static_cast<float>(Berg->GetVelocity().X));
				}
				if (bFlag && !bShotDone && T > TimeA + 0.12f)
				{
					bShotDone = true;
					Shot(bNoot ? TEXT("54_noot_iceberg") : TEXT("54_tornado_iceberg"));
				}
			},
			[this, bNoot]
			{
				const float Flew = MarkB - MarkA;
				const int32 IceThere = CountIce(MarkB - 60.f, 320.f, 2, nullptr);
				const int32 IceDropped = CountIce(Value, 200.f, 2, nullptr);
				const float WantSpeed = bNoot ? 1500.f : 1200.f;
				MatrixCheck(bNoot ? TEXT("Noot x Iceberg Drop") : TEXT("Tornado x Iceberg Drop"),
					CountA && Flew > 250.f && HighZ >= WantSpeed && IceThere == 4 && IceDropped == 0 && Penguin->IsAlive() && TimeB > 0.f,
					FString::Printf(TEXT("used %d, it flew %.0f cm right (want about %s), at up to %.0f cm/s (want %.0f or more), its ice blocks over there %d (want 4), where it was dropped %d (want 0), our penguin alive %d, landed %d"),
						CountA, Flew, bNoot ? TEXT("650") : TEXT("300 or more: drawn in, then thrown"), HighZ, WantSpeed, IceThere, IceDropped, Penguin->IsAlive() ? 1 : 0, TimeB > 0.f ? 1 : 0));
			});
	}

	// ---- Ice feet: a belly slide goes twice as far.
	AddStep(TEXT("matrix_SlideOnIceFeet"), 2.4f,
		[this]
		{
			PrepareBrawl(2400.f, {});
			Move->SetSlippery(6.f);
		},
		[this](float T)
		{
			if (!bFlag && T > 0.4f)
			{
				bFlag = true;
				StartX = static_cast<float>(Location().X);
				Penguin->BellySlide();
				return;
			}
			if (bFlag && Move->IsBellySliding())
			{
				Value = static_cast<float>(Location().X) - StartX;
			}
		},
		[this]
		{
			MatrixCheck(TEXT("Ice Gun x Belly Slide"), Value > 900.f,
				FString::Printf(TEXT("slid %.0f cm on ice feet (on plain snow: 589; want about twice)"), Value));
		});

	AddStep(TEXT("matrix_Summary"), 0.1f, nullptr, nullptr,
		[this]
		{
			Report(FString::Printf(TEXT("RESULT matrix: %d of %d new interaction checks ok"), MatrixPassed, MatrixChecks));
		});
}

void APenguinAutoTest::PlaceLookCamera(const FVector& Offset)
{
	AActor* Camera = LookCamera.Get();
	if (!Camera || !Penguin.IsValid())
	{
		return;
	}
	const FVector LookAt = Penguin->GetFeetLocation() + FVector(0.f, 0.f, 18.f);
	const FVector From = LookAt + Offset;
	Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
}

void APenguinAutoTest::AddPortalLookStep()
{
	// Snow Warp's portal from the stage camera's side: both ends, then one end up close.
	AddStep(TEXT("look_Portal"), 1.6f,
		[this]
		{
			PrepareBrawl(2400.f, { EPenguinAbility::SnowWarp });
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
				const FVector Middle = Location() + FVector(250.f, 0.f, 0.f);
				Stage->SetActorLocationAndRotation(Middle + FVector(0.f, 1000.f, 80.f), FRotator(-4.f, -90.f, 0.f));
			}
		},
		[this](float T)
		{
			if (!bFlag && T > 0.3f)
			{
				bFlag = true;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(500.f, 0.f, 0.f));
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
			}
			if (bFlag && MarkA < 0.f && T > 0.8f)
			{
				MarkA = T;
				Shot(TEXT("42_portal"));
			}
			if (bFlag && MarkB < 0.f && T > 1.0f)
			{
				// Up close to the end where the warp began.
				APinguPortal* Portal = nullptr;
				if (CountPortals(Penguin.Get(), &Portal) > 0 && LookCamera.IsValid())
				{
					LookCamera->SetActorLocationAndRotation(Portal->GetEnd(0) + FVector(60.f, 260.f, 20.f), FRotator(-4.f, -103.f, 0.f));
				}
				MarkB = T;
			}
			if (MarkB > 0.f && !bShotDone && T > MarkB + 0.25f)
			{
				bShotDone = true;
				Shot(TEXT("42_portal_close"));
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
			Report(FString::Printf(TEXT("portal look: warped %d, portals %d"), CountA, CountPortals(Penguin.Get(), nullptr)));
		});
}

namespace
{
	/** look_BombClock: the moments of the fuse it looks at (share of the fuse) and their screenshots. */
	constexpr float PinguBombClockMarks[3] = { 0.25f, 0.6f, 0.95f };
	const TCHAR* const PinguBombClockShots[3] = { TEXT("56_bomb_clock_25"), TEXT("56_bomb_clock_60"), TEXT("56_bomb_clock_95") };

	/** look_BombClock: what the test saw of the snow bomb's blast zone clock (GetBlastZoneProgress). */
	struct FPinguBombClockCheck
	{
		/** Where the step began (our penguin, after PrepareBrawl). */
		FVector Base = FVector::ZeroVector;
		// The thrown bomb: its fuse, when it was thrown, the clock at 25 / 60 / 95 % of the fuse, the last value while it
		// burnt, the value when it went off and how long after the throw.
		TWeakObjectPtr<APenguinProjectile> Lit;
		float Fuse = 0.f;
		float ThrownAt = -1.f;
		float At[3] = { -1.f, -1.f, -1.f };
		float LastLit = 0.f;
		float LitBlast = -1.f;
		float LitBlastAt = -1.f;
		bool bDrawn = false;
		// The clock went backwards (neither bomb is batted, so it never may).
		int32 LitBackwards = 0;
		int32 FrostedBackwards = 0;
		// The bomb whose fuse gets frozen: when it was dropped, frozen, seen thawed; the clock when frozen, how far it moved
		// while frozen, how long it was frozen, the clock a moment after the thaw, when it went off.
		TWeakObjectPtr<APenguinProjectile> Frosted;
		float DroppedAt = -1.f;
		float FreezeAt = -1.f;
		float ThawAt = -1.f;
		float AtFreeze = -1.f;
		float FrozenDrift = 0.f;
		float FrozenFor = 0.f;
		float AfterThaw = -1.f;
		float LastFrosted = 0.f;
		float FrostedBlast = -1.f;
		float FrostedBlastAt = -1.f;
		bool bFrozenShot = false;
	};
}

void APenguinAutoTest::AddEffectsLookStep(bool bUltra)
{
	// The bomb lands about 4 m ahead and goes off 1.3 s after the throw; the camera looks at where it lands from 13 m.
	const TCHAR* Prefix = bUltra ? TEXT("44_ultra") : TEXT("43_base");
	AddStep(bUltra ? TEXT("look_UltraEffects") : TEXT("look_BombZone"), bUltra ? 5.8f : 3.4f,
		[this, bUltra]
		{
			APinguFX::SetUltraOverride(bUltra ? 1 : 0);
			PrepareBrawl(2400.f, { EPenguinAbility::SnowBomb });
			TestBombA.Reset();
			PortalEndA = Location() + FVector(330.f, 0.f, 0.f); // where the bomb is (the camera follows it)
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
				const FVector Middle = Location() + FVector(330.f, 0.f, 0.f);
				Stage->SetActorLocationAndRotation(Middle + FVector(0.f, 1350.f, 260.f), FRotator(-8.f, -90.f, 0.f));
			}
		},
		[this, bUltra, Prefix](float T)
		{
			if (!bFlag && T > 0.2f)
			{
				bFlag = true;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(420.f, 0.f, 120.f));
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
				MarkA = T;
			}
			if (bFlag && !TestBombA.IsValid() && TimeA < 0.f)
			{
				TArray<APenguinProjectile*> Shots;
				APenguinProjectile::GetAll(GetWorld(), Shots);
				for (APenguinProjectile* Thrown : Shots)
				{
					if (Thrown->GetKind() == EPenguinProjectileKind::Bomb && Thrown->GetShooter() == Penguin.Get())
					{
						TestBombA = Thrown;
					}
				}
			}
			// The camera keeps the bomb (and later where it went off) in the middle.
			if (TestBombA.IsValid() && TestBombA->IsFlying())
			{
				PortalEndA = TestBombA->GetActorLocation();
				if (AActor* Camera = LookCamera.Get())
				{
					Camera->SetActorLocation(FVector(PortalEndA.X, 1350.f, Location().Z + 260.f));
				}
			}
			const FString Name = FString(Prefix);
			if (bFlag && !bKeyDone && T > MarkA + 0.55f)
			{
				bKeyDone = true;
				Shot(*(Name + TEXT("_bombzone_early")));
			}
			if (bFlag && MarkB < 0.f && T > MarkA + 1.12f)
			{
				MarkB = T;
				Shot(*(Name + TEXT("_bombzone_late")));
			}
			// It went off: the zone flash (and the Ultra blast) right after, and a moment later.
			if (bFlag && TimeA < 0.f && T > MarkA + 0.3f && (!TestBombA.IsValid() || !TestBombA->IsFlying()))
			{
				TimeA = T;
			}
			if (TimeA > 0.f && !bShotDone && T > TimeA + 0.05f)
			{
				bShotDone = true;
				Shot(*(Name + TEXT("_blast_a")));
			}
			if (TimeA > 0.f && TimeB < 0.f && T > TimeA + 0.22f)
			{
				TimeB = T;
				Shot(*(Name + TEXT("_blast_b")));
			}
			if (TimeA < 0.f)
			{
				return;
			}
			const FVector Middle = FVector(PortalEndA.X, 0.f, Location().Z + 120.f);
			const float After = T - TimeA;
			// Ultra: the other layers, set off in front of the camera one after another.
			if (bUltra && CountB == 0 && After > 0.8f)
			{
				CountB = 1;
				APinguFX::Shards(this, Middle + FVector(-260.f, 0.f, 0.f), 10, 900.f, FLinearColor(0.7f, 0.9f, 1.f));
				APinguFX::Puffs(this, Middle + FVector(260.f, 0.f, 0.f), 8, 40.f, 420.f, FVector::UpVector, 1.f, FLinearColor::White);
			}
			if (bUltra && CountB == 1 && After > 0.92f)
			{
				CountB = 2;
				Shot(TEXT("44_ultra_burst"));
			}
			if (bUltra && CountB == 2 && After > 1.5f)
			{
				CountB = 3;
				APinguFX::Sparkles(this, Middle + FVector(-250.f, 0.f, 40.f), 14, 70.f, FLinearColor(0.75f, 0.95f, 1.f));
				APinguFX::Stars(this, Middle + FVector(0.f, 0.f, 60.f), 6, 600.f);
				APinguFX::Ring(this, Middle + FVector(260.f, 0.f, 0.f), 160.f, FLinearColor(0.6f, 0.9f, 1.f), 0.5f);
			}
			if (bUltra && CountB == 3 && After > 1.68f)
			{
				CountB = 4;
				Shot(TEXT("44_ultra_glints"));
			}
			if (bUltra && CountB == 4 && After > 2.2f)
			{
				CountB = 5;
				APinguFX::Splash(this, Middle + FVector(0.f, 0.f, -100.f));
			}
			if (bUltra && CountB == 5 && After > 2.38f)
			{
				CountB = 6;
				Shot(TEXT("44_ultra_splash"));
			}
			// Both: an explosion seen from the game camera's usual distance (24 m), and the falling snow.
			const float GameView = bUltra ? 2.7f : 0.6f;
			if (CountA < 10 && After > GameView)
			{
				CountA = 10;
				if (AActor* Camera = LookCamera.Get())
				{
					Camera->SetActorLocationAndRotation(FVector(PortalEndA.X, 2400.f, Location().Z + 380.f), FRotator(-8.f, -90.f, 0.f));
				}
			}
			if (CountA == 10 && After > GameView + 0.2f)
			{
				CountA = 11;
				Shot(*(Name + TEXT("_game_snow"))); // the falling snow on its own
				if (const APinguFX* FX = APinguFX::Get(this))
				{
					FString Detail;
					const int32 Flakes = FX->CountUltraSnowflakes(Detail);
					Report(FString::Printf(TEXT("Ultra snowfall: %d flakes (%s)"), Flakes, *Detail));
				}
			}
			if (CountA == 11 && After > GameView + 0.3f)
			{
				CountA = 12;
				APinguFX::Explosion(this, Middle, 330.f);
			}
			if (CountA == 12 && After > GameView + 0.36f)
			{
				CountA = 13;
				Shot(*(Name + TEXT("_game_blast")));
			}
			if (CountA == 13 && After > GameView + 0.75f)
			{
				CountA = 14;
				Shot(*(Name + TEXT("_game_after")));
			}
		},
		[this, bUltra]
		{
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const APinguFX* FX = APinguFX::Get(this);
			FString SnowDetail;
			const int32 Flakes = FX ? FX->CountUltraSnowflakes(SnowDetail) : 0;
			Report(FString::Printf(TEXT("RESULT effects look%s: bomb thrown %d, went off %d after %.2f s, game-view shots %d, Ultra layers playing %d, Ultra snowflakes %d"),
				bUltra ? TEXT(" (Ultra)") : TEXT(""), CountA > 0 ? 1 : 0, TimeA > 0.f ? 1 : 0, TimeA > 0.f ? TimeA - MarkA : 0.f, CountA >= 14 ? 3 : 0,
				FX && FX->UsesUltraEffects() ? 1 : 0, Flakes));
			APinguFX::SetUltraOverride(-1);
		});

	if (bUltra)
	{
		return;
	}
	// The blast zone's outer ring is a clock: all dotted when the bomb is lit, a solid line sweeps round it from 12 o'clock
	// clockwise and closes the circle when the bomb goes off. A thrown bomb at 25, 60 and 95 % of its fuse (screenshots
	// 56_bomb_clock_25 / _60 / _95, the clock read back each time, never going backwards, full when it goes off); then a
	// bomb whose fuse is frozen for 0.6 s: its clock stands still meanwhile (56_bomb_clock_frozen), runs on after the thaw
	// and is full when the bomb goes off 0.6 s later than it would have.
	TSharedRef<FPinguBombClockCheck> Check = MakeShared<FPinguBombClockCheck>();
	AddStep(TEXT("look_BombClock"), 4.4f,
		[this, Check]
		{
			*Check = FPinguBombClockCheck();
			APinguFX::SetUltraOverride(0);
			PrepareBrawl(2400.f, { EPenguinAbility::SnowBomb });
			Check->Base = Location();
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
				Stage->SetActorLocationAndRotation(Check->Base + FVector(330.f, 1350.f, 260.f), FRotator(-8.f, -90.f, 0.f));
			}
		},
		[this, Check](float T)
		{
			// The thrown bomb (about 4 m ahead, like look_BombZone; the camera keeps it in the middle).
			if (Check->ThrownAt < 0.f && T > 0.2f)
			{
				Check->ThrownAt = T;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(420.f, 0.f, 120.f));
				if (Penguin->ActivateAbility(0))
				{
					TArray<APenguinProjectile*> Thrown;
					APenguinProjectile::GetAll(GetWorld(), Thrown);
					for (APenguinProjectile* One : Thrown)
					{
						if (One->GetKind() == EPenguinProjectileKind::Bomb && One->GetShooter() == Penguin.Get())
						{
							Check->Lit = One;
							Check->Fuse = One->GetLifeLeft(); // the whole fuse: it has not burnt yet
						}
					}
				}
			}
			if (APenguinProjectile* LitBomb = Check->Lit.Get(); LitBomb && Check->LitBlast < 0.f)
			{
				const float Progress = LitBomb->GetBlastZoneProgress();
				if (LitBomb->IsFlying())
				{
					Check->bDrawn = Check->bDrawn || LitBomb->HasBlastZone();
					if (Progress < Check->LastLit - 0.001f)
					{
						++Check->LitBackwards;
					}
					Check->LastLit = Progress;
					const float Since = T - Check->ThrownAt;
					for (int32 Index = 0; Index < 3; ++Index)
					{
						if (Check->At[Index] < 0.f && Since >= PinguBombClockMarks[Index] * Check->Fuse)
						{
							Check->At[Index] = Progress;
							Shot(PinguBombClockShots[Index]);
						}
					}
					if (AActor* Camera = LookCamera.Get())
					{
						Camera->SetActorLocation(FVector(LitBomb->GetActorLocation().X, 1350.f, Check->Base.Z + 260.f));
					}
				}
				else
				{
					// It went off (finished, still there for a moment): the clock it was left at.
					Check->LitBlast = Progress;
					Check->LitBlastAt = T - Check->ThrownAt;
				}
			}

			// The frosted bomb: dropped far from the first one's crater (and from us) once that one went off.
			if (Check->LitBlast >= 0.f && Check->DroppedAt < 0.f && T > Check->ThrownAt + Check->LitBlastAt + 0.3f)
			{
				Check->DroppedAt = T;
				const FVector DropAt = Check->Base + FVector(1100.f, 0.f, 150.f);
				Check->Frosted = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin.Get(), DropAt, FVector(0.f, 0.f, -100.f));
				if (AActor* Camera = LookCamera.Get())
				{
					Camera->SetActorLocation(FVector(DropAt.X, 1350.f, Check->Base.Z + 260.f));
				}
			}
			if (APenguinProjectile* FrostBomb = Check->Frosted.Get(); FrostBomb && Check->FrostedBlast < 0.f)
			{
				const float Progress = FrostBomb->GetBlastZoneProgress();
				const float Since = T - Check->DroppedAt;
				if (!FrostBomb->IsFlying())
				{
					Check->FrostedBlast = Progress;
					Check->FrostedBlastAt = Since;
					return;
				}
				if (Progress < Check->LastFrosted - 0.001f)
				{
					++Check->FrostedBackwards;
				}
				Check->LastFrosted = Progress;
				if (Check->FreezeAt < 0.f)
				{
					if (Since >= 0.5f)
					{
						// Frost on its fuse (as a frost breath does it): from now on the clock must stand still.
						Check->FreezeAt = T;
						Check->AtFreeze = Progress;
						FrostBomb->Freeze(0.6f);
					}
				}
				else if (Check->ThawAt < 0.f)
				{
					if (FrostBomb->IsFrozen())
					{
						Check->FrozenDrift = FMath::Max(Check->FrozenDrift, FMath::Abs(Progress - Check->AtFreeze));
						Check->FrozenFor = T - Check->FreezeAt;
						if (!Check->bFrozenShot && T >= Check->FreezeAt + 0.3f)
						{
							Check->bFrozenShot = true;
							Shot(TEXT("56_bomb_clock_frozen"));
						}
					}
					else
					{
						Check->ThawAt = T;
					}
				}
				else if (Check->AfterThaw < 0.f && T >= Check->ThawAt + 0.25f)
				{
					Check->AfterThaw = Progress;
				}
			}
		},
		[this, Check]
		{
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			APinguFX::SetUltraOverride(-1);
			bool bMarksOk = true;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				bMarksOk = bMarksOk && FMath::Abs(Check->At[Index] - PinguBombClockMarks[Index]) <= 0.05f;
			}
			const bool bLitOk = bMarksOk && Check->At[0] < Check->At[1] && Check->At[1] < Check->At[2] && Check->LitBlast >= 0.999f
				&& Check->LitBackwards == 0 && Check->bDrawn;
			Report(FString::Printf(TEXT("RESULT bomb clock: fuse %.2f s, clock at 25 %% %.3f, at 60 %% %.3f, at 95 %% %.3f, last while burning %.3f, when it went off %.3f (%.2f s after the throw), went backwards %d, zone drawn %d %s"),
				Check->Fuse, Check->At[0], Check->At[1], Check->At[2], Check->LastLit, Check->LitBlast, Check->LitBlastAt, Check->LitBackwards,
				Check->bDrawn ? 1 : 0, bLitOk ? TEXT("ok") : TEXT("FAIL")));
			// Frozen for 0.6 s: it goes off 1.9 s after the drop (its 1.3 s fuse + the frozen time).
			const bool bFrozenOk = Check->FreezeAt >= 0.f && Check->FrozenDrift <= 0.002f && Check->FrozenFor >= 0.5f
				&& Check->AfterThaw > Check->AtFreeze + 0.1f && Check->FrostedBlast >= 0.999f
				&& Check->FrostedBlastAt >= 1.8f && Check->FrostedBlastAt <= 2.1f && Check->FrostedBackwards == 0;
			Report(FString::Printf(TEXT("RESULT bomb clock frozen: clock %.3f when frozen, moved %.4f while frozen for %.2f s, %.3f a moment after the thaw, when it went off %.3f (%.2f s after the drop, 1.9 expected), went backwards %d %s"),
				Check->AtFreeze, Check->FrozenDrift, Check->FrozenFor, Check->AfterThaw, Check->FrostedBlast, Check->FrostedBlastAt,
				Check->FrostedBackwards, bFrozenOk ? TEXT("ok") : TEXT("FAIL")));
		});
}

void APenguinAutoTest::EndFilm()
{
	if (bFilmFixedStep)
	{
		bFilmFixedStep = false;
		FApp::SetFixedDeltaTime(FilmWasDelta);
		FApp::SetUseFixedTimeStep(bFilmWasFixed);
	}
}

void APenguinAutoTest::AddBackgroundLookStep()
{
	// The stage camera where a match puts it (35 degrees wide, 8 degrees down, 50 m out), then down at the sea and up
	// toward the horizon, then back for a film of the sea: 60 frames, each 1/20 s of game time after the one before (a
	// fixed time step: the screenshots take longer than that in real time).
	static constexpr int32 FilmFrames = 60;
	AddStep(TEXT("look_Background"), 6.f,
		[this]
		{
			PrepareBrawl(2400.f, {});
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
				// The main island (the one with the background), not the flat test strip.
				const ABlockIsland* Main = ABlockIsland::FindNear(GetWorld(), FVector::ZeroVector);
				const FBox Bounds = Main ? Main->GetBounds() : FBox(ForceInit);
				BackgroundFocus = (Bounds.IsValid ? FVector(Bounds.GetCenter().X, 0.f, Bounds.Max.Z) : Location()) + FVector(0.f, 0.f, 150.f);
				Stage->SetActorLocationAndRotation(BackgroundFocus - FRotator(-8.f, -90.f, 0.f).Vector() * 5000.f, FRotator(-8.f, -90.f, 0.f));
			}
		},
		[this](float T)
		{
			if (CountA == 0 && T > 0.6f)
			{
				CountA = 1;
				Shot(TEXT("45_bg_game_a"));
			}
			if (CountA == 1 && T > 1.1f)
			{
				CountA = 2;
				Shot(TEXT("45_bg_game_b")); // half a second later: the swell and the glints moved
			}
			if (CountA == 2 && T > 1.2f)
			{
				CountA = 3;
				if (AActor* Camera = LookCamera.Get())
				{
					const FVector Focus = BackgroundFocus + FVector(0.f, -9000.f, -1500.f);
					Camera->SetActorLocationAndRotation(Focus + FVector(0.f, 3500.f, 1100.f), FRotator(-17.f, -90.f, 0.f));
				}
			}
			if (CountA == 3 && T > 1.5f)
			{
				CountA = 4;
				Shot(TEXT("45_bg_sea")); // the water up close: swell, ripples, foam, floes
			}
			if (CountA == 4 && T > 1.6f)
			{
				CountA = 5;
				if (AActor* Camera = LookCamera.Get())
				{
					Camera->SetActorLocationAndRotation(BackgroundFocus + FVector(0.f, 5000.f, 550.f), FRotator(-1.f, -90.f, 0.f));
				}
			}
			if (CountA == 5 && T > 1.9f)
			{
				CountA = 6;
				Shot(TEXT("45_bg_horizon")); // the mountains and clouds (the game camera shows only their lower part)
			}
			if (CountA == 6 && T > 2.f)
			{
				CountA = 7;
				if (AActor* Camera = LookCamera.Get())
				{
					Camera->SetActorLocationAndRotation(BackgroundFocus - FRotator(-8.f, -90.f, 0.f).Vector() * 5000.f, FRotator(-8.f, -90.f, 0.f));
				}
			}
			if (CountA == 7 && T > 2.3f)
			{
				// From the next frame on every frame is 1/20 s of game time.
				CountA = 8;
				CountB = 0;
				bFilmWasFixed = FApp::UseFixedTimeStep();
				FilmWasDelta = FApp::GetFixedDeltaTime();
				FApp::SetFixedDeltaTime(1.0 / 20.0);
				FApp::SetUseFixedTimeStep(true);
				bFilmFixedStep = true;
			}
			else if (CountA == 8 && CountB < FilmFrames)
			{
				const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ClaudeLogs/Shots/Film") /
					FString::Printf(TEXT("45_bg_film_%02d.png"), CountB));
				FScreenshotRequest::RequestScreenshot(Path, false, false);
				if (++CountB == FilmFrames)
				{
					EndFilm(); // the next frames run in real time again (this frame's screenshot is still taken)
				}
			}
		},
		[this]
		{
			EndFilm();
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			Report(FString::Printf(TEXT("RESULT background look: shots %d, film frames %d"), CountA >= 6 ? 4 : CountA >= 4 ? 3 : CountA >= 2 ? 2 : CountA,
				CountA >= 8 ? CountB : 0));
		});
}

void APenguinAutoTest::BuildTunnel(const FVector& Base, float OffsetX, int32 Length, bool bDeadEnd)
{
	TunnelEntryX = TunnelExitX = 0.f;
	ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
	if (!Flat || Length <= 0)
	{
		return;
	}
	// The free row on the floor (just above its top), the ceiling one above it.
	const FIntPoint Foot = Flat->WorldToCell(Base + FVector(OffsetX, 0.f, 10.f));
	TArray<FIntPoint> Cells;
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Cells.Add(FIntPoint(Foot.X + Index, Foot.Y + 1));
		Cells.Add(FIntPoint(Foot.X + Index, Foot.Y + 2));
	}
	if (bDeadEnd)
	{
		for (int32 Row = 0; Row < 3; ++Row)
		{
			Cells.Add(FIntPoint(Foot.X + Length, Foot.Y + Row));
		}
	}
	Flat->AddBlocks(Cells, EBlockKind::Rock, 0.f);
	const float Half = Flat->CellSize * 0.5f;
	TunnelEntryX = static_cast<float>(Flat->CellToWorld(FIntPoint(Foot.X, Foot.Y + 1)).X) - Half;
	TunnelExitX = static_cast<float>(Flat->CellToWorld(FIntPoint(Foot.X + Length - 1, Foot.Y + 1)).X) + Half;
	Report(FString::Printf(TEXT("tunnel %d blocks long%s from x %.0f to %.0f, ceiling 1 block above the floor"),
		Length, bDeadEnd ? TEXT(" (dead end)") : TEXT(""), TunnelEntryX, TunnelExitX));
}

void APenguinAutoTest::AddSlideGapSteps()
{
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	const auto ClearTunnel = [this, Base]()
	{
		if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
		{
			Flat->ClearTemporaryBlocks();
		}
	};

	// ---- A gap one block high: standing up the penguin can't get in...
	AddStep(TEXT("slide_GapWalk"), 1.6f,
		[this, Base] { PrepareBrawl(2400.f, {}); BuildTunnel(Base, 300.f, 2, false); },
		[this](float T) { VirtualInput = T > 0.3f ? FVector2D(1.f, 0.f) : FVector2D::ZeroVector; },
		[this, ClearTunnel]
		{
			VirtualInput = FVector2D::ZeroVector;
			Report(FString::Printf(TEXT("RESULT slide gap walk: stopped %.0f cm from the gap (want below 0: in front of it), lying low %d (want 0)"),
				static_cast<float>(Location().X) - TunnelEntryX, Move->IsLyingLow() ? 1 : 0));
			ClearTunnel();
		});

	// ---- ...on its belly it slides right through, and gets up behind it.
	AddStep(TEXT("slide_GapShort"), 2.0f,
		[this, Base] { PrepareBrawl(2400.f, {}); BuildTunnel(Base, 200.f, 2, false); MarkA = 1000.f; },
		[this](float T)
		{
			if (!bFlag && T > 0.4f)
			{
				bFlag = true;
				Penguin->BellySlide();
				return;
			}
			const float X = static_cast<float>(Location().X);
			const float Radius = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
			if (X > TunnelEntryX + Radius && X < TunnelExitX - Radius)
			{
				CountA = 1; // all of it under the ceiling
				MarkA = FMath::Min(MarkA, 2.f * Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
			}
			MarkB = FMath::Max(MarkB, X);
		},
		[this, ClearTunnel]
		{
			Report(FString::Printf(TEXT("RESULT slide gap short: under the gap %d, the capsule there %.0f cm tall (want 60: the gap is 100), out the other side %d, standing again %d, sliding %d"),
				CountA, CountA ? MarkA : 0.f, MarkB > TunnelExitX + 30.f ? 1 : 0, Move->IsLyingLow() ? 0 : 1, Move->IsBellySliding() ? 1 : 0));
			ClearTunnel();
		});

	// ---- A gap longer than a slide: it never ends under the ceiling, it crawls on until it is out. Jump does nothing in
	// there (the press waits).
	AddStep(TEXT("slide_GapLong"), 3.6f,
		[this, Base] { PrepareBrawl(2400.f, {}); BuildTunnel(Base, 200.f, 8, false); JumpsAtStart = Move->JumpEvents; },
		[this](float T)
		{
			if (!bFlag && T > 0.4f)
			{
				bFlag = true;
				TimeA = T;
				Penguin->BellySlide();
				return;
			}
			if (!bFlag)
			{
				return;
			}
			const float X = static_cast<float>(Location().X);
			if (!bKeyDone && X > TunnelEntryX + 450.f)
			{
				bKeyDone = true;
				MarkA = T; // jump pressed under the ceiling
				Move->JumpPressed();
			}
			if (bKeyDone && T > MarkA + 0.1f)
			{
				Move->JumpReleased();
			}
			if (Move->JumpEvents != JumpsAtStart && X < TunnelExitX && CountB == 0)
			{
				CountB = 1; // jumped under the ceiling
			}
			if (!bShotDone && X > TunnelEntryX + 300.f)
			{
				bShotDone = true;
				Shot(TEXT("46_slide_gap"));
			}
			if (TimeB < 0.f && X > TunnelExitX + 30.f)
			{
				TimeB = T - TimeA;
			}
		},
		[this, ClearTunnel]
		{
			Move->JumpReleased();
			Report(FString::Printf(TEXT("RESULT slide gap long: out the other side %d after %.2f s (a slide alone stops after 600 cm: it crawled on), jumps under the ceiling %d (want 0), standing again %d"),
				TimeB > 0.f ? 1 : 0, TimeB, CountB, Move->IsLyingLow() ? 0 : 1));
			ClearTunnel();
		});

	// ---- A dead end: the slide stops at the wall, still lying under the ceiling; the stick turns it around and it crawls
	// back out.
	AddStep(TEXT("slide_GapDeadEnd"), 4.4f,
		[this, Base] { PrepareBrawl(2400.f, {}); BuildTunnel(Base, 200.f, 4, true); },
		[this](float T)
		{
			if (!bFlag && T > 0.4f)
			{
				bFlag = true;
				Penguin->BellySlide();
				return;
			}
			const float X = static_cast<float>(Location().X);
			MarkB = FMath::Max(MarkB, X);
			if (T > 2.2f)
			{
				VirtualInput = FVector2D(-1.f, 0.f);
				if (TimeB < 0.f && X < TunnelEntryX - 30.f && !Move->IsLyingLow())
				{
					TimeB = T - 2.2f;
				}
			}
			else if (T > 2.f && CountA == 0)
			{
				CountA = Move->IsLyingLow() && Move->IsBellySliding() ? 1 : 2; // still on the belly at the wall
			}
		},
		[this, ClearTunnel]
		{
			VirtualInput = FVector2D::ZeroVector;
			Report(FString::Printf(TEXT("RESULT slide gap dead end: got %.0f cm in (the wall at %.0f), lying there %d, backed out %d after %.2f s, standing again %d"),
				MarkB - TunnelEntryX, TunnelExitX - TunnelEntryX, CountA == 1 ? 1 : 0, TimeB > 0.f ? 1 : 0, TimeB, Move->IsLyingLow() ? 0 : 1));
			ClearTunnel();
		});
}

void APenguinAutoTest::AddBotNavSteps()
{
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	// The dummy is the bot (the test drives our own penguin's stick every frame, which would fight a brain); our penguin
	// stands still far to the right as its target. PrepareBrawl puts the dummy BotOffset from the strip's base, then our
	// penguin goes to TargetOffset. Everybody for themselves (a teammate is never a target).
	const auto PrepareBot = [this, Base](float BotOffset, float TargetOffset)
	{
		SpawnTarget(); // (a filtered run has no dummy yet)
		PrepareBrawl(BotOffset, {});
		Teleport(Base + FVector(TargetOffset, 0.f, 0.f));
		Penguin->SetTeam(-1);
		if (Target.IsValid())
		{
			Target->SetTeam(-1);
			SavedTargetAbilities = Target->GetAbilities()->GetAbilities();
		}
		MarkA = 1000.f;
		MarkB = -1.e9f;
	};
	// The brain on (normal skill) with these abilities; the dummy's jump and slide counters from here.
	const auto StartBot = [this](const TArray<EPenguinAbility>& Abilities)
	{
		APenguinCharacter* Bot = Target.Get();
		if (!Bot || !Bot->GetBot())
		{
			return;
		}
		Bot->GetAbilities()->SetAbilities(Abilities);
		const UPenguinMovementComponent* BotMove = Bot->GetPenguinMovement();
		BotJumpsAtStart = BotMove ? BotMove->JumpEvents : 0;
		BotSlidesAtStart = BotMove ? BotMove->SlideEvents : 0;
		Bot->GetBot()->SetBotEnabled(true, 1);
	};
	const auto StopBot = [this, Base]()
	{
		if (Target.IsValid())
		{
			if (Target->GetBot())
			{
				Target->GetBot()->SetBotEnabled(false);
			}
			if (SavedTargetAbilities.Num() > 0)
			{
				Target->GetAbilities()->SetAbilities(SavedTargetAbilities);
			}
		}
		APinguPortal::CloseAll(GetWorld());
		if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
		{
			Flat->ClearTemporaryBlocks();
		}
	};
	const auto BotJumps = [this]() -> int32
	{
		const UPenguinMovementComponent* BotMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
		return BotMove ? BotMove->JumpEvents - BotJumpsAtStart : 0;
	};
	const auto BotSlides = [this]() -> int32
	{
		const UPenguinMovementComponent* BotMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
		return BotMove ? BotMove->SlideEvents - BotSlidesAtStart : 0;
	};

	// ---- A gap one block high on the way to its target: the bot slides in on its belly (no hopping at the ceiling first),
	// comes out the far side and stands up again. Short (shorter than a slide) and long (it crawls on).
	for (const int32 Length : { 4, 8 })
	{
		const bool bLong = Length > 4;
		const float Limit = bLong ? 3.5f : 2.5f;
		AddStep(bLong ? TEXT("bot_SlideGapLong") : TEXT("bot_SlideGap"), bLong ? 5.f : 4.f,
			[this, Base, Length, PrepareBot, StartBot]
			{
				PrepareBot(-250.f, 2300.f);
				BuildTunnel(Base, 200.f, Length, false);
				StartBot({ EPenguinAbility::IceShell }); // (the shell only dodges: it walks up to its target)
			},
			[this, bLong, BotJumps](float T)
			{
				const APenguinCharacter* Bot = Target.Get();
				const UPenguinMovementComponent* BotMove = Bot ? Bot->GetPenguinMovement() : nullptr;
				if (!BotMove || !Bot->IsAlive())
				{
					return;
				}
				const float X = static_cast<float>(Bot->GetActorLocation().X);
				const float Radius = Bot->GetCapsuleComponent()->GetScaledCapsuleRadius();
				if (X > TunnelEntryX + Radius && X < TunnelExitX - Radius)
				{
					CountA = 1; // all of it under the ceiling
					MarkA = FMath::Min(MarkA, 2.f * Bot->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
				}
				if (X + Radius < TunnelEntryX)
				{
					CountB = BotJumps(); // hops before the ceiling (it must not hop against it)
				}
				if (TimeB < 0.f && X > TunnelExitX + 30.f && !BotMove->IsLyingLow())
				{
					TimeB = T; // out the far side, standing
				}
				if (!bShotDone && CountA && X > TunnelEntryX + 150.f)
				{
					bShotDone = true;
					Shot(bLong ? TEXT("55_bot_slide_gap_long") : TEXT("55_bot_slide_gap"));
				}
			},
			[this, bLong, Limit, BotSlides, StopBot]
			{
				const int32 Slides = BotSlides();
				const bool bOk = CountA == 1 && MarkA < 70.f && TimeB > 0.f && TimeB < Limit && CountB == 0 && Slides >= 1;
				Report(FString::Printf(TEXT("RESULT bot slide gap%s: under the gap %d, its capsule there %.0f cm (want 60), out the far side and standing %d after %.2f s (want within %.1f), hops before the gap %d (want 0), slides %d: %s"),
					bLong ? TEXT(" long") : TEXT(""), CountA, CountA ? MarkA : 0.f, TimeB > 0.f ? 1 : 0, TimeB, Limit, CountB, Slides, bOk ? TEXT("ok") : TEXT("FAIL")));
				StopBot();
			});
	}

	// ---- A dead end one block high: no slide into it; the bot jumps over it like a wall and goes on to its target.
	AddStep(TEXT("bot_SlideDeadEnd"), 4.f,
		[this, Base, PrepareBot, StartBot]
		{
			PrepareBot(-250.f, 2300.f);
			BuildTunnel(Base, 200.f, 4, true);
			StartBot({ EPenguinAbility::IceShell });
		},
		[this, BotJumps, BotSlides](float T)
		{
			const APenguinCharacter* Bot = Target.Get();
			const UPenguinMovementComponent* BotMove = Bot ? Bot->GetPenguinMovement() : nullptr;
			if (!BotMove || !Bot->IsAlive() || TimeB > 0.f)
			{
				return; // (past it, a belly slide attack at its target is fine)
			}
			if (BotMove->IsLyingLow())
			{
				CountA = 1; // on its belly at some point
			}
			CountB = BotSlides();
			Value = static_cast<float>(BotJumps());
			if (Bot->GetActorLocation().X > TunnelExitX + 130.f)
			{
				TimeB = T; // past the dead end's closed far end
			}
		},
		[this, StopBot]
		{
			const int32 Jumps = FMath::RoundToInt(Value);
			const bool bOk = CountA == 0 && CountB == 0 && TimeB > 0.f && TimeB < 3.f && Jumps >= 1 && Jumps <= 4;
			Report(FString::Printf(TEXT("RESULT bot slide dead end: lying low %d, slides %d (want 0 0), past its far end %d after %.2f s (want within 3), jumps %d (want 1 to 3): %s"),
				CountA, CountB, TimeB > 0.f ? 1 : 0, TimeB, Jumps, bOk ? TEXT("ok") : TEXT("FAIL")));
			StopBot();
		});

	// ---- A grown bot is too big for a gap one block high even on its belly: it never tries to slide in.
	AddStep(TEXT("bot_GrownNoSlide"), 4.5f,
		[this, Base, PrepareBot]
		{
			PrepareBot(-250.f, 2300.f);
			BuildTunnel(Base, 300.f, 4, false);
			if (Target.IsValid())
			{
				Target->ChangeSize(1.82f, 30.f);
			}
		},
		[this, StartBot, BotSlides](float T)
		{
			const APenguinCharacter* Bot = Target.Get();
			const UPenguinMovementComponent* BotMove = Bot ? Bot->GetPenguinMovement() : nullptr;
			if (!BotMove)
			{
				return;
			}
			if (!bFlag && T > 0.8f)
			{
				bFlag = true; // grown by now (it grows over about half a second)
				Value = Bot->GetSizeScale();
				StartBot({ EPenguinAbility::IceShell });
				return;
			}
			if (!bFlag || TimeB > 0.f)
			{
				return; // (once past the gap - over it - a belly slide attack at its target is fine)
			}
			if (BotMove->IsLyingLow())
			{
				CountA = 1;
			}
			CountB = BotSlides();
			MarkB = FMath::Max(MarkB, static_cast<float>(Bot->GetActorLocation().X));
			if (Bot->GetActorLocation().X > TunnelExitX + 60.f)
			{
				TimeB = T;
			}
		},
		[this, BotJumps, StopBot]
		{
			const bool bOk = Value > 1.6f && CountA == 0 && CountB == 0;
			Report(FString::Printf(TEXT("RESULT bot grown no slide: size %.2f (want 1.82), lying low %d, slides %d (want 0 0), jumps %d, got to %.0f cm of the gap (its face at 0), over it %d: %s"),
				Value, CountA, CountB, BotJumps(), MarkB - TunnelEntryX, TimeB > 0.f ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
			if (Target.IsValid())
			{
				Target->ResetForRound(Target->GetActorLocation()); // its normal size again
			}
			StopBot();
		});

	// ---- A portal whose other end takes the bot away from its target (end A on its way, end B behind it): it jumps over
	// end A and walks on - it never goes through (the old brain looped through it for ever).
	AddStep(TEXT("bot_PortalAvoid"), 3.2f,
		[this, Base, PrepareBot, StartBot]
		{
			PrepareBot(900.f, -900.f);
			PortalEndA = Base + FVector(250.f, 0.f, 0.f);
			PortalEndB = Base + FVector(1600.f, 0.f, 0.f);
			TestPortal = APinguPortal::Open(GetWorld(), Penguin.Get(), PortalEndA, PortalEndB);
			StartBot({ EPenguinAbility::IceShell });
		},
		[this](float T)
		{
			const APenguinCharacter* Bot = Target.Get();
			if (!Bot)
			{
				return;
			}
			if (!Bot->IsAlive())
			{
				CountB = 1;
				return;
			}
			const float X = static_cast<float>(Bot->GetActorLocation().X);
			if (TimeB < 0.f && CountB == 0 && X < PortalEndA.X - APinguPortal::EndHalfWidth - 80.f)
			{
				TimeB = T; // past end A on the far side
			}
			if (!bShotDone && FMath::Abs(X - static_cast<float>(PortalEndA.X)) < 40.f)
			{
				bShotDone = true;
				Shot(TEXT("55_bot_portal_jump")); // over the end
			}
		},
		[this, StopBot]
		{
			const int32 Transits = TestPortal.IsValid() ? TestPortal->GetTransitCount() : -1;
			const bool bOk = Transits == 0 && TimeB > 0.f && TimeB < 2.6f && CountB == 0;
			Report(FString::Printf(TEXT("RESULT bot portal avoid: through the portal %d times (want 0), past its end %d after %.2f s (want within 2.6), knocked out %d (want 0): %s"),
				Transits, TimeB > 0.f ? 1 : 0, TimeB, CountB, bOk ? TEXT("ok") : TEXT("FAIL")));
			StopBot();
		});

	// ---- A portal that takes the bot most of the way to its target: it walks into it (no jump over it) and comes out
	// next to the target - long before it could have walked there.
	AddStep(TEXT("bot_PortalUse"), 2.6f,
		[this, Base, PrepareBot, StartBot]
		{
			PrepareBot(-250.f, 2300.f);
			PortalEndA = Base + FVector(150.f, 0.f, 0.f);
			PortalEndB = Base + FVector(1900.f, 0.f, 0.f);
			TestPortal = APinguPortal::Open(GetWorld(), Penguin.Get(), PortalEndA, PortalEndB);
			StartBot({ EPenguinAbility::IceShell });
		},
		[this, Base](float T)
		{
			const APenguinCharacter* Bot = Target.Get();
			if (Bot && !Bot->IsAlive())
			{
				CountB = 1;
			}
			if (Bot && Bot->IsAlive() && CountB == 0 && TimeB < 0.f && Bot->GetActorLocation().X > Base.X + 1700.f)
			{
				TimeB = T; // over there (walking: about 3.5 s)
			}
		},
		[this, Base, StopBot]
		{
			const int32 Transits = TestPortal.IsValid() ? TestPortal->GetTransitCount() : -1;
			const float EndX = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X - Base.X) : 0.f;
			const bool bOk = Transits >= 1 && TimeB > 0.f && TimeB < 1.5f && EndX > 1500.f && CountB == 0;
			Report(FString::Printf(TEXT("RESULT bot portal use: through the portal %d times (want 1), at the far end after %.2f s (want within 1.5), still there at the end %d, knocked out %d (want 0): %s"),
				Transits, TimeB, EndX > 1500.f ? 1 : 0, CountB, bOk ? TEXT("ok") : TEXT("FAIL")));
			StopBot();
		});

	// ---- The same shortcut, but its far end is out over the sea past the strip's end: the bot does not take it (it would
	// drop into the sea) - over end A and on along the strip.
	AddStep(TEXT("bot_PortalOverSea"), 3.f,
		[this, Base, PrepareBot, StartBot]
		{
			PrepareBot(-250.f, 2300.f);
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			const FBox Bounds = Flat ? Flat->GetBounds() : FBox(Base, Base);
			PortalEndA = Base + FVector(150.f, 0.f, 0.f);
			PortalEndB = FVector(Bounds.Max.X + 350.f, 0.f, Base.Z); // no ground under it: the sea
			TestPortal = APinguPortal::Open(GetWorld(), Penguin.Get(), PortalEndA, PortalEndB);
			StartBot({ EPenguinAbility::IceShell });
		},
		[this, Base](float T)
		{
			const APenguinCharacter* Bot = Target.Get();
			if (!Bot)
			{
				return;
			}
			if (!Bot->IsAlive())
			{
				CountB = 1; // (practice brings it back after a second: remember it)
				return;
			}
			const float X = static_cast<float>(Bot->GetActorLocation().X);
			if (TimeB < 0.f && CountB == 0 && X > PortalEndA.X + APinguPortal::EndHalfWidth + 80.f && X < Base.X + 2400.f)
			{
				TimeB = T; // past end A, still on the strip
			}
		},
		[this, StopBot]
		{
			const int32 Transits = TestPortal.IsValid() ? TestPortal->GetTransitCount() : -1;
			const bool bOk = Transits == 0 && CountB == 0 && TimeB > 0.f && TimeB < 2.f;
			Report(FString::Printf(TEXT("RESULT bot portal over the sea: through the portal %d times (want 0), knocked out %d (want 0), past its end on the strip %d after %.2f s (want within 2): %s"),
				Transits, CountB, TimeB > 0.f ? 1 : 0, TimeB, bOk ? TEXT("ok") : TEXT("FAIL")));
			StopBot();
		});

	// ---- Size rays and the size limits: a bot with a grow ray, already 6.2x (then 8x), never grows itself any further (it
	// would pop past 8x) - it stays in and at most 8x. Our penguin stands 9 m away (normal size: no target for a grow ray).
	AddStep(TEXT("bot_SizeLimits"), 3.8f,
		[this, PrepareBot]
		{
			PrepareBot(-250.f, 900.f);
			if (Target.IsValid())
			{
				Target->ChangeSize(6.2f, 30.f);
			}
			MarkA = 0.f; // the biggest size target seen
		},
		[this, StartBot](float T)
		{
			APenguinCharacter* Bot = Target.Get();
			if (!Bot)
			{
				return;
			}
			MarkA = FMath::Max(MarkA, FMath::Max(Bot->GetSizeTarget(), Bot->GetSizeScale()));
			CountB |= Bot->IsKnockedOut() ? 1 : 0;
			if (!bFlag && T > 0.8f)
			{
				bFlag = true;
				StartBot({ EPenguinAbility::GrowRay });
				CountA = Bot->GetAbilities()->GetUseCount(0);
				Value = Bot->GetSizeScale();
			}
			if (bFlag && !bKeyDone && T > 2.1f)
			{
				bKeyDone = true;
				Bot->ChangeSize(PinguMaxSize / FMath::Max(Bot->GetSizeTarget(), 0.1f), 30.f); // the biggest size now
			}
		},
		[this, StopBot]
		{
			const APenguinCharacter* Bot = Target.Get();
			const int32 Uses = Bot ? Bot->GetAbilities()->GetUseCount(0) - CountA : -1;
			const float SizeNow = Bot ? Bot->GetSizeScale() : 0.f;
			const bool bOk = Value > 6.f && CountB == 0 && Uses == 0 && MarkA <= PinguMaxSize + 0.01f && SizeNow > 7.9f;
			Report(FString::Printf(TEXT("RESULT bot size limits: the bot at %.2f then %.2f with a grow ray: knocked out %d (want 0), grow rays used %d (want 0), biggest size %.2f (want 8 at most): %s"),
				Value, SizeNow, CountB, Uses, MarkA, bOk ? TEXT("ok") : TEXT("FAIL")));
			if (Target.IsValid())
			{
				Target->ResetForRound(Target->GetActorLocation());
			}
			StopBot();
		});

	// ---- An enemy at the biggest size within reach: the bot's grow ray pops it (our penguin at 8x, 7 m away: knocked out,
	// reason Overgrown).
	AddStep(TEXT("bot_GrowRayPops"), 4.5f,
		[this, PrepareBot]
		{
			PrepareBot(-250.f, 450.f);
			Penguin->ChangeSize(PinguMaxSize, 30.f);
		},
		[this, StartBot](float T)
		{
			APenguinCharacter* Bot = Target.Get();
			if (!Bot)
			{
				return;
			}
			if (!bFlag && T > 1.f)
			{
				bFlag = true;
				Value = Penguin->GetSizeScale();
				StartBot({ EPenguinAbility::GrowRay });
				CountA = Bot->GetAbilities()->GetUseCount(0);
			}
			if (bFlag && TimeA < 0.f && Penguin->IsKnockedOut())
			{
				TimeA = T;
				CountB = Penguin->GetKOReason() == EPenguinKOReason::Overgrown ? 1 : 0;
				MarkA = Bot->GetSizeScale();
				if (Bot->GetBot())
				{
					Bot->GetBot()->SetBotEnabled(false);
				}
			}
		},
		[this, StopBot]
		{
			const APenguinCharacter* Bot = Target.Get();
			const int32 Uses = Bot ? Bot->GetAbilities()->GetUseCount(0) - CountA : -1;
			const bool bOk = Value > 7.9f && TimeA > 0.f && CountB == 1 && Uses >= 1 && Bot && Bot->IsAlive();
			Report(FString::Printf(TEXT("RESULT bot grow ray pops: our penguin at %.2f, popped by the bot %d after %.2f s (reason Overgrown %d), grow rays used %d, the bot still in %d (its size then %.2f): %s"),
				Value, TimeA > 0.f ? 1 : 0, TimeA > 0.f ? TimeA - 1.f : -1.f, CountB, Uses, Bot && Bot->IsAlive() ? 1 : 0, MarkA, bOk ? TEXT("ok") : TEXT("FAIL")));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			if (Target.IsValid())
			{
				Target->ResetForRound(Target->GetActorLocation());
			}
			StopBot();
		});
}

void APenguinAutoTest::AddTeamSteps()
{
	// Teammates (team 0 both) - friendly fire: a snowball at the dummy, a noot and a gust point blank, a belly slide into
	// it - each lands like on anybody (every one from a fresh start: both put back in place, the dummy 250 cm ahead, 450
	// for the slide). Then the dummy on the other team: the same snowball hits.
	AddStep(TEXT("team_FriendlyFire"), 3.2f,
		[this]
		{
			SpawnTarget();
			PrepareBrawl(250.f, { EPenguinAbility::Snowball, EPenguinAbility::Noot, EPenguinAbility::Gust });
			Penguin->SetTeam(0);
			if (Target.IsValid())
			{
				Target->SetTeam(0);
			}
			MarkA = 0.f; // the farthest the teammate was pushed from where it stood (cm)
		},
		[this](float T)
		{
			UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (!TargetMove)
			{
				return;
			}
			// Four stages, 0.8 s each: put back in place at its start, the attack 0.15 s later, its hit counted at its end.
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			const int32 Stage = FMath::Min(static_cast<int32>(T / 0.8f), 3);
			const float StageT = T - Stage * 0.8f;
			if (CountA == Stage * 2)
			{
				CountA = Stage * 2 + 1;
				if (Stage > 0)
				{
					CountB |= TargetMove->HitEvents != TargetHitsAtStart ? (1 << (Stage - 1)) : 0;
				}
				Teleport(Base);
				Penguin->FaceDirection(1.f);
				ResetTarget(Base + FVector(Stage == 3 ? 450.f : 250.f, 0.f, 0.f));
				Value = static_cast<float>(Target->GetActorLocation().X);
				TargetHitsAtStart = TargetMove->HitEvents;
			}
			Penguin->SetAimPoint(Target->GetAimOrigin());
			if (CountA == Stage * 2 + 1 && StageT > 0.15f)
			{
				CountA = Stage * 2 + 2;
				if (Stage < 3)
				{
					CountB |= Penguin->ActivateAbility(Stage) ? (1 << (Stage + 4)) : 0; // snowball, noot, gust
				}
				else
				{
					Penguin->BellySlide();
				}
			}
			MarkA = FMath::Max(MarkA, FMath::Abs(static_cast<float>(Target->GetActorLocation().X) - Value));
		},
		[this]
		{
			const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			CountB |= TargetMove && TargetMove->HitEvents != TargetHitsAtStart ? (1 << 3) : 0;
			const int32 Used = ((CountB >> 4) & 1) + ((CountB >> 5) & 1) + ((CountB >> 6) & 1);
			const int32 Landed = (CountB & 1) + ((CountB >> 1) & 1) + ((CountB >> 2) & 1) + ((CountB >> 3) & 1);
			const bool bOk = Used == 3 && Landed == 4 && MarkA > 200.f;
			Report(FString::Printf(TEXT("RESULT team friendly fire: abilities used %d of 3, landed on the teammate: snowball %d, noot %d, gust %d, slide %d (want 1 1 1 1), "
				"pushed up to %.0f cm (want over 200), alive %d %s"),
				Used, CountB & 1, (CountB >> 1) & 1, (CountB >> 2) & 1, (CountB >> 3) & 1, MarkA, Target.IsValid() && Target->IsAlive() ? 1 : 0,
				bOk ? TEXT("ok") : TEXT("FAIL")));
		});
	// ---- Friendly fire with falling ice: our iceberg crushes a teammate right under it (reason Iceberg), as anybody's.
	const FVector TeamBase(FlatIslandX - 300.f, 0.f, 62.f);
	AddStep(TEXT("team_IcebergOnTeammate"), 2.4f,
		[this, TeamBase]
		{
			SpawnTarget();
			PrepareBrawl(2400.f, {});
			Penguin->SetTeam(0);
			if (Target.IsValid())
			{
				Target->SetTeam(0);
			}
			TestIceberg = AIcebergDrop::Drop(GetWorld(), Penguin.Get(), static_cast<float>(TeamBase.X) + 600.f, 0.f);
		},
		[this](float T)
		{
			APenguinCharacter* Dummy = Target.Get();
			const AIcebergDrop* Berg = TestIceberg.Get();
			if (!Dummy)
			{
				return;
			}
			if (Berg && !bFlag && Berg->IsFalling() && Berg->GetActorLocation().Z < 800.f)
			{
				bFlag = true;
				Dummy->NetTeleport(FVector((Berg->GetLeftX() + Berg->GetRightX()) * 0.5f, 0.f, 560.f), FVector::ZeroVector, true);
				return;
			}
			if (bFlag && !Dummy->IsAlive() && TimeA < 0.f)
			{
				TimeA = T;
				CountA = Dummy->GetKOReason() == EPenguinKOReason::Iceberg ? 1 : 0;
			}
		},
		[this]
		{
			const bool bOk = TimeA > 0.f && CountA == 1 && Penguin->IsAlive();
			Report(FString::Printf(TEXT("RESULT team iceberg on a teammate: put under it %d, knocked out %d, reason iceberg %d, the dropper still in %d: %s"),
				bFlag ? 1 : 0, TimeA > 0.f ? 1 : 0, CountA, Penguin->IsAlive() ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	// ---- Falling sand ice that we made fall: it crushes our teammate under it (reason Crushed); we are spared under our own.
	AddStep(TEXT("team_SandOnTeammate"), 1.6f,
		[this, TeamBase]
		{
			SpawnTarget();
			PrepareBrawl(650.f, {});
			Penguin->SetTeam(0);
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), TeamBase);
			if (!Flat || !Target.IsValid())
			{
				return;
			}
			Target->SetTeam(0);
			SandColumn = Flat->WorldToCell(FVector(Target->GetActorLocation().X, 0.f, 50.f));
			const FIntPoint Ours = Flat->WorldToCell(FVector(Location().X, 0.f, 50.f));
			Flat->AddSandBlocks({ FIntPoint(SandColumn.X, SandColumn.Y + 5), FIntPoint(Ours.X, Ours.Y + 5) }, EBlockKind::Ice, Penguin.Get());
		},
		[this](float T)
		{
			if (!Penguin->IsAlive())
			{
				MarkB = 1.f; // we went out under our own sand
			}
			if (Target.IsValid() && !Target->IsAlive() && TimeA < 0.f)
			{
				TimeA = T;
				CountA = Target->GetKOReason() == EPenguinKOReason::Crushed ? 1 : 0;
			}
		},
		[this]
		{
			const bool bOk = TimeA > 0.f && CountA == 1 && MarkB <= 0.f && Penguin->IsAlive();
			Report(FString::Printf(TEXT("RESULT team sand on a teammate: the teammate under our falling ice crushed %d, reason crushed %d; we under our own still in %d (want 1): %s"),
				TimeA > 0.f ? 1 : 0, CountA, MarkB <= 0.f && Penguin->IsAlive() ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});

	// ---- A hard bot dodges its teammate's snowballs (friendly fire: they hurt like anybody's), never its own. The dummy is
	// the bot (with an ice shell: its shell, a jump or a dash is a dodge); our penguin, its teammate, stands 6 m away. First
	// three of the bot's own snowballs fly at it from the left (they pass through it: no reason to dodge), then four of ours
	// (each with the bot put back in place first). A shot counts as dodged when the bot jumped, dashed or shelled within 0.6 s.
	struct FDodgeState
	{
		TArray<EPenguinAbility> Saved;
		int32 ShotIndex = -1;
		float ShotT = 0.f;
		int32 JumpsAtShot = 0;
		bool bReacted = false;
		int32 OwnReactions = 0;
		int32 MateDodges = 0;
		int32 MateShots = 0;
	};
	const TSharedRef<FDodgeState> Dodge = MakeShared<FDodgeState>();
	static const float DodgeShotTimes[] = { 0.6f, 1.3f, 2.f, 3.f, 3.9f, 4.8f, 5.7f };
	static constexpr int32 DodgeOwnShots = 3;
	static constexpr int32 NumDodgeShots = UE_ARRAY_COUNT(DodgeShotTimes);
	AddStep(TEXT("bot_DodgeTeammate"), 6.5f,
		[this, Dodge]
		{
			*Dodge = FDodgeState();
			SpawnTarget();
			PrepareBrawl(600.f, {});
			Teleport(FVector(FlatIslandX - 1000.f, 0.f, 62.f)); // out of the snowballs' way (they start 6 m left of the bot)
			Penguin->SetTeam(0);
			if (Target.IsValid() && Target->GetBot())
			{
				Target->SetTeam(0);
				Dodge->Saved = Target->GetAbilities()->GetAbilities();
				Target->GetAbilities()->SetAbilities({ EPenguinAbility::IceShell });
				Target->GetBot()->SetBotEnabled(true, 2);
			}
		},
		[this, Dodge, TeamBase](float T)
		{
			APenguinCharacter* Bot = Target.Get();
			const UPenguinMovementComponent* BotMove = Bot ? Bot->GetPenguinMovement() : nullptr;
			if (!BotMove)
			{
				return;
			}
			FDodgeState& D = *Dodge;
			// The last shot's window: did the bot react?
			if (D.ShotIndex >= 0 && T < D.ShotT + 0.6f)
			{
				D.bReacted |= BotMove->JumpEvents != D.JumpsAtShot || Bot->IsShielded() || BotMove->IsDashing();
			}
			const int32 Next = D.ShotIndex + 1;
			if (Next < NumDodgeShots && T > DodgeShotTimes[Next] - 0.3f && T < DodgeShotTimes[Next] - 0.25f)
			{
				ResetTarget(TeamBase + FVector(600.f, 0.f, 0.f)); // back in place, standing
			}
			if (Next < NumDodgeShots && T > DodgeShotTimes[Next])
			{
				// The window of the shot before is over: count it.
				if (D.ShotIndex >= 0)
				{
					if (D.ShotIndex < DodgeOwnShots)
					{
						D.OwnReactions += D.bReacted ? 1 : 0;
					}
					else
					{
						D.MateDodges += D.bReacted ? 1 : 0;
					}
				}
				D.ShotIndex = Next;
				D.ShotT = T;
				D.JumpsAtShot = BotMove->JumpEvents;
				D.bReacted = false;
				// Level at the bot's chest after 0.43 s (a snowball falls 700 cm/s²): 600 cm at 1400 cm/s, 175 up.
				const bool bOwn = Next < DodgeOwnShots;
				const FVector Aim = Bot->GetAimOrigin();
				APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, bOwn ? Bot : Penguin.Get(), Aim + FVector(-600.f, 0.f, 0.f), FVector(1400.f, 0.f, 175.f));
				D.MateShots += bOwn ? 0 : 1;
			}
		},
		[this, Dodge]
		{
			FDodgeState& D = *Dodge;
			if (D.ShotIndex >= DodgeOwnShots)
			{
				D.MateDodges += D.bReacted ? 1 : 0;
			}
			if (Target.IsValid())
			{
				if (Target->GetBot())
				{
					Target->GetBot()->SetBotEnabled(false);
				}
				if (D.Saved.Num() > 0)
				{
					Target->GetAbilities()->SetAbilities(D.Saved);
				}
			}
			const bool bOk = D.OwnReactions == 0 && D.MateDodges >= 1 && D.MateShots == NumDodgeShots - DodgeOwnShots;
			Report(FString::Printf(TEXT("RESULT bot dodges a teammate: its own snowballs reacted to %d of %d (want 0), its teammate's dodged %d of %d (want 1 or more): %s"),
				D.OwnReactions, DodgeOwnShots, D.MateDodges, D.MateShots, bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	AddStep(TEXT("team_EnemyHit"), 1.4f,
		[this]
		{
			PrepareBrawl(400.f, { EPenguinAbility::Snowball });
			Penguin->SetTeam(0);
			if (Target.IsValid())
			{
				Target->SetTeam(1);
			}
		},
		[this](float T)
		{
			if (Target.IsValid())
			{
				Penguin->SetAimPoint(Target->GetAimOrigin());
			}
			if (CountA == 0 && T > 0.3f)
			{
				CountA = 1;
				Penguin->ActivateAbility(0);
			}
		},
		[this]
		{
			const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			Report(FString::Printf(TEXT("RESULT team enemy hit: hits on the other team's penguin %d (want 1)"), TargetMove ? TargetMove->HitEvents - TargetHitsAtStart : -1));
			// Back to everybody for themselves for the rest of the test.
			Penguin->SetTeam(-1);
			if (Target.IsValid())
			{
				Target->SetTeam(-1);
			}
		});
}

void APenguinAutoTest::AddTeamLookStep()
{
	AddStep(TEXT("look_TeamsAndMarkers"), 3.2f,
		[this]
		{
			SpawnTarget();
			ClearAbilityLeftovers();
			APinguPickup::DestroyAll(GetWorld());
			// The course becomes an arena: the stage camera frames the whole island (island first).
			if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
			{
				Island->RegenerateArena(4242);
				const TArray<FVector> Starts = Island->GetSpreadSpawnLocations(4);
				if (Starts.Num() >= 4)
				{
					Teleport(Starts[1]);
					ResetTarget(Starts[2]);
				}
			}
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets(0.5f);
			}
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Icicle, EPenguinAbility::IceShell, EPenguinAbility::Noot });
			Penguin->SetTeam(0);
			if (Target.IsValid())
			{
				Target->GetAbilities()->SetAbilities({ EPenguinAbility::Snowball, EPenguinAbility::Gust, EPenguinAbility::IceGun });
				Target->SetTeam(1);
			}
			VirtualInput = FVector2D::ZeroVector;
			CountA = 0;
			bFlag = false;
		},
		[this](float T)
		{
			// Everybody's icons: ours full size, the other penguin's smaller (one of ours recharging).
			if (CountA == 0 && T > 0.6f)
			{
				CountA = 1;
				Penguin->SetAimPoint(Location() + FVector(-600.f, 0.f, 0.f));
				Penguin->ActivateAbility(0);
			}
			if (CountA == 1 && T > 1.3f)
			{
				CountA = 2;
				Shot(TEXT("48_icons_everybody"), true);
			}
			// The other penguin flung far out of the picture (well past the island's edge, high up): its marker shows at the
			// screen's edge, pointing at it.
			if (CountA == 2 && T > 1.6f && Target.IsValid())
			{
				CountA = 3;
				const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
				const FBox Bounds = Island ? Island->GetBounds() : FBox(FVector(-1000.f), FVector(1000.f));
				Target->NetTeleport(FVector(Bounds.Max.X + 2600.f, 0.f, Bounds.Max.Z - 100.f), FVector(0.f, 0.f, 650.f), true);
			}
			if (CountA == 3 && T > 1.85f)
			{
				CountA = 4;
				// Is it out of the picture (as the HUD decides it)?
				const APlayerController* Viewer = GetWorld()->GetFirstPlayerController();
				FVector2D Screen;
				int32 SizeX = 0;
				int32 SizeY = 0;
				if (Viewer && Target.IsValid() && Viewer->ProjectWorldLocationToScreen(Target->GetActorLocation(), Screen, false))
				{
					Viewer->GetViewportSize(SizeX, SizeY);
					bFlag = Screen.X < 0.0 || Screen.Y < 0.0 || Screen.X > SizeX || Screen.Y > SizeY;
				}
				Shot(TEXT("48_offscreen_marker"), true);
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT team look: shots %d, the flung penguin out of the picture %d (want 1: its marker at the edge)"),
				CountA >= 4 ? 2 : (CountA >= 2 ? 1 : 0), bFlag ? 1 : 0));
			Penguin->SetTeam(-1);
			if (Target.IsValid())
			{
				Target->SetTeam(-1);
			}
		});

	// ---- The stage camera frames a giant: the dummy at 8x on the arena (at the first spawn point with room for it), its head
	// and feet inside the picture, the camera no further out than MaxFrameDistance; then the giant knocked flying (flung)
	// - it no longer counts, the camera must not go further out for it.
	AddStep(TEXT("look_GiantFrame"), 3.2f,
		[this]
		{
			SpawnTarget();
			ClearAbilityLeftovers();
			APinguPickup::DestroyAll(GetWorld());
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			VirtualInput = FVector2D::ZeroVector;
			CountA = 0;
			CountB = 0;
			MarkA = MarkB = Value = -1.f;
			HighZ = 0.f;
			ABlockIsland* Island = ABlockIsland::Get(GetWorld());
			const TArray<FVector> Starts = Island ? Island->GetSpreadSpawnLocations(4) : TArray<FVector>();
			if (!Island || Starts.Num() < 2 || !Target.IsValid())
			{
				return;
			}
			Teleport(Starts[0]);
			// The dummy where an 8x penguin fits (standing on the ground there, nothing above it for 9 m).
			const UCapsuleComponent* Capsule = Target->GetCapsuleComponent();
			const float Radius = Capsule->GetUnscaledCapsuleRadius() * PinguMaxSize;
			const float Half = Capsule->GetUnscaledCapsuleHalfHeight() * PinguMaxSize;
			FVector Spot = Starts[1];
			for (int32 Index = 1; Index < Starts.Num(); ++Index)
			{
				const FVector Feet = Starts[Index] - FVector(0.f, 0.f, Capsule->GetUnscaledCapsuleHalfHeight());
				if (Island->CanFitCapsule(Feet + FVector(0.f, 0.f, Half + 5.f), Radius, Half))
				{
					Spot = Starts[Index];
					CountB = Index;
					break;
				}
			}
			ResetTarget(Spot);
			Target->ChangeSize(PinguMaxSize, 30.f);
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets(0.3f);
			}
		},
		[this](float T)
		{
			APenguinCharacter* Giant = Target.Get();
			const APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld());
			const APlayerController* Viewer = GetWorld()->GetFirstPlayerController();
			if (!Giant || !Stage || !Viewer)
			{
				return;
			}
			const float CameraOut = FMath::Abs(static_cast<float>(Stage->GetActorLocation().Y));
			if (CountA == 0 && T > 1.8f)
			{
				CountA = 1;
				// Its head and feet on the screen?
				const float Half = Giant->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
				int32 SizeX = 0;
				int32 SizeY = 0;
				Viewer->GetViewportSize(SizeX, SizeY);
				int32 Inside = 0;
				for (const float Up : { Half, -Half })
				{
					FVector2D Screen;
					if (Viewer->ProjectWorldLocationToScreen(Giant->GetActorLocation() + FVector(0.f, 0.f, Up), Screen, false))
					{
						Inside += Screen.X >= 0.0 && Screen.Y >= 0.0 && Screen.X <= SizeX && Screen.Y <= SizeY ? 1 : 0;
					}
				}
				Value = static_cast<float>(Inside);
				MarkA = CameraOut;
				MarkB = Giant->GetSizeScale();
				Shot(TEXT("56_giant_frame"));
			}
			if (CountA == 1 && T > 2.f)
			{
				// Knocked flying (stunned in the air): flung, it no longer counts for the frame.
				CountA = 2;
				FPenguinHit Hit;
				Hit.Instigator = Penguin.Get();
				Hit.Source = EPenguinAbility::Snowball;
				Hit.SourceSize = 1.f;
				Hit.Knockback = FVector2D(8000.f, 4000.f); // x 0.25 on an 8x penguin: 2000 sideways, 1000 up
				Hit.Stun = 2.f;
				Hit.Location = Giant->GetActorLocation();
				Giant->ApplyPenguinHit(Hit);
			}
			if (CountA == 2)
			{
				HighZ = FMath::Max(HighZ, CameraOut);
			}
		},
		[this]
		{
			const APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld());
			const float MaxOut = Stage ? Stage->MaxFrameDistance : 0.f;
			const bool bOk = MarkB > 7.9f && Value >= 2.f && MarkA > 0.f && MarkA <= MaxOut + 1.f && HighZ <= MarkA + 50.f;
			Report(FString::Printf(TEXT("RESULT giant frame: the dummy at %.2f (spot %d), head and feet in the picture %.0f of 2, camera out %.0f cm (want at most %.0f); "
				"flung, the camera out at most %.0f cm (want no further than before): %s"),
				MarkB, CountB, Value, MarkA, MaxOut, HighZ, bOk ? TEXT("ok") : TEXT("FAIL")));
			if (Target.IsValid())
			{
				Target->ResetForRound(Target->GetActorLocation());
			}
		});
}

void APenguinAutoTest::AddCooldownLookStep()
{
	// The icons above the head while two abilities recharge (one ready): screenshots with the UI, from a camera in front.
	AddStep(TEXT("look_Cooldown"), 3.f,
		[this]
		{
			PrepareBrawl(2400.f, { EPenguinAbility::Icicle, EPenguinAbility::IceShell, EPenguinAbility::Noot });
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this](float T)
		{
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Penguin->GetFeetLocation() + FVector(0.f, 0.f, 130.f);
				const FVector From = LookAt + FVector(0.f, 620.f, 40.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			if (CountA == 0 && T > 0.3f)
			{
				CountA = 1;
				Penguin->SetAimPoint(Location() + FVector(600.f, 0.f, 0.f));
				Penguin->ActivateAbility(0); // icicle: 3 s
			}
			if (CountA == 1 && T > 0.7f)
			{
				CountA = 2;
				Penguin->ActivateAbility(1); // ice shell: 4.5 s
			}
			const UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			if (CountA == 2 && T > 1.4f)
			{
				CountA = 3;
				MarkA = Abilities->GetCooldownFraction(0);
				MarkB = Abilities->GetCooldownFraction(1);
				Value = Abilities->GetCooldownFraction(2);
				Shot(TEXT("47_cooldown_a"), true);
			}
			if (CountA == 3 && T > 2.5f)
			{
				CountA = 4;
				Shot(TEXT("47_cooldown_b"), true);
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
			Report(FString::Printf(TEXT("RESULT cooldown look: shots %d, still to charge at the first: %.2f / %.2f / %.2f (want two charging, one ready)"),
				CountA >= 4 ? 2 : CountA >= 3 ? 1 : 0, MarkA, MarkB, Value));
		});
}

void APenguinAutoTest::AddStunAndIceShieldSteps()
{
	// ---- The slap: two seconds' stun (dizzy stars all of it), a little knock; a slap right after that stun only makes it flinch.
	AddStep(TEXT("slap_Stun"), 3.5f,
		[this]
		{
			PrepareBrawl(110.f, { EPenguinAbility::FlipperSlap });
			MarkA = MarkB = Value = 0.f;
		},
		[this](float T)
		{
			UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (!TargetMove)
			{
				return;
			}
			if (CountA == 0 && T > 0.25f)
			{
				CountA = 1;
				Penguin->SetAimPoint(Target->GetAimOrigin());
				Penguin->ActivateAbility(0);
			}
			if (CountA == 1 && TargetMove->HitEvents != TargetHitsAtStart)
			{
				CountA = 2;
				TimeA = T;
				MarkA = TargetMove->GetStunRemaining();
				StartX = static_cast<float>(Target->GetActorLocation().X);
			}
			if (CountA >= 2)
			{
				Value = FMath::Max(Value, FMath::Abs(static_cast<float>(TargetMove->Velocity.X)));
				if (TimeB < 0.f && !TargetMove->IsStunned())
				{
					TimeB = T;
					HighZ = static_cast<float>(Target->GetActorLocation().X) - StartX; // how far the little knock sent it
				}
				if (T > TimeA + 0.4f && T < TimeA + 0.6f && Target->IsDizzy())
				{
					CountB |= 1;
				}
				if (T > TimeA + 1.7f && T < TimeA + 1.85f && Target->IsDizzy())
				{
					CountB |= 4; // still dizzy late in the two seconds
				}
				if (TimeB > 0.f && T > TimeB + 0.1f && T < TimeB + 0.2f && !Target->IsDizzy())
				{
					CountB |= 2; // the stars are gone with the stun
				}
			}
			if (CountA == 2 && TimeB > 0.f && T > TimeB + 0.25f)
			{
				// Another slap right after the stun (within its grace): a flinch, not another second.
				CountA = 3;
				FPenguinHit Hit;
				Hit.Instigator = Penguin.Get();
				Hit.Source = EPenguinAbility::FlipperSlap;
				Hit.Knockback = FVector2D(300.f, 200.f);
				Hit.Stun = 1.f;
				Hit.Location = Target->GetActorLocation();
				Target->ApplyPenguinHit(Hit);
				MarkB = TargetMove->GetStunRemaining();
			}
		},
		[this]
		{
			const float Stunned = TimeA >= 0.f && TimeB >= 0.f ? TimeB - TimeA : -1.f;
			const bool bOk = Stunned > 1.8f && Stunned < 2.2f && (CountB & 7) == 7 && MarkB > 0.f && MarkB < 0.3f;
			Report(FString::Printf(TEXT("RESULT slap stun: stunned for %.2f s (want ~2, from %.2f at the hit), knocked at %.0f cm/s and %.0f cm (a little), "
				"dizzy %d, still dizzy at 1.8 s %d, stars gone after %d; a slap right after it stunned for %.2f s (want 0.25: no stun-lock) %s"),
				Stunned, MarkA, Value, HighZ, (CountB & 1) ? 1 : 0, (CountB & 4) ? 1 : 0, (CountB & 2) ? 1 : 0, MarkB, bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	// ---- Frozen: a snowball only bursts on the ice (still frozen, a small nudge), the ice takes the first hit that smashes ice
	// (a slap, a bomb; an icicle: no knock-out), wind blows
	// the ice cube away and it stays frozen. CountB collects what was right.
	AddStep(TEXT("ice_Shield"), 3.8f,
		[this]
		{
			PrepareBrawl(330.f, { EPenguinAbility::Snowball, EPenguinAbility::Gust, EPenguinAbility::Icicle });
			MarkA = MarkB = Value = 0.f;
		},
		[this](float T)
		{
			UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (!TargetMove)
			{
				return;
			}
			const auto Freeze = [this]()
			{
				FPenguinHit Hit;
				Hit.Instigator = Penguin.Get();
				Hit.Source = EPenguinAbility::FrostBreath;
				Hit.Freeze = 2.6f;
				Hit.Stun = 0.f;
				Hit.Location = Target->GetActorLocation();
				Target->ApplyPenguinHit(Hit);
				StartX = static_cast<float>(Target->GetActorLocation().X);
			};
			const auto Fire = [this](int32 Slot)
			{
				Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Snowball, EPenguinAbility::Gust, EPenguinAbility::Icicle }); // all ready
				Penguin->SetAimPoint(Target->GetAimOrigin());
				Penguin->ActivateAbility(Slot);
			};
			const float Moved = FMath::Abs(static_cast<float>(Target->GetActorLocation().X) - StartX);
			const bool bAlive = !Target->IsKnockedOut();
			if (CountA == 0 && T > 0.2f) { CountA = 1; Freeze(); }
			if (CountA == 1 && T > 0.35f) { CountA = 2; Fire(0); }                       // a snowball
			if (CountA == 2 && T > 0.85f)
			{
				CountA = 3;
				CountB |= (TargetMove->IsFrozen() ? 1 : 0) | (Moved < 150.f ? 2 : 0) | (bAlive ? 4 : 0);
				Value = Moved;
			}
			if (CountA == 3 && T > 0.95f)
			{
				CountA = 4;
				// (The snowball nudged the ice cube along: it goes back where it stood before the tornado's turn, so the tornado reaches it as ever.)
				Target->SetActorLocation(FVector(FlatIslandX - 300.f + 330.f, 0.f, 62.f), false, nullptr, ETeleportType::TeleportPhysics);
				TargetMove->Velocity = FVector::ZeroVector;
				Freeze();
			}
			if (CountA == 4 && T > 1.05f) { CountA = 5; Fire(1); }                       // a tornado (it throws at 1.55 s)
			if (CountA == 5 && T > 2.1f) // (it was sucked in first: judged once it has slid out past where it was)
			{
				CountA = 6;
				CountB |= (TargetMove->IsFrozen() ? 8 : 0) | (Moved > 100.f ? 16 : 0) | (bAlive ? 32 : 0);
				MarkA = Moved;
			}
			if (CountA == 6 && T > 2.15f) { CountA = 7; Freeze(); }                       // (we hung in the tornado's eye to 2.05 s)
			if (CountA == 7 && T > 2.25f) { CountA = 8; Fire(2); }                       // an icicle (deadly)
			if (CountA == 8 && T > 3.2f)
			{
				CountA = 9;
				CountB |= (bAlive ? 64 : 0) | (!TargetMove->IsFrozen() ? 128 : 0);
				MarkB = Moved;
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT ice shield: snowball on the ice - still frozen %d (want 1), nudged %.0f cm (a little: under 150) %d, alive %d; "
				"tornado - still frozen %d, slid %.0f cm (want > 100) %d, alive %d; icicle on the ice - alive %d (want 1), thawed %d, moved %.0f cm"),
				(CountB & 1) ? 1 : 0, Value, (CountB & 2) ? 1 : 0, (CountB & 4) ? 1 : 0, (CountB & 8) ? 1 : 0, MarkA, (CountB & 16) ? 1 : 0,
				(CountB & 32) ? 1 : 0, (CountB & 64) ? 1 : 0, (CountB & 128) ? 1 : 0, MarkB));
		});

	// ---- A belly stomp onto a frozen penguin: its squash and the slam right after are one attack - the ice takes it all.
	AddStep(TEXT("ice_ShieldStomp"), 1.8f,
		[this]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			Teleport(Base + FVector(0.f, 0.f, 420.f)); // right above the dummy
			ResetTarget(Base);
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::BellyStomp });
			CountA = CountB = 0;
			if (Target.IsValid())
			{
				FPenguinHit Hit;
				Hit.Instigator = Penguin.Get();
				Hit.Source = EPenguinAbility::FrostBreath;
				Hit.Freeze = 2.6f;
				Hit.Stun = 0.f;
				Hit.Location = Target->GetActorLocation();
				Target->ApplyPenguinHit(Hit);
				CountB |= Target->GetPenguinMovement()->IsFrozen() ? 1 : 0;
			}
		},
		[this](float T)
		{
			if (Target.IsValid() && Target->IsKnockedOut())
			{
				CountB |= 2;
			}
			if (CountA == 0 && T > 0.15f)
			{
				CountA = 1;
				Penguin->ActivateAbility(0);
			}
		},
		[this]
		{
			const UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			Report(FString::Printf(TEXT("RESULT ice shield stomp: frozen before %d, knocked out %d (want 0), thawed %d"),
				(CountB & 1) ? 1 : 0, (CountB & 2) ? 1 : 0, TargetMove && !TargetMove->IsFrozen() ? 1 : 0));
			if (Target.IsValid())
			{
				Target->ResetForRound(Target->GetActorLocation());
			}
		});

	AddStunControlStep();
	AddIceShieldMatrixStep();
	AddAbilityWhileBusyStep();
}

namespace PenguinAutoTestIce
{
	// The slap stun control step (APenguinAutoTest::AddStunControlStep).
	struct FStunControl
	{
		float HitT = -1.f;
		float EndT = -1.f;
		float StunAtHit = 0.f;
		float MaxToward = 0.f;       // toward the dummy (+X) while stunned
		float MaxTowardAfter = 0.f;  // in the half second after it
		float PrevVz = 0.f;
		int32 UsesBefore = 0;
		int32 UsesDuring = -1;
		int32 UsesAfter = -1;
		int32 Done = 0;              // the presses made (bits)
		bool bJumpedStunned = false;
		bool bDashedStunned = false;
		bool bJumpedAfter = false;
		bool bDizzy = false;
	};

	// The ice shield matrix (APenguinAutoTest::AddIceShieldMatrixStep): every kind of hit on a frozen penguin.
	enum class EWant : uint8 { Absorbed, Pushed, LeftAlone, Yanked, Crushed, SecondLands, Bounced };
	struct FIceCase
	{
		const TCHAR* Name;
		EPenguinAbility Source;
		FVector2D Knockback;
		float Stun;
		bool bDeadly;
		EPenguinKOReason Reason;
		bool bBellySlide;
		float Slip;
		EWant Want;
	};
	// Made the way each attack makes its hit (PenguinAbilities.cpp, PenguinProjectile.cpp, the belly slide in PenguinCharacter.cpp).
	static const FIceCase Cases[] = {
		{ TEXT("slap"), EPenguinAbility::FlipperSlap, FVector2D(650.f, 300.f), 1.f, false, EPenguinKOReason::None, false, 0.f, EWant::Absorbed },
		{ TEXT("snowball"), EPenguinAbility::Snowball, FVector2D(1300.f, 480.f), 0.4f, false, EPenguinKOReason::None, false, 0.f, EWant::Bounced },
		{ TEXT("icicle"), EPenguinAbility::Icicle, FVector2D(900.f, 700.f), 0.3f, true, EPenguinKOReason::Icicle, false, 0.f, EWant::Absorbed },
		{ TEXT("snow bomb blast"), EPenguinAbility::SnowBomb, FVector2D(1000.f, 700.f), 0.5f, false, EPenguinKOReason::None, false, 0.f, EWant::Absorbed },
		{ TEXT("snow bomb core"), EPenguinAbility::SnowBomb, FVector2D(1200.f, 480.f), 0.3f, true, EPenguinKOReason::Bomb, false, 0.f, EWant::Absorbed },
		{ TEXT("belly stomp"), EPenguinAbility::BellyStomp, FVector2D(700.f, 1000.f), 0.3f, true, EPenguinKOReason::Stomp, false, 0.f, EWant::Absorbed },
		{ TEXT("belly slide"), EPenguinAbility::None, FVector2D(1300.f, 300.f), 0.4f, false, EPenguinKOReason::None, true, 0.f, EWant::Pushed },
		{ TEXT("noot"), EPenguinAbility::Noot, FVector2D(1600.f, 400.f), 0.45f, false, EPenguinKOReason::None, false, 0.f, EWant::Pushed },
		{ TEXT("gust"), EPenguinAbility::Gust, FVector2D(1500.f, 380.f), 0.45f, false, EPenguinKOReason::None, false, 0.f, EWant::Pushed },
		{ TEXT("ice gun"), EPenguinAbility::IceGun, FVector2D(450.f, 180.f), 0.f, false, EPenguinKOReason::None, false, 3.f, EWant::LeftAlone },
		{ TEXT("hook"), EPenguinAbility::FishHook, FVector2D(-700.f, 900.f), 1.2f, false, EPenguinKOReason::None, false, 0.f, EWant::Yanked },
		{ TEXT("two bomb blasts"), EPenguinAbility::SnowBomb, FVector2D(1000.f, 700.f), 0.5f, false, EPenguinKOReason::None, false, 0.f, EWant::SecondLands },
		{ TEXT("iceberg"), EPenguinAbility::IcebergDrop, FVector2D(500.f, 300.f), 0.f, true, EPenguinKOReason::Iceberg, false, 0.f, EWant::Crushed },
	};
	constexpr int32 NumCases = UE_ARRAY_COUNT(Cases);
	constexpr float CaseTime = 0.8f;
	constexpr float TwiceTime = 1.3f;   // two snowballs: the second after the ice's guard (0.4 s)
	constexpr float TargetOffset = 330.f;

	struct FIceMatrixState
	{
		int32 Case = 0;
		int32 Stage = 0;          // 0 put in place, 1 frozen, 2 hit, 3 (two snowballs) the second one, 4 judged, 5 done
		float CaseT = 0.f;        // when this case started
		float HitX = 0.f;
		int32 HitsAtStart = 0;
		bool bFrozenBefore = false;
		bool bFirstChecked = false;
		bool bFirstAbsorbed = false;
		int32 Right = 0;
	};
}

void APenguinAutoTest::AddStunControlStep()
{
	// ---- Our own penguin slapped: for that second the stick, a jump, a dash and an ability do nothing (a jump pressed early in
	// the stun is dropped, not saved for later); right after it, it walks, uses an ability and jumps again.
	using PenguinAutoTestIce::FStunControl;
	const TSharedRef<FStunControl> State = MakeShared<FStunControl>();
	AddStep(TEXT("slap_StunControl"), 3.7f,
		[this, State]
		{
			PrepareBrawl(110.f, { EPenguinAbility::Snowball });
			*State = FStunControl();
		},
		[this, State](float T)
		{
			FStunControl& C = *State;
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			if (!Target.IsValid() || !Abilities)
			{
				return;
			}
			const float Vz = static_cast<float>(Move->Velocity.Z);
			if (C.HitT < 0.f)
			{
				if (T > 0.2f)
				{
					// The dummy slaps us (to the left, away from it); we push toward it all along.
					C.HitT = T;
					C.UsesBefore = Abilities->GetUseCount(0);
					FPenguinHit Hit;
					Hit.Instigator = Target.Get();
					Hit.Source = EPenguinAbility::FlipperSlap;
					Hit.Knockback = FVector2D(-650.f, 300.f);
					Hit.Stun = 2.f; // the slap's own stun (UPenguinAbilityComponent's SlapStun)
					Hit.Location = Penguin->GetActorLocation();
					Penguin->ApplyPenguinHit(Hit);
					C.StunAtHit = Move->GetStunRemaining();
					C.PrevVz = static_cast<float>(Move->Velocity.Z);
					VirtualInput = FVector2D(1.f, 0.f);
				}
				return;
			}
			const float Since = T - C.HitT;
			if (C.EndT < 0.f)
			{
				if (Move->IsStunned())
				{
					C.MaxToward = FMath::Max(C.MaxToward, static_cast<float>(Move->Velocity.X));
					C.bDizzy |= Penguin->IsDizzy();
					// (a jump kicks the speed up past 200 at once; landing only brings a fall to 0)
					C.bJumpedStunned |= Since > 0.05f && Vz > 200.f && Vz - C.PrevVz > 150.f;
					C.bDashedStunned |= Move->IsDashing();
					if (!(C.Done & 1) && Since > 0.15f) { C.Done |= 1; Move->JumpPressed(); }
					if (!(C.Done & 2) && Since > 0.25f) { C.Done |= 2; Move->JumpReleased(); }
					if (!(C.Done & 4) && Since > 0.45f)
					{
						C.Done |= 4;
						Penguin->SetAimPoint(Target->GetAimOrigin());
						Penguin->ActivateAbility(0);
					}
					if (!(C.Done & 8) && Since > 0.6f) { C.Done |= 8; Move->DashPressed(); }
				}
				else
				{
					C.EndT = T;
					C.UsesDuring = Abilities->GetUseCount(0) - C.UsesBefore;
				}
			}
			else
			{
				const float After = T - C.EndT;
				if (After < 0.5f)
				{
					C.MaxTowardAfter = FMath::Max(C.MaxTowardAfter, static_cast<float>(Move->Velocity.X));
				}
				if (!(C.Done & 16) && After > 0.5f)
				{
					C.Done |= 16;
					VirtualInput = FVector2D::ZeroVector;
					Penguin->SetAimPoint(Target->GetAimOrigin());
					Penguin->ActivateAbility(0);
					C.UsesAfter = Abilities->GetUseCount(0) - C.UsesBefore - FMath::Max(C.UsesDuring, 0);
				}
				if (!(C.Done & 32) && After > 0.65f)
				{
					C.Done |= 32;
					Move->JumpPressed();
				}
				if ((C.Done & 32) && !(C.Done & 64))
				{
					C.bJumpedAfter |= Vz > 200.f && (Vz - C.PrevVz > 150.f || !Move->IsMovingOnGround());
					if (After > 0.85f)
					{
						C.Done |= 64;
						Move->JumpReleased();
					}
				}
			}
			C.PrevVz = Vz;
		},
		[this, State]
		{
			const FStunControl& C = *State;
			VirtualInput = FVector2D::ZeroVector;
			Move->JumpReleased();
			Report(FString::Printf(TEXT("RESULT slap stun control: our penguin stunned %.2f s (want ~2, from %.2f), toward the dummy while stunned at most %.0f cm/s (want ~0), "
				"jumped while stunned %d, dashed %d, abilities used %d (want 0 0 0), dizzy %d; then walked at %.0f cm/s (want > 150), ability used %d (want 1), jumped %d (want 1)"),
				C.HitT >= 0.f && C.EndT >= 0.f ? C.EndT - C.HitT : -1.f, C.StunAtHit, C.MaxToward, C.bJumpedStunned ? 1 : 0, C.bDashedStunned ? 1 : 0,
				C.UsesDuring, C.bDizzy ? 1 : 0, C.MaxTowardAfter, C.UsesAfter, C.bJumpedAfter ? 1 : 0));
		});
}

namespace PenguinAutoTestBusy
{
	// The cases of APenguinAutoTest::AddAbilityWhileBusyStep, one after another (each from a fresh start), and when each starts.
	enum EBusyCase : int32 { CaseKnockback, CaseDash, CaseSlide, CaseFrozen, CaseDizzy, NumBusyCases };
	static const float CaseStart[NumBusyCases + 1] = { 0.f, 1.4f, 2.8f, 5.4f, 7.3f, 9.2f };
	// The dummy far ahead: the dash and the slide must not run into it.
	constexpr float DummyOffset = 1500.f;

	struct FCaseResult
	{
		bool bTried = false;
		bool bUsed = false;          // ActivateAbility said so ...
		int32 Uses = 0;              // ... and the slot's use count moved by this much
		bool bState = false;         // the state the case is about was on when we tried (stunned in the air / dashing / ...)
		bool bExtra = false;         // knockback: the hit stun was no slap's; slide: lying low; frozen / dizzy: (dizzy) stars
		bool bAfter = false;         // knockback: still stunned after the use; dash / slide: ended normally; frozen / dizzy: used once it was over
		float Number = 0.f;          // dash: cm dashed; slide: cm slid
	};

	struct FBusyState
	{
		int32 Case = -1;
		int32 Stage = 0;
		float EventT = -1.f;
		float StartX = 0.f;
		int32 UsesAtStart = 0;
		FCaseResult Results[NumBusyCases];
	};
}

void APenguinAutoTest::AddAbilityWhileBusyStep()
{
	// ---- Abilities in the middle of things. Knocked back by an ordinary hit (its stun on, in the air), dashing, belly
	// sliding: a snowball comes out, and the dash and the slide go on and end as always. Frozen or dizzy from a slap: nothing
	// comes out - and once the ice or the dizzy stun is over it works again.
	using namespace PenguinAutoTestBusy;
	const TSharedRef<FBusyState> State = MakeShared<FBusyState>();
	AddStep(TEXT("ability_WhileBusy"), CaseStart[NumBusyCases] + 0.2f,
		[State]
		{
			*State = PenguinAutoTestBusy::FBusyState();
		},
		[this, State](float T)
		{
			using namespace PenguinAutoTestBusy;
			FBusyState& S = *State;
			const int32 NextCase = S.Case + 1;
			if (NextCase < NumBusyCases && T >= CaseStart[NextCase])
			{
				// A fresh start for the next case: the snowball ready, nothing left over from the last one.
				S.Case = NextCase;
				S.Stage = 0;
				S.EventT = -1.f;
				PrepareBrawl(DummyOffset, { EPenguinAbility::Snowball });
				S.StartX = static_cast<float>(Location().X);
				S.UsesAtStart = Penguin->GetAbilities()->GetUseCount(0);
			}
			if (S.Case < 0 || S.Case >= NumBusyCases || !Target.IsValid())
			{
				return;
			}
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			FCaseResult& R = S.Results[S.Case];
			const float Since = T - CaseStart[S.Case];
			// A snowball up and back (away from the dummy).
			const auto Fire = [this, Abilities, &S, &R]()
			{
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(-300.f, 0.f, 300.f));
				R.bTried = true;
				R.bUsed = Penguin->ActivateAbility(0);
				R.Uses = Abilities->GetUseCount(0) - S.UsesAtStart;
				return R.bUsed;
			};
			// The dummy hits us (it is far away: the hit is made by hand, the way the attack makes it).
			const auto HitUs = [this](EPenguinAbility Source, const FVector2D& Knockback, float Stun, float Freeze)
			{
				FPenguinHit Hit;
				Hit.Instigator = Target.Get();
				Hit.Source = Source;
				Hit.Knockback = Knockback;
				Hit.Stun = Stun;
				Hit.Freeze = Freeze;
				Hit.Location = Penguin->GetActorLocation();
				Penguin->ApplyPenguinHit(Hit);
			};

			switch (S.Case)
			{
			case CaseKnockback:
				// A snowball's hit sends us flying up and to the left, stunned for 0.45 s; in the flight we throw one back.
				if (S.Stage == 0 && Since > 0.3f)
				{
					S.Stage = 1;
					S.EventT = T;
					HitUs(EPenguinAbility::Snowball, FVector2D(-600.f, 650.f), 0.45f, 0.f);
				}
				else if (S.Stage == 1 && T > S.EventT + 0.08f && Move->IsStunned() && !Move->IsMovingOnGround())
				{
					S.Stage = 2;
					R.bState = true;
					R.bExtra = !Move->IsSlapDizzy();
					Fire();
					R.bAfter = Move->IsStunned(); // the ability does not end the hit stun: the knock still counts
				}
				else if (S.Stage == 1 && Since > 1.f)
				{
					S.Stage = 2; // never saw the flight
				}
				break;

			case CaseDash:
				// A ground dash to the right; the snowball comes out while it dashes (on the ground: no recoil, the dash goes on).
				if (S.Stage == 0 && Since > 0.35f && Move->IsMovingOnGround())
				{
					S.Stage = 1;
					S.StartX = static_cast<float>(Location().X);
					Move->DashPressed();
				}
				else if (S.Stage == 1 && Move->IsDashing())
				{
					S.Stage = 2;
					R.bState = true;
					Fire();
				}
				else if (S.Stage == 1 && Since > 0.8f)
				{
					S.Stage = 2; // no dash
				}
				else if (S.Stage == 2 && Since > 1.25f)
				{
					S.Stage = 3;
					R.Number = static_cast<float>(Location().X) - S.StartX;
					R.bAfter = !Move->IsDashing() && Move->IsMovingOnGround();
				}
				break;

			case CaseSlide:
				// A belly slide to the right; the snowball comes out a moment into it, lying low; the slide slides on and ends,
				// and the penguin stands up again.
				if (S.Stage == 0 && Since > 0.35f && Move->IsMovingOnGround())
				{
					S.Stage = 1;
					S.StartX = static_cast<float>(Location().X);
					Penguin->BellySlide();
				}
				else if (S.Stage == 1 && Move->IsBellySliding())
				{
					if (S.EventT < 0.f)
					{
						S.EventT = T;
					}
					else if (T > S.EventT + 0.1f)
					{
						S.Stage = 2;
						R.bState = true;
						R.bExtra = Move->IsLyingLow();
						Fire();
						S.EventT = T;
					}
				}
				else if (S.Stage == 1 && Since > 0.9f)
				{
					S.Stage = 3; // no slide
				}
				else if (S.Stage == 2 && T > S.EventT + 0.05f)
				{
					S.Stage = 3;
					R.bAfter = Move->IsBellySliding(); // it slides on after the throw (judged with the end below)
				}
				else if (S.Stage == 3 && Since > 2.45f)
				{
					S.Stage = 4;
					R.Number = static_cast<float>(Location().X) - S.StartX;
					R.bAfter = R.bAfter && !Move->IsBellySliding() && !Move->IsLyingLow() && Move->IsMovingOnGround();
				}
				break;

			case CaseFrozen:
				// Frozen for a second: the press does nothing; thawed, the next one throws.
				if (S.Stage == 0 && Since > 0.35f)
				{
					S.Stage = 1;
					HitUs(EPenguinAbility::FrostBreath, FVector2D::ZeroVector, 0.f, 1.f);
				}
				else if (S.Stage == 1 && Since > 0.6f)
				{
					S.Stage = 2;
					R.bState = Move->IsFrozen();
					R.bExtra = Penguin->IsDizzy(); // (no stars in the ice)
					Fire();
				}
				else if (S.Stage == 2 && Since > 1.6f)
				{
					S.Stage = 3;
					R.bAfter = !Move->IsFrozen() && Penguin->ActivateAbility(0);
				}
				break;

			case CaseDizzy:
				// Slapped (its second of dizzy stun): the press does nothing; once the stars are gone, the next one throws.
				if (S.Stage == 0 && Since > 0.35f)
				{
					S.Stage = 1;
					HitUs(EPenguinAbility::FlipperSlap, FVector2D(-650.f, 300.f), 1.f, 0.f);
				}
				else if (S.Stage == 1 && Since > 0.6f)
				{
					S.Stage = 2;
					R.bState = Move->IsSlapDizzy() && Move->IsStunned();
					R.bExtra = Penguin->IsDizzy();
					Fire();
				}
				else if (S.Stage == 2 && Since > 1.65f)
				{
					S.Stage = 3;
					R.bAfter = !Move->IsSlapDizzy() && !Move->IsStunned() && Penguin->ActivateAbility(0);
				}
				break;

			default:
				break;
			}
		},
		[this, State]
		{
			using namespace PenguinAutoTestBusy;
			const FBusyState& S = *State;
			const FCaseResult& K = S.Results[CaseKnockback];
			const bool bKnockOk = K.bTried && K.bUsed && K.Uses == 1 && K.bState && K.bExtra && K.bAfter;
			Report(FString::Printf(TEXT("RESULT ability in a knockback flight: tried %d (stunned in the air %d, no slap %d), used %d, uses %d (want 1), "
				"still stunned after it %d: %s"), K.bTried ? 1 : 0, K.bState ? 1 : 0, K.bExtra ? 1 : 0, K.bUsed ? 1 : 0, K.Uses,
				K.bAfter ? 1 : 0, bKnockOk ? TEXT("ok") : TEXT("FAIL")));

			const FCaseResult& D = S.Results[CaseDash];
			const bool bDashOk = D.bTried && D.bUsed && D.Uses == 1 && D.bState && D.bAfter && D.Number > 150.f;
			Report(FString::Printf(TEXT("RESULT ability in a dash: tried while dashing %d, used %d, uses %d (want 1), the dash went %.0f cm and ended "
				"on the ground %d: %s"), D.bState ? 1 : 0, D.bUsed ? 1 : 0, D.Uses, D.Number, D.bAfter ? 1 : 0, bDashOk ? TEXT("ok") : TEXT("FAIL")));

			const FCaseResult& L = S.Results[CaseSlide];
			const bool bSlideOk = L.bTried && L.bUsed && L.Uses == 1 && L.bState && L.bExtra && L.bAfter && L.Number > 250.f;
			Report(FString::Printf(TEXT("RESULT ability in a belly slide: tried while sliding %d (lying low %d), used %d, uses %d (want 1), slid on, "
				"ended and stood up %d, slid %.0f cm: %s"), L.bState ? 1 : 0, L.bExtra ? 1 : 0, L.bUsed ? 1 : 0, L.Uses, L.bAfter ? 1 : 0,
				L.Number, bSlideOk ? TEXT("ok") : TEXT("FAIL")));

			const FCaseResult& F = S.Results[CaseFrozen];
			const bool bFrozenOk = F.bTried && F.bState && !F.bUsed && F.Uses == 0 && F.bAfter;
			Report(FString::Printf(TEXT("RESULT ability while frozen: tried in the ice %d, used %d, uses %d (want 0 0), thawed it works again %d: %s"),
				F.bState ? 1 : 0, F.bUsed ? 1 : 0, F.Uses, F.bAfter ? 1 : 0, bFrozenOk ? TEXT("ok") : TEXT("FAIL")));

			const FCaseResult& Z = S.Results[CaseDizzy];
			const bool bDizzyOk = Z.bTried && Z.bState && Z.bExtra && !Z.bUsed && Z.Uses == 0 && Z.bAfter;
			Report(FString::Printf(TEXT("RESULT ability while slap-dizzy: tried dizzy %d (stars %d), used %d, uses %d (want 0 0), after the stun it works "
				"again %d: %s"), Z.bState ? 1 : 0, Z.bExtra ? 1 : 0, Z.bUsed ? 1 : 0, Z.Uses, Z.bAfter ? 1 : 0, bDizzyOk ? TEXT("ok") : TEXT("FAIL")));
			VirtualInput = FVector2D::ZeroVector;
		});
}

void APenguinAutoTest::AddIceShieldMatrixStep()
{
	// ---- Every kind of hit on a frozen penguin, made the way each attack makes it: what smashes ice is used up on the ice
	// (no knock, no stun, no knock-out); a snowball only bursts on it (a small nudge, still frozen); wind and a belly slide push
	// the ice cube (still frozen); an ice bolt leaves the ice
	// alone; the hook yanks the whole ice cube (hook_IceCube: it shatters where it lands); an iceberg still crushes; and once
	// the ice has taken a hit, the next one lands.
	using namespace PenguinAutoTestIce;
	const TSharedRef<FIceMatrixState> State = MakeShared<FIceMatrixState>();
	float Duration = 0.9f;
	for (const FIceCase& Case : Cases)
	{
		Duration += Case.Want == EWant::SecondLands ? TwiceTime : CaseTime;
	}
	AddStep(TEXT("ice_ShieldMatrix"), Duration,
		[this, State]
		{
			PenguinAutoTestIce::FIceMatrixState& M = *State;
			PrepareBrawl(PenguinAutoTestIce::TargetOffset, { EPenguinAbility::Snowball });
			M = PenguinAutoTestIce::FIceMatrixState();
		},
		[this, State](float T)
		{
			using namespace PenguinAutoTestIce;
			FIceMatrixState& M = *State;
			UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (!TargetMove || M.Case >= NumCases)
			{
				return;
			}
			const FIceCase& Case = Cases[M.Case];
			const float Local = T - M.CaseT;
			const auto MakeHit = [this, &Case]()
			{
				FPenguinHit Hit;
				Hit.Instigator = Penguin.Get();
				Hit.Source = Case.Source;
				Hit.Knockback = Case.Knockback;
				Hit.Stun = Case.Stun;
				Hit.bDeadly = Case.bDeadly;
				Hit.Reason = Case.Reason;
				Hit.bBellySlide = Case.bBellySlide;
				Hit.Slip = Case.Slip;
				Hit.Location = Target->GetActorLocation();
				return Hit;
			};
			if (M.Stage == 0)
			{
				ResetTarget(FVector(FlatIslandX - 300.f + TargetOffset, 0.f, 62.f));
				M.Stage = 1;
				return;
			}
			if (M.Stage == 1 && Local > 0.08f)
			{
				FPenguinHit Freeze;
				Freeze.Instigator = Penguin.Get();
				Freeze.Source = EPenguinAbility::FrostBreath;
				Freeze.Freeze = 2.6f;
				Freeze.Stun = 0.f;
				Freeze.Location = Target->GetActorLocation();
				Target->ApplyPenguinHit(Freeze);
				M.bFrozenBefore = TargetMove->IsFrozen();
				M.Stage = 2;
				return;
			}
			if (M.Stage == 2 && Local > 0.18f)
			{
				M.HitsAtStart = TargetMove->HitEvents;
				M.HitX = static_cast<float>(Target->GetActorLocation().X);
				Target->ApplyPenguinHit(MakeHit());
				M.Stage = Case.Want == EWant::SecondLands ? 3 : 4;
				return;
			}
			if (M.Stage == 3)
			{
				if (!M.bFirstChecked && Local > 0.3f)
				{
					M.bFirstChecked = true;
					M.bFirstAbsorbed = !TargetMove->IsFrozen() && TargetMove->HitEvents == M.HitsAtStart && !TargetMove->IsStunned();
				}
				if (Local > 0.7f) // (after the ice's guard: the same attack is ignored for a moment after the ice took it)
				{
					M.HitX = static_cast<float>(Target->GetActorLocation().X);
					Target->ApplyPenguinHit(MakeHit());
					M.Stage = 4;
				}
				return;
			}
			const float JudgeAt = Case.Want == EWant::SecondLands ? 1.15f : 0.63f;
			if (M.Stage == 4 && Local > JudgeAt)
			{
				const float Moved = FMath::Abs(static_cast<float>(Target->GetActorLocation().X) - M.HitX);
				const bool bThawed = !TargetMove->IsFrozen();
				const int32 Hits = TargetMove->HitEvents - M.HitsAtStart;
				const bool bKO = Target->IsKnockedOut();
				const bool bStunned = TargetMove->IsStunned();
				bool bOk = M.bFrozenBefore;
				const TCHAR* Want = TEXT("");
				switch (Case.Want)
				{
				case EWant::Absorbed:
					Want = TEXT("the ice takes it (thawed, not moved, no hit, no stun, not out)");
					bOk = bOk && bThawed && Moved < 25.f && Hits == 0 && !bStunned && !bKO && !Target->IsDizzy();
					break;
				case EWant::Pushed:
					Want = TEXT("the ice cube slides off (still frozen, moved, no hit)");
					bOk = bOk && !bThawed && Moved > 60.f && Hits == 0 && !bKO;
					break;
				case EWant::LeftAlone:
					Want = TEXT("the ice stays (still frozen, no hit)");
					bOk = bOk && !bThawed && Hits == 0 && !bKO;
					break;
				case EWant::Yanked:
					Want = TEXT("the ice holds, the whole ice cube is yanked (still frozen, one hit, moved)");
					bOk = bOk && !bThawed && Hits == 1 && Moved > 60.f && !bKO;
					break;
				case EWant::Crushed:
					Want = TEXT("crushed (knocked out)");
					bOk = bOk && bKO;
					break;
				case EWant::Bounced:
					Want = TEXT("the ice shrugs it off (still frozen, nudged a little, no hit, no stun, not out)");
					bOk = bOk && !bThawed && Moved > 5.f && Moved < 150.f && Hits == 0 && !bStunned && !bKO && !Target->IsDizzy();
					break;
				case EWant::SecondLands:
					Want = TEXT("the ice takes the first, the second lands (one hit, moved)");
					bOk = bOk && M.bFirstAbsorbed && bThawed && Hits == 1 && Moved > 60.f && !bKO;
					break;
				}
				M.Right += bOk ? 1 : 0;
				Report(FString::Printf(TEXT("RESULT ice shield vs %s: %s - frozen before %d, thawed %d, moved %.0f cm, hits %d, stunned %d, knocked out %d%s: %s"),
					Case.Name, Want, M.bFrozenBefore ? 1 : 0, bThawed ? 1 : 0, Moved, Hits, bStunned ? 1 : 0, bKO ? 1 : 0,
					Case.Want == EWant::SecondLands ? *FString::Printf(TEXT(", first taken by the ice %d"), M.bFirstAbsorbed ? 1 : 0) : TEXT(""),
					bOk ? TEXT("ok") : TEXT("WRONG")));
				M.Stage = 5;
			}
			if (M.Stage == 5 && Local > (Case.Want == EWant::SecondLands ? TwiceTime : CaseTime))
			{
				++M.Case;
				M.Stage = 0;
				M.CaseT = T;
				M.bFrozenBefore = false;
				M.bFirstChecked = false;
				M.bFirstAbsorbed = false;
			}
		},
		[this, State]
		{
			using namespace PenguinAutoTestIce;
			const FIceMatrixState& M = *State;
			Report(FString::Printf(TEXT("RESULT ice shield matrix: %d of %d right (want all), %d cases run"), M.Right, NumCases, M.Case));
			ResetTarget(FVector(FlatIslandX - 300.f + TargetOffset, 0.f, 62.f));
		});
}

namespace PenguinAutoTestIce
{
	/** hook_IceCube: the frozen dummy yanked by the fish hook. */
	struct FHookCube
	{
		float FireT = -1.f;
		float YankT = -1.f;
		float LandT = -1.f;
		float ThawT = -1.f;
		float StartX = 0.f;       // the dummy at the yank
		float ThrowerX = 0.f;
		float LandX = 0.f;
		int32 HitsAtStart = 0;
		bool bFrozenAtYank = false;
		bool bFrozenMid = false;
		bool bMidSampled = false;
		// two samples of its rise / fall in the air: the gravity it flies with
		float SampleT[2] = { -1.f, -1.f };
		float SampleVz[2] = { 0.f, 0.f };
		float NearestToThrower = 1.e9f; // (it never gets past the thrower: it is no over-the-head throw)
		bool bAlive = true;
	};

	/** hook_IceBlock: an ice block of the island pulled out by the fish hook. */
	struct FHookBlock
	{
		FIntPoint Cell = FIntPoint(0, 0);
		int32 BrokenAtStart = 0;
		int32 IceAtStart = 0;
		float FireT = -1.f;
		float PullT = -1.f;
		float LandT = -1.f;
		bool bFlyerSeen = false;
		float SampleT[2] = { -1.f, -1.f };
		float SampleVz[2] = { 0.f, 0.f };
		int32 LandColumn = TNumericLimits<int32>::Lowest();
		int32 LandRow = TNumericLimits<int32>::Lowest();
	};

	/** frozen_Gravity: a frozen dummy's rise and fall against an unfrozen one's. */
	struct FFrozenGravity
	{
		int32 Stage = 0;
		float StageT = 0.f;
		float RiseVz[2] = { 0.f, 0.f };
		float RiseT[2] = { -1.f, -1.f };
		float FrozenRise = 0.f;
		float FrozenFall = 0.f;
		float NormalRise = 0.f;
		float NormalFall = 0.f;
		bool bFrozenAll = true;
	};

	/** Gravity from two samples of the vertical speed (cm/s^2, positive), 0 without both. */
	static float GravityFrom(const float Times[2], const float Speeds[2])
	{
		return Times[0] >= 0.f && Times[1] > Times[0] + 0.01f ? (Speeds[0] - Speeds[1]) / (Times[1] - Times[0]) : 0.f;
	}
}

void APenguinAutoTest::AddHookIceSteps()
{
	using namespace PenguinAutoTestIce;
	// ---- A frozen dummy hooked: the ice holds - the whole ice cube, heavy, comes about halfway on a low lob (falling like a
	// block of the island, still frozen in the air) and shatters where it lands. Never thrown over the thrower's head.
	const TSharedRef<FHookCube> Cube = MakeShared<FHookCube>();
	AddStep(TEXT("hook_IceCube"), 2.0f,
		[this, Cube]
		{
			PrepareBrawl(600.f, { EPenguinAbility::FishHook });
			*Cube = FHookCube();
		},
		[this, Cube](float T)
		{
			FHookCube& C = *Cube;
			UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (!TargetMove)
			{
				return;
			}
			if (C.FireT < 0.f)
			{
				if (T > 0.1f && !TargetMove->IsFrozen())
				{
					FPenguinHit Freeze;
					Freeze.Instigator = Penguin.Get();
					Freeze.Source = EPenguinAbility::FrostBreath;
					Freeze.Freeze = 2.6f;
					Freeze.Stun = 0.f;
					Freeze.Location = Target->GetActorLocation();
					Target->ApplyPenguinHit(Freeze);
				}
				if (T > 0.3f)
				{
					C.FireT = T;
					C.HitsAtStart = TargetMove->HitEvents;
					Penguin->SetAimPoint(Target->GetAimOrigin());
					Penguin->ActivateAbility(0);
				}
				return;
			}
			const float X = static_cast<float>(Target->GetActorLocation().X);
			if (C.YankT < 0.f)
			{
				if (TargetMove->HitEvents != C.HitsAtStart)
				{
					C.YankT = T;
					C.StartX = X;
					C.ThrowerX = static_cast<float>(Location().X);
					C.bFrozenAtYank = TargetMove->IsFrozen();
				}
				return;
			}
			const float Since = T - C.YankT;
			C.bAlive &= !Target->IsKnockedOut();
			C.NearestToThrower = FMath::Min(C.NearestToThrower, (X - C.ThrowerX) * FMath::Sign(C.StartX - C.ThrowerX));
			const bool bAir = !TargetMove->IsMovingOnGround();
			if (C.LandT < 0.f)
			{
				for (int32 Index = 0; Index < 2; ++Index)
				{
					if (C.SampleT[Index] < 0.f && bAir && Since > (Index == 0 ? 0.04f : 0.13f))
					{
						C.SampleT[Index] = T;
						C.SampleVz[Index] = static_cast<float>(TargetMove->Velocity.Z);
					}
				}
				if (!C.bMidSampled && Since > 0.1f && bAir)
				{
					C.bMidSampled = true;
					C.bFrozenMid = TargetMove->IsFrozen();
				}
				if (Since > 0.03f && !bAir)
				{
					C.LandT = T;
					C.LandX = X;
				}
			}
			if (C.ThawT < 0.f && !TargetMove->IsFrozen())
			{
				C.ThawT = T;
			}
		},
		[this, Cube]
		{
			const FHookCube& C = *Cube;
			const float Gap = FMath::Abs(C.StartX - C.ThrowerX);
			const float Came = C.LandT >= 0.f ? FMath::Abs(C.LandX - C.StartX) : 0.f;
			Report(FString::Printf(TEXT("RESULT hook ice cube: yanked %d (frozen %d), still frozen in the air %d, flying with gravity %.0f (want %.0f: a falling block's), "
				"came %.0f of %.0f cm (%.0f%%, want about half), never past the thrower %d, the ice shattered %.2f s after it landed (want ~0), alive %d"),
				C.YankT >= 0.f ? 1 : 0, C.bFrozenAtYank ? 1 : 0, C.bFrozenMid ? 1 : 0, GravityFrom(C.SampleT, C.SampleVz), ABlockIsland::FallingBlockGravity,
				Came, Gap, Gap > 1.f ? 100.f * Came / Gap : 0.f, C.NearestToThrower > 0.f ? 1 : 0,
				C.LandT >= 0.f && C.ThawT >= 0.f ? C.ThawT - C.LandT : -1.f, C.bAlive ? 1 : 0));
		});

	// ---- An ice block of the island hooked: pulled out whole (not broken), it comes about halfway on a low lob, falling like
	// a block, and is a block of the island again where it lands.
	const TSharedRef<FHookBlock> Block = MakeShared<FHookBlock>();
	AddStep(TEXT("hook_IceBlock"), 2.0f,
		[this, Block]
		{
			PrepareBrawl(-400.f, { EPenguinAbility::FishHook }); // the dummy behind us, out of the way
			*Block = FHookBlock();
			if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f)))
			{
				// One lasting ice block standing on the strip, 6 m ahead.
				Block->Cell = Flat->WorldToCell(Location() + FVector(600.f, 0.f, 0.f));
				Block->Cell.Y = 0;
				Flat->AddBlocks({ Block->Cell }, EBlockKind::Ice, 0.f);
				Flat->FlushEdits();
				Block->BrokenAtStart = Flat->GetBrokenBlocks();
				Block->IceAtStart = CountIce(static_cast<float>(Location().X) - 200.f, 1000.f, 3, nullptr);
			}
		},
		[this, Block](float T)
		{
			FHookBlock& B = *Block;
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			if (!Flat)
			{
				return;
			}
			if (B.FireT < 0.f)
			{
				if (T > 0.3f)
				{
					B.FireT = T;
					Penguin->SetAimPoint(Flat->CellToWorld(B.Cell));
					Penguin->ActivateAbility(0);
				}
				return;
			}
			if (B.PullT < 0.f && !Flat->IsSolidCell(B.Cell))
			{
				B.PullT = T;
			}
			if (B.PullT < 0.f || B.LandT >= 0.f)
			{
				return;
			}
			const AHookedIceBlock* Flyer = nullptr;
			for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
			{
				Flyer = *It;
			}
			if (Flyer && !Flyer->HasLanded())
			{
				B.bFlyerSeen = true;
				const float Since = T - B.PullT;
				for (int32 Index = 0; Index < 2; ++Index)
				{
					if (B.SampleT[Index] < 0.f && Since > (Index == 0 ? 0.04f : 0.13f))
					{
						B.SampleT[Index] = T;
						B.SampleVz[Index] = static_cast<float>(Flyer->GetVelocity().Z);
					}
				}
				return;
			}
			if (B.bFlyerSeen || T > B.PullT + 1.f)
			{
				// Landed: where is the ice block now? (row 0 or 1 between us and where it was)
				B.LandT = T;
				const FIntPoint Ours = Flat->WorldToCell(Location());
				const int32 Step = B.Cell.X > Ours.X ? 1 : -1;
				for (int32 Column = Ours.X; Column != B.Cell.X + Step && B.LandColumn == TNumericLimits<int32>::Lowest(); Column += Step)
				{
					for (int32 Row = 0; Row <= 1; ++Row)
					{
						if (Flat->GetBlock(Column, Row) == EBlockKind::Ice)
						{
							B.LandColumn = Column;
							B.LandRow = Row;
							break;
						}
					}
				}
			}
		},
		[this, Block]
		{
			const FHookBlock& B = *Block;
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			const int32 Broken = Flat ? Flat->GetBrokenBlocks() - B.BrokenAtStart : -1;
			const int32 IceNow = CountIce(static_cast<float>(Location().X) - 200.f, 1000.f, 3, nullptr);
			const FIntPoint Ours = Flat ? Flat->WorldToCell(Location()) : FIntPoint(0, 0);
			const bool bLanded = B.LandColumn != TNumericLimits<int32>::Lowest();
			const float Gap = Flat ? FMath::Abs(B.Cell.X - Ours.X) * Flat->CellSize : 0.f;
			const float Came = bLanded && Flat ? FMath::Abs(B.Cell.X - B.LandColumn) * Flat->CellSize : 0.f;
			Report(FString::Printf(TEXT("RESULT hook ice block: pulled out %d (its cell empty), broken %d (want 0), it flew %d with gravity %.0f (want %.0f: a falling block's), "
				"a block of the island again %d (row %d, ice blocks %d -> %d), came %.0f of %.0f cm (%.0f%%, want about half)"),
				B.PullT >= 0.f ? 1 : 0, Broken, B.bFlyerSeen ? 1 : 0, GravityFrom(B.SampleT, B.SampleVz), ABlockIsland::FallingBlockGravity,
				bLanded ? 1 : 0, bLanded ? B.LandRow : -1, B.IceAtStart, IceNow, Came, Gap, Gap > 1.f ? 100.f * Came / Gap : 0.f));
		});

	// ---- A frozen penguin is an ice cube: it rises and falls like a block of the island (the same gravity up and down, a
	// block's top falling speed); unfrozen, the penguin's own (lighter on the way up, a lower top speed).
	const TSharedRef<FFrozenGravity> Grav = MakeShared<FFrozenGravity>();
	AddStep(TEXT("frozen_Gravity"), 3.6f,
		[this, Grav]
		{
			PrepareBrawl(300.f, {});
			*Grav = FFrozenGravity();
		},
		[this, Grav](float T)
		{
			FFrozenGravity& G = *Grav;
			UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (!TargetMove)
			{
				return;
			}
			const FVector Up = Location() + FVector(300.f, 0.f, 1500.f);
			const float Vz = static_cast<float>(TargetMove->Velocity.Z);
			const float Local = T - G.StageT;
			const auto NextStage = [&G, T]() { ++G.Stage; G.StageT = T; G.RiseT[0] = G.RiseT[1] = -1.f; };
			const bool bFrozenPart = G.Stage <= 3;
			if (bFrozenPart && G.Stage > 0)
			{
				G.bFrozenAll &= TargetMove->IsFrozen();
			}
			switch (G.Stage)
			{
			case 0: // frozen
				if (T > 0.1f)
				{
					FPenguinHit Freeze;
					Freeze.Instigator = Penguin.Get();
					Freeze.Source = EPenguinAbility::FrostBreath;
					Freeze.Freeze = 3.f;
					Freeze.Stun = 0.f;
					Freeze.Location = Target->GetActorLocation();
					Target->ApplyPenguinHit(Freeze);
					NextStage();
				}
				break;
			case 1: // ... sent up
				if (Local > 0.1f)
				{
					TargetMove->ApplyKnockback(FVector2D(0.f, 1100.f), 0.f);
					NextStage();
				}
				break;
			case 2: // ... its rise
				if (G.RiseT[0] < 0.f && Local > 0.02f) { G.RiseT[0] = T; G.RiseVz[0] = Vz; }
				if (G.RiseT[1] < 0.f && Local > 0.09f) { G.RiseT[1] = T; G.RiseVz[1] = Vz; G.FrozenRise = GravityFrom(G.RiseT, G.RiseVz); }
				if (Local > 0.5f)
				{
					Target->NetTeleport(Up, FVector::ZeroVector, true); // ... and dropped from high up
					NextStage();
				}
				break;
			case 3: // ... its top falling speed (reached after 0.4 s, still well up in the air)
				if (Local > 0.6f)
				{
					G.FrozenFall = -Vz;
					TargetMove->SetFrozen(0.f); // thawed: the same again
					NextStage();
				}
				break;
			case 4: // unfrozen, on the ground again
				if (TargetMove->IsMovingOnGround() && Local > 0.1f)
				{
					TargetMove->ApplyKnockback(FVector2D(0.f, 1100.f), 0.f);
					NextStage();
				}
				break;
			case 5:
				if (G.RiseT[0] < 0.f && Local > 0.02f) { G.RiseT[0] = T; G.RiseVz[0] = Vz; }
				if (G.RiseT[1] < 0.f && Local > 0.09f) { G.RiseT[1] = T; G.RiseVz[1] = Vz; G.NormalRise = GravityFrom(G.RiseT, G.RiseVz); }
				if (Local > 0.5f)
				{
					Target->NetTeleport(Up, FVector::ZeroVector, true);
					NextStage();
				}
				break;
			case 6: // (reached after 0.31 s; down after about 0.84 s)
				if (Local > 0.7f)
				{
					G.NormalFall = -Vz;
					NextStage();
				}
				break;
			default:
				break;
			}
		},
		[this, Grav]
		{
			const FFrozenGravity& G = *Grav;
			Report(FString::Printf(TEXT("RESULT frozen gravity: frozen the whole time %d; the ice cube rises against %.0f cm/s^2 and falls at most %.0f cm/s "
				"(want %.0f and %.0f: a falling block's); unfrozen %.0f and %.0f (a penguin's own: about 4475 and 2200)"),
				G.bFrozenAll ? 1 : 0, G.FrozenRise, G.FrozenFall, ABlockIsland::FallingBlockGravity, ABlockIsland::FallingBlockMaxSpeed, G.NormalRise, G.NormalFall));
			if (Target.IsValid())
			{
				Target->ResetForRound(Location() + FVector(300.f, 0.f, 0.f));
			}
		});
}

void APenguinAutoTest::AddHookIceLookStep()
{
	using namespace PenguinAutoTestIce;
	// The yanked ice cube in mid-air (the hook's line on it), then an ice block pulled out of the island in mid-air and
	// where it came down.
	const TSharedRef<FHookBlock> Look = MakeShared<FHookBlock>();
	AddStep(TEXT("look_HookIce"), 3.4f,
		[this, Look]
		{
			SpawnTarget(); // (a look-only run has no dummy yet)
			PrepareBrawl(600.f, { EPenguinAbility::FishHook });
			*Look = FHookBlock();
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, Look](float T)
		{
			FHookBlock& L = *Look;
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			UPenguinMovementComponent* TargetMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (!Flat || !TargetMove)
			{
				return;
			}
			if (AActor* Camera = LookCamera.Get())
			{
				// From the side, the whole 6 m in the picture.
				const FVector LookAt = Location() + FVector(300.f, 0.f, 60.f);
				const FVector From = LookAt + FVector(0.f, 900.f, 140.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			// 1. the frozen dummy yanked
			if (CountA == 0 && T > 0.1f)
			{
				CountA = 1;
				FPenguinHit Freeze;
				Freeze.Instigator = Penguin.Get();
				Freeze.Source = EPenguinAbility::FrostBreath;
				Freeze.Freeze = 2.6f;
				Freeze.Stun = 0.f;
				Freeze.Location = Target->GetActorLocation();
				Target->ApplyPenguinHit(Freeze);
			}
			if (CountA == 1 && T > 0.4f)
			{
				CountA = 2;
				L.FireT = T;
				L.BrokenAtStart = TargetMove->HitEvents;
				Penguin->SetAimPoint(Target->GetAimOrigin());
				Penguin->ActivateAbility(0);
			}
			if (CountA == 2 && TargetMove->HitEvents != L.BrokenAtStart)
			{
				CountA = 3;
				TimeA = T;
			}
			if (CountA == 3 && T > TimeA + 0.12f)
			{
				CountA = 4;
				Shot(TEXT("51_hook_icecube"));
			}
			// 2. an ice block pulled out of the island
			if (CountA == 4 && T > 1.3f)
			{
				CountA = 5;
				ResetTarget(Location() - FVector(400.f, 0.f, 0.f)); // out of the way
				L.Cell = Flat->WorldToCell(Location() + FVector(600.f, 0.f, 0.f));
				L.Cell.Y = 0;
				Flat->AddBlocks({ L.Cell }, EBlockKind::Ice, 0.f);
				Flat->FlushEdits();
				Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::FishHook }); // ready again
			}
			if (CountA == 5 && T > 1.5f)
			{
				CountA = 6;
				Penguin->SetAimPoint(Flat->CellToWorld(L.Cell));
				Penguin->ActivateAbility(0);
			}
			if (CountA == 6 && !Flat->IsSolidCell(L.Cell))
			{
				CountA = 7;
				TimeB = T;
			}
			if (CountA == 7 && T > TimeB + 0.12f)
			{
				CountA = 8;
				Shot(TEXT("51_hook_iceblock"));
			}
			if (CountA == 8 && T > TimeB + 0.9f)
			{
				CountA = 9;
				Shot(TEXT("51_hook_iceblock_landed"));
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
			Report(FString::Printf(TEXT("RESULT hook and ice look: shots %d"), CountA >= 9 ? 3 : (CountA >= 8 ? 2 : (CountA >= 4 ? 1 : 0))));
		});

	// The fish on the hook up close in flight (it is a grey fish now: SM_Fish's vertex colours were greyed), and how colourful
	// the fish mesh's vertex colours are (saturation: the blue back was about 0.6).
	AddStep(TEXT("look_HookFish"), 1.2f,
		[this]
		{
			PrepareBrawl(2400.f, { EPenguinAbility::FishHook });
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
			TestBombA.Reset();
			MarkA = -1.f; // average saturation of the fish's vertex colours (-1: not readable here)
			MarkB = -1.f; // the most saturated one
			const UStaticMesh* Fish = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FX/SM_Fish.SM_Fish"));
			const FStaticMeshRenderData* RenderData = Fish ? Fish->GetRenderData() : nullptr;
			if (RenderData && RenderData->LODResources.Num() > 0)
			{
				const FColorVertexBuffer& Colors = RenderData->LODResources[0].VertexBuffers.ColorVertexBuffer;
				const uint32 NumColors = Colors.GetNumVertices();
				if (NumColors > 0 && Colors.GetVertexData())
				{
					double Sum = 0.0;
					float Most = 0.f;
					for (uint32 Index = 0; Index < NumColors; ++Index)
					{
						const FColor& C = Colors.VertexColor(Index);
						const int32 Hi = FMath::Max3(static_cast<int32>(C.R), static_cast<int32>(C.G), static_cast<int32>(C.B));
						const int32 Lo = FMath::Min3(static_cast<int32>(C.R), static_cast<int32>(C.G), static_cast<int32>(C.B));
						const float Saturation = Hi > 0 ? static_cast<float>(Hi - Lo) / static_cast<float>(Hi) : 0.f;
						Sum += Saturation;
						Most = FMath::Max(Most, Saturation);
					}
					MarkA = static_cast<float>(Sum / NumColors);
					MarkB = Most;
					CountB = static_cast<int32>(NumColors);
				}
			}
		},
		[this](float T)
		{
			if (!bFlag && T > 0.25f)
			{
				bFlag = true;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(600.f, 0.f, 80.f));
				CountA = Penguin->ActivateAbility(0) ? 1 : 0;
				TimeA = T;
			}
			if (!TestBombA.IsValid())
			{
				for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
				{
					if (It->GetKind() == EPenguinProjectileKind::FishHook)
					{
						TestBombA = *It;
					}
				}
			}
			const APenguinProjectile* Hook = TestBombA.Get();
			if (AActor* Camera = LookCamera.Get())
			{
				// Close on the fish from the side, a frame ahead of it (it flies 60 m/s).
				const FVector LookAt = Hook ? Hook->GetActorLocation() + Hook->GetVelocity() / 60.f : Location() + FVector(200.f, 0.f, 40.f);
				const FVector From = LookAt + FVector(-40.f, 300.f, 50.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			if (Hook && !bShotDone && TimeA > 0.f && FVector::Dist(Hook->GetActorLocation(), Penguin->GetAimOrigin()) > 350.f)
			{
				bShotDone = true;
				Shot(TEXT("57_hook_fish"));
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
			TestBombA.Reset();
			Report(FString::Printf(TEXT("RESULT hook fish look: hook thrown %d, shot %d; fish vertex colours %d, saturation average %.2f, most %.2f (-1: not readable; the blue fish had about 0.6 at most)"),
				CountA, bShotDone ? 1 : 0, CountB, MarkA, MarkB));
		});
}

void APenguinAutoTest::AddStunLookStep()
{
	AddStep(TEXT("look_SlapStunIce"), 3.4f,
		[this]
		{
			SpawnTarget(); // (a look-only run has no dummy yet)
			PrepareBrawl(110.f, { EPenguinAbility::FlipperSlap, EPenguinAbility::Snowball });
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this](float T)
		{
			if (!Target.IsValid())
			{
				return;
			}
			if (AActor* Camera = LookCamera.Get())
			{
				// Between the two, from the stage camera's side.
				const FVector LookAt = (Penguin->GetFeetLocation() + Target->GetFeetLocation()) * 0.5f + FVector(0.f, 0.f, 70.f);
				const FVector From = LookAt + FVector(0.f, 430.f, 60.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			if (CountA == 0 && T > 0.3f)
			{
				CountA = 1;
				CountB = 0;
				TimeA = T;
				Penguin->SetAimPoint(Target->GetAimOrigin());
				Penguin->ActivateAbility(0); // slap
			}
			if (CountA == 1 && CountB == 0 && T > TimeA + 0.07f)
			{
				CountB = 1;
				Shot(TEXT("50_slap_hit")); // the swing's swoosh, the gold burst on the one slapped
			}
			if (CountA == 1 && T > TimeA + 0.8f)
			{
				CountA = 2;
				Shot(TEXT("50_slap_dizzy")); // late in the stun: the hit's burst is gone, the dizzy stars circle
			}
			if (CountA == 2 && T > 1.6f)
			{
				CountA = 3;
				FPenguinHit Hit;
				Hit.Instigator = Penguin.Get();
				Hit.Source = EPenguinAbility::FrostBreath;
				Hit.Freeze = 2.6f;
				Hit.Stun = 0.f;
				Hit.Location = Target->GetActorLocation();
				Target->ApplyPenguinHit(Hit);
			}
			if (CountA == 3 && T > 1.9f)
			{
				CountA = 4;
				Shot(TEXT("50_ice_frozen"));
				Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::FlipperSlap, EPenguinAbility::Snowball });
				Penguin->SetAimPoint(Target->GetAimOrigin());
				Penguin->ActivateAbility(1); // snowball at the ice: it only bursts on it (the ice stays)
				TimeB = T;
			}
			if (CountA == 4 && T > TimeB + 0.35f)
			{
				CountA = 5;
				// (The snowball nudged the ice cube a little way off: back in the slap's reach first.)
				Target->SetActorLocation(FVector(FlatIslandX - 300.f + 110.f, 0.f, 62.f), false, nullptr, ETeleportType::TeleportPhysics);
				Target->GetPenguinMovement()->Velocity = FVector::ZeroVector;
				Penguin->SetAimPoint(Target->GetAimOrigin());
				Penguin->ActivateAbility(0); // the slap shatters it
				TimeB = T;
			}
			if (CountA == 5 && Target->GetPenguinMovement() && !Target->GetPenguinMovement()->IsFrozen())
			{
				CountA = 6;
				TimeB = T;
			}
			if (CountA == 6 && T > TimeB + 0.06f)
			{
				CountA = 7;
				Shot(TEXT("50_ice_shattered"));
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
			Report(FString::Printf(TEXT("RESULT slap and ice look: shots %d"), (CountA >= 7 ? 3 : CountA >= 4 ? 2 : CountA >= 2 ? 1 : 0) + (CountB > 0 ? 1 : 0)));
		});
}

namespace PenguinAutoTestDeaths
{
	// The death look step (APenguinAutoTest::AddDeathLookStep): the knock-outs in order, with the launch each one's hit has.
	struct FDeathCase
	{
		EPenguinKOReason Reason;
		FVector2D Launch;
	};
	static const FDeathCase DeathCases[] = {
		{ EPenguinKOReason::Stomp, FVector2D(600.f, 900.f) },
		{ EPenguinKOReason::Iceberg, FVector2D(500.f, 300.f) },
		{ EPenguinKOReason::Icicle, FVector2D(900.f, 700.f) },
		{ EPenguinKOReason::Bomb, FVector2D(1200.f, 500.f) },
		{ EPenguinKOReason::Sea, FVector2D(0.f, 0.f) },
	};
	constexpr int32 NumDeaths = UE_ARRAY_COUNT(DeathCases);
	constexpr float DeathFirst = 0.3f;    // the first round reset
	constexpr float DeathEvery = 3.f;     // one knock-out after another (the stomp's ghost is gone before the next shot, the
	                                      // snow bomb's practice respawn - 2.3 s of real time - has come)
	constexpr float DeathKOAfter = 0.1f;  // the knock-out this long after its round reset
	constexpr float DeathResetAt = DeathFirst + NumDeaths * DeathEvery; // the last round reset
	// The stomp's ghost shot: this long after the knock-out on this step's clock, looking this far above where the dummy
	// lay. The dummy hides at 0.85 s of game time; the knock-out's hit-stop (0.12 s real time at 0.02 dilation) puts the
	// practice respawn (1.1 s of real time, somewhere else on the island) at about 0.97 s of game time, so the camera
	// stays on the spot once the dummy is gone instead of following it to where it comes back. The ghost (out at about
	// 0.85 s) has risen about this far by the shot, fully there (it fades out only late in its 1.7 s).
	constexpr float DeathSpiritShotAt = 1.4f;
	constexpr float DeathSpiritLookUp = 40.f;
	// The icicle's shatter shot: this long after the knock-out (the block bursts at 0.5 s: its chunks are still round the
	// penguin then). From the burst on, the camera stays where the block burst (the penguin tumbles on out of the middle,
	// the chunks stay in the picture), from a bit further away for the whole icicle knock-out (this far, the same pitch);
	// before 0.3 s it follows the dummy whether the ice shows yet or not.
	constexpr float DeathShatterShotAt = 0.55f;
	const FVector DeathShatterFrom(0.f, 600.f, 84.f);
	constexpr float DeathShatterFollowUntil = 0.3f;
	// The close-up camera: this far from what it looks at (from the stage camera's side, about 8 degrees down, like the
	// game's camera); a pancake from further away (it is about twice as wide as a penguin), looking this high above its
	// feet.
	const FVector DeathLookFrom(0.f, 430.f, 60.f);
	const FVector DeathPancakeFrom(0.f, 560.f, 79.f);
	constexpr float DeathPancakeLookUp = 28.f;
	/** Where the stomped dummy lay / where the icicle's ice block burst / where the bombed dummy crumbled (sampled while it
	 *  is out and still there). */
	struct FDeathGhostSpot
	{
		FVector Spot = FVector::ZeroVector;
		bool bSeen = false;
	};
	// The snow bomb: charred black within 0.25 s, standing black for 0.5 s, crumbling into its pile from 0.75 s (for 0.36 s),
	// the eyes on the pile. Four close-ups: mid char-up, black in the hold, mid-crumble - the whole penguin in the picture
	// (from DeathCharFrom, looking DeathCharLookUp above its feet) - then the pile with its eyes, closer (from DeathAshFrom,
	// DeathAshLookUp above the floor where it crumbled; the camera moves in from DeathBombPileFrom). The pile is still
	// resting at the second pile time, before the practice respawn (APenguinGameMode's BombRespawnDelay, 2.3 s of real
	// time: about 2.2 s of game time after the knock-out with its hit-stop) ends the look; after it the pile must be gone.
	const FVector DeathCharFrom(0.f, 420.f, 58.f);
	constexpr float DeathCharLookUp = 52.f;
	const FVector DeathAshFrom(0.f, 340.f, 48.f);
	constexpr float DeathAshLookUp = 28.f;
	constexpr float DeathBombCharShotAt = 0.12f;
	constexpr float DeathBombBlackShotAt = 0.5f;
	constexpr float DeathBombCollapseShotAt = 0.93f;
	constexpr float DeathBombPileFrom = 1.2f;
	constexpr float DeathAshShotAt = 1.6f;
	constexpr float DeathAshBlowShotAt = 1.9f;
	constexpr float DeathAshResetCheckAt = 2.45f;
	/** Standing charred: sampled from here to here (the char-up done, the crumble not begun). */
	constexpr float DeathHoldFrom = 0.3f;
	constexpr float DeathHoldTo = 0.72f;
	/** The crumble begins this long after the knock-out: the 0.25 s char-up and the 0.5 s hold. */
	constexpr float DeathCrumbleWant = 0.75f;
	/** What the snow bomb's knock-out did: the soot over the body mid char-up and (at least) in the hold, how far the
	 *  dummy moved in the hold, its white eyes there (fewest seen, furthest off its own eyes, their radius) and the heap
	 *  parts there (most seen: want none yet), when it began crumbling (look time, -1 never), its mesh height and heap parts
	 *  mid-crumble, the heap parts, eyes and the eyes' height over the floor at the pile shot; then the pile: shown with its
	 *  eyes, how much was blown away at the second pile time (and still showing), and whether it was gone once the dummy
	 *  was back in (-1: not back in yet). */
	struct FDeathAshSeen
	{
		float CharMid = -1.f;
		float HoldMin = 2.f;
		FVector HoldSpot = FVector::ZeroVector;
		bool bHoldSpotSeen = false;
		float HoldMoved = 0.f;
		int32 HoldEyesMin = 99;
		float HoldEyeOff = 0.f;
		float HoldEyeRadius = 0.f;
		int32 HoldHeapMax = 0;
		float CrumbleAt = -1.f;
		float CollapseFlat = 1.f;
		int32 CollapseHeap = 0;
		int32 PileHeap = 0;
		int32 PileEyes = 0;
		float PileEyeHeight = -1.f;
		int32 Pile = 0;
		float Blown = 0.f;
		int32 Blowing = 0;
		int32 AfterReset = -1;
	};

	/** The snow bomb's white eyes against the dummy's own (the Eye_L / Eye_R bones): how far the further one is off (cm,
	 *  999 when they do not show), and their radius. */
	static float BombEyesOff(const APenguinCharacter* Dummy, float& OutRadius)
	{
		FVector Left = FVector::ZeroVector;
		FVector Right = FVector::ZeroVector;
		OutRadius = 0.f;
		const USkeletalMeshComponent* DummyMesh = Dummy ? Dummy->GetMesh() : nullptr;
		if (!DummyMesh || !APinguFX::GetAshPileEyeSpots(Dummy, Left, Right, OutRadius))
		{
			return 999.f;
		}
		FVector BoneLeft = DummyMesh->GetSocketLocation(FName(TEXT("Eye_L")));
		FVector BoneRight = DummyMesh->GetSocketLocation(FName(TEXT("Eye_R")));
		if (BoneLeft.X > BoneRight.X)
		{
			Swap(BoneLeft, BoneRight);
		}
		return static_cast<float>(FMath::Max(FVector::Dist(Left, BoneLeft), FVector::Dist(Right, BoneRight)));
	}
}

void APenguinAutoTest::AddDeathLookStep()
{
	using namespace PenguinAutoTestDeaths;
	// Five knock-outs of the dummy, 2.5 s apart (each starts with a round reset, the knock-out 0.1 s later): a stomp (a
	// pancake on the spot, stars, then its ghost floating up), an iceberg (a pancake squirting out sideways along the
	// ground, wobbling), an icicle (frozen in a block that tumbles and bursts into chunks at 0.5 s), a snow bomb (sooty on
	// the spot, crumbling into an ash pile with eyes that a gust blows away), the sea (gone at once, a sinking copy,
	// bubbles, the ghost out of the water) - at the main island's edge, where the sea is. Then a last round reset:
	// everything back to normal.
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	const TSharedRef<FDeathGhostSpot> Ghost = MakeShared<FDeathGhostSpot>();
	const TSharedRef<FDeathAshSeen> Ash = MakeShared<FDeathAshSeen>();

	AddStep(TEXT("look_Deaths"), DeathResetAt + 0.5f,
		[this, Ghost, Ash]
		{
			Ghost->bSeen = false;
			*Ash = FDeathAshSeen();
			SpawnTarget();
			PrepareBrawl(300.f, {});
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
			DeathStompFlat = DeathIcebergFlat = 1.f;
			DeathStompRise = DeathIcebergRise = -1.e9f;
			DeathStompDrift = DeathIcebergSide = 0.f;
			DeathIceEarly = DeathIceLate = DeathSoot = DeathSeaHidden = DeathActive = 0;
			DeathPhaseDone = -1;
			DeathSampleMask = 0;
			OverlayAtStart = Target.IsValid() && Target->GetMesh() ? Target->GetMesh()->GetOverlayMaterial() : nullptr;
		},
		[this, Base, Ghost, Ash](float T)
		{
			APenguinCharacter* Dummy = Target.Get();
			if (!Dummy)
			{
				return;
			}
			const ABlockIsland* Main = ABlockIsland::Get(GetWorld());
			const FBox MainBounds = Main ? Main->GetBounds() : FBox(FVector(-1000.f), FVector(1000.f));
			const float SeaZ = Main ? Main->SeaLevel : -1650.f;
			const FVector SeaSpot(MainBounds.Max.X + 300.f, 0.f, SeaZ + 140.f);
			const int32 Phase = FMath::Clamp(FMath::FloorToInt((T - DeathFirst) / DeathEvery), -1, NumDeaths);
			const float PhaseT = T - DeathFirst - Phase * DeathEvery;
			const float KOT = PhaseT - DeathKOAfter; // time since this phase's knock-out
			const bool bSea = Phase == NumDeaths - 1;

			// The close-up camera: from the stage camera's side, on the dummy (on the water at the sea). After a stomp it
			// stays where the dummy lay once it is gone (its ghost floats up there; practice brings the dummy back elsewhere).
			if (AActor* Camera = LookCamera.Get())
			{
				FVector LookAt = bSea && Phase < NumDeaths ? FVector(SeaSpot.X, 0.f, SeaZ + 80.f) : Dummy->GetActorLocation() + FVector(0.f, 0.f, 10.f);
				FVector LookFrom = DeathLookFrom;
				// A pancake (stomp, iceberg) lies low and wide: the camera looks at it on the floor from a bit further away,
				// at the same downward pitch (the whole squashed penguin, its flailing flippers and the stars in the picture).
				const bool bPancake = Phase >= 0 && Phase < NumDeaths
					&& (DeathCases[Phase].Reason == EPenguinKOReason::Stomp || DeathCases[Phase].Reason == EPenguinKOReason::Iceberg);
				if (bPancake && KOT > 0.f && Dummy->IsKnockedOut() && !Dummy->IsHidden())
				{
					LookAt = Dummy->GetFeetLocation() + FVector(0.f, 0.f, DeathPancakeLookUp);
					LookFrom = DeathPancakeFrom;
				}
				if (Phase >= 0 && Phase < NumDeaths && DeathCases[Phase].Reason == EPenguinKOReason::Stomp && KOT > 0.f && Phase == DeathPhaseDone)
				{
					if (Dummy->IsKnockedOut() && !Dummy->IsHidden())
					{
						Ghost->Spot = Dummy->GetActorLocation();
						Ghost->bSeen = true;
					}
					else if (Ghost->bSeen)
					{
						LookAt = Ghost->Spot + FVector(0.f, 0.f, DeathSpiritLookUp);
					}
				}
				// The icicle: a bit further away; once the block has burst, the camera stays where it burst.
				if (Phase >= 0 && Phase < NumDeaths && DeathCases[Phase].Reason == EPenguinKOReason::Icicle && KOT > 0.f && Phase == DeathPhaseDone)
				{
					LookFrom = DeathShatterFrom;
					if (Dummy->IsKnockedOut() && !Dummy->IsHidden() && (Dummy->IsDeathIceShown() || KOT < DeathShatterFollowUntil))
					{
						Ghost->Spot = Dummy->GetActorLocation();
						Ghost->bSeen = true;
					}
					else if (Ghost->bSeen)
					{
						LookAt = Ghost->Spot + FVector(0.f, 0.f, 10.f);
					}
				}
				// The snow bomb: on the dummy standing charred (all of it in the picture) until it has crumbled, then close on
				// the floor where it crumbled, and it stays there (the pile is the effects actor's, the dummy hides at about
				// 1.2 s and practice brings it back elsewhere).
				if (Phase >= 0 && Phase < NumDeaths && DeathCases[Phase].Reason == EPenguinKOReason::Bomb && KOT > 0.f && Phase == DeathPhaseDone)
				{
					if (Dummy->IsKnockedOut() && !Dummy->IsHidden() && KOT < DeathHoldTo)
					{
						Ghost->Spot = Dummy->GetFeetLocation();
						Ghost->bSeen = true;
					}
					const bool bPileView = KOT >= DeathBombPileFrom;
					LookFrom = bPileView ? DeathAshFrom : DeathCharFrom;
					LookAt = (Ghost->bSeen ? Ghost->Spot : Dummy->GetFeetLocation()) + FVector(0.f, 0.f, bPileView ? DeathAshLookUp : DeathCharLookUp);
				}
				const FVector From = LookAt + LookFrom;
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}

			// A new phase: a round reset, then the knock-out.
			if (Phase >= 0 && Phase > DeathPhaseDone)
			{
				DeathPhaseDone = Phase;
				DeathSampleMask = 0;
				Ghost->bSeen = false;
				ResetTarget(Phase == NumDeaths ? Base + FVector(300.f, 0.f, 0.f) : (bSea ? SeaSpot : Base + FVector(300.f, 0.f, 0.f)));
				return;
			}
			if (Phase < 0)
			{
				return;
			}
			if (Phase == NumDeaths)
			{
				// Back in after the round reset: sampled once it has stood a moment (the squash springs settle).
				if (PhaseT > 0.3f && !(DeathSampleMask & 1))
				{
					DeathSampleMask |= 1;
					const USkeletalMeshComponent* Mesh = Dummy->GetMesh();
					const FVector BaseScale = Dummy->GetBaseMeshScale();
					MarkA = Mesh && BaseScale.Z > 0.f ? static_cast<float>(Mesh->GetRelativeScale3D().Z / BaseScale.Z) : 0.f;
					MarkB = Mesh ? static_cast<float>(FVector::Dist(Mesh->GetRelativeLocation(), Dummy->GetBaseTranslationOffset())) : 999.f;
					Value = Mesh ? FMath::Max(FMath::Abs(static_cast<float>(Mesh->GetRelativeRotation().Pitch)), FMath::Abs(static_cast<float>(Mesh->GetRelativeRotation().Roll))) : 999.f;
					CountA = (Dummy->IsHidden() ? 1 : 0) | (Dummy->IsDeathLookActive() ? 2 : 0) | (Dummy->IsDeathIceShown() ? 4 : 0) | (Dummy->IsDeathSootShown() ? 8 : 0)
						| (Mesh && Mesh->GetOverlayMaterial() != OverlayAtStart.Get() ? 16 : 0) | (Dummy->IsKnockedOut() ? 32 : 0)
						| (Dummy->IsDeathAshPileShown() ? 64 : 0);
				}
				return;
			}

			const FDeathCase& Case = DeathCases[Phase];
			if (KOT >= 0.f && !(DeathSampleMask & 1))
			{
				DeathSampleMask |= 1;
				Dummy->KnockOut(Case.Reason, Penguin.Get(), Case.Launch); // (the sea may have taken it already: then nothing)
				DeathKOX = static_cast<float>(Dummy->GetActorLocation().X);
				return;
			}
			if (KOT < 0.f)
			{
				return;
			}
			const USkeletalMeshComponent* Mesh = Dummy->GetMesh();
			const float Flat = Mesh && Dummy->GetBaseMeshScale().Z > 0.f ? static_cast<float>(Mesh->GetRelativeScale3D().Z / Dummy->GetBaseMeshScale().Z) : 1.f;
			const FVector Velocity = Dummy->GetVelocity();
			const bool bShown = !Dummy->IsHidden() && Dummy->IsKnockedOut();
			// The look is on, for the knock-out's reason.
			if (KOT > 0.2f && !(DeathSampleMask & 2))
			{
				DeathSampleMask |= 2;
				DeathActive += Dummy->IsKnockedOut() && Dummy->IsDeathLookActive() && Dummy->GetKOReason() == Case.Reason ? 1 : 0;
				if (bSea)
				{
					DeathSeaHidden = Dummy->IsHidden() ? 1 : 0;
				}
			}
			switch (Case.Reason)
			{
			case EPenguinKOReason::Stomp:
				if (bShown && KOT < 0.8f)
				{
					DeathStompFlat = FMath::Min(DeathStompFlat, Flat);
					DeathStompRise = FMath::Max(DeathStompRise, static_cast<float>(Velocity.Z));
					DeathStompDrift = FMath::Max(DeathStompDrift, FMath::Abs(static_cast<float>(Dummy->GetActorLocation().X) - DeathKOX));
				}
				if (KOT > 0.25f && !(DeathSampleMask & 4))
				{
					DeathSampleMask |= 4;
					Shot(TEXT("53_death_stomp"));
				}
				if (KOT > DeathSpiritShotAt && !(DeathSampleMask & 8))
				{
					DeathSampleMask |= 8;
					Shot(TEXT("53_death_spirit")); // gone at 0.85 s: the little ghost floats up where it lay (the camera stays there)
				}
				break;
			case EPenguinKOReason::Iceberg:
				if (bShown && KOT < 0.8f)
				{
					DeathIcebergFlat = FMath::Min(DeathIcebergFlat, Flat);
					DeathIcebergRise = FMath::Max(DeathIcebergRise, static_cast<float>(Velocity.Z));
					DeathIcebergSide = FMath::Max(DeathIcebergSide, FMath::Abs(static_cast<float>(Velocity.X)));
				}
				if (KOT > 0.2f && !(DeathSampleMask & 4))
				{
					DeathSampleMask |= 4;
					Shot(TEXT("53_death_iceberg"));
				}
				break;
			case EPenguinKOReason::Icicle:
				if (KOT > 0.3f && !(DeathSampleMask & 4))
				{
					DeathSampleMask |= 4;
					DeathIceEarly = Dummy->IsDeathIceShown() ? 1 : 0;
					Shot(TEXT("53_death_icicle"));
				}
				if (KOT > DeathShatterShotAt && !(DeathSampleMask & 8))
				{
					DeathSampleMask |= 8;
					Shot(TEXT("53_death_icicle_shatter"));
				}
				if (KOT > 0.7f && !(DeathSampleMask & 16))
				{
					DeathSampleMask |= 16;
					DeathIceLate = Dummy->IsDeathIceShown() ? 0 : 1; // shattered by now
				}
				break;
			case EPenguinKOReason::Bomb:
			{
				int32 HeapParts = 0;
				int32 EyeWhites = 0;
				APinguFX::CountAshPileParts(Dummy, HeapParts, EyeWhites);
				const float Soot = Dummy->GetDeathSootCoverage();
				if (KOT > 0.3f && !(DeathSampleMask & 4))
				{
					DeathSampleMask |= 4;
					DeathSoot = Dummy->IsDeathSootShown() && Mesh && Mesh->GetOverlayMaterial() != nullptr && Mesh->GetOverlayMaterial() != OverlayAtStart.Get() ? 1 : 0;
				}
				if (KOT > DeathBombCharShotAt && !(DeathSampleMask & 64))
				{
					DeathSampleMask |= 64;
					Ash->CharMid = Soot;
					Shot(TEXT("53_death_bomb_1_char")); // the soot creeping in from the outline, a hot glow fading
				}
				if (KOT > DeathHoldFrom && KOT < DeathHoldTo && bShown)
				{
					// Standing there charred: solid black, still, its two white eyes on its own, no heap yet.
					Ash->HoldMin = FMath::Min(Ash->HoldMin, Soot);
					if (!Ash->bHoldSpotSeen)
					{
						Ash->HoldSpot = Dummy->GetActorLocation();
						Ash->bHoldSpotSeen = true;
					}
					Ash->HoldMoved = FMath::Max(Ash->HoldMoved, static_cast<float>(FVector::Dist(Dummy->GetActorLocation(), Ash->HoldSpot)));
					Ash->HoldEyesMin = FMath::Min(Ash->HoldEyesMin, EyeWhites);
					Ash->HoldHeapMax = FMath::Max(Ash->HoldHeapMax, HeapParts);
					float EyeRadius = 0.f;
					Ash->HoldEyeOff = FMath::Max(Ash->HoldEyeOff, BombEyesOff(Dummy, EyeRadius));
					Ash->HoldEyeRadius = EyeRadius;
				}
				if (KOT > DeathBombBlackShotAt && !(DeathSampleMask & 128))
				{
					DeathSampleMask |= 128;
					Shot(TEXT("53_death_bomb_2_black")); // all black but the eyes, standing stiff, smoking
				}
				if (Ash->CrumbleAt < 0.f && Dummy->GetDeathCrumbleTime() >= 0.f)
				{
					Ash->CrumbleAt = Dummy->GetDeathCrumbleTime();
				}
				if (KOT > DeathBombCollapseShotAt && !(DeathSampleMask & 256))
				{
					DeathSampleMask |= 256;
					Ash->CollapseFlat = bShown ? Flat : 0.f;
					Ash->CollapseHeap = HeapParts;
					Shot(TEXT("53_death_bomb_3_collapse")); // coming down into the heap, black dust, the eyes dropping
				}
				if (KOT > DeathAshShotAt && !(DeathSampleMask & 8))
				{
					DeathSampleMask |= 8;
					Ash->Pile = Dummy->IsDeathAshPileShown() ? 1 : 0;
					Ash->PileHeap = HeapParts;
					Ash->PileEyes = EyeWhites;
					FVector Left = FVector::ZeroVector;
					FVector Right = FVector::ZeroVector;
					float EyeRadius = 0.f;
					if (APinguFX::GetAshPileEyeSpots(Dummy, Left, Right, EyeRadius) && Ghost->bSeen)
					{
						Ash->PileEyeHeight = static_cast<float>((Left.Z + Right.Z) * 0.5 - Ghost->Spot.Z);
					}
					Shot(TEXT("53_death_bomb_4_pile")); // a heap of black ash, its two eyes on top looking out
				}
				if (KOT > DeathAshBlowShotAt && !(DeathSampleMask & 16))
				{
					DeathSampleMask |= 16;
					// The pile rests five seconds before the gust (look_AshRest has the gust and its picture): still all
					// there, nothing blown yet.
					Ash->Blown = Dummy->GetDeathAshBlown();
					Ash->Blowing = Dummy->IsDeathAshPileShown() ? 1 : 0;
				}
				if (KOT > DeathAshResetCheckAt && !(DeathSampleMask & 32))
				{
					DeathSampleMask |= 32;
					Ash->AfterReset = Dummy->IsKnockedOut() ? -1 : (Dummy->IsDeathAshPileShown() ? 1 : 0); // back in (practice): gone
				}
				break;
			}
			case EPenguinKOReason::Sea:
				if (KOT > 0.5f && !(DeathSampleMask & 4))
				{
					DeathSampleMask |= 4;
					Shot(TEXT("53_death_sea")); // bubbles, the sinking copy, the ghost coming out of the water
				}
				break;
			default:
				break;
			}
		},
		[this, Ash]
		{
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const bool bLooks = DeathStompFlat < 0.25f && DeathStompRise <= 1.f && DeathStompDrift < 10.f && DeathIcebergFlat < 0.25f && DeathIcebergSide > 600.f
				&& DeathIcebergRise <= 1.f && DeathIceEarly == 1 && DeathIceLate == 1 && DeathSoot == 1 && DeathSeaHidden == 1 && DeathActive == 5;
			Report(FString::Printf(TEXT("RESULT death looks: stomp flat %.2f (want under 0.25), rising %.0f cm/s (want 0) and moved %.0f cm (want under 10: on the spot); iceberg flat %.2f (want under 0.25), sideways %.0f cm/s (want about 900), rising %.0f cm/s (want 0: not launched up); icicle ice at 0.3 s %d, shattered at 0.7 s %d; bomb soot %d; sea gone at once %d; look on with its reason %d of 5: %s"),
				DeathStompFlat, DeathStompRise, DeathStompDrift, DeathIcebergFlat, DeathIcebergSide, DeathIcebergRise, DeathIceEarly, DeathIceLate, DeathSoot, DeathSeaHidden,
				DeathActive, bLooks ? TEXT("ok") : TEXT("FAIL")));
			// The snow bomb: charred (the soot half over the body mid char-up, solid black over all of it while it stands
			// there), standing still with its two white eyes on its own and no heap yet, crumbling 0.75 s after the knock-out
			// (coming down into a growing heap), then a heap of many parts with the two eyes on top.
			const FDeathAshSeen& A = *Ash;
			const bool bCharred = A.CharMid > 0.2f && A.CharMid < 0.9f && A.HoldMin > 0.999f && A.HoldMin <= 1.f && A.HoldMoved < 2.f
				&& A.HoldEyesMin == 2 && A.HoldEyeOff < 2.f && A.HoldHeapMax == 0 && FMath::Abs(A.CrumbleAt - DeathCrumbleWant) < 0.05f
				&& A.CollapseFlat < 0.9f && A.CollapseHeap > 0 && A.PileHeap >= 10 && A.PileEyes == 2 && A.PileEyeHeight > 25.f && A.PileEyeHeight < 60.f;
			Report(FString::Printf(TEXT("RESULT death bomb charred: soot over the body at %.2f s %.2f (want 0.2 to 0.9: charring), standing %.2f-%.2f s at least %.3f (want 1: solid black), moved %.1f cm (want under 2), white eyes %d (want 2) %.1f cm off its own (want under 2, radius %.1f cm), heap parts %d (want 0); crumbled %.3f s after the knock-out (want %.2f: 0.25 char-up + 0.5 standing), mid-crumble height %.2f (want under 0.9) with heap parts %d (want some); the pile at %.1f s: heap parts %d (want 10 or more), eyes %d (want 2) %.0f cm over the floor (want 25 to 60): %s"),
				DeathBombCharShotAt, A.CharMid, DeathHoldFrom, DeathHoldTo, A.HoldMin, A.HoldMoved, A.HoldEyesMin, A.HoldEyeOff, A.HoldEyeRadius, A.HoldHeapMax,
				A.CrumbleAt, DeathCrumbleWant, A.CollapseFlat, A.CollapseHeap, DeathAshShotAt, A.PileHeap, A.PileEyes, A.PileEyeHeight,
				bCharred ? TEXT("ok") : TEXT("FAIL")));
			// The snow bomb's ash pile: there with its eyes, still resting (nothing blown, still showing) a moment later, gone
			// once the dummy is back in (the practice respawn's reset ends it early).
			const bool bAsh = Ash->Pile == 1 && Ash->Blown < 0.01f && Ash->Blowing == 1 && Ash->AfterReset == 0;
			Report(FString::Printf(TEXT("RESULT death ash pile: shown with its eyes at %.2f s %d, blown away at %.2f s %.2f (want 0: it rests) and still showing %d, shown after the reset %d (want 0): %s"),
				DeathAshShotAt, Ash->Pile, DeathAshBlowShotAt, Ash->Blown, Ash->Blowing, Ash->AfterReset, bAsh ? TEXT("ok") : TEXT("FAIL")));
			const bool bReset = (DeathSampleMask & 1) && FMath::Abs(MarkA - 1.f) < 0.1f && MarkB < 2.f && Value < 0.5f && CountA == 0;
			Report(FString::Printf(TEXT("RESULT death reset: mesh height %.3f of normal (want 1), mesh offset %.1f cm (want 0), tilt %.1f deg (want 0), hidden %d, look on %d, ice %d, soot %d, overlay changed %d, knocked out %d, ash pile %d (want all 0): %s"),
				MarkA, MarkB, Value, (CountA & 1) ? 1 : 0, (CountA & 2) ? 1 : 0, (CountA & 4) ? 1 : 0, (CountA & 8) ? 1 : 0, (CountA & 16) ? 1 : 0, (CountA & 32) ? 1 : 0,
				(CountA & 64) ? 1 : 0, bReset ? TEXT("ok") : TEXT("FAIL")));
		});

	// ---- The snow bomb's ash pile rests (about five seconds) before the wind blows it away: a dummy that is not brought back
	// (the game mode, whose practice respawn would end the pile after 2.3 s, is held for the step). Blown up: the pile with
	// its eyes (crumbled from 0.75 s, settled by 1.1 s) still all there at 1.5 s and 6.0 s, being blown away at 6.95 s (the
	// picture), gone by 7.7 s. Then a second one: the floor under it broken away after 2 s - the gust comes at once (blown
	// by 2.7 s).
	static constexpr float AshThereEarlyAt = 1.5f;
	static constexpr float AshThereLateAt = 6.f;
	static constexpr float AshBlowingAt = 6.95f;
	static constexpr float AshGoneAt = 7.7f;
	struct FAshRest
	{
		int32 Seen = 0;          // bits: 1 there at 1.5 s, 2 there at 6.0 s, 4 blowing at 6.95 s, 8 gone at 7.7 s, 16 there at 1.9 s (second), 32 blown at 2.7 s
		float BlownAt6 = -1.f;
		float BlownSecond = -1.f;
		FVector Spot = FVector::ZeroVector;
		bool bSpotSeen = false;
		bool bFloorBroken = false;
		int32 Phase = 0;         // 0 before the first knock-out, 1 the first, 2 the second
		float KOAt = 0.f;
	};
	const TSharedRef<FAshRest> AshRest = MakeShared<FAshRest>();
	static constexpr float AshSecondAt = 8.f;
	AddStep(TEXT("look_AshRest"), AshSecondAt + 3.f,
		[this, AshRest]
		{
			*AshRest = FAshRest();
			SpawnTarget();
			PrepareBrawl(300.f, {});
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
			if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
			{
				Mode->SetActorTickEnabled(false); // no practice respawn for the step
			}
		},
		[this, AshRest, Base](float T)
		{
			using namespace PenguinAutoTestDeaths;
			APenguinCharacter* Dummy = Target.Get();
			if (!Dummy)
			{
				return;
			}
			FAshRest& A = *AshRest;
			const float KOT = T - A.KOAt;
			if (A.Phase > 0 && !A.bSpotSeen && KOT > 0.5f && Dummy->IsKnockedOut())
			{
				A.Spot = Dummy->GetFeetLocation();
				A.bSpotSeen = true;
			}
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = (A.bSpotSeen ? A.Spot : Dummy->GetFeetLocation()) + FVector(0.f, 0.f, DeathAshLookUp);
				const FVector From = LookAt + DeathAshFrom;
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			if (A.Phase == 0 && T > 0.1f)
			{
				A.Phase = 1;
				A.KOAt = T;
				Dummy->KnockOut(EPenguinKOReason::Bomb, Penguin.Get(), FVector2D(1200.f, 500.f));
				return;
			}
			if (A.Phase == 1)
			{
				const bool bPile = Dummy->IsDeathAshPileShown();
				const float Blown = Dummy->GetDeathAshBlown();
				if (!(A.Seen & 64) && KOT > AshThereEarlyAt)
				{
					A.Seen |= bPile && Blown < 0.01f ? 1 : 0;
					A.Seen |= 64; // (sampled)
				}
				if (!(A.Seen & 128) && KOT > AshThereLateAt)
				{
					A.Seen |= 128 | (bPile && Blown < 0.01f ? 2 : 0);
				}
				if (!(A.Seen & 256) && KOT > AshBlowingAt)
				{
					A.Seen |= 256 | (bPile ? 4 : 0);
					A.BlownAt6 = Blown;
					Shot(TEXT("53_death_bomb_blow")); // the gust blowing the pile away, ash streaming off downwind, the eyes left
				}
				if (!(A.Seen & 512) && KOT > AshGoneAt)
				{
					A.Seen |= 512 | (!bPile ? 8 : 0);
				}
				if (T > AshSecondAt - 0.2f && T < AshSecondAt - 0.1f)
				{
					ResetTarget(Base + FVector(300.f, 0.f, 0.f)); // back in (ends the look)
					A.bSpotSeen = false;
				}
				if (T > AshSecondAt)
				{
					A.Phase = 2;
					A.KOAt = T;
					Dummy->KnockOut(EPenguinKOReason::Bomb, Penguin.Get(), FVector2D(1200.f, 500.f));
				}
				return;
			}
			if (A.Phase == 2)
			{
				const bool bPile = Dummy->IsDeathAshPileShown();
				const float Blown = Dummy->GetDeathAshBlown();
				if (!(A.Seen & 1024) && KOT > 1.9f)
				{
					A.Seen |= 1024 | (bPile && Blown < 0.01f ? 16 : 0);
				}
				if (!A.bFloorBroken && KOT > 2.f && A.bSpotSeen)
				{
					A.bFloorBroken = true;
					int32 BrokeNow = 0;
					if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), A.Spot))
					{
						for (const float Dx : { -60.f, 0.f, 60.f })
						{
							BrokeNow += Flat->BreakBlockAt(A.Spot + FVector(Dx, 0.f, -50.f)) ? 1 : 0;
						}
					}
					// What the pile's floor check sees now (APenguinCharacter::HasAshFloor: ground within 12 cm under the pile).
					float GroundZ = -1.e6f;
					const ABlockIsland* Isle = ABlockIsland::FindNear(GetWorld(), FVector(A.Spot.X, 0.f, 0.f));
					const bool bGround = Isle && Isle->FindGroundBelow(FVector(A.Spot.X, 0.f, 12.f), 24.f, GroundZ);
					Report(FString::Printf(TEXT("ash pile floor: pile spot %s, blocks broken under it %d, the dummy at %s, pile shown %d; island %s, ground found %d at %.0f"),
						*A.Spot.ToString(), BrokeNow, *Dummy->GetActorLocation().ToString(), Dummy->IsDeathAshPileShown() ? 1 : 0, Isle ? *Isle->GetName() : TEXT("none"),
						bGround ? 1 : 0, GroundZ));
				}

				if (!(A.Seen & 2048) && KOT > 2.7f)
				{
					A.Seen |= 2048 | (Blown > 0.05f ? 32 : 0);
					A.BlownSecond = Blown;
				}
			}
		},
		[this, AshRest]
		{
			if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
			{
				Mode->SetActorTickEnabled(true);
			}
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const FAshRest& A = *AshRest;
			const bool bRest = (A.Seen & 1) && (A.Seen & 2) && (A.Seen & 4) && A.BlownAt6 > 0.1f && A.BlownAt6 < 1.f && (A.Seen & 8);
			Report(FString::Printf(TEXT("RESULT ash pile rests: all there at %.1f s %d and %.1f s %d, being blown away at %.2f s %d (blown %.2f, want 0.1 to 1), gone at %.1f s %d: %s"),
				AshThereEarlyAt, A.Seen & 1, AshThereLateAt, (A.Seen >> 1) & 1, AshBlowingAt, (A.Seen >> 2) & 1, A.BlownAt6, AshGoneAt, (A.Seen >> 3) & 1,
				bRest ? TEXT("ok") : TEXT("FAIL")));
			const bool bFloor = (A.Seen & 16) && A.bFloorBroken && (A.Seen & 32);
			Report(FString::Printf(TEXT("RESULT ash pile floor gone: resting at 1.9 s %d, the floor under it broken at 2 s %d, blown by 2.7 s %d (%.2f): %s"),
				(A.Seen >> 4) & 1, A.bFloorBroken ? 1 : 0, (A.Seen >> 5) & 1, A.BlownSecond, bFloor ? TEXT("ok") : TEXT("FAIL")));
			ResetTarget(Target.IsValid() ? Target->GetActorLocation() : FVector(FlatIslandX, 0.f, 62.f));
		});

	// ---- The snow bomb's charred look on the Low graphics preset: its penguin draws the merged low LOD (the eyes in one
	// section with the body) with the Low materials - the soot must still be solid black and the white eyes must still lie
	// over its own. The preset for the step (a test run saves nothing), the shaders it needs compiled first, then a snow
	// bomb knock-out of the dummy (not brought back: the game mode is held), shot standing black and as a pile; then the
	// preset as it was.
	struct FBombLow
	{
		bool bPresetSet = false;
		FPinguGraphicsOptions Before;
		float KOAt = -1.f;
		float HoldMin = 2.f;
		int32 HoldEyesMin = 99;
		float HoldEyeOff = 0.f;
		int32 Lod = -1;
		int32 PileHeap = 0;
		int32 PileEyes = 0;
		int32 Shots = 0;
	};
	const TSharedRef<FBombLow> BombLow = MakeShared<FBombLow>();
	static constexpr float BombLowWaitMax = 30.f;
	static constexpr float BombLowEndAfter = 2.f;
	AddStep(TEXT("look_DeathBombLow"), BombLowWaitMax + BombLowEndAfter + 1.f,
		[this, BombLow]
		{
			*BombLow = FBombLow();
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				BombLow->Before = Settings->GetGraphics();
				BombLow->bPresetSet = true;
				Settings->ApplyPreset(EPinguGraphicsPreset::Low);
			}
			SpawnTarget();
			PrepareBrawl(300.f, {});
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
			if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
			{
				Mode->SetActorTickEnabled(false); // no practice respawn for the step
			}
		},
		[this, BombLow](float T)
		{
			using namespace PenguinAutoTestDeaths;
			APenguinCharacter* Dummy = Target.Get();
			if (!Dummy)
			{
				return;
			}
			FBombLow& L = *BombLow;
			const float KOT = L.KOAt >= 0.f ? T - L.KOAt : -1.f;
			if (AActor* Camera = LookCamera.Get())
			{
				const bool bPileView = KOT >= DeathBombPileFrom;
				const FVector LookAt = Dummy->GetFeetLocation() + FVector(0.f, 0.f, bPileView ? DeathAshLookUp : DeathCharLookUp);
				const FVector From = LookAt + (bPileView ? DeathAshFrom : DeathCharFrom);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			if (L.KOAt < 0.f)
			{
				// The Low materials' shaders first (a moment at least; at most BombLowWaitMax).
				const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
				if (T > 0.6f && (!bCompiling || T > BombLowWaitMax))
				{
					L.KOAt = T;
					Dummy->KnockOut(EPenguinKOReason::Bomb, Penguin.Get(), FVector2D(1200.f, 500.f));
				}
				return;
			}
			int32 HeapParts = 0;
			int32 EyeWhites = 0;
			APinguFX::CountAshPileParts(Dummy, HeapParts, EyeWhites);
			if (KOT > DeathHoldFrom && KOT < DeathHoldTo && Dummy->IsKnockedOut() && !Dummy->IsHidden())
			{
				L.HoldMin = FMath::Min(L.HoldMin, Dummy->GetDeathSootCoverage());
				L.HoldEyesMin = FMath::Min(L.HoldEyesMin, EyeWhites);
				float EyeRadius = 0.f;
				L.HoldEyeOff = FMath::Max(L.HoldEyeOff, BombEyesOff(Dummy, EyeRadius));
				if (const USkeletalMeshComponent* DummyMesh = Dummy->GetMesh())
				{
					L.Lod = DummyMesh->GetPredictedLODLevel();
				}
			}
			if (KOT > DeathBombBlackShotAt && L.Shots == 0)
			{
				++L.Shots;
				Shot(TEXT("53_death_bomb_low_2_black"));
			}
			if (KOT > DeathAshShotAt && L.Shots == 1)
			{
				++L.Shots;
				L.PileHeap = HeapParts;
				L.PileEyes = EyeWhites;
				Shot(TEXT("53_death_bomb_low_4_pile"));
			}
			if (KOT > BombLowEndAfter)
			{
				StepTime = 1000.f; // done (the pile picture is written by now)
			}
		},
		[this, BombLow]
		{
			const FBombLow& L = *BombLow;
			if (UPinguSettings* Settings = UPinguSettings::Get(); Settings && L.bPresetSet)
			{
				// The preset as it was (a preset's own table and frame cap; custom options as they were).
				if (L.Before.Preset == EPinguGraphicsPreset::Custom)
				{
					Settings->SetGraphics(L.Before);
				}
				else
				{
					Settings->ApplyPreset(L.Before.Preset);
				}
			}
			if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
			{
				Mode->SetActorTickEnabled(true);
			}
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const bool bOk = L.KOAt >= 0.f && L.HoldMin > 0.999f && L.HoldMin <= 1.f && L.HoldEyesMin == 2 && L.HoldEyeOff < 2.f && L.PileHeap >= 10
				&& L.PileEyes == 2 && L.Shots == 2;
			Report(FString::Printf(TEXT("RESULT death bomb on Low: knocked out after %.1f s of shader wait, standing black at least %.3f (want 1), white eyes %d (want 2) %.1f cm off its own (want under 2), drawn at LOD %d, the pile's heap parts %d (want 10 or more) and eyes %d (want 2), shots %d: %s"),
				L.KOAt, L.HoldMin, L.HoldEyesMin, L.HoldEyeOff, L.Lod, L.PileHeap, L.PileEyes, L.Shots, bOk ? TEXT("ok") : TEXT("FAIL")));
			ResetTarget(Target.IsValid() ? Target->GetActorLocation() : FVector(FlatIslandX, 0.f, 62.f));
		});
}

void APenguinAutoTest::AddNootHornLookStep()
{
	// From the stage camera's side (+y: the penguin peeks at it): the horn at the first noot and at the second, put away
	// after; then a noot aimed up behind the penguin - it turns to it, the horn tips up. CountB collects what was right:
	// 1 out at the first shot, 2 out at the second, 4 put away after, 8 out at the third, 16 turned around for it.
	AddStep(TEXT("look_NootHorn"), 2.4f,
		[this]
		{
			PrepareBrawl(2400.f, { EPenguinAbility::Noot });
			MarkA = MarkB = Value = 0.f;
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this](float T)
		{
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Penguin->GetFeetLocation() + FVector(30.f, 0.f, 62.f);
				const FVector From = LookAt + FVector(25.f, 340.f, 35.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			const UStaticMeshComponent* Horn = Penguin->GetNootHorn();
			const bool bOut = Horn && Horn->ShouldRender() && Penguin->IsHonking();
			// How well the horn points along the aim on the play plane (1 = exactly).
			const auto Along = [Horn](const FVector2D& Aim)
			{
				if (!Horn)
				{
					return 0.f;
				}
				const FVector Forward = Horn->GetForwardVector();
				return static_cast<float>(FVector2D(Forward.X, Forward.Z).GetSafeNormal() | Aim.GetSafeNormal());
			};
			if (CountA == 0 && T > 0.3f)
			{
				CountA = 1;
				TimeA = T;
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(600.f, 0.f, 0.f));
				Penguin->ActivateAbility(0);
			}
			if (CountA == 1 && T > TimeA + 0.1f)
			{
				CountA = 2;
				Shot(TEXT("49_noot_horn_a"));
				CountB |= bOut ? 1 : 0;
				MarkA = Along(FVector2D(1.f, 0.f));
				Value = Horn && Penguin->GetMesh() ? static_cast<float>(Horn->GetComponentScale().Z / Penguin->GetMesh()->GetComponentScale().Z) : 0.f;
			}
			if (CountA == 2 && T > TimeA + 0.24f)
			{
				CountA = 3;
				Shot(TEXT("49_noot_horn_b"));
				CountB |= bOut ? 2 : 0;
			}
			if (CountA == 3 && T > TimeA + 0.8f)
			{
				CountA = 4;
				CountB |= !bOut ? 4 : 0;
			}
			if (CountA == 4 && T > 1.5f)
			{
				CountA = 5;
				TimeB = T;
				Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Noot }); // ready again
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(-400.f, 0.f, 400.f));
				Penguin->ActivateAbility(0);
			}
			if (CountA == 5 && T > TimeB + 0.14f)
			{
				CountA = 6;
				Shot(TEXT("49_noot_horn_c"));
				CountB |= bOut ? 8 : 0;
				CountB |= Penguin->GetVisualFacing() < 0.f ? 16 : 0;
				MarkB = Along(FVector2D(-1.f, 1.f));
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
			Report(FString::Printf(TEXT("RESULT noot horn look: shots %d, out at the first / second noot %d / %d, put away after %d, "
				"along the aim %.2f (want ~1), width at the first noot x%.2f; aimed up behind: out %d, turned %d, along %.2f"),
				CountA >= 6 ? 3 : CountA >= 3 ? 2 : CountA >= 2 ? 1 : 0, (CountB & 1) ? 1 : 0, (CountB & 2) ? 1 : 0, (CountB & 4) ? 1 : 0,
				MarkA, Value, (CountB & 8) ? 1 : 0, (CountB & 16) ? 1 : 0, MarkB));
		});
}

void APenguinAutoTest::AddShellBubbleLookStep()
{
	// The ice shell's bubble (M_ShieldBubble: shaded, a bright rim, a soft highlight up left, more solid than M_Bubble with
	// the penguin still seen inside) up close from the stage camera's side: one shot on High, then the Low preset (its
	// shaders compiled first, as in look_DeathBombLow) and one more; the preset as it was afterwards (a test run saves
	// nothing). Checked: the bubble showing at each shot and the material it draws with.
	struct FShellLook
	{
		bool bPresetSet = false;
		FPinguGraphicsOptions Before;
		int32 Stage = 0;
		float At = 0.f;
		int32 Shots = 0;
		int32 Shown[2] = { 0, 0 };
		FString Material[2];
	};
	const TSharedRef<FShellLook> Look = MakeShared<FShellLook>();
	static constexpr float ShellShotAfter = 0.25f;  // the shell lasts about a second
	static constexpr float ShellLowWaitMax = 30.f;
	AddStep(TEXT("look_ShellBubble"), 4.f + ShellLowWaitMax,
		[this, Look]
		{
			*Look = FShellLook();
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				Look->Before = Settings->GetGraphics();
				Look->bPresetSet = true;
				Settings->ApplyPreset(EPinguGraphicsPreset::High);
			}
			PrepareBrawl(2400.f, { EPenguinAbility::IceShell });
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, Look](float T)
		{
			FShellLook& L = *Look;
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Penguin->GetFeetLocation() + FVector(0.f, 0.f, 75.f); // the whole bubble in the picture
				const FVector From = LookAt + FVector(0.f, 640.f, 50.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			// The bubble's component and what it draws with.
			const auto Record = [this, &L](int32 Index)
			{
				TArray<UStaticMeshComponent*> Comps;
				Penguin->GetComponents<UStaticMeshComponent>(Comps);
				for (const UStaticMeshComponent* Comp : Comps)
				{
					if (Comp->GetName() == TEXT("ShieldBubble"))
					{
						L.Shown[Index] = Comp->ShouldRender() && Penguin->IsShielded() ? 1 : 0;
						const UMaterialInterface* Material = Comp->GetMaterial(0);
						L.Material[Index] = Material ? Material->GetName() : TEXT("none");
					}
				}
			};
			if (L.Stage == 0 && T > 0.3f)
			{
				L.Stage = 1;
				L.At = T;
				Penguin->ActivateAbility(0);
			}
			if (L.Stage == 1 && T > L.At + ShellShotAfter)
			{
				L.Stage = 2;
				Record(0);
				Shot(TEXT("59_shell_high"));
				++L.Shots;
			}
			if (L.Stage == 2 && T > L.At + 1.6f)
			{
				L.Stage = 3;
				L.At = T;
				if (UPinguSettings* Settings = UPinguSettings::Get())
				{
					Settings->ApplyPreset(EPinguGraphicsPreset::Low);
				}
			}
			if (L.Stage == 3)
			{
				const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
				if (T > L.At + 0.6f && (!bCompiling || T > L.At + ShellLowWaitMax))
				{
					L.Stage = 4;
					L.At = T;
					Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::IceShell }); // ready again
					Penguin->ActivateAbility(0);
				}
			}
			if (L.Stage == 4 && T > L.At + ShellShotAfter)
			{
				L.Stage = 5;
				Record(1);
				Shot(TEXT("59_shell_low"));
				++L.Shots;
			}
			if (L.Stage == 5 && T > L.At + ShellShotAfter + 0.3f)
			{
				StepTime = 1000.f; // done (the picture is written by now)
			}
		},
		[this, Look]
		{
			const FShellLook& L = *Look;
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
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const bool bOk = L.Shots == 2 && L.Shown[0] == 1 && L.Shown[1] == 1 && L.Material[0] == TEXT("M_ShieldBubble")
				&& L.Material[1] == TEXT("M_ShieldBubble");
			Report(FString::Printf(TEXT("RESULT shell bubble look: shots %d (want 2), bubble shown on High / Low %d / %d (want 1 / 1), material %s / %s (want M_ShieldBubble): %s"),
				L.Shots, L.Shown[0], L.Shown[1], *L.Material[0], *L.Material[1], bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}

void APenguinAutoTest::WatchIcicle(int32 Index, float StepTimeNow)
{
	UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
	if (IcicleCharge[Index] < 0.f && Abilities->GetIciclesThrown() > IciclesAtPress)
	{
		// Just thrown: our own flying icicle nearest to us. Its charge and the time from the press to the throw (server time).
		IcicleCharge[Index] = Abilities->GetLastIciclePower();
		double Pressed = 0.0;
		double Released = 0.0;
		double Thrown = 0.0;
		Abilities->GetLastIcicleTimes(Pressed, Released, Thrown);
		IcicleDelay[Index] = static_cast<float>(Thrown - Pressed);
		IcicleFromX = static_cast<float>(Penguin->GetActorLocation().X);
		IcicleFarX = IcicleFromX;
		float Nearest = 1.e9f;
		for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
		{
			if (It->GetKind() == EPenguinProjectileKind::Icicle && It->GetShooter() == Penguin.Get() && It->IsFlying())
			{
				const float Dist = static_cast<float>(FVector::Dist(It->GetActorLocation(), Penguin->GetActorLocation()));
				if (Dist < Nearest)
				{
					Nearest = Dist;
					WatchedIcicle = *It;
				}
			}
		}
		IcicleSpeedAtThrow[Index] = WatchedIcicle.IsValid() ? static_cast<float>(WatchedIcicle->GetVelocity().Size()) : -1.f;
		IcicleGravityAtThrow[Index] = WatchedIcicle.IsValid() ? WatchedIcicle->GetGravity() : -1.f;
		IcicleStartZ = WatchedIcicle.IsValid() ? static_cast<float>(WatchedIcicle->GetActorLocation().Z) : 0.f;
		IcicleDrop10m[Index] = -999.f;
		Report(FString::Printf(TEXT("icicle %d thrown at %.2f s: charge %.2f, held %.2f s, speed %.0f cm/s, gravity %.0f"), Index + 1, StepTimeNow,
			IcicleCharge[Index], Abilities->GetLastIcicleHold(), IcicleSpeedAtThrow[Index], IcicleGravityAtThrow[Index]));
	}
	// How far it got: while it flies, and where it stuck (it may fly out its time in the air instead).
	if (APenguinProjectile* Flying = WatchedIcicle.Get())
	{
		IcicleFarX = FMath::Max(IcicleFarX, static_cast<float>(Flying->GetActorLocation().X));
		if (IcicleDrop10m[Index] < -900.f && IcicleFarX - IcicleFromX >= 1000.f)
		{
			IcicleDrop10m[Index] = IcicleStartZ - static_cast<float>(Flying->GetActorLocation().Z); // how far it fell in its first 10 m
		}
		if (!Flying->IsFlying())
		{
			WatchedIcicle.Reset();
		}
	}
	if (IcicleCharge[Index] >= 0.f)
	{
		IcicleDistance[Index] = IcicleFarX - IcicleFromX;
	}
}

void APenguinAutoTest::AddIcicleThrowSteps()
{
	// ---- Close-ups from the stage camera's side (+y): the pull-out, half a second in, the cocked arm fully charged (shaking,
	// glowing), the release. Thrown with the ability key (the character's key handling). CountB: 1 in the flipper at the pull-out,
	// 2 half a second in, 8 fully charged, 4 gone right after the throw. And how the charge builds: the icicle in the flipper
	// shakes more, glows brighter, wears a brighter cold glow and sheds more sparkles at 0.2 / 0.5 / 1.0 s of the hold.
	struct FChargeLook
	{
		bool bTaken[3] = { false, false, false };
		float Amplitude[3] = { -1.f, -1.f, -1.f }; // the shake's envelope (cm) when the sample was taken
		float ShakeMax[3] = { 0.f, 0.f, 0.f };     // the biggest throw about the icicle got in the 0.1 s up to it (cm)
		float Glow[3] = { -1.f, -1.f, -1.f };
		float Halo[3] = { -1.f, -1.f, -1.f };
		float Sparkles[3] = { -1.f, -1.f, -1.f };
		int32 HaloShown = 0;
	};
	static constexpr float ChargeLookAt[3] = { 0.3f, 0.75f, 1.5f }; // (x1.5 with the longer charge)
	const TSharedRef<FChargeLook> ChargeLook = MakeShared<FChargeLook>();
	AddStep(TEXT("look_IcicleThrow"), 2.9f,
		[this, ChargeLook]
		{
			*ChargeLook = FChargeLook();
			PrepareBrawl(-1500.f, { EPenguinAbility::Icicle });
			IciclesAtPress = Penguin->GetAbilities()->GetIciclesThrown();
			MarkA = MarkB = -1.f;
			Value = 0.f;
			bFlag = false;
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, ChargeLook](float T)
		{
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Penguin->GetFeetLocation() + FVector(15.f, 0.f, 80.f);
				const FVector From = LookAt + FVector(10.f, 380.f, 25.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(700.f, 0.f, 0.f));
			if (CountA == 0 && T > 0.3f)
			{
				CountA = 1;
				TimeA = T;
				Penguin->SetAbilityKeyDown(0, true);
			}
			if (CountA == 1 && T >= TimeA + 0.065f)
			{
				CountA = 2;
				Shot(TEXT("55_icicle_throw_1")); // the pull-out: the icicle comes out from behind the back
				CountB |= Abilities->IsIcicleInHand() ? 1 : 0;
			}
			if (CountA == 2 && T >= TimeA + 0.75f)
			{
				CountA = 3;
				Shot(TEXT("55_icicle_throw_2")); // half a second: a faint glow, a little trembling
				CountB |= Abilities->IsIcicleInHand() ? 2 : 0;
			}
			if (CountA == 3 && T >= TimeA + 1.65f)
			{
				CountA = 4;
				Shot(TEXT("55_icicle_throw_3")); // fully charged: cocked over the head, drawn back, shaking, glowing, glinting
				CountB |= Abilities->IsIcicleInHand() ? 8 : 0;
			}
			if (CountA == 4 && T >= TimeA + 1.75f)
			{
				CountA = 5;
				TimeB = T;
				Penguin->SetAbilityKeyDown(0, false);
			}
			if (CountA == 5 && T >= TimeB + 0.01f)
			{
				CountA = 6;
				Shot(TEXT("55_icicle_throw_4")); // the release: swinging over the head to the front (the picture comes a frame later)
			}
			// How the charge builds: the biggest shake in the 0.1 s before each time, and what the icicle looks like at it.
			if (Abilities->GetIcicleShow() == EPinguIcicleShow::Charging && Abilities->IsIcicleInHand())
			{
				const float Held = Abilities->GetIcicleChargeTime();
				for (int32 Index = 0; Index < 3; ++Index)
				{
					if (!ChargeLook->bTaken[Index] && Held > ChargeLookAt[Index] - 0.1f)
					{
						ChargeLook->ShakeMax[Index] = FMath::Max(ChargeLook->ShakeMax[Index], Abilities->GetHandShakeOffset());
						if (Held >= ChargeLookAt[Index])
						{
							ChargeLook->bTaken[Index] = true;
							ChargeLook->Amplitude[Index] = Abilities->GetHandShakeAmplitude();
							ChargeLook->Glow[Index] = Abilities->GetHandGlow();
							ChargeLook->Halo[Index] = Abilities->GetHandHalo();
							ChargeLook->Sparkles[Index] = Abilities->GetHandSparkleRate();
							ChargeLook->HaloShown += Abilities->IsHandHaloShown() ? 1 : 0;
						}
					}
				}
			}
			if (CountA >= 5 && MarkA < 0.f && Abilities->GetIciclesThrown() > IciclesAtPress)
			{
				double Pressed = 0.0;
				double Released = 0.0;
				double Thrown = 0.0;
				Abilities->GetLastIcicleTimes(Pressed, Released, Thrown);
				MarkA = static_cast<float>(Thrown - Released);
				Value = Abilities->GetLastIciclePower();
				TimeA = T; // (the press is over: now the throw's time)
			}
			if (MarkA >= 0.f && !bFlag && T >= TimeA + 0.03f)
			{
				bFlag = true;
				CountB |= !Abilities->IsIcicleInHand() ? 4 : 0;
			}
			if (CountA >= 5 && MarkB < 0.f && Abilities->GetIcicleShow() == EPinguIcicleShow::None)
			{
				MarkB = T - TimeB;
			}
		},
		[this, ChargeLook]
		{
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			constexpr float WantRelease = UPenguinAbilityComponent::IcicleSwingTime;
			constexpr float WantShown = UPenguinAbilityComponent::IcicleSwingTime + UPenguinAbilityComponent::IcicleFollowTime;
			const bool bOk = CountA >= 6 && CountB == 15 && FMath::Abs(MarkA - WantRelease) < 0.025f && FMath::Abs(MarkB - WantShown) < 0.04f && Value > 0.99f;
			Report(FString::Printf(TEXT("RESULT icicle throw look: shots %d (want 4), in the flipper at the pull-out %d, charging %d, fully charged %d, gone after the throw %d; "
				"release to icicle %.3f s (want %.2f), the throw shown %.3f s after the release (want %.2f), charge %.2f (held 1.75 s: want 1) %s"),
				CountA >= 6 ? 4 : CountA >= 4 ? 3 : CountA >= 3 ? 2 : CountA >= 2 ? 1 : 0, (CountB & 1) ? 1 : 0, (CountB & 2) ? 1 : 0, (CountB & 8) ? 1 : 0, (CountB & 4) ? 1 : 0,
				MarkA, WantRelease, MarkB, WantShown, Value, bOk ? TEXT("ok") : TEXT("FAIL")));
			// The build-up: every number grows with the hold (0.3 / 0.75 / 1.5 s), and the full charge is clearly shaking, glowing and
			// sparkling.
			const FChargeLook& C = *ChargeLook;
			const bool bTaken = C.bTaken[0] && C.bTaken[1] && C.bTaken[2];
			const bool bShake = bTaken && C.ShakeMax[0] < C.ShakeMax[1] && C.ShakeMax[1] < C.ShakeMax[2] && C.Amplitude[0] < C.Amplitude[1] && C.Amplitude[1] < C.Amplitude[2]
				&& C.ShakeMax[0] < 1.f && C.ShakeMax[2] > 2.f;
			const bool bGlow = bTaken && C.Glow[0] < C.Glow[1] && C.Glow[1] < C.Glow[2] && C.Glow[2] > 3.f;
			const bool bHalo = bTaken && C.Halo[0] < C.Halo[1] && C.Halo[1] < C.Halo[2] && C.Halo[0] > 0.05f && C.HaloShown == 3;
			const bool bSparkles = bTaken && C.Sparkles[0] < C.Sparkles[1] && C.Sparkles[1] < C.Sparkles[2] && C.Sparkles[2] > 20.f;
			Report(FString::Printf(TEXT("RESULT icicle charge look: shake (biggest throw about, cm) %.2f / %.2f / %.2f (envelope %.2f / %.2f / %.2f), glow (emissive) %.2f / %.2f / %.2f, "
				"cold glow (opacity) %.2f / %.2f / %.2f (shown %d of 3), sparkles per second %.1f / %.1f / %.1f at 0.3 / 0.75 / 1.5 s: shake %s, glow %s, cold glow %s, sparkles %s"),
				C.ShakeMax[0], C.ShakeMax[1], C.ShakeMax[2], C.Amplitude[0], C.Amplitude[1], C.Amplitude[2], C.Glow[0], C.Glow[1], C.Glow[2],
				C.Halo[0], C.Halo[1], C.Halo[2], C.HaloShown, C.Sparkles[0], C.Sparkles[1], C.Sparkles[2],
				bShake ? TEXT("ok") : TEXT("FAIL"), bGlow ? TEXT("ok") : TEXT("FAIL"), bHalo ? TEXT("ok") : TEXT("FAIL"), bSparkles ? TEXT("ok") : TEXT("FAIL")));
		});

	// ---- How far it goes: a tap, half a second, a full second - thrown level along the strip with the ability key. Then the
	// cooldown (it starts with the throw, not the press) and a stun while charging (no icicle; the cooldown is used).
	AddStep(TEXT("look_IcicleCharge"), 10.0f,
		[this]
		{
			PrepareBrawl(-2400.f, { EPenguinAbility::Icicle });
			Teleport(FVector(FlatIslandX - 2400.f, 0.f, 62.f)); // the strip's left end: 54 m of it to the right
			for (int32 Index = 0; Index < 3; ++Index)
			{
				IcicleDistance[Index] = 0.f;
				IcicleCharge[Index] = -1.f;
				IcicleDelay[Index] = -1.f;
				IcicleSpeedAtThrow[Index] = -1.f;
				IcicleGravityAtThrow[Index] = -1.f;
				IcicleDrop10m[Index] = -999.f;
			}
			IcicleCooldownCharging = -1.f;
			IcicleCooldownThrown = -1.f;
			WatchedIcicle.Reset();
			MarkA = MarkB = -1.f;
			Value = -1.f;
		},
		[this](float T)
		{
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(1000.f, 0.f, 0.f)); // level, to the right
			const auto Press = [this, Abilities](float At)
			{
				Abilities->SetAbilities({ EPenguinAbility::Icicle }); // ready again
				IciclesAtPress = Abilities->GetIciclesThrown();
				TimeA = At;
				Penguin->SetAbilityKeyDown(0, true);
			};
			switch (CountA)
			{
			case 0: if (T >= 0.4f) { Press(T); CountA = 1; } break;                                        // a tap
			case 1: if (T >= TimeA + 0.02f) { Penguin->SetAbilityKeyDown(0, false); CountA = 2; } break;
			case 2: WatchIcicle(0, T); if (T >= 1.6f) { CountA = 3; } break;
			case 3: if (T >= 1.7f) { Press(T); CountA = 4; } break;                                        // half a second
			case 4:
				if (IcicleCooldownCharging < 0.f && T >= TimeA + 0.4f)
				{
					IcicleCooldownCharging = Abilities->GetCooldownRemaining(0);
				}
				if (T >= TimeA + 0.75f) // (half of the full charge's 1.5 s)
				{
					Penguin->SetAbilityKeyDown(0, false);
					CountA = 5;
				}
				break;
			case 5:
				WatchIcicle(1, T);
				if (IcicleCharge[1] >= 0.f && IcicleCooldownThrown < 0.f)
				{
					IcicleCooldownThrown = Abilities->GetCooldownRemaining(0);
				}
				if (T >= 3.9f)
				{
					CountA = 6;
				}
				break;
			case 6: if (T >= 4.0f) { Press(T); CountA = 7; } break;                                        // a full second
			case 7: if (T >= TimeA + 1.65f) { Penguin->SetAbilityKeyDown(0, false); CountA = 8; } break;
			case 8: WatchIcicle(2, T); if (T >= 7.6f) { CountA = 9; } break;
			case 9:                                                                                         // stunned while charging
				if (T >= 7.7f)
				{
					Press(T);
					CountB = Abilities->GetIciclesThrown();
					OwnHitsAtStart = Abilities->GetIciclesCancelled();
					CountA = 10;
				}
				break;
			case 10:
				if (T >= TimeA + 0.3f)
				{
					FPenguinHit Hit;
					Hit.Instigator = Target.Get();
					Hit.Source = EPenguinAbility::Snowball;
					Hit.Knockback = FVector2D(-300.f, 250.f);
					Hit.Stun = 0.4f;
					Hit.Location = Penguin->GetActorLocation();
					Penguin->ApplyPenguinHit(Hit);
					CountA = 11;
				}
				break;
			case 11:
				if (Value < 0.f && T >= TimeA + 0.4f)
				{
					Value = Abilities->IsIcicleInHand() ? 1.f : 0.f;
				}
				if (T >= TimeA + 0.6f)
				{
					Penguin->SetAbilityKeyDown(0, false); // let go after the hit: nothing comes out
					CountA = 12;
				}
				break;
			case 12:
				if (T >= TimeA + 1.2f)
				{
					TimeB = static_cast<float>(Abilities->GetIciclesThrown() - CountB);
					MarkA = static_cast<float>(Abilities->GetIciclesCancelled() - OwnHitsAtStart);
					MarkB = Abilities->GetCooldownRemaining(0);
					CountA = 13;
				}
				break;
			default:
				break;
			}
		},
		[this]
		{
			constexpr float Swing = UPenguinAbilityComponent::IciclePullOutTime + UPenguinAbilityComponent::IcicleSwingTime;
			// How far each one got: a tap is a weak lob that drops after about 2 m, half a second goes several metres, a full second
			// flies on (its 1.6 s: ~41 m at 2600 cm/s) - straight.
			const bool bCharges = IcicleCharge[0] >= 0.f && IcicleCharge[0] < 0.1f && FMath::Abs(IcicleCharge[1] - 0.5f) < 0.1f && IcicleCharge[2] > 0.99f;
			const bool bInBetween = IcicleDistance[0] < IcicleDistance[1] && IcicleDistance[1] < IcicleDistance[2];
			const bool bTapReach = IcicleDistance[0] > 150.f && IcicleDistance[0] < 280.f;
			const bool bHalfReach = IcicleDistance[1] > 600.f && IcicleDistance[1] < 1300.f;
			const bool bFullReach = IcicleDistance[2] > 3500.f;
			const bool bDistOk = bCharges && bInBetween && bTapReach && bHalfReach && bFullReach;
			Report(FString::Printf(TEXT("RESULT icicle charge distances: tap (charge %.2f) %.0f cm (want about %.0f: it drops after ~2 m), half a second (%.2f) %.0f cm (want 600 to 1300), "
				"a full second (%.2f) %.0f cm (want 3500 and more), each further than the one before %d %s"),
				IcicleCharge[0], IcicleDistance[0], PinguTuning::IcicleTapReach, IcicleCharge[1], IcicleDistance[1], IcicleCharge[2], IcicleDistance[2], bInBetween ? 1 : 0,
				bDistOk ? TEXT("ok") : TEXT("FAIL")));
			// The speeds as they left (the speed rises with the hold from 600 to 2600; the drop shrinks to nothing).
			const bool bTapSpeed = IcicleSpeedAtThrow[0] > 560.f && IcicleSpeedAtThrow[0] < 760.f && IcicleGravityAtThrow[0] > 2500.f;
			const bool bHalfSpeed = IcicleSpeedAtThrow[1] > 1500.f && IcicleSpeedAtThrow[1] < 1750.f && IcicleGravityAtThrow[1] > 600.f && IcicleGravityAtThrow[1] < 1200.f;
			const bool bFullSpeed = FMath::Abs(IcicleSpeedAtThrow[2] - PinguTuning::IcicleSpeed) < 15.f && IcicleGravityAtThrow[2] >= 0.f && IcicleGravityAtThrow[2] < 1.f;
			const bool bStraight = IcicleDrop10m[2] > -5.f && IcicleDrop10m[2] < 5.f && IcicleDrop10m[2] > -900.f;
			const bool bFaster = IcicleSpeedAtThrow[0] < IcicleSpeedAtThrow[1] && IcicleSpeedAtThrow[1] < IcicleSpeedAtThrow[2];
			Report(FString::Printf(TEXT("RESULT icicle charge speeds: tap %.0f cm/s with gravity %.0f (want about %.0f / %.0f), half a second %.0f cm/s with gravity %.0f (in between), "
				"a full second %.0f cm/s with gravity %.0f (want %.0f / 0), the full charge fell %.1f cm in its first 10 m (want under 5: perfectly straight), faster with the hold %d %s"),
				IcicleSpeedAtThrow[0], IcicleGravityAtThrow[0], PinguTuning::IcicleTapSpeed, PinguTuning::IcicleTapGravity, IcicleSpeedAtThrow[1], IcicleGravityAtThrow[1],
				IcicleSpeedAtThrow[2], IcicleGravityAtThrow[2], PinguTuning::IcicleSpeed, IcicleDrop10m[2], bFaster ? 1 : 0,
				bTapSpeed && bHalfSpeed && bFullSpeed && bStraight && bFaster ? TEXT("ok") : TEXT("FAIL")));
			const bool bDelayOk = FMath::Abs(IcicleDelay[0] - Swing) < 0.02f && FMath::Abs(IcicleDelay[1] - (0.75f + UPenguinAbilityComponent::IcicleSwingTime)) < 0.03f
				&& FMath::Abs(IcicleDelay[2] - (1.65f + UPenguinAbilityComponent::IcicleSwingTime)) < 0.03f;
			Report(FString::Printf(TEXT("RESULT icicle press to throw: tap %.3f s (want %.2f: the pull-out and the swing), half a second %.3f s, a full second %.3f s "
				"(want the hold + %.2f) %s"), IcicleDelay[0], Swing, IcicleDelay[1], IcicleDelay[2], UPenguinAbilityComponent::IcicleSwingTime,
				bDelayOk ? TEXT("ok") : TEXT("FAIL")));
			const float Cooldown = UPenguinAbilityComponent::GetInfo(EPenguinAbility::Icicle).Cooldown;
			const bool bCooldownOk = IcicleCooldownCharging == 0.f && IcicleCooldownThrown > Cooldown - 0.1f;
			Report(FString::Printf(TEXT("RESULT icicle cooldown: %.2f s left while charging (want 0), %.2f s right after the throw (want about %.1f: it starts with the throw) %s"),
				IcicleCooldownCharging, IcicleCooldownThrown, Cooldown, bCooldownOk ? TEXT("ok") : TEXT("FAIL")));
			const bool bStunOk = CountA >= 13 && TimeB == 0.f && MarkA == 1.f && Value == 0.f && MarkB > Cooldown - 1.5f;
			Report(FString::Printf(TEXT("RESULT icicle stunned while charging: icicles thrown %.0f (want 0), charges cancelled %.0f (want 1), in the flipper after the hit %.0f (want 0), "
				"cooldown used %.1f s %s"), TimeB, MarkA, Value, MarkB, bStunOk ? TEXT("ok") : TEXT("FAIL")));
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});

	// ---- The same full charge on the Low preset (its shaders compiled first, as in look_ShellBubble): the glow (the emissive and
	// the cold glow along it) and the shaking must still be there, with fewer sparkles; the preset as it was afterwards.
	struct FLowLook
	{
		bool bPresetSet = false;
		FPinguGraphicsOptions Before;
		int32 Stage = 0;
		float At = 0.f;
		int32 Shots = 0;
		bool bHaloShown = false;
		float Glow = 0.f;
		float Halo = 0.f;
		float ShakeMax = 0.f;
		float SparkleRate = 0.f;
		float Density = 1.f;
	};
	const TSharedRef<FLowLook> LowLook = MakeShared<FLowLook>();
	static constexpr float IcicleLowWaitMax = 30.f;
	AddStep(TEXT("look_IcicleChargeLow"), 5.f + IcicleLowWaitMax,
		[this, LowLook]
		{
			*LowLook = FLowLook();
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				LowLook->Before = Settings->GetGraphics();
				LowLook->bPresetSet = true;
			}
			PrepareBrawl(-1500.f, { EPenguinAbility::Icicle });
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, LowLook](float T)
		{
			FLowLook& L = *LowLook;
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Penguin->GetFeetLocation() + FVector(15.f, 0.f, 80.f);
				const FVector From = LookAt + FVector(10.f, 380.f, 25.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(700.f, 0.f, 0.f));
			if (L.Stage == 0 && T > 0.3f)
			{
				L.Stage = 1;
				L.At = T;
				if (UPinguSettings* Settings = UPinguSettings::Get())
				{
					Settings->ApplyPreset(EPinguGraphicsPreset::Low);
				}
			}
			if (L.Stage == 1)
			{
				const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
				if (T > L.At + 0.6f && (!bCompiling || T > L.At + IcicleLowWaitMax))
				{
					L.Stage = 2;
					L.At = T;
					Abilities->SetAbilities({ EPenguinAbility::Icicle }); // ready
					Penguin->SetAbilityKeyDown(0, true);
				}
			}
			if (L.Stage == 2 && Abilities->GetIcicleShow() == EPinguIcicleShow::Charging && Abilities->IsIcicleInHand())
			{
				const float Held = Abilities->GetIcicleChargeTime();
				if (Held > UPenguinAbilityComponent::IcicleFullChargeTime - 0.1f)
				{
					L.ShakeMax = FMath::Max(L.ShakeMax, Abilities->GetHandShakeOffset());
				}
				if (Held >= UPenguinAbilityComponent::IcicleFullChargeTime)
				{
					L.Stage = 3;
					L.At = T;
					L.Glow = Abilities->GetHandGlow();
					L.Halo = Abilities->GetHandHalo();
					L.bHaloShown = Abilities->IsHandHaloShown();
					L.SparkleRate = Abilities->GetHandSparkleRate();
					L.Density = UPinguSettings::GetFXDensity();
					Shot(TEXT("55_icicle_low_full"));
					++L.Shots;
				}
			}
			if (L.Stage == 3 && T > L.At + 0.25f)
			{
				L.Stage = 4;
				L.At = T;
				Penguin->SetAbilityKeyDown(0, false);
			}
			if (L.Stage == 4 && T > L.At + 0.5f)
			{
				StepTime = 1000.f; // done (the picture is written by now)
			}
		},
		[this, LowLook]
		{
			const FLowLook& L = *LowLook;
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
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const bool bOk = L.Shots == 1 && L.bHaloShown && L.Glow > 3.f && L.Halo > 0.4f && L.ShakeMax > 2.f && L.SparkleRate > 0.f && L.Density <= 0.31f;
			Report(FString::Printf(TEXT("RESULT icicle charge look on Low: shots %d (want 1), glow (emissive) %.2f (want over 3), cold glow shown %d with opacity %.2f (want 1 / over 0.4), "
				"shake %.2f cm (want over 2), sparkles asked for %.1f per second at the effects density %.2f (Low thins them out): %s"),
				L.Shots, L.Glow, L.bHaloShown ? 1 : 0, L.Halo, L.ShakeMax, L.SparkleRate, L.Density, bOk ? TEXT("ok") : TEXT("FAIL")));
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});
}

void APenguinAutoTest::AddIceFeetLookStep()
{
	// From the stage camera's side (+y: the penguins turn a little toward it).
	const FVector SideView(0.f, 230.f, 40.f);
	const FVector FrontView(170.f, 170.f, 45.f);
	AddStep(TEXT("look_IceFeet"), 2.6f,
		[this, SideView]
		{
			PrepareBrawl(2400.f, {});
			// The stage camera (every player looks through it) stops framing the island and goes where we put it.
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
				PlaceLookCamera(SideView);
			}
		},
		[this, SideView, FrontView](float T)
		{
			if (T < 0.9f)
			{
				PlaceLookCamera(SideView);
				if (T > 0.25f && MarkB < 0.f)
				{
					MarkB = T;
					Shot(TEXT("41_feet_plain")); // the feet as they are, for comparison
				}
				if (T > 0.35f && Value == 0.f)
				{
					Value = 1.f;
					Penguin->SetSlippery(8.f); // the ice gun's effect, ice chunks and all
				}
				if (T > 0.7f && !bFlag)
				{
					bFlag = true;
					Shot(TEXT("41_icefeet_side"));
					// Where the chunks are against the feet (the penguin faces +x): their middles should be ahead of the
					// ankles, around the feet, and reach the ground.
					const USkeletalMeshComponent* Body = Penguin->GetMesh();
					const FVector Feet = Penguin->GetFeetLocation();
					for (const TCHAR* Bone : { TEXT("Foot_L"), TEXT("Foot_R") })
					{
						for (const USceneComponent* Child : Body->GetAttachChildren())
						{
							if (Child && Child->GetAttachSocketName() == FName(Bone))
							{
								const FBox Box = Cast<UPrimitiveComponent>(Child) ? Cast<UPrimitiveComponent>(Child)->Bounds.GetBox() : FBox(ForceInit);
								Report(FString::Printf(TEXT("ice feet: %s chunk centre %s from the feet (ankle %s), bottom %.1f cm, top %.1f cm, visible %d"),
									Bone, *(Child->GetComponentLocation() - Feet).ToString(), *(Body->GetSocketLocation(FName(Bone)) - Feet).ToString(),
									Box.IsValid ? static_cast<float>(Box.Min.Z - Feet.Z) : 0.f, Box.IsValid ? static_cast<float>(Box.Max.Z - Feet.Z) : 0.f,
									Child->IsVisible() ? 1 : 0));
							}
						}
					}
				}
			}
			else if (T < 1.5f)
			{
				PlaceLookCamera(FrontView);
				if (T > 1.2f && !bKeyDone)
				{
					bKeyDone = true;
					Shot(TEXT("41_icefeet_front"));
				}
			}
			else
			{
				VirtualInput = FVector2D(1.f, 0.f);
				PlaceLookCamera(SideView);
				if (T > 2.2f && MarkA < 0.f)
				{
					MarkA = T;
					Shot(TEXT("41_icefeet_walk"));
				}
			}
		},
		[this]
		{
			VirtualInput = FVector2D::ZeroVector;
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
		});
}

void APenguinAutoTest::AddPegStep()
{
	AddStep(TEXT("move_IciclePad"), 2.4f,
		[this]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			Teleport(Base);
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Icicle });
			// A temporary ice wall to throw the icicle into.
			if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
			{
				const FIntPoint Foot = Flat->WorldToCell(Base + FVector(320.f, 0.f, 10.f));
				TArray<FIntPoint> Wall;
				for (int32 Row = 0; Row < 5; ++Row)
				{
					Wall.Add(FIntPoint(Foot.X, Foot.Y + Row));
				}
				Flat->AddBlocks(Wall, EBlockKind::Snow, 8.f); // snow: icicles shatter ice blocks instead of sticking in them
			}
			bFlag = false;
			bShotDone = false;
			MarkA = -1.f; // peg top Z
			Value = 0.f;
		},
		[this](float T)
		{
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			if (!bFlag)
			{
				Penguin->SetAimPoint(Base + FVector(320.f, 0.f, 170.f));
				if (T > 0.25f)
				{
					bFlag = true;
					Report(FString::Printf(TEXT("peg icicle thrown: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
				}
				return;
			}
			if (MarkA < 0.f && T > 0.8f)
			{
				// Where the wall face is (the icicle sticks into its left side).
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				const float FaceX = Flat ? static_cast<float>(Flat->CellToWorld(Flat->WorldToCell(Base + FVector(320.f, 0.f, 10.f))).X) - Flat->CellSize * 0.5f
					: static_cast<float>(Base.X) + 270.f;
				for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
				{
					if (It->HasPeg())
					{
						const FVector Top = It->GetPegTop();
						PegTopSeen = Top;
						// Only a peg sticking out of the wall counts.
						if (Top.X < FaceX - 80.f || Top.X > FaceX + 5.f || Top.Z < 100.f)
						{
							continue;
						}
						MarkA = static_cast<float>(Top.Z);
						// Drop the penguin onto the pad: it must throw it up (far higher than a jump).
						Teleport(FVector(Top.X, 0.f, Top.Z + Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 40.f));
						PadBouncesAtStart = Move->PadBounceEvents;
						PeakZ = static_cast<float>(Penguin->GetFeetLocation().Z);
						break;
					}
				}
				if (MarkA < 0.f)
				{
					MarkA = 0.f; // no peg: reported below
				}
			}
			if (MarkA > 0.f)
			{
				PeakZ = FMath::Max(PeakZ, static_cast<float>(Penguin->GetFeetLocation().Z));
			}
			if (MarkA > 0.f && T > 1.5f && !bShotDone)
			{
				Shot(TEXT("31_move_IciclePad")); // near the top of the bounce (2970 cm/s up: the top 0.66 s after it)
				bShotDone = true;
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT move IciclePad: pad at the wall %d (pad top seen at x %.0f z %.0f), bounces %d, feet rose %.0f cm above the pad top (want about 985: twice the old 490; a full jump: 290)"),
				MarkA > 0.f ? 1 : 0, PegTopSeen.X, PegTopSeen.Z, Move->PadBounceEvents - PadBouncesAtStart, MarkA > 0.f ? PeakZ - MarkA : 0.f));
		});

	// An icicle thrown into the ground sticks there (10 s) and is a jump pad you walk into: straight up, as high as the
	// wall pad throws.
	AddStep(TEXT("move_IcicleGroundPad"), 3.4f,
		[this]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			Teleport(Base);
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Icicle });
			VirtualInput = FVector2D::ZeroVector;
			bFlag = false;
			bShotDone = false;
			MarkA = -1.f; // feet Z when the pad was found (0: none)
			MarkB = -1.f; // step time of the throw
			Value = 0.f;  // the pad's time left when it was found
			PadBouncesAtStart = Move->PadBounceEvents;
			PeakZ = 0.f;
		},
		[this](float T)
		{
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			if (!bFlag)
			{
				// The penguin's icicle, straight down into the snow 3.5 m ahead (thrown at its own feet it would throw the
				// penguin up right away).
				if (T > 0.25f)
				{
					bFlag = true;
					const FVector Drop(Base.X + 350.f, 0.f, Base.Z + 60.f);
					Report(FString::Printf(TEXT("ground icicle thrown: %d"),
						APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Icicle, Penguin.Get(), Drop, FVector(0.f, 0.f, -1900.f)) ? 1 : 0));
				}
				return;
			}
			if (MarkA < 0.f && T > 0.8f)
			{
				MarkA = 0.f;
				for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
				{
					if (It->IsGroundPad())
					{
						MarkA = static_cast<float>(Penguin->GetFeetLocation().Z);
						Value = It->GetLifeLeft();
						PegTopSeen = It->GetActorLocation();
						PeakZ = MarkA;
						Shot(TEXT("52_icicle_ground_pad_stuck"));
						break;
					}
				}
			}
			// Walk into it; let go of the keys once it threw us.
			if (MarkA > 0.f && T > 1.0f)
			{
				const bool bBounced = Move->PadBounceEvents > PadBouncesAtStart;
				VirtualInput = bBounced ? FVector2D::ZeroVector : FVector2D(1.f, 0.f);
				if (bBounced && MarkB < 0.f)
				{
					MarkB = T;
				}
				PeakZ = FMath::Max(PeakZ, static_cast<float>(Penguin->GetFeetLocation().Z));
				if (MarkB > 0.f && T > MarkB + 0.6f && !bShotDone)
				{
					Shot(TEXT("52_icicle_ground_pad")); // near the top of the throw
					bShotDone = true;
				}
			}
		},
		[this]
		{
			VirtualInput = FVector2D::ZeroVector;
			const int32 Bounces = Move->PadBounceEvents - PadBouncesAtStart;
			const float Rose = MarkA > 0.f ? PeakZ - MarkA : 0.f;
			const bool bOk = MarkA > 0.f && Value > 9.f && Bounces >= 1 && Rose > 800.f;
			Report(FString::Printf(TEXT("RESULT move IcicleGroundPad: pad in the ground %d (at x %.0f z %.0f, %.1f s left, want about 10), bounces %d (want 1 or more: it can throw again on the way down), feet rose %.0f cm (want about 985, like the wall pad) %s"),
				MarkA > 0.f ? 1 : 0, PegTopSeen.X, PegTopSeen.Z, Value, Bounces, Rose, bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}

void APenguinAutoTest::AddCollapseStep()
{
	AddStep(TEXT("arena_Collapse"), 2.2f,
		[this]
		{
			ClearAbilityLeftovers();
			ResetTarget(FVector(FlatIslandX + 150.f, 0.f, 62.f));
			Teleport(FVector(FlatIslandX - 80.f, 0.f, 62.f));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			const FBox Bounds = Flat ? Flat->GetBounds() : FBox(ForceInit);
			MarkA = Flat ? static_cast<float>(Flat->GetNumBlocks()) : 0.f;
			StartX = Bounds.IsValid ? static_cast<float>(Bounds.Min.X) : 0.f;
			Value = Bounds.IsValid ? static_cast<float>(Bounds.Max.X) : 0.f;
			if (Flat)
			{
				Flat->StartCollapse(0.1f, 0.3f); // ten times faster than in a match
			}
		},
		[this](float T)
		{
			if (T > 1.2f && !bShotDone)
			{
				Shot(TEXT("34_arena_Collapse"));
				bShotDone = true;
			}
		},
		[this]
		{
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			const FBox Bounds = Flat ? Flat->GetBounds() : FBox(ForceInit);
			Report(FString::Printf(TEXT("RESULT arena Collapse: blocks %.0f -> %d, island x %.0f..%.0f -> %.0f..%.0f (middle %.0f), crumbling %d, penguin alive %d"),
				MarkA, Flat ? Flat->GetNumBlocks() : 0, StartX, Value, Bounds.IsValid ? Bounds.Min.X : 0.0, Bounds.IsValid ? Bounds.Max.X : 0.0,
				static_cast<double>(FlatIslandX), Flat && Flat->IsCollapsing() ? 1 : 0, Penguin->IsAlive() ? 1 : 0));
			if (Flat)
			{
				Flat->StopCollapse();
			}
		});
}

void APenguinAutoTest::AddClimbStep()
{
	AddStep(TEXT("move_LedgeClimb"), 2.6f,
		[this]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			Teleport(Base);
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			MarkB = -1.f; // wall top Z
			if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
			{
				// Six blocks high: a full jump + double jump tops out about one block short.
				const FIntPoint Foot = Flat->WorldToCell(Base + FVector(140.f, 0.f, 10.f));
				TArray<FIntPoint> Wall;
				for (int32 Row = 0; Row < 6; ++Row)
				{
					for (int32 Column = 0; Column < 3; ++Column) // wide enough to stand on top
					{
						Wall.Add(FIntPoint(Foot.X + Column, Foot.Y + Row));
					}
				}
				Flat->AddBlocks(Wall, EBlockKind::Ice, 8.f);
				MarkB = static_cast<float>(Flat->CellToWorld(FIntPoint(Foot.X, Foot.Y + 5)).Z) + Flat->CellSize * 0.5f;
			}
			MarkA = 0.f; // input script stage
			Value = 0.f; // highest feet Z before the climb
			PeakZ = -1.e9f; // highest feet Z after the grab
			StartZ = -1.f; // time of the grab
			ClimbsAtStart = Move->ClimbEvents;
			bShotDone = false;
		},
		[this](float T)
		{
			// Hold toward the wall; let go shortly after the grab so we stay on top.
			VirtualInput = StartZ >= 0.f && T > StartZ + 0.45f ? FVector2D::ZeroVector : FVector2D(1.f, 0.f);
			auto Stage = [this, T](float At, float Index, bool bPress)
			{
				if (T > At && MarkA < Index)
				{
					MarkA = Index;
					if (bPress)
					{
						Penguin->PressJump();
					}
					else
					{
						Penguin->ReleaseJump();
					}
				}
			};
			Stage(0.10f, 1.f, true);   // jump (held: full height)
			Stage(0.48f, 2.f, false);
			Stage(0.52f, 3.f, true);   // double jump
			Stage(0.86f, 4.f, false);
			Stage(0.90f, 5.f, true);   // out of jumps: grab the ledge
			Stage(0.98f, 6.f, false);
			if (Move->ClimbEvents == ClimbsAtStart)
			{
				Value = FMath::Max(Value, static_cast<float>(Penguin->GetFeetLocation().Z));
			}
			else
			{
				if (StartZ < 0.f)
				{
					StartZ = T;
				}
				PeakZ = FMath::Max(PeakZ, static_cast<float>(Penguin->GetFeetLocation().Z));
			}
			if (T > 1.3f && !bShotDone)
			{
				Shot(TEXT("33_move_LedgeClimb"));
				bShotDone = true;
			}
		},
		[this]
		{
			VirtualInput = FVector2D::ZeroVector;
			Report(FString::Printf(TEXT("RESULT move LedgeClimb: climbs %d, highest feet before the grab %.0f cm below the wall top, after it %.0f cm above, feet now %.0f cm above the wall top, grounded %d"),
				Move->ClimbEvents - ClimbsAtStart, MarkB - Value, PeakZ - MarkB, static_cast<float>(Penguin->GetFeetLocation().Z) - MarkB, Move->IsMovingOnGround() ? 1 : 0));
		});
}

void APenguinAutoTest::AddFloeStep()
{
	AddStep(TEXT("move_IcebergFloe"), 2.8f,
		[this]
		{
			ClearAbilityLeftovers();
			ResetTarget(FVector(FlatIslandX - 300.f + 1800.f, 0.f, 62.f));
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			const FBox Bounds = Flat ? Flat->GetBounds() : FBox(ForceInit);
			// Stand on the right end of the strip, aim out over the sea.
			const float EdgeX = Bounds.IsValid ? static_cast<float>(Bounds.Max.X) - 80.f : FlatIslandX + 2800.f;
			Teleport(FVector(EdgeX, 0.f, 62.f));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::IcebergDrop });
			bFlag = false;
			bShotDone = false;
			StartX = EdgeX;
		},
		[this](float T)
		{
			const FVector Aim(StartX + 360.f, 0.f, 62.f + 60.f);
			Penguin->SetAimPoint(Aim);
			if (T > 0.25f && !bFlag)
			{
				bFlag = true;
				Report(FString::Printf(TEXT("floe iceberg dropped: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
			if (T > 2.2f && !bShotDone)
			{
				Shot(TEXT("32_move_IcebergFloe"));
				bShotDone = true;
			}
		},
		[this]
		{
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			int32 FloeCells = 0;
			if (Flat)
			{
				for (float Dx = 150.f; Dx <= 650.f; Dx += 50.f)
				{
					for (float Z = -300.f; Z <= 400.f; Z += 50.f)
					{
						FloeCells += Flat->IsSolidAt(FVector(StartX + Dx, 0.f, Z)) ? 1 : 0;
					}
				}
			}
			Report(FString::Printf(TEXT("RESULT move IcebergFloe: floe samples %d (0 = no floe), caster alive %d"), FloeCells, Penguin->IsAlive() ? 1 : 0));
		});
}

void APenguinAutoTest::SpawnTarget()
{
	if (Target.IsValid() || !Penguin.IsValid())
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Target = GetWorld()->SpawnActor<APenguinCharacter>(Penguin->GetClass(), FVector(FlatIslandX + 400.f, 0.f, 62.f), FRotator::ZeroRotator, Params);
	if (Target.IsValid())
	{
		Target->SetPlayerSlot(1, TEXT("Dummy"));
		Target->SetInputLocked(false);
		Report(TEXT("target spawned"));
	}
}

void APenguinAutoTest::ResetTarget(const FVector& Where)
{
	if (Target.IsValid())
	{
		Target->ResetForRound(Where);
		Target->SetInputLocked(false);
	}
}

void APenguinAutoTest::ClearAbilityLeftovers()
{
	for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
	{
		It->Destroy();
	}
	for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
	{
		It->Destroy();
	}
	for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
	{
		It->Destroy();
	}
	APinguPortal::CloseAll(GetWorld());
	APinguTornado::DestroyAll(GetWorld());
	APinguGiantSnowball::DestroyAll(GetWorld());
	APinguSnowman::DestroyAll(GetWorld());
	if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f)))
	{
		// Fresh strip for every step: no ice walls, no frozen snow, no bomb craters from the previous one.
		Flat->Rebuild();
	}
}

void APenguinAutoTest::AddAbilityStep(EPenguinAbility Ability, float TargetOffset, float Duration)
{
	const FString Name = UPenguinAbilityComponent::GetInfo(Ability).Name;
	const FString StepName = FString::Printf(TEXT("ability_%s"), *Name.Replace(TEXT(" "), TEXT("")));
	AddStep(*StepName, Duration,
		[this, Ability, TargetOffset]
		{
			ClearAbilityLeftovers();
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			if (Ability == EPenguinAbility::BellyStomp)
			{
				Teleport(Base + FVector(0.f, 0.f, 420.f)); // right above the dummy
				ResetTarget(Base);
			}
			else
			{
				Teleport(Base);
				ResetTarget(Base + FVector(TargetOffset, 0.f, 0.f));
			}
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ Ability });
			StartX = Target.IsValid() ? Target->GetActorLocation().X : 0.f;
			Value = Penguin->GetActorLocation().X;
			TargetHitsAtStart = (Target.IsValid() && Target->GetPenguinMovement()) ? Target->GetPenguinMovement()->HitEvents : 0;
			CooldownAtHit = -1.f;
			CountB = 0; // the dummy was knocked out at some point of the step (it may be back by its end)
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			BrokenAtStart = Flat ? Flat->GetBrokenBlocks() : 0;
		},
		[this, Ability, Name](float T)
		{
			if (Target.IsValid() && Target->IsKnockedOut())
			{
				CountB = 1;
			}
			if (Target.IsValid())
			{
				const FVector Aim = Ability == EPenguinAbility::IceWall ? Penguin->GetActorLocation() + FVector(200.f, 0.f, 0.f)
					: Ability == EPenguinAbility::SnowWarp ? Penguin->GetActorLocation() + FVector(500.f, 0.f, 0.f)
					: Ability == EPenguinAbility::GrowRay ? Penguin->GetAimOrigin()
					: Target->GetAimOrigin();
				Penguin->SetAimPoint(Aim);
			}
			const UPenguinMovementComponent* HitMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
			if (bFlag && CooldownAtHit < 0.f && HitMove && HitMove->HitEvents != TargetHitsAtStart)
			{
				CooldownAtHit = Penguin->GetAbilities()->GetCooldownRemaining(0); // the snowball combo gives it right back
			}
			const float FireTime = Ability == EPenguinAbility::BellyStomp ? 0.15f : 0.25f;
			if (T > FireTime && !bFlag)
			{
				bFlag = true;
				const bool bUsed = Penguin->ActivateAbility(0);
				Report(FString::Printf(TEXT("%s used: %d"), *Name, bUsed ? 1 : 0));
			}
			if (T > FireTime + 0.25f && !bShotDone)
			{
				Shot(*FString::Printf(TEXT("20_ability_%s"), *Name.Replace(TEXT(" "), TEXT(""))));
				bShotDone = true;
				if (Ability == EPenguinAbility::IceShell || Ability == EPenguinAbility::FrostBreath || Ability == EPenguinAbility::IceGun)
				{
					DumpStatusVisuals(Penguin.Get(), *FString::Printf(TEXT("%s caster"), *Name));
					DumpStatusVisuals(Target.Get(), *FString::Printf(TEXT("%s target"), *Name));
				}
			}
		},
		[this, Name]
		{
			if (!Target.IsValid())
			{
				Report(FString::Printf(TEXT("RESULT ability %s: no target"), *Name));
				return;
			}
			const UPenguinMovementComponent* TargetMove = Target->GetPenguinMovement();
			const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
			Report(FString::Printf(TEXT("RESULT ability %s: blocks broken %d"), *Name, Flat ? Flat->GetBrokenBlocks() - BrokenAtStart : 0));
			if (CooldownAtHit >= 0.f)
			{
				Report(FString::Printf(TEXT("RESULT ability %s: cooldown left when it hit %.2f s (the snowball combo: 0)"), *Name, CooldownAtHit));
			}
			Report(FString::Printf(TEXT("RESULT ability %s: target KO %d, frozen %d, slippery %d, moved %.0f cm, hits %d, target size %.2f, caster moved %.0f cm, caster size %.2f, shield %d"),
				*Name, Target->IsKnockedOut() || CountB > 0 ? 1 : 0, TargetMove && TargetMove->IsFrozen() ? 1 : 0, TargetMove && TargetMove->IsSlippery() ? 1 : 0,
				Target->GetActorLocation().X - StartX, TargetMove ? TargetMove->HitEvents - TargetHitsAtStart : 0, Target->GetSizeScale(),
				Penguin->GetActorLocation().X - Value, Penguin->GetSizeScale(), Penguin->IsShielded() ? 1 : 0));
			// Size changes must not leak into the next steps.
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Target->ResetForRound(Target->GetActorLocation());
		});
}

FVector APenguinAutoTest::MeshForward(const APenguinCharacter* Who) const
{
	const USkeletalMeshComponent* SkelMesh = Who ? Who->GetMesh() : nullptr;
	return SkelMesh ? SkelMesh->GetComponentQuat().RotateVector(FVector(0.f, 1.f, 0.f)) : FVector::ZeroVector;
}

void APenguinAutoTest::AddFacingStep()
{
	// Play In Editor spawns the player turned like the viewport camera (yaw -90): the penguin must undo that,
	// then look right while running right and left while running left.
	AddStep(TEXT("facing_turned_spawn"), 1.8f,
		[this]
		{
			ClearAbilityLeftovers();
			Teleport(FVector(FlatIslandX - 1500.f, 0.f, 62.f));
			Penguin->SetActorRotation(FRotator(0.f, -90.f, 0.f));
			VirtualInput = FVector2D::ZeroVector;
			MarkA = -1000.f; // actor yaw seen a moment later
			MarkB = 0.f;     // mesh forward X while running right
			Value = 0.f;     // mesh forward X while running left
		},
		[this](float T)
		{
			if (T > 0.1f && MarkA < -999.f)
			{
				MarkA = static_cast<float>(Penguin->GetActorRotation().Yaw);
			}
			VirtualInput = T < 0.2f ? FVector2D::ZeroVector : (T < 1.0f ? FVector2D(1.f, 0.f) : FVector2D(-1.f, 0.f));
			if (T > 0.8f && T < 0.95f)
			{
				MarkB = static_cast<float>(MeshForward(Penguin.Get()).X);
			}
			if (T > 1.6f)
			{
				Value = static_cast<float>(MeshForward(Penguin.Get()).X);
			}
			if (T > 0.85f && !bShotDone)
			{
				Shot(TEXT("16_facing_after_turned_spawn"));
				bShotDone = true;
			}
		},
		[this]
		{
			VirtualInput = FVector2D::ZeroVector;
			Report(FString::Printf(TEXT("RESULT facing: actor yaw %.0f after a spawn turned -90, looks right running right %d (forward x %.2f), looks left running left %d (forward x %.2f)"),
				MarkA, MarkB > 0.5f ? 1 : 0, MarkB, Value < -0.5f ? 1 : 0, Value));
		});
}

void APenguinAutoTest::AddSizeKnockbackSteps()
{
	// The same snowball at a shrunken and at a grown dummy (the normal-size numbers are the "ability Snowball" line).
	for (const float Size : { 0.62f, 1.82f })
	{
		AddStep(*FString::Printf(TEXT("size_knockback_%.2f"), Size), 2.0f,
			[this, Size]
			{
				ClearAbilityLeftovers();
				const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
				Teleport(Base);
				ResetTarget(Base + FVector(400.f, 0.f, 0.f));
				Penguin->ResetForRound(Penguin->GetActorLocation());
				Penguin->SetInputLocked(false);
				Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Snowball });
				if (Target.IsValid())
				{
					Target->ChangeSize(Size, 30.f);
				}
				bFlag = false;
				StartX = 0.f;
				MarkA = 0.f; // target size when hit
				MarkB = 0.f; // capsule radius when hit
			},
			[this](float T)
			{
				if (!Target.IsValid())
				{
					return;
				}
				Penguin->SetAimPoint(Target->GetAimOrigin());
				if (T > 0.8f && !bFlag)
				{
					bFlag = true;
					StartX = static_cast<float>(Target->GetActorLocation().X);
					MarkA = Target->GetSizeScale();
					MarkB = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();
					Penguin->ActivateAbility(0);
				}
			},
			[this, Size]
			{
				const float Moved = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) - StartX : 0.f;
				Report(FString::Printf(TEXT("RESULT size knockback %s: target size %.2f (capsule radius %.0f) moved %.0f cm, KO %d"),
					Size < 1.f ? TEXT("small") : TEXT("big"), MarkA, MarkB, Moved, Target.IsValid() && Target->IsKnockedOut() ? 1 : 0));
				if (Target.IsValid())
				{
					Target->ResetForRound(Target->GetActorLocation());
				}
			});
	}

	// One size ray hit on Victim from our penguin (a step of PinguGrowRayFactor / PinguShrinkRayFactor, as a real ray's).
	const auto RayHit = [this](APenguinCharacter* Victim, bool bGrow) -> bool
	{
		if (!Victim)
		{
			return false;
		}
		FPenguinHit Hit;
		Hit.Instigator = Penguin.Get();
		Hit.Source = bGrow ? EPenguinAbility::GrowRay : EPenguinAbility::ShrinkRay;
		Hit.SizeFactor = bGrow ? PinguGrowRayFactor : PinguShrinkRayFactor;
		Hit.Location = Victim->GetActorLocation();
		return Victim->ApplyPenguinHit(Hit);
	};
	// The mesh's height now against its normal one (the death looks scale it).
	const auto MeshHeight = [](const APenguinCharacter* Who) -> float
	{
		const USkeletalMeshComponent* SkelMesh = Who ? Who->GetMesh() : nullptr;
		return SkelMesh && Who->GetBaseMeshScale().Z > 0.f ? static_cast<float>(SkelMesh->GetRelativeScale3D().Z / Who->GetBaseMeshScale().Z) : 1.f;
	};
	const auto BrokenOnStrip = [this]() -> int32
	{
		const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
		return Flat ? Flat->GetBrokenBlocks() - BrokenAtStart : 0;
	};

	// ---- Shrunk past the smallest size: two shrink hits take the dummy to 0.5x (still in); the third knocks it out (reason
	// Shrunk): it shrinks to nothing on the spot and is gone (hidden) at once.
	AddStep(TEXT("size_ShrunkKO"), 2.4f,
		[this]
		{
			PrepareBrawl(500.f, {});
			Value = 9.f; // the smallest mesh height while it shrinks away
		},
		[this, RayHit, MeshHeight](float T)
		{
			APenguinCharacter* Dummy = Target.Get();
			if (!Dummy)
			{
				return;
			}
			if (CountA == 0 && T > 0.2f)
			{
				CountA = 1;
				RayHit(Dummy, false);
			}
			if (CountA == 1 && T > 0.5f)
			{
				CountA = 2;
				RayHit(Dummy, false);
			}
			if (CountA == 2 && T > 1.2f)
			{
				CountA = 3;
				MarkA = Dummy->GetSizeScale();
				MarkB = Dummy->GetSizeTarget();
				CountB |= Dummy->IsAlive() ? 1 : 0;
			}
			if (CountA == 3 && T > 1.3f)
			{
				CountA = 4;
				RayHit(Dummy, false);
				TimeA = T;
				CountB |= Dummy->IsKnockedOut() && Dummy->GetKOReason() == EPenguinKOReason::Shrunk ? 2 : 0;
			}
			if (TimeA > 0.f)
			{
				if (Dummy->IsKnockedOut() && !Dummy->IsHidden())
				{
					Value = FMath::Min(Value, MeshHeight(Dummy));
				}
				if (!(CountB & 4) && T > TimeA + 0.6f)
				{
					CountB |= 4 | (Dummy->IsHidden() ? 8 : 0);
				}
			}
		},
		[this]
		{
			const bool bOk = FMath::Abs(MarkA - PinguMinSize) < 0.02f && (CountB & 1) && (CountB & 2) && Value < 0.35f && (CountB & 8);
			Report(FString::Printf(TEXT("RESULT size shrunk: two shrink hits: size %.2f, heading for %.2f (want 0.50), still in %d; the third: knocked out, reason Shrunk %d, "
				"shrank to %.2f of its height (want under 0.35), hidden 0.6 s after %d: %s"),
				MarkA, MarkB, CountB & 1, (CountB >> 1) & 1, Value < 9.f ? Value : -1.f, (CountB >> 3) & 1, bOk ? TEXT("ok") : TEXT("FAIL")));
			if (Target.IsValid())
			{
				Target->ResetForRound(Target->GetActorLocation());
			}
		});

	// ---- Grown past the biggest size: eight grow hits take the dummy to 8x (still in); the ninth knocks it out (reason
	// Overgrown): it puffs up and pops - the blast pushes our penguin 7 m away (not out) and breaks no block. The picture
	// from far in front (the whole giant).
	AddStep(TEXT("size_OvergrownKO"), 3.6f,
		[this]
		{
			PrepareBrawl(700.f, {});
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
			Value = 0.f; // the tallest mesh height while it puffs up
			HighZ = 0.f; // our penguin's fastest sideways speed after the pop
		},
		[this, RayHit, MeshHeight](float T)
		{
			APenguinCharacter* Dummy = Target.Get();
			if (!Dummy)
			{
				return;
			}
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Dummy->GetFeetLocation() + FVector(-250.f, 0.f, 450.f);
				const FVector From = LookAt + FVector(0.f, 3400.f, 300.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			if (CountA < 8 && T > 0.2f + 0.12f * CountA)
			{
				RayHit(Dummy, true);
				++CountA;
			}
			if (CountA == 8 && T > 1.9f)
			{
				CountA = 9;
				MarkA = Dummy->GetSizeScale();
				MarkB = Dummy->GetSizeTarget();
				CountB |= Dummy->IsAlive() ? 1 : 0;
				StartX = static_cast<float>(Location().X);
			}
			if (CountA == 9 && T > 2.f)
			{
				CountA = 10;
				RayHit(Dummy, true);
				TimeA = T;
				CountB |= Dummy->IsKnockedOut() && Dummy->GetKOReason() == EPenguinKOReason::Overgrown ? 2 : 0;
			}
			if (TimeA > 0.f)
			{
				const float KOT = T - TimeA;
				if (Dummy->IsKnockedOut() && !Dummy->IsHidden())
				{
					Value = FMath::Max(Value, MeshHeight(Dummy));
				}
				if (!bShotDone && KOT > 0.42f)
				{
					bShotDone = true;
					Shot(TEXT("54_size_pop")); // puffed up, about to pop
				}
				if (!(CountB & 4) && KOT > 0.8f)
				{
					CountB |= 4 | (Dummy->IsHidden() ? 8 : 0);
				}
				if (KOT > 0.5f)
				{
					HighZ = FMath::Max(HighZ, FMath::Abs(static_cast<float>(Move->Velocity.X)));
				}
			}
		},
		[this, BrokenOnStrip]
		{
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const float Pushed = StartX - static_cast<float>(Location().X);
			const int32 BrokenNow = BrokenOnStrip();
			const bool bOk = FMath::Abs(MarkA - PinguMaxSize) < 0.05f && (CountB & 1) && (CountB & 2) && Value > 1.15f && (CountB & 8)
				&& HighZ > 400.f && Penguin->IsAlive() && BrokenNow == 0;
			Report(FString::Printf(TEXT("RESULT size overgrown: eight grow hits: size %.2f, heading for %.2f (want 8.00), still in %d; the ninth: knocked out, reason Overgrown %d, "
				"puffed up to %.2f of its height (want over 1.15), popped (hidden 0.8 s after) %d; our penguin 7 m away pushed at %.0f cm/s (want over 400) and %.0f cm, still in %d, "
				"blocks broken %d (want 0): %s"),
				MarkA, MarkB, CountB & 1, (CountB >> 1) & 1, Value, (CountB >> 3) & 1, HighZ, Pushed, Penguin->IsAlive() ? 1 : 0, BrokenNow, bOk ? TEXT("ok") : TEXT("FAIL")));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			if (Target.IsValid())
			{
				Target->ResetForRound(Target->GetActorLocation());
			}
		});

	// ---- Knockback by relative size: the same push from our penguin at a 0.5x and a 2x dummy, and from our penguin at 2x at
	// a normal one, against the push between two normal penguins: (attacker / victim)^1.25 = 2.38, 0.42, 2.38.
	struct FRelCase
	{
		float OurSize;
		float DummySize;
		float Want;
	};
	static const FRelCase RelCases[] = { { 1.f, 1.f, 1.f }, { 1.f, 0.5f, 2.378f }, { 1.f, 2.f, 0.420f }, { 2.f, 1.f, 2.378f } };
	static constexpr int32 NumRelCases = UE_ARRAY_COUNT(RelCases);
	const TSharedRef<TArray<float>> RelSpeeds = MakeShared<TArray<float>>();
	AddStep(TEXT("size_RelativeKnockback"), NumRelCases * 1.f + 0.2f,
		[this, RelSpeeds]
		{
			PrepareBrawl(450.f, {});
			RelSpeeds->Init(0.f, NumRelCases);
		},
		[this, RelSpeeds](float T)
		{
			APenguinCharacter* Dummy = Target.Get();
			UPenguinMovementComponent* DummyMove = Dummy ? Dummy->GetPenguinMovement() : nullptr;
			if (!DummyMove)
			{
				return;
			}
			// A case a second: both put back at their places and sized, then (grown / shrunk by then) the push.
			const int32 Case = FMath::Min(static_cast<int32>(T / 1.f), NumRelCases - 1);
			const FRelCase& C = RelCases[Case];
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			if (CountA == Case * 2)
			{
				CountA = Case * 2 + 1;
				Teleport(Base);
				Penguin->ResetForRound(Penguin->GetActorLocation());
				ResetTarget(Base + FVector(450.f, 0.f, 0.f));
				if (C.OurSize != 1.f)
				{
					Penguin->ChangeSize(C.OurSize, 30.f);
				}
				if (C.DummySize != 1.f)
				{
					Dummy->ChangeSize(C.DummySize, 30.f);
				}
			}
			if (CountA == Case * 2 + 1 && T > Case * 1.f + 0.8f)
			{
				CountA = Case * 2 + 2;
				FPenguinHit Hit;
				Hit.Instigator = Penguin.Get();
				Hit.Source = EPenguinAbility::Snowball;
				Hit.SourceSize = 0.f; // our size counts (a real snowball sets 1: its own size carries ours)
				Hit.Knockback = FVector2D(600.f, 300.f);
				Hit.Stun = 0.2f;
				Hit.Location = Dummy->GetActorLocation();
				Dummy->ApplyPenguinHit(Hit);
				(*RelSpeeds)[Case] = static_cast<float>(DummyMove->Velocity.X);
				Report(FString::Printf(TEXT("relative knockback case %d: sizes %.2f on %.2f, the dummy sent off at %.0f cm/s"), Case, Penguin->GetSizeScale(),
					Dummy->GetSizeScale(), (*RelSpeeds)[Case]));
			}
		},
		[this, RelSpeeds]
		{
			const TArray<float>& Speeds = *RelSpeeds;
			const float Normal = Speeds.Num() > 0 ? Speeds[0] : 0.f;
			bool bOk = Normal > 100.f;
			FString Ratios;
			for (int32 Case = 1; Case < Speeds.Num(); ++Case)
			{
				const float Ratio = Normal > 1.f ? Speeds[Case] / Normal : 0.f;
				bOk &= FMath::Abs(Ratio - RelCases[Case].Want) < 0.06f * RelCases[Case].Want + 0.02f;
				Ratios += FString::Printf(TEXT("%s%.1fx on %.1fx: %.2f (want %.2f)"), Case > 1 ? TEXT(", ") : TEXT(""), RelCases[Case].OurSize, RelCases[Case].DummySize,
					Ratio, RelCases[Case].Want);
			}
			Report(FString::Printf(TEXT("RESULT size relative knockback: normal on normal %.0f cm/s; %s: %s"), Normal, *Ratios, bOk ? TEXT("ok") : TEXT("FAIL")));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			if (Target.IsValid())
			{
				Target->ResetForRound(Target->GetActorLocation());
			}
		});
}

void APenguinAutoTest::AddPickupSteps()
{
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	// A bubble floats down onto us: its ability goes into the middle slot, the other two stay.
	AddStep(TEXT("pickup_take"), 2.2f,
		[this, Base]
		{
			ClearAbilityLeftovers();
			APinguPickup::DestroyAll(GetWorld());
			Teleport(Base);
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Snowball, EPenguinAbility::FlipperSlap, EPenguinAbility::Icicle });
			TestPickup = APinguPickup::Spawn(GetWorld(), EPenguinAbility::Noot, Penguin->GetActorLocation() + FVector(0.f, 0.f, 260.f));
			MarkA = -1.f; // time taken
		},
		[this](float T)
		{
			if (MarkA < 0.f && !TestPickup.IsValid())
			{
				MarkA = T;
			}
			if (T > 0.3f && !bShotDone)
			{
				Shot(TEXT("40_pickup_falling"));
				bShotDone = true;
			}
		},
		[this]
		{
			const UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			const bool bMiddle = Abilities->GetAbility(UPenguinAbilityComponent::PickupSlot) == EPenguinAbility::Noot;
			const bool bKept = Abilities->GetAbility(0) == EPenguinAbility::Snowball && Abilities->GetAbility(2) == EPenguinAbility::Icicle;
			Report(FString::Printf(TEXT("RESULT pickup take: middle slot is now Noot %d, the other two kept %d, swap count %d, taken after %.2f s, bubbles left %d"),
				bMiddle ? 1 : 0, bKept ? 1 : 0, Abilities->GetSwapCount(UPenguinAbilityComponent::PickupSlot), MarkA, APinguPickup::CountAll(GetWorld())));
		});

	// An ability we already have: it is recharged, nothing is swapped.
	AddStep(TEXT("pickup_recharge"), 1.4f,
		[this, Base]
		{
			ClearAbilityLeftovers();
			APinguPickup::DestroyAll(GetWorld());
			Teleport(Base);
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Snowball, EPenguinAbility::FlipperSlap, EPenguinAbility::Icicle });
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(-400.f, 0.f, 0.f));
			Penguin->ActivateAbility(2); // the icicle: on cooldown (3 s) once it is thrown, right after the pull-out
			MarkA = -1.f;
			TestPickup.Reset();
		},
		[this](float T)
		{
			// Thrown: its cooldown runs - now the bubble (taken as it lands on us).
			const float Left = Penguin->GetAbilities()->GetCooldownRemaining(2);
			if (MarkA < 0.f && Left > 0.f)
			{
				MarkA = Left;
				TestPickup = APinguPickup::Spawn(GetWorld(), EPenguinAbility::Icicle, Penguin->GetActorLocation() + FVector(0.f, 0.f, 150.f));
			}
		},
		[this]
		{
			const UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			Report(FString::Printf(TEXT("RESULT pickup recharge: icicle cooldown %.1f s -> %.1f s, middle slot unchanged %d, bubble taken %d"),
				MarkA, Abilities->GetCooldownRemaining(2), Abilities->GetAbility(1) == EPenguinAbility::FlipperSlap ? 1 : 0, MarkA >= 0.f && !TestPickup.IsValid() ? 1 : 0));
		});

	// A bubble lying on the strip falls into the crater when the ground under it is blown away.
	AddStep(TEXT("pickup_ground_breaks"), 3.0f,
		[this, Base]
		{
			ClearAbilityLeftovers();
			APinguPickup::DestroyAll(GetWorld());
			Teleport(Base + FVector(-600.f, 0.f, 0.f));
			PickupStart = Base + FVector(400.f, 0.f, 200.f);
			TestPickup = APinguPickup::Spawn(GetWorld(), EPenguinAbility::Gust, PickupStart);
			bFlag = false;   // rested
			MarkA = -1.f;    // Z when resting
			MarkB = -1.f;    // time the ground broke
			PickupLowZ = 1.e9f;
		},
		[this](float T)
		{
			if (!TestPickup.IsValid())
			{
				return;
			}
			if (!bFlag && TestPickup->IsResting())
			{
				bFlag = true;
				MarkA = static_cast<float>(TestPickup->GetActorLocation().Z);
			}
			if (bFlag && MarkB < 0.f && T > 1.4f)
			{
				MarkB = T;
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), PickupStart))
				{
					Flat->RemoveBlocksInRadius(FVector(PickupStart.X, 0.f, 0.f), 160.f);
				}
			}
			if (MarkB > 0.f)
			{
				PickupLowZ = FMath::Min(PickupLowZ, static_cast<float>(TestPickup->GetActorLocation().Z));
			}
		},
		[this]
		{
			const float Fell = MarkA > -1.f && PickupLowZ < 1.e8f ? MarkA - PickupLowZ : 0.f;
			Report(FString::Printf(TEXT("RESULT pickup ground: rested %d (z %.0f), fell %.0f cm after the ground broke, still there %d"),
				bFlag ? 1 : 0, MarkA, Fell, TestPickup.IsValid() ? 1 : 0));
		});

	// A snowball flying through a bubble knocks it along (and keeps flying).
	AddStep(TEXT("pickup_pushed"), 1.6f,
		[this, Base]
		{
			ClearAbilityLeftovers();
			APinguPickup::DestroyAll(GetWorld());
			Teleport(Base);
			ResetTarget(Base + FVector(1800.f, 0.f, 0.f));
			Penguin->ResetForRound(Penguin->GetActorLocation());
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Snowball });
			PickupStart = Base + FVector(450.f, 0.f, 40.f);
			TestPickup = APinguPickup::Spawn(GetWorld(), EPenguinAbility::ShrinkRay, PickupStart);
			bFlag = false;
			StartX = 0.f;
		},
		[this](float T)
		{
			if (!TestPickup.IsValid())
			{
				return;
			}
			Penguin->SetAimPoint(TestPickup->GetIconLocation());
			if (T > 0.7f && !bFlag)
			{
				bFlag = true;
				StartX = static_cast<float>(TestPickup->GetActorLocation().X);
				Penguin->ActivateAbility(0);
			}
		},
		[this]
		{
			const float Moved = TestPickup.IsValid() ? static_cast<float>(TestPickup->GetActorLocation().X) - StartX : 0.f;
			Report(FString::Printf(TEXT("RESULT pickup pushed: a snowball knocked the bubble %.0f cm along, still there %d"), Moved, TestPickup.IsValid() ? 1 : 0));
		});

	// A knocked-out penguin's drop pops up where it was; one that fell into the sea comes down over the island.
	AddStep(TEXT("pickup_drops"), 4.0f,
		[this, Base]
		{
			ClearAbilityLeftovers();
			APinguPickup::DestroyAll(GetWorld());
			Teleport(Base + FVector(-900.f, 0.f, 0.f));
			PickupStart = Base + FVector(300.f, 0.f, 0.f);
			TestPickup = APinguPickup::SpawnDrop(GetWorld(), EPenguinAbility::SnowBomb, PickupStart, false);
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			const FBox Bounds = Flat ? Flat->GetBounds() : FBox(ForceInit);
			// "Fell into the sea" 6 m past the right end of the strip.
			StartX = Bounds.IsValid ? static_cast<float>(Bounds.Max.X) + 600.f : FlatIslandX + 3600.f;
			Value = Bounds.IsValid ? static_cast<float>(Bounds.Max.X) : FlatIslandX + 3000.f;
			TestBombA.Reset();
			TestBombB.Reset();
			SeaDrop = APinguPickup::SpawnDrop(GetWorld(), EPenguinAbility::Icicle, FVector(StartX, 0.f, -1600.f), true);
			PickupHighZ = -1.e9f;
			bFlag = false;
		},
		[this](float T)
		{
			if (TestPickup.IsValid())
			{
				PickupHighZ = FMath::Max(PickupHighZ, static_cast<float>(TestPickup->GetActorLocation().Z));
			}
			if (T > 1.2f && !bShotDone)
			{
				Shot(TEXT("41_pickup_drops"));
				bShotDone = true;
			}
		},
		[this]
		{
			const APinguPickup* Sea = SeaDrop.Get();
			Report(FString::Printf(TEXT("RESULT pickup drops: KO drop popped up %.0f cm and rests %d; sea drop at x %.0f (strip ends at %.0f, fell in at %.0f) rests %d"),
				PickupHighZ - static_cast<float>(PickupStart.Z) - 20.f, TestPickup.IsValid() && TestPickup->IsResting() ? 1 : 0,
				Sea ? Sea->GetActorLocation().X : 0.0, Value, StartX, Sea && Sea->IsResting() ? 1 : 0));
			APinguPickup::DestroyAll(GetWorld());
		});

	// Every few seconds one floats down from the sky over the main island.
	AddStep(TEXT("pickup_sky"), 7.5f,
		[this]
		{
			APinguPickup::DestroyAll(GetWorld());
			FRandomStream Rand(77);
			TestPickup = APinguPickup::SpawnFromSky(GetWorld(), Rand);
			PickupStart = TestPickup.IsValid() ? TestPickup->GetActorLocation() : FVector::ZeroVector;
			MarkA = -1.f;
		},
		[this](float T)
		{
			if (TestPickup.IsValid() && TestPickup->IsResting() && MarkA < 0.f)
			{
				MarkA = T;
			}
		},
		[this]
		{
			const APinguPickup* Sky = TestPickup.Get();
			const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
			const FBox Bounds = Island ? Island->GetBounds() : FBox(ForceInit);
			const bool bOnIsland = Sky && Bounds.IsValid && Sky->GetActorLocation().X >= Bounds.Min.X && Sky->GetActorLocation().X <= Bounds.Max.X;
			Report(FString::Printf(TEXT("RESULT pickup sky drop: spawned %d at z %.0f, landed %d after %.1f s at x %.0f z %.0f, on the island %d"),
				PickupStart.IsZero() ? 0 : 1, PickupStart.Z, MarkA >= 0.f ? 1 : 0, MarkA, Sky ? Sky->GetActorLocation().X : 0.0, Sky ? Sky->GetActorLocation().Z : 0.0, bOnIsland ? 1 : 0));
			APinguPickup::DestroyAll(GetWorld());
		});
}

// ============================================================================ jump forgiveness

void APenguinAutoTest::AddJumpForgivenessSteps(const FVector& FlatStart)
{
	AddStep(TEXT("stand_flat"), 0.7f, [this, FlatStart] { VirtualInput = FVector2D::ZeroVector; Teleport(FlatStart); }, nullptr,
		[this] { StandZ = static_cast<float>(Location().Z); });

	// Key down and key up in the same frame: a short hop like any tap, not a 30 cm blip.
	AddStep(TEXT("one_frame_tap"), 1.0f, [this] { ResetAir(); Move->JumpPressed(); Move->JumpReleased(); },
		[this](float T) { TrackAir(T); },
		[this]
		{
			const float Height = PeakZ - StartZ;
			Report(FString::Printf(TEXT("RESULT one-frame tap: height %.0f cm (%s), airtime %.3f s"), Height, Height >= 70.f ? TEXT("a short hop") : TEXT("TOO LOW"), LandTime));
		});

	// Hit (stunned 0.4 s), jump pressed 0.1 s into the stun: it must come out the moment the stun is over.
	AddStep(TEXT("stun_jump"), 1.4f,
		[this] { TimeA = -1.f; CountA = Move->JumpEvents + Move->AirJumpEvents; },
		[this](float T)
		{
			if (!bFlag && T > 0.3f)
			{
				Move->ApplyKnockback(FVector2D(-300.f, 0.f), 0.4f);
				bFlag = true;
				MarkA = T;
			}
			if (bFlag && MarkB < 0.f && T >= MarkA + 0.1f)
			{
				Move->JumpPressed();
				MarkB = T;
			}
			if (MarkB > 0.f && T > MarkB + 0.08f)
			{
				Move->JumpReleased();
			}
			if (TimeA < 0.f && Move->JumpEvents + Move->AirJumpEvents > CountA)
			{
				TimeA = T;
			}
		},
		[this]
		{
			Move->JumpReleased();
			const bool bJumped = TimeA > 0.f;
			Report(FString::Printf(TEXT("RESULT stun buffer: pressed %.2f s into a 0.40 s stun, jumped %d, %.2f s after the stun ended"),
				MarkB - MarkA, bJumped ? 1 : 0, bJumped ? TimeA - (MarkA + 0.4f) : -1.f));
		});

	// Walked down a block (100 cm) and pressed jump late - after the coyote time, feet 45 cm above the ground: a ground jump,
	// and the double jump is still there (it used to be spent, so the press meant for the double jump did nothing).
	AddStep(TEXT("step_down_late_jump"), 1.4f,
		[this, FlatStart]
		{
			Teleport(FVector(FlatStart.X + 400.f, 0.f, StandZ + 100.f));
			CountA = Move->JumpEvents;
			CountB = Move->AirJumpEvents;
			TimeA = -1.f;
			Value = -1.f;
		},
		[this](float T)
		{
			if (!bFlag && T > 0.02f && Move->IsFalling() && Location().Z - StandZ < 45.f)
			{
				bFlag = true;
				MarkA = T;
				StartZ = static_cast<float>(Location().Z) - StandZ;
				Move->JumpPressed();
			}
			if (bFlag && TimeA < 0.f && (Move->JumpEvents > CountA || Move->AirJumpEvents > CountB))
			{
				TimeA = T;
				Value = static_cast<float>(Move->GetAirJumpsLeft());
			}
			if (bFlag && T > MarkA + 0.25f)
			{
				Move->JumpReleased();
			}
		},
		[this]
		{
			Move->JumpReleased();
			Report(FString::Printf(TEXT("RESULT late jump after stepping down a block (pressed %.2f s after leaving, feet %.0f cm up): ground jump %d, double jump %d, double jump still there %d"),
				MarkA, StartZ, Move->JumpEvents - CountA, Move->AirJumpEvents - CountB, Value > 0.f ? 1 : 0));
		});

	// Jump and double jump, then press again just before landing: a ground jump that gives the double jump back (it used
	// to be dropped - no double jump left - and the jump never came).
	AddStep(TEXT("jump_before_landing"), 2.4f,
		[this, FlatStart]
		{
			Teleport(FVector(FlatStart.X + 800.f, 0.f, StandZ));
			TimeA = -1.f;
			Value = -1.f;
		},
		[this](float T)
		{
			if (MarkA < 0.f && T > 0.3f)
			{
				Move->JumpPressed();
				MarkA = T;
			}
			if (MarkA > 0.f && MarkB < 0.f && T > MarkA + 0.35f)
			{
				Move->JumpReleased();
				Move->JumpPressed(); // the double jump
				MarkB = T;
			}
			if (MarkB > 0.f && !bFlag && T > MarkB + 0.2f)
			{
				Move->JumpReleased();
				CountA = Move->JumpEvents;
				CountB = Move->AirJumpEvents;
				bFlag = true;
			}
			if (bFlag && !bShotDone && Move->IsFalling() && Move->Velocity.Z < 0.f && Location().Z - StandZ < 40.f)
			{
				bShotDone = true;
				StartZ = static_cast<float>(Location().Z) - StandZ;
				Move->JumpPressed();
			}
			if (bShotDone && TimeA < 0.f && (Move->JumpEvents > CountA || Move->AirJumpEvents > CountB))
			{
				TimeA = T;
				Value = static_cast<float>(Move->GetAirJumpsLeft());
				Move->JumpReleased();
			}
		},
		[this]
		{
			Move->JumpReleased();
			Report(FString::Printf(TEXT("RESULT jump pressed %.0f cm before landing with the double jump used: ground jump %d, double jump back %d"),
				StartZ, Move->JumpEvents - CountA, Value > 0.f ? 1 : 0));
		});

	// A small upward push while standing (a slap aimed a little up): stay on the ground, the next press is a ground jump.
	AddStep(TEXT("small_push_then_jump"), 1.1f,
		[this, FlatStart] { Teleport(FVector(FlatStart.X + 1200.f, 0.f, StandZ)); Value = 0.f; },
		[this](float T)
		{
			if (!bFlag && T > 0.4f)
			{
				bFlag = true;
				MarkA = T;
				CountA = Move->JumpEvents;
				CountB = Move->AirJumpEvents;
				Move->AddAbilityImpulse(FVector2D(300.f, 120.f));
			}
			if (bFlag && T > MarkA + 0.01f && T < MarkA + 0.06f && !Move->IsMovingOnGround())
			{
				Value = 1.f; // lifted off
			}
			if (bFlag && MarkB < 0.f && T > MarkA + 0.06f)
			{
				Move->JumpPressed();
				MarkB = T;
			}
			if (MarkB > 0.f && T > MarkB + 0.2f)
			{
				Move->JumpReleased();
			}
		},
		[this]
		{
			Move->JumpReleased();
			Report(FString::Printf(TEXT("RESULT small upward push while standing: stayed on the ground %d, next press a ground jump %d (double jump %d)"),
				Value > 0.f ? 0 : 1, Move->JumpEvents - CountA, Move->AirJumpEvents - CountB));
		});
}

// ============================================================================ real keyboard input

namespace PenguinInputTestPrivate
{
	constexpr int32 NumBindable = static_cast<int32>(EPinguInputAction::Count);

	FString DescribeKeyList(const TArray<FKey>& KeyList)
	{
		FString Text;
		for (const FKey& Key : KeyList)
		{
			if (!Text.IsEmpty())
			{
				Text += TEXT(" ");
			}
			Text += Key.ToString();
		}
		return Text.IsEmpty() ? FString(TEXT("none")) : Text;
	}

	/** Binds Key as the Slot key of Action (bOnly: the other slot is cleared) and says what the keys are now. */
	FString BindKey(EPinguInputAction Action, int32 Slot, const FKey& Key, bool bOnly)
	{
		UPinguSettings* Settings = UPinguSettings::Get();
		if (!Settings)
		{
			return TEXT("FAIL no settings object to bind keys with");
		}
		Settings->SetKey(Action, Slot, Key);
		if (bOnly)
		{
			Settings->ClearKey(Action, 1 - Slot);
		}
		return FString::Printf(TEXT("bound %s to %s while playing: its keys are now %s, the jump keys %s"), *Key.ToString(),
			*UPinguSettings::GetActionName(Action).ToString(), *DescribeKeyList(Settings->GetKeys(Action)),
			*DescribeKeyList(UPenguinInputConfig::GetJumpKeys()));
	}

	FString ResetBindings()
	{
		if (UPinguSettings* Settings = UPinguSettings::Get())
		{
			Settings->ResetKeyBindings();
		}
		return FString::Printf(TEXT("key bindings back to the defaults: the jump keys %s"), *DescribeKeyList(UPenguinInputConfig::GetJumpKeys()));
	}

	APenguinHUD* FindPlayerHUD(const UWorld* World)
	{
		const APlayerController* FirstController = World ? World->GetFirstPlayerController() : nullptr;
		return FirstController ? Cast<APenguinHUD>(FirstController->GetHUD()) : nullptr;
	}
}

APenguinInputTest::APenguinInputTest()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
}

void APenguinInputTest::BeginPlay()
{
	Super::BeginPlay();
	SaveBindings();
}

void APenguinInputTest::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RestoreBindings(); // also when the test ends early
	Super::EndPlay(EndPlayReason);
}

void APenguinInputTest::SaveBindings()
{
	UPinguSettings* Settings = UPinguSettings::Get();
	if (!Settings || bBindingsSaved)
	{
		return;
	}
	SavedKeys.Reset();
	FString Text;
	for (int32 Index = 0; Index < PenguinInputTestPrivate::NumBindable; ++Index)
	{
		const EPinguInputAction Action = static_cast<EPinguInputAction>(Index);
		const FKey Primary = Settings->GetKey(Action, 0);
		const FKey Secondary = Settings->GetKey(Action, 1);
		SavedKeys.Add(Primary);
		SavedKeys.Add(Secondary);
		Text += FString::Printf(TEXT("%s%s %s / %s"), Index > 0 ? TEXT(", ") : TEXT(""), *UPinguSettings::GetActionName(Action).ToString(),
			Primary.IsValid() ? *Primary.ToString() : TEXT("-"), Secondary.IsValid() ? *Secondary.ToString() : TEXT("-"));
	}
	bBindingsSaved = true;
	// In the log, so they could be put back by hand should the test never get to it.
	Report(FString::Printf(TEXT("the player's key bindings (put back at the end): %s"), *Text));
	Settings->ResetKeyBindings();
	Report(FString::Printf(TEXT("the test plays with the default bindings: the jump keys %s"),
		*PenguinInputTestPrivate::DescribeKeyList(UPenguinInputConfig::GetJumpKeys())));
}

void APenguinInputTest::RestoreBindings()
{
	if (!bBindingsSaved)
	{
		return;
	}
	bBindingsSaved = false;
	UPinguSettings* Settings = UPinguSettings::Get();
	if (!Settings)
	{
		return;
	}
	// Slot by slot, only what differs (every change is saved). The saved keys are all different, so a later slot never
	// takes its key away from one that is done already.
	int32 Changed = 0;
	for (int32 Index = 0; Index < PenguinInputTestPrivate::NumBindable && SavedKeys.Num() >= (Index + 1) * 2; ++Index)
	{
		const EPinguInputAction Action = static_cast<EPinguInputAction>(Index);
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			const FKey& Saved = SavedKeys[Index * 2 + Slot];
			if (Settings->GetKey(Action, Slot) == Saved)
			{
				continue;
			}
			++Changed;
			if (Saved.IsValid())
			{
				Settings->SetKey(Action, Slot, Saved);
			}
			else
			{
				Settings->ClearKey(Action, Slot);
			}
		}
	}
	Report(FString::Printf(TEXT("the player's key bindings are back (%d keys put back)"), Changed));
}

void APenguinInputTest::Report(const FString& Message) const
{
	UE_LOG(LogPingu, Display, TEXT("[INPUTTEST] %s"), *Message);
}

FString APenguinInputTest::DescribeFocus() const
{
	if (!FSlateApplication::IsInitialized())
	{
		return TEXT("no slate");
	}
	const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(0);
	const TSharedPtr<SViewport> Viewport = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetGameViewportWidget() : nullptr;
	const bool bViewport = Focused.IsValid() && Viewport.IsValid() && Focused == StaticCastSharedPtr<SWidget>(Viewport);
	return FString::Printf(TEXT("focus on %s (game viewport %d)"), Focused.IsValid() ? *Focused->GetTypeAsString() : TEXT("nothing"), bViewport ? 1 : 0);
}

void APenguinInputTest::SendKey(const FKey& Key, bool bDown)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const FKeyEvent Event(Key, Slate.GetModifierKeys(), 0u, false, 0u, 0u);
	if (bDown)
	{
		Slate.ProcessKeyDownEvent(Event);
	}
	else
	{
		Slate.ProcessKeyUpEvent(Event);
	}
}

APenguinCharacter* APenguinInputTest::GetPlayerPenguin() const
{
	const APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	return Controller ? Cast<APenguinCharacter>(Controller->GetPawn()) : nullptr;
}

void APenguinInputTest::PrepareArena()
{
	// A long flat strip so walking back and forth never ends in the sea; the bots stand still far away.
	if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
	{
		Island->Layout = EIslandLayout::Flat;
		Island->Rebuild();
	}
	APenguinCharacter* Player = GetPlayerPenguin();
	float BotX = -2500.f;
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		if (Penguin == Player)
		{
			continue;
		}
		if (UPenguinBotComponent* Bot = Penguin->GetBot())
		{
			Bot->SetBotEnabled(false);
		}
		Penguin->SetActorLocation(FVector(BotX, 0.f, 62.f), false, nullptr, ETeleportType::TeleportPhysics);
		BotX = BotX < 0.f ? 2500.f : BotX + 150.f;
	}
	if (Player)
	{
		Player->SetActorLocation(FVector(0.f, 0.f, 62.f), false, nullptr, ETeleportType::TeleportPhysics);
		if (UPenguinMovementComponent* Move = Player->GetPenguinMovement())
		{
			Move->StopMovementImmediately();
		}
	}
}

int32 APenguinInputTest::CountJumps(const UPenguinMovementComponent* Move, EJumpKind Kind)
{
	if (!Move)
	{
		return 0;
	}
	switch (Kind)
	{
	case EJumpKind::Ground: return Move->JumpEvents;
	case EJumpKind::Air: return Move->AirJumpEvents;
	default: return Move->JumpEvents + Move->AirJumpEvents;
	}
}

void APenguinInputTest::BuildScript()
{
	using namespace PenguinInputTestPrivate;
	const UPinguSettings* Settings = UPinguSettings::Get();
	auto KeysOf = [Settings](EPinguInputAction Action) { return Settings ? Settings->GetKeys(Action) : TArray<FKey>(); };
	auto Hold = [this](float At, const FKey& Key, bool bDown)
	{
		Keys.Add({ At, Key, bDown });
	};
	auto Press = [this](float At, const FKey& Key, float HoldTime)
	{
		Keys.Add({ At, Key, true });
		Keys.Add({ At + HoldTime, Key, false });
	};
	auto AddCheck = [this](float Start, float At, const FString& Label, EExpect Expect) -> FCheck&
	{
		Checks.Add(FCheck());
		FCheck& Item = Checks.Last();
		Item.StartTime = Start;
		Item.Time = At;
		Item.Label = Label;
		Item.Expect = Expect;
		return Item;
	};
	auto ExpectJump = [&AddCheck](float Start, float At, const FString& Label, EJumpKind Kind, float MinHeight = 0.f)
	{
		FCheck& Item = AddCheck(Start, At, Label, EExpect::Jump);
		Item.Kind = Kind;
		Item.MinHeight = MinHeight;
	};
	// No jump at all may come out, although the key reached the game (and with bWantUp: aimed up).
	auto ExpectNoJump = [&AddCheck](float Start, float At, const FString& Label, const FKey& Key, bool bWantUp)
	{
		FCheck& Item = AddCheck(Start, At, Label, EExpect::NoJump);
		Item.Kind = EJumpKind::Any;
		Item.ProofKey = Key;
		Item.bWantUpInput = bWantUp;
	};
	auto AddAction = [this](float At, TFunction<void()> Fn)
	{
		Actions.Add({ At, MoveTemp(Fn) });
	};

	// Read from the default bindings the test plays with (W / Up aim up and do not jump - unless the defaults change).
	const bool bWAimsUp = KeysOf(EPinguInputAction::Up).Contains(EKeys::W);
	const bool bUpArrowJumps = KeysOf(EPinguInputAction::Jump).Contains(EKeys::Up);
	const bool bUpArrowAimsUp = KeysOf(EPinguInputAction::Up).Contains(EKeys::Up);
	// K becomes the second jump key in the Jump slot that does not hold Space.
	const int32 SecondJumpSlot = Settings && Settings->GetKey(EPinguInputAction::Jump, 0) == EKeys::SpaceBar ? 1 : 0;

	// Walk right, jump with Space.
	Hold(0.3f, EKeys::D, true);
	Press(0.8f, EKeys::SpaceBar, 0.2f);
	ExpectJump(0.75f, 1.6f, TEXT("jump while walking (Space)"), EJumpKind::Ground);
	// Walk left and press W: W only aims up (an 8-way dash up), it must not jump.
	Hold(1.7f, EKeys::D, false);
	Hold(1.7f, EKeys::A, true);
	Press(2.0f, EKeys::W, 0.25f);
	ExpectNoJump(1.95f, 2.8f, TEXT("W does not jump (walking)"), EKeys::W, bWAimsUp);
	// Walk right, jump and double jump with Space.
	Hold(2.9f, EKeys::A, false);
	Hold(2.9f, EKeys::D, true);
	Press(3.2f, EKeys::SpaceBar, 0.15f);
	Press(3.6f, EKeys::SpaceBar, 0.15f);
	ExpectJump(3.5f, 4.3f, TEXT("double jump while walking (Space)"), EJumpKind::Air);
	// Walk left, press the Up arrow: a jump only if it is bound to Jump (not by default: it aims up like W).
	Hold(4.4f, EKeys::D, false);
	Hold(4.4f, EKeys::A, true);
	Press(4.7f, EKeys::Up, 0.2f);
	if (bUpArrowJumps)
	{
		ExpectJump(4.65f, 5.5f, TEXT("jump while walking (Up, bound to Jump)"), EJumpKind::Ground);
	}
	else
	{
		ExpectNoJump(4.65f, 5.5f, TEXT("Up does not jump (walking)"), EKeys::Up, bUpArrowAimsUp);
	}
	Hold(5.6f, EKeys::A, false);
	// Walking right with the arrows, jump with Space.
	Hold(5.9f, EKeys::Right, true);
	Press(6.2f, EKeys::SpaceBar, 0.2f);
	ExpectJump(6.15f, 7.0f, TEXT("jump while walking with the arrows (Space)"), EJumpKind::Ground);
	Hold(7.1f, EKeys::Right, false);
	// A second jump key bound while playing (K): it jumps at once, and Space still double jumps while K is held (a second
	// jump key used to do nothing while the first one was down).
	AddAction(7.3f, [this, SecondJumpSlot] { Report(BindKey(EPinguInputAction::Jump, SecondJumpSlot, EKeys::K, false)); });
	Press(7.8f, EKeys::K, 0.9f);
	ExpectJump(7.75f, 8.1f, TEXT("jump with K, bound as a second jump key while playing"), EJumpKind::Ground);
	Press(8.15f, EKeys::SpaceBar, 0.12f);
	ExpectJump(8.1f, 8.65f, TEXT("double jump with Space while K is still held"), EJumpKind::Air);
	AddAction(9.0f, [this] { Report(ResetBindings()); });
	// Walking left, a tap so quick that key down and key up arrive in the same frame: still a proper short hop.
	Hold(9.4f, EKeys::A, true);
	Press(9.8f, EKeys::SpaceBar, 0.f); // down and up in the same frame (the stable sort keeps down first)
	ExpectJump(9.75f, 10.5f, TEXT("tap inside one frame while walking (Space)"), EJumpKind::Ground, 70.f);
	Hold(10.6f, EKeys::A, false);
	// Hit by a snowball (stunned for 0.4 s), jump pressed 0.1 s later: it must come out when the stun ends.
	AddAction(11.0f, [this]
	{
		if (APenguinCharacter* Player = GetPlayerPenguin())
		{
			if (UPenguinMovementComponent* PlayerMove = Player->GetPenguinMovement())
			{
				PlayerMove->ApplyKnockback(FVector2D(-300.f, 0.f), 0.4f);
			}
		}
	});
	Press(11.1f, EKeys::SpaceBar, 0.1f);
	ExpectJump(11.05f, 11.9f, TEXT("jump pressed during a hit stun (Space)"), EJumpKind::Any);
	// Jump rebound to K (and nothing else): K jumps, Space no longer does.
	AddAction(12.2f, [this] { Report(BindKey(EPinguInputAction::Jump, 0, EKeys::K, true)); });
	Press(12.6f, EKeys::SpaceBar, 0.2f);
	ExpectNoJump(12.55f, 13.4f, TEXT("Space does not jump after rebinding Jump to K"), EKeys::SpaceBar, false);
	Press(13.6f, EKeys::K, 0.2f);
	ExpectJump(13.55f, 14.4f, TEXT("K jumps after rebinding Jump to K"), EJumpKind::Ground);
	AddAction(14.6f, [this] { Report(ResetBindings()); });
	// A movement key bound while playing works at once (through the rebuilt Enhanced Input mapping, not the jump keys).
	AddAction(14.9f, [this] { Report(BindKey(EPinguInputAction::MoveRight, 1, EKeys::O, false)); });
	Press(15.3f, EKeys::O, 0.6f);
	{
		FCheck& Walk = AddCheck(15.25f, 16.0f, TEXT("walk right with O, bound to move right while playing"), EExpect::WalkRight);
		Walk.ProofKey = EKeys::O;
		Walk.MinDistance = 120.f;
	}
	AddAction(16.1f, [this] { Report(ResetBindings()); });
	// Back on the default keys: Space jumps again.
	Press(16.5f, EKeys::SpaceBar, 0.2f);
	ExpectJump(16.45f, 17.3f, TEXT("jump with Space after the bindings were reset"), EJumpKind::Ground);
	// A menu takes the keys: D held when the pause menu opens and let go while it is open (the key up goes to the menu)
	// must not stay down when the game goes on.
	Hold(17.6f, EKeys::D, true);
	AddAction(18.0f, [this]
	{
		if (APenguinHUD* PauseHUD = FindPlayerHUD(GetWorld()))
		{
			PauseHUD->TogglePauseMenu();
			Report(FString::Printf(TEXT("pause menu opened: menu open %d"), PauseHUD->IsMenuOpen() ? 1 : 0));
		}
	});
	Hold(18.3f, EKeys::D, false);
	AddAction(18.6f, [this]
	{
		if (APenguinHUD* PauseHUD = FindPlayerHUD(GetWorld()))
		{
			PauseHUD->TogglePauseMenu();
			const bool bStillOpen = PauseHUD->IsMenuOpen();
			if (bStillOpen)
			{
				PauseHUD->Action_Resume(); // the second Esc did not close it: RESUME does
			}
			Report(FString::Printf(TEXT("pause menu closed: by the second toggle %d, menu open %d"), bStillOpen ? 0 : 1, PauseHUD->IsMenuOpen() ? 1 : 0));
		}
	});
	AddCheck(18.0f, 19.3f, TEXT("no key stuck down after the pause menu (D let go while it was open)"), EExpect::StandStill);
	// Ctrl: a belly slide (left Control by default).
	Press(19.8f, EKeys::LeftControl, 0.15f);
	{
		FCheck& Slide = AddCheck(19.75f, 20.9f, TEXT("belly slide with Ctrl"), EExpect::Slide);
		Slide.ProofKey = EKeys::LeftControl;
		Slide.MinDistance = 300.f;
	}
	// A size ray's key held down: the ray on yourself (J, the left ability's second key by default), whatever the aim.
	AddAction(21.1f, [this]
	{
		if (APenguinCharacter* Player = GetPlayerPenguin())
		{
			if (UPenguinAbilityComponent* PlayerAbilities = Player->GetAbilities())
			{
				PlayerAbilities->SetAbilities({ EPenguinAbility::GrowRay, EPenguinAbility::Snowball, EPenguinAbility::Icicle });
				Report(TEXT("the left ability is a grow ray now"));
			}
		}
	});
	Press(21.3f, EKeys::J, 0.75f);
	{
		FCheck& Grow = AddCheck(21.25f, 22.3f, TEXT("grow ray on yourself with J held"), EExpect::GrowSelf);
		Grow.ProofKey = EKeys::J;
	}
	// The icicle on J: a tap is a weak lob (about 600 cm/s, it drops after ~2 m), held a second it flies fast (2600 cm/s) and
	// perfectly straight (the release throws it).
	AddAction(22.8f, [this]
	{
		if (APenguinCharacter* Player = GetPlayerPenguin())
		{
			if (UPenguinAbilityComponent* PlayerAbilities = Player->GetAbilities())
			{
				PlayerAbilities->SetAbilities({ EPenguinAbility::Icicle, EPenguinAbility::Snowball, EPenguinAbility::GrowRay });
				Report(TEXT("the left ability is the icicle now"));
			}
		}
	});
	Press(23.1f, EKeys::J, 0.04f);
	{
		FCheck& Tap = AddCheck(23.05f, 24.1f, TEXT("icicle tapped with J: a weak lob"), EExpect::Icicle);
		Tap.ProofKey = EKeys::J;
		Tap.MaxCharge = 0.15f;
	}
	AddAction(24.2f, [this]
	{
		if (APenguinCharacter* Player = GetPlayerPenguin())
		{
			if (UPenguinAbilityComponent* PlayerAbilities = Player->GetAbilities())
			{
				PlayerAbilities->SetAbilities({ EPenguinAbility::Icicle, EPenguinAbility::Snowball, EPenguinAbility::GrowRay }); // ready again
			}
		}
	});
	Press(24.4f, EKeys::J, 1.65f); // (the full charge is 1.5 s since 2026-11-05)
	{
		FCheck& Far = AddCheck(24.35f, 27.5f, TEXT("icicle held with J for 1.65 s: a full throw (fast, straight)"), EExpect::Icicle);
		Far.ProofKey = EKeys::J;
		Far.MinCharge = 0.95f;
	}
	ScriptEnd = 27.9f;
	Keys.StableSort([](const FKeyStep& A, const FKeyStep& B) { return A.Time < B.Time; });
}

void APenguinInputTest::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.1f);
	StageTime += Dt;
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!GameState || !Controller)
	{
		return;
	}
	switch (Stage)
	{
	case 0: // the title menu: start a match like a player would (the menu had keyboard focus on PLAY)
		if (GameState->GetPhase() == EPinguPhase::Menu && StageTime > 2.f)
		{
			Report(FString::Printf(TEXT("menu: %s"), *DescribeFocus()));
			if (APenguinHUD* HUD = Cast<APenguinHUD>(Controller->GetHUD()))
			{
				HUD->Action_Play(); // your own lobby ...
			}
			Stage = 1;
			StageTime = 0.f;
		}
		break;

	case 1: // ... START MATCH there, then wait for GO
		if (GameState->GetPhase() == EPinguPhase::Lobby && !bLobbyStartSent && StageTime > 0.5f)
		{
			Report(FString::Printf(TEXT("lobby: %s"), *DescribeFocus()));
			if (APenguinHUD* HUD = Cast<APenguinHUD>(Controller->GetHUD()))
			{
				HUD->Action_StartLobbyMatch();
			}
			bLobbyStartSent = true;
		}
		if (GameState->GetPhase() == EPinguPhase::Fight && StageTime > 0.2f)
		{
			PrepareArena();
			BuildScript();
			Report(FString::Printf(TEXT("fight: %s"), *DescribeFocus()));
			Stage = 2;
			StageTime = 0.f;
		}
		else if (StageTime > 15.f)
		{
			Report(TEXT("RESULT FAIL the match never started"));
			Stage = 3;
		}
		break;

	case 2: // the key script
	{
		APenguinCharacter* Player = GetPlayerPenguin();
		const UPenguinMovementComponent* Move = Player ? Player->GetPenguinMovement() : nullptr;
		const APenguinPlayerController* PenguinController = Cast<APenguinPlayerController>(Controller);
		for (FAction& Action : Actions)
		{
			if (!Action.bDone && StageTime >= Action.Time)
			{
				Action.bDone = true;
				Action.Do();
			}
		}
		// Keys due this frame all go in now (a down and an up with nearly the same time land in the same frame).
		while (NextKey < Keys.Num() && Keys[NextKey].Time <= StageTime)
		{
			SendKey(Keys[NextKey].Key, Keys[NextKey].bDown);
			++NextKey;
		}
		const FVector Where = Player ? Player->GetActorLocation() : FVector::ZeroVector;
		for (FCheck& Item : Checks)
		{
			if (!Move)
			{
				break;
			}
			const UPenguinAbilityComponent* PlayerAbilities = Player ? Player->GetAbilities() : nullptr;
			if (!Item.bStarted && StageTime >= Item.StartTime)
			{
				Item.bStarted = true;
				Item.CountAtStart = Item.Expect == EExpect::Slide ? Move->SlideEvents
					: Item.Expect == EExpect::Icicle ? (PlayerAbilities ? PlayerAbilities->GetIciclesThrown() : 0) : CountJumps(Move, Item.Kind);
				Item.StartZ = Item.PeakZ = static_cast<float>(Where.Z);
				Item.StartX = static_cast<float>(Where.X);
			}
			if (!Item.bStarted || Item.bDone)
			{
				continue;
			}
			Item.PeakZ = FMath::Max(Item.PeakZ, static_cast<float>(Where.Z));
			if (Item.Expect == EExpect::Icicle && PlayerAbilities)
			{
				// The icicle thrown in the window: its speed and gravity as it left, and how far it got (from the player).
				if (Item.IcicleSpeed < 0.f && PlayerAbilities->GetIciclesThrown() > Item.CountAtStart)
				{
					float Nearest = 1.e9f;
					for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
					{
						const float Dist = static_cast<float>(FVector::Dist(It->GetActorLocation(), Where));
						if (It->GetKind() == EPenguinProjectileKind::Icicle && It->GetShooter() == Player && It->IsFlying() && Dist < Nearest)
						{
							Nearest = Dist;
							Item.Icicle = *It;
						}
					}
					if (const APenguinProjectile* Thrown = Item.Icicle.Get())
					{
						Item.IcicleSpeed = static_cast<float>(Thrown->GetVelocity().Size());
						Item.IcicleGravity = Thrown->GetGravity();
						Item.IcicleFromX = static_cast<float>(Where.X);
					}
				}
				if (const APenguinProjectile* Flying = Item.Icicle.Get())
				{
					Item.IcicleFar = FMath::Max(Item.IcicleFar, FMath::Abs(static_cast<float>(Flying->GetActorLocation().X) - Item.IcicleFromX));
					if (!Flying->IsFlying())
					{
						Item.Icicle.Reset();
					}
				}
			}
			Item.bProofSeen |= Item.ProofKey.IsValid() && Controller->IsInputKeyDown(Item.ProofKey);
			Item.bUpSeen |= Move->GetMoveInput().Y > 0.5f;
			Item.bMenuSeen |= PenguinController && PenguinController->IsInMenuMode();
			if (StageTime < Item.Time)
			{
				continue;
			}
			Item.bDone = true;
			const int32 Count = CountJumps(Move, Item.Kind) - Item.CountAtStart;
			const float Height = Item.PeakZ - Item.StartZ;
			bool bOk = false;
			FString Detail;
			switch (Item.Expect)
			{
			case EExpect::Jump:
				bOk = Count > 0 && Height >= Item.MinHeight;
				Detail = FString::Printf(TEXT("%s (%d, %.0f cm high)"), bOk ? TEXT("ok") : (Count > 0 ? TEXT("TOO LOW") : TEXT("IGNORED")), Count, Height);
				break;
			case EExpect::NoJump:
			{
				const bool bUpOk = !Item.bWantUpInput || Item.bUpSeen;
				bOk = Count == 0 && Item.bProofSeen && bUpOk;
				Detail = FString::Printf(TEXT("%s (%d jumps, the key reached the game %d, aimed up %d)"),
					Count > 0 ? TEXT("JUMPED") : (!Item.bProofSeen ? TEXT("KEY NEVER ARRIVED") : (bUpOk ? TEXT("ok") : TEXT("NO UP INPUT"))),
					Count, Item.bProofSeen ? 1 : 0, Item.bUpSeen ? 1 : 0);
				break;
			}
			case EExpect::WalkRight:
			{
				const float Moved = static_cast<float>(Where.X) - Item.StartX;
				bOk = Item.bProofSeen && Moved >= Item.MinDistance;
				Detail = FString::Printf(TEXT("%s (moved %.0f cm, the key reached the game %d)"), bOk ? TEXT("ok") : TEXT("DID NOT WALK"), Moved,
					Item.bProofSeen ? 1 : 0);
				break;
			}
			case EExpect::Slide:
			{
				const int32 Slides = Move->SlideEvents - Item.CountAtStart;
				const float Moved = FMath::Abs(static_cast<float>(Where.X) - Item.StartX);
				bOk = Item.bProofSeen && Slides > 0 && Moved >= Item.MinDistance;
				Detail = FString::Printf(TEXT("%s (%d slides, moved %.0f cm, the key reached the game %d)"),
					bOk ? TEXT("ok") : (Slides > 0 ? TEXT("TOO SHORT") : TEXT("NO SLIDE")), Slides, Moved, Item.bProofSeen ? 1 : 0);
				break;
			}
			case EExpect::GrowSelf:
			{
				const float SizeTarget = Player ? Player->GetSizeTarget() : 0.f;
				bOk = Item.bProofSeen && SizeTarget > 1.25f;
				Detail = FString::Printf(TEXT("%s (size target %.2f, the key reached the game %d)"), bOk ? TEXT("ok") : TEXT("DID NOT GROW"),
					SizeTarget, Item.bProofSeen ? 1 : 0);
				break;
			}
			case EExpect::Icicle:
			{
				// One icicle with the charge the key's hold gave: up to half a charge a slow, heavy lob (a tap: about 600 cm/s and a
				// drop of 2800), past that fast and straight (a full second: 2600 cm/s, no drop).
				const int32 Thrown = PlayerAbilities ? PlayerAbilities->GetIciclesThrown() - Item.CountAtStart : 0;
				const float Charge = PlayerAbilities ? PlayerAbilities->GetLastIciclePower() : -1.f;
				const bool bLob = Item.MaxCharge <= 0.5f;
				const bool bFlight = bLob ? Item.IcicleSpeed > 560.f && Item.IcicleSpeed < 760.f && Item.IcicleGravity > 2500.f
					: FMath::Abs(Item.IcicleSpeed - PinguTuning::IcicleSpeed) < 15.f && Item.IcicleGravity >= 0.f && Item.IcicleGravity < 1.f;
				bOk = Item.bProofSeen && Thrown == 1 && Charge >= Item.MinCharge && Charge <= Item.MaxCharge && bFlight;
				Detail = FString::Printf(TEXT("%s (%d thrown, charge %.2f, held %.2f s, left at %.0f cm/s with gravity %.0f, %.0f cm away so far, the key reached the game %d)"),
					bOk ? TEXT("ok") : (Thrown != 1 ? TEXT("NOT ONE ICICLE") : TEXT("WRONG THROW")), Thrown, Charge,
					PlayerAbilities ? PlayerAbilities->GetLastIcicleHold() : 0.f, Item.IcicleSpeed, Item.IcicleGravity, Item.IcicleFar, Item.bProofSeen ? 1 : 0);
				break;
			}
			case EExpect::StandStill:
			{
				const float Speed = FMath::Abs(static_cast<float>(Move->Velocity.X));
				const float MoveX = static_cast<float>(Move->GetMoveInput().X);
				bOk = Item.bMenuSeen && Speed < 30.f && FMath::Abs(MoveX) < 0.1f;
				Detail = FString::Printf(TEXT("%s (the menu took the input %d, speed %.0f, move input %.2f)"),
					bOk ? TEXT("ok") : (Item.bMenuSeen ? TEXT("KEY STUCK") : TEXT("NO MENU")), Item.bMenuSeen ? 1 : 0, Speed, MoveX);
				break;
			}
			}
			Passed += bOk ? 1 : 0;
			Report(FString::Printf(TEXT("RESULT %s: %s"), *Item.Label, *Detail));
		}
		if (StageTime > ScriptEnd)
		{
			Report(FString::Printf(TEXT("RESULT input: %d of %d checks ok, %s, player alive %d"), Passed, Checks.Num(), *DescribeFocus(),
				Player && Player->IsAlive() ? 1 : 0));
			if (APenguinPlayerController* JumpStatsController = Cast<APenguinPlayerController>(Controller))
			{
				JumpStatsController->LogJumpStats(TEXT("input test"));
			}
			Stage = 3;
		}
		break;
	}

	case 3:
		Stage = 4;
		RestoreBindings();
		PinguEndTestRun(this, TEXT("PinguInputTest"));
		break;

	default:
		break;
	}
}

// ============================================================================ -PinguLobbyTest

namespace PinguLobbyTestPrivate
{
	/** The lobby's + card (the button around its "ADD BOT" text) somewhere under Widget, or null. */
	TSharedPtr<SWidget> FindAddBotButtonUnder(const TSharedRef<SWidget>& Widget, int32 Depth)
	{
		if (Depth > 80)
		{
			return nullptr;
		}
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			if (StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString() == TEXT("ADD BOT"))
			{
				for (TSharedPtr<SWidget> Up = Widget->GetParentWidget(); Up.IsValid(); Up = Up->GetParentWidget())
				{
					if (Up->GetType() == FName(TEXT("SPinguButton")))
					{
						return Up;
					}
				}
			}
			return nullptr;
		}
		FChildren* Kids = Widget->GetChildren();
		for (int32 Index = 0; Kids && Index < Kids->Num(); ++Index)
		{
			if (TSharedPtr<SWidget> Found = FindAddBotButtonUnder(Kids->GetChildAt(Index), Depth + 1))
			{
				return Found;
			}
		}
		return nullptr;
	}

	/** The + card on the game window right now (a new widget after every rebuild of the cards), or null. */
	TSharedPtr<SWidget> FindAddBotButton()
	{
		UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
		const TSharedPtr<SWindow> Window = Viewport ? Viewport->GetWindow() : nullptr;
		return Window.IsValid() ? FindAddBotButtonUnder(Window.ToSharedRef(), 0) : nullptr;
	}

	/** The left mouse button, as Slate hands it to a widget's own handlers. */
	FPointerEvent LeftMouseEvent()
	{
		TSet<FKey> Pressed;
		Pressed.Add(EKeys::LeftMouseButton);
		return FPointerEvent(static_cast<uint32>(0), FVector2D::ZeroVector, FVector2D::ZeroVector, Pressed, EKeys::LeftMouseButton, 0.f, FModifierKeysState());
	}

	double SlateNow()
	{
		return FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetCurrentTime() : FPlatformTime::Seconds();
	}
}

APinguLobbyTest::APinguLobbyTest()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
}

void APinguLobbyTest::Report(const FString& Message) const
{
	UE_LOG(LogPingu, Display, TEXT("[LOBBYTEST] %s"), *Message);
}

void APinguLobbyTest::Shot(const TCHAR* Name) const
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ClaudeLogs/Lobby") / FString::Printf(TEXT("%s.png"), Name));
	FScreenshotRequest::RequestScreenshot(Path, true, false);
	Report(FString::Printf(TEXT("screenshot %s"), *Path));
}

APenguinHUD* APinguLobbyTest::GetHUD() const
{
	const APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	return Controller ? Cast<APenguinHUD>(Controller->GetHUD()) : nullptr;
}

FString APinguLobbyTest::DescribeCards() const
{
	static const TCHAR* Skills[] = { TEXT("easy"), TEXT("normal"), TEXT("hard") };
	FString Line;
	if (const APenguinHUD* HUD = GetHUD())
	{
		for (const FPinguLobbyPlayer& Card : HUD->GetLobbyCards())
		{
			const FString Team = Card.TeamChoice >= 0 ? APenguinCharacter::GetTeamName(Card.TeamChoice).ToString().ToLower() : FString(TEXT("no team"));
			Line += FString::Printf(TEXT("%s%s%s %s%s%s"), Line.IsEmpty() ? TEXT("") : TEXT(", "), *Card.Name, Card.bYou ? TEXT(" (you)") : TEXT(""), *Team,
				Card.bBot ? TEXT(" ") : TEXT(""), Card.bBot ? Skills[FMath::Clamp(Card.Skill, 0, 2)] : TEXT(""));
		}
	}
	return Line;
}

void APinguLobbyTest::KnockOutAllBut(int32 Team) const
{
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		if (It->IsAlive() && It->GetTeam() != Team)
		{
			It->KnockOut(EPenguinKOReason::Sea, nullptr);
		}
	}
}

void APinguLobbyTest::Finish(const TCHAR* Why)
{
	Report(FString::Printf(TEXT("RESULT lobby test: %d of %d checks ok (%s)"), Passed, Checked, Why));
	Stage = 100;
	PinguEndTestRun(this, TEXT("PinguLobbyTest"));
}

bool APinguLobbyTest::UpdateAddBotDoubleClick(APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check)
{
	using namespace PinguLobbyTestPrivate;
	constexpr int32 ClickDone = 6;
	switch (ClickStep)
	{
	case 0: // three bots to start from (the + card shows)
		for (int32 Guard = 0; Guard < 10 && LobbyHUD->GetBotCount() < 3; ++Guard)
		{
			LobbyHUD->Action_AddBot();
		}
		for (int32 Guard = 0; Guard < 10 && LobbyHUD->GetBotCount() > 3; ++Guard)
		{
			LobbyHUD->Action_RemoveBot(LobbyHUD->GetBotCount() - 1);
		}
		ClickStep = 1;
		ClickAt = StageTime;
		return true;

	case 1: // the first click on the + card (once the cards are built)
	{
		if (StageTime - ClickAt < 0.3f)
		{
			return true;
		}
		const TSharedPtr<SWidget> Button = FindAddBotButton();
		if (!Button.IsValid())
		{
			if (StageTime - ClickAt > 3.f)
			{
				Check(false, TEXT("a double click on the + card adds one bot (no + card found on the screen)"));
				Check(false, TEXT("a double click on a + card that has been up a while still presses it (no + card found on the screen)"));
				ClickStep = ClickDone;
			}
			return true;
		}
		BotsBeforeClick = LobbyHUD->GetBotCount();
		const FPointerEvent Event = LeftMouseEvent();
		Button->OnMouseButtonDown(Button->GetTickSpaceGeometry(), Event);
		Button->OnMouseButtonUp(Button->GetTickSpaceGeometry(), Event);
		ClickedAddButton = Button;
		ClickRealTime = SlateNow();
		ClickAt = StageTime;
		ClickStep = 2;
		Report(FString::Printf(TEXT("+ card clicked (%d bots before)"), BotsBeforeClick));
		return true;
	}

	case 2: // the second click of the double click: on the + card as it is now - rebuilt by the first click
	{
		const TSharedPtr<SWidget> Button = FindAddBotButton();
		const bool bRebuilt = Button.IsValid() && Button != ClickedAddButton.Pin();
		if (!bRebuilt && StageTime - ClickAt < 2.f)
		{
			return true; // (the cards are rebuilt a frame or two after the click)
		}
		if (Button.IsValid())
		{
			SecondClickGap = SlateNow() - ClickRealTime;
			const FPointerEvent Event = LeftMouseEvent();
			Button->OnMouseButtonDoubleClick(Button->GetTickSpaceGeometry(), Event);
			Button->OnMouseButtonUp(Button->GetTickSpaceGeometry(), Event);
		}
		Report(FString::Printf(TEXT("+ card double-clicked: the second click %.3f s after the first, on a rebuilt card %d"), SecondClickGap, bRebuilt ? 1 : 0));
		ClickAt = StageTime;
		ClickStep = 3;
		return true;
	}

	case 3:
		if (StageTime - ClickAt < 0.3f)
		{
			return true;
		}
		Check(LobbyHUD->GetBotCount() == BotsBeforeClick + 1,
			FString::Printf(TEXT("a double click on the + card adds one bot (%d -> %d, want one more; its second click %.3f s after the first, on the rebuilt card)"),
				BotsBeforeClick, LobbyHUD->GetBotCount(), SecondClickGap));
		ClickRealTime = SlateNow();
		ClickStep = 4;
		return true;

	case 4: // a + card that has been up a while (real time): a double click on it is a press (option arrows clicked fast)
	{
		if (SlateNow() - ClickRealTime < 0.9)
		{
			return true;
		}
		const TSharedPtr<SWidget> Button = FindAddBotButton();
		BotsBeforeClick = LobbyHUD->GetBotCount();
		if (Button.IsValid())
		{
			const FPointerEvent Event = LeftMouseEvent();
			Button->OnMouseButtonDoubleClick(Button->GetTickSpaceGeometry(), Event);
			Button->OnMouseButtonUp(Button->GetTickSpaceGeometry(), Event);
		}
		ClickAt = StageTime;
		ClickStep = 5;
		return true;
	}

	case 5:
		if (StageTime - ClickAt < 0.3f)
		{
			return true;
		}
		Check(LobbyHUD->GetBotCount() == BotsBeforeClick + 1,
			FString::Printf(TEXT("a double click on a + card that has been up a while still presses it (%d -> %d, want one more)"), BotsBeforeClick, LobbyHUD->GetBotCount()));
		ClickStep = ClickDone;
		return true;

	default:
		return false;
	}
}

void APinguLobbyTest::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.1f);
	StageTime += Dt;
	TotalTime += Dt;
	APenguinGameState* GameState = APenguinGameState::Get(this);
	APenguinHUD* HUD = GetHUD();
	APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr;
	if (Stage >= 100)
	{
		return;
	}
	if (TotalTime > 210.f)
	{
		Finish(*FString::Printf(TEXT("timeout in stage %d"), Stage));
		return;
	}
	if (!GameState || !HUD || !GameMode)
	{
		return;
	}
	const auto Check = [this](bool bOk, const FString& What)
	{
		++Checked;
		Passed += bOk ? 1 : 0;
		Report(FString::Printf(TEXT("RESULT %s: %s"), *What, bOk ? TEXT("ok") : TEXT("FAIL")));
	};
	const auto Next = [this]()
	{
		++Stage;
		StageTime = 0.f;
		bShotTaken = false;
	};
	const EPinguPhase Phase = GameState->GetPhase();

	switch (Stage)
	{
	case 0: // the title: PLAY (the 3D title's PLAY clicked where the mouse would; the old title: the action)
		if (Phase == EPinguPhase::Menu && StageTime > 2.f)
		{
			if (!UpdateMenu3DTitlePlay(HUD))
			{
				HUD->Action_Play();
			}
			Next();
		}
		break;

	case 1: // your own lobby: your card and the bots' (first a double click on the + card)
		if (Phase == EPinguPhase::Lobby && StageTime > 0.5f)
		{
			// The lobby island (the old card lobby with -PinguOldMenus: its + card's double click and its island sizes instead).
			const bool bIslandBusy = PinguUseOldMenus() ? (UpdateAddBotDoubleClick(HUD, Check) || UpdateLobbyIslandSizeCheck(HUD, Check))
				: UpdateLobbyIslandChecks(HUD, Check);
			if (bIslandBusy || UpdateCopyAddressCheck(HUD, Check) || UpdateMenu3DLobbyCheck(HUD, Check))
			{
				break;
			}
			const TArray<FPinguLobbyPlayer> Cards = HUD->GetLobbyCards();
			int32 People = 0;
			int32 Bots = 0;
			TSet<FString> Names;
			for (const FPinguLobbyPlayer& Card : Cards)
			{
				(Card.bBot ? Bots : People) += 1;
				Names.Add(Card.Name.ToLower());
			}
			Check(People == 1 && Bots >= 1 && Names.Num() == Cards.Num() && HUD->GetScreen() == EPinguScreen::Lobby && !HUD->IsNetworkGame(),
				FString::Printf(TEXT("PLAY opens your own lobby (%d person, %d bots, names all different %d, lobby card shows %d): %s"), People, Bots,
					Names.Num() == Cards.Num() ? 1 : 0, HUD->GetScreen() == EPinguScreen::Lobby ? 1 : 0, *DescribeCards()));
			// + twice, then X on the last one: four bots.
			for (int32 Guard = 0; Guard < 10 && HUD->GetBotCount() < 3; ++Guard)
			{
				HUD->Action_AddBot();
			}
			for (int32 Guard = 0; Guard < 10 && HUD->GetBotCount() > 3; ++Guard)
			{
				HUD->Action_RemoveBot(HUD->GetBotCount() - 1);
			}
			HUD->Action_AddBot();
			HUD->Action_AddBot();
			const int32 AfterAdd = HUD->GetBotCount();
			HUD->Action_RemoveBot(4);
			Check(AfterAdd == 5 && HUD->GetBotCount() == 4, FString::Printf(TEXT("+ adds a bot (3 -> %d), X removes one (-> %d)"), AfterAdd, HUD->GetBotCount()));
			// Skills (round and round): bot 0 easy, bot 1 hard, bot 2 normal, bot 3 hard.
			const auto SkillOf = [HUD](int32 Bot) { return HUD->GetMatchSettings().Bots.IsValidIndex(Bot) ? HUD->GetMatchSettings().Bots[Bot].Skill : -9; };
			for (int32 Bot = 0; Bot < 4; ++Bot)
			{
				for (int32 Guard = 0; Guard < 4 && SkillOf(Bot) != 1; ++Guard)
				{
					HUD->Action_ChangeBotSkill(Bot, 1);
				}
			}
			HUD->Action_ChangeBotSkill(0, -1);
			HUD->Action_ChangeBotSkill(1, 1);
			HUD->Action_ChangeBotSkill(3, 1);
			HUD->Action_ChangeBotSkill(3, 1);
			HUD->Action_ChangeBotSkill(3, -1); // normal -> hard -> easy (round and round) -> back to hard
			// Teams: you and bot 0 red, bots 1 and 2 blue (bot 2 the long way round: brown, no team, red, blue), bot 3 none.
			const auto TeamOf = [HUD](int32 Bot) { return HUD->GetMatchSettings().Bots.IsValidIndex(Bot) ? HUD->GetMatchSettings().Bots[Bot].Team : -9; };
			for (int32 Bot = 0; Bot < 4; ++Bot)
			{
				for (int32 Guard = 0; Guard < 12 && TeamOf(Bot) != -1; ++Guard)
				{
					HUD->Action_ChangeBotTeam(Bot, 1);
				}
			}
			for (int32 Guard = 0; Guard < 12 && HUD->GetLocalTeamChoice() != -1; ++Guard)
			{
				HUD->Action_ChangeTeamChoice(1, true);
			}
			HUD->Action_ChangeTeamChoice(1, true);
			HUD->Action_ChangeBotTeam(0, 1);
			HUD->Action_ChangeBotTeam(1, 1);
			HUD->Action_ChangeBotTeam(1, 1);
			HUD->Action_ChangeBotTeam(2, -1);
			const int32 Brown = TeamOf(2);
			HUD->Action_ChangeBotTeam(2, 1);
			const int32 NoneAgain = TeamOf(2);
			HUD->Action_ChangeBotTeam(2, 1);
			HUD->Action_ChangeBotTeam(2, 1);
			Check(Brown == PinguMaxTeams - 1 && NoneAgain == -1, FString::Printf(TEXT("a card's team goes round and round (no team, back: %s; on: %s)"),
				*APenguinCharacter::GetTeamName(Brown).ToString(), NoneAgain < 0 ? TEXT("no team") : *APenguinCharacter::GetTeamName(NoneAgain).ToString()));
			const bool bCards = HUD->GetLocalTeamChoice() == 0 && TeamOf(0) == 0 && TeamOf(1) == 1 && TeamOf(2) == 1 && TeamOf(3) == -1
				&& SkillOf(0) == 0 && SkillOf(1) == 2 && SkillOf(2) == 1 && SkillOf(3) == 2;
			Check(bCards, FString::Printf(TEXT("the cards as set: %s"), *DescribeCards()));
			CardsAtStart = DescribeCards();
			Next();
		}
		else if (StageTime > 15.f)
		{
			Finish(TEXT("PLAY did not open the lobby"));
		}
		break;

	case 2: // a look at the cards, then START
		if (!bShotTaken && StageTime > 0.6f)
		{
			Shot(TEXT("lobbytest_cards"));
			bShotTaken = true;
		}
		if (StageTime > 1.2f)
		{
			// The lobby island: START with keys only (Esc, the arrows, Enter); the card lobby: its action.
			if (PinguMenu3D::UseIslandLobby())
			{
				if (UpdateLobbyStripKeys(HUD, Check))
				{
					break;
				}
			}
			else
			{
				HUD->Action_StartLobbyMatch();
			}
			Next();
		}
		break;

	case 3: // the match is set up: teams from the cards, side by side, each bot at its card's skill
		if ((Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown) && StageTime > 0.2f)
		{
			TArray<APenguinCharacter*> All;
			for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
			{
				All.Add(*It);
			}
			const APenguinCharacter* You = Cast<APenguinCharacter>(GetWorld()->GetFirstPlayerController()->GetPawn());
			RedTeam = You ? You->GetTeam() : -1;
			const FPinguMatchSettings Settings = GameState->GetSettings();
			const auto BotNamed = [&All](const FString& Name) -> const APenguinCharacter*
			{
				for (const APenguinCharacter* Penguin : All)
				{
					if (!Penguin->GetPlayerState() && Penguin->GetDisplayName() == Name)
					{
						return Penguin;
					}
				}
				return nullptr;
			};
			const APenguinCharacter* Bot0 = Settings.Bots.Num() > 0 ? BotNamed(Settings.Bots[0].Name) : nullptr;
			const APenguinCharacter* Bot1 = Settings.Bots.Num() > 1 ? BotNamed(Settings.Bots[1].Name) : nullptr;
			const APenguinCharacter* Bot2 = Settings.Bots.Num() > 2 ? BotNamed(Settings.Bots[2].Name) : nullptr;
			const APenguinCharacter* Bot3 = Settings.Bots.Num() > 3 ? BotNamed(Settings.Bots[3].Name) : nullptr;
			const bool bFound = You && Bot0 && Bot1 && Bot2 && Bot3;
			const bool bTeams = bFound && GameState->IsTeamMatch() && RedTeam == 0 && Bot0->GetTeam() == 0 && Bot1->GetTeam() == 1 && Bot2->GetTeam() == 1
				&& Bot3->GetTeam() >= 0 && Bot3->GetTeam() != 0 && Bot3->GetTeam() != 1;
			Check(bTeams, FString::Printf(TEXT("teams from the cards: you %d, %s %d, %s %d, %s %d, %s %d (want 0 0 1 1 and the last on its own), team match %d"),
				You ? You->GetTeam() : -9, Bot0 ? *Bot0->GetDisplayName() : TEXT("?"), Bot0 ? Bot0->GetTeam() : -9, Bot1 ? *Bot1->GetDisplayName() : TEXT("?"),
				Bot1 ? Bot1->GetTeam() : -9, Bot2 ? *Bot2->GetDisplayName() : TEXT("?"), Bot2 ? Bot2->GetTeam() : -9, Bot3 ? *Bot3->GetDisplayName() : TEXT("?"),
				Bot3 ? Bot3->GetTeam() : -9, GameState->IsTeamMatch() ? 1 : 0));
			// Each bot plays at its card's skill.
			const auto SkillOf = [](const APenguinCharacter* Penguin) { const UPenguinBotComponent* Brain = Penguin ? Penguin->GetBot() : nullptr; return Brain && Brain->IsBotEnabled() ? Brain->GetDifficulty() : -1; };
			Check(SkillOf(Bot0) == 0 && SkillOf(Bot1) == 2 && SkillOf(Bot2) == 1 && SkillOf(Bot3) == 2,
				FString::Printf(TEXT("each bot at its card's skill: %d %d %d %d (want 0 2 1 2)"), SkillOf(Bot0), SkillOf(Bot1), SkillOf(Bot2), SkillOf(Bot3)));
			// Teammates side by side: along the island, each team's penguins one run of neighbours.
			TArray<const APenguinCharacter*> ByX(All);
			ByX.Sort([](const APenguinCharacter& A, const APenguinCharacter& B) { return A.GetActorLocation().X < B.GetActorLocation().X; });
			TArray<int32> Runs;
			FString Order;
			for (const APenguinCharacter* Penguin : ByX)
			{
				if (Runs.Num() == 0 || Runs.Last() != Penguin->GetTeam())
				{
					Runs.Add(Penguin->GetTeam());
				}
				Order += FString::Printf(TEXT("%s%s (%d, x %.0f)"), Order.IsEmpty() ? TEXT("") : TEXT(", "), *Penguin->GetDisplayName(), Penguin->GetTeam(), Penguin->GetActorLocation().X);
			}
			TSet<int32> Distinct(Runs);
			Check(Distinct.Num() == Runs.Num(), FString::Printf(TEXT("teammates start side by side (%d teams, %d runs): %s"), Distinct.Num(), Runs.Num(), *Order));
			// The table's rings: the two teams' penguins have one, the one on its own none.
			int32 Rings = 0;
			int32 NoRing = 0;
			for (const FPinguScore& Score : GameState->GetScores())
			{
				(HUD->GetScoreTeamRing(Score) >= 0 ? Rings : NoRing) += 1;
			}
			Check(Rings == 4 && NoRing == 1, FString::Printf(TEXT("team rings on the boards: %d penguins with one, %d without (want 4, 1)"), Rings, NoRing));
			// The bot that picked no team: inside the match a team of its own, but shown on its own everywhere - no team (no
			// ring, no team name), its own row on the boards; its icons above the head get no ring, the others' do.
			LoneName = Bot3 ? Bot3->GetDisplayName() : FString();
			LoneTeam = Bot3 ? Bot3->GetTeam() : -1;
			const FPinguScore* LoneRow = GameState->GetScores().FindByPredicate([Bot3](const FPinguScore& Entry) { return Bot3 && Entry.Penguin.Get() == Bot3; });
			const int32 LoneShown = GameState->GetShownTeam(LoneTeam);
			const int32 LoneRing = LoneRow ? HUD->GetScoreTeamRing(*LoneRow) : -9;
			const bool bLoneRow = LoneRow && HUD->GetLoneTeamMember(LoneTeam) == LoneRow;
			const int32 LoneIconRing = HUD->GetPenguinTeamRing(Bot3);
			const bool bOthersRinged = bFound && HUD->GetPenguinTeamRing(You) == RedTeam && HUD->GetPenguinTeamRing(Bot0) == RedTeam
				&& HUD->GetPenguinTeamRing(Bot1) == 1 && HUD->GetPenguinTeamRing(Bot2) == 1;
			Check(bFound && LoneTeam >= 0 && LoneShown < 0 && LoneRing < 0 && bLoneRow && LoneIconRing < 0 && bOthersRinged,
				FString::Printf(TEXT("%s (no team) shows on its own: team %d inside the match, shown as team %d, board ring %d, its own row %d, icon ring %d (want -1 -1 1 -1); the teammates' icon rings %d"),
					*LoneName, LoneTeam, LoneShown, LoneRing, bLoneRow ? 1 : 0, LoneIconRing, bOthersRinged ? 1 : 0));
			Next();
		}
		else if (StageTime > 10.f)
		{
			Finish(TEXT("START did not start the match"));
		}
		break;

	case 4: // round 1: the bots stand still - the team rings above the heads (a small one around each icon of the four in
		// teams, none for the lone bot) - then the blues and the lone one go into the sea
		if (Phase == EPinguPhase::Fight && StageTime > 0.3f && !bRingsChecked)
		{
			for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
			{
				if (UPenguinBotComponent* Brain = It->GetBot())
				{
					Brain->SetBotEnabled(false);
				}
			}
			bRingsChecked = true;
			TArray<FString> Ringed;
			int32 IconRings = 0;
			float RingToIcon = 0.f;
			float Clearance = 0.f;
			const bool bDrawn = HUD->GetOverheadTeamRings(Ringed, IconRings, RingToIcon, Clearance);
			// (Each ring just outside its own icon: about 1.2 times its diameter - the old one round all three was 2.7 times.)
			Check(bDrawn && Ringed.Num() == 4 && IconRings == 12 && !Ringed.Contains(LoneName) && RingToIcon > 1.f && RingToIcon < 1.5f && Clearance >= 0.f,
				FString::Printf(TEXT("team rings above the heads: one around each icon - %d penguins (%s), %d rings (want 4 and 12, none for %s), each %.2f times its icon (want under 1.5), %.1f between an icon and its ring (want 0 or more), drawn %d"),
					Ringed.Num(), *FString::Join(Ringed, TEXT(", ")), IconRings, *LoneName, RingToIcon, Clearance, bDrawn ? 1 : 0));
			Shot(TEXT("lobbytest_team_rings"));
		}
		if (Phase == EPinguPhase::Fight && bRingsChecked && StageTime > 0.9f)
		{
			KnockOutAllBut(RedTeam);
			Next();
		}
		break;

	case 5: // the leaderboard waits for a key: one pressed too early does nothing, it is still up after 6 s, a key goes on
		if (Phase == EPinguPhase::RoundOver)
		{
			const float PhaseTime = GameState->GetPhaseTime();
			if (!bEarlyKeySent && PhaseTime > 0.6f)
			{
				bEarlyKeySent = true;
				APenguinInputTest::SendKeyEvent(EKeys::SpaceBar, true);
				APenguinInputTest::SendKeyEvent(EKeys::SpaceBar, false);
				Report(TEXT("Space pressed 0.6 s into the leaderboard (too early)"));
			}
			if (!bShotTaken && PhaseTime > 2.4f)
			{
				Shot(TEXT("lobbytest_board"));
				bShotTaken = true;
			}
			if (!bWaitChecked && PhaseTime > 6.f)
			{
				bWaitChecked = true;
				Check(GameState->IsWaitingForContinue() && HUD->IsWaitingForContinueKey() && GameState->GetRoundNumber() == 1,
					FString::Printf(TEXT("the leaderboard waits for a key: still up after %.1f s (it went on by itself after 5.2 s), round %d, red wins %d"), PhaseTime,
						GameState->GetRoundNumber(), GameState->GetTeamWinCount(RedTeam)));
				APenguinInputTest::SendKeyEvent(EKeys::K, true);
				APenguinInputTest::SendKeyEvent(EKeys::K, false);
				KeySentAt = TotalTime;
				bKeySent = true;
				Report(TEXT("K pressed"));
			}
		}
		else if (bKeySent && (Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown))
		{
			Check(GameState->GetRoundNumber() == 2 && TotalTime - KeySentAt < 1.f,
				FString::Printf(TEXT("a key goes on: round %d %.2f s after it"), GameState->GetRoundNumber(), TotalTime - KeySentAt));
			Next();
		}
		else if (bEarlyKeySent && !bWaitChecked && Phase != EPinguPhase::RoundOver)
		{
			Check(false, TEXT("the early key (or the clock) ended the leaderboard"));
			Next();
		}
		break;

	case 6: // round 2: a blue bot knocked flying off the side - the camera stays on the island; then red wins again
	{
		const APenguinCamera* StageCamera = APenguinCamera::FindOrSpawn(GetWorld());
		if (Phase == EPinguPhase::Fight && StageTime > 0.3f && !bCameraHitDone && StageCamera)
		{
			bCameraHitDone = true;
			for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
			{
				if (UPenguinBotComponent* Brain = It->GetBot())
				{
					Brain->SetBotEnabled(false);
				}
			}
			// The blue bot nearest an end of the island, hit hard toward that end (out over the sea, not out of the round).
			APenguinCharacter* Flyer = nullptr;
			const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
			const FBox Bounds = Island ? Island->GetBounds() : FBox(ForceInit);
			const double MidX = Bounds.IsValid ? Bounds.GetCenter().X : 0.0;
			for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
			{
				if (It->IsAlive() && It->GetTeam() != RedTeam && (!Flyer || FMath::Abs(It->GetActorLocation().X - MidX) > FMath::Abs(Flyer->GetActorLocation().X - MidX)))
				{
					Flyer = *It;
				}
			}
			CameraStart = StageCamera->GetActorLocation();
			CameraHitAt = StageTime;
			CameraMaxMove = 0.f;
			if (Flyer)
			{
				FlyerStartX = static_cast<float>(Flyer->GetActorLocation().X);
				FlyerWay = Flyer->GetActorLocation().X >= MidX ? 1.f : -1.f;
				// Up off the ground first, above whatever stands at the island's end (some rounds' islands have a raised block
				// right beside it: hit into that it only flew a metre), so it flies out over the sea on any island.
				Flyer->NetTeleport(Flyer->GetActorLocation() + FVector(0.f, 0.f, 320.f), FVector::ZeroVector, true);
				FPenguinHit Hit;
				Hit.Instigator = Cast<APenguinCharacter>(GetWorld()->GetFirstPlayerController()->GetPawn());
				Hit.Source = EPenguinAbility::Snowball;
				Hit.Knockback = FVector2D(2600.f * FlyerWay, 700.f);
				Hit.Stun = 0.45f;
				Hit.Location = Flyer->GetActorLocation() - FVector(60.f * FlyerWay, 0.f, 0.f);
				Flyer->ApplyPenguinHit(Hit);
				CameraFlyer = Flyer;
			}
			Report(FString::Printf(TEXT("camera check: %s knocked flying %s"), Flyer ? *Flyer->GetDisplayName() : TEXT("nobody"), FlyerWay > 0.f ? TEXT("right") : TEXT("left")));
		}
		if (bCameraHitDone && StageCamera && StageTime - CameraHitAt < 1.5f)
		{
			CameraMaxMove = FMath::Max(CameraMaxMove, static_cast<float>(FVector::Dist(StageCamera->GetActorLocation(), CameraStart)));
			if (const APenguinCharacter* Flyer = CameraFlyer.Get())
			{
				FlyerMaxOut = FMath::Max(FlyerMaxOut, (static_cast<float>(Flyer->GetActorLocation().X) - FlyerStartX) * FlyerWay);
			}
			if (!bShotTaken && StageTime - CameraHitAt > 0.6f)
			{
				Shot(TEXT("lobbytest_camera_flung"));
				bShotTaken = true;
			}
		}
		if (bCameraHitDone && StageTime - CameraHitAt >= 1.5f)
		{
			// (The hit shakes the camera a little: up to about 70 cm. Following the flyer moved it metres.)
			Check(FlyerMaxOut > 600.f && CameraMaxMove < 150.f,
				FString::Printf(TEXT("a penguin knocked flying (%.0f cm out) leaves the camera where it is: it moved at most %.0f cm (want under 150, shake only)"),
					FlyerMaxOut, CameraMaxMove));
			KnockOutAllBut(RedTeam);
			Next();
		}
		break;
	}

	case 7: // endless: red's second win ends nothing; round 3: the bot that picked no team wins it (the banner names it, no
		// team); round 4 starts, the island starts crumbling (sudden death) and in the middle of that fight the pause menu's
		// END MATCH ends the match (red has the most wins; round 4 does not count) - the crumbling stops, and a knock-out
		// after that counts nothing
	{
		const auto NextSub = [this]() { ++SubStep; StageTime = 0.f; };
		const float PhaseTime = GameState->GetPhaseTime();
		ABlockIsland* Island = ABlockIsland::Get(GetWorld());
		const auto SumScores = [GameState](int32& OutKOs, int32& OutFalls, int32& OutWins)
		{
			OutKOs = OutFalls = OutWins = 0;
			for (const FPinguScore& Score : GameState->GetScores())
			{
				OutKOs += Score.KOs;
				OutFalls += Score.Falls;
				OutWins += Score.Wins;
			}
		};
		switch (SubStep)
		{
		case 0:
			if (Phase == EPinguPhase::RoundOver && PhaseTime > APenguinGameState::ContinueAfter + 0.6f)
			{
				Check(GameState->IsEndless() && GameState->GetTeamWinCount(RedTeam) == 2 && GameState->GetMatchWinnerTeam() < 0 && HUD->IsWaitingForContinueKey(),
					FString::Printf(TEXT("endless: after round 2 red has %d wins, match winner %d (want none), waits for a key %d, endless %d"),
						GameState->GetTeamWinCount(RedTeam), GameState->GetMatchWinnerTeam(), HUD->IsWaitingForContinueKey() ? 1 : 0, GameState->IsEndless() ? 1 : 0));
				Shot(TEXT("lobbytest_board_endless"));
				NextSub();
			}
			break;
		case 1: // a key: round 3
			if (StageTime > 0.3f)
			{
				APenguinInputTest::SendKeyEvent(EKeys::K, true);
				APenguinInputTest::SendKeyEvent(EKeys::K, false);
				Report(TEXT("K pressed: round 3"));
				NextSub();
			}
			break;
		case 2: // round 3's fight: everybody holds still, all but the lone bot go into the sea
			if (Phase == EPinguPhase::Fight && StageTime > 0.3f)
			{
				for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
				{
					if (UPenguinBotComponent* Brain = It->GetBot())
					{
						Brain->SetBotEnabled(false);
					}
				}
				KnockOutAllBut(LoneTeam);
				Report(FString::Printf(TEXT("round %d: everybody but %s into the sea"), GameState->GetRoundNumber(), *LoneName));
				NextSub();
			}
			else if (StageTime > 15.f)
			{
				Report(TEXT("round 3 did not start"));
				NextSub();
			}
			break;
		case 3: // the lone bot won the round: by its own name, no team anywhere
			if (Phase == EPinguPhase::RoundOver && GameState->GetPhaseTime() > APenguinGameState::ContinueAfter + 0.6f)
			{
				const FString Banner = HUD->GetBannerText().ToString();
				const FString Want = FString::Printf(TEXT("%s WINS!"), *LoneName.ToUpper());
				const FPinguScore* LoneRow = HUD->GetLoneTeamMember(LoneTeam);
				Check(GameState->GetRoundWinnerTeam() == LoneTeam && LoneTeam >= 0 && Banner == Want && !Banner.Contains(TEXT("TEAM")) && LoneRow && LoneRow->Name == LoneName
					&& GameState->GetTeamWinCount(LoneTeam) == 1 && LoneRow->Wins == 1 && GameState->GetTeamWinCount(RedTeam) == 2,
					FString::Printf(TEXT("%s (no team) wins round 3 on its own: banner \"%s\" (want \"%s\"), round winner team %d (its own %d), its row on the board %s with %d win(s), red %d"),
						*LoneName, *Banner, *Want, GameState->GetRoundWinnerTeam(), LoneTeam, LoneRow ? *LoneRow->Name : TEXT("none"), LoneRow ? LoneRow->Wins : -1,
						GameState->GetTeamWinCount(RedTeam)));
				Shot(TEXT("lobbytest_board_lone"));
				NextSub();
			}
			break;
		case 4: // a key: round 4
			if (StageTime > 0.6f)
			{
				APenguinInputTest::SendKeyEvent(EKeys::K, true);
				APenguinInputTest::SendKeyEvent(EKeys::K, false);
				Report(TEXT("K pressed: round 4"));
				NextSub();
			}
			break;
		case 5: // round 4's fight: everybody holds still, the island starts crumbling
			if (Phase == EPinguPhase::Fight && StageTime > 0.3f)
			{
				for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
				{
					if (UPenguinBotComponent* Brain = It->GetBot())
					{
						Brain->SetBotEnabled(false);
					}
				}
				if (Island)
				{
					Island->StartCollapse(0.8f, 1.2f);
				}
				Report(FString::Printf(TEXT("round %d: the island crumbles (%d)"), GameState->GetRoundNumber(), Island && Island->IsCollapsing() ? 1 : 0));
				NextSub();
			}
			else if (StageTime > 15.f)
			{
				Report(TEXT("round 4 did not start: END MATCH from where it is"));
				NextSub();
			}
			break;
		case 6:
			if (StageTime > 0.8f)
			{
				bCollapseBeforeEnd = Island && Island->IsCollapsing();
				PhaseAtEnd = static_cast<uint8>(Phase);
				HUD->TogglePauseMenu();
				NextSub();
			}
			break;
		case 7:
			if (StageTime > 0.4f)
			{
				Check(HUD->GetScreen() == EPinguScreen::Pause && HUD->CanEndMatch() && GameMode->IsGamePaused(),
					FString::Printf(TEXT("the pause menu in a match has END MATCH: pause screen %d, END MATCH %d, paused %d"),
						HUD->GetScreen() == EPinguScreen::Pause ? 1 : 0, HUD->CanEndMatch() ? 1 : 0, GameMode->IsGamePaused() ? 1 : 0));
				Shot(TEXT("lobbytest_pause"));
				NextSub();
			}
			break;
		case 8:
			if (StageTime > 0.4f)
			{
				HUD->Action_EndMatch();
				BlocksAtEnd = Island ? Island->GetNumBlocks() : -1;
				NextSub();
			}
			break;
		case 9:
			if (StageTime > 0.7f)
			{
				// (Round 4 was never finished: the results count 3 rounds - red 2 wins, the lone bot 1.)
				Check(Phase == EPinguPhase::MatchOver && GameState->GetMatchWinnerTeam() == RedTeam && HUD->GetScreen() == EPinguScreen::Results && !GameMode->IsGamePaused()
					&& GameState->GetRoundNumber() == 3,
					FString::Printf(TEXT("END MATCH: results after %d rounds (phase %s), winner team %d (want red %d: the most wins), results screen %d, paused %d"),
						GameState->GetRoundNumber(), *UEnum::GetValueAsString(Phase), GameState->GetMatchWinnerTeam(), RedTeam,
						HUD->GetScreen() == EPinguScreen::Results ? 1 : 0, GameMode->IsGamePaused() ? 1 : 0));
				const int32 BlocksNow = Island ? Island->GetNumBlocks() : -1;
				Check(PhaseAtEnd == static_cast<uint8>(EPinguPhase::Fight) && bCollapseBeforeEnd && Island && !Island->IsCollapsing() && BlocksNow == BlocksAtEnd,
					FString::Printf(TEXT("END MATCH in the middle of a fight stops the island's crumbling: ended from %s, crumbling before %d, after %d, blocks %d -> %d 0.7 s later (want the same)"),
						*UEnum::GetValueAsString(static_cast<EPinguPhase>(PhaseAtEnd)), bCollapseBeforeEnd ? 1 : 0, Island && Island->IsCollapsing() ? 1 : 0, BlocksAtEnd, BlocksNow));
				// A knock-out after the match ended (one of the others, by you): nothing counts any more.
				SumScores(KOsBefore, FallsBefore, WinsBefore);
				APenguinCharacter* You = Cast<APenguinCharacter>(GetWorld()->GetFirstPlayerController()->GetPawn());
				for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
				{
					if (It->IsAlive() && !It->IsHidden() && It->GetTeam() != RedTeam)
					{
						LateVictim = *It;
						It->KnockOut(EPenguinKOReason::Sea, You);
						break;
					}
				}
				NextSub();
			}
			break;
		case 10:
			if (StageTime > 0.5f)
			{
				int32 KOsNow = 0;
				int32 FallsNow = 0;
				int32 WinsNow = 0;
				SumScores(KOsNow, FallsNow, WinsNow);
				const bool bOut = LateVictim.IsValid() && LateVictim->IsKnockedOut();
				Check(bOut && KOsNow == KOsBefore && FallsNow == FallsBefore && WinsNow == WinsBefore && Phase == EPinguPhase::MatchOver && GameState->GetMatchWinnerTeam() == RedTeam,
					FString::Printf(TEXT("a knock-out after the match ended counts nothing: knocked out %d, knock-outs %d -> %d, falls %d -> %d, wins %d -> %d, still the results %d, winner %d"),
						bOut ? 1 : 0, KOsBefore, KOsNow, FallsBefore, FallsNow, WinsBefore, WinsNow, Phase == EPinguPhase::MatchOver ? 1 : 0, GameState->GetMatchWinnerTeam()));
				Next();
			}
			break;
		default:
			break;
		}
		break;
	}

	case 8: // a look at the results, then LOBBY: the same cards
		if (!bShotTaken && StageTime > 1.f)
		{
			// The results' title, had the lone bot won the match: its own name, no team (the winner set for a moment and put
			// back in the same frame - nothing is drawn in between).
			const FString RedTitle = HUD->GetResultsTitle().ToString();
			const int32 RealWinner = GameState->GetMatchWinnerTeam();
			GameState->SetTeamWinners(GameState->GetRoundWinnerTeam(), LoneTeam);
			const FString LoneTitle = HUD->GetResultsTitle().ToString();
			GameState->SetTeamWinners(GameState->GetRoundWinnerTeam(), RealWinner);
			const FString Want = FString::Printf(TEXT("%s WINS!"), *LoneName.ToUpper());
			Check(RedTitle == TEXT("VICTORY!") && LoneTitle == Want && !LoneTitle.Contains(TEXT("TEAM")),
				FString::Printf(TEXT("the results' title: \"%s\" (red, your team, won: want VICTORY!); had %s (no team) won: \"%s\" (want \"%s\")"),
					*RedTitle, *LoneName, *LoneTitle, *Want));
			Shot(TEXT("lobbytest_results"));
			bShotTaken = true;
		}
		if (StageTime > 1.6f && Phase == EPinguPhase::MatchOver)
		{
			HUD->Action_ReturnToLobby();
		}
		if (Phase == EPinguPhase::Lobby && StageTime > 2.f)
		{
			const FString Cards = DescribeCards();
			Check(Cards == CardsAtStart && HUD->GetScreen() == EPinguScreen::Lobby, FString::Printf(TEXT("LOBBY after the match: the same cards (%s)"), *Cards));
			// Back in the lobby nobody has a team any more (a card's team is for the next match); the no-team card stays NO TEAM.
			int32 WithTeam = 0;
			int32 WithRing = 0;
			for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
			{
				WithTeam += It->GetTeam() >= 0 ? 1 : 0;
				WithRing += HUD->GetPenguinTeamRing(*It) >= 0 ? 1 : 0;
			}
			int32 LoneCard = -9;
			for (const FPinguLobbyPlayer& Card : HUD->GetLobbyCards())
			{
				LoneCard = Card.Name == LoneName ? Card.TeamChoice : LoneCard;
			}
			Check(WithTeam == 0 && WithRing == 0 && LoneCard == -1 && !GameState->IsTeamMatch(),
				FString::Printf(TEXT("the lobby after a team match: %d penguins with a team, %d with a ring (want 0 0), %s's card team %d (want -1: NO TEAM), team match %d"),
					WithTeam, WithRing, *LoneName, LoneCard, GameState->IsTeamMatch() ? 1 : 0));
			HUD->Action_MainMenu();
			Next();
		}
		break;

	case 9: // BACK: the title
		if (Phase == EPinguPhase::Menu && StageTime > 1.f)
		{
			Check(HUD->GetScreen() == EPinguScreen::MainMenu, TEXT("BACK from the lobby: the title"));
			HUD->Action_Play();
			Next();
		}
		break;

	case 10: // PLAY again: a second match straight from the lobby (first ABILITIES: CHOOSE and two picks: PenguinAutoTestLobbyPicks.cpp)
		if (Phase == EPinguPhase::Lobby && StageTime > 0.5f)
		{
			if (UpdatePicksStage(HUD, Check))
			{
				HUD->Action_StartLobbyMatch();
				Next();
			}
		}
		else if (StageTime > 10.f)
		{
			Report(TEXT("no lobby after the second PLAY"));
			Next();
		}
		break;

	case 11: // its fight: the pause menu
		if (Phase == EPinguPhase::Fight && GameState->GetPhaseTime() > 0.3f)
		{
			CheckPicksDealt(HUD, Check);
			HUD->TogglePauseMenu();
			Next();
		}
		else if (StageTime > 15.f)
		{
			Report(TEXT("the second match's fight did not start"));
			Next();
		}
		break;

	case 12: // the pause menu's MAIN MENU (its bottom button in a solo game; it used to be QUIT)
		if (StageTime > 0.4f)
		{
			bPauseBeforeMainMenu = HUD->GetScreen() == EPinguScreen::Pause && GameMode->IsGamePaused() && Phase == EPinguPhase::Fight;
			HUD->Action_MainMenu();
			Next();
		}
		break;

	case 13: // the title again, the game not paused and still running
		if (StageTime > 1.f)
		{
			Check(bPauseBeforeMainMenu && Phase == EPinguPhase::Menu && HUD->GetScreen() == EPinguScreen::MainMenu && !GameMode->IsGamePaused(),
				FString::Printf(TEXT("the pause menu's MAIN MENU in a fight: paused with the pause screen before %d, then phase %s (want Menu), title screen %d, paused %d (want 0)"),
					bPauseBeforeMainMenu ? 1 : 0, *UEnum::GetValueAsString(Phase), HUD->GetScreen() == EPinguScreen::MainMenu ? 1 : 0, GameMode->IsGamePaused() ? 1 : 0));
			Next(); // Traits mode
		}
		break;

	case 14: // Traits mode (PenguinAutoTestTraits.cpp has the rest of it)
		UpdateTraitsStage(GameState, HUD, GameMode, Check);
		break;

	default:
		break;
	}
}
