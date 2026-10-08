// Included by net_session.cpp ONLY, after the recomp-net headers: turns each
// call it makes into a call through the loaded module (net_module_api.hpp).
#pragma once
#include "net_module_api.hpp"

#define rnet_rb_driver_confirmed_through (::retro::runner::g_nm.rnet_rb_driver_confirmed_through)
#define rnet_rb_driver_create (::retro::runner::g_nm.rnet_rb_driver_create)
#define rnet_rb_driver_destroy (::retro::runner::g_nm.rnet_rb_driver_destroy)
#define rnet_rb_driver_desync_count (::retro::runner::g_nm.rnet_rb_driver_desync_count)
#define rnet_rb_driver_episode_count (::retro::runner::g_nm.rnet_rb_driver_episode_count)
#define rnet_rb_driver_finish_frame (::retro::runner::g_nm.rnet_rb_driver_finish_frame)
#define rnet_rb_driver_poll_admit (::retro::runner::g_nm.rnet_rb_driver_poll_admit)
#define rnet_rb_driver_quiesce_state (::retro::runner::g_nm.rnet_rb_driver_quiesce_state)
#define rnet_rb_driver_refusal (::retro::runner::g_nm.rnet_rb_driver_refusal)
#define rnet_rb_driver_request_quiesce (::retro::runner::g_nm.rnet_rb_driver_request_quiesce)
#define rnet_rb_driver_rtt_estimate_ms (::retro::runner::g_nm.rnet_rb_driver_rtt_estimate_ms)
#define rnet_rb_driver_set_identity (::retro::runner::g_nm.rnet_rb_driver_set_identity)
#define rnet_rb_driver_shutdown (::retro::runner::g_nm.rnet_rb_driver_shutdown)
#define rnet_rb_driver_start (::retro::runner::g_nm.rnet_rb_driver_start)
#define rnet_session_create (::retro::runner::g_nm.rnet_session_create)
#define rnet_session_destroy (::retro::runner::g_nm.rnet_session_destroy)
#define rnet_session_is_running (::retro::runner::g_nm.rnet_session_is_running)
#define rnet_session_peer_disconnected (::retro::runner::g_nm.rnet_session_peer_disconnected)
#define rnet_session_pump (::retro::runner::g_nm.rnet_session_pump)
#define rnet_session_send_bye (::retro::runner::g_nm.rnet_session_send_bye)
#define rnet_session_start_lan (::retro::runner::g_nm.rnet_session_start_lan)
#define rnet_session_start_lan_hub (::retro::runner::g_nm.rnet_session_start_lan_hub)
#define rnet_session_wait_recv (::retro::runner::g_nm.rnet_session_wait_recv)
