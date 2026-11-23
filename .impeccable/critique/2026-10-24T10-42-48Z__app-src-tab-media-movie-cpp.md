---
target: Version selector, movie page
total_score: 21
max_score: 36
na_heuristics: 10
p0_count: 0
p1_count: 2
target_identity: "file:/Users/thcolin/orca/workspaces/gmca/emby-versions/app/src/tab/media_movie.cpp"
target_fingerprint: "sha256:049bbc3c7e536c4782ab4ac226fa5009519f7df4a37fdbace80efae85033282f"
target_path: /Users/thcolin/orca/workspaces/gmca/emby-versions/app/src/tab/media_movie.cpp
timestamp: 2026-10-24T10-42-48Z
slug: app-src-tab-media-movie-cpp
---
Method: dual-agent (A: design review · B: detector)

## Design Health Score: 21/36 (n/a: 10)
| # | Heuristic | Score | Key Issue |
|---|---|---|---|
| 1 | Visibility | 2 | closed cell shows "SD · H264" for two versions |
| 2 | Real world | 3 | raw "MPEG2VIDEO", no HDR/DV |
| 3 | Control | 3 | B/A, checkmark |
| 4 | Consistency | 2 | value alone in green, other column than actions |
| 5 | Error prevention | 2 | default = server order |
| 6 | Recognition | 2 | no title on closed cell |
| 7 | Efficiency | 2 | no sort, no preference |
| 8 | Minimalism | 3 | stray separator |
| 9 | Recovery | 2 | Name fallback silent |
| 10 | Help | n/a | self-describing list |

## Design specificity
Stock borealis, consistent; label vocabulary is the real gain. Detector: 0 findings on movie.xml and media_movie.cpp, not evidence (web-only detector). Browser step skipped: native app.

## Priority issues
1. [P1] Closed short labels collide (SD · H264 x2). Fix: add bitrate only to colliding short labels. clarify
2. [P1] Closed cell has no name/affordance (title hidden; was squeezed before too). Fix: icon/chevron, or pill in the action row. layout/shape
3. [P2] Server Name (editions) only used as fallback. clarify
4. [P2] Raw codec strings, no HDR. clarify
5. [P2] Default version = server order, not device max resolution. harden

## Persona red flags
Riley: Plex videoResolution fallback "4k" vs "4K"; 8 versions unsorted. Sam: green value at rest reads active.

## Questions
- Version as a property of Play ("Lire · 1080p ▾")?
- Would a device-aware default make the selector rarely needed?
