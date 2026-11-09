# ServerPerformance (ASE)

An [ArkServerApi](https://github.com/ArkServerApi/AseApi) plugin for ARK: Survival Evolved servers. It sets engine
**console variables** from its config and confirms with the engine that each one took, requests **garbage
collections** on a schedule, can lower how often **structures replicate**, and reports the server's real
**tick rate**, UObject counts, dino and structure counts and memory, on demand and in a rotating stats log.

## How it works

There are no gameplay hooks. Everything runs from ArkServerApi's one-second timer and the commands, on the
game thread.

- **Console variables.** Once the server has a world loaded, each entry under `ConsoleVariables` is set
  through the engine's own console-command handling. The plugin then reads the variable back and logs one
  of: `name = value (was old)`, `name = value (already set)`, or `name = value not applied: ...` with the
  reason (the name doesn't exist in this game build, or the engine refused or kept another value). Every
  `ConsoleVariables_Reapply_Seconds` the plugin reads them again and sets back any that something else has
  changed. The value each variable had before the plugin first changed it is remembered: removing it from
  the config and reloading, or unloading the plugin, puts that value back.
- **Timed garbage collection.** Every `AutoGC_Interval_Minutes` a collection is requested from the engine
  (`UWorld::ForceGarbageCollection`), which runs it at its next safe point in the frame, as it does its
  own scheduled collections. Nothing is collected from inside the plugin's timer.
- **Structures** (off by default). Every `CheckInterval_Seconds`, structures whose replication frequency
  is above `NetUpdateFrequency` are lowered to it. (`MinNetUpdateFrequency` is ASA-only and ignored here.) Frequencies are
  never raised, and classes matching `ExcludeClassesContaining` are left alone. Turning it off and
  reloading, or unloading the plugin, sets every structure back to its class default. Each pass logs how
  many it checked and lowered and how long it took.
- **Stats.** The server's tick rate is measured once a second from the engine's frame counter, so it is
  what the server really ran at, not a configured limit. `ServerPerformance.Stats` and the stats log also
  show live UObjects against the UObject array's size, dinos, structures, the process's working set and
  private memory, and the machine's free memory.
- **Network stats.** Every player connection is sampled once a second: total upload and download
  (average and peak), ping (average and worst), packet loss each way, and the share of time connections
  were bandwidth-limited (they had more to send than their rate allowed). That share is what tells you
  whether a struggling server is short of CPU or of per-client bandwidth. If it's high, the per-client
  limits (`MaxClientRate` / `MaxInternetClientRate` in `Engine.ini`) are the knob, not the engine
  variables.

Routine messages (each collection request, structure passes, stats lines) go to
`ServerPerformance_stats.log` in the plugin folder, which rotates by size. Console-variable results,
config problems and errors go to the API log.

**`gc.TimeBetweenPurgingPendingKillObjects` on ASE:** this engine version keeps the time between
collections as an engine setting, not a console variable. The plugin reads and writes that setting under
the console-variable name, so the same config works on both games. Other console variables go through the
engine's command processor as on ASA; this older engine has fewer of them, and the log says which ones it
doesn't know.

**Missing engine symbols:** ArkServerApi reads the server's PDB, and a name it doesn't find silently
resolves to the wrong address instead of failing. The plugin therefore checks every function, field and
global it uses when it loads. Anything missing is logged as `Server build has no <name>; the features that
need it are off`, and only those features stop; the rest keep working.

**Why only console variables, not any console command:** the plugin sends text to the engine's command
processor, so names and values are restricted to what a console variable looks like (letters, digits,
`.` and `_` in names, with at least one `.`; single-word values). Neither config.json nor
`ServerPerformance.CVar` can be used to run other commands.

## Build

**Shared API checkout (optional):** the project builds against `extern\AseApi` by default. To build several plugins against one shared AseApi checkout instead, set the MSBuild property `AseApiDir` to that folder, for example in a `Directory.Build.props` placed above the repo; the submodule then doesn't need to be checked out.

Requirements: Visual Studio 2026 with the "Desktop development with C++" workload (v145 toolset), and Git.

ArkServerApi is a git submodule at `extern\AseApi`. After cloning the repository:

```
git submodule update --init --recursive
```

Open `ServerPerformance.sln`, select **Release / x64**, build. `ArkApi.lib` ships in the AseApi repository at
`extern\AseApi\out_lib\`, so nothing has to be built first.

No vcpkg packages are needed: ArkServerApi bundles the fmt copy its headers use. No other libraries are
linked; process memory is read with `K32GetProcessMemoryInfo` from kernel32.

The result is `out\ServerPerformance\`, containing `ServerPerformance.dll`, its `.pdb`, `PluginInfo.json`,
`config.json`, `config.commented.json` and `THIRD-PARTY-NOTICES.md`. Keep the notices file with the DLL
when redistributing the plugin.

Build against the ArkServerApi version your servers run. To move to a different release, check out that tag
in the submodule and commit the new submodule pointer:

```
cd extern\AseApi
git fetch --tags
git checkout <tag>
cd ..\..
git add extern\AseApi
```

## Install

**VC++ runtime:** built with the v145 toolset (Visual Studio 2026) and `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`, so it also runs on servers with an older Visual C++ runtime. If the server crashes inside `std::mutex` or reports a missing `msvcp140.dll` entry point after installing, install the current Visual C++ Redistributable (x64).

The server machine needs a Microsoft Visual C++ x64 Redistributable at least as new as the Visual Studio
the plugin was built with. A machine with that Visual Studio installed already has it; otherwise install
the latest one from Microsoft.

1. Remove any other plugin that sets the same engine variables, forces garbage collection or changes
   structure replication. Two plugins setting the same variable would keep setting it back and forth (this
   plugin logs each time it has to put one back).
2. Copy the `out\ServerPerformance` folder to `ShooterGame\Binaries\Win64\ArkApi\Plugins\` on each server.
   The folder and the DLL must keep the same name.
3. Start the server. `ArkApi.log` shows `Config loaded: ...` when the plugin loads, any
   `Server build has no ...` lines, then one line per console variable once the world is up. Check those
   lines: a variable that is `not applied` is doing nothing.

To update a running server, drop the new build in as `ServerPerformance.dll.ArkApi` (the build produces that
copy too). If automatic plugin reloading is enabled in ArkApi's own `config.json`, ArkApi swaps it in: the old
version puts the original values back as it unloads and the new one applies the config again.

## config.json

Comments (`//` and `/* */`) are allowed in this file.

`configs\config.commented.json` holds the same settings and values as `config.json`, with an explanation
on every setting. It is a reference copy: the plugin only reads `config.json`.

### GarbageCollection

| Setting | Default | Meaning |
|---|---|---|
| `AutoGC_Enabled` | `true` | Request a collection every `AutoGC_Interval_Minutes`. `ServerPerformance.ForceGC` works either way. |
| `AutoGC_Interval_Minutes` | `15` | 1 to 1440, counted from when the world loaded (and from each reload). |
| `FullPurge` | `true` | `true`: everything unreachable is destroyed in that frame (one longer frame). `false`: the engine purges over the next frames instead (shorter frames, memory freed more slowly). |

### ConsoleVariables

| Setting | Default | Meaning |
|---|---|---|
| `ConsoleVariables` | see below | `"name": value` pairs, applied in name order. Values: numbers, `true`/`false` (sent as `1`/`0`) or one word. |
| `ConsoleVariables_Reapply_Seconds` | `300` | How often the variables are read back and anything changed by something else is set again. `0` = only on load and reload. |

Shipped values:

| Variable | Value | Engine default | Effect |
|---|---|---|---|
| `gc.TimeBetweenPurgingPendingKillObjects` | `20` | `60` | Seconds between the engine's own collections. Lower = smaller, more frequent collections. Written to the engine setting directly on ASE. |
| `gc.NumRetriesBeforeForcingGC` | `5` | `10` | Collections the engine may skip (object tables locked) before it forces one. |
| `gc.ActorClusteringEnabled` | `1` | `1` | GC clusters per actor. Only affects objects created after it is set. |
| `t.MaxFPS` | `30` | none | Server frame-rate cap. `0` = no cap. |

The `gc.NumRetriesBeforeForcingGC`, `gc.ActorClusteringEnabled` and `t.MaxFPS` entries are Unreal
Engine 5 variables; ASE's older engine may not have them, in which case the log shows them as `not found`
and they do nothing. They stay in the shipped file so it is the same as the ASA one; remove them from your
ASE servers' copy to keep the log clean.

**Optional variables, shipped switched off.** `config.commented.json` has these as commented-out lines
ready to try (remove the `//`, copy into `config.json`, reload). They are for ASA's Unreal Engine 5; ASE's
older engine probably lacks most of them, and the log will say `not found` for any it doesn't have:

| Variable | Suggested | What it does |
|---|---|---|
| `net.UseAdaptiveNetUpdateFrequency` | `1` | Actors that aren't changing are checked for updates less often, down to their minimum rate. Less replication CPU with many actors. Also what makes `Structures.MinNetUpdateFrequency` do anything. |
| `gc.LowMemory.MemoryThresholdMB` | `16384` | Below this much free memory, the engine collects at the low-memory interval instead. As far as can be told from engine source this is the machine's free memory (shared by all servers on the box), the "machine ... free" figure in Stats. |
| `gc.LowMemory.TimeBetweenPurgingPendingKillObjects` | `10` | Seconds between collections while in low-memory mode (engine default 30). |
| `gc.AllowIncrementalReachability`, `gc.AllowIncrementalGather`, `gc.IncrementalReachabilityTimeLimit` | `1`, `1`, `0.002` | **Experimental in the engine.** Spreads each collection's marking over several frames to remove GC stutter. Epic warns that game code holding object references the old way can have objects collected while still in use; ARK and every mod would have to comply. Test server only, with the full mod list, watching for crashes, vanishing items or dinos, or tames turning wild. |

Any other console variable can be added. Some can only be set from an ini before the engine starts (the
engine reports them as read-only, and the plugin logs `not applied`); those belong in `Engine.ini`
instead. The plugin's log line tells you which case you're in, so it is safe to try.

### Structures

| Setting | Default | Meaning |
|---|---|---|
| `Enabled` | `false` | Turn the structure pass on. Test on a test server first: a lowered structure's changes reach players later. |
| `NetUpdateFrequency` | `0.2` | Updates per second for structures above this (0.2 = every 5 s). 0.01 to 100. |
| `MinNetUpdateFrequency` | `0.1` | ASA only; ignored on ASE (kept in the file so both games share one config). |
| `CheckInterval_Seconds` | `300` | Seconds between passes (picks up newly placed structures). 10 to 86400. |
| `ExcludeClassesContaining` | turrets, doors, gates, elevators, lifts, teleporters, transmitters | Structures whose class path contains any of these (not case-sensitive) are left alone. |

### Stats

| Setting | Default | Meaning |
|---|---|---|
| `LogInterval_Seconds` | `300` | Seconds between stats lines in the stats log. Each line's tick figures cover the time since the previous one. `0` = no lines (the command still works). |
| `AlsoWriteToApiLog` | `false` | Also write routine messages (stats lines, collections, structure passes) to the API log. |
| `CountDinosAndStructures` | `true` | Include dino and structure counts (one object lookup of each, only when stats are taken). |
| `IncludeNetwork` | `true` | Sample every player connection once a second for the network figures (a few field reads per player). |
| `LogFileMaxSize_MB` | `5` | Size at which the stats log starts a new file. |
| `LogFilesKept` | `3` | Old stats log files kept. |

## Commands

All on RCON, or in the in-game console as an admin (`cheat ServerPerformance.Stats`).

| Command | What it does |
|---|---|
| `ServerPerformance.Stats` | Uptime, players, tick rate (average/min/max since the last stats line), UObjects (live, array slots used, array size, objects kept outside GC), dinos, structures, memory, network (upload/download, ping, packet loss, bandwidth-limited share), forced collections, and the current engine value of each configured variable. |
| `ServerPerformance.Stats json` | The same as one line of JSON, for scripts and monitoring tools. |
| `ServerPerformance.CVar <name>` | The engine's current value of any console variable. |
| `ServerPerformance.CVar <name> <value>` | Sets it now and confirms the engine took it. Not saved: add it to `ConsoleVariables` to keep it. A variable that is also in the config goes back to the config value on the next reapply. |
| `ServerPerformance.CVars` | Each configured variable: wanted value, engine value, status (`applied`, `rejected`, `not found`), the value it had before, and the engine's reason if refused. |
| `ServerPerformance.ForceGC [full\|quick]` | Requests a collection now (default: `FullPurge` from the config). |
| `ServerPerformance.Reload` | Re-reads `config.json`. If the file has a mistake, the old settings stay and the error is reported. Variables removed from the config go back to their earlier values. |

**Tuning a variable:** read it with `ServerPerformance.CVar <name>`, set a trial value with
`ServerPerformance.CVar <name> <value>`, watch `ServerPerformance.Stats` (tick rate, UObjects, memory) for
a while under normal load, and put the value you settle on in `config.json`.

## Troubleshooting

- **`Server build has no <name>; the features that need it are off`.** The server's PDB doesn't have that
  function, field or global under that name. Only the features that need it are off. Update the name in
  `src\EngineBridge.h` if the game renamed it.
- **`Config not loaded: ...` at startup.** The message says what is wrong. Fix the file and run
  `ServerPerformance.Reload`. Until then nothing is changed.
- **`<name> = <value> not applied: not a console variable in this game build`.** The name is misspelled
  or this engine build doesn't have it. `ServerPerformance.CVar <name>` gives the same answer on demand.
- **`... not applied: the engine kept ...`.** The engine refused the value, usually because the variable
  is read-only at runtime or was set by a higher-priority source (command line, device profile). Set it in
  `Engine.ini` instead, or remove it from the config.
- **`<name> was changed to ... outside this plugin; setting it back`.** Something else sets the same
  variable: another plugin, a mod, or the game itself. Find it and keep only one, otherwise they keep
  overriding each other.
- **`The UObject array does not look like the server API headers this plugin was built with`.** The
  plugin was built against a different ArkServerApi version than the server runs (the array's layout changes
  between game versions). Before counting, it checks that sampled objects carry their own slot number and
  stops if they don't, so the UObject figures show `?` instead of wrong numbers. Everything else keeps
  working. Rebuild against the ArkServerApi version on the servers.
- **`Server build has no UNetConnection.<field>; that network figure shows as ?`.** This engine version
  doesn't have that connection statistic. The other network figures still work.
- **Network shows a high bandwidth-limited share.** Players' connections are hitting their per-client rate
  limit, so updates queue up and players see rubber-banding or late updates. Raising `MaxClientRate` /
  `MaxInternetClientRate` in `Engine.ini` helps if the server's upload has room (compare the upload average
  and peak with your line). Lowering what is sent (the structure pass, adaptive replication) helps either way.
- **Tick rate lower than `t.MaxFPS`.** The server can't keep up at that rate under the current load; the
  cap is a ceiling, not a target.

## Tests

`tests\logic_tests.cpp` covers config parsing and warnings, console-variable name and value checks, reading
the engine's replies, value comparison, tick-rate and network sampling and the stats text. It needs no game and no
Windows. From a Visual Studio developer prompt in this folder:

```
cl /std:c++20 /EHsc /I extern\AseApi\version\Core\Public /I src tests\logic_tests.cpp
logic_tests.exe configs\config.json
```

Passing a config file validates it and prints any warnings, which is a quick way to check an edit before
putting it on a server.

Add `--shipped` after the path to also check the file against the values expected from the shipped
config. Run it on both shipped files after changing either, since they must hold the same values:

```
logic_tests.exe configs\config.json --shipped
logic_tests.exe configs\config.commented.json --shipped
```

## Files

| File | Purpose |
|---|---|
| `src\Main.cpp` | Timer, console-variable apply/reapply/restore, timed GC, structure pass, stats, commands. |
| `src\EngineBridge.h` | Engine access for ArkServerApi: running console commands and capturing their output, the purge-interval engine setting, GC requests, frame counter, UObject array, process memory, and the per-feature symbol check. |
| `src\PerfLogic.h` | Config parsing, name/value checks, reply parsing, tick-rate window, stats text. No game dependencies. |
| `configs\config.json` | Default configuration. |
| `configs\config.commented.json` | The same configuration with every setting explained. Not read by the plugin. |
| `configs\PluginInfo.json` | Plugin metadata read by ArkServerApi. |
| `..\THIRD-PARTY-NOTICES.md` | License notices for nlohmann/json, spdlog and the fmt copy bundled with spdlog, compiled into the DLL. Copied into the build output. |
| `tests\logic_tests.cpp` | Stand-alone tests for `PerfLogic.h` and the shipped config. |
| `extern\AseApi` | ArkServerApi submodule the plugin builds against. |
