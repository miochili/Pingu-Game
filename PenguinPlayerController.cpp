// Pingu - player controller.

#include "PenguinPlayerController.h"
#include "PinguAntiCheat.h"
#include "Penguin.h"
#include "PenguinBot.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PenguinInputConfig.h"
#include "PenguinMovementComponent.h"
#include "PenguinPlayerState.h"
#include "PinguNetSubsystem.h"
#include "PinguNetTest.h"
#include "PinguSettings.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SViewport.h"
#include "Engine/World.h"
#include "InputActionValue.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"

namespace PenguinPlayerControllerPrivate
{
	/** Counts jump key presses at the very start of Slate's input handling, before any widget or the game (the jump stats
	 *  compare them with the presses the game got), and every other key press too (the leaderboard between rounds waits for
	 *  one). Never takes a key. */
	class FJumpKeyCounter : public IInputProcessor
	{
	public:
		explicit FJumpKeyCounter(const TSharedPtr<SWindow>& InGameWindow)
			: GameWindow(InGameWindow)
		{
		}

		/** Presses since the player controller last took them (every frame). */
		int32 Presses = 0;
		/** Presses of any key or mouse button but the menu and help keys, since the player controller last took them. */
		int32 AnyPresses = 0;

		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override
		{
		}

		virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
		{
			if (!InKeyEvent.IsRepeat())
			{
				Count(SlateApp, InKeyEvent.GetKey());
			}
			return false;
		}

		// Jump can be bound to a mouse button too.
		virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			Count(SlateApp, MouseEvent.GetEffectingButton());
			return false;
		}

		virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			Count(SlateApp, MouseEvent.GetEffectingButton());
			return false;
		}

		virtual const TCHAR* GetDebugName() const override
		{
			return TEXT("PinguJumpKeyCounter");
		}

	private:
		void Count(FSlateApplication& SlateApp, const FKey& Key)
		{
			// Several game windows in one process (multi-player Play In Editor): only the presses made in ours count as jumps.
			const TSharedPtr<SWindow> Window = GameWindow.Pin();
			if (UPenguinInputConfig::IsJumpKey(Key) && (!Window.IsValid() || SlateApp.GetActiveTopLevelWindow() == Window))
			{
				++Presses;
			}
			// Any key goes on to the next round (whoever presses it) - but the menu (Esc, Start, the pause keys) and help (H,
			// F1) keys, which open their own things, and Shift / Ctrl / Alt / the Windows key (Alt+Tab away must not go on).
			const UPinguSettings* Settings = UPinguSettings::Get();
			const bool bMenuKey = Key == EKeys::Escape || Key == EKeys::Gamepad_Special_Right || Key == EKeys::Gamepad_Special_Left || Key == EKeys::H
				|| Key == EKeys::F1 || Key.IsModifierKey() || Key == EKeys::LeftCommand || Key == EKeys::RightCommand
				|| (Settings && Settings->GetKeys(EPinguInputAction::Pause).Contains(Key));
			if (!bMenuKey)
			{
				++AnyPresses;
			}
		}

		TWeakPtr<SWindow> GameWindow;
	};
}

APenguinPlayerController::APenguinPlayerController()
{
	bAutoManageActiveCameraTarget = false; // every player looks through the shared stage camera
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Default; // the plain OS arrow, in the menus and in the game
	bEnableClickEvents = false;
	bEnableMouseOverEvents = false;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	bShouldPerformFullTickWhenPaused = true;
}

void APenguinPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (!IsLocalPlayerController())
	{
		return; // a remote player's controller on the server
	}
	ViewSharedCamera();
	// Rebindable keys: whenever a binding changes (the controls screen), the new keys work at once, also mid-game.
	if (UPinguSettings* Settings = UPinguSettings::Get())
	{
		BindingsSource = Settings;
		BindingsChangedHandle = Settings->OnBindingsChanged.AddUObject(this, &APenguinPlayerController::HandleBindingsChanged);
	}
	RefreshInputMappings(); // the bindings may have changed since the mapping was built (an earlier Play In Editor run)
	ApplyStartupSettings();
	UpdateInputMode();

	// One counter for the keyboard (the first local player).
	const ULocalPlayer* OwningLocalPlayer = GetLocalPlayer();
	if (FSlateApplication::IsInitialized() && !JumpKeyCounter.IsValid() && OwningLocalPlayer && IsFirstLocalController())
	{
		TSharedPtr<SWindow> GameWindow;
		if (OwningLocalPlayer->ViewportClient)
		{
			GameWindow = OwningLocalPlayer->ViewportClient->GetWindow();
		}
		JumpKeyCounter = MakeShared<PenguinPlayerControllerPrivate::FJumpKeyCounter>(GameWindow);
		FSlateApplication::Get().RegisterInputPreProcessor(JumpKeyCounter);
	}
}

void APenguinPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (IsLocalPlayerController() && KeyJumpPresses > 0)
	{
		LogJumpStats(TEXT("end of play"));
	}
	if (UPinguSettings* Settings = BindingsSource.Get())
	{
		Settings->OnBindingsChanged.Remove(BindingsChangedHandle);
	}
	BindingsSource.Reset();
	BindingsChangedHandle.Reset();
	if (JumpKeyCounter.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(JumpKeyCounter);
	}
	JumpKeyCounter.Reset();
	Super::EndPlay(EndPlayReason);
}

void APenguinPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	ViewSharedCamera();
}

void APenguinPlayerController::ViewSharedCamera()
{
	// The stage camera is not replicated (each machine frames the penguins itself): a remote player's controller on the
	// server must not hand its client a view target that does not exist over there.
	if (!IsLocalController())
	{
		return;
	}
	APenguinCamera* StageCamera = SharedCamera.Get();
	if (!StageCamera)
	{
		StageCamera = APenguinCamera::FindOrSpawn(GetWorld());
		SharedCamera = StageCamera;
	}
	if (StageCamera && GetViewTarget() != StageCamera)
	{
		SetViewTarget(StageCamera);
	}
}

bool APenguinPlayerController::IsFirstLocalController() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return IsLocalPlayerController() && GameInstance && GameInstance->GetFirstLocalPlayerController(GetWorld()) == this;
}

void APenguinPlayerController::ApplyStartupSettings()
{
	UWorld* World = GetWorld();
	if (!World || !IsFirstLocalController())
	{
		return;
	}
	// Once per world start (the settings make these cheap and harmless to repeat anyway).
	static TWeakObjectPtr<UWorld> SettingsAppliedWorld;
	if (SettingsAppliedWorld.Get() == World)
	{
		return;
	}
	SettingsAppliedWorld = World;
	if (UPinguSettings* Settings = UPinguSettings::Get())
	{
		Settings->EnforceNativeResolution(); // Fullscreen / Borderless: the monitor's native resolution, also for an old smaller saved one
		Settings->AutoDetectIfNeeded(World);
		Settings->ApplyGraphics(World);
		Settings->ApplyAudio(this);
	}
}

void APenguinPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	RefreshInputMappings(); // adds the mapping context (keys from the current bindings) for this player

	const UPenguinInputConfig* Config = UPenguinInputConfig::Get();
	if (UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent))
	{
		Input->BindAction(Config->AddPlayer, ETriggerEvent::Started, this, &APenguinPlayerController::Input_AddPlayer);
		Input->BindAction(Config->ToggleHelp, ETriggerEvent::Started, this, &APenguinPlayerController::Input_ToggleHelp);
		Input->BindAction(Config->Pause, ETriggerEvent::Started, this, &APenguinPlayerController::Input_Pause);
	}
}

void APenguinPlayerController::RefreshInputMappings()
{
	UPenguinInputConfig* Config = UPenguinInputConfig::Get();
	Config->RebuildMappings(); // nothing to do when the keys did not change (every local player calls it)
	UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer());
	if (!Subsystem || !Config->Context)
	{
		return;
	}
	if (!Subsystem->HasMappingContext(Config->Context))
	{
		Subsystem->AddMappingContext(Config->Context, 0);
	}
	else if (AppliedMappingVersion != Config->GetMappingVersion())
	{
		// The same context with other keys inside: this player's copy of the mappings is rebuilt at the end of the frame.
		// Keys held right now go on working (none is ignored until released, none gets stuck).
		FModifyContextOptions Options;
		Options.bIgnoreAllPressedKeysUntilRelease = false;
		Subsystem->RequestRebuildControlMappings(Options, EInputMappingRebuildType::Rebuild);
	}
	AppliedMappingVersion = Config->GetMappingVersion();
}

void APenguinPlayerController::HandleBindingsChanged()
{
	if (IsLocalPlayerController())
	{
		RefreshInputMappings();
	}
}

void APenguinPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime); // processes this frame's input
	if (!IsLocalPlayerController())
	{
		return;
	}
	ViewSharedCamera(); // again if anything (the server, a level change) took the view somewhere else
	UpdateInputMode();
	UpdateJumpKeys();
	UpdateContinueKey();
	UpdateAim();
	UpdateWarpView(DeltaTime);
	PinguAntiCheat::TickClientHandshake(this, DeltaTime); // (a LAN joiner only: the join check)
	if (IsFirstLocalController())
	{
		if (UPinguSettings* Settings = UPinguSettings::Get())
		{
			// Low adapts its 3D resolution to this PC (real seconds: slow motion does not slow the GPU down).
			Settings->UpdateAutoResolution(GetWorld(), static_cast<float>(FApp::GetDeltaTime()));
			// A first start measures the preset High on this PC's frames for a few seconds and picks the preset.
			Settings->UpdateFirstStartPick(GetWorld());
		}
	}
}

void APenguinPlayerController::UpdateJumpKeys()
{
	int32 SlatePresses = 0;
	if (JumpKeyCounter.IsValid())
	{
		PenguinPlayerControllerPrivate::FJumpKeyCounter& Counter = static_cast<PenguinPlayerControllerPrivate::FJumpKeyCounter&>(*JumpKeyCounter);
		SlatePresses = Counter.Presses;
		Counter.Presses = 0;
	}
	if (UPenguinInputConfig::UseOldJumpInput())
	{
		return; // the pawn's Enhanced Input jump binding does it
	}
	APenguinCharacter* Penguin = Cast<APenguinCharacter>(GetPawn());
	if (!Penguin || bMenuMode || Penguin->IsInputBlockedByLobby())
	{
		if (Penguin && bJumpKeysDown)
		{
			Penguin->ReleaseJump();
		}
		bJumpKeysDown = false;
		return;
	}

	// The key states of this frame (not the jump action: an action fires "started" only when the first of its keys goes
	// down, so a second jump key did nothing while another one was held - and a very short tap could slip through).
	int32 Presses = 0;
	bool bAnyDown = false;
	for (const FKey& Key : UPenguinInputConfig::GetJumpKeys())
	{
		Presses += WasInputKeyJustPressed(Key) ? 1 : 0;
		bAnyDown |= IsInputKeyDown(Key);
	}
	KeyJumpPresses += Presses;
	if (SlatePresses > Presses)
	{
		// Pressed on the keyboard, but a widget (or another window) got it instead of the game.
		KeyJumpPressesLost += SlatePresses - Presses;
		const float Now = GetWorld() ? GetWorld()->GetRealTimeSeconds() : 0.f;
		if (Now - LastLostKeyLogTime > 1.f && FSlateApplication::IsInitialized())
		{
			LastLostKeyLogTime = Now;
			const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(0);
			UE_LOG(LogPingu, Log, TEXT("[JUMPS] %d jump key press(es) did not reach the game: keyboard focus on %s"), SlatePresses - Presses,
				Focused.IsValid() ? *Focused->GetTypeAsString() : TEXT("nothing"));
		}
	}

	if (Presses > 0)
	{
		Penguin->PressJump();
		bJumpKeysDown = true;
	}
	if (!bAnyDown && bJumpKeysDown)
	{
		// Also right after a press in the same frame: the movement keeps a tap going long enough for a short hop.
		Penguin->ReleaseJump();
		bJumpKeysDown = false;
	}
	else if (bAnyDown)
	{
		bJumpKeysDown = true;
	}
}

void APenguinPlayerController::UpdateContinueKey()
{
	int32 Presses = 0;
	if (JumpKeyCounter.IsValid())
	{
		PenguinPlayerControllerPrivate::FJumpKeyCounter& Counter = static_cast<PenguinPlayerControllerPrivate::FJumpKeyCounter&>(*JumpKeyCounter);
		Presses = Counter.AnyPresses;
		Counter.AnyPresses = 0;
	}
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (Presses == 0 || bMenuMode || !GameState || GameState->GetPhase() != EPinguPhase::RoundOver || !GameState->IsWaitingForContinue()
		|| GameState->GetPhaseTime() < APenguinGameState::ContinueAfter)
	{
		return; // (presses before the board has been up a moment were still part of the fight)
	}
	if (HasAuthority())
	{
		Rq26_Implementation(); // the host (or solo): straight to the game mode
	}
	else
	{
		Rq26(); // (a joiner's is ignored by the host: the net test sends it to check that)
	}
}

void APenguinPlayerController::Rq26_Implementation()
{
	// Host-only (the audit's hole 6): the host decides when the leaderboard goes on; a joiner's request is ignored.
	if (!IsLocalController())
	{
		PinguAntiCheat::NoteIgnored(this, TEXT("a host-only request"), TEXT("skipping the leaderboard"));
		return;
	}
	if (APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr)
	{
		GameMode->RequestContinueRound(this);
	}
}

namespace PenguinWarpViewPrivate
{
	/** A LAN client sends its screen (for its Snow Warps) at most this often (s), when it moved more than this (cm) ... */
	constexpr float SendInterval = 0.05f;
	constexpr double SendChange = 10.0;
	/** ... and at least this often anyway (s): the RPC is unreliable. */
	constexpr float KeepAliveInterval = 0.5f;
	/** The server takes nothing bigger than this (cm across) or not finite. */
	constexpr double MaxViewSize = 200000.0;
}

bool APenguinPlayerController::GetWarpView(FBox2D& OutView) const
{
	if (IsLocalController())
	{
		const APenguinCamera* StageCamera = SharedCamera.Get() ? SharedCamera.Get() : APenguinCamera::FindOrSpawn(GetWorld());
		return StageCamera && StageCamera->GetViewRect(OutView);
	}
	if (!WarpView.bIsValid)
	{
		return false;
	}
	OutView = WarpView;
	return true;
}

void APenguinPlayerController::UpdateWarpView(float DeltaTime)
{
	using namespace PenguinWarpViewPrivate;
	// Only a LAN client: the host's and a solo player's own screen is right here (GetWarpView).
	if (GetNetMode() != NM_Client)
	{
		return;
	}
	WarpViewSendWait -= DeltaTime;
	WarpViewKeepAlive -= DeltaTime;
	FBox2D View(ForceInit);
	if (WarpViewSendWait > 0.f || !GetWarpView(View))
	{
		return;
	}
	const bool bMoved = !SentWarpView.bIsValid || (View.Min - SentWarpView.Min).GetAbsMax() > SendChange || (View.Max - SentWarpView.Max).GetAbsMax() > SendChange;
	if (!bMoved && WarpViewKeepAlive > 0.f)
	{
		return;
	}
	Rq27(View.Min, View.Max);
	SentWarpView = View;
	WarpViewSendWait = SendInterval;
	WarpViewKeepAlive = KeepAliveInterval;
}

void APenguinPlayerController::Rq27_Implementation(FVector2D ViewMin, FVector2D ViewMax)
{
	using namespace PenguinWarpViewPrivate;
	if (ViewMin.ContainsNaN() || ViewMax.ContainsNaN() || ViewMin.X >= ViewMax.X || ViewMin.Y >= ViewMax.Y
		|| ViewMax.X - ViewMin.X > MaxViewSize || ViewMax.Y - ViewMin.Y > MaxViewSize)
	{
		return;
	}
	WarpView = FBox2D(ViewMin, ViewMax);
}

void APenguinPlayerController::LogJumpStats(const FString& When)
{
	const APenguinCharacter* Penguin = Cast<APenguinCharacter>(GetPawn());
	UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	if (!Move)
	{
		return;
	}
	const FPenguinJumpStats& Stats = Move->GetJumpStats();
	UE_LOG(LogPingu, Display,
		TEXT("[JUMPS] %s: %d jump key presses reached the game (%d lost on the way), %d presses in all -> %d jumps (%d ground, %d coyote, %d near the ground, %d double, %d ledge grabs), %d dropped (%d stunned, %d frozen, %d double jump used up, %d other)"),
		*When, KeyJumpPresses, KeyJumpPressesLost, Stats.Presses, Stats.Jumps(), Stats.GroundJumps, Stats.CoyoteJumps, Stats.NearGroundJumps,
		Stats.AirJumps, Stats.Climbs, Stats.Dropped, Stats.DroppedStunned, Stats.DroppedFrozen, Stats.DroppedNoAirJump, Stats.DroppedOther);
	ResetJumpStats();
}

void APenguinPlayerController::ResetJumpStats()
{
	const APenguinCharacter* Penguin = Cast<APenguinCharacter>(GetPawn());
	if (UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr)
	{
		Move->ResetJumpStats();
	}
	KeyJumpPresses = 0;
	KeyJumpPressesLost = 0;
}

void APenguinPlayerController::RequestTeamChoice(int32 Team)
{
	if (HasAuthority())
	{
		Rq24_Implementation(Team); // the host (or solo): straight to the game mode
	}
	else
	{
		Rq24(Team);
	}
}

void APenguinPlayerController::Rq24_Implementation(int32 Team)
{
	if (!IsLocalController() && !PinguAntiCheat::AllowRequest(this, PinguAntiCheat::EGate::Lobby))
	{
		return;
	}
	if (APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr)
	{
		GameMode->SetTeamChoice(GetPlayerState<APenguinPlayerState>(), Team);
	}
}

void APenguinPlayerController::RequestTraits(bool bOn)
{
	if (HasAuthority())
	{
		Rq25_Implementation(bOn); // the host (or solo): straight to the game mode
	}
	else
	{
		Rq25(bOn); // (a joiner's is ignored by the host: the net test sends it to check that)
	}
}

void APenguinPlayerController::Rq25_Implementation(bool bOn)
{
	// Host-only (the audit's hole 6): TRAITS is the host's match setting; a joiner's request is ignored.
	if (!IsLocalController())
	{
		PinguAntiCheat::NoteIgnored(this, TEXT("a host-only request"), TEXT("the TRAITS setting"));
		return;
	}
	APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr;
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	// The next match's setting: changed in the lobby only (a client's late press during the match start does nothing).
	if (GameMode && GameState && GameState->GetPhase() == EPinguPhase::Lobby)
	{
		GameMode->SetTraitsEnabled(bOn);
	}
}

// ---------------------------------------------------------------------------- anti-cheat (PinguAntiCheat.cpp does the work)

void APenguinPlayerController::Rq18_Implementation(const FString& Nonce, const FString& BuildId)
{
	PinguAntiCheat::ClientOnHello(this, Nonce, BuildId);
}

void APenguinPlayerController::Rq19_Implementation(const FString& Answer, const FString& Nonce, const FString& BuildId)
{
	PinguAntiCheat::HostOnAnswer(this, Answer, Nonce, BuildId);
}

void APenguinPlayerController::DevCheatHostOnlyRequests()
{
#if !UE_BUILD_SHIPPING
	// -PinguCheat=settings: a joiner that flips the host's TRAITS setting and presses on through the leaderboard (the host ignores both).
	bDevCheatFlip = !bDevCheatFlip;
	Rq25(bDevCheatFlip);
	Rq26();
#endif
}

void APenguinPlayerController::Rq22_Implementation()
{
	if (UPinguNetSubsystem* Net = UPinguNetSubsystem::Get(this))
	{
		Net->LeaveWithError(FText::FromString(TEXT("The host kicked you.")));
	}
}

void APenguinPlayerController::Rq20_Implementation(const FString& Answer)
{
	PinguAntiCheat::ClientOnHostAnswer(this, Answer);
}

void APenguinPlayerController::Rq21_Implementation()
{
	PinguAntiCheat::ClientOnRefused(this);
}

void APenguinPlayerController::Rq23_Implementation(int32 Serial, const FString& Value)
{
	if (APinguAntiCheat* AntiCheat = APinguAntiCheat::Get(GetWorld()))
	{
		AntiCheat->HostReceive(this, Serial, Value.Left(64));
	}
}

APenguinHUD* APenguinPlayerController::GetMenuHUD() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const APlayerController* FirstLocal = GameInstance ? GameInstance->GetFirstLocalPlayerController(GetWorld()) : nullptr;
	return Cast<APenguinHUD>(FirstLocal ? FirstLocal->GetHUD() : GetHUD());
}

bool APenguinPlayerController::WantsMenuInput() const
{
	// The automated match test plays with bots only: its screens never take the input.
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (GameState && GameState->IsMatchTest())
	{
		return false;
	}
	const APenguinHUD* PenguinHUD = GetMenuHUD();
	return PenguinHUD && PenguinHUD->IsMenuOpen();
}

void APenguinPlayerController::UpdateInputMode()
{
	const bool bMenu = WantsMenuInput();
	const APenguinHUD* PenguinHUD = GetMenuHUD();
	const bool bIsland = !bMenu && PenguinHUD && PenguinHUD->IsLobbyIslandScreen();
	if (!bInputModeSet || bMenu != bMenuMode || bIsland != bLobbyIslandMode)
	{
		bLobbyIslandMode = bIsland;
		ApplyInputMode(bMenu);
	}
}

void APenguinPlayerController::ApplyInputMode(bool bMenu)
{
	bInputModeSet = true;
	bMenuMode = bMenu;
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Default;
	CurrentMouseCursor = EMouseCursor::Default;
	if (bMenu)
	{
		// The menu owns the keys: the viewport passes none to the game until it closes.
		FInputModeUIOnly Mode;
		if (const APenguinHUD* PenguinHUD = GetMenuHUD())
		{
			Mode.SetWidgetToFocus(PenguinHUD->GetFocusWidget());
		}
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		SetInputMode(Mode);
		// Keys held while a menu opens must not stay stuck down (their key up goes to the menu): the penguin stops, a
		// held jump ends. Each key works again once it has been let go.
		FlushPressedKeys();
		if (APenguinCharacter* Penguin = Cast<APenguinCharacter>(GetPawn()))
		{
			Penguin->ReleaseJump();
			Penguin->ApplyVirtualMoveInput(FVector2D::ZeroVector);
			// A size ray's key held into the menu: its flushed release must not shoot it (nor the hold use it).
			Penguin->CancelAbilityKeys();
		}
		bJumpKeysDown = false;
	}
	else
	{
		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(false);
		// The lobby island: the mouse stays free (no lock, no capture below), as in a menu.
		Mode.SetLockMouseToViewportBehavior(bLobbyIslandMode ? EMouseLockMode::DoNotLock : EMouseLockMode::LockAlways);
		// Keyboard focus straight back on the game, not left on the menu button (PLAY, RESUME) that was clicked.
		const ULocalPlayer* OwningLocalPlayer = GetLocalPlayer();
		if (OwningLocalPlayer && OwningLocalPlayer->ViewportClient)
		{
			Mode.SetWidgetToFocus(OwningLocalPlayer->ViewportClient->GetGameViewportWidget());
		}
		SetInputMode(Mode);
		if (bLobbyIslandMode && OwningLocalPlayer && OwningLocalPlayer->ViewportClient)
		{
			// Every click on the island goes through its Slate layer (SPinguMenu3DInput); the viewport never captures the mouse
			// (a capture would route the mouse past the layer and, in the editor, pin the cursor to the viewport).
			OwningLocalPlayer->ViewportClient->SetMouseCaptureMode(EMouseCaptureMode::NoCapture);
		}
		UE_LOG(LogPingu, Log, TEXT("[INPUT] game input mode%s"), bLobbyIslandMode ? TEXT(" (lobby island: no mouse capture or lock)") : TEXT(""));
	}
}

void APenguinPlayerController::UpdateAim()
{
	APenguinCharacter* Penguin = Cast<APenguinCharacter>(GetPawn());
	UWorld* World = GetWorld();
	// Never aim for a penguin the bot brain steers (the menu demo, the automated match and LAN tests): it aims itself.
	const UPenguinBotComponent* Bot = Penguin ? Penguin->GetBot() : nullptr;
	if (!Penguin || !World || bMenuMode || (Bot && Bot->IsBotEnabled()))
	{
		return;
	}
	// The LAN test's two windows are real windows: somebody's mouse resting over one must not take over the aims the test
	// scripts (the icicle check's "aiming left" turned right on all frames once).
	static const bool bNetTest = APinguNetTest::IsRequested();
	if (bNetTest)
	{
		return;
	}
	const float Now = World->GetRealTimeSeconds();
	float MouseX = 0.f;
	float MouseY = 0.f;
	const bool bHasMouse = GetMousePosition(MouseX, MouseY);
	if (bHasMouse && FVector2D::Distance(FVector2D(MouseX, MouseY), LastMouse) > 1.5f)
	{
		LastMouse = FVector2D(MouseX, MouseY);
		LastMouseMoveTime = Now;
	}

	// Any gamepad use counts, not only the right stick.
	static const FKey PadButtons[] = {
		EKeys::Gamepad_FaceButton_Bottom, EKeys::Gamepad_FaceButton_Right, EKeys::Gamepad_FaceButton_Left, EKeys::Gamepad_FaceButton_Top,
		EKeys::Gamepad_LeftShoulder, EKeys::Gamepad_RightShoulder, EKeys::Gamepad_LeftTrigger, EKeys::Gamepad_RightTrigger,
		EKeys::Gamepad_DPad_Left, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_DPad_Up, EKeys::Gamepad_DPad_Down };
	bool bPadUsed = FMath::Abs(GetInputAnalogKeyState(EKeys::Gamepad_LeftX)) > 0.25f || FMath::Abs(GetInputAnalogKeyState(EKeys::Gamepad_LeftY)) > 0.25f
		|| FMath::Abs(GetInputAnalogKeyState(EKeys::Gamepad_RightX)) > 0.25f || FMath::Abs(GetInputAnalogKeyState(EKeys::Gamepad_RightY)) > 0.25f;
	for (const FKey& Key : PadButtons)
	{
		if (bPadUsed)
		{
			break;
		}
		bPadUsed = IsInputKeyDown(Key);
	}
	if (bPadUsed)
	{
		LastGamepadTime = Now;
	}

	const bool bStickIsNewer = FMath::Max(Penguin->GetLastStickAimTime(), LastGamepadTime) > LastMouseMoveTime;
	if (bStickIsNewer)
	{
		const FVector2D Stick = Penguin->GetStickAim();
		const bool bStickPushed = Stick.SizeSquared() > 0.09f;
		const FVector2D Dir = bStickPushed ? Stick.GetSafeNormal() : FVector2D(Penguin->GetFacing(), 0.f);
		Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(Dir.X, 0.f, Dir.Y) * 500.f, bStickPushed);
		return;
	}
	FVector WorldLocation;
	FVector WorldDirection;
	if (bHasMouse && DeprojectMousePositionToWorld(WorldLocation, WorldDirection) && FMath::Abs(WorldDirection.Y) > 1e-4)
	{
		// Where the mouse ray meets the play plane (y = 0).
		const double T = -WorldLocation.Y / WorldDirection.Y;
		if (T > 0.0)
		{
			Penguin->SetAimPoint(WorldLocation + WorldDirection * T);
		}
	}
}

void APenguinPlayerController::Input_AddPlayer(const FInputActionValue& Value)
{
	const UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance || GameInstance->GetNumLocalPlayers() >= 4)
	{
		return;
	}
	// The new player uses the next gamepad (player 2 = second controller, ...).
	if (UGameplayStatics::CreatePlayer(this, -1, true))
	{
		UE_LOG(LogPingu, Display, TEXT("Added local player %d"), GameInstance->GetNumLocalPlayers());
	}
}

void APenguinPlayerController::Input_ToggleHelp(const FInputActionValue& Value)
{
	if (APenguinHUD* PenguinHUD = GetMenuHUD())
	{
		PenguinHUD->ToggleHelp();
	}
}

void APenguinPlayerController::Input_Pause(const FInputActionValue& Value)
{
	// Esc / P / Start in the game. The HUD decides: solo pauses, a LAN game opens the local menu (the match runs on).
	// Inside the menus the menu widgets handle these keys themselves.
	if (APenguinHUD* PenguinHUD = GetMenuHUD())
	{
		PenguinHUD->TogglePauseMenu();
	}
}
