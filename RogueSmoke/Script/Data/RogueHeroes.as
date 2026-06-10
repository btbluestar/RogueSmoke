// RogueHeroes.as
// The shared hero roster: display data for the lobby's hero select, by index. The PAWN class
// for each index lives on BP_RaidGamemode.HeroPawnClasses (content reference — BPs carry asset
// wiring, script carries logic/data; CODING_STANDARDS §6). INDEXES MUST MATCH that array:
//   0 = Vanguard (setup), 1 = Bombardier (payoff).
// ARoguePlayerState.SelectedHeroIndex replicates a player's pick as an index into this roster.

struct FRogueHeroEntry
{
    FString Name;
    FString Role;        // SETUP / PAYOFF — keeps the synergy pairing legible in the lobby
    FString Blurb;
    FLinearColor Color;
}

namespace RogueHeroes
{
    int Num()
    {
        return 2;
    }

    FRogueHeroEntry Get(int Index)
    {
        FRogueHeroEntry Entry;
        if (Index == 0)
        {
            Entry.Name = "VANGUARD";
            Entry.Role = "SETUP";
            Entry.Blurb = "Taunt pulls enemies into a cluster.\nAnchor the fight; make the shot.";
            Entry.Color = FLinearColor(0.27, 0.84, 0.77);   // teal
        }
        else if (Index == 1)
        {
            Entry.Name = "BOMBARDIER";
            Entry.Role = "PAYOFF";
            Entry.Blurb = "Barrage devastates clustered enemies.\nCash in the Vanguard's setup.";
            Entry.Color = FLinearColor(1.0, 0.55, 0.15);    // orange
        }
        return Entry;
    }

    bool IsValidIndex(int Index)
    {
        return Index >= 0 && Index < Num();
    }
}
