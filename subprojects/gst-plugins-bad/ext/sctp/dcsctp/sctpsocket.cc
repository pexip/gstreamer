#include "sctpsocket.h"

#include <memory>

#include "net/dcsctp/public/dcsctp_options.h"
#include "net/dcsctp/socket/dcsctp_socket.h"
// #include "net/dcsctp/public/dcsctp_socket.h"

// When there is packet loss for a long time, the SCTP retry timers will use
// exponential backoff, which can grow to very long durations and when the
// connection recovers, it may take a long time to reach the new backoff
// duration. By limiting it to a reasonable limit, the time to recover reduces.
constexpr dcsctp::DurationMs kMaxTimerBackoffDuration = dcsctp::DurationMs (3000);

class SctpSocketCallbacksHandler : public dcsctp::DcSctpSocketCallbacks
{
public:
  SctpSocketCallbacksHandler(SctpSocket_Callbacks * callbacks): callbacks_(callbacks)
  {
  }

  virtual ~SctpSocketCallbacksHandler () override
  {
    callbacks_ = nullptr;
  }

  virtual dcsctp::SendPacketStatus SendPacketWithStatus (rtc::ArrayView <
      const uint8_t > data) override
  {
    return (dcsctp::SendPacketStatus)callbacks_->send_packet(callbacks_->user_data, data.data(), data.size());
  }

  virtual std::unique_ptr < dcsctp::Timeout >
      CreateTimeout (webrtc::TaskQueueBase::DelayPrecision precision) override
  {
    // return std::make_unique<SctpSocketTimeout> (precision);
    return nullptr;
  }

  virtual dcsctp::TimeMs TimeMillis () override
  {
    return static_cast<dcsctp::TimeMs>(0);
    // return static_cast<TimeMs>(g_get_monotonic_time ());
  }

  virtual uint32_t GetRandomInt (uint32_t low, uint32_t high) override
  {
    return 0;
    // return (uint32_t) g_random_int((gint32) low, (gint32) high); 
  }

  virtual void OnMessageReceived (dcsctp::DcSctpMessage message) override
  {
    auto payload = std::move(message).ReleasePayload();
    callbacks_->on_message_received(callbacks_->user_data, static_cast<uint16_t>(message.stream_id()), static_cast<uint32_t>(message.ppid()), payload.data(), payload.size());
  }

  virtual void OnError (dcsctp::ErrorKind error, absl::string_view message) override
  {
    callbacks_->on_error(callbacks_->user_data, (SctpSocket_Error) error);
  }

  virtual void OnAborted (dcsctp::ErrorKind error, absl::string_view message) override
  {
    callbacks_->on_aborted(callbacks_->user_data, (SctpSocket_Error) error);
  }

  virtual void OnConnected () override
  {
    callbacks_->on_connected(callbacks_->user_data);
  }

  virtual void OnClosed () override
  {
    callbacks_->on_closed(callbacks_->user_data);
  }

  virtual void OnConnectionRestarted () override
  {
    callbacks_->on_connection_restarted(callbacks_->user_data);
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

private:
  SctpSocket_Callbacks * callbacks_;
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
sctp_socket_new (int local_sctp_port, int remote_sctp_port,
    int max_message_size, SctpSocket_Callbacks * callbacks)
{
  dcsctp::DcSctpOptions options;
  options.local_port = local_sctp_port;
  options.remote_port = remote_sctp_port;
  options.max_message_size = max_message_size;
  options.max_timer_backoff_duration = kMaxTimerBackoffDuration;
  // Don't close the connection automatically on too many retransmissions.
  options.max_retransmissions = absl::nullopt;
  options.max_init_retransmits = absl::nullopt;

  std::unique_ptr < dcsctp::PacketObserver > packet_observer;
  std::shared_ptr<SctpSocketCallbacksHandler> callbacksHandler = std::make_shared<SctpSocketCallbacksHandler>(callbacks);


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
  (void) socket;
  (void) data;
  (void) len;
}

void
sctp_socket_connect (SctpSocket * socket)
{
  (void) socket;
}

void
sctp_socket_shutdown (SctpSocket * socket)
{
  (void) socket;

}

void
sctp_socket_close (SctpSocket * socket)
{
  (void) socket;

}

SctpSocket_State
sctp_socket_state (SctpSocket * socket)
{
  (void) socket;
  return SCTP_SOCKET_STATE_CLOSED;
}

SctpSocket_SendStatus
sctp_socket_send (SctpSocket * socket, const uint8_t * data, size_t len,
    uint16_t stream_id, uint32_t ppid, bool unordered, int32_t * lifetime,
    size_t *max_retransmissions)
{
  (void) socket;
  (void) stream_id;
  (void) ppid;
  (void) data;
  (void) len;
  (void) unordered;
  (void) lifetime;
  (void) max_retransmissions;
  return SCTP_SOCKET_STATUS_SUCCESS;
}

SctpSocket_ResetStreamStatus
sctp_socket_reset_streams (SctpSocket * socket,
    const uint16_t * streams, size_t len)
{
  (void) socket;
  (void) streams;
  (void) len;

  return SCTP_SOCKET_RESET_STREAM_STATUS_NOT_CONNECTED;
}
