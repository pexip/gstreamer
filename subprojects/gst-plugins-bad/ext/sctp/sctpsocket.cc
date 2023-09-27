#include "sctpsocket.h"

#include <memory>

#include "net/dcsctp/public/dcsctp_socket.h"

struct _SctpSocket
{
  _SctpSocket (SctpSocket_Callbacks * callbacks):socket_ (nullptr),
      callbacks_ (callbacks)
  {

  }

   ~_SctpSocket ()
  {

  }

  std::unique_ptr < dcsctp::DcSctpSocketInterface > socket_;
  SctpSocket_Callbacks *callbacks_;
};

SctpSocket *
sctp_socket_new (SctpSocket_Callbacks * callbacks)
{
  return new _SctpSocket (callbacks);
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
    uint16_t stream_id, uint32_t ppid, bool ordered, int32_t * lifetime,
    size_t *max_retransmissions)
{
  (void) socket;
  (void) stream_id;
  (void) ppid;
  (void) data;
  (void) len;
  (void) ordered;
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
