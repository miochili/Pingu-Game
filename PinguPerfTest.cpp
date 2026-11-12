// Pingu - automated performance measurement (-PinguPerfTest), see PinguPerfTest.h.

#include "PinguPerfTest.h"
#include "Penguin.h"
#include "PenguinBot.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "ChartCreation.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ShaderCompiler.h"
#include "UObject/Class.h"
#include "UnrealClient.h"

namespace PinguPerfTestPrivate
{
	constexpr double StartDelaySeconds = 1.5;
	constexpr double WarmupSeconds = 5.0;
	/** A first run compiles shaders for every preset (Low materials, no-Lumen permutations, ...): wait, not forever. */
	constexpr double MaxShaderWaitSeconds = 300.0;
	constexpr double MeasureSeconds = 10.0;
	/** The screenshot is written a frame or two after the request; the next preset must not end up in it. */
	constexpr double ScreenshotSeconds = 1.0;
	constexpr double MatchRestartCooldownSeconds = 5.0;
	/** Four presets take about 1.5 minutes, plus up to MaxShaderWaitSeconds each on a cold shader cache: never hang a test
	 *  machine beyond that. */
	constexpr double WatchdogSeconds = 1500.0;

	const EPinguGraphicsPreset Presets[] = { EPinguGraphicsPreset::Low, EPinguGraphicsPreset::Medium, EPinguGraphicsPreset::High, EPinguGraphicsPreset::Ultra };
	/** Fixed names (log lines and screenshot files never depend on the UI language). */
	const TCHAR* const PresetNames[] = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Ultra") };
	constexpr int32 NumPresets = UE_ARRAY_COUNT(Presets);

	/** Logged with each preset: what it really switched (a developer override on the command line shows up here). */
	const TCHAR* const ReportedCVars[] = { TEXT("r.ScreenPercentage"), TEXT("r.AntiAliasingMethod"), TEXT("r.DynamicGlobalIlluminationMethod"),
		TEXT("r.ReflectionMethod"), TEXT("r.Shadow.Virtual.Enable"), TEXT("r.ShadowQuality"), TEXT("r.Shadow.CSM.MaxCascades"),
		TEXT("r.Lumen.HardwareRayTracing"), TEXT("r.MaterialQualityLevel"), TEXT("r.SkeletalMeshLODBias"), TEXT("r.PostProcessAAQuality"),
		TEXT("r.AmbientOcclusionLevels"), TEXT("t.MaxFPS"), TEXT("r.VSync") };

	static float Average(const TArray<float>& Values)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		double Sum = 0.0;
		for (const float Value : Values)
		{
			Sum += Value;
		}
		return static_cast<float>(Sum / Values.Num());
	}

	/** Nearest-rank percentile (0.95 = the frame time 95 % of the frames are faster than). */
	static float Percentile(TArray<float> Values, float Fraction)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		Values.Sort();
		const int32 Index = FMath::Clamp(FMath::CeilToInt(Fraction * static_cast<float>(Values.Num())) - 1, 0, Values.Num() - 1);
		return Values[Index];
	}

	static bool IsCompilingShaders()
	{
		return GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
	}
}

/**
 * One measurement: the engine's performance chart (game / render / GPU time, draw calls and primitives from the RHI
 * counters - all computed inside the engine, so this module needs no RHI dependency) plus every frame time for the
 * 95th percentile.
 */
class FPinguPerfChart : public FPerformanceTrackingChart
{
public:
	FPinguPerfChart()
		: FPerformanceTrackingChart(FDateTime::Now(), TEXT("PinguPerfTest"))
	{
	}

	virtual void ProcessFrame(const FFrameData& FrameData) override
	{
		++FramesSeen;
		if (!bRecording)
		{
			return;
		}
		FPerformanceTrackingChart::ProcessFrame(FrameData);
		if (FrameData.bBinThisFrame)
		{
			FrameMs.Add(static_cast<float>(FrameData.DeltaSeconds * 1000.0));
			GpuMs.Add(static_cast<float>(FrameData.GPUTimeSeconds * 1000.0));
		}
	}

	void BeginRecording()
	{
		Reset(FDateTime::Now());
		FrameMs.Reset();
		GpuMs.Reset();
		bRecording = true;
	}

	void EndRecording()
	{
		bRecording = false;
	}

	/** Every frame the engine handed us (recording or not): tells whether the engine feeds consumers at all. */
	int64 FramesSeen = 0;
	bool bRecording = false;
	TArray<float> FrameMs;
	TArray<float> GpuMs;
};

APinguPerfTest::APinguPerfTest()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
}

void APinguPerfTest::BeginPlay()
{
	Super::BeginPlay();

	FVector2D ViewportSize = FVector2D::ZeroVector;
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(ViewportSize);
	}
	Report(FString::Printf(TEXT("start: GPU \"%s\", CPU \"%s\" (%d threads), RHI %s, viewport %dx%d, %s"),
		*FPlatformMisc::GetPrimaryGPUBrand(), *FPlatformMisc::GetCPUBrand().TrimStartAndEnd(), FPlatformMisc::NumberOfCoresIncludingHyperthreads(),
		*FApp::GetGraphicsRHI(), FMath::RoundToInt32(ViewportSize.X), FMath::RoundToInt32(ViewportSize.Y),
		GIsEditor ? TEXT("Play In Editor") : TEXT("game")));
	// The benchmark index next to the measured frame times is what tunes the first-start auto-detection.
	float GPUIndex = -1.f;
	float CPUIndex = -1.f;
	const EPinguGraphicsPreset Recommended = UPinguSettings::BenchmarkRecommendedPreset(GPUIndex, CPUIndex);
	Report(FString::Printf(TEXT("hardware benchmark: GPU index %.0f, CPU index %.0f -> a first start on this machine picks %s"),
		GPUIndex, CPUIndex, *UPinguSettings::GetPresetName(Recommended).ToString()));

	// The player's settings come back at the end (and a test run never saves anything, see UPinguSettings::Save).
	UPinguSettings* Settings = UPinguSettings::Get();
	PlayerGraphics = Settings->GetGraphics();
	PlayerFrameRateLimit = Settings->GetFrameRateLimit();
	bPlayerVSync = Settings->GetVSync();
	bHavePlayerSettings = true;
	// The real cost of every preset: no frame cap (Low has one), no vsync.
	Settings->SetVSync(false);
	Settings->SetFrameRateLimit(0);

	Chart = MakeShared<FPinguPerfChart>();
	if (GEngine)
	{
		GEngine->AddPerformanceDataConsumer(Chart);
	}

	// Which presets (default all four), and whether to dump a per-pass GPU profile of each.
	using namespace PinguPerfTestPrivate;
	PresetOrder.Reset();
	FString PresetList;
	if (FParse::Value(FCommandLine::Get(), TEXT("PinguPerfPresets="), PresetList, false)) // (false: the list has commas)
	{
		TArray<FString> Names;
		PresetList.ParseIntoArray(Names, TEXT(","), true);
		for (const FString& Name : Names)
		{
			for (int32 Index = 0; Index < NumPresets; ++Index)
			{
				if (Name.TrimStartAndEnd().Equals(PresetNames[Index], ESearchCase::IgnoreCase))
				{
					PresetOrder.Add(Index);
				}
			}
		}
	}
	if (PresetOrder.Num() == 0)
	{
		for (int32 Index = 0; Index < NumPresets; ++Index)
		{
			PresetOrder.Add(Index);
		}
	}
	bAutoResTest = FParse::Param(FCommandLine::Get(), TEXT("PinguAutoResTest"));
	bProfileGPU = FParse::Param(FCommandLine::Get(), TEXT("PinguPerfProfile"));
	if (bProfileGPU)
	{
		// The profile goes to the log only: no profiler window, and no screenshot of its own (ProfileGPU's screenshot
		// next to ours crashed the D3D12 back buffer copy).
		for (const TCHAR* Name : { TEXT("r.ProfileGPU.ShowUI"), TEXT("r.ProfileGPU.Screenshot") })
		{
			if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
			{
				CVar->Set(0, ECVF_SetByCode);
			}
		}
		Report(TEXT("GPU profiles on: one ProfileGPU dump per preset, 4 s into its measurement"));
	}
}

void APinguPerfTest::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Chart.IsValid())
	{
		if (GEngine)
		{
			GEngine->RemovePerformanceDataConsumer(Chart);
		}
		Chart.Reset();
	}
	// Stopped early. Play In Editor: UPinguSettings gives the editor its own render settings back when the session
	// ends (that happens before this; applying the player's options now would undo it).
	if (!GIsEditor)
	{
		RestorePlayerSettings();
	}
	Super::EndPlay(EndPlayReason);
}

void APinguPerfTest::Tick(float DeltaSeconds)
{
	using namespace PinguPerfTestPrivate;
	Super::Tick(DeltaSeconds);
	if (bFinished)
	{
		return;
	}
	// Real time: hit-stop and slow motion dilate DeltaSeconds.
	const double RealDelta = FMath::Clamp(FApp::GetDeltaTime(), 0.0, 1.0);
	TotalSeconds += RealDelta;
	StageSeconds += RealDelta;
	++StageFrames;

	if (TotalSeconds > WatchdogSeconds)
	{
		Finish(false, FString::Printf(TEXT("watchdog: %s still not done after %.0f s (stage %d)"), GetCurrentPresetName(), TotalSeconds,
			static_cast<int32>(Stage)));
		return;
	}

	// UGameEngine hands every frame to the performance data consumers; where nobody does (some editor paths), we do.
	if (Chart.IsValid() && !bTickMonitoringOurselves && Chart->FramesSeen == 0 && ++TicksSinceRegistered > 30)
	{
		bTickMonitoringOurselves = true;
		Report(TEXT("the engine does not pass frames to performance data consumers here: the test ticks the performance monitoring itself"));
	}
	if (bTickMonitoringOurselves && GEngine)
	{
		GEngine->TickPerformanceMonitoring(static_cast<float>(RealDelta));
	}

	BotCheckSeconds += RealDelta;
	if (BotCheckSeconds >= 0.5)
	{
		BotCheckSeconds = 0.0;
		KeepBotsPlaying();
	}

	switch (Stage)
	{
	case EStage::StartMatch:
		// The game mode puts up its menu right after spawning us: start the match once that has settled.
		if (StageSeconds >= StartDelaySeconds)
		{
			StartBotMatch();
			PresetIndex = 0;
			Stage = EStage::Apply;
		}
		break;

	case EStage::Apply:
		if (bAutoResTest)
		{
			TickAutoResTest(RealDelta);
			break;
		}
		ApplyCurrentPreset();
		Stage = EStage::Warmup;
		StageSeconds = 0.0;
		StageFrames = 0;
		ShaderLogSeconds = 0.0;
		break;

	case EStage::Warmup:
	{
		const bool bCompiling = IsCompilingShaders();
		if (bCompiling && StageSeconds - ShaderLogSeconds >= 10.0)
		{
			ShaderLogSeconds = StageSeconds;
			Report(FString::Printf(TEXT("%s: waiting for %d shaders to compile (%.0f s)"), GetCurrentPresetName(),
				GShaderCompilingManager->GetNumRemainingJobs(), StageSeconds));
		}
		const bool bShaderWaitOver = StageSeconds >= WarmupSeconds + MaxShaderWaitSeconds;
		if (StageSeconds >= WarmupSeconds && (!bCompiling || bShaderWaitOver))
		{
			if (bCompiling)
			{
				Report(FString::Printf(TEXT("WARN %s: shaders still compiling after %.0f s, measuring anyway"), GetCurrentPresetName(), StageSeconds));
			}
			Report(FString::Printf(TEXT("%s: warm-up done after %.1f s (%d frames)"), GetCurrentPresetName(), StageSeconds, StageFrames));
			BeginMeasure();
		}
		break;
	}

	case EStage::Measure:
		FramesCompilingShaders += IsCompilingShaders() ? 1 : 0;
		TickFrameMs.Add(static_cast<float>(RealDelta * 1000.0));
		if (bProfileGPU && !bProfiledThisPreset && StageSeconds >= 4.0 && GEngine)
		{
			bProfiledThisPreset = true;
			Report(FString::Printf(TEXT("PROFILE %s: GPU profile of the next frame follows (ProfileGPU)"), GetCurrentPresetName()));
			GEngine->Exec(GetWorld(), TEXT("ProfileGPU"));
		}
		if (StageSeconds >= MeasureSeconds)
		{
			EndMeasure();
		}
		break;

	case EStage::Screenshot:
		if (StageSeconds >= ScreenshotSeconds && StageFrames >= 3)
		{
			++PresetIndex;
			StageSeconds = 0.0;
			StageFrames = 0;
			if (PresetIndex < PresetOrder.Num())
			{
				Stage = EStage::Apply;
			}
			else
			{
				Finish(true, FString());
			}
		}
		break;

	default:
		break;
	}
}

void APinguPerfTest::TickAutoResTest(double RealDelta)
{
	using namespace PinguPerfTestPrivate;
	// Slow for 30 s (16 ms more GPU time than real: the 60 fps budget is blown at every step), then the real GPU time.
	constexpr double SlowSeconds = 30.0;
	constexpr double ExtraGpuMs = 16.0;
	constexpr double MaxDownSeconds = 25.0;
	constexpr double MaxUpSeconds = 75.0;
	UPinguSettings* Settings = UPinguSettings::Get();
	if (AutoResSeconds < 0.0)
	{
		AutoResSeconds = 0.0;
		Settings->ApplyPreset(EPinguGraphicsPreset::Low); // (a test run saves nothing) - with Low's 60 fps cap
		Settings->ApplyGraphics(GetWorld());
		UPinguSettings::SetAutoResTestExtraGpuMs(ExtraGpuMs);
		Report(FString::Printf(TEXT("autores: Low applied, the GPU now pretends to need %.0f ms more per frame for %.0f s"), ExtraGpuMs, SlowSeconds));
		return;
	}
	const double Before = AutoResSeconds;
	AutoResSeconds += RealDelta;
	const int32 Step = Settings->GetAutoStep();
	if (Step != AutoResLastStep)
	{
		Report(FString::Printf(TEXT("autores: step %d -> %d after %.1f s (3D %d%%)"), AutoResLastStep, Step, AutoResSeconds,
			Settings->GetEffectiveGraphics().ResolutionScale));
		AutoResLastStep = Step;
	}
	if (AutoResDownAt < 0.0 && Step == UPinguSettings::NumAutoSteps - 1)
	{
		AutoResDownAt = AutoResSeconds;
	}
	if (Before < SlowSeconds && AutoResSeconds >= SlowSeconds)
	{
		UPinguSettings::SetAutoResTestExtraGpuMs(0.0);
		Report(TEXT("autores: the GPU is fast again"));
	}
	if (AutoResSeconds >= SlowSeconds && AutoResUpAt < 0.0 && Step == 0)
	{
		AutoResUpAt = AutoResSeconds - SlowSeconds;
	}
	if (AutoResUpAt >= 0.0 || AutoResSeconds >= SlowSeconds + MaxUpSeconds)
	{
		const bool bDown = AutoResDownAt >= 0.0 && AutoResDownAt <= MaxDownSeconds;
		const bool bUp = AutoResUpAt >= 0.0 && AutoResUpAt <= MaxUpSeconds;
		Report(FString::Printf(TEXT("RESULT autores down: last step after %.1f s of a slow GPU (want under %.0f s): %s"),
			AutoResDownAt, MaxDownSeconds, bDown ? TEXT("ok") : TEXT("FAILED")));
		Report(FString::Printf(TEXT("RESULT autores up: back to Low as it is after %.1f s of a fast GPU (want under %.0f s): %s"),
			AutoResUpAt, MaxUpSeconds, bUp ? TEXT("ok") : TEXT("FAILED")));
		SummaryLines.Add(FString::Printf(TEXT("autores %s"), bDown && bUp ? TEXT("ok") : TEXT("FAILED")));
		Finish(bDown && bUp, bDown && bUp ? FString() : FString(TEXT("Low's adaptive resolution did not behave")));
	}
}

void APinguPerfTest::StartBotMatch()
{
	MatchStartSeconds = TotalSeconds;
	APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr;
	if (!GameMode)
	{
		Report(TEXT("WARN no Pingu game mode in this world: measuring whatever runs"));
		return;
	}
	const EPinguPhase Phase = GameMode->GetPhase();
	if (Phase == EPinguPhase::Menu || Phase == EPinguPhase::Lobby || Phase == EPinguPhase::Practice || Phase == EPinguPhase::MatchOver)
	{
		FPinguMatchSettings Match;
		Match.NumBots = 3;
		Match.Difficulty = 2;
		Match.RoundsToWin = 9;
		Match.bEndless = true; // rounds keep coming (island, collapse, abilities) for the whole test
		GameMode->StartMatch(Match);
		Report(FString::Printf(TEXT("bots-only endless match started (3 bots + the player's penguin on autopilot) from phase %s"),
			*UEnum::GetValueAsString(Phase)));
	}
	else
	{
		Report(FString::Printf(TEXT("a match is already running (phase %s): measuring it"), *UEnum::GetValueAsString(Phase)));
	}
}

void APinguPerfTest::KeepBotsPlaying()
{
	using namespace PinguPerfTestPrivate;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		UPenguinBotComponent* Bot = It->GetBot();
		if (Bot && !Bot->IsBotEnabled())
		{
			Bot->SetBotEnabled(true, 2);
		}
	}
	APenguinGameMode* GameMode = World->GetAuthGameMode<APenguinGameMode>();
	if (!GameMode)
	{
		return;
	}
	if (GameMode->IsGamePaused())
	{
		GameMode->SetPaused(false);
	}
	// The match ended after all (or something went back to the menu): a new one, so every preset sees a fight.
	const EPinguPhase Phase = GameMode->GetPhase();
	const bool bNoMatch = Phase == EPinguPhase::Menu || Phase == EPinguPhase::Lobby || Phase == EPinguPhase::Practice || Phase == EPinguPhase::MatchOver;
	if (bNoMatch && Stage != EStage::StartMatch && Stage != EStage::Done && TotalSeconds - MatchStartSeconds > MatchRestartCooldownSeconds)
	{
		++MatchRestarts;
		Report(FString::Printf(TEXT("WARN the match stopped (phase %s) during %s: starting a new one"), *UEnum::GetValueAsString(Phase), GetCurrentPresetName()));
		StartBotMatch();
	}
}

void APinguPerfTest::ApplyCurrentPreset()
{
	using namespace PinguPerfTestPrivate;
	UPinguSettings* Settings = UPinguSettings::Get();
	Settings->ApplyPreset(GetCurrentPreset()); // also sets the preset's frame cap (Low: 60) ...
	Settings->SetFrameRateLimit(0);            // ... which the measurement must not have
	Settings->ApplyGraphics(GetWorld());       // this world's sky light and sun (idempotent)

	FString Values;
	for (const TCHAR* Name : ReportedCVars)
	{
		if (const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name, false))
		{
			Values += FString::Printf(TEXT(" %s=%s"), Name, *CVar->GetString());
		}
	}
	Report(FString::Printf(TEXT("preset %s applied (FX density %.2f):%s"), GetCurrentPresetName(), UPinguSettings::GetFXDensity(), *Values));
}

void APinguPerfTest::BeginMeasure()
{
	Stage = EStage::Measure;
	StageSeconds = 0.0;
	StageFrames = 0;
	FramesCompilingShaders = 0;
	bProfiledThisPreset = false;
	TickFrameMs.Reset();
	if (Chart.IsValid())
	{
		Chart->BeginRecording();
	}
}

void APinguPerfTest::EndMeasure()
{
	using namespace PinguPerfTestPrivate;
	const TCHAR* Name = GetCurrentPresetName();
	if (Chart.IsValid())
	{
		Chart->EndRecording();
	}
	const bool bEngineData = Chart.IsValid() && Chart->GetNumFrames() > 0 && Chart->FrameMs.Num() > 0;
	const TArray<float>& Frames = bEngineData ? Chart->FrameMs : TickFrameMs;
	const float AverageMs = Average(Frames);
	const float P95Ms = Percentile(Frames, 0.95f);
	const float Fps = AverageMs > 0.f ? 1000.f / AverageMs : 0.f;

	float GpuMs = 0.f;
	if (bEngineData)
	{
		const double NumFrames = static_cast<double>(Chart->GetNumFrames());
		GpuMs = static_cast<float>(Chart->TotalFrameTime_GPU / NumFrames * 1000.0);
		const float GameMs = static_cast<float>(Chart->TotalFrameTime_GameThread / NumFrames * 1000.0);
		const float RenderMs = static_cast<float>(Chart->TotalFrameTime_RenderThread / NumFrames * 1000.0);
		const float RhiMs = static_cast<float>(Chart->TotalFrameTime_RHIThread / NumFrames * 1000.0);
		const int32 Draws = FMath::RoundToInt32(static_cast<double>(Chart->TotalDrawCalls) / NumFrames);
		const long long Prims = static_cast<long long>(static_cast<double>(Chart->TotalDrawnPrimitives) / NumFrames + 0.5);
		Report(FString::Printf(TEXT("RESULT %s: avg %.2f ms (%.1f fps), p95 %.2f ms, gpu %.2f ms, game %.2f ms, draws %d, prims %lld"),
			Name, AverageMs, Fps, P95Ms, GpuMs, GameMs, Draws, Prims));
		Report(FString::Printf(TEXT("DETAIL %s: render thread %.2f ms, RHI thread %.2f ms, gpu p95 %.2f ms, frames %d, draws max %d, prims max %d, ")
			TEXT("bound frames game %u / render %u / gpu %u, frames while compiling shaders %d, match restarts %d"),
			Name, RenderMs, RhiMs, Percentile(Chart->GpuMs, 0.95f), Chart->FrameMs.Num(), Chart->MaxDrawCalls, Chart->MaxDrawnPrimitives,
			Chart->NumFramesBound_GameThread, Chart->NumFramesBound_RenderThread, Chart->NumFramesBound_GPU, FramesCompilingShaders, MatchRestarts));
		if (GpuMs <= 0.f)
		{
			Report(FString::Printf(TEXT("WARN %s: no GPU timing from the RHI on this machine (gpu 0)"), Name));
		}
	}
	else
	{
		// Same line shape; the engine handed us no frames at all, so only our own frame times exist.
		Report(FString::Printf(TEXT("RESULT %s: avg %.2f ms (%.1f fps), p95 %.2f ms, gpu n/a ms, game n/a ms, draws n/a, prims n/a"),
			Name, AverageMs, Fps, P95Ms));
		Report(FString::Printf(TEXT("WARN %s: no engine frame data (%d frames from the test's own tick)"), Name, TickFrameMs.Num()));
	}
	SummaryLines.Add(FString::Printf(TEXT("%s %.1f ms / %.0f fps (p95 %.1f, gpu %.1f)"), Name, AverageMs, Fps, P95Ms, GpuMs));

	if (!bProfileGPU) // (profile runs only want the numbers)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ClaudeLogs/Perf") / FString(Name) + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		Report(FString::Printf(TEXT("screenshot %s"), *Path));
	}

	Stage = EStage::Screenshot;
	StageSeconds = 0.0;
	StageFrames = 0;
}

void APinguPerfTest::Finish(bool bSuccess, const FString& Why)
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	Stage = EStage::Done;
	if (Chart.IsValid())
	{
		Chart->EndRecording();
		if (GEngine)
		{
			GEngine->RemovePerformanceDataConsumer(Chart);
		}
		Chart.Reset();
	}
	if (SummaryLines.Num() > 0)
	{
		Report(TEXT("SUMMARY ") + FString::Join(SummaryLines, TEXT(" | ")));
	}
	if (!bSuccess)
	{
		Report(TEXT("FAIL ") + Why);
	}
	RestorePlayerSettings();
	Report(bSuccess ? TEXT("done") : TEXT("done (failed)"));
	if (GIsEditor)
	{
		// Play In Editor: end the session, not the editor.
		UKismetSystemLibrary::QuitGame(this, nullptr, EQuitPreference::Quit, false);
	}
	else
	{
		FPlatformMisc::RequestExit(false, TEXT("PinguPerfTest"));
	}
}

void APinguPerfTest::RestorePlayerSettings()
{
	if (!bHavePlayerSettings)
	{
		return;
	}
	bHavePlayerSettings = false;
	UPinguSettings* Settings = UPinguSettings::Get();
	Settings->SetGraphics(PlayerGraphics);
	Settings->SetFrameRateLimit(PlayerFrameRateLimit);
	Settings->SetVSync(bPlayerVSync);
	Report(FString::Printf(TEXT("the player's settings are back (%s preset, frame cap %d, vsync %d); a test run saves nothing"),
		*UPinguSettings::GetPresetName(PlayerGraphics.Preset).ToString(), PlayerFrameRateLimit, bPlayerVSync ? 1 : 0));
}

void APinguPerfTest::Report(const FString& Message) const
{
	UE_LOG(LogPingu, Display, TEXT("[PERFTEST] %s"), *Message);
}

EPinguGraphicsPreset APinguPerfTest::GetCurrentPreset() const
{
	const int32 Index = PresetOrder.IsValidIndex(PresetIndex) ? PresetOrder[PresetIndex] : FMath::Clamp(PresetIndex, 0, PinguPerfTestPrivate::NumPresets - 1);
	return PinguPerfTestPrivate::Presets[Index];
}

const TCHAR* APinguPerfTest::GetCurrentPresetName() const
{
	const int32 Index = PresetOrder.IsValidIndex(PresetIndex) ? PresetOrder[PresetIndex] : FMath::Clamp(PresetIndex, 0, PinguPerfTestPrivate::NumPresets - 1);
	return PinguPerfTestPrivate::PresetNames[Index];
}
