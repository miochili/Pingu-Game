// Pingu - the 3D menu's settings page (presets + every graphics option, display, audio: three tabs) and controls page (two
// rebindable keys per action, the gamepad layout) - today's Slate screens (PinguUISettings.cpp) as 3D: the plane, the tabs and
// presets as blue / dark segment slabs, each option a dark row slab with its value and two little arrow knobs, the volumes as
// rows with a groove and a snow knob, the key caps as dark slabs that turn orange while a key is being bound. Every change
// previews at once through UPinguSettings (the window mode and resolution wait for APPLY) and is saved by the APPLY plank
// (2026-11-07: the page runs a UPinguSettings edit session; BACK with changes left opens the "Apply changes?" board).

#include "PinguMenu3D.h"
#include "Penguin.h"
#include "PenguinHUD.h"
#include "PinguSettings.h"
#include "PinguUI.h"
#include "Engine/World.h"
#include "Text3DComponent.h"

#define LOCTEXT_NAMESPACE "PinguMenu3D"

namespace PinguMenu3DSettingsPrivate
{
	/** The frame rate caps to step through (0 = unlimited). */
	static const int32 FrameCaps[] = { 30, 60, 90, 120, 144, 165, 240, 0 };
	static constexpr int32 NumFrameCaps = static_cast<int32>(UE_ARRAY_COUNT(FrameCaps));

	static UPinguSettings* Prefs()
	{
		return UPinguSettings::Get();
	}

	static FText OnOff(bool bOn)
	{
		return bOn ? LOCTEXT("SettingOn3D", "ON") : LOCTEXT("SettingOff3D", "OFF");
	}

	static FText QualityName(EPinguQualityLevel Level)
	{
		return FText::FromString(UPinguSettings::GetQualityName(Level).ToString().ToUpper());
	}

	/** Steps an enum value inside [Min, Max]; bWrap goes round at the ends (a click), else it stops there. */
	static int32 StepValue(int32 Value, int32 Delta, int32 Min, int32 Max, bool bWrap)
	{
		const int32 Count = Max - Min + 1;
		if (Count <= 0)
		{
			return Value;
		}
		if (bWrap)
		{
			return Min + ((FMath::Clamp(Value, Min, Max) - Min + Delta) % Count + Count) % Count;
		}
		return FMath::Clamp(Value + Delta, Min, Max);
	}

	/** Changes one graphics option and applies it (the preset becomes Custom unless the options equal one). */
	static void ChangeGraphics(UWorld* World, TFunctionRef<void(FPinguGraphicsOptions&)> Change)
	{
		UPinguSettings* Current = Prefs();
		if (!Current)
		{
			return;
		}
		FPinguGraphicsOptions Options = Current->GetGraphics();
		Change(Options);
		Current->SetGraphics(Options);
		Current->ApplyGraphics(World);
	}

	/** The settings card and its rows. */
	constexpr float CardWidth = 800.f;
	constexpr float CardInner = CardWidth - 80.f;
	constexpr float RowHeight = 40.f;
	constexpr float RowGap = 6.f;
	/** The controls card. */
	constexpr float ControlsWidth = 1120.f;
}

// ============================================================================ settings

void APinguMenu3D::BuildSettings()
{
	using namespace PinguMenu3DSettingsPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	TWeakObjectPtr<APinguMenu3D> WeakThis(this);
	auto WorldOf = [WeakHUD]() -> UWorld* { const APenguinHUD* H = WeakHUD.Get(); return H ? H->GetWorld() : nullptr; };
	auto OnTab = [WeakThis](int32 Tab) { return TFunction<bool()>([WeakThis, Tab] { return WeakThis.IsValid() && WeakThis->SettingsTab == Tab; }); };
	constexpr float Width = CardWidth;
	constexpr float Inner = CardInner;
	constexpr float Left = -0.5f * Inner;
	constexpr float Right = 0.5f * Inner;
	constexpr float Height = 832.f;
	BeginPage(Settings, EPinguMenu3DPage::Settings, FVector2D(Width, Height));
	Settings.DefaultFocus = TEXT("Preset0");
	auto Z = [](float FromTop) { return 0.5f * Height - FromTop; };
	float Y = 24.f;

	// ---- the title, CUSTOM on the right while the options match no preset
	AddLabel(Settings, nullptr, FVector2D(Left, Z(Y + 26.f)), 42.f, [] { return LOCTEXT("SettingsTitle3D", "SETTINGS"); }, Style.Snow, PinguMenu3D::ETextAlign::Left, 0.f, true, 10.f);
	{
		const int32 FirstDeco = Settings.Decos.Num();
		const int32 FirstLabel = Settings.Labels.Num();
		AddPill(Settings, nullptr, FVector2D(Right, Z(Y + 26.f)), 26.f, 11.f, LOCTEXT("SettingsCustomTag", "CUSTOM"), Style.Gold, Style.Ink, 4.f, PinguMenu3D::ETextAlign::Right);
		auto Custom = [] { const UPinguSettings* Current = Prefs(); return Current && Current->GetGraphics().Preset == EPinguGraphicsPreset::Custom; };
		for (int32 Index = FirstDeco; Index < Settings.Decos.Num(); ++Index)
		{
			Settings.Decos[Index].Visible = Custom;
		}
		for (int32 Index = FirstLabel; Index < Settings.Labels.Num(); ++Index)
		{
			Settings.Labels[Index].Visible = Custom;
		}
	}
	Y += 52.f + 16.f;

	// ---- the tabs: LB  GRAPHICS | DISPLAY | AUDIO  RB
	{
		const FText TabNames[] = { LOCTEXT("TabGraphics3D", "GRAPHICS"), LOCTEXT("TabDisplay3D", "DISPLAY"), LOCTEXT("TabAudio3D", "AUDIO") };
		AddLabel(Settings, nullptr, FVector2D(Left, Z(Y + 22.f)), 11.f, [] { return LOCTEXT("TabPrev3D", "LB"); }, Style.IceDim, PinguMenu3D::ETextAlign::Left);
		float X = Left + 24.f + 12.f;
		for (int32 Tab = 0; Tab < 3; ++Tab)
		{
			const FText Name = TabNames[Tab];
			FButton& TabButton = AddButton(Settings, FName(*FString::Printf(TEXT("Tab%d"), Tab)), EPinguMenu3DKind::Segment, FVector2D(X + 88.f, Z(Y + 22.f)), FVector2D(176.f, 44.f), 16.f,
				[Name] { return Name; }, [WeakThis, Tab] { if (WeakThis.IsValid()) { WeakThis->SelectSettingsTab(Tab, false); } });
			TabButton.Selected = [WeakThis, Tab] { return WeakThis.IsValid() && WeakThis->SettingsTab == Tab; };
			X += 176.f + 8.f;
		}
		AddLabel(Settings, nullptr, FVector2D(X - 8.f + 12.f, Z(Y + 22.f)), 11.f, [] { return LOCTEXT("TabNext3D", "RB"); }, Style.IceDim, PinguMenu3D::ETextAlign::Left);
	}
	Y += 44.f + 24.f;
	const float TabsTop = Y;

	// ---- graphics: the presets, then every option
	{
		float GY = TabsTop;
		// (9 higher since 2026-10-26: the preset signs' snow covered its lower half.)
		const int32 Section = AddLabel(Settings, nullptr, FVector2D(Left, Z(GY + 4.f)), 14.f, [] { return LOCTEXT("PresetSection3D", "PRESET"); }, Style.Ice, PinguMenu3D::ETextAlign::Left);
		Settings.Labels[Section].Visible = OnTab(0);
		GY += 26.f + 8.f;
		struct FPresetButton
		{
			EPinguGraphicsPreset Preset;
			FText Name;
			FText Caption;
		};
		const FPresetButton Presets[] = {
			{ EPinguGraphicsPreset::Low, LOCTEXT("PresetLow3D", "LOW"), LOCTEXT("PresetLowCaption3D", "LAPTOP") },
			{ EPinguGraphicsPreset::Medium, LOCTEXT("PresetMedium3D", "MEDIUM"), FText::GetEmpty() },
			{ EPinguGraphicsPreset::High, LOCTEXT("PresetHigh3D", "HIGH"), FText::GetEmpty() },
			{ EPinguGraphicsPreset::Ultra, LOCTEXT("PresetUltra3D", "ULTRA"), FText::GetEmpty() },
		};
		const float PresetWidth = (Inner - 3.f * 8.f) / 4.f;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const EPinguGraphicsPreset Preset = Presets[Index].Preset;
			const FText Name = Presets[Index].Name;
			const float X = Left + Index * (PresetWidth + 8.f) + 0.5f * PresetWidth;
			const int32 ButtonIndex = Settings.Buttons.Num();
			FButton& Button = AddButton(Settings, FName(*FString::Printf(TEXT("Preset%d"), Index)), EPinguMenu3DKind::Segment, FVector2D(X, Z(GY + 28.f)), FVector2D(PresetWidth, 56.f), 18.f,
				[Name] { return Name; },
				[WorldOf, Preset]
				{
					if (UPinguSettings* Current = Prefs())
					{
						Current->ApplyPreset(Preset);
						Current->ApplyGraphics(WorldOf());
					}
				});
			Button.Visible = OnTab(0);
			Button.Selected = [Preset] { const UPinguSettings* Current = Prefs(); return Current && Current->GetGraphics().Preset == Preset; };
			if (!Presets[Index].Caption.IsEmpty())
			{
				// LAPTOP under LOW (on the button, so it pops with it).
				const FText Caption = Presets[Index].Caption;
				const int32 CaptionLabel = AddLabel(Settings, Settings.Buttons[ButtonIndex].Body, FVector2D(0.f, -19.f), 9.f, [Caption] { return Caption; }, Style.Ice,
					PinguMenu3D::ETextAlign::Center, 0.f, false, 1.f);
				Settings.Labels[CaptionLabel].Visible = OnTab(0);
			}
		}
		GY += 56.f + 16.f;
		auto AddGraphicsRow = [&](const TCHAR* Id, const FText& Label, TFunction<FText()> Value, TFunction<void(int32, bool)> OnChange)
		{
			AddOptionRow(Settings, FName(Id), FVector2D(0.f, Z(GY + 0.5f * RowHeight)), Inner, [Label] { return Label; }, MoveTemp(Value), MoveTemp(OnChange), 168.f, OnTab(0));
			GY += RowHeight + RowGap;
		};
		AddGraphicsRow(TEXT("ResScale"), LOCTEXT("ResScale3D", "RESOLUTION SCALE"),
			[]
			{
				const UPinguSettings* Current = Prefs();
				const int32 Option = Current ? Current->GetGraphics().ResolutionScale : 100;
				const int32 InUse = Current ? Current->GetEffectiveGraphics().ResolutionScale : Option;
				return InUse != Option ? FText::Format(LOCTEXT("ResScaleAuto3D", "AUTO {0}%"), FText::AsNumber(InUse)) : FText::Format(LOCTEXT("ResScaleValue3D", "{0}%"), FText::AsNumber(Option));
			},
			[WorldOf](int32 Delta, bool bWrap)
			{
				ChangeGraphics(WorldOf(), [Delta, bWrap](FPinguGraphicsOptions& Options)
				{
					const int32 StepIndex = StepValue((FMath::Clamp(Options.ResolutionScale, 50, 100) - 50 + 2) / 5, Delta, 0, 10, bWrap);
					Options.ResolutionScale = 50 + StepIndex * 5;
				});
			});
		AddGraphicsRow(TEXT("AntiAliasing"), LOCTEXT("AntiAliasing3D", "ANTI-ALIASING"),
			[]
			{
				const UPinguSettings* Current = Prefs();
				const EPinguAntiAliasing Option = Current ? Current->GetGraphics().AntiAliasing : EPinguAntiAliasing::TSR;
				const EPinguAntiAliasing InUse = Current ? Current->GetEffectiveGraphics().AntiAliasing : Option;
				const FString Name = UPinguSettings::GetAntiAliasingName(InUse).ToString().ToUpper();
				return FText::FromString(InUse != Option ? TEXT("AUTO ") + Name : Name);
			},
			[WorldOf](int32 Delta, bool bWrap)
			{
				ChangeGraphics(WorldOf(), [Delta, bWrap](FPinguGraphicsOptions& Options)
				{
					Options.AntiAliasing = static_cast<EPinguAntiAliasing>(StepValue(static_cast<int32>(Options.AntiAliasing), Delta,
						static_cast<int32>(EPinguAntiAliasing::Off), static_cast<int32>(EPinguAntiAliasing::TSR), bWrap));
				});
			});
		struct FQualityRow
		{
			const TCHAR* Id;
			FText Label;
			EPinguQualityLevel FPinguGraphicsOptions::* Member;
			bool bAllowOff;
		};
		const FQualityRow QualityRows[] = {
			{ TEXT("Shadows"), LOCTEXT("Shadows3D", "SHADOWS"), &FPinguGraphicsOptions::Shadows, true },
			{ TEXT("Lighting"), LOCTEXT("Lighting3D", "LIGHTING"), &FPinguGraphicsOptions::Lighting, false },
			{ TEXT("Reflections"), LOCTEXT("Reflections3D", "REFLECTIONS"), &FPinguGraphicsOptions::Reflections, true },
			{ TEXT("Effects"), LOCTEXT("Effects3D", "EFFECTS"), &FPinguGraphicsOptions::Effects, false },
			{ TEXT("PostProcess"), LOCTEXT("PostProcess3D", "POST PROCESS"), &FPinguGraphicsOptions::PostProcess, true },
			{ TEXT("ModelDetail"), LOCTEXT("ModelDetail3D", "MODEL DETAIL"), &FPinguGraphicsOptions::ModelDetail, false },
			{ TEXT("Textures"), LOCTEXT("Textures3D", "TEXTURES"), &FPinguGraphicsOptions::Textures, false },
		};
		for (const FQualityRow& Quality : QualityRows)
		{
			EPinguQualityLevel FPinguGraphicsOptions::* Member = Quality.Member;
			const int32 Min = Quality.bAllowOff ? static_cast<int32>(EPinguQualityLevel::Off) : static_cast<int32>(EPinguQualityLevel::Low);
			AddGraphicsRow(Quality.Id, Quality.Label,
				[Member]
				{
					const UPinguSettings* Current = Prefs();
					const EPinguQualityLevel Option = Current ? Current->GetGraphics().*Member : EPinguQualityLevel::High;
					const EPinguQualityLevel InUse = Current ? Current->GetEffectiveGraphics().*Member : Option;
					return InUse != Option ? FText::Format(LOCTEXT("QualityAuto3D", "AUTO {0}"), QualityName(InUse)) : QualityName(Option);
				},
				[WorldOf, Member, Min](int32 Delta, bool bWrap)
				{
					ChangeGraphics(WorldOf(), [Member, Min, Delta, bWrap](FPinguGraphicsOptions& Options)
					{
						Options.*Member = static_cast<EPinguQualityLevel>(StepValue(static_cast<int32>(Options.*Member), Delta, Min, static_cast<int32>(EPinguQualityLevel::Ultra), bWrap));
					});
				});
		}
		AddGraphicsRow(TEXT("ShowFps"), LOCTEXT("ShowFps3D", "SHOW FPS"),
			[] { const UPinguSettings* Current = Prefs(); return OnOff(Current && Current->GetGraphics().bShowFPS); },
			[WorldOf](int32 Delta, bool bWrap) { ChangeGraphics(WorldOf(), [](FPinguGraphicsOptions& Options) { Options.bShowFPS = !Options.bShowFPS; }); });
		// (The plane's height was made for this, the tallest page: 832.)
		Y = FMath::Max(Y, GY - RowGap);
	}

	// ---- display
	{
		float DY = TabsTop;
		auto AddDisplayRow = [&](const TCHAR* Id, const FText& Label, TFunction<FText()> Value, TFunction<void(int32, bool)> OnChange)
		{
			AddOptionRow(Settings, FName(Id), FVector2D(0.f, Z(DY + 0.5f * RowHeight)), Inner, [Label] { return Label; }, MoveTemp(Value), MoveTemp(OnChange), 200.f, OnTab(1));
			DY += RowHeight + RowGap;
		};
		AddDisplayRow(TEXT("WindowMode"), LOCTEXT("WindowMode3D", "WINDOW MODE"),
			[]
			{
				const UPinguSettings* Current = Prefs();
				switch (Current ? Current->GetWindowMode() : EWindowMode::Windowed)
				{
				case EWindowMode::Fullscreen: return LOCTEXT("Fullscreen3D", "FULLSCREEN");
				case EWindowMode::WindowedFullscreen: return LOCTEXT("Borderless3D", "BORDERLESS");
				default: return LOCTEXT("Windowed3D", "WINDOWED");
				}
			},
			[](int32 Delta, bool bWrap)
			{
				if (UPinguSettings* Current = Prefs())
				{
					Current->SetWindowMode(static_cast<EWindowMode::Type>(StepValue(static_cast<int32>(Current->GetWindowMode()), Delta, 0, 2, bWrap)));
				}
			});
		AddDisplayRow(TEXT("Resolution"), LOCTEXT("Resolution3D", "RESOLUTION"),
			[]
			{
				const UPinguSettings* Current = Prefs();
				const FIntPoint Resolution = Current ? Current->GetResolution() : FIntPoint(1920, 1080);
				// Fullscreen and Borderless always run at the monitor's native resolution: the row only shows it (locked).
				const bool bNative = Current && UPinguSettings::IsNativeResolutionMode(Current->GetWindowMode());
				return FText::FromString(bNative ? FString::Printf(TEXT("NATIVE %d x %d"), Resolution.X, Resolution.Y) : FString::Printf(TEXT("%d x %d"), Resolution.X, Resolution.Y));
			},
			[](int32 Delta, bool bWrap)
			{
				UPinguSettings* Current = Prefs();
				if (!Current || UPinguSettings::IsNativeResolutionMode(Current->GetWindowMode()))
				{
					return;
				}
				const TArray<FIntPoint> Resolutions = Current->GetSupportedResolutions();
				if (Resolutions.Num() == 0)
				{
					return;
				}
				// The closest supported one to the current resolution, then a step.
				const FIntPoint Now = Current->GetResolution();
				int32 Closest = 0;
				int64 Best = TNumericLimits<int64>::Max();
				for (int32 Index = 0; Index < Resolutions.Num(); ++Index)
				{
					const int64 Distance = FMath::Abs(static_cast<int64>(Resolutions[Index].X) * Resolutions[Index].Y - static_cast<int64>(Now.X) * Now.Y);
					if (Distance < Best)
					{
						Best = Distance;
						Closest = Index;
					}
				}
				Current->SetResolution(Resolutions[StepValue(Closest, Delta, 0, Resolutions.Num() - 1, bWrap)]);
			});
		AddDisplayRow(TEXT("VSync"), LOCTEXT("VSync3D", "VSYNC"),
			[] { const UPinguSettings* Current = Prefs(); return OnOff(Current && Current->GetVSync()); },
			[](int32 Delta, bool bWrap) { if (UPinguSettings* Current = Prefs()) { Current->SetVSync(!Current->GetVSync()); } });
		AddDisplayRow(TEXT("FrameCap"), LOCTEXT("FrameCap3D", "FRAME RATE CAP"),
			[]
			{
				const UPinguSettings* Current = Prefs();
				const int32 Limit = Current ? Current->GetFrameRateLimit() : 0;
				return Limit > 0 ? FText::Format(LOCTEXT("FrameCapValue3D", "{0} FPS"), FText::AsNumber(Limit)) : LOCTEXT("Unlimited3D", "UNLIMITED");
			},
			[](int32 Delta, bool bWrap)
			{
				UPinguSettings* Current = Prefs();
				if (!Current)
				{
					return;
				}
				const int32 Limit = Current->GetFrameRateLimit();
				int32 Closest = NumFrameCaps - 1;
				if (Limit > 0)
				{
					int32 Best = MAX_int32;
					for (int32 Index = 0; Index < NumFrameCaps - 1; ++Index)
					{
						if (FMath::Abs(FrameCaps[Index] - Limit) < Best)
						{
							Best = FMath::Abs(FrameCaps[Index] - Limit);
							Closest = Index;
						}
					}
				}
				Current->SetFrameRateLimit(FrameCaps[StepValue(Closest, Delta, 0, NumFrameCaps - 1, bWrap)]);
			});
		// The host's hot-join switch (2026-11-07; Linus: "disable hotjoining in settings but deafult is on"): host-only, saved
		// with the other settings. Joiners in a LAN game don't see it (the host decides); solo and the menus do (it applies
		// to the next game they host).
		{
			FButton& Row = AddOptionRow(Settings, TEXT("JoinRunning"), FVector2D(0.f, Z(DY + 0.5f * RowHeight)), Inner, [] { return LOCTEXT("JoinRunning3D", "JOIN RUNNING MATCHES (HOST)"); },
				[] { const UPinguSettings* Current = Prefs(); return OnOff(!Current || Current->GetJoinRunningMatches()); },
				[](int32 Delta, bool bWrap) { if (UPinguSettings* Current = Prefs()) { Current->SetJoinRunningMatches(!Current->GetJoinRunningMatches()); } }, 200.f,
				[WeakHUD, OnTab] { const APenguinHUD* H = WeakHUD.Get(); return OnTab(1)() && !(H && H->IsNetworkGame() && !H->HasMatchAuthority()); });
			(void)Row;
			DY += RowHeight + RowGap;
		}
		// The match camera's distance (2026-10-29; Linus: "a mac camera distance silder in the settings so u can adjust how close
		// the camera is"): a slider over 60-140 % (the slider's 0..1 in steps of 5 % = 4 % of distance), 100 % in the middle.
		{
			constexpr float MinScale = UPinguSettings::MinCameraDistance;
			constexpr float Span = UPinguSettings::MaxCameraDistance - UPinguSettings::MinCameraDistance;
			FButton& Row = AddSlider(Settings, TEXT("CameraDistance"), FVector2D(0.f, Z(DY + 0.5f * RowHeight)), Inner,
				[] { return LOCTEXT("CameraDistance3D", "CAMERA DISTANCE"); },
				[] { const UPinguSettings* Current = Prefs(); return ((Current ? Current->GetCameraDistance() : 1.f) - MinScale) / Span; },
				[](float NewValue) { if (UPinguSettings* Current = Prefs()) { Current->SetCameraDistance(MinScale + Span * FMath::Clamp(NewValue, 0.f, 1.f)); } });
			Row.Value = [] { const UPinguSettings* Current = Prefs(); return FText::Format(LOCTEXT("CameraDistancePercent3D", "{0}%"), FText::AsNumber(FMath::RoundToInt(100.f * (Current ? Current->GetCameraDistance() : 1.f)))); };
			Row.Visible = OnTab(1);
			DY += RowHeight + RowGap;
		}
	}

	// ---- audio
	{
		float AY = TabsTop;
		auto SetVolume = [WorldOf](int32 Which, float NewValue)
		{
			UPinguSettings* Current = Prefs();
			if (!Current)
			{
				return;
			}
			float Master = Current->GetMasterVolume();
			float Music = Current->GetMusicVolume();
			float Sfx = Current->GetSfxVolume();
			(Which == 0 ? Master : (Which == 1 ? Music : Sfx)) = FMath::Clamp(NewValue, 0.f, 1.f);
			Current->SetVolumes(Master, Music, Sfx);
			Current->ApplyAudio(WorldOf());
		};
		auto AddAudioRow = [&](const TCHAR* Id, const FText& Label, TFunction<float()> Value, int32 Which)
		{
			FButton& Row = AddSlider(Settings, FName(Id), FVector2D(0.f, Z(AY + 0.5f * RowHeight)), Inner, [Label] { return Label; }, MoveTemp(Value),
				[SetVolume, Which](float NewValue) { SetVolume(Which, NewValue); });
			Row.Visible = OnTab(2);
			AY += RowHeight + RowGap;
		};
		AddAudioRow(TEXT("Master"), LOCTEXT("MasterVolume3D", "MASTER"), [] { const UPinguSettings* Current = Prefs(); return Current ? Current->GetMasterVolume() : 1.f; }, 0);
		AddAudioRow(TEXT("Music"), LOCTEXT("MusicVolume3D", "MUSIC"), [] { const UPinguSettings* Current = Prefs(); return Current ? Current->GetMusicVolume() : 0.8f; }, 1);
		AddAudioRow(TEXT("Effects"), LOCTEXT("EffectsVolume3D", "EFFECTS"), [] { const UPinguSettings* Current = Prefs(); return Current ? Current->GetSfxVolume() : 1.f; }, 2);
	}

	// ---- CONTROLS (the keys open from here) and BACK
	Y += 24.f;
	AddButton(Settings, TEXT("Controls"), EPinguMenu3DKind::Normal, FVector2D(Left + 100.f, Z(Y + 28.f)), FVector2D(200.f, 56.f), 18.f,
		[] { return LOCTEXT("SettingsControls3D", "CONTROLS"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_Controls(); } });
	AddButton(Settings, TEXT("Back"), EPinguMenu3DKind::Primary, FVector2D(Right - 100.f, Z(Y + 28.f)), FVector2D(200.f, 56.f), 18.f,
		[] { return LOCTEXT("SettingsBack3D", "BACK"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_Back(); } });
	// APPLY (2026-11-07): next to BACK, greyed while nothing is changed; saves what the page changed.
	{
		FButton& Apply = AddButton(Settings, TEXT("Apply"), EPinguMenu3DKind::Primary, FVector2D(Right - 100.f - 200.f - 12.f, Z(Y + 28.f)), FVector2D(200.f, 56.f), 18.f,
			[] { return LOCTEXT("SettingsApply3D", "APPLY"); }, [WeakHUD, WeakThis]
			{
				if (APenguinHUD* H = WeakHUD.Get())
				{
					H->Action_ApplySettings();
					// APPLY greys out now: the focus goes to BACK (the next thing a player does).
					if (WeakThis.IsValid())
					{
						WeakThis->FocusButton(TEXT("Back"));
					}
				}
			});
		Apply.Enabled = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->HasSettingsPending(); };
	}
	EndStatic(Settings);
}

void APinguMenu3D::UpdateSettings(FPageParts& Parts)
{
	(void)Parts;
}

void APinguMenu3D::SelectSettingsTab(int32 Tab, bool bFocusFirst)
{
	if (HUD.IsValid() && HUD->HasSettingsConfirm())
	{
		return; // the "Apply changes?" board has the keys (LB / RB must not flip the page behind it)
	}
	SettingsTab = (Tab % 3 + 3) % 3;
	if (!Settings.bBuilt)
	{
		return;
	}
	// The rows of the new page show at once (the focus can go to one of them now), the old page's hide.
	for (FButton& Button : Settings.Buttons)
	{
		if (Button.Visible && Settings.bShown)
		{
			const bool bWant = Button.Visible();
			if (bWant != Button.bShown)
			{
				SetButtonVisible(Button, bWant);
			}
		}
	}
	if (Page != EPinguMenu3DPage::Settings)
	{
		return;
	}
	if (bFocusFirst)
	{
		FocusButton(SettingsTab == 1 ? FName(TEXT("WindowMode")) : (SettingsTab == 2 ? FName(TEXT("Master")) : FName(TEXT("Preset0"))));
	}
	else
	{
		FocusButton(FName(*FString::Printf(TEXT("Tab%d"), SettingsTab)));
	}
}

// ============================================================================ the "Apply changes?" board

void APinguMenu3D::BuildConfirm()
{
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	// APPLY CHANGES?, two planks side by side (an overlay on the settings page: Esc / B closes it, back into the settings).
	constexpr float Width = 576.f;
	constexpr float Height = 200.f;
	BeginPage(Confirm, EPinguMenu3DPage::Confirm, FVector2D(Width, Height));
	Confirm.DefaultFocus = TEXT("ConfirmApply");
	AddLabel(Confirm, nullptr, FVector2D(0.f, 0.5f * Height - 24.f - 24.f), 36.f, [] { return LOCTEXT("ConfirmTitle3D", "APPLY CHANGES?"); }, Style.Snow,
		PinguMenu3D::ETextAlign::Center, Width - 80.f, true, 10.f);
	constexpr float ButtonWidth = 232.f;
	AddButton(Confirm, TEXT("ConfirmApply"), EPinguMenu3DKind::Primary, FVector2D(-0.5f * ButtonWidth - 8.f, -0.5f * Height + 32.f + 28.f), FVector2D(ButtonWidth, 56.f), 20.f,
		[] { return LOCTEXT("ConfirmApply3D", "APPLY"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_ConfirmApply(); } });
	AddButton(Confirm, TEXT("ConfirmDiscard"), EPinguMenu3DKind::Normal, FVector2D(0.5f * ButtonWidth + 8.f, -0.5f * Height + 32.f + 28.f), FVector2D(ButtonWidth, 56.f), 20.f,
		[] { return LOCTEXT("ConfirmDiscard3D", "DISCARD"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_ConfirmDiscard(); } });
	EndStatic(Confirm);
}

// ============================================================================ controls

void APinguMenu3D::BuildControls()
{
	using namespace PinguMenu3DSettingsPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	constexpr float Width = ControlsWidth;
	constexpr float Inner = Width - 80.f;
	constexpr float Left = -0.5f * Inner;
	constexpr float Right = 0.5f * Inner;
	constexpr float Height = 730.f;
	BeginPage(Controls, EPinguMenu3DPage::Controls, FVector2D(Width, Height));
	auto Z = [](float FromTop) { return 0.5f * Height - FromTop; };
	float Y = 24.f;
	AddLabel(Controls, nullptr, FVector2D(Left, Z(Y + 26.f)), 42.f, [] { return LOCTEXT("ControlsTitle3D", "CONTROLS"); }, Style.Snow, PinguMenu3D::ETextAlign::Left, 0.f, true, 10.f);
	Y += 52.f + 16.f;
	// Column headers.
	const float KeyX = Left + 296.f;
	const float OtherX = KeyX + 208.f;
	const float PadX = OtherX + 232.f;
	AddLabel(Controls, nullptr, FVector2D(Left, Z(Y + 13.f)), 14.f, [] { return LOCTEXT("ColAction3D", "ACTION"); }, Style.Ice, PinguMenu3D::ETextAlign::Left);
	AddLabel(Controls, nullptr, FVector2D(KeyX, Z(Y + 13.f)), 14.f, [] { return LOCTEXT("ColKey3D", "KEY"); }, Style.Ice, PinguMenu3D::ETextAlign::Left);
	AddLabel(Controls, nullptr, FVector2D(OtherX, Z(Y + 13.f)), 14.f, [] { return LOCTEXT("ColKey2_3D", "OTHER KEY"); }, Style.Ice, PinguMenu3D::ETextAlign::Left);
	AddLabel(Controls, nullptr, FVector2D(PadX, Z(Y + 13.f)), 14.f, [] { return LOCTEXT("ColGamepad3D", "GAMEPAD"); }, Style.Ice, PinguMenu3D::ETextAlign::Left);
	Y += 26.f + 8.f;
	// Every action once, in the order of the enum - except that the belly slide (added last, see PinguSettings.h) sits right
	// under the dash, with the other moves.
	TArray<EPinguInputAction> ActionOrder;
	for (int32 ActionIndex = 0; ActionIndex < static_cast<int32>(EPinguInputAction::Count); ++ActionIndex)
	{
		const EPinguInputAction Action = static_cast<EPinguInputAction>(ActionIndex);
		if (Action != EPinguInputAction::BellySlide)
		{
			ActionOrder.Add(Action);
		}
		if (Action == EPinguInputAction::Dash)
		{
			ActionOrder.Add(EPinguInputAction::BellySlide);
		}
	}
	for (int32 RowIndex = 0; RowIndex < ActionOrder.Num(); ++RowIndex)
	{
		const EPinguInputAction Action = ActionOrder[RowIndex];
		const float RowZ = Z(Y + 20.f);
		const FString ActionName = UPinguSettings::GetActionName(Action).ToString().ToUpper();
		AddLabel(Controls, nullptr, FVector2D(Left, RowZ), 14.f, [ActionName] { return FText::FromString(ActionName); }, Style.Snow, PinguMenu3D::ETextAlign::Left, 280.f);
		for (int32 KeyIndex = 0; KeyIndex < 2; ++KeyIndex)
		{
			FButton& Cap = AddButton(Controls, FName(*FString::Printf(TEXT("Key%d_%d"), static_cast<int32>(Action), KeyIndex)), EPinguMenu3DKind::KeyCap,
				FVector2D((KeyIndex == 0 ? KeyX : OtherX) + 100.f, RowZ), FVector2D(200.f, 40.f), 14.f,
				[WeakHUD, Action, KeyIndex]
				{
					const APenguinHUD* H = WeakHUD.Get();
					if (H && H->IsCapturingKey(Action, KeyIndex))
					{
						return LOCTEXT("CapPress3D", "PRESS A KEY");
					}
					const UPinguSettings* Current = UPinguSettings::Get();
					const FKey Key = Current ? Current->GetKey(Action, KeyIndex) : FKey();
					return Key.IsValid() ? FText::FromString(UPinguSettings::GetKeyLabel(Key).ToString().ToUpper()) : LOCTEXT("CapEmpty3D", "-");
				},
				[WeakHUD, Action, KeyIndex] { if (APenguinHUD* H = WeakHUD.Get()) { H->BeginKeyCapture(Action, KeyIndex); } });
			Cap.Selected = [WeakHUD, Action, KeyIndex] { const APenguinHUD* H = WeakHUD.Get(); return H && H->IsCapturingKey(Action, KeyIndex); };
			if (RowIndex == 0 && KeyIndex == 0)
			{
				Controls.DefaultFocus = Cap.Id;
			}
		}
		// Read only: the gamepad layout is fixed.
		AddLabel(Controls, nullptr, FVector2D(PadX, RowZ), 13.f,
			[Action]
			{
				TArray<FString> Labels;
				for (const FKey& Key : UPinguSettings::GetGamepadKeys(Action))
				{
					Labels.Add(UPinguSettings::GetKeyLabel(Key).ToString().ToUpper());
				}
				return Labels.Num() > 0 ? FText::FromString(FString::Join(Labels, TEXT(" / "))) : LOCTEXT("NoPad3D", "-");
			}, Style.IceDim, PinguMenu3D::ETextAlign::Left, Right - PadX);
		Y += 40.f + (RowIndex + 1 < ActionOrder.Num() ? 6.f : 0.f);
	}
	Y += 16.f;
	AddButton(Controls, TEXT("Reset"), EPinguMenu3DKind::Ghost, FVector2D(Left + 148.f, Z(Y + 28.f)), FVector2D(296.f, 56.f), 16.f,
		[] { return LOCTEXT("ResetKeys3D", "RESET TO DEFAULTS"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_ResetKeys(); } });
	AddButton(Controls, TEXT("Back"), EPinguMenu3DKind::Primary, FVector2D(Right - 100.f, Z(Y + 28.f)), FVector2D(200.f, 56.f), 18.f,
		[] { return LOCTEXT("ControlsBack3D", "BACK"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_Back(); } });
	EndStatic(Controls);
}

#undef LOCTEXT_NAMESPACE
