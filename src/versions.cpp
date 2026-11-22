#include "core.hpp"
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
fs::path clients() { return root() / "versions/client"; }
std::vector<Json> installed() {
    std::vector<Json> rows;
    if (fs::exists(clients()))
        for (auto &e : fs::directory_iterator(clients())) {
            auto id = e.path().filename().string();
            if (is_uuid(id)) {
                auto r = read_json(e.path() / "installation.json");
                if (r.at("uuid") != id)
                    throw std::runtime_error("installation UUID mismatch");
                rows.push_back(r);
            }
        }
    return rows;
}
Json catalog(Json *latest = nullptr, bool can_launch_offline = false) {
    auto path = root() / "versions/catalog.json";
    auto result = process({"curl", "-fsSL", "--proto", "=https", "--max-time", can_launch_offline ? "3" : "30",
                           "https://raw.githubusercontent.com/LukasPAH/minecraft-windows-gdk-version-db/refs/"
                           "heads/main/historical_versions.json"},
                          {}, true);
    if (result.status && latest) return {}; // The caller can launch its last resolved installation offline.
    if (result.status && fs::exists(path))
        return read_json(path);
    if (result.status)
        throw std::runtime_error("catalog unavailable");
    auto raw = Json::parse(result.output);
    Json rows = Json::object();
    static const std::regex allowed(R"(^https?://assets[12]\.xboxlive\.com/[^?#[:space:]]+\.msixvc$)");
    for (std::string channel : {"release", "preview"})
        for (auto &item : raw.at(channel + "Versions")) {
            std::string label = item.at("version");
            auto pos = label.find(' ');
            auto number = label.substr(pos == std::string::npos ? 0 : pos + 1);
            for (auto &candidate : item.at("urls")) {
                auto url = candidate.get<std::string>();
                if (!std::regex_match(url, allowed))
                    continue;
                rows[number] = {{"version", number}, {"channel", channel}, {"url", url}};
                if (latest && raw.value(channel, "") == number)
                    (*latest)[channel] = rows[number];
                break;
            }
        }
    if (rows.empty())
        throw std::runtime_error("empty catalog");
    save(path, rows);
    return rows;
}
std::string content_id(const fs::path &archive) {
    std::ifstream f(archive, std::ios::binary);
    char magic[8];
    unsigned char id[16];
    if (!f.seekg(0x200).read(magic, 8) || std::string(magic, 8) != "msft-xvd" ||
        !f.seekg(0x220).read(reinterpret_cast<char *>(id), 16))
        throw std::runtime_error("invalid XVD header");
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    int order[] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out << '-';
        out << std::setw(2) << static_cast<unsigned>(id[order[i]]);
    }
    return out.str();
}
std::string install_version(const Json &item, bool copy, bool reuse, const Json &account) {
    const std::string key = item.at("version");
    valid_name(key);
    Lock download(root() / "locks" / ("download-" + key));
    std::string id = uuid();
    bool replace = false;
    for (auto &row : installed())
        if (!copy && row.at("version") == key && row.at("channel") == item.at("channel") && !row.at("copy").get<bool>()) {
            if (reuse) return row.at("uuid");
            if (!confirm("Download " + key + " again?", true))
                return {};
            id = row.at("uuid");
            replace = true;
            break;
        }
    Lock installation(root() / "locks" / (id + ".version"), false, false);
    auto account_id = account.at("uuid").get<std::string>();
    Lock use(root() / "locks" / (account_id + ".usage"), true);
    ensure_service(account);
    auto env = account_env(account);
    auto cli = executable("xodus-cli");
    checked({cli, "check-ownership", "9NBLGGH2JHXJ"}, env);
    auto stage = clients() / (".install-" + uuid());
    fs::create_directories(stage);
    auto game = stage / "game";
    try {
        checked({cli, "install-owned", "9NBLGGH2JHXJ", item.at("url"), game.string()}, env);
        std::string exe;
        for (auto &file : fs::recursive_directory_iterator(game)) {
            auto name = file.path().filename().string();
            if (name != "Minecraft.Windows.exe" && name != "Minecraft.Windows.Preview.exe")
                continue;
            if (!exe.empty() || !file.is_regular_file() || file.file_size() == 0)
                throw std::runtime_error("invalid executable");
            exe = fs::relative(file.path(), game).string();
        }
        if (exe.empty())
            throw std::runtime_error("Minecraft executable missing");
        auto content = content_id(game / ".xodus-streaming.msixvc");
        Json row{{"uuid", id},
                 {"version", key},
                 {"copy", copy},
                 {"channel", item.at("channel")},
                 {"contentId", content},
                 {"executable", exe},
                 {"installedBy", account_id}};
        save(stage / "installation.json", row);
        auto dest = clients() / id;
        if (replace) {
            if (syscall(SYS_renameat2, AT_FDCWD, stage.c_str(), AT_FDCWD, dest.c_str(), RENAME_EXCHANGE))
                throw std::system_error(errno, std::generic_category(), "replace installation");
            fs::remove_all(stage);
        } else
            fs::rename(stage, dest);
    } catch (...) {
        fs::remove_all(stage);
        throw;
    }
    return id;
}
} // namespace
Json version_resolve(const std::string &key) {
    for (auto &row : installed())
        if (row.at("uuid") == key || (row.at("version") == key && !row.at("copy").get<bool>()))
            return row;
    throw std::runtime_error("installation unavailable: " + key + "; use bedrock version list");
}
void update_profile_version(Json &profile, const Json &account) {
    const std::string track = profile.at("track");
    const std::string channel = track == "latest_release" ? "release" : "preview";
    if (track != "latest_release" && track != "latest_preview") throw std::runtime_error("invalid version track");
    Json latest = Json::object();
    bool installed_before = !profile.at("version").is_null() &&
        fs::is_regular_file(clients() / profile.at("version").get<std::string>() / "installation.json");
    if (catalog(&latest, installed_before).is_null()) {
        if (profile.at("version").is_null()) throw std::runtime_error("catalog unavailable for first launch");
        std::cerr << "Catalog unavailable; using the previous installation.\n";
        return;
    }
    if (!latest.contains(channel)) throw std::runtime_error("catalog has no current GDK package for " + channel);
    auto id = install_version(latest.at(channel), false, true, account);
    if (profile.at("version") != id) {
        profile["version"] = id;
        put("profiles", profile);
    }
}
void versions_command(const std::string &action, const std::string &key, bool copy) {
    auto local = installed();
    if (action == "package info") {
        auto row = version_resolve(key);
        Lock installation(root() / "locks" / (row.at("uuid").get<std::string>() + ".version"), true);
        auto package = clients() / row.at("uuid").get<std::string>() / "game/.xodus-streaming.msixvc";
        checked({executable("xvdtool"), "-i", "-nd", "-ne", package.string()}, xvdtool_environment());
        return;
    }
    if (action == "info") {
        for (auto &row : local)
            if (row.at("uuid") == key || (row["version"] == key && !row.at("copy").get<bool>())) {
                row["downloaded"] = true;
                std::cout << row.dump(2) << '\n';
                return;
            }
    }
    auto remote = catalog();
    if (action == "list") {
        for (auto &[number, item] : remote.items())
            std::cout << number << " " << item["channel"].get<std::string>() << '\n';
        for (auto &row : local)
            std::cout << row["version"].get<std::string>() << " " << row["uuid"].get<std::string>()
                      << (row["copy"].get<bool>() ? " copy" : " shared") << '\n';
        return;
    }
    if (!remote.contains(key))
        throw std::runtime_error("version unavailable: " + key);
    auto item = remote.at(key);
    if (action == "info") {
        item["downloaded"] = false;
        std::cout << item.dump(2) << '\n';
        return;
    }
    auto id = install_version(item, copy, false, default_account());
    if (!id.empty()) std::cout << id << '\n';
}
void services_ciks(const Json &a) {
    std::set<std::string> contents;
    for (auto &row : installed())
        contents.insert(row.at("contentId").get<std::string>());
    if (contents.empty()) {
        std::cout << "No installed Minecraft packages.\n";
        return;
    }
    auto temp = runtime() / ("bol-license-" + uuid());
    fs::create_directory(temp);
    try {
        for (auto &content : contents) {
            auto dest = temp / content;
            if (!is_uuid(content))
                throw std::runtime_error("invalid content ID");
            checked({executable("xodus-cli"), "license", content, dest.string()}, account_env(a), true);
            for (auto &e : fs::directory_iterator(dest)) {
                if (!e.is_regular_file() || e.path().extension() != ".cik" || e.file_size() != 48)
                    throw std::runtime_error("invalid CIK record");
                std::ifstream f(e.path(), std::ios::binary);
                unsigned char key[32];
                if (!f.seekg(16).read(reinterpret_cast<char *>(key), 32))
                    throw std::runtime_error("incomplete CIK");
                std::cout << content << " " << e.path().stem().string() << " ";
                for (auto byte : key)
                    std::cout << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
                std::cout << '\n';
            }
        }
    } catch (...) {
        fs::remove_all(temp);
        throw;
    }
    fs::remove_all(temp);
}
