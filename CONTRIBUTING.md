# Contributing

prospero-win favors reusable compatibility contracts over title-specific
patches. A change should explain which Windows or processor behavior it
implements, cite the relevant public reference, and include a focused
regression test.

## Your first contribution

A PS5, SDK and game installation are **not required** for documentation,
portable runtime code or host tests. Start with
[host setup](docs/DEVELOPMENT.md#host-setup), fork the repository on GitHub,
clone your fork, and create a branch:

```sh
git clone https://github.com/<your-account>/prospero-win.git
cd prospero-win
git switch -c fix/<topic>
python3 tools/check_setup.py
make -j2 all
```

Make one focused change. For new runtime behavior, add a regression test
that fails before the fix; explain the public Windows/processor contract.
Update the allowlist for new files, run the checks below, commit, push your
branch and open a pull request against `main`. The PR template asks for the
problem, resulting behavior, checks and remaining limitations. Expect review
before merging; a host pass does not substitute for console evidence.

Keep each PR independently useful and aim for 3–7 changed files. Divide a
larger feature into dependent PRs rather than mixing unrelated changes.

## Choose where to contribute

- **Game testing:** use the game report issue form. See [telemetry setup](docs/TELEMETRY.md) for the current live log collection process. Saved player reports are not available in the current runtime.
  Review logs before attaching them to an issue. Do not commit raw logs.
- **Profiles, controller presets, recipes:** send changes to
  [prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles).
  Name the tested game and runtime versions and how far you played.
- **Documentation:** fix an unclear step or stale claim, run `make audit`
  and `python3 tests/test_docs_links.py`, then open a focused PR. No console
  test is needed for prose that does not change a hardware claim.
- **Runtime code:** portable contracts belong in `src/`, platform adapters in
  `native/`, and Wine changes in `wine/`. See [development](docs/DEVELOPMENT.md).
- **Console validation:** report exact artifacts, firmware and tested scope;
  never infer complete compatibility from a screenshot.

## Before opening a change

```sh
make all
make sanitize
git diff --check
```

`make all` includes the fail-closed publication audit. New repository files
must be reviewed and added to `PUBLICATION_ALLOWLIST.txt`.

Do not submit proprietary executables, DLLs, shaders, SDK files, telemetry
transcripts, captures, private paths or secrets. Synthetic PE fixtures and
small original test programs are preferred.

Hardware claims require exact artifact identity plus structured
`ps5log/1` evidence. Screenshots and videos are useful supporting material
but do not establish execution, ownership or cleanup by themselves.

Keep pull requests focused. Describe current limitations explicitly and avoid
general compatibility or performance claims that the included evidence does
not establish.
