#pragma once
#include <stdexcept>
namespace minigit {
class Error : public std::runtime_error { public: using std::runtime_error::runtime_error; };
}
