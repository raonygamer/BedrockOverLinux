#!/usr/bin/env bash
# Offline CLI and filesystem tests, including hosts with no graphical session.
set -euo pipefail
cli=$(realpath "${1:-build/bedrock}")
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
export XDG_CONFIG_HOME="$scratch/config with spaces"
root="$XDG_CONFIG_HOME/bedrockoverlinux"
account=$(cat /proc/sys/kernel/random/uuid)
mkdir -p "$root/accounts"
printf '{"name":"QA"}\n' > "$root/accounts/$account.json"

reject() {
    if "$@" >"$scratch/error" 2>&1; then printf 'unexpected success: %s\n' "$*" >&2; exit 1; fi
}
"$cli" | cmp - <("$cli" --help)
reject "$cli" profile create ../outside
reject "$cli" profile create list
reject "$cli" profile create empty --version does-not-exist
"$cli" profile create Example --version latest_preview > "$scratch/id"
id=$(cat "$scratch/id")
reject "$cli" profile create Example
"$cli" profile "$id" version info | grep -F latest_preview
"$cli" profile "$id" env set EMPTY ''
"$cli" profile Example env set WITH_SPACE 'two words'
"$cli" profile Example env set EQUALS 'a=b'
reject "$cli" profile Example env set 3invalid value
reject "$cli" profile Example env set XODUS_SOCKET elsewhere
"$cli" profile Example env list | grep -F '"EMPTY": ""'
"$cli" profile Example env list | grep -F '"WITH_SPACE": "two words"'
"$cli" profile Example wrapper set -- env 'WRAPPED=two words'
"$cli" profile Example args set -- --flag 'literal $HOME'
"$cli" profile Example args info | grep -F -- '--flag'
"$cli" profile Example prelaunch set -- /bin/echo hello
"$cli" profile Example prelaunch clear
"$cli" profile Example prelaunch info | grep -Fx '[]'
"$cli" profile Example prefix set 'second-prefix'
"$cli" profile Example prefix info | grep -F 'second-prefix'
reject "$cli" profile Example prefix set '../escape'
# Independent concurrent edits on one record must all survive.
pids=()
for i in {1..12}; do "$cli" profile Example env set "JOB_$i" "$i" & pids+=("$!"); done
for pid in "${pids[@]}"; do wait "$pid"; done
settings=$("$cli" profile "$id" env list)
for i in {1..12}; do
    expected=$(printf '"JOB_%d": "%d"' "$i" "$i")
    grep -Fq "$expected" <<< "$settings"
done

# Incomplete JSON must fail without overwriting the damaged record.
record="$root/profiles/$id.json"
mv "$record" "$scratch/intact.json"
printf '{' > "$record"
reject "$cli" profile "$id" env set AFTER_DAMAGE ignored
test "$(cat "$record")" = '{'
mv "$scratch/intact.json" "$record"
"$cli" profile "$id" env list | grep -F JOB_12

# Competing creates must commit exactly one record.
pids=()
for _ in {1..8}; do "$cli" profile create race >/dev/null 2>&1 & pids+=("$!"); done
ok=0
for pid in "${pids[@]}"; do if wait "$pid"; then ok=$((ok+1)); fi; done
test "$ok" = 1
mkdir -p "$root/profiles/$id"
export XDG_DATA_HOME="$scratch/data with spaces"
"$cli" shortcut list | grep -Fx 'No profile shortcuts.'
desktop=$("$cli" shortcut add Example)
test "$desktop" = "$XDG_DATA_HOME/applications/bedrockoverlinux-$id.desktop"
"$cli" shortcut add "$id" | grep -Fx "$desktop"
"$cli" shortcut info Example | grep -F "profile $id launch"
"$cli" shortcut list | grep -F "Example $id"
if command -v desktop-file-validate >/dev/null; then desktop-file-validate "$desktop"; fi
"$cli" shortcut delete Example
test -e "$root/profiles/$id.json"
reject "$cli" shortcut info Example
printf 'unrelated' > "$desktop"
reject "$cli" shortcut add Example
reject "$cli" shortcut delete "$id"
test "$(cat "$desktop")" = unrelated
unlink "$desktop"
"$cli" shortcut add Example > /dev/null
touch "$scratch/keep"
ln -s "$scratch/keep" "$root/profiles/$id/link"
"$cli" profile delete Example < /dev/null
test -f "$root/profiles/$id.json"
printf 'y\nExample\n' | "$cli" profile delete "$id"
test ! -e "$root/profiles/$id.json" && test -f "$scratch/keep"

"$cli" shortcut list | grep -F "(missing profile) $id"
"$cli" shortcut delete "$id"
test ! -e "$desktop"

# Recover a deletion interrupted between hiding the record and removing its data.
rid=$("$cli" profile create recovery)
mkdir -p "$root/profiles/$rid"
mv "$root/profiles/$rid.json" "$root/profiles/$rid.deleting"
"$cli" profile list >/dev/null
test ! -d "$root/profiles/$rid" && test ! -e "$root/profiles/$rid.deleting"
echo CLI_EDGE_TESTS_PASS
