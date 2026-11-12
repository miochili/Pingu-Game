// Pingu - a human player in a match (host or LAN client).

#include "PenguinPlayerState.h"
#include "PenguinCharacter.h"
#include "Penguin.h"
#include "PenguinGameState.h"
#include "PinguAbilityPicks.h"
#include "PinguAntiCheat.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

APenguinPlayerState::APenguinPlayerState()
{
	// Player states replicate once a second by default; the lobby list should show a new slot right away (the setters
	// also push the change out at once).
	SetNetUpdateFrequency(10.f);
}

void APenguinPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APenguinPlayerState, PenguinSlot);
	DOREPLIFETIME(APenguinPlayerState, bIsHost);
	DOREPLIFETIME(APenguinPlayerState, TeamChoice);
	DOREPLIFETIME(APenguinPlayerState, bWaitingForRound);
	DOREPLIFETIME(APenguinPlayerState, Pk);
}

APenguinCharacter* APenguinPlayerState::GetPenguin() const
{
	return Cast<APenguinCharacter>(GetPawn());
}

void APenguinPlayerState::SetPenguinSlot(int32 Slot)
{
	if (PenguinSlot != Slot)
	{
		PenguinSlot = Slot;
		ForceNetUpdate();
	}
}

void APenguinPlayerState::SetIsHost(bool bInIsHost)
{
	if (bIsHost != bInIsHost)
	{
		bIsHost = bInIsHost;
		ForceNetUpdate();
	}
}

void APenguinPlayerState::SetTeamChoice(int32 InTeamChoice)
{
	const int32 Choice = InTeamChoice >= 0 && InTeamChoice < PinguMaxTeams ? InTeamChoice : -1;
	if (TeamChoice != Choice)
	{
		TeamChoice = Choice;
		ForceNetUpdate();
	}
}

void APenguinPlayerState::SetWaitingForRound(bool bInWaiting)
{
	if (bWaitingForRound != bInWaiting)
	{
		bWaitingForRound = bInWaiting;
		ForceNetUpdate();
	}
}

// ---------------------------------------------------------------------------- abilities CHOOSE (2026-11-07, PinguAbilityPicks.h)

TArray<uint8> APenguinPlayerState::GetShownPicks() const
{
	return SentPicks.Num() > 0 ? SentPicks.Last() : Pk;
}

void APenguinPlayerState::RequestPicks(const TArray<uint8>& InPicks)
{
	if (!PinguPicks::IsValid(InPicks))
	{
		return;
	}
	const APlayerController* OwnerController = Cast<APlayerController>(GetOwner());
	if (HasAuthority())
	{
		SetPicks(InPicks); // solo / the host: straight in
		SentPicks.Reset();  // (the replicated value is ours already)
		return;
	}
	if (!OwnerController || !OwnerController->IsLocalController())
	{
		return;
	}
	// A joiner remembers every set it asked for: the host may deal any of them (one may cross the match's start), nothing else.
	if (!SentPicks.ContainsByPredicate([&InPicks](const TArray<uint8>& Sent) { return Sent == InPicks; }))
	{
		SentPicks.Add(InPicks);
	}
	else
	{
		SentPicks.RemoveAll([&InPicks](const TArray<uint8>& Sent) { return Sent == InPicks; });
		SentPicks.Add(InPicks); // (the last one is what the lobby shows)
	}
	Rq28(InPicks);
}

bool APenguinPlayerState::SetPicks(const TArray<uint8>& InPicks)
{
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (!HasAuthority() || !GameState || GameState->GetPhase() != EPinguPhase::Lobby || !PinguPicks::IsValid(InPicks))
	{
		return false;
	}
	TArray<uint8> Clean = PinguPicks::HasAny(InPicks) ? InPicks : TArray<uint8>();
	if (Pk != Clean)
	{
		Pk = MoveTemp(Clean);
		ForceNetUpdate();
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s picks %s"), *GetPlayerName(), *PinguPicks::Describe(Pk));
	}
	return true;
}

void APenguinPlayerState::Rq28_Implementation(const TArray<uint8>& InPicks)
{
	APlayerController* OwnerController = Cast<APlayerController>(GetOwner());
	if (!OwnerController || !PinguAntiCheat::AllowRequest(OwnerController, PinguAntiCheat::EGate::Lobby))
	{
		return;
	}
	if (!PinguPicks::IsValid(InPicks))
	{
		PinguAntiCheat::NoteIgnored(OwnerController, TEXT("ability picks"), TEXT("not three different abilities"));
		return;
	}
	if (!SetPicks(InPicks))
	{
		PinguAntiCheat::NoteIgnored(OwnerController, TEXT("ability picks"), TEXT("outside the lobby"));
	}
}
