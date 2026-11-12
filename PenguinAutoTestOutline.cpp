// Pingu - no ink outline any more (APenguinCharacter::ApplyOutlineSetting always hides it): the autotest's look step
// (APenguinAutoTest::AddNoOutlineStep, for looktest too).

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinTraitComponent.h"
#include "PinguSettings.h"
#include "PinguTraitTable.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ShaderCompiler.h"

namespace PenguinAutoTestOutlinePrivate
{
	/** Longest wait for the shaders of a new material (the horn's hiding material on High, the Low materials), seconds. */
	constexpr float ShaderWaitMax = 30.f;
	/** The picture is taken this long after a change (the render state is rebuilt by then), and the horn this long after
	 *  the noot (it is out from about 0.1 s to 0.5 s). */
	constexpr float SettleTime = 0.4f;
	constexpr float HornAfter = 0.14f;
	constexpr float HornGoneAfter = 0.8f;

	/** How many LODs of the penguin have all of their outline sections in the wanted state (shown / hidden), counting only
	 *  LODs that have an outline section at all; OutLODs: the LODs of the mesh. */
	static int32 LODsWithOutline(const APenguinCharacter* Who, bool bWantShown, int32& OutLODs)
	{
		TArray<FPenguinOutlineLODInfo> Info;
		if (Who)
		{
			Who->GetOutlineInfo(Info);
		}
		OutLODs = Info.Num();
		int32 Right = 0;
		for (const FPenguinOutlineLODInfo& LOD : Info)
		{
			Right += LOD.Sections > 0 && (bWantShown ? LOD.Hidden == 0 : LOD.Hidden == LOD.Sections) ? 1 : 0;
		}
		return Right;
	}

	/** What the outline slot draws with: the parent of a dynamic instance (a trait's M_Outline_Clip), else the material itself. */
	static FString OutlineSlotMaterial(const APenguinCharacter* Who)
	{
		const USkeletalMeshComponent* SkelMesh = Who ? Who->GetMesh() : nullptr;
		if (!SkelMesh)
		{
			return TEXT("none");
		}
		const TArray<FName> Names = SkelMesh->GetMaterialSlotNames();
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Names[Index].ToString().Contains(TEXT("outline"), ESearchCase::IgnoreCase))
			{
				const UMaterialInterface* Material = SkelMesh->GetMaterial(Index);
				if (const UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Material); Dynamic && Dynamic->Parent)
				{
					return Dynamic->Parent->GetName();
				}
				return Material ? Material->GetName() : TEXT("none");
			}
		}
		return TEXT("no slot");
	}

	static bool IsClipMaterial(const FString& Name)
	{
		return Name.Contains(TEXT("Outline_Clip"));
	}
}

void APenguinAutoTest::AddNoOutlineStep()
{
	using namespace PenguinAutoTestOutlinePrivate;
	// The black ink outline is gone for good: our penguin up close from the stage camera's side on High (63_no_outline_high),
	// with the noot horn out (63_no_outline_horn), a penguin spawned later, Speedster's clipped outline material (63_no_outline_trait)
	// and the Low preset (63_no_outline_low). Checked: every LOD's outline section hidden, the horn's outline hidden, the
	// triangles drawn, the clip material still on the slot, and the preset left as it was (a test run saves nothing).
	struct FNoOutline
	{
		bool bPresetSet = false;
		FPinguGraphicsOptions Before;
		int32 Stage = 0;
		float At = 0.f;
		int32 Shots = 0;
		int32 LODs = 0;
		int32 HighRight = 0;
		bool bHornHas = false;
		bool bHornHidden = false;
		bool bHornOut = false;
		bool bHornHiddenOut = false;
		bool bDashing = false;
		int32 SpawnRight = 0;
		bool bSpawnHornHidden = false;
		FString TraitMaterial;
		int32 TraitRight = 0;
		int32 LowRight = 0;
		bool bLowHornHidden = false;
		int32 LowLod = -1;
		TArray<FPenguinOutlineLODInfo> Triangles;
		TWeakObjectPtr<APenguinCharacter> Extra;
	};
	const TSharedRef<FNoOutline> Look = MakeShared<FNoOutline>();
	AddStep(TEXT("look_NoOutline"), 12.f + 2.f * ShaderWaitMax,
		[this, Look]
		{
			*Look = FNoOutline();
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				Look->Before = Settings->GetGraphics();
				Look->bPresetSet = true;
				Settings->ApplyPreset(EPinguGraphicsPreset::High);
			}
			PrepareBrawl(2400.f, { EPenguinAbility::Noot });
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, Look](float T)
		{
			FNoOutline& L = *Look;
			UPinguSettings* Settings = UPinguSettings::Get();
			if (!Settings || !Penguin.IsValid())
			{
				StepTime = 1000.f;
				return;
			}
			if (AActor* Camera = LookCamera.Get())
			{
				const FVector LookAt = Penguin->GetFeetLocation() + FVector(20.f, 0.f, 52.f);
				const FVector From = LookAt + FVector(15.f, 430.f, 25.f);
				Camera->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
			}
			const auto Compiling = []() { return GShaderCompilingManager && GShaderCompilingManager->IsCompiling(); };
			const auto Noot = [this]()
			{
				Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Noot }); // ready again
				Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(600.f, 0.f, 0.f));
				Penguin->ActivateAbility(0);
			};
			const auto HornOut = [this]()
			{
				const UStaticMeshComponent* Horn = Penguin->GetNootHorn();
				return Horn && Horn->ShouldRender() && Penguin->IsHonking();
			};
			switch (L.Stage)
			{
			case 0: // High (the horn's hiding material compiles first)
				if (T > 0.3f && (!Compiling() || T > ShaderWaitMax))
				{
					L.Stage = 1;
					L.At = T;
				}
				break;
			case 1:
				if (T > L.At + SettleTime)
				{
					L.HighRight = LODsWithOutline(Penguin.Get(), false, L.LODs);
					L.bHornHidden = !Penguin->IsHornOutlineShown(&L.bHornHas);
					Penguin->GetOutlineInfo(L.Triangles);
					Shot(TEXT("63_no_outline_high"));
					++L.Shots;
					L.Stage = 2;
					L.At = T;
				}
				break;
			case 2:
				if (T > L.At + 0.25f)
				{
					Noot();
					L.Stage = 3;
					L.At = T;
				}
				break;
			case 3:
				if (T > L.At + HornAfter)
				{
					L.bHornOut = HornOut();
					L.bHornHiddenOut = !Penguin->IsHornOutlineShown();
					Shot(TEXT("63_no_outline_horn"));
					++L.Shots;
					L.Stage = 4;
					L.At = T;
				}
				break;
			case 4: // a dash: its afterimages (the one-slot ghost penguin: no hull of its own) draw as before
				if (T > L.At + HornGoneAfter)
				{
					Move->DashPressed();
					L.Stage = 5;
					L.At = T;
				}
				break;
			case 5:
				if (T > L.At + 0.07f)
				{
					L.bDashing = Move->IsDashing();
					L.Stage = 6;
					L.At = T;
				}
				break;
			case 6: // a penguin that spawns later has none either; then a trait that clips the outline (Speedster)
				if (T > L.At + 0.6f) // (the dash is over)
				{
					FActorSpawnParameters Params;
					Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					L.Extra = GetWorld()->SpawnActor<APenguinCharacter>(Penguin->GetClass(), FVector(FlatIslandX + 1500.f, 0.f, 62.f), FRotator::ZeroRotator, Params);
					if (APenguinCharacter* Extra = L.Extra.Get())
					{
						int32 LODs = 0;
						L.SpawnRight = LODsWithOutline(Extra, false, LODs);
						L.bSpawnHornHidden = !Extra->IsHornOutlineShown();
						Extra->Destroy();
					}
					if (UPenguinTraitComponent* Traits = Penguin->GetTraits())
					{
						Traits->SetTrait(EPinguTrait::Speedster, -1.f);
					}
					L.Stage = 7;
					L.At = T;
				}
				break;
			case 7: // trait on: the clipped outline material is there and the sections stay hidden
				if (T > L.At + SettleTime)
				{
					int32 LODs = 0;
					L.TraitMaterial = OutlineSlotMaterial(Penguin.Get());
					L.TraitRight = LODsWithOutline(Penguin.Get(), false, LODs);
					Shot(TEXT("63_no_outline_trait"));
					++L.Shots;
					L.Stage = 8;
					L.At = T;
				}
				break;
			case 8:
				if (T > L.At + 0.25f)
				{
					if (UPenguinTraitComponent* Traits = Penguin->GetTraits())
					{
						Traits->SetTrait(EPinguTrait::None, -1.f);
					}
					Settings->ApplyPreset(EPinguGraphicsPreset::Low);
					L.Stage = 9;
					L.At = T;
				}
				break;
			case 9: // Low's shaders first, then its picture
				if (T > L.At + 0.6f && (!Compiling() || T > L.At + ShaderWaitMax))
				{
					int32 LODs = 0;
					L.LowRight = LODsWithOutline(Penguin.Get(), false, LODs);
					L.bLowHornHidden = !Penguin->IsHornOutlineShown();
					if (const USkeletalMeshComponent* SkelMesh = Penguin->GetMesh())
					{
						L.LowLod = SkelMesh->GetPredictedLODLevel();
					}
					Shot(TEXT("63_no_outline_low"));
					++L.Shots;
					L.Stage = 10;
					L.At = T;
				}
				break;
			case 10:
				if (T > L.At + 0.3f)
				{
					StepTime = 1000.f; // done (the picture is written by now)
				}
				break;
			default:
				break;
			}
		},
		[this, Look]
		{
			FNoOutline& L = *Look;
			if (UPenguinTraitComponent* Traits = Penguin.IsValid() ? Penguin->GetTraits() : nullptr)
			{
				Traits->SetTrait(EPinguTrait::None, -1.f);
			}
			if (APenguinCharacter* Extra = L.Extra.Get())
			{
				Extra->Destroy();
			}
			if (UPinguSettings* Settings = UPinguSettings::Get(); Settings && L.bPresetSet)
			{
				// The options as they were (a preset's own table and frame cap; custom options as they were).
				if (L.Before.Preset == EPinguGraphicsPreset::Custom)
				{
					Settings->SetGraphics(L.Before);
				}
				else
				{
					Settings->ApplyPreset(L.Before.Preset);
				}
				Settings->ApplyGraphics(GetWorld());
			}
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			ResetTarget(Target.IsValid() ? Target->GetActorLocation() : FVector(FlatIslandX, 0.f, 62.f));

			const bool bHigh = L.LODs >= 4 && L.HighRight == L.LODs && L.bHornHas && L.bHornHidden && L.bHornOut && L.bHornHiddenOut;
			const bool bSpawn = L.SpawnRight == L.LODs && L.bSpawnHornHidden;
			const bool bTrait = IsClipMaterial(L.TraitMaterial) && L.TraitRight == L.LODs;
			const bool bLow = L.LowRight == L.LODs && L.bLowHornHidden;
			const bool bOk = bHigh && bSpawn && bTrait && bLow && L.bDashing && L.Shots == 4;
			FString Drawn;
			for (int32 LOD = 0; LOD < L.Triangles.Num(); ++LOD)
			{
				const FPenguinOutlineLODInfo& Info = L.Triangles[LOD];
				Drawn += FString::Printf(TEXT("%sLOD%d %d of %d (outline %d)"), Drawn.IsEmpty() ? TEXT("") : TEXT(", "), LOD, Info.DrawnTriangles, Info.Triangles, Info.OutlineTriangles);
			}
			Report(FString::Printf(TEXT("RESULT no outline: %d of %d LODs have all their outline sections hidden, the horn's outline hidden %d (horn has one %d, out %d, hidden while out %d), "
				"dashing %d; a penguin spawned later: %d of %d hidden, horn's %d; Speedster's clip material (%s) with %d of %d hidden; Low: %d of %d hidden, horn's %d (drawn at LOD %d); "
				"triangles drawn on High: %s; shots %d (want 4): %s"),
				L.HighRight, L.LODs, L.bHornHidden ? 1 : 0, L.bHornHas ? 1 : 0, L.bHornOut ? 1 : 0, L.bHornHiddenOut ? 1 : 0,
				L.bDashing ? 1 : 0, L.SpawnRight, L.LODs, L.bSpawnHornHidden ? 1 : 0, *L.TraitMaterial, L.TraitRight, L.LODs,
				L.LowRight, L.LODs, L.bLowHornHidden ? 1 : 0, L.LowLod, *Drawn, L.Shots, bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}
