#include "core.hpp"
#include <CLI/CLI.hpp>
#include <iostream>
#include <sys/stat.h>

int main(int argc, char **argv) {
    umask(0077);
    CLI::App app{"Minecraft Bedrock on Linux", "bedrock"};
    std::string selector, value, tool;
    bool copy = false, notify = false, package_info = false;
    std::string parsed_selector;
    Args startup_values;
    std::string env_key, env_value;
    std::function<void()> action;
    auto group = [](CLI::App *parent, const std::string &name, const std::string &description) {
        auto node = parent->add_subcommand(name, description);
        node->require_subcommand(1, 1);
        return node;
    };
    auto leaf = [&](CLI::App *parent, const std::string &name, const std::string &description,
                    std::function<void()> fn) {
        auto node = parent->add_subcommand(name, description);
        node->callback([&, fn] { action = fn; });
        return node;
    };
    // Normalize the dynamic selector before handing the command tree to CLI11.
    auto selected = [&](CLI::App *parent, const std::string &domain, const std::string &label) {
        if (argc >= 3 && argv[1] == domain && argv[2][0] != '-' && parent->get_subcommands([&](const CLI::App *cmd) { return cmd->get_name() == argv[2]; }).empty()) {
            selector = argv[2];
            parsed_selector = label;
            argv[2] = parsed_selector.data();
        }
        return group(parent, label, "Address by " + label);
    };
    auto profiles = group(&app, "profile", "Profiles, accounts, installations and Wine prefixes");
    leaf(profiles, "list", "List profiles and launch status", [&] { profile_command("list", "", ""); });
    auto create =
        leaf(profiles, "create", "Create a profile", [&] { profile_command("create", selector, value); });
    create->add_option("name", selector)->required();
    create->add_option("--version", value, "Installed version number, UUID, latest_release or latest_preview");
    auto delete_profile = [&] { profile_command("delete", selector, ""); };
    leaf(profiles, "delete", "Delete a profile and its saved data", delete_profile)
        ->add_option("name_or_uuid", selector)->required();
    auto target = selected(profiles, "profile", "NAME_OR_UUID");
    leaf(target, "delete", "Delete this profile and its saved data", delete_profile);
    leaf(target, "info", "Show profile metadata", [&] { profile_command("info", selector, ""); });
    leaf(target, "launch", "Start Minecraft", [&] {
        notify = true;
        profile_command("launch", selector, "");
    });
    for (std::string key : {"account", "version", "prefix"}) {
        auto section = group(target, key, "Profile " + key);
        leaf(section, "info", "Show " + key, [&, key] { profile_command(key + " info", selector, ""); });
        leaf(section, "set", "Assign " + key, [&, key] { profile_command(key + " set", selector, value); })
            ->add_option("value", value, key == "version" ? "Version number, UUID, latest_release or latest_preview" : "")
            ->required();
    }
    auto variables = group(target, "env", "Environment for this profile");
    leaf(variables, "list", "Show configured variables", [&] { profile_startup(selector, "env", "list", {}); });
    auto assign = leaf(variables, "set", "Set one variable", [&] {
        profile_startup(selector, "env", "set", {env_key, env_value});
    });
    assign->add_option("key", env_key)->required();
    assign->add_option("value", env_value)->required();
    leaf(variables, "unset", "Remove one profile override", [&] {
        profile_startup(selector, "env", "unset", {env_key});
    })->add_option("key", env_key)->required();
    for (std::string setting : {"prelaunch", "wrapper", "args"}) {
        auto options = group(target, setting, setting == "args" ? "Minecraft arguments" :
                             setting == "wrapper" ? "Command wrapping the game session" : "Command to run before launch");
        for (std::string action : {"info", "set", "clear"}) {
            auto cmd = leaf(options, action, action + " " + setting, [&, setting, action] {
                profile_startup(selector, setting, action, startup_values);
            });
            if (action == "set") cmd->add_option("argv", startup_values, "Literal arguments; use -- before flags")->required();
        }
    }
    auto shortcuts = group(&app, "shortcut", "Application menu shortcuts for profiles");
    leaf(shortcuts, "list", "List installed profile shortcuts", [] { shortcut_command("list", ""); });
    for (const auto &[op, description] : std::map<std::string, std::string>{
             {"add", "Install or refresh a profile shortcut"}, {"info", "Show the desktop entry and its path"},
             {"delete", "Remove a profile shortcut"}}) {
        auto cmd = leaf(shortcuts, op, description, [&, operation = op] { shortcut_command(operation, selector); });
        cmd->add_option("name_or_uuid", selector)->required();
    }
    auto prefixes = group(&app, "prefix", "Wine prefixes");
    leaf(prefixes, "list", "List prefixes and active profiles", [] { profile_command("prefix list", "", ""); });
    auto auth = group(&app, "auth", "Microsoft accounts");
    for (std::string key : {"login", "logout", "list"}) {
        auto node = leaf(auth, key, key + " Microsoft accounts", [&, key] { auth_command(key, value); });
        if (key == "logout")
            node->add_option("gamertag", value)->required();
    }
    leaf(auth, "refresh", "Retry pending Xbox gamertags", [] { auth_command("refresh", ""); });
    auto def = group(auth, "default", "Default account");
    leaf(def, "info", "Show default account", [] { auth_command("default info", ""); });
    leaf(def, "set", "Set default account", [&] { auth_command("default set", value); })
        ->add_option("gamertag", value)
        ->required();
    auto services = group(&app, "services", "Account-specific Xodus services");
    for (std::string key : {"list", "info", "licence", "ciks", "kill"}) {
        auto node = leaf(services, key, key + " for Xodus", [&, key] { services_command(key, value); });
        if (key != "list")
            node->add_option("gamertag", value)->required();
    }
    auto versions = group(&app, "version", "Catalog and installed versions");
    leaf(versions, "list", "List catalog and installations", [] { versions_command("list", "", false); });
    auto build = selected(versions, "version", "VERSION_OR_UUID");
    leaf(build, "info", "Show version details", [&] {
        versions_command(package_info ? "package info" : "info", selector, false);
    })->add_flag("--package", package_info, "Inspect installed XVD package with external xvdtool");
    leaf(build, "download", "Install or re-download shared installation", [&] {
        versions_command("download", selector, copy);
    })->add_flag("--copy", copy, "Create an independent installation");
    auto tools = group(&app, "tools", "Independently managed Xodus, WineGDK and xvdtool");
    for (std::string key : {"list", "check", "info", "install", "update"}) {
        auto node = leaf(tools, key, key + " external tools", [&, key] { tools_command(key, tool); });
        if (key != "list")
            node->add_option("tool", tool, "xodus, winegdk or xvdtool");
    }
    auto env = group(&app, "env", "System-wide environment");
    auto mojang = group(env, "COM_MOJANG", "Shared com.mojang folder");
    leaf(mojang, "set", "Set for future system login sessions", [&] { env_command(value); })
        ->add_option("profile", value)
        ->required();
    auto internal = leaf(&app, "_run", "Internal game supervisor", [&] { worker(value); });
    internal->group("");
    internal->add_option("uuid", value)->required();
    if (argc == 1) {
        std::cout << app.help();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "auth") {
        std::cout << auth->help();
        return 0;
    }
    CLI11_PARSE(app, argc, argv);
    try {
        if (!action)
            throw std::runtime_error("expected a command; see bedrock --help");
        action();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "bedrock: " << e.what() << '\n';
        if (notify)
            process({"notify-send", "--app-name=BedrockOverLinux", "--urgency=critical", "--",
                     "Minecraft Bedrock", e.what()});
        return 1;
    }
}
