// HeroAnimInstance.as
// The hero's anim-graph data source (Lyra's LyraAnimInstance role): AngelScript computes every
// variable the ABP graph reads, so all animation logic hot-reloads and the graph stays pure
// blending. Task 5 fills in the full variable set; this spike proves the subclass + override work.
//
// Binding facts confirmed from AnimInstance.h (F:\UEAS\Engine\Source\Runtime\Engine\Classes\Animation\AnimInstance.h):
//   - BlueprintUpdateAnimation(float DeltaTimeX) -> BlueprintImplementableEvent (line 551)
//   - TryGetPawnOwner() -> BlueprintCallable, returns APawn* (line 452)
//   - BlueprintInitializeAnimation() -> BlueprintImplementableEvent (line 547)
//   These are reflected via UFUNCTION, not explicit Bind_*.cpp registrations (no bind file for
//   UAnimInstance exists in the fork; the fork exposes UFUNCTION reflections directly to AS).
class URogueHeroAnimInstance : UAnimInstance
{
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float GroundSpeed = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bIsFalling = false;

    UFUNCTION(BlueprintOverride)
    void BlueprintUpdateAnimation(float DeltaTimeX)
    {
        APawn Pawn = TryGetPawnOwner();
        if (Pawn == nullptr)
            return;

        FVector Vel = Pawn.GetVelocity();
        GroundSpeed = FVector(Vel.X, Vel.Y, 0.0).Size();

        UPawnMovementComponent MoveComp = Pawn.GetMovementComponent();
        bIsFalling = (MoveComp != nullptr) ? MoveComp.IsFalling() : false;
    }
}
