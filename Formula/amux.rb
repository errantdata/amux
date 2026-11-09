# Homebrew formula for amux.
#
# This repository doubles as its own Homebrew tap, so there is no separate
# homebrew-* repository to maintain. Because the repo is not named
# `homebrew-amux`, users tap it by URL:
#
#   brew tap errantdata/amux https://github.com/errantdata/amux
#   brew install errantdata/amux/amux
#
# On each release: tag, then
#   curl -sL https://github.com/errantdata/amux/archive/refs/tags/vX.Y.Z.tar.gz \
#     | shasum -a 256
# and update `url` and `sha256` below.
class Amux < Formula
  desc "Terminal session manager: detach/attach with full history and fast reattach"
  homepage "https://github.com/errantdata/amux"
  url "https://github.com/errantdata/amux/archive/refs/tags/v1.0.1.tar.gz"
  sha256 "bd9ad3902e865f5cc1fdb8b5c399fdb260c1441ea77245472e27098840ba4b03"
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
