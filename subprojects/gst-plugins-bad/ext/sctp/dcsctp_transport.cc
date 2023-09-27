#include "dcsctp_transport.h"

#include <memory>

#include "net/dcsctp/public/dcsctp_socket.h"

struct _DCSctpTransport
{
  _DCSctpTransport (DCSctpTransport_Callbacks * callbacks):socket_ (nullptr),
      callbacks_ (callbacks)
  {

  }

   ~_DCSctpTransport ()
  {

  }

  std::unique_ptr < dcsctp::DcSctpSocketInterface > socket_;
  DCSctpTransport_Callbacks *callbacks_;


};

DCSctpTransport *
dcsctp_transport_new (DCSctpTransport_Callbacks * callbacks)
{
  return new _DCSctpTransport (callbacks);
}

void
dcsctp_transport_free (DCSctpTransport * s)
{
  delete s;
}


bool
dcsctp_transport_start (DCSctpTransport * transport, int local_sctp_port,
    int remote_sctp_port, int max_message_size)
{
  (void) transport;
  (void) local_sctp_port;
  (void) remote_sctp_port;
  (void) max_message_size;
  // if (!socket_) {
  //   dcsctp::DcSctpOptions options;
  //   options.local_port = local_sctp_port;
  //   options.remote_port = remote_sctp_port;
  //   options.max_message_size = max_message_size;
  //   options.max_timer_backoff_duration = kMaxTimerBackoffDuration;
  //   // Don't close the connection automatically on too many retransmissions.
  //   options.max_retransmissions = absl::nullopt;
  //   options.max_init_retransmits = absl::nullopt;

  //   std::unique_ptr < dcsctp::PacketObserver > packet_observer;
  //   if (RTC_LOG_CHECK_LEVEL (LS_VERBOSE)) {
  //     packet_observer =
  //         std::make_unique < dcsctp::TextPcapPacketObserver > (debug_name_);
  //   }

  //   socket_ = socket_factory_->Create (debug_name_, *this,
  //       std::move (packet_observer), options);
  // } else {
  //   if (local_sctp_port != socket_->options ().local_port ||
  //       remote_sctp_port != socket_->options ().remote_port) {
  //     // RTC_LOG (LS_ERROR)
  //     //     << debug_name_ << "->Start(local=" << local_sctp_port
  //     //     << ", remote=" << remote_sctp_port
  //     //     << "): Can't change ports on already started transport.";
  //     return false;
  //   }
  //   socket_->SetMaxMessageSize (max_message_size);
  // }

  // MaybeConnectSocket ();

  return true;
}

bool
dcsctp_transport_open_stream (DCSctpTransport * transport, int sid)
{
  (void) transport;
  (void) sid;

  return true;
}

bool
dcsctp_transport_reset_stream (DCSctpTransport * transport, int sid)
{

  (void) transport;
  (void) sid;
  return true;
}

DCSctpError
dcsctp_transport_send_data (DCSctpTransport * transport,
    DCSctp_DataMessageType msg_type, bool ordered, int max_rtx_count,
    int max_rtx_ms, void *data, size_t len)
{
  (void) transport;
  (void) msg_type;
  (void) ordered;
  (void) max_rtx_ms;
  (void) max_rtx_count;
  (void) data;
  (void) len;
  return DCSCTP_SUCCESS;

}
