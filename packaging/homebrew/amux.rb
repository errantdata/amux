# Homebrew formula for amux.
#
# This file lives here for review; Homebrew reads it from a *tap* repository.
# See packaging/homebrew/README.md for how to publish it.
#
# On each release, update `url` to the new tag and `sha256` to the value in
# that release's SHA256SUMS for the source tarball (amux-<version>.tar.gz).
class Amux < Formula
  desc "Terminal session manager: detach/attach with full history and fast reattach"
  homepage "https://github.com/errantdata/amux"
  url "https://github.com/errantdata/amux/archive/refs/tags/v1.0.0.tar.gz"
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
