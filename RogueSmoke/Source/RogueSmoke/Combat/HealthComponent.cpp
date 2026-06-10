// HealthComponent.cpp

#include "Combat/HealthComponent.h"
#include "AbilitySystem/RoguePlayerState.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

UHealthComponent::UHealthComponent()
{
	// Tick exists only to advance DoTs; it stays disabled until ApplyDot enables it.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	SetIsReplicatedByDefault(true);
}

void UHealthComponent::BeginPlay()
{
	Super::BeginPlay();
	Health = MaxHealth;
}

float UHealthComponent::ApplyDamage(float Amount, AActor* DamageInstigator)
{
	// Authority discipline (CODING_STANDARDS §4.4): damage is server-only.
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority() || IsDead() || Amount <= 0.f)
	{
		return 0.f;
	}

	const float Before = Health;
	Health = FMath::Max(0.f, Health - Amount);
	const float Applied = Before - Health;

	// Stat credit: every player->enemy damage path (hitscan, radial, future ones) funnels through
	// here, so this is the one place damage-dealt and kills attribute to the instigating player.
	// Pool-safe: IsDead() early-outs above and ResetHealth() restores on recycle, so a given death
	// credits exactly one kill.
	if (Applied > 0.f && DamageInstigator != nullptr)
	{
		if (const APawn* InstigatorPawn = Cast<APawn>(DamageInstigator))
		{
			if (ARoguePlayerState* PS = InstigatorPawn->GetPlayerState<ARoguePlayerState>())
			{
				PS->AddDamageDealt(Applied);
				if (IsDead())
				{
					PS->AddKill();
				}
			}
		}
	}

	if (IsDead())
	{
		OnDeath.Broadcast(GetOwner());
	}

	return Applied;
}

void UHealthComponent::ResetHealth()
{
	Health = MaxHealth;
	// Pooled-actor recycle: a fresh life must not inherit the previous life's burn/poison.
	ClearDots();
}

void UHealthComponent::ApplyDot(ERogueDotType Type, float DamagePerSecond, float Duration, AActor* DamageInstigator)
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority() || IsDead() ||
	    DamagePerSecond <= 0.f || Duration <= 0.f)
	{
		return;
	}

	FActiveDot& Dot = Dots[static_cast<int32>(Type)];
	// Refresh policy: restart the clock, keep the strongest tick. Re-proccing a weak burn never
	// downgrades a strong one, and stacking is bounded by design (one slot per type).
	Dot.Dps = FMath::Max(Dot.Dps, DamagePerSecond);
	Dot.Remaining = FMath::Max(Dot.Remaining, Duration);
	Dot.Instigator = DamageInstigator;

	SetComponentTickEnabled(true);
}

bool UHealthComponent::HasActiveDot(ERogueDotType Type) const
{
	return Dots[static_cast<int32>(Type)].Remaining > 0.f;
}

void UHealthComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                     FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	bool bAnyActive = false;
	for (FActiveDot& Dot : Dots)
	{
		if (Dot.Remaining <= 0.f)
		{
			continue;
		}

		const float Step = FMath::Min(DeltaTime, Dot.Remaining);
		ApplyDamage(Dot.Dps * Step, Dot.Instigator.Get());
		Dot.Remaining -= DeltaTime;

		if (IsDead())
		{
			// Death mid-dot: stop everything (the pool reset re-arms via ResetHealth).
			ClearDots();
			return;
		}

		if (Dot.Remaining > 0.f)
		{
			bAnyActive = true;
		}
		else
		{
			Dot.Dps = 0.f;
			Dot.Instigator = nullptr;
		}
	}

	if (!bAnyActive)
	{
		SetComponentTickEnabled(false);
	}
}

void UHealthComponent::ClearDots()
{
	for (FActiveDot& Dot : Dots)
	{
		Dot.Dps = 0.f;
		Dot.Remaining = 0.f;
		Dot.Instigator = nullptr;
	}
	SetComponentTickEnabled(false);
}

void UHealthComponent::OnRep_Health()
{
	// Hook for clients to update health bars / cosmetics. No gameplay logic here.
}

void UHealthComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UHealthComponent, Health);
}
