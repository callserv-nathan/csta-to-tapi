#include "BridgeProtocol.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <string_view>
#include <utility>

namespace zultys_ncall::tsp {
namespace {

constexpr DWORD kMaxFrameBytes = 65'536;
constexpr DWORD kDefaultTapiFailure = static_cast<DWORD>(LINEERR_OPERATIONFAILED);

class ScopedHandle final {
 public:
  explicit ScopedHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
  ~ScopedHandle() {
    if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
      ::CloseHandle(value_);
    }
  }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;

  HANDLE get() const { return value_; }

 private:
  HANDLE value_;
};

std::string ToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }

  const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    return {};
  }

  std::string result(static_cast<size_t>(bytes), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            bytes, nullptr, nullptr) != bytes) {
    return {};
  }
  return result;
}

std::wstring FromUtf8(const std::string_view value) {
  if (value.empty()) {
    return {};
  }

  const int characters = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0);
  if (characters <= 0) {
    return {};
  }

  std::wstring result(static_cast<size_t>(characters), L'\0');
  return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), result.data(),
                               characters) == characters
             ? result
             : std::wstring{};
}

std::string EscapeJson(const std::string_view value) {
  std::string result;
  result.reserve(value.size() + 8);
  constexpr char kHex[] = "0123456789abcdef";
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\b':
        result += "\\b";
        break;
      case '\f':
        result += "\\f";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (character < 0x20) {
          result += "\\u00";
          result += kHex[(character >> 4) & 0x0f];
          result += kHex[character & 0x0f];
        } else {
          result += static_cast<char>(character);
        }
        break;
    }
  }
  return result;
}

std::string JsonStringOrNull(const std::string& value) {
  return value.empty() ? "null" : "\"" + EscapeJson(value) + "\"";
}

const char* OperationName(const BridgeOperation operation) {
  switch (operation) {
    case BridgeOperation::kMakeCall:
      return "MakeCall";
    case BridgeOperation::kAnswer:
      return "Answer";
    case BridgeOperation::kDrop:
      return "ClearConnection";
    case BridgeOperation::kHold:
      return "HoldCall";
    case BridgeOperation::kUnhold:
      return "RetrieveCall";
    case BridgeOperation::kBlindTransfer:
      return "SingleStepTransferCall";
    case BridgeOperation::kSetupTransfer:
      return "HoldCall";
    case BridgeOperation::kDialConsultation:
      return "MakeCall";
    case BridgeOperation::kCompleteTransfer:
      return "TransferCall";
  }
  return "Unknown";
}

std::string BuildCommandEnvelope(const BridgeCommand& command,
                                 const std::uint64_t sequence) {
  const auto line_key = ToUtf8(command.line_key);
  const auto call_key = ToUtf8(command.call_key);
  const auto related_call_key = ToUtf8(command.related_call_key);
  const auto destination = ToUtf8(command.destination);

  // The outer PipeEnvelope fields are intentionally shared with the service:
  // { type, sequence, requestId, payload }.  The command payload is small and
  // stable enough to keep this native DLL free of a JSON dependency.
  return "{\"type\":\"Command\",\"sequence\":" +
         std::to_string(sequence) + ",\"requestId\":\"tapi-" +
         std::to_string(command.request_id) +
         "\",\"payload\":{\"operation\":\"" +
         OperationName(command.operation) + "\",\"callId\":" +
         JsonStringOrNull(call_key) + ",\"deviceId\":" +
         JsonStringOrNull(line_key) + ",\"destination\":" +
         JsonStringOrNull(destination) + ",\"activeCallId\":" +
         JsonStringOrNull(related_call_key) + ",\"activeDeviceId\":" +
         JsonStringOrNull(line_key) + "}}";
}

std::string BuildHelloEnvelope(const std::uint64_t sequence) {
  return "{\"type\":\"Hello\",\"sequence\":" + std::to_string(sequence) +
         ",\"requestId\":\"hello-" + std::to_string(sequence) +
         "\",\"payload\":{\"clientName\":\"Tsp.Provider\","
         "\"protocolVersion\":1}}";
}

std::string BuildSubscribeEnvelope(const std::uint64_t sequence) {
  return "{\"type\":\"Subscribe\",\"sequence\":" +
         std::to_string(sequence) + ",\"requestId\":\"subscribe-" +
         std::to_string(sequence) + "\",\"payload\":{}}";
}

enum class TransferResult {
  kComplete,
  kTimedOut,
  kFailed,
};

TransferResult Transfer(HANDLE pipe,
                        void* buffer,
                        const DWORD byte_count,
                        const bool write,
                        const DWORD timeout_ms) {
  auto* bytes = static_cast<unsigned char*>(buffer);
  DWORD offset = 0;
  while (offset < byte_count) {
    ScopedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.get() == nullptr || event.get() == INVALID_HANDLE_VALUE) {
      return TransferResult::kFailed;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD transferred = 0;
    const DWORD remaining = byte_count - offset;
    const BOOL complete = write
                              ? ::WriteFile(pipe, bytes + offset, remaining,
                                            &transferred, &overlapped)
                              : ::ReadFile(pipe, bytes + offset, remaining,
                                           &transferred, &overlapped);

    if (!complete) {
      const DWORD error = ::GetLastError();
      if (error != ERROR_IO_PENDING) {
        return TransferResult::kFailed;
      }

      if (::WaitForSingleObject(event.get(), timeout_ms) != WAIT_OBJECT_0) {
        // It is unsafe to free the OVERLAPPED/event storage until the cancelled
        // operation has completed. A partial frame cannot be resumed safely,
        // so the caller reconnects when cancellation transferred any bytes.
        ::CancelIoEx(pipe, &overlapped);
        ::GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
        return offset == 0 && transferred == 0 ? TransferResult::kTimedOut
                                                : TransferResult::kFailed;
      }

      if (!::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
        return TransferResult::kFailed;
      }
    }

    if (transferred == 0 || transferred > remaining) {
      return TransferResult::kFailed;
    }
    offset += transferred;
  }

  return TransferResult::kComplete;
}

bool WriteFrame(HANDLE pipe, const std::string_view body, const DWORD timeout_ms) {
  if (body.empty() || body.size() > kMaxFrameBytes) {
    return false;
  }

  const std::uint32_t size = static_cast<std::uint32_t>(body.size());
  std::array<std::uint8_t, sizeof(size)> header{};
  header[0] = static_cast<std::uint8_t>(size & 0xff);
  header[1] = static_cast<std::uint8_t>((size >> 8) & 0xff);
  header[2] = static_cast<std::uint8_t>((size >> 16) & 0xff);
  header[3] = static_cast<std::uint8_t>((size >> 24) & 0xff);

  return Transfer(pipe, header.data(), static_cast<DWORD>(header.size()), true,
                  timeout_ms) == TransferResult::kComplete &&
         Transfer(pipe, const_cast<char*>(body.data()),
                  static_cast<DWORD>(body.size()), true, timeout_ms) ==
             TransferResult::kComplete;
}

enum class FrameReadResult {
  kComplete,
  kTimedOut,
  kFailed,
};

FrameReadResult ReadFrameWithStatus(HANDLE pipe,
                                    std::string* body,
                                    const DWORD timeout_ms) {
  if (body == nullptr) {
    return FrameReadResult::kFailed;
  }

  std::array<std::uint8_t, sizeof(std::uint32_t)> header{};
  const auto header_result =
      Transfer(pipe, header.data(), static_cast<DWORD>(header.size()), false,
               timeout_ms);
  if (header_result == TransferResult::kTimedOut) {
    return FrameReadResult::kTimedOut;
  }
  if (header_result != TransferResult::kComplete) {
    return FrameReadResult::kFailed;
  }

  const std::uint32_t size = static_cast<std::uint32_t>(header[0]) |
                             (static_cast<std::uint32_t>(header[1]) << 8) |
                             (static_cast<std::uint32_t>(header[2]) << 16) |
                             (static_cast<std::uint32_t>(header[3]) << 24);
  if (size == 0 || size > kMaxFrameBytes) {
    return FrameReadResult::kFailed;
  }

  body->assign(size, '\0');
  return Transfer(pipe, body->data(), size, false, timeout_ms) ==
                 TransferResult::kComplete
             ? FrameReadResult::kComplete
             : FrameReadResult::kFailed;
}

bool ReadFrame(HANDLE pipe, std::string* body, const DWORD timeout_ms) {
  return ReadFrameWithStatus(pipe, body, timeout_ms) ==
         FrameReadResult::kComplete;
}

/*
 * The bridge uses byte-mode pipes, so a ReadFile operation can complete with
 * less than the requested count. Transfer loops until a whole frame segment
 * is read or written; frame boundaries remain owned by the uint32 prefix.
 */
std::string_view TrimLeft(const std::string_view value) {
  size_t index = 0;
  while (index < value.size() &&
         (value[index] == ' ' || value[index] == '\t' || value[index] == '\r' ||
          value[index] == '\n')) {
    ++index;
  }
  return value.substr(index);
}

bool ReadJsonString(const std::string_view document,
                    const std::string_view key,
                    std::string* value) {
  const std::string marker = "\"" + std::string(key) + "\"";
  const size_t marker_position = document.find(marker);
  if (marker_position == std::string_view::npos) {
    return false;
  }
  const size_t colon = document.find(':', marker_position + marker.size());
  if (colon == std::string_view::npos) {
    return false;
  }
  std::string_view remaining = TrimLeft(document.substr(colon + 1));
  if (remaining.empty() || remaining.front() != '"') {
    return false;
  }

  std::string decoded;
  for (size_t index = 1; index < remaining.size(); ++index) {
    const char character = remaining[index];
    if (character == '"') {
      *value = std::move(decoded);
      return true;
    }
    if (character != '\\' || ++index >= remaining.size()) {
      decoded += character;
      continue;
    }
    switch (remaining[index]) {
      case '"':
      case '\\':
      case '/':
        decoded += remaining[index];
        break;
      case 'b':
        decoded += '\b';
        break;
      case 'f':
        decoded += '\f';
        break;
      case 'n':
        decoded += '\n';
        break;
      case 'r':
        decoded += '\r';
        break;
      case 't':
        decoded += '\t';
        break;
      default:
        // Reply identifiers are ASCII/UTF-8. Unicode-escaped identifiers need
        // a real JSON parser when the native and managed DTOs are unified.
        return false;
    }
  }
  return false;
}

bool ReadJsonBoolean(const std::string_view document,
                     const std::string_view key,
                     bool* value) {
  if (value == nullptr) {
    return false;
  }
  const std::string marker = "\"" + std::string(key) + "\"";
  const size_t marker_position = document.find(marker);
  if (marker_position == std::string_view::npos) {
    return false;
  }
  const size_t colon = document.find(':', marker_position + marker.size());
  if (colon == std::string_view::npos) {
    return false;
  }
  const std::string_view bool_value = TrimLeft(document.substr(colon + 1));
  if (bool_value.starts_with("true")) {
    *value = true;
    return true;
  }
  if (bool_value.starts_with("false")) {
    *value = false;
    return true;
  }
  return false;
}

bool ReadJsonUint64(const std::string_view document,
                    const std::string_view key,
                    std::uint64_t* value) {
  if (value == nullptr) {
    return false;
  }
  const std::string marker = "\"" + std::string(key) + "\"";
  const size_t marker_position = document.find(marker);
  if (marker_position == std::string_view::npos) {
    return false;
  }
  const size_t colon = document.find(':', marker_position + marker.size());
  if (colon == std::string_view::npos) {
    return false;
  }
  const std::string_view number = TrimLeft(document.substr(colon + 1));
  std::uint64_t parsed = 0;
  size_t position = 0;
  while (position < number.size() && number[position] >= '0' &&
         number[position] <= '9') {
    const std::uint64_t digit =
        static_cast<std::uint64_t>(number[position] - '0');
    if (parsed > (UINT64_MAX - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
    ++position;
  }
  if (position == 0) {
    return false;
  }
  *value = parsed;
  return true;
}

bool IsEnvelopeType(const std::string_view envelope,
                    const std::string_view expected_type) {
  std::string type;
  return ReadJsonString(envelope, "type", &type) && type == expected_type;
}

bool ParseCallChanged(const std::string_view envelope, BridgeCallEvent* event) {
  if (event == nullptr || !IsEnvelopeType(envelope, "CallChanged")) {
    return false;
  }

  std::string call_id;
  std::string device_id;
  std::string state;
  if (!ReadJsonString(envelope, "callId", &call_id) ||
      !ReadJsonString(envelope, "deviceId", &device_id) ||
      !ReadJsonString(envelope, "state", &state)) {
    return false;
  }

  BridgeCallEvent parsed;
  parsed.bridge_call_id = FromUtf8(call_id);
  parsed.bridge_device_id = FromUtf8(device_id);
  if (parsed.bridge_call_id.empty() || parsed.bridge_device_id.empty()) {
    return false;
  }

  if (state == "Offering") {
    parsed.state = BridgeCallState::kOffering;
  } else if (state == "Connected") {
    parsed.state = BridgeCallState::kConnected;
  } else if (state == "Held") {
    parsed.state = BridgeCallState::kHeld;
  } else if (state == "Disconnected") {
    parsed.state = BridgeCallState::kDisconnected;
  } else {
    return false;
  }

  std::string direction;
  if (ReadJsonString(envelope, "direction", &direction)) {
    if (direction == "Incoming") {
      parsed.direction = BridgeCallDirection::kIncoming;
    } else if (direction == "Outgoing") {
      parsed.direction = BridgeCallDirection::kOutgoing;
    }
  }

  std::string value;
  if (ReadJsonString(envelope, "callingNumber", &value)) {
    parsed.caller_number = FromUtf8(value);
  }
  if (ReadJsonString(envelope, "callingName", &value)) {
    parsed.caller_name = FromUtf8(value);
  }
  if (ReadJsonString(envelope, "calledNumber", &value)) {
    parsed.called_number = FromUtf8(value);
  }
  if (ReadJsonString(envelope, "originalCalledNumber", &value)) {
    parsed.original_called_number = FromUtf8(value);
  }
  ReadJsonUint64(envelope, "revision", &parsed.revision);
  ReadJsonBoolean(envelope, "removed", &parsed.removed);
  *event = std::move(parsed);
  return true;
}

BridgeReply Failure(const LONG result = static_cast<LONG>(kDefaultTapiFailure)) {
  BridgeReply reply;
  reply.tapi_result = result;
  return reply;
}

LONG BridgeFailureToTapi(const std::string_view response) {
  std::string code;
  if (!ReadJsonString(response, "code", &code)) {
    return static_cast<LONG>(LINEERR_OPERATIONFAILED);
  }
  if (code == "Unavailable") {
    return static_cast<LONG>(LINEERR_SERVICE_NOT_RUNNING);
  }
  if (code == "Timeout") {
    return static_cast<LONG>(LINEERR_OPERATIONFAILED);
  }
  if (code == "Rejected") {
    return static_cast<LONG>(LINEERR_OPERATIONFAILED);
  }
  return static_cast<LONG>(LINEERR_OPERATIONFAILED);
}

}  // namespace

NamedPipeBridgeClient::NamedPipeBridgeClient(std::wstring pipe_name,
                                             const DWORD timeout_ms)
    : pipe_name_(std::move(pipe_name)), timeout_ms_(timeout_ms) {}

BridgeReply NamedPipeBridgeClient::Send(const BridgeCommand& command) {
  // Do not wait for a pipe instance.  TAPI calls this from TAPISRV; the worker
  // must return a deterministic service-not-running result rather than stall
  // the Telephony service while the bridge is unavailable.
  ScopedHandle pipe(::CreateFileW(
      pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED, nullptr));
  if (pipe.get() == INVALID_HANDLE_VALUE) {
    return Failure(static_cast<LONG>(LINEERR_SERVICE_NOT_RUNNING));
  }

  DWORD read_mode = PIPE_READMODE_BYTE;
  if (!::SetNamedPipeHandleState(pipe.get(), &read_mode, nullptr, nullptr)) {
    return Failure(static_cast<LONG>(LINEERR_SERVICE_NOT_RUNNING));
  }

  static std::atomic_uint64_t next_sequence{1};
  const std::string hello = BuildHelloEnvelope(next_sequence.fetch_add(1));
  if (!WriteFrame(pipe.get(), hello, timeout_ms_)) {
    return Failure(static_cast<LONG>(LINEERR_SERVICE_NOT_RUNNING));
  }
  bool received_hello_ack = false;
  for (int messages_read = 0; messages_read != 16 && !received_hello_ack;
       ++messages_read) {
    std::string hello_ack;
    if (!ReadFrame(pipe.get(), &hello_ack, timeout_ms_)) {
      return Failure(static_cast<LONG>(LINEERR_SERVICE_NOT_RUNNING));
    }
    received_hello_ack = IsEnvelopeType(hello_ack, "HelloAck");
  }
  if (!received_hello_ack) {
    return Failure(static_cast<LONG>(LINEERR_SERVICE_NOT_RUNNING));
  }

  const std::string request =
      BuildCommandEnvelope(command, next_sequence.fetch_add(1));
  if (!WriteFrame(pipe.get(), request, timeout_ms_)) {
    return Failure(static_cast<LONG>(LINEERR_SERVICE_NOT_RUNNING));
  }

  // This synchronous exchange lives on the provider worker, never a TSPI
  // entry-point thread. Call-state notifications arrive on a separate,
  // persistent subscription owned by ProviderRuntime.
  for (int replies_read = 0; replies_read != 16; ++replies_read) {
    std::string response;
    if (!ReadFrame(pipe.get(), &response, timeout_ms_)) {
      return Failure(static_cast<LONG>(LINEERR_SERVICE_NOT_RUNNING));
    }
    if (IsEnvelopeType(response, "CommandAccepted") ||
        IsEnvelopeType(response, "CallChanged")) {
      continue;
    }
    if (!IsEnvelopeType(response, "CommandCompleted")) {
      return Failure();
    }

    bool succeeded = false;
    if (!ReadJsonBoolean(response, "succeeded", &succeeded)) {
      return Failure();
    }
    BridgeReply reply;
    reply.tapi_result = succeeded ? 0 : BridgeFailureToTapi(response);
    if (succeeded) {
      std::string call_id;
      std::string device_id;
      if (ReadJsonString(response, "callId", &call_id)) {
        reply.bridge_call_id = FromUtf8(call_id);
      }
      if (ReadJsonString(response, "deviceId", &device_id)) {
        reply.bridge_device_id = FromUtf8(device_id);
      }
    }
    return reply;
  }

  return Failure();
}

std::unique_ptr<IBridgePipeClient> CreateDefaultBridgeClient() {
  return std::make_unique<NamedPipeBridgeClient>();
}

NamedPipeBridgeSubscriber::NamedPipeBridgeSubscriber(std::wstring pipe_name,
                                                     const DWORD timeout_ms)
    : pipe_name_(std::move(pipe_name)), timeout_ms_(timeout_ms) {}

void NamedPipeBridgeSubscriber::Run(const std::stop_token stop_token,
                                    const BridgeEventCallback& on_event) {
  static std::atomic_uint64_t next_sequence{1'000'000};
  while (!stop_token.stop_requested()) {
    ScopedHandle pipe(::CreateFileW(
        pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    DWORD read_mode = PIPE_READMODE_BYTE;
    if (!::SetNamedPipeHandleState(pipe.get(), &read_mode, nullptr, nullptr)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    const std::string hello = BuildHelloEnvelope(next_sequence.fetch_add(1));
    if (!WriteFrame(pipe.get(), hello, timeout_ms_)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    bool hello_acknowledged = false;
    for (int attempts = 0; attempts != 16 && !stop_token.stop_requested();) {
      std::string response;
      const auto result = ReadFrameWithStatus(pipe.get(), &response, timeout_ms_);
      if (result == FrameReadResult::kTimedOut) {
        continue;
      }
      if (result != FrameReadResult::kComplete) {
        break;
      }
      ++attempts;
      hello_acknowledged = IsEnvelopeType(response, "HelloAck");
    }
    if (!hello_acknowledged || stop_token.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    const std::string subscribe =
        BuildSubscribeEnvelope(next_sequence.fetch_add(1));
    if (!WriteFrame(pipe.get(), subscribe, timeout_ms_)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    bool subscribed = false;
    for (int attempts = 0; attempts != 16 && !stop_token.stop_requested();) {
      std::string response;
      const auto result = ReadFrameWithStatus(pipe.get(), &response, timeout_ms_);
      if (result == FrameReadResult::kTimedOut) {
        continue;
      }
      if (result != FrameReadResult::kComplete) {
        break;
      }
      ++attempts;
      BridgeCallEvent event;
      if (ParseCallChanged(response, &event)) {
        on_event(event);
      }
      subscribed = IsEnvelopeType(response, "Subscribed");
    }
    if (!subscribed || stop_token.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    while (!stop_token.stop_requested()) {
      std::string response;
      const auto result = ReadFrameWithStatus(pipe.get(), &response, timeout_ms_);
      if (result == FrameReadResult::kTimedOut) {
        continue;
      }
      if (result != FrameReadResult::kComplete) {
        break;
      }

      BridgeCallEvent event;
      if (ParseCallChanged(response, &event)) {
        on_event(event);
      }
    }

    if (!stop_token.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }
}

std::unique_ptr<IBridgeEventSubscriber> CreateDefaultBridgeSubscriber() {
  return std::make_unique<NamedPipeBridgeSubscriber>();
}

}  // namespace zultys_ncall::tsp
