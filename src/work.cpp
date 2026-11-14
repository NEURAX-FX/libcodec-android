// SPDX-License-Identifier: Apache-2.0
#include "work.hpp"

#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace c2 {
namespace {

// HIDL 1.0: sizes and member offsets cross-checked against the generated
// writeEmbeddedToParcel() methods; offsets inside a vector are relative to
// the entire child array, not to a separate Binder object for each element.
struct Ordinal { uint64_t timestamp, index, custom; };
struct Frame { uint32_t flags; Ordinal ordinal; GBinderHidlVec buffers, config, info; };
struct Worklet { uint32_t component; GBinderHidlVec tunings, failures; Frame output; };
struct Work { GBinderHidlVec chain; Frame input; GBinderHidlVec worklets; uint32_t processed; int32_t result; };
struct Bundle { GBinderHidlVec works, blocks; };
struct Block { uint32_t index; GBinderHidlVec meta; GBinderHidlHandle fence; };
struct Buffer { GBinderHidlVec info, blocks; };
struct InfoBuffer { uint32_t index; Buffer buffer; };
struct alignas(8) BaseBlock { uint8_t tag; alignas(8) GBinderHidlHandle native; uint64_t padding[3]; };
struct LinearHandle { GBinderFds header; int fd; uint32_t size_lo, size_hi, magic; };
struct Range { uint32_t offset, length; };
static_assert(sizeof(Bundle) == 32 && sizeof(Work) == 120 && sizeof(Worklet) == 120);
static_assert(sizeof(Frame) == 80 && offsetof(Frame, buffers) == 32 && offsetof(Work, worklets) == 96);
static_assert(sizeof(Block) == 40 && sizeof(Buffer) == 32 && sizeof(InfoBuffer) == 40);
static_assert(sizeof(BaseBlock) == 48 && offsetof(BaseBlock, native) == 8);
static_assert(sizeof(LinearHandle) == 28 && offsetof(LinearHandle, fd) == sizeof(GBinderFds));
constexpr uint32_t range_index = 0xe0000000, crop_index = 0xe0000001;
constexpr uint32_t linear_magic = 0xc2696f00; // shared C2AllocatorIon / C2DmaBufAllocator ABI

// All embedded memory and the dup'd fd must survive async request cancellation.
template<class T> T* make(GBinderWriter& writer) {
    return new (gbinder_writer_malloc(&writer, sizeof(T))) T{};
}

guint vec(GBinderWriter& writer, guint parent, guint offset, GBinderHidlVec& target,
          const void* data = nullptr, guint count = 0, size_t size = 0) {
    target.data.ptr = data;
    target.count = count;
    const GBinderParent at{parent, offset};
    return gbinder_writer_append_buffer_object_with_parent(&writer, data, count * size, &at);
}

void append_frame(GBinderWriter& writer, guint parent, guint offset, Frame& frame, uint32_t length) {
    Buffer* buffer = length ? make<Buffer>(writer) : nullptr;
    const auto buffers = vec(writer, parent, offset + offsetof(Frame, buffers), frame.buffers,
                             buffer, buffer ? 1 : 0, sizeof(Buffer));
    if (buffer) {
        vec(writer, buffers, offsetof(Buffer, info), buffer->info);
        auto* block = make<Block>(writer);
        const auto blocks = vec(writer, buffers, offsetof(Buffer, blocks), buffer->blocks,
                                block, 1, sizeof(Block));
        struct { ParamHeader header; Range payload; } meta{{16, range_index}, {0, length}};
        vec(writer, blocks, offsetof(Block, meta), block->meta,
            gbinder_writer_memdup(&writer, &meta, sizeof(meta)), sizeof(meta), 1);
        gbinder_writer_append_fds(&writer, nullptr, nullptr); // no fence after CPU sync ends
    }
    vec(writer, parent, offset + offsetof(Frame, config), frame.config);
    vec(writer, parent, offset + offsetof(Frame, info), frame.info);
}

struct BlockRef { uint32_t base; Crop crop; Parcel::Handle fence; };

void read_buffer(Parcel& parcel, const Parcel::Buffer& parent, size_t offset,
                 std::vector<BlockRef>* output) {
    size_t count;
    parcel.vector(parent, offset + offsetof(Buffer, info), 1, count);
    const auto blocks = parcel.vector(parent, offset + offsetof(Buffer, blocks), sizeof(Block), count);
    for (size_t i = 0; i < count; ++i) {
        const auto at = i * sizeof(Block);
        size_t bytes;
        const auto meta = parcel.vector(blocks, at + offsetof(Block, meta), 1, bytes);
        const auto fence = parcel.handle(blocks, at + offsetof(Block, fence));
        if (output) {
            const auto header = meta.at<ParamHeader>();
            if (header.index != crop_index || header.size != sizeof(ParamHeader) + sizeof(Crop) ||
                bytes != header.size) throw std::runtime_error("onWorkDone: expected graphic block crop");
            output->push_back({blocks.at<uint32_t>(at), meta.at<Crop>(sizeof(ParamHeader)), fence});
        }
    }
}

void read_frame(Parcel& parcel, const Parcel::Buffer& parent, size_t offset,
                std::vector<BlockRef>* output = nullptr) {
    size_t count;
    const auto buffers = parcel.vector(parent, offset + offsetof(Frame, buffers), sizeof(Buffer), count);
    for (size_t i = 0; i < count; ++i) read_buffer(parcel, buffers, i * sizeof(Buffer), output);
    parcel.vector(parent, offset + offsetof(Frame, config), 1, count);
    const auto infos = parcel.vector(parent, offset + offsetof(Frame, info), sizeof(InfoBuffer), count);
    for (size_t i = 0; i < count; ++i) {
        read_buffer(parcel, infos, i * sizeof(InfoBuffer) + offsetof(InfoBuffer, buffer), nullptr);
    }
}

} // namespace

void write_work(GBinderWriter& writer, const InputWork& input) {
    const bool data = input.length != 0;
    if (data ? input.fd < 0 || input.length > input.capacity : input.fd >= 0 || input.flags != flag_eos) {
        throw std::invalid_argument("invalid Codec2 input work");
    }
    LinearHandle* handle = nullptr;
    if (data) {
        handle = make<LinearHandle>(writer);
        const int fd = fcntl(input.fd, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "dup input DMA-BUF");
        gbinder_writer_add_cleanup(&writer, [](gpointer p) { close(GPOINTER_TO_INT(p)); }, GINT_TO_POINTER(fd));
        *handle = {{GBINDER_HIDL_FDS_VERSION, 1, 3}, fd, input.capacity, 0, linear_magic};
    }

    auto* bundle = make<Bundle>(writer);
    auto* work = make<Work>(writer);
    auto* worklet = make<Worklet>(writer);
    work->input.flags = input.flags;
    work->input.ordinal = {input.timestamp_us, input.index, 0};

    const auto root = gbinder_writer_append_buffer_object(&writer, bundle, sizeof(*bundle));
    const auto works = vec(writer, root, offsetof(Bundle, works), bundle->works, work, 1, sizeof(Work));
    vec(writer, works, offsetof(Work, chain), work->chain);
    append_frame(writer, works, offsetof(Work, input), work->input, input.length);
    const auto worklets = vec(writer, works, offsetof(Work, worklets), work->worklets, worklet, 1, sizeof(Worklet));
    vec(writer, worklets, offsetof(Worklet, tunings), worklet->tunings);
    vec(writer, worklets, offsetof(Worklet, failures), worklet->failures);
    append_frame(writer, worklets, offsetof(Worklet, output), worklet->output, 0);

    auto* base = data ? make<BaseBlock>(writer) : nullptr;
    const auto bases = vec(writer, root, offsetof(Bundle, blocks), bundle->blocks,
                           base, base ? 1 : 0, sizeof(BaseBlock));
    if (base) {
        base->native.data.fds = &handle->header;
        const GBinderParent at{bases, offsetof(BaseBlock, native)};
        gbinder_writer_append_fds(&writer, &handle->header, &at);
    }
}

void read_work_done(Parcel& parcel, const WorkDone& consumer) {
    const auto bundle = parcel.buffer(sizeof(Bundle));
    size_t count;
    const auto works = parcel.vector(bundle, offsetof(Bundle, works), sizeof(Work), count);
    struct Pending { WorkResult result; std::vector<BlockRef> refs; };
    std::vector<Pending> pending;
    pending.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto at = i * sizeof(Work);
        const auto work = works.at<Work>(at);
        if (work.result) throw std::runtime_error("onWorkDone: work " + std::to_string(work.input.ordinal.index) +
                                                  " returned Codec2 status " + std::to_string(work.result));
        size_t n;
        parcel.vector(works, at + offsetof(Work, chain), 1, n);
        read_frame(parcel, works, at + offsetof(Work, input));
        const auto worklets = parcel.vector(works, at + offsetof(Work, worklets), sizeof(Worklet), n);
        if (n != 1 || work.processed != 1) throw std::runtime_error("onWorkDone: expected one processed worklet");
        const auto worklet = worklets.at<Worklet>();
        parcel.vector(worklets, offsetof(Worklet, tunings), 1, n);
        parcel.vector(worklets, offsetof(Worklet, failures), 56, n);
        if (n != 0) throw std::runtime_error("onWorkDone: worklet had setting failures");
        Pending item{{work.input.ordinal.index, worklet.output.ordinal.timestamp, worklet.output.flags, {}}, {}};
        read_frame(parcel, worklets, offsetof(Worklet, output),
                   item.result.flags & (flag_drop | flag_discard | flag_codec_config) ? nullptr : &item.refs);
        pending.push_back(std::move(item));
    }
    const auto bases = parcel.vector(bundle, offsetof(Bundle, blocks), sizeof(BaseBlock), count);
    std::vector<Parcel::Handle> handles;
    handles.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto at = i * sizeof(BaseBlock);
        if (bases.at<uint8_t>(at) != 0) throw std::runtime_error("onWorkDone: non-native block (BufferPool not supported)");
        const auto handle = parcel.handle(bases, at + offsetof(BaseBlock, native));
        if (!handle) throw std::runtime_error("onWorkDone: null native block");
        handles.push_back(handle);
    }
    parcel.end();
    for (auto& item : pending) {
        for (const auto& ref : item.refs) {
            if (ref.base >= handles.size()) throw std::runtime_error("onWorkDone: invalid base-block index");
            item.result.blocks.push_back({ref.crop, handles[ref.base], ref.fence});
        }
        consumer(item.result);
    }
}

} // namespace c2
