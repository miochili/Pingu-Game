// Pingu - owns the Slate UI (menus, in-game HUD), its textures and the menu state: which screen shows, the data the
// screens read (replicated game state, LAN subsystem, settings) and the actions they trigger.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "PenguinGameState.h"
#include "PenguinTypes.h"
#include "PinguSettings.h"
#include "PinguTraitTable.h"
#include "Styling/SlateBrush.h"
#include "PenguinHUD.generated.h"

class APenguinCharacter;
class APenguinGameMode;
class APlayerState;
class SPinguRoot;
class SWidget;
class UGameViewportClient;
class UPinguNetSubsystem;
class UTexture2D;
struct FPinguLanGame;

/** Which full screen is showing (the in-game HUD shows under the ones that do not cover it). */
enum class EPinguScreen : uint8
{
	None,       // playing, no menu
	MainMenu,   // solo title screen
	Pause,      // solo: the game is paused; LAN: a local menu, the match keeps running
	Results,    // match over
	Controls,   // key bindings (from the settings or the pause menu)
	Lan,        // ONLINE: host / find / join a LAN game (from the title)
	Lobby,      // the lobby card: everybody's penguin (and the bots) on cards; solo (PLAY) a lobby of your own
	Settings,   // graphics, display, audio (from the title, the pause menu or the lobby)
	Pingupedia, // every ability and every trait with their numbers (from the title)
	TraitPicks, // the host's TRAITS list: which traits are in the round bag (from the lobby's PICK sign; 2026-11-07)
	Count,
};

/** A card of the lobby: a person (in join order, the host first) or one of the bots (after the people). */
struct FPinguLobbyPlayer
{
	FString Name;
	/** The colour / portrait it has (bots: the free slots after the people's, in order - as the match will give them). */
	int32 Slot = 0;
	bool bHost = false;
	bool bYou = false;
	/** The team picked on the card (-1: none - on their own). */
	int32 TeamChoice = -1;
	/** Joined during a match: plays from its next round. */
	bool bWaiting = false;
	/** A person's player id (APlayerState::GetPlayerId; INDEX_NONE for bots): what the host's kick x names. */
	int32 PlayerId = INDEX_NONE;
	/** A bot: its place in the match settings' bots and its skill (0 easy, 1 normal, 2 hard). */
	bool bBot = false;
	int32 BotIndex = INDEX_NONE;
	int32 Skill = 1;
};

UCLASS()
class PENGUIN_API APenguinHUD : public AHUD
{
	GENERATED_BODY()

public:
	APenguinHUD();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void DrawHUD() override;

	/** Keyboard / gamepad focus target for the current menu: always a widget that takes the keyboard focus whenever
	 *  IsMenuOpen() (null while playing). */
	TSharedPtr<SWidget> GetFocusWidget() const;
	/** CONTRACT (v2): a full-screen menu owns the input right now (title, lobby, pause, results, settings, controls,
	 *  LAN screens): the player controller switches to UI input and the penguin gets no input. */
	bool IsMenuOpen() const;
	/** The lobby island's screen shows (the lobby without -PinguOldMenus): no cards, no menu input - the penguins run around
	 *  on the island under it (APinguLobby). */
	bool IsLobbyIslandScreen() const;
	/** Tests: this screen shows whatever the phase says (None: back to the phase's own). The title's 3D input layer is only
	 *  there while the title screen shows (clicks_Menu3D). */
	void SetTestScreen(EPinguScreen InScreen);
	/** The lobby island's 3D button strip has the keys (Esc / gamepad Start on the island; Esc / B again goes back): the strip is
	 *  a menu then (arrows / d-pad move along it, Enter / A press) and the penguin gets no input. Ends with the lobby. */
	bool IsLobbyStripFocused() const;
	void SetLobbyStripFocus(bool bOn);
	/** CONTRACT (v2): Esc / P / Start. Solo: pauses the game (the world stops). LAN: opens or closes the local pause
	 *  menu, the match keeps running. Also backs out of sub-screens (settings, controls). */
	void TogglePauseMenu();
	/** The controls help (H / F1). It is only ever opened by the player. */
	void ToggleHelp();

	// ------------------------------------------------------------------ data for the widgets
	/** The replicated match state (every machine). Null for a moment on a joining client. */
	APenguinGameState* GetGameState() const;
	/** The server's game mode: solo and the LAN host only (null on clients). Only the host's actions use it. */
	APenguinGameMode* GetPenguinGameMode() const;
	APenguinCharacter* GetHumanPenguin() const;
	const FSlateBrush* GetAbilityBrush(EPenguinAbility Ability) const;
	/** A trait's badge (/Game/UI/Traits/T_Trait_<IconName>); draws nothing for None or a missing texture. */
	const FSlateBrush* GetTraitBrush(EPinguTrait Trait) const;
	/** A slot's portrait (T_Portrait_N: the face, the beanie left out) and its beanie layer (T_PortraitFelt_N: white, drawn
	 *  over it tinted with APenguinCharacter::GetBeanieTint - the slot's or the team's colour). Blender renders
	 *  (Tools/ArtGen/render_portraits.py).
	 *  DrawSize (the portrait's drawn size, px / menu units; 0 = big): under PortraitSmallBelow the head crop
	 *  (T_PortraitSmall_N / T_PortraitSmallFelt_N: head, hat and face detail, no flippers) so small faces stay readable. */
	const FSlateBrush* GetPortraitBrush(int32 Slot, float DrawSize = 0.f) const;
	const FSlateBrush* GetPortraitFeltBrush(int32 Slot, float DrawSize = 0.f) const;
	static constexpr float PortraitSmallBelow = 56.f;
	EPinguScreen GetScreen() const { return Screen; }
	/** 0 -> 1 (ease-out) over the first moments of the screen showing now. */
	float GetScreenAppear() const;
	/** The darkening behind a menu fades in only when it comes from the game (menu to menu it stays). */
	float GetDimAppear() const;
	/** Menus that darken the game behind them. */
	static bool ScreenDims(EPinguScreen InScreen);
	bool IsHelpVisible() const { return bShowHelp; }
	float GetUITime() const { return UITime; }

	EPinguPhase GetPhase() const;
	/** Seconds since the phase began (replicated server time). */
	float GetPhaseTime() const;
	/** Hosting or joined a LAN game. */
	bool IsNetworkGame() const;
	/** Solo or the LAN host: this machine runs the match (the menus may change it). */
	bool HasMatchAuthority() const;
	bool IsMatchTest() const;
	/** The match settings everybody sees (bots, difficulty, rounds, endless, teams). */
	FPinguMatchSettings GetMatchSettings() const;
	/** A team match is going on (the results and boards show teams). */
	bool IsTeamMatch() const;
	/** Traits mode is on (the match settings: the lobby's toggle; every machine). */
	bool AreTraitsOn() const;
	/** Team match: rounds a team won; the teams best first; the winning team of the match / of the last round (-1 none). */
	int32 GetTeamWinCount(int32 Team) const;
	TArray<int32> GetTeamBoard() const;
	int32 GetMatchWinnerTeam() const;
	int32 GetRoundWinnerTeam() const;
	/** The colour index a score row shows: its own (everybody keeps their colour in a team match too). */
	int32 GetScoreColorIndex(const FPinguScore& Score) const;
	/** The team ring a score row's portrait gets: its team in a team match - when it has two penguins or more (alone,
	 *  nobody's teammate: no ring) - else -1. */
	int32 GetScoreTeamRing(const FPinguScore& Score) const;
	/** The team ring a penguin's icons above its head (and its off-screen marker) get: APenguinGameState::GetShownTeam of its
	 *  team (-1: none - not a team match, or on its own). */
	int32 GetPenguinTeamRing(const APenguinCharacter* Penguin) const;
	/** What the banner says now ("ROUND 2", "FROSTY WINS!", ...) and the results' title (the lobby test reads them). */
	FText GetBannerText() const;
	FText GetResultsTitle() const;
	/** What the icons above the heads drew in their last paint (the lobby test reads it): the penguins whose icons got their
	 *  team's rings (a ring around each icon), how many rings, the biggest ring's diameter against its icon's and the least
	 *  room between an icon's edge and its ring. False when they have not been drawn in the last few frames. */
	bool GetOverheadTeamRings(TArray<FString>& OutRinged, int32& OutRings, float& OutRingToIcon, float& OutClearance) const;
	/** The PINGUPEDIA (the tests read it): the page that shows (0 abilities, 1 traits) and its selected entry; true when that
	 *  entry's row is wholly in the list's view. */
	bool GetPingupediaView(int32& OutPage, int32& OutEntry) const;
	/** The PINGUPEDIA (tests): shows Page with Entry selected; bFocus moves the keyboard focus to its row. */
	void SelectPingupediaPage(int32 Page, int32 Entry, bool bFocus);
	/** RoundOver: the next round waits for a key and the leaderboard has been up long enough to take one. */
	bool IsWaitingForContinueKey() const;
	/** Team match: the score row of a team's only penguin (somebody on their own: a team of one), null for a real team. */
	const FPinguScore* GetLoneTeamMember(int32 Team) const;
	/** The scores the screens show (made-up ones for -PinguMenuShots). */
	const TArray<FPinguScore>& GetScores() const;
	/** Indices into GetScores(), best first. */
	TArray<int32> GetLeaderboard() const;
	int32 GetRoundWinnerSlot() const;
	int32 GetMatchWinnerSlot() const;
	/** The score row of a slot, null if there is none (a player who left leaves a gap: a slot is not an index). */
	const FPinguScore* FindScoreBySlot(int32 Slot) const;
	/** This PC's player (solo: the human). */
	bool IsLocalScore(const FPinguScore& Score) const;
	APlayerState* GetLocalPlayerState() const;
	/** The lobby's people in join order (host first). */
	TArray<FPinguLobbyPlayer> GetLobbyPlayers() const;
	/** The lobby's cards: the people, then the bots (the match settings' bots, as many as fit). */
	TArray<FPinguLobbyPlayer> GetLobbyCards() const;
	FString GetHostName() const;
	/** The most bots the lobby allows now (humans + bots <= 10; solo 1..9). */
	int32 GetMaxBots() const;
	int32 GetMinBots() const;
	/** The bots the next match will have (the setting, kept inside the limits above). */
	int32 GetBotCount() const;
	/** The lobby's + card: this machine runs the match and there is room for one more penguin. */
	bool CanAddBot() const;
	/** A bot's card has its X: this machine runs the match and the match keeps its fewest bots without it (solo: one). */
	bool CanRemoveBot() const;
	/** The local player's team, picked on their card (-1: none). */
	int32 GetLocalTeamChoice() const;
	/** This PC's player joined during a match and waits for the next round. */
	bool IsLocalWaitingForRound() const;

	UPinguNetSubsystem* GetNet() const;
	bool IsConnecting() const;
	/** Games found on the LAN (made-up ones for -PinguMenuShots). */
	const TArray<FPinguLanGame>& GetFoundGames() const;
	bool IsSearching() const;
	/** What the LAN subsystem says went wrong (empty if fine). */
	FText GetNetError() const;
	bool HasErrorPopup() const;
	/** What we are joining (for "Connecting to ..."). */
	FString GetJoinTarget() const { return JoinTarget; }
	/** "192.168.1.20" (+ ":port" when not the default) for the lobby's "others join with". */
	FString GetHostAddressText() const;
	/** The lobby's address line was clicked (or confirmed) a moment ago: it says COPIED! (CopiedFeedbackTime seconds). */
	bool IsHostAddressCopied() const;
	static constexpr double CopiedFeedbackTime = 1.5;
	/** The system clipboard's text: put Text there (true once it is read back as Text: the clipboard can be busy for a moment),
	 *  and read it (empty when it holds no text). The copy line and the tests that check it use these. */
	static bool CopyTextToClipboard(const FString& Text);
	static FString ReadClipboardText();
	/** The name the LAN shows for us. */
	FString GetPlayerName() const;

	// key rebinding (controls screen)
	bool IsCapturingKey() const { return bCapturing; }
	bool IsCapturingKey(EPinguInputAction Action, int32 Index) const;
	void BeginKeyCapture(EPinguInputAction Action, int32 Index);
	/** A key or mouse button pressed while capturing: bound, cleared (Backspace / Delete), cancelled (Escape) or refused. */
	void HandleCapturedKey(const FKey& Key);
	void CancelKeyCapture();
	/** "Press a key for JUMP", or why the last key was refused. */
	FText GetCaptureMessage() const;

	// ------------------------------------------------------------------ actions from the widgets
	/** Title screen: PLAY opens a lobby of your own (you and your bots; nobody else can join). */
	void Action_Play();
	void Action_Controls();
	void Action_CloseControls();
	void Action_Settings();
	/** The settings page's APPLY (2026-11-07): saves what the page changed (the window mode / resolution go to the window). */
	void Action_ApplySettings();
	/** The settings page has changes APPLY has not saved yet (the 3D menus only: the old Slate menus apply at once). */
	bool HasSettingsPending() const;
	/** The "Apply changes?" board shows over the settings page (BACK / Esc / B with changes pending): APPLY saves and leaves,
	 *  DISCARD puts the old values back and leaves, Esc / B (CloseConfirm) goes back into the settings. */
	bool HasSettingsConfirm() const;
	void Action_ConfirmApply();
	void Action_ConfirmDiscard();
	void Action_CloseConfirm();
	/** Title screen: ONLINE (host a LAN game others can join, or join one). */
	void Action_Lan();
	/** Title screen: the PINGUPEDIA (Esc / B / BACK leave it again). */
	void Action_Pingupedia();
	/** The lobby's TRAITS ON / OFF (the next match): the host / solo player switches it (the match settings replicate). A
	 *  LAN client only sees the host's choice: this does nothing there (the host decides). */
	void Action_ToggleTraits();
	/** The host's TRAITS list (2026-11-07, shared-traits; PenguinHUDTraits.cpp): the lobby's PICK sign opens it (host / solo,
	 *  lobby only); a trait ticked (in the round bag) or not; ticking / unticking one (the last ticked one stays), all of them. */
	void Action_TraitPicks();
	bool IsTraitPicked(EPinguTrait Trait) const;
	int32 GetTraitPickCount() const;
	void Action_ToggleTraitPick(EPinguTrait Trait);
	void Action_PickAllTraits();
	/** ABILITIES RANDOM / CHOOSE (2026-11-07, PinguAbilityPicks.h; PenguinHUDPicks.cpp): the setting everybody sees; the host /
	 *  solo player flips it in the lobby (saved); your own picks (3 entries, None = random) and picking one for a slot (None: back
	 *  to random; an ability another of your slots holds is refused). */
	bool IsChooseAbilitiesOn() const;
	void Action_ToggleChooseAbilities();
	TArray<EPenguinAbility> GetOwnPicks() const;
	bool Action_SetPick(int32 Slot, EPenguinAbility Ability);
	/** The lobby's "Others join with ..." line: puts exactly the address it shows (GetHostAddressText: what a friend types under
	 *  JOIN BY ADDRESS) on the system clipboard, and the line says COPIED! for CopiedFeedbackTime seconds. Local to this PC. */
	void Action_CopyHostAddress();
	/** Leaves the screen on top (settings, controls, LAN); on the LAN screen while joining: cancels the join. */
	void Action_Back();
	void Action_Quit();
	void Action_Resume();
	void Action_Restart();
	void Action_MainMenu();
	void Action_Rematch();
	/** The pause menu has END MATCH: solo / the LAN host, during a match. */
	bool CanEndMatch() const;
	/** END MATCH: the results now (the most round wins win; APenguinGameMode::EndMatchNow). */
	void Action_EndMatch();
	/** The lobby's + card: one more bot (the last bot's skill, no team). */
	void Action_AddBot();
	/** A bot card's X. */
	void Action_RemoveBot(int32 BotIndex);
	/** The host's x on a joiner's card shows: LAN host, a person who is not you (nor the host's own card), not a bot. */
	bool CanKick(const FPinguLobbyPlayer& Card) const;
	/** The joiner's card x (the host): sends that player away (APenguinGameMode::KickPlayer). Nothing for anyone else. */
	void Action_KickPlayer(int32 PlayerId);
	/** The people the host can kick now, in lobby order (the pause menu lists them as KICK buttons during a match). Empty for
	 *  joiners and in solo games. */
	TArray<FPinguLobbyPlayer> GetKickTargets() const;
	/** A bot's card (the host): its team -1 / +1 (NO TEAM, RED, BLUE, ... BROWN, round and round). */
	void Action_ChangeBotTeam(int32 BotIndex, int32 Delta);
	/** A bot card's skill: EASY, NORMAL, HARD (round and round). */
	void Action_ChangeBotSkill(int32 BotIndex, int32 Delta);
	/** Your own card: your team -1 / +1 (NO TEAM, RED, ... BROWN, round and round; on a LAN client it goes to the host). */
	void Action_ChangeTeamChoice(int32 Delta, bool bWrap);
	// LAN
	void Action_HostGame();
	void Action_JoinGame(const FString& Address);
	void Action_JoinFound(const FString& Address);
	void Action_CancelJoin();
	void Action_LeaveGame();
	void Action_SetPlayerName(const FString& Name);
	void Action_StartLobbyMatch();
	void Action_ReturnToLobby();
	/** Lobby: close the lobby card and run around on the island (Esc opens it again). */
	void Action_WarmUp();
	void Action_DismissError();
	void Action_ResetKeys();
	void PlayUISound(bool bClick);
	void PlayBackSound();

	/** Debug read-out (movement numbers) in the corner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUD")
	bool bShowDebug = false;

private:
	void LoadTextures();
	void UpdateScreen();
	void UpdateFocus();
	EPinguScreen ComputeBaseScreen() const;
	void PushScreen(EPinguScreen NewScreen);
	void PopScreen();
	/** The HUD that owns the UI (the first local player's); other local players' HUDs forward to it. */
	const APenguinHUD* GetPrimaryHUD() const;
	APenguinHUD* GetPrimaryHUD();
	void ApplyMatchSettings(const FPinguMatchSettings& NewSettings);
	void TickMenuShots(float RealDeltaSeconds);
	/** -PinguMenuShots: the world behind the next picture - the lobby island for the lobby shots, the title's demo otherwise. */
	void PrepareMenuShotWorld();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> Textures;

	TMap<EPenguinAbility, FSlateBrush> AbilityBrushes;
	TMap<EPinguTrait, FSlateBrush> TraitBrushes;
	TArray<FSlateBrush> PortraitBrushes;
	TArray<FSlateBrush> PortraitFeltBrushes;
	TArray<FSlateBrush> SmallPortraitBrushes;
	TArray<FSlateBrush> SmallPortraitFeltBrushes;
	FSlateBrush EmptyBrush;

	TSharedPtr<SPinguRoot> Root;
	TSharedPtr<SWidget> RootContainer;
	/** The viewport the UI was added to (this world's own: several PIE instances each have one). */
	TWeakObjectPtr<UGameViewportClient> RootViewport;

	/** What shows now: the top of the stack, else the base screen (from the game phase). */
	EPinguScreen Screen = EPinguScreen::None;
	EPinguScreen BaseScreen = EPinguScreen::None;
	/** When the screen last changed (Slate time) and whether the one before darkened the game too. */
	double ScreenChangedAt = -10.0;
	bool bPrevScreenDimmed = false;
	/** Sub-screens opened on top of the base screen (LAN, settings, controls). */
	TArray<EPinguScreen> ScreenStack;
	/** LAN games: the local pause menu is open (the match goes on). */
	bool bLocalMenuOpen = false;
	/** LAN lobby: the lobby card is open (else the player runs around on the island). */
	bool bLobbyMenuOpen = true;
	/** The lobby island: its 3D button strip has the keys (IsLobbyStripFocused). */
	bool bLobbyStripFocus = false;
	bool bShowHelp = false;
	float UITime = 0.f;

	/** Where the focus was on each screen (to come back to it from a sub-screen). */
	TArray<TWeakPtr<SWidget>> ScreenFocus;
	/** Focus state (screen, popup, key capture) the focus was last set for. */
	uint32 FocusedStateKey = 0xFFFFFFFFu;

	/** The "Apply changes?" board is up (the primary HUD's). */
	bool bSettingsConfirm = false;

	// key capture
	bool bCapturing = false;
	EPinguInputAction CaptureAction = EPinguInputAction::Jump;
	int32 CaptureIndex = 0;
	FText CaptureNote;
	double CaptureNoteTime = -10.0;
	double CaptureStartTime = -10.0;

	FString JoinTarget;
	/** When the address line was last copied (Slate time). */
	double HostAddressCopiedAt = -100.0;

	// -PinguMenuShots: every screen in turn, a screenshot of each, then quit
	bool bMenuShots = false;
	int32 ShotIndex = -1;
	float ShotClock = 0.f;
	float ShotTimer = 0.f;
	bool bShotTaken = false;
	EPinguScreen ShotScreen = EPinguScreen::None;
	/** SetTestScreen is on (ShotScreen shows whatever the phase says). */
	bool bTestScreen = false;
	bool bShotError = false;
	bool bShotClient = false;
	/** 1: the big variant of a screen (ten players, teams); 2: solo (the lobby of your own). */
	int32 ShotVariant = 0;
	/** The lobby screenshot with TRAITS ON. */
	bool bShotTraits = false;
	/** After the PINGUPEDIA's traits picture: real keys sent like a player's (TickPingupediaKeys) - the next one, and how
	 *  many landed where they should. */
	int32 PediaKeyStep = 0;
	int32 PediaKeysOk = 0;
	FString PediaKeysLog;
	void TickPingupediaKeys();
	/** The "lobby_copied" picture: shortly before it, the address line is used like a player would - the action, then a real
	 *  Enter on the focused line - and the clipboard is read back (the player's own clipboard text is put back after). */
	bool bShotCopyDone = false;
	void CheckCopyAddressShot();
};
