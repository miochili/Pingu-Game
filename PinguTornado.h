// Pingu - the tornado (the Gust ability): a short whirlwind round its user that sucks everything in and throws it out.
//
// For PinguTuning::TornadoTime (1 s) the user hangs perfectly still in the tornado's eye (UPenguinMovementComponent::
// SetHover; the ability sets that). For the first TornadoSuckTime (0.5 s) everything within its reach is sucked in: other
// penguins are pulled toward the eye (UPenguinMovementComponent::SetTornadoPull), shots and ability bubbles circle it,
// falling icebergs are drawn in, and the island's ice blocks in reach are ripped out (AHookedIceBlock::RipOut) and circle
// it too. Then it throws everything out at once - a penguin with a normal hit (the nearer the middle, the harder: half the
// noot's point-blank push at the middle, 15 % at the edge), shots as its user's own, the ice as falling ice that crushes
// whoever it comes down on (the user's knock-out). Then it winds down.
//
// Two tornadoes whose reaches overlap suck each other in and merge into one giant tornado (BeginMerge, Absorb; the numbers
// are PinguTuning::TornadoMerge*): both eyes drift together with their users in them, then the bigger one (the older one of
// two alike) takes the other in - reach and push added up, its funnel as wide as both, both users side by side in its eye,
// neither one touched by it. It sucks on for at least half a second (again, if it had thrown already), throws, and lasts
// until the later of the two would have ended plus half a second. A third one that reaches it merges in the same way. What
// it throws counts for the user whose own tornado started nearest to it (GetCreditFor).
//
// LAN: the server spawns it and runs all of that; it replicates (users, eye and its drift, reach, size, the times of its
// throw and its end on the server clock, its merges), and every machine draws the funnel, the wind, the merge and the
// throw's burst from that by itself (no effect needs a broadcast). The pulls, hovers, the users' drift into the giant's eye
// and the hits reach a LAN player's own penguin through the movement component's forwarded effects; ripped ice leaves the
// island with the island's own replication.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "GameFramework/Actor.h"
#include "PinguTornado.generated.h"

class AHookedIceBlock;
class APenguinCharacter;
class APenguinProjectile;
class APinguPickup;
class AIcebergDrop;
class APinguFX;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UStaticMeshComponent;

UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguTornado : public AActor
{
	GENERATED_BODY()

public:
	APinguTornado();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Server or solo: a tornado round User's chest, its reach TornadoRadius x the user's size x ReachScale (Traits mode: the
	 *  use's strength's reach), its push x Strength. Null on a LAN client. */
	static APinguTornado* Spawn(APenguinCharacter* User, float Strength, float ReachScale);
	static void GetAll(const UWorld* World, TArray<APinguTornado*>& Out);
	/** Every tornado goes at once (a new round). */
	static void DestroyAll(UWorld* World);

	/** The penguin that started it (a giant: the one whose tornado took the others in). */
	APenguinCharacter* GetUser() const;
	/** Everybody hanging in it: its user, and in a giant the users of the tornadoes it took in (every machine). */
	const TArray<TObjectPtr<APenguinCharacter>>& GetUsers() const { return Users; }
	bool IsUser(const APenguinCharacter* Penguin) const;
	/** The eye (its user's chest when it started; while two merge it drifts to where they meet, then it stays there). */
	FVector GetCenter() const;
	float GetRadius() const { return Radius; }
	/** Seconds since it started, on the server's clock (every machine). */
	float GetAge() const;
	/** Seconds from its start to its throw and to its end (every machine; a merge puts both later). */
	float GetThrowAge() const { return static_cast<float>(ThrowTime - StartTime); }
	float GetEndAge() const { return static_cast<float>(EndTime - StartTime); }
	/** It has thrown everything out (the server: when it did; a client: by the clock). */
	bool HasThrown() const;
	/** How many tornadoes it has taken in (0: a plain one; every machine). */
	int32 GetMerges() const { return Merges; }
	/** Two are drifting together right now (server). */
	bool IsMerging() const { return MergeWith.IsValid(); }
	/** Its push x this (a plain one: the use's strength; a giant: the strengths added up) and the distance its push falls off
	 *  over (server). */
	float GetStrength() const { return Strength; }
	float GetFalloffRadius() const { return FalloffRadius; }
	/** The knock a penguin Distance from its middle gets at the throw, before the hit's own scaling (sizes, the fight's growth,
	 *  traits): the noot's point-blank push x the share there x its strength (cm/s, without the lift; server). */
	float GetPushAt(float Distance) const;
	/** Of its users, the one a thing At gets thrown by: whose own tornado started nearest to it (server). */
	APenguinCharacter* GetCreditFor(const FVector& At) const;

	// ---- what it did (server; the tests and the logs read it)
	/** A penguin it threw: how far from the middle it was then, the share of the push it got, the knock it was given (before
	 *  the hit's own scaling: size, the fight's growth, traits, its strength), whether its shell took it, whose hit it was. */
	struct FThrowRecord
	{
		TWeakObjectPtr<APenguinCharacter> Penguin;
		TWeakObjectPtr<APenguinCharacter> By;
		float Distance = 0.f;
		float Share = 0.f;
		FVector2D Knockback = FVector2D::ZeroVector;
		bool bShell = false;
		bool bHit = false;
	};
	const TArray<FThrowRecord>& GetThrows() const { return Throws; }
	/** Penguins it pulled in, shots it sucked in and threw out, ability bubbles, icebergs, ice blocks ripped out. */
	int32 GetPulledCount() const { return Pulled.Num(); }
	int32 GetShotsSucked() const { return SuckedShots.Num(); }
	int32 GetShotsThrown() const { return ShotsThrown; }
	int32 GetBubblesSucked() const { return SuckedBubbles.Num(); }
	int32 GetIcebergsDrawn() const { return DrawnIcebergs.Num(); }
	int32 GetIceRipped() const { return IceRipped; }
	/** Ice blocks it holds in its wind right now (a giant: its own and those of the ones it took in). */
	int32 GetIceHeld() const;
	/** Where the ice blocks it ripped out were (cells of the island, in the order they went). */
	const TArray<FIntPoint>& GetRippedCells() const { return RippedCells; }
	/** The look on this machine (tests): the pieces of its wind bands, the snow puffs at its foot and the bits swirling in
	 *  it, and how much of it shows now (0 .. 1: it forms, then winds down after the throw). */
	void GetLookCounts(int32& OutBands, int32& OutDust, int32& OutDebris) const
	{
		OutBands = NumBands;
		OutDust = NumDust;
		OutDebris = NumDebris;
	}
	float GetLookAmount() const { return LookAmount; }
	/** The look on this machine (tests): the funnel's radius at its top and its height as drawn now (cm), merge flashes shown. */
	float GetFunnelTopRadius() const { return ShownTopRadius; }
	float GetFunnelHeight() const { return ShownHeight; }
	int32 GetMergeFlashesShown() const { return MergeFlashes; }
	/** The merge's look on this machine (tests): how far its funnel's top leans toward where it meets the other one now (cm),
	 *  its squash and stretch now (its height x this: under 1 squashed, over 1 stretched), the swirl pieces and the shock rings'
	 *  pieces shown now, and the merge sounds it has started. */
	float GetMergeLean() const { return ShownLean; }
	float GetMergePop() const { return ShownPop; }
	int32 GetMergeSwirlShown() const { return SwirlShown; }
	int32 GetMergeRingsShown() const { return RingsShown; }
	int32 GetMergeSoundsShown() const { return MergeSounds; }
	/** Ultra: the 3D swirl strands drawn (pieces; 0 on the other presets). */
	int32 GetStrandCount() const { return NumStrands; }
	/** Its own look (tests): the random seed it was given, and how far its funnel's top stands off its eye sideways now (cm). */
	int32 GetLookSeed() const { return LookSeed; }
	float GetTopShift() const { return ShownTopShift; }

private:
	/** Server: one frame - the merges, the suck until the throw, the throw, the end. */
	void TickGameplay(float Dt);
	/** Server: everything in reach is drawn in (penguins once, shots and ripped ice every frame). */
	void Suck(float Dt);
	/** Server: everything goes out at once. */
	void ThrowOut();
	/** Server: the ice blocks in reach leave the island and join the wind (at the start, and when it grows in a merge) - until
	 *  it has ripped MaxBlocks out. */
	void RipIce(int32 MaxBlocks);
	/** Server: Other is one it reaches now - in reach, not behind a wall, not one of its users, not hanging in its own tornado. */
	bool Reaches(const APenguinCharacter* Other) const;
	/** The push share at Distance from the middle (TornadoPushCenterShare .. TornadoPushEdgeShare). */
	float GetShareAt(float Distance) const;
	/** Where a thing circling the eye is At seconds after the wind took it: an ellipse round Eye (X wide, Z flat), raised by
	 *  Raise, Phase radians round, its radius closing in from StartRadius to OrbitRadius. */
	FVector2D OrbitPoint(const FVector& Eye, float Phase, float StartRadius, float OrbitRadius, float Raise, float SpinSpeed, float At) const;
	/** The funnel's width x the users' size (1 + half its growth: a giant's eye and the rings things circle in are wider). */
	float GetSpread() const;
	/** Where a penguin it sucks in stops: this far from the eye. */
	float GetEyeRadius() const;

	// ---- merging (server)
	/** It can still merge: not taken in, not drifting toward another one already, and not about to end. */
	bool CanMerge(double Now) const;
	/** Looks for another tornado whose reach overlaps its own; the two start drifting together. */
	void TryLink(double Now);
	/** This one (the bigger, or the older of two alike) and Other drift together: both eyes and their users toward the point
	 *  between them; neither throws before the merge, and the giant's throw and end are set. */
	void BeginMerge(APinguTornado* Other, double Now);
	/** The drift is over: Other is taken in (its users, what it holds, its reach and push added) and goes. */
	void Absorb(APinguTornado* Other, double Now);
	/** The users still hanging in it hang on until its end, side by side in the eye at Eye; GlideTime > 0: they drift there
	 *  in that long (UPenguinMovementComponent::SetHoverGlide). */
	void PlaceUsers(const FVector& Eye, float GlideTime, double Now);
	/** Every penguin it is pulling in now is pulled toward Eye until its throw (Reach: the pull's reach). */
	void RepullAll(const FVector& Eye, float Reach, double Now);
	/** The eye drifts from where it is now to To over Seconds (replicated: every machine draws it moving). */
	void DriftTo(const FVector& To, float Seconds, double Now);

	/** Every machine: the funnel, the wind, the merge and the throw's burst, from the clock. */
	void TickLook(float Dt);
	void BuildLook();
	/** One soft wind streak (an effects particle, this machine only) flying from From at Velocity. */
	void SpawnStreak(APinguFX* FX, const FVector& From, const FVector& Velocity, float Life, float Opacity);
	/** Every machine: the flash, whoosh and burst of wind where two tornadoes have just become one. */
	void ShowMergeFlash(const FVector& Eye);
	/** Every machine: the merge's own pieces - the swirl of snow spiralling in where two meet while they drift together (the
	 *  lead's; flung out when they become one) and the two shock rings of the merge. */
	void TickMergeLook(const FVector& Eye, double Now, float S, float Wide, float Tall, float Foot);

	UPROPERTY(VisibleAnywhere, Category = "Tornado")
	TObjectPtr<USceneComponent> SceneRoot;

	/** The funnel's wind bands (soft glowing ribbons going round it, M_Trail), the snow puffs whirling at its foot and the
	 *  snow and ice bits swirling in it (M_FX). */
	UPROPERTY(VisibleAnywhere, Category = "Tornado")
	TObjectPtr<UInstancedStaticMeshComponent> Bands;

	UPROPERTY(VisibleAnywhere, Category = "Tornado")
	TObjectPtr<UInstancedStaticMeshComponent> Dust;

	/** The funnel itself: a glassy twister of wind, wide over the user's head and narrow at its feet (M_Ghost: see-through, a
	 *  bright rim), inside the bands - a mesh of rings round its swaying axis, rebuilt every frame. */
	UPROPERTY(VisibleAnywhere, Category = "Tornado")
	TObjectPtr<UProceduralMeshComponent> Funnel;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FunnelMaterial;

	UPROPERTY(VisibleAnywhere, Category = "Tornado")
	TObjectPtr<UInstancedStaticMeshComponent> Debris;

	/** The merge: snow spiralling in where two meet (M_FX flakes) and the shock rings when they become one (M_Trail ribbons). */
	UPROPERTY(VisibleAnywhere, Category = "Tornado")
	TObjectPtr<UInstancedStaticMeshComponent> Swirl;

	UPROPERTY(VisibleAnywhere, Category = "Tornado")
	TObjectPtr<UInstancedStaticMeshComponent> Rings;

	/** Ultra: the 3D swirls - thin twisting streaks spiralling up round it in layers (M_Trail), instead of the bands. */
	UPROPERTY(VisibleAnywhere, Category = "Tornado")
	TObjectPtr<UInstancedStaticMeshComponent> Strands;

	// ---- replicated
	/** Set before its first replication, never changed. */
	UPROPERTY(Replicated)
	TObjectPtr<APenguinCharacter> User;

	/** User, and the users of the tornadoes it took in. */
	UPROPERTY(Replicated)
	TArray<TObjectPtr<APenguinCharacter>> Users;

	/** Where the eye is (or, while two drift together, where it is going: from CenterFrom, over MoveTime from MoveStart). */
	UPROPERTY(Replicated)
	FVector_NetQuantize10 Center = FVector::ZeroVector;

	UPROPERTY(Replicated)
	FVector_NetQuantize10 CenterFrom = FVector::ZeroVector;

	UPROPERTY(Replicated)
	double MoveStart = 0.0;

	UPROPERTY(Replicated)
	float MoveTime = 0.f;

	UPROPERTY(Replicated)
	float Radius = 600.f;

	/** The users' size (the funnel's foot: their feet); a giant: the biggest user's. */
	UPROPERTY(Replicated)
	float SizeScale = 1.f;

	/** The funnel's width / SizeScale: 1 a plain tornado, about the number of tornadoes in it for a giant. */
	UPROPERTY(Replicated)
	float Grown = 1.f;

	/** Server clock times: it started (set before its first replication, never changed), it throws, it ends. */
	UPROPERTY(Replicated)
	double StartTime = 0.0;

	UPROPERTY(Replicated)
	double ThrowTime = 0.0;

	UPROPERTY(Replicated)
	double EndTime = 0.0;

	/** Its own look's random seed (set by the server at the spawn, never changed): tilt, S curve, bulges, spin, strands - so no
	 *  two tornadoes look alike and every LAN machine draws the same one. */
	UPROPERTY(Replicated)
	int32 LookSeed = 0;

	/** Tornadoes it has taken in (every machine shows a flash for each new one). */
	UPROPERTY(Replicated)
	uint8 Merges = 0;

	/** Drifting together with another one, this one takes the other in (the lead; replicated: on every machine the lead
	 *  draws the swirl between them and plays the merge sound, once for the two). */
	UPROPERTY(Replicated)
	bool bMergeLead = false;

	/** The distance its push falls off over (TornadoRadius x the user's size: the strength's reach does not stretch it - a
	 *  stronger tornado reaches further, and pushes x its strength at every distance; a giant: the two added up). Server. */
	float FalloffRadius = 600.f;

	// ---- server gameplay
	float Strength = 1.f;
	bool bThrown = false;
	bool bIceRipped = false;
	/** Taken in by another one (it goes; nothing of it happens any more). */
	bool bAbsorbed = false;
	/** Where each of Users was when its own tornado started (the credit for what a giant throws). */
	TArray<FVector> UserEyes;
	/** Drifting together with this one: this one takes it in at MergeAt (bMergeLead), or is taken in by it. */
	TWeakObjectPtr<APinguTornado> MergeWith;
	double MergeAt = 0.0;
	TArray<TWeakObjectPtr<APenguinCharacter>> Pulled;
	struct FHeldShot
	{
		TWeakObjectPtr<APenguinProjectile> Shot;
		float Phase = 0.f;
		float StartRadius = 0.f;
		float StartAge = 0.f;
	};
	TArray<FHeldShot> SuckedShots;
	TArray<TWeakObjectPtr<APinguPickup>> SuckedBubbles;
	TArray<TWeakObjectPtr<AIcebergDrop>> DrawnIcebergs;
	struct FHeldIce
	{
		TWeakObjectPtr<AHookedIceBlock> Block;
		float Phase = 0.f;
		float StartRadius = 0.f;
		float StartAge = 0.f;
		FVector2D Last = FVector2D::ZeroVector;
	};
	TArray<FHeldIce> HeldIce;
	TArray<FThrowRecord> Throws;
	TArray<FIntPoint> RippedCells;
	int32 ShotsThrown = 0;
	int32 IceRipped = 0;

	// ---- every machine: the look
	bool bLookBuilt = false;
	/** The throw time whose burst was shown (a merge sets a new one). */
	double BurstShownFor = -1.0;
	bool bStartShown = false;
	int32 MergesShown = 0;
	int32 MergeFlashes = 0;
	/** Server clock time of the last merge flash shown here (the funnel flares bright for a moment after it). */
	double MergeFlashAt = -1.0;
	/** The drift (its MoveStart) whose merge sound and swirl were started here; where the swirl's pieces start (cm from where
	 *  the two meet). */
	double DriftShownFor = -1.0;
	float SwirlStartRadius = 0.f;
	int32 MergeSounds = 0;
	float ShownLean = 0.f;
	float ShownPop = 1.f;
	int32 SwirlShown = 0;
	int32 RingsShown = 0;
	int32 NumSwirl = 0;
	int32 NumStrands = 0;
	int32 RingSegments = 0;
	/** The width drawn now: it grows to Grown over a moment after a merge. */
	float ShownGrown = 1.f;
	float ShownTopRadius = 0.f;
	float ShownHeight = 0.f;
	float LookAge = 0.f;
	float LookAmount = 0.f;
	float PuffTimer = 0.f;
	float SpinAngle = 0.f;
	/** Instances per component (Bands: layers x pieces). */
	int32 NumBands = 0;
	int32 NumDust = 0;
	int32 NumDebris = 0;
	int32 BandLayers = 0;
	int32 BandSegments = 0;
	/** Its shape's waves: their phases (from its start time: the same on every machine). */
	float ShapePhase[8] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	/** Its own shape (from LookSeed, rolled again at each merge): its tilt (-1 .. 1 of the largest), how far and how fast the tilt
	 *  wanders (two slow waves), the S curve's strength, waves up its height and speed, the bulges' size, count and speed, its
	 *  spin and the strands' turns (x), its sway in depth. */
	float LeanBase = 0.f;
	float LeanWander = 0.f;
	float LeanWanderRate[2] = { 1.f, 2.f };
	float LeanWanderPhase[2] = { 0.f, 0.f };
	float CurveStrength = 0.8f;
	float CurveWaves = 1.5f;
	float CurveSpeed = 2.2f;
	float BulgeSize = 0.2f;
	float BulgeWaves = 1.7f;
	float BulgeSpeed = 0.5f;
	float SpinScale = 1.f;
	float TurnScale = 1.f;
	float DepthSway = 0.35f;
	float ShownTopShift = 0.f;
	/** The glass funnel's mesh: rings x (sides + 1) vertices. */
	int32 GlassRings = 0;
	int32 GlassSides = 0;
	bool bGlassMade = false;
	int32 GlassSkipped = 0;
	TArray<FVector> GlassVertices;
	TArray<FVector> GlassNormals;
	TArray<FColor> GlassColors;
	/** The glass is M_TornadoGlass (soft outline), not the M_Ghost fallback. */
	bool bSoftGlass = false;
	int32 StrandLayers = 0;
	TArray<FTransform> TransformScratch;
	FRandomStream LookRand;
};
