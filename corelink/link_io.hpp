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

// Copies a packet into a message struct when the size matches exactly.
template <typename M>
bool as_msg(const std::vector<unsigned char>& buf, M& out) {
    if (buf.size() != sizeof(M)) return false;
    std::memcpy(&out, buf.data(), sizeof(M));
    return true;
}

} // namespace retro::corelink
