#pragma once

// Pure logic for ServerPerformance: config parsing, console-variable reply parsing and comparison,
// tick-rate sampling and the stats report text.
//
// This header deliberately has no dependency on the game or on server API game types (only on the
// nlohmann json.hpp that ships with AsaApi and AseApi), so it can be compiled and unit-tested on any
// machine, and shared unchanged by the ASA and ASE builds. See tests/logic_tests.cpp.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"

namespace Perf
{
	// ------------------------------------------------------------------------------------------
	// Text helpers
	// ------------------------------------------------------------------------------------------

	inline std::string Lower(std::string text)
	{
		for (char& c : text)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	inline std::string Trim(const std::string& text)
	{
		size_t begin = 0;
		size_t end = text.size();
		while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
			++begin;
		while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
			--end;
		return text.substr(begin, end - begin);
	}

	inline bool StartsWithNoCase(const std::string& text, const std::string& prefix)
	{
		return text.size() >= prefix.size() && Lower(text.substr(0, prefix.size())) == Lower(prefix);
	}

	inline bool ContainsNoCase(const std::string& text, const std::string& part)
	{
		return Lower(text).find(Lower(part)) != std::string::npos;
	}

	// Splits on whitespace.
	inline std::vector<std::string> Tokens(const std::string& text)
	{
		std::vector<std::string> out;
		std::string current;
		for (const char c : text)
		{
			if (std::isspace(static_cast<unsigned char>(c)))
			{
				if (!current.empty())
					out.push_back(std::move(current));
				current.clear();
			}
			else
			{
				current += c;
			}
		}
		if (!current.empty())
			out.push_back(std::move(current));
		return out;
	}

	// The whole string is a number (no trailing text).
	inline std::optional<double> ParseNumber(const std::string& text)
	{
		const std::string trimmed = Trim(text);
		if (trimmed.empty())
			return std::nullopt;
		char* end = nullptr;
		const double value = std::strtod(trimmed.c_str(), &end);
		if (end == nullptr || *end != '\0' || !std::isfinite(value))
			return std::nullopt;
		return value;
	}

	// Shortest plain form: 20, 0.2, 1.5 (no exponent, no trailing zeros).
	inline std::string FormatNumber(double value)
	{
		if (std::fabs(value - std::round(value)) < 1e-9 && std::fabs(value) < 1e15)
			return std::to_string(static_cast<long long>(std::llround(value)));

		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "%.6f", value);
		std::string out = buffer;
		while (!out.empty() && out.back() == '0')
			out.pop_back();
		if (!out.empty() && out.back() == '.')
			out.pop_back();
		return out;
	}

	// ------------------------------------------------------------------------------------------
	// Console variables
	// ------------------------------------------------------------------------------------------

	// Console variable names look like "gc.TimeBetweenPurgingPendingKillObjects": letters, digits,
	// '.' and '_', with at least one '.'. Anything else is refused, so neither config.json nor the
	// CVar command can be used to run an arbitrary engine command (the value is sent to the engine's
	// command processor).
	inline bool IsValidCVarName(const std::string& name)
	{
		if (name.empty() || name.size() > 128 || name.find('.') == std::string::npos)
			return false;
		if (name.front() == '.' || name.back() == '.')
			return false;
		for (const char c : name)
		{
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_')
				return false;
		}
		return true;
	}

	// Values are numbers or simple words: letters, digits and . _ - +
	inline bool IsValidCVarValue(const std::string& value)
	{
		if (value.empty() || value.size() > 64)
			return false;
		for (const char c : value)
		{
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-' && c != '+')
				return false;
		}
		return true;
	}

	// True when the engine's value is the one wanted. Numbers compare numerically (the engine prints
	// floats its own way, e.g. "20.000000"); true/false compare with 1/0; anything else compares as
	// text, ignoring case.
	inline bool ValuesMatch(const std::string& wanted, const std::string& actual)
	{
		auto as_number = [](const std::string& text) -> std::optional<double>
		{
			const std::string lower = Lower(Trim(text));
			if (lower == "true") return 1.0;
			if (lower == "false") return 0.0;
			return ParseNumber(lower);
		};

		const std::optional<double> a = as_number(wanted);
		const std::optional<double> b = as_number(actual);
		if (a && b)
			return std::fabs(*a - *b) <= 1e-4 * (std::max)(1.0, std::fabs(*a));
		return Lower(Trim(wanted)) == Lower(Trim(actual));
	}

	// Finds the value of `name` in what the engine printed after a console-variable query, e.g.
	//   gc.TimeBetweenPurgingPendingKillObjects = "20"      LastSetBy: Console
	//   t.MaxFPS = 30
	inline std::optional<std::string> ParseCVarReply(const std::string& name, const std::string& reply)
	{
		size_t start = 0;
		while (start <= reply.size())
		{
			size_t end = reply.find('\n', start);
			if (end == std::string::npos)
				end = reply.size();
			const std::string line = Trim(reply.substr(start, end - start));
			start = end + 1;

			if (!StartsWithNoCase(line, name))
				continue;
			std::string rest = Trim(line.substr(name.size()));
			if (rest.empty() || rest.front() != '=')
				continue;
			rest = Trim(rest.substr(1));

			if (!rest.empty() && rest.front() == '"')
			{
				const size_t close = rest.find('"', 1);
				if (close == std::string::npos)
					return rest.substr(1);
				return rest.substr(1, close - 1);
			}
			const std::vector<std::string> words = Tokens(rest);
			if (words.empty())
				return std::nullopt;
			return words.front();
		}
		return std::nullopt;
	}

	struct CVarSetting
	{
		std::string name;
		std::string value;   // as text, the way it is sent to the engine
	};

	// ------------------------------------------------------------------------------------------
	// Settings
	// ------------------------------------------------------------------------------------------

	struct Settings
	{
		// Garbage collection
		bool auto_gc = true;
		int auto_gc_minutes = 15;
		bool full_purge = true;

		// Console variables, applied in this order
		std::vector<CVarSetting> cvars;
		int cvar_reapply_seconds = 300;   // 0 = only when loaded or reloaded

		// Structure replication
		bool structures = false;
		double structure_net_frequency = 0.2;
		double structure_min_net_frequency = 0.1;
		int structure_check_seconds = 300;
		std::vector<std::string> structure_exclude;

		// Stats
		int stats_seconds = 300;   // 0 = no periodic stats lines
		bool stats_to_api_log = false;
		bool count_actors = true;
		bool network = true;
		int stats_log_max_mb = 5;
		int stats_log_files = 3;

		std::vector<std::string> warnings;
	};

	namespace Detail
	{
		inline const nlohmann::json* Child(const nlohmann::json& parent, const char* key)
		{
			if (!parent.is_object())
				return nullptr;
			const auto it = parent.find(key);
			return it == parent.end() || it->is_null() ? nullptr : &*it;
		}

		inline void ReadBool(const nlohmann::json& parent, const char* key, bool& out, const std::string& where,
			std::vector<std::string>& warnings)
		{
			const nlohmann::json* value = Child(parent, key);
			if (value == nullptr)
				return;
			if (value->is_boolean())
				out = value->get<bool>();
			else
				warnings.push_back(where + key + " must be true or false; using " + (out ? "true" : "false"));
		}

		inline void ReadInt(const nlohmann::json& parent, const char* key, int& out, int min, int max,
			const std::string& where, std::vector<std::string>& warnings)
		{
			const nlohmann::json* value = Child(parent, key);
			if (value == nullptr)
				return;
			if (!value->is_number())
			{
				warnings.push_back(where + key + " must be a number; using " + std::to_string(out));
				return;
			}
			const double number = value->get<double>();
			const double clamped = (std::min)((std::max)(std::round(number), static_cast<double>(min)), static_cast<double>(max));
			if (clamped != number)
				warnings.push_back(where + key + " " + FormatNumber(number) + " is out of range ("
					+ std::to_string(min) + " to " + std::to_string(max) + "); using " + FormatNumber(clamped));
			out = static_cast<int>(clamped);
		}

		inline void ReadDouble(const nlohmann::json& parent, const char* key, double& out, double min, double max,
			const std::string& where, std::vector<std::string>& warnings)
		{
			const nlohmann::json* value = Child(parent, key);
			if (value == nullptr)
				return;
			if (!value->is_number())
			{
				warnings.push_back(where + key + " must be a number; using " + FormatNumber(out));
				return;
			}
			const double number = value->get<double>();
			const double clamped = (std::min)((std::max)(number, min), max);
			if (clamped != number)
				warnings.push_back(where + key + " " + FormatNumber(number) + " is out of range ("
					+ FormatNumber(min) + " to " + FormatNumber(max) + "); using " + FormatNumber(clamped));
			out = clamped;
		}

		inline const nlohmann::json& Section(const nlohmann::json& root, const char* key, std::vector<std::string>& warnings)
		{
			static const nlohmann::json empty = nlohmann::json::object();
			const nlohmann::json* section = Child(root, key);
			if (section == nullptr)
				return empty;
			if (!section->is_object())
			{
				warnings.push_back(std::string(key) + " must be an object { ... }; using its defaults");
				return empty;
			}
			return *section;
		}
	}

	inline Settings ParseSettings(const nlohmann::json& root)
	{
		using namespace Detail;

		if (!root.is_object())
			throw std::runtime_error("the top level of config.json must be an object { ... }");

		Settings s;
		std::vector<std::string>& w = s.warnings;

		const nlohmann::json& gc = Section(root, "GarbageCollection", w);
		ReadBool(gc, "AutoGC_Enabled", s.auto_gc, "GarbageCollection.", w);
		ReadInt(gc, "AutoGC_Interval_Minutes", s.auto_gc_minutes, 1, 1440, "GarbageCollection.", w);
		ReadBool(gc, "FullPurge", s.full_purge, "GarbageCollection.", w);

		if (const nlohmann::json* cvars = Child(root, "ConsoleVariables"))
		{
			if (!cvars->is_object())
			{
				w.push_back("ConsoleVariables must be an object { \"name\": value, ... }; no console variables are set");
			}
			else
			{
				for (auto it = cvars->begin(); it != cvars->end(); ++it)
				{
					const std::string& name = it.key();
					const nlohmann::json& value = it.value();

					if (!IsValidCVarName(name))
					{
						w.push_back("ConsoleVariables: '" + name + "' is not a console variable name; skipped");
						continue;
					}

					std::string text;
					if (value.is_boolean())
						text = value.get<bool>() ? "1" : "0";
					else if (value.is_number())
						text = FormatNumber(value.get<double>());
					else if (value.is_string())
						text = Trim(value.get<std::string>());
					else
					{
						w.push_back("ConsoleVariables." + name + " must be a number, true/false or a word; skipped");
						continue;
					}

					if (!IsValidCVarValue(text))
					{
						w.push_back("ConsoleVariables." + name + ": '" + text + "' is not a valid value; skipped");
						continue;
					}

					const auto duplicate = std::find_if(s.cvars.begin(), s.cvars.end(),
						[&](const CVarSetting& existing) { return Lower(existing.name) == Lower(name); });
					if (duplicate != s.cvars.end())
					{
						w.push_back("ConsoleVariables: '" + name + "' is listed twice; the last one is used");
						duplicate->value = text;
						continue;
					}
					s.cvars.push_back({ name, text });
				}
			}
		}
		ReadInt(root, "ConsoleVariables_Reapply_Seconds", s.cvar_reapply_seconds, 0, 86400, "", w);

		const nlohmann::json& st = Section(root, "Structures", w);
		ReadBool(st, "Enabled", s.structures, "Structures.", w);
		ReadDouble(st, "NetUpdateFrequency", s.structure_net_frequency, 0.01, 100.0, "Structures.", w);
		ReadDouble(st, "MinNetUpdateFrequency", s.structure_min_net_frequency, 0.01, 100.0, "Structures.", w);
		ReadInt(st, "CheckInterval_Seconds", s.structure_check_seconds, 10, 86400, "Structures.", w);
		if (const nlohmann::json* exclude = Child(st, "ExcludeClassesContaining"))
		{
			if (!exclude->is_array())
				w.push_back("Structures.ExcludeClassesContaining must be a list [ \"...\", ... ]; nothing is excluded");
			else
				for (const nlohmann::json& entry : *exclude)
				{
					if (entry.is_string() && !Trim(entry.get<std::string>()).empty())
						s.structure_exclude.push_back(Trim(entry.get<std::string>()));
					else
						w.push_back("Structures.ExcludeClassesContaining: entries must be non-empty text; one entry skipped");
				}
		}
		if (s.structure_min_net_frequency > s.structure_net_frequency)
		{
			w.push_back("Structures.MinNetUpdateFrequency is above NetUpdateFrequency; using "
				+ FormatNumber(s.structure_net_frequency) + " for both");
			s.structure_min_net_frequency = s.structure_net_frequency;
		}

		const nlohmann::json& stats = Section(root, "Stats", w);
		ReadInt(stats, "LogInterval_Seconds", s.stats_seconds, 0, 86400, "Stats.", w);
		ReadBool(stats, "AlsoWriteToApiLog", s.stats_to_api_log, "Stats.", w);
		ReadBool(stats, "CountDinosAndStructures", s.count_actors, "Stats.", w);
		ReadBool(stats, "IncludeNetwork", s.network, "Stats.", w);
		ReadInt(stats, "LogFileMaxSize_MB", s.stats_log_max_mb, 1, 1024, "Stats.", w);
		ReadInt(stats, "LogFilesKept", s.stats_log_files, 1, 50, "Stats.", w);

		// Anything else at the top level is probably a typo.
		static const char* known[] = { "GarbageCollection", "ConsoleVariables", "ConsoleVariables_Reapply_Seconds",
			"Structures", "Stats" };
		for (auto it = root.begin(); it != root.end(); ++it)
		{
			if (std::find_if(std::begin(known), std::end(known), [&](const char* k) { return it.key() == k; }) == std::end(known))
				w.push_back("unknown setting '" + it.key() + "' is ignored");
		}

		return s;
	}

	// True when a structure's class path contains one of the exclusion entries (case-insensitive).
	inline bool IsExcluded(const std::string& class_path, const std::vector<std::string>& exclude)
	{
		return std::any_of(exclude.begin(), exclude.end(),
			[&](const std::string& part) { return ContainsNoCase(class_path, part); });
	}

	// The structure pass only ever lowers: a structure already at or below the target is left alone.
	inline bool NeedsLowering(double current, double target)
	{
		return current > target + 1e-6;
	}

	// ------------------------------------------------------------------------------------------
	// Tick rate
	// ------------------------------------------------------------------------------------------

	// Server ticks per second, sampled once per second from the engine's frame counter.
	class TickWindow
	{
	public:
		void Add(double ticks_per_second)
		{
			if (!std::isfinite(ticks_per_second) || ticks_per_second < 0.0)
				return;
			min_ = (std::min)(min_, ticks_per_second);
			max_ = (std::max)(max_, ticks_per_second);
			sum_ += ticks_per_second;
			++count_;
		}

		void Reset() { *this = TickWindow{}; }

		int Count() const { return count_; }
		double Min() const { return count_ > 0 ? min_ : 0.0; }
		double Max() const { return count_ > 0 ? max_ : 0.0; }
		double Average() const { return count_ > 0 ? sum_ / count_ : 0.0; }

	private:
		double min_ = (std::numeric_limits<double>::max)();
		double max_ = 0.0;
		double sum_ = 0.0;
		int count_ = 0;
	};

	// Frames between two samples divided by the real time between them.
	inline double TicksPerSecond(unsigned long long frames_before, unsigned long long frames_after, double seconds)
	{
		if (seconds <= 0.0 || frames_after < frames_before)
			return -1.0;
		return static_cast<double>(frames_after - frames_before) / seconds;
	}

	// ------------------------------------------------------------------------------------------
	// Network
	// ------------------------------------------------------------------------------------------

	// One player connection as the engine reports it (its rates cover the engine's last one-second
	// stat period). A field the server build doesn't have stays empty.
	struct ConnectionSample
	{
		std::optional<double> ping_seconds;
		std::optional<long long> in_bytes;      // per second
		std::optional<long long> out_bytes;     // per second
		std::optional<long long> in_packets;    // per second
		std::optional<long long> out_packets;   // per second
		std::optional<long long> in_lost;       // packets lost in the last period
		std::optional<long long> out_lost;
		std::optional<long long> queued_bits;   // above 0: the connection has more to send than its rate allows
	};

	struct NetStats
	{
		int samples = 0;            // seconds sampled
		int connections = -1;       // at the last sample
		double out_kbps_avg = -1.0; // all connections together, KB/s
		double out_kbps_peak = -1.0;
		double in_kbps_avg = -1.0;
		double in_kbps_peak = -1.0;
		double ping_ms_avg = -1.0;
		double ping_ms_max = -1.0;
		double out_loss_pct = -1.0;
		double in_loss_pct = -1.0;
		double saturated_pct = -1.0; // share of connection-seconds that were bandwidth-limited
	};

	// Server-wide network figures, sampled once per second over a window.
	class NetWindow
	{
	public:
		void Add(const std::vector<ConnectionSample>& connections)
		{
			++samples_;
			connections_ = static_cast<int>(connections.size());

			long long out_total = 0;
			long long in_total = 0;
			bool have_out = false;
			bool have_in = false;
			for (const ConnectionSample& c : connections)
			{
				if (c.out_bytes) { out_total += (std::max)(0LL, *c.out_bytes); have_out = true; }
				if (c.in_bytes) { in_total += (std::max)(0LL, *c.in_bytes); have_in = true; }
				if (c.ping_seconds && *c.ping_seconds >= 0.0)
				{
					ping_sum_ += *c.ping_seconds;
					ping_max_ = (std::max)(ping_max_, *c.ping_seconds);
					++ping_count_;
				}
				if (c.out_packets && c.out_lost)
				{
					out_packets_ += (std::max)(0LL, *c.out_packets);
					out_lost_ += (std::max)(0LL, *c.out_lost);
					have_out_loss_ = true;
				}
				if (c.in_packets && c.in_lost)
				{
					in_packets_ += (std::max)(0LL, *c.in_packets);
					in_lost_ += (std::max)(0LL, *c.in_lost);
					have_in_loss_ = true;
				}
				if (c.queued_bits)
				{
					++queued_samples_;
					if (*c.queued_bits > 0)
						++saturated_samples_;
				}
			}
			// A second with no players counts as 0 traffic, once the build is known to report it.
			have_out_ = have_out_ || have_out;
			have_in_ = have_in_ || have_in;
			out_sum_ += static_cast<double>(out_total);
			in_sum_ += static_cast<double>(in_total);
			out_peak_ = (std::max)(out_peak_, static_cast<double>(out_total));
			in_peak_ = (std::max)(in_peak_, static_cast<double>(in_total));
		}

		void Reset() { *this = NetWindow{}; }

		int Samples() const { return samples_; }

		NetStats Summary() const
		{
			NetStats s;
			s.samples = samples_;
			if (samples_ == 0)
				return s;
			s.connections = connections_;
			if (have_out_)
			{
				s.out_kbps_avg = out_sum_ / samples_ / 1024.0;
				s.out_kbps_peak = out_peak_ / 1024.0;
			}
			if (have_in_)
			{
				s.in_kbps_avg = in_sum_ / samples_ / 1024.0;
				s.in_kbps_peak = in_peak_ / 1024.0;
			}
			if (ping_count_ > 0)
			{
				s.ping_ms_avg = ping_sum_ / ping_count_ * 1000.0;
				s.ping_ms_max = ping_max_ * 1000.0;
			}
			if (have_out_loss_)
				s.out_loss_pct = LossPercent(out_lost_, out_packets_);
			if (have_in_loss_)
				s.in_loss_pct = LossPercent(in_lost_, in_packets_);
			if (queued_samples_ > 0)
				s.saturated_pct = 100.0 * static_cast<double>(saturated_samples_) / static_cast<double>(queued_samples_);
			return s;
		}

	private:
		// Lost packets as a share of packets sent (or received) plus lost.
		static double LossPercent(long long lost, long long packets)
		{
			const long long total = lost + packets;
			return total > 0 ? 100.0 * static_cast<double>(lost) / static_cast<double>(total) : 0.0;
		}

		int samples_ = 0;
		int connections_ = 0;
		bool have_out_ = false;
		bool have_in_ = false;
		double out_sum_ = 0.0;
		double in_sum_ = 0.0;
		double out_peak_ = 0.0;
		double in_peak_ = 0.0;
		double ping_sum_ = 0.0;
		double ping_max_ = 0.0;
		long long ping_count_ = 0;
		bool have_out_loss_ = false;
		bool have_in_loss_ = false;
		long long out_packets_ = 0;
		long long out_lost_ = 0;
		long long in_packets_ = 0;
		long long in_lost_ = 0;
		long long queued_samples_ = 0;
		long long saturated_samples_ = 0;
	};

	// ------------------------------------------------------------------------------------------
	// Stats report
	// ------------------------------------------------------------------------------------------

	struct Snapshot
	{
		long long uptime_seconds = 0;
		int players = -1;

		// Tick rate over the current window
		int tick_samples = 0;
		double tick_avg = 0.0;
		double tick_min = 0.0;
		double tick_max = 0.0;

		// UObjects: live objects, array slots in use, array capacity, objects outside GC
		long long uobjects = -1;
		long long uobject_slots = -1;
		long long uobject_capacity = -1;
		long long objects_outside_gc = -1;

		long long dinos = -1;
		long long structures = -1;

		// Process / machine memory in MB
		double working_set_mb = -1.0;
		double peak_working_set_mb = -1.0;
		double private_mb = -1.0;
		double system_available_mb = -1.0;
		double system_total_mb = -1.0;

		long long forced_gcs = 0;
		long long seconds_since_forced_gc = -1;   // -1 = none yet

		bool network = false;   // network figures were collected
		NetStats net;

		std::vector<std::pair<std::string, std::string>> cvars;   // name, current engine value ("?" = unknown)
	};

	inline std::string FormatDuration(long long seconds)
	{
		if (seconds < 0)
			return "never";
		const long long d = seconds / 86400;
		const long long h = (seconds % 86400) / 3600;
		const long long m = (seconds % 3600) / 60;
		const long long s = seconds % 60;
		char buffer[64];
		if (d > 0)
			std::snprintf(buffer, sizeof(buffer), "%lldd %lldh %lldm", d, h, m);
		else if (h > 0)
			std::snprintf(buffer, sizeof(buffer), "%lldh %lldm", h, m);
		else if (m > 0)
			std::snprintf(buffer, sizeof(buffer), "%lldm %llds", m, s);
		else
			std::snprintf(buffer, sizeof(buffer), "%llds", s);
		return buffer;
	}

	namespace Detail
	{
		inline std::string Count(long long value) { return value < 0 ? "?" : std::to_string(value); }
		inline std::string Mb(double value) { return value < 0 ? "?" : FormatNumber(std::round(value)) + " MB"; }
		inline std::string Tick(double value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%.1f", value);
			return buffer;
		}
		// Rounded whole number, "?" when unknown (negative).
		inline std::string Num(double value) { return value < 0 ? "?" : FormatNumber(std::round(value)); }
		// One decimal, "?" when unknown.
		inline std::string Pct(double value) { return value < 0 ? "?" : Tick(value); }
	}

	// One line describing the network window, for the Stats command.
	inline std::string FormatNetText(const NetStats& n)
	{
		using namespace Detail;
		if (n.samples == 0)
			return "Network: no samples yet";
		return "Network (last " + std::to_string(n.samples) + "s): " + Count(n.connections) + " connections, out avg "
			+ Num(n.out_kbps_avg) + " KB/s (peak " + Num(n.out_kbps_peak) + "), in avg " + Num(n.in_kbps_avg) + " KB/s (peak "
			+ Num(n.in_kbps_peak) + "), ping avg " + Num(n.ping_ms_avg) + " ms (max " + Num(n.ping_ms_max) + "), loss out "
			+ Pct(n.out_loss_pct) + "% / in " + Pct(n.in_loss_pct) + "%, bandwidth-limited " + Pct(n.saturated_pct) + "% of the time";
	}

	// Multi-line text for the Stats command.
	inline std::string FormatStatsText(const Snapshot& s)
	{
		using namespace Detail;
		std::string out;
		out += "Uptime " + FormatDuration(s.uptime_seconds) + ", players " + Count(s.players) + "\n";
		if (s.tick_samples > 0)
			out += "Tick rate (last " + std::to_string(s.tick_samples) + "s): avg " + Tick(s.tick_avg) + ", min "
				+ Tick(s.tick_min) + ", max " + Tick(s.tick_max) + "\n";
		else
			out += "Tick rate: no samples yet\n";
		out += "UObjects: " + Count(s.uobjects) + " live, " + Count(s.uobject_slots) + " slots used of "
			+ Count(s.uobject_capacity) + ", " + Count(s.objects_outside_gc) + " outside GC\n";
		if (s.dinos >= 0 || s.structures >= 0)
			out += "Dinos " + Count(s.dinos) + ", structures " + Count(s.structures) + "\n";
		out += "Memory: working set " + Mb(s.working_set_mb) + " (peak " + Mb(s.peak_working_set_mb) + "), private "
			+ Mb(s.private_mb) + "; machine " + Mb(s.system_available_mb) + " free of " + Mb(s.system_total_mb) + "\n";
		if (s.network)
			out += FormatNetText(s.net) + "\n";
		out += "Forced GCs: " + std::to_string(s.forced_gcs) + ", last " + (s.seconds_since_forced_gc < 0
			? std::string("never") : FormatDuration(s.seconds_since_forced_gc) + " ago") + "\n";
		for (const auto& [name, value] : s.cvars)
			out += name + " = " + value + "\n";
		if (!out.empty() && out.back() == '\n')
			out.pop_back();
		return out;
	}

	// One line for the stats log.
	inline std::string FormatStatsLine(const Snapshot& s)
	{
		using namespace Detail;
		std::string out = "players=" + Count(s.players);
		out += " tick_avg=" + Tick(s.tick_avg) + " tick_min=" + Tick(s.tick_min) + " tick_max=" + Tick(s.tick_max);
		out += " uobjects=" + Count(s.uobjects) + " uobject_slots=" + Count(s.uobject_slots);
		if (s.dinos >= 0) out += " dinos=" + Count(s.dinos);
		if (s.structures >= 0) out += " structures=" + Count(s.structures);
		out += " ws_mb=" + (s.working_set_mb < 0 ? std::string("?") : FormatNumber(std::round(s.working_set_mb)));
		out += " private_mb=" + (s.private_mb < 0 ? std::string("?") : FormatNumber(std::round(s.private_mb)));
		out += " machine_free_mb=" + (s.system_available_mb < 0 ? std::string("?") : FormatNumber(std::round(s.system_available_mb)));
		if (s.network)
		{
			out += " connections=" + Count(s.net.connections);
			out += " out_kbps_avg=" + Num(s.net.out_kbps_avg) + " out_kbps_peak=" + Num(s.net.out_kbps_peak);
			out += " in_kbps_avg=" + Num(s.net.in_kbps_avg) + " in_kbps_peak=" + Num(s.net.in_kbps_peak);
			out += " ping_ms_avg=" + Num(s.net.ping_ms_avg) + " ping_ms_max=" + Num(s.net.ping_ms_max);
			out += " out_loss_pct=" + Pct(s.net.out_loss_pct) + " in_loss_pct=" + Pct(s.net.in_loss_pct);
			out += " saturated_pct=" + Pct(s.net.saturated_pct);
		}
		out += " forced_gcs=" + std::to_string(s.forced_gcs);
		return out;
	}

	// JSON for the Stats command with the "json" argument (for scripts and monitoring tools).
	inline nlohmann::json StatsJson(const Snapshot& s)
	{
		auto number_or_null = [](double value) -> nlohmann::json { return value < 0 ? nlohmann::json() : nlohmann::json(std::round(value)); };
		auto count_or_null = [](long long value) -> nlohmann::json { return value < 0 ? nlohmann::json() : nlohmann::json(value); };

		auto value_or_null = [](double value) -> nlohmann::json { return value < 0 ? nlohmann::json() : nlohmann::json(value); };

		nlohmann::json cvars = nlohmann::json::object();
		for (const auto& [name, value] : s.cvars)
			cvars[name] = value;

		return {
			{ "uptime_seconds", s.uptime_seconds },
			{ "players", count_or_null(s.players) },
			{ "tick", { { "samples", s.tick_samples }, { "avg", s.tick_avg }, { "min", s.tick_min }, { "max", s.tick_max } } },
			{ "uobjects", { { "live", count_or_null(s.uobjects) }, { "slots_used", count_or_null(s.uobject_slots) },
				{ "capacity", count_or_null(s.uobject_capacity) }, { "outside_gc", count_or_null(s.objects_outside_gc) } } },
			{ "dinos", count_or_null(s.dinos) },
			{ "structures", count_or_null(s.structures) },
			{ "memory_mb", { { "working_set", number_or_null(s.working_set_mb) }, { "peak_working_set", number_or_null(s.peak_working_set_mb) },
				{ "private", number_or_null(s.private_mb) }, { "machine_available", number_or_null(s.system_available_mb) },
				{ "machine_total", number_or_null(s.system_total_mb) } } },
			{ "forced_gcs", s.forced_gcs },
			{ "seconds_since_forced_gc", count_or_null(s.seconds_since_forced_gc) },
			{ "cvars", cvars },
			{ "network", !s.network ? nlohmann::json() : nlohmann::json{
				{ "samples", s.net.samples }, { "connections", count_or_null(s.net.connections) },
				{ "out_kbps_avg", value_or_null(s.net.out_kbps_avg) }, { "out_kbps_peak", value_or_null(s.net.out_kbps_peak) },
				{ "in_kbps_avg", value_or_null(s.net.in_kbps_avg) }, { "in_kbps_peak", value_or_null(s.net.in_kbps_peak) },
				{ "ping_ms_avg", value_or_null(s.net.ping_ms_avg) }, { "ping_ms_max", value_or_null(s.net.ping_ms_max) },
				{ "out_loss_pct", value_or_null(s.net.out_loss_pct) }, { "in_loss_pct", value_or_null(s.net.in_loss_pct) },
				{ "saturated_pct", value_or_null(s.net.saturated_pct) } } },
		};
	}
} // namespace Perf
