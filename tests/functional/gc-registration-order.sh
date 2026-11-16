#!/usr/bin/env bash

source common.sh

TODO_NixOS
needLocalStore "registration ordering must be enabled in the process running GC"
enableFeatures "gc-registration-order"

printf '%4096s' garbage > "$TEST_ROOT/payload"
paths=()
previous=0
for i in 1 2 3 4; do
    path=$(nix store add-path --name "gc-candidate-$i" "$TEST_ROOT/payload")
    registered=$(nix path-info --json --json-format 1 "$path" | jq '.[].registrationTime')
    [[ $registered -gt $previous ]]
    previous=$registered
    paths+=("$path")
    sleep 1
done

nix store gc --max 0
nix-store --gc --max-freed 0
nix-collect-garbage --max-freed 0
nix store gc --dry-run
nix-store --gc --print-dead
nix-collect-garbage --dry-run
for path in "${paths[@]}"; do
    test -e "$path"
done

nix store gc --max 1
test ! -e "${paths[0]}"
for path in "${paths[@]:1}"; do
    test -e "$path"
done

nix-store --gc --max-freed 1
test ! -e "${paths[1]}"
test -e "${paths[2]}"
test -e "${paths[3]}"

nix-collect-garbage --max-freed 1
test ! -e "${paths[2]}"
test -e "${paths[3]}"

nix store gc
test ! -e "${paths[3]}"
