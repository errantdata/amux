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

1. Push the tag and let the Release workflow finish.
2. Take the source tarball's checksum from the release's `SHA256SUMS` — the
   line for `amux-<version>.tar.gz`.
3. In the tap, update the formula's `url` (new tag) and `sha256`, then commit.

To check a change before pushing it:

```sh
brew install --build-from-source ./Formula/amux.rb
brew test amux
brew audit --strict --online amux
```

## Note on the repository name

The formula's `url` and `homepage` assume the project repo is named `amux`. If
you keep the current `abduco-mux` name instead, change those two fields to
match — Homebrew does not follow GitHub's rename redirects for `url`
reliably, and a stale URL breaks installs.
