// Pingu - owns the Slate UI (menus, in-game HUD), its textures and the menu state: which screen shows, the data the
// screens read (replicated game state, LAN subsystem, settings) and the actions they trigger.
//
// The screens never talk to the game mode: on a LAN client there is none. Only the actions of the host / solo player
// (start, rematch, lobby, match settings, practice, menu, pause) call it, through GetPenguinGameMode() (authority only).

#include "PenguinHUD.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PenguinPlayerController.h"
#include "PenguinPlayerState.h"
#include "PinguFX.h"
#include "PinguMenu3D.h"
#include "PinguMenu2D.h"
#include "DynamicRHI.h"
#include "RenderTimer.h"
#include "PinguNetSubsystem.h"
#include "PinguSettings.h"
#include "PinguUI.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ShaderCompiler.h"
#include "UnrealClient.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWeakWidget.h"

#define LOCTEXT_NAMESPACE "PenguinHUD"

namespace PenguinHUDPrivate
{
	/** Steps Value inside [Min, Max]; bWrap goes round at the ends (a click), else it stops there (arrows). */
	static int32 StepValue(int32 Value, int32 Delta, int32 Min, int32 Max, bool bWrap)
	{
		const int32 Count = Max - Min + 1;
		if (Count <= 0)
		{
			return Min;
		}
		if (bWrap)
		{
			return Min + ((FMath::Clamp(Value, Min, Max) - Min + Delta) % Count + Count) % Count;
		}
		return FMath::Clamp(Value + Delta, Min, Max);
	}

	static bool IsInGamePhase(EPinguPhase Phase)
	{
		return Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown || Phase == EPinguPhase::Fight
			|| Phase == EPinguPhase::RoundOver || Phase == EPinguPhase::Practice;
	}

	// ---- -PinguMenuShots
	struct FMenuShot
	{
		const TCHAR* Name;
		EPinguScreen Screen;
		/** The error popup over the screen. */
		bool bError;
		/** Seen from a LAN client (no authority): the lobby / results variants the joined players see. */
		bool bClient;
		/** Settings page (-1: not the settings). */
		int32 SettingsPage;
		/** 1: the big variant (ten players, teams); 2: solo (PLAY's lobby of your own). */
		int32 Variant;
		/** The PINGUPEDIA's page (0 abilities, 1 traits) and the entry it shows (scrolled to it). */
		int32 Page;
		int32 Entry;
		/** The lobby's TRAITS ON. */
		bool bTraits;
		/** The lobby's address line is copied shortly before the picture (CheckCopyAddressShot): it says COPIED!. */
		bool bCopy = false;
	};
	/** The PINGUPEDIA's shots: an ability with a tag and many numbers (Belly Stomp), and the last trait (its list scrolled to
	 *  the bottom). */
	constexpr int32 ShotStompEntry = static_cast<int32>(EPenguinAbility::BellyStomp) - 1;
	constexpr int32 ShotJesterEntry = static_cast<int32>(EPinguTrait::Jester) - 1;
	static const FMenuShot MenuShots[] = {
		{ TEXT("main_menu"), EPinguScreen::MainMenu, false, false, -1, 0, 0, 0, false },
		{ TEXT("lan"), EPinguScreen::Lan, false, false, -1, 0, 0, 0, false },
		{ TEXT("lobby_solo"), EPinguScreen::Lobby, false, false, -1, 2, 0, 0, false },
		{ TEXT("lobby"), EPinguScreen::Lobby, false, false, -1, 0, 0, 0, false },
		{ TEXT("lobby_copied"), EPinguScreen::Lobby, false, false, -1, 0, 0, 0, false, true },
		{ TEXT("lobby_client"), EPinguScreen::Lobby, false, true, -1, 0, 0, 0, true },
		{ TEXT("lobby_ten"), EPinguScreen::Lobby, false, false, -1, 1, 0, 0, false },
		{ TEXT("lobby_traits"), EPinguScreen::Lobby, false, false, -1, 2, 0, 0, true },
		{ TEXT("trait_picks"), EPinguScreen::TraitPicks, false, false, -1, 2, 0, 0, true }, // (the host's TRAITS list, 2026-11-07: three unticked)
		{ TEXT("settings"), EPinguScreen::Settings, false, false, 0, 0, 0, 0, false },
		{ TEXT("settings_display"), EPinguScreen::Settings, false, false, 1, 0, 0, 0, false },
		{ TEXT("settings_audio"), EPinguScreen::Settings, false, false, 2, 0, 0, 0, false },
		{ TEXT("settings_apply"), EPinguScreen::Settings, false, false, 0, 5, 0, 0, false }, // (SHOW FPS flipped: APPLY enabled, 2026-11-07)
		{ TEXT("settings_confirm"), EPinguScreen::Settings, false, false, 0, 6, 0, 0, false }, // (and BACK pressed: the "Apply changes?" board)
		{ TEXT("controls"), EPinguScreen::Controls, false, false, -1, 0, 0, 0, false },
		{ TEXT("pingupedia_abilities"), EPinguScreen::Pingupedia, false, false, -1, 0, 0, ShotStompEntry, false },
		{ TEXT("pingupedia_snowball"), EPinguScreen::Pingupedia, false, false, -1, 0, 0, 0, false }, // (the first ability: its combo lines)
		{ TEXT("pingupedia_giant_snowball"), EPinguScreen::Pingupedia, false, false, -1, 0, 0, static_cast<int32>(EPenguinAbility::GiantSnowball) - 1, false }, // (the 17th ability, 2026-10-29)
		{ TEXT("pingupedia_snowman"), EPinguScreen::Pingupedia, false, false, -1, 0, 0, static_cast<int32>(EPenguinAbility::Snowman) - 1, false }, // (the 18th ability, 2026-11-03)
		{ TEXT("pingupedia_flap"), EPinguScreen::Pingupedia, false, false, -1, 0, 0, static_cast<int32>(EPenguinAbility::Flap) - 1, false }, // (the 19th ability, 2026-11-04)
		{ TEXT("pingupedia_ice_touch"), EPinguScreen::Pingupedia, false, false, -1, 0, 1, static_cast<int32>(EPinguTrait::IceTouch) - 1, false },
		{ TEXT("pingupedia_giant"), EPinguScreen::Pingupedia, false, false, -1, 0, 1, static_cast<int32>(EPinguTrait::Giant) - 1, false },
		{ TEXT("pingupedia_second_chance"), EPinguScreen::Pingupedia, false, false, -1, 0, 1, static_cast<int32>(EPinguTrait::SecondChance) - 1, false },
		{ TEXT("pingupedia_sticky"), EPinguScreen::Pingupedia, false, false, -1, 0, 1, static_cast<int32>(EPinguTrait::Sticky) - 1, false },
		{ TEXT("pingupedia_gambler"), EPinguScreen::Pingupedia, false, false, -1, 0, 1, static_cast<int32>(EPinguTrait::Gambler) - 1, false },
		{ TEXT("pingupedia_traits"), EPinguScreen::Pingupedia, false, false, -1, 0, 1, ShotJesterEntry, false },
		{ TEXT("pause"), EPinguScreen::Pause, false, false, -1, 0, 0, 0, false },
		{ TEXT("pause_host"), EPinguScreen::Pause, false, false, -1, 4, 0, 0, false }, // (a LAN host with a joiner: the KICK button, 2026-11-07)
		{ TEXT("results"), EPinguScreen::Results, false, false, -1, 0, 0, 0, false },
		{ TEXT("results_client"), EPinguScreen::Results, false, true, -1, 0, 0, 0, false },
		{ TEXT("results_teams"), EPinguScreen::Results, false, false, -1, 1, 0, 0, false },
		{ TEXT("error"), EPinguScreen::MainMenu, true, false, -1, 0, 0, 0, false },
		{ TEXT("error_removed"), EPinguScreen::MainMenu, true, false, -1, 3, 0, 0, false },
	};
	static constexpr int32 NumMenuShots = static_cast<int32>(UE_ARRAY_COUNT(MenuShots));
	/** Each screen: let its transition finish, take the shot, give the shot a moment to be written. */
	constexpr float ShotSettle = 0.6f;
	constexpr float ShotWrite = 0.5f;
	/** The copy line is used this long before its picture (COPIED! shows 1.5 s: the picture is well inside). */
	constexpr float ShotCopyLead = 0.35f;

	// ---- the menus' frame time (2026-10-27, card 2026-10-27_gui-2d: the 2D menus against the 3D ones - menushots, then
	// menushots with PINGU_3D_MENUS=1, back to back). Four screens are measured before their picture: the title (also with no
	// menu at all: the world alone), the lobby with ten penguins, the settings, the PINGUPEDIA.
	constexpr float PerfSettle = 1.5f;
	constexpr float PerfWindow = 2.5f;
	constexpr float PerfGap = 0.6f;
	/** 0: no measurement, 1: the screen, 2: the screen and then the world without a menu. */
	static int32 PerfKind(const TCHAR* Name)
	{
		const FString Shot(Name);
		if (Shot == TEXT("main_menu"))
		{
			return 2;
		}
		return Shot == TEXT("lobby_ten") || Shot == TEXT("settings") || Shot == TEXT("pingupedia_abilities") ? 1 : 0;
	}
	/** How much longer such a screen waits before its picture. */
	static float PerfExtra(int32 Kind)
	{
		return Kind == 0 ? 0.f : (Kind == 1 ? PerfSettle + PerfWindow : PerfSettle + 2.f * PerfWindow + 2.f * PerfGap);
	}
	struct FPerfSum
	{
		double Frame = 0.0;
		double Game = 0.0;
		double Draw = 0.0;
		double Gpu = 0.0;
		int32 Count = 0;
		int32 Parts = 0;
		int32 Draws = 0;
		int32 Verts = 0;
		/** One frame (its real length: -benchmark gives the game a fixed step, the clock on the wall is what costs). */
		void Add(double RealSeconds)
		{
			Frame += 1000.0 * RealSeconds;
			Game += FPlatformTime::ToMilliseconds(GGameThreadTime);
			Draw += FPlatformTime::ToMilliseconds(GRenderThreadTime);
			Gpu += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
			++Count;
			Parts = FMath::Max(Parts, PinguMenu2D::GetPaintedParts());
			Draws = FMath::Max(Draws, PinguMenu2D::GetPaintedDraws());
			Verts = FMath::Max(Verts, PinguMenu2D::GetPaintedVerts());
		}
	};
	static FPerfSum PerfMenu;
	static FPerfSum PerfWorld;
	static int32 PerfStage = 0;

	static void LogPerf(const TCHAR* Shot, const TCHAR* What, const FPerfSum& Sum, const APinguMenu3D* Menu)
	{
		const double N = FMath::Max(Sum.Count, 1);
		int32 Primitives = 0;
		int32 FlatParts = 0;
		if (Menu)
		{
			Menu->CountParts(Primitives, FlatParts);
		}
		UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT perf %s %s (%s menus): frame %.2f ms (%.0f fps), game %.2f, draw %.2f, gpu %.2f ms over %d frames; painted %d parts in %d Slate draws, %d vertices; the menu actor has %d render primitives, %d 2D parts (a measurement: compare 2D with 3D)"),
			Shot, What, PinguMenu3D::IsFlat() ? TEXT("2D") : TEXT("3D"), Sum.Frame / N, 1000.0 / FMath::Max(Sum.Frame / N, 0.01), Sum.Game / N, Sum.Draw / N, Sum.Gpu / N,
			Sum.Count, Sum.Parts, Sum.Draws, Sum.Verts, Primitives, FlatParts);
	}
	/** The PINGUPEDIA's key check after its traits picture: how many keys, and the time between two. */
	constexpr int32 PediaKeyCount = 4;
	constexpr float PediaKeyGap = 0.2f;
	/** Every PINGUPEDIA card must leave this much of its panel free (Slate units): a font or a stat line that grows it shows up
	 *  here before a card ever needs its scroll bar (it was 590 of 596 with Rammetto One, 605 in a build with wider labels; Titan One since 2026-10-24). */
	constexpr float PediaCardSpare = 12.f;

	/** Made-up games for the LAN screenshot. */
	static const TArray<FPinguLanGame>& ShotGames()
	{
		static TArray<FPinguLanGame> Games;
		if (Games.Num() == 0)
		{
			FPinguLanGame& Open = Games.AddDefaulted_GetRef();
			Open.Address = TEXT("192.168.1.23:7777");
			Open.HostName = TEXT("Frosty");
			Open.Players = 2;
			Open.MaxPlayers = 10;
			FPinguLanGame& Busy = Games.AddDefaulted_GetRef();
			Busy.Address = TEXT("192.168.1.31:7777");
			Busy.HostName = TEXT("Pebble");
			Busy.Players = 6;
			Busy.MaxPlayers = 10;
			Busy.bInMatch = true;
			FPinguLanGame& Full = Games.AddDefaulted_GetRef();
			Full.Address = TEXT("192.168.1.40:7777");
			Full.HostName = TEXT("Chilly");
			Full.Players = 10;
			Full.MaxPlayers = 10;
			Full.bInMatch = true;
		}
		return Games;
	}

	/** The made-up team match of the big results screenshot: ten penguins - purple (3 wins, the longest title), red (you),
	 *  green, and one on its own (ShotLoneSlot: it picked no team - inside the match a team of one, brown, that never
	 *  shows). */
	static int32 ShotTeamOf(int32 Slot)
	{
		static const int32 Teams[] = { 0, 4, 0, 4, 0, 4, 2, 2, 2, 9 };
		return Teams[FMath::Clamp(Slot, 0, 9)];
	}
	constexpr int32 ShotLoneSlot = 9;
	/** The team the big lobby screenshot's card of a slot picked (the one on its own: NO TEAM). */
	static int32 ShotTeamChoiceOf(int32 Slot)
	{
		return Slot == ShotLoneSlot ? -1 : ShotTeamOf(Slot);
	}
	static int32 ShotTeamWins(int32 Team)
	{
		return Team == 4 ? 3 : (Team == 0 ? 2 : (Team == 2 ? 1 : 0));
	}

	/** Made-up standings for the results screenshots (the human first: VICTORY!). Variant 1: ten penguins in teams
	 *  (ShotTeamOf: purple won 3 : 2 : 1 : 0). */
	static const TArray<FPinguScore>& ShotScores(const FString& PlayerName, int32 Variant)
	{
		static TArray<FPinguScore> Scores[2];
		TArray<FPinguScore>& Rows = Scores[FMath::Clamp(Variant, 0, 1)];
		if (Rows.Num() == 0)
		{
			const TCHAR* Names[] = { TEXT(""), TEXT("Frosty"), TEXT("Pebble"), TEXT("Waddles"), TEXT("Chilly"), TEXT("Slushy"), TEXT("Flipper"), TEXT("Nibbles"),
				TEXT("Puddles"), TEXT("Mochi") };
			const int32 Wins[] = { 3, 2, 1, 1, 0, 0, 0, 0, 0, 0 };
			const int32 KOs[] = { 7, 5, 4, 3, 2, 2, 1, 1, 0, 0 };
			const int32 Falls[] = { 1, 3, 4, 3, 4, 3, 4, 2, 4, 4 };
			const int32 Hits[] = { 18, 14, 11, 9, 8, 6, 7, 3, 5, 2 };
			const int32 Count = Variant == 1 ? 10 : 6;
			for (int32 Slot = 0; Slot < Count; ++Slot)
			{
				FPinguScore& Score = Rows.AddDefaulted_GetRef();
				Score.Name = Slot == 0 ? PlayerName : FString(Names[Slot]);
				Score.Slot = Slot;
				Score.KOs = KOs[Slot];
				Score.Falls = Falls[Slot];
				Score.Hits = Hits[Slot];
				Score.bHuman = Slot == 0;
				Score.Team = Variant == 1 ? ShotTeamOf(Slot) : -1;
				Score.Wins = Variant == 1 ? ShotTeamWins(Score.Team) : Wins[Slot];
			}
		}
		return Rows;
	}

	/** A made-up bot of the lobby screenshots. */
	struct FShotBot
	{
		const TCHAR* Name;
		int32 Skill;
		int32 Team;
	};

	/** Made-up bots for the lobby screenshots: variant 0 (a LAN game): two; 2 (solo): three, of every skill. */
	static void ShotBots(FPinguMatchSettings& Settings, int32 Variant)
	{
		Settings.Bots.Reset();
		static const FShotBot LanBots[] = { { TEXT("Waddles"), 1, 1 }, { TEXT("Mochi"), 2, -1 } };
		static const FShotBot SoloBots[] = { { TEXT("Waddles"), 0, 3 }, { TEXT("Pebble"), 1, 3 }, { TEXT("Blizzard"), 2, -1 } };
		// The full lobby (variant 1): nine bots with you - each in a team but the last (ShotLoneSlot), every skill (2026-10-29: the
		// wooden card lobby shows them as bot cards: BOT, the difficulty, the x; the lobby island keeps its real bots instead).
		static const FShotBot FullBots[] = { { TEXT("Frosty"), 0, 0 }, { TEXT("Pebble"), 1, 0 }, { TEXT("Waddles"), 2, 0 }, { TEXT("Chilly"), 0, 0 },
			{ TEXT("Slushy"), 1, 0 }, { TEXT("Flipper"), 2, 0 }, { TEXT("Nibbles"), 0, 0 }, { TEXT("Puddles"), 1, 0 }, { TEXT("Mochi"), 2, 0 } };
		const FShotBot* Bots = Variant == 2 ? SoloBots : (Variant == 1 ? FullBots : LanBots);
		const int32 Count = Variant == 1 ? UE_ARRAY_COUNT(FullBots) : (Variant == 2 ? UE_ARRAY_COUNT(SoloBots) : UE_ARRAY_COUNT(LanBots));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FPinguBotSetup& Setup = Settings.Bots.AddDefaulted_GetRef();
			Setup.Name = Bots[Index].Name;
			Setup.Skill = Bots[Index].Skill;
			// (The full lobby's teams by slot - the bots take slots 1..9 - as the people's cards had them.)
			Setup.Team = Variant == 1 ? ShotTeamChoiceOf(Index + 1) : Bots[Index].Team;
		}
		Settings.NumBots = Settings.Bots.Num();
	}
}

APenguinHUD::APenguinHUD()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	ScreenFocus.SetNum(static_cast<int32>(EPinguScreen::Count));
}

void APenguinHUD::BeginPlay()
{
	Super::BeginPlay();
	// All local players share one screen: only the first player's HUD builds the UI (the others forward to it).
	UWorld* World = GetWorld();
	if (!PlayerOwner || !World || PlayerOwner != World->GetFirstPlayerController())
	{
		return;
	}
	// This world's own viewport (several PIE instances each have one).
	UGameViewportClient* Viewport = World->GetGameViewport();
	if (!Viewport && GEngine)
	{
		Viewport = GEngine->GameViewport;
	}
	if (!Viewport)
	{
		return;
	}
	bMenuShots = FParse::Param(FCommandLine::Get(), TEXT("PinguMenuShots"));
	LoadTextures();
	Root = SNew(SPinguRoot).HUD(this);
	RootContainer = SNew(SWeakWidget).PossiblyNullContent(Root);
	Viewport->AddViewportWidgetContent(RootContainer.ToSharedRef(), 10);
	RootViewport = Viewport;
	UpdateScreen();
	if (bMenuShots)
	{
		UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] waiting for the title screen"));
	}
}

void APenguinHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The LAN subsystem lives on (game instance): a search the LAN screen started must not run on.
	if (Root.IsValid() && ScreenStack.Contains(EPinguScreen::Lan))
	{
		if (UPinguNetSubsystem* Net = GetNet())
		{
			Net->StopSearch();
		}
	}
	bCapturing = false;
	UGameViewportClient* Viewport = RootViewport.Get();
	if (RootContainer.IsValid() && Viewport)
	{
		Viewport->RemoveViewportWidgetContent(RootContainer.ToSharedRef());
	}
	RootContainer.Reset();
	Root.Reset();
	Super::EndPlay(EndPlayReason);
}

void APenguinHUD::LoadTextures()
{
	EmptyBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
	for (int32 Index = 1; Index < static_cast<int32>(EPenguinAbility::Count); ++Index)
	{
		const EPenguinAbility Ability = static_cast<EPenguinAbility>(Index);
		const FString Name = UPenguinAbilityComponent::GetInfo(Ability).IconName;
		const FString Path = FString::Printf(TEXT("/Game/UI/Icons/T_Icon_%s.T_Icon_%s"), *Name, *Name);
		FSlateBrush& Brush = AbilityBrushes.Add(Ability);
		Brush.ImageSize = FVector2D(92.f, 92.f);
		if (UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *Path, nullptr, LOAD_Quiet | LOAD_NoWarn))
		{
			Textures.Add(Texture);
			Brush.SetResourceObject(Texture);
			Brush.DrawAs = ESlateBrushDrawType::Image;
		}
		else
		{
			Brush.DrawAs = ESlateBrushDrawType::NoDrawType;
		}
	}
	// A badge per trait (Traits mode: the reveal's cards, the PINGUPEDIA).
	for (const EPinguTrait Trait : PinguTraits::GetAll())
	{
		const FString Name = PinguTraits::GetInfo(Trait).IconName;
		const FString Path = FString::Printf(TEXT("/Game/UI/Traits/T_Trait_%s.T_Trait_%s"), *Name, *Name);
		FSlateBrush& Brush = TraitBrushes.Add(Trait);
		Brush.ImageSize = FVector2D(128.f, 128.f);
		if (UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *Path, nullptr, LOAD_Quiet | LOAD_NoWarn))
		{
			Textures.Add(Texture);
			Brush.SetResourceObject(Texture);
			Brush.DrawAs = ESlateBrushDrawType::Image;
		}
		else
		{
			Brush.DrawAs = ESlateBrushDrawType::NoDrawType;
		}
	}
	// A portrait per slot: the face (the beanie left out) and the beanie layer the UI tints (slot or team colour), each as
	// the full bust (flippers in) and the head crop for small draws (T_PortraitSmall*).
	PortraitBrushes.SetNum(PinguMaxPenguins);
	PortraitFeltBrushes.SetNum(PinguMaxPenguins);
	SmallPortraitBrushes.SetNum(PinguMaxPenguins);
	SmallPortraitFeltBrushes.SetNum(PinguMaxPenguins);
	for (int32 Slot = 0; Slot < PinguMaxPenguins; ++Slot)
	{
		for (int32 Layer = 0; Layer < 4; ++Layer)
		{
			static const TCHAR* const Names[] = { TEXT("T_Portrait"), TEXT("T_PortraitFelt"), TEXT("T_PortraitSmall"), TEXT("T_PortraitSmallFelt") };
			TArray<FSlateBrush>* const Sets[] = { &PortraitBrushes, &PortraitFeltBrushes, &SmallPortraitBrushes, &SmallPortraitFeltBrushes };
			const TCHAR* Name = Names[Layer];
			const FString Path = FString::Printf(TEXT("/Game/UI/Portraits/%s_%d.%s_%d"), Name, Slot, Name, Slot);
			FSlateBrush& Brush = (*Sets[Layer])[Slot];
			Brush.ImageSize = FVector2D(64.f, 64.f);
			if (UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *Path, nullptr, LOAD_Quiet | LOAD_NoWarn))
			{
				Textures.Add(Texture);
				Brush.SetResourceObject(Texture);
				Brush.DrawAs = ESlateBrushDrawType::Image;
			}
			else
			{
				Brush.DrawAs = ESlateBrushDrawType::NoDrawType;
			}
		}
	}
}

const FSlateBrush* APenguinHUD::GetAbilityBrush(EPenguinAbility Ability) const
{
	const FSlateBrush* Brush = AbilityBrushes.Find(Ability);
	return Brush ? Brush : &EmptyBrush;
}

const FSlateBrush* APenguinHUD::GetTraitBrush(EPinguTrait Trait) const
{
	const FSlateBrush* Brush = TraitBrushes.Find(Trait);
	return Brush ? Brush : &EmptyBrush;
}

namespace PenguinHUDPrivate
{
	// The head crop for small draws when it is loaded (else the full bust).
	const FSlateBrush* PickPortrait(const TArray<FSlateBrush>& Full, const TArray<FSlateBrush>& Small, int32 Slot, float DrawSize, const FSlateBrush& Empty)
	{
		if (DrawSize > 0.f && DrawSize < APenguinHUD::PortraitSmallBelow && Small.IsValidIndex(Slot) && Small[Slot].GetDrawType() != ESlateBrushDrawType::NoDrawType)
		{
			return &Small[Slot];
		}
		return Full.IsValidIndex(Slot) ? &Full[Slot] : &Empty;
	}
}

const FSlateBrush* APenguinHUD::GetPortraitBrush(int32 Slot, float DrawSize) const
{
	return PenguinHUDPrivate::PickPortrait(PortraitBrushes, SmallPortraitBrushes, Slot, DrawSize, EmptyBrush);
}

const FSlateBrush* APenguinHUD::GetPortraitFeltBrush(int32 Slot, float DrawSize) const
{
	return PenguinHUDPrivate::PickPortrait(PortraitFeltBrushes, SmallPortraitFeltBrushes, Slot, DrawSize, EmptyBrush);
}

// ---------------------------------------------------------------------------- data

APenguinGameState* APenguinHUD::GetGameState() const
{
	return GetWorld() ? GetWorld()->GetGameState<APenguinGameState>() : nullptr;
}

APenguinGameMode* APenguinHUD::GetPenguinGameMode() const
{
	return GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr;
}

APenguinCharacter* APenguinHUD::GetHumanPenguin() const
{
	return PlayerOwner ? Cast<APenguinCharacter>(PlayerOwner->GetPawn()) : nullptr;
}

EPinguPhase APenguinHUD::GetPhase() const
{
	const APenguinGameState* GameState = GetGameState();
	return GameState ? GameState->GetPhase() : EPinguPhase::Menu;
}

float APenguinHUD::GetPhaseTime() const
{
	const APenguinGameState* GameState = GetGameState();
	return GameState ? GameState->GetPhaseTime() : 0.f;
}

bool APenguinHUD::IsNetworkGame() const
{
	if (bMenuShots && (bShotClient || (ShotScreen == EPinguScreen::Lobby && ShotVariant != 2) || (ShotScreen == EPinguScreen::Pause && ShotVariant == 4)))
	{
		return true; // screenshot of a LAN client's view, or of a LAN lobby (solo's own lobby: variant 2)
	}
	const UWorld* World = GetWorld();
	if (World && World->GetNetMode() != NM_Standalone)
	{
		return true;
	}
	const APenguinGameState* GameState = GetGameState();
	return GameState && GameState->IsNetworkGame();
}

bool APenguinHUD::HasMatchAuthority() const
{
	if (bMenuShots && bShotClient)
	{
		return false; // screenshot of a LAN client's view
	}
	return GetPenguinGameMode() != nullptr;
}

bool APenguinHUD::IsMatchTest() const
{
	const APenguinGameState* GameState = GetGameState();
	return GameState && GameState->IsMatchTest();
}

FPinguMatchSettings APenguinHUD::GetMatchSettings() const
{
	const APenguinGameState* GameState = GetGameState();
	FPinguMatchSettings Settings = GameState ? GameState->GetSettings() : FPinguMatchSettings();
	if (bMenuShots && ShotScreen == EPinguScreen::Lobby)
	{
		// (The lobby island's full lobby keeps its real nine bots - PrepareMenuShotWorld puts them on the island in their teams:
		// made-up ones would leave the HUD with none, and no bot's x would show.)
		if (ShotVariant != 1 || !PinguMenu3D::UseIslandLobby())
		{
			PenguinHUDPrivate::ShotBots(Settings, ShotVariant);
		}
		Settings.bTraits = bShotTraits;
	}
	return Settings;
}

bool APenguinHUD::AreTraitsOn() const
{
	return GetMatchSettings().bTraits;
}

bool APenguinHUD::IsTeamMatch() const
{
	if (bMenuShots && ShotScreen == EPinguScreen::Results)
	{
		return ShotVariant == 1;
	}
	const APenguinGameState* GameState = GetGameState();
	return GameState && GameState->IsTeamMatch();
}

int32 APenguinHUD::GetTeamWinCount(int32 Team) const
{
	if (bMenuShots && ShotScreen == EPinguScreen::Results)
	{
		return PenguinHUDPrivate::ShotTeamWins(Team);
	}
	const APenguinGameState* GameState = GetGameState();
	return GameState ? GameState->GetTeamWinCount(Team) : 0;
}

TArray<int32> APenguinHUD::GetTeamBoard() const
{
	if (bMenuShots && ShotScreen == EPinguScreen::Results)
	{
		return { 4, 0, 2, 9 };
	}
	const APenguinGameState* GameState = GetGameState();
	return GameState ? GameState->GetTeamBoard() : TArray<int32>();
}

int32 APenguinHUD::GetMatchWinnerTeam() const
{
	if (bMenuShots && ShotScreen == EPinguScreen::Results)
	{
		return ShotVariant == 1 ? 4 : -1;
	}
	const APenguinGameState* GameState = GetGameState();
	return GameState ? GameState->GetMatchWinnerTeam() : -1;
}

int32 APenguinHUD::GetRoundWinnerTeam() const
{
	const APenguinGameState* GameState = GetGameState();
	return GameState ? GameState->GetRoundWinnerTeam() : -1;
}

int32 APenguinHUD::GetScoreColorIndex(const FPinguScore& Score) const
{
	return Score.Slot;
}

int32 APenguinHUD::GetScoreTeamRing(const FPinguScore& Score) const
{
	if (Score.Team < 0 || !IsTeamMatch())
	{
		return -1;
	}
	int32 Members = 0;
	for (const FPinguScore& Other : GetScores())
	{
		Members += Other.Team == Score.Team ? 1 : 0;
	}
	return Members >= 2 ? Score.Team : -1;
}

int32 APenguinHUD::GetPenguinTeamRing(const APenguinCharacter* Penguin) const
{
	const APenguinGameState* GameState = GetGameState();
	return Penguin && GameState ? GameState->GetShownTeam(Penguin->GetTeam()) : -1;
}

FText APenguinHUD::GetBannerText() const
{
	return Root.IsValid() ? Root->GetBannerText() : FText::GetEmpty();
}

FText APenguinHUD::GetResultsTitle() const
{
	return Root.IsValid() ? Root->GetResultsTitle() : FText::GetEmpty();
}

bool APenguinHUD::GetOverheadTeamRings(TArray<FString>& OutRinged, int32& OutRings, float& OutRingToIcon, float& OutClearance) const
{
	const SPinguOverheadAbilities::FPaintStats& PaintStats = SPinguOverheadAbilities::GetLastPaintStats();
	OutRinged = PaintStats.Ringed;
	OutRings = PaintStats.IconRings;
	OutRingToIcon = PaintStats.RingToIcon;
	OutClearance = PaintStats.MinClearance;
	return PaintStats.Frame > 0 && GFrameCounter - PaintStats.Frame <= 5;
}

const FPinguScore* APenguinHUD::GetLoneTeamMember(int32 Team) const
{
	const FPinguScore* Found = nullptr;
	for (const FPinguScore& Score : GetScores())
	{
		if (Team >= 0 && Score.Team == Team)
		{
			if (Found)
			{
				return nullptr; // two or more: a real team
			}
			Found = &Score;
		}
	}
	return Found;
}

bool APenguinHUD::IsWaitingForContinueKey() const
{
	const APenguinGameState* GameState = GetGameState();
	return GameState && GameState->GetPhase() == EPinguPhase::RoundOver && GameState->IsWaitingForContinue()
		&& GameState->GetPhaseTime() >= APenguinGameState::ContinueAfter;
}

const TArray<FPinguScore>& APenguinHUD::GetScores() const
{
	if (bMenuShots && ShotScreen == EPinguScreen::Results)
	{
		return PenguinHUDPrivate::ShotScores(GetPlayerName(), ShotVariant);
	}
	static const TArray<FPinguScore> NoScores;
	const APenguinGameState* GameState = GetGameState();
	return GameState ? GameState->GetScores() : NoScores;
}

TArray<int32> APenguinHUD::GetLeaderboard() const
{
	const APenguinGameState* GameState = GetGameState();
	if (GameState && !(bMenuShots && ShotScreen == EPinguScreen::Results))
	{
		return GameState->GetLeaderboard();
	}
	// Made-up scores: most wins, then most knock-outs, then fewest falls, then slot order.
	const TArray<FPinguScore>& Scores = GetScores();
	TArray<int32> Order;
	for (int32 Index = 0; Index < Scores.Num(); ++Index)
	{
		Order.Add(Index);
	}
	Order.StableSort([&Scores](int32 A, int32 B)
	{
		if (Scores[A].Wins != Scores[B].Wins)
		{
			return Scores[A].Wins > Scores[B].Wins;
		}
		if (Scores[A].KOs != Scores[B].KOs)
		{
			return Scores[A].KOs > Scores[B].KOs;
		}
		if (Scores[A].Falls != Scores[B].Falls)
		{
			return Scores[A].Falls < Scores[B].Falls;
		}
		return Scores[A].Slot < Scores[B].Slot;
	});
	return Order;
}

int32 APenguinHUD::GetRoundWinnerSlot() const
{
	const APenguinGameState* GameState = GetGameState();
	return GameState ? GameState->GetRoundWinnerSlot() : -1;
}

int32 APenguinHUD::GetMatchWinnerSlot() const
{
	if (bMenuShots && ShotScreen == EPinguScreen::Results)
	{
		return 0;
	}
	const APenguinGameState* GameState = GetGameState();
	return GameState ? GameState->GetMatchWinnerSlot() : -1;
}

const FPinguScore* APenguinHUD::FindScoreBySlot(int32 Slot) const
{
	// Rows can go (a LAN player left), so a slot is not an index.
	return Slot < 0 ? nullptr : GetScores().FindByPredicate([Slot](const FPinguScore& Score) { return Score.Slot == Slot; });
}

APlayerState* APenguinHUD::GetLocalPlayerState() const
{
	return PlayerOwner ? PlayerOwner->PlayerState.Get() : nullptr;
}

bool APenguinHUD::IsLocalScore(const FPinguScore& Score) const
{
	if (!IsNetworkGame())
	{
		return Score.bHuman; // solo: the one human is this player
	}
	const APlayerState* Local = GetLocalPlayerState();
	return Local && Score.PlayerState == Local;
}

TArray<FPinguLobbyPlayer> APenguinHUD::GetLobbyPlayers() const
{
	TArray<FPinguLobbyPlayer> Players;
	if (bMenuShots && (ShotScreen == EPinguScreen::Lobby || (ShotScreen == EPinguScreen::Pause && ShotVariant == 4)))
	{
		// Made up for the screenshot: us hosting and a friend joined (or the other way round for the client view); the big
		// variant: ten people in teams (some on their own); solo: just us (in the blue team).
		FPinguLobbyPlayer& Host = Players.AddDefaulted_GetRef();
		Host.Name = bShotClient ? FString(TEXT("Frosty")) : GetPlayerName();
		Host.Slot = 0;
		Host.bHost = true;
		Host.bYou = !bShotClient;
		Host.TeamChoice = ShotVariant == 2 ? 3 : 1;
		if (ShotVariant == 2)
		{
			return Players;
		}
		if (ShotVariant == 1)
		{
			// (The card lobby since 2026-10-29: the nine bots of ShotBots' full lobby; the island: its real ones.)
			// The lobby island's full lobby: us and nine real bots on the island (made-up people would leave no room for the
			// bots, and a bot's x shows only while there is a bot to send away - 2026-10-27: lobby_ten showed no x at all).
			Host.TeamChoice = 0;
			return Players;
		}
		FPinguLobbyPlayer& Friend = Players.AddDefaulted_GetRef();
		Friend.Name = bShotClient ? GetPlayerName() : FString(TEXT("Frosty"));
		Friend.Slot = 1;
		Friend.bYou = bShotClient;
		Friend.PlayerId = 2; // (made up)
		if (ShotVariant == 1)
		{
			// (Before adding more: the array may move, and Host / Friend with it.)
			Host.TeamChoice = 0;
			Friend.TeamChoice = 4;
			const TCHAR* More[] = { TEXT("Pebble"), TEXT("Waddles"), TEXT("Chilly"), TEXT("Slushy"), TEXT("Flipper"), TEXT("Nibbles"), TEXT("Puddles"), TEXT("Mochi") };
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(More); ++Index)
			{
				FPinguLobbyPlayer& Other = Players.AddDefaulted_GetRef();
				Other.Name = More[Index];
				Other.Slot = Index + 2;
				Other.TeamChoice = PenguinHUDPrivate::ShotTeamChoiceOf(Index + 2);
			}
		}
		return Players;
	}
	const APenguinGameState* GameState = GetGameState();
	if (!GameState)
	{
		return Players;
	}
	TArray<APenguinPlayerState*> States = GameState->GetPenguinPlayers();
	if (States.Num() == 0)
	{
		for (APlayerState* PlayerState : GameState->PlayerArray)
		{
			if (APenguinPlayerState* PenguinState = Cast<APenguinPlayerState>(PlayerState))
			{
				States.Add(PenguinState);
			}
		}
	}
	const APlayerState* Local = GetLocalPlayerState();
	for (const APenguinPlayerState* State : States)
	{
		if (!State)
		{
			continue;
		}
		FPinguLobbyPlayer& Player = Players.AddDefaulted_GetRef();
		Player.Name = State->GetPlayerName();
		Player.Slot = State->GetPenguinSlot() >= 0 ? State->GetPenguinSlot() : Players.Num() - 1;
		Player.bHost = State->IsHost();
		Player.bYou = State == Local;
		Player.TeamChoice = State->GetTeamChoice();
		Player.bWaiting = State->IsWaitingForRound();
		Player.PlayerId = State->GetPlayerId();
	}
	return Players;
}

FString APenguinHUD::GetHostName() const
{
	const TArray<FPinguLobbyPlayer> Players = GetLobbyPlayers();
	for (const FPinguLobbyPlayer& Player : Players)
	{
		if (Player.bHost)
		{
			return Player.Name;
		}
	}
	return Players.Num() > 0 ? Players[0].Name : LOCTEXT("TheHost", "the host").ToString();
}

TArray<FPinguLobbyPlayer> APenguinHUD::GetLobbyCards() const
{
	TArray<FPinguLobbyPlayer> Cards = GetLobbyPlayers();
	// The bots take the slots the people leave free, in order (as EnsurePenguins gives them out).
	uint32 Used = 0;
	for (const FPinguLobbyPlayer& Person : Cards)
	{
		Used |= 1u << FMath::Clamp(Person.Slot, 0, 31);
	}
	const FPinguMatchSettings Settings = GetMatchSettings();
	const int32 Bots = FMath::Min(Settings.Bots.Num(), FMath::Max(PinguMaxPenguins - Cards.Num(), 0));
	for (int32 Index = 0; Index < Bots; ++Index)
	{
		int32 Slot = 0;
		while (Slot < PinguMaxPenguins && (Used & (1u << Slot)) != 0)
		{
			++Slot;
		}
		Used |= 1u << FMath::Min(Slot, 31);
		const FPinguBotSetup& Setup = Settings.Bots[Index];
		FPinguLobbyPlayer& Card = Cards.AddDefaulted_GetRef();
		Card.Name = Setup.Name;
		Card.Slot = FMath::Min(Slot, PinguMaxPenguins - 1);
		Card.TeamChoice = Setup.Team;
		Card.bBot = true;
		Card.BotIndex = Index;
		Card.Skill = Setup.Skill;
	}
	return Cards;
}

int32 APenguinHUD::GetMaxBots() const
{
	const bool bLanRules = IsNetworkGame() || (bMenuShots && ShotScreen == EPinguScreen::Lobby && ShotVariant != 2);
	if (!bLanRules)
	{
		return PinguMaxPenguins - 1;
	}
	// Humans + bots never exceed ten penguins.
	return FMath::Clamp(PinguMaxPenguins - FMath::Max(GetLobbyPlayers().Num(), 1), 0, PinguMaxPenguins - 1);
}

int32 APenguinHUD::GetMinBots() const
{
	const bool bLanRules = IsNetworkGame() || (bMenuShots && ShotScreen == EPinguScreen::Lobby && ShotVariant != 2);
	return bLanRules ? 0 : 1;
}

int32 APenguinHUD::GetBotCount() const
{
	return FMath::Clamp(GetMatchSettings().NumBots, GetMinBots(), GetMaxBots());
}

bool APenguinHUD::CanAddBot() const
{
	return HasMatchAuthority() && GetBotCount() < GetMaxBots();
}

bool APenguinHUD::CanRemoveBot() const
{
	return HasMatchAuthority() && GetBotCount() > GetMinBots();
}

int32 APenguinHUD::GetLocalTeamChoice() const
{
	if (bMenuShots && ShotScreen == EPinguScreen::Lobby)
	{
		for (const FPinguLobbyPlayer& Player : GetLobbyPlayers())
		{
			if (Player.bYou)
			{
				return Player.TeamChoice;
			}
		}
		return -1;
	}
	const APenguinPlayerState* Local = Cast<APenguinPlayerState>(GetLocalPlayerState());
	return Local ? Local->GetTeamChoice() : -1;
}

bool APenguinHUD::IsLocalWaitingForRound() const
{
	const APenguinPlayerState* Local = Cast<APenguinPlayerState>(GetLocalPlayerState());
	return Local && Local->IsWaitingForRound() && !bMenuShots;
}

UPinguNetSubsystem* APenguinHUD::GetNet() const
{
	return UPinguNetSubsystem::Get(this);
}

bool APenguinHUD::IsConnecting() const
{
	const UPinguNetSubsystem* Net = GetNet();
	return Net && Net->GetState() == EPinguNetState::Connecting && !bMenuShots;
}

const TArray<FPinguLanGame>& APenguinHUD::GetFoundGames() const
{
	if (bMenuShots)
	{
		return PenguinHUDPrivate::ShotGames();
	}
	static const TArray<FPinguLanGame> NoGames;
	const UPinguNetSubsystem* Net = GetNet();
	return Net ? Net->GetFoundGames() : NoGames;
}

bool APenguinHUD::IsSearching() const
{
	const UPinguNetSubsystem* Net = GetNet();
	return bMenuShots || (Net && Net->IsSearching());
}

FText APenguinHUD::GetNetError() const
{
	if (bMenuShots)
	{
		// (Variant 3: the message a removed joiner gets, 2026-10-30.)
		return bShotError ? (ShotVariant == 3 ? LOCTEXT("ShotRemoved", "The host kicked you.") : LOCTEXT("ShotError", "Could not connect to 192.168.1.23.")) : FText::GetEmpty();
	}
	const UPinguNetSubsystem* Net = GetNet();
	return Net ? Net->GetLastError() : FText::GetEmpty();
}

bool APenguinHUD::HasErrorPopup() const
{
	return !IsMatchTest() && !GetNetError().IsEmpty();
}

FString APenguinHUD::GetHostAddressText() const
{
	if (bMenuShots)
	{
		return TEXT("192.168.1.23"); // made up, like the LAN screen's games: the pictures never show this PC's own address
	}
	const UPinguNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return FString();
	}
	const FString Address = Net->GetLocalAddress();
	return Net->GetGamePort() != UPinguNetSubsystem::DefaultGamePort ? FString::Printf(TEXT("%s:%d"), *Address, Net->GetGamePort()) : Address;
}

bool APenguinHUD::IsHostAddressCopied() const
{
	return PinguUI::Now() - HostAddressCopiedAt < CopiedFeedbackTime;
}

bool APenguinHUD::CopyTextToClipboard(const FString& Text)
{
	// The clipboard can be busy for a moment (a clipboard manager, remote desktop): ClipboardCopy does nothing then and says
	// nothing, so the text is read back, and put there again if it is not what we asked for.
	constexpr int32 Tries = 5;
	for (int32 Try = 0; Try < Tries; ++Try)
	{
		FPlatformApplicationMisc::ClipboardCopy(*Text);
		FString Back;
		FPlatformApplicationMisc::ClipboardPaste(Back);
		if (Back == Text)
		{
			return true;
		}
		FPlatformProcess::Sleep(0.02f);
	}
	return false;
}

FString APenguinHUD::ReadClipboardText()
{
	FString Text;
	for (int32 Try = 0; Try < 5 && Text.IsEmpty(); ++Try)
	{
		Text.Reset(); // (a failed ClipboardPaste leaves the old text in it)
		FPlatformApplicationMisc::ClipboardPaste(Text);
		if (Text.IsEmpty())
		{
			FPlatformProcess::Sleep(0.02f);
		}
	}
	return Text;
}

FString APenguinHUD::GetPlayerName() const
{
	const UPinguSettings* Settings = UPinguSettings::Get();
	return Settings ? Settings->GetPlayerName() : FString(TEXT("Pingu"));
}

// ---------------------------------------------------------------------------- screens

bool APenguinHUD::ScreenDims(EPinguScreen InScreen)
{
	return InScreen == EPinguScreen::Pause || InScreen == EPinguScreen::Results || InScreen == EPinguScreen::Controls
		|| InScreen == EPinguScreen::Lan || InScreen == EPinguScreen::Settings || InScreen == EPinguScreen::Pingupedia;
}

float APenguinHUD::GetScreenAppear() const
{
	return PinguUI::EaseOut(static_cast<float>(PinguUI::Now() - ScreenChangedAt) / FPinguStyle::ScreenInTime);
}

float APenguinHUD::GetDimAppear() const
{
	return bPrevScreenDimmed ? 1.f : GetScreenAppear();
}

EPinguScreen APenguinHUD::ComputeBaseScreen() const
{
	using namespace PenguinHUDPrivate;
	const UWorld* World = GetWorld();
	const APenguinGameState* GameState = GetGameState();
	if (!World || !GameState || GameState->IsMatchTest())
	{
		return EPinguScreen::None;
	}
	const bool bNetwork = IsNetworkGame();
	switch (GameState->GetPhase())
	{
	case EPinguPhase::Menu:
		// While a join is under way the world already reports itself as a client (the engine's pending net game): keep the
		// menus, so the LAN screen shows "Connecting..." with its CANCEL. Once in the host's world, nothing until its state.
		return bNetwork && !IsConnecting() ? EPinguScreen::None : EPinguScreen::MainMenu;
	case EPinguPhase::Lobby:
		return bLobbyMenuOpen ? EPinguScreen::Lobby : EPinguScreen::None;
	case EPinguPhase::MatchOver:
		return EPinguScreen::Results;
	default:
		if (bNetwork)
		{
			return bLocalMenuOpen ? EPinguScreen::Pause : EPinguScreen::None;
		}
		return GameState->IsGamePaused() || World->IsPaused() ? EPinguScreen::Pause : EPinguScreen::None;
	}
}

void APenguinHUD::SetTestScreen(EPinguScreen InScreen)
{
	bTestScreen = InScreen != EPinguScreen::None;
	ShotScreen = InScreen;
	UpdateScreen();
}

void APenguinHUD::UpdateScreen()
{
	using namespace PenguinHUDPrivate;
	const EPinguPhase Phase = GetPhase();
	// Per-visit menus: the lobby card opens again on the next lobby, the LAN pause menu closes with the match.
	if (Phase != EPinguPhase::Lobby)
	{
		bLobbyMenuOpen = true;
		bLobbyStripFocus = false;
	}
	if (!IsInGamePhase(Phase))
	{
		bLocalMenuOpen = false;
	}
	// The controls help closes with the game HUD, so it never shows up on its own in the next match.
	if (Phase == EPinguPhase::Menu || Phase == EPinguPhase::MatchOver)
	{
		bShowHelp = false;
	}

	const EPinguScreen NewBase = ComputeBaseScreen();
	if (NewBase != BaseScreen)
	{
		// The game moved on (a match started, the results came, back in the lobby): sub-screens close with it.
		if (ScreenStack.Contains(EPinguScreen::Lan))
		{
			if (UPinguNetSubsystem* Net = GetNet())
			{
				Net->StopSearch();
			}
		}
		ScreenStack.Reset();
		bCapturing = false;
		if (ScreenFocus.IsValidIndex(static_cast<int32>(NewBase)))
		{
			ScreenFocus[static_cast<int32>(NewBase)].Reset();
		}
		BaseScreen = NewBase;
	}

	EPinguScreen NewScreen = ScreenStack.Num() > 0 ? ScreenStack.Last() : BaseScreen;
	if ((bMenuShots || bTestScreen) && ShotScreen != EPinguScreen::None)
	{
		NewScreen = ShotScreen;
	}
	if (NewScreen != Screen)
	{
		bPrevScreenDimmed = ScreenDims(Screen);
		Screen = NewScreen;
		ScreenChangedAt = PinguUI::Now();
		if (Root.IsValid())
		{
			Root->OnScreenShown(Screen);
		}
	}
	if (Screen != EPinguScreen::Controls)
	{
		bCapturing = false;
	}
	// The settings page's edit session (2026-11-07): runs while the settings or controls page shows; leaving them any other way
	// than through the board (the game moved on) discards what was not applied.
	if (Root.IsValid() && PinguMenu3D::IsEnabled())
	{
		UPinguSettings* Prefs = UPinguSettings::Get();
		if (Screen == EPinguScreen::Settings || Screen == EPinguScreen::Controls)
		{
			Prefs->BeginEditSession();
		}
		else
		{
			Prefs->EndEditSession(GetWorld());
			bSettingsConfirm = false;
		}
	}
}

void APenguinHUD::PushScreen(EPinguScreen NewScreen)
{
	if (!Root.IsValid() || bMenuShots || Screen == NewScreen)
	{
		return;
	}
	const int32 Existing = ScreenStack.Find(NewScreen);
	if (Existing != INDEX_NONE)
	{
		ScreenStack.SetNum(Existing + 1);
	}
	else
	{
		ScreenStack.Add(NewScreen);
	}
	if (ScreenFocus.IsValidIndex(static_cast<int32>(NewScreen)))
	{
		ScreenFocus[static_cast<int32>(NewScreen)].Reset(); // a fresh visit starts at the top
	}
	if (NewScreen == EPinguScreen::Lan)
	{
		if (UPinguNetSubsystem* Net = GetNet())
		{
			Net->StartSearch();
		}
	}
	UpdateScreen();
}

void APenguinHUD::PopScreen()
{
	if (ScreenStack.Num() == 0)
	{
		return;
	}
	const EPinguScreen Top = ScreenStack.Last();
	if (Top == EPinguScreen::Lan)
	{
		UPinguNetSubsystem* Net = GetNet();
		if (Net && IsConnecting())
		{
			Net->LeaveGame(); // BACK while joining cancels the join; the screen stays
			return;
		}
		if (Root.IsValid())
		{
			Root->CommitTextBoxes();
		}
		if (Net)
		{
			Net->StopSearch();
		}
	}
	if (Top == EPinguScreen::Settings && Root.IsValid())
	{
		// The "Apply changes?" board: a back while it shows goes back into the settings; a back with changes pending asks first.
		if (bSettingsConfirm)
		{
			bSettingsConfirm = false;
			return;
		}
		if (HasSettingsPending())
		{
			bSettingsConfirm = true;
			return;
		}
	}
	if (Top == EPinguScreen::Controls)
	{
		bCapturing = false;
	}
	ScreenStack.Pop();
	UpdateScreen();
}

const APenguinHUD* APenguinHUD::GetPrimaryHUD() const
{
	if (Root.IsValid())
	{
		return this;
	}
	const UWorld* World = GetWorld();
	const APlayerController* First = World ? World->GetFirstPlayerController() : nullptr;
	const APenguinHUD* FirstHUD = First ? Cast<APenguinHUD>(First->GetHUD()) : nullptr;
	return FirstHUD ? FirstHUD : this;
}

APenguinHUD* APenguinHUD::GetPrimaryHUD()
{
	return const_cast<APenguinHUD*>(static_cast<const APenguinHUD*>(this)->GetPrimaryHUD());
}

TSharedPtr<SWidget> APenguinHUD::GetFocusWidget() const
{
	const APenguinHUD* Primary = GetPrimaryHUD();
	if (Primary != this)
	{
		return Primary->GetFocusWidget();
	}
	if (!Root.IsValid())
	{
		return nullptr;
	}
	TSharedPtr<SWidget> Target;
	if (bCapturing)
	{
		Target = Root->GetCaptureWidget();
	}
	else if (HasErrorPopup())
	{
		Target = Root->GetErrorFocusTarget();
	}
	else if (Screen != EPinguScreen::None && (!IsLobbyIslandScreen() || bLobbyStripFocus))
	{
		const int32 ScreenIndex = static_cast<int32>(Screen);
		if (ScreenFocus.IsValidIndex(ScreenIndex))
		{
			Target = ScreenFocus[ScreenIndex].Pin();
		}
		if (!Target.IsValid() || !Target->SupportsKeyboardFocus())
		{
			Target = Root->GetFocusTarget(Screen);
		}
	}
	// Only ever a widget that takes the keyboard focus (null outside the menus).
	return Target.IsValid() && Target->SupportsKeyboardFocus() ? Target : nullptr;
}

bool APenguinHUD::IsMenuOpen() const
{
	const APenguinHUD* Primary = GetPrimaryHUD();
	if (Primary != this)
	{
		return Primary->IsMenuOpen();
	}
	// The lobby island's screen is no menu: the penguins run around under it (its buttons still take clicks).
	return Root.IsValid() && ((Screen != EPinguScreen::None && (!IsLobbyIslandScreen() || bLobbyStripFocus)) || bCapturing || HasErrorPopup());
}

bool APenguinHUD::IsLobbyStripFocused() const
{
	const APenguinHUD* Primary = GetPrimaryHUD();
	// (The phase too: the match starts a frame before this HUD's tick moves its screen on, the strip's keys end with the lobby.)
	return Primary->bLobbyStripFocus && Primary->IsLobbyIslandScreen() && Primary->GetPhase() == EPinguPhase::Lobby;
}

void APenguinHUD::SetLobbyStripFocus(bool bOn)
{
	APenguinHUD* Primary = GetPrimaryHUD();
	if (Primary != this)
	{
		Primary->SetLobbyStripFocus(bOn);
		return;
	}
	bOn = bOn && GetPhase() == EPinguPhase::Lobby && PinguMenu3D::UseIslandLobby();
	if (bOn == bLobbyStripFocus)
	{
		return;
	}
	bLobbyStripFocus = bOn;
	// The strip's hot button: its default (START for the host / solo) when the keys come to it.
	if (APinguMenu3D* Menu3D = bOn ? APinguMenu3D::Get(this) : nullptr)
	{
		Menu3D->FocusDefault();
	}
	UE_LOG(LogPingu, Display, TEXT("[LOBBY] the button strip %s the keys"), bOn ? TEXT("has") : TEXT("gives back"));
}

bool APenguinHUD::IsLobbyIslandScreen() const
{
	const APenguinHUD* Primary = GetPrimaryHUD();
	return Primary->Screen == EPinguScreen::Lobby && PinguMenu3D::UseIslandLobby();
}

void APenguinHUD::TogglePauseMenu()
{
	using namespace PenguinHUDPrivate;
	APenguinHUD* Primary = GetPrimaryHUD();
	if (Primary != this)
	{
		Primary->TogglePauseMenu();
		return;
	}
	if (!Root.IsValid() || bMenuShots)
	{
		return;
	}
	// The screen of this very moment (the phase may have moved on since the last tick).
	UpdateScreen();
	// Innermost first: the key being bound, the error popup, a sub-screen.
	if (bCapturing)
	{
		CancelKeyCapture();
		return;
	}
	if (HasErrorPopup())
	{
		PlayBackSound();
		Action_DismissError();
		return;
	}
	if (ScreenStack.Num() > 0)
	{
		PlayBackSound();
		PopScreen();
		return;
	}
	const APenguinGameState* GameState = GetGameState();
	if (!GameState || IsMatchTest())
	{
		return;
	}
	switch (BaseScreen)
	{
	case EPinguScreen::Lobby:
		if (PinguMenu3D::UseIslandLobby())
		{
			// The lobby island: everybody runs around already, its 3D buttons stay. Esc / Start gives the strip the keys (a
			// keyboard or gamepad player reaches START that way); again (or B) hands them back to the island.
			if (bLobbyStripFocus)
			{
				PlayBackSound();
			}
			else
			{
				PlayUISound(true);
			}
			SetLobbyStripFocus(!bLobbyStripFocus);
			break;
		}
		// The lobby card closes: run around on the island (Esc brings it back). (The wooden card lobby too, 2026-10-29 - Linus:
		// "keep the the function of the playing in the lobby when pressig esc".)
		PlayBackSound();
		bLobbyMenuOpen = false;
		break;
	case EPinguScreen::Pause:
		PlayBackSound();
		Action_Resume();
		return;
	case EPinguScreen::None:
	{
		const EPinguPhase Phase = GameState->GetPhase();
		if (Phase == EPinguPhase::Lobby)
		{
			PlayUISound(true);
			bLobbyMenuOpen = true;
		}
		else if (IsInGamePhase(Phase))
		{
			PlayUISound(true);
			if (IsNetworkGame())
			{
				bLocalMenuOpen = true; // LAN: a local menu, the match goes on (the penguin gets no input)
			}
			else if (APenguinGameMode* GameMode = GetPenguinGameMode())
			{
				GameMode->SetPaused(true); // solo: the world stops
			}
		}
		break;
	}
	default:
		// Title screen and results: nothing to toggle.
		return;
	}
	UpdateScreen();
}

void APenguinHUD::ToggleHelp()
{
	APenguinHUD* Primary = GetPrimaryHUD();
	if (Primary != this)
	{
		Primary->ToggleHelp();
		return;
	}
	bShowHelp = !bShowHelp;
}

void APenguinHUD::UpdateFocus()
{
	if (!Root.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedPtr<SWidget> Focused = Slate.GetUserFocusedWidget(0);
	const bool bPopup = HasErrorPopup();
	const int32 ScreenIndex = static_cast<int32>(Screen);

	// Remember where the player is on this screen (to come back to it from a sub-screen).
	if (Screen != EPinguScreen::None && !bCapturing && !bPopup && Focused.IsValid() && ScreenFocus.IsValidIndex(ScreenIndex))
	{
		const TSharedPtr<SWidget> ScreenWidget = Root->GetScreenWidget(Screen);
		if (ScreenWidget.IsValid() && ScreenWidget->HasUserFocusedDescendants(0))
		{
			ScreenFocus[ScreenIndex] = Focused;
		}
	}

	const TSharedPtr<SWidget> Want = GetFocusWidget();
	const uint32 StateKey = static_cast<uint32>(ScreenIndex) | (bCapturing ? 0x100u : 0u) | (bPopup ? 0x200u : 0u) | (bLobbyStripFocus ? 0x400u : 0u);
	if (!Want.IsValid())
	{
		FocusedStateKey = StateKey;
		return;
	}
	// Take the focus when the menu changed, or when it fell into the game viewport (a click on nothing). Never out of
	// another window or panel (the editor, another PIE instance): only when nothing or our own viewport has it.
	TSharedPtr<SWidget> GameViewportWidget;
	if (UGameViewportClient* Viewport = RootViewport.Get())
	{
		GameViewportWidget = Viewport->GetGameViewportWidget();
	}
	// A control of ours that got disabled under the focus (HOST GAME while joining): keys skip disabled widgets, so
	// the focus moves on to what the screen wants now.
	const bool bDisabledHere = Focused.IsValid() && !Focused->IsEnabled() && Root->HasUserFocusedDescendants(0);
	const bool bLost = !Focused.IsValid() || Focused.Get() == GameViewportWidget.Get() || bDisabledHere;
	if (StateKey == FocusedStateKey && !bLost)
	{
		return;
	}
	Slate.SetAllUserFocus(Want, EFocusCause::SetDirectly);
	if (Want->HasAnyUserFocus().IsSet())
	{
		FocusedStateKey = StateKey;
	}
	else if (ScreenFocus.IsValidIndex(ScreenIndex) && ScreenFocus[ScreenIndex].Pin() == Want)
	{
		ScreenFocus[ScreenIndex].Reset(); // it went away: the screen's first control next frame
	}
}

void APenguinHUD::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// Real time: the UI keeps its pace through pauses and slow motion.
	const float RealDelta = FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.1f);
	UITime += RealDelta;
	if (!Root.IsValid())
	{
		return;
	}
	UpdateScreen();
	UpdateFocus();
	if (bMenuShots)
	{
		TickMenuShots(RealDelta);
	}
}

void APenguinHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!bShowDebug || !Canvas || !GetWorld() || PlayerOwner != GetWorld()->GetFirstPlayerController())
	{
		return;
	}
	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	float Y = Canvas->ClipY - 140.f;
	int32 Index = 1;
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It, ++Index)
	{
		const UPenguinMovementComponent* Move = It->GetPenguinMovement();
		if (!Move)
		{
			continue;
		}
		const FString State = Move->IsDashing() ? TEXT("dash") : Move->IsBellySliding() ? TEXT("slide")
			: Move->IsFalling() ? TEXT("air") : TEXT("ground");
		DrawText(FString::Printf(TEXT("%s  speed %4.0f  vz %5.0f  %s%s%s"), *It->GetDisplayName(), FMath::Abs(Move->Velocity.X), Move->Velocity.Z, *State,
			Move->IsStunned() ? TEXT(" stunned") : TEXT(""), Move->IsFrozen() ? TEXT(" frozen") : TEXT("")),
			FLinearColor(0.7f, 0.9f, 1.f), 20.f, Y, Font, 1.f);
		Y += 18.f;
	}
}

// ---------------------------------------------------------------------------- key rebinding

bool APenguinHUD::IsCapturingKey(EPinguInputAction Action, int32 Index) const
{
	return bCapturing && CaptureAction == Action && CaptureIndex == Index;
}

void APenguinHUD::BeginKeyCapture(EPinguInputAction Action, int32 Index)
{
	bCapturing = true;
	CaptureAction = Action;
	CaptureIndex = FMath::Clamp(Index, 0, 1);
	CaptureNote = FText::GetEmpty();
	CaptureStartTime = PinguUI::Now();
}

void APenguinHUD::HandleCapturedKey(const FKey& Key)
{
	if (!bCapturing || !Key.IsValid())
	{
		return;
	}
	// The click that opened the capture (or a double click on the key cap) is not the new key.
	if (Key.IsMouseButton() && PinguUI::Now() - CaptureStartTime < 0.2)
	{
		return;
	}
	if (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right || Key == EKeys::Gamepad_Special_Right)
	{
		CancelKeyCapture();
		return;
	}
	UPinguSettings* Settings = UPinguSettings::Get();
	if (!Settings)
	{
		CancelKeyCapture();
		return;
	}
	if (Key == EKeys::BackSpace || Key == EKeys::Delete)
	{
		Settings->ClearKey(CaptureAction, CaptureIndex);
		bCapturing = false;
		PlayUISound(true);
		return;
	}
	if (!UPinguSettings::CanBind(Key))
	{
		CaptureNote = FText::Format(LOCTEXT("CannotBind", "{0} can't be used here. Pick another key."), UPinguSettings::GetKeyLabel(Key));
		CaptureNoteTime = PinguUI::Now();
		PlayBackSound();
		return;
	}
	// A key another action had moves over to this one (UPinguSettings takes it away there).
	Settings->SetKey(CaptureAction, CaptureIndex, Key);
	bCapturing = false;
	PlayUISound(true);
}

void APenguinHUD::CancelKeyCapture()
{
	if (bCapturing)
	{
		bCapturing = false;
		PlayBackSound();
	}
}

FText APenguinHUD::GetCaptureMessage() const
{
	if (!CaptureNote.IsEmpty() && PinguUI::Now() - CaptureNoteTime < 2.5)
	{
		return CaptureNote;
	}
	const FText Action = FText::FromString(UPinguSettings::GetActionName(CaptureAction).ToString().ToUpper());
	return CaptureIndex == 0
		? FText::Format(LOCTEXT("CaptureFirst", "for {0}"), Action)
		: FText::Format(LOCTEXT("CaptureSecond", "for {0} (other key)"), Action);
}

// ---------------------------------------------------------------------------- sounds

void APenguinHUD::PlayUISound(bool bClick)
{
	// Local: UI sounds are never sent to the other players.
	APinguFX::Sound(this, bClick ? EPinguSound::UIClick : EPinguSound::UIHover, bClick ? 1.f : 0.6f, 0.03f);
}

void APenguinHUD::PlayBackSound()
{
	APinguFX::Sound(this, EPinguSound::UIBack, 0.9f, 0.03f);
}

// ---------------------------------------------------------------------------- actions

void APenguinHUD::ApplyMatchSettings(const FPinguMatchSettings& NewSettings)
{
	if (APenguinGameMode* GameMode = GetPenguinGameMode())
	{
		GameMode->SetMatchSettings(NewSettings);
	}
}

void APenguinHUD::Action_Play()
{
	if (APenguinGameMode* GameMode = GetPenguinGameMode())
	{
		// A lobby of your own: your penguin on the island, the bots on their cards (the last ones you had).
		ScreenStack.Reset();
		bLobbyMenuOpen = true;
		GameMode->EnterLobby();
	}
}

void APenguinHUD::Action_Controls()
{
	PushScreen(EPinguScreen::Controls);
}

void APenguinHUD::Action_CloseControls()
{
	if (ScreenStack.Num() > 0 && ScreenStack.Last() == EPinguScreen::Controls)
	{
		PopScreen();
	}
}

void APenguinHUD::Action_Settings()
{
	PushScreen(EPinguScreen::Settings);
}

bool APenguinHUD::HasSettingsPending() const
{
	const UPinguSettings* Prefs = UPinguSettings::Get();
	return PinguMenu3D::IsEnabled() && Prefs && Prefs->HasPendingChanges();
}

bool APenguinHUD::HasSettingsConfirm() const
{
	return GetPrimaryHUD()->bSettingsConfirm;
}

void APenguinHUD::Action_ApplySettings()
{
	if (UPinguSettings* Prefs = UPinguSettings::Get())
	{
		PlayUISound(true);
		Prefs->ApplyEditSession();
	}
}

void APenguinHUD::Action_ConfirmApply()
{
	APenguinHUD* Primary = GetPrimaryHUD();
	if (Primary != this)
	{
		Primary->Action_ConfirmApply();
		return;
	}
	if (!bSettingsConfirm)
	{
		return;
	}
	if (UPinguSettings* Prefs = UPinguSettings::Get())
	{
		Prefs->ApplyEditSession();
	}
	bSettingsConfirm = false;
	PlayBackSound();
	PopScreen();
}

void APenguinHUD::Action_ConfirmDiscard()
{
	APenguinHUD* Primary = GetPrimaryHUD();
	if (Primary != this)
	{
		Primary->Action_ConfirmDiscard();
		return;
	}
	if (!bSettingsConfirm)
	{
		return;
	}
	if (UPinguSettings* Prefs = UPinguSettings::Get())
	{
		Prefs->DiscardEditSession(GetWorld());
	}
	bSettingsConfirm = false;
	PlayBackSound();
	PopScreen();
}

void APenguinHUD::Action_CloseConfirm()
{
	APenguinHUD* Primary = GetPrimaryHUD();
	if (Primary != this)
	{
		Primary->Action_CloseConfirm();
		return;
	}
	if (bSettingsConfirm)
	{
		bSettingsConfirm = false;
		PlayBackSound();
	}
}

void APenguinHUD::Action_Lan()
{
	// From the solo title screen only (LAN games are entered from there).
	if (BaseScreen == EPinguScreen::MainMenu)
	{
		PushScreen(EPinguScreen::Lan);
	}
}

void APenguinHUD::Action_Pingupedia()
{
	// From the solo title screen only.
	if (BaseScreen == EPinguScreen::MainMenu)
	{
		PushScreen(EPinguScreen::Pingupedia);
	}
}

bool APenguinHUD::GetPingupediaView(int32& OutPage, int32& OutEntry) const
{
	const APenguinHUD* Primary = GetPrimaryHUD();
	if (Primary != this)
	{
		return Primary->GetPingupediaView(OutPage, OutEntry);
	}
	OutPage = -1;
	OutEntry = -1;
	return Root.IsValid() && Root->GetPingupediaView(OutPage, OutEntry);
}

void APenguinHUD::SelectPingupediaPage(int32 Page, int32 Entry, bool bFocus)
{
	APenguinHUD* Primary = GetPrimaryHUD();
	if (Primary != this)
	{
		Primary->SelectPingupediaPage(Page, Entry, bFocus);
		return;
	}
	if (Root.IsValid())
	{
		Root->SelectPingupediaPage(Page, Entry, bFocus);
	}
}

void APenguinHUD::Action_ToggleTraits()
{
	// The host (and solo) only: the match settings, which replicate. A LAN client's lobby shows the host's choice without a
	// button (Linus: the host decides), so nothing of a client's goes to the host.
	if (bMenuShots)
	{
		return;
	}
	if (APenguinGameMode* GameMode = GetPenguinGameMode())
	{
		GameMode->SetTraitsEnabled(!AreTraitsOn());
	}
}

void APenguinHUD::Action_CopyHostAddress()
{
	// The same text the line shows (the best of this PC's adapters, see UPinguNetSubsystem::GetLocalAddress), so what lands on
	// the clipboard is what a friend types under JOIN BY ADDRESS. COPIED! shows only when the clipboard really holds it now.
	const FString Address = GetHostAddressText();
	if (Address.IsEmpty())
	{
		return;
	}
	const bool bCopied = CopyTextToClipboard(Address);
	if (bCopied)
	{
		HostAddressCopiedAt = PinguUI::Now();
	}
	UE_LOG(LogPingu, Display, TEXT("[UI] copy the host address %s: %s"), *Address, bCopied ? TEXT("on the clipboard") : TEXT("the clipboard was busy"));
}

void APenguinHUD::Action_Back()
{
	if (bCapturing)
	{
		CancelKeyCapture();
		return;
	}
	if (ScreenStack.Num() > 0)
	{
		PlayBackSound();
		PopScreen();
	}
}

void APenguinHUD::Action_Quit()
{
	UKismetSystemLibrary::QuitGame(this, PlayerOwner, EQuitPreference::Quit, false);
}

void APenguinHUD::Action_Resume()
{
	ScreenStack.Reset();
	if (IsNetworkGame())
	{
		bLocalMenuOpen = false;
	}
	else if (APenguinGameMode* GameMode = GetPenguinGameMode())
	{
		GameMode->SetPaused(false);
	}
	UpdateScreen();
}

void APenguinHUD::Action_Restart()
{
	APenguinGameMode* GameMode = GetPenguinGameMode();
	if (!GameMode || IsNetworkGame())
	{
		return;
	}
	ScreenStack.Reset();
	GameMode->SetPaused(false);
	if (GetPhase() == EPinguPhase::Practice)
	{
		GameMode->StartPractice();
	}
	else
	{
		GameMode->Rematch();
	}
}

void APenguinHUD::Action_MainMenu()
{
	if (IsNetworkGame())
	{
		Action_LeaveGame(); // a LAN game has no title screen of its own
		return;
	}
	if (APenguinGameMode* GameMode = GetPenguinGameMode())
	{
		ScreenStack.Reset();
		GameMode->SetPaused(false);
		GameMode->EnterMenu();
	}
}

void APenguinHUD::Action_Rematch()
{
	if (APenguinGameMode* GameMode = GetPenguinGameMode())
	{
		GameMode->Rematch();
	}
}

bool APenguinHUD::CanEndMatch() const
{
	if (bMenuShots && ShotScreen == EPinguScreen::Pause)
	{
		return true; // the pause screenshot: as in a solo match
	}
	const EPinguPhase Phase = GetPhase();
	return HasMatchAuthority() && (Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown || Phase == EPinguPhase::Fight || Phase == EPinguPhase::RoundOver);
}

void APenguinHUD::Action_EndMatch()
{
	APenguinGameMode* GameMode = GetPenguinGameMode();
	if (!GameMode || !CanEndMatch())
	{
		return;
	}
	ScreenStack.Reset();
	bLocalMenuOpen = false;
	if (GameMode->EndMatchNow())
	{
		UpdateScreen();
	}
}

void APenguinHUD::Action_AddBot()
{
	if (!CanAddBot())
	{
		return;
	}
	FPinguMatchSettings MatchSettings = GetMatchSettings();
	MatchSettings.Bots.SetNum(GetBotCount());
	FPinguBotSetup NewBot;
	NewBot.Skill = MatchSettings.Bots.Num() > 0 ? MatchSettings.Bots.Last().Skill : 1; // the game mode names it
	MatchSettings.Bots.Add(NewBot);
	MatchSettings.NumBots = MatchSettings.Bots.Num();
	ApplyMatchSettings(MatchSettings);
}

void APenguinHUD::Action_RemoveBot(int32 BotIndex)
{
	FPinguMatchSettings MatchSettings = GetMatchSettings();
	if (!CanRemoveBot() || !MatchSettings.Bots.IsValidIndex(BotIndex))
	{
		return;
	}
	MatchSettings.Bots.RemoveAt(BotIndex);
	MatchSettings.NumBots = MatchSettings.Bots.Num();
	ApplyMatchSettings(MatchSettings);
}

bool APenguinHUD::CanKick(const FPinguLobbyPlayer& Card) const
{
	return HasMatchAuthority() && IsNetworkGame() && !Card.bBot && !Card.bYou && !Card.bHost && Card.PlayerId != INDEX_NONE;
}

TArray<FPinguLobbyPlayer> APenguinHUD::GetKickTargets() const
{
	TArray<FPinguLobbyPlayer> Targets;
	for (const FPinguLobbyPlayer& Player : GetLobbyPlayers())
	{
		if (CanKick(Player))
		{
			Targets.Add(Player);
		}
	}
	return Targets;
}

void APenguinHUD::Action_KickPlayer(int32 PlayerId)
{
	APenguinGameMode* GameMode = GetPenguinGameMode();
	const APenguinGameState* GameState = GetGameState();
	if (bMenuShots || !GameMode || !GameState || !HasMatchAuthority())
	{
		return;
	}
	for (APlayerState* PlayerState : GameState->PlayerArray)
	{
		if (PlayerState && PlayerState->GetPlayerId() == PlayerId)
		{
			GameMode->KickPlayer(Cast<APenguinPlayerState>(PlayerState));
			return;
		}
	}
}

void APenguinHUD::Action_ChangeBotTeam(int32 BotIndex, int32 Delta)
{
	FPinguMatchSettings MatchSettings = GetMatchSettings();
	if (!HasMatchAuthority() || !MatchSettings.Bots.IsValidIndex(BotIndex))
	{
		return;
	}
	// NO TEAM (-1), RED, BLUE, ... BROWN: as an index 0..PinguMaxTeams, round and round.
	int32& Team = MatchSettings.Bots[BotIndex].Team;
	Team = PenguinHUDPrivate::StepValue(FMath::Clamp(Team, -1, PinguMaxTeams - 1) + 1, Delta, 0, PinguMaxTeams, true) - 1;
	ApplyMatchSettings(MatchSettings);
}

void APenguinHUD::Action_ChangeBotSkill(int32 BotIndex, int32 Delta)
{
	FPinguMatchSettings MatchSettings = GetMatchSettings();
	if (!HasMatchAuthority() || !MatchSettings.Bots.IsValidIndex(BotIndex))
	{
		return;
	}
	int32& Skill = MatchSettings.Bots[BotIndex].Skill;
	Skill = PenguinHUDPrivate::StepValue(FMath::Clamp(Skill, 0, 2), Delta, 0, 2, true);
	ApplyMatchSettings(MatchSettings);
}

void APenguinHUD::Action_ChangeTeamChoice(int32 Delta, bool bWrap)
{
	// NO TEAM (-1), RED, BLUE, ... BROWN (as an index 0..PinguMaxTeams).
	const int32 Next = PenguinHUDPrivate::StepValue(FMath::Clamp(GetLocalTeamChoice(), -1, PinguMaxTeams - 1) + 1, Delta, 0, PinguMaxTeams, bWrap) - 1;
	if (APenguinPlayerController* Controller = Cast<APenguinPlayerController>(PlayerOwner))
	{
		Controller->RequestTeamChoice(Next);
	}
}

void APenguinHUD::Action_HostGame()
{
	if (UPinguNetSubsystem* Net = GetNet())
	{
		UE_LOG(LogPingu, Display, TEXT("[UI] host a LAN game as \"%s\""), *GetPlayerName());
		Net->HostGame(); // reloads the arena as a listen server: this HUD goes with the old world
	}
}

void APenguinHUD::Action_JoinGame(const FString& Address)
{
	const FString Trimmed = Address.TrimStartAndEnd();
	UPinguNetSubsystem* Net = GetNet();
	if (Trimmed.IsEmpty() || !Net)
	{
		PlayBackSound(); // nothing typed
		return;
	}
	JoinTarget = Trimmed;
	UE_LOG(LogPingu, Display, TEXT("[UI] join %s"), *Trimmed);
	Net->JoinGame(Trimmed); // a bad address sets the error (the popup shows it)
}

void APenguinHUD::Action_JoinFound(const FString& Address)
{
	UPinguNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return;
	}
	JoinTarget = Address;
	for (const FPinguLanGame& Game : GetFoundGames())
	{
		if (Game.Address == Address)
		{
			JoinTarget = FText::Format(LOCTEXT("JoinTargetGame", "{0}'s game"), FText::FromString(Game.HostName)).ToString();
			if (Game.Players >= Game.MaxPlayers)
			{
				return; // full: the row says so (a running match is fine: you play from its next round)
			}
		}
	}
	UE_LOG(LogPingu, Display, TEXT("[UI] join %s"), *Address);
	Net->JoinGame(Address);
}

void APenguinHUD::Action_CancelJoin()
{
	UPinguNetSubsystem* Net = GetNet();
	if (Net && IsConnecting())
	{
		Net->LeaveGame();
	}
}

void APenguinHUD::Action_LeaveGame()
{
	if (UPinguNetSubsystem* Net = GetNet())
	{
		UE_LOG(LogPingu, Display, TEXT("[UI] leave the LAN game"));
		Net->LeaveGame();
	}
}

void APenguinHUD::Action_SetPlayerName(const FString& Name)
{
	const FString Trimmed = Name.TrimStartAndEnd().Left(20);
	UPinguSettings* Settings = UPinguSettings::Get();
	if (Settings && !Trimmed.IsEmpty() && Trimmed != Settings->GetPlayerName())
	{
		Settings->SetPlayerName(Trimmed);
	}
}

void APenguinHUD::Action_StartLobbyMatch()
{
	if (APenguinGameMode* GameMode = GetPenguinGameMode())
	{
		// A match from the lobby is always endless: rounds until somebody picks END MATCH in the menu.
		FPinguMatchSettings MatchSettings = GetMatchSettings();
		MatchSettings.NumBots = FMath::Clamp(MatchSettings.NumBots, GetMinBots(), GetMaxBots());
		MatchSettings.bEndless = true;
		GameMode->StartMatch(MatchSettings);
	}
}

void APenguinHUD::Action_ReturnToLobby()
{
	if (APenguinGameMode* GameMode = GetPenguinGameMode())
	{
		GameMode->ReturnToLobby();
	}
}

void APenguinHUD::Action_WarmUp()
{
	bLobbyMenuOpen = false;
	ScreenStack.Reset();
	UpdateScreen();
}

void APenguinHUD::Action_DismissError()
{
	if (bMenuShots)
	{
		bShotError = false;
		return;
	}
	if (UPinguNetSubsystem* Net = GetNet())
	{
		Net->ClearLastError();
	}
}

void APenguinHUD::Action_ResetKeys()
{
	if (UPinguSettings* Settings = UPinguSettings::Get())
	{
		Settings->ResetKeyBindings();
	}
}

// ---------------------------------------------------------------------------- -PinguMenuShots

void APenguinHUD::TickPingupediaKeys()
{
	using namespace PenguinHUDPrivate;
	// Real key events through Slate, as a player's keyboard / gamepad sends them, from the Jester row of the traits page:
	// Up to the trait above, LB to the abilities (same place in the list), Right back to the traits, d-pad down to Jester.
	struct FPediaKey
	{
		FKey Key;
		int32 Page;
		int32 Entry;
		const TCHAR* What;
	};
	static const FPediaKey Keys[PediaKeyCount] = {
		{ EKeys::Up, 1, ShotJesterEntry - 1, TEXT("Up") },
		{ EKeys::Gamepad_LeftShoulder, 0, ShotJesterEntry - 1, TEXT("LB") },
		{ EKeys::Right, 1, ShotJesterEntry - 1, TEXT("Right") },
		{ EKeys::Gamepad_DPad_Down, 1, ShotJesterEntry, TEXT("d-pad down") },
	};
	if (PediaKeyStep > 0 && PediaKeyStep <= PediaKeyCount)
	{
		const FPediaKey& Last = Keys[PediaKeyStep - 1];
		int32 PediaPage = -1;
		int32 PediaEntry = -1;
		GetPingupediaView(PediaPage, PediaEntry);
		const bool bLanded = PediaPage == Last.Page && PediaEntry == Last.Entry;
		PediaKeysOk += bLanded ? 1 : 0;
		PediaKeysLog += FString::Printf(TEXT("%s%s -> page %d entry %d%s"), PediaKeysLog.IsEmpty() ? TEXT("") : TEXT(", "), Last.What, PediaPage, PediaEntry,
			bLanded ? TEXT("") : TEXT(" (wrong)"));
	}
	if (PediaKeyStep < PediaKeyCount && FSlateApplication::IsInitialized())
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		const FKeyEvent Event(Keys[PediaKeyStep].Key, Slate.GetModifierKeys(), 0u, false, 0u, 0u);
		Slate.ProcessKeyDownEvent(Event);
		Slate.ProcessKeyUpEvent(Event);
	}
	else if (PediaKeyStep == PediaKeyCount)
	{
		UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT pingupedia keys: %s: %d of %d %s"), *PediaKeysLog, PediaKeysOk, PediaKeyCount,
			PediaKeysOk == PediaKeyCount ? TEXT("ok") : TEXT("FAIL"));
	}
	++PediaKeyStep;
}

void APenguinHUD::CheckCopyAddressShot()
{
	// What the player had copied (text only: an image on the clipboard cannot be put back) comes back at the end.
	const FString Saved = ReadClipboardText();
	const FString Shown = GetHostAddressText();
	const FString SentinelAction = TEXT("pingu test: not the address (action)");
	const FString SentinelKey = TEXT("pingu test: not the address (Enter)");

	// 1. The action itself, as the line's click calls it.
	CopyTextToClipboard(SentinelAction);
	Action_CopyHostAddress();
	const FString AfterAction = ReadClipboardText();
	const bool bActionOk = !Shown.IsEmpty() && AfterAction == Shown;

	// 2. A real Enter key on the focused line: the button's confirm goes through the same widget the mouse presses.
	CopyTextToClipboard(SentinelKey);
	bool bFocused = false;
	APinguMenu3D* Menu3D = PinguMenu3D::IsEnabled() ? APinguMenu3D::Get(this) : nullptr;
	if (Menu3D && Menu3D->GetPage() == EPinguMenu3DPage::Lobby)
	{
		// The 3D lobby: the strip gets the keys (as Esc does on the island), the focus goes to the line, then a real Enter
		// through Slate (the strip's input layer presses the hot 3D button: "focus" = the layer and the line had it).
		SetLobbyStripFocus(true);
		const bool bLine = Menu3D->FocusButton(FName(TEXT("Address"))) && Menu3D->GetHotId() == FName(TEXT("Address"));
		const TSharedPtr<SWidget> Layer = GetFocusWidget();
		if (Layer.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication& Slate = FSlateApplication::Get();
			Slate.SetAllUserFocus(Layer, EFocusCause::Navigation);
			bFocused = bLine && Layer->HasAnyUserFocus().IsSet();
			const FKeyEvent Event(EKeys::Enter, Slate.GetModifierKeys(), 0u, false, 0u, 0u);
			Slate.ProcessKeyDownEvent(Event);
			Slate.ProcessKeyUpEvent(Event);
		}
		SetLobbyStripFocus(false);
	}
	else if (Root.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		if (const TSharedPtr<SWidget> Line = Root->GetLobbyAddressWidget())
		{
			Slate.SetAllUserFocus(Line, EFocusCause::Navigation);
			bFocused = Line->HasAnyUserFocus().IsSet();
			const FKeyEvent Event(EKeys::Enter, Slate.GetModifierKeys(), 0u, false, 0u, 0u);
			Slate.ProcessKeyDownEvent(Event);
			Slate.ProcessKeyUpEvent(Event);
		}
	}
	const FString AfterKey = ReadClipboardText();
	const bool bKeyOk = bFocused && !Shown.IsEmpty() && AfterKey == Shown;
	const bool bFeedback = IsHostAddressCopied();

	const bool bRestored = CopyTextToClipboard(Saved);
	const bool bOk = bActionOk && bKeyOk && bFeedback && bRestored;
	const TCHAR* How = Menu3D && Menu3D->GetPage() == EPinguMenu3DPage::Lobby ? TEXT("a real Enter on the focused 3D line (focus") : TEXT("a real Enter on the focused line (focus");
	UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT copy address: the line shows \"%s\"; the action put \"%s\" on the clipboard (%s), %s %d) put \"%s\" (%s); the line says COPIED! %d; the clipboard text from before put back (%d characters) %d: %s"),
		*Shown, *AfterAction, bActionOk ? TEXT("same") : TEXT("DIFFERENT"), How, bFocused ? 1 : 0, *AfterKey, bKeyOk ? TEXT("same") : TEXT("DIFFERENT"),
		bFeedback ? 1 : 0, Saved.Len(), bRestored ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL"));
}

void APenguinHUD::PrepareMenuShotWorld()
{
	using namespace PenguinHUDPrivate;
	APenguinGameMode* GameMode = GetPenguinGameMode();
	if (!GameMode || !PinguMenu3D::UseIslandLobby())
	{
		return;
	}
	// The lobby shots show the lobby island behind their buttons: the real lobby (your penguin and some bots - ten penguins
	// in teams for the big one); the other screens the title's bots' demo again.
	if (ShotScreen == EPinguScreen::Lobby)
	{
		const int32 Bots = ShotVariant == 1 ? PinguMaxPenguins - 1 : 3;
		FPinguMatchSettings Settings = GameMode->GetSettings();
		if (Settings.Bots.Num() != Bots || GetPhase() != EPinguPhase::Lobby)
		{
			Settings.Bots.SetNum(Bots);
			Settings.NumBots = Bots;
			for (int32 Index = 0; Index < Bots; ++Index)
			{
				Settings.Bots[Index].Team = ShotVariant == 1 ? ShotTeamChoiceOf(Index + 1) : (Index == 0 ? 1 : -1);
				Settings.Bots[Index].Skill = Index % 3;
			}
			GameMode->SetMatchSettings(Settings);
			if (GetPhase() != EPinguPhase::Lobby)
			{
				GameMode->EnterLobby();
			}
		}
	}
	else if (GetPhase() == EPinguPhase::Lobby)
	{
		GameMode->EnterMenu();
	}
}

void APenguinHUD::TickMenuShots(float RealDeltaSeconds)
{
	using namespace PenguinHUDPrivate;
	ShotClock += RealDeltaSeconds;
	if (ShotIndex < 0)
	{
		// The title screen as the player sees it: the menu demo running, the shaders compiled (like -PinguMenuShot).
		const bool bCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
		if (GetPhase() != EPinguPhase::Menu || ShotClock < 3.f || (bCompiling && ShotClock < 120.f))
		{
			return;
		}
		ShotIndex = 0;
		ShotTimer = 0.f;
		bShotTaken = false;
		ShotScreen = MenuShots[0].Screen;
		bShotError = MenuShots[0].bError;
		bShotClient = MenuShots[0].bClient;
		ShotVariant = MenuShots[0].Variant;
		bShotTraits = MenuShots[0].bTraits;
		UpdateScreen();
		return;
	}
	ShotTimer += RealDeltaSeconds;
	if (ShotIndex >= NumMenuShots)
	{
		if (ShotTimer > ShotWrite)
		{
			UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT %d screens"), NumMenuShots);
			bMenuShots = false;
			PinguEndTestRun(this, TEXT("PinguMenuShots"));
		}
		return;
	}
	if (!bShotTaken && !bShotCopyDone && MenuShots[ShotIndex].bCopy && ShotTimer >= ShotSettle - ShotCopyLead)
	{
		bShotCopyDone = true;
		CheckCopyAddressShot();
	}
	// The frame time of this screen (and of the world alone) before its picture.
	const int32 Perf = PerfKind(MenuShots[ShotIndex].Name);
	const float Settle = ShotSettle + PerfExtra(Perf);
	static double PerfLastReal = 0.0;
	const double PerfNow = FPlatformTime::Seconds();
	const double PerfReal = PerfLastReal > 0.0 ? FMath::Min(PerfNow - PerfLastReal, 0.5) : 0.0;
	PerfLastReal = PerfNow;
	if (Perf > 0 && !bShotTaken)
	{
		APinguMenu3D* Menu = APinguMenu3D::Find(GetWorld());
		const float T = ShotTimer - ShotSettle;
		const float End1 = PerfSettle + PerfWindow;
		if (PerfStage == 0 && T >= PerfSettle)
		{
			PerfMenu = FPerfSum();
			PerfWorld = FPerfSum();
			PerfStage = 1;
		}
		if (PerfStage == 1)
		{
			PerfMenu.Add(PerfReal);
			if (T >= End1)
			{
				LogPerf(MenuShots[ShotIndex].Name, TEXT("with the menu"), PerfMenu, Menu);
				PerfStage = 2;
				if (Perf == 2 && Menu)
				{
					Menu->SetHiddenForTest(true);
				}
			}
		}
		else if (PerfStage == 2 && Perf == 2 && T >= End1 + PerfGap)
		{
			PerfWorld.Add(PerfReal);
			if (T >= End1 + PerfGap + PerfWindow)
			{
				LogPerf(MenuShots[ShotIndex].Name, TEXT("no menu (the world alone)"), PerfWorld, Menu);
				PerfStage = 3;
				if (Menu)
				{
					Menu->SetHiddenForTest(false);
				}
			}
		}
	}
	if (!bShotTaken && ShotTimer >= Settle)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ClaudeLogs/Menu") / FString::Printf(TEXT("%s.png"), MenuShots[ShotIndex].Name));
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] screenshot %s"), *Path);
		bShotTaken = true;
		// The team screenshots: the penguin that picked no team shows on its own - no ring, no team tag, its card NO TEAM.
		if (ShotVariant == 1 && ShotScreen == EPinguScreen::Results)
		{
			int32 Rings = 0;
			const FPinguScore* LoneRow = nullptr;
			for (const FPinguScore& Score : GetScores())
			{
				Rings += GetScoreTeamRing(Score) >= 0 ? 1 : 0;
				LoneRow = Score.Slot == ShotLoneSlot ? &Score : LoneRow;
			}
			const bool bOk = Rings == GetScores().Num() - 1 && LoneRow && GetScoreTeamRing(*LoneRow) < 0 && GetLoneTeamMember(LoneRow->Team) == LoneRow;
			UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT results_teams: %d of %d rows with a team ring (want all but one), %s on its own: ring %d, its own row %d: %s"),
				Rings, GetScores().Num(), LoneRow ? *LoneRow->Name : TEXT("nobody"), LoneRow ? GetScoreTeamRing(*LoneRow) : -9,
				LoneRow && GetLoneTeamMember(LoneRow->Team) == LoneRow ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL"));
		}
		else if (ShotVariant == 1 && ShotScreen == EPinguScreen::Lobby)
		{
			int32 InTeams = 0;
			int32 LoneChoice = -9;
			const TArray<FPinguLobbyPlayer> Cards = GetLobbyCards();
			for (const FPinguLobbyPlayer& Card : Cards)
			{
				InTeams += Card.TeamChoice >= 0 ? 1 : 0;
				LoneChoice = Card.Slot == ShotLoneSlot ? Card.TeamChoice : LoneChoice;
			}
			UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT lobby_ten: %d of %d cards in a team, the one on its own (slot %d) team %d (want -1: NO TEAM): %s"),
				InTeams, Cards.Num(), ShotLoneSlot, LoneChoice, InTeams == Cards.Num() - 1 && LoneChoice == -1 ? TEXT("ok") : TEXT("FAIL"));
		}
		else if (ShotScreen == EPinguScreen::Pause && ShotVariant == 4)
		{
			// The host's pause menu lists the joiner as a KICK button (2026-11-07): the made-up friend is the one target.
			const TArray<FPinguLobbyPlayer> Targets = GetKickTargets();
			UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT pause_host: %d kick target(s) (want 1), the first \"%s\" (want Frosty): %s"), Targets.Num(),
				Targets.Num() > 0 ? *Targets[0].Name : TEXT("-"), Targets.Num() == 1 && Targets[0].Name == TEXT("Frosty") ? TEXT("ok") : TEXT("FAIL"));
		}
		else if (ShotScreen == EPinguScreen::Settings && MenuShots[ShotIndex].SettingsPage == 0)
		{
			// The settings page's APPLY (2026-11-07): greyed while nothing is changed (settings), enabled after a change
			// (settings_apply), and the "Apply changes?" board with APPLY and DISCARD over the page (settings_confirm).
			const APinguMenu3D* Menu = APinguMenu3D::Find(GetWorld());
			auto Find = [Menu](FName Id)
			{
				FPinguMenu3DButtonInfo Found;
				for (int32 Index = 0; Menu && Index < Menu->GetButtonCount(); ++Index)
				{
					const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
					if (Info.Id == Id)
					{
						Found = Info;
					}
				}
				return Found;
			};
			const bool bPending = HasSettingsPending();
			if (ShotVariant == 6)
			{
				const FPinguMenu3DButtonInfo ConfirmApply = Find(TEXT("ConfirmApply"));
				const FPinguMenu3DButtonInfo ConfirmDiscard = Find(TEXT("ConfirmDiscard"));
				const bool bOk = bPending && HasSettingsConfirm() && Menu && Menu->GetOverlay() == EPinguMenu3DPage::Confirm
					&& ConfirmApply.bShown && ConfirmApply.bFocusable && ConfirmDiscard.bShown && ConfirmDiscard.bFocusable && ConfirmApply.bHot;
				UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT settings_confirm: changes pending %d, the board shows %d (overlay %s), APPLY shown %d focusable %d hot %d, DISCARD shown %d focusable %d: %s"),
					bPending ? 1 : 0, HasSettingsConfirm() ? 1 : 0, Menu ? APinguMenu3D::PageName(Menu->GetOverlay()) : TEXT("-"), ConfirmApply.bShown ? 1 : 0,
					ConfirmApply.bFocusable ? 1 : 0, ConfirmApply.bHot ? 1 : 0, ConfirmDiscard.bShown ? 1 : 0, ConfirmDiscard.bFocusable ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL"));
			}
			else
			{
				const FPinguMenu3DButtonInfo Apply = Find(TEXT("Apply"));
				const bool bWant = ShotVariant == 5;
				const bool bOk = Apply.bShown && Apply.bEnabled == bWant && bPending == bWant && !HasSettingsConfirm();
				UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT %s: APPLY shown %d, enabled %d (want %d), changes pending %d: %s"),
					ShotVariant == 5 ? TEXT("settings_apply") : TEXT("settings_clean"), Apply.bShown ? 1 : 0, Apply.bEnabled ? 1 : 0, bWant ? 1 : 0, bPending ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL"));
			}
		}
		else if (ShotScreen == EPinguScreen::Settings && MenuShots[ShotIndex].SettingsPage == 1)
		{
			// The host's hot-join switch (2026-11-07): flips and flips back (nothing is saved in an automated run).
			UPinguSettings* Prefs = UPinguSettings::Get();
			const bool bWas = Prefs && Prefs->GetJoinRunningMatches();
			bool bFlipped = false;
			bool bBack = false;
			if (Prefs)
			{
				Prefs->SetJoinRunningMatches(!bWas);
				bFlipped = Prefs->GetJoinRunningMatches() == !bWas;
				Prefs->SetJoinRunningMatches(true);
				bBack = Prefs->GetJoinRunningMatches() == !FParse::Param(FCommandLine::Get(), TEXT("PinguNoHotJoin"));
			}
			UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT settings_display: join running matches was %s, flips %d, and back to ON (the default) %d: %s"),
				bWas ? TEXT("ON") : TEXT("OFF"), bFlipped ? 1 : 0, bBack ? 1 : 0, (bFlipped || FParse::Param(FCommandLine::Get(), TEXT("PinguNoHotJoin"))) && bBack ? TEXT("ok") : TEXT("FAIL"));
		}
		else if (ShotScreen == EPinguScreen::Pingupedia && MenuShots[ShotIndex].Page == 1 && MenuShots[ShotIndex].Entry == ShotJesterEntry && Root.IsValid())
		{
			// Every ability and every trait listed, every card fits its panel, every stat line has a name of its own, the
			// title menu's PINGUPEDIA label fits its button, and the traits' list scrolled down to the last one (Jester).
			const PinguUI::FPingupediaStats& Pedia = PinguUI::GetPingupediaStats();
			int32 PediaPage = -1;
			int32 PediaEntry = -1;
			const bool bInView = Root->GetPingupediaView(PediaPage, PediaEntry);
			const int32 WantAbilities = static_cast<int32>(EPenguinAbility::Count) - 1;
			const bool bOk = Pedia.Abilities == WantAbilities && Pedia.Traits == PinguNumTraits && Pedia.LabelWidth > 1.f
				&& Pedia.LabelWidth <= Pedia.LabelRoom && Pedia.LabelFont >= 22.f && Pedia.TallestCard > 1.f && Pedia.TallestCard + PediaCardSpare <= Pedia.CardRoom
				&& Pedia.RawLabels == 0 && PediaPage == 1 && PediaEntry == ShotJesterEntry && bInView;
			UE_LOG(LogPingu, Display, TEXT("[MENUSHOTS] RESULT pingupedia: %d abilities, %d traits, tallest card %.0f of %.0f (%s; want %.0f spare), %d stat lines without a name (%s), title label %.0f of %.0f (font %.0f), %s selected on page %d, its row in view %d: %s"),
				Pedia.Abilities, Pedia.Traits, Pedia.TallestCard, Pedia.CardRoom, *Pedia.TallestName, PediaCardSpare, Pedia.RawLabels,
				Pedia.RawLabelNames.IsEmpty() ? TEXT("-") : *Pedia.RawLabelNames, Pedia.LabelWidth, Pedia.LabelRoom, Pedia.LabelFont,
				*PinguTraits::GetLogName(static_cast<EPinguTrait>(FMath::Clamp(PediaEntry + 1, 0, PinguNumTraits))), PediaPage, bInView ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL"));
			PediaKeyStep = 0;
			PediaKeysOk = 0;
			PediaKeysLog.Reset();
		}
	}
	else if (bShotTaken && ShotScreen == EPinguScreen::Pingupedia && MenuShots[ShotIndex].Page == 1 && MenuShots[ShotIndex].Entry == ShotJesterEntry && PediaKeyStep <= PediaKeyCount)
	{
		// The picture is written: now the keys, one every PediaKeyGap seconds (the focus moves a frame after a page switch).
		if (ShotTimer >= ShotSettle + ShotWrite + PediaKeyStep * PediaKeyGap)
		{
			TickPingupediaKeys();
		}
	}
	else if (bShotTaken && ShotTimer >= Settle + ShotWrite)
	{
		++ShotIndex;
		PerfStage = 0;
		ShotTimer = 0.f;
		bShotTaken = false;
		bShotCopyDone = false;
		if (ShotIndex < NumMenuShots)
		{
			const FMenuShot& Shot = MenuShots[ShotIndex];
			ShotScreen = Shot.Screen;
			bShotError = Shot.bError;
			bShotClient = Shot.bClient;
			ShotVariant = Shot.Variant;
			bShotTraits = Shot.bTraits;
			if (ScreenFocus.IsValidIndex(static_cast<int32>(ShotScreen)))
			{
				ScreenFocus[static_cast<int32>(ShotScreen)].Reset();
			}
			// Same screen again (another page, the client view, the popup): restart its transition too.
			ScreenChangedAt = PinguUI::Now();
			FocusedStateKey = 0xFFFFFFFFu;
		}
		else
		{
			ShotScreen = EPinguScreen::None;
			bShotError = false;
			bShotClient = false;
			ShotVariant = 0;
			bShotTraits = false;
		}
		PrepareMenuShotWorld();
		UpdateScreen();
		if (ShotIndex < NumMenuShots && MenuShots[ShotIndex].SettingsPage >= 0 && Root.IsValid())
		{
			bSettingsConfirm = false;
			Root->SelectSettingsTab(MenuShots[ShotIndex].SettingsPage, true);
			if (MenuShots[ShotIndex].Variant >= 5 && Root.IsValid())
			{
				// Something changed (SHOW FPS flipped, kept until the next shot's screen ends the settings session), then BACK.
				UPinguSettings* Prefs = UPinguSettings::Get();
				if (!Prefs->HasPendingChanges())
				{
					FPinguGraphicsOptions Options = Prefs->GetGraphics();
					Options.bShowFPS = !Options.bShowFPS;
					Prefs->SetGraphics(Options);
				}
				bSettingsConfirm = MenuShots[ShotIndex].Variant == 6;
			}
		}
		if (ShotIndex < NumMenuShots && MenuShots[ShotIndex].Screen == EPinguScreen::Pingupedia && Root.IsValid())
		{
			Root->SelectPingupediaPage(MenuShots[ShotIndex].Page, MenuShots[ShotIndex].Entry, true);
		}
	}
}

#undef LOCTEXT_NAMESPACE
