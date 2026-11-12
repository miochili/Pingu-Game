// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Penguin : ModuleRules
{
	public Penguin(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "Slate", "SlateCore" });

		// Sockets + Networking: the LAN game finder (UDP broadcast beacon in UPinguNetSubsystem).
		// Niagara: the Ultra effects' layers (APinguFX).
		// RenderCore + RHI: IsRayTracingEnabled / GIsRHIInitialized (the first-start preset pick, UPinguSettings).
		// ApplicationCore: FPlatformApplicationMisc's clipboard (the lobby's address line copies the host's address).
		// Text3D: the 3D menu's extruded, bevelled letters (PinguMenu3D).
		// PakFile: the signed .pak's signature-failure delegate (PinguTamper: a modified pak blocks online play).
		PrivateDependencyModuleNames.AddRange(new string[] { "Sockets", "Networking", "Niagara", "ProceduralMeshComponent", "RenderCore", "RHI", "ApplicationCore", "Text3D", "PakFile" });
	}
}
