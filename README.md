```
 _____
/  ___|
\ `--.   ___  _ __ __   __  ___  _ __
 `--. \ / _ \| '__|\ \ / / / _ \| '__|
/\__/ /|  __/| |    \ V / |  __/| |
\____/  \___||_|     \_/   \___||_|
______               __                                                          .-''''''-.
| ___ \             / _|                                                        / .  '  .  \
| |_/ /  ___  _ __ | |_   ___   _ __  _ __ ___    __ _  _ __    ___   ___      | .     /  . |
|  __/  / _ \| '__||  _| / _ \ | '__|| '_ ` _ \  / _` || '_ \  / __| / _ \     |      /     |
| |    |  __/| |   | |  | (_) || |   | | | | | || (_| || | | || (__ |  __/     |     o      |
\_|     \___||_|   |_|   \___/ |_|   |_| |_| |_| \__,_||_| |_| \___| \___|      \__________/
```

Server plugin for engine-level tuning and performance monitoring. It sets Unreal Engine console variables
from its config and confirms with the engine that each one took, requests garbage collections on a
schedule, can lower how often structures replicate, and reports the server's real tick rate, UObject
counts, dino and structure counts and memory, on demand and in a rotating stats log. `ServerPerformance.CVar`
reads or sets any console variable live, so values can be tried on a running server before they go into
the config.

**Status: new.** Both versions are written; neither has run on a server yet. Test on the test server
before the live cluster.

| Folder | Game | Server API (submodule) | Player key |
|---|---|---|---|
| [`ASA`](ASA/README.md) | ARK: Survival Ascended | [AsaApi](https://github.com/ArkServerApi/AsaApi) at `ASA/extern/AsaApi` | not used (no per-player data) |
| [`ASE`](ASE/README.md) | ARK: Survival Evolved | [ArkServerApi (AseApi)](https://github.com/ArkServerApi/AseApi) at `ASE/extern/AseApi` | not used (no per-player data) |

Each folder is a self-contained Visual Studio solution. Clone with submodules:

```
git clone --recurse-submodules https://github.com/bushome/ServerPerformance.git
```

or, in an existing clone:

```
git submodule update --init --recursive
```

The logic (`src/PerfLogic.h`), the tests and the configs are the same in both folders; only
`src/Main.cpp`, `src/EngineBridge.h` and the project files differ by API. Differences in behaviour:

- **Missing engine symbols.** On ASA every engine function and field the plugin uses is looked up while it
  loads, so after a game update that removes one, AsaApi skips loading the plugin and logs the name (the
  server keeps running). On ASE a missing symbol switches off only the feature that needs it, with an error
  in the log.
- **`gc.TimeBetweenPurgingPendingKillObjects`** is a console variable on ASA (Unreal Engine 5). ASE's engine
  keeps it as an engine setting instead; the ASE build reads and writes that setting under the same name,
  so one config works for both.
- **`Structures.MinNetUpdateFrequency`** exists only on ASA; ASE ignores it.

Build and install steps are in each folder's README.

## Development

Developed with AI assistance (Claude). Every change is reviewed and tested on a dedicated test server
before going live.
