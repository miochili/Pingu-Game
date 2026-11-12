// Pingu - LAN multiplayer sessions: host, find games (UDP broadcast beacon), join, leave, and the way back to the solo
// menu when something fails.
//
// The world's net mode is the truth: every time the game instance gets a new world the state follows it (a listen server
// is Hosting, a client world is Connected, a solo world is Offline). The calls below only start the travel.

#include "PinguNetSubsystem.h"
#include "Penguin.h"
#include "PinguAntiCheat.h"
#include "PinguNetTest.h"
#include "PinguSettings.h"
#include "Common/UdpSocketBuilder.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UObject/Package.h"

namespace PinguNetSubsystemPrivate
{
	/** Beacon packet: magic, version, host id (u32), game port (u16), players, max players, flags, name length, name (UTF-16
	 *  code units). Little endian. About 30-60 bytes. */
	constexpr int32 MagicSize = 6;
	static const uint8 BeaconMagic[MagicSize] = { 'P', 'I', 'N', 'G', 'U', '2' };
	constexpr uint8 BeaconVersion = 1;
	constexpr uint8 FlagInMatch = 1;
	constexpr uint8 FlagClosing = 2;  // the host stops: drop it from the list at once
	constexpr int32 HeaderSize = MagicSize + 1 + 4 + 2 + 1 + 1 + 1 + 1;

	constexpr double BeaconInterval = 1.0;
	constexpr double GameTimeout = 4.0;      // a game not heard from for this long drops off the list
	constexpr double TargetsRefresh = 10.0;  // network adapters come and go (Wi-Fi reconnects)
	constexpr double SocketRetry = 3.0;      // the beacon port is busy / no network: try again after this
	constexpr double ConnectTimeout = 15.0;  // the engine would wait 60 s for a host that is not there
	constexpr double HostTimeout = 10.0;     // the listen map must be up by then
	constexpr double AddressCacheTime = 5.0;
	constexpr int32 MaxNameLength = 20;      // AGameModeBase::InitNewPlayer cuts player names to 20 too
	constexpr int32 MaxPacketsPerTick = 64;
	constexpr int32 MaxPacketSize = 512;

	// Which message wins when one failure produces several: the host's own refusal text beats our guesses.
	constexpr int32 ErrorGeneric = 1;
	constexpr int32 ErrorSpecific = 2;

	ISocketSubsystem* GetSockets()
	{
		return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}

	/** FUdpSocketBuilder needs the Networking module started (FIPv4Endpoint asserts otherwise) and a socket subsystem. */
	bool IsLanReady()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("Networking")) && GetSockets() != nullptr;
	}

	void DestroySocket(FSocket*& Socket)
	{
		if (!Socket)
		{
			return;
		}
		Socket->Close();
		if (ISocketSubsystem* Sockets = GetSockets())
		{
			Sockets->DestroySocket(Socket);
		}
		Socket = nullptr;
	}

	/** Host byte order IPv4 -> "a.b.c.d". */
	FString IpToString(uint32 Ip)
	{
		return FString::Printf(TEXT("%u.%u.%u.%u"), (Ip >> 24) & 0xFFu, (Ip >> 16) & 0xFFu, (Ip >> 8) & 0xFFu, Ip & 0xFFu);
	}

	/** The IPv4 of an address in host byte order (also from an IPv4-mapped IPv6 address "::ffff:a.b.c.d"). */
	bool GetIPv4(const FInternetAddr& Addr, uint32& OutIp)
	{
		const TArray<uint8> Raw = Addr.GetRawIp();
		int32 First = INDEX_NONE;
		if (Raw.Num() == 4)
		{
			First = 0;
		}
		else if (Raw.Num() == 16 && Raw[10] == 0xFF && Raw[11] == 0xFF)
		{
			First = 12;
			for (int32 Index = 0; Index < 10; ++Index)
			{
				if (Raw[Index] != 0)
				{
					First = INDEX_NONE;
				}
			}
		}
		if (First == INDEX_NONE)
		{
			return false;
		}
		OutIp = (uint32(Raw[First]) << 24) | (uint32(Raw[First + 1]) << 16) | (uint32(Raw[First + 2]) << 8) | uint32(Raw[First + 3]);
		return OutIp != 0;
	}

	/** IPv4 addresses of this PC's network adapters (host byte order). */
	TArray<uint32> GetAdapterAddresses()
	{
		TArray<uint32> Result;
		TArray<TSharedPtr<FInternetAddr>> Addresses;
		ISocketSubsystem* Sockets = GetSockets();
		if (Sockets && Sockets->GetLocalAdapterAddresses(Addresses))
		{
			for (const TSharedPtr<FInternetAddr>& Address : Addresses)
			{
				uint32 Ip = 0;
				if (Address.IsValid() && GetIPv4(*Address, Ip))
				{
					Result.AddUnique(Ip);
				}
			}
		}
		return Result;
	}

	/** The address of this PC the system sends from when it talks to another PC: the adapter the LAN / Wi-Fi is on, not one of the
	 *  virtual adapters a PC can also have (VMware, Hyper-V, VirtualBox, Docker: their own little 192.168.x.1 networks). A UDP
	 *  "connection" to a far address sends nothing, it only makes the system pick its route. 0 when there is no route. */
	uint32 GetRouteAddress()
	{
		ISocketSubsystem* Sockets = GetSockets();
		FSocket* Probe = Sockets ? Sockets->CreateSocket(NAME_DGram, TEXT("PinguRouteProbe"), FNetworkProtocolTypes::IPv4) : nullptr;
		if (!Probe)
		{
			return 0;
		}
		uint32 Result = 0;
		TSharedRef<FInternetAddr> Far = Sockets->CreateInternetAddr();
		Far->SetIp(0x08080808u);  // the IP first: it sets the address family the port goes into
		Far->SetPort(53);
		if (Probe->Connect(*Far))
		{
			TSharedRef<FInternetAddr> Local = Sockets->CreateInternetAddr();
			Probe->GetAddress(*Local);
			if (!GetIPv4(*Local, Result))
			{
				Result = 0;
			}
		}
		DestroySocket(Probe);
		return Result;
	}

	/** How good an address is to tell the other players: home networks first, then any IPv4; loopback never. */
	int32 ScoreAddress(uint32 Ip)
	{
		const uint32 A = Ip >> 24;
		const uint32 B = (Ip >> 16) & 0xFFu;
		const uint32 C = (Ip >> 8) & 0xFFu;
		if (A == 0 || A == 127 || A >= 224)
		{
			return -1;  // "this network", loopback, multicast / reserved
		}
		if (A == 192 && B == 168)
		{
			return C == 56 ? 3 : 5;  // 192.168.56.x is VirtualBox's host-only network
		}
		if (A == 10)
		{
			return 4;
		}
		if (A == 172 && B >= 16 && B <= 31)
		{
			return 2;  // also Hyper-V / WSL / Docker virtual adapters
		}
		if (A == 169 && B == 254)
		{
			return 0;  // link-local: no DHCP answer
		}
		return 1;
	}

	/** Player names travel in the join URL (?Name=...): no URL delimiters or control characters, at most 20 long. */
	FString CleanPlayerName(const FString& Name)
	{
		FString Clean;
		for (const TCHAR Ch : Name)
		{
			const bool bDelimiter = Ch == TEXT('?') || Ch == TEXT('#') || Ch == TEXT('=') || Ch == TEXT(':') || Ch == TEXT('"')
				|| Ch == TEXT('/') || Ch == TEXT('\\') || Ch == TEXT('|');
			if (Ch >= TEXT(' ') && Ch != 127 && !bDelimiter)
			{
				Clean.AppendChar(Ch);
			}
		}
		Clean.TrimStartAndEndInline();
		Clean = Clean.Left(MaxNameLength);
		Clean.TrimEndInline();
		return Clean.IsEmpty() ? FString(TEXT("Pingu")) : Clean;
	}

	FString GetPlayerName()
	{
		const UPinguSettings* Settings = UPinguSettings::Get();
		return CleanPlayerName(Settings ? Settings->GetPlayerName() : FString());
	}

	/** "/Game/Maps/TestIsland" of a world, PIE prefix removed: what OpenLevel needs in PIE and in -game. */
	FString GetMapPackageName(const UWorld* World)
	{
		const UPackage* Package = World ? World->GetPackage() : nullptr;
		FString Map = Package ? UWorld::RemovePIEPrefix(Package->GetName()) : FString();
		if (Map.IsEmpty() && World)
		{
			Map = UGameplayStatics::GetCurrentLevelName(World, true);
		}
		return Map;
	}

	/** A refusal the host's game mode wrote ("The game is full.") is shown as it is; engine details are not. */
	bool IsHostMessage(const FString& Message)
	{
		if (Message.IsEmpty() || Message.Len() > 100)
		{
			return false;
		}
		static const TCHAR* const Technical[] = { TEXT("::"), TEXT("["), TEXT("Driver"), TEXT("Socket"), TEXT("onnection"),
			TEXT("Elapsed"), TEXT("0x"), TEXT("Pending"), TEXT("UNet"), TEXT("imed out") };
		for (const TCHAR* Word : Technical)
		{
			if (Message.Contains(Word, ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	/** "192.168.1.20", "192.168.1.20:7777" or a host name. */
	bool ParseAddress(const FString& Address, FString& OutHost, int32& OutPort)
	{
		const FString Text = Address.TrimStartAndEnd();
		FString Host = Text;
		int32 Colon = INDEX_NONE;
		if (Text.FindLastChar(TEXT(':'), Colon))
		{
			Host = Text.Left(Colon);
			const FString PortText = Text.Mid(Colon + 1);
			if (PortText.IsEmpty() || PortText.Len() > 5 || Host.Contains(TEXT(":")))
			{
				return false;  // no port after the colon, or an IPv6 address
			}
			for (const TCHAR Ch : PortText)
			{
				if (!FChar::IsDigit(Ch))
				{
					return false;
				}
			}
			OutPort = FCString::Atoi(*PortText);
			if (OutPort <= 0 || OutPort > 65535)
			{
				return false;
			}
		}
		if (Host.IsEmpty() || Host.Len() > 253)
		{
			return false;
		}
		for (const TCHAR Ch : Host)
		{
			if (!FChar::IsAlnum(Ch) && Ch != TEXT('.') && Ch != TEXT('-'))
			{
				return false;
			}
		}
		OutHost = Host;
		return true;
	}

	const TCHAR* NetModeName(ENetMode Mode)
	{
		switch (Mode)
		{
		case NM_Standalone: return TEXT("standalone");
		case NM_DedicatedServer: return TEXT("dedicated server");
		case NM_ListenServer: return TEXT("listen server");
		case NM_Client: return TEXT("client");
		default: return TEXT("?");
		}
	}

	const TCHAR* StateName(EPinguNetState State)
	{
		switch (State)
		{
		case EPinguNetState::Offline: return TEXT("Offline");
		case EPinguNetState::Hosting: return TEXT("Hosting");
		case EPinguNetState::Connecting: return TEXT("Connecting");
		case EPinguNetState::Connected: return TEXT("Connected");
		default: return TEXT("?");
		}
	}

	void PutU16(TArray<uint8>& Out, uint32 Value)
	{
		Out.Add(uint8(Value & 0xFFu));
		Out.Add(uint8((Value >> 8) & 0xFFu));
	}

	void PutU32(TArray<uint8>& Out, uint32 Value)
	{
		PutU16(Out, Value & 0xFFFFu);
		PutU16(Out, Value >> 16);
	}

	/** Reads a packet without ever running past its end (bOk goes false instead). */
	struct FPacketReader
	{
		const uint8* Data = nullptr;
		int32 Size = 0;
		int32 Pos = 0;
		bool bOk = true;

		uint32 U8()
		{
			if (Pos + 1 > Size)
			{
				bOk = false;
				return 0;
			}
			return Data[Pos++];
		}
		uint32 U16()
		{
			const uint32 Low = U8();
			return Low | (U8() << 8);
		}
		uint32 U32()
		{
			const uint32 Low = U16();
			return Low | (U16() << 16);
		}
	};
}

// ---------------------------------------------------------------------------- lifetime

UPinguNetSubsystem* UPinguNetSubsystem::Get(const UObject* WorldContext)
{
	if (!WorldContext)
	{
		return nullptr;
	}
	const UGameInstance* GameInstance = Cast<UGameInstance>(WorldContext);
	if (!GameInstance)
	{
		const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
		GameInstance = World ? World->GetGameInstance() : nullptr;
	}
	return GameInstance ? GameInstance->GetSubsystem<UPinguNetSubsystem>() : nullptr;
}

void UPinguNetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The socket builder's FIPv4Endpoint needs the Networking module started (a Build.cs dependency alone does not start it).
	FModuleManager::Get().LoadModule(TEXT("Networking"));

	const FGuid Guid = FGuid::NewGuid();
	InstanceId = Guid.A ^ Guid.B ^ Guid.C ^ Guid.D;
	if (InstanceId == 0)
	{
		InstanceId = 1;
	}
	bNetTest = APinguNetTest::IsRequested();

	if (GEngine)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &UPinguNetSubsystem::HandleNetworkFailure);
		TravelFailureHandle = GEngine->OnTravelFailure().AddUObject(this, &UPinguNetSubsystem::HandleTravelFailure);
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UPinguNetSubsystem::TickNet));
}

void UPinguNetSubsystem::Deinitialize()
{
	// Ticker and engine events first: nothing of ours may run on a half torn down subsystem.
	FTSTicker::RemoveTicker(TickerHandle);
	TickerHandle.Reset();
	if (GEngine)
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		GEngine->OnTravelFailure().Remove(TravelFailureHandle);
	}
	NetworkFailureHandle.Reset();
	TravelFailureHandle.Reset();

	StopBeacon();
	StopSearch();
	State = EPinguNetState::Offline;
	bReturnPending = false;
	bHostTravelPending = false;
	CurrentWorld.Reset();
	Super::Deinitialize();
}

bool UPinguNetSubsystem::TickNet(float DeltaTime)
{
	using namespace PinguNetSubsystemPrivate;
	if (IsEngineExitRequested())
	{
		return true;
	}
	const double Now = FPlatformTime::Seconds();
	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;

	// A new world (hosting / joining / leaving reload the map; PIE starts with one).
	if (World != CurrentWorld.Get())
	{
		CurrentWorld = World;
		bWorldBegunPlay = false;
		if (World)
		{
			OnWorldChanged(World);
		}
	}
	// A client's world begins play once the host's game state arrived.
	if (World && !bWorldBegunPlay && World->HasBegunPlay())
	{
		bWorldBegunPlay = true;
		OnWorldBegunPlay(World);
	}

	if (bReturnPending && World && !World->bIsTearingDown)
	{
		bReturnPending = false;
		ReturnToSoloMenu(true);
	}

	// Watchdogs (frames as well as seconds: a long blocking map load must not count as waiting).
	if (State == EPinguNetState::Connecting && Now - JoinStartTime > ConnectTimeout && GFrameCounter - JoinStartFrame > 30)
	{
		UE_LOG(LogPingu, Warning, TEXT("[NET] no answer from %s:%d after %.0f s"), *JoinHost, JoinPort, ConnectTimeout);
		SetError(MakeCouldNotConnectText(), ErrorGeneric);
		ReturnToSoloMenu(false);  // still in the solo world: drop the join, the menu stays
	}
	if (State == EPinguNetState::Hosting && bHostTravelPending && Now - HostStartTime > HostTimeout && GFrameCounter - HostStartFrame > 30)
	{
		UE_LOG(LogPingu, Warning, TEXT("[NET] the arena did not come up as a listen server"));
		SetError(FText::FromString(TEXT("Could not host the game.")), ErrorGeneric);
		ReturnToSoloMenu(true);
	}

	// The engine replaces ?Name= with the online subsystem's nickname when there is one: set our name once in the game.
	if (!bNameSent && World && (State == EPinguNetState::Connected || (State == EPinguNetState::Hosting && !bHostTravelPending)))
	{
		APlayerController* PlayerController = GameInstance->GetFirstLocalPlayerController(World);
		const APlayerState* PlayerState = PlayerController ? PlayerController->GetPlayerState<APlayerState>() : nullptr;
		if (PlayerState)
		{
			const FString Name = GetPlayerName();
			if (PlayerState->GetPlayerName() != Name)
			{
				UE_LOG(LogPingu, Log, TEXT("[NET] player name \"%s\" -> \"%s\""), *PlayerState->GetPlayerName(), *Name);
				PlayerController->SetName(Name);
			}
			bNameSent = true;
		}
	}

	TickBeacon(Now);
	TickSearch(Now);
	return true;
}

void UPinguNetSubsystem::OnWorldChanged(UWorld* World)
{
	using namespace PinguNetSubsystemPrivate;
	const ENetMode Mode = World->GetNetMode();
	UE_LOG(LogPingu, Log, TEXT("[NET] world %s: %s (was %s)"), *World->GetMapName(), NetModeName(Mode), StateName(State));
	bNameSent = false;

	if (Mode == NM_ListenServer)
	{
		if (State != EPinguNetState::Hosting)
		{
			UE_LOG(LogPingu, Log, TEXT("[NET] this world is a listen server (not from HostGame, e.g. the editor's net mode): hosting"));
		}
		State = EPinguNetState::Hosting;
		bHostTravelPending = false;
		// The listen port: the one the net driver really bound (7777 from HostGame; the next free one if that was taken -
		// the driver tries 512 ports; the editor's own "Play As Listen Server" uses its Server Port setting).
		int32 Port = World->URL.Port;
		if (UNetDriver* Driver = World->GetNetDriver())
		{
			if (const TSharedPtr<const FInternetAddr> Local = Driver->GetLocalAddr())
			{
				Port = Local->GetPort();
			}
		}
		GamePort = Port > 0 ? Port : DefaultGamePort;
		AdvertisedPlayers = FMath::Max(AdvertisedPlayers, 1);
		StopSearch();
		StartBeacon();
		UE_LOG(LogPingu, Display, TEXT("[NET] hosting on port %d, LAN address %s"), GamePort, *GetLocalAddress());
		return;
	}
	if (Mode == NM_Client)
	{
		State = EPinguNetState::Connected;
		bHostTravelPending = false;
		StopBeacon();
		StopSearch();
		ClearLastError();
		UE_LOG(LogPingu, Display, TEXT("[NET] joined the game at %s:%d"), *JoinHost, JoinPort);
		return;
	}

	// A solo world.
	StopBeacon();
	if (State == EPinguNetState::Hosting && bHostTravelPending)
	{
		// The arena reloaded but did not become a server: it would sit in the lobby alone. Back to a plain solo map.
		SetError(FText::FromString(TEXT("Could not host the game.")), ErrorGeneric);
		ReturnToSoloMenu(true);
		return;
	}
	if (State == EPinguNetState::Connected)
	{
		SetError(FText::FromString(TEXT("The host left the game.")), ErrorGeneric);  // the engine took us back by itself
	}
	else if (State == EPinguNetState::Connecting)
	{
		SetError(MakeCouldNotConnectText(), ErrorGeneric);
	}
	State = EPinguNetState::Offline;
	bHostTravelPending = false;
}

void UPinguNetSubsystem::OnWorldBegunPlay(UWorld* World)
{
	if (World->IsGameWorld() && World->GetNetMode() == NM_ListenServer)
	{
		APinguAntiCheat::GetOrSpawn(World); // the fair picks and the join check (PinguAntiCheat.h)
	}
	if (!bNetTest || !World->IsGameWorld())
	{
		return;
	}
	// -PinguNetTest: the test goes on in every world it travels through (clients have no game mode to spawn it).
	TActorIterator<APinguNetTest> Existing(World);
	if (!Existing)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		World->SpawnActor<APinguNetTest>(Params);
	}
}

// ---------------------------------------------------------------------------- session

bool UPinguNetSubsystem::HostGame()
{
	using namespace PinguNetSubsystemPrivate;
	PinguAntiCheat::StartFileDigest(); // (the join check's hash of our game files, in the background)
	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	if (!World || !GEngine)
	{
		return false;
	}
	if (State == EPinguNetState::Hosting && World->GetNetMode() == NM_ListenServer)
	{
		return true;  // already hosting
	}
	const FString Map = GetMapPackageName(World);
	if (Map.IsEmpty())
	{
		return false;
	}
	if (GEngine->PendingNetGameFromWorld(World))
	{
		GEngine->CancelPending(World);  // a join still under way
	}
	ClearLastError();
	StopSearch();
	bReturnPending = false;
	State = EPinguNetState::Hosting;
	bHostTravelPending = true;
	HostStartTime = FPlatformTime::Seconds();
	HostStartFrame = GFrameCounter;
	AdvertisedPlayers = 1;
	bAdvertisedInMatch = false;
	UE_LOG(LogPingu, Display, TEXT("[NET] host: %s?listen?Lobby"), *Map);
	// ?listen makes the map a listen server, ?Lobby tells the game mode to start in the lobby. The map's package name
	// (PIE prefix removed) reloads the same arena in PIE and in -game.
	UGameplayStatics::OpenLevel(World, FName(*Map), true, TEXT("listen?Lobby"));
	return true;
}

bool UPinguNetSubsystem::JoinGame(const FString& Address)
{
	using namespace PinguNetSubsystemPrivate;
	PinguAntiCheat::StartFileDigest(); // (the join check's hash of our game files, in the background)
	FString Host;
	int32 Port = DefaultGamePort;
	if (!ParseAddress(Address, Host, Port))
	{
		SetError(FText::FromString(FString::Printf(TEXT("\"%s\" is not an address. Type it like 192.168.1.20."), *Address.TrimStartAndEnd().Left(40))), ErrorSpecific);
		return false;
	}
	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	if (!World || !GEngine)
	{
		return false;
	}
	if (GEngine->PendingNetGameFromWorld(World))
	{
		GEngine->CancelPending(World);  // a join to somewhere else still under way
	}
	ClearLastError();
	bReturnPending = false;
	bHostTravelPending = false;
	StopBeacon();
	JoinHost = Host;
	JoinPort = Port;
	JoinStartTime = FPlatformTime::Seconds();
	JoinStartFrame = GFrameCounter;

	// ?PinguName reaches the host untouched (the engine swaps ?Name for the online subsystem's nickname if there is one).
	const FString Name = GetPlayerName();
	const FString URL = FString::Printf(TEXT("%s:%d?Name=%s?PinguName=%s"), *Host, Port, *Name, *Name);
	UE_LOG(LogPingu, Display, TEXT("[NET] join %s"), *URL);
	if (APlayerController* PlayerController = GameInstance->GetFirstLocalPlayerController(World))
	{
		PlayerController->ClientTravel(URL, TRAVEL_Absolute);
	}
	else
	{
		GEngine->SetClientTravel(World, *URL, TRAVEL_Absolute);
	}
	State = EPinguNetState::Connecting;
	return true;
}

void UPinguNetSubsystem::LeaveGame()
{
	UE_LOG(LogPingu, Display, TEXT("[NET] leave (%s)"), PinguNetSubsystemPrivate::StateName(State));
	ReturnToSoloMenu(false);
}

void UPinguNetSubsystem::LeaveWithError(const FText& Message)
{
	UE_LOG(LogPingu, Display, TEXT("[NET] leaving: %s"), *Message.ToString());
	if (!IsClient())
	{
		return;
	}
	SetError(Message, PinguNetSubsystemPrivate::ErrorSpecific + 1);
	RequestReturnToSoloMenu(); // (next tick: the engine's own failure handling must not overwrite it)
}

void UPinguNetSubsystem::RequestReturnToSoloMenu()
{
	bReturnPending = true;
}

void UPinguNetSubsystem::ReturnToSoloMenu(bool bForceReload)
{
	using namespace PinguNetSubsystemPrivate;
	// A failure queued the engine's own travel to the default map: only our reload overrides it.
	bForceReload = bForceReload || bReturnPending;
	bReturnPending = false;
	const EPinguNetState OldState = State;
	State = EPinguNetState::Offline;
	bHostTravelPending = false;
	StopBeacon();

	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	if (!World || !GEngine)
	{
		return;
	}
	if (GEngine->PendingNetGameFromWorld(World))
	{
		// Still joining: drop it, or it would finish connecting after we are back in the menu.
		UE_LOG(LogPingu, Log, TEXT("[NET] dropping the join to %s:%d"), *JoinHost, JoinPort);
		GEngine->CancelPending(World);
	}
	// A plain solo world (the menu we joined from) stays; a server / client world, a hosting travel under way or a map
	// loaded for the lobby reloads without options.
	const bool bPlainSolo = World->GetNetMode() == NM_Standalone && OldState != EPinguNetState::Hosting && !World->URL.HasOption(TEXT("Lobby"));
	if (bForceReload || !bPlainSolo)
	{
		const FString Map = GetMapPackageName(World);
		UE_LOG(LogPingu, Display, TEXT("[NET] back to the solo menu: %s"), *Map);
		UGameplayStatics::OpenLevel(World, FName(*Map), true, FString());
	}
}

void UPinguNetSubsystem::SetError(const FText& Message, int32 Priority)
{
	if (Message.IsEmpty())
	{
		return;
	}
	if (LastError.IsEmpty() || Priority >= LastErrorPriority)
	{
		LastError = Message;
		LastErrorPriority = Priority;
		UE_LOG(LogPingu, Display, TEXT("[NET] error: %s"), *Message.ToString());
	}
}

FText UPinguNetSubsystem::MakeCouldNotConnectText() const
{
	const FString Where = JoinPort == DefaultGamePort ? JoinHost : FString::Printf(TEXT("%s:%d"), *JoinHost, JoinPort);
	return FText::FromString(FString::Printf(TEXT("Could not connect to %s."), *Where));
}

// ---------------------------------------------------------------------------- failures

bool UPinguNetSubsystem::IsOurFailure(const UWorld* FailedWorld, const UNetDriver* NetDriver) const
{
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return false;
	}
	if (FailedWorld)
	{
		return FailedWorld->GetGameInstance() == GameInstance;
	}
	// A join that failed before the host's map loaded comes without a world: is it our pending connection?
	UWorld* World = GameInstance->GetWorld();
	const UPendingNetGame* Pending = (GEngine && World) ? GEngine->PendingNetGameFromWorld(World) : nullptr;
	return Pending && NetDriver && Pending->NetDriver.Get() == NetDriver;
}

void UPinguNetSubsystem::HandleNetworkFailure(UWorld* FailedWorld, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString)
{
	using namespace PinguNetSubsystemPrivate;
	if (!IsOurFailure(FailedWorld, NetDriver))
	{
		return;
	}
	const ENetMode FailedMode = FailedWorld ? FailedWorld->GetNetMode() : NM_Client;
	UE_LOG(LogPingu, Log, TEXT("[NET] network failure %s (%s, %s): %s"), ENetworkFailure::ToString(FailureType), NetModeName(FailedMode),
		StateName(State), *ErrorString);

	const bool bServerSetupFailed = FailureType == ENetworkFailure::NetDriverAlreadyExists || FailureType == ENetworkFailure::NetDriverCreateFailure
		|| FailureType == ENetworkFailure::NetDriverListenFailure;
	if (State == EPinguNetState::Hosting)
	{
		// Clients come and go (or time out): the host plays on. Only a server that could not start ends the game.
		if (bServerSetupFailed && FailedMode != NM_Client)
		{
			SetError(FText::FromString(TEXT("Could not host the game.")), ErrorGeneric);
			RequestReturnToSoloMenu();
		}
		return;
	}
	if (!IsClient() || FailedMode == NM_ListenServer || FailedMode == NM_DedicatedServer)
	{
		return;
	}

	const FString Text = ErrorString.TrimStartAndEnd();
	if ((FailureType == ENetworkFailure::PendingConnectionFailure || FailureType == ENetworkFailure::FailureReceived) && IsHostMessage(Text))
	{
		SetError(FText::FromString(Text), ErrorSpecific);  // the host's own words, e.g. "The game is full."
	}
	else if (FailureType == ENetworkFailure::OutdatedClient || FailureType == ENetworkFailure::OutdatedServer)
	{
		SetError(FText::FromString(TEXT("The host has a different version of the game.")), ErrorSpecific);
	}
	else if (State == EPinguNetState::Connecting)
	{
		SetError(MakeCouldNotConnectText(), ErrorGeneric);
	}
	else if (FailureType == ENetworkFailure::ConnectionTimeout)
	{
		SetError(FText::FromString(TEXT("Lost the connection to the host.")), ErrorGeneric);
	}
	else
	{
		SetError(FText::FromString(TEXT("The host left the game.")), ErrorGeneric);
	}
	RequestReturnToSoloMenu();
}

void UPinguNetSubsystem::HandleTravelFailure(UWorld* FailedWorld, ETravelFailure::Type FailureType, const FString& ErrorString)
{
	using namespace PinguNetSubsystemPrivate;
	if (FailedWorld ? FailedWorld->GetGameInstance() != GetGameInstance() : State == EPinguNetState::Offline)
	{
		return;
	}
	UE_LOG(LogPingu, Log, TEXT("[NET] travel failure %s (%s): %s"), ETravelFailure::ToString(FailureType), StateName(State), *ErrorString);
	if (State == EPinguNetState::Hosting)
	{
		SetError(FText::FromString(TEXT("Could not host the game.")), ErrorGeneric);
		RequestReturnToSoloMenu();
		return;
	}
	if (!IsClient())
	{
		return;
	}
	if (FailureType == ETravelFailure::PackageMissing || FailureType == ETravelFailure::PackageVersion || FailureType == ETravelFailure::NoDownload)
	{
		SetError(FText::FromString(TEXT("The host has a different version of the game.")), ErrorSpecific);
	}
	else if (State == EPinguNetState::Connecting)
	{
		SetError(MakeCouldNotConnectText(), ErrorGeneric);
	}
	else
	{
		SetError(FText::FromString(TEXT("Lost the connection to the host.")), ErrorGeneric);
	}
	RequestReturnToSoloMenu();
}

// ---------------------------------------------------------------------------- for the UI

FString UPinguNetSubsystem::GetLocalAddress() const
{
	using namespace PinguNetSubsystemPrivate;
	// The UI asks every frame; listing the adapters is not free.
	const double Now = FPlatformTime::Seconds();
	if (CachedLocalAddress.IsEmpty() || Now - LocalAddressTime > AddressCacheTime)
	{
		const TArray<uint32> Adapters = GetAdapterAddresses();
		uint32 Best = 0;
		int32 BestScore = -1;
		for (const uint32 Ip : Adapters)
		{
			const int32 Score = ScoreAddress(Ip);
			if (Score > BestScore)
			{
				BestScore = Score;
				Best = Ip;
			}
		}
		// Several adapters can score the same (VMware's host-only networks are 192.168.x.1 as well, and nobody else can reach
		// them): the address the system routes outwards from - the LAN / Wi-Fi - wins, as long as it is a home / office
		// network address of this PC. No route (not on a network): the best score as before.
		const uint32 Route = GetRouteAddress();
		const bool bRoute = Route != 0 && Adapters.Contains(Route) && ScoreAddress(Route) >= 2;
		if (bRoute)
		{
			Best = Route;
		}
		const FString Address = Best != 0 ? IpToString(Best) : FString(TEXT("127.0.0.1"));
		if (Address != CachedLocalAddress)
		{
			FString All;
			for (const uint32 Ip : Adapters)
			{
				All += (All.IsEmpty() ? TEXT("") : TEXT(", ")) + IpToString(Ip);
			}
			UE_LOG(LogPingu, Log, TEXT("[NET] address to tell the others: %s (%s; this PC's adapters: %s)"), *Address,
				bRoute ? TEXT("the route out") : TEXT("best of the adapters, no usable route"), All.IsEmpty() ? TEXT("none") : *All);
		}
		CachedLocalAddress = Address;
		bLocalAddressFromRoute = bRoute;
		LocalAddressTime = Now;
	}
	return CachedLocalAddress;
}

FString UPinguNetSubsystem::GetRouteAddressText()
{
	const uint32 Route = PinguNetSubsystemPrivate::GetRouteAddress();
	return Route != 0 ? PinguNetSubsystemPrivate::IpToString(Route) : FString();
}

void UPinguNetSubsystem::SetAdvertisedInfo(int32 Players, bool bInMatch)
{
	const int32 Clamped = FMath::Clamp(Players, 0, 255);
	if (Clamped != AdvertisedPlayers || bInMatch != bAdvertisedInMatch)
	{
		AdvertisedPlayers = Clamped;
		bAdvertisedInMatch = bInMatch;
		NextBeaconTime = 0.0;  // tell the others right away
	}
}

// ---------------------------------------------------------------------------- LAN beacon (host)

void UPinguNetSubsystem::StartBeacon()
{
	bBeaconActive = true;
	bBeaconErrorLogged = false;
	NextBeaconTime = 0.0;
	BeaconTargetsTime = -100.0;
}

void UPinguNetSubsystem::StopBeacon()
{
	if (bBeaconActive && BeaconSocket)
	{
		SendBeacon(true);  // the searching PCs drop us from their lists at once
	}
	bBeaconActive = false;
	PinguNetSubsystemPrivate::DestroySocket(BeaconSocket);
}

void UPinguNetSubsystem::TickBeacon(double Now)
{
	using namespace PinguNetSubsystemPrivate;
	if (!bBeaconActive || State != EPinguNetState::Hosting || Now < NextBeaconTime)
	{
		return;
	}
	NextBeaconTime = Now + BeaconInterval;
	if (!BeaconSocket)
	{
		// An unbound sending socket: it never competes with a search socket for the beacon port.
		BeaconSocket = IsLanReady() ? FUdpSocketBuilder(TEXT("PinguLanBeacon")).AsNonBlocking().WithBroadcast().Build() : nullptr;
		if (!BeaconSocket)
		{
			if (!bBeaconErrorLogged)
			{
				UE_LOG(LogPingu, Warning, TEXT("[NET] could not open the LAN beacon socket: other PCs will not list this game (joining by address works)"));
				bBeaconErrorLogged = true;
			}
			NextBeaconTime = Now + SocketRetry;
			return;
		}
	}
	if (Now - BeaconTargetsTime > TargetsRefresh)
	{
		RefreshBeaconTargets(Now);
	}
	SendBeacon(false);
}

void UPinguNetSubsystem::RefreshBeaconTargets(double Now)
{
	using namespace PinguNetSubsystemPrivate;
	BeaconTargetsTime = Now;
	BeaconTargets.Reset();
	// 255.255.255.255 leaves Windows on one adapter only (the one with the best route), so also every home network's own
	// broadcast address (x.y.z.255: home routers use /24), and 127.0.0.1 for a second copy of the game on this PC.
	BeaconTargets.Add(0xFFFFFFFFu);
	for (const uint32 Ip : GetAdapterAddresses())
	{
		if (ScoreAddress(Ip) >= 2)
		{
			BeaconTargets.AddUnique(Ip | 0xFFu);
		}
	}
	BeaconTargets.AddUnique(0x7F000001u);
}

void UPinguNetSubsystem::SendBeacon(bool bClosing)
{
	using namespace PinguNetSubsystemPrivate;
	ISocketSubsystem* Sockets = GetSockets();
	if (!BeaconSocket || !Sockets)
	{
		return;
	}
	TArray<uint8> Packet;
	Packet.Append(BeaconMagic, MagicSize);
	Packet.Add(BeaconVersion);
	PutU32(Packet, InstanceId);
	PutU16(Packet, uint32(FMath::Clamp(GamePort, 1, 65535)));
	Packet.Add(uint8(FMath::Clamp(AdvertisedPlayers, 0, 255)));
	Packet.Add(uint8(MaxPlayers));
	Packet.Add(uint8((bAdvertisedInMatch ? FlagInMatch : 0) | (bClosing ? FlagClosing : 0)));
	const FString Name = GetPlayerName();
	Packet.Add(uint8(Name.Len()));
	for (const TCHAR Ch : Name)
	{
		PutU16(Packet, uint32(Ch) & 0xFFFFu);
	}

	bool bAnySent = false;
	TSharedRef<FInternetAddr> Target = Sockets->CreateInternetAddr();
	for (const uint32 Ip : BeaconTargets)
	{
		Target->SetIp(Ip);  // the IP first: it sets the address family the port goes into
		Target->SetPort(BeaconPort);
		int32 BytesSent = 0;
		if (BeaconSocket->SendTo(Packet.GetData(), Packet.Num(), BytesSent, *Target) && BytesSent == Packet.Num())
		{
			bAnySent = true;
		}
	}
	if (!bAnySent && !bBeaconErrorLogged)
	{
		UE_LOG(LogPingu, Warning, TEXT("[NET] the LAN beacon could not be sent (%s): no network?"), Sockets->GetSocketError(Sockets->GetLastErrorCode()));
		bBeaconErrorLogged = true;
	}
}

// ---------------------------------------------------------------------------- finding games

void UPinguNetSubsystem::StartSearch()
{
	if (bSearching)
	{
		return;
	}
	bSearching = true;
	NextSearchBindTime = 0.0;
	if (!OpenSearchSocket())
	{
		NextSearchBindTime = FPlatformTime::Seconds() + PinguNetSubsystemPrivate::SocketRetry;
	}
}

void UPinguNetSubsystem::StopSearch()
{
	bSearching = false;
	PinguNetSubsystemPrivate::DestroySocket(SearchSocket);
	FoundGames.Reset();
}

bool UPinguNetSubsystem::OpenSearchSocket()
{
	using namespace PinguNetSubsystemPrivate;
	if (SearchSocket)
	{
		return true;
	}
	// Reusable: a second copy of the game on this PC (the two-process test) binds the same port, and broadcasts reach both.
	SearchSocket = IsLanReady()
		? FUdpSocketBuilder(TEXT("PinguLanSearch")).AsNonBlocking().AsReusable().WithBroadcast().BoundToPort(uint16(BeaconPort))
			.WithReceiveBufferSize(64 * 1024).Build()
		: nullptr;
	if (!SearchSocket)
	{
		if (!bSearchErrorLogged)
		{
			UE_LOG(LogPingu, Warning, TEXT("[NET] could not listen on UDP port %d for LAN games (in use?); retrying. Joining by address works."), BeaconPort);
			bSearchErrorLogged = true;
		}
		return false;
	}
	bSearchErrorLogged = false;
	UE_LOG(LogPingu, Log, TEXT("[NET] looking for LAN games on UDP port %d"), BeaconPort);
	return true;
}

void UPinguNetSubsystem::TickSearch(double Now)
{
	using namespace PinguNetSubsystemPrivate;
	if (!bSearching)
	{
		return;
	}
	if (!SearchSocket && Now >= NextSearchBindTime && !OpenSearchSocket())
	{
		NextSearchBindTime = Now + SocketRetry;
	}
	ISocketSubsystem* Sockets = GetSockets();
	if (SearchSocket && Sockets)
	{
		TSharedRef<FInternetAddr> Sender = Sockets->CreateInternetAddr();
		uint8 Buffer[MaxPacketSize];
		for (int32 Count = 0; Count < MaxPacketsPerTick; ++Count)
		{
			uint32 PendingSize = 0;
			if (!SearchSocket->HasPendingData(PendingSize))
			{
				break;
			}
			int32 BytesRead = 0;
			if (!SearchSocket->RecvFrom(Buffer, MaxPacketSize, BytesRead, *Sender) || BytesRead <= 0)
			{
				break;
			}
			HandleBeacon(Buffer, BytesRead, *Sender, Now);
		}
	}
	FoundGames.RemoveAll([Now](const FPinguLanGame& Game) { return Now - Game.LastSeen > GameTimeout; });
}

void UPinguNetSubsystem::HandleBeacon(const uint8* Data, int32 Size, const FInternetAddr& Sender, double Now)
{
	using namespace PinguNetSubsystemPrivate;
	if (Size < HeaderSize)
	{
		return;
	}
	for (int32 Index = 0; Index < MagicSize; ++Index)
	{
		if (Data[Index] != BeaconMagic[Index])
		{
			return;  // not ours
		}
	}
	FPacketReader Reader;
	Reader.Data = Data;
	Reader.Size = Size;
	Reader.Pos = MagicSize;
	if (Reader.U8() != BeaconVersion)
	{
		return;  // another version of the game: another packet layout
	}
	const uint32 HostId = Reader.U32();
	const int32 Port = int32(Reader.U16());
	const int32 Players = int32(Reader.U8());
	const int32 Max = int32(Reader.U8());
	const uint32 Flags = Reader.U8();
	const int32 NameLength = int32(Reader.U8());
	FString HostName;
	for (int32 Index = 0; Index < NameLength && Reader.bOk; ++Index)
	{
		const uint32 Code = Reader.U16();
		if (Code >= 32 && Code != 127)
		{
			HostName.AppendChar(TCHAR(Code));
		}
	}
	uint32 SenderIp = 0;
	if (!Reader.bOk || HostId == 0 || HostId == InstanceId || Port <= 0 || !GetIPv4(Sender, SenderIp))
	{
		return;  // broken, or our own beacon
	}

	const int32 Found = FoundGames.IndexOfByPredicate([HostId](const FPinguLanGame& Game) { return Game.HostId == HostId; });
	if ((Flags & FlagClosing) != 0)
	{
		if (Found != INDEX_NONE)
		{
			UE_LOG(LogPingu, Log, TEXT("[NET] the game at %s closed"), *FoundGames[Found].Address);
			FoundGames.RemoveAt(Found);
		}
		return;
	}
	const bool bNew = Found == INDEX_NONE;
	FPinguLanGame& Game = bNew ? FoundGames.AddDefaulted_GetRef() : FoundGames[Found];
	Game.HostId = HostId;
	// On the host's own PC the beacon comes in over loopback and over the LAN: keep the LAN address (others can use it too).
	const bool bLoopback = (SenderIp >> 24) == 127;
	if (bNew || !bLoopback || Game.Address.StartsWith(TEXT("127.")))
	{
		Game.Address = FString::Printf(TEXT("%s:%d"), *IpToString(SenderIp), Port);
	}
	Game.HostName = HostName.IsEmpty() ? FString(TEXT("Pingu")) : HostName.Left(MaxNameLength);
	Game.Players = FMath::Clamp(Players, 0, 16);
	Game.MaxPlayers = FMath::Clamp(Max, 1, 16);
	Game.bInMatch = (Flags & FlagInMatch) != 0;
	Game.LastSeen = Now;
	if (bNew)
	{
		UE_LOG(LogPingu, Log, TEXT("[NET] found %s's game at %s (%d/%d%s)"), *Game.HostName, *Game.Address, Game.Players, Game.MaxPlayers,
			Game.bInMatch ? TEXT(", in a match") : TEXT(""));
	}
}
