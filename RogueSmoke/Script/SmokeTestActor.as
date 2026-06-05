// SmokeTestActor.as
// SETUP §5.1 — the FIRST thing to validate. Place one in a level, hit Play, confirm
// the on-screen Print. Then edit Message, save, and confirm hot-reload updates it live
// WITHOUT restarting PIE. If that loop doesn't work, stop and fix the toolchain before
// writing any gameplay (SETUP.md / MVP arch §9).
class ASmokeTestActor : AActor
{
    UPROPERTY(EditAnywhere, Category = "SmokeTest")
    FString Message = "AngelScript OK - edit me and save to test hot-reload";

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        Print(Message, 10.0);
    }
}
