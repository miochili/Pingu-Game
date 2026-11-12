// Pingu - player controller: rebindable input mapping, jump keys, mouse / stick aim, menus and pause, shared camera.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "PenguinPlayerController.generated.h"

class APenguinCamera;
class APenguinHUD;
class IInputProcessor;
class UPinguSettings;
struct FInputActionValue;

/**
 * Everything here is for the player at this PC: on a listen server, the controllers of the remote players do nothing
 * local (no input mapping, no view target, no menus, no settings) - their machines do that themselves.
 */
UCLASS()
class PENGUIN_API APenguinPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	APenguinPlayerController();

	virtual void PlayerTick(float DeltaTime) override;

	/** True while a menu owns the input (APenguinHUD::IsMenuOpen: title, lobby, pause, results, settings, controls, ...). */
	bool IsInMenuMode() const { return bMenuMode; }

	/** Logs what became of this player's jump presses since the last call (keys that reached the game or got lost before
	 *  it, jumps by kind, dropped presses and why), then starts counting again. The game mode calls it after every round. */
	void LogJumpStats(const FString& When);
	/** Starts counting from zero (at GO). */
	void ResetJumpStats();

	/** The lobby's YOUR TEAM: the team this player wants in team matches (-1 = whichever needs a player). On a LAN client
	 *  it goes to the host; the next match uses it. */
	void RequestTeamChoice(int32 Team);
	/** The lobby's TRAITS ON / OFF (APenguinHUD::Action_ToggleTraits). The host / solo: at once; a LAN client's goes to the
	 *  host, which takes it in the lobby only. */
	void RequestTraits(bool bOn);

	/** Dev builds, -PinguCheat=settings (PinguAntiCheat.cpp): a joiner's TRAITS flip and leaderboard skip, sent at the host. */
	void DevCheatHostOnlyRequests();

	/** Snow Warp: what this player's screen shows of the play plane (Min/Max.X: world X, Min/Max.Y: world Z). This machine's
	 *  player: from the stage camera (APenguinCamera::GetViewRect); a LAN client's on the server: as its machine last sent
	 *  it. False while not known. */
	bool GetWarpView(FBox2D& OutView) const;

	// ------------------------------------------------------------------ anti-cheat (PinguAntiCheat.h): the join check, fair picks
	/** Host -> joiner: our nonce and build id; the joiner answers with a hash of its game files salted with the nonce. */
	UFUNCTION(Client, Reliable)
	void Rq18(const FString& Nonce, const FString& BuildId);
	/** Joiner -> host: its answer, a nonce of its own for the host, its build id. */
	UFUNCTION(Server, Reliable)
	void Rq19(const FString& Answer, const FString& Nonce, const FString& BuildId);
	/** Host -> joiner: the host's answer to the joiner's nonce (the joiner checks it). */
	UFUNCTION(Client, Reliable)
	void Rq20(const FString& Answer);
	/** Host -> joiner: the games don't match (the joiner leaves; the host drops it a moment later anyway). */
	UFUNCTION(Client, Reliable)
	void Rq21();
	/** Host -> joiner: the host removed you (APenguinGameMode::KickPlayer): leave with "The host kicked you.". */
	UFUNCTION(Client, Reliable)
	void Rq22();
	/** Joiner -> host: its random value for the round seed of commit Serial (APinguAntiCheat::HostReceive). */
	UFUNCTION(Server, Reliable)
	void Rq23(int32 Serial, const FString& Value);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void SetupInputComponent() override;

	UFUNCTION(Server, Reliable)
	void Rq24(int32 Team);

	/** A LAN client toggled TRAITS in the lobby: the host's setting, so the host ignores it (kept for older clients). */
	UFUNCTION(Server, Reliable)
	void Rq25(bool bOn);

	/** A LAN client pressed a key on the leaderboard: the host decides when it goes on, so the host ignores it (kept for older clients). */
	UFUNCTION(Server, Reliable)
	void Rq26();

	/** A LAN client's screen on the play plane (GetWarpView), sent while it changes: the server keeps the exits of this
	 *  player's Snow Warps inside it (APinguPortal::ClampWarpMove). */
	UFUNCTION(Server, Unreliable)
	void Rq27(FVector2D ViewMin, FVector2D ViewMax);

private:
	bool bDevCheatFlip = false;
	/** Looks through the stage camera (only the local player's controller: the camera exists on every machine for itself). */
	void ViewSharedCamera();
	/** The first local player of this world: it owns the keyboard counter and applies the saved settings at startup. */
	bool IsFirstLocalController() const;
	/** First local controller, once per world start: graphics auto-detection (first start only), graphics and audio. */
	void ApplyStartupSettings();

	/** Puts the current key bindings into the mapping context and makes this player's Enhanced Input use them (from the
	 *  next frame on). Cheap when nothing changed. */
	void RefreshInputMappings();
	void HandleBindingsChanged();

	/** All local players share one screen: the menus belong to the first local player's HUD. */
	APenguinHUD* GetMenuHUD() const;
	/** A menu owns the input right now (from the HUD; never in the automated match test). */
	bool WantsMenuInput() const;
	void UpdateInputMode();
	void ApplyInputMode(bool bMenu);

	void UpdateAim();
	/** Reads the jump keys: every new press of any of them is a jump (also while another one is still held, also a press and
	 *  release inside one frame); letting go of all of them releases the jump (short hop). */
	void UpdateJumpKeys();
	/** The leaderboard between rounds stays until somebody presses a key: any key or mouse button (but the menu and help
	 *  keys) sends us on to the next round - once the board has been up a moment (APenguinGameState::ContinueAfter). */
	void UpdateContinueKey();
	/** A LAN client: sends its screen on the play plane to the server while it changes (Rq27). */
	void UpdateWarpView(float DeltaTime);
	void Input_AddPlayer(const FInputActionValue& Value);
	void Input_ToggleHelp(const FInputActionValue& Value);
	void Input_Pause(const FInputActionValue& Value);

	bool bMenuMode = false;
	/** The lobby island shows (no menu): the mouse is never captured or locked by the viewport there (the island's Slate layer
	 *  takes every click: SPinguMenu3DInput), the cursor always shows. */
	bool bLobbyIslandMode = false;
	bool bInputModeSet = false;
	FVector2D LastMouse = FVector2D::ZeroVector;
	float LastMouseMoveTime = -100.f;
	/** Last time any gamepad stick or button was used (a pad player who never touches the right stick aims where he faces). */
	float LastGamepadTime = -200.f;

	TWeakObjectPtr<APenguinCamera> SharedCamera;

	/** The settings whose OnBindingsChanged we listen to. */
	TWeakObjectPtr<UPinguSettings> BindingsSource;
	FDelegateHandle BindingsChangedHandle;
	/** UPenguinInputConfig::GetMappingVersion() this player's Enhanced Input last got. */
	int32 AppliedMappingVersion = -1;

	bool bJumpKeysDown = false;
	/** Jump key presses that reached the game / that Slate got from the OS but never reached it (since LogJumpStats). */
	int32 KeyJumpPresses = 0;
	int32 KeyJumpPressesLost = 0;
	float LastLostKeyLogTime = -100.f;
	/** Counts the jump key presses as Slate gets them, before any widget can take them. */
	TSharedPtr<IInputProcessor> JumpKeyCounter;

	/** Server: a LAN client's screen (Rq27). LAN client: the last one sent, and when the next may go. */
	FBox2D WarpView = FBox2D(ForceInit);
	FBox2D SentWarpView = FBox2D(ForceInit);
	float WarpViewSendWait = 0.f;
	float WarpViewKeepAlive = 0.f;
};
