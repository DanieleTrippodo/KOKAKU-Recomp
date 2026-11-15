#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace kokaku {
// Decodes one encoded frame supplied by the game, including archive streams.
class AtracDecoder {
public:
    AtracDecoder();
    ~AtracDecoder();
    AtracDecoder(AtracDecoder &&) noexcept;
    AtracDecoder &operator=(AtracDecoder &&) noexcept;
    bool open(bool plus, unsigned rate, unsigned channels, unsigned block_align,
              std::span<const std::uint8_t> extra);
    bool decode(std::span<const std::uint8_t> packet, std::vector<std::uint8_t> &stereo_pcm);
    bool is_open() const noexcept;
    void close() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
