// The recomp-net functions the runner calls, as pointers into the loaded
// module. Add a name here when net_session.cpp starts calling a new one: the
// loader refuses a module that lacks any entry, at load, by name.
#pragma once

extern "C" {
#include "recomp_net/rb_driver.h"
#include "recomp_net/session.h"
}

#define RETRO_NET_MODULE_SYMBOLS(X)  \
    X(rnet_rb_driver_confirmed_through) \
    X(rnet_rb_driver_create)         \
    X(rnet_rb_driver_destroy)        \
    X(rnet_rb_driver_desync_count)   \
    X(rnet_rb_driver_episode_count)  \
    X(rnet_rb_driver_finish_frame)   \
    X(rnet_rb_driver_poll_admit)     \
    X(rnet_rb_driver_quiesce_state)  \
    X(rnet_rb_driver_refusal)        \
    X(rnet_rb_driver_request_quiesce) \
    X(rnet_rb_driver_rtt_estimate_ms) \
    X(rnet_rb_driver_set_identity)   \
    X(rnet_rb_driver_shutdown)       \
    X(rnet_rb_driver_start)          \
    X(rnet_session_create)           \
    X(rnet_session_destroy)          \
    X(rnet_session_is_running)       \
    X(rnet_session_peer_disconnected) \
    X(rnet_session_pump)             \
    X(rnet_session_send_bye)         \
    X(rnet_session_start_lan)        \
    X(rnet_session_start_lan_hub)    \
    X(rnet_session_wait_recv)

namespace retro::runner {
struct NetModuleApi {
#define RETRO_NM_FIELD(n) decltype(&::n) n;
    RETRO_NET_MODULE_SYMBOLS(RETRO_NM_FIELD)
#undef RETRO_NM_FIELD
};
extern NetModuleApi g_nm;
} // namespace retro::runner
