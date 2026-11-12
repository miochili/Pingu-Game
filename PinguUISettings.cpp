// Pingu - the settings screen (graphics presets + every graphics option, display, audio) and the controls screen
// (two rebindable keys per action, the gamepad layout). Every change applies at once through UPinguSettings.
// Also the option / slider rows and key caps the other screens use.

#include "PinguUI.h"
#include "PenguinHUD.h"
#include "PinguMenu3D.h"
#include "PinguSettings.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PinguUI"

namespace PinguUISettingsPrivate
{
	/** The frame rate caps to step through (0 = unlimited). */
	static const int32 FrameCaps[] = { 30, 60, 90, 120, 144, 165, 240, 0 };
	static constexpr int32 NumFrameCaps = static_cast<int32>(UE_ARRAY_COUNT(FrameCaps));

	static UPinguSettings* Settings()
	{
		return UPinguSettings::Get();
	}

	static FText OnOff(bool bOn)
	{
		return bOn ? LOCTEXT("SettingOn", "ON") : LOCTEXT("SettingOff", "OFF");
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
		UPinguSettings* Current = Settings();
		if (!Current)
		{
			return;
		}
		FPinguGraphicsOptions Options = Current->GetGraphics();
		Change(Options);
		Current->SetGraphics(Options);
		// The world's lights too (SetGraphics applies the engine side; this is cheap and idempotent).
		Current->ApplyGraphics(World);
	}

	/** A quality option row: Low..Ultra, or Off..Ultra where off is allowed. */
	struct FQualityRow
	{
		FText Label;
		FText Hint;
		EPinguQualityLevel FPinguGraphicsOptions::* Member;
		bool bAllowOff;
	};
}

// ============================================================================ rows

TSharedRef<SPinguButton> SPinguRoot::MakeOptionRow(const FText& Label, TAttribute<FText> Value, TFunction<void(int32, bool)> OnChange,
	const FText& InHint, TAttribute<bool> Enabled, float Height, float ValueWidth)
{
	const FPinguStyle& Style = FPinguStyle::Get();
	// The arrows step the row they sit in (they are made before it, so they reach it through this holder).
	TSharedRef<TWeakPtr<SPinguButton>> RowHolder = MakeShared<TWeakPtr<SPinguButton>>();
	const FText RowHint = InHint;
	TSharedRef<SPinguButton> Row = SNew(SPinguButton)
		.Kind(EPinguButtonKind::Row)
		.Height(Height)
		.HAlign(HAlign_Fill)
		.ContentPadding(FMargin(16.f, 0.f, 4.f, 0.f))
		.HUD(HUD)
		.IsEnabled(Enabled)
		.OnStep_Lambda([OnChange](int32 Direction) { OnChange(Direction, false); })
		.OnClicked_Lambda([OnChange]() { OnChange(1, true); })
		.OnFocused_Lambda([this, RowHint]() { SetHint(RowHint); })
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Label).Font(FPinguStyle::Title(18.f)).ColorAndOpacity(Style.Ice)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SPinguButton)
				.Kind(EPinguButtonKind::Arrow)
				.IsFocusable(false)
				.Width(32.f)
				.Height(32.f)
				.ContentPadding(FMargin(0.f))
				.HUD(HUD)
				.OnClicked_Lambda([RowHolder]() { if (TSharedPtr<SPinguButton> Pinned = RowHolder->Pin()) { Pinned->Step(-1, false); } })
				[
					SNew(SPinguArrow).Direction(-1).Size(12.f).Color(Style.Ice)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(ValueWidth)
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock).Text(Value).Font(FPinguStyle::Title(18.f)).ColorAndOpacity(Style.Snow)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SPinguButton)
				.Kind(EPinguButtonKind::Arrow)
				.IsFocusable(false)
				.Width(32.f)
				.Height(32.f)
				.ContentPadding(FMargin(0.f))
				.HUD(HUD)
				.OnClicked_Lambda([RowHolder]() { if (TSharedPtr<SPinguButton> Pinned = RowHolder->Pin()) { Pinned->Step(1, false); } })
				[
					SNew(SPinguArrow).Direction(1).Size(12.f).Color(Style.Ice)
				]
			]
		];
	*RowHolder = Row;
	return Row;
}

TSharedRef<SPinguButton> SPinguRoot::MakeSliderRow(const FText& Label, TAttribute<float> Value, TFunction<void(float)> OnChanged, const FText& InHint)
{
	const FPinguStyle& Style = FPinguStyle::Get();
	TSharedRef<TWeakPtr<SPinguButton>> RowHolder = MakeShared<TWeakPtr<SPinguButton>>();
	const FText RowHint = InHint;
	TSharedRef<SPinguButton> Row = SNew(SPinguButton)
		.Kind(EPinguButtonKind::Row)
		.Height(40.f)
		.HAlign(HAlign_Fill)
		.ContentPadding(FMargin(16.f, 0.f, 16.f, 0.f))
		.HUD(HUD)
		.OnStep_Lambda([Value, OnChanged](int32 Direction)
		{
			OnChanged(FMath::Clamp(FMath::RoundToFloat((Value.Get() + 0.05f * Direction) * 20.f) / 20.f, 0.f, 1.f));
		})
		.OnFocused_Lambda([this, RowHint]() { SetHint(RowHint); })
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Label).Font(FPinguStyle::Title(18.f)).ColorAndOpacity(Style.Ice)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(200.f)
				.HeightOverride(24.f)
				[
					SNew(SPinguSlider)
					.Value(Value)
					.Step(0.05f)
					.IsHot_Lambda([RowHolder]() { const TSharedPtr<SPinguButton> Pinned = RowHolder->Pin(); return Pinned.IsValid() && Pinned->IsHot(); })
					.OnValueChanged_Lambda([OnChanged](float NewValue) { OnChanged(NewValue); })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(64.f)
				.HAlign(HAlign_Right)
				[
					SNew(STextBlock)
					.Text_Lambda([Value]() { return FText::Format(LOCTEXT("Percent", "{0}%"), FText::AsNumber(FMath::RoundToInt(Value.Get() * 100.f))); })
					.Font(FPinguStyle::Title(18.f))
					.ColorAndOpacity(Style.Snow)
				]
			]
		];
	*RowHolder = Row;
	return Row;
}

TSharedRef<SPinguButton> SPinguRoot::MakeKeyCap(EPinguInputAction Action, int32 Index)
{
	// What the action does beyond its name, under the list while one of its keys has the focus.
	FText ActionHint;
	switch (Action)
	{
	case EPinguInputAction::BellySlide:
		ActionHint = LOCTEXT("CapHintSlide", "Tap: slide on your belly - twice as far on ice. Slid three blocks, it knocks penguins away. In the air: a belly-flop.");
		break;
	case EPinguInputAction::Ability1:
	case EPinguInputAction::Ability2:
	case EPinguInputAction::Ability3:
		ActionHint = LOCTEXT("CapHintAbility", "Hold the key of a shrink or grow ray to use it on yourself.");
		break;
	default:
		break;
	}
	return SNew(SPinguButton)
		.Kind(EPinguButtonKind::KeyCap)
		.Width(200.f)
		.Height(40.f)
		.FontSize(18.f)
		.ContentPadding(FMargin(8.f, 0.f))
		.HUD(HUD)
		.IsSelected_Lambda([this, Action, Index]() { return HUD.IsValid() && HUD->IsCapturingKey(Action, Index); })
		.Text_Lambda([this, Action, Index]()
		{
			if (HUD.IsValid() && HUD->IsCapturingKey(Action, Index))
			{
				return LOCTEXT("CapPress", "PRESS A KEY");
			}
			const UPinguSettings* Current = UPinguSettings::Get();
			const FKey Key = Current ? Current->GetKey(Action, Index) : FKey();
			return Key.IsValid() ? FText::FromString(UPinguSettings::GetKeyLabel(Key).ToString().ToUpper()) : LOCTEXT("CapEmpty", "-");
		})
		.OnClicked_Lambda([this, Action, Index]() { if (HUD.IsValid()) { HUD->BeginKeyCapture(Action, Index); } })
		.OnFocused_Lambda([this, ActionHint]()
		{
			if (!ActionHint.IsEmpty())
			{
				SetHint(ActionHint);
				return;
			}
			SetHint(GIsEditor
				? LOCTEXT("CapHintEditor", "Click a key (or press Enter / A) to change it. In the editor's Play mode Esc stops Play: the Pause key opens the menu.")
				: LOCTEXT("CapHint", "Click a key (or press Enter / A) to change it. Esc always opens the menu."));
		});
}

// ============================================================================ settings

TSharedRef<SWidget> SPinguRoot::BuildSettings()
{
	using namespace PinguUISettingsPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();

	// The 3D settings page (PinguMenu3D): the input layer only.
	if (PinguMenu3D::IsEnabled())
	{
		TSharedRef<SPinguMenu3DInput> Input = SNew(SPinguMenu3DInput).HUD(HUD);
		FirstFocus.Add(static_cast<uint8>(EPinguScreen::Settings), Input);
		return MakeScreen(EPinguScreen::Settings, Input, false);
	}

	// ---- presets
	struct FPresetButton
	{
		EPinguGraphicsPreset Preset;
		FText Name;
		FText Caption;
		FText Hint;
	};
	const FPresetButton Presets[] = {
		{ EPinguGraphicsPreset::Low, LOCTEXT("PresetLow", "LOW"), LOCTEXT("PresetLowCaption", "LAPTOP"),
			LOCTEXT("PresetLowHint", "Made for ordinary laptops (integrated graphics): simple lighting and shadows, 60 fps.") },
		{ EPinguGraphicsPreset::Medium, LOCTEXT("PresetMedium", "MEDIUM"), FText::GetEmpty(),
			LOCTEXT("PresetMediumHint", "Between the two: sharper shadows and more effects than Low, still without Lumen.") },
		{ EPinguGraphicsPreset::High, LOCTEXT("PresetHigh", "HIGH"), FText::GetEmpty(),
			LOCTEXT("PresetHighHint", "The full look: Lumen lighting and reflections, detailed materials.") },
		{ EPinguGraphicsPreset::Ultra, LOCTEXT("PresetUltra", "ULTRA"), FText::GetEmpty(),
			LOCTEXT("PresetUltraHint", "Everything at its best. Needs a strong graphics card.") },
	};
	TSharedRef<SHorizontalBox> PresetStrip = SNew(SHorizontalBox);
	TSharedPtr<SPinguButton> FirstPreset;
	for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(Presets)); ++Index)
	{
		const EPinguGraphicsPreset Preset = Presets[Index].Preset;
		const FText PresetHint = Presets[Index].Hint;
		const bool bCaption = !Presets[Index].Caption.IsEmpty();
		TSharedRef<SPinguButton> Button = SNew(SPinguButton)
			.Kind(EPinguButtonKind::Segment)
			.Height(56.f)
			.ContentPadding(FMargin(4.f, 0.f))
			.HUD(HUD)
			.IsSelected_Lambda([Preset]()
			{
				const UPinguSettings* Current = Settings();
				return Current && Current->GetGraphics().Preset == Preset;
			})
			.OnClicked_Lambda([this, Preset]()
			{
				if (UPinguSettings* Current = Settings())
				{
					Current->ApplyPreset(Preset);
					Current->ApplyGraphics(HUD.IsValid() ? HUD->GetWorld() : nullptr);
				}
			})
			.OnFocused_Lambda([this, PresetHint]() { SetHint(PresetHint); })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(STextBlock).Text(Presets[Index].Name).Font(FPinguStyle::Title(22.f)).ColorAndOpacity(Style.Snow)
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Visibility(bCaption ? EVisibility::HitTestInvisible : EVisibility::Collapsed)
					.Text(Presets[Index].Caption)
					.Font(FPinguStyle::Title(12.f))
					.ColorAndOpacity(Style.Ice)
				]
			];
		if (!FirstPreset.IsValid())
		{
			FirstPreset = Button;
		}
		PresetStrip->AddSlot().FillWidth(1.f).Padding(Index > 0 ? 8.f : 0.f, 0.f, 0.f, 0.f)
		[
			Button
		];
	}

	// ---- graphics options
	TSharedRef<SVerticalBox> Graphics = SNew(SVerticalBox);
	auto AddGraphicsRow = [&Graphics](const TSharedRef<SWidget>& Row)
	{
		Graphics->AddSlot().AutoHeight().Padding(0.f, Graphics->NumSlots() > 0 ? 6.f : 0.f, 0.f, 0.f)
		[
			Row
		];
	};
	AddGraphicsRow(MakeOptionRow(LOCTEXT("ResScale", "RESOLUTION SCALE"),
		TAttribute<FText>::CreateLambda([]()
		{
			const UPinguSettings* Current = Settings();
			const int32 Option = Current ? Current->GetGraphics().ResolutionScale : 100;
			const int32 InUse = Current ? Current->GetEffectiveGraphics().ResolutionScale : Option;
			// Low lowered it by itself (this PC could not keep 60 fps): what it is using now.
			return InUse != Option ? FText::Format(LOCTEXT("ResScaleAuto", "AUTO {0}%"), FText::AsNumber(InUse))
				: FText::Format(LOCTEXT("ResScaleValue", "{0}%"), FText::AsNumber(Option));
		}),
		[this](int32 Delta, bool bWrap)
		{
			ChangeGraphics(HUD.IsValid() ? HUD->GetWorld() : nullptr, [Delta, bWrap](FPinguGraphicsOptions& Options)
			{
				const int32 StepIndex = StepValue((FMath::Clamp(Options.ResolutionScale, 50, 100) - 50 + 2) / 5, Delta, 0, 10, bWrap);
				Options.ResolutionScale = 50 + StepIndex * 5;
			});
		},
		LOCTEXT("ResScaleHint", "3D resolution in percent of the window. Lower is faster; menus and text always stay sharp. Low lowers it by itself (AUTO) when the PC can't keep 60 fps."),
		TAttribute<bool>(true), 40.f));
	AddGraphicsRow(MakeOptionRow(LOCTEXT("AntiAliasing", "ANTI-ALIASING"),
		TAttribute<FText>::CreateLambda([]()
		{
			const UPinguSettings* Current = Settings();
			const EPinguAntiAliasing Option = Current ? Current->GetGraphics().AntiAliasing : EPinguAntiAliasing::TSR;
			const EPinguAntiAliasing InUse = Current ? Current->GetEffectiveGraphics().AntiAliasing : Option;
			const FString Name = UPinguSettings::GetAntiAliasingName(InUse).ToString().ToUpper();
			return FText::FromString(InUse != Option ? TEXT("AUTO ") + Name : Name);
		}),
		[this](int32 Delta, bool bWrap)
		{
			ChangeGraphics(HUD.IsValid() ? HUD->GetWorld() : nullptr, [Delta, bWrap](FPinguGraphicsOptions& Options)
			{
				Options.AntiAliasing = static_cast<EPinguAntiAliasing>(StepValue(static_cast<int32>(Options.AntiAliasing), Delta,
					static_cast<int32>(EPinguAntiAliasing::Off), static_cast<int32>(EPinguAntiAliasing::TSR), bWrap));
			});
		},
		LOCTEXT("AntiAliasingHint", "Smooths jagged edges. TSR looks best, FXAA is the cheapest."),
		TAttribute<bool>(true), 40.f));

	const FQualityRow QualityRows[] = {
		{ LOCTEXT("Shadows", "SHADOWS"), LOCTEXT("ShadowsHint", "Shadow quality. Low or off helps a lot on laptops."), &FPinguGraphicsOptions::Shadows, true },
		{ LOCTEXT("Lighting", "LIGHTING"), LOCTEXT("LightingHint", "Low / Medium: a simple sky light. High / Ultra: Lumen global illumination (a strong graphics card)."), &FPinguGraphicsOptions::Lighting, false },
		{ LOCTEXT("Reflections", "REFLECTIONS"), LOCTEXT("ReflectionsHint", "Reflections on the ice and the sea."), &FPinguGraphicsOptions::Reflections, true },
		{ LOCTEXT("Effects", "EFFECTS"), LOCTEXT("EffectsHint", "How many particles: snow puffs, sparkles, trails. Ultra adds richer effects: flashes, sparks, glints, drifting snow."), &FPinguGraphicsOptions::Effects, false },
		{ LOCTEXT("PostProcess", "POST PROCESS"), LOCTEXT("PostProcessHint", "Bloom, ambient occlusion and the other screen effects."), &FPinguGraphicsOptions::PostProcess, true },
		{ LOCTEXT("ModelDetail", "MODEL DETAIL"), LOCTEXT("ModelDetailHint", "Penguin and island detail. High / Ultra: the detailed felt and snow materials."), &FPinguGraphicsOptions::ModelDetail, false },
		{ LOCTEXT("Textures", "TEXTURES"), LOCTEXT("TexturesHint", "Texture resolution. Lower needs less video memory."), &FPinguGraphicsOptions::Textures, false },
	};
	for (const FQualityRow& Quality : QualityRows)
	{
		EPinguQualityLevel FPinguGraphicsOptions::* Member = Quality.Member;
		const int32 Min = Quality.bAllowOff ? static_cast<int32>(EPinguQualityLevel::Off) : static_cast<int32>(EPinguQualityLevel::Low);
		AddGraphicsRow(MakeOptionRow(Quality.Label,
			TAttribute<FText>::CreateLambda([Member]()
			{
				const UPinguSettings* Current = Settings();
				const EPinguQualityLevel Option = Current ? Current->GetGraphics().*Member : EPinguQualityLevel::High;
				const EPinguQualityLevel InUse = Current ? Current->GetEffectiveGraphics().*Member : Option;
				return InUse != Option ? FText::Format(LOCTEXT("QualityAuto", "AUTO {0}"), QualityName(InUse)) : QualityName(Option);
			}),
			[this, Member, Min](int32 Delta, bool bWrap)
			{
				ChangeGraphics(HUD.IsValid() ? HUD->GetWorld() : nullptr, [Member, Min, Delta, bWrap](FPinguGraphicsOptions& Options)
				{
					Options.*Member = static_cast<EPinguQualityLevel>(StepValue(static_cast<int32>(Options.*Member), Delta, Min, static_cast<int32>(EPinguQualityLevel::Ultra), bWrap));
				});
			},
			Quality.Hint, TAttribute<bool>(true), 40.f));
	}
	AddGraphicsRow(MakeOptionRow(LOCTEXT("ShowFps", "SHOW FPS"),
		TAttribute<FText>::CreateLambda([]() { const UPinguSettings* Current = Settings(); return OnOff(Current && Current->GetGraphics().bShowFPS); }),
		[this](int32 Delta, bool bWrap)
		{
			ChangeGraphics(HUD.IsValid() ? HUD->GetWorld() : nullptr, [](FPinguGraphicsOptions& Options) { Options.bShowFPS = !Options.bShowFPS; });
		},
		LOCTEXT("ShowFpsHint", "Frames per second in the top right corner."),
		TAttribute<bool>(true), 40.f));

	// ---- display
	TSharedRef<SVerticalBox> Display = SNew(SVerticalBox);
	TSharedPtr<SWidget> FirstDisplayRow;
	auto AddDisplayRow = [&Display, &FirstDisplayRow](const TSharedRef<SWidget>& Row)
	{
		if (!FirstDisplayRow.IsValid())
		{
			FirstDisplayRow = Row;
		}
		Display->AddSlot().AutoHeight().Padding(0.f, Display->NumSlots() > 0 ? 6.f : 0.f, 0.f, 0.f)
		[
			Row
		];
	};
	AddDisplayRow(MakeOptionRow(LOCTEXT("WindowMode", "WINDOW MODE"),
		TAttribute<FText>::CreateLambda([]()
		{
			const UPinguSettings* Current = Settings();
			switch (Current ? Current->GetWindowMode() : EWindowMode::Windowed)
			{
			case EWindowMode::Fullscreen: return LOCTEXT("Fullscreen", "FULLSCREEN");
			case EWindowMode::WindowedFullscreen: return LOCTEXT("Borderless", "BORDERLESS");
			default: return LOCTEXT("Windowed", "WINDOWED");
			}
		}),
		[](int32 Delta, bool bWrap)
		{
			if (UPinguSettings* Current = Settings())
			{
				const int32 Mode = StepValue(static_cast<int32>(Current->GetWindowMode()), Delta, 0, 2, bWrap);
				Current->SetWindowMode(static_cast<EWindowMode::Type>(Mode));
			}
		},
		LOCTEXT("WindowModeHint", "Fullscreen, a borderless window or a normal window. (In the editor's Play mode this may do nothing.)"),
		TAttribute<bool>(true), 40.f, 200.f));
	AddDisplayRow(MakeOptionRow(LOCTEXT("Resolution", "RESOLUTION"),
		TAttribute<FText>::CreateLambda([]()
		{
			const UPinguSettings* Current = Settings();
			const FIntPoint Resolution = Current ? Current->GetResolution() : FIntPoint(1920, 1080);
			return FText::FromString(FString::Printf(TEXT("%d x %d"), Resolution.X, Resolution.Y));
		}),
		[](int32 Delta, bool bWrap)
		{
			UPinguSettings* Current = Settings();
			if (!Current)
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
		},
		LOCTEXT("ResolutionHint", "The screen / window resolution. (In the editor's Play mode this may do nothing.)"),
		TAttribute<bool>(true), 40.f, 200.f));
	AddDisplayRow(MakeOptionRow(LOCTEXT("VSync", "VSYNC"),
		TAttribute<FText>::CreateLambda([]() { const UPinguSettings* Current = Settings(); return OnOff(Current && Current->GetVSync()); }),
		[](int32 Delta, bool bWrap) { if (UPinguSettings* Current = Settings()) { Current->SetVSync(!Current->GetVSync()); } },
		LOCTEXT("VSyncHint", "Waits for the monitor: no tearing, a little more input delay."),
		TAttribute<bool>(true), 40.f, 200.f));
	AddDisplayRow(MakeOptionRow(LOCTEXT("FrameCap", "FRAME RATE CAP"),
		TAttribute<FText>::CreateLambda([]()
		{
			const UPinguSettings* Current = Settings();
			const int32 Limit = Current ? Current->GetFrameRateLimit() : 0;
			return Limit > 0 ? FText::Format(LOCTEXT("FrameCapValue", "{0} FPS"), FText::AsNumber(Limit)) : LOCTEXT("Unlimited", "UNLIMITED");
		}),
		[](int32 Delta, bool bWrap)
		{
			UPinguSettings* Current = Settings();
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
		},
		LOCTEXT("FrameCapHint", "The most frames per second. 60 keeps a laptop cool and saves battery."),
		TAttribute<bool>(true), 40.f, 200.f));

	// ---- audio
	auto SetVolume = [this](int32 Which, float NewValue)
	{
		UPinguSettings* Current = Settings();
		if (!Current)
		{
			return;
		}
		float Master = Current->GetMasterVolume();
		float Music = Current->GetMusicVolume();
		float Sfx = Current->GetSfxVolume();
		(Which == 0 ? Master : (Which == 1 ? Music : Sfx)) = FMath::Clamp(NewValue, 0.f, 1.f);
		Current->SetVolumes(Master, Music, Sfx);
		Current->ApplyAudio(HUD.IsValid() ? HUD->GetWorld() : nullptr);
	};
	TSharedRef<SPinguButton> FirstAudioRow = MakeSliderRow(LOCTEXT("MasterVolume", "MASTER"),
		TAttribute<float>::CreateLambda([]() { const UPinguSettings* Current = Settings(); return Current ? Current->GetMasterVolume() : 1.f; }),
		[SetVolume](float NewValue) { SetVolume(0, NewValue); },
		LOCTEXT("MasterHint", "Everything."));
	TSharedRef<SVerticalBox> Audio = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			FirstAudioRow
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			MakeSliderRow(LOCTEXT("MusicVolume", "MUSIC"),
				TAttribute<float>::CreateLambda([]() { const UPinguSettings* Current = Settings(); return Current ? Current->GetMusicVolume() : 0.8f; }),
				[SetVolume](float NewValue) { SetVolume(1, NewValue); },
				LOCTEXT("MusicHint", "The music."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			MakeSliderRow(LOCTEXT("EffectsVolume", "EFFECTS"),
				TAttribute<float>::CreateLambda([]() { const UPinguSettings* Current = Settings(); return Current ? Current->GetSfxVolume() : 1.f; }),
				[SetVolume](float NewValue) { SetVolume(2, NewValue); },
				LOCTEXT("EffectsVolumeHint", "Sound effects: hits, splashes, the menus."))
		];

	// ---- the three tabs (only one shows; the hidden ones keep their size so the card never jumps)
	TSharedRef<SWidget> GraphicsTab = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeSection(LOCTEXT("PresetSection", "PRESET"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			PresetStrip
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 16.f, 0.f, 0.f)
		[
			Graphics
		];
	TSharedRef<SWidget> DisplayTab = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			Display
		];
	TSharedRef<SWidget> AudioTab = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			Audio
		];
	SettingsTabFirst[0] = FirstPreset;
	SettingsTabFirst[1] = FirstDisplayRow;
	SettingsTabFirst[2] = FirstAudioRow;

	const FText TabNames[] = { LOCTEXT("TabGraphics", "GRAPHICS"), LOCTEXT("TabDisplay", "DISPLAY"), LOCTEXT("TabAudio", "AUDIO") };
	TSharedRef<SHorizontalBox> TabStrip = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(STextBlock).Text(LOCTEXT("TabPrev", "LB")).Font(FPinguStyle::Title(14.f)).ColorAndOpacity(Style.IceDim)
		];
	for (int32 Tab = 0; Tab < 3; ++Tab)
	{
		TabStrip->AddSlot().AutoWidth().Padding(Tab > 0 ? 8.f : 0.f, 0.f, 0.f, 0.f)
		[
			SAssignNew(SettingsTabButtons[Tab], SPinguButton)
			.Kind(EPinguButtonKind::Segment)
			.Width(176.f)
			.Height(FPinguStyle::SmallHeight)
			.FontSize(20.f)
			.Text(TabNames[Tab])
			.HUD(HUD)
			.IsSelected_Lambda([this, Tab]() { return SettingsTab == Tab; })
			.OnClicked_Lambda([this, Tab]() { SelectSettingsTab(Tab, false); })
			.OnFocused_Lambda([this]() { SetHint(LOCTEXT("TabHint", "LB / RB switch between the pages.")); })
		];
	}
	TabStrip->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 0.f, 0.f)
	[
		SNew(STextBlock).Text(LOCTEXT("TabNext", "RB")).Font(FPinguStyle::Title(14.f)).ColorAndOpacity(Style.IceDim)
	];

	auto TabVisibility = [this](int32 Tab)
	{
		return TAttribute<EVisibility>::CreateLambda([this, Tab]() { return SettingsTab == Tab ? EVisibility::SelfHitTestInvisible : EVisibility::Hidden; });
	};

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				MakeTitle(LOCTEXT("SettingsTitle", "SETTINGS"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox)
				.Visibility_Lambda([]()
				{
					const UPinguSettings* Current = Settings();
					return Current && Current->GetGraphics().Preset == EPinguGraphicsPreset::Custom ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
				})
				[
					MakeTag(LOCTEXT("CustomTag", "CUSTOM"), Style.Gold, Style.Ink)
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		[
			TabStrip
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot().VAlign(VAlign_Top)
			[
				SNew(SBox).Visibility(TabVisibility(0))
				[
					GraphicsTab
				]
			]
			+ SOverlay::Slot().VAlign(VAlign_Top)
			[
				SNew(SBox).Visibility(TabVisibility(1))
				[
					DisplayTab
				]
			]
			+ SOverlay::Slot().VAlign(VAlign_Top)
			[
				SNew(SBox).Visibility(TabVisibility(2))
				[
					AudioTab
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			// The keys: the controls screen opens from here (the title has no button of its own for them).
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SPinguButton)
				.Width(200.f)
				.Text(LOCTEXT("SettingsControls", "CONTROLS"))
				.HUD(HUD)
				.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Controls(); } })
				.OnFocused_Lambda([this]() { SetHint(LOCTEXT("SettingsControlsHint", "Which key does what - change any of them.")); })
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SSpacer)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SPinguButton)
				.Kind(EPinguButtonKind::Primary)
				.Width(200.f)
				.Text(LOCTEXT("SettingsBack", "BACK"))
				.HUD(HUD)
				.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Back(); } })
				.OnFocused_Lambda([this]() { SetHint(LOCTEXT("SettingsBackHint", "Everything is saved as you go.")); })
			]
		];
	FirstFocus.Add(static_cast<uint8>(EPinguScreen::Settings), FirstPreset);

	return MakeScreen(EPinguScreen::Settings,
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			MakeCard(Box, FMargin(40.f, 24.f, 40.f, 32.f), 800.f)
		]);
}

void SPinguRoot::SelectSettingsTab(int32 Tab, bool bFocusFirst)
{
	SettingsTab = (Tab % 3 + 3) % 3;
	if (APinguMenu3D* Menu3D = PinguMenu3D::IsEnabled() && HUD.IsValid() ? APinguMenu3D::Get(HUD.Get()) : nullptr)
	{
		Menu3D->SelectSettingsTab(SettingsTab, bFocusFirst);
		return;
	}
	SetHint(FText::GetEmpty());
	// The focus must not stay on a row of a page that just went hidden. The new page shows after the next layout
	// pass, so the focus moves there from Tick.
	PendingFocus = bFocusFirst ? SettingsTabFirst[SettingsTab] : SettingsTabButtons[SettingsTab];
	PendingFocusUntil = PinguUI::Now() + 0.5;
}

// ============================================================================ controls

TSharedRef<SWidget> SPinguRoot::BuildControls()
{
	const FPinguStyle& Style = FPinguStyle::Get();
	// The 3D controls page: the input layer only (the key capture keeps its own Slate layer, which takes the keys).
	if (PinguMenu3D::IsEnabled())
	{
		TSharedRef<SPinguMenu3DInput> Input = SNew(SPinguMenu3DInput).HUD(HUD);
		FirstFocus.Add(static_cast<uint8>(EPinguScreen::Controls), Input);
		return MakeScreen(EPinguScreen::Controls, Input, false);
	}
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	// Column headers.
	Rows->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(296.f)
			[
				MakeSection(LOCTEXT("ColAction", "ACTION"))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(208.f)
			[
				MakeSection(LOCTEXT("ColKey1", "KEY"))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(232.f)
			[
				MakeSection(LOCTEXT("ColKey2", "OTHER KEY"))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			MakeSection(LOCTEXT("ColGamepad", "GAMEPAD"))
		]
	];
	// Every action once, in the order of the enum - except that the belly slide (added last, see PinguSettings.h) sits
	// right under the dash, with the other moves.
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
	TSharedPtr<SWidget> FirstCap;
	for (int32 ActionIndex = 0; ActionIndex < ActionOrder.Num(); ++ActionIndex)
	{
		const EPinguInputAction Action = ActionOrder[ActionIndex];
		TSharedRef<SPinguButton> Primary = MakeKeyCap(Action, 0);
		if (!FirstCap.IsValid())
		{
			FirstCap = Primary;
		}
		Rows->AddSlot().AutoHeight().Padding(0.f, ActionIndex > 0 ? 6.f : 0.f, 0.f, 0.f)
		[
			SNew(SBox)
			.HeightOverride(40.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(296.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(UPinguSettings::GetActionName(Action).ToString().ToUpper()))
						.Font(FPinguStyle::Title(18.f))
						.ColorAndOpacity(Style.Snow)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					Primary
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					MakeKeyCap(Action, 1)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(32.f, 0.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text_Lambda([Action]()
					{
						// Read only: the gamepad layout is fixed.
						TArray<FString> Labels;
						for (const FKey& Key : UPinguSettings::GetGamepadKeys(Action))
						{
							Labels.Add(UPinguSettings::GetKeyLabel(Key).ToString().ToUpper());
						}
						return Labels.Num() > 0 ? FText::FromString(FString::Join(Labels, TEXT(" / "))) : LOCTEXT("NoPad", "-");
					})
					.Font(FPinguStyle::Title(16.f))
					.ColorAndOpacity(Style.IceDim)
				]
			]
		];
	}

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeTitle(LOCTEXT("ControlsTitle", "CONTROLS"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		[
			Rows
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SPinguButton)
				.Kind(EPinguButtonKind::Ghost)
				.Width(296.f)
				.Height(FPinguStyle::ButtonHeight)
				.FontSize(20.f)
				.Text(LOCTEXT("ResetKeys", "RESET TO DEFAULTS"))
				.HUD(HUD)
				.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_ResetKeys(); } })
				.OnFocused_Lambda([this]() { SetHint(LOCTEXT("ResetHint", "Back to the standard keys (Space jumps, Shift dashes, Ctrl belly slides, W / S aim the dash).")); })
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SSpacer)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SPinguButton)
				.Kind(EPinguButtonKind::Primary)
				.Width(200.f)
				.Text(LOCTEXT("ControlsBack", "BACK"))
				.HUD(HUD)
				.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Back(); } })
				.OnFocused_Lambda([this]() { SetHint(LOCTEXT("ControlsBackHint", "The keys are saved as you change them.")); })
			]
		];
	FirstFocus.Add(static_cast<uint8>(EPinguScreen::Controls), FirstCap);

	return MakeScreen(EPinguScreen::Controls,
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			MakeCard(Box, FMargin(40.f, 24.f, 40.f, 32.f), 1120.f)
		]);
}

#undef LOCTEXT_NAMESPACE
