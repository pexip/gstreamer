#ifndef _SCTP_SOCKET_H_
#define _SCTP_SOCKET_H_

#include <stddef.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct _SctpSocket SctpSocket;
typedef struct _SctpSocket_Callbacks SctpSocket_Callbacks; 

typedef enum
{
  // The socket is closed.
  SCTP_SOCKET_STATE_CLOSED,
  // The socket has initiated a connection, which is not yet established. Note
  // that for incoming connections and for reconnections when the socket is
  // already connected, the socket will not transition to this state.
  SCTP_SOCKET_STATE_CONNECTING,
  // The socket is connected, and the connection is established.
  SCTP_SOCKET_STATE_CONNECTED,
  // The socket is shutting down, and the connection is not yet closed.
  SCTP_SOCKET_STATE_SHUTTING_DOWN,

  __SCTP_SOCKET_STATE_MAX__
} SctpSocket_State;

typedef enum
{
  // Indicates that no error has occurred. This will never be the case when
  // `OnError` or `OnAborted` is called.
  SCTP_SOCKET_SUCCESS = 0,

  // There have been too many retries or timeouts, and the library has given up.
  SCTP_SOCKET_ERROR_TOO_MANY_RETRIES,
  // A command was received that is only possible to execute when the socket is
  // connected, which it is not.
  SCTP_SOCKET_ERROR_NOT_CONNECTED,
  // Parsing of the command or its parameters failed.
  SCTP_SOCKET_ERROR_PARSE_FAILED,
  // Commands are received in the wrong sequence, which indicates a
  // synchronisation mismatch between the peers.
  SCTP_SOCKET_ERROR_WRONG_SEQUENCE,
  // The peer has reported an issue using ERROR or ABORT command.
  SCTP_SOCKET_ERROR_PEER_REPORTED,
  // The peer has performed a protocol violation.
  SCTP_SOCKET_ERROR_PROTOCOL_VIOLATION,
  // The receive or send buffers have been exhausted.
  SCTP_SOCKET_ERROR_RESOURCE_EXHAUSTION,
  // The client has performed an invalid operation.
  SCTP_SOCKET_ERROR_UNSUPPORTED_OPERATION,

  __SCTP_SOCKET_ERROR_MAX__
} SctpSocket_Error;

typedef enum
{
  // The message was enqueued successfully. As sending the message is done
  // asynchronously, this is no guarantee that the message has been actually
  // sent.
  SCTP_SOCKET_STATUS_SUCCESS = 0,
  // The message was rejected as the payload was empty (which is not allowed in
  // SCTP).
  SCTP_SOCKET_STATUS_MESSAGE_EMPTY,
  // The message was rejected as the payload was larger than what has been set
  // as `DcSctpOptions.max_message_size`.
  SCTP_SOCKET_STATUS_MESSAGE_TOO_LARGE,
  // The message could not be enqueued as the socket is out of resources. This
  // mainly indicates that the send queue is full.
  SCTP_SOCKET_STATUS_ERROR_RESOURCE_EXHAUSTION,
  // The message could not be sent as the socket is shutting down.
  SCTP_SOCKET_STATUS_ERROR_SHUTTING_DOWN,
  __SCTP_SOCKET_SEND_STATUS_MAX__
} SctpSocket_SendStatus;

typedef enum
{
  // If the connection is not yet established, this will be returned.
  SCTP_SOCKET_RESET_STREAM_STATUS_NOT_CONNECTED = 0,
  // Indicates that ResetStreams operation has been successfully initiated.
  SCTP_SOCKET_RESET_STREAM_STATUS_PERFORMED,
  // Indicates that ResetStreams has failed as it's not supported by the peer.
  SCTP_SOCKET_RESET_STREAM_STATUS_NOT_SUPPORTED,

} SctpSocket_ResetStreamStatus;

typedef enum
{
  // Indicates that the packet was successfully sent. As sending is unreliable,
  // there are no guarantees that the packet was actually delivered.
  SCTP_SOCKET_SEND_PACKET_STATUS_SUCCESS = 0,
  // The packet was not sent due to a temporary failure, such as the local send
  // buffer becoming exhausted. This return value indicates that the socket will
  // recover and sending that packet can be retried at a later time.
  SCTP_SOCKET_SEND_PACKET_STATUS_TEMPORARY_FAILURE,
  // The packet was not sent due to other reasons.
  SCTP_SOCKET_SEND_PACKET_STATUS_ERROR,

} SctpSocket_SendPacketStatus;

// typedef enum
// {
// 	DCSCTP_DATA_MSG_TYPE_TEXT,
// 	DCSCTP_DATA_MSG_TYPE_BIN,
// 	DCSCTP_DATA_MSG_TYPE_CTRL,
// } DCSctp_DataMessageType;

// typedef void (*SctpSocket_DataReceivedCallback)(void * user_data, int channel_id, DCSctp_DataMessageType type, const uint8_t * data, size_t len);
// typedef void (*SctpSocket_ChannelClosingCallback)(void * user_data, int channel_id);
// typedef void (*SctpSocket_ChannelClosedCallback)(void * user_data, int channel_id);
// typedef void (*SctpSocket_ReadyToSendCallback)(void * user_data);
// typedef void (*SctpSocket_TransportClosedCallback)(void * user_data, SctpSocket_Error error);

typedef SctpSocket_SendPacketStatus (*SctpSocket_SendPacket) (void * user_data, const uint8_t * data, size_t len); 
typedef void (*SctpSocket_OnMessageReceived) (void * user_data, uint16_t stream_id, uint32_t ppid, const uint8_t * data, size_t len);
typedef void (*SctpSocket_OnError) (void * user_data, SctpSocket_Error error);
typedef void (*SctpSocket_OnAborted) (void * user_data, SctpSocket_Error error);
typedef void (*SctpSocket_OnConnected) (void * user_data);
typedef void (*SctpSocket_OnClosed) (void * user_data);
typedef void (*SctpSocket_OnConnectionRestarted) (void * user_data);

typedef void (*SctpSocket_OnStreamsResetFailed) (void * user_data, const uint16_t * streams, size_t len);
typedef void (*SctpSocket_OnStreamsResetPerformed) (void * user_data, const uint16_t * streams, size_t len);
typedef void (*SctpSocket_OnIncomingStreamsReset) (void * user_data, const uint16_t * streams, size_t len);
typedef void (*SctpSocket_OnBufferedAmountLow) (void * user_data, uint16_t stream_id);
typedef void (*SctpSocket_OnTotalBufferedAmountLow) (void * user_data);

struct _SctpSocket_Callbacks
{
  SctpSocket_SendPacket send_packet;
  SctpSocket_OnMessageReceived on_message_received;

  SctpSocket_OnError on_error;
  SctpSocket_OnAborted on_aborted;
  SctpSocket_OnConnected on_connected;
  SctpSocket_OnClosed on_closed;
  SctpSocket_OnConnectionRestarted on_connection_restarted;

  SctpSocket_OnStreamsResetFailed on_streams_reset_failed;
  SctpSocket_OnStreamsResetPerformed on_streams_reset_performed;
  SctpSocket_OnIncomingStreamsReset on_incoming_streams_reset;
  SctpSocket_OnBufferedAmountLow on_buffered_amount_low;
  SctpSocket_OnTotalBufferedAmountLow on_total_buffered_amount_low;

  void * user_data;
};

SctpSocket * sctp_socket_new (SctpSocket_Callbacks * callbacks);
 
void sctp_socket_free (SctpSocket * socket);

// bool sctp_socket_start (SctpSocket * socket, int local_sctp_port, int remote_sctp_port, int max_message_size);

// To be called when an incoming SCTP packet is to be processed.
void sctp_socket_receive_packet (SctpSocket * socket, const uint8_t * data, size_t len);

// Connects the socket. This is an asynchronous operation, and
// `DcSctpSocketCallbacks::OnConnected` will be called on success.
void sctp_socket_connect (SctpSocket * socket);

// Gracefully shutdowns the socket and sends all outstanding data. This is an
// asynchronous operation and `DcSctpSocketCallbacks::OnClosed` will be called
// on success.
void sctp_socket_shutdown (SctpSocket * socket);

// Closes the connection non-gracefully. Will send ABORT if the connection is
// not already closed. No callbacks will be made after Close() has returned.
void sctp_socket_close (SctpSocket * socket);

// The socket state.
SctpSocket_State sctp_socket_state (SctpSocket * socket);

// Sends the message `message` using the provided send options.
// Sending a message is an asynchronous operation, and the `OnError` callback
// may be invoked to indicate any errors in sending the message.
//
// The association does not have to be established before calling this method.
// If it's called before there is an established association, the message will
// be queued.
SctpSocket_SendStatus sctp_socket_send (SctpSocket * socket, uint16_t stream_id, uint32_t ppid, const uint8_t * data, size_t len);

// Resetting streams is an asynchronous operation and the results will
// be notified using `DcSctpSocketCallbacks::OnStreamsResetDone()` on success
// and `DcSctpSocketCallbacks::OnStreamsResetFailed()` on failure. Note that
// only outgoing streams can be reset.
//
// When it's known that the peer has reset its own outgoing streams,
// `DcSctpSocketCallbacks::OnIncomingStreamReset` is called.
//
// Note that resetting a stream will also remove all queued messages on those
// streams, but will ensure that the currently sent message (if any) is fully
// sent before closing the stream.
//
// Resetting streams can only be done on an established association that
// supports stream resetting. Calling this method on e.g. a closed association
// or streams that don't support resetting will not perform any operation.
SctpSocket_ResetStreamStatus sctp_socket_reset_streams (SctpSocket * socket, const uint16_t * streams, size_t len);


#ifdef __cplusplus
}
#endif

#endif // _SCTP_SOCKET_H_