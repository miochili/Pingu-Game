// Pingu - automated movement check (run the game with -PinguAutoTest).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PenguinTypes.h"
#include "PenguinAutoTest.generated.h"

class APenguinCharacter;
class APenguinProjectile;
class APinguPickup;
class AIcebergDrop;
class APinguPortal;
class UPenguinMovementComponent;
class UMaterialInterface;
class SWidget;
struct FPinguTraitTestState;
struct FPinguIslandTestState;

/**
 * Drives the first penguin through a scripted set of moves, logs the measured numbers
 * (acceleration time, jump heights, dash distance, ability hits, pickups, ...) with the prefix [AUTOTEST],
 * takes screenshots into Saved/ClaudeLogs/Shots and quits. Spawned by the game mode only when
 * the command line contains -PinguAutoTest, so it never runs in normal play.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APenguinAutoTest : public AActor
{
	GENERATED_BODY()

public:
	APenguinAutoTest();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	struct FStep
	{
		FString Name;
		float Duration = 1.f;
		TFunction<void()> Start;
		TFunction<void(float)> Update;
		TFunction<void()> End;
	};

	void BuildScript();
	void AddStep(const TCHAR* Name, float Duration, TFunction<void()> Start, TFunction<void(float)> Update, TFunction<void()> End);
	void Teleport(const FVector& Location);
	/** A screenshot in Saved/ClaudeLogs/Shots (bWithUI: with the Slate UI - the icons above the heads, the HUD). */
	void Shot(const TCHAR* Name, bool bWithUI = false);
	void Report(const FString& Message) const;
	/** Logs every static mesh component of a penguin (mesh, material, visibility) - status visuals check. */
	void DumpStatusVisuals(const APenguinCharacter* Who, const TCHAR* Label) const;
	void ResetAir();
	void TrackAir(float StepTime);
	void Finish();
	FVector Location() const;

	void AddAbilityStep(EPenguinAbility Ability, float TargetOffset, float Duration);
	/** Every ability is also a movement tool: use it with an aim relative to the penguin and measure how it moved. */
	void AddMoveStep(EPenguinAbility Ability, const FVector& AimOffset, float StartHeight, float FireTime, float Duration);
	void AddPegStep();
	void AddFloeStep();
	/** Jump + double jump next to a wall that is one block too tall, then jump again: the ledge grab must pull us up. */
	void AddClimbStep();
	/** Sudden death on the test strip (fast): the blocks must go from the outside in while the middle stays. */
	void AddCollapseStep();
	/** Everything interacts with everything: bomb chain reactions and frozen bombs. */
	void AddInteractionSteps();
	/** The brawl rules: the belly slide (distance, hit, bump, bomb kick), ice feet, size ray keys (hold on yourself, tap to
	 *  shoot), the noot's shotgun falloff, the long fish hook (over-head throw, grabbing a bubble), lasting ice walls broken
	 *  block by block, and iceberg ice that falls and stacks like sand. */
	void AddBrawlSteps();
	/** A brawl step's start: fresh strip, the penguin at the strip's base facing right with these abilities (none: keep),
	 *  the dummy TargetOffset ahead, the per-step counters cleared. */
	void PrepareBrawl(float TargetOffset, const TArray<EPenguinAbility>& Abilities);
	/** Ice blocks on the test strip in the columns from FromX over Width cm, in the Rows rows above its floor (OutTemporary:
	 *  how many of them go away by themselves). */
	int32 CountIce(float FromX, float Width, int32 Rows, int32* OutTemporary) const;
	/** Every ability with every other: the interactions added for the full matrix (push clash, walls, shells, the Snow Warp
	 *  portal, frost on icicles / pads / icebergs, ice feet slides), each a pass / fail check, and a summary line. */
	void AddMatrixSteps();
	/** Close-ups of the ice gun's ice chunks on our penguin's feet (from the side, the front, walking): screenshots
	 *  through the stage camera held close, and where the chunks sit against the feet. */
	void AddIceFeetLookStep();
	/** Snow Warp's portal in pictures: both ends, then one up close (screenshots through the stage camera). */
	void AddPortalLookStep();
	/** The snow bomb's blast zone in pictures (every effects setting looks the same): early and late in the fuse, the
	 *  explosion's zone flash. With bUltra the Ultra (Niagara) layers play too (forced on for the step), and after the
	 *  blast a burst, glints, a ring and a splash are set off in front of the camera. */
	void AddEffectsLookStep(bool bUltra);
	/** The background from the game camera: the sea twice half a second apart (it must move), the horizon, then a short
	 *  film of the sea from the game camera (Saved/ClaudeLogs/Shots/Film: 60 frames, 1/20 s of game time apart). */
	void AddBackgroundLookStep();
	/** The background on High and on Low (PenguinAutoTestBackdrop.cpp): look_BackgroundPresets, the game view, the horizon,
	 *  the sky and the nearest iceberg on each preset (45_bg_high_* / 45_bg_low_*), the bergs' LODs and the background's
	 *  triangles from the game camera. */
	void AddBackdropPresetStep();
	/** AddBackgroundLookStep: what the camera looks at (above the main island's middle). */
	FVector BackgroundFocus = FVector::ZeroVector;
	/** AddBackgroundLookStep's film runs on a fixed time step: whether we turned it on, and the engine's setting before. */
	bool bFilmFixedStep = false;
	bool bFilmWasFixed = false;
	double FilmWasDelta = 1.0 / 30.0;
	/** Puts the engine's time step back after the film (nothing when the film did not change it). */
	void EndFilm();
	/** The ability icons above the head while they recharge (the dark clock): screenshots with the UI. */
	void AddCooldownLookStep();
	/** The noot's horn (the beak stretched into a wide trumpet) up close: both noots, then one aimed up behind. */
	void AddNootHornLookStep();
	/** The ice shell's bubble (M_ShieldBubble) up close round our penguin: a shot on High and one on Low. */
	void AddShellBubbleLookStep();
	/** No ink outline (PenguinAutoTestOutline.cpp, look_NoOutline): our penguin up close on High (the horn out too) and Low
	 *  (63_no_outline_*); every LOD's outline section and the horn's outline hidden, a penguin spawned later, a trait's
	 *  clipped outline material still on the slot. */
	void AddNoOutlineStep();
	/** The 3D menus (PenguinAutoTestMenu3D.cpp, look_Menu3D): the 3D title's hover outline, the keys through its five buttons, a
	 *  click (push-in, then the action), the lobby's controls; 73_menu3d_* at 1280x720 and 1920x1080. */
	void AddMenu3DLookStep();
	/** Jester's felt cap (PenguinAutoTestJester.cpp, look_JesterHat): on High (static), Ultra (spring bones: the bells must
	 *  swing in a run, a jump, the landing) and Low; sitting on the head, the beanie hidden, the penguin's LOD, the outline (always hidden)
	 *  option (67_jester_hat_*). */
	void AddJesterHatStep();
	/** The icicle's charged throw (its key held: faster and straighter; a tap: a weak lob): close-ups of the pull-out, half a
	 *  second in, the cocked arm at full charge (shaking, glowing) and the release (the icicle in the flipper while charging,
	 *  gone after the throw, the timings; the shake, glow, cold glow and sparkles at 0.2 / 0.5 / 1.0 s); a full charge on the
	 *  Low preset (look_IcicleChargeLow); then a tap, a medium hold and a full charge thrown level along the strip (their
	 *  speed, drop and how far each one gets), the cooldown starting with the throw, and a stun while charging that throws
	 *  nothing. */
	void AddIcicleThrowSteps();
	/** The penguin faces its aim while it charges and throws the icicle (PenguinAutoTestIcicleFacing.cpp, look_IcicleFacing): running
	 *  right while aiming left it still faces left through the charge and the throw (the icicle leaves from the front, the near
	 *  flipper throws), then turns back; the aim crossing over turns it round at once, a vertical aim keeps its side
	 *  (55_icicle_facing_*). */
	void AddIcicleFacingStep();
	/** The snow bomb knocks out and pushes its own thrower too (PenguinAutoTestSelfBomb.cpp): selfbomb_<case> (inside the kill
	 *  radius we are out, credited to nobody even with the dummy as the last attacker; in the push radius pushed exactly like the
	 *  dummy; outside nothing; a grown bomb; the shell holds it; a batted-back bomb is the dummy's), selfbomb_FeetDrop (the ability
	 *  aimed at our feet), look_SelfBomb (68_self_bomb_*), bot_BombThrow (the bots' check of where a throw lands) and
	 *  bot_BombFlee* (a bot runs out of a live bomb's blast, through it at the island's end, or puts its shell up). */
	void AddSelfBombSteps();
	/** The HUD cleanup (PenguinAutoTestHudCleanup.cpp): look_HudTraitCorner (the trait's badge alone in the top-left corner). */
	void AddHudCleanupSteps();
	/** The GUI wood's tiling (PenguinAutoTestWood.cpp): look_WoodTiling (neighbouring planks never show the same grain in line). */
	void AddWoodTilingStep();
	/** The giant snowball (PenguinAutoTestGiantSnowball.cpp): giant_*, look_GiantBuild, bot_Giant*. */
	void AddGiantSnowballSteps();
	/** The snowman (PenguinAutoTestSnowman.cpp): snowman_*, look_Snowman*. */
	void AddSnowmanSteps();
	/** The ability tuning of 2026-11-05 (PenguinAutoTestAbilityTuning.cpp): tuning_SnowmanCooldown, icicle_Melt, _LateRelease, _BotHold. */
	void AddAbilityTuningSteps();
	/** The LAN anti-cheat (PenguinAutoTestAntiCheat.cpp): anticheat_FairSeed, _HostGates, _HostRules, _Judge, _FileCheck. */
	void AddAntiCheatSteps();
	/** The ability picks' deal and its anti-cheat (PenguinAutoTestAbilityPicks.cpp): anticheat_PicksDeal, _PicksReveal, _PicksJudge. */
	void AddAbilityPicksSteps();
	/** The host's say over a LAN joiner's own penguin (PenguinAutoTestHostAuthority.cpp): hostauth_Limits, _LegalMoves, _Cheats. */
	void AddHostAuthoritySteps();
	/** Flap, the 19th ability (PenguinAutoTestFlap.cpp): flap_Ground, _Fall, _Cooldown, _Blocked, _Ends, _Scale, _HostJudge, look_Flap. */
	void AddFlapSteps();
	/** Ice feet = a slide (PenguinAutoTestIceFeet.cpp, 2026-11-05): icefeet_NoWalk, _Slide, _Edge, _Jump, _Knockback, _Ends, _Bot, _HostJudge, look_IceFeetSlide. */
	void AddIceFeetSteps();
	/** The kill cam (PenguinAutoTestKillCam.cpp, 2026-11-07): killcam_Curve, _Unescapable, _Escapable, _HookInReach, _HookOutOfReach, _Icicle, _NotFinal. */
	void AddKillCamSteps();
	/** The build limit (PenguinAutoTestBuildLimit.cpp, 2026-11-05): buildlimit_Place, look_BuildLimitPuff, buildlimit_Bots. They
	 *  turn the main island into an arena: last in the script. */
	void AddBuildLimitSteps();
	/** The ice wall is solid the moment it is cast, and a grown penguin's ice is bigger (PenguinAutoTestIceScale.cpp): ice_WallInstant
	 *  (the cast's frame: blocks, collision, a snowball in it broken, the dummy lifted out; the look rising: 71_icewall_instant),
	 *  ice_WallInstantSide, ice_WallSize1/2/4 and ice_IcebergSize1/2/4 (71_icewall_size*, 71_iceberg_size*), ice_BigIcebergOverhang,
	 *  ice_BigBlockSand, ice_BigBlockBreak, ice_BigBlockTornado (big blocks: 2x2 cells drawn as one). */
	void AddIceScaleSteps();
	/** The island's blocks keep their look when blocks go (PenguinAutoTestIslandLooks.cpp): island_StableLooks (sudden death, a block
	 *  broken out, a sand block landing: no block's texture / tint changes; 72_island_stable_before / after). */
	void AddIslandLooksSteps();
	/** The arena's ends step down to the sea (PenguinAutoTestMapEdges.cpp, 2026-11-05): mapedges_Stats (50 seeds, old vs new share of
	 *  stepping ends), look_MapEdges (97_mapedges_*: four seeds before / after). */
	void AddMapEdgeSteps();
	/** The round hazards (PenguinAutoTestHazards.cpp, 2026-11-07): hazard_Setup, _RisingSea, _Crumble, _CrumbleMin, _CrumbleBot, _Wind,
	 *  _WindAgainst, _Stop, _Rain, look_HazardWind (98_hazard_*). They make the main island an arena: after the build limit's. */
	void AddRoundHazardSteps();
	/** AddIcicleThrowSteps: following one thrown icicle (from where to how far it got), and per throw (tap, medium, full)
	 *  its distance, charge and the time from the press to the throw; the cooldowns seen while charging / after the throw. */
	void WatchIcicle(int32 Index, float StepTime);
	TWeakObjectPtr<APenguinProjectile> WatchedIcicle;
	int32 IciclesAtPress = 0;
	float IcicleFromX = 0.f;
	float IcicleFarX = 0.f;
	float IcicleDistance[3] = { 0.f, 0.f, 0.f };
	float IcicleCharge[3] = { -1.f, -1.f, -1.f };
	float IcicleDelay[3] = { -1.f, -1.f, -1.f };
	/** Per throw: its speed and gravity as it left, and how far below its starting height it was after 10 m of flight
	 *  (IcicleStartZ: where it began; IcicleDrop10m < 0 until it got that far). */
	float IcicleSpeedAtThrow[3] = { -1.f, -1.f, -1.f };
	float IcicleGravityAtThrow[3] = { -1.f, -1.f, -1.f };
	float IcicleDrop10m[3] = { -999.f, -999.f, -999.f };
	float IcicleStartZ = 0.f;
	float IcicleCooldownCharging = -1.f;
	float IcicleCooldownThrown = -1.f;
	/** The slap stuns for a second (dizzy) and knocks only a little, no stun-lock; a frozen penguin's ice takes the first
	 *  hit that smashes ice (a slap, a deadly icicle; not a snowball: it only bursts on it) and wind blows the ice cube away
	 *  (still frozen). */
	void AddStunAndIceShieldSteps();
	/** The snowball combo and its bounce off a frozen penguin's ice (PenguinAutoTestSnowball.cpp): snowball_Combo (nine hits in a
	 *  row: the push grows +35 % a hit up to x3), snowball_ComboWindow (the chain starts over after its window; a miss breaks it),
	 *  snowball_IceCube (a snowball leaves a frozen dummy's ice alone; a slap still shatters it) and look_SnowballCombo (the
	 *  pictures, 69_snowball_*). */
	void AddSnowballSteps();
	void AddSnowballLookStep();
	/** Our own penguin slapped: for the second of the stun the stick, a jump, a dash and an ability do nothing; then all work. */
	void AddStunControlStep();
	/** Abilities in the middle of things: in a knockback flight (the hit stun on), in a dash and in a belly slide they work
	 *  (and the dash / slide end normally); frozen or dizzy from a slap they don't (and work again once that is over). */
	void AddAbilityWhileBusyStep();
	/** Every kind of hit on a frozen penguin: smashes ice (taken by the ice), wind / belly slide (the ice cube slides), ice bolt
	 *  (left alone), hook (breaks, throws), iceberg (crushes), and a second hit after the ice took the first (lands). */
	void AddIceShieldMatrixStep();
	/** Close-ups: the dizzy stars after a slap, a frozen penguin's ice shattering under a snowball. */
	void AddStunLookStep();
	/** The fish hook and ice: a frozen dummy is yanked in its ice cube about halfway (falling like a block, still frozen in
	 *  the air) and the ice shatters where it lands; an ice block of the island is pulled out whole, comes about halfway and
	 *  is a block again (not broken); a frozen penguin rises and falls like a block (gravity, top falling speed). */
	void AddHookIceSteps();
	/** Pictures: the yanked ice cube in mid-air, the pulled-out ice block in mid-air and where it landed. */
	void AddHookIceLookStep();
	/** Team matches have friendly fire: a teammate's snowball, noot, gust and slide land like anybody's, our iceberg and our
	 *  falling sand crush a teammate (we are spared under our own); a hard bot dodges its teammate's snowballs (never its
	 *  own); a shot at a penguin of the other team hits. */
	void AddTeamSteps();
	/** Last (it turns the course into an arena): the island-first camera, everybody's ability icons (others smaller) and
	 *  the marker at the screen's edge for a penguin flung out of the picture - screenshots with the UI. */
	void AddTeamLookStep();
	/** The lobby island's look (PenguinAutoTestLobbyIsland.cpp; last, it ends in practice again): look_LobbyIsland - five penguins
	 *  idling and strolling on the lobby island (74_lobby_overview), a bot puffing in (74_lobby_puff_in), a click on a bot: the
	 *  hop-spin and its beanie in the team's colour (74_lobby_team_hop, 74_lobby_team), the tags close up (74_lobby_tags), a bot
	 *  puffing away (74_lobby_puff_out), a last overview (74_lobby_idles). */
	void AddLobbyIslandLookStep();
	/** The mouse on the 3D title and the lobby island (PenguinAutoTestClicks.cpp, clicks_Menu3D): real Slate clicks at nine points
	 *  over every button, tag, x, penguin and the "+" at 1280x720 and 1920x1080 (all must hit), the viewport mapping with a PIE-style
	 *  offset and DPI, no walking on the island. */
	void AddMenuClickStep();
	/** The 3D menu's other pages (PenguinAutoTestScreens3D.cpp, clicks_Menu3DScreens): every button of pause / LAN / results / the
	 *  settings pages / controls / PINGUPEDIA at five points at two sizes, the LAN address field with real keys (typed, Ctrl+V,
	 *  Backspace, Enter), a key cap's capture with a real key, the frame time with the results page up. */
	void AddMenuScreensClickStep();
	/** Pictures of every 3D page at 1920x1080 (look_Menu3DScreens): 75_menu3d_*_1080. */
	void AddMenu3DScreensLookStep();
	/** The belly slide through a gap one block high: walking is stopped by it, a slide goes through (a short gap, one
	 *  longer than a slide - it crawls on - and a dead end it backs out of), standing up behind it. */
	void AddSlideGapSteps();
	/** A tunnel one block high on the flat strip: Length blocks of ceiling (two rows) from OffsetX right of Base, one block
	 *  above the floor; bDeadEnd closes its far end. Sets TunnelEntryX / TunnelExitX. */
	void BuildTunnel(const FVector& Base, float OffsetX, int32 Length, bool bDeadEnd);
	float TunnelEntryX = 0.f;
	float TunnelExitX = 0.f;
	/** The bots find their way: through a gap one block high on the belly (a short one, a long one; a dead end they jump
	 *  over; a grown bot that does not fit never tries), and portals - over one that would take them away from their
	 *  target, through one that takes them to it, never into one whose far end is over the sea. The dummy plays the bot,
	 *  our penguin stands still as its target. */
	void AddBotNavSteps();
	/** The bots don't stand around and don't loop through portals (PenguinAutoTestBots.cpp, 2026-11-07): bot_IdleEngage*,
	 *  bot_PortalLoopFall, _LoopNear, _LoopShort*, _ShortcutWall, bot_PortalFuzz00..35. */
	void AddBotAggroSteps();
	/** AddBotNavSteps: the dummy's abilities before (put back after), its jump / slide counters when its brain went on. */
	TArray<EPenguinAbility> SavedTargetAbilities;
	int32 BotJumpsAtStart = 0;
	int32 BotSlidesAtStart = 0;
	/** The cartoon deaths, one after another on the dummy (stomp, iceberg, icicle, snow bomb, the sea): close-ups, how flat
	 *  / frozen / sooty / gone it is, and everything back to normal after the round reset. */
	void AddDeathLookStep();
	/** AddDeathLookStep: per knock-out kind what was seen (flat, ice, soot, active with its reason ...). */
	float DeathStompFlat = 1.f;
	float DeathIcebergFlat = 1.f;
	float DeathStompRise = 0.f;
	float DeathStompDrift = 0.f;
	float DeathIcebergRise = 0.f;
	float DeathIcebergSide = 0.f;
	float DeathKOX = 0.f;
	int32 DeathIceEarly = 0;
	int32 DeathIceLate = 0;
	int32 DeathSoot = 0;
	int32 DeathSeaHidden = 0;
	int32 DeathActive = 0;
	int32 DeathPhaseDone = -1;
	int32 DeathSampleMask = 0;
	TWeakObjectPtr<UMaterialInterface> OverlayAtStart;
	/** The falling iceberg against penguins: one under it is carried down and crushed against the floor, one at its side is
	 *  shoved aside (not hurt); falling sand ice crushes the dummy under it and shoves our penguin beside it. */
	void AddIcebergPressSteps();
	/** The penguin's jump counter when a step began (no jumps under a low ceiling). */
	int32 JumpsAtStart = 0;
	/** Our close-up camera follows our penguin's feet from this offset (cm), looking at them. */
	void PlaceLookCamera(const FVector& Offset);
	/** Open portals of PortalOwner (OutFirst: one of them). */
	int32 CountPortals(const APenguinCharacter* PortalOwner, APinguPortal** OutFirst) const;
	/** One RESULT line of the interaction checks, counted for the summary. */
	void MatrixCheck(const TCHAR* Label, bool bOk, const FString& Detail);
	/** The player's penguin spawned turned (Play In Editor spawns at the camera): it must still face where it runs. */
	void AddFacingStep();
	/** Ability bubbles: taking one (middle slot), recharging an owned ability, falling when the ground breaks,
	 *  pushed by a shot, dropped from the sky and by a knocked-out penguin. */
	void AddPickupSteps();
	/** Small penguins fly further, big ones less (the same snowball); the size limits (shrunk past 0.5x: gone; grown past 8x:
	 *  it pops, the blast pushes); knockback by relative size (attacker / victim). */
	void AddSizeKnockbackSteps();
	/** Jump presses that used to get lost or come out wrong: a tap inside one frame, a press during a hit stun, a late press
	 *  after walking down a block, a press just before landing with the double jump used, a press right after a small push. */
	void AddJumpForgivenessSteps(const FVector& FlatStart);
	/** The facing direction of the penguin's mesh in the world (the mesh looks along its +Y). */
	FVector MeshForward(const APenguinCharacter* Who) const;
	void SpawnTarget();
	void ResetTarget(const FVector& Where);
	void ClearAbilityLeftovers();

	TWeakObjectPtr<APenguinCharacter> Penguin;
	TWeakObjectPtr<APenguinCharacter> Target;
	TWeakObjectPtr<UPenguinMovementComponent> Move;
	TArray<FStep> Steps;
	int32 StepIndex = -1;
	float StepTime = 0.f;
	FVector2D VirtualInput = FVector2D::ZeroVector;
	bool bFinished = false;

	// scratch metrics shared by the steps
	float StartZ = 0.f;
	float PeakZ = 0.f;
	float PeakTime = -1.f;
	float LandTime = -1.f;
	bool bLeftGround = false;
	float MarkA = -1.f;
	float MarkB = -1.f;
	float Value = 0.f;
	float StartX = 0.f;
	bool bFlag = false;
	bool bShotDone = false;
	int32 Frames = 0;
	TWeakObjectPtr<APenguinProjectile> TestBombA;
	TWeakObjectPtr<APenguinProjectile> TestBombB;
	/** Jump presses while walking: when each came, whether a jump had to come out, the jump counters then. */
	TArray<float> PressTimes;
	TArray<bool> PressShouldJump;
	TArray<int32> PressCounters;
	bool bJumpKeyDown = false;
	/** The penguin's height standing on the flat strip, and scratch counters for the jump steps. */
	float StandZ = 0.f;
	int32 CountA = 0;
	int32 CountB = 0;
	float TimeA = -1.f;
	TWeakObjectPtr<APinguPickup> TestPickup;
	TWeakObjectPtr<APinguPickup> SeaDrop;
	FVector PickupStart = FVector::ZeroVector;
	float PickupLowZ = 0.f;
	float PickupHighZ = 0.f;
	float MoveLowZ = 0.f;     // lowest point after using a movement ability
	float MoveBounceZ = 0.f;  // highest point reached after that lowest point (stomp bounce)
	int32 TargetHitsAtStart = 0;
	/** Ability steps: the caster's cooldown when its shot first hit the target (-1: no hit; the snowball combo: 0). */
	float CooldownAtHit = -1.f;
	int32 ClimbsAtStart = 0;
	/** IciclePad: jump pad bounces when the penguin was dropped onto the pad. */
	int32 PadBouncesAtStart = 0;
	int32 BrokenAtStart = 0; // the dummy's hit counter when an ability step started (hits are counted per step)
	FVector PegTopSeen = FVector::ZeroVector;
	/** Brawl steps: the dummy's velocity when it was hit, belly slides when the step began, a second timer, a key released,
	 *  the dummy's highest point, the sand column (x, the row right above the floor). */
	FVector TargetVelAtHit = FVector::ZeroVector;
	int32 SlidesAtStart = 0;
	float TimeB = -1.f;
	bool bKeyDone = false;
	float HighZ = 0.f;
	FIntPoint SandColumn = FIntPoint::ZeroValue;
	/** Matrix steps: our own hit counter when the step began, the iceberg under test, the checks run / passed. */
	int32 OwnHitsAtStart = 0;
	TWeakObjectPtr<AIcebergDrop> TestIceberg;
	int32 MatrixChecks = 0;
	int32 MatrixPassed = 0;
	/** Matrix steps, the portal: its ends (where we put them / where we stood), the portal under test, where we arrived. */
	FVector PortalEndA = FVector::ZeroVector;
	FVector PortalEndB = FVector::ZeroVector;
	TWeakObjectPtr<APinguPortal> TestPortal;
	float ArriveX = 0.f;
	/** AddIceFeetLookStep: the stage camera while we hold it for the close-ups. */
	TWeakObjectPtr<AActor> LookCamera;

	// ---- Traits mode (PenguinAutoTestTraits.cpp)
	/** The traits: the deal (a bag: never two the same in a round, all 17 within two rounds of ten; Second Chance's two
	 *  abilities), every trait's numbers measured on our penguin or the dummy (trait_<Name>, ok / FAIL), a bubble picked up
	 *  with Second Chance; then their looks (AddTraitLookSteps). */
	void AddTraitSteps();
	/** The traits made for everybody having the round's one trait (2026-11-07; PenguinAutoTestSharedTraits.cpp). */
	void AddSharedTraitSteps();
	/** Pictures: every trait's look (60_trait_<name>), all of them in a line (60_trait_lineup), the reveal's cards, their
	 *  flight and the changed penguins (61_trait_reveal_*), the looks on Low and Ultra (62_trait_<preset>). */
	void AddTraitLookSteps();
	/** The traits steps' own state (defined in PenguinAutoTestTraits.cpp). */
	TSharedPtr<FPinguTraitTestState> TraitTest;

	// ---- The island's size (PenguinAutoTestIsland.cpp)
	/** After everything: the arena for 2, 4, 6 and 10 penguins (width, starts on the ground, sky drops, the camera;
	 *  58_island_size_<n>), the camera's furthest view, following on a ten-penguin island (58_island_follow_a/b), Snow Warp
	 *  kept on the screen - also a simulated 21:9 one (58_warp_in_view). */
	void AddIslandSizeSteps();
	TSharedPtr<FPinguIslandTestState> IslandTest;

	// ---- The tornado (the Gust slot; PenguinAutoTestTornado.cpp)
	/** ability_Tornado (two dummies at 2 and 5 m: sucked in, then thrown - the near one harder), tornado_Hover (still in
	 *  the air, no other ability meanwhile), tornado_Ice (the island's ice ripped out, swirled, thrown; one block comes down
	 *  on the dummy: crushed only once it reaches it), tornado_Snowball (sucked in, circles, thrown out as ours), tornado_Merge
	 *  (ours and the dummy's 8 m apart merge into one giant: reach, push, times, both users in its eye, a dummy 10.5 m out thrown,
	 *  the ice of both); screenshots 65_tornado_*. */
	void AddTornadoSteps();
	/** look_Tornado: the funnel up close on High and on Low (65_tornado_look_high / _low); look_TornadoMerge: two merging up
	 *  close (65_tornado_merge_look_*). */
	void AddTornadoLookStep();
	/** A second dummy for the tornado's steps (spawned on demand, destroyed at their ends). */
	TWeakObjectPtr<APenguinCharacter> TornadoDummy;

	static constexpr float FlatIslandX = 25000.f;
};

/**
 * Real keyboard input through the whole chain (-PinguInputTest): starts a match from the title menu like a player,
 * then presses keys as if from the keyboard (through Slate, the viewport and the player controller): walking with A / D
 * and the arrows, jumping / double jumping with Space, W (and the Up arrow unless it is bound to Jump) only aiming up -
 * no jump, a second jump key bound while playing (K) with Space while it is held, a tap that goes down and up inside one
 * frame, a jump pressed during a hit stun, Jump rebound to K (K jumps, Space does not), a movement key bound while
 * playing, a key held into the pause menu that must not stay stuck. Runs with the default key bindings and puts the
 * player's own bindings back at the end. Logs [INPUTTEST] RESULT lines and quits.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APenguinInputTest : public AActor
{
	GENERATED_BODY()

public:
	APenguinInputTest();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	struct FKeyStep
	{
		float Time = 0.f;
		FKey Key;
		bool bDown = true;
	};
	/** Which jumps a check counts. */
	enum class EJumpKind : uint8 { Ground, Air, Any };
	/** What a check wants to see in its time window. */
	enum class EExpect : uint8
	{
		Jump,       // a jump of its kind (at least MinHeight high)
		NoJump,     // no jump at all, although ProofKey reached the game
		WalkRight,  // at least MinDistance to the right, with ProofKey
		StandStill, // a menu took the input during the window, and at its end the penguin stands still (no key stuck down)
		Slide,      // a belly slide started, at least MinDistance along (either way), with ProofKey
		GrowSelf,   // the penguin's size target grew (a size ray key held: the ray on yourself), with ProofKey
		Icicle,     // one icicle thrown (its key: ProofKey) with a charge from MinCharge to MaxCharge: a lob (slow, heavy) up
		            // to a charge of 0.5, else straight and fast
	};
	struct FCheck
	{
		/** Icicle: the charge the throw must have, the icicle seen (its speed and gravity when it left), how far it got. */
		float MinCharge = 0.f;
		float MaxCharge = 1.f;
		TWeakObjectPtr<APenguinProjectile> Icicle;
		float IcicleSpeed = -1.f;
		float IcicleGravity = 0.f;
		float IcicleFromX = 0.f;
		float IcicleFar = 0.f;
		float StartTime = 0.f;
		float Time = 0.f;
		FString Label;
		EExpect Expect = EExpect::Jump;
		EJumpKind Kind = EJumpKind::Ground;
		/** The jump must also reach this height (0 = any). */
		float MinHeight = 0.f;
		float MinDistance = 0.f;
		/** NoJump / WalkRight: this key must have reached the game (down in the player input), or the check proves nothing. */
		FKey ProofKey;
		/** NoJump: the move input must also have pointed up (W / Up aim up). */
		bool bWantUpInput = false;
		int32 CountAtStart = 0;
		float StartZ = 0.f;
		float PeakZ = 0.f;
		float StartX = 0.f;
		bool bProofSeen = false;
		bool bUpSeen = false;
		bool bMenuSeen = false;
		bool bStarted = false;
		bool bDone = false;
	};
	/** Something done to the game at a moment of the key script (a hit that stuns the player, a key binding, a menu). */
	struct FAction
	{
		float Time = 0.f;
		TFunction<void()> Do;
		bool bDone = false;
	};

	void Report(const FString& Message) const;
	/** The widget that has keyboard focus, and whether it is the game viewport. */
	FString DescribeFocus() const;
	/** A key event as if from the OS: routed by Slate to the focused widget (the viewport passes it on to the game). */
	static void SendKey(const FKey& Key, bool bDown);
	APenguinCharacter* GetPlayerPenguin() const;

public:
	/** SendKey for the other tests (the lobby test's key on the leaderboard). */
	static void SendKeyEvent(const FKey& Key, bool bDown) { SendKey(Key, bDown); }

private:
	void PrepareArena();
	void BuildScript();
	/** Remembers the player's key bindings and switches to the default ones for the test. */
	void SaveBindings();
	/** Puts the player's own bindings back (only the keys that differ are written). */
	void RestoreBindings();

	/** The jump counter a check looks at. */
	static int32 CountJumps(const UPenguinMovementComponent* Move, EJumpKind Kind);

	int32 Stage = 0;
	/** PLAY opens your own lobby: the match starts from there (START MATCH). */
	bool bLobbyStartSent = false;
	float StageTime = 0.f;
	TArray<FKeyStep> Keys;
	TArray<FCheck> Checks;
	TArray<FAction> Actions;
	int32 NextKey = 0;
	int32 Passed = 0;
	float ScriptEnd = 9.f;
	/** The player's bindings at the start: primary + secondary key of every action, in action order. */
	TArray<FKey> SavedKeys;
	bool bBindingsSaved = false;
};

/**
 * -PinguLobbyTest: the title's PLAY, its lobby and the round flow, driven the way a player does through the HUD's actions
 * (and a real key for the leaderboard). PLAY opens the lobby (your card + the bots'); a double click on the + card adds
 * one bot (its second click lands on the rebuilt card), a double click on a card that has been up a while still presses
 * it; + adds bots, X removes one, the skills and the teams change on the cards (also round and round); START: an endless
 * match, the teams come from the cards (you and a bot red, two bots blue, one on its own), teammates start side by side,
 * each bot plays at its card's skill; the one that picked no team shows on its own (no ring, no team name), the
 * teammates' icons above their heads get a small ring each; round 1 (the blues and the lone bot go into the sea): the
 * leaderboard waits for a key (one pressed too early does nothing; still up after 6 s), a key goes on; round 2: a blue bot
 * flung off the side leaves the camera where it is, red wins again, the match goes on (endless); round 3: the lone bot
 * wins it (the banner: its own name); round 4: the island starts crumbling and the pause menu's END MATCH ends the match
 * in the middle of the fight (red has the most wins: red wins; round 4 does not count; the crumbling stops; a knock-out
 * after that counts nothing; the results' title would name the lone bot itself, had it won); LOBBY keeps the cards (and
 * nobody shows a team there); BACK to the title. Then Traits mode: PLAY, TRAITS is off in the lobby and the toggle turns it
 * on; nine bots and START: ten penguins, ten different traits; the reveal's cards show and the countdown waits for them
 * (the intro lasts 4.8 s or more); END MATCH from the pause menu and LOBBY: TRAITS still on; the toggle turns it off.
 * Screenshots in Saved/ClaudeLogs/Lobby. Logs [LOBBYTEST] RESULT lines and quits.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguLobbyTest : public AActor
{
	GENERATED_BODY()

public:
	APinguLobbyTest();

	virtual void Tick(float DeltaSeconds) override;

private:
	void Report(const FString& Message) const;
	void Shot(const TCHAR* Name) const;
	/** The cards in a line: "You red, Waddles red easy, ..." */
	FString DescribeCards() const;
	/** Knocks out every penguin of the match not in Team (into the sea). */
	void KnockOutAllBut(int32 Team) const;
	class APenguinHUD* GetHUD() const;
	void Finish(const TCHAR* Why);
	/** The lobby's + card double-clicked, through the card's own mouse handlers: the first click adds a bot and rebuilds the
	 *  cards, the second click lands on the new + card as a double click and must add nothing; a double click on a + card
	 *  that has been up a while still presses it. True while it runs (Check logs a RESULT line). */
	bool UpdateAddBotDoubleClick(class APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check);

	int32 Stage = 0;
	float StageTime = 0.f;
	float TotalTime = 0.f;
	int32 Passed = 0;
	int32 Checked = 0;
	bool bEarlyKeySent = false;
	bool bWaitChecked = false;
	bool bKeySent = false;
	bool bShotTaken = false;
	float KeySentAt = -1.f;
	int32 RedTeam = -1;
	/** The bot that picked no team (the last card): its name and its team inside the match (a team of one). */
	FString LoneName;
	int32 LoneTeam = -1;
	/** Round 1: the team rings above the heads were checked (and the screenshot taken). */
	bool bRingsChecked = false;
	/** The cards when the match started (the lobby must have the same ones after it). */
	FString CardsAtStart;
	/** Ending the match from the pause menu: one step after another. */
	int32 SubStep = 0;
	/** Round 2: a blue bot knocked flying off the side; the camera must stay (it moved at most CameraMaxMove). */
	bool bCameraHitDone = false;
	float CameraHitAt = 0.f;
	FVector CameraStart = FVector::ZeroVector;
	float CameraMaxMove = 0.f;
	TWeakObjectPtr<APenguinCharacter> CameraFlyer;
	float FlyerStartX = 0.f;
	float FlyerWay = 1.f;
	float FlyerMaxOut = 0.f;
	/** The lobby's + card double-clicked (the second click lands on the rebuilt card): one step after another. */
	int32 ClickStep = 0;
	int32 BotsBeforeClick = 0;
	float ClickAt = 0.f;
	/** Slate's (real) time of the click, and when the rebuilt + card got the second click. */
	double ClickRealTime = 0.0;
	double SecondClickGap = -1.0;
	TWeakPtr<SWidget> ClickedAddButton;
	/** END MATCH in the middle of round 3's fight: the island was crumbling then, its blocks at the end, the phase. */
	bool bCollapseBeforeEnd = false;
	/** The second match: the pause menu was up (and the game paused) before its MAIN MENU was pressed. */
	bool bPauseBeforeMainMenu = false;
	int32 BlocksAtEnd = -1;
	uint8 PhaseAtEnd = 0;
	/** A knock-out after the match ended: the score sums before it (knock-outs, falls, wins), the one knocked out. */
	int32 KOsBefore = 0;
	int32 FallsBefore = 0;
	int32 WinsBefore = 0;
	TWeakObjectPtr<APenguinCharacter> LateVictim;
	/** Traits mode (the last part, after the title again): the reveal's cards seen at most at once, the Intro's longest
	 *  phase time before the countdown, the traits the ten penguins were dealt. */
	void UpdateTraitsStage(class APenguinGameState* GameState, class APenguinHUD* LobbyHUD, class APenguinGameMode* GameMode,
		TFunctionRef<void(bool, const FString&)> Check);
	int32 TraitCardsSeen = 0;
	int32 TraitFlyingSeen = 0;
	float TraitIntroLongest = 0.f;
	/** The reveal's card layout while the cards show (PinguUI::GetTraitRevealStats): frames sampled, the fewest cards, the
	 *  most at an edge (penguins out of the picture), and the most overlapping pairs / cards over a marker / cards not
	 *  wholly in the picture (want 0 each). */
	int32 TraitLayoutFrames = 0;
	int32 TraitLayoutFewest = 1000;
	int32 TraitLayoutEdge = 0;
	int32 TraitLayoutOverlaps = 0;
	int32 TraitLayoutOverMarkers = 0;
	int32 TraitLayoutOutside = 0;
	/** 2026-11-07 (shared-traits): the round's one banner (frames it showed, its lines, ever out of the picture) and the TRAITS
	 *  list's sub-steps (0 not opened, 1 judged, 2 left). */
	int32 TraitBannerFrames = 0;
	int32 TraitBannerLines = 0;
	bool bTraitBannerOutside = false;
	int32 TraitListStep = 0;
	bool bTraitCountdownSeen = false;
	bool bTraitDealChecked = false;
	int32 TraitSubStep = 0;

	/** Ability picks (2026-11-07, PenguinAutoTestLobbyPicks.cpp): before the second match the lobby's ABILITIES switch goes to
	 *  CHOOSE and slots 1 and 3 get Snowball and Flap by real presses (lobby_picks_empty / _grid / _filled); true once done. In
	 *  that match's fight the loadout must hold them (CheckPicksDealt). */
	bool UpdatePicksStage(class APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check);
	void CheckPicksDealt(class APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check);
	int32 PicksSubStep = 0;
	float PicksAt = 0.f;

	/** The lobby island's size (PenguinAutoTestIsland.cpp), right after the + card's double click: nine bots - a bigger
	 *  island, the camera at its furthest on your penguin (lobbytest_island_big) - then one bot - a small island, the camera
	 *  closer (lobbytest_island_small). True while it runs (Check logs a RESULT line each). */
	bool UpdateLobbyIslandSizeCheck(class APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check);
	int32 IslandStep = 0;
	float IslandStepAt = 0.f;
	int32 IslandStartPenguins = 0;
	float IslandStartWidth = 0.f;
	float IslandBigWidth = 0.f;
	float IslandBigDistance = 0.f;

	/** The lobby island (PenguinAutoTestLobbyIsland.cpp; not with -PinguOldMenus): the island and your penguin's tag, the "+"
	 *  (a bot puffs in), a click on the bot (its team, the beanie; a right click back), its difficulty tag, its x (it puffs
	 *  away) - all as clicks at screen positions. True while it runs (Check logs a RESULT line per part). */
	bool UpdateLobbyIslandChecks(class APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check);
	int32 LobbyStep = 0;
	float LobbyStepAt = -1.f;
	bool LobbyShot = false;
	TArray<FString> LobbyNames;
	int32 LobbyBotsBefore = 0;
	int32 LobbyPuffInsBefore = 0;
	int32 LobbyPuffOutsBefore = 0;
	int32 LobbyPokesBefore = 0;
	int32 LobbyTeamBefore = -1;
	int32 LobbySkillBefore = 0;
	float LobbyMinPop = 2.f;
	float LobbyBotSeenAt = -1.f;
	bool LobbyRightClicked = false;
	bool LobbyLeftOk = false;
	bool LobbyShrinking = false;
	FString LobbyAfterLeft;
	FString LobbyDetail;
	TWeakObjectPtr<APenguinCharacter> LobbyBot;
	/** The 3D button strip in front of a penguin: a click there is the menu's (APinguMenu3D::HasButtonAt), never the island's. */
	void CheckLobbyButtonStrip(class APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check);
	/** Keys only (PenguinAutoTestLobbyIsland.cpp): real key events - Esc gives the 3D strip the keys (START hot, the penguin gets
	 *  no input), Left / Right move along it, Esc gives them back, gamepad Start takes them again, Enter presses START (the match
	 *  starts). True while it runs (one RESULT line). */
	bool UpdateLobbyStripKeys(class APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check);
	int32 StripKeyStep = 0;
	float StripKeyAt = -1.f;
	bool bStripKeysOk = true;
	FString StripKeyLog;

	/** The host's address line (PenguinAutoTestCopyAddress.cpp), after the island check: the HUD's copy action puts the address
	 *  it shows on the clipboard (the player's own clipboard text is put back after), COPIED! shows 1.5 s. True while it runs
	 *  (Check logs one RESULT line). */
	bool UpdateCopyAddressCheck(class APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check);
	/** The 3D menus (PenguinAutoTestMenu3D.cpp; not with -PinguOldMenus): the title's PLAY clicked on the 3D title (true: it was;
	 *  else the test calls the action), then in the lobby its 3D TRAITS clicked twice and SETTINGS once (BACK returns): one RESULT
	 *  line. UpdateMenu3DLobbyCheck is true while it runs. */
	bool UpdateMenu3DTitlePlay(class APenguinHUD* LobbyHUD);
	bool UpdateMenu3DLobbyCheck(class APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check);
	int32 Menu3DStep = 0;
	float Menu3DStepAt = -1.f;
	FString Menu3DShown;
	FString Menu3DLabels;
	bool bMenu3DPlayClicked = false;
	bool bMenu3DTraitsBefore = false;
	bool bMenu3DClicks = false;
	bool bMenu3DTraitsToggled = false;
	bool bMenu3DTraitsBack = false;
	bool bMenu3DSettings = false;
	int32 CopyStep = 0;
	double CopyAt = 0.0;
	FString CopySaved;
	FString CopyShown;
	FString CopyHeld;
	/** The address the system routes outwards from, and whether the line shows it (any private one: not a virtual adapter's). */
	FString CopyRoute;
	bool bCopyRouteOk = false;
	bool bCopyShownBefore = false;
	bool bCopyShownAtOnce = false;
	bool bCopyShownMid = false;
	bool bCopyShownMidSampled = false;
};
