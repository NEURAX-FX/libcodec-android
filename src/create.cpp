// SPDX-License-Identifier: Apache-2.0
#include "component.hpp"

#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    const char* device = GBINDER_DEFAULT_HWBINDER;
    const char* xml = c2::vendor_xml;
    std::string name;
    c2::PictureSize size{128, 96};
    bool verbose = false;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help") {
                std::cout << "Usage: c2-create [--name COMPONENT] [--size WxH] [--params] "
                             "[--device PATH] [--codec-xml PATH]\n";
                return 0;
            }
            if (arg == "--params") { verbose = true; continue; }
            if ((arg != "--name" && arg != "--size" && arg != "--device" && arg != "--codec-xml") ||
                i + 1 >= argc) throw std::invalid_argument("invalid arguments (see --help)");
            const char* value = argv[++i];
            if (!*value || std::string_view(value).substr(0, 2) == "--") {
                throw std::invalid_argument("missing value for " + std::string(arg));
            }
            if (arg == "--name") name = value;
            if (arg == "--size") size = c2::parse_size(value);
            if (arg == "--device") device = value;
            if (arg == "--codec-xml") xml = value;
        }
    } catch (const std::invalid_argument& e) {
        std::cerr << "c2-create: " << e.what() << '\n';
        return 2;
    }

    try {
        const c2::Store store(device, "default");
        auto traits = store.list();
        c2::classify(traits, "default", xml);
        const auto& selected = c2::choose_decoder(traits, "video/avc", name);
        c2::Component component(store, selected.name);
        std::cout << "Created: " << selected.name << std::endl;
        const auto descriptors = component.describe();
        std::cout << "Supported parameters: " << descriptors.size() << std::endl;
        if (verbose) {
            for (const auto& item : descriptors) {
                std::cout << "  0x" << std::hex << item.index << std::dec << ' ' << item.name << '\n';
            }
        }
        const auto actual = c2::configure_size(component, size);
        if (actual.width != size.width || actual.height != size.height) {
            throw std::runtime_error("HAL did not accept requested raw.size; returned " +
                                     std::to_string(actual.width) + 'x' + std::to_string(actual.height));
        }
        std::cout << "Configured: " << actual.width << 'x' << actual.height << std::endl;
        component.start();
        std::cout << "Started" << std::endl;
        component.stop();
        component.check();
        std::cout << "Stopped" << std::endl;
        component.release();
        component.check();
        std::cout << "Released\n" << std::flush;
        return std::cout ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "c2-create: " << e.what() << '\n';
        return 1;
    }
}
