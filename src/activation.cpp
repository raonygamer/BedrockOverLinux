#include "activation.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

namespace {
fs::path session_path(const std::string &session) {
    if (!is_uuid(session)) throw std::runtime_error("invalid import session");
    return runtime() / "bedrockoverlinux/import" / session;
}
}

ActivationSession::ActivationSession(const std::string &session) : directory(session_path(session)) {
    fs::create_directories(directory);
}
ActivationSession::~ActivationSession() {
    std::error_code ignored;
    fs::remove_all(directory, ignored);
}
std::string wine_file_path(const fs::path &path) {
    auto value = "Z:" + path.string();
    std::replace(value.begin(), value.end(), '/', '\\');
    return value;
}
void forward_file(const Json &profile, const fs::path &file) {
    using namespace std::chrono_literals;
    const auto owner_path = root() / "locks" / (profile.at("prefix").get<std::string>() + ".owner");
    const auto owner = read_json(owner_path);
    if (owner.at("profile") != profile.at("uuid")) throw std::runtime_error("prefix busy in another profile");
    if (!owner.contains("activation")) throw std::runtime_error("restart this profile with updated bedrock and WineGDK to forward files");
    const auto dir = session_path(owner.at("activation"));
    auto check_session = [&] {
        if (!fs::is_directory(dir) || read_json(owner_path) != owner)
            throw std::runtime_error("the import session ended");
    };
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!fs::exists(dir / "ready")) {
        check_session();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Minecraft did not register its file importer; update WineGDK and restart the profile");
        std::this_thread::sleep_for(40ms);
    }
    const auto token = uuid();
    const auto pending = dir / (token + ".req");
    const auto temp = dir / (token + ".tmp");
    const auto ack = dir / (token + ".ack");
    const auto err = dir / (token + ".err");
    // Never recreate a session directory on shutdown. Publish only a complete request.
    try {
        std::ofstream out;
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.open(temp, std::ios::binary);
        out << wine_file_path(file);
        out.close();
        fs::rename(temp, pending);
    } catch (...) {
        fs::remove(temp);
        throw;
    }
    const auto delivery_deadline = std::chrono::steady_clock::now() + 15s;
    for (;;) {
        if (fs::remove(ack)) return;
        if (fs::remove(err)) throw std::runtime_error("WineGDK rejected the import request");
        check_session();
        if (std::chrono::steady_clock::now() >= delivery_deadline) {
            if (fs::remove(pending)) throw std::runtime_error("Minecraft did not accept the file in time");
            throw std::runtime_error("Minecraft accepted the request but has not confirmed delivery");
        }
        std::this_thread::sleep_for(20ms);
    }
}
