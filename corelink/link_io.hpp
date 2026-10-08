#pragma once

// Messages over the link's control channel (transport.hpp): one whole message
// at a time, with any handles it carries.

#include "link_protocol.hpp"
#include "transport.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace retro::corelink {

template <typename M>
bool send_msg(Channel& ch, M& m, const NativeHandle* handles = nullptr, std::size_t count = 0) {
    m.h.size = sizeof(M);
    return send_packet(ch, &m, sizeof(M), handles, count);
}

// The first `size` bytes of `m`: a grown message sent at its older size to a
// peer speaking an older minor.
template <typename M>
bool send_msg_sized(Channel& ch, M& m, std::size_t size) {
    if (size < sizeof(MsgHeader) || size > sizeof(M)) return false;
    m.h.size = static_cast<std::uint32_t>(size);
    return send_packet(ch, &m, size);
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
// before its first appended field -- is read and the rest zeroed. FrameDone
// grew in 2.2 (its reader passes kFrameDoneSize21); every other reader passes
// sizeof(M).
//
// A 1.0 peer still demands the exact 1.0 size. A message that grows must
// therefore be sent at its old size to a session speaking an older minor
// (send_msg_sized).
template <typename M>
bool as_msg(const std::vector<unsigned char>& buf, M& out, std::size_t min_size = sizeof(M)) {
    if (buf.size() < min_size || min_size < sizeof(MsgHeader)) return false;
    std::memset(&out, 0, sizeof(M));
    std::memcpy(&out, buf.data(), buf.size() < sizeof(M) ? buf.size() : sizeof(M));
    return true;
}

// 2.1. A data accessory's bytes, sent at their own length: the header fields
// and `len` bytes, never the whole struct. False when `len` is more than one
// message carries (the sender keeps the bytes; nothing partial goes out).
inline bool send_accessory_msg(Channel& ch, Msg type, std::uint32_t seat, std::uint32_t slot,
                               const void* data, std::size_t len) {
    if (len > kMaxAccessoryBytes) return false;
    std::vector<unsigned char> pkt(kAccessoryMsgHeaderSize + len);
    AccessoryDataMsg head{};
    head.h.type = type;
    head.h.size = static_cast<std::uint32_t>(pkt.size());
    head.seat = seat;
    head.slot = slot;
    head.len = static_cast<std::uint32_t>(len);
    std::memcpy(pkt.data(), &head, kAccessoryMsgHeaderSize);
    if (len) std::memcpy(pkt.data() + kAccessoryMsgHeaderSize, data, len);
    return send_packet(ch, pkt.data(), pkt.size());
}

// The reverse: the fields, and the bytes `len` says are there. False for a
// packet shorter than its own `len` claims.
inline bool as_accessory_msg(const std::vector<unsigned char>& buf, std::uint32_t& seat,
                             std::uint32_t& slot, std::vector<std::uint8_t>& bytes) {
    if (buf.size() < kAccessoryMsgHeaderSize) return false;
    AccessoryDataMsg head{};
    std::memcpy(&head, buf.data(), kAccessoryMsgHeaderSize);
    if (head.len > kMaxAccessoryBytes || buf.size() < kAccessoryMsgHeaderSize + head.len) return false;
    seat = head.seat;
    slot = head.slot;
    bytes.assign(buf.begin() + static_cast<std::ptrdiff_t>(kAccessoryMsgHeaderSize),
                 buf.begin() + static_cast<std::ptrdiff_t>(kAccessoryMsgHeaderSize + head.len));
    return true;
}

} // namespace retro::corelink
