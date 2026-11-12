// Pingu - the mouse on the 3D title and the lobby island (2026-10-24, Linus: "the lobby registers the mouse really bad ... the
// click register way beter"): clicks_Menu3D (autotest; alone: PINGU_AUTOTEST_ONLY=clicks_Menu3D). Every click goes the real way
// a player's does: a Slate mouse-down / up at a screen position (FSlateApplication::ProcessMouseButtonDownEvent), through the
// screen's input layer (SPinguMenu3DInput), the viewport widget's geometry (PinguMenu3D::ScreenToViewportPixel) and the player
// controller's deprojection, into the 3D buttons (APinguMenu3D) or the island's things (APinguLobby, in its probe mode: resolved,
// not done).
//   1. The 3D title: every button at 9 points over its face (the middle, the edges at 96 % of its size, the corners), at the
//      game's 1280x720 and at 1920x1080 (r.SetRes; reported if the window would not change): every click must press that button.
//   2. The lobby island (4 bots, as look_LobbyIsland): every x, difficulty tag, name tag, penguin body and the "+" at 9 points
//      each, and the strip's buttons (the lobby page in its test mode), at both sizes: every click must hit that thing.
//   3. The viewport mapping with a made-up PIE-style geometry (the viewport 320, 180 px into the window at a DPI scale of 1.5):
//      a pixel to a screen position and back must come out the same.
//   4. No walking on the lobby island: the move / jump input is blocked there (APenguinCharacter::IsInputBlockedByLobby) and not
//      in practice; the D key held 0.6 s moves our penguin nowhere.
// RESULT lines: one per size and page, one for the mapping, one for the walking, and a summary.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PinguLobby.h"
#include "PinguMenu3D.h"
#include "PinguPickup.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Layout/Geometry.h"
#include "Slate/SceneViewport.h"

namespace PenguinAutoTestClicksPrivate
{
	/** The nine points over a face: the middle, the edges and the corners at this share of the half size. */
	constexpr float EdgeShare = 0.96f;
	/** Seconds between two button presses (a press's push-in must end before the next one counts). */
	constexpr float PressGap = 0.13f;

	struct FClickRun
	{
		int32 Stage = 0;
		float NextAt = 0.f;
		TWeakObjectPtr<APinguMenu3D> Menu;
		TArray<FName> Fired;
		// the queue of clicks of the stage that runs
		struct FClick
		{
			FVector2D Pixel = FVector2D::ZeroVector;
			FName ButtonId;
			/** A 3D button: its index and the point of its face (-1..1), projected when the click is sent (the card sways). */
			int32 ButtonIndex = INDEX_NONE;
			FVector2D FaceShare = FVector2D::ZeroVector;
			APinguLobby::EHit Kind = APinguLobby::EHit::None;
			TWeakObjectPtr<const APenguinCharacter> Penguin;
			FString What;
			/** An island thing: its point in the world (projected when the click is sent: the camera settles, a bot may shift). */
			FVector World = FVector::ZeroVector;
			bool bWorld = false;
		};
		TArray<FClick> Queue;
		int32 QueueAt = 0;
		/** Clicks of the queue judged so far (a button's is judged once its press fired), and how many ids had fired before the last. */
		int32 Counted = 0;
		int32 FiredBefore = 0;
		int32 Hits = 0;
		int32 Misses = 0;
		FString MissNotes;
		TMap<FString, FIntPoint> PerKind;
		FIntPoint Size = FIntPoint::ZeroValue;
		// the results per stage
		TArray<FString> Lines;
		bool bAllOk = true;
		// the lobby
		TArray<TWeakObjectPtr<APenguinCharacter>> Hidden;
		bool bBlockedInLobby = false;
		bool bBlockedInPractice = true;
		float WalkStartX = 0.f;
		float WalkedX = 0.f;
		bool bKeyDown = false;
		bool bMappingOk = false;
		FString MappingNote;
	};
	static FClickRun& Run()
	{
		static FClickRun State;
		return State;
	}

	static FSceneViewport* SceneViewport(const UWorld* World)
	{
		UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
		return Client ? Client->GetGameViewport() : nullptr;
	}

	/** A left (or right) click at a viewport pixel the way the mouse does it: a Slate mouse-down and up at that screen position. */
	static bool SlateClick(const UWorld* World, const FVector2D& Pixel, bool bRight)
	{
		FSceneViewport* Viewport = SceneViewport(World);
		if (!Viewport || !FSlateApplication::IsInitialized())
		{
			return false;
		}
		FSlateApplication& Slate = FSlateApplication::Get();
		const FVector2D Screen = PinguMenu3D::ScreenFromGeometry(Viewport->GetCachedGeometry(), Pixel);
		const FKey Button = bRight ? EKeys::RightMouseButton : EKeys::LeftMouseButton;
		TSet<FKey> Pressed;
		Pressed.Add(Button);
		const FPointerEvent Move(0u, FSlateApplication::CursorPointerIndex, Screen, Screen, TSet<FKey>(), EKeys::Invalid, 0.f, Slate.GetModifierKeys());
		Slate.ProcessMouseMoveEvent(Move);
		const FPointerEvent Down(0u, FSlateApplication::CursorPointerIndex, Screen, Screen, Pressed, Button, 0.f, Slate.GetModifierKeys());
		Slate.ProcessMouseButtonDownEvent(nullptr, Down);
		const FPointerEvent Up(0u, FSlateApplication::CursorPointerIndex, Screen, Screen, TSet<FKey>(), Button, 0.f, Slate.GetModifierKeys());
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

	/** The nine points of a rectangle (its middle and half size in pixels). */
	static void NinePoints(const FVector2D& Centre, const FVector2D& Half, TArray<FVector2D>& Out)
	{
		for (int32 Y = -1; Y <= 1; ++Y)
		{
			for (int32 X = -1; X <= 1; ++X)
			{
				Out.Add(Centre + FVector2D(X * Half.X * EdgeShare, Y * Half.Y * EdgeShare));
			}
		}
	}

	static void QueueTitleButtons(FClickRun& State, const FIntPoint& Size)
	{
		APinguMenu3D* Menu = State.Menu.Get();
		for (int32 Index = 0; Menu && Index < Menu->GetButtonCount(); ++Index)
		{
			const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
			if (!Info.bFocusable || Info.ScreenSize.X <= 0.f)
			{
				continue;
			}
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				for (int32 X = -1; X <= 1; ++X)
				{
					FClickRun::FClick& Click = State.Queue.AddDefaulted_GetRef();
					Click.ButtonIndex = Index;
					Click.FaceShare = FVector2D(X * EdgeShare, Y * EdgeShare);
					Click.ButtonId = Info.Id;
					Click.What = Info.Id.ToString();
				}
			}
		}
	}

	static void QueueLobbyTargets(FClickRun& State, const APinguLobby* Lobby, const APlayerController* Player)
	{
		if (!Lobby || !Player)
		{
			return;
		}
		for (const APinguLobby::FTarget& Target : Lobby->GetClickTargets())
		{
			// The nine points over the thing in its own plane (the x is round: its edge points at 0.68, the disc's inscribed square;
			// a penguin's body: its rows at the chest and the feet, since its tags stand in front of its head - the tags' plane is
			// nearer the camera, so a click on the head lands on the tag, as it looks).
			const float Share = (Target.Kind == APinguLobby::EHit::Remove ? 0.68f : 1.f) * EdgeShare;
			const float ShareZ = Target.Kind == APinguLobby::EHit::Body ? 0.45f : Share;
			TArray<FVector> Points;
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				for (int32 X = -1; X <= 1; ++X)
				{
					Points.Add(Target.Centre + FVector(X * Target.Half.X * Share, 0.f, Y * Target.Half.Y * ShareZ));
				}
			}
			for (const FVector& P : Points)
			{
				FClickRun::FClick& Click = State.Queue.AddDefaulted_GetRef();
				Click.World = P;
				Click.bWorld = true;
				Click.Kind = Target.Kind;
				Click.Penguin = Target.Penguin;
				Click.What = FString::Printf(TEXT("%s%s"), APinguLobby::HitName(Target.Kind),
					Target.Penguin.IsValid() ? *FString::Printf(TEXT("(%s)"), *Target.Penguin->GetDisplayName()) : TEXT(""));
			}
		}
	}

	static void Count(FClickRun& State, const FString& Kind, bool bHit, const FString& Note)
	{
		FIntPoint& Pair = State.PerKind.FindOrAdd(Kind);
		Pair.X += bHit ? 1 : 0;
		Pair.Y += 1;
		State.Hits += bHit ? 1 : 0;
		State.Misses += bHit ? 0 : 1;
		if (!bHit)
		{
			UE_LOG(LogPingu, Log, TEXT("[CLICKS] miss: %s"), *Note);
			if (State.MissNotes.Len() < 1200)
			{
				State.MissNotes += (State.MissNotes.IsEmpty() ? TEXT("") : TEXT("; ")) + Note;
			}
		}
	}

	static FString PerKindText(const FClickRun& State)
	{
		FString Text;
		for (const TPair<FString, FIntPoint>& Pair : State.PerKind)
		{
			Text += FString::Printf(TEXT("%s%s %d/%d"), Text.IsEmpty() ? TEXT("") : TEXT(", "), *Pair.Key, Pair.Value.X, Pair.Value.Y);
		}
		return Text;
	}

	static void FinishStage(TFunctionRef<void(const FString&)> Report, FClickRun& State, const TCHAR* Label)
	{
		const bool bOk = State.Misses == 0 && State.Hits > 0;
		State.bAllOk &= bOk;
		const FString Line = FString::Printf(TEXT("RESULT clicks %s at %dx%d: %d/%d points hit (%s)%s %s"), Label, State.Size.X, State.Size.Y,
			State.Hits, State.Hits + State.Misses, *PerKindText(State), State.MissNotes.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" missed: %s"), *State.MissNotes),
			bOk ? TEXT("ok") : TEXT("FAIL"));
		Report(Line);
		State.Lines.Add(Line);
		State.Queue.Reset();
		State.QueueAt = 0;
		State.Counted = 0;
		State.FiredBefore = 0;
		State.Hits = 0;
		State.Misses = 0;
		State.MissNotes.Reset();
		State.PerKind.Reset();
	}
}

void APenguinAutoTest::AddMenuClickStep()
{
	using namespace PenguinAutoTestClicksPrivate;
	AddStep(TEXT("clicks_Menu3D"), 75.f, // (75 since 2026-10-29: the card lobby has 18 buttons x 9 points at two sizes)
		[this]
		{
			FClickRun& State = Run();
			State = FClickRun();
			UWorld* World = GetWorld();
			APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
			APenguinHUD* HUD = Controller ? Cast<APenguinHUD>(Controller->GetHUD()) : nullptr;
			State.Menu = PinguMenu3D::IsEnabled() && HUD ? APinguMenu3D::Get(HUD) : nullptr;
			State.bBlockedInPractice = Penguin.IsValid() && Penguin->IsInputBlockedByLobby();
			// The real title screen (its input layer is only there while it shows), its 3D page in the test mode.
			if (HUD)
			{
				HUD->SetTestScreen(EPinguScreen::MainMenu);
			}
			if (APinguMenu3D* Menu = State.Menu.Get())
			{
				Menu->SetTestPage(EPinguMenu3DPage::Title, [](FName Id) { Run().Fired.Add(Id); });
			}
			// 3. The viewport mapping with a PIE-style geometry: 320, 180 px into the window, DPI 1.5.
			const FGeometry Geometry = FGeometry::MakeRoot(FVector2D(1000.f, 600.f), FSlateLayoutTransform(1.5f, FVector2D(320.f, 180.f)));
			const FVector2D Pixels[3] = { FVector2D(0.f, 0.f), FVector2D(640.f, 360.f), FVector2D(1499.f, 899.f) };
			State.bMappingOk = true;
			for (const FVector2D& Pixel : Pixels)
			{
				const FVector2D Screen = PinguMenu3D::ScreenFromGeometry(Geometry, Pixel);
				const FVector2D Back = PinguMenu3D::PixelFromGeometry(Geometry, Screen);
				State.bMappingOk &= FVector2D::Distance(Back, Pixel) < 0.05f;
				State.MappingNote += FString::Printf(TEXT("%s(%.0f, %.0f) -> screen (%.0f, %.0f) -> (%.1f, %.1f)"), State.MappingNote.IsEmpty() ? TEXT("") : TEXT("; "),
					Pixel.X, Pixel.Y, Screen.X, Screen.Y, Back.X, Back.Y);
			}
			// The window's own mapping: the middle pixel to a screen position and back.
			if (const FSceneViewport* Viewport = SceneViewport(World))
			{
				const FIntPoint Size = ViewSize(World);
				const FVector2D Mid(0.5f * Size.X, 0.5f * Size.Y);
				FVector2D Back;
				const bool bReal = PinguMenu3D::ScreenToViewportPixel(World, PinguMenu3D::ScreenFromGeometry(Viewport->GetCachedGeometry(), Mid), Back)
					&& FVector2D::Distance(Back, Mid) < 0.05f;
				State.bMappingOk &= bReal;
				State.MappingNote += FString::Printf(TEXT("; the window's: (%.0f, %.0f) back as (%.1f, %.1f), scale %.2f"), Mid.X, Mid.Y, Back.X, Back.Y,
					Viewport->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale());
			}
		},
		[this](float T)
		{
			FClickRun& State = Run();
			UWorld* World = GetWorld();
			APinguMenu3D* Menu = State.Menu.Get();
			APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
			APenguinGameMode* GameMode = World ? World->GetAuthGameMode<APenguinGameMode>() : nullptr;
			APinguLobby* Lobby = APinguLobby::Get(World);
			if (!Menu || !Player || !GameMode || T < State.NextAt)
			{
				return;
			}
			// One click of the queue (title / strip buttons: spaced by the press's push-in; the island's things: one a frame).
			auto Pump = [&](bool bButtons) -> bool
			{
				if (bButtons)
				{
					// The last click's button must have fired (its id recorded) before the next one is judged and sent.
					if (Menu->IsPressPending())
					{
						return false;
					}
					if (State.Counted < State.QueueAt)
					{
						const FClickRun::FClick& Last = State.Queue[State.QueueAt - 1];
						const bool bHit = State.Fired.Num() == State.FiredBefore + 1 && State.Fired.Last() == Last.ButtonId;
						Count(State, Last.What, bHit, FString::Printf(TEXT("%s at %.0f,%.0f fired %s"), *Last.What, Last.Pixel.X, Last.Pixel.Y,
							State.Fired.Num() > State.FiredBefore ? *State.Fired.Last().ToString() : TEXT("nothing")));
						State.Counted = State.QueueAt;
					}
					if (State.QueueAt >= State.Queue.Num())
					{
						return true;
					}
					FClickRun::FClick& Click = State.Queue[State.QueueAt++];
					State.FiredBefore = State.Fired.Num();
					FVector2D Pixel;
					if (Menu->GetButtonFacePixel(Click.ButtonIndex, Click.FaceShare, Pixel))
					{
						Click.Pixel = Pixel;
					}
					SlateClick(World, Click.Pixel, false);
					State.NextAt = T + PressGap;
					return false;
				}
				if (State.QueueAt >= State.Queue.Num())
				{
					return true;
				}
				FClickRun::FClick& Click = State.Queue[State.QueueAt++];
				State.Counted = State.QueueAt;
				if (Click.bWorld)
				{
					FVector2D Pixel;
					if (Player->ProjectWorldLocationToScreen(Click.World, Pixel, false))
					{
						Click.Pixel = Pixel;
					}
				}
				if (Lobby)
				{
					Lobby->ClearProbe();
				}
				SlateClick(World, Click.Pixel, false);
				const APinguLobby::FHit Probe = Lobby ? Lobby->GetLastProbe() : APinguLobby::FHit();
				bool bHit = Probe.Kind == Click.Kind && Probe.Penguin.Get() == Click.Penguin.Get();
				// Bots that stand close: a neighbour's x, difficulty tag or name tag may lie over this tag's end or this body. By the
				// agreed order (+, x, difficulty, name, body) the neighbour's thing wins there: that is not a miss. The "+" never
				// covers anything (it is the thing this step guards).
				const auto Rank = [](APinguLobby::EHit Kind)
				{
					return Kind == APinguLobby::EHit::Remove ? 1 : Kind == APinguLobby::EHit::Skill ? 2 : Kind == APinguLobby::EHit::Name ? 3 : Kind == APinguLobby::EHit::Body ? 4 : 9;
				};
				if (!bHit && Click.Penguin.IsValid() && Probe.Penguin.IsValid() && Probe.Penguin.Get() != Click.Penguin.Get()
					&& Rank(Probe.Kind) < Rank(Click.Kind))
				{
					bHit = true;
					UE_LOG(LogPingu, Log, TEXT("[CLICKS] %s at %.0f,%.0f lies under a neighbour's %s(%s): fine"), *Click.What, Click.Pixel.X, Click.Pixel.Y,
						APinguLobby::HitName(Probe.Kind), *Probe.Penguin->GetDisplayName());
				}
				Count(State, Click.What, bHit, FString::Printf(TEXT("%s at %.0f,%.0f got %s%s"), *Click.What, Click.Pixel.X, Click.Pixel.Y,
					APinguLobby::HitName(Probe.Kind), Probe.Penguin.IsValid() ? *FString::Printf(TEXT("(%s)"), *Probe.Penguin->GetDisplayName()) : TEXT("")));
				return false;
			};
			switch (State.Stage)
			{
			case 0: // the title has come in at the game's size
				if (T > 0.8f)
				{
					State.Size = ViewSize(World);
					QueueTitleButtons(State, State.Size);
					State.Fired.Reset();
					State.Stage = 1;
				}
				break;
			case 1:
				if (Pump(true))
				{
					FinishStage([this](const FString& M) { Report(M); }, State, TEXT("3D title"));
					SetRes(World, 1920, 1080);
					State.NextAt = T + 1.2f;
					State.Stage = 2;
				}
				break;
			case 2: // the title at 1920x1080 (or whatever the window became)
				State.Size = ViewSize(World);
				QueueTitleButtons(State, State.Size);
				State.Fired.Reset();
				State.Stage = 3;
				break;
			case 3:
				if (Pump(true))
				{
					FinishStage([this](const FString& M) { Report(M); }, State, TEXT("3D title"));
					// 2. The lobby island with four bots (as look_LobbyIsland), still at the big size.
					for (TActorIterator<APenguinCharacter> It(World); It; ++It)
					{
						if (*It != Penguin && !GameMode->GetPenguins().Contains(*It))
						{
							It->SetActorHiddenInGame(true);
							It->SetActorEnableCollision(false);
							It->SetActorLocation(FVector(0.f, 3000.f, -5000.f));
							State.Hidden.Add(*It);
						}
					}
					APinguPickup::DestroyAll(World);
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
					if (Penguin.IsValid())
					{
						Penguin->SetTeam(-1);
					}
					if (APenguinHUD* HUD = Cast<APenguinHUD>(Player->GetHUD()))
					{
						HUD->SetTestScreen(EPinguScreen::None);
					}
					GameMode->EnterLobby();
					PinguLobby::SetBotsFrozen(true);
					if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(World))
					{
						Stage->SetActorTickEnabled(true);
						Stage->SnapToTargets(0.5f);
					}
					VirtualInput = FVector2D::ZeroVector;
					Menu->SetTestPage(EPinguMenu3DPage::Lobby, [](FName Id) { Run().Fired.Add(Id); });
					State.NextAt = T + 2.5f;
					State.Stage = 4;
				}
				break;
			case 4: // the island's things, then the strip
				if (!PinguMenu3D::UseIslandLobby())
				{
					// (The wooden card lobby, 2026-10-29: its buttons - every card that is yours, the bots' difficulty and x, ADD BOT,
					// the signs - at nine points each, as the title's.)
					State.Size = ViewSize(World);
					QueueTitleButtons(State, State.Size);
					State.Fired.Reset();
					State.Stage = 6;
					break;
				}
				if (!Lobby)
				{
					Report(TEXT("RESULT clicks lobby island: no lobby actor FAIL"));
					State.bAllOk = false;
					State.Stage = 8;
					break;
				}
				State.Size = ViewSize(World);
				State.bBlockedInLobby = Penguin.IsValid() && Penguin->IsInputBlockedByLobby();
				Lobby->SetClickProbe(true);
				QueueLobbyTargets(State, Lobby, Player);
				State.Stage = 5;
				break;
			case 5:
				if (Pump(false))
				{
					Lobby->SetClickProbe(false);
					QueueTitleButtons(State, State.Size); // the strip's buttons (the lobby page shows)
					State.Fired.Reset();
					State.Stage = 6;
				}
				break;
			case 6:
				if (Pump(true))
				{
					FinishStage([this](const FString& M) { Report(M); }, State, PinguMenu3D::UseIslandLobby() ? TEXT("lobby island") : TEXT("card lobby"));
					if (State.Size.X > 1600)
					{
						SetRes(World, 1280, 720);
						State.NextAt = T + 1.2f;
						State.Stage = 4; // once more at the game's own size
						State.Size = FIntPoint::ZeroValue;
					}
					else
					{
						// 4. The D key held: our penguin must stay put on the island.
						State.WalkStartX = Penguin.IsValid() ? static_cast<float>(Penguin->GetActorLocation().X) : 0.f;
						APenguinInputTest::SendKeyEvent(EKeys::D, true);
						State.bKeyDown = true;
						State.NextAt = T + 0.6f;
						State.Stage = 7;
					}
				}
				break;
			case 7:
				APenguinInputTest::SendKeyEvent(EKeys::D, false);
				State.bKeyDown = false;
				State.WalkedX = Penguin.IsValid() ? static_cast<float>(Penguin->GetActorLocation().X) - State.WalkStartX : 0.f;
				State.Stage = 8;
				break;
			default:
				break;
			}
		},
		[this]
		{
			FClickRun& State = Run();
			if (State.bKeyDown)
			{
				APenguinInputTest::SendKeyEvent(EKeys::D, false);
			}
			if (APinguLobby* Lobby = APinguLobby::Get(GetWorld()))
			{
				Lobby->SetClickProbe(false);
			}
			PinguLobby::SetBotsFrozen(false);
			if (APinguMenu3D* Menu = State.Menu.Get())
			{
				Menu->SetTestPage(EPinguMenu3DPage::None);
			}
			if (APlayerController* Player = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
			{
				if (APenguinHUD* HUD = Cast<APenguinHUD>(Player->GetHUD()))
				{
					HUD->SetTestScreen(EPinguScreen::None);
				}
			}
			SetRes(GetWorld(), 1280, 720);
			Report(FString::Printf(TEXT("RESULT clicks viewport mapping (PIE-style offset 320, 180 and DPI 1.5): %s %s"), *State.MappingNote, State.bMappingOk ? TEXT("ok") : TEXT("FAIL")));
			// (The card lobby: its panel has the keys - the penguin stays put without being blocked; Esc hands them to the game.)
			const bool bWalkOk = (State.bBlockedInLobby || !PinguMenu3D::UseIslandLobby()) && !State.bBlockedInPractice && FMath::Abs(State.WalkedX) < 5.f;
			Report(FString::Printf(TEXT("RESULT clicks no walking in the lobby: input blocked on the island %d, in practice %d, D held 0.6 s moved %.1f cm %s"),
				State.bBlockedInLobby ? 1 : 0, State.bBlockedInPractice ? 1 : 0, State.WalkedX, bWalkOk ? TEXT("ok") : TEXT("FAIL")));
			const bool bOk = State.bAllOk && State.Lines.Num() == 4 && State.bMappingOk && bWalkOk;
			Report(FString::Printf(TEXT("RESULT clicks menu3d: %d click runs (title and lobby at two sizes), %d done %s"), State.Lines.Num(), State.bAllOk ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
			// Back to practice (the bots go), the dummy back in sight.
			if (APenguinGameMode* GameMode = GetWorld()->GetAuthGameMode<APenguinGameMode>())
			{
				GameMode->StartPractice();
			}
			for (const TWeakObjectPtr<APenguinCharacter>& Each : State.Hidden)
			{
				if (Each.IsValid())
				{
					Each->SetActorHiddenInGame(false);
					Each->SetActorEnableCollision(true);
				}
			}
		});
}
