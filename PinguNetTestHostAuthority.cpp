// Pingu - the LAN test's checks of the host's say over the joiner's own penguin (PinguHostAuthority.h, card
// 2026-10-30_host-authority; see PinguNetTest.h). Every honest nettest: the host's knockback / freeze of the client's penguin was
// handed back on the host's path, nothing pulled back, and the host's corrections a minute (the honest client must feel none).
// The cheat runs (pipeline steps, no holds; the client cheats with -PinguCheat, both run -PinguNetCheat=<kind>):
//  -PinguNetCheat=move    the client runs -PinguCheat=fly,speed,fakey,noack: it flies out over the sea and hangs there (gravity
//                         0), runs at 6x its speed saying it is in the air, moves 3 m off the play plane, never confirms an effect
//                         and jumps 15 m along the island every 2 s. The host must refuse those moves, take its penguin over and
//                         put it back, keep it on Y 0 - and a flying one still ends in the sea.
//  -PinguNetCheat=combat  the client runs -PinguCheat=noknock,slide,stomp,giant: it ignores knockbacks / freezes / pulls, claims a
//                         new belly slide in every sync, a stomp landing every 0.5 s and a hard landing every 0.3 s. The host knocks
//                         its penguin toward the sea every 3 s of a fight: the host must move it there itself (into the sea), count
//                         about one slide a cooldown, and none of the stomp or landing claims.

#include "PinguNetTest.h"
#include "Penguin.h"
#include "BlockIsland.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PinguAntiCheat.h"
#include "PinguHostAuthority.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace PinguNetTestHostAuthorityPrivate
{
	/** The host knocks the cheating client's penguin toward the sea this often in a fight (s), this hard, stunned this long. */
	constexpr float KnockEvery = 3.f;
	constexpr float KnockSide = 2600.f;
	constexpr float KnockUp = 1000.f;
	constexpr float KnockStun = 0.8f;

	FString RunKind()
	{
		FString Kind;
		FParse::Value(FCommandLine::Get(), TEXT("PinguNetCheat="), Kind);
		return Kind.ToLower();
	}

	/** What the host saw of the client's penguin (the whole test: the worlds come and go). */
	struct FWatch
	{
		float KnockClock = 0.f;
		int32 Knocks = 0;
		double LastKnock = -100.0;
		int32 SeaOuts = 0;
		int32 SeaOutsAfterKnock = 0;
		bool bWasOut = false;
		float MostOffPlane = 0.f;
		float FightSeconds = 0.f;
		int32 Samples = 0;
		/** Honest runs, round 3: the host flaps the client's penguin now and then (as its Flap ability would). */
		float FlapClock = 0.f;
		int32 HostFlaps = 0;
		float RoundThreeSeconds = 0.f;
		/** Honest runs, rounds 2 on: the host gives the client's penguin ice feet now and then (the ice gun's hit: a slide, 2026-11-05). */
		float SlipClock = 0.f;
		int32 HostSlips = 0;
		float SlipSeconds = 0.f;
	};

	FWatch& Watch()
	{
		static FWatch Seen;
		return Seen;
	}
}

void APinguNetTest::UpdateHostAuthority(float DeltaSeconds)
{
	using namespace PinguNetTestHostAuthorityPrivate;
	if (!bHostRole || GetNetMode() != NM_ListenServer)
	{
		return;
	}
	FWatch& Seen = Watch();
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	APenguinCharacter* Client = GetOtherPlayerPenguin();
	if (!GameState || !Client)
	{
		return;
	}
	const bool bFight = GameState->GetPhase() == EPinguPhase::Fight;
	const double Now = FPlatformTime::Seconds();

	// The client's penguin as the host has it: on the play plane, and how often it went into the sea.
	if (bFight && Client->IsAlive())
	{
		Seen.FightSeconds += DeltaSeconds;
		++Seen.Samples;
		Seen.MostOffPlane = FMath::Max(Seen.MostOffPlane, FMath::Abs(static_cast<float>(Client->GetActorLocation().Y)));
	}
	const bool bOut = Client->IsKnockedOut();
	if (bOut && !Seen.bWasOut && Client->GetKOReason() == EPenguinKOReason::Sea)
	{
		++Seen.SeaOuts;
		const bool bAfterKnock = Now - Seen.LastKnock < 6.0;
		Seen.SeaOutsAfterKnock += bAfterKnock ? 1 : 0;
		Report(FString::Printf(TEXT("host authority: the client's penguin went into the sea%s (%d so far)"),
			bAfterKnock ? TEXT(" after our knock") : TEXT(""), Seen.SeaOuts));
	}
	Seen.bWasOut = bOut;

	// Honest runs, round 3's free fight: every 5 s the host starts a flap of the client's penguin (what its Flap ability does on
	// the host): its climb must never be refused (the host's judge knows of it: UPenguinMovementComponent::StartFlap).
	if (RunKind().IsEmpty() && bFight && GameState->GetRoundNumber() >= 3 && Client->IsAlive())
	{
		Seen.FlapClock += DeltaSeconds;
		Seen.RoundThreeSeconds += DeltaSeconds;
		UPenguinMovementComponent* FlapMove = Client->GetPenguinMovement();
		if (Seen.FlapClock >= 5.f && FlapMove && !FlapMove->IsHostOwned() && FlapMove->StartFlap())
		{
			Seen.FlapClock = 0.f;
			++Seen.HostFlaps;
			Report(FString::Printf(TEXT("host authority: flapped the client's penguin (flap %d, %.0f cm up)"), Seen.HostFlaps, FlapMove->GetLastFlapHeight()));
		}
	}

	// Honest runs, round 2 on: every 7 s the host gives the client's penguin 3 s of ice feet (what the ice gun's hit does): it
	// slides on its own (no walking), and the host's judge must never refuse or pull back the slide (the honest check below).
	if (RunKind().IsEmpty() && bFight && GameState->GetRoundNumber() >= 2 && Client->IsAlive())
	{
		Seen.SlipClock += DeltaSeconds;
		Seen.SlipSeconds += DeltaSeconds;
		if (Seen.SlipClock >= 7.f && !Client->GetPenguinMovement()->IsSlippery())
		{
			Seen.SlipClock = 0.f;
			++Seen.HostSlips;
			Client->SetSlippery(3.f);
			Report(FString::Printf(TEXT("host authority: gave the client's penguin ice feet for 3 s (slip %d)"), Seen.HostSlips));
		}
	}

	// The combat run: now and then a hard knock toward the nearer sea (the cheating client ignores it on its own machine).
	if (RunKind() == TEXT("combat") && bFight && Client->IsAlive())
	{
		Seen.KnockClock += DeltaSeconds;
		UPenguinMovementComponent* Move = Client->GetPenguinMovement();
		if (Seen.KnockClock >= KnockEvery && Move && !Move->IsHostOwned())
		{
			Seen.KnockClock = 0.f;
			const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Client->GetActorLocation());
			const float MiddleX = Island ? static_cast<float>(Island->GetBounds().GetCenter().X) : 0.f;
			const float Side = Client->GetActorLocation().X >= MiddleX ? 1.f : -1.f;
			Move->ApplyKnockback(FVector2D(Side * KnockSide, KnockUp), KnockStun);
			++Seen.Knocks;
			Seen.LastKnock = Now;
			Report(FString::Printf(TEXT("host authority: knocked the client's penguin toward the sea (%s, knock %d)"), Side > 0.f ? TEXT("right") : TEXT("left"), Seen.Knocks));
		}
	}
}

void APinguNetTest::AddHostAuthorityChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestHostAuthorityPrivate;
	if (!bHostRole)
	{
		return; // (the host decides: the client's own view has nothing to judge here)
	}
	const FString Kind = RunKind();
	const FWatch& Seen = Watch();
	const PinguHostAuthority::FStats& N = PinguHostAuthority::Stats();
	const float PerMinute = PinguHostAuthority::CorrectionsPerMinute();
	const int32 OldGate = PinguAntiCheat::GetIgnoredMoves();
	Report(FString::Printf(TEXT("host authority numbers: %s; the old rules (before 2026-10-30) alone refused %d; the client's penguin in fights %.0f s, into the sea %d times"),
		*PinguHostAuthority::DescribeStats(), OldGate, Seen.FightSeconds, Seen.SeaOuts));

	if (Kind == TEXT("move"))
	{
		Check(TEXT("host authority: the flying / fast / jumping client's moves refused, its penguin taken over and put back"),
			N.Refused + N.IgnoredWhileTaken >= 20 && N.Penalties >= 1 && N.PullBacks >= 1,
			FString::Printf(TEXT("%d refused + %d ignored while taken over (want 20 or more together), %d take-overs, %d pull-backs (want 1 or more each)"),
				N.Refused, N.IgnoredWhileTaken, N.Penalties, N.PullBacks));
		Check(TEXT("host authority: the client's penguin stays on the play plane"), N.YClamped >= 10 && Seen.MostOffPlane < 1.f && Seen.Samples > 0,
			FString::Printf(TEXT("%d moves put back on y 0 (want 10 or more), the host's copy at most %.1f cm off it (want under 1)"), N.YClamped, Seen.MostOffPlane));
		Check(TEXT("host authority: hanging in the air over the sea does not save it"), Seen.SeaOuts >= 1,
			FString::Printf(TEXT("the client's penguin went into the sea %d times (want 1 or more)"), Seen.SeaOuts));
		return;
	}
	if (Kind == TEXT("combat"))
	{
		const int32 MostSlides = FMath::CeilToInt(Seen.FightSeconds / (1.f - static_cast<float>(FPinguSlideGate::Slack))) + 4;
		Check(TEXT("host authority: the knocks the client ignored moved its penguin anyway (the host's own flight)"),
			Seen.Knocks >= 2 && N.Handovers >= Seen.Knocks - 1 && N.HandoverMisses >= 1 && Seen.SeaOutsAfterKnock >= 1,
			FString::Printf(TEXT("%d knocks, %d hand-backs, %d of them off the host's path (want 1 or more), into the sea after a knock %d times (want 1 or more)"),
				Seen.Knocks, N.Handovers, N.HandoverMisses, Seen.SeaOutsAfterKnock));
		Check(TEXT("host authority: a new belly slide in every sync, about one a cooldown counted"),
			N.SlidesRefused >= 50 && N.SlidesAccepted <= MostSlides,
			FString::Printf(TEXT("%d counted (want %d at most in %.0f s of fights), %d refused (want 50 or more)"), N.SlidesAccepted, MostSlides, Seen.FightSeconds, N.SlidesRefused));
		Check(TEXT("host authority: made-up stomp and hard landings ignored"), N.StompClaimsIgnored >= 5 && N.LandsRefused >= 5,
			FString::Printf(TEXT("%d stomp landings ignored, %d landings refused (want 5 or more each)"), N.StompClaimsIgnored, N.LandsRefused));
		return;
	}
	if (Kind == TEXT("flap"))
	{
		Check(TEXT("host authority: flaps the client made up (flying up) refused, its penguin taken over"),
			N.Refused + N.IgnoredWhileTaken >= 1 && N.Penalties >= 1,
			FString::Printf(TEXT("%d refused + %d ignored while taken over (want 1 or more), %d take-overs (want 1 or more)"), N.Refused, N.IgnoredWhileTaken, N.Penalties));
		return;
	}
	if (!Kind.IsEmpty())
	{
		return; // (the other cheat runs: PinguNetTestAntiCheat.cpp)
	}
	// The honest client: the host's knockback / freeze of its penguin handed back where its own path went, nothing pulled back,
	// no correction it could feel.
	Check(TEXT("host authority: the honest client feels no correction"),
		N.Handovers >= 1 && N.HandoverMisses == 0 && N.PullBacks == 0 && N.Refused == 0 && N.YClamped == 0 && N.SlidesRefused <= 1
			&& (Seen.HostFlaps >= 1 || Seen.RoundThreeSeconds < 6.f) && (Seen.HostSlips >= 1 || Seen.SlipSeconds < 9.f),
		FString::Printf(TEXT("%d hand-backs (want 1 or more), %d off the path, %d pull-backs, %d refused, %d off y 0, %d slides refused (want 0 each, a slide racing a hit: 1); %d flaps of the host's (want 1 or more after 6 s of round 3: %.0f s); %d ice-feet slides of the host's (want 1 or more after 9 s of fights from round 2: %.0f s); %.2f corrections a minute"),
			N.Handovers, N.HandoverMisses, N.PullBacks, N.Refused, N.YClamped, N.SlidesRefused, Seen.HostFlaps, Seen.RoundThreeSeconds, Seen.HostSlips, Seen.SlipSeconds, PerMinute));
}
