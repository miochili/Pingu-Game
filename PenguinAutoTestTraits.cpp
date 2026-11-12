// Pingu - the autotest's Traits mode steps (-PinguAutoTest, APenguinAutoTest::AddTraitSteps): the deal, every trait
// measured on our penguin or the dummy (RESULT trait_<Name> lines, each ok / FAIL), Second Chance and an ability bubble,
// and pictures of the looks and the reveal (look_Trait* steps, for looktest too). Plus the lobby test's last part: TRAITS
// in the lobby and a Traits match from there (APinguLobbyTest::UpdateTraitsStage).
//
// A trait is set without a reveal (UPenguinTraitComponent::SetTrait with no reveal start: it changes the penguin at
// once) and taken away at the end of its step, so the steps after these see no trait. What a trait scales is measured
// against the same thing without it in the same step (a push, a gust, a braking distance): the fight's knockback growth
// or the size rule never get in the way.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PenguinTraitLooks.h"
#include "PinguFX.h"
#include "PinguJesterHat.h"
#include "PinguPickup.h"
#include "PinguSettings.h"
#include "PinguTraitTable.h"
#include "PinguUI.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ShaderCompiler.h"

/** What the traits steps keep between them: the checks run and passed (the summary line). */
struct FPinguTraitTestState
{
	int32 Checked = 0;
	int32 Passed = 0;
	TArray<FString> Failed;
};

namespace PenguinAutoTestTraitsPrivate
{
	/** A trait on a penguin at once (no reveal); None takes it away. */
	void SetTraitNow(APenguinCharacter* Who, EPinguTrait Trait)
	{
		if (UPenguinTraitComponent* Traits = Who ? Who->GetTraits() : nullptr)
		{
			Traits->SetTrait(Trait, -1.f);
		}
	}

	/** A plain push from By (null: nobody's, a normal penguin's size) on Victim standing at Where - 1000 cm/s along, 300
	 *  up, the way a snowball hits: the sideways speed it got. (The hit sets the velocity at once: ApplyKnockback.) */
	float PushSpeed(APenguinCharacter* By, APenguinCharacter* Victim, const FVector& Where)
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

	bool IsNear(float Have, float Want, float Tolerance)
	{
		return FMath::Abs(Have - Want) <= Tolerance;
	}

	float Ratio(float Top, float Bottom)
	{
		return FMath::Abs(Bottom) > 1.f ? Top / Bottom : 0.f;
	}

	/** The flow clock (the trait's events and reveal are on it). */
	float FlowNow(const UObject* Context)
	{
		const APenguinGameState* GameState = APenguinGameState::Get(Context);
		return GameState ? static_cast<float>(GameState->GetFlowTime()) : 0.f;
	}

	/** Static mesh components on a penguin (a trait's look adds its pieces as these). */
	int32 CountMeshPieces(const APenguinCharacter* Who)
	{
		TArray<UStaticMeshComponent*> Comps;
		if (Who)
		{
			Who->GetComponents<UStaticMeshComponent>(Comps);
		}
		return Comps.Num();
	}

	/** How round the penguin's eyes are now: the largest ratio of an eye bone's longest to its shortest world axis (1 =
	 *  a perfect sphere; a blink squashes one axis). */
	float EyeRoundness(const APenguinCharacter* Who)
	{
		const USkeletalMeshComponent* SkelMesh = Who ? Who->GetMesh() : nullptr;
		float Worst = 0.f;
		for (const TCHAR* Bone : { TEXT("Eye_L"), TEXT("Eye_R") })
		{
			if (!SkelMesh || SkelMesh->GetBoneIndex(FName(Bone)) == INDEX_NONE)
			{
				return 0.f;
			}
			const FVector Scale = SkelMesh->GetSocketTransform(FName(Bone), RTS_World).GetScale3D().GetAbs();
			Worst = FMath::Max(Worst, static_cast<float>(Scale.GetMax() / FMath::Max(Scale.GetMin(), 1.e-4)));
		}
		return Worst;
	}

	/** The snow bomb's white eyes against the penguin's own (the Eye_L / Eye_R bones): how far the further one is off
	 *  (cm, 999 when they do not show) and their radius. */
	float BombEyesOffOwn(const APenguinCharacter* Who, float& OutRadius)
	{
		FVector Left = FVector::ZeroVector;
		FVector Right = FVector::ZeroVector;
		OutRadius = 0.f;
		const USkeletalMeshComponent* SkelMesh = Who ? Who->GetMesh() : nullptr;
		if (!SkelMesh || !APinguFX::GetAshPileEyeSpots(Who, Left, Right, OutRadius))
		{
			return 999.f;
		}
		FVector BoneLeft = SkelMesh->GetSocketLocation(FName(TEXT("Eye_L")));
		FVector BoneRight = SkelMesh->GetSocketLocation(FName(TEXT("Eye_R")));
		if (BoneLeft.X > BoneRight.X)
		{
			Swap(BoneLeft, BoneRight);
		}
		return static_cast<float>(FMath::Max(FVector::Dist(Left, BoneLeft), FVector::Dist(Right, BoneRight)));
	}

	bool HasOverlay(const APenguinCharacter* Who)
	{
		const USkeletalMeshComponent* SkelMesh = Who ? Who->GetMesh() : nullptr;
		return SkelMesh && SkelMesh->GetOverlayMaterial() != nullptr;
	}

	/** Ice blocks in a row of the flat strip from FromX to ToX (Row: the cell row of z; the floor's top row is at z -50). */
	int32 CountIceInRow(const ABlockIsland* Flat, float FromX, float ToX, float Z)
	{
		if (!Flat)
		{
			return 0;
		}
		const FIntPoint First = Flat->WorldToCell(FVector(FromX, 0.f, Z));
		const FIntPoint Last = Flat->WorldToCell(FVector(ToX, 0.f, Z));
		int32 Count = 0;
		for (int32 Column = First.X; Column <= Last.X; ++Column)
		{
			Count += Flat->GetBlock(Column, First.Y) == EBlockKind::Ice ? 1 : 0;
		}
		return Count;
	}

	/** A wall of Rows blocks, Columns wide, standing on the strip's floor from FromX on (temporary: ClearTemporaryBlocks). */
	void AddWall(ABlockIsland* Flat, float FromX, int32 Rows, int32 Columns, EBlockKind Kind, float Lifetime)
	{
		if (!Flat)
		{
			return;
		}
		const FIntPoint Foot = Flat->WorldToCell(FVector(FromX, 0.f, 10.f));
		TArray<FIntPoint> Cells;
		for (int32 Row = 0; Row < Rows; ++Row)
		{
			for (int32 Column = 0; Column < Columns; ++Column)
			{
				Cells.Add(FIntPoint(Foot.X + Column, Foot.Y + Row));
			}
		}
		Flat->AddBlocks(Cells, Kind, Lifetime);
		Flat->FlushEdits();
	}

	/** The abilities of a loadout that are real (an empty slot is None). */
	int32 CountReal(const TArray<EPenguinAbility>& Loadout)
	{
		int32 Count = 0;
		for (const EPenguinAbility Ability : Loadout)
		{
			Count += Ability != EPenguinAbility::None ? 1 : 0;
		}
		return Count;
	}

	FString DescribeLoadout(const TArray<EPenguinAbility>& Loadout)
	{
		FString Out;
		for (const EPenguinAbility Ability : Loadout)
		{
			Out += FString::Printf(TEXT("%s%s"), Out.IsEmpty() ? TEXT("") : TEXT(" / "), UPenguinAbilityComponent::GetInfo(Ability).Name);
		}
		return Out;
	}
}

void APenguinAutoTest::AddTraitSteps()
{
	using namespace PenguinAutoTestTraitsPrivate;
	TraitTest = MakeShared<FPinguTraitTestState>();
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	// One RESULT line per trait (counted for the summary at the end).
	const auto Judge = [this](const TCHAR* Name, bool bOk, const FString& Detail)
	{
		FPinguTraitTestState& State = *TraitTest;
		++State.Checked;
		State.Passed += bOk ? 1 : 0;
		if (!bOk)
		{
			State.Failed.Add(Name);
		}
		Report(FString::Printf(TEXT("RESULT trait_%s: %s: %s"), Name, *Detail, bOk ? TEXT("ok") : TEXT("FAIL")));
	};
	// A step's start: a fresh strip, our penguin at the base with these abilities (none: keep), the dummy Offset ahead,
	// then the traits (after the round resets: those put the base size back to 1).
	const auto Prepare = [this](float Offset, const TArray<EPenguinAbility>& Abilities, EPinguTrait Ours, EPinguTrait Dummy)
	{
		SpawnTarget(); // (a run filtered to these steps: the dummy may not be there yet)
		PrepareBrawl(Offset, Abilities);
		SetTraitNow(Penguin.Get(), Ours);
		SetTraitNow(Target.Get(), Dummy);
	};
	// A step's end: no trait on anybody, hands off the controls.
	const auto Clear = [this]()
	{
		SetTraitNow(Penguin.Get(), EPinguTrait::None);
		SetTraitNow(Target.Get(), EPinguTrait::None);
		VirtualInput = FVector2D::ZeroVector;
		Move->JumpReleased();
	};

	// ---- The deal (2026-11-07, shared-traits): ONE trait a round for all ten penguins, from a bag of every trait per match.
	// 1000 matches of as many rounds as there are traits: every round all ten the same, every trait once in those rounds; the
	// host's list: with only three ticked, only those three come (each once in every three rounds). Second Chance's loadout:
	// two different abilities in slots 0 and 2 (slot 1 empty).
	AddStep(TEXT("trait_deal"), 0.1f,
		[this, Judge]
		{
			APenguinGameMode* GameMode = GetWorld()->GetAuthGameMode<APenguinGameMode>();
			if (!GameMode)
			{
				Judge(TEXT("deal"), false, TEXT("no game mode"));
				return;
			}
			const TArray<EPinguTrait> ForcedBefore = GameMode->ForcedTraits;
			const TArray<EPinguTrait> BagBefore = GameMode->TraitBag;
			GameMode->ForcedTraits.Reset();
			TArray<APenguinCharacter*> Ten;
			Ten.Init(nullptr, PinguMaxPenguins);
			constexpr int32 Matches = 1000;
			int32 Repeated = 0; // rounds where the ten did not all get the same trait
			int32 Missing = 0;  // matches where a trait did not come in the first PinguNumTraits rounds
			int32 Empty = 0;
			TArray<int32> Counts;
			Counts.Init(0, PinguNumTraits + 1);
			APenguinGameState* DealState = GetWorld()->GetGameState<APenguinGameState>();
			const uint64 OffBefore = DealState ? DealState->GetSettings().TraitsOff : 0;
			if (DealState)
			{
				DealState->GetMutableSettings().TraitsOff = 0;
			}
			for (int32 Match = 0; Match < Matches; ++Match)
			{
				GameMode->TraitBag.Reset(); // a new match: a fresh bag
				TArray<EPinguTrait> Seen;
				for (int32 Round = 0; Round < PinguNumTraits; ++Round)
				{
					const TArray<EPinguTrait> Dealt = GameMode->DealTraits(Ten);
					TArray<EPinguTrait> Different;
					for (const EPinguTrait Trait : Dealt)
					{
						Different.AddUnique(Trait);
						Empty += PinguTraits::IsReal(Trait) ? 0 : 1;
					}
					if (Dealt.Num() > 0)
					{
						Seen.AddUnique(Dealt[0]);
						const int32 Index = static_cast<int32>(Dealt[0]);
						if (Counts.IsValidIndex(Index))
						{
							++Counts[Index];
						}
					}
					Repeated += Different.Num() == 1 && Dealt.Num() == Ten.Num() ? 0 : 1;
				}
				Missing += Seen.Num() == PinguNumTraits ? 0 : 1;
			}
			// The host's list: three ticked.
			int32 Outside = 0;
			int32 BadThrees = 0;
			if (DealState)
			{
				const EPinguTrait Ticked[] = { EPinguTrait::Rubber, EPinguTrait::LowGravity, EPinguTrait::DeadlyEdges };
				uint64 Off = ~uint64(0);
				for (const EPinguTrait Trait : Ticked)
				{
					Off &= ~(uint64(1) << static_cast<int32>(Trait));
				}
				DealState->GetMutableSettings().TraitsOff = Off;
				GameMode->TraitBag.Reset();
				TArray<EPinguTrait> Three;
				for (int32 Round = 0; Round < 30; ++Round)
				{
					const EPinguTrait Got = GameMode->DealTraits(Ten)[0];
					Outside += Got == Ticked[0] || Got == Ticked[1] || Got == Ticked[2] ? 0 : 1;
					Three.AddUnique(Got);
					if (Round % 3 == 2)
					{
						BadThrees += Three.Num() == 3 ? 0 : 1;
						Three.Reset();
					}
				}
				DealState->GetMutableSettings().TraitsOff = OffBefore;
			}
			int32 Fewest = TNumericLimits<int32>::Max();
			int32 Most = 0;
			for (int32 Index = 1; Index <= PinguNumTraits; ++Index)
			{
				Fewest = FMath::Min(Fewest, Counts[Index]);
				Most = FMath::Max(Most, Counts[Index]);
			}
			GameMode->ForcedTraits = ForcedBefore;
			GameMode->TraitBag = BagBefore;
			// Second Chance: two abilities.
			FRandomStream Rand(4321);
			int32 BadShape = 0;
			for (int32 Roll = 0; Roll < Matches; ++Roll)
			{
				const TArray<EPenguinAbility> Loadout = UPenguinAbilityComponent::RollLoadout(Rand, 2);
				const bool bShape = Loadout.Num() == 3 && Loadout[1] == EPenguinAbility::None && Loadout[0] != EPenguinAbility::None
					&& Loadout[2] != EPenguinAbility::None && Loadout[0] != Loadout[2];
				BadShape += bShape ? 0 : 1;
			}
			const bool bOk = Repeated == 0 && Missing == 0 && Empty == 0 && BadShape == 0 && Fewest == Matches && Most == Matches && Outside == 0 && BadThrees == 0;
			Judge(TEXT("deal"), bOk, FString::Printf(TEXT("%d matches of %d rounds x 10 penguins: rounds not all the same %d, matches missing a trait %d, empty deals %d "
				"(want 0 0 0), each trait dealt %d to %d times (want %d); 3 ticked, 30 rounds: %d outside them, %d threes with a repeat (want 0 0); "
				"Second Chance loadouts: %d of %d not {A, none, B} (want 0)"),
				Matches, PinguNumTraits, Repeated, Missing, Empty, Fewest, Most, Matches, Outside, BadThrees, BadShape, Matches));
		},
		nullptr, nullptr);

	// ---- Heavyweight: pushed 0.35x as far, no double jump, runs at 0.80x.
	{
		struct FHeavy
		{
			float Normal = 0.f;
			float Heavy = 0.f;
			float TopSpeed = 0.f;
			int32 AirJumpsAtStart = 0;
		};
		const TSharedRef<FHeavy> L = MakeShared<FHeavy>();
		AddStep(TEXT("trait_Heavyweight"), 2.6f,
			[this, L, Prepare, Base]
			{
				*L = FHeavy();
				Prepare(900.f, {}, EPinguTrait::None, EPinguTrait::None);
				const FVector Spot = Base + FVector(900.f, 0.f, 0.f);
				L->Normal = PushSpeed(Penguin.Get(), Target.Get(), Spot);
				SetTraitNow(Target.Get(), EPinguTrait::Heavyweight);
				L->Heavy = PushSpeed(Penguin.Get(), Target.Get(), Spot);
				SetTraitNow(Target.Get(), EPinguTrait::None);
				ResetTarget(Spot);
				SetTraitNow(Penguin.Get(), EPinguTrait::Heavyweight);
				L->AirJumpsAtStart = Move->AirJumpEvents;
			},
			[this, L](float T)
			{
				VirtualInput = T > 0.1f && T < 1.1f ? FVector2D(1.f, 0.f) : FVector2D::ZeroVector;
				if (T > 0.8f && T < 1.1f)
				{
					L->TopSpeed = FMath::Max(L->TopSpeed, static_cast<float>(Move->Velocity.X));
				}
				if (!bFlag && T > 1.5f)
				{
					bFlag = true;
					Move->JumpPressed();
				}
				if (MarkA < 0.f && T > 1.8f)
				{
					MarkA = T;
					Move->JumpReleased();
				}
				if (MarkB < 0.f && T > 1.9f)
				{
					MarkB = T;
					Move->JumpPressed(); // in the air: a double jump would come out
				}
				if (T > 2.1f)
				{
					Move->JumpReleased();
				}
			},
			[this, L, Judge, Clear]
			{
				const float Taken = Ratio(L->Heavy, L->Normal);
				const int32 AirJumps = Move->AirJumpEvents - L->AirJumpsAtStart;
				const float Run = Ratio(L->TopSpeed, Move->RunSpeed);
				const bool bOk = IsNear(Taken, 0.35f, 0.05f) && AirJumps == 0 && Move->GetMaxAirJumpsNow() == 0 && IsNear(Run, 0.8f, 0.03f);
				Judge(TEXT("Heavyweight"), bOk, FString::Printf(TEXT("pushed x%.2f (%.0f / %.0f cm/s, want 0.35), double jumps %d of %d allowed (want 0 0), runs %.0f cm/s = x%.2f (want 0.80)"),
					Taken, L->Heavy, L->Normal, AirJumps, Move->GetMaxAirJumpsNow(), L->TopSpeed, Run));
				Clear();
			});
	}

	// ---- Quick Draw / Slow Burn / Jester: the tornado's (the Gust slot's) cooldown and push (strength: its throw's knock x the
	// use's strength) - and Jester's slot turning into another ability 0.4 s after the use, its cooldown carried over. A tornado
	// throws half a second after its use and holds its user a second: first one without the trait, then one with it.
	struct FGustCase
	{
		EPinguTrait Trait;
		const TCHAR* Name;
		float Cooldown;
		float Strength;
	};
	for (const FGustCase& Case : { FGustCase{ EPinguTrait::QuickDraw, TEXT("QuickDraw"), 1.75f, 0.7f }, FGustCase{ EPinguTrait::SlowBurn, TEXT("SlowBurn"), 7.f, 1.75f },
		FGustCase{ EPinguTrait::Jester, TEXT("Jester"), 3.5f, 1.3f } })
	{
		struct FGust
		{
			float NormalPush = 0.f;
			float TraitPush = 0.f;
			int32 HitsSeen = 0;
			float Cooldown = -1.f;
			float Length = -1.f;
			float UsedAt = -1.f;
			float SwapAt = -1.f;
			float LeftBefore = -1.f;
			float LeftAfter = -1.f;
			EPenguinAbility Swapped = EPenguinAbility::None;
		};
		const TSharedRef<FGust> L = MakeShared<FGust>();
		const TArray<EPenguinAbility> Kit = { EPenguinAbility::Gust, EPenguinAbility::Snowball, EPenguinAbility::IceWall };
		AddStep(*FString::Printf(TEXT("trait_%s"), Case.Name), 2.6f,
			[this, L, Prepare, Kit]
			{
				*L = FGust();
				Prepare(220.f, Kit, EPinguTrait::None, EPinguTrait::None);
			},
			[this, L, Case, Kit, Base](float T)
			{
				UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
				const UPenguinMovementComponent* DummyMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
				if (!Abilities || !DummyMove)
				{
					return;
				}
				// The dummy's knock when the tornado throws it (the first one's, then the trait's).
				if (bFlag && DummyMove->HitEvents != L->HitsSeen)
				{
					L->HitsSeen = DummyMove->HitEvents;
					(L->UsedAt < 0.f ? L->NormalPush : L->TraitPush) = DummyMove->LastHitSpeed;
				}
				// First a tornado without the trait, then one with it (both from the base at the dummy 220 cm ahead).
				if (!bFlag && T > 0.15f)
				{
					bFlag = true;
					L->HitsSeen = DummyMove->HitEvents;
					Penguin->SetAimPoint(Target->GetAimOrigin());
					Penguin->ActivateAbility(0);
				}
				if (L->UsedAt < 0.f && T > 1.25f) // (the first one threw at 0.65 s and let us go at 1.15 s)
				{
					Teleport(Base);
					ResetTarget(Base + FVector(220.f, 0.f, 0.f));
					SetTraitNow(Penguin.Get(), Case.Trait);
					Abilities->SetAbilities(Kit);
					L->HitsSeen = DummyMove->HitEvents;
					Penguin->SetAimPoint(Target->GetAimOrigin());
					Penguin->ActivateAbility(0);
					L->UsedAt = T;
					L->Cooldown = Abilities->GetCooldownRemaining(0);
					L->Length = Abilities->GetCooldownLength(0);
				}
				// Jester: the slot turns into another ability (watched every frame).
				if (L->UsedAt >= 0.f && L->SwapAt < 0.f)
				{
					if (Abilities->GetAbility(0) != EPenguinAbility::Gust)
					{
						L->SwapAt = T - L->UsedAt;
						L->Swapped = Abilities->GetAbility(0);
						L->LeftAfter = Abilities->GetCooldownRemaining(0);
					}
					else
					{
						L->LeftBefore = Abilities->GetCooldownRemaining(0);
					}
				}
			},
			[this, L, Case, Judge, Clear]
			{
				const float Strength = Ratio(L->TraitPush, L->NormalPush);
				bool bOk = IsNear(L->Cooldown, Case.Cooldown, 0.05f) && IsNear(Strength, Case.Strength, 0.05f);
				FString Detail = FString::Printf(TEXT("tornado cooldown %.2f s (length %.2f, want %.2f), its throw x%.2f (%.0f / %.0f cm/s, want x%.2f)"),
					L->Cooldown, L->Length, Case.Cooldown, Strength, L->TraitPush, L->NormalPush, Case.Strength);
				if (Case.Trait == EPinguTrait::Jester)
				{
					// (The new one is none of the other two; the cooldown runs on: a frame apart, about 1/60 s less.)
					const bool bNew = L->Swapped != EPenguinAbility::None && L->Swapped != EPenguinAbility::Gust && L->Swapped != EPenguinAbility::Snowball
						&& L->Swapped != EPenguinAbility::IceWall;
					const bool bCarried = L->LeftBefore > 0.f && L->LeftAfter > 0.f && FMath::Abs(L->LeftBefore - L->LeftAfter) < 0.1f;
					bOk = bOk && IsNear(L->SwapAt, 0.4f, 0.07f) && bNew && bCarried;
					Detail += FString::Printf(TEXT(", the slot became %s %.2f s after the use (want another ability after 0.4 s), cooldown %.2f -> %.2f s (want carried over)"),
						UPenguinAbilityComponent::GetInfo(L->Swapped).Name, L->SwapAt, L->LeftBefore, L->LeftAfter);
				}
				Judge(Case.Name, bOk, Detail);
				Clear();
			});
	}

	// ---- Ice Touch: the snow it walks on turns to ice, a snowball that touches it freezes and drops, a penguin it touches
	// gets frozen feet.
	{
		struct FIce
		{
			float WalkFrom = 0.f;
			int32 IceCells = -1;
			int32 OwnHits = 0;
			TWeakObjectPtr<APenguinProjectile> Ball;
			float BallSpeed = 0.f;
			bool bBallFrozen = false;
			float BallDropVz = 0.f;
			float BallKeptVx = 1.f;
			bool bTouchStarted = false;
			bool bDummySlips = false;
		};
		const TSharedRef<FIce> L = MakeShared<FIce>();
		AddStep(TEXT("trait_IceTouch"), 3.8f,
			[this, L, Prepare]
			{
				*L = FIce();
				Prepare(2400.f, {}, EPinguTrait::IceTouch, EPinguTrait::None);
				L->WalkFrom = static_cast<float>(Location().X);
				L->OwnHits = Move->HitEvents;
			},
			[this, L, Base](float T)
			{
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				// Four blocks to the right.
				VirtualInput = T > 0.1f && T < 1.4f && Location().X < L->WalkFrom + 400.f ? FVector2D(1.f, 0.f) : FVector2D::ZeroVector;
				if (L->IceCells < 0 && T > 1.4f)
				{
					L->IceCells = CountIceInRow(Flat, L->WalkFrom - 60.f, L->WalkFrom + 520.f, -50.f);
				}
				// The dummy throws a snowball at us.
				if (!bFlag && T > 1.5f && Target.IsValid())
				{
					bFlag = true;
					ResetTarget(Location() + FVector(700.f, 0.f, 0.f));
					const FVector From = Target->GetAimOrigin() - FVector(45.f, 0.f, 0.f);
					const FVector Dir = (Penguin->GetAimOrigin() + FVector(0.f, 0.f, 30.f) - From).GetSafeNormal();
					L->BallSpeed = 2100.f;
					L->Ball = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Target.Get(), From, Dir * L->BallSpeed);
				}
				if (const APenguinProjectile* Ball = L->Ball.Get())
				{
					if (Ball->IsFrozen())
					{
						if (!L->bBallFrozen)
						{
							L->bBallFrozen = true;
							L->BallKeptVx = FMath::Abs(static_cast<float>(Ball->GetVelocity().X)) / FMath::Max(L->BallSpeed, 1.f);
						}
						L->BallDropVz = FMath::Min(L->BallDropVz, static_cast<float>(Ball->GetVelocity().Z));
					}
				}
				// A touch: walking into the dummy on fresh snow.
				if (!L->bTouchStarted && T > 2.6f)
				{
					L->bTouchStarted = true;
					L->OwnHits = Move->HitEvents - L->OwnHits; // (the snowball part is over: hits taken by then)
					if (L->Ball.IsValid())
					{
						L->Ball->Destroy();
					}
					const FVector Spot = Base + FVector(-1500.f, 0.f, 0.f);
					Teleport(Spot);
					ResetTarget(Spot + FVector(130.f, 0.f, 0.f));
				}
				if (L->bTouchStarted)
				{
					VirtualInput = T > 2.8f && T < 3.4f ? FVector2D(1.f, 0.f) : FVector2D::ZeroVector;
					const UPenguinMovementComponent* DummyMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
					L->bDummySlips |= DummyMove && DummyMove->IsSlippery();
				}
			},
			[this, L, Judge, Clear]
			{
				const bool bDropped = L->bBallFrozen && L->BallDropVz < -50.f && L->BallKeptVx < 0.3f;
				const bool bOk = L->IceCells >= 3 && bDropped && L->OwnHits == 0 && L->bDummySlips;
				Judge(TEXT("IceTouch"), bOk, FString::Printf(TEXT("ice cells after a 4 block walk %d (want 3 or more); the dummy's snowball frozen %d, falling at %.0f cm/s, kept x%.2f of its speed along (want frozen, falling, under 0.3), "
					"hits on us %d (want 0); the dummy we walked into has frozen feet %d (want 1)"),
					L->IceCells, L->bBallFrozen ? 1 : 0, -L->BallDropVz, L->BallKeptVx, L->OwnHits, L->bDummySlips ? 1 : 0));
				Clear();
			});
	}

	// ---- Featherweight: a triple jump (two air jumps), a slow fall (at most 950 cm/s), pushed 1.5x as far.
	{
		struct FFeather
		{
			float Normal = 0.f;
			float Light = 0.f;
			int32 AirJumpsAtStart = 0;
			int32 AirJumps = 0;
			float FastestFall = 0.f;
			bool bDropped = false;
		};
		const TSharedRef<FFeather> L = MakeShared<FFeather>();
		AddStep(TEXT("trait_Featherweight"), 4.4f,
			[this, L, Prepare, Base]
			{
				*L = FFeather();
				Prepare(900.f, {}, EPinguTrait::None, EPinguTrait::None);
				const FVector Spot = Base + FVector(900.f, 0.f, 0.f);
				L->Normal = PushSpeed(Penguin.Get(), Target.Get(), Spot);
				SetTraitNow(Target.Get(), EPinguTrait::Featherweight);
				L->Light = PushSpeed(Penguin.Get(), Target.Get(), Spot);
				SetTraitNow(Target.Get(), EPinguTrait::None);
				ResetTarget(Spot);
				SetTraitNow(Penguin.Get(), EPinguTrait::Featherweight);
				L->AirJumpsAtStart = Move->AirJumpEvents;
			},
			[this, L, Base](float T)
			{
				// Jump, then three more presses in the air: two come out.
				static const float Presses[] = { 0.2f, 0.5f, 0.8f, 1.1f };
				for (int32 Index = 0; Index < 4; ++Index)
				{
					if (T > Presses[Index] && CountA == Index)
					{
						Move->JumpPressed();
						CountA = Index + 1;
					}
					if (T > Presses[Index] + 0.2f && CountB == Index)
					{
						Move->JumpReleased();
						CountB = Index + 1;
					}
				}
				if (T < 1.8f)
				{
					L->AirJumps = Move->AirJumpEvents - L->AirJumpsAtStart;
				}
				// Dropped from 15 m: the fastest fall.
				if (!L->bDropped && T > 1.8f)
				{
					L->bDropped = true;
					Teleport(Base + FVector(0.f, 0.f, 1500.f));
				}
				if (L->bDropped && Move->IsFalling())
				{
					L->FastestFall = FMath::Max(L->FastestFall, static_cast<float>(-Move->Velocity.Z));
				}
			},
			[this, L, Judge, Clear]
			{
				const float Taken = Ratio(L->Light, L->Normal);
				const bool bOk = IsNear(Taken, 1.5f, 0.05f) && L->AirJumps == 2 && Move->GetMaxAirJumpsNow() == 2 && L->FastestFall > 500.f && L->FastestFall <= 960.f;
				Judge(TEXT("Featherweight"), bOk, FString::Printf(TEXT("air jumps %d of 3 presses (want 2), fastest fall %.0f cm/s (want 950 at most), pushed x%.2f (want 1.5)"),
					L->AirJumps, L->FastestFall, Taken));
				Clear();
			});
	}

	// ---- Tiny: one shrink step small (0.707), walks through a gap one block high standing up; one shrink ray hit more makes
	// it 0.5 (still in), a second one makes it vanish.
	{
		struct FTiny
		{
			float Size = 0.f;
			bool bThrough = false;
			bool bLay = false;
			int32 SlidesAtStart = 0;
			float DummyAfterOne = 0.f;
			bool bAliveAfterOne = false;
			uint8 Reason = 0;
		};
		const TSharedRef<FTiny> L = MakeShared<FTiny>();
		AddStep(TEXT("trait_Tiny"), 3.8f,
			[this, L, Prepare, Base]
			{
				*L = FTiny();
				Prepare(2400.f, {}, EPinguTrait::Tiny, EPinguTrait::Tiny);
				BuildTunnel(Base, 300.f, 2, false);
				L->SlidesAtStart = Move->SlideEvents;
			},
			[this, L](float T)
			{
				if (T > 0.6f && L->Size <= 0.f)
				{
					L->Size = Penguin->GetSizeScale();
				}
				VirtualInput = T > 0.7f && T < 2.2f ? FVector2D(1.f, 0.f) : FVector2D::ZeroVector;
				L->bLay |= Move->IsLyingLow() || Move->IsBellySliding();
				if (static_cast<float>(Location().X) > TunnelExitX + 40.f)
				{
					L->bThrough = true;
				}
				// Shrink ray hits on the Tiny dummy.
				const auto Shrink = [this]()
				{
					FPenguinHit Hit;
					Hit.Instigator = Penguin.Get();
					Hit.Source = EPenguinAbility::ShrinkRay;
					Hit.SizeFactor = PinguShrinkRayFactor;
					Hit.Stun = 0.f;
					Hit.Location = Target->GetActorLocation();
					Target->ApplyPenguinHit(Hit);
				};
				if (Target.IsValid() && !bFlag && T > 2.3f)
				{
					bFlag = true;
					Shrink();
				}
				if (Target.IsValid() && MarkA < 0.f && T > 2.9f)
				{
					MarkA = T;
					L->DummyAfterOne = Target->GetSizeTarget();
					L->bAliveAfterOne = Target->IsAlive();
					Shrink();
				}
				if (Target.IsValid() && Target->IsKnockedOut())
				{
					L->Reason = static_cast<uint8>(Target->GetKOReason());
				}
			},
			[this, L, Judge, Clear, Base]
			{
				const bool bOk = IsNear(L->Size, PinguShrinkRayFactor, 0.02f) && L->bThrough && !L->bLay && Move->SlideEvents == L->SlidesAtStart
					&& IsNear(L->DummyAfterOne, PinguMinSize, 0.02f) && L->bAliveAfterOne && L->Reason == static_cast<uint8>(EPenguinKOReason::Shrunk);
				Judge(TEXT("Tiny"), bOk, FString::Printf(TEXT("size %.3f (want 0.707), walked through a gap one block high %d, on its belly %d, slides %d (want 1 0 0); "
					"one shrink ray hit: %.2f, still in %d (want 0.5, 1); a second one: knocked out %s (want Shrunk)"),
					L->Size, L->bThrough ? 1 : 0, L->bLay ? 1 : 0, Move->SlideEvents - L->SlidesAtStart, L->DummyAfterOne, L->bAliveAfterOne ? 1 : 0,
					*UEnum::GetValueAsString(static_cast<EPenguinKOReason>(L->Reason))));
				Clear();
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
				{
					Flat->ClearTemporaryBlocks();
				}
				ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			});
	}

	// ---- Giant: two grow steps big (1.68), pushed about half as far (by a normal penguin: the size rule), a hard landing
	// breaks the ice under its feet.
	{
		struct FGiant
		{
			float Normal = 0.f;
			float Big = 0.f;
			float Size = 0.f;
			float DummySize = 0.f;
			int32 IceBefore = 0;
			int32 IceAfter = -1;
			int32 BrokenBefore = 0;
			int32 Broken = 0;
			int32 LandsAtDrop = 0;
			float LandSpeed = 0.f;
			float FallSpeed = 0.f;
			bool bDropped = false;
		};
		const TSharedRef<FGiant> L = MakeShared<FGiant>();
		AddStep(TEXT("trait_Giant"), 2.6f,
			[this, L, Prepare, Base]
			{
				*L = FGiant();
				Prepare(1000.f, {}, EPinguTrait::None, EPinguTrait::None);
				// Pushed by nobody (a normal penguin's size): without the trait, then (below) once it has grown.
				L->Normal = PushSpeed(nullptr, Target.Get(), Base + FVector(1000.f, 0.f, 0.f));
				ResetTarget(Base + FVector(1000.f, 0.f, 0.f));
				SetTraitNow(Target.Get(), EPinguTrait::Giant);
				SetTraitNow(Penguin.Get(), EPinguTrait::Giant);
			},
			[this, L, Base](float T)
			{
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (!bFlag && T > 0.8f)
				{
					bFlag = true;
					L->Size = Penguin->GetSizeScale();
					L->DummySize = Target.IsValid() ? Target->GetSizeScale() : 0.f;
					L->Big = PushSpeed(nullptr, Target.Get(), Base + FVector(1000.f, 0.f, 0.f));
					// The floor under the base turns to ice, then we drop onto it from 7 m.
					if (Flat)
					{
						Flat->FreezeBlocksInRadius(FVector(Base.X, 0.f, -50.f), 160.f, true);
						L->IceBefore = CountIceInRow(Flat, Base.X - 250.f, Base.X + 250.f, -50.f);
						L->BrokenBefore = Flat->GetBrokenBlocks();
					}
					Teleport(Base + FVector(0.f, 0.f, 700.f));
					L->bDropped = true;
					L->LandsAtDrop = Move->LandEvents;
				}
				if (L->bDropped && Move->LandEvents == L->LandsAtDrop)
				{
					L->FallSpeed = FMath::Max(L->FallSpeed, static_cast<float>(-Move->Velocity.Z));
				}
				else if (L->bDropped && L->LandSpeed <= 0.f)
				{
					L->LandSpeed = L->FallSpeed;
				}
				if (L->IceAfter < 0 && T > 2.4f && Flat)
				{
					L->IceAfter = CountIceInRow(Flat, Base.X - 250.f, Base.X + 250.f, -50.f);
					L->Broken = Flat->GetBrokenBlocks() - L->BrokenBefore;
				}
			},
			[this, L, Judge, Clear, Base]
			{
				const float Taken = Ratio(L->Big, L->Normal);
				const bool bBroke = L->Broken >= 1 || (L->IceAfter >= 0 && L->IceAfter < L->IceBefore);
				const bool bOk = IsNear(L->Size, 1.682f, 0.03f) && IsNear(L->DummySize, 1.682f, 0.03f) && IsNear(Taken, 0.52f, 0.05f) && L->LandSpeed >= 700.f && bBroke;
				Judge(TEXT("Giant"), bOk, FString::Printf(TEXT("size %.2f, the dummy %.2f (want 1.68), pushed x%.2f by a normal penguin (want 0.52), landed at %.0f cm/s (want 700 or more) on %d ice blocks: "
					"broken %d, ice left %d (want some broken)"), L->Size, L->DummySize, Taken, L->LandSpeed, L->IceBefore, L->Broken, L->IceAfter));
				Clear();
				ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			});
	}

	// ---- Glass Cannon: pushes 1.5x as hard, is pushed 1.5x as far.
	AddStep(TEXT("trait_GlassCannon"), 0.3f,
		[this, Prepare, Judge, Clear, Base]
		{
			Prepare(900.f, {}, EPinguTrait::None, EPinguTrait::None);
			const FVector Spot = Base + FVector(900.f, 0.f, 0.f);
			const float Normal = PushSpeed(Penguin.Get(), Target.Get(), Spot);
			SetTraitNow(Penguin.Get(), EPinguTrait::GlassCannon);
			const float Dealt = PushSpeed(Penguin.Get(), Target.Get(), Spot);
			SetTraitNow(Penguin.Get(), EPinguTrait::None);
			SetTraitNow(Target.Get(), EPinguTrait::GlassCannon);
			const float Taken = PushSpeed(Penguin.Get(), Target.Get(), Spot);
			const bool bOk = IsNear(Ratio(Dealt, Normal), 1.5f, 0.05f) && IsNear(Ratio(Taken, Normal), 1.5f, 0.05f);
			Judge(TEXT("GlassCannon"), bOk, FString::Printf(TEXT("its push x%.2f, pushed x%.2f (want 1.5 1.5; %.0f / %.0f / %.0f cm/s)"),
				Ratio(Dealt, Normal), Ratio(Taken, Normal), Dealt, Taken, Normal));
			Clear();
			ResetTarget(Spot);
		},
		nullptr, nullptr);

	// The stage camera held still for the pictures of a step (the look steps' way), and let go again at its end.
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

	// ---- Rubber: no ledge grab; in a knockback flight it bounces off a wall (back the other way); every fall bounces back up
	// 70 % as high - dropped from 5 m: 3.5 m, then 2.45 m, ... until the bounce would leave slower than 500 cm/s, then it
	// settles; a jump pressed for a landing jumps instead of bouncing.
	{
		struct FRubber
		{
			int32 ClimbsAtStart = 0;
			bool bKnocked = false;
			float TowardWall = 0.f;
			float AwayAfter = 0.f;
			// the 5 m drop: the capsule's middle when dropped and when it rested (heights are measured from there), the top of
			// every bounce (by BounceEvents since the drop), when it settled
			bool bDropped = false;
			float DropZ = 0.f;
			float LandZ = 0.f;
			float RestZ = 0.f;
			int32 LandsAtDrop = 0;
			int32 BouncesAtDrop = 0;
			TArray<float> BounceTop;
			float FirstFall = 0.f;
			float FirstUp = 0.f;
			float GroundTime = 0.f;
			float SettledAt = -1.f;
			int32 Bounces = 0;
			bool bShotFall = false;
			int32 ApexShots = 0;
			// a jump pressed just before a landing
			bool bJumpDrop = false;
			bool bJumpPressed = false;
			int32 JumpsAtPress = 0;
			int32 BouncesAtPress = 0;
			bool bJumpSeen = false;
			int32 BouncesAtJump = 0;
			float JumpPressedAt = -1.f;
		};
		const TSharedRef<FRubber> L = MakeShared<FRubber>();
		static constexpr float DropHeight = 500.f;
		static constexpr float DropAt = 3.f;
		static constexpr float JumpDropAt = 8.2f;
		AddStep(TEXT("trait_Rubber"), 9.6f,
			[this, L, Prepare, Base]
			{
				*L = FRubber();
				Prepare(2400.f, {}, EPinguTrait::Rubber, EPinguTrait::None);
				// Six blocks high (a jump and a double jump top out about one block short: the ledge grab would do the rest).
				AddWall(ABlockIsland::FindNear(GetWorld(), Base), Base.X + 140.f, 6, 3, EBlockKind::Rock, 10.f);
				L->ClimbsAtStart = Move->ClimbEvents;
			},
			[this, L, Base, HoldStage](float T)
			{
				// Jump, double jump and a third press next to the wall: no ledge grab.
				if (T < 1.7f)
				{
					VirtualInput = FVector2D(1.f, 0.f);
					static const float Stages[] = { 0.1f, 0.48f, 0.52f, 0.86f, 0.9f, 0.98f };
					for (int32 Index = 0; Index < 6; ++Index)
					{
						if (T > Stages[Index] && CountA == Index)
						{
							CountA = Index + 1;
							if (Index % 2 == 0)
							{
								Move->JumpPressed();
							}
							else
							{
								Move->JumpReleased();
							}
						}
					}
				}
				// Knocked flying into the wall.
				if (!L->bKnocked && T > 1.8f)
				{
					L->bKnocked = true;
					VirtualInput = FVector2D::ZeroVector;
					Move->JumpReleased();
					Teleport(Base + FVector(-300.f, 0.f, 0.f));
					FPenguinHit Hit;
					Hit.Instigator = Target.Get();
					Hit.Source = EPenguinAbility::Snowball;
					Hit.Knockback = FVector2D(1500.f, 350.f);
					Hit.Stun = 0.6f;
					Hit.Location = Location();
					Penguin->ApplyPenguinHit(Hit);
				}
				if (L->bKnocked && T < 2.9f)
				{
					const float Vx = static_cast<float>(Move->Velocity.X);
					L->TowardWall = FMath::Max(L->TowardWall, Vx);
					L->AwayAfter = FMath::Max(L->AwayAfter, -Vx);
				}
				// Dropped from 5 m (its feet 5 m over the floor, at rest): every landing bounces back up 70 % as high.
				const FVector DropSpot = Base + FVector(-1200.f, 0.f, DropHeight);
				if (!L->bDropped && T > DropAt)
				{
					L->bDropped = true;
					Teleport(DropSpot);
					L->DropZ = static_cast<float>(Location().Z);
					L->LandsAtDrop = Move->LandEvents;
					L->BouncesAtDrop = Move->BounceEvents;
					HoldStage(FVector(DropSpot.X, 0.f, 270.f), 2100.f);
				}
				if (L->bDropped && L->SettledAt < 0.f)
				{
					const float Z = static_cast<float>(Location().Z);
					if (L->LandZ == 0.f && Move->LandEvents != L->LandsAtDrop)
					{
						L->LandZ = Z; // (the first landing's frame: it is back on the floor's height before the bounce moves it)
					}
					const int32 Bounce = Move->BounceEvents - L->BouncesAtDrop;
					if (Bounce == 1 && L->FirstUp <= 0.f)
					{
						L->FirstFall = Move->LastBounceFallHeight;
						L->FirstUp = Move->LastBounceUpSpeed;
					}
					if (Bounce >= 1)
					{
						while (L->BounceTop.Num() < Bounce)
						{
							L->BounceTop.Add(-1.e9f);
						}
						L->BounceTop[Bounce - 1] = FMath::Max(L->BounceTop[Bounce - 1], Z);
						// The pictures: falling, then the top of the first two bounces.
						if (Bounce <= 2 && L->ApexShots < Bounce && Move->IsFalling() && Move->Velocity.Z <= 0.f)
						{
							L->ApexShots = Bounce;
							Shot(Bounce == 1 ? TEXT("70_rubber_2_bounce1_top") : TEXT("70_rubber_3_bounce2_top"));
						}
					}
					if (!L->bShotFall && T > DropAt + 0.2f)
					{
						L->bShotFall = true;
						Shot(TEXT("70_rubber_1_drop"));
					}
					// Settled: on the ground for a moment with no bounce since.
					L->GroundTime = Move->IsMovingOnGround() ? L->GroundTime + GetWorld()->GetDeltaSeconds() : 0.f;
					if (Bounce >= 1 && L->GroundTime > 0.3f)
					{
						L->SettledAt = T - L->GroundTime;
						L->RestZ = Z;
						L->Bounces = Bounce;
					}
				}
				// A jump pressed just before a landing (2 m drop): it jumps, it does not bounce.
				if (!L->bJumpDrop && T > JumpDropAt)
				{
					L->bJumpDrop = true;
					Teleport(Base + FVector(-1200.f, 0.f, 200.f));
				}
				if (L->bJumpDrop && !L->bJumpPressed && Move->IsFalling() && Move->Velocity.Z < 0.f && L->RestZ > 0.f
					&& static_cast<float>(Location().Z) - L->RestZ < 30.f)
				{
					L->bJumpPressed = true;
					L->JumpPressedAt = T;
					L->JumpsAtPress = Move->JumpEvents;
					L->BouncesAtPress = Move->BounceEvents;
					Move->JumpPressed();
				}
				if (L->bJumpPressed && !L->bJumpSeen && Move->JumpEvents != L->JumpsAtPress)
				{
					L->bJumpSeen = true;
					L->BouncesAtJump = Move->BounceEvents;
				}
				if (L->bJumpPressed && T > L->JumpPressedAt + 0.15f)
				{
					Move->JumpReleased();
				}
			},
			[this, L, Judge, Clear, Base, ReleaseStage]
			{
				using namespace PinguTraitTuning;
				ReleaseStage();
				const int32 Climbs = Move->ClimbEvents - L->ClimbsAtStart;
				const bool bWall = L->TowardWall > 500.f && L->AwayAfter > 0.5f * L->TowardWall;
				// Heights over the resting middle (its feet over the floor).
				const float Rest = L->RestZ > 0.f ? L->RestZ : L->LandZ;
				const float Fell = L->DropZ - Rest;
				const float First = L->BounceTop.Num() >= 1 ? L->BounceTop[0] - Rest : 0.f;
				const float Second = L->BounceTop.Num() >= 2 ? L->BounceTop[1] - Rest : 0.f;
				FString Tops;
				for (const float Top : L->BounceTop)
				{
					Tops += FString::Printf(TEXT("%s%.0f"), Tops.IsEmpty() ? TEXT("") : TEXT(" / "), Top - Rest);
				}
				bool bFalling = true; // every bounce lower than the one before
				for (int32 Index = 1; Index < L->BounceTop.Num(); ++Index)
				{
					bFalling &= L->BounceTop[Index] < L->BounceTop[Index - 1];
				}
				const float WantFirst = RubberBounceHeight * Fell;
				const float WantSecond = RubberBounceHeight * RubberBounceHeight * Fell;
				const bool bHeights = FMath::Abs(First - WantFirst) <= 0.05f * WantFirst && FMath::Abs(Second - WantSecond) <= 0.05f * WantSecond && bFalling;
				const bool bSettled = L->SettledAt > 0.f && L->Bounces >= 3;
				const bool bJumpInstead = L->bJumpSeen && L->BouncesAtJump == L->BouncesAtPress;
				const bool bOk = Climbs == 0 && !Move->CanGrabLedge() && bWall && bHeights && bSettled && bJumpInstead;
				Judge(TEXT("Rubber"), bOk, FString::Printf(TEXT("ledge grabs %d (want 0); knocked at the wall at %.0f cm/s, back off it at %.0f (want back, about 0.85x); "
					"dropped %.0f cm: bounced back up %.0f then %.0f cm (want %.0f and %.0f, 5 %% each; the first bounce saw a fall of %.0f cm, up at %.0f cm/s), "
					"all %d bounce tops %s cm, lower every time %d, settled %.2f s after the drop (want 3 bounces or more, then settled); "
					"a jump pressed for a landing: jumped %d, bounced %d (want 1, 0)"),
					Climbs, L->TowardWall, L->AwayAfter, Fell, First, Second, WantFirst, WantSecond, L->FirstFall, L->FirstUp, L->Bounces, *Tops, bFalling ? 1 : 0,
					L->SettledAt > 0.f ? L->SettledAt - DropAt : -1.f, L->bJumpSeen ? 1 : 0, L->BouncesAtJump - L->BouncesAtPress));
				Clear();
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
				{
					Flat->ClearTemporaryBlocks();
				}
			});
	}

	// ---- Second Chance (on the dummy), on the main island made a ten-penguin arena (as wide as they get) and put back at
	// every step's end: the first fall into the sea a fish throws it high over the island's highest block and toward its
	// middle, at most 45 degrees off straight up; on the way up it passes through the blocks in its way without stopping
	// (never ends inside one) and comes down on the island, alive. From next to the island's side (with the old checks: two
	// abilities, the second fall a knock-out, one rescue counted), from 15 m out, from under an overhang built at the island's
	// end, and from 120 m out: thrown at 45 degrees anyway and lost.
	{
		enum class ERescueCase : uint8 { Side, Far, Overhang, TooFar };
		struct FRescueIsland
		{
			bool bSaved = false;
			EIslandLayout Layout = EIslandLayout::MovementTest;
			int32 Seed = 0;
			int32 Penguins = 0;
		};
		struct FRescue
		{
			ABlockIsland* Island = nullptr;
			FBox Bounds = FBox(ForceInit);
			float DropX = 0.f;
			float RescuedAt = -1.f;
			FVector Start = FVector::ZeroVector;
			FVector Throw = FVector::ZeroVector;
			float MidX = 0.f;
			float AimX = 0.f;
			float Highest = 0.f;
			bool bTooFarPlanned = false;
			int32 NudgesAtRescue = 0;
			float LastVx = 0.f;
			bool bLastVx = false;
			// the way up
			bool bEverPassing = false;
			int32 InsideFrames = 0;      // frames inside a block while passing through
			bool bTopReached = false;
			float TopAt = -1.f;
			bool bStoppedBelowTop = false;
			int32 SideStops = 0;          // frames its sideways speed dropped more than the air drag allows (a wall in the way)
			float PassEndFeet = -1.e9f;
			float MaxFeet = -1.e9f;
			int32 InsideAfter = 0;        // frames inside a block once the blocks counted again
			// the way down
			float LandedAt = -1.f;
			FVector LandAt = FVector::ZeroVector;
			bool bAliveSampled = false;
			bool bAliveAfter = false;
			bool bStayed = false;         // still standing where it came down (not slid off an edge)
			bool bKnockedOut = false;
			uint8 Reason = 0;
			int32 Shots = 0;
			// the side case: the loadout, the second fall
			int32 Abilities = 0;
			bool bShape = false;
			bool bSecondFall = false;
			uint8 SecondReason = 0;
		};
		const TSharedRef<FRescueIsland> RI = MakeShared<FRescueIsland>();
		static constexpr int32 RescueSeed = 7071;
		static constexpr float DropAboveSea = 400.f;
		const auto AddRescueStep = [this, RI, Judge, Clear, Base, HoldStage, ReleaseStage](const TCHAR* Name, ERescueCase Case, float Duration)
		{
			const TSharedRef<FRescue> L = MakeShared<FRescue>();
			const FString Label = FString(Name).Replace(TEXT("trait_"), TEXT(""));
			const FString ShotBase = FString::Printf(TEXT("70_secondchance_%s"), *Label.Replace(TEXT("SecondChance"), TEXT("")).ToLower());
			AddStep(Name, Duration,
				[this, L, RI, Case, HoldStage]
				{
					*L = FRescue();
					SpawnTarget();
					PrepareBrawl(2400.f, {});
					ABlockIsland* Island = ABlockIsland::Get(GetWorld());
					if (!Island || !Target.IsValid())
					{
						return;
					}
					if (!RI->bSaved)
					{
						RI->bSaved = true;
						RI->Layout = Island->Layout;
						RI->Seed = Island->Seed;
						RI->Penguins = Island->ArenaPenguins;
					}
					Island->ArenaPenguins = PinguMaxPenguins;
					Island->RegenerateArena(RescueSeed);
					L->Island = Island;
					L->Bounds = Island->GetBounds();
					SetTraitNow(Target.Get(), EPinguTrait::SecondChance);
					if (Case == ERescueCase::Side)
					{
						if (UPenguinAbilityComponent* DummyAbilities = Target->GetAbilities())
						{
							FRandomStream Rand(77);
							DummyAbilities->SetAbilities(UPenguinAbilityComponent::RollLoadout(Rand, Target->GetTraits()->GetNumAbilities()));
							const TArray<EPenguinAbility>& Loadout = DummyAbilities->GetAbilities();
							L->Abilities = CountReal(Loadout);
							L->bShape = Loadout.Num() == 3 && Loadout[1] == EPenguinAbility::None;
						}
					}
					const float Cell = Island->CellSize;
					switch (Case)
					{
					case ERescueCase::Side:
						L->DropX = static_cast<float>(L->Bounds.Max.X) + 150.f;
						break;
					case ERescueCase::Far:
						L->DropX = static_cast<float>(L->Bounds.Min.X) - 1500.f;
						break;
					case ERescueCase::TooFar:
						L->DropX = static_cast<float>(L->Bounds.Min.X) - 12000.f;
						break;
					case ERescueCase::Overhang:
					{
						// A shelf of 4 blocks out from the island's right end, level with its top: the dummy falls in under it.
						const int32 EdgeX = Island->WorldToCell(FVector(L->Bounds.Max.X - 0.5f * Cell, 0.f, 0.f)).X;
						const float EdgeWorldX = static_cast<float>(Island->CellToWorld(FIntPoint(EdgeX, 0)).X);
						float EdgeTop = 0.f;
						if (Island->FindGroundBelow(FVector(EdgeWorldX, 0.f, L->Bounds.Max.Z + 10.f), static_cast<float>(L->Bounds.Max.Z - L->Bounds.Min.Z) + 2.f * Cell, EdgeTop))
						{
							const int32 Row = Island->WorldToCell(FVector(EdgeWorldX, 0.f, EdgeTop - 0.5f * Cell)).Y;
							TArray<FIntPoint> Shelf;
							for (int32 Out = 1; Out <= 4; ++Out)
							{
								Shelf.Add(FIntPoint(EdgeX + Out, Row));
							}
							Island->AddBlocks(Shelf, EBlockKind::Rock, 0.f);
							Island->FlushEdits();
						}
						L->Bounds = Island->GetBounds();
						L->DropX = static_cast<float>(Island->CellToWorld(FIntPoint(EdgeX + 3, 0)).X);
						break;
					}
					}
					const FVector Drop(L->DropX, 0.f, Island->SeaLevel + DropAboveSea);
					Target->NetTeleport(Drop, FVector(0.f, 0.f, -300.f), true);
					// The camera from the side: the way from the sea up over the island to its middle.
					const float MidX = static_cast<float>(L->Bounds.GetCenter().X);
					const float LookX = Case == ERescueCase::TooFar ? L->DropX + 2500.f : L->DropX + 0.45f * (MidX - L->DropX);
					const float LookZ = 0.5f * (Island->SeaLevel + static_cast<float>(L->Bounds.Max.Z) + 300.f);
					HoldStage(FVector(LookX, 0.f, LookZ), FMath::Max(3800.f, 0.9f * FMath::Abs(MidX - L->DropX) + 1800.f));
				},
				[this, L, Case, ShotBase](float T)
				{
					if (!Target.IsValid() || !L->Island)
					{
						return;
					}
					const UPenguinMovementComponent* DummyMove = Target->GetPenguinMovement();
					const UPenguinTraitComponent* Traits = Target->GetTraits();
					const UCapsuleComponent* Capsule = Target->GetCapsuleComponent();
					if (!DummyMove || !Traits || !Capsule)
					{
						return;
					}
					const float Dt = GetWorld()->GetDeltaSeconds();
					const FVector Loc = Target->GetActorLocation();
					const float Feet = static_cast<float>(Target->GetFeetLocation().Z);
					// A picture with the held camera moved onto the dummy (a little ahead: the shot is written a frame later).
					const auto TakeShot = [this, &ShotBase, &Loc, DummyMove](const TCHAR* Suffix)
					{
						if (AActor* Stage = LookCamera.Get())
						{
							const FVector At = Loc + DummyMove->Velocity * 0.03f;
							Stage->SetActorLocationAndRotation(FVector(At.X, 3200.f, At.Z), FRotator(0.f, -90.f, 0.f));
						}
						Shot(*(ShotBase + Suffix));
					};
					const bool bFits = L->Island->CanFitCapsule(Loc, Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());
					if (L->RescuedAt < 0.f && Traits->GetEvents().SecondChanceAt >= 0.f)
					{
						const FPinguTraitCounters& C = Traits->GetCounters();
						L->RescuedAt = T;
						L->Start = C.RescueStart;
						L->Throw = C.RescueThrow;
						L->MidX = C.RescueMidX;
						L->AimX = C.RescueLandX;
						L->Highest = C.RescueHighestTop;
						L->bTooFarPlanned = C.bRescueTooFar;
						L->NudgesAtRescue = DummyMove->GetPassThroughNudges();
					}
					if (L->RescuedAt >= 0.f && L->LandedAt < 0.f && !L->bKnockedOut)
					{
						const bool bPassing = DummyMove->IsPassingThroughIsland();
						L->bEverPassing |= bPassing;
						L->MaxFeet = FMath::Max(L->MaxFeet, Feet);
						if (bPassing && !bFits)
						{
							++L->InsideFrames;
						}
						if (!bPassing && !bFits)
						{
							++L->InsideAfter;
						}
						if (L->bEverPassing && !bPassing && L->PassEndFeet < -1.e8f)
						{
							L->PassEndFeet = Feet;
						}
						if (!L->bTopReached)
						{
							if (Feet > L->Highest)
							{
								L->bTopReached = true;
								L->TopAt = T;
							}
							else if (DummyMove->Velocity.Z <= 0.f && T > L->RescuedAt + 0.05f)
							{
								L->bStoppedBelowTop = true;
							}
							const float Vx = FMath::Abs(static_cast<float>(DummyMove->Velocity.X));
							if (L->bLastVx && Vx < L->LastVx - DummyMove->KnockbackAirDrag * Dt * 1.5f - 5.f)
							{
								++L->SideStops;
							}
							L->LastVx = Vx;
							L->bLastVx = T > L->RescuedAt + 0.05f;
						}
						if (L->bTopReached && DummyMove->IsMovingOnGround())
						{
							L->LandedAt = T;
							L->LandAt = Loc;
						}
						// The pictures (the camera on the dummy): out of the sea, over the island's highest block (the blocks count
						// again), at the top, then down on the island (or lost).
						if (L->Shots == 0 && T > L->RescuedAt + 0.08f)
						{
							L->Shots = 1;
							TakeShot(TEXT("_1_sea"));
						}
						if (L->Shots == 1 && L->PassEndFeet > -1.e8f)
						{
							L->Shots = 2;
							TakeShot(TEXT("_2_over"));
						}
						if (L->Shots == 2 && L->bTopReached && DummyMove->Velocity.Z <= 0.f)
						{
							L->Shots = 3;
							TakeShot(TEXT("_3_top"));
						}
					}
					if (L->RescuedAt >= 0.f && L->LandedAt < 0.f && !L->bKnockedOut && Target->IsKnockedOut())
					{
						L->bKnockedOut = true;
						L->Reason = static_cast<uint8>(Target->GetKOReason());
						if (L->Shots == 3)
						{
							L->Shots = 4;
							TakeShot(TEXT("_4_lost"));
						}
					}
					if (L->LandedAt >= 0.f)
					{
						L->InsideAfter += bFits || L->bAliveSampled ? 0 : 1;
						if (L->Shots == 3 && T > L->LandedAt + 0.25f)
						{
							L->Shots = 4;
							TakeShot(TEXT("_4_landed"));
						}
						if (!L->bAliveSampled && T > L->LandedAt + 1.5f)
						{
							L->bAliveSampled = true;
							L->bAliveAfter = Target->IsAlive();
							L->bStayed = L->bAliveAfter && DummyMove->IsMovingOnGround() && FMath::Abs(Loc.X - L->LandAt.X) < 150.f
								&& FMath::Abs(Loc.Z - L->LandAt.Z) < 20.f;
						}
					}
					// The side case: into the sea again - a knock-out (once a round).
					if (Case == ERescueCase::Side && L->bAliveSampled && L->bAliveAfter && !L->bSecondFall)
					{
						L->bSecondFall = true;
						Target->NetTeleport(FVector(static_cast<float>(L->Bounds.Max.X) + 2000.f, 0.f, L->Island->SeaLevel + DropAboveSea), FVector(0.f, 0.f, -300.f), true);
					}
					if (L->bSecondFall && Target->IsKnockedOut())
					{
						L->SecondReason = static_cast<uint8>(Target->GetKOReason());
					}
				},
				[this, L, RI, Case, Judge, Clear, Base, ReleaseStage, Label]
				{
					using namespace PinguTraitTuning;
					ReleaseStage();
					const UPenguinTraitComponent* Traits = Target.IsValid() ? Target->GetTraits() : nullptr;
					const UPenguinMovementComponent* DummyMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
					const int32 Rescues = Traits ? Traits->GetCounters().SecondChances : 0;
					const int32 Nudges = DummyMove ? DummyMove->GetPassThroughNudges() - L->NudgesAtRescue : 0;
					const float Angle = FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(static_cast<float>(L->Throw.X)), FMath::Max(static_cast<float>(L->Throw.Z), 1.f)));
					const float ToMiddle = L->MidX - static_cast<float>(L->Start.X);
					const bool bTowardMiddle = FMath::Abs(ToMiddle) < 50.f || L->Throw.X * ToMiddle > 0.f;
					const bool bThrown = L->RescuedAt >= 0.f && L->Throw.Z > 2000.f && Angle <= SecondChanceMaxAngle + 0.1f && bTowardMiddle;
					const bool bUp = L->bEverPassing && L->bTopReached && !L->bStoppedBelowTop && L->SideStops == 0 && L->PassEndFeet > L->Highest
						&& L->MaxFeet >= L->Highest + 0.9f * SecondChanceApexAbove && L->InsideAfter == 0;
					const bool bOnIsland = L->LandedAt >= 0.f && L->LandAt.X >= L->Bounds.Min.X && L->LandAt.X <= L->Bounds.Max.X && L->bAliveAfter && L->bStayed;
					bool bOk = false;
					FString Extra;
					switch (Case)
					{
					case ERescueCase::Side:
						bOk = bThrown && bUp && bOnIsland && L->InsideFrames > 0 && L->Abilities == 2 && L->bShape && L->bSecondFall
							&& L->SecondReason == static_cast<uint8>(EPenguinKOReason::Sea) && Rescues == 1;
						Extra = FString::Printf(TEXT("; abilities %d in slots 0 and 2 %d (want 2, 1); the second fall: knocked out %s (want Sea), rescues counted %d (want 1)"),
							L->Abilities, L->bShape ? 1 : 0, *UEnum::GetValueAsString(static_cast<EPenguinKOReason>(L->SecondReason)), Rescues);
						break;
					case ERescueCase::Overhang:
						bOk = bThrown && bUp && bOnIsland && L->InsideFrames > 0;
						break;
					case ERescueCase::Far:
						bOk = bThrown && bUp && bOnIsland;
						break;
					case ERescueCase::TooFar:
						bOk = L->RescuedAt >= 0.f && FMath::Abs(Angle - SecondChanceMaxAngle) <= 0.5f && bTowardMiddle && L->bTooFarPlanned && L->LandedAt < 0.f
							&& L->bKnockedOut && L->Reason == static_cast<uint8>(EPenguinKOReason::Sea);
						Extra = FString::Printf(TEXT("; too far planned %d, knocked out %s (want Sea, never on the island)"), L->bTooFarPlanned ? 1 : 0,
							*UEnum::GetValueAsString(static_cast<EPenguinKOReason>(L->Reason)));
						break;
					}
					Judge(*Label, bOk, FString::Printf(TEXT("from the sea at x %.0f (island x %.0f to %.0f, its highest block's top %.0f): thrown %d, %.0f cm/s up, %.0f sideways = %.1f deg "
						"off straight up toward the middle x %.0f %d (want 45 at most); through the blocks %d (%d frames inside them), on up without a stop %d (sideways stops %d), "
						"the blocks back with its feet at %.0f, top of its feet %.0f (want %.0f or more), inside a block afterwards %d frames, nudged out %d; landed %d at x %.0f "
						"(aimed at x %.0f), alive and standing there 1.5 s later %d %d%s"),
						L->DropX, L->Bounds.Min.X, L->Bounds.Max.X, L->Highest, L->RescuedAt >= 0.f ? 1 : 0, L->Throw.Z, L->Throw.X, Angle, L->MidX, bTowardMiddle ? 1 : 0,
						L->bEverPassing ? 1 : 0, L->InsideFrames, !L->bStoppedBelowTop && L->bTopReached ? 1 : 0, L->SideStops, L->PassEndFeet, L->MaxFeet,
						L->Highest + 0.9f * SecondChanceApexAbove, L->InsideAfter, Nudges, L->LandedAt >= 0.f ? 1 : 0, L->LandAt.X, L->AimX, L->bAliveAfter ? 1 : 0, L->bStayed ? 1 : 0, *Extra));
					Clear();
					// The island as it was (the course), the dummy back on the strip.
					if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()); Island && RI->bSaved)
					{
						Island->ClearTemporaryBlocks();
						Island->Layout = RI->Layout;
						Island->Seed = RI->Seed;
						Island->ArenaPenguins = RI->Penguins;
						Island->Rebuild();
						RI->bSaved = false;
					}
					ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
				});
		};
		AddRescueStep(TEXT("trait_SecondChanceSide"), ERescueCase::Side, 6.5f);
		AddRescueStep(TEXT("trait_SecondChanceFar"), ERescueCase::Far, 4.5f);
		AddRescueStep(TEXT("trait_SecondChanceOverhang"), ERescueCase::Overhang, 4.5f);
		AddRescueStep(TEXT("trait_SecondChanceTooFar"), ERescueCase::TooFar, 5.f);
	}

	// ---- Second Chance and an ability bubble: the bubble replaces one of its two abilities (slot 0); the empty slot stays
	// empty - it always has exactly two.
	{
		struct FBubble
		{
			TArray<EPenguinAbility> Before;
			EPenguinAbility Bubble = EPenguinAbility::None;
			float TakenAt = -1.f;
		};
		const TSharedRef<FBubble> L = MakeShared<FBubble>();
		AddStep(TEXT("trait_SecondChancePickup"), 2.2f,
			[this, L, Prepare]
			{
				*L = FBubble();
				Prepare(1800.f, {}, EPinguTrait::SecondChance, EPinguTrait::None);
				FRandomStream Rand(99);
				UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
				Abilities->SetAbilities(UPenguinAbilityComponent::RollLoadout(Rand, Penguin->GetTraits()->GetNumAbilities()));
				L->Before = Abilities->GetAbilities();
				for (const EPenguinAbility Candidate : { EPenguinAbility::Noot, EPenguinAbility::Gust, EPenguinAbility::IceGun })
				{
					if (!L->Before.Contains(Candidate))
					{
						L->Bubble = Candidate;
						break;
					}
				}
				TestPickup = APinguPickup::Spawn(GetWorld(), L->Bubble, Penguin->GetActorLocation() + FVector(0.f, 0.f, 260.f));
			},
			[this, L](float T)
			{
				if (L->TakenAt < 0.f && !TestPickup.IsValid())
				{
					L->TakenAt = T;
				}
			},
			[this, L, Judge, Clear]
			{
				const TArray<EPenguinAbility> After = Penguin->GetAbilities()->GetAbilities();
				const bool bOk = L->TakenAt >= 0.f && After.Num() == 3 && L->Before.Num() == 3 && After[1] == EPenguinAbility::None && After[0] == L->Bubble
					&& After[2] == L->Before[2] && CountReal(After) == 2;
				Judge(TEXT("SecondChancePickup"), bOk, FString::Printf(TEXT("%s, a %s bubble taken %d -> %s (want it in slot 0, slot 1 still empty: two abilities)"),
					*DescribeLoadout(L->Before), UPenguinAbilityComponent::GetInfo(L->Bubble).Name, L->TakenAt >= 0.f ? 1 : 0, *DescribeLoadout(After)));
				Clear();
				APinguPickup::DestroyAll(GetWorld());
			});
	}

	// ---- Leech: cooldowns x1.5, and every hit it lands takes 2 s off all of them.
	{
		struct FLeech
		{
			float ShellAt = -1.f;
			float ShellLength = 0.f;
			float GustLength = 0.f;
			int32 DummyHits = 0;
			float Cut = 0.f;
			float ShellLeft = 0.f;
		};
		const TSharedRef<FLeech> L = MakeShared<FLeech>();
		AddStep(TEXT("trait_Leech"), 1.6f,
			[this, L, Prepare]
			{
				*L = FLeech();
				Prepare(220.f, { EPenguinAbility::Gust, EPenguinAbility::IceShell }, EPinguTrait::Leech, EPinguTrait::None);
				L->DummyHits = Target.IsValid() && Target->GetPenguinMovement() ? Target->GetPenguinMovement()->HitEvents : 0;
			},
			[this, L](float T)
			{
				UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
				if (!bFlag && T > 0.25f)
				{
					bFlag = true;
					Penguin->SetAimPoint(Penguin->GetAimOrigin());
					Penguin->ActivateAbility(1); // the shell: its cooldown runs (no hit)
					L->ShellAt = T;
					L->ShellLength = Abilities->GetCooldownLength(1);
				}
				if (MarkA < 0.f && T > 0.6f && Target.IsValid())
				{
					MarkA = T;
					Penguin->SetAimPoint(Target->GetAimOrigin());
					Penguin->ActivateAbility(0); // the gust: a hit
					L->GustLength = Abilities->GetCooldownLength(0);
				}
				if (L->ShellAt >= 0.f && T > 1.4f && MarkB < 0.f)
				{
					MarkB = T;
					// What the shell's cooldown should have left by now, and what it has.
					L->ShellLeft = Abilities->GetCooldownRemaining(1);
					L->Cut = (L->ShellLength - (T - L->ShellAt)) - L->ShellLeft;
				}
			},
			[this, L, Judge, Clear]
			{
				const UPenguinMovementComponent* DummyMove = Target.IsValid() ? Target->GetPenguinMovement() : nullptr;
				const int32 Hits = DummyMove ? DummyMove->HitEvents - L->DummyHits : 0;
				const float Drained = Penguin->GetTraits()->GetCounters().LeechSeconds;
				const bool bOk = IsNear(L->ShellLength, 4.5f * 1.5f, 0.05f) && IsNear(L->GustLength, 3.5f * 1.5f, 0.05f) && Hits == 1 && IsNear(L->Cut, 2.f, 0.15f);
				Judge(TEXT("Leech"), bOk, FString::Printf(TEXT("cooldowns: shell %.2f s, gust %.2f s (want 6.75 5.25: x1.5); the gust hit %d time(s), the shell's cooldown then %.2f s shorter (want 2), %.2f s left; drained %.1f s in all"),
					L->ShellLength, L->GustLength, Hits, L->Cut, L->ShellLeft, Drained));
				Clear();
			});
	}

	// ---- Gambler: 100 noots point blank - about 60 % hit twice as hard, the 40 % rest fizzle (the dummy untouched). The roll is its own
	// random stream, seeded from the trait's serial (it differs with the steps that ran before): over 100 uses the share
	// stays within 45 to 75 % (three standard deviations), and the numbers themselves are checked exactly: the table's
	// double chance 0.60, the PINGUPEDIA's stat lines 60 % / 40 %.
	{
		struct FGamble
		{
			float NormalPush = 0.f;
			int32 Uses = 0;
			int32 Doubles = 0;
			int32 Fizzles = 0;
			int32 Plain = 0;
			int32 FizzleHits = 0;
			int32 DoubleMisses = 0;
			float DoublePushSum = 0.f;
		};
		const TSharedRef<FGamble> L = MakeShared<FGamble>();
		static constexpr int32 GambleUses = 100;
		static constexpr float GambleEvery = 0.06f;
		/** The dummy stands this far ahead: the honk's mouth is inside its capsule (a noot's push falls off with the distance, and
		 *  the reach grows with a double's strength, so point blank keeps the pushes to a clean x2). */
		static constexpr float GambleDistance = 55.f;
		AddStep(TEXT("trait_Gambler"), 0.3f + (GambleUses + 1) * GambleEvery,
			[this, L, Prepare]
			{
				*L = FGamble();
				Prepare(GambleDistance, { EPenguinAbility::Noot }, EPinguTrait::None, EPinguTrait::None);
			},
			[this, L, Base](float T)
			{
				// One noot every GambleEvery seconds from the base at the dummy (ready again each time); the first without the trait.
				const int32 Due = FMath::Min(GambleUses + 1, FMath::FloorToInt((T - 0.1f) / GambleEvery) + 1);
				if (Due <= L->Uses || !Target.IsValid())
				{
					return;
				}
				UPenguinTraitComponent* Traits = Penguin->GetTraits();
				const UPenguinMovementComponent* DummyMove = Target->GetPenguinMovement();
				Teleport(Base);
				ResetTarget(Base + FVector(GambleDistance, 0.f, 0.f));
				Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Noot });
				Penguin->SetAimPoint(Target->GetAimOrigin());
				const int32 HitsBefore = DummyMove->HitEvents;
				const uint8 SerialBefore = Traits->GetEvents().GambleSerial;
				Penguin->ActivateAbility(0);
				const bool bHit = DummyMove->HitEvents != HitsBefore;
				const float Push = static_cast<float>(DummyMove->Velocity.X);
				if (L->Uses == 0)
				{
					L->NormalPush = Push;
					SetTraitNow(Penguin.Get(), EPinguTrait::Gambler);
				}
				else if (Traits->GetEvents().GambleSerial == SerialBefore)
				{
					++L->Plain;
				}
				else if (Traits->GetEvents().bGambleDouble)
				{
					++L->Doubles;
					L->DoubleMisses += bHit ? 0 : 1;
					L->DoublePushSum += bHit ? Push : 0.f;
				}
				else
				{
					++L->Fizzles;
					L->FizzleHits += bHit ? 1 : 0;
				}
				++L->Uses;
			},
			[this, L, Judge, Clear]
			{
				const int32 Rolled = L->Doubles + L->Fizzles;
				const float Share = Rolled > 0 ? static_cast<float>(L->Doubles) / Rolled : 0.f;
				const int32 Hits = L->Doubles - L->DoubleMisses;
				const float DoubleStrength = Hits > 0 ? Ratio(L->DoublePushSum / Hits, L->NormalPush) : 0.f;
				const FPinguTraitCounters& Counters = Penguin->GetTraits()->GetCounters();
				// The numbers the card shows: the table's 0.60 and the stat lines 60 % (up) and 40 % (down).
				TArray<FPinguStatLine> Lines;
				PinguTraits::GetStatLines(EPinguTrait::Gambler, Lines);
				float CardDouble = -1.f;
				float CardFizzle = -1.f;
				for (const FPinguStatLine& Line : Lines)
				{
					CardDouble = Line.Kind == EPinguStatKind::Chance ? Line.Value : CardDouble;
					CardFizzle = Line.Kind == EPinguStatKind::FizzleChance ? Line.Value : CardFizzle;
				}
				const bool bCard = IsNear(PinguTraitTuning::GambleDoubleChance, 0.6f, 0.0005f) && IsNear(CardDouble, 0.6f, 0.0005f) && IsNear(CardFizzle, 0.4f, 0.0005f);
				const bool bOk = Rolled == GambleUses && L->Plain == 0 && Share >= 0.45f && Share <= 0.75f && L->FizzleHits == 0 && L->DoubleMisses == 0
					&& IsNear(DoubleStrength, 2.f, 0.1f) && bCard;
				Judge(TEXT("Gambler"), bOk, FString::Printf(TEXT("%d noots: doubles %d (%.0f %%, want about 60, 45 to 75), fizzles %d, plain %d (want 0); fizzles that still hit %d (want 0), doubles that missed %d; "
					"a double pushes x%.2f (want 2); counted: doubles %d, fizzles %d; the card says double %.0f %% / fizzle %.0f %% (want 60 / 40)"),
					L->Uses - 1, L->Doubles, 100.f * Share, L->Fizzles, L->Plain, L->FizzleHits, L->DoubleMisses, DoubleStrength, Counters.Doubles, Counters.Fizzles,
					100.f * CardDouble, 100.f * CardFizzle));
				Clear();
			});
	}

	// ---- Speedster: runs 1.5x as fast (840), dashes 1.5x as fast; slippery feet: from full speed it takes about 8x as far
	// to stop (7-9x).
	{
		struct FSpeed
		{
			int32 Stage = 0;
			float StageAt = 0.f;
			float FromX = 0.f;
			float NormalSpeed = 0.f;
			float NormalStop = -1.f;
			float TopSpeed = 0.f;
			float FastSpeed = 0.f;
			float FastStop = -1.f;
			float DashPeak = 0.f;
		};
		const TSharedRef<FSpeed> L = MakeShared<FSpeed>();
		AddStep(TEXT("trait_Speedster"), 5.8f,
			[this, L, Prepare, Base]
			{
				*L = FSpeed();
				Prepare(2400.f, {}, EPinguTrait::None, EPinguTrait::None);
				ResetTarget(Base + FVector(3200.f, 0.f, 0.f)); // (well out of the way of the runs)
				Teleport(FVector(FlatIslandX - 2600.f, 0.f, 62.f));
			},
			[this, L](float T)
			{
				FSpeed& S = *L;
				const float X = static_cast<float>(Location().X);
				const float Vx = static_cast<float>(Move->Velocity.X);
				switch (S.Stage)
				{
				case 0: // without the trait: run, let go, how far to stop
					VirtualInput = FVector2D(1.f, 0.f);
					if (T > 0.9f)
					{
						S.NormalSpeed = Vx;
						S.FromX = X;
						VirtualInput = FVector2D::ZeroVector;
						S.Stage = 1;
					}
					break;
				case 1:
					if (FMath::Abs(Vx) < 5.f)
					{
						S.NormalStop = X - S.FromX;
						SetTraitNow(Penguin.Get(), EPinguTrait::Speedster);
						S.Stage = 2;
						S.StageAt = T;
					}
					break;
				case 2: // Speedster: the same
					VirtualInput = FVector2D(1.f, 0.f);
					if (T > S.StageAt + 0.9f)
					{
						S.TopSpeed = FMath::Max(S.TopSpeed, Vx);
					}
					if (T > S.StageAt + 1.3f)
					{
						S.FastSpeed = Vx;
						S.FromX = X;
						VirtualInput = FVector2D::ZeroVector;
						S.Stage = 3;
					}
					break;
				case 3:
					if (FMath::Abs(Vx) < 5.f)
					{
						S.FastStop = X - S.FromX;
						S.Stage = 4;
						S.StageAt = T;
					}
					break;
				case 4: // a dash to the right
					VirtualInput = FVector2D(1.f, 0.f);
					if (T > S.StageAt + 0.15f)
					{
						Move->DashPressed();
						S.Stage = 5;
						S.StageAt = T;
					}
					break;
				case 5:
					if (Move->IsDashing())
					{
						S.DashPeak = FMath::Max(S.DashPeak, FMath::Abs(Vx));
					}
					if (T > S.StageAt + 0.5f)
					{
						VirtualInput = FVector2D::ZeroVector;
						S.Stage = 6;
					}
					break;
				default:
					break;
				}
			},
			[this, L, Judge, Clear]
			{
				const FSpeed& S = *L;
				const float StopRatio = S.NormalStop > 1.f ? S.FastStop / S.NormalStop : 0.f;
				const float Dash = Ratio(S.DashPeak, Move->DashSpeed);
				const bool bOk = IsNear(S.TopSpeed, Move->RunSpeed * 1.5f, 20.f) && IsNear(Dash, 1.5f, 0.08f) && StopRatio >= 7.f && StopRatio <= 9.f;
				Judge(TEXT("Speedster"), bOk, FString::Printf(TEXT("runs %.0f cm/s (want %.0f), dash %.0f cm/s = x%.2f (want 1.5); stopping from %.0f cm/s took %.0f cm, without the trait from %.0f: %.0f cm (x%.1f, want 7 to 9)"),
					S.TopSpeed, Move->RunSpeed * 1.5f, S.DashPeak, Dash, S.FastSpeed, S.FastStop, S.NormalSpeed, S.NormalStop, StopRatio));
				Clear();
			});
	}

	// ---- Hot Feet: frost and ice bolts do nothing; the block it stands on melts after a second; ice it walks into melts.
	{
		struct FHot
		{
			bool bFrozen = true;
			bool bSlips = true;
			float StandAt = -1.f;
			FIntPoint Under = FIntPoint(0, 0);
			EBlockKind UnderKind = EBlockKind::None;
			float MeltAfter = -1.f;
			bool bWallBuilt = false;
			FIntPoint Ice = FIntPoint(0, 0);
			float WalkAt = -1.f;
			float IceGoneAfter = -1.f;
			int32 IceLeft = -1;
		};
		const TSharedRef<FHot> L = MakeShared<FHot>();
		AddStep(TEXT("trait_HotFeet"), 4.4f,
			[this, L, Prepare, Base]
			{
				*L = FHot();
				Prepare(2400.f, {}, EPinguTrait::HotFeet, EPinguTrait::None);
				// Frost and an ice bolt from the dummy.
				FPenguinHit Frost;
				Frost.Instigator = Target.Get();
				Frost.Source = EPenguinAbility::FrostBreath;
				Frost.Freeze = 2.3f;
				Frost.Stun = 0.f;
				Frost.Location = Location();
				Penguin->ApplyPenguinHit(Frost);
				FPenguinHit Bolt;
				Bolt.Instigator = Target.Get();
				Bolt.Source = EPenguinAbility::IceGun;
				Bolt.Slip = 6.f;
				Bolt.Stun = 0.f;
				Bolt.Location = Location();
				Penguin->ApplyPenguinHit(Bolt);
				L->bFrozen = Move->IsFrozen();
				L->bSlips = Move->IsSlippery();
				Teleport(Base);
			},
			[this, L, Base](float T)
			{
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (!Flat)
				{
					return;
				}
				// Standing still: the block under the feet melts.
				if (L->StandAt < 0.f && T > 0.05f && Move->IsMovingOnGround())
				{
					L->StandAt = T;
					L->Under = Flat->WorldToCell(Penguin->GetFeetLocation() - FVector(0.f, 0.f, 50.f));
					L->UnderKind = Flat->GetBlock(L->Under.X, L->Under.Y);
				}
				if (L->StandAt >= 0.f && L->MeltAfter < 0.f && Flat->GetBlock(L->Under.X, L->Under.Y) == EBlockKind::None)
				{
					L->MeltAfter = T - L->StandAt;
				}
				// An ice column 1.5 m ahead on fresh snow: walk into it.
				if (!L->bWallBuilt && T > 2.2f)
				{
					L->bWallBuilt = true;
					const FVector Spot = Base + FVector(-1500.f, 0.f, 0.f);
					Teleport(Spot);
					AddWall(Flat, Spot.X + 150.f, 3, 1, EBlockKind::Ice, 10.f);
					L->Ice = Flat->WorldToCell(FVector(Spot.X + 150.f, 0.f, 10.f));
					L->WalkAt = T;
				}
				if (L->bWallBuilt)
				{
					VirtualInput = FVector2D(1.f, 0.f);
					if (L->IceGoneAfter < 0.f && Flat->GetBlock(L->Ice.X, L->Ice.Y) != EBlockKind::Ice)
					{
						L->IceGoneAfter = T - L->WalkAt;
					}
					L->IceLeft = 0;
					for (int32 Row = 0; Row < 3; ++Row)
					{
						L->IceLeft += Flat->GetBlock(L->Ice.X, L->Ice.Y + Row) == EBlockKind::Ice ? 1 : 0;
					}
				}
			},
			[this, L, Judge, Clear, Base]
			{
				const bool bOk = !L->bFrozen && !L->bSlips && L->MeltAfter >= 0.95f && L->MeltAfter <= 1.25f && L->IceGoneAfter >= 0.f && L->IceGoneAfter < 1.2f;
				Judge(TEXT("HotFeet"), bOk, FString::Printf(TEXT("frost: frozen %d, ice bolt: slips %d (want 0 0); the %s block under it melted after %.2f s (want 1.0 - 1.2); an ice column 1.5 m ahead: its first block gone %.2f s after it set off "
					"(want under 1.2), ice blocks left %d of 3"), L->bFrozen ? 1 : 0, L->bSlips ? 1 : 0, *UEnum::GetValueAsString(L->UnderKind), L->MeltAfter, L->IceGoneAfter, L->IceLeft));
				Clear();
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
				{
					Flat->ClearTemporaryBlocks();
				}
			});
	}

	// ---- Mirror: the first shot that hits it goes back to its thrower (the dummy is hit, not us); a second one within 6 s
	// hits us; 6 s after the first it sends one back again.
	{
		struct FMirror
		{
			int32 OwnHits = 0;
			int32 DummyHits = 0;
			int32 Phase = 0;
			float PhaseAt = 0.f;
			float ReflectedAt = -1.f;
			float Recharge = -1.f;
			int32 Back[3] = { -1, -1, -1 };
			int32 OnUs[3] = { -1, -1, -1 };
		};
		const TSharedRef<FMirror> L = MakeShared<FMirror>();
		AddStep(TEXT("trait_Mirror"), 8.2f,
			[this, L, Prepare]
			{
				*L = FMirror();
				Prepare(700.f, {}, EPinguTrait::Mirror, EPinguTrait::None);
			},
			[this, L, Base](float T)
			{
				if (!Target.IsValid())
				{
					return;
				}
				FMirror& M = *L;
				const UPenguinMovementComponent* DummyMove = Target->GetPenguinMovement();
				const UPenguinTraitComponent* Traits = Penguin->GetTraits();
				const auto Throw = [this, &M, DummyMove, Base]()
				{
					Teleport(Base);
					ResetTarget(Base + FVector(700.f, 0.f, 0.f));
					M.OwnHits = Move->HitEvents;
					M.DummyHits = DummyMove->HitEvents;
					const FVector From = Target->GetAimOrigin() - FVector(45.f, 0.f, 0.f);
					const FVector Dir = (Penguin->GetAimOrigin() + FVector(0.f, 0.f, 40.f) - From).GetSafeNormal();
					APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Target.Get(), From, Dir * 2100.f);
				};
				// Shots at 0.3 s, 1.6 s and 6.4 s after the first one came back (each judged 0.9 s later).
				const float Times[3] = { 0.3f, 1.6f, M.ReflectedAt >= 0.f ? M.ReflectedAt + 6.3f : 1000.f };
				for (int32 Index = 0; Index < 3; ++Index)
				{
					if (M.Phase == Index * 2 && T > Times[Index])
					{
						Throw();
						M.Phase = Index * 2 + 1;
						M.PhaseAt = T;
					}
					if (M.Phase == Index * 2 + 1)
					{
						if (Index == 0 && M.ReflectedAt < 0.f && Traits->GetEvents().MirrorReadyAt > FlowNow(this) + 1.f)
						{
							M.ReflectedAt = T;
							M.Recharge = Traits->GetEvents().MirrorReadyAt - FlowNow(this);
						}
						if (T > M.PhaseAt + 0.9f)
						{
							M.Back[Index] = DummyMove->HitEvents - M.DummyHits;
							M.OnUs[Index] = Move->HitEvents - M.OwnHits;
							M.Phase = Index * 2 + 2;
						}
					}
				}
			},
			[this, L, Judge, Clear]
			{
				const FMirror& M = *L;
				const int32 Reflections = Penguin->GetTraits()->GetCounters().Reflections;
				const bool bOk = M.Back[0] == 1 && M.OnUs[0] == 0 && M.OnUs[1] == 1 && M.Back[1] == 0 && M.Back[2] == 1 && M.OnUs[2] == 0 && IsNear(M.Recharge, 6.f, 0.2f);
				Judge(TEXT("Mirror"), bOk, FString::Printf(TEXT("first shot: back on its thrower %d, on us %d (want 1 0); one 1.3 s later: on us %d, back %d (want 1 0); one 6 s after the first: back %d, on us %d (want 1 0); "
					"ready again %.2f s after the first (want 6); reflections counted %d"), M.Back[0], M.OnUs[0], M.OnUs[1], M.Back[1], M.Back[2], M.OnUs[2], M.Recharge, Reflections));
				Clear();
			});
	}

	// ---- Sticky: a ledge 2 blocks above its feet grabbed from the wall (a normal penguin reaches 1.5 blocks: its reach up is
	// 1.7x); any wall touched in the air holds it - 5 s with no input and not a centimetre down; the stick away from the wall
	// lets go at once; down held slides it down at 250 cm/s, up does nothing; jump is the wall jump; a hit knocks it off; no
	// belly slide.
	{
		struct FSticky
		{
			int32 ClimbsAtStart = 0;
			bool bClimbStarted = false;
			// hanging with no input
			bool bHangSet = false;
			float HangFrom = -1.f;
			float HangZ = 0.f;
			float HangDrift = 0.f;
			int32 HangLostFrames = 0;
			float HangLongest = 0.f;
			// pressing away
			bool bAwayPressed = false;
			int32 AwayFrames = -1;
			float AwayX = 0.f;
			float AwayMoved = 0.f;
			// down and up
			bool bSlideSet = false;
			float SlideFromZ = 0.f;
			float SlideFromT = -1.f;
			float SlideSpeed = 0.f;
			bool bSlideStuck = true;
			float UpFromZ = 0.f;
			float UpMoved = 0.f;
			bool bUpStuck = true;
			// the wall jump
			int32 WallJumpsBefore = 0;
			bool bWallJumped = false;
			float WallJumpVx = 0.f;
			float WallJumpVz = 0.f;
			// a hit
			bool bHitSet = false;
			bool bHitDone = false;
			bool bStuckBeforeHit = false;
			bool bKnockedOff = false;
			// the belly slide
			bool bSlideTried = false;
			int32 SlidesBefore = 0;
			bool bSlid = false;
		};
		const TSharedRef<FSticky> L = MakeShared<FSticky>();
		AddStep(TEXT("trait_Sticky"), 12.f,
			[this, L, Prepare, Base]
			{
				*L = FSticky();
				Prepare(2400.f, {}, EPinguTrait::Sticky, EPinguTrait::None);
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				AddWall(Flat, Base.X + 140.f, 4, 3, EBlockKind::Rock, 14.f);
				AddWall(Flat, Base.X - 1500.f + 140.f, 10, 2, EBlockKind::Rock, 14.f);
				L->ClimbsAtStart = Move->ClimbEvents;
				// Against the 4 block wall in the air, the feet 2 blocks up (its top 2 blocks above them): falling, holding toward it.
				if (Flat)
				{
					const FVector Foot = Flat->CellToWorld(Flat->WorldToCell(FVector(Base.X + 140.f, 0.f, 10.f)));
					const float Face = static_cast<float>(Foot.X) - Flat->CellSize * 0.5f;
					const float Radius = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
					const float Half = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
					const float FloorTop = static_cast<float>(Foot.Z) - Flat->CellSize * 0.5f;
					Teleport(FVector(Face - Radius - 4.f, 0.f, FloorTop + 2.f * Flat->CellSize + Half + 4.f));
				}
				VirtualInput = FVector2D(1.f, 0.f);
			},
			[this, L, Base, HoldStage](float T)
			{
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				// Next to the 10 block wall's face (on its left), its feet Feet cm over the floor, standing still in the air.
				const auto PutAtTallWall = [this, Flat, Base](float Feet)
				{
					if (!Flat)
					{
						return;
					}
					const FVector Foot = Flat->CellToWorld(Flat->WorldToCell(FVector(Base.X - 1500.f + 140.f, 0.f, 10.f)));
					const float Face = static_cast<float>(Foot.X) - Flat->CellSize * 0.5f;
					const float Radius = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
					const float Half = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
					const float FloorTop = static_cast<float>(Foot.Z) - Flat->CellSize * 0.5f;
					Teleport(FVector(Face - Radius - 4.f, 0.f, FloorTop + Feet + Half + 2.f));
				};
				const float Z = static_cast<float>(Location().Z);
				// A press against the 4 block wall, its top 2 blocks above the feet: the ledge grab pulls us up.
				if (T < 1.8f)
				{
					VirtualInput = FVector2D(1.f, 0.f);
					if (T > 0.12f && CountA == 0)
					{
						CountA = 1;
						Move->JumpPressed();
					}
					if (T > 0.25f && CountA == 1)
					{
						CountA = 2;
						Move->JumpReleased();
					}
				}
				// 5 s next to the tall wall in the air, no input at all: it sticks and does not move.
				if (!L->bHangSet && T > 2.f)
				{
					L->bHangSet = true;
					Move->JumpReleased();
					VirtualInput = FVector2D::ZeroVector;
					PutAtTallWall(400.f);
					HoldStage(FVector(Location().X - 100.f, 0.f, 520.f), 2300.f);
				}
				if (L->bHangSet && T < 7.f)
				{
					VirtualInput = FVector2D::ZeroVector;
					if (Move->IsWallClinging())
					{
						if (L->HangFrom < 0.f)
						{
							L->HangFrom = T;
							L->HangZ = Z;
						}
						L->HangLongest = T - L->HangFrom;
						L->HangDrift = FMath::Max(L->HangDrift, FMath::Abs(Z - L->HangZ));
					}
					else if (L->HangFrom >= 0.f)
					{
						++L->HangLostFrames;
					}
					if (CountB == 0 && T > 4.5f)
					{
						CountB = 1;
						Shot(TEXT("70_sticky_1_hang"));
					}
				}
				// The stick away from the wall: it lets go within a frame or two (and falls away from it).
				if (!L->bAwayPressed && T >= 7.f)
				{
					L->bAwayPressed = true;
					L->AwayFrames = 0;
					L->AwayX = static_cast<float>(Location().X);
				}
				if (L->bAwayPressed && T < 7.4f)
				{
					VirtualInput = FVector2D(-1.f, 0.f);
					if (Move->IsWallClinging() && L->AwayFrames >= 0)
					{
						++L->AwayFrames;
					}
					else if (L->AwayFrames >= 0)
					{
						L->AwayFrames = -L->AwayFrames - 1; // done: -(frames still stuck) - 1
					}
					L->AwayMoved = L->AwayX - static_cast<float>(Location().X);
				}
				// Back on the wall; down held: a slow slide; up held: nothing.
				if (!L->bSlideSet && T > 7.5f)
				{
					L->bSlideSet = true;
					VirtualInput = FVector2D::ZeroVector;
					PutAtTallWall(650.f);
				}
				if (L->bSlideSet && T > 7.7f && T < 8.7f)
				{
					VirtualInput = FVector2D(0.f, -1.f);
					if (L->SlideFromT < 0.f)
					{
						L->SlideFromT = T;
						L->SlideFromZ = Z;
					}
					L->bSlideStuck &= Move->IsWallClinging();
					L->SlideSpeed = (L->SlideFromZ - Z) / FMath::Max(T - L->SlideFromT, 0.01f);
					if (CountB == 1 && T > 8.2f)
					{
						CountB = 2;
						Shot(TEXT("70_sticky_2_slide"));
					}
				}
				if (L->bSlideSet && T >= 8.7f && T < 9.2f)
				{
					VirtualInput = FVector2D(0.f, 1.f);
					if (L->UpFromZ == 0.f)
					{
						L->UpFromZ = Z;
					}
					L->bUpStuck &= Move->IsWallClinging();
					L->UpMoved = FMath::Max(L->UpMoved, FMath::Abs(Z - L->UpFromZ));
				}
				// Jump: the wall jump (the ledge is 5 blocks up: out of reach), away from the wall and up.
				if (L->bSlideSet && T >= 9.2f && CountA == 2)
				{
					CountA = 3;
					VirtualInput = FVector2D::ZeroVector;
					L->WallJumpsBefore = Move->WallJumpEvents;
				}
				if (CountA == 3 && T > 9.3f)
				{
					CountA = 4;
					Move->JumpPressed();
				}
				if (CountA == 4 && !L->bWallJumped && Move->WallJumpEvents != L->WallJumpsBefore)
				{
					L->bWallJumped = true;
					L->WallJumpVx = static_cast<float>(Move->Velocity.X);
					L->WallJumpVz = static_cast<float>(Move->Velocity.Z);
				}
				if (CountA == 4 && T > 9.45f)
				{
					CountA = 5;
					Move->JumpReleased();
				}
				if (CountA == 5 && CountB == 2 && T > 9.55f)
				{
					CountB = 3;
					Shot(TEXT("70_sticky_3_walljump"));
				}
				// Stuck again, then a hit from the side (away from the wall): knocked off it.
				if (!L->bHitSet && T > 10.f)
				{
					L->bHitSet = true;
					VirtualInput = FVector2D::ZeroVector;
					PutAtTallWall(400.f);
				}
				if (L->bHitSet && !L->bHitDone && T > 10.3f)
				{
					L->bHitDone = true;
					L->bStuckBeforeHit = Move->IsWallClinging();
					FPenguinHit Hit;
					Hit.Instigator = Target.Get();
					Hit.Source = EPenguinAbility::Snowball;
					Hit.Knockback = FVector2D(-900.f, 300.f);
					Hit.Stun = 0.3f;
					Hit.Location = Location();
					Penguin->ApplyPenguinHit(Hit);
				}
				if (L->bHitDone && T > 10.32f && T < 10.5f)
				{
					L->bKnockedOff |= !Move->IsWallClinging() && Move->Velocity.X < -200.f;
				}
				// No belly slide.
				if (!L->bSlideTried && T > 11.f)
				{
					L->bSlideTried = true;
					VirtualInput = FVector2D::ZeroVector;
					Teleport(Base + FVector(-700.f, 0.f, 0.f));
				}
				if (L->bSlideTried && CountB == 3 && T > 11.3f)
				{
					CountB = 4;
					L->SlidesBefore = Move->SlideEvents;
					Penguin->BellySlide();
				}
				if (CountB == 4)
				{
					L->bSlid |= Move->IsBellySliding() || Move->SlideEvents != L->SlidesBefore;
				}
			},
			[this, L, Judge, Clear, Base, ReleaseStage]
			{
				using namespace PinguTraitTuning;
				ReleaseStage();
				const int32 Climbs = Move->ClimbEvents - L->ClimbsAtStart;
				const int32 AwayStuckFrames = L->AwayFrames < 0 ? -L->AwayFrames - 1 : 99;
				const bool bHang = L->HangFrom >= 0.f && L->HangLongest >= 4.8f && L->HangDrift <= 0.5f && L->HangLostFrames == 0;
				const bool bAway = AwayStuckFrames <= 2 && L->AwayMoved > 30.f;
				const bool bSlide = L->bSlideStuck && FMath::Abs(L->SlideSpeed - StickyClingSlideSpeed) <= 0.1f * StickyClingSlideSpeed;
				const bool bUp = L->bUpStuck && L->UpMoved <= 0.5f;
				const bool bWallJump = L->bWallJumped && L->WallJumpVx < -0.9f * StickyWallJumpSide && L->WallJumpVz > 1000.f;
				const bool bHit = L->bStuckBeforeHit && L->bKnockedOff;
				const bool bOk = Climbs >= 1 && bHang && bAway && bSlide && bUp && bWallJump && bHit && !L->bSlid && !Move->CanBellySlideNow()
					&& IsNear(Move->GetEffectiveClimbReachUp(), Move->ClimbReachUp * 1.7f, 1.f);
				Judge(TEXT("Sticky"), bOk, FString::Printf(TEXT("grabbed a ledge 2 blocks above its feet %d (reach up %.0f cm, forward %.0f; want 1); no input: stuck to the wall %.2f s, "
					"moved %.1f cm, let go %d frames (want 4.8 s or more, 0.5 cm at most, 0); the stick away: let go after %d frames (want 2 at most), %.0f cm off the wall 0.4 s later; "
					"down held: slid at %.0f cm/s still stuck %d (want %.0f, 10 %%); up held: moved %.1f cm still stuck %d (want 0); wall jump %d at %.0f / %.0f cm/s (want away %.0f, up); "
					"a hit knocked it off %d (stuck before %d); belly slide %d (want 0)"),
					Climbs, Move->GetEffectiveClimbReachUp(), Move->GetEffectiveClimbReachForward(), L->HangLongest, L->HangDrift, L->HangLostFrames,
					AwayStuckFrames, L->AwayMoved, L->SlideSpeed, L->bSlideStuck ? 1 : 0, StickyClingSlideSpeed, L->UpMoved, L->bUpStuck ? 1 : 0,
					L->bWallJumped ? 1 : 0, L->WallJumpVx, L->WallJumpVz, -StickyWallJumpSide, L->bKnockedOff ? 1 : 0, L->bStuckBeforeHit ? 1 : 0, L->bSlid ? 1 : 0));
				Clear();
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
				{
					Flat->ClearTemporaryBlocks();
				}
			});
	}

	// ---- The summary.
	AddSharedTraitSteps(); // the traits made for everybody having them (PenguinAutoTestSharedTraits.cpp)

	AddStep(TEXT("trait_summary"), 0.1f,
		[this]
		{
			const FPinguTraitTestState& State = *TraitTest;
			Report(FString::Printf(TEXT("RESULT traits: %d of %d trait checks ok%s%s"), State.Passed, State.Checked,
				State.Failed.Num() > 0 ? TEXT(", failed: ") : TEXT(""), *FString::Join(State.Failed, TEXT(", "))));
			PrepareBrawl(2400.f, {});
		},
		nullptr, nullptr);

	AddTraitLookSteps();
}

void APenguinAutoTest::AddTraitLookSteps()
{
	using namespace PenguinAutoTestTraitsPrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	const auto HoldCamera = [this]()
	{
		if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
		{
			Stage->SetActorTickEnabled(false);
			LookCamera = Stage;
		}
	};
	const auto ReleaseCamera = [this]()
	{
		if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
		{
			Stage->SetActorTickEnabled(true);
			Stage->SnapToTargets();
		}
		LookCamera.Reset();
	};
	const auto SpawnExtra = [this](const FVector& Where, int32 Slot) -> APenguinCharacter*
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		APenguinCharacter* Extra = GetWorld()->SpawnActor<APenguinCharacter>(Penguin->GetClass(), Where, FRotator::ZeroRotator, Params);
		if (Extra)
		{
			Extra->SetPlayerSlot(Slot % PinguMaxPenguins, FString::Printf(TEXT("Extra %d"), Slot));
			Extra->SetInputLocked(false);
			Extra->FaceDirection(1.f);
		}
		return Extra;
	};

	// ---- Every trait's look on our penguin, one after another (a close-up each), then all of them in a line.
	{
		struct FLooks
		{
			TArray<EPinguTrait> All;
			int32 Next = 0;
			float SetAt = -1.f;
			bool bShotDue = false;
			int32 Shots = 0;
			int32 PlainPieces = 0;
			int32 MostPieces = 0;
			FString Detail;
			bool bGlassOverlay = false;
			bool bMirrorOverlay = false;
			bool bHeavyBody = false;
			/** Jester's felt cap (PinguJesterHat): there, on spring bones on Ultra effects and static otherwise, the beanie hidden. */
			bool bJesterHat = false;
			FString JesterHat = TEXT("not seen");
			bool bLineup = false;
			float LineupAt = -1.f;
			bool bLineupShot = false;
			/** The roundest the eyes were (EyeRoundness, the least over the frames before the shot: a blink squashes them)
			 *  with Heavyweight's and with Giant's body. */
			float EyeRound[2] = { 0.f, 0.f };
			TArray<TWeakObjectPtr<APenguinCharacter>> Extras;
		};
		const TSharedRef<FLooks> L = MakeShared<FLooks>();
		static constexpr float LookEvery = 0.8f;
		static constexpr float LookShotAfter = 0.6f;
		AddStep(TEXT("look_TraitLooks"), 0.3f + PinguNumTraits * LookEvery + 2.f,
			[this, L, HoldCamera]
			{
				*L = FLooks();
				L->All = PinguTraits::GetAll();
				SpawnTarget();
				PrepareBrawl(2400.f, {});
				L->PlainPieces = CountMeshPieces(Penguin.Get());
				HoldCamera();
			},
			[this, L, Base, SpawnExtra](float T)
			{
				FLooks& S = *L;
				if (S.Next < S.All.Num() && T > 0.3f + S.Next * LookEvery)
				{
					// A fresh penguin (normal size, on its feet) with the next trait.
					Teleport(Base);
					Penguin->ResetForRound(Location());
					Penguin->SetInputLocked(false);
					Penguin->FaceDirection(1.f);
					SetTraitNow(Penguin.Get(), S.All[S.Next]);
					S.SetAt = T;
					S.bShotDue = true;
					++S.Next;
				}
				if (S.bShotDue && T > S.SetAt + 0.25f && T <= S.SetAt + LookShotAfter)
				{
					const EPinguTrait Trait = S.All[S.Next - 1];
					const int32 Case = Trait == EPinguTrait::Heavyweight ? 0 : (Trait == EPinguTrait::Giant ? 1 : -1);
					const float Round = EyeRoundness(Penguin.Get());
					if (Case >= 0 && Round > 0.f)
					{
						S.EyeRound[Case] = S.EyeRound[Case] > 0.f ? FMath::Min(S.EyeRound[Case], Round) : Round;
					}
				}
				if (S.bShotDue && T > S.SetAt + LookShotAfter)
				{
					S.bShotDue = false;
					const EPinguTrait Trait = S.All[S.Next - 1];
					const FPinguTraitInfo& Info = PinguTraits::GetInfo(Trait);
					// Close up from the camera's side, the whole penguin in the picture (a giant too).
					if (AActor* Camera = LookCamera.Get())
					{
						const float Size = FMath::Max(Penguin->GetSizeScale(), 1.f);
						const FVector LookAt = Penguin->GetFeetLocation() + FVector(0.f, 0.f, 55.f * Size);
						const FVector From = LookAt + FVector(0.f, 480.f * Size, 40.f * Size);
						Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
					}
					Shot(*FString::Printf(TEXT("60_trait_%s"), Info.IconName));
					++S.Shots;
					const int32 Pieces = CountMeshPieces(Penguin.Get()) - S.PlainPieces;
					const bool bOverlay = HasOverlay(Penguin.Get());
					const FVector Body = PinguTraitLooks::GetBodyShape(Penguin.Get());
					S.MostPieces = FMath::Max(S.MostPieces, Pieces);
					S.bGlassOverlay |= Trait == EPinguTrait::GlassCannon && bOverlay;
					S.bMirrorOverlay |= Trait == EPinguTrait::Mirror && bOverlay;
					S.bHeavyBody |= Trait == EPinguTrait::Heavyweight && Body.X > 1.1f;
					if (Trait == EPinguTrait::Jester)
					{
						PinguJesterHat::FStats Hat;
						int32 BeanieHidden = 0;
						const bool bHat = PinguTraitLooks::GetJesterHatStats(Penguin.Get(), Hat, &BeanieHidden);
						const bool bWantSpring = UPinguSettings::UseUltraEffects();
						S.bJesterHat = bHat && Hat.bShown && Hat.bSkeletal == bWantSpring && (!bWantSpring || Hat.Chains == 3) && BeanieHidden > 0;
						S.JesterHat = FString::Printf(TEXT("%s %s (want %s), %d points, beanie hidden %d sections"), bHat ? *Hat.Mesh : TEXT("no cap"),
							Hat.bSkeletal ? TEXT("on spring bones") : TEXT("static"), bWantSpring ? TEXT("spring bones on Ultra") : TEXT("static"), Hat.Chains, BeanieHidden);
					}
					S.Detail += FString::Printf(TEXT("%s%s %d pieces%s%s size %.2f"), S.Detail.IsEmpty() ? TEXT("") : TEXT(", "), Info.IconName, Pieces,
						bOverlay ? TEXT(" overlay") : TEXT(""), Body.Equals(FVector::OneVector, 0.01f) ? TEXT("") : *FString::Printf(TEXT(" body %.2f/%.2f/%.2f"), Body.X, Body.Y, Body.Z),
						Penguin->GetSizeScale());
				}
				// All of them in two lines (nine on the strip, eight on a ledge above them): ours, the dummy and fifteen more.
				if (!S.bLineup && S.Next >= S.All.Num() && !S.bShotDue && T > 0.3f + S.All.Num() * LookEvery)
				{
					S.bLineup = true;
					S.LineupAt = T;
					constexpr float Spacing = 125.f;
					constexpr int32 PerRow = 9;
					constexpr int32 LedgeRows = 3;
					const float FirstX = Base.X - Spacing * 4.f;
					float LedgeTop = 0.f;
					if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
					{
						const FIntPoint Foot = Flat->WorldToCell(FVector(FirstX - 100.f, 0.f, 10.f));
						TArray<FIntPoint> Cells;
						for (int32 Column = 0; Column < 14; ++Column)
						{
							Cells.Add(FIntPoint(Foot.X + Column, Foot.Y + LedgeRows));
						}
						Flat->AddBlocks(Cells, EBlockKind::Rock, 5.f);
						Flat->FlushEdits();
						LedgeTop = static_cast<float>(Flat->CellToWorld(FIntPoint(Foot.X, Foot.Y + LedgeRows)).Z) + Flat->CellSize * 0.5f;
					}
					for (int32 Index = 0; Index < S.All.Num(); ++Index)
					{
						const bool bLedge = Index >= PerRow;
						const FVector Where(FirstX + (bLedge ? (Index - PerRow) * Spacing + Spacing * 0.5f : Index * Spacing), 0.f, (bLedge ? LedgeTop : 0.f) + 62.f);
						APenguinCharacter* Who = nullptr;
						if (Index == 0)
						{
							Who = Penguin.Get();
							Teleport(Where);
							Penguin->ResetForRound(Where);
							Penguin->SetInputLocked(false);
						}
						else if (Index == 1 && Target.IsValid())
						{
							Who = Target.Get();
							ResetTarget(Where);
						}
						else
						{
							Who = SpawnExtra(Where, Index);
							S.Extras.Add(Who);
						}
						if (Who)
						{
							Who->FaceDirection(1.f);
							SetTraitNow(Who, S.All[Index]);
						}
					}
					if (AActor* Camera = LookCamera.Get())
					{
						const FVector LookAt(Base.X, 0.f, 0.5f * LedgeTop + 60.f);
						const FVector From = LookAt + FVector(0.f, 1750.f, 80.f);
						Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
					}
				}
				if (S.bLineup && !S.bLineupShot && T > S.LineupAt + 0.7f)
				{
					S.bLineupShot = true;
					Shot(TEXT("60_trait_lineup"));
					++S.Shots;
				}
			},
			[this, L, ReleaseCamera, Base]
			{
				FLooks& S = *L;
				for (const TWeakObjectPtr<APenguinCharacter>& Extra : S.Extras)
				{
					if (Extra.IsValid())
					{
						Extra->Destroy();
					}
				}
				S.Extras.Reset();
				SetTraitNow(Penguin.Get(), EPinguTrait::None);
				SetTraitNow(Target.Get(), EPinguTrait::None);
				ReleaseCamera();
				PrepareBrawl(2400.f, {});
				const bool bOk = S.Shots == PinguNumTraits + 1 && S.MostPieces <= 4 && S.bGlassOverlay && S.bMirrorOverlay && S.bHeavyBody && S.bJesterHat;
				Report(FString::Printf(TEXT("RESULT trait looks: %d shots (want %d), at most %d pieces on a penguin (want 4 at most), Glass Cannon overlay %d, Mirror overlay %d, Heavyweight's fat body %d, Jester's cap %d (%s): %s"),
					S.Shots, PinguNumTraits + 1, S.MostPieces, S.bGlassOverlay ? 1 : 0, S.bMirrorOverlay ? 1 : 0, S.bHeavyBody ? 1 : 0, S.bJesterHat ? 1 : 0, *S.JesterHat,
					bOk ? TEXT("ok") : TEXT("FAIL")));
				Report(FString::Printf(TEXT("trait looks: %s"), *S.Detail));
				const bool bRound = S.EyeRound[0] >= 1.f && S.EyeRound[0] <= 1.02f && S.EyeRound[1] >= 1.f && S.EyeRound[1] <= 1.02f;
				Report(FString::Printf(TEXT("RESULT trait eyes: round on a fat and a tall body - the longest to the shortest axis of an eye: Heavyweight x%.3f, Giant x%.3f (want 1.02 at most): %s"),
					S.EyeRound[0], S.EyeRound[1], bRound ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- The reveal: four penguins get their traits with a reveal - the cards above them, their flight into the penguins,
	// the changed penguins (Giant big, Tiny small). Screenshots with the UI.
	{
		struct FReveal
		{
			float Start = 0.f;
			TArray<TWeakObjectPtr<APenguinCharacter>> Extras;
			int32 MostCards = 0;
			int32 MostFlying = 0;
			int32 MostFlashes = 0;
			int32 Shots = 0;
			int32 CardsAtShot = 0;
			int32 FlyingAtShot = 0;
			float GiantSize = 0.f;
			float TinySize = 0.f;
			FString Names;
		};
		const TSharedRef<FReveal> L = MakeShared<FReveal>();
		static constexpr float RevealLead = 0.3f;
		AddStep(TEXT("look_TraitReveal"), RevealLead + UPenguinTraitComponent::RevealEndAt + 1.f,
			[this, L, Base, SpawnExtra, HoldCamera]
			{
				*L = FReveal();
				SpawnTarget();
				PrepareBrawl(250.f, {});
				L->Start = FlowNow(this) + RevealLead;
				const auto Reveal = [L](APenguinCharacter* Who, EPinguTrait Trait)
				{
					if (UPenguinTraitComponent* Traits = Who ? Who->GetTraits() : nullptr)
					{
						Traits->SetTrait(Trait, L->Start);
					}
				};
				Reveal(Penguin.Get(), EPinguTrait::Giant);
				Reveal(Target.Get(), EPinguTrait::Tiny);
				APenguinCharacter* First = SpawnExtra(Base + FVector(-450.f, 0.f, 0.f), 2);
				APenguinCharacter* Second = SpawnExtra(Base + FVector(500.f, 0.f, 0.f), 3);
				L->Extras = { First, Second };
				Reveal(First, EPinguTrait::Jester);
				Reveal(Second, EPinguTrait::GlassCannon);
				// The four of them in the picture (the stage camera held: it would frame the whole strip).
				HoldCamera();
				if (AActor* Camera = LookCamera.Get())
				{
					const FVector LookAt = Base + FVector(25.f, 0.f, 80.f);
					const FVector From = LookAt + FVector(0.f, 1800.f, 40.f);
					Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
			},
			[this, L](float T)
			{
				FReveal& S = *L;
				const PinguUI::FTraitRevealStats& Stats = PinguUI::GetTraitRevealStats();
				S.MostCards = FMath::Max(S.MostCards, Stats.Cards);
				S.MostFlying = FMath::Max(S.MostFlying, Stats.Flying);
				S.MostFlashes = FMath::Max(S.MostFlashes, Stats.Flashes);
				const float Age = T - RevealLead;
				const float ShotAges[3] = { 1.6f, UPenguinTraitComponent::CardsTime + UPenguinTraitComponent::FlyTime * 0.5f, UPenguinTraitComponent::RevealEndAt + 0.3f };
				static const TCHAR* ShotNames[3] = { TEXT("61_trait_reveal_cards"), TEXT("61_trait_reveal_fly"), TEXT("61_trait_reveal_done") };
				if (S.Shots < 3 && Age > ShotAges[S.Shots])
				{
					if (S.Shots == 0)
					{
						S.CardsAtShot = Stats.Cards;
						S.Names = FString::Join(Stats.Names, TEXT(", "));
					}
					else if (S.Shots == 1)
					{
						S.FlyingAtShot = Stats.Flying;
					}
					else
					{
						S.GiantSize = Penguin->GetSizeScale();
						S.TinySize = Target.IsValid() ? Target->GetSizeScale() : 0.f;
					}
					Shot(ShotNames[S.Shots], true);
					++S.Shots;
				}
			},
			[this, L, ReleaseCamera]
			{
				FReveal& S = *L;
				for (const TWeakObjectPtr<APenguinCharacter>& Extra : S.Extras)
				{
					if (Extra.IsValid())
					{
						Extra->Destroy();
					}
				}
				S.Extras.Reset();
				SetTraitNow(Penguin.Get(), EPinguTrait::None);
				SetTraitNow(Target.Get(), EPinguTrait::None);
				ReleaseCamera();
				PrepareBrawl(2400.f, {});
				const bool bOk = S.Shots == 3 && S.MostCards >= 4 && S.CardsAtShot >= 4 && S.MostFlying >= 1 && IsNear(S.GiantSize, 1.682f, 0.05f) && IsNear(S.TinySize, PinguShrinkRayFactor, 0.03f);
				Report(FString::Printf(TEXT("RESULT trait reveal: shots %d (want 3), cards up to %d (%d at the first shot, want 4), flying up to %d (%d at the second shot, want 1 or more), flashes %d, "
					"after it the Giant %.2f and Tiny %.2f (want 1.68 0.71); cards: %s: %s"), S.Shots, S.MostCards, S.CardsAtShot, S.MostFlying, S.FlyingAtShot, S.MostFlashes,
					S.GiantSize, S.TinySize, S.Names.IsEmpty() ? TEXT("none") : *S.Names, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- The reveal with penguins out of the picture: ten penguins spread 42 m wide, the camera held on the middle 11 m,
	// so three stand beyond each side of the screen (their markers at the edges). Every card must show wholly in the
	// picture, none may overlap another or cover a marker, and the six edge cards line up along their edges; they fly to
	// the markers. Checked on every frame the cards show.
	{
		struct FEdge
		{
			float Start = 0.f;
			TArray<TWeakObjectPtr<APenguinCharacter>> Extras;
			int32 Frames = 0;
			int32 FewestCards = 1000;
			int32 MostCards = 0;
			int32 MostEdgeCards = 0;
			int32 MostMarkers = 0;
			int32 MostOverlaps = 0;
			int32 MostOverMarkers = 0;
			int32 MostOutside = 0;
			int32 MostFlying = 0;
			int32 Shots = 0;
		};
		const TSharedRef<FEdge> L = MakeShared<FEdge>();
		static constexpr float EdgeLead = 0.3f;
		AddStep(TEXT("look_TraitRevealEdge"), EdgeLead + UPenguinTraitComponent::RevealEndAt + 1.f,
			[this, L, Base, SpawnExtra, HoldCamera]
			{
				*L = FEdge();
				SpawnTarget();
				PrepareBrawl(250.f, {});
				L->Start = FlowNow(this) + EdgeLead;
				// Left to right: three off the left edge, four in the picture (ours the first of them), three off the right.
				static const float Offsets[] = { -2100.f, -1600.f, -1100.f, -450.f, -100.f, 200.f, 480.f, 1100.f, 1600.f, 2100.f };
				static const EPinguTrait Traits[] = { EPinguTrait::Featherweight, EPinguTrait::SecondChance, EPinguTrait::GlassCannon, EPinguTrait::Giant,
					EPinguTrait::Tiny, EPinguTrait::Jester, EPinguTrait::HotFeet, EPinguTrait::Heavyweight, EPinguTrait::QuickDraw, EPinguTrait::SlowBurn };
				const auto Reveal = [L](APenguinCharacter* Who, EPinguTrait Trait)
				{
					if (UPenguinTraitComponent* Comp = Who ? Who->GetTraits() : nullptr)
					{
						Comp->SetTrait(Trait, L->Start);
					}
				};
				for (int32 Index = 0; Index < UE_ARRAY_COUNT(Offsets); ++Index)
				{
					const FVector Where = Base + FVector(Offsets[Index], 0.f, 0.f);
					APenguinCharacter* Who = nullptr;
					if (Index == 3)
					{
						Who = Penguin.Get();
						Teleport(Where);
					}
					else if (Index == 4 && Target.IsValid())
					{
						Who = Target.Get();
						ResetTarget(Where);
					}
					else
					{
						Who = SpawnExtra(Where, Index);
						L->Extras.Add(Who);
					}
					Reveal(Who, Traits[Index]);
				}
				HoldCamera();
				if (AActor* Camera = LookCamera.Get())
				{
					const FVector LookAt = Base + FVector(0.f, 0.f, 80.f);
					const FVector From = LookAt + FVector(0.f, 1800.f, 40.f);
					Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
			},
			[this, L](float T)
			{
				FEdge& S = *L;
				const PinguUI::FTraitRevealStats& Stats = PinguUI::GetTraitRevealStats();
				const float Age = T - EdgeLead;
				// The cards stand from about 0.6 s (the last one popped in) until the first flies at 3 s.
				if (Age > 0.7f && Age < UPenguinTraitComponent::CardsTime - 0.05f && Stats.Frame + 2 >= GFrameCounter)
				{
					++S.Frames;
					S.FewestCards = FMath::Min(S.FewestCards, Stats.Cards);
					S.MostCards = FMath::Max(S.MostCards, Stats.Cards);
					S.MostEdgeCards = FMath::Max(S.MostEdgeCards, Stats.EdgeCards);
					S.MostMarkers = FMath::Max(S.MostMarkers, Stats.Markers);
					S.MostOverlaps = FMath::Max(S.MostOverlaps, Stats.OverlapPairs);
					S.MostOverMarkers = FMath::Max(S.MostOverMarkers, Stats.CardsOverMarkers);
					S.MostOutside = FMath::Max(S.MostOutside, Stats.CardsOutside);
				}
				S.MostFlying = FMath::Max(S.MostFlying, Stats.Flying);
				const float ShotAges[3] = { 1.6f, UPenguinTraitComponent::CardsTime + UPenguinTraitComponent::FlyTime * 0.6f, UPenguinTraitComponent::RevealEndAt + 0.3f };
				static const TCHAR* ShotNames[3] = { TEXT("63_trait_reveal_edge_cards"), TEXT("63_trait_reveal_edge_fly"), TEXT("63_trait_reveal_edge_done") };
				if (S.Shots < 3 && Age > ShotAges[S.Shots])
				{
					Shot(ShotNames[S.Shots], true);
					++S.Shots;
				}
			},
			[this, L, ReleaseCamera]
			{
				FEdge& S = *L;
				for (const TWeakObjectPtr<APenguinCharacter>& Extra : S.Extras)
				{
					if (Extra.IsValid())
					{
						Extra->Destroy();
					}
				}
				S.Extras.Reset();
				SetTraitNow(Penguin.Get(), EPinguTrait::None);
				SetTraitNow(Target.Get(), EPinguTrait::None);
				ReleaseCamera();
				PrepareBrawl(2400.f, {});
				const bool bOk = S.Frames >= 10 && S.FewestCards == 10 && S.MostCards == 10 && S.MostEdgeCards == 6 && S.MostMarkers >= 6
					&& S.MostOverlaps == 0 && S.MostOverMarkers == 0 && S.MostOutside == 0 && S.MostFlying >= 6 && S.Shots == 3;
				Report(FString::Printf(TEXT("RESULT trait reveal edge: %d frames of cards: cards %d..%d (want 10), at the edges %d (want 6), markers %d (want 6 or more); "
					"overlapping pairs %d, cards over a marker %d, cards not wholly in the picture %d (want 0 0 0); flying up to %d, shots %d: %s"),
					S.Frames, S.FewestCards == 1000 ? 0 : S.FewestCards, S.MostCards, S.MostEdgeCards, S.MostMarkers, S.MostOverlaps, S.MostOverMarkers, S.MostOutside,
					S.MostFlying, S.Shots, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- The snow bomb's white eyes on a fat and a tall penguin: the dummy as Heavyweight, then as Giant, knocked out by a
	// snow bomb - while it stands charred the white eyes lie over its own eyeballs (APenguinCharacter::UpdateDeathFaceEyes),
	// which keep their round shape on the trait's body (PenguinAnimInstance::ApplyBodyShape). A picture of each.
	{
		struct FBombEyes
		{
			int32 Stage = 0;
			float KOAt = -1.f;
			float Off[2] = { -1.f, -1.f };
			int32 FewestEyes[2] = { 9, 9 };
			int32 Frames[2] = { 0, 0 };
			float Radius[2] = { 0.f, 0.f };
			int32 Shots = 0;
		};
		const TSharedRef<FBombEyes> L = MakeShared<FBombEyes>();
		static constexpr float SecondAt = 3.3f;
		static constexpr float KOAfter = 0.8f;
		AddStep(TEXT("look_TraitBombEyes"), SecondAt + KOAfter + 1.4f,
			[this, L, Base, HoldCamera]
			{
				*L = FBombEyes();
				SpawnTarget();
				PrepareBrawl(250.f, {});
				ResetTarget(Base + FVector(250.f, 0.f, 0.f));
				SetTraitNow(Target.Get(), EPinguTrait::Heavyweight);
				HoldCamera();
			},
			[this, L, Base](float T)
			{
				FBombEyes& S = *L;
				APenguinCharacter* Dummy = Target.Get();
				if (!Dummy)
				{
					return;
				}
				if (S.Stage == 0 && T > SecondAt)
				{
					// (After the first one's respawn time: a practice respawn never comes in between.)
					S.Stage = 1;
					S.KOAt = -1.f;
					ResetTarget(Base + FVector(250.f, 0.f, 0.f));
					SetTraitNow(Dummy, EPinguTrait::Giant);
				}
				const int32 Case = S.Stage;
				const float CaseT = T - (Case == 0 ? 0.f : SecondAt);
				if (AActor* Camera = LookCamera.Get())
				{
					// Before the knock-out: its face from the front (the way it looks, turned toward the camera a little),
					// then from the side as the game shows it.
					const float Size = FMath::Max(Dummy->GetSizeScale(), 1.f);
					const bool bFront = CaseT < 0.55f;
					const FVector LookAt = Dummy->GetFeetLocation() + FVector(0.f, 0.f, (bFront ? 85.f : 70.f) * Size);
					const FVector From = LookAt + (bFront ? FVector(Dummy->GetFacing() * 260.f, 170.f, 25.f) : FVector(0.f, 330.f, 30.f)) * Size;
					Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
				if (S.Shots == Case * 3 && CaseT > 0.45f)
				{
					Shot(Case == 0 ? TEXT("64_trait_eyes_heavyweight_front") : TEXT("64_trait_eyes_giant_front"));
					++S.Shots;
				}
				if (S.Shots == Case * 3 + 1 && CaseT > 0.72f)
				{
					Shot(Case == 0 ? TEXT("64_trait_eyes_heavyweight_side") : TEXT("64_trait_eyes_giant_side"));
					++S.Shots;
				}
				if (S.KOAt < 0.f && T > (Case == 0 ? 0.f : SecondAt) + KOAfter)
				{
					Dummy->KnockOut(EPenguinKOReason::Bomb, Penguin.Get(), FVector2D(1200.f, 500.f));
					S.KOAt = T;
				}
				const float KOT = S.KOAt >= 0.f ? T - S.KOAt : -1.f;
				if (KOT > 0.3f && KOT < 0.72f && Dummy->IsKnockedOut() && !Dummy->IsHidden())
				{
					int32 Heap = 0;
					int32 EyeWhites = 0;
					APinguFX::CountAshPileParts(Dummy, Heap, EyeWhites);
					float Radius = 0.f;
					const float Off = BombEyesOffOwn(Dummy, Radius);
					++S.Frames[Case];
					S.FewestEyes[Case] = FMath::Min(S.FewestEyes[Case], EyeWhites);
					S.Off[Case] = FMath::Max(S.Off[Case], Off);
					S.Radius[Case] = Radius;
				}
				if (KOT > 0.55f && S.Shots == Case * 3 + 2)
				{
					Shot(Case == 0 ? TEXT("64_trait_bomb_eyes_heavyweight") : TEXT("64_trait_bomb_eyes_giant"));
					++S.Shots;
				}
			},
			[this, L, ReleaseCamera]
			{
				const FBombEyes& S = *L;
				SetTraitNow(Target.Get(), EPinguTrait::None);
				ReleaseCamera();
				PrepareBrawl(2400.f, {});
				const bool bOk = S.Frames[0] >= 5 && S.Frames[1] >= 5 && S.FewestEyes[0] == 2 && S.FewestEyes[1] == 2 && S.Off[0] >= 0.f && S.Off[0] < 2.f
					&& S.Off[1] >= 0.f && S.Off[1] < 2.f && S.Shots == 6;
				Report(FString::Printf(TEXT("RESULT trait bomb eyes: Heavyweight white eyes %d (want 2) %.1f cm off its own (want under 2, radius %.1f cm), Giant white eyes %d %.1f cm off (radius %.1f cm), "
					"frames %d / %d, shots %d: %s"), S.FewestEyes[0] == 9 ? 0 : S.FewestEyes[0], S.Off[0], S.Radius[0], S.FewestEyes[1] == 9 ? 0 : S.FewestEyes[1], S.Off[1], S.Radius[1],
					S.Frames[0], S.Frames[1], S.Shots, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- Gambler's fizzle: the ability turns into sparkling dust. Rolls are forced (UPenguinTraitComponent::SetForcedGamble).
	// A fizzle of a snowball (the burst, then the dust), of a gust (its icon crumbling), a double on the ice shell (the gold
	// shine), then a fizzle of every ability in turn (each plays its dust, makes no effect, counts no use) and one on Low.
	// Pictures with the UI; the numbers come from the effects actor (dust events and particles) and the overhead widget's
	// paint stats (icons crumbling, dust pieces drawn).
	{
		struct FProbe
		{
			int32 Events = 0;
			int32 Particles = 0;
			int32 Uses = 0;
			int32 Hits = 0;
			int32 Projectiles = 0;
		};
		struct FFizzle
		{
			int32 Stage = 0;
			float At = 0.f;
			bool bShotA = false;
			bool bShotB = false;
			bool bShotLow = false;
			/** The icon's dust has been seen (the UI runs on its own clock, so its end is waited for by what it paints). */
			bool bDustSeen = false;
			float PeakAge = -1.f;
			int32 Shots = 0;
			FProbe Before;
			/** What the snowball's fizzle made (deltas against Before, a moment after it) and the gust's. */
			FProbe Snowball;
			FProbe Gust;
			/** The icon dust and shine starts at the start of the step, the paint stats' peaks. */
			int32 DustStartsBase = 0;
			int32 ShineStartsBase = 0;
			int32 DustStartsAfterB = 0;
			int32 PeakIcons = 0;
			int32 PeakShine = 0;
			int32 PeakPieces = 0;
			int32 PiecesFull = 0;
			int32 PiecesLow = 0;
			int32 ParticlesLow = 0;
			float DensityFull = 1.f;
			float DensityLow = 1.f;
			/** The double's use: the ice shell came out (a use counted), its shine started. */
			int32 DoubleUses = 0;
			int32 ShineStarts = 0;
			/** Every ability in turn. */
			int32 Index = 0;
			bool bActive = false;
			float NextAt = 0.f;
			float CheckAt = 0.f;
			int32 ShapesOk = 0;
			FString Bad;
			bool bPresetSet = false;
			FPinguGraphicsOptions PresetBefore;
		};
		const TSharedRef<FFizzle> L = MakeShared<FFizzle>();
		static constexpr float FizzleWaitMax = 30.f;
		static constexpr int32 AbilityCount = static_cast<int32>(EPenguinAbility::Count) - 1;
		const auto Probe = [this]() -> FProbe
		{
			FProbe Now;
			if (const APinguFX* FX = APinguFX::Get(this))
			{
				Now.Events = FX->GetFizzleDustEvents();
				Now.Particles = FX->GetFizzleDustParticles();
			}
			Now.Uses = Penguin->GetAbilities()->GetUseCount(0) + Penguin->GetAbilities()->GetUseCount(1) + Penguin->GetAbilities()->GetUseCount(2);
			Now.Hits = Target.IsValid() && Target->GetPenguinMovement() ? Target->GetPenguinMovement()->HitEvents : 0;
			for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
			{
				++Now.Projectiles;
			}
			return Now;
		};
		const auto Difference = [](const FProbe& Now, const FProbe& Was) -> FProbe
		{
			FProbe Out;
			Out.Events = Now.Events - Was.Events;
			Out.Particles = Now.Particles - Was.Particles;
			Out.Uses = Now.Uses - Was.Uses;
			Out.Hits = Now.Hits - Was.Hits;
			Out.Projectiles = Now.Projectiles - Was.Projectiles;
			return Out;
		};
		// A fizzle of the ability in a slot, aimed at the dummy.
		const auto Press = [this](int32 Slot)
		{
			Penguin->SetAimPoint(Target.IsValid() ? Target->GetAimOrigin() : Location() + FVector(300.f, 0.f, 0.f));
			Penguin->ActivateAbility(Slot);
		};
		AddStep(TEXT("look_GamblerFizzle"), 120.f,
			[this, L, HoldCamera]
			{
				*L = FFizzle();
				SpawnTarget();
				PrepareBrawl(300.f, { EPenguinAbility::Snowball, EPenguinAbility::IceShell, EPenguinAbility::Gust });
				SetTraitNow(Penguin.Get(), EPinguTrait::Gambler);
				Penguin->GetTraits()->SetForcedGamble(0);
				L->DustStartsBase = SPinguOverheadAbilities::GetLastPaintStats().DustStarts;
				L->ShineStartsBase = SPinguOverheadAbilities::GetLastPaintStats().ShineStarts;
				if (const APinguFX* FX = APinguFX::Get(this))
				{
					L->DensityFull = FX->GetDensity();
				}
				HoldCamera();
			},
			[this, L, Probe, Difference, Press](float T)
			{
				FFizzle& S = *L;
				if (AActor* Camera = LookCamera.Get())
				{
					const FVector LookAt = Penguin->GetFeetLocation() + FVector(60.f, 0.f, 85.f);
					const FVector From = LookAt + FVector(0.f, 430.f, 30.f);
					Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
				const SPinguOverheadAbilities::FPaintStats& Stats = SPinguOverheadAbilities::GetLastPaintStats();
				S.PeakIcons = FMath::Max(S.PeakIcons, Stats.DustIcons);
				S.PeakShine = FMath::Max(S.PeakShine, Stats.ShineIcons);
				S.PeakPieces = FMath::Max(S.PeakPieces, Stats.DustPieces);
				switch (S.Stage)
				{
				case 0: // the snowball fizzles: a burst, then the dust falling
					if (T > 0.4f)
					{
						S.Before = Probe();
						S.PeakPieces = 0;
						Press(0);
						S.Snowball = Difference(Probe(), S.Before);
						S.At = T;
						S.Stage = 1;
					}
					break;
				case 1:
					if (!S.bShotA && T > S.At + 0.07f)
					{
						S.bShotA = true;
						Shot(TEXT("66_gambler_fizzle_1"), true);
						++S.Shots;
					}
					if (!S.bShotB && T > S.At + 0.4f)
					{
						S.bShotB = true;
						Shot(TEXT("66_gambler_fizzle_2"), true);
						++S.Shots;
					}
					if (T > S.At + 1.15f)
					{
						S.bShotA = false;
						S.Stage = 2;
					}
					break;
				case 2: // the gust fizzles: its icon crumbling
					S.Before = Probe();
					Press(2);
					S.Gust = Difference(Probe(), S.Before);
					S.At = T;
					S.Stage = 3;
					break;
				case 3:
					// (The icon's crumbling runs on the UI's own clock, not the test's: the picture is taken by its age, and the
					// end is the dust no longer being painted.)
					if (Stats.DustIcons > 0 && !S.bDustSeen)
					{
						S.bDustSeen = true;
						S.PeakPieces = 0;
					}
					if (Stats.DustPieces == S.PeakPieces && Stats.DustPieces > 0)
					{
						S.PeakAge = Stats.DustAge;
					}
					if (!S.bShotA && ((Stats.DustAge >= 0.22f && Stats.DustAge < 0.5f) || T > S.At + 3.f))
					{
						S.bShotA = true;
						Shot(TEXT("66_gambler_fizzle_3"), true);
						++S.Shots;
					}
					if ((S.bDustSeen && Stats.DustIcons == 0) || T > S.At + 3.f)
					{
						S.PiecesFull = S.PeakPieces;
						S.DustStartsAfterB = Stats.DustStarts;
						S.bShotA = false;
						S.bDustSeen = false;
						S.Stage = 4;
					}
					break;
				case 4: // a double on the ice shell: the gold shine
				{
					Penguin->GetTraits()->SetForcedGamble(1);
					S.Before = Probe();
					Press(1);
					S.DoubleUses = Difference(Probe(), S.Before).Uses;
					S.At = T;
					S.Stage = 5;
					break;
				}
				case 5:
					if (!S.bShotA && ((Stats.ShineAge >= 0.15f && Stats.ShineAge < 0.4f) || T > S.At + 2.f))
					{
						S.bShotA = true;
						Shot(TEXT("66_gambler_double_1"), true);
						++S.Shots;
					}
					if (T > S.At + 1.0f)
					{
						S.ShineStarts = Stats.ShineStarts - S.ShineStartsBase;
						Penguin->GetTraits()->SetForcedGamble(0);
						S.NextAt = T;
						S.Stage = 6;
					}
					break;
				case 6: // every ability in turn (the icicle fizzles when its throw is through, a moment later)
					if (S.Index >= AbilityCount)
					{
						S.Stage = 7;
						break;
					}
					if (!S.bActive && T >= S.NextAt)
					{
						const EPenguinAbility Ability = static_cast<EPenguinAbility>(S.Index + 1);
						Penguin->GetAbilities()->SetAbilities({ Ability });
						S.Before = Probe();
						Press(0);
						S.bActive = true;
						S.CheckAt = T + 0.3f;
					}
					else if (S.bActive && T >= S.CheckAt)
					{
						const EPenguinAbility Ability = static_cast<EPenguinAbility>(S.Index + 1);
						const FProbe Made = Difference(Probe(), S.Before);
						const bool bOk = Made.Events == 1 && Made.Particles >= 8 && Made.Uses == 0 && Made.Hits == 0 && Made.Projectiles == 0;
						S.ShapesOk += bOk ? 1 : 0;
						if (!bOk)
						{
							S.Bad += FString::Printf(TEXT("%s%s (events %d, particles %d, uses %d, hits %d, shots %d)"), S.Bad.IsEmpty() ? TEXT("") : TEXT(", "),
								UPenguinAbilityComponent::GetInfo(Ability).Name, Made.Events, Made.Particles, Made.Uses, Made.Hits, Made.Projectiles);
						}
						S.bActive = false;
						S.NextAt = T + 0.1f;
						++S.Index;
					}
					break;
				case 7: // Low: fewer particles, the dust still shows
					if (UPinguSettings* Settings = UPinguSettings::Get())
					{
						S.PresetBefore = Settings->GetGraphics();
						S.bPresetSet = true;
						Settings->ApplyPreset(EPinguGraphicsPreset::Low);
					}
					S.At = T;
					S.Stage = 8;
					break;
				case 8:
				{
					const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
					if (T > S.At + 0.8f && (!bCompiling || T > S.At + FizzleWaitMax))
					{
						if (const APinguFX* FX = APinguFX::Get(this))
						{
							S.DensityLow = FX->GetDensity();
						}
						Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Snowball, EPenguinAbility::IceShell, EPenguinAbility::Gust });
						S.Before = Probe();
						S.PeakPieces = 0;
						Press(0);
						S.ParticlesLow = Difference(Probe(), S.Before).Particles;
						S.At = T;
						S.Stage = 9;
					}
					break;
				}
				case 9: // (the picture waits: a screenshot's hitch makes the overhead widget start over - see SPinguOverheadAbilities::Tick)
					if (Stats.DustIcons > 0 && !S.bDustSeen)
					{
						S.bDustSeen = true;
						S.PeakPieces = 0;
					}
					if ((S.bDustSeen && Stats.DustIcons == 0) || T > S.At + 3.f)
					{
						S.PiecesLow = S.PeakPieces;
						// Once more for the picture.
						Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Snowball, EPenguinAbility::IceShell, EPenguinAbility::Gust });
						Press(0);
						S.At = T;
						S.Stage = 10;
					}
					break;
				default:
					if (!S.bShotLow && T > S.At + 0.12f)
					{
						S.bShotLow = true;
						Shot(TEXT("66_gambler_fizzle_low"), true);
						++S.Shots;
					}
					if (S.bShotLow && T > S.At + 0.5f)
					{
						StepTime = 1000.f; // done (the pictures are written by now)
					}
					break;
				}
			},
			[this, L, ReleaseCamera]
			{
				const FFizzle& S = *L;
				if (UPinguSettings* Settings = UPinguSettings::Get(); Settings && S.bPresetSet)
				{
					if (S.PresetBefore.Preset == EPinguGraphicsPreset::Custom)
					{
						Settings->SetGraphics(S.PresetBefore);
					}
					else
					{
						Settings->ApplyPreset(S.PresetBefore.Preset);
					}
				}
				if (Penguin.IsValid() && Penguin->GetTraits())
				{
					Penguin->GetTraits()->SetForcedGamble(-1);
				}
				SetTraitNow(Penguin.Get(), EPinguTrait::None);
				ReleaseCamera();
				PrepareBrawl(2400.f, {});
				const int32 DustStarts = S.DustStartsAfterB - S.DustStartsBase;
				const bool bBurst = S.Snowball.Events == 1 && S.Snowball.Particles >= 8 && S.Snowball.Uses == 0 && S.Snowball.Hits == 0 && S.Snowball.Projectiles == 0
					&& S.Gust.Events == 1 && S.Gust.Particles >= 8 && S.Gust.Uses == 0 && S.Gust.Hits == 0 && S.Gust.Projectiles == 0;
				const bool bIcon = DustStarts == 2 && S.PeakIcons >= 1 && S.PiecesFull >= 12;
				const bool bShine = S.DoubleUses == 1 && S.ShineStarts == 1 && S.PeakShine >= 1;
				const bool bShapes = S.ShapesOk == AbilityCount;
				const bool bLow = S.DensityLow < S.DensityFull - 0.05f && S.ParticlesLow >= 8 && S.ParticlesLow < S.Snowball.Particles && S.PiecesLow >= 4 && S.PiecesLow < S.PiecesFull;
				const bool bOk = bBurst && bIcon && bShine && bShapes && bLow && S.Shots == 5;
				Report(FString::Printf(TEXT("RESULT gambler fizzle: snowball burst %d dust particles (want 8 or more), gust %d; no ability effect: uses %d / %d, hits %d / %d, shots %d / %d (want 0); "
					"icon dust starts %d (want 2), icons crumbling at most %d, dust pieces drawn at most %d, at the icon dust's age %.2f s (want 12 or more); the double: uses %d (want 1), shine starts %d (want 1), icons shining %d; "
					"fizzle of every ability: %d of %d make one dust burst, no effect%s%s; Low (effects %.1f against %.1f): %d particles against %d, %d icon pieces against %d; shots %d (want 5): %s"),
					S.Snowball.Particles, S.Gust.Particles, S.Snowball.Uses, S.Gust.Uses, S.Snowball.Hits, S.Gust.Hits, S.Snowball.Projectiles, S.Gust.Projectiles,
					DustStarts, S.PeakIcons, S.PiecesFull, S.PeakAge, S.DoubleUses, S.ShineStarts, S.PeakShine, S.ShapesOk, AbilityCount, S.Bad.IsEmpty() ? TEXT("") : TEXT(": bad "), *S.Bad,
					S.DensityLow, S.DensityFull, S.ParticlesLow, S.Snowball.Particles, S.PiecesLow, S.PiecesFull, S.Shots, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- The looks on Low and on Ultra: Glass Cannon (ours) and Mirror (the dummy) side by side - their overlays show on
	// every preset. The preset as it was afterwards (a test run saves nothing).
	{
		struct FPresets
		{
			bool bPresetSet = false;
			FPinguGraphicsOptions Before;
			int32 Stage = 0;
			float At = 0.f;
			int32 Shots = 0;
			bool bOverlays[2] = { false, false };
		};
		const TSharedRef<FPresets> L = MakeShared<FPresets>();
		static constexpr float PresetWaitMax = 30.f;
		AddStep(TEXT("look_TraitPresets"), 3.f + 2.f * PresetWaitMax,
			[this, L, HoldCamera]
			{
				*L = FPresets();
				if (UPinguSettings* Settings = UPinguSettings::Get())
				{
					L->Before = Settings->GetGraphics();
					L->bPresetSet = true;
					Settings->ApplyPreset(EPinguGraphicsPreset::Low);
				}
				SpawnTarget();
				PrepareBrawl(170.f, {});
				SetTraitNow(Penguin.Get(), EPinguTrait::GlassCannon);
				SetTraitNow(Target.Get(), EPinguTrait::Mirror);
				L->At = 0.f;
				HoldCamera();
			},
			[this, L](float T)
			{
				FPresets& S = *L;
				if (AActor* Camera = LookCamera.Get())
				{
					const FVector LookAt = Penguin->GetFeetLocation() + FVector(85.f, 0.f, 60.f);
					const FVector From = LookAt + FVector(0.f, 420.f, 40.f);
					Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
				const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
				const bool bReady = T > S.At + 0.8f && (!bCompiling || T > S.At + PresetWaitMax);
				if ((S.Stage == 0 || S.Stage == 1) && bReady)
				{
					S.bOverlays[S.Stage] = HasOverlay(Penguin.Get()) && HasOverlay(Target.Get());
					Shot(S.Stage == 0 ? TEXT("62_trait_low") : TEXT("62_trait_ultra"));
					++S.Shots;
					S.At = T;
					++S.Stage;
					if (S.Stage == 1)
					{
						if (UPinguSettings* Settings = UPinguSettings::Get())
						{
							Settings->ApplyPreset(EPinguGraphicsPreset::Ultra);
						}
					}
				}
				if (S.Stage == 2 && T > S.At + 0.3f)
				{
					StepTime = 1000.f; // done (the picture is written by now)
				}
			},
			[this, L, ReleaseCamera]
			{
				const FPresets& S = *L;
				if (UPinguSettings* Settings = UPinguSettings::Get(); Settings && S.bPresetSet)
				{
					if (S.Before.Preset == EPinguGraphicsPreset::Custom)
					{
						Settings->SetGraphics(S.Before);
					}
					else
					{
						Settings->ApplyPreset(S.Before.Preset);
					}
				}
				SetTraitNow(Penguin.Get(), EPinguTrait::None);
				SetTraitNow(Target.Get(), EPinguTrait::None);
				ReleaseCamera();
				const bool bOk = S.Shots == 2 && S.bOverlays[0] && S.bOverlays[1];
				Report(FString::Printf(TEXT("RESULT trait presets: shots %d (want 2), Glass Cannon and Mirror overlays on Low %d, on Ultra %d (want 1 1): %s"),
					S.Shots, S.bOverlays[0] ? 1 : 0, S.bOverlays[1] ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}
}

// ============================================================================ the lobby test: Traits mode

void APinguLobbyTest::UpdateTraitsStage(APenguinGameState* GameState, APenguinHUD* LobbyHUD, APenguinGameMode* GameMode,
	TFunctionRef<void(bool, const FString&)> Check)
{
	const EPinguPhase Phase = GameState->GetPhase();
	const auto NextSub = [this]() { ++TraitSubStep; StageTime = 0.f; };
	switch (TraitSubStep)
	{
	case 0: // the title: PLAY
		if (Phase == EPinguPhase::Menu && StageTime > 0.5f)
		{
			LobbyHUD->Action_Play();
			NextSub();
		}
		break;

	case 1: // the lobby: TRAITS is off; the toggle
		if (Phase == EPinguPhase::Lobby && StageTime > 0.6f)
		{
			bTraitDealChecked = !LobbyHUD->AreTraitsOn() && !GameState->AreTraitsOn(); // (kept for the check below)
			LobbyHUD->Action_ToggleTraits();
			NextSub();
		}
		else if (StageTime > 10.f)
		{
			Check(false, TEXT("TRAITS: PLAY did not open the lobby"));
			Finish(TEXT("no lobby for the traits part"));
		}
		break;

	case 2: // on now; nine bots (ten penguins)
		if (StageTime > 0.3f)
		{
			const bool bOn = LobbyHUD->AreTraitsOn() && GameState->AreTraitsOn();
			Check(bTraitDealChecked && bOn, FString::Printf(TEXT("TRAITS in the lobby: off at first %d, on after the toggle %d"), bTraitDealChecked ? 1 : 0, bOn ? 1 : 0));
			bTraitDealChecked = false;
			for (int32 Guard = 0; Guard < 12 && LobbyHUD->GetBotCount() < PinguMaxPenguins - 1; ++Guard)
			{
				LobbyHUD->Action_AddBot();
			}
			NextSub();
		}
		break;

	case 3: // a look at the cards (rebuilt for the new bots by now); the host's TRAITS list (LIST): one trait out, all back in; START
		if (!bShotTaken && StageTime > 0.7f)
		{
			bShotTaken = true;
			Shot(TEXT("lobbytest_traits_on"));
			LobbyHUD->Action_TraitPicks();
		}
		if (bShotTaken && TraitListStep == 0 && StageTime > 1.4f)
		{
			TraitListStep = 1;
			const bool bListShown = LobbyHUD->GetScreen() == EPinguScreen::TraitPicks;
			const int32 Before = LobbyHUD->GetTraitPickCount();
			LobbyHUD->Action_ToggleTraitPick(EPinguTrait::HotFeet);
			const bool bOut = !GameState->GetSettings().IsTraitAllowed(EPinguTrait::HotFeet) && LobbyHUD->GetTraitPickCount() == Before - 1;
			Shot(TEXT("lobbytest_trait_list"));
			LobbyHUD->Action_PickAllTraits();
			const bool bAllBack = GameState->GetSettings().TraitsOff == 0 && LobbyHUD->GetTraitPickCount() == PinguNumTraits;
			Check(bListShown && Before == PinguNumTraits && bOut && bAllBack,
				FString::Printf(TEXT("the TRAITS list: shown after LIST %d, %d of %d ticked at first, Hot Feet out %d, ALL ticks them all again %d"),
					bListShown ? 1 : 0, Before, PinguNumTraits, bOut ? 1 : 0, bAllBack ? 1 : 0));
		}
		if (TraitListStep == 1 && StageTime > 2.f)
		{
			TraitListStep = 2;
			LobbyHUD->Action_Back();
		}
		if (TraitListStep == 2 && StageTime > 2.5f)
		{
			Check(LobbyHUD->GetScreen() == EPinguScreen::Lobby, FString::Printf(TEXT("DONE / back from the TRAITS list: the lobby again %d"),
				LobbyHUD->GetScreen() == EPinguScreen::Lobby ? 1 : 0));
			bShotTaken = false;
			TraitListStep = 0;
			LobbyHUD->Action_StartLobbyMatch();
			TraitCardsSeen = 0;
			TraitFlyingSeen = 0;
			TraitIntroLongest = 0.f;
			bTraitCountdownSeen = false;
			NextSub();
		}
		break;

	case 4: // the intro: ten penguins, one trait for all (2026-11-07); its one banner (no cards); the countdown only after it
	{
		if (Phase == EPinguPhase::Intro)
		{
			TraitIntroLongest = FMath::Max(TraitIntroLongest, GameState->GetPhaseTime());
		}
		if (Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown)
		{
			const PinguUI::FTraitRevealStats& Stats = PinguUI::GetTraitRevealStats();
			TraitCardsSeen = FMath::Max(TraitCardsSeen, Stats.Cards);
			TraitFlyingSeen = FMath::Max(TraitFlyingSeen, Stats.Flying);
			if (Stats.bBanner && Stats.Frame + 2 >= GFrameCounter)
			{
				++TraitBannerFrames;
				TraitBannerLines = FMath::Max(TraitBannerLines, Stats.BannerLines);
				bTraitBannerOutside |= Stats.bBannerOutside;
			}
			// The cards' layout while they all show (from 0.7 s into the reveal until the first flies): ten penguins on a big
			// island, some of them out of the picture - their cards line up at the screen's edges.
			const float RevealAge = GameState->GetPhaseTime() - APenguinGameState::TraitRevealDelay;
			if (Phase == EPinguPhase::Intro && RevealAge > 0.7f && RevealAge < UPenguinTraitComponent::CardsTime - 0.05f && Stats.Frame + 2 >= GFrameCounter)
			{
				++TraitLayoutFrames;
				TraitLayoutFewest = FMath::Min(TraitLayoutFewest, Stats.Cards);
				TraitLayoutEdge = FMath::Max(TraitLayoutEdge, Stats.EdgeCards);
				TraitLayoutOverlaps = FMath::Max(TraitLayoutOverlaps, Stats.OverlapPairs);
				TraitLayoutOverMarkers = FMath::Max(TraitLayoutOverMarkers, Stats.CardsOverMarkers);
				TraitLayoutOutside = FMath::Max(TraitLayoutOutside, Stats.CardsOutside);
			}
		}
		if (Phase == EPinguPhase::Intro && !bTraitDealChecked && GameState->GetPhaseTime() > 0.3f)
		{
			bTraitDealChecked = true;
			int32 Penguins = 0;
			TArray<EPinguTrait> Different;
			FString Line;
			for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
			{
				const UPenguinTraitComponent* Traits = It->GetTraits();
				const EPinguTrait Trait = Traits ? Traits->GetTrait() : EPinguTrait::None;
				++Penguins;
				if (PinguTraits::IsReal(Trait))
				{
					Different.AddUnique(Trait);
				}
				Line += FString::Printf(TEXT("%s%s %s"), Line.IsEmpty() ? TEXT("") : TEXT(", "), *It->GetDisplayName(), *PinguTraits::GetLogName(Trait));
			}
			Check(Penguins == PinguMaxPenguins && Different.Num() == 1 && Different[0] == GameState->GetRoundTrait(),
				FString::Printf(TEXT("START with TRAITS on: %d penguins, %d different traits (want 10 1: everybody the round's %s): %s"), Penguins, Different.Num(),
					*PinguTraits::GetLogName(GameState->GetRoundTrait()), *Line));
		}
		if (!bShotTaken && Phase == EPinguPhase::Intro && GameState->GetPhaseTime() > APenguinGameState::TraitRevealDelay + 1.5f)
		{
			bShotTaken = true;
			Shot(TEXT("lobbytest_traits_reveal"));
		}
		if (Phase == EPinguPhase::Fight)
		{
			Check(TraitBannerFrames >= 10 && TraitCardsSeen == 0 && TraitIntroLongest >= 4.8f,
				FString::Printf(TEXT("the reveal: the one logo in %d frames (want 10 or more), cards %d (want 0), the countdown came after %.2f s of intro (want 4.8 or more)"),
					TraitBannerFrames, TraitCardsSeen, TraitIntroLongest));
			Check(TraitBannerLines == 0 && !bTraitBannerOutside,
				FString::Printf(TEXT("the round-start logo: %d words with it (want 0: only the logo, no plank, no text), wholly in the picture %d"), TraitBannerLines,
					bTraitBannerOutside ? 0 : 1));
			NextSub();
		}
		else if (StageTime > 25.f)
		{
			Check(false, TEXT("the traits match's fight never started"));
			NextSub();
		}
		break;
	}

	case 5: // END MATCH from the pause menu
		if (StageTime > 0.6f)
		{
			LobbyHUD->TogglePauseMenu();
			NextSub();
		}
		break;

	case 6:
		if (StageTime > 0.4f)
		{
			LobbyHUD->Action_EndMatch();
			NextSub();
		}
		break;

	case 7: // the results, then LOBBY: TRAITS still on
		if (Phase == EPinguPhase::MatchOver && StageTime > 1.f)
		{
			LobbyHUD->Action_ReturnToLobby();
			NextSub();
		}
		else if (StageTime > 8.f)
		{
			Report(FString::Printf(TEXT("END MATCH did not show the results (phase %s)"), *UEnum::GetValueAsString(Phase)));
			LobbyHUD->Action_ReturnToLobby();
			NextSub();
		}
		break;

	case 8:
		if (Phase == EPinguPhase::Lobby && StageTime > 1.f)
		{
			Check(LobbyHUD->AreTraitsOn() && LobbyHUD->GetScreen() == EPinguScreen::Lobby,
				FString::Printf(TEXT("END MATCH, LOBBY: TRAITS still on %d, lobby screen %d"), LobbyHUD->AreTraitsOn() ? 1 : 0, LobbyHUD->GetScreen() == EPinguScreen::Lobby ? 1 : 0));
			LobbyHUD->Action_ToggleTraits();
			NextSub();
		}
		else if (StageTime > 8.f)
		{
			Check(false, FString::Printf(TEXT("END MATCH, LOBBY: no lobby (phase %s)"), *UEnum::GetValueAsString(Phase)));
			NextSub();
		}
		break;

	case 9:
		if (StageTime > 0.3f)
		{
			Check(!LobbyHUD->AreTraitsOn() && !GameState->AreTraitsOn(), FString::Printf(TEXT("the toggle turns TRAITS off: %s"), LobbyHUD->AreTraitsOn() ? TEXT("still on") : TEXT("off")));
			LobbyHUD->Action_MainMenu();
			NextSub();
		}
		break;

	default:
		if (StageTime > 0.5f)
		{
			Finish(TEXT("done"));
		}
		break;
	}
}
