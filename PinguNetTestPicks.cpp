// Pingu - the LAN test of the ability picks (2026-11-07, card ability-picks; PinguAbilityPicks.h). Step netcheatchoose
// (-PinguNetCheat=choose, both windows -PinguChoose: the host's lobby is ABILITIES: CHOOSE): the client picks Snowball, random,
// Flap in the lobby (APenguinPlayerState::RequestPicks -> the host's Rq28); the host's match deals them through the fair deal
// (the reveal carries the picks). Both log a check: the joiner's penguin held Snowball / a random other / Flap at every round's
// countdown (want 3 rounds or more). The client's usual anti-cheat check must stay at 0 points with every deal found.

#include "PinguNetTest.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinPlayerState.h"
#include "PinguAbilityPicks.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace PinguNetTestPicksPrivate
{
	bool IsChooseRun()
	{
		FString Kind;
		FParse::Value(FCommandLine::Get(), TEXT("PinguNetCheat="), Kind);
		return Kind.ToLower() == TEXT("choose");
	}
	bool bSent = false;
	TSet<int32> RoundsSeen;
	int32 RoundsRight = 0;
	FString FirstWrong;
}

void APinguNetTest::PreparePicksMatch(FPinguMatchSettings& Settings)
{
	// (Every lobby match keeps the lobby's switch: the test's own settings read it the same way.)
	Settings.bChooseAbilities = PinguPicks::LoadChooseSetting();
}

void APinguNetTest::UpdatePicksRun()
{
	using namespace PinguNetTestPicksPrivate;
	if (!IsChooseRun())
	{
		return;
	}
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (!GameState)
	{
		return;
	}
	if (!bHostRole && !bSent && GameState->GetPhase() == EPinguPhase::Lobby && GameState->GetSettings().bChooseAbilities)
	{
		const APlayerController* Own = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (APenguinPlayerState* State = Own ? Own->GetPlayerState<APenguinPlayerState>() : nullptr)
		{
			bSent = true;
			State->RequestPicks(PinguPicks::ToBytes({ EPenguinAbility::Snowball, EPenguinAbility::None, EPenguinAbility::Flap }));
			Report(TEXT("picks: asked the host for Snowball, random, Flap"));
		}
	}
	if (GameState->GetPhase() != EPinguPhase::Countdown || RoundsSeen.Contains(GameState->GetRoundNumber()))
	{
		return;
	}
	// The joiner's penguin: on the client our own, on the host the one a remote player controls.
	const APenguinCharacter* Joiner = nullptr;
	if (!bHostRole)
	{
		Joiner = GetOwnPenguin();
	}
	else
	{
		for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
		{
			if (It->IsPlayerControlled() && !It->IsLocallyControlled())
			{
				Joiner = *It;
			}
		}
	}
	const UPenguinAbilityComponent* Abilities = Joiner ? Joiner->GetAbilities() : nullptr;
	if (!Abilities || Abilities->GetAbilities().Num() != 3)
	{
		return;
	}
	const TArray<EPenguinAbility>& Loadout = Abilities->GetAbilities();
	RoundsSeen.Add(GameState->GetRoundNumber());
	const bool bRight = Loadout[0] == EPenguinAbility::Snowball && Loadout[2] == EPenguinAbility::Flap && Loadout[1] != EPenguinAbility::None
		&& Loadout[1] != EPenguinAbility::Snowball && Loadout[1] != EPenguinAbility::Flap;
	RoundsRight += bRight ? 1 : 0;
	const FString Text = PinguPicks::Describe(PinguPicks::ToBytes(Loadout));
	if (!bRight && FirstWrong.IsEmpty())
	{
		FirstWrong = FString::Printf(TEXT("round %d: %s"), GameState->GetRoundNumber(), *Text);
	}
	Report(FString::Printf(TEXT("picks: round %d the joiner holds %s"), GameState->GetRoundNumber(), *Text));
}

void APinguNetTest::AddPicksChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestPicksPrivate;
	if (!IsChooseRun())
	{
		return;
	}
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	const int32 Rounds = RoundsSeen.Num();
	Check(TEXT("picks: the joiner's picks dealt every round (Snowball, a random other, Flap)"), (bHostRole || bSent) && Rounds >= 3 && RoundsRight == Rounds,
		FString::Printf(TEXT("%d of %d rounds right (want 3 or more, all)%s%s"), RoundsRight, Rounds, bHostRole ? TEXT("") : (bSent ? TEXT(", asked") : TEXT(", never asked")),
			FirstWrong.IsEmpty() ? TEXT("") : *(TEXT("; first wrong ") + FirstWrong)));
	(void)GameState;
}
