#ifndef FILLBEDJOB_HPP
#define FILLBEDJOB_HPP

#include "ArrangeJob.hpp"

namespace Slic3r { namespace GUI {

class Plater;

class FillBedJob : public Job
{
    int     m_object_idx = -1;

    using ArrangePolygon  = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    ArrangePolygons m_selected;
    ArrangePolygons m_unselected;
    //BBS: add partplate related logic
    ArrangePolygons m_locked;;

    Points m_bedpts;

    arrangement::ArrangeParams params;

    int m_status_range = 0;
    Plater *m_plater;

public:

    void prepare();
    void process(Ctl &ctl) override;

    FillBedJob();

    int status_range() const
    {
        return m_status_range;
    }

    void finalize(bool canceled, std::exception_ptr &e) override;
};

// Fills new plates with copies of the selected object, one plate per trade-off between how
// many copies fit and how much room each of them gets, so the options can be compared side
// by side and the preferred plate printed. The source object is left where it is.
class FillBedOptionsJob : public Job
{
    using ArrangePolygon  = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    // Past this many copies per plate the packer crawls; such a plate is named "N+".
    static constexpr int MAX_COPIES_PER_OPTION = 100;

    // The closest two copies are ever packed, when support room is given up for count.
    static constexpr double TIGHT_GAP_MM = 2.;

    struct Variant
    {
        std::string     label;          // translated, starts the plate name
        bool            rotations;      // let the packer turn the copies, half turns included
        bool            support_room;   // keep the arranger's brim / support ring around each copy
        double          gap_mm;         // extra room around each copy, on top of the above
        double          edge_mm;        // extra room along the plate edge
        ArrangePolygons placed = {};    // copies that fit, in first-plate coordinates
        bool            capped = false; // hit MAX_COPIES_PER_OPTION
    };

    Plater                    *m_plater;
    int                        m_object_idx   = -1;
    int                        m_instance_idx = 0;
    ArrangePolygon             m_template;
    ArrangePolygons            m_fixed;     // excluded regions and the prime tower
    arrangement::ArrangeParams m_params;
    std::optional<Vec2d>       m_tower_pos; // plate-local, reused on every new plate
    std::vector<Variant>       m_variants;

    void prepare();

public:
    FillBedOptionsJob();

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &e) override;
};

}} // namespace Slic3r::GUI

#endif // FILLBEDJOB_HPP
