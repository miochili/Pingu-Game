// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogPingu, Log, All);

/** Automated tests (-PinguAutoTest, -PinguMatchTest, -PinguNetTest, ...) end their run with this: in the editor's Play
 *  mode only the play session ends (the editor stays open), a -game process quits. */
PENGUIN_API void PinguEndTestRun(const UObject* WorldContext, const TCHAR* Reason);

/** -PinguOldMenus: the old flat Slate menus and the old card lobby instead of the 3D menus and the lobby island (the one
 *  fallback switch for both; read once from the command line). */
PENGUIN_API bool PinguUseOldMenus();
