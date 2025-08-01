/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "net/dcsctp/packet/chunk/heartbeat_ack_chunk.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "api/array_view.h"
#include "net/dcsctp/packet/bounded_byte_reader.h"
#include "net/dcsctp/packet/bounded_byte_writer.h"
#include "net/dcsctp/packet/parameter/parameter.h"

namespace dcsctp {

// https://tools.ietf.org/html/rfc4960#section-3.3.6

//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |   Type = 5    | Chunk  Flags  |    Heartbeat Ack Length       |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  \                                                               \
//  /            Heartbeat Information TLV (Variable-Length)        /
//  \                                                               \
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
constexpr int HeartbeatAckChunk::kType;

std::optional<HeartbeatAckChunk> HeartbeatAckChunk::Parse(
    webrtc::ArrayView<const uint8_t> data) {
  std::optional<BoundedByteReader<kHeaderSize>> reader = ParseTLV(data);
  if (!reader.has_value()) {
    return std::nullopt;
  }

  std::optional<Parameters> parameters =
      Parameters::Parse(reader->variable_data());
  if (!parameters.has_value()) {
    return std::nullopt;
  }
  return HeartbeatAckChunk(*std::move(parameters));
}

void HeartbeatAckChunk::SerializeTo(std::vector<uint8_t>& out) const {
  webrtc::ArrayView<const uint8_t> parameters = parameters_.data();
  BoundedByteWriter<kHeaderSize> writer = AllocateTLV(out, parameters.size());
  writer.CopyToVariableData(parameters);
}

std::string HeartbeatAckChunk::ToString() const {
  return "HEARTBEAT-ACK";
}
}  // namespace dcsctp
