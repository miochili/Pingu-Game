// Pingu - the abilities' tuning numbers (see PinguTuning.h): the PINGUPEDIA's stat lines.

#include "PinguTuning.h"
#include "PenguinAbilities.h"
#include "PenguinMovementComponent.h"

void PinguTuning::GetAbilityStats(EPenguinAbility Ability, const UPenguinMovementComponent* Move, TArray<FPinguStatLine>& Out)
{
	Out.Reset();
	if (Ability == EPenguinAbility::None || Ability >= EPenguinAbility::Count)
	{
		return;
	}
	const UPenguinMovementComponent* Movement = Move ? Move : GetDefault<UPenguinMovementComponent>();
	const auto Add = [&Out](EPinguStatKind Kind, float Value) { Out.Add({ Kind, Value, true }); };
	Add(EPinguStatKind::Cooldown, UPenguinAbilityComponent::GetInfo(Ability).Cooldown);
	switch (Ability)
	{
	case EPenguinAbility::Snowball:
		Add(EPinguStatKind::Knockback, SnowballPush);
		Add(EPinguStatKind::Lift, SnowballLift);
		Add(EPinguStatKind::Stun, SnowballStun);
		Add(EPinguStatKind::Speed, SnowballSpeed);
		Add(EPinguStatKind::ComboStep, SnowballComboStep); // every hit in a row pushes this much harder ...
		Add(EPinguStatKind::ComboMax, SnowballComboMax);   // ... up to this many times the first hit's push
		break;
	case EPenguinAbility::Icicle:
		Add(EPinguStatKind::Speed, IcicleSpeed);     // a full throw (a second's hold): perfectly straight
		Add(EPinguStatKind::Range, IcicleTapReach);  // a tap: a weak lob that drops after about this far
		Add(EPinguStatKind::Duration, PegLifetime);
		Add(EPinguStatKind::Lift, Movement ? Movement->JumpPadSpeed : 0.f); // stuck in a wall or the ground: a jump pad
		break;
	case EPenguinAbility::BellyStomp:
		Add(EPinguStatKind::Speed, Movement ? Movement->StompSpeed : 0.f);
		Add(EPinguStatKind::KillRadius, StompSlamReach);
		Add(EPinguStatKind::Radius, StompShockRadius);
		Add(EPinguStatKind::Knockback, StompShockPush);
		Add(EPinguStatKind::Stun, StompShockStun);
		break;
	case EPenguinAbility::IcebergDrop:
		Add(EPinguStatKind::Range, IcebergReachX);
		break;
	case EPenguinAbility::FlipperSlap:
		Add(EPinguStatKind::Stun, SlapStun);
		Add(EPinguStatKind::Knockback, SlapPush);
		Add(EPinguStatKind::Range, SlapReachAhead + SlapHitRadius);
		break;
	case EPenguinAbility::FrostBreath:
		Add(EPinguStatKind::Freeze, FrostFreezeTime);
		Add(EPinguStatKind::Range, FrostRange);
		Add(EPinguStatKind::Cone, FrostConeDeg);
		Add(EPinguStatKind::Duration, FrostBreathTime);
		break;
	case EPenguinAbility::IceWall:
		Add(EPinguStatKind::Blocks, static_cast<float>(IceWallBlocks));
		Add(EPinguStatKind::Range, IceWallRange);
		break;
	case EPenguinAbility::SnowWarp:
		Add(EPinguStatKind::Range, WarpRange);
		break;
	case EPenguinAbility::FishHook:
		Add(EPinguStatKind::Range, HookReach);
		Add(EPinguStatKind::Speed, HookSpeed);
		break;
	case EPenguinAbility::IceShell:
		Add(EPinguStatKind::Duration, ShellDuration);
		break;
	case EPenguinAbility::Gust: // the tornado: its throw at the middle (the edge gets 0.3 of it), its reach, how long it lasts
		Add(EPinguStatKind::Knockback, NootPointBlankPush * TornadoPushCenterShare);
		Add(EPinguStatKind::Lift, TornadoLiftCenter);
		Add(EPinguStatKind::Radius, TornadoRadius);
		Add(EPinguStatKind::Stun, TornadoStunCenter);
		Add(EPinguStatKind::Duration, TornadoTime);
		break;
	case EPenguinAbility::SnowBomb:
		Add(EPinguStatKind::Fuse, BombFuse);
		Add(EPinguStatKind::KillRadius, BombKillRadius);
		Add(EPinguStatKind::Radius, BombPushRadius);
		Add(EPinguStatKind::Knockback, BombPush);
		break;
	case EPenguinAbility::Noot:
		Add(EPinguStatKind::Knockback, NootPointBlankPush);
		Add(EPinguStatKind::Range, NootRange);
		Add(EPinguStatKind::Cone, NootConeDeg);
		Add(EPinguStatKind::Stun, NootStunPointBlank);
		break;
	case EPenguinAbility::IceGun:
		Add(EPinguStatKind::Slip, IceGunSlipTime);
		Add(EPinguStatKind::Speed, IceBoltSpeed);
		break;
	case EPenguinAbility::ShrinkRay:
		Add(EPinguStatKind::SizeStep, PinguShrinkRayFactor);
		Add(EPinguStatKind::Duration, SizeRayDuration);
		break;
	case EPenguinAbility::GrowRay:
		Add(EPinguStatKind::SizeStep, PinguGrowRayFactor);
		Add(EPinguStatKind::Duration, SizeRayDuration);
		break;
	case EPenguinAbility::Snowman:
		Add(EPinguStatKind::Duration, SnowmanReload);   // THROWS EVERY
		Add(EPinguStatKind::Range, SnowmanRange);
		Add(EPinguStatKind::Knockback, SnowballPush);   // its snowballs: the snowball's push (and its combo)
		break;
	case EPenguinAbility::GiantSnowball:
		Add(EPinguStatKind::Duration, GiantBuildTime);                 // ROLL-UP: hold this long
		Add(EPinguStatKind::Speed, GiantRollSpeed);                    // ROLL SPEED
		Add(EPinguStatKind::Range, GiantRollMaxSize);                  // GROWS TO (rolling; snowballs push it further)
		Add(EPinguStatKind::Blocks, static_cast<float>(GiantWallHits)); // BREAKS ON WALL HIT
		break;
	case EPenguinAbility::Flap:
		Add(EPinguStatKind::Range, FlapPadHeight * FlapGroundHeightShare); // FROM THE GROUND: climbs this high
		Add(EPinguStatKind::Radius, FlapPadHeight * FlapFallHeightShare);  // FROM A FALL
		Add(EPinguStatKind::Speed, FlapPadSpeed * FlapSpeedShare);         // CLIMB SPEED
		break;
	default:
		break;
	}
}
