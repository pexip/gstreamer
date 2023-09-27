#ifndef _DCSCTP_TRANSPORT_H_
#define _DCSCTP_TRANSPORT_H_

#include <stddef.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct _DCSctpTransport DCSctpTransport;
typedef struct _DCSctpTransport_Callbacks DCSctpTransport_Callbacks; 

typedef enum
{
  DCSCTP_SUCCESS = 0,
  __DCSCTP_ERROR_MAX__
} DCSctpError;


typedef enum
{
	DCSCTP_DATA_MSG_TYPE_TEXT,
	DCSCTP_DATA_MSG_TYPE_BIN,
	DCSCTP_DATA_MSG_TYPE_CTRL,
} DCSctp_DataMessageType;

typedef void (*DCSctpTransport_DataReceivedCallback)(void * user_data, int channel_id, DCSctp_DataMessageType type, const uint8_t * data, size_t len);
typedef void (*DCSctpTransport_ChannelClosingCallback)(void * user_data, int channel_id);
typedef void (*DCSctpTransport_ChannelClosedCallback)(void * user_data, int channel_id);
typedef void (*DCSctpTransport_ReadyToSendCallback)(void * user_data);
typedef void (*DCSctpTransport_TransportClosedCallback)(void * user_data, DCSctpError error);

struct _DCSctpTransport_Callbacks
{
  DCSctpTransport_DataReceivedCallback data_received;
  DCSctpTransport_ChannelClosingCallback channel_closing;
  DCSctpTransport_ChannelClosedCallback hannel_closed;
  DCSctpTransport_ReadyToSendCallback ready_to_send;
  DCSctpTransport_TransportClosedCallback transport_closed;
  void * user_data;
};

DCSctpTransport *
dcsctp_transport_new (DCSctpTransport_Callbacks * callbacks);

void
dcsctp_transport_free (DCSctpTransport *);

bool
dcsctp_transport_start (DCSctpTransport * transport,
	                    int local_sctp_port,
                        int remote_sctp_port,
                        int max_message_size);

bool
dcsctp_transport_open_stream (DCSctpTransport * transport, int sid);

bool
dcsctp_transport_reset_stream (DCSctpTransport * transport, int sid);

DCSctpError
dcsctp_transport_send_data (DCSctpTransport * transport, DCSctp_DataMessageType msg_type, bool ordered, int max_rtx_count, int max_rtx_ms, void * data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // _DCSCTP_TRANSPORT_H_