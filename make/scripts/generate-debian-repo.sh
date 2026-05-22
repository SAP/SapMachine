#!/usr/bin/env bash
# Copyright (c) 2024 SAP SE or an SAP affiliate company. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# generate-debian-repo.sh
#
# Generates a Debian APT repository that conforms to the Debian Repository
# Format specification: https://wiki.debian.org/DebianRepository/Format
#
# Issue: https://github.com/SAP/SapMachine/issues/2216
#
# The sapmachine apt repository at dist.sapmachine.io was non-compliant:
#   1. The suite was "./", which is not a real suite name and causes most
#      tools (other than apt itself) to generate broken URLs.
#   2. The Release file was missing the required "Architecture:" field.
#
# This script generates a standards-compliant repository using:
#   - Proper suite/codename naming (e.g. "stable", "testing")
#   - Correct component naming (e.g. "main")
#   - All required Release file fields including "Architecture:"
#   - Packages index generated with dpkg-scanpackages
#   - InRelease / Release.gpg files for GPG signature (if key is provided)
#
# Usage:
#   generate-debian-repo.sh [OPTIONS]
#
# Options:
#   -d, --deb-dir DIR       Directory containing .deb packages (required)
#   -o, --output-dir DIR    Output directory for the repository (required)
#   -s, --suite SUITE       Suite name (default: "stable")
#   -c, --component COMP    Component name (default: "main")
#   -a, --arch ARCH         Architecture (default: auto-detected from packages)
#   -k, --gpg-key KEY_ID    GPG key ID for signing (optional)
#   -O, --origin ORIGIN     Origin field (default: "SapMachine")
#   -L, --label LABEL       Label field (default: "SapMachine")
#   -D, --description DESC  Description field
#   -h, --help              Show this help message

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
DEB_DIR=""
OUTPUT_DIR=""
SUITE="stable"
COMPONENT="main"
ARCH=""
GPG_KEY_ID=""
ORIGIN="SapMachine"
LABEL="SapMachine"
DESCRIPTION="SAP SapMachine JDK Debian Repository"

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
usage() {
    grep '^#' "$0" | sed 's/^# \{0,1\}//' | grep -A1000 '^Usage:' | head -30
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--deb-dir)     DEB_DIR="$2";       shift 2 ;;
        -o|--output-dir)  OUTPUT_DIR="$2";    shift 2 ;;
        -s|--suite)       SUITE="$2";         shift 2 ;;
        -c|--component)   COMPONENT="$2";     shift 2 ;;
        -a|--arch)        ARCH="$2";          shift 2 ;;
        -k|--gpg-key)     GPG_KEY_ID="$2";    shift 2 ;;
        -O|--origin)      ORIGIN="$2";        shift 2 ;;
        -L|--label)       LABEL="$2";         shift 2 ;;
        -D|--description) DESCRIPTION="$2";   shift 2 ;;
        -h|--help)        usage ;;
        *) echo "Unknown option: $1" >&2; usage ;;
    esac
done

# ---------------------------------------------------------------------------
# Validate required arguments
# ---------------------------------------------------------------------------
if [[ -z "$DEB_DIR" ]]; then
    echo "ERROR: --deb-dir is required." >&2
    exit 1
fi
if [[ -z "$OUTPUT_DIR" ]]; then
    echo "ERROR: --output-dir is required." >&2
    exit 1
fi
if [[ ! -d "$DEB_DIR" ]]; then
    echo "ERROR: deb-dir '$DEB_DIR' does not exist." >&2
    exit 1
fi

# Validate suite name: must not be "./" (which is non-compliant per the spec)
if [[ "$SUITE" == "./" ]] || [[ "$SUITE" == "." ]]; then
    echo "ERROR: suite '$SUITE' is non-compliant. See https://wiki.debian.org/DebianRepository/Format" >&2
    echo "       Use a proper suite name like 'stable', 'testing', 'sapmachine', etc." >&2
    exit 1
fi

# Validate suite name contains no special characters that would break paths
if [[ "$SUITE" =~ [[:space:]/] ]]; then
    echo "ERROR: suite '$SUITE' must not contain spaces or slashes." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Auto-detect architectures from .deb packages if not specified
# ---------------------------------------------------------------------------
detect_architectures() {
    local deb_dir="$1"
    local arches=()
    for deb in "$deb_dir"/*.deb; do
        [[ -f "$deb" ]] || continue
        # dpkg-deb --info returns Architecture: field
        local arch
        arch=$(dpkg-deb --info "$deb" 2>/dev/null | grep '^[[:space:]]*Architecture:' | awk '{print $2}')
        if [[ -n "$arch" ]]; then
            arches+=("$arch")
        fi
    done
    # Remove duplicates
    printf '%s\n' "${arches[@]}" | sort -u | tr '\n' ' ' | sed 's/ $//'
}

if [[ -z "$ARCH" ]]; then
    if command -v dpkg-deb >/dev/null 2>&1; then
        ARCH=$(detect_architectures "$DEB_DIR")
        if [[ -z "$ARCH" ]]; then
            ARCH="all"
            echo "WARNING: No .deb packages found in '$DEB_DIR'; defaulting Architecture to 'all'" >&2
        fi
    else
        ARCH="amd64"
        echo "WARNING: dpkg-deb not found; defaulting Architecture to 'amd64'" >&2
    fi
fi

# ---------------------------------------------------------------------------
# Build the repository structure:
#   $OUTPUT_DIR/dists/$SUITE/$COMPONENT/binary-$ARCH/
#   $OUTPUT_DIR/pool/$COMPONENT/
# ---------------------------------------------------------------------------
echo "Generating Debian APT repository..."
echo "  DEB directory : $DEB_DIR"
echo "  Output dir    : $OUTPUT_DIR"
echo "  Suite         : $SUITE"
echo "  Component     : $COMPONENT"
echo "  Architecture  : $ARCH"

# Create directory structure
DISTS_DIR="$OUTPUT_DIR/dists/$SUITE"
POOL_DIR="$OUTPUT_DIR/pool/$COMPONENT"
mkdir -p "$POOL_DIR"

# Copy .deb packages into the pool
shopt -s nullglob
deb_count=0
for deb in "$DEB_DIR"/*.deb; do
    cp -f "$deb" "$POOL_DIR/"
    (( deb_count++ ))
done
echo "  Copied $deb_count .deb package(s) to pool"

# Generate per-architecture Packages index
for arch in $ARCH; do
    BINARY_DIR="$DISTS_DIR/$COMPONENT/binary-$arch"
    mkdir -p "$BINARY_DIR"

    if command -v dpkg-scanpackages >/dev/null 2>&1; then
        # dpkg-scanpackages produces a Packages file relative to OUTPUT_DIR
        pushd "$OUTPUT_DIR" > /dev/null
        dpkg-scanpackages --arch "$arch" "pool/$COMPONENT" /dev/null 2>/dev/null \
            > "$BINARY_DIR/Packages"
        gzip -9 -c "$BINARY_DIR/Packages" > "$BINARY_DIR/Packages.gz"
        bzip2 -9 -c "$BINARY_DIR/Packages" > "$BINARY_DIR/Packages.bz2"
        popd > /dev/null
        echo "  Generated Packages index for arch: $arch"
    else
        # Fallback when dpkg-scanpackages is not available (e.g. macOS CI without dpkg)
        # Create empty but valid index files so the Release file can still be generated
        echo "" > "$BINARY_DIR/Packages"
        gzip -9 -c "$BINARY_DIR/Packages" > "$BINARY_DIR/Packages.gz"
        bzip2 -9 -c "$BINARY_DIR/Packages" > "$BINARY_DIR/Packages.bz2"
        echo "  WARNING: dpkg-scanpackages not found; created empty Packages index for arch: $arch" >&2
    fi
done

# ---------------------------------------------------------------------------
# Generate the Release file
#
# The Release file MUST include (per Debian spec):
#   - Suite or Codename (we use Suite)
#   - Components
#   - Architecture  ← this was MISSING in the old repo (issue #2216)
#   - Date
#   - MD5Sum / SHA1 / SHA256 checksums of all index files
# ---------------------------------------------------------------------------
RELEASE_FILE="$DISTS_DIR/Release"
DATE_RFC2822=$(date -Ru 2>/dev/null || date -u "+%a, %d %b %Y %H:%M:%S +0000")

cat > "$RELEASE_FILE" <<EOF
Origin: $ORIGIN
Label: $LABEL
Suite: $SUITE
Codename: $SUITE
Components: $COMPONENT
Architecture: $ARCH
Description: $DESCRIPTION
Date: $DATE_RFC2822
EOF

# Append checksum sections (SHA256, SHA1, MD5Sum)
# Each section lists: <hash>  <size>  <path-relative-to-dists/suite>
generate_checksums() {
    local hash_cmd="$1"
    local section_header="$2"
    local dists_dir="$3"
    echo "$section_header"
    find "$dists_dir" -mindepth 2 -type f \( -name 'Packages' -o -name 'Packages.gz' -o -name 'Packages.bz2' \) | \
    sort | while read -r file; do
        local rel_path="${file#$dists_dir/}"
        local size
        size=$(wc -c < "$file")
        local hash
        hash=$($hash_cmd "$file" | awk '{print $1}')
        printf " %s %d %s\n" "$hash" "$size" "$rel_path"
    done
}

generate_checksums "md5sum"  "MD5Sum:"  "$DISTS_DIR"    >> "$RELEASE_FILE"
generate_checksums "sha1sum" "SHA1:"    "$DISTS_DIR"    >> "$RELEASE_FILE"
generate_checksums "sha256sum" "SHA256:" "$DISTS_DIR"   >> "$RELEASE_FILE"

echo "  Generated Release file: $RELEASE_FILE"

# ---------------------------------------------------------------------------
# Optionally sign the Release file with GPG
# ---------------------------------------------------------------------------
if [[ -n "$GPG_KEY_ID" ]]; then
    if command -v gpg >/dev/null 2>&1; then
        # Inline signature (InRelease) — preferred by modern apt
        gpg --default-key "$GPG_KEY_ID" \
            --clearsign \
            --armor \
            --output "$DISTS_DIR/InRelease" \
            "$RELEASE_FILE"
        echo "  Generated signed InRelease file"

        # Detached signature (Release.gpg) — for compatibility
        gpg --default-key "$GPG_KEY_ID" \
            --detach-sign \
            --armor \
            --output "$DISTS_DIR/Release.gpg" \
            "$RELEASE_FILE"
        echo "  Generated detached Release.gpg signature"
    else
        echo "WARNING: gpg not found; skipping signing." >&2
    fi
fi

echo "Done. Repository at: $OUTPUT_DIR"
echo ""
echo "Example sources.list entry:"
echo "  deb [arch=$ARCH] <base-url> $SUITE $COMPONENT"
