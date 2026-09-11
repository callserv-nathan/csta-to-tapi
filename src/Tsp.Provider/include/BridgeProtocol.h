#pragma once

#include "TapiCompatibility.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace zultys_ncall::tsp {

// This is deliberately small.  The bridge service owns MX/WebSocket state;
// the TSP only translates TAPI requests and receives normalized call events.
enum class BridgeOperation {
  kMakeCall,
  kAnswer,
  kDrop,
  kHold,
  kUnhold,
  kBlindTransfer,
  kSetupTransfer,
  kDialConsultation,
  kCompleteTransfer,
};

struct BridgeCommand {
  BridgeOperation operation{};
  DRV_REQUESTID request_id{};
  // Empty means the bridge's configured single operator/device. The v1 TSP
  // has one line and intentionally does not duplicate service configuration.
  std::wstring line_key;
  std::wstring call_key;
  std::wstring related_call_key;
  std::wstring destination;
  DWORD country_code{};
  DWORD transfer_mode{};
};

struct BridgeReply {
  // TAPI result returned through ASYNC_COMPLETION.  Zero is success.
  LONG tapi_result{static_cast<LONG>(LINEERR_OPERATIONFAILED)};
  std::wstring bridge_call_id;
  std::wstring bridge_device_id;
};

enum class BridgeCallState {
  kOffering,
  kConnected,
  kHeld,
  kDisconnected,
};

enum class BridgeCallDirection {
  kUnknown,
  kIncoming,
  kOutgoing,
};

struct BridgeCallEvent {
  std::wstring bridge_call_id;
  std::wstring bridge_device_id;
  BridgeCallState state{BridgeCallState::kOffering};
  BridgeCallDirection direction{BridgeCallDirection::kUnknown};
  std::wstring caller_number;
  std::wstring caller_name;
  std::wstring called_number;
  std::wstring original_called_number;
  std::uint64_t revision{};
  bool removed{false};
};

using BridgeEventCallback = std::function<void(const BridgeCallEvent&)>;

// The wire contract is a little-endian uint32 byte length followed by UTF-8
// JSON PipeEnvelope values. Commands use type="Command" and a string request
// ID. The bridge currently returns CommandCompleted with a BridgeCommandResult
// payload; callers must tolerate unrelated CallChanged messages interleaved on
// the same subscription. Keeping direct named-pipe I/O behind an interface
// also lets a deterministic in-memory client be supplied by the simulator test
// harness later.
class IBridgePipeClient {
 public:
  virtual ~IBridgePipeClient() = default;
  virtual BridgeReply Send(const BridgeCommand& command) = 0;
};

class NamedPipeBridgeClient final : public IBridgePipeClient {
 public:
  explicit NamedPipeBridgeClient(std::wstring pipe_name = kDefaultPipeName,
                                 DWORD timeout_ms = 10'000);

  BridgeReply Send(const BridgeCommand& command) override;

 private:
  std::wstring pipe_name_;
  DWORD timeout_ms_;
};

class IBridgeEventSubscriber {
 public:
  virtual ~IBridgeEventSubscriber() = default;
  virtual void Run(std::stop_token stop_token,
                   const BridgeEventCallback& on_event) = 0;
};

class NamedPipeBridgeSubscriber final : public IBridgeEventSubscriber {
 public:
  explicit NamedPipeBridgeSubscriber(
      std::wstring pipe_name = kDefaultPipeName,
      DWORD timeout_ms = 1'000);

  void Run(std::stop_token stop_token,
           const BridgeEventCallback& on_event) override;

 private:
  std::wstring pipe_name_;
  DWORD timeout_ms_;
};

std::unique_ptr<IBridgePipeClient> CreateDefaultBridgeClient();
std::unique_ptr<IBridgeEventSubscriber> CreateDefaultBridgeSubscriber();

}  // namespace zultys_ncall::tsp
