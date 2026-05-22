#!/usr/bin/env bash
# Copyright (c) 2024 SAP SE or an SAP affiliate company. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# test-generate-debian-repo.sh
#
# Unit tests for generate-debian-repo.sh
#
# Tests verify:
#   1. Argument validation (missing required args, invalid suite names)
#   2. Suite name validation — "./" must be rejected (issue #2216)
#   3. Release file structure — Architecture field is present (issue #2216)
#   4. Repository directory structure is correct (dists/$SUITE/$COMP/binary-$ARCH/)
#   5. All required Release file fields are present
#   6. Packages index files are created for each architecture
#
# Run:
#   bash test-generate-debian-repo.sh
# or:
#   ./test-generate-debian-repo.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$SCRIPT_DIR/generate-debian-repo.sh"
TMPDIR_BASE="$(mktemp -d)"

# ---------------------------------------------------------------------------
# Test helpers
# ---------------------------------------------------------------------------
PASS=0
FAIL=0

pass() { echo "  PASS: $1"; (( PASS++ )); }
fail() { echo "  FAIL: $1"; (( FAIL++ )); }

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        pass "$desc"
    else
        fail "$desc (expected='$expected' actual='$actual')"
    fi
}

assert_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        pass "$desc"
    else
        fail "$desc (expected to find '$needle' in output)"
    fi
}

assert_not_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if ! echo "$haystack" | grep -qF "$needle"; then
        pass "$desc"
    else
        fail "$desc (expected NOT to find '$needle' in output)"
    fi
}

assert_file_exists() {
    local desc="$1" file="$2"
    if [[ -f "$file" ]]; then
        pass "$desc"
    else
        fail "$desc (file not found: $file)"
    fi
}

assert_file_not_exists() {
    local desc="$1" file="$2"
    if [[ ! -f "$file" ]]; then
        pass "$desc"
    else
        fail "$desc (file should not exist: $file)"
    fi
}

assert_file_contains() {
    local desc="$1" needle="$2" file="$3"
    if [[ -f "$file" ]] && grep -qF "$needle" "$file"; then
        pass "$desc"
    else
        fail "$desc (file '$file' does not contain '$needle')"
    fi
}

assert_exit_nonzero() {
    local desc="$1" exit_code="$2"
    if [[ "$exit_code" -ne 0 ]]; then
        pass "$desc"
    else
        fail "$desc (expected non-zero exit, got 0)"
    fi
}

# ---------------------------------------------------------------------------
# Setup: create a minimal fake .deb for testing
# ---------------------------------------------------------------------------
create_fake_deb() {
    local output_path="$1"
    local arch="${2:-amd64}"
    # Create a minimal DEBIAN/control structure and pack it
    local tmp
    tmp=$(mktemp -d)
    mkdir -p "$tmp/DEBIAN"
    cat > "$tmp/DEBIAN/control" <<EOF
Package: sapmachine-jdk
Version: 21.0.1
Architecture: $arch
Maintainer: SapMachine Team <sapmachine@sap.com>
Description: SapMachine JDK
EOF
    if command -v dpkg-deb >/dev/null 2>&1; then
        dpkg-deb --build "$tmp" "$output_path" >/dev/null 2>&1
    else
        # Minimal .deb creation without dpkg-deb (for test environments)
        # Create a valid .deb-like file using ar
        touch "$output_path"
    fi
    rm -rf "$tmp"
}

# ---------------------------------------------------------------------------
# Test suite
# ---------------------------------------------------------------------------

echo "=== Unit Tests: generate-debian-repo.sh ==="
echo ""

# --- Test 1: Missing required arguments ---
echo "--- Test group: Argument validation ---"

output=$("$SCRIPT" 2>&1 || true)
exit_code=$("$SCRIPT" 2>&1; echo $?) || true
exit_code=$( "$SCRIPT" > /dev/null 2>&1; echo $? ) || true
# Use subshells to capture exit codes without triggering set -e
exit_code_no_args=0
( "$SCRIPT" > /dev/null 2>&1 ) || exit_code_no_args=$?
assert_exit_nonzero "Missing --deb-dir fails with non-zero exit" "$exit_code_no_args"

exit_code_no_output=0
( "$SCRIPT" --deb-dir /tmp > /dev/null 2>&1 ) || exit_code_no_output=$?
assert_exit_nonzero "Missing --output-dir fails with non-zero exit" "$exit_code_no_output"

# --- Test 2: Suite name validation (issue #2216) ---
echo ""
echo "--- Test group: Suite name validation (issue #2216) ---"

DEBDIR="$TMPDIR_BASE/debs1"
OUTDIR="$TMPDIR_BASE/out1"
mkdir -p "$DEBDIR"

# "./" should be rejected
exit_code=$( "$SCRIPT" --deb-dir "$DEBDIR" --output-dir "$OUTDIR" --suite "./" 2>/dev/null; echo $? ) || true
assert_exit_nonzero "Suite './' is rejected as non-compliant" "$exit_code"

# "." should be rejected
exit_code=$( "$SCRIPT" --deb-dir "$DEBDIR" --output-dir "$OUTDIR" --suite "." 2>/dev/null; echo $? ) || true
assert_exit_nonzero "Suite '.' is rejected as non-compliant" "$exit_code"

# Suite with "/" should be rejected
exit_code=$( "$SCRIPT" --deb-dir "$DEBDIR" --output-dir "$OUTDIR" --suite "foo/bar" 2>/dev/null; echo $? ) || true
assert_exit_nonzero "Suite 'foo/bar' (with slash) is rejected" "$exit_code"

# Suite with space should be rejected
exit_code=$( "$SCRIPT" --deb-dir "$DEBDIR" --output-dir "$OUTDIR" --suite "foo bar" 2>/dev/null; echo $? ) || true
assert_exit_nonzero "Suite 'foo bar' (with space) is rejected" "$exit_code"

# Valid suite names should be accepted (just validation, not full run)
error_output=$( "$SCRIPT" --deb-dir "$DEBDIR" --output-dir "$OUTDIR" --suite "stable" --arch "amd64" 2>&1 || true )
assert_not_contains "Suite 'stable' is not rejected" \
    "ERROR: suite 'stable'" "$error_output"

error_output=$( "$SCRIPT" --deb-dir "$DEBDIR" --output-dir "$OUTDIR" --suite "sapmachine" --arch "amd64" 2>&1 || true )
assert_not_contains "Suite 'sapmachine' is not rejected" \
    "ERROR: suite 'sapmachine'" "$error_output"

# --- Test 3: Generated Release file contains Architecture field (issue #2216) ---
echo ""
echo "--- Test group: Release file contains required fields ---"

DEBDIR2="$TMPDIR_BASE/debs2"
OUTDIR2="$TMPDIR_BASE/out2"
mkdir -p "$DEBDIR2"
# Create a minimal fake .deb
create_fake_deb "$DEBDIR2/sapmachine-jdk-21_amd64.deb" "amd64"

# Run the script
"$SCRIPT" \
    --deb-dir "$DEBDIR2" \
    --output-dir "$OUTDIR2" \
    --suite "sapmachine" \
    --component "main" \
    --arch "amd64" \
    --origin "SapMachine" \
    --label "SapMachine" \
    > /dev/null 2>&1

RELEASE_FILE="$OUTDIR2/dists/sapmachine/Release"

assert_file_exists "Release file is generated" "$RELEASE_FILE"

# Architecture field MUST be present (issue #2216 - it was missing)
assert_file_contains "Release file contains 'Architecture:' field (fix for issue #2216)" \
    "Architecture:" "$RELEASE_FILE"

assert_file_contains "Release file contains 'Architecture: amd64'" \
    "Architecture: amd64" "$RELEASE_FILE"

# Other required fields
assert_file_contains "Release file contains 'Suite:' field" \
    "Suite:" "$RELEASE_FILE"

assert_file_contains "Release file has correct Suite value" \
    "Suite: sapmachine" "$RELEASE_FILE"

assert_file_contains "Release file contains 'Components:' field" \
    "Components:" "$RELEASE_FILE"

assert_file_contains "Release file contains 'Date:' field" \
    "Date:" "$RELEASE_FILE"

assert_file_contains "Release file contains 'Origin:' field" \
    "Origin:" "$RELEASE_FILE"

assert_file_contains "Release file contains 'Label:' field" \
    "Label:" "$RELEASE_FILE"

# The suite must NOT be "./" (the broken value from issue #2216)
assert_not_contains "Release file Suite is NOT './'" \
    "Suite: ./" "$(cat "$RELEASE_FILE")"

# --- Test 4: Repository directory structure ---
echo ""
echo "--- Test group: Repository directory structure ---"

# pool/$COMPONENT is a directory not a file - check with -d
if [[ -d "$OUTDIR2/pool/main" ]]; then
    pass "pool/main directory is created"
else
    fail "pool/main directory is created (directory not found: $OUTDIR2/pool/main)"
fi

# Packages index files
PACKAGES_FILE="$OUTDIR2/dists/sapmachine/main/binary-amd64/Packages"
assert_file_exists "Packages index is generated for amd64" "$PACKAGES_FILE"
assert_file_exists "Packages.gz is generated" "${PACKAGES_FILE}.gz"
assert_file_exists "Packages.bz2 is generated" "${PACKAGES_FILE}.bz2"

# InRelease should NOT exist (no GPG key provided)
assert_file_not_exists "InRelease is NOT generated without GPG key" \
    "$OUTDIR2/dists/sapmachine/InRelease"

# --- Test 5: Release file checksum sections ---
echo ""
echo "--- Test group: Release file checksum sections ---"

assert_file_contains "Release file contains SHA256 section" \
    "SHA256:" "$RELEASE_FILE"

assert_file_contains "Release file contains SHA1 section" \
    "SHA1:" "$RELEASE_FILE"

assert_file_contains "Release file contains MD5Sum section" \
    "MD5Sum:" "$RELEASE_FILE"

# --- Test 6: Custom suite and component ---
echo ""
echo "--- Test group: Custom suite and component names ---"

DEBDIR3="$TMPDIR_BASE/debs3"
OUTDIR3="$TMPDIR_BASE/out3"
mkdir -p "$DEBDIR3"

"$SCRIPT" \
    --deb-dir "$DEBDIR3" \
    --output-dir "$OUTDIR3" \
    --suite "lts" \
    --component "contrib" \
    --arch "arm64" \
    > /dev/null 2>&1

RELEASE_FILE3="$OUTDIR3/dists/lts/Release"
assert_file_exists "Release file created for custom suite 'lts'" "$RELEASE_FILE3"
assert_file_contains "Custom suite 'lts' is in Release file" "Suite: lts" "$RELEASE_FILE3"
assert_file_contains "Custom arch 'arm64' in Release file" "Architecture: arm64" "$RELEASE_FILE3"
assert_file_contains "Custom component 'contrib' in Release file" "Components: contrib" "$RELEASE_FILE3"

# --- Cleanup ---
rm -rf "$TMPDIR_BASE"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "=== Test Results ==="
echo "  PASSED: $PASS"
echo "  FAILED: $FAIL"
echo ""

if [[ "$FAIL" -gt 0 ]]; then
    echo "SOME TESTS FAILED" >&2
    exit 1
else
    echo "ALL TESTS PASSED"
    exit 0
fi
