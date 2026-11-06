# The Homebrew formula of the C++ port of the Claude Code CLI.
#
# Build the release first:
#
#     scripts/package.sh
#
# and then install the formula:
#
#     brew install --build-from-source ./packaging/homebrew/cc-cpp.rb
#
# The archive the formula names is the one the package script writes, so the
# version and the architecture of the formula have to follow the name of the
# file it points at. The name carries the triple of the machine.

class ClaudeCpp < Formula
  desc "An interactive agent that writes and runs code"
  homepage "https://github.com/mhdimo/cc_cpp"
  version "0.1.0"

  triple = Hardware::CPU.arch == :arm64 ? "aarch64-apple-darwin" : "x86_64-apple-darwin"
  url "file:///Users/liang/cc_cpp/dist/claude-code-cpp-#{version}-#{triple}.tar.gz"

  depends_on "cmake" => :build

  def install
    bin.install "cc++"
    if File.exist?("share/man/man1/cc++.1")
      man1.install "share/man/man1/cc++.1"
    end
    doc.install "README.md" if File.exist?("README.md")
    doc.install "CHANGELOG.md" if File.exist?("CHANGELOG.md")
  end

  def caveats
    <<~EOS
      The program reads its settings from ~/.claude and ~/.claude.json, and
      its transcripts from ~/.claude/projects, the way the reference does.
    EOS
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/cc++ --version")
  end
end
