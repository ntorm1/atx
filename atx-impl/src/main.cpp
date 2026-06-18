#include <iostream>

#include "dispatch.hpp"

int main(int argc, char** argv) {
    return atx::impl::dispatch(argc, argv, std::cout, std::cerr);
}
