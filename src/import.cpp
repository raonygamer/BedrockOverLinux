#include "core.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>

const Args import_extensions{".mcpack", ".mcaddon", ".mcworld", ".mctemplate"};

namespace {
std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Stage {
    fs::path path;
    ~Stage() { std::error_code ec; fs::remove_all(path, ec); }
};

struct Budget { size_t entries = 0; uint64_t bytes = 0; };

void unpack(const fs::path &file, const fs::path &out, Budget &budget) {
    std::unique_ptr<archive, decltype(&archive_read_free)> zip(archive_read_new(), archive_read_free);
    if (!zip) throw std::bad_alloc();
    archive_read_support_format_zip(zip.get());
    auto check = [&](int status) {
        if (status != ARCHIVE_OK) throw std::runtime_error(std::string("invalid ZIP: ") + (archive_error_string(zip.get()) ? archive_error_string(zip.get()) : "unreadable entry"));
    };
    check(archive_read_open_filename(zip.get(), file.c_str(), 65536));
    archive_entry *entry;
    std::set<std::string> files;
    fs::create_directories(out);
    for (;;) {
        int status = archive_read_next_header(zip.get(), &entry);
        if (status == ARCHIVE_EOF) break;
        check(status);
        if (++budget.entries > 100000) throw std::runtime_error("too many archive entries");
        const char *entry_name = archive_entry_pathname(entry);
        if (!entry_name || !*entry_name) throw std::runtime_error("missing archive entry name");
        std::string name = entry_name;
        std::replace(name.begin(), name.end(), '\\', '/');
        fs::path rel(name);
        if (rel.is_absolute() || name.find(':') != name.npos ||
            std::find(rel.begin(), rel.end(), fs::path("..")) != rel.end())
            throw std::runtime_error("unsafe archive path: " + name);
        rel = rel.lexically_normal();
        auto path = out / rel;
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry))
            throw std::runtime_error("archive contains a link");
        if (archive_entry_filetype(entry) == AE_IFDIR) { fs::create_directories(path); continue; }
        if (archive_entry_filetype(entry) != AE_IFREG || !files.insert(lower(rel.string())).second)
            throw std::runtime_error("unsupported or duplicate archive entry: " + name);
        fs::create_directories(path.parent_path());
        std::ofstream dest;
        dest.exceptions(std::ios::badbit | std::ios::failbit);
        dest.open(path, std::ios::binary);
        std::array<char, 65536> buf;
        for (;;) {
            auto n = archive_read_data(zip.get(), buf.data(), buf.size());
            if (n < 0) throw std::runtime_error("corrupt archive entry: " + name);
            if (n == 0) break;
            budget.bytes += n;
            if (budget.bytes > (uint64_t{16} << 30)) throw std::runtime_error("expanded content exceeds 16 GiB");
            dest.write(buf.data(), n);
        }
        dest.close();
    }
    check(archive_read_close(zip.get()));
}

bool numeric(const std::string &s) { return !s.empty() && s.find_first_not_of("0123456789") == s.npos; }

fs::path player_storage(const fs::path &users, std::string player) {
    if (player.empty() && fs::is_directory(users)) {
        for (const auto &e : fs::directory_iterator(users)) {
            const auto name = e.path().filename().string();
            if (!numeric(name) || !e.is_directory()) continue;
            if (!player.empty()) throw std::runtime_error("multiple player folders; select one with --player <folder>");
            player = name;
        }
    }
    if (!numeric(player) || !fs::is_directory(users / player))
        throw std::runtime_error("no matching player storage; open this profile in Minecraft first");
    return users / player / "games/com.mojang";
}

bool installed_pack(const fs::path &dest) {
    if (!fs::is_directory(dest.parent_path())) return false;
    // Minecraft names imported directories after the pack. The manifest identifies the pack.
    for (const auto &entry : fs::directory_iterator(dest.parent_path())) {
        const auto manifest = entry.path() / "manifest.json";
        if (!fs::is_regular_file(manifest) || fs::file_size(manifest) > 4 * 1024 * 1024) continue;
        std::ifstream input(manifest);
        const auto data = Json::parse(input, nullptr, false);
        if (!data.is_object() || !data.contains("header") || !data["header"].is_object()) continue;
        const auto &header = data["header"];
        if (header.contains("uuid") && header["uuid"].is_string() &&
            lower(header["uuid"].get<std::string>()) == dest.filename().string()) return true;
    }
    return false;
}

using Moves = std::vector<std::pair<fs::path, fs::path>>;
void pack(const fs::path &dir, const fs::path &shared, Moves &moves, bool require_template) {
    auto manifest = dir / "manifest.json";
    if (fs::file_size(manifest) > 4 * 1024 * 1024) throw std::runtime_error("oversized pack manifest");
    auto data = read_json(manifest);
    auto id = lower(data.at("header").at("uuid").get<std::string>());
    if (!is_uuid(id)) throw std::runtime_error("invalid manifest UUID");
    std::set<std::string> kinds;
    for (const auto &m : data.at("modules")) {
        const auto t = m.at("type").get<std::string>();
        if (t == "resources") kinds.insert("resource_packs");
        else if (t == "data" || t == "script") kinds.insert("behavior_packs");
        else if (t == "world_template") kinds.insert("world_templates");
        else if (t == "skin_pack") kinds.insert("skin_packs");
        else throw std::runtime_error("unknown pack module: " + t);
    }
    if (kinds.size() != 1 || (require_template && *kinds.begin() != "world_templates"))
        throw std::runtime_error("unsupported pack manifest for this file type");
    moves.emplace_back(dir, shared / *kinds.begin() / id);
}

// Stop at a content root so bundled subpacks and world resources stay with their owner.
void discover(const fs::path &dir, const fs::path &shared, Moves &moves, bool bundle, bool is_template,
              std::vector<fs::path> &nested) {
    if (fs::is_regular_file(dir / "manifest.json")) {
        pack(dir, shared, moves, is_template);
        return;
    }
    for (const auto &e : fs::directory_iterator(dir)) {
        if (e.path().filename() == "__MACOSX") continue;
        if (e.is_directory()) discover(e.path(), shared, moves, bundle, is_template, nested);
        else if (bundle && lower(e.path().extension()) == ".mcpack") nested.push_back(e.path());
    }
}
}

void install_content(const fs::path &profile, const fs::path &file, const std::string &player) {
    const auto ext = lower(file.extension());
    fs::create_directories(profile);
    Stage stage{profile / (".import-" + uuid())};
    if (!fs::create_directory(stage.path)) throw std::runtime_error("cannot create import staging directory");
    Budget budget;
    auto tree = stage.path / "content";
    unpack(file, tree, budget);
    Moves moves;
    const auto users = profile / "Users";
    if (ext == ".mcworld") {
        const auto dest = player_storage(users, player) / "minecraftWorlds";
        for (const auto &e : fs::recursive_directory_iterator(tree)) {
            if (e.is_regular_file() && e.path().filename() == "level.dat" && fs::is_directory(e.path().parent_path() / "db"))
                moves.emplace_back(e.path().parent_path(), dest / uuid());
        }
        if (moves.size() != 1) throw std::runtime_error("expected one world containing level.dat and db/");
    } else {
        const auto shared = users / "Shared/games/com.mojang";
        std::vector<fs::path> nested;
        discover(tree, shared, moves, ext == ".mcaddon", ext == ".mctemplate", nested);
        for (const auto &archive : nested) {
            const auto sub = stage.path / uuid();
            unpack(archive, sub, budget);
            const auto before = moves.size();
            std::vector<fs::path> unused;
            discover(sub, shared, moves, false, false, unused);
            if (moves.size() != before + 1) throw std::runtime_error("invalid embedded .mcpack");
        }
        if (moves.empty() || (ext != ".mcaddon" && moves.size() != 1))
            throw std::runtime_error("expected a valid pack manifest");
    }

    for (auto &[src, dest] : moves) {
        if (dest.parent_path().filename() == "world_templates")
            dest = player_storage(users, player) / "world_templates" / dest.filename();
    }

    std::set<fs::path> targets;
    for (const auto &[src, dest] : moves) {
        if (fs::weakly_canonical(dest.parent_path()) != fs::weakly_canonical(profile) / dest.parent_path().lexically_relative(profile))
            throw std::runtime_error("content directory crosses a symbolic link: " + dest.string());
        if (!targets.insert(dest).second || fs::exists(fs::symlink_status(dest)) ||
            (ext != ".mcworld" && installed_pack(dest)))
            throw std::runtime_error("content already present: " + dest.string());
        fs::create_directories(dest.parent_path());
    }
    size_t committed = 0;
    try {
        for (const auto &[src, dest] : moves) {
            fs::rename(src, dest);
            ++committed;
        }
    } catch (...) {
        while (committed) {
            const auto &[src, dest] = moves[--committed];
            fs::rename(dest, src);
        }
        throw;
    }
    for (const auto &[src, dest] : moves) std::cout << "Imported " << dest << '\n';
}
