// Pingu - the ability tuning of 2026-11-05 (card ability-tuning). The autotest's steps (APenguinAutoTest::AddAbilityTuningSteps;
// filter tuning_ / icicle_ + spawn_target):
//
//  tuning_SnowmanCooldown  the Snowman's cooldown is 35 % shorter than the old 9.5 s (6.175 s): the ability's number, and the
//                          cooldown really running after a build.
//  icicle_Melt             a key held past the full charge (1.5 s) + IcicleMeltAfterFull (5 s): the icicle melts in the flipper,
//                          nothing is thrown, the cooldown runs, a late key release does nothing (shot 97_icicle_melt).
//  icicle_LateRelease      let go 0.5 s before the melt: it is thrown as usual (power 1, cooldown from the throw, no melt).
//  icicle_BotHold          a bot's hold (ChargeAndThrow) of any length is let go before the melt: thrown, not melted.
//  (the pick rule - truly random, no forced killer - is checked by loadout_rule, anticheat_FairSeed and the traits' deal.)

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinProjectile.h"
#include "PinguTuning.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace PenguinAutoTestAbilityTuningPrivate
{
	const TCHAR* TuneOk(bool bOk) { return bOk ? TEXT("ok") : TEXT("FAIL"); }

	struct FRun
	{
		int32 Phase = 0;
		float PressAt = 0.f;
		int32 ThrownAtStart = 0;
		int32 MeltedAtStart = 0;
		int32 CancelledAtStart = 0;
		float CooldownBefore = -1.f;   // just before the melt
		float CooldownAfter = -1.f;    // a moment after it
		bool bInHandBefore = false;
		bool bShownAfter = true;
		int32 ThrownAfter = 0;
		int32 MeltedAfter = 0;
		int32 ThrownLate = 0;          // after the late key release
		int32 Shots = 0;
		float LastPower = -1.f;
		float CooldownAtThrow = -1.f;
	};
}

void APenguinAutoTest::AddAbilityTuningSteps()
{
	using namespace PenguinAutoTestAbilityTuningPrivate;
	constexpr float Full = PinguTuning::IcicleFullChargeTime;
	constexpr float Melt = PinguTuning::IcicleMeltAfterFull;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	// ---- the Snowman's cooldown: 9.5 s - 35 % = 6.175 s.
	AddStep(TEXT("tuning_SnowmanCooldown"), 0.8f,
		[this, Base]
		{
			PrepareBrawl(2400.f, { EPenguinAbility::Snowman });
			Teleport(Base + FVector(-150.f, 0.f, 0.f));
			Value = -1.f;
			CountA = 0;
		},
		[this](float T)
		{
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
			if (CountA == 0 && T > 0.2f)
			{
				CountA = Penguin->ActivateAbility(0) ? 1 : 2;
			}
			if (CountA == 1 && Value < 0.f && T > 0.3f)
			{
				Value = Abilities->GetCooldownRemaining(0);
			}
		},
		[this]
		{
			const float Table = UPenguinAbilityComponent::GetInfo(EPenguinAbility::Snowman).Cooldown;
			const float Want = 9.5f * 0.65f;
			const bool bOk = FMath::Abs(Table - Want) < 0.001f && CountA == 1 && Value > Table - 0.35f && Value <= Table;
			Report(FString::Printf(TEXT("RESULT tuning snowman cooldown: table %.3f s (want %.3f = 9.5 s - 35 %%), %.2f s left 0.1 s after a build (want just under it), built %d (want 1) %s"),
				Table, Want, Value, CountA == 1 ? 1 : 0, TuneOk(bOk)));
			ClearAbilityLeftovers();
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});

	// ---- icicle_Melt: the key held for ever.
	const TSharedRef<FRun> MeltRun = MakeShared<FRun>();
	AddStep(TEXT("icicle_Melt"), 0.5f + Full + Melt + 1.8f,
		[this, MeltRun]
		{
			*MeltRun = FRun();
			PrepareBrawl(2400.f, { EPenguinAbility::Icicle });
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			MeltRun->ThrownAtStart = Abilities->GetIciclesThrown();
			MeltRun->MeltedAtStart = Abilities->GetIciclesMelted();
			MeltRun->CancelledAtStart = Abilities->GetIciclesCancelled();
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, MeltRun](float T)
		{
			FRun& R = *MeltRun;
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Penguin->GetFeetLocation() + FVector(15.f, 0.f, 80.f);
				const FVector From = LookAt + FVector(10.f, 380.f, 25.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(700.f, 0.f, 0.f));
			switch (R.Phase)
			{
			case 0:
				if (T > 0.3f)
				{
					R.PressAt = T;
					Penguin->SetAbilityKeyDown(0, true);
					R.Phase = 1;
				}
				break;
			case 1: // just before the melt: still charging, in the flipper, no cooldown yet
				if (T >= R.PressAt + Full + Melt - 0.25f)
				{
					R.bInHandBefore = Abilities->IsIcicleInHand() && Abilities->GetIcicleShow() == EPinguIcicleShow::Charging;
					R.CooldownBefore = Abilities->GetCooldownRemaining(0);
					Shot(TEXT("97_icicle_melt_1")); // fully charged for 5 s: about to go
					++R.Shots;
					R.Phase = 2;
				}
				break;
			case 2: // just after it
				if (T >= R.PressAt + Full + Melt + 0.12f)
				{
					R.bShownAfter = Abilities->IsIcicleInHand() || Abilities->GetIcicleShow() != EPinguIcicleShow::None;
					R.CooldownAfter = Abilities->GetCooldownRemaining(0);
					R.ThrownAfter = Abilities->GetIciclesThrown();
					R.MeltedAfter = Abilities->GetIciclesMelted();
					Shot(TEXT("97_icicle_melt_2")); // gone: the poof
					++R.Shots;
					R.Phase = 3;
				}
				break;
			case 3: // the key is let go late: nothing comes out
				if (T >= R.PressAt + Full + Melt + 0.6f)
				{
					Penguin->SetAbilityKeyDown(0, false);
					R.Phase = 4;
				}
				break;
			case 4:
				if (T >= R.PressAt + Full + Melt + 1.5f)
				{
					R.ThrownLate = Abilities->GetIciclesThrown();
					R.Phase = 5;
				}
				break;
			default:
				break;
			}
		},
		[this, MeltRun]
		{
			const FRun& R = *MeltRun;
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const float Cooldown = UPenguinAbilityComponent::GetInfo(EPenguinAbility::Icicle).Cooldown;
			const bool bOk = R.Phase >= 5 && R.bInHandBefore && R.CooldownBefore == 0.f && !R.bShownAfter && R.MeltedAfter == R.MeltedAtStart + 1
				&& R.ThrownAfter == R.ThrownAtStart && R.ThrownLate == R.ThrownAtStart && R.CooldownAfter > Cooldown - 0.5f && R.Shots == 2;
			Report(FString::Printf(TEXT("RESULT icicle melt: held %.1f + %.1f s - in the flipper before it %d (want 1, cooldown %.2f s: want 0), after it in hand %d (want 0), melted %d (want 1), "
				"thrown %d / after the late release %d (want 0 0), cooldown %.2f s (want about %.1f) %s"),
				Full, Melt, R.bInHandBefore ? 1 : 0, R.CooldownBefore, R.bShownAfter ? 1 : 0, R.MeltedAfter - R.MeltedAtStart, R.ThrownAfter - R.ThrownAtStart,
				R.ThrownLate - R.ThrownAtStart, R.CooldownAfter, Cooldown, TuneOk(bOk)));
			ClearAbilityLeftovers();
			Penguin->CancelAbilityKeys();
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});

	// ---- icicle_LateRelease: let go 0.5 s before the melt: a full-charge throw.
	const TSharedRef<FRun> LateRun = MakeShared<FRun>();
	AddStep(TEXT("icicle_LateRelease"), 0.5f + Full + Melt + 1.f,
		[this, LateRun]
		{
			*LateRun = FRun();
			PrepareBrawl(2400.f, { EPenguinAbility::Icicle });
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			LateRun->ThrownAtStart = Abilities->GetIciclesThrown();
			LateRun->MeltedAtStart = Abilities->GetIciclesMelted();
		},
		[this, LateRun](float T)
		{
			FRun& R = *LateRun;
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(700.f, 0.f, 0.f));
			if (R.Phase == 0 && T > 0.3f)
			{
				R.PressAt = T;
				Penguin->SetAbilityKeyDown(0, true);
				R.Phase = 1;
			}
			else if (R.Phase == 1 && T >= R.PressAt + Full + Melt - 0.5f)
			{
				Penguin->SetAbilityKeyDown(0, false);
				R.Phase = 2;
			}
			else if (R.Phase == 2 && Abilities->GetIciclesThrown() > R.ThrownAtStart)
			{
				R.LastPower = Abilities->GetLastIciclePower();
				R.CooldownAtThrow = Abilities->GetCooldownRemaining(0);
				R.Phase = 3;
			}
		},
		[this, LateRun]
		{
			const FRun& R = *LateRun;
			const UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			const float Cooldown = UPenguinAbilityComponent::GetInfo(EPenguinAbility::Icicle).Cooldown;
			const bool bOk = R.Phase == 3 && R.LastPower > 0.99f && Abilities->GetIciclesMelted() == R.MeltedAtStart && R.CooldownAtThrow > Cooldown - 0.3f;
			Report(FString::Printf(TEXT("RESULT icicle late release: let go 0.5 s before the melt: thrown %d (want 1), charge %.2f (want 1), melted %d (want 0), cooldown %.2f s (want about %.1f) %s"),
				R.Phase == 3 ? 1 : 0, R.LastPower, Abilities->GetIciclesMelted() - R.MeltedAtStart, R.CooldownAtThrow, Cooldown, TuneOk(bOk)));
			ClearAbilityLeftovers();
			Penguin->CancelAbilityKeys();
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});

	// ---- icicle_BotHold: a hold of 99 s asked of ChargeAndThrow (what a bot does) is let go before the melt.
	const TSharedRef<FRun> BotRun = MakeShared<FRun>();
	AddStep(TEXT("icicle_BotHold"), 0.5f + Full + Melt + 1.f,
		[this, BotRun]
		{
			*BotRun = FRun();
			PrepareBrawl(2400.f, { EPenguinAbility::Icicle });
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			BotRun->ThrownAtStart = Abilities->GetIciclesThrown();
			BotRun->MeltedAtStart = Abilities->GetIciclesMelted();
		},
		[this, BotRun](float T)
		{
			FRun& R = *BotRun;
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(700.f, 0.f, 0.f));
			if (R.Phase == 0 && T > 0.3f)
			{
				R.PressAt = T;
				R.Phase = Abilities->ChargeAndThrow(0, 99.f) ? 1 : 9;
			}
			else if (R.Phase == 1 && Abilities->GetIciclesThrown() > R.ThrownAtStart)
			{
				R.CooldownAtThrow = T - R.PressAt; // the hold it really had
				R.Phase = 2;
			}
		},
		[this, BotRun]
		{
			const FRun& R = *BotRun;
			const UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			const bool bOk = R.Phase == 2 && R.CooldownAtThrow < Full + Melt && R.CooldownAtThrow > Full + Melt - 1.5f && Abilities->GetIciclesMelted() == R.MeltedAtStart;
			Report(FString::Printf(TEXT("RESULT icicle bot hold: 99 s asked, thrown after %.2f s (want under %.1f, the melt, and above %.1f), melted %d (want 0) %s"),
				R.CooldownAtThrow, Full + Melt, Full + Melt - 1.5f, Abilities->GetIciclesMelted() - R.MeltedAtStart, TuneOk(bOk)));
			ClearAbilityLeftovers();
			Penguin->CancelAbilityKeys();
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});
}
