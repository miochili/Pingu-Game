// Pingu - input actions and key bindings, built in code.

#include "PenguinInputConfig.h"
#include "Penguin.h"
#include "PinguSettings.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"

namespace PenguinInputConfigPrivate
{
	UPenguinInputConfig* GPenguinInputConfig = nullptr;

	constexpr int32 NumBindable = static_cast<int32>(EPinguInputAction::Count);

	bool IsMoveAction(EPinguInputAction Action)
	{
		return Action == EPinguInputAction::MoveLeft || Action == EPinguInputAction::MoveRight
			|| Action == EPinguInputAction::Up || Action == EPinguInputAction::Down;
	}

	/** The sticks are mapped as analog 2D axes (left: move, right: aim), never again as digital move keys. */
	bool IsStickKey(const FKey& Key)
	{
		return Key == EKeys::Gamepad_Left2D || Key == EKeys::Gamepad_LeftX || Key == EKeys::Gamepad_LeftY
			|| Key == EKeys::Gamepad_LeftStick_Up || Key == EKeys::Gamepad_LeftStick_Down
			|| Key == EKeys::Gamepad_LeftStick_Left || Key == EKeys::Gamepad_LeftStick_Right
			|| Key == EKeys::Gamepad_Right2D || Key == EKeys::Gamepad_RightX || Key == EKeys::Gamepad_RightY
			|| Key == EKeys::Gamepad_RightStick_Up || Key == EKeys::Gamepad_RightStick_Down
			|| Key == EKeys::Gamepad_RightStick_Left || Key == EKeys::Gamepad_RightStick_Right;
	}

	/** Every key of an action: the player's keyboard / mouse keys first, then the gamepad's. */
	TArray<FKey> GatherKeys(const UPinguSettings* Settings, EPinguInputAction Action)
	{
		TArray<FKey> Keys;
		if (Settings)
		{
			for (const FKey& Key : Settings->GetKeys(Action))
			{
				// Escape always pauses and backs out of menus: it never does anything else on top.
				if (Key.IsValid() && Key != EKeys::Escape)
				{
					Keys.AddUnique(Key);
				}
			}
		}
		for (const FKey& Key : UPinguSettings::GetGamepadKeys(Action))
		{
			if (Key.IsValid() && !(IsMoveAction(Action) && IsStickKey(Key)))
			{
				Keys.AddUnique(Key);
			}
		}
		return Keys;
	}

	UInputAction* MakeAction(UObject* Outer, const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(Outer, FName(Name), RF_Transient);
		Action->ValueType = Type;
		// No action may swallow its keys (a key the player moves to another action must never be eaten on the way).
		Action->bConsumeInput = false;
		return Action;
	}

	FString DescribeKeys(const TArray<FKey>& Keys)
	{
		FString Text;
		for (const FKey& Key : Keys)
		{
			Text += TEXT(" ") + Key.ToString();
		}
		return Text.IsEmpty() ? FString(TEXT(" -")) : Text;
	}
}

UPenguinInputConfig* UPenguinInputConfig::Get()
{
	using namespace PenguinInputConfigPrivate;
	if (!GPenguinInputConfig || !IsValid(GPenguinInputConfig))
	{
		GPenguinInputConfig = NewObject<UPenguinInputConfig>(GetTransientPackage(), NAME_None, RF_Transient);
		GPenguinInputConfig->AddToRoot();
		GPenguinInputConfig->CreateActions();
		GPenguinInputConfig->RebuildMappings();
	}
	return GPenguinInputConfig;
}

const TArray<FKey>& UPenguinInputConfig::GetJumpKeys()
{
	return Get()->JumpKeys;
}

bool UPenguinInputConfig::IsJumpKey(const FKey& Key)
{
	return GetJumpKeys().Contains(Key);
}

bool UPenguinInputConfig::UseOldJumpInput()
{
	static const bool bOld = FParse::Param(FCommandLine::Get(), TEXT("PinguOldInput"));
	return bOld;
}

void UPenguinInputConfig::CreateActions()
{
	using PenguinInputConfigPrivate::MakeAction;
	Move = MakeAction(this, TEXT("IA_Move"), EInputActionValueType::Axis2D);
	Jump = MakeAction(this, TEXT("IA_Jump"), EInputActionValueType::Boolean);
	Dash = MakeAction(this, TEXT("IA_Dash"), EInputActionValueType::Boolean);
	Respawn = MakeAction(this, TEXT("IA_Respawn"), EInputActionValueType::Boolean);
	AddPlayer = MakeAction(this, TEXT("IA_AddPlayer"), EInputActionValueType::Boolean);
	ToggleHelp = MakeAction(this, TEXT("IA_ToggleHelp"), EInputActionValueType::Boolean);
	Ability1 = MakeAction(this, TEXT("IA_Ability1"), EInputActionValueType::Boolean);
	Ability2 = MakeAction(this, TEXT("IA_Ability2"), EInputActionValueType::Boolean);
	Ability3 = MakeAction(this, TEXT("IA_Ability3"), EInputActionValueType::Boolean);
	Aim = MakeAction(this, TEXT("IA_Aim"), EInputActionValueType::Axis2D);
	Pause = MakeAction(this, TEXT("IA_Pause"), EInputActionValueType::Boolean);
	BellySlide = MakeAction(this, TEXT("IA_BellySlide"), EInputActionValueType::Boolean);
	Pause->bTriggerWhenPaused = true;
	ToggleHelp->bTriggerWhenPaused = true;

	Context = NewObject<UInputMappingContext>(this, TEXT("IMC_Penguin"), RF_Transient);
}

bool UPenguinInputConfig::RebuildMappings()
{
	using namespace PenguinInputConfigPrivate;
	if (!Context)
	{
		return false;
	}
	const UPinguSettings* Settings = UPinguSettings::Get();

	// The keys every action gets now.
	TArray<FKey> ActionKeys[NumBindable];
	TSet<FKey> UsedKeys;
	FString Description;
	for (int32 Index = 0; Index < NumBindable; ++Index)
	{
		const EPinguInputAction Action = static_cast<EPinguInputAction>(Index);
		ActionKeys[Index] = GatherKeys(Settings, Action);
		UsedKeys.Append(ActionKeys[Index]);
		Description += FString::Printf(TEXT("%s%s:%s"), Index > 0 ? TEXT(", ") : TEXT(""), *UPinguSettings::GetActionName(Action).ToString(),
			*DescribeKeys(ActionKeys[Index]));
	}
	// Help and respawn keep their fixed keys - except a key the player gave to an action: that one only does the action.
	TArray<FKey> HelpKeys;
	for (const FKey& Key : { EKeys::H, EKeys::F1 })
	{
		if (!UsedKeys.Contains(Key))
		{
			HelpKeys.Add(Key);
		}
	}
	TArray<FKey> RespawnKeys;
	for (const FKey& Key : { EKeys::R, EKeys::Gamepad_Special_Left })
	{
		if (!UsedKeys.Contains(Key))
		{
			RespawnKeys.Add(Key);
		}
	}
	Description += FString::Printf(TEXT(", help:%s, respawn:%s"), *DescribeKeys(HelpKeys), *DescribeKeys(RespawnKeys));
	if (!Settings)
	{
		UE_LOG(LogPingu, Warning, TEXT("[INPUT] no settings object: only the gamepad and the fixed keys work"));
	}
	if (Description == BuiltKeys)
	{
		return false;
	}
	BuiltKeys = Description;
	++MappingVersion;

	// The players' subsystems hold their own copies of the mappings until they rebuild (see RequestRebuildControlMappings
	// in APenguinPlayerController), so the context can be refilled in place.
	Context->UnmapAll();

	// Digital keys feed the 2D move axis: X = right, Y = up (down in the air = fast fall). Right before left as always:
	// holding both, left wins.
	auto MapMoveKeys = [this](const TArray<FKey>& MoveKeys, bool bNegate, bool bVertical)
	{
		for (const FKey& Key : MoveKeys)
		{
			FEnhancedActionKeyMapping& Mapping = Context->MapKey(Move, Key);
			if (bVertical)
			{
				UInputModifierSwizzleAxis* Swizzle = NewObject<UInputModifierSwizzleAxis>(this);
				Swizzle->Order = EInputAxisSwizzle::YXZ;
				Mapping.Modifiers.Add(Swizzle);
			}
			if (bNegate)
			{
				Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(this));
			}
		}
	};
	MapMoveKeys(ActionKeys[static_cast<int32>(EPinguInputAction::MoveRight)], false, false);
	MapMoveKeys(ActionKeys[static_cast<int32>(EPinguInputAction::MoveLeft)], true, false);
	MapMoveKeys(ActionKeys[static_cast<int32>(EPinguInputAction::Up)], false, true);
	MapMoveKeys(ActionKeys[static_cast<int32>(EPinguInputAction::Down)], true, true);
	{
		FEnhancedActionKeyMapping& Stick = Context->MapKey(Move, EKeys::Gamepad_Left2D);
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(this);
		DeadZone->LowerThreshold = 0.22f;
		DeadZone->UpperThreshold = 0.95f;
		DeadZone->Type = EDeadZoneType::Radial;
		Stick.Modifiers.Add(DeadZone);
	}

	auto MapKeys = [this](const UInputAction* Action, const TArray<FKey>& KeysOfAction)
	{
		for (const FKey& Key : KeysOfAction)
		{
			Context->MapKey(Action, Key);
		}
	};
	// The jump action only matters with -PinguOldInput: normally the player controller reads the jump keys itself.
	JumpKeys = ActionKeys[static_cast<int32>(EPinguInputAction::Jump)];
	MapKeys(Jump, JumpKeys);
	MapKeys(Dash, ActionKeys[static_cast<int32>(EPinguInputAction::Dash)]);
	MapKeys(BellySlide, ActionKeys[static_cast<int32>(EPinguInputAction::BellySlide)]);
	// Left / middle / right mouse button (by default) = the left / middle / right icon above your head.
	MapKeys(Ability1, ActionKeys[static_cast<int32>(EPinguInputAction::Ability1)]);
	MapKeys(Ability2, ActionKeys[static_cast<int32>(EPinguInputAction::Ability2)]);
	MapKeys(Ability3, ActionKeys[static_cast<int32>(EPinguInputAction::Ability3)]);
	Context->MapKey(Pause, EKeys::Escape);
	MapKeys(Pause, ActionKeys[static_cast<int32>(EPinguInputAction::Pause)]);
	MapKeys(ToggleHelp, HelpKeys);
	MapKeys(Respawn, RespawnKeys);
	// AddPlayer is not bound: local multiplayer needs its own join flow first.
	{
		FEnhancedActionKeyMapping& Stick = Context->MapKey(Aim, EKeys::Gamepad_Right2D);
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(this);
		DeadZone->LowerThreshold = 0.25f;
		DeadZone->UpperThreshold = 0.95f;
		DeadZone->Type = EDeadZoneType::Radial;
		Stick.Modifiers.Add(DeadZone);
	}

	UE_LOG(LogPingu, Log, TEXT("[INPUT] keys (version %d): %s"), MappingVersion, *Description);
	return true;
}
