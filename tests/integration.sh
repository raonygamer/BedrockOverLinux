#!/usr/bin/env bash
# Requires a user systemd manager, jq and Perl. No Microsoft account or game download.
set -euo pipefail
cli=$(realpath "${1:-build/bedrock}")
scratch=$(mktemp -d)
export XDG_CONFIG_HOME="$scratch/config"
export BEDROCK_XODUS_CLI="$scratch/xodus-cli" BEDROCK_XODUS_SERVICE="$scratch/xodus-service"
export BEDROCK_WINE="$scratch/wine"
cleanup() {
    for f in "$XDG_CONFIG_HOME"/bedrockoverlinux/accounts/*.json; do
        if test -f "$f"; then systemctl --user stop "bedrock-xodus-$(basename "$f" .json).service" 2>/dev/null || true; fi
    done
    rm -rf "$scratch"
}
trap cleanup EXIT
cat > "$BEDROCK_XODUS_SERVICE" <<'PERL'
#!/usr/bin/perl
use IO::Socket::UNIX;
unlink $ENV{XODUS_SOCKET};
my $s=IO::Socket::UNIX->new(Type=>SOCK_STREAM,Local=>$ENV{XODUS_SOCKET},Listen=>8) or die $!;
while (my $c=$s->accept()) {
    if (read($c, my $header, 8) == 8) {
        my ($magic,$kind,$length)=unpack("Vvv",$header);
        read($c, my $request, $length);
        my $body="";
        if (-e "$ENV{XODUS_CONFIG_DIR}/broker-test") {
            if ($kind==3) { open(my $ready, ">", "$ENV{XODUS_CONFIG_DIR}/msa-ready") or die $!; close $ready; $body="<MSATokenResponse/>"; }
            if ($kind==7 && -e "$ENV{XODUS_CONFIG_DIR}/msa-ready") { $body="<XboxTokenResponse><UniqueModernGamertag>Other</UniqueModernGamertag></XboxTokenResponse>"; }
        }
        print $c pack("Vvv",$magic,$kind+1,length($body)).$body;
    }
    close $c;
}
PERL
cat > "$BEDROCK_XODUS_CLI" <<'SH'
#!/bin/sh
set -eu
case "$1" in
login) echo "${TEST_TAG:-Player}" > "$XODUS_CONFIG_DIR/tag"; if test "${TEST_BROKER:-0}" = 1; then touch "$XODUS_CONFIG_DIR/broker-test"; fi;;
accounts)
    case "$2" in
    list) jq -n --arg tag "$(cat "$XODUS_CONFIG_DIR/tag")" '[{id:$tag,username:("MS-"+$tag),active:true}]';;
    profile) test "${FAIL_GAMERTAG:-0}" = 0 || exit 1; jq -n --arg tag "$(cat "$XODUS_CONFIG_DIR/tag")" '{gamertag:$tag}';;
    esac;;
check-ownership) echo '{"status":"purchased"}';;
install-owned)
    test "${FAIL_INSTALL:-0}" = 0 && test ! -e "$XDG_CONFIG_HOME/fail-download" || exit 1
    mkdir -p "$4"
    echo fixture > "$4/Minecraft.Windows.exe"
    case "$3" in
    */preview.msixvc) printf '<Game/>' > "$4/MicrosoftGame.Config";;
    *) printf '<Package><Identity Name="Fixture" Publisher="CN=Fixture" Version="1.0.0.0"/></Package>' > "$4/appxmanifest.xml";;
    esac
    if test -e "$XDG_CONFIG_HOME/truncated-package"; then touch "$4/.xodus-streaming.msixvc"; exit 0; fi
    truncate -s 560 "$4/.xodus-streaming.msixvc"
    printf msft-xvd | dd of="$4/.xodus-streaming.msixvc" bs=1 seek=512 conv=notrunc status=none;;
run)
    if test "${QA_STARTUP:-}" = 'value with spaces'; then
        test -f "$BEDROCK_PROFILE_DIR/prelaunch-ran"
        test -f "$BEDROCK_PROFILE_DIR/wrapper-ran"
        test "$PWD" = "$BEDROCK_PROFILE_DIR"
        test "$2" = "$BEDROCK_GAME_DIR"
        test "$7" = --
        test "$8" = --fixture
        test "$9" = 'literal $HOME'
    fi
    if test -f "$2/MicrosoftGame.Config"; then test ! -e "$2/MicrosoftGame.config"
    else test -f "$2/MicrosoftGame.config"; fi
    sleep 3;;
license) mkdir -p "$3"; truncate -s 48 "$3/00000000-0000-0000-0000-000000000000.cik";;
logout) rm -f "$XODUS_CONFIG_DIR/tag";;
*) exit 2;;
esac
SH
cat > "$BEDROCK_WINE" <<'SH'
#!/bin/sh
mkdir -p "$WINEPREFIX/drive_c/users/$(id -un)/AppData/Roaming"
touch "$WINEPREFIX/system.reg"
SH
printf '#!/bin/sh\nexit 0\n' > "$scratch/wineserver"
chmod +x "$scratch"/{xodus-cli,xodus-service,wine,wineserver}
# Keep catalog requests local, exercising the external command protocol.
cat > "$scratch/curl" <<'SH'
#!/bin/sh
test ! -e "$XDG_CONFIG_HOME/offline" || exit 1
release=1.2.3
if test -e "$XDG_CONFIG_HOME/update"; then release=1.2.10; fi
printf '{"release":"%s","preview":"1.2.9","releaseVersions":[{"version":"Release 1.2.3","urls":["https://assets1.xboxlive.com/test.msixvc"]},{"version":"Release 1.2.10","urls":["https://assets1.xboxlive.com/next.msixvc"]}],"previewVersions":[{"version":"Preview 1.2.9","urls":["https://assets1.xboxlive.com/preview.msixvc"]}]}' "$release"
SH
chmod +x "$scratch/curl"
printf '#!/bin/sh\nexit 0\n' > "$scratch/notify-send"
chmod +x "$scratch/notify-send"
export PATH="$scratch:$PATH"
"$cli" | cmp - <("$cli" --help)
if "$cli" profile list; then exit 1; fi
FAIL_GAMERTAG=1 "$cli" auth login
"$cli" auth list | grep -F '* MS-Player' | grep -F 'gamertag pending'
before=$("$cli" auth default info | jq -r .uuid)
"$cli" profile list | grep release
if FAIL_GAMERTAG=1 "$cli" auth refresh; then exit 1; fi
"$cli" auth refresh
test "$before" = "$("$cli" auth default info | jq -r .uuid)"
if "$cli" auth login; then exit 1; fi
FAIL_GAMERTAG=1 TEST_BROKER=1 TEST_TAG=Other "$cli" auth login
"$cli" auth list | grep -F '* Player'
"$cli" auth default set Other
"$cli" auth default info | jq -e '.name=="Other"'
"$cli" auth default set Player
"$cli" version 1.2.3 info | jq -e '.downloaded==false'
"$cli" version 1.2.3 download
shared=$("$cli" version 1.2.3 info | jq -r .uuid)
"$cli" version 1.2.3 download --copy
copy=$("$cli" version list | awk '/ copy$/{print $2}')
test "$shared" != "$copy"
printf 'n\n' | "$cli" version 1.2.3 download
printf 'y\n' | "$cli" version 1.2.3 download
if printf 'y\n' | FAIL_INSTALL=1 "$cli" version 1.2.3 download; then exit 1; fi
test "$shared" = "$("$cli" version 1.2.3 info | jq -r .uuid)"
# A partial package must leave the existing files untouched and discard staging.
clients="$XDG_CONFIG_HOME/bedrockoverlinux/versions/client"
touch "$clients/$shared/keep-old" "$XDG_CONFIG_HOME/truncated-package"
if printf 'y\n' | "$cli" version 1.2.3 download >"$scratch/download-error" 2>&1; then exit 1; fi
grep -F 'invalid XVD header' "$scratch/download-error"
test -e "$clients/$shared/keep-old"
test -z "$(find "$clients" -name '.install-*' -print -quit)"
rm "$XDG_CONFIG_HOME/truncated-package"
# Track the catalog's declared head, reuse shared installs, and keep the previous UUID on failure.
"$cli" profile create tracking --version latest_release
"$cli" profile tracking version info | jq -e '.track=="latest_release" and .uuid==null'
touch "$XDG_CONFIG_HOME/offline"
if "$cli" profile tracking launch >"$scratch/launch-error" 2>&1; then exit 1; fi
grep -F 'catalog unavailable for first launch' "$scratch/launch-error"
"$cli" profile tracking version info | jq -e '.uuid==null'
rm "$XDG_CONFIG_HOME/offline"
"$cli" profile tracking launch
"$cli" profile tracking version info | jq -e --arg id "$shared" '.uuid==$id'
touch "$XDG_CONFIG_HOME/update" "$XDG_CONFIG_HOME/fail-download"
if "$cli" profile tracking launch; then exit 1; fi
"$cli" profile tracking version info | jq -e --arg id "$shared" '.uuid==$id'
rm "$XDG_CONFIG_HOME/fail-download"
"$cli" profile tracking launch
"$cli" profile tracking version info | jq -e '.version=="1.2.10" and .track=="latest_release"'
touch "$XDG_CONFIG_HOME/offline"
"$cli" profile tracking launch
rm "$XDG_CONFIG_HOME/offline"
"$cli" profile tracking version set latest_preview
"$cli" profile tracking launch
"$cli" profile tracking version info | jq -e '.version=="1.2.9" and .channel=="preview" and .track=="latest_preview"'
"$cli" profile tracking version set "$copy"
"$cli" profile tracking info | jq -e --arg id "$copy" '.version==$id and (has("track")|not)'
"$cli" profile create first --version 1.2.3
"$cli" profile create second --version "$copy"
"$cli" profile second account set Other
"$cli" profile second account info | jq -e '.name=="Other"'
"$cli" profile second version info | jq -e --arg id "$copy" '.uuid==$id'
cat > "$scratch/wrap" <<'SH'
#!/bin/sh
set -eu
touch "$BEDROCK_PROFILE_DIR/wrapper-ran"
exec "$@"
SH
chmod +x "$scratch/wrap"
"$cli" profile first env set QA_STARTUP 'value with spaces'
"$cli" profile first env list | jq -e '.QA_STARTUP=="value with spaces"'
if "$cli" profile first env set WINEPREFIX /tmp; then exit 1; fi
"$cli" profile first wrapper set -- "$scratch/wrap"
"$cli" profile first args set -- --fixture 'literal $HOME'
"$cli" profile first args info | jq -e '.==["--fixture","literal $HOME"]'
"$cli" profile first prelaunch set -- false
if "$cli" profile first launch; then exit 1; fi
"$cli" profile first prelaunch set -- touch prelaunch-ran
"$cli" profile first launch
"$cli" profile first env unset QA_STARTUP
for setting in wrapper prelaunch args; do
    "$cli" profile first "$setting" clear
    "$cli" profile first "$setting" info | jq -e '.==[]'
done
"$cli" profile first wrapper set -- "$scratch/missing-wrapper"
if "$cli" profile first launch; then exit 1; fi
# Interrupt an active process tree. Its profile and prefix must become reusable.
cat > "$scratch/wait-wrapper" <<'SH'
#!/bin/sh
echo "$$" > "$BEDROCK_PROFILE_DIR/wrapper.pid"
exec sleep 60
SH
chmod +x "$scratch/wait-wrapper"
"$cli" profile first wrapper set -- "$scratch/wait-wrapper"
profile_id=$("$cli" profile first info | jq -r .uuid)
prefix_id=$("$cli" profile first prefix info | jq -r .uuid)
pidfile="$XDG_CONFIG_HOME/bedrockoverlinux/profiles/$profile_id/wrapper.pid"
"$cli" profile first launch >"$scratch/interrupted" 2>&1 &
caller=$!
for _ in {1..100}; do test -s "$pidfile" && break; sleep .1; done
wrapper_pid=$(cat "$pidfile")
unit="bedrock-game-$prefix_id.service"
systemctl --user stop "$unit"
wait "$caller" || :  # systemd can report a requested stop as successful.
if kill -0 "$wrapper_pid" 2>/dev/null; then exit 1; fi
"$cli" prefix list | grep -F "$prefix_id available"
"$cli" profile first wrapper clear
"$cli" profile first launch &
playing=$!
for _ in {1..100}; do
    if "$cli" profile list | grep -q 'first .*prefix busy'; then break; fi
    sleep .1
done
"$cli" profile list | grep 'second .*prefix busy'
"$cli" prefix list | grep busy
if "$cli" profile first prefix set changed-while-running; then exit 1; fi
if "$cli" profile first env set WHILE_RUNNING changed; then exit 1; fi
if printf 'y\n' | "$cli" version 1.2.3 download; then exit 1; fi
test -e "$clients/$shared/keep-old"
if "$cli" profile second launch; then exit 1; fi
if printf 'y\nfirst\n' | "$cli" profile delete first; then exit 1; fi
printf 'n\n' | "$cli" services kill Player
wait "$playing"
"$cli" profile second prefix set another
"$cli" profile second prefix info | jq -e '.name=="another"'
"$cli" profile second version set "$shared"
"$cli" profile second launch &
second=$!
"$cli" profile first launch &
first=$!
wait "$first"
wait "$second"
"$cli" services info Player | grep running
"$cli" services licence Player
"$cli" services ciks Player | grep -q '0000000000000000$'
"$cli" services kill Player
"$cli" services kill Player | grep stopped
target=$("$cli" profile first info | jq -r .uuid)
data="$XDG_CONFIG_HOME/bedrockoverlinux/profiles/$target"
touch "$data/world-fixture"
printf 'n\n' | "$cli" profile delete first
printf 'y\nwrong-name\n' | "$cli" profile "$target" delete
printf 'y\n' | "$cli" profile delete first
test -f "$data/world-fixture"
printf 'y\nfirst\n' | "$cli" profile "$target" delete
test ! -e "$data"
test ! -e "$data.json"
"$cli" profile second prefix set default
# Users left by another installation must survive an attempted launch.
users="$XDG_CONFIG_HOME/bedrockoverlinux/prefixes/default/drive_c/users/$(id -un)/AppData/Roaming/Minecraft Bedrock/Users"
test -L "$users"
unlink "$users"
mkdir "$users"
printf 'saved world' > "$users/world"
if "$cli" profile second launch >"$scratch/storage-error" 2>&1; then exit 1; fi
grep -F 'existing game data requires manual import' "$scratch/storage-error"
test "$(cat "$users/world")" = 'saved world'
mv "$users" "$scratch/external-worlds"
ln -s "$scratch/external-worlds" "$users"
if "$cli" profile second launch >"$scratch/storage-error" 2>&1; then exit 1; fi
grep -F 'unmanaged storage link' "$scratch/storage-error"
test "$(readlink "$users")" = "$scratch/external-worlds"
test "$(cat "$users/world")" = 'saved world'
unlink "$users"
"$cli" profile second launch
second_id=$("$cli" profile second info | jq -r .uuid)
test "$(readlink "$users")" = "$XDG_CONFIG_HOME/bedrockoverlinux/profiles/$second_id/Users"
printf 'y\nrelease\n' | "$cli" profile delete release
if "$cli" profile release info; then exit 1; fi
"$cli" profile preview info > /dev/null
"$cli" auth logout Player
"$cli" auth default info | jq -e '.name=="Other"'
"$cli" profile list | grep 'preview .*signed out'
"$cli" auth logout Other
