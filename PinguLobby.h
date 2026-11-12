// Pingu - the lobby island (2026-10-23): everybody's penguin and the bots stand around on a small real island; bots and
// people puff in and out with magic dust, a click on a penguin changes its team (the beanie shows it), name tags over the
// heads, a difficulty tag over each bot (a click changes it), a little "x" to send a bot away, a "+" on the step to add one.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PinguMenu3D.h"
#include "PinguLobby.generated.h"

class APenguinCharacter;
class APenguinHUD;
class APlayerController;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UText3DComponent;

/** One penguin on the lobby island, as every machine sees it (replicated by APinguLobby). */
USTRUCT()
struct FPinguLobbyPenguin
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<APenguinCharacter> Penguin = nullptr;

	/** Goes up with every poke (its team changed: a click on it): every machine plays the hop-spin. */
	UPROPERTY()
	uint8 PokeSerial = 0;

	/** The poke's way round (+1: the next team, -1: the one before). */
	UPROPERTY()
	int8 PokeDir = 1;

	/** Flow time (APenguinGameState::GetFlowTime) it puffed in (it pops up out of the dust); very old: it was there already. */
	UPROPERTY()
	float PuffInAt = -1000.f;

	/** Flow time it started puffing away (it shrinks into the dust and goes); negative: it stays. */
	UPROPERTY()
	float PuffOutAt = -1.f;

	/** How it waits on its spot (PinguLobby::EPose: 0 stand, 1 sit, 2 hang from an edge) and the side it faces / hangs out to (+1: +X). */
	UPROPERTY()
	uint8 Pose = 0;
	UPROPERTY()
	int8 Side = 1;

	/** The waiting gag it plays (PinguLobby::EGag; 0 none), since this flow time, and at whom (a noot at a neighbour; null: the camera). */
	UPROPERTY()
	uint8 Gag = 0;
	UPROPERTY()
	float GagAt = -1000.f;
	UPROPERTY()
	TObjectPtr<APenguinCharacter> GagTarget = nullptr;

	/** The fish made it fat: it swells from FatAt, and is back to normal at FatUntil (flow times). */
	UPROPERTY()
	float FatAt = -1000.f;
	UPROPERTY()
	float FatUntil = -1000.f;
};

namespace PinguLobby
{
	/** How a penguin waits on its lobby spot. */
	enum class EPose : uint8 { Stand, Sit, Hang };
	/** The waiting gags (2026-10-27, Linus: "ocasionaly eat a fish and get fat ... use their trumped at an other player ... or at
	 *  the person playing the game ... and more such animations so it looks more funn"). */
	enum class EGag : uint8 { None, Fish, NootFriend, NootCamera, Wave, Doze, Dance, Shiver, Count };
	/** How long a gag plays (s). */
	PENGUIN_API float GetGagLength(EGag Gag);
	PENGUIN_API const TCHAR* GetGagName(EGag Gag);
	/** When the noot's two blasts go off (s into NootFriend / NootCamera); the doze's startle. */
	constexpr float NootBlastTimes[2] = { 0.5f, 1.1f };
	constexpr float DozeStartleTime = 4.2f;
	/** The fish (2026-10-27, second pass - Linus: "make the fish eatin animatin also better"): it pops into the flipper, is looked
	 *  at, tossed up at FishTossTime, caught in the beak at FishGulpTime (gulp, a swallow bump), the belly swells from FatDelay over
	 *  FatSwell with a wobble, a happy pat and a burp at FishBurpTime; back to normal over FatShrink at the fat's end. */
	constexpr float FishTossTime = 1.05f;
	constexpr float FishGulpTime = 1.8f;
	constexpr float FatDelay = 2.2f;
	constexpr float FishBurpTime = 3.f;
	constexpr float FatSwell = 0.7f;
	constexpr float FatShrink = 3.5f;

	/** What the lobby island asks of a penguin's animation (PenguinAnimInstance reads it in PreUpdate). */
	struct FAnimState
	{
		/** On the lobby island: its pose, the gags. */
		bool bLobby = false;
		/** Seconds since the last poke (a hop-spin; -1: none lately) and its way round. */
		float PokeAge = -1.f;
		float PokeDir = 1.f;
		EPose Pose = EPose::Stand;
		/** The side it hangs out to (+1: +X) or turns from. */
		float Side = 1.f;
		/** Where the body is drawn from its capsule (world cm: lower when sitting, out over the edge and down when hanging). */
		FVector Offset = FVector::ZeroVector;
		/** The gag, how far into it (s), and its aim: +1 / -1 a neighbour on that side (X), 0 the camera. */
		EGag Gag = EGag::None;
		float GagAge = -1.f;
		float GagDir = 0.f;
		/** A neighbour's noot hit it this long ago (s; -1: not lately): it flinches (never pushed), away from DirX. */
		float FlinchAge = -1.f;
		float FlinchDir = 1.f;
		/** How fat (0 normal, 1 fully swollen). */
		float Fat = 0.f;
	};
	PENGUIN_API FAnimState GetAnimState(const APenguinCharacter* Penguin);
	/** Tests (clicks_Menu3D): nothing moves on its own (no gags start) while on. */
	PENGUIN_API void SetBotsFrozen(bool bFrozen);
	/** Tests: no new gag starts by itself while on (StartGag still works). */
	PENGUIN_API void SetGagsPaused(bool bPaused);

	/** The magic-dust puff a penguin appears in (bIn) or vanishes in, at its feet: a sparkly poof, a ring, a sound. Server
	 *  gameplay calls it inside its own FPinguFXBroadcastScope (every machine sees it). */
	PENGUIN_API void MagicPuff(const UObject* WorldContext, const FVector& Feet, const FLinearColor& Color, bool bIn);
}

/**
 * The lobby island's actor: one per world, spawned by the game mode (server) when the lobby island opens, replicated.
 *
 * Server: notices penguins arriving (a bot added, a LAN player joining: a puff and a pop-in) and leaving (a bot sent away:
 * PuffAway shrinks it into the dust; a player who left: a puff where they stood), pokes a penguin whose team changed, and
 * puts everybody on a fixed spot of its own (sitting, or hanging from an edge: nobody walks), and starts the waiting gags.
 * Every machine: the beanies in the picked teams' colours, the pop in / out, the 3D name and difficulty tags, the "+", and
 * the local player's clicks (mouse: left click forward, right click back; keys: Tab picks a penguin, Enter / Backspace
 * change its team, Enter on the "+" adds a bot, Delete sends a picked bot away; a gamepad's d-pad and Y / X do the same).
 * Clicks go through the HUD's lobby actions (APenguinHUD::Action_*), as the old cards did: a LAN client can only change
 * its own penguin's team (the server RPC), the host / solo player everything.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguLobby : public AActor
{
	GENERATED_BODY()

public:
	APinguLobby();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** The world's lobby actor (null until the server spawned it / it replicated). */
	static APinguLobby* Get(const UWorld* World);
	/** Server: the lobby actor, spawned if there is none. Elsewhere the same as Get. */
	static APinguLobby* FindOrSpawn(UWorld* World);

	// ------------------------------------------------------------------ server
	/** The lobby island just opened: whoever is on it now was there already (no puff); everybody goes to a spot. */
	void OnLobbyStarted();
	/** Starts a gag on a penguin now (tests; the gag director uses it too). Target: a NootFriend's neighbour (null: the nearest).
	 *  False when it can't (no entry, leaving). */
	bool StartGag(APenguinCharacter* Penguin, PinguLobby::EGag Gag, APenguinCharacter* Target = nullptr);
	/** Tests: makes a penguin fat at once, back to normal in Seconds (the shrink included). */
	void SetFatFor(APenguinCharacter* Penguin, float Seconds);
	/** A bot leaves the lobby (its X): it shrinks into a puff of dust and is gone a moment later. The game mode already took it
	 *  out of its penguins and its brain away. */
	void PuffAway(APenguinCharacter* Penguin);

	// ------------------------------------------------------------------ every machine
	/** The lobby island shows here now (the lobby phase on the lobby island). */
	bool IsShowing() const;
	/** A penguin's lobby team as every machine knows it: a person's pick (its player state), a bot's setup's (the match
	 *  settings); -1: no team. */
	static int32 GetLobbyTeam(const APenguinCharacter* Penguin);
	/** Where the "+" is (world) and whether it shows (the host / solo player with room for one more). */
	bool GetPlusSpot(FVector& OutCentre) const;
	bool IsPlusShown() const { return bPlusShown; }
	/** A penguin's tags: the name tag's middle, and a bot's difficulty tag / x (false: none shown). */
	bool GetNameTagSpot(const APenguinCharacter* Penguin, FVector& OutCentre) const;
	bool GetSkillTagSpot(const APenguinCharacter* Penguin, FVector& OutCentre) const;
	bool GetRemoveSpot(const APenguinCharacter* Penguin, FVector& OutCentre) const;
	/** What the name tag / difficulty tag of a penguin says now (empty: no tag). */
	FString GetNameTagText(const APenguinCharacter* Penguin) const;
	FString GetSkillTagText(const APenguinCharacter* Penguin) const;
	/** Seconds since the penguin puffed in (flow time; very large: it was there when the lobby opened, -1: not in the lobby). */
	float GetPuffInAge(const APenguinCharacter* Penguin) const;
	/** The pop-in / out scale this machine gives a penguin now (1: full size). */
	float GetPopScale(const APenguinCharacter* Penguin) const;
	/** The last poke's age on this machine (seconds) and its way round; false when there was none in the last second. */
	bool GetPoke(const APenguinCharacter* Penguin, float& OutAge, float& OutDir) const;
	/** Puffs this machine saw start (the magic dust in / out, counted where the entries change: tests). */
	int32 GetPuffInsSeen() const { return PuffInsSeen; }
	int32 GetPuffOutsSeen() const { return PuffOutsSeen; }
	/** Pokes this machine played (hop-spins; tests). */
	int32 GetPokesSeen() const { return PokesSeen; }

	/** The pose, gag, flinch and fat part of PinguLobby::GetAnimState. */
	void FillAnimState(const APenguinCharacter* Penguin, PinguLobby::FAnimState& State) const;
	/** Where the penguin's body is drawn (world: its capsule plus the pose's offset, the middle of the body) - click it there. */
	FVector GetBodyCentre(const APenguinCharacter* Penguin) const;
	/** The pose's offset of the drawn body from the capsule (world cm; zero: none). */
	FVector GetVisualOffset(const APenguinCharacter* Penguin) const;
	/** Its pose and gag as this machine knows them (from the replicated entry), how far into the gag (s), how fat (0..1). */
	PinguLobby::EPose GetPose(const APenguinCharacter* Penguin) const;
	PinguLobby::EGag GetGag(const APenguinCharacter* Penguin, float& OutAge) const;
	float GetFat(const APenguinCharacter* Penguin) const;
	/** Gags this machine saw start (entries' gags changing; tests) and the most playing at once. */
	int32 GetGagsSeen() const { return GagsSeen; }
	int32 GetMostGagsAtOnce() const { return MostGagsAtOnce; }
	/** How far its tag was pushed aside by the overlap solver (cm; the spots are meant to keep this 0) and its row (0 / 1). */
	float GetTagShift(const APenguinCharacter* Penguin) const;
	int32 GetTagRow(const APenguinCharacter* Penguin) const;
	/** The lobby's props shown here now (the fish, the Zzz letters; tests: none outside the lobby). */
	int32 CountShownProps() const;
	/** Server: the spots (capsule places, in the order newcomers get them) and the spot a penguin has (-1: none). */
	struct FSpot
	{
		FVector Location = FVector::ZeroVector;
		PinguLobby::EPose Pose = PinguLobby::EPose::Sit;
		int8 Side = 1;
	};
	const TArray<FSpot>& GetSpots() const { return Spots; }
	int32 GetSpotOf(const APenguinCharacter* Penguin) const;

	/** A click of this machine's player at a point of the play plane's picture (world: the cursor's ray already meets the
	 *  tags' plane - ClickAtScreen does that). Right: the right mouse button (back). True when it hit something of the lobby.
	 *  Tests use it to click like a player. */
	bool ClickAt(APlayerController* Player, const FVector& WorldPoint, bool bRight);
	/** The cursor's ray through ScreenPosition (viewport pixels) to the lobby's things, then ClickAt. */
	bool ClickAtScreen(APlayerController* Player, const FVector2D& ScreenPosition, bool bRight);
	/** The same with the cursor's ray met twice: TagPoint on the tags' plane, BodyPoint on the play plane (the penguins). */
	bool ClickAtPoints(APlayerController* Player, const FVector& TagPoint, const FVector& BodyPoint, bool bRight);
	/** The keyboard / gamepad pick (Tab): what it is on (a penguin, or null on the "+" - see IsPlusFocused). */
	const APenguinCharacter* GetFocused() const { return Focused.Get(); }
	bool IsPlusFocused() const { return bPlusFocused; }

	/** What a click on the island can hit (2026-10-24: generous shapes, hover feedback, a probe for the tests). */
	enum class EHit : uint8 { None, Remove, Skill, Body, Plus, Name };
	struct FHit
	{
		EHit Kind = EHit::None;
		TWeakObjectPtr<const APenguinCharacter> Penguin;
	};
	/** What a click at this viewport pixel would hit (nothing happens). */
	FHit ProbeAtScreen(APlayerController* Player, const FVector2D& ScreenPosition) const;
	/** The cursor is over this viewport pixel: the thing under it lights up (the x, a difficulty tag, the "+", a penguin's name). */
	void HoverAtScreen(const FVector2D& ScreenPosition);
	/** Tests: while on, clicks are resolved (GetLastProbe) but do nothing. */
	void SetClickProbe(bool bOn) { bClickProbe = bOn; }
	void ClearProbe() { LastProbe = FHit(); }
	const FHit& GetLastProbe() const { return LastProbe; }
	/** Tests: everything a click can hit right now, with its middle (world) and half size (cm) in its plane (Y = Centre.Y). */
	struct FTarget
	{
		EHit Kind = EHit::None;
		TWeakObjectPtr<const APenguinCharacter> Penguin;
		FVector Centre = FVector::ZeroVector;
		FVector2D Half = FVector2D::ZeroVector;
	};
	TArray<FTarget> GetClickTargets() const;
	static const TCHAR* HitName(EHit Kind);

private:
	/** Server: puffs and pokes from what changed since the last frame; everybody on its spot; the gags. */
	void TickServer(float Dt);
	/** Server: the island's spots (BuildSpots), a newcomer's spot (AssignSpot) and putting it there (PlaceOnSpot). */
	void BuildSpots();
	int32 AssignSpot(const APenguinCharacter* Penguin) const;
	void PlaceOnSpot(APenguinCharacter* Penguin, int32 Spot);
	void HoldSpots(float Dt);
	/** Server: starts gags now and then (1-2 at once), fires their sounds and effects at their moments. */
	void TickGags(float Dt);
	void FireGagEvents(APenguinCharacter* Penguin, const FPinguLobbyPenguin& Entry, float From, float To);
	APenguinCharacter* FindNeighbour(const APenguinCharacter* Penguin, float MaxDistance) const;
	/** Every machine: the fish and the Zzz of the gags playing. */
	void UpdateProps();
	void HideProps();
	/** Every machine: hats, pop scales, pokes, tags, the "+", the local player's input. */
	void TickLooks(float Dt);
	void UpdateTags();
	void UpdatePlus(float Dt);
	void HandleLocalInput();
	/** Puts every penguin's hat and size back (the lobby island closed). */
	void ResetLooks();
	const FPinguLobbyPenguin* FindEntry(const APenguinCharacter* Penguin) const;
	APenguinHUD* GetLocalHUD(APlayerController*& OutPlayer) const;
	/** The lobby's penguins in order (left to right on the island): the focus cycle's order. */
	TArray<APenguinCharacter*> GetPickable() const;
	/** A tag's parts: a slab (rounded, bevelled: PinguMenu3D::FSlab) with a 3D text on it, under Parent. */
	struct FTagPiece
	{
		PinguMenu3D::FSlab Slab;
		UText3DComponent* Text = nullptr;
		USceneComponent* Root = nullptr;
		FString Shown;
		float Width = 0.f;
	};
	/** bPlate false (the "+" and the bots' x, 2026-11-07): no disc behind it, only its word, in the Face colour. */
	void BuildPiece(FTagPiece& Piece, USceneComponent* Parent, float TextSize, const FLinearColor& Face, const FLinearColor& Lip, bool bPlate = true);
	/** Sets its text (rebuilt only when it changed) and lays the slab round it; returns the slab's width. bRound: a disc as wide
	 *  as it is high (the x), whatever the text's width. */
	float LayPiece(FTagPiece& Piece, const FString& Text, float TextSize, float Height, float MinWidth, bool bRound = false);
	USceneComponent* MakeRoot();
	/** What a click at these points (the tags' plane, the play plane) hits, with the bot's index in the settings for a bot. */
	FHit ResolveAt(APlayerController* Player, const FVector& TagPoint, const FVector& BodyPoint, bool bRight, int32& OutBotIndex) const;
	/** The cursor's ray met on the tags' plane and the play plane (false: no ray). */
	bool ScreenToPlanes(APlayerController* Player, const FVector2D& ScreenPosition, FVector& OutTagPoint, FVector& OutBodyPoint) const;
	/** The piece lights up (glows, grows a little) while hovered. */
	void SetPieceHover(FTagPiece& Piece, bool bHover, const FLinearColor& Face, const FLinearColor& Lip, float BaseGlow = 0.f);

	UPROPERTY(Replicated)
	TArray<FPinguLobbyPenguin> Entries;

	// ---- server
	struct FServerSeen
	{
		TWeakObjectPtr<APenguinCharacter> Penguin;
		FVector Feet = FVector::ZeroVector;
		FLinearColor Color = FLinearColor::White;
		int32 Team = -1;
		/** Its spot (index into Spots), how long it has been off it (s), its last gag's kind and end (flow time). */
		int32 Spot = INDEX_NONE;
		float OffSpotTime = 0.f;
		uint8 LastGag = 0;
		float GagFreeAt = 0.f;
		/** The gag time its events were fired up to (FireGagEvents). */
		float EventsDoneTo = 0.f;
		float FatShrinkFired = -1000.f;
	};
	TArray<FServerSeen> ServerSeen;
	bool bServerStarted = false;
	TArray<FSpot> Spots;
	FRandomStream WalkRand;
	float NextGagAt = 0.f;

	// ---- every machine
	struct FTagView
	{
		TWeakObjectPtr<APenguinCharacter> Penguin;
		/** The whole tag (turned to face the camera: its +Y reads to the right, -X toward the camera). */
		USceneComponent* Root = nullptr;
		FTagPiece Name;
		FTagPiece Skill;
		FTagPiece Remove;
		FVector NameCentre = FVector::ZeroVector;
		FVector SkillCentre = FVector::ZeroVector;
		FVector RemoveCentre = FVector::ZeroVector;
		FVector2D NameHalf = FVector2D::ZeroVector;
		FVector2D SkillHalf = FVector2D::ZeroVector;
		bool bSkillShown = false;
		bool bRemoveShown = false;
		FString NameText;
		FString SkillText;
		int32 SkillShown = -1;
		bool bFocusShown = false;
		/** Pushed aside this far (cm) so it does not overlap a neighbour's tag. */
		float Shift = 0.f;
		/** 1: lifted a row up (a close neighbour's tag in the low row); the tags' lane (their penguins' head height, cm). */
		int32 Row = 0;
		float LaneZ = 0.f;
	};
	TArray<FTagView> TagViews;

	/** A penguin's gag props on this machine (made the first time it needs them). */
	struct FPropView
	{
		TWeakObjectPtr<const APenguinCharacter> Penguin;
		UStaticMeshComponent* Fish = nullptr;
		USceneComponent* ZRoot = nullptr;
		USceneComponent* Z[3] = {};
		float LastGagAt = -1000.f;
	};
	TArray<FPropView> PropViews;
	int32 GagsSeen = 0;
	int32 MostGagsAtOnce = 0;
	/** Keeps the tags' and the "+"'s components alive. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> Parts;

	struct FPokeSeen
	{
		TWeakObjectPtr<const APenguinCharacter> Penguin;
		uint8 Serial = 0;
		double StartedAt = -100.0;
		float Dir = 1.f;
	};
	TArray<FPokeSeen> PokeSeen;
	/** Entries seen last frame (this machine), for the puff counters. */
	TArray<TWeakObjectPtr<APenguinCharacter>> EntriesSeen;
	TArray<TWeakObjectPtr<APenguinCharacter>> PuffingOutSeen;
	int32 PuffInsSeen = 0;
	int32 PuffOutsSeen = 0;
	int32 PokesSeen = 0;
	bool bLooksApplied = false;

	// the "+"
	USceneComponent* PlusRoot = nullptr;
	FTagPiece Plus;
	FVector PlusCentre = FVector::ZeroVector;
	bool bPlusShown = false;
	float PlusTime = 0.f;
	float PlusPress = 0.f;

	// the keyboard / gamepad pick
	TWeakObjectPtr<const APenguinCharacter> Focused;
	bool bPlusFocused = false;

	// the mouse: what it is over (HoverAtScreen), the tests' probe
	FHit Hovered;
	bool bClickProbe = false;
	FHit LastProbe;
};
