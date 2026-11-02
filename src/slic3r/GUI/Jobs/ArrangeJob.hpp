#ifndef ARRANGEJOB_HPP
#define ARRANGEJOB_HPP


#include <optional>

#include "Job.hpp"
#include "libslic3r/Arrange.hpp"
#include "slic3r/GUI/app/ArrangeRequest.hpp"   // F3：Request 显式传递（纯值头，零 include 纯净）

namespace Slic3r {

class ModelInstance;

namespace GUI {

class Plater;

class ArrangeJob : public Job
{
    using ArrangePolygon = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    //BBS: add locked logic
    ArrangePolygons m_selected, m_unselected, m_unprintable, m_locked;
    std::vector<ModelInstance*> m_unarranged;
    std::map<int, ArrangePolygons> m_selected_groups;   // groups of selected items for sequential printing
    std::vector<int> m_uncompatible_plates;  // plate indices with different printing sequence than global

    arrangement::ArrangeParams params;
    int current_plate_index = 0;
    Polygon bed_poly;
    Plater *m_plater;
    app::ArrangeRequest m_request;   // F3：prepare 分支改读自持 Request（commit ② 落地读点）

    // BBS: add flag for whether on current part plate
    bool only_on_partplate{false};

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();

    // Prepare the selected and unselected items separately. If nothing is
    // selected, behaves as if everything would be selected.
    void prepare_selected();

    void prepare_all();

    //BBS:prepare the items from current selected partplate
    void prepare_partplate();
    void prepare_wipe_tower();

    ArrangePolygon prepare_arrange_polygon(void* instance);

protected:

    void check_unprintable();

public:

    void prepare();

    void process(Ctl &ctl) override;

    ArrangeJob(app::ArrangeRequest request);   // F3：prepare 前置语义随调用传入（原 Plater 旧隐式写点）

    int status_range() const
    {
        // ensure finalize() is called after all operations in process() is finished.
        return int(m_selected.size() + m_unprintable.size() + 1);
    }

    void finalize(bool canceled, std::exception_ptr &e) override;
};

std::optional<arrangement::ArrangePolygon> get_wipe_tower_arrangepoly(const Plater &);

// The gap between logical beds in the x axis expressed in ratio of
// the current bed width.
static const constexpr double LOGICAL_BED_GAP = 1. / 5.;

//BBS: add sudoku-style strides for x and y
// Stride between logical beds
double bed_stride_x(const Plater* plater);
double bed_stride_y(const Plater* plater);

arrangement::ArrangeParams init_arrange_params(Plater *p, bool current_plate_scope = false);   // F3 D4：盘级 seq 覆盖的状态读取改形参（唯一显式传参点 ArrangeJob prepare）

}} // namespace Slic3r::GUI

#endif // ARRANGEJOB_HPP
