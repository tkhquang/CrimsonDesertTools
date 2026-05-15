# external/reshade-sdk

Vendored, unmodified files from two upstream projects:

- **Dear ImGui** (`include/imgui.h`, `include/imconfig.h`) -- header subset
  matching the ReShade SDK release bundle.
- **ReShade SDK** (`include/reshade*.hpp`) and **ReShade host-side imgui
  function-table populator** (`source/imgui_function_table_19250.{hpp,cpp}`).

Layout mirrors upstream:

```
external/reshade-sdk/
  include/   crosire/reshade/include/   (SDK release headers)
  source/    crosire/reshade/source/    (host-side populator pair)
```

Files are byte-for-byte copies of the upstream sources and retain their
original copyright headers. See [LICENSE](LICENSE) for the full notices.

## Refresh

To pull a newer ReShade revision (e.g. when bumping ImGui's
`IMGUI_VERSION_NUM`):

```sh
bash scripts/refresh_reshade_populator.sh <git-ref> <imgui-version-num>
```

This re-fetches the populator pair under `source/`.  The headers in
`include/` come from the ReShade SDK release zip (Patrick distributes
that separately); refresh those by hand when bumping the SDK.

## Why no submodule

We need only ~12 files (~80 KB) out of a ~25 MB ReShade tree.  Vendoring
keeps the repo small, makes diffs against upstream readable, and avoids
the licensing optics of pulling in ReShade's GPL-licensed runtime
sources (which we don't use).
