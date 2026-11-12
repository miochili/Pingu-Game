// Pingu - the replicated match state, and the round flow's music, sounds and camera moments on every machine.
//
// Who plays what (each exactly once per machine): the server's setters call the handlers directly (a listen server gets
// no replication notifies of its own), clients get the same handlers from OnRep_Phase / OnRep_CameraMoment. The
// countdown beeps follow GetCountdownNumber on every machine. None of it goes through FPinguFXBroadcastScope (that
// would play it twice on the clients).

#include "PenguinGameState.h"
#include "Penguin.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinPlayerState.h"
#include "PinguFX.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

namespace PenguinGameStatePrivate
{
	constexpr int32 MaxFeedLines = 5;
	constexpr float FeedLifetime = 5.f;
	/** A camera moment that reaches a client later than this is skipped (it would swoop after the fact). */
	constexpr float MaxCameraMomentAge = 1.f;
	/** Clients: the camera keeps snapping to the new round's framing this long (the penguins' new places and the new
	 *  island replicate a moment after the phase). */
	constexpr float ClientSnapHold = 0.35f;

	/** 1 = menu music (title, lobby, results), 2 = battle. */
	int32 MusicFor(EPinguPhase Phase)
	{
		switch (Phase)
		{
		case EPinguPhase::Intro:
		case EPinguPhase::Countdown:
		case EPinguPhase::Fight:
		case EPinguPhase::RoundOver:
		case EPinguPhase::Practice:
			return 2;
		default:
			return 1;
		}
	}
}

APenguinGameState::APenguinGameState()
{
	// Every machine ticks it: the countdown beeps, and the server's feed pruning.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	// Phase changes should reach the LAN clients within a frame or two (they are also pushed with ForceNetUpdate).
	SetNetUpdateFrequency(30.f);
}

void APenguinGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APenguinGameState, Phase);
	DOREPLIFETIME(APenguinGameState, PhaseStartTime);
	DOREPLIFETIME(APenguinGameState, PhaseSerial);
	DOREPLIFETIME(APenguinGameState, RoundNumber);
	DOREPLIFETIME(APenguinGameState, Scores);
	DOREPLIFETIME(APenguinGameState, Feed);
	DOREPLIFETIME(APenguinGameState, RoundWinnerSlot);
	DOREPLIFETIME(APenguinGameState, MatchWinnerSlot);
	DOREPLIFETIME(APenguinGameState, TeamWins);
	DOREPLIFETIME(APenguinGameState, RoundWinnerTeam);
	DOREPLIFETIME(APenguinGameState, MatchWinnerTeam);
	DOREPLIFETIME(APenguinGameState, Settings);
	DOREPLIFETIME(APenguinGameState, bMatchTest);
	DOREPLIFETIME(APenguinGameState, bTeamMatch);
	DOREPLIFETIME(APenguinGameState, bWaitingForContinue);
	DOREPLIFETIME(APenguinGameState, bPaused);
	DOREPLIFETIME(APenguinGameState, CollapseStartTime);
	DOREPLIFETIME(APenguinGameState, RoundTrait);
	DOREPLIFETIME(APenguinGameState, DeadlyEdges);
	DOREPLIFETIME(APenguinGameState, CameraMoment);
}

APenguinGameState* APenguinGameState::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	return World ? World->GetGameState<APenguinGameState>() : nullptr;
}

// ---------------------------------------------------------------------------- clock

bool APenguinGameState::IsNetworkGame() const
{
	return GetNetMode() != NM_Standalone;
}

double APenguinGameState::GetFlowTime() const
{
	return IsNetworkGame() ? GetServerWorldTimeSeconds() : SoloClock;
}

void APenguinGameState::OnRep_ReplicatedWorldTimeSecondsDouble()
{
	const UWorld* World = GetWorld();
	const double Sample = World ? ReplicatedWorldTimeSecondsDouble - World->GetTimeSeconds() : 0.0;
	const bool bOff = World && NumServerWorldTimeSecondsDeltas > 0 && FMath::Abs(Sample - static_cast<double>(ServerWorldTimeSecondsDelta)) > 0.3;
	ClockOffSamples = bOff ? ClockOffSamples + 1 : 0;
	Super::OnRep_ReplicatedWorldTimeSecondsDouble();
	if (ClockOffSamples >= 2)
	{
		UE_LOG(LogPingu, Display, TEXT("[NET] the server clock moved %.2f s against ours (a long frame on one machine): following it right away"),
			Sample - static_cast<double>(ServerWorldTimeSecondsDelta));
		SumServerWorldTimeSecondsDelta = Sample;
		NumServerWorldTimeSecondsDeltas = 1;
		ServerWorldTimeSecondsDelta = static_cast<float>(Sample);
		ClockOffSamples = 0;
	}
}

void APenguinGameState::AdvanceSoloClock(float RealSeconds)
{
	if (!IsNetworkGame())
	{
		SoloClock += FMath::Max(RealSeconds, 0.f);
	}
}

float APenguinGameState::GetPhaseTime() const
{
	// A client's synced server time can be a hair behind the server's phase start.
	return FMath::Max(0.f, static_cast<float>(GetFlowTime() - PhaseStartTime));
}

float APenguinGameState::GetFeedAge(const FPinguFeedEntry& Entry) const
{
	return FMath::Max(0.f, static_cast<float>(GetFlowTime() - Entry.Time));
}

float APenguinGameState::GetKnockbackScale() const
{
	if (Phase != EPinguPhase::Fight)
	{
		return 1.f;
	}
	// The longer a fight goes, the further every knock sends you: rounds can't stall.
	return GetKnockbackScaleAt(RoundTrait, GetPhaseTime());
}

float APenguinGameState::GetKnockbackScaleAt(EPinguTrait Trait, float FightTime)
{
	// Traits mode (2026-11-07): Fast Growth grows it faster, Pinball doubles it from the start.
	const float Growth = Trait == EPinguTrait::FastGrowth ? PinguTraitTuning::FastGrowthPerSecond : KnockbackGrowthPerSecond;
	const float Scale = FMath::Clamp(1.f + Growth * FMath::Max(FightTime, 0.f), 1.f, MaxKnockbackScale);
	return Trait == EPinguTrait::Pinball ? Scale * PinguTraitTuning::PinballKnockback : Scale;
}

// ---------------------------------------------------------------------------- reading

const FPinguScore* APenguinGameState::FindScore(int32 Slot) const
{
	const int32 Index = FindScoreIndex(Slot);
	return Index != INDEX_NONE ? &Scores[Index] : nullptr;
}

int32 APenguinGameState::FindScoreIndex(int32 Slot) const
{
	if (Slot < 0)
	{
		return INDEX_NONE;
	}
	return Scores.IndexOfByPredicate([Slot](const FPinguScore& Row) { return Row.Slot == Slot; });
}

TArray<int32> APenguinGameState::GetLeaderboard() const
{
	TArray<int32> Order;
	for (int32 Index = 0; Index < Scores.Num(); ++Index)
	{
		Order.Add(Index);
	}
	Order.StableSort([this](int32 A, int32 B)
	{
		const FPinguScore& ScoreA = Scores[A];
		const FPinguScore& ScoreB = Scores[B];
		if (ScoreA.Wins != ScoreB.Wins)
		{
			return ScoreA.Wins > ScoreB.Wins;
		}
		if (ScoreA.KOs != ScoreB.KOs)
		{
			return ScoreA.KOs > ScoreB.KOs;
		}
		if (ScoreA.Falls != ScoreB.Falls)
		{
			return ScoreA.Falls < ScoreB.Falls;
		}
		return ScoreA.Slot < ScoreB.Slot;
	});
	return Order;
}

TArray<int32> APenguinGameState::GetWinsBoard() const
{
	TArray<int32> Order;
	for (int32 Index = 0; Index < Scores.Num(); ++Index)
	{
		Order.Add(Index);
	}
	// Only what the board shows decides the order (no hidden knock-out counts).
	Order.StableSort([this](int32 A, int32 B)
	{
		return Scores[A].Wins != Scores[B].Wins ? Scores[A].Wins > Scores[B].Wins : Scores[A].Slot < Scores[B].Slot;
	});
	return Order;
}

bool APenguinGameState::IsTeamMatch() const
{
	return bTeamMatch && Phase != EPinguPhase::Lobby && Phase != EPinguPhase::Menu && Phase != EPinguPhase::Practice;
}

TArray<int32> APenguinGameState::GetTeamBoard() const
{
	TArray<int32> Teams;
	for (const FPinguScore& Row : Scores)
	{
		if (Row.Team >= 0)
		{
			Teams.AddUnique(Row.Team);
		}
	}
	Teams.Sort();
	Teams.StableSort([this](int32 A, int32 B) { return GetTeamWinCount(A) > GetTeamWinCount(B); });
	return Teams;
}

TArray<int32> APenguinGameState::GetTeamMembers(int32 Team) const
{
	TArray<int32> Members;
	for (int32 Index = 0; Index < Scores.Num(); ++Index)
	{
		if (Team >= 0 && Scores[Index].Team == Team)
		{
			Members.Add(Index);
		}
	}
	return Members;
}

int32 APenguinGameState::CountTeamMembers(int32 Team) const
{
	int32 Members = 0;
	for (const FPinguScore& Row : Scores)
	{
		Members += Team >= 0 && Row.Team == Team ? 1 : 0;
	}
	return Members;
}

int32 APenguinGameState::GetShownTeam(int32 Team) const
{
	return Team >= 0 && IsTeamMatch() && CountTeamMembers(Team) >= 2 ? Team : -1;
}

int32 APenguinGameState::GetLocalTeam() const
{
	const APenguinPlayerState* Local = GetLocalPenguinPlayer();
	if (!Local || !IsTeamMatch())
	{
		return -1;
	}
	const FPinguScore* Row = Scores.FindByPredicate([Local](const FPinguScore& Entry) { return Entry.PlayerState == Local; });
	return Row ? Row->Team : -1;
}

int32 APenguinGameState::GetCountdownNumber() const
{
	const float Elapsed = GetPhaseTime();
	if (Phase == EPinguPhase::Countdown)
	{
		return FMath::Clamp(3 - FMath::FloorToInt(Elapsed / CountStep), 1, 3);
	}
	if (Phase == EPinguPhase::Fight && Elapsed < GoTime)
	{
		return 0;
	}
	return -1;
}

TArray<APenguinPlayerState*> APenguinGameState::GetPenguinPlayers() const
{
	TArray<APenguinPlayerState*> Players;
	for (APlayerState* Player : PlayerArray)
	{
		APenguinPlayerState* PenguinPlayer = Cast<APenguinPlayerState>(Player);
		if (IsValid(PenguinPlayer) && !PenguinPlayer->IsABot() && !PenguinPlayer->IsInactive())
		{
			Players.Add(PenguinPlayer);
		}
	}
	// By slot (the host has 0); players still waiting for a slot last, in join order.
	Players.StableSort([](const APenguinPlayerState& A, const APenguinPlayerState& B)
	{
		const int32 SlotA = A.GetPenguinSlot() >= 0 ? A.GetPenguinSlot() : MAX_int32;
		const int32 SlotB = B.GetPenguinSlot() >= 0 ? B.GetPenguinSlot() : MAX_int32;
		return SlotA != SlotB ? SlotA < SlotB : A.GetPlayerId() < B.GetPlayerId();
	});
	return Players;
}

APenguinPlayerState* APenguinGameState::GetLocalPenguinPlayer() const
{
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const APlayerController* Controller = GameInstance ? GameInstance->GetFirstLocalPlayerController(World) : nullptr;
	return Controller ? Controller->GetPlayerState<APenguinPlayerState>() : nullptr;
}

// ---------------------------------------------------------------------------- writing (server)

void APenguinGameState::SetPhase(EPinguPhase NewPhase)
{
	Phase = NewPhase;
	PhaseStartTime = static_cast<float>(GetFlowTime());
	++PhaseSerial;
	ForceNetUpdate();
	HandlePhaseChanged(false);
}

void APenguinGameState::SetWinners(int32 InRoundWinnerSlot, int32 InMatchWinnerSlot)
{
	RoundWinnerSlot = InRoundWinnerSlot;
	MatchWinnerSlot = InMatchWinnerSlot;
}

void APenguinGameState::SetTeamWinners(int32 InRoundWinnerTeam, int32 InMatchWinnerTeam)
{
	RoundWinnerTeam = InRoundWinnerTeam;
	MatchWinnerTeam = InMatchWinnerTeam;
}

void APenguinGameState::AddFeed(const FString& Text, const FLinearColor& Color)
{
	FPinguFeedEntry& Entry = Feed.AddDefaulted_GetRef();
	Entry.Text = Text;
	Entry.Color = Color;
	Entry.Time = static_cast<float>(GetFlowTime());
	while (Feed.Num() > PenguinGameStatePrivate::MaxFeedLines)
	{
		Feed.RemoveAt(0);
	}
	ForceNetUpdate();
}

void APenguinGameState::PruneFeed()
{
	const double Now = GetFlowTime();
	Feed.RemoveAll([Now](const FPinguFeedEntry& Entry) { return Now - Entry.Time > PenguinGameStatePrivate::FeedLifetime; });
}

void APenguinGameState::FocusCamera(const FVector& Location, float Duration, float ZoomScale)
{
	CameraMoment.Location = Location;
	CameraMoment.Duration = Duration;
	CameraMoment.Zoom = ZoomScale;
	CameraMoment.Time = static_cast<float>(GetFlowTime());
	++CameraMoment.Serial;
	ForceNetUpdate();
	ApplyCameraMoment();
}

// ---------------------------------------------------------------------------- every machine

void APenguinGameState::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (HasAuthority())
	{
		PruneFeed();
	}
	UpdateCountdownCue(false);
}

void APenguinGameState::PostNetInit()
{
	Super::PostNetInit();
	// Whatever arrived with the first replication happened before we joined: from now on, changes play their sounds.
	bInitialReplicationDone = true;
	if (HandledPhaseSerial != PhaseSerial)
	{
		HandlePhaseChanged(true);
	}
}

void APenguinGameState::OnRep_Phase()
{
	HandlePhaseChanged(!bInitialReplicationDone);
}

void APenguinGameState::OnRep_CameraMoment()
{
	if (!bInitialReplicationDone)
	{
		return;  // a swoop from before we joined
	}
	const float Age = static_cast<float>(GetFlowTime()) - CameraMoment.Time;
	if (FMath::Abs(Age) <= PenguinGameStatePrivate::MaxCameraMomentAge)
	{
		ApplyCameraMoment();
	}
}

void APenguinGameState::ApplyCameraMoment() const
{
	if (APenguinCamera* Camera = APenguinCamera::FindOrSpawn(GetWorld()))
	{
		Camera->FocusOn(CameraMoment.Location, CameraMoment.Duration, CameraMoment.Zoom);
	}
}

void APenguinGameState::HandlePhaseChanged(bool bQuiet)
{
	using namespace PenguinGameStatePrivate;
	if (HandledPhaseSerial == PhaseSerial)
	{
		return;
	}
	HandledPhaseSerial = PhaseSerial;

	APinguFX::Music(this, MusicFor(Phase));

	// The server's game mode frames each new round itself; a client snaps when the phase arrives (and for a moment
	// after, while the penguins' new places and the new island come in).
	if (!HasAuthority() && (Phase == EPinguPhase::Intro || Phase == EPinguPhase::Lobby || Phase == EPinguPhase::Practice))
	{
		if (APenguinCamera* Camera = APenguinCamera::FindOrSpawn(GetWorld()))
		{
			Camera->SnapToTargets(ClientSnapHold);
		}
	}

	if (!bQuiet)
	{
		const APenguinPlayerState* LocalPlayer = GetLocalPenguinPlayer();
		const int32 LocalSlot = LocalPlayer ? LocalPlayer->GetPenguinSlot() : -1;
		switch (Phase)
		{
		case EPinguPhase::Intro:
			if (RoundNumber == 1)
			{
				APinguFX::Sound(this, EPinguSound::UIStart);  // a match starts (also a rematch)
			}
			break;

		case EPinguPhase::RoundOver:
		{
			// The fanfare when this machine's player (or its team) won the round, a softer gong for everybody else.
			const int32 LocalTeam = GetLocalTeam();
			const bool bWon = !bMatchTest && ((RoundWinnerSlot >= 0 && RoundWinnerSlot == LocalSlot) || (RoundWinnerTeam >= 0 && RoundWinnerTeam == LocalTeam));
			APinguFX::Sound(this, bWon ? EPinguSound::RoundWin : EPinguSound::CountGo, bWon ? 1.f : 0.6f, 0.f);
			break;
		}

		case EPinguPhase::MatchOver:
		{
			const int32 LocalTeam = GetLocalTeam();
			const bool bWon = !bMatchTest && ((MatchWinnerSlot >= 0 && MatchWinnerSlot == LocalSlot) || (MatchWinnerTeam >= 0 && MatchWinnerTeam == LocalTeam));
			APinguFX::Sound(this, bWon ? EPinguSound::MatchWin : EPinguSound::MatchLose);
			break;
		}

		default:
			break;
		}
	}
	UpdateCountdownCue(bQuiet);
}

void APenguinGameState::UpdateCountdownCue(bool bQuiet)
{
	const int32 Number = GetCountdownNumber();
	if (Number == CuedCountdownNumber)
	{
		return;
	}
	CuedCountdownNumber = Number;
	if (bQuiet || HandledPhaseSerial != PhaseSerial)
	{
		return;  // joined in the middle of a countdown (or its phase has not been handled yet): no beep for it
	}
	if (Number >= 1)
	{
		APinguFX::Sound(this, EPinguSound::CountBeep, 1.f, 0.f);
	}
	else if (Number == 0)
	{
		APinguFX::Sound(this, EPinguSound::CountGo, 1.f, 0.f);
	}
}
