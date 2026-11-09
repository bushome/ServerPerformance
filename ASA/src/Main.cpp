// ServerPerformance - engine tuning and performance stats for ARK servers.
//
//   * Console variables from config.json are set once the world is loaded, read back from the
//     engine to confirm they took, and re-checked every ConsoleVariables_Reapply_Seconds. The value
//     each one had before this plugin changed it is kept and restored when it is removed from the
//     config or the plugin unloads.
//   * Optional timed garbage collection (off by default): a collection is requested from the engine
//     every AutoGC_Interval_Minutes; the engine runs it at its next safe point in the frame. The
//     shipped config instead tunes ARK's own collection schedule through ConsoleVariables.
//   * Optional structure replication pass: lowers NetUpdateFrequency on structures, never raises it,
//     and puts the class defaults back when turned off or unloaded.
//   * Hitch log: any one-second sample below Stats.HitchThreshold_TicksPerSecond is written to the
//     stats log with its length, so a freeze can be matched to what ran at that moment.
//   * Stats: real server tick rate (from the engine's frame counter), UObject counts, dino and
//     structure counts, memory, and network totals sampled from every player connection (bandwidth,
//     ping, packet loss, bandwidth-limited share), on demand and as a line in a rotating stats log.
//
// There are no gameplay hooks. Everything runs from the server API's one-second timer and the
// RCON / admin console commands, all on the game thread.

#include "API/ARK/Ark.h"
#include "Logger/spdlog/sinks/file_sinks.h"

#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "EngineBridge.h"
#include "PerfLogic.h"

#ifndef PROJECT_NAME
#define PROJECT_NAME "ServerPerformance"
#endif

namespace
{
	using Perf::Settings;

	constexpr const char* kTimerId = PROJECT_NAME "_Timer";

	Settings g_settings;
	bool g_settings_loaded = false;
	bool g_world_ready = false;
	long long g_clock = 0;   // seconds since load, advanced by the timer

	// ------------------------------------------------------------------------------------------
	// Paths and logging
	// ------------------------------------------------------------------------------------------

	std::string PluginDir()
	{
		return AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" PROJECT_NAME;
	}

	std::shared_ptr<spdlog::logger> g_stats_log;
	int g_stats_log_mb = 0;
	int g_stats_log_files = 0;

	void OpenStatsLog()
	{
		if (g_stats_log && g_stats_log_mb == g_settings.stats_log_max_mb && g_stats_log_files == g_settings.stats_log_files)
			return;

		g_stats_log.reset();
		try
		{
			auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(PluginDir() + "/" PROJECT_NAME "_stats.log",
				static_cast<std::size_t>(g_settings.stats_log_max_mb) * 1024 * 1024, static_cast<std::size_t>(g_settings.stats_log_files));
			g_stats_log = std::make_shared<spdlog::logger>(PROJECT_NAME "_stats", sink);
			g_stats_log->set_pattern("%Y-%m-%d %H:%M:%S %v");
			g_stats_log->flush_on(spdlog::level::info);
			g_stats_log_mb = g_settings.stats_log_max_mb;
			g_stats_log_files = g_settings.stats_log_files;
		}
		catch (const std::exception& error)
		{
			g_stats_log.reset();
			Log::GetLog()->warn("Cannot open the stats log ({}); periodic messages go to the API log", error.what());
		}
	}

	// Routine, periodic messages: the stats log, plus the API log when asked for (or when the stats
	// log could not be opened).
	template <typename... Args>
	void Periodic(const char* format, const Args&... args)
	{
		if (g_stats_log)
			g_stats_log->info(format, args...);
		if (!g_stats_log || g_settings.stats_to_api_log)
			Log::GetLog()->info(format, args...);
	}

	std::string FirstLine(const std::string& text)
	{
		const std::string trimmed = Perf::Trim(text);
		const size_t end = trimmed.find_first_of("\r\n");
		return end == std::string::npos ? trimmed : Perf::Trim(trimmed.substr(0, end));
	}

	// ------------------------------------------------------------------------------------------
	// Tick rate
	// ------------------------------------------------------------------------------------------

	Perf::TickWindow g_ticks;
	Perf::NetWindow g_net;
	int g_hitches = 0;   // samples below the hitch threshold in the current window
	bool g_have_tick_sample = false;
	unsigned long long g_last_frames = 0;
	std::chrono::steady_clock::time_point g_last_tick_time;

	void SampleTickRate()
	{
		if (!Engine::HasFrameCounter())
			return;
		const auto now = std::chrono::steady_clock::now();
		const unsigned long long frames = Engine::FrameCounter();
		if (g_have_tick_sample)
		{
			const double seconds = std::chrono::duration<double>(now - g_last_tick_time).count();
			g_ticks.Add(Perf::TicksPerSecond(g_last_frames, frames, seconds));

			// A freeze arrives here as one long sample, so log it with its length.
			if (g_settings_loaded && frames >= g_last_frames)
			{
				const std::optional<std::string> hitch = Perf::DescribeHitch(frames - g_last_frames, seconds, g_settings.hitch_threshold);
				if (hitch)
				{
					++g_hitches;
					Periodic("{}", *hitch);
				}
			}
		}
		g_have_tick_sample = true;
		g_last_frames = frames;
		g_last_tick_time = now;
	}

	void SampleNetwork()
	{
		if (g_settings_loaded && g_settings.network && Engine::HasNetwork())
			g_net.Add(Engine::ReadConnections());
	}

	void ResetWindows()
	{
		g_ticks.Reset();
		g_net.Reset();
		g_hitches = 0;
	}

	// ------------------------------------------------------------------------------------------
	// Console variables
	// ------------------------------------------------------------------------------------------

	enum class CVarStatus { Pending, Applied, Rejected, NotFound };

	struct CVarState
	{
		std::string name;
		std::string wanted;
		CVarStatus status = CVarStatus::Pending;
		std::string detail;   // what the engine said when it was rejected
	};
	std::vector<CVarState> g_cvars;   // from config.json, in apply order

	// The value each variable had before this plugin first changed it. Key: lower-case name.
	struct Original
	{
		std::string name;
		std::string value;
		bool changed = false;   // this plugin has set it since
	};
	std::map<std::string, Original> g_originals;

	// The engine's current value, or nullopt when the engine doesn't know the name.
	std::optional<std::string> ReadCVar(const std::string& name, std::string* raw = nullptr)
	{
		const Engine::ExecResult result = Engine::QueryCVar(name);
		if (raw != nullptr)
			*raw = result.output;
		if (!result.handled)
			return std::nullopt;
		return Perf::ParseCVarReply(name, result.output);
	}

	void RememberOriginal(const std::string& name, const std::string& value)
	{
		const std::string key = Perf::Lower(name);
		if (g_originals.find(key) == g_originals.end())
			g_originals[key] = Original{ name, value, false };
	}

	// Sets one variable and checks the engine took it. Returns the engine's value afterwards, or
	// nullopt with `problem` filled in.
	std::optional<std::string> WriteCVar(const std::string& name, const std::string& value, std::string& problem)
	{
		std::string raw;
		const std::optional<std::string> before = ReadCVar(name, &raw);
		if (!before)
		{
			problem = "not a console variable in this game build";
			if (!FirstLine(raw).empty())
				problem += " (engine: " + FirstLine(raw) + ")";
			return std::nullopt;
		}

		RememberOriginal(name, *before);
		if (Perf::ValuesMatch(value, *before))
			return before;

		const Engine::ExecResult set = Engine::SetCVar(name, value);
		const std::optional<std::string> after = ReadCVar(name);
		if (after && Perf::ValuesMatch(value, *after))
		{
			g_originals[Perf::Lower(name)].changed = true;
			return after;
		}

		problem = "the engine kept " + after.value_or("?");
		if (!FirstLine(set.output).empty())
			problem += " (engine: " + FirstLine(set.output) + ")";
		return std::nullopt;
	}

	void ApplyCVar(CVarState& state, bool quiet_if_unchanged)
	{
		std::string problem;
		const std::optional<std::string> before = ReadCVar(state.name);
		const std::optional<std::string> result = WriteCVar(state.name, state.wanted, problem);

		if (!result)
		{
			const bool already_reported = state.status == CVarStatus::Rejected || state.status == CVarStatus::NotFound;
			state.status = before ? CVarStatus::Rejected : CVarStatus::NotFound;
			state.detail = problem;
			if (!already_reported)
				Log::GetLog()->warn("{} = {} not applied: {}", state.name, state.wanted, problem);
			return;
		}

		const bool changed = before && !Perf::ValuesMatch(state.wanted, *before);
		state.status = CVarStatus::Applied;
		state.detail.clear();
		if (changed)
			Log::GetLog()->info("{} = {} (was {})", state.name, *result, *before);
		else if (!quiet_if_unchanged)
			Log::GetLog()->info("{} = {} (already set)", state.name, *result);
	}

	void ApplyAllCVars()
	{
		for (CVarState& state : g_cvars)
			ApplyCVar(state, false);
	}

	// Periodic check: put back anything something else has changed. Variables the engine refused are
	// not retried until the next reload, so a bad entry doesn't fill the log.
	void ReapplyCVars()
	{
		for (CVarState& state : g_cvars)
		{
			if (state.status != CVarStatus::Applied)
				continue;
			const std::optional<std::string> current = ReadCVar(state.name);
			if (current && Perf::ValuesMatch(state.wanted, *current))
				continue;
			Log::GetLog()->warn("{} was changed to {} outside this plugin; setting it back to {}",
				state.name, current.value_or("?"), state.wanted);
			ApplyCVar(state, true);
		}
	}

	void RestoreOriginal(const std::string& name)
	{
		const auto it = g_originals.find(Perf::Lower(name));
		if (it == g_originals.end() || !it->second.changed)
			return;
		Engine::SetCVar(it->second.name, it->second.value);
		const std::optional<std::string> now = ReadCVar(it->second.name);
		if (now && Perf::ValuesMatch(it->second.value, *now))
			Log::GetLog()->info("{} restored to {}", it->second.name, it->second.value);
		else
			Log::GetLog()->warn("{} could not be restored to {} (engine has {})", it->second.name, it->second.value, now.value_or("?"));
		it->second.changed = false;
	}

	void RestoreAllOriginals()
	{
		for (auto& [key, original] : g_originals)
			RestoreOriginal(original.name);
	}

	bool IsConfigured(const std::string& name)
	{
		const std::string key = Perf::Lower(name);
		for (const CVarState& state : g_cvars)
			if (Perf::Lower(state.name) == key)
				return true;
		return false;
	}

	// ------------------------------------------------------------------------------------------
	// Garbage collection
	// ------------------------------------------------------------------------------------------

	long long g_next_gc = 0;
	long long g_forced_gcs = 0;
	long long g_last_gc = -1;

	bool RequestGC(bool full_purge, const char* reason)
	{
		if (!Engine::RequestGarbageCollection(full_purge))
			return false;
		++g_forced_gcs;
		g_last_gc = g_clock;
		Periodic("GC requested ({}, {})", full_purge ? "full purge" : "incremental purge", reason);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Structures
	// ------------------------------------------------------------------------------------------

	long long g_next_structure_pass = 0;
	bool g_structures_touched = false;

	// Calls `visit` for every structure whose class is not excluded.
	template <typename Visit>
	void ForEachStructure(Visit&& visit, int& excluded)
	{
		excluded = 0;
		const TArray<UObject*> structures = Engine::ObjectsOfClass(Engine::StructureClass());
		std::unordered_map<UClass*, bool> class_excluded;
		for (UObject* object : structures)
		{
			UClass* the_class = Engine::ClassOf(object);
			if (object == nullptr || the_class == nullptr)
				continue;
			auto it = class_excluded.find(the_class);
			if (it == class_excluded.end())
				it = class_excluded.emplace(the_class, Perf::IsExcluded(Engine::ClassPath(the_class), g_settings.structure_exclude)).first;
			if (it->second)
			{
				++excluded;
				continue;
			}
			visit(static_cast<AActor*>(object), the_class);
		}
	}

	void LowerStructureFrequencies()
	{
		static bool reported = false;
		if (!Engine::HasStructures())
		{
			if (!reported)
				Log::GetLog()->error("Structures.Enabled is on, but this server build lacks what the structure pass needs; skipped");
			reported = true;
			return;
		}
		const auto started = std::chrono::steady_clock::now();
		const float target = static_cast<float>(g_settings.structure_net_frequency);
		const float target_min = static_cast<float>(g_settings.structure_min_net_frequency);
		int checked = 0;
		int lowered = 0;
		int excluded = 0;

		ForEachStructure([&](AActor* actor, UClass*)
		{
			++checked;
			bool changed = false;
			if (Engine::HasMinNetUpdateFrequency() && Perf::NeedsLowering(Engine::MinNetUpdateFrequency(actor), target_min))
			{
				Engine::SetMinNetUpdateFrequency(actor, target_min);
				changed = true;
			}
			if (Perf::NeedsLowering(Engine::NetUpdateFrequency(actor), target))
			{
				Engine::SetNetUpdateFrequency(actor, target);
				changed = true;
			}
			if (changed)
				++lowered;
		}, excluded);

		g_structures_touched = true;
		const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		Periodic("Structures: {} checked, {} lowered, {} excluded ({} ms)", checked, lowered, excluded, ms);
	}

	void RestoreStructureFrequencies()
	{
		if (!g_structures_touched || !Engine::HasStructures())
			return;
		int restored = 0;
		int excluded = 0;
		std::unordered_map<UClass*, AActor*> defaults;
		ForEachStructure([&](AActor* actor, UClass* the_class)
		{
			auto it = defaults.find(the_class);
			if (it == defaults.end())
				it = defaults.emplace(the_class, Engine::DefaultActor(the_class)).first;
			AActor* defaults_actor = it->second;
			if (defaults_actor == nullptr)
				return;
			Engine::SetNetUpdateFrequency(actor, Engine::NetUpdateFrequency(defaults_actor));
			if (Engine::HasMinNetUpdateFrequency())
				Engine::SetMinNetUpdateFrequency(actor, Engine::MinNetUpdateFrequency(defaults_actor));
			++restored;
		}, excluded);
		g_structures_touched = false;
		Log::GetLog()->info("Structures: replication frequency restored to class defaults on {}", restored);
	}

	// ------------------------------------------------------------------------------------------
	// Stats
	// ------------------------------------------------------------------------------------------

	long long g_next_stats = 0;

	Perf::Snapshot TakeSnapshot()
	{
		Perf::Snapshot s;
		s.uptime_seconds = g_clock;
		s.players = Engine::PlayerCount();

		s.tick_samples = g_ticks.Count();
		s.hitches = g_hitches;
		s.tick_avg = g_ticks.Average();
		s.tick_min = g_ticks.Min();
		s.tick_max = g_ticks.Max();

		const Engine::ObjectCounts objects = Engine::CountObjects();
		s.uobjects = objects.live;
		s.uobject_slots = objects.slots;
		s.uobject_capacity = objects.capacity;
		s.objects_outside_gc = objects.outside_gc;

		if (g_settings.count_actors)
		{
			s.dinos = Engine::CountOfClass(Engine::DinoClass());
			s.structures = Engine::CountOfClass(Engine::StructureClass());
		}

		const Engine::MemoryInfo memory = Engine::Memory();
		s.working_set_mb = memory.working_set_mb;
		s.peak_working_set_mb = memory.peak_working_set_mb;
		s.private_mb = memory.private_mb;
		s.system_available_mb = memory.system_available_mb;
		s.system_total_mb = memory.system_total_mb;

		s.network = g_settings.network && Engine::HasNetwork();
		if (s.network)
			s.net = g_net.Summary();

		s.forced_gcs = g_forced_gcs;
		s.seconds_since_forced_gc = g_last_gc < 0 ? -1 : g_clock - g_last_gc;

		for (const CVarState& state : g_cvars)
			s.cvars.emplace_back(state.name, ReadCVar(state.name).value_or("?"));
		return s;
	}

	// ------------------------------------------------------------------------------------------
	// Config
	// ------------------------------------------------------------------------------------------

	Settings LoadSettings()
	{
		const std::string path = PluginDir() + "/config.json";

		std::ifstream file(path);
		if (!file.is_open())
			throw std::runtime_error("cannot open " + path);

		nlohmann::json root;
		try
		{
			// Last argument: allow // and /* */ comments in config.json.
			root = nlohmann::json::parse(file, nullptr, true, true);
		}
		catch (const nlohmann::json::exception& error)
		{
			throw std::runtime_error(std::string("config.json is not valid JSON: ") + error.what());
		}

		return Perf::ParseSettings(root);
	}

	void Schedule()
	{
		g_next_gc = g_clock + static_cast<long long>(g_settings.auto_gc_minutes) * 60;
		g_next_structure_pass = g_clock + 1;   // first pass on the next tick
		g_next_stats = g_clock + g_settings.stats_seconds;
	}

	bool ReloadSettings(std::string& error)
	{
		Settings loaded;
		try
		{
			loaded = LoadSettings();
		}
		catch (const std::exception& exception)
		{
			error = exception.what();
			return false;
		}

		for (const std::string& warning : loaded.warnings)
			Log::GetLog()->warn("Config: {}", warning);

		const bool structures_were_on = g_settings_loaded && g_settings.structures;
		std::vector<std::string> previous_names;
		for (const CVarState& state : g_cvars)
			previous_names.push_back(state.name);

		g_settings = std::move(loaded);
		g_settings_loaded = true;
		OpenStatsLog();

		g_cvars.clear();
		for (const Perf::CVarSetting& setting : g_settings.cvars)
			g_cvars.push_back(CVarState{ setting.name, setting.value, CVarStatus::Pending, {} });

		Log::GetLog()->info("Config loaded: {} console variable(s), auto GC {}, structures {}, stats every {}s",
			g_cvars.size(), g_settings.auto_gc ? "every " + std::to_string(g_settings.auto_gc_minutes) + " min" : std::string("off"),
			g_settings.structures ? "on" : "off", g_settings.stats_seconds);
		if (!g_settings.network)
			g_net.Reset();

		if (g_world_ready)
		{
			// Variables taken out of the config go back to their earlier value.
			for (const std::string& name : previous_names)
				if (!IsConfigured(name))
					RestoreOriginal(name);
			ApplyAllCVars();
			if (structures_were_on && !g_settings.structures)
				RestoreStructureFrequencies();
			Schedule();
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Timer
	// ------------------------------------------------------------------------------------------

	void OnTimer()
	{
		++g_clock;
		try
		{
			SampleTickRate();

			if (!g_settings_loaded || AsaApi::GetApiUtils().GetWorld() == nullptr)
				return;

			SampleNetwork();

			if (!g_world_ready)
			{
				g_world_ready = true;
				ApplyAllCVars();
				Schedule();
				return;
			}

			if (g_settings.cvar_reapply_seconds > 0 && g_clock % g_settings.cvar_reapply_seconds == 0)
				ReapplyCVars();

			if (g_settings.auto_gc && g_clock >= g_next_gc)
			{
				RequestGC(g_settings.full_purge, "timed");
				g_next_gc = g_clock + static_cast<long long>(g_settings.auto_gc_minutes) * 60;
			}

			if (g_settings.structures && g_clock >= g_next_structure_pass)
			{
				LowerStructureFrequencies();
				g_next_structure_pass = g_clock + g_settings.structure_check_seconds;
			}

			if (g_settings.stats_seconds > 0 && g_clock >= g_next_stats)
			{
				Periodic("Stats: {}", Perf::FormatStatsLine(TakeSnapshot()));
				ResetWindows();
				g_next_stats = g_clock + g_settings.stats_seconds;
			}
			else if (g_settings.stats_seconds == 0 && g_ticks.Count() >= 300)
			{
				ResetWindows();   // without stats lines, the figures cover the last few minutes
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("Timer: {}", exception.what());
		}
	}

	// ------------------------------------------------------------------------------------------
	// Commands
	// ------------------------------------------------------------------------------------------

	std::string StatusText(CVarStatus status)
	{
		switch (status)
		{
		case CVarStatus::Applied: return "applied";
		case CVarStatus::Rejected: return "rejected";
		case CVarStatus::NotFound: return "not found";
		default: return "waiting for the world to load";
		}
	}

	std::string CmdReload(const std::vector<std::string>&)
	{
		std::string error;
		if (ReloadSettings(error))
			return PROJECT_NAME ": config reloaded";
		Log::GetLog()->error("Config reload failed: {}", error);
		return PROJECT_NAME ": reload failed - " + error;
	}

	std::string CmdStats(const std::vector<std::string>& args)
	{
		const Perf::Snapshot snapshot = TakeSnapshot();
		if (args.size() > 1 && Perf::Lower(args[1]) == "json")
			return Perf::StatsJson(snapshot).dump();
		return Perf::FormatStatsText(snapshot);
	}

	std::string CmdForceGC(const std::vector<std::string>& args)
	{
		bool full = g_settings.full_purge;
		if (args.size() > 1)
		{
			const std::string mode = Perf::Lower(args[1]);
			if (mode == "full") full = true;
			else if (mode == "quick" || mode == "incremental") full = false;
			else return "Usage: " PROJECT_NAME ".ForceGC [full|quick]";
		}
		if (!RequestGC(full, "command"))
			return PROJECT_NAME ": the engine is not available";
		return std::string(PROJECT_NAME ": garbage collection requested (") + (full ? "full purge" : "incremental purge")
			+ "); the engine runs it on its next frame";
	}

	std::string CmdCVar(const std::vector<std::string>& args)
	{
		if (args.size() < 2 || args.size() > 3)
			return "Usage: " PROJECT_NAME ".CVar <name> [value]";
		const std::string& name = args[1];
		if (!Perf::IsValidCVarName(name))
			return PROJECT_NAME ": '" + name + "' is not a console variable name";

		if (args.size() == 2)
		{
			std::string raw;
			const std::optional<std::string> value = ReadCVar(name, &raw);
			if (!value)
				return PROJECT_NAME ": " + name + " is not a console variable in this game build";
			return name + " = " + *value;
		}

		const std::string& value = args[2];
		if (!Perf::IsValidCVarValue(value))
			return PROJECT_NAME ": '" + value + "' is not a valid value";

		std::string problem;
		const std::optional<std::string> before = ReadCVar(name);
		const std::optional<std::string> result = WriteCVar(name, value, problem);
		if (!result)
			return PROJECT_NAME ": " + name + " " + value + " not applied: " + problem;

		Log::GetLog()->info("{} = {} (was {}, set by command)", name, *result, before.value_or("?"));
		std::string reply = name + " = " + *result + " (was " + before.value_or("?") + ")";
		if (IsConfigured(name))
			reply += ". config.json sets this too, so the config value comes back on the next reapply or reload";
		else
			reply += ". Not saved: add it to ConsoleVariables in config.json to keep it";
		return reply;
	}

	std::string CmdCVars(const std::vector<std::string>&)
	{
		if (g_cvars.empty())
			return PROJECT_NAME ": no console variables in config.json";
		std::string out;
		for (const CVarState& state : g_cvars)
		{
			if (!out.empty()) out += "\n";
			const auto original = g_originals.find(Perf::Lower(state.name));
			out += state.name + ": wanted " + state.wanted + ", engine " + ReadCVar(state.name).value_or("?") + ", " + StatusText(state.status);
			if (original != g_originals.end())
				out += ", before " + original->second.value;
			if (!state.detail.empty())
				out += " (" + state.detail + ")";
		}
		return out;
	}

	using Handler = std::string(*)(const std::vector<std::string>&);
	struct CommandEntry
	{
		const char* name;
		Handler handler;
	};
	const CommandEntry kCommands[] = {
		{ PROJECT_NAME ".Reload", &CmdReload },
		{ PROJECT_NAME ".Stats", &CmdStats },
		{ PROJECT_NAME ".ForceGC", &CmdForceGC },
		{ PROJECT_NAME ".CVar", &CmdCVar },
		{ PROJECT_NAME ".CVars", &CmdCVars },
	};

	std::string Run(const FString* command_line)
	{
		const std::vector<std::string> args = Perf::Tokens(command_line != nullptr ? command_line->ToString() : std::string());
		if (args.empty())
			return "";
		for (const CommandEntry& command : kCommands)
		{
			if (Perf::Lower(args[0]) == Perf::Lower(command.name))
			{
				try
				{
					return command.handler(args);
				}
				catch (const std::exception& error)
				{
					Log::GetLog()->error("{}: {}", command.name, error.what());
					return std::string(command.name) + " failed: " + error.what();
				}
			}
		}
		return "";
	}

	void OnConsole(APlayerController* player_controller, FString* command, bool /*write_to_log*/)
	{
		auto* shooter_controller = static_cast<AShooterPlayerController*>(player_controller);
		if (shooter_controller == nullptr || !shooter_controller->bIsAdmin()())
			return;
		const std::string reply = Run(command);
		if (!reply.empty())
			AsaApi::GetApiUtils().SendServerMessage(shooter_controller, FColorList::Green, "{}", reply);
	}

	void OnRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld* /*world*/)
	{
		FString reply(AsaApi::Tools::Utf8Decode(Run(&rcon_packet->Body)));
		rcon_connection->SendMessageW(rcon_packet->Id, 0, &reply);
	}
} // namespace

// Called by AsaApi when the plugin is loaded.
extern "C" __declspec(dllexport) void Plugin_Init()
{
	Log::Get().Init(PROJECT_NAME);

	// A missing offset stops the plugin from loading here, rather than the server later.
	Engine::RequireSymbols();

	std::string error;
	if (!ReloadSettings(error))
	{
		Log::GetLog()->error("Config not loaded: {}", error);
		Log::GetLog()->error("Nothing is changed until config.json is fixed and " PROJECT_NAME ".Reload is run");
	}

	for (const CommandEntry& command : kCommands)
	{
		AsaApi::GetCommands().AddConsoleCommand(command.name, &OnConsole);
		AsaApi::GetCommands().AddRconCommand(command.name, &OnRcon);
	}
	AsaApi::GetCommands().AddOnTimerCallback(kTimerId, &OnTimer);
}

// Called by AsaApi when the plugin is unloaded.
extern "C" __declspec(dllexport) void Plugin_Unload()
{
	AsaApi::GetCommands().RemoveOnTimerCallback(kTimerId);
	for (const CommandEntry& command : kCommands)
	{
		AsaApi::GetCommands().RemoveConsoleCommand(command.name);
		AsaApi::GetCommands().RemoveRconCommand(command.name);
	}

	try
	{
		if (g_world_ready)
		{
			RestoreAllOriginals();
			RestoreStructureFrequencies();
		}
	}
	catch (const std::exception& error)
	{
		Log::GetLog()->error("Unload: {}", error.what());
	}

	g_cvars.clear();
	g_originals.clear();
	g_stats_log.reset();
}
