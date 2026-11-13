// SPDX-License-Identifier: Apache-2.0
#include "store.hpp"

#include <iostream>

int main(int argc, char** argv) {
    gchar* device = nullptr;
    gchar* instance = nullptr;
    gchar* xml = nullptr;
    gboolean hardware_only = FALSE;
    const GOptionEntry options[] = {
        {"device", 'd', 0, G_OPTION_ARG_FILENAME, &device, "HwBinder device (default /dev/hwbinder)", "PATH"},
        {"instance", 'i', 0, G_OPTION_ARG_STRING, &instance, "HIDL instance (default default)", "NAME"},
        {"codec-xml", 'x', 0, G_OPTION_ARG_FILENAME, &xml, "Android codec XML (default /vendor/etc/media_codecs_c2.xml)", "PATH"},
        {"hardware-only", 0, 0, G_OPTION_ARG_NONE, &hardware_only, "Show hardware AVC/HEVC decoders only", nullptr},
        {nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr},
    };
    std::unique_ptr<GOptionContext, decltype(&g_option_context_free)> context(
        g_option_context_new("- enumerate Android Codec2 components"), g_option_context_free);
    g_option_context_add_main_entries(context.get(), options, nullptr);
    GError* error = nullptr;
    const bool parsed = g_option_context_parse(context.get(), &argc, &argv, &error);
    std::unique_ptr<gchar, decltype(&g_free)> free_device(device, g_free),
        free_instance(instance, g_free), free_xml(xml, g_free);
    if (!parsed || argc != 1 || (instance && (!*instance || std::strchr(instance, '/')))) {
        std::cerr << "c2-list: " << (error ? error->message : "invalid arguments (see --help)") << '\n';
        if (error) g_error_free(error);
        return 2;
    }

    try {
        const char* owner = instance ? instance : "default";
        const c2::Store store(device ? device : GBINDER_DEFAULT_HWBINDER, owner);
        auto traits = store.list();
        c2::classify(traits, owner, xml ? xml : c2::vendor_xml);
        std::cout << "Service: " << store.service() << "\nInterface chain:\n";
        for (const auto& iface : store.interfaces()) std::cout << "  " << iface << '\n';
        size_t matched = 0;
        std::cout << "NAME\tDOMAIN\tKIND\tMIME\tACCELERATION\tRANK\tFLAGS\tALIASES\n";
        for (const auto& trait : traits) {
            const bool selected = c2::hardware_video_decoder(trait);
            matched += selected;
            if (hardware_only && !selected) continue;
            std::cout << trait.name << '\t' << c2::to_string(trait.domain) << '\t'
                      << c2::to_string(trait.kind) << '\t' << trait.mime << '\t'
                      << c2::to_string(trait.acceleration) << '\t' << trait.rank << '\t';
            if (selected) std::cout << "avc/hevc ";
            if (trait.secure) std::cout << "secure ";
            if (trait.disabled) std::cout << "disabled ";
            std::cout << '\t';
            const char* sep = "";
            for (const auto& alias : trait.aliases) {
                std::cout << sep << alias;
                sep = ",";
            }
            std::cout << '\n';
        }
        std::cout << "HAL components: " << traits.size()
                  << "; hardware AVC/HEVC decoders: " << matched << '\n' << std::flush;
        return std::cout ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "c2-list: " << e.what() << '\n';
        return 1;
    }
}
