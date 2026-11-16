---
synopsis: Prefer older registered paths during garbage collection
---

The experimental feature `gc-registration-order` makes whole-store garbage collection prefer candidates with older registration times, after considering unregistered entries.
This can preserve recently registered paths when collection stops after freeing a requested amount of space.
It also applies to automatic garbage collection.

The feature changes candidate selection, not reachability checks or dependency-safe deletion.
For daemon-backed stores, enable it in the daemon's configuration.
