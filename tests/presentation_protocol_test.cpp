#include "link_io.hpp"
#include <cstring>
#include <vector>
using namespace retro::corelink;
int main() {
    if (link_has_presentation_size(2) || !link_has_presentation_size(3)) return 1;
    GrantMsg sent{}; sent.h.type = Msg::Grant; sent.frame_number = 42;
    sent.pads[0].buttons = 123; sent.presentation_width = 2560; sent.presentation_height = 1080;
    std::vector<unsigned char> bytes(sizeof sent);
    std::memcpy(bytes.data(), &sent, sizeof sent);
    GrantMsg got{};
    if (!as_msg(bytes, got, kGrantSize22) || got.presentation_width != 2560 || got.presentation_height != 1080) return 2;
    bytes.resize(kGrantSize22);
    if (!as_msg(bytes, got, kGrantSize22) || got.frame_number != 42 || got.pads[0].buttons != 123 ||
        got.presentation_width || got.presentation_height) return 3;
    bytes.resize(kGrantSize22 - 1);
    if (as_msg(bytes, got, kGrantSize22)) return 4;
    if (link_has_video_demand(5) || !link_has_video_demand(6)) return 5;
    sent.flags = kGrantSkipVideo;
    bytes.resize(sizeof sent); std::memcpy(bytes.data(), &sent, sizeof sent);
    if (!as_msg(bytes, got, kGrantSize22) || !(got.flags & kGrantSkipVideo)) return 6;
    bytes.resize(kGrantSize25);
    if (!as_msg(bytes, got, kGrantSize22) || got.flags || got.presentation_width != 2560) return 7;
    bytes.resize(kGrantSize22);
    if (!as_msg(bytes, got, kGrantSize22) || got.flags || got.presentation_width) return 8;
    return 0;
}
