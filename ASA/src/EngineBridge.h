#pragma once

// ASA (AsaApi) side of the engine access ServerPerformance needs: running engine console commands and
// capturing what they print, requesting a garbage collection, and reading the frame counter, the
// UObject array and process memory. Main.cpp only talks to the engine through this header (and the
// AsaApi utilities), so the ASE build differs here and in Main.cpp only.

#include "API/ARK/Ark.h"

#include <psapi.h>

#include <optional>
#include <string>
#include <vector>

#include "PerfLogic.h"

namespace Engine
{
	// --------------------------------------------------------------------------------------------
	// Offsets this plugin calls or reads. RequireSymbols() touches each one while the plugin loads:
	// AsaApi refuses to load a plugin that asks for a missing offset during Plugin_Init (it logs the
	// name and the server carries on), whereas a missing offset asked for later stops the server.
	// So a game update that removes one of these makes the plugin fail to load, never crash later.
	// --------------------------------------------------------------------------------------------

	inline constexpr const char* kFunctions[] = {
		"UEngine.Exec(UWorld*,wchar_t*,FOutputDevice&)",
		"UEngine.ForceGarbageCollection(bool)",
		"Global.GetObjectsOfClass(UClass*,TArray<UObject*,TSizedDefaultAllocator<32>>&,bool,EObjectFlags,EInternalObjectFlags)",
		"UClass.GetDefaultObject(bool)",
		"UVictoryCore.ClassToStringReference(TSubclassOf<UObject>)",
		"AActor.GetNetUpdateFrequency()",
		"AActor.SetNetUpdateFrequency(float)",
		"AActor.GetMinNetUpdateFrequency()",
		"AActor.SetMinNetUpdateFrequency(float)",
	};
	inline constexpr const char* kData[] = {
		"Global.GEngine",
		"Global.GFrameCounter",
		"Global.GUObjectArray",
	};
	inline constexpr const char* kFields[] = {
		"UWorld.PlayerControllerList",
		"UObjectBase.ClassPrivate",
		"UObjectBase.InternalIndex",
		"APlayerController.NetConnection",
		"UNetConnection.AvgLag",
		"UNetConnection.InBytesPerSecond",
		"UNetConnection.OutBytesPerSecond",
		"UNetConnection.InPacketsPerSecond",
		"UNetConnection.OutPacketsPerSecond",
		"UNetConnection.InPacketsLost",
		"UNetConnection.OutPacketsLost",
		"UNetConnection.QueuedBits",
	};

	inline void RequireSymbols()
	{
		for (const char* name : kFunctions)
			(void)GetAddress(std::string(name));
		for (const char* name : kData)
			(void)GetDataAddress(std::string(name));
		for (const char* name : kFields)
			(void)GetAddress(static_cast<const void*>(nullptr), std::string(name));
	}

	// What this build can do. Everything on ASA (RequireSymbols has checked it); the ASE build answers
	// per feature.
	inline bool HasFrameCounter() { return true; }
	inline bool HasStructures() { return true; }
	inline bool HasMinNetUpdateFrequency() { return true; }

	// --------------------------------------------------------------------------------------------
	// Console commands
	// --------------------------------------------------------------------------------------------

	// Collects what the engine prints while running a command. The virtual functions are declared in
	// exactly the order of the engine's FOutputDevice (API/UE/Misc/OutputDevice.h), followed by its two
	// flags, so the engine can use it wherever it expects an FOutputDevice. It is a separate class
	// rather than a subclass because FOutputDevice::SerializeRecord has no definition outside the
	// engine, and a subclass would need one to link.
	class ExecOutput
	{
	public:
		virtual ~ExecOutput() = default;
		virtual void Serialize(const TCHAR* text, ELogVerbosity::Type /*verbosity*/, const FName& /*category*/) { Append(text); }
		virtual void Serialize(const TCHAR* text, ELogVerbosity::Type /*verbosity*/, const FName& /*category*/, const double /*time*/) { Append(text); }
		virtual void SerializeRecord(const UE::FLogRecord& /*record*/) {}
		virtual void Flush() {}
		virtual void TearDown() {}
		virtual void Dump(class FArchive& /*archive*/) {}
		virtual bool IsMemoryOnly() const { return true; }
		virtual bool CanBeUsedOnAnyThread() const { return false; }
		virtual bool CanBeUsedOnMultipleThreads() const { return false; }
		virtual bool CanBeUsedOnPanicThread() const { return false; }

		// Same position and meaning as FOutputDevice's protected members.
		bool suppress_event_tag = true;
		bool auto_emit_line_terminator = false;

		std::wstring text;

	private:
		void Append(const TCHAR* line)
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

	// Runs one engine console command on the game thread. Only call with text built from validated
	// names and values (see Perf::IsValidCVarName / IsValidCVarValue).
	inline ExecResult Exec(const std::string& command)
	{
		ExecResult result;
		UEngine* engine = Globals::GEngine()();
		if (engine == nullptr)
			return result;

		const std::wstring wide = AsaApi::Tools::Utf8Decode(command);
		ExecOutput output;
		result.handled = engine->Exec(AsaApi::GetApiUtils().GetWorld(), wide.c_str(), reinterpret_cast<FOutputDevice*>(&output));
		result.output = AsaApi::Tools::Utf8Encode(output.text);
		return result;
	}

	// Asking the engine for a variable's value: running its bare name prints "name = "value"".
	inline ExecResult QueryCVar(const std::string& name)
	{
		return Exec(name);
	}

	inline ExecResult SetCVar(const std::string& name, const std::string& value)
	{
		return Exec(name + " " + value);
	}

	// --------------------------------------------------------------------------------------------
	// Garbage collection
	// --------------------------------------------------------------------------------------------

	// Hands the request to the engine, which collects at its next safe point in the frame.
	inline bool RequestGarbageCollection(bool full_purge)
	{
		UEngine* engine = Globals::GEngine()();
		if (engine == nullptr)
			return false;
		engine->ForceGarbageCollection(full_purge);
		return true;
	}

	// --------------------------------------------------------------------------------------------
	// Numbers for stats
	// --------------------------------------------------------------------------------------------

	inline unsigned long long FrameCounter()
	{
		return Globals::GFrameCounter()();
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
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		return world != nullptr ? world->PlayerControllerListField().Num() : -1;
	}

	// All live objects of a class and its subclasses, class defaults and objects waiting for GC excluded.
	inline TArray<UObject*> ObjectsOfClass(UClass* the_class)
	{
		TArray<UObject*> results;
		if (the_class != nullptr)
			Globals::GetObjectsOfClass(the_class, results, true, EObjectFlags::RF_ClassDefaultObject,
				EInternalObjectFlags::Garbage);
		return results;
	}

	inline UClass* StructureClass() { return APrimalStructure::StaticClass(); }
	inline UClass* DinoClass() { return APrimalDinoCharacter::StaticClass(); }

	inline long long CountOfClass(UClass* the_class)
	{
		return the_class != nullptr ? ObjectsOfClass(the_class).Num() : -1;
	}

	inline std::string ClassPath(UClass* the_class)
	{
		return AsaApi::IApiUtils::GetClassBlueprint(the_class).ToString();
	}

	inline UClass* ClassOf(UObject* object)
	{
		return object != nullptr ? object->ClassPrivateField() : nullptr;
	}

	inline AActor* DefaultActor(UClass* the_class)
	{
		return the_class != nullptr ? static_cast<AActor*>(the_class->GetDefaultObject(true)) : nullptr;
	}

	inline float NetUpdateFrequency(AActor* actor) { return actor->GetNetUpdateFrequency(); }
	inline float MinNetUpdateFrequency(AActor* actor) { return actor->GetMinNetUpdateFrequency(); }
	inline void SetNetUpdateFrequency(AActor* actor, float value) { actor->SetNetUpdateFrequency(value); }
	inline void SetMinNetUpdateFrequency(AActor* actor, float value) { actor->SetMinNetUpdateFrequency(value); }

	// --------------------------------------------------------------------------------------------
	// Network
	// --------------------------------------------------------------------------------------------

	inline bool HasNetwork() { return true; }

	// One sample per connected player. The engine updates these rates once per second.
	inline std::vector<Perf::ConnectionSample> ReadConnections()
	{
		std::vector<Perf::ConnectionSample> out;
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (world == nullptr)
			return out;
		for (TWeakObjectPtr<APlayerController> weak_controller : world->PlayerControllerListField())
		{
			APlayerController* controller = weak_controller.Get();
			UNetConnection* connection = controller != nullptr ? controller->NetConnectionField().Get() : nullptr;
			if (connection == nullptr)
				continue;
			Perf::ConnectionSample sample;
			sample.ping_seconds = connection->AvgLagField();
			sample.in_bytes = connection->InBytesPerSecondField();
			sample.out_bytes = connection->OutBytesPerSecondField();
			sample.in_packets = connection->InPacketsPerSecondField();
			sample.out_packets = connection->OutPacketsPerSecondField();
			sample.in_lost = connection->InPacketsLostField();
			sample.out_lost = connection->OutPacketsLostField();
			sample.queued_bits = connection->QueuedBitsField();
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
