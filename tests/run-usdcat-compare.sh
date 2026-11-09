#!/bin/bash

# Script to run batch comparisons of lusdcat vs usdcat output with detailed diffs
# Tests both USDA and USDC formats

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPARE_SCRIPT="$SCRIPT_DIR/compare-usda.js"
LUSDCAT_PATH="${LUSDCAT_PATH:-./build/lusdcat}"
# Prefer the repo-local OpenUSD helper install, then a sibling OpenUSD checkout,
# External paths are intentionally never guessed from a developer's home
# directory. Prepare the repo-local oracle with scripts/build-openusd-usdcat.sh
# or set USDCAT_PATH explicitly.
if [ -z "$USDCAT_PATH" ]; then
  if [ -x "$SCRIPT_DIR/../ref/dist/bin/usdcat" ]; then
    USDCAT_PATH="$SCRIPT_DIR/../ref/dist/bin/usdcat"
  elif [ -x "$SCRIPT_DIR/../../OpenUSD/dist/bin/usdcat" ]; then
    USDCAT_PATH="$SCRIPT_DIR/../../OpenUSD/dist/bin/usdcat"
  else
    USDCAT_PATH=""
  fi
fi
USDCHECKER_PATH="${USDCHECKER_PATH:-$(dirname "$USDCAT_PATH")/usdchecker}"
TIMEOUT_MS="${TIMEOUT_MS:-60000}"
SHOW_DETAILED_DIFF="${SHOW_DETAILED_DIFF:-true}"
SHOW_FAILURE_SUMMARY="${SHOW_FAILURE_SUMMARY:-true}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print headers
print_header() {
  echo ""
  echo -e "${BLUE}════════════════════════════════════════════════════════${NC}"
  echo -e "${BLUE}$1${NC}"
  echo -e "${BLUE}════════════════════════════════════════════════════════${NC}"
  echo ""
}

# Function to print section
print_section() {
  echo ""
  echo -e "${YELLOW}→ $1${NC}"
  echo ""
}

# Function to check if file exists
check_executable() {
  local path="$1"
  local name="$2"

  # Expand ~ if present
  path="${path/#\~/$HOME}"

  if [ ! -x "$path" ]; then
    echo -e "${RED}Error: $name not found or not executable at: $path${NC}"
    return 1
  fi
  return 0
}

# Function to run comparison on a folder
run_folder_comparison() {
  local folder="$1"
  local folder_name="$2"
  local file_pattern="$3"

  if [ ! -d "$folder" ]; then
    echo -e "${RED}Error: Folder not found: $folder${NC}"
    return 1
  fi

  print_section "Testing $folder_name ($file_pattern files)"

  local detailed_flag=""
  if [ "$SHOW_DETAILED_DIFF" = "true" ]; then
    detailed_flag="--detailed-diff"
  fi

  local lusdcat_arg_flags=()
  local a
  for a in $LUSDCAT_ARGS; do lusdcat_arg_flags+=(--lusdcat-arg "$a"); done

  node "$COMPARE_SCRIPT" \
    --lusdcat "$LUSDCAT_PATH" \
    "${lusdcat_arg_flags[@]}" \
    --usdcat "$USDCAT_PATH" \
    --timeout "$TIMEOUT_MS" \
    --continue-on-error \
    $detailed_flag \
    "$folder/$file_pattern"
}

# Function to print failure and warning summary from results file
# Note: Uses plain text (no ANSI colors) so output is clean when redirected
print_failure_summary() {
  local results_file="$1"

  if [ ! -f "$results_file" ]; then
    return
  fi

  # Strip ANSI codes for processing
  local clean_results
  clean_results=$(sed 's/\x1b\[[0-9;]*m//g' "$results_file")

  # Extract failed files (lines with "✗" followed by "difference(s)")
  local failed_files
  failed_files=$(echo "$clean_results" | grep -B1 "✗.*difference(s)" | grep "Processing:" | sed 's/.*Processing: //' | sort -u)

  # Extract warning/error files (lines with "⚠" or "Error:")
  local warning_files
  warning_files=$(echo "$clean_results" | grep -B1 -E "(⚠|Error:)" | grep "Processing:" | sed 's/.*Processing: //' | sort -u)

  local has_output=false

  if [ -n "$failed_files" ] || [ -n "$warning_files" ]; then
    echo ""
    echo "========================================================"
    echo "Failure and Warning Summary"
    echo "========================================================"
    echo ""
  fi

  if [ -n "$failed_files" ]; then
    has_output=true
    local fail_count
    fail_count=$(echo "$failed_files" | wc -l)
    echo "[X] Failed Files ($fail_count):"
    echo "$failed_files" | while read -r file; do
      echo "  - $file"
    done
    echo ""
  fi

  if [ -n "$warning_files" ]; then
    has_output=true
    local warn_count
    warn_count=$(echo "$warning_files" | wc -l)
    echo "[!] Warning/Error Files ($warn_count):"
    echo "$warning_files" | while read -r file; do
      echo "  - $file"
    done
    echo ""
  fi

  if [ "$has_output" = true ]; then
    echo "--------------------------------------------------------"
    echo ""
  fi
}


# ---------------------------------------------------------------------------
# usdchecker validation pass (differential).
#
# For every tests/usda fixture, write a .usdc with lusdcat and require that
# OpenUSD's usdchecker reports EXACTLY the same set of validator rules on our
# crate as on the source .usda. The fixtures intentionally trip content lints
# (MissingUpAxisMetadata, MissingMetersPerUnitMetadata, unresolvable refs, ...),
# so absolute cleanliness is not the bar -- parity is: a crate-encoding bug
# shows up as usdchecker failing to open our file, or as extra/missing rules.
# Rule IDs only (the parenthesized validator tokens) so differing file paths
# in the messages don't produce false diffs.
# ---------------------------------------------------------------------------
run_usdchecker_pass() {
  local fail_marker="$1"

  if [ ! -x "$USDCHECKER_PATH" ]; then
    echo "usdchecker not found at: $USDCHECKER_PATH (set USDCHECKER_PATH) -- skipping validation pass"
    return 0
  fi

  print_section "usdchecker validation pass (usda vs our usdc)"

  local tmpdir
  tmpdir=$(mktemp -d)
  local checked=0 skipped=0 failed=0

  # Relative references/payloads/sublayers in the fixtures must resolve from
  # the written .usdc exactly as they do from the source .usda, or resolver
  # lints (UnresolvableDependency) fire differently on the two and produce
  # false parity diffs. Symlink the fixture directory's contents next to the
  # crates we write.
  ln -s "$SCRIPT_DIR"/usda/* "$tmpdir"/ 2>/dev/null || true

  # Rule multiset: "count (validator:Rule.Name)" lines. "Failed to open stage."
  # has no rule token; represent it explicitly so an unreadable crate FAILS
  # parity instead of comparing as an empty set.
  checker_rules() {
    local out
    # usdchecker exits nonzero for ordinary validation findings. Those are the
    # data this differential pass compares, not a shell-level failure.
    if ! out=$("$USDCHECKER_PATH" --noAssetChecks -s "$1" 2>&1); then
      :
    fi
    # A source-open failure has no validator token. Keep errexit from stopping
    # the group before the explicit FAILED_TO_OPEN sentinel is emitted.
    { echo "$out" | grep -oE '\((usd|sdf|ar)[A-Za-z]*[Vv]alidators?:[A-Za-z0-9_.]+\)' || true;
      echo "$out" | grep -cF "Failed to open stage." | grep -v '^0$' | sed 's/^/FAILED_TO_OPEN x/'; } \
      | sort | uniq -c | sed 's/^ *//'
  }

  local f base
  for f in "$SCRIPT_DIR"/usda/*.usda; do
    base=$(basename "$f")

    # shellcheck disable=SC2086  # LUSDCAT_ARGS is a word list
    if ! "$LUSDCAT_PATH" $LUSDCAT_ARGS --output-format usdc -o "$tmpdir/$base.usdc" "$f" >/dev/null 2>&1; then
      skipped=$((skipped+1))   # fixture lusdcat cannot write standalone
      echo -e "${YELLOW}- usdchecker skip: $base (lusdcat cannot write standalone)${NC}"
      continue
    fi

    checker_rules "$f" > "$tmpdir/a.rules"
    if grep -q FAILED_TO_OPEN "$tmpdir/a.rules"; then
      skipped=$((skipped+1))   # pxr cannot open the SOURCE usda; nothing to compare
      echo -e "${YELLOW}- usdchecker skip: $base (OpenUSD cannot open source USDA)${NC}"
      continue
    fi
    checker_rules "$tmpdir/$base.usdc" > "$tmpdir/b.rules"

    checked=$((checked+1))
    if ! cmp -s "$tmpdir/a.rules" "$tmpdir/b.rules"; then
      failed=$((failed+1))
      echo -e "${RED}✗ usdchecker parity: $base${NC}"
      diff "$tmpdir/a.rules" "$tmpdir/b.rules" | sed 's/^/    /'
    fi
    rm -f "$tmpdir/$base.usdc"
  done
  rm -rf "$tmpdir"

  echo ""
  echo "usdchecker parity: $checked compared, $failed failed, $skipped skipped"
  if [ "$failed" -gt 0 ]; then
    echo "usdchecker" >> "$fail_marker"
  fi
}

# Main execution
main() {
  # Expand tilde in paths
  LUSDCAT_PATH="${LUSDCAT_PATH/#\~/$HOME}"
  USDCAT_PATH="${USDCAT_PATH/#\~/$HOME}"

  print_header "USD File Format Comparison Suite"
  echo "Configuration:"
  echo "  Comparison Script: $COMPARE_SCRIPT"
  echo "  lusdcat Path: $LUSDCAT_PATH"
  echo "  usdcat Path: $USDCAT_PATH"
  echo "  Timeout: ${TIMEOUT_MS}ms"
  echo "  Detailed Diff: $SHOW_DETAILED_DIFF"
  echo "  Failure Summary: $SHOW_FAILURE_SUMMARY"
  echo ""

  # Check prerequisites
  echo "Checking prerequisites..."

  if [ ! -f "$COMPARE_SCRIPT" ]; then
    echo -e "${RED}Error: compare-usda.js not found at: $COMPARE_SCRIPT${NC}"
    exit 1
  fi

  if [ -z "$USDCAT_PATH" ]; then
    echo -e "${RED}Error: OpenUSD usdcat is not configured. Run scripts/build-openusd-usdcat.sh or set USDCAT_PATH.${NC}"
    exit 2
  fi

  if ! check_executable "$LUSDCAT_PATH" "lusdcat"; then
    exit 1
  fi

  if ! check_executable "$USDCAT_PATH" "usdcat"; then
    exit 1
  fi

  # The next-core lusdcat (tests/core/lusdcat.cc) prints only a
  # "loaded: N prims" summary unless asked for output; --rewrite-layer prints
  # the root layer as USDA, matching `usdcat <file>`. The legacy lusdcat
  # prints USDA by default. Detect which one we have unless LUSDCAT_ARGS is set.
  if [ -z "${LUSDCAT_ARGS+x}" ]; then
    LUSDCAT_ARGS=""
    if "$LUSDCAT_PATH" "$SCRIPT_DIR/usda/anytype-001.usda" 2>/dev/null \
        | head -1 | grep -q '^loaded:'; then
      LUSDCAT_ARGS="--rewrite-layer"
    fi
  fi
  echo "  lusdcat args: ${LUSDCAT_ARGS:-<none>}"

  echo -e "${GREEN}✓ All prerequisites met${NC}"
  echo ""

  # Create results directory
  RESULTS_DIR="$SCRIPT_DIR/comparison-results"
  mkdir -p "$RESULTS_DIR"
  TIMESTAMP=$(date +%Y%m%d_%H%M%S)
  RESULTS_FILE="$RESULTS_DIR/results_${TIMESTAMP}.log"

  echo "Results will be saved to: $RESULTS_FILE"
  echo ""

  # The report block below runs on the left side of a pipe (a subshell), so
  # pass/fail state must escape through a marker file.
  FAIL_MARKER=$(mktemp)
  : > "$FAIL_MARKER"

  # Run comparisons and capture output
  {
    print_header "USD File Format Comparison Results - $TIMESTAMP"

    # Test USDA files
    print_section "USDA (ASCII) Format Tests"
    echo "Testing all .usda files in tests/usda directory..."
    echo ""
    if ! run_folder_comparison "$SCRIPT_DIR/usda" "USDA Files" "*.usda"; then
      echo "USDA" >> "$FAIL_MARKER"
    fi

    # Test USDC files
    print_section "USDC (Binary/Crate) Format Tests"
    echo "Testing all .usdc files in tests/usdc directory..."
    echo ""
    if ! run_folder_comparison "$SCRIPT_DIR/usdc" "USDC Files" "*.usdc"; then
      echo "USDC" >> "$FAIL_MARKER"
    fi

    # usdchecker validation pass (differential; skipped when usdchecker absent)
    run_usdchecker_pass "$FAIL_MARKER" || true

    print_header "Comparison Complete"
  } | tee "$RESULTS_FILE"

  # Print failure summary if enabled
  if [ "$SHOW_FAILURE_SUMMARY" = "true" ]; then
    print_failure_summary "$RESULTS_FILE"
  fi

  if [ -s "$FAIL_MARKER" ]; then
    echo ""
    echo -e "${RED}✗ Comparison failures detected (see log above)${NC}"
    rm -f "$FAIL_MARKER"
    exit 1
  fi
  rm -f "$FAIL_MARKER"

  echo ""
  echo -e "${GREEN}✓ Comparison complete!${NC}"
  echo "Full results saved to: $RESULTS_FILE"
  echo ""
  echo "Usage examples for custom runs:"
  echo "  # Test specific USDA file with detailed diff"
  echo "  SHOW_DETAILED_DIFF=true node $COMPARE_SCRIPT --detailed-diff tests/usda/cube.usda"
  echo ""
  echo "  # Test specific files with lusdcat/usdcat"
  echo "  LUSDCAT_PATH=./build_gcc/lusdcat USDCAT_PATH=ref/dist/bin/usdcat \\"
  echo "    node $COMPARE_SCRIPT --detailed-diff --lusdcat ./build_gcc/lusdcat --usdcat ref/dist/bin/usdcat tests/usda/cube.usda"
  echo ""
}

# Show help
show_help() {
  cat << EOF
Usage: $0 [OPTIONS]

Run comprehensive batch comparisons of lusdcat vs usdcat outputs with detailed diffs.
Tests both USDA (ASCII) and USDC (Binary/Crate) file formats.

OPTIONS:
  -h, --help              Show this help message
  --lusdcat PATH          Path to lusdcat executable (default: ./build_gcc/lusdcat)
  --usdcat PATH           Path to usdcat executable (default: ref/dist/bin/usdcat when present)
  --timeout MS            Timeout per file in milliseconds (default: 60000)
  --no-detailed-diff      Disable detailed diff output (shows summary only)
  --no-failure-summary    Disable failure/warning summary at the end

ENVIRONMENT VARIABLES:
  LUSDCAT_PATH            Override lusdcat path
  LUSDCAT_ARGS            Extra lusdcat args (default: auto, --rewrite-layer
                          for the next-core lusdcat; set empty to disable)
  USDCAT_PATH             Override usdcat path
  TIMEOUT_MS              Override timeout
  SHOW_DETAILED_DIFF      Set to 'false' to disable detailed diffs (default: true)
  SHOW_FAILURE_SUMMARY    Set to 'false' to disable failure summary (default: true)

EXAMPLES:
  # Run with default settings
  $0

  # Run without detailed diffs (faster, summary only)
  $0 --no-detailed-diff

  # Run with custom tool paths
  LUSDCAT_PATH=./build_asan/lusdcat USDCAT_PATH=ref/dist/bin/usdcat $0

  # Run with longer timeout for slow systems
  $0 --timeout 120000

EOF
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    -h|--help)
      show_help
      exit 0
      ;;
    --lusdcat)
      LUSDCAT_PATH="$2"
      shift 2
      ;;
    --usdcat)
      USDCAT_PATH="$2"
      shift 2
      ;;
    --timeout)
      TIMEOUT_MS="$2"
      shift 2
      ;;
    --no-detailed-diff)
      SHOW_DETAILED_DIFF="false"
      shift
      ;;
    --no-failure-summary)
      SHOW_FAILURE_SUMMARY="false"
      shift
      ;;
    *)
      echo "Unknown option: $1"
      show_help
      exit 1
      ;;
  esac
done

# Run main function
main
