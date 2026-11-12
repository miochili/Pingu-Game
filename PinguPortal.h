// Pingu - Snow Warp's portal.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PinguPortal.generated.h"

class APenguinCharacter;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/**
 * Snow Warp still teleports at once, and leaves a portal: one end where the warp began, one where it ended (upright
 * ovals facing the camera, a little bigger than a penguin). Whatever goes into one end comes out of the other with the
 * speed it had: penguins (sliding, stomping, flying or frozen, they go on doing it), shots and bombs (not the fish hook,
 * not a hooked penguin) and ability bubbles. Only what comes in from outside goes through: whatever just came out of an
 * end, stood in one when the portal opened (like the warper itself) or appeared in one (a shot thrown from inside it) is
 * left alone until it has been out of both ends. A penguin has one portal: its next warp closes the old one. A new round
 * closes them all, a portal goes when its penguin leaves the game, and another penguin's gust blows it out (CloseInArea).
 *
 * LAN: the server sends things through (penguins with NetTeleport, which reaches a remote player's own machine; shots and
 * bubbles move on the server and replicate). The portal replicates its ends and its penguin; every machine draws it.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguPortal : public AActor
{
	GENERATED_BODY()

public:
	APinguPortal();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** InOwner's portal from From (where the warp began) to To (where it ended), both penguin middles; InOwner's old
	 *  portal closes. Server or solo only: null on a LAN client. */
	static APinguPortal* Open(UWorld* World, APenguinCharacter* InOwner, const FVector& From, const FVector& To);
	/** Every portal goes at once (a new round), no effects. Server or solo. */
	static void CloseAll(UWorld* World);
	static void GetAll(const UWorld* World, TArray<APinguPortal*>& Out);
	/** InOwner's portal, or null. */
	static APinguPortal* FindOwnedBy(const UWorld* World, const APenguinCharacter* InOwner);

	/** Server: closes with a puff at both ends. */
	void Close();
	/** The gust blows portals out like candles: every portal with an end in the circle (the end's oval reaching into it)
	 *  closes - not By's own (like its own shots). Server or solo (0 on a LAN client). Returns how many closed. */
	static int32 CloseInArea(UWorld* World, const APenguinCharacter* By, const FVector& Origin, float AreaRadius);

	/** End 0: where the warp began, 1: where it ended (middles, on the play plane). */
	FVector GetEnd(int32 Index) const { return Index == 0 ? EndA : EndB; }
	APenguinCharacter* GetPortalOwner() const { return PortalOwner.Get(); }
	/** Point lies inside end Index's oval (grown by Margin cm all round). */
	bool IsInEnd(int32 Index, const FVector& Point, float Margin = 0.f) const;

	// ---- Snow Warp stays on the warper's screen (so walking through the portal lands you in view too)
	/** Where Placer's warp may end (Min/Max.X: world X, Min/Max.Y: world Z): what its player's screen shows of the play plane
	 *  (a LAN client's as its machine sent it), never wider than a 16:9 screen of the same height (an ultrawide screen warps
	 *  no further than a 16:9 one; a narrower one keeps its own), WarpViewMargin inside the edges. A bot, or a player whose
	 *  screen is not known (yet): the island's frame. False: no limit (no island framed either). Server or solo. */
	static bool GetWarpView(const APenguinCharacter* Placer, FBox2D& OutView);
	/** Snow Warp's move Delta from Start (the furthest it would go), its end pulled inside GetWarpView - with room for all of
	 *  the penguin there and for the warp's lift above it - and never longer than Delta was. */
	static FVector ClampWarpMove(const APenguinCharacter* Placer, const FVector& Start, const FVector& Delta);
	/** Where lies inside GetWarpView (a shot Snow Warp swaps places with). True when there is no limit. */
	static bool IsInWarpView(const APenguinCharacter* Placer, const FVector& Where);
	/** View cut to a 16:9 rectangle of the same middle and height (where it is wider). */
	static FBox2D ToReferenceView(const FBox2D& View);

	/** The exit stays this share of the view's half-width and half-height inside its edges. */
	static constexpr float WarpViewMargin = 0.1f;
	/** The widest screen the exit is allowed on: 16:9. */
	static constexpr float WarpReferenceAspect = 16.f / 9.f;
	/** Snow Warp tries spots up to this many of the penguin's half-heights above where it aims (UPenguinAbilityComponent). */
	static constexpr float WarpLiftHalfHeights = 1.5f;
	/** How many things went through so far (server). */
	int32 GetTransitCount() const { return TransitCount; }

	/** An end's oval: half its width and half its height (cm). */
	static constexpr float EndHalfWidth = 42.f;
	static constexpr float EndHalfHeight = 60.f;

protected:
	virtual void BeginPlay() override;

private:
	UFUNCTION()
	void OnRep_Portal();

	/** Everything that can go through a portal now, with the point that has to enter an end and how far it moved
	 *  back along its path this frame (fast shots: the middle of the frame's move counts too). */
	void ForEachTraveller(float DeltaSeconds, TFunctionRef<void(AActor* Thing, const FVector& Point, const FVector& Back)> Visit) const;
	/** Server: sends whatever entered an end through to the other one. */
	void TickTransit(float DeltaSeconds);
	/** Server: Thing at Point went into end From: out of the other end. False if it could not go (no room for a
	 *  penguin there). */
	bool SendThrough(AActor* Thing, int32 From, const FVector& Point);
	/** The look's meshes and materials (every machine). */
	void SetupLook();
	/** Colours from the portal's penguin (its frame) - again when it arrives on a client. */
	void ApplyColors();
	void UpdateLook(float DeltaSeconds);

	/** End 0 (where the warp began) and end 1 (where it ended). */
	UPROPERTY(ReplicatedUsing = OnRep_Portal)
	FVector EndA = FVector::ZeroVector;

	UPROPERTY(ReplicatedUsing = OnRep_Portal)
	FVector EndB = FVector::ZeroVector;

	UPROPERTY(ReplicatedUsing = OnRep_Portal)
	TObjectPtr<APenguinCharacter> PortalOwner;

	/** Per end: frame ring, two swirling rings inside it, the glowing sheet (end A's four, then end B's). */
	UPROPERTY(VisibleAnywhere, Category = "Portal")
	TArray<TObjectPtr<UStaticMeshComponent>> Parts;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FrameMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SwirlMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SheetMaterial;

	/** Server: what has been out of both ends since it last came out of one (or since the portal opened, or since it
	 *  appeared): the things that go through when they come into an end. */
	TArray<TWeakObjectPtr<AActor>> Eligible;
	int32 TransitCount = 0;
	/** Every machine: seconds since it appeared here (it pops open), and the next sparkle. */
	float Age = 0.f;
	float SparkleTimer = 0.f;
	bool bLookReady = false;
};
