// ============================================================
// backend_profile.h — Lune 后端配置
//
// Lune 的注入、管道和控制台流程对三个运行时保持一致。这里仅保存配套
// DLL 的身份与 IPC 命名规则，避免主流程出现 IL2CPP / Mono / Unreal
// 条件分支。
// ============================================================
#pragma once

enum class BackendKind
{
    Il2Cpp,
    Mono,
    Unreal,
};

struct BackendProfile
{
    BackendKind kind;
    const wchar_t* selector;
    const wchar_t* cliName;
    const wchar_t* runtimeName;
    const wchar_t* runtimeVersion;
    const wchar_t* runtimeTarget;
    const wchar_t* dllName;
    const wchar_t* pipePrefix;
    const wchar_t* sharedMemoryPrefix;
    const char* protocolVersion;
    const wchar_t* prompt;
};

inline constexpr BackendProfile IL2CPP_PROFILE{
    BackendKind::Il2Cpp,
    L"i",
    L"ILune",
    L"Il2CppLua",
    L"4.1.0",
    L"IL2CPP",
    L"Il2CppLua.dll",
    L"\\\\.\\pipe\\Il2CppLua_",
    L"Il2CppLua_Config_",
    "Il2CppLua/4.1.0",
    L"ilune >> ",
};

inline constexpr BackendProfile MONO_PROFILE{
    BackendKind::Mono,
    L"m",
    L"MLune",
    L"MonoLua",
    L"2.0.0",
    L"Mono",
    L"MonoLua.dll",
    L"\\\\.\\pipe\\MonoLua_",
    L"MonoLua_Config_",
    "MonoLua/2.0.0",
    L"mlune >> ",
};

inline constexpr BackendProfile UNREAL_PROFILE{
    BackendKind::Unreal,
    L"u",
    L"ULune",
    L"UnrealLua",
    L"1.0.0",
    L"Unreal Engine",
    L"UnrealLua.dll",
    L"\\\\.\\pipe\\UnrealLua_",
    L"UnrealLua_Config_",
    "UnrealLua/1.0.0",
    L"ulune >> ",
};

inline const BackendProfile* FindBackend(wchar_t selector) noexcept
{
    switch (selector)
    {
    case L'i': return &IL2CPP_PROFILE;
    case L'm': return &MONO_PROFILE;
    case L'u': return &UNREAL_PROFILE;
    default: return nullptr;
    }
}
