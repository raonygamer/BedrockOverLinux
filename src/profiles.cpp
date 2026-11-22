#include "core.hpp"
#include <fstream>
#include <iostream>
#include <pugixml.hpp>
#include <pwd.h>
#include <regex>
#include <sstream>
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
void initialize(const Json &account) {
    auto px = prefix("default", true);
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
        auto converted = new_profile(p.at("name"), account, px);
        converted["uuid"] = p.at("uuid");
        if (p.contains("version") && p["version"].is_string()) {
            try {
                converted["version"] = version_resolve(p["version"]).at("uuid");
            } catch (const std::runtime_error &) {
                std::cerr << "Legacy version pending; original saved in " << backup << '\n';
            }
        }
        put("profiles", converted);
    }
    auto seeded = profiles / ".defaults-created";
    if (fs::exists(seeded)) return;
    for (std::string name : {"release", "preview"}) {
        auto matches = [&](const Json &r) { return r.at("name") == name; };
        if (std::none_of(rows.begin(), rows.end(), matches))
            put("profiles", new_profile(name, account, px));
    }
    write_file(seeded, "");
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
std::string availability(const Json &p) {
    if (busy(prefix(p.at("prefix"), false)))
        return "prefix busy";
    if (!fs::exists(record_path("accounts", p.at("account"))))
        return "signed out";
    if (!p.contains("track") && p.at("version").is_null())
        return "select a version";
    try {
        if (!p.contains("track")) version_resolve(p.at("version"));
        executable("wine");
        executable("xodus-cli");
        executable("xodus-service");
    } catch (const std::exception &e) {
        return e.what();
    }
    return "ready";
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
    initialize(default_account());
    auto profile = lookup(records("profiles"), selector);
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
            if (key == "WINEPREFIX" || key == "WINESERVER" || key.rfind("XODUS_", 0) == 0)
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

void worker(const std::string &selector) {
    if (!is_uuid(selector))
        throw std::runtime_error("invalid worker UUID");
    Lock profile_lock(root() / "locks" / (selector + ".profile"), false, false);
    auto p = lookup(records("profiles"), selector);
    auto px = prefix(p.at("prefix"), false);
    Lock lease(root() / "locks" / (id(px) + ".prefix"), false, false);
    save(root() / "locks" / (id(px) + ".owner"), {{"profile", id(p)}});
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
    if (!fs::exists(dir / "system.reg"))
        checked({wine, "wineboot", "-u"}, env);
    auto user = getpwuid(getuid());
    if (!user)
        throw std::runtime_error("Wine user unavailable");
    auto appdata = dir / "drive_c/users" / user->pw_name / "AppData";
    bool preview = version.at("channel") == "preview";
    auto data = root() / "profiles" / id(p);
    fs::create_directories(shared(p));
    bind_storage(data / "Users",
                 appdata / "Roaming" / (preview ? "Minecraft Bedrock Preview" : "Minecraft Bedrock") / "Users",
                 root() / "profiles");
    bind_storage(data / "LocalState",
                 appdata / "Local/Packages" /
                     (preview ? "Microsoft.MinecraftWindowsBeta_8wekyb3d8bbwe"
                              : "Microsoft.MinecraftUWP_8wekyb3d8bbwe") /
                     "LocalState",
                 root() / "profiles");
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
    env["BEDROCK_PROFILE_DIR"] = data.string();
    env["BEDROCK_GAME_DIR"] = game.string();
    const auto prelaunch = p.value("prelaunch", Args{});
    const auto wrapper = p.value("wrapper", Args{});
    const auto arguments = p.value("args", Args{});
    if (!arguments.empty()) {
        run.push_back("--");
        run.insert(run.end(), arguments.begin(), arguments.end());
    }
    fs::current_path(data);
    if (!prelaunch.empty()) checked(prelaunch, env);
    run.insert(run.begin(), wrapper.begin(), wrapper.end());
    auto result = process(run, env);
    checked({env.at("WINESERVER"), "-w"}, env);
    if (result.status)
        throw std::runtime_error("game exited " + std::to_string(result.status));
}
void profile_command(const std::string &action, const std::string &selector, const std::string &value) {
    Json profile;
    if (action == "delete") {
        {
            Lock metadata(root() / "locks/metadata");
            initialize(default_account());
            profile = lookup(records("profiles"), selector);
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
        initialize(account);
        auto rows = records("profiles");
        if (action == "list") {
            bool color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
            for (auto &p : rows) {
                auto state = availability(p);
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
        auto state = availability(profile);
        if (state != "ready")
            throw std::runtime_error(state);
    }
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
                "--property=BindsTo=" + caller,
                "--property=After=" + caller,
                "--unit=bedrock-game-" + profile.at("prefix").get<std::string>() + ".service"};
    for (auto key : {"HOME", "PATH", "XDG_CONFIG_HOME", "XDG_RUNTIME_DIR", "DISPLAY", "WAYLAND_DISPLAY",
                     "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "LD_LIBRARY_PATH", "BEDROCK_WINE",
                     "BEDROCK_WINESERVER", "BEDROCK_XODUS_CLI", "BEDROCK_XODUS_SERVICE"}) {
        if (auto val = getenv(key))
            launch.push_back("--setenv=" + std::string(key) + "=" + val);
    }
    launch.insert(launch.end(), {fs::read_symlink("/proc/self/exe").string(), "_run", id(profile)});
    checked(launch);
}
void env_command(const std::string &selector) {
    Json p;
    {
        Lock lock(root() / "locks/metadata");
        initialize(default_account());
        p = lookup(records("profiles"), selector);
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
tmp=$(mktemp /etc/.bedrock-environment.XXXXXX)
trap 'rm -f "$tmp"' EXIT
if [ -f "$file" ]; then
    cp --preserve=all "$file" "$tmp"
    awk '!/^[[:space:]]*(export[[:space:]]+)?COM_MOJANG[[:space:]]*=/' "$file" > "$tmp"
else
    chmod 644 "$tmp"
fi
printf 'COM_MOJANG="%s"\n' "$1" >> "$tmp"
mv -fT "$tmp" "$file"
)SH";
    Args cmd{"/usr/bin/flock", "/etc/.bedrock-environment.lock",      "/bin/sh", "-c", script, "bedrock",
             path.string(),    getenv("COM_MOJANG") ? "set" : "unset"};
    if (geteuid() != 0)
        cmd.insert(cmd.begin(), "/usr/bin/sudo");
    checked(cmd);
    std::cout << "COM_MOJANG=" << path.string() << " (new login sessions)\n";
}
