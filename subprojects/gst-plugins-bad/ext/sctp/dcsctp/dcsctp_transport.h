#ifndef _DCSCTP_TRANSPORT_H_
#define _DCSCTP_TRANSPORT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct DCSctpTransport DCSctpTransport;

DCSctpTransport *
dcsctp_socket_new (void);

void
dcsctp_socket_free (DCSctpTransport *);

#ifdef __cplusplus
}
#endif

#endif // _DCSCTP_TRANSPORT_H_