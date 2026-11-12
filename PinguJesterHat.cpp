// Pingu - the Jester trait's hat: the static cap and the Ultra cap with spring bones on its points (see PinguJesterHat.h).

#include "PinguJesterHat.h"
#include "Penguin.h"
#include "PinguSettings.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PinguJesterHat)

namespace PinguJesterHatPrivate
{
	/** The hat's origin on the head (Pingu.blend rest pose: Tools/ArtGen/make_jester_hat.py's PIVOT). */
	const FVector BlendPivot(0.f, 0.f, 1.6f);
	/** The head the points stay out of: a sphere round the head's dome (Pingu.blend (0, 0, 1.436)), as wide as the crown. */
	const FVector BlendHeadCentre(0.f, 0.f, 1.436f);
	constexpr float BlendHeadRadius = 0.56f;

	const TCHAR* const StaticPath = TEXT("/Game/Penguin/JesterHat/SM_JesterHat.SM_JesterHat");
	const TCHAR* const SkeletalPath = TEXT("/Game/Penguin/JesterHat/SK_JesterHat.SK_JesterHat");
	/** The three points: left, the middle one up the back, right (bones Point_<X>_1..4, Bell_<X>, BellEnd_<X>). */
	const TCHAR* const ChainNames[] = { TEXT("L"), TEXT("M"), TEXT("R") };
	constexpr int32 PointBones = 4;

	/** The spring steps: at most this long each (a long frame takes several), never more than MaxSteps a frame. */
	constexpr float MaxStepTime = 1.f / 90.f;
	constexpr int32 MaxSteps = 8;
	/** The hat moved further than this in one frame (cm, at the hat's own size): a teleport - the points start at rest. */
	constexpr float TeleportDistance = 160.f;
	/** A bell swinging this fast against the head (cm/s, at the hat's own size) jingles; at most one jingle per gap. */
	constexpr float SwingJingleSpeed = 380.f;
	constexpr float SwingJingleGap = 0.45f;
	constexpr float Gravity = 980.f;

	/** Per joint, from the point's base to its tip, then the bell (tuned offline against a swaying, bobbing, turning head,
	 *  a landing at 9 m/s and a run's start and stop: idle sway moves the bells about 3 cm, a landing about 20 cm and a run
	 *  about 18 cm, settling within half a second): the spring back to its rest pose on its parent and the one back to its
	 *  rest place on the head (rad/s), the damping of the motion against the head (1/s), a little air drag (1/s), how much
	 *  gravity pulls (x g), how far it may bend against its parent (degrees) and how fast it may move against the head
	 *  (cm/s). The points: stuffed felt, stiffer at the base; the bell: a pendulum on a loose loop. */
	struct FJointTune
	{
		float Omega;
		float ShapeOmega;
		float Damping;
		float AirDrag;
		float GravityScale;
		float MaxBend;
		float MaxSpeed;
	};
	const FJointTune Tunes[PointBones + 1] = {
		{ 22.f, 30.f, 11.f, 0.1f, 0.05f, 22.f, 350.f },
		{ 18.f, 30.f, 11.f, 0.1f, 0.06f, 28.f, 400.f },
		{ 15.f, 28.f, 10.5f, 0.1f, 0.08f, 34.f, 450.f },
		{ 12.f, 25.f, 10.f, 0.1f, 0.10f, 40.f, 500.f },
		{ 6.f, 0.f, 3.5f, 0.1f, 1.f, 75.f, 650.f },
	};

	/** The colours the hat wears: its felt panels in shades of the penguin's colour (the beanie's felt, read off the
	 *  penguin: the left and right points and their crown panels; the middle point a lighter shade of it), the band in
	 *  the beanie cuff's colour. */
	struct FTints
	{
		FLinearColor Main = FLinearColor(0.32f, 0.012f, 0.014f);
		FLinearColor Second = FLinearColor(0.6f, 0.3f, 0.3f);
		FLinearColor Band = FLinearColor(0.45f, 0.012f, 0.014f);
	};

	/** A slot colour off the penguin (its beanie's felt / cuff instances: ApplyBeanieColor). */
	static bool ReadSlotColor(const USkeletalMeshComponent* PenguinMesh, const TCHAR* Piece, FLinearColor& Out)
	{
		if (!PenguinMesh)
		{
			return false;
		}
		const TArray<FName> Names = PenguinMesh->GetMaterialSlotNames();
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Names[Index].ToString().Contains(Piece, ESearchCase::IgnoreCase))
			{
				if (UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(PenguinMesh->GetMaterial(Index)))
				{
					Out = Dynamic->K2_GetVectorParameterValue(TEXT("Color"));
					return true;
				}
			}
		}
		return false;
	}

	static FTints MakeTints(const USkeletalMeshComponent* PenguinMesh)
	{
		FTints Tints;
		ReadSlotColor(PenguinMesh, TEXT("felt"), Tints.Main);
		FLinearColor Cuff = Tints.Main;
		if (ReadSlotColor(PenguinMesh, TEXT("wool"), Cuff))
		{
			Tints.Band = Cuff;
		}
		// The second shade: lighter (toward a warm white), a dark colour a little less so it keeps its hue.
		const float Light = Tints.Main.GetLuminance() < 0.1f ? 0.42f : 0.5f;
		Tints.Second = FMath::Lerp(Tints.Main, FLinearColor(0.9f, 0.86f, 0.78f), Light);
		Tints.Main.A = Tints.Second.A = Tints.Band.A = 1.f;
		return Tints;
	}

	/** Mesh slots by a piece of their name. */
	static void FindSlots(const UPrimitiveComponent* Component, const TCHAR* Piece, TArray<int32>& Out)
	{
		const TArray<FName> Names = Component->GetMaterialSlotNames();
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Names[Index].ToString().Contains(Piece, ESearchCase::IgnoreCase))
			{
				Out.Add(Index);
			}
		}
	}

	/** The material slot a section of a skinned LOD draws (the LOD's material map first). */
	static int32 SectionMaterial(const USkinnedAsset* Asset, int32 LOD, int32 Section, int32 SectionMaterialIndex)
	{
		const FSkeletalMeshLODInfo* Info = Asset ? Asset->GetLODInfo(LOD) : nullptr;
		if (Info && Info->LODMaterialMap.IsValidIndex(Section) && Info->LODMaterialMap[Section] != INDEX_NONE)
		{
			return Info->LODMaterialMap[Section];
		}
		return SectionMaterialIndex;
	}

	static int32 CountLODs(const UPrimitiveComponent* Component)
	{
		if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			return Static->GetStaticMesh() ? Static->GetStaticMesh()->GetNumLODs() : 0;
		}
		if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
		{
			return Skinned->GetNumLODs();
		}
		return 0;
	}

	/** Shows or hides the hat's ink outline: the skeletal one hides its outline sections (every LOD); the static one gets
	 *  a material that draws nothing in its outline slot (like the noot horn's: a static mesh cannot hide one section). */
	static void ApplyOutline(PinguJesterHat::FHat& Hat, bool bShow)
	{
		UPrimitiveComponent* Component = Hat.Component.Get();
		if (!Component)
		{
			return;
		}
		TArray<int32> Slots;
		FindSlots(Component, TEXT("outline"), Slots);
		if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
		{
			const USkinnedAsset* Asset = Skinned->GetSkinnedAsset();
			const FSkeletalMeshRenderData* RenderData = Asset ? Asset->GetResourceForRendering() : nullptr;
			if (RenderData)
			{
				for (int32 LOD = 0; LOD < RenderData->LODRenderData.Num(); ++LOD)
				{
					const TArray<FSkelMeshRenderSection>& Sections = RenderData->LODRenderData[LOD].RenderSections;
					for (int32 Section = 0; Section < Sections.Num(); ++Section)
					{
						const int32 Material = SectionMaterial(Asset, LOD, Section, Sections[Section].MaterialIndex);
						if (Slots.Contains(Material))
						{
							Skinned->ShowMaterialSection(Material, Section, bShow, LOD);
						}
					}
				}
			}
		}
		else if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			UMaterialInstanceDynamic* Hider = Hat.OutlineHider.Get();
			if (!bShow && !Hider)
			{
				if (UMaterialInterface* Ghost = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Ghost.M_Ghost"), nullptr, LOAD_Quiet | LOAD_NoWarn))
				{
					Hider = UMaterialInstanceDynamic::Create(Ghost, Static);
					Hider->SetScalarParameterValue(TEXT("Opacity"), 0.f);
					Hat.OutlineHider = Hider;
				}
			}
			for (const int32 Slot : Slots)
			{
				Static->SetMaterial(Slot, !bShow && Hider ? Hider : nullptr); // nullptr: the mesh's own (M_Outline)
			}
		}
		Hat.bOutlineShown = bShow;
		Hat.bOutlineKnown = true;
	}
}

// ---------------------------------------------------------------------------- the Ultra hat's spring bones

UPinguJesterHatComponent::UPinguJesterHatComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	// The pose is ours: work it out (and send it to the renderer) whether or not the hat was on screen last frame.
	VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	// The points swing beyond the rest pose's bounds.
	BoundsScale = 1.4f;
}

bool UPinguJesterHatComponent::InitSpring()
{
	using namespace PinguJesterHatPrivate;
	Chains.Reset();
	const USkinnedAsset* Asset = GetSkinnedAsset();
	if (!Asset || BoneSpaceTransforms.Num() != Asset->GetRefSkeleton().GetNum())
	{
		return false;
	}
	const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
	const TArray<FTransform>& Local = Ref.GetRefBonePose();
	TArray<FTransform> InComponent;
	InComponent.SetNum(Local.Num());
	int32 Mirrored = 0;
	for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
	{
		const int32 Parent = Ref.GetParentIndex(Bone);
		InComponent[Bone] = Parent == INDEX_NONE ? Local[Bone] : Local[Bone] * InComponent[Parent];
		const FVector S = Local[Bone].GetScale3D();
		Mirrored += S.X * S.Y * S.Z < 0.0 ? 1 : 0;
	}
	for (const TCHAR* Name : ChainNames)
	{
		TArray<int32> Bones;
		for (int32 Index = 1; Index <= PointBones; ++Index)
		{
			Bones.Add(Ref.FindBoneIndex(FName(*FString::Printf(TEXT("Point_%s_%d"), Name, Index))));
		}
		Bones.Add(Ref.FindBoneIndex(FName(*FString::Printf(TEXT("Bell_%s"), Name))));
		Bones.Add(Ref.FindBoneIndex(FName(*FString::Printf(TEXT("BellEnd_%s"), Name))));
		if (Bones.Contains(INDEX_NONE))
		{
			UE_LOG(LogPingu, Warning, TEXT("[JESTERHAT] %s: no bones for the point %s"), *GetNameSafe(Asset), Name);
			continue;
		}
		FChain& Chain = Chains.AddDefaulted_GetRef();
		const int32 ParentBone = Ref.GetParentIndex(Bones[0]);
		Chain.ParentRest = ParentBone == INDEX_NONE ? FTransform::Identity : InComponent[ParentBone];
		Chain.RestHead = InComponent[Bones[0]].GetLocation();
		Chain.RestBell = InComponent[Bones.Last()].GetLocation();
		for (int32 Index = 0; Index <= PointBones; ++Index)
		{
			FJoint& Joint = Chain.Joints.AddDefaulted_GetRef();
			const int32 Bone = Bones[Index];
			Joint.Bone = Bone;
			Joint.RestPose = InComponent[Bone];
			Joint.RestHead = InComponent[Bone].GetLocation();
			const FVector Segment = InComponent[Bones[Index + 1]].GetLocation() - Joint.RestHead;
			Joint.RestLength = FMath::Max(static_cast<float>(Segment.Size()), 0.01f);
			Joint.RestDir = Segment / Joint.RestLength;
			const FJointTune& Tune = Tunes[Index];
			Joint.Stiffness = Tune.Omega;
			Joint.ShapeStiffness = Tune.ShapeOmega;
			Joint.Drag = Tune.Damping;
			Joint.Gravity = Tune.GravityScale;
			Joint.MaxBend = Tune.MaxBend;
			Joint.MaxSpeed = Tune.MaxSpeed;
		}
	}
	// The head sphere in the hat's space: the FBX gives Unreal (100 x, -100 y, 100 z) of Pingu.blend's, round the pivot.
	const FVector FromPivot = BlendHeadCentre - BlendPivot;
	HeadCentre = FVector(FromPivot.X, -FromPivot.Y, FromPivot.Z) * 100.f;
	HeadRadius = BlendHeadRadius * 100.f;
	const FTransform ToWorld = GetComponentTransform();
	UE_LOG(LogPingu, Log, TEXT("[JESTERHAT] %s: %d points on spring bones (%d bones, %d mirrored in the rest pose); the hat's scale in the world %s"),
		*GetNameSafe(Asset), Chains.Num(), Ref.GetNum(), Mirrored, *ToWorld.GetScale3D().ToString());
	return Chains.Num() > 0;
}

void UPinguJesterHatComponent::ResetSpring()
{
	bNeedsReset = false;
	const USkinnedAsset* Asset = GetSkinnedAsset();
	if (!Asset || BoneSpaceTransforms.Num() != Asset->GetRefSkeleton().GetNum())
	{
		bNeedsReset = true;
		return;
	}
	BoneSpaceTransforms = Asset->GetRefSkeleton().GetRefBonePose();
	LastStepTime = 0.f;
	const FTransform ToWorld = GetComponentTransform();
	for (FChain& Chain : Chains)
	{
		for (FJoint& Joint : Chain.Joints)
		{
			Joint.Tail = Joint.PrevTail = ToWorld.TransformPosition(Joint.RestHead + Joint.RestDir * Joint.RestLength);
			Joint.Bend = FQuat::Identity;
		}
	}
	LastHead = Chains.Num() > 0 ? ToWorld.TransformPosition(Chains[0].RestHead) : ToWorld.GetLocation();
	LastToWorld = ToWorld;
	MarkRefreshTransformDirty();
}

void UPinguJesterHatComponent::StepSpring(float Dt, const FTransform& ToWorld)
{
	using namespace PinguJesterHatPrivate;
	const float Scale = FMath::Max(static_cast<float>(ToWorld.GetScale3D().GetAbs().GetMax()), 0.001f);
	const FVector Anchor = Chains.Num() > 0 ? ToWorld.TransformPosition(Chains[0].RestHead) : ToWorld.GetLocation();
	// The head's own motion this step: what the points are dragged along by (damping acts on the motion against it).
	const FVector HeadStep = Anchor - LastHead;
	LastHead = Anchor;
	const FVector Head = ToWorld.TransformPosition(HeadCentre);
	const float HeadReach = HeadRadius * Scale;
	const FVector GravityStep = FVector(0.f, 0.f, -Gravity * Scale) * (Dt * Dt);
	const float StepRatio = LastStepTime > 0.f ? FMath::Clamp(Dt / LastStepTime, 0.5f, 2.f) : 1.f;
	LastStepTime = Dt;
	for (int32 ChainIndex = 0; ChainIndex < Chains.Num(); ++ChainIndex)
	{
		FChain& Chain = Chains[ChainIndex];
		FQuat ParentBend = FQuat::Identity;
		FVector Base = ToWorld.TransformPosition(Chain.RestHead);
		for (int32 Index = 0; Index < Chain.Joints.Num(); ++Index)
		{
			FJoint& Joint = Chain.Joints[Index];
			// Where its rest pose points now: its rest direction, turned with its parent (in the hat's space), into the world.
			const FVector RestDirHat = ParentBend.RotateVector(Joint.RestDir);
			const FVector RestReach = ToWorld.TransformVector(RestDirHat * Joint.RestLength);
			const float Length = FMath::Max(static_cast<float>(RestReach.Size()), 0.01f);
			const FVector RestDir = RestReach / Length;
			// Verlet: the motion it had (against the head's: damped and capped; a little air drag), the springs toward where
			// its rest pose on the parent puts it and toward its rest place on the head, gravity.
			const FVector Moved = (Joint.Tail - Joint.PrevTail) * StepRatio;
			FVector Against = (Moved - HeadStep) * FMath::Exp(-Joint.Drag * Dt);
			const float MostAgainst = Joint.MaxSpeed * Scale * Dt;
			if (Against.SizeSquared() > FMath::Square(MostAgainst))
			{
				Against = Against.GetSafeNormal() * MostAgainst;
			}
			const FVector Motion = (HeadStep + Against) * FMath::Exp(-Tunes[FMath::Min(Index, PointBones)].AirDrag * Dt);
			const FVector RestTail = Base + RestDir * Length;
			const FVector ShapeTail = ToWorld.TransformPosition(Joint.RestHead + Joint.RestDir * Joint.RestLength);
			FVector Next = Joint.Tail + Motion + (RestTail - Joint.Tail) * (Joint.Stiffness * Joint.Stiffness * Dt * Dt)
				+ (ShapeTail - Joint.Tail) * (Joint.ShapeStiffness * Joint.ShapeStiffness * Dt * Dt) + GravityStep * Joint.Gravity;
			// Never into the head.
			const FVector FromHead = Next - Head;
			if (FromHead.SizeSquared() < FMath::Square(HeadReach))
			{
				Next = Head + FromHead.GetSafeNormal() * HeadReach;
			}
			// Its length, and no further than MaxBend from its rest pose.
			FVector Dir = (Next - Base).GetSafeNormal();
			if (Dir.IsNearlyZero())
			{
				Dir = RestDir;
			}
			const float Cos = static_cast<float>(Dir | RestDir);
			const float MaxCos = FMath::Cos(FMath::DegreesToRadians(Joint.MaxBend));
			if (Cos < MaxCos)
			{
				const FVector Side = (Dir - RestDir * Cos).GetSafeNormal();
				const float Angle = FMath::DegreesToRadians(Joint.MaxBend);
				Dir = Side.IsNearlyZero() ? RestDir : (RestDir * FMath::Cos(Angle) + Side * FMath::Sin(Angle));
			}
			Next = Base + Dir * Length;
			Joint.PrevTail = Joint.Tail;
			Joint.Tail = Next;
			// Its turn now (the hat's space): the rest direction (with the parent's turn) onto the new one.
			const FVector DirHat = ToWorld.InverseTransformVector(Dir).GetSafeNormal();
			Joint.Bend = (DirHat.IsNearlyZero() ? FQuat::Identity : FQuat::FindBetweenNormals(RestDirHat, DirHat)) * ParentBend;
			ParentBend = Joint.Bend;
			Base = Next;
		}
		// The bell: how far from where it hangs at rest (in the hat's own frame), how fast against the head.
		const FJoint& Bell = Chain.Joints.Last();
		const float SwingSpeed = static_cast<float>(((Bell.Tail - Bell.PrevTail) - HeadStep).Size()) / FMath::Max(Dt, 1.e-4f) / Scale;
		if (SwingSpeed > SwingJingleSpeed && SwingCooldown <= 0.f)
		{
			bBigSwing = true;
			SwingCooldown = SwingJingleGap;
		}
		const FVector Start = BellStart.IsValidIndex(ChainIndex) ? BellStart[ChainIndex] : Chain.RestBell;
		MostBellMove = FMath::Max(MostBellMove, static_cast<float>(FVector::Dist(ToWorld.InverseTransformPosition(Bell.Tail), Start)) * Scale);
	}
}

void UPinguJesterHatComponent::ApplySpring()
{
	// Every bone's pose in the hat's space: its rest pose turned about its head by its bend and moved to where its head is
	// now (the parent's tail); the engine wants it against the parent's pose.
	const FTransform ToWorld = GetComponentTransform();
	for (const FChain& Chain : Chains)
	{
		FTransform Parent = Chain.ParentRest;
		FVector Head = Chain.RestHead;
		for (const FJoint& Joint : Chain.Joints)
		{
			const FTransform Now = Joint.RestPose * FTransform(-Joint.RestHead) * FTransform(Joint.Bend) * FTransform(Head);
			if (BoneSpaceTransforms.IsValidIndex(Joint.Bone))
			{
				BoneSpaceTransforms[Joint.Bone] = Now.GetRelativeTransform(Parent);
			}
			Parent = Now;
			Head = ToWorld.InverseTransformPosition(Joint.Tail);
		}
	}
	MarkRefreshTransformDirty();
}

void UPinguJesterHatComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	using namespace PinguJesterHatPrivate;
	if (!bSpringInit)
	{
		bSpringInit = InitSpring();
		bNeedsReset = true;
	}
	SwingCooldown -= DeltaTime;
	if (bSpringInit && !bHiddenInGame)
	{
		if (bNeedsReset)
		{
			ResetSpring();
		}
		// A teleport (a respawn, a warp, the round's reset): the points start again from their rest pose.
		const FTransform ToWorld = GetComponentTransform();
		const float Scale = static_cast<float>(ToWorld.GetScale3D().GetAbs().GetMax());
		const FVector Anchor = Chains.Num() > 0 ? ToWorld.TransformPosition(Chains[0].RestHead) : ToWorld.GetLocation();
		if (FVector::Dist(Anchor, LastHead) > TeleportDistance * FMath::Max(Scale, 0.1f))
		{
			ResetSpring();
		}
		const float Dt = FMath::Min(DeltaTime, MaxStepTime * MaxSteps);
		const int32 Steps = FMath::Clamp(FMath::CeilToInt(Dt / MaxStepTime), 1, MaxSteps);
		if (Dt > 0.f)
		{
			for (int32 Step = 1; Step <= Steps; ++Step)
			{
				FTransform Between;
				Between.Blend(LastToWorld, ToWorld, static_cast<float>(Step) / Steps);
				StepSpring(Dt / Steps, Between);
			}
		}
		LastToWorld = ToWorld;
		ApplySpring();
	}
	else if (bHiddenInGame)
	{
		bNeedsReset = true;
	}
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

float UPinguJesterHatComponent::GetBellOffset(int32 Chain) const
{
	if (!Chains.IsValidIndex(Chain) || Chains[Chain].Joints.Num() == 0)
	{
		return 0.f;
	}
	const FTransform ToWorld = GetComponentTransform();
	return static_cast<float>(FVector::Dist(Chains[Chain].Joints.Last().Tail, ToWorld.TransformPosition(Chains[Chain].RestBell)));
}

void UPinguJesterHatComponent::ResetSpringStats()
{
	const FTransform ToWorld = GetComponentTransform();
	BellStart.Reset();
	for (const FChain& Chain : Chains)
	{
		BellStart.Add(Chain.Joints.Num() > 0 ? ToWorld.InverseTransformPosition(Chain.Joints.Last().Tail) : Chain.RestBell);
	}
	MostBellMove = 0.f;
}

bool UPinguJesterHatComponent::ConsumeBigSwing()
{
	const bool bSwung = bBigSwing;
	bBigSwing = false;
	return bSwung;
}

// ---------------------------------------------------------------------------- putting it on

FVector PinguJesterHat::GetBlendPivot()
{
	return PinguJesterHatPrivate::BlendPivot;
}

bool PinguJesterHat::Build(FHat& Hat, USkeletalMeshComponent* PenguinMesh, FName Bone, const FTransform& InBone, bool bSpring, bool bShadows,
	TArray<TObjectPtr<UObject>>& Keep)
{
	using namespace PinguJesterHatPrivate;
	Hat = FHat();
	AActor* PenguinActor = PenguinMesh ? PenguinMesh->GetOwner() : nullptr;
	if (!PenguinActor || PenguinMesh->GetBoneIndex(Bone) == INDEX_NONE)
	{
		return false;
	}
	UPrimitiveComponent* Made = nullptr;
	if (bSpring)
	{
		if (USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, SkeletalPath, nullptr, LOAD_Quiet | LOAD_NoWarn))
		{
			UPinguJesterHatComponent* Spring = NewObject<UPinguJesterHatComponent>(PenguinActor, NAME_None, RF_Transient);
			Spring->SetSkinnedAssetAndUpdate(Mesh, true);
			Made = Spring;
			Hat.bSkeletal = true;
		}
		else
		{
			UE_LOG(LogPingu, Warning, TEXT("[JESTERHAT] no %s: the static hat instead (run the jesterhat import phase)"), SkeletalPath);
		}
	}
	if (!Made)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, StaticPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!Mesh)
		{
			return false;
		}
		UStaticMeshComponent* Static = NewObject<UStaticMeshComponent>(PenguinActor, NAME_None, RF_Transient);
		Static->SetStaticMesh(Mesh);
		Made = Static;
	}
	Made->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Made->SetGenerateOverlapEvents(false);
	Made->SetCanEverAffectNavigation(false);
	Made->SetCastShadow(bShadows);
	Made->bReceivesDecals = false;
	Made->SetupAttachment(PenguinMesh, Bone);
	Made->SetRelativeTransform(InBone);
	Made->RegisterComponent();
	if (Hat.bSkeletal)
	{
		Made->AddTickPrerequisiteComponent(PenguinMesh); // (the head's pose of this frame first)
	}
	Keep.Add(Made);
	Hat.Component = Made;
	Hat.InBone = InBone;
	Hat.bShown = !Made->bHiddenInGame;

	// The felt panels in shades of the penguin's colour, the band in its cuff's.
	const FTints Tints = MakeTints(PenguinMesh);
	const TArray<FName> Names = Made->GetMaterialSlotNames();
	for (int32 Slot = 0; Slot < Names.Num(); ++Slot)
	{
		const FString Name = Names[Slot].ToString().ToLower();
		const FLinearColor* Tint = nullptr;
		if (Name.Contains(TEXT("felt_m")))
		{
			Tint = &Tints.Second;
		}
		else if (Name.Contains(TEXT("felt")))
		{
			Tint = &Tints.Main;
		}
		else if (Name.Contains(TEXT("wool")) || Name.Contains(TEXT("band")))
		{
			Tint = &Tints.Band;
		}
		UMaterialInterface* Parent = Tint ? Made->GetMaterial(Slot) : nullptr;
		if (!Parent)
		{
			continue;
		}
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, Made);
		Instance->SetVectorParameterValue(TEXT("Color"), *Tint);
		Made->SetMaterial(Slot, Instance);
		Hat.Tints.Add(Instance);
		Keep.Add(Instance);
	}
	return true;
}

void PinguJesterHat::Update(FHat& Hat, USkeletalMeshComponent* PenguinMesh, float Grow, bool bShow)
{
	using namespace PinguJesterHatPrivate;
	UPrimitiveComponent* Component = Hat.Component.Get();
	if (!Component)
	{
		return;
	}
	bShow = bShow && Grow > 0.01f;
	if (Component->bHiddenInGame == bShow)
	{
		Component->SetHiddenInGame(!bShow);
		if (UPinguJesterHatComponent* Spring = Cast<UPinguJesterHatComponent>(Component); Spring && bShow)
		{
			Spring->ResetSpring();
		}
	}
	Hat.bShown = bShow;
	if (!bShow)
	{
		return;
	}
	// Its size: the look's pop (elastic) on top of its place on the bone.
	FTransform Place = Hat.InBone;
	Place.SetScale3D(Hat.InBone.GetScale3D() * FMath::Max(Grow, 0.001f));
	if (!Component->GetRelativeTransform().Equals(Place, 1.e-4))
	{
		Component->SetRelativeTransform(Place);
	}
	// The LOD the penguin draws (the hat is smaller: on its own it would drop its detail too soon).
	const int32 LODs = CountLODs(Component);
	const int32 Wanted = PenguinMesh && LODs > 0 ? FMath::Clamp(PenguinMesh->GetPredictedLODLevel(), 0, LODs - 1) : 0;
	if (Wanted != Hat.LOD)
	{
		if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			Static->SetForcedLodModel(Wanted + 1);
		}
		else if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
		{
			Skinned->SetForcedLOD(Wanted + 1);
		}
		Hat.LOD = Wanted;
	}
	// The ink outline is gone for good: hidden once, when the hat is built.
	if (!Hat.bOutlineKnown)
	{
		ApplyOutline(Hat, false);
	}
}

void PinguJesterHat::Destroy(FHat& Hat, TArray<TObjectPtr<UObject>>& Keep)
{
	if (UPrimitiveComponent* Component = Hat.Component.Get())
	{
		Keep.RemoveSingle(Component);
		Component->DestroyComponent();
	}
	for (const TWeakObjectPtr<UMaterialInstanceDynamic>& Tint : Hat.Tints)
	{
		if (UMaterialInstanceDynamic* Instance = Tint.Get())
		{
			Keep.RemoveSingle(Instance);
		}
	}
	Hat = FHat();
}

bool PinguJesterHat::ConsumeBigSwing(FHat& Hat)
{
	UPinguJesterHatComponent* Spring = Cast<UPinguJesterHatComponent>(Hat.Component.Get());
	return Spring && Hat.bShown && Spring->ConsumeBigSwing();
}

void PinguJesterHat::GetStats(const FHat& Hat, FStats& Out)
{
	using namespace PinguJesterHatPrivate;
	Out = FStats();
	UPrimitiveComponent* Component = Hat.Component.Get();
	if (!Component)
	{
		return;
	}
	Out.bPresent = true;
	Out.Component = Component;
	Out.bShown = Hat.bShown && !Component->bHiddenInGame;
	Out.LODs = CountLODs(Component);
	Out.LOD = Hat.LOD;
	TArray<int32> Slots;
	FindSlots(Component, TEXT("outline"), Slots);
	Out.OutlineSlots = Slots.Num();
	if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
	{
		Out.Mesh = GetNameSafe(Static->GetStaticMesh());
		Out.bOutlineShown = Slots.Num() > 0;
		for (const int32 Slot : Slots)
		{
			Out.bOutlineShown &= Static->GetMaterial(Slot) != Hat.OutlineHider.Get() || !Hat.OutlineHider.IsValid();
		}
	}
	else if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
	{
		const USkinnedAsset* Asset = Skinned->GetSkinnedAsset();
		Out.bSkeletal = true;
		Out.Mesh = GetNameSafe(Asset);
		Out.Bones = Asset ? Asset->GetRefSkeleton().GetNum() : 0;
		const FSkeletalMeshRenderData* RenderData = Asset ? Asset->GetResourceForRendering() : nullptr;
		if (RenderData)
		{
			for (int32 LOD = 0; LOD < RenderData->LODRenderData.Num(); ++LOD)
			{
				const TArray<FSkelMeshRenderSection>& Sections = RenderData->LODRenderData[LOD].RenderSections;
				for (int32 Section = 0; Section < Sections.Num(); ++Section)
				{
					const int32 Material = SectionMaterial(Asset, LOD, Section, Sections[Section].MaterialIndex);
					if (Slots.Contains(Material))
					{
						++Out.OutlineSections;
						Out.OutlineHidden += Skinned->IsMaterialSectionShown(Material, LOD) ? 0 : 1;
					}
				}
			}
		}
		Out.bOutlineShown = Out.OutlineSections > 0 && Out.OutlineHidden == 0;
		if (const UPinguJesterHatComponent* Spring = Cast<UPinguJesterHatComponent>(Skinned))
		{
			Out.Chains = Spring->GetChainCount();
			for (int32 Chain = 0; Chain < 3 && Chain < Out.Chains; ++Chain)
			{
				Out.BellOffset[Chain] = Spring->GetBellOffset(Chain);
			}
			Out.MostBellMove = Spring->GetMostBellMove();
		}
	}
}
