#include "core.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <regex>
#include <sys/stat.h>
#include <unistd.h>

namespace {
enum class Manager { pacman, apt, dnf, unknown };

Manager package_manager() {
    // Pick the host distribution even if an unrelated manager is also installed.
    std::ifstream release("/etc/os-release");
    std::string ids;
    for (std::string line; std::getline(release, line);) {
        if (line.rfind("ID=", 0) == 0 || line.rfind("ID_LIKE=", 0) == 0) {
            std::string value = line.substr(line.find('=') + 1);
            for (auto &c : value)
                if (c == '"' || c == '\'')
                    c = ' ';
            ids += " " + value;
        }
    }
    std::istringstream tokens(ids);
    for (std::string id; tokens >> id;) {
        if (id == "arch")
            return Manager::pacman;
        if (id == "debian" || id == "ubuntu")
            return Manager::apt;
        if (id == "fedora" || id == "rhel")
            return Manager::dnf;
    }
    return Manager::unknown;
}

Args missing_packages(Manager manager, const Args &wanted) {
    Args missing;
    if (manager == Manager::pacman) {
        Args query{"/usr/bin/pacman", "-T"};
        query.insert(query.end(), wanted.begin(), wanted.end());
        auto result = process(query, {}, true);
        if (result.status != 0 && result.status != 127)
            throw std::runtime_error("pacman could not check dependencies");
        std::istringstream lines(result.output);
        for (std::string pkg; lines >> pkg;) {
            if (std::find(wanted.begin(), wanted.end(), pkg) == wanted.end())
                throw std::runtime_error("unexpected pacman dependency: " + pkg);
            missing.push_back(pkg);
        }
        if (result.status != 0 && missing.empty())
            throw std::runtime_error("pacman -T failed");
        return missing;
    }
    for (const auto &pkg : wanted) {
        if (manager == Manager::apt) {
            auto installed = process({"/usr/bin/dpkg-query", "-W", "-f=${db:Status-Status}", pkg}, {}, true);
            if (installed.status != 0 || installed.output != "installed")
                missing.push_back(pkg);
        } else if (process({"/usr/bin/rpm", "-q", pkg}, {}, true).status != 0) {
            missing.push_back(pkg);
        }
    }
    return missing;
}
void install_packages(Manager manager, const Args &wanted) {
    auto missing = missing_packages(manager, wanted);
    if (missing.empty())
        return;
    Args command;
    if (manager == Manager::pacman)
        command = {"/usr/bin/pacman", "-S", "--needed", "--noconfirm"};
    if (manager == Manager::apt)
        command = {"/usr/bin/apt-get", "install", "-y", "--no-install-recommends"};
    if (manager == Manager::dnf)
        command = {"/usr/bin/dnf", "install", "-y"};
    command.insert(command.end(), missing.begin(), missing.end());
    std::cerr << "Installing system dependencies:";
    for (const auto &pkg : missing)
        std::cerr << ' ' << pkg;
    std::cerr << '\n';
    if (geteuid() != 0) {
        if (isatty(STDIN_FILENO) && access("/usr/bin/sudo", X_OK) == 0)
            command.insert(command.begin(), "/usr/bin/sudo");
        else if (access("/usr/bin/pkexec", X_OK) == 0)
            command.insert(command.begin(), "/usr/bin/pkexec");
        else
            command.insert(command.begin(), "/usr/bin/sudo");
    }
    // The CLI uses a private umask for game data. System packages need normal file modes.
    auto previous = umask(0022);
    try {
        checked(command);
    } catch (...) {
        umask(previous);
        throw;
    }
    umask(previous);
}
// Debian's default repositories do not contain Microsoft.NETCore.App 8.
void install_private_dotnet() {
    const auto base = root() / "tools/dotnet";
    Lock lock(base / ".install.lock");
    auto index = Json::parse(checked({"curl", "-fsSL", "--proto", "=https", "--max-time", "60",
        "https://builds.dotnet.microsoft.com/dotnet/release-metadata/8.0/releases.json"}, {}, true));
    const auto &runtime = index.at("releases").at(0).at("runtime");
    std::string version = runtime.at("version");
    if (!std::regex_match(version, std::regex(R"(8\.0\.[0-9]+)"))) throw std::runtime_error("invalid .NET 8 release");
    auto dest = base / version;
    if (!fs::exists(dest)) {
        const auto &files = runtime.at("files");
        auto asset = std::find_if(files.begin(), files.end(), [](const Json &f) {
            return f.at("name") == "dotnet-runtime-linux-x64.tar.gz" && f.at("rid") == "linux-x64";
        });
        if (asset == files.end()) throw std::runtime_error(".NET Linux x64 asset missing");
        std::string digest = asset->at("hash");
        std::transform(digest.begin(), digest.end(), digest.begin(), [](unsigned char c) { return std::tolower(c); });
        auto stage = base / (".install-" + uuid());
        fs::create_directories(stage / "tree");
        auto archive = (stage / "runtime.tar.gz").string();
        try {
            checked({"curl", "-fL", "--proto", "=https", "--retry", "2", "-o", archive, asset->at("url")});
            if (checked({"sha512sum", archive}, {}, true).substr(0,128) != digest)
                throw std::runtime_error(".NET runtime checksum mismatch");
            checked({"bsdtar", "--no-same-owner", "--no-same-permissions", "-xf", archive, "-C", (stage/"tree").string()});
            checked({(stage/"tree/dotnet").string(), "--list-runtimes"}, {}, true);
            fs::rename(stage / "tree", dest);
            fs::remove_all(stage);
        } catch (...) { fs::remove_all(stage); throw; }
    }
    save(base/"current.json", {{"tag",version}});
}
} // namespace

void install_dependencies(const std::string &tool) {
    auto manager = package_manager();
    if (manager == Manager::unknown)
        return; // A preconfigured host can still pass the executable probes.
    bool arch = manager == Manager::pacman;
    bool deb = manager == Manager::apt;
    Args packages{"curl", "coreutils", arch ? "libarchive" : (deb ? "libarchive-tools" : "bsdtar")};
    if (tool.empty() || tool == "xodus") {
        packages.push_back(arch ? "webkit2gtk-4.1" : (deb ? "libwebkit2gtk-4.1-0" : "webkit2gtk4.1"));
        packages.push_back(arch ? "libnotify" : (deb ? "libnotify-bin" : "libnotify"));
    }
    if (tool.empty() || tool == "winegdk")
        packages.push_back(arch ? "vulkan-icd-loader" : (deb ? "libvulkan1" : "vulkan-loader"));
    bool private_dotnet = false;
    if (tool.empty() || tool == "xvdtool") {
        private_dotnet = deb && process({"/usr/bin/apt-cache","show","dotnet-runtime-8.0"},{},true).status != 0;
        packages.push_back(private_dotnet ? "icu-devtools" : "dotnet-runtime-8.0");
    }
    install_packages(manager, packages);
    if (private_dotnet) install_private_dotnet();
}

Env xvdtool_environment() {
    auto base = root() / "tools/dotnet";
    auto state = base / "current.json";
    if (!fs::exists(state)) return {};
    std::string version = read_json(state).at("tag");
    valid_name(version);
    return {{"DOTNET_ROOT_X64",(base/version).string()}};
}
