// GA_WeaponFire.as
// The shooting ability (Lyra's ULyraGameplayAbility_RangedWeapon role). Server-authoritative hitscan
// through the seam: reads the hero's equipped weapon, fires BulletsPerCartridge pellets within the
// current spread cone from the camera aim, and damages enemies via UCombatSubsystem::FireHitscan.
// Rate / ammo / reload / spread are owned by URogueWeaponComponent. Cosmetics + recoil go out on an
// unreliable multicast (Hero.Multicast_FireFX).
//
// Semi-auto fires once per activation (the controller activates on press). Full-auto re-activates
// each frame while held (driven by the hero Tick); URogueWeaponComponent.CanFire() gates the rate.
class UGA_WeaponFire : UGA_RogueAbility
{
    // 0.5 * (PI / 180): degrees of full spread -> radians of cone half-angle for VRandCone.
    const float HalfAngleDegToRad = 0.00872664626;

    UFUNCTION(BlueprintOverride)
    void ActivateAbility()
    {
        AHeroCharacter Hero = GetHero();
        if (Hero == nullptr || Hero.Weapon == nullptr || !Hero.Weapon.CanFire())
        {
            EndAbility();
            return;
        }

        if (!CommitAbility())
        {
            EndAbility();
            return;
        }

        if (HasAuthority())
            FireOneCartridge(Hero);

        EndAbility();
    }

    private void FireOneCartridge(AHeroCharacter Hero)
    {
        URogueWeaponComponent Weapon = Hero.Weapon;
        URogueWeaponDefinition Def = Weapon.Definition;
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Def == nullptr || Combat == nullptr)
            return;

        // Third-person convergence (D-0014): the camera sits over the shoulder, so firing straight from it
        // looks wrong. Instead find the world point under the crosshair from the camera, then fire from the
        // MUZZLE toward that point — bullets originate at the gun yet land on the reticle. On a listen
        // server the camera transform reflects the replicated control rotation. Client target-data aim is a
        // later precision upgrade.
        AActor Avatar = GetAvatarActorFromActorInfo();
        FVector CamLoc = Hero.FollowCamera.GetWorldLocation();
        FVector CamDir = Hero.FollowCamera.GetForwardVector();
        FVector AimPoint = Combat.ResolveAimPoint(CamLoc, CamDir, Def.MaxRange, Avatar);

        FVector MuzzleLoc = Hero.GetMuzzleLocation();
        FVector BaseDir = (AimPoint - MuzzleLoc).GetSafeNormal();
        if (BaseDir.IsNearlyZero())
            BaseDir = CamDir;   // degenerate (muzzle ~ aim point): fall back to camera forward

        bool bMoving = Hero.CharacterMovement != nullptr && Hero.CharacterMovement.Velocity.Size() > 50.0;
        float HalfAngleRad = Weapon.GetSpreadDegrees(bMoving, Hero.bFocusing) * HalfAngleDegToRad;

        // Weapon upgrades (WEAPON_UPGRADES_PLAN.md): per-shot behavior from URogueCombatSet
        // attributes, executed by the seam. Defaults (all attributes 0) = a plain hitscan.
        // WeaponDamageBonus is the weapon track; AbilityPower stays ability-only.
        FWeaponShotParams Shot;
        Shot.Damage = Def.Damage * (1.0 + GetCombatAttribute(n"WeaponDamageBonus"));
        Shot.PierceCount = int(GetCombatAttribute(n"PierceCount"));
        Shot.ChainCount = int(GetCombatAttribute(n"ChainCount"));
        Shot.BurnChance = GetCombatAttribute(n"BurnChance");
        Shot.PoisonChance = GetCombatAttribute(n"PoisonChance");
        Shot.ChainIgniteFraction = GetCombatAttribute(n"ChainIgniteFraction");
        Shot.ClusterChainBonusArcs = int(GetCombatAttribute(n"ClusterChainBonusArcs"));

        TArray<FVector> Impacts;
        bool bHitEnemy = false;
        for (int i = 0; i < Def.BulletsPerCartridge; i++)
        {
            FVector Dir = (HalfAngleRad > 0.0) ? Math::VRandCone(BaseDir, HalfAngleRad) : BaseDir;
            FVector End = MuzzleLoc + Dir * Def.MaxRange;
            FHitscanResult Result = Combat.FireWeaponShot(MuzzleLoc, End, Shot, Avatar);
            Impacts.Add(Result.ImpactPoint);
            if (Result.bHitEnemy)
                bHitEnemy = true;
        }

        Weapon.NotifyFired();
        Hero.Multicast_FireFX(MuzzleLoc, Impacts, bHitEnemy);
    }
}
