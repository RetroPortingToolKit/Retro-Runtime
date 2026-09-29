#include "runner_probe.hpp"

#include "transport.hpp"

#include <chrono>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

namespace retro::corelink {

const char* runner_file_name() {
#if defined(_WIN32)
    return "retro-core-runner.exe";
#else
    return "retro-core-runner";
#endif
}

bool probe_runner(const fs::path& runner, RunnerVersion& out, std::string* error,
                  int timeout_ms) {
    auto fail = [&](const std::string& m) {
        if (error) *error = path_utf8(runner) + ": " + m;
        return false;
    };
    std::error_code ec;
    if (!fs::is_regular_file(runner, ec)) return fail("not found");

    std::mt19937_64 rng(static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path report = fs::temp_directory_path(ec) /
                            ("retro-runner-version-" + std::to_string(rng()) + ".txt");
    SpawnSpec spec;
    spec.args = {path_utf8(runner), "--version"};
    spec.log = report;
    std::string err;
    const auto code = run_to_completion(spec, timeout_ms, &err);
    std::map<std::string, std::string> fields;
    {
        std::ifstream in(report);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto sp = line.find(' ');
            if (sp != std::string::npos) fields[line.substr(0, sp)] = line.substr(sp + 1);
        }
    }
    fs::remove(report, ec);
    if (!code) return fail(err);
    if (*code != 0) return fail("--version exited " + std::to_string(*code));

    auto num = [&](const std::string& key, std::uint32_t& v) {
        const auto it = fields.find(key);
        if (it == fields.end()) return false;
        try {
            v = static_cast<std::uint32_t>(std::stoul(it->second));
        } catch (...) {
            return false;
        }
        return true;
    };
    RunnerVersion v;
    v.version = fields["version"];
    v.commit = fields["commit"];
    v.gl = fields["gl"] == "1";
    if (fields.count("game_package") && !num("game_package", v.game_package)) {
        return fail("--version printed an unreadable game_package line");
    }
    if (fields.count("describe") && !num("describe", v.describe)) {
        return fail("--version printed an unreadable describe line");
    }
    if (fields.count("netplay") && !num("netplay", v.netplay)) {
        return fail("--version printed an unreadable netplay line");
    }
    if (fields.count("transfer_pak_seats") && !num("transfer_pak_seats", v.transfer_pak_seats)) {
        return fail("--version printed an unreadable transfer_pak_seats line");
    }
    const std::string link = fields["link_protocol"];
    const auto dot = link.find('.');
    bool ok = !v.version.empty() && dot != std::string::npos && num("rcore_abi_major", v.abi_major) &&
              num("rcore_draft_revision", v.draft_revision);
    if (ok) {
        try {
            v.link_major = static_cast<std::uint32_t>(std::stoul(link.substr(0, dot)));
            v.link_minor = static_cast<std::uint32_t>(std::stoul(link.substr(dot + 1)));
        } catch (...) {
            ok = false;
        }
    }
    if (!ok) return fail("--version printed no version report (a runner from before 0.1.0?)");
    out = v;
    return true;
}

} // namespace retro::corelink
