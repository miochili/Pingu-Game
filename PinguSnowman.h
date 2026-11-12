// Pingu - the snowman (the 18th ability, 2026-11-03; Linus: "I really like the snowman but make it last for ever and just die from
// like everything just like the ice blocks. Hes balls also have the combo but dont get instantly back and he cant move").
//
// The ability (UPenguinAbilityComponent::DoSnowman) builds one on the tile (grid block) in front of its builder (a quick build:
// three balls popping up, a carrot, coal eyes, stick arms and a hat in its builder's colour). It stands there until something
// breaks it or the round ends - it never moves; its tile gone (broken, melted, fallen, sunk), it breaks. As many as you like
// (2026-11-05), one per tile: a taken tile puts it on the next tile on, never further; in the air, past the build limit
// (PinguBuildLimit) or on a taken next tile nothing is built (a "no" puff, nothing spent). Every PinguTuning::SnowmanReload s it
// throws a snowball at the nearest enemy it can see within SnowmanRange (never at its builder or the builder's teammates); after a
// hit the next one comes SnowmanHitReload s after the last. The snowball is its builder's: its hit goes on with the
// builder's snowball combo (and pushes that much harder), but gives no snowball back (APenguinProjectile::SetNoRefund).
//
// Almost everything breaks it, as it breaks the island's ice blocks: shots that touch it (snowballs - not its own -, icicles, ice
// bolts, the fish hook; a bomb goes off on it), the slap, the noot, frost, a belly stomp's landing, a bomb's blast (BreakInArea from
// those abilities), a tornado reaching it, a Giant Snowball rolling into it, falling icebergs and falling ice, a sliding, stomping
// or Hot Feet penguin touching it, a block appearing where it stands. The shrink / grow rays resize it (and its snowballs);
// shrunk past SnowmanMinSize it melts away. Penguins walk past it (no collision). The table: INTERACTIONS.md.
//
// LAN: the host spawns it, aims and throws (the snowballs are ordinary replicated projectiles) and breaks it; it replicates its
// builder, size, build time, throws (for the arm's swing) and its place (only a fall moves it). Effects are broadcast.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PinguSnowman.generated.h"

class APenguinCharacter;
class APenguinProjectile;
class ABlockIsland;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UProceduralMeshComponent;

UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguSnowman : public AActor
{
	GENERATED_BODY()

public:
	APinguSnowman();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Server or solo: Builder builds one at Feet (the top of its tile, the tile's middle), facing Side (+1 right, -1 left). Null
	 *  on a LAN client. */
	static APinguSnowman* Build(APenguinCharacter* Builder, const FVector& Feet, float Side);
	/** Server: where Builder would build one now, aiming toward Side: on the tile in front of it (a wall there: its own tile), the
	 *  next tile on if that one has a snowman. False (OutRefusedAt: where it would have gone, for the "no" puff; OutWhy for the
	 *  log): its builder in the air, no tile there (a gap, a drop of more than SnowmanMaxDropBlocks), no room, the next tile taken
	 *  too, or past the build limit. */
	static bool FindSpot(const APenguinCharacter* Builder, float Side, FVector& OutFeet, FVector* OutRefusedAt = nullptr, FString* OutWhy = nullptr);
	/** The snowman standing on Tile of Island, if any. */
	static APinguSnowman* FindOnTile(const UWorld* World, const ABlockIsland* Island, const FIntPoint& Tile);
	/** Server: Shot (thrown by a snowman) hit a penguin: that snowman's next throw comes sooner (SnowmanHitReload). */
	static void NoteShotHit(const UWorld* World, const APenguinProjectile* Shot);
	static void GetAll(const UWorld* World, TArray<APinguSnowman*>& Out);
	/** Builder's standing snowman, if any. */
	static APinguSnowman* FindOf(const UWorld* World, const APenguinCharacter* Builder);
	/** Every one goes at once (a new round). */
	static void DestroyAll(UWorld* World);
	/** Server: every snowman whose body is within Radius of Center (and, with CosHalfAngle > -1, in the cone along ConeDirection)
	 *  breaks - By's doing (What for the log). Returns how many. The slap, the noot, frost, a stomp's landing, a bomb's blast. */
	static int32 BreakInArea(UWorld* World, const FVector& Center, float Radius, APenguinCharacter* By, const TCHAR* What,
		const FVector& ConeDirection = FVector::ZeroVector, float CosHalfAngle = -2.f);

	/** Server: it crumbles (By: whose doing, may be null). */
	void Break(APenguinCharacter* By, const TCHAR* Why);
	/** Server: its size x Factor (the rays); past SnowmanMinSize it melts. */
	void Resize(float Factor, APenguinCharacter* By);

	APenguinCharacter* GetBuilder() const { return Builder.Get(); }
	float GetSize() const { return SizeScale; }
	/** Built (it throws from then on). */
	bool IsStanding() const;
	/** Its feet (the actor's place) and the point it throws from (its arm, at its head's height). */
	FVector GetFeet() const { return GetActorLocation(); }
	FVector GetThrowPoint() const;
	/** Seconds since it was cast (server clock, every machine). */
	float GetAge() const;
	/** Server, tests: snowballs thrown, the penguin it threw the last one at. */
	int32 GetThrows() const { return Throws; }
	APenguinCharacter* GetLastTarget() const { return LastTarget.Get(); }
	/** Server, tests: its snowballs that hit a penguin, and the throws that came sooner for it. */
	int32 GetHits() const { return Hits; }
	int32 GetQuickThrows() const { return QuickThrows; }
	/** Server: the tile it stands on (island cell). */
	FIntPoint GetTile() const { return Tile; }
	/** The last one that went and why (tests read it after the actor is gone). */
	static FString LastEnd;
	/** Tests: its parts drawn (the balls, the face, the buttons, the arms, the hat, the scarf) on this machine, the arm's swing now
	 *  (0 .. 1). */
	int32 GetShownParts() const;
	float GetArmSwing() const { return ArmSwing; }
	/** Tests: the triangles its look draws on this machine now. */
	int32 GetLookTriangles() const;

private:
	void TickServer(float Dt);
	void TickFall(float Dt, const ABlockIsland* Island);
	void TouchShots();
	void TouchThings();
	void TryThrow();
	APenguinCharacter* FindTarget() const;
	/** Distance on the play plane from Point to its body (a line from its middle ball to its head), minus its radius (< 0: inside). */
	float DistanceToBody(const FVector& Point) const;
	void End(const TCHAR* Why);

	// ---- every machine (the look: 2026-11-04 card snowman-look; meshes made here, MakeMeshes, no assets)
	void MakeLook();
	/** (Re)builds every part's mesh for the effects setting's detail (0 Low, 1 normal, 2 Ultra). */
	void MakeMeshes(int32 Detail);
	/** Its beanie and scarf in its builder's beanie colours (they follow a change). */
	void UpdateColors();
	void TickLook(float Dt);

	UPROPERTY(VisibleAnywhere, Category = "Snowman")
	TObjectPtr<USceneComponent> SceneRoot;

	/** Bottom, middle, head ball (lumpy packed snow, each sunk into the one below); the head's middle (the face, the hat and the
	 *  scarf on it); the face (coal eyes and smile, a carrot; it turns), the coal buttons (on the middle ball), the stick arms, the
	 *  beanie (felt, a ribbed wool cuff, a pompom), the scarf (wool, a striped tail). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> Balls;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Head;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Face;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Buttons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> Arms;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Hat;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Scarf;

	/** Snow (the island's M_Block_Snow), coal and sticks (the penguin's clay), the carrot (the beak's clay), the beanie's felt, the
	 *  cuff's and the scarf's wool, the scarf's white stripes, the pompom. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> Materials;

	// ---- replicated
	UPROPERTY(Replicated)
	TObjectPtr<APenguinCharacter> Builder;

	UPROPERTY(Replicated)
	float SizeScale = 1.f;

	UPROPERTY(Replicated)
	double BuildStart = 0.0;

	/** Bumped by every throw; the side it threw to (+1 right, -1 left). */
	UPROPERTY(Replicated)
	uint8 ThrowSerial = 0;

	UPROPERTY(Replicated)
	float ThrowSide = 1.f;

	/** The side it faces (+1 right, -1 left: its builder's aim when it was built). */
	UPROPERTY(Replicated)
	float Facing = 1.f;

	/** Its lumps (every machine makes the same snowman from it). */
	UPROPERTY(Replicated)
	int32 LookSeed = 1;

	// ---- server
	float ThrowTimer = 0.f;
	float FallSpeed = 0.f;
	int32 Throws = 0;
	int32 Hits = 0;
	int32 QuickThrows = 0;
	/** World time of its last throw; its next throw comes sooner (a hit). */
	double LastThrowTime = -1000.0;
	bool bQuickNext = false;
	FIntPoint Tile = FIntPoint(0, 0);
	bool bHasTile = false;
	TWeakObjectPtr<APenguinCharacter> LastTarget;
	/** Its own snowballs (they never break it). */
	TArray<TWeakObjectPtr<APenguinProjectile>> OwnThrows;
	bool bEnded = false;

	// ---- this machine
	bool bLookMade = false;
	uint8 SeenThrow = 0;
	float SwingTime = 10.f;
	float ArmSwing = 0.f;
	float ShownSize = 1.f;
	float HeadTurn = 0.f;
	int32 MeshDetail = -1;
	int32 MeshSeed = 0;
	FLinearColor ShownFelt = FLinearColor::Transparent;
	FLinearColor ShownCuff = FLinearColor::Transparent;
};
