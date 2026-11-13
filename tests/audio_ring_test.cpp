#include "audio_ring.h"
#include <array>
int main() {
    for (uint64_t position : {0ull, 1ull, 7ull, 8ull, 65535ull}) {
        for (size_t count = 0; count <= 8; ++count) {
            std::array<uint32_t, 8> ring{}, input{1,2,3,4,5,6,7,8}, output{};
            nx::plat::write_audio_ring(ring.data(), ring.size(), position, input.data(), count);
            nx::plat::read_audio_ring(output.data(), ring.data(), ring.size(), position, count);
            for (size_t i = 0; i < count; ++i) assert(output[i] == input[i]);
            for (size_t i = count; i < output.size(); ++i) assert(output[i] == 0);
        }
    }
}
