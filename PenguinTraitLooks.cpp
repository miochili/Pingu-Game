// Pingu - the traits' looks (UPenguinTraitComponent's look hooks): the pieces on the penguin, its body's shape
// (APenguinCharacter::SetTraitBodyScale), overlays and idle effects. Every machine builds its own from the replicated
// trait state and events and the flow clock (GetRevealAge): local only, never broadcast.
//
// How a look comes and goes:
//   - a new trait state (a new Serial: every round deals one) takes the old look off at once;
//   - the new look goes on when the reveal reaches its transform (UPenguinTraitComponent::TransformAt, a little later
//     for penguins further right, as their cards fly in one after the other): an elastic pop of the body's shape and of
//     the pieces, a burst in the trait's colour and the trait's own sound - on every machine, timed on the flow clock;
//   - a machine that sees a trait whose transform is long past (a LAN client joining late, a trait without a reveal)
//     puts the look on quietly: no pop, no burst, no sound.
//
// Pieces are engine shapes (/Engine/BasicShapes) and a few effect meshes (/Game/FX) with their own material instances,
// on the penguin mesh's bones (placed from Pingu.blend's rest pose through the bones' reference pose, like the noot
// horn): at most four per penguin, no collision, shadows only from the High shadows setting up. Low shows the pieces and
// the Glass Cannon / Mirror overlays only (no idle effects); idle effects scale with the effects density. Jester's cap is
// a mesh of its own (PinguJesterHat: Tools/ArtGen/make_jester_hat.py), on spring bones on Ultra.
//
// The beanie: Speedster wears a headband instead, Slow Burn a burning fuse instead of the pom. The beanie, its cuff and
// the pom are sections of their own (felt, wool, pom) on the Body bone - those are hidden (ShowMaterialSection). The
// ink outline is one section of the whole penguin and on LOD2 / LOD3 the pom shares the merged slot, so those two slots
// get M_Outline_Clip / M_Penguin_LowMerged_Clip (Tools/ue_setup_project.py, phase traitlooks): the same looks with a
// clip (a sphere round the pom; or above the cuff's line, outside the head's own dome) that follows the Body bone.

#include "PenguinTraitLooks.h"
#include "PenguinTraitComponent.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PinguFX.h"
#include "PinguJesterHat.h"
#include "PinguSettings.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UnrealClient.h"

namespace PenguinTraitLooksPrivate
{
	// ---- Pingu.blend's rig in its rest pose (Blender units: x across the penguin, -y forward, z up). The bone heads tie
	// the imported mesh's axes and units to Blender's (as in APenguinCharacter::FitIceFeet).
	const FVector BlendFootLHead(-0.234f, 0.f, 0.4f);
	const FVector BlendFootRHead(0.234f, 0.f, 0.4f);
	const FVector BlendBodyHead(0.f, 0.f, 0.45f);
	const FVector BlendEyeTargetHead(0.f, -2.5f, 1.35f);

	// ---- the head and the beanie (measured in Pingu.blend's rest pose)
	/** The head's dome under the beanie: inside a sphere round this point (LOD2 / LOD3's merged slot keeps it; the ink
	 *  outline above the cuff's line goes whole - the beanie's own outline lies too close to the dome's to tell apart). */
	const FVector BlendDomeCentre(0.f, 0.f, 1.436f);
	constexpr float BlendDomeKeepRadius = 0.53f;
	/** The pom: its middle and the radius that takes its ink outline too. */
	const FVector BlendPomCentre(0.f, 0.088f, 2.05f);
	constexpr float BlendPomClipRadius = 0.142f;
	/** The cuff's lower edge: about a plane through these two points on the middle line (front low ... back), tilted back. */
	const FVector BlendCuffFront(0.f, -0.42f, 1.725f);
	const FVector BlendCuffBack(0.f, 0.5f, 1.572f);

	/** Speedster's headband (SM_Ring, a torus): round the upper head above the eyes, tipped back a little; its outer width,
	 *  its height (the tube stretched tall: a rounded band) and the knot at the back. */
	const FVector BlendBandCentre(0.f, 0.03f, 1.74f);
	const FVector BlendBandAxis(0.f, 0.14f, 1.f);
	constexpr float BlendBandWidth = 0.98f;
	constexpr float BlendBandHeight = 0.19f;
	const FVector BlendBandKnot(0.f, 0.5f, 1.7f);

	/** A trait's transform: the pop of the body's shape and of the pieces lasts this long (elastic). */
	constexpr float PopTime = 0.6f;
	/** The cards fly in one after the other from left to right (UI: PinguUITraits.cpp uses the same): this much later per
	 *  penguin further right. */
	constexpr float StaggerStep = 0.03f;
	/** A transform this long past when this machine first sees the trait: put the look on quietly (a late joiner). */
	constexpr float QuietAfter = 0.3f;
	/** Pieces per penguin, at most. */
	constexpr int32 MaxPieces = 4;

	enum class EShape : uint8
	{
		Sphere,
		Cube,
		Cylinder,
		Cone,
		Fish,
		Shard,
		Ring,
		Count
	};

	static UStaticMesh* LoadShape(EShape Shape)
	{
		static const TCHAR* Paths[] = {
			TEXT("/Engine/BasicShapes/Sphere.Sphere"),
			TEXT("/Engine/BasicShapes/Cube.Cube"),
			TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),
			TEXT("/Engine/BasicShapes/Cone.Cone"),
			TEXT("/Game/FX/SM_Fish.SM_Fish"),
			TEXT("/Game/FX/SM_Shard.SM_Shard"),
			TEXT("/Game/FX/SM_Ring.SM_Ring"),
		};
		static_assert(UE_ARRAY_COUNT(Paths) == static_cast<int32>(EShape::Count), "shape table out of sync");
		UStaticMesh* Found = LoadObject<UStaticMesh>(nullptr, Paths[static_cast<int32>(Shape)], nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!Found && Shape >= EShape::Fish)
		{
			Found = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"), nullptr, LOAD_Quiet | LOAD_NoWarn);
		}
		return Found;
	}

	static UMaterialInterface* LoadMaterial(const TCHAR* Path)
	{
		return LoadObject<UMaterialInterface>(nullptr, Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
	}

	/** The rig's map from Pingu.blend's rest pose to the mesh's component space (reference pose). */
	struct FRigMap
	{
		bool bValid = false;
		FVector Origin = FVector::ZeroVector;
		FVector MapX = FVector(100.f, 0.f, 0.f);
		FVector MapY = FVector(0.f, 100.f, 0.f);
		FVector MapZ = FVector(0.f, 0.f, 100.f);
		/** The penguin's own frame in the mesh: X forward (Blender -y), Y across (Blender +x), Z up. */
		FQuat Frame = FQuat::Identity;
		/** Mesh units per Blender unit. */
		float Unit = 100.f;

		FVector ToMesh(const FVector& Blend) const
		{
			return Origin + MapX * Blend.X + MapY * Blend.Y + MapZ * Blend.Z;
		}
		/** A direction in Pingu.blend -> the penguin's frame (X forward, Y across, Z up). */
		static FVector BlendDirToFrame(const FVector& Dir)
		{
			return FVector(-Dir.Y, Dir.X, Dir.Z);
		}
	};

	static bool BuildRigMap(const USkeletalMeshComponent* SkelMesh, FRigMap& Out)
	{
		Out.bValid = false;
		const USkeletalMesh* Asset = SkelMesh ? SkelMesh->GetSkeletalMeshAsset() : nullptr;
		if (!Asset)
		{
			return false;
		}
		for (const TCHAR* Bone : { TEXT("Root"), TEXT("Body"), TEXT("Foot_L"), TEXT("Foot_R"), TEXT("Eye_Target") })
		{
			if (SkelMesh->GetBoneIndex(FName(Bone)) == INDEX_NONE)
			{
				return false; // not the penguin rig
			}
		}
		const auto RefLocation = [Asset](const TCHAR* Bone) { return Asset->GetComposedRefPoseMatrix(FName(Bone)).GetOrigin(); };
		Out.Origin = RefLocation(TEXT("Root"));
		Out.MapX = (RefLocation(TEXT("Foot_R")) - RefLocation(TEXT("Foot_L"))) / static_cast<float>(BlendFootRHead.X - BlendFootLHead.X);
		Out.MapZ = (RefLocation(TEXT("Body")) - Out.Origin) / static_cast<float>(BlendBodyHead.Z);
		Out.MapY = (RefLocation(TEXT("Eye_Target")) - RefLocation(TEXT("Body")) - Out.MapZ * static_cast<float>(BlendEyeTargetHead.Z - BlendBodyHead.Z))
			/ static_cast<float>(BlendEyeTargetHead.Y - BlendBodyHead.Y);
		Out.Unit = static_cast<float>(Out.MapZ.Size());
		const FVector Up = Out.MapZ.GetSafeNormal();
		const FVector Forward = (-Out.MapY - Up * (-Out.MapY | Up)).GetSafeNormal();
		const FVector Across = Up ^ Forward;
		if (Up.IsNearlyZero() || Forward.IsNearlyZero() || Out.Unit <= 1.e-3f)
		{
			return false;
		}
		Out.Frame = FMatrix(Forward, Across, Up, FVector::ZeroVector).ToQuat();
		Out.bValid = true;
		return true;
	}

	/** 0 -> 1 with an elastic overshoot (easeOutElastic). */
	static float ElasticOut(float P)
	{
		if (P <= 0.f)
		{
			return 0.f;
		}
		if (P >= 1.f)
		{
			return 1.f;
		}
		return FMath::Pow(2.f, -10.f * P) * FMath::Sin((P * 10.f - 0.75f) * (2.f * UE_PI / 3.f)) + 1.f;
	}

	/** What the graphics settings let the look show (read every frame; a change rebuilds the look's materials). */
	struct FLookQuality
	{
		EPinguQualityLevel Detail = EPinguQualityLevel::High;
		EPinguQualityLevel Effects = EPinguQualityLevel::High;
		bool bShadows = true;
		bool bUltra = false;

		bool operator==(const FLookQuality& Other) const
		{
			return Detail == Other.Detail && Effects == Other.Effects && bShadows == Other.bShadows && bUltra == Other.bUltra;
		}
		bool operator!=(const FLookQuality& Other) const { return !(*this == Other); }
		/** Idle effects (Medium effects and up). */
		bool HasIdleFX() const { return Effects >= EPinguQualityLevel::Medium; }
		/** The extra overlays (Ice Touch, Rubber): High detail and up. */
		bool HasExtraOverlays() const { return Detail >= EPinguQualityLevel::High; }
	};

	static FLookQuality ReadQuality()
	{
		FLookQuality Q;
		if (const UPinguSettings* Settings = UPinguSettings::Get())
		{
			const FPinguGraphicsOptions& Options = Settings->GetGraphics();
			Q.Detail = Options.ModelDetail;
			Q.Effects = Options.Effects;
			Q.bShadows = Options.Shadows >= EPinguQualityLevel::High;
		}
		Q.bUltra = UPinguSettings::UseUltraEffects();
		return Q;
	}

	/** Seconds between two idle effects that come every Interval at full density (none below Medium). */
	static float IdleInterval(float Interval)
	{
		return Interval / FMath::Max(UPinguSettings::GetFXDensity(), 0.3f);
	}

	/** Rank of Penguin among the penguins whose trait reveal starts with its own, left to right (0 = leftmost): the cards
	 *  fly in, and the penguins change, in that order. */
	static int32 GetRevealRank(const APenguinCharacter* Penguin, float RevealStart)
	{
		const UWorld* World = Penguin ? Penguin->GetWorld() : nullptr;
		if (!World || RevealStart < 0.f)
		{
			return 0;
		}
		const double X = Penguin->GetActorLocation().X;
		int32 Rank = 0;
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			const APenguinCharacter* Other = *It;
			const UPenguinTraitComponent* Traits = IsValid(Other) && Other != Penguin ? Other->GetTraits() : nullptr;
			if (!Traits || !Traits->HasAnyTrait() || FMath::Abs(Traits->GetState().RevealStart - RevealStart) > 0.05f)
			{
				continue;
			}
			const double OtherX = Other->GetActorLocation().X;
			Rank += (OtherX < X || (OtherX == X && Other->GetPlayerSlot() < Penguin->GetPlayerSlot())) ? 1 : 0;
		}
		return Rank;
	}

	/** -PinguTraitLookShots (with -PinguMatchTest and -PinguTrait=...): Agent C's own pictures of every look - the reveal
	 *  as the match shows it, then each penguin close up, on the preset it runs on and on Low (Saved/ClaudeLogs/Traits). */
	struct FLookShots
	{
		bool bChecked = false;
		bool bOn = false;
		uint64 LastFrame = 0;
		int32 Stage = 0;
		float Clock = 0.f;
		int32 Target = 0;
		int32 Round = 0;
		int32 RevealShots = 0;
		TWeakObjectPtr<APenguinCamera> Camera;
		TArray<TWeakObjectPtr<APenguinCharacter>> Order;
		/** The graphics options the run started with (put back at the end). */
		FPinguGraphicsOptions Before;
	};

	static FLookShots& GetLookShots()
	{
		static FLookShots Shots;
		if (!Shots.bChecked)
		{
			Shots.bChecked = true;
			// (Only in a test run: it switches the graphics preset, and a test run never saves settings.)
			Shots.bOn = FParse::Param(FCommandLine::Get(), TEXT("PinguTraitLookShots")) && UPinguSettings::IsAutomatedRun();
		}
		return Shots;
	}

	static PinguTraitLooks::FLookCounters& MutableCounters()
	{
		static PinguTraitLooks::FLookCounters Counters;
		return Counters;
	}

	static void RequestShot(const FString& Name)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ClaudeLogs/Traits") / Name + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		UE_LOG(LogPingu, Display, TEXT("[TRAITLOOKS] screenshot %s"), *Path);
	}
}

/** One piece of a look: an engine shape (or an effect mesh) on one of the penguin's bones. */
struct FPinguTraitPiece
{
	TWeakObjectPtr<UStaticMeshComponent> Component;
	TWeakObjectPtr<UMaterialInstanceDynamic> Material;
	FName Bone;
	/** The bone in the mesh's reference pose (component space): the piece's place is given against the rest pose. */
	FTransform BoneRef = FTransform::Identity;
	/** Where it sits (Pingu.blend's rest pose), how it turns in the penguin's frame (X forward, Y across, Z up) and its size
	 *  along its own axes (Blender units). */
	FVector Location = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector Size = FVector::OneVector;
	/** The mesh's own middle and size (engine shapes: 0 and 100 along every axis). */
	FVector MeshCentre = FVector::ZeroVector;
	FVector MeshSize = FVector(100.f);
	/** Its colour, glow and roughness (M_Prop). */
	FLinearColor Color = FLinearColor::White;
	float Emissive = 0.f;
	/** The pop in starts this much after the look's (seconds). */
	float PopDelay = 0.f;
	bool bShown = true;
};

/** The look's own state on this machine (made on first use). */
struct FPinguTraitLooksState
{
	// ---- which trait the look is for
	bool bSerialKnown = false;
	uint8 Serial = 0;
	EPinguTrait Trait = EPinguTrait::None;
	/** The trait waits for its transform to put the look on. */
	bool bPending = false;
	/** The look is on. */
	bool bApplied = false;
	/** Seconds since the look went on (the pop runs while this is below PopTime); large when it went on quietly. */
	float Age = 1000.f;
	/** The reveal's stagger for this penguin (seconds after TransformAt). */
	float Stagger = 0.f;
	/** The look went on quietly (no pop, no burst, no sound). */
	bool bQuiet = false;

	// ---- what the look changed on the penguin
	TArray<FPinguTraitPiece> Pieces;
	/** Instanced pieces: one component (one piece, one draw) for several shapes of the same mesh, coloured per instance
	 *  through the effects' material M_FX (Jester's points and its bells). */
	struct FInstanced
	{
		TWeakObjectPtr<UInstancedStaticMeshComponent> Component;
		FTransform BoneRef = FTransform::Identity;
		FVector MeshCentre = FVector::ZeroVector;
		FVector MeshSize = FVector(100.f);
		/** Per instance, like FPinguTraitPiece: where (Pingu.blend's rest pose), how it turns (the penguin's frame), its size
		 *  (Blender units) and its colour. */
		TArray<FVector> Locations;
		TArray<FQuat> Rotations;
		TArray<FVector> Sizes;
		TArray<FLinearColor> Colors;
		float Emissive = 0.f;
		float PopDelay = 0.f;
		/** The instances swing a little (bells). */
		bool bSwing = false;
	};
	TArray<FInstanced> Instanced;
	/** Jester's felt cap (PinguJesterHat: static, or on spring bones on Ultra). */
	PinguJesterHat::FHat Hat;
	/** The trait's body shape the animation gives the Body bone right now (side, forward, up; the pop's ease included): its
	 *  eyes, flippers, tail and feet keep their shape (PenguinAnimInstance). */
	FVector BoneShape = FVector::OneVector;
	/** Mesh slots given another material (slot, the material before). */
	TArray<TPair<int32, TWeakObjectPtr<UMaterialInterface>>> SlotsBefore;
	/** Mesh sections hidden (material index, section, LOD). */
	struct FHiddenSection
	{
		int32 MaterialIndex = 0;
		int32 Section = 0;
		int32 LOD = 0;
	};
	TArray<FHiddenSection> HiddenSections;
	/** The clip materials on this penguin (their clip follows the Body bone). */
	TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> ClipMaterials;
	enum class EClip : uint8
	{
		None,
		Pom,     // Slow Burn: the pom
		Beanie,  // Speedster: the whole beanie
	};
	EClip Clip = EClip::None;
	/** The overlay this look wants (null: none) and its material (made once, reused). */
	bool bWantsOverlay = false;
	TWeakObjectPtr<UMaterialInstanceDynamic> Overlay;
	/** The body's shape the look gives. */
	FVector BodyScale = FVector::OneVector;
	/** The settings the look was built for. */
	PenguinTraitLooksPrivate::FLookQuality Quality;
	PenguinTraitLooksPrivate::FRigMap Map;

	// ---- motion and effects
	/** An extra squash spring of the body (Heavyweight's heavier landings, Rubber's wobble): + taller, - squashed. */
	float Jelly = 0.f;
	float JellyVelocity = 0.f;
	float FxTimer = 0.f;
	float FxTimer2 = 0.f;
	float Clock = 0.f;
	int32 SeenLand = 0;
	int32 SeenJump = 0;
	int32 SeenAirJump = 0;
	uint8 SeenGamble = 0;
	uint8 SeenSwaps = 0;
	float SeenSecondChance = -1.f;
	bool bMirrorReady = true;
	bool bClinging = false;
	float CooldownSeen[3] = { 0.f, 0.f, 0.f };
	/** Gambler: the dice tumble (seconds since the last roll that was not a plain use; large: none) and how. */
	float DiceRollAge = 1000.f;
	bool bDiceDouble = false;
	/** Second Chance: seconds since the fish leapt off (< 0: it is still on the head). */
	float FishLeapAge = -1.f;
	FRandomStream Rand;
};

namespace PenguinTraitLooksPrivate
{
	using FLook = FPinguTraitLooksState;

	/** Adds a piece (at most MaxPieces): Shape on Bone, its material M_Prop in Color (Emissive glow, Roughness) unless the
	 *  mesh keeps its own (bOwnMaterial). The caller sets its place (Location / Rotation / Size). */
	static FPinguTraitPiece* AddPiece(UPenguinTraitComponent* Traits, FLook& Look, USkeletalMeshComponent* SkelMesh, EShape Shape, const TCHAR* Bone,
		const FLinearColor& Color, float Emissive = 0.f, float Roughness = 0.5f, bool bOwnMaterial = false)
	{
		const int32 Used = Look.Pieces.Num() + Look.Instanced.Num();
		const USkeletalMesh* Asset = SkelMesh ? SkelMesh->GetSkeletalMeshAsset() : nullptr;
		if (Used >= MaxPieces || !Asset || SkelMesh->GetBoneIndex(FName(Bone)) == INDEX_NONE)
		{
			return nullptr;
		}
		UStaticMesh* StaticMesh = LoadShape(Shape);
		AActor* PenguinActor = Traits->GetOwner();
		if (!StaticMesh || !PenguinActor)
		{
			return nullptr;
		}
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(PenguinActor, NAME_None, RF_Transient);
		Component->SetStaticMesh(StaticMesh);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCanEverAffectNavigation(false);
		Component->SetCastShadow(Look.Quality.bShadows);
		Component->bReceivesDecals = false;
		Component->SetupAttachment(SkelMesh, FName(Bone));
		Component->RegisterComponent();
		Traits->TraitObjects.Add(Component);

		FPinguTraitPiece& Piece = Look.Pieces.AddDefaulted_GetRef();
		Piece.Component = Component;
		Piece.Bone = FName(Bone);
		Piece.BoneRef = FTransform(Asset->GetComposedRefPoseMatrix(Piece.Bone));
		Piece.Color = Color;
		Piece.Emissive = Emissive;
		const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
		Piece.MeshCentre = Bounds.Origin;
		Piece.MeshSize = FVector(FMath::Max(Bounds.BoxExtent.X, 0.01), FMath::Max(Bounds.BoxExtent.Y, 0.01), FMath::Max(Bounds.BoxExtent.Z, 0.01)) * 2.f;
		if (!bOwnMaterial)
		{
			if (UMaterialInterface* Prop = LoadMaterial(TEXT("/Game/FX/M_Prop.M_Prop")))
			{
				UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Prop, Component);
				Material->SetVectorParameterValue(TEXT("Color"), Color);
				Material->SetScalarParameterValue(TEXT("Emissive"), Emissive);
				Material->SetScalarParameterValue(TEXT("Roughness"), Roughness);
				Component->SetMaterial(0, Material);
				Piece.Material = Material;
				Traits->TraitObjects.Add(Material);
			}
		}
		return &Piece;
	}

	/** Adds an instanced piece (counts as one piece): Shape on Bone with the effects' material M_FX (each instance's colour
	 *  and glow in its custom data). The caller fills its instances. */
	static FLook::FInstanced* AddInstanced(UPenguinTraitComponent* Traits, FLook& Look, USkeletalMeshComponent* SkelMesh, EShape Shape, const TCHAR* Bone,
		float Emissive, float PopDelay)
	{
		const int32 Used = Look.Pieces.Num() + Look.Instanced.Num();
		const USkeletalMesh* Asset = SkelMesh ? SkelMesh->GetSkeletalMeshAsset() : nullptr;
		if (Used >= MaxPieces || !Asset || SkelMesh->GetBoneIndex(FName(Bone)) == INDEX_NONE)
		{
			return nullptr;
		}
		UStaticMesh* StaticMesh = LoadShape(Shape);
		UMaterialInterface* FXMaterial = LoadMaterial(TEXT("/Game/FX/M_FX.M_FX"));
		AActor* PenguinActor = Traits->GetOwner();
		if (!StaticMesh || !FXMaterial || !PenguinActor)
		{
			return nullptr;
		}
		UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(PenguinActor, NAME_None, RF_Transient);
		Component->SetStaticMesh(StaticMesh);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCanEverAffectNavigation(false);
		Component->SetCastShadow(Look.Quality.bShadows);
		Component->SetupAttachment(SkelMesh, FName(Bone));
		Component->NumCustomDataFloats = 5; // like the effect particles (M_FX): R, G, B, glow, opacity
		Component->RegisterComponent();
		Component->SetMaterial(0, FXMaterial);
		Traits->TraitObjects.Add(Component);
		FLook::FInstanced& Group = Look.Instanced.AddDefaulted_GetRef();
		Group.Component = Component;
		Group.BoneRef = FTransform(Asset->GetComposedRefPoseMatrix(FName(Bone)));
		const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
		Group.MeshCentre = Bounds.Origin;
		Group.MeshSize = FVector(FMath::Max(Bounds.BoxExtent.X, 0.01), FMath::Max(Bounds.BoxExtent.Y, 0.01), FMath::Max(Bounds.BoxExtent.Z, 0.01)) * 2.f;
		Group.Emissive = Emissive;
		Group.PopDelay = PopDelay;
		return &Group;
	}

	/** The piece's place on its bone now: its rest pose place, size x SizeScale. */
	static void PlacePiece(const FRigMap& Map, FPinguTraitPiece& Piece, float SizeScale)
	{
		UStaticMeshComponent* Component = Piece.Component.Get();
		if (!Component)
		{
			return;
		}
		const FQuat Rotation = Map.Frame * Piece.Rotation;
		const float Grow = FMath::Max(SizeScale, 0.001f);
		const FVector Scale = Piece.Size * Map.Unit / Piece.MeshSize * Grow;
		const FVector Location = Map.ToMesh(Piece.Location) - Rotation.RotateVector(Piece.MeshCentre * Scale);
		const FTransform InMesh(Rotation, Location, Scale);
		Component->SetRelativeTransform(InMesh.GetRelativeTransform(Piece.BoneRef));
	}

	/** Same size along every axis: the longest axis of the mesh gets Length (Blender units) - for the effect meshes. */
	static void SetUniformLength(FPinguTraitPiece& Piece, float Length)
	{
		const double Longest = FMath::Max3(Piece.MeshSize.X, Piece.MeshSize.Y, Piece.MeshSize.Z);
		Piece.Size = Piece.MeshSize * (Length / FMath::Max(Longest, 0.01));
	}

	/** A rotation in the penguin's frame that turns the shape's +Z (a cone's tip, a cylinder's axis) along Dir (Blender
	 *  axes), then RollDegrees about it. */
	static FQuat AxisAlong(const FVector& BlendDir, float RollDegrees = 0.f)
	{
		const FVector Dir = FRigMap::BlendDirToFrame(BlendDir).GetSafeNormal();
		return FQuat::FindBetweenNormals(FVector::UpVector, Dir) * FQuat(FVector::UpVector, FMath::DegreesToRadians(RollDegrees));
	}

	/** A rotation in the penguin's frame that turns the mesh's +X (the shard's tip, the fish's head) along Dir (Blender). */
	static FQuat XAlong(const FVector& BlendDir, const FVector& BlendUpHint = FVector(0.f, 0.f, 1.f))
	{
		const FVector Dir = FRigMap::BlendDirToFrame(BlendDir).GetSafeNormal();
		const FVector UpHint = FRigMap::BlendDirToFrame(BlendUpHint).GetSafeNormal();
		return FRotationMatrix::MakeFromXZ(Dir, UpHint).ToQuat();
	}

	/** Mesh slot indices by a piece of their name (lower case), e.g. "felt", "outline". */
	static void FindSlots(const USkeletalMeshComponent* SkelMesh, const TCHAR* Piece, TArray<int32>& Out, const TCHAR* Except = nullptr)
	{
		const TArray<FName> Names = SkelMesh->GetMaterialSlotNames();
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			const FString Name = Names[Index].ToString().ToLower();
			if (Name.Contains(Piece) && (!Except || !Name.Contains(Except)))
			{
				Out.Add(Index);
			}
		}
	}

	/** Hides every section (every LOD) that draws one of these material slots. */
	static void HideSlots(FLook& Look, USkeletalMeshComponent* SkelMesh, const TArray<int32>& Slots)
	{
		USkeletalMesh* Asset = SkelMesh ? SkelMesh->GetSkeletalMeshAsset() : nullptr;
		const FSkeletalMeshRenderData* RenderData = Asset ? Asset->GetResourceForRendering() : nullptr;
		if (!RenderData || Slots.Num() == 0)
		{
			return;
		}
		for (int32 LOD = 0; LOD < RenderData->LODRenderData.Num(); ++LOD)
		{
			const FSkeletalMeshLODInfo* LODInfo = Asset->GetLODInfo(LOD);
			const TArray<FSkelMeshRenderSection>& Sections = RenderData->LODRenderData[LOD].RenderSections;
			for (int32 Section = 0; Section < Sections.Num(); ++Section)
			{
				int32 MaterialIndex = Sections[Section].MaterialIndex;
				if (LODInfo && LODInfo->LODMaterialMap.IsValidIndex(Section) && LODInfo->LODMaterialMap[Section] != INDEX_NONE)
				{
					MaterialIndex = LODInfo->LODMaterialMap[Section];
				}
				if (Slots.Contains(MaterialIndex))
				{
					SkelMesh->ShowMaterialSection(MaterialIndex, Section, false, LOD);
					FLook::FHiddenSection& Hidden = Look.HiddenSections.AddDefaulted_GetRef();
					Hidden.MaterialIndex = MaterialIndex;
					Hidden.Section = Section;
					Hidden.LOD = LOD;
				}
			}
		}
	}

	/** Gives a slot a dynamic instance of Material (remembering what it had). */
	static UMaterialInstanceDynamic* SwapSlot(UPenguinTraitComponent* Traits, FLook& Look, USkeletalMeshComponent* SkelMesh, int32 Slot, UMaterialInterface* Material)
	{
		if (!Material || Slot < 0 || Slot >= SkelMesh->GetNumMaterials())
		{
			return nullptr;
		}
		UMaterialInterface* Before = SkelMesh->GetMaterial(Slot);
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, SkelMesh);
		Look.SlotsBefore.Add(TPair<int32, TWeakObjectPtr<UMaterialInterface>>(Slot, Before));
		SkelMesh->SetMaterial(Slot, Instance);
		Traits->TraitObjects.Add(Instance);
		return Instance;
	}

	/** The clip (Speedster's beanie, Slow Burn's pom) in the mesh's component space now: it follows the Body bone. */
	static void UpdateClip(const FLook& Look, const USkeletalMeshComponent* SkelMesh)
	{
		if (Look.Clip == FLook::EClip::None || Look.ClipMaterials.Num() == 0 || !Look.Map.bValid)
		{
			return;
		}
		const USkeletalMesh* Asset = SkelMesh->GetSkeletalMeshAsset();
		const FName Body(TEXT("Body"));
		if (!Asset || SkelMesh->GetBoneIndex(Body) == INDEX_NONE)
		{
			return;
		}
		// Rest pose -> where the Body bone is now (component space): everything clipped hangs on that bone.
		const FTransform BodyRef(Asset->GetComposedRefPoseMatrix(Body));
		const FTransform BodyNow = SkelMesh->GetSocketTransform(Body, RTS_Component);
		const FTransform RestToNow = BodyRef.Inverse() * BodyNow;
		const float Unit = Look.Map.Unit * static_cast<float>(RestToNow.GetScale3D().GetAbsMax());
		FLinearColor Sphere(0.f, 0.f, 0.f, 0.f);
		FLinearColor PlanePoint(0.f, 0.f, 0.f, 0.f);
		FLinearColor PlaneNormal(0.f, 0.f, 0.f, 0.f);
		FLinearColor KeepSphere(0.f, 0.f, 0.f, 0.f);
		if (Look.Clip == FLook::EClip::Pom)
		{
			const FVector Centre = RestToNow.TransformPosition(Look.Map.ToMesh(BlendPomCentre));
			Sphere = FLinearColor(Centre.X, Centre.Y, Centre.Z, BlendPomClipRadius * Unit);
		}
		else
		{
			const FVector Front = RestToNow.TransformPosition(Look.Map.ToMesh(BlendCuffFront));
			const FVector Back = RestToNow.TransformPosition(Look.Map.ToMesh(BlendCuffBack));
			const FVector Across = RestToNow.TransformVectorNoScale(Look.Map.MapX.GetSafeNormal());
			FVector Normal = (Across ^ (Back - Front)).GetSafeNormal();
			const FVector Up = RestToNow.TransformVectorNoScale(Look.Map.MapZ.GetSafeNormal());
			Normal = (Normal | Up) < 0.f ? -Normal : Normal;
			PlanePoint = FLinearColor(Front.X, Front.Y, Front.Z, 0.f);
			PlaneNormal = FLinearColor(Normal.X, Normal.Y, Normal.Z, 0.f);
			const FVector Dome = RestToNow.TransformPosition(Look.Map.ToMesh(BlendDomeCentre));
			KeepSphere = FLinearColor(Dome.X, Dome.Y, Dome.Z, BlendDomeKeepRadius * Unit);
		}
		for (int32 Index = 0; Index < Look.ClipMaterials.Num(); ++Index)
		{
			if (UMaterialInstanceDynamic* Material = Look.ClipMaterials[Index].Get())
			{
				// (The ink outline keeps nothing above the cuff's line.)
				const bool bOutline = Material->Parent && Material->Parent->GetName().Contains(TEXT("Outline"));
				Material->SetVectorParameterValue(TEXT("ClipSphere"), Sphere);
				Material->SetVectorParameterValue(TEXT("ClipPlanePoint"), PlanePoint);
				Material->SetVectorParameterValue(TEXT("ClipPlaneNormal"), PlaneNormal);
				Material->SetVectorParameterValue(TEXT("ClipKeepSphere"), bOutline ? FLinearColor(0.f, 0.f, 0.f, 0.f) : KeepSphere);
			}
		}
	}

	/** The penguin's slot colour for pieces lit in the world: its beanie's colour, vivid (the UI colour is too pale). */
	static FLinearColor PieceSlotColor(const APenguinCharacter* Penguin, float Shade = 1.f)
	{
		FLinearColor Color = APenguinCharacter::GetBeanieTint(Penguin->GetColorIndex()) * Shade;
		Color.A = 1.f;
		return Color;
	}

	/** World point of a rest-pose (Blender) point on the Body bone now. */
	static FVector BodyPointNow(const FLook& Look, const USkeletalMeshComponent* SkelMesh, const FVector& Blend)
	{
		const USkeletalMesh* Asset = SkelMesh->GetSkeletalMeshAsset();
		const FName Body(TEXT("Body"));
		if (!Asset || !Look.Map.bValid || SkelMesh->GetBoneIndex(Body) == INDEX_NONE)
		{
			return SkelMesh->GetComponentLocation();
		}
		const FTransform BodyRef(Asset->GetComposedRefPoseMatrix(Body));
		const FTransform BodyNow = SkelMesh->GetSocketTransform(Body, RTS_World);
		return (BodyRef.Inverse() * BodyNow).TransformPosition(Look.Map.ToMesh(Blend));
	}

	/** Quick Draw: a little lightning bolt at Where (world), about Size cm long, gone in a moment. */
	static void SpawnBolt(APinguFX* FX, FRandomStream& Rand, const FVector& Where, float Size, const FVector& Velocity)
	{
		if (!FX)
		{
			return;
		}
		// A zigzag on the play plane, a little toward the camera (+Y): three segments, each a bright core and a soft glow.
		FVector Points[4];
		const float Angle = Rand.FRandRange(0.f, 2.f * UE_PI);
		const FVector Along(FMath::Cos(Angle), 0.f, FMath::Sin(Angle));
		const FVector Side(-Along.Z, 0.f, Along.X);
		Points[0] = Where - Along * (Size * 0.5f) + FVector(0.f, 30.f, 0.f);
		for (int32 Index = 1; Index < 4; ++Index)
		{
			const float Kink = (Index % 2 == 1 ? 1.f : -1.f) * Size * Rand.FRandRange(0.18f, 0.3f);
			Points[Index] = Points[0] + Along * (Size * Index / 3.f) + Side * (Index < 3 ? Kink : 0.f);
		}
		const float Life = Rand.FRandRange(0.13f, 0.2f);
		++MutableCounters().Bolts;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FVector Delta = Points[Index + 1] - Points[Index];
			const float Length = static_cast<float>(Delta.Size());
			if (Length < 1.f)
			{
				continue;
			}
			const FRotator Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Delta / Length).Rotator();
			FPinguParticleDesc Core;
			Core.Mesh = EPinguFXMesh::Puff;
			Core.Location = Points[Index] + Delta * 0.5f;
			Core.Velocity = Velocity;
			Core.Rotation = Rotation;
			Core.Length = Length * 1.12f;
			Core.Size = Size * 0.075f;
			Core.EndSize = Size * 0.05f;
			Core.Curve = EPinguSizeCurve::Linear;
			Core.Life = Life;
			Core.Color = FLinearColor(1.f, 0.92f, 0.35f);
			Core.Emissive = 6.f;
			FX->Spawn(Core);
			FPinguParticleDesc Glow = Core;
			Glow.Mesh = EPinguFXMesh::Glow;
			Glow.Size = Size * 0.32f;
			Glow.EndSize = Size * 0.15f;
			Glow.Length = Length * 1.2f;
			Glow.Opacity = 0.55f;
			Glow.bFadeOut = true;
			Glow.Color = FLinearColor(1.f, 0.85f, 0.2f);
			Glow.Emissive = 3.f;
			FX->Spawn(Glow);
		}
	}
}

using namespace PenguinTraitLooksPrivate;

// ---------------------------------------------------------------------------- building a look

namespace PenguinTraitLooksPrivate
{
	/** Jester's felt cap on the Body bone (PinguJesterHat): its origin on the head where make_jester_hat.py modelled it,
	 *  its axes the FBX's (Unreal gets 100 x, -100 y, 100 z of Pingu.blend: the mesh's MapX, -MapY, MapZ); on Ultra
	 *  effects the one on spring bones. False when the hat's assets are missing. */
	static bool BuildJesterHat(UPenguinTraitComponent* Traits, FLook& Look, USkeletalMeshComponent* SkelMesh)
	{
		const FName Body(TEXT("Body"));
		const USkeletalMesh* Asset = SkelMesh->GetSkeletalMeshAsset();
		if (!Asset || !Look.Map.bValid || SkelMesh->GetBoneIndex(Body) == INDEX_NONE)
		{
			return false;
		}
		const FVector Up = Look.Map.MapZ.GetSafeNormal();
		const FVector Forward = (-Look.Map.MapY - Up * (-Look.Map.MapY | Up)).GetSafeNormal();
		const FVector Across = Forward ^ Up;
		const FQuat Rotation = FMatrix(Across, Forward, Up, FVector::ZeroVector).ToQuat();
		const FTransform InMesh(Rotation, Look.Map.ToMesh(PinguJesterHat::GetBlendPivot()), FVector(Look.Map.Unit / 100.f));
		const FTransform BoneRef(Asset->GetComposedRefPoseMatrix(Body));
		static bool bLogged = false;
		if (!bLogged)
		{
			bLogged = true;
			// (MapX should point along the hat's x: +1)
			UE_LOG(LogPingu, Log, TEXT("[JESTERHAT] on the Body bone: pivot %s in the mesh, across . MapX %.3f, unit %.2f"),
				*InMesh.GetLocation().ToString(), static_cast<float>(Across | Look.Map.MapX.GetSafeNormal()), Look.Map.Unit);
		}
		return PinguJesterHat::Build(Look.Hat, SkelMesh, Body, InMesh.GetRelativeTransform(BoneRef), Look.Quality.bUltra, Look.Quality.bShadows,
			Traits->TraitObjects);
	}

	/** The pieces, materials and overlay of the trait's look (the look is off: ClearLook before). */
	static void BuildLook(UPenguinTraitComponent* Traits, FLook& Look, APenguinCharacter* Penguin, USkeletalMeshComponent* SkelMesh)
	{
		Look.Quality = ReadQuality();
		BuildRigMap(SkelMesh, Look.Map);
		const FPinguTraitInfo& Info = PinguTraits::GetInfo(Look.Trait);
		Look.BodyScale = Info.BodyScale;
		Look.bWantsOverlay = false;
		Look.Clip = FLook::EClip::None;
		if (!Look.Map.bValid)
		{
			return; // not the penguin rig (no mesh yet): only the body's shape
		}
		const FLinearColor TraitColor = Info.Color;
		const FLinearColor Slot = PieceSlotColor(Penguin);
		const FLookQuality& Q = Look.Quality;
		FPinguTraitPiece* P = nullptr;
		switch (Look.Trait)
		{
		case EPinguTrait::Heavyweight:
			// The fat body (BodyScale) and heavier landings (the jelly spring); nothing to wear.
			break;

		case EPinguTrait::QuickDraw:
			// Nothing to wear: little lightning bolts keep popping up round it (TickLooks).
			break;

		case EPinguTrait::SlowBurn:
		{
			// The pom goes; a fuse burns where it sat: a bent wick and an ember at its tip.
			TArray<int32> Pom;
			FindSlots(SkelMesh, TEXT("pom"), Pom);
			HideSlots(Look, SkelMesh, Pom);
			Look.Clip = FLook::EClip::Pom;
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Cylinder, TEXT("Body"), FLinearColor(0.16f, 0.11f, 0.07f), 0.f, 0.9f)) != nullptr)
			{
				P->Location = FVector(0.f, 0.07f, 2.04f);
				P->Rotation = AxisAlong(FVector(0.f, 0.25f, 1.f));
				P->Size = FVector(0.085f, 0.085f, 0.2f);
			}
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Cylinder, TEXT("Body"), FLinearColor(0.16f, 0.11f, 0.07f), 0.f, 0.9f)) != nullptr)
			{
				P->Location = FVector(0.f, 0.17f, 2.2f);
				P->Rotation = AxisAlong(FVector(0.f, 0.85f, 0.6f));
				P->Size = FVector(0.08f, 0.08f, 0.2f);
				P->PopDelay = 0.05f;
			}
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), FLinearColor(1.f, 0.42f, 0.08f), 4.f, 0.4f)) != nullptr)
			{
				P->Location = FVector(0.f, 0.26f, 2.27f);
				P->Size = FVector(0.15f);
				P->PopDelay = 0.1f;
			}
			break;
		}

		case EPinguTrait::IceTouch:
		{
			// Ice crystals growing out of the upper back and the back of the beanie (they stick out of the silhouette).
			const FLinearColor Ice(0.22f, 0.66f, 1.f);
			struct FCrystal
			{
				FVector Where;
				FVector Dir;
				float Length;
			};
			const FCrystal Crystals[] = {
				{ FVector(-0.22f, 0.4f, 1.45f), FVector(-0.4f, 0.8f, 0.6f), 0.8f },
				{ FVector(0.22f, 0.4f, 1.45f), FVector(0.4f, 0.8f, 0.6f), 0.8f },
				{ FVector(0.f, 0.3f, 1.9f), FVector(0.f, 0.55f, 1.f), 0.72f },
			};
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Crystals); ++Index)
			{
				if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Shard, TEXT("Body"), Ice, 1.6f, 0.1f)) != nullptr)
				{
					P->Rotation = XAlong(Crystals[Index].Dir, FVector(0.f, -1.f, 0.f));
					SetUniformLength(*P, Crystals[Index].Length);
					// The crystal's base sits on the body: its middle half its length out.
					P->Location = Crystals[Index].Where + Crystals[Index].Dir.GetSafeNormal() * (Crystals[Index].Length * 0.42f);
					P->PopDelay = 0.06f * Index;
				}
			}
			Look.bWantsOverlay = Q.HasExtraOverlays();
			break;
		}

		case EPinguTrait::Featherweight:
			// Two small white wings on the back (they flap in the air: TickLooks).
			for (const float Side : { -1.f, 1.f })
			{
				if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), FLinearColor(0.88f, 0.94f, 1.f), 0.3f, 0.6f)) != nullptr)
				{
					P->Size = FVector(0.13f, 0.98f, 0.5f);
					P->PopDelay = Side > 0.f ? 0.05f : 0.f;
				}
			}
			break;

		case EPinguTrait::Tiny:
		{
			// A big pink bow on the front of the beanie: two loops and the knot.
			const FLinearColor Pink(1.f, 0.32f, 0.66f);
			for (const float Side : { -1.f, 1.f })
			{
				if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), Pink, 0.15f, 0.45f)) != nullptr)
				{
					P->Location = FVector(Side * 0.24f, -0.22f, 2.f);
					P->Rotation = AxisAlong(FVector(Side, 0.f, 0.4f));
					P->Size = FVector(0.3f, 0.16f, 0.5f);
					P->PopDelay = Side > 0.f ? 0.05f : 0.f;
				}
			}
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), Pink * 0.8f, 0.1f, 0.45f)) != nullptr)
			{
				P->Location = FVector(0.f, -0.26f, 1.97f);
				P->Size = FVector(0.2f);
				P->PopDelay = 0.1f;
			}
			break;
		}

		case EPinguTrait::Giant:
			// Nothing to wear: its size and a body stretched a little taller and slimmer (BodyScale) are its look.
			break;

		case EPinguTrait::GlassCannon:
		{
			// Glass in the penguin's own colour: a see-through tinted sheen over it (every setting) and glossy clay.
			Look.bWantsOverlay = true;
			if (Q.Detail >= EPinguQualityLevel::Medium)
			{
				TArray<int32> Clay;
				FindSlots(SkelMesh, TEXT("clay"), Clay);
				FindSlots(SkelMesh, TEXT("orange"), Clay);
				FindSlots(SkelMesh, TEXT("white"), Clay, TEXT("lowmerged"));
				for (const int32 Index : Clay)
				{
					if (UMaterialInstanceDynamic* Glassy = SwapSlot(Traits, Look, SkelMesh, Index, SkelMesh->GetMaterial(Index)))
					{
						Glassy->SetScalarParameterValue(TEXT("Roughness"), 0.08f);
						Glassy->SetScalarParameterValue(TEXT("Specular"), 1.f);
					}
				}
			}
			break;
		}

		case EPinguTrait::Rubber:
			// A yellow band round the middle like the bouncy ball of its badge (and the wobble, TickLooks).
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Ring, TEXT("Body"), FLinearColor(1.f, 0.78f, 0.12f), 0.2f, 0.3f)) != nullptr)
			{
				P->Location = FVector(0.f, 0.f, 0.6f);
				P->Rotation = FQuat(FVector(1.f, 0.f, 0.f), FMath::DegreesToRadians(90.f)); // the ring's axis (its Y) up
				P->Size = FVector(1.08f, 0.2f, 1.08f);
			}
			Look.bWantsOverlay = Q.HasExtraOverlays();
			break;

		case EPinguTrait::SecondChance:
			// A fish riding on the beanie (it leaps off when it saves the penguin).
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Fish, TEXT("Body"), FLinearColor::White, 0.f, 0.5f, true)) != nullptr)
			{
				P->Location = FVector(0.f, -0.02f, 2.2f);
				P->Rotation = XAlong(FVector(0.f, -1.f, 0.f));
				SetUniformLength(*P, 0.82f);
			}
			break;

		case EPinguTrait::Leech:
			// A vampire: fangs under the beak and a high wine-red collar standing up behind the head.
			for (const float Side : { -1.f, 1.f })
			{
				if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Cone, TEXT("Body"), FLinearColor(0.98f, 0.97f, 0.94f), 0.25f, 0.3f)) != nullptr)
				{
					P->Location = FVector(Side * 0.08f, -0.57f, 1.05f);
					P->Rotation = AxisAlong(FVector(0.f, 0.f, -1.f));
					P->Size = FVector(0.11f, 0.11f, 0.24f);
					P->PopDelay = Side > 0.f ? 0.04f : 0.f;
				}
			}
			for (const float Side : { -1.f, 1.f })
			{
				if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), FLinearColor(0.5f, 0.02f, 0.06f), 0.12f, 0.35f)) != nullptr)
				{
					P->Location = FVector(Side * 0.26f, 0.44f, 1.62f);
					// Its flat side faces forward and out; it leans back and out to its side, above the shoulders.
					P->Rotation = FRotationMatrix::MakeFromXZ(FRigMap::BlendDirToFrame(FVector(Side * 0.45f, -0.9f, 0.f)).GetSafeNormal(),
						FRigMap::BlendDirToFrame(FVector(Side * 0.45f, 0.55f, 1.f)).GetSafeNormal()).ToQuat();
					P->Size = FVector(0.08f, 0.74f, 0.98f);
					P->PopDelay = 0.1f;
				}
			}
			break;

		case EPinguTrait::Gambler:
			// Two dice circling the head.
			for (int32 Index = 0; Index < 2; ++Index)
			{
				if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Cube, TEXT("Body"), FLinearColor(0.8f, 0.04f, 0.06f), 0.25f, 0.2f)) != nullptr)
				{
					P->Size = FVector(0.27f);
					P->PopDelay = 0.06f * Index;
				}
			}
			break;

		case EPinguTrait::Speedster:
		{
			// No beanie: a thick, rounded headband in the penguin's colour round the upper head (above the eyes), tied at the
			// back, its two ends flying behind.
			TArray<int32> Beanie;
			FindSlots(SkelMesh, TEXT("felt"), Beanie);
			FindSlots(SkelMesh, TEXT("wool"), Beanie);
			FindSlots(SkelMesh, TEXT("pom"), Beanie);
			HideSlots(Look, SkelMesh, Beanie);
			Look.Clip = FLook::EClip::Beanie;
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Ring, TEXT("Body"), Slot, 0.1f, 0.55f)) != nullptr)
			{
				// SM_Ring is a torus round its Y axis: that axis up (tipped back a little), round the head, a tall rounded tube.
				P->Location = BlendBandCentre;
				P->Rotation = FQuat::FindBetweenNormals(FVector(0.f, 1.f, 0.f), FRigMap::BlendDirToFrame(BlendBandAxis).GetSafeNormal());
				P->Size = FVector(BlendBandWidth, BlendBandHeight, BlendBandWidth);
			}
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), Slot * 0.85f, 0.1f, 0.55f)) != nullptr)
			{
				// The knot at the back.
				P->Location = BlendBandKnot;
				P->Size = FVector(0.17f, 0.12f, 0.15f);
				P->PopDelay = 0.05f;
			}
			for (const float Side : { -1.f, 1.f })
			{
				if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), Slot * 0.92f, 0.1f, 0.55f)) != nullptr)
				{
					P->Size = FVector(0.06f, 0.44f, 0.13f);
					P->PopDelay = Side > 0.f ? 0.12f : 0.08f;
				}
			}
			break;
		}

		case EPinguTrait::HotFeet:
			// The feet glow red hot, little flames licking up off them.
			for (const TCHAR* Bone : { TEXT("Foot_L"), TEXT("Foot_R") })
			{
				const float Side = FCString::Strcmp(Bone, TEXT("Foot_L")) == 0 ? -1.f : 1.f;
				if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, Bone, FLinearColor(1.f, 0.22f, 0.03f), 3.f, 0.5f)) != nullptr)
				{
					P->Location = FVector(Side * 0.234f, -0.26f, 0.31f);
					P->Size = FVector(0.62f, 0.74f, 0.32f);
					P->PopDelay = Side > 0.f ? 0.05f : 0.f;
				}
			}
			for (const TCHAR* Bone : { TEXT("Foot_L"), TEXT("Foot_R") })
			{
				const float Side = FCString::Strcmp(Bone, TEXT("Foot_L")) == 0 ? -1.f : 1.f;
				if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Cone, Bone, FLinearColor(1.f, 0.72f, 0.12f), 5.f, 0.5f)) != nullptr)
				{
					P->Location = FVector(Side * 0.234f, -0.3f, 0.66f);
					P->Size = FVector(0.34f, 0.34f, 0.6f);
					P->PopDelay = 0.1f + (Side > 0.f ? 0.05f : 0.f);
				}
			}
			break;

		case EPinguTrait::Mirror:
			// A silver disc on the chest (it shines while a shot can be sent back) and a silver sheen over the penguin.
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Cylinder, TEXT("Body"), FLinearColor(0.82f, 0.86f, 0.95f), 2.f, 0.08f)) != nullptr)
			{
				P->Location = FVector(0.f, -0.57f, 0.74f);
				P->Rotation = AxisAlong(FVector(0.f, -1.f, 0.15f));
				P->Size = FVector(0.5f, 0.5f, 0.07f);
			}
			Look.bWantsOverlay = true;
			break;

		case EPinguTrait::Sticky:
			// Green slime dripping off the flippers and the beanie.
			for (const TCHAR* Bone : { TEXT("Flipper_L"), TEXT("Flipper_R") })
			{
				const float Side = FCString::Strcmp(Bone, TEXT("Flipper_L")) == 0 ? -1.f : 1.f;
				if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, Bone, FLinearColor(0.35f, 0.9f, 0.12f), 0.35f, 0.15f)) != nullptr)
				{
					P->Location = FVector(Side * 0.64f, -0.06f, 0.6f);
					P->Size = FVector(0.17f, 0.17f, 0.3f);
					P->PopDelay = Side > 0.f ? 0.05f : 0.f;
				}
			}
			// A blob of goo on the beanie, one drip running down its front.
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), FLinearColor(0.35f, 0.9f, 0.12f), 0.35f, 0.15f)) != nullptr)
			{
				P->Location = FVector(0.f, 0.02f, 1.97f);
				P->Rotation = AxisAlong(FVector(0.f, 0.2f, 1.f));
				P->Size = FVector(0.66f, 0.62f, 0.24f);
				P->PopDelay = 0.1f;
			}
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), FLinearColor(0.35f, 0.9f, 0.12f), 0.35f, 0.15f)) != nullptr)
			{
				P->Location = FVector(0.18f, -0.34f, 1.84f);
				P->Size = FVector(0.13f, 0.13f, 0.3f);
				P->PopDelay = 0.15f;
			}
			break;

		case EPinguTrait::Jester:
		{
			// Just a jester's hat: no beanie (hidden like Speedster's), a felt jester cap (PinguJesterHat: a crown in a rolled
			// band, three floppy points in shades of the penguin's colour, a gold bell on each; on Ultra its points flop on
			// spring bones).
			TArray<int32> Beanie;
			FindSlots(SkelMesh, TEXT("felt"), Beanie);
			FindSlots(SkelMesh, TEXT("wool"), Beanie);
			FindSlots(SkelMesh, TEXT("pom"), Beanie);
			HideSlots(Look, SkelMesh, Beanie);
			Look.Clip = FLook::EClip::Beanie;
			if (BuildJesterHat(Traits, Look, SkelMesh))
			{
				break;
			}
			// (The hat's assets are missing - the jesterhat import phase: a cap of engine shapes instead, the points and the
			// bells one instanced piece each.)
			if ((P = AddPiece(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), Slot, 0.06f, 0.65f)) != nullptr)
			{
				// The cap: over the head's top, tipped back like the beanie sat, its front edge above the eyes.
				P->Location = FVector(0.f, 0.08f, 1.69f);
				P->Rotation = AxisAlong(FVector(0.f, 0.25f, 1.f));
				P->Size = FVector(1.03f, 1.05f, 0.68f);
			}
			struct FPoint
			{
				FVector Base;
				FVector Dir;
				float Shade;
			};
			const FPoint Points[] = {
				{ FVector(-0.24f, 0.06f, 1.9f), FVector(-1.f, 0.3f, 0.32f), 1.f },
				{ FVector(0.f, 0.2f, 1.97f), FVector(0.f, 1.f, 0.65f), 0.6f },
				{ FVector(0.24f, 0.06f, 1.9f), FVector(1.f, 0.3f, 0.32f), 1.f },
			};
			constexpr float Length = 0.72f;
			TArray<FVector> BellPlaces;
			if (FLook::FInstanced* Tips = AddInstanced(Traits, Look, SkelMesh, EShape::Cone, TEXT("Body"), 0.08f, 0.06f))
			{
				for (const FPoint& Point : Points)
				{
					const FVector Dir = Point.Dir.GetSafeNormal();
					Tips->Locations.Add(Point.Base + Dir * (Length * 0.5f));
					Tips->Rotations.Add(AxisAlong(Dir));
					Tips->Sizes.Add(FVector(0.36f, 0.36f, Length));
					Tips->Colors.Add(PieceSlotColor(Penguin, Point.Shade));
					BellPlaces.Add(Point.Base + Dir * (Length + 0.04f) - FVector(0.f, 0.f, 0.05f));
				}
			}
			if (FLook::FInstanced* Bells = AddInstanced(Traits, Look, SkelMesh, EShape::Sphere, TEXT("Body"), 0.6f, 0.15f))
			{
				Bells->bSwing = true;
				for (const FVector& Place : BellPlaces)
				{
					Bells->Locations.Add(Place);
					Bells->Rotations.Add(FQuat::Identity);
					Bells->Sizes.Add(FVector(0.2f));
					Bells->Colors.Add(FLinearColor(1.f, 0.7f, 0.1f));
				}
			}
			break;
		}

		default:
			break;
		}

		// The clip materials on the ink outline and LOD2 / LOD3's merged slot (Speedster, Slow Burn).
		if (Look.Clip != FLook::EClip::None)
		{
			struct FClipSlot
			{
				const TCHAR* Piece;
				const TCHAR* Material;
			};
			const FClipSlot ClipSlots[] = {
				{ TEXT("outline"), TEXT("/Game/Penguin/Materials/M_Outline_Clip.M_Outline_Clip") },
				{ TEXT("lowmerged"), TEXT("/Game/Penguin/Materials/M_Penguin_LowMerged_Clip.M_Penguin_LowMerged_Clip") },
			};
			for (const FClipSlot& ClipSlot : ClipSlots)
			{
				TArray<int32> Slots;
				FindSlots(SkelMesh, ClipSlot.Piece, Slots);
				UMaterialInterface* ClipMaterial = LoadMaterial(ClipSlot.Material);
				for (const int32 Index : Slots)
				{
					if (UMaterialInstanceDynamic* Instance = SwapSlot(Traits, Look, SkelMesh, Index, ClipMaterial))
					{
						Look.ClipMaterials.Add(Instance);
					}
				}
			}
		}

		// The overlay: a see-through sheen (M_Ghost, like the dash afterimages) in the trait's tint.
		if (Look.bWantsOverlay)
		{
			UMaterialInstanceDynamic* Overlay = Look.Overlay.Get();
			if (!Overlay)
			{
				if (UMaterialInterface* Ghost = LoadMaterial(TEXT("/Game/FX/M_Ghost.M_Ghost")))
				{
					Overlay = UMaterialInstanceDynamic::Create(Ghost, Penguin);
					Traits->TraitObjects.Add(Overlay);
					Look.Overlay = Overlay;
				}
			}
			if (Overlay)
			{
				FLinearColor Tint = TraitColor;
				float Opacity = 0.3f;
				switch (Look.Trait)
				{
				case EPinguTrait::GlassCannon:
					Tint = PieceSlotColor(Penguin);
					Opacity = Q.Detail <= EPinguQualityLevel::Low ? 0.55f : 0.4f;
					break;
				case EPinguTrait::Mirror:
					Tint = FLinearColor(0.85f, 0.9f, 1.f);
					Opacity = 0.3f;
					break;
				case EPinguTrait::IceTouch:
					Tint = FLinearColor(0.55f, 0.9f, 1.f);
					Opacity = 0.2f;
					break;
				case EPinguTrait::Rubber:
					Tint = FLinearColor(1.f, 0.25f, 0.6f);
					Opacity = 0.18f;
					break;
				default:
					break;
				}
				Overlay->SetVectorParameterValue(TEXT("Color"), Tint);
				Overlay->SetScalarParameterValue(TEXT("Opacity"), Opacity);
			}
			else
			{
				Look.bWantsOverlay = false;
			}
		}
	}

	/** Takes everything the look changed off the penguin again (the overlay goes in KeepOverlay). */
	static void ClearLook(UPenguinTraitComponent* Traits, FLook& Look, APenguinCharacter* Penguin)
	{
		USkeletalMeshComponent* SkelMesh = Penguin ? Penguin->GetMesh() : nullptr;
		for (FPinguTraitPiece& Piece : Look.Pieces)
		{
			if (UStaticMeshComponent* Component = Piece.Component.Get())
			{
				Traits->TraitObjects.RemoveSingle(Component);
				Component->DestroyComponent();
			}
			if (UMaterialInstanceDynamic* Material = Piece.Material.Get())
			{
				Traits->TraitObjects.RemoveSingle(Material);
			}
		}
		Look.Pieces.Reset();
		for (FLook::FInstanced& Group : Look.Instanced)
		{
			if (UInstancedStaticMeshComponent* Component = Group.Component.Get())
			{
				Traits->TraitObjects.RemoveSingle(Component);
				Component->DestroyComponent();
			}
		}
		Look.Instanced.Reset();
		PinguJesterHat::Destroy(Look.Hat, Traits->TraitObjects);
		if (SkelMesh)
		{
			// The slots' materials back (the latest change first), then the hidden sections.
			for (int32 Index = Look.SlotsBefore.Num() - 1; Index >= 0; --Index)
			{
				const int32 Slot = Look.SlotsBefore[Index].Key;
				if (UMaterialInterface* Current = SkelMesh->GetMaterial(Slot))
				{
					Traits->TraitObjects.RemoveSingle(Current);
				}
				SkelMesh->SetMaterial(Slot, Look.SlotsBefore[Index].Value.Get());
			}
			for (const FLook::FHiddenSection& Hidden : Look.HiddenSections)
			{
				SkelMesh->ShowMaterialSection(Hidden.MaterialIndex, Hidden.Section, true, Hidden.LOD);
			}
		}
		Look.SlotsBefore.Reset();
		Look.HiddenSections.Reset();
		Look.ClipMaterials.Reset();
		Look.Clip = FLook::EClip::None;
		Look.bWantsOverlay = false;
		Look.BodyScale = FVector::OneVector;
		Look.BoneShape = FVector::OneVector;
		Look.bApplied = false;
		Look.Jelly = Look.JellyVelocity = 0.f;
		Look.FishLeapAge = -1.f;
		Look.DiceRollAge = 1000.f;
		if (Penguin)
		{
			Penguin->SetTraitBodyScale(FVector::OneVector);
		}
	}

	/** The overlay as the look wants it - never over the snow bomb's soot (the death look restores what it found). */
	static void UpdateOverlay(FLook& Look, APenguinCharacter* Penguin)
	{
		USkeletalMeshComponent* SkelMesh = Penguin->GetMesh();
		if (!SkelMesh || Penguin->IsDeathSootShown())
		{
			return;
		}
		UMaterialInstanceDynamic* Ours = Look.Overlay.Get();
		UMaterialInterface* Current = SkelMesh->GetOverlayMaterial();
		UMaterialInterface* Wanted = Look.bApplied && Look.bWantsOverlay ? Ours : nullptr;
		if (Current == Wanted)
		{
			return;
		}
		if (Wanted)
		{
			if (!Current || Current == Ours)
			{
				SkelMesh->SetOverlayMaterial(Wanted);
			}
		}
		else if (Current && Current == Ours)
		{
			SkelMesh->SetOverlayMaterial(nullptr);
		}
	}

	/** The pieces' places this frame: the pop in, and whatever moves (wings, dice, the fish, the ember, the headband's ends). */
	static void AnimatePieces(UPenguinTraitComponent* Traits, FLook& Look, APenguinCharacter* Penguin, float Dt)
	{
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const bool bAir = Move && Move->IsFalling();
		const bool bHidePieces = Penguin->IsDeathLookActive();
		const float Clock = Look.Clock;
		const FVector Velocity = Penguin->GetVelocity();
		for (int32 Index = 0; Index < Look.Pieces.Num(); ++Index)
		{
			FPinguTraitPiece& Piece = Look.Pieces[Index];
			UStaticMeshComponent* Component = Piece.Component.Get();
			if (!Component)
			{
				continue;
			}
			float Grow = ElasticOut(FMath::Clamp((Look.Age - Piece.PopDelay) / PopTime, 0.f, 1.f));
			bool bShow = !bHidePieces && Grow > 0.01f;
			switch (Look.Trait)
			{
			case EPinguTrait::Featherweight:
			{
				// Each wing out of the upper back, to its side and back; flapping fast in the air, a slow flutter standing.
				const float Side = Index == 0 ? -1.f : 1.f;
				const float Flap = bAir ? 34.f * FMath::Sin(Clock * 2.f * UE_PI * 5.f) : 8.f * FMath::Sin(Clock * 2.f * UE_PI * 0.8f + Side);
				const FVector Out = FVector(Side * 0.45f, 0.8f, 0.55f + 0.014f * Flap).GetSafeNormal();
				Piece.Location = FVector(Side * 0.15f, 0.42f, 1.25f) + Out * 0.42f;
				// The sphere's long axis (its Y, Size 0.72) along Out, its flat side (X) facing back.
				Piece.Rotation = FRotationMatrix::MakeFromYZ(FRigMap::BlendDirToFrame(Out), FVector(0.f, 0.f, 1.f)).ToQuat();
				break;
			}
			case EPinguTrait::Gambler:
			{
				// Two dice circling the head (front, side, back), tumbling - fast for a moment after a roll.
				const float Phase = Clock * 1.6f + Index * UE_PI;
				const float Radius = 0.62f;
				Piece.Location = FVector(FMath::Sin(Phase) * Radius * 0.75f, FMath::Cos(Phase) * Radius, 2.05f + 0.06f * FMath::Sin(Phase * 2.f));
				const float RollSpeed = Look.DiceRollAge < 0.6f ? 1440.f * (1.f - Look.DiceRollAge / 0.6f) : 0.f;
				const float Turn = Clock * 70.f + RollSpeed * Look.DiceRollAge + Index * 45.f;
				Piece.Rotation = FRotator(Turn * 0.7f, Turn, Turn * 0.4f).Quaternion();
				break;
			}
			case EPinguTrait::SecondChance:
			{
				// The fish rides on the beanie, its tail wiggling; when it saved the penguin it leaps off (an arc up and away,
				// spinning) and is gone.
				const float Wiggle = 12.f * FMath::Sin(Clock * 2.f * UE_PI * 2.2f);
				if (Look.FishLeapAge >= 0.f)
				{
					const float T = Look.FishLeapAge;
					bShow = bShow && T < 0.9f;
					Piece.Location = FVector(0.f, -0.02f + 1.6f * T, 2.2f + 3.2f * T - 5.f * T * T);
					Piece.Rotation = FQuat(FVector(1.f, 0.f, 0.f), FMath::DegreesToRadians(T * 900.f)) * XAlong(FVector(0.f, 1.f, 0.4f));
					Grow *= FMath::Clamp(1.f - (T - 0.6f) / 0.3f, 0.f, 1.f);
				}
				else
				{
					Piece.Location = FVector(0.f, -0.02f, 2.2f);
					Piece.Rotation = XAlong(FVector(0.f, -1.f, 0.f)) * FQuat(FVector(0.f, 0.f, 1.f), FMath::DegreesToRadians(Wiggle));
				}
				break;
			}
			case EPinguTrait::SlowBurn:
				if (Index == 2)
				{
					// The ember pulses.
					const float Pulse = 0.5f + 0.5f * FMath::Sin(Clock * 2.f * UE_PI * 3.f);
					Grow *= 0.85f + 0.3f * Pulse;
					if (UMaterialInstanceDynamic* Material = Piece.Material.Get())
					{
						Material->SetScalarParameterValue(TEXT("Emissive"), 3.f + 5.f * Pulse);
					}
				}
				break;
			case EPinguTrait::Speedster:
				if (Index >= 2)
				{
					// The headband's two ends fly out behind the knot, further and flapping faster with speed, a little apart.
					const float Speed = FMath::Clamp(static_cast<float>(FMath::Abs(Velocity.X)) / 1200.f, 0.f, 1.f);
					const float Wave = FMath::Sin(Clock * (6.f + 16.f * Speed) + Index * 1.3f);
					const float Lift = (Index == 2 ? 0.2f : -0.1f) + 0.12f * Wave;
					const float Apart = Index == 2 ? -0.12f : 0.12f;
					const FVector Dir = FVector(Apart, 1.f, Lift - 0.45f * (1.f - Speed)).GetSafeNormal();
					Piece.Location = BlendBandKnot + FVector(0.f, 0.04f, 0.f) + Dir * 0.22f;
					// Its long axis (Y) along Dir, flat side facing out to the side; twisting a little as it flaps.
					Piece.Rotation = FRotationMatrix::MakeFromYZ(FRigMap::BlendDirToFrame(Dir), FVector(0.f, 0.f, 1.f)).ToQuat()
						* FQuat(FVector(0.f, 1.f, 0.f), FMath::DegreesToRadians(25.f * Wave));
				}
				break;
			case EPinguTrait::Mirror:
				if (UMaterialInstanceDynamic* Material = Piece.Material.Get())
				{
					Material->SetScalarParameterValue(TEXT("Emissive"), Look.bMirrorReady ? 1.6f + 0.5f * FMath::Sin(Clock * 5.f) : 0.f);
				}
				break;
			case EPinguTrait::HotFeet:
				if (Index < 2)
				{
					if (UMaterialInstanceDynamic* Material = Piece.Material.Get())
					{
						Material->SetScalarParameterValue(TEXT("Emissive"), 2.5f + 1.5f * FMath::Sin(Clock * 7.f + Index));
					}
				}
				else
				{
					// The flames flicker.
					const float Flicker = 0.75f + 0.25f * FMath::Sin(Clock * 23.f + Index * 1.7f) + 0.12f * FMath::Sin(Clock * 41.f + Index);
					Piece.Size = FVector(0.34f * (1.1f - 0.2f * Flicker), 0.34f * (1.1f - 0.2f * Flicker), 0.6f * Flicker);
					Piece.Location.Z = 0.44f + 0.25f * Flicker;
				}
				break;
			case EPinguTrait::Sticky:
			{
				// The drips stretch slowly and snap back (the blob on the beanie wobbles).
				const float Drip = FMath::Frac(Clock * 0.45f + Index * 0.37f);
				if (Index == 2)
				{
					Piece.Size.Z = 0.24f * (1.f + 0.08f * FMath::Sin(Clock * 5.f));
				}
				else
				{
					Piece.Size.Z = (Index == 3 ? 0.3f : 0.3f) * (1.f + 0.6f * Drip);
				}
				break;
			}
			default:
				break;
			}
			if (Component->bHiddenInGame == bShow)
			{
				Component->SetHiddenInGame(!bShow);
			}
			if (bShow)
			{
				PlacePiece(Look.Map, Piece, Grow);
			}
		}
		// The instanced pieces (Jester's points and bells): every instance placed like a piece, popping in with its group.
		for (FLook::FInstanced& Group : Look.Instanced)
		{
			UInstancedStaticMeshComponent* Component = Group.Component.Get();
			if (!Component)
			{
				continue;
			}
			const float Grow = ElasticOut(FMath::Clamp((Look.Age - Group.PopDelay) / PopTime, 0.f, 1.f));
			const bool bShow = !bHidePieces && Grow > 0.01f && Look.Map.bValid;
			if (Component->bHiddenInGame == bShow)
			{
				Component->SetHiddenInGame(!bShow);
			}
			if (!bShow)
			{
				continue;
			}
			TArray<FTransform> Places;
			for (int32 Index = 0; Index < Group.Locations.Num(); ++Index)
			{
				const FQuat Rotation = Look.Map.Frame * Group.Rotations[Index];
				const FVector Scale = Group.Sizes[Index] * Look.Map.Unit / Group.MeshSize * Grow;
				FVector Where = Group.Locations[Index];
				if (Group.bSwing)
				{
					Where.Z += 0.015f * FMath::Sin(Clock * 9.f + Index * 2.f);
				}
				const FVector Location = Look.Map.ToMesh(Where) - Rotation.RotateVector(Group.MeshCentre * Scale);
				Places.Add(FTransform(Rotation, Location, Scale).GetRelativeTransform(Group.BoneRef));
			}
			if (Component->GetInstanceCount() != Places.Num())
			{
				Component->ClearInstances();
				Component->SetNumCustomDataFloats(5);
				for (int32 Index = 0; Index < Places.Num(); ++Index)
				{
					const int32 Instance = Component->AddInstance(Places[Index], false);
					const FLinearColor Color = Group.Colors.IsValidIndex(Index) ? Group.Colors[Index] : FLinearColor::White;
					const float Data[5] = { Color.R, Color.G, Color.B, Group.Emissive, 1.f };
					Component->SetCustomData(Instance, MakeArrayView(Data, 5), false);
				}
				Component->MarkRenderStateDirty();
			}
			else
			{
				Component->BatchUpdateInstancesTransforms(0, Places, false, true);
			}
		}
		// Jester's cap: popping in with the look, hidden with the pieces, the penguin's LOD (its outline stays hidden).
		if (Look.Hat.Component.IsValid())
		{
			const float Grow = ElasticOut(FMath::Clamp(Look.Age / PopTime, 0.f, 1.f));
			PinguJesterHat::Update(Look.Hat, Penguin->GetMesh(), Grow, !bHidePieces && Look.Map.bValid);
		}
		(void)Traits;
	}

	/** The body's shape: the trait's (popping in elastically at the transform) and the jelly spring's squash. */
	static void UpdateBody(FLook& Look, APenguinCharacter* Penguin, float Dt)
	{
		// The jelly spring: Rubber wobbles long and loose, everybody else settles quickly.
		const bool bRubber = Look.Trait == EPinguTrait::Rubber;
		const float Stiffness = bRubber ? 250.f : 420.f;
		const float Damping = bRubber ? 8.f : 26.f;
		if (Dt > 0.f)
		{
			Look.JellyVelocity += (-Stiffness * Look.Jelly - Damping * Look.JellyVelocity) * Dt;
			Look.Jelly = FMath::Clamp(Look.Jelly + Look.JellyVelocity * Dt, -0.45f, 0.45f);
		}
		// The transform's pop: the shape eases in with an elastic overshoot, plus a squash-and-stretch boing.
		const float P = FMath::Clamp(Look.Age / PopTime, 0.f, 1.f);
		// The shape goes to the Body bone (PenguinAnimInstance: the eyes, flippers, tail and feet keep their own shape); the
		// mesh only gets the squash and stretch.
		const FVector Shape = FMath::Lerp(FVector::OneVector, Look.BodyScale, ElasticOut(P));
		Look.BoneShape = Shape.Equals(FVector::OneVector, 1.e-4) ? FVector::OneVector : Shape;
		const float Boing = Look.Age < PopTime ? 0.24f * FMath::Exp(-6.f * Look.Age) * FMath::Sin(Look.Age * 26.f) : 0.f;
		const float S = Look.Jelly + Boing;
		const FVector Squash(1.f - 0.5f * S, 1.f - 0.5f * S, 1.f + S);
		Penguin->SetTraitBodyScale(Squash.Equals(FVector::OneVector, 1.e-4) ? FVector::OneVector : Squash);
	}

	/** Idle effects, sounds and the reactions to the trait's events (local on every machine). */
	static void UpdateEffects(UPenguinTraitComponent* Traits, FLook& Look, APenguinCharacter* Penguin, float Dt)
	{
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		USkeletalMeshComponent* SkelMesh = Penguin->GetMesh();
		if (!Move || !SkelMesh)
		{
			return;
		}
		const FPinguTraitEvents& Events = Traits->GetEvents();
		const APenguinGameState* GameState = APenguinGameState::Get(Traits);
		const double Now = GameState ? GameState->GetFlowTime() : 0.0;
		const bool bVisible = !Penguin->IsHidden() && !Penguin->IsKnockedOut() && !Penguin->IsDeathLookActive();
		const FLookQuality& Q = Look.Quality;
		const float SizeScale = Penguin->GetSizeScale();
		const FVector Feet = Penguin->GetFeetLocation();
		const FVector Velocity = Penguin->GetVelocity();
		APinguFX* FX = APinguFX::Get(Penguin);

		// Movement events (every machine has the counters).
		const bool bLanded = Move->LandEvents != Look.SeenLand;
		const bool bJumped = Move->JumpEvents != Look.SeenJump || Move->AirJumpEvents != Look.SeenAirJump;
		const float LandSpeed = Move->LastLandSpeed;
		Look.SeenLand = Move->LandEvents;
		Look.SeenJump = Move->JumpEvents;
		Look.SeenAirJump = Move->AirJumpEvents;

		// Gambler's rolls, Jester's swaps, Second Chance's fish, Mirror's readiness, Leech's cooldown cuts.
		const bool bGamble = Events.GambleSerial != Look.SeenGamble;
		Look.SeenGamble = Events.GambleSerial;
		const bool bSwapped = Events.JesterSwaps != Look.SeenSwaps;
		Look.SeenSwaps = Events.JesterSwaps;
		const bool bRescued = Events.SecondChanceAt >= 0.f && Events.SecondChanceAt != Look.SeenSecondChance;
		Look.SeenSecondChance = Events.SecondChanceAt;
		const bool bMirrorReady = Now >= Events.MirrorReadyAt;
		const bool bMirrorBack = bMirrorReady && !Look.bMirrorReady;
		Look.bMirrorReady = bMirrorReady;
		bool bLeeched = false;
		if (const UPenguinAbilityComponent* Abilities = Penguin->GetAbilities())
		{
			for (int32 Slot = 0; Slot < 3; ++Slot)
			{
				const float Left = Abilities->GetCooldownRemaining(Slot);
				bLeeched |= Look.CooldownSeen[Slot] - Left > 1.f;
				Look.CooldownSeen[Slot] = Left;
			}
		}
		const bool bCling = Move->IsWallClinging();
		const bool bClingStart = bCling && !Look.bClinging;
		Look.bClinging = bCling;
		Look.DiceRollAge += Dt;
		if (bGamble)
		{
			Look.DiceRollAge = 0.f;
			Look.bDiceDouble = Events.bGambleDouble;
		}
		if (Look.FishLeapAge >= 0.f)
		{
			Look.FishLeapAge += Dt;
		}
		if (bRescued)
		{
			// The leap is timed from the rescue on the flow clock (a late look: the fish is long gone).
			Look.FishLeapAge = FMath::Max(static_cast<float>(Now - Events.SecondChanceAt), 0.f);
		}

		if (!bVisible)
		{
			return;
		}
		const float Density = UPinguSettings::GetFXDensity();
		const FLinearColor TraitColor = PinguTraits::GetInfo(Look.Trait).Color;
		Look.FxTimer -= Dt;
		Look.FxTimer2 -= Dt;
		switch (Look.Trait)
		{
		case EPinguTrait::Heavyweight:
			if (bLanded && LandSpeed > 300.f)
			{
				// Lands heavy: squashes 1.6 times as far as anybody, dust when it was a big drop.
				Look.JellyVelocity -= 4.f * FMath::GetMappedRangeValueClamped(FVector2f(300.f, 2200.f), FVector2f(0.4f, 1.f), LandSpeed);
				if (Q.HasIdleFX() && LandSpeed > 600.f)
				{
					APinguFX::GroundPuffs(Penguin, Feet, FMath::Clamp(LandSpeed / 1400.f, 0.5f, 1.4f) * SizeScale);
					++MutableCounters().IdleEffects;
				}
			}
			break;

		case EPinguTrait::QuickDraw:
			// Charged with electricity: little bolts pop up round it and vanish (a few on Low too).
			if (Look.FxTimer <= 0.f)
			{
				Look.FxTimer = Look.Rand.FRandRange(0.1f, 0.22f) / FMath::Max(Density, 0.5f);
				const FVector Middle = Penguin->GetActorLocation() + FVector(0.f, 0.f, 10.f * SizeScale);
				const float Angle = Look.Rand.FRandRange(0.f, 2.f * UE_PI);
				const FVector Where = Middle + FVector(FMath::Cos(Angle) * 52.f, 0.f, FMath::Sin(Angle) * 66.f) * SizeScale * Look.Rand.FRandRange(0.8f, 1.15f);
				SpawnBolt(FX, Look.Rand, Where, 42.f * SizeScale, Velocity);
			}
			break;

		case EPinguTrait::SlowBurn:
			if (Q.HasIdleFX())
			{
				const FVector Tip = BodyPointNow(Look, SkelMesh, FVector(0.f, 0.27f, 2.29f));
				if (Look.FxTimer <= 0.f && FX)
				{
					// Sparks off the burning tip.
					Look.FxTimer = IdleInterval(0.07f);
					FPinguParticleDesc Spark;
					Spark.Mesh = EPinguFXMesh::Star;
					Spark.Location = Tip;
					Spark.Velocity = Velocity * 0.5f + FVector(Look.Rand.FRandRange(-160.f, 160.f), Look.Rand.FRandRange(0.f, 60.f), Look.Rand.FRandRange(60.f, 260.f));
					Spark.Size = Look.Rand.FRandRange(5.f, 9.f) * SizeScale;
					Spark.EndSize = 0.f;
					Spark.Curve = EPinguSizeCurve::Linear;
					Spark.Life = Look.Rand.FRandRange(0.18f, 0.32f);
					Spark.Gravity = 600.f;
					Spark.Color = Look.Rand.FRand() < 0.5f ? FLinearColor(1.f, 0.85f, 0.3f) : FLinearColor(1.f, 0.45f, 0.1f);
					Spark.Emissive = 6.f;
					Spark.Spin = FVector(0.f, 0.f, Look.Rand.FRandRange(-400.f, 400.f));
					FX->Spawn(Spark);
					++MutableCounters().IdleEffects;
				}
				if (Look.FxTimer2 <= 0.f)
				{
					// A wisp of smoke.
					Look.FxTimer2 = IdleInterval(0.5f);
					APinguFX::Puffs(Penguin, Tip + FVector(0.f, 0.f, 6.f), 1, 14.f * SizeScale, 70.f, FVector::UpVector, 0.3f, FLinearColor(0.35f, 0.33f, 0.32f));
					++MutableCounters().IdleEffects;
				}
			}
			break;

		case EPinguTrait::IceTouch:
			if (Q.HasIdleFX() && Look.FxTimer <= 0.f && Move->IsMovingOnGround())
			{
				Look.FxTimer = IdleInterval(0.35f);
				APinguFX::Sparkles(Penguin, Feet + FVector(0.f, 0.f, 8.f), 1, 26.f * SizeScale, FLinearColor(0.6f, 0.92f, 1.f));
				++MutableCounters().IdleEffects;
			}
			break;

		case EPinguTrait::Featherweight:
			if (Q.HasIdleFX() && bJumped)
			{
				APinguFX::Flakes(Penguin, Penguin->GetActorLocation() + FVector(0.f, 0.f, 20.f * SizeScale), 2, 90.f, FLinearColor(0.97f, 0.98f, 1.f));
				++MutableCounters().IdleEffects;
			}
			break;

		case EPinguTrait::Giant:
			if (bLanded && LandSpeed > 500.f)
			{
				APinguFX::Shake(Penguin, FMath::Clamp(LandSpeed / 6000.f, 0.05f, 0.18f));
				if (Q.HasIdleFX())
				{
					APinguFX::GroundPuffs(Penguin, Feet, 1.2f);
					++MutableCounters().IdleEffects;
				}
			}
			if (Q.HasIdleFX() && Move->IsMovingOnGround() && FMath::Abs(Velocity.X) > 150.f && Look.FxTimer <= 0.f)
			{
				// Heavy footsteps.
				Look.FxTimer = IdleInterval(0.3f);
				APinguFX::GroundPuffs(Penguin, Feet, 0.35f);
				++MutableCounters().IdleEffects;
			}
			break;

		case EPinguTrait::GlassCannon:
			if (Q.HasIdleFX() && Look.FxTimer <= 0.f)
			{
				// A glint runs over the glass now and then.
				Look.FxTimer = IdleInterval(0.6f);
				const FVector Where = Penguin->GetActorLocation() + FVector(Look.Rand.FRandRange(-22.f, 22.f), 35.f, Look.Rand.FRandRange(-20.f, 45.f)) * SizeScale;
				APinguFX::Sparkles(Penguin, Where, 1, 4.f, FLinearColor::White);
				++MutableCounters().IdleEffects;
			}
			break;

		case EPinguTrait::Rubber:
			if (bLanded && LandSpeed > 300.f)
			{
				Look.JellyVelocity -= 3.5f * FMath::GetMappedRangeValueClamped(FVector2f(300.f, 2000.f), FVector2f(0.4f, 1.f), LandSpeed);
				if (LandSpeed >= PinguTraitTuning::RubberMinSpeed)
				{
					APinguFX::Sound(Penguin, EPinguSound::Jump, 0.55f, 0.08f, 1.6f); // boing
				}
			}
			break;

		case EPinguTrait::Leech:
			if (bLeeched)
			{
				// Drinks the hit: wine-red sparkles flow in.
				APinguFX::Sparkles(Penguin, Penguin->GetActorLocation() + FVector(0.f, 0.f, 20.f), 6, 45.f * SizeScale, FLinearColor(0.6f, 0.05f, 0.18f));
				++MutableCounters().IdleEffects;
				Look.JellyVelocity += 1.5f;
			}
			break;

		case EPinguTrait::Gambler:
			if (bGamble)
			{
				const FVector Head = Penguin->GetActorLocation() + FVector(0.f, 0.f, 70.f * SizeScale);
				if (Look.bDiceDouble)
				{
					APinguFX::Sparkles(Penguin, Head, 8, 50.f * SizeScale, FLinearColor(1.f, 0.78f, 0.15f));
					++MutableCounters().IdleEffects;
					APinguFX::Ring(Penguin, Head, 70.f * SizeScale, FLinearColor(1.f, 0.78f, 0.15f), 0.3f);
					++MutableCounters().IdleEffects;
				}
				// (A fizzle: the dice tumble as they do; the ability itself turns into sparkling dust where it would have come
				// out - UPenguinAbilityComponent::FizzleUse - and its overhead icon crumbles - SPinguOverheadAbilities.)
			}
			break;

		case EPinguTrait::Speedster:
			if (FMath::Abs(Velocity.X) > 800.f && Look.FxTimer <= 0.f)
			{
				if (Q.bUltra)
				{
					Look.FxTimer = 0.05f;
					APinguFX::Ghost(Penguin, SkelMesh, TraitColor, 0.2f, 0.35f);
					++MutableCounters().IdleEffects;
				}
				else if (Q.HasIdleFX() && Move->IsMovingOnGround())
				{
					Look.FxTimer = IdleInterval(0.08f);
					APinguFX::Trail(Penguin, Feet + FVector(0.f, 0.f, 10.f), 18.f * SizeScale, FLinearColor(0.92f, 0.93f, 0.96f), FVector(-Velocity.X * 0.1f, 0.f, 30.f));
					++MutableCounters().IdleEffects;
				}
			}
			break;

		case EPinguTrait::HotFeet:
			if (Q.HasIdleFX())
			{
				if (Look.FxTimer <= 0.f && FX)
				{
					// Embers rising off the feet.
					Look.FxTimer = IdleInterval(0.12f);
					FPinguParticleDesc Ember;
					Ember.Mesh = EPinguFXMesh::Puff;
					Ember.Location = Feet + FVector(Look.Rand.FRandRange(-22.f, 22.f), Look.Rand.FRandRange(0.f, 20.f), 6.f) * SizeScale;
					Ember.Velocity = FVector(Look.Rand.FRandRange(-30.f, 30.f), 0.f, Look.Rand.FRandRange(70.f, 150.f));
					Ember.Size = 4.f * SizeScale;
					Ember.EndSize = 0.f;
					Ember.Curve = EPinguSizeCurve::Linear;
					Ember.Life = Look.Rand.FRandRange(0.35f, 0.6f);
					Ember.Color = FLinearColor(1.f, 0.42f, 0.06f);
					Ember.Emissive = 5.f;
					FX->Spawn(Ember);
					++MutableCounters().IdleEffects;
				}
				if (Look.FxTimer2 <= 0.f && Move->IsMovingOnGround())
				{
					// Steam off the snow it stands on.
					Look.FxTimer2 = IdleInterval(0.55f);
					APinguFX::Puffs(Penguin, Feet + FVector(0.f, 10.f, 4.f), 1, 16.f * SizeScale, 60.f, FVector::UpVector, 0.5f, FLinearColor(0.92f, 0.93f, 0.95f));
					++MutableCounters().IdleEffects;
				}
			}
			break;

		case EPinguTrait::Mirror:
			if (bMirrorBack)
			{
				// Ready again: a flash on the disc.
				const FVector Disc = BodyPointNow(Look, SkelMesh, FVector(0.f, -0.61f, 0.74f));
				APinguFX::Sparkles(Penguin, Disc, 4, 18.f * SizeScale, FLinearColor(0.9f, 0.95f, 1.f));
				++MutableCounters().IdleEffects;
				APinguFX::Ring(Penguin, Disc, 40.f * SizeScale, FLinearColor(0.85f, 0.9f, 1.f), 0.25f);
				++MutableCounters().IdleEffects;
			}
			break;

		case EPinguTrait::Sticky:
			if (bClingStart && Q.HasIdleFX())
			{
				APinguFX::Puffs(Penguin, Penguin->GetActorLocation() + FVector(Penguin->GetFacing() * 28.f, 0.f, 10.f) * SizeScale, 4, 16.f * SizeScale, 90.f,
					FVector::UpVector, 0.9f, FLinearColor(0.35f, 0.9f, 0.12f));
				++MutableCounters().IdleEffects;
			}
			break;

		case EPinguTrait::Jester:
		{
			// (Ultra: the bells jingle too when they swing hard - a turn, a stop, a knock.)
			const bool bSwung = PinguJesterHat::ConsumeBigSwing(Look.Hat);
			if ((bLanded && LandSpeed > 400.f) || bSwapped || bSwung)
			{
				APinguFX::Sound(Penguin, EPinguSound::UIClick, bSwapped ? 0.5f : 0.3f, 0.12f, 2.f); // jingle
				if (bSwapped)
				{
					const FVector HatTop = BodyPointNow(Look, SkelMesh, FVector(0.f, 0.12f, 2.2f));
					APinguFX::Sparkles(Penguin, HatTop, 4, 30.f * SizeScale, FLinearColor(1.f, 0.75f, 0.15f));
					++MutableCounters().IdleEffects;
				}
			}
			break;
		}

		default:
			break;
		}
	}

	/** The look goes on: whatever the trait and the movement did before is not reacted to (a late joiner's old events, the
	 *  last round's cooldowns). A rescue that already happened leaves no fish. */
	static void SyncSeen(const UPenguinTraitComponent* Traits, FLook& Look, const APenguinCharacter* Penguin)
	{
		const FPinguTraitEvents& Events = Traits->GetEvents();
		Look.SeenGamble = Events.GambleSerial;
		Look.SeenSwaps = Events.JesterSwaps;
		Look.SeenSecondChance = Events.SecondChanceAt;
		Look.FishLeapAge = Events.SecondChanceAt >= 0.f ? 1000.f : -1.f;
		const APenguinGameState* GameState = APenguinGameState::Get(Traits);
		Look.bMirrorReady = !GameState || GameState->GetFlowTime() >= Events.MirrorReadyAt;
		if (const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
		{
			Look.SeenLand = Move->LandEvents;
			Look.SeenJump = Move->JumpEvents;
			Look.SeenAirJump = Move->AirJumpEvents;
			Look.bClinging = Move->IsWallClinging();
		}
		const UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
		for (int32 Slot = 0; Slot < 3; ++Slot)
		{
			Look.CooldownSeen[Slot] = Abilities ? Abilities->GetCooldownRemaining(Slot) : 0.f;
		}
	}

	/** The transform on this machine: a burst in the trait's colour and the trait's own sound. */
	static void PlayTransform(FLook& Look, APenguinCharacter* Penguin)
	{
		if (Penguin->IsHidden() || Penguin->IsKnockedOut())
		{
			return;
		}
		const FPinguTraitInfo& Info = PinguTraits::GetInfo(Look.Trait);
		const float SizeScale = Penguin->GetSizeScale();
		const FVector Middle = Penguin->GetActorLocation() + FVector(0.f, 0.f, 10.f * SizeScale);
		// Puffs in the trait's colour (about six on Low), a few sparkles and a ring.
		const int32 Puffs = Look.Quality.Effects <= EPinguQualityLevel::Low ? 20 : 12;
		APinguFX::Puffs(Penguin, Middle, Puffs, 30.f * SizeScale, 320.f, FVector::UpVector, 1.f, FMath::Lerp(Info.Color, FLinearColor::White, 0.25f));
		APinguFX::Sparkles(Penguin, Middle + FVector(0.f, 0.f, 20.f), 6, 55.f * SizeScale, Info.Color);
		APinguFX::Ring(Penguin, Middle, 85.f * SizeScale, Info.Color, 0.35f);
		struct FTransformSound
		{
			EPinguSound Sound;
			float Pitch;
		};
		FTransformSound Sound = { EPinguSound::Pickup, 1.f };
		switch (Look.Trait)
		{
		case EPinguTrait::Heavyweight: Sound = { EPinguSound::LandHard, 0.7f }; break;
		case EPinguTrait::QuickDraw: Sound = { EPinguSound::Throw, 1.5f }; break;
		case EPinguTrait::SlowBurn: Sound = { EPinguSound::BombFuse, 1.f }; break;
		case EPinguTrait::IceTouch: Sound = { EPinguSound::Freeze, 1.f }; break;
		case EPinguTrait::Featherweight: Sound = { EPinguSound::Gust, 1.6f }; break;
		case EPinguTrait::Tiny: Sound = { EPinguSound::Shrink, 1.3f }; break;
		case EPinguTrait::Giant: Sound = { EPinguSound::Grow, 0.7f }; break;
		case EPinguTrait::GlassCannon: Sound = { EPinguSound::Shatter, 1.6f }; break;
		case EPinguTrait::Rubber: Sound = { EPinguSound::Jump, 1.6f }; break;
		case EPinguTrait::SecondChance: Sound = { EPinguSound::Splash, 1.f }; break;
		case EPinguTrait::Leech: Sound = { EPinguSound::Squawk, 0.6f }; break;
		case EPinguTrait::Gambler: Sound = { EPinguSound::Pickup, 1.f }; break;
		case EPinguTrait::Speedster: Sound = { EPinguSound::Dash, 1.f }; break;
		case EPinguTrait::HotFeet: Sound = { EPinguSound::Melt, 1.f }; break;
		case EPinguTrait::Mirror: Sound = { EPinguSound::ShieldOn, 1.f }; break;
		case EPinguTrait::Sticky: Sound = { EPinguSound::HookHit, 0.7f }; break;
		case EPinguTrait::Jester: Sound = { EPinguSound::UIStart, 1.5f }; break;
		default: break;
		}
		++MutableCounters().Transforms;
		// Every penguin changes within a moment: each sound a little quieter than alone.
		APinguFX::Sound(Penguin, Sound.Sound, 0.55f, 0.05f, Sound.Pitch);
	}

	/** -PinguTraitLookShots: Agent C's own pictures (see FLookShots). Runs once per frame, from any penguin's look. */
	static void UpdateLookShots(const UPenguinTraitComponent* Traits, float Dt)
	{
		FLookShots& Shots = GetLookShots();
		if (!Shots.bOn || Shots.LastFrame == GFrameCounter)
		{
			return;
		}
		Shots.LastFrame = GFrameCounter;
		UWorld* World = Traits->GetWorld();
		const APenguinGameState* GameState = APenguinGameState::Get(Traits);
		if (!World || !GameState)
		{
			return;
		}
		// The reveal as the match shows it (every round): the cards, their flight, the change, after it.
		if (GameState->GetPhase() == EPinguPhase::Intro && Traits->HasAnyTrait())
		{
			if (GameState->GetRoundNumber() != Shots.Round)
			{
				Shots.Round = GameState->GetRoundNumber();
				Shots.RevealShots = 0;
			}
			static const float ShotAges[] = { 0.25f, 1.5f, 3.2f, 3.5f, 3.62f, 4.1f };
			static const TCHAR* ShotNames[] = { TEXT("pop"), TEXT("cards"), TEXT("fly"), TEXT("land"), TEXT("change"), TEXT("done") };
			const float Age = Traits->GetRevealAge();
			if (Shots.RevealShots < UE_ARRAY_COUNT(ShotAges) && Age >= ShotAges[Shots.RevealShots] && Shots.Round <= 2)
			{
				RequestShot(FString::Printf(TEXT("reveal_r%d_%d_%s"), Shots.Round, Shots.RevealShots, ShotNames[Shots.RevealShots]));
				++Shots.RevealShots;
			}
			return;
		}
		if (GameState->GetPhase() != EPinguPhase::Fight || Shots.Stage >= 3)
		{
			return;
		}
		// Close-ups: every penguin with a trait, on this preset (stage 0), then on Low (stage 1, after its shaders).
		Shots.Clock += Dt;
		if (Shots.Order.Num() == 0)
		{
			if (Shots.Clock < 1.2f)
			{
				return;
			}
			for (TActorIterator<APenguinCharacter> It(World); It; ++It)
			{
				if (It->GetTraits() && It->GetTraits()->HasAnyTrait())
				{
					Shots.Order.Add(*It);
				}
			}
			Shots.Order.Sort([](const TWeakObjectPtr<APenguinCharacter>& A, const TWeakObjectPtr<APenguinCharacter>& B)
			{
				return A.IsValid() && B.IsValid() && A->GetPlayerSlot() < B->GetPlayerSlot();
			});
			Shots.Camera = APenguinCamera::FindOrSpawn(World);
			if (APenguinCamera* Stage = Shots.Camera.Get())
			{
				Stage->SetActorTickEnabled(false);
			}
			Shots.Target = 0;
			Shots.Clock = 0.f;
		}
		APenguinCamera* Stage = Shots.Camera.Get();
		if (Shots.Stage == 2)
		{
			// Back to the preset it ran on, the stage camera framing again: done.
			if (Stage)
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				Settings->SetGraphics(Shots.Before); // (a test run saves nothing)
			}
			Shots.Stage = 3;
			UE_LOG(LogPingu, Display, TEXT("[TRAITLOOKS] close-ups done"));
			return;
		}
		if (Shots.Target >= Shots.Order.Num())
		{
			// Next stage: Low.
			Shots.Stage += 1;
			Shots.Target = 0;
			Shots.Clock = -2.5f; // Low's shaders
			if (Shots.Stage == 1)
			{
				if (UPinguSettings* Settings = UPinguSettings::Get())
				{
					Shots.Before = Settings->GetGraphics();
					Settings->ApplyPreset(EPinguGraphicsPreset::Low);
				}
			}
			return;
		}
		const APenguinCharacter* Penguin = Shots.Order[Shots.Target].Get();
		if (!Penguin || Penguin->IsKnockedOut() || Penguin->IsHidden())
		{
			++Shots.Target;
			Shots.Clock = 0.f;
			return;
		}
		if (Stage)
		{
			const float Size = Penguin->GetSizeScale();
			const FVector LookAt = Penguin->GetActorLocation() + FVector(0.f, 0.f, 25.f * Size);
			const FVector From = LookAt + FVector(0.f, 520.f * Size, 60.f * Size);
			Stage->SetActorLocationAndRotation(From, (LookAt - From).Rotation());
		}
		if (Shots.Clock >= 0.35f)
		{
			const UPenguinTraitComponent* Its = Penguin->GetTraits();
			RequestShot(FString::Printf(TEXT("look_%s_%s"), *FString(Its->GetInfo().IconName), Shots.Stage == 0 ? TEXT("high") : TEXT("low")));
			++Shots.Target;
			Shots.Clock = 0.f;
		}
	}
}

// ---------------------------------------------------------------------------- the tests' view of the looks

namespace PenguinTraitLooksPrivate
{
	/** Every trait component's look on this machine (the tests read them through GetLookStats). */
	static TMap<TWeakObjectPtr<const UPenguinTraitComponent>, TWeakPtr<FPinguTraitLooksState>>& GetRegistry()
	{
		static TMap<TWeakObjectPtr<const UPenguinTraitComponent>, TWeakPtr<FPinguTraitLooksState>> Registry;
		return Registry;
	}
}

bool PinguTraitLooks::GetLookStats(const APenguinCharacter* Penguin, FLookStats& Out)
{
	Out = FLookStats();
	const UPenguinTraitComponent* Traits = Penguin ? Penguin->GetTraits() : nullptr;
	if (!Traits)
	{
		return false;
	}
	const TWeakPtr<FPinguTraitLooksState>* Found = PenguinTraitLooksPrivate::GetRegistry().Find(Traits);
	const TSharedPtr<FPinguTraitLooksState> Look = Found ? Found->Pin() : nullptr;
	if (!Look.IsValid())
	{
		return true; // never had a look
	}
	Out.Trait = Look->bApplied || Look->bPending ? Look->Trait : EPinguTrait::None;
	Out.bPending = Look->bPending;
	Out.bApplied = Look->bApplied;
	Out.bQuiet = Look->bQuiet;
	Out.Age = Look->bApplied ? (Look->bQuiet ? Look->Clock : Look->Age) : 0.f;
	for (const FPinguTraitPiece& Piece : Look->Pieces)
	{
		if (const UStaticMeshComponent* Component = Piece.Component.Get())
		{
			++Out.Pieces;
			Out.PiecesShown += Component->bHiddenInGame ? 0 : 1;
			Out.ShadowPieces += Component->CastShadow ? 1 : 0;
		}
	}
	for (const FPinguTraitLooksState::FInstanced& Group : Look->Instanced)
	{
		if (const UInstancedStaticMeshComponent* Component = Group.Component.Get())
		{
			++Out.Pieces;
			Out.PiecesShown += Component->bHiddenInGame ? 0 : 1;
			Out.ShadowPieces += Component->CastShadow ? 1 : 0;
		}
	}
	if (const UPrimitiveComponent* Component = Look->Hat.Component.Get())
	{
		++Out.Pieces; // (Jester's cap)
		Out.PiecesShown += Component->bHiddenInGame ? 0 : 1;
		Out.ShadowPieces += Component->CastShadow ? 1 : 0;
	}
	const USkeletalMeshComponent* SkelMesh = Penguin->GetMesh();
	Out.bOverlay = SkelMesh && Look->Overlay.IsValid() && SkelMesh->GetOverlayMaterial() == Look->Overlay.Get();
	Out.BodyScale = (Look->bApplied ? Look->BoneShape : FVector::OneVector) * Penguin->GetTraitBodyScale();
	Out.HiddenSections = Look->HiddenSections.Num();
	Out.ClipMaterials = Look->ClipMaterials.Num();
	Out.SwappedSlots = Look->SlotsBefore.Num();
	return true;
}

FVector PinguTraitLooks::GetBodyShape(const APenguinCharacter* Penguin)
{
	const UPenguinTraitComponent* Traits = Penguin ? Penguin->GetTraits() : nullptr;
	const TWeakPtr<FPinguTraitLooksState>* Found = Traits ? PenguinTraitLooksPrivate::GetRegistry().Find(Traits) : nullptr;
	const TSharedPtr<FPinguTraitLooksState> Look = Found ? Found->Pin() : nullptr;
	return Look.IsValid() && Look->bApplied ? Look->BoneShape : FVector::OneVector;
}

const PinguTraitLooks::FLookCounters& PinguTraitLooks::GetLookCounters()
{
	return PenguinTraitLooksPrivate::MutableCounters();
}

bool PinguTraitLooks::GetJesterHatStats(const APenguinCharacter* Penguin, PinguJesterHat::FStats& Out, int32* OutBeanieHidden)
{
	Out = PinguJesterHat::FStats();
	if (OutBeanieHidden)
	{
		*OutBeanieHidden = 0;
	}
	const UPenguinTraitComponent* Traits = Penguin ? Penguin->GetTraits() : nullptr;
	const TWeakPtr<FPinguTraitLooksState>* Found = Traits ? PenguinTraitLooksPrivate::GetRegistry().Find(Traits) : nullptr;
	const TSharedPtr<FPinguTraitLooksState> Look = Found ? Found->Pin() : nullptr;
	if (!Look.IsValid() || !Look->bApplied || Look->Trait != EPinguTrait::Jester)
	{
		return false;
	}
	PinguJesterHat::GetStats(Look->Hat, Out);
	if (OutBeanieHidden)
	{
		*OutBeanieHidden = Look->HiddenSections.Num();
	}
	return Out.bPresent;
}

// ---------------------------------------------------------------------------- the hooks

void UPenguinTraitComponent::OnLookStateChanged()
{
	if (!LooksState.IsValid())
	{
		LooksState = MakeShared<FPinguTraitLooksState>();
		PenguinTraitLooksPrivate::GetRegistry().Add(this, LooksState);
	}
	FPinguTraitLooksState& Look = *LooksState;
	if (Look.bSerialKnown && Look.Serial == State.Serial)
	{
		return; // the same trait: its events are read every frame (TickLooks)
	}
	// A new trait (or none): the old look comes off at once, the new one waits for its transform.
	APenguinCharacter* Penguin = GetPenguin();
	PenguinTraitLooksPrivate::ClearLook(this, Look, Penguin);
	Look.bSerialKnown = true;
	Look.Serial = State.Serial;
	Look.Trait = State.Trait;
	Look.bPending = HasAnyTrait();
	Look.Age = 1000.f;
	Look.Clock = 0.f;
	Look.Stagger = 0.f;
	Look.bQuiet = false;
	Look.Rand.Initialize(static_cast<int32>(State.Serial) * 977 + (Penguin ? Penguin->GetPlayerSlot() : 0) * 131 + 7);
	if (Penguin)
	{
		Penguin->SetTraitBodyScale(FVector::OneVector);
	}
}

void UPenguinTraitComponent::TickLooks(float DeltaTime)
{
	using namespace PenguinTraitLooksPrivate;
	PenguinTraitLooksPrivate::UpdateLookShots(this, DeltaTime);
	if (!LooksState.IsValid())
	{
		if (!HasAnyTrait())
		{
			return; // nothing ever shown
		}
		OnLookStateChanged();
	}
	FPinguTraitLooksState& Look = *LooksState;
	if (!Look.bSerialKnown || Look.Serial != State.Serial)
	{
		OnLookStateChanged(); // (a state that came in without a notify)
	}
	APenguinCharacter* Penguin = GetPenguin();
	USkeletalMeshComponent* SkelMesh = Penguin ? Penguin->GetMesh() : nullptr;
	if (!Penguin || !SkelMesh)
	{
		return;
	}
	const float Dt = FMath::Min(DeltaTime, 0.05f);

	// Waiting for the transform: on at TransformAt (+ this penguin's place in the row of cards), with the pop, the
	// burst and the sound - or quietly when that moment is long past here.
	if (Look.bPending && HasAnyTrait() && SkelMesh->GetSkeletalMeshAsset())
	{
		const float Age = GetRevealAge();
		if (Look.Stagger <= 0.f && State.RevealStart >= 0.f)
		{
			Look.Stagger = StaggerStep * static_cast<float>(GetRevealRank(Penguin, State.RevealStart));
		}
		const float Since = Age - (TransformAt + (State.RevealStart >= 0.f ? Look.Stagger : 0.f));
		if (Since >= 0.f)
		{
			Look.bPending = false;
			BuildLook(this, Look, Penguin, SkelMesh);
			SyncSeen(this, Look, Penguin);
			Look.bApplied = true;
			const bool bQuiet = State.RevealStart < 0.f || Since > QuietAfter;
			Look.bQuiet = bQuiet;
			Look.Age = bQuiet ? 1000.f : Since;
			if (!bQuiet)
			{
				PlayTransform(Look, Penguin);
			}
			else
			{
				++MutableCounters().QuietLooks;
			}
			UE_LOG(LogPingu, Display, TEXT("[TRAITLOOKS] %s: %s look on%s (%d pieces, overlay %d, clip %d, body %s)"), *Penguin->GetDisplayName(),
				*PinguTraits::GetLogName(Look.Trait), bQuiet ? TEXT(" quietly") : TEXT(""), Look.Pieces.Num() + Look.Instanced.Num() + (Look.Hat.Component.IsValid() ? 1 : 0),
				Look.bWantsOverlay ? 1 : 0, static_cast<int32>(Look.Clip), *Look.BodyScale.ToString());
		}
	}
	if (!Look.bApplied)
	{
		UpdateOverlay(Look, Penguin);
		return;
	}

	// The settings changed (a preset): build the look again for them (no pop).
	const FLookQuality Quality = ReadQuality();
	if (Quality != Look.Quality)
	{
		const float KeepAge = Look.Age;
		const float KeepClock = Look.Clock;
		const float KeepFish = Look.FishLeapAge;
		ClearLook(this, Look, Penguin);
		BuildLook(this, Look, Penguin, SkelMesh);
		Look.bApplied = true;
		Look.Age = KeepAge;
		Look.Clock = KeepClock;
		Look.FishLeapAge = KeepFish;
	}

	Look.Age += Dt;
	Look.Clock += Dt;
	UpdateEffects(this, Look, Penguin, Dt);
	UpdateBody(Look, Penguin, Dt);
	AnimatePieces(this, Look, Penguin, Dt);
	UpdateClip(Look, SkelMesh);
	UpdateOverlay(Look, Penguin);
}

void UPenguinTraitComponent::EndLooks()
{
	if (LooksState.IsValid())
	{
		APenguinCharacter* Penguin = GetPenguin();
		PenguinTraitLooksPrivate::ClearLook(this, *LooksState, Penguin);
		if (Penguin)
		{
			PenguinTraitLooksPrivate::UpdateOverlay(*LooksState, Penguin);
		}
	}
	PenguinTraitLooksPrivate::GetRegistry().Remove(this);
	LooksState.Reset();
}
