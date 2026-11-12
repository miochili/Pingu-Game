// Pingu - flat text on the face of a 3D menu panel (see PinguMenu3DOverlay.h).

#include "PinguMenu3DOverlay.h"
#include "Penguin.h"
#include "PenguinHUD.h"
#include "PinguMenu3D.h"
#include "PinguMenu2D.h"
#include "PinguUI.h"
#include "Engine/World.h"
#include "Layout/ArrangedChildren.h"
#include "Widgets/Text/STextBlock.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"

SPinguMenu3DOverlay::SPinguMenu3DOverlay()
	: Children(this)
{
	SetCanTick(true);
	bCanSupportFocus = false;
}

void SPinguMenu3DOverlay::Construct(const FArguments& InArgs)
{
	HUD = InArgs._HUD;
	// Nothing here takes clicks: they go to the input layer under this one.
	SetVisibility(EVisibility::HitTestInvisible);
}

APinguMenu3D* SPinguMenu3DOverlay::GetMenu() const
{
	return HUD.IsValid() && PinguMenu3D::IsEnabled() ? APinguMenu3D::Get(HUD.Get()) : nullptr;
}

void SPinguMenu3DOverlay::Rebuild()
{
	Children.Empty();
	Blocks.Reset();
	Places.Reset();
	APinguMenu3D* Menu = GetMenu();
	const int32 Count = Menu ? Menu->GetFlatCount() : 0;
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FText Text;
		FSlateFontInfo Font;
		FLinearColor Color;
		int32 Justify = 0;
		bool bVisible = false;
		if (!Menu->GetFlat(Index, Text, Font, Color, Justify, bVisible))
		{
			continue;
		}
		// The text, its colour and whether it shows follow the menu's flat by index (the set of flats is rebuilt together).
		auto MenuOf = [WeakHUD]() -> APinguMenu3D* { return WeakHUD.IsValid() && PinguMenu3D::IsEnabled() ? APinguMenu3D::Get(WeakHUD.Get()) : nullptr; };
		float Width = 100.f;
		float Height = 20.f;
		Menu->GetFlatSize(Index, Width, Height);
		TSharedRef<STextBlock> Block = SNew(STextBlock)
			.Font(Font)
			// (A little slack: the card's lines are wrapped to the box already - the fit check measures them; the shadow made Slate
			// break a full line once more, "falls" / "faster" on lines of their own, 2026-10-26.)
			.WrapTextAt(Width + 8.f)
			// (A soft dark shadow, no outline: an outline made the lines wider and taller than the card's layout.)
			.ShadowOffset(FVector2D(1.5f, 1.5f))
			.ShadowColorAndOpacity(FLinearColor(0.02f, 0.01f, 0.005f, 0.9f))
			.Justification(Justify == 1 ? ETextJustify::Center : (Justify == 2 ? ETextJustify::Right : ETextJustify::Left))
			.Text_Lambda([MenuOf, Index]()
			{
				FText T;
				FSlateFontInfo F;
				FLinearColor C;
				int32 J = 0;
				bool bV = false;
				const APinguMenu3D* M = MenuOf();
				return M && M->GetFlat(Index, T, F, C, J, bV) ? T : FText::GetEmpty();
			})
			.ColorAndOpacity_Lambda([MenuOf, Index]()
			{
				FText T;
				FSlateFontInfo F;
				FLinearColor C = FLinearColor::White;
				int32 J = 0;
				bool bV = false;
				const APinguMenu3D* M = MenuOf();
				return FSlateColor(M && M->GetFlat(Index, T, F, C, J, bV) ? C : FLinearColor::White);
			});
		Block->SetRenderTransformPivot(FVector2D(0.f, 0.f));
		FSlot::FSlotArguments Args(MakeUnique<FSlot>());
		Args.AttachWidget(Block);
		Children.AddSlot(MoveTemp(Args));
		Blocks.Add(Block);
		Places.AddDefaulted();
	}
	BuiltVersion = Menu ? Menu->GetFlatVersion() : -1;
}

void SPinguMenu3DOverlay::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SPanel::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	APinguMenu3D* Menu = GetMenu();
	if (!Menu)
	{
		if (Blocks.Num() > 0)
		{
			Rebuild();
		}
		return;
	}
	if (Menu->GetFlatVersion() != BuiltVersion || Blocks.Num() != Menu->GetFlatCount())
	{
		Rebuild();
	}
	const UWorld* World = HUD.IsValid() ? HUD->GetWorld() : nullptr;
	for (int32 Index = 0; Index < Blocks.Num(); ++Index)
	{
		FPlace& Place = Places[Index];
		FText Text;
		FSlateFontInfo Font;
		FLinearColor Color;
		int32 Justify = 0;
		bool bVisible = false;
		FVector2D TL;
		FVector2D TR;
		FVector2D BL;
		Place.bShown = Menu->GetFlat(Index, Text, Font, Color, Justify, bVisible) && bVisible && Menu->GetFlatFrame(Index, TL, TR, BL);
		Blocks[Index]->SetVisibility(Place.bShown ? EVisibility::HitTestInvisible : EVisibility::Collapsed);
		if (!Place.bShown)
		{
			continue;
		}
		// The box's corners as this panel's local points (viewport pixels -> the screen -> local).
		FVector2D Corners[3] = { TL, TR, BL };
		for (FVector2D& Corner : Corners)
		{
			FVector2D Screen;
			if (!PinguMenu3D::ViewportPixelToScreen(World, Corner, Screen))
			{
				Place.bShown = false;
				break;
			}
			Corner = AllottedGeometry.AbsoluteToLocal(Screen);
		}
		if (!Place.bShown)
		{
			Blocks[Index]->SetVisibility(EVisibility::Collapsed);
			continue;
		}
		// The flat's own size (page units) is what its text was laid out with: the projected width over it is the layout scale
		// (the text is rasterised at that scale: crisp); the rest of the panel's turn is a small 2D skew (a render transform
		// about the box's top left corner).
		float Width = 1.f;
		float Height = 1.f;
		Menu->GetFlatSize(Index, Width, Height);
		Width = FMath::Max(Width, 1.f);
		Height = FMath::Max(Height, 1.f);
		const FVector2D U = Corners[1] - Corners[0];
		const FVector2D V = Corners[2] - Corners[0];
		Place.TopLeft = Corners[0];
		Place.Size = FVector2D(Width, Height);
		Place.Scale = FMath::Max(static_cast<float>(U.Size()) / Width, 0.01f);
		const float WS = Width * Place.Scale;
		const float HS = Height * Place.Scale;
		Place.Skew = FMatrix2x2(static_cast<float>(U.X) / WS, static_cast<float>(U.Y) / WS, static_cast<float>(V.X) / HS, static_cast<float>(V.Y) / HS);
		Blocks[Index]->SetRenderTransform(FSlateRenderTransform(Place.Skew));
	}
}

void SPinguMenu3DOverlay::OnArrangeChildren(const FGeometry& AllottedGeometry, FArrangedChildren& ArrangedChildren) const
{
	for (int32 Index = 0; Index < Children.Num() && Index < Places.Num(); ++Index)
	{
		const FPlace& Place = Places[Index];
		const TSharedRef<SWidget>& Widget = Children[Index].GetWidget();
		if (!Place.bShown || ArrangedChildren.Accepts(Widget->GetVisibility()) == false)
		{
			continue;
		}
		// (A little taller than the box: the texts' dark edge (2026-10-26) makes each line a few pixels taller, the last one must
		// not be cut off.)
		ArrangedChildren.AddWidget(AllottedGeometry.MakeChild(Widget, Place.Size + FVector2D(0.f, 0.2f * Place.Size.Y + 8.f), FSlateLayoutTransform(Place.Scale, Place.TopLeft)));
	}
}

int32 SPinguMenu3DOverlay::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	// The 2D menus (2026-10-27): the signs, boards, tags and their shading first, under the flat texts and the words.
	int32 BaseLayer = LayerId;
	APinguMenu3D* Menu = GetMenu();
	if (Menu && PinguMenu3D::IsFlat())
	{
		BaseLayer = PinguMenu2D::Paint(HUD.IsValid() ? HUD->GetWorld() : nullptr, Menu, AllottedGeometry, OutDrawElements, LayerId) + 1;
	}
	int32 Layer = SPanel::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, BaseLayer, InWidgetStyle, bParentEnabled);
	if (!Menu || !FSlateApplication::IsInitialized())
	{
		return Layer;
	}
	Menu->GetOverlayTexts(Texts);
	if (Texts.Num() == 0)
	{
		return Layer;
	}
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const UWorld* World = HUD.IsValid() ? HUD->GetWorld() : nullptr;
	auto ToLocal = [World, &AllottedGeometry](const FVector2D& Pixel, FVector2D& OutLocal)
	{
		FVector2D Screen;
		if (!PinguMenu3D::ViewportPixelToScreen(World, Pixel, Screen))
		{
			return false;
		}
		OutLocal = AllottedGeometry.AbsoluteToLocal(Screen);
		return true;
	};
	// The letters' edge and shadow: a dark brown, like the wood's darkest grain.
	const FLinearColor Edge(0.03f, 0.016f, 0.008f, 1.f);
	const FLinearColor Shadow(0.f, 0.f, 0.f, 0.45f);
	const int32 ShadowLayer = Layer + 1;
	const int32 TextLayer = Layer + 2;
	// The font size per screen unit of capital height: taken from the longer words (their width matches the 3D letters' width
	// well), then used for every text, so single digits and short tags get the same letter size as the words around them.
	FSlateFontInfo Probe = FPinguStyle::Title(20.f);
	const float RefSize = Probe.Size;
	for (const FPinguMenu3DTextDraw& Draw : Texts)
	{
		FVector2D TL;
		FVector2D TR;
		FVector2D BL;
		if (Draw.Text.Len() < 5 || !ToLocal(Draw.TopLeft, TL) || !ToLocal(Draw.TopRight, TR) || !ToLocal(Draw.BottomLeft, BL))
		{
			continue;
		}
		const float Cap = static_cast<float>((BL - TL).Size());
		const float RefWidth = static_cast<float>(Measure->Measure(Draw.Text, Probe).X);
		if (Cap > 2.f && RefWidth > 1.f)
		{
			const float Sample = RefSize * static_cast<float>((TR - TL).Size()) / RefWidth / Cap;
			SizePerCap = SizePerCap > 0.f ? FMath::Lerp(SizePerCap, Sample, 0.05f) : Sample;
		}
	}
	if (SizePerCap <= 0.f)
	{
		return Layer;
	}
	for (const FPinguMenu3DTextDraw& Draw : Texts)
	{
		FVector2D TL;
		FVector2D TR;
		FVector2D BL;
		if (!ToLocal(Draw.TopLeft, TL) || !ToLocal(Draw.TopRight, TR) || !ToLocal(Draw.BottomLeft, BL))
		{
			continue;
		}
		const FVector2D U = TR - TL;
		const FVector2D V = BL - TL;
		const float Width = static_cast<float>(U.Size());
		const float Cap = static_cast<float>(V.Size());
		if (Width < 1.f || Cap < 2.f)
		{
			continue;
		}
		// The size from the capitals' height on the screen (in half steps: the font cache stays small).
		FSlateFontInfo Font = Probe;
		Font.Size = FMath::Clamp(FMath::RoundToFloat(2.f * SizePerCap * Cap) * 0.5f, 4.f, 240.f);
		// Dark words (a tag's on its light pill) need no edge and no shadow; light words get both.
		const bool bDark = Draw.Color.GetLuminance() < 0.2f;
		Font.OutlineSettings.OutlineSize = bDark ? 0 : FMath::Clamp(FMath::RoundToInt(Font.Size * (Draw.bBig ? 0.11f : 0.13f)), 1, 10);
		Font.OutlineSettings.OutlineColor = Edge;
		FVector2D Measured = Measure->Measure(Draw.Text, Font);
		{
			// A word that runs wider than its 3D word at this size gets a smaller font (crisp, not a squeezed picture).
			const float Fit = Width / FMath::Max(static_cast<float>(Measured.X) - 2.f * Font.OutlineSettings.OutlineSize, 1.f);
			const float CapScale = Cap * SizePerCap / Font.Size;
			if (Fit < 0.97f * CapScale)
			{
				Font.Size = FMath::Max(FMath::FloorToFloat(2.f * Font.Size * Fit / CapScale) * 0.5f, 4.f);
				Font.OutlineSettings.OutlineSize = bDark ? 0 : FMath::Clamp(FMath::RoundToInt(Font.Size * (Draw.bBig ? 0.11f : 0.13f)), 1, 10);
				Measured = Measure->Measure(Draw.Text, Font);
			}
		}
		const float SlateWidth = static_cast<float>(Measured.X);
		const float MaxHeight = Measure->GetMaxCharacterHeight(Font);
		const float Baseline = MaxHeight + Measure->GetBaseline(Font);
		// Slate's text space -> this panel: x along the 3D line, y down across it (one scale: the screen per Slate unit), the
		// words' middle on the 3D words' middle, the baseline on the 3D baseline.
		const FVector2D Along = U.GetSafeNormal();
		// (Never wider than the 3D word, 2026-10-26, fourth pass: the 3D layout fits every word into its sign, pill, row or box,
		// so the drawn word fits too - a word whose letters run wider in Slate is drawn a little smaller.)
		const float FitScale = Width / FMath::Max(SlateWidth - 2.f * Font.OutlineSettings.OutlineSize, 1.f);
		const FVector2D Ax = Along * FMath::Min(Cap * SizePerCap / Font.Size, 1.02f * FitScale);
		const FVector2D Ay = V.GetSafeNormal() * Ax.Size();
		const FVector2D Middle = BL + 0.5 * U;
		const FVector2D Origin = Middle - Ax * (0.5f * SlateWidth) - Ay * Baseline;
		const FSlateRenderTransform Turn(FMatrix2x2(static_cast<float>(Ax.X), static_cast<float>(Ax.Y), static_cast<float>(Ay.X), static_cast<float>(Ay.Y)));
		const FVector2f Size(static_cast<float>(Measured.X), MaxHeight);
		const FVector2D Down = Ay.GetSafeNormal() * FMath::Max(1.5f, 0.07f * Font.Size);
		FSlateFontInfo ShadowFont = Font;
		ShadowFont.OutlineSettings.OutlineColor = Shadow;
		if (!bDark)
		{
			FSlateDrawElement::MakeText(OutDrawElements, ShadowLayer,
				AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform(FVector2f(Origin + Down)), Turn, FVector2f(0.f, 0.f)), Draw.Text, ShadowFont,
				ESlateDrawEffect::None, Shadow);
		}
		FSlateDrawElement::MakeText(OutDrawElements, TextLayer,
			AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform(FVector2f(Origin)), Turn, FVector2f(0.f, 0.f)), Draw.Text, Font,
			ESlateDrawEffect::None, Draw.Color);
	}
	return TextLayer;
}
