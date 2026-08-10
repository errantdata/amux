# Publishing the Homebrew formula

Homebrew installs third-party formulae from a **tap**, which is just a GitHub
repository named `homebrew-<something>`. The formula in this directory is the
source of truth; the tap holds a copy.

## One-time setup

1. Create a repo called **`errantdata/homebrew-tap`** (the `homebrew-` prefix is
   required; users type the part after it).
2. Copy the formula into it:

   ```sh
   mkdir -p Formula
   cp path/to/amux/packaging/homebrew/amux.rb Formula/amux.rb
   git add Formula/amux.rb && git commit -m "amux 1.0.0" && git push
   ```

Users then install with:

```sh
brew install errantdata/tap/amux
```

That works on both macOS (Apple Silicon and Intel) and Linuxbrew, because the
formula builds from source with the project's own `configure`/`make`.

## On each release

There is no release pipeline to wait for — tag, then hash the tag archive:

```sh
git tag -a v1.0.0 -m "amux 1.0.0" && git push origin v1.0.0
curl -sL https://github.com/errantdata/amux/archive/refs/tags/v1.0.0.tar.gz \
  | shasum -a 256
```

Put that tag and checksum into the formula's `url` and `sha256`, and commit it
to the tap.

To check a change before pushing it:

```sh
brew install --build-from-source ./Formula/amux.rb
brew test amux
brew audit --strict --online amux
```

## If the repository ever moves

The formula pins three fields to the repository: `homepage`, `url` and `head`.
Homebrew does not follow GitHub's rename redirects for `url` reliably, so a
rename means updating all three — a stale `url` breaks installs rather than
redirecting them.
