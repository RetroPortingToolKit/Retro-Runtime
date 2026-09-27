#pragma once

// Messages over the link's control channel (transport.hpp): one whole message
// at a time, with any handles it carries.

#include "link_protocol.hpp"
#include "transport.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

namespace retro::corelink {

template <typename M>
bool send_msg(Channel& ch, M& m, const NativeHandle* handles = nullptr, std::size_t count = 0) {
    m.h.size = sizeof(M);
    return send_packet(ch, &m, sizeof(M), handles, count);
}

// The packet's type, once its size is at least a header.
inline bool packet_type(const std::vector<unsigned char>& buf, Msg& out) {
    if (buf.size() < sizeof(MsgHeader)) return false;
    out = reinterpret_cast<const MsgHeader*>(buf.data())->type;
    return true;
}

// Copies a packet into a message struct. The minor-version rule
// (link_protocol.hpp): a newer peer may have appended fields, so a longer
// packet is read and its tail ignored; an older peer may not have sent fields
// this side appended, so a packet down to `min_size` -- the message's size
// before its first appended field -- is read and the rest zeroed. No message
// has grown yet, so every reader passes sizeof(M).
//
// A 1.0 peer still demands the exact 1.0 size. A message that grows must
// therefore be sent at its old size to a session speaking an older minor.
template <typename M>
bool as_msg(const std::vector<unsigned char>& buf, M& out, std::size_t min_size = sizeof(M)) {
    if (buf.size() < min_size || min_size < sizeof(MsgHeader)) return false;
    std::memset(&out, 0, sizeof(M));
    std::memcpy(&out, buf.data(), buf.size() < sizeof(M) ? buf.size() : sizeof(M));
    return true;
}

} // namespace retro::corelink
