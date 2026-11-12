// Pingu - the penguin faces where it aims while it charges and throws the icicle (UPenguinAbilityComponent::GetIcicleFacing,
// APenguinCharacter::GetVisualFacing): the autotest's look step (APenguinAutoTest::AddIcicleFacingStep, for looktest too).
//
// look_IcicleFacing, run A: the penguin runs right while it aims left, charges and throws. Its mesh must face left during the whole
// charge and the throw (the movement keeps running right: that is its own facing), the icicle in its flipper is on the left, the
// flipper the camera sees is Flipper_R, the thrown icicle starts on the left of the body, and after the follow-through the mesh turns
// back to where it runs. Run B: standing, aiming right, charging; the aim crosses over to the left (it turns round, quickly, with
// the icicle in its flipper never jumping across the body), then straight up (it keeps the side it has), the throw goes straight up,
// and afterwards it turns back. Screenshots 55_icicle_facing_*.

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace PenguinAutoTestIcicleFacingPrivate
{
	/** The mesh looks along +Y turned 56 degrees to the camera: its forward X is about 0.83 once it faces a side. This much counts as facing it. */
	constexpr float FacesSide = 0.5f;
	/** The mesh has this long (s) after the press, or after the aim crossed over, to turn round to the aim. */
	constexpr float TurnTimeMax = 0.25f;
	/** And this long after the follow-through to turn back to where the penguin moves (s). */
	constexpr float ReturnTimeMax = 0.6f;
	/** An icicle drawn at more than this share of its size counts as seen. */
	constexpr float HandVisibleScale = 0.3f;
	/** The icicle's spawn must be this far (cm) on the aim's side of the capsule's middle. */
	constexpr float SpawnSideMin = 20.f;
	/** The aim's distance from the penguin (cm). */
	constexpr float AimLength = 700.f;

	struct FFacingLook
	{
		int32 Phase = 0;
		FVector2D AimDir = FVector2D(-1.f, 0.f);
		int32 ThrownAtStart = 0;
		float PhaseAt = 0.f;
		// run A
		float PressA = 0.f;
		float TurnA = -1.f;
		int32 FramesA = 0;
		int32 BadFacingA = 0;
		int32 RunningRightA = 0;
		float HandTipSumA = 0.f;
		int32 HandFramesA = 0;
		int32 BadHandSideA = 0;
		bool bSpawnSeenA = false;
		float SpawnRelX = 0.f;
		float SpawnFwdX = 0.f;
		float ShowEndA = -1.f;
		float ReturnA = -1.f;
		float ReturnFwdA = 0.f;
		// run B
		float PressB = 0.f;
		float SwitchB = -1.f;
		float TurnB = -1.f;
		float UpAtB = -1.f;
		int32 FramesStayB = 0;
		int32 BadStayB = 0;
		int32 FramesUpB = 0;
		int32 BadUpB = 0;
		float ShowEndB = -1.f;
		float ReturnB = -1.f;
		float MinScaleTurn = 1.f;
		// both: the icicle in the flipper from one frame to the next
		bool bHandPrev = false;
		float HandPrevScale = 0.f;
		bool bHandPrevNearL = true;
		int32 HandSwitches = 0;
		int32 Shots = 0;
	};
}

void APenguinAutoTest::AddIcicleFacingStep()
{
	using namespace PenguinAutoTestIcicleFacingPrivate;
	const TSharedRef<FFacingLook> Facing = MakeShared<FFacingLook>();
	AddStep(TEXT("look_IcicleFacing"), 9.f,
		[this, Facing]
		{
			*Facing = FFacingLook();
			PrepareBrawl(2600.f, { EPenguinAbility::Icicle }); // (the dummy far to the right, out of the way of the throws to the left)
			Facing->ThrownAtStart = Penguin->GetAbilities()->GetIciclesThrown();
			VirtualInput = FVector2D(1.f, 0.f); // running right ...
			Facing->AimDir = FVector2D(-1.f, 0.f); // ... aiming left
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, Facing](float T)
		{
			FFacingLook& L = *Facing;
			UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Penguin->GetFeetLocation() + FVector(15.f, 0.f, 80.f);
				const FVector From = LookAt + FVector(10.f, 380.f, 25.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(L.AimDir.X * AimLength, 0.f, L.AimDir.Y * AimLength));
			const float FwdX = static_cast<float>(MeshForward(Penguin.Get()).X);
			const bool bShowing = Abilities->GetIcicleShow() != EPinguIcicleShow::None;
			const float Since = T - L.PhaseAt;

			// The icicle in the flipper, frame to frame: which flipper's hand it is nearer to must not change while it shows at more
			// than a third of its size (a jump across the body to the other flipper would show; it swaps at its smallest).
			FVector HandCenter = FVector::ZeroVector;
			FVector HandTip = FVector::ZeroVector;
			float HandScale = 0.f;
			const bool bHand = Abilities->GetHandIcicleState(HandCenter, HandTip, HandScale);
			const FVector HandL = Penguin->GetMesh()->GetSocketLocation(FName(TEXT("Hand_L")));
			const FVector HandR = Penguin->GetMesh()->GetSocketLocation(FName(TEXT("Hand_R")));
			const bool bNearL = FVector::Dist(HandCenter, HandL) < FVector::Dist(HandCenter, HandR);
			if (bHand && L.bHandPrev && L.HandPrevScale > HandVisibleScale && HandScale > HandVisibleScale && bNearL != L.bHandPrevNearL)
			{
				++L.HandSwitches;
				Report(FString::Printf(TEXT("icicle facing: the icicle jumped to the other flipper while it showed (phase %d, %.3f s in, size %.2f)"), L.Phase, Since, HandScale));
			}
			L.bHandPrev = bHand;
			L.HandPrevScale = HandScale;
			L.bHandPrevNearL = bNearL;

			switch (L.Phase)
			{
			case 0: // running right, aiming left: the key goes down
				if (T >= 0.6f)
				{
					Abilities->SetAbilities({ EPenguinAbility::Icicle }); // ready
					L.ThrownAtStart = Abilities->GetIciclesThrown();
					Penguin->SetAbilityKeyDown(0, true);
					L.PressA = T;
					L.PhaseAt = T;
					L.Phase = 1;
				}
				break;
			case 1: // charging (A)
			{
				++L.FramesA;
				if (L.TurnA < 0.f && FwdX < -FacesSide)
				{
					L.TurnA = Since;
				}
				if (Since >= TurnTimeMax + 0.05f)
				{
					// (the turn is over: from here the mesh must face the aim on every frame, the icicle points to the aim's side and is in
					// the flipper the camera sees - on its side, +Y)
					L.BadFacingA += FwdX < -FacesSide ? 0 : 1;
					if (bHand && HandScale > 0.9f)
					{
						L.HandTipSumA += static_cast<float>(HandTip.X);
						// (the flipper the camera sees is the one on the +Y side: the icicle is nearer its hand than the other's)
						const FVector& NearHand = HandL.Y > HandR.Y ? HandL : HandR;
						const FVector& FarHand = HandL.Y > HandR.Y ? HandR : HandL;
						L.BadHandSideA += FVector::Dist(HandCenter, NearHand) < FVector::Dist(HandCenter, FarHand) ? 0 : 1;
						++L.HandFramesA;
					}
				}
				// (the penguin's own facing is the way it runs: right)
				L.RunningRightA += (Penguin->GetFacing() > 0.f && Move->Velocity.X > 200.f) ? 1 : 0;
				if (L.Shots == 0 && Since >= 0.3f)
				{
					Shot(TEXT("55_icicle_facing_1")); // running right, charging to the left: facing left
					++L.Shots;
				}
				if (L.Shots == 1 && Since >= 0.75f)
				{
					Shot(TEXT("55_icicle_facing_2")); // drawn back, shaking
					++L.Shots;
				}
				if (Since >= 0.9f)
				{
					Penguin->SetAbilityKeyDown(0, false);
					L.PhaseAt = T;
					L.Phase = 2;
				}
				break;
			}
			case 2: // let go: the swing, the icicle leaves, the follow-through
			{
				if (bShowing)
				{
					L.BadFacingA += FwdX < -FacesSide ? 0 : 1;
				}
				if (!L.bSpawnSeenA && Abilities->GetIciclesThrown() > L.ThrownAtStart)
				{
					// Just thrown: our nearest flying icicle - where it started against the body's middle, and where the mesh looks.
					float Nearest = 1.e9f;
					for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
					{
						if (It->GetKind() == EPenguinProjectileKind::Icicle && It->GetShooter() == Penguin.Get() && It->IsFlying())
						{
							const float Dist = static_cast<float>(FVector::Dist(It->GetActorLocation(), Penguin->GetActorLocation()));
							if (Dist < Nearest)
							{
								Nearest = Dist;
								L.SpawnRelX = static_cast<float>(It->GetActorLocation().X - Penguin->GetActorLocation().X);
								L.SpawnFwdX = FwdX;
								L.bSpawnSeenA = true;
							}
						}
					}
				}
				if (L.Shots == 2 && Since >= 0.03f)
				{
					Shot(TEXT("55_icicle_facing_3")); // the swing (the picture comes a frame later)
					++L.Shots;
				}
				if (!bShowing && Abilities->GetIciclesThrown() > L.ThrownAtStart)
				{
					L.ShowEndA = T;
					L.PhaseAt = T;
					L.Phase = 3;
				}
				break;
			}
			case 3: // the follow-through is over: it turns back to where it runs (right)
				if (L.ReturnA < 0.f && FwdX > FacesSide)
				{
					L.ReturnA = Since;
					L.ReturnFwdA = FwdX;
				}
				if (L.Shots == 3 && Since >= 0.4f)
				{
					Shot(TEXT("55_icicle_facing_4")); // still running right: facing right again
					++L.Shots;
				}
				if (Since >= 0.8f)
				{
					// Run B: standing still, aiming right, ready again.
					VirtualInput = FVector2D::ZeroVector;
					L.AimDir = FVector2D(1.f, 0.f);
					L.PhaseAt = T;
					L.Phase = 4;
				}
				break;
			case 4: // standing, aiming right (the penguin faces right): the key goes down
				if (Since >= 0.6f)
				{
					Abilities->SetAbilities({ EPenguinAbility::Icicle });
					L.ThrownAtStart = Abilities->GetIciclesThrown();
					Penguin->SetAbilityKeyDown(0, true);
					L.PressB = T;
					L.PhaseAt = T;
					L.Phase = 5;
				}
				break;
			case 5: // charging (B): right for 0.45 s, then over to the left, then straight up
			{
				if (Since < 0.45f)
				{
					// (aimed right and facing right: it must not turn at all)
					if (Since >= 0.1f)
					{
						++L.FramesStayB;
						L.BadStayB += FwdX > FacesSide ? 0 : 1;
					}
				}
				else if (L.SwitchB < 0.f)
				{
					L.SwitchB = T;
					L.AimDir = FVector2D(-1.f, 0.f); // the aim crosses over
				}
				if (L.SwitchB >= 0.f)
				{
					const float SinceSwitch = T - L.SwitchB;
					if (L.TurnB < 0.f && FwdX < -FacesSide)
					{
						L.TurnB = SinceSwitch;
					}
					if (SinceSwitch < 0.5f)
					{
						L.MinScaleTurn = FMath::Min(L.MinScaleTurn, bHand ? HandScale : 1.f);
					}
					if (L.Shots == 4 && SinceSwitch >= 0.045f)
					{
						Shot(TEXT("55_icicle_facing_turn")); // half way round: the body faces the camera, the icicle shrunk away
						++L.Shots;
					}
					if (L.Shots == 5 && SinceSwitch >= 0.45f)
					{
						Shot(TEXT("55_icicle_facing_5")); // turned round to the left
						++L.Shots;
					}
					if (SinceSwitch >= 0.5f && L.UpAtB < 0.f)
					{
						L.UpAtB = T;
						L.AimDir = FVector2D(0.f, 1.f); // straight up: it keeps the side it has
					}
				}
				if (L.UpAtB >= 0.f && T - L.UpAtB >= 0.1f)
				{
					++L.FramesUpB;
					L.BadUpB += FwdX < -FacesSide ? 0 : 1;
				}
				if (L.UpAtB >= 0.f && T - L.UpAtB >= 0.4f)
				{
					Penguin->SetAbilityKeyDown(0, false);
					L.PhaseAt = T;
					L.Phase = 6;
				}
				break;
			}
			case 6: // thrown straight up; turned back afterwards (the penguin still faces right as it moves)
				if (!bShowing && Abilities->GetIciclesThrown() > L.ThrownAtStart)
				{
					L.ShowEndB = T;
					L.PhaseAt = T;
					L.Phase = 7;
				}
				break;
			case 7:
				if (L.ReturnB < 0.f && FwdX > FacesSide)
				{
					L.ReturnB = Since;
				}
				if (Since >= 0.6f)
				{
					L.Phase = 8;
					StepTime = 1000.f; // done (the pictures are written by now)
				}
				break;
			default:
				break;
			}
		},
		[this, Facing]
		{
			const FFacingLook& L = *Facing;
			VirtualInput = FVector2D::ZeroVector;
			Penguin->SetAbilityKeyDown(0, false);
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			const float HandTipAverage = L.HandFramesA > 0 ? L.HandTipSumA / L.HandFramesA : 0.f;
			const bool bRunOk = L.Phase >= 3 && L.TurnA >= 0.f && L.TurnA < TurnTimeMax && L.BadFacingA == 0 && L.FramesA > 20 && L.RunningRightA > 20
				&& L.HandFramesA > 10 && L.BadHandSideA == 0 && HandTipAverage < -FacesSide && L.bSpawnSeenA && L.SpawnRelX < -SpawnSideMin && L.SpawnFwdX < -FacesSide
				&& L.ReturnA >= 0.f && L.ReturnA < ReturnTimeMax && L.ReturnFwdA > FacesSide;
			Report(FString::Printf(TEXT("RESULT icicle facing: running right (on %d of %d frames) aiming left, the mesh turned to the aim in %.3f s (want under %.2f) and faced it on all but %d frames (want 0), "
				"the icicle in the flipper pointed left (tip x %.2f on average, want under -%.1f) from the flipper the camera sees on all but %d of %d frames (want 0), "
				"the thrown icicle started %.0f cm left of the capsule's middle with the mesh facing %.2f (want left of -%.0f, facing under -%.1f), "
				"turned back to where it runs %.3f s after the follow-through (want under %.1f) %s"),
				L.RunningRightA, L.FramesA, L.TurnA, TurnTimeMax, L.BadFacingA, HandTipAverage, FacesSide, L.BadHandSideA, L.HandFramesA, L.SpawnRelX, L.SpawnFwdX, SpawnSideMin,
				FacesSide, L.ReturnA, ReturnTimeMax, bRunOk ? TEXT("ok") : TEXT("FAIL")));
			const bool bTurnOk = L.Phase >= 7 && L.BadStayB == 0 && L.FramesStayB > 10 && L.TurnB >= 0.f && L.TurnB < TurnTimeMax && L.BadUpB == 0 && L.FramesUpB > 10
				&& L.ReturnB >= 0.f && L.ReturnB < ReturnTimeMax && L.HandSwitches == 0 && L.MinScaleTurn < HandVisibleScale;
			Report(FString::Printf(TEXT("RESULT icicle facing turn: aimed right and facing right the mesh never turned (bad frames %d of %d, want 0), the aim crossed over to the left: turned round in %.3f s (want under %.2f), "
				"then straight up it kept its side (bad frames %d of %d, want 0), turned back %.3f s after the follow-through (want under %.1f), the icicle in the flipper shrank to %.2f of its size on the way round "
				"(want under %.1f) and changed flipper while it showed %d times (want 0) %s"),
				L.BadStayB, L.FramesStayB, L.TurnB, TurnTimeMax, L.BadUpB, L.FramesUpB, L.ReturnB, ReturnTimeMax, L.MinScaleTurn, HandVisibleScale, L.HandSwitches,
				bTurnOk ? TEXT("ok") : TEXT("FAIL")));
			Penguin->ResetForRound(Penguin->GetActorLocation());
		});
}
