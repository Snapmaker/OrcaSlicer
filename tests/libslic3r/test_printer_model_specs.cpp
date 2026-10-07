// The Printer Selection table's size and toolhead columns (libslic3r/PrinterModelSpecs).
#include <catch2/catch.hpp>

#include <map>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/PrinterModelSpecs.hpp"

using namespace Slic3r;

TEST_CASE("printable_area_size: list, string, negative and malformed", "[PrinterModelSpecs]")
{
    auto sz = printable_area_size(nlohmann::json::array({ "0x0", "256x0", "256x256", "0x256" }));
    REQUIRE(sz);
    CHECK(sz->first == Approx(256.));
    CHECK(sz->second == Approx(256.));

    sz = printable_area_size(nlohmann::json("0x0,220x0,220x215,0x215"));
    REQUIRE(sz);
    CHECK(sz->first == Approx(220.));
    CHECK(sz->second == Approx(215.));

    // A delta bed as a polygon around the origin.
    sz = printable_area_size(nlohmann::json::array({ "-125x0", "0x-125", "125x0", "0x125" }));
    REQUIRE(sz);
    CHECK(sz->first == Approx(250.));
    CHECK(sz->second == Approx(250.));

    sz = printable_area_size(nlohmann::json::array({ "0x0", "350.5x0", "350.5x320", "0x320" }));
    REQUIRE(sz);
    CHECK(sz->first == Approx(350.5));

    CHECK_FALSE(printable_area_size(nlohmann::json()));
    CHECK_FALSE(printable_area_size(nlohmann::json(42)));
    CHECK_FALSE(printable_area_size(nlohmann::json::array({ "0x0" })));
    CHECK_FALSE(printable_area_size(nlohmann::json::array({ "0x0", "abc" })));
    CHECK_FALSE(printable_area_size(nlohmann::json::array({ "0x0", "10x" })));
    CHECK_FALSE(printable_area_size(nlohmann::json::array({ "0x0", "0x0" })));
    CHECK_FALSE(printable_area_size(nlohmann::json::array({ "0x0", 5 })));
}

TEST_CASE("profile_number: number, string, first of a list", "[PrinterModelSpecs]")
{
    CHECK(*profile_number(nlohmann::json(250)) == Approx(250.));
    CHECK(*profile_number(nlohmann::json("270.05")) == Approx(270.05));
    CHECK(*profile_number(nlohmann::json::array({ "1010" })) == Approx(1010.));
    CHECK_FALSE(profile_number(nlohmann::json("")));
    CHECK_FALSE(profile_number(nlohmann::json("250mm")));
    CHECK_FALSE(profile_number(nlohmann::json::array()));
    CHECK_FALSE(profile_number(nlohmann::json()));
}

TEST_CASE("MachinePresetIndex: inherited values, the 0.4 mm preset, fallbacks", "[PrinterModelSpecs]")
{
    MachinePresetIndex idx;
    CHECK_FALSE(idx.add(nlohmann::json::object({ { "inherits", "x" } }))); // no name
    CHECK_FALSE(idx.add(nlohmann::json("not an object")));

    idx.add({ { "name", "fdm_common" }, { "printable_area", { "0x0", "200x0", "200x180", "0x180" } }, { "printable_height", "150" },
              { "nozzle_diameter", { "0.4" } } });
    idx.add({ { "name", "fdm_dual" }, { "inherits", "fdm_common" }, { "nozzle_diameter", { "0.4", "0.4" } } });
    // A 0.2 and two 0.4 presets of the model; the shortest 0.4 name wins.
    idx.add({ { "name", "P 0.2 nozzle" }, { "inherits", "fdm_dual" }, { "instantiation", "true" }, { "printer_model", "P" },
              { "nozzle_diameter", { "0.2", "0.2" } }, { "printable_height", "140" } });
    idx.add({ { "name", "P HF 0.4 nozzle" }, { "inherits", "fdm_dual" }, { "instantiation", "true" }, { "printer_model", "P" },
              { "printable_height", "999" } });
    idx.add({ { "name", "P 0.4 nozzle" }, { "inherits", "fdm_dual" }, { "instantiation", "true" }, { "printer_model", "P" } });
    // Not instantiated: never picked even though it names the model.
    idx.add({ { "name", "P template" }, { "inherits", "fdm_dual" }, { "printer_model", "P" }, { "printable_height", "1" } });
    CHECK(idx.size() == 6);

    PrinterModelSpecs s = idx.specs_for_model("P", "0.4;0.2");
    CHECK(s.machine == "P 0.4 nozzle");
    REQUIRE(s.size_x);
    CHECK(*s.size_x == Approx(200.));
    CHECK(*s.size_y == Approx(180.));
    CHECK(*s.size_z == Approx(150.));
    CHECK(s.extruders == 2);

    // No 0.4 preset: the model's first listed nozzle.
    MachinePresetIndex only02;
    only02.add({ { "name", "Q 0.6 nozzle" }, { "instantiation", "true" }, { "printer_model", "Q" }, { "nozzle_diameter", { "0.6" } },
                 { "printable_area", "0x0,300x0,300x300,0x300" }, { "printable_height", 400 } });
    only02.add({ { "name", "Q 0.2 nozzle" }, { "instantiation", "true" }, { "printer_model", "Q" }, { "nozzle_diameter", { "0.2" } },
                 { "printable_area", "0x0,100x0,100x100,0x100" }, { "printable_height", 100 } });
    s = only02.specs_for_model("Q", "0.2;0.6");
    CHECK(s.machine == "Q 0.2 nozzle");
    CHECK(*s.size_x == Approx(100.));
    CHECK(s.extruders == 1);

    // printer_model inherited from the base.
    MachinePresetIndex inh;
    inh.add({ { "name", "base" }, { "printer_model", "R" }, { "nozzle_diameter", { "0.4", "0.4", "0.4", "0.4" } } });
    inh.add({ { "name", "R 0.4 nozzle" }, { "inherits", "base" }, { "instantiation", "true" } });
    s = inh.specs_for_model("R", "0.4");
    CHECK(s.machine == "R 0.4 nozzle");
    CHECK(s.extruders == 4);
    CHECK_FALSE(s.size_x); // no printable_area anywhere: unknown, not zero
    CHECK_FALSE(s.size_z);

    // Unknown model, and an inherits cycle, do not hang or throw.
    CHECK(idx.specs_for_model("nope", "0.4").machine.empty());
    MachinePresetIndex cyc;
    cyc.add({ { "name", "a" }, { "inherits", "b" }, { "instantiation", "true" }, { "printer_model", "C" } });
    cyc.add({ { "name", "b" }, { "inherits", "a" } });
    s = cyc.specs_for_model("C", "0.4");
    CHECK(s.machine == "a");
    CHECK_FALSE(s.size_x);
    CHECK_FALSE(s.extruders);
}

namespace {

nlohmann::json pms_read_json(const boost::filesystem::path &p)
{
    boost::nowide::ifstream f(p.string());
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(s, nullptr, false);
}

const boost::filesystem::path &pms_profiles_dir()
{
    static const boost::filesystem::path p = boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles";
    return p;
}

// What WebGuideDialog does per vendor: index every machine of the vendor, then ask per model.
std::map<std::string, PrinterModelSpecs> pms_vendor_specs(const std::string &vendor)
{
    std::map<std::string, PrinterModelSpecs> out;
    const nlohmann::json index = pms_read_json(pms_profiles_dir() / (vendor + ".json"));
    if (!index.is_object()) return out;
    const boost::filesystem::path dir = pms_profiles_dir() / vendor;
    MachinePresetIndex machines;
    if (index.contains("machine_list"))
        for (const nlohmann::json &m : index["machine_list"])
            if (m.contains("sub_path") && m["sub_path"].is_string())
                machines.add(pms_read_json(dir / m["sub_path"].get<std::string>()));
    if (index.contains("machine_model_list"))
        for (const nlohmann::json &m : index["machine_model_list"]) {
            if (!m.contains("sub_path") || !m["sub_path"].is_string() || !m.contains("name") || !m["name"].is_string()) continue;
            const nlohmann::json model = pms_read_json(dir / m["sub_path"].get<std::string>());
            const std::string nozzles = model.is_object() && model.contains("nozzle_diameter") && model["nozzle_diameter"].is_string() ?
                                            model["nozzle_diameter"].get<std::string>() : std::string();
            out[m["name"].get<std::string>()] = machines.specs_for_model(m["name"].get<std::string>(), nozzles);
        }
    return out;
}

} // namespace

TEST_CASE("Shipped profiles: sizes and toolheads of known printers", "[PrinterModelSpecs]")
{
    auto snap = pms_vendor_specs("Snapmaker");
    REQUIRE(snap.count("Snapmaker U1"));
    const PrinterModelSpecs &u1 = snap["Snapmaker U1"];
    CHECK(u1.extruders == 4);
    REQUIRE(u1.size_x);
    CHECK(*u1.size_x == Approx(270.));
    CHECK(*u1.size_y == Approx(270.));
    REQUIRE(u1.size_z);
    CHECK(*u1.size_z == Approx(270.).margin(0.5));

    auto bbl = pms_vendor_specs("BBL");
    REQUIRE(bbl.count("Bambu Lab H2D"));
    CHECK(bbl["Bambu Lab H2D"].extruders == 2);
    CHECK(*bbl["Bambu Lab H2D"].size_x == Approx(350.));
    CHECK(*bbl["Bambu Lab H2D"].size_y == Approx(320.));
    CHECK(*bbl["Bambu Lab H2D"].size_z == Approx(325.));
    REQUIRE(bbl.count("Bambu Lab X1 Carbon"));
    CHECK(bbl["Bambu Lab X1 Carbon"].extruders == 1);
    CHECK(*bbl["Bambu Lab X1 Carbon"].size_x == Approx(256.));

    auto prusa = pms_vendor_specs("Prusa");
    REQUIRE(prusa.count("Prusa CORE One INDX 4T"));
    REQUIRE(prusa.count("Prusa CORE One INDX 8T"));
    CHECK(prusa["Prusa CORE One INDX 4T"].extruders == 4);
    CHECK(prusa["Prusa CORE One INDX 8T"].extruders == 8);

    // printable_area as one comma-separated string.
    auto creality = pms_vendor_specs("Creality");
    REQUIRE(creality.count("Creality K2 SE"));
    REQUIRE(creality["Creality K2 SE"].size_x);
    CHECK(*creality["Creality K2 SE"].size_x == Approx(220.));
    CHECK(*creality["Creality K2 SE"].size_y == Approx(215.));
}

TEST_CASE("Shipped profiles: nearly every model has all four values", "[PrinterModelSpecs]")
{
    size_t total = 0, complete = 0;
    std::string missing;
    for (const auto &entry : boost::filesystem::directory_iterator(pms_profiles_dir())) {
        if (boost::filesystem::is_directory(entry) || entry.path().extension() != ".json") continue;
        for (const auto &[model, s] : pms_vendor_specs(entry.path().stem().string())) {
            ++total;
            if (s.size_x && s.size_y && s.size_z && s.extruders && *s.extruders >= 1)
                ++complete;
            else
                missing += entry.path().stem().string() + "/" + model + "; ";
        }
    }
    INFO("incomplete: " << missing);
    REQUIRE(total > 300);
    // Today one model (a Wanhao France entry with no machine preset) has none; allow a few more.
    CHECK(complete + 5 >= total);
}
