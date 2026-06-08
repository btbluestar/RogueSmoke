// DebugConfig.as
// Single switch for all RogueSmoke debug drawing. Flip bEnabled to false and SAVE to turn
// every debug visualization off at once (hot-reloads). The fork requires global variables to
// be const, so this is a compile-time switch rather than a live one — for a runtime toggle
// we'd back it with a console variable. Per-actor bShowDebug flags still apply on top.
namespace RaidDebug
{
    const bool bEnabled = true;
}
