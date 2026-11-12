// Pingu - the player's settings: graphics (presets + options), display, audio, key bindings, LAN name. Saved per user.

#include "PinguSettings.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PinguFX.h"
#include "PinguNetTest.h"
#include "ChartCreation.h"
#include "ComponentRecreateRenderStateContext.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameDelegates.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameUserSettings.h"
#include "GenericPlatform/GenericApplication.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "RenderUtils.h"
#include "RHIGlobals.h"
#include "Scalability.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "PinguSettings"

/*
 * GRAPHICS PRESETS
 * ================
 * Two layers, both applied by ApplyGraphics:
 *  1. Scalability groups (sg.*, the engine's own quality tables in BaseScalability.ini + our Config/DefaultScalability.ini).
 *     Level per option (0 low, 1 medium, 2 high, 3 epic = today's look, 4 cinematic):
 *
 *        option        drives                                        Off Low Medium High Ultra
 *        Shadows       sg.ShadowQuality                               0   0    1     3    3
 *        Lighting      sg.GlobalIlluminationQuality                   0   0    1     3    3
 *        Reflections   sg.ReflectionQuality                           0   0    1     3    4
 *        Effects       sg.EffectsQuality                              0   0    1     3    3
 *        PostProcess   sg.PostProcessQuality                          0   0    1     3    4
 *                      sg.AntiAliasingQuality                         0   1    2     3    4
 *        ModelDetail   sg.ViewDistance/Shading/Foliage/LandscapeQuality 0 0    1     3    3
 *        Textures      sg.TextureQuality                              0   1    2     3    4
 *        3D resolution sg.ResolutionQuality = ResolutionScale
 *
 *  2. Console variables on top (priority SetByGameOverride: beats project settings, device profiles and scalability;
 *     ConsoleVariables.ini, the command line and the console still beat us). See BuildRenderSettings for the values:
 *     r.ScreenPercentage, r.AntiAliasingMethod (+ r.TemporalAA.Upsampling), r.Shadow.Virtual.Enable, r.ShadowQuality,
 *     CSM cascades / resolution / distance scale / caster threshold, VSM resolution bias, r.DynamicGlobalIlluminationMethod,
 *     r.Lumen.HardwareRayTracing (+ lighting mode), r.ReflectionMethod, r.SSR.Quality, r.BloomQuality,
 *     r.MaterialQualityLevel (Low 0, Medium 2, High 1, Ultra 3 - the art's QualitySwitch materials), r.DetailMode,
 *     r.StaticMeshLODDistanceScale. A variable that a scalability group also sets always gets an explicit value (never
 *     "back to default"), so switching presets can never leave a stale value behind.
 *
 *  Plus, per world: the sky light's real-time capture (only with Lumen; captured once otherwise) and the sun's cascaded
 *  shadow distance (8000 cm on Low / Medium: the camera is 30-68 m from the play plane), and the frame cap.
 *
 *  What each preset gets (a full set of FPinguGraphicsOptions, see GetPresetOptions):
 *
 *            res  AA   shadows  lighting reflections effects post  model  textures  frame cap  FX density
 *   Low      67   TAA  Low      Low      Low         Low     Low   Low    Low       60         0.3
 *   Medium   85   TAA  Medium   Medium   Medium      Medium  Med.  Med.   Medium    -          0.6
 *   High     100  TSR  High     High     High        High    High  High   High      -          1
 *   Ultra    100  TSR  Ultra    Ultra    Ultra       Ultra   Ultra Ultra  Ultra     -          1
 *
 *   Low    = an ordinary laptop (Intel Iris Xe / Radeon 680M) at 1080p, 60 fps: no Lumen (sky light captured once, DFAO
 *            off, no reflections), no virtual shadow maps (1 cascade 1024, 80 m), TAA upsampling from 67 %, AO off, cheap
 *            bloom / tonemapper, LOD bias 2, Low materials, R11G11B10 scene colour, cheap sky atmosphere, fewer particles.
 *            And Low adapts (UpdateAutoResolution): GPU time per frame over 90 % of the budget (60 fps) -> the next step
 *            67 % TAA -> 58 % TAA -> 50 % TAA -> 50 % FXAA without bloom (the post effects then run at the 3D resolution,
 *            not the window's) -> the same without shadows; under 55 % of the budget for 10 s -> one step back up.
 *            Why: on the RTX 4070 Ti Low takes 1.0 ms of GPU time at 1080p, but 0.8 ms of it does not shrink with the 3D
 *            resolution (small passes, output-resolution passes: TAA upsampling, tonemapper, bloom, UI). Scaled by
 *            3DMark Time Spy graphics scores (4070 Ti 22.5k; Iris Xe 1.3-1.8k, Vega 7/8 1.1-1.2k, 680M / 780M 2-2.9k,
 *            UHD 620 0.36k) that is about 13-18 ms on Iris Xe / Vega (right at 60 fps), 7-11 ms on 680M / 780M, and
 *            45-60 ms on UHD 620 - so a fixed 67 % would leave the slower half of the laptops just under 60.
 *   Medium = sky light (captured once) + distance field AO, SSR, 1 cascade 2048, TAA from 85 %, SSAO, Medium materials.
 *   High   = today's look: every group Epic, Lumen GI + reflections, VSM, TSR 100 %, real-time sky capture.
 *   Ultra  = High + sharper VSM, Lumen with hardware ray tracing and hit-lit reflections, translucency reflections,
 *            Epic materials (detailed felt / clay), finer LODs, cinematic post / textures / TSR history.
 */

namespace PinguSettingsPrivate
{
	constexpr int32 NumActions = static_cast<int32>(EPinguInputAction::Count);
	/** Bump when the default bindings change so that old saved bindings must not survive (they are reset). */
	constexpr int32 CurrentBindingsVersion = 1;
	constexpr int32 MaxPlayerNameLength = 20;
	/** Today's sound mix at the default slider positions (APinguFX used SFX 0.8, music 0.45; sliders default 1 / 0.8). */
	constexpr float SfxMix = 0.8f;
	constexpr float MusicMix = 0.45f / 0.8f;
	/** Priority of our console variables (see the comment at the top). */
	constexpr EConsoleVariableFlags SetByPingu = ECVF_SetByGameOverride;
	/** Cascaded shadow distance on Low / Medium: the camera is 30-68 m from the play plane, so 80 m covers it all. */
	constexpr float CascadedShadowDistance = 8000.f;

	UPinguSettings* GInstance = nullptr;
	/** GetFXDensity's value for the current Effects option (see CacheFXDensity). */
	float GFXDensity = 1.f;
	/** UseUltraEffects's value. */
	bool GFXUltra = false;

	/** Particle density per Effects option: Off, Low, Medium, High (today), Ultra (the same density plus the Niagara
	 *  layers). */
	static void CacheFXDensity(const FPinguGraphicsOptions& Options)
	{
		static const float Density[] = { 0.3f, 0.3f, 0.6f, 1.f, 1.f };
		GFXDensity = Density[FMath::Clamp(static_cast<int32>(Options.Effects), 0, 4)];
		GFXUltra = Options.Effects == EPinguQualityLevel::Ultra;
	}

	/** A console variable value; an empty Value means "the project's own value" (only for variables no scalability
	 *  group sets, so restoring them is exact). */
	struct FCVarSetting
	{
		const TCHAR* Name = nullptr;
		FString Value;
	};

	struct FSunBackup
	{
		TWeakObjectPtr<UDirectionalLightComponent> Light;
		float Distance = 0.f;
	};

	// ---------------------------------------------------------------------------- Low adapts (UPinguSettings::GetAutoStep)

	/** 3D resolution per step (step 3 on: the fast upscale, step 4: no shadows either). */
	static const int32 AutoStepResolution[UPinguSettings::NumAutoSteps] = { 67, 58, 50, 50, 50 };

	/** GPU time per frame from the engine's per-frame performance data (the numbers of stat unit; no RHI dependency). */
	class FAutoResMonitor : public IPerformanceDataConsumer
	{
	public:
		virtual void StartCharting() override {}
		virtual void StopCharting() override {}
		virtual void ProcessFrame(const FFrameData& FrameData) override
		{
			// Hitches (loading, a window being dragged) say nothing about the steady cost; no timing = nothing to go by.
			if (FrameData.TrueDeltaSeconds <= 0.0 || FrameData.TrueDeltaSeconds > 0.25 || FrameData.GPUTimeSeconds <= 0.0)
			{
				return;
			}
			GpuMsSum += FrameData.GPUTimeSeconds * 1000.0 + FakeGpuMs;
			++Frames;
		}
		void Reset()
		{
			GpuMsSum = 0.0;
			Frames = 0;
		}
		double AverageGpuMs() const { return Frames > 0 ? GpuMsSum / Frames : 0.0; }

		double GpuMsSum = 0.0;
		int32 Frames = 0;
		/** -PinguAutoResFakeGpuMs=: added to every frame (tests: pretend the graphics chip is slower). */
		double FakeGpuMs = 0.0;
	};

	/** The first start's measuring (UPinguSettings::UpdateFirstStartPick): every steady frame's GPU time, so the median can
	 *  be taken (robust against the frames that compile a shader on a first start). */
	class FPickMonitor : public IPerformanceDataConsumer
	{
	public:
		virtual void StartCharting() override {}
		virtual void StopCharting() override {}
		virtual void ProcessFrame(const FFrameData& FrameData) override
		{
			// Up to a second per frame: a weak chip's slow frames count (the median takes care of a loading hitch).
			if (!bRecording || FrameData.TrueDeltaSeconds <= 0.0 || FrameData.TrueDeltaSeconds > 1.0 || FrameData.GPUTimeSeconds <= 0.0)
			{
				return;
			}
			GpuMs.Add(FrameData.GPUTimeSeconds * 1000.0);
		}
		double MedianGpuMs() const
		{
			if (GpuMs.Num() == 0)
			{
				return 0.0;
			}
			TArray<double> Sorted = GpuMs;
			Sorted.Sort();
			return Sorted[Sorted.Num() / 2];
		}

		bool bRecording = false;
		TArray<double> GpuMs;
	};

	/** The options as rendered: Low with its adaptive step (see UPinguSettings::GetAutoStep). */
	static FPinguGraphicsOptions EffectiveOptions(const FPinguGraphicsOptions& Options, int32 Step)
	{
		FPinguGraphicsOptions Effective = Options;
		if (Options.Preset != EPinguGraphicsPreset::Low || Step <= 0)
		{
			return Effective;
		}
		Step = FMath::Min(Step, UPinguSettings::NumAutoSteps - 1);
		Effective.ResolutionScale = FMath::Min(Effective.ResolutionScale, AutoStepResolution[Step]);
		if (Step >= 3)
		{
			// The fast upscale: FXAA, the post effects at the 3D resolution (with TAA they run at the window's), and no
			// bloom (post process Off = Low's post quality without it).
			Effective.AntiAliasing = EPinguAntiAliasing::FXAA;
			Effective.PostProcess = EPinguQualityLevel::Off;
		}
		if (Step >= 4)
		{
			Effective.Shadows = EPinguQualityLevel::Off;
		}
		return Effective;
	}

	static const TCHAR* DescribeAutoStep(int32 Step)
	{
		static const TCHAR* const Names[UPinguSettings::NumAutoSteps] = { TEXT("Low as it is (3D 67 %)"), TEXT("3D 58 %"), TEXT("3D 50 %"),
			TEXT("3D 50 %, fast upscale (FXAA, no bloom)"), TEXT("3D 50 %, fast upscale, no shadows") };
		return Names[FMath::Clamp(Step, 0, UPinguSettings::NumAutoSteps - 1)];
	}

	/** Runtime state of the one settings object (the contract's ApplyGraphics / ApplyAudio are const). */
	struct FRuntimeState
	{
		/** Value of every console variable we manage before our first change (restored when it goes back to the
		 *  project's value, and - in the editor - when Play In Editor ends). */
		TMap<FString, FString> CVarBackups;
		/** Variables already reported as missing / overridden (logged once). */
		TSet<FString> Reported;
		TWeakObjectPtr<UWorld> LastWorld;
		/** Sky lights whose real-time capture we switched off. */
		TArray<TWeakObjectPtr<USkyLightComponent>> StaticSkyLights;
		/** Suns whose cascaded shadow distance we changed (original value). */
		TArray<FSunBackup> Suns;
		bool bEditorLevelsSaved = false;
		Scalability::FQualityLevels EditorLevels = Scalability::FQualityLevels(false);
		bool bEditorMaxFPSSaved = false;
		float EditorMaxFPS = 0.f;
		bool bSaveSkipLogged = false;

		/** Low adapts: the step in use, the GPU time monitor, and the timing of the measuring windows (real seconds). */
		int32 AutoStep = 0;
		TSharedPtr<FAutoResMonitor> AutoMonitor;
		TWeakObjectPtr<UWorld> AutoWorld;
		double AutoSinceChange = 0.0;  // since the last step change or world start
		double AutoWindowTime = 0.0;   // length of the current measuring window
		bool bAutoWindowOpen = false;  // measuring (from 1 s after a change on: the change's own hitch is over)
		double AutoSinceStepUp = -1.0; // since the last step up (-1: none yet)
		double AutoUpBlockedFor = 0.0; // a step up that did not fit: no new one for this long
		double AutoFakeGpuMs = 0.0;    // tests: added to every frame's GPU time

		/** The first start's pick (AutoDetectIfNeeded, then UpdateFirstStartPick every frame): High on screen, measured. */
		bool bPickRunning = false;
		bool bPickDone = false;        // this run already picked (the test switch ignores bAutoDetected)
		bool bPickApplying = false;    // the pick itself changes the graphics (SetGraphics / ApplyPreset: not the player)
		double PickFirstStart = 0.0;   // FPlatformTime seconds: the measuring began (it gives up 30 s later)
		double PickStart = 0.0;        // ... the probe preset went on, or a new world came: the warm-up runs from here
		double PickMeasureStart = 0.0; // ... the frames are recorded from here
		double PickWindowStart = 0.0;  // ... the current window of a second began
		TArray<double> PickWindowMedians; // each finished window's median GPU ms
		TArray<int32> PickWindowFrames;
		TWeakObjectPtr<UWorld> PickWorld;
		TSharedPtr<FPickMonitor> PickMonitor;
		float PickGPUIndex = -1.f;     // the engine benchmark (recorded; the fallback when no frame can be measured)
		float PickCPUIndex = -1.f;
		double PickBenchSeconds = 0.0;
		EPinguGraphicsPreset PickFallback = EPinguGraphicsPreset::Medium;
	};

	static FRuntimeState& State()
	{
		static FRuntimeState Instance;
		return Instance;
	}

	static UGameUserSettings* GetGameUserSettings()
	{
		return GEngine ? GEngine->GetGameUserSettings() : nullptr;
	}

	static int32 LevelIndex(EPinguQualityLevel Level)
	{
		return FMath::Clamp(static_cast<int32>(Level), 0, 4);
	}

	/** -Name, -Name=... */
	static bool HasSwitch(const TCHAR* CommandLine, const TCHAR* Name)
	{
		FString Value;
		return FParse::Param(CommandLine, Name) || FParse::Value(CommandLine, *(FString(Name) + TEXT("=")), Value);
	}

	static void ReportOnce(const TCHAR* Name, const TCHAR* Why)
	{
		FRuntimeState& S = State();
		const FString Key(Name);
		if (!S.Reported.Contains(Key))
		{
			S.Reported.Add(Key);
			UE_LOG(LogPingu, Log, TEXT("[SETTINGS] %s %s"), Name, Why);
		}
	}

	// ---------------------------------------------------------------------------- console variables

	static IConsoleVariable* FindCVar(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name, false);
	}

	static bool CVarMatches(const IConsoleVariable* CVar, const FString& Value)
	{
		if (CVar->IsVariableString())
		{
			return CVar->GetString() == Value;
		}
		const float Wanted = FCString::Atof(*Value);
		if (CVar->IsVariableFloat())
		{
			return FMath::IsNearlyEqual(CVar->GetFloat(), Wanted, 1.e-4f);
		}
		return CVar->GetInt() == FMath::RoundToInt(Wanted); // int and bool variables
	}

	static uint32 SetByOf(const IConsoleVariable* CVar)
	{
		return static_cast<uint32>(CVar->GetFlags()) & static_cast<uint32>(ECVF_SetByMask);
	}

	/** Set by the command line, ConsoleVariables.ini, the console or engine code: a developer override we leave alone. */
	static bool IsHigherPriority(const IConsoleVariable* CVar)
	{
		return SetByOf(CVar) > static_cast<uint32>(SetByPingu);
	}

	static bool NeedsChange(const FCVarSetting& Setting)
	{
		IConsoleVariable* CVar = FindCVar(Setting.Name);
		if (!CVar)
		{
			return false;
		}
		if (Setting.Value.IsEmpty())
		{
			return State().CVarBackups.Contains(Setting.Name);
		}
		return !IsHigherPriority(CVar) && !CVarMatches(CVar, Setting.Value);
	}

	/** Back to the value before our first change: drop our priority level (cvar history), or set the old value. */
	static void RestoreCVar(const TCHAR* Name, IConsoleVariable* CVar, const FString& Original)
	{
		if (SetByOf(CVar) != static_cast<uint32>(SetByPingu))
		{
			return; // somebody else owns it now
		}
		CVar->Unset(SetByPingu);
		if (SetByOf(CVar) == static_cast<uint32>(SetByPingu) && !CVarMatches(CVar, Original))
		{
			CVar->Set(*Original, SetByPingu); // no cvar history in this build: the old value at our priority
		}
	}

	static void ApplyCVar(const FCVarSetting& Setting)
	{
		FRuntimeState& S = State();
		IConsoleVariable* CVar = FindCVar(Setting.Name);
		if (!CVar)
		{
			ReportOnce(Setting.Name, TEXT("does not exist in this engine version (skipped)"));
			return;
		}
		if (Setting.Value.IsEmpty())
		{
			if (const FString* Original = S.CVarBackups.Find(Setting.Name))
			{
				RestoreCVar(Setting.Name, CVar, *Original);
				S.CVarBackups.Remove(Setting.Name);
			}
			return;
		}
		if (CVarMatches(CVar, Setting.Value))
		{
			return;
		}
		if (IsHigherPriority(CVar))
		{
			ReportOnce(Setting.Name, TEXT("is set by the command line, ConsoleVariables.ini or the console: left as it is"));
			return;
		}
		CVar->Set(*Setting.Value, SetByPingu);
	}

	/** Remembers the values before our first change (before the scalability groups change them, too). */
	static void BackupCVars(const TArray<FCVarSetting>& Settings)
	{
		FRuntimeState& S = State();
		for (const FCVarSetting& Setting : Settings)
		{
			if (Setting.Value.IsEmpty() || S.CVarBackups.Contains(Setting.Name))
			{
				continue;
			}
			if (const IConsoleVariable* CVar = FindCVar(Setting.Name))
			{
				S.CVarBackups.Add(Setting.Name, CVar->GetString());
			}
		}
	}

	static FString Num(float Value)
	{
		return FString::SanitizeFloat(Value, 0);
	}

	/** Both layers for a set of options (see the comment at the top of the file). */
	static void BuildRenderSettings(const FPinguGraphicsOptions& O, Scalability::FQualityLevels& Levels, TArray<FCVarSetting>& Out)
	{
		using Q = EPinguQualityLevel;
		// Scalability group level per option value: Off, Low, Medium, High, Ultra.
		static const int32 ShadowGroup[] = { 0, 0, 1, 3, 3 };
		static const int32 LightingGroup[] = { 0, 0, 1, 3, 3 };
		static const int32 ReflectionGroup[] = { 0, 0, 1, 3, 4 };
		static const int32 EffectsGroup[] = { 0, 0, 1, 3, 3 };
		static const int32 PostGroup[] = { 0, 0, 1, 3, 4 };
		static const int32 AntiAliasingGroup[] = { 0, 1, 2, 3, 4 };
		static const int32 DetailGroup[] = { 0, 0, 1, 3, 3 };
		static const int32 TextureGroup[] = { 0, 1, 2, 3, 4 };

		const int32 Shadows = LevelIndex(O.Shadows);
		const int32 Lighting = LevelIndex(O.Lighting);
		const int32 Reflections = LevelIndex(O.Reflections);
		const int32 Post = LevelIndex(O.PostProcess);
		const int32 Detail = LevelIndex(O.ModelDetail);

		Levels.ResolutionQuality = static_cast<float>(FMath::Clamp(O.ResolutionScale, 50, 100));
		Levels.ShadowQuality = ShadowGroup[Shadows];
		Levels.GlobalIlluminationQuality = LightingGroup[Lighting];
		Levels.ReflectionQuality = ReflectionGroup[Reflections];
		Levels.EffectsQuality = EffectsGroup[LevelIndex(O.Effects)];
		Levels.PostProcessQuality = PostGroup[Post];
		Levels.AntiAliasingQuality = AntiAliasingGroup[Post];
		Levels.ViewDistanceQuality = DetailGroup[Detail];   // r.SkeletalMeshLODBias 2 / 2 / 1 / 0 / 0, r.ViewDistanceScale
		Levels.ShadingQuality = DetailGroup[Detail];
		Levels.FoliageQuality = DetailGroup[Detail];        // no foliage below Medium
		Levels.LandscapeQuality = DetailGroup[Detail];
		Levels.TextureQuality = TextureGroup[LevelIndex(O.Textures)];

		auto Add = [&Out](const TCHAR* Name, const FString& Value)
		{
			FCVarSetting& Setting = Out.AddDefaulted_GetRef();
			Setting.Name = Name;
			Setting.Value = Value;
		};
		auto Pick = [](const TCHAR* const (&Values)[5], int32 Index) { return FString(Values[FMath::Clamp(Index, 0, 4)]); };

		// ---- 3D resolution and anti-aliasing (the UI is always drawn at the full window resolution)
		Add(TEXT("r.ScreenPercentage"), FString::FromInt(FMath::Clamp(O.ResolutionScale, 50, 100)));
		static const TCHAR* const AntiAliasingMethod[] = { TEXT("0"), TEXT("1"), TEXT("2"), TEXT("4") }; // none, FXAA, TAA, TSR
		Add(TEXT("r.AntiAliasingMethod"), AntiAliasingMethod[FMath::Clamp(static_cast<int32>(O.AntiAliasing), 0, 3)]);
		// TAA upsamples temporally from the lower 3D resolution (much sharper than FXAA + a spatial upscale).
		Add(TEXT("r.TemporalAA.Upsampling"), O.AntiAliasing == EPinguAntiAliasing::TAA ? TEXT("1") : TEXT(""));
#if WITH_EDITOR
		if (GIsEditor)
		{
			// Play In Editor uses the editor viewport's screen percentage unless told otherwise.
			Add(TEXT("r.Editor.Viewport.OverridePIEScreenPercentage"), TEXT("0"));
			// ... and presents through the editor window, whose vsync is r.VSyncEditor (r.VSync is pinned off in the
			// editor): the game's VSYNC option while playing, the editor's own value back when Play ends.
			const UGameUserSettings* UserSettings = GetGameUserSettings();
			Add(TEXT("r.VSyncEditor"), (UserSettings && UserSettings->IsVSyncEnabled()) ? TEXT("1") : TEXT(""));
		}
#endif

		// ---- shadows: cascaded shadow maps below High (one cascade: in this side view the near cascades would only cover
		// the empty air between the camera and the play plane), virtual shadow maps from High on.
		static const TCHAR* const VirtualShadows[] = { TEXT("0"), TEXT("0"), TEXT("0"), TEXT("1"), TEXT("1") };
		static const TCHAR* const ShadowQuality[] = { TEXT("0"), TEXT("2"), TEXT("3"), TEXT("5"), TEXT("5") };
		static const TCHAR* const Cascades[] = { TEXT("1"), TEXT("1"), TEXT("1"), TEXT("10"), TEXT("10") };
		static const TCHAR* const CascadeResolution[] = { TEXT("512"), TEXT("1024"), TEXT("2048"), TEXT("2048"), TEXT("2048") };
		static const TCHAR* const ShadowResolution[] = { TEXT("512"), TEXT("512"), TEXT("1024"), TEXT("2048"), TEXT("2048") };
		static const TCHAR* const CasterThreshold[] = { TEXT("0.06"), TEXT("0.02"), TEXT("0.02"), TEXT("0.01"), TEXT("0.01") };
		static const TCHAR* const VirtualBias[] = { TEXT("0"), TEXT("0"), TEXT("0"), TEXT("-1.5"), TEXT("-2") };
		Add(TEXT("r.Shadow.Virtual.Enable"), Pick(VirtualShadows, Shadows));
		Add(TEXT("r.ShadowQuality"), Pick(ShadowQuality, Shadows)); // 0 = no shadows (Off only: the engine's Low has none)
		Add(TEXT("r.Shadow.CSM.MaxCascades"), Pick(Cascades, Shadows));
		Add(TEXT("r.Shadow.MaxCSMResolution"), Pick(CascadeResolution, Shadows));
		Add(TEXT("r.Shadow.MaxResolution"), Pick(ShadowResolution, Shadows));
		Add(TEXT("r.Shadow.DistanceScale"), TEXT("1")); // the sun's distance itself is set per preset (ApplyWorldLighting)
		Add(TEXT("r.Shadow.RadiusThreshold"), Pick(CasterThreshold, Shadows)); // keeps a far penguin's shadow when zoomed out
		Add(TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectional"), Pick(VirtualBias, Shadows));
		Add(TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectionalMoving"), Pick(VirtualBias, Shadows));

		// ---- global illumination: plain sky light below High, Lumen from High on (Ultra with hardware ray tracing;
		// High keeps the project's own "use hardware ray tracing when available" = today's look).
		const bool bLumen = O.Lighting >= Q::High;
		Add(TEXT("r.DynamicGlobalIlluminationMethod"), bLumen ? TEXT("1") : TEXT("0"));
		Add(TEXT("r.Lumen.HardwareRayTracing"), O.Lighting == Q::Ultra ? TEXT("1") : TEXT(""));

		// ---- reflections: none (sky light only) on Low, screen space on Medium (and on High+ without Lumen lighting),
		// Lumen from High on; Ultra: hit lighting for the reflections (the penguins look right in the ice).
		const bool bLumenReflections = bLumen && O.Reflections >= Q::High;
		Add(TEXT("r.ReflectionMethod"), bLumenReflections ? TEXT("1") : (O.Reflections >= Q::Medium ? TEXT("2") : TEXT("0")));
		static const TCHAR* const SSRQuality[] = { TEXT("0"), TEXT("0"), TEXT("2"), TEXT("3"), TEXT("4") };
		Add(TEXT("r.SSR.Quality"), Pick(SSRQuality, Reflections));
		Add(TEXT("r.Lumen.HardwareRayTracing.LightingMode"), (bLumenReflections && O.Reflections == Q::Ultra && O.Lighting == Q::Ultra) ? TEXT("1") : TEXT(""));

		// ---- post process: everything else comes from sg.PostProcessQuality (AO off on Low, lens flares from High,
		// tonemapper 0 / 2 / 5, bloom 4 at 25 % / 35 % resolution below High); Off also drops the bloom.
		static const TCHAR* const Bloom[] = { TEXT("0"), TEXT("4"), TEXT("4"), TEXT("5"), TEXT("5") };
		Add(TEXT("r.BloomQuality"), Pick(Bloom, Post));

		// ---- model detail: material quality for the art's QualitySwitch materials, detail mode, LOD distances
		static const TCHAR* const MaterialQuality[] = { TEXT("0"), TEXT("0"), TEXT("2"), TEXT("1"), TEXT("3") }; // Low, Low, Medium, High, Epic
		static const TCHAR* const DetailMode[] = { TEXT("0"), TEXT("0"), TEXT("1"), TEXT("3"), TEXT("3") };
		// Low 2.5: even at the closest camera zoom the island blocks draw their cheap LOD (art: LOD1 at screen size 0.075).
		static const float LODDistanceScale[] = { 2.5f, 2.5f, 1.4f, 1.f, 0.75f };
		Add(TEXT("r.MaterialQualityLevel"), Pick(MaterialQuality, Detail));
		Add(TEXT("r.DetailMode"), Pick(DetailMode, Detail));
		Add(TEXT("r.StaticMeshLODDistanceScale"), Num(LODDistanceScale[Detail]));
	}

	static EPinguGraphicsPreset DetectPreset(const FPinguGraphicsOptions& Options)
	{
		for (const EPinguGraphicsPreset Candidate : { EPinguGraphicsPreset::Low, EPinguGraphicsPreset::Medium, EPinguGraphicsPreset::High, EPinguGraphicsPreset::Ultra })
		{
			if (Options.SameOptionsAs(UPinguSettings::GetPresetOptions(Candidate)))
			{
				return Candidate;
			}
		}
		return EPinguGraphicsPreset::Custom;
	}

	static void SanitizeOptions(FPinguGraphicsOptions& Options)
	{
		Options.ResolutionScale = FMath::Clamp(Options.ResolutionScale, 50, 100);
		auto FixLevel = [](EPinguQualityLevel& Level)
		{
			if (static_cast<uint8>(Level) > static_cast<uint8>(EPinguQualityLevel::Ultra))
			{
				Level = EPinguQualityLevel::High;
			}
		};
		FixLevel(Options.Shadows);
		FixLevel(Options.Lighting);
		FixLevel(Options.Reflections);
		FixLevel(Options.Effects);
		FixLevel(Options.PostProcess);
		FixLevel(Options.ModelDetail);
		FixLevel(Options.Textures);
		if (static_cast<uint8>(Options.AntiAliasing) > static_cast<uint8>(EPinguAntiAliasing::TSR))
		{
			Options.AntiAliasing = EPinguAntiAliasing::TSR;
		}
		if (static_cast<uint8>(Options.Preset) > static_cast<uint8>(EPinguGraphicsPreset::Custom))
		{
			Options.Preset = EPinguGraphicsPreset::Custom;
		}
	}

	static FString DescribeOptions(const FPinguGraphicsOptions& O)
	{
		return FString::Printf(TEXT("%s: 3D %d%%, AA %s, shadows %s, lighting %s, reflections %s, effects %s, post %s, models %s, textures %s"),
			*UPinguSettings::GetPresetName(O.Preset).ToString(), O.ResolutionScale, *UPinguSettings::GetAntiAliasingName(O.AntiAliasing).ToString(),
			*UPinguSettings::GetQualityName(O.Shadows).ToString(), *UPinguSettings::GetQualityName(O.Lighting).ToString(),
			*UPinguSettings::GetQualityName(O.Reflections).ToString(), *UPinguSettings::GetQualityName(O.Effects).ToString(),
			*UPinguSettings::GetQualityName(O.PostProcess).ToString(), *UPinguSettings::GetQualityName(O.ModelDetail).ToString(),
			*UPinguSettings::GetQualityName(O.Textures).ToString());
	}

	/** The benchmark's GPU index from which ChoosePreset picks Medium / High / Ultra (below Medium's: Low). */
	static constexpr float MediumGPUIndex = 130.f;
	static constexpr float HighGPUIndex = 380.f;
	static constexpr float UltraGPUIndex = 650.f;

	/** The first start's real decider (ChoosePresetFromFrames): the preset High on screen (the title's bot demo, this PC's
	 *  own window size), the median GPU time of its frames. Ultra up to PickUltraMaxMs (with hardware ray tracing), High up
	 *  to PickHighMaxMs, Medium up to PickMediumMaxMs, else Low (which adapts further by itself). Measured at 1080p in a bot
	 *  match (High / Ultra GPU ms): RTX 4070 Ti 5.7 / 6.7, RTX 4060 laptop 10.5 / 12.8; integrated graphics about 35 at
	 *  High (Medium about a quarter of High, Low a fifth). */
	static constexpr double PickUltraMaxMs = 8.0;
	static constexpr double PickHighMaxMs = 13.0;
	static constexpr double PickMediumMaxMs = 35.0;
	// A laptop's graphics chip starts a process in a power-saving state and only goes to full power after about 15 s of load:
	// the RTX 4060 laptop (nvidia-smi) ran 13 s in P3 (1.8 GHz, 26 W, 99 % busy: the title in High at 29-33 ms per frame),
	// then in P0 (2.3-2.5 GHz, 50 W: 9.3 ms). A start right after another one, with the chip still at full power, drew 9.3 ms
	// from the first second; the engine benchmark read GPU index 60-80 cold, 225 warm. (With no visible window - tests'
	// -RenderOffscreen - it stayed in P3 for good.) So the frames are measured in windows of a second for up to
	// PickMeasureSeconds, and the fastest three windows in a row count (their middle value): the chip only gets faster. Done
	// early once that is already fast enough for Ultra (a desktop chip is at full power at once).
	static constexpr double PickWarmupSeconds = 2.0;   // the probe preset's (and a new world's) first frames are no steady ones
	static constexpr double PickWindowSeconds = 1.0;
	static constexpr int32 PickMinWindowFrames = 15;
	static constexpr int32 PickStableWindows = 3;
	static constexpr int32 PickMinFrames = 45;
	static constexpr double PickMeasureSeconds = 25.0;
	static constexpr double PickGiveUpSeconds = 40.0;  // no GPU timings by then (a minimized window, an RHI without timers)
	/** The first start's engine benchmark: a fifth of the default work (about 2 s instead of 11 on a laptop: the window shows
	 *  nothing meanwhile), enough for the record and the fallback. */
	static constexpr int32 FirstStartBenchmarkWorkScale = 2;

	/** The fallback pick from the engine's hardware benchmark alone (no frame could be measured). Intel integrated graphics
	 *  always get Low; Ultra only on a strong GPU that can do hardware ray tracing (Ultra's Lumen uses it). The benchmark
	 *  underrates laptops badly: an RTX 4060 laptop scored GPU index 76-80 at every start (its GPU stays at low clocks
	 *  through the benchmark's short bursts), against 722 for an RTX 4070 Ti that is only about twice as fast in this game
	 *  - hence the frame measuring. */
	static EPinguGraphicsPreset ChoosePreset(float GPUIndex, float CPUIndex, const FString& GPUBrand, bool bRayTracing)
	{
		// Synth benchmark GPU index (100 = a good GPU of ~2013): Iris Xe ~50-80, Radeon 680M / GTX 1650 ~120-150,
		// RTX 3050 laptop ~250-350, RTX 4070 Ti ~720 (Ultra there: 139 fps at 1080p against High's 164, perftest
		// 2026-10-10). The benchmark measures the GPU that renders; the brand is the display adapter's, which on a hybrid
		// laptop (Intel driving the screen, an NVIDIA rendering) is the Intel one: the brand only decides when there is no
		// benchmark.
		const bool bIntelIntegrated = GPUIndex <= 0.f && GPUBrand.Contains(TEXT("Intel")) && !GPUBrand.Contains(TEXT("Arc"));
		EPinguGraphicsPreset Chosen = EPinguGraphicsPreset::Medium;
		if (bIntelIntegrated)
		{
			Chosen = EPinguGraphicsPreset::Low;
		}
		else if (GPUIndex > 0.f)
		{
			Chosen = GPUIndex < MediumGPUIndex ? EPinguGraphicsPreset::Low
				: GPUIndex < HighGPUIndex ? EPinguGraphicsPreset::Medium
				: (GPUIndex < UltraGPUIndex || !bRayTracing) ? EPinguGraphicsPreset::High
				: EPinguGraphicsPreset::Ultra;
		}
		// A weak CPU (old dual core) struggles with Lumen's scene updates too.
		if (CPUIndex > 0.f && CPUIndex < 40.f && (Chosen == EPinguGraphicsPreset::High || Chosen == EPinguGraphicsPreset::Ultra))
		{
			Chosen = EPinguGraphicsPreset::Medium;
		}
		return Chosen;
	}

	/** The first start's pick from the measured GPU time of the preset High's frames (see PickUltraMaxMs). A weak CPU (an
	 *  old dual core: CPU index under 40) struggles with Lumen's scene updates too: at most Medium then. */
	static EPinguGraphicsPreset ChoosePresetFromFrames(double HighGpuMs, bool bRayTracing, float CPUIndex)
	{
		EPinguGraphicsPreset Chosen = (HighGpuMs <= PickUltraMaxMs && bRayTracing) ? EPinguGraphicsPreset::Ultra
			: HighGpuMs <= PickHighMaxMs ? EPinguGraphicsPreset::High
			: HighGpuMs <= PickMediumMaxMs ? EPinguGraphicsPreset::Medium
			: EPinguGraphicsPreset::Low;
		if (CPUIndex > 0.f && CPUIndex < 40.f && (Chosen == EPinguGraphicsPreset::High || Chosen == EPinguGraphicsPreset::Ultra))
		{
			Chosen = EPinguGraphicsPreset::Medium;
		}
		return Chosen;
	}

	/** -PinguFirstStartTest: the first start's pick runs although it is an automated run (and settings are saved), nothing
	 *  is saved, a RESULT line is logged and the run ends (pipeline step firststarttest). */
	static bool IsFirstStartTest()
	{
		static const bool bTest = FParse::Param(FCommandLine::Get(), TEXT("PinguFirstStartTest"));
		return bTest;
	}

	/** Hardware ray tracing is on for this run (GPU, driver and RHI can do it; Ultra's Lumen uses it). */
	static bool HasRayTracing()
	{
		return GIsRHIInitialized && IsRayTracingEnabled();
	}

	/** The user's config already holds graphics settings (from a build before the first-start pick, or written by hand):
	 *  they are the player's own, so the first-start pick must not replace them. */
	static bool HasSavedGraphics(const UObject* Settings)
	{
		FString Saved;
		return GConfig && GConfig->GetString(*Settings->GetClass()->GetPathName(), TEXT("Graphics"), Saved, GGameUserSettingsIni)
			&& !Saved.TrimStartAndEnd().IsEmpty();
	}

	// ---------------------------------------------------------------------------- key bindings

	static void GetDefaultKeys(EPinguInputAction Action, FKey& OutPrimary, FKey& OutSecondary)
	{
		OutPrimary = FKey();
		OutSecondary = FKey();
		switch (Action)
		{
		case EPinguInputAction::MoveLeft: OutPrimary = EKeys::A; OutSecondary = EKeys::Left; break;
		case EPinguInputAction::MoveRight: OutPrimary = EKeys::D; OutSecondary = EKeys::Right; break;
		case EPinguInputAction::Up: OutPrimary = EKeys::W; OutSecondary = EKeys::Up; break;
		case EPinguInputAction::Down: OutPrimary = EKeys::S; OutSecondary = EKeys::Down; break;
		case EPinguInputAction::Jump: OutPrimary = EKeys::SpaceBar; break;
		case EPinguInputAction::Dash: OutPrimary = EKeys::LeftShift; OutSecondary = EKeys::RightShift; break;
		// Left / middle / right mouse button = the left / top / right icon; J K L for the keyboard-only right hand
		// (and E stands in for a missing middle button, see GetFallbackKey).
		case EPinguInputAction::Ability1: OutPrimary = EKeys::LeftMouseButton; OutSecondary = EKeys::J; break;
		case EPinguInputAction::Ability2: OutPrimary = EKeys::MiddleMouseButton; OutSecondary = EKeys::K; break;
		case EPinguInputAction::Ability3: OutPrimary = EKeys::RightMouseButton; OutSecondary = EKeys::L; break;
		case EPinguInputAction::Pause: OutPrimary = EKeys::P; break;
		// Saved bindings from before the slide existed get these in Validate (a new action takes its free default keys).
		case EPinguInputAction::BellySlide: OutPrimary = EKeys::LeftControl; OutSecondary = EKeys::RightControl; break;
		default: break;
		}
	}

	static bool IsSet(const FKey& Key)
	{
		return Key.IsValid() && Key != EKeys::Invalid;
	}

	// ---------------------------------------------------------------------------- player name

	static FString SanitizePlayerName(const FString& Raw)
	{
		FString Clean;
		bool bPendingSpace = false;
		for (const TCHAR Char : Raw)
		{
			if (FChar::IsWhitespace(Char))
			{
				bPendingSpace = !Clean.IsEmpty();
				continue;
			}
			// Letters and digits of any script, and a few harmless marks (nothing that means something in a travel URL).
			const bool bAllowed = FChar::IsAlnum(Char) || Char == TEXT('-') || Char == TEXT('_') || Char == TEXT('.') || Char == TEXT('\'');
			if (!bAllowed)
			{
				continue;
			}
			if (bPendingSpace)
			{
				Clean.AppendChar(TEXT(' '));
				bPendingSpace = false;
			}
			Clean.AppendChar(Char);
		}
		if (Clean.Len() > MaxPlayerNameLength)
		{
			Clean.LeftInline(MaxPlayerNameLength);
			Clean.TrimEndInline();
		}
		return Clean;
	}

	static const FString& GetDefaultPlayerName()
	{
		static const FString Default = []()
		{
			FString Name = SanitizePlayerName(FString(FPlatformProcess::UserName(false)));
			if (Name.IsEmpty())
			{
				return FString(TEXT("Penguin"));
			}
			Name[0] = FChar::ToUpper(Name[0]); // "linus" -> "Linus"
			return Name;
		}();
		return Default;
	}
}

// ============================================================================ options

bool FPinguGraphicsOptions::SameOptionsAs(const FPinguGraphicsOptions& Other) const
{
	return ResolutionScale == Other.ResolutionScale
		&& AntiAliasing == Other.AntiAliasing
		&& Shadows == Other.Shadows
		&& Lighting == Other.Lighting
		&& Reflections == Other.Reflections
		&& Effects == Other.Effects
		&& PostProcess == Other.PostProcess
		&& ModelDetail == Other.ModelDetail
		&& Textures == Other.Textures;
}

// ============================================================================ the settings object

UPinguSettings* UPinguSettings::Get()
{
	using namespace PinguSettingsPrivate;
	if (!GInstance || !IsValid(GInstance))
	{
		GInstance = NewObject<UPinguSettings>(GetTransientPackage(), NAME_None);
		GInstance->AddToRoot();
		GInstance->LoadConfig();
		GInstance->Validate();
#if WITH_EDITOR
		if (GIsEditor)
		{
			FGameDelegates::Get().GetEndPlayMapDelegate().AddUObject(GInstance, &UPinguSettings::HandleEndPlayMap);
		}
#endif
		UE_LOG(LogPingu, Log, TEXT("[SETTINGS] loaded: %s; name \"%s\"; volumes %.2f / %.2f / %.2f%s"), *DescribeOptions(GInstance->Graphics),
			*GInstance->GetPlayerName(), GInstance->MasterVolume, GInstance->MusicVolume, GInstance->SfxVolume,
			IsAutomatedRun() ? TEXT(" (automated run: nothing is saved)") : TEXT(""));
	}
	return GInstance;
}

bool UPinguSettings::IsAutomatedRun()
{
	static const bool bAutomated = []()
	{
		const TCHAR* CommandLine = FCommandLine::Get();
		for (const TCHAR* Switch : { TEXT("PinguAutoTest"), TEXT("PinguMatchTest"), TEXT("PinguInputTest"), TEXT("PinguPerfTest"),
			TEXT("PinguMenuShot"), TEXT("PinguMenuShots"), TEXT("PinguNetTest"), TEXT("PinguRoundTest"), TEXT("PinguLobbyTest"),
			TEXT("PinguTraits"), TEXT("PinguTrait"), TEXT("PinguFirstStartTest") })
		{
			if (PinguSettingsPrivate::HasSwitch(CommandLine, Switch))
			{
				return true;
			}
		}
		return APinguNetTest::IsRequested() || FApp::IsUnattended();
	}();
	return bAutomated;
}

void UPinguSettings::Validate()
{
	using namespace PinguSettingsPrivate;
	SanitizeOptions(Graphics);
	if (Graphics.Preset != EPinguGraphicsPreset::Custom)
	{
		// A named preset always uses its current table (a newer build may have tuned it).
		const bool bShowFPS = Graphics.bShowFPS;
		Graphics = GetPresetOptions(Graphics.Preset);
		Graphics.bShowFPS = bShowFPS;
	}
	else
	{
		Graphics.Preset = DetectPreset(Graphics);
	}
	CacheFXDensity(Graphics);

	auto FixVolume = [](float& Volume, float Default)
	{
		Volume = FMath::IsFinite(Volume) ? FMath::Clamp(Volume, 0.f, 1.f) : Default;
	};
	FixVolume(MasterVolume, 1.f);
	FixVolume(MusicVolume, 0.8f);
	FixVolume(SfxVolume, 1.f);
	CameraDistance = FMath::IsFinite(CameraDistance) ? FMath::Clamp(CameraDistance, MinCameraDistance, MaxCameraDistance) : 1.f;
	PlayerName = SanitizePlayerName(PlayerName);

	// ---- key bindings: one entry per action in action order, each key used once, nothing that cannot be bound
	if (BindingsVersion != CurrentBindingsVersion)
	{
		if (KeyBindings.Num() > 0)
		{
			UE_LOG(LogPingu, Display, TEXT("[SETTINGS] key bindings from an older version (%d): back to the defaults"), BindingsVersion);
		}
		ResetBindingsToDefaults();
		return;
	}
	TArray<FPinguKeyBinding> Clean;
	Clean.SetNum(NumActions);
	TArray<bool> bSeen;
	bSeen.Init(false, NumActions);
	TSet<FKey> Used;
	for (int32 Index = 0; Index < NumActions; ++Index)
	{
		Clean[Index].Action = static_cast<EPinguInputAction>(Index);
	}
	auto TakeKey = [&Used](const FKey& Key, FKey& Slot)
	{
		if (IsSet(Key) && CanBind(Key) && !Used.Contains(Key))
		{
			Slot = Key;
			Used.Add(Key);
		}
	};
	for (const FPinguKeyBinding& Saved : KeyBindings)
	{
		const int32 Index = static_cast<int32>(Saved.Action);
		if (Index < 0 || Index >= NumActions || bSeen[Index])
		{
			continue;
		}
		bSeen[Index] = true;
		TakeKey(Saved.Primary, Clean[Index].Primary);
		TakeKey(Saved.Secondary, Clean[Index].Secondary);
	}
	// Actions the saved list does not know yet get their default keys where those are still free.
	for (int32 Index = 0; Index < NumActions; ++Index)
	{
		if (!bSeen[Index])
		{
			FKey Primary;
			FKey Secondary;
			GetDefaultKeys(static_cast<EPinguInputAction>(Index), Primary, Secondary);
			TakeKey(Primary, Clean[Index].Primary);
			TakeKey(Secondary, Clean[Index].Secondary);
		}
	}
	KeyBindings = MoveTemp(Clean);
}

void UPinguSettings::Save()
{
	using namespace PinguSettingsPrivate;
	if (bEditing)
	{
		return; // the settings page is open: nothing is saved before APPLY (ApplyEditSession)
	}
	if (IsAutomatedRun())
	{
		FRuntimeState& S = State();
		if (!S.bSaveSkipLogged)
		{
			S.bSaveSkipLogged = true;
			UE_LOG(LogPingu, Display, TEXT("[SETTINGS] automated run: settings changes are not saved"));
		}
		return;
	}
	SaveConfig();
	if (UGameUserSettings* UserSettings = GetGameUserSettings())
	{
		if (GIsEditor)
		{
			// Only the display options: UGameUserSettings::SaveSettings would also store the game's scalability levels
			// as the editor's own (GEditorSettingsIni), and the editor viewport would start in Low next time.
			UserSettings->SaveConfig();
		}
		else
		{
			// Also stores [ScalabilityGroups], so the next start begins with these quality levels.
			UserSettings->SaveSettings();
		}
	}
	if (GConfig)
	{
		GConfig->Flush(false, GGameUserSettingsIni);
	}
}

// ============================================================================ graphics

void UPinguSettings::SetGraphics(const FPinguGraphicsOptions& NewOptions)
{
	using namespace PinguSettingsPrivate;
	StopFirstStartPickForPlayer();
	FPinguGraphicsOptions Options = NewOptions;
	SanitizeOptions(Options);
	Options.Preset = DetectPreset(Options);
	Graphics = Options;
	CacheFXDensity(Graphics);
	ApplyGraphics(nullptr);
	Save();
}

void UPinguSettings::ApplyPreset(EPinguGraphicsPreset Preset)
{
	using namespace PinguSettingsPrivate;
	StopFirstStartPickForPlayer();
	if (Preset == EPinguGraphicsPreset::Custom || static_cast<uint8>(Preset) > static_cast<uint8>(EPinguGraphicsPreset::Custom))
	{
		// "Custom" is not a table: keep the options as they are.
		Graphics.Preset = DetectPreset(Graphics);
		ApplyGraphics(nullptr);
		Save();
		return;
	}
	if (UGameUserSettings* UserSettings = GetGameUserSettings())
	{
		// Low's 60 fps cap comes and goes with Low; a cap the player chose himself (SetFrameRateLimit) stays. (Settings
		// saved before bPresetFrameCap existed: Low with exactly Low's cap counts as Low's.)
		const int32 PresetCap = GetPresetFrameRateLimit(Preset);
		const int32 CurrentCap = FMath::RoundToInt32(UserSettings->GetFrameRateLimit());
		const int32 LowCap = GetPresetFrameRateLimit(EPinguGraphicsPreset::Low);
		const bool bCapFromPreset = bPresetFrameCap || (Graphics.Preset == EPinguGraphicsPreset::Low && CurrentCap == LowCap);
		if (PresetCap > 0)
		{
			UserSettings->SetFrameRateLimit(static_cast<float>(PresetCap));
			bPresetFrameCap = true;
		}
		else if (bCapFromPreset)
		{
			UserSettings->SetFrameRateLimit(0.f);
			bPresetFrameCap = false;
		}
	}
	FPinguGraphicsOptions Options = GetPresetOptions(Preset);
	Options.bShowFPS = Graphics.bShowFPS;
	Graphics = Options;
	CacheFXDensity(Graphics);
	ApplyGraphics(nullptr);
	Save();
}

FPinguGraphicsOptions UPinguSettings::GetPresetOptions(EPinguGraphicsPreset Preset)
{
	FPinguGraphicsOptions Options;
	auto All = [&Options](EPinguQualityLevel Level)
	{
		Options.Shadows = Level;
		Options.Lighting = Level;
		Options.Reflections = Level;
		Options.Effects = Level;
		Options.PostProcess = Level;
		Options.ModelDetail = Level;
		Options.Textures = Level;
	};
	switch (Preset)
	{
	case EPinguGraphicsPreset::Low:
		All(EPinguQualityLevel::Low);
		Options.ResolutionScale = 67;
		Options.AntiAliasing = EPinguAntiAliasing::TAA;
		break;
	case EPinguGraphicsPreset::Medium:
		All(EPinguQualityLevel::Medium);
		Options.ResolutionScale = 85;
		Options.AntiAliasing = EPinguAntiAliasing::TAA;
		break;
	case EPinguGraphicsPreset::Ultra:
		All(EPinguQualityLevel::Ultra);
		Options.ResolutionScale = 100;
		Options.AntiAliasing = EPinguAntiAliasing::TSR;
		break;
	case EPinguGraphicsPreset::High:
	default:
		// Custom has no table of its own: the High options.
		All(EPinguQualityLevel::High);
		Options.ResolutionScale = 100;
		Options.AntiAliasing = EPinguAntiAliasing::TSR;
		break;
	}
	Options.Preset = Preset;
	Options.bShowFPS = false;
	return Options;
}

int32 UPinguSettings::GetPresetFrameRateLimit(EPinguGraphicsPreset Preset)
{
	return Preset == EPinguGraphicsPreset::Low ? 60 : 0;
}

void UPinguSettings::ApplyGraphics(UWorld* World) const
{
	using namespace PinguSettingsPrivate;
	FRuntimeState& S = State();
	if (World && World->IsGameWorld())
	{
		S.LastWorld = World;
	}
	UWorld* TargetWorld = World ? World : S.LastWorld.Get();

	Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();
	TArray<FCVarSetting> CVars;
	const FPinguGraphicsOptions Effective = EffectiveOptions(Graphics, S.AutoStep);
	BuildRenderSettings(Effective, Levels, CVars);

	const bool bLevelsChange = Scalability::GetQualityLevels() != Levels;
	int32 Changes = 0;
	for (const FCVarSetting& Setting : CVars)
	{
		Changes += NeedsChange(Setting) ? 1 : 0;
	}
	if (bLevelsChange || Changes > 0)
	{
#if WITH_EDITOR
		if (GIsEditor && !S.bEditorLevelsSaved)
		{
			S.EditorLevels = Scalability::GetQualityLevels();
			S.bEditorLevelsSaved = true;
		}
#endif
		BackupCVars(CVars);
		{
			// One render state rebuild for all of it (several of these variables rebuild everything on their own).
			FGlobalComponentRecreateRenderStateContext Recreate;
			if (bLevelsChange)
			{
				Scalability::SetQualityLevels(Levels);
			}
			for (const FCVarSetting& Setting : CVars)
			{
				ApplyCVar(Setting);
			}
			IConsoleManager::Get().CallAllConsoleVariableSinks();
		}
		UE_LOG(LogPingu, Display, TEXT("[SETTINGS] graphics applied (%s): %s%s; %d console variables changed"),
			bLevelsChange ? TEXT("quality levels changed") : TEXT("quality levels kept"), *DescribeOptions(Effective),
			Effective.SameOptionsAs(Graphics) ? TEXT("") : *FString::Printf(TEXT(" (Low adapting: step %d)"), S.AutoStep), Changes);
	}
	// Keep the engine's own copy in sync (UGameUserSettings::ApplyNonResolutionSettings re-applies it).
	if (UGameUserSettings* UserSettings = GetGameUserSettings())
	{
		UserSettings->ScalabilityQuality = Levels;
	}
	ApplyFrameRateLimit();
	ApplyWorldLighting(TargetWorld);
}

void UPinguSettings::ApplyWorldLighting(UWorld* World) const
{
	using namespace PinguSettingsPrivate;
	if (!World || !World->IsGameWorld())
	{
		return;
	}
	FRuntimeState& S = State();
	S.StaticSkyLights.RemoveAll([](const TWeakObjectPtr<USkyLightComponent>& Light) { return !Light.IsValid(); });
	S.Suns.RemoveAll([](const FSunBackup& Sun) { return !Sun.Light.IsValid(); });

	const FPinguGraphicsOptions Effective = EffectiveOptions(Graphics, S.AutoStep);
	// The sky never changes in a round: a real-time capture every frame only pays off with Lumen (today's look).
	const bool bRealTimeSky = Effective.Lighting >= EPinguQualityLevel::High;
	// Cascaded shadows (below High): a tight distance gives all the shadow map resolution to the arena.
	const bool bCascaded = Effective.Shadows == EPinguQualityLevel::Low || Effective.Shadows == EPinguQualityLevel::Medium;
	int32 SkyChanges = 0;
	int32 SunChanges = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TArray<USkyLightComponent*> SkyLights;
		It->GetComponents(SkyLights);
		for (USkyLightComponent* Sky : SkyLights)
		{
			const int32 Known = S.StaticSkyLights.IndexOfByPredicate([Sky](const TWeakObjectPtr<USkyLightComponent>& Light) { return Light.Get() == Sky; });
			if (bRealTimeSky)
			{
				if (Known != INDEX_NONE)
				{
					Sky->SetRealTimeCaptureEnabled(true);
					S.StaticSkyLights.RemoveAtSwap(Known);
					++SkyChanges;
				}
			}
			else if (Known == INDEX_NONE && Sky->IsRealTimeCaptureEnabled())
			{
				Sky->SetRealTimeCaptureEnabled(false);
				Sky->RecaptureSky(); // captured once, with the sky as it is now
				S.StaticSkyLights.Add(Sky);
				++SkyChanges;
			}
		}

		TArray<UDirectionalLightComponent*> Lights;
		It->GetComponents(Lights);
		for (UDirectionalLightComponent* Sun : Lights)
		{
			const int32 Known = S.Suns.IndexOfByPredicate([Sun](const FSunBackup& Backup) { return Backup.Light.Get() == Sun; });
			if (bCascaded)
			{
				if (Known == INDEX_NONE)
				{
					FSunBackup& Backup = S.Suns.AddDefaulted_GetRef();
					Backup.Light = Sun;
					Backup.Distance = Sun->DynamicShadowDistanceMovableLight;
				}
				if (!FMath::IsNearlyEqual(Sun->DynamicShadowDistanceMovableLight, CascadedShadowDistance))
				{
					Sun->SetDynamicShadowDistanceMovableLight(CascadedShadowDistance);
					++SunChanges;
				}
			}
			else if (Known != INDEX_NONE)
			{
				Sun->SetDynamicShadowDistanceMovableLight(S.Suns[Known].Distance);
				S.Suns.RemoveAtSwap(Known);
				++SunChanges;
			}
		}
	}
	if (SkyChanges > 0 || SunChanges > 0)
	{
		UE_LOG(LogPingu, Display, TEXT("[SETTINGS] world lighting: sky light %s (%d changed), sun shadow distance %s (%d changed)"),
			bRealTimeSky ? TEXT("real-time capture") : TEXT("captured once"), SkyChanges,
			bCascaded ? TEXT("80 m cascade") : TEXT("level's own"), SunChanges);
	}
}

int32 UPinguSettings::GetAutoStep() const
{
	return IsAutoResolutionActive() ? PinguSettingsPrivate::State().AutoStep : 0;
}

FPinguGraphicsOptions UPinguSettings::GetEffectiveGraphics() const
{
	return PinguSettingsPrivate::EffectiveOptions(Graphics, PinguSettingsPrivate::State().AutoStep);
}

void UPinguSettings::UpdateAutoResolution(UWorld* World, float RealDeltaSeconds)
{
	using namespace PinguSettingsPrivate;
	if (!World || !World->IsGameWorld() || !GEngine)
	{
		return;
	}
	FRuntimeState& S = State();
	static const int32 ForcedStep = []()
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("PinguAutoResStep="), Value);
		return Value;
	}();
	static const bool bAllowed = !IsAutomatedRun() || FParse::Param(FCommandLine::Get(), TEXT("PinguAutoRes"))
		|| FParse::Param(FCommandLine::Get(), TEXT("PinguAutoResTest"));

	auto ChangeStep = [this, &S, World](int32 NewStep, const FString& Why)
	{
		S.AutoStep = FMath::Clamp(NewStep, 0, NumAutoSteps - 1);
		S.AutoSinceChange = 0.0;
		S.AutoWindowTime = 0.0;
		S.bAutoWindowOpen = false;
		if (S.AutoMonitor.IsValid())
		{
			S.AutoMonitor->Reset();
		}
		UE_LOG(LogPingu, Display, TEXT("[SETTINGS] Low adapts: %s -> step %d: %s"), *Why, S.AutoStep, DescribeAutoStep(S.AutoStep));
		ApplyGraphics(World);
	};

	if (ForcedStep >= 0)
	{
		// -PinguAutoResStep=N (tests): this step, whatever the GPU time.
		if (S.AutoStep != FMath::Clamp(ForcedStep, 0, NumAutoSteps - 1))
		{
			ChangeStep(ForcedStep, TEXT("forced by -PinguAutoResStep"));
		}
		return;
	}
	if (!bAllowed || !IsAutoResolutionActive())
	{
		return;
	}
	if (!S.AutoMonitor.IsValid())
	{
		S.AutoMonitor = MakeShared<FAutoResMonitor>();
		FParse::Value(FCommandLine::Get(), TEXT("PinguAutoResFakeGpuMs="), S.AutoFakeGpuMs);
		S.AutoMonitor->FakeGpuMs = S.AutoFakeGpuMs;
		GEngine->AddPerformanceDataConsumer(S.AutoMonitor);
	}
	FAutoResMonitor& Monitor = *S.AutoMonitor;
	const double Dt = FMath::Clamp(static_cast<double>(RealDeltaSeconds), 0.0, 0.25);
	if (S.AutoWorld.Get() != World)
	{
		// A new world (a match, the menu, Play In Editor): its loading says nothing about the steady cost.
		S.AutoWorld = World;
		S.AutoSinceChange = 0.0;
		S.AutoWindowTime = 0.0;
		S.bAutoWindowOpen = false;
		Monitor.Reset();
	}
	S.AutoSinceChange += Dt;
	S.AutoUpBlockedFor = FMath::Max(0.0, S.AutoUpBlockedFor - Dt);
	if (S.AutoSinceStepUp >= 0.0)
	{
		S.AutoSinceStepUp += Dt;
	}
	if (!S.bAutoWindowOpen)
	{
		if (S.AutoSinceChange >= 1.0)
		{
			Monitor.Reset();
			S.AutoWindowTime = 0.0;
			S.bAutoWindowOpen = true;
		}
		return;
	}
	// Windows of 2 s.
	S.AutoWindowTime += Dt;
	if (S.AutoWindowTime < 2.0 || Monitor.Frames < 20)
	{
		return;
	}
	const double GpuMs = Monitor.AverageGpuMs();
	const int32 Frames = Monitor.Frames;
	Monitor.Reset();
	S.AutoWindowTime = 0.0;
	const int32 Cap = GetFrameRateLimit();
	const double BudgetMs = 1000.0 / static_cast<double>(Cap > 0 ? FMath::Max(Cap, 20) : 60);
	const FString Why = FString::Printf(TEXT("GPU %.1f ms per frame over %d frames, budget %.1f ms (%d fps)"), GpuMs, Frames, BudgetMs, FMath::RoundToInt32(1000.0 / BudgetMs));
	if (GpuMs > 0.9 * BudgetMs && S.AutoStep < NumAutoSteps - 1)
	{
		if (S.AutoSinceStepUp >= 0.0 && S.AutoSinceStepUp < 20.0)
		{
			S.AutoUpBlockedFor = 90.0; // the last step up did not fit after all: stay down for a while
		}
		ChangeStep(S.AutoStep + 1, Why);
	}
	else if (S.AutoStep > 0 && GpuMs < 0.55 * BudgetMs && S.AutoSinceChange >= 10.0 && S.AutoUpBlockedFor <= 0.0)
	{
		S.AutoSinceStepUp = 0.0;
		ChangeStep(S.AutoStep - 1, Why);
	}
}

void UPinguSettings::SetAutoResTestExtraGpuMs(double Milliseconds)
{
	PinguSettingsPrivate::FRuntimeState& S = PinguSettingsPrivate::State();
	S.AutoFakeGpuMs = FMath::Max(0.0, Milliseconds);
	if (S.AutoMonitor.IsValid())
	{
		S.AutoMonitor->FakeGpuMs = S.AutoFakeGpuMs;
	}
}

void UPinguSettings::AutoDetectIfNeeded(UWorld* World)
{
	using namespace PinguSettingsPrivate;
	FRuntimeState& S = State();
	// Tests never pick (nor save), except -PinguFirstStartTest, which picks without saving. In the editor and in a packaged
	// game alike, only a user config without any graphics settings counts as a first start: settings the player already
	// has are never replaced.
	const bool bTest = IsFirstStartTest();
	if (S.bPickRunning || S.bPickDone || (!bTest && (bAutoDetected || IsAutomatedRun())))
	{
		return;
	}
	if (!bTest && HasSavedGraphics(this))
	{
		bAutoDetected = true; // in memory only: nothing of the player's config is rewritten
		UE_LOG(LogPingu, Display, TEXT("[SETTINGS] graphics settings already saved (%s): no first-start pick"),
			*GetPresetName(Graphics.Preset).ToString());
		return;
	}
	if (World && World->IsGameWorld())
	{
		S.LastWorld = World;
	}
	// 1) The engine's hardware benchmark: recorded, and the fallback when no frame can be measured. A short run (about two
	// seconds before the title shows; the full one took 11 s on a laptop), as it does not decide on its own any more.
	const double BenchStart = FPlatformTime::Seconds();
	S.PickFallback = BenchmarkRecommendedPreset(S.PickGPUIndex, S.PickCPUIndex, FirstStartBenchmarkWorkScale);
	S.PickBenchSeconds = FPlatformTime::Seconds() - BenchStart;
	UE_LOG(LogPingu, Display, TEXT("[SETTINGS] first start on this machine: engine benchmark (%.1f s) GPU \"%s\" index %.0f, CPU index %.0f (alone it would pick %s); now measuring the preset High on this PC's own frames"),
		S.PickBenchSeconds, *FPlatformMisc::GetPrimaryGPUBrand(), S.PickGPUIndex, S.PickCPUIndex, *GetPresetName(S.PickFallback).ToString());

	// 2) High on screen; UpdateFirstStartPick measures its frames (the title's bot demo) and picks.
	if (!World || !World->IsGameWorld() || !GEngine || !FApp::CanEverRender())
	{
		FinishFirstStartPick(World, -1.0, 0, TEXT("no game view to measure"));
		return;
	}
	S.bPickRunning = true;
	S.PickWorld = World;
	S.PickFirstStart = FPlatformTime::Seconds();
	S.PickStart = S.PickFirstStart;
	S.PickMonitor = MakeShared<FPickMonitor>();
	GEngine->AddPerformanceDataConsumer(S.PickMonitor);
	S.bPickApplying = true;
	FPinguGraphicsOptions Probe = GetPresetOptions(EPinguGraphicsPreset::High);
	Probe.bShowFPS = Graphics.bShowFPS;
	Graphics = Probe; // in memory only: the pick saves what it chose
	CacheFXDensity(Graphics);
	ApplyGraphics(World);
	S.bPickApplying = false;
}

void UPinguSettings::UpdateFirstStartPick(UWorld* World)
{
	using namespace PinguSettingsPrivate;
	FRuntimeState& S = State();
	if (!S.bPickRunning || !World || !World->IsGameWorld() || !S.PickMonitor.IsValid())
	{
		return;
	}
	FPickMonitor& Monitor = *S.PickMonitor;
	const double Now = FPlatformTime::Seconds();
	if (S.PickWorld.Get() != World)
	{
		// A new world (the player hosted or joined a LAN game): its loading is no steady frame - warm up again.
		S.PickWorld = World;
		S.PickStart = Now;
		Monitor.bRecording = false;
		Monitor.GpuMs.Reset();
		S.PickWindowMedians.Reset();
		S.PickWindowFrames.Reset();
	}
	// The fastest three windows in a row: the middle of their medians (a single light window does not count), and their frames.
	auto FastestWindows = [&S](double& OutMs, int32& OutFrames)
	{
		OutMs = 0.0;
		OutFrames = 0;
		for (int32 First = 0; First + PickStableWindows <= S.PickWindowMedians.Num(); ++First)
		{
			TArray<double> Three;
			int32 Frames = 0;
			for (int32 Index = First; Index < First + PickStableWindows; ++Index)
			{
				Three.Add(S.PickWindowMedians[Index]);
				Frames += S.PickWindowFrames[Index];
			}
			Three.Sort();
			const double Middle = Three[Three.Num() / 2];
			if (OutMs <= 0.0 || Middle < OutMs)
			{
				OutMs = Middle;
				OutFrames = Frames;
			}
		}
	};
	if (Now - S.PickFirstStart > PickGiveUpSeconds)
	{
		FinishFirstStartPick(World, -1.0, Monitor.GpuMs.Num(), TEXT("too few frames with a GPU time in 40 s"));
		return;
	}
	if (Now - S.PickStart < PickWarmupSeconds)
	{
		return;
	}
	if (!Monitor.bRecording)
	{
		Monitor.GpuMs.Reset();
		Monitor.bRecording = true;
		S.PickMeasureStart = Now;
		S.PickWindowStart = Now;
		return;
	}
	if (Now - S.PickWindowStart < PickWindowSeconds)
	{
		return;
	}
	// A window of a second is over. One with too few frames goes on longer (a weak chip's slow frames: up to 4 s, then at
	// least 3 frames count; a hitch or a hidden window's nothing does not).
	if (Monitor.GpuMs.Num() < PickMinWindowFrames && Now - S.PickWindowStart < 4.0 * PickWindowSeconds)
	{
		return;
	}
	if (Monitor.GpuMs.Num() >= 3)
	{
		S.PickWindowMedians.Add(Monitor.MedianGpuMs());
		S.PickWindowFrames.Add(Monitor.GpuMs.Num());
		UE_LOG(LogPingu, Display, TEXT("[SETTINGS] first start measuring: %.1f s in, High %.1f ms GPU per frame (median of %d frames)"),
			Now - S.PickStart, S.PickWindowMedians.Last(), Monitor.GpuMs.Num());
	}
	Monitor.GpuMs.Reset();
	S.PickWindowStart = Now;
	// -PinguFirstStartMinWindows=N (tests): measure at least N windows (to watch how the GPU time settles).
	static const int32 MinWindows = []()
	{
		int32 Value = PickStableWindows;
		FParse::Value(FCommandLine::Get(), TEXT("PinguFirstStartMinWindows="), Value);
		return FMath::Max(Value, PickStableWindows);
	}();
	if (S.PickWindowMedians.Num() < MinWindows)
	{
		return;
	}
	double Ms = 0.0;
	int32 Frames = 0;
	FastestWindows(Ms, Frames);
	// Done early at either end: already fast enough for Ultra (it can only get faster), or so slow that not even a laptop
	// chip's full power (about three times as fast) would reach Medium - Low then, without minutes of a few frames a second.
	const bool bTopAlready = Ms > 0.0 && Ms <= PickUltraMaxMs && HasRayTracing() && S.PickWindowMedians.Num() >= MinWindows;
	const bool bBottomAlready = Ms > 3.0 * PickMediumMaxMs && S.PickWindowMedians.Num() >= MinWindows;
	if ((bTopAlready && Frames >= PickMinFrames) || bBottomAlready || (Now - S.PickMeasureStart >= PickMeasureSeconds && Frames >= PickMinFrames))
	{
		FinishFirstStartPick(World, Ms, Frames, nullptr);
	}
}

void UPinguSettings::FinishFirstStartPick(UWorld* World, double HighGpuMs, int32 Frames, const TCHAR* Problem)
{
	using namespace PinguSettingsPrivate;
	FRuntimeState& S = State();
	S.bPickRunning = false;
	S.bPickDone = true;
	if (S.PickMonitor.IsValid() && GEngine)
	{
		GEngine->RemovePerformanceDataConsumer(S.PickMonitor);
	}
	S.PickMonitor.Reset();

	FIntPoint ViewSize(0, 0);
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		ViewSize = GEngine->GameViewport->Viewport->GetSizeXY();
	}
	const bool bRayTracing = HasRayTracing();
	const bool bMeasured = HighGpuMs > 0.0;
	const EPinguGraphicsPreset Chosen = bMeasured ? ChoosePresetFromFrames(HighGpuMs, bRayTracing, S.PickCPUIndex) : S.PickFallback;
	const FString How = bMeasured
		? FString::Printf(TEXT("High measured %.1f ms GPU per frame (median of %d frames at %dx%d)"), HighGpuMs, Frames, ViewSize.X, ViewSize.Y)
		: FString::Printf(TEXT("no frames measured (%s): the engine benchmark's pick"), Problem ? Problem : TEXT("?"));
	bAutoDetected = true;
	// Shipping builds have no log file, so the result is also kept in the saved config (AutoDetectedWith).
	AutoDetectedWith = FString::Printf(TEXT("%s: GPU \"%s\", %s; engine benchmark GPU index %.0f, CPU index %.0f; ray tracing %s -> %s"),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M")), *FPlatformMisc::GetPrimaryGPUBrand(), *How, S.PickGPUIndex, S.PickCPUIndex,
		bRayTracing ? TEXT("yes") : TEXT("no"), *GetPresetName(Chosen).ToString());
	UE_LOG(LogPingu, Display, TEXT("[SETTINGS] first start pick: %s (High's GPU time: Ultra up to %.0f ms with ray tracing, High up to %.0f, Medium up to %.0f, else Low)"),
		*AutoDetectedWith, PickUltraMaxMs, PickHighMaxMs, PickMediumMaxMs);
	S.bPickApplying = true;
	ApplyPreset(Chosen); // applies and saves (with bAutoDetected and AutoDetectedWith; a test run saves nothing)
	S.bPickApplying = false;

	if (IsFirstStartTest())
	{
		const bool bOk = bMeasured && Frames >= 3 * 3 && Graphics.Preset == Chosen && ViewSize.X > 0;
		UE_LOG(LogPingu, Display, TEXT("[FIRSTSTART] RESULT first start pick: %s, engine benchmark %.1f s (alone: %s), preset now %s, saved nothing (test run): %s"),
			*How, S.PickBenchSeconds, *GetPresetName(S.PickFallback).ToString(), *GetPresetName(Graphics.Preset).ToString(), bOk ? TEXT("ok") : TEXT("FAIL"));
		PinguEndTestRun(World ? static_cast<const UObject*>(World) : GetTransientPackage(), TEXT("first start pick test done"));
	}
}

void UPinguSettings::StopFirstStartPickForPlayer()
{
	using namespace PinguSettingsPrivate;
	FRuntimeState& S = State();
	if (!S.bPickRunning || S.bPickApplying)
	{
		return;
	}
	// The player chose graphics while the first start was still measuring: the player's choice wins.
	S.bPickRunning = false;
	S.bPickDone = true;
	if (S.PickMonitor.IsValid() && GEngine)
	{
		GEngine->RemovePerformanceDataConsumer(S.PickMonitor);
	}
	S.PickMonitor.Reset();
	bAutoDetected = true;
	AutoDetectedWith = FString::Printf(TEXT("%s: GPU \"%s\", the player chose the graphics during the first start's measuring; engine benchmark GPU index %.0f, CPU index %.0f"),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M")), *FPlatformMisc::GetPrimaryGPUBrand(), S.PickGPUIndex, S.PickCPUIndex);
	UE_LOG(LogPingu, Display, TEXT("[SETTINGS] first start pick stopped: %s"), *AutoDetectedWith);
}

EPinguGraphicsPreset UPinguSettings::BenchmarkRecommendedPreset(float& OutGPUIndex, float& OutCPUIndex, int32 WorkScale)
{
	OutGPUIndex = -1.f;
	OutCPUIndex = -1.f;
	UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	if (UserSettings && FApp::CanEverRender())
	{
		// The engine's synthetic benchmark (WorkScale 10: 1 s on a desktop, 11 s on a laptop). It only fills in the game user
		// settings' suggested quality levels in memory; ApplyGraphics replaces them with the preset's.
		UserSettings->RunHardwareBenchmark(FMath::Max(1, WorkScale));
		OutGPUIndex = UserSettings->GetLastGPUBenchmarkResult();
		OutCPUIndex = UserSettings->GetLastCPUBenchmarkResult();
	}
	return PinguSettingsPrivate::ChoosePreset(OutGPUIndex, OutCPUIndex, FPlatformMisc::GetPrimaryGPUBrand(), PinguSettingsPrivate::HasRayTracing());
}

// ============================================================================ display

bool UPinguSettings::IsNativeResolutionMode(EWindowMode::Type Mode)
{
	return Mode == EWindowMode::Fullscreen || Mode == EWindowMode::WindowedFullscreen;
}

FIntPoint UPinguSettings::ResolveResolution(EWindowMode::Type Mode, FIntPoint Wanted, FIntPoint Native)
{
	return IsNativeResolutionMode(Mode) && Native.X > 0 && Native.Y > 0 ? Native : Wanted;
}

FIntPoint UPinguSettings::GetNativeResolution()
{
	// The monitor the game window is on (its middle decides), at its native pixels; before a window exists (or when the platform
	// lists no monitors) the engine's desktop resolution of the primary monitor.
	FDisplayMetrics Metrics;
	FDisplayMetrics::RebuildDisplayMetrics(Metrics);
	if (GEngine && GEngine->GameViewport && Metrics.MonitorInfo.Num() > 0)
	{
		const TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow();
		if (Window.IsValid())
		{
			const FVector2f Position = Window->GetPositionInScreen();
			const FVector2f Middle = Position + 0.5f * Window->GetSizeInScreen();
			for (const FMonitorInfo& Monitor : Metrics.MonitorInfo)
			{
				const FPlatformRect& Rect = Monitor.DisplayRect;
				if (Middle.X >= Rect.Left && Middle.X < Rect.Right && Middle.Y >= Rect.Top && Middle.Y < Rect.Bottom && Monitor.NativeWidth > 0 && Monitor.NativeHeight > 0)
				{
					return FIntPoint(Monitor.NativeWidth, Monitor.NativeHeight);
				}
			}
		}
	}
	const UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	return UserSettings ? UserSettings->GetDesktopResolution() : FIntPoint(1920, 1080);
}

namespace PinguSettingsPrivate
{
	/** A window as big as the screen would hide its title bar: the biggest convenient window size under the screen (or 3/4 of it)
	 *  when Current fills the screen, else Current. */
	static FIntPoint PickWindowedResolution(FIntPoint Desktop, FIntPoint Current)
	{
		if (Desktop.X <= 0 || Desktop.Y <= 0 || (Current.X < Desktop.X && Current.Y < Desktop.Y))
		{
			return Current;
		}
		TArray<FIntPoint> Windowed;
		UKismetSystemLibrary::GetConvenientWindowedResolutions(Windowed);
		FIntPoint Best(FMath::RoundToInt(Desktop.X * 0.75f), FMath::RoundToInt(Desktop.Y * 0.75f));
		for (const FIntPoint& Size : Windowed)
		{
			if (Size.X < Desktop.X && Size.Y < Desktop.Y && Size.X * Size.Y > Best.X * Best.Y)
			{
				Best = Size;
			}
		}
		return Best;
	}
}

EWindowMode::Type UPinguSettings::GetWindowMode() const
{
	if (bEditing)
	{
		return DraftMode;
	}
	const UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	return UserSettings ? UserSettings->GetFullscreenMode() : EWindowMode::Windowed;
}

void UPinguSettings::SetWindowMode(EWindowMode::Type Mode)
{
	UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	if (!UserSettings)
	{
		return;
	}
	const FIntPoint Native = GetNativeResolution();
	if (bEditing)
	{
		// Only the draft: the window changes on APPLY.
		DraftMode = Mode;
		DraftResolution = IsNativeResolutionMode(Mode) ? ResolveResolution(Mode, DraftResolution, Native)
			: (Mode == EWindowMode::Windowed ? PinguSettingsPrivate::PickWindowedResolution(Native, DraftResolution) : DraftResolution);
		return;
	}
	UserSettings->SetFullscreenMode(Mode);
	if (IsNativeResolutionMode(Mode))
	{
		UserSettings->SetScreenResolution(ResolveResolution(Mode, UserSettings->GetScreenResolution(), Native)); // always the whole screen
	}
	else if (Mode == EWindowMode::Windowed)
	{
		UserSettings->SetScreenResolution(PinguSettingsPrivate::PickWindowedResolution(Native, UserSettings->GetScreenResolution()));
	}
	ApplyResolution();
	Save();
}

FIntPoint UPinguSettings::GetResolution() const
{
	if (bEditing)
	{
		return DraftResolution;
	}
	const UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	if (!UserSettings)
	{
		return FIntPoint(1920, 1080);
	}
	const FIntPoint Resolution = UserSettings->GetScreenResolution();
	return (Resolution.X > 0 && Resolution.Y > 0) ? Resolution : UserSettings->GetDesktopResolution();
}

void UPinguSettings::SetResolution(FIntPoint Resolution)
{
	UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	if (!UserSettings || Resolution.X < 640 || Resolution.Y < 360 || IsNativeResolutionMode(GetWindowMode()))
	{
		return; // Fullscreen and Borderless are always native: the choice is locked
	}
	if (bEditing)
	{
		DraftResolution = Resolution;
		return;
	}
	UserSettings->SetScreenResolution(Resolution);
	ApplyResolution();
	Save();
}

void UPinguSettings::EnforceNativeResolution()
{
	UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	if (!UserSettings || GIsEditor || IsAutomatedRun() || bEditing || !IsNativeResolutionMode(UserSettings->GetFullscreenMode()))
	{
		return;
	}
	const FIntPoint Native = GetNativeResolution();
	const FIntPoint Saved = UserSettings->GetScreenResolution();
	if (Native.X <= 0 || Native.Y <= 0 || Saved == Native)
	{
		return;
	}
	UE_LOG(LogPingu, Display, TEXT("[SETTINGS] fullscreen / borderless runs at the monitor's native resolution: %d x %d (saved %d x %d)"), Native.X, Native.Y, Saved.X, Saved.Y);
	UserSettings->SetScreenResolution(Native);
	ApplyResolution();
	Save();
}

TArray<FIntPoint> UPinguSettings::GetSupportedResolutions() const
{
	TArray<FIntPoint> Found;
	if (GetWindowMode() == EWindowMode::Windowed)
	{
		UKismetSystemLibrary::GetConvenientWindowedResolutions(Found);
	}
	if (Found.Num() == 0)
	{
		UKismetSystemLibrary::GetSupportedFullscreenResolutions(Found);
	}
	Found.Add(GetResolution());
	TArray<FIntPoint> Result;
	for (const FIntPoint& Size : Found)
	{
		if (Size.X >= 640 && Size.Y >= 360 && !Result.Contains(Size))
		{
			Result.Add(Size);
		}
	}
	Result.Sort([](const FIntPoint& A, const FIntPoint& B)
	{
		const int64 AreaA = static_cast<int64>(A.X) * A.Y;
		const int64 AreaB = static_cast<int64>(B.X) * B.Y;
		return AreaA != AreaB ? AreaA < AreaB : A.X < B.X;
	});
	return Result;
}

void UPinguSettings::ApplyResolution() const
{
	UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	// Tests keep their window (and -windowed / -ResX / -ResY always win: the command line overrides are applied).
	// In the editor the PIE viewport belongs to the editor: the choice is only stored.
	if (!UserSettings || IsAutomatedRun() || GIsEditor)
	{
		return;
	}
	UserSettings->ApplyResolutionSettings(true);
	UserSettings->ConfirmVideoMode();
}

bool UPinguSettings::GetVSync() const
{
	const UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	return UserSettings && UserSettings->IsVSyncEnabled();
}

void UPinguSettings::SetVSync(bool bEnabled)
{
	UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	if (!UserSettings)
	{
		return;
	}
	UserSettings->SetVSyncEnabled(bEnabled);
	if (!GIsEditor)
	{
		// What UGameUserSettings::ApplyNonResolutionSettings does (an r.VSync in the engine ini still wins).
		if (IConsoleVariable* CVar = PinguSettingsPrivate::FindCVar(TEXT("r.VSync")))
		{
			CVar->Set(bEnabled ? 1 : 0, ECVF_SetByGameSetting);
		}
	}
	else
	{
		ApplyGraphics(nullptr); // Play In Editor: the editor window's vsync while playing (BuildRenderSettings)
	}
	Save();
}

int32 UPinguSettings::GetFrameRateLimit() const
{
	const UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	return UserSettings ? FMath::Max(0, FMath::RoundToInt(UserSettings->GetFrameRateLimit())) : 0;
}

void UPinguSettings::SetFrameRateLimit(int32 FramesPerSecond)
{
	UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	if (!UserSettings)
	{
		return;
	}
	UserSettings->SetFrameRateLimit(static_cast<float>(FMath::Clamp(FramesPerSecond, 0, 1000)));
	bPresetFrameCap = false; // the player's own cap: presets leave it alone
	ApplyFrameRateLimit();
	Save();
}

void UPinguSettings::ApplyFrameRateLimit() const
{
	using namespace PinguSettingsPrivate;
	const UGameUserSettings* UserSettings = GetGameUserSettings();
	if (!UserSettings || !GEngine)
	{
		return;
	}
	const float Wanted = FMath::Max(UserSettings->GetFrameRateLimit(), 0.f);
	if (FMath::IsNearlyEqual(GEngine->GetMaxFPS(), Wanted))
	{
		return;
	}
#if WITH_EDITOR
	FRuntimeState& S = State();
	if (GIsEditor && !S.bEditorMaxFPSSaved)
	{
		S.EditorMaxFPS = GEngine->GetMaxFPS();
		S.bEditorMaxFPSSaved = true;
	}
#endif
	GEngine->SetMaxFPS(Wanted);
}

// ============================================================================ the settings page's edit session

UPinguSettings::FEditState UPinguSettings::CaptureEditState() const
{
	FEditState State;
	State.Graphics = Graphics;
	State.bPresetFrameCap = bPresetFrameCap;
	State.Master = MasterVolume;
	State.Music = MusicVolume;
	State.Sfx = SfxVolume;
	State.CameraDistance = CameraDistance;
	State.bJoin = bJoinRunningMatches;
	State.Keys = KeyBindings;
	State.Mode = GetWindowMode();
	State.Resolution = GetResolution();
	State.bVSync = GetVSync();
	State.FrameCap = GetFrameRateLimit();
	return State;
}

void UPinguSettings::BeginEditSession()
{
	if (bEditing)
	{
		return;
	}
	const UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	DraftMode = UserSettings ? UserSettings->GetFullscreenMode() : EWindowMode::Windowed;
	DraftResolution = UserSettings ? UserSettings->GetScreenResolution() : FIntPoint(1920, 1080);
	if (DraftResolution.X <= 0 || DraftResolution.Y <= 0)
	{
		DraftResolution = UserSettings ? UserSettings->GetDesktopResolution() : FIntPoint(1920, 1080);
	}
	EditStart = CaptureEditState(); // (reads GetWindowMode / GetResolution: the draft is what the window has now)
	bEditing = true;
}

bool UPinguSettings::HasPendingChanges() const
{
	if (!bEditing)
	{
		return false;
	}
	const FEditState Now = CaptureEditState();
	const FEditState& Was = EditStart;
	if (Now.Graphics.Preset != Was.Graphics.Preset || !Now.Graphics.SameOptionsAs(Was.Graphics) || Now.Graphics.bShowFPS != Was.Graphics.bShowFPS)
	{
		return true;
	}
	if (!FMath::IsNearlyEqual(Now.Master, Was.Master, 0.0001f) || !FMath::IsNearlyEqual(Now.Music, Was.Music, 0.0001f) || !FMath::IsNearlyEqual(Now.Sfx, Was.Sfx, 0.0001f)
		|| !FMath::IsNearlyEqual(Now.CameraDistance, Was.CameraDistance, 0.0001f) || Now.bJoin != Was.bJoin)
	{
		return true;
	}
	if (Now.Mode != Was.Mode || Now.Resolution != Was.Resolution || Now.bVSync != Was.bVSync || Now.FrameCap != Was.FrameCap)
	{
		return true;
	}
	if (Now.Keys.Num() != Was.Keys.Num())
	{
		return true;
	}
	for (int32 Index = 0; Index < Now.Keys.Num(); ++Index)
	{
		if (Now.Keys[Index].Action != Was.Keys[Index].Action || Now.Keys[Index].Primary != Was.Keys[Index].Primary || Now.Keys[Index].Secondary != Was.Keys[Index].Secondary)
		{
			return true;
		}
	}
	return false;
}

void UPinguSettings::ApplyEditSession()
{
	if (!bEditing)
	{
		return;
	}
	UGameUserSettings* UserSettings = PinguSettingsPrivate::GetGameUserSettings();
	const bool bDisplayChanged = UserSettings && (UserSettings->GetFullscreenMode() != DraftMode || UserSettings->GetScreenResolution() != DraftResolution);
	if (bDisplayChanged)
	{
		UserSettings->SetFullscreenMode(DraftMode);
		UserSettings->SetScreenResolution(DraftResolution);
	}
	// Saved for real (Save() waits while a session runs), then the display goes to the window.
	bEditing = false;
	Save();
	if (bDisplayChanged)
	{
		ApplyResolution();
	}
	bEditing = true;
	EditStart = CaptureEditState();
	UE_LOG(LogPingu, Display, TEXT("[SETTINGS] APPLY: saved%s"), bDisplayChanged ? TEXT(" (the window mode / resolution changed)") : TEXT(""));
}

void UPinguSettings::DiscardEditSession(UWorld* World)
{
	using namespace PinguSettingsPrivate;
	if (!bEditing)
	{
		return;
	}
	const FEditState Was = EditStart;
	// Every setter below runs with the session on: nothing is saved, the live previews just go back.
	Graphics = Was.Graphics;
	CacheFXDensity(Graphics);
	MasterVolume = Was.Master;
	MusicVolume = Was.Music;
	SfxVolume = Was.Sfx;
	CameraDistance = Was.CameraDistance;
	bJoinRunningMatches = Was.bJoin;
	if (UGameUserSettings* UserSettings = GetGameUserSettings())
	{
		UserSettings->SetFrameRateLimit(static_cast<float>(Was.FrameCap));
	}
	bPresetFrameCap = Was.bPresetFrameCap;
	SetVSync(Was.bVSync);
	ApplyGraphics(World); // also the frame cap
	ApplyAudio(World);
	DraftMode = Was.Mode;
	DraftResolution = Was.Resolution;
	bool bKeysChanged = KeyBindings.Num() != Was.Keys.Num();
	for (int32 Index = 0; !bKeysChanged && Index < Was.Keys.Num(); ++Index)
	{
		bKeysChanged = KeyBindings[Index].Action != Was.Keys[Index].Action || KeyBindings[Index].Primary != Was.Keys[Index].Primary || KeyBindings[Index].Secondary != Was.Keys[Index].Secondary;
	}
	if (bKeysChanged)
	{
		KeyBindings = Was.Keys;
		OnBindingsChanged.Broadcast();
	}
	UE_LOG(LogPingu, Display, TEXT("[SETTINGS] DISCARD: the values from before the settings page are back"));
}

void UPinguSettings::EndEditSession(UWorld* World)
{
	if (!bEditing)
	{
		return;
	}
	if (HasPendingChanges())
	{
		DiscardEditSession(World);
	}
	bEditing = false;
}

float UPinguSettings::GetFXDensity()
{
	// Called per projectile per frame and per effect: a plain cached value (refreshed whenever the options change).
	if (!PinguSettingsPrivate::GInstance)
	{
		Get();
	}
	return PinguSettingsPrivate::GFXDensity;
}

bool UPinguSettings::UseUltraEffects()
{
	if (!PinguSettingsPrivate::GInstance)
	{
		Get();
	}
	return PinguSettingsPrivate::GFXUltra;
}

FText UPinguSettings::GetPresetName(EPinguGraphicsPreset Preset)
{
	switch (Preset)
	{
	case EPinguGraphicsPreset::Low: return LOCTEXT("PresetLow", "Low");
	case EPinguGraphicsPreset::Medium: return LOCTEXT("PresetMedium", "Medium");
	case EPinguGraphicsPreset::High: return LOCTEXT("PresetHigh", "High");
	case EPinguGraphicsPreset::Ultra: return LOCTEXT("PresetUltra", "Ultra");
	default: return LOCTEXT("PresetCustom", "Custom");
	}
}

FText UPinguSettings::GetQualityName(EPinguQualityLevel Level)
{
	switch (Level)
	{
	case EPinguQualityLevel::Off: return LOCTEXT("QualityOff", "Off");
	case EPinguQualityLevel::Low: return LOCTEXT("QualityLow", "Low");
	case EPinguQualityLevel::Medium: return LOCTEXT("QualityMedium", "Medium");
	case EPinguQualityLevel::High: return LOCTEXT("QualityHigh", "High");
	default: return LOCTEXT("QualityUltra", "Ultra");
	}
}

FText UPinguSettings::GetAntiAliasingName(EPinguAntiAliasing Method)
{
	switch (Method)
	{
	case EPinguAntiAliasing::Off: return LOCTEXT("AAOff", "Off");
	case EPinguAntiAliasing::FXAA: return LOCTEXT("AAFXAA", "FXAA");
	case EPinguAntiAliasing::TAA: return LOCTEXT("AATAA", "TAA");
	default: return LOCTEXT("AATSR", "TSR");
	}
}

FText UPinguSettings::GetPresetDescription(EPinguGraphicsPreset Preset)
{
	switch (Preset)
	{
	case EPinguGraphicsPreset::Low: return LOCTEXT("PresetLowInfo", "For ordinary laptops: simple lighting and shadows, 60 fps. Lowers the 3D resolution by itself when the PC can't keep up.");
	case EPinguGraphicsPreset::Medium: return LOCTEXT("PresetMediumInfo", "For gaming laptops and older graphics cards.");
	case EPinguGraphicsPreset::High: return LOCTEXT("PresetHighInfo", "The full look: Lumen lighting and soft shadows.");
	case EPinguGraphicsPreset::Ultra: return LOCTEXT("PresetUltraInfo", "Everything at its best, with ray-traced reflections. Needs a fast graphics card.");
	default: return LOCTEXT("PresetCustomInfo", "Your own mix of options.");
	}
}

// ============================================================================ audio

void UPinguSettings::SetVolumes(float InMaster, float InMusic, float InSfx)
{
	MasterVolume = FMath::IsFinite(InMaster) ? FMath::Clamp(InMaster, 0.f, 1.f) : MasterVolume;
	MusicVolume = FMath::IsFinite(InMusic) ? FMath::Clamp(InMusic, 0.f, 1.f) : MusicVolume;
	SfxVolume = FMath::IsFinite(InSfx) ? FMath::Clamp(InSfx, 0.f, 1.f) : SfxVolume;
	ApplyAudio(nullptr);
	Save();
}

void UPinguSettings::SetCameraDistance(float InDistance)
{
	if (!FMath::IsFinite(InDistance))
	{
		return;
	}
	CameraDistance = FMath::Clamp(InDistance, MinCameraDistance, MaxCameraDistance);
	Save();
}

bool UPinguSettings::GetJoinRunningMatches() const
{
	return bJoinRunningMatches && !FParse::Param(FCommandLine::Get(), TEXT("PinguNoHotJoin"));
}

void UPinguSettings::SetJoinRunningMatches(bool bInJoin)
{
	bJoinRunningMatches = bInJoin;
	Save();
}

void UPinguSettings::SetChooseAbilities(bool bInChoose)
{
	bChooseAbilities = bInChoose;
	Save();
}

void UPinguSettings::ApplyAudio(const UObject* WorldContext) const
{
	using namespace PinguSettingsPrivate;
	FRuntimeState& S = State();
	UWorld* World = WorldContext ? WorldContext->GetWorld() : S.LastWorld.Get();
	if (!World || !World->IsGameWorld())
	{
		return;
	}
	S.LastWorld = World;
	APinguFX::SetVolumes(World, SfxMix * SfxVolume * MasterVolume, MusicMix * MusicVolume * MasterVolume);
}

// ============================================================================ controls

FPinguKeyBinding* UPinguSettings::FindBinding(EPinguInputAction Action)
{
	const int32 Index = static_cast<int32>(Action);
	if (KeyBindings.IsValidIndex(Index) && KeyBindings[Index].Action == Action)
	{
		return &KeyBindings[Index];
	}
	return KeyBindings.FindByPredicate([Action](const FPinguKeyBinding& Binding) { return Binding.Action == Action; });
}

const FPinguKeyBinding* UPinguSettings::FindBinding(EPinguInputAction Action) const
{
	return const_cast<UPinguSettings*>(this)->FindBinding(Action);
}

bool UPinguSettings::IsKeyBound(const FKey& Key) const
{
	for (const FPinguKeyBinding& Binding : KeyBindings)
	{
		if (Binding.Primary == Key || Binding.Secondary == Key)
		{
			return true;
		}
	}
	return false;
}

TArray<FKey> UPinguSettings::GetKeys(EPinguInputAction Action) const
{
	using namespace PinguSettingsPrivate;
	TArray<FKey> Keys;
	if (const FPinguKeyBinding* Binding = FindBinding(Action))
	{
		if (IsSet(Binding->Primary))
		{
			Keys.Add(Binding->Primary);
		}
		if (IsSet(Binding->Secondary) && Binding->Secondary != Binding->Primary)
		{
			Keys.Add(Binding->Secondary);
		}
	}
	const FKey Fallback = GetFallbackKey(Action);
	if (IsSet(Fallback))
	{
		Keys.AddUnique(Fallback);
	}
	return Keys;
}

FKey UPinguSettings::GetKey(EPinguInputAction Action, int32 Index) const
{
	const FPinguKeyBinding* Binding = FindBinding(Action);
	if (!Binding || Index < 0 || Index > 1)
	{
		return EKeys::Invalid;
	}
	const FKey& Key = Index == 0 ? Binding->Primary : Binding->Secondary;
	return PinguSettingsPrivate::IsSet(Key) ? Key : EKeys::Invalid;
}

FKey UPinguSettings::GetFallbackKey(EPinguInputAction Action) const
{
	if (Action != EPinguInputAction::Ability2)
	{
		return EKeys::Invalid;
	}
	const FPinguKeyBinding* Binding = FindBinding(Action);
	const bool bOnMiddleButton = Binding && (Binding->Primary == EKeys::MiddleMouseButton || Binding->Secondary == EKeys::MiddleMouseButton);
	return (bOnMiddleButton && !IsKeyBound(EKeys::E)) ? EKeys::E : EKeys::Invalid;
}

void UPinguSettings::SetKey(EPinguInputAction Action, int32 Index, const FKey& Key)
{
	FPinguKeyBinding* Binding = FindBinding(Action);
	if (!Binding || Index < 0 || Index > 1 || !CanBind(Key))
	{
		return;
	}
	FKey& Slot = Index == 0 ? Binding->Primary : Binding->Secondary;
	if (Slot != Key)
	{
		// One key, one action: whoever had it loses it (also the other slot of this action).
		for (FPinguKeyBinding& Other : KeyBindings)
		{
			if (Other.Primary == Key)
			{
				Other.Primary = FKey();
			}
			if (Other.Secondary == Key)
			{
				Other.Secondary = FKey();
			}
		}
		Slot = Key;
		UE_LOG(LogPingu, Display, TEXT("[SETTINGS] %s key %d = %s"), *GetActionName(Action).ToString(), Index + 1, *Key.ToString());
		Save();
	}
	// Also when nothing changed: listeners rebuild their mapping after every accepted call.
	OnBindingsChanged.Broadcast();
}

void UPinguSettings::ClearKey(EPinguInputAction Action, int32 Index)
{
	FPinguKeyBinding* Binding = FindBinding(Action);
	if (!Binding || Index < 0 || Index > 1)
	{
		return;
	}
	FKey& Slot = Index == 0 ? Binding->Primary : Binding->Secondary;
	if (PinguSettingsPrivate::IsSet(Slot))
	{
		UE_LOG(LogPingu, Display, TEXT("[SETTINGS] %s key %d cleared (was %s)"), *GetActionName(Action).ToString(), Index + 1, *Slot.ToString());
		Slot = FKey();
		Save();
	}
	OnBindingsChanged.Broadcast();
}

void UPinguSettings::ResetBindingsToDefaults()
{
	using namespace PinguSettingsPrivate;
	KeyBindings.Reset();
	KeyBindings.SetNum(NumActions);
	for (int32 Index = 0; Index < NumActions; ++Index)
	{
		FPinguKeyBinding& Binding = KeyBindings[Index];
		Binding.Action = static_cast<EPinguInputAction>(Index);
		GetDefaultKeys(Binding.Action, Binding.Primary, Binding.Secondary);
	}
	BindingsVersion = CurrentBindingsVersion;
}

void UPinguSettings::ResetKeyBindings()
{
	ResetBindingsToDefaults();
	UE_LOG(LogPingu, Display, TEXT("[SETTINGS] key bindings reset to the defaults"));
	Save();
	OnBindingsChanged.Broadcast();
}

TArray<FKey> UPinguSettings::GetGamepadKeys(EPinguInputAction Action)
{
	switch (Action)
	{
	case EPinguInputAction::MoveLeft: return { EKeys::Gamepad_DPad_Left, EKeys::Gamepad_LeftStick_Left };
	case EPinguInputAction::MoveRight: return { EKeys::Gamepad_DPad_Right, EKeys::Gamepad_LeftStick_Right };
	case EPinguInputAction::Up: return { EKeys::Gamepad_DPad_Up, EKeys::Gamepad_LeftStick_Up };
	case EPinguInputAction::Down: return { EKeys::Gamepad_DPad_Down, EKeys::Gamepad_LeftStick_Down };
	case EPinguInputAction::Jump: return { EKeys::Gamepad_FaceButton_Bottom };
	case EPinguInputAction::Dash: return { EKeys::Gamepad_RightShoulder, EKeys::Gamepad_RightTrigger };
	case EPinguInputAction::Ability1: return { EKeys::Gamepad_FaceButton_Left };
	case EPinguInputAction::Ability2: return { EKeys::Gamepad_FaceButton_Top };
	case EPinguInputAction::Ability3: return { EKeys::Gamepad_FaceButton_Right };
	case EPinguInputAction::Pause: return { EKeys::Gamepad_Special_Right };
	// The left shoulder buttons mirror the dash on the right ones (LB also flips the settings pages, but only in menus).
	case EPinguInputAction::BellySlide: return { EKeys::Gamepad_LeftShoulder, EKeys::Gamepad_LeftTrigger };
	default: return {};
	}
}

bool UPinguSettings::CanBind(const FKey& Key)
{
	if (!PinguSettingsPrivate::IsSet(Key) || Key.IsGamepadKey() || Key.IsTouch() || Key.IsGesture() || Key.IsAnalog()
		|| Key.IsDeprecated() || !Key.IsBindableToActions())
	{
		return false;
	}
	// Escape always pauses / backs out, Backspace clears a key on the controls screen, the Windows keys open the Start
	// menu, and the mouse wheel would bind itself while scrolling a menu.
	static const FKey Reserved[] = { EKeys::Escape, EKeys::BackSpace, EKeys::LeftCommand, EKeys::RightCommand,
		EKeys::MouseScrollUp, EKeys::MouseScrollDown, EKeys::MouseWheelAxis, EKeys::AnyKey };
	for (const FKey& Blocked : Reserved)
	{
		if (Key == Blocked)
		{
			return false;
		}
	}
	return true;
}

FText UPinguSettings::GetActionName(EPinguInputAction Action)
{
	switch (Action)
	{
	case EPinguInputAction::MoveLeft: return LOCTEXT("ActionMoveLeft", "Move left");
	case EPinguInputAction::MoveRight: return LOCTEXT("ActionMoveRight", "Move right");
	case EPinguInputAction::Up: return LOCTEXT("ActionUp", "Aim up");
	case EPinguInputAction::Down: return LOCTEXT("ActionDown", "Aim down / fall fast");
	case EPinguInputAction::Jump: return LOCTEXT("ActionJump", "Jump");
	case EPinguInputAction::Dash: return LOCTEXT("ActionDash", "Dash");
	case EPinguInputAction::Ability1: return LOCTEXT("ActionAbility1", "Left ability");
	case EPinguInputAction::Ability2: return LOCTEXT("ActionAbility2", "Top ability");
	case EPinguInputAction::Ability3: return LOCTEXT("ActionAbility3", "Right ability");
	case EPinguInputAction::Pause: return LOCTEXT("ActionPause", "Pause");
	case EPinguInputAction::BellySlide: return LOCTEXT("ActionBellySlide", "Belly slide");
	default: return FText::GetEmpty();
	}
}

FText UPinguSettings::GetKeyLabel(const FKey& Key)
{
	if (!PinguSettingsPrivate::IsSet(Key))
	{
		return FText::GetEmpty();
	}
	struct FLabel
	{
		FKey Key;
		const TCHAR* Text;
	};
	static const FLabel Labels[] = {
		{ EKeys::SpaceBar, TEXT("SPACE") }, { EKeys::LeftShift, TEXT("SHIFT") }, { EKeys::RightShift, TEXT("R-SHIFT") },
		{ EKeys::LeftControl, TEXT("CTRL") }, { EKeys::RightControl, TEXT("R-CTRL") }, { EKeys::LeftAlt, TEXT("ALT") },
		{ EKeys::RightAlt, TEXT("R-ALT") }, { EKeys::LeftCommand, TEXT("WIN") }, { EKeys::RightCommand, TEXT("WIN") },
		{ EKeys::Enter, TEXT("ENTER") }, { EKeys::Tab, TEXT("TAB") }, { EKeys::BackSpace, TEXT("BACKSPACE") },
		{ EKeys::Escape, TEXT("ESC") }, { EKeys::CapsLock, TEXT("CAPS") }, { EKeys::Pause, TEXT("PAUSE") },
		{ EKeys::Left, TEXT("LEFT") }, { EKeys::Right, TEXT("RIGHT") }, { EKeys::Up, TEXT("UP") }, { EKeys::Down, TEXT("DOWN") },
		{ EKeys::Insert, TEXT("INS") }, { EKeys::Delete, TEXT("DEL") }, { EKeys::Home, TEXT("HOME") }, { EKeys::End, TEXT("END") },
		{ EKeys::PageUp, TEXT("PG UP") }, { EKeys::PageDown, TEXT("PG DN") },
		{ EKeys::LeftMouseButton, TEXT("LMB") }, { EKeys::RightMouseButton, TEXT("RMB") }, { EKeys::MiddleMouseButton, TEXT("MMB") },
		{ EKeys::ThumbMouseButton, TEXT("MB4") }, { EKeys::ThumbMouseButton2, TEXT("MB5") },
		{ EKeys::MouseScrollUp, TEXT("WHEEL UP") }, { EKeys::MouseScrollDown, TEXT("WHEEL DOWN") },
		{ EKeys::Zero, TEXT("0") }, { EKeys::One, TEXT("1") }, { EKeys::Two, TEXT("2") }, { EKeys::Three, TEXT("3") }, { EKeys::Four, TEXT("4") },
		{ EKeys::Five, TEXT("5") }, { EKeys::Six, TEXT("6") }, { EKeys::Seven, TEXT("7") }, { EKeys::Eight, TEXT("8") }, { EKeys::Nine, TEXT("9") },
		{ EKeys::NumPadZero, TEXT("NUM 0") }, { EKeys::NumPadOne, TEXT("NUM 1") }, { EKeys::NumPadTwo, TEXT("NUM 2") },
		{ EKeys::NumPadThree, TEXT("NUM 3") }, { EKeys::NumPadFour, TEXT("NUM 4") }, { EKeys::NumPadFive, TEXT("NUM 5") },
		{ EKeys::NumPadSix, TEXT("NUM 6") }, { EKeys::NumPadSeven, TEXT("NUM 7") }, { EKeys::NumPadEight, TEXT("NUM 8") },
		{ EKeys::NumPadNine, TEXT("NUM 9") }, { EKeys::Multiply, TEXT("NUM *") }, { EKeys::Add, TEXT("NUM +") },
		{ EKeys::Subtract, TEXT("NUM -") }, { EKeys::Decimal, TEXT("NUM .") }, { EKeys::Divide, TEXT("NUM /") },
		{ EKeys::Semicolon, TEXT(";") }, { EKeys::Equals, TEXT("=") }, { EKeys::Comma, TEXT(",") }, { EKeys::Hyphen, TEXT("-") },
		{ EKeys::Underscore, TEXT("_") }, { EKeys::Period, TEXT(".") }, { EKeys::Slash, TEXT("/") }, { EKeys::Tilde, TEXT("`") },
		{ EKeys::LeftBracket, TEXT("[") }, { EKeys::Backslash, TEXT("\\") }, { EKeys::RightBracket, TEXT("]") },
		{ EKeys::Apostrophe, TEXT("'") },
		// gamepad (Xbox names, for the help overlay)
		{ EKeys::Gamepad_FaceButton_Bottom, TEXT("A") }, { EKeys::Gamepad_FaceButton_Right, TEXT("B") },
		{ EKeys::Gamepad_FaceButton_Left, TEXT("X") }, { EKeys::Gamepad_FaceButton_Top, TEXT("Y") },
		{ EKeys::Gamepad_LeftShoulder, TEXT("LB") }, { EKeys::Gamepad_RightShoulder, TEXT("RB") },
		{ EKeys::Gamepad_LeftTrigger, TEXT("LT") }, { EKeys::Gamepad_RightTrigger, TEXT("RT") },
		{ EKeys::Gamepad_Special_Right, TEXT("START") }, { EKeys::Gamepad_Special_Left, TEXT("BACK") },
		{ EKeys::Gamepad_LeftThumbstick, TEXT("LS") }, { EKeys::Gamepad_RightThumbstick, TEXT("RS") },
		{ EKeys::Gamepad_DPad_Left, TEXT("D-PAD LEFT") }, { EKeys::Gamepad_DPad_Right, TEXT("D-PAD RIGHT") },
		{ EKeys::Gamepad_DPad_Up, TEXT("D-PAD UP") }, { EKeys::Gamepad_DPad_Down, TEXT("D-PAD DOWN") },
		{ EKeys::Gamepad_LeftStick_Left, TEXT("STICK LEFT") }, { EKeys::Gamepad_LeftStick_Right, TEXT("STICK RIGHT") },
		{ EKeys::Gamepad_LeftStick_Up, TEXT("STICK UP") }, { EKeys::Gamepad_LeftStick_Down, TEXT("STICK DOWN") },
	};
	for (const FLabel& Label : Labels)
	{
		if (Label.Key == Key)
		{
			return FText::FromString(Label.Text);
		}
	}
	// Letters, F-keys and anything else: the engine's short name in capitals ("A", "F5").
	return FText::FromString(Key.GetDisplayName(false).ToString().ToUpper());
}

// ============================================================================ LAN

FString UPinguSettings::GetPlayerName() const
{
	const FString Name = PinguSettingsPrivate::SanitizePlayerName(PlayerName);
	return Name.IsEmpty() ? PinguSettingsPrivate::GetDefaultPlayerName() : Name;
}

void UPinguSettings::SetPlayerName(const FString& Name)
{
	// Empty: the computer's user name again.
	const FString Clean = PinguSettingsPrivate::SanitizePlayerName(Name);
	if (Clean == PlayerName)
	{
		return;
	}
	PlayerName = Clean;
	Save();
}

// ============================================================================ editor

void UPinguSettings::HandleEndPlayMap()
{
#if WITH_EDITOR
	using namespace PinguSettingsPrivate;
	// Play In Editor shares the console variables with the editor: give the editor viewport its own settings back
	// (otherwise picking Low in a game leaves the editor without Lumen and with Low materials).
	FRuntimeState& S = State();
	const int32 Restored = S.CVarBackups.Num();
	if (Restored > 0 || S.bEditorLevelsSaved)
	{
		FGlobalComponentRecreateRenderStateContext Recreate;
		if (S.bEditorLevelsSaved)
		{
			Scalability::SetQualityLevels(S.EditorLevels);
			// ApplyGraphics copied the game's levels into the engine's game user settings: a later ApplySettings in
			// the editor must not bring them back.
			if (UGameUserSettings* UserSettings = GetGameUserSettings())
			{
				UserSettings->ScalabilityQuality = S.EditorLevels;
			}
		}
		for (const TPair<FString, FString>& Backup : S.CVarBackups)
		{
			if (IConsoleVariable* CVar = FindCVar(*Backup.Key))
			{
				RestoreCVar(*Backup.Key, CVar, Backup.Value);
			}
		}
		IConsoleManager::Get().CallAllConsoleVariableSinks();
	}
	if (S.bEditorMaxFPSSaved && GEngine)
	{
		GEngine->SetMaxFPS(S.EditorMaxFPS);
	}
	S.CVarBackups.Reset();
	S.StaticSkyLights.Reset();
	S.Suns.Reset();
	S.bEditorLevelsSaved = false;
	S.bEditorMaxFPSSaved = false;
	S.LastWorld.Reset();
	if (Restored > 0)
	{
		UE_LOG(LogPingu, Display, TEXT("[SETTINGS] Play In Editor ended: the editor's own render settings are back (%d console variables)"), Restored);
	}
#endif
}

#undef LOCTEXT_NAMESPACE
