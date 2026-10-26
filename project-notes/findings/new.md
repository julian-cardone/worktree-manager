# New findings

Why `wt new` checks and reports what it does.
See [findings.md](../findings.md) for how findings are organized.

## Addressed

### Branch names that differ only in case share ref files

On a case-insensitive filesystem, loose refs are files, so `Fix` and `fix` are one file, and `Fix` and `fix/login` need `refs/heads/Fix` as both a file and a directory.
Git catches this only by accident: a loose ref or reflog happens to be in the way, but a packed ref with reflogs off (`core.logAllRefUpdates=false`, the default in bare repositories) is not, so Git creates the second branch, and later updates to either then fail or shadow the other.
When Git reports `core.ignorecase`, `wt new` refuses a branch that equals, contains, or is contained by an existing one ignoring case, and names the existing branch.

### A failed `wt new` can't tell what it created from what was already there

`wt new` checks that the branch and path are free, but another process can take either before `git worktree add` runs, and Git then fails with that state in place.
`wt new` used to call whatever existed afterwards "left behind" by Git and suggest `wt remove` and `git branch -d` for it, including a directory that wasn't a worktree at all.
It now reports the full current state, removes nothing, and suggests cleanup only when the branch is still at the commit `wt` started it from and the path is absent or a worktree with the task's branch checked out.
