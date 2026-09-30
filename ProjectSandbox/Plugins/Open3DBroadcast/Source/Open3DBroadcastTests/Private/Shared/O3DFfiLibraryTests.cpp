// Copyright Lifelike & Believable. All Rights Reserved.

// WP-F3 (TRF-14, TRF-28): FO3DFfiLibrary, the shared FFI DLL loader the MoQ and WebRTC modules
// use. The platform calls are faked (FO3DFfiLibraryOps), so these tests need no DLL and check
// the decisions: where the library is looked for, that nothing is loaded when the file is
// missing, and that module shutdown stops live instances and keeps the library loaded while an
// instance is still referenced.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#include "O3DFfiLibrary.h"
#include "O3DSenderRegistry.h"
#include "O3DTestFakes.h"

namespace O3DFfiLibraryTest
{
	/** What the fake platform calls saw. Declared before the library so it outlives it. */
	struct FFakePlatform
	{
		FString PluginBaseDir = TEXT("/FakePlugins/Owner");
		bool bFileExists = true;
		bool bLoadSucceeds = true;
		int32 DummyTarget = 0;

		FString RequestedPlugin;
		FString CheckedPath;
		int32 LoadCalls = 0;
		int32 FreeCalls = 0;
		void* FreedHandle = nullptr;

		void* FakeHandle() { return &DummyTarget; }

		FO3DFfiLibraryOps MakeOps()
		{
			FO3DFfiLibraryOps Ops;
			Ops.FindPluginBaseDir = [this](const FString& PluginName) -> FString
			{
				RequestedPlugin = PluginName;
				return PluginBaseDir;
			};
			Ops.FileExists = [this](const FString& Path)
			{
				CheckedPath = Path;
				return bFileExists;
			};
			Ops.LoadDll = [this](const FString& Path) -> void*
			{
				(void)Path;
				++LoadCalls;
				return bLoadSucceeds ? FakeHandle() : nullptr;
			};
			Ops.FreeDll = [this](void* Handle)
			{
				++FreeCalls;
				FreedHandle = Handle;
			};
			Ops.GetExport = [](void* Handle, const TCHAR* SymbolName) -> void*
			{
				(void)Handle;
				(void)SymbolName;
				return nullptr;
			};
			return Ops;
		}
	};

	FO3DFfiLibraryDesc MakeDesc()
	{
		FO3DFfiLibraryDesc Desc;
		Desc.DisplayName = TEXT("Test FFI");
		Desc.OwningPluginName = TEXT("OwnerPlugin");
		Desc.RelativePath = TEXT("Source/Mod/ThirdParty/lib/bin/Win64/test_ffi.dll");
		return Desc;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DFfiLibraryPathTest, "Open3DBroadcast.Shared.FfiLibrary.ResolvesPathFromOwningPlugin", O3DB_TEST_FLAGS)
bool FO3DFfiLibraryPathTest::RunTest(const FString& Parameters)
{
	using namespace O3DFfiLibraryTest;

	FFakePlatform Platform;
	FO3DFfiLibrary Library(MakeDesc(), Platform.MakeOps());

	TestTrue(TEXT("Load succeeds"), Library.Load());
	TestTrue(TEXT("Library reports loaded"), Library.IsLoaded());
	TestEqual(TEXT("Looks up the owning plugin named in the descriptor"), Platform.RequestedPlugin, FString(TEXT("OwnerPlugin")));
	const FString Expected = FPaths::Combine(Platform.PluginBaseDir, MakeDesc().RelativePath);
	TestEqual(TEXT("Path is the plugin base dir plus the relative path"), Library.GetLibraryPath(), Expected);
	TestEqual(TEXT("The resolved path is the one checked"), Platform.CheckedPath, Expected);
	TestTrue(TEXT("Second Load is a no-op"), Library.Load());
	TestEqual(TEXT("The DLL is loaded once"), Platform.LoadCalls, 1);

	TestTrue(TEXT("Unload with no instances frees the library"), Library.Unload() == EO3DFfiUnloadResult::Unloaded);
	TestEqual(TEXT("FreeDll called once"), Platform.FreeCalls, 1);
	TestTrue(TEXT("FreeDll got the loaded handle"), Platform.FreedHandle == Platform.FakeHandle());
	TestTrue(TEXT("Second Unload does nothing"), Library.Unload() == EO3DFfiUnloadResult::NotLoaded);
	TestEqual(TEXT("FreeDll still called once"), Platform.FreeCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DFfiLibraryMissingTest, "Open3DBroadcast.Shared.FfiLibrary.MissingLibraryFailsCleanly", O3DB_TEST_FLAGS)
bool FO3DFfiLibraryMissingTest::RunTest(const FString& Parameters)
{
	using namespace O3DFfiLibraryTest;

	// The load failures are logged at Error by design; they are the behaviour under test.
	AddExpectedError(TEXT("Test FFI"), EAutomationExpectedMessageFlags::Contains, 3);

	{
		FFakePlatform Platform;
		Platform.bFileExists = false;
		FO3DFfiLibrary Library(MakeDesc(), Platform.MakeOps());
		TestFalse(TEXT("Load fails when the file is missing"), Library.Load());
		TestFalse(TEXT("Not loaded"), Library.IsLoaded());
		TestEqual(TEXT("No load attempted for a missing file"), Platform.LoadCalls, 0);
		TestTrue(TEXT("Unload of a library that never loaded does nothing"), Library.Unload() == EO3DFfiUnloadResult::NotLoaded);
		TestEqual(TEXT("FreeDll never called"), Platform.FreeCalls, 0);
	}
	{
		FFakePlatform Platform;
		Platform.PluginBaseDir.Reset();
		FO3DFfiLibrary Library(MakeDesc(), Platform.MakeOps());
		TestFalse(TEXT("Load fails when the owning plugin is not found"), Library.Load());
		TestEqual(TEXT("No load attempted without a plugin"), Platform.LoadCalls, 0);
	}
	{
		FFakePlatform Platform;
		Platform.bLoadSucceeds = false;
		FO3DFfiLibrary Library(MakeDesc(), Platform.MakeOps());
		TestFalse(TEXT("Load fails when the loader returns null"), Library.Load());
		TestFalse(TEXT("Not loaded"), Library.IsLoaded());
		TestTrue(TEXT("GetExport is null when not loaded"), Library.GetExport(TEXT("anything")) == nullptr);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DFfiLibraryUnloadWithLiveInstanceTest, "Open3DBroadcast.Shared.FfiLibrary.UnloadWithLiveInstance", O3DB_TEST_FLAGS)
bool FO3DFfiLibraryUnloadWithLiveInstanceTest::RunTest(const FString& Parameters)
{
	using namespace O3DFfiLibraryTest;

	// The KeptLoaded case logs a Warning by design.
	AddExpectedError(TEXT("still referenced"), EAutomationExpectedMessageFlags::Contains, 1);

	FFakePlatform Platform;
	const TSharedRef<FO3DFfiLibrary, ESPMode::ThreadSafe> Library =
		MakeShared<FO3DFfiLibrary, ESPMode::ThreadSafe>(MakeDesc(), Platform.MakeOps());
	if (!TestTrue(TEXT("Load succeeds"), Library->Load()))
	{
		return false;
	}

	// Register a factory the way the MoQ and WebRTC modules do, under a name unique to this test.
	const FName TransportName(*FString::Printf(TEXT("%s_FfiLibrary_%s"), FO3DFakeTransportScope::GetNamePrefix(), *FGuid::NewGuid().ToString()));
	O3DTransport::RegisterSender(TransportName, [Library]() -> TSharedPtr<IOpen3DSender>
	{
		return Library->TrackInstance(MakeShared<FO3DFakeSender, ESPMode::ThreadSafe>());
	});

	// One instance a component still holds, and one that was already released.
	TSharedPtr<IOpen3DSender> Live = O3DTransport::CreateSender(TransportName);
	{
		const TSharedPtr<IOpen3DSender> Released = O3DTransport::CreateSender(TransportName);
		TestTrue(TEXT("Second instance created"), Released.IsValid());
	}
	if (!TestTrue(TEXT("Instance created through the registry"), Live.IsValid()))
	{
		O3DTransport::UnregisterSender(TransportName);
		return false;
	}
	FO3DFakeSender* LiveFake = static_cast<FO3DFakeSender*>(Live.Get());
	TestTrue(TEXT("Live instance starts"), Live->Initialize(FO3DTransportConfig()) && Live->Start());
	TestEqual(TEXT("Only the referenced instance counts as live"), Library->GetNumLiveInstances(), 1);

	// Module shutdown sequence (ShutdownModule in both FFI transport modules).
	O3DTransport::UnregisterSender(TransportName);
	TestFalse(TEXT("No new instances after unregister"), O3DTransport::CreateSender(TransportName).IsValid());
	TestEqual(TEXT("StopLiveInstances stops the one live instance"), Library->StopLiveInstances(), 1);
	TestEqual(TEXT("The live instance saw Stop()"), LiveFake->GetStopCalls(), 1);
	TestTrue(TEXT("Unload refuses while the instance is referenced"), Library->Unload() == EO3DFfiUnloadResult::KeptLoaded);
	TestEqual(TEXT("FreeDll not called while referenced"), Platform.FreeCalls, 0);
	TestTrue(TEXT("Library stays loaded"), Library->IsLoaded());

	// The owner can still use and stop its instance: the library it runs on is still mapped.
	Live->Stop();
	TestEqual(TEXT("Owner's own Stop() still runs"), LiveFake->GetStopCalls(), 2);

	// Once the last reference is gone, the library can be freed.
	Live.Reset();
	TestEqual(TEXT("No live instances left"), Library->GetNumLiveInstances(), 0);
	TestEqual(TEXT("StopLiveInstances has nothing to stop"), Library->StopLiveInstances(), 0);
	TestTrue(TEXT("Unload frees the library"), Library->Unload() == EO3DFfiUnloadResult::Unloaded);
	TestEqual(TEXT("FreeDll called once"), Platform.FreeCalls, 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
