#pragma once

// ASE (ArkServerApi) side of the engine access ServerPerformance needs: running engine console
// commands and capturing what they print, requesting a garbage collection, and reading the frame
// counter, the UObject array and process memory. Main.cpp only talks to the engine through this
// header (and the server API utilities), so the ASA build differs here and in Main.cpp only.

#include "API/ARK/Ark.h"
#include "Logger/Logger.h"
#include "Tools.h"

#include <psapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "PerfLogic.h"

namespace Engine
{
	// --------------------------------------------------------------------------------------------
	// Symbols. ArkServerApi reads every function, field and global from the server's PDB. Each name
	// this plugin needs is checked first; a missing symbol switches off only the feature that needs
	// it, with an error in the log.
	// --------------------------------------------------------------------------------------------

	namespace Detail
	{
		inline bool g_exec = false;
		inline bool g_purge_field = false;
		inline bool g_gc = false;
		inline bool g_frames = false;
		inline bool g_objects = false;
		inline bool g_players = false;
		inline bool g_classes = false;     // class lookups, class defaults, class paths
		inline bool g_structures = false;  // g_classes + object-of-class lookup + NetUpdateFrequency
		inline bool g_dinos = false;
		inline bool g_net_connection = false;   // APlayerController.NetConnection
		inline bool g_net_fields[8] = {};       // the UNetConnection fields in kNetFields order

		inline constexpr const char* kNetFields[8] = {
			"UNetConnection.AvgLag", "UNetConnection.InBytesPerSecond", "UNetConnection.OutBytesPerSecond",
			"UNetConnection.InPacketsPerSecond", "UNetConnection.OutPacketsPerSecond", "UNetConnection.InPacketsLost",
			"UNetConnection.OutPacketsLost", "UNetConnection.QueuedBits",
		};

		// Newer AseApi builds export a silent lookup that says whether a name exists (FindNativeOffset).
		// It is looked up at run time so this file builds against either API version; nullptr on an API without it.
		using FindNativeOffsetFn = bool (*)(const char*, intptr_t*);
		inline FindNativeOffsetFn NativeFinder()
		{
			static const FindNativeOffsetFn finder = []() -> FindNativeOffsetFn
			{
				HMODULE api = nullptr;
				const auto anchor = static_cast<LPVOID (*)(const std::string&)>(&GetDataAddress);
				if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCWSTR>(anchor), &api) || api == nullptr)
					return nullptr;
				return reinterpret_cast<FindNativeOffsetFn>(reinterpret_cast<void*>(
					GetProcAddress(api, "?FindNativeOffset@@YA_NPEBDPEA_J@Z")));
			}();
			return finder;
		}

		// Without that export (AseApi 3.56 and older) an unknown name resolves to offset 0, so it is
		// compared with a name that certainly doesn't exist.
		inline bool HasFunction(const char* name)
		{
			if (const FindNativeOffsetFn find = NativeFinder())
				return find(name, nullptr);
			static const LPVOID missing = GetAddress(std::string("ServerPerformance.NoSuchFunction"));
			return GetAddress(std::string(name)) != missing;
		}

		inline bool HasData(const char* name)
		{
			if (const FindNativeOffsetFn find = NativeFinder())
				return find(name, nullptr);
			static const LPVOID missing = GetDataAddress(std::string("Global.ServerPerformanceNoSuchData"));
			return GetDataAddress(std::string(name)) != missing;
		}

		inline bool HasField(const char* name)
		{
			if (const FindNativeOffsetFn find = NativeFinder())
				return find(name, nullptr);
			return GetAddress(static_cast<const void*>(nullptr), std::string(name)) != 0;
		}

		inline bool All(std::initializer_list<bool> checks)
		{
			for (const bool ok : checks)
				if (!ok) return false;
			return true;
		}
	}

	inline void RequireSymbols()
	{
		using namespace Detail;
		struct Check { const char* name; bool ok; };
		const Check checks[] = {
			{ "Global.GEngine", HasData("Global.GEngine") },
			{ "Global.IConsoleManager::Singleton", HasData("Global.IConsoleManager::Singleton") },
			{ "FConsoleManager.ProcessUserConsoleInput", HasFunction("FConsoleManager.ProcessUserConsoleInput") },
			{ "UEngine.TimeBetweenPurgingPendingKillObjects", HasField("UEngine.TimeBetweenPurgingPendingKillObjects") },
			{ "UWorld.ForceGarbageCollection", HasFunction("UWorld.ForceGarbageCollection") },
			{ "Global.GFrameCounter", HasData("Global.GFrameCounter") },
			{ "Global.GUObjectArray", HasData("Global.GUObjectArray") },
			{ "UObjectBase.InternalIndex", HasField("UObjectBase.InternalIndex") },
			{ "UWorld.PlayerControllerList", HasField("UWorld.PlayerControllerList") },
			{ "UObjectBase.Class", HasField("UObjectBase.Class") },
			{ "UClass.GetDefaultObject", HasFunction("UClass.GetDefaultObject") },
			{ "UVictoryCore.ClassToStringReference", HasFunction("UVictoryCore.ClassToStringReference") },
			{ "Global.GetObjectsOfClass", HasFunction("Global.GetObjectsOfClass") },
			{ "AActor.NetUpdateFrequency", HasField("AActor.NetUpdateFrequency") },
			{ "APrimalStructure.GetPrivateStaticClass", HasFunction("APrimalStructure.GetPrivateStaticClass") },
			{ "APrimalDinoCharacter.GetPrivateStaticClass", HasFunction("APrimalDinoCharacter.GetPrivateStaticClass") },
		};
		for (const Check& check : checks)
			if (!check.ok)
				Log::GetLog()->error("Server build has no {}; the features that need it are off", check.name);

		auto ok = [&](const char* name)
		{
			for (const Check& check : checks)
				if (std::string(check.name) == name) return check.ok;
			return false;
		};

		const bool engine = ok("Global.GEngine");
		g_exec = ok("Global.IConsoleManager::Singleton") && ok("FConsoleManager.ProcessUserConsoleInput");
		g_purge_field = engine && ok("UEngine.TimeBetweenPurgingPendingKillObjects");
		g_gc = ok("UWorld.ForceGarbageCollection");
		g_frames = ok("Global.GFrameCounter");
		g_objects = ok("Global.GUObjectArray") && ok("UObjectBase.InternalIndex");
		g_players = ok("UWorld.PlayerControllerList");
		g_classes = All({ ok("UObjectBase.Class"), ok("UClass.GetDefaultObject"), ok("UVictoryCore.ClassToStringReference"),
			ok("Global.GetObjectsOfClass") });
		g_structures = g_classes && ok("AActor.NetUpdateFrequency") && ok("APrimalStructure.GetPrivateStaticClass");
		g_dinos = g_classes && ok("APrimalDinoCharacter.GetPrivateStaticClass");

		// Network stats: each connection field is optional; only the controller's connection is required.
		g_net_connection = HasField("APlayerController.NetConnection");
		if (!g_net_connection)
			Log::GetLog()->error("Server build has no APlayerController.NetConnection; network stats are off");
		for (int i = 0; i < 8; ++i)
		{
			g_net_fields[i] = HasField(kNetFields[i]);
			if (g_net_connection && !g_net_fields[i])
				Log::GetLog()->warn("Server build has no {}; that network figure shows as ?", kNetFields[i]);
		}
	}

	inline bool HasFrameCounter() { return Detail::g_frames; }
	inline bool HasStructures() { return Detail::g_structures; }
	inline bool HasMinNetUpdateFrequency() { return false; }   // not in this engine version

	// --------------------------------------------------------------------------------------------
	// Console commands
	// --------------------------------------------------------------------------------------------

	// Collects what the engine prints while running a command. Its virtual functions are in the order
	// of this engine version's FOutputDevice, followed by its two flags, so the engine can use it
	// wherever it expects an FOutputDevice (the engine only calls Serialize on it here).
	class ExecOutput
	{
	public:
		virtual ~ExecOutput() = default;
		virtual void Serialize(const wchar_t* text, unsigned char /*verbosity*/, const void* /*category*/) { Append(text); }
		virtual void Flush() {}
		virtual void TearDown() {}
		virtual void Dump(void* /*archive*/) {}
		virtual bool IsMemoryOnly() const { return true; }
		virtual bool CanBeUsedOnAnyThread() const { return false; }

		bool suppress_event_tag = true;
		bool auto_emit_line_terminator = false;

		std::wstring text;

	private:
		void Append(const wchar_t* line)
		{
			if (line == nullptr)
				return;
			text += line;
			text += L'\n';
		}
	};

	struct ExecResult
	{
		bool handled = false;
		std::string output;
	};

	// Runs one console-variable query or assignment on the game thread through the engine's console
	// manager (FConsoleManager::ProcessUserConsoleInput), the part of the engine that handles
	// "name" and "name value". Same path as the ASA build, where UEngine::Exec crashes on the
	// dedicated server. Only call with text built from validated names and values (see
	// Perf::IsValidCVarName / IsValidCVarValue).
	inline ExecResult Exec(const std::string& command)
	{
		ExecResult result;
		if (!Detail::g_exec)
			return result;
		void* manager = *GetNativeDataPointerField<void**>("Global.IConsoleManager::Singleton");
		if (manager == nullptr)
			return result;

		const std::wstring wide = ArkApi::Tools::Utf8Decode(command);
		ExecOutput output;
		result.handled = NativeCall<bool, const wchar_t*, ExecOutput*, UWorld*>(manager, "FConsoleManager.ProcessUserConsoleInput",
			wide.c_str(), &output, ArkApi::GetApiUtils().GetWorld());
		result.output = ArkApi::Tools::Utf8Encode(output.text);
		return result;
	}

	// This engine version keeps the time between garbage collections as an engine setting, not a
	// console variable. Reading and setting it under its later console-variable name lets the same
	// config work on both games.
	inline constexpr const char* kPurgeIntervalName = "gc.TimeBetweenPurgingPendingKillObjects";

	inline bool IsPurgeInterval(const std::string& name)
	{
		return _stricmp(name.c_str(), kPurgeIntervalName) == 0;
	}

	inline std::string FormatFloat(float value)
	{
		char buffer[32];
		std::snprintf(buffer, sizeof(buffer), "%f", value);
		return buffer;
	}

	// Asking the engine for a variable's value: running its bare name prints "name = value".
	inline ExecResult QueryCVar(const std::string& name)
	{
		if (IsPurgeInterval(name))
		{
			ExecResult result;
			UEngine* engine = Detail::g_purge_field ? Globals::GEngine()() : nullptr;
			if (engine == nullptr)
				return result;
			result.handled = true;
			result.output = name + " = " + FormatFloat(engine->TimeBetweenPurgingPendingKillObjectsField());
			return result;
		}
		return Exec(name);
	}

	inline ExecResult SetCVar(const std::string& name, const std::string& value)
	{
		if (IsPurgeInterval(name))
		{
			ExecResult result;
			UEngine* engine = Detail::g_purge_field ? Globals::GEngine()() : nullptr;
			if (engine == nullptr)
				return result;
			char* end = nullptr;
			const float seconds = std::strtof(value.c_str(), &end);
			if (end == nullptr || *end != '\0' || seconds < 0.f)
			{
				result.handled = true;
				result.output = "not a number of seconds";
				return result;
			}
			engine->TimeBetweenPurgingPendingKillObjectsField() = seconds;
			result.handled = true;
			result.output = name + " = " + FormatFloat(seconds);
			return result;
		}
		return Exec(name + " " + value);
	}

	// --------------------------------------------------------------------------------------------
	// Garbage collection
	// --------------------------------------------------------------------------------------------

	// Hands the request to the engine, which collects at its next safe point in the frame.
	inline bool RequestGarbageCollection(bool full_purge)
	{
		UWorld* world = Detail::g_gc ? ArkApi::GetApiUtils().GetWorld() : nullptr;
		if (world == nullptr)
			return false;
		world->ForceGarbageCollection(full_purge);
		return true;
	}

	// --------------------------------------------------------------------------------------------
	// Numbers for stats
	// --------------------------------------------------------------------------------------------

	inline unsigned long long FrameCounter()
	{
		return Detail::g_frames ? *GetNativeDataPointerField<unsigned long long*>("Global.GFrameCounter") : 0;
	}

	struct ObjectCounts
	{
		long long live = -1;
		long long slots = -1;
		long long capacity = -1;
		long long outside_gc = -1;
	};

	namespace Detail
	{
		// Counts the non-empty slots of the UObject array. Every sampled object must carry its own
		// slot number (UObjectBase::InternalIndex); if one doesn't, the array layout in the server API
		// headers this was built with is not the running server's, and the walk stops with false
		// instead of reading the wrong memory. A read fault has the same result. No C++ objects live in
		// this function, so it can use a structured exception handler.
		inline bool WalkObjectArray(FChunkedFixedUObjectArray& objects, DWORD64 index_offset, long long& live)
		{
#ifdef _MSC_VER
			__try
			{
#endif
				long long count = 0;
				for (int i = 0; i < objects.NumElements; ++i)
				{
					const FUObjectItem* item = objects.GetObjectPtr(i);
					const UObject* object = item->Object;
					if (object == nullptr)
						continue;
					if (count < 256 || (count & 4095) == 0)
					{
						const int stored = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(object) + index_offset);
						if (stored != i)
							return false;
					}
					++count;
				}
				live = count;
				return true;
#ifdef _MSC_VER
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#endif
		}
	}

	// Walks the UObject array (pointer reads only, a few milliseconds for a few million entries).
	inline ObjectCounts CountObjects()
	{
		ObjectCounts counts;
		if (!Detail::g_objects)
			return counts;

		static bool reported = false;
		auto layout_mismatch = [&]
		{
			if (!reported)
				Log::GetLog()->error("The UObject array does not look like the server API headers this plugin was built with; "
					"UObject counts are unavailable. Rebuild against the server API version the servers run.");
			reported = true;
			return counts;
		};

		FUObjectArray& array = Globals::GUObjectArray()();
		FChunkedFixedUObjectArray& objects = array.ObjObjects;
		constexpr int kPerChunk = FChunkedFixedUObjectArray::ElementsPerChunk;
		if (objects.Objects == nullptr || objects.NumElements < 0 || objects.MaxElements <= 0
			|| objects.NumElements > objects.MaxElements || objects.MaxElements > (1 << 28)
			|| objects.NumChunks < (objects.NumElements + kPerChunk - 1) / kPerChunk || objects.NumChunks > objects.MaxChunks)
			return layout_mismatch();

		long long live = 0;
		if (!Detail::WalkObjectArray(objects, GetAddress(static_cast<const void*>(nullptr), std::string("UObjectBase.InternalIndex")), live))
			return layout_mismatch();

		counts.live = live;
		counts.slots = objects.NumElements;
		counts.capacity = objects.MaxElements;
		counts.outside_gc = array.ObjLastNonGCIndex >= 0 ? array.ObjLastNonGCIndex + 1 : 0;
		return counts;
	}

	inline int PlayerCount()
	{
		UWorld* world = Detail::g_players ? ArkApi::GetApiUtils().GetWorld() : nullptr;
		return world != nullptr ? world->PlayerControllerListField().Num() : -1;
	}

	// All live objects of a class and its subclasses, class defaults and objects waiting for GC excluded.
	inline TArray<UObject*> ObjectsOfClass(UClass* the_class)
	{
		TArray<UObject*> results;
		if (the_class != nullptr && Detail::g_classes)
			Globals::GetObjectsOfClass(the_class, &results, true,
				static_cast<EObjectFlags>(static_cast<int>(EObjectFlags::RF_ClassDefaultObject) | static_cast<int>(EObjectFlags::RF_PendingKill)));
		return results;
	}

	inline UClass* StructureClass() { return Detail::g_structures ? APrimalStructure::GetPrivateStaticClass() : nullptr; }
	inline UClass* DinoClass() { return Detail::g_dinos ? APrimalDinoCharacter::GetPrivateStaticClass() : nullptr; }

	inline long long CountOfClass(UClass* the_class)
	{
		return the_class != nullptr ? ObjectsOfClass(the_class).Num() : -1;
	}

	inline std::string ClassPath(UClass* the_class)
	{
		return ArkApi::IApiUtils::GetClassBlueprint(the_class).ToString();
	}

	inline UClass* ClassOf(UObject* object)
	{
		return object != nullptr ? object->ClassField() : nullptr;
	}

	inline AActor* DefaultActor(UClass* the_class)
	{
		return the_class != nullptr ? static_cast<AActor*>(the_class->GetDefaultObject(true)) : nullptr;
	}

	inline float NetUpdateFrequency(AActor* actor) { return actor->NetUpdateFrequencyField(); }
	inline void SetNetUpdateFrequency(AActor* actor, float value) { actor->NetUpdateFrequencyField() = value; }
	// Not in this engine version (HasMinNetUpdateFrequency() is false, so these are never called).
	inline float MinNetUpdateFrequency(AActor*) { return 0.f; }
	inline void SetMinNetUpdateFrequency(AActor*, float) {}

	// --------------------------------------------------------------------------------------------
	// Network
	// --------------------------------------------------------------------------------------------

	inline bool HasNetwork() { return Detail::g_net_connection; }

	namespace Detail
	{
		inline std::optional<long long> IntField(UNetConnection* connection, bool available, const char* name)
		{
			if (!available)
				return std::nullopt;
			return *GetNativePointerField<int*>(connection, name);
		}
	}

	// One sample per connected player. The engine updates these rates once per second. Fields this
	// server build doesn't have stay empty and show as "?" in the stats.
	inline std::vector<Perf::ConnectionSample> ReadConnections()
	{
		using namespace Detail;
		std::vector<Perf::ConnectionSample> out;
		UWorld* world = g_net_connection && g_players ? ArkApi::GetApiUtils().GetWorld() : nullptr;
		if (world == nullptr)
			return out;
		for (auto& weak_controller : world->PlayerControllerListField())
		{
			APlayerController* controller = weak_controller.Get();
			UNetConnection* connection = controller != nullptr ? controller->NetConnectionField() : nullptr;
			if (connection == nullptr)
				continue;
			Perf::ConnectionSample sample;
			if (g_net_fields[0])
				sample.ping_seconds = *GetNativePointerField<float*>(connection, "UNetConnection.AvgLag");
			sample.in_bytes = IntField(connection, g_net_fields[1], "UNetConnection.InBytesPerSecond");
			sample.out_bytes = IntField(connection, g_net_fields[2], "UNetConnection.OutBytesPerSecond");
			sample.in_packets = IntField(connection, g_net_fields[3], "UNetConnection.InPacketsPerSecond");
			sample.out_packets = IntField(connection, g_net_fields[4], "UNetConnection.OutPacketsPerSecond");
			sample.in_lost = IntField(connection, g_net_fields[5], "UNetConnection.InPacketsLost");
			sample.out_lost = IntField(connection, g_net_fields[6], "UNetConnection.OutPacketsLost");
			sample.queued_bits = IntField(connection, g_net_fields[7], "UNetConnection.QueuedBits");
			out.push_back(sample);
		}
		return out;
	}

	struct MemoryInfo
	{
		double working_set_mb = -1.0;
		double peak_working_set_mb = -1.0;
		double private_mb = -1.0;
		double system_available_mb = -1.0;
		double system_total_mb = -1.0;
	};

	inline MemoryInfo Memory()
	{
		constexpr double mb = 1024.0 * 1024.0;
		MemoryInfo info;

		PROCESS_MEMORY_COUNTERS_EX process{};
		process.cb = sizeof(process);
		if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)))
		{
			info.working_set_mb = static_cast<double>(process.WorkingSetSize) / mb;
			info.peak_working_set_mb = static_cast<double>(process.PeakWorkingSetSize) / mb;
			info.private_mb = static_cast<double>(process.PrivateUsage) / mb;
		}

		MEMORYSTATUSEX system{};
		system.dwLength = sizeof(system);
		if (GlobalMemoryStatusEx(&system))
		{
			info.system_available_mb = static_cast<double>(system.ullAvailPhys) / mb;
			info.system_total_mb = static_cast<double>(system.ullTotalPhys) / mb;
		}
		return info;
	}
} // namespace Engine
