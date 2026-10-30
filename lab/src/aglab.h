#pragma once
// aglab entry point (lab/src/aglab.cpp), shared by the CLI and, later, the mobile shells' C ABI.

namespace ag::lab {

int aglab_main(int argc, const char* const* argv);

} // namespace ag::lab
