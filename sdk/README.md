# SDK patches

Patches against [rexglue-sdk](https://github.com/rexglue/rexglue-sdk) commit `c94f5eb` (the SDK is not vendored here; `third_party/` is git-ignored).

- `series-nhl-nuc/`: the ordered series (one patch per change), `series-nhl-nuc` lists the order.
- `rexglue-nhl-nuc-combined.patch`: the whole series as one patch (checked to apply to a clean `c94f5eb`).
- `pending/`: two default-off experiments exported from a working tree, not part of the series (see its README).
- the other `rexglue-*.patch` files: earlier single patches kept for history (macOS FSI/tiling fixes by Redth, diagnostics).

**Licence and attribution.** rexglue-sdk is Copyright (c) 2026 Tom Clay and derived from Xenia (Copyright (c) 2022 Ben Vanik and
Xenia project contributors), under the BSD 3-clause licence (text in [../release/THIRD-PARTY-NOTICES.txt](../release/THIRD-PARTY-NOTICES.txt)).
These patches modify that code and are derivative works: they keep the same licence and notices. New files added by
a patch (for example the fused-resolve shaders) are offered under the same terms.
