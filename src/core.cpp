#include "core.hpp"
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <regex>
#include <sys/file.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

std::string uuid() {
    std::string value;
    if (!(std::ifstream("/proc/sys/kernel/random/uuid") >> value))
        throw std::runtime_error("UUID unavailable");
    return value;
}
bool is_uuid(const std::string &s) {
    static const std::regex pattern("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
    return std::regex_match(s, pattern);
}
void valid_name(const std::string &s) {
    static const std::regex pattern("[a-zA-Z0-9][a-zA-Z0-9_.-]{0,63}");
    if (!std::regex_match(s, pattern) || is_uuid(s))
        throw std::runtime_error("invalid name: " + s);
}
fs::path root() {
    const char *base = getenv("XDG_CONFIG_HOME");
    if (base && fs::path(base).is_absolute())
        return fs::path(base) / "bedrockoverlinux";
    const char *home = getenv("HOME");
    if (!home || !fs::path(home).is_absolute())
        throw std::runtime_error("HOME unavailable");
    return fs::path(home) / ".config/bedrockoverlinux";
}
fs::path runtime() {
    auto base = getenv("XDG_RUNTIME_DIR");
    if (!base || !fs::path(base).is_absolute())
        throw std::runtime_error("XDG_RUNTIME_DIR unavailable");
    return base;
}
Json read_json(const fs::path &p) {
    std::ifstream f(p);
    if (!f)
        throw std::runtime_error("cannot read " + p.string());
    return Json::parse(f);
}
void write_file(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    auto tmp = path.string() + "." + uuid() + ".tmp";
    try {
        std::ofstream out;
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out.open(tmp);
        out << text;
        out.close();
        fs::rename(tmp, path);
    } catch (...) {
        fs::remove(tmp);
        throw;
    }
}
void save(const fs::path &p, const Json &j) { write_file(p, j.dump(2) + "\n"); }
Lock::Lock(const fs::path &path, bool shared, bool wait) {
    fs::create_directories(path.parent_path());
    fd = open(path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), path.string());
    if (flock(fd, (shared ? LOCK_SH : LOCK_EX) | (wait ? 0 : LOCK_NB)) < 0) {
        int e = errno;
        close(fd);
        throw std::system_error(e, std::generic_category(), path.string());
    }
}
Lock::~Lock() { close(fd); }

pid_t spawn_process(Args args, const Env &env, int input, int output, int error) {
    pid_t pid = fork();
    if (pid < 0) throw std::system_error(errno, std::generic_category(), "fork");
    if (pid == 0) {
        if ((input >= 0 && dup2(input, STDIN_FILENO) < 0) ||
            (output >= 0 && dup2(output, STDOUT_FILENO) < 0) ||
            (error >= 0 && dup2(error, STDERR_FILENO) < 0)) _exit(127);
        for (const auto &[k, v] : env)
            setenv(k.c_str(), v.c_str(), 1);
        std::vector<char *> argv;
        for (auto &arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        perror(argv[0]);
        _exit(127);
    }
    return pid;
}

Result process(Args args, const Env &env, bool capture) {
    int pipefd[2]{-1, -1};
    if (capture && pipe2(pipefd, O_CLOEXEC))
        throw std::system_error(errno, std::generic_category(), "pipe");
    pid_t pid;
    try {
        pid = spawn_process(std::move(args), env, -1, pipefd[1], -1);
    } catch (...) {
        if (capture) { close(pipefd[0]); close(pipefd[1]); }
        throw;
    }
    std::string output;
    if (capture) {
        close(pipefd[1]);
        char buf[8192];
        for (;;) {
            auto n = read(pipefd[0], buf, sizeof buf);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            output.append(buf, n);
        }
        close(pipefd[0]);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "waitpid");
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), output};
}
std::string checked(Args args, const Env &env, bool capture) {
    auto r = process(args, env, capture);
    if (r.status)
        throw std::runtime_error(args.front() + " exited " + std::to_string(r.status));
    return r.output;
}
bool confirm(const std::string &prompt, bool yes) {
    std::cerr << prompt << (yes ? " [Y/n] " : " [y/N] ");
    std::string reply;
    if (!std::getline(std::cin, reply))
        return false;
    return reply == "y" || reply == "Y" || (reply.empty() && yes);
}
fs::path record_path(const std::string &group, const std::string &id) {
    if (!is_uuid(id))
        throw std::runtime_error("invalid UUID: " + id);
    return root() / group / (id + ".json");
}
std::vector<Json> records(const std::string &group) {
    std::vector<Json> rows;
    auto directory = root() / group;
    if (fs::exists(directory))
        for (const auto &e : fs::directory_iterator(directory)) {
            if (e.path().extension() != ".json")
                continue;
            auto id = e.path().stem().string();
            if (!is_uuid(id))
                throw std::runtime_error("invalid record filename: " + e.path().string());
            auto row = read_json(e.path());
            row["uuid"] = id;
            rows.push_back(row);
        }
    std::sort(rows.begin(), rows.end(),
              [](const Json &a, const Json &b) { return a.value("name", "") < b.value("name", ""); });
    return rows;
}
void put(const std::string &group, const Json &row) { save(record_path(group, row.at("uuid")), row); }
Json lookup(const std::vector<Json> &rows, const std::string &selector, const std::string &key) {
    for (const auto &row : rows)
        if (row.at("uuid") == selector || row.value(key, "") == selector)
            return row;
    throw std::runtime_error("unknown identifier: " + selector);
}
