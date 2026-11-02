// CalibrationFlow 实现（阶段 G2）。
// 迁移纪律：Plater 9 公有校准流 + 5 私有 PA 辅助（含 F1 移交 cut_horizontal）+
// priv record_start_print_preset + 自由函数 adjust_settings_for_flowrate_calib 逐字迁入；
// 改写仅限宿主解引用（priv 成员 / GUI_App 反查 / 非限定宿主方法）。
// 依赖规则：hpp 零宿主 include；cpp 级允许 Tab/GLCanvas3D/Selection 等（B2/B3/G1 cpp-only 先例）；
// 切片控制域头不入本 cpp（set_calib_params 走槽），但 MainFrame/GLCanvas3D
// 链仍触达 boost/asio——Windows 前置块必须置于一切 include 之前（hpp 链中 wx/defs.h 会先以
// WIN32_LEAN_AND_MEAN 引入 windows.h，winnls 必须在此前定义 CP_ACP）。
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>
#endif

#include "CalibrationFlow.hpp"
#include "../Plater.hpp"        // 阶段 H1 2.1：F3 q 桥接（preset_bundle/get_tab/get_mode_str/get_agent）
#include "../MainFrame.hpp"      // 阶段 H1 2.1：typed 注入 MainFrame*（select_tab）
#include "LayoutManager.hpp"     // 阶段 H1 2.1：late_bind 后 sidebar/obj_list/view3D 直读
#include "../GUI_Preview.hpp"    // 阶段 H1 2.1：View3D 完整类型（get_canvas3d/selection）
#include "SceneRefreshCoordinator.hpp"  // 阶段 H1 2.1：late_bind 后 changed_objects 直调
#include "ProjectSession.hpp"    // 阶段 H1 2.1：ctor 注入 ProjectSession&（new_project/add_model）
#include "SlicingController.hpp" // 阶段 H1 2.1：ctor 注入 SlicingController&（set_calib_params）
#include "ObjectEditCommands.hpp"  // 阶段 H1 2.1：ctor 注入 ObjectEditCommands&（apply_cut_object_to_model）

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <clocale>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/iterator/counting_iterator.hpp>

#include <wx/debug.h>      // wxCHECK_RET
#include <wx/string.h>
#include <wx/utils.h>      // wxBusyCursor
#include <wx/window.h>

#include "../GLCanvas3D.hpp"                 // GLCanvas3D / Selection / GizmosManager（槽解引用）
#include "../GUI_ObjectList.hpp"             // ObjectList（槽解引用）
#include "../I18N.hpp"                       // _L / _u8L
#include "../MainFrame.hpp"                  // MainFrame::tp3DEditor（select_tab 槽实参）
#include "../NotificationManager.hpp"        // push_notification
#include "../PartPlate.hpp"                  // PartPlateList / PartPlate
#include "../Selection.hpp"                  // TransformationType / selection 方法
#include "../Tab.hpp"                        // Tab::update_dirty / reload_config / update_ui_from_settings
#include "NotificationHub.hpp"
#include "libslic3r/Arrange.hpp"             // arrangement::ArrangePolygons / arrange / get_shrink_bedpts
#include "libslic3r/CutUtils.hpp"            // Cut / ModelObjectCutAttributes
#include "libslic3r/Flow.hpp"                // Flow::mm3_per_mm
#include "libslic3r/Geometry.hpp"            // get_extents
#include "libslic3r/PresetFlowVariant.hpp"   // get_nozzle_volume_type / get_preset_value_at
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "nlohmann/json.hpp"

using nlohmann::json;   // 原 Plater.cpp:225 `using namespace nlohmann` 的定点版（record 内 `json j;` 逐字）

namespace Slic3r {
namespace GUI {
namespace app {

namespace {

// —— 原 Plater.cpp 自由函数（Slic3r::GUI 命名空间），全仓唯一调用方为
// calib_flowrate，随迁入本 TU。逐字保留，宿主解引用改经 Host。——

} // namespace

void CalibrationFlow::adjust_settings_for_flowrate_calib(bool linear, int pass)
{
    auto print_config = &m_q->preset_bundle()->prints.get_edited_preset().config;
    auto printerConfig = &m_q->preset_bundle()->printers.get_edited_preset().config;
    auto filament_config = &m_q->preset_bundle()->filaments.get_edited_preset().config;

    /// --- scale ---
    // model is created for a 0.4 nozzle, scale z with nozzle size.
    const ConfigOptionFloats* nozzle_diameter_config = printerConfig->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->values.size() > 0);
    float nozzle_diameter = nozzle_diameter_config->values[0];
    float xyScale = nozzle_diameter / 0.6;
    //scale z to have 10 layers
    // 2 bottom, 5 top, 3 sparse infill
    double first_layer_height = print_config->option<ConfigOptionFloat>("initial_layer_print_height")->value;
    double layer_height = nozzle_diameter / 2.0; // prefer 0.2 layer height for 0.4 nozzle
    first_layer_height = std::max(first_layer_height, layer_height);

    const auto canvas    = m_layout->view3D->get_canvas3d();
    auto&      selection = canvas->get_selection();
    selection.setup_cache();
    TransformationType transformation_type;
    transformation_type.set_relative();
    float zscale = (first_layer_height + 9 * layer_height) / 2;
    // only enlarge
    if (xyScale > 1.2) {
        selection.scale({xyScale, xyScale, zscale}, transformation_type);
    } else {
        selection.scale({1, 1, zscale}, transformation_type);
    }
    canvas->do_scale("");

    auto cur_flowrate = filament_config->option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0);
    Flow infill_flow = Flow(nozzle_diameter * 1.2f, layer_height, nozzle_diameter);
    const auto *max_volumetric_speed_opt = filament_config->option<ConfigOptionFloats>("filament_max_volumetric_speed");
    const FilamentVolumeType volume_type = get_nozzle_volume_type(m_q->preset_bundle()->printers.get_edited_preset().config, 0);
    double filament_max_volumetric_speed = get_preset_value_at(*filament_config, *max_volumetric_speed_opt,
                                                               ConfigFlowDomain::Filament, volume_type);
    double max_infill_speed;
    if (linear)
        max_infill_speed = filament_max_volumetric_speed /
                           (infill_flow.mm3_per_mm() * (cur_flowrate + (pass == 2 ? 0.035 : 0.05)) / cur_flowrate);
    else
        max_infill_speed = filament_max_volumetric_speed / (infill_flow.mm3_per_mm() * (pass == 1 ? 1.2 : 1));

    double internal_solid_speed = std::floor(std::min(print_config->opt_float("internal_solid_infill_speed", 0), max_infill_speed));
    double top_surface_speed = std::floor(std::min(print_config->opt_float("top_surface_speed", 0), max_infill_speed));

    // adjust parameters
    for (auto _obj : m_model.objects) {
        _obj->ensure_on_bed();
        _obj->config.set_key_value("wall_loops", new ConfigOptionInt(1));
        _obj->config.set_key_value("only_one_wall_top", new ConfigOptionBool(true));
        _obj->config.set_key_value("thick_internal_bridges", new ConfigOptionBool(false));
        _obj->config.set_key_value("enable_extra_bridge_layer", new ConfigOptionEnum<EnableExtraBridgeLayer>(eblDisabled));
        _obj->config.set_key_value("internal_bridge_density", new ConfigOptionPercent(100));
        _obj->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(35));
        _obj->config.set_key_value("min_width_top_surface", new ConfigOptionFloatOrPercent(100,true));
        _obj->config.set_key_value("bottom_shell_layers", new ConfigOptionInt(2));
        _obj->config.set_key_value("top_shell_layers", new ConfigOptionInt(5));
        _obj->config.set_key_value("top_shell_thickness", new ConfigOptionFloat(0));
        _obj->config.set_key_value("bottom_shell_thickness", new ConfigOptionFloat(0));
        _obj->config.set_key_value("detect_thin_wall", new ConfigOptionBool(true));
        _obj->config.set_key_value("filter_out_gap_fill", new ConfigOptionFloat(0));
        _obj->config.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
        _obj->config.set_key_value("top_surface_line_width", new ConfigOptionFloatOrPercent(nozzle_diameter * 1.2f, false));
        _obj->config.set_key_value("internal_solid_infill_line_width", new ConfigOptionFloatOrPercent(nozzle_diameter * 1.2f, false));
        _obj->config.set_key_value("top_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipArchimedeanChords));
        _obj->config.set_key_value("top_solid_infill_flow_ratio", new ConfigOptionFloat(1.0f));
        _obj->config.set_key_value("infill_direction", new ConfigOptionFloat(45));
        _obj->config.set_key_value("solid_infill_direction", new ConfigOptionFloat(135));
        _obj->config.set_key_value("align_infill_direction_to_model", new ConfigOptionBool(true));
        _obj->config.set_key_value("ironing_type", new ConfigOptionEnum<IroningType>(IroningType::NoIroning));
        _obj->config.set_key_value("internal_solid_infill_speed", new ConfigOptionFloats { internal_solid_speed });
        _obj->config.set_key_value("top_surface_speed", new ConfigOptionFloats { top_surface_speed });
        _obj->config.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));
        _obj->config.set_key_value("gap_fill_target", new ConfigOptionEnum<GapFillTarget>(GapFillTarget::gftNowhere));
        print_config->set_key_value("max_volumetric_extrusion_rate_slope", new ConfigOptionFloats { 0. });
        _obj->config.set_key_value("calib_flowrate_topinfill_special_order", new ConfigOptionBool(true));

        // extract flowrate from name, filename format: flowrate_xxx
        std::string obj_name = _obj->name;
        assert(obj_name.length() > 9);
        obj_name = obj_name.substr(9);
        if (obj_name[0] == 'm')
            obj_name[0] = '-';
        // Orca: force set locale to C to avoid parsing error
        const std::string _loc = std::setlocale(LC_NUMERIC, nullptr);
        std::setlocale(LC_NUMERIC,"C");
        auto              modifier  = 1.0f;
        try {
            modifier = stof(obj_name);
        } catch (...) {
        }
        // restore locale
        std::setlocale(LC_NUMERIC, _loc.c_str());

        if(linear)
            _obj->config.set_key_value("print_flow_ratio", new ConfigOptionFloat((cur_flowrate + modifier)/cur_flowrate));
        else
            _obj->config.set_key_value("print_flow_ratio", new ConfigOptionFloat(1.0f + modifier/100.f));

    }

    print_config->set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    print_config->set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    print_config->set_key_value("initial_layer_print_height", new ConfigOptionFloat(first_layer_height));
    print_config->set_key_value("reduce_crossing_wall", new ConfigOptionBool(true));


    m_q->get_tab(int(Preset::TYPE_PRINT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINTER))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINT))->reload_config();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->reload_config();
    m_q->get_tab(int(Preset::TYPE_PRINTER))->reload_config();
}

CalibrationFlow::CalibrationFlow(NotificationHub& hub, Plater* q, Model& model, PartPlateList& partplate_list,
                             MainFrame* main_frame, ProjectSession& session, SlicingController& slicing,
                             ObjectEditCommands& obj_cmds)
    : m_hub(hub)
    , m_q(q)
    , m_model(model)
    , m_partplate_list(partplate_list)
    , m_main_frame(main_frame)
    , m_session(session)
    , m_slicing(slicing)
    , m_obj_cmds(obj_cmds)
{
}

void CalibrationFlow::late_bind(LayoutManager& layout, SceneRefreshCoordinator& refresh)
{
    m_layout = &layout;
    m_refresh = &refresh;
}

// —— 公有校准流 ——

void CalibrationFlow::calib_pa(const Calib_Params& params)
{
    const auto calib_pa_name = wxString::Format(L"Pressure Advance Test");
    m_session.new_project(false, false, calib_pa_name);
    m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
    switch (params.mode) {
        case CalibMode::Calib_PA_Line:
            m_session.add_model(false, Slic3r::resources_dir() + "/calib/pressure_advance/pressure_advance_test.stl");
            break;
        case CalibMode::Calib_PA_Pattern:
            _calib_pa_pattern(params);
            break;
        case CalibMode::Calib_PA_Tower:
            _calib_pa_tower(params);
            break;
        default: break;
    }
    auto printer_config = &m_q->preset_bundle()->printers.get_edited_preset().config;
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    m_slicing.background_process().fff_print()->set_calib_params(params);
}

void CalibrationFlow::_calib_pa_pattern(const Calib_Params& params)
{
    std::vector<double> speeds{params.speeds};
    std::vector<double> accels{params.accelerations};
    std::vector<size_t> object_idxs{};
    /* Set common parameters */
    auto printer_config = &m_q->preset_bundle()->printers.get_edited_preset().config;
    DynamicPrintConfig& print_config = m_q->preset_bundle()->prints.get_edited_preset().config;
    auto filament_config = &m_q->preset_bundle()->filaments.get_edited_preset().config;
    double nozzle_diameter = printer_config->option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);
    filament_config->set_key_value("filament_retract_when_changing_layer", new ConfigOptionBoolsNullable{false});
    filament_config->set_key_value("filament_wipe", new ConfigOptionBoolsNullable{false});
    printer_config->set_key_value("wipe", new ConfigOptionBools{false});
    printer_config->set_key_value("retract_when_changing_layer", new ConfigOptionBools{false});
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});

    //Orca: find acceleration to use in the test
    auto accel = print_config.option<ConfigOptionFloats>("outer_wall_acceleration")->values.front(); // get the outer wall acceleration
    if (accel == 0) // if outer wall accel isnt defined, fall back to inner wall accel
        accel = print_config.option<ConfigOptionFloats>("inner_wall_acceleration")->values.front();
    if (accel == 0) // if inner wall accel is not defined fall back to default accel
        accel = print_config.option<ConfigOptionFloats>("default_acceleration")->values.front();
    // Orca: Set all accelerations except first layer, as the first layer accel doesnt affect the PA test since accel
    // is set to the travel accel before printing the pattern.
    if (accels.empty()) {
        accels.assign({accel});
        const auto msg{_L("INFO:") + "\n" +
                       _L("No accelerations provided for calibration. Use default acceleration value ") + std::to_string(long(accel)) + wxString::FromUTF8("mm/s²")};
        m_hub.nm()->push_notification(msg.ToStdString());
    } else {
        // set max acceleration in case of batch mode to get correct test pattern size
        accel = *std::max_element(accels.begin(), accels.end());
    }
    print_config.set_key_value( "outer_wall_acceleration", new ConfigOptionFloats { accel });
    print_config.set_key_value( "print_sequence", new ConfigOptionEnum(PrintSequence::ByLayer));

    //Orca: find jerk value to use in the test
    if(print_config.option<ConfigOptionFloats>("default_jerk")->values.front() > 0){ // we have set a jerk value
        auto jerk = print_config.option<ConfigOptionFloats>("outer_wall_jerk")->values.front(); // get outer wall jerk
        if (jerk == 0) // if outer wall jerk is not defined, get inner wall jerk
            jerk = print_config.option<ConfigOptionFloats>("inner_wall_jerk")->values.front();
        if (jerk == 0) // if inner wall jerk is not defined, get the default jerk
            jerk = print_config.option<ConfigOptionFloats>("default_jerk")->values.front();

        //Orca: Set jerk values. Again first layer jerk should not matter as it is reset to the travel jerk before the
        // first PA pattern is printed.
        print_config.set_key_value( "default_jerk", new ConfigOptionFloats { jerk });
        print_config.set_key_value( "outer_wall_jerk", new ConfigOptionFloats { jerk });
        print_config.set_key_value( "inner_wall_jerk", new ConfigOptionFloats { jerk });
        print_config.set_key_value( "top_surface_jerk", new ConfigOptionFloats { jerk });
        print_config.set_key_value( "infill_jerk", new ConfigOptionFloats { jerk });
        print_config.set_key_value( "travel_jerk", new ConfigOptionFloats { jerk });
    }

    for (const auto& opt : SuggestedConfigCalibPAPattern().float_pairs) {
        print_config.set_key_value(
            opt.first,
            new ConfigOptionFloat(opt.second)
        );
    }

    for (const auto& opt : SuggestedConfigCalibPAPattern().nozzle_ratio_pairs) {
        print_config.set_key_value(
            opt.first,
            new ConfigOptionFloatOrPercent(nozzle_diameter * opt.second / 100, false)
        );
    }

    for (const auto& opt : SuggestedConfigCalibPAPattern().int_pairs) {
        print_config.set_key_value(
            opt.first,
            new ConfigOptionInt(opt.second)
        );
    }

    print_config.set_key_value(
        SuggestedConfigCalibPAPattern().brim_pair.first,
        new ConfigOptionEnum<BrimType>(SuggestedConfigCalibPAPattern().brim_pair.second)
    );

    // Orca: Set the outer wall speed to the optimal speed for the test, cap it with max volumetric speed
    if (speeds.empty()) {
        double speed = CalibPressureAdvance::find_optimal_PA_speed(
            m_q->preset_bundle()->full_config(),
            print_config.get_abs_value("line_width", nozzle_diameter),
            print_config.get_abs_value("layer_height"), 0);
        print_config.set_key_value("outer_wall_speed", new ConfigOptionFloats { speed });

        speeds.assign({speed});
        const auto msg{_L("INFO:") + "\n" +
                       _L("No speeds provided for calibration. Use default optimal speed ") + std::to_string(long(speed)) + "mm/s"};
        m_hub.nm()->push_notification(msg.ToStdString());
    } else if (speeds.size() == 1) {
        // If we have single value provided, set speed using global configuration.
        // per-object config is not set in this case
        print_config.set_key_value("outer_wall_speed", new ConfigOptionFloats { speeds.front() });
    }

    m_q->get_tab(int(Preset::TYPE_PRINT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINTER))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINT))->reload_config();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->reload_config();
    m_q->get_tab(int(Preset::TYPE_PRINTER))->reload_config();

    const DynamicPrintConfig full_config = m_q->preset_bundle()->full_config();
    PresetBundle* preset_bundle = m_q->preset_bundle();
    const bool is_bbl_machine = preset_bundle->is_bbl_vendor();
    auto cur_plate = m_partplate_list.get_plate(0);

    // add "handle" cube
    m_layout->sidebar->obj_list()->load_generic_subobject("Cube", ModelVolumeType::INVALID);
    auto *cube = m_model.objects[0];

    CalibPressureAdvancePattern pa_pattern(
        params,
        full_config,
        is_bbl_machine,
        *cube,
        cur_plate->get_origin()
    );

    /* Having PA pattern configured, we could make a set of polygons resembling N test patterns.
     * We'll arrange this set of polygons, so we would know position of each test pattern and
     * could position test cubes later on
     *
     * We'll take advantage of already existing cube: scale it up to test pattern size to use
     * as a reference for objects arrangement. Polygon is slightly oversized to add spaces between patterns.
     * That arrangement will be used to place 'handle cubes' for each test. */
    auto cube_bb = cube->raw_bounding_box();
    cube->scale((pa_pattern.print_size_x() + 4) / cube_bb.size().x(),
                (pa_pattern.print_size_y() + 4) / cube_bb.size().y(),
                pa_pattern.max_layer_z() / cube_bb.size().z());

    arrangement::ArrangePolygons arranged_items;
    {
        arrangement::ArrangeParams ap;
        Points bedpts = arrangement::get_shrink_bedpts(&full_config, ap);

        for(size_t i = 0; i < speeds.size() * accels.size(); i++) {
            arrangement::ArrangePolygon p;
            cube->instances[0]->get_arrange_polygon(&p);
            p.bed_idx = 0;
            arranged_items.emplace_back(p);
        }

        arrangement::arrange(arranged_items, bedpts, ap);
    }

    /* scale cube back to the size of test pattern 'handle' */
    cube_bb = cube->raw_bounding_box();
    cube->scale(pa_pattern.handle_xy_size() / cube_bb.size().x(),
                pa_pattern.handle_xy_size() / cube_bb.size().y(),
                pa_pattern.max_layer_z() / cube_bb.size().z());

    /* Set speed and acceleration on per-object basis and arrange anchor object on the plates.
     * Test gcode will be genecated during plate slicing */
    for(size_t test_idx = 0; test_idx < arranged_items.size(); test_idx++) {
        const auto &ai = arranged_items[test_idx];
        size_t plate_idx = arranged_items[test_idx].bed_idx;
        auto tspd = speeds[test_idx % speeds.size()];
        auto tacc = accels[test_idx / speeds.size()];

        /* make an own copy of anchor cube for each test */
        auto obj = test_idx == 0 ? cube : m_model.add_object(*cube);
        auto obj_idx = std::distance(m_model.objects.begin(), std::find(m_model.objects.begin(), m_model.objects.end(), obj));
        obj->name.assign(std::string("pa_pattern_") + std::to_string(int(tspd)) + std::string("_") + std::to_string(int(tacc)));

        auto &obj_config = obj->config;
        if (speeds.size() > 1)
            obj_config.set_key_value("outer_wall_speed", new ConfigOptionFloats { tspd });
        if (accels.size() > 1)
            obj_config.set_key_value("outer_wall_acceleration", new ConfigOptionFloats { tacc });

        auto cur_plate = m_partplate_list.get_plate(plate_idx);
        if (!cur_plate) {
            plate_idx = m_partplate_list.create_plate();
            cur_plate = m_partplate_list.get_plate(plate_idx);
        }

        object_idxs.emplace_back(obj_idx);
        m_partplate_list.add_to_plate(obj_idx, 0, plate_idx);
        const Vec3d obj_offset{unscale<double>(ai.translation(X)),
                               unscale<double>(ai.translation(Y)),
                               0};
        obj->instances[0]->set_offset(cur_plate->get_origin() + obj_offset + pa_pattern.handle_pos_offset());
        obj->ensure_on_bed();

        if (obj_idx == 0)
            m_layout->sidebar->obj_list()->update_name_for_items();
        else
            m_layout->sidebar->obj_list()->add_object_to_list(obj_idx);
    }

    m_model.calib_pa_pattern = std::make_unique<CalibPressureAdvancePattern>(pa_pattern);
    m_refresh->changed_objects(object_idxs);
}

void CalibrationFlow::_calib_pa_pattern_gen_gcode()
{
    if (!m_model.calib_pa_pattern)
        return;

    auto cur_plate = m_partplate_list.get_curr_plate();
    if (cur_plate->empty())
        return;

    /* Container to store custom g-codes genereted by the test generator.
     * We'll store gcode for all tests on a single plate here. Once the plate handling is done,
     * all the g-codes will be merged into a single one on per-layer basis */
    std::vector<CustomGCode::Info> mgc;
    PresetBundle *preset_bundle = m_q->preset_bundle();

    /* iterate over all cubes on current plate and generate gcode for them */
    for (auto obj : cur_plate->get_objects_on_this_plate()) {
        auto gcode = m_model.calib_pa_pattern->generate_custom_gcodes(
                                preset_bundle->full_config(),
                                preset_bundle->is_bbl_vendor(),
                                *obj,
                                cur_plate->get_origin()
        );
        mgc.emplace_back(gcode);
    }

    // move first item into model custom gcode
    auto &pcgc = m_model.plates_custom_gcodes[m_partplate_list.get_curr_plate_index()];
    pcgc = std::move(mgc[0]);
    mgc.erase(mgc.begin());

    // concat layer gcodes for each test
    for (size_t i = 0; i < pcgc.gcodes.size(); i++) {
        for (auto &gc : mgc) {
            pcgc.gcodes[i].extra += gc.gcodes[i].extra;
        }
    }
}

void CalibrationFlow::cut_horizontal(size_t obj_idx, size_t instance_idx, double z, ModelObjectCutAttributes attributes)
{
    wxCHECK_RET(obj_idx < m_model.objects.size(), "obj_idx out of bounds");
    auto *object = m_model.objects[obj_idx];

    wxCHECK_RET(instance_idx < object->instances.size(), "instance_idx out of bounds");

    if (! attributes.has(ModelObjectCutAttribute::KeepUpper) && ! attributes.has(ModelObjectCutAttribute::KeepLower))
        return;

    wxBusyCursor wait;

    const Vec3d instance_offset = object->instances[instance_idx]->get_offset();
    Cut         cut(object, instance_idx, Geometry::translation_transform(z * Vec3d::UnitZ() - instance_offset), attributes);
    const auto  new_objects = cut.perform_with_plane();

    m_obj_cmds.apply_cut_object_to_model(obj_idx, new_objects);
}

void CalibrationFlow::_calib_pa_tower(const Calib_Params& params) {
    m_session.add_model(false, Slic3r::resources_dir() + "/calib/pressure_advance/tower_with_seam.stl");

    auto& print_config = m_q->preset_bundle()->prints.get_edited_preset().config;
    auto printer_config = &m_q->preset_bundle()->printers.get_edited_preset().config;
    auto filament_config = &m_q->preset_bundle()->filaments.get_edited_preset().config;

    const double nozzle_diameter = printer_config->option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);

    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats{ 1.0f });


    auto& obj_cfg = m_model.objects[0]->config;

    obj_cfg.set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    auto full_config = m_q->preset_bundle()->full_config();
    auto wall_speed = CalibPressureAdvance::find_optimal_PA_speed(
        full_config, full_config.get_abs_value("line_width", nozzle_diameter),
        full_config.get_abs_value("layer_height"), 0);
    obj_cfg.set_key_value("outer_wall_speed", new ConfigOptionFloats { wall_speed });
    obj_cfg.set_key_value("inner_wall_speed", new ConfigOptionFloats { wall_speed });
    obj_cfg.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spRear));
    obj_cfg.set_key_value("wall_loops", new ConfigOptionInt(2));
    obj_cfg.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    obj_cfg.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
    obj_cfg.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    obj_cfg.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btEar));
    obj_cfg.set_key_value("brim_object_gap", new ConfigOptionFloat(.0f));
    obj_cfg.set_key_value("brim_ears_max_angle", new ConfigOptionFloat(135.f));
    obj_cfg.set_key_value("brim_width", new ConfigOptionFloat(6.f));
    obj_cfg.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));
    print_config.set_key_value("max_volumetric_extrusion_rate_slope", new ConfigOptionFloats { 0. });

    m_refresh->changed_objects({ 0 });
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINTER))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINT))->reload_config();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->reload_config();
    m_q->get_tab(int(Preset::TYPE_PRINTER))->reload_config();

    auto new_height = std::ceil((params.end - params.start) / params.step) + 1;
    auto obj_bb = m_model.objects[0]->bounding_box_exact();
    if (new_height < obj_bb.size().z()) {
        cut_horizontal(0, 0, new_height, ModelObjectCutAttribute::KeepLower);
    }

    _calib_pa_select_added_objects();
}

void CalibrationFlow::_calib_pa_select_added_objects() {
    // update printable state for new volumes on canvas3D
    m_layout->view3D->get_canvas3d()->update_instance_printable_state_for_objects({0});

    Selection& selection = m_layout->view3D->get_canvas3d()->get_selection();
    selection.clear();
    selection.add_object(0, false);

    // BBS: update object list selection
    m_layout->sidebar->obj_list()->update_selections();
    selection.notify_instance_update(-1, -1);
    if (m_layout->view3D->get_canvas3d()->get_gizmos_manager().is_enabled()) {
        // this is required because the selected object changed and the flatten on face an sla support gizmos need to be updated accordingly
        m_layout->view3D->get_canvas3d()->update_gizmos_on_off_state();
    }
}

// Adjust settings for flowrate calibration
// For linear mode, pass 1 means normal version while pass 2 mean "for perfectionists" version
// （实现见文件顶部匿名命名空间：adjust_settings_for_flowrate_calib）

void CalibrationFlow::calib_flowrate(bool is_linear, int pass) {
    if (pass != 1 && pass != 2)
        return;
    wxString calib_name;
    if (is_linear) {
        calib_name = L"Orca YOLO Flow Calibration";
        if (pass == 2)
            calib_name += L" - Perfectionist version";
    } else
        calib_name = wxString::Format(L"Flowrate Test - Pass%d", pass);

    if (m_session.new_project(false, false, calib_name) == wxID_CANCEL)
        return;

    m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));

    if (is_linear) {
        if (pass == 1)
            m_session.add_model(false,
                      (boost::filesystem::path(Slic3r::resources_dir()) / "calib" / "filament_flow" / "Orca-LinearFlow.3mf").string());
        else
            m_session.add_model(false,
                      (boost::filesystem::path(Slic3r::resources_dir()) / "calib" / "filament_flow" / "Orca-LinearFlow_fine.3mf").string());
    } else {
        if (pass == 1)
            m_session.add_model(false,
                      (boost::filesystem::path(Slic3r::resources_dir()) / "calib" / "filament_flow" / "flowrate-test-pass1.3mf").string());
        else
            m_session.add_model(false,
                      (boost::filesystem::path(Slic3r::resources_dir()) / "calib" / "filament_flow" / "flowrate-test-pass2.3mf").string());
    }

    adjust_settings_for_flowrate_calib(is_linear, pass);
    m_q->get_tab(int(Preset::TYPE_PRINTER))->reload_config();
    auto printer_config = &m_q->preset_bundle()->printers.get_edited_preset().config;
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});

    // Refresh object after scaling
    const std::vector<size_t> object_idx(boost::counting_iterator<size_t>(0), boost::counting_iterator<size_t>(m_model.objects.size()));
    m_refresh->changed_objects(object_idx);
}


void CalibrationFlow::calib_temp(const Calib_Params& params) {
    const auto calib_temp_name = wxString::Format(L"Nozzle temperature test");
    m_session.new_project(false, false, calib_temp_name);
    m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Temp_Tower)
        return;

    m_session.add_model(false, Slic3r::resources_dir() + "/calib/temperature_tower/temperature_tower.stl");
    auto printer_config = &m_q->preset_bundle()->printers.get_edited_preset().config;
    auto filament_config = &m_q->preset_bundle()->filaments.get_edited_preset().config;
    auto start_temp = lround(params.start);
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    filament_config->set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts(1,(int)start_temp));
    filament_config->set_key_value("nozzle_temperature", new ConfigOptionInts(1,(int)start_temp));
    m_model.objects[0]->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    m_model.objects[0]->config.set_key_value("brim_width", new ConfigOptionFloat(5.0));
    m_model.objects[0]->config.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));
    m_model.objects[0]->config.set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    m_model.objects[0]->config.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));

    m_refresh->changed_objects({ 0 });
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINT))->reload_config();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->reload_config();

    // cut upper
    auto obj_bb = m_model.objects[0]->bounding_box_exact();
    auto block_count = lround((350 - params.end) / 5 + 1);
    if(block_count > 0){
        // add EPSILON offset to avoid cutting at the exact location where the flat surface is
        auto new_height = block_count * 10.0 + EPSILON;
        if (new_height < obj_bb.size().z()) {
            cut_horizontal(0, 0, new_height, ModelObjectCutAttribute::KeepLower);
        }
    }

    // cut bottom
    obj_bb = m_model.objects[0]->bounding_box_exact();
    block_count = lround((350 - params.start) / 5);
    if(block_count > 0){
        auto new_height = block_count * 10.0 + EPSILON;
        if (new_height < obj_bb.size().z()) {
            cut_horizontal(0, 0, new_height, ModelObjectCutAttribute::KeepUpper);
        }
    }

    m_slicing.background_process().fff_print()->set_calib_params(params);
}

void CalibrationFlow::calib_max_vol_speed(const Calib_Params& params)
{
    const auto calib_vol_speed_name = wxString::Format(L"Max volumetric speed test");
    m_session.new_project(false, false, calib_vol_speed_name);
    m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Vol_speed_Tower)
        return;

    m_session.add_model(false, Slic3r::resources_dir() + "/calib/volumetric_speed/SpeedTestStructure.step");

    auto print_config = &m_q->preset_bundle()->prints.get_edited_preset().config;
    auto filament_config = &m_q->preset_bundle()->filaments.get_edited_preset().config;
    auto printer_config = &m_q->preset_bundle()->printers.get_edited_preset().config;
    auto obj = m_model.objects[0];
    auto& obj_cfg = obj->config;

    auto bed_shape = printer_config->option<ConfigOptionPoints>("printable_area")->values;
    BoundingBoxf bed_ext = get_extents(bed_shape);
    auto scale_obj = (bed_ext.size().x() - 10) / obj->bounding_box_exact().size().x();
    if (scale_obj < 1.0)
        obj->scale(scale_obj, 1, 1);

    const ConfigOptionFloats* nozzle_diameter_config = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->values.size() > 0);
    double nozzle_diameter = nozzle_diameter_config->values[0];
    double line_width = nozzle_diameter * 1.75;
    double layer_height = nozzle_diameter * 0.8;

    auto max_lh = printer_config->option<ConfigOptionFloats>("max_layer_height");
    if (max_lh->values[0] < layer_height)
        max_lh->values[0] = { layer_height };

    size_t max_speed_variants = 1;
    if (const auto *flow_support = filament_config->option<ConfigOptionStrings>("filament_flow_support");
        flow_support != nullptr && !flow_support->values.empty())
        max_speed_variants = flow_support->values.size();
    filament_config->set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats(max_speed_variants, 200.));
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats{0.0});
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    obj_cfg.set_key_value("enable_overhang_speed", new ConfigOptionBools { false });
    obj_cfg.set_key_value("wall_loops", new ConfigOptionInt(1));
    obj_cfg.set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    obj_cfg.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    obj_cfg.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
    obj_cfg.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    obj_cfg.set_key_value("overhang_reverse", new ConfigOptionBool(false));
    obj_cfg.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(line_width, false));
    obj_cfg.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    obj_cfg.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterAndInner));
    obj_cfg.set_key_value("brim_width", new ConfigOptionFloat(5.0));
    obj_cfg.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));
    print_config->set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    print_config->set_key_value("spiral_mode", new ConfigOptionBool(true));
    print_config->set_key_value("max_volumetric_extrusion_rate_slope", new ConfigOptionFloats { 0. });

    m_refresh->changed_objects({ 0 });
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINTER))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINT))->reload_config();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->reload_config();
    m_q->get_tab(int(Preset::TYPE_PRINTER))->reload_config();

    //  cut upper
    auto obj_bb = obj->bounding_box_exact();
    auto height = (params.end - params.start + 1) / params.step;
    if (height < obj_bb.size().z()) {
        cut_horizontal(0, 0, height, ModelObjectCutAttribute::KeepLower);
    }

    auto new_params = params;
    auto mm3_per_mm = Flow(line_width, layer_height, nozzle_diameter).mm3_per_mm() *
                      filament_config->option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0);
    new_params.end = params.end / mm3_per_mm;
    new_params.start = params.start / mm3_per_mm;
    new_params.step = params.step / mm3_per_mm;


    m_slicing.background_process().fff_print()->set_calib_params(new_params);
}

void CalibrationFlow::calib_retraction(const Calib_Params& params)
{
    const auto calib_retraction_name = wxString::Format(L"Retraction test");
    m_session.new_project(false, false, calib_retraction_name);
    m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Retraction_tower)
        return;

    m_session.add_model(false, Slic3r::resources_dir() + "/calib/retraction/retraction_tower.stl");

    auto print_config = &m_q->preset_bundle()->prints.get_edited_preset().config;
    auto filament_config = &m_q->preset_bundle()->filaments.get_edited_preset().config;
    auto printer_config = &m_q->preset_bundle()->printers.get_edited_preset().config;
    auto obj = m_model.objects[0];

    double layer_height = 0.2;

    auto max_lh = printer_config->option<ConfigOptionFloats>("max_layer_height");
    if (max_lh->values[0] < layer_height)
        max_lh->values[0] = { layer_height };

    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    printer_config->set_key_value("use_firmware_retraction", new ConfigOptionBool(false));
    obj->config.set_key_value("wall_loops", new ConfigOptionInt(2));
    obj->config.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    obj->config.set_key_value("bottom_shell_layers", new ConfigOptionInt(3));
    obj->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print_config->set_key_value("initial_layer_print_height", new ConfigOptionFloat(layer_height));
    obj->config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    obj->config.set_key_value("alternate_extra_wall", new ConfigOptionBool(false));

    m_refresh->changed_objects({ 0 });

    //  cut upper
    auto obj_bb = obj->bounding_box_exact();
    auto height = 1.0 + 0.4 + ((params.end - params.start)) / params.step;
    if (height < obj_bb.size().z()) {
        cut_horizontal(0, 0, height, ModelObjectCutAttribute::KeepLower);
    }

    m_slicing.background_process().fff_print()->set_calib_params(params);
}

void CalibrationFlow::calib_VFA(const Calib_Params& params)
{
    const auto calib_vfa_name = wxString::Format(L"VFA test");
    m_session.new_project(false, false, calib_vfa_name);
    m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_VFA_Tower)
        return;

    m_session.add_model(false, Slic3r::resources_dir() + "/calib/vfa/VFA.stl");
    auto print_config = &m_q->preset_bundle()->prints.get_edited_preset().config;
    auto filament_config = &m_q->preset_bundle()->filaments.get_edited_preset().config;
    auto printer_config  = &m_q->preset_bundle()->printers.get_edited_preset().config;
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats { 0.0 });
    print_config->set_key_value("enable_overhang_speed", new ConfigOptionBools { false });
    print_config->set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    print_config->set_key_value("wall_loops", new ConfigOptionInt(1));
    print_config->set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    print_config->set_key_value("top_shell_layers", new ConfigOptionInt(0));
    print_config->set_key_value("bottom_shell_layers", new ConfigOptionInt(1));
    print_config->set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print_config->set_key_value("overhang_reverse", new ConfigOptionBool(false));
    print_config->set_key_value("detect_thin_wall", new ConfigOptionBool(false));
    print_config->set_key_value("spiral_mode", new ConfigOptionBool(true));
    m_model.objects[0]->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    m_model.objects[0]->config.set_key_value("brim_width", new ConfigOptionFloat(3.0));
    m_model.objects[0]->config.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));

    m_refresh->changed_objects({ 0 });
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_ui_from_settings();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_ui_from_settings();

    // cut upper
    auto obj_bb = m_model.objects[0]->bounding_box_exact();
    auto height = 5 * ((params.end - params.start) / params.step + 1);
    if (height < obj_bb.size().z()) {
        cut_horizontal(0, 0, height, ModelObjectCutAttribute::KeepLower);
    }

    m_slicing.background_process().fff_print()->set_calib_params(params);
}

void CalibrationFlow::calib_input_shaping_freq(const Calib_Params& params)
{
    const auto calib_input_shaping_name = wxString::Format(L"Input shaping Frequency test");
    m_session.new_project(false, false, calib_input_shaping_name);
    m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Input_shaping_freq)
        return;

    m_session.add_model(false, Slic3r::resources_dir() + (params.test_model < 1 ? "/calib/input_shaping/ringing_tower.stl" : "/calib/input_shaping/fast_tower_test.stl"));
    auto print_config = &m_q->preset_bundle()->prints.get_edited_preset().config;
    auto filament_config = &m_q->preset_bundle()->filaments.get_edited_preset().config;
    auto printer_config  = &m_q->preset_bundle()->printers.get_edited_preset().config;
    printer_config->set_key_value("machine_max_junction_deviation", new ConfigOptionFloats {0.3});
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_min_speed", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_for_layer_cooling", new ConfigOptionBools{false});
    filament_config->set_key_value("enable_pressure_advance", new ConfigOptionBools {true});
    filament_config->set_key_value("pressure_advance", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("adaptive_pressure_advance", new ConfigOptionBools{false});
    print_config->set_key_value("layer_height", new ConfigOptionFloat(0.2));
    print_config->set_key_value("enable_overhang_speed", new ConfigOptionBools { false });
    print_config->set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    print_config->set_key_value("wall_loops", new ConfigOptionInt(1));
    print_config->set_key_value("top_shell_layers", new ConfigOptionInt(0));
    print_config->set_key_value("bottom_shell_layers", new ConfigOptionInt(1));
    print_config->set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print_config->set_key_value("detect_thin_wall", new ConfigOptionBool(false));
    print_config->set_key_value("spiral_mode", new ConfigOptionBool(true));
    print_config->set_key_value("spiral_mode_smooth", new ConfigOptionBool(false));
    print_config->set_key_value("bottom_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    print_config->set_key_value("outer_wall_speed", new ConfigOptionFloats { 200. });
    print_config->set_key_value("default_acceleration", new ConfigOptionFloats { 2000. });
    print_config->set_key_value("outer_wall_acceleration", new ConfigOptionFloats { 2000. });
    print_config->set_key_value("default_junction_deviation", new ConfigOptionFloats { 0.25 });
    m_model.objects[0]->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    m_model.objects[0]->config.set_key_value("brim_width", new ConfigOptionFloat(3.0));
    m_model.objects[0]->config.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));

    m_refresh->changed_objects({ 0 });
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_ui_from_settings();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_ui_from_settings();

    m_slicing.background_process().fff_print()->set_calib_params(params);
}

void CalibrationFlow::calib_input_shaping_damp(const Calib_Params& params)
{
    const auto calib_input_shaping_name = wxString::Format(L"Input shaping Damping test");
    m_session.new_project(false, false, calib_input_shaping_name);
    m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Input_shaping_damp)
        return;

    m_session.add_model(false, Slic3r::resources_dir() + (params.test_model < 1 ? "/calib/input_shaping/ringing_tower.stl" : "/calib/input_shaping/fast_tower_test.stl"));
    auto print_config = &m_q->preset_bundle()->prints.get_edited_preset().config;
    auto filament_config = &m_q->preset_bundle()->filaments.get_edited_preset().config;
    auto printer_config  = &m_q->preset_bundle()->printers.get_edited_preset().config;
    printer_config->set_key_value("machine_max_junction_deviation", new ConfigOptionFloats{0.3});
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_min_speed", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_for_layer_cooling", new ConfigOptionBools{false});
    filament_config->set_key_value("enable_pressure_advance", new ConfigOptionBools {true});
    filament_config->set_key_value("pressure_advance", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("adaptive_pressure_advance", new ConfigOptionBools{false});
    print_config->set_key_value("layer_height", new ConfigOptionFloat(0.2));
    print_config->set_key_value("enable_overhang_speed", new ConfigOptionBools{false});
    print_config->set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    print_config->set_key_value("wall_loops", new ConfigOptionInt(1));
    print_config->set_key_value("top_shell_layers", new ConfigOptionInt(0));
    print_config->set_key_value("bottom_shell_layers", new ConfigOptionInt(1));
    print_config->set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print_config->set_key_value("detect_thin_wall", new ConfigOptionBool(false));
    print_config->set_key_value("spiral_mode", new ConfigOptionBool(true));
    print_config->set_key_value("spiral_mode_smooth", new ConfigOptionBool(false));
    print_config->set_key_value("bottom_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    print_config->set_key_value("outer_wall_speed", new ConfigOptionFloats { 200. });
    print_config->set_key_value("default_acceleration", new ConfigOptionFloats { 2000. });
    print_config->set_key_value("outer_wall_acceleration", new ConfigOptionFloats { 2000. });
    print_config->set_key_value("default_junction_deviation", new ConfigOptionFloats { 0.25 });
    m_model.objects[0]->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    m_model.objects[0]->config.set_key_value("brim_width", new ConfigOptionFloat(3.0));
    m_model.objects[0]->config.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));

    m_refresh->changed_objects({ 0 });
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_ui_from_settings();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_ui_from_settings();

    m_slicing.background_process().fff_print()->set_calib_params(params);
}

void CalibrationFlow::calib_junction_deviation(const Calib_Params& params)
{
    const auto calib_junction_deviation = wxString::Format(L"Junction Deviation test");
    m_session.new_project(false, false, calib_junction_deviation);
    m_main_frame->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Junction_Deviation)
        return;

    m_session.add_model(false, Slic3r::resources_dir() + (params.test_model < 1 ? "/calib/input_shaping/ringing_tower.stl" : "/calib/input_shaping/fast_tower_test.stl"));
    auto print_config = &m_q->preset_bundle()->prints.get_edited_preset().config;
    auto filament_config = &m_q->preset_bundle()->filaments.get_edited_preset().config;
    auto printer_config  = &m_q->preset_bundle()->printers.get_edited_preset().config;
    printer_config->set_key_value("machine_max_junction_deviation", new ConfigOptionFloats{1.0});
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_min_speed", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_for_layer_cooling", new ConfigOptionBools{false});
    size_t max_speed_variants = 1;
    if (const auto *flow_support = filament_config->option<ConfigOptionStrings>("filament_flow_support");
        flow_support != nullptr && !flow_support->values.empty())
        max_speed_variants = flow_support->values.size();
    filament_config->set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats(max_speed_variants, 200.));
    filament_config->set_key_value("enable_pressure_advance", new ConfigOptionBools {true});
    filament_config->set_key_value("pressure_advance", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("adaptive_pressure_advance", new ConfigOptionBools{false});
    print_config->set_key_value("layer_height", new ConfigOptionFloat(0.2));
    print_config->set_key_value("enable_overhang_speed", new ConfigOptionBools{false});
    print_config->set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    print_config->set_key_value("wall_loops", new ConfigOptionInt(1));
    print_config->set_key_value("top_shell_layers", new ConfigOptionInt(0));
    print_config->set_key_value("bottom_shell_layers", new ConfigOptionInt(1));
    print_config->set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print_config->set_key_value("detect_thin_wall", new ConfigOptionBool(false));
    print_config->set_key_value("spiral_mode", new ConfigOptionBool(true));
    print_config->set_key_value("spiral_mode_smooth", new ConfigOptionBool(false));
    print_config->set_key_value("bottom_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    print_config->set_key_value("outer_wall_speed", new ConfigOptionFloats { 200. });
    print_config->set_key_value("default_acceleration", new ConfigOptionFloats { 2000. });
    print_config->set_key_value("outer_wall_acceleration", new ConfigOptionFloats { 2000. });
    print_config->set_key_value("default_junction_deviation", new ConfigOptionFloats { 0.0 });
    m_model.objects[0]->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    m_model.objects[0]->config.set_key_value("brim_width", new ConfigOptionFloat(3.0));
    m_model.objects[0]->config.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));

    m_refresh->changed_objects({ 0 });
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_dirty();
    m_q->get_tab(int(Preset::TYPE_PRINT))->update_ui_from_settings();
    m_q->get_tab(int(Preset::TYPE_FILAMENT))->update_ui_from_settings();

    m_slicing.background_process().fff_print()->set_calib_params(params);
}

// —— priv 归宿 ——

void CalibrationFlow::record_start_print_preset(std::string action) {
    // record start print preset
    try {
        json j;
        j["user_mode"] = m_q->get_mode_str();
        int  plate_count = m_partplate_list.get_plate_count();
        j["plate_count"] = plate_count;
        unsigned int obj_count = m_model.objects.size();
        j["obj_count"] = obj_count;
        auto printer_preset = m_q->preset_bundle()->printers.get_edited_preset_with_vendor_profile().preset;
        if (printer_preset.is_system) {
            j["printer_preset_name"] = printer_preset.name;
        }
        else {
            j["printer_preset_name"] = printer_preset.config.opt_string("inherits");
        }
        auto filament_presets = m_q->preset_bundle()->filament_presets;
        for (int i = 0; i < filament_presets.size(); ++i) {
            auto filament_preset = m_q->preset_bundle()->filaments.find_preset(filament_presets[i]);
            if (filament_preset->is_system) {
                j["filament_preset_" + std::to_string(i)] = filament_preset->name;
            }
            else {
                j["filament_preset_" + std::to_string(i)] = filament_preset->config.opt_string("inherits");
            }
        }

        Preset& print_preset = m_q->preset_bundle()->prints.get_edited_preset();
        if (print_preset.is_system) {
            j["process_preset"] = print_preset.name;
        }
        else {
            j["process_preset"] = print_preset.config.opt_string("inherits");
        }

        j["record_event"] = action;
        NetworkAgent* agent = m_q->get_agent();
    }
    catch (...) {
        return;
    }

}

} // namespace app
} // namespace GUI
} // namespace Slic3r
