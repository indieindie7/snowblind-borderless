# snowblind-borderless

Borderless fullscreen for **Project: Snowblind** (PC, 2005, Direct3D 9). A tiny `d3d9.dll` proxy:
the game asks for exclusive fullscreen, the proxy turns it into a windowed device and makes the
game window a border-less popup covering the monitor. Alt-tab is instant, no mode switch, no
"game freezes after tabbing out".

## Install
1. Build (`build.bat`, needs Visual Studio with the x86 toolset) or take `d3d9.dll` from a release.
2. Copy `d3d9.dll` next to `Snowblind.exe` (Steam: `steamapps\common\Project Snowblind`).
3. Steam version only: if the game shows "Failed to find Steam", copy `steam.dll` from the Steam
   folder (`C:\Program Files (x86)\Steam\steam.dll`) next to `Snowblind.exe` too.
4. Pick the render resolution in the game's setup (`Snowblind.exe -setup`); the backbuffer is
   stretched to the monitor, so a 4:3 mode is stretched to 16:9 (same as the dgVoodoo route). Use
   the community Widescreen Patch for a real 16:9 field of view.

Remove `d3d9.dll` to go back to exclusive fullscreen.

## How it works
`Direct3DCreate9` is wrapped; `IDirect3D9::CreateDevice` and `IDirect3DDevice9::Reset` (vtable
slot 16) get `Windowed = TRUE`, refresh rate 0, 16-bit back buffers promoted to X8R8G8B8, and the
device window restyled to `WS_POPUP` at the monitor rectangle. `Present` (slot 17) is hooked only
for the frame counter in the log. All other exports forward to the system `d3d9.dll`.

## Logging
Create an empty `snowblind-borderless.log` next to the exe; the proxy appends what it changed
(present params, window rect, device result, a line every 200 frames). Delete the file to disable.

## Notes
- The real game code lives in `Snowblind.SP` / `Snowblind.MP`; `Snowblind.exe` is the Steam DRM
  loader. Both modules import `d3d9.dll`, so the proxy in the exe folder is picked up.
- Launching through Steam's "Play" dialog or the exe directly both work (Steam must be logged in).

## Sound on Windows 10/11

**Menus silent** (intros have audio): install a DirectSound wrapper. [IndirectSound](https://www.indirectsound.com/) 0.20
(copy its `dsound.dll` + `dsound.ini` next to `Snowblind.exe`, set `milesCompatibility = true`) or
[DSOAL](https://github.com/kcat/dsoal) (`dsound.dll` + `dsoal-aldrv.dll`) both fix the menus.

**Levels silent** (menus fine, no music or effects in gameplay): fixed by this proxy. The engine
streams all level audio through one 11776-byte DirectSound buffer (flags `0x000140e0`), fills it
with real samples, but only ever calls `SetVolume(-10000)` (mute) on it. The proxy hooks
`DirectSoundCreate(8)` (via `LoadLibraryA`) -> `IDirectSound::CreateSoundBuffer` (vtable slot 3)
-> `IDirectSoundBuffer::SetVolume` (slot 15) and keeps that one buffer at 0 dB; the in-game
SFX/Music sliders still work because the mix already carries them. Confirmed on Windows 11 with DSOAL.
Ruled out along the way: wrapper choice, vsync, one-core affinity, VRAM/RAM caps, speaker config,
the `game.ini` volume letter.

## Alt-Tab and the Steam overlay

The game sets its window topmost (WS_EX_TOPMOST) on its own after the device is created, so without help every other window ends up behind the game and Alt-Tab looks broken. The proxy turns HWND_TOPMOST into HWND_NOTOPMOST in SetWindowPos and strips the flag on a 500 ms timer; the log shows `timer: topmost set by game -> cleared`. The game pauses while another window is active and resumes on Alt-Tab back.

Do not add d3d9 vtable hooks beyond Reset (slot 16): Steam's overlay (`gameoverlayrenderer.dll`) hooks Present/EndScene/swapchain Present too, and both patching the same slots crashes the game a few seconds after a Steam launch (c0000005 in Snowblind.exe). Direct launches of the exe (no overlay) hide that bug. DirectInput is already opened non-exclusive by the game, so a dinput8 proxy is not needed (`dinput8.cpp` is kept only as a diagnostic).

## Extras (since 2026-10-18)

- **Fast-forward cinematics:** in an in-engine cinematic (you can look around but not move), hold Esc to run the game clock 8x. The level audio is muted while you hold it, so speech doesn't drift out of sync. A short tap still opens the pause menu. Outside cinematics Esc works as normal. The proxy reads the game's own flag for this: byte +0x1d of the object at `*0x1023308` in Snowblind.SP, which is set at 0x49bbc0 and cleared at 0x49bc60.
- **Keeps running unfocused:** the game no longer pauses when you alt-tab. Keyboard and mouse input is ignored while another window has focus.
- **Playtest marks:** create a `marks` folder in the game folder to turn this on. F9 saves a screenshot to `marks/NNN.png` and opens a note box (Enter saves, Esc cancels). Notes go to `marks/marks.jsonl`, in the same format as [playtest-marks](https://github.com/indieindie7/playtest-marks). The editor has pen, arrow, text and Pick tools: Pick shows the names of the objects under the cursor (from the game's string table), and the mark records the picked objects, the camera (position, angles) and nearby objects.
- **Level sound:** if levels are silent while the menu has sound, Windows may have stored a mute for `Snowblind.SP` in the volume mixer. The proxy un-mutes its own audio session while the game runs (log: "audio session was muted in the Windows mixer -> un-muted"). Create a file `keep-mute` in the game folder to turn this off.
- **No aiming reticle:** the reticle is removed. Create a file `show-reticle` in the game folder to keep it.
- **FPS graph:** the last 240 frame times are drawn in the top-left corner, with the current, average and worst frame rate. F10 toggles it.

## Widescreen (community Widescreen Patch 1.0 by Felix Krull, MIT)

The patch is itself a `d3d9.dll` wrapper (source: bitbucket.org/fk/snowblind-widescreen-fix,
mirror: gamepressure.com "Project Snowblind Widescreen Patch", zip `snowblindwidescreenfix10.zip`).
It cannot sit next to this proxy under its own name, so this proxy chain-loads it:

1. Put this proxy in the game dir as `d3d9.dll`.
2. Put the patch's `d3d9.dll` in the game dir as **`d3d9_widescreen.dll`** (it loads the
   system d3d9 itself, so there is no recursion).
3. Copy `snowblind-widescreen.ini` next to it; set `windowed=1` so the patch agrees with the
   windowed device this proxy creates. `width=0`/`height=0` = desktop resolution; `fovPercent`
   adjusts FOV.
4. The VC++ 2013 x86 runtime (`msvcr120.dll`) must be installed (the zip ships `vcredist_x86.exe`).

The log shows `d3d9_widescreen.dll -> <addr>` when the chain worked; without the file the
proxy falls back to the system d3d9. Known patch limits: HUD stays in the centre 4:3 area,
cutscene bars do not resize.
