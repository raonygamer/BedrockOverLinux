#!/usr/bin/env bash
# Exercise machine-wide shell integration in a private /etc. Requires user namespaces and Fish.
set -euo pipefail
cli=$(realpath "${1:-build/bedrock}")
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
mkdir -p "$scratch/home" "$scratch/config with spaces/bedrockoverlinux/accounts"
printf '{"name":"QA"}' > "$scratch/config with spaces/bedrockoverlinux/accounts/$(cat /proc/sys/kernel/random/uuid).json"
unshare --user --map-root-user --mount --propagation private bash -se -- "$cli" "$scratch" <<'CHECK'
set -euo pipefail
cli=$1
work=$2
export HOME="$work/home" XDG_CONFIG_HOME="$work/config with spaces"
unset COM_MOJANG
mount -t tmpfs tmpfs /etc
mkdir /etc/profile.d
printf '. /etc/profile.d/bedrockoverlinux.sh\n' >/etc/profile
touch /etc/bash.bashrc
a=$("$cli" profile create First)
b=$("$cli" profile create Second)
path_for() { printf '%s/bedrockoverlinux/profiles/%s/Users/Shared/games/com.mojang' "$XDG_CONFIG_HOME" "$1"; }
"$cli" env COM_MOJANG set "$a"
check_shells() {
    export EXPECTED="$1"
    for shell in bash fish; do
        for mode in -ic -lic; do
            env COM_MOJANG=stale "$shell" "$mode" '/usr/bin/test "$COM_MOJANG" = "$EXPECTED"' </dev/null
        done
    done
}
check_shells "$(path_for "$a")"
printf 'KEEP=value\n' >>/etc/environment
if printf 'n\n' | "$cli" env COM_MOJANG set "$b"; then exit 1; fi
check_shells "$(path_for "$a")"
printf 'y\n' | "$cli" env COM_MOJANG set "$b"
check_shells "$(path_for "$b")"
grep -Fxq KEEP=value /etc/environment
test "$(grep -c bedrockoverlinux.sh /etc/bash.bashrc)" = 1
echo SHELL_ENVIRONMENT_PASS
CHECK
