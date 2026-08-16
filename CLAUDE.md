# Working in this repo

## Branching and PRs

- Don't commit directly to `main`. Create a feature branch for each change
  (e.g. `git checkout -b add-thing`) and push that.
- Open a pull request for the branch (`gh pr create`) instead of merging
  locally. Let review/CI happen on the PR before it lands on `main`.
- Keep `main` deployable: only merge PRs that build and have been run.
