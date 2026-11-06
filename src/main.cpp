// Claude Code — a C++20 port of the Claude Code CLI, built on ai-sdk-cpp.

#include "cli/app.hpp"
#include "cli/args.hpp"
#include "core/error.hpp"
#include "core/version.hpp"
#include "utils/path.hpp"

#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv + 1, argv + argc);

    // The path of the program is remembered, for the port looks for the
    // files that came with it beside itself.
    cc::set_program_path(argc > 0 ? argv[0] : "");

    try {
        cc::cli::Args args = cc::cli::parse_args(arguments);

        if (args.show_help) {
            std::cout << cc::cli::help_text() << "\n";
            return 0;
        }
        if (args.show_version) {
            std::cout << cc::cli::version_text() << "\n";
            return 0;
        }

        cc::cli::App::Options options;
        options.args = std::move(args);
        options.cwd = cc::cwd();

        cc::cli::App app(std::move(options));
        return app.run();
    } catch (const cc::UsageError& error) {
        std::cerr << "error: " << error.what() << "\n\n";
        std::cerr << cc::cli::help_text() << "\n";
        return 2;
    } catch (const cc::ConfigError& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
