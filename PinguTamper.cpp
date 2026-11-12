// Pingu - runtime tamper checks for the packaged game. See PinguTamper.h for what this is and what it can't stop.

#include "PinguTamper.h"
#include "Penguin.h"
#include "PinguSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#include "Misc/DelayedAutoRegister.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "IO/IoDispatcher.h"
#include "IPlatformFilePak.h"
#include <atomic>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <tlhelp32.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace PinguTamperPrivate
{
	// The live checks only run in a packaged Shipping game on Windows that is not an automated test run. Everywhere else
	// (editor, PIE, -game tests, Development packages) they are off, so solo play and the tests behave exactly as before.
	bool ChecksActive()
	{
#if !UE_BUILD_SHIPPING || !PLATFORM_WINDOWS
		return false;
#else
		static const bool bActive = FPlatformProperties::RequiresCookedData() && !UPinguSettings::IsAutomatedRun();
		return bActive;
#endif
	}

	std::atomic<bool> bForeign{ false };
	std::atomic<bool> bDebuggerSeen{ false };
	std::atomic<bool> bCodePatched{ false };
	std::atomic<bool> bStartupTamper{ false };
	std::atomic<bool> bInited{ false };
	std::atomic<double> LastCodeCheck{ -1000.0 };
	std::atomic<double> LastModuleScan{ -1000.0 };
	std::atomic<bool> bCodeCheckInFlight{ false };

	FCriticalSection DescLock;
	FString StartupDesc;
	FString Token;

	void AddDesc(const FString& Line)
	{
		FScopeLock Guard(&DescLock);
		StartupDesc += StartupDesc.IsEmpty() ? Line : (TEXT("; ") + Line);
	}

	// A signed container block or pak chunk whose hash does not match its signature: the pak was modified. The engine only
	// logs a warning and plays on; we count it as tamper. Active in every packaged build (any configuration): only a
	// changed container can trigger it, so it cannot hit an honest player. Called from the IO threads.
	std::atomic<bool> bPakSignature{ false };

	void NotePakSignatureFailure(const FString& What)
	{
		bool bExpected = false;
		if (bPakSignature.compare_exchange_strong(bExpected, true))
		{
			AddDesc(FString::Printf(TEXT("pak signature failure (%s)"), *What));
			UE_LOG(LogPingu, Warning, TEXT("[TAMPER] pak signature failure in %s: online play is blocked"), *What);
		}
	}

	void BindSignatureHooks()
	{
		if (!FPlatformProperties::RequiresCookedData())
		{
			return; // editor / -game: no signed paks
		}
		if (FIoDispatcher::IsInitialized())
		{
			FIoDispatcher::Get().OnSignatureError().AddLambda([](const FIoSignatureError& Error)
			{
				NotePakSignatureFailure(FString::Printf(TEXT("%s block %d"), *Error.ContainerName, Error.BlockIndex));
			});
		}
		FPakPlatformFile::FPakSigningFailureHandlerData& Handlers = FPakPlatformFile::GetPakSigningFailureHandlerData();
		FScopeLock Guard(&Handlers.GetLock());
		Handlers.GetPakChunkSignatureCheckFailedDelegate().AddLambda([](const FPakChunkSignatureCheckFailedData& Data)
		{
			NotePakSignatureFailure(FString::Printf(TEXT("%s chunk %d"), *FPaths::GetCleanFilename(Data.PakFilename), Data.ChunkIndex));
		});
		Handlers.GetPrincipalSignatureTableCheckFailedDelegate().AddLambda([](const FString& PakFilename)
		{
			NotePakSignatureFailure(FString::Printf(TEXT("%s signature table"), *FPaths::GetCleanFilename(PakFilename)));
		});
	}

	// Right after the IO dispatcher starts (FileSystemReady), before the startup packages and shaders are read.
	static FDelayedAutoRegisterHelper GSignatureHooks(EDelayedRegisterRunPhase::FileSystemReady, []() { BindSignatureHooks(); });

#if PLATFORM_WINDOWS
	// The game's root folder (the one with Engine\ and the project, where the launcher and the staging manifests live).
	FString GameRoot()
	{
		return PinguTamper::NormalizePath(FPaths::RootDir());
	}

	// Every file we shipped, as normalised full paths: read from the staging manifests at the build root. A proxy DLL an
	// attacker drops into the game folder is not in here, so it stands out. The exe is always allowed. bOutHaveManifest:
	// at least one manifest was read (without one, only known proxy names count as foreign: see IsForeignModule).
	TSet<FString> ShippedFiles(const FString& Root, bool& bOutHaveManifest)
	{
		bOutHaveManifest = false;
		TSet<FString> Out;
		Out.Add(PinguTamper::NormalizePath(FPlatformProcess::ExecutablePath()));
		const FString RootReal = FPaths::RootDir();
		for (const TCHAR* Manifest : { TEXT("Manifest_NonUFSFiles_Win64.txt"), TEXT("Manifest_UFSFiles_Win64.txt") })
		{
			FString Text;
			if (FFileHelper::LoadFileToString(Text, *(RootReal / Manifest)))
			{
				bOutHaveManifest = true;
				TArray<FString> Lines;
				Text.ParseIntoArrayLines(Lines);
				for (const FString& Line : Lines)
				{
					FString Rel = Line;
					int32 Tab;
					if (Line.FindChar(TEXT('\t'), Tab))
					{
						Rel = Line.Left(Tab);
					}
					Rel.TrimStartAndEndInline();
					if (!Rel.IsEmpty())
					{
						Out.Add(PinguTamper::NormalizePath(RootReal / Rel));
					}
				}
			}
		}
		return Out;
	}

	// Walks our own loaded modules. Returns the first foreign one found (empty when clean).
	FString FirstForeignModule(const FString& Root, const TSet<FString>& Shipped, bool bHaveManifest)
	{
		FString Found;
		HANDLE Snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, 0);
		if (Snap == INVALID_HANDLE_VALUE)
		{
			return Found; // can't look: say clean rather than risk a false positive
		}
		MODULEENTRY32W Entry;
		Entry.dwSize = sizeof(Entry);
		if (::Module32FirstW(Snap, &Entry))
		{
			do
			{
				const FString Path(Entry.szExePath);
				if (PinguTamper::IsForeignModule(Path, Root, Shipped, bHaveManifest))
				{
					Found = Path;
					break;
				}
			} while (::Module32NextW(Snap, &Entry));
		}
		::CloseHandle(Snap);
		return Found;
	}

	bool DebuggerNow()
	{
		if (::IsDebuggerPresent())
		{
			return true;
		}
		BOOL bRemote = 0;
		return ::CheckRemoteDebuggerPresent(::GetCurrentProcess(), &bRemote) && bRemote != 0;
	}

	// Our own exe's .text (code) section in memory: where it starts and how long, from the PE headers of the loaded image.
	bool TextSection(const uint8*& OutPtr, SIZE_T& OutSize)
	{
		HMODULE Base = ::GetModuleHandleW(nullptr);
		if (!Base)
		{
			return false;
		}
		const uint8* Image = reinterpret_cast<const uint8*>(Base);
		const IMAGE_DOS_HEADER* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Image);
		if (Dos->e_magic != IMAGE_DOS_SIGNATURE)
		{
			return false;
		}
		const IMAGE_NT_HEADERS* Nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(Image + Dos->e_lfanew);
		if (Nt->Signature != IMAGE_NT_SIGNATURE)
		{
			return false;
		}
		const IMAGE_SECTION_HEADER* Section = IMAGE_FIRST_SECTION(Nt);
		for (int32 i = 0; i < Nt->FileHeader.NumberOfSections; ++i, ++Section)
		{
			if (FCStringAnsi::Strncmp(reinterpret_cast<const char*>(Section->Name), ".text", 5) == 0)
			{
				OutPtr = Image + Section->VirtualAddress;
				OutSize = Section->Misc.VirtualSize;
				return OutSize > 0;
			}
		}
		return false;
	}

	uint32 TextCrc()
	{
		const uint8* Ptr = nullptr;
		SIZE_T Size = 0;
		if (!TextSection(Ptr, Size))
		{
			return 0;
		}
		return FCrc::MemCrc32(Ptr, static_cast<int32>(FMath::Min<SIZE_T>(Size, 0x7FFFFFFF)));
	}

	uint32 BaselineCrc = 0;
	constexpr double CodeInterval = 20.0;   // seconds between background code-section re-hashes
	constexpr double ModuleInterval = 8.0;  // seconds between foreign-DLL re-scans
#endif // PLATFORM_WINDOWS
}

FString PinguTamper::NormalizePath(const FString& Path)
{
	FString Full = FPaths::ConvertRelativePathToFull(Path);
	Full.ReplaceInline(TEXT("\\"), TEXT("/"));
	Full.ToLowerInline();
	while (Full.EndsWith(TEXT("/")))
	{
		Full.LeftChopInline(1);
	}
	return Full;
}

bool PinguTamper::IsKnownProxyName(const FString& FileName)
{
	// DLL names Windows looks up in the exe's folder first, which mod loaders and cheat injectors (UE4SS and friends)
	// impersonate. None of them is a file we ship (our dbghelp.dll sits under Engine\Binaries\ThirdParty, so it is not here).
	static const TCHAR* const Names[] = {
		TEXT("dwmapi.dll"), TEXT("version.dll"), TEXT("winmm.dll"), TEXT("dinput8.dll"), TEXT("dsound.dll"), TEXT("d3d9.dll"),
		TEXT("d3d10.dll"), TEXT("d3d11.dll"), TEXT("d3d12.dll"), TEXT("dxgi.dll"), TEXT("opengl32.dll"), TEXT("winhttp.dll"),
		TEXT("wininet.dll"), TEXT("xinput1_1.dll"), TEXT("xinput1_2.dll"), TEXT("xinput1_3.dll"), TEXT("xinput1_4.dll"),
		TEXT("xinput9_1_0.dll"), TEXT("ue4ss.dll"), TEXT("ue5ss.dll") };
	const FString Lower = FPaths::GetCleanFilename(FileName).ToLower();
	for (const TCHAR* Name : Names)
	{
		if (Lower == Name)
		{
			return true;
		}
	}
	return false;
}

bool PinguTamper::IsForeignModule(const FString& ModulePathFull, const FString& GameRootFull, const TSet<FString>& ShippedLower, bool bHaveManifest)
{
	const FString Module = NormalizePath(ModulePathFull);
	const FString Root = NormalizePath(GameRootFull);
	if (Module.IsEmpty() || Root.IsEmpty())
	{
		return false;
	}
	// Only modules inside the game folder can be foreign; anything elsewhere (Windows, drivers, Steam / Discord / NVIDIA
	// overlays in their own install dirs) is fine.
	if (Module != Root && !Module.StartsWith(Root + TEXT("/")))
	{
		return false;
	}
	// Inside the game folder: foreign if it is not one of the files we shipped. Without a manifest to say what we
	// shipped (a copy missing it), only the known proxy names count, so an honest player is never flagged.
	if (!bHaveManifest)
	{
		return IsKnownProxyName(Module);
	}
	return !ShippedLower.Contains(Module);
}

void PinguTamper::Init()
{
	using namespace PinguTamperPrivate;
	bool bExpected = false;
	if (!bInited.compare_exchange_strong(bExpected, true))
	{
		return; // already done
	}
	if (!ChecksActive())
	{
		FScopeLock Guard(&DescLock);
		Token.Reset();
		return;
	}
#if PLATFORM_WINDOWS
	const FString Root = GameRoot();
	bool bHaveManifest = false;
	const TSet<FString> Shipped = ShippedFiles(Root, bHaveManifest);
	const FString Foreign = FirstForeignModule(Root, Shipped, bHaveManifest);
	if (!Foreign.IsEmpty())
	{
		bForeign = true;
		AddDesc(FString::Printf(TEXT("foreign module %s"), *FPaths::GetCleanFilename(Foreign)));
	}
	if (DebuggerNow())
	{
		bDebuggerSeen = true;
		AddDesc(TEXT("debugger at startup"));
	}
	BaselineCrc = TextCrc();
	LastCodeCheck = FPlatformTime::Seconds();
	LastModuleScan = FPlatformTime::Seconds();
	bStartupTamper = bForeign.load() || bDebuggerSeen.load();
	{
		FScopeLock Guard(&DescLock);
		Token = bStartupTamper.load() ? FString(TEXT("t1")) : FString();
	}
	UE_LOG(LogPingu, Log, TEXT("[TAMPER] startup check: %s"), bStartupTamper.load() ? *DescribeStartup() : TEXT("clean"));
#endif
}

FString PinguTamper::StartupToken()
{
	Init();
	if (PinguTamperPrivate::bPakSignature.load())
	{
		return TEXT("t1"); // a modified pak was read before this answer
	}
	FScopeLock Guard(&PinguTamperPrivate::DescLock);
	return PinguTamperPrivate::Token;
}

bool PinguTamper::PakSignatureFailed()
{
	return PinguTamperPrivate::bPakSignature.load();
}

bool PinguTamper::Recheck()
{
	using namespace PinguTamperPrivate;
	Init();
	if (bPakSignature.load())
	{
		return true; // any packaged build (only a changed pak can set it)
	}
	if (!ChecksActive())
	{
		return false;
	}
#if PLATFORM_WINDOWS
	if (bForeign.load() || bDebuggerSeen.load() || bCodePatched.load())
	{
		return true;
	}
	const double Now = FPlatformTime::Seconds();
	if (DebuggerNow())
	{
		bDebuggerSeen = true;
		AddDesc(TEXT("debugger attached"));
		return true;
	}
	// A foreign DLL loaded since startup (throttled).
	if (Now - LastModuleScan.load() >= ModuleInterval)
	{
		LastModuleScan = Now;
		const FString Root = GameRoot();
		bool bHaveManifest = false;
		const TSet<FString> Shipped = ShippedFiles(Root, bHaveManifest);
		const FString Foreign = FirstForeignModule(Root, Shipped, bHaveManifest);
		if (!Foreign.IsEmpty())
		{
			bForeign = true;
			AddDesc(FString::Printf(TEXT("foreign module %s (later)"), *FPaths::GetCleanFilename(Foreign)));
			return true;
		}
	}
	// Our code section changed since the snapshot: hashed on a background thread so the game thread never hitches.
	if (Now - LastCodeCheck.load() >= CodeInterval)
	{
		bool bExpected = false;
		if (bCodeCheckInFlight.compare_exchange_strong(bExpected, true))
		{
			LastCodeCheck = Now;
			const uint32 Baseline = BaselineCrc;
			Async(EAsyncExecution::Thread, [Baseline]()
			{
				const uint32 Current = TextCrc();
				if (Current != 0 && Current != Baseline)
				{
					bCodePatched = true;
					AddDesc(TEXT("code section changed"));
				}
				bCodeCheckInFlight = false;
			});
		}
	}
	return bForeign.load() || bDebuggerSeen.load() || bCodePatched.load();
#else
	return false;
#endif
}

FString PinguTamper::DescribeStartup()
{
	FScopeLock Guard(&PinguTamperPrivate::DescLock);
	return PinguTamperPrivate::StartupDesc;
}
