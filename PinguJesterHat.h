// Pingu - the Jester trait's hat (PenguinTraitLooks.cpp puts it on a Jester's head): a felt jester cap from
// Tools/ArtGen/make_jester_hat.py (imported by Tools/ue_jester_hat.py, phase jesterhat).
//   - every preset but Ultra effects: /Game/Penguin/JesterHat/SM_JesterHat, a static mesh (no bones, no physics);
//   - Ultra effects: /Game/Penguin/JesterHat/SK_JesterHat on a UPinguJesterHatComponent - its three points are chains of
//     four bones (Point_<L|M|R>_1..4) with a bell hanging off each tip (Bell_<X>), moved by code-driven spring bones: each
//     bone springs back toward its rest pose on its parent and toward its rest place on the head, the head's motion drags
//     them (damped and capped against it), gravity (Verlet, steps of at most 1/90 s with the hat's place blended across a
//     frame's steps), a sphere round the head keeps them out of it. Local on every machine: nothing replicates.
// Both sit on the penguin's Body bone (the head and the beanie are on it), sized with it (size rays, the body's squash),
// draw the LOD the penguin draws, tint their felt panels in shades of the penguin's colour and keep their ink outline hidden.

#pragma once

#include "CoreMinimal.h"
#include "Components/PoseableMeshComponent.h"
#include "PinguJesterHat.generated.h"

class UMaterialInstanceDynamic;
class UPrimitiveComponent;
class USkeletalMeshComponent;

/** The Ultra hat: SK_JesterHat with spring bones on its points (see the file comment). Ticks after the penguin's mesh. */
UCLASS(Transient, ClassGroup = Pingu)
class PENGUIN_API UPinguJesterHatComponent : public UPoseableMeshComponent
{
	GENERATED_BODY()

public:
	UPinguJesterHatComponent(const FObjectInitializer& ObjectInitializer);

	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Every point back in its rest pose where the hat is now (the next tick starts from there). */
	void ResetSpring();
	/** The chains found in the mesh (3: left, middle, right; 0 before the first tick or without the bones). */
	int32 GetChainCount() const { return Chains.Num(); }
	/** How far the bell of a chain is from where it hangs at rest (cm, the hat's own frame: the head's motion does not count). */
	float GetBellOffset(int32 Chain) const;
	/** The most any bell moved in the hat's own frame (cm: against the head) since ResetSpringStats, which takes where the
	 *  bells are then as the start. */
	float GetMostBellMove() const { return MostBellMove; }
	void ResetSpringStats();
	/** A bell swung hard since the last call (a jingle; at most one every SwingJingleGap). */
	bool ConsumeBigSwing();

private:
	/** One simulated bone: from its head to its child's head (its tail). Its rest pose is kept in the hat's own (component)
	 *  space and every rotation is worked out from directions there, so a mirrored or scaled parent changes nothing. */
	struct FJoint
	{
		int32 Bone = INDEX_NONE;
		/** The bone at rest (component space), its head there, the direction to its tail and how far (the mesh's units). */
		FTransform RestPose = FTransform::Identity;
		FVector RestHead = FVector::ZeroVector;
		FVector RestDir = FVector::ForwardVector;
		float RestLength = 1.f;
		/** How it is turned away from its rest pose now (component space, its parents' turn included). */
		FQuat Bend = FQuat::Identity;
		/** The springs (rad/s): toward its rest pose on its (moving) parent, and toward its rest place on the head (keeps the
		 *  point's shape: a chain of soft bones alone flops far too much); the damping of its motion against the head (1/s);
		 *  how much gravity pulls (x g); how far it may bend from its rest pose on its parent (degrees); how fast it may move
		 *  against the head (cm/s at the hat's own size: felt's own friction - a hard stop or a turn whips it, but not wildly). */
		float Stiffness = 1.f;
		float ShapeStiffness = 0.f;
		float Drag = 1.f;
		float Gravity = 0.f;
		float MaxBend = 50.f;
		float MaxSpeed = 500.f;
		/** The tail now and a step ago (world). */
		FVector Tail = FVector::ZeroVector;
		FVector PrevTail = FVector::ZeroVector;
	};
	struct FChain
	{
		/** The bone the chain hangs on (Root): its rest pose in the component (it never moves). */
		FTransform ParentRest = FTransform::Identity;
		/** The first joint's head at rest (component). */
		FVector RestHead = FVector::ZeroVector;
		/** The joints: the four point bones, then the bell. */
		TArray<FJoint> Joints;
		/** Where the bell's middle is at rest (component) - the tail of the last joint. */
		FVector RestBell = FVector::ZeroVector;
	};

	bool InitSpring();
	/** One step of Dt with the hat at ToWorld (a frame's steps get the hat's place blended from the last frame's to this
	 *  one's: the head's motion is spread over them, never all in the first). */
	void StepSpring(float Dt, const FTransform& ToWorld);
	void ApplySpring();

	TArray<FChain> Chains;
	bool bSpringInit = false;
	bool bNeedsReset = true;
	/** The last step's length (the Verlet motion is scaled when the steps change length). */
	float LastStepTime = 0.f;
	FVector LastHead = FVector::ZeroVector;
	/** The hat's place in the world at the last step. */
	FTransform LastToWorld = FTransform::Identity;
	/** Where the bells were at ResetSpringStats (the hat's own frame) and the most they moved since (cm). */
	TArray<FVector> BellStart;
	float MostBellMove = 0.f;
	float SwingCooldown = 0.f;
	bool bBigSwing = false;
	/** The head sphere (component, rest units) the points stay out of. */
	FVector HeadCentre = FVector::ZeroVector;
	float HeadRadius = 0.f;
};

namespace PinguJesterHat
{
	/** The hat's origin on the penguin: Pingu.blend's rest pose, Blender units (make_jester_hat.py's PIVOT). */
	PENGUIN_API FVector GetBlendPivot();

	/** The hat a look put on a penguin. */
	struct FHat
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		/** Its tinted felt panels and band (dynamic instances of the penguin's felt / wool). */
		TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> Tints;
		/** The static hat's outline slot hidden (always): an M_Ghost instance at opacity 0, like the horn's. */
		TWeakObjectPtr<UMaterialInstanceDynamic> OutlineHider;
		/** Its place on the bone at full size. */
		FTransform InBone = FTransform::Identity;
		bool bSkeletal = false;
		bool bShown = false;
		bool bOutlineShown = true;
		bool bOutlineKnown = false;
		/** The LOD it was told to draw (0-based; -1: none yet). */
		int32 LOD = -1;
	};

	/** Puts the hat on PenguinMesh's Bone at InBone: the skeletal one with spring bones (bSpring: Ultra) or the static one.
	 *  Its objects go into Keep (the trait component's TraitObjects). False when the hat's assets are missing. */
	PENGUIN_API bool Build(FHat& Hat, USkeletalMeshComponent* PenguinMesh, FName Bone, const FTransform& InBone, bool bSpring, bool bShadows,
		TArray<TObjectPtr<UObject>>& Keep);
	/** Every frame: shown or not, its size (the look's pop: Grow), the penguin's LOD (its outline stays hidden). */
	PENGUIN_API void Update(FHat& Hat, USkeletalMeshComponent* PenguinMesh, float Grow, bool bShow);
	/** Takes it off again. */
	PENGUIN_API void Destroy(FHat& Hat, TArray<TObjectPtr<UObject>>& Keep);
	/** Ultra: a bell swung hard (a jingle). */
	PENGUIN_API bool ConsumeBigSwing(FHat& Hat);

	/** The hat as this machine shows it (the tests). */
	struct FStats
	{
		bool bPresent = false;
		/** The Ultra hat (spring bones) or the static one. */
		bool bSkeletal = false;
		bool bShown = false;
		FString Mesh;
		/** The mesh's LODs and the one it is told to draw (the penguin's, 0-based). */
		int32 LODs = 0;
		int32 LOD = -1;
		/** Ultra: the points on spring bones and the bones of the mesh. */
		int32 Chains = 0;
		int32 Bones = 0;
		/** Its ink outline: shown (the skeletal one: all its outline sections on every LOD; the static one: its own outline
		 *  material in the slot), and the skeletal one's outline sections and how many are hidden. */
		bool bOutlineShown = true;
		int32 OutlineSlots = 0; // material slots called outline
		int32 OutlineSections = 0;
		int32 OutlineHidden = 0;
		/** Ultra: how far each bell is from its rest place now, and the most one moved since ResetSpringStats (cm, the hat's
		 *  own frame); 0 for the static hat. */
		float BellOffset[3] = { 0.f, 0.f, 0.f };
		float MostBellMove = 0.f;
		TWeakObjectPtr<UPrimitiveComponent> Component;
	};
	PENGUIN_API void GetStats(const FHat& Hat, FStats& Out);
}
