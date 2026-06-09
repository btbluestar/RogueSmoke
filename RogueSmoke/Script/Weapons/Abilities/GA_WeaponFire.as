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

        // Aim from the follow camera (over-the-shoulder). On a listen server the camera transform
        // reflects the replicated control rotation. Client target-data aim is a later precision upgrade.
        FVector AimStart = Hero.FollowCamera.GetWorldLocation();
        FVector AimDir = Hero.FollowCamera.GetForwardVector();

        bool bMoving = Hero.CharacterMovement != nullptr && Hero.CharacterMovement.Velocity.Size() > 50.0;
        float HalfAngleRad = Weapon.GetSpreadDegrees(bMoving) * HalfAngleDegToRad;

        AActor Avatar = GetAvatarActorFromActorInfo();
        TArray<FVector> Impacts;
        for (int i = 0; i < Def.BulletsPerCartridge; i++)
        {
            FVector Dir = (HalfAngleRad > 0.0) ? Math::VRandCone(AimDir, HalfAngleRad) : AimDir;
            FVector End = AimStart + Dir * Def.MaxRange;
            FHitscanResult Result = Combat.FireHitscan(AimStart, End, Def.Damage, Avatar);
            Impacts.Add(Result.ImpactPoint);
        }

        Weapon.NotifyFired();
        Hero.Multicast_FireFX(AimStart, Impacts);
    }
}
