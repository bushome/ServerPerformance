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
- **Garbage collection.** The shipped config tunes the engine's own collection schedule through
  `ConsoleVariables` (see [Garbage collection on ARK](#garbage-collection-on-ark)). The plugin can also
  request collections itself every `AutoGC_Interval_Minutes` (`UWorld::ForceGarbageCollection`; off by default),
  which the engine runs at its next safe point in the frame. Nothing is collected from inside the
  plugin's timer.
- **Hitch log.** Every second in which the server runs fewer than `HitchThreshold_TicksPerSecond` frames
  is written to the stats log the moment it ends, e.g. `Hitch: 16 frames in 7.3 s (2.2 per second)`. A
  freeze shows as one line with its length, so it can be matched against the server log.
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
the console-variable name. Other console variables go through the
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
| `AutoGC_Enabled` | `false` | Request a collection every `AutoGC_Interval_Minutes`. Off because tuning the engine's own schedule works better (see below). `ServerPerformance.ForceGC` works either way. |
| `AutoGC_Interval_Minutes` | `15` | 1 to 1440, counted from when the world loaded (and from each reload). |
| `FullPurge` | `false` | `true`: ask for everything unreachable to be destroyed in one frame. `false`: ask for it to be spread over the next frames. On ASA ARK's server does a full purge on every collection anyway, so this makes no practical difference there. |

### ConsoleVariables

| Setting | Default | Meaning |
|---|---|---|
| `ConsoleVariables` | see below | `"name": value` pairs, applied in name order. Values: numbers, `true`/`false` (sent as `1`/`0`) or one word. |
| `ConsoleVariables_Reapply_Seconds` | `300` | How often the variables are read back and anything changed by something else is set again. `0` = only on load and reload. |

Shipped values:

| Variable | Value | Engine default | Effect |
|---|---|---|---|
| `gc.TimeBetweenPurgingPendingKillObjects` | `300` | see `ServerPerformance.CVar` | Seconds between collections. An engine setting on ASE, not a console variable; the plugin writes it under this name. Its effect on ASE has not been measured the way the ASA interval was, so watch memory in `ServerPerformance.Stats` after changing it. |
| `t.MaxFPS` | `30` | none | Server frame-rate cap. `0` = no cap. |

The ASE shipped config differs from the ASA one here. ASA's collection variables
(`gc.PrimalDedicatedServerOnly...`, `...OnIdleServerMultiplier`, `gc.NumRetriesBeforeForcingGC`,
`gc.ActorClusteringEnabled`) are not in ASE's game build; setting one only logs
`not a console variable in this game build`.

**Optional variables.** The optional variables offered in the ASA config
(`net.UseAdaptiveNetUpdateFrequency`, `gc.LowMemory.*`, the incremental-reachability set) are Unreal
Engine 5 variables and are not in ASE's game build either, so the ASE `config.commented.json` does not
list them.

Any other console variable can be added. Some can only be set from an ini before the engine starts (the
engine reports them as read-only, and the plugin logs `not applied`); those belong in `Engine.ini`
instead. The plugin's log line tells you which case you're in, so it is safe to try.

### Garbage collection on ARK

Measured on a Lost Colony live save on the test server (about 1.9 million objects, 17,000 structures):

- **ARK's dedicated server uses its own interval,** `gc.PrimalDedicatedServerOnlyTimeBetweenPurgingPendingKillObjects`
  (default 720 s). Unreal's `gc.TimeBetweenPurgingPendingKillObjects` is accepted but ignored there.
- **While nobody is on, that interval is multiplied by 10,** so an empty map collects only every 2 hours
  and its memory climbs. The shipped multiplier of 1 keeps empty maps collecting; a hitch on an empty
  server bothers nobody.
- **Every collection is a full purge in one frame,** even when the engine is asked for an incremental one,
  and also whenever more than `gc.UnreachableObjectsFullPurgeThreshold` (25,000) objects are waiting.
- **Each collection costs about 0.2 s however little garbage there is** (the engine scans every live
  object), plus a little per object destroyed: about 0.25 s at 10,000 objects, 0.6 s at 160,000. So
  fewer collections beat smaller ones: at 120 s players get a 0.2 s hitch every 2 minutes. 300 s keeps
  memory flat with one small hitch every 5 minutes.
- `gc.AllowParallelGC`, `gc.CreateGCClusters` and `gc.UseDisregardForGCOnDedicatedServers` are all on
  already. The only engine option that shortens the per-collection cost itself is the experimental
  incremental reachability mode in `config.commented.json`, with the risks described there.

These settings exist only on ASA; on ASE the log shows them as `not found`.

### Engine variables reference

Every engine variable this project has looked at, **as found on ASA**. ASE runs an older engine: most of
the `gc.Primal...`, pause and incremental-reachability variables don't exist there, and the others may
have different values. Check any of them on an ASE server with `ServerPerformance.CVar <name>`.

**ARK value** is what the ASA 97.18 dedicated server
reported before this plugin changed anything (read with `ServerPerformance.CVar <name>` on the test
server); "not read" means nobody has checked it yet, so run that command to see it. **Shipped** is what
`config.json` sets; anything else is left at ARK's value. All of these can be tried live with
`ServerPerformance.CVar <name> <value>` unless marked as startup-only.

**Garbage collection: the interval**

| Variable | ARK value | Shipped | What it does | Finding |
|---|---|---|---|---|
| `gc.PrimalDedicatedServerOnlyTimeBetweenPurgingPendingKillObjects` | 720 | **300** | Seconds between collections on ARK's dedicated server. | Tuned. The one interval ARK's server actually uses. |
| `gc.TimeBetweenPurgingPendingKillObjectsOnIdleServerMultiplier` | 10 | **1** | Multiplier on the interval while no players are on. | Tuned. At 10 an empty map collects every 2 hours. |
| `gc.PrimalDedicatedServerOnlyMemoryPressureTimeBetweenPurgingPendingKillObjects` | 180 | - | Interval ARK uses when the server is under memory pressure. | Untested. |
| `gc.TimeBetweenPurgingPendingKillObjects` | 61.1 | - | Unreal's own interval. | No effect: accepted but ignored by ARK's dedicated server. |
| `gc.LowMemory.MemoryThresholdMB` | 0 (off) | - | Below this much free memory, Unreal's low-memory interval applies. | Untested. Commented example in `config.commented.json`. |
| `gc.LowMemory.TimeBetweenPurgingPendingKillObjects` | not read | - | Interval while in low-memory mode. | Untested. |
| `gc.NumRetriesBeforeForcingGC` | 10 | **5** | Collections skipped (object tables locked) before one is forced. | Kept from the old setup; effect not measured. |

**Garbage collection: how each collection runs**

| Variable | ARK value | Shipped | What it does | Finding |
|---|---|---|---|---|
| `gc.UnreachableObjectsFullPurgeThreshold` | 25000 | - | Waiting objects that force a full purge. | No practical effect: ARK does a full purge on every collection anyway. |
| `gc.IncrementalGCTimePerFrame` | 0.05 | - | Seconds per frame for an incremental purge. | No practical effect, for the same reason. |
| `gc.LowMemory.IncrementalGCTimePerFrame` | not read | - | The same, in low-memory mode. | No practical effect expected. |
| `gc.IncrementalBeginDestroyEnabled` | 1 | - | Destroy objects incrementally with a per-frame time limit. | Already on. |
| `gc.AllowParallelGC` | 1 | - | Scan objects on several threads. | Already on. Collection time also depends on the cores Process Lasso gives the server. |
| `gc.CreateGCClusters` | 1 | - | Group objects into clusters for faster scanning. | Already on. |
| `gc.ActorClusteringEnabled` | 1 | 1 | Cluster an actor's objects together. | Already on; the shipped entry changes nothing. |
| `gc.UseDisregardForGCOnDedicatedServers` | 1 | - | Keep startup objects out of every scan. | Already on (Stats shows them as "outside GC"). |
| `gc.MaxObjectsNotConsideredByGC` | startup only | - | Size of that startup pool. | Can only be set in `Engine.ini` before start. |
| `gc.MaxObjectsInGame` | startup only | - | Size of the object table (Stats shows it as the UObject capacity, 25,231,360). | Can only be set in `Engine.ini`. Plenty of headroom (about 8% used on Lost Colony). |
| `gc.AllowIncrementalReachability` | 0 | - | **Experimental.** Spread the scan over several frames. | Untested. The only option that shortens the ~0.2 s fixed cost; risky with mods. |
| `gc.AllowIncrementalGather` | not read | - | **Experimental.** Part of the same mode. | Untested. |
| `gc.IncrementalReachabilityTimeLimit` | not read | - | **Experimental.** Per-frame budget for that mode. | Untested. |

**Garbage collection: ARK's pause system**

| Variable | ARK value | Shipped | What it does | Finding |
|---|---|---|---|---|
| `gc.PauseGCTimeLimitSeconds` | 30 | - | A paused GC resumes after this many seconds. | Not tuned. Nothing found that pauses GC. |
| `gc.PauseGCFreeMemThresholdMB` | 300 | - | A paused GC resumes when free memory drops below this. | Not tuned. |
| `gc.PauseGCForceNoFullPurgeOnResume` | not read | - | Drop full-purge requests made while paused. | Untested. |
| `gc.PauseGCDeferredMapLoadTrim` | not read | - | Map-load memory trim after GC resumes. | Untested. |

**Debug only: never set these on a live server**

| Variable | What it does |
|---|---|
| `gc.CollectGarbageEveryFrame` | Collects every N frames. |
| `gc.ContinuousIncrementalGC` | Starts a new incremental collection as soon as the last one ends. |

**Server tick and replication**

| Variable | ARK value | Shipped | What it does | Finding |
|---|---|---|---|---|
| `t.MaxFPS` | 0 (no cap) | **30** | Server frame-rate cap. | Holds a steady 29.9 on Lost Colony. |
| `net.UseAdaptiveNetUpdateFrequency` | not read | - | Replicate idle actors less often. Also what makes `Structures.MinNetUpdateFrequency` do anything. | Untested. Commented example in `config.commented.json`. |
| `p.Chaos.MaxSubSteps` | does not exist | - | (Physics substeps in other games.) | Not a console variable in ASA. |

**Network limits (Engine.ini settings, not console variables)**

| Setting | Section | What it does |
|---|---|---|
| `MaxClientRate`, `MaxInternetClientRate` | `[/Script/OnlineSubsystemUtils.IpNetDriver]` and `[/script/socketsubsystemepic.epicnetdriver]` | Bytes per second for one player. |
| `TotalNetBandwidth` | `[/script/engine.gamenetworkmanager]` | Bytes per second for the whole server, shared between its players. |
| `MaxDynamicBandwidth`, `MinDynamicBandwidth` | `[/script/engine.gamenetworkmanager]` | Range each player's share is kept in. |

Each player gets `TotalNetBandwidth` divided by the number of players, kept between the dynamic minimum
and maximum. Up to 4 players on a map each get the full 5 MB/s per player at 20 MB/s total. The Network
line in `ServerPerformance.Stats` shows whether players are hitting their limit ("bandwidth-limited").

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
| `HitchThreshold_TicksPerSecond` | `25` | Log every second with fewer server frames than this to the stats log, with its length. With `t.MaxFPS 30`, 25 catches garbage collections (~0.25 s) and anything longer. `0` = off. |
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
- **`Hitch:` lines in the stats log.** Each is a stall that ended at that time; subtract its length to see
  when it started, then look at the server log just before. A line every collection interval is garbage
  collection (about 0.25 s on a big map); one about every 15 minutes is the world save (about 1 s). Several
  seconds with nothing in the log is often a console window: selecting text in a Windows console window
  (QuickEdit mode) pauses the whole server until the selection is cleared. Turn QuickEdit off in the
  console window's Properties, and copy lines from the log files instead.
- **Tick rate lower than `t.MaxFPS`.** The server can't keep up at that rate under the current load; the
  cap is a ceiling, not a target.

## Tests

`tests\logic_tests.cpp` covers config parsing and warnings, console-variable name and value checks, reading
the engine's replies, value comparison, tick-rate, hitch and network sampling and the stats text. It needs no game and no
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
