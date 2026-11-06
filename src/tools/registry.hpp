#pragma once

#include "sandbox/manager.hpp"
#include "tools/impl/common.hpp"
#include "tools/tool.hpp"

#include <memory>
#include <string>

namespace cc::tools {

class BashTool;
class ReadTool;
class ShellSession;

/// The tools available in a session.
///
/// The file tools share one `FileStateTracker` so the "read before write"
/// rule holds across them.
class Registry {
public:
    Registry();

    /// The tools, in the order they are offered to the model.
    const Tools& all() const { return tools_; }

    /// The names of the tools, in order.
    std::vector<std::string> names() const;

    /// Gives the tools the sandbox of the session.
    ///
    /// The commands of the `Bash` tool run inside it, and the prompt of the
    /// tool tells the model what it does.
    void set_sandbox(sandbox::Manager* manager);

    /// Gives the tool of the shell the shell of the session.
    ///
    /// The shell stays alive for the whole session, so that a `cd` of a
    /// command moves the folder the session is in, the way the reference
    /// does it.
    void set_shell_session(ShellSession* session);

    /// True when the model of the session reads the PDF files.
    ///
    /// The prompt of the `Read` tool leaves the line about the PDF files out
    /// when the model does not.
    void set_pdf_supported(bool supported);

    /// True when the instructions about git belong in the prompt of the
    /// `Bash` tool.
    void set_include_git_instructions(bool included);

private:
    std::shared_ptr<FileStateTracker> tracker_;
    Tools tools_;

    /// The tool that runs the commands of the session.
    std::shared_ptr<BashTool> bash_;

    /// The tool that reads the files of the session.
    std::shared_ptr<ReadTool> read_;
};

} // namespace cc::tools
