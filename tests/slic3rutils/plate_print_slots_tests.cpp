#include <catch2/catch.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

// GUI_App.hpp first: it brings in the Windows headers that the boost::asio parts of PartPlate.hpp need.
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/PlatePrintSlots.hpp"

using namespace Slic3r;
using Slic3r::GUI::claim_free_print_index;
using Slic3r::GUI::restored_plates_keep_print;
// Slic3r::PartPlateList is also forward-declared (UndoRedo.hpp), so name the GUI one in full.
using GuiPlateList = Slic3r::GUI::PartPlateList;
using Catch::Matchers::WithinAbs;

// Bug: after an undo/redo, a plate added later could share its Print with another plate. That
// Print carries the other plate's origin, so plate 1 sliced at X = x - 324 (a U1 plate stride),
// its prime tower was drawn on plate 2's spot, and the slice failed until the app restarted.
// The cause was a stray init() in PartPlateList::reset(false), which rewound the print-index
// counter while every plate's Print was still filed (a 2.1.2 merge left it behind).

TEST_CASE("A new print index skips every index already filed", "[PlatePrintSlots]")
{
    const std::map<int, int> prints{{0, 0}, {1, 0}, {3, 0}};
    const std::map<int, int> results{{0, 0}, {1, 0}, {2, 0}};

    int next = 0;
    REQUIRE(claim_free_print_index(prints, results, next) == 4);
    REQUIRE(next == 5);
    REQUIRE(claim_free_print_index(prints, results, next) == 5);

    int negative = -3;
    const std::map<int, int> empty;
    REQUIRE(claim_free_print_index(empty, empty, negative) == 0);
}

TEST_CASE("Restored plates that saved the same print index do not share a Print", "[PlatePrintSlots]")
{
    const std::set<int> registered{1, 2};
    auto is_registered = [&registered](int idx) { return registered.count(idx) > 0; };

    // Plates 2 and 3 both saved index 1: only the first keeps it.
    const std::vector<bool> keep = restored_plates_keep_print(std::vector<int>{2, 1, 1}, is_registered);
    REQUIRE(keep == std::vector<bool>{true, true, false});

    // An index that is gone, or a plate that never had one, needs a new Print too.
    REQUIRE(restored_plates_keep_print(std::vector<int>{7, -1}, is_registered) == std::vector<bool>{false, false});
}

namespace {
struct PlatePrint
{
    PrintBase   *print  = nullptr;
    GCodeProcessorResult *result = nullptr;
    int          index  = -1;
};

PlatePrint print_of(GuiPlateList &list, int plate)
{
    PlatePrint out;
    list.get_plate(plate)->get_print(&out.print, &out.result, &out.index);
    return out;
}
} // namespace

TEST_CASE("Undo/redo's plate reset keeps every plate on a Print of its own", "[PlatePrintSlots][PartPlateList]")
{
    Model         model;
    GuiPlateList  list(270, 270, 270, nullptr, &model, ptFFF);
    REQUIRE(list.create_plate(false) == 1);

    std::set<int>       old_indices;
    std::set<PrintBase*> old_prints;
    for (int i = 0; i < list.get_plate_count(); ++i) {
        const PlatePrint pp = print_of(list, i);
        old_indices.insert(pp.index);
        old_prints.insert(pp.print);
    }
    REQUIRE(old_indices.size() == 2);

    // UndoRedo::StackImpl::load_snapshot: reset(false), deserialize the plates, rebuild.
    // reset(false) must not create a plate of its own, nor rewind the print counter while the
    // old plates' Prints are still filed.
    list.reset(false);
    REQUIRE(list.get_plate_count() == 0);

    // A plate made from here on must take an index no live Print uses.
    REQUIRE(list.create_plate(false) == 0);
    REQUIRE(list.create_plate(false) == 1);
    for (int i = 0; i < list.get_plate_count(); ++i) {
        const PlatePrint pp = print_of(list, i);
        INFO("plate " << i << " print index " << pp.index);
        REQUIRE(old_indices.count(pp.index) == 0);
        REQUIRE(old_prints.count(pp.print) == 0);
    }

    std::vector<bool>        no_sliced;
    std::vector<std::string> no_paths;
    REQUIRE(list.rebuild_plates_after_deserialize(no_sliced, no_paths) == 0);

    // The symptom check: each plate slices with its own Print, at its own origin.
    const PlatePrint first  = print_of(list, 0);
    const PlatePrint second = print_of(list, 1);
    REQUIRE(first.print != nullptr);
    REQUIRE(second.print != nullptr);
    REQUIRE(first.print != second.print);
    REQUIRE(first.result != second.result);
    REQUIRE(first.index != second.index);
    for (int i = 0; i < list.get_plate_count(); ++i) {
        const PlatePrint pp           = print_of(list, i);
        const Vec3d      plate_origin = list.get_plate(i)->get_origin();
        const Vec3d      print_origin = static_cast<Print*>(pp.print)->get_plate_origin();
        INFO("plate " << i);
        REQUIRE_THAT(print_origin.x(), WithinAbs(plate_origin.x(), 1e-9));
        REQUIRE_THAT(print_origin.y(), WithinAbs(plate_origin.y(), 1e-9));
    }
    // Plate 2 sits one stride (270 * 1.2) to the right of plate 1.
    REQUIRE_THAT(list.get_plate(1)->get_origin().x() - list.get_plate(0)->get_origin().x(), WithinAbs(324.0, 1e-6));
}
