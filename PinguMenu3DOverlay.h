// Pingu - flat text on the face of a 3D menu panel (PinguMenu3D.h). Long body text (the PINGUPEDIA's descriptions and stat
// lines, the key capture's hint, the error popup's message) is ordinary Slate text: a paragraph in Text3D would be hundreds of
// glyph meshes rebuilt on every selection. SPinguMenu3DOverlay draws it where the 3D panel's face is on the screen, every
// frame: each text's box is projected through the player's camera (its top left, top right and bottom left corner), and the
// text block is laid out at that place and scale with a render transform for the panel's slight turn (the parallax and the
// sway), so it reads as printed on the panel. Hit-test invisible: the clicks go to the SPinguMenu3DInput layer under it.
// With the 2D menus (the default since 2026-10-27, PinguMenu2D.h) it also paints the menu's signs and boards under the words.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SPanel.h"
#include "Layout/Children.h"
#include "PinguMenu3D.h"

class APenguinHUD;
class APinguMenu3D;
class STextBlock;

class SPinguMenu3DOverlay : public SPanel
{
public:
	SLATE_BEGIN_ARGS(SPinguMenu3DOverlay) {}
		SLATE_ARGUMENT(TWeakObjectPtr<APenguinHUD>, HUD)
	SLATE_END_ARGS()

	SPinguMenu3DOverlay();
	void Construct(const FArguments& InArgs);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual void OnArrangeChildren(const FGeometry& AllottedGeometry, FArrangedChildren& ArrangedChildren) const override;
	virtual FChildren* GetChildren() override { return &Children; }
	/** The menu's texts (every label, value, tab, tag and heading - 2026-10-26, third pass): painted on top of the flats each frame
	 *  where their 3D letters are, at the screen's own resolution (crisp on every graphics setting), with a dark-brown edge and a
	 *  soft shadow. */
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override { return FVector2D(8.f, 8.f); }

	/** A text's slot: the text block for one of the menu's flats. */
	class FSlot : public TSlotBase<FSlot>
	{
	public:
		FSlot() : TSlotBase<FSlot>() {}
	};

private:
	APinguMenu3D* GetMenu() const;
	/** Makes one text block per flat of the page that shows (again when the menu's set of flats changed). */
	void Rebuild();

	TWeakObjectPtr<APenguinHUD> HUD;
	TPanelChildren<FSlot> Children;
	TArray<TSharedPtr<STextBlock>> Blocks;
	int32 BuiltVersion = -1;
	/** Where each block goes this frame (local units: the top left, the layout scale and the residual 2D transform). */
	struct FPlace
	{
		FVector2D TopLeft = FVector2D::ZeroVector;
		FVector2D Size = FVector2D::ZeroVector;
		float Scale = 1.f;
		FMatrix2x2 Skew = FMatrix2x2();
		bool bShown = false;
	};
	TArray<FPlace> Places;
	/** The menu's texts this frame (filled while painting). */
	mutable TArray<FPinguMenu3DTextDraw> Texts;
	/** The font size per screen unit of capital height (learnt from the longer words while painting). */
	mutable float SizePerCap = 0.f;
};
