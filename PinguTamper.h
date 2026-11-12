// Pingu - runtime tamper checks for the packaged game (card 2026-11-05_anti-reverse; builds on PinguAntiCheat).
//
// Three signals, all looking at our OWN process only (no scanning of other processes, no system changes):
//   1. our code section in memory is snapshotted at startup and re-hashed now and then (catches an injected hook that
//      patches our code, e.g. a UE4SS ProcessEvent trampoline),
//   2. a DLL loaded from inside the game folder that is not one of the files we shipped (catches UE4SS-style proxies
//      such as dwmapi.dll, xinput*.dll, version.dll, UE4SS.dll dropped next to the exe),
//   3. a debugger attached to us,
//   4. a signed pak / IoStore block whose signature check failed (the pak was modified; the engine itself only warns).
//      This one is live in every packaged build, any configuration (only a changed pak can trigger it).
//
// The signals are live only in a packaged Shipping game on Windows that is not an automated test run. In the editor, in
// PIE and in -game tests every function is a no-op that reports "clean", so solo play and the tests are unaffected.
//
// On detection nothing is said to the player. The startup result is folded into the join handshake answer
// (PinguAntiCheat::AnswerNonce) so a tampered build silently fails the join check: a tampered joiner is refused with
// "Your games don't match.", and a joiner leaves a tampered host. A tamper seen later (Recheck) makes a joiner leave
// quietly. Honest builds always produce an empty token, so their answers match each other.

#pragma once

#include "CoreMinimal.h"

namespace PinguTamper
{
	/**
	 * Pure, testable in every build config. Is ModulePathFull a foreign DLL - loaded from inside the game folder
	 * (GameRootFull) but NOT one of the files we shipped (ShippedLower: normalised, lowercased full paths)? A module
	 * outside the game folder (Windows, GPU drivers, Steam / Discord / NVIDIA overlays in their own install dirs) is
	 * never foreign. The paths need not be pre-normalised; this normalises them.
	 */
	PENGUIN_API bool IsForeignModule(const FString& ModulePathFull, const FString& GameRootFull, const TSet<FString>& ShippedLower,
		bool bHaveManifest = true);

	/** A DLL name mod loaders / injectors impersonate (dwmapi, version, xinput*, d3d*, dxgi, UE4SS ...). Used for a game folder
	 *  whose staging manifests are missing (bHaveManifest false): then only these count as foreign. */
	PENGUIN_API bool IsKnownProxyName(const FString& FileName);

	/** Full, lowercased, forward slashes, no trailing slash: the form used to compare paths. */
	PENGUIN_API FString NormalizePath(const FString& Path);

	/** Runs the startup checks once and caches them. A no-op outside a packaged Shipping game. Safe everywhere. */
	PENGUIN_API void Init();

	/**
	 * "" when clean; a short fixed tag when a startup tamper was found (a foreign DLL in the game folder, or a debugger
	 * at startup) or a modified pak has been read so far. Folded into the join handshake answer. Honest builds always
	 * return "".
	 */
	PENGUIN_API FString StartupToken();

	/**
	 * A signed pak / IoStore block failed its signature check (the pak was modified). Bound at FileSystemReady to the
	 * engine's signature-failure delegates (FIoDispatcher::OnSignatureError, FPakPlatformFile's handlers), which only log a
	 * warning by themselves. Active in every packaged build, any configuration; never set in the editor or -game.
	 */
	PENGUIN_API bool PakSignatureFailed();

	/**
	 * Re-run the live checks (a debugger now, our code section changed since the startup snapshot, a foreign DLL loaded
	 * since startup). Returns true once tamper is seen (then stays true). Always false outside a packaged Shipping game.
	 * Cheap to call every frame - the heavy code-section hash runs on a background thread, throttled.
	 */
	PENGUIN_API bool Recheck();

	/** For logs and tests: what the checks found (empty when clean or inactive). */
	PENGUIN_API FString DescribeStartup();
}
