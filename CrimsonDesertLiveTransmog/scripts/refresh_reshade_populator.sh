#!/usr/bin/env bash
# refresh_reshade_populator.sh
#
# Re-vendor the two ReShade host-side function-table files
# (imgui_function_table_<NNNNN>.hpp / .cpp) verbatim from upstream into
# external/reshade-sdk/source/.  Run after bumping ImGui's FetchContent
# tag to a version whose IMGUI_VERSION_NUM has changed.
#
# Usage:
#   bash scripts/refresh_reshade_populator.sh [REF] [VERSION]
#     REF     git ref in crosire/reshade to fetch from (default: main)
#     VERSION IMGUI_VERSION_NUM the upstream files are named with (default: 19250)
#
# Both arguments default to the current pinned values.  Vendored files
# keep their upstream names exactly -- no renaming, no edits.  The
# adaptation (build-guard macros + extern "C" facade) lives in
# src/imgui_function_table_populator.cpp; when the version number
# changes, update the two #include lines there to point at the new
# basename and rebuild.
#
# License of the vendored files: BSD-3-Clause OR MIT (preserved in the
# upstream SPDX header).  Compatible with this project.

set -euo pipefail

REF="${1:-main}"
VERSION="${2:-19250}"
BASE="https://raw.githubusercontent.com/crosire/reshade/${REF}/source"
DST_DIR="$(cd "$(dirname "$0")/.." && pwd)/external/reshade-sdk/source"

mkdir -p "${DST_DIR}"

echo "Fetching imgui_function_table_${VERSION}.{hpp,cpp} from crosire/reshade@${REF}..."
curl -fsSL "${BASE}/imgui_function_table_${VERSION}.hpp" -o "${DST_DIR}/imgui_function_table_${VERSION}.hpp"
curl -fsSL "${BASE}/imgui_function_table_${VERSION}.cpp" -o "${DST_DIR}/imgui_function_table_${VERSION}.cpp"

echo "Vendored to:"
echo "  ${DST_DIR}/imgui_function_table_${VERSION}.hpp"
echo "  ${DST_DIR}/imgui_function_table_${VERSION}.cpp"
echo
echo "If you bumped to a new VERSION, also:"
echo "  1. Delete the previous-version files in external/reshade-sdk/source/."
echo "  2. Update the two #include lines in src/imgui_function_table_populator.cpp."
echo "  3. Update the IMGUI_VERSION_NUM check in external/reshade-sdk/include/reshade_overlay.hpp."
