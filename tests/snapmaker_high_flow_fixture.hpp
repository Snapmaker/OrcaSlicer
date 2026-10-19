#pragma once

// Snapmaker Orca: in-memory High Flow data for U1 sizes other than 0.4 mm (the only one shipped
// with it), so tests of size_offers_high_flow (src/slic3r/GUI/HighFlowNotices.cpp) can give a
// loaded bundle a High Flow 0.6 mm machine preset and filament. Nothing is written to disk.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { namespace Test {

// Gives `machine` a Standard and a High Flow column per tool head, both holding the head's own
// value (the invariants M1 / M2 of resources/profiles/Snapmaker/README.md), and the declaration
// keys in the layout of Snapmaker U1 (0.4 nozzle).
inline void declare_high_flow_per_head(DynamicPrintConfig &machine)
{
    const auto *diameters = machine.option<ConfigOptionFloats>("nozzle_diameter");
    REQUIRE(diameters != nullptr);
    const size_t heads = diameters->size();
    REQUIRE(heads > 0);
    for (const std::set<std::string> *keys : {&printer_options_with_variant_1, &printer_options_with_variant_2}) {
        const size_t stride = keys == &printer_options_with_variant_2 ? 2 : 1;
        for (const std::string &key : *keys) {
            if (key == "printer_extruder_id" || key == "printer_extruder_variant")
                continue;
            auto *option = dynamic_cast<ConfigOptionVectorBase *>(machine.option(key));
            if (option == nullptr || option->size() != stride * heads)
                continue;
            const std::unique_ptr<ConfigOption> narrow(option->clone());
            option->resize(2 * stride * heads, narrow.get());
            for (size_t head = 0; head < heads; ++head)
                for (size_t column = 0; column < 2; ++column)
                    for (size_t k = 0; k < stride; ++k)
                        option->set_at(narrow.get(), (2 * head + column) * stride + k, head * stride + k);
        }
    }
    std::vector<int>         ids;
    std::vector<std::string> variants;
    for (size_t head = 0; head < heads; ++head)
        for (const char *variant : {"Direct Drive Standard", "Direct Drive High Flow"}) {
            ids.emplace_back(int(head) + 1);
            variants.emplace_back(variant);
        }
    machine.set_key_value("extruder_variant_list",
                          new ConfigOptionStrings(std::vector<std::string>(heads, "Direct Drive Standard,Direct Drive High Flow")));
    machine.set_key_value("printer_extruder_id",      new ConfigOptionInts(ids));
    machine.set_key_value("printer_extruder_variant", new ConfigOptionStrings(variants));
}

// The U1 0.6 mm machine preset of `bundle` declares High Flow for every tool head, as a vendor
// update of Snapmaker's would make it. Returns that system preset.
inline Preset &u1_0_6_declares_high_flow(PresetBundle &bundle)
{
    Preset *machine = bundle.printers.find_preset("Snapmaker U1 (0.6 nozzle)", false, true);
    REQUIRE(machine != nullptr);
    declare_high_flow_per_head(machine->config);
    return *machine;
}

// Gives the one-column filament preset `name` of `bundle` a High Flow column: every variant-aware
// key two wide, `high_flow_values` (text form) in the High Flow column, other keys equal in both.
// Returns that system preset.
inline Preset &filament_gets_high_flow_column(PresetBundle &bundle, const std::string &name,
                                              const std::vector<std::pair<std::string, std::string>> &high_flow_values)
{
    Preset *preset = bundle.filaments.find_preset(name, false, true);
    REQUIRE(preset != nullptr);
    DynamicPrintConfig &config   = preset->config;
    const auto         *variants = config.option<ConfigOptionStrings>("filament_extruder_variant");
    REQUIRE(variants != nullptr);
    REQUIRE(variants->size() == 1);
    for (const std::string &key : filament_options_with_variant) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (option != nullptr && option->size() == 1)
            option->resize(2);
    }
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive High Flow"}));
    for (const auto &[key, value] : high_flow_values) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        REQUIRE(option != nullptr);
        REQUIRE(option->size() == 2);
        const std::unique_ptr<ConfigOption> parsed(option->clone());
        REQUIRE(parsed->deserialize(value));
        option->set_at(parsed.get(), 1, 0);
    }
    return *preset;
}

}} // namespace Slic3r::Test
