// Pingu - Jester's felt cap (PinguJesterHat, put on by PenguinTraitLooks.cpp): the autotest's look step look_JesterHat
// (APenguinAutoTest::AddJesterHatStep, for looktest too).
//
// Our penguin gets Jester (no reveal: the look goes on at once) and is looked at on High (the static cap), Ultra (the cap
// on spring bones) and Low (static, the smallest LOD): close-ups from the front and the side, then on Ultra a run, a jump,
// the landing and a turn filmed from the side - the bells must swing against the head (and on High they must not: the
// static cap has no bones). Also checked: the beanie hidden, the cap sitting on the head where it was modelled (its origin
// against the eyes and the Body bone) - grown and shrunk by size rays too, the cap drawing the LOD the penguin draws, and
// its ink outline staying hidden.
// Shots: Saved/ClaudeLogs/Shots/67_jester_hat_*.png.

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinTraitComponent.h"
#include "PenguinTraitLooks.h"
#include "PinguJesterHat.h"
#include "PinguSettings.h"
#include "PinguTraitTable.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "ShaderCompiler.h"

namespace PenguinAutoTestJesterPrivate
{
	/** Longest wait for a preset's shaders, seconds. */
	constexpr float ShaderWaitMax = 30.f;
	/** A preset's change settles this long (the look is built again for it) before its pictures. */
	constexpr float SettleTime = 0.7f;
	/** Ultra: in the run, the jump and the landing the bells must swing at least this far against the head (cm); standing,
	 *  they hang at most this far from their rest place (gravity sags the soft points a little). */
	constexpr float WantBellMove = 5.f;
	constexpr float MostRestSag = 10.f;
	/** Where the cap's origin is against the eyes and the Body bone (Pingu.blend's rest pose: the pivot (0, 0, 1.6), the
	 *  eye bones (+-0.203, -0.418, 1.514), the Body bone (0, 0, 0.45)): its distances to them in eye spans (0.406 apart) -
	 *  the same however the penguin leans or turns and whatever its size. */
	constexpr float WantToEye = 0.4726f / 0.406f;
	constexpr float WantToBody = 1.15f / 0.406f;
	constexpr float FitSlack = 0.05f;

	struct FFit
	{
		bool bValid = false;
		float ToEyeL = 0.f;
		float ToEyeR = 0.f;
		float ToBody = 0.f;
		bool bAbove = false;

		float Worst() const
		{
			return bValid ? FMath::Max3(FMath::Abs(ToEyeL - WantToEye), FMath::Abs(ToEyeR - WantToEye), FMath::Abs(ToBody - WantToBody) * 0.5f) : 99.f;
		}
		bool IsOk() const { return bValid && bAbove && Worst() < FitSlack; }
	};

	static FFit MeasureFit(const APenguinCharacter* Who, const PinguJesterHat::FStats& Stats)
	{
		FFit Fit;
		const USkeletalMeshComponent* SkelMesh = Who ? Who->GetMesh() : nullptr;
		const UPrimitiveComponent* Hat = Stats.Component.Get();
		const FName EyeL(TEXT("Eye_L"));
		const FName EyeR(TEXT("Eye_R"));
		const FName Body(TEXT("Body"));
		if (!SkelMesh || !Hat || SkelMesh->GetBoneIndex(EyeL) == INDEX_NONE || SkelMesh->GetBoneIndex(EyeR) == INDEX_NONE
			|| SkelMesh->GetBoneIndex(Body) == INDEX_NONE)
		{
			return Fit;
		}
		const FVector L = SkelMesh->GetSocketLocation(EyeL);
		const FVector R = SkelMesh->GetSocketLocation(EyeR);
		const float Span = static_cast<float>(FVector::Dist(L, R));
		if (Span < 1.f)
		{
			return Fit;
		}
		const FVector Origin = Hat->GetComponentLocation();
		Fit.ToEyeL = static_cast<float>(FVector::Dist(Origin, L)) / Span;
		Fit.ToEyeR = static_cast<float>(FVector::Dist(Origin, R)) / Span;
		Fit.ToBody = static_cast<float>(FVector::Dist(Origin, SkelMesh->GetSocketLocation(Body))) / Span;
		Fit.bAbove = Origin.Z > (L.Z + R.Z) * 0.5;
		Fit.bValid = true;
		return Fit;
	}

	/** What the step saw of the cap on one preset. */
	struct FSeen
	{
		bool bPresent = false;
		bool bSkeletal = false;
		bool bShown = false;
		FString Mesh;
		int32 LOD = -1;
		int32 LODs = 0;
		int32 PenguinLOD = -1;
		int32 Chains = 0;
		int32 Bones = 0;
		int32 BeanieHidden = 0;
		bool bOutline = false;
		/** Ultra: the furthest a bell hangs from its rest place while standing (cm: gravity sags it a little). */
		float RestBells = 0.f;
		FFit Fit;

		bool LODOk() const { return LOD >= 0 && LOD == FMath::Clamp(PenguinLOD, 0, FMath::Max(LODs - 1, 0)); }
		FString Describe() const
		{
			return FString::Printf(TEXT("%s %s (LOD %d of %d, the penguin's %d)%s, beanie hidden %d sections, outline hidden %d, on the head %.3f off"),
				bPresent ? (bSkeletal ? TEXT("spring bones") : TEXT("static")) : TEXT("no cap"), *Mesh, LOD, LODs, PenguinLOD,
				bSkeletal ? *FString::Printf(TEXT(", %d points, %d bones, standing the bells %.1f cm from rest"), Chains, Bones, RestBells) : TEXT(""),
				BeanieHidden, bOutline ? 1 : 0, Fit.Worst());
		}
	};

	/** The cap's ink outline is hidden: it has an outline slot, the skeletal one has every outline section hidden (all LODs),
	 *  the static one wears the material that draws nothing. */
	static bool OutlineHidden(const PinguJesterHat::FStats& Stats)
	{
		return Stats.OutlineSlots > 0 && !Stats.bOutlineShown && (!Stats.bSkeletal || (Stats.OutlineSections > 0 && Stats.OutlineHidden == Stats.OutlineSections));
	}

	static FSeen Look(const APenguinCharacter* Who)
	{
		FSeen Seen;
		PinguJesterHat::FStats Stats;
		Seen.bPresent = PinguTraitLooks::GetJesterHatStats(Who, Stats, &Seen.BeanieHidden);
		Seen.bSkeletal = Stats.bSkeletal;
		Seen.bShown = Stats.bShown;
		Seen.Mesh = Stats.Mesh;
		Seen.LOD = Stats.LOD;
		Seen.LODs = Stats.LODs;
		Seen.Chains = Stats.Chains;
		Seen.Bones = Stats.Bones;
		Seen.bOutline = OutlineHidden(Stats);
		Seen.RestBells = FMath::Max3(Stats.BellOffset[0], Stats.BellOffset[1], Stats.BellOffset[2]);
		Seen.PenguinLOD = Who && Who->GetMesh() ? Who->GetMesh()->GetPredictedLODLevel() : -1;
		Seen.Fit = MeasureFit(Who, Stats);
		return Seen;
	}
}

void APenguinAutoTest::AddJesterHatStep()
{
	using namespace PenguinAutoTestJesterPrivate;
	struct FHatLook
	{
		bool bPresetSet = false;
		FPinguGraphicsOptions Before;
		int32 Stage = 0;
		float At = 0.f;
		int32 Shots = 0;
		FSeen High;
		FSeen Ultra;
		FSeen Low;
		/** Ultra with a grow ray (x1.82) and then shrunk (to x0.6): the cap still on the head. */
		FSeen Grown;
		FSeen Shrunk;
		float GrownSize = 0.f;
		float ShrunkSize = 0.f;
		/** High: the static cap in the run and jump - its place on the head never changes, no bell moves. */
		FTransform HighPlace = FTransform::Identity;
		float HighPlaceMoved = 0.f;
		float HighBellMove = 0.f;
		/** Ultra: the bells against the head - the most in the run, jump and landing, then the most up to the end of the turn. */
		float UltraRunJumpLand = 0.f;
		float UltraTurn = 0.f;
		float LandBells[3] = { 0.f, 0.f, 0.f };
		bool bAir = false;
		bool bLanded = false;
		float PeakZ = -1.e9f;
		float JumpAt = -1.f;
		/** The outline on Ultra (its outline sections hidden, still hidden later) and on Low (the static cap's outline slot hidden). */
		int32 UltraOutlineSections = 0;
		int32 UltraOutlineHidden = 0;
		bool bUltraOutlineBack = false;
		bool bLowOutlineOff = false;
		bool bLowOutlineBack = false;
	};
	const TSharedRef<FHatLook> L = MakeShared<FHatLook>();
	AddStep(TEXT("look_JesterHat"), 23.f + 3.f * ShaderWaitMax,
		[this, L]
		{
			*L = FHatLook();
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				L->Before = Settings->GetGraphics();
				L->bPresetSet = true;
				Settings->ApplyPreset(EPinguGraphicsPreset::High);
				FPinguGraphicsOptions Options = Settings->GetGraphics();
				Settings->SetGraphics(Options);
				Settings->ApplyGraphics(GetWorld());
			}
			SpawnTarget();
			PrepareBrawl(2400.f, {});
			if (UPenguinTraitComponent* Traits = Penguin->GetTraits())
			{
				Traits->SetTrait(EPinguTrait::Jester, -1.f);
			}
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, L](float T)
		{
			FHatLook& S = *L;
			UPinguSettings* Settings = UPinguSettings::Get();
			if (!Settings || !Penguin.IsValid())
			{
				StepTime = 1000.f;
				return;
			}
			const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
			const auto Ready = [&S, T, bCompiling]() { return T > S.At + SettleTime && (!bCompiling || T > S.At + ShaderWaitMax); };
			const auto Next = [&S, T](int32 Stage) { S.Stage = Stage; S.At = T; };
			const auto Camera = [this](const FVector& Offset)
			{
				if (AActor* Cam = LookCamera.Get())
				{
					const float Size = FMath::Max(Penguin->GetSizeScale(), 1.f);
					const FVector LookAt = Penguin->GetFeetLocation() + FVector(0.f, 0.f, 70.f * Size);
					const FVector From = LookAt + Offset * Size;
					Cam->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
				}
			};
			const FVector Front(330.f, 300.f, 30.f);
			const FVector Side(0.f, 470.f, 50.f);
			const FVector Follow(-60.f, 600.f, 70.f);
			const auto HatStats = [this]()
			{
				PinguJesterHat::FStats Stats;
				PinguTraitLooks::GetJesterHatStats(Penguin.Get(), Stats);
				return Stats;
			};
			switch (S.Stage)
			{
			case 0: // High: the static cap, from the front
				Camera(Front);
				if (Ready())
				{
					S.High = Look(Penguin.Get());
					Shot(TEXT("67_jester_hat_high_front"));
					++S.Shots;
					Next(1);
				}
				break;
			case 1: // ... and from the side
				Camera(Side);
				if (T > S.At + 0.25f)
				{
					Shot(TEXT("67_jester_hat_high_side"));
					++S.Shots;
					if (const UPrimitiveComponent* Hat = HatStats().Component.Get())
					{
						S.HighPlace = Hat->GetRelativeTransform();
					}
					VirtualInput = FVector2D(1.f, 0.f);
					Next(2);
				}
				break;
			case 2: // High: a run and a jump - the static cap never moves on the head
			{
				Camera(Follow);
				const PinguJesterHat::FStats Stats = HatStats();
				if (const UPrimitiveComponent* Hat = Stats.Component.Get())
				{
					S.HighPlaceMoved = FMath::Max(S.HighPlaceMoved, static_cast<float>(FVector::Dist(Hat->GetRelativeLocation(), S.HighPlace.GetLocation())));
				}
				S.HighBellMove = FMath::Max(S.HighBellMove, Stats.MostBellMove);
				if (T > S.At + 0.4f && T < S.At + 0.45f)
				{
					Move->JumpPressed();
				}
				if (T > S.At + 0.9f)
				{
					Move->JumpReleased();
				}
				if (T > S.At + 1.8f)
				{
					VirtualInput = FVector2D::ZeroVector;
					Move->JumpReleased();
					// Ultra: the cap on spring bones
					Settings->ApplyPreset(EPinguGraphicsPreset::Ultra);
					Next(3);
				}
				break;
			}
			case 3: // back to the start; Ultra's look is built again
				if (T > S.At + 0.1f && S.At >= 0.f)
				{
					PrepareBrawl(2400.f, {});
					if (UPenguinTraitComponent* Traits = Penguin->GetTraits(); Traits && Traits->GetTrait() != EPinguTrait::Jester)
					{
						Traits->SetTrait(EPinguTrait::Jester, -1.f);
					}
					Next(4);
				}
				break;
			case 4: // Ultra, from the front
				Camera(Front);
				if (Ready() && T > S.At + 1.2f) // (the points come to rest)
				{
					S.Ultra = Look(Penguin.Get());
					Shot(TEXT("67_jester_hat_ultra_front"));
					++S.Shots;
					Next(5);
				}
				break;
			case 5: // ... and from the side, standing
				Camera(Side);
				if (T > S.At + 0.25f)
				{
					Shot(TEXT("67_jester_hat_ultra_side"));
					++S.Shots;
					if (UPinguJesterHatComponent* Spring = Cast<UPinguJesterHatComponent>(HatStats().Component.Get()))
					{
						Spring->ResetSpringStats();
					}
					VirtualInput = FVector2D(1.f, 0.f);
					S.bAir = false;
					S.bLanded = false;
					S.PeakZ = -1.e9f;
					S.JumpAt = -1.f;
					Next(6);
				}
				break;
			case 6: // Ultra: run, jump, land (filmed from the side, following)
			{
				Camera(Follow);
				const float Since = T - S.At;
				if (Since > 0.45f && S.Shots == 4)
				{
					Shot(TEXT("67_jester_hat_run"));
					++S.Shots;
				}
				if (Since > 0.55f && S.JumpAt < 0.f)
				{
					Move->JumpPressed();
					S.JumpAt = T;
				}
				if (S.JumpAt >= 0.f)
				{
					const bool bAir = Move->IsFalling();
					const float Z = static_cast<float>(Penguin->GetActorLocation().Z);
					if (bAir)
					{
						S.bAir = true;
						if (Z >= S.PeakZ)
						{
							S.PeakZ = Z;
						}
						else if (S.Shots == 5 && Move->Velocity.Z < 0.f)
						{
							Shot(TEXT("67_jester_hat_jump")); // just past the top
							++S.Shots;
						}
					}
					if (T > S.JumpAt + 0.5f)
					{
						Move->JumpReleased();
					}
					if (S.bAir && !bAir && !S.bLanded)
					{
						S.bLanded = true;
						VirtualInput = FVector2D::ZeroVector; // it stops on landing: the points fly on
						Next(7);
					}
				}
				if (Since > 3.f)
				{
					Next(7); // (never landed: the check fails below)
				}
				break;
			}
			case 7: // the landing
				Camera(Follow);
				if (T > S.At + 0.07f && S.Shots == 6)
				{
					Shot(TEXT("67_jester_hat_land"));
					++S.Shots;
					const PinguJesterHat::FStats Stats = HatStats();
					for (int32 Chain = 0; Chain < 3; ++Chain)
					{
						S.LandBells[Chain] = Stats.BellOffset[Chain];
					}
				}
				if (T > S.At + 0.35f)
				{
					S.UltraRunJumpLand = HatStats().MostBellMove;
					VirtualInput = FVector2D(-1.f, 0.f); // a turn
					Next(8);
				}
				break;
			case 8: // the turn
				Camera(Follow);
				if (T > S.At + 0.16f && S.Shots == 7)
				{
					Shot(TEXT("67_jester_hat_turn"));
					++S.Shots;
				}
				if (T > S.At + 0.5f)
				{
					VirtualInput = FVector2D::ZeroVector;
					S.UltraTurn = HatStats().MostBellMove;
					Next(9);
				}
				break;
			case 9: // Ultra: the cap's outline sections are hidden
				Camera(Side);
				if (T > S.At + 0.5f)
				{
					const PinguJesterHat::FStats Stats = HatStats();
					S.UltraOutlineSections = Stats.OutlineSections;
					S.UltraOutlineHidden = Stats.OutlineHidden;
					Shot(TEXT("67_jester_hat_ultra_outline_off"));
					++S.Shots;
					Next(30);
				}
				break;
			case 30: // (the picture is written first)
				if (T > S.At + 0.3f)
				{
					Next(10);
				}
				break;
			case 10:
				if (T > S.At + 0.3f)
				{
					S.bUltraOutlineBack = OutlineHidden(HatStats());
					Penguin->ChangeSize(1.82f, 30.f); // a grow ray's step and a half
					Next(20);
				}
				break;
			case 20: // grown: the cap grows with the head (the picture once the ray's ring and sparkles are gone)
				Camera(Front);
				if (T > S.At + 2.8f)
				{
					S.Grown = Look(Penguin.Get());
					S.GrownSize = Penguin->GetSizeScale();
					Shot(TEXT("67_jester_hat_grown"));
					++S.Shots;
					Penguin->ChangeSize(0.6f / FMath::Max(Penguin->GetSizeTarget(), 0.1f), 30.f);
					Next(21);
				}
				break;
			case 21: // shrunk
				Camera(Side);
				if (T > S.At + 2.8f)
				{
					S.Shrunk = Look(Penguin.Get());
					S.ShrunkSize = Penguin->GetSizeScale();
					Shot(TEXT("67_jester_hat_shrunk"));
					++S.Shots;
					Next(22);
				}
				break;
			case 22: // (the picture is written first) normal size again; Low
				if (T > S.At + 0.3f)
				{
					PrepareBrawl(2400.f, {});
					if (UPenguinTraitComponent* Traits = Penguin->GetTraits(); Traits && Traits->GetTrait() != EPinguTrait::Jester)
					{
						Traits->SetTrait(EPinguTrait::Jester, -1.f);
					}
					Settings->ApplyPreset(EPinguGraphicsPreset::Low);
					Next(11);
				}
				break;
			case 11: // Low: static, the smallest LOD; its outline hidden too
				Camera(Side);
				if (Ready())
				{
					S.Low = Look(Penguin.Get());
					Next(12);
				}
				break;
			case 12:
				Camera(Side);
				if (T > S.At + 0.4f)
				{
					S.bLowOutlineOff = OutlineHidden(HatStats());
					Next(13);
				}
				break;
			case 13:
				Camera(Side);
				if (T > S.At + 0.4f)
				{
					S.bLowOutlineBack = OutlineHidden(HatStats());
					Shot(TEXT("67_jester_hat_low"));
					++S.Shots;
					Next(14);
				}
				break;
			case 14:
				if (T > S.At + 0.3f)
				{
					StepTime = 1000.f; // done (the picture is written by now)
				}
				break;
			default:
				break;
			}
		},
		[this, L]
		{
			FHatLook& S = *L;
			VirtualInput = FVector2D::ZeroVector;
			Move->JumpReleased();
			if (UPenguinTraitComponent* Traits = Penguin.IsValid() ? Penguin->GetTraits() : nullptr)
			{
				Traits->SetTrait(EPinguTrait::None, -1.f);
			}
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
				Settings->ApplyGraphics(GetWorld());
			}
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			PrepareBrawl(2400.f, {});

			const bool bHigh = S.High.bPresent && !S.High.bSkeletal && S.High.bShown && S.High.LODOk() && S.High.BeanieHidden > 0 && S.High.Fit.IsOk()
				&& S.High.bOutline && S.HighPlaceMoved < 0.5f && S.HighBellMove <= 0.f;
			const bool bUltra = S.Ultra.bPresent && S.Ultra.bSkeletal && S.Ultra.bShown && S.Ultra.Chains == 3 && S.Ultra.LODOk() && S.Ultra.BeanieHidden > 0
				&& S.Ultra.Fit.IsOk() && S.Ultra.bOutline && S.Ultra.RestBells <= MostRestSag && S.bLanded && S.UltraRunJumpLand >= WantBellMove;
			const bool bLow = S.Low.bPresent && !S.Low.bSkeletal && S.Low.LODOk() && S.Low.BeanieHidden > 0 && S.Low.Fit.IsOk();
			const bool bOutline = S.UltraOutlineSections > 0 && S.UltraOutlineHidden == S.UltraOutlineSections && S.bUltraOutlineBack && S.bLowOutlineOff && S.bLowOutlineBack;
			const bool bSize = S.Grown.bPresent && S.Grown.Fit.IsOk() && S.Grown.LODOk() && S.GrownSize > 1.6f
				&& S.Shrunk.bPresent && S.Shrunk.Fit.IsOk() && S.Shrunk.LODOk() && S.ShrunkSize < 0.7f;
			const bool bOk = bHigh && bUltra && bLow && bOutline && bSize && S.Shots == 12;
			Report(FString::Printf(TEXT("RESULT jester hat: High %s; Ultra %s; Low %s; High in a run and jump: moved on the head %.2f cm, bells %.1f cm (want 0 0); "
				"Ultra: the bells swung up to %.1f cm against the head in the run / jump / landing (want %.0f or more; landed %d; at the landing %.1f / %.1f / %.1f), "
				"%.1f with the turn; outline: %d of %d outline sections hidden on Ultra, still hidden later %d, Low's hidden %d, still hidden %d; "
				"size rays: grown x%.2f (on the head %.3f off, LOD %d / the penguin's %d), shrunk x%.2f (%.3f off, LOD %d / %d); shots %d (want 12): %s"),
				*S.High.Describe(), *S.Ultra.Describe(), *S.Low.Describe(), S.HighPlaceMoved, S.HighBellMove,
				S.UltraRunJumpLand, WantBellMove, S.bLanded ? 1 : 0, S.LandBells[0], S.LandBells[1], S.LandBells[2], S.UltraTurn,
				S.UltraOutlineHidden, S.UltraOutlineSections, S.bUltraOutlineBack ? 1 : 0, S.bLowOutlineOff ? 1 : 0, S.bLowOutlineBack ? 1 : 0,
				S.GrownSize, S.Grown.Fit.Worst(), S.Grown.LOD, S.Grown.PenguinLOD, S.ShrunkSize, S.Shrunk.Fit.Worst(), S.Shrunk.LOD, S.Shrunk.PenguinLOD,
				S.Shots, bOk ? TEXT("ok") : TEXT("FAIL")));
			Report(FString::Printf(TEXT("jester hat on the head (distances in eye spans, want %.3f / %.3f / %.3f to the left eye / right eye / Body bone): High %.3f / %.3f / %.3f, Ultra %.3f / %.3f / %.3f, Low %.3f / %.3f / %.3f"),
				WantToEye, WantToEye, WantToBody, S.High.Fit.ToEyeL, S.High.Fit.ToEyeR, S.High.Fit.ToBody, S.Ultra.Fit.ToEyeL, S.Ultra.Fit.ToEyeR, S.Ultra.Fit.ToBody,
				S.Low.Fit.ToEyeL, S.Low.Fit.ToEyeR, S.Low.Fit.ToBody));
		});
}
