/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "net/dcsctp/packet/parameter/add_outgoing_streams_request_parameter.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "api/array_view.h"
#include "net/dcsctp/common/internal_types.h"
#include "net/dcsctp/packet/bounded_byte_reader.h"
#include "net/dcsctp/packet/bounded_byte_writer.h"
#include "rtc_base/strings/string_builder.h"

namespace dcsctp {

// https://tools.ietf.org/html/rfc6525#section-4.5

//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |     Parameter Type = 17       |      Parameter Length = 12    |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |          Re-configuration Request Sequence Number             |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |      Number of new streams    |         Reserved              |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
constexpr int AddOutgoingStreamsRequestParameter::kType;

std::optional<AddOutgoingStreamsRequestParameter>
AddOutgoingStreamsRequestParameter::Parse(
    webrtc::ArrayView<const uint8_t> data) {
  std::optional<BoundedByteReader<kHeaderSize>> reader = ParseTLV(data);
  if (!reader.has_value()) {
    return std::nullopt;
  }
  ReconfigRequestSN request_sequence_number(reader->Load32<4>());
  uint16_t nbr_of_new_streams = reader->Load16<8>();

  return AddOutgoingStreamsRequestParameter(request_sequence_number,
                                            nbr_of_new_streams);
}

void AddOutgoingStreamsRequestParameter::SerializeTo(
    std::vector<uint8_t>& out) const {
  BoundedByteWriter<kHeaderSize> writer = AllocateTLV(out);
  writer.Store32<4>(*request_sequence_number_);
  writer.Store16<8>(nbr_of_new_streams_);
}

std::string AddOutgoingStreamsRequestParameter::ToString() const {
  webrtc::StringBuilder sb;
  sb << "Add Outgoing Streams Request, req_seq_nbr="
     << *request_sequence_number();
  return sb.Release();
}

}  // namespace dcsctp
