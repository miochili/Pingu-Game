// Pingu - automated LAN test (see PinguNetTest.h).
//
// Host:   solo world -> HostGame -> listen world (lobby): wait for the client -> StartMatch (1 bot, first to 2) -> MatchOver
//         + 3 s -> RESULT lines -> LeaveGame -> quit.
// -PinguNetLate (both): the host starts a team match (2 teams, 1 bot, first to 3) right away, alone with its bot; the client
//         joins while it runs, watches until the next round and plays from there (no slide checks). Both machines check the
//         teams as they show them: host + client a team (a ring each), the bot alone in the other one shown on its own.
// Client: solo world -> JoinGame (retries while the host is not up yet) -> client world: our penguin plays by its bot brain
//         through the real owner -> server path -> the host leaves -> back in a solo world -> RESULT lines -> quit.
// The actor is new in every world; what it knows across the map loads lives in FProgress. Lines that both machines should
// log the same way (phases, penguins, feed, scores, island blocks) carry no role or time, so the two logs diff cleanly.

#include "PinguNetTest.h"
#include "PinguNetTestGrant.h"
#include "PinguNetTestKillCam.h"
#include "RenderTimer.h"
#include "DynamicRHI.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinBot.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PenguinMovementComponent.h"
#include "PenguinPlayerState.h"
#include "PenguinProjectile.h"
#include "PinguAntiCheat.h"
#include "PinguNetSubsystem.h"
#include "PinguPickup.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace PinguNetTestPrivate
{
	constexpr float StartDelay = 2.f;          // in the solo world before hosting / the first join
	constexpr float JoinRetryDelay = 2.f;      // a join attempt failed: try again after this
	constexpr int32 MaxJoinAttempts = 6;
	constexpr float BeaconWait = 40.f;         // client: before the first join, wait this long for the host's LAN beacon
	constexpr float ClientWaitTimeout = 90.f;  // host: a client must join within this
	constexpr float MatchStartDelay = 3.f;     // host: the client's penguin spawns and replicates meanwhile
	// Host: the match also waits for the client's lobby Snow Warp (IsLobbyWarpSettled), but never longer than this after the join.
	constexpr float LobbyWarpWait = 20.f;
	constexpr float QuitAfterMatchOver = 3.f;
	constexpr float QuitAfterLeave = 5.f;      // host: the leave did not bring us to a solo world: quit anyway
	constexpr double HostTimeLimit = 240.0;
	constexpr double ClientTimeLimit = 260.0;
	// -PinguNetLate: the match starts this long after the lobby is up, and the test may take longer (more rounds).
	constexpr float LateMatchStartDelay = 3.f;
	constexpr double LateTimeLimit = 420.0;
	// Client: at most this many frames a second. Both test processes share this PC's one GPU, and Windows serves the window in
	// front first - the client's, started second. Uncapped (about 100 fps) it left the host, the server, at 5-7 fps: too slow
	// for its per-frame samples and its knock-out timing (2026-10-20, laptop). A real LAN game has a GPU on each machine.
	constexpr float ClientMaxFPS = 50.f;
	constexpr float TeleportStep = 400.f;      // a jump this big in one frame is a respawn, not walking
	constexpr float MinOwnDistance = 500.f;
	constexpr int32 MinRounds = 3;             // the host ends the (endless) match after this many (round 3: a free fight - the joiner judges the host's motion; 2 before 2026-11-04)
	constexpr int32 ExpectedPenguins = 3;      // host + client + 1 bot
	// The belly slide over LAN (APinguNetTest::UpdateSlideCheck), phase times in rounds 1 and 2.
	constexpr float SlideHoldFrom = 0.f;       // everybody holds still from the start of the fight (no knock-out first) ...
	constexpr float SlideHoldTo = 4.5f;        // ... to here (then it is judged)
	constexpr float SlideSetupAt = 0.9f;       // host: the platform, the two players' penguins on it
	constexpr float SlidePressAt = 2.2f;       // the slider slides
	constexpr float SlideGap = 450.f;          // centre to centre: the slide has gone its three blocks when it gets there
	constexpr float SlideGapTolerance = 80.f;
	constexpr float SlideStickTime = 0.15f;    // the slider's stick points the slide's way this long
	constexpr float SlideFullLength = 560.f;   // a slide that stopped before this ran into something
	constexpr int32 SlidePlatformColumns = 16;
	constexpr int32 SlidePlatformRowsUp = 4;   // above the island's highest block
	constexpr float SlidePlatformLife = 5.f;
	// The stun / ice check (APinguNetTest::UpdateEffectCheck), after the slide check in the same hold (phase times).
	constexpr float EffectHoldTo = 10.f;       // everybody holds still to here, then it is judged
	constexpr float EffectSetupAt = 5.f;       // host: another platform (two rows above the slide's), the two penguins on it
	constexpr int32 EffectPlatformRowsUp = 6;
	constexpr float EffectPlatformLife = 5.5f;
	constexpr float EffectSlapGap = 110.f;     // round 1: in slap reach
	constexpr float EffectIceGap = 300.f;      // round 2
	constexpr float EffectSlapAt = 5.8f;
	constexpr float EffectFreezeAAt = 5.5f;
	constexpr float EffectSnowballAt = 6.1f;
	constexpr float EffectFreezeBAt = 6.9f;
	constexpr float EffectGustAt = 7.3f;
	// ... then the fish hook (APinguNetTest::UpdateHookCheck): round 1 at an ice block on the platform's left end (3 blocks
	// left of the host), round 2 at the client's penguin, frozen once more.
	constexpr float EffectParkAt = 7.35f;      // (round 1: the client's penguin put back 3 m to the right first - out of the hook's way)
	constexpr float EffectParkGap = 300.f;
	constexpr float EffectHookBlockAt = 7.6f;
	constexpr float EffectFreezeCAt = 8.f;
	constexpr float EffectYankAt = 8.25f;
	// ... and round 1 after the hook: an ordinary hit (no slap) sends the client's penguin flying, and it throws a snowball in
	// the flight - abilities work in a knockback (APinguNetTest::UpdateEffectCheck). The host gives it the snowball first.
	constexpr float EffectGiftAt = 8.4f;
	constexpr float EffectKnockAt = 8.7f;
	constexpr float EffectKnockSide = 450.f;   // away from the host (cm/s, before the fight's knockback growth)
	constexpr float EffectKnockUp = 650.f;
	constexpr float EffectKnockStun = 0.5f;
	constexpr float EffectKnockPressAfter = 0.12f; // the client presses this long into the flight
	// ... and round 1 holds on for the ground pad (APinguNetTest::UpdatePadCheck): an icicle stuck in a fresh platform.
	// round 2, last: the host grows the bot to 8x on a platform above the island (this machine must see it at 8x), the client's
	// penguin 7 m from it; one more grow step pops it (knocked out Overgrown: puffed up, then gone) and its blast pushes the
	// client's penguin. The bot's knock-out ends the round (its team is out).
	constexpr float SizeHoldTo = 13.f;
	constexpr float SizeSetupAt = 10.1f;
	constexpr float SizeWatchAt = 11.4f;
	constexpr float SizePopAt = 11.5f;
	constexpr float SizeJudgeAfter = 1.2f;     // world time after the knock-out was first seen here
	constexpr float SizeGiveUpAfter = 6.f;     // no knock-out seen this long after the sample: judged anyway
	constexpr int32 SizePlatformRowsUp = 3;
	constexpr int32 SizePlatformColumns = 24;
	constexpr float SizePlatformLife = 3.5f;
	constexpr float SizeClientGap = 700.f;     // from the giant's middle to the client's penguin

	constexpr float PadHoldTo = 12.5f;
	constexpr float PadSetupAt = 10.1f;        // host: the platform, the client's penguin on it, the icicle stuck in it
	constexpr float PadWatchAt = 10.5f;
	constexpr float PadWalkAt = 10.7f;         // the client's penguin walks into the icicle
	constexpr int32 PadPlatformRowsUp = 9;
	constexpr int32 PadPlatformColumns = 20;
	constexpr float PadPlatformLife = 3.5f;
	constexpr float PadGap = 220.f;            // from the client's penguin to the icicle
	constexpr float PadMinRise = 600.f;        // the pad throws about 985 cm up; a jump is 290

	// ... and round 1 holds on once more for the icicle's charged throw (APinguNetTest::UpdateThrowCheck): on a platform high
	// above, both throwing to the right - the host a full second's charge (fast, perfectly straight: 2600 cm/s) and a tap (a weak
	// lob that drops after about 2 m), then the client half a second's on its own machine (in between: about 8 m), the host's
	// penguin behind it by then. The host stands 10 m right of the client, so its tap's pad lies beyond where the client's
	// icicle comes down. While they charge they aim left first (the host for 0.6 s, the client for its whole hold) and let go
	// aiming right: both machines check that each charging penguin faces its aim (see ThrowFaceFlipAfter).
	constexpr float ThrowHoldTo = 18.8f;
	constexpr float ThrowSetupAt = 12.6f;
	constexpr int32 ThrowPlatformRowsUp = 13;  // above the pad's
	constexpr int32 ThrowPlatformColumns = 24;
	constexpr float ThrowPlatformLife = 6.6f;
	constexpr int32 ThrowClientColumn = 4;     // from the platform's left end
	constexpr int32 ThrowHostColumn = 14;
	constexpr int32 ThrowHostAsideColumn = 1;  // the host's penguin out of the client's way (behind it)
	constexpr float ThrowLongPressAt = 13.f;
	constexpr float ThrowLongHold = 1.6f;      // a full charge (UPenguinAbilityComponent::IcicleFullChargeTime: 1.5 s since 2026-11-05)
	constexpr float ThrowTapAt = 15.f;
	constexpr float ThrowAsideAt = 15.9f;
	constexpr float ThrowClientPressAt = 16.2f;
	constexpr float ThrowClientHold = 0.5f;
	constexpr float ThrowJudgeAt = 18.6f;
	constexpr float ThrowSeenFrom = 0.25f;     // the charge shows on the other machine from this long after the press ...
	constexpr float ThrowSeenTo = 0.45f;       // ... to this long after it (the client's: half a second, the release follows)
	constexpr float ThrowSeenToLong = 0.9f;    // the host's charge lasts 1.1 s: the client has until this long after its press to see it
	constexpr float ThrowMinLong = 3500.f;     // a full charge flies its 41 m (1.6 s at 2600 cm/s); a tap drops after about 2 m
	constexpr float ThrowMinTap = 120.f;
	constexpr float ThrowMaxTap = 400.f;
	// The penguins face their aim while they charge (UPenguinAbilityComponent::GetIcicleFacing). The host's penguin (it stands facing right)
	// aims left until ThrowFaceFlipAfter into its charge, then right; the client's aims left for its whole hold (it lets go aiming right).
	// Windows (s after the press) in which the mesh must look left / right on the machine that shows it - clear of the turns (a turn
	// takes 0.05 s; the aim reaches the other machine within about 0.1 s).
	constexpr float ThrowFaceFlipAfter = 0.6f;
	constexpr float ThrowFaceHostLeft[2] = { 0.3f, 0.55f };
	constexpr float ThrowFaceHostRight[2] = { 0.85f, 1.05f };
	constexpr float ThrowFaceClientLeft[2] = { 0.2f, 0.42f };
	constexpr int32 ThrowFaceMinFrames = 3;    // frames a window must have had at least ...
	constexpr int32 ThrowFaceMaxWrong = 2;     // ... and at most this many of them looking the wrong way (a hitch)
	constexpr float ThrowMinMedium = 450.f;    // half a second's charge: about 8 m (a hold of 0.4 s, the client's clock being a little off, still makes 450 cm)
	constexpr float ThrowMaxMedium = 1300.f;
	constexpr float ThrowLongOverTap = 5.f;    // the long one at least this many times as far as the tap

	enum class ERole : uint8
	{
		None,
		Host,
		Client,
	};

	/** What the test knows across the map loads of hosting / joining / leaving (one test per process). */
	struct FProgress
	{
		bool bInitialized = false;
		ERole Role = ERole::None;
		FString Address;
		double StartTime = 0.0;
		// host
		bool bHostRequested = false;
		bool bClientJoined = false;
		int32 MaxPlayers = 0;
		bool bMatchStarted = false;
		// client
		int32 JoinAttempts = 0;
		bool bConnected = false;
		int32 ConnectedAttempt = 0;
		FString LastJoinError;
		/** LAN games heard while searching in the solo world (the host's beacon). */
		TArray<FString> FoundGames;
		// both
		bool bMatchOver = false;
		int32 MatchWinnerSlot = -1;
		FString MatchWinnerName;
		int32 RoundsFought = 0;
		int32 BlockCountsLogged = 0;
		int32 MaxPenguins = 0;
		int32 FeedLines = 0;
		int32 ProjectilesSeen = 0;
		int32 PickupsSeen = 0;
		float OwnDistance = 0.f;
		int32 OwnAbilityUses = 0;
		// the belly slide over LAN (UpdateSlideCheck): 0 = round 1, the client slides into the host; 1 = round 2, the other way
		bool bSlideJudged[2] = { false, false };
		bool bSlideOk[2] = { false, false };
		FString SlideDetail[2];
		// the stun / ice check (UpdateEffectCheck): 0 = round 1, the slap on the client; 1 = round 2, the ice on the client
		bool bEffectJudged[2] = { false, false };
		bool bEffectOk[2] = { false, false };
		FString EffectDetail[2];
		// round 1, judged with the slap: the client's penguin uses an ability in a knockback flight
		bool bKnockJudged = false;
		bool bKnockOk = false;
		FString KnockDetail;
		// the hook check (UpdateHookCheck): 0 = round 1, an ice block of the platform; 1 = round 2, the client's ice cube
		bool bHookJudged[2] = { false, false };
		bool bHookOk[2] = { false, false };
		FString HookDetail[2];
		bool bPadJudged = false;
		bool bPadOk = false;
		FString PadDetail;
		// round 1, after the pad: the icicle's charged throw (UpdateThrowCheck)
		bool bThrowJudged = false;
		bool bThrowOk = false;
		FString ThrowDetail;
		// round 2, last: the giant that pops (UpdateSizeCheck / UpdateSizeWatch)
		bool bSizeJudged = false;
		bool bSizeOk = false;
		FString SizeDetail;
		// client: the others' knock-outs and their death looks here (WatchDeathLooks)
		int32 RemoteKOs = 0;
		int32 RemoteKOsLookOn = 0;
		int32 RemoteKOsShaped = 0;
		FString DeathDetail;
		bool bResultsLogged = false;
		// The match is endless; only the host can end it (END MATCH).
		bool bClientSawEndless = false;      // client: the match replicated as endless
		bool bClientCouldEndMatch = false;   // client: its pause menu offered END MATCH during the match (it must not)
		// -PinguNetLate
		bool bLate = false;
		bool bClientJoinedMidMatch = false;  // host: the client came while the match ran
		bool bSawWaiting = false;            // client: our player state said "waiting for the next round"
		bool bSawWaitingFeed = false;        // client: the feed said someone "joined - plays from the next round" (the host
		                                     // admitted us between two rounds, too quickly for our player state to show it)
		int32 OwnPenguinRound = -1;          // client: the round our penguin first showed up in
		int32 OwnTeam = -1;                  // client: our penguin's team
		int32 TeamsInMatch = 0;              // the match's teams (as replicated)
		// both: the teams as this machine shows them in a fight with all three penguins (the client's came): a team of
		// two (host + client, a ring each) and the bot alone in the other one - on its own, no team shown
		bool bTeamViewSeen = false;
		bool bTeamViewOk = false;
		FString TeamViewDetail;
	};

	FProgress& Progress()
	{
		static FProgress Instance;
		return Instance;
	}

	ERole GetRole()
	{
		const TCHAR* CommandLine = FCommandLine::Get();
		FString Role;
		if (FParse::Value(CommandLine, TEXT("PinguNetTest="), Role))
		{
			return Role.Equals(TEXT("client"), ESearchCase::IgnoreCase) ? ERole::Client : ERole::Host;
		}
		if (FParse::Param(CommandLine, TEXT("PinguNetTest")))
		{
			FString Address;
			return FParse::Value(CommandLine, TEXT("PinguNetAddr="), Address) ? ERole::Client : ERole::Host;
		}
		return ERole::None;
	}

	FString GetJoinAddress()
	{
		FString Address;
		if (!FParse::Value(FCommandLine::Get(), TEXT("PinguNetAddr="), Address) || Address.IsEmpty())
		{
			Address = TEXT("127.0.0.1");
		}
		return Address;
	}

	const TCHAR* PhaseName(EPinguPhase Phase)
	{
		switch (Phase)
		{
		case EPinguPhase::Menu: return TEXT("Menu");
		case EPinguPhase::Lobby: return TEXT("Lobby");
		case EPinguPhase::Intro: return TEXT("Intro");
		case EPinguPhase::Countdown: return TEXT("Countdown");
		case EPinguPhase::Fight: return TEXT("Fight");
		case EPinguPhase::RoundOver: return TEXT("RoundOver");
		case EPinguPhase::MatchOver: return TEXT("MatchOver");
		case EPinguPhase::Practice: return TEXT("Practice");
		default: return TEXT("?");
		}
	}

	bool IsMatchPhase(EPinguPhase Phase)
	{
		return Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown || Phase == EPinguPhase::Fight
			|| Phase == EPinguPhase::RoundOver || Phase == EPinguPhase::MatchOver;
	}

	const TCHAR* NetModeName(ENetMode Mode)
	{
		switch (Mode)
		{
		case NM_Standalone: return TEXT("standalone");
		case NM_DedicatedServer: return TEXT("dedicated server");
		case NM_ListenServer: return TEXT("listen server");
		case NM_Client: return TEXT("client");
		default: return TEXT("?");
		}
	}

	const TCHAR* NetStateName(EPinguNetState State)
	{
		switch (State)
		{
		case EPinguNetState::Offline: return TEXT("Offline");
		case EPinguNetState::Hosting: return TEXT("Hosting");
		case EPinguNetState::Connecting: return TEXT("Connecting");
		case EPinguNetState::Connected: return TEXT("Connected");
		default: return TEXT("?");
		}
	}

	/** The scoreboard in replicated order: the same text on both machines. */
	FString DescribeScores(const APenguinGameState* GameState)
	{
		FString Out;
		for (const FPinguScore& Score : GameState->GetScores())
		{
			Out += FString::Printf(TEXT("%s[slot %d \"%s\" human %d: %d wins, %d KOs]"), Out.IsEmpty() ? TEXT("") : TEXT(" "), Score.Slot,
				*Score.Name, Score.bHuman ? 1 : 0, Score.Wins, Score.KOs);
		}
		return Out.IsEmpty() ? FString(TEXT("(none)")) : Out;
	}

	/** One penguin in the "penguins N: [...]" line. */
	struct FPenguinLine
	{
		int32 Slot = 0;
		FString Text;
	};

	/** Human players in the game (host + clients): the game state's player list, bots never counted. */
	int32 CountPlayers(const APenguinGameState* GameState)
	{
		if (!GameState)
		{
			return 0;
		}
		int32 Humans = 0;
		for (const APlayerState* PlayerState : GameState->PlayerArray)
		{
			if (PlayerState && !PlayerState->IsABot())
			{
				++Humans;
			}
		}
		return FMath::Max(Humans, GameState->GetPenguinPlayers().Num());
	}
}

APinguNetTest::APinguNetTest()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	bReplicates = false;  // every machine runs its own
}

bool APinguNetTest::IsRequested()
{
	return PinguNetTestPrivate::GetRole() != PinguNetTestPrivate::ERole::None;
}

void APinguNetTest::Report(const FString& Message) const
{
	UE_LOG(LogPingu, Display, TEXT("[NETTEST] %s"), *Message);
}

void APinguNetTest::BeginPlay()
{
	Super::BeginPlay();
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	if (!P.bInitialized)
	{
		P.bInitialized = true;
		P.Role = GetRole();
		P.Address = GetJoinAddress();
		P.StartTime = FPlatformTime::Seconds();
		P.bLate = FParse::Param(FCommandLine::Get(), TEXT("PinguNetLate"));
		Report(P.Role == ERole::Client ? FString::Printf(TEXT("start: client, host address %s"), *P.Address) : FString(TEXT("start: host")));
	}
	bHostRole = P.Role != ERole::Client;
	const UPinguNetSubsystem* Net = UPinguNetSubsystem::Get(this);
	Report(FString::Printf(TEXT("world %s: %s, session %s"), *GetWorld()->GetMapName(), NetModeName(GetNetMode()),
		Net ? NetStateName(Net->GetState()) : TEXT("none")));
}

void APinguNetTest::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	using namespace PinguNetTestPrivate;
	if (bQuitRequested)
	{
		return;
	}
	// Real time: the round-end slow motion of solo games must not stretch the test's waits.
	WorldTime += FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.25f);
	PinguNetTestKillCam::Tick(GetWorld(), bHostRole); // (the kill cam check: PinguNetTestKillCam.cpp)

	// Frame time, every 3 s (2026-10-27: the host stalled ~1.5 s a frame after the join): the slowest frame and where the time
	// went - game thread, render thread, GPU (ms of the last frame).
	{
		static double PerfFrom = 0.0;
		static float PerfWorst = 0.f;
		static int32 PerfFrames = 0;
		PerfWorst = FMath::Max(PerfWorst, static_cast<float>(FApp::GetDeltaTime()));
		++PerfFrames;
		const double RealNow = FPlatformTime::Seconds();
		if (RealNow - PerfFrom > 3.0)
		{
			UE_LOG(LogPingu, Display, TEXT("[NETPERF] %s %d frames in %.1f s (worst %.0f ms); last: game %.1f ms, render %.1f ms, gpu %.1f ms"),
				bHostRole ? TEXT("host") : TEXT("client"), PerfFrames, PerfFrom > 0.0 ? RealNow - PerfFrom : 0.0, PerfWorst * 1000.f,
				FPlatformTime::ToMilliseconds(GGameThreadTime), FPlatformTime::ToMilliseconds(GRenderThreadTime), FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)));
			PerfFrom = RealNow;
			PerfWorst = 0.f;
			PerfFrames = 0;
		}
	}

	// The client leaves the host its share of the GPU (ClientMaxFPS). Checked every tick: applying the graphics settings
	// (each new world) puts the player's own cap back.
	if (!bHostRole && GEngine && !FMath::IsNearlyEqual(GEngine->GetMaxFPS(), ClientMaxFPS))
	{
		Report(FString::Printf(TEXT("client frame cap: %.0f fps (was %.0f) - the host shares this PC's GPU"), ClientMaxFPS, GEngine->GetMaxFPS()));
		GEngine->SetMaxFPS(ClientMaxFPS);
	}

	const FProgress& P = Progress();
	const double Limit = P.bLate || IsTraitsRun() || IsCheatRun() ? LateTimeLimit : (bHostRole ? HostTimeLimit : ClientTimeLimit);
	if (FPlatformTime::Seconds() - P.StartTime > Limit)
	{
		Finish(FString::Printf(TEXT("time limit of %.0f s"), Limit));
		return;
	}
	UpdateHostAuthority(FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.25f)); // (PinguNetTestHostAuthority.cpp)
	switch (GetNetMode())
	{
	case NM_ListenServer:
		TickHost();
		break;
	case NM_Client:
		TickClient();
		break;
	default:
		TickSolo();
		break;
	}
}

void APinguNetTest::TickSolo()
{
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	if (P.bResultsLogged)
	{
		Finish(TEXT("done"));  // back in a solo world after the game
		return;
	}
	UPinguNetSubsystem* Net = UPinguNetSubsystem::Get(this);
	if (!Net)
	{
		Finish(TEXT("no net subsystem"));
		return;
	}

	if (bHostRole)
	{
		if (!P.bHostRequested)
		{
			if (WorldTime >= StartDelay)
			{
				Report(TEXT("host game"));
				P.bHostRequested = true;
				if (!Net->HostGame())
				{
					Finish(TEXT("HostGame refused"));
				}
			}
		}
		else if (Net->GetState() == EPinguNetState::Offline)
		{
			// We hosted, and a solo world came back before the match was over: hosting failed.
			Report(FString::Printf(TEXT("back in a solo world: \"%s\""), *Net->GetLastError().ToString()));
			Finish(TEXT("hosting failed"));
		}
		return;
	}

	if (P.bConnected && RejoinAfterKick(Net->GetLastError().ToString()))
	{
		// Kicked by the host: nothing is held against us, so join again at once (the join code below takes it from here).
		Report(TEXT("kicked by the host: joining again"));
		P.bConnected = false;
		P.JoinAttempts = 0;
	}
	if (P.bConnected)
	{
		// The host left (or we lost it): the subsystem brought us back to the solo menu.
		Report(FString::Printf(TEXT("back in a solo world: \"%s\""), *Net->GetLastError().ToString()));
		Finish(TEXT("the host left"));
		return;
	}

	// The LAN finder runs like on the LAN screen: the host's beacon must show up (we still join the given address).
	if (!bSearchStarted)
	{
		bSearchStarted = true;
		Net->StartSearch();
	}
	for (const FPinguLanGame& Game : Net->GetFoundGames())
	{
		if (!P.FoundGames.Contains(Game.Address))
		{
			P.FoundGames.Add(Game.Address);
			Report(FString::Printf(TEXT("found game \"%s\" at %s (%d/%d players%s)"), *Game.HostName, *Game.Address, Game.Players,
				Game.MaxPlayers, Game.bInMatch ? TEXT(", in a match") : TEXT("")));
		}
	}
	if (P.JoinAttempts == 0 && P.FoundGames.Num() == 0 && WorldTime < StartDelay + BeaconWait)
	{
		return;  // the host is not up yet (or its beacon does not reach us: then we join after BeaconWait anyway)
	}

	if (Net->GetState() == EPinguNetState::Connecting)
	{
		bJoinUnderWay = true;
		return;
	}
	if (bJoinUnderWay)
	{
		// The attempt ended in this world (no answer): wait a little before the next one.
		bJoinUnderWay = false;
		JoinWaitUntil = WorldTime + JoinRetryDelay;
	}
	if (WorldTime < FMath::Max(StartDelay, JoinWaitUntil))
	{
		return;
	}
	const FString Error = Net->GetLastError().ToString();
	if (P.JoinAttempts > 0 && !Error.IsEmpty())
	{
		P.LastJoinError = Error;
		NoteJoinFailure(Error);
		Report(FString::Printf(TEXT("join attempt %d failed: \"%s\""), P.JoinAttempts, *Error));
	}
	if (P.JoinAttempts >= MaxJoinAttempts)
	{
		Finish(FString::Printf(TEXT("could not join after %d attempts"), P.JoinAttempts));
		return;
	}
	++P.JoinAttempts;
	Report(FString::Printf(TEXT("join attempt %d: %s"), P.JoinAttempts, *P.Address));
	if (!Net->JoinGame(P.Address))
	{
		Finish(FString::Printf(TEXT("JoinGame refused \"%s\": %s"), *P.Address, *Net->GetLastError().ToString()));
	}
}

void APinguNetTest::TickHost()
{
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	UPinguNetSubsystem* Net = UPinguNetSubsystem::Get(this);
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (!bWorldLogged && Net)
	{
		bWorldLogged = true;
		Report(FString::Printf(TEXT("hosting: port %d, LAN address %s"), Net->GetGamePort(), *Net->GetLocalAddress()));
	}
	if (P.bResultsLogged)
	{
		// We left after the results; quit here if no solo world came.
		if (LeaveTime >= 0.f && WorldTime - LeaveTime > QuitAfterLeave)
		{
			Finish(TEXT("done (the leave did not travel)"));
		}
		return;
	}
	UpdateHotJoinSwitch();
	UpdatePicksRun(); // -PinguNetCheat=choose: the joiner's picks each round (PinguNetTestPicks.cpp)
	if (UpdateTraitsCheck() || UpdateSlideCheck()) // (-PinguNetTraits: PinguNetTestTraits.cpp)
	{
		APinguAntiCheat::DevQuietMotion(GetWorld(), 1.5f); // our script moves penguins about: the client's anti-cheat must not judge it (renewed every frame of the hold; short, so the free fight after it is judged)
	}
	else
	{
		DriveOwnPenguin();
	}
	UpdateSizeWatch();
	Observe();

	const int32 Players = CountPlayers(GameState);
	if (Players > P.MaxPlayers)
	{
		P.MaxPlayers = Players;
	}
	if (P.bLate)
	{
		// The match first (alone with a bot, two teams), the client joins while it runs.
		if (!P.bMatchStarted && WorldTime >= LateMatchStartDelay)
		{
			APenguinGameMode* GameMode = GetWorld()->GetAuthGameMode<APenguinGameMode>();
			if (!GameMode)
			{
				Finish(TEXT("no game mode on the host"));
				return;
			}
			FPinguMatchSettings Settings;
			Settings.NumBots = 1;
			Settings.Difficulty = 1;
			Settings.RoundsToWin = 3;
			Settings.NumTeams = 2;
			PrepareTraitsMatch(GameMode, Settings);
			Report(TEXT("start match before anybody joined: 1 bot, 2 teams, first to 3 wins"));
			P.bMatchStarted = true;
			GameMode->StartMatch(Settings);
		}
		if (!P.bClientJoined && Players >= 2)
		{
			P.bClientJoined = true;
			P.bClientJoinedMidMatch = GameState && IsMatchPhase(GameState->GetPhase());
			ClientJoinedTime = WorldTime;
			Report(FString::Printf(TEXT("client joined: %d players, during the match %d"), Players, P.bClientJoinedMidMatch ? 1 : 0));
		}
		else if (!P.bClientJoined && WorldTime > ClientWaitTimeout + 60.f)
		{
			Finish(FString::Printf(TEXT("no client joined within %.0f s"), ClientWaitTimeout + 60.f));
			return;
		}
	}
	else if (!P.bClientJoined)
	{
		if (Players >= 2)
		{
			P.bClientJoined = true;
			ClientJoinedTime = WorldTime;
			Report(FString::Printf(TEXT("client joined: %d players"), Players));
		}
		else if (WorldTime > ClientWaitTimeout)
		{
			Finish(FString::Printf(TEXT("no client joined within %.0f s"), ClientWaitTimeout));
		}
		return;
	}
	if (!P.bMatchStarted && !P.bLate)
	{
		// The client needs its lobby for the Snow Warp check (its map load's first frames can take 1 to 5 s): wait for the warp.
		if (WorldTime - ClientJoinedTime >= MatchStartDelay && (IsLobbyWarpSettled() || WorldTime - ClientJoinedTime >= LobbyWarpWait))
		{
			APenguinGameMode* GameMode = GetWorld()->GetAuthGameMode<APenguinGameMode>();
			if (!GameMode)
			{
				Finish(TEXT("no game mode on the host"));
				return;
			}
			// Endless, as every match from the lobby: the host ends it with END MATCH once MinRounds rounds are over.
			FPinguMatchSettings Settings;
			Settings.NumBots = 1;
			Settings.Difficulty = 1;
			Settings.bEndless = true;
			PrepareTraitsMatch(GameMode, Settings);
			PreparePicksMatch(Settings); // (ABILITIES as the lobby has it: -PinguChoose)
			Report(FString::Printf(TEXT("start match: 1 bot, difficulty 1, endless (END MATCH after %d rounds)"), MinRounds));
			P.bMatchStarted = true;
			GameMode->StartMatch(Settings);
		}
		return;
	}
	if (!P.bLate && !bEndMatchSent && GameState && GameState->GetPhase() == EPinguPhase::RoundOver && GameState->GetRoundNumber() >= MinRounds
		&& GameState->GetPhaseTime() > 1.5f)
	{
		APenguinGameMode* GameMode = GetWorld()->GetAuthGameMode<APenguinGameMode>();
		bEndMatchSent = true;
		const bool bEnded = GameMode && GameMode->EndMatchNow();
		Report(FString::Printf(TEXT("END MATCH after round %d: %s"), GameState->GetRoundNumber(), bEnded ? TEXT("ended") : TEXT("refused")));
	}
	if (P.bMatchOver && MatchOverTime >= 0.f && WorldTime - MatchOverTime >= QuitAfterMatchOver)
	{
		LogResults(TEXT("match over"));
		Report(TEXT("leave: the client should drop back to its menu"));
		LeaveTime = WorldTime;
		if (Net)
		{
			Net->LeaveGame();
		}
		else
		{
			Finish(TEXT("done"));
		}
	}
}

void APinguNetTest::TickClient()
{
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	if (!P.bConnected)
	{
		P.bConnected = true;
		P.ConnectedAttempt = P.JoinAttempts;
		NoteConnected();
		Report(FString::Printf(TEXT("connected to %s (attempt %d)"), *P.Address, P.JoinAttempts));
	}
	if (!UpdateTraitsCheck() && !UpdateSlideCheck()) // (-PinguNetTraits: PinguNetTestTraits.cpp)
	{
		DriveOwnPenguin();
	}
	UpdateSizeWatch();
	Observe();
	WatchDeathLooks();
	UpdatePicksRun(); // -PinguNetCheat=choose: our picks in the lobby, our loadout each round (PinguNetTestPicks.cpp)
}

void APinguNetTest::WatchDeathLooks()
{
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	const APenguinCharacter* Own = GetOwnPenguin();
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Other = *It;
		if (Other == Own)
		{
			continue;
		}
		FDeathWatch* Watch = DeathWatches.FindByPredicate([Other](const FDeathWatch& Entry) { return Entry.Penguin.Get() == Other; });
		if (Other->IsKnockedOut() && !Watch)
		{
			FDeathWatch& NewWatch = DeathWatches.AddDefaulted_GetRef();
			NewWatch.Penguin = Other;
			NewWatch.SeenAt = WorldTime;
			Watch = &NewWatch;
		}
		if (!Watch)
		{
			continue;
		}
		const EPenguinKOReason Reason = Other->GetKOReason();
		if (Other->IsKnockedOut())
		{
			if (Reason != EPenguinKOReason::None)
			{
				Watch->Reason = static_cast<uint8>(Reason);
				Watch->bReasonSeen = true;
			}
			Watch->bLookOn |= Other->IsDeathLookActive();
			const bool bShown = !Other->IsHidden();
			Watch->bWasShown |= bShown;
			// What the reason looks like on this machine (sampled while it is still there; the sea hides it at once).
			const USkeletalMeshComponent* Mesh = Other->GetMesh();
			const float BaseZ = static_cast<float>(Other->GetBaseMeshScale().Z);
			const float Flat = Mesh && BaseZ > 0.f ? static_cast<float>(Mesh->GetRelativeScale3D().Z) / BaseZ : 1.f;
			switch (Reason)
			{
			case EPenguinKOReason::Stomp:
			case EPenguinKOReason::Iceberg:
			case EPenguinKOReason::Crushed:
				Watch->bShapeSeen |= bShown && Other->IsDeathLookActive() && Flat < 0.5f;
				break;
			case EPenguinKOReason::Icicle:
				Watch->bShapeSeen |= bShown && Other->IsDeathIceShown();
				break;
			case EPenguinKOReason::Bomb:
				Watch->bShapeSeen |= Other->IsDeathSootShown();
				break;
			case EPenguinKOReason::Sea:
				Watch->bShapeSeen |= !bShown && Other->IsDeathLookActive();
				break;
			case EPenguinKOReason::Shrunk:
				Watch->bShapeSeen |= bShown && Other->IsDeathLookActive() && Flat < 0.7f; // shrinking away
				break;
			case EPenguinKOReason::Overgrown:
				Watch->bShapeSeen |= bShown && Other->IsDeathLookActive() && Flat > 1.1f; // puffing up
				break;
			default:
				Watch->bShapeSeen |= Other->IsDeathLookActive(); // the tumble: the look only takes over the ghost
				break;
			}
		}
		// Judged a moment after it was first seen out (or once it is back in): one knock-out each. The entry stays until the
		// penguin is back in (a new round), so the same knock-out is not counted again.
		if (Watch->bJudged)
		{
			if (!Other->IsKnockedOut())
			{
				DeathWatches.RemoveAt(static_cast<int32>(Watch - DeathWatches.GetData()));
			}
			continue;
		}
		// (3.6 s of real time: the round-winning knock-out plays in the kill cam's slow motion, up to 3.4 s for its death look.)
		if (WorldTime - Watch->SeenAt > 3.6f || !Other->IsKnockedOut())
		{
			Watch->bJudged = true;
			++P.RemoteKOs;
			P.RemoteKOsLookOn += Watch->bLookOn && Watch->bReasonSeen ? 1 : 0;
			// (A knock-out first seen when the penguin was already gone shows no shape: only its look counts then.)
			const bool bShapeExpected = Watch->bWasShown || static_cast<EPenguinKOReason>(Watch->Reason) == EPenguinKOReason::Sea;
			P.RemoteKOsShaped += Watch->bShapeSeen || !bShapeExpected ? 1 : 0;
			const FString Line = FString::Printf(TEXT("%s %s: look %d, shape %d"), *Other->GetDisplayName(),
				*UEnum::GetValueAsString(static_cast<EPenguinKOReason>(Watch->Reason)), Watch->bLookOn ? 1 : 0, Watch->bShapeSeen ? 1 : 0);
			Report(FString::Printf(TEXT("death look seen: %s"), *Line));
			if (P.DeathDetail.Len() < 400)
			{
				P.DeathDetail += (P.DeathDetail.IsEmpty() ? TEXT("") : TEXT("; ")) + Line;
			}
			if (!Other->IsKnockedOut())
			{
				DeathWatches.RemoveAt(static_cast<int32>(Watch - DeathWatches.GetData()));
			}
		}
	}
}

// ---------------------------------------------------------------------------- what both machines see

void APinguNetTest::Observe()
{
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	UWorld* World = GetWorld();
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	UpdateIslandWatch(); // the island's size, the camera, Snow Warp on screen (PinguNetTestIsland.cpp)
	UpdateLobbyIslandWatch(); // the lobby island: the other player's penguin, its puff, the name tags (PinguNetTestLobby.cpp)
	UpdateIceScaleWatch(); // -PinguNetLate: a big block in the island the late joiner gets (PinguNetTestIceScale.cpp)

	if (GameState)
	{
		if (IsMatchPhase(GameState->GetPhase()))
		{
			P.TeamsInMatch = FMath::Max(P.TeamsInMatch, GameState->GetSettings().NumTeams);
		}
		// Late join: our player waits for the next round (until our penguin comes).
		if (const APenguinPlayerState* Local = GameState->GetLocalPenguinPlayer())
		{
			if (Local->IsWaitingForRound() && !P.bSawWaiting)
			{
				P.bSawWaiting = true;
				Report(FString::Printf(TEXT("waiting for the next round (round %d, phase %s)"), GameState->GetRoundNumber(), PhaseName(GameState->GetPhase())));
			}
		}
		// Client: an endless match (as the host started it), and no END MATCH in its pause menu.
		if (!bHostRole && !P.bLate)
		{
			if (IsMatchPhase(GameState->GetPhase()) && GameState->IsEndless())
			{
				P.bClientSawEndless = true;
			}
			const APlayerController* LocalController = World ? World->GetFirstPlayerController() : nullptr;
			const APenguinHUD* LocalHUD = LocalController ? Cast<APenguinHUD>(LocalController->GetHUD()) : nullptr;
			if (LocalHUD && LocalHUD->CanEndMatch())
			{
				P.bClientCouldEndMatch = true;
			}
		}
		// Late join: the teams as this machine shows them (from the replicated score rows: the same on both machines) - the
		// host and the client together (a ring each), the bot alone in its team: on its own (no ring, no team name, its own
		// row). Judged on the last look in a fight with all three penguins.
		if (P.bLate && GameState->GetPhase() == EPinguPhase::Fight && GameState->IsTeamMatch() && GameState->GetScores().Num() == ExpectedPenguins)
		{
			const APlayerController* ViewController = World ? World->GetFirstPlayerController() : nullptr;
			const APenguinHUD* ViewHUD = ViewController ? Cast<APenguinHUD>(ViewController->GetHUD()) : nullptr;
			int32 HumansInTeam = 0;
			int32 BotsAlone = 0;
			int32 IconRings = 0;
			FString Rows;
			for (const FPinguScore& Row : GameState->GetScores())
			{
				const int32 Shown = GameState->GetShownTeam(Row.Team);
				const bool bOwnRow = ViewHUD && ViewHUD->GetLoneTeamMember(Row.Team) == &Row;
				HumansInTeam += Row.bHuman && Shown >= 0 ? 1 : 0;
				BotsAlone += !Row.bHuman && Shown < 0 && Row.Team >= 0 && bOwnRow ? 1 : 0;
				IconRings += ViewHUD && Row.Penguin && ViewHUD->GetPenguinTeamRing(Row.Penguin) >= 0 ? 1 : 0;
				Rows += FString::Printf(TEXT("%s%s team %d shown %d"), Rows.IsEmpty() ? TEXT("") : TEXT(", "), *Row.Name, Row.Team, Shown);
			}
			P.bTeamViewSeen = true;
			P.bTeamViewOk = HumansInTeam == 2 && BotsAlone == 1 && IconRings == 2;
			P.TeamViewDetail = FString::Printf(TEXT("%d people in a shown team (want 2), %d bot on its own (want 1), %d penguins with an icon ring (want 2): %s"),
				HumansInTeam, BotsAlone, IconRings, *Rows);
		}
		const uint8 Phase = static_cast<uint8>(GameState->GetPhase());
		const int32 Round = GameState->GetRoundNumber();
		if (!bPhaseSeen || Phase != LastPhase || Round != LastRound)
		{
			bPhaseSeen = true;
			LastPhase = Phase;
			LastRound = Round;
			OnPhase(GameState);
		}

		// New kill-feed lines: the feed is newest last and old lines drop off the front, so what is new is what follows the
		// longest end of the last feed that the current one starts with (no timestamps needed).
		TArray<FString> Feed;
		for (const FPinguFeedEntry& Entry : GameState->GetFeed())
		{
			Feed.Add(Entry.Text);
		}
		int32 Overlap = 0;
		for (int32 Count = FMath::Min(LastFeed.Num(), Feed.Num()); Count > 0 && Overlap == 0; --Count)
		{
			bool bMatch = true;
			for (int32 Index = 0; Index < Count && bMatch; ++Index)
			{
				bMatch = LastFeed[LastFeed.Num() - Count + Index] == Feed[Index];
			}
			if (bMatch)
			{
				Overlap = Count;
			}
		}
		for (int32 Index = Overlap; Index < Feed.Num(); ++Index)
		{
			++P.FeedLines;
			Report(FString::Printf(TEXT("feed \"%s\""), *Feed[Index]));
			if (Feed[Index].Contains(TEXT("plays from the next round")))
			{
				P.bSawWaitingFeed = true;
			}
		}
		LastFeed = MoveTemp(Feed);
	}

	// Penguins (slot, name, human) in slot order: logged whenever that changes.
	TArray<FPenguinLine> Lines;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		const APenguinCharacter* Penguin = *It;
		FPenguinLine& Line = Lines.AddDefaulted_GetRef();
		Line.Slot = Penguin->GetPlayerSlot();
		Line.Text = FString::Printf(TEXT("slot %d \"%s\" human %d"), Line.Slot, *Penguin->GetDisplayName(), Penguin->GetPlayerState() ? 1 : 0);
	}
	Lines.Sort([](const FPenguinLine& A, const FPenguinLine& B) { return A.Slot != B.Slot ? A.Slot < B.Slot : A.Text < B.Text; });
	FString PenguinsText = FString::Printf(TEXT("penguins %d:"), Lines.Num());
	for (const FPenguinLine& Line : Lines)
	{
		PenguinsText += TEXT(" [") + Line.Text + TEXT("]");
	}
	if (PenguinsText != LastPenguinsText)
	{
		LastPenguinsText = PenguinsText;
		Report(PenguinsText);
	}
	if (GameState && IsMatchPhase(GameState->GetPhase()))
	{
		P.MaxPenguins = FMath::Max(P.MaxPenguins, Lines.Num());
	}

	// Projectiles and ability bubbles (counted per round; very short-lived shots may never reach a client).
	TArray<APenguinProjectile*> Projectiles;
	APenguinProjectile::GetAll(World, Projectiles);
	for (APenguinProjectile* Projectile : Projectiles)
	{
		if (Projectile && !SeenProjectiles.ContainsByPredicate([Projectile](const TWeakObjectPtr<AActor>& Seen) { return Seen.Get() == Projectile; }))
		{
			SeenProjectiles.Add(Projectile);
			++RoundProjectiles;
			++P.ProjectilesSeen;
		}
	}
	SeenProjectiles.RemoveAll([](const TWeakObjectPtr<AActor>& Seen) { return !Seen.IsValid(); });
	TArray<APinguPickup*> Pickups;
	APinguPickup::GetAll(World, Pickups);
	for (APinguPickup* Pickup : Pickups)
	{
		if (Pickup && !SeenPickups.ContainsByPredicate([Pickup](const TWeakObjectPtr<AActor>& Seen) { return Seen.Get() == Pickup; }))
		{
			SeenPickups.Add(Pickup);
			++RoundPickups;
			++P.PickupsSeen;
		}
	}
	SeenPickups.RemoveAll([](const TWeakObjectPtr<AActor>& Seen) { return !Seen.IsValid(); });

	// Our own penguin: how far it went and which abilities it used (on a client these come back from the server).
	APenguinCharacter* Own = GetOwnPenguin();
	if (Own != OwnPenguin.Get())
	{
		OwnPenguin = Own;
		LastUseCounts.Reset();
		if (Own)
		{
			LastOwnLocation = Own->GetActorLocation();
			Report(FString::Printf(TEXT("own penguin: slot %d \"%s\""), Own->GetPlayerSlot(), *Own->GetDisplayName()));
			if (P.OwnPenguinRound < 0 && GameState && IsMatchPhase(GameState->GetPhase()))
			{
				P.OwnPenguinRound = GameState->GetRoundNumber();
				Report(FString::Printf(TEXT("own penguin in the match from round %d"), P.OwnPenguinRound));
			}
		}
	}
	if (!Own)
	{
		return;
	}
	if (Own->GetTeam() >= 0)
	{
		P.OwnTeam = Own->GetTeam();
	}
	const FVector Location = Own->GetActorLocation();
	const float Step = static_cast<float>(FVector::Dist(Location, LastOwnLocation));
	LastOwnLocation = Location;
	if (Step < TeleportStep)
	{
		P.OwnDistance += Step;
		RoundDistance += Step;
	}
	if (const UPenguinAbilityComponent* Abilities = Own->GetAbilities())
	{
		for (int32 Slot = 0; Slot < 3; ++Slot)
		{
			const int32 Uses = Abilities->GetUseCount(Slot);
			if (!LastUseCounts.IsValidIndex(Slot))
			{
				LastUseCounts.Add(Uses);
				continue;
			}
			if (Uses > LastUseCounts[Slot])
			{
				const int32 NewUses = Uses - LastUseCounts[Slot];
				P.OwnAbilityUses += NewUses;
				RoundAbilities += NewUses;
				Report(FString::Printf(TEXT("own ability %s (slot %d) used, %d so far"),
					UPenguinAbilityComponent::GetInfo(Abilities->GetAbility(Slot)).Name, Slot, P.OwnAbilityUses));
			}
			LastUseCounts[Slot] = Uses;  // a new loadout starts its counters again
		}
	}
}

void APinguNetTest::OnPhase(const APenguinGameState* GameState)
{
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	const EPinguPhase Phase = GameState->GetPhase();
	const int32 Round = GameState->GetRoundNumber();
	Report(FString::Printf(TEXT("phase %s, round %d"), PhaseName(Phase), Round));

	if (Phase == EPinguPhase::Fight && Round != LastFightRound)
	{
		LastFightRound = Round;
		++P.RoundsFought;
		RoundProjectiles = 0;
		RoundPickups = 0;
		RoundAbilities = 0;
		RoundDistance = 0.f;
		// Every client must have exactly the server's island.
		const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
		if (Island)
		{
			++P.BlockCountsLogged;
		}
		Report(FString::Printf(TEXT("fight round %d: island blocks %d, cells %08x, generation %d, penguins %d"), Round,
			Island ? Island->GetNumBlocks() : -1, Island ? Island->GetCellChecksum() : 0u, Island ? Island->GetGeneration() : -1, CountPenguins()));
	}
	else if (Phase == EPinguPhase::RoundOver)
	{
		Report(FString::Printf(TEXT("round %d over: winner slot %d"), Round, GameState->GetRoundWinnerSlot()));
		Report(TEXT("scores ") + DescribeScores(GameState));
		Report(FString::Printf(TEXT("own round %d: moved %.0f cm, abilities %d, seen projectiles %d, pickups %d"), Round, RoundDistance,
			RoundAbilities, RoundProjectiles, RoundPickups));
	}
	else if (Phase == EPinguPhase::MatchOver && !P.bMatchOver)
	{
		P.bMatchOver = true;
		MatchOverTime = WorldTime;
		P.MatchWinnerSlot = GameState->GetMatchWinnerSlot();
		for (const FPinguScore& Score : GameState->GetScores())
		{
			if (Score.Slot == P.MatchWinnerSlot)
			{
				P.MatchWinnerName = Score.Name;
			}
		}
		Report(FString::Printf(TEXT("match over after %d rounds: winner slot %d \"%s\""), Round, P.MatchWinnerSlot, *P.MatchWinnerName));
		Report(TEXT("scores ") + DescribeScores(GameState));
	}
}

void APinguNetTest::DriveOwnPenguin()
{
	APenguinCharacter* Own = GetOwnPenguin();
	UPenguinBotComponent* Bot = Own ? Own->GetBot() : nullptr;
	if (Bot && !Bot->IsBotEnabled())
	{
		// StartMatch switches the human penguins' brains off; the test switches ours back on.
		Bot->SetBotEnabled(true, 1);
		++BotSwitchOns;
		if (BotSwitchOns <= 3 || BotSwitchOns % 100 == 0)
		{
			Report(FString::Printf(TEXT("own penguin: bot brain on (%d)"), BotSwitchOns));
		}
	}
}

// ---------------------------------------------------------------------------- the belly slide over LAN

bool APinguNetTest::UpdateSlideCheck()
{
	using namespace PinguNetTestPrivate;
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (!GameState || !IsMatchPhase(GameState->GetPhase()) || Progress().bLate || IsTraitsRun() || IsCheatRun())
	{
		return false;  // (-PinguNetLate: the client is not in the first round, so no slide checks; -PinguNetTraits: its own; a cheat run: none)
	}
	const int32 Round = GameState->GetRoundNumber();
	if (Round != SlideRound)
	{
		JudgeSlideCheck(SlideRound - 1, TEXT("the round ended first")); // (only if that one was still running)
		JudgeEffectCheck(SlideRound - 1, TEXT("the round ended first"));
		JudgeHookCheck(SlideRound - 1, TEXT("the round ended first"));
		if (SlideRound == 1)
		{
			JudgePadCheck(TEXT("the round ended first"));
			JudgeThrowCheck(TEXT("the round ended first"));
			JudgeTornadoCheck(TEXT("the round ended first"));
			JudgeSelfBombCheck(TEXT("the round ended first"));
		}
		if (SlideRound == 2)
		{
			JudgeIceScaleCheck(TEXT("the round ended first"));
			JudgeGiantCheck(TEXT("the round ended first"));
			JudgeSnowmanCheck(TEXT("the round ended first"));
		}
		ReleaseHold();
		ResetTornadoCheck();
		ResetSelfBombCheck();
		ResetIceScaleCheck();
		ResetGiantCheck();
		ResetSnowmanCheck();
		SlideRound = Round;
		bEffectSetUp = false;
		bEffectJudged = false;
		bEffectWatching = false;
		EffectStep = 0;
		EffectHitT = -1.f;
		EffectStunAtHit = 0.f;
		EffectStunEndT = -1.f;
		EffectMaxToward = 0.f;
		EffectMovedAgainT = -1.f;
		bEffectAbilityTried = false;
		bEffectDizzy = false;
		EffectUsesBeforeKnock = -1;
		bKnockWatching = false;
		KnockUsesAtStart = 0;
		KnockHitsAtStart = 0;
		KnockT = -1.f;
		bKnockPressed = false;
		bKnockSlotReady = false;
		KnockUseT = -1.f;
		bKnockUseStunned = false;
		bKnockUseInAir = false;
		bEffectFrozenA = false;
		EffectThawT = -1.f;
		EffectSnowballT = -1.f;
		EffectMaxSpeedOnIce = 0.f;
		bEffectFrozenB = false;
		EffectGustT = -1.f;
		bEffectStillFrozen = false;
		bEffectStillSampled = false;
		EffectSlid = 0.f;
		EffectHitsAfterIce = 0;
		bEffectAlive = true;
		bHookFired = false;
		bClientParked = false;
		bHookWatching = false;
		bFlyerSeen = false;
		BrokenAtHook = 0;
		bYankWatching = false;
		YankT = -1.f;
		YankLandT = -1.f;
		YankThawT = -1.f;
		bYankFrozenAt = false;
		bYankFrozenMid = false;
		bYankMidSampled = false;
		bYankAlive = true;
		YankSampleT[0] = YankSampleT[1] = -1.f;
		YankVzSamples.Reset();
		YankNearest = 1.e9f;
		bSlideSetUp = false;
		bSlidePressed = false;
		bSlideJudged = false;
		bSlideWatching = false;
		SlideInputUntil = -1.f;
		SlideHitDistance = -1.f;
		SlideTargetVelocity = FVector::ZeroVector;
		bOwnSlideSeen = false;
		OwnSlideStop = -1.f;
		bHoldOver = false;
		bPadSetUp = false;
		bPadWatching = false;
		bPadJudged = false;
		bPadSeen = false;
		PadBounceT = -1.f;
		bThrowSetUp = false;
		bThrowWatching = false;
		bThrowJudged = false;
		ThrowStep = 0;
		bThrowOtherChargeSeen = false;
		bThrowOwnShownAtOnce = false;
		bThrowFaceFlipped = false;
		FMemory::Memzero(ThrowFaceFrames);
		FMemory::Memzero(ThrowFaceWrong);
		ThrowWatches.Reset();
	}
	const int32 Index = Round - 1;
	if (Index < 0 || Index > 1 || bHoldOver)
	{
		return false;
	}
	if (GameState->GetPhase() != EPinguPhase::Fight)
	{
		JudgeSlideCheck(Index, TEXT("the fight ended first"));
		JudgeEffectCheck(Index, TEXT("the fight ended first"));
		JudgeHookCheck(Index, TEXT("the fight ended first"));
		if (Index == 0)
		{
			JudgePadCheck(TEXT("the fight ended first"));
			JudgeThrowCheck(TEXT("the fight ended first"));
			JudgeTornadoCheck(TEXT("the fight ended first"));
			JudgeSelfBombCheck(TEXT("the fight ended first"));
		}
		else
		{
			JudgeIceScaleCheck(TEXT("the fight ended first"));
			JudgeGiantCheck(TEXT("the fight ended first"));
			JudgeSnowmanCheck(TEXT("the fight ended first"));
		}
		ReleaseHold();
		// (Before the fight - intro, countdown - the hold is still to come.)
		bHoldOver = GameState->GetPhase() != EPinguPhase::Intro && GameState->GetPhase() != EPinguPhase::Countdown;
		return false;
	}
	const float T = GameState->GetPhaseTime();
	if (T < SlideHoldFrom)
	{
		return false;
	}
	// Round 1 holds on after the stun / hook checks for the ground pad, the icicle throws and the tornado (PinguNetTestTornado.cpp),
	// round 2 for the giant and the ice wall / big iceberg (PinguNetTestIceScale.cpp).
	if (T >= (Index == 0 ? ThrowHoldTo + TornadoCheckLength + SelfBombCheckLength : SizeHoldTo + IceScaleCheckLength + GiantCheckLength + SnowmanCheckLength))
	{
		JudgeSlideCheck(Index, TEXT("judged at the end of the hold"));
		JudgeEffectCheck(Index, TEXT("judged at the end of the hold"));
		JudgeHookCheck(Index, TEXT("judged at the end of the hold"));
		if (Index == 0)
		{
			JudgePadCheck(TEXT("judged at the end of the hold"));
			JudgeThrowCheck(TEXT("judged at the end of the hold"));
			JudgeTornadoCheck(TEXT("judged at the end of the hold"));
			JudgeSelfBombCheck(TEXT("judged at the end of the hold"));
		}
		else
		{
			JudgeIceScaleCheck(TEXT("judged at the end of the hold"));
			JudgeGiantCheck(TEXT("judged at the end of the hold"));
			JudgeSnowmanCheck(TEXT("judged at the end of the hold"));
		}
		ReleaseHold();
		bHoldOver = true;
		return false;
	}
	// After the slide's part of the hold: the stun / ice check (our stick is what it says); round 1 then the ground pad and
	// the icicle throws.
	const bool bEffectPart = T >= SlideHoldTo;
	const bool bPadPart = T >= EffectHoldTo;
	const bool bThrowPart = Index == 0 && T >= PadHoldTo;
	const bool bTornadoPart = Index == 0 && T >= ThrowHoldTo;
	const bool bSelfBombPart = Index == 0 && T >= ThrowHoldTo + TornadoCheckLength;
	const bool bIcePart = Index == 1 && T >= SizeHoldTo;
	const bool bGiantPart = Index == 1 && T >= SizeHoldTo + IceScaleCheckLength;
	const bool bSnowmanPart = Index == 1 && T >= SizeHoldTo + IceScaleCheckLength + GiantCheckLength;
	FVector2D OwnStick = T < SlideInputUntil ? FVector2D(SlideWay, 0.f) : FVector2D::ZeroVector;
	if (bSnowmanPart)
	{
		// Round 2's very last part: the snowman (PinguNetTestSnowman.cpp).
		JudgeSlideCheck(Index, TEXT("judged at the end of its hold"));
		JudgeEffectCheck(Index, TEXT("judged at the end of its hold"));
		JudgeHookCheck(Index, TEXT("judged at the end of its hold"));
		JudgeIceScaleCheck(TEXT("judged at the end of its hold"));
		JudgeGiantCheck(TEXT("judged at the end of its hold"));
		UpdateSnowmanCheck(T - SizeHoldTo - IceScaleCheckLength - GiantCheckLength);
		OwnStick = FVector2D::ZeroVector;
	}
	else if (bGiantPart)
	{
		// Round 2's very last part: the giant snowball (PinguNetTestGiant.cpp).
		JudgeSlideCheck(Index, TEXT("judged at the end of its hold"));
		JudgeEffectCheck(Index, TEXT("judged at the end of its hold"));
		JudgeHookCheck(Index, TEXT("judged at the end of its hold"));
		JudgeIceScaleCheck(TEXT("judged at the end of its hold"));
		UpdateGiantCheck(T - SizeHoldTo - IceScaleCheckLength);
		OwnStick = FVector2D::ZeroVector;
	}
	else if (bIcePart)
	{
		// Round 2, after the giant: the ice wall raised under the client's penguin and the big iceberg (the size check judges
		// itself: UpdateSizeWatch).
		JudgeSlideCheck(Index, TEXT("judged at the end of its hold"));
		JudgeEffectCheck(Index, TEXT("judged at the end of its hold"));
		JudgeHookCheck(Index, TEXT("judged at the end of its hold"));
		UpdateIceScaleCheck(T - SizeHoldTo);
		OwnStick = FVector2D::ZeroVector;
	}
	else if (bSelfBombPart)
	{
		JudgeSlideCheck(Index, TEXT("judged at the end of its hold"));
		JudgeEffectCheck(Index, TEXT("judged at the end of its hold"));
		JudgeHookCheck(Index, TEXT("judged at the end of its hold"));
		JudgePadCheck(TEXT("judged at the end of its hold"));
		JudgeThrowCheck(TEXT("judged at the end of its hold"));
		JudgeTornadoCheck(TEXT("judged at the end of its hold"));
		UpdateSelfBombCheck(T - ThrowHoldTo - TornadoCheckLength);
		OwnStick = FVector2D::ZeroVector;
	}
	else if (bTornadoPart)
	{
		JudgeSlideCheck(Index, TEXT("judged at the end of its hold"));
		JudgeEffectCheck(Index, TEXT("judged at the end of its hold"));
		JudgeHookCheck(Index, TEXT("judged at the end of its hold"));
		JudgePadCheck(TEXT("judged at the end of its hold"));
		JudgeThrowCheck(TEXT("judged at the end of its hold"));
		UpdateTornadoCheck(T - ThrowHoldTo);
		OwnStick = FVector2D::ZeroVector;
	}
	else if (bThrowPart)
	{
		JudgeSlideCheck(Index, TEXT("judged at the end of its hold"));
		JudgeEffectCheck(Index, TEXT("judged at the end of its hold"));
		JudgeHookCheck(Index, TEXT("judged at the end of its hold"));
		JudgePadCheck(TEXT("judged at the end of its hold"));
		UpdateThrowCheck(T);
		OwnStick = FVector2D::ZeroVector;
	}
	else if (bPadPart)
	{
		JudgeSlideCheck(Index, TEXT("judged at the end of its hold"));
		JudgeEffectCheck(Index, TEXT("judged at the end of its hold"));
		JudgeHookCheck(Index, TEXT("judged at the end of its hold"));
		OwnStick = Index == 0 ? UpdatePadCheck(T) : UpdateSizeCheck(T);
	}
	else if (bEffectPart)
	{
		JudgeSlideCheck(Index, TEXT("judged at the end of its hold"));
		OwnStick = UpdateEffectCheck(Index, T);
	}
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	const bool bWeSlide = (Index == 0) != bHostRole; // round 1: the client slides, round 2: the host
	APenguinCharacter* Slider = bWeSlide ? Own : Other;
	APenguinCharacter* Target = bWeSlide ? Other : Own;
	const UPenguinMovementComponent* SliderMove = Slider ? Slider->GetPenguinMovement() : nullptr;
	const UPenguinMovementComponent* TargetMove = Target ? Target->GetPenguinMovement() : nullptr;
	const UPenguinMovementComponent* OwnMove = Own ? Own->GetPenguinMovement() : nullptr;
	if (!bSlideWatching)
	{
		bSlideWatching = true;
		SlideTargetHitsAtStart = TargetMove ? TargetMove->HitEvents : 0;
		SlideOwnSlidesAtStart = OwnMove ? OwnMove->SlideEvents : 0;
		Report(FString::Printf(TEXT("slide check %d: everybody holds still, %s"), Index + 1, bWeSlide ? TEXT("we slide") : TEXT("the other player slides")));
	}

	// Everybody holds still: our own penguin (its stick only points the slide's way for a moment), on the host the bots too.
	if (Own)
	{
		if (UPenguinBotComponent* OwnBot = Own->GetBot())
		{
			if (OwnBot->IsBotEnabled())
			{
				OwnBot->SetBotEnabled(false);
			}
		}
		Own->ApplyVirtualMoveInput(OwnStick);
	}
	if (bHostRole)
	{
		for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
		{
			APenguinCharacter* Penguin = *It;
			UPenguinBotComponent* Bot = Penguin->GetBot();
			if (Penguin->IsBot() && Bot && Bot->IsBotEnabled())
			{
				Bot->SetBotEnabled(false);
				HeldBots.AddUnique(Penguin);
			}
		}
	}

	if (bEffectPart)
	{
		return true;
	}

	// Host: a platform in the air over the middle of the island (it goes away by itself), the slider and its target on it.
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (bHostRole && !bSlideSetUp && T >= SlideSetupAt && Slider && Target && Island)
	{
		bSlideSetUp = true;
		// Nothing left flying that could knock either of them off the platform: a shot or an iceberg cast in the very first
		// frame of the fight, before the hold (an iceberg once crushed a penguin right as it landed up there).
		for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		const FBox Bounds = Island->GetBounds();
		const float MidX = Bounds.IsValid ? static_cast<float>(Bounds.Min.X + Bounds.Max.X) * 0.5f : 0.f;
		const float TopZ = Bounds.IsValid ? static_cast<float>(Bounds.Max.Z) : 0.f;
		const FIntPoint Left = Island->WorldToCell(FVector(MidX - SlidePlatformColumns * 0.5f * Island->CellSize, 0.f,
			TopZ + (SlidePlatformRowsUp + 0.5f) * Island->CellSize));
		TArray<FIntPoint> Cells;
		for (int32 Column = 0; Column < SlidePlatformColumns; ++Column)
		{
			Cells.Add(FIntPoint(Left.X + Column, Left.Y));
		}
		const int32 Added = Island->AddBlocks(Cells, EBlockKind::Rock, SlidePlatformLife).Num();
		Island->FlushEdits(); // its collision now: the penguins land on it right away
		const float FloorZ = static_cast<float>(Island->CellToWorld(Left).Z) + Island->CellSize * 0.5f;
		const float StartX = static_cast<float>(Island->CellToWorld(FIntPoint(Left.X + 2, Left.Y)).X);
		const auto Place = [FloorZ](APenguinCharacter* Penguin, float X)
		{
			const float Half = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			Penguin->NetTeleport(FVector(X, 0.f, FloorZ + Half + 2.f), FVector::ZeroVector, true);
		};
		Place(Slider, StartX);
		Place(Target, StartX + SlideGap);
		SlideTargetHitsAtStart = TargetMove ? TargetMove->HitEvents : 0;
		Report(FString::Printf(TEXT("slide check %d: platform of %d blocks at z %.0f, \"%s\" slides into \"%s\" from %.0f cm away (both still in %d)"), Index + 1,
			Added, FloorZ, *Slider->GetDisplayName(), *Target->GetDisplayName(), SlideGap, Slider->IsAlive() && Target->IsAlive() ? 1 : 0));
	}

	// The slider slides at its target.
	if (bWeSlide && !bSlidePressed && T >= SlidePressAt && Own && Other && OwnMove)
	{
		bSlidePressed = true;
		const float Dx = static_cast<float>(Other->GetActorLocation().X - Own->GetActorLocation().X);
		const float Dz = static_cast<float>(Other->GetFeetLocation().Z - Own->GetFeetLocation().Z);
		const bool bInPlace = FMath::Abs(FMath::Abs(Dx) - SlideGap) < SlideGapTolerance && FMath::Abs(Dz) < 40.f && OwnMove->IsMovingOnGround()
			&& Own->IsAlive() && Other->IsAlive();
		Report(FString::Printf(TEXT("slide check %d: %s (the other penguin %.0f cm along, %.0f cm up)"), Index + 1,
			bInPlace ? TEXT("sliding") : TEXT("NOT IN PLACE - sliding anyway"), Dx, Dz));
		SlideWay = Dx >= 0.f ? 1.f : -1.f;
		SlideInputUntil = T + SlideStickTime;
		Own->FaceDirection(SlideWay);
		Own->ApplyVirtualMoveInput(FVector2D(SlideWay, 0.f));
		Own->BellySlide();
	}

	// What happened: the target hit (and how far the slide had gone then), our own slide stopping early.
	if (TargetMove && SlideHitDistance < 0.f && TargetMove->HitEvents != SlideTargetHitsAtStart)
	{
		SlideHitDistance = SliderMove ? SliderMove->GetBellySlideDistance() : 0.f;
		SlideTargetVelocity = TargetMove->Velocity;
	}
	if (bWeSlide && OwnMove)
	{
		if (OwnMove->IsBellySliding())
		{
			bOwnSlideSeen = true;
		}
		else if (bOwnSlideSeen && OwnSlideStop < 0.f)
		{
			OwnSlideStop = OwnMove->GetBellySlideDistance();
		}
	}
	return true;
}

void APinguNetTest::JudgeSlideCheck(int32 Index, const TCHAR* Why)
{
	using namespace PinguNetTestPrivate;
	if (Index < 0 || Index > 1 || bSlideJudged || !bSlideWatching)
	{
		return;
	}
	bSlideJudged = true;

	FProgress& P = Progress();
	if (P.bSlideJudged[Index])
	{
		return;
	}
	P.bSlideJudged[Index] = true;
	const bool bWeSlide = (Index == 0) != bHostRole;
	const bool bHit = SlideHitDistance >= 0.f;
	bool bOk = bHit;
	FString Detail;
	if (bWeSlide)
	{
		// Our slide ran into the other penguin, knocked it away and stopped right there (the server's contact reached us).
		const APenguinCharacter* Own = GetOwnPenguin();
		const UPenguinMovementComponent* OwnMove = Own ? Own->GetPenguinMovement() : nullptr;
		const int32 Slides = OwnMove ? OwnMove->SlideEvents - SlideOwnSlidesAtStart : 0;
		const bool bStopped = OwnSlideStop >= 0.f && OwnSlideStop < SlideFullLength;
		bOk = bHit && Slides > 0 && bStopped;
		Detail = FString::Printf(TEXT("our slides %d, it stopped after %.0f cm; the other penguin knocked away %d (the slide at %.0f cm, sent off at %.0f / %.0f cm/s)"),
			Slides, OwnSlideStop, bHit ? 1 : 0, SlideHitDistance, SlideTargetVelocity.X, SlideTargetVelocity.Z);
	}
	else
	{
		Detail = FString::Printf(TEXT("our penguin knocked away by the other's slide %d (the slide at %.0f cm, sent off at %.0f / %.0f cm/s)"),
			bHit ? 1 : 0, SlideHitDistance, SlideTargetVelocity.X, SlideTargetVelocity.Z);
	}
	P.bSlideOk[Index] = bOk;
	P.SlideDetail[Index] = FString::Printf(TEXT("%s; %s"), *Detail, Why);
	Report(FString::Printf(TEXT("slide check %d: %s (%s)"), Index + 1, bOk ? TEXT("ok") : TEXT("FAIL"), *P.SlideDetail[Index]));
}

// ---------------------------------------------------------------------------- the slap's stun and the ice shield over LAN

namespace PinguNetTestPrivate
{
	/** A slot whose ability a press uses at once: not an empty one, not the icicle (its press only begins a charge, thrown
	 *  when the key goes up - a use that may come after the stun); a ready one if there is. Slot 0 if none fits. */
	static int32 PlainAbilitySlot(const APenguinCharacter* Penguin)
	{
		const UPenguinAbilityComponent* Abilities = Penguin ? Penguin->GetAbilities() : nullptr;
		int32 Found = INDEX_NONE;
		for (int32 Slot = 0; Abilities && Slot < Abilities->GetAbilities().Num(); ++Slot)
		{
			const EPenguinAbility Ability = Abilities->GetAbility(Slot);
			if (Ability == EPenguinAbility::None || UPenguinAbilityComponent::IsChargedAbility(Ability))
			{
				continue;
			}
			if (Abilities->GetCooldownRemaining(Slot) <= 0.f)
			{
				return Slot;
			}
			Found = Found == INDEX_NONE ? Slot : Found;
		}
		return Found == INDEX_NONE ? 0 : Found;
	}

	static int32 SumUses(const APenguinCharacter* Penguin)
	{
		const UPenguinAbilityComponent* Abilities = Penguin ? Penguin->GetAbilities() : nullptr;
		int32 Uses = 0;
		for (int32 Slot = 0; Abilities && Slot < Abilities->GetAbilities().Num(); ++Slot)
		{
			Uses += Abilities->GetUseCount(Slot);
		}
		return Uses;
	}
}

FVector2D APinguNetTest::UpdateEffectCheck(int32 Index, float T)
{
	using namespace PinguNetTestPrivate;
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	APenguinCharacter* HostPenguin = bHostRole ? Own : Other;
	APenguinCharacter* ClientPenguin = bHostRole ? Other : Own;
	UPenguinMovementComponent* ClientMove = ClientPenguin ? ClientPenguin->GetPenguinMovement() : nullptr;
	if (!HostPenguin || !ClientPenguin || !ClientMove)
	{
		return FVector2D::ZeroVector;
	}
	if (!bEffectWatching)
	{
		bEffectWatching = true;
		EffectHitsAtStart = ClientMove->HitEvents;
		EffectUsesAtStart = SumUses(ClientPenguin);
		Report(FString::Printf(TEXT("effect check %d: %s"), Index + 1, Index == 0
			? TEXT("the host's penguin slaps the client's") : TEXT("the host freezes the client's penguin, hits the ice with a snowball (it stays), freezes it again, blows a gust at it")));
	}

	// Host: another platform in the air (two rows above the slide's), the two penguins on it.
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (bHostRole && !bEffectSetUp && T >= EffectSetupAt && Island)
	{
		bEffectSetUp = true;
		for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		const FBox Bounds = Island->GetBounds();
		const float MidX = Bounds.IsValid ? static_cast<float>(Bounds.Min.X + Bounds.Max.X) * 0.5f : 0.f;
		const float TopZ = Bounds.IsValid ? static_cast<float>(Bounds.Max.Z) : 0.f;
		const FIntPoint Left = Island->WorldToCell(FVector(MidX - SlidePlatformColumns * 0.5f * Island->CellSize, 0.f,
			TopZ + (EffectPlatformRowsUp + 0.5f) * Island->CellSize));
		if (Index == 0)
		{
			// An ice block standing on the platform's left end, for the hook later (UpdateHookCheck).
			EffectIceCell = FIntPoint(Left.X, Left.Y + 1);
		}
		TArray<FIntPoint> Cells;
		for (int32 Column = 0; Column < SlidePlatformColumns; ++Column)
		{
			Cells.Add(FIntPoint(Left.X + Column, Left.Y));
		}
		const int32 Added = Island->AddBlocks(Cells, EBlockKind::Rock, EffectPlatformLife).Num();
		if (Index == 0)
		{
			Island->AddBlocks({ EffectIceCell }, EBlockKind::Ice, EffectPlatformLife);
		}
		Island->FlushEdits();
		const float FloorZ = static_cast<float>(Island->CellToWorld(Left).Z) + Island->CellSize * 0.5f;
		const float StartX = static_cast<float>(Island->CellToWorld(FIntPoint(Left.X + 3, Left.Y)).X);
		const auto Place = [FloorZ](APenguinCharacter* Penguin, float X)
		{
			const float Half = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			Penguin->NetTeleport(FVector(X, 0.f, FloorZ + Half + 2.f), FVector::ZeroVector, true);
		};
		const float Gap = Index == 0 ? EffectSlapGap : EffectIceGap;
		Place(HostPenguin, StartX);
		Place(ClientPenguin, StartX + Gap);
		Report(FString::Printf(TEXT("effect check %d: platform of %d blocks at z %.0f, the client's penguin %.0f cm from the host's"), Index + 1, Added, FloorZ, Gap));
	}
	// From the host's penguin to the client's (taken before the hits move it).
	const float Dx = static_cast<float>(ClientPenguin->GetActorLocation().X - HostPenguin->GetActorLocation().X);
	if (FMath::Abs(Dx) > 10.f && EffectHitT < 0.f && EffectSnowballT < 0.f)
	{
		EffectWay = Dx > 0.f ? 1.f : -1.f;
	}
	const auto HostHit = [this, HostPenguin, ClientPenguin](EPenguinAbility Source, const FVector2D& Knockback, float Stun, float Freeze)
	{
		FPenguinHit Hit;
		Hit.Instigator = HostPenguin;
		Hit.Source = Source;
		Hit.Knockback = Knockback;
		Hit.Stun = Stun;
		Hit.Freeze = Freeze;
		Hit.Location = ClientPenguin->GetActorLocation();
		const bool bLanded = ClientPenguin->ApplyPenguinHit(Hit);
		Report(FString::Printf(TEXT("effect check: %s at the client's penguin, landed %d"), UPenguinAbilityComponent::GetInfo(Source).Name, bLanded ? 1 : 0));
		return bLanded;
	};

	FVector2D Stick = FVector2D::ZeroVector;
	if (Index == 0)
	{
		// The slap: a second's stun - the client's player pushes toward the host and tries an ability all along.
		if (bHostRole && EffectStep == 0 && T >= EffectSlapAt)
		{
			EffectStep = 1;
			HostHit(EPenguinAbility::FlipperSlap, FVector2D(EffectWay * 650.f, 300.f), 1.f, 0.f);
		}
		if (EffectHitT < 0.f && ClientMove->HitEvents != EffectHitsAtStart)
		{
			EffectHitT = T;
		}
		if (EffectHitT >= 0.f)
		{
			const float StunLeft = ClientMove->GetStunRemaining();
			EffectStunAtHit = FMath::Max(EffectStunAtHit, StunLeft);
			const float Toward = -EffectWay * static_cast<float>(ClientMove->Velocity.X);
			if (EffectStunEndT < 0.f)
			{
				if (StunLeft > 0.f || T < EffectHitT + 0.1f)
				{
					EffectMaxToward = FMath::Max(EffectMaxToward, Toward);
					bEffectDizzy |= ClientPenguin->IsDizzy();
				}
				else
				{
					EffectStunEndT = T;
				}
			}
			else if (EffectMovedAgainT < 0.f && Toward > 150.f)
			{
				EffectMovedAgainT = T;
			}
			if (!bHostRole && (EffectMovedAgainT < 0.f || T < EffectMovedAgainT + 0.3f))
			{
				Stick = FVector2D(-EffectWay, 0.f);
				// (Pressed early in the stun: at a low frame rate a press late in it reached the server after its stun had ended.)
				if (!bEffectAbilityTried && T > EffectHitT + 0.1f && EffectStunEndT < 0.f && Own)
				{
					bEffectAbilityTried = true;
					Own->ActivateAbility(PlainAbilitySlot(Own));
				}
			}
		}

		// Then (after the hook) an ordinary hit, no slap, sends the client's penguin flying: in the flight, still stunned, its
		// player throws a snowball - straight down, a hop - and the server lets it (only the slap's dizzy stun and the ice stop
		// abilities). The host hands it the snowball first (ready).
		if (T >= EffectGiftAt && !bKnockWatching)
		{
			bKnockWatching = true;
			EffectUsesBeforeKnock = SumUses(ClientPenguin) - EffectUsesAtStart;
			KnockUsesAtStart = SumUses(ClientPenguin);
			KnockHitsAtStart = ClientMove->HitEvents;
			if (bHostRole && ClientPenguin->GetAbilities())
			{
				PinguNetTestGrant::Give(ClientPenguin->GetAbilities(), EPenguinAbility::Snowball);
			}
		}
		if (bHostRole && EffectStep == 1 && T >= EffectKnockAt)
		{
			EffectStep = 2;
			const float Away = ClientPenguin->GetActorLocation().X >= HostPenguin->GetActorLocation().X ? 1.f : -1.f;
			HostHit(EPenguinAbility::Snowball, FVector2D(Away * EffectKnockSide, EffectKnockUp), EffectKnockStun, 0.f);
		}
		if (bKnockWatching && KnockT < 0.f && T >= EffectKnockAt && ClientMove->HitEvents != KnockHitsAtStart)
		{
			KnockT = T;
		}
		if (!bHostRole && Own && KnockT >= 0.f && !bKnockPressed && T > KnockT + EffectKnockPressAfter && T < KnockT + EffectKnockStun
			&& ClientMove->IsStunned() && !ClientMove->IsMovingOnGround())
		{
			bKnockPressed = true;
			const UPenguinAbilityComponent* OwnAbilities = Own->GetAbilities();
			const int32 SnowballSlot = OwnAbilities ? OwnAbilities->FindSlot(EPenguinAbility::Snowball) : INDEX_NONE;
			bKnockSlotReady = SnowballSlot != INDEX_NONE && OwnAbilities->IsReady(SnowballSlot);
			if (SnowballSlot != INDEX_NONE)
			{
				Own->SetAimPoint(Own->GetAimOrigin() - FVector(0.f, 0.f, 500.f)); // at the platform below (a hop, nobody there)
				Own->ActivateAbility(SnowballSlot);
			}
		}
		if (bKnockWatching && KnockT >= 0.f && KnockUseT < 0.f && SumUses(ClientPenguin) > KnockUsesAtStart)
		{
			KnockUseT = T;
			bKnockUseStunned = ClientMove->IsStunned();
			bKnockUseInAir = !ClientMove->IsMovingOnGround();
		}
	}
	else
	{
		// The ice: frozen, a snowball (it bursts on the ice: still frozen, a nudge), frozen again, a gust (the ice cube slides off, still frozen).
		if (bHostRole)
		{
			if (EffectStep == 0 && T >= EffectFreezeAAt)
			{
				EffectStep = 1;
				HostHit(EPenguinAbility::FrostBreath, FVector2D::ZeroVector, 0.f, 3.f);
			}
			if (EffectStep == 1 && T >= EffectSnowballAt)
			{
				EffectStep = 2;
				HostHit(EPenguinAbility::Snowball, FVector2D(EffectWay * 1300.f, 480.f), 0.4f, 0.f);
			}
			if (EffectStep == 2 && T >= EffectFreezeBAt)
			{
				EffectStep = 3;
				HostHit(EPenguinAbility::FrostBreath, FVector2D::ZeroVector, 0.f, 3.f);
			}
			if (EffectStep == 3 && T >= EffectGustAt)
			{
				EffectStep = 4;
				HostHit(EPenguinAbility::Gust, FVector2D(EffectWay * 1900.f, 380.f), 0.45f, 0.f);
			}
		}
		const bool bFrozen = ClientMove->IsFrozen();
		if (T >= EffectFreezeAAt && T < EffectSnowballAt)
		{
			bEffectFrozenA |= bFrozen;
		}
		if (T >= EffectSnowballAt && EffectSnowballT < 0.f)
		{
			EffectSnowballT = T;
		}
		if (EffectSnowballT >= 0.f && T < EffectFreezeBAt)
		{
			if (EffectThawT < 0.f && !bFrozen && bEffectFrozenA)
			{
				EffectThawT = T;
			}
			if (T > EffectSnowballT + 0.05f)
			{
				EffectMaxSpeedOnIce = FMath::Max(EffectMaxSpeedOnIce, static_cast<float>(ClientMove->Velocity.Size2D()));
			}
		}
		if (T >= EffectFreezeBAt && T < EffectGustAt)
		{
			bEffectFrozenB |= bFrozen;
		}
		if (T >= EffectGustAt && EffectGustT < 0.f)
		{
			EffectGustT = T;
			EffectGustFrom = ClientPenguin->GetActorLocation();
		}
		if (EffectGustT >= 0.f)
		{
			if (!bEffectStillSampled && T > EffectGustT + 0.4f)
			{
				bEffectStillSampled = true;
				bEffectStillFrozen = bFrozen;
			}
			if (T < EffectFreezeCAt)
			{
				EffectSlid = FMath::Max(EffectSlid, FMath::Abs(static_cast<float>(ClientPenguin->GetActorLocation().X - EffectGustFrom.X)));
			}
		}
		if (T < EffectFreezeCAt)
		{
			EffectHitsAfterIce = ClientMove->HitEvents - EffectHitsAtStart; // (the hook check's yank comes after)
		}
		bEffectAlive &= ClientPenguin->IsAlive();
	}
	UpdateHookCheck(Index, T, HostPenguin, ClientPenguin);
	return Stick;
}

void APinguNetTest::JudgeEffectCheck(int32 Index, const TCHAR* Why)
{
	using namespace PinguNetTestPrivate;
	if (Index < 0 || Index > 1 || bEffectJudged || !bEffectWatching)
	{
		return;
	}
	bEffectJudged = true;
	FProgress& P = Progress();
	if (P.bEffectJudged[Index])
	{
		return;
	}
	P.bEffectJudged[Index] = true;
	const APenguinCharacter* ClientPenguin = bHostRole ? GetOtherPlayerPenguin() : GetOwnPenguin();
	bool bOk = false;
	FString Detail;
	if (Index == 0)
	{
		const float Stunned = EffectHitT >= 0.f && EffectStunEndT >= 0.f ? EffectStunEndT - EffectHitT : -1.f;
		// (Counted up to the knock below: the snowball thrown in that flight is meant to come out.)
		const int32 UsesDuring = EffectUsesBeforeKnock >= 0 ? EffectUsesBeforeKnock : SumUses(ClientPenguin) - EffectUsesAtStart;
		// (Walking would be 560 cm/s toward the host. The host's copy of the client's penguin follows the owner's moves a
		// moment late and has shown up to ~110 cm/s of drift while stunned at a good frame rate: a looser bound there.)
		const float MaxToward = bHostRole ? 200.f : 80.f;
		bOk = EffectHitT >= 0.f && Stunned > 0.8f && Stunned < 1.35f && EffectMaxToward < MaxToward && bEffectDizzy;
		if (!bHostRole)
		{
			// Our own penguin: the whole second without moving or using anything, then it walked again.
			bOk = bOk && EffectStunAtHit > 0.85f && EffectMovedAgainT >= 0.f && bEffectAbilityTried && UsesDuring == 0;
		}
		Detail = FString::Printf(TEXT("slapped %d, stunned %.2f s (from %.2f), toward the host at most %.0f cm/s while stunned, dizzy %d, walked again %d, "
			"ability tried %d and used %d"), EffectHitT >= 0.f ? 1 : 0, Stunned, EffectStunAtHit, EffectMaxToward, bEffectDizzy ? 1 : 0,
			EffectMovedAgainT >= 0.f ? 1 : 0, bEffectAbilityTried ? 1 : 0, UsesDuring);

		// The knock afterwards: the snowball thrown in the flight came out, while the client's penguin was still stunned in the air.
		if (bKnockWatching && !P.bKnockJudged)
		{
			P.bKnockJudged = true;
			const int32 KnockUses = SumUses(ClientPenguin) - KnockUsesAtStart;
			const float UseAfter = KnockT >= 0.f && KnockUseT >= 0.f ? KnockUseT - KnockT : -1.f;
			bool bKnockOk = KnockT >= 0.f && KnockUses == 1 && UseAfter >= 0.f && UseAfter < EffectKnockStun && bKnockUseStunned && bKnockUseInAir;
			if (!bHostRole)
			{
				bKnockOk = bKnockOk && bKnockPressed && bKnockSlotReady;
			}
			P.bKnockOk = bKnockOk;
			P.KnockDetail = FString::Printf(TEXT("knocked %d, %s, uses %d (want 1), it showed here %.2f s into the flight, stunned %d, in the air %d; %s"),
				KnockT >= 0.f ? 1 : 0, bHostRole ? TEXT("the client's press is its own") : (bKnockPressed ? (bKnockSlotReady ? TEXT("pressed in the flight, snowball ready")
				: TEXT("pressed in the flight, NO snowball ready")) : TEXT("NOT pressed (never stunned in the air)")), KnockUses, UseAfter,
				bKnockUseStunned ? 1 : 0, bKnockUseInAir ? 1 : 0, Why);
			Report(FString::Printf(TEXT("knock check: %s (%s)"), bKnockOk ? TEXT("ok") : TEXT("FAIL"), *P.KnockDetail));
		}
	}
	else
	{
		// The snowball bursts on the ice: the client's penguin stays frozen from the first freeze to the second (it never thaws:
		// EffectThawT stays -1) and only gets a small nudge - far under the snowball's own push (1300 cm/s) - seen on this machine.
		bOk = bEffectFrozenA && EffectThawT < 0.f && EffectSnowballT >= 0.f && EffectMaxSpeedOnIce < 600.f && bEffectFrozenB && bEffectStillFrozen
			&& EffectSlid > 100.f && EffectHitsAfterIce == 0 && bEffectAlive;
		Detail = FString::Printf(TEXT("frozen %d, the snowball left the ice alone (thawed %d, want 0), fastest the cube went after it %.0f cm/s (a nudge: under 600); frozen again %d, after the gust still frozen %d, "
			"slid %.0f cm; hits taken %d, alive %d"), bEffectFrozenA ? 1 : 0, EffectThawT >= 0.f ? 1 : 0, EffectMaxSpeedOnIce, bEffectFrozenB ? 1 : 0,
			bEffectStillFrozen ? 1 : 0, EffectSlid, EffectHitsAfterIce, bEffectAlive ? 1 : 0);
	}
	P.bEffectOk[Index] = bOk;
	P.EffectDetail[Index] = FString::Printf(TEXT("%s; %s"), *Detail, Why);
	Report(FString::Printf(TEXT("effect check %d: %s (%s)"), Index + 1, bOk ? TEXT("ok") : TEXT("FAIL"), *P.EffectDetail[Index]));
}

// ---------------------------------------------------------------------------- the fish hook and ice over LAN

void APinguNetTest::UpdateHookCheck(int32 Index, float T, APenguinCharacter* HostPenguin, APenguinCharacter* ClientPenguin)
{
	using namespace PinguNetTestPrivate;
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	UPenguinMovementComponent* ClientMove = ClientPenguin ? ClientPenguin->GetPenguinMovement() : nullptr;
	if (!Island || !HostPenguin || !ClientMove)
	{
		return;
	}
	// The host's fish hook, the way the ability throws it (UPenguinAbilityComponent::DoFishHook), at a point.
	const auto ThrowHook = [this, HostPenguin](const FVector& At)
	{
		const FVector From = HostPenguin->GetAimOrigin();
		FVector Dir = At - From;
		Dir.Y = 0.f;
		Dir = Dir.GetSafeNormal();
		HostPenguin->SetAimPoint(At);
		return APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::FishHook, HostPenguin, From + Dir * 45.f, Dir * APenguinProjectile::HookSpeed) != nullptr;
	};

	if (Index == 0)
	{
		// Round 1: an ice block of the platform pulled out whole - it comes about halfway and is a block again.
		if (bHostRole && !bClientParked && T >= EffectParkAt)
		{
			// The client's penguin walked back to the host after its stun: 3 m to the right again, away from the hook's way.
			bClientParked = true;
			const FVector HostAt = HostPenguin->GetActorLocation();
			const float Half = ClientPenguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const float FloorZ = static_cast<float>(HostAt.Z) - HostPenguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			ClientPenguin->NetTeleport(FVector(HostAt.X + EffectParkGap, 0.f, FloorZ + Half + 2.f), FVector::ZeroVector, true);
		}
		if (T < EffectHookBlockAt)
		{
			return;
		}
		if (!bHookWatching)
		{
			bHookWatching = true;
			BrokenAtHook = Island->GetBrokenBlocks();
		}
		if (bHostRole && !bHookFired)
		{
			bHookFired = true;
			const bool bThere = Island->GetBlock(EffectIceCell.X, EffectIceCell.Y) == EBlockKind::Ice;
			const bool bThrown = bThere && ThrowHook(Island->CellToWorld(EffectIceCell));
			Report(FString::Printf(TEXT("hook check 1: the host's fish hook at the ice block on the platform's end (there %d, thrown %d)"),
				bThere ? 1 : 0, bThrown ? 1 : 0));
		}
		if (!bFlyerSeen)
		{
			for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
			{
				// (Seen right after it came out: still in the cell it came out of.)
				bFlyerSeen = true;
				FlyerFromCell = Island->WorldToCell(It->GetActorLocation());
				Report(FString::Printf(TEXT("hook check 1: the pulled-out ice block flies (from cell %d,%d)"), FlyerFromCell.X, FlyerFromCell.Y));
				break;
			}
		}
		return;
	}

	// Round 2: the client's penguin frozen once more and hooked - the whole ice cube comes about halfway, falling like a
	// block, and shatters where it lands. The client's own machine flies it; the server breaks the ice where it lands.
	if (T < EffectFreezeCAt)
	{
		return;
	}
	if (!bYankWatching)
	{
		bYankWatching = true;
		YankHitsAtStart = ClientMove->HitEvents;
		if (bHostRole)
		{
			FPenguinHit Freeze;
			Freeze.Instigator = HostPenguin;
			Freeze.Source = EPenguinAbility::FrostBreath;
			Freeze.Freeze = 3.f;
			Freeze.Stun = 0.f;
			Freeze.Location = ClientPenguin->GetActorLocation();
			ClientPenguin->ApplyPenguinHit(Freeze);
		}
	}
	if (bHostRole && !bHookFired && T >= EffectYankAt)
	{
		bHookFired = true;
		const bool bThrown = ThrowHook(ClientPenguin->GetAimOrigin());
		Report(FString::Printf(TEXT("hook check 2: the host's fish hook at the client's frozen penguin (frozen %d, thrown %d)"),
			ClientMove->IsFrozen() ? 1 : 0, bThrown ? 1 : 0));
	}
	const float X = static_cast<float>(ClientPenguin->GetActorLocation().X);
	if (YankT < 0.f)
	{
		if (T >= EffectYankAt && ClientMove->HitEvents != YankHitsAtStart)
		{
			YankT = T;
			YankFromX = X;
			YankHostX = static_cast<float>(HostPenguin->GetActorLocation().X);
			bYankFrozenAt = ClientMove->IsFrozen();
		}
		return;
	}
	const float Since = T - YankT;
	bYankAlive &= ClientPenguin->IsAlive();
	YankNearest = FMath::Min(YankNearest, (X - YankHostX) * FMath::Sign(YankFromX - YankHostX));
	const bool bAir = !ClientMove->IsMovingOnGround();
	if (YankLandT < 0.f)
	{
		for (int32 Sample = 0; Sample < 2; ++Sample)
		{
			if (YankSampleT[Sample] < 0.f && bAir && Since > (Sample == 0 ? 0.04f : 0.13f))
			{
				YankSampleT[Sample] = T;
				YankSampleVz[Sample] = static_cast<float>(ClientMove->Velocity.Z);
			}
		}
		// Every frame in the air (on this machine's game clock), slower than a falling block's top speed: the gravity is fitted
		// to all of them (two single frames were too noisy at a low frame rate).
		if (bAir && Since > 0.04f && ClientMove->Velocity.Z > -0.95f * ABlockIsland::FallingBlockMaxSpeed)
		{
			YankVzSamples.Add(FVector2D(GetWorld()->GetTimeSeconds(), ClientMove->Velocity.Z));
		}
		if (!bYankMidSampled && Since > 0.1f && bAir)
		{
			bYankMidSampled = true;
			bYankFrozenMid = ClientMove->IsFrozen();
		}
		if (Since > 0.05f && !bAir)
		{
			YankLandT = T;
			YankLandX = X;
		}
	}
	if (YankThawT < 0.f && !ClientMove->IsFrozen())
	{
		YankThawT = T;
	}
}

void APinguNetTest::JudgeHookCheck(int32 Index, const TCHAR* Why)
{
	using namespace PinguNetTestPrivate;
	if (Index < 0 || Index > 1)
	{
		return;
	}
	FProgress& P = Progress();
	if (P.bHookJudged[Index])
	{
		return;
	}
	const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if ((Index == 0 && !bHookWatching) || (Index == 1 && !bYankWatching) || !Island)
	{
		return; // (it never started: logged as not run)
	}
	P.bHookJudged[Index] = true;
	bool bOk = false;
	FString Detail;
	if (Index == 0)
	{
		// Where it came out is empty; it is an ice block again 1 to 3 columns toward the host, in the same row; not broken.
		const APenguinCharacter* HostPenguin = bHostRole ? GetOwnPenguin() : GetOtherPlayerPenguin();
		const float FromX = static_cast<float>(Island->CellToWorld(FlyerFromCell).X);
		const int32 Side = HostPenguin && HostPenguin->GetActorLocation().X < FromX ? -1 : 1;
		const bool bFromEmpty = bFlyerSeen && !Island->IsSolidCell(FlyerFromCell);
		int32 Came = 0;
		for (int32 Step = 1; Step <= 3 && Came == 0; ++Step)
		{
			if (Island->GetBlock(FlyerFromCell.X + Side * Step, FlyerFromCell.Y) == EBlockKind::Ice)
			{
				Came = Step;
			}
		}
		const int32 Broken = Island->GetBrokenBlocks() - BrokenAtHook;
		bOk = bFlyerSeen && bFromEmpty && Came > 0 && Broken == 0;
		Detail = FString::Printf(TEXT("saw it fly %d, its cell empty %d, an ice block again %d blocks toward the host (want 1-2: about halfway of 3), broken %d"),
			bFlyerSeen ? 1 : 0, bFromEmpty ? 1 : 0, Came, Broken);
	}
	else
	{
		const float Gap = FMath::Abs(YankFromX - YankHostX);
		const float Came = YankLandT >= 0.f ? FMath::Abs(YankLandX - YankFromX) : 0.f;
		const float Share = Gap > 1.f ? Came / Gap : 0.f;
		float Gravity = YankSampleT[0] >= 0.f && YankSampleT[1] > YankSampleT[0] + 0.01f
			? (YankSampleVz[0] - YankSampleVz[1]) / (YankSampleT[1] - YankSampleT[0]) : 0.f;
		// A least-squares line through every sample in the air (when they span more than 0.08 s).
		if (YankVzSamples.Num() >= 3 && YankVzSamples.Last().X - YankVzSamples[0].X > 0.08)
		{
			double MeanT = 0.0;
			double MeanV = 0.0;
			for (const FVector2D& Sample : YankVzSamples)
			{
				MeanT += Sample.X;
				MeanV += Sample.Y;
			}
			MeanT /= YankVzSamples.Num();
			MeanV /= YankVzSamples.Num();
			double Covariance = 0.0;
			double Spread = 0.0;
			for (const FVector2D& Sample : YankVzSamples)
			{
				Covariance += (Sample.X - MeanT) * (Sample.Y - MeanV);
				Spread += (Sample.X - MeanT) * (Sample.X - MeanT);
			}
			Gravity = Spread > 1.e-6 ? static_cast<float>(-Covariance / Spread) : Gravity;
		}
		const float ThawAfter = YankLandT >= 0.f && YankThawT >= 0.f ? YankThawT - YankLandT : -1.f;
		// (The gravity counts on the client, whose machine flies its penguin: the host's copy follows its updates a moment late.)
		const bool bGravityOk = bHostRole || (Gravity > 6000.f && Gravity < 9000.f);
		bOk = YankT >= 0.f && bYankFrozenAt && bYankFrozenMid && YankLandT >= 0.f && Share > 0.3f && Share < 0.7f && YankNearest > 0.f
			&& ThawAfter > -0.05f && ThawAfter < 0.4f && bYankAlive && bGravityOk;
		Detail = FString::Printf(TEXT("yanked %d (frozen %d), frozen in the air %d, gravity %.0f (want ~%.0f), came %.0f of %.0f cm (%.0f%%, want about half), "
			"never past the host %d, the ice shattered %.2f s after it landed, alive %d"), YankT >= 0.f ? 1 : 0, bYankFrozenAt ? 1 : 0,
			bYankFrozenMid ? 1 : 0, Gravity, ABlockIsland::FallingBlockGravity, Came, Gap, 100.f * Share, YankNearest > 0.f ? 1 : 0, ThawAfter,
			bYankAlive ? 1 : 0);
	}
	P.bHookOk[Index] = bOk;
	P.HookDetail[Index] = FString::Printf(TEXT("%s; %s"), *Detail, Why);
	Report(FString::Printf(TEXT("hook check %d: %s (%s)"), Index + 1, bOk ? TEXT("ok") : TEXT("FAIL"), *P.HookDetail[Index]));
}

FVector2D APinguNetTest::UpdatePadCheck(float T)
{
	using namespace PinguNetTestPrivate;
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	APenguinCharacter* HostPenguin = bHostRole ? Own : Other;
	APenguinCharacter* ClientPenguin = bHostRole ? Other : Own;
	const UPenguinMovementComponent* ClientMove = ClientPenguin ? ClientPenguin->GetPenguinMovement() : nullptr;
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (!HostPenguin || !ClientPenguin || !ClientMove || !Island)
	{
		return FVector2D::ZeroVector;
	}

	// Host: a fresh platform above the others, the two players' penguins on it, an icicle stuck in it right of the client's.
	if (bHostRole && !bPadSetUp && T >= PadSetupAt)
	{
		bPadSetUp = true;
		for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		const FBox Bounds = Island->GetBounds();
		const float MidX = Bounds.IsValid ? static_cast<float>(Bounds.Min.X + Bounds.Max.X) * 0.5f : 0.f;
		const float TopZ = Bounds.IsValid ? static_cast<float>(Bounds.Max.Z) : 0.f;
		const FIntPoint Left = Island->WorldToCell(FVector(MidX - PadPlatformColumns * 0.5f * Island->CellSize, 0.f,
			TopZ + (PadPlatformRowsUp + 0.5f) * Island->CellSize));
		TArray<FIntPoint> Cells;
		for (int32 Column = 0; Column < PadPlatformColumns; ++Column)
		{
			Cells.Add(FIntPoint(Left.X + Column, Left.Y));
		}
		const int32 Added = Island->AddBlocks(Cells, EBlockKind::Rock, PadPlatformLife).Num();
		Island->FlushEdits();
		const float FloorZ = static_cast<float>(Island->CellToWorld(Left).Z) + Island->CellSize * 0.5f;
		const float HostX = static_cast<float>(Island->CellToWorld(FIntPoint(Left.X + 1, Left.Y)).X);
		const float ClientX = static_cast<float>(Island->CellToWorld(FIntPoint(Left.X + 4, Left.Y)).X);
		const auto Place = [FloorZ](APenguinCharacter* Penguin, float X)
		{
			const float Half = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			Penguin->NetTeleport(FVector(X, 0.f, FloorZ + Half + 2.f), FVector::ZeroVector, true);
		};
		Place(HostPenguin, HostX);
		Place(ClientPenguin, ClientX);
		// The icicle the way the ability throws it, straight down from just above the platform: it sticks in the rock.
		const FVector PadAt(ClientX + PadGap, 0.f, FloorZ + 90.f);
		const bool bThrown = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Icicle, HostPenguin, PadAt, FVector(0.f, 0.f, -1900.f)) != nullptr;
		Report(FString::Printf(TEXT("pad check: platform of %d blocks at z %.0f, the client's penguin at x %.0f, an icicle thrown down at x %.0f (%d)"),
			Added, FloorZ, ClientX, PadAt.X, bThrown ? 1 : 0));
	}

	// Both: the icicle stuck in the platform (as this machine sees it), how high the client's penguin goes.
	for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
	{
		bPadSeen |= It->IsGroundPad();
	}
	const float FeetZ = static_cast<float>(ClientPenguin->GetFeetLocation().Z);
	if (T < PadWatchAt)
	{
		return FVector2D::ZeroVector;
	}
	if (!bPadWatching)
	{
		bPadWatching = true;
		PadBouncesAtStart = ClientMove->PadBounceEvents;
		PadStartFeetZ = FeetZ;
		PadPeakFeetZ = FeetZ;
		Report(FString::Printf(TEXT("pad check: the client's penguin walks into the icicle in the ground (seen here %d)"), bPadSeen ? 1 : 0));
	}
	PadPeakFeetZ = FMath::Max(PadPeakFeetZ, FeetZ);
	const bool bBounced = ClientMove->PadBounceEvents != PadBouncesAtStart;
	if (bBounced && PadBounceT < 0.f)
	{
		PadBounceT = T;
	}
	// The client walks right (into it) until it threw it; the host holds still.
	return !bHostRole && T >= PadWalkAt && !bBounced ? FVector2D(1.f, 0.f) : FVector2D::ZeroVector;
}

void APinguNetTest::JudgePadCheck(const TCHAR* Why)
{
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	if (bPadJudged || !bPadWatching || P.bPadJudged)
	{
		return; // (it never started: logged as not run)
	}
	bPadJudged = true;
	P.bPadJudged = true;
	const APenguinCharacter* ClientPenguin = bHostRole ? GetOtherPlayerPenguin() : GetOwnPenguin();
	const UPenguinMovementComponent* ClientMove = ClientPenguin ? ClientPenguin->GetPenguinMovement() : nullptr;
	const int32 Bounces = ClientMove ? ClientMove->PadBounceEvents - PadBouncesAtStart : 0;
	const float Rose = PadPeakFeetZ - PadStartFeetZ;
	// (The host sees its copy of the client's penguin follow the client's moves; the throw is the client's own.)
	P.bPadOk = bPadSeen && Bounces >= 1 && Rose > PadMinRise;
	P.PadDetail = FString::Printf(TEXT("icicle in the ground seen %d, thrown %d times (want 1 or more, first %.2f s into the walk), feet rose %.0f cm (want over %.0f); %s"),
		bPadSeen ? 1 : 0, Bounces, PadBounceT >= 0.f ? PadBounceT - PadWalkAt : -1.f, Rose, PadMinRise, Why);
	Report(FString::Printf(TEXT("pad check: %s (%s)"), P.bPadOk ? TEXT("ok") : TEXT("FAIL"), *P.PadDetail));
}

void APinguNetTest::UpdateThrowCheck(float T)
{
	using namespace PinguNetTestPrivate;
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	APenguinCharacter* HostPenguin = bHostRole ? Own : Other;
	APenguinCharacter* ClientPenguin = bHostRole ? Other : Own;
	UPenguinAbilityComponent* HostAbilities = HostPenguin ? HostPenguin->GetAbilities() : nullptr;
	UPenguinAbilityComponent* ClientAbilities = ClientPenguin ? ClientPenguin->GetAbilities() : nullptr;
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (!HostAbilities || !ClientAbilities || !Island || Progress().bThrowJudged)
	{
		return;
	}
	if (!bThrowWatching)
	{
		bThrowWatching = true;
		Report(TEXT("throw check: the host's penguin charges an icicle a full second and throws it, then taps one; the client's charges half a second"));
	}
	const auto AimRight = [](APenguinCharacter* Penguin)
	{
		Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(1000.f, 0.f, 0.f)); // level, to the right (the release takes it)
	};
	const auto AimLeft = [](APenguinCharacter* Penguin)
	{
		Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(-1000.f, 0.f, 0.f)); // level, to the left (never the throw's aim: it lets go aiming right)
	};

	// Host: a platform high above everything, the client's penguin on its left part, the host's two blocks right of it; both
	// get the icicle (ready).
	if (bHostRole && !bThrowSetUp && T >= ThrowSetupAt)
	{
		bThrowSetUp = true;
		for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		const FBox Bounds = Island->GetBounds();
		const float MidX = Bounds.IsValid ? static_cast<float>(Bounds.Min.X + Bounds.Max.X) * 0.5f : 0.f;
		const float TopZ = Bounds.IsValid ? static_cast<float>(Bounds.Max.Z) : 0.f;
		const FIntPoint Left = Island->WorldToCell(FVector(MidX - ThrowPlatformColumns * 0.5f * Island->CellSize, 0.f,
			TopZ + (ThrowPlatformRowsUp + 0.5f) * Island->CellSize));
		TArray<FIntPoint> Cells;
		for (int32 Column = 0; Column < ThrowPlatformColumns; ++Column)
		{
			Cells.Add(FIntPoint(Left.X + Column, Left.Y));
		}
		const int32 Added = Island->AddBlocks(Cells, EBlockKind::Rock, ThrowPlatformLife).Num();
		Island->FlushEdits();
		const float FloorZ = static_cast<float>(Island->CellToWorld(Left).Z) + Island->CellSize * 0.5f;
		const auto Place = [FloorZ, Island, Left](APenguinCharacter* Penguin, int32 Column)
		{
			const float Half = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const float X = static_cast<float>(Island->CellToWorld(FIntPoint(Left.X + Column, Left.Y)).X);
			Penguin->NetTeleport(FVector(X, 0.f, FloorZ + Half + 2.f), FVector::ZeroVector, true);
			Penguin->FaceDirection(1.f);
		};
		Place(ClientPenguin, ThrowClientColumn);
		Place(HostPenguin, ThrowHostColumn);
		PinguNetTestGrant::Give(HostAbilities, EPenguinAbility::Icicle);
		PinguNetTestGrant::Give(ClientAbilities, EPenguinAbility::Icicle);
		Report(FString::Printf(TEXT("throw check: platform of %d blocks at z %.0f, both penguins on it with the icicle"), Added, FloorZ));
	}

	// Host: a full second's charge and the release; a tap; then out of the client's way (behind it).
	if (bHostRole)
	{
		const int32 Slot = HostAbilities->FindSlot(EPenguinAbility::Icicle);
		if (ThrowStep == 0 && T >= ThrowLongPressAt && Slot != INDEX_NONE)
		{
			ThrowStep = 1;
			AimLeft(HostPenguin); // (it faces its aim while it charges: it stands facing right)
			const bool bBegun = HostAbilities->BeginCharge(Slot);
			Report(FString::Printf(TEXT("throw check: the host's penguin charges (%d), aiming left first"), bBegun ? 1 : 0));
		}
		else if (ThrowStep == 1 && !bThrowFaceFlipped && T >= ThrowLongPressAt + ThrowFaceFlipAfter)
		{
			bThrowFaceFlipped = true;
			AimRight(HostPenguin); // the aim crosses over: the penguin turns round to it
		}
		else if (ThrowStep == 1 && T >= ThrowLongPressAt + ThrowLongHold)
		{
			ThrowStep = 2;
			AimRight(HostPenguin);
			HostAbilities->ReleaseCharge(Slot);
		}
		else if (ThrowStep == 2 && T >= ThrowTapAt && Slot != INDEX_NONE)
		{
			ThrowStep = 3;
			PinguNetTestGrant::Give(HostAbilities, EPenguinAbility::Icicle); // ready again (its cooldown runs from the long throw)
			AimRight(HostPenguin);
			const bool bBegun = HostAbilities->BeginCharge(Slot);
			Report(FString::Printf(TEXT("throw check: the host's penguin taps (%d)"), bBegun ? 1 : 0));
		}
		else if (ThrowStep == 3)
		{
			ThrowStep = 4;
			AimRight(HostPenguin);
			HostAbilities->ReleaseCharge(Slot); // the next frame: a tap
		}
		else if (ThrowStep == 4 && T >= ThrowAsideAt)
		{
			ThrowStep = 5;
			const float Half = HostPenguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const FVector Feet = ClientPenguin->GetFeetLocation();
			const float Aside = static_cast<float>(ThrowClientColumn - ThrowHostAsideColumn) * Island->CellSize;
			HostPenguin->NetTeleport(FVector(Feet.X - Aside, 0.f, Feet.Z + Half + 2.f), FVector::ZeroVector, true);
		}
	}
	else
	{
		// Client: our own charge, half a second - shown here at once, the server gets the press and the release.
		const int32 Slot = ClientAbilities->FindSlot(EPenguinAbility::Icicle);
		if (ThrowStep == 0 && T >= ThrowClientPressAt && Slot != INDEX_NONE)
		{
			ThrowStep = 1;
			AimLeft(ClientPenguin); // (aims left for the whole hold, lets go aiming right)
			const bool bBegun = ClientAbilities->BeginCharge(Slot);
			bThrowOwnShownAtOnce = bBegun && ClientAbilities->GetIcicleShow() == EPinguIcicleShow::Charging;
			Report(FString::Printf(TEXT("throw check: our penguin charges (%d), shown here at once %d"), bBegun ? 1 : 0, bThrowOwnShownAtOnce ? 1 : 0));
		}
		else if (ThrowStep == 1 && T >= ThrowClientPressAt + ThrowClientHold)
		{
			ThrowStep = 2;
			AimRight(ClientPenguin);
			ClientAbilities->ReleaseCharge(Slot);
		}
	}

	// Both: the other player's charge as this machine shows it (the icicle in its flipper).
	const float OtherPressAt = bHostRole ? ThrowClientPressAt : ThrowLongPressAt;
	const UPenguinAbilityComponent* OtherAbilities = bHostRole ? ClientAbilities : HostAbilities;
	if (T >= OtherPressAt + ThrowSeenFrom && T <= OtherPressAt + (bHostRole ? ThrowSeenTo : ThrowSeenToLong) && OtherAbilities->IsIcicleInHand()
		&& OtherAbilities->GetIcicleShow() == EPinguIcicleShow::Charging)
	{
		if (!bThrowOtherChargeSeen)
		{
			Report(FString::Printf(TEXT("throw check: the %s penguin seen charging here (%.2f s held as shown)"), bHostRole ? TEXT("client's") : TEXT("host's"),
				OtherAbilities->GetIcicleChargeTime()));
		}
		bThrowOtherChargeSeen = true;
	}

	// Both: each charging penguin faces its aim as this machine shows it (the mesh's forward along X: the host's penguin looks left while
	// it aims left although it stands facing right, then right again; the client's looks left). Counted per window (JudgeThrowCheck).
	const auto SampleFacing = [this, T](int32 Who, const APenguinCharacter* Charger, float PressAt, const float (&Left)[2], const float (*Right)[2])
	{
		const USkeletalMeshComponent* SkelMesh = Charger ? Charger->GetMesh() : nullptr;
		const UPenguinAbilityComponent* ChargerAbilities = Charger ? Charger->GetAbilities() : nullptr;
		if (!SkelMesh || !ChargerAbilities || ChargerAbilities->GetIcicleShow() != EPinguIcicleShow::Charging)
		{
			return;
		}
		const float FwdX = static_cast<float>(SkelMesh->GetComponentQuat().RotateVector(FVector(0.f, 1.f, 0.f)).X);
		if (T >= PressAt + Left[0] && T <= PressAt + Left[1])
		{
			++ThrowFaceFrames[Who][0];
			ThrowFaceWrong[Who][0] += FwdX < -0.5f ? 0 : 1;
		}
		if (Right && T >= PressAt + (*Right)[0] && T <= PressAt + (*Right)[1])
		{
			++ThrowFaceFrames[Who][1];
			ThrowFaceWrong[Who][1] += FwdX > 0.5f ? 0 : 1;
		}
	};
	SampleFacing(0, HostPenguin, ThrowLongPressAt, ThrowFaceHostLeft, &ThrowFaceHostRight);
	SampleFacing(1, ClientPenguin, ThrowClientPressAt, ThrowFaceClientLeft, nullptr);

	// Both: every icicle of the two players, from where its thrower stood to how far it got.
	for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
	{
		APenguinProjectile* Shot = *It;
		const APenguinCharacter* Shooter = Shot->GetShooter();
		if (Shot->GetKind() != EPenguinProjectileKind::Icicle || !Shot->IsFlying() || (Shooter != HostPenguin && Shooter != ClientPenguin)
			|| ThrowWatches.ContainsByPredicate([Shot](const FThrowWatch& Watch) { return Watch.Icicle.Get() == Shot; }))
		{
			continue;
		}
		FThrowWatch& Watch = ThrowWatches.AddDefaulted_GetRef();
		Watch.Icicle = Shot;
		Watch.bHost = Shooter == HostPenguin;
		Watch.FromX = static_cast<float>(Shooter->GetActorLocation().X);
		Report(FString::Printf(TEXT("throw check: an icicle of the %s penguin, %.0f cm/s, gravity %.0f"), Watch.bHost ? TEXT("host's") : TEXT("client's"),
			Shot->GetVelocity().Size(), Shot->GetGravity()));
	}
	for (FThrowWatch& Watch : ThrowWatches)
	{
		if (const AActor* Icicle = Watch.Icicle.Get())
		{
			Watch.Far = FMath::Max(Watch.Far, FMath::Abs(static_cast<float>(Icicle->GetActorLocation().X) - Watch.FromX));
		}
	}
	if (T >= ThrowJudgeAt)
	{
		JudgeThrowCheck(TEXT("judged after the throws"));
	}
}

void APinguNetTest::JudgeThrowCheck(const TCHAR* Why)
{
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	if (bThrowJudged || !bThrowWatching || P.bThrowJudged)
	{
		return; // (it never started: logged as not run)
	}
	bThrowJudged = true;
	P.bThrowJudged = true;
	// In the order they came: the host's full charge, the host's tap; the client's half second.
	TArray<float> HostFar;
	TArray<float> ClientFar;
	for (const FThrowWatch& Watch : ThrowWatches)
	{
		(Watch.bHost ? HostFar : ClientFar).Add(Watch.Far);
	}
	const float Long = HostFar.Num() > 0 ? HostFar[0] : 0.f;
	const float Tap = HostFar.Num() > 1 ? HostFar[1] : 0.f;
	const float Medium = ClientFar.Num() > 0 ? ClientFar[0] : 0.f;
	const bool bCounts = HostFar.Num() == 2 && ClientFar.Num() == 1;
	const bool bFar = Long > ThrowMinLong && Tap > ThrowMinTap && Tap < ThrowMaxTap && Long > ThrowLongOverTap * Tap && Medium > ThrowMinMedium && Medium < ThrowMaxMedium
		&& Medium > Tap && Medium < Long;
	// The client also showed its own charge before the server knew of it.
	const bool bOwnOk = bHostRole || bThrowOwnShownAtOnce;
	// Each charging penguin faced its aim on this machine: the host's looked left while it aimed left (standing facing right), then right
	// again; the client's looked left (windows with enough frames, hardly any of them wrong).
	const auto FaceWindowOk = [this](int32 Who, int32 Window)
	{
		return ThrowFaceFrames[Who][Window] >= ThrowFaceMinFrames && ThrowFaceWrong[Who][Window] <= ThrowFaceMaxWrong;
	};
	const bool bFaceOk = FaceWindowOk(0, 0) && FaceWindowOk(0, 1) && FaceWindowOk(1, 0);
	P.bThrowOk = bThrowOtherChargeSeen && bOwnOk && bCounts && bFar && bFaceOk;
	P.ThrowDetail = FString::Printf(TEXT("the %s charge seen here %d%s; icicles: the host's %d, the client's %d (want 2 and 1); the host's full charge %.0f cm, its tap %.0f cm, "
		"the client's half second %.0f cm (want far: 3500 and more; short: 120 to 400; in between: 450 to 1300); facing the aim - wrong frames of those sampled: the host's penguin aiming left "
		"%d of %d, aiming right again %d of %d, the client's aiming left %d of %d (want at least %d sampled, at most %d wrong) %d; %s"),
		bHostRole ? TEXT("client's") : TEXT("host's"), bThrowOtherChargeSeen ? 1 : 0,
		bHostRole ? TEXT("") : *FString::Printf(TEXT(", our own shown at the press %d"), bThrowOwnShownAtOnce ? 1 : 0),
		HostFar.Num(), ClientFar.Num(), Long, Tap, Medium, ThrowFaceWrong[0][0], ThrowFaceFrames[0][0], ThrowFaceWrong[0][1], ThrowFaceFrames[0][1],
		ThrowFaceWrong[1][0], ThrowFaceFrames[1][0], ThrowFaceMinFrames, ThrowFaceMaxWrong, bFaceOk ? 1 : 0, Why);
	Report(FString::Printf(TEXT("throw check: %s (%s)"), P.bThrowOk ? TEXT("ok") : TEXT("FAIL"), *P.ThrowDetail));
}

FVector2D APinguNetTest::UpdateSizeCheck(float T)
{
	using namespace PinguNetTestPrivate;
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	APenguinCharacter* HostPenguin = bHostRole ? Own : Other;
	APenguinCharacter* ClientPenguin = bHostRole ? Other : Own;
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (!HostPenguin || !ClientPenguin || !Island || Progress().bSizeJudged)
	{
		return FVector2D::ZeroVector;
	}
	if (!SizeGiant.IsValid())
	{
		// The bot (the only penguin no player has).
		for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
		{
			const APlayerState* PlayerState = It->GetPlayerState();
			if (*It != HostPenguin && *It != ClientPenguin && (!PlayerState || PlayerState->IsABot()))
			{
				SizeGiant = *It;
				break;
			}
		}
	}
	APenguinCharacter* Giant = SizeGiant.Get();
	if (!Giant)
	{
		return FVector2D::ZeroVector;
	}

	// Host: a platform above the island, the bot on it growing to 8x, the client's penguin 7 m to its right.
	if (bHostRole && !bSizeSetUp && T >= SizeSetupAt)
	{
		bSizeSetUp = true;
		for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		const FBox Bounds = Island->GetBounds();
		const float MidX = Bounds.IsValid ? static_cast<float>(Bounds.Min.X + Bounds.Max.X) * 0.5f : 0.f;
		const float TopZ = Bounds.IsValid ? static_cast<float>(Bounds.Max.Z) : 0.f;
		const FIntPoint Left = Island->WorldToCell(FVector(MidX - SizePlatformColumns * 0.5f * Island->CellSize, 0.f,
			TopZ + (SizePlatformRowsUp + 0.5f) * Island->CellSize));
		TArray<FIntPoint> Cells;
		for (int32 Column = 0; Column < SizePlatformColumns; ++Column)
		{
			Cells.Add(FIntPoint(Left.X + Column, Left.Y));
		}
		const int32 Added = Island->AddBlocks(Cells, EBlockKind::Rock, SizePlatformLife).Num();
		Island->FlushEdits();
		const float FloorZ = static_cast<float>(Island->CellToWorld(Left).Z) + Island->CellSize * 0.5f;
		const float GiantX = static_cast<float>(Island->CellToWorld(FIntPoint(Left.X + 8, Left.Y)).X);
		const auto Place = [FloorZ](APenguinCharacter* Penguin, float X)
		{
			const float Half = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			Penguin->NetTeleport(FVector(X, 0.f, FloorZ + Half + 2.f), FVector::ZeroVector, true);
		};
		Place(Giant, GiantX);
		Place(ClientPenguin, GiantX + SizeClientGap);
		Giant->ChangeSize(PinguMaxSize, 30.f);
		Report(FString::Printf(TEXT("size check: platform of %d blocks at z %.0f, \"%s\" growing to 8x at x %.0f, the client's penguin %.0f cm right of it"),
			Added, FloorZ, *Giant->GetDisplayName(), GiantX, SizeClientGap));
	}
	// Both: the giant's size as this machine has it, then (host) one more grow step pops it.
	if (!bSizeSampled && T >= SizeWatchAt)
	{
		bSizeSampled = true;
		SizeSampledAt = WorldTime;
		SizeSeen = Giant->GetSizeScale();
		const UPenguinMovementComponent* ClientMove = ClientPenguin->GetPenguinMovement();
		SizeClientHitsAtStart = ClientMove ? ClientMove->HitEvents : 0;
		Report(FString::Printf(TEXT("size check: \"%s\" at %.2f here, the client's penguin %.0f cm from it"), *Giant->GetDisplayName(), SizeSeen,
			FMath::Abs(static_cast<float>(ClientPenguin->GetActorLocation().X - Giant->GetActorLocation().X))));
	}
	// (Host: the pop waits until the giant has grown all the way here - a slow frame can leave it at 7.9 by SizePopAt.)
	if (bHostRole && bSizeSampled && !bSizePopped && T >= SizePopAt && (Giant->GetSizeScale() >= PinguMaxSize * 0.995f || T >= SizePopAt + 1.f))
	{
		bSizePopped = true;
		Giant->ChangeSize(PinguGrowRayFactor, 12.f, HostPenguin); // past 8x: it pops (the host's knock-out)
		Report(FString::Printf(TEXT("size check: one more grow step - knocked out %d"), Giant->IsKnockedOut() ? 1 : 0));
	}
	return FVector2D::ZeroVector;
}

void APinguNetTest::UpdateSizeWatch()
{
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	APenguinCharacter* Giant = SizeGiant.Get();
	if (!bSizeSampled || P.bSizeJudged || !Giant)
	{
		return;
	}
	// The biggest it got here before it popped (sampled from SizeWatchAt on: at a low frame rate the replicated size can
	// still be catching up at the first sample).
	if (!Giant->IsKnockedOut())
	{
		SizeSeen = FMath::Max(SizeSeen, Giant->GetSizeScale());
	}
	if (Giant->IsKnockedOut())
	{
		if (SizeKOSeenAt < 0.f)
		{
			SizeKOSeenAt = WorldTime;
		}
		if (Giant->GetKOReason() != EPenguinKOReason::None)
		{
			SizeKOReason = static_cast<uint8>(Giant->GetKOReason());
		}
		if (Giant->IsHidden())
		{
			bSizeHiddenSeen = true;
		}
		else
		{
			// Puffed up (its death look: taller than its normal mesh) while it is still there.
			const USkeletalMeshComponent* Mesh = Giant->GetMesh();
			const float BaseZ = static_cast<float>(Giant->GetBaseMeshScale().Z);
			const float Tall = Mesh && BaseZ > 0.f ? static_cast<float>(Mesh->GetRelativeScale3D().Z) / BaseZ : 1.f;
			SizeMaxTall = FMath::Max(SizeMaxTall, Tall);
		}
	}
	const bool bTimeUp = SizeKOSeenAt >= 0.f && WorldTime - SizeKOSeenAt > SizeJudgeAfter;
	const bool bBackIn = SizeKOSeenAt >= 0.f && !Giant->IsKnockedOut();
	const bool bNever = SizeKOSeenAt < 0.f && WorldTime - SizeSampledAt > SizeGiveUpAfter;
	if (!bTimeUp && !bBackIn && !bNever)
	{
		return;
	}
	P.bSizeJudged = true;
	const APenguinCharacter* ClientPenguin = bHostRole ? GetOtherPlayerPenguin() : GetOwnPenguin();
	const UPenguinMovementComponent* ClientMove = ClientPenguin ? ClientPenguin->GetPenguinMovement() : nullptr;
	const int32 Pushed = ClientMove ? ClientMove->HitEvents - SizeClientHitsAtStart : 0;
	P.bSizeOk = SizeSeen > 7.8f && SizeKOReason == static_cast<uint8>(EPenguinKOReason::Overgrown) && bSizeHiddenSeen && SizeMaxTall > 1.1f && Pushed >= 1;
	P.SizeDetail = FString::Printf(TEXT("the bot at %.2f here (want 8), knocked out %d with reason %s (want Overgrown), puffed up to %.2f of its height (want over 1.1), "
		"gone %d, the client's penguin pushed by the blast %d times (want 1 or more)"),
		SizeSeen, SizeKOSeenAt >= 0.f ? 1 : 0, *UEnum::GetValueAsString(static_cast<EPenguinKOReason>(SizeKOReason)), SizeMaxTall, bSizeHiddenSeen ? 1 : 0, Pushed);
	Report(FString::Printf(TEXT("size check: %s (%s)"), P.bSizeOk ? TEXT("ok") : TEXT("FAIL"), *P.SizeDetail));
}

void APinguNetTest::ReleaseHold()
{
	// Everybody moves again (our own brain: DriveOwnPenguin).
	if (APenguinCharacter* Own = GetOwnPenguin())
	{
		Own->ApplyVirtualMoveInput(FVector2D::ZeroVector);
	}
	for (const TWeakObjectPtr<APenguinCharacter>& Held : HeldBots)
	{
		APenguinCharacter* Penguin = Held.Get();
		if (UPenguinBotComponent* Bot = Penguin ? Penguin->GetBot() : nullptr)
		{
			Bot->SetBotEnabled(true, 1);
		}
	}
	HeldBots.Reset();
}

APenguinCharacter* APinguNetTest::GetOtherPlayerPenguin() const
{
	const APenguinCharacter* Own = GetOwnPenguin();
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		const APlayerState* PlayerState = Penguin->GetPlayerState();
		if (Penguin != Own && PlayerState && !PlayerState->IsABot())
		{
			return Penguin;
		}
	}
	return nullptr;
}

APenguinCharacter* APinguNetTest::GetOwnPenguin() const
{
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const APlayerController* PlayerController = GameInstance ? GameInstance->GetFirstLocalPlayerController(World) : nullptr;
	return PlayerController ? Cast<APenguinCharacter>(PlayerController->GetPawn()) : nullptr;
}

int32 APinguNetTest::CountPenguins() const
{
	int32 Count = 0;
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		++Count;
	}
	return Count;
}

// ---------------------------------------------------------------------------- results

void APinguNetTest::LogResults(const FString& Why)
{
	using namespace PinguNetTestPrivate;
	FProgress& P = Progress();
	if (P.bResultsLogged)
	{
		return;
	}
	P.bResultsLogged = true;
	int32 Checks = 0;
	int32 Passed = 0;
	const auto Check = [this, &Checks, &Passed](const TCHAR* Label, bool bOk, const FString& Detail)
	{
		++Checks;
		Passed += bOk ? 1 : 0;
		Report(FString::Printf(TEXT("RESULT %s: %s (%s)"), Label, bOk ? TEXT("ok") : TEXT("FAIL"), *Detail));
	};

	if (IsCheatRun())
	{
		// A cheat run (PinguNetTestAntiCheat.cpp): only whether the anti-cheat did its job.
		AddAntiCheatChecks(Check);
		AddHostAuthorityChecks(Check); // the movement cheats (PinguNetTestHostAuthority.cpp)
		Report(FString::Printf(TEXT("RESULT %s: %s (%d/%d checks; %s; %.0f s)"), bHostRole ? TEXT("host") : TEXT("client"),
			Passed == Checks ? TEXT("ok") : TEXT("FAIL"), Passed, Checks, *Why, FPlatformTime::Seconds() - P.StartTime));
		return;
	}
	if (P.bLate)
	{
		// The late join: in during the match, a penguin from a later round, two teams.
		if (bHostRole)
		{
			Check(TEXT("client joined during the match"), P.bClientJoined && P.bClientJoinedMidMatch,
				P.bClientJoined ? FString::Printf(TEXT("%d players, during the match %d"), P.MaxPlayers, P.bClientJoinedMidMatch ? 1 : 0) : FString(TEXT("nobody joined")));
			Check(TEXT("rounds played"), P.RoundsFought >= MinRounds, FString::Printf(TEXT("%d, want %d or more"), P.RoundsFought, MinRounds));
			Check(TEXT("match finished"), P.bMatchOver, P.bMatchOver ? FString::Printf(TEXT("winner slot %d"), P.MatchWinnerSlot) : FString(TEXT("no match over")));
			Check(TEXT("penguins 3 (the client's came)"), P.MaxPenguins == ExpectedPenguins, FString::Printf(TEXT("%d in the match"), P.MaxPenguins));
			Check(TEXT("2 teams"), P.TeamsInMatch == 2, FString::Printf(TEXT("%d"), P.TeamsInMatch));
		}
		else
		{
			Check(TEXT("connected"), P.bConnected, P.bConnected ? FString::Printf(TEXT("%s, attempt %d"), *P.Address, P.ConnectedAttempt)
				: FString::Printf(TEXT("%d attempts, last error \"%s\""), P.JoinAttempts, *P.LastJoinError));
			// Joining in the gap between two rounds, the host admits us at once: then only the feed shows that we waited.
			Check(TEXT("waited for the next round"), P.bSawWaiting || P.bSawWaitingFeed,
				P.bSawWaiting ? FString(TEXT("yes")) : (P.bSawWaitingFeed ? FString(TEXT("yes, the feed said so (admitted between two rounds)")) : FString(TEXT("never waiting"))));
			Check(TEXT("own penguin from a later round"), P.OwnPenguinRound >= 1, FString::Printf(TEXT("round %d"), P.OwnPenguinRound));
			Check(TEXT("own penguin moved"), P.OwnDistance > MinOwnDistance, FString::Printf(TEXT("%.0f cm, want more than %.0f"), P.OwnDistance, MinOwnDistance));
			Check(TEXT("used abilities"), P.OwnAbilityUses > 0, FString::Printf(TEXT("%d"), P.OwnAbilityUses));
			Check(TEXT("own team"), P.OwnTeam >= 0, FString::Printf(TEXT("team %d of %d"), P.OwnTeam, P.TeamsInMatch));
		}
		// Both: whoever is alone in a team shows on its own here too (from the replicated score rows).
		Check(TEXT("teams as shown: a team of two, the bot on its own"), P.bTeamViewOk, P.bTeamViewSeen ? P.TeamViewDetail : FString(TEXT("no fight with all three seen")));
		AddIslandChecks(Check); // the island's size (PinguNetTestIsland.cpp)
		AddIceScaleChecks(Check); // big blocks in the island the late joiner gets (PinguNetTestIceScale.cpp; not with traits)
		AddAntiCheatChecks(Check); // the join check, the deals recomputed, no false alarm (PinguNetTestAntiCheat.cpp)
		if (IsTraitsRun())
		{
			AddTraitsChecks(Check); // the late join in Traits mode (PinguNetTestTraits.cpp)
		}
		Report(FString::Printf(TEXT("RESULT %s: %s (%d/%d checks; %s; %.0f s; feed lines %d, projectiles %d, pickups %d)"),
			bHostRole ? TEXT("host") : TEXT("client"), Passed == Checks ? TEXT("ok") : TEXT("FAIL"), Passed, Checks, *Why,
			FPlatformTime::Seconds() - P.StartTime, P.FeedLines, P.ProjectilesSeen, P.PickupsSeen));
		return;
	}
	if (bHostRole)
	{
		Check(TEXT("client joined"), P.bClientJoined, P.bClientJoined ? FString::Printf(TEXT("%d players"), P.MaxPlayers) : FString(TEXT("nobody joined")));
		Check(TEXT("rounds played"), P.RoundsFought >= MinRounds, FString::Printf(TEXT("%d, want %d or more"), P.RoundsFought, MinRounds));
		Check(TEXT("match finished"), P.bMatchOver, P.bMatchOver
			? FString::Printf(TEXT("winner slot %d \"%s\""), P.MatchWinnerSlot, *P.MatchWinnerName) : FString(TEXT("no match over")));
		Check(TEXT("penguins 3"), P.MaxPenguins == ExpectedPenguins, FString::Printf(TEXT("%d in the match"), P.MaxPenguins));
	}
	else
	{
		Check(TEXT("found host beacon"), P.FoundGames.Num() > 0, P.FoundGames.Num() > 0
			? FString::Join(P.FoundGames, TEXT(", ")) : FString::Printf(TEXT("no LAN game heard on UDP %d"), UPinguNetSubsystem::BeaconPort));
		Check(TEXT("connected"), P.bConnected, P.bConnected
			? FString::Printf(TEXT("%s, attempt %d"), *P.Address, P.ConnectedAttempt)
			: FString::Printf(TEXT("%d attempts, last error \"%s\""), P.JoinAttempts, *P.LastJoinError));
		Check(TEXT("saw 3 penguins"), P.MaxPenguins == ExpectedPenguins, FString::Printf(TEXT("%d in the match"), P.MaxPenguins));
		Check(TEXT("own penguin moved"), P.OwnDistance > MinOwnDistance, FString::Printf(TEXT("%.0f cm, want more than %.0f"), P.OwnDistance, MinOwnDistance));
		Check(TEXT("used abilities"), P.OwnAbilityUses > 0, FString::Printf(TEXT("%d"), P.OwnAbilityUses));
		Check(TEXT("saw rounds"), P.RoundsFought >= MinRounds, FString::Printf(TEXT("%d, want %d or more"), P.RoundsFought, MinRounds));
		Check(TEXT("block counts logged"), P.BlockCountsLogged >= MinRounds && P.BlockCountsLogged == P.RoundsFought,
			FString::Printf(TEXT("%d of %d fights"), P.BlockCountsLogged, P.RoundsFought));
		Check(TEXT("endless match ended by the host, no END MATCH here"), P.bClientSawEndless && P.bMatchOver && !P.bClientCouldEndMatch,
			FString::Printf(TEXT("endless %d, results %d, END MATCH offered %d"), P.bClientSawEndless ? 1 : 0, P.bMatchOver ? 1 : 0, P.bClientCouldEndMatch ? 1 : 0));
		Check(TEXT("the others' knock-outs play their death looks here"), P.RemoteKOs > 0 && P.RemoteKOsLookOn == P.RemoteKOs && P.RemoteKOsShaped == P.RemoteKOs,
			FString::Printf(TEXT("%d knock-outs, look on with its reason %d, looked like it %d: %s"), P.RemoteKOs, P.RemoteKOsLookOn, P.RemoteKOsShaped,
				P.DeathDetail.IsEmpty() ? TEXT("none seen") : *P.DeathDetail));
	}
	if (IsTraitsRun())
	{
		// Traits mode (PinguNetTestTraits.cpp) instead of the slide, stun, hook, pad, throw and size checks.
		AddTraitsChecks(Check);
		AddAntiCheatChecks(Check); // the traits dealt from the shared seed too (PinguNetTestAntiCheat.cpp)
		Report(FString::Printf(TEXT("RESULT %s: %s (%d/%d checks; %s; %.0f s; feed lines %d, projectiles %d, pickups %d)"),
			bHostRole ? TEXT("host") : TEXT("client"), Passed == Checks ? TEXT("ok") : TEXT("FAIL"), Passed, Checks, *Why,
			FPlatformTime::Seconds() - P.StartTime, P.FeedLines, P.ProjectilesSeen, P.PickupsSeen));
		return;
	}
	// Both machines: the belly slide over LAN, each way; the slap's stun and the ice shield on the client's penguin.
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Check(Index == 0 ? TEXT("belly slide client into host") : TEXT("belly slide host into client"), P.bSlideOk[Index],
			P.bSlideJudged[Index] ? P.SlideDetail[Index] : FString(TEXT("not run")));
	}
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Check(Index == 0 ? TEXT("slap stun on the client's penguin") : TEXT("ice shield on the client's penguin"), P.bEffectOk[Index],
			P.bEffectJudged[Index] ? P.EffectDetail[Index] : FString(TEXT("not run")));
	}
	Check(TEXT("ability in a knockback flight on the client's penguin"), P.bKnockOk, P.bKnockJudged ? P.KnockDetail : FString(TEXT("not run")));
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Check(Index == 0 ? TEXT("hook pulls an ice block out") : TEXT("hook yanks the client's ice cube"), P.bHookOk[Index],
			P.bHookJudged[Index] ? P.HookDetail[Index] : FString(TEXT("not run")));
	}
	Check(TEXT("icicle in the ground throws the client's penguin up"), P.bPadOk, P.bPadJudged ? P.PadDetail : FString(TEXT("not run")));
	Check(TEXT("icicle: the other's charge seen, held longer flies further (full, tap, half a second), the chargers face their aim"), P.bThrowOk,
		P.bThrowJudged ? P.ThrowDetail : FString(TEXT("not run")));
	Check(TEXT("a giant pops: 8x seen here, knocked out Overgrown, its blast pushes the client's penguin"), P.bSizeOk,
		P.bSizeJudged ? P.SizeDetail : FString(TEXT("not run")));
	AddTornadoChecks(Check); // round 1's tornado (PinguNetTestTornado.cpp)
	AddSelfBombChecks(Check); // round 1's last part: a bomb knocks out its own thrower (PinguNetTestSelfBomb.cpp)
	AddIceScaleChecks(Check); // round 2's last part: the ice wall solid at once, the big iceberg (PinguNetTestIceScale.cpp)
	AddGiantChecks(Check); // round 2's last part: a giant snowball swallows the client's penguin (PinguNetTestGiant.cpp)
	AddSnowmanChecks(Check); // round 2's very last part: a snowman throws at the client's penguin (PinguNetTestSnowman.cpp)
	AddIslandChecks(Check); // the island's size, the camera, Snow Warp on screen (PinguNetTestIsland.cpp)
	AddLobbyIslandChecks(Check); // the lobby island: the other's penguin, its puff, the name tags (PinguNetTestLobby.cpp)
	AddAntiCheatChecks(Check); // the join check, the deals recomputed, nothing of the client's ignored (PinguNetTestAntiCheat.cpp)
	AddHostAuthorityChecks(Check); // the host's hand-backs, no correction the client could feel (PinguNetTestHostAuthority.cpp)
	PinguNetTestKillCam::AddChecks(Check); // every round's last knock-out in slow motion on both, back in step (PinguNetTestKillCam.cpp)
	Report(FString::Printf(TEXT("RESULT %s: %s (%d/%d checks; %s; %.0f s; feed lines %d, projectiles %d, pickups %d)"),
		bHostRole ? TEXT("host") : TEXT("client"), Passed == Checks ? TEXT("ok") : TEXT("FAIL"), Passed, Checks, *Why,
		FPlatformTime::Seconds() - P.StartTime, P.FeedLines, P.ProjectilesSeen, P.PickupsSeen));
}

void APinguNetTest::Finish(const FString& Why)
{
	if (bQuitRequested)
	{
		return;
	}
	bQuitRequested = true;
	Report(FString::Printf(TEXT("finish: %s"), *Why));
	LogResults(Why);
	PinguEndTestRun(this, TEXT("PinguNetTest"));
}
