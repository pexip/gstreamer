/*
 * Copyright (c) 2023, Pexip AS
 *  @author: Tulio Beloqui <tulio@pexip.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "sctpsocket.h"

#include <iostream>
#include <memory>
#include <cstdio>

#include "net/dcsctp/public/dcsctp_options.h"
#include "net/dcsctp/socket/dcsctp_socket.h"
#include "rtc_base/logging.h"

static std::vector<uint16_t>
get_vector_from_streams (rtc::ArrayView<const dcsctp::StreamID> streams)
{
  std::vector<uint16_t> values;

  for (auto & streamID : streams) {
    values.push_back (static_cast<uint16_t> (streamID));
  }

  return values;
}

class SctpSocketTimeoutHandler : public dcsctp::Timeout
{
public:
  SctpSocketTimeoutHandler (webrtc::TaskQueueBase::DelayPrecision precision, SctpSocket_Callbacks callbacks)
    : callbacks_ (callbacks)
    , timeout_ (nullptr)
  {
    // TODO: use the precision
    (void)precision;
    timeout_ = callbacks_.timeout_create (callbacks_.user_data);
  }

  virtual ~SctpSocketTimeoutHandler () override
  {
    callbacks_.timeout_delete (callbacks_.user_data, timeout_);
  }

  virtual void
  Start (dcsctp::DurationMs duration, dcsctp::TimeoutID timeout_id) override
  {
    callbacks_.timeout_start (callbacks_.user_data, timeout_, duration.value (), timeout_id.value ());
  }

  virtual void
  Stop () override
  {
    callbacks_.timeout_stop (callbacks_.user_data, timeout_);
  }

private:
  SctpSocket_Callbacks callbacks_;
  void * timeout_;
};

class GstLogSink : public rtc::LogSink
{
public:
  GstLogSink (SctpSocket_LoggingFunction log_func)
    : log_func_ (log_func)
  {
  }

  virtual ~GstLogSink () override
  {
  }

  virtual void
  OnLogMessage (const std::string & message, rtc::LoggingSeverity severity) override
  {
    if (log_func_) {
      log_func_ (static_cast<SctpSocket_LoggingSeverity> (severity), message.c_str ());
    }
  }

  virtual void
  OnLogMessage (const std::string & message) override
  {
  }

private:
  SctpSocket_LoggingFunction log_func_;
};

class GstPacketObserver : public dcsctp::PacketObserver
{
public:
  GstPacketObserver (SctpSocket_PacketDumpFunction on_sent_packet, SctpSocket_PacketDumpFunction on_received_packet)
    : on_sent_packet_ (on_sent_packet)
    , on_received_packet_ (on_received_packet)
  {
  }

  virtual ~GstPacketObserver () override
  {
  }

  virtual void
  OnSentPacket (dcsctp::TimeMs now, rtc::ArrayView<const uint8_t> payload) override
  {
    on_sent_packet_ (static_cast<int64_t> (now), payload.data (), payload.size ());
  }

  virtual void
  OnReceivedPacket (dcsctp::TimeMs now, rtc::ArrayView<const uint8_t> payload) override
  {
    on_received_packet_ (static_cast<int64_t> (now), payload.data (), payload.size ());
  }

private:
  SctpSocket_PacketDumpFunction on_sent_packet_;
  SctpSocket_PacketDumpFunction on_received_packet_;
};

class SctpSocketCallbacksHandler : public dcsctp::DcSctpSocketCallbacks
{
public:
  SctpSocketCallbacksHandler (SctpSocket_Callbacks callbacks)
    : callbacks_ (callbacks)
  {
  }

  virtual ~SctpSocketCallbacksHandler () override
  {
  }

  /* DcSctpSocketCallbacks */

  virtual dcsctp::SendPacketStatus
  SendPacketWithStatus (rtc::ArrayView<const uint8_t> data) override
  {
    return (dcsctp::SendPacketStatus)callbacks_.send_packet (callbacks_.user_data, data.data (), data.size ());
  }

  virtual std::unique_ptr<dcsctp::Timeout>
  CreateTimeout (webrtc::TaskQueueBase::DelayPrecision precision) override
  {
    return std::make_unique<SctpSocketTimeoutHandler> (precision, callbacks_);
  }

  virtual dcsctp::TimeMs
  TimeMillis () override
  {
    uint32_t timems = callbacks_.time_millis (callbacks_.user_data);
    return dcsctp::TimeMs (timems);
  }

  virtual uint32_t
  GetRandomInt (uint32_t low, uint32_t high) override
  {
    return callbacks_.get_random_int (callbacks_.user_data, low, high);
  }

  virtual void
  OnMessageReceived (dcsctp::DcSctpMessage message) override
  {
    auto payload = std::move (message).ReleasePayload ();
    callbacks_.on_message_received (callbacks_.user_data, message.stream_id ().value (), message.ppid ().value (),
                                    payload.data (), payload.size ());
  }

  virtual void
  OnError (dcsctp::ErrorKind error, absl::string_view message) override
  {
    callbacks_.on_error (callbacks_.user_data, (SctpSocket_Error)error, message.data ());
  }

  virtual void
  OnAborted (dcsctp::ErrorKind error, absl::string_view message) override
  {
    callbacks_.on_aborted (callbacks_.user_data, (SctpSocket_Error)error, message.data ());
  }

  virtual void
  OnConnected () override
  {
    callbacks_.on_connected (callbacks_.user_data);
  }

  virtual void
  OnClosed () override
  {
    callbacks_.on_closed (callbacks_.user_data);
  }

  virtual void
  OnConnectionRestarted () override
  {
    callbacks_.on_connection_restarted (callbacks_.user_data);
  }

  virtual void
  OnStreamsResetFailed (rtc::ArrayView<const dcsctp::StreamID> outgoing_streams, absl::string_view reason) override
  {
    auto streams = get_vector_from_streams (outgoing_streams);
    callbacks_.on_streams_reset_failed (callbacks_.user_data, streams.data (), streams.size (), reason.data ());
  }

  virtual void
  OnStreamsResetPerformed (rtc::ArrayView<const dcsctp::StreamID> outgoing_streams) override
  {
    auto streams = get_vector_from_streams (outgoing_streams);
    callbacks_.on_streams_reset_performed (callbacks_.user_data, streams.data (), streams.size ());
  }

  virtual void
  OnIncomingStreamsReset (rtc::ArrayView<const dcsctp::StreamID> incoming_streams) override
  {
    auto streams = get_vector_from_streams (incoming_streams);
    callbacks_.on_incoming_streams_reset (callbacks_.user_data, streams.data (), streams.size ());
  }

  virtual void
  OnBufferedAmountLow (dcsctp::StreamID stream_id) override
  {
    callbacks_.on_buffered_amount_low (callbacks_.user_data, static_cast<uint16_t> (stream_id));
  }

  virtual void
  OnTotalBufferedAmountLow () override
  {
    callbacks_.on_total_buffered_amount_low (callbacks_.user_data);
  }

private:
  SctpSocket_Callbacks callbacks_;
};

struct _SctpSocket
{
  _SctpSocket (std::unique_ptr<dcsctp::DcSctpSocketInterface> socket,
               std::shared_ptr<SctpSocketCallbacksHandler> callbacksHandler)
    : socket_ (std::move (socket))
    , callbacksHandler_ (callbacksHandler)
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
  assert (opts);
  assert (callbacks);

  dcsctp::DcSctpOptions options;
  options.local_port = opts->local_port;
  options.remote_port = opts->remote_port;
  options.max_message_size = opts->max_message_size;

  if (opts->max_timer_backoff_duration_ms != -1) {
    options.max_timer_backoff_duration = absl::make_optional (dcsctp::DurationMs (opts->max_timer_backoff_duration_ms));
  } else {
    options.max_timer_backoff_duration = absl::nullopt;
  }

  if (opts->heartbeat_interval_ms != -1) {
    options.heartbeat_interval = dcsctp::DurationMs (opts->heartbeat_interval_ms);
  }

  if (opts->max_retransmissions != -1) {
    options.max_retransmissions = absl::make_optional (opts->max_retransmissions);
  } else {
    options.max_retransmissions = absl::nullopt;
  }

  if (opts->max_init_retransmits != -1) {
    options.max_init_retransmits = absl::make_optional (opts->max_init_retransmits);
  } else {
    options.max_init_retransmits = absl::nullopt;
  }

  std::unique_ptr<dcsctp::PacketObserver> packet_observer;
  std::shared_ptr<SctpSocketCallbacksHandler> callbacksHandler =
    std::make_shared<SctpSocketCallbacksHandler> (*callbacks);

  if (callbacks->on_sent_packet && callbacks->on_received_packet) {
    packet_observer = std::make_unique<GstPacketObserver> (callbacks->on_sent_packet, callbacks->on_received_packet);
  }

  char name[32];
  std::snprintf (name, sizeof(name), "SctpSocket assoc: %p", callbacks->user_data);

  auto socket =
    std::make_unique<dcsctp::DcSctpSocket> (name, *callbacksHandler, std::move (packet_observer), options);
  return new _SctpSocket (std::move (socket), callbacksHandler);
}

void
sctp_socket_free (SctpSocket * socket)
{
  delete socket;
}

void
sctp_socket_receive_packet (SctpSocket * socket, uint8_t * data, size_t len)
{
  assert (socket);
  assert (socket->socket_);

  rtc::ArrayView<uint8_t> packet (data, len);
  socket->socket_->ReceivePacket (packet);
}

void
sctp_socket_handle_timeout (SctpSocket * socket, uint64_t timeout_id)
{
  assert (socket);
  assert (socket->socket_);
  socket->socket_->HandleTimeout (dcsctp::TimeoutID (timeout_id));
}

void
sctp_socket_connect (SctpSocket * socket)
{
  assert (socket);
  assert (socket->socket_);
  socket->socket_->Connect ();
}

void
sctp_socket_shutdown (SctpSocket * socket)
{
  assert (socket);
  assert (socket->socket_);
  socket->socket_->Shutdown ();
}

void
sctp_socket_close (SctpSocket * socket)
{
  assert (socket);
  assert (socket->socket_);
  socket->socket_->Close ();
}

SctpSocket_State
sctp_socket_state (SctpSocket * socket)
{
  assert (socket);
  assert (socket->socket_);

  dcsctp::SocketState state = socket->socket_->state ();
  return static_cast<SctpSocket_State> (state);
}

SctpSocket_SendStatus
sctp_socket_send (SctpSocket * socket, const uint8_t * data, size_t len, uint16_t stream_id, uint32_t ppid,
                  bool unordered, int32_t * lifetime, size_t * max_retransmissions)
{
  assert (socket);
  assert (socket->socket_);

  auto max_message_size = socket->socket_->options ().max_message_size;
  if (max_message_size > 0 && len > max_message_size) {
    return SCTP_SOCKET_STATUS_MESSAGE_TOO_LARGE;
  }

  std::vector<uint8_t> message_payload;
  if (len > 0 && data != nullptr) {
    message_payload.assign(data, data + len);
  } else {
    // https://www.rfc-editor.org/rfc/rfc8831.html#section-6.6
    // SCTP does not support the sending of empty user messages. Therefore, if
    // an empty message has to be sent, the appropriate PPID (WebRTC String
    // Empty or WebRTC Binary Empty) is used, and the SCTP user message of one
    // zero byte is sent.
    message_payload.push_back ('\0');
  }

  dcsctp::DcSctpMessage message (dcsctp::StreamID (stream_id), dcsctp::PPID (ppid), std::move (message_payload));

  dcsctp::SendOptions send_options;
  send_options.unordered = dcsctp::IsUnordered (unordered);
  if (lifetime) {
    assert (*lifetime >= 0 && *lifetime <= std::numeric_limits<uint16_t>::max ());
    send_options.lifetime = dcsctp::DurationMs (*lifetime);
  }
  if (max_retransmissions) {
    assert (*max_retransmissions >= 0 && *max_retransmissions <= std::numeric_limits<uint16_t>::max ());
    send_options.max_retransmissions = *max_retransmissions;
  }

  auto status = socket->socket_->Send (std::move (message), send_options);
  return static_cast<SctpSocket_SendStatus> (status);
}

void
sctp_socket_send_abort (SctpSocket * socket, const char *message)
{
  assert (socket);
  assert (socket->socket_);
  socket->socket_->SendAbort (message);
}

SctpSocket_ResetStreamStatus
sctp_socket_reset_streams (SctpSocket * socket, const uint16_t * streams, size_t len)
{
  assert (socket);
  assert (socket->socket_);

  std::vector<dcsctp::StreamID> reset_streams;
  for (size_t i = 0; i < len; i++)
    reset_streams.push_back (dcsctp::StreamID (streams[i]));

  auto status = socket->socket_->ResetStreams (reset_streams);
  return static_cast<SctpSocket_ResetStreamStatus> (status);
}

void
sctp_socket_register_logging_function (SctpSocket_LoggingFunction log_func)
{
  static std::shared_ptr<GstLogSink> gst_log_sink_ = std::make_shared<GstLogSink> (log_func);
  rtc::LogMessage::SetLogToStderr (false);
  rtc::LogMessage::AddLogToStream (gst_log_sink_.get (), rtc::LS_VERBOSE);
}
