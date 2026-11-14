#include "config_env.h"
#include "nvn/nvn_mali_spirv.h"
#include <cassert>
#include <map>
#include <sstream>
int main() {
    std::istringstream input("\xef\xbb\xbf NX_SET_CACHE = 0\r\nNX_CLEARS_V2=0\nNX_CLEARS_V2=1\n"
                             "\tNX_MAX_ANISO = 1\nNX_EXP_NO_WB = 0\n#NX_BUFFER_CACHE=0\n"
                             "NX_MALI_SPIRV_OPT=0\nNX_BUFFER_CACHE=1\nNX_IMAGE_STORAGE=0\n"
                             "resolution=1280x720\nNX_BAD KEY=1\nNX_=1\n");
    std::map<std::string, std::string> env{{"NX_SET_CACHE", "1"}};
    nx::plat::load_config_environment(input, [&](auto k, auto v) { env[k] = v; });
    assert(env.size() == 7 && env["NX_SET_CACHE"] == "0" && env["NX_CLEARS_V2"] == "1");
    assert(env["NX_MAX_ANISO"] == "1" && env["NX_EXP_NO_WB"] == "0");
    assert(env["NX_MALI_SPIRV_OPT"] == "0" && env["NX_IMAGE_STORAGE"] == "0");
    std::vector<uint32_t> code{0x07230203, 0x10000, 0, 10, 0,
        (2u << 16) | 37u, 9, // OpTypeQueue must remain untouched
        (1u << 16), // OpNop
        (4u << 16) | 8u, 1, 2, 3, // OpLine
        (5u << 16) | 246u, 2, 3, 9, 7, // Unroll | DependencyLength, preserve operand
        (1u << 16) | 317u};
    std::vector<uint32_t> out;
    assert(nx::nvn::optimize_spirv_mali(code, out));
    assert(out.size() == 12 && out[5] == code[5] && out[6] == 9 && out[10] == 10 && out[11] == 7);
    std::vector<uint32_t> untouched{42};
    assert(!nx::nvn::optimize_spirv_mali(out, untouched) && untouched[0] == 42);
    code.push_back(0); // malformed instruction: no partial result published
    assert(!nx::nvn::optimize_spirv_mali(code, untouched) && untouched[0] == 42);
}
