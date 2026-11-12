// Pingu - the snowball's combo and its bounce off a frozen penguin's ice: the autotest's steps (APenguinAutoTest::AddSnowballSteps).
//
// snowball_Combo: nine snowballs in a row at the dummy, each thrown the moment the last hit gave the ability back: the push grows
// +35 % a hit (x1.00 / 1.35 / 1.70 / 2.05 / 2.40 / 2.75, then the x3 cap for the 7th, 8th and 9th), measured as the dummy's knock
// speed (UPenguinMovementComponent::LastHitSpeed). The chain counter, the pop the victim carries (APenguinCharacter::ShowComboHit)
// and the refund (no cooldown left at the hit) are read on the way.
// snowball_ComboWindow: the chain starts over after SnowballComboWindow (a hit that comes later is the first again), goes on after
// that, and a snowball that misses breaks it - even when the next hit comes inside the window.
// snowball_IceCube: a frozen dummy: a snowball bursts on its ice (still frozen, ice still showing, no hit, no knock-out, only a small
// nudge, no snowball back for the thrower, no combo) twice; a hit made by hand does not land either; then a flipper slap shatters the
// ice as before.
// look_SnowballCombo: the combo's pictures with the UI (the "x2", "x4" and capped "x7" popping at the dummy), and the bounce
// (69_snowball_combo_*, 69_snowball_vs_ice, 69_snowball_vs_ice_slap).

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PinguTuning.h"
#include "PinguUI.h"
#include "Engine/World.h"

namespace PenguinAutoTestSnowballPrivate
{
	/** How far from the thrower the dummy stands (cm): a short throw (about 0.2 s of flight). */
	constexpr float ComboDistance = 400.f;
	/** The pause between a hit and the next throw: the dummy is put back in its place meanwhile (s). */
	constexpr float HitGap = 0.12f;
	/** The combo run: how many snowballs, and the push each should have (x the first one's): +35 % a hit, capped at x3. */
	constexpr int32 RunHits = 9;
	static const float WantScale[RunHits] = { 1.f, 1.35f, 1.7f, 2.05f, 2.4f, 2.75f, 3.f, 3.f, 3.f };
	/** The ratios must come out within this share of what is wanted. */
	constexpr float RatioTolerance = 0.03f;

	/** What one landed hit left behind (read the frame after it). */
	struct FHitRecord
	{
		bool bSeen = false;
		float At = 0.f;           // test time
		float Speed = 0.f;        // the dummy's knock speed (cm/s)
		float RoundScale = 1.f;   // the fight's knockback growth at that moment (1 outside a fight: the autotest is none)
		int32 Chain = 0;          // the thrower's chain counter
		int32 PopCount = 0;       // the number the victim carries for the overhead pop
		int32 PopSerial = 0;
		float Cooldown = -1.f;    // the thrower's snowball cooldown left (0: given back)
		float Since = -1.f;       // seconds since the hit before
	};

	struct FComboRun
	{
		int32 Stage = 0;          // 0 waiting to throw, 1 in the air, 2 landed (waiting out the gap), 3 gave up
		int32 Index = 0;          // the hit being thrown / just landed
		float NextAt = 0.3f;
		float ThrowAt = 0.f;
		int32 HitsBefore = 0;
		int32 Refused = 0;        // throws the ability refused (it should always be ready: the last hit gave it back)
		float LastHitAt = -1.f;
		FVector DummyAt = FVector::ZeroVector;
		FHitRecord Hits[RunHits];
	};

	/** snowball_ComboWindow's plan: a hit, one right after it, one after the window, one right after that, a miss, a hit. */
	struct FWindowAction
	{
		bool bMiss;
		float Wait;          // pause before the throw (s), on top of the ability being ready
		float WantScale;     // the push it should have (x the first hit's)
		int32 WantChain;     // the chain counter after it
	};
	static const FWindowAction WindowPlan[] = {
		{ false, 0.f, 1.f, 1 },    // a first hit
		{ false, 0.f, 1.35f, 2 },  // right after it: the second
		{ false, 1.7f, 1.f, 1 },   // later than the window: a new chain's first
		{ false, 0.f, 1.35f, 2 },  // the new chain goes on
		{ true, 0.f, 0.f, 0 },     // a miss ...
		{ false, 0.f, 1.f, 1 },    // ... breaks the chain although this hit comes inside the window
	};
	constexpr int32 WindowActions = UE_ARRAY_COUNT(WindowPlan);

	struct FWindowRun
	{
		int32 Stage = 0;
		int32 Action = 0;
		float NextAt = 0.3f;
		float ThrowAt = 0.f;
		int32 HitsBefore = 0;
		float LastHitAt = -1.f;
		float SinceLastHit = -1.f;     // the last action's hit after the hit before it
		FVector DummyAt = FVector::ZeroVector;
		FHitRecord Hits[WindowActions];
	};

	/** snowball_IceCube: a frozen dummy against snowballs, then a slap. */
	struct FIceRun
	{
		int32 Stage = 0;
		float StageAt = 0.f;
		FVector DummyAt = FVector::ZeroVector;
		bool bFrozenStart = false;
		bool bIceStart = false;
		int32 HitsBefore = 0;
		float StartX = 0.f;
		float MaxSpeed = 0.f;
		bool bKnockedOut = false;
		bool bStunned = false;
		bool bShotBounce = false;
		bool bShotSlap = false;
		// after snowball 1 / 2: still frozen, ice showing, hits taken, moved (cm), snowball cooldown left (0 would be a refund), chain
		bool bFrozen[2] = { false, false };
		bool bIce[2] = { false, false };
		int32 Hits[2] = { -1, -1 };
		float Moved[2] = { -1.f, -1.f };
		float Cooldown[2] = { -1.f, -1.f };
		int32 Chain[2] = { -1, -1 };
		bool bThrown[2] = { false, false };
		bool bHandLanded = true;      // a snowball hit made by hand: did it land (it must not)
		// after the slap: thawed, ice gone, hits taken, speed after, stunned, knocked out
		bool bSlapThawed = false;
		bool bSlapIceGone = false;
		int32 SlapHits = -1;
		float SlapSpeed = -1.f;
		bool bSlapStunned = false;
		bool bSlapUsed = false;
	};

	/** snowball look: the combo up close with the UI. */
	struct FComboLook
	{
		int32 Stage = 0;
		int32 Index = 0;
		float NextAt = 0.4f;
		float ThrowAt = 0.f;
		float LandedAt = -1.f;
		int32 HitsBefore = 0;
		int32 Shots = 0;
		int32 PopStartsBase = 0;
		int32 ShotsWithPop = 0;
		int32 LastShotCount = 0;
		FVector DummyAt = FVector::ZeroVector;
		bool bBounceShot = false;
	};
	/** The hits that get a picture (1-based: the 2nd, the 4th and the capped 7th). */
	constexpr int32 LookShotHits[3] = { 2, 4, 7 };
	constexpr int32 LookHits = 7;
}

void APenguinAutoTest::AddSnowballSteps()
{
	using namespace PenguinAutoTestSnowballPrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	// A snowball thrown (the real ability: slot 0) at the dummy, which is put back in its place first. False if the ability was
	// not ready.
	const auto Throw = [this](const FVector& DummyAt, int32& OutHitsBefore) -> bool
	{
		if (!Target.IsValid() || !Penguin.IsValid())
		{
			return false;
		}
		ResetTarget(DummyAt);
		OutHitsBefore = Target->GetPenguinMovement() ? Target->GetPenguinMovement()->HitEvents : 0;
		Penguin->SetAimPoint(Target->GetAimOrigin());
		return Penguin->ActivateAbility(0);
	};
	// What a landed hit left behind.
	const auto Read = [this](float T, float LastHitAt) -> FHitRecord
	{
		FHitRecord Record;
		Record.bSeen = true;
		Record.At = T;
		Record.Speed = Target->GetPenguinMovement()->LastHitSpeed;
		const APenguinGameState* FlowState = APenguinGameState::Get(this);
		Record.RoundScale = FlowState ? FlowState->GetKnockbackScale() : 1.f;
		Record.Chain = Penguin->GetAbilities()->GetSnowballComboHits();
		Record.PopCount = Target->GetComboPopCount();
		Record.PopSerial = Target->GetComboPopSerial();
		Record.Cooldown = Penguin->GetAbilities()->GetCooldownRemaining(0);
		Record.Since = LastHitAt >= 0.f ? T - LastHitAt : -1.f;
		return Record;
	};

	// ---- snowball_Combo: nine hits in a row.
	{
		const TSharedRef<FComboRun> S = MakeShared<FComboRun>();
		AddStep(TEXT("snowball_Combo"), 7.f,
			[this, S, Base]
			{
				*S = FComboRun();
				PrepareBrawl(ComboDistance, { EPenguinAbility::Snowball });
				S->DummyAt = Base + FVector(ComboDistance, 0.f, 0.f);
			},
			[this, S, Throw, Read](float T)
			{
				FComboRun& R = *S;
				if (!Target.IsValid() || R.Index >= RunHits || R.Stage == 3)
				{
					return;
				}
				const UPenguinMovementComponent* TargetMove = Target->GetPenguinMovement();
				if (R.Stage == 0 && T >= R.NextAt)
				{
					if (Throw(R.DummyAt, R.HitsBefore))
					{
						R.Stage = 1;
						R.ThrowAt = T;
					}
					else if (++R.Refused > 30)
					{
						R.Stage = 3; // the ability never came back
					}
				}
				else if (R.Stage == 1)
				{
					if (TargetMove && TargetMove->HitEvents != R.HitsBefore)
					{
						R.Hits[R.Index] = Read(T, R.LastHitAt);
						R.LastHitAt = T;
						R.Stage = 2;
						R.NextAt = T + HitGap;
					}
					else if (T > R.ThrowAt + 1.f)
					{
						R.Stage = 3; // a miss: the combo is over
					}
				}
				else if (R.Stage == 2 && T >= R.NextAt)
				{
					++R.Index;
					R.Stage = 0;
				}
			},
			[this, S]
			{
				const FComboRun& R = *S;
				int32 Seen = 0;
				for (const FHitRecord& Hit : R.Hits)
				{
					Seen += Hit.bSeen ? 1 : 0;
				}
				// Every hit's push against the first one's, with the fight's own growth taken out.
				const float FirstSpeed = R.Hits[0].bSeen ? R.Hits[0].Speed / FMath::Max(R.Hits[0].RoundScale, 0.01f) : 0.f;
				float Ratio[RunHits];
				bool bRatiosOk = Seen == RunHits && FirstSpeed > 100.f;
				bool bChainOk = Seen == RunHits;
				bool bPopOk = Seen == RunHits;
				bool bRefundOk = Seen == RunHits;
				FString Ratios;
				FString Chains;
				for (int32 Index = 0; Index < RunHits; ++Index)
				{
					const FHitRecord& Hit = R.Hits[Index];
					Ratio[Index] = Hit.bSeen && FirstSpeed > 1.f ? Hit.Speed / FMath::Max(Hit.RoundScale, 0.01f) / FirstSpeed : 0.f;
					bRatiosOk &= FMath::Abs(Ratio[Index] - WantScale[Index]) <= RatioTolerance * WantScale[Index];
					bChainOk &= Hit.Chain == Index + 1;
					// The pop: from the second hit on the victim carries the hit's number, and the serial moves on with each.
					bPopOk &= Index == 0 || (Hit.PopCount == Index + 1 && (Index == 1 || Hit.PopSerial != R.Hits[Index - 1].PopSerial));
					bRefundOk &= Hit.Cooldown >= 0.f && Hit.Cooldown < 0.01f;
					Ratios += FString::Printf(TEXT("%s%.2f"), Index > 0 ? TEXT(" / ") : TEXT(""), Ratio[Index]);
					Chains += FString::Printf(TEXT("%s%d"), Index > 0 ? TEXT(" ") : TEXT(""), Hit.Chain);
				}
				const bool bOk = bRatiosOk && bChainOk && bPopOk && bRefundOk;
				Report(FString::Printf(TEXT("RESULT snowball combo: %d of %d hits in a row, push x%s (want 1 / 1.35 / 1.7 / 2.05 / 2.4 / 2.75 / 3 / 3 / 3, within %.0f %%), "
					"first hit %.0f cm/s, chain counter %s %s, the victim's pop numbers %s, the snowball given back at every hit %s, throws refused %d: %s"),
					Seen, RunHits, *Ratios, RatioTolerance * 100.f, FirstSpeed, *Chains, bChainOk ? TEXT("ok") : TEXT("WRONG"), bPopOk ? TEXT("ok") : TEXT("WRONG"),
					bRefundOk ? TEXT("yes") : TEXT("NO"), R.Refused, bOk ? TEXT("ok") : TEXT("FAIL")));
				ResetTarget(R.DummyAt);
			});
	}

	// ---- snowball_ComboWindow: the chain starts over after its window, and a miss breaks it.
	{
		const TSharedRef<FWindowRun> S = MakeShared<FWindowRun>();
		AddStep(TEXT("snowball_ComboWindow"), 10.f,
			[this, S, Base]
			{
				*S = FWindowRun();
				PrepareBrawl(ComboDistance, { EPenguinAbility::Snowball });
				S->DummyAt = Base + FVector(ComboDistance, 0.f, 0.f);
			},
			[this, S, Throw, Read](float T)
			{
				FWindowRun& R = *S;
				if (!Target.IsValid() || R.Action >= WindowActions || R.Stage == 3)
				{
					return;
				}
				const FWindowAction& Action = WindowPlan[R.Action];
				const UPenguinMovementComponent* TargetMove = Target->GetPenguinMovement();
				const UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
				if (R.Stage == 0 && T >= R.NextAt && Abilities->IsReady(0))
				{
					if (Action.bMiss)
					{
						// Thrown up into the sky over the dummy: it misses.
						Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(150.f, 0.f, 900.f));
						R.Stage = Penguin->ActivateAbility(0) ? 1 : 3;
						R.ThrowAt = T;
					}
					else if (Throw(R.DummyAt, R.HitsBefore))
					{
						R.Stage = 1;
						R.ThrowAt = T;
					}
					else
					{
						R.Stage = 3;
					}
				}
				else if (R.Stage == 1)
				{
					if (Action.bMiss)
					{
						if (T > R.ThrowAt + 0.8f) // (its life is 0.55 s)
						{
							R.Stage = 2;
						}
					}
					else if (TargetMove && TargetMove->HitEvents != R.HitsBefore)
					{
						R.Hits[R.Action] = Read(T, R.LastHitAt);
						R.SinceLastHit = R.LastHitAt >= 0.f ? T - R.LastHitAt : -1.f;
						R.LastHitAt = T;
						R.Stage = 2;
					}
					else if (T > R.ThrowAt + 1.f)
					{
						R.Stage = 3;
					}
				}
				if (R.Stage == 2)
				{
					++R.Action;
					R.Stage = 0;
					R.NextAt = T + HitGap + (R.Action < WindowActions ? WindowPlan[R.Action].Wait : 0.f);
				}
			},
			[this, S]
			{
				const FWindowRun& R = *S;
				// Pushes against the first hit's (the fight's growth taken out), chain counters, and the plan's expectations.
				const FHitRecord& First = R.Hits[0];
				const float FirstSpeed = First.bSeen ? First.Speed / FMath::Max(First.RoundScale, 0.01f) : 0.f;
				bool bOk = R.Action >= WindowActions && FirstSpeed > 100.f;
				FString Ratios;
				FString Chains;
				for (int32 Index = 0; Index < WindowActions; ++Index)
				{
					const FWindowAction& Plan = WindowPlan[Index];
					if (Plan.bMiss)
					{
						continue;
					}
					const FHitRecord& Hit = R.Hits[Index];
					const float Ratio = Hit.bSeen && FirstSpeed > 1.f ? Hit.Speed / FMath::Max(Hit.RoundScale, 0.01f) / FirstSpeed : 0.f;
					bOk &= Hit.bSeen && FMath::Abs(Ratio - Plan.WantScale) <= RatioTolerance * Plan.WantScale && Hit.Chain == Plan.WantChain;
					Ratios += FString::Printf(TEXT("%s%.2f"), Ratios.IsEmpty() ? TEXT("") : TEXT(" / "), Ratio);
					Chains += FString::Printf(TEXT("%s%d"), Chains.IsEmpty() ? TEXT("") : TEXT(" "), Hit.Chain);
				}
				// The last hit must have come inside the window - else the miss would prove nothing.
				const bool bInside = R.SinceLastHit >= 0.f && R.SinceLastHit < PinguTuning::SnowballComboWindow;
				bOk &= bInside;
				Report(FString::Printf(TEXT("RESULT snowball combo window: push x%s (want 1 / 1.35 / 1 / 1.35 / 1: the third hit came %.1f s after the second, past the "
					"%.1f s window, so it is a first again), chain counter %s (want 1 2 1 2 1); the last hit came %.2f s after the one before - inside the window %d, "
					"and still x1 because the miss in between broke the chain: %s"),
					*Ratios, WindowPlan[2].Wait + HitGap, PinguTuning::SnowballComboWindow, *Chains, R.SinceLastHit, bInside ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
				ResetTarget(R.DummyAt);
			});
	}

	// ---- snowball_IceCube: a frozen dummy's ice is not broken by a snowball; a slap still shatters it.
	{
		const TSharedRef<FIceRun> S = MakeShared<FIceRun>();
		AddStep(TEXT("snowball_IceCube"), 4.f,
			[this, S, Base]
			{
				*S = FIceRun();
				PrepareBrawl(110.f, { EPenguinAbility::Snowball, EPenguinAbility::FlipperSlap });
				S->DummyAt = Base + FVector(110.f, 0.f, 0.f);
				if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
				{
					Stage->SetActorTickEnabled(false);
					LookCamera = Stage;
				}
			},
			[this, S](float T)
			{
				FIceRun& R = *S;
				if (!Target.IsValid())
				{
					return;
				}
				UPenguinMovementComponent* TargetMove = Target->GetPenguinMovement();
				UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
				if (!TargetMove || !Abilities)
				{
					return;
				}
				if (AActor* Camera = LookCamera.Get())
				{
					// Between the two, from the stage camera's side.
					const FVector LookAt = (Penguin->GetFeetLocation() + R.DummyAt) * 0.5f + FVector(0.f, 0.f, 25.f);
					const FVector From = LookAt + FVector(0.f, 430.f, 60.f);
					Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
				const float Speed = FMath::Abs(static_cast<float>(TargetMove->Velocity.X));
				if (R.Stage >= 2)
				{
					R.bKnockedOut |= Target->IsKnockedOut();
				}
				if (R.Stage >= 2 && R.Stage < 6)
				{
					R.MaxSpeed = FMath::Max(R.MaxSpeed, Speed);
				}
				const auto ThrowBall = [this, &R, Abilities, TargetMove]()
				{
					R.HitsBefore = TargetMove->HitEvents;
					R.StartX = static_cast<float>(Target->GetActorLocation().X);
					Penguin->SetAimPoint(Target->GetAimOrigin());
					return Abilities->IsReady(0) && Penguin->ActivateAbility(0);
				};
				const auto Judge = [this, &R, Abilities, TargetMove](int32 Index)
				{
					R.bFrozen[Index] = TargetMove->IsFrozen();
					R.bIce[Index] = Target->IsIceBlockShown();
					R.Hits[Index] = TargetMove->HitEvents - R.HitsBefore;
					R.Moved[Index] = FMath::Abs(static_cast<float>(Target->GetActorLocation().X) - R.StartX);
					R.Cooldown[Index] = Abilities->GetCooldownRemaining(0);
					R.Chain[Index] = Abilities->GetSnowballComboHits();
				};
				switch (R.Stage)
				{
				case 0: // freeze the dummy (long: it must not thaw by itself during the step)
					if (T > 0.25f)
					{
						FPenguinHit Freeze;
						Freeze.Instigator = Penguin.Get();
						Freeze.Source = EPenguinAbility::FrostBreath;
						Freeze.Freeze = 8.f;
						Freeze.Stun = 0.f;
						Freeze.Location = Target->GetActorLocation();
						Target->ApplyPenguinHit(Freeze);
						R.bFrozenStart = TargetMove->IsFrozen();
						R.bIceStart = Target->IsIceBlockShown();
						R.Stage = 1;
						R.StageAt = T;
					}
					break;
				case 1: // the first snowball
					if (T > R.StageAt + 0.15f)
					{
						R.bThrown[0] = ThrowBall();
						R.Stage = 2;
						R.StageAt = T;
					}
					break;
				case 2:
					if (!R.bShotBounce && T > R.StageAt + 0.09f)
					{
						R.bShotBounce = true;
						Shot(TEXT("69_snowball_vs_ice"));
					}
					if (T > R.StageAt + 0.6f)
					{
						Judge(0);
						R.Stage = 3;
					}
					break;
				case 3: // the second one (the cooldown of the first - not given back - is 0.9 s)
					if (T > R.StageAt + 1.0f)
					{
						R.bThrown[1] = ThrowBall();
						R.Stage = 4;
						R.StageAt = T;
					}
					break;
				case 4:
					if (T > R.StageAt + 0.6f)
					{
						Judge(1);
						// A snowball hit made by hand lands on nothing either.
						FPenguinHit Hit;
						Hit.Instigator = Penguin.Get();
						Hit.Source = EPenguinAbility::Snowball;
						Hit.SourceSize = 1.f;
						Hit.Knockback = FVector2D(PinguTuning::SnowballPush, PinguTuning::SnowballLift);
						Hit.Stun = PinguTuning::SnowballStun;
						Hit.Location = Target->GetActorLocation();
						R.bHandLanded = Target->ApplyPenguinHit(Hit);
						R.Stage = 5;
						R.StageAt = T;
					}
					break;
				case 5: // put the (still frozen) dummy back in slap reach, calm, and slap it
					if (T > R.StageAt + 0.2f)
					{
						Target->SetActorLocation(R.DummyAt, false, nullptr, ETeleportType::TeleportPhysics);
						TargetMove->Velocity = FVector::ZeroVector;
						R.Stage = 6;
						R.StageAt = T;
					}
					break;
				case 6:
					if (T > R.StageAt + 0.15f)
					{
						R.HitsBefore = TargetMove->HitEvents;
						Penguin->SetAimPoint(Target->GetAimOrigin());
						R.bSlapUsed = Penguin->ActivateAbility(1);
						R.Stage = 7;
						R.StageAt = T;
					}
					break;
				case 7:
					if (!R.bShotSlap && (!TargetMove->IsFrozen() || T > R.StageAt + 0.3f)) // (the frame the ice bursts: its shards are still flying)
					{
						R.bShotSlap = true;
						Shot(TEXT("69_snowball_vs_ice_slap"));
					}
					if (T > R.StageAt + 0.45f)
					{
						R.bSlapThawed = !TargetMove->IsFrozen();
						R.bSlapIceGone = !Target->IsIceBlockShown();
						R.SlapHits = TargetMove->HitEvents - R.HitsBefore;
						R.SlapSpeed = Speed;
						R.bSlapStunned = TargetMove->IsStunned();
						R.Stage = 8;
					}
					break;
				default:
					break;
				}
			},
			[this, S]
			{
				const FIceRun& R = *S;
				if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
				{
					Stage->SetActorTickEnabled(true);
					Stage->SnapToTargets();
				}
				LookCamera.Reset();
				bool bBounceOk = R.bFrozenStart && R.bIceStart;
				for (int32 Index = 0; Index < 2; ++Index)
				{
					// Still frozen with its ice showing, no hit, a small nudge only, the snowball NOT given back (0.9 s - the time since
					// the throw is still running), no chain.
					bBounceOk &= R.bThrown[Index] && R.bFrozen[Index] && R.bIce[Index] && R.Hits[Index] == 0 && R.Moved[Index] > 5.f && R.Moved[Index] < 150.f
						&& R.Cooldown[Index] > 0.1f && R.Chain[Index] == 0;
				}
				bBounceOk &= !R.bKnockedOut && !R.bHandLanded && R.MaxSpeed < 600.f;
				const bool bSlapOk = R.bSlapUsed && R.bSlapThawed && R.bSlapIceGone && R.SlapHits == 0 && R.SlapSpeed < 60.f && !R.bSlapStunned;
				Report(FString::Printf(TEXT("RESULT snowball vs ice: frozen %d, ice showing %d; snowball 1 - thrown %d, still frozen %d, ice still there %d, hits taken %d, nudged %.0f cm, "
					"cooldown left %.2f s (not given back), chain %d; snowball 2 - thrown %d, still frozen %d, ice still there %d, hits %d, nudged %.0f cm, cooldown left %.2f s, chain %d; "
					"fastest the ice cube went %.0f cm/s (want under 600), knocked out %d, a snowball hit made by hand landed %d (want 0): %s"),
					R.bFrozenStart ? 1 : 0, R.bIceStart ? 1 : 0, R.bThrown[0] ? 1 : 0, R.bFrozen[0] ? 1 : 0, R.bIce[0] ? 1 : 0, R.Hits[0], R.Moved[0], R.Cooldown[0], R.Chain[0],
					R.bThrown[1] ? 1 : 0, R.bFrozen[1] ? 1 : 0, R.bIce[1] ? 1 : 0, R.Hits[1], R.Moved[1], R.Cooldown[1], R.Chain[1],
					R.MaxSpeed, R.bKnockedOut ? 1 : 0, R.bHandLanded ? 1 : 0, bBounceOk ? TEXT("ok") : TEXT("FAIL")));
				Report(FString::Printf(TEXT("RESULT snowball vs ice, then a slap: slap used %d, ice shattered (thawed %d, ice gone %d), hits taken %d, speed after %.0f cm/s, stunned %d "
					"(want shattered, no knock, no stun, like any ice-smashing hit): %s"),
					R.bSlapUsed ? 1 : 0, R.bSlapThawed ? 1 : 0, R.bSlapIceGone ? 1 : 0, R.SlapHits, R.SlapSpeed, R.bSlapStunned ? 1 : 0, bSlapOk ? TEXT("ok") : TEXT("FAIL")));
				if (Target.IsValid())
				{
					Target->ResetForRound(R.DummyAt);
				}
			});
	}

	AddSnowballLookStep(); // the pictures
}

void APenguinAutoTest::AddSnowballLookStep()
{
	using namespace PenguinAutoTestSnowballPrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	// look_SnowballCombo: seven hits in a row at the dummy from the stage camera's side, with the UI. The 2nd, 4th and 7th (the capped
	// one) get a picture while their "x" is popping at the dummy.
	const TSharedRef<FComboLook> S = MakeShared<FComboLook>();
	AddStep(TEXT("look_SnowballCombo"), 14.f,
		[this, S, Base]
		{
			*S = FComboLook();
			SpawnTarget(); // (a look-only run has no dummy yet)
			PrepareBrawl(ComboDistance, { EPenguinAbility::Snowball });
			S->DummyAt = Base + FVector(ComboDistance, 0.f, 0.f);
			S->PopStartsBase = SPinguOverheadAbilities::GetLastPaintStats().ComboPopStarts;
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, S](float T)
		{
			FComboLook& L = *S;
			if (!Target.IsValid() || !Penguin.IsValid())
			{
				return;
			}
			if (AActor* Camera = LookCamera.Get())
			{
				// The dummy up close and in the middle, room above it for the number (the thrower is out of the picture).
				const FVector LookAt = L.DummyAt + FVector(0.f, 0.f, 25.f);
				const FVector From = LookAt + FVector(0.f, 600.f, 20.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			const SPinguOverheadAbilities::FPaintStats& Stats = SPinguOverheadAbilities::GetLastPaintStats();
			UPenguinMovementComponent* TargetMove = Target->GetPenguinMovement();
			if (L.Index >= LookHits || !TargetMove)
			{
				return;
			}
			const int32 Wanted = L.Index + 1;
			switch (L.Stage)
			{
			case 0: // throw
				if (T >= L.NextAt && Penguin->GetAbilities()->IsReady(0))
				{
					ResetTarget(L.DummyAt);
					L.HitsBefore = TargetMove->HitEvents;
					Penguin->SetAimPoint(Target->GetAimOrigin());
					if (Penguin->ActivateAbility(0))
					{
						L.Stage = 1;
						L.ThrowAt = T;
					}
				}
				break;
			case 1: // in the air, then landed
				if (TargetMove->HitEvents != L.HitsBefore)
				{
					L.LandedAt = T;
					L.Stage = 2;
				}
				else if (T > L.ThrowAt + 1.f)
				{
					L.Stage = 3; // missed: the look is over
				}
				break;
			case 2:
			{
				// Back in its place a moment after the hit (the push sends it far); the picture is taken by the pop's age (the UI runs on
				// real time): the number settled and rising, the hit's burst still there.
				const bool bShotHit = Wanted == LookShotHits[0] || Wanted == LookShotHits[1] || Wanted == LookShotHits[2];
				if (T >= L.LandedAt + HitGap && Target->GetActorLocation().X > L.DummyAt.X + 5.f)
				{
					ResetTarget(L.DummyAt);
				}
				const bool bPopping = Stats.ComboPops > 0 && Stats.ComboPopAge >= 0.15f && Stats.ComboPopAge < 0.4f;
				if (bShotHit && (bPopping || T > L.LandedAt + 0.9f) && L.LastShotCount != Wanted)
				{
					L.LastShotCount = Wanted;
					Shot(*FString::Printf(TEXT("69_snowball_combo_%d"), Wanted), true);
					++L.Shots;
					L.ShotsWithPop += bPopping && Stats.LastComboCount == Wanted ? 1 : 0;
				}
				if (T >= L.LandedAt + 0.6f && (!bShotHit || L.LastShotCount == Wanted))
				{
					++L.Index;
					L.Stage = 0;
					L.NextAt = T;
				}
				break;
			}
			default:
				break;
			}
		},
		[this, S]
		{
			const FComboLook& L = *S;
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const SPinguOverheadAbilities::FPaintStats& Stats = SPinguOverheadAbilities::GetLastPaintStats();
			// The overhead widget started a pop for every hit from the second on (6 of 7), and the three pictures had one showing.
			const int32 Starts = Stats.ComboPopStarts - L.PopStartsBase;
			const bool bOk = L.Index >= LookHits && L.Shots == 3 && L.ShotsWithPop == 3 && Starts >= LookHits - 1;
			Report(FString::Printf(TEXT("RESULT snowball combo look: %d of %d hits, %d pictures (want 3), %d of them with the number popping, the overhead widget started %d pops "
				"(want %d): %s"), L.Index, LookHits, L.Shots, L.ShotsWithPop, Starts, LookHits - 1, bOk ? TEXT("ok") : TEXT("FAIL")));
			if (Target.IsValid())
			{
				Target->ResetForRound(L.DummyAt);
			}
		});
}
