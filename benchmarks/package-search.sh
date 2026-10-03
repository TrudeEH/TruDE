#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
old_ref=18acfa3f1004be820a031d872004037cdf3d5135
new_ref=96ac5fe8d4eba79b9c77d0aeef99303783611411
output_dir=${1:-${TMPDIR:-/tmp}/dotfiles-package-search-benchmark-$$}
runs=${BENCHMARK_RUNS:-5}
case $runs in
    ''|*[!0-9]*|0) printf 'BENCHMARK_RUNS must be a positive integer.\n' >&2; exit 2 ;;
esac
mkdir -p "$output_dir"
output_dir=$(CDPATH='' cd -- "$output_dir" && pwd)
fixture=$(mktemp -d /tmp/package-search-benchmark.XXXXXX)
trap 'rm -rf "$fixture"' 0
trap 'exit 1' HUP INT TERM

. /etc/os-release
export BENCHMARK_UI_HELPER=$repo_root/scripts/lib/ui.sh
git -C "$repo_root" show "$old_ref:scripts/tui/package-manager-tui" \
    | sed '/^command -v whiptail/,$d' \
    | awk -v suite="$VERSION_CODENAME" '
        {
            gsub(/stable-backports/, suite "-backports")
            gsub(/stable-updates/, suite "-updates")
            gsub(/stable-security/, suite "-security")
            gsub(/stable\//, suite "/")
            print
        }
    ' > "$fixture/old.sh"
git -C "$repo_root" show "$new_ref:scripts/tui/package-manager-tui" \
    | sed '/^command -v whiptail/,$d' \
    | sed '/^\. .*lib\/ui\.sh"$/c\
. "$BENCHMARK_UI_HELPER"' > "$fixture/new.sh"
for version in old new; do
    cat >> "$fixture/$version.sh" <<'RUNNER'

# Measure result construction without interaction, installation or APT refresh.
if [ "$2" = apt ]; then
    create_flatpak_search_results() {
        : > "$temporary_dir/empty-flatpak"
        printf '%s\n' "$temporary_dir/empty-flatpak"
    }
fi
results=$(create_unified_install_results "$1")
cat "$results"
RUNNER
done
mkdir "$fixture/bin"
cat > "$fixture/bin/apt-cache" <<'PROFILE'
#!/bin/sh
printf '%s\n' "$1" >> "$BENCHMARK_APT_CALLS"
exec /usr/bin/apt-cache "$@"
PROFILE
chmod 755 "$fixture/bin/apt-cache"

printf 'mode\tquery\ttrial\tversion\tmilliseconds\trows\n' > "$output_dir/timings.tsv"
printf 'mode\tquery\tversion\tsearch_calls\tpolicy_calls\trows\n' > "$output_dir/calls.tsv"
printf 'mode\tquery\tidentical_ids\trows\n' > "$output_dir/correctness.tsv"

measure() {
    version=$1
    mode=$2
    query=$3
    trial=$4
    start=$(date +%s%N)
    sh "$fixture/$version.sh" "$query" "$mode" > "$fixture/timed-results"
    end=$(date +%s%N)
    elapsed=$((end - start))
    milliseconds=$(awk -v elapsed="$elapsed" 'BEGIN { printf "%.3f", elapsed / 1000000 }')
    rows=$(wc -l < "$fixture/timed-results")
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$mode" "$query" "$trial" "$version" "$milliseconds" "$rows" >> "$output_dir/timings.tsv"
    printf '  %s %s: %s ms (%s rows)\n' "$trial" "$version" "$milliseconds" "$rows"
}

benchmark() {
    mode=$1
    query=$2
    printf 'Benchmarking %s: %s\n' "$mode" "$query"
    for version in old new; do
        calls=$fixture/$version-calls
        : > "$calls"
        # Profile separately: the timing runs do not use this logging wrapper.
        BENCHMARK_APT_CALLS=$calls PATH=$fixture/bin:$PATH \
            sh "$fixture/$version.sh" "$query" "$mode" > "$fixture/$version-results"
        cut -f1,2 "$fixture/$version-results" | LC_ALL=C sort > "$fixture/$version-ids"
        search_calls=$(awk '$0 == "search" { n++ } END { print n + 0 }' "$calls")
        policy_calls=$(awk '$0 == "policy" { n++ } END { print n + 0 }' "$calls")
        rows=$(wc -l < "$fixture/$version-results")
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$mode" "$query" "$version" "$search_calls" "$policy_calls" "$rows" >> "$output_dir/calls.tsv"
    done
    cmp "$fixture/old-ids" "$fixture/new-ids"
    printf '%s\t%s\tyes\t%s\n' "$mode" "$query" "$rows" >> "$output_dir/correctness.tsv"
    # Profiling also warms caches once per version. Alternate the timing order.
    trial=1
    while [ "$trial" -le "$runs" ]; do
        if [ $((trial % 2)) -eq 1 ]; then
            measure old "$mode" "$query" "$trial"
            measure new "$mode" "$query" "$trial"
        else
            measure new "$mode" "$query" "$trial"
            measure old "$mode" "$query" "$trial"
        fi
        trial=$((trial + 1))
    done
}

benchmark apt '^jq$'
benchmark apt nmap
benchmark apt '^git(-|$)'
benchmark apt browser
benchmark apt dotfiles-benchmark-no-match-8ec051
benchmark full nmap
printf 'Results: %s\n' "$output_dir"
