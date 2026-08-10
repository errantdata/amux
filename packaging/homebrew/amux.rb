# Homebrew formula for amux.
#
# This file lives here for review; Homebrew reads it from a *tap* repository.
# See packaging/homebrew/README.md for how to publish it.
#
# On each release, update `url` to the new tag and `sha256` to the value in
# that release's SHA256SUMS for the source tarball (amux-<version>.tar.gz).
#
# `url` deliberately points at the release *asset* rather than GitHub's
# auto-generated /archive/refs/tags/ tarball. GitHub regenerates those on
# demand, and a change to how it generates them broke sha256 checksums across
# Homebrew, Nix and others in early 2023; it aims for stability now but does
# not guarantee it. An uploaded asset is a static file, so its checksum cannot
# drift -- and it is the one the release workflow already hashes for you.
class Amux < Formula
  desc "Terminal session manager: detach/attach with full history and fast reattach"
  homepage "https://github.com/errantdata/amux"
  url "https://github.com/errantdata/amux/releases/download/v1.0.0/amux-1.0.0.tar.gz"
  sha256 "REPLACE_WITH_SHA256_OF_THE_SOURCE_TARBALL"
  license "ISC"
  head "https://github.com/errantdata/amux.git", branch: "main"

  def install
    system "./configure", "--prefix=#{prefix}", "--mandir=#{man}"
    system "make"
    system "make", "install"
    zsh_completion.install "contrib/amux.zsh" => "_amux"
    doc.install "DESIGN.md", "README.md"
  end

  test do
    # Keep sockets inside the sandbox rather than $HOME/.amux.
    ENV["AMUX_SOCKET_DIR"] = testpath

    # Create a detached session, confirm it is listed, then confirm the
    # history dump added by this fork answers for it.
    system bin/"amux", "-n", "brewtest", "sh", "-c", "echo hello-from-amux; sleep 10"
    sleep 1
    assert_match "brewtest", shell_output("#{bin}/amux")
    assert_match "hello-from-amux", shell_output("#{bin}/amux -H brewtest")
  end
end
