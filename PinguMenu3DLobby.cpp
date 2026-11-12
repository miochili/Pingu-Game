// Pingu - the wooden card lobby (2026-10-29, card 2026-10-29_wood-card-lobby; Linus: "i think the old lobby was better, can you
// use the old layout and things but with the new wooden style and design guidlines"), decluttered the same day (card
// 2026-10-29_lobby-declutter; Linus: "make the lobby gui less clutterd use only the things accualy nedded so it looks less full
// and easyer to use"). A page of APinguMenu3D: a board of loose planks with a snow slab that holds only the players' cards and ADD
// BOT (five to a row, a second row only when needed; no empty places) and grows and shrinks smoothly with them; each card a small
// plank sign with the portrait (its team as the ring round it, only when in a team) and the name (yours in gold); a bot's card also
// a small difficulty chip (a click steps it) and a small x. Under the cards one row of signs - BACK (LAN: LEAVE / CLOSE GAME),
// SETTINGS, TRAITS, START the big one - and the host's "Others join with <address>" line, small and light. Behind it the title's
// island (no lobby island); Esc hides the panel and you play on the island while waiting, Esc again brings it back
// (APenguinHUD::TogglePauseMenu, bLobbyMenuOpen). The lobby island stays behind -PinguIslandLobby (PinguMenu3D::UseIslandLobby).

#include "PinguMenu3D.h"
#include "PinguWoodStyle.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinHUD.h"
#include "PinguUI.h"
#include "Misc/App.h"

#define LOCTEXT_NAMESPACE "PinguMenu3D"

namespace PinguMenu3DLobbyPrivate
{
	/** The cards (design pixels): five to a row at most. */
	constexpr int32 CardsPerRow = 5;
	constexpr float CardWidth = 128.f;
	constexpr float CardHeight = 164.f;
	constexpr float CardGap = 18.f;
	constexpr float CardRadius = 12.f;
	/** The board: its top stays put (it grows downward), its padding, the title on the top plank, the gaps. */
	constexpr float PanelTop = 220.f;
	constexpr float PanelPad = 30.f;
	constexpr float TitleHeight = 36.f;
	constexpr float TitleGap = 16.f;
	constexpr float ButtonsGap = 24.f;
	constexpr float ButtonHeight = 46.f;
	constexpr float StartHeight = 58.f;
	constexpr float AddressGap = 8.f;
	constexpr float AddressHeight = 30.f;
	constexpr float GridTop = PanelTop - PanelPad - TitleHeight - TitleGap;
	/** The signs: BACK / LEAVE / CLOSE GAME, SETTINGS, TRAITS, START (one row, START the big one). */
	constexpr float SignGap = 12.f;
	constexpr float BackW = 172.f;
	constexpr float SettingsW = 150.f;
	constexpr float TraitsW = 164.f;
	/** The small LIST sign right of TRAITS: the host's TRAITS list (2026-11-07, shared-traits; PinguMenu3DTraits.cpp). */
	constexpr float TraitListW = 84.f;
	constexpr float StartW = 210.f;
	constexpr float SignsWidth = BackW + SettingsW + TraitsW + TraitListW + StartW + 4.f * SignGap;
	/** A card's parts: they stand out of its face by this much (the x and the difficulty chip stand in front of the popped card). */
	constexpr float Relief = 2.f;
	constexpr float FrontProtrude = 42.f;
	constexpr float Portrait = 64.f;
	constexpr float RemoveSize = 26.f;
	constexpr float ChipWidth = 96.f;
	constexpr float ChipHeight = 26.f;
	/** How fast the board follows its size (units per second at most, eased). */
	constexpr float GrowSpeed = 9.f;

	static FText DifficultyName(int32 Skill)
	{
		switch (Skill)
		{
		case 0: return LOCTEXT("CardLobbyEasy", "EASY");
		case 2: return LOCTEXT("CardLobbyHard", "HARD");
		default: return LOCTEXT("CardLobbyNormal", "NORMAL");
		}
	}
	static FLinearColor DifficultyColor(int32 Skill)
	{
		const FPinguStyle& Style = FPinguStyle::Get();
		return Skill == 0 ? Style.Green : (Skill == 2 ? Style.Red : Style.Gold);
	}
	static float GridWidthOf(int32 Count)
	{
		const int32 Columns = FMath::Clamp(Count, 1, CardsPerRow);
		return Columns * CardWidth + (Columns - 1) * CardGap;
	}
	static float GridHeightOf(int32 Count)
	{
		const int32 Rows = FMath::Max(1, (Count + CardsPerRow - 1) / CardsPerRow);
		return Rows * CardHeight + (Rows - 1) * CardGap;
	}
	/** The middle of the Place-th card of Count, each row centred. */
	static FVector2D PlaceCenter(int32 Place, int32 Count)
	{
		const int32 Row = Place / CardsPerRow;
		const int32 Column = Place % CardsPerRow;
		const int32 InRow = FMath::Min(CardsPerRow, Count - Row * CardsPerRow);
		const float RowLeft = -0.5f * GridWidthOf(InRow);
		return FVector2D(RowLeft + 0.5f * CardWidth + Column * (CardWidth + CardGap), GridTop - 0.5f * CardHeight - Row * (CardHeight + CardGap));
	}
	static FString CardId(const FPinguLobbyPlayer& Card)
	{
		return FString::Printf(TEXT("%s_%s"), Card.bBot ? TEXT("B") : TEXT("P"), *Card.Name);
	}
	/** The board's size for these cards: as wide as the cards or the signs, as tall as they need (the address line when it shows). */
	static float PanelWidthFor(float GridWidth)
	{
		return FMath::Max(GridWidth, SignsWidth) + 2.f * PanelPad;
	}
	static float ButtonsZFor(float GridHeight)
	{
		return GridTop - GridHeight - ButtonsGap - 0.5f * StartHeight;
	}
	static float AddressZFor(float GridHeight)
	{
		return ButtonsZFor(GridHeight) - 0.5f * StartHeight - AddressGap - 0.5f * AddressHeight;
	}
	static float PanelHeightFor(float GridHeight, bool bAddress)
	{
		const float Bottom = bAddress ? AddressZFor(GridHeight) - 0.5f * AddressHeight : ButtonsZFor(GridHeight) - 0.5f * StartHeight;
		return PanelTop - Bottom + PanelPad;
	}
}

void APinguMenu3D::BuildCardLobby()
{
	using namespace PinguMenu3DLobbyPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	bCardLobby = true;
	// The board: fewer, taller loose planks than the other boards, a little overhang (calm), a snow slab on the top plank.
	Lobby.PlankRow = 118.f;
	Lobby.PlankOverhang = 34.f;
	CardLobbyTargetW = PanelWidthFor(GridWidthOf(2));
	CardLobbyTargetGridH = GridHeightOf(2);
	CardLobbyW = CardLobbyTargetW;
	CardLobbyGridH = CardLobbyTargetGridH;
	BeginPage(Lobby, EPinguMenu3DPage::Lobby, FVector2D(CardLobbyW, PanelHeightFor(CardLobbyGridH, false)));
	Lobby.DefaultFocus = TEXT("Start");

	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	auto Authority = [WeakHUD]() { const APenguinHUD* H = WeakHUD.Get(); return H && H->HasMatchAuthority(); };
	auto Network = [WeakHUD]() { const APenguinHUD* H = WeakHUD.Get(); return H && H->IsNetworkGame(); };

	// A small title on the top plank.
	AddLabel(Lobby, nullptr, FVector2D(0.f, PanelTop - PanelPad - 0.5f * TitleHeight + 4.f), 30.f, [] { return LOCTEXT("CardLobbyTitle", "LOBBY"); },
		Style.Snow, PinguMenu3D::ETextAlign::Center, 0.f, true, 6.f);

	// ---- the signs (one row, centred): BACK / LEAVE / CLOSE GAME, SETTINGS, TRAITS, START. LayoutCardLobby moves them up and down.
	const float ButtonsZ = ButtonsZFor(CardLobbyGridH);
	float X = -0.5f * SignsWidth;
	NextSignPoint = -1;
	FButton& Back = AddButton(Lobby, TEXT("Back"), EPinguMenu3DKind::Ghost, FVector2D(X + 0.5f * BackW, ButtonsZ), FVector2D(BackW, ButtonHeight), 15.f,
		[] { return LOCTEXT("CardLobbyBack", "BACK"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_MainMenu(); } });
	Back.Visible = [Network] { return !Network(); };
	NextSignPoint = -1;
	FButton& Leave = AddButton(Lobby, TEXT("Leave"), EPinguMenu3DKind::Danger, Back.Center, FVector2D(BackW, ButtonHeight), 15.f,
		[Authority] { return Authority() ? LOCTEXT("CardLobbyClose", "CLOSE GAME") : LOCTEXT("CardLobbyLeave", "LEAVE"); },
		[WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_LeaveGame(); } });
	Leave.Visible = Network;
	X += BackW + SignGap;
	AddButton(Lobby, TEXT("Settings"), EPinguMenu3DKind::Normal, FVector2D(X + 0.5f * SettingsW, ButtonsZ), FVector2D(SettingsW, ButtonHeight), 15.f,
		[] { return LOCTEXT("CardLobbySettings", "SETTINGS"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_Settings(); } });
	X += SettingsW + SignGap;
	const float TraitsX = X + 0.5f * TraitsW;
	FButton& Traits = AddButton(Lobby, TEXT("Traits"), EPinguMenu3DKind::Toggle, FVector2D(TraitsX, ButtonsZ), FVector2D(TraitsW, ButtonHeight), 15.f,
		[WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->AreTraitsOn() ? LOCTEXT("CardLobbyTraitsOn", "TRAITS ON") : LOCTEXT("CardLobbyTraitsOff", "TRAITS OFF"); },
		[WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_ToggleTraits(); } });
	Traits.Visible = Authority;
	Traits.Selected = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->AreTraitsOn(); };
	X += TraitsW + SignGap;
	// LIST: the host's TRAITS list (which traits the round bag deals; TRAITS stays the on / off switch).
	FButton& TraitList = AddButton(Lobby, TEXT("TraitList"), EPinguMenu3DKind::Normal, FVector2D(X + 0.5f * TraitListW, ButtonsZ),
		FVector2D(TraitListW, ButtonHeight), 15.f, [] { return LOCTEXT("CardLobbyTraitList", "LIST"); },
		[WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_TraitPicks(); } });
	TraitList.Visible = Authority;
	X += TraitListW + SignGap;
	const float StartX = X + 0.5f * StartW;
	NextSignPoint = 1;
	FButton& Start = AddButton(Lobby, TEXT("Start"), EPinguMenu3DKind::Primary, FVector2D(StartX, ButtonsZ), FVector2D(StartW, StartHeight), 24.f,
		[] { return LOCTEXT("CardLobbyStart", "START"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_StartLobbyMatch(); } });
	Start.Visible = Authority;
	// A LAN client: the host's TRAITS choice to read, and "Waiting for <host>..." where START is.
	CardTraitsLabel = AddLabel(Lobby, nullptr, FVector2D(TraitsX, ButtonsZ), 15.f,
		[WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->AreTraitsOn() ? LOCTEXT("CardLobbyTraitsOnRead", "TRAITS ON") : LOCTEXT("CardLobbyTraitsOffRead", "TRAITS OFF"); },
		Style.IceDim, PinguMenu3D::ETextAlign::Center, TraitsW, true, 6.f);
	Lobby.Labels[CardTraitsLabel].Visible = [Authority] { return !Authority(); };
	CardWaitingLabel = AddLabel(Lobby, nullptr, FVector2D(StartX, ButtonsZ), 15.f,
		[WeakHUD]
		{
			const APenguinHUD* H = WeakHUD.Get();
			static const TCHAR* Steps[] = { TEXT(""), TEXT("."), TEXT(".."), TEXT("...") };
			return FText::Format(LOCTEXT("CardLobbyWaiting", "Waiting for {0}{1}"), FText::FromString(H ? H->GetHostName() : FString()),
				FText::FromString(Steps[static_cast<int32>(PinguUI::Now() * 3.0) % 4]));
		}, Style.Gold, PinguMenu3D::ETextAlign::Center, StartW, true, 6.f);
	Lobby.Labels[CardWaitingLabel].Visible = [Authority] { return !Authority(); };

	// ---- the host's address line, small and light under the signs: "Others join with <address>" and the copy icon (a click
	// copies it: COPIED! in green)
	AddressLeft = -0.5f * SignsWidth;
	FButton& Address = AddButton(Lobby, TEXT("Address"), EPinguMenu3DKind::Ghost, FVector2D(0.f, AddressZFor(CardLobbyGridH)),
		FVector2D(360.f, AddressHeight), 13.f,
		[WeakHUD]
		{
			const APenguinHUD* H = WeakHUD.Get();
			if (H && H->IsHostAddressCopied())
			{
				return FText::Format(LOCTEXT("CardLobbyJoinCopied", "Others join with  {0}"), LOCTEXT("CardLobbyCopied", "COPIED!"));
			}
			return FText::Format(LOCTEXT("CardLobbyJoin", "Others join with  {0}"), FText::FromString(H ? H->GetHostAddressText() : FString()));
		},
		[WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_CopyHostAddress(); } });
	Address.Visible = [Authority, Network] { return Authority() && Network(); };
	Address.bInstant = true;
	Address.Protrude = 8.f;
	Address.LabelColor = [] { return FPinguStyle::Get().IceDim; };
	CopyIcon = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	CopyIcon->SetupAttachment(Address.Body);
	CopyIcon->RegisterComponent();
	CopyBack.Build(this, CopyIcon);
	CopyBack.SetShape(11.f, 13.f, 3.f, 4.f);
	CopyBack.Root->SetRelativeLocation(FVector(-2.f, -3.f, 3.f));
	CopyFront.Build(this, CopyIcon);
	CopyFront.SetShape(11.f, 13.f, 3.f, 4.f);
	CopyFront.Root->SetRelativeLocation(FVector(-5.f, 3.f, -3.f));
	EndStatic(Lobby);
}

void APinguMenu3D::LayoutCardLobby(FPageParts& Parts, float Delta)
{
	using namespace PinguMenu3DLobbyPrivate;
	const APenguinHUD* PenguinHUD = HUD.Get();
	const bool bAddress = PenguinHUD && PenguinHUD->HasMatchAuthority() && PenguinHUD->IsNetworkGame();
	// The board follows its target size, eased (quick, then slowing down: it grows and shrinks with the cards).
	const float Ease = 1.f - FMath::Exp(-GrowSpeed * FMath::Max(Delta, 0.f));
	CardLobbyW = FMath::Abs(CardLobbyTargetW - CardLobbyW) < 0.5f ? CardLobbyTargetW : FMath::Lerp(CardLobbyW, CardLobbyTargetW, Ease);
	CardLobbyGridH = FMath::Abs(CardLobbyTargetGridH - CardLobbyGridH) < 0.5f ? CardLobbyTargetGridH : FMath::Lerp(CardLobbyGridH, CardLobbyTargetGridH, Ease);
	const float Height = PanelHeightFor(CardLobbyGridH, bAddress);
	if (!Parts.Plane.IsBuilt())
	{
		return;
	}
	if (!FMath::IsNearlyEqual(CardLobbyW, Parts.Plane.Width, 0.25f) || !FMath::IsNearlyEqual(Height, Parts.Plane.Height, 0.25f))
	{
		SetPlaneShape(Parts, CardLobbyW, Height);
	}
	// (Its top stays put: it grows downward. Also the first time - BeginPage made it at the right size, in the middle.)
	if (!FMath::IsNearlyEqual(static_cast<float>(Parts.Plane.Root->GetRelativeLocation().Z), PanelTop - 0.5f * Height, 0.05f))
	{
		Parts.Plane.Root->SetRelativeLocation(FVector(0.f, 0.f, PanelTop - 0.5f * Height));
	}
	// The signs and the address line under the cards.
	const float ButtonsZ = ButtonsZFor(CardLobbyGridH);
	const float AddressZ = AddressZFor(CardLobbyGridH);
	for (int32 Index = 0; Index < Parts.StaticButtons && Index < Parts.Buttons.Num(); ++Index)
	{
		FButton& Button = Parts.Buttons[Index];
		const float Z = Button.Id == FName(TEXT("Address")) ? AddressZ : ButtonsZ;
		if (!FMath::IsNearlyEqual(Button.Center.Y, Z, 0.05f) && Button.Root)
		{
			Button.Center.Y = Z;
			Button.Root->SetRelativeLocation(FVector(0.f, Button.Center.X, Z));
		}
	}
	for (const int32 LabelIndex : { CardTraitsLabel, CardWaitingLabel })
	{
		if (Parts.Labels.IsValidIndex(LabelIndex) && Parts.Labels[LabelIndex].Text)
		{
			FLabel& Label = Parts.Labels[LabelIndex];
			const FVector Old = Label.Text->GetRelativeLocation();
			if (FMath::IsNearlyEqual(static_cast<float>(Old.Z), ButtonsZ - 7.5f, 0.05f))
			{
				continue;
			}
			Label.Text->SetRelativeLocation(FVector(Old.X, Old.Y, ButtonsZ - 7.5f));
			if (Label.Ink)
			{
				const FVector OldInk = Label.Ink->GetRelativeLocation();
				Label.Ink->SetRelativeLocation(FVector(OldInk.X, OldInk.Y, ButtonsZ - 7.5f));
			}
		}
	}
}

void APinguMenu3D::UpdateCardLobby(FPageParts& Parts)
{
	using namespace PinguMenu3DLobbyPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return;
	}
	const TArray<FPinguLobbyPlayer> Cards = PenguinHUD->GetLobbyCards();
	const bool bAuthority = PenguinHUD->HasMatchAuthority();
	const bool bNetwork = PenguinHUD->IsNetworkGame();
	const bool bAdd = bAuthority && PenguinHUD->CanAddBot();
	const bool bRemove = bAuthority && PenguinHUD->CanRemoveBot();
	const int32 Count = FMath::Min(Cards.Num() + (bAdd ? 1 : 0), PinguMaxPenguins);
	CardLobbyTargetW = PanelWidthFor(GridWidthOf(Count));
	CardLobbyTargetGridH = GridHeightOf(Count) + GetPicksHeight(); // (+ the ability picks' row: PinguMenu3DPicks.cpp)
	LayoutCardLobby(Parts, FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.1f));
	FString Key = FString::Printf(TEXT("%d%d%d%d:"), bAuthority ? 1 : 0, bNetwork ? 1 : 0, bAdd ? 1 : 0, bRemove ? 1 : 0);
	for (const FPinguLobbyPlayer& Card : Cards)
	{
		Key += FString::Printf(TEXT("%s/%d/%d/%d/%d/%d/%d/%d/%d|"), *Card.Name, Card.Slot, Card.bHost ? 1 : 0, Card.bYou ? 1 : 0, Card.bBot ? 1 : 0, Card.BotIndex,
			Card.TeamChoice, Card.Skill, Card.bWaiting ? 1 : 0);
	}
	Key += GetPicksKey();
	if (Key == Parts.RowsKey)
	{
		return;
	}
	// The hot button keeps its place through the rebuild (its id: the same card, its x, its difficulty, ADD BOT, a sign).
	const bool bInput = &Parts == GetInputParts();
	FName WasHot = bInput ? GetHotId() : NAME_None;
	if (WasHot.ToString().StartsWith(TEXT("PickCell_")))
	{
		WasHot = FName(*FString::Printf(TEXT("PickSlot_%c"), WasHot.ToString()[9])); // (a pick closes the grid: back to its slot)
	}
	const bool bWasMouse = Parts.bHotByMouse;
	const bool bFirst = Parts.RowsKey.IsEmpty();
	ClearRows(Parts);
	Parts.RowsKey = Key;
	if (bFirst)
	{
		// (The page shows: the board at its size at once, no growing in.)
		CardLobbyW = CardLobbyTargetW;
		CardLobbyGridH = CardLobbyTargetGridH;
		LayoutCardLobby(Parts, 0.f);
	}

	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	int32 Place = 0;
	const float Top = 0.5f * CardHeight;
	// A card's x (a bot's, or the host's kick x on a joiner's): just a red "X" with the page's soft word shadow, no plank or disc
	// behind it (Linus 2026-11-07: "the cross in the lobby to kick has no background"). A plain button: it pops, tilts and brightens
	// on the hover and the focus like the rest. It stands in front of the card (it always wins the click), a little in from the corner.
	const auto AddCornerX = [this, &Parts, Top](const FString& CardKey, const TCHAR* Prefix, const FVector2D& Center, TFunction<void()> OnClick)
	{
		FButton& Remove = AddButton(Parts, FName(*(FString(Prefix) + CardKey)), EPinguMenu3DKind::Plain,
			FVector2D(Center.X + 0.5f * CardWidth - 21.f, Center.Y + Top - 21.f), FVector2D(RemoveSize + 8.f, RemoveSize + 8.f), 26.f,
			[] { return LOCTEXT("CardLobbyRemove", "X"); }, MoveTemp(OnClick));
		Remove.Protrude = FrontProtrude;
		Remove.LabelColor = [] { return PinguWood::Danger; };
		Remove.PlainHover = FMath::Lerp(PinguWood::Danger, FLinearColor::White, 0.45f);
	};
	for (const FPinguLobbyPlayer& Card : Cards)
	{
		if (Place >= PinguMaxPenguins)
		{
			break;
		}
		const FVector2D Center = PlaceCenter(Place++, Count);
		const FString Id = CardId(Card);
		// Yours to change: your own card and - the host's - the bots' (a click: the next team).
		const bool bMine = Card.bYou || (Card.bBot && bAuthority);
		const bool bBot = Card.bBot;
		const int32 BotIndex = Card.BotIndex;
		TFunction<void()> Action;
		if (bMine)
		{
			Action = [WeakHUD, bBot, BotIndex]
			{
				if (APenguinHUD* H = WeakHUD.Get())
				{
					bBot ? H->Action_ChangeBotTeam(BotIndex, 1) : H->Action_ChangeTeamChoice(1, true);
				}
			};
		}
		FButton& CardButton = AddButton(Parts, FName(*(TEXT("Card_") + Id)), EPinguMenu3DKind::Normal, Center, FVector2D(CardWidth, CardHeight), 14.f,
			[] { return FText::GetEmpty(); }, MoveTemp(Action));
		CardButton.bFocusable = bMine;
		// (A small plaque: plank corners, not a sign's round ends.)
		CardButton.Slab.SetShape(CardWidth, CardHeight, CardRadius, CardButton.Slab.Depth);
		USceneComponent* Face = CardButton.Body;
		// The portrait in its hat's colour; its team as the ring round it (only when in a team).
		const float PortraitZ = Top - 14.f - 0.5f * Portrait;
		AddPortrait(Parts, Face, FVector2D(0.f, PortraitZ), Portrait, Card.Slot, Card.TeamChoice, Relief);
		// The name in the colour of its penguin; yours in gold (the only "you" marker).
		const FLinearColor Color = APenguinCharacter::GetSlotColor(Card.Slot);
		const FString Name = PinguUI::Upper(Card.Name).ToString();
		const float NameZ = PortraitZ - 0.5f * Portrait - 18.f;
		AddLabel(Parts, Face, FVector2D(0.f, NameZ), 16.f, [Name] { return FText::FromString(Name); },
			Card.bYou ? Style.Gold : FMath::Lerp(Color, FLinearColor::White, 0.3f), PinguMenu3D::ETextAlign::Center, CardWidth - 16.f, true, Relief);
		if (bBot)
		{
			// A bot's difficulty: one small chip in its colour (the host clicks through EASY, NORMAL, HARD; the others read it).
			const int32 Skill = Card.Skill;
			const FLinearColor Words = PinguMenu3D::TagTextColor(DifficultyColor(Skill));
			const float ChipZ = -Top + 8.f + 0.5f * ChipHeight;
			if (bAuthority)
			{
				FButton& SkillButton = AddButton(Parts, FName(*(TEXT("Skill_") + Id)), EPinguMenu3DKind::Segment, FVector2D(Center.X, Center.Y + ChipZ),
					FVector2D(ChipWidth, ChipHeight), 13.f, [] { return FText::GetEmpty(); },
					[WeakHUD, BotIndex] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_ChangeBotSkill(BotIndex, 1); } });
				SkillButton.Protrude = FrontProtrude;
				SkillButton.LabelColor = [Words] { return Words; };
				// (Its word as a label of its own: a sign's label is fitted between its nails - too small on a chip.)
				AddLabel(Parts, SkillButton.Body, FVector2D(0.f, 0.f), 13.f, [Skill] { return DifficultyName(Skill); }, Words, PinguMenu3D::ETextAlign::Center,
					0.f, true, Relief);
			}
			else
			{
				AddPill(Parts, Face, FVector2D(0.f, ChipZ), ChipHeight - 2.f, 13.f, DifficultyName(Skill), PinguMenu3D::WoodRow * 0.9f, Words, Relief,
					PinguMenu3D::ETextAlign::Center);
			}
			if (bRemove)
			{
				AddCornerX(Id, TEXT("Remove_"), Center, [WeakHUD, BotIndex] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_RemoveBot(BotIndex); } });
			}
		}
		else if (PenguinHUD->CanKick(Card))
		{
			// The host's x on a joiner's card (the same small red x as a bot's; a joiner never sees one): sends them away.
			const int32 PlayerId = Card.PlayerId;
			AddCornerX(Id, TEXT("Remove_"), Center, [WeakHUD, PlayerId] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_KickPlayer(PlayerId); } });
		}
	}
	if (bAdd && Place < PinguMaxPenguins)
	{
		// ADD BOT: a big plain "+" in the card's place, no plank, no ball (Linus 2026-11-07: "Just a big + (no background)"); the page's
		// soft word shadow, and it pops, tilts and turns gold on the hover and the focus.
		const FVector2D Center = PlaceCenter(Place++, Count);
		FButton& Add = AddButton(Parts, TEXT("AddBot"), EPinguMenu3DKind::Plain, Center, FVector2D(CardWidth, CardHeight), 96.f,
			[] { return LOCTEXT("CardLobbyAddBotPlus", "+"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_AddBot(); } });
		Add.Protrude = FrontProtrude;
		Add.LabelColor = [] { return FLinearColor(0.95f, 0.95f, 0.92f); };
	}
	AddPicksRows(Parts, GridTop - GridHeightOf(Count)); // the ability picks under the cards (CHOOSE / the host's switch)
	// (Rows made while the page shows: shown now - ShowPage showed only the buttons it had.)
	for (int32 Index = Parts.StaticButtons; Index < Parts.Buttons.Num(); ++Index)
	{
		SetButtonVisible(Parts.Buttons[Index], Parts.bShown);
	}
	if (!WasHot.IsNone())
	{
		for (int32 Index = 0; Index < Parts.Buttons.Num(); ++Index)
		{
			if (Parts.Buttons[Index].Id == WasHot && IsButtonLive(Parts.Buttons[Index]))
			{
				Parts.Hot = Index;
				Parts.bHotByMouse = bWasMouse;
				break;
			}
		}
	}
	UE_LOG(LogPingu, Log, TEXT("[MENU3D] card lobby: %d cards, add %d, remove %d (%s)"), Cards.Num(), bAdd ? 1 : 0, bRemove ? 1 : 0, bAuthority ? TEXT("host / solo") : TEXT("client"));
}

#undef LOCTEXT_NAMESPACE
