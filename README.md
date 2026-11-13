# P4G — Persona 4 Golden for Android

**0.5.0 Beta** · ARM64 · Vulkan · English app interface

P4G is an unofficial Android port of the Nintendo Switch version of **Persona 4 Golden**. It uses native ARM64 execution and a compatibility runtime that translates the game's NVN graphics commands to Vulkan.

Game preparation runs directly on the phone. Select your own game dump and keys, prepare the game, and launch it from the main menu.

Discord:https://discord.gg/yF6FtNK6rM
[Downloads](https://github.com/nxdevxz/p4g-android/releases) · [Builds](https://github.com/nxdevxz/p4g-android/actions/workflows/build-p4g.yml) · [Report an issue](https://github.com/nxdevxz/p4g-android/issues)

## Current status

The project is in **Beta**. Gameplay, battles, cutscenes and audio have been tested during development. The reported graphics orientation problems have been corrected, and the 30 FPS limiter has been tested without a noticeable change in game speed.

A full playthrough and broad device compatibility have **not** been verified. Performance and stability depend on the device, driver and game revision.

## Features

- On-device preparation from an NSP or XCI game dump.
- Local Maxwell-to-SPIR-V generation, including resource mappings. No game shader pack is included in the APK.
- Automatic preparation checks when opening the app.
- Separate preparation screen and main menu.
- English app interface, including preparation and exit confirmation.
- Resolution, 16:9/16:10/stretch screen formats and FPS settings.
- On-screen controls with adjustable size, opacity, position and sensitivity.
- Save import and export, with backups before importing.
- Translation and CPK mod import.
- Confirmation when using the phone's Back button or gesture during gameplay.
- Return to the main menu after confirming that you want to exit the game.

## Requirements

- An **ARM64 Android device** with a Vulkan 1.1-capable GPU.
- **Android 11 or newer** for the current build configuration, which targets API 30 for DEX generation.
- Your own Persona 4 Golden Nintendo Switch game dump, in **NSP or XCI** format.
- Your own **prod.keys** file; some dumps may also require **title.keys**.
- Enough free storage for the game dump, prepared files, shaders and saves.

Minimum RAM and GPU performance requirements have not been established across devices. An ARM64 processor and Vulkan support alone do not guarantee compatibility.

The runtime accepts the base game (1.0.0, Build ID `C433D980688380A6`) and the official 1.0.1/1.0.2 update executable (Build ID `66B2B6D53549A636`). Unknown revisions are rejected before launch.

## Installation and first launch

1. Download **P4G.apk** from a published release or a completed GitHub Actions build.
2. Install the APK and open **P4G**.
3. Grant the requested file access permission.
4. Select the base game's **NSP/XCI** file, your **prod.keys** file and, optionally, the official update **NSP**.
5. Tap **Prepare** and wait for preparation to finish. When an update is selected, its ExeFS and patched RomFS are installed atomically over the base dump.
6. To update an existing installation later, open the separate **Game update** section, choose the original base game and update NSP, then tap **Apply update**. The launcher explains why the original container is needed again; a failed or cancelled preparation keeps the previous installation playable.
6. A **Preparation complete** message appears, and the app returns to the main menu automatically.
7. Adjust your settings and tap **Play**.

Keep the selected game file available in its original location. The runtime reads it when starting the game.

If preparation data or generated shaders are missing or invalid, the app returns to the preparation screen. Preparation scans the game modules and tests the compiler. Additional shaders are generated on first use, which may briefly pause gameplay. Do not manually copy an old shader pack. See [local shader generation](docs/p4g-local-shaders.md) for details.

### Updating

Install the new APK over the existing installation using a build signed with the **same signing key**. The package name remains `io.github.p4grecomp`.

Back up your saves before updating. If Android rejects the update, check that both APKs use the same signing key before uninstalling anything.

## Main menu

| Setting | Purpose |
|---|---|
| Output resolution | Select 360p, 480p, 720p or 1080p. |
| Fullscreen (stretch) | Fill the screen when enabled; preserve the game image's aspect ratio when disabled. |
| FPS limit | Choose 30 FPS, 45 FPS or the option labeled 60 FPS. |
| FPS counter | Show or hide the frame-rate counter. |
| Pipeline prewarming (experimental) | Background is the default. Aggressive waits for every currently available pipeline learned in earlier sessions, trading longer loading for fewer later first-use stalls. Off disables prewarming. First-run pipelines cannot be predicted. |
| On-screen controls | Show or hide the touch controls. |
| Button size | Adjust touch button size. |
| Button opacity | Adjust touch button opacity. |
| Touch stick sensitivity | Adjust the virtual movement stick. |
| Touch camera sensitivity | Adjust touch camera sensitivity. |
| Controls position | Open the touch control layout editor. |
| Saves | Import or export save backups. |
| Disable motion blur (experimental) | Off by default. Turn On to disable the matched motion-blur effect. |
| GPU driver | Import, select and delete saved Turnip or PanVK drivers, or select System driver. |
| Mods and translations | List installed files, enable or disable mods and inspect file conflicts. |
| Translation / CPK mod | Import a translation or replacement CPK package. |
| Play | Start the game. |

**30 FPS** and **45 FPS** apply host-side frame-rate caps. **60 FPS** is the label for the existing game-default option: it does not guarantee a constant 60 FPS or override every frame-rate choice made by the game.

The English app interface does not force the game itself into English. Game text and translation mods are separate.

## Saves

The default save directory is:

```text
Internal storage/Persona 4 Golden/user/save/
```

Import a **ZIP** or a **folder** containing the actual P4G save files, for example:

```text
data0001.bin
data0001.binslot
data0002.bin
data0002.binslot
system.bin
system.binslot
```

- Include both the `.bin` and `.binslot` files for each slot you import.
- Include the system files if they are present in your backup.
- Nested backup folders are supported. If a package contains several separate save folders, select only the backup you want to use.
- Existing saves are backed up to `user/save_backup_...` before import. Slots not included in the import are preserved.
- Export creates a `P4G-saves-...zip` file in **Downloads**, which can be imported again.

The importer copies save files; it does **not** convert save formats. Use a backup compatible with this version of P4G. Importing a save does not automatically load it: select **Load Game** inside the game.

## Translation and CPK mods

Use **Translation / CPK mod → Import CPK** to install a package with its original filename, such as `data_e.cpk`, under `Persona 4 Golden/mods/translation/romfs/`. Importing the same filename replaces that file. Use a package intended for the supported game revision.

Open **Mods and translations → Manage** to inspect files, enable or disable mods and view conflicts. The last active folder in alphabetical order takes priority for overlapping files. Importing into a disabled translation keeps it disabled until you enable it in the manager. Changes apply the next time you open the game.

## Exiting a game

Press the phone's **Back** button or use the Back gesture during gameplay. Choose **Keep playing** to dismiss the dialog, or **Exit** to end the session and return to the main menu.

Exiting does not automatically save your progress. Save inside the game first.

## Troubleshooting

| Problem | What to check |
|---|---|
| Preparation screen appears again | Confirm that the game file is still readable and that preparation files and required shaders have not been removed. |
| Preparation fails | Check the selected game revision, keys, file access and available storage. |
| Imported save does not appear | Import the matching `.bin` and `.binslot` pair, then open Load Game. |
| APK will not update | Verify the signing key and installed package. |
| Frame rate is low | Try a lower resolution. A frame-rate limit cannot increase performance below that limit. |
| Old app icon remains | Refresh or restart the launcher; some launchers cache icons after an update. |

When reporting a bug, include your device model, Android version, GPU, game revision, reproduction steps and screenshots or video when useful. Include the relevant **nx.log** from the game's data directory when available.

Do not attach game dumps or encryption keys to public bug reports.

## Building

1. Fork or clone this repository.
2. Configure the repository secrets `P4G_SIGNING_KEY_B64` and `P4G_SIGNING_PASSWORD` required by `.github/workflows/build-p4g.yml`.
3. Open **Actions → P4G Beta → Run workflow**.
4. Wait for a successful build.
5. Download the **P4G-0.4.6-Beta** artifact and extract **P4G.apk**.

The workflow builds the native runtime and preparation libraries, compiles the Java interface, runs the included checks and packages the APK. A successful build does not replace testing on an Android device.

## Project structure

| Path | Purpose |
|---|---|
| `games/p4g/java/` | Launcher, preparation, controls editor and save management. |
| `games/p4g/import/` | Game preparation and shader-processing components. |
| `tools/maxwell/spirv_harness/` | Source-built Maxwell shader compiler used on the device. |
| `runtime/src/nce/` | Native ARM64 execution. |
| `runtime/src/nvn/` | NVN-to-Vulkan graphics runtime. |
| `runtime/src/hle_audio.cpp` | Audio compatibility. |
| `android/res/` | Android resources and launcher icon. |
| `tests/` | Regression checks. |

## Credits and license

This project builds on the compatibility runtime and native port work of its original contributors. Preserve the existing copyright notices and attribution when redistributing source code.

The repository's source-code license is available in [LICENSE](LICENSE). Third-party components retain their respective licenses. Game content, character artwork and trademarks are not covered by the source-code license.

Persona 4 Golden and related trademarks belong to their respective rights holders, including ATLUS and SEGA. The launcher uses a custom typographic icon.

This is an unofficial fan project and is not affiliated with or endorsed by ATLUS, SEGA or Nintendo. Game dumps and encryption keys are not distributed with this project; users must supply their own.
