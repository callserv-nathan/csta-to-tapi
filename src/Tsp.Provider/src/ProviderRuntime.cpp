#include "ProviderRuntime.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>

namespace zultys_ncall::tsp {
namespace {

constexpr wchar_t kLineName[] = L"Zultys nCall Bridge";

template <typename T>
LONG InitializeFixedStructure(T* value) {
  if (value == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }

  const DWORD total_size = value->dwTotalSize;
  if (total_size < sizeof(T)) {
    // TAPI callers are expected to allocate enough for the fixed portion
    // before asking for optional variable data.
    return static_cast<LONG>(LINEERR_STRUCTURETOOSMALL);
  }

  std::memset(value, 0, sizeof(T));
  value->dwTotalSize = total_size;
  value->dwNeededSize = sizeof(T);
  value->dwUsedSize = sizeof(T);
  return 0;
}

void AddUnicodeString(LPLINEDEVCAPS caps,
                      const wchar_t* value,
                      DWORD* size_field,
                      DWORD* offset_field) {
  const DWORD text_size =
      static_cast<DWORD>(std::wcslen(value) * sizeof(wchar_t));
  const DWORD required_size = sizeof(LINEDEVCAPS) + text_size;
  caps->dwNeededSize = required_size;
  if (caps->dwTotalSize < required_size) {
    return;
  }

  auto* destination = reinterpret_cast<std::byte*>(caps) + sizeof(LINEDEVCAPS);
  std::memcpy(destination, value, text_size);
  *size_field = text_size;
  *offset_field = sizeof(LINEDEVCAPS);
  caps->dwUsedSize = required_size;
}

DWORD CallFeaturesForState(const DWORD state) {
  DWORD features = LINECALLFEATURE_DROP;
  if (state == LINECALLSTATE_OFFERING) {
    return features;
  }
  if (state == LINECALLSTATE_ONHOLD) {
    return features | LINECALLFEATURE_UNHOLD | LINECALLFEATURE_COMPLETETRANSF;
  }
  if (state == LINECALLSTATE_CONNECTED) {
    return features | LINECALLFEATURE_HOLD | LINECALLFEATURE_BLINDTRANSFER |
           LINECALLFEATURE_SETUPTRANSFER | LINECALLFEATURE_COMPLETETRANSF;
  }
  if (state == LINECALLSTATE_DIALTONE || state == LINECALLSTATE_DIALING ||
      state == LINECALLSTATE_PROCEEDING || state == LINECALLSTATE_RINGBACK) {
    return features;
  }
  return features;
}

bool VersionInRange(const DWORD low_version,
                    const DWORD high_version,
                    DWORD* negotiated_version) {
  if (negotiated_version == nullptr || low_version > high_version) {
    return false;
  }
  const DWORD highest_compatible =
      std::min(high_version, kSupportedTspiVersion);
  if (highest_compatible < low_version ||
      highest_compatible < kMinimumTspiVersion) {
    return false;
  }
  *negotiated_version = highest_compatible;
  return true;
}

std::wstring BridgeKey(const std::wstring& call_id,
                       const std::wstring& device_id) {
  return call_id + L"\x1f" + device_id;
}

DWORD TapiCallState(const BridgeCallEvent& event) {
  switch (event.state) {
    case BridgeCallState::kOffering:
      return event.direction == BridgeCallDirection::kOutgoing
                 ? LINECALLSTATE_DIALING
                 : LINECALLSTATE_OFFERING;
    case BridgeCallState::kConnected:
      return LINECALLSTATE_CONNECTED;
    case BridgeCallState::kHeld:
      return LINECALLSTATE_ONHOLD;
    case BridgeCallState::kDisconnected:
      return LINECALLSTATE_DISCONNECTED;
  }
  return LINECALLSTATE_DISCONNECTED;
}

DWORD TapiCallOrigin(const BridgeCallDirection direction) {
  return direction == BridgeCallDirection::kIncoming
             ? LINECALLORIGIN_INBOUND
             : direction == BridgeCallDirection::kOutgoing
                   ? LINECALLORIGIN_OUTBOUND
                   : LINECALLORIGIN_UNKNOWN;
}

}  // namespace

struct ProviderRuntime::LineContext {
  DWORD device_id{};
  HTAPILINE tapi_line{};
  LINEEVENT event_proc{};
  DWORD media_modes{LINEMEDIAMODE_INTERACTIVEVOICE};
  DWORD line_state_mask{};
  DWORD address_state_mask{};
};

struct ProviderRuntime::CallContext {
  std::shared_ptr<LineContext> line;
  HTAPICALL tapi_call{};
  DWORD provider_call_id{};
  std::wstring client_call_key;
  std::wstring bridge_call_id;
  std::wstring bridge_device_id;
  std::wstring caller_number;
  std::wstring caller_name;
  std::wstring called_number;
  std::wstring original_called_number;
  std::wstring dialed_number;
  DWORD origin{LINECALLORIGIN_UNKNOWN};
  DWORD state{LINECALLSTATE_IDLE};
  std::uint64_t bridge_revision{};
  bool valid_with_tapi{false};
};

ProviderRuntime& ProviderRuntime::Instance() {
  static ProviderRuntime runtime;
  return runtime;
}

ProviderRuntime::ProviderRuntime() = default;

ProviderRuntime::~ProviderRuntime() {
  ProviderShutdown(kSupportedTspiVersion, permanent_provider_id_);
}

LONG ProviderRuntime::ProviderEnumDevices(const DWORD permanent_provider_id,
                                          LPDWORD line_count,
                                          LPDWORD phone_count,
                                          HPROVIDER /*provider*/,
                                          LINEEVENT /*line_create_proc*/,
                                          PHONEEVENT /*phone_create_proc*/) {
  if (line_count == nullptr || phone_count == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }

  *line_count = kProviderLineCount;
  *phone_count = 0;

  std::lock_guard lock(mutex_);
  permanent_provider_id_ = permanent_provider_id;
  return 0;
}

LONG ProviderRuntime::ProviderInit(const DWORD tspi_version,
                                   const DWORD permanent_provider_id,
                                   const DWORD line_device_id_base,
                                   const DWORD phone_device_id_base,
                                   const DWORD_PTR line_count,
                                   const DWORD_PTR phone_count,
                                   ASYNC_COMPLETION completion_proc,
                                   LPDWORD tspi_options) {
  if (tspi_version < kMinimumTspiVersion || completion_proc == nullptr ||
      tspi_options == nullptr || line_count != kProviderLineCount ||
      phone_count != 0) {
    return static_cast<LONG>(LINEERR_OPERATIONFAILED);
  }

  std::lock_guard lock(mutex_);
  if (initialized_) {
    return static_cast<LONG>(LINEERR_OPERATIONFAILED);
  }

  permanent_provider_id_ = permanent_provider_id;
  line_device_id_base_ = line_device_id_base;
  phone_device_id_base_ = phone_device_id_base;
  completion_proc_ = completion_proc;
  *tspi_options = 0;
  bridge_client_ = CreateDefaultBridgeClient();
  bridge_subscriber_ = CreateDefaultBridgeSubscriber();
  initialized_ = true;
  stopping_ = false;
  worker_ = std::jthread([this](const std::stop_token stop_token) {
    WorkerLoop(stop_token);
  });
  event_worker_ = std::jthread([this](const std::stop_token stop_token) {
    EventLoop(stop_token);
  });
  return 0;
}

LONG ProviderRuntime::ProviderShutdown(const DWORD /*tspi_version*/,
                                       const DWORD /*permanent_provider_id*/) {
  std::jthread worker_to_join;
  std::jthread event_worker_to_join;
  {
    std::lock_guard lock(mutex_);
    if (!initialized_) {
      return 0;
    }

    // Do not complete queued commands during TAPI shutdown: TAPISRV has
    // already invalidated the associated request/call handles.
    initialized_ = false;
    stopping_ = true;
    completion_proc_ = nullptr;
    pending_commands_.clear();
    calls_.clear();
    lines_.clear();
    unbound_events_.clear();
    worker_to_join = std::move(worker_);
    event_worker_to_join = std::move(event_worker_);
  }

  worker_to_join.request_stop();
  event_worker_to_join.request_stop();
  queue_ready_.notify_all();
  // Keep bridge_client_ alive until the worker has stopped because its current
  // named-pipe operation may still be running.
  if (worker_to_join.joinable()) {
    worker_to_join.join();
  }
  if (event_worker_to_join.joinable()) {
    event_worker_to_join.join();
  }
  {
    std::lock_guard lock(mutex_);
    bridge_client_.reset();
    bridge_subscriber_.reset();
    stopping_ = false;
  }
  return 0;
}

bool ProviderRuntime::IsKnownDeviceLocked(const DWORD device_id) const {
  return initialized_ && device_id == line_device_id_base_;
}

LONG ProviderRuntime::NegotiateTspiVersion(const DWORD device_id,
                                           const DWORD low_version,
                                           const DWORD high_version,
                                           LPDWORD negotiated_version) {
  if (negotiated_version == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }

  std::lock_guard lock(mutex_);
  if (device_id != INITIALIZE_NEGOTIATION &&
      (!initialized_ || device_id != line_device_id_base_)) {
    return static_cast<LONG>(LINEERR_BADDEVICEID);
  }
  if (!VersionInRange(low_version, high_version, negotiated_version)) {
    return static_cast<LONG>(LINEERR_INCOMPATIBLEAPIVERSION);
  }
  return 0;
}

LONG ProviderRuntime::NegotiateExtVersion(const DWORD device_id,
                                          const DWORD tspi_version,
                                          const DWORD low_version,
                                          const DWORD high_version,
                                          LPDWORD negotiated_version) {
  if (negotiated_version == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }

  std::lock_guard lock(mutex_);
  if (!IsKnownDeviceLocked(device_id)) {
    return static_cast<LONG>(LINEERR_BADDEVICEID);
  }
  if (tspi_version < kMinimumTspiVersion || low_version > 0 ||
      high_version < 0) {
    return static_cast<LONG>(LINEERR_INCOMPATIBLEEXTVERSION);
  }

  *negotiated_version = 0;
  return 0;
}

LONG ProviderRuntime::GetDevCaps(const DWORD device_id,
                                 const DWORD tspi_version,
                                 const DWORD /*ext_version*/,
                                 LPLINEDEVCAPS caps) {
  if (caps == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }

  std::lock_guard lock(mutex_);
  if (!IsKnownDeviceLocked(device_id)) {
    return static_cast<LONG>(LINEERR_BADDEVICEID);
  }
  if (tspi_version < kMinimumTspiVersion) {
    return static_cast<LONG>(LINEERR_INCOMPATIBLEAPIVERSION);
  }
  if (const LONG result = InitializeFixedStructure(caps); result != 0) {
    return result;
  }

  caps->dwPermanentLineID = permanent_provider_id_;
  caps->dwStringFormat = STRINGFORMAT_UNICODE;
  caps->dwAddressModes = LINEADDRESSMODE_ADDRESSID;
  caps->dwNumAddresses = 1;
  caps->dwBearerModes = LINEBEARERMODE_VOICE;
  caps->dwMediaModes = LINEMEDIAMODE_INTERACTIVEVOICE;
  caps->dwMaxNumActiveCalls = 2;
  caps->dwLineStates = LINEDEVSTATE_CONNECTED | LINEDEVSTATE_DISCONNECTED |
                           LINEDEVSTATE_INSERVICE | LINEDEVSTATE_OUTOFSERVICE;
  caps->dwLineFeatures = LINEFEATURE_MAKECALL;
  AddUnicodeString(caps, kLineName, &caps->dwLineNameSize,
                   &caps->dwLineNameOffset);
  return 0;
}

LONG ProviderRuntime::GetAddressCaps(const DWORD device_id,
                                     const DWORD address_id,
                                     const DWORD tspi_version,
                                     const DWORD /*ext_version*/,
                                     LPLINEADDRESSCAPS caps) {
  if (caps == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }

  std::lock_guard lock(mutex_);
  if (!IsKnownDeviceLocked(device_id)) {
    return static_cast<LONG>(LINEERR_BADDEVICEID);
  }
  if (address_id != 0) {
    return static_cast<LONG>(LINEERR_INVALADDRESSID);
  }
  if (tspi_version < kMinimumTspiVersion) {
    return static_cast<LONG>(LINEERR_INCOMPATIBLEAPIVERSION);
  }
  if (const LONG result = InitializeFixedStructure(caps); result != 0) {
    return result;
  }

  caps->dwLineDeviceID = device_id;
  caps->dwAddressSharing = LINEADDRESSSHARING_PRIVATE;
  caps->dwCallStates = LINECALLSTATE_OFFERING | LINECALLSTATE_DIALTONE |
                       LINECALLSTATE_DIALING | LINECALLSTATE_RINGBACK |
                       LINECALLSTATE_CONNECTED | LINECALLSTATE_ONHOLD |
                       LINECALLSTATE_DISCONNECTED | LINECALLSTATE_IDLE;
  caps->dwMaxNumActiveCalls = 2;
  caps->dwMaxNumOnHoldCalls = 1;
  caps->dwCallFeatures = LINECALLFEATURE_DROP | LINECALLFEATURE_HOLD |
                         LINECALLFEATURE_UNHOLD |
                         LINECALLFEATURE_BLINDTRANSFER |
                         LINECALLFEATURE_SETUPTRANSFER |
                         LINECALLFEATURE_COMPLETETRANSF | LINECALLFEATURE_DIAL;
  caps->dwTransferModes = LINETRANSFERMODE_TRANSFER;
  caps->dwAvailableMediaModes = LINEMEDIAMODE_INTERACTIVEVOICE;
  return 0;
}

LONG ProviderRuntime::GetExtensionId(const DWORD device_id,
                                     const DWORD tspi_version,
                                     LPLINEEXTENSIONID extension_id) {
  if (extension_id == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }
  std::lock_guard lock(mutex_);
  if (!IsKnownDeviceLocked(device_id)) {
    return static_cast<LONG>(LINEERR_BADDEVICEID);
  }
  if (tspi_version < kMinimumTspiVersion) {
    return static_cast<LONG>(LINEERR_INCOMPATIBLEAPIVERSION);
  }
  std::memset(extension_id, 0, sizeof(*extension_id));
  return 0;
}

std::shared_ptr<ProviderRuntime::LineContext> ProviderRuntime::FindLineLocked(
    const HDRVLINE driver_line) const {
  const auto found = lines_.find(driver_line);
  return found == lines_.end() ? nullptr : found->second;
}

std::shared_ptr<ProviderRuntime::CallContext> ProviderRuntime::FindCallLocked(
    const HDRVCALL driver_call) const {
  const auto found = calls_.find(driver_call);
  return found == calls_.end() ? nullptr : found->second;
}

LONG ProviderRuntime::OpenLine(const DWORD device_id,
                               HTAPILINE tapi_line,
                               LPHDRVLINE driver_line,
                               const DWORD tspi_version,
                               LINEEVENT event_proc) {
  if (driver_line == nullptr || event_proc == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }
  std::vector<std::shared_ptr<CallContext>> existing_calls;
  {
    std::lock_guard lock(mutex_);
    if (!IsKnownDeviceLocked(device_id)) {
      return static_cast<LONG>(LINEERR_BADDEVICEID);
    }
    if (tspi_version < kMinimumTspiVersion) {
      return static_cast<LONG>(LINEERR_INCOMPATIBLEAPIVERSION);
    }

    auto line = std::make_shared<LineContext>();
    line->device_id = device_id;
    line->tapi_line = tapi_line;
    line->event_proc = event_proc;
    const HDRVLINE handle = reinterpret_cast<HDRVLINE>(line.get());
    lines_.emplace(handle, line);
    *driver_line = handle;

    // A TAPI client may open after the bridge has already observed a call.
    // Introduce each active call through LINE_NEWCALL rather than waiting for
    // the next unrelated MX event.
    for (const auto& entry : unbound_events_) {
      const BridgeCallEvent& event = entry.second;
      if (event.removed) {
        continue;
      }
      auto call = CreateCallLocked(line, nullptr, TapiCallOrigin(event.direction));
      UpdateCallLocked(call, event);
      existing_calls.push_back(std::move(call));
    }
  }

  for (const auto& call : existing_calls) {
    ReportNewCall(call);
  }
  return 0;
}

LONG ProviderRuntime::CloseLine(const HDRVLINE driver_line) {
  std::lock_guard lock(mutex_);
  const auto line = FindLineLocked(driver_line);
  if (line == nullptr) {
    return static_cast<LONG>(LINEERR_INVALLINEHANDLE);
  }

  for (auto iterator = calls_.begin(); iterator != calls_.end();) {
    if (iterator->second->line == line) {
      iterator = calls_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  lines_.erase(driver_line);
  return 0;
}

LONG ProviderRuntime::GetLineDevStatus(const HDRVLINE driver_line,
                                       LPLINEDEVSTATUS status) {
  if (status == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }
  std::lock_guard lock(mutex_);
  const auto line = FindLineLocked(driver_line);
  if (line == nullptr) {
    return static_cast<LONG>(LINEERR_INVALLINEHANDLE);
  }
  if (const LONG result = InitializeFixedStructure(status); result != 0) {
    return result;
  }

  DWORD active_calls = 0;
  DWORD held_calls = 0;
  for (const auto& [_, call] : calls_) {
    if (call->line != line) {
      continue;
    }
    if (call->state == LINECALLSTATE_ONHOLD) {
      ++held_calls;
    } else if (call->state != LINECALLSTATE_IDLE &&
               call->state != LINECALLSTATE_DISCONNECTED) {
      ++active_calls;
    }
  }
  status->dwNumOpens = 1;
  status->dwOpenMediaModes = line->media_modes;
  status->dwNumActiveCalls = active_calls;
  status->dwNumOnHoldCalls = held_calls;
  status->dwLineFeatures = LINEFEATURE_MAKECALL;
  status->dwDevStatusFlags = LINEDEVSTATUSFLAGS_CONNECTED |
                             LINEDEVSTATUSFLAGS_INSERVICE;
  status->dwAvailableMediaModes = LINEMEDIAMODE_INTERACTIVEVOICE;
  return 0;
}

LONG ProviderRuntime::GetAddressStatus(const HDRVLINE driver_line,
                                       const DWORD address_id,
                                       LPLINEADDRESSSTATUS status) {
  if (status == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }
  std::lock_guard lock(mutex_);
  const auto line = FindLineLocked(driver_line);
  if (line == nullptr) {
    return static_cast<LONG>(LINEERR_INVALLINEHANDLE);
  }
  if (address_id != 0) {
    return static_cast<LONG>(LINEERR_INVALADDRESSID);
  }
  if (const LONG result = InitializeFixedStructure(status); result != 0) {
    return result;
  }

  for (const auto& [_, call] : calls_) {
    if (call->line != line) {
      continue;
    }
    if (call->state == LINECALLSTATE_ONHOLD) {
      ++status->dwNumOnHoldCalls;
    } else if (call->state != LINECALLSTATE_IDLE &&
               call->state != LINECALLSTATE_DISCONNECTED) {
      ++status->dwNumActiveCalls;
    }
  }
  status->dwAddressFeatures = LINEADDRFEATURE_MAKECALL;
  return 0;
}

LONG ProviderRuntime::GetAddressId(const HDRVLINE driver_line,
                                   LPDWORD address_id,
                                   const DWORD /*address_mode*/,
                                   LPCWSTR /*address*/,
                                   const DWORD /*size*/) {
  if (address_id == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }
  std::lock_guard lock(mutex_);
  if (FindLineLocked(driver_line) == nullptr) {
    return static_cast<LONG>(LINEERR_INVALLINEHANDLE);
  }
  *address_id = 0;
  return 0;
}

LONG ProviderRuntime::GetNumAddressIds(const HDRVLINE driver_line,
                                       LPDWORD address_count) {
  if (address_count == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }
  std::lock_guard lock(mutex_);
  if (FindLineLocked(driver_line) == nullptr) {
    return static_cast<LONG>(LINEERR_INVALLINEHANDLE);
  }
  *address_count = 1;
  return 0;
}

LONG ProviderRuntime::SetDefaultMediaDetection(const HDRVLINE driver_line,
                                                const DWORD media_modes) {
  std::lock_guard lock(mutex_);
  const auto line = FindLineLocked(driver_line);
  if (line == nullptr) {
    return static_cast<LONG>(LINEERR_INVALLINEHANDLE);
  }
  line->media_modes = media_modes;
  return 0;
}

LONG ProviderRuntime::SetStatusMessages(const HDRVLINE driver_line,
                                        const DWORD line_states,
                                        const DWORD address_states) {
  std::lock_guard lock(mutex_);
  const auto line = FindLineLocked(driver_line);
  if (line == nullptr) {
    return static_cast<LONG>(LINEERR_INVALLINEHANDLE);
  }
  line->line_state_mask = line_states;
  line->address_state_mask = address_states;
  return 0;
}

std::shared_ptr<ProviderRuntime::CallContext> ProviderRuntime::CreateCallLocked(
    const std::shared_ptr<LineContext>& line,
    const HTAPICALL tapi_call,
    const DWORD origin,
    const bool valid_with_tapi) {
  auto call = std::make_shared<CallContext>();
  call->line = line;
  call->tapi_call = tapi_call;
  call->provider_call_id = next_provider_call_id_++;
  call->client_call_key = L"tapi-" + std::to_wstring(call->provider_call_id);
  call->origin = origin;
  call->valid_with_tapi = valid_with_tapi;
  const HDRVCALL handle = reinterpret_cast<HDRVCALL>(call.get());
  calls_.emplace(handle, call);
  return call;
}

std::shared_ptr<ProviderRuntime::CallContext>
ProviderRuntime::FindPendingOutboundLocked(const BridgeCallEvent& event) const {
  if (event.direction != BridgeCallDirection::kOutgoing ||
      event.called_number.empty()) {
    return nullptr;
  }

  for (const auto& [_, call] : calls_) {
    if (!call->bridge_call_id.empty() || call->dialed_number.empty()) {
      continue;
    }
    if ((call->origin == LINECALLORIGIN_OUTBOUND ||
         call->origin == LINECALLORIGIN_INTERNAL) &&
        call->dialed_number == event.called_number) {
      return call;
    }
  }
  return nullptr;
}

DWORD ProviderRuntime::UpdateCallLocked(const std::shared_ptr<CallContext>& call,
                                        const BridgeCallEvent& event) {
  if (call == nullptr) {
    return 0;
  }

  DWORD call_info_changes = 0;
  call->bridge_call_id = event.bridge_call_id;
  call->bridge_device_id = event.bridge_device_id;
  if (!event.caller_number.empty() &&
      call->caller_number != event.caller_number) {
    call->caller_number = event.caller_number;
    call_info_changes |= LINECALLINFOSTATE_CALLERID;
  }
  if (!event.caller_name.empty() && call->caller_name != event.caller_name) {
    call->caller_name = event.caller_name;
    call_info_changes |= LINECALLINFOSTATE_CALLERID;
  }
  if (!event.called_number.empty() &&
      call->called_number != event.called_number) {
    call->called_number = event.called_number;
    call_info_changes |= LINECALLINFOSTATE_CALLEDID;
  }
  if (!event.original_called_number.empty() &&
      call->original_called_number != event.original_called_number) {
    call->original_called_number = event.original_called_number;
    // TAPI exposes the effective destination through CalledID. The bridge
    // intentionally substitutes original-called identity there when CAD
    // supplies it, which preserves the screen-pop destination after a forward.
    call_info_changes |= LINECALLINFOSTATE_CALLEDID;
  }
  if (event.direction != BridgeCallDirection::kUnknown) {
    const DWORD origin = TapiCallOrigin(event.direction);
    if (call->origin != origin) {
      call->origin = origin;
      call_info_changes |= LINECALLINFOSTATE_ORIGIN;
    }
  }
  call->state = event.removed ? LINECALLSTATE_DISCONNECTED
                              : TapiCallState(event);
  call->bridge_revision = std::max(call->bridge_revision, event.revision);
  return call_info_changes;
}

LONG ProviderRuntime::QueueCommand(const DRV_REQUESTID request_id,
                                   BridgeCommand command,
                                   const std::shared_ptr<CallContext>& call) {
  {
    std::lock_guard lock(mutex_);
    if (!initialized_ || stopping_ || completion_proc_ == nullptr ||
        bridge_client_ == nullptr) {
      return static_cast<LONG>(LINEERR_SERVICE_NOT_RUNNING);
    }
    pending_commands_.push_back(PendingCommand{request_id, std::move(command), call});
  }
  queue_ready_.notify_one();
  return static_cast<LONG>(request_id);
}

LONG ProviderRuntime::MakeCall(const DRV_REQUESTID request_id,
                               const HDRVLINE driver_line,
                               const HTAPICALL tapi_call,
                               LPHDRVCALL driver_call,
                               LPCWSTR destination,
                               const DWORD country_code,
                               LPLINECALLPARAMS /*call_params*/) {
  if (driver_call == nullptr || destination == nullptr || *destination == L'\0') {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }

  std::shared_ptr<CallContext> call;
  {
    std::lock_guard lock(mutex_);
    const auto line = FindLineLocked(driver_line);
    if (line == nullptr) {
      return static_cast<LONG>(LINEERR_INVALLINEHANDLE);
    }
    call = CreateCallLocked(line, tapi_call, LINECALLORIGIN_OUTBOUND);
    call->dialed_number = destination;
    *driver_call = reinterpret_cast<HDRVCALL>(call.get());
  }

  BridgeCommand command;
  command.operation = BridgeOperation::kMakeCall;
  command.request_id = request_id;
  command.call_key = call->client_call_key;
  command.destination = destination;
  command.country_code = country_code;
  return QueueCommand(request_id, std::move(command), call);
}

LONG ProviderRuntime::Answer(const DRV_REQUESTID request_id,
                             const HDRVCALL driver_call,
                             LPCSTR /*user_user_info*/,
                             DWORD /*size*/) {
  (void)request_id;
  (void)driver_call;
  // ZAC owns media and answering. The documented third-party WebSocket API
  // used by this bridge has no matching answer command, so this is not
  // advertised in capability data and must fail explicitly if called.
  return static_cast<LONG>(LINEERR_OPERATIONUNAVAIL);
}

LONG ProviderRuntime::Drop(const DRV_REQUESTID request_id,
                           const HDRVCALL driver_call,
                           LPCSTR /*user_user_info*/,
                           DWORD /*size*/) {
  std::shared_ptr<CallContext> call;
  {
    std::lock_guard lock(mutex_);
    call = FindCallLocked(driver_call);
    if (call == nullptr) {
      return static_cast<LONG>(LINEERR_INVALCALLHANDLE);
    }
    if (call->bridge_call_id.empty()) {
      return static_cast<LONG>(LINEERR_INVALCALLSTATE);
    }
  }
  BridgeCommand command;
  command.operation = BridgeOperation::kDrop;
  command.request_id = request_id;
  command.call_key = call->bridge_call_id;
  command.line_key = call->bridge_device_id;
  return QueueCommand(request_id, std::move(command), call);
}

LONG ProviderRuntime::Hold(const DRV_REQUESTID request_id,
                           const HDRVCALL driver_call) {
  std::shared_ptr<CallContext> call;
  {
    std::lock_guard lock(mutex_);
    call = FindCallLocked(driver_call);
    if (call == nullptr) {
      return static_cast<LONG>(LINEERR_INVALCALLHANDLE);
    }
    if (call->bridge_call_id.empty()) {
      return static_cast<LONG>(LINEERR_INVALCALLSTATE);
    }
  }
  BridgeCommand command;
  command.operation = BridgeOperation::kHold;
  command.request_id = request_id;
  command.call_key = call->bridge_call_id;
  command.line_key = call->bridge_device_id;
  return QueueCommand(request_id, std::move(command), call);
}

LONG ProviderRuntime::Unhold(const DRV_REQUESTID request_id,
                             const HDRVCALL driver_call) {
  std::shared_ptr<CallContext> call;
  {
    std::lock_guard lock(mutex_);
    call = FindCallLocked(driver_call);
    if (call == nullptr) {
      return static_cast<LONG>(LINEERR_INVALCALLHANDLE);
    }
    if (call->bridge_call_id.empty()) {
      return static_cast<LONG>(LINEERR_INVALCALLSTATE);
    }
  }
  BridgeCommand command;
  command.operation = BridgeOperation::kUnhold;
  command.request_id = request_id;
  command.call_key = call->bridge_call_id;
  command.line_key = call->bridge_device_id;
  return QueueCommand(request_id, std::move(command), call);
}

LONG ProviderRuntime::BlindTransfer(const DRV_REQUESTID request_id,
                                    const HDRVCALL driver_call,
                                    LPCWSTR destination,
                                    const DWORD country_code) {
  if (destination == nullptr || *destination == L'\0') {
    return static_cast<LONG>(LINEERR_INVALADDRESS);
  }
  std::shared_ptr<CallContext> call;
  {
    std::lock_guard lock(mutex_);
    call = FindCallLocked(driver_call);
    if (call == nullptr) {
      return static_cast<LONG>(LINEERR_INVALCALLHANDLE);
    }
    if (call->bridge_call_id.empty()) {
      return static_cast<LONG>(LINEERR_INVALCALLSTATE);
    }
  }
  BridgeCommand command;
  command.operation = BridgeOperation::kBlindTransfer;
  command.request_id = request_id;
  command.call_key = call->bridge_call_id;
  command.line_key = call->bridge_device_id;
  command.destination = destination;
  command.country_code = country_code;
  return QueueCommand(request_id, std::move(command), call);
}

LONG ProviderRuntime::SetupTransfer(const DRV_REQUESTID request_id,
                                    const HDRVCALL driver_call,
                                    const HTAPICALL tapi_consult_call,
                                    LPHDRVCALL driver_consult_call,
                                    LPLINECALLPARAMS /*call_params*/) {
  if (driver_consult_call == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }

  std::shared_ptr<CallContext> source_call;
  std::shared_ptr<CallContext> consultation_call;
  {
    std::lock_guard lock(mutex_);
    source_call = FindCallLocked(driver_call);
    if (source_call == nullptr) {
      return static_cast<LONG>(LINEERR_INVALCALLHANDLE);
    }
    if (source_call->bridge_call_id.empty()) {
      return static_cast<LONG>(LINEERR_INVALCALLSTATE);
    }
    consultation_call = CreateCallLocked(source_call->line, tapi_consult_call,
                                         LINECALLORIGIN_INTERNAL);
    consultation_call->state = LINECALLSTATE_DIALTONE;
    *driver_consult_call = reinterpret_cast<HDRVCALL>(consultation_call.get());
  }

  BridgeCommand command;
  command.operation = BridgeOperation::kSetupTransfer;
  command.request_id = request_id;
  command.call_key = source_call->bridge_call_id;
  command.line_key = source_call->bridge_device_id;
  return QueueCommand(request_id, std::move(command), consultation_call);
}

LONG ProviderRuntime::Dial(const DRV_REQUESTID request_id,
                           const HDRVCALL driver_call,
                           LPCWSTR destination,
                           const DWORD country_code) {
  if (destination == nullptr || *destination == L'\0') {
    return static_cast<LONG>(LINEERR_INVALADDRESS);
  }
  std::shared_ptr<CallContext> call;
  {
    std::lock_guard lock(mutex_);
    call = FindCallLocked(driver_call);
    if (call == nullptr) {
      return static_cast<LONG>(LINEERR_INVALCALLHANDLE);
    }
    if (!call->valid_with_tapi) {
      return static_cast<LONG>(LINEERR_INVALCALLSTATE);
    }
    call->dialed_number = destination;
  }
  BridgeCommand command;
  command.operation = BridgeOperation::kDialConsultation;
  command.request_id = request_id;
  command.line_key = call->bridge_device_id;
  command.destination = destination;
  command.country_code = country_code;
  return QueueCommand(request_id, std::move(command), call);
}

LONG ProviderRuntime::CompleteTransfer(const DRV_REQUESTID request_id,
                                       const HDRVCALL driver_call,
                                       const HDRVCALL driver_consult_call,
                                       HTAPICALL /*tapi_conference_call*/,
                                       LPHDRVCALL driver_conference_call,
                                       const DWORD transfer_mode) {
  if (driver_conference_call != nullptr) {
    *driver_conference_call = nullptr;
  }
  if (transfer_mode != LINETRANSFERMODE_TRANSFER) {
    return static_cast<LONG>(LINEERR_OPERATIONUNAVAIL);
  }

  std::shared_ptr<CallContext> source_call;
  std::shared_ptr<CallContext> consultation_call;
  {
    std::lock_guard lock(mutex_);
    source_call = FindCallLocked(driver_call);
    consultation_call = FindCallLocked(driver_consult_call);
    if (source_call == nullptr || consultation_call == nullptr) {
      return static_cast<LONG>(LINEERR_INVALCALLHANDLE);
    }
    if (source_call->bridge_call_id.empty() ||
        consultation_call->bridge_call_id.empty()) {
      return static_cast<LONG>(LINEERR_INVALCALLSTATE);
    }
  }
  BridgeCommand command;
  command.operation = BridgeOperation::kCompleteTransfer;
  command.request_id = request_id;
  command.call_key = source_call->bridge_call_id;
  command.related_call_key = consultation_call->bridge_call_id;
  command.line_key = consultation_call->bridge_device_id.empty()
                         ? source_call->bridge_device_id
                         : consultation_call->bridge_device_id;
  command.transfer_mode = transfer_mode;
  return QueueCommand(request_id, std::move(command), source_call);
}

LONG ProviderRuntime::CloseCall(const HDRVCALL driver_call) {
  std::lock_guard lock(mutex_);
  if (calls_.erase(driver_call) == 0) {
    return static_cast<LONG>(LINEERR_INVALCALLHANDLE);
  }
  return 0;
}

LONG ProviderRuntime::GetCallInfo(const HDRVCALL driver_call,
                                  LPLINECALLINFO info) {
  if (info == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }
  std::lock_guard lock(mutex_);
  const auto call = FindCallLocked(driver_call);
  if (call == nullptr) {
    return static_cast<LONG>(LINEERR_INVALCALLHANDLE);
  }
  if (const LONG result = InitializeFixedStructure(info); result != 0) {
    return result;
  }

  info->dwLineDeviceID = call->line->device_id;
  info->dwAddressID = 0;
  info->dwBearerMode = LINEBEARERMODE_VOICE;
  info->dwMediaMode = LINEMEDIAMODE_INTERACTIVEVOICE;
  info->dwCallID = call->provider_call_id;
  info->dwCallStates = LINECALLSTATE_OFFERING | LINECALLSTATE_DIALING |
                       LINECALLSTATE_CONNECTED | LINECALLSTATE_ONHOLD |
                       LINECALLSTATE_DISCONNECTED | LINECALLSTATE_IDLE;
  info->dwOrigin = call->origin;

  const std::wstring& called_number = call->original_called_number.empty()
                                          ? call->called_number
                                          : call->original_called_number;
  const auto byte_count = [](const std::wstring& value) {
    return static_cast<DWORD>(value.size() * sizeof(wchar_t));
  };
  const DWORD caller_number_size = byte_count(call->caller_number);
  const DWORD caller_name_size = byte_count(call->caller_name);
  const DWORD called_number_size = byte_count(called_number);
  DWORD needed = sizeof(LINECALLINFO);
  needed += caller_number_size;
  needed += caller_name_size;
  needed += called_number_size;
  info->dwNeededSize = needed;

  if (!call->caller_number.empty()) {
    info->dwCallerIDFlags |= LINECALLPARTYID_ADDRESS;
  }
  if (!call->caller_name.empty()) {
    info->dwCallerIDFlags |= LINECALLPARTYID_NAME;
  }
  if (!called_number.empty()) {
    info->dwCalledIDFlags |= LINECALLPARTYID_ADDRESS;
  }
  if (info->dwTotalSize < needed) {
    return 0;
  }

  DWORD offset = sizeof(LINECALLINFO);
  const auto append = [&](const std::wstring& value, DWORD* size, DWORD* field_offset) {
    if (value.empty()) {
      return;
    }
    const DWORD bytes = byte_count(value);
    std::memcpy(reinterpret_cast<std::byte*>(info) + offset, value.data(), bytes);
    *size = bytes;
    *field_offset = offset;
    offset += bytes;
  };
  append(call->caller_number, &info->dwCallerIDSize, &info->dwCallerIDOffset);
  append(call->caller_name, &info->dwCallerIDNameSize, &info->dwCallerIDNameOffset);
  append(called_number, &info->dwCalledIDSize, &info->dwCalledIDOffset);
  info->dwUsedSize = offset;
  return 0;
}

LONG ProviderRuntime::GetCallStatus(const HDRVCALL driver_call,
                                    LPLINECALLSTATUS status) {
  if (status == nullptr) {
    return static_cast<LONG>(LINEERR_INVALPOINTER);
  }
  std::lock_guard lock(mutex_);
  const auto call = FindCallLocked(driver_call);
  if (call == nullptr) {
    return static_cast<LONG>(LINEERR_INVALCALLHANDLE);
  }
  if (const LONG result = InitializeFixedStructure(status); result != 0) {
    return result;
  }
  status->dwCallState = call->state;
  status->dwCallPrivilege = LINECALLPRIVILEGE_OWNER;
  status->dwCallFeatures = CallFeaturesForState(call->state);
  return 0;
}

void ProviderRuntime::WorkerLoop(const std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    PendingCommand pending;
    {
      std::unique_lock lock(mutex_);
      queue_ready_.wait(lock, [&] {
        return stop_token.stop_requested() || !pending_commands_.empty();
      });
      if (stop_token.stop_requested()) {
        return;
      }
      pending = std::move(pending_commands_.front());
      pending_commands_.pop_front();
    }

    IBridgePipeClient* bridge_client = nullptr;
    {
      std::lock_guard lock(mutex_);
      if (!initialized_ || bridge_client_ == nullptr) {
        bridge_client = nullptr;
      } else {
        // The pipe client is used by exactly one runtime worker, so a service
        // receives TAPI commands in request order. ProviderShutdown retains
        // ownership until this worker has joined.
        bridge_client = bridge_client_.get();
      }
    }
    BridgeReply reply;
    if (bridge_client == nullptr) {
      reply.tapi_result = static_cast<LONG>(LINEERR_SERVICE_NOT_RUNNING);
    } else {
      reply = bridge_client->Send(pending.command);
    }

    ASYNC_COMPLETION completion = nullptr;
    std::shared_ptr<CallContext> state_notification;
    DWORD state_notification_value = LINECALLSTATE_IDLE;
    {
      std::lock_guard lock(mutex_);
      if (initialized_ && !stopping_) {
        if (pending.call != nullptr && reply.tapi_result == 0) {
          const DWORD old_state = pending.call->state;
          if (!reply.bridge_call_id.empty()) {
            pending.call->bridge_call_id = reply.bridge_call_id;
            pending.call->bridge_device_id = reply.bridge_device_id;
            const auto cached = unbound_events_.find(
                BridgeKey(reply.bridge_call_id, reply.bridge_device_id));
            if (cached != unbound_events_.end()) {
              UpdateCallLocked(pending.call, cached->second);
            }
          }

          if (!pending.call->valid_with_tapi) {
            pending.call->valid_with_tapi = true;
            if (pending.call->state == LINECALLSTATE_IDLE) {
              pending.call->state =
                  pending.command.operation == BridgeOperation::kSetupTransfer
                      ? LINECALLSTATE_DIALTONE
                      : LINECALLSTATE_DIALING;
            }
            state_notification = pending.call;
            state_notification_value = pending.call->state;
          } else if (pending.call->state != old_state) {
            state_notification = pending.call;
            state_notification_value = pending.call->state;
          }
        } else if (pending.call != nullptr && !pending.call->valid_with_tapi) {
          const HDRVCALL handle = reinterpret_cast<HDRVCALL>(pending.call.get());
          const auto found = calls_.find(handle);
          if (found != calls_.end() && found->second == pending.call) {
            calls_.erase(found);
          }
        }
        completion = completion_proc_;
      }
    }
    if (completion != nullptr && !stop_token.stop_requested()) {
      completion(pending.request_id, reply.tapi_result);
    }
    if (reply.tapi_result == 0 && state_notification != nullptr &&
        !stop_token.stop_requested()) {
      NotifyCallState(state_notification, state_notification_value, 0,
                      LINEMEDIAMODE_INTERACTIVEVOICE);
    }
  }
}

void ProviderRuntime::EventLoop(const std::stop_token stop_token) {
  IBridgeEventSubscriber* subscriber = nullptr;
  {
    std::lock_guard lock(mutex_);
    subscriber = bridge_subscriber_.get();
  }
  if (subscriber == nullptr) {
    return;
  }

  subscriber->Run(stop_token, [this](const BridgeCallEvent& event) {
    try {
      ApplyBridgeCallEvent(event);
    } catch (...) {
      // No exception may escape into the long-lived TAPISRV worker thread.
    }
  });
}

void ProviderRuntime::ApplyBridgeCallEvent(const BridgeCallEvent& event) {
  if (event.bridge_call_id.empty() || event.bridge_device_id.empty()) {
    return;
  }

  struct CallInfoNotification {
    std::shared_ptr<CallContext> call;
    DWORD changed_states{};
  };
  struct CallStateNotification {
    std::shared_ptr<CallContext> call;
    DWORD state{};
  };

  std::vector<CallInfoNotification> call_info_notifications;
  std::vector<CallStateNotification> call_state_notifications;
  std::vector<std::shared_ptr<CallContext>> new_calls;
  {
    std::lock_guard lock(mutex_);
    if (!initialized_ || stopping_) {
      return;
    }

    const std::wstring key =
        BridgeKey(event.bridge_call_id, event.bridge_device_id);
    if (event.removed) {
      unbound_events_.erase(key);
    } else {
      unbound_events_[key] = event;
    }

    bool matched = false;
    for (const auto& entry : calls_) {
      const auto& call = entry.second;
      if (call->bridge_call_id != event.bridge_call_id ||
          call->bridge_device_id != event.bridge_device_id) {
        continue;
      }
      matched = true;
      if (event.revision != 0 && call->bridge_revision >= event.revision) {
        continue;
      }
      const DWORD old_state = call->state;
      const DWORD call_info_changes = UpdateCallLocked(call, event);
      if (call->valid_with_tapi && call_info_changes != 0) {
        call_info_notifications.push_back({call, call_info_changes});
      }
      if (call->valid_with_tapi && call->state != old_state) {
        call_state_notifications.push_back({call, call->state});
      }
    }

    if (!matched && !event.removed) {
      const auto pending = FindPendingOutboundLocked(event);
      if (pending != nullptr) {
        const DWORD old_state = pending->state;
        const DWORD call_info_changes = UpdateCallLocked(pending, event);
        if (pending->valid_with_tapi && call_info_changes != 0) {
          call_info_notifications.push_back({pending, call_info_changes});
        }
        if (pending->valid_with_tapi && pending->state != old_state) {
          call_state_notifications.push_back({pending, pending->state});
        }
      } else {
        for (const auto& entry : lines_) {
          const auto& line = entry.second;
          auto call = CreateCallLocked(line, nullptr, TapiCallOrigin(event.direction));
          UpdateCallLocked(call, event);
          new_calls.push_back(std::move(call));
        }
      }
    }
  }

  for (const auto& notification : call_info_notifications) {
    NotifyCallInfo(notification.call, notification.changed_states);
  }
  for (const auto& notification : call_state_notifications) {
    NotifyCallState(notification.call, notification.state, 0,
                    LINEMEDIAMODE_INTERACTIVEVOICE);
  }
  for (const auto& call : new_calls) {
    ReportNewCall(call);
  }
}

void ProviderRuntime::ReportNewCall(const std::shared_ptr<CallContext>& call) {
  if (call == nullptr || call->line == nullptr ||
      call->line->event_proc == nullptr) {
    return;
  }

  HTAPICALL tapi_call = nullptr;
  call->line->event_proc(
      call->line->tapi_line, nullptr, LINE_NEWCALL,
      reinterpret_cast<DWORD_PTR>(call.get()),
      reinterpret_cast<DWORD_PTR>(&tapi_call), 0);
  if (tapi_call == nullptr) {
    std::lock_guard lock(mutex_);
    const HDRVCALL handle = reinterpret_cast<HDRVCALL>(call.get());
    const auto found = calls_.find(handle);
    if (found != calls_.end() && found->second == call) {
      calls_.erase(found);
    }
    return;
  }

  DWORD initial_state = LINECALLSTATE_IDLE;
  bool accepted = false;
  {
    std::lock_guard lock(mutex_);
    const HDRVCALL handle = reinterpret_cast<HDRVCALL>(call.get());
    const auto found = calls_.find(handle);
    if (initialized_ && !stopping_ && found != calls_.end() &&
        found->second == call) {
      call->tapi_call = tapi_call;
      call->valid_with_tapi = true;
      initial_state = call->state;
      accepted = true;
    }
  }
  if (accepted) {
    NotifyCallState(call, initial_state, 0, LINEMEDIAMODE_INTERACTIVEVOICE);
  }
}

void ProviderRuntime::NotifyCallState(const std::shared_ptr<CallContext>& call,
                                      const DWORD state,
                                      const DWORD mode,
                                      const DWORD media_mode) {
  if (call == nullptr || !call->valid_with_tapi || call->line == nullptr ||
      call->line->event_proc == nullptr) {
    return;
  }
  call->line->event_proc(call->line->tapi_line, call->tapi_call, LINE_CALLSTATE,
                         state, mode, media_mode);
}

void ProviderRuntime::NotifyCallInfo(const std::shared_ptr<CallContext>& call,
                                     const DWORD changed_states) {
  if (call == nullptr || changed_states == 0 || !call->valid_with_tapi ||
      call->line == nullptr || call->line->event_proc == nullptr) {
    return;
  }
  call->line->event_proc(call->line->tapi_line, call->tapi_call, LINE_CALLINFO,
                         changed_states, 0, 0);
}

}  // namespace zultys_ncall::tsp
