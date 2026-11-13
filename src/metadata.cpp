// SPDX-License-Identifier: Apache-2.0
#include "store.hpp"

#include <algorithm>
#include <stdexcept>

namespace c2 {
namespace {

struct CodecType {
    std::string mime;
    bool secure = false;
};

struct CodecInfo {
    std::string name;
    Kind kind;
    std::vector<CodecType> types;
    bool software = false;
    bool disabled = false;
};

const char* attribute(const gchar** names, const gchar** values, std::string_view key) {
    for (; *names; ++names, ++values) {
        if (*names == key) return *values;
    }
    return "";
}

bool truth(const char* value) {
    for (const char* yes : {"1", "true", "t", "yes", "y", "enabled"}) {
        if (!g_ascii_strcasecmp(value, yes)) return true;
    }
    return false;
}

struct CodecXml {
    std::vector<CodecInfo> rows;
    int depth = 0;
    Kind section = Kind::other;
    CodecInfo* codec = nullptr;
    static constexpr size_t no_type = SIZE_MAX;
    size_t declared_type = no_type;
    size_t nested_type = no_type;
    bool updating = false;

    static void start(GMarkupParseContext*, const gchar* name, const gchar** keys,
                      const gchar** values, gpointer opaque, GError** error) noexcept {
        auto& self = *static_cast<CodecXml*>(opaque);
        try {
            self.start_element(name, keys, values);
        } catch (const std::exception& e) {
            g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT, e.what());
        }
    }

    static void end(GMarkupParseContext*, const gchar*, gpointer opaque, GError**) noexcept {
        auto& self = *static_cast<CodecXml*>(opaque);
        if (self.depth == 4) self.nested_type = no_type;
        if (self.depth == 3) {
            self.codec = nullptr;
            self.declared_type = no_type;
        }
        if (self.depth == 2) self.section = Kind::other;
        --self.depth;
    }

    void start_element(std::string_view element, const gchar** keys, const gchar** values) {
        const auto attr = [&](auto key) { return attribute(keys, values, key); };
        ++depth;
        if (depth == 1 && element != "MediaCodecs") throw std::runtime_error("expected <MediaCodecs>");
        if (depth == 2) {
            if (element == "Decoders") section = Kind::decoder;
            if (element == "Encoders") section = Kind::encoder;
        } else if (depth == 3 && section != Kind::other && element == "MediaCodec") {
            const std::string_view name(attr("name"));
            if (name.empty()) return;
            auto item = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
                return row.kind == section && row.name == name;
            });
            updating = truth(attr("update"));
            if (updating) {
                if (item == rows.end()) return;
                const std::string_view mime(attr("type"));
                if (mime.empty()) {
                    if (item->types.size() != 1) return;
                    declared_type = 0;
                } else {
                    const auto found = std::find_if(item->types.begin(), item->types.end(),
                        [&](const auto& entry) { return entry.mime == mime; });
                    if (found == item->types.end()) return;
                    declared_type = static_cast<size_t>(found - item->types.begin());
                }
                codec = &*item;
            } else {
                if (item != rows.end()) return;
                rows.push_back({std::string(name), section, {}});
                codec = &rows.back();
                if (*attr("type")) {
                    codec->types.push_back({attr("type")});
                    declared_type = 0;
                }
            }
            const auto* enabled = attr("enabled");
            if (*enabled) codec->disabled = !truth(enabled);
            else if (!updating && *attr("domain")) codec->disabled = true;
        } else if (codec && depth == 4 && element == "Type") {
            const std::string_view mime(attr("name"));
            if (mime.empty()) return;
            const auto found = std::find_if(codec->types.begin(), codec->types.end(),
                [&](const auto& entry) { return entry.mime == mime; });
            const bool update_type = updating || truth(attr("update"));
            if (update_type && found != codec->types.end()) {
                nested_type = static_cast<size_t>(found - codec->types.begin());
            } else if (!update_type && found == codec->types.end()) {
                nested_type = codec->types.size();
                codec->types.push_back({std::string(mime)});
            }
        } else if (codec && depth == 4 && element == "Attribute") {
            const std::string_view value(attr("name"));
            codec->software |= value == "software-codec";
            codec->disabled |= value == "disabled";
        } else if (codec && (depth == 4 || depth == 5) && element == "Feature" &&
                   std::string_view(attr("name")) == "secure-playback") {
            const auto index = depth == 4 ? declared_type : nested_type;
            if (index == no_type) return;
            const auto* optional = attr("optional");
            codec->types[index].secure = truth(attr("required")) ||
                (*optional && !truth(optional)) || truth(attr("value"));
        }
    }
};

} // namespace

void classify(std::vector<Traits>& traits, std::string_view instance, const char* xml_path) {
    const bool software_store = instance == "software" || instance == "vendor-software";
    for (auto& item : traits) {
        item.acceleration = software_store ? Acceleration::software : Acceleration::unknown;
        item.secure = item.disabled = false;
    }
    if (!xml_path) return;

    gchar* data = nullptr;
    gsize size;
    GError* raw_error = nullptr;
    if (!g_file_get_contents(xml_path, &data, &size, &raw_error)) {
        std::unique_ptr<GError, decltype(&g_error_free)> error(raw_error, g_error_free);
        if (g_error_matches(error.get(), G_FILE_ERROR, G_FILE_ERROR_NOENT) &&
            std::strcmp(xml_path, vendor_xml) == 0) return;
        throw std::runtime_error(error->message);
    }
    std::unique_ptr<gchar, decltype(&g_free)> xml(data, g_free);
    CodecXml state;
    const GMarkupParser parser{CodecXml::start, CodecXml::end, nullptr, nullptr, nullptr};
    std::unique_ptr<GMarkupParseContext, decltype(&g_markup_parse_context_free)> context(
        g_markup_parse_context_new(&parser, G_MARKUP_PREFIX_ERROR_POSITION, &state, nullptr),
        g_markup_parse_context_free);
    if (!g_markup_parse_context_parse(context.get(), xml.get(), size, &raw_error) ||
        !g_markup_parse_context_end_parse(context.get(), &raw_error)) {
        std::unique_ptr<GError, decltype(&g_error_free)> error(raw_error, g_error_free);
        throw std::runtime_error(std::string(xml_path) + ": " + error->message);
    }

    // Codec2InfoBuilder derives hardware acceleration from the codec's owner
    // plus its XML attributes. Do not label unknown records from the name.
    for (auto& trait : traits) {
        for (const auto& codec : state.rows) {
            if (codec.name != trait.name || codec.kind != trait.kind) continue;
            const auto type = std::find_if(codec.types.begin(), codec.types.end(),
                [&](const auto& entry) { return entry.mime == trait.mime; });
            if (type == codec.types.end()) continue;
            trait.disabled = codec.disabled;
            trait.secure = type->secure;
            if (codec.software) trait.acceleration = Acceleration::software;
            else if (trait.acceleration == Acceleration::unknown && instance == "default") {
                trait.acceleration = Acceleration::hardware;
            }
        }
    }
}

} // namespace c2
