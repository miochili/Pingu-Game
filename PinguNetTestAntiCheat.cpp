// Pingu - the LAN test's anti-cheat checks (PinguAntiCheat.h; see PinguNetTest.h). Every nettest run: the join check passed
// both ways, the client recomputed every deal from the shared seed and found it on every penguin, the host ignored nothing of
// the honest client's, and the client saw no rule break (no false alarm from normal LAN lag). The cheat runs (pipeline steps
// netcheathost / netcheatclient, no holds):
//  -PinguNetCheat=host   the host runs with -PinguCheat=picks,cooldown: its own penguin gets abilities of its choice and ignores
//                        its cooldowns. The client must leave the match by itself with "Your games don't match.".
//  -PinguNetCheat=client the client runs with -PinguCheat=client: it spams ability requests (far over the rate, a slot it does
//                        not have) and teleports its own penguin 20 m every 3 s in fights. The host must ignore all of it, the
//                        client plays on to the end (and sees no rule break of the honest host's).
//  -PinguNetCheat=settings the client runs with -PinguCheat=settings: it flips the host's TRAITS setting and skips the leaderboard
//                        four times a second. The host must ignore every one (its TRAITS setting stays as it was).
//  -PinguNetCheat=badhash the client runs with -PinguCheat=badhash: its join answer is wrong. The host must refuse it, the client
//                        must leave with "Your games don't match.".
//  -PinguNetCheat=tamper the client runs with -PinguCheat=tamper: its join answer carries a tamper token (what a modified
//                        packaged Shipping game produces via PinguTamper). The host must refuse it, the client must leave with
//                        "Your games don't match.". (Added 2026-11-05, not yet run: goes into the next LAN batch.)
//  -PinguNetCheat=move / combat: the client cheats with its own movement (PinguNetTestHostAuthority.cpp has those checks).
//  -PinguNetCheat=kick   the host runs with -PinguCheat=kick: it kicks the joiner 12 s after the join check, as the lobby's x
//                        does. The client must be back at the menu with "The host kicked you.", join again at once (a kick bans
//                        nobody: 2026-11-07) and stay (the dev kick fires once).
//  -PinguNetCheat=nohotjoin (with -PinguNetLate: the match runs before the joiner comes) the host turns "Join running matches"
//                        off (memory only): the joiner is turned away with "The match is running - join again when it ends ..."
//                        at least twice, then the host turns it back on and the same joiner gets in mid-match.
//  -PinguNetCheat=trainer the host runs with -PinguCheat=trainer: every 50 ms it writes its loadout (Snow Bomb, Ice Gun, Noot), zero
//                        cooldowns and doubled run speed / 4 air jumps / half gravity straight into memory, as PinguTrainer.exe
//                        did (2026-11-05). The host's guards must undo every write (restores counted) and the client sees an
//                        honest game: it stays, 0 points.
//  -PinguNetCheat=trainerraw the same with the host's guards off (-PinguCheat=trainer,noguard): the trainer works, the client's
//                        judge must catch it and leave with "Your games don't match.".
// (Every honest run also checks that the joiner's name is not the host's: both test windows start with the same player name.)

#include "PinguNetTest.h"
#include "Penguin.h"
#include "PinguAntiCheat.h"
#include "PinguMemoryGuard.h"
#include "PinguNames.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PinguSettings.h"
#include "EngineUtils.h"
#include "PinguNetSubsystem.h"
#include "PenguinGameState.h"
#include "PenguinPlayerState.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace PinguNetTestAntiCheatPrivate
{
	FString CheatRunKind()
	{
		FString Kind;
		FParse::Value(FCommandLine::Get(), TEXT("PinguNetCheat="), Kind);
		return Kind.ToLower();
	}

	/** The client's kicks seen (the dev kick fires once), and the host's hot-join switch: 0 not yet, 1 turned off, 2 turned back on. */
	int32 KicksSeen = 0;
	int32 HotJoinStage = 0;
	int32 RefusalsSeen = 0;
	/** The client's "connected" moments (a refused try counts too: its refusal arrives after it). */
	int32 ConnectsSeen = 0;
	/** The join error the client heard last (a refused join attempt). */
	FString LastJoinRefusal;

	/** The host's joiners still in the game. */
	int32 CountRemotePlayers(const UWorld* World)
	{
		int32 Count = 0;
		if (World)
		{
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				const APlayerController* Controller = It->Get();
				Count += Controller && !Controller->IsLocalController() ? 1 : 0;
			}
		}
		return Count;
	}
}

bool APinguNetTest::IsCheatRun()
{
	return !PinguNetTestAntiCheatPrivate::CheatRunKind().IsEmpty();
}

void APinguNetTest::NoteConnected()
{
	++PinguNetTestAntiCheatPrivate::ConnectsSeen;
}

void APinguNetTest::NoteJoinFailure(const FString& Error)
{
	PinguNetTestAntiCheatPrivate::LastJoinRefusal = Error;
}

bool APinguNetTest::RejoinAfterKick(const FString& Error)
{
	using namespace PinguNetTestAntiCheatPrivate;
	if (CheatRunKind() == TEXT("kick") && KicksSeen == 0 && Error == TEXT("The host kicked you."))
	{
		++KicksSeen;
		return true;
	}
	// nohotjoin: a refusal arrives after the join already counted as connected: note it, and try again (the host turns joining
	// back on after two).
	if (CheatRunKind() == TEXT("nohotjoin") && Error.StartsWith(TEXT("The match is running")) && RefusalsSeen < 8)
	{
		++RefusalsSeen;
		PinguNetTestAntiCheatPrivate::LastJoinRefusal = Error;
		return true;
	}
	return false;
}

void APinguNetTest::UpdateHotJoinSwitch()
{
	using namespace PinguNetTestAntiCheatPrivate;
	if (CheatRunKind() != TEXT("nohotjoin"))
	{
		return;
	}
	UPinguSettings* Prefs = UPinguSettings::Get();
	const APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr;
	if (!Prefs || !GameMode)
	{
		return;
	}
	if (HotJoinStage == 0)
	{
		HotJoinStage = 1;
		Prefs->SetJoinRunningMatches(false); // (an automated run saves nothing)
		Report(TEXT("hot-join: Join running matches turned OFF for this run"));
	}
	else if (HotJoinStage == 1 && GameMode->GetRefusedRunningJoins() >= 2)
	{
		HotJoinStage = 2;
		Prefs->SetJoinRunningMatches(true);
		Report(FString::Printf(TEXT("hot-join: %d join(s) turned away, Join running matches back ON"), GameMode->GetRefusedRunningJoins()));
	}
}

void APinguNetTest::AddAntiCheatChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestAntiCheatPrivate;
	const FString Kind = CheatRunKind();
	const PinguAntiCheat::FClientReport& Seen = PinguAntiCheat::GetClientReport();
	const int32 Ok = PinguAntiCheat::GetHandshakesOk();
	const int32 Failed = PinguAntiCheat::GetHandshakesFailed();
	const int32 Ignored = PinguAntiCheat::GetIgnoredCount();
	const int32 IgnoredMoves = PinguAntiCheat::GetIgnoredMoves();
	const FString Strikes = Seen.Strikes.Num() > 0 ? FString::Join(Seen.Strikes, TEXT(" / ")) : FString(TEXT("none"));
	UPinguNetSubsystem* Net = UPinguNetSubsystem::Get(this);
	const FString LastError = Net ? Net->GetLastError().ToString() : FString();

	if (bHostRole)
	{
		const int32 Remote = CountRemotePlayers(GetWorld());
		if (Kind == TEXT("host"))
		{
			Check(TEXT("anticheat: the client passed the join check"), Ok >= 1 && Failed == 0, FString::Printf(TEXT("%d passed, %d failed"), Ok, Failed));
			Check(TEXT("anticheat: the cheating host's client left by itself"), Remote == 0,
				FString::Printf(TEXT("%d joiner(s) still here at the end"), Remote));
			return;
		}
		if (Kind == TEXT("trainer") || Kind == TEXT("trainerraw"))
		{
			int32 Restores = 0;
			for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
			{
				Restores += IsValid(*It) && It->GetAbilities() ? It->GetAbilities()->GetShadowRestores() : 0;
			}
			const int32 MoveRestores = PinguMemoryGuard::GetMovementRestores();
			if (Kind == TEXT("trainer"))
			{
				Check(TEXT("anticheat: the trainer's writes undone on the host, the client still plays"), Ok >= 1 && Remote >= 1 && Restores >= 10 && MoveRestores >= 10,
					FString::Printf(TEXT("%d passed, %d joiner(s) here (want 1), %d loadout / cooldown and %d movement writes undone (want 10 or more each)"),
						Ok, Remote, Restores, MoveRestores));
			}
			else
			{
				Check(TEXT("anticheat: the trainer (guards off) works on the host, its client left by itself"), Ok >= 1 && Remote == 0 && Restores == 0,
					FString::Printf(TEXT("%d passed, %d joiner(s) still here (want 0), %d writes undone (want 0: guards off)"), Ok, Remote, Restores));
			}
			return;
		}
		if (Kind == TEXT("settings"))
		{
			const APenguinGameState* State = APenguinGameState::Get(this);
			Check(TEXT("anticheat: the joiner's TRAITS flips and leaderboard skips ignored"), Ignored >= 10 && Remote >= 1 && State && !State->AreTraitsOn(),
				FString::Printf(TEXT("%d ignored (want 10 or more), %d joiner(s) here, TRAITS %s (want off)"), Ignored, Remote, State && State->AreTraitsOn() ? TEXT("on") : TEXT("off")));
			return;
		}
		if (Kind == TEXT("badhash"))
		{
			Check(TEXT("anticheat: the faked join answer refused, the joiner removed"), Failed >= 1 && Ok == 0 && Remote == 0,
				FString::Printf(TEXT("%d passed (want 0), %d failed (want 1 or more), %d joiner(s) still here (want 0)"), Ok, Failed, Remote));
			return;
		}
		if (Kind == TEXT("tamper"))
		{
			// The joiner ran -PinguCheat=tamper: its handshake answer carries a tamper token, so it fails the join check
			// exactly like a modified game would (PinguTamper folds the real token in a packaged Shipping build).
			Check(TEXT("anticheat: the tampered joiner refused, removed"), Failed >= 1 && Ok == 0 && Remote == 0,
				FString::Printf(TEXT("%d passed (want 0), %d failed (want 1 or more), %d joiner(s) still here (want 0)"), Ok, Failed, Remote));
			return;
		}
		if (Kind == TEXT("kick"))
		{
			// The kick bans nobody: the same joiner passed the join check again (2 passes) and is here at the end.
			Check(TEXT("kick: the kicked joiner came back at once and is here"), Ok >= 2 && Remote >= 1,
				FString::Printf(TEXT("%d join checks passed (want 2 or more: before and after the kick), %d joiner(s) here at the end (want 1)"), Ok, Remote));
			return;
		}
		if (Kind == TEXT("nohotjoin"))
		{
			const APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr;
			const int32 Refused = GameMode ? GameMode->GetRefusedRunningJoins() : -1;
			Check(TEXT("hot-join off: a mid-match joiner turned away, then let in when it was back on"), Refused >= 2 && HotJoinStage == 2 && Remote >= 1,
				FString::Printf(TEXT("%d joiner(s) turned away while off (want 2 or more), switch stage %d (want 2), %d joiner(s) here at the end (want 1)"), Refused, HotJoinStage, Remote));
			return;
		}
		if (Kind == TEXT("client"))
		{
			Check(TEXT("anticheat: the client passed the join check"), Ok >= 1 && Failed == 0, FString::Printf(TEXT("%d passed, %d failed"), Ok, Failed));
			Check(TEXT("anticheat: the cheating client's requests ignored"), Ignored >= 10 && IgnoredMoves >= 1,
				FString::Printf(TEXT("%d ignored, %d of them moves (want 10 or more, 1 or more)"), Ignored, IgnoredMoves));
			Check(TEXT("anticheat: the cheating client still plays (blocked silently)"), Remote >= 1, FString::Printf(TEXT("%d joiner(s) here at the end"), Remote));
			return;
		}
		if (Kind == TEXT("move") || Kind == TEXT("combat") || Kind == TEXT("flap"))
		{
			// The movement cheats (PinguNetTestHostAuthority.cpp judges them): here only that it joined and plays on.
			Check(TEXT("anticheat: the client passed the join check and still plays"), Ok >= 1 && Failed == 0 && Remote >= 1,
				FString::Printf(TEXT("%d passed, %d failed, %d joiner(s) here at the end"), Ok, Failed, Remote));
			return;
		}
		AddPicksChecks(Check); // -PinguNetCheat=choose (PinguNetTestPicks.cpp)
		{
			// Both windows start with the same player name: the joiner's must have got a number, and never look like the host's.
			const APenguinGameState* State = APenguinGameState::Get(this);
			FString HostName;
			FString JoinerName;
			if (State)
			{
				for (const APlayerState* Player : State->PlayerArray)
				{
					const APenguinPlayerState* PenguinPlayer = Cast<APenguinPlayerState>(Player);
					if (PenguinPlayer)
					{
						(PenguinPlayer->IsHost() ? HostName : JoinerName) = PenguinPlayer->GetPlayerName();
					}
				}
			}
			Check(TEXT("anticheat: the joiner's name is not the host's, nor a lookalike"),
				!HostName.IsEmpty() && !JoinerName.IsEmpty() && PinguNames::Fold(HostName) != PinguNames::Fold(JoinerName),
				FString::Printf(TEXT("host \"%s\", joiner \"%s\""), *HostName, *JoinerName));
		}
		Check(TEXT("anticheat: join check passed, nothing of the honest client's ignored"), Ok >= 1 && Failed == 0 && Ignored == 0,
			FString::Printf(TEXT("%d passed, %d failed, %d request(s) ignored (%d moves)%s"), Ok, Failed, Ignored, IgnoredMoves,
				Ignored > 0 ? *(TEXT(": ") + FString::Join(PinguAntiCheat::GetIgnoredLines(), TEXT(" / "))) : TEXT("")));
		return;
	}

	if (Kind == TEXT("badhash"))
	{
		Check(TEXT("anticheat: the faked join answer made us leave with \"Your games don't match.\""), LastError == TEXT("Your games don't match."),
			FString::Printf(TEXT("error \"%s\""), *LastError));
		return;
	}
	if (Kind == TEXT("tamper"))
	{
		Check(TEXT("anticheat: the tamper check made us leave with \"Your games don't match.\""), LastError == TEXT("Your games don't match."),
			FString::Printf(TEXT("error \"%s\""), *LastError));
		return;
	}
	if (Kind == TEXT("kick"))
	{
		// (The checks run when the host has left and we are back in a solo world: what we saw is counted on the way.)
		Check(TEXT("kick: kicked by the host (\"The host kicked you.\"), joined again at once and played on"),
			KicksSeen == 1 && ConnectsSeen >= 2 && Seen.bHandshakeOk && !Seen.bLeft,
			FString::Printf(TEXT("%d kick(s) seen (want 1), connected %d time(s) (want 2), join check %d, left by itself %d (want 0), last error \"%s\""),
				KicksSeen, ConnectsSeen, Seen.bHandshakeOk ? 1 : 0, Seen.bLeft ? 1 : 0, *LastError));
		return;
	}
	if (Kind == TEXT("nohotjoin"))
	{
		const FString& Refusal = PinguNetTestAntiCheatPrivate::LastJoinRefusal;
		Check(TEXT("hot-join off: turned away mid-match with the message, then joined"),
			Refusal.Contains(TEXT("The match is running - join again when it ends")) && RefusalsSeen >= 2 && Seen.bHandshakeOk && !Seen.bLeft,
			FString::Printf(TEXT("refusal \"%s\", %d refusal(s) (want 2 or more), join check passed %d (only after a real join), left by itself %d (want 0)"),
				*Refusal, RefusalsSeen, Seen.bHandshakeOk ? 1 : 0, Seen.bLeft ? 1 : 0));
		return;
	}
	if (Kind == TEXT("trainer"))
	{
		Check(TEXT("anticheat: the guarded host's trainer never showed: no rule break seen, still playing"), Seen.bHandshakeOk && !Seen.bLeft && Seen.Points == 0 && Seen.UsesJudged >= 1,
			FString::Printf(TEXT("join %d, left %d, %d points (want 0), %d uses judged; seen: %s"), Seen.bHandshakeOk ? 1 : 0, Seen.bLeft ? 1 : 0, Seen.Points,
				Seen.UsesJudged, *Strikes));
		return;
	}
	if (Kind == TEXT("host") || Kind == TEXT("trainerraw"))
	{
		// (the strike kinds since 2026-11-05: "not dealt <ability>", "too many uses <ability>", "no cooldown", "cooldown cut")
		const bool bCaught = Seen.Strikes.ContainsByPredicate([](const FString& Line)
		{
			return Line.StartsWith(TEXT("not dealt")) || Line.StartsWith(TEXT("too many uses")) || Line.StartsWith(TEXT("no cooldown"))
				|| Line.StartsWith(TEXT("cooldown cut"));
		});
		Check(TEXT("anticheat: join check passed"), Seen.bHandshakeOk, Seen.bHandshakeOk ? FString(TEXT("yes")) : FString(TEXT("no")));
		Check(TEXT("anticheat: the cheating host caught (its picks / cooldowns), left with \"Your games don't match.\""),
			Seen.bLeft && bCaught && Seen.Points >= FPinguCheatJudge::LeaveAt && LastError == TEXT("Your games don't match."),
			FString::Printf(TEXT("left %d (%s), %d points, error \"%s\"; seen: %s"), Seen.bLeft ? 1 : 0, *Seen.LeftWhy, Seen.Points, *LastError, *Strikes));
		return;
	}
	AddPicksChecks(Check); // -PinguNetCheat=choose (PinguNetTestPicks.cpp)
	Check(TEXT("anticheat: join check passed"), Seen.bHandshakeOk, Seen.bHandshakeOk ? FString(TEXT("yes")) : FString(TEXT("no")));
	Check(TEXT("anticheat: every deal recomputed and found, the host's play judged, no false alarm"),
		!Seen.bLeft && Seen.Points == 0 && Seen.DealsVerified >= 3 && Seen.UsesJudged >= 1 && Seen.SamplesJudged >= 1,
		FString::Printf(TEXT("%d value(s) added, %d deals verified (want 3 or more), %d uses and %d motion samples judged, %d points (want 0), left %d; seen: %s"),
			Seen.Contributions, Seen.DealsVerified, Seen.UsesJudged, Seen.SamplesJudged, Seen.Points, Seen.bLeft ? 1 : 0, *Strikes));
}
