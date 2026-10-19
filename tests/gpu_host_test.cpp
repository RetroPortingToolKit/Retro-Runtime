#include "host_session.hpp"
#include <cstdlib>
#include <iostream>

using namespace retro::runner;
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL " << what << '\n'; std::exit(1); }
}
struct TestSink : Sink {
    bool available = false;
    unsigned samples = 0, registrations = 0, pictures = 0, retires = 0;
    rcore_gpu_capabilities caps{};
    void log(std::uint32_t, const char*) override {}
    void event(const rcore_event&) override {}
    void frame(const rcore_frame&) override {}
    void audio(const std::int16_t*, std::uint32_t) override {}
    void audio_rate(std::uint32_t) override {}
    void input(std::uint32_t, rcore_pad&) override {}
    bool gpu_capabilities(rcore_gpu_capabilities& out) override {
        ++samples; out = caps; return available;
    }
    bool gpu_register(const rcore_gpu_ring&) override { ++registrations; return true; }
    std::uint32_t gpu_released(std::uint64_t, std::uint32_t& mask) override {
        mask = 2; return RCORE_GPU_RING_READY;
    }
    bool gpu_frame(const rcore_gpu_frame&) override { ++pictures; return true; }
    void gpu_unregister(std::uint64_t) override { ++retires; }
};
int main() {
    LoadedCore core;
    TestSink absent;
    HostSession cpu(core, absent);
    cpu.offer_gpu_frames();
    absent.available = true;
    cpu.offer_gpu_frames();
    check(absent.samples == 1 && !cpu.host_api()->gpu_frames_capabilities &&
          !cpu.host_api()->video_submit_gpu, "unsupported host remains CPU for session");

    TestSink sink;
    sink.available = true;
    sink.caps.struct_size = sizeof sink.caps;
    sink.caps.handle_types = RCORE_GPU_HANDLE_WIN32_NT;
    sink.caps.sync_types = RCORE_GPU_SYNC_BINARY;
    sink.caps.max_images = 3;
    sink.caps.max_width = sink.caps.max_height = UINT32_MAX;
    sink.caps.device_uuid[0] = 7;
    sink.caps.device_luid[0] = 9;
    sink.caps.device_node_mask = 1;
    HostSession gpu(core, sink);
    gpu.offer_gpu_frames();
    const auto* h = gpu.host_api();
    check(h->gpu_frames_capabilities && h->gpu_frames_register && h->gpu_frames_released &&
          h->video_submit_gpu && h->gpu_frames_unregister, "complete optional API");
    sink.caps.device_uuid[0] = 55;
    gpu.offer_gpu_frames();
    rcore_gpu_capabilities sampled{};
    sampled.struct_size = sizeof sampled;
    check(h->gpu_frames_capabilities(h->host_ctx, &sampled) && sampled.device_uuid[0] == 7 &&
          sink.samples == 1, "actual capabilities sampled once and fixed");
    --sampled.struct_size;
    check(!h->gpu_frames_capabilities(h->host_ctx, &sampled), "short capability output refused");

    rcore_gpu_ring ring{};
    ring.struct_size = sizeof ring;
    ring.generation = 42;
    ring.handle_type = RCORE_GPU_HANDLE_WIN32_NT;
    ring.sync_type = RCORE_GPU_SYNC_BINARY;
    ring.pixel_format = RCORE_GPU_FORMAT_RGBA8;
    ring.flags = RCORE_GPU_ORIGIN_TOP_LEFT;
    ring.width = 8; ring.height = 4; ring.image_count = 2;
    ring.device_uuid[0] = 7; ring.device_luid[0] = 9; ring.device_node_mask = 1;
    for (unsigned i = 0; i < ring.image_count; ++i) {
        ring.images[i].struct_size = sizeof(rcore_gpu_image);
        ring.images[i].allocation_size = 128;
        ring.images[i].memory_handle = 100 + i * 3;
        ring.images[i].ready_semaphore = 101 + i * 3;
        ring.images[i].release_semaphore = 102 + i * 3;
    }
    check(h->gpu_frames_register(h->host_ctx, &ring) == 1, "valid ring reaches sink");
    auto reject = [&](rcore_gpu_ring bad, const char* why) {
        const auto before = sink.registrations;
        check(!h->gpu_frames_register(h->host_ctx, &bad) && sink.registrations == before, why);
    };
    auto bad = ring; bad.device_uuid[0] ^= 1; reject(bad, "UUID mismatch");
    bad = ring; bad.device_luid[0] ^= 1; reject(bad, "LUID mismatch");
    bad = ring; bad.device_node_mask = 2; reject(bad, "node mismatch");
    bad = ring; bad.image_count = 4; reject(bad, "too many images");
    bad = ring; bad.width = 0; reject(bad, "empty geometry");
    bad = ring; bad.generation = 0; reject(bad, "zero generation");
    bad = ring; bad.images[0].allocation_size = 127; reject(bad, "short image allocation");
    bad = ring; bad.images[0].ready_semaphore = bad.images[0].release_semaphore; reject(bad, "same binary semaphore");
    bad = ring; bad.images[1].memory_handle = bad.images[0].memory_handle; reject(bad, "duplicate image handle");
    bad = ring; bad.images[1].ready_semaphore = bad.images[0].release_semaphore; reject(bad, "cross-image semaphore alias");
    bad = ring; bad.width = bad.height = UINT32_MAX;
    bad.images[0].allocation_size = bad.images[1].allocation_size = UINT64_MAX;
    reject(bad, "pixel byte-count overflow");
    std::uint32_t released = 88;
    check(h->gpu_frames_released(h->host_ctx, 0, &released) == RCORE_GPU_RING_REFUSED && released == 0,
          "invalid release is empty");
    check(h->gpu_frames_released(h->host_ctx, 42, &released) == RCORE_GPU_RING_READY && released == 2,
          "ownership event forwarded");
    rcore_gpu_frame frame{};
    frame.struct_size = sizeof frame; frame.generation = 42;
    check(h->video_submit_gpu(h->host_ctx, &frame), "GPU submission accepted");
    frame.image_index = 3;
    check(!h->video_submit_gpu(h->host_ctx, &frame) && sink.pictures == 1, "invalid slot refused");
    h->gpu_frames_unregister(h->host_ctx, 0); h->gpu_frames_unregister(h->host_ctx, 42);
    check(sink.retires == 1, "one retirement forwarded");
    std::cout << "GPU host contract: fixed offer, adapter and geometry refusal, CPU fallback PASS\n";
}
