#include "core.hpp"
#include "activation.hpp"
#include <fstream>
#include <fcntl.h>
#include <sys/wait.h>
#include <iostream>
#include <pugixml.hpp>
#include <pwd.h>
#include <regex>
#include <sstream>
#include <set>
#include <strings.h>
#include <unistd.h>

namespace {
std::string id(const Json &obj) { return obj.at("uuid").get<std::string>(); }
Json prefix(const std::string &selector, bool create) {
    auto rows = records("prefix-registry");
    for (auto &p : rows)
        if (p["uuid"] == selector || p["name"] == selector)
            return p;
    if (!create)
        throw std::runtime_error("unknown prefix: " + selector);
    valid_name(selector);
    fs::create_directories(root() / "prefixes" / selector);
    Json p{{"uuid", uuid()}, {"name", selector}};
    put("prefix-registry", p);
    return p;
}
fs::path shared(const Json &p) { return root() / "profiles" / id(p) / "Users/Shared/games/com.mojang"; }
Json new_profile(const std::string &name, const Json &account, const Json &px) {
    return {{"uuid", uuid()},
            {"name", name},
            {"account", account.at("uuid")},
            {"prefix", px.at("uuid")},
            {"version", nullptr}};
}
std::vector<Json> initialize(const Json &account) {
    auto profiles = root() / "profiles";
    fs::create_directories(profiles);
    // A renamed record means both confirmations completed, even after an interrupted cleanup.
    for (const auto &entry : fs::directory_iterator(profiles)) {
        auto uuid = entry.path().stem().string();
        if (entry.path().extension() == ".deleting" && is_uuid(uuid)) {
            fs::remove_all(profiles / uuid);
            fs::remove(entry.path());
        }
    }
    auto rows = records("profiles");
    for (auto &p : rows) {
        if (p.contains("prefix"))
            continue;
        // The initial scaffold stored version labels; keep its original file for recovery.
        auto backup = root() / "legacy" / (id(p) + ".json");
        if (!fs::exists(backup))
            save(backup, p);
        auto converted = new_profile(p.at("name"), account, prefix("default", true));
        converted["uuid"] = p.at("uuid");
        if (p.contains("version") && p["version"].is_string()) {
            try {
                converted["version"] = version_resolve(p["version"]).at("uuid");
            } catch (const std::runtime_error &) {
                std::cerr << "Legacy version pending; original saved in " << backup << '\n';
            }
        }
        put("profiles", converted);
        p = std::move(converted);
    }
    auto seeded = profiles / ".defaults-created";
    if (fs::exists(seeded)) return rows;
    auto px = prefix("default", true);
    for (std::string name : {"release", "preview"}) {
        auto matches = [&](const Json &r) { return r.at("name") == name; };
        if (std::none_of(rows.begin(), rows.end(), matches))
            put("profiles", new_profile(name, account, px));
    }
    write_file(seeded, "");
    return records("profiles");
}
void select_version(Json &profile, const std::string &value) {
    if (value == "latest_release" || value == "latest_preview") {
        if (profile.value("track", "") != value) profile["version"] = nullptr;
        profile["track"] = value;
    } else {
        profile["version"] = version_resolve(value).at("uuid");
        profile.erase("track");
    }
}
bool busy(const Json &px) {
    try {
        Lock probe(root() / "locks" / (id(px) + ".prefix"), false, false);
        return false;
    } catch (const std::system_error &e) {
        if (e.code().value() == EWOULDBLOCK)
            return true;
        throw;
    }
}
// Take one snapshot per CLI invocation. The worker acquires the actual locks.
struct LaunchChecks {
    std::map<std::string, bool> prefixes_busy;
    std::optional<std::string> tool_status;

    std::string availability(const Json &profile) {
        const std::string prefix_id = profile.at("prefix");
        auto found = prefixes_busy.find(prefix_id);
        if (found == prefixes_busy.end())
            found = prefixes_busy.emplace(prefix_id, busy(prefix(prefix_id, false))).first;
        if (found->second) return "prefix busy";
        if (!fs::exists(record_path("accounts", profile.at("account")))) return "signed out";
        if (!profile.contains("track")) {
            if (profile.at("version").is_null()) return "select a version";
            try {
                version_resolve(profile.at("version"));
            } catch (const std::exception &e) {
                return e.what();
            }
        }
        if (!tool_status) {
            try {
                executable("wine");
                executable("xodus-cli");
                executable("xodus-service");
                tool_status = "ready";
            } catch (const std::exception &e) {
                tool_status = e.what();
            }
        }
        return *tool_status;
    }
};
// Wine initializes its services while Xodus prepares the licensed executable.
class WineWarmup {
    pid_t pid;
    int input;
public:
    WineWarmup(const std::string &wine, const Env &env) {
        int channel[2];
        if (pipe2(channel, O_CLOEXEC)) throw std::system_error(errno, std::generic_category(), "pipe");
        int quiet = open("/dev/null", O_WRONLY | O_CLOEXEC);
        try {
            if (quiet < 0) throw std::system_error(errno, std::generic_category(), "/dev/null");
            pid = spawn_process({wine, "cmd", "/d", "/q"}, env, channel[0], quiet, quiet);
        } catch (...) {
            if (quiet >= 0) close(quiet);
            close(channel[0]);
            close(channel[1]);
            throw;
        }
        close(quiet);
        close(channel[0]);
        input = channel[1];
    }
    WineWarmup(const WineWarmup &) = delete;
    ~WineWarmup() {
        close(input); // EOF exits cmd normally, including when Xodus fails to launch.
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
};
bool native_profile_mapping(const fs::path &wine) {
    auto info = fs::canonical(wine).parent_path().parent_path() / "share/wine/wine.inf";
    std::ifstream input(info);
    std::string line;
    return std::getline(input, line) && line == ";; WineGDK capability: bedrock-data-dir-v1";
}

// Only retire links that an older bedrock build could have created. A link
// owned by the user needs an explicit migration before changing its meaning.
bool legacy_storage_link(const fs::path &link) {
    if (!fs::is_symlink(link)) return false;
    const auto target = fs::weakly_canonical(link.parent_path() / fs::read_symlink(link));
    const auto profile = target.parent_path();
    if (target.filename() != link.filename() || !is_uuid(profile.filename().string()) ||
        profile.parent_path() != fs::weakly_canonical(root() / "profiles"))
        throw std::runtime_error("unmanaged storage link: " + link.string());
    return true;
}

void bind_storage(const fs::path &source, const fs::path &dest, const fs::path &owner) {
    fs::create_directories(source);
    fs::create_directories(dest.parent_path());
    if (fs::is_symlink(dest)) {
        auto old = fs::weakly_canonical(dest.parent_path() / fs::read_symlink(dest)).lexically_relative(owner);
        if (old.empty() || *old.begin() == "..")
            throw std::runtime_error("unmanaged storage link: " + dest.string());
    } else if (fs::exists(dest)) {
        if (!fs::is_directory(dest) || !fs::is_empty(dest))
            throw std::runtime_error("existing game data requires manual import: " + dest.string());
        fs::remove(dest);
    }
    auto tmp = dest.string() + "." + uuid();
    fs::create_directory_symlink(source, tmp);
    fs::rename(tmp, dest);
}
void ensure_game_config(const fs::path &directory, const Json &version) {
    auto target = directory / "MicrosoftGame.config";
    fs::path manifest_path;
    for (const auto &entry : fs::directory_iterator(directory)) {
        auto name = entry.path().filename();
        if (strcasecmp(name.c_str(), "MicrosoftGame.config") == 0 && entry.is_regular_file())
            return;
        if (strcasecmp(name.c_str(), "AppxManifest.xml") == 0 && entry.is_regular_file())
            manifest_path = entry.path();
    }
    if (manifest_path.empty())
        throw std::runtime_error("missing AppxManifest.xml in " + directory.string());
    pugi::xml_document manifest, doc;
    auto parsed = manifest.load_file(manifest_path.c_str());
    if (!parsed)
        throw std::runtime_error(manifest_path.string() + ": " + parsed.description());
    auto identity = manifest.child("Package").child("Identity");
    bool preview = version.at("channel") == "preview";
    auto game = doc.append_child("Game");
    game.append_attribute("configVersion") = "1";
    auto dest = game.append_child("Identity");
    for (auto key : {"Name", "Publisher", "Version"}) {
        if (!identity.attribute(key))
            throw std::runtime_error("invalid package identity");
        dest.append_attribute(key) = identity.attribute(key).value();
    }
    game.append_child("TitleId").text() = preview ? "717D695F" : "35760C07";
    game.append_child("MSAAppId").text() = preview ? "00000000403FC600" : "0000000040159362";
    game.append_child("StoreId").text() = preview ? "9P5X4QVLC2XR" : "9NBLGGH2JHXJ";
    auto exe = game.append_child("ExecutableList").append_child("Executable");
    exe.append_attribute("Name") = fs::path(version.at("executable").get<std::string>()).filename().c_str();
    exe.append_attribute("TargetDeviceFamily") = "PC";
    exe.append_attribute("Id") = "Game";
    game.append_child("ProtocolList").append_child("Protocol").append_attribute("Name") =
        preview ? "minecraft-preview" : "minecraft";
    game.append_child("Resources").append_child("Resource").append_attribute("Language") = "en-us";
    game.append_child("AdvancedUserModel").text() = "true";
    game.append_child("MSAFullTrust").text() = "true";
    std::ostringstream out;
    doc.save(out);
    write_file(target, out.str());
}
} // namespace
void profile_startup(const std::string &selector, const std::string &setting, const std::string &action,
                     const Args &values) {
    Lock metadata(root() / "locks/metadata");
    auto profile = lookup(initialize(default_account()), selector);
    auto configured = profile.value(setting, setting == "env" ? Json::object() : Json::array());
    if (action == "list" || action == "info") {
        std::cout << configured.dump(2) << '\n';
        return;
    }
    Lock edit(root() / "locks" / (id(profile) + ".profile"), false, false);
    if (setting == "env") {
        const auto &key = values.at(0);
        static const std::regex valid_key("[A-Za-z_][A-Za-z0-9_]*");
        if (!std::regex_match(key, valid_key)) throw std::runtime_error("invalid environment key: " + key);
        if (action == "unset") configured.erase(key);
        else {
            if (key == "WINEPREFIX" || key == "WINESERVER" || key == "WINEGDK_BEDROCK_DATA_DIR" || key == "WINEGDK_FILE_ACTIVATION_DIR" || key.rfind("XODUS_", 0) == 0)
                throw std::runtime_error("variable managed by the selected account or prefix: " + key);
            configured[key] = values.at(1);
        }
    } else if (action == "clear") configured.clear();
    else {
        if (values.empty() || (setting != "args" && values.front().empty()))
            throw std::runtime_error("missing command");
        configured = values;
    }
    if (configured.empty()) profile.erase(setting);
    else profile[setting] = configured;
    put("profiles", profile);
}

void worker(const std::string &selector, const std::string &session_id) {
    if (!is_uuid(selector))
        throw std::runtime_error("invalid worker UUID");
    Lock profile_lock(root() / "locks" / (selector + ".profile"), false, false);
    auto p = lookup(records("profiles"), selector);
    auto px = prefix(p.at("prefix"), false);
    Lock lease(root() / "locks" / (id(px) + ".prefix"), false, false);
    ActivationSession session(session_id);
    save(root() / "locks" / (id(px) + ".owner"), {{"profile", id(p)}, {"activation", session_id}});
    auto a = lookup(records("accounts"), p.at("account"));
    if (p.contains("track")) update_profile_version(p, a);
    if (p.at("version").is_null())
        throw std::runtime_error("select a version first");
    auto version_id = p.at("version").get<std::string>();
    if (!is_uuid(version_id))
        throw std::runtime_error("invalid version UUID");
    Lock install(root() / "locks" / (version_id + ".version"), true, false);
    auto version = version_resolve(version_id);
    Lock usage(root() / "locks" / (id(a) + ".usage"), true);
    ensure_service(a);
    auto env = account_env(a);
    auto dir = root() / "prefixes" / px.at("name").get<std::string>();
    auto wine = executable("wine");
    auto server = fs::path(wine).parent_path() / "wineserver";
    env["WINESERVER"] =
        !getenv("BEDROCK_WINESERVER") && fs::exists(server) ? server.string() : executable("wineserver");
    env["WINEPREFIX"] = dir.string();
    env["WINEBOOT_HIDE_DIALOG"] = "1";
    const auto prelaunch = p.value("prelaunch", Args{});
    const auto wrapper = p.value("wrapper", Args{});
    // Custom launch commands can depend on a populated registry and drive_c.
    // Direct launches use Wine's automatic first-run initialization.
    if ((!prelaunch.empty() || !wrapper.empty()) && !fs::exists(dir / "system.reg"))
        checked({wine, "wineboot", "-u"}, env);
    auto user = getpwuid(getuid());
    if (!user)
        throw std::runtime_error("Wine user unavailable");
    auto appdata = dir / "drive_c/users" / user->pw_name / "AppData";
    bool preview = version.at("channel") == "preview";
    auto data = root() / "profiles" / id(p);
    fs::create_directories(shared(p));
    auto roaming = appdata / "Roaming";
    auto local_state = appdata / "Local/Packages" /
        (preview ? "Microsoft.MinecraftWindowsBeta_8wekyb3d8bbwe" : "Microsoft.MinecraftUWP_8wekyb3d8bbwe") /
        "LocalState";
    bool native = native_profile_mapping(wine);
    if (native) {
        std::vector<fs::path> legacy_links;
        for (auto channel : {"Minecraft Bedrock", "Minecraft Bedrock Preview"}) {
            auto folder = roaming / channel;
            if (fs::is_symlink(folder)) throw std::runtime_error("unmanaged storage link: " + folder.string());
            fs::create_directories(folder); // The game directory is visible when enumerating Roaming.
            auto users = folder / "Users";
            if (legacy_storage_link(users)) legacy_links.push_back(users);
            else if (fs::exists(users) && (!fs::is_directory(users) || !fs::is_empty(users)))
                throw std::runtime_error("existing game data requires manual import: " + users.string());
        }
        for (auto package : {"Microsoft.MinecraftUWP_8wekyb3d8bbwe", "Microsoft.MinecraftWindowsBeta_8wekyb3d8bbwe"}) {
            auto path = appdata / "Local/Packages" / package / "LocalState";
            if (legacy_storage_link(path)) legacy_links.push_back(path);
        }
        for (const auto &path : legacy_links) fs::remove(path);
    } else {
        bind_storage(data / "Users", roaming / (preview ? "Minecraft Bedrock Preview" : "Minecraft Bedrock") / "Users",
                     root() / "profiles");
        bind_storage(data / "LocalState", local_state, root() / "profiles");
    }
    auto game = root() / "versions/client" / id(version) / "game";
    auto relative = fs::path(version.at("executable").get<std::string>());
    ensure_game_config((game / relative).parent_path(), version);
    std::string exe = relative.string();
    std::replace(exe.begin(), exe.end(), '/', '\\');
    auto cli = executable("xodus-cli");
    Args run{cli, "run", game.string(), wine, "--exe", exe};
    if (version.at("installedBy") == a.at("uuid"))
        run.push_back("--offline-license");
    else
        checked({cli, "check-ownership", "9NBLGGH2JHXJ"}, env);
    auto profile_env = p.value("env", Env{});
    for (const auto &[key, value] : profile_env) {
        if (key != "WINEPREFIX" && key != "WINESERVER" && key.rfind("XODUS_", 0) != 0) env[key] = value;
    }
    env["WINEGDK_BEDROCK_DATA_DIR"] = native ? data.string() : "";
    env["WINEGDK_FILE_ACTIVATION_DIR"] = wine_file_path(session.directory);
    env["BEDROCK_PROFILE_DIR"] = data.string();
    env["BEDROCK_GAME_DIR"] = game.string();
    auto arguments = p.value("args", Args{});
    if (const auto *file = getenv("BEDROCK_GAME_IMPORT_FILE"); file && *file) {
        // Wine creates the Z: mapping to / when it initializes a standard prefix.
        const auto drive = dir / "dosdevices/z:";
        if (fs::exists(dir / "system.reg") && (!fs::exists(drive) || !fs::equivalent(drive, "/")))
            throw std::runtime_error("in-game imports require the Wine prefix Z: drive to map to /");
        arguments.push_back(wine_file_path(file));
    }
    if (!arguments.empty()) {
        run.push_back("--");
        run.insert(run.end(), arguments.begin(), arguments.end());
    }
    fs::current_path(data);
    if (!prelaunch.empty()) checked(prelaunch, env);
    if (!wrapper.empty()) {
        const std::string token = "%command%";
        if (wrapper.size() == 1 && wrapper.front() != token && wrapper.front().find(token) != std::string::npos) {
            std::string script = wrapper.front();
            for (size_t pos = 0; (pos = script.find(token, pos)) != std::string::npos; pos += 4)
                script.replace(pos, token.size(), "\"$@\"");
            run.insert(run.begin(), {"/bin/sh", "-c", script, "bedrock-wrapper"});
        } else {
            Args expanded;
            bool inserted = false;
            for (const auto &word : wrapper) {
                if (word == token) {
                    expanded.insert(expanded.end(), run.begin(), run.end());
                    inserted = true;
                } else expanded.push_back(word);
            }
            if (!inserted) expanded.insert(expanded.end(), run.begin(), run.end());
            run = std::move(expanded);
        }
    }
    std::optional<WineWarmup> warmup;
    if (wrapper.empty() && !getenv("BEDROCK_WINE") && !getenv("BEDROCK_WINESERVER") &&
        fs::is_regular_file(server.string() + ".bedrock-original"))
        warmup.emplace(wine, env);
    auto result = process(run, env);
    warmup.reset();
    checked({env.at("WINESERVER"), "-w"}, env);
    if (result.status)
        throw std::runtime_error("game exited " + std::to_string(result.status));
}
void profile_command(const std::string &action, const std::string &selector, const std::string &value) {
    Json profile;
    bool forward = false;
    if (action == "delete") {
        {
            Lock metadata(root() / "locks/metadata");
            profile = lookup(initialize(default_account()), selector);
        }
        auto uuid = id(profile);
        Lock lease(root() / "locks" / (uuid + ".profile"), false, false);
        const auto record = record_path("profiles", uuid);
        const auto data = record.parent_path() / uuid;
        if (!fs::exists(record)) throw std::runtime_error("profile already deleted");
        auto name = profile.at("name").get<std::string>();
        std::cerr << "Permanently delete profile " << name << " (" << uuid << ") and its worlds, packs and settings in "
                  << data << "?\n";
        if (confirm("Continue?")) {
            std::cerr << "Type the exact profile name to confirm: ";
            std::string reply;
            if (std::getline(std::cin, reply) && reply == name) {
                Lock metadata(root() / "locks/metadata");
                const auto import_config = root() / "import.json";
                if (fs::exists(import_config) && read_json(import_config).at("profile") == uuid)
                    fs::remove(import_config);
                auto pending = record.parent_path() / (uuid + ".deleting");
                fs::rename(record, pending);
                fs::remove_all(data);
                fs::remove(pending);
                std::cout << "Deleted profile " << name << '\n';
                return;
            }
        }
        std::cout << "Deletion cancelled.\n";
        return;
    }
    {
        Lock lock(root() / "locks/metadata");
        if (action == "prefix list") {
            prefix("default", true);
            for (auto &p : records("prefix-registry")) {
                bool in_use = busy(p);
                std::cout << p.at("name").get<std::string>() << " " << id(p)
                          << (in_use ? " busy" : " available");
                auto owner = root() / "locks" / (id(p) + ".owner");
                if (in_use && fs::exists(owner))
                    std::cout << " " << read_json(owner).at("profile").get<std::string>();
                std::cout << '\n';
            }
            return;
        }
        auto account = default_account();
        auto rows = initialize(account);
        if (action == "list") {
            LaunchChecks checks;
            bool color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
            for (auto &p : rows) {
                auto state = checks.availability(p);
                std::cout << (color ? (state == "ready" ? "\033[32m" : "\033[31m") : "")
                          << p.at("name").get<std::string>() << " " << id(p) << " prefix=" << p["prefix"]
                          << " version=" << (p.contains("track") ? p.at("track") : p.at("version")) << " " << state << (color ? "\033[0m" : "") << '\n';
            }
            return;
        }
        if (action == "create") {
            valid_name(selector);
            for (auto s : {"list", "create", "delete"})
                if (selector == s)
                    throw std::runtime_error("reserved profile name");
            for (auto &p : rows)
                if (p.at("name") == selector)
                    throw std::runtime_error("profile exists");
            profile = new_profile(selector, account, prefix("default", true));
            if (!value.empty())
                select_version(profile, value);
            put("profiles", profile);
            std::cout << id(profile) << '\n';
            return;
        }
        profile = lookup(rows, selector);
        if (action == "info") {
            std::cout << profile.dump(2) << '\n';
            return;
        }
        if (action == "account info") {
            std::cout << lookup(records("accounts"), profile.at("account")).dump(2) << '\n';
            return;
        }
        if (action == "version info") {
            if (profile.contains("track")) {
                Json info = profile.at("version").is_null() ? Json::object() : version_resolve(profile.at("version"));
                info["track"] = profile.at("track");
                info["uuid"] = profile.at("version");
                std::cout << info.dump(2) << '\n';
            } else if (profile.at("version").is_null())
                std::cout << "No version selected\n";
            else
                std::cout << version_resolve(profile.at("version")).dump(2) << '\n';
            return;
        }
        if (action == "prefix info") {
            std::cout << prefix(profile.at("prefix"), false).dump(2) << '\n';
            return;
        }
        if (action != "launch") {
            Lock edit(root() / "locks" / (id(profile) + ".profile"), false, false);
            if (action == "prefix set")
                profile["prefix"] = prefix(value, true).at("uuid");
            else if (action == "account set")
                profile["account"] = lookup(records("accounts"), value).at("uuid");
            else if (action == "version set")
                select_version(profile, value);
            else
                throw std::runtime_error("unknown profile action");
            put("profiles", profile);
            return;
        }
        auto state = LaunchChecks{}.availability(profile);
        forward = state == "prefix busy" && !value.empty();
        if (state != "ready" && !forward) throw std::runtime_error(state);
    }
    if (forward) {
        forward_file(profile, value);
        return;
    }
    const auto session_id = uuid();
    auto caller = "bedrock-client-" + uuid() + ".scope";
    Args launch{"systemd-run",
                "--user",
                "--scope",
                "--quiet",
                "--collect",
                "--unit=" + caller,
                "systemd-run",
                "--user",
                "--wait",
                "--quiet",
                "--pipe",
                "--collect",
                "--service-type=exec",
                "--property=RuntimeDirectory=bedrockoverlinux/import/" + session_id,
                "--property=RuntimeDirectoryMode=0700",
                "--property=BindsTo=" + caller,
                "--property=After=" + caller,
                "--unit=bedrock-game-" + profile.at("prefix").get<std::string>() + ".service"};
    for (auto key : {"HOME", "PATH", "XDG_CONFIG_HOME", "XDG_RUNTIME_DIR", "DISPLAY", "WAYLAND_DISPLAY",
                     "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "LD_LIBRARY_PATH", "BEDROCK_WINE",
                     "BEDROCK_WINESERVER", "BEDROCK_XODUS_CLI", "BEDROCK_XODUS_SERVICE"}) {
        if (auto val = getenv(key))
            launch.push_back("--setenv=" + std::string(key) + "=" + val);
    }
    launch.push_back("--setenv=BEDROCK_GAME_IMPORT_FILE=" + value);
    launch.insert(launch.end(), {fs::read_symlink("/proc/self/exe").string(), "_run", id(profile), session_id});
    checked(launch);
}
void env_command(const std::string &selector) {
    Json p;
    {
        Lock lock(root() / "locks/metadata");
        p = lookup(initialize(default_account()), selector);
    }
    auto path = shared(p);
    fs::create_directories(path);
    if (path.string().find_first_of("\r\n\"\\$`") != std::string::npos)
        throw std::runtime_error("path cannot be represented in /etc/environment");
    // Keep the privileged edit limited to this one key and recheck under the lock.
    std::string script = R"SH(
set -eu
file=/etc/environment
if [ -L "$file" ]; then echo "refusing symlink /etc/environment" >&2; exit 1; fi
if [ "$2" = set ] || { [ -f "$file" ] && grep -Eq '^[[:space:]]*(export[[:space:]]+)?COM_MOJANG[[:space:]]*=' "$file"; }; then
    printf 'Replace the system-wide COM_MOJANG in /etc/environment? [y/N] ' >&2
    read -r reply || exit 1
    case "$reply" in y|Y) ;; *) exit 1;; esac
fi
# Hooks read only the value written below, so changing profiles has one commit point.
work=$(mktemp -d /etc/.bedrock-environment.XXXXXX)
trap 'rm -rf -- "$work"' EXIT
mkdir -p /etc/profile.d /etc/fish/conf.d
cat > "$work/profile.sh" <<'POSIX'
_bedrock_mojang=$(sed -n 's/^COM_MOJANG="\(.*\)"$/\1/p' /etc/environment)
if [ -n "$_bedrock_mojang" ]; then export COM_MOJANG="$_bedrock_mojang"; fi
unset _bedrock_mojang
POSIX
cat > "$work/profile.fish" <<'FISH'
set -l bedrock_mojang (string replace --filter --regex '^COM_MOJANG="(.*)"$' '$1' < /etc/environment)
if test (count $bedrock_mojang) -eq 1
    set -gx COM_MOJANG "$bedrock_mojang"
end
FISH
chmod 644 "$work/profile.sh" "$work/profile.fish"
mv -fT "$work/profile.sh" /etc/profile.d/bedrockoverlinux.sh
mv -fT "$work/profile.fish" /etc/fish/conf.d/bedrockoverlinux.fish
hook='[ ! -r /etc/profile.d/bedrockoverlinux.sh ] || . /etc/profile.d/bedrockoverlinux.sh'
for rc in /etc/bash.bashrc /etc/bashrc; do
    if [ -f "$rc" ] && ! grep -Fxq "$hook" "$rc"; then
        printf '\n%s\n' "$hook" >> "$rc"
    fi
done
tmp="$work/environment"
if [ -f "$file" ]; then
    cp --preserve=all "$file" "$tmp"
    awk '!/^[[:space:]]*(export[[:space:]]+)?COM_MOJANG[[:space:]]*=/' "$file" > "$tmp"
else
    touch "$tmp"
    chmod 644 "$tmp"
fi
printf 'COM_MOJANG="%s"\n' "$1" >> "$tmp"
mv -fT "$tmp" "$file"
)SH";
    Args cmd{"/usr/bin/flock", "/etc/.bedrock-environment.lock",      "/bin/sh", "-c", script, "bedrock",
             path.string(),    getenv("COM_MOJANG") ? "set" : "unset"};
    if (geteuid() != 0) {
        bool use_sudo = isatty(STDIN_FILENO) && access("/usr/bin/sudo", X_OK) == 0;
        cmd.insert(cmd.begin(), !use_sudo && access("/usr/bin/pkexec", X_OK) == 0
                                    ? "/usr/bin/pkexec" : "/usr/bin/sudo");
    }
    checked(cmd);
    std::cout << "COM_MOJANG=" << path.string() << " (new Bash, Fish and login sessions)\n";
}

namespace {
fs::path applications_directory() {
    const char *base = getenv("XDG_DATA_HOME");
    if (base && fs::path(base).is_absolute()) return fs::path(base) / "applications";
    const char *home = getenv("HOME");
    if (!home || !fs::path(home).is_absolute()) throw std::runtime_error("HOME unavailable");
    return fs::path(home) / ".local/share/applications";
}

std::string desktop_argument(const std::string &value) {
    // Desktop entries unescape string values first, then parse the quoted Exec arguments.
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '\\') quoted += "\\\\\\\\";
        else if (c == '"' || c == '$' || c == '`') { quoted += "\\\\"; quoted += c; }
        else if (c == '%') quoted += "%%";
        else if (static_cast<unsigned char>(c) < 32) throw std::runtime_error("control character in launcher path");
        else quoted += c;
    }
    return quoted + '"';
}

bool managed_shortcut(const fs::path &path, const std::string &uuid) {
    if (!fs::is_regular_file(fs::symlink_status(path))) return false;
    std::ifstream file(path);
    std::string line;
    const auto exec_prefix = "Exec=/usr/bin/env " + desktop_argument("XDG_CONFIG_HOME=" + root().parent_path().string()) + " ";
    bool matches_profile = false, matches_config = false;
    while (std::getline(file, line)) {
        if (line == "X-BedrockOverLinux-Profile=" + uuid) matches_profile = true;
        if (line.compare(0, exec_prefix.size(), exec_prefix) == 0) matches_config = true;
    }
    return matches_profile && matches_config;
}
std::string shortcut_icon(const Json &profile) {
    const auto clients = root() / "versions/client";
    std::vector<fs::path> installations;
    if (profile.at("version").is_string()) {
        const std::string version = profile.at("version");
        if (is_uuid(version)) installations.push_back(clients / version);
    }
    if (profile.contains("track") && fs::exists(clients)) {
        const auto channel = profile.at("track") == "latest_preview" ? "preview" : "release";
        std::vector<fs::path> same_channel;
        for (const auto &entry : fs::directory_iterator(clients)) {
            if (!is_uuid(entry.path().filename().string())) continue;
            auto metadata = entry.path() / "installation.json";
            if (fs::is_regular_file(metadata) && read_json(metadata).value("channel", "") == channel)
                same_channel.push_back(entry.path());
        }
        std::sort(same_channel.begin(), same_channel.end());
        installations.insert(installations.end(), same_channel.begin(), same_channel.end());
    }
    for (const auto &path : installations) {
        if (!fs::is_regular_file(path / "installation.json")) continue;
        auto game = path / "game";
        auto folder = game / fs::path(read_json(path / "installation.json").at("executable").get<std::string>()).parent_path();
        if (!fs::is_directory(folder)) continue;
        std::vector<std::string> icons;
        for (const auto &entry : fs::directory_iterator(folder)) {
            if (strcasecmp(entry.path().filename().c_str(), "AppxManifest.xml")) continue;
            pugi::xml_document doc;
            if (doc.load_file(entry.path().c_str())) {
                auto visual = doc.select_node("//*[local-name()='VisualElements']").node();
                for (auto key : {"Square150x150Logo", "Square44x44Logo", "Logo"})
                    if (auto asset = visual.attribute(key)) icons.emplace_back(asset.value());
            }
            break;
        }
        icons.insert(icons.end(), {"Logo.png", "LargeLogo.png", "StoreLogo.png", "SmallLogo.png"});
        for (auto icon : icons) {
            std::replace(icon.begin(), icon.end(), '\\', '/');
            auto full = fs::weakly_canonical(folder / icon);
            auto relative = full.lexically_relative(fs::weakly_canonical(game));
            if (relative.empty() || *relative.begin() == ".." || !fs::is_regular_file(full)) continue;
            std::string escaped;
            for (char c : full.string()) {
                if (c == '\\') escaped += "\\\\";
                else if (c == '\n') escaped += "\\n";
                else if (c == '\r') escaped += "\\r";
                else escaped += c;
            }
            return escaped;
        }
    }
    return "applications-games";
}

void write_shortcut(const fs::path &path, const Json &profile, const std::string &uuid) {
    auto name = profile.at("name").get<std::string>();
    valid_name(name);
    auto command = "/usr/bin/env " + desktop_argument("XDG_CONFIG_HOME=" + root().parent_path().string())
                 + " " + desktop_argument(fs::read_symlink("/proc/self/exe").string()) + " profile " + uuid + " launch";
    write_file(path, "[Desktop Entry]\nType=Application\nName=Minecraft Bedrock - " + name
               + "\nExec=" + command + "\nIcon=" + shortcut_icon(profile) + "\nTerminal=false\nCategories=Game;\n"
               + "X-BedrockOverLinux-Profile=" + uuid + "\n");
}
}

void shortcut_command(const std::string &action, const std::string &selector) {
    Lock lock(root() / "locks/metadata");
    const auto directory = applications_directory();
    const std::string prefix = "bedrockoverlinux-";
    const auto catalog = root() / "shortcuts.json";
    std::set<std::string> shortcuts;
    const bool has_catalog = fs::exists(catalog);
    if (has_catalog) {
        auto data = read_json(catalog);
        if (data.at("schema") != 1) throw std::runtime_error("unsupported shortcut catalog");
        shortcuts = data.at("profiles").get<std::set<std::string>>();
        for (const auto &uuid : shortcuts)
            if (!is_uuid(uuid)) throw std::runtime_error("invalid profile UUID in shortcut catalog");
    }
    auto persist = [&] { save(catalog, {{"schema", 1}, {"profiles", shortcuts}}); };
    const auto previous_size = shortcuts.size();
    if (fs::exists(directory)) {
        for (const auto &entry : fs::directory_iterator(directory)) {
            auto stem = entry.path().stem().string();
            if (entry.path().extension() != ".desktop" || stem.rfind(prefix, 0) != 0) continue;
            auto uuid = stem.substr(prefix.size());
            if (is_uuid(uuid) && managed_shortcut(entry.path(), uuid)) shortcuts.insert(uuid);
        }
    }
    if (!has_catalog || previous_size != shortcuts.size()) persist();
    if (action == "list" || action == "update") {
        bool failed = false;
        std::vector<std::string> lines;
        for (const auto &uuid : shortcuts) {
            const auto record = record_path("profiles", uuid);
            const auto path = directory / (prefix + uuid + ".desktop");
            try {
                if (!fs::exists(record)) {
                    lines.push_back("(missing profile) " + uuid + " " + path.string());
                    continue;
                }
                auto profile = read_json(record);
                auto name = profile.at("name").get<std::string>();
                if (action == "update") {
                    if (fs::exists(fs::symlink_status(path)) && !managed_shortcut(path, uuid))
                        throw std::runtime_error("unmanaged shortcut: " + path.string());
                    write_shortcut(path, profile, uuid);
                }
                lines.push_back(name + " " + uuid + " " + path.string());
            } catch (const std::exception &e) {
                failed = true;
                std::cerr << uuid << ": " << e.what() << '\n';
            }
        }
        std::sort(lines.begin(), lines.end());
        for (const auto &line : lines) std::cout << line << '\n';
        if (shortcuts.empty()) std::cout << "No profile shortcuts.\n";
        if (failed) throw std::runtime_error("could not process all shortcuts");
        return;
    }
    std::string uuid = selector;
    Json profile;
    if (action == "add" || !is_uuid(selector)) {
        auto profiles = action == "add" ? initialize(default_account()) : records("profiles");
        profile = lookup(profiles, selector);
        uuid = id(profile);
    }
    const auto path = directory / (prefix + uuid + ".desktop");
    const bool exists = fs::exists(fs::symlink_status(path));
    if (exists && !managed_shortcut(path, uuid)) throw std::runtime_error("unmanaged shortcut: " + path.string());
    if (action == "add") {
        write_shortcut(path, profile, uuid);
        shortcuts.insert(uuid);
        persist();
        std::cout << path.string() << '\n';
    } else if (action == "delete" && shortcuts.erase(uuid)) {
        fs::remove(path);
        persist();
        std::cout << "Deleted " << path.string() << '\n';
    } else if (action == "info" && exists) {
        std::cout << path.string() << '\n' << std::ifstream(path).rdbuf();
    } else throw std::runtime_error("shortcut not installed: " + path.string());
}

void import_default(const std::string &selector) {
    Lock lock(root() / "locks/metadata");
    const auto config = root() / "import.json";
    const auto profiles = initialize(default_account());
    if (selector.empty()) {
        if (!fs::exists(config)) throw std::runtime_error("choose a profile with bedrock import default set");
        auto p = lookup(profiles, read_json(config).at("profile"));
        std::cout << p.at("name").get<std::string>() << ' ' << id(p) << '\n';
        return;
    }

    const auto profile = lookup(profiles, selector);
    const auto apps = applications_directory();
    const auto mime = apps.parent_path() / "mime";
    const std::string desktop = "bedrockoverlinux-import.desktop";
    std::string types, xml = "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n";
    Args associate{"xdg-mime", "default", desktop};
    for (const auto &ext : import_extensions) {
        std::string type = "application/x-bedrockoverlinux-" + ext.substr(1);
        xml += "<mime-type type=\"" + type + "\"><comment>Minecraft " + ext +
               "</comment><sub-class-of type=\"application/zip\"/><glob pattern=\"*" + ext + "\"/></mime-type>\n";
        types += type + ";";
        associate.push_back(type);
    }
    write_file(mime / "packages/bedrockoverlinux-import.xml", xml + "</mime-info>\n");
    write_file(apps / desktop, "[Desktop Entry]\nType=Application\nName=Import into Minecraft Bedrock\n"
        "NoDisplay=true\nTerminal=false\nExec=/usr/bin/env " + desktop_argument("XDG_CONFIG_HOME=" + root().parent_path().string()) +
        " " + desktop_argument(fs::read_symlink("/proc/self/exe").string()) + " import open -- %f\nMimeType=" + types + "\n");
    checked({"update-mime-database", mime.string()});
    checked({"update-desktop-database", apps.string()});
    checked(associate);
    save(config, {{"profile", id(profile)}});
    std::cout << "Double-click imports use " << profile.at("name").get<std::string>() << '\n';
}

void import_content(const std::string &selector, const fs::path &file, const std::string &player, bool in_game) {
    Json p;
    {
        Lock lock(root() / "locks/metadata");
        const auto config = root() / "import.json";
        if (selector.empty() && !fs::exists(config)) throw std::runtime_error("choose a profile with bedrock import default set");
        p = lookup(initialize(default_account()), selector.empty() ? read_json(config).at("profile").get<std::string>() : selector);
    }
    const auto ext = file.extension().string();
    if (std::none_of(import_extensions.begin(), import_extensions.end(),
                     [&](const auto &supported) { return strcasecmp(ext.c_str(), supported.c_str()) == 0; }))
        throw std::runtime_error("expected .mcpack, .mcaddon, .mcworld or .mctemplate");
    if (!fs::is_regular_file(file)) throw std::runtime_error("missing content file: " + file.string());
    if (in_game) {
        const auto absolute = fs::absolute(file).lexically_normal();
        profile_command("launch", id(p), absolute.string());
        return;
    }
    Lock lease(root() / "locks" / (id(p) + ".profile"), false, false);
    if (!fs::exists(record_path("profiles", id(p)))) throw std::runtime_error("profile was deleted");
    install_content(root() / "profiles" / id(p), file, player);
}
