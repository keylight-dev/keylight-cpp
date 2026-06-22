// Copyright Keylight. All Rights Reserved.
// Keylight.Build.cs — Unreal Build Tool rules for the Keylight plugin module.
//
// To point at the SDK headers, set KEYLIGHT_SDK_INCLUDE_PATH in your project's
// .uproject or override it here. The default assumes the SDK lives as a sibling
// to your Plugins/Keylight directory and you have symlinked/copied the SDK root:
//
//   <YourProject>/
//   ├── Plugins/
//   │   └── Keylight/                 ← this plugin
//   └── ThirdParty/
//       └── keylight-cpp/             ← C++ SDK root (clone of keylight-cpp)
//           └── include/
//               └── keylight/
//                   ├── client.hpp
//                   ├── transport.hpp
//                   └── ...
//
// Then set:  KeylightSdkInclude = Path.Combine(ModuleDirectory, "..", "..", "..",
//                "..", "ThirdParty", "keylight-cpp", "include");
//
// The SDK is header-only for the transport/client API surface consumed here
// (all template/inline implementations are in the headers). No static or
// shared library linkage is required.

using UnrealBuildTool;
using System.IO;

public class Keylight : ModuleRules
{
    public Keylight(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // ── Adjust this path to point at your keylight-cpp SDK checkout ──────
        // Default: ThirdParty/keylight-cpp relative to the project root.
        string ProjectRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", ".."));
        string KeylightSdkInclude = Path.Combine(ProjectRoot, "ThirdParty", "keylight-cpp", "include");

        // Fall back to an environment variable if set (CI / per-dev override).
        string EnvOverride = System.Environment.GetEnvironmentVariable("KEYLIGHT_SDK_INCLUDE_PATH");
        if (!string.IsNullOrEmpty(EnvOverride))
        {
            KeylightSdkInclude = EnvOverride;
        }

        PublicIncludePaths.AddRange(new string[] { });
        PrivateIncludePaths.AddRange(new string[]
        {
            // Make <keylight/transport.hpp> etc. resolvable.
            KeylightSdkInclude,
        });

        // UE module dependencies:
        //   Core, CoreUObject, Engine  — baseline UObject subsystem machinery
        //   HTTP                        — FHttpModule / IHttpRequest
        //   Json                        — FJsonObject (not used directly in the
        //                                 transport, but available for consumers)
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "HTTP",
            "Json",
        });

        // The keylight SDK uses C++17 features (std::optional, structured bindings,
        // std::filesystem on POSIX).  UE 5.x defaults to C++17; this is explicit.
        CppStandard = CppStandardVersion.Cpp17;

        // Silence MSVC warnings about std::string in exported classes.
        // The SDK headers are consumed only within this module, so dllexport
        // boundary concerns do not apply.
        bEnableExceptions = false;
        bUseRTTI = false;
    }
}
