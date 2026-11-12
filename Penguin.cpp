// Copyright Epic Games, Inc. All Rights Reserved.

#include "Penguin.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogPingu);

void PinguEndTestRun(const UObject* WorldContext, const TCHAR* Reason)
{
	if (GIsEditor && WorldContext)
	{
		// Play In Editor: quitting the whole editor from inside a play session can crash its shutdown.
		UKismetSystemLibrary::QuitGame(WorldContext, nullptr, EQuitPreference::Quit, false);
		return;
	}
	FPlatformMisc::RequestExit(false, Reason);
}

IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, Penguin, "Penguin" );

bool PinguUseOldMenus()
{
	// The switch, or [PinguMenus] bOldMenus=True in the game config (DefaultGame.ini: Linus's way to keep the old menus).
	static const bool bOld = []()
	{
		bool bConfigOld = false;
		if (GConfig)
		{
			GConfig->GetBool(TEXT("PinguMenus"), TEXT("bOldMenus"), bConfigOld, GGameIni);
		}
		return FParse::Param(FCommandLine::Get(), TEXT("PinguOldMenus")) || bConfigOld;
	}();
	return bOld;
}
