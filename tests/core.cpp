// SPDX-License-Identifier: Apache-2.0
#include "component.hpp"

#include <stdexcept>

namespace {

const char* xml;

c2::Traits video(const char* name, const char* mime, c2::Kind kind = c2::Kind::decoder) {
    return {name, c2::Domain::video, kind, 512, mime, {}};
}

void metadata() {
    std::vector<c2::Traits> traits{
        video("unrelated.prefix.hardware", "video/avc"), video("c2.mtk.software", "video/avc"),
        video("c2.mtk.unlisted.decoder", "video/avc"), video("vendor.disabled", "video/hevc"),
        video("vendor.encoder", "video/avc", c2::Kind::encoder),
        video("vendor.encoder", "video/avc"),
        video("unrelated.prefix.hardware", "video/hevc"),
        video("vendor.enabled", "video/avc"),
    };
    c2::classify(traits, "default", xml);
    g_assert_true(c2::hardware_video_decoder(traits[0]));
    g_assert_true(traits[1].acceleration == c2::Acceleration::software);
    g_assert_true(traits[2].acceleration == c2::Acceleration::unknown);
    g_assert_false(c2::hardware_video_decoder(traits[3]));
    g_assert_true(traits[3].disabled);
    g_assert_false(c2::hardware_video_decoder(traits[4]));
    g_assert_true(traits[4].acceleration == c2::Acceleration::hardware);
    g_assert_true(traits[5].acceleration == c2::Acceleration::unknown);
    g_assert_true(traits[6].acceleration == c2::Acceleration::unknown);
    g_assert_true(c2::hardware_video_decoder(traits[7]));
    try {
        (void)c2::choose_decoder(traits, "video/hevc", "vendor.disabled");
        g_assert_not_reached();
    } catch (const std::runtime_error&) {}
    c2::classify(traits, "software", xml);
    g_assert_true(traits[0].acceleration == c2::Acceleration::software);
}

void secure_mime_scope() {
    std::vector<c2::Traits> traits{video("vendor.mixed", "video/avc"),
                                  video("vendor.mixed", "video/hevc")};
    c2::classify(traits, "default", xml);
    g_assert_true(traits[0].secure);
    g_assert_false(traits[1].secure);
    try {
        (void)c2::choose_decoder(traits, "video/avc");
        g_assert_not_reached();
    } catch (const std::runtime_error&) {}
    const auto& clear = c2::choose_decoder(traits, "video/hevc");
    g_assert_cmpstr(clear.name.c_str(), ==, "vendor.mixed");
}

void secure_feature_forms() {
    std::vector<c2::Traits> traits{video("vendor.dual", "video/avc"),
                                  video("vendor.dual", "video/hevc"),
                                  video("vendor.value", "video/avc"),
                                  video("vendor.optional", "video/avc")};
    c2::classify(traits, "default", xml);
    g_assert_false(traits[0].secure);
    for (size_t i = 1; i < traits.size(); ++i) {
        g_assert_true(c2::hardware_video_decoder(traits[i]));
        g_assert_true(traits[i].secure);
        try {
            (void)c2::choose_decoder(traits, traits[i].mime, traits[i].name);
            g_assert_not_reached();
        } catch (const std::runtime_error&) {}
    }
}

void param_bytes() {
    const std::vector<uint8_t> fixture{
        12, 0, 0, 0, 2, 0x18, 0, 0xd2, 0x23, 0, 0, 0, 0, 0, 0, 0,
        16, 0, 0, 0, 0, 0x18, 0, 0xd2, 128, 0, 0, 0, 96, 0, 0, 0,
    };
    const auto packed = c2::pack_params({c2::make_param(0xd2001802, uint32_t{0x23}),
                                        c2::make_param(c2::output_size, c2::PictureSize{128, 96})});
    g_assert_true(packed == fixture);
    const auto parsed = c2::parse_params({fixture.data(), fixture.size(), 0});
    g_assert_cmpuint(parsed.size(), ==, 2);
    g_assert_cmphex(parsed[0].index, ==, 0xd2001802);
    g_assert_cmphex(parsed[1].index, ==, 0xd2001800);
    const auto size = c2::param_value<c2::PictureSize>(parsed[1]);
    g_assert_cmpuint(size.width, ==, 128);
    g_assert_cmpuint(size.height, ==, 96);
}

void malformed_param() {
    const uint8_t no_padding[12] = {12, 0, 0, 0, 2, 0x18, 0, 0xd2, 0x23};
    const uint8_t short_size[8] = {4, 0, 0, 0, 2, 0x18, 0, 0xd2};
    for (const auto& blob : {c2::Parcel::Buffer{no_padding, sizeof(no_padding), 0},
                            c2::Parcel::Buffer{short_size, sizeof(short_size), 0}}) {
        try {
            (void)c2::parse_params(blob);
            g_assert_not_reached();
        } catch (const std::runtime_error&) {}
    }
}

} // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_assert_cmpint(argc, ==, 2);
    xml = argv[1];
    g_test_add_func("/metadata/hardware", metadata);
    g_test_add_func("/metadata/secure-mime-scope", secure_mime_scope);
    g_test_add_func("/metadata/secure-feature-forms", secure_feature_forms);
    g_test_add_func("/params/bytes", param_bytes);
    g_test_add_func("/params/invalid-size", malformed_param);
    return g_test_run();
}
