# Styx: Master of Shadows – Engine Fix (High FPS)

Play *Styx: Master of Shadows* (Unreal Engine 3, 64-bit) above its 62 fps cap **without ragdolls flying
away**, and pick your own frame rate limit. A standalone `dinput8.dll`, no ReShade needed.

Player downloads are on [Nexus Mods](https://www.nexusmods.com/styxmasterofshadows/mods/10). Companion mod:
[Styx Upscaler](https://www.nexusmods.com/styxmasterofshadows/mods/9) ([source](https://github.com/MasterBerkay/styx-upscaler)) – FSR 4 / DLSS for the game.

## The bug

UE3 steps the PhysX 2 scene once per frame: `setTiming(dt / N, N, NX_TIMESTEP_FIXED)` followed by
`simulate(dt)`. When a pawn turns into a ragdoll its bodies start out interpenetrating each other and the
floor, and PhysX resolves that with a velocity of roughly *penetration / step*. At 360 fps the step is ~6×
smaller than at the 62 fps the game was tuned for, so bodies are pushed ~6× harder and fly away.
(The game's own ragdoll blend, `AStyxPawn_BaseNative::UpdateRagdoll`, is correctly frame-time based.)

## The fix

When `PhysXCore64.dll` is loaded, the `NpScene::setTiming` entry of the NpScene vtable is replaced. A
requested step smaller than `1 / PhysicsRate` is raised to it (and `maxIter` to at least 8). PhysX then
accumulates frame times and simulates in those fixed steps, so the physics behave as at 62 fps while
rendering and input run at the full frame rate. The hook only installs on the exact PhysXCore64.dll build
shipped with the game (slot and target are verified).

The frame rate limit is written to `[Engine.Engine] MaxSmoothedFrameRate` in the user's `StyxEngine.ini`
when the DLL loads. `dinput8.dll` is a static import of `StyxGame.exe`, so this happens before the engine
reads its config. All DirectInput exports are forwarded to the system `dinput8.dll`.

## Building and installing

Visual Studio 2022 Build Tools (x64): `build.bat` → `bin\dinput8.dll`. No dependencies.

Copy `bin\dinput8.dll` and `dist\StyxEngineFix.ini` next to `Binaries\Win64\StyxGame.exe`:

```ini
[Framerate]
FpsLimit=144     ; 0 = unlimited (game default 62)
[Physics]
PhysicsRate=62   ; physics steps per second, 0 = off
```

A log is written to `StyxEngineFix.log` next to the DLL.

## License

MIT (see `LICENSE`). Styx: Master of Shadows is © Cyanide / Focus Entertainment; no game files are
included. Not affiliated with Cyanide, Focus Entertainment or NVIDIA (PhysX).
