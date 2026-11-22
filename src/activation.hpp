#pragma once
#include "core.hpp"

// This directory exists only while the worker owns the Wine prefix.
struct ActivationSession {
    fs::path directory;
    explicit ActivationSession(const std::string &session);
    ~ActivationSession();
};
std::string wine_file_path(const fs::path &);
void forward_file(const Json &profile, const fs::path &file);
