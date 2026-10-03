# Package search benchmark

Measured on 2026-10-03. Building the complete `nmap` search result, including real Flatpak results, fell from a median of **2.501 s to 1.218 s**: **2.05× faster**, or **51.3% less waiting**. Both versions returned the same package sources and IDs.

## Results

Five timed runs per version and query, after one untimed profiling run per version. The table reports medians. APT-only measurements include both Debian and Backports result construction. Full mode also runs the real Flatpak search against the user's Flathub catalog.

| Search work | Query | Result rows | Previous | New | Speedup |
| --- | --- | ---: | ---: | ---: | ---: |
| APT only | `^jq$` | 1 | 0.283 s | 0.145 s | 1.95× |
| APT only | `nmap` | 39 | 1.614 s | 0.330 s | 4.90× |
| APT only | `^git(-\|$)` | 72 | 2.287 s | 0.186 s | 12.30× |
| APT only | `browser` | 110 | 18.582 s | 0.776 s | 23.96× |
| APT only | No match | 0 | 0.511 s | 0.265 s | 1.93× |
| APT + Flatpak | `nmap` | 41 | 2.501 s | 1.218 s | 2.05× |

The complete `nmap` search has a smaller improvement than APT alone because Flatpak search time is still present in both versions.

## Why it is faster

The previous implementation runs APT search twice, once for Debian and once for Backports. It then starts `apt-cache policy` for individual packages and additional version lookups. The new implementation searches once, checks policies in batches, and reuses those results for both lists.

| Query | Old search calls | New search calls | Old policy calls | New policy calls |
| --- | ---: | ---: | ---: | ---: |
| `^jq$` | 2 | 1 | 2 | 1 |
| `nmap` | 2 | 1 | 72 | 1 |
| `^git(-\|$)` | 2 | 1 | 142 | 1 |
| `browser` | 2 | 1 | 1302 | 1 |
| No match | 2 | 1 | 0 | 0 |

Call counting was performed separately from timing, so the logging wrapper is absent from timed runs.

## Comparison and correctness

- Previous implementation: commit `18acfa3f1004be820a031d872004037cdf3d5135`.
- New implementation: commit `96ac5fe8d4eba79b9c77d0aeef99303783611411`.
- The old code only recognizes `stable` repository labels. The temporary old runner changes those label strings to `trixie` and its updates/security/backports equivalents so it works with the current metadata. Its search algorithm is unchanged. No system repositories were edited.
- Both runners source the script's function definitions and call `create_unified_install_results`. The new runner resolves the shared UI library from the repository because it runs from a temporary location.
- In APT-only mode, both runners override Flatpak result retrieval with an empty result. Full mode uses real Flatpak search without an override.
- For each query, untimed outputs were compared by package source and identifier. They matched exactly. Row counts stayed consistent across every timed run. Descriptions were not required to match because the newer code fixes backport version labels.
- Each timed run starts a fresh shell and temporary directory, so search-result files are not reused between trials. Filesystem/APT metadata and Flatpak catalog caches are warm. Old/new execution order alternates between trials.
- Timing includes shell startup, function loading, result construction, and temporary-file cleanup. It excludes interactive menus, installation, and repository refreshes. No packages were installed and no `apt update` was performed.

## Variation and limits

| Mode | Query | Previous range | New range |
| --- | --- | ---: | ---: |
| apt | `^jq$` | 0.277–0.287 s | 0.141–0.148 s |
| apt | `nmap` | 1.584–1.632 s | 0.322–0.379 s |
| apt | `^git(-\|$)` | 2.273–2.293 s | 0.180–0.189 s |
| apt | `browser` | 18.377–18.918 s | 0.767–0.784 s |
| apt | No match | 0.494–0.524 s | 0.252–0.268 s |
| full | `nmap` | 2.484–2.554 s | 1.175–1.227 s |

Machine: Debian 13.7, AMD Ryzen 7 7700 (16 logical CPUs), CPU governor `performance`. APT 3.0.3, dash 0.5.12-12, mawk 1.3.4.20250131-1, Flatpak 1.16.6-1~deb13u3. These are measurements on this machine with its installed package metadata; cold caches, network refreshes, and other machines may give different times.

## Reproduce

From the repository root:

```sh
sh benchmarks/package-search.sh /tmp/package-search-benchmark
```

Set `BENCHMARK_RUNS` to change the number of trials (default: five). The two measured commit IDs are fixed in the script. The benchmark uses shell tools already available on Debian; it does not require Python or an additional benchmarking package.

Raw results: [timings](package-search-timings.tsv), [APT process counts](package-search-calls.tsv), and [output comparisons](package-search-correctness.tsv). The rerunnable harness is [package-search.sh](package-search.sh).
