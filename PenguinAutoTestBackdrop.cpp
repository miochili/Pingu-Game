// Pingu - the background on two presets: the autotest's look_BackgroundPresets step (APenguinAutoTest::AddBackdropPresetStep).
//
// The same four views on High, then on Low (its shaders compiled first, as in look_ShellBubble): the game camera (35 degrees
// wide, 8 degrees down, 50 m out), the horizon (1 degree down), the sky (14 degrees up: the cirrostratus veil) and the
// nearest background iceberg up close (45_bg_high_* / 45_bg_low_*). Checked: the cloud ring shows and draws M_CloudRing, every
// iceberg mesh has its three LODs, and what the background costs from the game camera: its triangles on each preset (each
// instance at the LOD its screen size picks with the preset's r.StaticMeshLODDistanceScale, the way the engine picks it).
// The preset as it was afterwards (a test run saves nothing).

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCamera.h"
#include "PinguSettings.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "ShaderCompiler.h"
#include "StaticMeshResources.h"

namespace PenguinAutoTestBackdropPrivate
{
	/** The longest wait for a preset's shaders (s). */
	constexpr float ShaderWaitMax = 60.f;
	/** Time between moving the camera and taking its picture (s): the camera's frame settles, the picture is written. */
	constexpr float SettleTime = 0.35f;
	/** Time after a picture's request before the camera moves on (s): the picture is written a frame or two later. */
	constexpr float ShotWriteTime = 0.25f;
	/** The background's instanced parts (component names in ABlockIsland). */
	static const TCHAR* const BackdropParts[] = { TEXT("Backdrop"), TEXT("BackdropPeaks"), TEXT("BackdropDomes"), TEXT("Floes"), TEXT("Mountains") };

	struct FBackdropLook
	{
		bool bPresetSet = false;
		FPinguGraphicsOptions Before;
		int32 Preset = 0;      // 0 High, 1 Low
		int32 Stage = 0;       // 0 wait for shaders, 1..4 the views, 5 next preset
		bool bShotTaken = false;
		float At = 0.f;
		int32 Shots = 0;
		int32 CloudShown[2] = { 0, 0 };
		FString CloudMaterial[2];
		int64 Triangles[2] = { 0, 0 };
		int32 Instances = 0;
		FString Meshes;
		bool bMeshesOk = true;
	};

	/** The triangles of a mesh's LOD (0 when there is no render data). */
	static int32 LodTriangles(const UStaticMesh* Mesh, int32 Lod)
	{
		const FStaticMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
		return Data && Data->LODResources.IsValidIndex(Lod) ? Data->LODResources[Lod].GetNumTriangles() : 0;
	}

	/** The LOD the engine would draw for a bounding sphere of Radius at Distance (horizontal FOV, 16:9), with the preset's
	 *  r.StaticMeshLODDistanceScale: the screen size (2 x the projected radius, as ComputeBoundsScreenSize) divided by that
	 *  scale, against the mesh's LOD screen sizes. */
	static int32 PickLod(const UStaticMesh* Mesh, float Radius, float Distance, float FovDeg)
	{
		const FStaticMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!Data || Data->LODResources.Num() == 0)
		{
			return 0;
		}
		static const IConsoleVariable* DistanceScaleVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.StaticMeshLODDistanceScale"));
		const float DistanceScale = DistanceScaleVar ? FMath::Max(DistanceScaleVar->GetFloat(), 0.01f) : 1.f;
		const float ProjY = 1.f / FMath::Tan(FMath::DegreesToRadians(FovDeg * 0.5f)) * (16.f / 9.f);
		const float ScreenSize = ProjY * Radius / FMath::Max(Distance, 1.f) / DistanceScale;
		int32 Lod = 0;
		for (int32 Index = 1; Index < Data->LODResources.Num() && Index < MAX_STATIC_MESH_LODS; ++Index)
		{
			if (ScreenSize < Data->ScreenSize[Index].GetValue())
			{
				Lod = Index;
			}
		}
		return Lod;
	}
}

void APenguinAutoTest::AddBackdropPresetStep()
{
	using namespace PenguinAutoTestBackdropPrivate;
	const TSharedRef<FBackdropLook> Look = MakeShared<FBackdropLook>();
	AddStep(TEXT("look_BackgroundPresets"), 2.f * (ShaderWaitMax + 6.f),
		[this, Look]
		{
			*Look = FBackdropLook();
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				Look->Before = Settings->GetGraphics();
				Look->bPresetSet = true;
				Settings->ApplyPreset(EPinguGraphicsPreset::High);
			}
			PrepareBrawl(2400.f, {});
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
				const ABlockIsland* Main = ABlockIsland::FindNear(GetWorld(), FVector::ZeroVector);
				const FBox Bounds = Main ? Main->GetBounds() : FBox(ForceInit);
				BackgroundFocus = (Bounds.IsValid ? FVector(Bounds.GetCenter().X, 0.f, Bounds.Max.Z) : Location()) + FVector(0.f, 0.f, 150.f);
				Stage->SetActorLocationAndRotation(BackgroundFocus - FRotator(-8.f, -90.f, 0.f).Vector() * 5000.f, FRotator(-8.f, -90.f, 0.f));
			}
			// The iceberg meshes and their LODs.
			if (const ABlockIsland* Main = ABlockIsland::FindNear(GetWorld(), FVector::ZeroVector))
			{
				TArray<UInstancedStaticMeshComponent*> Parts;
				Main->GetComponents<UInstancedStaticMeshComponent>(Parts);
				for (const UInstancedStaticMeshComponent* Part : Parts)
				{
					const FString Name = Part->GetName();
					if (Name != TEXT("Backdrop") && Name != TEXT("BackdropPeaks") && Name != TEXT("BackdropDomes") && Name != TEXT("Floes"))
					{
						continue;
					}
					const UStaticMesh* Mesh = Part->GetStaticMesh();
					const FStaticMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
					const int32 Lods = Data ? Data->LODResources.Num() : 0;
					FString Tris;
					for (int32 Lod = 0; Lod < Lods; ++Lod)
					{
						Tris += FString::Printf(TEXT("%s%d"), Lod ? TEXT("/") : TEXT(""), LodTriangles(Mesh, Lod));
					}
					Look->Meshes += FString::Printf(TEXT("%s%s %s x%d (%s)"), Look->Meshes.IsEmpty() ? TEXT("") : TEXT(", "), *Name,
						Mesh ? *Mesh->GetName() : TEXT("none"), Part->GetInstanceCount(), *Tris);
					Look->bMeshesOk &= Lods == 3 || Part->GetInstanceCount() == 0;
				}
			}
		},
		[this, Look](float T)
		{
			FBackdropLook& L = *Look;
			AActor* Camera = LookCamera.Get();
			const ABlockIsland* Main = ABlockIsland::FindNear(GetWorld(), FVector::ZeroVector);
			const TCHAR* const PresetName = L.Preset == 0 ? TEXT("high") : TEXT("low");
			if (L.Stage == 0)
			{
				const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
				if (T > L.At + 0.8f && (!bCompiling || T > L.At + ShaderWaitMax))
				{
					L.Stage = 1;
					L.At = T;
					if (Camera)
					{
						Camera->SetActorLocationAndRotation(BackgroundFocus - FRotator(-8.f, -90.f, 0.f).Vector() * 5000.f, FRotator(-8.f, -90.f, 0.f));
					}
					// The background's triangles from the game camera on this preset.
					if (Main && Camera)
					{
						const FVector From = Camera->GetActorLocation();
						int64 Sum = 0;
						int32 Count = 0;
						TArray<UInstancedStaticMeshComponent*> Parts;
						Main->GetComponents<UInstancedStaticMeshComponent>(Parts);
						for (const UInstancedStaticMeshComponent* Part : Parts)
						{
							const FString Name = Part->GetName();
							bool bBackdrop = false;
							for (const TCHAR* Wanted : BackdropParts)
							{
								bBackdrop |= Name == Wanted;
							}
							const UStaticMesh* Mesh = Part->GetStaticMesh();
							if (!bBackdrop || !Mesh || !Part->IsVisible())
							{
								continue;
							}
							const float MeshRadius = Mesh->GetBounds().SphereRadius;
							for (int32 Index = 0; Index < Part->GetInstanceCount(); ++Index)
							{
								FTransform Instance;
								Part->GetInstanceTransform(Index, Instance, true);
								const float Radius = MeshRadius * Instance.GetScale3D().GetAbsMax();
								const int32 Lod = PickLod(Mesh, Radius, FVector::Dist(Instance.GetLocation(), From), 35.f);
								Sum += LodTriangles(Mesh, Lod);
								++Count;
							}
						}
						L.Triangles[L.Preset] = Sum;
						L.Instances = Count;
					}
					// The cloud ring.
					if (Main)
					{
						TArray<UStaticMeshComponent*> Comps;
						Main->GetComponents<UStaticMeshComponent>(Comps);
						for (const UStaticMeshComponent* Comp : Comps)
						{
							if (Comp->GetName() == TEXT("CloudRing"))
							{
								L.CloudShown[L.Preset] = Comp->IsVisible() ? 1 : 0;
								const UMaterialInterface* Material = Comp->GetMaterial(0);
								L.CloudMaterial[L.Preset] = Material ? Material->GetName() : TEXT("none");
							}
						}
					}
				}
				return;
			}
			if (L.Stage >= 1 && L.Stage <= 4 && !L.bShotTaken && T > L.At + SettleTime)
			{
				static const TCHAR* const Views[] = { TEXT("game"), TEXT("horizon"), TEXT("sky"), TEXT("berg") };
				Shot(*FString::Printf(TEXT("45_bg_%s_%s"), PresetName, Views[L.Stage - 1]));
				++L.Shots;
				L.bShotTaken = true;
				L.At = T;
				return;
			}
			// The picture is written a frame or two after the request: the camera moves on only after that.
			if (L.Stage >= 1 && L.Stage <= 4 && L.bShotTaken && T > L.At + ShotWriteTime)
			{
				L.bShotTaken = false;
				++L.Stage;
				L.At = T;
				if (!Camera)
				{
					return;
				}
				if (L.Stage == 2)
				{
					Camera->SetActorLocationAndRotation(BackgroundFocus + FVector(0.f, 5000.f, 550.f), FRotator(-1.f, -90.f, 0.f));
				}
				else if (L.Stage == 3)
				{
					Camera->SetActorLocationAndRotation(BackgroundFocus + FVector(0.f, 5000.f, 550.f), FRotator(14.f, -90.f, 0.f));
				}
				else if (L.Stage == 4 && Main)
				{
					// The nearest peaked, domed or tabular berg (the one with the smallest distance out), from 3.2 of its size in
					// front of it and a little above.
					FTransform Best;
					float BestY = -1e12f;
					TArray<UInstancedStaticMeshComponent*> Parts;
					Main->GetComponents<UInstancedStaticMeshComponent>(Parts);
					for (const UInstancedStaticMeshComponent* Part : Parts)
					{
						const FString Name = Part->GetName();
						if (Name != TEXT("Backdrop") && Name != TEXT("BackdropPeaks") && Name != TEXT("BackdropDomes"))
						{
							continue;
						}
						for (int32 Index = 0; Index < Part->GetInstanceCount(); ++Index)
						{
							FTransform Instance;
							Part->GetInstanceTransform(Index, Instance, true);
							if (Instance.GetLocation().Y > BestY)
							{
								BestY = Instance.GetLocation().Y;
								Best = Instance;
							}
						}
					}
					const FVector Size = Best.GetScale3D() * 100.f;
					const FVector Target = Best.GetLocation() + FVector(0.f, 0.f, Size.Z * 0.1f);
					const FVector From = Target + FVector(0.f, FMath::Max(Size.X, Size.Y) * 3.2f, Size.Z * 0.6f);
					Camera->SetActorLocationAndRotation(From, (Target - From).Rotation());
				}
				return;
			}
			if (L.Stage == 5 && T > L.At + 0.3f)
			{
				if (L.Preset == 0)
				{
					L.Preset = 1;
					L.Stage = 0;
					L.At = T;
					if (UPinguSettings* Settings = UPinguSettings::Get())
					{
						Settings->ApplyPreset(EPinguGraphicsPreset::Low);
					}
				}
				else
				{
					StepTime = 1000.f; // done (the last picture is written by now)
				}
			}
		},
		[this, Look]
		{
			const FBackdropLook& L = *Look;
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
			const bool bOk = L.Shots == 8 && L.CloudShown[0] == 1 && L.CloudShown[1] == 1 && L.CloudMaterial[0] == TEXT("M_CloudRing")
				&& L.CloudMaterial[1] == TEXT("M_CloudRing") && L.bMeshesOk;
			Report(FString::Printf(TEXT("RESULT background presets: shots %d (want 8), cloud ring shown on High / Low %d / %d, material %s / %s (want M_CloudRing), bergs %s (want 3 LODs each), background triangles from the game camera High %lld / Low %lld (%d instances): %s"),
				L.Shots, L.CloudShown[0], L.CloudShown[1], *L.CloudMaterial[0], *L.CloudMaterial[1], *L.Meshes,
				L.Triangles[0], L.Triangles[1], L.Instances, bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}
