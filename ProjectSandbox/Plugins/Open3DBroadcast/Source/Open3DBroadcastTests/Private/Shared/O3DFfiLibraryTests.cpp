// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors
//
// WP-F3 (TRF-14, TRF-28): FO3DFfiLibrary, the shared FFI DLL loader the MoQ and WebRTC modules
// use. The platform calls are faked (FO3DFfiLibraryOps), so these tests need no DLL and check
// the decisions: where the library is looked for, that nothing is loaded when the file is
// missing, and that module shutdown (unregister, which drains, then Unload) keeps the library
// loaded while the transport registry still counts a referenced instance (WP-A1 PR 2, ADR 0007
// item 5).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#include "O3DFfiLibrary.h"
#include "O3DTestFakes.h"
#include "Transport/O3DTransportRegistry.h"

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

	// By design: the registry reports the instance a component still holds after the unregister
	// (Error), and Unload refuses while it is referenced (Warning).
	AddExpectedError(TEXT("unregistered with 1 live instance"), EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedError(TEXT("library left loaded until process exit"), EAutomationExpectedMessageFlags::Contains, 1);

	// A private registry stands in for the process-wide one (WP-A1 PR 2: the registry tracks the
	// instances, the library asks it).
	const TSharedRef<FO3DTransportRegistry, ESPMode::ThreadSafe> Registry = MakeShared<FO3DTransportRegistry, ESPMode::ThreadSafe>();
	const FName TransportName(TEXT("FfiLibraryTestTransport"));

	FFakePlatform Platform;
	FO3DFfiLibraryOps Ops = Platform.MakeOps();
	Ops.CountLiveInstances = [Registry](FName Name) { return Registry->GetNumLiveInstances(Name); };
	FO3DFfiLibraryDesc Desc = MakeDesc();
	Desc.TransportNames.Add(TransportName);
	FO3DFfiLibrary Library(Desc, MoveTemp(Ops));
	if (!TestTrue(TEXT("Load succeeds"), Library.Load()))
	{
		return false;
	}

	// Registered the way the MoQ and WebRTC modules do: only after the library loaded.
	FO3DTransportDescriptor Descriptor;
	Descriptor.Name = TransportName;
	Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
	Descriptor.CreateSender = []() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeSender, ESPMode::ThreadSafe>(); };
	FO3DTransportRegistration Registration = Registry->Register(MoveTemp(Descriptor));

	// One instance an owner that did not subscribe still holds, and one that was already released.
	TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Live = Registry->CreateSender(TransportName);
	{
		const TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Released = Registry->CreateSender(TransportName);
		TestTrue(TEXT("Second instance created"), Released.IsValid());
	}
	if (!TestTrue(TEXT("Instance created through the registry"), Live.IsValid()))
	{
		return false;
	}
	FO3DFakeSender* LiveFake = static_cast<FO3DFakeSender*>(Live.Get());
	TestTrue(TEXT("Live instance starts"), Live->Initialize(FO3DTransportConfig()) && Live->Start());
	TestEqual(TEXT("Only the referenced instance counts as live"), Library.GetNumLiveInstances(), 1);

	// Module shutdown sequence (ShutdownModule in both FFI transport modules): unregister, which
	// drains, then free the FFI handle.
	Registration.Reset();
	TestFalse(TEXT("No new instances after unregister"), Registry->IsRegistered(TransportName, EO3DTransportRole::Sender));
	TestEqual(TEXT("The drain stopped the live instance"), LiveFake->GetStopCalls(), 1);
	TestTrue(TEXT("Unload refuses while the instance is referenced"), Library.Unload() == EO3DFfiUnloadResult::KeptLoaded);
	TestEqual(TEXT("FreeDll not called while referenced"), Platform.FreeCalls, 0);
	TestTrue(TEXT("Library stays loaded"), Library.IsLoaded());

	// The owner can still use and stop its instance: the library it runs on is still mapped.
	Live->Stop();
	TestEqual(TEXT("Owner's own Stop() still runs"), LiveFake->GetStopCalls(), 2);

	// Once the last reference is gone, the library can be freed.
	Live.Reset();
	TestEqual(TEXT("No live instances left"), Library.GetNumLiveInstances(), 0);
	TestTrue(TEXT("Unload frees the library"), Library.Unload() == EO3DFfiUnloadResult::Unloaded);
	TestEqual(TEXT("FreeDll called once"), Platform.FreeCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DFfiLibraryUnloadAfterDrainTest, "Open3DBroadcast.Shared.FfiLibrary.UnloadAfterCleanDrain", O3DB_TEST_FLAGS)
bool FO3DFfiLibraryUnloadAfterDrainTest::RunTest(const FString& Parameters)
{
	using namespace O3DFfiLibraryTest;

	// No AddExpectedError: the owner releases its instance on OnTransportUnregistering, so the
	// drain reports no leak and the library is freed at once.
	const TSharedRef<FO3DTransportRegistry, ESPMode::ThreadSafe> Registry = MakeShared<FO3DTransportRegistry, ESPMode::ThreadSafe>();
	const FName TransportName(TEXT("FfiLibraryTestDrained"));

	FFakePlatform Platform;
	FO3DFfiLibraryOps Ops = Platform.MakeOps();
	Ops.CountLiveInstances = [Registry](FName Name) { return Registry->GetNumLiveInstances(Name); };
	FO3DFfiLibraryDesc Desc = MakeDesc();
	Desc.TransportNames.Add(TransportName);
	FO3DFfiLibrary Library(Desc, MoveTemp(Ops));
	if (!TestTrue(TEXT("Load succeeds"), Library.Load()))
	{
		return false;
	}

	FO3DTransportDescriptor Descriptor;
	Descriptor.Name = TransportName;
	Descriptor.CreateReceiver = []() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeReceiver, ESPMode::ThreadSafe>(); };
	FO3DTransportRegistration Registration = Registry->Register(MoveTemp(Descriptor));

	TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> Owned = Registry->CreateReceiver(TransportName);
	TestTrue(TEXT("Instance created"), Owned.IsValid());
	const FDelegateHandle Handle = Registry->OnTransportUnregistering().AddLambda([&Owned, TransportName](FName Name)
	{
		if (Name == TransportName && Owned.IsValid())
		{
			Owned->Stop();
			Owned.Reset();
		}
	});
	TestEqual(TEXT("One live instance"), Library.GetNumLiveInstances(), 1);

	Registration.Reset();
	Registry->OnTransportUnregistering().Remove(Handle);

	TestFalse(TEXT("The owner released its instance"), Owned.IsValid());
	TestEqual(TEXT("Nothing live"), Library.GetNumLiveInstances(), 0);
	TestTrue(TEXT("Unload frees the library"), Library.Unload() == EO3DFfiUnloadResult::Unloaded);
	TestEqual(TEXT("FreeDll called once"), Platform.FreeCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DFfiLibraryNoTransportNamesTest, "Open3DBroadcast.Shared.FfiLibrary.UnloadWithoutTransportNamesCountsNothing", O3DB_TEST_FLAGS)
bool FO3DFfiLibraryNoTransportNamesTest::RunTest(const FString& Parameters)
{
	using namespace O3DFfiLibraryTest;

	// A library whose descriptor names no transport (or whose ops cannot count) has nothing to
	// wait for; it is freed as before.
	FFakePlatform Platform;
	FO3DFfiLibrary Library(MakeDesc(), Platform.MakeOps());
	TestTrue(TEXT("Load succeeds"), Library.Load());
	TestEqual(TEXT("No transport names: nothing live"), Library.GetNumLiveInstances(), 0);
	TestTrue(TEXT("Unload frees the library"), Library.Unload() == EO3DFfiUnloadResult::Unloaded);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
