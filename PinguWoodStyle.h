// Pingu - the one wood of the whole UI: style 5 "Snowy cabin" (2026-11-07, card 2026-11-07_wood-style5-apply; Linus: "use design 5",
// "the wooden texture tiles wong at the endges of the planks ... make a design guide rule so all gui looks the same").
//
// The rules are docs/UI_STYLE.md; this file is their code. Everything that looks like wood comes from here:
//  - the atlas /Game/UI/Menu3D/T_UI2D_CabinAtlas (Tools/ArtGen/make_ui_wood_cabin.py; its spec SourceArt/UI3D/T_UI2D_CabinAtlas.json,
//    mirrored below): plank strips [left cap | 4 tiles | right cap] per class and state, the snow strip, nails, icicles,
//    knob, well, pip and a solid block;
//  - the strip rule (UI_STYLE section 2): one uniform scale from the plank's height, caps never stretched, a whole number of tiles
//    in between - no seams at the plank ends, no squeezed bands, no stretched middles (LayoutPlank); which tiles, mirrored or not
//    and how long each is comes from the plank's seed (PlankTiles), so no two planks and no stretch of one plank repeat;
//  - the snow slab, the nails and the icicles on top of a plank (LayoutSnow / LayoutNails / LayoutIcicles, UI_STYLE section 4);
//  - the palette of the words on wood and the hot look (keyboard / gamepad focus = mouse hover, no rope, no arrow);
//  - PaintPlank: the whole plank (+ snow, nails, icicles) as one Slate draw for the Slate HUD and widgets (SPinguShape's Wood,
//    the trait banner and pills, the round-win banner).
// The 2D menu painter (PinguMenu2D.cpp) lays its planks with the same Layout* functions and projects them through its camera.
// No other code draws wood its own way.

#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"

class UTexture;
class FSlateWindowElementList;
struct FGeometry;

namespace PinguWood
{
	/** What a wood surface is (the menus store it in UPinguMenu2DPart::WoodCut). Auto / Sign: a button (small or large by its
	 *  height); Board: a panel board; Pill: a small tag / value; Row: a list or settings row (quiet, no snow); Banner: the round
	 *  banners and the ability bar; Title: a title plank (large, nails, snow, icicles); Track: the slider's groove. */
	enum class ECut : uint8 { Auto, Sign, Board, Pill, Row, Banner, Title, Track };
	/** A plank's state (the menus store it in UPinguMenu2DPart::WoodState). Focused draws as Hovered (focus = hover); Pressed is
	 *  the normal picture tinted 0.86; Selected is the normal picture (the words show it); Inset is a Row. */
	enum class EState : uint8 { Normal, Hovered, Pressed, Focused, Disabled, Selected, Inset, Count };
	/** The plank classes of the atlas (UI_STYLE section 3). */
	enum class EClass : uint8 { Board, Row, ButtonS, ButtonL, Banner, Pill, Track, Snow, Count };
	/** The fixed pieces and sprites. */
	enum class ESprite : uint8 { Nail0, Nail1, Icicle0, Icicle1, Icicle2, Icicle3, Icicle4, Knot0, Knot1, Pip, Solid, Well, Knob, KnobHover, Count };

	// ---- the palette (linear; UI_STYLE section 5). Every page and the HUD use these, so no screen has its own tone.
	/** Words on wood: #FFF4E2. */
	inline const FLinearColor Text(1.f, 0.905f, 0.761f);
	/** Disabled / secondary words: #B9B2A8. */
	inline const FLinearColor TextDim(0.485f, 0.445f, 0.392f);
	/** The letters' outline and dark lines: #2A1406. */
	inline const FLinearColor Ink(0.0232f, 0.0070f, 0.0018f);
	/** The main action and banner words, the "+": #FFD23F. */
	inline const FLinearColor Accent(1.f, 0.644f, 0.0497f);
	/** Values, selected options, the slider fill, the on pip: #A8EBFF (icy). */
	inline const FLinearColor Value(0.392f, 0.831f, 1.f);
	/** LEAVE / CLOSE GAME, the kick X, warnings: #FF6A55. */
	inline const FLinearColor Danger(1.f, 0.144f, 0.0908f);
	/** The 3D menus' wood colour (M_Menu3D_Wood's Color): the cabin mid #98572B. */
	inline const FLinearColor Mid3D(0.31f, 0.095f, 0.024f);

	/** A colour (a team, FULL, YOU) as words on the wood: light enough for the dark outline round them. */
	inline FLinearColor WordsOf(const FLinearColor& Color)
	{
		FLinearColor Out = FMath::Lerp(Color, FLinearColor::White, 0.25f);
		for (int32 Step = 0; Step < 4 && Out.GetLuminance() < 0.4f; ++Step)
		{
			Out = FMath::Lerp(Out, FLinearColor::White, 0.3f);
		}
		Out.A = 1.f;
		return Out;
	}

	/** A Slate font for words straight on the wood: the ink outline, 0.085 of the size (UI_STYLE section 5). */
	inline FSlateFontInfo OnWoodFont(FSlateFontInfo Font)
	{
		Font.OutlineSettings.OutlineSize = FMath::Max(Font.OutlineSettings.OutlineSize, FMath::Max(2, FMath::RoundToInt(0.085f * Font.Size)));
		Font.OutlineSettings.OutlineColor = Ink;
		return Font;
	}

	/** The hot button's look (one place, so another look is one edit). Linus 2026-11-07: the keyboard / controller focus looks
	 *  exactly like the mouse hover - no rope round it, no arrow beside it. */
	struct FHotLook
	{
		bool bFocusLikeHover = true;
		bool bRope = false;
		bool bArrow = false;
		EState HotState = EState::Hovered;
	};
	PENGUIN_API const FHotLook& HotLook();

	/** The atlas (null until imported: the painter then falls back to its old shaded wood). Loaded once, kept for the run. */
	PENGUIN_API UTexture* GetAtlas();
	/** The atlas's width in texels. Every U and V a layout gives out is in "UV texels": U / AtlasSize and V / AtlasSize are the
	 *  texture coordinates (V is the true texel row * AtlasSize / AtlasHeight: the atlas is 2048 x 4096). */
	constexpr float AtlasSize = 2048.f;
	constexpr float AtlasHeight = 4096.f;
	/** Different middle tiles per plank strip (card 2026-11-07_trait-logo-wood-tiling: was 2). */
	constexpr int32 AtlasTiles = 4;
	/** Atlas texels per design px (1 design px = 1 screen px at 1080p; the menus' page units and the HUD's Slate units). */
	constexpr float TexelsPerDesignPx = 2.f;

	/** A plank strip of the atlas (texels): its rect (pads included), pads, cap, tile and the body's height. Snow: TopLine is the
	 *  row that sits on the plank's top edge. */
	struct FStrip
	{
		float X = 0.f, Y = 0.f, W = 0.f, H = 0.f;
		float PadSide = 44.f, PadTop = 32.f, PadBottom = 56.f;
		float Cap = 64.f, Tile = 384.f;
		int32 Tiles = 4;
		float BodyH = 208.f;
		float TopLine = 0.f;
	};
	/** The strip of a class in a state (hover / disabled where the atlas has them, else the normal one). */
	PENGUIN_API FStrip Strip(EClass Class, EState State);
	/** A sprite's rect in atlas texels. */
	struct FRect
	{
		float X = 0.f, Y = 0.f, W = 0.f, H = 0.f;
	};
	PENGUIN_API FRect Sprite(ESprite Piece);
	/** The solid white block (plain fills sample its centre). */
	PENGUIN_API FRect Solid();
	/** A class's body height in design px (board 104, row 86, button_s 94, button_l 116, banner 146, pill 60, track 20). */
	PENGUIN_API float BodyDesign(EClass Class);
	/** The class a cut is drawn with at a body height of HeightDesign design px. */
	PENGUIN_API EClass ClassOf(ECut Cut, EState State, float HeightDesign);
	/** The picture state a state is drawn with (Focused -> Hovered, Pressed / Selected / Inset -> Normal). */
	PENGUIN_API EState PictureOf(EState State);
	/** The tint of a state (Pressed: 0.86 grey, else white). */
	PENGUIN_API float ShadeOf(EState State);
	/** Gets snow / nails / icicles (UI_STYLE section 4). */
	PENGUIN_API bool HasSnow(ECut Cut);
	PENGUIN_API bool HasNails(ECut Cut);
	PENGUIN_API bool HasIcicles(ECut Cut);

	/** One quad of a layout: its rect in plank space (X right, Y down, the body's top left at 0, 0, in the caller's units) and its
	 *  atlas rect in UV texels (see AtlasSize; U0 > U1 = drawn mirrored). */
	struct FQuad
	{
		float X0 = 0.f, Y0 = 0.f, X1 = 0.f, Y1 = 0.f;
		float U0 = 0.f, V0 = 0.f, U1 = 0.f, V1 = 0.f;
	};
	/** One middle tile of a plank: which of the strip's tiles, mirrored or not, and its length (in the caller's units). */
	struct FTilePick
	{
		int32 Variant = 0;
		bool bMirrored = false;
		float Length = 0.f;
	};
	/** The tiles of a plank's middle (UI_STYLE section 2): n = round(Middle / TileLength) tiles; the first any of Tiles, each next
	 *  one another than the one before; mirrored by a coin (bMirrorOk); lengths jittered 0.85..1.15 and fitted to Middle. All from
	 *  a hash of Seed: stable from frame to frame, different from plank to plank. StartState 0 .. 2 * Tiles - 1 fixes the first
	 *  tile (variant = StartState % Tiles, mirrored = StartState >= Tiles); -1: from the hash too. Replaces Out. */
	PENGUIN_API void PlankTiles(uint32 Seed, float Middle, float TileLength, int32 Tiles, bool bMirrorOk, TArray<FTilePick>& Out,
		int32 StartState = -1);
	/** The strip rule: a plank of body W x H (pads around it), its middle's tiles from PlankTiles(hash of Seed, board). Floats that
	 *  follow on exactly: no gaps, no overlaps. A plank taller than 1.25x its class (in Units per design px) is stacked from
	 *  horizontal boards (never a rotated picture). Appends. */
	PENGUIN_API void LayoutPlank(EClass Class, EState State, float W, float H, uint32 Seed, float UnitsPerDesignPx, TArray<FQuad>& Out,
		bool bOneBoard = false, int32 StartState = -1);
	/** A plank's first tile is one of StartStates (4 tiles x mirrored or not): StartState for LayoutPlank / PlankTiles. */
	constexpr int32 StartStates = 2 * AtlasTiles;
	/** The start a plank's seed would pick by itself (where nothing has to differ from a neighbour). */
	PENGUIN_API int32 PreferredStart(uint32 Seed);
	/** Two planks of one class are neighbours (their wood would be seen side by side): stacked - the next row, under 1.6 heights
	 *  apart and side by side over half the shorter one - or in one row with their ends under 40 units apart. Centres in page
	 *  units, Y up or down (only the distance counts). The painter gives neighbours different starts; look_WoodTiling checks it. */
	PENGUIN_API bool AreNeighbours(const FVector2D& CentreA, float WA, float HA, const FVector2D& CentreB, float WB, float HB);
	/** How many boards a plank of height H is stacked from (1 up to 1.25x its class's height). */
	PENGUIN_API int32 StackCount(EClass Class, float H, float UnitsPerDesignPx);
	/** The snow slab on a plank of body W x H (UI_STYLE section 4). */
	PENGUIN_API void LayoutSnow(float W, float H, uint32 Seed, float UnitsPerDesignPx, TArray<FQuad>& Out);
	/** The nails on a board / title / banner of body W x H. */
	PENGUIN_API void LayoutNails(float W, float H, float UnitsPerDesignPx, TArray<FQuad>& Out);
	/** 3 to 5 icicles under a title / banner of body W x H. */
	PENGUIN_API void LayoutIcicles(float W, float H, uint32 Seed, float UnitsPerDesignPx, TArray<FQuad>& Out);
	/** A sprite centred at (CX, CY) (Top: hung from its top centre) at Scale units per texel. */
	PENGUIN_API void LayoutSprite(ESprite Piece, float CX, float CY, float Scale, bool bTop, TArray<FQuad>& Out);

	/** How a Slate plank looks. */
	struct FPlankLook
	{
		ECut Cut = ECut::Sign;
		EState State = EState::Normal;
		/** -1: as the cut (HasSnow / HasNails / HasIcicles); 0 / 1: forced. */
		int8 Snow = -1;
		int8 Nails = -1;
		int8 Icicles = -1;
		uint32 Seed = 1;
		float Opacity = 1.f;
		/** Never stacked: one board however tall (words that must not cross a seam: the trait banner's wrapped lines). */
		bool bOneBoard = false;
	};
	/** Paints a plank whose body is Size (Slate units = design px) with its top left at TopLeft in Geometry's local space, drawn
	 *  Scale times as large (pads, snow and icicles reach out of it), as one Slate draw. False without the atlas. */
	PENGUIN_API bool PaintPlank(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FVector2f& TopLeft, const FVector2f& Size,
		const FPlankLook& Look, float Scale = 1.f);
	/** Paints a list of quads in the atlas: a quad point P lands on Geometry's local Offset + P * Scale. */
	PENGUIN_API bool PaintQuads(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FVector2f& Offset, const TArray<FQuad>& Quads,
		const FLinearColor& Tint, float Scale = 1.f);
}
