// Pingu - the tornado (the Gust ability): the autotest's steps (APenguinAutoTest::AddTornadoSteps, AddTornadoLookStep).
//
// ability_Tornado: two dummies, 2 m and 5 m from us, are sucked in for half a second, then thrown out - the near one harder,
// as the push's share at their distances says. tornado_Hover: in the air we hang still for the whole second (drift under
// 2 cm), no other ability meanwhile, our double jump back. tornado_Ice: the strip's ice blocks in reach are ripped out,
// circle us, are thrown out and land as the island's ice again; one comes down on the dummy - knocked out (Crushed) only
// once it reaches its head. tornado_Snowball: the dummy's snowball is sucked in, circles us without hitting us and flies out
// as ours. look_Tornado: the funnel up close on High and Low. Screenshots 65_tornado_*.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguSettings.h"
#include "PinguTornado.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "DynamicRHI.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ShaderCompiler.h"

namespace PenguinAutoTestTornadoPrivate
{
	/** The tornado User made (the newest one), or null. */
	APinguTornado* FindTornado(const UWorld* World, const APenguinCharacter* User)
	{
		TArray<APinguTornado*> All;
		APinguTornado::GetAll(World, All);
		APinguTornado* Found = nullptr;
		for (APinguTornado* Tornado : All)
		{
			if (Tornado->GetUser() == User)
			{
				Found = Tornado;
			}
		}
		return Found;
	}

	/** One dummy watched through ability_Tornado. */
	struct FDummyWatch
	{
		float StartDist = 0.f;
		float DistBeforeHit = -1.f;
		int32 HitsAtStart = 0;
		float HitAt = -1.f;      // s after the cast
		float HitSpeed = 0.f;    // its knock (UPenguinMovementComponent::LastHitSpeed)
		float RecordKnock = 0.f; // the knock the tornado gave it (its record, before the hit's own scaling)
		float RecordShare = 0.f;
		float RecordDist = 0.f;
	};

	float DistToEye(const APenguinCharacter* Who, const FVector& Eye)
	{
		return Who ? static_cast<float>(FVector::Dist(FVector(Who->GetActorLocation().X, 0.f, Who->GetActorLocation().Z), FVector(Eye.X, 0.f, Eye.Z))) : -1.f;
	}

	/** Watches a dummy every frame of the step: its distance from the eye until its hit, the hit's time and knock. */
	void WatchDummy(FDummyWatch& Watch, const APenguinCharacter* Dummy, const FVector& Eye, float SinceCast)
	{
		const UPenguinMovementComponent* DummyMove = Dummy ? Dummy->GetPenguinMovement() : nullptr;
		if (!DummyMove || Watch.HitAt >= 0.f)
		{
			return;
		}
		if (DummyMove->HitEvents != Watch.HitsAtStart)
		{
			Watch.HitAt = SinceCast;
			Watch.HitSpeed = DummyMove->LastHitSpeed;
			return;
		}
		Watch.DistBeforeHit = DistToEye(Dummy, Eye);
	}

	/** The tornado's record of a penguin it threw. */
	void ReadRecord(FDummyWatch& Watch, const APinguTornado* Tornado, const APenguinCharacter* Dummy)
	{
		if (!Tornado || !Dummy)
		{
			return;
		}
		for (const APinguTornado::FThrowRecord& Record : Tornado->GetThrows())
		{
			if (Record.Penguin.Get() == Dummy)
			{
				Watch.RecordKnock = static_cast<float>(Record.Knockback.Size());
				Watch.RecordShare = Record.Share;
				Watch.RecordDist = Record.Distance;
			}
		}
	}
}

void APenguinAutoTest::AddTornadoSteps()
{
	using namespace PenguinAutoTestTornadoPrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	// A second dummy where it is wanted (spawned the first time; it is gone again at the end of each step).
	const auto PlaceSecondDummy = [this](const FVector& Where) -> APenguinCharacter*
	{
		if (!TornadoDummy.IsValid() && Penguin.IsValid())
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			TornadoDummy = GetWorld()->SpawnActor<APenguinCharacter>(Penguin->GetClass(), Where, FRotator::ZeroRotator, Params);
			if (APenguinCharacter* Spawned = TornadoDummy.Get())
			{
				Spawned->SetPlayerSlot(2, TEXT("Dummy 2"));
			}
		}
		APenguinCharacter* Second = TornadoDummy.Get();
		if (Second)
		{
			Second->ResetForRound(Where);
			Second->SetInputLocked(false);
		}
		return Second;
	};
	const auto RemoveSecondDummy = [this]()
	{
		if (APenguinCharacter* Second = TornadoDummy.Get())
		{
			Second->Destroy();
		}
		TornadoDummy.Reset();
	};
	// The stage camera, parked to look at LookAt from the camera's side (Back cm away), or back to its own work.
	const auto ParkCamera = [this](const FVector& LookAt, float Back, float Up)
	{
		if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
		{
			Stage->SetActorTickEnabled(false);
			LookCamera = Stage;
			const FVector From = LookAt + FVector(0.f, Back, Up);
			Stage->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
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

	// ---- ability_Tornado: the dummy 2 m away and a second one 5 m away are sucked in for half a second, then thrown out -
	// the near one harder, by the push's share at their distances then (50 % of the noot's push at the middle down to 15 %
	// at the edge). We stand still on the ground meanwhile.
	{
		struct FAbilityState
		{
			FDummyWatch Near;
			FDummyWatch Far;
			float CastT = -1.f;
			FVector CastAt = FVector::ZeroVector;
			FVector Eye = FVector::ZeroVector;
			float Drift = 0.f;
			bool bUsed = false;
			bool bTornadoSeen = false;
			int32 Shots = 0;
			TWeakObjectPtr<APinguTornado> Tornado;
			float Reach = PinguTuning::TornadoRadius;
		};
		const TSharedRef<FAbilityState> S = MakeShared<FAbilityState>();
		AddStep(TEXT("ability_Tornado"), 2.f,
			[this, S, Base, PlaceSecondDummy, ParkCamera]
			{
				*S = FAbilityState();
				PrepareBrawl(200.f, { EPenguinAbility::Gust });
				PlaceSecondDummy(Base + FVector(500.f, 0.f, 0.f));
				ParkCamera(Base + FVector(260.f, 0.f, 110.f), 1700.f, 160.f);
			},
			[this, S](float T)
			{
				APenguinCharacter* Second = TornadoDummy.Get();
				if (!Target.IsValid() || !Second)
				{
					return;
				}
				if (S->CastT < 0.f && T > 0.25f)
				{
					S->CastT = T;
					S->CastAt = Location();
					S->Eye = Penguin->GetAimOrigin();
					S->Near.StartDist = DistToEye(Target.Get(), S->Eye);
					S->Far.StartDist = DistToEye(Second, S->Eye);
					S->Near.HitsAtStart = Target->GetPenguinMovement()->HitEvents;
					S->Far.HitsAtStart = Second->GetPenguinMovement()->HitEvents;
					S->bUsed = Penguin->ActivateAbility(0);
					S->Tornado = FindTornado(GetWorld(), Penguin.Get());
					S->bTornadoSeen = S->Tornado.IsValid();
					return;
				}
				if (S->CastT < 0.f)
				{
					return;
				}
				const float Since = T - S->CastT;
				if (Since <= PinguTuning::TornadoTime - 0.02f)
				{
					S->Drift = FMath::Max(S->Drift, static_cast<float>(FVector::Dist(Location(), S->CastAt)));
				}
				WatchDummy(S->Near, Target.Get(), S->Eye, Since);
				WatchDummy(S->Far, Second, S->Eye, Since);
				// Its records of the two, once it has thrown them (it is gone a little after its second).
				if (Since > PinguTuning::TornadoSuckTime + 0.05f && Since < PinguTuning::TornadoTime)
				{
					ReadRecord(S->Near, S->Tornado.Get(), Target.Get());
					ReadRecord(S->Far, S->Tornado.Get(), Second);
					if (const APinguTornado* Tornado = S->Tornado.Get())
					{
						S->Reach = Tornado->GetRadius();
					}
				}
				if (S->Shots == 0 && Since > 0.3f)
				{
					++S->Shots;
					Shot(TEXT("65_tornado_suck"));
				}
				if (S->Shots == 1 && Since > 0.56f)
				{
					++S->Shots;
					Shot(TEXT("65_tornado_throw"));
				}
			},
			[this, S, RemoveSecondDummy, ReleaseCamera]
			{
				ReleaseCamera();
				RemoveSecondDummy();
				const FDummyWatch& N = S->Near;
				const FDummyWatch& F = S->Far;
				const float PulledNear = N.StartDist - N.DistBeforeHit;
				const float PulledFar = F.StartDist - F.DistBeforeHit;
				const float Measured = F.HitSpeed > 1.f ? N.HitSpeed / F.HitSpeed : 0.f;
				const float Expected = F.RecordKnock > 1.f ? N.RecordKnock / F.RecordKnock : 0.f;
				// The shares the push's rule gives at the distances the dummies had at the throw.
				const float Reach = S->Reach;
				const auto Rule = [Reach](float Distance) { return FMath::Lerp(PinguTuning::TornadoPushCenterShare, PinguTuning::TornadoPushEdgeShare, FMath::Clamp(Distance / Reach, 0.f, 1.f)); };
				const bool bShares = FMath::IsNearlyEqual(N.RecordShare, Rule(N.RecordDist), 0.005f) && FMath::IsNearlyEqual(F.RecordShare, Rule(F.RecordDist), 0.005f);
				const bool bOnTime = FMath::Abs(N.HitAt - PinguTuning::TornadoSuckTime) < 0.05f && FMath::Abs(F.HitAt - PinguTuning::TornadoSuckTime) < 0.05f;
				const bool bOk = S->bUsed && S->bTornadoSeen && PulledNear > 40.f && PulledFar > 40.f && bOnTime && N.HitSpeed > F.HitSpeed && Expected > 1.f
					&& FMath::Abs(Measured / Expected - 1.f) < 0.12f && bShares && S->Drift < 2.f && S->Shots == 2;
				Report(FString::Printf(TEXT("RESULT tornado: used %d, near dummy %.0f -> %.0f cm from the eye before the throw (pulled %.0f, want over 40), far %.0f -> %.0f (pulled %.0f), "
					"thrown %.2f / %.2f s after the cast (want %.2f), at %.0f / %.0f cm/s: x%.2f (want x%.2f from its shares %.3f / %.3f at %.0f / %.0f cm, within 12 %%; the rule's shares %d), "
					"we moved %.2f cm (want under 2): %s"),
					S->bUsed ? 1 : 0, N.StartDist, N.DistBeforeHit, PulledNear, F.StartDist, F.DistBeforeHit, PulledFar, N.HitAt, F.HitAt, PinguTuning::TornadoSuckTime,
					N.HitSpeed, F.HitSpeed, Measured, Expected, N.RecordShare, F.RecordShare, N.RecordDist, F.RecordDist, bShares ? 1 : 0, S->Drift,
					bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- tornado_Hover: in the air we hang perfectly still in the eye for the whole second, with no other ability meanwhile
	// (our snowball refused) and our double jump back; then we fall and the snowball works again.
	{
		struct FHoverState
		{
			float CastT = -1.f;
			FVector CastAt = FVector::ZeroVector;
			float Drift = 0.f;
			int32 TriedDuring = 0;
			int32 UsedDuring = 0;
			int32 UsedAfter = -1;
			bool bHovering = false;
			int32 AirJumps = -1;
			float FellAfter = 0.f;
			bool bShot = false;
		};
		const TSharedRef<FHoverState> S = MakeShared<FHoverState>();
		AddStep(TEXT("tornado_Hover"), 1.9f,
			[this, S, Base, ParkCamera]
			{
				*S = FHoverState();
				PrepareBrawl(2400.f, { EPenguinAbility::Gust, EPenguinAbility::Snowball });
				Teleport(Base + FVector(0.f, 0.f, 330.f)); // in the air, falling
				ParkCamera(Base + FVector(0.f, 0.f, 330.f), 1100.f, 60.f);
			},
			[this, S, ParkCamera](float T)
			{
				if (S->CastT < 0.f)
				{
					if (T > 0.12f)
					{
						S->CastT = T;
						S->CastAt = Location();
						Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(300.f, 0.f, 0.f));
						Penguin->ActivateAbility(0);
						ParkCamera(S->CastAt + FVector(0.f, 0.f, 90.f), 1100.f, 60.f); // the funnel round it in the picture
					}
					return;
				}
				const float Since = T - S->CastT;
				if (Since <= PinguTuning::TornadoTime - 0.02f)
				{
					S->Drift = FMath::Max(S->Drift, static_cast<float>(FVector::Dist(Location(), S->CastAt)));
				}
				if ((S->TriedDuring == 0 && Since > 0.3f) || (S->TriedDuring == 1 && Since > 0.9f))
				{
					++S->TriedDuring;
					S->bHovering = Move->IsHovering();
					S->AirJumps = Move->GetAirJumpsLeft();
					S->UsedDuring += Penguin->ActivateAbility(1) ? 1 : 0;
				}
				if (!S->bShot && Since > 0.4f)
				{
					S->bShot = true;
					Shot(TEXT("65_tornado_hover"));
				}
				if (S->UsedAfter < 0 && Since > PinguTuning::TornadoTime + 0.1f)
				{
					S->UsedAfter = Penguin->ActivateAbility(1) ? 1 : 0;
				}
				if (Since > PinguTuning::TornadoTime + 0.4f)
				{
					S->FellAfter = FMath::Max(S->FellAfter, static_cast<float>(S->CastAt.Z - Location().Z));
				}
			},
			[this, S, ReleaseCamera]
			{
				ReleaseCamera();
				const bool bOk = S->CastT >= 0.f && S->Drift < 2.f && S->bHovering && S->TriedDuring == 2 && S->UsedDuring == 0 && S->UsedAfter == 1
					&& S->FellAfter > 20.f && S->AirJumps > 0;
				Report(FString::Printf(TEXT("RESULT tornado hover: in the air it moved %.2f cm in its second (want under 2), hanging in its eye %d, another ability during it %d of %d (want 0), "
					"after it %d (want 1), fell %.0f cm after it (want over 20), double jump back %d: %s"),
					S->Drift, S->bHovering ? 1 : 0, S->UsedDuring, S->TriedDuring, S->UsedAfter, S->FellAfter, S->AirJumps > 0 ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- tornado_Ice: two ice columns (two blocks each) 2.5 m to either side are ripped out of the strip, circle us and are
	// thrown out; they land as the strip's ice again. One of them is about to come down: the dummy is put right where it
	// will - knocked out (Crushed, by the block of our tornado) only once the ice reaches its head, not while it is above.
	{
		struct FIceState
		{
			float CastT = -1.f;
			int32 IceBefore = 0;
			int32 IceLeftAfterRip = -1;
			int32 MaxSwirling = 0;
			int32 Ripped = 0;
			int32 ThrownOut = 0;
			TWeakObjectPtr<AHookedIceBlock> Watched;
			bool bPlaced = false;
			float PlacedHead = 0.f;     // the dummy's head top when it was put there
			float BlockAboveAtPlace = 0.f;
			float ClosestAlive = 1.e9f; // the nearest the block's bottom came to the head while the dummy was still in
			float GapAtKO = 1.e9f;      // the block's bottom over the head when it was knocked out
			float KOT = -1.f;
			EPenguinKOReason Reason = EPenguinKOReason::None;
			int32 Crushed = 0;
			bool bByUs = false;
			int32 StillFlying = 0;
			int32 IceAfter = 0;
			int32 Shots = 0;
		};
		const TSharedRef<FIceState> S = MakeShared<FIceState>();
		static constexpr float ColumnOffset = 250.f;
		AddStep(TEXT("tornado_Ice"), 3.2f,
			[this, S, Base, ParkCamera]
			{
				*S = FIceState();
				PrepareBrawl(2400.f, { EPenguinAbility::Gust });
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f)))
				{
					TArray<FIntPoint> Cells;
					for (const float Side : { -ColumnOffset, ColumnOffset })
					{
						const FIntPoint Foot = Flat->WorldToCell(FVector(Base.X + Side, 0.f, 50.f));
						Cells.Add(Foot);
						Cells.Add(FIntPoint(Foot.X, Foot.Y + 1));
					}
					Flat->AddBlocks(Cells, EBlockKind::Ice, 0.f);
				}
				S->IceBefore = CountIce(static_cast<float>(Base.X) - ColumnOffset - 60.f, 2.f * ColumnOffset + 120.f, 3, nullptr);
				ParkCamera(Base + FVector(0.f, 0.f, 160.f), 1500.f, 120.f);
			},
			[this, S, Base](float T)
			{
				if (S->CastT < 0.f)
				{
					if (T > 0.3f)
					{
						S->CastT = T;
						Penguin->ActivateAbility(0);
					}
					return;
				}
				const float Since = T - S->CastT;
				int32 Swirling = 0;
				int32 Flying = 0;
				for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
				{
					if (It->IsFromTornado() && !It->HasLanded())
					{
						Swirling += It->IsSwirling() ? 1 : 0;
						Flying += It->IsSwirling() ? 0 : 1;
					}
				}
				S->MaxSwirling = FMath::Max(S->MaxSwirling, Swirling);
				S->ThrownOut = FMath::Max(S->ThrownOut, Flying);
				if (S->IceLeftAfterRip < 0 && Since > 0.05f)
				{
					S->IceLeftAfterRip = CountIce(static_cast<float>(Base.X) - ColumnOffset - 60.f, 2.f * ColumnOffset + 120.f, 3, nullptr);
					if (const APinguTornado* Tornado = FindTornado(GetWorld(), Penguin.Get()))
					{
						S->Ripped = Tornado->GetIceRipped();
					}
				}
				if (S->Shots == 0 && Since > 0.35f)
				{
					++S->Shots;
					Shot(TEXT("65_tornado_ice_swirl"));
				}
				if (S->Shots == 1 && Since > 0.62f)
				{
					++S->Shots;
					Shot(TEXT("65_tornado_ice_thrown"));
				}
				// Thrown: the dummy is put where one block will come down - standing on the strip, the block coming down onto its
				// head (worked out from where it is now and its own flight: its gravity, its sideways speed), one that comes down
				// well away from where any other one does.
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
				if (!S->bPlaced && Since > PinguTuning::TornadoSuckTime + 0.06f && Target.IsValid() && Flat)
				{
					const float Half = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
					const float StandZ = 2.f + Half; // on the strip (its top: z 0)
					const float HeadTop = StandZ + Half;
					struct FLanding
					{
						AHookedIceBlock* Block;
						float X;
						float Above;
						float Fall;
					};
					TArray<FLanding> Landings;
					for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
					{
						if (!It->IsFromTornado() || It->IsSwirling() || It->HasLanded())
						{
							continue;
						}
						const FVector At = It->GetCenter();
						const FVector V = It->GetVelocity();
						// The block's middle is CellSize / 2 over its bottom: down to the head top + that, falling like a block.
						const float Drop = static_cast<float>(At.Z) - (HeadTop + Flat->CellSize * 0.5f);
						const float G = ABlockIsland::FallingBlockGravity;
						const float Vz = static_cast<float>(V.Z);
						const float Fall = (Vz + FMath::Sqrt(FMath::Max(Vz * Vz + 2.f * G * Drop, 0.f))) / G;
						Landings.Add({ *It, static_cast<float>(At.X + V.X * Fall), static_cast<float>(At.Z) - Flat->CellSize * 0.5f - HeadTop, Fall });
					}
					const FLanding* Best = nullptr;
					float BestApart = 0.f;
					for (const FLanding& Landing : Landings)
					{
						float Apart = 1.e9f;
						for (const FLanding& Other : Landings)
						{
							if (Other.Block != Landing.Block)
							{
								Apart = FMath::Min(Apart, FMath::Abs(Other.X - Landing.X));
							}
						}
						if (Landing.Above > 100.f && Apart > BestApart)
						{
							BestApart = Apart;
							Best = &Landing;
						}
					}
					if (Best)
					{
						S->bPlaced = true;
						S->Watched = Best->Block;
						ResetTarget(FVector(Best->X, 0.f, StandZ));
						S->PlacedHead = HeadTop;
						S->BlockAboveAtPlace = Best->Above;
						Report(FString::Printf(TEXT("tornado ice: the dummy put %.0f cm along, under a block %.0f cm over its head (falling in about %.2f s, the next block's landing %.0f cm away)"),
							Best->X - static_cast<float>(Base.X), Best->Above, Best->Fall, BestApart));
					}
				}
				// Every frame it stands there: how close the tornado's ice over it has come (any block over its middle: the
				// gap from its bottom to the head), and the gap when it was knocked out.
				if (S->bPlaced && S->KOT < 0.f && Target.IsValid() && Flat)
				{
					const float DummyX = static_cast<float>(Target->GetActorLocation().X);
					float Gap = 1.e9f;
					for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
					{
						if (It->IsFromTornado() && FMath::Abs(static_cast<float>(It->GetCenter().X) - DummyX) < Flat->CellSize * 0.5f)
						{
							Gap = FMath::Min(Gap, static_cast<float>(It->GetCenter().Z) - Flat->CellSize * 0.5f - S->PlacedHead);
						}
					}
					if (Target->IsAlive())
					{
						S->ClosestAlive = FMath::Min(S->ClosestAlive, Gap);
					}
					else
					{
						S->KOT = Since;
						S->Reason = Target->GetKOReason();
						S->GapAtKO = Gap;
						for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
						{
							if (It->IsFromTornado() && It->GetPenguinsCrushed() > 0)
							{
								S->Crushed += It->GetPenguinsCrushed();
								S->bByUs = It->GetThrower() == Penguin.Get();
							}
						}
						if (S->Shots == 2)
						{
							++S->Shots;
							Shot(TEXT("65_tornado_ice_crush"));
						}
					}
				}
			},
			[this, S, Base, ReleaseCamera]
			{
				ReleaseCamera();
				for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
				{
					S->StillFlying += It->IsFromTornado() && !It->HasLanded() ? 1 : 0;
				}
				// Where they came down: anywhere on the strip near us (the floor row and a couple above).
				S->IceAfter = CountIce(static_cast<float>(Base.X) - 1500.f, 3000.f, 4, nullptr);
				// Knocked out only when a block of the tornado reached its head (within a frame's fall), never by ice still above
				// it - and no block went down through it without knocking it out.
				const bool bContact = S->KOT >= 0.f && S->GapAtKO < 60.f && S->ClosestAlive > -60.f && S->BlockAboveAtPlace > 100.f;
				const bool bOk = S->CastT >= 0.f && S->IceBefore == 4 && S->Ripped == 4 && S->IceLeftAfterRip == 0 && S->MaxSwirling == 4 && S->ThrownOut >= 1 && bContact
					&& S->Reason == EPenguinKOReason::Crushed && S->Crushed == 1 && S->bByUs && S->StillFlying == 0 && S->IceAfter == 4;
				Report(FString::Printf(TEXT("RESULT tornado ice: ice before %d (want 4), ripped out %d, left in the columns %d (want 0), circling %d at once (want 4), thrown out %d; "
					"the dummy under a block %.0f cm over its head: knocked out %.2f s after the cast with the ice %.0f cm over its head (want under 60: contact), the nearest the ice came while it "
					"was still in %.0f cm (want over -60), reason %s (want Crushed), crushed by our tornado's blocks %d (want 1), ours %d; still flying at the end %d (want 0), the strip's ice after %d (want 4): %s"),
					S->IceBefore, S->Ripped, S->IceLeftAfterRip, S->MaxSwirling, S->ThrownOut, S->BlockAboveAtPlace, S->KOT, S->GapAtKO, S->ClosestAlive,
					S->Reason == EPenguinKOReason::Crushed ? TEXT("Crushed") : TEXT("other"), S->Crushed, S->bByUs ? 1 : 0, S->StillFlying, S->IceAfter, bOk ? TEXT("ok") : TEXT("FAIL")));
				if (Target.IsValid())
				{
					Target->ResetForRound(Target->GetActorLocation());
				}
			});
	}

	// ---- tornado_Snowball: the dummy's snowball comes past 5 m away: sucked in, it circles us without hitting us and flies
	// out at the throw as our snowball, fast.
	{
		struct FBallState
		{
			float CastT = -1.f;
			TWeakObjectPtr<APenguinProjectile> Ball;
			float StartDist = 0.f;
			float MinDist = 1.e9f;
			bool bAliveAtThrow = false;
			float ThrownSpeed = 0.f;
			bool bOurs = false;
			int32 OwnHits = 0;
			bool bShot = false;
		};
		const TSharedRef<FBallState> S = MakeShared<FBallState>();
		AddStep(TEXT("tornado_Snowball"), 1.7f,
			[this, S, Base, ParkCamera]
			{
				*S = FBallState();
				PrepareBrawl(2400.f, { EPenguinAbility::Gust });
				OwnHitsAtStart = Move->HitEvents;
				ParkCamera(Base + FVector(200.f, 0.f, 110.f), 1300.f, 120.f);
			},
			[this, S, Base](float T)
			{
				if (S->CastT < 0.f)
				{
					if (T > 0.3f && Target.IsValid())
					{
						S->CastT = T;
						Penguin->ActivateAbility(0);
						S->Ball = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Target.Get(), Base + FVector(520.f, 0.f, 70.f), FVector(-700.f, 0.f, 150.f));
						S->StartDist = 520.f;
					}
					return;
				}
				const float Since = T - S->CastT;
				const APenguinProjectile* Ball = S->Ball.Get();
				const FVector Eye = Penguin->GetAimOrigin();
				if (Ball && Since < PinguTuning::TornadoSuckTime)
				{
					S->MinDist = FMath::Min(S->MinDist, static_cast<float>(FVector::Dist(Ball->GetActorLocation(), Eye)));
					S->bAliveAtThrow = Ball->IsFlying();
				}
				if (Ball && Since > PinguTuning::TornadoSuckTime + 0.05f && S->ThrownSpeed <= 0.f)
				{
					S->ThrownSpeed = static_cast<float>(Ball->GetVelocity().Size());
					S->bOurs = Ball->GetShooter() == Penguin.Get();
				}
				if (!S->bShot && Since > 0.35f)
				{
					S->bShot = true;
					Shot(TEXT("65_tornado_snowball"));
				}
			},
			[this, S, ReleaseCamera]
			{
				ReleaseCamera();
				S->OwnHits = Move->HitEvents - OwnHitsAtStart;
				const bool bOk = S->CastT >= 0.f && S->MinDist < 250.f && S->bAliveAtThrow && S->ThrownSpeed > 1500.f && S->bOurs && S->OwnHits == 0;
				Report(FString::Printf(TEXT("RESULT tornado snowball: the dummy's snowball sucked in from %.0f to %.0f cm of our chest (want under 250), still flying at the throw %d, "
					"thrown out at %.0f cm/s (want over 1500) as our snowball %d, we were hit %d (want 0): %s"),
					S->StartDist, S->MinDist, S->bAliveAtThrow ? 1 : 0, S->ThrownSpeed, S->bOurs ? 1 : 0, S->OwnHits, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- tornado_Merge: we and the dummy, 8 m apart, start a tornado at the same moment. Their reaches overlap (6 + 6 m): they
	// suck each other in - both eyes and both of us drift to the middle in 0.3 s - and become one giant tornado: reach 12 m, its
	// push at the middle twice a plain one's, both of us side by side in its eye, never hit by it, hanging there until it ends
	// (0.5 s after our own would have: 1.5 s after the cast); it throws 0.5 s after the merge. A second dummy 10.5 m from the
	// middle (6.5 m from the nearest of us: out of a plain tornado's reach) is sucked in and thrown - the dummy's hit (its own
	// tornado started nearer). Ice: a column 2.5 m outside each of us, ripped out by our own tornadoes, swirls on in the giant;
	// a third one 11 m from the middle (7 m from us) only the giant reaches. Screenshots 65_tornado_merge_1..4.
	{
		struct FMergeState
		{
			float CastT = -1.f;
			bool bOursUsed = false;
			bool bTheirsUsed = false;
			int32 TornadoesAtCast = 0;
			float SinglePush = 0.f;
			float SingleRadius = 0.f;
			int32 RippedBeforeMerge = 0;
			FVector Middle = FVector::ZeroVector;
			TWeakObjectPtr<APinguTornado> Giant;
			float MergedAt = -1.f;
			int32 TornadoesAfter = -1;
			int32 MostAfter = 0;
			float Radius = 0.f;
			float Falloff = 0.f;
			float Strength = 0.f;
			float GiantPush = 0.f;
			float ThrowAge = 0.f;
			float EndAge = 0.f;
			int32 Users = 0;
			float EyeOff = -1.f;
			int32 HoverFrames = 0;
			int32 NotHovering = 0;
			float Gap = -1.f;
			float GapWant = 0.f;
			float FromEye = -1.f;
			int32 OwnHitsStart = 0;
			int32 TheirHitsStart = 0;
			FDummyWatch Far;
			bool bFarByThem = false;
			bool bFarRecorded = false;
			float FarKnockWant = 0.f;
			bool bThereAt13 = false;
			float GoneAt = -1.f;
			int32 IceRipped = 0;
			int32 MostSwirling = 0;
			float IceFarthest = -1.f;
			int32 Shots = 0;
		};
		const TSharedRef<FMergeState> S = MakeShared<FMergeState>();
		static constexpr float Apart = 800.f;
		static constexpr float FarFromMiddle = 1050.f;
		static constexpr float OwnIceOut = 250.f;
		static constexpr float GiantIceOut = 1100.f;
		AddStep(TEXT("tornado_Merge"), 2.4f,
			[this, S, Base, PlaceSecondDummy, ParkCamera]
			{
				*S = FMergeState();
				PrepareBrawl(Apart, { EPenguinAbility::Gust });
				if (Target.IsValid())
				{
					Target->GetAbilities()->SetAbilities({ EPenguinAbility::Gust });
					S->TheirHitsStart = Target->GetPenguinMovement()->HitEvents;
				}
				S->OwnHitsStart = Move->HitEvents;
				const float MiddleX = static_cast<float>(Base.X) + Apart * 0.5f;
				PlaceSecondDummy(FVector(MiddleX + FarFromMiddle, 0.f, Base.Z));
				if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f)))
				{
					TArray<FIntPoint> Cells;
					for (const float X : { static_cast<float>(Base.X) - OwnIceOut, static_cast<float>(Base.X) + Apart + OwnIceOut, MiddleX - GiantIceOut })
					{
						const FIntPoint Foot = Flat->WorldToCell(FVector(X, 0.f, 50.f));
						Cells.Add(Foot);
						Cells.Add(FIntPoint(Foot.X, Foot.Y + 1));
					}
					Flat->AddBlocks(Cells, EBlockKind::Ice, 0.f);
				}
				ParkCamera(FVector(MiddleX, 0.f, 380.f), 3500.f, 300.f); // the whole giant's reach: the far dummy and the far ice too
			},
			[this, S](float T)
			{
				APenguinCharacter* Second = TornadoDummy.Get();
				if (!Target.IsValid() || !Second)
				{
					return;
				}
				if (S->CastT < 0.f)
				{
					if (T > 0.3f)
					{
						S->CastT = T;
						S->Middle = (Penguin->GetAimOrigin() + Target->GetAimOrigin()) * 0.5f;
						S->Middle.Y = 0.f;
						S->bTheirsUsed = Target->ActivateAbility(0);
						S->bOursUsed = Penguin->ActivateAbility(0);
						TArray<APinguTornado*> All;
						APinguTornado::GetAll(GetWorld(), All);
						S->TornadoesAtCast = All.Num();
						for (const APinguTornado* Each : All)
						{
							S->RippedBeforeMerge += Each->GetIceRipped();
						}
						if (const APinguTornado* Ours = FindTornado(GetWorld(), Penguin.Get()))
						{
							S->SinglePush = Ours->GetPushAt(0.f);
							S->SingleRadius = Ours->GetRadius();
						}
						S->Far.StartDist = DistToEye(Second, S->Middle);
						S->Far.HitsAtStart = Second->GetPenguinMovement()->HitEvents;
					}
					return;
				}
				const float Since = T - S->CastT;
				TArray<APinguTornado*> All;
				APinguTornado::GetAll(GetWorld(), All);
				APinguTornado* Giant = nullptr;
				for (APinguTornado* Each : All)
				{
					Giant = Each->GetMerges() > 0 ? Each : Giant;
				}
				if (Giant && !S->Giant.IsValid())
				{
					S->Giant = Giant;
					S->MergedAt = Since;
				}
				if (S->Giant.IsValid())
				{
					S->MostAfter = FMath::Max(S->MostAfter, All.Num());
				}
				if (S->TornadoesAfter < 0 && Since > PinguTuning::TornadoMergeDriftTime + 0.1f)
				{
					S->TornadoesAfter = All.Num();
				}
				const FVector Eye = Giant ? Giant->GetCenter() : S->Middle;
				if (Giant)
				{
					S->Radius = Giant->GetRadius();
					S->Falloff = Giant->GetFalloffRadius();
					S->Strength = Giant->GetStrength();
					S->GiantPush = Giant->GetPushAt(0.f);
					S->ThrowAge = Giant->GetThrowAge();
					S->EndAge = Giant->GetEndAge();
					S->Users = Giant->GetUsers().Num();
					S->EyeOff = static_cast<float>(FVector::Dist(FVector(Eye.X, 0.f, Eye.Z), S->Middle));
					S->IceRipped = Giant->GetIceRipped();
					S->bThereAt13 |= Since > 1.3f && Since < 1.4f;
					// Ripped ice from both, swirling in the giant before its throw: how many, and how far out the farthest one is.
					if (Since < S->ThrowAge - 0.02f)
					{
						int32 Swirling = 0;
						float Farthest = 0.f;
						for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
						{
							if (It->IsFromTornado() && It->IsSwirling())
							{
								++Swirling;
								Farthest = FMath::Max(Farthest, static_cast<float>(FVector::Dist(FVector(It->GetCenter().X, 0.f, It->GetCenter().Z), FVector(Eye.X, 0.f, Eye.Z))));
							}
						}
						S->MostSwirling = FMath::Max(S->MostSwirling, Swirling);
						if (Since > S->ThrowAge - 0.1f)
						{
							S->IceFarthest = FMath::Max(S->IceFarthest, Farthest);
						}
					}
				}
				else if (S->MergedAt >= 0.f && S->GoneAt < 0.f)
				{
					S->GoneAt = Since;
				}
				// Both of us hang in it the whole time (until 0.05 s before its end), side by side in its eye once they are one.
				if (Since > 0.05f && Since < (S->EndAge > 0.f ? S->EndAge : PinguTuning::TornadoTime + PinguTuning::TornadoMergeExtraTime) - 0.05f)
				{
					++S->HoverFrames;
					S->NotHovering += Move->IsHovering() && Target->GetPenguinMovement()->IsHovering() ? 0 : 1;
				}
				if (S->Gap < 0.f && Since > PinguTuning::TornadoMergeDriftTime + 0.15f && Giant)
				{
					S->Gap = FMath::Abs(static_cast<float>(Target->GetActorLocation().X - Location().X));
					S->GapWant = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius() + Target->GetCapsuleComponent()->GetScaledCapsuleRadius() + PinguTuning::TornadoMergeEyeGap;
					const FVector MidChest = (Penguin->GetAimOrigin() + Target->GetAimOrigin()) * 0.5f;
					S->FromEye = static_cast<float>(FVector::Dist(FVector(MidChest.X, 0.f, MidChest.Z), FVector(Eye.X, 0.f, Eye.Z)));
				}
				// The far dummy: its distance from the middle until its hit, the hit; the giant's record of it.
				WatchDummy(S->Far, Second, Eye, Since);
				if (Giant && !S->bFarRecorded && Giant->HasThrown())
				{
					for (const APinguTornado::FThrowRecord& Record : Giant->GetThrows())
					{
						if (Record.Penguin.Get() == Second)
						{
							S->bFarRecorded = true;
							S->bFarByThem = Record.By.Get() == Target.Get();
							S->Far.RecordShare = Record.Share;
							S->Far.RecordDist = Record.Distance;
							S->Far.RecordKnock = static_cast<float>(Record.Knockback.Size());
							const APenguinGameState* Flow = APenguinGameState::Get(this);
							S->FarKnockWant = S->Far.RecordKnock * Giant->GetStrength() * (Flow ? Flow->GetKnockbackScale() : 1.f);
						}
					}
				}
				const float ShotAt[] = { 0.15f, PinguTuning::TornadoMergeDriftTime + 0.12f, (S->ThrowAge > 0.f ? S->ThrowAge : 0.8f) + 0.06f, 1.2f };
				const TCHAR* ShotNames[] = { TEXT("65_tornado_merge_1_drift"), TEXT("65_tornado_merge_2_giant"), TEXT("65_tornado_merge_3_throw"), TEXT("65_tornado_merge_4_after") };
				if (S->Shots < 4 && Since > ShotAt[S->Shots])
				{
					Shot(ShotNames[S->Shots]);
					++S->Shots;
				}
			},
			[this, S, RemoveSecondDummy, ReleaseCamera]
			{
				ReleaseCamera();
				RemoveSecondDummy();
				const int32 OwnHits = Move->HitEvents - S->OwnHitsStart;
				const int32 TheirHits = Target.IsValid() ? Target->GetPenguinMovement()->HitEvents - S->TheirHitsStart : -1;
				const FDummyWatch& F = S->Far;
				const float Pulled = F.StartDist - F.DistBeforeHit;
				const float Rule = FMath::Lerp(PinguTuning::TornadoPushCenterShare, PinguTuning::TornadoPushEdgeShare, FMath::Clamp(F.RecordDist / FMath::Max(S->Falloff, 1.f), 0.f, 1.f));
				const float PushTimes = S->SinglePush > 1.f ? S->GiantPush / S->SinglePush : 0.f;
				const bool bMerged = S->bOursUsed && S->bTheirsUsed && S->TornadoesAtCast == 2 && S->MergedAt >= 0.f && S->MergedAt < PinguTuning::TornadoMergeDriftTime + 0.05f
					&& S->TornadoesAfter == 1 && S->MostAfter == 1 && S->Users == 2;
				const bool bSize = FMath::IsNearlyEqual(S->Radius, 2.f * S->SingleRadius, 1.f) && FMath::IsNearlyEqual(S->Falloff, 2.f * PinguTuning::TornadoRadius, 1.f)
					&& FMath::IsNearlyEqual(PushTimes, 2.f, 0.02f) && S->EyeOff < 5.f;
				const bool bTimes = FMath::Abs(S->ThrowAge - (PinguTuning::TornadoMergeDriftTime + PinguTuning::TornadoMergeSuckTime)) < 0.05f
					&& FMath::Abs(S->EndAge - (PinguTuning::TornadoTime + PinguTuning::TornadoMergeExtraTime)) < 0.05f && S->bThereAt13 && S->GoneAt > S->EndAge;
				const bool bUsers = S->HoverFrames > 60 && S->NotHovering == 0 && OwnHits == 0 && TheirHits == 0 && FMath::Abs(S->Gap - S->GapWant) < 10.f && S->FromEye < 10.f;
				const bool bFar = F.StartDist > PinguTuning::TornadoRadius + 300.f && Pulled > 40.f && F.HitAt > 0.f && FMath::Abs(F.HitAt - S->ThrowAge) < 0.05f && S->bFarRecorded
					&& FMath::IsNearlyEqual(F.RecordShare, Rule, 0.005f) && S->bFarByThem && FMath::Abs(F.HitSpeed / FMath::Max(S->FarKnockWant, 1.f) - 1.f) < 0.05f;
				const bool bIce = S->RippedBeforeMerge == 4 && S->IceRipped == 6 && S->MostSwirling == 6 && S->IceFarthest > 0.f && S->IceFarthest < 500.f;
				const bool bOk = bMerged && bSize && bTimes && bUsers && bFar && bIce && S->Shots == 4;
				Report(FString::Printf(TEXT("RESULT tornado merge: started %d / %d, %d tornadoes at the cast, one giant %.2f s after (want under %.2f) with %d users (want 2), %d tornado(es) after "
					"(want 1, at most %d); reach %.0f cm (want 2 x %.0f), falloff %.0f, push at its middle %.0f cm/s = x%.2f a plain one's %.0f (want x2.00), its eye %.1f cm from halfway; throws %.2f s "
					"after the cast (want %.2f), ends at %.2f (want %.2f), still there at 1.3 s %d, gone at %.2f; we both hung in it on %d of %d frames (want all), hit %d / %d (want 0 / 0), "
					"%.0f cm apart in its eye (want %.0f), our middle %.1f cm from its eye; the far dummy %.0f -> %.0f cm from the middle (pulled %.0f, want over 40), thrown %.2f s after the cast "
					"(want at the throw) at %.0f cm/s (want %.0f: its knock %.0f x the push x%.2f), its share %.3f at %.0f cm (the rule's %.3f), the dummy's hit %d (want 1); ice ripped %d "
					"before the merge (want 4), %d in all (want 6), %d swirling in the giant at once (want 6), the farthest %.0f cm from its eye before the throw (want under 500): %s"),
					S->bOursUsed ? 1 : 0, S->bTheirsUsed ? 1 : 0, S->TornadoesAtCast, S->MergedAt, PinguTuning::TornadoMergeDriftTime + 0.05f, S->Users, S->TornadoesAfter, S->MostAfter,
					S->Radius, S->SingleRadius, S->Falloff, S->GiantPush, PushTimes, S->SinglePush, S->EyeOff, S->ThrowAge,
					PinguTuning::TornadoMergeDriftTime + PinguTuning::TornadoMergeSuckTime, S->EndAge, PinguTuning::TornadoTime + PinguTuning::TornadoMergeExtraTime,
					S->bThereAt13 ? 1 : 0, S->GoneAt, S->HoverFrames - S->NotHovering, S->HoverFrames, OwnHits, TheirHits, S->Gap, S->GapWant, S->FromEye, F.StartDist, F.DistBeforeHit, Pulled,
					F.HitAt, F.HitSpeed, S->FarKnockWant, F.RecordKnock, S->Strength, F.RecordShare, F.RecordDist, Rule, S->bFarByThem ? 1 : 0, S->RippedBeforeMerge, S->IceRipped,
					S->MostSwirling, S->IceFarthest, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}
}

void APenguinAutoTest::AddTornadoLookStep()
{
	using namespace PenguinAutoTestTornadoPrivate;
	// The funnel up close from the camera's side: one picture on High while it sucks, then the Low preset (its shaders
	// compiled first) and one more; the preset as it was afterwards (a test run saves nothing). Checked: it shows on both,
	// with fewer pieces on Low.
	struct FLook
	{
		bool bPresetSet = false;
		FPinguGraphicsOptions Before;
		int32 Stage = 0;
		float At = 0.f;
		int32 Shots = 0;
		int32 Counts[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
		float Amount[2] = { 0.f, 0.f };
	};
	const TSharedRef<FLook> Look = MakeShared<FLook>();
	static constexpr float ShotAfter = 0.3f;
	static constexpr float LowWaitMax = 30.f;
	AddStep(TEXT("look_Tornado"), 4.f + LowWaitMax,
		[this, Look]
		{
			*Look = FLook();
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				Look->Before = Settings->GetGraphics();
				Look->bPresetSet = true;
				Settings->ApplyPreset(EPinguGraphicsPreset::High);
			}
			PrepareBrawl(2400.f, { EPenguinAbility::Gust });
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, Look](float T)
		{
			FLook& L = *Look;
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Penguin->GetFeetLocation() + FVector(0.f, 0.f, 130.f); // the whole funnel in the picture
				const FVector From = LookAt + FVector(0.f, 950.f, 90.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			const auto Record = [this, &L](int32 Index)
			{
				if (const APinguTornado* Tornado = FindTornado(GetWorld(), Penguin.Get()))
				{
					Tornado->GetLookCounts(L.Counts[Index][0], L.Counts[Index][1], L.Counts[Index][2]);
					L.Amount[Index] = Tornado->GetLookAmount();
				}
			};
			if (L.Stage == 0 && T > 0.3f)
			{
				L.Stage = 1;
				L.At = T;
				Penguin->ActivateAbility(0);
			}
			if (L.Stage == 1 && T > L.At + ShotAfter)
			{
				L.Stage = 2;
				Record(0);
				Shot(TEXT("65_tornado_look_high"));
				++L.Shots;
			}
			if (L.Stage == 2 && T > L.At + 1.5f)
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
				if (T > L.At + 0.6f && (!bCompiling || T > L.At + LowWaitMax))
				{
					L.Stage = 4;
					L.At = T;
					Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Gust }); // ready again
					Penguin->ActivateAbility(0);
				}
			}
			if (L.Stage == 4 && T > L.At + ShotAfter)
			{
				L.Stage = 5;
				Record(1);
				Shot(TEXT("65_tornado_look_low"));
				++L.Shots;
			}
			if (L.Stage == 5 && T > L.At + 1.3f)
			{
				StepTime = 1000.f; // done (the pictures are written, the tornado is gone)
			}
		},
		[this, Look]
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
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const int32 High = L.Counts[0][0] + L.Counts[0][1] + L.Counts[0][2];
			const int32 Low = L.Counts[1][0] + L.Counts[1][1] + L.Counts[1][2];
			const bool bOk = L.Shots == 2 && L.Counts[1][0] > 0 && L.Counts[1][1] > 0 && Low < High && L.Amount[0] > 0.8f && L.Amount[1] > 0.8f;
			Report(FString::Printf(TEXT("RESULT tornado look: High %d band pieces / %d dust puffs / %d bits, Low %d / %d / %d (fewer, none missing), shown %.2f / %.2f at the pictures (want over 0.8): %s"),
				L.Counts[0][0], L.Counts[0][1], L.Counts[0][2], L.Counts[1][0], L.Counts[1][1], L.Counts[1][2], L.Amount[0], L.Amount[1], bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	// ---- look_TornadoMerge: two tornadoes 7 m apart, up close from the camera's side, as they suck each other in and become one
	// giant: the two funnels drifting together (65_tornado_merge_look_1_drift), the merge's flash (_2_flash), the giant (_3_giant),
	// its throw (_4_throw). Checked on this machine's look: one merge flash, the giant's funnel about twice as wide and half as
	// tall again as a plain one's, twice the bits swirling in it, fully shown.
	struct FMergeLook
	{
		float CastT = -1.f;
		int32 Shots = 0;
		bool bSingleSeen = false;
		float SingleTop = 0.f;
		float SingleHeight = 0.f;
		int32 SingleDebris = 0;
		bool bGiantSeen = false;
		float GiantTop = 0.f;
		float GiantHeight = 0.f;
		int32 GiantDebris = 0;
		int32 Flashes = 0;
		float Amount = 0.f;
		int32 Tornadoes = 0;
	};
	const TSharedRef<FMergeLook> Merge = MakeShared<FMergeLook>();
	static constexpr float PartnerApart = 700.f;
	AddStep(TEXT("look_TornadoMerge"), 2.2f,
		[this, Merge]
		{
			*Merge = FMergeLook();
			PrepareBrawl(2400.f, { EPenguinAbility::Gust });
			const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
			const FVector PartnerAt = Base + FVector(PartnerApart, 0.f, 0.f);
			if (!TornadoDummy.IsValid() && Penguin.IsValid())
			{
				FActorSpawnParameters Params;
				Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				TornadoDummy = GetWorld()->SpawnActor<APenguinCharacter>(Penguin->GetClass(), PartnerAt, FRotator::ZeroRotator, Params);
				if (APenguinCharacter* Spawned = TornadoDummy.Get())
				{
					Spawned->SetPlayerSlot(3, TEXT("Dummy 3"));
				}
			}
			if (APenguinCharacter* Partner = TornadoDummy.Get())
			{
				Partner->ResetForRound(PartnerAt);
				Partner->SetInputLocked(false);
				Partner->FaceDirection(-1.f);
				Partner->GetAbilities()->SetAbilities({ EPenguinAbility::Gust });
			}
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
				const FVector LookAt = Base + FVector(PartnerApart * 0.5f, 0.f, 200.f);
				const FVector From = LookAt + FVector(0.f, 1650.f, 120.f);
				Stage->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
		},
		[this, Merge](float T)
		{
			FMergeLook& M = *Merge;
			APenguinCharacter* Partner = TornadoDummy.Get();
			if (!Partner)
			{
				return;
			}
			if (M.CastT < 0.f)
			{
				if (T > 0.3f)
				{
					M.CastT = T;
					Partner->ActivateAbility(0);
					Penguin->ActivateAbility(0);
				}
				return;
			}
			const float Since = T - M.CastT;
			TArray<APinguTornado*> All;
			APinguTornado::GetAll(GetWorld(), All);
			int32 Bands = 0;
			int32 Dust = 0;
			int32 Debris = 0;
			if (!M.bSingleSeen && Since > 0.13f)
			{
				if (const APinguTornado* Ours = FindTornado(GetWorld(), Penguin.Get()); Ours && Ours->GetMerges() == 0)
				{
					M.bSingleSeen = true;
					M.SingleTop = Ours->GetFunnelTopRadius();
					M.SingleHeight = Ours->GetFunnelHeight();
					Ours->GetLookCounts(Bands, Dust, Debris);
					M.SingleDebris = Debris;
				}
			}
			if (!M.bGiantSeen && Since > PinguTuning::TornadoMergeDriftTime + 0.35f)
			{
				for (const APinguTornado* Each : All)
				{
					if (Each->GetMerges() > 0)
					{
						M.bGiantSeen = true;
						M.GiantTop = Each->GetFunnelTopRadius();
						M.GiantHeight = Each->GetFunnelHeight();
						Each->GetLookCounts(Bands, Dust, Debris);
						M.GiantDebris = Debris;
						M.Flashes = Each->GetMergeFlashesShown();
						M.Amount = Each->GetLookAmount();
						M.Tornadoes = All.Num();
					}
				}
			}
			const float ShotAt[] = { 0.15f, PinguTuning::TornadoMergeDriftTime + 0.04f, PinguTuning::TornadoMergeDriftTime + 0.3f,
				PinguTuning::TornadoMergeDriftTime + PinguTuning::TornadoMergeSuckTime + 0.08f };
			const TCHAR* ShotNames[] = { TEXT("65_tornado_merge_look_1_drift"), TEXT("65_tornado_merge_look_2_flash"), TEXT("65_tornado_merge_look_3_giant"),
				TEXT("65_tornado_merge_look_4_throw") };
			if (M.Shots < 4 && Since > ShotAt[M.Shots])
			{
				Shot(ShotNames[M.Shots]);
				++M.Shots;
			}
		},
		[this, Merge]
		{
			const FMergeLook& M = *Merge;
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			if (APenguinCharacter* Partner = TornadoDummy.Get())
			{
				Partner->Destroy();
			}
			TornadoDummy.Reset();
			const float Wider = M.SingleTop > 1.f ? M.GiantTop / M.SingleTop : 0.f;
			const float Taller = M.SingleHeight > 1.f ? M.GiantHeight / M.SingleHeight : 0.f;
			const bool bOk = M.bSingleSeen && M.bGiantSeen && M.Tornadoes == 1 && M.Flashes == 1 && Wider > 1.8f && Taller > 1.4f && M.GiantDebris == 2 * M.SingleDebris
				&& M.Amount > 0.8f && M.Shots == 4;
			Report(FString::Printf(TEXT("RESULT tornado merge look: a plain funnel %.0f cm wide at its top / %.0f tall / %d bits, the giant %.0f / %.0f / %d (x%.2f wide, want over 1.8; x%.2f tall, "
				"want over 1.4; twice the bits), %d tornado(es) then (want 1), merge flashes %d (want 1), shown %.2f (want over 0.8): %s"),
				M.SingleTop, M.SingleHeight, M.SingleDebris, M.GiantTop, M.GiantHeight, M.GiantDebris, Wider, Taller, M.Tornadoes, M.Flashes, M.Amount, bOk ? TEXT("ok") : TEXT("FAIL")));
		});
	// ---- look_TornadoMergeStrip: frame strips of the merge (8 pictures from the moment two link, 65_tornado_strip_<case>_1..8)
	// for four cases one after the other: main (two plain ones 7 m apart), giant (ours size 2 and a plain one 11 m apart), third
	// (two merge, then the dummy's tornado 7 m further on joins the giant) and edge (at the island's edge, the partner over
	// the sea, the dummy sucked in and its snowball in the wind). Checked: each case ends in one tornado with every user in it,
	// and the merge's look showed: the funnels leaning toward each other (at least 80 cm x size), the giant squashed (under
	// 0.85 of its height) then stretched (over 1.05), the swirl and the shock rings shown, one merge sound per merge.
	struct FStripCase
	{
		const TCHAR* Name;
		float OurSize;
		float Apart;
		bool bThird;
		bool bEdge;
		float CameraBack;
	};
	struct FStrip
	{
		int32 Case = -1;
		float CaseStart = 0.f;
		float CastT = -1.f;
		float ThirdT = -1.f;
		float FramesFrom = -1.f;
		int32 Frames = 0;
		int32 ShotsTaken = 0;
		int32 Done = 0;
		int32 Good = 0;
		FString Lines;
		int32 MostMerges = 0;
		int32 Users = 0;
		int32 Tornadoes = 0;
		int32 Flashes = 0;
		float MostLean = 0.f;
		float LowestPop = 1.f;
		float HighestPop = 1.f;
		int32 SwirlFrames = 0;
		int32 RingFrames = 0;
		int32 Sounds = 0;
	};
	static const FStripCase StripCases[] = {
		{ TEXT("main"), 1.f, 700.f, false, false, 1650.f },
		{ TEXT("giant"), 2.f, 1100.f, false, false, 2700.f },
		{ TEXT("third"), 1.f, 700.f, true, false, 2300.f },
		{ TEXT("edge"), 1.f, 700.f, false, true, 1900.f },
	};
	static constexpr int32 NumStripCases = UE_ARRAY_COUNT(StripCases);
	static constexpr float StripFrameAt[] = { 0.02f, 0.10f, 0.18f, 0.26f, 0.32f, 0.40f, 0.52f, 0.70f };
	static constexpr int32 StripFrames = UE_ARRAY_COUNT(StripFrameAt);
	static constexpr float StripCaseTime = 3.6f;
	const TSharedRef<FStrip> Strip = MakeShared<FStrip>();
	AddStep(TEXT("look_TornadoMergeStrip"), StripCaseTime * NumStripCases + 1.f,
		[Strip]
		{
			*Strip = FStrip();
		},
		[this, Strip](float T)
		{
			FStrip& St = *Strip;
			const auto Setup = [this, &St](int32 Index)
			{
				const FStripCase& C = StripCases[Index];
				St.Case = Index;
				St.CaseStart = St.CastT = St.ThirdT = St.FramesFrom = -1.f;
				St.Frames = 0;
				St.MostMerges = St.Users = St.Tornadoes = St.Flashes = 0;
				St.MostLean = 0.f;
				St.LowestPop = St.HighestPop = 1.f;
				St.SwirlFrames = St.RingFrames = St.Sounds = 0;
				APinguTornado::DestroyAll(GetWorld());
				const float Edge = FlatIslandX + 2700.f;
				const FVector Ours = C.bEdge ? FVector(Edge, 0.f, 62.f) : FVector(FlatIslandX - 300.f, 0.f, 62.f);
				PrepareBrawl(2400.f, { EPenguinAbility::Gust });
				Teleport(Ours);
				ResetTarget(C.bEdge ? Ours + FVector(-450.f, 0.f, 0.f) : Ours + FVector(C.Apart * 2.f, 0.f, 0.f));
				if (Target.IsValid())
				{
					Target->GetAbilities()->SetAbilities({ C.bEdge ? EPenguinAbility::Snowball : EPenguinAbility::Gust });
					Target->FaceDirection(C.bEdge ? 1.f : -1.f);
				}
				const FVector PartnerAt = Ours + FVector(C.Apart, 0.f, C.bEdge ? 250.f : 0.f);
				if (!TornadoDummy.IsValid() && Penguin.IsValid())
				{
					FActorSpawnParameters Params;
					Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					TornadoDummy = GetWorld()->SpawnActor<APenguinCharacter>(Penguin->GetClass(), PartnerAt, FRotator::ZeroRotator, Params);
					if (APenguinCharacter* Spawned = TornadoDummy.Get())
					{
						Spawned->SetPlayerSlot(3, TEXT("Dummy 3"));
					}
				}
				if (APenguinCharacter* Partner = TornadoDummy.Get())
				{
					Partner->ResetForRound(PartnerAt);
					Partner->SetInputLocked(false);
					Partner->FaceDirection(-1.f);
					Partner->GetAbilities()->SetAbilities({ EPenguinAbility::Gust });
				}
				if (C.OurSize > 1.01f)
				{
					Penguin->ChangeSize(C.OurSize, 30.f);
				}
				if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
				{
					Stage->SetActorTickEnabled(false);
					LookCamera = Stage;
					const float Span = C.bThird ? C.Apart * 2.f : C.Apart;
					const FVector LookAt = Ours + FVector(Span * 0.5f, 0.f, 200.f * C.OurSize);
					const FVector From = LookAt + FVector(0.f, C.CameraBack, 120.f);
					Stage->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
			};
			if (St.Case < 0)
			{
				Setup(0);
			}
			if (St.CaseStart < 0.f)
			{
				St.CaseStart = T;
			}
			const FStripCase& C = StripCases[St.Case];
			APenguinCharacter* Partner = TornadoDummy.Get();
			const float InCase = T - St.CaseStart;
			// The partner over the sea casts in the air (it would fall first): it is held up there until then.
			if (Partner && C.bEdge && St.CastT < 0.f)
			{
				Partner->SetActorLocation(FVector(FlatIslandX + 2700.f + C.Apart, 0.f, 312.f), false, nullptr, ETeleportType::TeleportPhysics);
				Partner->GetPenguinMovement()->StopMovementImmediately();
			}
			if (St.CastT < 0.f)
			{
				const bool bSized = C.OurSize < 1.01f || Penguin->GetSizeScale() > C.OurSize - 0.03f || InCase > 2.5f;
				if (Partner && InCase > 0.3f && bSized)
				{
					St.CastT = T;
					if (C.bEdge && Target.IsValid())
					{
						Target->ActivateAbility(0); // its snowball flies into the wind
					}
					Partner->ActivateAbility(0);
					Penguin->ActivateAbility(0);
				}
				return;
			}
			const float Since = T - St.CastT;
			if (C.bThird && St.ThirdT < 0.f && Since > 0.55f && Target.IsValid())
			{
				St.ThirdT = T;
				Target->ActivateAbility(0);
			}
			TArray<APinguTornado*> All;
			APinguTornado::GetAll(GetWorld(), All);
			for (const APinguTornado* Each : All)
			{
				St.MostMerges = FMath::Max(St.MostMerges, Each->GetMerges());
				St.MostLean = FMath::Max(St.MostLean, Each->GetMergeLean());
				St.LowestPop = FMath::Min(St.LowestPop, Each->GetMergePop());
				St.HighestPop = FMath::Max(St.HighestPop, Each->GetMergePop());
				St.SwirlFrames += Each->GetMergeSwirlShown() > 0 ? 1 : 0;
				St.RingFrames += Each->GetMergeRingsShown() > 0 ? 1 : 0;
				St.Sounds = FMath::Max(St.Sounds, Each->GetMergeSoundsShown());
				if (Each->IsMerging() && St.FramesFrom < 0.f && (!C.bThird || St.ThirdT >= 0.f))
				{
					St.FramesFrom = T;
				}
			}
			if (St.FramesFrom >= 0.f && St.Frames < StripFrames && T - St.FramesFrom >= StripFrameAt[St.Frames])
			{
				++St.Frames;
				++St.ShotsTaken;
				Shot(*FString::Printf(TEXT("65_tornado_strip_%s_%d"), C.Name, St.Frames));
			}
			if (Since > 1.0f && Since < 1.1f && All.Num() > 0)
			{
				St.Tornadoes = All.Num();
				St.Users = All[0]->GetUsers().Num();
				St.Flashes = All[0]->GetMergeFlashesShown();
			}
			if (InCase > StripCaseTime)
			{
				const int32 WantUsers = C.bThird ? 3 : 2;
				const int32 WantMerges = C.bThird ? 2 : 1;
				const bool bLook = St.MostLean > 80.f * C.OurSize && St.LowestPop < 0.85f && St.HighestPop > 1.05f && St.SwirlFrames > 10 && St.RingFrames > 10
					&& St.Sounds == WantMerges;
				const bool bOk = St.Frames == StripFrames && St.Tornadoes == 1 && St.Users == WantUsers && St.MostMerges == WantMerges && St.Flashes == WantMerges && bLook;
				St.Good += bOk ? 1 : 0;
				++St.Done;
				St.Lines += FString::Printf(TEXT("%s%s: %d frames, %d tornado(es) at 1 s, %d users (want %d), %d merges (want %d), %d flashes, lean %.0f cm, pop %.2f .. %.2f, swirl on %d frames, "
					"rings on %d, %d merge sound(s)"), St.Lines.IsEmpty() ? TEXT("") : TEXT("; "),
					C.Name, St.Frames, St.Tornadoes, St.Users, WantUsers, St.MostMerges, WantMerges, St.Flashes, St.MostLean, St.LowestPop, St.HighestPop, St.SwirlFrames, St.RingFrames, St.Sounds);
				if (St.Case + 1 < NumStripCases)
				{
					Setup(St.Case + 1);
				}
				else
				{
					StepTime = 1000.f; // done
				}
			}
		},
		[this, Strip]
		{
			const FStrip& St = *Strip;
			APinguTornado::DestroyAll(GetWorld());
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			if (APenguinCharacter* Partner = TornadoDummy.Get())
			{
				Partner->Destroy();
			}
			TornadoDummy.Reset();
			Penguin->ResetForRound(Penguin->GetActorLocation());
			const bool bOk = St.Done == NumStripCases && St.Good == NumStripCases && St.ShotsTaken == NumStripCases * StripFrames;
			Report(FString::Printf(TEXT("RESULT tornado merge strips: %d of %d cases right, %d pictures (want %d) - %s: %s"), St.Good, NumStripCases, St.ShotsTaken,
				NumStripCases * StripFrames, *St.Lines, bOk ? TEXT("ok") : TEXT("FAIL")));
		});
	// ---- look_TornadoQuality: one tornado on Low and on Ultra - up close (65_tornado_q_<low|ultra>_close), in the match's own
	// camera (_match) and a giant's (size 2.5, _giant) - and what it costs: the GPU's frame time with none, one and three
	// tornadoes in view (15 m apart: no merges) on each preset. Checked: every picture taken, the funnel shown, the Ultra look
	// on Ultra only (its 3D swirl strands; Low draws none); the GPU numbers are measurements (want: Low not dearer than before).
	struct FQuality
	{
		bool bPresetSet = false;
		FPinguGraphicsOptions Before;
		int32 Preset = 0;   // 0 Low, 1 Ultra
		int32 Stage = 0;
		float At = 0.f;
		int32 Shots = 0;
		float Gpu[2][3] = { { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } };
		int32 GpuFrames[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
		int32 Strands[2] = { 0, 0 };
		int32 Bands[2] = { 0, 0 };
		float Amount[2] = { 0.f, 0.f };
		int32 Seen3 = 0;
	};
	const TSharedRef<FQuality> Quality = MakeShared<FQuality>();
	static constexpr float QualityWaitMax = 30.f;
	AddStep(TEXT("look_TornadoQuality"), 2.f * (QualityWaitMax + 12.f),
		[this, Quality]
		{
			*Quality = FQuality();
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				Quality->Before = Settings->GetGraphics();
				Quality->bPresetSet = true;
			}
		},
		[this, Quality](float T)
		{
			FQuality& Q = *Quality;
			const FVector Middle(FlatIslandX, 0.f, 62.f);
			const auto Park = [this](const FVector& LookAt, float Back, float Up)
			{
				if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
				{
					Stage->SetActorTickEnabled(false);
					LookCamera = Stage;
					const FVector From = LookAt + FVector(0.f, Back, Up);
					Stage->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
			};
			const auto UseTornado = [this](APenguinCharacter* Who)
			{
				if (Who)
				{
					Who->GetAbilities()->SetAbilities({ EPenguinAbility::Gust }); // ready again
					Who->ActivateAbility(0);
				}
			};
			const auto Name = [&Q](const TCHAR* What)
			{
				return FString::Printf(TEXT("65_tornado_q_%s_%s"), Q.Preset == 0 ? TEXT("low") : TEXT("ultra"), What);
			};
			const auto Next = [&Q, T](int32 Stage)
			{
				Q.Stage = Stage;
				Q.At = T;
			};
			const float Since = T - Q.At;
			switch (Q.Stage)
			{
			case 0: // the preset, its shaders compiled first
				if (UPinguSettings* Settings = UPinguSettings::Get())
				{
					Settings->ApplyPreset(Q.Preset == 0 ? EPinguGraphicsPreset::Low : EPinguGraphicsPreset::Ultra);
				}
				APinguTornado::DestroyAll(GetWorld());
				Next(1);
				break;
			case 1:
			{
				const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
				if (Since > 0.6f && (!bCompiling || Since > QualityWaitMax))
				{
					PrepareBrawl(2400.f, { EPenguinAbility::Gust });
					Teleport(Middle);
					Park(Middle + FVector(0.f, 0.f, 130.f), 950.f, 90.f);
					Next(2);
				}
				break;
			}
			case 2: // up close
				if (Since > 0.3f)
				{
					UseTornado(Penguin.Get());
					Next(3);
				}
				break;
			case 3:
				if (Since > 0.35f && Q.Shots == 3 * Q.Preset)
				{
					if (const APinguTornado* Tornado = FindTornado(GetWorld(), Penguin.Get()))
					{
						int32 Dust = 0;
						int32 Debris = 0;
						Tornado->GetLookCounts(Q.Bands[Q.Preset], Dust, Debris);
						Q.Strands[Q.Preset] = Tornado->GetStrandCount();
						Q.Amount[Q.Preset] = Tornado->GetLookAmount();
					}
					Shot(*Name(TEXT("close")));
					++Q.Shots;
				}
				if (Since > 1.7f)
				{
					// The match's own camera on us.
					if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
					{
						Stage->SetActorTickEnabled(true);
						Stage->SnapToTargets();
					}
					Next(4);
				}
				break;
			case 4:
				if (Since > 0.4f && Q.Shots == 3 * Q.Preset + 1)
				{
					UseTornado(Penguin.Get());
					Next(5);
				}
				break;
			case 5:
				if (Since > 0.35f && Q.Shots == 3 * Q.Preset + 1)
				{
					Shot(*Name(TEXT("match")));
					++Q.Shots;
				}
				if (Since > 1.7f)
				{
					Penguin->ChangeSize(2.5f, 30.f);
					Park(Middle + FVector(0.f, 0.f, 330.f), 2100.f, 150.f);
					Next(6);
				}
				break;
			case 6: // a giant's tornado
				if (Penguin->GetSizeScale() > 2.45f || Since > 3.f)
				{
					UseTornado(Penguin.Get());
					Next(7);
				}
				break;
			case 7:
				if (Since > 0.35f && Q.Shots == 3 * Q.Preset + 2)
				{
					Shot(*Name(TEXT("giant")));
					++Q.Shots;
				}
				if (Since > 1.7f)
				{
					// The cost: none, one, then three tornadoes in view.
					Penguin->ResetForRound(Middle);
					Teleport(Middle);
					ResetTarget(Middle + FVector(-1500.f, 0.f, 0.f));
					if (!TornadoDummy.IsValid() && Penguin.IsValid())
					{
						FActorSpawnParameters Params;
						Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
						TornadoDummy = GetWorld()->SpawnActor<APenguinCharacter>(Penguin->GetClass(), Middle + FVector(1500.f, 0.f, 0.f), FRotator::ZeroRotator, Params);
						if (APenguinCharacter* Spawned = TornadoDummy.Get())
						{
							Spawned->SetPlayerSlot(3, TEXT("Dummy 3"));
						}
					}
					if (APenguinCharacter* Third = TornadoDummy.Get())
					{
						Third->ResetForRound(Middle + FVector(1500.f, 0.f, 0.f));
						Third->SetInputLocked(false);
					}
					Park(Middle + FVector(0.f, 0.f, 250.f), 3600.f, 200.f);
					Next(8);
				}
				break;
			case 8:
			case 9:
			case 10:
			{
				// 8: none (after 0.3 s, for 0.8 s); 9: one (0.15 .. 0.95 s after its cast); 10: three.
				const int32 Count = Q.Stage - 8;
				const float From = Count == 0 ? 0.3f : 0.15f;
				const float To = Count == 0 ? 1.1f : 0.95f;
				if (Since >= From && Since < To)
				{
					Q.Gpu[Q.Preset][Count] += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
					++Q.GpuFrames[Q.Preset][Count];
					if (Count == 2)
					{
						TArray<APinguTornado*> All;
						APinguTornado::GetAll(GetWorld(), All);
						Q.Seen3 = FMath::Max(Q.Seen3, All.Num());
					}
				}
				if (Since > (Count == 0 ? 1.1f : 1.8f))
				{
					if (Count < 2)
					{
						Next(Q.Stage + 1);
						UseTornado(Penguin.Get());
						if (Count == 1)
						{
							UseTornado(Target.Get());
							UseTornado(TornadoDummy.Get());
						}
					}
					else if (Q.Preset == 0)
					{
						Q.Preset = 1;
						Next(0);
					}
					else
					{
						StepTime = 1000.f; // done
					}
				}
				break;
			}
			default:
				break;
			}
		},
		[this, Quality]
		{
			const FQuality& Q = *Quality;
			if (UPinguSettings* Settings = UPinguSettings::Get(); Settings && Q.bPresetSet)
			{
				if (Q.Before.Preset == EPinguGraphicsPreset::Custom)
				{
					Settings->SetGraphics(Q.Before);
				}
				else
				{
					Settings->ApplyPreset(Q.Before.Preset);
				}
			}
			APinguTornado::DestroyAll(GetWorld());
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			if (APenguinCharacter* Third = TornadoDummy.Get())
			{
				Third->Destroy();
			}
			TornadoDummy.Reset();
			Penguin->ResetForRound(Penguin->GetActorLocation());
			float Ms[2][3];
			for (int32 P = 0; P < 2; ++P)
			{
				for (int32 C = 0; C < 3; ++C)
				{
					Ms[P][C] = Q.GpuFrames[P][C] > 0 ? Q.Gpu[P][C] / Q.GpuFrames[P][C] : -1.f;
				}
			}
			const bool bOk = Q.Shots == 6 && Q.Amount[0] > 0.8f && Q.Amount[1] > 0.8f && Q.Strands[0] == 0 && Q.Strands[1] > 0 && Q.Seen3 == 3
				&& Q.GpuFrames[0][2] > 10 && Q.GpuFrames[1][2] > 10;
			Report(FString::Printf(TEXT("RESULT tornado quality: Low %d band pieces / %d strands, Ultra %d / %d (want strands on Ultra only), shown %.2f / %.2f, %d pictures (want 6), "
				"3 tornadoes seen %d; GPU ms with 0 / 1 / 3 tornadoes in view: Low %.2f / %.2f / %.2f (+%.2f / +%.2f), Ultra %.2f / %.2f / %.2f (+%.2f / +%.2f): %s"),
				Q.Bands[0], Q.Strands[0], Q.Bands[1], Q.Strands[1], Q.Amount[0], Q.Amount[1], Q.Shots, Q.Seen3,
				Ms[0][0], Ms[0][1], Ms[0][2], Ms[0][1] - Ms[0][0], Ms[0][2] - Ms[0][0], Ms[1][0], Ms[1][1], Ms[1][2], Ms[1][1] - Ms[1][0], Ms[1][2] - Ms[1][0],
				bOk ? TEXT("ok") : TEXT("FAIL")));
		});
	// ---- look_TornadoVariety: six tornadoes side by side at once (4 m apart, a small reach each so none merge), on Ultra
	// (65_tornado_var_ultra_1 / _2 a moment later) and on Low (65_tornado_var_low); then one up close over its life
	// (65_tornado_var_life_1..8). Checked: six at once, six different look seeds, their tilts differ (the tops stand off their
	// eyes by different amounts), the one up close wanders while it lives.
	struct FVariety
	{
		bool bPresetSet = false;
		FPinguGraphicsOptions Before;
		int32 Stage = 0;
		float At = 0.f;
		int32 Shots = 0;
		int32 MostAtOnce = 0;
		int32 Seeds = 0;
		float ShiftMin = 0.f;
		float ShiftMax = 0.f;
		float LifeMin = 1.e6f;
		float LifeMax = -1.e6f;
		int32 LifeFrames = 0;
		TArray<TWeakObjectPtr<APenguinCharacter>> Extras;
	};
	const TSharedRef<FVariety> Variety = MakeShared<FVariety>();
	static constexpr float VarietyApart = 400.f;
	static constexpr int32 VarietyCount = 6;
	static constexpr float VarietyWaitMax = 30.f;
	static constexpr float VarietyLifeAt[] = { 0.1f, 0.22f, 0.34f, 0.46f, 0.58f, 0.7f, 0.82f, 0.94f };
	AddStep(TEXT("look_TornadoVariety"), 2.f * VarietyWaitMax + 12.f,
		[this, Variety]
		{
			*Variety = FVariety();
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				Variety->Before = Settings->GetGraphics();
				Variety->bPresetSet = true;
			}
		},
		[this, Variety](float T)
		{
			FVariety& V = *Variety;
			const FVector First(FlatIslandX - 1000.f, 0.f, 62.f);
			const auto Row = [this, &V, &First]() -> TArray<APenguinCharacter*>
			{
				// Us, the dummy and four more penguins in a row.
				TArray<APenguinCharacter*> Who = { Penguin.Get(), Target.Get() };
				while (V.Extras.Num() < VarietyCount - 2 && Penguin.IsValid())
				{
					FActorSpawnParameters Params;
					Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					APenguinCharacter* Extra = GetWorld()->SpawnActor<APenguinCharacter>(Penguin->GetClass(), First, FRotator::ZeroRotator, Params);
					if (Extra)
					{
						Extra->SetPlayerSlot(4 + V.Extras.Num(), FString::Printf(TEXT("Dummy %d"), 4 + V.Extras.Num()));
					}
					V.Extras.Add(Extra);
				}
				for (const TWeakObjectPtr<APenguinCharacter>& Extra : V.Extras)
				{
					Who.Add(Extra.Get());
				}
				for (int32 Index = 0; Index < Who.Num(); ++Index)
				{
					if (APenguinCharacter* Each = Who[Index])
					{
						const FVector At = First + FVector(VarietyApart * Index, 0.f, 0.f);
						if (Each == Penguin.Get())
						{
							Teleport(At);
							Penguin->ResetForRound(At);
						}
						else
						{
							Each->ResetForRound(At);
							Each->SetInputLocked(false);
						}
					}
				}
				return Who;
			};
			const auto Park = [this](const FVector& LookAt, float Back, float Up)
			{
				if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
				{
					Stage->SetActorTickEnabled(false);
					LookCamera = Stage;
					const FVector From = LookAt + FVector(0.f, Back, Up);
					Stage->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
			};
			const auto Measure = [this, &V]()
			{
				TArray<APinguTornado*> All;
				APinguTornado::GetAll(GetWorld(), All);
				V.MostAtOnce = FMath::Max(V.MostAtOnce, All.Num());
				TSet<int32> Unique;
				float Low = 1.e6f;
				float High = -1.e6f;
				for (const APinguTornado* Each : All)
				{
					Unique.Add(Each->GetLookSeed());
					Low = FMath::Min(Low, Each->GetTopShift());
					High = FMath::Max(High, Each->GetTopShift());
				}
				if (All.Num() == VarietyCount)
				{
					V.Seeds = FMath::Max(V.Seeds, Unique.Num());
					V.ShiftMin = Low;
					V.ShiftMax = High;
				}
			};
			const auto Next = [&V, T](int32 Stage)
			{
				V.Stage = Stage;
				V.At = T;
			};
			const float Since = T - V.At;
			const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
			switch (V.Stage)
			{
			case 0:
				PrepareBrawl(2400.f, { EPenguinAbility::Gust });
				if (UPinguSettings* Settings = UPinguSettings::Get())
				{
					Settings->ApplyPreset(EPinguGraphicsPreset::Ultra);
				}
				Next(1);
				break;
			case 1:
			case 4:
				if (Since > 0.6f && (!bCompiling || Since > VarietyWaitMax))
				{
					APinguTornado::DestroyAll(GetWorld());
					const TArray<APenguinCharacter*> Who = Row();
					Park(First + FVector(VarietyApart * (VarietyCount - 1) * 0.5f, 0.f, 200.f), 3500.f, 150.f);
					for (APenguinCharacter* Each : Who)
					{
						APinguTornado::Spawn(Each, 1.f, 0.2f); // (a small reach: side by side, none merge)
					}
					Next(V.Stage + 1);
				}
				break;
			case 2:
			case 5:
				if (Since > 0.05f)
				{
					Measure();
				}
				if (Since > 0.35f && V.Shots == (V.Stage == 2 ? 0 : 2))
				{
					Shot(V.Stage == 2 ? TEXT("65_tornado_var_ultra_1") : TEXT("65_tornado_var_low"));
					++V.Shots;
				}
				if (V.Stage == 2 && Since > 0.75f && V.Shots == 1)
				{
					Shot(TEXT("65_tornado_var_ultra_2"));
					++V.Shots;
				}
				if (Since > 1.6f)
				{
					if (V.Stage == 2)
					{
						if (UPinguSettings* Settings = UPinguSettings::Get())
						{
							Settings->ApplyPreset(EPinguGraphicsPreset::Low);
						}
						Next(4);
					}
					else
					{
						// Back to Ultra for one up close over its life.
						if (UPinguSettings* Settings = UPinguSettings::Get())
						{
							Settings->ApplyPreset(EPinguGraphicsPreset::Ultra);
						}
						Next(6);
					}
				}
				break;
			case 6:
				if (Since > 0.6f && (!bCompiling || Since > VarietyWaitMax))
				{
					APinguTornado::DestroyAll(GetWorld());
					for (const TWeakObjectPtr<APenguinCharacter>& Extra : V.Extras)
					{
						if (APenguinCharacter* Each = Extra.Get())
						{
							Each->Destroy(); // (one would stand next to us)
						}
					}
					V.Extras.Reset();
					PrepareBrawl(2400.f, { EPenguinAbility::Gust });
					Park(Penguin->GetFeetLocation() + FVector(0.f, 0.f, 230.f), 1800.f, 120.f);
					APinguTornado::Spawn(Penguin.Get(), 1.f, 1.f);
					Next(7);
				}
				break;
			case 7:
			{
				const APinguTornado* Ours = FindTornado(GetWorld(), Penguin.Get());
				if (Ours && Since > 0.15f && Since < 0.95f)
				{
					V.LifeMin = FMath::Min(V.LifeMin, Ours->GetTopShift());
					V.LifeMax = FMath::Max(V.LifeMax, Ours->GetTopShift());
					++V.LifeFrames;
				}
				const int32 Frame = V.Shots - 3;
				if (Frame >= 0 && Frame < UE_ARRAY_COUNT(VarietyLifeAt) && Since > VarietyLifeAt[Frame])
				{
					Shot(*FString::Printf(TEXT("65_tornado_var_life_%d"), Frame + 1));
					++V.Shots;
				}
				if (Since > 1.4f)
				{
					StepTime = 1000.f; // done
				}
				break;
			}
			default:
				break;
			}
		},
		[this, Variety]
		{
			const FVariety& V = *Variety;
			if (UPinguSettings* Settings = UPinguSettings::Get(); Settings && V.bPresetSet)
			{
				if (V.Before.Preset == EPinguGraphicsPreset::Custom)
				{
					Settings->SetGraphics(V.Before);
				}
				else
				{
					Settings->ApplyPreset(V.Before.Preset);
				}
			}
			APinguTornado::DestroyAll(GetWorld());
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			for (const TWeakObjectPtr<APenguinCharacter>& Extra : V.Extras)
			{
				if (APenguinCharacter* Each = Extra.Get())
				{
					Each->Destroy();
				}
			}
			Penguin->ResetForRound(Penguin->GetActorLocation());
			const float Spread = V.ShiftMax - V.ShiftMin;
			const float Wander = V.LifeMax - V.LifeMin;
			const bool bOk = V.MostAtOnce == VarietyCount && V.Seeds == VarietyCount && Spread > 60.f && Wander > 15.f
				&& V.Shots == 3 + UE_ARRAY_COUNT(VarietyLifeAt);
			Report(FString::Printf(TEXT("RESULT tornado variety: %d at once (want %d), %d different looks (want %d), their tops %.0f .. %.0f cm off their eyes (want over 60 apart), "
				"one wandered %.0f cm in its life (want over 15, %d frames), %d pictures (want %d): %s"),
				V.MostAtOnce, VarietyCount, V.Seeds, VarietyCount, V.ShiftMin, V.ShiftMax, Wander, V.LifeFrames, V.Shots, 3 + static_cast<int32>(UE_ARRAY_COUNT(VarietyLifeAt)),
				bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}
