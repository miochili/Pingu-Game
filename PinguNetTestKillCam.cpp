// Pingu - the LAN test's kill cam check. See PinguNetTestKillCam.h.

#include "PinguNetTestKillCam.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/App.h"
#include "Penguin.h"
#include "PenguinGameState.h"
#include "PinguKillCam.h"

namespace PinguNetTestKillCamPrivate
{
	/** s: a joiner's server time offset may move this much from the fight to the banner. */
	constexpr double MaxDrift = 0.1;
	/** s of real time into RoundOver when the round's end is judged. */
	constexpr float JudgeAfter = 0.3f;

	struct FState
	{
		bool bHost = false;
		EPinguPhase LastPhase = EPinguPhase::Menu;
		int32 LastShowcases = 0;
		float RoundLowest = 1.f;
		double FightOffset = 0.0;
		bool bFightOffsetKnown = false;
		float RoundOverTime = 0.f;
		bool bRoundJudged = false;
		int32 RoundEnds = 0;
		int32 SlowEnds = 0;
		int32 BackToNormal = 0;
		int32 InSync = 0;
		float WorstLowest = 1.f;
		double WorstDrift = 0.0;
		FString Lines;
	};

	FState& State()
	{
		static FState S;
		return S;
	}

	float Dilation(const UWorld* World)
	{
		const AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
		return Settings ? Settings->GetEffectiveTimeDilation() : 1.f;
	}
}

void PinguNetTestKillCam::Tick(const UWorld* World, bool bHost)
{
	using namespace PinguNetTestKillCamPrivate;
	FState& S = State();
	S.bHost = bHost;
	const APenguinGameState* GameState = World ? APenguinGameState::Get(World) : nullptr;
	if (!GameState)
	{
		return;
	}
	const EPinguPhase Phase = GameState->GetPhase();
	// The client's server time offset (the host's own is 0).
	const double Offset = GameState->GetServerWorldTimeSeconds() - World->GetTimeSeconds();
	if (Phase == EPinguPhase::Fight)
	{
		if (S.LastPhase != EPinguPhase::Fight)
		{
			S.RoundLowest = 1.f;
			S.bFightOffsetKnown = false;
			S.bRoundJudged = false;
			if (const APinguKillCam* KillCam = APinguKillCam::Get(World))
			{
				S.LastShowcases = KillCam->GetTestStats().Showcases;
			}
		}
		if (!S.bFightOffsetKnown || S.RoundLowest >= 1.f)
		{
			S.FightOffset = Offset; // (the last value before the slow motion)
			S.bFightOffsetKnown = true;
		}
		S.RoundLowest = FMath::Min(S.RoundLowest, Dilation(World));
	}
	else if (Phase == EPinguPhase::RoundOver)
	{
		if (S.LastPhase != EPinguPhase::RoundOver)
		{
			S.RoundOverTime = 0.f;
		}
		S.RoundOverTime += FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.25f);
		if (!S.bRoundJudged && S.RoundOverTime >= JudgeAfter)
		{
			S.bRoundJudged = true;
			const APinguKillCam* KillCam = APinguKillCam::Get(World);
			const int32 Showcases = KillCam ? KillCam->GetTestStats().Showcases : 0;
			const bool bSlow = Showcases > S.LastShowcases && S.RoundLowest <= 0.12f;
			const float Now = Dilation(World);
			const double Drift = S.bFightOffsetKnown ? FMath::Abs(Offset - S.FightOffset) : 0.0;
			++S.RoundEnds;
			S.SlowEnds += bSlow ? 1 : 0;
			S.BackToNormal += FMath::IsNearlyEqual(Now, 1.f) ? 1 : 0;
			S.InSync += Drift < MaxDrift ? 1 : 0;
			S.WorstLowest = FMath::Min(S.WorstLowest, S.RoundLowest);
			S.WorstDrift = FMath::Max(S.WorstDrift, Drift);
			const FString Line = FString::Printf(TEXT("round %d: slow motion %d (lowest %.3fx), %.2fx at the banner, the server clock offset moved %.0f ms"),
				GameState->GetRoundNumber(), bSlow ? 1 : 0, S.RoundLowest, Now, Drift * 1000.0);
			S.Lines += (S.Lines.IsEmpty() ? TEXT("") : TEXT("; ")) + Line;
			UE_LOG(LogPingu, Display, TEXT("[NETTEST] kill cam %s: %s"), bHost ? TEXT("host") : TEXT("client"), *Line);
		}
	}
	S.LastPhase = Phase;
}

void PinguNetTestKillCam::AddChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestKillCamPrivate;
	const FState& S = State();
	const bool bOk = S.RoundEnds >= 1 && S.SlowEnds == S.RoundEnds && S.BackToNormal == S.RoundEnds && S.InSync == S.RoundEnds;
	Check(TEXT("kill cam: every round's last knock-out in slow motion here, 1x at the banner, the clock with the host's"), bOk,
		FString::Printf(TEXT("%d round ends, %d slowed, %d back at 1x, %d in sync (worst lowest %.3fx, drift %.0f ms): %s"), S.RoundEnds, S.SlowEnds,
			S.BackToNormal, S.InSync, S.WorstLowest, S.WorstDrift * 1000.0, *S.Lines));
}
