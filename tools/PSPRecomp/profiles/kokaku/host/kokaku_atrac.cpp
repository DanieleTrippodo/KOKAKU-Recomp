#include "kokaku_atrac.hpp"
#include <algorithm>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

namespace kokaku {
struct AtracDecoder::State {
    AVCodecContext *codec{};
    AVPacket *packet{};
    AVFrame *frame{};
    SwrContext *resampler{};
    bool ready{};
    ~State() {
        swr_free(&resampler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
    }
};
AtracDecoder::AtracDecoder() = default;
AtracDecoder::~AtracDecoder() = default;
AtracDecoder::AtracDecoder(AtracDecoder &&) noexcept = default;
AtracDecoder &AtracDecoder::operator=(AtracDecoder &&) noexcept = default;
bool AtracDecoder::is_open() const noexcept { return state_ && state_->ready; }
void AtracDecoder::close() noexcept { state_.reset(); }

bool AtracDecoder::open(bool plus, unsigned rate, unsigned channels, unsigned block_align,
                        std::span<const std::uint8_t> extra) {
    close();
    if (!rate || channels < 1 || channels > 2 || !block_align) return false;
    const AVCodec *decoder = avcodec_find_decoder(plus ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3);
    if (!decoder) return false;
    auto s = std::make_unique<State>();
    s->codec = avcodec_alloc_context3(decoder);
    s->packet = av_packet_alloc();
    s->frame = av_frame_alloc();
    if (!s->codec || !s->packet || !s->frame) return false;
    s->codec->sample_rate = rate;
    s->codec->block_align = block_align;
    av_channel_layout_default(&s->codec->ch_layout, channels);
    if (!plus && !extra.empty()) {
        s->codec->extradata = static_cast<std::uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!s->codec->extradata) return false;
        std::memcpy(s->codec->extradata, extra.data(), extra.size());
        s->codec->extradata_size = static_cast<int>(extra.size());
    }
    if (avcodec_open2(s->codec, decoder, nullptr) < 0) return false;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&s->resampler, &stereo, AV_SAMPLE_FMT_S16, rate,
                           &s->codec->ch_layout, s->codec->sample_fmt, rate, 0, nullptr) < 0 ||
        swr_init(s->resampler) < 0) return false;
    s->ready = true;
    state_ = std::move(s);
    return true;
}

bool AtracDecoder::decode(std::span<const std::uint8_t> encoded, std::vector<std::uint8_t> &pcm) {
    pcm.clear();
    if (!is_open() || encoded.empty() || encoded.size() > 65536) return false;
    auto &s = *state_;
    av_packet_unref(s.packet);
    if (av_new_packet(s.packet, static_cast<int>(encoded.size())) < 0) return false;
    std::memcpy(s.packet->data, encoded.data(), encoded.size());
    const int sent = avcodec_send_packet(s.codec, s.packet);
    av_packet_unref(s.packet);
    if (sent < 0) return false;
    while (true) {
        const int got = avcodec_receive_frame(s.codec, s.frame);
        if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return true;
        if (got < 0) return false;
        const auto begin = pcm.size();
        const int capacity = swr_get_out_samples(s.resampler, s.frame->nb_samples);
        if (capacity < 0) { av_frame_unref(s.frame); return false; }
        pcm.resize(begin + static_cast<std::size_t>(capacity) * 4);
        std::uint8_t *target = pcm.data() + begin;
        const int count = swr_convert(s.resampler, &target, capacity,
            const_cast<const std::uint8_t **>(s.frame->extended_data), s.frame->nb_samples);
        av_frame_unref(s.frame);
        if (count < 0) return false;
        pcm.resize(begin + static_cast<std::size_t>(count) * 4);
    }
}
} // namespace kokaku
