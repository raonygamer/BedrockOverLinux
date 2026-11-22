#include "core.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <pugixml.hpp>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
std::string unit(const Json &a) { return "bedrock-xodus-" + a.at("uuid").get<std::string>() + ".service"; }
fs::path usage(const Json &a) { return root() / "locks" / (a.at("uuid").get<std::string>() + ".usage"); }
fs::path defaults() { return root() / "auth.json"; }
int socket_connect_error(const std::string &path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
        throw std::runtime_error("Xodus socket path too long");
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category());
    int error = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 ? 0 : errno;
    close(fd);
    return error;
}

// Called under the account lifecycle lock, with the systemd unit stopped.
void clear_stale_socket(const std::string &path) {
    struct stat entry{};
    if (lstat(path.c_str(), &entry) < 0) {
        if (errno == ENOENT) return;
        throw std::system_error(errno, std::generic_category(), path);
    }
    if (!S_ISSOCK(entry.st_mode) || entry.st_uid != geteuid())
        throw std::runtime_error("unexpected entry at Xodus socket path: " + path);
    int error = socket_connect_error(path);
    if (error == ENOENT) return;
    if (error == 0)
        throw std::runtime_error("another service is listening on the Xodus socket: " + path);
    if (error != ECONNREFUSED)
        throw std::system_error(error, std::generic_category(), path);
    if (unlink(path.c_str()) < 0 && errno != ENOENT)
        throw std::system_error(errno, std::generic_category(), path);
}
bool busy(const Json &a) {
    try {
        Lock lock(usage(a), false, false);
        return false;
    } catch (const std::system_error &e) {
        if (e.code().value() == EWOULDBLOCK)
            return true;
        throw;
    }
}
void stop(const Json &a) {
    if (service_running(a))
        checked({"systemctl", "--user", "stop", unit(a)});
    clear_stale_socket(account_env(a).at("XODUS_SOCKET"));
}
bool valid_tag(const std::string &tag) {
    return !tag.empty() && tag.size() <= 128 &&
           std::none_of(tag.begin(), tag.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

std::vector<char> xbox_message(const std::string &path, unsigned char kind, const std::string &request) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
        return {};
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return {};
    auto close_stream = [](FILE *f) { fclose(f); };
    std::unique_ptr<FILE, decltype(close_stream)> stream(fdopen(fd, "rb"), close_stream);
    if (!stream) {
        close(fd);
        return {};
    }
    timeval timeout{30, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
        connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
        return {};

    // Xodus XML frame: magic, message type and payload length, all little-endian.
    std::string frame("XSDX");
    frame.push_back(static_cast<char>(kind));
    frame.push_back(0);
    frame.push_back(static_cast<char>(request.size() & 0xff));
    frame.push_back(static_cast<char>(request.size() >> 8));
    frame += request;
    for (size_t sent = 0; sent < frame.size();) {
        auto n = send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return {};
        sent += static_cast<size_t>(n);
    }
    unsigned char header[8];
    if (fread(header, 1, sizeof(header), stream.get()) != sizeof(header) || std::memcmp(header, "XSDX", 4) ||
        header[4] != kind + 1 || header[5] != 0)
        return {};
    std::vector<char> body(header[6] | (header[7] << 8));
    if (body.empty() || fread(body.data(), 1, body.size(), stream.get()) != body.size())
        return {};
    return body;
}

// Initialize the same Microsoft session used by the game, then read only its verified Xbox display name.
std::string session_gamertag(const Json &account) {
    Lock use(usage(account), true);
    ensure_service(account);
    auto path = account_env(account).at("XODUS_SOCKET");
    if (xbox_message(path, 3,
                     "<MsaTokenRequest><ClientId>0000000040159362</ClientId>"
                     "<MSAFullTrust>true</MSAFullTrust></MsaTokenRequest>")
            .empty())
        return {};
    const std::string request =
        "<XboxTokenRequest><PackageFamilyName>Microsoft.MinecraftUWP_8wekyb3d8bbwe</PackageFamilyName>"
        "<TitleId>896928775</TitleId><RelyingParty>https://profile.xboxlive.com/users/me/profile/settings"
        "?settings=Gamertag</RelyingParty><HttpMethod>GET</HttpMethod></XboxTokenRequest>";
    auto body = xbox_message(path, 7, request);
    if (body.empty())
        return {};
    pugi::xml_document doc;
    if (!doc.load_buffer_inplace(body.data(), body.size()))
        return {};
    auto response = doc.child("XboxTokenResponse");
    for (const char *key : {"UniqueModernGamertag", "Gamertag"}) {
        std::string tag = response.child_value(key);
        if (valid_tag(tag))
            return tag;
    }
    return {};
}

bool refresh_gamertag(Json &account, const std::string &cli) {
    auto result = process({"timeout", "22s", cli, "accounts", "profile", account.at("xodusId")},
                          account_env(account), true);
    auto profile = Json::parse(result.output, nullptr, false);
    std::string tag;
    if (result.status == 0 && profile.is_object() && profile.contains("gamertag") &&
        profile["gamertag"].is_string())
        tag = profile.at("gamertag");
    if (!valid_tag(tag)) {
        try {
            tag = session_gamertag(account);
        } catch (const std::exception &e) {
            std::cerr << "Xbox session lookup: " << e.what() << '\n';
        }
    }
    if (!valid_tag(tag))
        return false;
    account["name"] = tag;
    account.erase("gamertagPending");
    put("accounts", account);
    return true;
}
} // namespace
Env account_env(const Json &a) {
    auto id = a.at("uuid").get<std::string>();
    if (!is_uuid(id))
        throw std::runtime_error("invalid account UUID");
    auto sock = "bol-" + id + ".sock";
    return {{"XODUS_CONFIG_DIR", (root() / "xodus" / id).string()},
            {"XODUS_SOCK_NAME", sock},
            {"XODUS_SOCKET", (runtime() / sock).string()}};
}
Json default_account() {
    auto rows = records("accounts");
    if (rows.empty())
        throw std::runtime_error("sign in first: bedrock auth login");
    if (!fs::exists(defaults()))
        return rows.front();
    return lookup(rows, read_json(defaults()).at("account"));
}
bool service_running(const Json &a) {
    auto r = process({"systemctl", "--user", "is-active", unit(a)}, {}, true);
    if (r.status != 0 && r.status != 3 && r.status != 4)
        throw std::runtime_error("cannot query user systemd manager");
    return r.output == "active\n" || r.output == "activating\n";
}
void ensure_service(const Json &a) {
    Lock lock(root() / "locks" / (a.at("uuid").get<std::string>() + ".service"));
    if (!fs::exists(record_path("accounts", a.at("uuid"))))
        throw std::runtime_error("account signed out");
    auto env = account_env(a);
    if (!service_running(a)) {
        clear_stale_socket(env.at("XODUS_SOCKET"));
        Args args{"systemd-run", "--user", "--quiet", "--collect", "--service-type=exec", "--unit=" + unit(a)};
        for (auto &[k, v] : env)
            args.push_back("--setenv=" + k + "=" + v);
        args.push_back(executable("xodus-service"));
        checked(args);
    }
    auto until = std::chrono::steady_clock::now() + std::chrono::minutes(6);
    for (int tick = 0; socket_connect_error(env.at("XODUS_SOCKET")) != 0; ++tick) {
        if ((tick % 10 == 0 && !service_running(a)) || std::chrono::steady_clock::now() > until)
            throw std::runtime_error("Xodus did not become ready; see journalctl --user -u " + unit(a));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
void auth_command(const std::string &action, const std::string &arg) {
    Lock lock(root() / "locks/auth");
    auto accounts = records("accounts");
    if (action == "list") {
        if (accounts.empty()) {
            std::cout << "No accounts. Run bedrock auth login.\n";
            return;
        }
        auto active = default_account();
        for (auto &a : accounts)
            std::cout << (a == active ? "* " : "  ") << a["name"].get<std::string>() << " "
                      << a["uuid"].get<std::string>()
                      << (a.value("gamertagPending", false) ? " (gamertag pending)" : "") << '\n';
    } else if (action == "refresh") {
        for (auto &account : accounts) {
            if (!account.value("gamertagPending", false))
                continue;
            if (!refresh_gamertag(account, executable("xodus-cli")))
                throw std::runtime_error("gamertag still unavailable; account remains signed in");
            std::cout << account.at("name").get<std::string>() << " " << account.at("uuid").get<std::string>()
                      << '\n';
        }
    } else if (action == "default info") {
        std::cout << default_account().dump(2) << '\n';
    } else if (action == "login") {
        Json a{{"uuid", uuid()}};
        auto env = account_env(a);
        auto directory = fs::path(env.at("XODUS_CONFIG_DIR"));
        fs::create_directories(directory);
        auto cli = executable("xodus-cli");
        try {
            checked({cli, "login"}, env);
            auto saved = Json::parse(checked({cli, "accounts", "list"}, env, true));
            if (!saved.is_array() || saved.size() != 1)
                throw std::runtime_error("expected one account in new Xodus profile");
            std::string id = saved.at(0).at("id");
            a["xodusId"] = id;
            a["name"] = saved.at(0).value("username", "");
            if (a["name"] == "")
                a["name"] = "Xbox-" + a.at("uuid").get<std::string>().substr(0, 8);
            a["gamertagPending"] = true;
            for (const auto &existing : accounts)
                if (existing.value("xodusId", "") == id)
                    throw std::runtime_error("already logged in as " + existing.at("name").get<std::string>());
            put("accounts", a);
        } catch (...) {
            try {
                process({executable("xodus-cli"), "logout"}, env, true);
            } catch (...) {
            }
            fs::remove_all(directory);
            throw;
        }
        if (accounts.empty())
            save(defaults(), {{"account", a.at("uuid")}});
        if (!refresh_gamertag(a, cli))
            std::cerr << "Signed in. To retry the Xbox gamertag lookup: bedrock auth refresh\n";
        std::cout << a.at("name").get<std::string>() << " " << a.at("uuid").get<std::string>() << '\n';
    } else {
        auto a = lookup(accounts, arg);
        if (action == "default set")
            save(defaults(), {{"account", a["uuid"]}});
        else {
            Lock lifecycle(root() / "locks" / (a.at("uuid").get<std::string>() + ".service"));
            Lock use(usage(a), false, false);
            stop(a);
            checked({executable("xodus-cli"), "logout"}, account_env(a));
            if (a == default_account()) {
                if (accounts.size() == 1)
                    fs::remove(defaults());
                else
                    for (auto &other : accounts)
                        if (other != a) {
                            save(defaults(), {{"account", other["uuid"]}});
                            break;
                        }
            }
            fs::remove(record_path("accounts", a["uuid"]));
            fs::remove_all(account_env(a).at("XODUS_CONFIG_DIR"));
        }
    }
}
void services_command(const std::string &action, const std::string &arg) {
    auto accounts = records("accounts");
    if (action == "list" || action == "info") {
        if (action == "info")
            accounts = {lookup(accounts, arg)};
        for (auto &a : accounts)
            std::cout << a.at("name").get<std::string>() << " " << unit(a) << " "
                      << (service_running(a) ? "running" : "stopped") << (busy(a) ? " (in use)" : "") << '\n';
        return;
    }
    auto a = lookup(accounts, arg);
    if (action == "kill") {
        Lock gate(root() / "locks" / (a.at("uuid").get<std::string>() + ".service"));
        if (!service_running(a)) {
            clear_stale_socket(account_env(a).at("XODUS_SOCKET"));
            std::cout << "already stopped\n";
            return;
        }
        if (busy(a) && !confirm("Game or download in progress. Stop this account's Xodus service?"))
            return;
        stop(a);
    } else {
        Lock use(usage(a), true);
        ensure_service(a);
        checked({executable("xodus-cli"), "check-ownership", "9NBLGGH2JHXJ"}, account_env(a));
        if (action == "ciks")
            services_ciks(a);
    }
}
