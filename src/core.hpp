#pragma once
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Args = std::vector<std::string>;
using Env = std::map<std::string, std::string>;
std::string uuid();
bool is_uuid(const std::string &);
void valid_name(const std::string &);
fs::path root();
fs::path runtime();
Json read_json(const fs::path &);
void write_file(const fs::path &, const std::string &);
void save(const fs::path &, const Json &);
class Lock {
    int fd;

  public:
    Lock(const fs::path &, bool shared = false, bool wait = true);
    Lock(const Lock &) = delete;
    ~Lock();
};
struct Result {
    int status;
    std::string output;
};
pid_t spawn_process(Args, const Env &, int input, int output, int error);
Result process(Args, const Env & = {}, bool capture = false);
std::string checked(Args, const Env & = {}, bool capture = false);
bool confirm(const std::string &, bool yes = false);
std::vector<Json> records(const std::string &);
Json lookup(const std::vector<Json> &, const std::string &, const std::string &key = "name");
fs::path record_path(const std::string &, const std::string &);
void put(const std::string &, const Json &);
std::string executable(const std::string &);
void tools_command(const std::string &, const std::string &);
Json default_account();
Env account_env(const Json &);
void auth_command(const std::string &, const std::string &);
void services_command(const std::string &, const std::string &);
void ensure_service(const Json &);
bool service_running(const Json &);
void services_ciks(const Json &);
void update_profile_version(Json &profile, const Json &account);
Json version_resolve(const std::string &);
void versions_command(const std::string &, const std::string &, bool);
void profile_command(const std::string &, const std::string &, const std::string &);
void worker(const std::string &, const std::string &);
void env_command(const std::string &);
void install_dependencies(const std::string &tool);
void profile_startup(const std::string &selector, const std::string &setting, const std::string &action, const Args &values);
Env xvdtool_environment();
void import_content(const std::string &selector, const fs::path &file, const std::string &player, bool in_game);
void import_default(const std::string &selector);
void shortcut_command(const std::string &action, const std::string &selector);
extern const Args import_extensions;
void install_content(const fs::path &profile_dir, const fs::path &file, const std::string &player);
