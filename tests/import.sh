#!/usr/bin/env bash
set -euo pipefail
cli=$(realpath "${1:-build/bedrock}")
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
export HOME="$tmp/home" XDG_CONFIG_HOME="$tmp/config with spaces" XDG_DATA_HOME="$tmp/data"
export XDG_CURRENT_DESKTOP=generic
unset DESKTOP_SESSION KDE_FULL_SESSION
mkdir -p "$tmp/bin"
printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/notify-send"
chmod +x "$tmp/bin/notify-send"
export PATH="$tmp/bin:$PATH"
root="$XDG_CONFIG_HOME/bedrockoverlinux"
mkdir -p "$root/accounts" "$HOME" "$tmp/work/pack" "$tmp/work/world/db" "$tmp/work/bundle"
printf '{"name":"QA"}' > "$root/accounts/$(cat /proc/sys/kernel/random/uuid).json"
a=$("$cli" profile create Alpha)
b=$("$cli" profile create Beta --version latest_release)
pack=$(cat /proc/sys/kernel/random/uuid)
manifest() { printf '{"header":{"uuid":"%s"},"modules":[{"type":"%s"}]}' "$1" "$2" > "$tmp/work/pack/manifest.json"; }
zipdir() { bsdtar --format zip -cf "$1" -C "$2" .; }
reject() { if "$@" >"$tmp/err" 2>&1; then echo "unexpected success: $*" >&2; exit 1; fi; }
manifest "$pack" resources
zipdir "$tmp/a pack.MCPACK" "$tmp/work/pack"
"$cli" profile Alpha import "$tmp/a pack.MCPACK"
test -f "$root/profiles/$a/Users/Shared/games/com.mojang/resource_packs/$pack/manifest.json"
reject "$cli" profile Alpha import "$tmp/a pack.MCPACK"
packs="$root/profiles/$a/Users/Shared/games/com.mojang/resource_packs"
mv "$packs/$pack" "$packs/InstalledByMinecraft"
reject "$cli" profile Alpha import "$tmp/a pack.MCPACK"
"$cli" import default set Beta
test "$(jq -r .profile "$root/import.json")" = "$b"
"$cli" import default info | grep -F "$b"
desktop="$XDG_DATA_HOME/applications/bedrockoverlinux-import.desktop"
desktop-file-validate "$desktop"
type=$(LC_ALL=C gio info -a standard::content-type "$tmp/a pack.MCPACK" | sed -n "s/^  standard::content-type: //p")
test "$type" = application/x-bedrockoverlinux-mcpack
test "$(xdg-mime query default "$type")" = bedrockoverlinux-import.desktop
cat > "$tmp/bin/systemd-run" <<'SH'
#!/bin/sh
printf '%s\n' "$@" > "$HOME/launch-argv"
SH
chmod +x "$tmp/bin/systemd-run"
export BEDROCK_WINE=/usr/bin/true BEDROCK_XODUS_CLI=/usr/bin/true BEDROCK_XODUS_SERVICE=/usr/bin/true
# Launch the installed entry with the desktop parser and a different caller config.
XDG_CONFIG_HOME="$tmp/wrong" gio launch "$desktop" "$tmp/a pack.MCPACK"
for i in {1..50}; do
    test -f "$HOME/launch-argv" && break
    sleep .1
done
grep -Fx -- "--setenv=BEDROCK_GAME_IMPORT_FILE=$tmp/a pack.MCPACK" "$HOME/launch-argv"
grep -Fx "$b" "$HOME/launch-argv"
test ! -d "$root/profiles/$b/Users"
printf 'world' > "$tmp/work/world/level.dat"
touch "$tmp/work/world/db/CURRENT"
zipdir "$tmp/example.mcworld" "$tmp/work/world"
reject "$cli" profile Beta import "$tmp/example.mcworld"
users="$root/profiles/$b/Users"
mkdir -p "$users/42" "$users/35"
reject "$cli" profile Beta import "$tmp/example.mcworld"
"$cli" profile Beta import "$tmp/example.mcworld" --player 42
test "$(cat "$users"/42/games/com.mojang/minecraftWorlds/*/level.dat)" = world
manifest "$pack" world_template
zipdir "$tmp/example.mctemplate" "$tmp/work/pack"
"$cli" profile Beta import "$tmp/example.mctemplate" --player 35
test -f "$users/35/games/com.mojang/world_templates/$pack/manifest.json"
behavior=$(cat /proc/sys/kernel/random/uuid)
manifest "$behavior" data
zipdir "$tmp/work/bundle/b.mcpack" "$tmp/work/pack"
cp "$tmp/a pack.MCPACK" "$tmp/work/bundle/a.mcpack"
zipdir "$tmp/bundle.mcaddon" "$tmp/work/bundle"
reject "$cli" profile Alpha import "$tmp/bundle.mcaddon"
test ! -d "$root/profiles/$a/Users/Shared/games/com.mojang/behavior_packs/$behavior"
c=$("$cli" profile create Gamma)
"$cli" profile Gamma import "$tmp/bundle.mcaddon"
test -f "$root/profiles/$c/Users/Shared/games/com.mojang/behavior_packs/$behavior/manifest.json"
ln -s /etc/passwd "$tmp/work/pack/link"
zipdir "$tmp/link.mcpack" "$tmp/work/pack"
reject "$cli" profile Gamma import "$tmp/link.mcpack"
grep -F 'archive contains a link' "$tmp/err"
echo IMPORT_OK
