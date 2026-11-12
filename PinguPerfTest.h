// Pingu - automated performance measurement (run the game with -PinguPerfTest).
//
// CONTRACT (v2): APenguinGameMode spawns one when the command line has -PinguPerfTest. It plays a bot match on every
// graphics preset in turn and logs [PERFTEST] RESULT lines (frame time, GPU time, draw calls, triangles), then quits.
//
// Per preset (Low, Medium, High, Ultra): apply it (frame cap and vsync off, so the numbers are the real cost), warm up
// 5 s (longer while shaders compile, up to 5 minutes), measure 10 s, log
//   [PERFTEST] RESULT <preset>: avg X ms (Y fps), p95 Z ms, gpu G ms, game T ms, draws D, prims P
// (+ a DETAIL line), and take a screenshot to Saved/ClaudeLogs/Perf/<preset>.png. Then the player's settings come back
// (nothing is ever saved on a test run) and the game quits (Play In Editor: the session ends).
// The numbers come from the engine's performance tracking (UEngine performance data consumers): frame time, game thread
// time, GPU time (RHI GPU frame cycles), draw calls and primitives (RHI counters).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PinguSettings.h"
#include "PinguPerfTest.generated.h"

class FPinguPerfChart;

UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguPerfTest : public AActor
{
	GENERATED_BODY()

public:
	APinguPerfTest();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	enum class EStage : uint8
	{
		StartMatch,  // let the game mode settle, then a bots-only endless match
		Apply,       // the next preset
		Warmup,
		Measure,
		Screenshot,
		Done,
	};

	void StartBotMatch();
	/** Every penguin plays by itself (also the local player's) and the game is never paused. */
	void KeepBotsPlaying();
	void ApplyCurrentPreset();
	void BeginMeasure();
	void EndMeasure();
	void Finish(bool bSuccess, const FString& Why);
	void RestorePlayerSettings();
	void Report(const FString& Message) const;
	EPinguGraphicsPreset GetCurrentPreset() const;
	const TCHAR* GetCurrentPresetName() const;

	/** Our performance data consumer (registered with the engine while the test runs). */
	TSharedPtr<FPinguPerfChart> Chart;
	EStage Stage = EStage::StartMatch;
	int32 PresetIndex = 0;
	/** Real (undilated) seconds: hit-stop and slow motion must not stretch the measurement. */
	double TotalSeconds = 0.0;
	double StageSeconds = 0.0;
	double BotCheckSeconds = 0.0;
	double ShaderLogSeconds = 0.0;
	/** When StartBotMatch last ran (a match that ends anyway is restarted, at most every few seconds). */
	double MatchStartSeconds = -100.0;
	int32 MatchRestarts = 0;
	int32 StageFrames = 0;
	int32 FramesCompilingShaders = 0;
	/** The engine does not hand frames to performance consumers here (some editor paths): we tick it ourselves. */
	bool bTickMonitoringOurselves = false;
	int32 TicksSinceRegistered = 0;
	bool bFinished = false;
	/** Last resort when no frame data arrives at all: frame times from our own tick. */
	TArray<float> TickFrameMs;
	TArray<FString> SummaryLines;

	FPinguGraphicsOptions PlayerGraphics;
	int32 PlayerFrameRateLimit = 0;
	bool bPlayerVSync = false;
	bool bHavePlayerSettings = false;

	/** -PinguPerfPresets=Low,High: only these presets (indices into the test's preset table), in this order. */
	TArray<int32> PresetOrder;
	/** -PinguPerfProfile: one GPU profile (the engine's ProfileGPU, per pass, into the log) during each preset. */
	bool bProfileGPU = false;
	bool bProfiledThisPreset = false;

	/** -PinguAutoResTest: Low's adaptive resolution (UPinguSettings::GetAutoStep) with a pretend-slow graphics chip -
	 *  slow for a while (it must go all the way down), then fast again (it must come back to Low as it is). Replaces the
	 *  preset measurements. */
	void TickAutoResTest(double RealDelta);
	bool bAutoResTest = false;
	double AutoResSeconds = -1.0;   // since the test's Low was applied (-1: not yet)
	double AutoResDownAt = -1.0;    // when the last step was reached
	double AutoResUpAt = -1.0;      // when it was back at step 0 after the slow part
	int32 AutoResLastStep = 0;
};
