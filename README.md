# sekiro_crashfix

A small `dinput8.dll` that fixes the random crash of **Sekiro: Shadows Die Twice** (Steam, version 1.6.0.0) that some PCs get every few minutes, very often during loading screens.

*Version française plus bas.*

## Symptoms

The game closes to the desktop without any message, typically after 5 to 20 minutes, often while loading (fast travel, death, entering an area). The Windows Event Viewer (Windows Logs → Application) shows an error like:

```
Faulting application name: sekiro.exe, version: 1.6.0.0
Faulting module name: sekiro.exe, version: 1.6.0.0
Exception code: 0xc0000005
Fault offset: 0x0000000001aa564f
```

The fault offset is usually `0x1aa564f`, `0x1aa566d` or `0x1aa573b`. Sometimes the faulting module is `ntdll.dll` instead (a crash inside `RtlpWaitOnCriticalSection`). Other games run fine, and verifying the game files, updating drivers, disabling overlays or removing mods doesn't help.

If that matches, this DLL is for you.

## Install

1. Download `dinput8.dll` from the [Releases](../../releases) page (or build it yourself, see below).
2. Put it in the game folder, next to `sekiro.exe`. In Steam: right-click Sekiro → Manage → Browse local files.
3. Launch the game from Steam as usual.

A file `sekiro_crashfix.log` appears in the same folder. Its second line should say `sekiro_crashfix 1.0.0 active`.

**Uninstall:** delete `dinput8.dll`. Nothing else is modified, on disk or in your save.

## Compatibility

- Sekiro **1.6.0.0, Steam**. On any other executable the DLL checks the code it is about to patch, finds it different, writes `ERROR: game code never found, fix NOT applied` to the log and does nothing else.
- It can't be used together with another mod that is also a `dinput8.dll` (for example Sekiro Mod Engine), since Windows loads only one file with that name.
- Sekiro has no online mode, so there is no ban risk.

## What it does

The crash comes from a use-after-free in the game: some code still decrements the last field of a 0x258-byte object up to about half a second after that object has been destroyed. In the game's memory allocator, that field becomes part of the allocator's own bookkeeping as soon as the object is freed. The decrement corrupts it, and the game crashes a bit later, somewhere unrelated. When the memory has already been reused by another object, that object gets damaged instead, which crashes too.

The DLL:

1. **Delays the release of objects of that size by 10 seconds** (quarantine). The late decrement lands in dead memory that still belongs to the dead object and does no harm. This addresses the root cause.
2. **Checks every free before the game performs it**, as a safety net. A corrupted neighbour pointer is repaired when the correct value can be proven, and a free that would corrupt the heap is skipped (a few hundred bytes are leaked instead).
3. Turns the game's own "block already freed" checks, which do nothing in the retail build, into a clean exit.

The game logic isn't changed and no file on disk is modified. The full analysis is in [docs/TECHNICAL.md](docs/TECHNICAL.md).

### Why only some players?

The bug is a timing issue between the game's threads. On the PC where it was investigated (16 logical CPUs), the game crashed after about 5 minutes with all cores and after about 11 minutes when limited to 8. CPUs with many cores make the bad timing more likely. That is a plausible explanation, not a proven one.

## The log file

`sekiro_crashfix.log` is written next to `sekiro.exe`, and you can delete it at any time. Every 10 minutes it prints a status line:

```
alive: frees=4791184 quarantined=42 q_total=1327 q_released=1285 repaired=0 skipped=0 game_checks=0
```

- `quarantined`, `q_total`, `q_released`: objects currently held, held in total, and released.
- `repaired`: corruptions repaired by the safety net. Each one is a crash that would have happened.
- `skipped`: frees refused because they would have corrupted the heap.
- `game_checks`: invalid frees detected by the game's own checks.

If the game ever crashes with the DLL installed, the log ends with an `=== EXCEPTION` report (registers, call stack, last frees). Please attach it to an issue.

## Results

Tested on one PC (16 logical CPUs, Windows 11, NVIDIA GPU) that crashed every 5 to 19 minutes:

- with only the safety net: 2 hours of play, 22 corruptions repaired, no crash;
- with the quarantine: no corruption left to repair, and no crash.

Reports from other machines are welcome.

## Building

The DLL is written in C without the C runtime and needs only LLVM, not Visual Studio or the Windows SDK.

- Windows: install [LLVM](https://github.com/llvm/llvm-project/releases), then run `build\build.bat`.
- Linux / WSL / macOS: install `clang`, `lld` and `llvm` (for `llvm-dlltool`), then run `./build/build.sh`.

The result is `out/dinput8.dll`.

## Tests

The heap validation and quarantine logic can be tested on a simulated copy of the game's heap layout, on Linux, WSL or macOS:

```
./tests/run_tests.sh
```

## How this was made

This fix was developed with the help of an LLM (Claude, by Anthropic). The analysis of the crash dumps, the reverse engineering of the game's allocator and the code of the DLL were done in a conversation with the model. I ran the game, collected the crash dumps and WinDbg output, and tested each version of the DLL over several hours of play. The results above come from those tests.

## Disclaimer

Not affiliated with FromSoftware, Activision or Valve. Use at your own risk. Backing up your save (`%AppData%\Sekiro`) from time to time is always a good idea.

License: MIT.

---

## En français

Une petite `dinput8.dll` qui corrige le crash aléatoire de **Sekiro** (Steam, 1.6.0.0) qui survient sur certains PC toutes les quelques minutes, souvent pendant les chargements, avec une erreur `0xc0000005` à l'offset `0x1aa564f`, `0x1aa566d` ou `0x1aa573b` dans l'Observateur d'événements.

**Installation :** copier `dinput8.dll` à côté de `sekiro.exe`, puis lancer le jeu depuis Steam. **Désinstallation :** supprimer le fichier.

**Cause :** un bout de code du jeu décrémente encore le dernier champ d'un objet de 0x258 octets jusqu'à une demi-seconde après sa destruction. Ce champ fait alors partie des données internes de l'allocateur mémoire du jeu, qui se retrouve corrompu, et le jeu plante plus tard. La DLL garde ces objets détruits 10 secondes avant de les rendre à l'allocateur, ce qui neutralise la décrémentation tardive. Elle vérifie aussi chaque libération mémoire, en filet de sécurité. Détails dans [docs/TECHNICAL.md](docs/TECHNICAL.md) (en anglais).

Ce correctif a été développé avec l'aide d'un LLM (Claude, d'Anthropic), qui a analysé les dumps, étudié l'allocateur du jeu et écrit le code. J'ai fourni les dumps et les sorties WinDbg, et testé chaque version en jeu.
