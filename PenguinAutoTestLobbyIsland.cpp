// Pingu - the lobby test's look at the lobby island (APinguLobbyTest::UpdateLobbyIslandChecks), driven like a player with
// clicks at screen positions (APinguLobby::ClickAtScreen: the mouse's own path): PLAY opens a small real island with your
// penguin on it and its name tag; the "+" adds a bot that puffs in (it pops up out of the dust); a click on the bot changes
// its team (the hop-spin, the beanie in the team's colour), a right click changes it back; a click on its difficulty tag
// changes the difficulty; its x sends it away in a puff. Screenshots lobbytest_island / _puff_in / _team / _tags / _puff_out.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCamera.h"
#include "PenguinGameMode.h"
#include "PenguinMovementComponent.h"
#include "PinguPickup.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PinguLobby.h"
#include "PinguMenu3D.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Components/SkeletalMeshComponent.h"

namespace PenguinAutoTestLobbyIslandPrivate
{
	/** The lobby island: 12 or 13 blocks wide (the two-penguin arena is 15 and up). */
	constexpr float MinWidthBlocks = 11.5f;
	constexpr float MaxWidthBlocks = 14.5f;
	/** Each step gives up after this long (s). */
	constexpr float StepTimeout = 4.f;

	APenguinCharacter* FindBot(const UWorld* World, const FString& Name)
	{
		for (TActorIterator<APenguinCharacter> It(const_cast<UWorld*>(World)); It; ++It)
		{
			if (IsValid(*It) && !It->GetPlayerState() && It->GetDisplayName() == Name)
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** A click like a player's: at the point's place on the screen, through the lobby's mouse path. */
	bool ClickOn(APinguLobby* Lobby, APlayerController* Player, const FVector& World, bool bRight, FString& OutWhere)
	{
		FVector2D Screen;
		if (!Lobby || !Player || !Player->ProjectWorldLocationToScreen(World, Screen, false))
		{
			OutWhere = TEXT("off screen");
			return false;
		}
		OutWhere = FString::Printf(TEXT("%.0f, %.0f px"), Screen.X, Screen.Y);
		return Lobby->ClickAtScreen(Player, Screen, bRight);
	}

	int32 SkillOf(const APenguinHUD* HUD, const FString& Name)
	{
		const FPinguMatchSettings Settings = HUD->GetMatchSettings();
		const FPinguBotSetup* Setup = Settings.Bots.FindByPredicate([&Name](const FPinguBotSetup& Bot) { return Bot.Name == Name; });
		return Setup ? Setup->Skill : -9;
	}

	int32 TeamOf(const APenguinHUD* HUD, const FString& Name)
	{
		const FPinguMatchSettings Settings = HUD->GetMatchSettings();
		const FPinguBotSetup* Setup = Settings.Bots.FindByPredicate([&Name](const FPinguBotSetup& Bot) { return Bot.Name == Name; });
		return Setup ? Setup->Team : -9;
	}
}

// ---------------------------------------------------------------------------- the spots and the waiting gags (2026-10-27)

namespace PenguinAutoTestLobbyIslandPrivate
{
	/** The lobby with this many bots (each its skill), our penguin in it, the dummy and the steps' other penguins out of sight. */
	void OpenTestLobby(UWorld* World, APenguinCharacter* Own, int32 NumBots, TArray<TWeakObjectPtr<APenguinCharacter>>& OutHidden)
	{
		APenguinGameMode* GameMode = World ? World->GetAuthGameMode<APenguinGameMode>() : nullptr;
		if (!GameMode)
		{
			return;
		}
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			if (*It != Own && !GameMode->GetPenguins().Contains(*It))
			{
				It->SetActorHiddenInGame(true);
				It->SetActorEnableCollision(false);
				It->SetActorLocation(FVector(0.f, 3000.f, -5000.f));
				OutHidden.Add(*It);
			}
		}
		APinguPickup::DestroyAll(World);
		FPinguMatchSettings Settings = GameMode->GetSettings();
		Settings.Bots.SetNum(NumBots);
		for (int32 Index = 0; Index < Settings.Bots.Num(); ++Index)
		{
			Settings.Bots[Index].Team = -1;
			Settings.Bots[Index].Skill = Index % 3;
		}
		Settings.NumBots = NumBots;
		GameMode->SetMatchSettings(Settings);
		if (Own)
		{
			Own->SetTeam(-1);
		}
		GameMode->EnterLobby();
		if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(World))
		{
			Stage->SetActorTickEnabled(true);
			Stage->SnapToTargets(0.5f);
		}
	}

	void CloseTestLobby(UWorld* World, TArray<TWeakObjectPtr<APenguinCharacter>>& Hidden)
	{
		PinguLobby::SetGagsPaused(false);
		if (APenguinGameMode* GameMode = World ? World->GetAuthGameMode<APenguinGameMode>() : nullptr)
		{
			GameMode->StartPractice();
		}
		for (const TWeakObjectPtr<APenguinCharacter>& Each : Hidden)
		{
			if (Each.IsValid())
			{
				Each->SetActorHiddenInGame(false);
				Each->SetActorEnableCollision(true);
			}
		}
		Hidden.Reset();
	}

	/** lobbytest: our penguin's Body bone scale before its fish (the match must have it back). */
	float& StripCarryBase()
	{
		static float Base = 0.f;
		return Base;
	}

	/** A click target's rectangle on the screen (false: off screen). */
	bool TargetOnScreen(APlayerController* Player, const APinguLobby::FTarget& Target, FBox2D& OutBox)
	{
		FVector2D A;
		FVector2D B;
		if (!Player->ProjectWorldLocationToScreen(Target.Centre + FVector(-Target.Half.X, 0.f, -Target.Half.Y), A, false)
			|| !Player->ProjectWorldLocationToScreen(Target.Centre + FVector(Target.Half.X, 0.f, Target.Half.Y), B, false))
		{
			return false;
		}
		OutBox = FBox2D(FVector2D(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y)), FVector2D(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y)));
		return true;
	}

	/** The Body bone's scale (component space: the fish's fat) and how far the root bone is from the mesh's origin (a hang). */
	float BodyBoneScale(const APenguinCharacter* Penguin)
	{
		const USkeletalMeshComponent* SkelMesh = Penguin ? Penguin->GetMesh() : nullptr;
		return SkelMesh && SkelMesh->GetBoneIndex(TEXT("Body")) != INDEX_NONE ? static_cast<float>(SkelMesh->GetSocketTransform(TEXT("Body"), RTS_Component).GetScale3D().X) : 0.f;
	}

	float RootBoneOffset(const APenguinCharacter* Penguin)
	{
		const USkeletalMeshComponent* SkelMesh = Penguin ? Penguin->GetMesh() : nullptr;
		if (!SkelMesh || !SkelMesh->GetSkeletalMeshAsset() || SkelMesh->GetNumBones() == 0)
		{
			return -1.f;
		}
		const FName Bone = SkelMesh->GetBoneIndex(TEXT("Root")) != INDEX_NONE ? FName(TEXT("Root")) : SkelMesh->GetBoneName(0);
		const FVector Root = SkelMesh->GetBoneLocation(Bone, EBoneSpaces::WorldSpace);
		return static_cast<float>(FVector::Dist(Root, SkelMesh->GetComponentLocation()));
	}

	/** How far the penguin's feet are drawn above (+) or below (-) where they stand without any lobby pose (cm): the lowest
	 *  Foot bone now against the same bone in the mesh's reference pose (the game's plain standing on its capsule's floor). */
	float FeetDrop(const APenguinCharacter* Penguin)
	{
		const USkeletalMeshComponent* SkelMesh = Penguin ? Penguin->GetMesh() : nullptr;
		const USkeletalMesh* Asset = SkelMesh ? SkelMesh->GetSkeletalMeshAsset() : nullptr;
		if (!Asset)
		{
			return 0.f;
		}
		const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
		TArray<FTransform> RefCS;
		RefCS.SetNum(Ref.GetNum());
		float NowLow = TNumericLimits<float>::Max();
		float RefLow = TNumericLimits<float>::Max();
		for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
		{
			const int32 Parent = Ref.GetParentIndex(Index);
			RefCS[Index] = Parent == INDEX_NONE ? Ref.GetRefBonePose()[Index] : Ref.GetRefBonePose()[Index] * RefCS[Parent];
			const FName Name = Ref.GetBoneName(Index);
			if (!Name.ToString().StartsWith(TEXT("Foot")))
			{
				continue;
			}
			NowLow = FMath::Min(NowLow, static_cast<float>(SkelMesh->GetBoneLocation(Name, EBoneSpaces::WorldSpace).Z));
			RefLow = FMath::Min(RefLow, static_cast<float>(SkelMesh->GetComponentTransform().TransformPosition(RefCS[Index].GetLocation()).Z));
		}
		return NowLow < TNumericLimits<float>::Max() ? NowLow - RefLow : 0.f;
	}

	struct FGagLook
	{
		/** Feet against the plain standing (FeetDrop): the most below it (cm) and where, all the time; the most off it at rest. */
		float FeetLowest = 0.f;
		FString FeetLowestWho;
		float FeetRestOff = 0.f;
		int32 FeetSamples = 0;
		TArray<TWeakObjectPtr<APenguinCharacter>> Hidden;
		TSet<FString> Taken;
		bool bJudged = false;
		// the gags: who, which, when; what was seen
		struct FPlay
		{
			TWeakObjectPtr<APenguinCharacter> Who;
			PinguLobby::EGag Gag = PinguLobby::EGag::None;
			float At = 0.f;
			bool bStarted = false;
			bool bSeen = false;
			bool bEnded = false;
		};
		TArray<FPlay> Plays;
		TWeakObjectPtr<APenguinCharacter> Flinched;
		FString FlinchedName;
		FVector FlinchedAt = FVector::ZeroVector;
		float FlinchedMoved = 0.f;
		float FriendHonk = 0.f;
		float CameraHonk = 0.f;
		float MostFat = 0.f;
		float BaseScale = 0.f;
		float FatScale = 0.f;
		float AfterScale = 0.f;
		float FatAfter = -1.f;
		int32 ZzzProps = 0;
		int32 FishProps = 0;
		bool bCarryArmed = false;
		bool bPracticed = false;
		FString Carry;
		bool bCarryOk = false;
		float YouBaseScale = 0.f;
	};
	FGagLook& GagLook()
	{
		static FGagLook Look;
		return Look;
	}
}

namespace PenguinAutoTestLobbyIslandPrivate
{
	/** lobbytest's card lobby run (2026-10-29): what it saw between its steps. */
	struct FCardLobbyRun
	{
		int32 CardBotsBefore = 0;
		FString CardBotName;
		int32 CardTeamBefore = -9;
		int32 CardSkillBefore = -9;
		FName CardHotBefore;
		bool CardPanelHidden = false;
		float CardWalkStartX = 0.f;
		float CardWalked = 0.f;
	};
	static FCardLobbyRun& CardRun()
	{
		static FCardLobbyRun Run;
		return Run;
	}
}

bool APinguLobbyTest::UpdateLobbyIslandChecks(APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check)
{
	using namespace PenguinAutoTestLobbyIslandPrivate;
	if (LobbyStep < 0)
	{
		return false; // done
	}
	if (LobbyStepAt < 0.f)
	{
		LobbyStepAt = TotalTime;
	}
	UWorld* World = GetWorld();
	APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
	APinguLobby* Lobby = APinguLobby::Get(World);
	const ABlockIsland* Island = ABlockIsland::Get(World);
	APenguinCharacter* You = Player ? Cast<APenguinCharacter>(Player->GetPawn()) : nullptr;
	const float Since = TotalTime - LobbyStepAt;
	const auto NextStep = [this](int32 Step)
	{
		LobbyStep = Step;
		LobbyStepAt = TotalTime;
		LobbyShot = false;
	};
	if (LobbyStep > 0 && Since > StepTimeout + 1.f)
	{
		Check(false, FString::Printf(TEXT("lobby island step %d timed out (%s)"), LobbyStep, *LobbyDetail));
		NextStep(-1);
		return false;
	}
	if (!PinguMenu3D::UseIslandLobby())
	{
		// ---- The wooden card lobby (2026-10-29, card 2026-10-29_wood-card-lobby): the panel over the title's island (no lobby
		// island, no lobby actor), real clicks on ADD BOT, a bot's card (its team), its difficulty and its x; then Linus's "keep the
		// the function of the playing in the lobby when pressig esc": a real Esc hides the panel, the D key moves the penguin,
		// Esc brings the panel back with the focus where it was.
		APinguMenu3D* Menu = PinguMenu3D::IsEnabled() ? APinguMenu3D::Get(LobbyHUD) : nullptr;
		const auto FindId = [Menu](const FString& Id)
		{
			for (int32 Index = 0; Menu && Index < Menu->GetButtonCount(); ++Index)
			{
				const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
				if (Info.Id == FName(*Id) && Info.bShown)
				{
					return Index;
				}
			}
			return int32(INDEX_NONE);
		};
		const auto ClickId = [Menu, &FindId](const FString& Id)
		{
			const int32 Index = FindId(Id);
			return Index != INDEX_NONE && Menu->PointerDown(Menu->GetButtonInfo(Index).ScreenCenter);
		};
		const auto LastBot = [LobbyHUD]()
		{
			FPinguLobbyPlayer Found;
			for (const FPinguLobbyPlayer& Card : LobbyHUD->GetLobbyCards())
			{
				if (Card.bBot)
				{
					Found = Card;
				}
			}
			return Found;
		};
		const auto PressKey = [](const FKey& Key)
		{
			APenguinInputTest::SendKeyEvent(Key, true);
			APenguinInputTest::SendKeyEvent(Key, false);
		};
		if (!Menu)
		{
			Check(false, TEXT("card lobby: no 3D menu"));
			NextStep(-1);
			return false;
		}
		switch (LobbyStep)
		{
		case 0:
		{
			if (Menu->GetPage() != EPinguMenu3DPage::Lobby || Since < 1.2f)
			{
				return true;
			}
			int32 CardButtons = 0;
			for (int32 Index = 0; Index < Menu->GetButtonCount(); ++Index)
			{
				const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
				CardButtons += Info.bShown && Info.Id.ToString().StartsWith(TEXT("Card_")) ? 1 : 0;
			}
			const int32 Cards = LobbyHUD->GetLobbyCards().Num();
			const bool bNoIsland = (!Lobby || !Lobby->IsShowing()) && (!Island || Island->Layout != EIslandLayout::Lobby);
			const bool bAdd = FindId(TEXT("AddBot")) != INDEX_NONE;
			const bool bOk = CardButtons == Cards && Cards >= 2 && bAdd && bNoIsland && LobbyHUD->IsMenuOpen() && LobbyHUD->GetScreen() == EPinguScreen::Lobby;
			Check(bOk, FString::Printf(TEXT("PLAY opens the wooden card lobby: %d cards for %d players and bots, ADD BOT %d, no lobby island %d, the panel has the keys %d"),
				CardButtons, Cards, bAdd ? 1 : 0, bNoIsland ? 1 : 0, LobbyHUD->IsMenuOpen() ? 1 : 0));
			Shot(TEXT("lobbytest_card_lobby"));
			CardRun().CardBotsBefore = LobbyHUD->GetBotCount();
			ClickId(TEXT("AddBot"));
			NextStep(1);
			return true;
		}
		case 1:
		{
			if (Since < 0.5f)
			{
				return true;
			}
			const int32 After = LobbyHUD->GetBotCount();
			Check(After == CardRun().CardBotsBefore + 1, FString::Printf(TEXT("card lobby: a click on ADD BOT adds a bot (%d -> %d)"), CardRun().CardBotsBefore, After));
			const FPinguLobbyPlayer Bot = LastBot();
			CardRun().CardBotName = Bot.Name;
			CardRun().CardTeamBefore = Bot.TeamChoice;
			CardRun().CardSkillBefore = Bot.Skill;
			ClickId(TEXT("Card_B_") + Bot.Name);
			NextStep(2);
			return true;
		}
		case 2:
		{
			if (Since < 0.5f)
			{
				return true;
			}
			const FPinguLobbyPlayer Bot = LastBot();
			Check(Bot.Name == CardRun().CardBotName && Bot.TeamChoice != CardRun().CardTeamBefore, FString::Printf(TEXT("card lobby: a click on %s's card changes its team (%d -> %d)"),
				*CardRun().CardBotName, CardRun().CardTeamBefore, Bot.TeamChoice));
			ClickId(TEXT("Skill_B_") + CardRun().CardBotName);
			NextStep(3);
			return true;
		}
		case 3:
		{
			if (Since < 0.5f)
			{
				return true;
			}
			const FPinguLobbyPlayer Bot = LastBot();
			Check(Bot.Name == CardRun().CardBotName && Bot.Skill != CardRun().CardSkillBefore, FString::Printf(TEXT("card lobby: a click on its difficulty changes it (%d -> %d)"),
				CardRun().CardSkillBefore, Bot.Skill));
			CardRun().CardBotsBefore = LobbyHUD->GetBotCount();
			ClickId(TEXT("Remove_B_") + CardRun().CardBotName);
			NextStep(4);
			return true;
		}
		case 4:
		{
			if (Since < 0.6f)
			{
				return true;
			}
			const int32 After = LobbyHUD->GetBotCount();
			Check(After == CardRun().CardBotsBefore - 1 && LastBot().Name != CardRun().CardBotName, FString::Printf(TEXT("card lobby: a click on its x sends it away (%d -> %d)"), CardRun().CardBotsBefore, After));
			// The Esc run: SETTINGS hot first (the focus must come back there).
			Menu->FocusButton(TEXT("Settings"));
			NextStep(5);
			return true;
		}
		case 5:
			if (Since < 0.3f)
			{
				return true;
			}
			CardRun().CardHotBefore = Menu->GetHotId();
			PressKey(EKeys::Escape);
			NextStep(6);
			return true;
		case 6:
		{
			if (Since < 0.5f)
			{
				return true;
			}
			CardRun().CardPanelHidden = LobbyHUD->GetScreen() == EPinguScreen::None && Menu->GetPage() == EPinguMenu3DPage::None && !LobbyHUD->IsMenuOpen();
			CardRun().CardWalkStartX = You ? static_cast<float>(You->GetActorLocation().X) : 0.f;
			Shot(TEXT("lobbytest_card_lobby_hidden"));
			APenguinInputTest::SendKeyEvent(EKeys::D, true);
			NextStep(7);
			return true;
		}
		case 7:
			if (Since < 0.8f)
			{
				return true;
			}
			APenguinInputTest::SendKeyEvent(EKeys::D, false);
			CardRun().CardWalked = You ? static_cast<float>(You->GetActorLocation().X) - CardRun().CardWalkStartX : 0.f;
			PressKey(EKeys::Escape);
			NextStep(8);
			return true;
		case 8:
		{
			if (Since < 0.6f)
			{
				return true;
			}
			const bool bBack = LobbyHUD->GetScreen() == EPinguScreen::Lobby && Menu->GetPage() == EPinguMenu3DPage::Lobby && LobbyHUD->IsMenuOpen();
			const FName HotAfter = Menu->GetHotId();
			Check(CardRun().CardPanelHidden && CardRun().CardWalked > 50.f && bBack && HotAfter == CardRun().CardHotBefore,
				FString::Printf(TEXT("card lobby: Esc hides the panel (%d), the penguin plays on the island (D held 0.8 s: %.0f cm, want > 50), Esc brings it back (%d) with the focus on %s (want %s)"),
					CardRun().CardPanelHidden ? 1 : 0, CardRun().CardWalked, bBack ? 1 : 0, *HotAfter.ToString(), *CardRun().CardHotBefore.ToString()));
			NextStep(-1);
			return false;
		}
		default:
			return false;
		}
	}
	switch (LobbyStep)
	{
	case 0: // the island, your penguin, its tag; then the "+"
	{
		if (!Lobby || !Island || !You || Since < 1.5f)
		{
			if (Since > StepTimeout + 1.f)
			{
				Check(false, TEXT("PLAY opens the lobby island (no lobby actor / island / penguin)"));
				NextStep(-1);
			}
			return true;
		}
		const FBox Bounds = Island->GetBounds();
		const float Width = Bounds.IsValid ? static_cast<float>(Bounds.Max.X - Bounds.Min.X) / Island->CellSize : 0.f;
		const FVector At = You->GetActorLocation();
		const bool bOnIsland = You->IsAlive() && At.X > Bounds.Min.X && At.X < Bounds.Max.X && At.Z > Bounds.Min.Z;
		const FString Tag = Lobby->GetNameTagText(You);
		FVector Plus;
		const bool bPlus = Lobby->GetPlusSpot(Plus);
		const bool bOk = Lobby->IsShowing() && Island->Layout == EIslandLayout::Lobby && Width >= MinWidthBlocks && Width <= MaxWidthBlocks && bOnIsland
			&& Tag == You->GetDisplayName() && bPlus && !LobbyHUD->IsMenuOpen();
		Check(bOk, FString::Printf(TEXT("PLAY opens the lobby island: layout lobby %d, %.1f blocks wide (want 12 to 13), your penguin on it %d at %.0f, %.0f, its tag \"%s\" (\"%s\"), the + shown %d, no menu input %d"),
			Island->Layout == EIslandLayout::Lobby ? 1 : 0, Width, bOnIsland ? 1 : 0, At.X, At.Z, *Tag, *You->GetDisplayName(), bPlus ? 1 : 0, LobbyHUD->IsMenuOpen() ? 0 : 1));
		Shot(TEXT("lobbytest_island"));
		CheckLobbyButtonStrip(LobbyHUD, Check);
		LobbyNames.Reset();
		for (const FPinguBotSetup& Bot : LobbyHUD->GetMatchSettings().Bots)
		{
			LobbyNames.Add(Bot.Name);
		}
		LobbyBotsBefore = LobbyHUD->GetBotCount();
		LobbyPuffInsBefore = Lobby->GetPuffInsSeen();
		LobbyMinPop = 2.f;
		FString Where;
		const bool bClicked = ClickOn(Lobby, Player, Plus, false, Where);
		LobbyDetail = FString::Printf(TEXT("clicked the + at %s (%d)"), *Where, bClicked ? 1 : 0);
		NextStep(1);
		return true;
	}
	case 1: // the new bot puffs in: tiny, then full size
	{
		FString NewName;
		for (const FPinguBotSetup& Bot : LobbyHUD->GetMatchSettings().Bots)
		{
			if (!LobbyNames.Contains(Bot.Name))
			{
				NewName = Bot.Name;
			}
		}
		APenguinCharacter* Bot = NewName.IsEmpty() ? nullptr : FindBot(World, NewName);
		if (Bot)
		{
			LobbyBot = Bot;
			if (LobbyBotSeenAt < 0.f)
			{
				LobbyBotSeenAt = TotalTime;
			}
			LobbyMinPop = FMath::Min(LobbyMinPop, Lobby ? Lobby->GetPopScale(Bot) : 1.f);
			if (!LobbyShot && TotalTime - LobbyBotSeenAt > 0.12f)
			{
				Shot(TEXT("lobbytest_puff_in"));
				LobbyShot = true;
			}
		}
		if (!Bot || TotalTime - LobbyBotSeenAt < 0.9f)
		{
			return true;
		}
		const float Pop = Lobby ? Lobby->GetPopScale(Bot) : 0.f;
		const FVector At = Bot->GetActorLocation();
		const FBox Bounds = Island ? Island->GetBounds() : FBox(ForceInit);
		const bool bOn = Bounds.IsValid && At.X > Bounds.Min.X && At.X < Bounds.Max.X && At.Z > Bounds.Min.Z;
		const int32 PuffIns = Lobby ? Lobby->GetPuffInsSeen() - LobbyPuffInsBefore : 0;
		Check(LobbyHUD->GetBotCount() == LobbyBotsBefore + 1 && PuffIns == 1 && LobbyMinPop < 0.5f && Pop > 0.99f && bOn,
			FString::Printf(TEXT("the + adds a bot that puffs in: %s (%d -> %d bots, %d puff in), smallest %.2f then %.2f, on the island %d at %.0f, %.0f; %s"),
				*NewName, LobbyBotsBefore, LobbyHUD->GetBotCount(), PuffIns, LobbyMinPop, Pop, bOn ? 1 : 0, At.X, At.Z, *LobbyDetail));
		// Its team: a click on it.
		LobbyTeamBefore = TeamOf(LobbyHUD, NewName);
		LobbyPokesBefore = Lobby ? Lobby->GetPokesSeen() : 0;
		FString Where;
		ClickOn(Lobby, Player, Lobby->GetBodyCentre(Bot), false, Where);
		LobbyDetail = FString::Printf(TEXT("clicked %s at %s"), *NewName, *Where);
		NextStep(2);
		return true;
	}
	case 2: // a click: the next team, the beanie in its colour; a right click: back
	{
		APenguinCharacter* Bot = LobbyBot.Get();
		if (!Bot)
		{
			Check(false, TEXT("a click on the bot: it went away"));
			NextStep(-1);
			return false;
		}
		const FString Name = Bot->GetDisplayName();
		if (!LobbyShot && Since > 0.2f)
		{
			Shot(TEXT("lobbytest_team"));
			LobbyShot = true;
		}
		if (Since < 0.7f)
		{
			return true;
		}
		if (!LobbyRightClicked)
		{
			const int32 Team = TeamOf(LobbyHUD, Name);
			const int32 Pokes = Lobby ? Lobby->GetPokesSeen() - LobbyPokesBefore : 0;
			LobbyAfterLeft = FString::Printf(TEXT("team %d -> %d, beanie colour %d (its own %d), %d hop-spin"), LobbyTeamBefore, Team, Bot->GetHatColorIndex(), Bot->GetColorIndex(), Pokes);
			LobbyLeftOk = Team == LobbyTeamBefore + 1 && Team >= 0 && Bot->GetHatColorIndex() == Team && Pokes == 1;
			FString Where;
			ClickOn(Lobby, Player, Lobby->GetBodyCentre(Bot), true, Where);
			LobbyRightClicked = true;
			LobbyPokesBefore = Lobby ? Lobby->GetPokesSeen() : 0;
			LobbyStepAt = TotalTime; // wait again
			return true;
		}
		const int32 Team = TeamOf(LobbyHUD, Name);
		const int32 Pokes = Lobby ? Lobby->GetPokesSeen() - LobbyPokesBefore : 0;
		const bool bBack = Team == LobbyTeamBefore && Bot->GetHatColorIndex() == (Team >= 0 ? Team : Bot->GetColorIndex()) && Pokes == 1;
		Check(LobbyLeftOk && bBack, FString::Printf(TEXT("a click on the bot changes its team: %s; a right click back: team %d, beanie %d, %d hop-spin; %s"),
			*LobbyAfterLeft, Team, Bot->GetHatColorIndex(), Pokes, *LobbyDetail));
		// Its difficulty tag.
		FVector Spot;
		LobbySkillBefore = SkillOf(LobbyHUD, Name);
		FString Where = TEXT("no tag");
		if (Lobby && Lobby->GetSkillTagSpot(Bot, Spot))
		{
			ClickOn(Lobby, Player, Spot, false, Where);
		}
		LobbyDetail = FString::Printf(TEXT("clicked the difficulty tag at %s"), *Where);
		NextStep(3);
		return true;
	}
	case 3: // the difficulty tag: the next difficulty, the tag says it
	{
		APenguinCharacter* Bot = LobbyBot.Get();
		if (!Bot || Since < 0.4f)
		{
			return true;
		}
		const FString Name = Bot->GetDisplayName();
		const int32 Skill = SkillOf(LobbyHUD, Name);
		const FString Tag = Lobby ? Lobby->GetSkillTagText(Bot) : FString();
		static const TCHAR* Names[] = { TEXT("EASY"), TEXT("NORMAL"), TEXT("HARD") };
		const bool bOk = Skill == (LobbySkillBefore + 1) % 3 && Tag == Names[FMath::Clamp(Skill, 0, 2)];
		Check(bOk, FString::Printf(TEXT("a click on the difficulty tag: %d -> %d, the tag says %s; %s"), LobbySkillBefore, Skill, *Tag, *LobbyDetail));
		Shot(TEXT("lobbytest_tags"));
		// Its x.
		FVector Spot;
		FString Where = TEXT("no x");
		LobbyBotsBefore = LobbyHUD->GetBotCount();
		LobbyPuffOutsBefore = Lobby ? Lobby->GetPuffOutsSeen() : 0;
		if (Lobby && Lobby->GetRemoveSpot(Bot, Spot))
		{
			ClickOn(Lobby, Player, Spot, false, Where);
		}
		LobbyDetail = FString::Printf(TEXT("clicked the x at %s"), *Where);
		LobbyShrinking = false;
		NextStep(4);
		return true;
	}
	case 4: // the x: it puffs away
	{
		APenguinCharacter* Bot = LobbyBot.Get();
		if (Bot && Lobby && Lobby->GetPopScale(Bot) < 0.99f)
		{
			LobbyShrinking = true;
		}
		if (!LobbyShot && Since > 0.1f)
		{
			Shot(TEXT("lobbytest_puff_out"));
			LobbyShot = true;
		}
		if (Since < 0.8f)
		{
			return true;
		}
		const int32 PuffOuts = Lobby ? Lobby->GetPuffOutsSeen() - LobbyPuffOutsBefore : 0;
		Check(!Bot && LobbyShrinking && PuffOuts == 1 && LobbyHUD->GetBotCount() == LobbyBotsBefore - 1,
			FString::Printf(TEXT("its x sends it away in a puff: shrank %d, gone %d, %d puff out, %d -> %d bots; %s"), LobbyShrinking ? 1 : 0, Bot ? 0 : 1, PuffOuts,
				LobbyBotsBefore, LobbyHUD->GetBotCount(), *LobbyDetail));
		NextStep(-1);
		return false;
	}
	default:
		return false;
	}
}

void APinguLobbyTest::CheckLobbyButtonStrip(APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check)
{
	using namespace PenguinAutoTestLobbyIslandPrivate;
	UWorld* World = GetWorld();
	APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
	APinguLobby* Lobby = APinguLobby::Get(World);
	APinguMenu3D* Menu = APinguMenu3D::Find(World);
	if (!Menu || !Player || !Lobby || PinguUseOldMenus())
	{
		Check(PinguUseOldMenus(), TEXT("the 3D button strip keeps its clicks: no 3D menu here"));
		return;
	}
	int32 ViewX = 0;
	int32 ViewY = 0;
	Player->GetViewportSize(ViewX, ViewY);
	FString Detail;
	bool bOk = true;
	int32 Tried = 0;
	for (int32 Index = 0; Index < Menu->GetButtonCount(); ++Index)
	{
		const FPinguMenu3DButtonInfo Info = Menu->GetButtonInfo(Index);
		if (!Info.bShown || (Info.Id != FName(TEXT("Start")) && Info.Id != FName(TEXT("Traits"))))
		{
			continue;
		}
		const FVector2D Pixel(Info.ScreenCenter.X * ViewX, Info.ScreenCenter.Y * ViewY);
		// A bot right behind the button (where the cursor's ray meets the play plane): without the menu's say it would get the click.
		FVector Origin;
		FVector Direction;
		APenguinCharacter* Bot = nullptr;
		for (TActorIterator<APenguinCharacter> It(World); It && !Bot; ++It)
		{
			Bot = IsValid(*It) && !It->GetPlayerState() && Lobby->GetPopScale(*It) > 0.99f ? *It : nullptr;
		}
		if (!Bot || !Player->DeprojectScreenPositionToWorld(Pixel.X, Pixel.Y, Origin, Direction) || FMath::Abs(Direction.Y) < 1.e-3)
		{
			bOk = false;
			Detail += FString::Printf(TEXT("%s: no bot / ray; "), *Info.Id.ToString());
			continue;
		}
		const FVector Behind = Origin + Direction * (-Origin.Y / Direction.Y);
		const FVector Was = Bot->GetActorLocation();
		// (Its body as drawn - sitting or hanging off an edge - right behind the button.)
		Bot->SetActorLocation(FVector(Behind.X, 0.f, Behind.Z) - Lobby->GetVisualOffset(Bot), false, nullptr, ETeleportType::TeleportPhysics);
		const FString Name = Bot->GetDisplayName();
		const int32 TeamBefore = TeamOf(LobbyHUD, Name);
		const int32 Bots = LobbyHUD->GetBotCount();
		const bool bHas = APinguMenu3D::HasButtonAt(World, Pixel);
		const bool bTaken = Lobby->ClickAtScreen(Player, Pixel, false);
		// (The same point off the strip does reach the bot: the test is real.)
		const bool bWithout = Lobby->ClickAt(Player, Lobby->GetBodyCentre(Bot), true);
		const int32 TeamAfter = TeamOf(LobbyHUD, Name);
		Bot->SetActorLocation(Was, false, nullptr, ETeleportType::TeleportPhysics);
		++Tried;
		const bool bThis = bHas && !bTaken && bWithout && LobbyHUD->GetBotCount() == Bots;
		bOk &= bThis;
		Detail += FString::Printf(TEXT("%s at %.0f, %.0f px: a button there %d, the island took it %d, %s behind it (team %d -> %d after the island's own right click: back) ; "),
			*Info.Id.ToString(), Pixel.X, Pixel.Y, bHas ? 1 : 0, bTaken ? 1 : 0, *Name, TeamBefore, TeamAfter);
		// Its team back as it was (the right click above stepped it back once).
		if (TeamAfter != TeamBefore)
		{
			const int32 BotIndex = LobbyHUD->GetMatchSettings().Bots.IndexOfByPredicate([&Name](const FPinguBotSetup& Setup) { return Setup.Name == Name; });
			LobbyHUD->Action_ChangeBotTeam(BotIndex, 1);
		}
	}
	Check(bOk && Tried == 2, FString::Printf(TEXT("a click on the 3D button strip (START, TRAITS) with a penguin behind it stays the menu's: %s"), *Detail));
}

bool APinguLobbyTest::UpdateLobbyStripKeys(APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check)
{
	UWorld* World = GetWorld();
	APinguMenu3D* Menu = APinguMenu3D::Find(World);
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (StripKeyStep < 0)
	{
		return false;
	}
	if (StripKeyAt < 0.f)
	{
		StripKeyAt = TotalTime;
	}
	const float Since = TotalTime - StripKeyAt;
	const auto Press = [](const FKey& Key)
	{
		APenguinInputTest::SendKeyEvent(Key, true);
		APenguinInputTest::SendKeyEvent(Key, false);
	};
	const auto Next = [this](int32 Step)
	{
		StripKeyStep = Step;
		StripKeyAt = TotalTime;
	};
	const auto Note = [this, LobbyHUD, Menu](const TCHAR* What, bool bOk)
	{
		bStripKeysOk &= bOk;
		StripKeyLog += FString::Printf(TEXT("%s%s: strip %d, menu input %d, hot %s%s"), StripKeyLog.IsEmpty() ? TEXT("") : TEXT("; "), What,
			LobbyHUD->IsLobbyStripFocused() ? 1 : 0, LobbyHUD->IsMenuOpen() ? 1 : 0, Menu ? *Menu->GetHotId().ToString() : TEXT("-"), bOk ? TEXT("") : TEXT(" (WRONG)"));
	};
	const FName Start(TEXT("Start"));
	constexpr float Wait = 0.3f;
	switch (StripKeyStep)
	{
	case 0: // on the island: no menu input; Esc
		if (Since < Wait)
		{
			return true;
		}
		Note(TEXT("on the island"), !LobbyHUD->IsLobbyStripFocused() && !LobbyHUD->IsMenuOpen());
		Press(EKeys::Escape);
		Next(1);
		return true;
	case 1: // the strip has the keys, START hot; Left
		if (Since < Wait)
		{
			return true;
		}
		Note(TEXT("Esc"), LobbyHUD->IsLobbyStripFocused() && LobbyHUD->IsMenuOpen() && Menu && Menu->GetHotId() == Start);
		Shot(TEXT("lobbytest_strip_keys"));
		Press(EKeys::Left);
		Next(2);
		return true;
	case 2: // another button; Right
		if (Since < Wait)
		{
			return true;
		}
		Note(TEXT("Left"), Menu && Menu->GetHotId() != Start && Menu->GetHotId() != NAME_None);
		Press(EKeys::Right);
		Next(3);
		return true;
	case 3: // START again; Esc
		if (Since < Wait)
		{
			return true;
		}
		Note(TEXT("Right"), Menu && Menu->GetHotId() == Start);
		Press(EKeys::Escape);
		Next(4);
		return true;
	case 4: // back on the island; gamepad Start
		if (Since < Wait)
		{
			return true;
		}
		// (Fat from a fish and a dozing bot with its Zzz as the match starts: none of it may come along - checked at step 6.)
		if (APinguLobby* Lobby = APinguLobby::Get(World))
		{
			APenguinCharacter* You = World->GetFirstPlayerController() ? Cast<APenguinCharacter>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
			PenguinAutoTestLobbyIslandPrivate::StripCarryBase() = PenguinAutoTestLobbyIslandPrivate::BodyBoneScale(You);
			Lobby->SetFatFor(You, 30.f);
			for (TActorIterator<APenguinCharacter> It(World); It; ++It)
			{
				if (IsValid(*It) && !It->GetPlayerState() && Lobby->StartGag(*It, PinguLobby::EGag::Doze))
				{
					break;
				}
			}
		}
		Note(TEXT("Esc again"), !LobbyHUD->IsLobbyStripFocused() && !LobbyHUD->IsMenuOpen() && GameState && GameState->GetPhase() == EPinguPhase::Lobby);
		Press(EKeys::Gamepad_Special_Right);
		Next(5);
		return true;
	case 5: // the strip again, START hot; Enter
		if (Since < Wait)
		{
			return true;
		}
		Note(TEXT("gamepad Start"), LobbyHUD->IsLobbyStripFocused() && Menu && Menu->GetHotId() == Start);
		Press(EKeys::Enter);
		Next(6);
		return true;
	case 6: // the match starts
	{
		const EPinguPhase Phase = GameState ? GameState->GetPhase() : EPinguPhase::Lobby;
		const bool bStarted = Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown || Phase == EPinguPhase::Fight;
		if ((!bStarted && Since < 3.f) || (bStarted && Since < 1.f))
		{
			return true;
		}
		Note(TEXT("Enter"), bStarted && !LobbyHUD->IsLobbyStripFocused());
		Check(bStripKeysOk, FString::Printf(TEXT("the 3D strip with keys only: %s; the match started %d"), *StripKeyLog, bStarted ? 1 : 0));
		{
			// Nothing of the lobby's waiting comes into the match: no fat, no sitting / hanging pose, no fish or Zzz.
			using namespace PenguinAutoTestLobbyIslandPrivate;
			const APinguLobby* Lobby = APinguLobby::Get(World);
			APenguinCharacter* You = World->GetFirstPlayerController() ? Cast<APenguinCharacter>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
			const PinguLobby::FAnimState State = PinguLobby::GetAnimState(You);
			const float Scale = BodyBoneScale(You);
			const float Root = RootBoneOffset(You);
			const float Base = StripCarryBase();
			const int32 Props = Lobby ? Lobby->CountShownProps() : 0;
			const bool bClean = You && !State.bLobby && State.Fat == 0.f && Props == 0 && Base > 0.f && FMath::Abs(Scale / Base - 1.f) < 0.03f && Root >= 0.f && Root < 20.f;
			Check(bClean, FString::Printf(TEXT("the lobby's fat, pose and props stay in the lobby: lobby anim %d, fat %.2f, props %d, body bone %.3f (the lobby's %.3f before the fish), root bone %.1f cm off the mesh"),
				State.bLobby ? 1 : 0, State.Fat, Props, Scale, Base, Root));
		}
		Next(-1);
		if (!bStarted)
		{
			LobbyHUD->Action_StartLobbyMatch(); // (the rest of the test goes on)
		}
		return false;
	}
	default:
		return false;
	}
}

// ---------------------------------------------------------------------------- the autotest's look step

namespace PenguinAutoTestLobbyIslandPrivate
{
	struct FLobbyLook
	{
		int32 Shots = 0;
		TSet<FString> Taken;
		bool bAdded = false;
		bool bClicked = false;
		bool bRemoved = false;
		bool bFocus = false;
		FString NewBot;
		TWeakObjectPtr<APenguinCharacter> Poked;
		int32 PokedTeam = -9;
		float MostOffSpot = 0.f;
		int32 SpotSamples = 0;
		float MinPop = 2.f;
		int32 MaxTags = 0;
		int32 MaxPenguins = 0;
		int32 PuffIns = 0;
		int32 PuffOuts = 0;
		int32 Pokes = 0;
		float Width = 0.f;
		TArray<TWeakObjectPtr<APenguinCharacter>> Hidden;
	};
	FLobbyLook& LobbyLook()
	{
		static FLobbyLook Look;
		return Look;
	}
}

void APenguinAutoTest::AddLobbyIslandLookStep()
{
	using namespace PenguinAutoTestLobbyIslandPrivate;
	if (!PinguMenu3D::UseIslandLobby())
	{
		// (The wooden card lobby since 2026-10-29: no lobby island - its look steps run with -PinguIslandLobby; the card lobby's
		// own are look_LobbyCards2D and lobbytest.)
		AddStep(TEXT("look_LobbyIsland"), 0.2f, [] {}, [](float) {},
			[this] { Report(TEXT("RESULT lobby island look: the lobby island is off (the wooden card lobby; -PinguIslandLobby shows it) ok")); });
		return;
	}
	AddStep(TEXT("look_LobbyIsland"), 16.5f,
		[this]
		{
			FLobbyLook& L = LobbyLook();
			L = FLobbyLook();
			APenguinGameMode* GameMode = GetWorld()->GetAuthGameMode<APenguinGameMode>();
			if (!GameMode)
			{
				return;
			}
			// The dummy (and any other penguin the steps made) waits out of sight.
			for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
			{
				if (*It != Penguin && !GameMode->GetPenguins().Contains(*It))
				{
					It->SetActorHiddenInGame(true);
					It->SetActorEnableCollision(false);
					It->SetActorLocation(FVector(0.f, 3000.f, -5000.f));
					L.Hidden.Add(*It);
				}
			}
			APinguPickup::DestroyAll(GetWorld());
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
			FLobbyLook& L = LobbyLook();
			UWorld* World = GetWorld();
			APenguinGameMode* GameMode = World->GetAuthGameMode<APenguinGameMode>();
			APinguLobby* Lobby = APinguLobby::Get(World);
			APlayerController* Player = World->GetFirstPlayerController();
			APenguinHUD* HUD = Player ? Cast<APenguinHUD>(Player->GetHUD()) : nullptr;
			if (!GameMode || !Lobby || !HUD)
			{
				return;
			}
			// Sampled all the time: the tags shown, the penguins there.
			int32 Tags = 0;
			int32 Count = 0;
			for (APenguinCharacter* Each : GameMode->GetPenguins())
			{
				if (!IsValid(Each))
				{
					continue;
				}
				++Count;
				Tags += Lobby->GetNameTagText(Each).IsEmpty() ? 0 : 1;
			}
			L.MaxTags = FMath::Max(L.MaxTags, Tags);
			L.MaxPenguins = FMath::Max(L.MaxPenguins, Count);
			const auto ShotAt = [&](float When, const TCHAR* Name, bool bUI)
			{
				if (T >= When && !L.Taken.Contains(Name))
				{
					L.Taken.Add(Name);
					Shot(Name, bUI);
					++L.Shots;
				}
			};
			ShotAt(3.6f, TEXT("74_lobby_overview"), true);
			// A bot puffs in (the "+": a click, as a player does).
			if (T > 4.f && !L.bAdded)
			{
				L.bAdded = true;
				TSet<FString> Before;
				for (const FPinguBotSetup& Bot : HUD->GetMatchSettings().Bots)
				{
					Before.Add(Bot.Name);
				}
				FVector Plus;
				if (Lobby->GetPlusSpot(Plus))
				{
					Lobby->ClickAt(Player, Plus, false);
				}
				for (const FPinguBotSetup& Bot : HUD->GetMatchSettings().Bots)
				{
					if (!Before.Contains(Bot.Name))
					{
						L.NewBot = Bot.Name;
					}
				}
			}
			if (L.bAdded && T < 5.f)
			{
				for (APenguinCharacter* Each : GameMode->GetPenguins())
				{
					if (IsValid(Each) && Each->GetDisplayName() == L.NewBot)
					{
						L.MinPop = FMath::Min(L.MinPop, Lobby->GetPopScale(Each));
					}
				}
			}
			ShotAt(4.13f, TEXT("74_lobby_puff_in"), false);
			// A click on a bot: the next team (red); our own penguin blue, another bot blue too.
			if (T > 5.6f && !L.bClicked)
			{
				L.bClicked = true;
				TArray<APenguinCharacter*> Bots;
				const TArray<TObjectPtr<APenguinCharacter>> Before = GameMode->GetPenguins();
				for (APenguinCharacter* Each : Before)
				{
					if (IsValid(Each) && !Each->GetPlayerState())
					{
						Bots.Add(Each);
					}
				}
				if (Bots.Num() >= 2)
				{
					L.Poked = Bots[0];
					Lobby->ClickAt(Player, Lobby->GetBodyCentre(Bots[0]), false);
					Lobby->ClickAt(Player, Lobby->GetBodyCentre(Bots[1]), false);
					Lobby->ClickAt(Player, Lobby->GetBodyCentre(Bots[1]), false);
				}
				Lobby->ClickAt(Player, Lobby->GetBodyCentre(Penguin.Get()), false);
				Lobby->ClickAt(Player, Lobby->GetBodyCentre(Penguin.Get()), false);
			}
			ShotAt(5.75f, TEXT("74_lobby_team_hop"), false);
			ShotAt(6.6f, TEXT("74_lobby_team"), false);
			if (T > 6.6f && L.PokedTeam == -9 && L.Poked.IsValid())
			{
				L.PokedTeam = L.Poked->GetHatColorIndex();
			}
			// The tags close up: the camera swoops to a bot (the knock-out zoom), the keyboard pick on it.
			if (T > 7.f && !L.bFocus && L.Poked.IsValid())
			{
				L.bFocus = true;
				if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(World))
				{
					Stage->FocusOn(Lobby->GetBodyCentre(L.Poked.Get()) + FVector(0.f, 0.f, 90.f), 3.f, 0.42f);
				}
			}
			ShotAt(8.5f, TEXT("74_lobby_tags"), false);
			// A bot sent away (its x).
			if (T > 10.f && !L.bRemoved)
			{
				L.bRemoved = true;
				const TArray<TObjectPtr<APenguinCharacter>> Before = GameMode->GetPenguins(); // (the x changes the list)
				for (APenguinCharacter* Each : Before)
				{
					FVector Spot;
					if (IsValid(Each) && Each->GetDisplayName() == L.NewBot && Lobby->GetRemoveSpot(Each, Spot))
					{
						Lobby->ClickAt(Player, Spot, false);
					}
				}
			}
			ShotAt(10.1f, TEXT("74_lobby_puff_out"), false);
			// Nobody walks (2026-10-27): every penguin stays on its spot (its capsule; a newcomer from its first moment there).
			if (T > 1.5f)
			{
				for (APenguinCharacter* Each : GameMode->GetPenguins())
				{
					const int32 Spot = IsValid(Each) ? Lobby->GetSpotOf(Each) : INDEX_NONE;
					if (Lobby->GetSpots().IsValidIndex(Spot) && Lobby->GetPopScale(Each) > 0.99f)
					{
						const float Off = static_cast<float>(FVector::Dist(Each->GetActorLocation(), Lobby->GetSpots()[Spot].Location));
						L.MostOffSpot = FMath::Max(L.MostOffSpot, Off);
						++L.SpotSamples;
					}
				}
			}
			ShotAt(16.f, TEXT("74_lobby_idles"), true);
			L.PuffIns = Lobby->GetPuffInsSeen();
			L.PuffOuts = Lobby->GetPuffOutsSeen();
			L.Pokes = Lobby->GetPokesSeen();
			if (const ABlockIsland* Island = ABlockIsland::Get(World))
			{
				const FBox Bounds = Island->GetBounds();
				L.Width = Bounds.IsValid ? static_cast<float>(Bounds.Max.X - Bounds.Min.X) / Island->CellSize : 0.f;
			}
		},
		[this]
		{
			FLobbyLook& L = LobbyLook();
			const bool bOk = L.Width >= MinWidthBlocks && L.Width <= MaxWidthBlocks && L.MaxPenguins >= 6 && L.MaxTags >= 5 && L.MinPop < 0.5f
				&& L.PuffIns >= 1 && L.PuffOuts >= 1 && L.Pokes >= 3 && L.PokedTeam == 0 && L.SpotSamples > 100 && L.MostOffSpot < 15.f;
			Report(FString::Printf(TEXT("RESULT lobby island look: %.1f blocks wide, %d penguins at most, %d name tags, a bot popped in from %.2f, %d puffs in / %d out, %d hop-spins, the poked bot's beanie %d (want 0, red), nobody walked: at most %.1f cm off its spot in %d samples (want under 15) %s"),
				L.Width, L.MaxPenguins, L.MaxTags, L.MinPop, L.PuffIns, L.PuffOuts, L.Pokes, L.PokedTeam, L.MostOffSpot, L.SpotSamples, bOk ? TEXT("ok") : TEXT("FAIL")));
			// Back to practice (the bots go), the dummy back in sight.
			if (APenguinGameMode* GameMode = GetWorld()->GetAuthGameMode<APenguinGameMode>())
			{
				GameMode->StartPractice();
			}
			for (const TWeakObjectPtr<APenguinCharacter>& Each : L.Hidden)
			{
				if (Each.IsValid())
				{
					Each->SetActorHiddenInGame(false);
					Each->SetActorEnableCollision(true);
				}
			}
		});

using namespace PenguinAutoTestLobbyIslandPrivate;
	// Ten penguins on the lobby island: each on a spot of its own (sitting, some hanging off an edge), no two bodies over each
	// other, no tag over another's tag or body or pushed aside, nothing near the "+", every body and name tag clickable.
	AddStep(TEXT("look_LobbySpots"), 7.5f,
		[this]
		{
			FGagLook& L = GagLook();
			L = FGagLook();
			PinguLobby::SetGagsPaused(true);
			ClearAbilityLeftovers();
			OpenTestLobby(GetWorld(), Penguin.Get(), PinguMaxPenguins - 1, L.Hidden);
			VirtualInput = FVector2D::ZeroVector;
		},
		[this](float T)
		{
			FGagLook& L = GagLook();
			UWorld* World = GetWorld();
			APenguinGameMode* GameMode = World->GetAuthGameMode<APenguinGameMode>();
			APinguLobby* Lobby = APinguLobby::Get(World);
			APlayerController* Player = World->GetFirstPlayerController();
			if (!GameMode || !Lobby || !Player)
			{
				return;
			}
			if (T >= 3.f && !L.Taken.Contains(TEXT("74_lobby_spots_ten")))
			{
				L.Taken.Add(TEXT("74_lobby_spots_ten"));
				Shot(TEXT("74_lobby_spots_ten"), true);
			}
			if (T >= 3.4f && !L.bJudged)
			{
				L.bJudged = true;
				int32 OnSpots = 0;
				float MostMeshOff = 0.f;
				FString MeshLine;
				int32 Sit = 0;
				int32 Hang = 0;
				int32 Count = 0;
				float MostShift = 0.f;
				int32 BodyClicks = 0;
				int32 NameClicks = 0;
				FString Bad;
				TSet<int32> SpotsUsed;
				for (APenguinCharacter* Each : GameMode->GetPenguins())
				{
					if (!IsValid(Each))
					{
						continue;
					}
					++Count;
					const int32 Spot = Lobby->GetSpotOf(Each);
					if (Lobby->GetSpots().IsValidIndex(Spot) && FVector::Dist(Each->GetActorLocation(), Lobby->GetSpots()[Spot].Location) < 15.f && !SpotsUsed.Contains(Spot))
					{
						++OnSpots;
						SpotsUsed.Add(Spot);
					}
					Sit += Lobby->GetPose(Each) == PinguLobby::EPose::Sit ? 1 : 0;
					// Where its mesh is drawn: the root bone near the drawn body's feet (a sitting one), out over the edge (a hanging one).
					if (const USkeletalMeshComponent* SkelMesh = Each->GetMesh())
					{
						const FName Bone = SkelMesh->GetBoneIndex(TEXT("Root")) != INDEX_NONE ? FName(TEXT("Root")) : SkelMesh->GetBoneName(0);
						const FVector RootAt = SkelMesh->GetNumBones() > 0 ? SkelMesh->GetBoneLocation(Bone, EBoneSpaces::WorldSpace) : FVector::ZeroVector;
						const FVector Want = Lobby->GetBodyCentre(Each) - FVector(0.f, 0.f, 56.f);
						const float Off = static_cast<float>(FVector::Dist2D(FVector(RootAt.X, RootAt.Z, 0.f), FVector(Want.X, Want.Z, 0.f)));
						MostMeshOff = FMath::Max(MostMeshOff, Off);
						MeshLine += FString::Printf(TEXT(" %s %.0f,%.0f;"), *Each->GetDisplayName(), RootAt.X - Want.X, RootAt.Z - Want.Z);
					}
					Hang += Lobby->GetPose(Each) == PinguLobby::EPose::Hang ? 1 : 0;
					MostShift = FMath::Max(MostShift, FMath::Abs(Lobby->GetTagShift(Each)));
					FVector2D Screen;
					if (Player->ProjectWorldLocationToScreen(Lobby->GetBodyCentre(Each), Screen, false))
					{
						const APinguLobby::FHit Hit = Lobby->ProbeAtScreen(Player, Screen);
						const bool bOk = Hit.Kind == APinguLobby::EHit::Body && Hit.Penguin.Get() == Each;
						BodyClicks += bOk ? 1 : 0;
						Bad += bOk ? TEXT("") : *FString::Printf(TEXT(" body of %s got %s;"), *Each->GetDisplayName(), APinguLobby::HitName(Hit.Kind));
					}
					FVector Tag;
					if (Lobby->GetNameTagSpot(Each, Tag) && Player->ProjectWorldLocationToScreen(Tag, Screen, false))
					{
						const APinguLobby::FHit Hit = Lobby->ProbeAtScreen(Player, Screen);
						const bool bOk = Hit.Kind == APinguLobby::EHit::Name && Hit.Penguin.Get() == Each;
						NameClicks += bOk ? 1 : 0;
						Bad += bOk ? TEXT("") : *FString::Printf(TEXT(" name of %s got %s;"), *Each->GetDisplayName(), APinguLobby::HitName(Hit.Kind));
					}
				}
				// What covers what on the screen (different penguins only; the "+" with anything).
				const TArray<APinguLobby::FTarget> Targets = Lobby->GetClickTargets();
				int32 BodyOverBody = 0;
				int32 TagOverTag = 0;
				int32 TagOverBody = 0;
				int32 NearPlus = 0;
				for (int32 I = 0; I < Targets.Num(); ++I)
				{
					for (int32 J = I + 1; J < Targets.Num(); ++J)
					{
						const APinguLobby::FTarget& A = Targets[I];
						const APinguLobby::FTarget& B = Targets[J];
						const bool bPlus = A.Kind == APinguLobby::EHit::Plus || B.Kind == APinguLobby::EHit::Plus;
						if (!bPlus && A.Penguin == B.Penguin)
						{
							continue;
						}
						FBox2D BoxA;
						FBox2D BoxB;
						if (!TargetOnScreen(Player, A, BoxA) || !TargetOnScreen(Player, B, BoxB))
						{
							continue;
						}
						// A body counts as covered only in its middle (60 % of its width, up to the head): the tags stand in front of the island, so
						// on the screen a neighbour's tag can reach just over a body's edge (its clicks there go to the tag first).
						const bool bBodyA = A.Kind == APinguLobby::EHit::Body;
						const bool bBodyB = B.Kind == APinguLobby::EHit::Body;
						if (bBodyA != bBodyB)
						{
							FBox2D& Body = bBodyA ? BoxA : BoxB;
							const float Trim = 0.2f * static_cast<float>(Body.GetSize().X);
							Body.Min.X += Trim;
							Body.Max.X -= Trim;
							// (and only up to the head: the click box reaches 10-20 cm over it, to just under its own tags)
							Body.Min.Y += 0.15f * static_cast<float>(Body.GetSize().Y);
						}
						if (!BoxA.Intersect(BoxB))
						{
							continue;
						}
						const FString What = FString::Printf(TEXT(" %s(%s) over %s(%s);"), APinguLobby::HitName(A.Kind), A.Penguin.IsValid() ? *A.Penguin->GetDisplayName() : TEXT("-"),
							APinguLobby::HitName(B.Kind), B.Penguin.IsValid() ? *B.Penguin->GetDisplayName() : TEXT("-"));
						Bad += What;
						if (bPlus)
						{
							++NearPlus;
						}
						else if (bBodyA && bBodyB)
						{
							++BodyOverBody;
						}
						else if (bBodyA || bBodyB)
						{
							++TagOverBody;
						}
						else
						{
							++TagOverTag;
						}
					}
				}
				int32 Stand = 0;
				for (APenguinCharacter* Each : GameMode->GetPenguins())
				{
					Stand += IsValid(Each) && Lobby->GetPose(Each) == PinguLobby::EPose::Stand ? 1 : 0;
				}
				const bool bOk = Count == PinguMaxPenguins && OnSpots == Count && Hang == 0 && Sit == 0 && Stand == Count && BodyOverBody == 0 && TagOverTag == 0
					&& TagOverBody == 0 && NearPlus == 0 && MostShift < 25.f && BodyClicks == Count && NameClicks == Count && MostMeshOff < 45.f;
				Report(FString::Printf(TEXT("RESULT lobby spots drawn: the meshes' root bones at most %.0f cm from the drawn bodies' feet (want under 45):%s %s"), MostMeshOff, *MeshLine,
					MostMeshOff < 45.f ? TEXT("ok") : TEXT("FAIL")));
				Report(FString::Printf(TEXT("RESULT lobby spots: %d penguins (want %d), %d on spots of their own (%d standing, %d sitting, %d hanging - want all standing; %d spots), bodies over bodies %d, tags over tags %d, tags over a body %d, near the + %d, tags pushed at most %.1f cm (want under 25: still over their penguin), clicks on the body %d / name %d of %d%s %s"),
					Count, PinguMaxPenguins, OnSpots, Stand, Sit, Hang, Lobby->GetSpots().Num(), BodyOverBody, TagOverTag, TagOverBody, NearPlus, MostShift, BodyClicks, NameClicks, Count,
					Bad.IsEmpty() ? TEXT("") : *(TEXT(";") + Bad.Left(600)), bOk ? TEXT("ok") : TEXT("FAIL")));
				// Feet on the snow: every penguin at rest exactly where it stands in a match (its feet not sunk, not floating).
				Report(FString::Printf(TEXT("RESULT lobby feet at rest: %d samples, the feet at most %.1f cm off the plain standing (want under 2) %s"),
					L.FeetSamples, L.FeetRestOff, L.FeetSamples > 100 && L.FeetRestOff < 2.f ? TEXT("ok") : TEXT("FAIL")));
			}
			// Feet sampled at rest (no gags here), then two ground-level close-ups: the lowest standing penguin and the highest.
			if (T > 1.8f && T < 3.4f)
			{
				for (APenguinCharacter* Each : GameMode->GetPenguins())
				{
					if (IsValid(Each) && Lobby->GetPopScale(Each) > 0.99f)
					{
						L.FeetRestOff = FMath::Max(L.FeetRestOff, FMath::Abs(FeetDrop(Each)));
						++L.FeetSamples;
					}
				}
			}
			const auto CloseUp = [&](float When, bool bHighest)
			{
				const FString Key = FString::Printf(TEXT("ground %d"), bHighest ? 1 : 0);
				if (T < When || L.Taken.Contains(Key))
				{
					return;
				}
				L.Taken.Add(Key);
				const APenguinCharacter* Pick = nullptr;
				for (APenguinCharacter* Each : GameMode->GetPenguins())
				{
					if (IsValid(Each) && (!Pick || (bHighest ? Each->GetActorLocation().Z > Pick->GetActorLocation().Z + 1.f
						: Each->GetActorLocation().X < Pick->GetActorLocation().X)))
					{
						Pick = Each;
					}
				}
				if (Pick)
				{
					if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(World))
					{
						Stage->FocusOn(Pick->GetActorLocation() - FVector(0.f, 0.f, 40.f), 1.6f, 0.28f);
					}
				}
			};
			CloseUp(3.6f, false);
			if (T >= 4.7f && !L.Taken.Contains(TEXT("74_lobby_ground_a")))
			{
				L.Taken.Add(TEXT("74_lobby_ground_a"));
				Shot(TEXT("74_lobby_ground_a"), false);
			}
			CloseUp(5.6f, true);
			if (T >= 6.7f && !L.Taken.Contains(TEXT("74_lobby_ground_b")))
			{
				L.Taken.Add(TEXT("74_lobby_ground_b"));
				Shot(TEXT("74_lobby_ground_b"), false);
			}
		},
		[this]
		{
			FGagLook& L = GagLook();
			if (!L.bJudged)
			{
				Report(TEXT("RESULT lobby spots: never judged (no lobby) FAIL"));
			}
			CloseTestLobby(GetWorld(), L.Hidden);
		});

	// The waiting gags, each started on purpose (the random picks paused), a picture of each: a noot at a neighbour (it
	// flinches, never pushed), a noot at the camera, the fish (fat, then back to normal), a wave, a dance, a doze (Zzz), a
	// shiver; then into practice: nothing of it carries on (no fat, no pose, no props).
	AddStep(TEXT("look_LobbyGags"), 18.5f,
		[this]
		{
			FGagLook& L = GagLook();
			L = FGagLook();
			PinguLobby::SetGagsPaused(true);
			ClearAbilityLeftovers();
			OpenTestLobby(GetWorld(), Penguin.Get(), 4, L.Hidden);
			VirtualInput = FVector2D::ZeroVector;
		},
		[this](float T)
		{
			using PinguLobby::EGag;
			FGagLook& L = GagLook();
			UWorld* World = GetWorld();
			APenguinGameMode* GameMode = World->GetAuthGameMode<APenguinGameMode>();
			APinguLobby* Lobby = APinguLobby::Get(World);
			if (!GameMode || !Lobby)
			{
				return;
			}
			TArray<APenguinCharacter*> Bots;
			for (APenguinCharacter* Each : GameMode->GetPenguins())
			{
				if (IsValid(Each) && !Each->GetPlayerState() && Lobby->GetPopScale(Each) > 0.99f)
				{
					Bots.Add(Each);
				}
			}
			Bots.Sort([](const APenguinCharacter& A, const APenguinCharacter& B) { return A.GetActorLocation().X < B.GetActorLocation().X; });
			const auto ShotAt = [&](float When, const TCHAR* Name, bool bUI)
			{
				if (T >= When && !L.Taken.Contains(Name))
				{
					L.Taken.Add(Name);
					Shot(Name, bUI);
				}
			};
			const auto Play = [&](float When, APenguinCharacter* Who, EGag Gag)
			{
				if (T >= When && Who && !L.Plays.ContainsByPredicate([When](const FGagLook::FPlay& Item) { return Item.At == When; }))
				{
					FGagLook::FPlay& New = L.Plays.AddDefaulted_GetRef();
					New.Who = Who;
					New.Gag = Gag;
					New.At = When;
					New.bStarted = Lobby->StartGag(Who, Gag);
				}
			};
			// Nothing carries into the next thing: fat, a doze and its Zzz on, then off to practice (the bots go there).
			if (T >= 16.6f && !L.bCarryArmed && Bots.Num() > 0)
			{
				L.bCarryArmed = true;
				Lobby->SetFatFor(Penguin.Get(), 30.f);
				Lobby->StartGag(Bots[0], EGag::Doze);
			}
			if (T >= 17.5f && !L.bPracticed)
			{
				L.bPracticed = true;
				PinguLobby::SetGagsPaused(false);
				GameMode->StartPractice();
			}
			if (T >= 18.2f && L.Carry.IsEmpty())
			{
				const PinguLobby::FAnimState State = PinguLobby::GetAnimState(Penguin.Get());
				const float Scale = BodyBoneScale(Penguin.Get());
				const float Root = RootBoneOffset(Penguin.Get());
				L.bCarryOk = !Lobby->IsShowing() && !State.bLobby && State.Fat == 0.f && Lobby->CountShownProps() == 0 && L.YouBaseScale > 0.f
					&& FMath::Abs(Scale / L.YouBaseScale - 1.f) < 0.03f && Root >= 0.f && Root < 20.f;
				L.Carry = FMath::IsNearlyEqual(L.YouBaseScale, 0.f) ? TEXT("no body bone") : FString::Printf(TEXT("lobby showing %d, lobby anim %d, fat %.2f, props %d, body scale %.3f of the lobby's %.3f, root bone %.1f cm off"),
					Lobby->IsShowing() ? 1 : 0, State.bLobby ? 1 : 0, State.Fat, Lobby->CountShownProps(), Scale, L.YouBaseScale, Root);
			}
			if (Bots.Num() < 4 || L.bPracticed)
			{
				return;
			}
			APenguinCharacter* Hanger = nullptr;
			for (APenguinCharacter* Each : Bots)
			{
				Hanger = !Hanger && Lobby->GetPose(Each) == PinguLobby::EPose::Hang ? Each : Hanger;
			}
			APenguinCharacter* Fat = Bots[1] != Hanger ? Bots[1] : Bots[2];
			if (T < 0.9f)
			{
				L.BaseScale = BodyBoneScale(Fat);
				L.YouBaseScale = BodyBoneScale(Penguin.Get());
			}
			// The plan (never more than two at once).
			Play(1.f, Bots[0] != Hanger ? Bots[0] : Bots[3], EGag::NootFriend);
			Play(3.2f, Bots[2] != Hanger ? Bots[2] : Bots[3], EGag::NootCamera);
			Play(5.4f, Fat, EGag::Fish);
			Play(8.3f, Hanger ? Hanger : Bots[3], EGag::Wave);
			Play(10.6f, Bots[3] != Hanger ? Bots[3] : Bots[0], EGag::Dance);
			Play(10.61f, Bots[0] != Hanger ? Bots[0] : Bots[2], EGag::Doze);
			Play(13.9f, Fat, EGag::Shiver);
			if (T >= 9.2f && T < 9.3f)
			{
				Lobby->SetFatFor(Fat, 1.f); // (the fat of a real fish lasts 10-20 s: back to normal sooner here)
			}
			// What each gag does, as seen here.
			for (FGagLook::FPlay& Each : L.Plays)
			{
				APenguinCharacter* Who = Each.Who.Get();
				float Age = -1.f;
				const EGag Now = Who ? Lobby->GetGag(Who, Age) : EGag::None;
				const float Since = T - Each.At;
				Each.bSeen |= Now == Each.Gag && Since < 0.5f;
				if (Since > PinguLobby::GetGagLength(Each.Gag) + 0.15f && Since < PinguLobby::GetGagLength(Each.Gag) + 0.6f && Now != Each.Gag)
				{
					Each.bEnded = true;
				}
				if (Now == EGag::NootFriend && Who)
				{
					L.FriendHonk = FMath::Max(L.FriendHonk, Who->GetHonkWeight());
				}
				if (Now == EGag::NootCamera && Who)
				{
					L.CameraHonk = FMath::Max(L.CameraHonk, Who->GetHonkWeight());
				}
			}
			for (APenguinCharacter* Each : GameMode->GetPenguins())
			{
				const PinguLobby::FAnimState State = PinguLobby::GetAnimState(Each);
				if (State.FlinchAge >= 0.f && !L.Flinched.IsValid())
				{
					L.Flinched = Each;
					L.FlinchedName = Each->GetDisplayName();
					L.FlinchedAt = Each->GetActorLocation();
				}
			}
			if (L.Flinched.IsValid() && T < 3.2f)
			{
				L.FlinchedMoved = FMath::Max(L.FlinchedMoved, static_cast<float>(FVector::Dist(L.Flinched->GetActorLocation(), L.FlinchedAt)));
			}
			if (T > 5.4f && T < 9.2f)
			{
				L.MostFat = FMath::Max(L.MostFat, Lobby->GetFat(Fat));
				L.FatScale = FMath::Max(L.FatScale, BodyBoneScale(Fat));
			}
			if (T > 5.6f && T < 6.9f)
			{
				L.FishProps = FMath::Max(L.FishProps, Lobby->CountShownProps());
			}
			if (T > 10.3f && T < 10.5f)
			{
				L.FatAfter = Lobby->GetFat(Fat);
				L.AfterScale = BodyBoneScale(Fat);
			}
			for (APenguinCharacter* Each : GameMode->GetPenguins())
			{
				if (IsValid(Each) && Lobby->GetPopScale(Each) > 0.99f)
				{
					const float Drop = FeetDrop(Each);
					++L.FeetSamples;
					if (Drop < L.FeetLowest)
					{
						float GagAge = -1.f;
						L.FeetLowest = Drop;
						L.FeetLowestWho = FString::Printf(TEXT("%s (%s)"), *Each->GetDisplayName(), PinguLobby::GetGagName(Lobby->GetGag(Each, GagAge)));
					}
				}
			}
			if (T > 11.5f && T < 14.f)
			{
				L.ZzzProps = FMath::Max(L.ZzzProps, Lobby->CountShownProps());
			}
			// Close-ups (the camera's knock-out zoom on the one playing): the noot at the camera, the fish going down and the fat, the doze.
			const auto CloseOn = [&](float When, APenguinCharacter* Who, float Hold = 1.4f)
			{
				const FString Key = FString::Printf(TEXT("focus %.2f"), When);
				if (T >= When && Who && !L.Taken.Contains(Key))
				{
					L.Taken.Add(Key);
					if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(World))
					{
						Stage->FocusOn(Lobby->GetBodyCentre(Who) + FVector(0.f, 0.f, 40.f), Hold, 0.4f);
					}
				}
			};
			for (const FGagLook::FPlay& Each : L.Plays)
			{
				if (Each.Gag == EGag::NootCamera || Each.Gag == EGag::Doze)
				{
					CloseOn(Each.At + (Each.Gag == EGag::Doze ? 1.4f : 0.1f), Each.Who.Get());
				}
			}
			CloseOn(4.8f, Fat, 4.4f); // the fish eater, zoomed in before it starts (the strip's frames share one framing)
			ShotAt(1.6f, TEXT("74_lobby_gag_noot_friend"), false);
			ShotAt(3.85f, TEXT("74_lobby_gag_noot_camera"), false);
			// The fish as a strip of frames (close up): held, looked at, the wind-up, the toss, high up, falling into the open beak,
			// the gulp's bulge, swollen, the burp.
			static const float FishFrames[] = { 0.45f, 0.8f, 1.f, 1.2f, 1.45f, 1.72f, 2.f, 2.6f, 3.05f };
			for (int32 Frame = 0; Frame < UE_ARRAY_COUNT(FishFrames); ++Frame)
			{
				const FString Name = FString::Printf(TEXT("74_lobby_fish_%02d"), Frame + 1);
				if (T >= 5.4f + FishFrames[Frame] && !L.Taken.Contains(Name))
				{
					// Where the eater is in the picture (the strip is cut round it).
					FVector2D Screen;
					APlayerController* Viewer = World->GetFirstPlayerController();
					if (Viewer && Fat && Viewer->ProjectWorldLocationToScreen(Lobby->GetBodyCentre(Fat) + FVector(0.f, 0.f, 30.f), Screen, false))
					{
						Report(FString::Printf(TEXT("fish strip frame %d at %.0f, %.0f px"), Frame + 1, Screen.X, Screen.Y));
					}
				}
				ShotAt(5.4f + FishFrames[Frame], *Name, false);
			}
			ShotAt(8.6f, TEXT("74_lobby_gag_fat"), false);
			ShotAt(9.f, TEXT("74_lobby_gag_wave"), false);
			ShotAt(11.4f, TEXT("74_lobby_gag_dance"), false);
			ShotAt(12.75f, TEXT("74_lobby_gag_doze"), false);
			ShotAt(14.95f, TEXT("74_lobby_gag_startle"), false);
			ShotAt(14.8f, TEXT("74_lobby_gag_shiver"), false);
		},
		[this]
		{
			using PinguLobby::EGag;
			FGagLook& L = GagLook();
			UWorld* World = GetWorld();
			const APinguLobby* Lobby = APinguLobby::Get(World);
			FString Gags;
			int32 Good = 0;
			for (const FGagLook::FPlay& Each : L.Plays)
			{
				const bool bOk = Each.bStarted && Each.bSeen && Each.bEnded;
				Good += bOk ? 1 : 0;
				Gags += FString::Printf(TEXT(" %s %s/%s/%s;"), PinguLobby::GetGagName(Each.Gag), Each.bStarted ? TEXT("started") : TEXT("NOT started"),
					Each.bSeen ? TEXT("playing") : TEXT("NOT playing"), Each.bEnded ? TEXT("ended") : TEXT("NOT ended"));
			}
			const int32 Most = Lobby ? Lobby->GetMostGagsAtOnce() : 99;
			const bool bGags = L.Plays.Num() == 7 && Good == 7 && Most <= 2;
			Report(FString::Printf(TEXT("RESULT lobby gags: %d of %d started, played and ended (%d at once at most, want 2 or fewer):%s %s"), Good, L.Plays.Num(), Most, *Gags,
				bGags ? TEXT("ok") : TEXT("FAIL")));
			const bool bNoot = !L.FlinchedName.IsEmpty() && L.FlinchedMoved < 3.f && L.FriendHonk > 0.5f && L.CameraHonk > 0.5f;
			Report(FString::Printf(TEXT("RESULT lobby noot: at a neighbour - %s flinched, moved %.1f cm (want under 3: never pushed), the horn %.2f; at the camera - the horn %.2f (want over 0.5) %s"),
				L.FlinchedName.IsEmpty() ? TEXT("nobody") : *L.FlinchedName, L.FlinchedMoved, L.FriendHonk, L.CameraHonk, bNoot ? TEXT("ok") : TEXT("FAIL")));
			const bool bFat = L.FishProps >= 1 && L.MostFat > 0.9f && L.BaseScale > 0.f && L.FatScale / L.BaseScale > 1.25f && L.FatAfter == 0.f && FMath::Abs(L.AfterScale / L.BaseScale - 1.f) < 0.03f;
			Report(FString::Printf(TEXT("RESULT lobby fish: the fish shown %d, fat %.2f (want over 0.9), body bone %.3f -> %.3f (x%.2f, want over 1.25), back to normal: fat %.2f, body %.3f (want %.3f) %s"),
				L.FishProps, L.MostFat, L.BaseScale, L.FatScale, L.BaseScale > 0.f ? L.FatScale / L.BaseScale : 0.f, L.FatAfter, L.AfterScale, L.BaseScale, bFat ? TEXT("ok") : TEXT("FAIL")));
			Report(FString::Printf(TEXT("RESULT lobby feet during the gags: %d samples, the feet at most %.1f cm below the plain standing%s (want under 2: never sunk) %s"),
				L.FeetSamples, -L.FeetLowest, L.FeetLowestWho.IsEmpty() ? TEXT("") : *(TEXT(", ") + L.FeetLowestWho), L.FeetSamples > 500 && L.FeetLowest > -2.f ? TEXT("ok") : TEXT("FAIL")));
			Report(FString::Printf(TEXT("RESULT lobby doze: the Zzz shown (%d props, want 1 or more) %s"), L.ZzzProps, L.ZzzProps >= 1 ? TEXT("ok") : TEXT("FAIL")));
			Report(FString::Printf(TEXT("RESULT lobby gags carry over: %s %s"), L.Carry.IsEmpty() ? TEXT("never judged") : *L.Carry, L.bCarryOk ? TEXT("ok") : TEXT("FAIL")));
			CloseTestLobby(World, L.Hidden);
		});
}
