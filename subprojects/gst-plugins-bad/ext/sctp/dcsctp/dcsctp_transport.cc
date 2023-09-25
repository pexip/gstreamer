#include "dcsctp_transport.h"

struct DCSctpTransport
{
  DCSctpTransport ()
  {

  }

   ~DCSctpTransport ()
  {

  }

};

DCSctpTransport *
dcsctp_transport_new (void)
{
  return new DCSctpTransport ();
}

void
dcsctp_transport_free (DCSctpTransport * s)
{
  delete s;
}
