// AttackingElite.h
// C++ base for the attacking elites/specials (Carapace / Spitter / Bloater / Lunger — bio-horde roster).
// Extends AEliteEnemyBase so it inherits replicated health, the Clustered synergy state, pull steering,
// and pool registration. Self-contained like AFodderEnemy: it owns a visible body with Visibility-blocking
// collision, so an elite is visible AND hittable with no Blueprint or assigned mesh.
//
// The shared server loop — acquire the nearest LIVING hero (GAS Health > 0), approach to PreferredRange,
// telegraph, then attack — lives here ("compile the simulation"). The per-archetype attack is a
// BlueprintNativeEvent that AngelScript subclasses override ("script the decision"). All tuning is
// UPROPERTY (designer-iterable). Telegraphs are a hard readability requirement (GDD §10); the wind-up is
// the player's counterplay window.

#pragma once

#include "CoreMinimal.h"
#include "Enemies/EliteEnemyBase.h"
#include "AttackingElite.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class APawn;

UENUM(BlueprintType)
enum class EEngageState : uint8
{
	// Holds at a ring standoff, no telegraph/attack — until the combat director grants a token.
	Background,
	// Holds a token: runs the full approach→telegraph→attack loop.
	Engaged
};

UCLASS()
class ROGUESMOKE_API AAttackingElite : public AEliteEnemyBase
{
	GENERATED_BODY()

public:
	AAttackingElite();

	// Tunables are BlueprintReadWrite so AngelScript subclasses can both set them via `default` and read
	// them in overridden attacks (e.g. Lunger/Bloater reading AttackDamage/AttackRadius).

	/** 0 = keep the HealthComponent default (100); else set max+current at BeginPlay (per-archetype HP). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float MaxHealthOverride = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float MoveSpeed = 220.f;

	/** Stop closing once this near the target (melee elites ~150; ranged elites set this large to kite). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float PreferredRange = 160.f;

	/** The attack reaches when the target is within this distance. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float AttackRange = 220.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float AttackDamage = 15.f;

	/** >0 = radial attack (slam/explosion) damaging all players in the radius; 0 = single-target. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float AttackRadius = 0.f;

	/** During a dash (e.g. the Lunger's leap), deal AttackDamage on first contact within this range. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float DashContactRange = 160.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float AttackInterval = 2.5f;

	/** Wind-up before the attack lands — the readability tell + the player's dodge window (GDD §10). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float TelegraphSeconds = 0.7f;

	/** Per-archetype silhouette scale, applied to the body at BeginPlay. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	FVector BodyScale = FVector(1.f, 1.f, 2.f);

	/** Per-archetype body tint (readability — tell archetypes apart at a glance). Applied at BeginPlay. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	FLinearColor BodyColor = FLinearColor::White;

	/** Body scale multiplier reached at the END of the wind-up (the Bloater's swell tell; 1 = none). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float TelegraphSwell = 1.f;

	UPROPERTY(EditDefaultsOnly, Category="Enemy")
	bool bShowDebug = true;

	/** The currently targeted player pawn (nearest living hero). Valid only on the server. */
	UFUNCTION(BlueprintPure, Category="Enemy")
	APawn* GetCurrentTarget() const { return Target.Get(); }

	/** True while this elite is winding up an attack (for cues / animation). */
	UFUNCTION(BlueprintPure, Category="Enemy")
	bool IsTelegraphing() const { return bTelegraphing; }

	/**
	 * The attack committed at the end of the telegraph. Override in AngelScript subclasses for ranged /
	 * explosion / charge behaviors. Default: radial slam when AttackRadius > 0, else single-target melee.
	 */
	UFUNCTION(BlueprintNativeEvent, Category="Combat")
	void PerformAttack();
	virtual void PerformAttack_Implementation();

	/**
	 * Server event at the START of a wind-up — the moment to lock targets and show attack-specific
	 * telegraphs (e.g. the Brood-mother ringing its artillery zone via ShowTelegraphZone). Default: nothing.
	 */
	UFUNCTION(BlueprintNativeEvent, Category="Combat")
	void OnTelegraphStarted();
	virtual void OnTelegraphStarted_Implementation() {}

	/** Cosmetic health hook (every machine): white hit-flash, then the base class death burst. */
	virtual void NotifyHealthVisual(bool bDamaged, bool bDied) override;

	/**
	 * Begin a self-propelled dash (charge) along Direction (flattened) at Speed for Duration seconds. While
	 * dashing the elite ignores normal approach/telegraph and slides smoothly; on first contact within
	 * DashContactRange of the target it deals AttackDamage once. Archetypes trigger this from their attack
	 * (the Lunger's leap) instead of teleporting; reusable for any future charger.
	 */
	UFUNCTION(BlueprintCallable, Category="Combat")
	void StartDash(FVector Direction, float Speed, float Duration);

	/** True while a dash is in progress (for cues / gating). */
	UFUNCTION(BlueprintPure, Category="Enemy")
	bool IsDashing() const { return bDashing; }

	/** Combat-director scheduling. Background = circle/hold; Engaged = token-holder, full attack loop. */
	UFUNCTION(BlueprintCallable, Category="Combat|Director")
	void SetEngageState(EEngageState NewState);

	UFUNCTION(BlueprintPure, Category="Combat|Director")
	EEngageState GetEngageState() const { return EngageState; }

	/** Seconds since the last engage-state change (Engaged timeout clock AND Background cooldown clock). */
	UFUNCTION(BlueprintPure, Category="Combat|Director")
	float GetTimeInEngageState() const { return TimeInEngageState; }

	/** True once this elite has committed an attack during its current Engaged stint (release signal). */
	UFUNCTION(BlueprintPure, Category="Combat|Director")
	bool HasAttackedThisEngagement() const { return bAttackedThisEngagement; }

	/** Token-gated? False for fodder (AFodderEnemy) so the director ignores them. */
	UFUNCTION(BlueprintPure, Category="Combat|Director")
	bool GetUsesAttackToken() const { return bUsesAttackToken; }

	/** Alive per its HealthComponent (no health component = treat as alive). */
	UFUNCTION(BlueprintPure, Category="Combat|Director")
	bool IsAlive() const;

	/** Ring standoff a Background token-user holds at (further than AttackRange). */
	UFUNCTION(BlueprintPure, Category="Combat|Director")
	float GetRingStandoff() const { return AttackRange * RingStandoffMult; }

	/** Background token-users hold at AttackRange * this (the combat circle radius). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Combat|Director")
	float RingStandoffMult = 1.6f;

	/** Token-gated by the combat director. Set false on an AAttackingElite subclass to let it
	 *  attack freely (skip the ring-standoff/token loop). AFodderEnemy is NOT a subclass of
	 *  AAttackingElite — it is already exempt by not having this loop at all. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Combat|Director")
	bool bUsesAttackToken = true;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void ClearTransientState() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual FLinearColor GetDeathBurstColor() const override { return BodyColor; }

	/** Cheap visible body + hitscan target (Visibility-blocking), no skeletal mesh / AIController. */
	UPROPERTY(VisibleAnywhere, Category="Enemy")
	UStaticMeshComponent* Body;

	// Telegraph ground rings (GDD §10 cue pass): a danger footprint plus a fill disc that reaches
	// the edge exactly at impact. Driven cosmetically on every machine from replicated bTelegraphing.
	UPROPERTY(VisibleAnywhere, Category="Enemy")
	UStaticMeshComponent* TelegraphOutline;

	UPROPERTY(VisibleAnywhere, Category="Enemy")
	UStaticMeshComponent* TelegraphFill;

	UPROPERTY()
	UMaterialInstanceDynamic* BodyMID = nullptr;

	UPROPERTY()
	UMaterialInstanceDynamic* TelegraphFillMID = nullptr;

	EEngageState EngageState = EEngageState::Background;
	float TimeInEngageState = 0.f;
	bool bAttackedThisEngagement = false;

	TWeakObjectPtr<APawn> Target;
	float AttackCooldown = 0.f;
	float TelegraphRemaining = 0.f;

	/** Replicated so remote clients see the wind-up cues, not just the listen-server host. */
	UPROPERTY(ReplicatedUsing=OnRep_Telegraphing)
	bool bTelegraphing = false;

	UFUNCTION()
	void OnRep_Telegraphing();

	/** Client-side wind-up progress (the server uses TelegraphRemaining; clients accumulate). */
	float LocalTelegraphElapsed = 0.f;

	/** World time before which the body flashes white (hit feedback). */
	float FlashUntilSeconds = 0.f;

	/** Per-frame, every machine: rings, body warning pulse, swell, hit flash. Stateless cosmetics. */
	void UpdateCombatCosmetics(float DeltaSeconds);

	// Dash (charge) state — see StartDash. Slides the actor over several frames instead of teleporting.
	bool bDashing = false;
	FVector DashDir = FVector::ZeroVector;
	float DashSpeed = 0.f;
	float DashTimeRemaining = 0.f;
	bool bDashHitApplied = false;
	void UpdateDash(float DeltaSeconds);

	void AcquireTarget();
	bool IsTargetInAttackRange() const;
	void ApproachTarget(float DeltaSeconds, float StopRange);
	void FaceTarget();
	/** Current Health of a pawn via its ASC (GAS); returns 1 when unknown (treat as alive). */
	float GetPawnHealth(APawn* P) const;
	void DrawEnemyDebug() const;
};
