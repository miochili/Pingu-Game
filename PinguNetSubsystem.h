// Pingu - LAN multiplayer sessions: host a game, find games on the local network, join by address, leave.
//
// CONTRACT (v2): the UI calls these. One person per PC; the host is a listen server and plays too.
// Lives on the game instance, so it survives the level loads that hosting / joining / leaving do.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/EngineBaseTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "PinguNetSubsystem.generated.h"

class FInternetAddr;
class FSocket;
class UNetDriver;
class UWorld;

/** Where this game instance is in a LAN session. */
UENUM(BlueprintType)
enum class EPinguNetState : uint8
{
	Offline,     // solo / main menu
	Hosting,     // listen server, others can join
	Connecting,  // join requested, travelling to the host
	Connected,   // in the host's game as a client
};

/** A game found on the LAN (from the host's broadcast beacon). */
USTRUCT(BlueprintType)
struct FPinguLanGame
{
	GENERATED_BODY()

	/** "192.168.1.20:7777": pass it to JoinGame. */
	UPROPERTY()
	FString Address;

	UPROPERTY()
	FString HostName;

	UPROPERTY()
	int32 Players = 0;

	UPROPERTY()
	int32 MaxPlayers = 10;

	/** A match is running: whoever joins now plays from its next round. */
	UPROPERTY()
	bool bInMatch = false;

	/** FPlatformTime::Seconds() when the last beacon arrived (games not heard from for a few seconds drop off). */
	double LastSeen = 0.0;

	/** The host's random id from its beacon: one entry per host, even when its beacon arrives over several networks. */
	uint32 HostId = 0;
};

UCLASS()
class PENGUIN_API UPinguNetSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** The subsystem of WorldContext's game instance (null if there is none). */
	static UPinguNetSubsystem* Get(const UObject* WorldContext);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ------------------------------------------------------------------ session
	/** Reopen the arena as a listen server in the lobby and start advertising it on the LAN. */
	bool HostGame();
	/** Travel to a host: "192.168.1.20" or "192.168.1.20:7777". Our player name goes along (?Name=). */
	bool JoinGame(const FString& Address);
	/** Back to the solo main menu. A host closes the game (its clients drop back to their menus). */
	void LeaveGame();
	/** A joiner leaves the host's game and the menu shows Message as the reason (the anti-cheat's "Your games don't match."). */
	void LeaveWithError(const FText& Message);

	// ------------------------------------------------------------------ finding games
	void StartSearch();
	void StopSearch();
	bool IsSearching() const { return bSearching; }
	/** Games heard recently, newest info. */
	const TArray<FPinguLanGame>& GetFoundGames() const { return FoundGames; }

	// ------------------------------------------------------------------ for the UI
	EPinguNetState GetState() const { return State; }
	bool IsHosting() const { return State == EPinguNetState::Hosting; }
	bool IsClient() const { return State == EPinguNetState::Connecting || State == EPinguNetState::Connected; }
	/** This PC's LAN address (best guess, e.g. "192.168.1.20") for the host to tell the others: the adapter the system routes
	 *  outwards from (the Wi-Fi / cable), else the best home-network address of the adapters. It is what the lobby shows and
	 *  copies. */
	FString GetLocalAddress() const;
	/** The last GetLocalAddress came from the route out (false: the best of the adapters; no usable route). */
	bool IsLocalAddressFromRoute() const { return bLocalAddressFromRoute; }
	/** The address the system sends from toward another PC ("10.0.0.5"), "" without a route (what the lobby test compares with). */
	static FString GetRouteAddressText();
	int32 GetGamePort() const { return GamePort; }
	/** Why the last join / game ended badly ("Could not reach 192.168.1.20", "The host left the game"); empty if fine. */
	FText GetLastError() const { return LastError; }
	void ClearLastError() { LastError = FText::GetEmpty(); LastErrorPriority = 0; }

	/** Host: what the LAN beacon tells the others (the game mode keeps it up to date). */
	void SetAdvertisedInfo(int32 Players, bool bInMatch);

	static constexpr int32 DefaultGamePort = 7777;
	static constexpr int32 BeaconPort = 7787;
	static constexpr int32 MaxPlayers = 10;

protected:
	EPinguNetState State = EPinguNetState::Offline;
	bool bSearching = false;
	int32 GamePort = DefaultGamePort;
	TArray<FPinguLanGame> FoundGames;
	FText LastError;

private:
	/** Every frame (core ticker, game thread): notices new worlds, runs the beacon and the search, the join / host watchdogs. */
	bool TickNet(float DeltaTime);
	/** The game instance's world changed: hosting started, joined, or back in a solo world (the world's net mode is the truth). */
	void OnWorldChanged(UWorld* World);
	void OnWorldBegunPlay(UWorld* World);

	void HandleNetworkFailure(UWorld* FailedWorld, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void HandleTravelFailure(UWorld* FailedWorld, ETravelFailure::Type FailureType, const FString& ErrorString);
	/** Engine failures are broadcast for every game instance (several in multi-player PIE): only ours count. */
	bool IsOurFailure(const UWorld* FailedWorld, const UNetDriver* NetDriver) const;
	/** Keeps the most telling message of one failure (the host's own refusal text beats our generic ones). */
	void SetError(const FText& Message, int32 Priority);
	/** The engine queues its own travel (to the default map) while it reports a failure: we return one tick later, which wins. */
	void RequestReturnToSoloMenu();
	/** Stops the beacon, drops a join still under way and reloads the map without options when we are not in a plain solo world. */
	void ReturnToSoloMenu(bool bForceReload);
	FText MakeCouldNotConnectText() const;

	// LAN beacon (host) and search
	void StartBeacon();
	void StopBeacon();
	void TickBeacon(double Now);
	void SendBeacon(bool bClosing);
	void RefreshBeaconTargets(double Now);
	void TickSearch(double Now);
	bool OpenSearchSocket();
	void HandleBeacon(const uint8* Data, int32 Size, const FInternetAddr& Sender, double Now);

	FSocket* BeaconSocket = nullptr;
	FSocket* SearchSocket = nullptr;
	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;

	/** The world we last looked at (weak: the old world is gone after a map load). */
	TWeakObjectPtr<UWorld> CurrentWorld;
	bool bWorldBegunPlay = false;
	/** -PinguNetTest: an APinguNetTest goes into every world. */
	bool bNetTest = false;
	/** Random per game instance: our own beacon is ignored, and copies of one host's beacon count as one game. */
	uint32 InstanceId = 0;
	int32 LastErrorPriority = 0;
	bool bReturnPending = false;

	// joining
	FString JoinHost;
	int32 JoinPort = DefaultGamePort;
	double JoinStartTime = 0.0;
	uint64 JoinStartFrame = 0;
	/** The engine may replace our ?Name= with the online subsystem's nickname: we set it again once in the game. */
	bool bNameSent = false;

	// hosting
	bool bHostTravelPending = false;
	double HostStartTime = 0.0;
	uint64 HostStartFrame = 0;
	bool bBeaconActive = false;
	double NextBeaconTime = 0.0;
	int32 AdvertisedPlayers = 1;
	bool bAdvertisedInMatch = false;
	/** Where the beacon goes (host byte order): 255.255.255.255, each private adapter's x.y.z.255, 127.0.0.1. */
	TArray<uint32> BeaconTargets;
	double BeaconTargetsTime = -100.0;
	bool bBeaconErrorLogged = false;

	// search
	double NextSearchBindTime = 0.0;
	bool bSearchErrorLogged = false;

	mutable FString CachedLocalAddress;
	mutable double LocalAddressTime = -100.0;
	mutable bool bLocalAddressFromRoute = false;
};
