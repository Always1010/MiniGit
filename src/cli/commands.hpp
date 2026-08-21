#pragma once
#include <stdexcept>
#include <string_view>
#include <vector>
namespace minigit::cli {
using Arguments = std::vector<std::string_view>;
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// basic_commands.cpp
int run_help(const Arguments& args);
int run_version(const Arguments& args);
int run_init(const Arguments& args);
int run_hash_object(const Arguments& args);
int run_cat_file(const Arguments& args);
int run_add(const Arguments& args);
int run_config(const Arguments& args);
int run_commit(const Arguments& args);
int run_status(const Arguments& args);

// inspection_commands.cpp
int run_diff(const Arguments& args);
int run_show(const Arguments& args);
int run_log(const Arguments& args);
int run_reflog(const Arguments& args);

// checkout_commands.cpp
int run_switch(const Arguments& args);
int run_mv(const Arguments& args);
int run_rm(const Arguments& args);
int run_reset(const Arguments& args);
int run_restore(const Arguments& args);
int run_branch(const Arguments& args);
int run_tag(const Arguments& args);

// history_commands.cpp
int run_merge(const Arguments& args);
int run_rebase(const Arguments& args);
int run_replay(const Arguments& args);
int run_gc(const Arguments& args);
int run_recover(const Arguments& args);

// stash_commands.cpp
int run_stash(const Arguments& args);
}
