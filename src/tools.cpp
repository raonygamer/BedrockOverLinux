#include "core.hpp"
#include <iostream>
#include <regex>
#include <sstream>
#include <sys/utsname.h>
#include <unistd.h>

namespace {
struct Tool {
    std::string name, repo, asset_pattern, primary;
};
const std::vector<Tool> definitions{
    {"xodus", "BedrockNative/xodus", R"(xodus-.*-linux-x86_64\.tar\.gz)", "xodus-cli"},
    {"winegdk", "BedrockNative/WineGDK", R"(wine-.*\.tar\.gz)", "wine"},
    {"xvdtool", "AmethystAPI/xvdtool", R"(xvdtool-linux-x64\.zip)", "xvdtool"}};
fs::path base(const Tool &t) { return root() / "tools" / t.name; }
std::string override_key(std::string bin) {
    for (auto &c : bin)
        c = c == '-' ? '_' : std::toupper(static_cast<unsigned char>(c));
    return "BEDROCK_" + bin;
}
fs::path find_binary(const fs::path &tree, const std::string &name) {
    for (const auto &e : fs::recursive_directory_iterator(tree)) {
        auto file = e.path().filename().string();
        bool matches = file == name || (name == "xvdtool" && file == "XVDTool");
        if (matches && e.is_regular_file() && access(e.path().c_str(), X_OK) == 0 &&
            (name != "wine" || e.path().parent_path().filename() == "bin"))
            return e.path();
    }
    throw std::runtime_error("missing executable in release: " + name);
}
Json latest(const Tool &t) {
    return Json::parse(checked({"curl", "-fsSL", "--proto", "=https", "--max-time", "60",
                                "https://api.github.com/repos/" + t.repo + "/releases/latest"},
                               {}, true));
}
void verify_executable(const Tool &tool, const std::string &binary) {
    auto arg = tool.name == "xvdtool" ? "-h" : (tool.name == "winegdk" ? "--version" : "--help");
    if (process({binary, arg}, tool.name == "xvdtool" ? xvdtool_environment() : Env{}, true).status != 0)
        throw std::runtime_error(binary + " failed its startup check");
}

void install(const Tool &t, const Json &release) {
    utsname os{};
    if (uname(&os) || std::string(os.machine) != "x86_64")
        throw std::runtime_error("release needs Linux x86_64");
    auto dir = base(t);
    std::string tag = release.at("tag_name");
    valid_name(tag);
    auto dest = dir / tag;
    if (!fs::exists(dest)) {
        auto stage = dir / (".install-" + uuid());
        fs::create_directories(stage / "tree");
        try {
            Json asset;
            for (auto &a : release.at("assets"))
                if (std::regex_match(a.at("name").get<std::string>(), std::regex(t.asset_pattern)))
                    asset = a;
            if (asset.is_null())
                throw std::runtime_error("Linux release asset unavailable");
            std::string url = asset.at("browser_download_url");
            std::string digest = asset.value("digest", "");
            if (!std::regex_match(digest, std::regex("sha256:[0-9a-f]{64}")))
                throw std::runtime_error("release asset missing SHA-256 digest");
            auto archive = (stage / "archive").string();
            checked({"curl", "-fL", "--proto", "=https", "--retry", "2", "-o", archive, url});
            if (checked({"sha256sum", archive}, {}, true).substr(0, 64) != digest.substr(7))
                throw std::runtime_error("release checksum mismatch");
            checked({"bsdtar", "--no-same-owner", "--no-same-permissions", "-xf", archive, "-C",
                     (stage / "tree").string()});
            // ZIP releases may omit Unix executable permissions.
            if (t.name == "xvdtool")
                for (auto &e : fs::recursive_directory_iterator(stage / "tree")) {
                    if (e.is_regular_file() &&
                        (e.path().filename() == "xvdtool" || e.path().filename() == "XVDTool"))
                        fs::permissions(e.path(), fs::perms::owner_exec, fs::perm_options::add);
                }
            find_binary(stage / "tree", t.primary);
            fs::rename(stage / "tree", dest);
            fs::remove_all(stage);
        } catch (...) {
            fs::remove_all(stage);
            throw;
        }
    }
    verify_executable(t, find_binary(dest, t.primary));
    if (t.name == "xodus")
        find_binary(dest, "xodus-service");
    save(dir / "current.json", {{"tag", tag}});
    std::cout << t.name << " " << tag << '\n';
}
} // namespace
std::string executable(const std::string &name) {
    if (auto path = getenv(override_key(name).c_str())) {
        if (!fs::path(path).is_absolute() || access(path, X_OK))
            throw std::runtime_error("invalid executable: " + std::string(path));
        return path;
    }
    for (const auto &t : definitions) {
        if (name != t.primary && !(t.name == "xodus" && name == "xodus-service") &&
            !(t.name == "winegdk" && (name == "wineserver" || name == "wineboot")))
            continue;
        auto state = base(t) / "current.json";
        if (fs::exists(state)) {
            std::string tag = read_json(state).at("tag");
            valid_name(tag);
            return find_binary(base(t) / tag, name);
        }
        // A plain system Wine must never stand in for WineGDK.
        std::string command = name == "wine" ? "winegdk" : name;
        std::istringstream paths(getenv("PATH") ? getenv("PATH") : "");
        for (std::string p; std::getline(paths, p, ':');) {
            auto full = fs::path(p) / command;
            if (full.is_absolute() && access(full.c_str(), X_OK) == 0)
                return full;
        }
        throw std::runtime_error(name + " unavailable; run bedrock tools install " + t.name);
    }
    throw std::runtime_error("unknown executable: " + name);
}
void tools_command(const std::string &action, const std::string &name) {
    if (action == "install" || action == "update") {
        if (!name.empty() && std::none_of(definitions.begin(), definitions.end(),
                                          [&](const Tool &tool) { return tool.name == name; }))
            throw std::runtime_error("unknown tool: " + name);
        install_dependencies(name);
    }
    bool found = false;
    bool usable = true;
    for (const auto &t : definitions) {
        if (!name.empty() && t.name != name)
            continue;
        found = true;
        auto path = base(t) / "current.json";
        std::string local = fs::exists(path) ? read_json(path).at("tag").get<std::string>() : "unmanaged";
        if (action == "list" || action == "info") {
            std::string binary;
            try {
                binary = executable(t.primary);
            } catch (const std::runtime_error &) {
                binary = "unavailable";
            }
            std::cout << t.name << "  " << local << "  " << binary << '\n';
            continue;
        }
        auto release = latest(t);
        if (action == "check") {
            std::string health;
            try {
                auto binary = executable(t.primary);
                verify_executable(t, binary);
                health = "ready";
                if (t.name == "xodus")
                    executable("xodus-service");
            } catch (const std::runtime_error &e) {
                health = e.what();
            }
            if (health != "ready")
                usable = false;
            std::cout << t.name << "  " << health << "  local=" << local
                      << "  latest=" << release.at("tag_name").get<std::string>() << '\n';
        } else {
            Lock lock(base(t) / ".install.lock");
            install(t, release);
        }
    }
    if (!usable)
        throw std::runtime_error("one or more external tools are unavailable");
    if (!found)
        throw std::runtime_error("unknown tool: " + name);
}
