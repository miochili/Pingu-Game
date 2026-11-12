// Pingu - the stage camera: the island first (arena rounds, the lobby, the menu demo), Smash-style on the practice course.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PenguinCamera.generated.h"

class APenguinCharacter;
class UCameraComponent;

/**
 * Looks at the 2D play plane from the side (from +Y toward -Y, so +X is screen-right).
 * On an arena island the whole island is the picture: it stays framed whatever happens, penguins close above or beside
 * it widen the frame a little, and penguins flung further away are off screen (the HUD marks them at the screen's edge).
 * A small island (few penguins) is framed closer. The camera never goes further out than MaxFrameDistance: an island too
 * big for that (many penguins) is not framed whole - the camera stays that far out and follows this machine's penguin
 * with a little lag (never too far behind, the penguin never at the screen's edge), or with none (knocked out, the menu's
 * demo, the bot matches of the tests) the middle of the penguins still in; it shows no more sea past the island's ends
 * than the island's frame would.
 * On the practice course it pans to the penguins and zooms out to fit them and most of the island (Smash-style).
 * One camera is shared by every local player (no split screen). Also does the screen shake
 * and the dramatic zoom on the last knock-out of a round.
 *
 * LAN: the server's camera replicates (only so view-target changes the server sends resolve on the clients); every
 * machine moves it by itself from the penguins it has and the replicated APenguinGameState.
 */
UCLASS()
class PENGUIN_API APenguinCamera : public AActor
{
	GENERATED_BODY()

public:
	APenguinCamera();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** The camera in the level, or a newly spawned one (a LAN client only finds the server's, null until it arrived). */
	static APenguinCamera* FindOrSpawn(UWorld* World);

	/** Screen shake: 0..1, stacks and decays. */
	void AddTrauma(float Amount);

	/** Swoop toward a point for a moment (knock-out zoom). */
	void FocusOn(const FVector& Location, float Duration, float ZoomScale = 0.7f);

	/** The kill cam (APinguKillCam, this machine): frame these points (the victim, the hitter) this much (0 .. 1, eased here);
	 *  it holds until set again. Blend 0: the normal framing. bSnap: jump there now (a little wider, then it zooms in gently). */
	void SetKillCamView(const FBox& Frame, float Blend, bool bSnap = false);
	float GetKillCamBlend() const { return KillCamBlend; }

	/** Jump straight to the framing (new round), and keep jumping for HoldSeconds (a client, while the penguins' new
	 *  places come in). */
	void SnapToTargets(float HoldSeconds = 0.f);

	/** How far out the camera is now (cm, along its view). */
	float GetFrameDistance() const { return Distance; }
	/** The player's camera distance setting (UPinguSettings::GetCameraDistance) in a match; 1 on the title and in the lobby. */
	float GetUserDistanceScale() const;
	/** The point on the play plane the camera looks at now (X, Z; without the shake and the title menu's shift). */
	FVector2D GetFocusPoint() const { return FVector2D(FocusX + FocusShift, FocusZ); }
	/** The island is too big for the furthest view and the camera follows (at least half-way blended in). */
	bool IsFollowing() const { return FollowBlend >= 0.5f; }
	/** The penguin the camera follows when the island is too big: this machine's own (null: the middle of the penguins). */
	const APenguinCharacter* GetFollowPenguin() const { return FollowPenguin.Get(); }
	/** The point it follows (X, Z), kept inside the island's frame. */
	FVector2D GetFollowPoint() const { return FollowPoint; }
	/** What this machine's player sees of the play plane (Y = 0), world X (Min/Max.X) and Z (Min/Max.Y): the screen's
	 *  corners projected onto it, the biggest rectangle inside. False without a player or a viewport. */
	bool GetViewRect(FBox2D& OutRect) const;
	/** Tests: GetViewRect gives this instead (an invalid box: the real view again). */
	void SetViewRectOverride(const FBox2D& Rect) { ViewRectOverride = Rect; }
	/** The island's frame with its margins as of the last frame (X and Z, as GetViewRect): what the camera shows of an arena
	 *  that fits. False before the first arena frame. */
	bool GetSharedFrameRect(FBox2D& OutRect) const;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> Camera;

	/** Horizontal field of view. Low values flatten the view (more 2D, like Smash). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float FieldOfView = 35.f;

	/** Negative = looking slightly down onto the stage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float Pitch = -8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ForceUnits = "cm"))
	float MinDistance = 3000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ForceUnits = "cm"))
	float MaxDistance = 6800.f;

	/** Space kept around the penguins (X = sideways, Y = up/down). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FVector2D FramePadding = FVector2D(620.f, 380.f);

	/** How much of the island is always kept in view (0 = only the penguins, 1 = the whole island). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0", ClampMax = "1"))
	float IslandFraming = 0.65f;

	/** Arena island: room kept around it - sideways (X), above its highest block (Y) - and below its lowest ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Island")
	FVector2D IslandMargin = FVector2D(250.f, 450.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Island", meta = (ForceUnits = "cm"))
	float IslandMarginBelow = 650.f;

	/** The lobby island (EIslandLayout::Lobby): room kept around it - sideways (X), above its highest block (Y, the name
	 *  tags) - and below its lowest ground (the lobby's buttons at the bottom of the screen). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Island")
	FVector2D LobbyMargin = FVector2D(180.f, 380.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Island", meta = (ForceUnits = "cm"))
	float LobbyMarginBelow = 420.f;

	/** Arena island: penguins this far outside the island's frame (sideways X, above Y) still widen it - further away they
	 *  go off screen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Island")
	FVector2D FollowMargin = FVector2D(450.f, 650.f);

	/** A small arena (fewer than four penguins): the room around the island (IslandMargin, IslandMarginBelow) shrinks with
	 *  the island's size (ABlockIsland::GetArenaScale), down to this share - the camera comes in close. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Island", meta = (ClampMin = "0.2", ClampMax = "1"))
	float MinIslandMarginScale = 0.6f;

	/** Arena: the camera never goes further out than this (today's view of a four-penguin island; a giant does not widen it
	 *  either). An island that needs more is followed (see the Camera|Follow numbers). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Island", meta = (ForceUnits = "cm"))
	float MaxFrameDistance = 5500.f;

	/** Following (an island too big for MaxFrameDistance): how quickly the view glides after the penguin (a critically damped
	 *  spring's smooth time). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Follow", meta = (ForceUnits = "s"))
	float FollowSmoothTime = 0.2f;

	/** How long the camera takes to change between framing the whole island and following. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Follow", meta = (ForceUnits = "s"))
	float FollowBlendTime = 0.2f;

	/** The most the view's middle trails the followed point, as a share of the view's half-width (X) and half-height (Y).
	 *  (The point is the penguin, kept inside the island's frame: at an island's end the view stops at the frame.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Follow")
	FVector2D FollowMaxLag = FVector2D(0.25f, 0.5f);

	/** The followed penguin (all of it) always stays this share of the half-width (X) and half-height (Y) inside the
	 *  screen's edges: the camera is dragged along at once, past the island's frame if need be. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Follow")
	FVector2D FollowEdgeKeep = FVector2D(0.25f, 0.2f);

	/** The followed point jumped further than this in one frame (a warp, a respawn, another penguin to follow): the view glides
	 *  there in GlideTime instead of trailing it (the edge keep still holds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Follow", meta = (ForceUnits = "cm"))
	float GlideJumpDistance = 300.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Follow", meta = (ForceUnits = "s"))
	float GlideTime = 0.15f;

	/** Arena island: a penguin in the air flying sideways faster than this (or stunned) was knocked flying - it does not
	 *  widen the frame until it lands again (a dash is 1900). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Island", meta = (ForceUnits = "cm/s"))
	float FlungSpeed = 2000.f;

	/** Aim a bit above the penguins' centre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ForceUnits = "cm"))
	float FocusHeightOffset = 120.f;

	/** Look ahead in the running direction (seconds of velocity). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float LookAheadTime = 0.18f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ForceUnits = "cm"))
	float MaxLookAhead = 240.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ForceUnits = "s"))
	float HorizontalSmoothTime = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ForceUnits = "s"))
	float VerticalSmoothTime = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ForceUnits = "s"))
	float ZoomSmoothTime = 0.5f;

	/** Penguins below this height (falling off the island) are no longer followed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ForceUnits = "cm"))
	float IgnoreBelowZ = -900.f;

	/** The camera focus never goes below this height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ForceUnits = "cm"))
	float MinFocusZ = -150.f;

	/** Biggest shake offset in cm (at 3000 cm distance) and roll in degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Shake")
	float ShakeOffset = 40.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Shake")
	float ShakeRoll = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Shake")
	float TraumaDecay = 1.8f;

private:
	/** A LAN client: this machine's player looks through this camera (in case nothing else set it up). */
	void ClaimLocalView();
	/** Arena island: the box to frame (play plane, Y = 0): the island with its margins, plus the penguins near it (a grown
	 *  one with its whole body, head to feet). False
	 *  when the island is not an arena (the practice course frames the penguins). */
	bool GetIslandFrame(FBox& OutFrame);
	/** The penguin was knocked flying and has not landed since (see FlungSpeed). Remembers it per penguin. */
	bool IsFlung(const APenguinCharacter* Penguin);
	/** This machine's player's penguin, to follow: alive, shown, not falling into the sea (IgnoreBelowZ). None in the title
	 *  menu's demo and the bot matches of the tests (-PinguMatchTest): the middle of the penguins is followed there. */
	APenguinCharacter* FindLocalPenguin() const;
	/** What to follow: this machine's penguin (OutBody: all of it, head to feet; bOutFlung: knocked flying) or the middle of
	 *  the live penguins not knocked flying (a point). False when there is nobody. */
	bool GetFollowTarget(FBox& OutBody, bool& bOutFlung);

	/** Penguins knocked flying, until they land (IsFlung). */
	TMap<TWeakObjectPtr<const APenguinCharacter>, bool> Flung;

	/** Following: 0 = the whole island framed, 1 = following (blends over FollowBlendTime). */
	float FollowBlend = 0.f;
	/** The followed point inside the island's frame (held while the followed penguin is knocked flying; a jump of it starts
	 *  a glide), and whom it follows. */
	FVector2D FollowPoint = FVector2D::ZeroVector;
	bool bFollowPointValid = false;
	TWeakObjectPtr<const APenguinCharacter> FollowPenguin;
	/** A glide to a point that jumped (real seconds left; from where). */
	float GlideLeft = 0.f;
	FVector2D GlideFrom = FVector2D::ZeroVector;
	/** The island's frame as of the last frame (GetSharedFrameRect). */
	FBox LastIslandFrame = FBox(ForceInit);
	/** Tests: GetViewRect's answer instead of the real view. */
	FBox2D ViewRectOverride = FBox2D(ForceInit);

	float FocusX = 0.f;
	float FocusZ = 0.f;
	float FocusVelocityX = 0.f;
	float FocusVelocityZ = 0.f;
	float Distance = 3000.f;
	float DistanceVelocity = 0.f;
	FBox LastTargets = FBox(ForceInit);
	bool bInitialized = false;
	/** Real seconds the camera keeps snapping (SnapToTargets with a hold). */
	float SnapHoldLeft = 0.f;
	/** The island's ground, lowest and highest standing place (cached per island generation). */
	int32 GroundGeneration = -1;
	float GroundMinZ = 0.f;
	float GroundMaxZ = 0.f;

	float Trauma = 0.f;
	float ShakeTime = 0.f;

	/** Slides the picture right while the title menu covers the left side; FocusShift: by how much (cm) this frame. */
	float MenuShift = 0.f;
	float FocusShift = 0.f;

	FVector FocusOverride = FVector::ZeroVector;
	float FocusOverrideLeft = 0.f;
	float FocusOverrideDuration = 0.f;
	float FocusOverrideZoom = 1.f;

	/** The kill cam's frame and how far in (SetKillCamView). */
	FBox KillCamFrame = FBox(ForceInit);
	float KillCamBlend = 0.f;
	bool bKillCamSnap = false;
};
