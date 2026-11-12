// Pingu - input actions and key bindings, built in code (no input assets needed).

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "UObject/Object.h"
#include "PenguinInputConfig.generated.h"

class UInputAction;
class UInputMappingContext;

/**
 * Creates the Enhanced Input actions + mapping context at runtime. The keyboard / mouse keys come from the player's
 * bindings (UPinguSettings, rebindable on the controls screen), the gamepad layout from UPinguSettings::GetGamepadKeys.
 *
 * Default keys (UPinguSettings): A / D or the arrows move, W / Up aim up (8-way dash up - they do NOT jump), S / Down
 *           fast fall in the air, Space jumps, Shift dashes, Ctrl belly slides, the mouse aims, the mouse buttons fire
 *           the three abilities (holding a shrink / grow ray's button uses it on yourself: APenguinCharacter).
 * Always:   Esc pauses (and backs out of menus); H / F1 help and R respawn (practice) unless the player gave one of
 *           those keys to an action.
 * Gamepad:  left stick / d-pad move (down in the air: fast fall), right stick aims, A jumps, LB / LT belly slide, the
 *           rest as UPinguSettings::GetGamepadKeys says, Back respawns (practice).
 *
 * The action objects never change (the character and the controller bind them once). When the bindings change,
 * RebuildMappings() swaps the keys inside the same mapping context.
 */
UCLASS()
class PENGUIN_API UPenguinInputConfig : public UObject
{
	GENERATED_BODY()

public:
	static UPenguinInputConfig* Get();

	/** The jump keys: the keys bound to Jump (Space by default; W only if the player binds it) + the gamepad's jump
	 *  button. The player controller reads them directly: every new press of any of them is a jump, even while another
	 *  jump key is still held, and even a press and release within one frame. */
	static const TArray<FKey>& GetJumpKeys();
	static bool IsJumpKey(const FKey& Key);
	/** -PinguOldInput (input test only): jumps through the Enhanced Input jump action, the way it was before. */
	static bool UseOldJumpInput();

	/** Puts the current bindings into Context, in place (the same context object stays applied to the players). Does
	 *  nothing when no key changed. True if keys changed: every player's Enhanced Input subsystem must then rebuild its
	 *  control mappings (APenguinPlayerController does that when UPinguSettings::OnBindingsChanged fires). */
	bool RebuildMappings();
	/** Goes up with every rebuild that changed keys. */
	int32 GetMappingVersion() const { return MappingVersion; }

	UPROPERTY() TObjectPtr<UInputMappingContext> Context;
	UPROPERTY() TObjectPtr<UInputAction> Move;
	UPROPERTY() TObjectPtr<UInputAction> Jump;
	UPROPERTY() TObjectPtr<UInputAction> Dash;
	UPROPERTY() TObjectPtr<UInputAction> Respawn;
	UPROPERTY() TObjectPtr<UInputAction> AddPlayer;
	UPROPERTY() TObjectPtr<UInputAction> ToggleHelp;
	UPROPERTY() TObjectPtr<UInputAction> Ability1;
	UPROPERTY() TObjectPtr<UInputAction> Ability2;
	UPROPERTY() TObjectPtr<UInputAction> Ability3;
	UPROPERTY() TObjectPtr<UInputAction> Aim;
	UPROPERTY() TObjectPtr<UInputAction> Pause;
	UPROPERTY() TObjectPtr<UInputAction> BellySlide;

private:
	void CreateActions();

	TArray<FKey> JumpKeys;
	/** Every key of the last build in readable form (also logged): a rebuild with the same keys is skipped. */
	FString BuiltKeys;
	int32 MappingVersion = 0;
};
