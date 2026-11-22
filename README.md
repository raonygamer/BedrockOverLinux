# BedrockOverLinux

**English** · [Português (Brasil)](README.pt-BR.md)

A C++ command-line tool for running Minecraft Bedrock for Windows on Linux. Use `bedrock` to see the available commands or add `--help` to any command.

## Installation

On x86_64 Arch Linux, Ubuntu, Debian or Fedora, clone the repository and run the installer.

```sh
git clone https://github.com/raonygamer/BedrockOverLinux.git
cd BedrockOverLinux
./install.sh
```

The installer installs build dependencies, compiles the CLI into `~/.local/bin`, and installs the external tools in your configuration directory. Use `./install.sh --cli-only` to install just the CLI. On apt-based systems that lack a .NET 8 package, xvdtool uses a private copy of the official Microsoft runtime verified with SHA-512. Playing requires a graphical session with a systemd user manager, a working Secret Service and Vulkan drivers.

With `~/.local/bin` on your PATH, sign in with the Microsoft account that owns Minecraft for Windows and create a profile that follows the latest release.

```sh
bedrock tools check
bedrock auth login
bedrock profile create survival --version latest_release
bedrock profile survival launch
```

The first launch downloads the game. To pin a specific version, find it with `bedrock version list`, download it with `bedrock version <version> download` and select it with `bedrock profile survival version set <version>`.

## External tools

`bedrock tools list` shows local paths. Use `tools info [tool]` to inspect the selected installation or `tools check [tool]` to check upstream releases and test the local executable. The `tools install [tool]` and `tools update [tool]` commands download the Linux release, verify its SHA-256 against the digest published by GitHub and test the executable before activating it. Omit the tool name to operate on all three. Each release has its own directory, so running processes keep using their original files after an update.

- Xodus handles Microsoft sign-in, licensing, game installation and launch. Its Linux binary needs native libraries including WebKitGTK 4.1 and a working desktop Secret Service. [Xodus](https://github.com/BedrockNative/xodus)
- WineGDK runs the Windows game client. [WineGDK](https://github.com/BedrockNative/WineGDK)
- xvdtool inspects package metadata with `bedrock version <version|uuid> info --package`. Its v1.0.3 Linux release requires .NET 8. [xvdtool](https://github.com/AmethystAPI/xvdtool)

To use your own installations, set absolute paths in `BEDROCK_XODUS_CLI`, `BEDROCK_XODUS_SERVICE`, `BEDROCK_WINE`, `BEDROCK_WINESERVER` or `BEDROCK_XVDTOOL`. The CLI looks for WineGDK as `winegdk` on your PATH.

## Commands

```text
bedrock auth login
bedrock auth list
bedrock auth refresh
bedrock auth logout <gamertag>
bedrock auth default info
bedrock auth default set <gamertag>

bedrock profile list
bedrock profile create <name> [--version <version|uuid|latest_release|latest_preview>]
bedrock profile delete <name|uuid>
bedrock profile <name|uuid> delete
bedrock profile <name|uuid> info
bedrock profile <name|uuid> launch
bedrock profile <name|uuid> env list
bedrock profile <name|uuid> env set <key> <value>
bedrock profile <name|uuid> env unset <key>
bedrock profile <name|uuid> prelaunch|wrapper|args info
bedrock profile <name|uuid> prelaunch|wrapper|args set -- <arguments...>
bedrock profile <name|uuid> prelaunch|wrapper|args clear
bedrock profile <name|uuid> account info
bedrock profile <name|uuid> account set <gamertag>
bedrock profile <name|uuid> version info
bedrock profile <name|uuid> version set <version|uuid|latest_release|latest_preview>
bedrock profile <name|uuid> prefix info
bedrock profile <name|uuid> prefix set <prefix>
bedrock prefix list

bedrock shortcut add <name|uuid>
bedrock shortcut info <name|uuid>
bedrock shortcut delete <name|uuid>
bedrock shortcut list

bedrock services list
bedrock services info <gamertag>
bedrock services licence <gamertag>
bedrock services ciks <gamertag>
bedrock services kill <gamertag>

bedrock version list
bedrock version <version|uuid> info [--package]
bedrock version <version> download [--copy]

bedrock env COM_MOJANG set <name|uuid>
```

Once Xodus confirms the account, the CLI saves the login. If the gamertag lookup fails, select the account by its listed UUID or Microsoft account name. Run `bedrock auth refresh` to retry the lookup.

Profile commands require a signed-in account. The first use creates the `release` and `preview` profiles. Each profile stores the UUID of the default account at the time of creation. If you create a profile without a version, use `version set` before launching. You can select profiles and accounts by name or gamertag on the command line. Saved references use UUIDs.

With `--version latest_release` or `--version latest_preview`, the profile checks its channel at each launch and installs the shared copy if needed. When the catalog is offline, the profile can launch its previously resolved installation. Use `version set` with a version number or installation UUID to pin a version again.

A version number selects the shared installation. Downloading it again asks for confirmation and keeps the same UUID. Each download with `--copy` gets a new UUID. An interrupted download can leave a hidden staging directory while preserving the previous installation. [Version catalog](https://github.com/LukasPAH/minecraft-windows-gdk-version-db)

For example, `bedrock profile survival env set MANGOHUD 1` sets an environment variable for that profile and `bedrock profile survival wrapper set -- gamemoderun` runs its launch command through GameMode. A `prelaunch` command runs in the profile directory and must succeed for launch to continue. Use `args` to pass individual arguments to Minecraft and shell quotes to preserve spaces. Commands receive `BEDROCK_PROFILE_DIR`, `BEDROCK_GAME_DIR` and the profile's environment.

Deleting a profile asks you to enter `y` and then its exact name. This removes that profile's worlds, packs and settings. You can also delete the two initial profiles.

All profiles start with the `default` Wine prefix. Use `prefix set` to select an existing prefix or create one by name. Each profile has its own game data even when several profiles share a prefix. A running game locks its prefix. The profile list shows availability in text and uses green and red on color terminals. Launch failures also trigger a desktop notification.

Each login has a private Xodus directory. The account's systemd user service starts on demand. The `services kill` command asks for confirmation if the service is in use. The `services ciks` command asks Xodus for the licensed content keys for installed Bedrock packages and prints them to standard output.

Run `bedrock shortcut add release` to add **Minecraft Bedrock - release** to your application menu. The desktop entry launches the profile by UUID without opening a terminal. It lives in `$XDG_DATA_HOME/applications` (default `~/.local/share/applications`) for your desktop user. Run `add` again after moving the CLI to refresh its path. If you already deleted the profile, use the UUID from `shortcut list` to remove its shortcut.

## Files

The default configuration directory is `~/.config/bedrockoverlinux`. The CLI respects `XDG_CONFIG_HOME`.

```text
profiles/<uuid>.json
profiles/<uuid>/Users/Shared/games/com.mojang/
prefixes/default/
prefix-registry/<uuid>.json
versions/client/<uuid>/installation.json
versions/client/<uuid>/game/
accounts/<uuid>.json
xodus/<uuid>/
tools/<tool>/<tag>/
```

`bedrock env COM_MOJANG set <profile>` writes the absolute path of the profile's `Shared/games/com.mojang` directory to `/etc/environment` using sudo. It asks before replacing an existing value. This machine-wide setting takes effect in shells started from a new login session that reads the file. Existing sessions keep their environment, and access to worlds follows the profile's filesystem permissions.

## Tests

After building, run `bash tests/cli.sh build/bedrock` for offline CLI tests covering concurrent profile creation, deletion confirmation and arguments containing spaces.

Run `bash tests/integration.sh build/bedrock` in a session with a systemd user manager, Perl and jq. The suite uses temporary storage, test accounts and a test service on a Unix socket. Testing real sign-in, ownership and gameplay requires an account that owns Minecraft for Windows.
