# Name

`nix-store --gc` - run garbage collection

# Synopsis

`nix-store` `--gc` [`--print-roots` | `--print-live` | `--print-dead`] [`--max-freed` *bytes*]

# Description

Without additional flags, the operation `--gc` performs a garbage
collection on the Nix store. That is, all paths in the Nix store not
reachable via file system references from a set of “roots”, are deleted.

The following suboperations may be specified:

- `--print-roots`

  This operation prints on standard output the set of roots used by
  the garbage collector.

- `--print-live`

  This operation prints on standard output the set of “live” store
  paths, which are all the store paths reachable from the roots. Live
  paths should never be deleted, since that would break consistency —
  it would become possible that applications are installed that
  reference things that are no longer present in the store.

- `--print-dead`

  This operation prints out on standard output the set of “dead” store
  paths, which is just the opposite of the set of live paths: any path
  in the store that is not live (with respect to the roots) is dead.

By default, all unreachable paths are deleted. The following options
control what gets deleted and in what order:

- `--max-freed` *bytes*

  Keep deleting paths until at least *bytes* bytes have been deleted,
  then stop. The argument *bytes* can be followed by the
  multiplicative suffix `K`, `M`, `G` or `T`, denoting KiB, MiB, GiB
  or TiB units.

The behaviour of the collector is also influenced by the
`keep-outputs` and `keep-derivations` settings in the Nix
configuration file.

By default, the collector prints the total number of freed bytes when it
finishes (or when it is interrupted).

## Ordering {#ordering}

By default, garbage collection considers candidates in filesystem directory order.
The experimental feature
[`gc-registration-order`](@docroot@/development/experimental-features.md#xp-feature-gc-registration-order)
instead prefers candidates with older recorded registration times.
This also applies to `nix store gc`, `nix-collect-garbage`, and automatic garbage collection.
It is particularly useful when collection stops early after freeing a requested amount of space.

Unregistered entries are considered first, in filesystem order.
Registered candidates are ordered by increasing registration time, with store-path names breaking ties.
An unknown registration time (zero) is treated as oldest.
Registration time is neither last-use time nor the time a path became unreachable.
The ordering uses a snapshot of registration metadata, so concurrent registrations may affect it.

This is a candidate-selection preference, not a strict deletion order.
Newer unreachable paths referring to an older candidate may need to be deleted first.
Reachability checks and GC roots still protect live paths.
Explicit-path deletion, live/dead reporting, and dry runs are unchanged.
Preparing the candidate order requires additional startup time and memory proportional to the number of registered paths and directory entries.

Enable the feature in the process performing collection:

```ini
extra-experimental-features = gc-registration-order
```

For daemon-backed stores, set this in the daemon's configuration and restart the daemon.
Client experimental-feature settings are not forwarded to the daemon.

{{#include ./opt-common.md}}

{{#include ../opt-common.md}}

{{#include ../env-common.md}}

# Examples

To delete all unreachable paths, just do:

```console
$ nix-store --gc
deleting `/nix/store/kq82idx6g0nyzsp2s14gfsc38npai7lf-cairo-1.0.4.tar.gz.drv'
...
8825586 bytes freed (8.42 MiB)
```

To delete at least 100 MiBs of unreachable paths:

```console
$ nix-store --gc --max-freed $((100 * 1024 * 1024))
```
