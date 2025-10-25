/*
 * Copyright (c) 2023, Pexip AS
 *  @author: Tulio Beloqui <tulio@pexip.com>
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

#ifndef _SCTP_SOCKET_H_
#define _SCTP_SOCKET_H_

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct _SctpSocket SctpSocket;
  typedef struct _SctpSocket_Callbacks SctpSocket_Callbacks;

  //////////////////////////////////////////////////////////////////////
  // The meanings of the levels are:
  //  SCTP_SOCKET_VERBOSE: This level is for data which we do not want to appear in the
  //   normal debug log, but should appear in diagnostic logs.
  //  SCTP_SOCKET_INFO: Chatty level used in debugging for all sorts of things, the default
  //   in debug builds.
  //  SCTP_SOCKET_WARNING: Something that may warrant investigation.
  //  SCTP_SOCKET_ERROR: Something that should not have occurred.
  //  SCTP_SOCKET_NONE: Don't log.
  typedef enum
  {
    SCTP_SOCKET_VERBOSE,
    SCTP_SOCKET_INFO,
    SCTP_SOCKET_WARNING,
    SCTP_SOCKET_ERROR,
    SCTP_SOCKET_NONE,
  } SctpSocket_LoggingSeverity;

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

  typedef enum
  {
    DELAY_PRECISION_LOW,
    DELAY_PRECISION_HIGH,
  } SctpSocket_DelayPrecision;

  typedef struct
  {
    int local_port;
    int remote_port;
    size_t max_message_size;

    int32_t max_timer_backoff_duration_ms;
    int32_t heartbeat_interval_ms;

    int max_retransmissions;
    int max_init_retransmits;

    bool dump_packets;

  } SctpSocket_Options;

  typedef SctpSocket_SendPacketStatus (*SctpSocket_SendPacket) (void * user_data, const uint8_t * data, size_t len);
  typedef void (*SctpSocket_OnMessageReceived) (void * user_data, uint16_t stream_id, uint32_t ppid,
                                                const uint8_t * data, size_t len);
  typedef void (*SctpSocket_OnError) (void * user_data, SctpSocket_Error error, const char * message);
  typedef void (*SctpSocket_OnAborted) (void * user_data, SctpSocket_Error error, const char * message);
  typedef void (*SctpSocket_OnConnected) (void * user_data);
  typedef void (*SctpSocket_OnClosed) (void * user_data);
  typedef void (*SctpSocket_OnConnectionRestarted) (void * user_data);

  typedef void (*SctpSocket_OnStreamsResetFailed) (void * user_data, const uint16_t * streams, size_t len,
                                                   const char * message);
  typedef void (*SctpSocket_OnStreamsResetPerformed) (void * user_data, const uint16_t * streams, size_t len);
  typedef void (*SctpSocket_OnIncomingStreamsReset) (void * user_data, const uint16_t * streams, size_t len);
  typedef void (*SctpSocket_OnBufferedAmountLow) (void * user_data, uint16_t stream_id);
  typedef void (*SctpSocket_OnTotalBufferedAmountLow) (void * user_data);

  typedef void * (*SctpSocketTimeout_Create) (void * user_data);
  typedef void (*SctpSocketTimeout_Delete) (void * user_data, void * timeout);
  typedef void (*SctpSocketTimeout_Start) (void * user_data, void * timeout, int32_t milliseconds, uint64_t timeout_id);
  typedef void (*SctpSocketTimeout_Stop) (void * user_data, void * timeout);

  typedef uint64_t (*SctpSocketTimeout_TimeMillis) (void * user_data);
  typedef uint32_t (*SctpSocket_GetRandomInt) (void * user_data, uint32_t low, uint32_t high);

  typedef void (*SctpSocket_LoggingFunction) (SctpSocket_LoggingSeverity severity, const char * msg);
  typedef void (*SctpSocket_PacketDumpFunction) (int64_t now, const uint8_t * data, size_t len);

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

    SctpSocketTimeout_Create timeout_create;
    SctpSocketTimeout_Delete timeout_delete;
    SctpSocketTimeout_Start timeout_start;
    SctpSocketTimeout_Stop timeout_stop;

    SctpSocketTimeout_TimeMillis time_millis;
    SctpSocket_GetRandomInt get_random_int;

    SctpSocket_PacketDumpFunction on_sent_packet;
    SctpSocket_PacketDumpFunction on_received_packet;

    void * user_data;
  };

  SctpSocket * sctp_socket_new (SctpSocket_Options * opts, SctpSocket_Callbacks * callbacks);

  void sctp_socket_free (SctpSocket * socket);

  // To be called when an incoming SCTP packet is to be processed.
  void sctp_socket_receive_packet (SctpSocket * socket, uint8_t * data, size_t len);

  // To be called when a timeout has expired. The `timeout_id` is provided
  // when the timeout was initiated.
  void sctp_socket_handle_timeout (SctpSocket * socket, uint64_t timeout_id);

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
  SctpSocket_SendStatus sctp_socket_send (SctpSocket * socket, const uint8_t * data, size_t len, uint16_t stream_id,
                                          uint32_t ppid, bool unordered, int32_t * lifetime,
                                          size_t * max_retransmissions);

  void sctp_socket_send_abort (SctpSocket * socket, const char *message);

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

  void sctp_socket_register_logging_function (SctpSocket_LoggingFunction log_func);

#ifdef __cplusplus
}
#endif

#endif // _SCTP_SOCKET_H_
