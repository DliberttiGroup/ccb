# Maven troubleshooting

[Docs index](index.md) · [Maven](maven.md)

*ccb-maven-troubleshooting*

Code meanings: [diagnostics.md](diagnostics.md).

| Code | Fix |
| ---- | --- |
| `CCB-E3021` | Artifact not in the local cache yet. Run `ccb setup`, then build again. Same for a parent POM evicted from the cache: setup re-prefetches the whole chain. |
| `CCB-E3024` | Parent chain loops, or is deeper than 10 ancestors. Fix the chain in the POMs. |
| `CCB-E3020` | Downloaded file doesn't match the repo's published checksum. Delete the cache entry (or the whole `<cache>/<group-path>/<artifact>/<version>/` dir) and re-run `ccb setup`; if it persists, the repo or mirror is serving bad bytes. |
| `CCB-E3019` | Coordinate doesn't exist in Google Maven (or Central, if enabled). Check spelling and version, and whether the artifact actually lives on Central (then set `maven_central`). Also: a range no published version satisfies, a snapshot with no snapshot metadata, or a missing credential variable for a private repo (the diagnostic names the variable). |
| `CCB-E1005` (coordinate) | Declared coordinate malformed or unpinned. Fix the string in `ccb.toml`. Plugin coordinates must be exact versions. |
| `CCB-E1005` (repo entry) | `[deps] maven_repos` entry malformed: use `url` or `url username env:VAR` (or a literal password). Userinfo in the URL is not allowed. |
| `CCB-E1005` (exclusion) | Exclusion removes something the project declares directly. Remove the exclusion or the direct coordinate. |
| `CCB-E3023` | Resolved tree deviates from `ccb.lock` (version, packaging, hash, missing, or stale entry), or the lockfile is malformed. Run `ccb setup` to regenerate it from the current declarations. |
