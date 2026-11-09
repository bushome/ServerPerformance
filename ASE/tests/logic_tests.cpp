// Stand-alone tests for src/PerfLogic.h. No game, server API DLL or Windows needed.
//
// Build and run from the project folder (any C++20 compiler):
//   cl /std:c++20 /EHsc /I extern\AsaApi\AsaApi\Core\Public /I src tests\logic_tests.cpp && logic_tests.exe configs\config.json
//   clang++ -std=c++20 -I extern/AsaApi/AsaApi/Core/Public -I src tests/logic_tests.cpp -o logic_tests && ./logic_tests configs/config.json
// (ASE folder: use extern\AseApi\version\Core\Public as the first include path.)
//
// The optional argument is a config.json to validate. Add --shipped after it to also check the file
// against the expectations for the shipped config at the bottom of this file:
//   logic_tests.exe configs\config.json --shipped
// configs\config.commented.json holds the same values, so it must pass the same check.

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

#include "PerfLogic.h"

using namespace Perf;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(condition) \
	do { \
		++g_checks; \
		if (!(condition)) { \
			++g_failures; \
			std::cerr << "FAILED line " << __LINE__ << ": " #condition "\n"; \
		} \
	} while (false)

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static Settings Parse(const char* text)
{
	return ParseSettings(nlohmann::json::parse(text, nullptr, true, true));
}

static bool HasWarning(const Settings& s, const std::string& part)
{
	for (const std::string& warning : s.warnings)
		if (warning.find(part) != std::string::npos)
			return true;
	return false;
}

static void TestDefaults()
{
	const Settings s = Parse("{}");
	CHECK(!s.auto_gc && s.auto_gc_minutes == 15 && !s.full_purge);
	CHECK(Near(s.hitch_threshold, 25.0));
	CHECK(s.cvars.empty());
	CHECK(s.cvar_reapply_seconds == 300);
	CHECK(!s.structures);
	CHECK(Near(s.structure_net_frequency, 0.2) && Near(s.structure_min_net_frequency, 0.1));
	CHECK(s.structure_check_seconds == 300 && s.structure_exclude.empty());
	CHECK(s.stats_seconds == 300 && !s.stats_to_api_log && s.count_actors && s.network);
	CHECK(s.stats_log_max_mb == 5 && s.stats_log_files == 3);
	CHECK(s.warnings.empty());
}

static void TestParsing()
{
	const Settings s = Parse(R"({
		// comments are allowed
		"GarbageCollection": { "AutoGC_Enabled": true, "AutoGC_Interval_Minutes": 30, "FullPurge": true },
		"ConsoleVariables": { "t.MaxFPS": 60, "gc.ActorClusteringEnabled": true, "p.Ratio": 0.25, "r.Mode": "Fast" },
		"ConsoleVariables_Reapply_Seconds": 0,
		"Structures": { "Enabled": true, "NetUpdateFrequency": 0.5, "MinNetUpdateFrequency": 0.25,
			"CheckInterval_Seconds": 60, "ExcludeClassesContaining": [ " Turret ", "Door" ] },
		"Stats": { "LogInterval_Seconds": 0, "AlsoWriteToApiLog": true, "CountDinosAndStructures": false, "IncludeNetwork": false,
			"HitchThreshold_TicksPerSecond": 0,
			"LogFileMaxSize_MB": 10, "LogFilesKept": 5 }
	})");
	CHECK(s.auto_gc && s.auto_gc_minutes == 30 && s.full_purge);
	CHECK(s.cvars.size() == 4);
	// nlohmann sorts object keys, so variables apply in name order
	CHECK(s.cvars[0].name == "gc.ActorClusteringEnabled" && s.cvars[0].value == "1");
	CHECK(s.cvars[1].name == "p.Ratio" && s.cvars[1].value == "0.25");
	CHECK(s.cvars[2].name == "r.Mode" && s.cvars[2].value == "Fast");
	CHECK(s.cvars[3].name == "t.MaxFPS" && s.cvars[3].value == "60");
	CHECK(s.cvar_reapply_seconds == 0);
	CHECK(s.structures && Near(s.structure_net_frequency, 0.5) && Near(s.structure_min_net_frequency, 0.25));
	CHECK(s.structure_check_seconds == 60);
	CHECK(s.structure_exclude.size() == 2 && s.structure_exclude[0] == "Turret");
	CHECK(s.stats_seconds == 0 && s.stats_to_api_log && !s.count_actors && !s.network);
	CHECK(Near(s.hitch_threshold, 0.0));
	CHECK(s.stats_log_max_mb == 10 && s.stats_log_files == 5);
	CHECK(s.warnings.empty());
}

static void TestWarnings()
{
	const Settings s = Parse(R"({
		"GarbageCollection": { "AutoGC_Enabled": "yes", "AutoGC_Interval_Minutes": 0 },
		"ConsoleVariables": {
			"exit": 1,
			"t.MaxFPS; quit": 1,
			"t.Good": [1, 2],
			"t.Bad": "two words",
			"t.Ok": 5
		},
		"Structures": { "NetUpdateFrequency": 0.1, "MinNetUpdateFrequency": 0.5, "ExcludeClassesContaining": "Door" },
		"Stats": 5,
		"Typo": true
	})");
	CHECK(!s.auto_gc);                      // bad type keeps the default
	CHECK(HasWarning(s, "AutoGC_Enabled"));
	CHECK(s.auto_gc_minutes == 1);          // clamped
	CHECK(HasWarning(s, "AutoGC_Interval_Minutes"));
	CHECK(s.cvars.size() == 1 && s.cvars[0].name == "t.Ok");
	CHECK(HasWarning(s, "'exit' is not a console variable name"));
	CHECK(HasWarning(s, "'t.MaxFPS; quit' is not a console variable name"));
	CHECK(HasWarning(s, "t.Good must be"));
	CHECK(HasWarning(s, "'two words' is not a valid value"));
	CHECK(Near(s.structure_min_net_frequency, 0.1));   // pulled down to NetUpdateFrequency
	CHECK(HasWarning(s, "MinNetUpdateFrequency is above"));
	CHECK(HasWarning(s, "ExcludeClassesContaining must be a list"));
	CHECK(HasWarning(s, "Stats must be an object"));
	CHECK(HasWarning(s, "unknown setting 'Typo'"));

	bool threw = false;
	try { (void)Parse("[1, 2]"); }
	catch (const std::exception&) { threw = true; }
	CHECK(threw);
}

static void TestCVarNames()
{
	CHECK(IsValidCVarName("gc.TimeBetweenPurgingPendingKillObjects"));
	CHECK(IsValidCVarName("p.Chaos.MaxSubSteps"));
	CHECK(IsValidCVarName("net_x.Y_2"));
	CHECK(!IsValidCVarName("exit"));             // no dot: a command, not a variable
	CHECK(!IsValidCVarName(".x"));
	CHECK(!IsValidCVarName("x."));
	CHECK(!IsValidCVarName("t.MaxFPS 30"));
	CHECK(!IsValidCVarName("t.MaxFPS|quit"));
	CHECK(!IsValidCVarName(""));
	CHECK(IsValidCVarValue("30") && IsValidCVarValue("-1.5") && IsValidCVarValue("1e+3") && IsValidCVarValue("Fast"));
	CHECK(!IsValidCVarValue("") && !IsValidCVarValue("1 2") && !IsValidCVarValue("1;quit") && !IsValidCVarValue("\"x\""));
}

static void TestValuesMatch()
{
	CHECK(ValuesMatch("20", "20.000000"));
	CHECK(ValuesMatch("0.2", "0.200000"));
	CHECK(ValuesMatch("1", "true"));
	CHECK(ValuesMatch("0", "False"));
	CHECK(ValuesMatch("Fast", "fast"));
	CHECK(!ValuesMatch("20", "60"));
	CHECK(!ValuesMatch("0.2", "0.21"));
	CHECK(!ValuesMatch("Fast", "Slow"));
}

static void TestReplyParsing()
{
	// UE5 query reply
	CHECK(ParseCVarReply("gc.TimeBetweenPurgingPendingKillObjects",
		"gc.TimeBetweenPurgingPendingKillObjects = \"20\"      LastSetBy: Console").value_or("") == "20");
	// UE4 style, unquoted, different case, other lines around it
	CHECK(ParseCVarReply("t.MaxFPS", "Some other line\r\nT.MAXFPS = 30.000000\r\n").value_or("") == "30.000000");
	// set reply
	CHECK(ParseCVarReply("p.Chaos.MaxSubSteps", "p.Chaos.MaxSubSteps = \"2\"").value_or("") == "2");
	// a longer name that starts the same does not match
	CHECK(!ParseCVarReply("t.MaxFPS", "t.MaxFPSSmoothing = \"1\"").has_value());
	// help text or errors are not a value
	CHECK(!ParseCVarReply("t.MaxFPS", "Unable to edit t.MaxFPS: read only").has_value());
	CHECK(!ParseCVarReply("t.MaxFPS", "").has_value());
}

static void TestHelpers()
{
	CHECK(FormatNumber(20) == "20");
	CHECK(FormatNumber(0.2) == "0.2");
	CHECK(FormatNumber(-1.5) == "-1.5");
	CHECK(FormatNumber(0.000001) == "0.000001");
	CHECK(Tokens("  ServerPerformance.CVar   t.MaxFPS  30 ").size() == 3);
	CHECK(IsExcluded("Blueprint'/Game/Structures/StructureBP_Turret.StructureBP_Turret_C'", { "turret" }));
	CHECK(!IsExcluded("Blueprint'/Game/Structures/Wall.Wall_C'", { "turret", "door" }));
	CHECK(NeedsLowering(1.0, 0.2) && !NeedsLowering(0.2, 0.2) && !NeedsLowering(0.1, 0.2));
	CHECK(FormatDuration(-1) == "never");
	CHECK(FormatDuration(59) == "59s");
	CHECK(FormatDuration(3725) == "1h 2m");
	CHECK(FormatDuration(90061) == "1d 1h 1m");
}

static void TestHitch()
{
	// a normal second at a 30 fps cap
	CHECK(!DescribeHitch(30, 1.0, 25.0).has_value());
	// a garbage collection: ~0.25 s lost in one second
	const std::optional<std::string> gc = DescribeHitch(22, 1.0, 25.0);
	CHECK(gc.has_value() && gc->find("22 frames in 1.0 s (22.0 per second)") != std::string::npos);
	// a freeze arrives as one long sample
	const std::optional<std::string> freeze = DescribeHitch(16, 7.3, 25.0);
	CHECK(freeze.has_value() && freeze->find("16 frames in 7.3 s (2.2 per second)") != std::string::npos);
	// threshold 0 turns it off; a zero-length sample is ignored
	CHECK(!DescribeHitch(1, 10.0, 0.0).has_value());
	CHECK(!DescribeHitch(0, 0.0, 25.0).has_value());
}

static void TestTickWindow()
{
	TickWindow w;
	CHECK(w.Count() == 0 && Near(w.Average(), 0) && Near(w.Min(), 0));
	w.Add(30); w.Add(20); w.Add(40); w.Add(-1);   // negative samples are ignored
	CHECK(w.Count() == 3 && Near(w.Average(), 30) && Near(w.Min(), 20) && Near(w.Max(), 40));
	w.Reset();
	CHECK(w.Count() == 0);

	CHECK(Near(TicksPerSecond(100, 130, 1.0), 30));
	CHECK(Near(TicksPerSecond(100, 130, 2.0), 15));
	CHECK(TicksPerSecond(130, 100, 1.0) < 0);
	CHECK(TicksPerSecond(100, 130, 0.0) < 0);
}

static ConnectionSample Conn(double ping, long long in_b, long long out_b, long long in_p, long long out_p,
	long long in_l, long long out_l, long long queued)
{
	ConnectionSample c;
	c.ping_seconds = ping; c.in_bytes = in_b; c.out_bytes = out_b; c.in_packets = in_p; c.out_packets = out_p;
	c.in_lost = in_l; c.out_lost = out_l; c.queued_bits = queued;
	return c;
}

static void TestNetWindow()
{
	NetWindow w;
	CHECK(w.Summary().samples == 0 && w.Summary().connections == -1 && w.Summary().out_kbps_avg < 0);

	// second 1: two players
	w.Add({ Conn(0.050, 1024, 10240, 30, 60, 0, 1, -500), Conn(0.150, 2048, 20480, 30, 39, 0, 0, 800) });
	// second 2: nobody online
	w.Add({});
	NetStats n = w.Summary();
	CHECK(n.samples == 2 && n.connections == 0);
	CHECK(Near(n.out_kbps_avg, 15.0));    // (10 + 20 KB) over 2 seconds
	CHECK(Near(n.out_kbps_peak, 30.0));
	CHECK(Near(n.in_kbps_avg, 1.5) && Near(n.in_kbps_peak, 3.0));
	CHECK(Near(n.ping_ms_avg, 100.0) && Near(n.ping_ms_max, 150.0));
	CHECK(Near(n.out_loss_pct, 1.0));     // 1 lost of 99 sent + 1 lost
	CHECK(Near(n.in_loss_pct, 0.0));
	CHECK(Near(n.saturated_pct, 50.0));   // one of two connection-seconds had queued data

	// fields this server build doesn't report stay unknown
	NetWindow partial;
	ConnectionSample only_ping;
	only_ping.ping_seconds = 0.02;
	partial.Add({ only_ping });
	n = partial.Summary();
	CHECK(n.out_kbps_avg < 0 && n.out_loss_pct < 0 && n.saturated_pct < 0);
	CHECK(Near(n.ping_ms_avg, 20.0));

	w.Reset();
	CHECK(w.Samples() == 0);
}

static void TestStatsText()
{
	Snapshot s;
	s.uptime_seconds = 3725;
	s.players = 12;
	s.tick_samples = 300; s.tick_avg = 29.94; s.tick_min = 18.0; s.tick_max = 30.0; s.hitches = 2;
	s.uobjects = 1500000; s.uobject_slots = 1600000; s.uobject_capacity = 2162688; s.objects_outside_gc = 400000;
	s.dinos = 3000; s.structures = 90000;
	s.working_set_mb = 18432.4; s.peak_working_set_mb = 20000; s.private_mb = 19000; s.system_available_mb = 100000; s.system_total_mb = 262144;
	s.forced_gcs = 4; s.seconds_since_forced_gc = 120;
	s.cvars = { { "t.MaxFPS", "30" } };
	s.network = true;
	s.net.samples = 300; s.net.connections = 12; s.net.out_kbps_avg = 850.4; s.net.out_kbps_peak = 2100;
	s.net.in_kbps_avg = 120; s.net.in_kbps_peak = 300; s.net.ping_ms_avg = 45; s.net.ping_ms_max = 210;
	s.net.out_loss_pct = 0.25; s.net.in_loss_pct = 0.1; s.net.saturated_pct = 1.2;

	const std::string text = FormatStatsText(s);
	CHECK(text.find("Uptime 1h 2m, players 12") != std::string::npos);
	CHECK(text.find("avg 29.9, min 18.0, max 30.0, hitches 2") != std::string::npos);
	CHECK(text.find("1500000 live, 1600000 slots used of 2162688") != std::string::npos);
	CHECK(text.find("Dinos 3000, structures 90000") != std::string::npos);
	CHECK(text.find("working set 18432 MB") != std::string::npos);
	CHECK(text.find("Forced GCs: 4, last 2m 0s ago") != std::string::npos);
	CHECK(text.find("t.MaxFPS = 30") != std::string::npos);
	CHECK(text.find("Network (last 300s): 12 connections, out avg 850 KB/s (peak 2100)") != std::string::npos);
	CHECK(text.find("ping avg 45 ms (max 210), loss out 0.2% / in 0.1%, bandwidth-limited 1.2%") != std::string::npos);
	CHECK(text.back() != '\n');

	const std::string line = FormatStatsLine(s);
	CHECK(line.find("players=12 tick_avg=29.9") != std::string::npos);
	CHECK(line.find("hitches=2") != std::string::npos);
	CHECK(line.find("dinos=3000 structures=90000") != std::string::npos);
	CHECK(line.find('\n') == std::string::npos);
	CHECK(line.find("connections=12 out_kbps_avg=850 out_kbps_peak=2100") != std::string::npos);
	CHECK(line.find("saturated_pct=1.2") != std::string::npos);

	const nlohmann::json j = StatsJson(s);
	CHECK(j["players"] == 12);
	CHECK(j["tick"]["hitches"] == 2);
	CHECK(j["uobjects"]["live"] == 1500000);
	CHECK(j["cvars"]["t.MaxFPS"] == "30");
	CHECK(j["network"]["connections"] == 12);
	CHECK(Near(j["network"]["ping_ms_max"].get<double>(), 210));

	Snapshot unknown;
	CHECK(FormatStatsText(unknown).find("UObjects: ? live") != std::string::npos);
	CHECK(FormatStatsText(unknown).find("Dinos") == std::string::npos);
	CHECK(StatsJson(unknown)["players"].is_null());
	CHECK(StatsJson(unknown)["network"].is_null());
	CHECK(FormatStatsLine(unknown).find("connections=") == std::string::npos);
}

// Expectations for configs/config.json and configs/config.commented.json (same values).
static void CheckShipped(const Settings& s)
{
	CHECK(s.warnings.empty());
	CHECK(!s.auto_gc && s.auto_gc_minutes == 15 && !s.full_purge);
	CHECK(s.cvars.size() == 5);
	auto value_of = [&](const std::string& name) -> std::string
	{
		for (const CVarSetting& c : s.cvars)
			if (c.name == name) return c.value;
		return "";
	};
	CHECK(value_of("gc.PrimalDedicatedServerOnlyTimeBetweenPurgingPendingKillObjects") == "300");
	CHECK(value_of("gc.TimeBetweenPurgingPendingKillObjectsOnIdleServerMultiplier") == "1");
	CHECK(value_of("gc.TimeBetweenPurgingPendingKillObjects").empty());   // ignored by ARK's dedicated server
	CHECK(value_of("gc.NumRetriesBeforeForcingGC") == "5");
	CHECK(value_of("gc.ActorClusteringEnabled") == "1");
	CHECK(value_of("t.MaxFPS") == "30");
	CHECK(s.cvar_reapply_seconds == 300);
	CHECK(!s.structures);
	CHECK(Near(s.structure_net_frequency, 0.2) && Near(s.structure_min_net_frequency, 0.1));
	CHECK(s.structure_check_seconds == 300);
	CHECK(s.structure_exclude.size() == 7);
	CHECK(s.stats_seconds == 300 && !s.stats_to_api_log && s.count_actors && s.network);
	CHECK(Near(s.hitch_threshold, 25.0));
	CHECK(s.stats_log_max_mb == 5 && s.stats_log_files == 3);
}

int main(int argc, char** argv)
{
	TestDefaults();
	TestParsing();
	TestWarnings();
	TestCVarNames();
	TestValuesMatch();
	TestReplyParsing();
	TestHelpers();
	TestHitch();
	TestTickWindow();
	TestNetWindow();
	TestStatsText();

	if (argc > 1)
	{
		std::ifstream file(argv[1]);
		if (!file.is_open())
		{
			std::cerr << "cannot open " << argv[1] << "\n";
			return 2;
		}
		try
		{
			const Settings s = ParseSettings(nlohmann::json::parse(file, nullptr, true, true));
			std::cout << argv[1] << ": " << s.cvars.size() << " console variable(s), auto GC "
				<< (s.auto_gc ? "every " + std::to_string(s.auto_gc_minutes) + " min" : std::string("off"))
				<< ", structures " << (s.structures ? "on" : "off") << "\n";
			for (const std::string& warning : s.warnings)
				std::cout << "  warning: " << warning << "\n";
			if (argc > 2 && std::string(argv[2]) == "--shipped")
				CheckShipped(s);
		}
		catch (const std::exception& error)
		{
			std::cerr << argv[1] << ": " << error.what() << "\n";
			return 2;
		}
	}

	std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
