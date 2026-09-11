#pragma once

// nCall uses the classic TAPI/C surface.  Keep the provider on TAPI 2.2 until
// a later compatibility test proves that a newer TSPI level buys us something.
// This must precede the Windows SDK TAPI headers.
#ifndef TAPI_CURRENT_VERSION
#define TAPI_CURRENT_VERSION 0x00020002
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <tapi.h>
#include <tspi.h>

namespace zultys_ncall::tsp {

inline constexpr DWORD kMinimumTspiVersion = 0x00020000;
inline constexpr DWORD kSupportedTspiVersion = 0x00020002;
inline constexpr DWORD kProviderLineCount = 1;
inline constexpr wchar_t kDefaultPipeName[] = L"\\\\.\\pipe\\Zultys.NCall.Bridge.v1";

}  // namespace zultys_ncall::tsp
