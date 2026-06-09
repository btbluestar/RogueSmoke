// DownComponent.as
// Down/revive — the co-op lose condition (MVP §12) and the D-0010 party-wipe hook. When a hero's
// Health hits 0 it goes DOWNED instead of dying: a bleed-out timer runs, a living teammate standing
// nearby revives it, and if the timer expires it becomes run-DEAD. When EVERY hero is incapacitated
// the party has wiped -> RaidObjective.NotifyPartyWiped() ends the run in defeat.
//
// Server-authoritative. The *replicated* life-state lives on AHeroCharacter (bDowned/bDeadDowned —
// they ride the proven-replicating pawn); this component owns the logic, tunables, and timers.
// MVP revive is proximity-auto (a living teammate within radius fills the bar); a hold-interact
// version + a downed crawl/ragdoll pose are polish.

class URogueDownComponent : UActorComponent
{
    // Seconds a downed hero survives before becoming run-dead (if not revived).
    UPROPERTY(EditDefaultsOnly, Category = "Down")
    float BleedOutSeconds = 30.0;

    // Seconds a living teammate must stand within ReviveRadius to revive a downed hero.
    UPROPERTY(EditDefaultsOnly, Category = "Down")
    float ReviveHoldSeconds = 3.0;

    UPROPERTY(EditDefaultsOnly, Category = "Down")
    float ReviveRadius = 240.0;

    // Fraction of MaxHealth restored on revive.
    UPROPERTY(EditDefaultsOnly, Category = "Down")
    float ReviveHealthFraction = 0.5;

    private AHeroCharacter Hero;
    private float BleedOutRemaining = 0.0;
    private float ReviveProgress = 0.0;

    // Called by the hero (server + clients) once the ASC is ready. Self-gates to the server: only
    // there do we subscribe to Health changes and run the down/revive state machine.
    void Initialize(AHeroCharacter InHero)
    {
        Hero = InHero;
        if (Hero == nullptr || !Hero.HasAuthority())
            return;

        UAngelscriptAbilitySystemComponent ASC = Hero.GetRogueAbilitySystem();
        if (ASC != nullptr)
            ASC.RegisterAttributeChangedCallback(URogueHealthSet, n"Health", this, n"OnHealthChanged");
    }

    // Server: fires on every Health change. We act only on the crossing to <= 0 (going down).
    // Using the attribute-changed callback (not the health set's one-shot OnOutOfHealth) means revive
    // and re-death work cleanly without the C++ out-of-health latch getting in the way.
    UFUNCTION()
    void OnHealthChanged(FAngelscriptAttributeChangedData Data)
    {
        if (Hero == nullptr || !Hero.HasAuthority())
            return;
        if (Data.GetNewValue() <= 0.0 && !Hero.IsIncapacitated())
            EnterDowned();
    }

    private void EnterDowned()
    {
        BleedOutRemaining = BleedOutSeconds;
        ReviveProgress = 0.0;
        Hero.SetDownedState(true, false);   // downed, not yet dead
        CheckPartyWipe();
    }

    // Driven by the hero's server Tick.
    void TickDown(float DeltaSeconds)
    {
        if (Hero == nullptr || !Hero.HasAuthority() || !Hero.IsDowned())
            return;

        // Revive: a living teammate within range fills the bar; it decays if they step away.
        if (IsLivingTeammateNearby())
        {
            ReviveProgress += DeltaSeconds;
            if (ReviveProgress >= ReviveHoldSeconds)
            {
                Revive();
                return;
            }
        }
        else
        {
            ReviveProgress = Math::Max(0.0, ReviveProgress - DeltaSeconds);
        }

        // Bleed-out -> run-dead (not revivable in the MVP).
        BleedOutRemaining -= DeltaSeconds;
        if (BleedOutRemaining <= 0.0)
        {
            Hero.SetDownedState(false, true);   // dead
            CheckPartyWipe();
        }
    }

    private void Revive()
    {
        ReviveProgress = 0.0;
        UAngelscriptAbilitySystemComponent ASC = Hero.GetRogueAbilitySystem();
        if (ASC != nullptr)
        {
            float MaxHP = ASC.GetAttributeCurrentValue(URogueHealthSet, n"MaxHealth", 100.0);
            ASC.SetAttributeBaseValue(URogueHealthSet, n"Health", MaxHP * ReviveHealthFraction);
        }
        Hero.SetDownedState(false, false);   // back on your feet
    }

    // A non-incapacitated hero other than us, within ReviveRadius.
    private bool IsLivingTeammateNearby() const
    {
        FVector Mine = Hero.GetActorLocation();
        float RadiusSq = ReviveRadius * ReviveRadius;

        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        for (AHeroCharacter Other : Heroes)
        {
            if (Other == nullptr || Other == Hero || Other.IsIncapacitated())
                continue;
            if ((Mine - Other.GetActorLocation()).SizeSquared() <= RadiusSq)
                return true;
        }
        return false;
    }

    // Party wipe = every hero incapacitated. Ends the run in defeat via the objective hook (D-0010).
    private void CheckPartyWipe()
    {
        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        if (Heroes.Num() == 0)
            return;

        for (AHeroCharacter H : Heroes)
        {
            if (H != nullptr && !H.IsIncapacitated())
                return;     // someone is still up — not a wipe
        }

        TArray<ARaidObjective> Objectives;
        GetAllActorsOfClass(Objectives);
        for (ARaidObjective Obj : Objectives)
        {
            if (Obj != nullptr)
                Obj.NotifyPartyWiped();
        }
    }
}
