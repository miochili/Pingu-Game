// Pingu - the kill cam (APinguKillCam, card 2026-11-07_kill-cam). The autotest's steps (APenguinAutoTest::AddKillCamSteps;
// filter killcam_ + spawn_target). The dummy is made the round's "last victim" (APinguKillCam::SetTestFinalVictim: practice has
// no round to end); the round flow itself is roundtest / matchtest.
//
//  killcam_Curve        the slow motion's curve: starts at 0.05x, eases in (faster and faster), covers exactly the game time
//                       asked for, ends at 1x; planned again from the middle it still covers the rest exactly.
//  killcam_Unescapable  knocked far out over the sea, stunned, no abilities: predicted before the splash (Approach), near-frozen,
//                       1x again exactly when the splash's death animation ends (0.9 s of game time after the knock-out), 2-3.5 s
//                       real in all; frames 98_killcam_<n> (look shots through the slow motion).
//  killcam_Escapable    knocked up over the island: never triggered, lands, normal speed throughout.
//  killcam_HookInReach  knocked out over the sea with a fish hook ready: it could hook the island's side until just above the sea, so
//                       the point of no return (and the slow motion) comes late, still before the splash.
//  killcam_HookOutOfReach  a fish hook ready but knocked too far out: predicted once the hook can't reach (the point of no return).
//  killcam_Icicle       an icicle flying at it: predicted ShotWindow before the hit, the icicle's death animation in slow motion.
//  killcam_NotFinal     the same knock-out without being the last victim: no slow motion at all.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguKillCam.h"

namespace PenguinAutoTestKillCamPrivate
{
	FString Ok(bool bOk) { return bOk ? TEXT("ok") : TEXT("FAIL"); }

	/** What a kill cam step saw. */
	struct FRun
	{
		bool bHit = false;
		float HitT = -1.f;
		float FirstTriggerT = -1.f;
		float KOT = -1.f;
		float LowestWorld = 1.f;
		/** The kill cam's own dilation (the world's also dips for the hits' hit-stops). */
		float LowestKillCam = 1.f;
		float HighestZoom = 0.f;
		int32 Shots = 0;
		float NextShotReal = 0.f;
		double StartReal = 0.0;
		bool bLanded = false;
	};

	FRun& Run()
	{
		static FRun State;
		return State;
	}

	float WorldDilation(const UWorld* World)
	{
		const AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
		return Settings ? Settings->GetEffectiveTimeDilation() : 1.f;
	}
}

void APenguinAutoTest::AddKillCamSteps()
{
	using namespace PenguinAutoTestKillCamPrivate;

	// The dummy as the last victim, a clean kill cam.
	const auto Prepare = [this](const TArray<EPenguinAbility>& TargetAbilities)
	{
		Run() = FRun();
		PrepareBrawl(600.f, { EPenguinAbility::Icicle });
		if (APinguKillCam* KillCam = APinguKillCam::Get(this))
		{
			KillCam->ResetTestStats();
			KillCam->SetTestFinalVictim(Target.Get());
		}
		if (Target.IsValid())
		{
			Target->GetAbilities()->SetAbilities(TargetAbilities);
		}
		Run().StartReal = GetWorld()->GetAudioTimeSeconds();
	};
	// Every frame: the lowest dilation, the zoom, the first trigger and the knock-out.
	const auto Watch = [this](float T, const TCHAR* ShotPrefix)
	{
		FRun& R = Run();
		const APinguKillCam* KillCam = APinguKillCam::Get(this);
		R.LowestWorld = FMath::Min(R.LowestWorld, WorldDilation(GetWorld()));
		if (KillCam)
		{
			R.HighestZoom = FMath::Max(R.HighestZoom, KillCam->GetZoomBlend());
			R.LowestKillCam = FMath::Min(R.LowestKillCam, KillCam->GetDilation());
			if (R.FirstTriggerT < 0.f && KillCam->GetStage() != EPinguKillCamStage::Off && KillCam->GetStage() != EPinguKillCamStage::Release)
			{
				R.FirstTriggerT = T;
			}
		}
		if (R.KOT < 0.f && Target.IsValid() && Target->IsKnockedOut())
		{
			R.KOT = T;
		}
		if (R.bHit && Target.IsValid() && !Target->IsKnockedOut() && Target->GetPenguinMovement()->IsMovingOnGround() && T > R.HitT + 0.2f)
		{
			R.bLanded = true;
		}
		// Look shots through the slow motion: every 0.35 s of real time while it runs (and one just after).
		const float Real = static_cast<float>(GetWorld()->GetAudioTimeSeconds() - R.StartReal);
		if (ShotPrefix && R.FirstTriggerT >= 0.f && R.Shots < 8 && Real >= R.NextShotReal)
		{
			R.NextShotReal = Real + 0.35f;
			Shot(*FString::Printf(TEXT("98_killcam_%s_%d_speed%03d"), ShotPrefix, R.Shots, FMath::RoundToInt(WorldDilation(GetWorld()) * 100.f)));
			++R.Shots;
		}
	};
	const auto Hit = [this](const FVector2D& Knockback, float Stun)
	{
		FPenguinHit H;
		H.Instigator = Penguin.Get();
		H.Source = EPenguinAbility::FlipperSlap;
		H.Knockback = Knockback;
		H.Stun = Stun;
		H.Location = Target->GetActorLocation();
		Target->ApplyPenguinHit(H);
		Run().bHit = true;
	};
	const auto EdgeX = [this]()
	{
		const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), FVector(FlatIslandX, 0.f, 0.f));
		const FBox Box = Flat ? Flat->GetBounds() : FBox(ForceInit);
		return Box.IsValid ? static_cast<float>(Box.Max.X) : FlatIslandX + 3000.f;
	};
	const auto Finish = [this]()
	{
		if (APinguKillCam* KillCam = APinguKillCam::Get(this))
		{
			KillCam->SetTestFinalVictim(nullptr);
		}
	};

	AddStep(TEXT("killcam_Curve"), 0.1f, nullptr, nullptr,
		[this]
		{
			bool bAll = true;
			FString Line;
			for (const float G : { 0.3f, 0.9f, 1.2f, 1.65f, 2.4f })
			{
				float From = 0.f;
				float Exponent = 0.f;
				float Duration = 0.f;
				PinguKillCam::PlanCurve(G, From, Exponent, Duration);
				// Numerically: the game time covered, the start, the end, speeding up faster and faster (each step's gain bigger).
				constexpr int32 Steps = 400;
				float Sum = 0.f;
				float LastGain = -1.f;
				bool bEaseIn = true;
				float Prev = PinguKillCam::CurveAt(From, Exponent, Duration, 0.f);
				for (int32 Step = 1; Step <= Steps; ++Step)
				{
					const float T = Duration * Step / Steps;
					const float D = PinguKillCam::CurveAt(From, Exponent, Duration, T);
					Sum += 0.5f * (D + Prev) * Duration / Steps;
					bEaseIn &= D - Prev >= LastGain - 1.e-5f;
					LastGain = D - Prev;
					Prev = D;
				}
				// Planned again halfway: the rest from there covers exactly what it is asked to.
				const float Mid = PinguKillCam::CurveAt(From, Exponent, Duration, Duration * 0.5f);
				const float Rest = PinguKillCam::PlanRest(Mid, Exponent, 0.85f);
				const float RestGame = PinguKillCam::GameTimeOf(Mid, Exponent, Rest);
				const bool bOk = FMath::Abs(Sum - G) < 0.01f && FMath::IsNearlyEqual(Prev, 1.f) && bEaseIn && Exponent >= 1.f
					&& FMath::Abs(RestGame - 0.85f) < 0.005f && From <= PinguKillCam::StartDilation + 0.001f
					&& (G < 0.8f || G > PinguKillCam::MaxLead + PinguKillCam::SeaDeathShowTime + 0.01f
						|| (Duration >= PinguKillCam::RealMin - 0.01f && Duration <= PinguKillCam::RealMax + 0.01f));
				bAll &= bOk;
				Line += FString::Printf(TEXT("%s%.1f s game: from %.2fx, power %.2f, %.2f s real, covers %.3f, ends %.2fx, ease-in %d, rest %.3f"),
					Line.IsEmpty() ? TEXT("") : TEXT("; "), G, From, Exponent, Duration, Sum, Prev, bEaseIn ? 1 : 0, RestGame);
			}
			Report(FString::Printf(TEXT("RESULT killcam curve: %s %s"), *Line, *Ok(bAll)));
		});

	AddStep(TEXT("killcam_Unescapable"), 5.f,
		[this, Prepare, EdgeX]
		{
			Prepare({});
		},
		[this, Watch, Hit, EdgeX](float T)
		{
			if (!Run().bHit && T > 0.15f && Target.IsValid())
			{
				Run().HitT = T;
				// Put over the sea past the island's end, a little above its top, and hit the same frame.
				Target->NetTeleport(FVector(EdgeX() + 500.f, 0.f, 260.f), FVector::ZeroVector, true);
				Hit(FVector2D(1500.f, 400.f), 1.2f);
			}
			Watch(T, TEXT("sea"));
		},
		[this, Finish]
		{
			const FRun& R = Run();
			const APinguKillCam* KillCam = APinguKillCam::Get(this);
			const APinguKillCam::FTestStats S = KillCam ? KillCam->GetTestStats() : APinguKillCam::FTestStats();
			const bool bBeforeKO = R.FirstTriggerT >= 0.f && R.KOT > R.FirstTriggerT;
			const bool bOk = S.Predicted == 1 && S.FalseTriggers == 0 && S.Showcases == 1 && bBeforeKO && S.Lowest <= 0.06f
				&& FMath::Abs(S.KOToNormalGame - S.ShowTime) < 0.06f && FMath::IsNearlyEqual(S.ShowTime, PinguKillCam::SeaDeathShowTime)
				&& S.TotalReal >= 2.f && S.TotalReal <= 3.6f && R.HighestZoom > 0.99f && FMath::IsNearlyEqual(WorldDilation(GetWorld()), 1.f);
			Report(FString::Printf(TEXT("RESULT killcam unescapable: predicted %d (want 1, before the knock-out: trigger %.2f s, KO %.2f s game; no return %.2f s, ")
				TEXT("slow motion from %.2f s before the splash), false %d, ")
				TEXT("lowest %.3fx (want 0.05), 1x %.3f s of game time after the knock-out (want the death animation's %.2f), %.2f s real in all (want 2-3.5), ")
				TEXT("zoom %.2f, now %.2fx %s"), S.Predicted, R.FirstTriggerT, R.KOT, S.NoReturnLead, S.TriggerLead, S.FalseTriggers, S.Lowest, S.KOToNormalGame, S.ShowTime, S.TotalReal,
				R.HighestZoom, WorldDilation(GetWorld()), *Ok(bOk)));
			Finish();
		});

	AddStep(TEXT("killcam_Escapable"), 2.5f,
		[this, Prepare, EdgeX]
		{
			Prepare({});
			Target->NetTeleport(FVector(EdgeX() - 900.f, 0.f, 62.f), FVector::ZeroVector, false);
		},
		[this, Watch, Hit, EdgeX](float T)
		{
			if (!Run().bHit && T > 0.15f && Target.IsValid())
			{
				Run().HitT = T;
				Hit(FVector2D(350.f, 1300.f), 0.4f);
			}
			Watch(T, nullptr);
		},
		[this, Finish]
		{
			const FRun& R = Run();
			const APinguKillCam* KillCam = APinguKillCam::Get(this);
			const APinguKillCam::FTestStats S = KillCam ? KillCam->GetTestStats() : APinguKillCam::FTestStats();
			const bool bOk = S.Predicted == 0 && S.Showcases == 0 && R.KOT < 0.f && R.bLanded && R.LowestKillCam >= 1.f;
			Report(FString::Printf(TEXT("RESULT killcam escapable: predicted %d, slow motions %d (want 0 0), knocked out %d (want 0), landed %d, the kill cam's lowest %.2fx (want 1) %s"),
				S.Predicted, S.Showcases, R.KOT >= 0.f ? 1 : 0, R.bLanded ? 1 : 0, R.LowestKillCam, *Ok(bOk)));
			Finish();
		});

	AddStep(TEXT("killcam_HookInReach"), 5.f,
		[this, Prepare, EdgeX]
		{
			Prepare({ EPenguinAbility::FishHook });
		},
		[this, Watch, Hit, EdgeX](float T)
		{
			if (!Run().bHit && T > 0.15f && Target.IsValid())
			{
				Run().HitT = T;
				// Put over the sea past the island's end, a little above its top, and hit the same frame.
				Target->NetTeleport(FVector(EdgeX() + 500.f, 0.f, 260.f), FVector::ZeroVector, true);
				Hit(FVector2D(1500.f, 400.f), 1.2f);
			}
			Watch(T, nullptr);
		},
		[this, Finish]
		{
			const FRun& R = Run();
			const APinguKillCam* KillCam = APinguKillCam::Get(this);
			const APinguKillCam::FTestStats S = KillCam ? KillCam->GetTestStats() : APinguKillCam::FTestStats();
			// The hook reaches the island's side all the way down: no return only when even a hook would come too late (near the
			// sea), so the slow motion starts late - but before the splash, and never while it could still get back.
			const bool bOk = S.Predicted == 1 && S.FalseTriggers == 0 && S.Showcases == 1 && R.KOT > R.FirstTriggerT && S.NoReturnLead >= 0.f
				&& S.NoReturnLead < 0.75f && FMath::Abs(S.KOToNormalGame - S.ShowTime) < 0.06f && S.Lowest <= 0.06f;
			Report(FString::Printf(TEXT("RESULT killcam hook in reach: predicted %d (want 1, late: the hook could save it until then), false %d, no return %.2f s ")
				TEXT("before the splash (want under 0.75: later than without the hook), trigger %.2f s, KO %.2f s, lowest %.3fx, 1x %.3f s of game time after the ")
				TEXT("knock-out (want %.2f) %s"), S.Predicted, S.FalseTriggers, S.NoReturnLead, R.FirstTriggerT, R.KOT, S.Lowest, S.KOToNormalGame, S.ShowTime, *Ok(bOk)));
			Finish();
		});

	AddStep(TEXT("killcam_HookOutOfReach"), 5.f,
		[this, Prepare]
		{
			Prepare({ EPenguinAbility::FishHook });
		},
		[this, Watch, Hit, EdgeX](float T)
		{
			if (!Run().bHit && T > 0.15f && Target.IsValid())
			{
				Run().HitT = T;
				// Far out (35 m), flying further: the hook can't reach the island any more.
				Target->NetTeleport(FVector(EdgeX() + 3400.f, 0.f, 0.f), FVector::ZeroVector, true);
				Hit(FVector2D(1600.f, 300.f), 0.3f);
			}
			Watch(T, TEXT("hook"));
		},
		[this, Finish]
		{
			const FRun& R = Run();
			const APinguKillCam* KillCam = APinguKillCam::Get(this);
			const APinguKillCam::FTestStats S = KillCam ? KillCam->GetTestStats() : APinguKillCam::FTestStats();
			const bool bOk = S.Predicted == 1 && S.FalseTriggers == 0 && R.FirstTriggerT >= 0.f && R.KOT > R.FirstTriggerT && S.NoReturnLead >= S.TriggerLead
				&& S.TriggerLead > 0.f && S.TriggerLead <= PinguKillCam::MaxLead + 0.02f && FMath::Abs(S.KOToNormalGame - S.ShowTime) < 0.06f;
			Report(FString::Printf(TEXT("RESULT killcam hook out of reach: predicted %d (want 1: the hook no longer reaches), false %d, no return %.2f s before the splash, ")
				TEXT("slow motion from %.2f s before it (want up to %.2f), trigger %.2f s, KO %.2f s, 1x %.3f s after the knock-out (want %.2f) %s"), S.Predicted,
				S.FalseTriggers, S.NoReturnLead, S.TriggerLead, PinguKillCam::MaxLead, R.FirstTriggerT, R.KOT, S.KOToNormalGame, S.ShowTime, *Ok(bOk)));
			Finish();
		});

	AddStep(TEXT("killcam_Icicle"), 4.f,
		[this, Prepare]
		{
			Prepare({});
		},
		[this, Watch](float T)
		{
			FRun& R = Run();
			if (!R.bHit && T > 0.2f && Target.IsValid())
			{
				R.bHit = true;
				R.HitT = T;
				const FVector From = Penguin->GetActorLocation() + FVector(80.f, 0.f, 10.f);
				APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Icicle, Penguin.Get(), From, FVector(2600.f, 0.f, 0.f));
			}
			Watch(T, TEXT("icicle"));
		},
		[this, Finish]
		{
			const FRun& R = Run();
			const APinguKillCam* KillCam = APinguKillCam::Get(this);
			const APinguKillCam::FTestStats S = KillCam ? KillCam->GetTestStats() : APinguKillCam::FTestStats();
			const float Lead = R.KOT - R.FirstTriggerT;
			const bool bOk = S.PredictedShot == 1 && S.FalseTriggers == 0 && Target.IsValid() && R.KOT >= 0.f && Lead > 0.f && Lead <= PinguKillCam::ShotWindow + 0.03f
				&& FMath::Abs(S.KOToNormalGame - S.ShowTime) < 0.06f && FMath::IsNearlyEqual(S.ShowTime, APenguinCharacter::GetDeathShowTime(EPenguinKOReason::Icicle))
				&& S.Lowest <= 0.06f;
			Report(FString::Printf(TEXT("RESULT killcam icicle: the shot predicted %d (want 1), %.3f s of game time before the hit (want up to %.2f), false %d, ")
				TEXT("lowest %.3fx, 1x %.3f s of game time after the knock-out (want %.2f) %s"), S.PredictedShot, Lead, PinguKillCam::ShotWindow, S.FalseTriggers,
				S.Lowest, S.KOToNormalGame, S.ShowTime, *Ok(bOk)));
			Finish();
		});

	AddStep(TEXT("killcam_NotFinal"), 4.f,
		[this, Prepare, EdgeX, Finish]
		{
			Prepare({});
			Finish(); // (not the last victim: practice has no round to end)
		},
		[this, Watch, Hit, EdgeX](float T)
		{
			if (!Run().bHit && T > 0.15f && Target.IsValid())
			{
				Run().HitT = T;
				// Put over the sea past the island's end, a little above its top, and hit the same frame.
				Target->NetTeleport(FVector(EdgeX() + 500.f, 0.f, 260.f), FVector::ZeroVector, true);
				Hit(FVector2D(1500.f, 400.f), 1.2f);
			}
			Watch(T, nullptr);
		},
		[this]
		{
			const FRun& R = Run();
			const APinguKillCam* KillCam = APinguKillCam::Get(this);
			const APinguKillCam::FTestStats S = KillCam ? KillCam->GetTestStats() : APinguKillCam::FTestStats();
			const bool bOk = R.KOT >= 0.f && S.Predicted == 0 && S.Showcases == 0 && R.FirstTriggerT < 0.f && R.LowestKillCam >= 1.f;
			Report(FString::Printf(TEXT("RESULT killcam not final: knocked out %d (want 1), predicted %d, slow motions %d (want 0 0), the kill cam's lowest %.2fx (want 1) %s"),
				R.KOT >= 0.f ? 1 : 0, S.Predicted, S.Showcases, R.LowestKillCam, *Ok(bOk)));
		});
}
