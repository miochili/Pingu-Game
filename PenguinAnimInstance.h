// Pingu - code-driven animation (no Animation Blueprint needed).

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "PenguinTypes.h"
#include "PenguinAnimInstance.generated.h"

class UAnimSequence;

/** One entry in the cross-fade stack: a clip, or the speed-driven locomotion blend. */
struct FPenguinAnimLayer
{
	EPenguinClip Clip = EPenguinClip::Idle;
	bool bLocomotion = false;
	const UAnimSequence* Sequence = nullptr;
	float Length = 0.f;
	float Time = 0.f;
	float Rate = 1.f;
	float Weight = 0.f;
	float FadeInSpeed = 10.f;
	bool bLoop = true;
};

/**
 * Picks the penguin's clips from the movement state and cross-fades between them.
 *
 *  - Locomotion: Idle / Waddle / Jog / RunFast blended by the real speed with one shared foot
 *    phase that advances by speed / stride, so the planted foot never slides (the clips are
 *    authored for 200 / 380 / 560 cm/s).
 *  - One-shots for jumps, landings, skids, stops, hits, dashes, slides; idle fidgets.
 *  - Ability actions play on top (flippers and body), keeping the feet running.
 *  - The eyes look at the aim point (mouse), blink, and the body leans toward it a little.
 *
 * Runs on the animation worker thread; the game-thread state is copied in PreUpdate.
 */
USTRUCT()
struct FPenguinAnimInstanceProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	FPenguinAnimInstanceProxy() {}
	FPenguinAnimInstanceProxy(UAnimInstance* InAnimInstance) : FAnimInstanceProxy(InAnimInstance) {}

protected:
	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual void Update(float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	enum class EMode : uint8 { Ground, Air, Dash, Slide, StunAir, Frozen, KO, Stomp, Victory };
	enum class EBoneKind : uint8 { Other, Root, Body, Flipper, Tail, Foot, Eye, Beak };

	void Play(EPenguinClip Clip, float BlendTime, float Rate, float StartTime = 0.f);
	void PlayLocomotion(float BlendTime);
	bool PlayOneShot(EPenguinClip Clip, float BlendTime, float Rate, float StartFrame = 0.f);
	void UpdateStateMachine(float Dt);
	void UpdateLocomotion(float Dt);
	void UpdateAction(float Dt, EMode Mode);
	void AdvanceLayers(float Dt);
	bool IsOneShotStillValid(EMode Mode) const;
	bool IsTopLayerFinished() const;
	bool IsLocomotionTop() const { return Layers.Num() > 0 && Layers.Last().bLocomotion; }
	const UAnimSequence* GetClip(EPenguinClip Clip) const;
	static bool IsLoopingClip(EPenguinClip Clip);
	static bool IsFidget(EPenguinClip Clip);

	void EvaluateClip(const UAnimSequence* Sequence, float Time, bool bLoop, FPoseContext& Pose) const;
	void EvaluateLayer(const FPenguinAnimLayer& Layer, FPoseContext& Pose) const;
	void EvaluateLocomotion(FPoseContext& Pose) const;
	void ApplyAction(FPoseContext& Output);
	/** The noot: turns the body (about its base, the feet stay) so the beak - and the horn on it - points along the noot. */
	void ApplyHonkAim(FPoseContext& Output);
	/** The icicle throw (UPenguinAbilityComponent's charge, as this machine shows it): where the throwing flipper is now. */
	void UpdateIcicleThrow(float Dt);
	/** The throwing flipper (the one the camera sees) posed by code - pulled out from behind the back, cocked over the head,
	 *  drawn back as it charges (trembling at full), swung forward and through - the other flipper out for balance, the body
	 *  leaning back and into the throw. Blended over whatever else plays (the legs keep running). */
	void ApplyIcicleThrow(FPoseContext& Output);
	/** Flap: both flippers beat up and down, fast and wide (a big, silly bird), the body bobbing with them. */
	void ApplyFlap(FPoseContext& Output);
	void ApplyLookAt(FPoseContext& Output);
	/** A trait's body shape (PinguTraitLooks::GetBodyShape, copied in PreUpdate) on the Body bone - what hangs on it (eyes,
	 *  flippers, tail) and the feet keep their own shape: the eyes stay round. */
	void ApplyBodyShape(FPoseContext& Output);
	void CachePenguinBones(const FBoneContainer& Bones);
	/** The lobby island (PinguLobby::GetAnimState): the turn toward the camera / a neighbour, eased. Lobby only: a match never
	 *  gets here. */
	void UpdateLobby(float Dt, EMode Mode);
	/** The lobby's pose on the bones (after the clips, before the noot's aim and the eyes): sitting or hanging, and the gag's
	 *  flippers and body. */
	void ApplyLobbyPose(FPoseContext& Output);
	/** Last of all: the root turned (to the camera / a neighbour, the poke's spin, a sway) and moved (hanging off the edge, hops). */
	void ApplyLobbyPlacement(FPoseContext& Output);
	/** A flipper turned the clips' way (Flare out to its side, Swing forward; degrees from its rest), blended in by Alpha. */
	void PoseFlipper(FPoseContext& Output, int32 Side, float Flare, float Swing, float Alpha) const;
	/** The body tipped about its base (Lean degrees forward; negative: back) and lowered by Drop (share of its height). */
	void PoseBody(FPoseContext& Output, float Lean, float Drop) const;
	float ActionMask(EBoneKind Kind) const;

	FPenguinAnimInputs In;
	TArray<const UAnimSequence*> ClipTable;
	TArray<FPenguinAnimLayer> Layers;

	bool bStarted = false;
	EMode PrevMode = EMode::Ground;
	EMode CurrentMode = EMode::Ground;
	EPenguinClip CurrentClip = EPenguinClip::Idle;
	bool bOneShotActive = false;
	bool bApexPending = false;
	float RecentRunSpeed = 0.f;

	// locomotion
	float LocoSpeed = 0.f;
	float LocoPhase = 0.f;
	float IdleTime = 0.f;
	float MoveAlpha = 0.f;
	EPenguinClip GaitA = EPenguinClip::Waddle;
	EPenguinClip GaitB = EPenguinClip::Waddle;
	float GaitBlend = 0.f;

	// idle fidgets
	float IdleTimer = 0.f;
	float NextFidget = 6.f;
	FRandomStream Rand;

	// ability action overlay
	int32 SeenAction = 0;
	const UAnimSequence* ActionSequence = nullptr;
	float ActionTime = 0.f;
	float ActionLength = 0.f;
	float ActionRate = 1.f;
	float ActionWeight = 0.f;
	bool bActionPlaying = false;

	// the icicle throw (code-driven: no clip). A flipper pose in the clips' vocabulary (Tools/penguin_anims.py): Flare
	// lifts the flipper out to the side and up, Swing swings it forward (negative: back), both in degrees from its rest.
	struct FThrowArm
	{
		float Flare = 0.f;
		float Swing = 0.f;
		/** The body leans forward this many degrees (negative: back). */
		float Lean = 0.f;
	};
	/** What the ability component shows (copied in PreUpdate). */
	uint8 ThrowStage = 0;
	float ThrowStageTime = 0.f;
	float ThrowChargeTime = 0.f;
	/** Flap (the ability): how much the flapping pose shows (eased in and out) and the beat's clock. */
	float FlapAlpha = 0.f;
	float FlapClock = 0.f;
	/** The pose now, how much of it shows (0..1), and how much Flipper_L is the throwing one (1: it is, the one seen facing right;
	 *  0: Flipper_R, facing left; in between while the aim crosses over and the penguin turns round - the two flippers swap
	 *  roles over ThrowSwapTime, at once when the throw starts). */
	FThrowArm ThrowPose;
	float ThrowWeight = 0.f;
	float ThrowLeftBlend = 1.f;
	/** The rig in its reference pose (component space): Body's and the flippers' rotations, the flippers' rest direction
	 *  (shoulder to hand) and the side each one is on (+1 / -1 along the mesh's X). Index 0: Flipper_L, 1: Flipper_R. */
	int32 MeshFlipper[2] = { INDEX_NONE, INDEX_NONE };
	FQuat BodyRestRotation = FQuat::Identity;
	FQuat FlipperRestRotation[2] = { FQuat::Identity, FQuat::Identity };
	FVector FlipperRestDir[2] = { FVector(0.f, 0.f, -1.f), FVector(0.f, 0.f, -1.f) };
	float FlipperSide[2] = { 1.f, -1.f };

	// eyes and attention
	float LastDt = 0.f;
	float TotalTime = 0.f;
	FVector SmoothedGaze[2];
	bool bGazeInit = false;
	float BlinkTimer = 3.f;
	float BlinkTime = -1.f;
	float AttentionYaw = 0.f;
	float AttentionPitch = 0.f;

	// the lobby island (copied in PreUpdate: PinguLobby::GetAnimState; 2026-10-27: everybody sits or hangs on a spot of its own
	// and plays the waiting gags the server picks)
	bool bLobby = false;
	/** Seconds since this penguin was poked (clicked: a hop-spin; -1: not lately), and its way round (+1 / -1). */
	float LobbyPokeAge = -1.f;
	float LobbyPokeDir = 1.f;
	/** PinguLobby::EPose (0 stand, 1 sit, 2 hang), the side it hangs out to / turns from (+1: +X). */
	uint8 LobbyPose = 0;
	float LobbySide = 1.f;
	/** The pose's offset of the drawn body (component space, mesh units: copied from the world offset in PreUpdate). */
	FVector LobbyOffset = FVector::ZeroVector;
	/** PinguLobby::EGag, how far into it (s), its aim (+1 / -1 a neighbour on that side, 0 the camera). */
	uint8 LobbyGag = 0;
	float LobbyGagAge = -1.f;
	float LobbyGagDir = 0.f;
	/** A neighbour's noot (s ago; -1 none) and the side it flinches away to. */
	float LobbyFlinchAge = -1.f;
	float LobbyFlinchDir = 1.f;
	/** The beak's way now (world yaw, degrees: from the mesh component) and the yaw the lobby turns the body by (smoothed). */
	float LobbyFacingYaw = 0.f;
	float LobbyYaw = 0.f;
	bool bLobbyYawInit = false;
	/** The view's axis (world +Y: toward the camera) in component space: the sway's axis. */
	FVector LobbyViewAxis = FVector(0.f, 1.f, 0.f);
	/** The pose's weight (eases in when the lobby starts). */
	float LobbyPoseWeight = 0.f;
	/** Root bone (mesh index) and the body's height over the root in the rest pose (component units: the hops' scale). */
	int32 MeshRoot = INDEX_NONE;
	float BodyRestHeight = 50.f;
	/** Mesh units per cm (the mesh component's scale, copied in PreUpdate): the hops are in cm. */
	float MeshUnitsPerCm = 1.f;

	/** The trait's body shape (side, forward, up of the mesh; (1, 1, 1): none). */
	FVector BodyShape = FVector::OneVector;
	/** The Body bone's children and the feet (mesh bone indices), for ApplyBodyShape. */
	TArray<int32> MeshBodyChildren;
	TArray<int32> MeshFeet;

	// bone cache (by mesh bone index)
	bool bBonesCached = false;
	TArray<EBoneKind> BoneKinds;
	int32 MeshBody = INDEX_NONE;
	int32 MeshEye[2] = { INDEX_NONE, INDEX_NONE };
	FVector EyeRestLook[2];
	int32 EyeBlinkAxis[2] = { 2, 2 };
	FVector BodyRestForward = FVector(0.f, 1.f, 0.f);
	FVector BodyRestUp = FVector(0.f, 0.f, 1.f);

	int32 SeenJump = 0;
	int32 SeenAirJump = 0;
	int32 SeenDash = 0;
	int32 SeenSlide = 0;
	int32 SeenLand = 0;
	int32 SeenHit = 0;

	/**
	 * LAN: the penguin is not simulated on this machine, its movement state comes by replication - a jump or land counter
	 * can arrive a frame before the movement mode it belongs to. Such an event waits a moment for its mode (age, -1 none).
	 */
	bool bRemotePenguin = false;
	float PendingJumpAge = -1.f;
	float PendingAirJumpAge = -1.f;
	float PendingLandAge = -1.f;
};

/**
 * Native anim instance used directly as the mesh's AnimClass. All logic lives in the proxy
 * (FPenguinAnimInstanceProxy) so it runs in parallel with the game thread.
 */
UCLASS(Transient, NotBlueprintable)
class PENGUIN_API UPenguinAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;
};
