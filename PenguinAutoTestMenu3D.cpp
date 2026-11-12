// Pingu - the 3D menus' tests (PinguMenu3D).
//
// look_Menu3D (autotest / looktest, APenguinAutoTest::AddMenu3DLookStep): the 3D title shown by hand (APinguMenu3D::SetTestPage,
// its buttons report their id instead of doing their action):
//   - the pointer over SETTINGS (traced into the 3D buttons from a point on the screen, as the mouse is): SETTINGS is hot, its
//     outline glows, every other button's outline is gone;
//   - the keyboard focus from PLAY down through all five buttons in order (Navigate Down, as the arrow keys / d-pad do), and
//     Down at the last one stays there;
//   - a click on ONLINE: the button pushes in (it sinks toward the plane) and its action fires once, after the push;
//   - the lobby's controls (solo: SETTINGS, BACK, TRAITS, START);
//   pictures 73_menu3d_title / _hover / _focus / _lobby_controls at the game's 1280x720, and the same four at 1920x1080 (a high
//   resolution screenshot: the menu is geometry, the letters must stay crisp) as 73_menu3d_*_1080.
// APinguLobbyTest::UpdateMenu3DTitlePlay / UpdateMenu3DLobbyCheck (lobbytest): PLAY clicked on the real 3D title opens the lobby;
// in the lobby the 3D TRAITS button toggles the traits and back, SETTINGS opens the settings (BACK returns); one RESULT line.

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PinguMenu3D.h"
#include "PinguMenu2D.h"
#include "PinguLobby.h"
#include "PinguSettings.h"
#include "PenguinCamera.h"
#include "PenguinGameMode.h"
#include "PenguinCharacter.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HighResScreenshot.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"

/** PenguinAutoTestSettingsApply.cpp: the settings page's APPLY / board flow (true while it runs). */
bool PinguSettingsApplyFlow(APenguinHUD* HUD, APinguMenu3D* Menu, float Now, TFunctionRef<void(bool, const FString&)> Check);

namespace PenguinAutoTestMenu3DPrivate
{
	struct FMenu3DRun
	{
		TWeakObjectPtr<APinguMenu3D> Menu;
		TArray<FName> Fired;
		int32 Stage = 0;
		/** The 2D menus (2026-10-27): the menu actor's render primitives (want 0), its 2D parts, the parts painted on the title. */
		int32 Primitives = -1;
		int32 FlatParts = 0;
		int32 Painted = 0;
		float NextAt = 0.f;
		// hover
		bool bHoverHot = false;
		float HoverOutline = 0.f;
		float OthersOutline = 0.f;
		/** The wood menus (2026-10-26): the hovered sign tilts (degrees) and has no rope; the keyboard-focused one has the rope
		 *  and the yellow arrow. */
		float HoverTilt = 0.f;
		/** The overlay's words (2026-10-26, third pass): how many buttons' labels sit on their own sign (the middle of the words
		 *  inside the sign's face on the screen), of how many looked at (the title and the lobby strip). */
		int32 TextsOnSigns = 0;
		int32 TextsChecked = 0;
		float FocusOutline = 0.f;
		bool bFocusArrow = false;
		/** 2026-11-07 (Linus: the keyboard focus looks exactly like the hover): the focused sign tilts like a hovered one. */
		float FocusTilt = 0.f;
		// focus order
		TArray<FName> Order;
		TArray<FName> Visited;
		FName AfterLastDown;
		// click
		float PopBefore = 0.f;
		float PopPushed = 100.f;
		float PushMax = 0.f;
		int32 FiredBefore = 0;
		float ClickAt = -1.f;
		float FiredAt = -1.f;
		// lobby
		TArray<FName> LobbyShown;
		int32 Buttons = 0;
		FString Notes;
		/** After a picture nothing changes for a moment (screenshots are written a frame or two after the request). */
		float HoldUntil = 0.f;
		/** High resolution pictures waiting to be checked: their file names. */
		TArray<FString> BigShots;
	};
	static FMenu3DRun& Run()
	{
		static FMenu3DRun State;
		return State;
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

	/** The labels of the page's shown buttons: the middle of the overlay's words inside the sign's face on the screen. */
	static void CheckTexts(APinguMenu3D* Menu, FMenu3DRun& State)
	{
		FVector2D View(0.f, 0.f);
		const APlayerController* Controller = Menu && Menu->GetWorld() ? Menu->GetWorld()->GetFirstPlayerController() : nullptr;
		if (!Menu || !PinguMenu3D::GetViewportPixelSize(Controller, View))
		{
			return;
		}
		for (int32 Index = 0; Index < Menu->GetButtonCount(); ++Index)
		{
			const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
			FVector2D Words;
			if (!Info.bShown || Info.Label.IsEmpty() || !Menu->GetButtonTextPixel(Index, Words))
			{
				continue;
			}
			++State.TextsChecked;
			const FVector2D Face = Info.ScreenCenter * View;
			const FVector2D Half = 0.5f * Info.ScreenSize * View;
			if (FMath::Abs(Words.X - Face.X) <= Half.X && FMath::Abs(Words.Y - Face.Y) <= Half.Y)
			{
				++State.TextsOnSigns;
			}
			else
			{
				UE_LOG(LogPingu, Log, TEXT("[AUTOTEST] menu 3d: the words of %s at %s, its sign at %s +- %s"), *Info.Id.ToString(), *Words.ToString(), *Face.ToString(), *Half.ToString());
			}
		}
	}

	static FString ShotPath(const TCHAR* Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ClaudeLogs/Shots") / FString(Name) + TEXT(".png"));
	}

	/** The window's picture with the UI (2026-10-27: the menu's words are drawn by the Slate text overlay, which the engine's
	 *  HighResShot left out - the signs showed no text). The name keeps its old _1080. */
	static void BigShot(UWorld* World, const TCHAR* Name)
	{
		(void)World;
		const FString Path = ShotPath(Name);
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		Run().BigShots.Add(Path);
	}
}

// ============================================================================ the lobby's tags at every size, the camera distance

namespace PenguinAutoTestMenu3DPrivate
{
	/** look_LobbyTags2D: the sizes and the UI scales (Windows display scaling on Linus's laptop: 125 / 150 %). */
	struct FTagSize { int32 Width; int32 Height; float Scale; };
	static const FTagSize TagSizes[] = {
		{ 1280, 720, 1.f }, { 1920, 1080, 1.f }, { 2560, 1440, 1.f }, { 2000, 1232, 1.f }, { 1920, 1080, 1.25f }, { 1920, 1080, 1.5f },
		{ 2560, 1440, 1.5f }, { 2000, 1232, 1.25f },
	};
	constexpr int32 NumTagSizes = UE_ARRAY_COUNT(TagSizes);
	constexpr float TagSizeTime = 1.6f;
	struct FTagsRun
	{
		TArray<TWeakObjectPtr<APenguinCharacter>> Hidden;
		int32 View = -1;
		float ViewAt = 0.f;
		bool bMeasured = false;
		bool bShot = false;
		TArray<FString> Lines;
		int32 Checked = 0;
		float WorstPaint = 0.f;
		float WorstWord = 0.f;
		/** The worst word's distance from its pill's middle against the pill's half width. */
		float WorstShare = 0.f;
		int32 WordsFound = 0;
		int32 Views = 0;
	};
	static FTagsRun& TagsRun()
	{
		static FTagsRun Value;
		return Value;
	}
	struct FCameraRun
	{
		float Distance[3] = { 0.f, 0.f, 0.f };
		float Scale[3] = { 0.f, 0.f, 0.f };
		int32 Stage = -1;
		float Before = 1.f;
	};
	static FCameraRun& CameraRun()
	{
		static FCameraRun Value;
		return Value;
	}
}

void APenguinAutoTest::AddMenu3DLookStep()
{
	using namespace PenguinAutoTestMenu3DPrivate;
	AddStep(TEXT("look_Menu3D"), 10.f,
		[this]
		{
			FMenu3DRun& State = Run();
			State = FMenu3DRun();
			APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
			APenguinHUD* HUD = Controller ? Cast<APenguinHUD>(Controller->GetHUD()) : nullptr;
			APinguMenu3D* Menu = PinguMenu3D::IsEnabled() && HUD ? APinguMenu3D::Get(HUD) : nullptr;
			State.Menu = Menu;
			if (Menu)
			{
				Menu->SetTestPage(EPinguMenu3DPage::Title, [](FName Id) { Run().Fired.Add(Id); });
			}
		},
		[this](float T)
		{
			FMenu3DRun& State = Run();
			APinguMenu3D* Menu = State.Menu.Get();
			if (!Menu)
			{
				return;
			}
			const int32 Count = Menu->GetButtonCount();
			if (T < State.HoldUntil)
			{
				return;
			}
			auto Hold = [&State, T]() { State.HoldUntil = T + 0.2f; };
			switch (State.Stage)
			{
			case 0: // the title has come in
				if (T > 0.6f)
				{
					CheckTexts(Menu, State);
					Menu->CountParts(State.Primitives, State.FlatParts);
					State.Painted = PinguMenu2D::GetPaintedParts();
					State.Order = Menu->GetFocusOrder();
					State.Buttons = Count;
					Shot(TEXT("73_menu3d_title"));
					State.Stage = 1;
					State.NextAt = T + 0.15f;
				}
				break;
			case 1: // the pointer over SETTINGS
				if (T > State.NextAt)
				{
					const int32 Settings = FindButton(Menu, TEXT("Settings"));
					if (Settings != INDEX_NONE)
					{
						Menu->PointerMove(Menu->GetButtonInfo(Settings).ScreenCenter);
					}
					State.Stage = 2;
					State.NextAt = T + 0.45f;
				}
				break;
			case 2: // its outline glows, the others' are gone
				if (T > State.NextAt)
				{
					for (int32 Index = 0; Index < Count; ++Index)
					{
						const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
						if (Info.Id == FName(TEXT("Settings")))
						{
							State.bHoverHot = Info.bHot;
							State.HoverOutline = Info.Outline;
							State.HoverTilt = Info.Tilt;
						}
						else
						{
							State.OthersOutline = FMath::Max(State.OthersOutline, Info.Outline);
						}
					}
					Shot(TEXT("73_menu3d_hover"));
					Hold();
					State.Stage = 21;
				}
				break;
			case 21:
				{
					Menu->PointerLeave();
					Menu->FocusButton(TEXT("Play"));
					State.Visited.Add(Menu->GetHotId());
					State.Stage = 3;
					State.NextAt = T + 0.2f;
				}
				break;
			case 3: // the keys: Down through all five
				if (T > State.NextAt)
				{
					Menu->Navigate(EUINavigation::Down);
					const FName Hot = Menu->GetHotId();
					if (State.Visited.Num() < 5)
					{
						State.Visited.Add(Hot);
						State.NextAt = T + 0.12f;
					}
					else
					{
						State.AfterLastDown = Hot;
						Menu->Navigate(EUINavigation::Up); // to PINGUPEDIA for the picture
						State.Stage = 4;
						State.NextAt = T + 0.4f;
					}
				}
				break;
			case 4:
				if (T > State.NextAt)
				{
					const int32 Pedia = FindButton(Menu, TEXT("Pingupedia"));
					if (Pedia != INDEX_NONE)
					{
						const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Pedia);
						State.FocusOutline = Info.bHot ? Info.Outline : 0.f;
						State.bFocusArrow = Info.bFocusArrow;
						State.FocusTilt = Info.bHot ? Info.Tilt : 0.f;
					}
					Shot(TEXT("73_menu3d_focus"));
					Hold();
					State.Stage = 41;
				}
				break;
			case 41:
				{
					// The click on ONLINE: where its face is now, and how far it stands out before.
					const int32 Online = FindButton(Menu, TEXT("Online"));
					if (Online != INDEX_NONE)
					{
						Menu->FocusButton(TEXT("Online"));
						State.FiredBefore = Menu->GetActionsFired();
						State.PopBefore = Menu->GetButtonInfo(Online).Pop;
						State.Stage = 5;
						State.NextAt = T + 0.35f;
					}
					else
					{
						State.Stage = 6;
					}
				}
				break;
			case 5:
			{
				const int32 Online = FindButton(Menu, TEXT("Online"));
				if (State.ClickAt < 0.f && T > State.NextAt)
				{
					State.PopBefore = Menu->GetButtonInfo(Online).Pop;
					Menu->PointerDown(Menu->GetButtonInfo(Online).ScreenCenter);
					State.ClickAt = T;
				}
				else if (State.ClickAt >= 0.f)
				{
					const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Online);
					State.PopPushed = FMath::Min(State.PopPushed, Info.Pop);
					State.PushMax = FMath::Max(State.PushMax, Info.Push);
					if (State.FiredAt < 0.f && State.Fired.Num() > 0)
					{
						State.FiredAt = T - State.ClickAt;
					}
					if (T > State.ClickAt + 0.4f)
					{
						State.Stage = 6;
					}
				}
				break;
			}
			case 6: // the lobby's controls
				Menu->SetTestPage(EPinguMenu3DPage::Lobby, [](FName Id) { Run().Fired.Add(Id); });
				State.Stage = 7;
				State.NextAt = T + 0.6f;
				break;
			case 7:
				if (T > State.NextAt)
				{
					for (int32 Index = 0; Index < Count; ++Index)
					{
						const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
						const FString IdText = Info.Id.ToString();
						// (The card lobby's cards are buttons too: the signs only.)
						if (Info.bShown && !IdText.StartsWith(TEXT("Card_")) && !IdText.StartsWith(TEXT("Skill_")) && !IdText.StartsWith(TEXT("Remove_")) && IdText != TEXT("AddBot")
							&& (!IdText.StartsWith(TEXT("Pick")) || IdText == TEXT("PicksMode"))) // (nor the ability picks' PickSlot_* / PickCell_*; its ABILITIES toggle PicksMode is a sign)
						{
							State.LobbyShown.Add(Info.Id);
						}
					}
					CheckTexts(Menu, State);
					Menu->FocusButton(TEXT("Start"));
					State.Stage = 8;
					State.NextAt = T + 0.35f;
				}
				break;
			case 8:
				if (T > State.NextAt)
				{
					Shot(TEXT("73_menu3d_lobby_controls"));
					Hold();
					State.Stage = 81;
				}
				break;
			case 81:
				{
					// The same four at 1920x1080.
					Menu->SetTestPage(EPinguMenu3DPage::Title, [](FName Id) { Run().Fired.Add(Id); });
					State.Stage = 9;
					State.NextAt = T + 0.6f;
				}
				break;
			case 9:
				if (T > State.NextAt)
				{
					BigShot(GetWorld(), TEXT("73_menu3d_title_1080"));
					Hold();
					State.Stage = 91;
				}
				break;
			case 91:
				{
					const int32 Settings = FindButton(Menu, TEXT("Settings"));
					Menu->PointerMove(Menu->GetButtonInfo(Settings).ScreenCenter);
					State.Stage = 10;
					State.NextAt = T + 0.45f;
				}
				break;
			case 10:
				if (T > State.NextAt)
				{
					BigShot(GetWorld(), TEXT("73_menu3d_hover_1080"));
					Hold();
					State.Stage = 101;
				}
				break;
			case 101:
				{
					Menu->PointerLeave();
					Menu->FocusButton(TEXT("Pingupedia"));
					State.Stage = 11;
					State.NextAt = T + 0.45f;
				}
				break;
			case 11:
				if (T > State.NextAt)
				{
					BigShot(GetWorld(), TEXT("73_menu3d_focus_1080"));
					Hold();
					State.Stage = 111;
				}
				break;
			case 111:
				{
					Menu->SetTestPage(EPinguMenu3DPage::Lobby, [](FName Id) { Run().Fired.Add(Id); });
					State.Stage = 12;
					State.NextAt = T + 0.6f;
				}
				break;
			case 12:
				if (T > State.NextAt)
				{
					Menu->FocusButton(TEXT("Start"));
					State.Stage = 13;
					State.NextAt = T + 0.35f;
				}
				break;
			case 13:
				if (T > State.NextAt)
				{
					BigShot(GetWorld(), TEXT("73_menu3d_lobby_controls_1080"));
					State.Stage = 14;
				}
				break;
			default:
				break;
			}
		},
		[this]
		{
			FMenu3DRun& State = Run();
			APinguMenu3D* Menu = State.Menu.Get();
			if (!Menu)
			{
				Report(FString::Printf(TEXT("RESULT menu 3d: no 3D menu (enabled %d) FAIL"), PinguMenu3D::IsEnabled() ? 1 : 0));
				return;
			}
			Menu->SetTestPage(EPinguMenu3DPage::None);
			static const FName WantOrder[] = { TEXT("Play"), TEXT("Online"), TEXT("Settings"), TEXT("Pingupedia"), TEXT("Quit") };
			bool bOrder = State.Visited.Num() == 5 && State.Order.Num() == 5;
			FString Visited;
			for (int32 Index = 0; Index < State.Visited.Num(); ++Index)
			{
				Visited += (Index ? TEXT(" > ") : TEXT("")) + State.Visited[Index].ToString();
				bOrder = bOrder && State.Visited[Index] == WantOrder[FMath::Min(Index, 4)] && State.Order[FMath::Min(Index, State.Order.Num() - 1)] == WantOrder[FMath::Min(Index, 4)];
			}
			const bool bLastStays = State.AfterLastDown == FName(TEXT("Quit"));
			// Hover: the sign tilts, no rope on it or any other; keys (2026-11-07, card wood-ui): the same look as the hover - it
			// tilts, no rope, no arrow.
			const bool bHover = State.bHoverHot && State.HoverTilt > 2.f && State.HoverOutline < 0.01f && State.OthersOutline < 0.01f;
			const bool bFocus = State.FocusOutline < 0.01f && !State.bFocusArrow && State.FocusTilt > 2.f;
			const int32 FiredOnline = State.Fired.Num() > 0 && State.Fired[0] == FName(TEXT("Online")) ? 1 : 0;
			const bool bClick = FiredOnline == 1 && State.Fired.Num() == 1 && State.PopPushed < State.PopBefore - 3.f && State.PushMax > 0.5f
				&& State.FiredAt > 0.05f && State.FiredAt < 0.3f;
			// (+ TraitList, the host's TRAITS list sign, and PicksMode, the ABILITIES toggle: both 2026-11-07)
			TArray<FName> WantLobby = { TEXT("Settings"), TEXT("Back"), TEXT("Start"), TEXT("Traits"), TEXT("TraitList"), TEXT("PicksMode") };
			bool bLobby = State.LobbyShown.Num() == WantLobby.Num();
			FString Lobby;
			for (const FName& Id : State.LobbyShown)
			{
				Lobby += (Lobby.IsEmpty() ? TEXT("") : TEXT(" ")) + Id.ToString();
				bLobby = bLobby && WantLobby.Contains(Id);
			}
			// The high resolution pictures: written (with or without an extra .png the engine may add).
			int32 Big = 0;
			for (const FString& Path : State.BigShots)
			{
				if (IFileManager::Get().FileExists(*Path) || IFileManager::Get().FileExists(*(Path + TEXT(".png"))))
				{
					++Big;
				}
			}
			const bool bTexts = State.TextsChecked >= 9 && State.TextsOnSigns == State.TextsChecked;
			const bool bOk = State.Buttons == 5 && bOrder && bLastStays && bHover && bFocus && bClick && bLobby && bTexts;
			Report(FString::Printf(TEXT("RESULT menu 3d: title %d buttons; hover on SETTINGS: hot %d, tilted %.1f deg (want > 2), its rope %.2f, the others' at most %.2f (want 0); keys on PINGUPEDIA (like the hover): tilted %.1f deg (want > 2), rope %.2f, the yellow arrow %d (want 0, 0); keys from PLAY down: %s (want PLAY > ONLINE > SETTINGS > PINGUPEDIA > QUIT), Down at QUIT stays %d; click on ONLINE: stood out %.1f, pushed to %.1f (push %.2f), its action fired %d time(s) %.2f s after the click (want 1, about 0.09 s); lobby controls shown: %s; the overlay's words on their own sign %d of %d (want all); %d of 4 pictures with the UI (the _1080 ones) written %s"),
				State.Buttons, State.bHoverHot ? 1 : 0, State.HoverTilt, State.HoverOutline, State.OthersOutline, State.FocusTilt, State.FocusOutline, State.bFocusArrow ? 1 : 0, *Visited, bLastStays ? 1 : 0,
				State.PopBefore, State.PopPushed, State.PushMax, State.Fired.Num(), State.FiredAt, *Lobby, State.TextsOnSigns, State.TextsChecked, Big, bOk ? TEXT("ok") : TEXT("FAIL")));
			// The 2D menus (2026-10-27, card 2026-10-27_gui-2d: "not 3d so its wayy less performance impact"): no render part at all
			// on the menu actor (no meshes, lights, materials, text glyphs), the title painted from its 2D parts. With the 3D menus
			// (PINGU_3D_MENUS=1) it says how many render parts the 3D title had.
			if (PinguMenu3D::IsFlat())
			{
				const bool bFlatOk = State.Primitives == 0 && State.FlatParts > 50 && State.Painted > 20;
				Report(FString::Printf(TEXT("RESULT menu 2d: the menu actor has %d render primitives (want 0) and %d 2D parts, %d painted on the title (want > 20) %s"),
					State.Primitives, State.FlatParts, State.Painted, bFlatOk ? TEXT("ok") : TEXT("FAIL")));
			}
			else
			{
				Report(FString::Printf(TEXT("RESULT menu 2d: the 3D menus are on (PINGU_3D_MENUS / -Pingu3DMenus): the menu actor has %d render primitives ok"), State.Primitives));
			}
		});

	// (2026-10-29, card 2026-10-29_lobby-gui-fix - Linus: "the names of the nametag dont line up in the lobby and the x button also
	// doesnt really ling up". His game ran in a 2000x1232 window: the painter's own projection kept the horizontal field of view,
	// the engine keeps the vertical one of a 16:9 picture - every tag part drifted sideways from its word, more the further from
	// the middle. Now the painter uses the engine's own projection for the world.) The lobby with four bots at eight sizes / UI
	// scales: every name pill, difficulty pill and x painted where the engine projects it (within 1.5 px), every name word on its
	// own pill (its middle within 0.15 of the pill's half width); a picture per view (78_lobby_tags_<w>x<h>_<scale>).
	if (PinguMenu3D::UseIslandLobby())
	{
	AddStep(TEXT("look_LobbyTags2D"), 2.f + NumTagSizes * TagSizeTime + 0.5f,
		[this]
		{
			FTagsRun& R = TagsRun();
			R = FTagsRun();
			APenguinGameMode* GameMode = GetWorld()->GetAuthGameMode<APenguinGameMode>();
			if (!GameMode)
			{
				return;
			}
			for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
			{
				if (*It != Penguin && !GameMode->GetPenguins().Contains(*It))
				{
					It->SetActorHiddenInGame(true);
					It->SetActorEnableCollision(false);
					It->SetActorLocation(FVector(0.f, 3000.f, -5000.f));
					R.Hidden.Add(*It);
				}
			}
			ClearAbilityLeftovers();
			FPinguMatchSettings Settings = GameMode->GetSettings();
			Settings.Bots.SetNum(4);
			for (int32 Index = 0; Index < Settings.Bots.Num(); ++Index)
			{
				Settings.Bots[Index].Team = -1;
				Settings.Bots[Index].Skill = Index % 3;
			}
			Settings.NumBots = 4;
			GameMode->SetMatchSettings(Settings);
			Penguin->SetTeam(-1);
			GameMode->EnterLobby();
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets(0.5f);
			}
			VirtualInput = FVector2D::ZeroVector;
		},
		[this](float T)
		{
			FTagsRun& R = TagsRun();
			UWorld* World = GetWorld();
			APlayerController* Player = World->GetFirstPlayerController();
			APenguinHUD* HUD = Player ? Cast<APenguinHUD>(Player->GetHUD()) : nullptr;
			APinguLobby* Lobby = APinguLobby::Get(World);
			APinguMenu3D* Menu = HUD && PinguMenu3D::IsEnabled() ? APinguMenu3D::Get(HUD) : nullptr;
			if (!Lobby || !Menu || !Player || T < 2.f)
			{
				return;
			}
			const int32 View = FMath::Min(static_cast<int32>((T - 2.f) / TagSizeTime), NumTagSizes);
			if (View != R.View)
			{
				R.View = View;
				R.ViewAt = T;
				R.bMeasured = false;
				R.bShot = false;
				if (View < NumTagSizes)
				{
					GEngine->Exec(World, *FString::Printf(TEXT("r.SetRes %dx%dw"), TagSizes[View].Width, TagSizes[View].Height));
					if (FSlateApplication::IsInitialized())
					{
						FSlateApplication::Get().SetApplicationScale(TagSizes[View].Scale);
					}
				}
				return;
			}
			if (View >= NumTagSizes)
			{
				return;
			}
			if (!R.bMeasured && T > R.ViewAt + 0.9f)
			{
				R.bMeasured = true;
				++R.Views;
				float Paint = 0.f;
				float Word = 0.f;
				int32 Checked = 0;
				TArray<FPinguMenu3DTextDraw> Texts;
				Menu->GetOverlayTexts(Texts);
				for (const APinguLobby::FTarget& Target : Lobby->GetClickTargets())
				{
					if (Target.Kind != APinguLobby::EHit::Name && Target.Kind != APinguLobby::EHit::Skill && Target.Kind != APinguLobby::EHit::Remove)
					{
						continue;
					}
					FVector2D Engine;
					FVector2D Painted;
					if (!Player->ProjectWorldLocationToScreen(Target.Centre, Engine, false) || !PinguMenu2D::ProjectWorldForTest(Menu, Target.Centre, Painted))
					{
						continue;
					}
					++Checked;
					Paint = FMath::Max(Paint, static_cast<float>((Engine - Painted).Size()));
					if (Target.Kind != APinguLobby::EHit::Name || !Target.Penguin.IsValid())
					{
						continue;
					}
					// Its word: the overlay text with the tag's name.
					const FString Want = Lobby->GetNameTagText(Target.Penguin.Get());
					float Best = 1.e9f;
					for (const FPinguMenu3DTextDraw& Draw : Texts)
					{
						if (Draw.Text != Want)
						{
							continue;
						}
						const FVector2D Middle = 0.5 * (Draw.TopLeft + Draw.TopRight) + 0.5 * (Draw.BottomLeft - Draw.TopLeft);
						Best = FMath::Min(Best, static_cast<float>(FMath::Abs(Middle.X - Engine.X)));
					}
					// (Against the pill's own half width on the screen: the words are centred on their glyphs' ink, the pill on
					// their advance - a few pixels at 1440p, never off the pill.)
					FVector2D Edge;
					const float HalfPx = Player->ProjectWorldLocationToScreen(Target.Centre + FVector(Target.Half.X, 0.0, 0.0), Edge, false)
						? FMath::Max(static_cast<float>(FMath::Abs(Edge.X - Engine.X)), 1.f) : 1.f;
					if (Best < 1.e8f)
					{
						++R.WordsFound;
						Word = FMath::Max(Word, Best);
						R.WorstShare = FMath::Max(R.WorstShare, Best / HalfPx);
					}
				}
				FVector2D Size;
				PinguMenu3D::GetViewportPixelSize(Player, Size);
				R.Checked += Checked;
				R.WorstPaint = FMath::Max(R.WorstPaint, Paint);
				R.WorstWord = FMath::Max(R.WorstWord, Word);
				R.Lines.Add(FString::Printf(TEXT("%dx%d at %.0f%% (got %.0fx%.0f): %d parts, painter off by %.1f px, words off by %.1f px"),
					TagSizes[View].Width, TagSizes[View].Height, 100.f * TagSizes[View].Scale, Size.X, Size.Y, Checked, Paint, Word));
			}
			if (!R.bShot && T > R.ViewAt + 1.0f)
			{
				R.bShot = true;
				Shot(*FString::Printf(TEXT("78_lobby_tags_%dx%d_%.0f"), TagSizes[View].Width, TagSizes[View].Height, 100.f * TagSizes[View].Scale), true);
			}
		},
		[this]
		{
			FTagsRun& R = TagsRun();
			UWorld* World = GetWorld();
			GEngine->Exec(World, TEXT("r.SetRes 1280x720w"));
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().SetApplicationScale(1.f);
			}
			const bool bOk = R.Views == NumTagSizes && R.Checked >= NumTagSizes * 6 && R.WorstPaint < 1.5f && R.WordsFound >= NumTagSizes * 3 && R.WorstShare < 0.15f;
			Report(FString::Printf(TEXT("RESULT lobby tags 2d: %d of %d views, %d tag parts, the painter at most %.1f px off the engine's place (want < 1.5), %d name words, at most %.1f px off their pill's middle = %.2f of its half width (want < 0.15); %s %s"),
				R.Views, NumTagSizes, R.Checked, R.WorstPaint, R.WordsFound, R.WorstWord, R.WorstShare, *FString::Join(R.Lines, TEXT("; ")), bOk ? TEXT("ok") : TEXT("FAIL")));
			if (APenguinGameMode* GameMode = World->GetAuthGameMode<APenguinGameMode>())
			{
				GameMode->StartPractice();
			}
			for (const TWeakObjectPtr<APenguinCharacter>& Each : R.Hidden)
			{
				if (Each.IsValid())
				{
					Each->SetActorHiddenInGame(false);
					Each->SetActorEnableCollision(true);
				}
			}
		});

	}
	else
	{
	// (2026-10-29, card 2026-10-29_wood-card-lobby: the wooden card lobby instead of the island - the card-layout test in the tag
	// test's place.) The lobby with four bots at the same eight sizes / UI scales: the panel and every button on the screen, no two
	// cards over each other, every bot's x and difficulty inside its own card, every word inside its card / pill / sign (the text-fit
	// check); a picture per view (78_card_lobby_<w>x<h>_<scale>).
	AddStep(TEXT("look_LobbyCards2D"), 2.f + NumTagSizes * TagSizeTime + 0.5f,
		[this]
		{
			FTagsRun& R = TagsRun();
			R = FTagsRun();
			APenguinGameMode* GameMode = GetWorld()->GetAuthGameMode<APenguinGameMode>();
			if (!GameMode)
			{
				return;
			}
			ClearAbilityLeftovers();
			FPinguMatchSettings Settings = GameMode->GetSettings();
			Settings.Bots.SetNum(4);
			for (int32 Index = 0; Index < Settings.Bots.Num(); ++Index)
			{
				Settings.Bots[Index].Team = Index % 2 == 0 ? Index / 2 : -1;
				Settings.Bots[Index].Skill = Index % 3;
			}
			Settings.NumBots = 4;
			GameMode->SetMatchSettings(Settings);
			GameMode->EnterLobby();
			VirtualInput = FVector2D::ZeroVector;
		},
		[this](float T)
		{
			FTagsRun& R = TagsRun();
			UWorld* World = GetWorld();
			APlayerController* Player = World->GetFirstPlayerController();
			APenguinHUD* HUD = Player ? Cast<APenguinHUD>(Player->GetHUD()) : nullptr;
			APinguMenu3D* Menu = HUD && PinguMenu3D::IsEnabled() ? APinguMenu3D::Get(HUD) : nullptr;
			if (!Menu || !Player || T < 2.f)
			{
				return;
			}
			const int32 View = FMath::Min(static_cast<int32>((T - 2.f) / TagSizeTime), NumTagSizes);
			if (View != R.View)
			{
				R.View = View;
				R.ViewAt = T;
				R.bMeasured = false;
				R.bShot = false;
				if (View < NumTagSizes)
				{
					GEngine->Exec(World, *FString::Printf(TEXT("r.SetRes %dx%dw"), TagSizes[View].Width, TagSizes[View].Height));
					if (FSlateApplication::IsInitialized())
					{
						FSlateApplication::Get().SetApplicationScale(TagSizes[View].Scale);
					}
				}
				return;
			}
			if (View >= NumTagSizes)
			{
				return;
			}
			if (!R.bMeasured && T > R.ViewAt + 0.9f && Menu->GetPage() == EPinguMenu3DPage::Lobby)
			{
				R.bMeasured = true;
				++R.Views;
				// The buttons' boxes on the screen (0..1 of the viewport).
				struct FBox01 { FString Id; FVector2D Min; FVector2D Max; };
				TArray<FBox01> Boxes;
				int32 Outside = 0;
				for (int32 Index = 0; Index < Menu->GetButtonCount(); ++Index)
				{
					const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
					if (!Info.bShown)
					{
						continue;
					}
					FBox01& Box = Boxes.AddDefaulted_GetRef();
					Box.Id = Info.Id.ToString();
					Box.Min = Info.ScreenCenter - 0.5 * Info.ScreenSize;
					Box.Max = Info.ScreenCenter + 0.5 * Info.ScreenSize;
					Outside += Box.Min.X < 0.0 || Box.Min.Y < 0.0 || Box.Max.X > 1.0 || Box.Max.Y > 1.0 ? 1 : 0;
				}
				int32 Overlaps = 0;
				int32 Stray = 0;
				for (const FBox01& A : Boxes)
				{
					const bool bCardA = A.Id.StartsWith(TEXT("Card_")) || A.Id == TEXT("AddBot");
					for (const FBox01& B : Boxes)
					{
						const bool bCardB = B.Id.StartsWith(TEXT("Card_")) || B.Id == TEXT("AddBot");
						if (&A < &B && bCardA && bCardB && A.Min.X < B.Max.X && B.Min.X < A.Max.X && A.Min.Y < B.Max.Y && B.Min.Y < A.Max.Y)
						{
							++Overlaps;
						}
					}
					// A bot's x / difficulty: inside its own card.
					FString Name;
					if (A.Id.Split(TEXT("Remove_"), nullptr, &Name) || A.Id.Split(TEXT("Skill_"), nullptr, &Name))
					{
						const FBox01* Card = Boxes.FindByPredicate([&Name](const FBox01& Other) { return Other.Id == TEXT("Card_") + Name; });
						Stray += !Card || A.Min.X < Card->Min.X || A.Max.X > Card->Max.X || A.Min.Y < Card->Min.Y || A.Max.Y > Card->Max.Y ? 1 : 0;
					}
				}
				FVector2D PanelCenter;
				FVector2D PanelSize;
				const bool bPanel = Menu->GetPlaneScreenRect(PanelCenter, PanelSize);
				const bool bPanelIn = bPanel && PanelCenter.X - 0.5 * PanelSize.X >= 0.0 && PanelCenter.X + 0.5 * PanelSize.X <= 1.0
					&& PanelCenter.Y - 0.5 * PanelSize.Y >= 0.0 && PanelCenter.Y + 0.5 * PanelSize.Y <= 1.0;
				int32 TextsChecked = 0;
				FString MisfitList;
				const int32 Misfits = Menu->CountTextMisfits(TextsChecked, MisfitList);
				const bool bViewOk = Boxes.Num() >= 8 && Outside == 0 && Overlaps == 0 && Stray == 0 && bPanelIn && Misfits == 0;
				R.Checked += Boxes.Num();
				R.WordsFound += TextsChecked;
				R.WorstPaint = FMath::Max(R.WorstPaint, static_cast<float>(Outside + Overlaps + Stray + Misfits + (bPanelIn ? 0 : 1)));
				FVector2D Size;
				PinguMenu3D::GetViewportPixelSize(Player, Size);
				R.Lines.Add(FString::Printf(TEXT("%dx%d at %.0f%% (got %.0fx%.0f): %d buttons, %d off the screen, %d cards overlapping, %d x / difficulty off their card, panel on the screen %d (%.2f x %.2f), %d of %d texts run over%s%s"),
					TagSizes[View].Width, TagSizes[View].Height, 100.f * TagSizes[View].Scale, Size.X, Size.Y, Boxes.Num(), Outside, Overlaps, Stray, bPanelIn ? 1 : 0,
					PanelSize.X, PanelSize.Y, Misfits, TextsChecked, MisfitList.IsEmpty() ? TEXT("") : TEXT(": "), *MisfitList.Left(120)));
				if (!bViewOk)
				{
					R.WorstWord += 1.f;
				}
			}
			if (!R.bShot && T > R.ViewAt + 1.0f)
			{
				R.bShot = true;
				Shot(*FString::Printf(TEXT("78_card_lobby_%dx%d_%.0f"), TagSizes[View].Width, TagSizes[View].Height, 100.f * TagSizes[View].Scale), true);
			}
		},
		[this]
		{
			FTagsRun& R = TagsRun();
			UWorld* World = GetWorld();
			GEngine->Exec(World, TEXT("r.SetRes 1280x720w"));
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().SetApplicationScale(1.f);
			}
			// (WorstWord counts the views that failed.)
			const bool bOk = R.Views == NumTagSizes && R.WorstWord < 0.5f;
			Report(FString::Printf(TEXT("RESULT lobby cards 2d: %d of %d views, %d failed; %s %s"), R.Views, NumTagSizes, FMath::RoundToInt(R.WorstWord),
				*FString::Join(R.Lines, TEXT("; ")), bOk ? TEXT("ok") : TEXT("FAIL")));
			if (APenguinGameMode* GameMode = World->GetAuthGameMode<APenguinGameMode>())
			{
				GameMode->StartPractice();
			}
		});
	}

	// (2026-10-29, Linus: "add a mac camera distance silder in the settings so u can adjust how close the camera is".) In practice
	// (a match phase): the camera's distance at the setting 100 %, 60 % and 140 %; the near and far ones about that share of the
	// 100 % one; pictures 79_camera_distance_<100|60|140>; the setting put back.
	AddStep(TEXT("look_CameraDistance"), 7.5f,
		[this]
		{
			FCameraRun& C = CameraRun();
			C = FCameraRun();
			UPinguSettings* Settings = UPinguSettings::Get();
			C.Before = Settings ? Settings->GetCameraDistance() : 1.f;
			ClearAbilityLeftovers();
		},
		[this](float T)
		{
			FCameraRun& C = CameraRun();
			UPinguSettings* Settings = UPinguSettings::Get();
			APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld());
			if (!Settings || !Stage)
			{
				return;
			}
			static const float Scales[3] = { 1.f, UPinguSettings::MinCameraDistance, UPinguSettings::MaxCameraDistance };
			const int32 Stage3 = FMath::Min(static_cast<int32>(T / 2.4f), 2);
			if (Stage3 != C.Stage)
			{
				C.Stage = Stage3;
				Settings->SetCameraDistance(Scales[Stage3]);
			}
			const float Into = T - 2.4f * Stage3;
			if (Into > 2.f && C.Distance[Stage3] <= 0.f)
			{
				C.Distance[Stage3] = Stage->GetFrameDistance();
				C.Scale[Stage3] = Stage->GetUserDistanceScale();
				Shot(*FString::Printf(TEXT("79_camera_distance_%.0f"), 100.f * Scales[Stage3]), true);
			}
		},
		[this]
		{
			FCameraRun& C = CameraRun();
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				Settings->SetCameraDistance(C.Before);
			}
			const float Near = C.Distance[0] > 1.f ? C.Distance[1] / C.Distance[0] : 0.f;
			const float Far = C.Distance[0] > 1.f ? C.Distance[2] / C.Distance[0] : 0.f;
			const bool bOk = FMath::Abs(Near - UPinguSettings::MinCameraDistance) < 0.06f && FMath::Abs(Far - UPinguSettings::MaxCameraDistance) < 0.08f
				&& FMath::IsNearlyEqual(C.Scale[1], UPinguSettings::MinCameraDistance, 0.01f) && FMath::IsNearlyEqual(C.Scale[2], UPinguSettings::MaxCameraDistance, 0.01f);
			Report(FString::Printf(TEXT("RESULT camera distance: at 100 %% %.0f cm, at 60 %% %.0f cm (%.2f of it, want 0.60), at 140 %% %.0f cm (%.2f, want 1.40); the camera's own scale %.2f / %.2f %s"),
				C.Distance[0], C.Distance[1], Near, C.Distance[2], Far, C.Scale[1], C.Scale[2], bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}

// ============================================================================ lobbytest

bool APinguLobbyTest::UpdateMenu3DTitlePlay(APenguinHUD* LobbyHUD)
{
	using namespace PenguinAutoTestMenu3DPrivate;
	APinguMenu3D* Menu = PinguMenu3D::IsEnabled() && LobbyHUD ? APinguMenu3D::Get(LobbyHUD) : nullptr;
	if (!Menu || Menu->GetPage() != EPinguMenu3DPage::Title)
	{
		return false;
	}
	// PLAY clicked where the mouse would: its face's middle on the screen, traced into the 3D buttons.
	const int32 Play = FindButton(Menu, TEXT("Play"));
	bMenu3DPlayClicked = Play != INDEX_NONE && Menu->PointerDown(Menu->GetButtonInfo(Play).ScreenCenter);
	Report(FString::Printf(TEXT("3D title: PLAY clicked %d"), bMenu3DPlayClicked ? 1 : 0));
	return bMenu3DPlayClicked;
}

bool APinguLobbyTest::UpdateMenu3DLobbyCheck(APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check)
{
	using namespace PenguinAutoTestMenu3DPrivate;
	constexpr int32 Done = 99;
	if (Menu3DStep == Done || !PinguMenu3D::IsEnabled() || !LobbyHUD)
	{
		return false;
	}
	APinguMenu3D* Menu = APinguMenu3D::Get(LobbyHUD);
	const float Now = TotalTime;
	auto Click = [Menu](FName Id)
	{
		const int32 Index = FindButton(Menu, Id);
		return Index != INDEX_NONE && Menu->GetButtonInfo(Index).bShown && Menu->PointerDown(Menu->GetButtonInfo(Index).ScreenCenter);
	};
	auto LabelOf = [Menu](FName Id)
	{
		const int32 Index = FindButton(Menu, Id);
		return Index != INDEX_NONE ? Menu->GetButtonInfo(Index).Label : FString();
	};
	switch (Menu3DStep)
	{
	case 0:
		if (!Menu || Menu->GetPage() != EPinguMenu3DPage::Lobby)
		{
			Menu3DStepAt = Menu3DStepAt < 0.f ? Now : Menu3DStepAt;
			if (Now - Menu3DStepAt > 3.f)
			{
				Check(false, TEXT("3D lobby: the 3D lobby page never showed"));
				Menu3DStep = Done;
			}
			return true;
		}
		Menu3DStepAt = Now;
		Menu3DStep = 1;
		return true;
	case 1:
		if (Now - Menu3DStepAt < 0.6f)
		{
			return true;
		}
		for (int32 Index = 0; Index < Menu->GetButtonCount(); ++Index)
		{
			const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
			const FString IdText = Info.Id.ToString();
			// (The card lobby's cards, difficulties, x and ADD BOT are buttons too: only the signs are listed.)
			const bool bCardPart = IdText.StartsWith(TEXT("Card_")) || IdText.StartsWith(TEXT("Skill_")) || IdText.StartsWith(TEXT("Remove_")) || IdText == TEXT("AddBot");
			if (Info.bShown && !bCardPart)
			{
				Menu3DShown += (Menu3DShown.IsEmpty() ? TEXT("") : TEXT(" ")) + IdText;
			}
		}
		Shot(TEXT("lobbytest_menu3d"));
		bMenu3DTraitsBefore = LobbyHUD->AreTraitsOn();
		Menu3DLabels = LabelOf(TEXT("Traits"));
		bMenu3DClicks = Click(TEXT("Traits"));
		Menu3DStepAt = Now;
		Menu3DStep = 2;
		return true;
	case 2:
		if (Now - Menu3DStepAt < 0.35f)
		{
			return true;
		}
		bMenu3DTraitsToggled = LobbyHUD->AreTraitsOn() != bMenu3DTraitsBefore;
		Menu3DLabels += TEXT(" > ") + LabelOf(TEXT("Traits"));
		bMenu3DClicks = Click(TEXT("Traits")) && bMenu3DClicks;
		Menu3DStepAt = Now;
		Menu3DStep = 3;
		return true;
	case 3:
		if (Now - Menu3DStepAt < 0.35f)
		{
			return true;
		}
		bMenu3DTraitsBack = LobbyHUD->AreTraitsOn() == bMenu3DTraitsBefore;
		Menu3DLabels += TEXT(" > ") + LabelOf(TEXT("Traits"));
		bMenu3DClicks = Click(TEXT("Settings")) && bMenu3DClicks;
		Menu3DStepAt = Now;
		Menu3DStep = 4;
		return true;
	case 4:
		if (Now - Menu3DStepAt < 0.4f)
		{
			return true;
		}
		bMenu3DSettings = LobbyHUD->GetScreen() == EPinguScreen::Settings;
		// (2026-11-07: the settings page's APPLY / "Apply changes?" board first, still on the settings page afterwards.)
		Menu3DStep = 6;
		return true;
	case 6:
		if (PinguSettingsApplyFlow(LobbyHUD, Menu, Now, Check))
		{
			return true;
		}
		LobbyHUD->Action_Back();
		Menu3DStepAt = Now;
		Menu3DStep = 5;
		return true;
	case 5:
	{
		if (Now - Menu3DStepAt < 0.5f)
		{
			return true;
		}
		const bool bBack = LobbyHUD->GetScreen() == EPinguScreen::Lobby && Menu->GetPage() == EPinguMenu3DPage::Lobby;
		// (Any order: the island's strip lists SETTINGS first, the card lobby BACK - Linus's order since 2026-10-29.)
		TArray<FString> ShownIds;
		Menu3DShown.ParseIntoArray(ShownIds, TEXT(" "));
		ShownIds.Remove(TEXT("PicksMode")); // (the ability picks' row under the cards, 2026-11-07: its own lobbytest checks)
		ShownIds.Remove(TEXT("TraitList")); // (the host's TRAITS list sign, 2026-11-07: its own lobbytest checks - the traits stage)
		ShownIds.Sort();
		const bool bShownOk = FString::Join(ShownIds, TEXT(" ")) == TEXT("Back Settings Start Traits");
		const bool bOk = bMenu3DPlayClicked && bShownOk && bMenu3DClicks && bMenu3DTraitsToggled && bMenu3DTraitsBack && bMenu3DSettings && bBack;
		Check(bOk, FString::Printf(TEXT("3D menu buttons: PLAY clicked on the 3D title %d; the lobby's 3D buttons shown: %s (want Settings Back Start Traits); clicks traced %d; TRAITS toggled %d and back %d (%s); SETTINGS opened the settings %d, back in the lobby %d"),
			bMenu3DPlayClicked ? 1 : 0, *Menu3DShown, bMenu3DClicks ? 1 : 0, bMenu3DTraitsToggled ? 1 : 0, bMenu3DTraitsBack ? 1 : 0, *Menu3DLabels,
			bMenu3DSettings ? 1 : 0, bBack ? 1 : 0));
		Menu3DStep = Done;
		return false;
	}
	default:
		return false;
	}
}
