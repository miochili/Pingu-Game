// Pingu - the autotest's steps for the shared traits (2026-11-07, card shared-traits: every penguin has the round's one trait).
// APenguinAutoTest::AddSharedTraitSteps, called from AddTraitSteps before its summary. Each step gives the trait to both our
// penguin and the dummy (everybody has it), measures its number against the same thing without it in the same step, logs
// RESULT trait_<Name> ... ok / FAIL and takes it away again. Pictures (with the UI): 110_trait_logo (+ _second_chance, _corner_flight, _corner), 111_trait_damage_percent,
// 112_trait_deadly_edges, 113_trait_ice_rink.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PinguTraitTable.h"
#include "PinguUI.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

namespace PenguinAutoTestSharedTraitsPrivate
{
	/** Everybody (our penguin and the dummy) gets Trait at once (no reveal); None takes it away. */
	void GiveBoth(APenguinCharacter* A, APenguinCharacter* B, EPinguTrait Trait)
	{
		for (APenguinCharacter* Who : { A, B })
		{
			if (UPenguinTraitComponent* Traits = Who ? Who->GetTraits() : nullptr)
			{
				Traits->SetTrait(Trait, -1.f);
			}
		}
	}

	/** The round trait as a match would have it (the game state's; Deadly Edges: these sides). */
	void SetRoundTrait(const UObject* Context, EPinguTrait Trait, const FVector2D& Edges = FVector2D::ZeroVector)
	{
		if (APenguinGameState* GameState = Context && Context->GetWorld() ? Context->GetWorld()->GetGameState<APenguinGameState>() : nullptr)
		{
			GameState->SetRoundTrait(Trait, Edges);
		}
	}

	/** A plain snowball-like push (1000 along, 300 up) from By on Victim standing at Where: the sideways speed it got. */
	float Push(APenguinCharacter* By, APenguinCharacter* Victim, const FVector& Where)
	{
		if (!Victim)
		{
			return 0.f;
		}
		Victim->NetTeleport(Where, FVector::ZeroVector, false);
		FPenguinHit Hit;
		Hit.Instigator = By;
		Hit.Source = EPenguinAbility::Snowball;
		Hit.Knockback = FVector2D(1000.f, 300.f);
		Hit.Stun = 0.3f;
		Hit.Location = Victim->GetActorLocation();
		Victim->ApplyPenguinHit(Hit);
		const UPenguinMovementComponent* VictimMove = Victim->GetPenguinMovement();
		return VictimMove ? static_cast<float>(VictimMove->Velocity.X) : 0.f;
	}

	float Ratio(float Top, float Bottom)
	{
		return FMath::Abs(Bottom) > 1.f ? Top / Bottom : 0.f;
	}

	bool Near(float Have, float Want, float Tolerance)
	{
		return FMath::Abs(Have - Want) <= Tolerance;
	}

	bool BothHave(const APenguinCharacter* A, const APenguinCharacter* B, EPinguTrait Trait)
	{
		return A && B && A->GetTraits() && B->GetTraits() && A->GetTraits()->HasTrait(Trait) && B->GetTraits()->HasTrait(Trait)
			&& A->GetTraits()->IsTransformed() && B->GetTraits()->IsTransformed();
	}
}

void APenguinAutoTest::AddSharedTraitSteps()
{
	using namespace PenguinAutoTestSharedTraitsPrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	const auto Judge = [this](const TCHAR* Name, bool bOk, const FString& Detail)
	{
		Report(FString::Printf(TEXT("RESULT trait_%s: %s: %s"), Name, *Detail, bOk ? TEXT("ok") : TEXT("FAIL")));
	};
	const auto Start = [this](float Offset, EPinguTrait Trait)
	{
		SpawnTarget();
		PrepareBrawl(Offset, {});
		GiveBoth(Penguin.Get(), Target.Get(), Trait);
	};
	const auto Finish = [this]()
	{
		GiveBoth(Penguin.Get(), Target.Get(), EPinguTrait::None);
		SetRoundTrait(this, EPinguTrait::None);
		VirtualInput = FVector2D::ZeroVector;
		if (UPenguinMovementComponent* M = Move.Get())
		{
			M->JumpReleased();
		}
	};
	// The stage camera held still on a spot for a picture, and let go again.
	const auto HoldStage = [this](const FVector& LookAt, float Distance)
	{
		if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
		{
			Stage->SetActorTickEnabled(false);
			LookCamera = Stage;
			Stage->SetActorLocationAndRotation(LookAt + FVector(0.f, Distance, 0.f), FRotator(0.f, -90.f, 0.f));
		}
	};
	const auto ReleaseStage = [this]()
	{
		if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
		{
			Stage->SetActorTickEnabled(true);
			Stage->SnapToTargets();
		}
		LookCamera.Reset();
	};

	// ---- Pinball / Fast Growth: the fight's knockback (APenguinGameState::GetKnockbackScale's rule) x2 from the start / growing
	// +6 % a second; both penguins have it.
	AddStep(TEXT("trait_PinballGrowth"), 0.1f,
		[this, Start, Judge, Finish]
		{
			Start(900.f, EPinguTrait::Pinball);
			const bool bBoth = BothHave(Penguin.Get(), Target.Get(), EPinguTrait::Pinball);
			const float Normal0 = APenguinGameState::GetKnockbackScaleAt(EPinguTrait::None, 0.f);
			const float Normal10 = APenguinGameState::GetKnockbackScaleAt(EPinguTrait::None, 10.f);
			const float Pinball0 = APenguinGameState::GetKnockbackScaleAt(EPinguTrait::Pinball, 0.f);
			const float Pinball10 = APenguinGameState::GetKnockbackScaleAt(EPinguTrait::Pinball, 10.f);
			const float Fast10 = APenguinGameState::GetKnockbackScaleAt(EPinguTrait::FastGrowth, 10.f);
			const bool bPinball = bBoth && Near(Pinball0, 2.f, 0.01f) && Near(Pinball10, 2.f * Normal10, 0.01f) && Near(Normal0, 1.f, 0.01f);
			Judge(TEXT("Pinball"), bPinball, FString::Printf(TEXT("knockback x%.2f at the fight's start, x%.2f after 10 s (want x2.00, x%.2f), both have it %d"),
				Pinball0, Pinball10, 2.f * Normal10, bBoth ? 1 : 0));
			GiveBoth(Penguin.Get(), Target.Get(), EPinguTrait::FastGrowth);
			const bool bBothFast = BothHave(Penguin.Get(), Target.Get(), EPinguTrait::FastGrowth);
			Judge(TEXT("FastGrowth"), bBothFast && Near(Fast10, 1.6f, 0.01f) && Near(Normal10, 1.2f, 0.01f),
				FString::Printf(TEXT("knockback after 10 s x%.2f (normal x%.2f; want x1.60 and x1.20), both have it %d"), Fast10, Normal10, bBothFast ? 1 : 0));
			Finish();
		},
		nullptr, nullptr);

	// ---- Damage %: every hit taken makes the next ones send it 12 % further (and the number shows under the penguin).
	{
		struct FDamage
		{
			float Pushes[3] = { 0.f, 0.f, 0.f };
			int32 DummyDamage = 0;
			int32 OurDamage = 0;
			int32 Labels = 0;
			int32 MaxShown = -1;
		};
		const TSharedRef<FDamage> L = MakeShared<FDamage>();
		AddStep(TEXT("trait_DamagePercent"), 1.2f,
			[this, L, Start, Base]
			{
				*L = FDamage();
				Start(600.f, EPinguTrait::DamagePercent);
				const FVector Spot = Base + FVector(600.f, 0.f, 0.f);
				for (int32 Hit = 0; Hit < 3; ++Hit)
				{
					L->Pushes[Hit] = Push(Penguin.Get(), Target.Get(), Spot);
				}
				ResetTarget(Spot);
				L->DummyDamage = Target.IsValid() && Target->GetTraits() ? Target->GetTraits()->GetDamagePercent() : -1;
				L->OurDamage = Penguin.IsValid() && Penguin->GetTraits() ? Penguin->GetTraits()->GetDamagePercent() : -1;
			},
			[this, L](float T)
			{
				if (T > 0.6f)
				{
					const SPinguOverheadAbilities::FPaintStats& Paint = SPinguOverheadAbilities::GetLastPaintStats();
					L->Labels = FMath::Max(L->Labels, Paint.DamageLabels);
					L->MaxShown = FMath::Max(L->MaxShown, Paint.MaxDamageShown);
				}
				if (!bShotDone && T > 0.8f)
				{
					bShotDone = true;
					Shot(TEXT("111_trait_damage_percent"), true);
				}
			},
			[this, L, Judge, Finish]
			{
				const float Second = Ratio(L->Pushes[1], L->Pushes[0]);
				const float Third = Ratio(L->Pushes[2], L->Pushes[0]);
				const bool bOk = Near(Second, 1.12f, 0.02f) && Near(Third, 1.24f, 0.02f) && L->DummyDamage == 36 && L->OurDamage == 0 && L->Labels >= 2 && L->MaxShown == 36;
				Judge(TEXT("DamagePercent"), bOk, FString::Printf(TEXT("pushes x1.00 / x%.2f / x%.2f (want 1.12 1.24), the dummy's damage %d %% (want 36), ours %d %% (want 0), "
					"labels under the penguins %d showing up to %d %% (want 2 or more, 36)"), Second, Third, L->DummyDamage, L->OurDamage, L->Labels, L->MaxShown));
				Finish();
				bShotDone = false;
			});
	}

	// ---- Recoil: our hit pushes us back with half its force (away from the one we hit).
	AddStep(TEXT("trait_Recoil"), 0.2f,
		[this, Start, Base, Judge, Finish]
		{
			Start(500.f, EPinguTrait::None);
			const FVector Spot = Base + FVector(500.f, 0.f, 0.f);
			const float NormalPush = Push(Penguin.Get(), Target.Get(), Spot);
			const float NormalBack = Move.IsValid() ? static_cast<float>(Move->Velocity.X) : 0.f;
			ResetTarget(Spot);
			Teleport(Base);
			GiveBoth(Penguin.Get(), Target.Get(), EPinguTrait::Recoil);
			const float Pushed = Push(Penguin.Get(), Target.Get(), Spot);
			const float Back = Move.IsValid() ? static_cast<float>(Move->Velocity.X) : 0.f;
			const bool bOk = BothHave(Penguin.Get(), Target.Get(), EPinguTrait::Recoil) && FMath::Abs(NormalBack) < 1.f && Back < 0.f
				&& Near(Ratio(-Back, Pushed), 0.5f, 0.05f);
			Judge(TEXT("Recoil"), bOk, FString::Printf(TEXT("pushed the dummy %.0f cm/s, pushed back %.0f cm/s (x%.2f of it, want 0.5, away); without the trait %.0f / %.0f"),
				Pushed, Back, Ratio(-Back, Pushed), NormalPush, NormalBack));
			Finish();
			ResetTarget(Spot);
		},
		nullptr, nullptr);

	// ---- Heavy Hitters: pushes x1.5, cooldowns x1.5.
	AddStep(TEXT("trait_HeavyHitters"), 0.2f,
		[this, Start, Base, Judge, Finish]
		{
			Start(900.f, EPinguTrait::None);
			const FVector Spot = Base + FVector(900.f, 0.f, 0.f);
			const float Normal = Push(Penguin.Get(), Target.Get(), Spot);
			GiveBoth(Penguin.Get(), Target.Get(), EPinguTrait::HeavyHitters);
			const float Heavy = Push(Penguin.Get(), Target.Get(), Spot);
			const float Cooldown = Penguin.IsValid() && Penguin->GetTraits() ? Penguin->GetTraits()->GetCooldownScale() : 0.f;
			const float DummyCooldown = Target.IsValid() && Target->GetTraits() ? Target->GetTraits()->GetCooldownScale() : 0.f;
			Judge(TEXT("HeavyHitters"), Near(Ratio(Heavy, Normal), 1.5f, 0.05f) && Near(Cooldown, 1.5f, 0.01f) && Near(DummyCooldown, 1.5f, 0.01f),
				FString::Printf(TEXT("push x%.2f (want 1.5; %.0f / %.0f cm/s), cooldowns x%.2f / x%.2f (want 1.5 both)"), Ratio(Heavy, Normal), Heavy, Normal, Cooldown, DummyCooldown));
			Finish();
			ResetTarget(Spot);
		},
		nullptr, nullptr);

	// ---- Low / High Gravity: the same held jump goes 1 / gravity as high (2.5x on 0.4, 0.625x on 1.6).
	{
		struct FGravity
		{
			float NormalTop = 0.f;
			float TraitTop = 0.f;
			float FromZ = 0.f;
		};
		const auto AddGravityStep = [this, Start, Judge, Finish, Base](const TCHAR* Name, EPinguTrait Trait, float Want, float Tolerance, float TraitAt, float Duration)
		{
			const TSharedRef<FGravity> L = MakeShared<FGravity>();
			AddStep(Name, Duration,
				[this, L, Start]
				{
					*L = FGravity();
					Start(2400.f, EPinguTrait::None);
					L->FromZ = static_cast<float>(Location().Z);
					CountA = 0;
				},
				[this, L, Trait, TraitAt, Base](float T)
				{
					// A held jump without the trait, then (back on the floor, at TraitAt) the same jump with it.
					const float Z = static_cast<float>(Location().Z) - L->FromZ;
					if (CountA == 0 && T > 0.1f)
					{
						CountA = 1;
						Move->JumpPressed();
					}
					if (CountA == 1 && T > 0.1f)
					{
						L->NormalTop = FMath::Max(L->NormalTop, Z);
					}
					if (CountA == 1 && T > TraitAt)
					{
						CountA = 2;
						Move->JumpReleased();
						Teleport(Base);
						GiveBoth(Penguin.Get(), Target.Get(), Trait);
					}
					if (CountA == 2 && T > TraitAt + 0.15f)
					{
						CountA = 3;
						L->FromZ = static_cast<float>(Location().Z);
						Move->JumpPressed();
					}
					if (CountA == 3)
					{
						L->TraitTop = FMath::Max(L->TraitTop, static_cast<float>(Location().Z) - L->FromZ);
					}
				},
				[this, L, Name, Trait, Want, Tolerance, Judge, Finish]
				{
					const float Got = Ratio(L->TraitTop, L->NormalTop);
					const bool bBoth = BothHave(Penguin.Get(), Target.Get(), Trait);
					const float Scale = Target.IsValid() && Target->GetTraits() ? Target->GetTraits()->GetNumbers().GravityScale : 0.f;
					Judge(Name + 6, bBoth && Near(Got, Want, Tolerance), FString::Printf(TEXT("a held jump %.0f cm high (normal %.0f): x%.2f (want about %.2f), the dummy's gravity x%.2f, both have it %d"),
						L->TraitTop, L->NormalTop, Got, Want, Scale, bBoth ? 1 : 0));
					Finish();
				});
		};
		AddGravityStep(TEXT("trait_LowGravity"), EPinguTrait::LowGravity, 1.f / PinguTraitTuning::LowGravityScale, 0.35f, 1.6f, 5.2f);
		AddGravityStep(TEXT("trait_HighGravity"), EPinguTrait::HighGravity, 1.f / PinguTraitTuning::HighGravityScale, 0.1f, 1.6f, 2.8f);
	}

	// ---- Turbo: runs 1.5x as fast (measured), cooldowns x0.5.
	{
		struct FTurbo
		{
			float NormalTop = 0.f;
			float TurboTop = 0.f;
		};
		const TSharedRef<FTurbo> L = MakeShared<FTurbo>();
		AddStep(TEXT("trait_Turbo"), 2.6f,
			[this, L, Start]
			{
				*L = FTurbo();
				Start(2400.f, EPinguTrait::None);
				CountA = 0;
			},
			[this, L, Base](float T)
			{
				VirtualInput = (T > 0.1f && T < 1.1f) || (T > 1.4f && T < 2.4f) ? FVector2D(1.f, 0.f) : FVector2D::ZeroVector;
				const float Vx = static_cast<float>(Move->Velocity.X);
				if (T < 1.2f)
				{
					L->NormalTop = FMath::Max(L->NormalTop, Vx);
				}
				else if (CountA == 0)
				{
					CountA = 1;
					Teleport(Base);
					GiveBoth(Penguin.Get(), Target.Get(), EPinguTrait::Turbo);
				}
				else
				{
					L->TurboTop = FMath::Max(L->TurboTop, Vx);
				}
			},
			[this, L, Judge, Finish]
			{
				const float Cooldown = Target.IsValid() && Target->GetTraits() ? Target->GetTraits()->GetCooldownScale() : 0.f;
				const bool bBoth = BothHave(Penguin.Get(), Target.Get(), EPinguTrait::Turbo);
				Judge(TEXT("Turbo"), bBoth && Near(Ratio(L->TurboTop, L->NormalTop), 1.5f, 0.06f) && Near(Cooldown, 0.5f, 0.01f),
					FString::Printf(TEXT("top run speed %.0f cm/s (normal %.0f: x%.2f, want 1.5), cooldowns x%.2f (want 0.5), both have it %d"),
						L->TurboTop, L->NormalTop, Ratio(L->TurboTop, L->NormalTop), Cooldown, bBoth ? 1 : 0));
				Finish();
			});
	}

	// ---- Bouncy: knocked flying into a wall, it bounces back (85 % of its speed); without the trait it stops at the wall.
	{
		struct FBouncy
		{
			float NormalAway = 0.f;
			float NormalToward = 0.f;
			float Toward = 0.f;
			float Away = 0.f;
		};
		const TSharedRef<FBouncy> L = MakeShared<FBouncy>();
		AddStep(TEXT("trait_Bouncy"), 2.4f,
			[this, L, Start, Base]
			{
				*L = FBouncy();
				Start(2400.f, EPinguTrait::None);
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
				{
					// A wall six blocks high, three wide, a little ahead (temporary: it goes by itself).
					const FIntPoint Foot = Flat->WorldToCell(FVector(Base.X + 140.f, 0.f, 10.f));
					TArray<FIntPoint> Cells;
					for (int32 Row = 0; Row < 6; ++Row)
					{
						for (int32 Column = 0; Column < 3; ++Column)
						{
							Cells.Add(FIntPoint(Foot.X + Column, Foot.Y + Row));
						}
					}
					Flat->AddBlocks(Cells, EBlockKind::Rock, 6.f);
					Flat->FlushEdits();
				}
				CountA = 0;
			},
			[this, L, Base](float T)
			{
				const auto Knock = [this, Base]()
				{
					Teleport(Base + FVector(-300.f, 0.f, 0.f));
					FPenguinHit Hit;
					Hit.Instigator = Target.Get();
					Hit.Source = EPenguinAbility::Snowball;
					Hit.Knockback = FVector2D(1500.f, 350.f);
					Hit.Stun = 0.6f;
					Hit.Location = Location();
					Penguin->ApplyPenguinHit(Hit);
				};
				if (CountA == 0 && T > 0.1f)
				{
					CountA = 1;
					Knock();
				}
				const float Vx = static_cast<float>(Move->Velocity.X);
				if (CountA == 1 && T < 1.1f)
				{
					L->NormalToward = FMath::Max(L->NormalToward, Vx);
					L->NormalAway = FMath::Max(L->NormalAway, -Vx);
				}
				if (CountA == 1 && T > 1.2f)
				{
					CountA = 2;
					GiveBoth(Penguin.Get(), Target.Get(), EPinguTrait::Bouncy);
					Knock();
				}
				if (CountA == 2 && T < 2.3f)
				{
					L->Toward = FMath::Max(L->Toward, Vx);
					L->Away = FMath::Max(L->Away, -Vx);
				}
			},
			[this, L, Judge, Finish]
			{
				const bool bBoth = BothHave(Penguin.Get(), Target.Get(), EPinguTrait::Bouncy);
				const bool bOk = bBoth && L->Away > 0.5f * L->Toward && L->NormalAway < 0.25f * L->NormalToward;
				Judge(TEXT("Bouncy"), bOk, FString::Printf(TEXT("into the wall at %.0f cm/s, back off it at %.0f (want over half); without the trait %.0f / %.0f (want under a quarter), both have it %d"),
					L->Toward, L->Away, L->NormalToward, L->NormalAway, bBoth ? 1 : 0));
				Finish();
			});
	}

	// ---- Ice Rink: the island's snow turns to ice and every floor slides (rock too).
	{
		struct FRink
		{
			int32 IceBefore = 0;
			int32 IceAfter = 0;
			bool bOnIceRock = false;
			int32 RinkCells = 0;
			int32 Layout = 0;
			int32 Seed = 0;
			int32 ArenaPenguins = 0;
		};
		const TSharedRef<FRink> L = MakeShared<FRink>();
		const auto CountIsland = [](const ABlockIsland* Island)
		{
			int32 Count = 0;
			if (Island)
			{
				const FBox Bounds = Island->GetBounds();
				const FIntPoint Low = Island->WorldToCell(Bounds.Min + FVector(1.f, 0.f, 1.f));
				const FIntPoint High = Island->WorldToCell(Bounds.Max - FVector(1.f, 0.f, 1.f));
				for (int32 X = Low.X; X <= High.X; ++X)
				{
					for (int32 Z = Low.Y; Z <= High.Y; ++Z)
					{
						Count += Island->GetBlock(X, Z) == EBlockKind::Ice ? 1 : 0;
					}
				}
			}
			return Count;
		};
		AddStep(TEXT("trait_IceRink"), 1.4f,
			[this, L, Start, CountIsland, HoldStage]
			{
				*L = FRink();
				Start(2400.f, EPinguTrait::None);
				ABlockIsland* Island = ABlockIsland::Get(GetWorld());
				L->IceBefore = CountIsland(Island);
				SetRoundTrait(this, EPinguTrait::IceRink);
				GiveBoth(Penguin.Get(), Target.Get(), EPinguTrait::IceRink); // (the transform freezes the island's snow)
				if (Island)
				{
					Island->FlushEdits();
					L->IceAfter = CountIsland(Island);
					const FBox Bounds = Island->GetBounds();
					HoldStage(FVector(Bounds.GetCenter().X, 0.f, Bounds.GetCenter().Z + 200.f), 0.9f * static_cast<float>(Bounds.Max.X - Bounds.Min.X) + 1500.f);
				}
				L->RinkCells = Penguin.IsValid() && Penguin->GetTraits() ? Penguin->GetTraits()->GetCounters().RinkCells
					+ (Target.IsValid() && Target->GetTraits() ? Target->GetTraits()->GetCounters().RinkCells : 0) : 0;
			},
			[this, L](float T)
			{
				// On the flat strip (no snow turned there): every floor slides all the same.
				L->bOnIceRock |= T > 0.2f && Move->IsMovingOnGround() && Move->IsOnIce();
				if (!bShotDone && T > 0.9f)
				{
					bShotDone = true;
					Shot(TEXT("113_trait_ice_rink"));
				}
			},
			[this, L, Judge, Finish, ReleaseStage]
			{
				const bool bBoth = BothHave(Penguin.Get(), Target.Get(), EPinguTrait::IceRink);
				Judge(TEXT("IceRink"), bBoth && L->IceAfter > L->IceBefore + 10 && L->bOnIceRock && L->RinkCells > 10,
					FString::Printf(TEXT("ice blocks on the island %d -> %d (want 10 or more new), the transforms froze %d, our floor slides %d (want 1), both have it %d"),
						L->IceBefore, L->IceAfter, L->RinkCells, L->bOnIceRock ? 1 : 0, bBoth ? 1 : 0));
				Finish();
				ReleaseStage();
				bShotDone = false;
				// The island as it was (its snow back).
				if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
				{
					Island->Rebuild();
				}
			});
	}

	// ---- Deadly Edges: past a side (the round's start + 1.5 m) is out at once - the dummy, put there, goes out; we stay in.
	// The red sides show (the reveal layer's lines).
	{
		struct FEdges
		{
			bool bDummyOut = false;
			EPenguinKOReason Reason = EPenguinKOReason::None;
			bool bWeStay = true;
			int32 Lines = 0;
		};
		const TSharedRef<FEdges> L = MakeShared<FEdges>();
		AddStep(TEXT("trait_DeadlyEdges"), 1.6f,
			[this, L, Start, Base, HoldStage]
			{
				*L = FEdges();
				Start(400.f, EPinguTrait::DeadlyEdges);
				CountA = 0;
				SetRoundTrait(this, EPinguTrait::DeadlyEdges, FVector2D(Base.X - 500.f, Base.X + 900.f));
				HoldStage(Base + FVector(200.f, 0.f, 150.f), 3400.f);
			},
			[this, L, Base](float T)
			{
				L->Lines = FMath::Max(L->Lines, PinguUI::GetTraitRevealStats().EdgeLines);
				if (!bShotDone && T > 0.5f)
				{
					bShotDone = true;
					Shot(TEXT("112_trait_deadly_edges"), true);
				}
				if (CountA == 0 && T > 0.7f && Target.IsValid())
				{
					CountA = 1;
					Target->NetTeleport(Base + FVector(1000.f, 0.f, 300.f), FVector::ZeroVector, true);
				}
				if (CountA == 1 && Target.IsValid() && Target->IsKnockedOut())
				{
					L->bDummyOut = true;
					L->Reason = Target->GetKOReason();
				}
				L->bWeStay &= Penguin.IsValid() && !Penguin->IsKnockedOut();
			},
			[this, L, Judge, Finish, ReleaseStage, Base]
			{
				Judge(TEXT("DeadlyEdges"), L->bDummyOut && L->Reason == EPenguinKOReason::Edge && L->bWeStay && L->Lines == 2,
					FString::Printf(TEXT("the dummy past the right side out %d (reason %s, want Edge), we inside still in %d, red side lines %d (want 2)"),
						L->bDummyOut ? 1 : 0, *UEnum::GetValueAsString(L->Reason), L->bWeStay ? 1 : 0, L->Lines));
				Finish();
				ReleaseStage();
				bShotDone = false;
				CountA = 0;
				ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			});
	}

	// ---- Mirror for everybody: a shot between two Mirrors goes back once each way (each recharges 6 s), then lands - no
	// endless ping-pong.
	{
		struct FMirrors
		{
			TWeakObjectPtr<APenguinProjectile> Ball;
			int32 Reflections = 0;
			int32 ReflectionsBefore = 0; // (the counters run on over the steps: trait_Mirror's are in them)
			bool bGone = false;
			float GoneAt = -1.f;
		};
		const TSharedRef<FMirrors> L = MakeShared<FMirrors>();
		AddStep(TEXT("trait_MirrorBoth"), 3.f,
			[this, L, Start, Base]
			{
				*L = FMirrors();
				Start(700.f, EPinguTrait::Mirror);
				L->ReflectionsBefore = (Penguin.IsValid() ? Penguin->GetTraits()->GetCounters().Reflections : 0)
					+ (Target.IsValid() ? Target->GetTraits()->GetCounters().Reflections : 0);
				if (Target.IsValid())
				{
					const FVector From = Target->GetAimOrigin() - FVector(45.f, 0.f, 0.f);
					const FVector Dir = (Penguin->GetAimOrigin() - From).GetSafeNormal();
					L->Ball = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Target.Get(), From, Dir * 2100.f);
				}
			},
			[this, L](float T)
			{
				L->Reflections = (Penguin.IsValid() ? Penguin->GetTraits()->GetCounters().Reflections : 0)
					+ (Target.IsValid() ? Target->GetTraits()->GetCounters().Reflections : 0) - L->ReflectionsBefore;
				if (!L->bGone && !L->Ball.IsValid())
				{
					L->bGone = true;
					L->GoneAt = T;
				}
			},
			[this, L, Judge, Finish, Base]
			{
				Judge(TEXT("MirrorBoth"), L->Reflections == 2 && L->bGone, FString::Printf(TEXT("reflections %d (want 2: once each way), the snowball gone %d after %.2f s (want 1)"),
					L->Reflections, L->bGone ? 1 : 0, L->GoneAt));
				if (APenguinProjectile* Ball = L->Ball.Get())
				{
					Ball->Destroy();
				}
				Finish();
				ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			});
	}

	// ---- Every trait for everybody: both get it, both change (the same numbers, the same size), nothing breaks; 0.3 s each.
	{
		const TArray<EPinguTrait> All = PinguTraits::GetAll();
		struct FAll
		{
			int32 Index = -1;
			int32 Bad = 0;
			TArray<FString> BadNames;
		};
		const TSharedRef<FAll> L = MakeShared<FAll>();
		AddStep(TEXT("trait_AllSame"), 0.3f * All.Num() + 0.2f,
			[this, L, Start]
			{
				*L = FAll();
				Start(500.f, EPinguTrait::None);
			},
			[this, L, All](float T)
			{
				const int32 Want = FMath::Min(FMath::FloorToInt(T / 0.3f), All.Num() - 1);
				if (Want == L->Index)
				{
					return;
				}
				// The last one held for 0.3 s: judge it, then the next.
				if (L->Index >= 0)
				{
					const EPinguTrait Was = All[L->Index];
					const bool bSame = BothHave(Penguin.Get(), Target.Get(), Was) && Penguin->IsAlive() && Target->IsAlive()
						&& FMath::IsNearlyEqual(Penguin->GetBaseSize(), Target->GetBaseSize(), 0.01f);
					if (!bSame)
					{
						++L->Bad;
						L->BadNames.Add(PinguTraits::GetLogName(Was));
					}
				}
				L->Index = Want;
				SetRoundTrait(this, All[Want]);
				GiveBoth(Penguin.Get(), Target.Get(), All[Want]);
			},
			[this, L, All, Judge, Finish, Base]
			{
				Judge(TEXT("AllSame"), L->Bad == 0 && L->Index == All.Num() - 1, FString::Printf(TEXT("%d traits on both penguins, %d not the same on both or broke%s%s (want 0)"),
					L->Index + 1, L->Bad, L->BadNames.Num() > 0 ? TEXT(": ") : TEXT(""), *FString::Join(L->BadNames, TEXT(", "))));
				Finish();
				if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
				{
					Island->Rebuild(); // (Ice Rink froze it, Hot Feet may have melted some)
				}
				ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			});
	}

	// ---- The round intro's one trait (look): only the trait's logo (card 2026-11-07_trait-logo-wood-tiling; Linus: "just the
	// trait logo no planks or text with it") - big, centred near the top, no words, no cards over the penguins - then it flies
	// into the top-left corner icon and lands exactly on it; gone once the penguins change.
	{
		struct FLogo
		{
			bool bShown = false;
			int32 Lines = 0;
			bool bOutside = false;
			int32 Cards = 0;
			FString Name;
			bool bGoneAfter = false;
			/** The logo's box at its biggest (absolute = window px in -game) and the window. */
			FSlateRect Biggest;
			FVector2f Viewport = FVector2f::ZeroVector;
			/** The logo's last box before it landed, the corner icon's box just after, and the frames between the logo going and
			 *  the icon showing (want none: a hand-off, no gap). */
			FSlateRect LastFlight;
			float LastFlightAge = -1.f;
			FSlateRect CornerRect;
			bool bCornerAfter = false;
			int32 GapFrames = 0;
			bool bShotFlight = false;
			bool bShotCorner = false;
		};
		const TSharedRef<FLogo> L = MakeShared<FLogo>();
		const auto BeginLogo = [this, L, Start, Base, HoldStage](EPinguTrait Trait)
		{
			*L = FLogo();
			Start(500.f, EPinguTrait::None);
			SetRoundTrait(this, Trait);
			const APenguinGameState* GameState = APenguinGameState::Get(this);
			const float Now = GameState ? static_cast<float>(GameState->GetFlowTime()) : 0.f;
			for (APenguinCharacter* Who : { Penguin.Get(), Target.Get() })
			{
				if (UPenguinTraitComponent* Traits = Who ? Who->GetTraits() : nullptr)
				{
					Traits->SetTrait(Trait, Now + 0.1f);
				}
			}
			if (GEngine && GEngine->GameViewport)
			{
				FVector2D ViewSize;
				GEngine->GameViewport->GetViewportSize(ViewSize);
				L->Viewport = FVector2f(static_cast<float>(ViewSize.X), static_cast<float>(ViewSize.Y));
			}
			HoldStage(Base + FVector(250.f, 0.f, 150.f), 1900.f);
		};
		// Samples the whole step (the reveal starts 0.1 s in: Age = T - 0.1).
		const auto SampleLogo = [this, L](float T, const TCHAR* BigShot, const TCHAR* CornerShot)
		{
			const PinguUI::FTraitRevealStats& Stats = PinguUI::GetTraitRevealStats();
			const PinguUI::FTraitCornerStats& Corner = PinguUI::GetTraitCornerStats();
			const float Age = T - 0.1f;
			const bool bLogo = Stats.bBanner && Stats.Frame + 2 >= GFrameCounter;
			if (bLogo)
			{
				L->bShown = true;
				L->Lines = FMath::Max(L->Lines, Stats.BannerLines);
				L->bOutside |= Stats.bBannerOutside;
				L->Cards = FMath::Max(L->Cards, Stats.Cards);
				L->Name = Stats.BannerName;
				if (Stats.BannerRect.GetSize().Y > L->Biggest.GetSize().Y)
				{
					L->Biggest = Stats.BannerRect;
				}
				if (Stats.BannerAge > UPenguinTraitComponent::CardsTime)
				{
					L->LastFlight = Stats.BannerRect;
					L->LastFlightAge = Stats.BannerAge;
				}
			}
			const bool bCorner = Corner.bShown && Corner.Frame + 2 >= GFrameCounter;
			if (!bLogo && !bCorner && Age > UPenguinTraitComponent::CardsTime && Age < UPenguinTraitComponent::TransformAt + 0.3f)
			{
				++L->GapFrames;
			}
			if (bCorner && !L->bCornerAfter && Age > UPenguinTraitComponent::TransformAt)
			{
				L->bCornerAfter = true;
				L->CornerRect = Corner.Rect;
			}
			if (!bShotDone && T > 1.6f)
			{
				bShotDone = true;
				Shot(BigShot, true);
			}
			if (CornerShot && !L->bShotFlight && Age > UPenguinTraitComponent::CardsTime + 0.2f)
			{
				L->bShotFlight = true;
				Shot(*FString::Printf(TEXT("%s_flight"), CornerShot), true);
			}
			if (CornerShot && !L->bShotCorner && Age > UPenguinTraitComponent::TransformAt + 0.3f)
			{
				L->bShotCorner = true;
				Shot(CornerShot, true);
			}
			if (T > 4.1f)
			{
				L->bGoneAfter = !Stats.bBanner;
			}
		};
		AddStep(TEXT("look_TraitBanner"), 4.4f,
			[BeginLogo] { BeginLogo(EPinguTrait::LowGravity); },
			[SampleLogo](float T) { SampleLogo(T, TEXT("110_trait_logo"), nullptr); },
			[this, L, Judge, Finish, ReleaseStage]
			{
				const float W = FMath::Max(L->Viewport.X, 1.f);
				const float H = FMath::Max(L->Viewport.Y, 1.f);
				const FVector2f Mid = 0.5f * FVector2f(L->Biggest.Left + L->Biggest.Right, L->Biggest.Top + L->Biggest.Bottom);
				const float Big = L->Biggest.GetSize().Y / H;
				const bool bBig = Big >= 0.15f && Big <= 0.3f;
				const bool bPlace = FMath::Abs(Mid.X - 0.5f * W) <= 0.03f * W && Mid.Y >= 0.12f * H && Mid.Y <= 0.35f * H;
				Judge(TEXT("Banner"), L->bShown && L->Lines == 0 && !L->bOutside && L->Cards == 0 && L->Name == TEXT("Low Gravity") && L->bGoneAfter && bBig && bPlace,
					FString::Printf(TEXT("the round-start logo showed %d (%s), words with it %d (want 0: no plank, no text), %.1f %% of the window's height at its "
						"biggest (want 15-30), centre at %.0f, %.0f px in a %.0f x %.0f window (want centred, 12-35 %% down) %d, inside the picture %d, cards over the "
						"penguins %d (want 0), gone once they changed %d"),
						L->bShown ? 1 : 0, *L->Name, L->Lines, 100.f * Big, Mid.X, Mid.Y, W, H, bPlace ? 1 : 0, L->bOutside ? 0 : 1, L->Cards, L->bGoneAfter ? 1 : 0));
				Finish();
				ReleaseStage();
				bShotDone = false;
			});
		// The flight into the corner (Second Chance): the logo's last box before it landed lies on the corner icon's box (centres
		// within 1.5 % of the window's height, sizes within 15 %), and the icon is there in the frame the logo goes (no gap).
		AddStep(TEXT("look_TraitBannerLong"), 4.4f,
			[BeginLogo] { BeginLogo(EPinguTrait::SecondChance); },
			[SampleLogo](float T) { SampleLogo(T, TEXT("110_trait_logo_second_chance"), TEXT("110_trait_logo_corner")); },
			[this, L, Judge, Finish, ReleaseStage]
			{
				const float H = FMath::Max(L->Viewport.Y, 1.f);
				const FVector2f FlightMid = 0.5f * FVector2f(L->LastFlight.Left + L->LastFlight.Right, L->LastFlight.Top + L->LastFlight.Bottom);
				const FVector2f CornerMid = 0.5f * FVector2f(L->CornerRect.Left + L->CornerRect.Right, L->CornerRect.Top + L->CornerRect.Bottom);
				const float Off = FVector2f::Distance(FlightMid, CornerMid) / H;
				const float SizeRatio = L->LastFlight.GetSize().Y / FMath::Max(L->CornerRect.GetSize().Y, 1.f);
				// (The last sampled flight frame is up to 1/60 s before landing: the box is nearly the corner's.)
				const bool bLands = L->bCornerAfter && L->LastFlightAge > UPenguinTraitComponent::TransformAt - 0.05f && Off <= 0.015f
					&& SizeRatio >= 0.85f && SizeRatio <= 1.15f;
				Judge(TEXT("BannerLong"), L->bShown && L->Lines == 0 && !L->bOutside && L->Name == TEXT("Second Chance") && bLands && L->GapFrames == 0,
					FString::Printf(TEXT("the logo showed %d (%s, words %d, want 0), its last flight frame at %.2f s %.1f %% of the height from the corner icon "
						"(want 1.5 or less), %.2fx its size (want 0.85-1.15), the corner icon after it %d, frames with neither %d (want 0), inside the picture %d"),
						L->bShown ? 1 : 0, *L->Name, L->Lines, L->LastFlightAge, 100.f * Off, SizeRatio, L->bCornerAfter ? 1 : 0, L->GapFrames, L->bOutside ? 0 : 1));
				Finish();
				ReleaseStage();
				bShotDone = false;
			});
	}
}
