// Pingu - the player's settings: graphics (presets + options), audio, key bindings, LAN name. Saved per user.
//
// CONTRACT (v2): the UI (settings + controls screens), the input config / player controller (key bindings) and the
// effects (density) use this API.

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericWindow.h"
#include "InputCoreTypes.h"
#include "UObject/Object.h"
#include "PinguSettings.generated.h"

/** Graphics presets. Low is the optimised one for ordinary laptops (integrated graphics). */
UENUM(BlueprintType)
enum class EPinguGraphicsPreset : uint8
{
	Low,
	Medium,
	High,
	Ultra,
	Custom,
};

UENUM(BlueprintType)
enum class EPinguQualityLevel : uint8
{
	Off,
	Low,
	Medium,
	High,
	Ultra,
};

UENUM(BlueprintType)
enum class EPinguAntiAliasing : uint8
{
	Off,
	FXAA,
	TAA,
	TSR,
};

/** Every graphics option of the settings screen. A preset is a full set of these. */
USTRUCT(BlueprintType)
struct FPinguGraphicsOptions
{
	GENERATED_BODY()

	UPROPERTY()
	EPinguGraphicsPreset Preset = EPinguGraphicsPreset::High;

	/** 3D resolution in percent of the window (50..100); the UI always renders sharp at full resolution. */
	UPROPERTY()
	int32 ResolutionScale = 100;

	UPROPERTY()
	EPinguAntiAliasing AntiAliasing = EPinguAntiAliasing::TSR;

	UPROPERTY()
	EPinguQualityLevel Shadows = EPinguQualityLevel::High;

	/** Global illumination: Low/Medium plain sky light, High/Ultra Lumen. */
	UPROPERTY()
	EPinguQualityLevel Lighting = EPinguQualityLevel::High;

	UPROPERTY()
	EPinguQualityLevel Reflections = EPinguQualityLevel::High;

	/** Particle density, glows and trails. */
	UPROPERTY()
	EPinguQualityLevel Effects = EPinguQualityLevel::High;

	/** Bloom, ambient occlusion, exposure, ... */
	UPROPERTY()
	EPinguQualityLevel PostProcess = EPinguQualityLevel::High;

	/** Mesh LODs and material detail (High/Ultra: the detailed penguin and block materials). */
	UPROPERTY()
	EPinguQualityLevel ModelDetail = EPinguQualityLevel::High;

	UPROPERTY()
	EPinguQualityLevel Textures = EPinguQualityLevel::High;

	/** UNUSED. The old OUTLINE option: the ink outline is gone from the game, so nothing reads or sets this. The field stays
	 *  only so a saved settings file that still holds it loads without complaint. */
	UPROPERTY()
	bool bPenguinOutline = false;

	/** Frames per second shown in a corner. */
	UPROPERTY()
	bool bShowFPS = false;

	/** Everything except Preset and bShowFPS (not a quality option: toggling it keeps the preset). */
	bool SameOptionsAs(const FPinguGraphicsOptions& Other) const;
};

/** Things the player does, each with up to two keyboard / mouse keys. */
UENUM(BlueprintType)
enum class EPinguInputAction : uint8
{
	MoveLeft,
	MoveRight,
	Up,          // aims an 8-way dash up (does NOT jump)
	Down,        // fast fall in the air
	Jump,
	Dash,
	Ability1,    // left icon
	Ability2,    // top icon (picked-up abilities go here)
	Ability3,    // right icon
	Pause,
	BellySlide,  // tap: slide on the belly (a belly-flop dive in the air). Last, so saved bindings keep their order.
	Count UMETA(Hidden)
};

USTRUCT(BlueprintType)
struct FPinguKeyBinding
{
	GENERATED_BODY()

	UPROPERTY()
	EPinguInputAction Action = EPinguInputAction::Jump;

	UPROPERTY()
	FKey Primary;

	UPROPERTY()
	FKey Secondary;
};

/**
 * Graphics presets (the full tables - every scalability group and console variable - are in PinguSettings.cpp):
 *
 *            3D res  AA    shadows                 lighting / reflections               materials   frame cap
 *   Low      67%     TAA   1 cascade, 1024         sky light captured once / none       Low (0)     60
 *   Medium   85%     TAA   1 cascade, 2048         sky light captured once + DFAO / SSR Medium (2)  -
 *   High     100%    TSR   virtual shadow maps     Lumen, real-time sky (today's look)  High (1)    -
 *   Ultra    100%    TSR   sharper VSM             Lumen + hardware ray tracing         Epic (3)    -
 *
 * The UI (Slate) always renders at the full window resolution; only the 3D view is scaled.
 */
UCLASS(Config = GameUserSettings, ConfigDoNotCheckDefaults)
class PENGUIN_API UPinguSettings : public UObject
{
	GENERATED_BODY()

public:
	/** The one settings object (loaded from the user's config on first use). */
	static UPinguSettings* Get();

	/** Writes everything to the user's config. The setters below save by themselves.
	 *  Never writes anything on automated test runs (IsAutomatedRun): tests must not change the player's settings. */
	void Save();

	/** -PinguAutoTest, -PinguMatchTest, -PinguInputTest, -PinguPerfTest, -PinguMenuShot(s), -PinguNetTest(=...), -unattended. */
	static bool IsAutomatedRun();

	// ------------------------------------------------------------------ graphics
	const FPinguGraphicsOptions& GetGraphics() const { return Graphics; }
	/** Takes the options (Preset becomes Custom unless they equal a preset), applies and saves them. */
	void SetGraphics(const FPinguGraphicsOptions& NewOptions);
	/** Applies and saves a preset. */
	void ApplyPreset(EPinguGraphicsPreset Preset);
	static FPinguGraphicsOptions GetPresetOptions(EPinguGraphicsPreset Preset);
	/** Pushes the current graphics options to the engine (console variables, scalability, the world's lights). */
	void ApplyGraphics(UWorld* World = nullptr) const;
	/** First start on this machine (no graphics settings saved yet): the engine's hardware benchmark (recorded; the fallback),
	 *  then the preset High goes on screen and UpdateFirstStartPick measures its real frames (the title's bot demo, this PC's
	 *  own window size; up to 25 s, as a laptop's graphics chip only goes to full power after about 15 s) and picks: High's
	 *  GPU time per frame up to 8 ms Ultra (with hardware ray tracing), 13 ms High, 35 ms Medium, else Low. Saves, with what
	 *  it measured in AutoDetectedWith. Never on automated runs (except
	 *  -PinguFirstStartTest, which saves nothing); settings already saved are never replaced (editor or packaged game); a
	 *  graphics choice of the player's while it measures wins. */
	void AutoDetectIfNeeded(UWorld* World = nullptr);
	/** Every frame, from the first local player controller: the first start's measuring (see AutoDetectIfNeeded). */
	void UpdateFirstStartPick(UWorld* World);
	/** Runs the engine's hardware benchmark (WorkScale 10: about a second on a desktop, 11 s on a laptop) and returns the
	 *  preset its numbers alone suggest: weak GPU (and every Intel integrated GPU) Low, mid-range Medium, strong High, very
	 *  strong with hardware ray tracing Ultra. It underrates laptops badly (their GPU stays at low clocks through it), which
	 *  is why the first start measures real frames. Changes nothing. */
	static EPinguGraphicsPreset BenchmarkRecommendedPreset(float& OutGPUIndex, float& OutCPUIndex, int32 WorkScale = 10);

	// display (stored by the engine's game user settings)
	EWindowMode::Type GetWindowMode() const;
	void SetWindowMode(EWindowMode::Type Mode);
	FIntPoint GetResolution() const;
	void SetResolution(FIntPoint Resolution);
	/** Resolutions the monitor supports, smallest first. */
	TArray<FIntPoint> GetSupportedResolutions() const;
	/** The native (desktop) resolution of the monitor the game window is on (the primary one before a window exists). */
	static FIntPoint GetNativeResolution();
	/** Fullscreen and Borderless always run at the monitor's native resolution (2026-11-07; Linus: "make the game resolution
	 *  always be max when fullscreened or borderless fullscreen"); Windowed keeps its size. */
	static bool IsNativeResolutionMode(EWindowMode::Type Mode);
	/** The resolution a window mode runs at: Native in the two fullscreen modes (when Native is valid), else Wanted. */
	static FIntPoint ResolveResolution(EWindowMode::Type Mode, FIntPoint Wanted, FIntPoint Native);
	/** Startup (every world start): a saved Fullscreen / Borderless config with a smaller resolution goes to the native one and
	 *  is saved. Not in the editor (the PIE viewport belongs to the editor) and not on automated runs. */
	void EnforceNativeResolution();
	bool GetVSync() const;
	void SetVSync(bool bEnabled);
	/** 0 = unlimited. */
	int32 GetFrameRateLimit() const;
	void SetFrameRateLimit(int32 FramesPerSecond);

	/** 0.3 .. 1: how many particles the effects spawn (from the Effects quality: Off / Low 0.3, Medium 0.6, High / Ultra
	 *  1 = today). A cached value: cheap enough to call per projectile per frame and per effect. */
	static float GetFXDensity();
	/** Effects quality Ultra: the effects add their Niagara layers (light flashes, sparks, glints, powder, a heavier
	 *  snowfall). A cached value like GetFXDensity. */
	static bool UseUltraEffects();

	static FText GetPresetName(EPinguGraphicsPreset Preset);
	static FText GetQualityName(EPinguQualityLevel Level);
	static FText GetAntiAliasingName(EPinguAntiAliasing Method);
	/** One line for the settings screen ("For ordinary laptops ..."). */
	static FText GetPresetDescription(EPinguGraphicsPreset Preset);
	/** The frame cap a preset brings (0 = none). Low caps at 60 to keep laptops cool and steady; applying another preset
	 *  removes that cap again but keeps a cap the player chose (SetFrameRateLimit). */
	static int32 GetPresetFrameRateLimit(EPinguGraphicsPreset Preset);

	/** Low adapts to the PC: when the graphics chip cannot hold the frame rate (GPU time per frame over 90 % of the
	 *  frame cap's budget, 60 fps without a cap), the 3D resolution goes down step by step - 67 %, 58 %, 50 %, then a
	 *  faster upscale (FXAA, the post effects at the 3D resolution, no bloom), then no shadows - and back up when there is
	 *  room again. Only the preset Low (a Custom mix stays as it is); nothing of it is saved. 0 = Low as it is. */
	static constexpr int32 NumAutoSteps = 5;
	int32 GetAutoStep() const;
	/** Low's adaptive steps are on: the preset is Low (a Custom mix is never adapted; the SHOW FPS option does
	 *  not change that). */
	bool IsAutoResolutionActive() const { return Graphics.Preset == EPinguGraphicsPreset::Low; }
	/** The options as rendered right now: GetGraphics(), with Low's adaptive step applied (the menu shows these as AUTO). */
	FPinguGraphicsOptions GetEffectiveGraphics() const;
	/** Every frame, from the first local player controller of a game world (real seconds): watches the GPU time and
	 *  changes Low's step when needed. Automated runs leave it alone (their measurements stay comparable) unless
	 *  -PinguAutoRes; -PinguAutoResStep=N forces a step (tests). */
	void UpdateAutoResolution(UWorld* World, float RealDeltaSeconds);
	/** Tests (-PinguAutoResTest): pretend the graphics chip is slower - this much GPU time is added to every frame. */
	static void SetAutoResTestExtraGpuMs(double Milliseconds);

	// ------------------------------------------------------------------ the settings page's edit session (2026-11-07)
	// Linus: "make an aply button for the settings and when u dindnt click and leave the settings and u have changed sth it asks
	// u if u want to apply or not". While the settings page is open a session runs: the page's changes preview live (graphics,
	// audio, camera distance, vsync, frame cap, key bindings) but are not saved until APPLY, and the window mode / resolution
	// are only a draft (the real window changes on APPLY). DISCARD puts every value back, exactly. Without a session (the old
	// Slate menus, tests that call the setters directly) every setter applies and saves at once, as before.
	/** The settings page opened: remembers everything the pages can change. Nothing happens while a session runs. */
	void BeginEditSession();
	bool IsEditSession() const { return bEditing; }
	/** Something differs from when the session began (or from the last APPLY). */
	bool HasPendingChanges() const;
	/** APPLY: the display draft goes to the window, everything is saved; the session goes on with this as its new start. */
	void ApplyEditSession();
	/** DISCARD: every value back as it was (previews undone, the display draft dropped); the session goes on. */
	void DiscardEditSession(UWorld* World);
	/** The page closed: a session with changes left is discarded, then it ends. */
	void EndEditSession(UWorld* World);

	// ------------------------------------------------------------------ audio (0..1)
	float GetMasterVolume() const { return MasterVolume; }
	float GetMusicVolume() const { return MusicVolume; }
	float GetSfxVolume() const { return SfxVolume; }
	void SetVolumes(float InMaster, float InMusic, float InSfx);
	/** Pushes the volumes to the sound effects / music player of that world. */
	void ApplyAudio(const UObject* WorldContext) const;

	// ------------------------------------------------------------------ the match camera (2026-10-29)
	/** How far the match camera sits, against the game's own framing (MinCameraDistance..MaxCameraDistance, 1 = the default):
	 *  this machine only (the camera is local), saved, live. Linus: "a mac camera distance silder in the settings". */
	static constexpr float MinCameraDistance = 0.6f;
	static constexpr float MaxCameraDistance = 1.4f;
	float GetCameraDistance() const { return CameraDistance; }
	void SetCameraDistance(float InDistance);

	// ------------------------------------------------------------------ hot-join (2026-11-07)
	/** Host setting, default ON, saved: may people join while a match runs (they play from its next round)? OFF: the host
	 *  turns them away with a message until the match ends (the lobby, between matches, always works). Read on the machine
	 *  that hosts. The test switch -PinguNoHotJoin forces OFF for a run without saving. Linus: "disable hotjoining in
	 *  settings but deafult is on". */
	bool GetJoinRunningMatches() const;
	void SetJoinRunningMatches(bool bInJoin);

	// ------------------------------------------------------------------ abilities RANDOM / CHOOSE (2026-11-07, PinguAbilityPicks.h)
	/** The host's lobby setting, default RANDOM (false), saved; the lobby reads it when it opens. */
	bool GetChooseAbilities() const { return bChooseAbilities; }
	void SetChooseAbilities(bool bInChoose);

	// ------------------------------------------------------------------ controls
	/** The keyboard / mouse keys of an action (primary first, empties left out, then GetFallbackKey if there is one). */
	TArray<FKey> GetKeys(EPinguInputAction Action) const;
	/** Index 0 = primary, 1 = secondary. EKeys::Invalid when unbound. */
	FKey GetKey(EPinguInputAction Action, int32 Index) const;
	/** E stands in for the middle mouse button (mice / touchpads without one): it works for the top ability while that
	 *  ability is on the middle mouse button and E is not bound to anything. EKeys::Invalid otherwise / for other actions. */
	FKey GetFallbackKey(EPinguInputAction Action) const;
	/** Binds a key; if another action had that key it loses it. Saves and broadcasts OnBindingsChanged. */
	void SetKey(EPinguInputAction Action, int32 Index, const FKey& Key);
	/** Unbinds a slot. Saves and broadcasts OnBindingsChanged. */
	void ClearKey(EPinguInputAction Action, int32 Index);
	/** Back to the default keys. Saves and broadcasts OnBindingsChanged. */
	void ResetKeyBindings();
	/** The gamepad layout (not rebindable): A jump, RB / RT dash, LB / LT belly slide, X / Y / B the left / top / right
	 *  ability, Start pause, D-pad + left stick to move and aim. For the four move actions the left-stick directions are
	 *  listed for labels and help screens only (the player controller maps the stick as an analog axis). */
	static TArray<FKey> GetGamepadKeys(EPinguInputAction Action);
	/** Keyboard keys and mouse buttons can be bound; Escape (always pause / back), gamepad and analog keys cannot. */
	static bool CanBind(const FKey& Key);
	static FText GetActionName(EPinguInputAction Action);
	/** Short label for a key cap ("SPACE", "LMB", "SHIFT", "A"). */
	static FText GetKeyLabel(const FKey& Key);
	/** Fired after every accepted SetKey, ClearKey and ResetKeyBindings (also when the key was already there), so the
	 *  player controllers rebuild their input mapping. */
	FSimpleMulticastDelegate OnBindingsChanged;

	// ------------------------------------------------------------------ LAN
	/** The name other players see. Defaults to the computer's user name. */
	FString GetPlayerName() const;
	void SetPlayerName(const FString& Name);

protected:
	UPROPERTY(Config)
	FPinguGraphicsOptions Graphics;

	UPROPERTY(Config)
	TArray<FPinguKeyBinding> KeyBindings;

	/** Bumped when the default bindings change incompatibly (old saved bindings are reset). */
	UPROPERTY(Config)
	int32 BindingsVersion = 0;

	UPROPERTY(Config)
	bool bAutoDetected = false;

	/** What the first-start pick measured and chose ("date: GPU ... index N, CPU index N, ray tracing yes -> High"). Kept
	 *  in the config because a Shipping build writes no log: a friend can read it in GameUserSettings.ini. */
	UPROPERTY(Config)
	FString AutoDetectedWith;

	/** The frame cap was set by a preset (Low's 60), not by the player: the next preset without a cap removes it. */
	UPROPERTY(Config)
	bool bPresetFrameCap = false;

	UPROPERTY(Config)
	float MasterVolume = 1.f;

	UPROPERTY(Config)
	float MusicVolume = 0.8f;

	UPROPERTY(Config)
	float SfxVolume = 1.f;

	UPROPERTY(Config)
	float CameraDistance = 1.f;

	UPROPERTY(Config)
	bool bJoinRunningMatches = true;

	UPROPERTY(Config)
	bool bChooseAbilities = false;

	UPROPERTY(Config)
	FString PlayerName;

private:
	/** What the settings pages can change (the edit session's start). */
	struct FEditState
	{
		FPinguGraphicsOptions Graphics;
		bool bPresetFrameCap = false;
		float Master = 1.f;
		float Music = 0.8f;
		float Sfx = 1.f;
		float CameraDistance = 1.f;
		bool bJoin = true;
		TArray<FPinguKeyBinding> Keys;
		EWindowMode::Type Mode = EWindowMode::Windowed;
		FIntPoint Resolution = FIntPoint(1920, 1080);
		bool bVSync = false;
		int32 FrameCap = 0;
	};
	FEditState CaptureEditState() const;
	bool bEditing = false;
	FEditState EditStart;
	/** The window mode and resolution as the page shows them while a session runs (the window itself is untouched until APPLY). */
	EWindowMode::Type DraftMode = EWindowMode::Windowed;
	FIntPoint DraftResolution = FIntPoint(1920, 1080);

	/** After loading: clamps the options, refreshes a named preset from its current table, repairs the bindings. */
	void Validate();
	void ResetBindingsToDefaults();
	FPinguKeyBinding* FindBinding(EPinguInputAction Action);
	const FPinguKeyBinding* FindBinding(EPinguInputAction Action) const;
	bool IsKeyBound(const FKey& Key) const;
	/** Pushes the frame cap of the game user settings to the engine (only when it differs). */
	void ApplyFrameRateLimit() const;
	/** The world's sky lights (real-time capture or captured once) and sun (cascaded shadow distance). */
	void ApplyWorldLighting(UWorld* World) const;
	/** Applies resolution / window mode (never on automated runs or in the editor; the command line wins). */
	void ApplyResolution() const;
	/** Editor: Play In Editor ended - the editor gets its own render settings back. */
	void HandleEndPlayMap();
	/** The first start's pick decides (HighGpuMs <= 0: nothing measured, the engine benchmark's pick), applies and saves. */
	void FinishFirstStartPick(UWorld* World, double HighGpuMs, int32 Frames, const TCHAR* Problem);
	/** SetGraphics / ApplyPreset by the player while the first start still measures: the measuring stops, the choice wins. */
	void StopFirstStartPickForPlayer();
};
