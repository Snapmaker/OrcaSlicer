#ifndef ORIENTJOB_HPP
#define ORIENTJOB_HPP

#include "Job.hpp"
#include "libslic3r/Orient.hpp"
#include "slic3r/GUI/app/ArrangeRequest.hpp"   // F3：Request 显式传递（纯值头，零 include 纯净）

namespace Slic3r {

class ModelObject;

namespace GUI {

class Plater;

class OrientJob : public Job
{
    using OrientMesh = orientation::OrientMesh;
    using OrientMeshs = orientation::OrientMeshs;

    OrientMeshs m_selected, m_unselected, m_unprintable;
    Plater     *m_plater;
    app::OrientRequest m_request;   // F3：prepare 分支改读自持 Request（commit ② 落地读点）

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();

    //BBS: add only one plate mode
    void prepare_selection(std::vector<bool> obj_sel, bool only_one_plate);
    
    // Prepare the selected and unselected items separately. If nothing is
    // selected, behaves as if everything would be selected.
    void prepare_selected();

    //BBS:prepare the items from current selected partplate
    void prepare_partplate();

public:
    void prepare();
    
    void process(Ctl &ctl) override;

    OrientJob(app::OrientRequest request);   // F3：prepare 前置语义随调用传入（原 Plater 旧隐式写点）
    
    void finalize(bool canceled, std::exception_ptr &e) override;
#if 0
    static
    orientation::OrientMesh get_orient_mesh(ModelObject* obj, const Plater* plater)
    {
        using OrientMesh = orientation::OrientMesh;
        OrientMesh om;
        om.name = obj->name;
        om.mesh = obj->mesh(); // don't know the difference to obj->raw_mesh(). Both seem OK
        om.setter = [obj, plater](const OrientMesh& p) {
            obj->rotate(p.angle, p.axis);
            obj->ensure_on_bed();
        };
        return om;
    }
#endif
    static orientation::OrientMesh get_orient_mesh(ModelInstance* instance);
};


}} // namespace Slic3r::GUI

#endif // ORIENTJOB_HPP
