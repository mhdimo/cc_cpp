#include <catch2/catch_test_macros.hpp>

#include "utils/fs.hpp"
#include "utils/git.hpp"
#include "utils/path.hpp"
#include "utils/process.hpp"

#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace cc;

namespace {

/// A repository of its own, with a file that changed in it.
struct Repository {
    explicit Repository(const std::string& name) {
        const char* base = std::getenv("TMPDIR");
        std::string root = base != nullptr ? base : "/tmp";
        while (!root.empty() && root.back() == '/') root.pop_back();

        path = path_join(path_join(root, "cc-git-test-" + std::to_string(::getpid())), name);
        remove_dir_all(path);
        mkdirp(path);
        run_process("git init -q && git config user.email t@t && git config user.name t", path, "",
                    30);
    }

    ~Repository() { remove_dir_all(path); }

    std::string path;
};

} // namespace

TEST_CASE("The number of the files that changed is asked for") {
    const Repository repository("count");

    // A file that is written and added is a file that changed.
    write_file(repository.path + "/one.txt", "one\n");
    run_process("git add one.txt && git commit -q -m first", repository.path, "", 30);
    write_file(repository.path + "/one.txt", "one\ntwo\n");

    REQUIRE(git::changed_file_count(repository.path) == 1);
}

TEST_CASE("A folder that is not a repository has no files that changed") {
    REQUIRE(git::changed_file_count("/nowhere/at/all") == 0);
}
