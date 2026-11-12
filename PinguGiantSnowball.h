// Pingu - the giant snowball (the 17th ability, 2026-10-29; Linus: "a new item called giant snow ball ... roll a snowball for a
// few seconds and cant move during that time ... it rolls in that direction and gets a bit bigger as it rolls and when it hits a
// wall it bounces off ... after 2 bounces max it dies at the next bounce or when it rolls of the map ... when a pingu gets hit by
// it it consumes the pingu ... when it gets destroyed the pingu gets out where it happened").
//
// The ability key held (UPenguinAbilityComponent): the penguin hangs still (UPenguinMovementComponent::SetHover) and pats a
// little ball that grows to 1 tile in front of it for PinguTuning::GiantBuildTime; let go sooner (or hit, frozen, knocked out)
// and it crumbles - the cooldown runs all the same. Then it rolls off by itself, slowly, along the ground: it drops down ledges,
// never rolls uphill (any step up is a wall), bounces back from walls and breaks on the third wall hit (or after
// GiantMaxLife). It grows as it rolls (1 -> 3 tiles); snowballs and the grow ray make it bigger past that, the shrink ray
// smaller. Nothing pushes it.
//
// Whatever it touches it swallows: penguins (anybody, its roller too: hidden, no collision, hanging still where they were, drawn
// tumbling in the ball), shots (stored) and ability bubbles. Broken, it lets them all out where it broke; rolled into the sea,
// the penguins inside go into the sea with it. What breaks it and what freezes it (it swallows nothing then and slides faster):
// BurstInArea / FreezeInArea, called by the bomb, the noot, the slap and the frost breath; itself it checks belly stomps,
// icebergs, Hot Feet and Ice Touch penguins, snowballs, ice bolts, rays and other giant snowballs. (INTERACTIONS.md has the table.)
//
// LAN: the server spawns and runs it; it replicates its motion (place and velocity with the server time: clients run it on
// from there and blend), its size, phase, frozen flag, wall hits, the penguins inside and its pop / bounce counters. Every
// machine draws it and the penguins tumbling in it, and keeps the swallowed penguins without collision. Hiding and the hover of
// the swallowed (the actor's hidden flag replicates; SetHover reaches a LAN player's own penguin), the release (NetTeleport)
// and the effects (FPinguFXBroadcastScope) are the server's.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PenguinProjectile.h"
#include "PenguinTypes.h"
#include "PinguGiantSnowball.generated.h"

class APenguinCharacter;
class ABlockIsland;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class USkeletalMeshComponent;
class UStaticMeshComponent;

UENUM()
enum class EPinguGiantPhase : uint8
{
	/** Being rolled up in front of its penguin (the key is held). */
	Building,
	/** Rolling on by itself. */
	Rolling,
	/** Broken, sunk or crumbled: it goes. */
	Done,
};

/** Where it is and how it moves, as the server had it at Time (server clock). */
USTRUCT()
struct FPinguGiantMotion
{
	GENERATED_BODY()

	UPROPERTY()
	float X = 0.f;

	UPROPERTY()
	float Z = 0.f;

	UPROPERTY()
	float VX = 0.f;

	UPROPERTY()
	float VZ = 0.f;

	UPROPERTY()
	double Time = 0.0;
};

UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguGiantSnowball : public AActor
{
	GENERATED_BODY()

public:
	APinguGiantSnowball();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Server or solo: Roller starts rolling one up toward Side (+1 right, -1 left), from the ability in Slot. Null on a LAN client
	 *  or when there is no room / no island. */
	static APinguGiantSnowball* BeginRollUp(APenguinCharacter* Roller, int32 Slot, float Side);
	static void GetAll(const UWorld* World, TArray<APinguGiantSnowball*>& Out);
	/** Every one goes at once (a new round): the penguins inside come back where they were (the round reset places them). */
	static void DestroyAll(UWorld* World);
	/** Server: every rolling giant snowball whose surface is within Radius of Center (and, with CosHalfAngle > -1, in the cone
	 *  along ConeDirection) breaks - By's doing (What for the log). Returns how many. The bomb's blast, the noot, the slap. */
	static int32 BurstInArea(UWorld* World, const FVector& Center, float Radius, APenguinCharacter* By, const TCHAR* What,
		const FVector& ConeDirection = FVector::ZeroVector, float CosHalfAngle = -2.f);
	/** Server: the same reach freezes them instead (the frost breath). Returns how many froze now. */
	static int32 FreezeInArea(UWorld* World, const FVector& Center, float Radius, const FVector& ConeDirection, float CosHalfAngle);
	/** The giant snowball Penguin is inside, if any (every machine). */
	static APinguGiantSnowball* FindHolding(const UWorld* World, const APenguinCharacter* Penguin);

	/** Server: the key was let go while it is still being rolled up (or the roll-up broke): it crumbles. The cooldown runs. */
	void CancelRollUp(const TCHAR* Why);
	/** Server: it breaks right here - the penguins inside pop out, what it swallowed flies out. By: whose doing (may be null). */
	void Burst(APenguinCharacter* By, const TCHAR* Why);
	/** Server: frozen for good - it swallows nothing more, slides faster and shoves the penguins it meets. */
	void Freeze(const TCHAR* Why);
	/** Server: DiameterDelta cm bigger (or smaller), within GiantMinSize .. GiantSafetyMaxSize (no rolling cap: snowballs, rays).
	 *  bPop: a visible pop (every machine). */
	void AddSize(float DiameterDelta, bool bPop);
	/** Server: its size x Factor (the rays: PinguGrowRayFactor / PinguShrinkRayFactor), within the same limits. */
	void ScaleSize(float Factor);

	EPinguGiantPhase GetPhase() const { return Phase; }
	bool IsRolling() const { return Phase == EPinguGiantPhase::Rolling; }
	bool IsBuilding() const { return Phase == EPinguGiantPhase::Building; }
	/** Its diameter (cm): what it is heading for (server: gameplay; every machine: replicated) and what is drawn / collides now. */
	float GetTargetDiameter() const { return Diameter; }
	float GetDiameter() const { return ShownDiameter; }
	float GetRadius() const { return ShownDiameter * 0.5f; }
	/** Where its middle is on this machine (the server's, or a client's smoothed copy). */
	FVector GetCenter() const { return GetActorLocation(); }
	bool IsFrozen() const { return bFrozen; }
	int32 GetWallHits() const { return WallHits; }
	/** +1 right, -1 left. */
	float GetDirection() const { return Direction; }
	APenguinCharacter* GetRoller() const { return Roller.Get(); }
	int32 GetSlot() const { return Slot; }
	const TArray<TObjectPtr<APenguinCharacter>>& GetInside() const { return Inside; }
	bool HoldsPenguin(const APenguinCharacter* Penguin) const;
	/** Server: what it has swallowed besides penguins (shots, ability bubbles) and how far it has rolled (cm). */
	int32 GetItemsInside() const { return Items.Num() + Bubbles.Num(); }
	float GetRolled() const { return Rolled; }
	/** Server, tests: how often it grew from a hit (snowballs, rays) and its last cause of death. */
	int32 GetSizeHits() const { return SizeHits; }
	/** Seconds since its roll-up started (server clock, every machine). */
	float GetAge() const;
	/** The roll-up's progress (0 .. 1) on this machine. */
	float GetBuildAlpha() const;
	/** Tests: the penguins drawn poking out of it on this machine (and the other things), its spin (degrees rolled), the vertices
	 *  of its lumpy mesh and how far its lumps reach out and in (share of the radius). */
	int32 GetShownInside() const;
	int32 GetShownStuck() const;
	int32 GetMeshVertices() const { return MeshVertices; }
	float GetLumpOut() const { return LumpOut; }
	float GetLumpIn() const { return LumpIn; }
	/** Codes of the things that poke out (Stuck). */
	static constexpr uint8 StuckSnowball = 1;
	static constexpr uint8 StuckIcicle = 2;
	static constexpr uint8 StuckBomb = 3;
	static constexpr uint8 StuckBubble = 16;
	static constexpr int32 MaxStuckSnowballs = 8;
	float GetSpinDegrees() const { return SpinDeg; }
	/** Tests: the snow puffs it has kicked up behind it on this machine. */
	int32 GetTrailPuffs() const { return TrailPuffs; }
	/** Server, tests: steps it has climbed, whether it is climbing one now, whether it may climb the next one (after a climb
	 *  not until it has rolled down a step), and how far it rolled without growing because a block above left no room (cm). */
	int32 GetClimbs() const { return Climbs; }
	bool IsClimbing() const { return bClimbing; }
	bool CanClimb() const { return bClimbReady; }
	float GetRoofHeld() const { return RoofHeld; }

	/** Tests (server): roll off now (the roll-up skipped), and a size set at once (drawn at once too). */
	void LaunchForTest();
	void SetSizeForTest(float NewDiameter);

	/** Server, the last one that went and why (tests read it after the actor is gone). */
	static FString LastEnd;
	static FVector LastEndAt;

private:
	// ---- server gameplay
	void TickBuild(float Dt);
	void TickRoll(float Dt);
	void Launch();
	/** Rolled into the sea (or fell past the island): the penguins inside go into the sea with it. */
	void Sink();
	/** Something solid in front of it above what it can climb (Dir: the side it rolls to). */
	bool HitsWall(const ABlockIsland* Island, float X, float Z, float R, float Dir, FVector* OutHit = nullptr) const;
	/** The highest its middle can be held up by the ground under it (false: nothing under it). Wider than GiantGapBridgeSize,
	 *  a gap one block wide holds it as if it were filled. */
	bool FindSupport(const ABlockIsland* Island, float X, float Z, float R, float& OutZ) const;
	/** No block over the top of a ball of radius R with its middle at X, Z (it may grow / climb there). */
	bool HasRoom(const ABlockIsland* Island, float X, float Z, float R) const;
	/** Rolled into a wall at Hit: a step one block high it may climb starts a climb (true); anything else is a wall (false). */
	bool TryStartClimb(const ABlockIsland* Island, float X, float Z, float R, const FVector& Hit);
	/** One tick of a climb: up the step's face, then over its corner (sets X, Z). */
	void TickClimb(float Dt, float Speed, float R, float& X, float& Z);
	void TouchPenguins();
	void TouchShots();
	void TouchBubbles();
	void TouchOthers();
	void Swallow(APenguinCharacter* Penguin);
	void Shove(APenguinCharacter* Penguin);
	/** Lets every penguin inside out at Where (bSea: into the water). */
	void Release(const FVector& Where, bool bSea);
	void PublishMotion(bool bForce);
	void End(const TCHAR* Why);

	// ---- every machine
	void TickLook(float Dt);
	void UpdateInsideLook(const FQuat& Spin, const FVector& Middle);
	/** The lumpy ball's mesh (unit radius): its detail by the effects preset, its snow texture's scale by Diameter. */
	void BuildBallMesh(float ForDiameter);
	/** Places what pokes out of it (the penguins' heads or feet, the shots, the bubbles) - all turn with the ball. */
	void UpdateStuckLook(const FQuat& Spin, const FVector& Middle);
	/** Server: one more thing poking out of it. */
	void AddStuck(uint8 Code);
	void UpdateCollisionOfInside();
	void PlayLocalSounds(float Dt);
	/** A small, soft trail of snow kicked up behind it while it rolls on the ground (cosmetic, this machine only). */
	void TickTrail(float Dt);
	UFUNCTION()
	void OnRep_Motion();

	UPROPERTY(VisibleAnywhere, Category = "GiantSnowball")
	TObjectPtr<USceneComponent> SceneRoot;

	/** The ball: a lumpy packed-snow ball made here (BuildBallMesh: unit radius, scaled to its size), turned as it rolls. */
	UPROPERTY(VisibleAnywhere, Category = "GiantSnowball")
	TObjectPtr<UProceduralMeshComponent> Ball;

	/** The island's snow (M_Block_Snow: its sparkle, mottle and soft blue shadows) and, frozen, its ice (M_Block_Ice). */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SnowMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> IceMaterial;

	/** What it swallowed besides penguins, poking out of its snow (one part each: snowballs, icicles, bombs, bubbles). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> StuckParts;

	/** The penguins tumbling inside, one copy each (their mesh and colours, the reference pose). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> Puppets;

	// ---- replicated
	UPROPERTY(Replicated)
	EPinguGiantPhase Phase = EPinguGiantPhase::Building;

	UPROPERTY(Replicated)
	TObjectPtr<APenguinCharacter> Roller;

	UPROPERTY(Replicated)
	float Direction = 1.f;

	UPROPERTY(Replicated)
	double BuildStart = 0.0;

	UPROPERTY(Replicated)
	float Diameter = 100.f;

	UPROPERTY(Replicated)
	bool bFrozen = false;

	UPROPERTY(Replicated)
	int32 WallHits = 0;

	UPROPERTY(Replicated)
	TArray<TObjectPtr<APenguinCharacter>> Inside;

	/** Bumped for a pop (grown or shrunk by a hit) and a wall bounce: every machine squashes the ball once. */
	UPROPERTY(Replicated)
	uint8 PopSerial = 0;

	UPROPERTY(Replicated)
	uint8 BounceSerial = 0;

	UPROPERTY(ReplicatedUsing = OnRep_Motion)
	FPinguGiantMotion Motion;

	/** Its lumps (every machine builds the same ball from it). */
	UPROPERTY(Replicated)
	int32 LookSeed = 1;

	/** What pokes out of it besides penguins, in the order it came in: StuckSnowball, StuckIcicle, StuckBomb, or
	 *  StuckBubble + the ability (an ability bubble). Snowballs that grew it stay stuck in it too (at most MaxStuckSnowballs). */
	UPROPERTY(Replicated)
	TArray<uint8> Stuck;

	// ---- server state
	int32 Slot = INDEX_NONE;
	float VX = 0.f;
	float VZ = 0.f;
	bool bGrounded = true;
	float Rolled = 0.f;
	double RollStart = 0.0;
	/** The step it climbs: the x of its face, the z of its top, the column, since when (s); after a climb the floor it climbed
	 *  onto (the next climb waits until it is below that by half a block). */
	bool bClimbing = false;
	bool bClimbReady = true;
	float ClimbFaceX = 0.f;
	float ClimbTopZ = 0.f;
	float ClimbFloorZ = 0.f;
	float ClimbTime = 0.f;
	FIntPoint ClimbCell = FIntPoint::ZeroValue;
	int32 Climbs = 0;
	float RoofHeld = 0.f;
	float PatTimer = 0.f;
	int32 PatCount = 0;
	float HoverTimer = 0.f;
	float PublishTimer = 0.f;
	int32 SizeHits = 0;
	struct FStoredShot
	{
		EPenguinProjectileKind Kind = EPenguinProjectileKind::Snowball;
		TWeakObjectPtr<APenguinCharacter> Shooter;
		float Size = 1.f;
	};
	TArray<FStoredShot> Items;
	TArray<EPenguinAbility> Bubbles;
	/** Frozen: when each penguin was last shoved (server time). */
	TMap<TWeakObjectPtr<APenguinCharacter>, double> Shoved;

	// ---- this machine
	float ShownDiameter = 100.f;
	float SpinDeg = 0.f;
	float PopTime = 10.f;
	float BounceTime = 10.f;
	uint8 SeenPop = 0;
	uint8 SeenBounce = 0;
	float RollSoundTimer = 0.f;
	/** The diameter its snow texture was laid out for (rebuilt when it grows or shrinks far from it), and its detail. */
	float MeshBuiltFor = 0.f;
	int32 MeshDetail = -1;
	/** Where it was drawn last frame (X): its spin follows the way it really went on this machine. */
	float PrevLookX = 0.f;
	float PrevLookZ = 0.f;
	bool bLookReady = false;
	/** Clients: the motion's place they run on from (blended into where they draw it). */
	FVector SmoothError = FVector::ZeroVector;
	/** Penguins this machine has taken the collision from (given back when they leave the ball or it goes). */
	TArray<TWeakObjectPtr<APenguinCharacter>> NoCollision;
	int32 MeshVertices = 0;
	/** The snow it kicks up behind it while it rolls (every machine its own): time to the next puff, and the puffs made (tests). */
	float TrailTimer = 0.f;
	int32 TrailPuffs = 0;
	float LumpOut = 0.f;
	float LumpIn = 0.f;
};
