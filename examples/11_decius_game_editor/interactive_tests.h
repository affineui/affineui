#pragma once

#include <string_view>

namespace ge {

bool is_interactive_test(std::string_view name);
int run_interactive_test(std::string_view name);
void print_interactive_test_usage();

}  // namespace ge
