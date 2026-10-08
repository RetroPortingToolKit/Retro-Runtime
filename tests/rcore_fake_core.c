/*
 * rcore_fake_core -- the smallest core that exercises the host: a silent core
 * that states a 50 Hz frame rate after load(), submits one 64x48 frame per
 * run_frame, and logs how many frames it ran at unload. A host pacing it
 * correctly runs it at 50 frames a second, not the 60 Hz fallback.
 *
 * It declares one 64-byte battery region, "battery", erase value 0xFF, and
 * frame k writes k & 0xff at offset k % 64. So after N frames the save is
 * known exactly, whichever host held it.
 *
 * It declares SAVESTATE: its state is 12 bytes, "FAKE" and the frame count
 * (little-endian u64). Frame k's picture is k & 0xff everywhere, so after a
 * state saved at frame K is loaded, the next picture is K+1 -- a host can see
 * a load land without reading anything but pixels. unserialize() refuses
 * anything else with RCORE_ERR_CONTENT, touching nothing.
 *
 * FAKE_CORE_CRASH_AT=N (an instrument knob) makes frame N kill the process
 * outright, with no unload: a crash, as far as the host can tell. The host
 * link must still write the save as frames 1..N-1 left it.
 *
 * It declares four options and three input descriptors that it never reads:
 * they are there for --describe (tests/describe_test.cmake), so every option
 * type, every flag, a NULL default and description, and a TAB, LF, CR and
 * backslash inside a field are all exercised.
 *
 * It declares ACCESSORY_DATA and one accessory type, n64.vru (rev 7), on any
 * seat's slot 0 -- the type the runner's --vruN binds -- so the host side of
 * accessory_poll / accessory_notify can be driven (tests/accessory_test.cmake).
 * Each frame it drains the poll for every bound (seat, slot), logging
 *   FAKE_ACCESSORY frame=K seat=S slot=L <bytes>
 * per message and answering each with a notify, {"echo":<bytes>,"frame":K};
 * then drains again and logs FAKE_ACCESSORY_UNSTABLE at ERROR if the second
 * drain differs from the first (the contract: stable for the frame). It also
 * logs FAKE_VRU_SEAT seat=S connected=C each frame for a bound seat, which
 * the host must report as 0. The package build declares none of this, so a
 * core WITHOUT the type exists for the refusals.
 *
 * Built with FAKE_GAME_PACKAGE it is fake_pkg_core: core id "fake_pkg", it
 * also declares GAME_PACKAGE, and load() requires package_path to name a
 * readable file whose first line is FAKE_PACKAGE_MAGIC (tests/fake_package.txt),
 * logging "FAKE_PACKAGE ok <path>". Anything else is RCORE_ERR_CONTENT.
 *
 * Built with FAKE_FUTURE_CAP it is fake_future_core: the plain core plus a
 * capability bit this runner has no name for (bit 40), as a core built
 * against a newer draft revision declares. Its sidecar names it
 * "future_feature". The runner must warn and run it, not refuse it.
 *
 * Test fixture only; it exports rcore_entry and nothing else. Its sidecar
 * manifest is written beside it by CMake.
 */
#include "rcore/rcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <signal.h>
#  include <unistd.h>
#endif

#define W 64
#define H 48

static const rcore_host_api* g_host;
static unsigned long long g_frames;
static unsigned char g_pixels[W * H * 4];
static unsigned char* g_battery;
static unsigned long long g_crash_at;

static rcore_save_region k_regions[1];

#if defined(FAKE_GAME_PACKAGE)
#  define FAKE_ID "fake_pkg"
#  define FAKE_CAPS (RCORE_CAP_RUN_FRAME | RCORE_CAP_SAVESTATE | RCORE_CAP_GAME_PACKAGE)
#  define FAKE_PACKAGE_MAGIC "rcore fake game package"
#elif defined(FAKE_FUTURE_CAP)
#  define FAKE_ID "fake_future"
#  define FAKE_CAPS (RCORE_CAP_RUN_FRAME | RCORE_CAP_SAVESTATE | RCORE_CAP_ACCESSORY_DATA | (1ull << 40))
#  define FAKE_ACCESSORY_DATA 1
#else
#  define FAKE_ID "fake"
#  define FAKE_CAPS (RCORE_CAP_RUN_FRAME | RCORE_CAP_SAVESTATE | RCORE_CAP_ACCESSORY_DATA)
#  define FAKE_ACCESSORY_DATA 1
#endif

#if defined(FAKE_ACCESSORY_DATA)
#  define MAX_BOUND 8
#  define MAX_MSG 65536
static uint32_t g_bound_seat[MAX_BOUND], g_bound_slot[MAX_BOUND];
static unsigned g_bound;
static char g_msg[MAX_MSG], g_msg2[MAX_MSG];

static const rcore_accessory_type k_accessory_types[] = {
    {sizeof(rcore_accessory_type), RCORE_ACC_FLAG_NETPLAY, 0xF, 0x1, "n64.vru", "VRU Microphone",
     NULL},
};

static const rcore_accessory_type* accessory_types(uint32_t* count) {
    *count = sizeof k_accessory_types / sizeof k_accessory_types[0];
    return k_accessory_types;
}

static int host_has_accessory_data(void) {
    return g_host->struct_size >= offsetof(rcore_host_api, accessory_notify) + sizeof(void*) &&
           g_host->accessory_poll && g_host->accessory_notify;
}

/* Drain one bound accessory: log each message and echo it back; then drain
 * again and compare, which is the contract's "stable for the frame". */
static void drain_accessory(uint32_t seat, uint32_t slot) {
    char line[MAX_MSG + 96];
    size_t sizes[64];
    unsigned count = 0, i;
    size_t n;
    if (!host_has_accessory_data()) return;
    while ((n = g_host->accessory_poll(g_host->host_ctx, seat, slot, g_msg, sizeof g_msg)) != 0) {
        if (n > sizeof g_msg) {
            g_host->log(g_host->host_ctx, RCORE_LOG_ERROR, "FAKE_ACCESSORY message too large");
            return;
        }
        snprintf(line, sizeof line, "FAKE_ACCESSORY frame=%llu seat=%u slot=%u %.*s", g_frames,
                 seat, slot, (int)n, g_msg);
        g_host->log(g_host->host_ctx, RCORE_LOG_INFO, line);
        snprintf(line, sizeof line, "{\"echo\":%.*s,\"frame\":%llu}", (int)n, g_msg, g_frames);
        g_host->accessory_notify(g_host->host_ctx, seat, slot, line, strlen(line));
        if (count < sizeof sizes / sizeof sizes[0]) sizes[count] = n;
        ++count;
    }
    /* The second drain must repeat the first, message for message. */
    for (i = 0; i < count; ++i) {
        n = g_host->accessory_poll(g_host->host_ctx, seat, slot, g_msg2, sizeof g_msg2);
        if (i < sizeof sizes / sizeof sizes[0] && n != sizes[i]) {
            snprintf(line, sizeof line, "FAKE_ACCESSORY_UNSTABLE frame=%llu message %u: %zu then %zu bytes",
                     g_frames, i, sizes[i], n);
            g_host->log(g_host->host_ctx, RCORE_LOG_ERROR, line);
            return;
        }
    }
    if (count && g_host->accessory_poll(g_host->host_ctx, seat, slot, g_msg2, sizeof g_msg2) != 0) {
        snprintf(line, sizeof line, "FAKE_ACCESSORY_UNSTABLE frame=%llu: the second drain is longer",
                 g_frames);
        g_host->log(g_host->host_ctx, RCORE_LOG_ERROR, line);
    }
}

static void bound_seat_pad(uint32_t seat) {
    char line[96];
    rcore_pad pad;
    memset(&pad, 0, sizeof pad);
    pad.struct_size = sizeof pad;
    g_host->input_get(g_host->host_ctx, seat, &pad);
    snprintf(line, sizeof line, "FAKE_VRU_SEAT seat=%u connected=%u buttons=%u", seat,
             (unsigned)pad.connected, (unsigned)pad.buttons);
    g_host->log(g_host->host_ctx, RCORE_LOG_INFO, line);
}
#endif

static const rcore_core_info k_info = {
    sizeof(rcore_core_info), RCORE_ABI_MAJOR, RCORE_ABI_MINOR, 0,
    FAKE_ID, "1.0", "test", FAKE_CAPS, NULL,
};

static const char* const k_mode_values[] = {"fast", "accurate", "tab\there", NULL};

static const rcore_option k_options[] = {
    {sizeof(rcore_option), RCORE_OPT_ENUM, RCORE_OPT_FLAG_RESTART | RCORE_OPT_FLAG_NETPLAY, 0,
     "video.mode", "Video mode", "Line one\nline two\twith a back\\slash", "fast",
     k_mode_values, 0, 0},
    {sizeof(rcore_option), RCORE_OPT_BOOL, 0, 0, "audio.mute", "Mute", NULL, "0", NULL, 0, 0},
    {sizeof(rcore_option), RCORE_OPT_INT, RCORE_OPT_FLAG_DEVELOPER, 0, "cpu.overclock",
     "Overclock", "Percent over stock", NULL, NULL, -5, 1000000000000LL},
    {sizeof(rcore_option), RCORE_OPT_STRING,
     RCORE_OPT_FLAG_RESTART | RCORE_OPT_FLAG_NETPLAY | RCORE_OPT_FLAG_DEVELOPER, 0,
     "debug.trace", "Trace\rfile", NULL, NULL, NULL, 0, 0},
};

static const rcore_input_descriptor k_inputs[] = {
    {sizeof(rcore_input_descriptor), RCORE_PAD_SOUTH, 0, 0, "A"},
    {sizeof(rcore_input_descriptor), 0, RCORE_AXIS_RX + 1, -1, "C-Left"},
    {sizeof(rcore_input_descriptor), 0, RCORE_AXIS_LX + 1, 0, "Stick"},
};

static const rcore_option* opts(uint32_t* count) {
    *count = sizeof k_options / sizeof k_options[0];
    return k_options;
}
static const rcore_input_descriptor* descs(uint32_t* count) {
    *count = sizeof k_inputs / sizeof k_inputs[0];
    return k_inputs;
}

static rcore_result init(const rcore_host_api* host, const rcore_init_params* params) {
    (void)params;
    g_host = host;
    return RCORE_OK;
}

#if defined(FAKE_GAME_PACKAGE)
/* The package must be named, readable, and the fixture package. */
static rcore_result check_package(const rcore_load_params* params) {
    char line[512], first[64];
    FILE* f;
    if (!params->package_path) {
        g_host->log(g_host->host_ctx, RCORE_LOG_ERROR, "FAKE_PACKAGE missing: no package_path");
        return RCORE_ERR_CONTENT;
    }
    f = fopen(params->package_path, "rb");
    if (!f || !fgets(first, sizeof first, f) ||
        strncmp(first, FAKE_PACKAGE_MAGIC, strlen(FAKE_PACKAGE_MAGIC)) != 0) {
        if (f) fclose(f);
        snprintf(line, sizeof line, "FAKE_PACKAGE rejected %s", params->package_path);
        g_host->log(g_host->host_ctx, RCORE_LOG_ERROR, line);
        return RCORE_ERR_CONTENT;
    }
    fclose(f);
    snprintf(line, sizeof line, "FAKE_PACKAGE ok %s", params->package_path);
    g_host->log(g_host->host_ctx, RCORE_LOG_INFO, line);
    return RCORE_OK;
}
#endif

static rcore_result load(const rcore_load_params* params, const rcore_save_region** regions,
                         uint32_t* count) {
#if defined(FAKE_GAME_PACKAGE)
    {
        const rcore_result rc = check_package(params);
        if (rc != RCORE_OK) return rc;
    }
#endif
#if defined(FAKE_ACCESSORY_DATA)
    {
        uint32_t i;
        g_bound = 0;
        for (i = 0; i < params->accessory_count && g_bound < MAX_BOUND; ++i) {
            const rcore_accessory_binding* b = &params->accessories[i];
            if (b->type_id && strcmp(b->type_id, "n64.vru") == 0) {
                g_bound_seat[g_bound] = b->seat;
                g_bound_slot[g_bound] = b->slot;
                ++g_bound;
            }
        }
    }
#endif
    (void)params;
    memset(k_regions, 0, sizeof k_regions);
    k_regions[0].struct_size = sizeof(rcore_save_region);
    k_regions[0].kind = RCORE_SAVE_BATTERY;
    k_regions[0].id = "battery";
    k_regions[0].size = 64;
    k_regions[0].erase_value = 0xFF;
    *regions = k_regions;
    *count = 1;
    {
        const char* crash = getenv("FAKE_CORE_CRASH_AT");
        g_crash_at = crash ? strtoull(crash, NULL, 10) : 0;
    }
    /* The rate is known once content is: state it (rev 5). */
    if (g_host->struct_size >= offsetof(rcore_host_api, set_frame_rate) + sizeof(void*) &&
        g_host->set_frame_rate) {
        g_host->set_frame_rate(g_host->host_ctx, 50, 1);
    }
    return RCORE_OK;
}

static void die_now(void) {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 0xC0000409u);
#else
    kill(getpid(), SIGKILL);
#endif
}

static rcore_result run_frame(void) {
    ++g_frames;
    if (g_crash_at && g_frames == g_crash_at) die_now();
#if defined(FAKE_ACCESSORY_DATA)
    {
        /* At the frame boundary, before the frame runs (rev 7). */
        unsigned i;
        for (i = 0; i < g_bound; ++i) {
            drain_accessory(g_bound_seat[i], g_bound_slot[i]);
            bound_seat_pad(g_bound_seat[i]);
        }
    }
#endif
    if (!g_battery) g_battery = (unsigned char*)g_host->save_memory(g_host->host_ctx, "battery");
    if (g_battery) g_battery[g_frames % 64] = (unsigned char)(g_frames & 0xff);
    memset(g_pixels, (int)(g_frames & 0xff), sizeof g_pixels);
    rcore_frame f;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    f.width = W;
    f.height = H;
    f.stride = W * 4;
    f.pixel_format = RCORE_PIXEL_RGBA8;
    f.pixels = g_pixels;
    f.frame_number = g_frames;
    f.game_frame = g_frames / 2; /* rev 8: a game drawing every other frame */
    g_host->video_submit(g_host->host_ctx, &f);
    return RCORE_OK;
}

#define STATE_SIZE 12u

static uint64_t state_size(void) { return STATE_SIZE; }

static rcore_result serialize(void* out, uint64_t size) {
    unsigned char* p = (unsigned char*)out;
    int i;
    if (!out || size < STATE_SIZE) return RCORE_ERR_INTERNAL;
    memcpy(p, "FAKE", 4);
    for (i = 0; i < 8; ++i) p[4 + i] = (unsigned char)(g_frames >> (8 * i));
    return RCORE_OK;
}

static rcore_result unserialize(const void* in, uint64_t size) {
    const unsigned char* p = (const unsigned char*)in;
    unsigned long long frames = 0;
    int i;
    if (!in || size != STATE_SIZE || memcmp(p, "FAKE", 4) != 0) {
        g_host->log(g_host->host_ctx, RCORE_LOG_ERROR, "FAKE_STATE refused: not a fake state");
        return RCORE_ERR_CONTENT;
    }
    for (i = 7; i >= 0; --i) frames = (frames << 8) | p[4 + i];
    g_frames = frames;
    return RCORE_OK;
}

static void unload(void) {
    char line[64];
    snprintf(line, sizeof line, "FAKE_DONE frames=%llu", g_frames);
    g_host->log(g_host->host_ctx, RCORE_LOG_INFO, line);
}

static void deinit(void) {}

static const rcore_core_api k_api = {
    sizeof(rcore_core_api), 0, &k_info, opts, descs, init, load, run_frame, NULL,
    NULL, state_size, serialize, unserialize, unload, deinit,
#if defined(FAKE_ACCESSORY_DATA)
    accessory_types,
#endif
};

RCORE_EXPORT const rcore_core_api* rcore_entry(uint32_t host_abi_major) {
    return host_abi_major == RCORE_ABI_MAJOR ? &k_api : NULL;
}
