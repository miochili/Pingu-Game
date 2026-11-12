// Pingu - ability bubbles. A random one floats down from the sky every few seconds and every knocked-out
// penguin drops one of its abilities. Touch a bubble to swap its ability into your middle (MMB) slot.
//
// LAN (v2): bubbles are spawned, moved, pushed, taken and popped by the server only. They replicate (always relevant):
// the ability, the spawn time on the server clock and a motion snapshot (location, velocity, resting) that the server
// sends when the bubble's motion changes and a few times a second while it floats. Clients fly their copy on from the
// latest snapshot with the same physics and blend out the small corrections; the look (pop-in, bob, blink, glow trail,
// landing ring) is made on every machine.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PenguinTypes.h"
#include "PinguPickup.generated.h"

class ABlockIsland;
class APenguinCharacter;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/** Where a bubble is and how it moves, as the server last saw it. */
USTRUCT()
struct FPinguPickupMotion
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	/** cm/s on the play plane (X right, Z up). */
	UPROPERTY()
	FVector Velocity = FVector::ZeroVector;

	/** Server world time of the snapshot (clients catch up the time it took to arrive). */
	UPROPERTY()
	double ServerTime = 0.0;

	UPROPERTY()
	bool bResting = false;

	/** Rolling along the ground (friction instead of air drag). */
	UPROPERTY()
	bool bSliding = false;

	/** Bumped by every push (the bubble wobbles on every machine). */
	UPROPERTY()
	uint8 PushCount = 0;
};

UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguPickup : public AActor
{
	GENERATED_BODY()

public:
	APinguPickup();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Server only (nullptr on a LAN client). A bubble holding Ability at Location, starting with InVelocity (cm/s on
	 *  the play plane: X right, Z up). */
	static APinguPickup* Spawn(UWorld* World, EPenguinAbility Ability, const FVector& Location, const FVector& InVelocity = FVector::ZeroVector);
	/** Server only. A random ability floats down from the sky over a random spot of the island that has ground under it. */
	static APinguPickup* SpawnFromSky(UWorld* World, FRandomStream& Rand);
	/** Server only. A knocked-out penguin drops one of its abilities: it pops out where the penguin was, or (fallen into
	 *  the sea) floats down from the sky over the nearest part of the island. */
	static APinguPickup* SpawnDrop(UWorld* World, EPenguinAbility Ability, const FVector& Where, bool bFellIntoSea);

	/** Every bubble that can still be taken (every machine). */
	static void GetAll(const UWorld* World, TArray<APinguPickup*>& Out);
	static int32 CountAll(const UWorld* World);
	/** Server only (the clients' copies go with the server's). */
	static void DestroyAll(UWorld* World);

	/** Server only. Knocked by a shot, a blast or a gust (cm/s added to its velocity). A bubble on a hook ignores it. */
	void Push(const FVector& Impulse);
	/** Through a portal: somewhere else at once, flying on with the speed it had. Server only (clients snap there). */
	void WarpTo(const FVector& NewLocation);
	/** Server only. Every bubble in the circle (a cone if CosHalfAngle > -1) is pushed away from Origin (or along
	 *  ConeDirection), Lift cm/s up on top (wind blows bubbles off the ground: the gust); both full in the middle, half at the
	 *  edge. Returns how many. */
	static int32 PushInArea(UWorld* World, const FVector& Origin, float AreaRadius, float Strength,
		const FVector& ConeDirection = FVector::ZeroVector, float CosHalfAngle = -2.f, float Lift = 0.f);

	/** Server only. Caught by a fish hook: the bubble rides on the hook (on every machine) until the hook hands it over
	 *  (GiveTo) or lets go (ReleaseFromHook). Nobody can take it on the way. */
	void AttachToHook(AActor* Hook);
	/** Server only. Off the hook, floating on with InVelocity (cm/s on the play plane). */
	void ReleaseFromHook(const FVector& InVelocity = FVector::ZeroVector);
	/** Riding on a fish hook. */
	bool IsOnHook() const { return IsValid(ReelHook); }
	/** Server only. This penguin takes the bubble right now (reeled in by its hook, grabbed by its snow warp). False if
	 *  it is gone already or the penguin can't take it (knocked out, hidden). */
	bool GiveTo(APenguinCharacter* Penguin);

	EPenguinAbility GetAbility() const { return Ability; }
	/** Centre of the bubble (the HUD draws the ability icon here). */
	FVector GetIconLocation() const;
	/** Pops in when it appears (0 -> 1, overshooting a little). */
	float GetIconScale() const;
	/** 1 normally, blinking during its last seconds. */
	float GetOpacity() const;
	float GetRadius() const { return Radius; }
	/** Lying on the ground (not floating down). */
	bool IsResting() const { return bResting; }
	/** Can be taken right now (not in the short delay after it pops out of a knocked-out penguin, not on a hook). */
	bool CanBeTaken() const { return PickupDelay <= 0.f && !bTaken && !IsOnHook(); }
	float GetTimeLeft() const { return Life - Age; }

	/** Bubble radius (cm): a penguin touching it takes it. */
	static constexpr float BubbleRadius = 42.f;
	/** Top speed while floating down (cm/s). */
	static constexpr float FallSpeed = 320.f;
	/** Seconds before an untouched bubble pops by itself. */
	static constexpr float Lifetime = 22.f;

protected:
	virtual void BeginPlay() override;

private:
	void Collect(APenguinCharacter* Penguin);
	/** Gone: sparkles (or a splash in the sea), then destroyed. */
	void Pop(bool bSplash);
	/** The first live penguin touching the bubble. */
	APenguinCharacter* FindToucher() const;
	/** One step of the bubble's flight on Location (and Velocity, bResting, bSliding). False when nothing could move. */
	bool MoveBubble(float Dt, FVector& Location);
	void UpdateLook(float Dt);
	/** Loads the bubble mesh and material (once) and tints it in the ability colour. */
	void SetupLook();
	/** Bubble scale (pop-in, wobble), bob and opacity. */
	void ApplyLook();
	/** A flat ring on the ground where the floating bubble will land. */
	void ShowLandingMarker();
	/** Inside a block of the island (an ice wall grew here, or it popped out inside one): up onto that column. */
	bool EscapeBlocks(FVector& InOutLocation) const;

	/** Server: copies the motion into the replicated snapshot when it changed (or now and then while floating). */
	void PublishMotion(float Dt, bool bForce);
	/** LAN client: age, bob and take delay from the server clock; flies on from the last snapshot. */
	void TickClient(float Dt);
	/** The server's game clock (the same on every machine). */
	double GetServerNow() const;

	UFUNCTION()
	void OnRep_Ability();

	UFUNCTION()
	void OnRep_Motion();

	UPROPERTY(VisibleAnywhere, Category = "Pickup")
	TObjectPtr<UStaticMeshComponent> Bubble;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BubbleMaterial;

	// ---- replicated (written on the server only)
	UPROPERTY(Transient, ReplicatedUsing = OnRep_Ability)
	EPenguinAbility Ability = EPenguinAbility::None;

	/** Server world time it appeared (clients' age, pop-in and blink). */
	UPROPERTY(Transient, Replicated)
	double SpawnTime = 0.0;

	/** Bob phase at spawn (bubbles don't bob in step). */
	UPROPERTY(Transient, Replicated)
	float BobStart = 0.f;

	/** Nobody may take it during its first seconds (a drop from a knocked-out penguin). */
	UPROPERTY(Transient, Replicated)
	float TakeDelay = 0.f;

	UPROPERTY(Transient, ReplicatedUsing = OnRep_Motion)
	FPinguPickupMotion Motion;

	/** The fish hook reeling the bubble in: every machine draws the bubble on its own copy of the hook. */
	UPROPERTY(Transient, Replicated)
	TObjectPtr<AActor> ReelHook;

	// ---- simulation (the server's is the real one, a client's follows the snapshots)
	FVector Velocity = FVector::ZeroVector;
	float Radius = BubbleRadius;
	float Age = 0.f;
	float Life = Lifetime;
	float Bob = 0.f;
	float MarkerTimer = 0.f;
	float SparkleTimer = 0.f;
	float PushCooldown = 0.f;
	/** Nobody can take it for a moment after it pops out of a knocked-out penguin (so everyone sees it). */
	float PickupDelay = 0.f;
	FVector LastTrailPoint = FVector::ZeroVector;
	bool bTrailStarted = false;
	bool bResting = false;
	bool bTaken = false;

	/** The island it floats over (sea level, blocks growing under it). */
	TWeakObjectPtr<ABlockIsland> HomeIsland;
	/** Extra wobble after a bounce or a push (1 -> 0). */
	float Jiggle = 0.f;
	float AppliedOpacity = -1.f;
	/** Touched the ground last frame while moving (friction instead of air drag). */
	bool bSliding = false;
	bool bLookLoaded = false;

	// ---- server: snapshot sending
	float PublishTimer = 0.f;
	/** The motion jumped (push, bounce, freed from a block): send it at once. */
	bool bMotionKick = false;
	uint8 PushSerial = 0;

	// ---- LAN client
	/** Where the client's own flight has the bubble; it is shown at SimLocation + VisualOffset (the offset of the last
	 *  correction, fading out). */
	FVector SimLocation = FVector::ZeroVector;
	FVector VisualOffset = FVector::ZeroVector;
	bool bHasMotion = false;
	uint8 SeenPushCount = 0;
};
