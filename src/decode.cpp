// SPDX-License-Identifier: Apache-2.0
#include "component.hpp"
#include "dmabuf.hpp"
#include "frame.hpp"
#include "input.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {

constexpr std::string_view usage =
    "Usage: c2-decode [--name COMPONENT] [--size WxH] [--format h264|hevc|obu]\n"
    "                 [--device PATH] [--codec-xml PATH] [--dma-heap PATH] INPUT OUTPUT.yuv\n"
    "Decode local H.264/HEVC/AV1 using Android Codec2 hardware; write packed 8-bit I420.\n"
    "AV1 inputs: IVF, Matroska/WebM, MP4, or raw low-overhead OBU.\n"
    "OUTPUT.yuv must not exist. Use --size if the input has no coded dimensions.\n";

struct Options {
    const char* input = nullptr;
    const char* output = nullptr;
    const char* device = GBINDER_DEFAULT_HWBINDER;
    const char* xml = c2::vendor_xml;
    const char* heap = "/dev/dma_heap/system";
    const char* format = nullptr;
    std::string_view name;
    c2::PictureSize size{};
};

Options options(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.substr(0, 2) != "--") {
            if (arg.empty() || opt.output) throw std::invalid_argument("expected INPUT and OUTPUT.yuv");
            (opt.input ? opt.output : opt.input) = argv[i];
            continue;
        }
        if (++i == argc || !*argv[i] || std::string_view(argv[i]).substr(0, 2) == "--") {
            throw std::invalid_argument("missing value for " + std::string(arg));
        }
        const char* value = argv[i];
        if (arg == "--name") opt.name = value;
        else if (arg == "--size") opt.size = c2::parse_size(value);
        else if (arg == "--device") opt.device = value;
        else if (arg == "--codec-xml") opt.xml = value;
        else if (arg == "--dma-heap") opt.heap = value;
        else if (arg == "--format" &&
                 (!std::strcmp(value, "h264") || !std::strcmp(value, "hevc") || !std::strcmp(value, "obu"))) {
            opt.format = value;
        } else throw std::invalid_argument("invalid option or value for " + std::string(arg));
    }
    if (!opt.output) throw std::invalid_argument("expected INPUT and OUTPUT.yuv");
    return opt;
}

struct Completions {
    static constexpr size_t limit = 32;
    struct Slot { uint64_t index = 0; bool pending = false; };
    std::array<Slot, limit> slots{};
    c2::FrameWriter& writer;
    size_t pending = 0;
    uint64_t frames = 0;
    bool eos = false;

    void done(const c2::WorkResult& work) {
        auto& slot = slots[work.index % limit];
        if (eos || !slot.pending || slot.index != work.index) {
            throw std::runtime_error("onWorkDone: unexpected completion for work " + std::to_string(work.index));
        }
        if (work.blocks.size() > 1) throw std::runtime_error("onWorkDone: expected one block per frame");
        if (!work.blocks.empty()) {
            writer.write(work.blocks.front());
            ++frames;
        }
        if (!(work.flags & c2::flag_incomplete)) {
            slot.pending = false;
            --pending;
        }
        eos = work.flags & c2::flag_eos;
    }
};

template<class Ready> void wait(c2::Component& component, Ready ready) {
    if (ready()) return;
    bool expired = false;
    const auto timer = g_timeout_add_seconds(8, [](gpointer p) -> gboolean {
        *static_cast<bool*>(p) = true;
        return G_SOURCE_REMOVE;
    }, &expired);
    try {
        while (!ready() && !expired) {
            g_main_context_iteration(nullptr, TRUE);
            component.check();
        }
    } catch (...) {
        if (!expired) g_source_remove(timer);
        throw;
    }
    if (!expired) g_source_remove(timer);
    if (!ready()) throw std::runtime_error("timed out waiting for Codec2 onWorkDone");
}

void decode(const Options& opt) {
    c2::Input input(opt.input, opt.format);
    const auto size = opt.size.width ? opt.size : input.size();
    if (!size.width || !size.height) throw std::runtime_error("input has unknown size; supply --size WxH");
    if ((size.width | size.height) & 1) throw std::runtime_error("I420 output requires even dimensions");
    c2::Packet packet{};
    if (!input.next(packet)) throw std::runtime_error("input has no video access units");

    const auto heap = c2::open_dma_heap(opt.heap);
    const c2::Store store(opt.device, "default");
    auto traits = store.list();
    c2::classify(traits, "default", opt.xml);
    const auto& selected = c2::choose_decoder(traits, input.mime(), opt.name);
    c2::FrameWriter writer(opt.output, size);
    Completions completed{{}, writer};
    c2::Component component(store, selected.name, [&](const c2::WorkResult& work) { completed.done(work); });

    c2::configure_readback(component, size);
    component.start();

    uint64_t index = 0, timestamp = 0;
    auto submit = [&](uint64_t pts, uint32_t flags, int fd, uint32_t capacity, uint32_t length) {
        auto& slot = completed.slots[index % Completions::limit];
        wait(component, [&] { return !slot.pending; });
        if (completed.eos) throw std::runtime_error("Codec2 returned premature end of stream");
        slot = {index, true}; // callback may run before the queue reply
        ++completed.pending;
        component.queue({index++, pts, flags, fd, capacity, length});
    };
    do {
        uint32_t capacity;
        const auto fd = c2::upload_packet(heap.get(), packet.size, capacity, [&](auto* dst) {
            std::memcpy(dst, packet.data, packet.size);
        });
        submit(packet.timestamp_us, 0, fd.get(), capacity, packet.size);
        timestamp = std::max(timestamp, packet.timestamp_us);
    } while (input.next(packet));
    submit(timestamp, c2::flag_eos, -1, 0, 0);
    wait(component, [&] { return completed.eos && !completed.pending; });
    component.stop();
    component.release();
    component.check();
    if (!completed.frames) throw std::runtime_error("Codec2 returned no decoded frames");
    writer.finish();
    std::cout << "Decoded: " << completed.frames << " frame(s), " << size.width << 'x' << size.height
              << " I420, " << selected.name << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage;
        return 0;
    }
    Options opt;
    try {
        opt = options(argc, argv);
    } catch (const std::invalid_argument& e) {
        std::cerr << "c2-decode: " << e.what() << '\n' << usage;
        return 2;
    }
    try {
        decode(opt);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "c2-decode: " << e.what() << '\n';
        return 1;
    }
}
