# epm

A package manager for Echo. It resolves, fetches and vendors. It never compiles
anything. It writes a `module.eco` only as a whole new file (`epm init`) or as
the one `#[requires:]` line `epm add` was asked for.

A released `epm` arrives next to `echoc` from the install one-liner. This tree
is the bootstrap: path-depends on `../../echolibs/libjson` and
`../../echolibs/libcurl`, compiled by this tree's `echoc`.

```bash
# from this directory, with this tree's echoc (it must understand -p manifest)
../build/echoc test
../build/echoc build

# epm shells out to echoc. A released epm looks next to itself first
# (install.sh puts both in the same directory). ECHOC wins when set,
# which is how this tree points at ../build/echoc:
export ECHOC=$PWD/../build/echoc
```

```
epm --help
epm --version
epm init greet
epm init greet --yes
epm install
epm add --path ../../echolibs/libjson
epm add --path ../../echolibs/libcurl
epm add other --git https://example.com/other.git --range ^0.2
epm remove other
epm update
epm update other
epm verify
epm tree
```

`epm --help` and `epm <command> --help` are the page; `epm add --help git`
is one option in full. Refusals go to stderr with the same `error:` shape
echoc uses. `-m, --module` names the project; omitted, epm walks up from
the working directory. `init` does not take `-m`: it creates a project,
it does not operate on one. `--version` is epm's own version. The package range
is `--range`.

`init` walks a short wizard and writes a new module into a new directory
named after it. Enter takes a default. `epm init greet --yes` skips the
questions and writes those defaults. A `module.eco` already there is a
refusal.

`install` applies `epm.lock.json` to `vendor/`. `update` is what re-resolves
within the declared ranges and rewrites the lock. A first `install` with no
lock file does an `update`.

A library that is already on disk is a path, not a package. `--path` writes
`#[depends:]` and stops there — same spelling epm uses to bootstrap itself
on `../../echolibs/libjson` and `../../echolibs/libcurl`. `--git` is for
something epm has to fetch into `vendor/`.

After `epm install`, `vendor/` is on disk and `echoc build` needs no flags.
`epm.lock.json` is committed; `vendor/` is not.

A remote is a `Source`. The origin on a `#[requires:]` is a tagged locator
(`source: git "..."`); a later host is another tag, not a new field. GitHub
remotes list tags and fetch a `module.eco` over HTTP; every other host (and
GitHub when HTTP fails) uses git. The clone that fills `vendor/` is always
git. There is no registry in v1.
