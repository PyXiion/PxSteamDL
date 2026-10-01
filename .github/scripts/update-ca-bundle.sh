#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Looks for a Mozilla CA bundle (as extracted by curl) that is newer than the one pinned in CMakeLists.txt and, if
# there is one, rewrites the pinned date and SHA-256 there. Writes "date=<new date>" to $GITHUB_OUTPUT when it did.
# CA_BASE_URL and CMAKE_FILE exist for testing.
set -euo pipefail

base=${CA_BASE_URL:-https://curl.se/ca}
cmake_file=${CMAKE_FILE:-CMakeLists.txt}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

current=$(sed -n 's/^set(PXSTEAMDL_CA_BUNDLE_DATE \([0-9-]*\))$/\1/p' "$cmake_file")
if [ -z "$current" ]; then
  echo "::error::No PXSTEAMDL_CA_BUNDLE_DATE in $cmake_file" >&2
  exit 1
fi

# The newest bundle names the date it was extracted on in its header: "## Certificate data from Mozilla as of: ...".
curl -fsSL "$base/cacert.pem" -o "$work/latest.pem"
as_of=$(sed -n 's/^## Certificate data from Mozilla as of: //p' "$work/latest.pem" | head -n 1)
if [ -z "$as_of" ]; then
  echo "::error::The bundle has no 'Certificate data from Mozilla as of' line" >&2
  exit 1
fi
new=$(date -u -d "$as_of" +%F)
if [[ ! "$new" > "$current" ]]; then
  echo "The pinned bundle ($current) is up to date; the newest is $new"
  exit 0
fi

# The pin names a file by date, so fetch that very file and check that it is what the newest bundle is.
if ! curl -fsSL "$base/cacert-$new.pem" -o "$work/dated.pem"; then
  echo "::error::$base/cacert-$new.pem does not exist; the date in the bundle's header may not be its file name" >&2
  exit 1
fi
hash=$(sha256sum "$work/dated.pem" | cut -d' ' -f1)
if [ "$hash" != "$(sha256sum "$work/latest.pem" | cut -d' ' -f1)" ]; then
  echo "::error::cacert-$new.pem differs from cacert.pem" >&2
  exit 1
fi
if [ "$(grep -c 'BEGIN CERTIFICATE' "$work/dated.pem")" -lt 50 ]; then
  echo "::error::cacert-$new.pem has suspiciously few certificates" >&2
  exit 1
fi

# The hash line to change is the one of the cacert package; other packages have theirs.
sed -i "s/^set(PXSTEAMDL_CA_BUNDLE_DATE .*/set(PXSTEAMDL_CA_BUNDLE_DATE $new)/" "$cmake_file"
sed -i "/NAME cacert/,/DOWNLOAD_NO_EXTRACT/ s/SHA256=[0-9a-f]\{64\}/SHA256=$hash/" "$cmake_file"
if ! grep -q "SHA256=$hash" "$cmake_file" || ! grep -q "PXSTEAMDL_CA_BUNDLE_DATE $new)" "$cmake_file"; then
  echo "::error::Could not update $cmake_file" >&2
  exit 1
fi
echo "Pinned bundle: $current -> $new ($hash)"
echo "date=$new" >> "${GITHUB_OUTPUT:-/dev/null}"
