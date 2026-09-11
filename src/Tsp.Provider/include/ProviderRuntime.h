#pragma once

#include "BridgeProtocol.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>

namespace zultys_ncall::tsp {

// All TSPI entry points delegate here.  The class owns all state that can live
// after an individual TAPI application's process exits because the DLL itself
// is loaded into TAPISRV.
class ProviderRuntime final {
 public:
  static ProviderRuntime& Instance();

  ProviderRuntime(const ProviderRuntime&) = delete;
  ProviderRuntime& operator=(const ProviderRuntime&) = delete;

  LONG ProviderEnumDevices(DWORD permanent_provider_id,
                           LPDWORD line_count,
                           LPDWORD phone_count,
                           HPROVIDER provider,
                           LINEEVENT line_create_proc,
                           PHONEEVENT phone_create_proc);
  LONG ProviderInit(DWORD tspi_version,
                    DWORD permanent_provider_id,
                    DWORD line_device_id_base,
                    DWORD phone_device_id_base,
                    DWORD_PTR line_count,
                    DWORD_PTR phone_count,
                    ASYNC_COMPLETION completion_proc,
                    LPDWORD tspi_options);
  LONG ProviderShutdown(DWORD tspi_version, DWORD permanent_provider_id);

  LONG NegotiateTspiVersion(DWORD device_id,
                            DWORD low_version,
                            DWORD high_version,
                            LPDWORD negotiated_version);
  LONG NegotiateExtVersion(DWORD device_id,
                           DWORD tspi_version,
                           DWORD low_version,
                           DWORD high_version,
                           LPDWORD negotiated_version);
  LONG GetDevCaps(DWORD device_id,
                  DWORD tspi_version,
                  DWORD ext_version,
                  LPLINEDEVCAPS caps);
  LONG GetAddressCaps(DWORD device_id,
                      DWORD address_id,
                      DWORD tspi_version,
                      DWORD ext_version,
                      LPLINEADDRESSCAPS caps);
  LONG GetExtensionId(DWORD device_id,
                      DWORD tspi_version,
                      LPLINEEXTENSIONID extension_id);
  LONG OpenLine(DWORD device_id,
                HTAPILINE tapi_line,
                LPHDRVLINE driver_line,
                DWORD tspi_version,
                LINEEVENT event_proc);
  LONG CloseLine(HDRVLINE driver_line);
  LONG GetLineDevStatus(HDRVLINE driver_line, LPLINEDEVSTATUS status);
  LONG GetAddressStatus(HDRVLINE driver_line,
                        DWORD address_id,
                        LPLINEADDRESSSTATUS status);
  LONG GetAddressId(HDRVLINE driver_line,
                    LPDWORD address_id,
                    DWORD address_mode,
                    LPCWSTR address,
                    DWORD size);
  LONG GetNumAddressIds(HDRVLINE driver_line, LPDWORD address_count);
  LONG SetDefaultMediaDetection(HDRVLINE driver_line, DWORD media_modes);
  LONG SetStatusMessages(HDRVLINE driver_line,
                         DWORD line_states,
                         DWORD address_states);

  LONG MakeCall(DRV_REQUESTID request_id,
                HDRVLINE driver_line,
                HTAPICALL tapi_call,
                LPHDRVCALL driver_call,
                LPCWSTR destination,
                DWORD country_code,
                LPLINECALLPARAMS call_params);
  LONG Answer(DRV_REQUESTID request_id,
              HDRVCALL driver_call,
              LPCSTR user_user_info,
              DWORD size);
  LONG Drop(DRV_REQUESTID request_id,
            HDRVCALL driver_call,
            LPCSTR user_user_info,
            DWORD size);
  LONG Hold(DRV_REQUESTID request_id, HDRVCALL driver_call);
  LONG Unhold(DRV_REQUESTID request_id, HDRVCALL driver_call);
  LONG BlindTransfer(DRV_REQUESTID request_id,
                     HDRVCALL driver_call,
                     LPCWSTR destination,
                     DWORD country_code);
  LONG SetupTransfer(DRV_REQUESTID request_id,
                     HDRVCALL driver_call,
                     HTAPICALL tapi_consult_call,
                     LPHDRVCALL driver_consult_call,
                     LPLINECALLPARAMS call_params);
  LONG Dial(DRV_REQUESTID request_id,
            HDRVCALL driver_call,
            LPCWSTR destination,
            DWORD country_code);
  LONG CompleteTransfer(DRV_REQUESTID request_id,
                        HDRVCALL driver_call,
                        HDRVCALL driver_consult_call,
                        HTAPICALL tapi_conference_call,
                        LPHDRVCALL driver_conference_call,
                        DWORD transfer_mode);
  LONG CloseCall(HDRVCALL driver_call);
  LONG GetCallInfo(HDRVCALL driver_call, LPLINECALLINFO info);
  LONG GetCallStatus(HDRVCALL driver_call, LPLINECALLSTATUS status);

 private:
  ProviderRuntime();
  ~ProviderRuntime();

  struct LineContext;
  struct CallContext;
  struct PendingCommand {
    DRV_REQUESTID request_id{};
    BridgeCommand command{};
    std::shared_ptr<CallContext> call;
  };

  bool IsKnownDeviceLocked(DWORD device_id) const;
  std::shared_ptr<LineContext> FindLineLocked(HDRVLINE driver_line) const;
  std::shared_ptr<CallContext> FindCallLocked(HDRVCALL driver_call) const;
  std::shared_ptr<CallContext> CreateCallLocked(
      const std::shared_ptr<LineContext>& line,
      HTAPICALL tapi_call,
      DWORD origin,
      bool valid_with_tapi = false);
  std::shared_ptr<CallContext> FindPendingOutboundLocked(
      const BridgeCallEvent& event) const;
  DWORD UpdateCallLocked(const std::shared_ptr<CallContext>& call,
                         const BridgeCallEvent& event);
  LONG QueueCommand(DRV_REQUESTID request_id,
                    BridgeCommand command,
                    const std::shared_ptr<CallContext>& call = nullptr);
  void WorkerLoop(std::stop_token stop_token);
  void EventLoop(std::stop_token stop_token);
  void ApplyBridgeCallEvent(const BridgeCallEvent& event);
  void ReportNewCall(const std::shared_ptr<CallContext>& call);
  void NotifyCallState(const std::shared_ptr<CallContext>& call,
                       DWORD state,
                       DWORD mode,
                       DWORD media_mode);
  void NotifyCallInfo(const std::shared_ptr<CallContext>& call,
                      DWORD changed_states);

  mutable std::mutex mutex_;
  std::condition_variable queue_ready_;
  std::deque<PendingCommand> pending_commands_;
  std::unordered_map<HDRVLINE, std::shared_ptr<LineContext>> lines_;
  std::unordered_map<HDRVCALL, std::shared_ptr<CallContext>> calls_;
  std::unordered_map<std::wstring, BridgeCallEvent> unbound_events_;
  std::unique_ptr<IBridgePipeClient> bridge_client_;
  std::unique_ptr<IBridgeEventSubscriber> bridge_subscriber_;
  std::jthread worker_;
  std::jthread event_worker_;
  ASYNC_COMPLETION completion_proc_{};
  DWORD permanent_provider_id_{};
  DWORD line_device_id_base_{};
  DWORD phone_device_id_base_{};
  DWORD next_provider_call_id_{1};
  bool initialized_{false};
  bool stopping_{false};
};

}  // namespace zultys_ncall::tsp
