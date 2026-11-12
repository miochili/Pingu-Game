// Pingu - the 3D menu's other pages (the whole-menu job, 2026-10-24: pause, LAN, results, settings, controls, PINGUPEDIA, the
// round board; PinguMenu3DPages.cpp, PinguMenu3DSettings.cpp, PinguMenu3DPedia.cpp) in two autotest steps:
//   clicks_Menu3DScreens (APenguinAutoTest::AddMenuScreensClickStep; alone: PINGU_AUTOTEST_ONLY=clicks_Menu3DScreens): every
//     focusable button of every page at five points over its face (the middle and the edges at 96 %), real Slate clicks as
//     clicks_Menu3D sends them, at the game's 1280x720 and at 1920x1080: every click must press that button. Then the LAN page's
//     address field with real key events (typed characters, Ctrl+V from the clipboard, Backspace, Enter does the field's action),
//     a key cap of the controls page: a click starts the capture (the PRESS A KEY overlay shows), a real key binds it (then put
//     back), and the frame time with the results page up against the frame time with no page (the fps note).
//   look_Menu3DScreens (AddMenu3DScreensLookStep; looktest): pictures of every page at 1920x1080 (HighResShot): 75_menu3d_<page>_1080
//     (pause with SETTINGS hot, lan with an address typed, results, the three settings pages, controls and its PRESS A KEY overlay,
//     the PINGUPEDIA's Belly Stomp and Jester cards, the round board).

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinHUD.h"
#include "PinguMenu3D.h"
#include "PinguSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Slate/SceneViewport.h"
#include "UnrealClient.h"

namespace PenguinAutoTestScreens3DPrivate
{
	constexpr float EdgeShare = 0.96f;
	constexpr float PressGap = 0.13f;

	/** A page to click through: the HUD screen that shows it, the 3D page, a settings tab (-1: none). */
	struct FPageJob
	{
		EPinguScreen Screen;
		EPinguMenu3DPage Page;
		int32 SettingsTab;
		const TCHAR* Label;
		/** The card lobby's bots for this run (0: as the game has them), every other one in a team. */
		int32 Bots = 0;
	};
	static const FPageJob Jobs[] = {
		{ EPinguScreen::Pause, EPinguMenu3DPage::Pause, -1, TEXT("pause") },
		{ EPinguScreen::Lan, EPinguMenu3DPage::Lan, -1, TEXT("lan") },
		{ EPinguScreen::Results, EPinguMenu3DPage::Results, -1, TEXT("results") },
		{ EPinguScreen::Settings, EPinguMenu3DPage::Settings, 0, TEXT("settings graphics") },
		{ EPinguScreen::Settings, EPinguMenu3DPage::Settings, 1, TEXT("settings display") },
		{ EPinguScreen::Settings, EPinguMenu3DPage::Settings, 2, TEXT("settings audio") },
		{ EPinguScreen::Controls, EPinguMenu3DPage::Controls, -1, TEXT("controls") },
		{ EPinguScreen::Pingupedia, EPinguMenu3DPage::Pingupedia, -1, TEXT("pingupedia") },
		// (The wooden card lobby, 2026-10-29: every card that is yours, a bot's difficulty and x, ADD BOT, the signs.)
		{ EPinguScreen::Lobby, EPinguMenu3DPage::Lobby, -1, TEXT("card lobby"), 4 },
		// (A full lobby too, 2026-10-29: nine bot cards - BOT, difficulty and x on every one, teams on.)
		{ EPinguScreen::Lobby, EPinguMenu3DPage::Lobby, -1, TEXT("card lobby full"), 9 },
	};
	constexpr int32 NumJobs = static_cast<int32>(UE_ARRAY_COUNT(Jobs));

	struct FClick
	{
		int32 ButtonIndex = INDEX_NONE;
		FVector2D FaceShare = FVector2D::ZeroVector;
		FVector2D Pixel = FVector2D::ZeroVector;
		FName ButtonId;
	};

	struct FScreensRun
	{
		TWeakObjectPtr<APinguMenu3D> Menu;
		TWeakObjectPtr<APenguinHUD> HUD;
		TArray<FName> Fired;
		int32 Stage = 0;
		int32 Job = 0;
		int32 SizePass = 0;
		float NextAt = 0.f;
		TArray<FClick> Queue;
		int32 QueueAt = 0;
		int32 Counted = 0;
		int32 FiredBefore = 0;
		int32 Hits = 0;
		int32 Misses = 0;
		FString MissNotes;
		FIntPoint Size = FIntPoint::ZeroValue;
		int32 Runs = 0;
		bool bAllOk = true;
		// the field
		FString TypedShown;
		FString PastedShown;
		FString BackspacedShown;
		bool bEnterFired = false;
		FString ClipboardSaved;
		// the key capture
		bool bCaptureStarted = false;
		bool bOverlayShown = false;
		FKey OldKey;
		FKey NewKey;
		bool bBound = false;
		bool bCaptureEnded = false;
		// the frame time
		double FrameAccum = 0.0;
		int32 FrameCount = 0;
		double LastReal = 0.0;
		float NoMenuMs = 0.f;
		float ResultsMs = 0.f;
		/** (2026-10-26, the wood menus: the title page too - its hanging board, the post, the signs.) */
		float TitleMs = 0.f;
		/** The match settings before the card lobby runs changed the bots (put back at the end). */
		FPinguMatchSettings SavedSettings;
		bool bSettingsSaved = false;
	};
	static FScreensRun& Run()
	{
		static FScreensRun State;
		return State;
	}

	static FSceneViewport* SceneViewport(const UWorld* World)
	{
		UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
		return Client ? Client->GetGameViewport() : nullptr;
	}

	static bool SlateClick(const UWorld* World, const FVector2D& Pixel)
	{
		FSceneViewport* Viewport = SceneViewport(World);
		if (!Viewport || !FSlateApplication::IsInitialized())
		{
			return false;
		}
		FSlateApplication& Slate = FSlateApplication::Get();
		const FVector2D Screen = PinguMenu3D::ScreenFromGeometry(Viewport->GetCachedGeometry(), Pixel);
		TSet<FKey> Pressed;
		Pressed.Add(EKeys::LeftMouseButton);
		const FPointerEvent Move(0u, FSlateApplication::CursorPointerIndex, Screen, Screen, TSet<FKey>(), EKeys::Invalid, 0.f, Slate.GetModifierKeys());
		Slate.ProcessMouseMoveEvent(Move);
		const FPointerEvent Down(0u, FSlateApplication::CursorPointerIndex, Screen, Screen, Pressed, EKeys::LeftMouseButton, 0.f, Slate.GetModifierKeys());
		Slate.ProcessMouseButtonDownEvent(nullptr, Down);
		const FPointerEvent Up(0u, FSlateApplication::CursorPointerIndex, Screen, Screen, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, Slate.GetModifierKeys());
		Slate.ProcessMouseButtonUpEvent(Up);
		return true;
	}

	static void SetRes(UWorld* World, int32 Width, int32 Height)
	{
		if (GEngine)
		{
			GEngine->Exec(World, *FString::Printf(TEXT("r.SetRes %dx%dw"), Width, Height));
		}
	}

	static FIntPoint ViewSize(const UWorld* World)
	{
		const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		FVector2D Size;
		return PinguMenu3D::GetViewportPixelSize(Controller, Size) ? FIntPoint(FMath::RoundToInt(Size.X), FMath::RoundToInt(Size.Y)) : FIntPoint::ZeroValue;
	}

	/** Every focusable button of the page that shows at five points (the middle, the four edge middles). */
	static void QueueButtons(FScreensRun& State)
	{
		APinguMenu3D* Menu = State.Menu.Get();
		static const FVector2D Points[5] = { FVector2D(0.f, 0.f), FVector2D(-EdgeShare, 0.f), FVector2D(EdgeShare, 0.f), FVector2D(0.f, -EdgeShare), FVector2D(0.f, EdgeShare) };
		for (int32 Index = 0; Menu && Index < Menu->GetButtonCount(); ++Index)
		{
			const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
			if (!Info.bFocusable || Info.ScreenSize.X <= 0.f)
			{
				continue;
			}
			for (const FVector2D& Point : Points)
			{
				FClick& Click = State.Queue.AddDefaulted_GetRef();
				Click.ButtonIndex = Index;
				Click.FaceShare = Point;
				Click.ButtonId = Info.Id;
			}
		}
	}

	/** Sends a real character (as typing does) and a real key (down + up) through Slate. */
	static void TypeString(const FString& Text)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		FSlateApplication& Slate = FSlateApplication::Get();
		for (const TCHAR Char : Text)
		{
			const FCharacterEvent Event(Char, Slate.GetModifierKeys(), 0u, false);
			Slate.ProcessKeyCharEvent(Event);
		}
	}

	static void PressKey(const FKey& Key, bool bControl)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		FSlateApplication& Slate = FSlateApplication::Get();
		const FModifierKeysState Modifiers(false, false, bControl, bControl, false, false, false, false, false);
		const FKeyEvent Down(Key, Modifiers, 0u, false, 0u, 0u);
		Slate.ProcessKeyDownEvent(Down);
		const FKeyEvent Up(Key, Modifiers, 0u, false, 0u, 0u);
		Slate.ProcessKeyUpEvent(Up);
	}

	static FString FieldText(const APinguMenu3D* Menu, FName Id)
	{
		for (int32 Index = 0; Menu && Index < Menu->GetButtonCount(); ++Index)
		{
			const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
			if (Info.Id == Id)
			{
				return Info.Label;
			}
		}
		return FString();
	}

	static int32 FindButton(const APinguMenu3D* Menu, FName Id)
	{
		for (int32 Index = 0; Menu && Index < Menu->GetButtonCount(); ++Index)
		{
			if (Menu->GetButtonInfo(Index).Id == Id)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	static FString ShotPath(const TCHAR* Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ClaudeLogs/Shots") / FString(Name) + TEXT(".png"));
	}
}

void APenguinAutoTest::AddMenuScreensClickStep()
{
	using namespace PenguinAutoTestScreens3DPrivate;
	AddStep(TEXT("clicks_Menu3DScreens"), 270.f,
		[this]
		{
			FScreensRun& State = Run();
			State = FScreensRun();
			UWorld* World = GetWorld();
			APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
			APenguinHUD* HUD = Controller ? Cast<APenguinHUD>(Controller->GetHUD()) : nullptr;
			State.HUD = HUD;
			State.Menu = PinguMenu3D::IsEnabled() && HUD ? APinguMenu3D::Get(HUD) : nullptr;
			State.ClipboardSaved = APenguinHUD::ReadClipboardText();
		},
		[this](float T)
		{
			FScreensRun& State = Run();
			UWorld* World = GetWorld();
			APinguMenu3D* Menu = State.Menu.Get();
			APenguinHUD* HUD = State.HUD.Get();
			if (!Menu || !HUD)
			{
				return;
			}
			// The real frame time, averaged over the stage (the fps note).
			const double Now = FPlatformTime::Seconds();
			if (State.LastReal > 0.0)
			{
				State.FrameAccum += Now - State.LastReal;
				++State.FrameCount;
			}
			State.LastReal = Now;
			if (T < State.NextAt)
			{
				return;
			}
			auto FinishRun = [this, &State](const FString& Label)
			{
				const bool bOk = State.Misses == 0 && State.Hits > 0;
				State.bAllOk &= bOk;
				++State.Runs;
				Report(FString::Printf(TEXT("RESULT clicks %s at %dx%d: %d/%d points hit%s %s"), *Label, State.Size.X, State.Size.Y, State.Hits, State.Hits + State.Misses,
					State.MissNotes.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" missed: %s"), *State.MissNotes), bOk ? TEXT("ok") : TEXT("FAIL")));
				State.Queue.Reset();
				State.QueueAt = 0;
				State.Counted = 0;
				State.FiredBefore = 0;
				State.Hits = 0;
				State.Misses = 0;
				State.MissNotes.Reset();
			};
			// One click of the queue; true when the queue is done.
			auto Pump = [&]() -> bool
			{
				if (Menu->IsPressPending())
				{
					return false;
				}
				if (State.Counted < State.QueueAt)
				{
					const FClick& Last = State.Queue[State.QueueAt - 1];
					const bool bHit = State.Fired.Num() == State.FiredBefore + 1 && State.Fired.Last() == Last.ButtonId;
					State.Hits += bHit ? 1 : 0;
					State.Misses += bHit ? 0 : 1;
					if (!bHit && State.MissNotes.Len() < 900)
					{
						State.MissNotes += FString::Printf(TEXT("%s%s at %.0f,%.0f fired %s"), State.MissNotes.IsEmpty() ? TEXT("") : TEXT("; "), *Last.ButtonId.ToString(),
							Last.Pixel.X, Last.Pixel.Y, State.Fired.Num() > State.FiredBefore ? *State.Fired.Last().ToString() : TEXT("nothing"));
					}
					State.Counted = State.QueueAt;
				}
				if (State.QueueAt >= State.Queue.Num())
				{
					return true;
				}
				FClick& Click = State.Queue[State.QueueAt++];
				State.FiredBefore = State.Fired.Num();
				FVector2D Pixel;
				if (Menu->GetButtonFacePixel(Click.ButtonIndex, Click.FaceShare, Pixel))
				{
					Click.Pixel = Pixel;
				}
				SlateClick(World, Click.Pixel);
				State.NextAt = T + PressGap;
				return false;
			};
			switch (State.Stage)
			{
			case 0: // the first page
				if (T > 1.0f)
				{
					State.FrameAccum = 0.0;
					State.FrameCount = 0;
					State.Stage = 1;
				}
				break;
			case 1: // show the job's page
			{
				const FPageJob& Job = Jobs[State.Job];
				if (Job.Bots > 0)
				{
					if (APenguinGameMode* GameMode = World ? World->GetAuthGameMode<APenguinGameMode>() : nullptr)
					{
						if (!State.bSettingsSaved)
						{
							State.SavedSettings = GameMode->GetSettings();
							State.bSettingsSaved = true;
						}
						FPinguMatchSettings Settings = GameMode->GetSettings();
						Settings.Bots.SetNum(Job.Bots);
						for (int32 Index = 0; Index < Job.Bots; ++Index)
						{
							Settings.Bots[Index].Name = FString::Printf(TEXT("Bot%d"), Index + 1);
							Settings.Bots[Index].Team = Job.Bots > 4 ? (Index % 2 == 0 ? Index % 4 : -1) : -1;
							Settings.Bots[Index].Skill = Index % 3;
						}
						Settings.NumBots = Job.Bots;
						GameMode->SetMatchSettings(Settings);
					}
				}
				HUD->SetTestScreen(Job.Screen);
				Menu->SetTestPage(Job.Page, [](FName Id) { Run().Fired.Add(Id); });
				if (Job.SettingsTab >= 0)
				{
					Menu->SelectSettingsTab(Job.SettingsTab, true);
				}
				State.NextAt = T + 0.9f;
				State.Stage = 2;
				break;
			}
			case 2: // queue its buttons
				if (Jobs[State.Job].SettingsTab >= 0)
				{
					Menu->SelectSettingsTab(Jobs[State.Job].SettingsTab, true);
				}
				State.Size = ViewSize(World);
				State.Fired.Reset();
				QueueButtons(State);
				State.Stage = 3;
				break;
			case 3:
				if (Pump())
				{
					FinishRun(Jobs[State.Job].Label);
					++State.Job;
					if (State.Job < NumJobs)
					{
						State.Stage = 1;
					}
					else if (State.SizePass == 0)
					{
						// Once more at 1920x1080.
						State.SizePass = 1;
						State.Job = 0;
						SetRes(World, 1920, 1080);
						State.NextAt = T + 1.2f;
						State.Stage = 1;
					}
					else
					{
						// The frame time at 1920x1080 with no page, then with the results page up (10 rows at most; whatever the arena has).
						HUD->SetTestScreen(EPinguScreen::None);
						Menu->SetTestPage(EPinguMenu3DPage::None);
						State.FrameAccum = 0.0;
						State.FrameCount = 0;
						State.NextAt = T + 1.5f;
						State.Stage = 31;
					}
				}
				break;
			case 31:
				State.NoMenuMs = State.FrameCount > 0 ? static_cast<float>(1000.0 * State.FrameAccum / State.FrameCount) : 0.f;
				HUD->SetTestScreen(EPinguScreen::Results);
				Menu->SetTestPage(EPinguMenu3DPage::Results, [](FName Id) { Run().Fired.Add(Id); });
				State.FrameAccum = 0.0;
				State.FrameCount = 0;
				State.NextAt = T + 1.5f;
				State.Stage = 32;
				break;
			case 32: // the results' frame time, then the title's
				State.ResultsMs = State.FrameCount > 0 ? static_cast<float>(1000.0 * State.FrameAccum / State.FrameCount) : 0.f;
				Menu->SetTestPage(EPinguMenu3DPage::Title, [](FName Id) { Run().Fired.Add(Id); });
				State.FrameAccum = 0.0;
				State.FrameCount = 0;
				State.NextAt = T + 1.5f;
				State.Stage = 4;
				break;
			case 4: // the title's frame time, then the LAN page's field
				State.TitleMs = State.FrameCount > 0 ? static_cast<float>(1000.0 * State.FrameAccum / State.FrameCount) : 0.f;
				SetRes(World, 1280, 720);
				HUD->SetTestScreen(EPinguScreen::Lan);
				Menu->SetTestPage(EPinguMenu3DPage::Lan, [](FName Id) { Run().Fired.Add(Id); });
				State.NextAt = T + 1.2f;
				State.Stage = 5;
				break;
			case 5: // type into the address field
				Menu->FocusButton(TEXT("Address"));
				State.Fired.Reset();
				PressKey(EKeys::Delete, false); // (the field may still hold what an earlier step typed)
				TypeString(TEXT("192.168.1.5"));
				State.TypedShown = FieldText(Menu, TEXT("Address"));
				APenguinHUD::CopyTextToClipboard(TEXT("10.0.0.7"));
				PressKey(EKeys::V, true);
				State.PastedShown = FieldText(Menu, TEXT("Address"));
				PressKey(EKeys::BackSpace, false);
				PressKey(EKeys::BackSpace, false);
				State.BackspacedShown = FieldText(Menu, TEXT("Address"));
				PressKey(EKeys::Enter, false);
				State.NextAt = T + 0.3f;
				State.Stage = 6;
				break;
			case 6: // Enter fired the field's action; then the controls page for real (the capture overlay needs the HUD's own screen)
				State.bEnterFired = State.Fired.Num() == 1 && State.Fired[0] == FName(TEXT("Address"));
				APenguinHUD::CopyTextToClipboard(State.ClipboardSaved);
				Menu->SetTestPage(EPinguMenu3DPage::None);
				HUD->SetTestScreen(EPinguScreen::Controls);
				State.NextAt = T + 1.2f;
				State.Stage = 7;
				break;
			case 7: // a click on the first key cap starts the capture
			{
				const int32 Cap = FindButton(Menu, TEXT("Key0_0"));
				if (const UPinguSettings* Settings = UPinguSettings::Get())
				{
					State.OldKey = Settings->GetKey(EPinguInputAction::MoveLeft, 0);
				}
				if (Cap != INDEX_NONE)
				{
					FVector2D Pixel;
					if (Menu->GetButtonFacePixel(Cap, FVector2D::ZeroVector, Pixel))
					{
						SlateClick(World, Pixel);
					}
				}
				State.NextAt = T + 0.5f;
				State.Stage = 8;
				break;
			}
			case 8: // the overlay shows; a real key binds it
				State.bCaptureStarted = HUD->IsCapturingKey(EPinguInputAction::MoveLeft, 0);
				State.bOverlayShown = Menu->GetOverlay() == EPinguMenu3DPage::Capture;
				State.NewKey = EKeys::K;
				APenguinInputTest::SendKeyEvent(EKeys::K, true);
				APenguinInputTest::SendKeyEvent(EKeys::K, false);
				State.NextAt = T + 0.3f;
				State.Stage = 9;
				break;
			case 9:
			{
				UPinguSettings* Settings = UPinguSettings::Get();
				State.bBound = Settings && Settings->GetKey(EPinguInputAction::MoveLeft, 0) == EKeys::K;
				State.bCaptureEnded = !HUD->IsCapturingKey() && Menu->GetOverlay() == EPinguMenu3DPage::None;
				if (Settings && State.OldKey.IsValid())
				{
					Settings->SetKey(EPinguInputAction::MoveLeft, 0, State.OldKey);
				}
				HUD->SetTestScreen(EPinguScreen::None);
				State.Stage = 10;
				break;
			}
			default:
				break;
			}
		},
		[this]
		{
			FScreensRun& State = Run();
			APinguMenu3D* Menu = State.Menu.Get();
			if (!Menu)
			{
				Report(FString::Printf(TEXT("RESULT clicks screens: no 3D menu (enabled %d) FAIL"), PinguMenu3D::IsEnabled() ? 1 : 0));
				return;
			}
			Menu->SetTestPage(EPinguMenu3DPage::None);
			if (APenguinHUD* HUD = State.HUD.Get())
			{
				HUD->SetTestScreen(EPinguScreen::None);
			}
			SetRes(GetWorld(), 1280, 720);
			// (The bots as they were before the card lobby runs.)
			if (State.bSettingsSaved)
			{
				if (APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr)
				{
					GameMode->SetMatchSettings(State.SavedSettings);
				}
			}
			APenguinHUD::CopyTextToClipboard(State.ClipboardSaved);
			const bool bField = State.TypedShown == TEXT("192.168.1.5") && State.PastedShown == TEXT("192.168.1.510.0.0.7") && State.BackspacedShown == TEXT("192.168.1.510.0.0")
				&& State.bEnterFired;
			Report(FString::Printf(TEXT("RESULT clicks lan field: typed \"%s\" (want 192.168.1.5), after Ctrl+V \"%s\" (want 192.168.1.510.0.0.7), after two Backspaces \"%s\", Enter fired the field's action %d %s"),
				*State.TypedShown, *State.PastedShown, *State.BackspacedShown, State.bEnterFired ? 1 : 0, bField ? TEXT("ok") : TEXT("FAIL")));
			const bool bCapture = State.bCaptureStarted && State.bOverlayShown && State.bBound && State.bCaptureEnded;
			Report(FString::Printf(TEXT("RESULT clicks key capture: a click on the key cap started it %d, the PRESS A KEY overlay showed %d, a real K bound it %d, the capture ended %d (the old key put back) %s"),
				State.bCaptureStarted ? 1 : 0, State.bOverlayShown ? 1 : 0, State.bBound ? 1 : 0, State.bCaptureEnded ? 1 : 0, bCapture ? TEXT("ok") : TEXT("FAIL")));
			Report(FString::Printf(TEXT("RESULT clicks frame time: no menu %.1f ms, the results page up %.1f ms, the title %.1f ms at 1920x1080 (want about the same; a measurement)"), State.NoMenuMs, State.ResultsMs, State.TitleMs));
			const bool bOk = State.bAllOk && State.Runs == 2 * NumJobs && bField && bCapture;
			Report(FString::Printf(TEXT("RESULT clicks screens: %d of %d page runs (eight pages at two sizes) all hit %d, field %d, capture %d %s"), State.Runs, 2 * NumJobs, State.bAllOk ? 1 : 0,
				bField ? 1 : 0, bCapture ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}

// ============================================================================ pictures of every page

namespace PenguinAutoTestScreens3DPrivate
{
	struct FShotJob
	{
		EPinguScreen Screen;
		EPinguMenu3DPage Page;
		int32 SettingsTab;
		const TCHAR* Name;
		/** What to do before the picture: 1 hover SETTINGS (pause), 2 type an address (lan), 3 PINGUPEDIA Belly Stomp, 4 Jester,
		 *  5 the key capture, 6 hover a settings row. */
		int32 Prepare;
	};
	static const FShotJob Shots[] = {
		{ EPinguScreen::Pause, EPinguMenu3DPage::Pause, -1, TEXT("75_menu3d_pause_1080"), 1 },
		{ EPinguScreen::Lan, EPinguMenu3DPage::Lan, -1, TEXT("75_menu3d_lan_1080"), 2 },
		{ EPinguScreen::Results, EPinguMenu3DPage::Results, -1, TEXT("75_menu3d_results_1080"), 0 },
		{ EPinguScreen::Settings, EPinguMenu3DPage::Settings, 0, TEXT("75_menu3d_settings_1080"), 6 },
		{ EPinguScreen::Settings, EPinguMenu3DPage::Settings, 1, TEXT("75_menu3d_settings_display_1080"), 0 },
		{ EPinguScreen::Settings, EPinguMenu3DPage::Settings, 2, TEXT("75_menu3d_settings_audio_1080"), 0 },
		{ EPinguScreen::Controls, EPinguMenu3DPage::Controls, -1, TEXT("75_menu3d_controls_1080"), 0 },
		{ EPinguScreen::Controls, EPinguMenu3DPage::None, -1, TEXT("75_menu3d_controls_capture_1080"), 5 },
		{ EPinguScreen::Pingupedia, EPinguMenu3DPage::Pingupedia, -1, TEXT("75_menu3d_pingupedia_1080"), 3 },
		{ EPinguScreen::Pingupedia, EPinguMenu3DPage::Pingupedia, -1, TEXT("75_menu3d_pingupedia_traits_1080"), 4 },
		{ EPinguScreen::None, EPinguMenu3DPage::Board, -1, TEXT("75_menu3d_board_1080"), 0 },
	};
	constexpr int32 NumShots = static_cast<int32>(UE_ARRAY_COUNT(Shots));

	struct FLookRun
	{
		TWeakObjectPtr<APinguMenu3D> Menu;
		TWeakObjectPtr<APenguinHUD> HUD;
		int32 Stage = 0;
		int32 Shot = 0;
		float NextAt = 0.f;
		TArray<FString> Paths;
		FString PagesShown;
		int32 PagesOk = 0;
		FKey OldKey;
		/** The readability pictures (2026-10-26, third pass): five pages at 1280x720 on the LOW and the ULTRA preset, each with
		 *  the 3D letters (before) and with the Slate overlay's words (after); the graphics options to put back. */
		int32 Read = 0;
		bool bReadStarted = false;
		/** The fit check (2026-10-26, fourth pass): texts looked at, texts running past their box, the first few; the PINGUPEDIA
		 *  cards walked (page, entry). */
		int32 FitChecked = 0;
		int32 FitBad = 0;
		FString FitList;
		int32 PediaPage = 0;
		int32 PediaEntry = 0;
		int32 PediaCards = 0;
		FPinguGraphicsOptions OldGraphics;
	};
	static constexpr int32 ReadPages = 5;
	static constexpr int32 ReadShots = 4 * ReadPages;
	static FLookRun& Look()
	{
		static FLookRun State;
		return State;
	}

	/** A picture of the window (with the UI: the flat text on the panels is Slate, which HighResShot leaves out); the step runs
	 *  the window at 1920x1080 (r.SetRes). */
	static void BigShot(UWorld* World, const TCHAR* Name)
	{
		(void)World;
		const FString Path = ShotPath(Name);
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		Look().Paths.Add(Path);
	}
}

void APenguinAutoTest::AddMenu3DScreensLookStep()
{
	using namespace PenguinAutoTestScreens3DPrivate;
	AddStep(TEXT("look_Menu3DScreens"), 34.f + 1.6f * ReadShots + 3.f + 18.f,
		[this]
		{
			FLookRun& State = Look();
			State = FLookRun();
			APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
			APenguinHUD* HUD = Controller ? Cast<APenguinHUD>(Controller->GetHUD()) : nullptr;
			State.HUD = HUD;
			State.Menu = PinguMenu3D::IsEnabled() && HUD ? APinguMenu3D::Get(HUD) : nullptr;
			if (const UPinguSettings* Settings = UPinguSettings::Get())
			{
				State.OldKey = Settings->GetKey(EPinguInputAction::MoveLeft, 0);
				State.OldGraphics = Settings->GetGraphics();
			}
			SetRes(GetWorld(), 1920, 1080);
			State.NextAt = 1.2f;
		},
		[this](float T)
		{
			FLookRun& State = Look();
			APinguMenu3D* Menu = State.Menu.Get();
			APenguinHUD* HUD = State.HUD.Get();
			UWorld* World = GetWorld();
			if (!Menu || !HUD || T < State.NextAt)
			{
				return;
			}
			switch (State.Stage)
			{
			case 0: // show the page
			{
				HUD->CancelKeyCapture(); // (the capture's picture is written by now)
				if (State.Shot >= NumShots)
				{
					State.Stage = 10;
					break;
				}
				const FShotJob& Job = Shots[State.Shot];
				HUD->SetTestScreen(Job.Screen);
				Menu->SetTestPage(Job.Page);
				if (Job.SettingsTab >= 0)
				{
					Menu->SelectSettingsTab(Job.SettingsTab, true);
				}
				State.NextAt = T + 0.7f;
				State.Stage = 1;
				break;
			}
			case 1: // prepare it
			{
				const FShotJob& Job = Shots[State.Shot];
				if (Job.SettingsTab >= 0)
				{
					Menu->SelectSettingsTab(Job.SettingsTab, true);
				}
				switch (Job.Prepare)
				{
				case 1:
					Menu->FocusButton(TEXT("Settings"));
					break;
				case 2:
					Menu->FocusButton(TEXT("Address"));
					TypeString(TEXT("192.168.1.23"));
					break;
				case 3:
					Menu->SelectPingupediaPage(0, static_cast<int32>(EPenguinAbility::BellyStomp) - 1, true);
					break;
				case 4:
					Menu->SelectPingupediaPage(1, PinguNumTraits - 1, true);
					break;
				case 5:
					HUD->BeginKeyCapture(EPinguInputAction::MoveLeft, 0);
					break;
				case 6:
					Menu->FocusButton(TEXT("Shadows"));
					break;
				default:
					break;
				}
				State.NextAt = T + 0.5f;
				State.Stage = 2;
				break;
			}
			case 2: // the picture
			{
				const FShotJob& Job = Shots[State.Shot];
				const bool bShown = (Job.Page == EPinguMenu3DPage::None ? Menu->GetOverlay() == EPinguMenu3DPage::Capture : Menu->GetPage() == Job.Page)
					&& Menu->GetButtonCount() >= 0;
				State.PagesOk += bShown ? 1 : 0;
				State.PagesShown += FString::Printf(TEXT("%s%s%s"), State.PagesShown.IsEmpty() ? TEXT("") : TEXT(" "), APinguMenu3D::PageName(Job.Page == EPinguMenu3DPage::None ? Menu->GetOverlay() : Job.Page),
					bShown ? TEXT("") : TEXT("(missing)"));
				{
					int32 Checked = 0;
					FString List;
					const int32 Bad = Menu->CountTextMisfits(Checked, List);
					State.FitChecked += Checked;
					State.FitBad += Bad;
					if (Bad > 0 && State.FitList.Len() < 400)
					{
						State.FitList += FString::Printf(TEXT("[%s] %s"), Job.Name, *List);
					}
				}
				BigShot(World, Job.Name);
				++State.Shot;
				State.NextAt = T + 0.5f;
				State.Stage = 0;
				break;
			}
			case 10: // the readability pictures: a preset and the text's kind for the next five, then a page
			{
				if (State.Read >= ReadShots)
				{
					// Every PINGUPEDIA card (abilities, then traits) for the fit check.
					HUD->SetTestScreen(EPinguScreen::Pingupedia);
					Menu->SetTestPage(EPinguMenu3DPage::Pingupedia);
					State.NextAt = T + 0.8f;
					State.Stage = 13;
					break;
				}
				const bool bLow = State.Read < 2 * ReadPages;
				const bool bOverlay = (State.Read / ReadPages) % 2 == 1;
				if (!State.bReadStarted || State.Read % ReadPages == 0)
				{
					if (!State.bReadStarted)
					{
						SetRes(World, 1280, 720);
						State.bReadStarted = true;
					}
					if (State.Read % (2 * ReadPages) == 0)
					{
						if (UPinguSettings* Settings = UPinguSettings::Get())
						{
							Settings->ApplyPreset(bLow ? EPinguGraphicsPreset::Low : EPinguGraphicsPreset::Ultra);
							Settings->ApplyGraphics(World);
						}
					}
					PinguMenu3D::SetTextOverlay(bOverlay);
				}
				static const EPinguMenu3DPage Pages[ReadPages] = { EPinguMenu3DPage::Title, EPinguMenu3DPage::Lobby, EPinguMenu3DPage::Settings,
					EPinguMenu3DPage::Pingupedia, EPinguMenu3DPage::Results };
				static const EPinguScreen Screens[ReadPages] = { EPinguScreen::None, EPinguScreen::None, EPinguScreen::Settings, EPinguScreen::Pingupedia,
					EPinguScreen::Results };
				const int32 PageIndex = State.Read % ReadPages;
				HUD->SetTestScreen(Screens[PageIndex]);
				Menu->SetTestPage(Pages[PageIndex]);
				if (Pages[PageIndex] == EPinguMenu3DPage::Settings)
				{
					Menu->SelectSettingsTab(0, true);
				}
				State.NextAt = T + 0.9f;
				State.Stage = 11;
				break;
			}
			case 11:
			{
				const int32 PageIndex = State.Read % ReadPages;
				if (PageIndex == 0)
				{
					Menu->FocusButton(TEXT("Play"));
				}
				else if (PageIndex == 1)
				{
					Menu->FocusButton(TEXT("Start"));
				}
				else if (PageIndex == 2)
				{
					Menu->FocusButton(TEXT("Shadows"));
				}
				else if (PageIndex == 3)
				{
					Menu->SelectPingupediaPage(0, static_cast<int32>(EPenguinAbility::BellyStomp) - 1, true);
				}
				State.NextAt = T + 0.6f;
				State.Stage = 12;
				break;
			}
			case 13: // a card
			{
				Menu->SelectPingupediaPage(State.PediaPage, State.PediaEntry, true);
				int32 ShownPage = -1;
				int32 ShownEntry = -1;
				Menu->GetPingupediaView(ShownPage, ShownEntry);
				if (ShownPage != State.PediaPage || ShownEntry != State.PediaEntry)
				{
					// Past the page's last entry: the next page, or done.
					if (State.PediaPage == 0)
					{
						State.PediaPage = 1;
						State.PediaEntry = 0;
						break;
					}
					State.Stage = 15;
					break;
				}
				State.NextAt = T + 0.2f;
				State.Stage = 14;
				break;
			}
			case 14:
			{
				int32 Checked = 0;
				FString List;
				const int32 Bad = Menu->CountTextMisfits(Checked, List);
				State.FitChecked += Checked;
				State.FitBad += Bad;
				++State.PediaCards;
				if (State.PediaPage == 0 && State.PediaEntry == 13)
				{
					// The list scrolled half way (2026-10-27: Linus wanted it to scroll cleanly) - a picture of it (not counted).
					FScreenshotRequest::RequestScreenshot(ShotPath(TEXT("77_pingupedia_midscroll")), true, false);
				}
				if (Bad > 0 && State.FitList.Len() < 400)
				{
					State.FitList += FString::Printf(TEXT("[pingupedia %d/%d] %s"), State.PediaPage, State.PediaEntry, *List);
				}
				++State.PediaEntry;
				State.Stage = 13;
				break;
			}
			case 12:
			{
				static const TCHAR* Names[ReadPages] = { TEXT("title"), TEXT("lobby"), TEXT("settings"), TEXT("pingupedia"), TEXT("results") };
				const bool bLow = State.Read < 2 * ReadPages;
				const bool bOverlay = (State.Read / ReadPages) % 2 == 1;
				const FString Name = FString::Printf(TEXT("76_read_%s_%s_%s"), Names[State.Read % ReadPages], bLow ? TEXT("low") : TEXT("ultra"),
					bOverlay ? TEXT("overlay") : TEXT("3d"));
				BigShot(World, *Name);
				++State.Read;
				State.NextAt = T + 0.3f;
				State.Stage = 10;
				break;
			}
			default:
				break;
			}
		},
		[this]
		{
			FLookRun& State = Look();
			APinguMenu3D* Menu = State.Menu.Get();
			PinguMenu3D::SetTextOverlay(true);
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				if (State.bReadStarted)
				{
					Settings->SetGraphics(State.OldGraphics);
					Settings->ApplyGraphics(GetWorld());
				}
			}
			if (!Menu)
			{
				Report(FString::Printf(TEXT("RESULT menu 3d screens: no 3D menu (enabled %d) FAIL"), PinguMenu3D::IsEnabled() ? 1 : 0));
				return;
			}
			Menu->SetTestPage(EPinguMenu3DPage::None);
			if (APenguinHUD* HUD = State.HUD.Get())
			{
				HUD->CancelKeyCapture();
				HUD->SetTestScreen(EPinguScreen::None);
			}
			SetRes(GetWorld(), 1280, 720);
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				if (State.OldKey.IsValid())
				{
					Settings->SetKey(EPinguInputAction::MoveLeft, 0, State.OldKey);
				}
			}
			int32 Written = 0;
			for (const FString& Path : State.Paths)
			{
				if (IFileManager::Get().FileExists(*Path) || IFileManager::Get().FileExists(*(Path + TEXT(".png"))))
				{
					++Written;
				}
			}
			const bool bFit = State.FitChecked > 200 && State.FitBad == 0 && State.PediaCards >= 30;
			Report(FString::Printf(TEXT("RESULT menu 3d text fit: %d texts checked on %d pages and %d PINGUPEDIA cards, %d run past their sign / row / strip / board%s%s %s"),
				State.FitChecked, NumShots, State.PediaCards, State.FitBad, State.FitBad > 0 ? TEXT(": ") : TEXT(""), *State.FitList, bFit ? TEXT("ok") : TEXT("FAIL")));
			const bool bOk = State.PagesOk == NumShots && Written == NumShots + ReadShots;
			Report(FString::Printf(TEXT("RESULT menu 3d screens: %d of %d pages shown (%s), %d of %d pictures written (%d at 1920x1080, %d readability pictures at 1280x720: LOW / ULTRA, 3D letters / overlay) %s"),
				State.PagesOk, NumShots, *State.PagesShown, Written, NumShots + ReadShots, NumShots, ReadShots, bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}
