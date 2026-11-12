// Pingu - the snow bomb knocks out and pushes its own thrower too (APenguinProjectile::Explode; a self-knock-out is credited
// to nobody: APenguinCharacter::KnockOut). The autotest's steps (APenguinAutoTest::AddSelfBombSteps, also for looktest):
//
//  selfbomb_<case>   a bomb of ours at rest beside our penguin (the dummy on the other side of it): inside the kill radius we are
//                    out (reason Bomb, our own doing, credited to nobody - not even to the dummy that hit us a moment before),
//                    in the push radius we are pushed exactly like the dummy at the same distance, outside nothing happens, a
//                    grown bomb's kill radius grows with it, our ice shell holds the blast, a bomb that was batted back (what the
//                    Mirror trait and the shell do) knocks us out as the dummy's.
//  selfbomb_FeetDrop the ability aimed at our own feet: dropped on a short fuse, it knocks us out (no more bomb jump).
//  look_SelfBomb     that knock-out up close: the blast, the charred stand, the ash pile (screenshots 68_self_bomb_*).
//  bot_BombThrow     the bots' throw check: not where the bomb would land in their own blast (near, at their feet, into a wall).
//  bot_BombFlee*     a live bomb next to a bot (its own, another's): it runs out of the blast; at the end of the island it runs
//                    through it, or puts its shell up.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinBot.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"

namespace PenguinAutoTestSelfBombPrivate
{
	/** What a penguin standing near the bomb should get. */
	enum class EWant : uint8
	{
		SelfOut,   // knocked out by its own bomb: reason Bomb, self-inflicted, credited to nobody
		OtherOut,  // knocked out by the other penguin's bomb: reason Bomb, credited to the other
		Pushed,    // alive, hit once, flung away at the speed the push rule gives
		Untouched, // alive, no hit, standing
	};

	struct FCase
	{
		const TCHAR* Name;
		float PenguinDist;  // cm from our capsule to the bomb (its middle, at rest)
		float DummyDist;    // the dummy's, on the other side of it (< 0: far away)
		float Grow;         // the bomb is grown by this much (1: as it is)
		bool bShell;        // our ice shell is up
		bool bHitFirst;     // the dummy hits us a moment before (we are its last attacker)
		bool bReflect;      // the dummy bats the bomb back just before it goes off (the bomb is its own then)
		EWant WantPenguin;
		EWant WantDummy;
	};

	struct FRun
	{
		FCase C = {};
		bool bShellUp = false;
		bool bHitDone = false;
		bool bArmed = false;
		float BlastAt = -1.f;
		float PenguinDist = -1.f;
		float DummyDist = -1.f;
		float PenguinPeak = 0.f;  // the fastest it flew away from the bomb after the blast (cm/s)
		float DummyPeak = 0.f;
		float PenguinLift = 0.f;
		float DummyLift = 0.f;
		int32 PenguinHits0 = 0;
		int32 DummyHits0 = 0;
	};

	static FString CreditOf(const APenguinCharacter* Who)
	{
		const APenguinCharacter* By = Who ? Who->GetKOBy() : nullptr;
		return By ? By->GetDisplayName() : FString(TEXT("nobody"));
	}

	/** The push the rule gives at this distance from the bomb's middle edge (cm, along X, at normal size and fight time). */
	static float ExpectedPush(float Dist, float Grow)
	{
		const float Kill = PinguTuning::BombKillRadius * Grow;
		const float Push = PinguTuning::BombPushRadius * Grow;
		return PinguTuning::BombPush * (1.f - 0.5f * FMath::Clamp((Dist - Kill) / (Push - Kill), 0.f, 1.f));
	}

	/** Does Who (one of the two) end up as Want says? Detail gets what it did. */
	static bool Judge(EWant Want, const APenguinCharacter* Who, const APenguinCharacter* Other, float Dist, float Peak, float Lift, int32 Hits, float Grow, FString& Detail)
	{
		const bool bOut = Who->IsKnockedOut();
		const EPenguinKOReason Reason = Who->GetKOReason();
		const bool bSelf = Who->WasKOSelfInflicted();
		const FString Credit = CreditOf(Who);
		if (bOut)
		{
			Detail = FString::Printf(TEXT("out (%s, self-inflicted %d, credited to %s)"), *UEnum::GetValueAsString(Reason), bSelf ? 1 : 0, *Credit);
		}
		else
		{
			Detail = FString::Printf(TEXT("alive, hit %d x, flung %.0f cm/s (up %.0f)"), Hits, Peak, Lift);
		}
		switch (Want)
		{
		case EWant::SelfOut:
			return bOut && Reason == EPenguinKOReason::Bomb && bSelf && !Who->GetKOBy();
		case EWant::OtherOut:
			return bOut && Reason == EPenguinKOReason::Bomb && !bSelf && Who->GetKOBy() == Other;
		case EWant::Pushed:
		{
			const float Want0 = ExpectedPush(Dist, Grow);
			Detail += FString::Printf(TEXT(", the rule says %.0f"), Want0);
			return !bOut && Hits == 1 && Peak > 0.6f * Want0 && Peak < 1.7f * Want0 && Lift > 300.f;
		}
		default:
			return !bOut && Hits == 0 && Peak < 40.f;
		}
	}
}

void APenguinAutoTest::AddSelfBombSteps()
{
	using namespace PenguinAutoTestSelfBombPrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	// ---- the cases: where the bomb lies (distances are from the capsule's surface; the kill radius is 115 cm, the push radius 330)
	static const FCase Cases[] = {
		// inside the kill radius: we are out, so is the dummy (its kill is ours; ours is nobody's, even with the dummy as the last attacker)
		{ TEXT("Kill"), 60.f, 60.f, 1.f, false, true, false, EWant::SelfOut, EWant::OtherOut },
		// between the two radii: both pushed alike (we are no exception: no more bomb jump)
		{ TEXT("PushNear"), 150.f, 150.f, 1.f, false, false, false, EWant::Pushed, EWant::Pushed },
		{ TEXT("PushFar"), 250.f, 250.f, 1.f, false, false, false, EWant::Pushed, EWant::Pushed },
		// outside: nothing
		{ TEXT("Outside"), 400.f, 400.f, 1.f, false, false, false, EWant::Untouched, EWant::Untouched },
		// a grown bomb (the grow ray): the kill radius grows with it (115 x 1.6 = 184: 150 is inside now)
		{ TEXT("Grown"), 150.f, 150.f, 1.6f, false, false, false, EWant::SelfOut, EWant::OtherOut },
		// the ice shell holds the blast, our own too
		{ TEXT("Shell"), 60.f, -1.f, 1.f, true, false, false, EWant::Untouched, EWant::Untouched },
		// batted back (the Mirror trait, the shell, a slap ... : Reflect): the dummy's bomb now, we are just a victim - its kill
		{ TEXT("Reflected"), 60.f, -1.f, 1.f, false, false, true, EWant::OtherOut, EWant::Untouched },
	};
	for (const FCase& Case : Cases)
	{
		const TSharedRef<FRun> Run = MakeShared<FRun>();
		AddStep(*FString::Printf(TEXT("selfbomb_%s"), Case.Name), 1.5f,
			[this, Run, &Case, Base]
			{
				SpawnTarget();
				PrepareBrawl(2400.f, {});
				*Run = FRun();
				Run->C = Case;
				Penguin->SetTeam(-1);
				const float Radius = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
				const float BombX = static_cast<float>(Location().X) + Radius + Case.PenguinDist;
				const float DummyX = Case.DummyDist >= 0.f ? BombX + Radius + Case.DummyDist : static_cast<float>(Base.X) + 2400.f;
				if (Target.IsValid())
				{
					Target->SetTeam(-1);
					ResetTarget(FVector(DummyX, 0.f, Base.Z));
				}
				Penguin->GetAbilities()->SetAbilities({ Case.bShell ? EPenguinAbility::IceShell : EPenguinAbility::Snowball });
				TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin.Get(), FVector(BombX, 0.f, 34.f), FVector::ZeroVector);
				if (TestBombA.IsValid() && Case.Grow != 1.f)
				{
					TestBombA->Rescale(Case.Grow);
				}
			},
			[this, Run](float T)
			{
				FRun& R = *Run;
				APenguinCharacter* Dummy = Target.Get();
				if (!Dummy || !Dummy->GetPenguinMovement())
				{
					return;
				}
				if (R.C.bShell && !R.bShellUp && T > 0.05f)
				{
					R.bShellUp = true;
					Penguin->SetAimPoint(Penguin->GetAimOrigin());
					Penguin->ActivateAbility(0);
				}
				if (R.C.bHitFirst && !R.bHitDone && T > 0.12f)
				{
					// The dummy hits us (no push): from now on it is our last attacker - a knock-out of ours that is our own doing is
					// still not its point.
					R.bHitDone = true;
					FPenguinHit Hit;
					Hit.Instigator = Dummy;
					Hit.Source = EPenguinAbility::Snowball;
					Hit.Knockback = FVector2D::ZeroVector;
					Hit.Stun = 0.f;
					Hit.Location = Penguin->GetActorLocation();
					Penguin->ApplyPenguinHit(Hit);
				}
				APenguinProjectile* Bomb = TestBombA.Get();
				if (!R.bArmed && T > 0.3f && Bomb)
				{
					R.bArmed = true;
					R.PenguinDist = Penguin->DistanceTo(Bomb->GetActorLocation());
					R.DummyDist = Dummy->DistanceTo(Bomb->GetActorLocation());
					R.PenguinHits0 = Move->HitEvents;
					R.DummyHits0 = Dummy->GetPenguinMovement()->HitEvents;
					if (R.C.bReflect)
					{
						Bomb->Reflect(Dummy, FVector(-1.f, 0.f, 0.f), 10.f); // (it hops up and goes off in a moment, where it lay)
						Bomb->Detonate(0.04f);
					}
					else
					{
						Bomb->Detonate(0.f);
					}
				}
				if (R.bArmed && R.BlastAt < 0.f && (!Bomb || !Bomb->IsFlying()))
				{
					R.BlastAt = T;
				}
				if (R.BlastAt >= 0.f && T < R.BlastAt + 0.25f)
				{
					// The bomb is to our right, the dummy to its right: we are flung left, the dummy right.
					R.PenguinPeak = FMath::Max(R.PenguinPeak, static_cast<float>(-Move->Velocity.X));
					R.DummyPeak = FMath::Max(R.DummyPeak, static_cast<float>(Dummy->GetPenguinMovement()->Velocity.X));
					R.PenguinLift = FMath::Max(R.PenguinLift, static_cast<float>(Move->Velocity.Z));
					R.DummyLift = FMath::Max(R.DummyLift, static_cast<float>(Dummy->GetPenguinMovement()->Velocity.Z));
				}
			},
			[this, Run]
			{
				const FRun& R = *Run;
				const APenguinCharacter* Dummy = Target.Get();
				if (!Dummy || !Dummy->GetPenguinMovement() || R.BlastAt < 0.f)
				{
					Report(FString::Printf(TEXT("RESULT self bomb %s: FAIL (the bomb never went off)"), R.C.Name));
					return;
				}
				FString OurDetail;
				FString DummyDetail;
				const bool bOurs = Judge(R.C.WantPenguin, Penguin.Get(), Dummy, R.PenguinDist, R.PenguinPeak, R.PenguinLift, Move->HitEvents - R.PenguinHits0, R.C.Grow, OurDetail);
				const bool bDummys = Judge(R.C.WantDummy, Dummy, Penguin.Get(), R.DummyDist, R.DummyPeak, R.DummyLift, Dummy->GetPenguinMovement()->HitEvents - R.DummyHits0, R.C.Grow, DummyDetail);
				bool bAlike = true;
				FString Alike;
				if (R.C.WantPenguin == EWant::Pushed && R.C.WantDummy == EWant::Pushed)
				{
					// Pushed like anyone else: the dummy at the same distance flies as fast.
					const float Most = FMath::Max(R.PenguinPeak, R.DummyPeak);
					bAlike = Most > 1.f && FMath::Abs(R.PenguinPeak - R.DummyPeak) < 0.12f * Most;
					Alike = FString::Printf(TEXT(", we %.0f and the dummy %.0f cm/s %s"), R.PenguinPeak, R.DummyPeak, bAlike ? TEXT("alike") : TEXT("DIFFER"));
				}
				Report(FString::Printf(TEXT("RESULT self bomb %s: the bomb %.0f cm from us and %.0f from the dummy (kill radius %.0f x %.1f, push radius %.0f): we are %s %s; the dummy is %s %s%s: %s"),
					R.C.Name, R.PenguinDist, R.DummyDist, PinguTuning::BombKillRadius, R.C.Grow, PinguTuning::BombPushRadius * R.C.Grow,
					*OurDetail, bOurs ? TEXT("as wanted") : TEXT("NOT as wanted"), *DummyDetail, bDummys ? TEXT("as wanted") : TEXT("NOT as wanted"), *Alike,
					bOurs && bDummys && bAlike ? TEXT("ok") : TEXT("FAIL")));
				ClearAbilityLeftovers();
				Penguin->ResetForRound(Penguin->GetActorLocation());
				Target->ResetForRound(Target->GetActorLocation());
			});
	}

	// ---- the ability aimed at our own feet: the bomb is dropped there on a short fuse and goes off under us - no bomb jump, we
	// are out (its short fuse is 0.6 s from the throw).
	AddStep(TEXT("selfbomb_FeetDrop"), 1.8f,
		[this]
		{
			SpawnTarget();
			PrepareBrawl(2400.f, { EPenguinAbility::SnowBomb });
			Penguin->SetTeam(-1);
			TimeA = -1.f;
			TimeB = -1.f;
		},
		[this](float T)
		{
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(0.f, 0.f, -120.f));
			if (!bFlag && T > 0.25f)
			{
				bFlag = true;
				TimeA = T;
				Report(FString::Printf(TEXT("snow bomb at our feet used: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
			if (TimeB < 0.f && Penguin->IsKnockedOut())
			{
				TimeB = T;
			}
		},
		[this]
		{
			const float Delay = TimeA >= 0.f && TimeB >= 0.f ? TimeB - TimeA : -1.f;
			const bool bOk = Delay > 0.45f && Delay < 0.9f && Penguin->GetKOReason() == EPenguinKOReason::Bomb && Penguin->WasKOSelfInflicted() && !Penguin->GetKOBy();
			Report(FString::Printf(TEXT("RESULT self bomb at the feet: knocked out %.2f s after the throw (want 0.5 to 0.8, the fuse is 0.6), reason %s, self-inflicted %d, credited to %s: %s"),
				Delay, *UEnum::GetValueAsString(Penguin->GetKOReason()), Penguin->WasKOSelfInflicted() ? 1 : 0, *CreditOf(Penguin.Get()), bOk ? TEXT("ok") : TEXT("FAIL")));
			ClearAbilityLeftovers();
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});

	// ---- the look: the same knock-out up close from the stage camera's side - the blast, standing charred, the ash pile.
	AddStep(TEXT("look_SelfBomb"), 3.4f,
		[this]
		{
			SpawnTarget();
			PrepareBrawl(2400.f, { EPenguinAbility::SnowBomb });
			Penguin->SetTeam(-1);
			TimeA = -1.f;
			TimeB = -1.f;
			MarkA = 0.f;
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
		[this](float T)
		{
			// Far for the blast (its whole ring in the picture), then close for the charred stand and the pile.
			PlaceLookCamera(TimeB >= 0.f && T - TimeB > 0.3f ? FVector(0.f, 520.f, 110.f) : FVector(0.f, 1100.f, 230.f));
			if (!Penguin->IsKnockedOut())
			{
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(0.f, 0.f, -120.f));
			}
			if (!bFlag && T > 0.4f)
			{
				bFlag = true;
				Report(FString::Printf(TEXT("snow bomb at our feet used: %d"), Penguin->ActivateAbility(0) ? 1 : 0));
			}
			if (TimeB < 0.f && Penguin->IsKnockedOut())
			{
				TimeB = T;
			}
			if (TimeB >= 0.f)
			{
				const float Since = T - TimeB;
				if (MarkA < 1.f && Since > 0.12f)
				{
					MarkA = 1.f;
					Shot(TEXT("68_self_bomb_1_blast"));
				}
				if (MarkA < 2.f && Since > 0.6f)
				{
					MarkA = 2.f;
					Shot(TEXT("68_self_bomb_2_charred"));
				}
				if (MarkA < 3.f && Since > 1.7f)
				{
					MarkA = 3.f;
					Shot(TEXT("68_self_bomb_3_pile"));
				}
			}
		},
		[this]
		{
			Report(FString::Printf(TEXT("RESULT self bomb look: knocked out %d, soot shown %d, 3 shots taken %d, reason %s: %s"), Penguin->IsKnockedOut() ? 1 : 0,
				Penguin->IsDeathSootShown() || Penguin->IsDeathAshPileShown() ? 1 : 0, MarkA >= 3.f ? 1 : 0, *UEnum::GetValueAsString(Penguin->GetKOReason()),
				Penguin->IsKnockedOut() && MarkA >= 3.f && Penguin->GetKOReason() == EPenguinKOReason::Bomb ? TEXT("ok") : TEXT("FAIL")));
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
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});

	// ---- the bots' throw check: where would the bomb land? Inside our own push radius (330 cm and the margin): no. The dummy
	// stands on the strip, our penguin far to the left out of every arc.
	AddStep(TEXT("bot_BombThrow"), 0.6f,
		[this, Base]
		{
			SpawnTarget();
			PrepareBrawl(0.f, {});
			Penguin->SetTeam(-1);
			Teleport(Base + FVector(-1500.f, 0.f, 0.f));
			if (Target.IsValid())
			{
				Target->SetTeam(-1);
			}
		},
		[this, Base](float T)
		{
			if (bFlag || T < 0.3f || !Target.IsValid() || !Target->GetBot())
			{
				return;
			}
			bFlag = true;
			ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
			const UPenguinBotComponent* Brain = Target->GetBot();
			const APenguinCharacter* Dummy = Target.Get();
			const FVector Feet = Dummy->GetFeetLocation();
			const auto Safe = [Brain, Dummy, Flat, &Feet](float AimOffsetX) { return Brain->BombThrowIsSafe(Dummy, Flat, Feet + FVector(AimOffsetX, 0.f, 0.f)) ? 1 : 0; };
			CountA = Safe(300.f);                                    // 0
			CountB = Safe(700.f);                                    // 1
			const int32 Far = Safe(850.f);                           // 1 (the bots throw up to 850 cm)
			const int32 Left = Safe(-700.f);                         // 1
			const int32 AtFeet = Brain->BombThrowIsSafe(Dummy, Flat, Dummy->GetFeetLocation()) ? 1 : 0; // 0: dropped on its own feet
			// A four-block ice wall 2 m in front of it: the low arc to 7 m runs into it and lands right behind it.
			int32 BehindWall = -1;
			if (Flat)
			{
				const FIntPoint Foot = Flat->WorldToCell(FVector(Dummy->GetActorLocation().X + 200.f, 0.f, 50.f));
				TArray<FIntPoint> Cells;
				for (int32 Row = 0; Row < 4; ++Row)
				{
					Cells.Add(FIntPoint(Foot.X, Foot.Y + Row));
				}
				Flat->AddBlocks(Cells, EBlockKind::Ice, 0.f);
				BehindWall = Safe(700.f); // 0
			}
			const bool bOk = CountA == 0 && CountB == 1 && Far == 1 && Left == 1 && AtFeet == 0 && BehindWall == 0;
			Report(FString::Printf(TEXT("RESULT bot bomb throw: aimed 3 m away %d (want 0), 7 m %d (1), 8.5 m %d (1), 7 m the other way %d (1), at its own feet %d (0), 7 m behind a wall 2 m ahead %d (0): %s"),
				CountA, CountB, Far, Left, AtFeet, BehindWall, bOk ? TEXT("ok") : TEXT("FAIL")));
		},
		[this]
		{
			ClearAbilityLeftovers();
		});

	// ---- a live bomb beside a bot: it runs out of the blast. The dummy is the bot (our penguin stands far off as its target); the
	// bomb lies 1.2 m to the dummy's right with its fuse burning - the bomb of the dummy itself, or ours.
	enum class EFleeWant : uint8
	{
		Clear,   // out of the blast when it goes off (farther than the push radius), unhurt
		Passed,  // cornered at the island's end: through it and on past the bomb's middle (alive; the push may still reach it, inward)
		Shelled, // cornered with the ice shell ready: the shell holds the blast
	};
	struct FFlee
	{
		const TCHAR* Name;
		bool bOwn;         // the dummy's own bomb (else ours)
		float DummyOffset; // where the dummy stands, from the strip's base
		float BombSide;    // the bomb is this many cm to its right (negative: left)
		bool bShell;       // it has the ice shell
		EFleeWant Want;
	};
	static const FFlee Flees[] = {
		{ TEXT("Own"), true, 0.f, 120.f, false, EFleeWant::Clear },
		{ TEXT("Other"), false, 0.f, 120.f, false, EFleeWant::Clear },
		// at the end of the island (150 cm from it) with the bomb on the inner side: out through the blast
		{ TEXT("EdgeOwn"), true, 3150.f, -120.f, false, EFleeWant::Passed },
		// ... and with the shell ready: the shell
		{ TEXT("EdgeShell"), true, 3150.f, -120.f, true, EFleeWant::Shelled },
	};
	struct FFleeRun
	{
		float LastDist = -1.f;
		float LastSide = 0.f;  // the bot's X minus the bomb's, as of the last frame the bomb was there
		float BlastAt = -1.f;
		bool bShieldSeen = false;
		int32 Hits0 = 0;
	};
	for (const FFlee& Flee : Flees)
	{
		const TSharedRef<FFleeRun> Run = MakeShared<FFleeRun>();
		AddStep(*FString::Printf(TEXT("bot_BombFlee%s"), Flee.Name), 2.f,
			[this, Run, &Flee, Base]
			{
				SpawnTarget();
				PrepareBrawl(Flee.DummyOffset, {});
				*Run = FFleeRun();
				Penguin->SetTeam(-1);
				Teleport(Base + FVector(Flee.DummyOffset > 1000.f ? 1000.f : 2300.f, 0.f, 0.f)); // our penguin: its far-off target
				APenguinCharacter* Bot = Target.Get();
				if (!Bot || !Bot->GetBot())
				{
					return;
				}
				Bot->SetTeam(-1);
				SavedTargetAbilities = Bot->GetAbilities()->GetAbilities();
				Bot->GetAbilities()->SetAbilities({ Flee.bShell ? EPenguinAbility::IceShell : EPenguinAbility::Snowball });
				TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Flee.bOwn ? Bot : Penguin.Get(),
					FVector(Bot->GetActorLocation().X + Flee.BombSide, 0.f, 34.f), FVector::ZeroVector);
				Run->Hits0 = Bot->GetPenguinMovement() ? Bot->GetPenguinMovement()->HitEvents : 0;
				Bot->GetBot()->SetBotEnabled(true, 1);
			},
			[this, Run](float T)
			{
				const APenguinCharacter* Bot = Target.Get();
				const APenguinProjectile* Bomb = TestBombA.Get();
				if (!Bot)
				{
					return;
				}
				Run->bShieldSeen |= Bot->IsShielded();
				if (Bomb && Bomb->IsFlying())
				{
					Run->LastDist = Bot->DistanceTo(Bomb->GetActorLocation());
					Run->LastSide = static_cast<float>(Bot->GetActorLocation().X - Bomb->GetActorLocation().X);
				}
				else if (Run->BlastAt < 0.f && T > 0.3f)
				{
					Run->BlastAt = T;
				}
			},
			[this, Run, &Flee]
			{
				APenguinCharacter* Bot = Target.Get();
				if (!Bot || !Bot->GetPenguinMovement())
				{
					Report(FString::Printf(TEXT("RESULT bot bomb flee %s: FAIL (no bot)"), Flee.Name));
					return;
				}
				const int32 Hits = Bot->GetPenguinMovement()->HitEvents - Run->Hits0;
				const bool bBlasted = Run->BlastAt > 0.f;
				bool bGood = false;
				const TCHAR* Want = TEXT("");
				switch (Flee.Want)
				{
				case EFleeWant::Clear:
					Want = TEXT("farther than the push radius, not hit");
					bGood = Run->LastDist > PinguTuning::BombPushRadius && Hits == 0;
					break;
				case EFleeWant::Passed:
					// Cornered: past the bomb's middle toward the island (the bomb lay to its left, so its X is less than the bomb's).
					Want = TEXT("past the bomb's middle on the island's side, alive");
					bGood = Run->LastSide < -PinguTuning::BombKillRadius;
					break;
				case EFleeWant::Shelled:
					Want = TEXT("the shell up, not hit");
					bGood = Run->bShieldSeen && Hits == 0;
					break;
				}
				const bool bOk = bBlasted && Bot->IsAlive() && bGood;
				Report(FString::Printf(TEXT("RESULT bot bomb flee %s: the bomb went off %d after %.2f s, the bot %.0f cm from it then (push radius %.0f; %.0f cm to its right), shell %d, alive %d, hit %d x (want: %s): %s"),
					Flee.Name, bBlasted ? 1 : 0, Run->BlastAt, Run->LastDist, PinguTuning::BombPushRadius, Run->LastSide, Run->bShieldSeen ? 1 : 0, Bot->IsAlive() ? 1 : 0, Hits,
					Want, bOk ? TEXT("ok") : TEXT("FAIL")));
				if (Bot->GetBot())
				{
					Bot->GetBot()->SetBotEnabled(false);
				}
				if (SavedTargetAbilities.Num() > 0)
				{
					Bot->GetAbilities()->SetAbilities(SavedTargetAbilities);
				}
				ClearAbilityLeftovers();
				Bot->ResetForRound(Bot->GetActorLocation());
			});
	}
}
