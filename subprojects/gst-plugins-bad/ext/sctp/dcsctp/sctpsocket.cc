#include "sctpsocket.h"

#include <memory>
#include <iostream>

#include "rtc_base/logging.h"
#include "net/dcsctp/public/dcsctp_options.h"
#include "net/dcsctp/socket/dcsctp_socket.h"

class SctpSocketTimeoutHandler : public dcsctp::Timeout
{
public:
  SctpSocketTimeoutHandler(webrtc::TaskQueueBase::DelayPrecision precision, SctpSocket_Callbacks callbacks):
    callbacks_(callbacks),
    timeout_(nullptr)
  {
    // TODO: use the precision
    (void) precision;
    timeout_ = callbacks_.timeout_create (callbacks_.user_data);
  }

  virtual ~SctpSocketTimeoutHandler () override
  {
    callbacks_.timeout_delete (callbacks_.user_data, timeout_);
  }

  virtual void Start(dcsctp::DurationMs duration, dcsctp::TimeoutID timeout_id) override
  {
    // std::cout << " TIMEOUT_ID " << timeout_id.value()  << "    " << timeout_id << std::endl;
    callbacks_.timeout_start (callbacks_.user_data, timeout_, duration.value(), timeout_id.value());
  }

  virtual void Stop() override
  {
    callbacks_.timeout_stop (callbacks_.user_data, timeout_); 
  }

private:
  SctpSocket_Callbacks callbacks_;
  void * timeout_;
};

class SctpSocketCallbacksHandler :  public dcsctp::DcSctpSocketCallbacks, public rtc::LogSink
{
public:
  SctpSocketCallbacksHandler(SctpSocket_Callbacks callbacks): callbacks_(callbacks)
  {
    rtc::LogMessage::SetLogToStderr(false);
    rtc::LogMessage::AddLogToStream(this, rtc::LS_VERBOSE);
  }

  virtual ~SctpSocketCallbacksHandler () override
  {
    rtc::LogMessage::RemoveLogToStream(this);
  }

  /* DcSctpSocketCallbacks */

  virtual dcsctp::SendPacketStatus SendPacketWithStatus (rtc::ArrayView <
      const uint8_t > data) override
  {
    return (dcsctp::SendPacketStatus)callbacks_.send_packet(callbacks_.user_data, data.data(), data.size());
  }

  virtual std::unique_ptr < dcsctp::Timeout >
      CreateTimeout (webrtc::TaskQueueBase::DelayPrecision precision) override
  {
    return std::make_unique<SctpSocketTimeoutHandler> (precision, callbacks_);
  }

  virtual dcsctp::TimeMs TimeMillis () override
  {
    uint32_t timems = callbacks_.time_millis (callbacks_.user_data);
    return static_cast<dcsctp::TimeMs>(timems);
  }

  virtual uint32_t GetRandomInt (uint32_t low, uint32_t high) override
  {
    return callbacks_.get_random_int(callbacks_.user_data, low, high);
  }

  virtual void OnMessageReceived (dcsctp::DcSctpMessage message) override
  {
    auto payload = std::move(message).ReleasePayload();
    callbacks_.on_message_received(callbacks_.user_data, message.stream_id().value(), message.ppid().value(), payload.data(), payload.size());
  }

  virtual void OnError (dcsctp::ErrorKind error, absl::string_view message) override
  {
    callbacks_.on_error(callbacks_.user_data, (SctpSocket_Error) error, message.data());
  }

  virtual void OnAborted (dcsctp::ErrorKind error, absl::string_view message) override
  {
    callbacks_.on_aborted(callbacks_.user_data, (SctpSocket_Error) error, message.data());
  }

  virtual void OnConnected () override
  {
    callbacks_.on_connected(callbacks_.user_data);
  }

  virtual void OnClosed () override
  {
    callbacks_.on_closed(callbacks_.user_data);
  }

  virtual void OnConnectionRestarted () override
  {
    callbacks_.on_connection_restarted(callbacks_.user_data);
  }

  virtual void OnStreamsResetFailed (rtc::ArrayView < const dcsctp::StreamID >
      outgoing_streams, absl::string_view reason) override
  {

  }

  virtual void OnStreamsResetPerformed (rtc::ArrayView < const dcsctp::StreamID >
      outgoing_streams) override
  {

  }

  virtual void OnIncomingStreamsReset (rtc::ArrayView < const dcsctp::StreamID >
      incoming_streams)
  {

  }

  virtual void OnBufferedAmountLow (dcsctp::StreamID stream_id) override
  {
  }

  virtual void OnTotalBufferedAmountLow () override
  {
  }

  /* LogSink */
  virtual void OnLogMessage(const std::string& message,
                            rtc::LoggingSeverity severity)
  {
   if (callbacks_.log_message) {
      callbacks_.log_message(callbacks_.user_data, static_cast<SctpSocket_LoggingSeverity>(severity), message.c_str());
    }
  }

  virtual void OnLogMessage(const std::string& message) override
  {
  }

private:
  SctpSocket_Callbacks callbacks_;
};

struct _SctpSocket
{
  _SctpSocket (std::unique_ptr <dcsctp::DcSctpSocketInterface> socket, std::shared_ptr<SctpSocketCallbacksHandler> callbacksHandler):
  socket_(std::move(socket)),
  callbacksHandler_(callbacksHandler)
  {
  }

   ~_SctpSocket ()
  {
  }

  std::unique_ptr<dcsctp::DcSctpSocketInterface> socket_;
  std::shared_ptr<SctpSocketCallbacksHandler> callbacksHandler_;
};

SctpSocket *
sctp_socket_new (SctpSocket_Options * opts, SctpSocket_Callbacks * callbacks)
{
  dcsctp::DcSctpOptions options;
  options.local_port = opts->local_port;
  options.remote_port = opts->remote_port;
  options.max_message_size = opts->max_message_size;

  options.max_timer_backoff_duration = dcsctp::DurationMs(opts->max_timer_backoff_duration_ms);
  options.max_retransmissions = opts->max_retransmissions ?
    absl::make_optional(*opts->max_retransmissions) : absl::nullopt;
  options.max_init_retransmits = opts->max_init_retransmits ?
    absl::make_optional(*opts->max_init_retransmits) : absl::nullopt;

  std::unique_ptr < dcsctp::PacketObserver > packet_observer;
  std::shared_ptr<SctpSocketCallbacksHandler> callbacksHandler = std::make_shared<SctpSocketCallbacksHandler>(*callbacks);

  // if (RTC_LOG_CHECK_LEVEL (LS_VERBOSE)) {
  //   packet_observer =
  //       std::make_unique < dcsctp::TextPcapPacketObserver > (debug_name_);
  // }

  auto socket = std::make_unique<dcsctp::DcSctpSocket>("SctpSocket", *callbacksHandler, nullptr, options);
  return new _SctpSocket (std::move(socket), callbacksHandler);
}

void
sctp_socket_free (SctpSocket * socket)
{
  delete socket;
}

void
sctp_socket_receive_packet (SctpSocket * socket, const uint8_t * data,
    size_t len)
{
  assert(socket);
  assert(socket->socket_);

  // FIXME: review this extra copy, is it necessary ?
  std::vector<uint8_t> packet(data, data +len);
  socket->socket_->ReceivePacket(packet);
}

void sctp_socket_handle_timeout (SctpSocket * socket, uint64_t timeout_id)
{
  assert(socket);
  assert(socket->socket_);
  socket->socket_->HandleTimeout (dcsctp::TimeoutID(timeout_id));
}

void
sctp_socket_connect (SctpSocket * socket)
{
  assert(socket);
  assert(socket->socket_);
  socket->socket_->Connect();
}

void
sctp_socket_shutdown (SctpSocket * socket)
{
  assert(socket);
  assert(socket->socket_);
  socket->socket_->Shutdown();
}

void
sctp_socket_close (SctpSocket * socket)
{
  assert(socket);
  assert(socket->socket_);
  socket->socket_->Close();
}

SctpSocket_State
sctp_socket_state (SctpSocket * socket)
{
  assert(socket);
  assert(socket->socket_);

  dcsctp::SocketState state = socket->socket_->state(); 
  return static_cast<SctpSocket_State>(state);
}

SctpSocket_SendStatus
sctp_socket_send (SctpSocket * socket, const uint8_t * data, size_t len,
    uint16_t stream_id, uint32_t ppid, bool unordered, int32_t * lifetime,
    size_t *max_retransmissions)
{
  assert(socket);
  assert(socket->socket_);

  auto max_message_size = socket->socket_->options().max_message_size;
  if (max_message_size > 0 && len > max_message_size) {
    // RTC_LOG(LS_WARNING) << debug_name_
    //                     << "->SendData(...): "
    //                        "Trying to send packet bigger "
    //                        "than the max message size: "
    //                     << payload.size() << " vs max of " << max_message_size;
    // return RTCError(RTCErrorType::INVALID_RANGE);
    return SCTP_SOCKET_STATUS_MESSAGE_TOO_LARGE;
  }

  std::vector<uint8_t> message_payload(data,
                                       data +len);
  if (message_payload.empty()) {
    // https://www.rfc-editor.org/rfc/rfc8831.html#section-6.6
    // SCTP does not support the sending of empty user messages. Therefore, if
    // an empty message has to be sent, the appropriate PPID (WebRTC String
    // Empty or WebRTC Binary Empty) is used, and the SCTP user message of one
    // zero byte is sent.
    message_payload.push_back('\0');
  }

  dcsctp::DcSctpMessage message(
      dcsctp::StreamID(stream_id),
      dcsctp::PPID(ppid),
      std::move(message_payload));

  dcsctp::SendOptions send_options;
  send_options.unordered = dcsctp::IsUnordered(unordered);
  if (lifetime) {
    assert(*lifetime >= 0 &&
             *lifetime <= std::numeric_limits<uint16_t>::max());
    send_options.lifetime = dcsctp::DurationMs(*lifetime);
  }
  if (max_retransmissions) {
    assert(*max_retransmissions >= 0 &&
            *max_retransmissions <= std::numeric_limits<uint16_t>::max());
    send_options.max_retransmissions = *max_retransmissions;
  }

  auto status = socket->socket_->Send(std::move(message), send_options);
  return static_cast<SctpSocket_SendStatus>(status);
}

SctpSocket_ResetStreamStatus
sctp_socket_reset_streams (SctpSocket * socket,
    const uint16_t * streams, size_t len)
{
  assert(socket);
  assert(socket->socket_);

  std::vector<dcsctp::StreamID> reset_streams;
  for (size_t i = 0 ; i < len; i++)
    reset_streams.push_back(dcsctp::StreamID(streams[i]));

  auto status = socket->socket_->ResetStreams(reset_streams);
  return static_cast<SctpSocket_ResetStreamStatus>(status);
}
