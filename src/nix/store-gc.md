R""(

# Examples

* Delete unreachable paths in the Nix store:

  ```console
  # nix store gc
  ```

* Delete up to 1 gigabyte of garbage:

  ```console
  # nix store gc --max 1G
  ```

# Description

This command deletes unreachable paths in the Nix store.

With the [`gc-registration-order`](@docroot@/development/experimental-features.md#xp-feature-gc-registration-order)
experimental feature enabled, it prefers candidates with older registration times.
For example, to prefer older paths when freeing disk space in a directly accessed local store:

```console
# nix --extra-experimental-features gc-registration-order store gc --max 1G
```

For daemon-backed stores, enable the feature in the daemon's configuration instead.
See [garbage collection ordering](@docroot@/command-ref/nix-store/gc.md#ordering)
for details and limitations.

)""
