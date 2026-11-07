#include "route.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace retro::runner;
namespace {
void check_at(bool yes,int line) { if (!yes) { std::cerr << "route contract check failed at line " << line << '\n'; std::exit(1); } }
#define check(yes) check_at((yes),__LINE__)
std::uint64_t digest = 0;
const rcore_host_api* anchor_host = nullptr;
std::uint64_t state_hash() { return digest; }
std::uint64_t anchor_size() { return sizeof digest; }
rcore_result anchor_write(void* data, std::uint64_t size) {
    if (!data || size != sizeof digest) return RCORE_ERR_CONTENT;
    std::memcpy(data,&digest,sizeof digest);return RCORE_OK;
}
rcore_result anchor_read(const void* data, std::uint64_t size) {
    std::uint32_t width=0,height=0;
    if (!data || size != sizeof digest || !anchor_host ||
        !anchor_host->presentation_size(anchor_host->host_ctx,&width,&height) ||
        width != 1440 || height != 900) return RCORE_ERR_CONTENT;
    std::memcpy(&digest,data,sizeof digest);return RCORE_OK;
}
const rcore_option* new_options(std::uint32_t* count) {
    static rcore_option option{};
    option.struct_size=sizeof option;option.key="new_default";option.default_value="off";
    *count=1;return &option;
}
class TestSink : public Sink {
public:
    bool noisy = false;
    void log(std::uint32_t,const char*) override {}
    void event(const rcore_event&) override {}
    void frame(const rcore_frame&) override {}
    void audio(const std::int16_t*,std::uint32_t) override {}
    void audio_rate(std::uint32_t) override {}
    void input(std::uint32_t seat,rcore_pad& pad) override {
        if(noisy) { pad.connected=1;pad.buttons=0xffffffff;return; }
        pad.connected = seat % 2;
        pad.buttons = 1u << seat;
        for (unsigned i=0;i<RCORE_AXIS_COUNT;++i) pad.axes[i]=std::int16_t(-32768+int(seat*100+i));
    }
};
void drive(Route& route,HostSession& session) {
    const auto* host=session.host_api();
    check(host->wall_clock_us(host->host_ctx)>0);
    check(route.initial_saves());
    for(unsigned field=1;field<=4;++field) {
        check(route.begin(640,480));
        for(unsigned seat=0;seat<RCORE_MAX_SEATS;++seat) {
            rcore_pad p{}; host->input_get(host->host_ctx,seat,&p);
            check(p.connected==seat%2 && p.buttons==(1u<<seat));
            for(unsigned axis=0;axis<RCORE_AXIS_COUNT;++axis) check(p.axes[axis]==-32768+int(seat*100+axis));
        }
        check(host->wall_clock_us(host->host_ctx)>0);
        std::uint32_t pixel=0xff112233+field;
        rcore_frame frame{};frame.struct_size=sizeof frame;frame.width=frame.height=1;
        frame.stride=4;frame.pixel_format=RCORE_PIXEL_RGBA8;frame.pixels=&pixel;
        host->video_submit(host->host_ctx,&frame);
        std::int16_t samples[]={-32768,32767,-111,123};
        host->audio_push(host->host_ctx,samples,2);
        digest=field*7;
        check(route.end());
    }
    session.set_route(nullptr);
    check(route.finish(true));
}
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    rcore_pad live[RCORE_MAX_SEATS]{};live[0].connected=1;
    live[0].axes[0]=8000;check(!Route::live_input(live));
    live[0].axes[0]=8001;check(Route::live_input(live));
    live[0].axes[0]=-8001;check(Route::live_input(live));
    live[0].axes[0]=0;live[0].buttons=1;check(Route::live_input(live));
    live[0].connected=0;check(!Route::live_input(live));
    fs::path out=argv[1];fs::create_directories(out);
    fs::remove(out/"input.route");fs::remove(out/"input.route.partial");
    rcore_core_info info{};info.struct_size=sizeof info;info.core_id="route_fixture";info.core_version="1";
    rcore_core_api api{};api.struct_size=sizeof api;api.state_hash=state_hash;
    LoadedCore core;core.api=&api;core.info=&info;core.sha256="fixture";
    TestSink sink;
    rcore_save_region region{};region.struct_size=sizeof region;region.id="battery";region.size=16;region.erase_value=0x7a;
    RouteArgs args;args.record=out/"input.route";args.runner=fs::absolute(argv[0]);args.marks[2]="gameplay";
    HostSession recording(core,sink);std::string error;check(recording.set_options({},&error));
    recording.adopt_save_regions(&region,1,{});
    Route recorder(core,recording,args,out);check(recorder.open(argv[0],"",".",false));recording.set_route(&recorder);
    drive(recorder,recording);
    HostSession playing(core,sink);check(playing.set_options({},&error));playing.adopt_save_regions(&region,1,{});
    sink.noisy=true;
    std::memset(playing.save_regions()[0].data,0,16);
    args.play=args.record;args.record.clear();
    Route player(core,playing,args,out);check(player.open(argv[0],"",".",false));playing.set_route(&player);
    drive(player,playing);
    for(unsigned i=0;i<16;++i)check(playing.save_regions()[0].data[i]==0x7a);
    auto newer_api=api;newer_api.options=new_options;
    auto newer=core;newer.api=&newer_api;newer.sha256="newer_fixture";
    HostSession upgraded(newer,sink);check(upgraded.set_options({},&error));upgraded.adopt_save_regions(&region,1,{});
    args.allow_core_change=true;
    Route upgrade(newer,upgraded,args,out);check(upgrade.open(argv[0],"",".",false));upgraded.set_route(&upgrade);
    drive(upgrade,upgraded);
    HostSession changed(newer,sink);check(changed.set_options({{"new_default","on"}},&error));
    Route invalid(newer,changed,args,out);check(!invalid.open(argv[0],"",".",false));
    const auto reference=args.runner;
    args.runner=out/"alternate-runner";std::ofstream(args.runner) << "alternate host fixture";
    args.runner_reference=reference;
    HostSession reference_session(core,sink);check(reference_session.set_options({},&error));
    Route compatible(core,reference_session,args,out);check(compatible.open(argv[0],"",".",false));
    args.runner_reference=args.runner;
    Route wrong_reference(core,reference_session,args,out);check(!wrong_reference.open(argv[0],"",".",false));
    // The saved anchor's viewport can differ from its first recorded field.
    // A fresh core must see the saved size before init/load and unserialize.
    fs::remove(out/"anchor.route");fs::remove(out/"anchor.route.partial");
    auto anchor_info=info;anchor_info.capabilities|=RCORE_CAP_SAVESTATE;
    auto anchor_api=api;anchor_api.serialize_size=anchor_size;
    anchor_api.serialize=anchor_write;anchor_api.unserialize=anchor_read;
    auto anchor_core=core;anchor_core.info=&anchor_info;anchor_core.api=&anchor_api;
    TestSink anchor_sink;
    HostSession anchor_recording(anchor_core,anchor_sink);check(anchor_recording.set_options({},&error));
    anchor_recording.set_presentation_size(1440,900);
    RouteArgs anchor_args;anchor_args.runner=fs::absolute(argv[0]);anchor_args.record=out/"anchor.route";
    Route anchor_recorder(anchor_core,anchor_recording,anchor_args,out);
    check(anchor_recorder.open(argv[0],"",".",false));anchor_recording.set_route(&anchor_recorder);
    anchor_host=anchor_recording.host_api();check(anchor_recorder.capture_anchor());
    drive(anchor_recorder,anchor_recording);
    HostSession anchor_playing(anchor_core,anchor_sink);check(anchor_playing.set_options({},&error));
    anchor_sink.noisy=true;
    anchor_args.play=anchor_args.record;anchor_args.record.clear();
    Route anchor_player(anchor_core,anchor_playing,anchor_args,out);
    check(anchor_player.open(argv[0],"",".",false));anchor_playing.set_route(&anchor_player);
    anchor_host=anchor_playing.host_api();
    std::uint32_t width=0,height=0;
    check(anchor_host->presentation_size(anchor_host->host_ctx,&width,&height) && width==1440 && height==900);
    drive(anchor_player,anchor_playing);
    std::cout << "8 seats, 6 axes, clocks, save restoration, anchor viewport, state, audio and picture replay pass\n";
}
