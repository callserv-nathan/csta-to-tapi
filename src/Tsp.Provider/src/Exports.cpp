#include "ProviderRuntime.h"

using zultys_ncall::tsp::ProviderRuntime;

namespace {

constexpr LONG kUnexpectedFailure = static_cast<LONG>(LINEERR_OPERATIONFAILED);

template <typename Callable>
LONG InvokeTspi(Callable&& callable) noexcept {
  try {
    return callable();
  } catch (...) {
    // A C++ exception crossing a TSPI ABI boundary can terminate TAPISRV.
    return kUnexpectedFailure;
  }
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE /*instance*/,
                                DWORD /*reason*/,
                                LPVOID /*reserved*/) {
  // Do not connect to the bridge or allocate provider state under the loader
  // lock. TAPI invokes TSPI_providerInit after loading this DLL.
  return TRUE;
}

extern "C" LONG TSPIAPI TSPI_providerEnumDevices(
    DWORD permanent_provider_id,
    LPDWORD line_count,
    LPDWORD phone_count,
    HPROVIDER provider,
    LINEEVENT line_create_proc,
    PHONEEVENT phone_create_proc) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().ProviderEnumDevices(
        permanent_provider_id, line_count, phone_count, provider,
        line_create_proc, phone_create_proc);
  });
}

extern "C" LONG TSPIAPI TSPI_providerInit(
    DWORD tspi_version,
    DWORD permanent_provider_id,
    DWORD line_device_id_base,
    DWORD phone_device_id_base,
    DWORD_PTR line_count,
    DWORD_PTR phone_count,
    ASYNC_COMPLETION completion_proc,
    LPDWORD tspi_options) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().ProviderInit(
        tspi_version, permanent_provider_id, line_device_id_base,
        phone_device_id_base, line_count, phone_count, completion_proc,
        tspi_options);
  });
}

extern "C" LONG TSPIAPI TSPI_providerShutdown(
    DWORD tspi_version,
    DWORD permanent_provider_id) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().ProviderShutdown(tspi_version,
                                                        permanent_provider_id);
  });
}

// TAPI 2.x uses a separate UI DLL for configuration. The MSI/configuration
// utility owns installation and configuration, so these legacy callbacks are
// intentionally no-ops but exported for older TAPI discovery paths.
extern "C" LONG TSPIAPI TSPI_providerConfig(
    HWND /*owner*/, DWORD /*permanent_provider_id*/) {
  return 0;
}

extern "C" LONG TSPIAPI TSPI_providerInstall(
    HWND /*owner*/, DWORD /*permanent_provider_id*/) {
  return 0;
}

extern "C" LONG TSPIAPI TSPI_providerRemove(
    HWND /*owner*/, DWORD /*permanent_provider_id*/) {
  return 0;
}

extern "C" LONG TSPIAPI TSPI_lineNegotiateTSPIVersion(
    DWORD device_id,
    DWORD low_version,
    DWORD high_version,
    LPDWORD negotiated_version) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().NegotiateTspiVersion(
        device_id, low_version, high_version, negotiated_version);
  });
}

extern "C" LONG TSPIAPI TSPI_lineNegotiateExtVersion(
    DWORD device_id,
    DWORD tspi_version,
    DWORD low_version,
    DWORD high_version,
    LPDWORD negotiated_version) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().NegotiateExtVersion(
        device_id, tspi_version, low_version, high_version, negotiated_version);
  });
}

extern "C" LONG TSPIAPI TSPI_lineGetDevCaps(
    DWORD device_id,
    DWORD tspi_version,
    DWORD ext_version,
    LPLINEDEVCAPS caps) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().GetDevCaps(device_id, tspi_version,
                                                   ext_version, caps);
  });
}

extern "C" LONG TSPIAPI TSPI_lineGetAddressCaps(
    DWORD device_id,
    DWORD address_id,
    DWORD tspi_version,
    DWORD ext_version,
    LPLINEADDRESSCAPS caps) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().GetAddressCaps(
        device_id, address_id, tspi_version, ext_version, caps);
  });
}

extern "C" LONG TSPIAPI TSPI_lineGetExtensionID(
    DWORD device_id,
    DWORD tspi_version,
    LPLINEEXTENSIONID extension_id) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().GetExtensionId(device_id, tspi_version,
                                                       extension_id);
  });
}

extern "C" LONG TSPIAPI TSPI_lineOpen(
    DWORD device_id,
    HTAPILINE tapi_line,
    LPHDRVLINE driver_line,
    DWORD tspi_version,
    LINEEVENT event_proc) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().OpenLine(device_id, tapi_line,
                                                driver_line, tspi_version,
                                                event_proc);
  });
}

extern "C" LONG TSPIAPI TSPI_lineClose(
    HDRVLINE driver_line) {
  return InvokeTspi(
      [&] { return ProviderRuntime::Instance().CloseLine(driver_line); });
}

extern "C" LONG TSPIAPI TSPI_lineGetLineDevStatus(
    HDRVLINE driver_line,
    LPLINEDEVSTATUS status) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().GetLineDevStatus(driver_line, status);
  });
}

extern "C" LONG TSPIAPI TSPI_lineGetAddressStatus(
    HDRVLINE driver_line,
    DWORD address_id,
    LPLINEADDRESSSTATUS status) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().GetAddressStatus(driver_line, address_id,
                                                         status);
  });
}

extern "C" LONG TSPIAPI TSPI_lineGetAddressID(
    HDRVLINE driver_line,
    LPDWORD address_id,
    DWORD address_mode,
    LPCWSTR address,
    DWORD size) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().GetAddressId(driver_line, address_id,
                                                     address_mode, address, size);
  });
}

extern "C" LONG TSPIAPI TSPI_lineGetNumAddressIDs(
    HDRVLINE driver_line,
    LPDWORD address_count) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().GetNumAddressIds(driver_line,
                                                        address_count);
  });
}

extern "C" LONG TSPIAPI TSPI_lineSetDefaultMediaDetection(
    HDRVLINE driver_line,
    DWORD media_modes) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().SetDefaultMediaDetection(driver_line,
                                                                 media_modes);
  });
}

extern "C" LONG TSPIAPI TSPI_lineSetStatusMessages(
    HDRVLINE driver_line,
    DWORD line_states,
    DWORD address_states) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().SetStatusMessages(driver_line,
                                                         line_states,
                                                         address_states);
  });
}

extern "C" LONG TSPIAPI TSPI_lineMakeCall(
    DRV_REQUESTID request_id,
    HDRVLINE driver_line,
    HTAPICALL tapi_call,
    LPHDRVCALL driver_call,
    LPCWSTR destination,
    DWORD country_code,
    LPLINECALLPARAMS call_params) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().MakeCall(
        request_id, driver_line, tapi_call, driver_call, destination,
        country_code, call_params);
  });
}

extern "C" LONG TSPIAPI TSPI_lineAnswer(
    DRV_REQUESTID request_id,
    HDRVCALL driver_call,
    LPCSTR user_user_info,
    DWORD size) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().Answer(request_id, driver_call,
                                              user_user_info, size);
  });
}

extern "C" LONG TSPIAPI TSPI_lineDrop(
    DRV_REQUESTID request_id,
    HDRVCALL driver_call,
    LPCSTR user_user_info,
    DWORD size) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().Drop(request_id, driver_call,
                                            user_user_info, size);
  });
}

extern "C" LONG TSPIAPI TSPI_lineHold(
    DRV_REQUESTID request_id,
    HDRVCALL driver_call) {
  return InvokeTspi(
      [&] { return ProviderRuntime::Instance().Hold(request_id, driver_call); });
}

extern "C" LONG TSPIAPI TSPI_lineUnhold(
    DRV_REQUESTID request_id,
    HDRVCALL driver_call) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().Unhold(request_id, driver_call);
  });
}

extern "C" LONG TSPIAPI TSPI_lineBlindTransfer(
    DRV_REQUESTID request_id,
    HDRVCALL driver_call,
    LPCWSTR destination,
    DWORD country_code) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().BlindTransfer(
        request_id, driver_call, destination, country_code);
  });
}

extern "C" LONG TSPIAPI TSPI_lineSetupTransfer(
    DRV_REQUESTID request_id,
    HDRVCALL driver_call,
    HTAPICALL tapi_consult_call,
    LPHDRVCALL driver_consult_call,
    LPLINECALLPARAMS call_params) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().SetupTransfer(
        request_id, driver_call, tapi_consult_call, driver_consult_call,
        call_params);
  });
}

extern "C" LONG TSPIAPI TSPI_lineDial(
    DRV_REQUESTID request_id,
    HDRVCALL driver_call,
    LPCWSTR destination,
    DWORD country_code) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().Dial(request_id, driver_call,
                                            destination, country_code);
  });
}

extern "C" LONG TSPIAPI TSPI_lineCompleteTransfer(
    DRV_REQUESTID request_id,
    HDRVCALL driver_call,
    HDRVCALL driver_consult_call,
    HTAPICALL tapi_conference_call,
    LPHDRVCALL driver_conference_call,
    DWORD transfer_mode) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().CompleteTransfer(
        request_id, driver_call, driver_consult_call, tapi_conference_call,
        driver_conference_call, transfer_mode);
  });
}

extern "C" LONG TSPIAPI TSPI_lineCloseCall(
    HDRVCALL driver_call) {
  return InvokeTspi(
      [&] { return ProviderRuntime::Instance().CloseCall(driver_call); });
}

extern "C" LONG TSPIAPI TSPI_lineGetCallInfo(
    HDRVCALL driver_call,
    LPLINECALLINFO info) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().GetCallInfo(driver_call, info);
  });
}

extern "C" LONG TSPIAPI TSPI_lineGetCallStatus(
    HDRVCALL driver_call,
    LPLINECALLSTATUS status) {
  return InvokeTspi([&] {
    return ProviderRuntime::Instance().GetCallStatus(driver_call, status);
  });
}
