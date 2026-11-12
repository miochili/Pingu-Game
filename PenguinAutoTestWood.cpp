// Pingu - the GUI wood's tiling (card 2026-11-07_trait-logo-wood-tiling; Linus: "matke the texture so it doesnt repeat cus u can
// see the tiling really well"). APenguinAutoTest::AddWoodTilingStep:
//
//  look_WoodTiling   the settings page, the PINGUPEDIA and the lobby, each in the 2D menus: every pair of neighbouring wood planks
//                    of one class (stacked rows, or side by side) is laid out by PinguWood::LayoutPlank exactly as the painter
//                    does, and compared along their common stretch: they must not start with the same tile (variant, mirrored or
//                    not, about the same length: want 0 pairs), and the share of the stretch where both show the same grain at
//                    the same place (the same atlas column within 8 texels) must stay small (want 15 % or less; the old 2-tile
//                    rule with equal lengths gave about half). Inside a plank no tile follows itself (want 0).
//                    Shots 130_wood_settings, 130_wood_pingupedia, 130_wood_lobby.

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinHUD.h"
#include "PinguMenu2D.h"
#include "PinguMenu3D.h"
#include "PinguWoodStyle.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

namespace PenguinAutoTestWoodPrivate
{
	/** A wood plank of the open page: its centre (menu units: Y right, Z up), body size, class and the tiles it is drawn with. */
	struct FPlank
	{
		FVector2D Centre = FVector2D::ZeroVector;
		float W = 0.f;
		float H = 0.f;
		PinguWood::EClass Class = PinguWood::EClass::Board;
		uint32 Seed = 0;
		int32 Start = -1;
		/** The middle's tile quads: X along the plank (from its left end), U range (U0 > U1: mirrored). */
		TArray<PinguWood::FQuad> Tiles;
		int32 Repeats = 0;
	};

	struct FRun
	{
		TWeakObjectPtr<APinguMenu3D> Menu;
		int32 PageIndex = -1;
		float PageAt = 0.f;
		bool bSampled = false;
		int32 Planks = 0;
		int32 Pairs = 0;
		int32 SameStart = 0;
		float WorstShare = 0.f;
		float ShareSum = 0.f;
		int32 Repeats = 0;
		FString Pages;
		/** The pairs that start alike (page, seeds, centres), for the log. */
		FString SameStartList;
		/** The pair with the most grain in line (for the log). */
		FString WorstPair;
		const TCHAR* Page = TEXT("");
	};
	static FRun& Run()
	{
		static FRun Value;
		return Value;
	}

	/** The painter's seed of a menu part (PinguMenu2D.cpp, its Slab case: the grain offset the menu code sets per plank and its
	 *  board variant). Kept the same as there: the check must see the planks as they are drawn. */
	static uint32 SeedOf(const UPinguMenu2DPart& Part)
	{
		return static_cast<uint32>(FMath::Abs(Part.GrainOffset.X * 7919.0 + Part.GrainOffset.Y * 104729.0)) + 977u * Part.WoodVariant;
	}

	/** The planks of the menu's open page (one board each: stacked panels are skipped). */
	static void CollectPlanks(APinguMenu3D* Menu, TArray<FPlank>& Out)
	{
		const USceneComponent* MenuRoot = Menu->GetMenuRootComponent();
		if (!MenuRoot)
		{
			return;
		}
		TInlineComponentArray<UPinguMenu2DPart*> Parts(Menu);
		for (const UPinguMenu2DPart* Part : Parts)
		{
			if (!Part || !Part->IsVisible() || Part->Kind != EPinguMenu2DKind::Slab || !Part->bWood || Part->bShadow || Part->bVertical)
			{
				continue;
			}
			bool bShown = true;
			for (const USceneComponent* Item = Part; Item && Item != MenuRoot; Item = Item->GetAttachParent())
			{
				bShown &= Item->IsVisible();
			}
			if (!bShown || Part->Width < 40.f || Part->Height < 10.f)
			{
				continue;
			}
			const PinguWood::ECut Cut = static_cast<PinguWood::ECut>(Part->WoodCut);
			const PinguWood::EState State = static_cast<PinguWood::EState>(Part->WoodState);
			const PinguWood::EClass Class = PinguWood::ClassOf(Cut, State, Part->Height);
			if (Class == PinguWood::EClass::Track || PinguWood::StackCount(Class, Part->Height, 1.f) != 1)
			{
				continue;
			}
			FPlank& Plank = Out.AddDefaulted_GetRef();
			// (Its place at rest on the page and its first tile as the painter gave them in its last paint.)
			Plank.Centre = PinguMenu2D::GetRestPlace(*Part);
			Plank.Start = PinguMenu2D::GetWoodStart(Part);
			Plank.W = Part->Width;
			Plank.H = Part->Height;
			Plank.Class = Class;
			Plank.Seed = SeedOf(*Part);
			TArray<PinguWood::FQuad> Quads;
			PinguWood::LayoutPlank(Class, State, Part->Width, Part->Height, SeedOf(*Part), 1.f, Quads, false, Plank.Start);
			// (The middle: every quad but the two caps.)
			for (int32 Index = 1; Index + 1 < Quads.Num(); ++Index)
			{
				Plank.Tiles.Add(Quads[Index]);
				if (Index > 1 && FMath::IsNearlyEqual(FMath::Min(Quads[Index].U0, Quads[Index].U1), FMath::Min(Quads[Index - 1].U0, Quads[Index - 1].U1), 1.f))
				{
					++Plank.Repeats;
				}
			}
		}
	}

	/** The atlas column a plank shows at X along it (its own left end = 0), or -1 on a cap. */
	static float ColumnAt(const FPlank& Plank, float X)
	{
		for (const PinguWood::FQuad& Q : Plank.Tiles)
		{
			if (X >= Q.X0 && X < Q.X1)
			{
				return FMath::Lerp(Q.U0, Q.U1, (X - Q.X0) / FMath::Max(Q.X1 - Q.X0, 0.001f));
			}
		}
		return -1.f;
	}

	/** Compares two neighbours over their common stretch: same start, and the share showing the same column at the same place. */
	static void Compare(const FPlank& A, const FPlank& B, FRun& R)
	{
		const float Left = FMath::Max(A.Centre.X - 0.5f * A.W, B.Centre.X - 0.5f * B.W);
		const float Right = FMath::Min(A.Centre.X + 0.5f * A.W, B.Centre.X + 0.5f * B.W);
		++R.Pairs;
		if (A.Tiles.Num() > 0 && B.Tiles.Num() > 0)
		{
			const PinguWood::FQuad& FA = A.Tiles[0];
			const PinguWood::FQuad& FB = B.Tiles[0];
			const float TileA = FA.X1 - FA.X0;
			const float TileB = FB.X1 - FB.X0;
			if (FMath::IsNearlyEqual(FA.U0, FB.U0, 1.f) && FMath::IsNearlyEqual(FA.U1, FB.U1, 1.f)
				&& FMath::Abs(TileA - TileB) < 0.1f * FMath::Max(TileA, TileB))
			{
				++R.SameStart;
				R.SameStartList += FString::Printf(TEXT(" [%s: seeds %u / %u at %.0f, %.0f / %.0f, %.0f, %.0f / %.0f wide]"), R.Page, A.Seed, B.Seed,
					A.Centre.X, A.Centre.Y, B.Centre.X, B.Centre.Y, A.W, B.W);
			}
		}
		if (Right - Left < 20.f)
		{
			return;
		}
		int32 Samples = 0;
		int32 Same = 0;
		for (float X = Left; X < Right; X += 2.f)
		{
			const float CA = ColumnAt(A, X - (A.Centre.X - 0.5f * A.W));
			const float CB = ColumnAt(B, X - (B.Centre.X - 0.5f * B.W));
			if (CA < 0.f || CB < 0.f)
			{
				continue;
			}
			++Samples;
			Same += FMath::Abs(CA - CB) <= 8.f ? 1 : 0;
		}
		const float Share = Samples > 0 ? static_cast<float>(Same) / static_cast<float>(Samples) : 0.f;
		if (Share > R.WorstShare)
		{
			R.WorstShare = Share;
			R.WorstPair = FString::Printf(TEXT("%s: starts %d / %d at %.0f, %.0f / %.0f, %.0f, %.0f / %.0f wide, %d / %d tiles"), R.Page, A.Start, B.Start,
				A.Centre.X, A.Centre.Y, B.Centre.X, B.Centre.Y, A.W, B.W, A.Tiles.Num(), B.Tiles.Num());
		}
		R.ShareSum += Share;
	}

	static void Measure(APinguMenu3D* Menu, FRun& R, const TCHAR* Page)
	{
		R.Page = Page;
		TArray<FPlank> Planks;
		CollectPlanks(Menu, Planks);
		const int32 PairsBefore = R.Pairs;
		for (int32 I = 0; I < Planks.Num(); ++I)
		{
			R.Repeats += Planks[I].Repeats;
			for (int32 J = I + 1; J < Planks.Num(); ++J)
			{
				const FPlank& A = Planks[I];
				const FPlank& B = Planks[J];
				if (A.Class != B.Class)
				{
					continue;
				}
				const bool bNeighbours = PinguWood::AreNeighbours(A.Centre, A.W, A.H, B.Centre, B.W, B.H);
				if (bNeighbours)
				{
					Compare(A, B, R);
				}
			}
		}
		R.Planks += Planks.Num();
		R.Pages += FString::Printf(TEXT("%s%s %d planks %d pairs"), R.Pages.IsEmpty() ? TEXT("") : TEXT(", "), Page, Planks.Num(), R.Pairs - PairsBefore);
	}

	struct FPageJob
	{
		EPinguMenu3DPage Page;
		const TCHAR* Name;
		const TCHAR* ShotName;
	};
	static const FPageJob Jobs[] = {
		{ EPinguMenu3DPage::Settings, TEXT("settings"), TEXT("130_wood_settings") },
		{ EPinguMenu3DPage::Pingupedia, TEXT("pingupedia"), TEXT("130_wood_pingupedia") },
		{ EPinguMenu3DPage::Lobby, TEXT("lobby"), TEXT("130_wood_lobby") },
	};
	constexpr float PageTime = 1.4f;
}

void APenguinAutoTest::AddWoodTilingStep()
{
	using namespace PenguinAutoTestWoodPrivate;
	AddStep(TEXT("look_WoodTiling"), PageTime * UE_ARRAY_COUNT(Jobs) + 0.6f,
		[this]
		{
			FRun& R = Run();
			R = FRun();
			APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
			APenguinHUD* HUD = Controller ? Cast<APenguinHUD>(Controller->GetHUD()) : nullptr;
			R.Menu = PinguMenu3D::IsEnabled() && PinguMenu3D::IsFlat() && HUD ? APinguMenu3D::Get(HUD) : nullptr;
		},
		[this](float T)
		{
			FRun& R = Run();
			APinguMenu3D* Menu = R.Menu.Get();
			if (!Menu)
			{
				return;
			}
			const int32 Want = FMath::Min(FMath::FloorToInt(T / PageTime), static_cast<int32>(UE_ARRAY_COUNT(Jobs)) - 1);
			if (Want != R.PageIndex)
			{
				R.PageIndex = Want;
				R.PageAt = T;
				R.bSampled = false;
				Menu->SetTestPage(Jobs[Want].Page);
			}
			// (After the page has come in: its planks stand still.)
			if (!R.bSampled && T > R.PageAt + 1.1f)
			{
				R.bSampled = true;
				Measure(Menu, R, Jobs[R.PageIndex].Name);
				Shot(Jobs[R.PageIndex].ShotName, true);
			}
		},
		[this]
		{
			FRun& R = Run();
			if (APinguMenu3D* Menu = R.Menu.Get())
			{
				Menu->SetTestPage(EPinguMenu3DPage::None);
			}
			const float Mean = R.Pairs > 0 ? R.ShareSum / static_cast<float>(R.Pairs) : 0.f;
			const bool bOk = R.Menu.IsValid() && R.Pairs >= 10 && R.SameStart == 0 && R.WorstShare <= 0.15f && R.Repeats == 0;
			Report(FString::Printf(TEXT("RESULT wood tiling: %d planks, %d neighbour pairs (want 10 or more; %s), pairs starting with the same tile %d (want 0), "
				"the same grain at the same place: worst %.0f %% mean %.1f %% of their common stretch (want 15 %% or less), a tile right after itself %d (want 0): %s"),
				R.Planks, R.Pairs, *R.Pages, R.SameStart, 100.f * R.WorstShare, 100.f * Mean, R.Repeats, bOk ? TEXT("ok") : TEXT("FAIL")));
			if (!R.SameStartList.IsEmpty())
			{
				UE_LOG(LogPingu, Log, TEXT("[AUTOTEST] wood tiling: pairs starting alike:%s"), *R.SameStartList);
			}
			UE_LOG(LogPingu, Log, TEXT("[AUTOTEST] wood tiling: most grain in line: %s"), *R.WorstPair);
		});
}
