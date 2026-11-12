// Pingu - the 2D menus (2026-10-27, card 2026-10-27_gui-2d; Linus: "make it 2d agin but so it looks the exact same ... so same
// textures animationa and placements but not 3d so its wayy less performance impact").
//
// The wood menus (APinguMenu3D: every page, the lobby's strip) and the lobby island's tags, x and "+" keep all of their code -
// layout, input, focus, hit shapes, animations - but with the 2D menus on (the default; -Pingu3DMenus, [PinguMenus] b3DMenus=True
// in DefaultGame.ini or the environment variable PINGU_3D_MENUS=1 give the 3D ones back) their slabs, rings, pieces and icons are
// UPinguMenu2DPart: a transform node that remembers what it stands for (a slab of this size and wood, a rope, a nail ...) and has
// no mesh, no material, no render or physics state. SPinguMenu3DOverlay paints them every frame with PinguMenu2D::Paint, under the
// words it already drew: each part is projected through the same camera the 3D menu used (so every place, size, tilt, wiggle,
// sway, drop-in and push is the 3D one) and drawn as Slate geometry, shaded per vertex from the 3D pieces' own shapes and normals
// with the menu's two lights (the bevels, the side walls, the snow, the nails, the rope's strands), the wood grain from one
// texture (T_UI2D_Grain: white with the grain's darkening as alpha; a block of solid alpha is what the plain shapes sample, so
// everything but the icons is one Slate draw). The menu actor itself no longer follows the camera: in 2D it stands still and the
// painter and the mouse use a virtual camera with the real one's field of view (APinguMenu3D::IsFlat paths).

#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "PinguMenu2D.generated.h"

class APinguMenu3D;
class FSlateWindowElementList;
class UTexture;
struct FGeometry;

/** What a 2D part stands for. */
enum class EPinguMenu2DKind : uint8
{
	/** A rounded, bevelled slab (PinguMenu3D::FSlab): a plank, a sign, a pill, a disc, a soft shadow. */
	Slab,
	/** A tube round a rounded rectangle (PinguMenu3D::FRing): the focus rope, the selected row's gold rim, a team ring. */
	Ring,
	/** One of the wooden signs' pieces (make_ui_meshes.py) or a snow ball. */
	Piece,
	/** A picture (a portrait, an ability icon, a trait badge). */
	Icon,
};

enum class EPinguMenu2DPiece : uint8 { ArrowTip, Pointer, SnowCap, Nail, Icicle, Ball, None };

/**
 * A 2D menu part (see the file comment). A UStaticMeshComponent only so the 3D code's piece pointers (UStaticMeshComponent*) take
 * it unchanged: it never gets a mesh, a render state or a physics state.
 */
UCLASS(Transient, NotBlueprintable)
class PENGUIN_API UPinguMenu2DPart : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UPinguMenu2DPart();

	virtual bool ShouldCreateRenderState() const override { return false; }
	virtual bool ShouldCreatePhysicsState() const override { return false; }

	EPinguMenu2DKind Kind = EPinguMenu2DKind::Slab;
	EPinguMenu2DPiece Piece = EPinguMenu2DPiece::None;
	/** A slab's face (width, height, corner radius) and depth; a ring's centre line (width, height, corner radius) and its tube. */
	float Width = 0.f;
	float Height = 0.f;
	float Radius = 0.f;
	float Depth = 0.f;
	float Tube = 0.f;
	/** The colours (linear; as the 3D material's Color / Lip / Glow) and a shadow's opacity. */
	FLinearColor Face = FLinearColor::White;
	FLinearColor Lip = FLinearColor::Gray;
	float Glow = 0.f;
	float Opacity = 1.f;
	bool bShadow = false;
	bool bRope = false;
	/** Wood: how strong the grain is (0..1) and where it lies (as M_Menu3D_Wood's Grain / GrainOffset / GrainScale / Vertical). */
	bool bWood = false;
	float Grain = 1.f;
	FVector2D GrainOffset = FVector2D::ZeroVector;
	FVector2D GrainScale = FVector2D(520.f, 150.f);
	bool bVertical = false;
	/** The rendered wood (PinguWoodStyle.h, 2026-11-07): which plank picture of the atlas a wood slab is drawn with, its state and
	 *  (boards) which of the board planks. Values of PinguWood::ECut / EState. */
	uint8 WoodCut = 0;
	uint8 WoodState = 0;
	uint8 WoodVariant = 0;
	/** An icon's picture and tint. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture> Texture;
	FLinearColor Tint = FLinearColor::White;
	/** A sign's arrow point takes its plank's colours and grain (the 3D tip shared the plank's material). */
	TWeakObjectPtr<UPinguMenu2DPart> ColorFrom;
	/** Lit like the island (the lobby's tags), not by the menu's own light. */
	bool bWorldLit = false;
	/** Drawn over the whole page (the focus arrow, the snow puff). */
	bool bOnTop = false;
	/** When it was made (the painter's tie-break: later parts of the same depth go on top, as the 3D pieces sat). */
	uint32 Order = 0;
};

namespace PinguMenu2D
{
	/** Paints World's 2D parts - the lobby island's (behind), the darkening behind a page, then the menu's page by page - into
	 *  OutDrawElements from LayerId on; Geometry is the full-screen widget's. Returns the topmost layer used. */
	PENGUIN_API int32 Paint(const UWorld* World, APinguMenu3D* Menu, const FGeometry& Geometry, FSlateWindowElementList& OutDrawElements, int32 LayerId);
	/** Parts painted in the last Paint, and the Slate draws they took (tests, the perf note). */
	PENGUIN_API int32 GetPaintedParts();
	PENGUIN_API int32 GetPaintedDraws();
	PENGUIN_API int32 GetPaintedVerts();
	/** The tag of a button's Body (PinguMenu3D: the picture that drops in, tilts and grows): a plank's place on its page is taken
	 *  without it, so it stays the same through the animations. */
	inline const FName RestBodyTag(TEXT("PinguMenuBody"));
	/** A wood plank's place on its page at rest (page units: X right, Y up; the centre), as the painter sees it. */
	PENGUIN_API FVector2D GetRestPlace(const UPinguMenu2DPart& Part);
	/** The first tile (PinguWood::StartState) the painter gave a menu plank in its last paint: neighbouring planks of one class
	 *  never get the same one (card 2026-11-07_trait-logo-wood-tiling). -1: not painted as a plank. */
	PENGUIN_API int32 GetWoodStart(const UPinguMenu2DPart* Part);
	/** Tests: the viewport pixel the painter draws a world point at (the lobby's tags; the engine's own view projection). */
	PENGUIN_API bool ProjectWorldForTest(APinguMenu3D* Menu, const FVector& World, FVector2D& OutPixel);
}
