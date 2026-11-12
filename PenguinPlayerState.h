// Pingu - a human player in a match (host or LAN client). The name is the engine's player name (?Name= on join).
//
// CONTRACT (v2): used by the UI (lobby list, "YOU" tags, name tags) and the game mode.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "PenguinPlayerState.generated.h"

class APenguinCharacter;

UCLASS()
class PENGUIN_API APenguinPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	APenguinPlayerState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Colour / portrait slot 0..9 of this player's penguin, -1 until the server assigned one. */
	int32 GetPenguinSlot() const { return PenguinSlot; }
	/** The player hosting the LAN game (also true in solo games). */
	bool IsHost() const { return bIsHost; }
	/** This player's penguin (null between possessions, and for a moment on a client until it has replicated). */
	APenguinCharacter* GetPenguin() const;
	/** The team this player wants in team matches (the lobby's YOUR TEAM): 0..PinguMaxTeams-1, or -1 = whichever team
	 *  needs a player (the teams are evened out). */
	int32 GetTeamChoice() const { return TeamChoice; }
	/** Joined while a match was running: watching until the next round, which brings a penguin for them. */
	bool IsWaitingForRound() const { return bWaitingForRound; }

	// server
	void SetPenguinSlot(int32 Slot);
	void SetIsHost(bool bInIsHost);
	void SetTeamChoice(int32 InTeamChoice);
	void SetWaitingForRound(bool bInWaiting);

	// ---- abilities CHOOSE (2026-11-07, card ability-picks; PinguAbilityPicks.h)
	/** The person's picks for their three slots (EPenguinAbility values, None = random), empty = all random. Only the lobby
	 *  changes them, so a match keeps them. */
	const TArray<uint8>& GetPicks() const { return Pk; }
	/** What this machine's own lobby shows: the last picks it asked for (its own player), else the replicated ones. */
	TArray<uint8> GetShownPicks() const;
	/** The local player picks (any time in the lobby): solo / the host set them at once, a joiner asks the host (Rq28). */
	void RequestPicks(const TArray<uint8>& InPicks);
	/** A joiner: every set of picks it asked for this game (its judge holds the host's deal to one of them). */
	const TArray<TArray<uint8>>& GetSentPicks() const { return SentPicks; }
	/** Server: checked picks (PinguPicks::IsValid, the lobby only); false if refused. */
	bool SetPicks(const TArray<uint8>& InPicks);

protected:
	/** A joiner's picks to the host (the lobby only, rate limited, checked: anything else is ignored and logged). */
	UFUNCTION(Server, Reliable)
	void Rq28(const TArray<uint8>& InPicks);

	UPROPERTY(Replicated)
	int32 PenguinSlot = -1;

	UPROPERTY(Replicated)
	bool bIsHost = false;

	UPROPERTY(Replicated)
	int32 TeamChoice = -1;

	UPROPERTY(Replicated)
	bool bWaitingForRound = false;

	/** The picks (a short name: the memory trainer looked properties up by name, FEATURES 7.1). */
	UPROPERTY(Replicated)
	TArray<uint8> Pk;

	/** This machine's own asks (not replicated): the last one shows in its lobby, all of them are what its judge accepts. */
	TArray<TArray<uint8>> SentPicks;
};
