#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "EdgeGrid.hpp"
#include "Layer.hpp"
#include "PaintDepth.hpp"
#include "Print.hpp"
#include "Geometry/VoronoiVisualUtils.hpp"
#include "Geometry/VoronoiUtils.hpp"
#include "MutablePolygon.hpp"
#include "format.hpp"

#include <algorithm>
#include <utility>
#include <unordered_set>

#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>
#include <mutex>
#include <boost/thread/lock_guard.hpp>

//#define MM_SEGMENTATION_DEBUG_GRAPH
//#define MM_SEGMENTATION_DEBUG_REGIONS
//#define MM_SEGMENTATION_DEBUG_INPUT
//#define MM_SEGMENTATION_DEBUG_PAINTED_LINES
//#define MM_SEGMENTATION_DEBUG_COLORIZED_POLYGONS

#if defined(MM_SEGMENTATION_DEBUG_GRAPH) || defined(MM_SEGMENTATION_DEBUG_REGIONS) || \
    defined(MM_SEGMENTATION_DEBUG_INPUT) || defined(MM_SEGMENTATION_DEBUG_PAINTED_LINES) || \
    defined(MM_SEGMENTATION_DEBUG_COLORIZED_POLYGONS)
#define MM_SEGMENTATION_DEBUG
#endif

//#define MM_SEGMENTATION_DEBUG_TOP_BOTTOM

namespace Slic3r {
using boost::polygon::voronoi_diagram;

static inline Point mk_point(const Voronoi::VD::vertex_type *point) { return {coord_t(point->x()), coord_t(point->y())}; }

static inline Point mk_point(const Voronoi::Internal::point_type &point) { return {coord_t(point.x()), coord_t(point.y())}; }

static inline Point mk_point(const voronoi_diagram<double>::vertex_type &point) { return {coord_t(point.x()), coord_t(point.y())}; }

static inline Point mk_point(const Vec2d &point) { return {coord_t(std::round(point.x())), coord_t(std::round(point.y()))}; }

static inline Vec2d mk_vec2(const voronoi_diagram<double>::vertex_type *point) { return {point->x(), point->y()}; }

static bool vertex_equal_to_point(const Voronoi::VD::vertex_type &vertex, const Vec2d &ipt)
{
    // Convert ipt to doubles, force the 80bit FPU temporary to 64bit and then compare.
    // This should work with any settings of math compiler switches and the C++ compiler
    // shall understand the memcpies as type punning and it shall optimize them out.
    using ulp_cmp_type = boost::polygon::detail::ulp_comparison<double>;
    ulp_cmp_type         ulp_cmp;
    static constexpr int ULPS = boost::polygon::voronoi_diagram_traits<double>::vertex_equality_predicate_type::ULPS;
    return ulp_cmp(vertex.x(), ipt.x(), ULPS) == ulp_cmp_type::EQUAL && ulp_cmp(vertex.y(), ipt.y(), ULPS) == ulp_cmp_type::EQUAL;
}

static inline bool vertex_equal_to_point(const Voronoi::VD::vertex_type *vertex, const Vec2d &ipt) { return vertex_equal_to_point(*vertex, ipt); }

struct MMU_Graph
{
    enum class ARC_TYPE { BORDER, NON_BORDER };

    struct Arc
    {
        size_t   from_idx;
        size_t   to_idx;
        int      color;
        ARC_TYPE type;

        bool operator==(const Arc &rhs) const { return (from_idx == rhs.from_idx) && (to_idx == rhs.to_idx) && (color == rhs.color) && (type == rhs.type); }
        bool operator!=(const Arc &rhs) const { return !operator==(rhs); }
    };

    struct Node
    {
        Vec2d             point;
        std::list<size_t> arc_idxs;

        void remove_edge(const size_t to_idx, MMU_Graph &graph)
        {
            for (auto arc_it = this->arc_idxs.begin(); arc_it != this->arc_idxs.end(); ++arc_it) {
                MMU_Graph::Arc &arc = graph.arcs[*arc_it];
                if (arc.to_idx == to_idx) {
                    assert(arc.type != ARC_TYPE::BORDER);
                    this->arc_idxs.erase(arc_it);
                    break;
                }
            }
        }
    };

    std::vector<MMU_Graph::Node> nodes;
    std::vector<MMU_Graph::Arc>  arcs;
    size_t                       all_border_points{};

    std::vector<size_t> polygon_idx_offset;
    std::vector<size_t> polygon_sizes;

    void remove_edge(const size_t from_idx, const size_t to_idx)
    {
        nodes[from_idx].remove_edge(to_idx, *this);
        nodes[to_idx].remove_edge(from_idx, *this);
    }

    [[nodiscard]] size_t get_global_index(const size_t poly_idx, const size_t point_idx) const { return polygon_idx_offset[poly_idx] + point_idx; }

    void append_edge(const size_t &from_idx, const size_t &to_idx, int color = -1, ARC_TYPE type = ARC_TYPE::NON_BORDER)
    {
        // Don't append duplicate edges between the same nodes.
        for (const size_t &arc_idx : this->nodes[from_idx].arc_idxs)
            if (arcs[arc_idx].to_idx == to_idx) return;
        for (const size_t &arc_idx : this->nodes[to_idx].arc_idxs)
            if (arcs[arc_idx].to_idx == from_idx) return;

        this->nodes[from_idx].arc_idxs.push_back(this->arcs.size());
        this->arcs.push_back({from_idx, to_idx, color, type});

        // Always insert only one directed arc for the input polygons.
        // Two directed arcs in both directions are inserted if arcs aren't between points of the input polygons.
        if (type == ARC_TYPE::NON_BORDER) {
            this->nodes[to_idx].arc_idxs.push_back(this->arcs.size());
            this->arcs.push_back({to_idx, from_idx, color, type});
        }
    }

    // It assumes that between points of the input polygons is always only one directed arc,
    // with the same direction as lines of the input polygon.
    [[nodiscard]] MMU_Graph::Arc get_border_arc(size_t idx) const
    {
        assert(idx < this->all_border_points);
        return this->arcs[idx];
    }

    [[nodiscard]] size_t nodes_count() const { return this->nodes.size(); }

    void remove_nodes_with_one_arc()
    {
        std::queue<size_t> update_queue;
        for (const MMU_Graph::Node &node : this->nodes) {
            size_t node_idx = &node - &this->nodes.front();
            // Skip nodes that represent points of input polygons.
            if (node.arc_idxs.size() == 1 && node_idx >= this->all_border_points) update_queue.emplace(&node - &this->nodes.front());
        }

        while (!update_queue.empty()) {
            size_t           node_from_idx = update_queue.front();
            MMU_Graph::Node &node_from     = this->nodes[update_queue.front()];
            update_queue.pop();
            if (node_from.arc_idxs.empty()) continue;

            assert(node_from.arc_idxs.size() == 1);
            size_t           node_to_idx = arcs[node_from.arc_idxs.front()].to_idx;
            MMU_Graph::Node &node_to     = this->nodes[node_to_idx];
            this->remove_edge(node_from_idx, node_to_idx);
            if (node_to.arc_idxs.size() == 1 && node_to_idx >= this->all_border_points) update_queue.emplace(node_to_idx);
        }
    }

    void add_contours(const std::vector<std::vector<ColoredLine>> &color_poly)
    {
        this->all_border_points = nodes.size();
        this->polygon_sizes     = std::vector<size_t>(color_poly.size());
        for (size_t polygon_idx = 0; polygon_idx < color_poly.size(); ++polygon_idx) this->polygon_sizes[polygon_idx] = color_poly[polygon_idx].size();
        this->polygon_idx_offset    = std::vector<size_t>(color_poly.size());
        this->polygon_idx_offset[0] = 0;
        for (size_t polygon_idx = 1; polygon_idx < color_poly.size(); ++polygon_idx) {
            this->polygon_idx_offset[polygon_idx] = this->polygon_idx_offset[polygon_idx - 1] + color_poly[polygon_idx - 1].size();
        }

        size_t poly_idx = 0;
        for (const std::vector<ColoredLine> &color_lines : color_poly) {
            size_t line_idx = 0;
            for (const ColoredLine &color_line : color_lines) {
                size_t from_idx = this->get_global_index(poly_idx, line_idx);
                size_t to_idx   = this->get_global_index(poly_idx, (line_idx + 1) % color_lines.size());
                this->append_edge(from_idx, to_idx, color_line.color, ARC_TYPE::BORDER);
                ++line_idx;
            }
            ++poly_idx;
        }
    }

    // Nodes 0..all_border_points are only one with are on countour. Other vertexis are consider as not on coouter. So we check if base on attach index
    inline bool is_vertex_on_contour(const Voronoi::VD::vertex_type *vertex) const
    {
        assert(vertex != nullptr);
        return vertex->color() < this->all_border_points;
    }

    [[nodiscard]] inline bool is_edge_attach_to_contour(const voronoi_diagram<double>::const_edge_iterator &edge_iterator) const
    {
        return this->is_vertex_on_contour(edge_iterator->vertex0()) || this->is_vertex_on_contour(edge_iterator->vertex1());
    }

    [[nodiscard]] inline bool is_edge_connecting_two_contour_vertices(const voronoi_diagram<double>::const_edge_iterator &edge_iterator) const
    {
        return this->is_vertex_on_contour(edge_iterator->vertex0()) && this->is_vertex_on_contour(edge_iterator->vertex1());
    }

    // All Voronoi vertices are post-processes to merge very close vertices to single. Witch eliminates issues with intersection edges.
    // Also, Voronoi vertices outside of the bounding of input polygons are throw away by marking them.
    void append_voronoi_vertices(const Geometry::VoronoiDiagram &vd, const Polygons &color_poly_tmp, BoundingBox bbox)
    {
        bbox.offset(SCALED_EPSILON);

        struct CPoint
        {
            CPoint() = delete;
            CPoint(const Vec2d &point, size_t contour_idx, size_t point_idx) : m_point_double(point), m_point(mk_point(point)), m_point_idx(point_idx), m_contour_idx(contour_idx)
            {}
            CPoint(const Vec2d &point, size_t point_idx) : m_point_double(point), m_point(mk_point(point)), m_point_idx(point_idx), m_contour_idx(0) {}
            const Vec2d m_point_double;
            const Point m_point;
            size_t      m_point_idx;
            size_t      m_contour_idx;

            [[nodiscard]] const Vec2d &point_double() const { return m_point_double; }
            [[nodiscard]] const Point &point() const { return m_point; }
            bool                       operator==(const CPoint &rhs) const
            {
                return this->m_point_double == rhs.m_point_double && this->m_contour_idx == rhs.m_contour_idx && this->m_point_idx == rhs.m_point_idx;
            }
        };
        struct CPointAccessor
        {
            const Point *operator()(const CPoint &pt) const { return &pt.point(); }
        };
        typedef ClosestPointInRadiusLookup<CPoint, CPointAccessor> CPointLookupType;

        CPointLookupType closest_voronoi_point(coord_t(SCALED_EPSILON));
        CPointLookupType closest_contour_point(3 * coord_t(SCALED_EPSILON));
        for (const Polygon &polygon : color_poly_tmp)
            for (const Point &pt : polygon.points) closest_contour_point.insert(CPoint(Vec2d(pt.x(), pt.y()), &polygon - &color_poly_tmp.front(), &pt - &polygon.points.front()));

        for (const voronoi_diagram<double>::vertex_type &vertex : vd.vertices()) {
            vertex.color(-1);
            Vec2d vertex_point_double = Vec2d(vertex.x(), vertex.y());
            Point vertex_point        = mk_point(vertex);

            const Vec2d &first_point_double  = this->nodes[this->get_border_arc(vertex.incident_edge()->cell()->source_index()).from_idx].point;
            const Vec2d &second_point_double = this->nodes[this->get_border_arc(vertex.incident_edge()->twin()->cell()->source_index()).from_idx].point;

            if (vertex_equal_to_point(&vertex, first_point_double)) {
                assert(vertex.color() != vertex.incident_edge()->cell()->source_index());
                assert(vertex.color() != vertex.incident_edge()->twin()->cell()->source_index());
                vertex.color(this->get_border_arc(vertex.incident_edge()->cell()->source_index()).from_idx);
            } else if (vertex_equal_to_point(&vertex, second_point_double)) {
                assert(vertex.color() != vertex.incident_edge()->cell()->source_index());
                assert(vertex.color() != vertex.incident_edge()->twin()->cell()->source_index());
                vertex.color(this->get_border_arc(vertex.incident_edge()->twin()->cell()->source_index()).from_idx);
            } else if (bbox.contains(vertex_point)) {
                if (auto [contour_pt, c_dist_sqr] = closest_contour_point.find(vertex_point); contour_pt != nullptr && c_dist_sqr < Slic3r::sqr(3 * SCALED_EPSILON)) {
                    vertex.color(this->get_global_index(contour_pt->m_contour_idx, contour_pt->m_point_idx));
                } else if (auto [voronoi_pt, v_dist_sqr] = closest_voronoi_point.find(vertex_point); voronoi_pt == nullptr || v_dist_sqr >= Slic3r::sqr(SCALED_EPSILON / 10.0)) {
                    closest_voronoi_point.insert(CPoint(vertex_point_double, this->nodes_count()));
                    vertex.color(this->nodes_count());
                    this->nodes.push_back({vertex_point_double});
                } else {
                    // Boost Voronoi diagram generator sometimes creates two very closed points instead of one point.
                    // For the example points (146872.99999999997, -146872.99999999997) and (146873, -146873), this example also included in Voronoi generator test cases.
                    std::vector<std::pair<const CPoint *, double>> all_closes_c_points = closest_voronoi_point.find_all(vertex_point);
                    int                                            merge_to_point      = -1;
                    for (const std::pair<const CPoint *, double> &c_point : all_closes_c_points)
                        if ((vertex_point_double - c_point.first->point_double()).squaredNorm() <= Slic3r::sqr(EPSILON)) {
                            merge_to_point = int(c_point.first->m_point_idx);
                            break;
                        }

                    if (merge_to_point != -1) {
                        vertex.color(merge_to_point);
                    } else {
                        closest_voronoi_point.insert(CPoint(vertex_point_double, this->nodes_count()));
                        vertex.color(this->nodes_count());
                        this->nodes.push_back({vertex_point_double});
                    }
                }
            }
        }
    }

    void garbage_collect()
    {
        std::vector<int> nodes_map(this->nodes.size(), -1);
        int              nodes_count = 0;
        size_t           arcs_count  = 0;
        for (const MMU_Graph::Node &node : this->nodes)
            if (size_t node_idx = &node - &this->nodes.front(); !node.arc_idxs.empty()) {
                nodes_map[node_idx] = nodes_count++;
                arcs_count += node.arc_idxs.size();
            }

        std::vector<MMU_Graph::Node> new_nodes;
        std::vector<MMU_Graph::Arc>  new_arcs;
        new_nodes.reserve(nodes_count);
        new_arcs.reserve(arcs_count);
        for (const MMU_Graph::Node &node : this->nodes)
            if (size_t node_idx = &node - &this->nodes.front(); nodes_map[node_idx] >= 0) {
                new_nodes.push_back({node.point});
                for (const size_t &arc_idx : node.arc_idxs) {
                    const Arc &arc = this->arcs[arc_idx];
                    new_nodes.back().arc_idxs.emplace_back(new_arcs.size());
                    new_arcs.push_back({size_t(nodes_map[arc.from_idx]), size_t(nodes_map[arc.to_idx]), arc.color, arc.type});
                }
            }

        this->nodes = std::move(new_nodes);
        this->arcs  = std::move(new_arcs);
    }
};

static Polygon colored_points_to_polygon(const std::vector<ColoredLine> &lines)
{
    Polygon out;
    out.points.reserve(lines.size());
    for (const ColoredLine &l : lines) out.points.emplace_back(l.line.a);
    return out;
}

static Polygons colored_points_to_polygon(const std::vector<std::vector<ColoredLine>> &lines)
{
    Polygons out;
    out.reserve(lines.size());
    for (const std::vector<ColoredLine> &l : lines) out.emplace_back(colored_points_to_polygon(l));
    return out;
}

static std::vector<std::vector<const MMU_Graph::Arc *>> get_all_next_arcs(
    const MMU_Graph &graph, std::vector<bool> &used_arcs, const Linef &process_line, const MMU_Graph::Arc &original_arc, const int color)
{
    std::vector<std::vector<const MMU_Graph::Arc *>> all_next_arcs;
    for (const size_t &arc_idx : graph.nodes[original_arc.to_idx].arc_idxs) {
        std::vector<const MMU_Graph::Arc *> next_continue_arc;

        const MMU_Graph::Arc &arc = graph.arcs[arc_idx];
        if (graph.nodes[arc.to_idx].point == process_line.a || used_arcs[arc_idx]) continue;

        if (original_arc.type == MMU_Graph::ARC_TYPE::BORDER && original_arc.color != color) continue;

        if (arc.type == MMU_Graph::ARC_TYPE::BORDER && arc.color != color) continue;

        Vec2d arc_line = graph.nodes[arc.to_idx].point - graph.nodes[arc.from_idx].point;
        next_continue_arc.emplace_back(&arc);
        all_next_arcs.emplace_back(next_continue_arc);
    }
    return all_next_arcs;
}

static std::vector<const MMU_Graph::Arc *> get_next_arc(
    const MMU_Graph &graph, std::vector<bool> &used_arcs, const Linef &process_line, const MMU_Graph::Arc &original_arc, const int color)
{
    std::vector<const MMU_Graph::Arc *> res;

    std::vector<std::vector<const MMU_Graph::Arc *>> all_next_arcs = get_all_next_arcs(graph, used_arcs, process_line, original_arc, color);
    if (all_next_arcs.empty()) {
        res.emplace_back(&original_arc);
        return res;
    }

    std::vector<std::pair<std::vector<const MMU_Graph::Arc *>, double>> sorted_arcs;
    for (auto next_arc : all_next_arcs) {
        if (next_arc.empty()) continue;

        Vec2d process_line_vec_n   = (process_line.a - process_line.b).normalized();
        Vec2d neighbour_line_vec_n = (graph.nodes[next_arc.back()->to_idx].point - graph.nodes[next_arc.back()->from_idx].point).normalized();

        double angle = ::acos(std::clamp(neighbour_line_vec_n.dot(process_line_vec_n), -1.0, 1.0));
        if (Slic3r::cross2(neighbour_line_vec_n, process_line_vec_n) < 0.0) angle = 2.0 * (double) PI - angle;

        sorted_arcs.emplace_back(next_arc, angle);
    }

    std::sort(sorted_arcs.begin(), sorted_arcs.end(),
              [](std::pair<std::vector<const MMU_Graph::Arc *>, double> &l, std::pair<std::vector<const MMU_Graph::Arc *>, double> &r) -> bool { return l.second < r.second; });

    // Try to return left most edge witch is unused
    for (auto &sorted_arc : sorted_arcs) {
        if (size_t arc_idx = sorted_arc.first.back() - &graph.arcs.front(); !used_arcs[arc_idx]) return sorted_arc.first;
    }

    if (sorted_arcs.empty()) {
        res.emplace_back(&original_arc);
        return res;
    }

    return sorted_arcs.front().first;
}

static bool is_profile_self_interaction(Polygon poly)
{
    auto  lines = poly.lines();
    Point intersection;
    for (int i = 0; i < lines.size(); ++i) {
        for (int j = i + 2; j < std::min(lines.size(), lines.size() + i - 1); ++j) {
            if (lines[i].intersection(lines[j], &intersection)) return true;
        }
    }
    return false;
}

static inline Polygon to_polygon(const std::vector<std::pair<size_t, Linef>> &id_to_lines)
{
    std::vector<Linef> lines;
    for (auto id_to_line : id_to_lines) lines.emplace_back(id_to_line.second);

    Polygon poly_out;
    poly_out.points.reserve(lines.size());
    for (const Linef &line : lines) poly_out.points.emplace_back(mk_point(line.a));
    return poly_out;
}

static std::vector<ExPolygons> extract_colored_segments(const MMU_Graph& graph, const size_t num_facets_states)
{
    std::vector<bool> used_arcs(graph.arcs.size(), false);

    auto all_arc_used = [&used_arcs](const MMU_Graph::Node &node) -> bool {
        return std::all_of(node.arc_idxs.cbegin(), node.arc_idxs.cend(), [&used_arcs](const size_t &arc_idx) -> bool { return used_arcs[arc_idx]; });
    };

    std::vector<ExPolygons> expolygons_segments(num_facets_states);
    for (size_t node_idx = 0; node_idx < graph.all_border_points; ++node_idx) {
        const MMU_Graph::Node &node = graph.nodes[node_idx];

        for (const size_t &arc_idx : node.arc_idxs) {
            const MMU_Graph::Arc &arc = graph.arcs[arc_idx];
            if (arc.type == MMU_Graph::ARC_TYPE::NON_BORDER || used_arcs[arc_idx]) continue;
            // Paint colour overflow: drop arcs whose colour is outside the current
            // filament/segmentation range rather than indexing expolygons_segments[].
            // Does not change in-range paint-depth or inspect-paint (Edge #46) paths.
            if (arc.color < 0 || size_t(arc.color) >= num_facets_states) continue;

            Linef process_line(graph.nodes[arc.from_idx].point, graph.nodes[arc.to_idx].point);
            used_arcs[arc_idx] = true;

            std::vector<std::pair<size_t, Linef>> arc_id_to_face_lines;
            arc_id_to_face_lines.emplace_back(std::make_pair(arc_idx, process_line));
            Vec2d start_p = process_line.a;

            Linef                 p_vec = process_line;
            const MMU_Graph::Arc *p_arc = &arc;
            bool                  flag  = false;
            do {
                std::vector<const MMU_Graph::Arc *> nexts = get_next_arc(graph, used_arcs, p_vec, *p_arc, arc.color);
                for (auto next : nexts) {
                    size_t next_arc_idx = next - &graph.arcs.front();
                    if (used_arcs[next_arc_idx]) {
                        flag = true;
                        break;
                    }
                }

                if (flag) break;

                for (auto next : nexts) {
                    size_t next_arc_idx = next - &graph.arcs.front();
                    arc_id_to_face_lines.emplace_back(std::make_pair(next_arc_idx, Linef(graph.nodes[next->from_idx].point, graph.nodes[next->to_idx].point)));
                    used_arcs[next_arc_idx] = true;
                }

                p_vec = Linef(graph.nodes[nexts.back()->from_idx].point, graph.nodes[nexts.back()->to_idx].point);
                p_arc = nexts.back();

            } while (graph.nodes[p_arc->to_idx].point != start_p || !all_arc_used(graph.nodes[p_arc->to_idx]));

            if (Polygon poly = to_polygon(arc_id_to_face_lines); poly.is_counter_clockwise() && poly.is_valid()) {
                expolygons_segments[arc.color].emplace_back(std::move(poly));
            } else {
                while (arc_id_to_face_lines.size() > 1) {
                    auto id_to_line             = arc_id_to_face_lines.back();
                    used_arcs[id_to_line.first] = false;
                    arc_id_to_face_lines.pop_back();
                    Linef add_line(arc_id_to_face_lines.back().second.b, arc_id_to_face_lines.front().second.a);
                    arc_id_to_face_lines.emplace_back(std::make_pair(-1, add_line));
                    Polygon poly = to_polygon(arc_id_to_face_lines);
                    if (!is_profile_self_interaction(poly) && poly.is_counter_clockwise() && poly.is_valid()) {
                        expolygons_segments[arc.color].emplace_back(std::move(poly));
                        break;
                    }
                    arc_id_to_face_lines.pop_back();
                }
            }
        }
    }
    return expolygons_segments;
}

bool is_equal(float left, float right, float eps = 1e-3) {
    return abs(left - right) <= eps;
}

bool is_less(float left, float right, float eps = 1e-3) {
    return left + eps < right;
}

// Assumes that is at most same projected_l length or below than projection_l
static bool project_line_on_line(const Line &projection_l, const Line &projected_l, Line *new_projected)
{
    const Vec2d  v1 = (projection_l.b - projection_l.a).cast<double>();
    const Vec2d  va = (projected_l.a - projection_l.a).cast<double>();
    const Vec2d  vb = (projected_l.b - projection_l.a).cast<double>();
    const double l2 = v1.squaredNorm(); // avoid a sqrt
    if (l2 == 0.0)
        return false;
    double t1 = va.dot(v1) / l2;
    double t2 = vb.dot(v1) / l2;
    t1        = std::clamp(t1, 0., 1.);
    t2        = std::clamp(t2, 0., 1.);
    assert(t1 >= 0.);
    assert(t2 >= 0.);
    assert(t1 <= 1.);
    assert(t2 <= 1.);

    Point p1       = projection_l.a + (t1 * v1).cast<coord_t>();
    Point p2       = projection_l.a + (t2 * v1).cast<coord_t>();
    *new_projected = Line(p1, p2);
    return true;
}

struct PaintedLine
{
    size_t contour_idx;
    size_t line_idx;
    Line   projected_line;
    int    color;
    // Deft: provenance of the facet this line was projected from - a fixed (volume, extruder,
    // facet) index triple, not a thread id or an arrival order. Two colour-painted facets can
    // legitimately project to the same contour_idx/line_idx/position/length (e.g. a colour
    // boundary sitting on a shared mesh edge), and post_process_painted_lines's sort comparator
    // does not look past those four fields - so without a further deterministic key, std::sort
    // leaves such a pair in whichever order they happened to be appended by the parallel_for over
    // facets, i.e. thread-arrival order. These two fields let the comparator finish the ordering
    // instead of leaving it to std::sort's (undefined, for equal keys) placement.
    size_t vol_idx   = 0;
    size_t facet_idx = 0;
};

struct PaintedLineVisitor
{
    PaintedLineVisitor(const EdgeGrid::Grid &grid, std::vector<PaintedLine> &painted_lines, std::mutex &painted_lines_mutex, size_t reserve) : grid(grid), painted_lines(painted_lines), painted_lines_mutex(painted_lines_mutex)
    {
        painted_lines_set.reserve(reserve);
    }

    void reset() { painted_lines_set.clear(); }

    bool operator()(coord_t iy, coord_t ix)
    {
        // Called with a row and column of the grid cell, which is intersected by a line.
        auto         cell_data_range        = grid.cell_data_range(iy, ix);
        const Vec2d  v1                     = line_to_test.vector().cast<double>();
        const double v1_sqr_norm            = v1.squaredNorm();
        const double heuristic_thr_part     = line_to_test.length() + append_threshold;
        for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++it_contour_and_segment) {
            Line        grid_line         = grid.line(*it_contour_and_segment);
            const Vec2d v2                = grid_line.vector().cast<double>();
            double      heuristic_thr_sqr = Slic3r::sqr(heuristic_thr_part + grid_line.length());

            // An inexpensive heuristic to test whether line_to_test and grid_line can be somewhere close enough to each other.
            // This helps filter out cases when the following expensive calculations are useless.
            if ((grid_line.a - line_to_test.a).cast<double>().squaredNorm() > heuristic_thr_sqr ||
                (grid_line.b - line_to_test.a).cast<double>().squaredNorm() > heuristic_thr_sqr ||
                (grid_line.a - line_to_test.b).cast<double>().squaredNorm() > heuristic_thr_sqr ||
                (grid_line.b - line_to_test.b).cast<double>().squaredNorm() > heuristic_thr_sqr)
                continue;

            // When lines have too different length, it is necessary to normalize them
            if (Slic3r::sqr(v1.dot(v2)) > cos_threshold2 * v1_sqr_norm * v2.squaredNorm()) {
                // The two vectors are nearly collinear (their mutual angle is lower than 30 degrees)
                if (painted_lines_set.find(*it_contour_and_segment) == painted_lines_set.end()) {
                    if (grid_line.distance_to_squared(line_to_test.a) < append_threshold2 ||
                        grid_line.distance_to_squared(line_to_test.b) < append_threshold2 ||
                        line_to_test.distance_to_squared(grid_line.a) < append_threshold2 ||
                        line_to_test.distance_to_squared(grid_line.b) < append_threshold2) {
                        Line line_to_test_projected;
                        project_line_on_line(grid_line, line_to_test, &line_to_test_projected);

                        if ((line_to_test_projected.a - grid_line.a).cast<double>().squaredNorm() > (line_to_test_projected.b - grid_line.a).cast<double>().squaredNorm())
                            line_to_test_projected.reverse();

                        painted_lines_set.insert(*it_contour_and_segment);
                        {
                            boost::lock_guard<std::mutex> lock(painted_lines_mutex);
                            painted_lines.push_back({it_contour_and_segment->first, it_contour_and_segment->second, line_to_test_projected, this->color, this->vol_idx, this->facet_idx});
                        }
                    }
                }
            }
        }
        // Continue traversing the grid along the edge.
        return true;
    }

    const EdgeGrid::Grid                                                                 &grid;
    std::vector<PaintedLine>                                                             &painted_lines;
    std::mutex                                                                           &painted_lines_mutex;
    Line                                                                                  line_to_test;
    std::unordered_set<std::pair<size_t, size_t>, boost::hash<std::pair<size_t, size_t>>> painted_lines_set;
    int                                                                                   color             = -1;
    // Deft: which facet this visitor's line came from - see PaintedLine::vol_idx/facet_idx.
    size_t                                                                                vol_idx           = 0;
    size_t                                                                                facet_idx         = 0;

    static inline const double                                                            cos_threshold2    = Slic3r::sqr(cos(M_PI * 30. / 180.));
    static inline const double                                                            append_threshold  = 50 * SCALED_EPSILON;
    static inline const double                                                            append_threshold2 = Slic3r::sqr(append_threshold);
};

BoundingBox get_extents(const std::vector<ColoredLines> &colored_polygons) {
    BoundingBox bbox;
    for (const ColoredLines &colored_lines : colored_polygons) {
        for (const ColoredLine &colored_line : colored_lines) {
            bbox.merge(colored_line.line.a);
            bbox.merge(colored_line.line.b);
        }
    }
    return bbox;
}

// Flatten the vector of vectors into a vector.
static inline ColoredLines to_lines(const std::vector<ColoredLines> &c_lines)
{
    size_t n_lines = 0;
    for (const auto &c_line : c_lines)
        n_lines += c_line.size();
    ColoredLines lines;
    lines.reserve(n_lines);
    for (const auto &c_line : c_lines)
        lines.insert(lines.end(), c_line.begin(), c_line.end());
    return lines;
}

static std::vector<std::pair<size_t, size_t>> get_segments(const ColoredLines &polygon)
{
    std::vector<std::pair<size_t, size_t>> segments;

    size_t segment_end = 0;
    while (segment_end + 1 < polygon.size() && polygon[segment_end].color == polygon[segment_end + 1].color)
        segment_end++;

    if (segment_end == polygon.size() - 1)
        return {std::make_pair(0, polygon.size() - 1)};

    size_t first_different_color = (segment_end + 1) % polygon.size();
    for (size_t line_offset_idx = 0; line_offset_idx < polygon.size(); ++line_offset_idx) {
        size_t start_s = (first_different_color + line_offset_idx) % polygon.size();
        size_t end_s   = start_s;

        while (line_offset_idx + 1 < polygon.size() && polygon[start_s].color == polygon[(first_different_color + line_offset_idx + 1) % polygon.size()].color) {
            end_s = (first_different_color + line_offset_idx + 1) % polygon.size();
            line_offset_idx++;
        }
        segments.emplace_back(start_s, end_s);
    }
    return segments;
}

static std::vector<PaintedLine> filter_painted_lines(const Line &line_to_process, const size_t start_idx, const size_t end_idx, const std::vector<PaintedLine> &painted_lines)
{
    const int                filter_eps_value = scale_(0.1f);
    std::vector<PaintedLine> filtered_lines;
    filtered_lines.emplace_back(painted_lines[start_idx]);
    for (size_t line_idx = start_idx + 1; line_idx <= end_idx; ++line_idx) {
        // line_to_process is already all colored. Skip another possible duplicate coloring.
        if(filtered_lines.back().projected_line.b == line_to_process.b)
            break;

        PaintedLine &prev = filtered_lines.back();
        const PaintedLine &curr = painted_lines[line_idx];

        double prev_length        = prev.projected_line.length();
        double curr_dist_start    = (curr.projected_line.a - prev.projected_line.a).cast<double>().norm();
        double dist_between_lines = curr_dist_start - prev_length;

        if (dist_between_lines >= 0) {
            if (prev.color == curr.color) {
                if (dist_between_lines <= filter_eps_value) {
                    prev.projected_line.b = curr.projected_line.b;
                } else {
                    filtered_lines.emplace_back(curr);
                }
            } else {
                filtered_lines.emplace_back(curr);
            }
        } else {
            double curr_dist_end = (curr.projected_line.b - prev.projected_line.a).cast<double>().norm();
            if (curr_dist_end > prev_length) {
                if (prev.color == curr.color)
                    prev.projected_line.b = curr.projected_line.b;
                else
                    filtered_lines.push_back({curr.contour_idx, curr.line_idx, Line{prev.projected_line.b, curr.projected_line.b}, curr.color, curr.vol_idx, curr.facet_idx});
            }
        }
    }

    if (double dist_to_start = (filtered_lines.front().projected_line.a - line_to_process.a).cast<double>().norm(); dist_to_start <= filter_eps_value)
        filtered_lines.front().projected_line.a = line_to_process.a;

    if (double dist_to_end = (filtered_lines.back().projected_line.b - line_to_process.b).cast<double>().norm(); dist_to_end <= filter_eps_value)
        filtered_lines.back().projected_line.b = line_to_process.b;

    return filtered_lines;
}

static std::vector<std::vector<PaintedLine>> post_process_painted_lines(const std::vector<EdgeGrid::Contour> &contours, std::vector<PaintedLine> &&painted_lines)
{
    if (painted_lines.empty())
        return {};

    // Deft: this comparator used to stop at (contour_idx, line_idx, start distance, length). Two
    // painted lines from different facets can agree on all four - most commonly a colour boundary
    // that sits exactly on a shared mesh edge, which is exactly the case a 3-filament painted
    // model produces - and std::sort leaves such a pair in whatever order they were appended in,
    // which is the parallel_for-over-facets thread-arrival order (see PaintedLine::vol_idx /
    // facet_idx). Falling through to (color, vol_idx, facet_idx) makes this a total order over the
    // facets actually in the model, so the result no longer depends on how the work was scheduled.
    auto comp = [&contours](const PaintedLine &first, const PaintedLine &second) {
        if (first.contour_idx != second.contour_idx)
            return first.contour_idx < second.contour_idx;
        if (first.line_idx != second.line_idx)
            return first.line_idx < second.line_idx;
        Point  first_start_p  = contours[first.contour_idx].segment_start(first.line_idx);
        double first_dist_sqr = (first.projected_line.a - first_start_p).cast<double>().squaredNorm();
        double second_dist_sqr = (second.projected_line.a - first_start_p).cast<double>().squaredNorm();
        if (first_dist_sqr != second_dist_sqr)
            return first_dist_sqr < second_dist_sqr;
        double first_len_sqr  = (first.projected_line.b - first.projected_line.a).cast<double>().squaredNorm();
        double second_len_sqr = (second.projected_line.b - second.projected_line.a).cast<double>().squaredNorm();
        if (first_len_sqr != second_len_sqr)
            return first_len_sqr < second_len_sqr;
        if (first.color != second.color)
            return first.color < second.color;
        if (first.vol_idx != second.vol_idx)
            return first.vol_idx < second.vol_idx;
        return first.facet_idx < second.facet_idx;
    };
    std::sort(painted_lines.begin(), painted_lines.end(), comp);

    std::vector<std::vector<PaintedLine>> filtered_painted_lines(contours.size());
    size_t prev_painted_line_idx = 0;
    for (size_t curr_painted_line_idx = 0; curr_painted_line_idx < painted_lines.size(); ++curr_painted_line_idx) {
        size_t next_painted_line_idx = curr_painted_line_idx + 1;
        if (next_painted_line_idx >= painted_lines.size() || painted_lines[curr_painted_line_idx].contour_idx != painted_lines[next_painted_line_idx].contour_idx || painted_lines[curr_painted_line_idx].line_idx != painted_lines[next_painted_line_idx].line_idx) {
            const PaintedLine &start_line      = painted_lines[prev_painted_line_idx];
            const Line        &line_to_process = contours[start_line.contour_idx].get_segment(start_line.line_idx);
            Slic3r::append(filtered_painted_lines[painted_lines[curr_painted_line_idx].contour_idx], filter_painted_lines(line_to_process, prev_painted_line_idx, curr_painted_line_idx, painted_lines));
            prev_painted_line_idx = next_painted_line_idx;
        }
    }

    return filtered_painted_lines;
}

#ifndef NDEBUG
static bool are_lines_connected(const ColoredLines &colored_lines)
{
    for (size_t line_idx = 1; line_idx < colored_lines.size(); ++line_idx)
        if (colored_lines[line_idx - 1].line.b != colored_lines[line_idx].line.a)
            return false;
    return true;
}
#endif

static ColoredLines colorize_line(const Line &line_to_process,
                                              const size_t              start_idx,
                                              const size_t              end_idx,
                                              const std::vector<PaintedLine> &painted_contour)
{
    assert(start_idx < painted_contour.size() && end_idx < painted_contour.size() && start_idx <= end_idx);
    assert(std::all_of(painted_contour.begin() + start_idx, painted_contour.begin() + end_idx + 1, [&painted_contour, &start_idx](const auto &p_line) { return painted_contour[start_idx].line_idx == p_line.line_idx; }));

    const int          filter_eps_value = scale_(0.1f);
    ColoredLines       final_lines;
    const PaintedLine &first_line = painted_contour[start_idx];
    if (double dist_to_start = (first_line.projected_line.a - line_to_process.a).cast<double>().norm(); dist_to_start > filter_eps_value)
        final_lines.push_back({Line(line_to_process.a, first_line.projected_line.a), 0});
    final_lines.push_back({first_line.projected_line, first_line.color});

    for (size_t line_idx = start_idx + 1; line_idx <= end_idx; ++line_idx) {
        ColoredLine       &prev = final_lines.back();
        const PaintedLine &curr = painted_contour[line_idx];

        double line_dist = (curr.projected_line.a - prev.line.b).cast<double>().norm();
        if (line_dist <= filter_eps_value) {
            if (prev.color == curr.color) {
                prev.line.b = curr.projected_line.b;
            } else {
                prev.line.b = curr.projected_line.a;
                final_lines.push_back({curr.projected_line, curr.color});
            }
        } else {
            final_lines.push_back({Line(prev.line.b, curr.projected_line.a), 0});
            final_lines.push_back({curr.projected_line, curr.color});
        }
    }

    // If there is non-painted space, then inserts line painted by a default color.
    if (double dist_to_end = (final_lines.back().line.b - line_to_process.b).cast<double>().norm(); dist_to_end > filter_eps_value)
        final_lines.push_back({Line(final_lines.back().line.b, line_to_process.b), 0});

    // Make sure all the lines are connected.
    assert(are_lines_connected(final_lines));

    for (size_t line_idx = 2; line_idx < final_lines.size(); ++line_idx) {
        const ColoredLine &line_0 = final_lines[line_idx - 2];
        ColoredLine       &line_1 = final_lines[line_idx - 1];
        const ColoredLine &line_2 = final_lines[line_idx - 0];

        if (line_0.color == line_2.color && line_0.color != line_1.color)
            if (line_1.line.length() <= scale_(0.2)) line_1.color = line_0.color;
    }

    ColoredLines colored_lines_simple;
    colored_lines_simple.emplace_back(final_lines.front());
    for (size_t line_idx = 1; line_idx < final_lines.size(); ++line_idx) {
        const ColoredLine &line_0 = final_lines[line_idx];

        if (colored_lines_simple.back().color == line_0.color)
            colored_lines_simple.back().line.b = line_0.line.b;
        else
            colored_lines_simple.emplace_back(line_0);
    }

    final_lines = colored_lines_simple;

    if (final_lines.size() > 1)
        if (final_lines.front().color != final_lines[1].color && final_lines.front().line.length() <= scale_(0.2)) {
            final_lines[1].line.a = final_lines.front().line.a;
            final_lines.erase(final_lines.begin());
        }

    if (final_lines.size() > 1)
        if (final_lines.back().color != final_lines[final_lines.size() - 2].color && final_lines.back().line.length() <= scale_(0.2)) {
            final_lines[final_lines.size() - 2].line.b = final_lines.back().line.b;
            final_lines.pop_back();
        }

    return final_lines;
}

static ColoredLines filter_colorized_polygon(ColoredLines &&new_lines) {
    for (size_t line_idx = 2; line_idx < new_lines.size(); ++line_idx) {
        const ColoredLine &line_0 = new_lines[line_idx - 2];
        ColoredLine       &line_1 = new_lines[line_idx - 1];
        const ColoredLine &line_2 = new_lines[line_idx - 0];

        if (line_0.color == line_2.color && line_0.color != line_1.color && line_0.color >= 1) {
            if (line_1.line.length() <= scale_(0.5)) line_1.color = line_0.color;
        }
    }

    for (size_t line_idx = 3; line_idx < new_lines.size(); ++line_idx) {
        const ColoredLine &line_0 = new_lines[line_idx - 3];
        ColoredLine       &line_1 = new_lines[line_idx - 2];
        ColoredLine       &line_2 = new_lines[line_idx - 1];
        const ColoredLine &line_3 = new_lines[line_idx - 0];

        if (line_0.color == line_3.color && (line_0.color != line_1.color || line_0.color != line_2.color) && line_0.color >= 1 && line_3.color >= 1) {
            if ((line_1.line.length() + line_2.line.length()) <= scale_(0.5)) {
                line_1.color = line_0.color;
                line_2.color = line_0.color;
            }
        }
    }

    std::vector<std::pair<size_t, size_t>> segments       = get_segments(new_lines);
    auto                                   segment_length = [&new_lines](const std::pair<size_t, size_t> &segment) {
        double total_length = 0;
        for (size_t seg_start_idx = segment.first; seg_start_idx != segment.second; seg_start_idx = (seg_start_idx + 1 < new_lines.size()) ? seg_start_idx + 1 : 0)
            total_length += new_lines[seg_start_idx].line.length();
        total_length += new_lines[segment.second].line.length();
        return total_length;
    };

    if (segments.size() >= 2)
        for (size_t curr_idx = 0; curr_idx < segments.size(); ++curr_idx) {
            size_t next_idx = next_idx_modulo(curr_idx, segments.size());
            assert(curr_idx != next_idx);

            int color0 = new_lines[segments[curr_idx].first].color;
            int color1 = new_lines[segments[next_idx].first].color;

            double seg0l = segment_length(segments[curr_idx]);
            double seg1l = segment_length(segments[next_idx]);

            if (color0 != color1 && seg0l >= scale_(0.1) && seg1l <= scale_(0.2)) {
                for (size_t seg_start_idx = segments[next_idx].first; seg_start_idx != segments[next_idx].second; seg_start_idx = (seg_start_idx + 1 < new_lines.size()) ? seg_start_idx + 1 : 0)
                    new_lines[seg_start_idx].color = color0;
                new_lines[segments[next_idx].second].color = color0;
            }
        }

    segments = get_segments(new_lines);
    if (segments.size() >= 2)
        for (size_t curr_idx = 0; curr_idx < segments.size(); ++curr_idx) {
            size_t next_idx = next_idx_modulo(curr_idx, segments.size());
            assert(curr_idx != next_idx);

            int    color0 = new_lines[segments[curr_idx].first].color;
            int    color1 = new_lines[segments[next_idx].first].color;
            double seg1l  = segment_length(segments[next_idx]);

            if (color0 >= 1 && color0 != color1 && seg1l <= scale_(0.2)) {
                for (size_t seg_start_idx = segments[next_idx].first; seg_start_idx != segments[next_idx].second; seg_start_idx = (seg_start_idx + 1 < new_lines.size()) ? seg_start_idx + 1 : 0)
                    new_lines[seg_start_idx].color = color0;
                new_lines[segments[next_idx].second].color = color0;
            }
        }

    segments = get_segments(new_lines);
    if (segments.size() >= 3)
        for (size_t curr_idx = 0; curr_idx < segments.size(); ++curr_idx) {
            size_t next_idx      = next_idx_modulo(curr_idx, segments.size());
            size_t next_next_idx = next_idx_modulo(next_idx, segments.size());

            int color0 = new_lines[segments[curr_idx].first].color;
            int color1 = new_lines[segments[next_idx].first].color;
            int color2 = new_lines[segments[next_next_idx].first].color;

            if (color0 > 0 && color0 == color2 && color0 != color1 && segment_length(segments[next_idx]) <= scale_(0.5)) {
                for (size_t seg_start_idx = segments[next_next_idx].first; seg_start_idx != segments[next_next_idx].second; seg_start_idx = (seg_start_idx + 1 < new_lines.size()) ? seg_start_idx + 1 : 0)
                    new_lines[seg_start_idx].color = color0;
                new_lines[segments[next_next_idx].second].color = color0;
            }
        }

    return std::move(new_lines);
}

static ColoredLines colorize_contour(const EdgeGrid::Contour &contour, const std::vector<PaintedLine> &painted_contour) {
    assert(painted_contour.empty() || std::all_of(painted_contour.begin(), painted_contour.end(), [&painted_contour](const auto &p_line) { return painted_contour.front().contour_idx == p_line.contour_idx; }));

    ColoredLines colorized_contour;
    if (painted_contour.empty()) {
        // Appends contour with default color for lines before the first PaintedLine.
        colorized_contour.reserve(contour.num_segments());
        for (const Line &line : contour.get_segments())
            colorized_contour.emplace_back(ColoredLine{line, 0});
        return colorized_contour;
    }

    colorized_contour.reserve(contour.num_segments() + painted_contour.size());
    for (size_t idx = 0; idx < painted_contour.front().line_idx; ++idx)
        colorized_contour.emplace_back(ColoredLine{contour.get_segment(idx), 0});

    size_t prev_painted_line_idx = 0;
    for (size_t curr_painted_line_idx = 0; curr_painted_line_idx < painted_contour.size(); ++curr_painted_line_idx) {
        size_t next_painted_line_idx = curr_painted_line_idx + 1;
        if (next_painted_line_idx >= painted_contour.size() || painted_contour[curr_painted_line_idx].line_idx != painted_contour[next_painted_line_idx].line_idx) {
            const std::vector<PaintedLine> &painted_contour_copy = painted_contour;
            Slic3r::append(colorized_contour, colorize_line(contour.get_segment(painted_contour[prev_painted_line_idx].line_idx), prev_painted_line_idx, curr_painted_line_idx, painted_contour_copy));

            // Appends contour with default color for lines between the current and the next PaintedLine.
            if (next_painted_line_idx < painted_contour.size())
                for (size_t idx = painted_contour[curr_painted_line_idx].line_idx + 1; idx < painted_contour[next_painted_line_idx].line_idx; ++idx)
                    colorized_contour.emplace_back(ColoredLine{contour.get_segment(idx), 0});

            prev_painted_line_idx = next_painted_line_idx;
        }
    }

    // Appends contour with default color for lines after the last PaintedLine.
    for (size_t idx = painted_contour.back().line_idx + 1; idx < contour.num_segments(); ++idx)
        colorized_contour.emplace_back(ColoredLine{contour.get_segment(idx), 0});

    assert(!colorized_contour.empty());
    return filter_colorized_polygon(std::move(colorized_contour));
}

static std::vector<ColoredLines> colorize_contours(const std::vector<EdgeGrid::Contour> &contours, const std::vector<std::vector<PaintedLine>> &painted_contours)
{
    assert(contours.size() == painted_contours.size());
    std::vector<ColoredLines> colorized_contours(contours.size());
    for (const std::vector<PaintedLine> &painted_contour : painted_contours) {
        size_t contour_idx              = &painted_contour - &painted_contours.front();
        colorized_contours[contour_idx] = colorize_contour(contours[contour_idx], painted_contours[contour_idx]);
    }

    size_t poly_idx = 0;
    for (ColoredLines &color_lines : colorized_contours) {
        size_t line_idx = 0;
        for (size_t color_line_idx = 0; color_line_idx < color_lines.size(); ++color_line_idx) {
            color_lines[color_line_idx].poly_idx       = int(poly_idx);
            color_lines[color_line_idx].local_line_idx = int(line_idx);
            ++line_idx;
        }
        ++poly_idx;
    }

    return colorized_contours;
}

// Determines if the line points from the point between two contour lines is pointing inside polygon or outside.
static inline bool points_inside(const Line &contour_first, const Line &contour_second, const Point &new_point)
{
    // TODO: Used in points_inside for decision if line leading thought the common point of two lines is pointing inside polygon or outside
    auto three_points_inward_normal = [](const Point &left, const Point &middle, const Point &right) -> Vec2d {
        assert(left != middle);
        assert(middle != right);
        return (perp(Point(middle - left)).cast<double>().normalized() + perp(Point(right - middle)).cast<double>().normalized()).normalized();
    };

    assert(contour_first.b == contour_second.a);
    Vec2d  inward_normal = three_points_inward_normal(contour_first.a, contour_first.b, contour_second.b);
    Vec2d  edge_norm     = (new_point - contour_first.b).cast<double>().normalized();
    double side          = inward_normal.dot(edge_norm);
    //    assert(side != 0.);
    return side > 0.;
}

enum VD_ANNOTATION : Voronoi::VD::cell_type::color_type {
    VERTEX_ON_CONTOUR = 1,
    DELETED           = 2
};

#ifdef MM_SEGMENTATION_DEBUG_GRAPH
static void export_graph_to_svg(const std::string &path, const Voronoi::VD& vd, const std::vector<ColoredLines>& colored_polygons) {
    const coordf_t                 stroke_width = scaled<coordf_t>(0.05f);
    const BoundingBox              bbox         = get_extents(colored_polygons);

    SVG svg(path.c_str(), bbox);
    for (const ColoredLines &colored_lines : colored_polygons)
        for (const ColoredLine &colored_line : colored_lines)
            svg.draw(colored_line.line, "black", stroke_width);

    for (const Voronoi::VD::vertex_type &vertex : vd.vertices()) {
        if (Geometry::VoronoiUtils::is_in_range<coord_t>(vertex)) {
            if (const Point pt = Geometry::VoronoiUtils::to_point(&vertex).cast<coord_t>(); vertex.color() == VD_ANNOTATION::VERTEX_ON_CONTOUR) {
                svg.draw(pt, "blue", coord_t(stroke_width));
            } else if (vertex.color() != VD_ANNOTATION::DELETED) {
                svg.draw(pt, "green", coord_t(stroke_width));
            }
        }
    }

    for (const Voronoi::VD::edge_type &edge : vd.edges()) {
        if (edge.is_infinite() || !Geometry::VoronoiUtils::is_in_range<coord_t>(edge))
            continue;

        const Point from = Geometry::VoronoiUtils::to_point(edge.vertex0()).cast<coord_t>();
        const Point to   = Geometry::VoronoiUtils::to_point(edge.vertex1()).cast<coord_t>();

        if (edge.color() != VD_ANNOTATION::DELETED)
            svg.draw(Line(from, to), "red", stroke_width);
    }
}
#endif // MM_SEGMENTATION_DEBUG_GRAPH

static size_t non_deleted_edge_count(const VD::vertex_type &vertex) {
    size_t               non_deleted_edge_cnt = 0;
    const VD::edge_type *edge                 = vertex.incident_edge();
    do {
        if (edge->color() != VD_ANNOTATION::DELETED)
            ++non_deleted_edge_cnt;
    } while (edge = edge->prev()->twin(), edge != vertex.incident_edge());

    return non_deleted_edge_cnt;
}

static bool can_vertex_be_deleted(const VD::vertex_type &vertex) {
    if (vertex.color() == VD_ANNOTATION::VERTEX_ON_CONTOUR || vertex.color() == VD_ANNOTATION::DELETED)
        return false;

    return non_deleted_edge_count(vertex) <= 1;
}

static void delete_vertex_deep(const VD::vertex_type &vertex) {
    std::queue<const VD::vertex_type *> vertices_to_delete;
    vertices_to_delete.emplace(&vertex);

    while (!vertices_to_delete.empty()) {
        const VD::vertex_type &vertex_to_delete = *vertices_to_delete.front();
        vertices_to_delete.pop();
        vertex_to_delete.color(VD_ANNOTATION::DELETED);

        const VD::edge_type *edge = vertex_to_delete.incident_edge();
        do {
            edge->color(VD_ANNOTATION::DELETED);
            edge->twin()->color(VD_ANNOTATION::DELETED);

            if (edge->is_finite() && can_vertex_be_deleted(*edge->vertex1()))
                vertices_to_delete.emplace(edge->vertex1());
        } while (edge = edge->prev()->twin(), edge != vertex_to_delete.incident_edge());
    }
}

static inline Vec2d mk_point_vec2d(const VD::vertex_type *point) {
    assert(point != nullptr);
    return {point->x(), point->y()};
}

static inline Vec2d mk_vector_vec2d(const VD::edge_type *edge) {
    assert(edge != nullptr);
    return mk_point_vec2d(edge->vertex1()) - mk_point_vec2d(edge->vertex0());
}

static inline Vec2d mk_flipped_vector_vec2d(const VD::edge_type *edge) {
    assert(edge != nullptr);
    return mk_point_vec2d(edge->vertex0()) - mk_point_vec2d(edge->vertex1());
}

static double edge_length(const VD::edge_type &edge) {
    assert(edge.is_finite());
    return mk_vector_vec2d(&edge).norm();
}

// Used in remove_multiple_edges_in_vertices()
// Returns length of edge with is connected to contour. To this length is include other edges with follows it if they are almost straight (with the
// tolerance of 15) And also if node between two subsequent edges is connected only to these two edges.
static inline double calc_total_edge_length(const VD::edge_type &starting_edge)
{
    double               total_edge_length = edge_length(starting_edge);
    const VD::edge_type *prev              = &starting_edge;
    do {
        if (prev->is_finite() && non_deleted_edge_count(*prev->vertex1()) > 2)
            break;

        bool                 found_next_edge = false;
        const VD::edge_type *current         = prev->next();
        do {
            if (current->color() == VD_ANNOTATION::DELETED)
                continue;

            Vec2d  first_line_vec_n  = mk_flipped_vector_vec2d(prev).normalized();
            Vec2d  second_line_vec_n = mk_vector_vec2d(current).normalized();
            double angle             = ::acos(std::clamp(first_line_vec_n.dot(second_line_vec_n), -1.0, 1.0));
            if (Slic3r::cross2(first_line_vec_n, second_line_vec_n) < 0.0)
                angle = 2.0 * (double) PI - angle;

            if (std::abs(angle - PI) >= (PI / 12))
                continue;

            prev               = current;
            found_next_edge    = true;
            total_edge_length += edge_length(*current);

            break;
        } while (current = current->prev()->twin(), current != prev->next());

        if (!found_next_edge)
            break;

    } while (prev != &starting_edge);

    return total_edge_length;
}

// When a Voronoi vertex has more than one Voronoi edge (for example, in concave parts of a polygon),
// we leave just one Voronoi edge in the Voronoi vertex.
// This Voronoi edge is selected based on a heuristic.
static void remove_multiple_edges_in_vertex(const VD::vertex_type &vertex) {
    if (non_deleted_edge_count(vertex) <= 1)
        return;

    std::vector<std::pair<const VD::edge_type *, double>> edges_to_check;
    const VD::edge_type *edge = vertex.incident_edge();
    do {
        if (edge->color() == VD_ANNOTATION::DELETED)
            continue;

        edges_to_check.emplace_back(edge, calc_total_edge_length(*edge));
    } while (edge = edge->prev()->twin(), edge != vertex.incident_edge());

    std::sort(edges_to_check.begin(), edges_to_check.end(), [](const auto &l, const auto &r) -> bool {
        return l.second > r.second;
    });

    while (edges_to_check.size() > 1) {
        const VD::edge_type &edge_to_check = *edges_to_check.back().first;
        edge_to_check.color(VD_ANNOTATION::DELETED);
        edge_to_check.twin()->color(VD_ANNOTATION::DELETED);

        if (const VD::vertex_type &vertex_to_delete = *edge_to_check.vertex1(); can_vertex_be_deleted(vertex_to_delete))
            delete_vertex_deep(vertex_to_delete);

        edges_to_check.pop_back();
    }
}

// Fix-wave F2 (.superpowers/sdd/2026-08-31-paint-depth/outward-bleed-investigation.md section
// 2.3): the "keep base" core that cut_segmented_layers subtracts from every colour's claim.
//
// The clamp is `claim \ core` with `core` = the layer eroded by the band, so a painted colour
// survives only within `band` of the layer contour. Wherever the local half-thickness is LESS
// than the band that erosion comes back EMPTY, `diff_ex(claim, {})` is the claim itself, and the
// clamp is a COMPLETE no-op on that geometry - the painted colour keeps the entire local
// cross-section. It is not a partial weakening; it is an on/off cliff, and nothing anywhere
// reports it. At paint_depth_mm = 4-6mm the condition holds across most of a typical organic
// model (anything under 8-12mm thick locally), which is exactly why, past a certain depth
// setting, increasing it stopped deepening the paint and started revealing the raw Voronoi
// partition instead.
//
// DEGRADATION RULE. Where a full-`band` inset leaves no core, fall back to the widest band from
// the halving ladder band/4, band/8, ... band/128 that the local geometry can support - where
// "can support" means the part survives an opening at TWICE that band. A part of local
// half-thickness t therefore ends up with an effective band b satisfying 2b <= t < 4b, i.e.
//
//     t/4 < b <= t/2
//
//   - `b <= t/2` is the load-bearing half: the base core left behind is t - b >= t/2, so at
//     least half of the local cross-section stays base-coloured. The claim becomes proportionate
//     to the geometry instead of swallowing it whole - which is the whole point, since a band
//     deeper than the feature is a request the geometry cannot honour.
//   - `b > t/4` bounds how much paint the degradation gives away.
// Testing membership at 2b rather than at b is what buys the first bound: at radius b alone a
// part with t barely above b would keep a vanishing base core, i.e. the no-op all over again.
//
// Thick geometry is bit-for-bit untouched: where the full-band erosion is non-empty, `thin` is
// empty, the ladder never runs, and the core is exactly today's `offset_ex(layer, -band)`.
//
// DEGENERATE LIMIT, stated plainly: the ladder is bounded at six steps, so its last membership
// radius is band/64 - i.e. any part whose local HALF-thickness is under band/64 keeps the old
// no-op (0.022mm of half-thickness, 0.045mm of material, at a 1.44mm walls-mode band; 0.094mm at
// a 6mm one). Both are far below a single extrusion, i.e. geometry that cannot carry two colours
// through its thickness under any clamp. Wave A's `min_claim_width` floor below normally stops
// the ladder well before this bound is reached and is now the binding limit.
//
// AND WHAT THIS DOES NOT DO, also plainly: it bounds the claim, it does not stop a Voronoi cell
// from WRAPPING onto an opposite face. The clamp keeps whatever lies within `band` of ANY
// boundary, so where a painted surface's own cell reaches around a rounded fin tip, the far
// face's perimeter band still survives for any b > 0. Suppressing that needs a clamp measured
// from the PAINTED boundary rather than from the layer contour - a different mechanism, out of
// this fix's scope, and recorded as such in the fix-wave report.
//
// WAVE A / C-1 (.superpowers/sdd/2026-08-31-paint-depth/bleed-and-walls-fixwave-review.md):
// THE LADDER IS FLOORED AT ONE PRINTABLE EXTRUSION. As shipped it had no lower bound, and its
// FIRST and widest step is `b = band/4` - so the widest lateral claim the degradation could ever
// produce was a quarter of the band. In the default walls mode that is unconditionally narrower
// than one external extrusion: band/4 = (N*s + 2h(1-pi/4) + 0.25s)/4 only reaches 0.45mm at
// N >= 3.9 walls, so at the shipped default (walls = 3, 0.45mm lines, 0.1mm layers,
// band = 1.4357) EVERY activation of the ladder produced a claim of at most 0.359mm - 0.80 of a
// bead at step 0, 0.40 at step 1, 0.20 at step 2.
//
// What that costs downstream: the claim is a separate PrintRegion whose perimeters are generated
// on the strip alone (Layer.cpp:184, :257-260). At b = 0.25mm Arachne sees
// T = 0.25 - 2h(1-pi/4) = 0.207mm, above min_feature_size (0.1) and below min_bead_width (0.34),
// so WideningBeadingStrategy::getOptimalBeadCount returns one bead and compute() widens it to
// 0.34mm - a 0.34mm bead extruded into a 0.207mm gap, ~64% local over-extrusion, on both faces of
// every thin wall in the model on every painted layer. One step further down (b = 0.0897,
// T = 0.047 < min_feature_size) the strip produces NO TOOLPATH AT ALL while the base region has
// already been cut back by it - a 47um unfilled band. Before F2 those geometries hit the (ugly
// but printable) no-op and got a single correctly-beaded painted region, so this was a new
// regression, in the default mode, on exactly the thin-organic geometry F2 was written for.
//
// `min_claim_width` is the max external perimeter width across the object's regions (plumbed from
// the call site, where it is already computed next to the band). Below it the ladder STOPS and
// the old no-op stands: a part that cannot carry a printable painted skin keeps its whole
// cross-section, as it did before F2, rather than getting an unprintable one. This costs F2
// nothing where it actually helps - in millimetres mode at 4-6mm, band/4 is 1.0-1.5mm, well above
// one bead for steps 0-1 - and removes the entire sub-bead regime.
//
// WAVE A / I-2, corrected by the fix wave (.superpowers/sdd/2026-08-31-paint-depth/
// wave-a-review.md): THE LADDER'S THRESHOLDS COME FROM THE UN-NOTCHED BAND (`ladder_band`), AND
// SO DOES WHETHER A PART ENTERS THE LADDER AT ALL. cut_segmented_layers narrows the band by the
// interlocking notch on even layers. The first fix-wave pass moved the ladder's STEP (b) to
// ladder_band but left `core`/`thin` - the decision of whether a part is thick enough to skip the
// ladder entirely - reading the (possibly notched) `band`. That decision was therefore still
// parity-dependent: a part whose local half-thickness sits between the notched and un-notched
// band was "thick enough" (full band) on the parity with the narrower (notched) threshold and
// "too thin" (ladder-degraded) on the parity with the wider (un-notched) one - an alternation
// between the FULL band and the ladder's first step, an order of magnitude larger than the notch
// itself, on exactly the geometry this fix exists to protect (reachable once
// 0.25*ladder_band >= min_claim_width, i.e. paint_depth_walls >= 4 at stock flows, or in
// millimetres mode once the band is wide enough to arm the ladder at all).
//
// Fix: decide core/thin from `ladder_band` (parity-independent) throughout, so the SET of
// geometry the ladder touches cannot depend on parity either. The ACTUAL erosion depth for
// geometry that is NOT degraded still uses the real (possibly notched) `band` - the interlocking
// tooth's intentional alternation on non-degraded geometry is unchanged; only whether a part
// counts as "non-degraded" in the first place is now parity-independent. At the shipped default
// (walls = 3) the ladder is floored off by min_claim_width before this distinction can ever
// matter, so this is unchanged there (see the Wave A / C-1 comment above).
static ExPolygons paint_depth_clamp_keep_core(const ExPolygons &layer_slices, const float band,
                                              const float ladder_band, const float min_claim_width)
{
    // Membership test: is a part thick enough to skip the ladder? Always against ladder_band, the
    // un-notched value, so the answer cannot alternate with parity.
    ExPolygons core_full = offset_ex(layer_slices, -ladder_band);
    // `thin` = the parts of the layer a full-ladder_band inset leaves no core in, i.e. everything
    // thinner than 2*ladder_band. Built by re-dilating `core_full` rather than calling
    // opening_ex(), which would repeat the erosion we already have.
    ExPolygons thin = core_full.empty() ? layer_slices : diff_ex(layer_slices, offset_ex(core_full, ladder_band));

    // The baseline claim for non-degraded geometry still erodes by the REAL (possibly notched)
    // band, preserving the interlocking tooth's intended parity alternation there. band ==
    // ladder_band on every layer where the notch is not currently narrowing this call (odd
    // layers, or any layer once interlocking_cut_width <= 0) - reuse core_full rather than paying
    // a second, identical erosion.
    ExPolygons core = (band == ladder_band) ? core_full : offset_ex(layer_slices, -band);

    constexpr int max_ladder_steps = 6;
    float b = 0.25f * ladder_band;
    for (int step = 0; step < max_ladder_steps && ! thin.empty() && b >= min_claim_width && b > 0.f; ++step, b *= 0.5f) {
        // Which of the still-uncored parts are at least 2*b thick?
        const ExPolygons fits = intersection_ex(thin, opening_ex(layer_slices, 2.f * b));
        if (fits.empty())
            continue;
        append(core, intersection_ex(offset_ex(layer_slices, -b), fits));
        core = union_ex(core);
        thin = diff_ex(thin, fits);
    }
    return core;
}

static void cut_segmented_layers(const std::vector<ExPolygons>        &input_expolygons,
                                 std::vector<std::vector<ExPolygons>> &segmented_regions,
                                 const float                           cut_width,
                                 const float                           interlocking_depth,
                                 // Wave A / C-1: the narrowest painted claim the degradation ladder may
                                 // emit, in scaled units - one external extrusion. See
                                 // paint_depth_clamp_keep_core above.
                                 const float                           min_claim_width,
                                 // Fix-wave (absorb-tail-review.md Minor 3): out-parameter, one
                                 // ExPolygons per layer, capturing the SAME keep_core this function
                                 // already computes below and uses to cut painted claims back - the
                                 // F2 ladder's own deliberately-preserved base residue. Left empty
                                 // per-layer wherever this function does not compute one for that
                                 // layer (region_cut_width <= 0, or the whole-layer all-empty skip),
                                 // matching the shape merge_segmented_layers's absorb stage expects
                                 // (see its own new parameter). Resized here so callers do not need
                                 // to know this function's internal layer count.
                                 std::vector<ExPolygons>              &keep_core_by_layer_out,
                                 const std::function<void()>          &throw_on_cancel_callback)
{
    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - cutting segmented layers in parallel - begin";
    keep_core_by_layer_out.assign(segmented_regions.size(), ExPolygons());
    // Fix-wave F1: interlocking_cut_width (cut_width minus the interlock sub-band, clamped
    // to 0) is the Prusa-style even-layer cut width - it carves the interlock "tooth" at the
    // INNER boundary of the clamped claim, not a full replacement of cut_width by the tiny
    // interlocking_depth value. Gating on interlocking_cut_width > 0.f (rather than
    // interlocking_depth != 0.f) rather than std::max()-ing at the interlock value directly
    // also gives fix-wave F4 for free: when cut_width == 0 (e.g. paint_depth_mm == 0 in
    // millimeters mode), interlocking_cut_width == max(0 - interlocking_depth, 0) == 0
    // regardless of interlocking_depth's magnitude, so region_cut_width falls through to
    // cut_width == 0 on EVERY layer (even and odd alike) - the whole-layer skip below then
    // leaves segmented_regions untouched, i.e. unlimited, coherently on both parities. No
    // separate zero-band special case is needed at the call site.
    const float interlocking_cut_width = interlocking_depth > 0.f ? std::max(cut_width - interlocking_depth, 0.f) : 0.f;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, segmented_regions.size()),
    [&segmented_regions, &input_expolygons, &cut_width, &interlocking_cut_width, &min_claim_width, &keep_core_by_layer_out, &throw_on_cancel_callback](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            throw_on_cancel_callback();
            const float  region_cut_width       = ((layer_idx % 2 == 0) && (interlocking_cut_width > 0.f)) ? interlocking_cut_width : cut_width;
            const size_t num_extruders_plus_one = segmented_regions[layer_idx].size();
            if (region_cut_width > 0.f) {
                // Wave A / I-1: nothing to cut back means nothing to build a keep-core for. F2
                // hoisted keep_core out of the per-extruder loop below (correctly - it used to
                // rebuild the identical eroded layer once per extruder), but it landed ABOVE the
                // "claim is empty" guard, so a layer with no painted claim at all started paying a
                // whole-layer offset_ex plus, wherever any part of that layer is thinner than
                // 2*band, up to six ladder steps - each a whole-layer opening (two offsets) plus an
                // intersection, a diff and a union - and then discarding all of it. On a tall object
                // with paint on a small area that is every layer; in millimetres mode at 4-6mm,
                // where 2*band is 8-12mm, essentially every layer of an organic model reports a
                // non-empty `thin` and enters the ladder. The ladder's cost is proportional to the
                // whole layer's polygon complexity, not to its thin parts.
                if (std::all_of(segmented_regions[layer_idx].begin(), segmented_regions[layer_idx].end(),
                                [](const ExPolygons &ex_polygons) { return ex_polygons.empty(); }))
                    continue;
                // Fix-wave F2: the core no longer collapses to nothing on geometry thinner than
                // the band (see paint_depth_clamp_keep_core above). Wave A: the ladder's step is
                // chosen against the UN-NOTCHED cut_width (I-2) and floored at one external
                // extrusion (C-1); only the full-band erosion uses the notched region_cut_width.
                const ExPolygons keep_core = paint_depth_clamp_keep_core(input_expolygons[layer_idx], region_cut_width, cut_width, min_claim_width);
                // Fix-wave (absorb-tail-review.md Minor 3): capture it for the absorb stage too -
                // see this function's header comment and merge_segmented_layers's own use below.
                keep_core_by_layer_out[layer_idx] = keep_core;
                std::vector<ExPolygons> segmented_regions_cuts(num_extruders_plus_one); // Indexed by extruder_id
                for (size_t extruder_idx = 0; extruder_idx < num_extruders_plus_one; ++extruder_idx)
                    if (const ExPolygons &ex_polygons = segmented_regions[layer_idx][extruder_idx]; !ex_polygons.empty())
                        segmented_regions_cuts[extruder_idx] = diff_ex(ex_polygons, keep_core);
                segmented_regions[layer_idx] = std::move(segmented_regions_cuts);
            }
        }
    }); // end of parallel_for
    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - cutting segmented layers in parallel - end";
}

static bool is_volume_sinking(const indexed_triangle_set &its, const Transform3d &trafo)
{
    const Transform3f trafo_f = trafo.cast<float>();
    for (const stl_vertex &vertex : its.vertices)
        if ((trafo_f * vertex).z() < SINKING_Z_THRESHOLD) return true;
    return false;
}

//#define MMU_SEGMENTATION_DEBUG_TOP_BOTTOM

// Vertical paint-depth alignment fix (.superpowers/sdd/2026-08-31-paint-depth/
// vertical-depth-investigation.md, shell-coverage-investigation.md): discover_vertical_shells
// / discover_horizontal_shells (PrintObject.cpp:1954-1967, :1983-1996, :4141-4147) build the
// solid top/bottom shell to whichever is DEEPER of "N layers" (top_shell_layers /
// bottom_shell_layers) or "T millimeters" (top_shell_thickness / bottom_shell_thickness),
// walked against each layer's REAL print_z / bottom_z - e.g. PrintObject.cpp:1960-1961:
//   for (; i < int(cache_top_botom_regions.size()) &&
//          (i < itop || m_layers[i]->print_z - print_z < region_config.top_shell_thickness - EPSILON); ++i)
// This mirrors that exact walk (same strict "< thickness - EPSILON" boundary, same use of
// each layer's actual print_z/height so variable layer height is handled without any
// assumption of a constant layer height) to compute, for a specific painted surface layer,
// how many layers its claim needs to descend to cover the same "N or T, whichever is deeper"
// shell. Returns the count as a TOTAL layer depth (the surface layer plus however many
// sub-layers below/above it), directly comparable to / max-able with n_layers, so callers can
// feed the result straight into the existing count-only descent loops at :1400 (top) / :1420
// (bottom) unchanged.
//
// Why max(n_layers, thickness-driven-count) is exactly equivalent to inlining the shell
// generators' OR-condition into the descent loop, not just an approximation: both the
// "count" half (m < n_layers) and the "thickness" half (cumulative height < thickness) of
// that OR are monotonic in descent depth - each holds for depths 1..k and fails for every
// depth beyond some k (print_z/bottom_z are monotonic per layer, so the cumulative height
// gap only grows as the walk goes further) - so each is exactly the set {1..k} for its own
// k, and the union of two such prefix sets is simply {1..max(k1,k2)}. Feeding
// max(n_layers, thickness-driven-count) into the ORIGINAL unchanged loop bound therefore
// reproduces the interleaved walk's result exactly, including its boundary/EPSILON handling.
static inline int effective_shell_layers_by_thickness(const ConstLayerPtrsAdaptor &layers, size_t surface_layer_idx, bool top, int n_layers, double thickness)
{
    // Fix-wave C1 (.superpowers/sdd/2026-08-31-paint-depth/vertical-depth-fix-review.md):
    // a zero layer count is not "no minimum, thickness still applies" - it is "no shell at
    // all", exactly mirroring PrintObject.cpp:1965 (`if (n_top_layers > 0)`), :1994 (bottom
    // counterpart) and :4124 (`if (num_solid_layers == 0) continue;`), which gate their entire
    // gather/scatter blocks on a nonzero count and never reach the thickness term when it's 0.
    // LayerRegion.cpp:1025-1036 goes further and demotes the surface itself (stTop/stBottom ->
    // stInternal/stInternalVoid) in that case, so there is no solid skin to paint a color onto,
    // let alone a shell beneath it. Returning early here (before the thickness walk below can
    // raise `effective` above 0) keeps this helper's zero-shell case matching that reality.
    if (n_layers <= 0)
        return n_layers;
    int effective = n_layers;
    if (thickness > 0.) {
        const size_t   num_layers = layers.size();
        const coordf_t base       = top ? layers[surface_layer_idx]->print_z : layers[surface_layer_idx]->bottom_z();
        int            m          = 0;
        // Fix-wave I1: `++m` must count the surface layer itself, in addition to every layer
        // strictly below/above it that the walk visits. When the loop breaks, the layer that
        // triggered the break is already included by that same iteration's `++m`, so `m` is
        // already the correct total depth. When the loop instead runs off the end of the
        // object (idx < 0 for top, idx == num_layers for bottom) every visited layer was inside
        // `thickness` and none of them ever triggered a break - `m` as left by the loop counts
        // only the layers below/above the surface, one short of the total depth including the
        // surface layer itself, so it needs one more increment here.
        if (top) {
            int idx = int(surface_layer_idx) - 1;
            for (; idx >= 0; --idx) {
                ++m;
                if (base - layers[idx]->print_z >= thickness - EPSILON)
                    break;
            }
            if (idx < 0)
                ++m;
        } else {
            size_t idx = surface_layer_idx + 1;
            for (; idx < num_layers; ++idx) {
                ++m;
                if (layers[idx]->bottom_z() - base >= thickness - EPSILON)
                    break;
            }
            if (idx >= num_layers)
                ++m;
        }
        effective = std::max(effective, m);
    }
    return effective;
}

// TAPER BOUND (approved user decision, .superpowers/sdd/2026-08-31-paint-depth/
// taper-bound-report.md): "painted top/bottom claims keep FULL WIDTH for the solid-shell
// depth; taper only below that". Since the vertical-depth fix wave the claim depth IS the
// solid-shell depth, so within the claim there must be no width loss at all.
//
// WHAT THE EROSION IS FOR (established before changing it; the descent loops below do
// `offset -= (stat.extrusion_spacing + stat.extrusion_width)` and apply that shrink to the
// LAYER OUTLINE, not to the painted patch). It is a PERIMETER-SAFETY MARGIN ON INFERRED
// CLAIMS. The surface layer's claim is what the user actually painted and is appended with no
// margin at all; every layer below it is an INFERENCE - "the shell under this painted face
// should be painted too" - and BBS keeps those inferred claims at least one wall stack clear
// of the layer's own contour. Its own comment says so: "offset width should be 2*spacing to
// avoid too narrow area which has overlap of wall line". Land a colour boundary inside a
// perimeter band and that perimeter loop is split into a painted arc and a base arc with a
// sub-wall-width sliver between them - the unprintable-sliver / exterior-dimple class of
// upstream PrusaSlicer #7104 ("polygons < 0.1mm^2 ... causing dimples on outer primers",
// the filter_out_small_polygons() call in segmentation_top_and_bottom_layers below) and #7235
// ("MMU Painting still creating dimples in exterior perimeter", whose fix is the offset2_ex
// clean-up in merge_segmented_layers below).
//
// Its most important consequence, and the reason it cannot simply be deleted: a STEEP
// (near-vertical) painted surface projects, at each layer, a thin staircase band of width
// layer_height / tan(slope) that by construction HUGS that layer's contour - it is exactly
// the annulus between this layer's outline and the next one up, which is all that the
// occlusion trim below leaves. Propagating such a band at full width would leave a base strip only
// `layer_height / tan(slope)` wide at the perimeter of each layer below - narrower than one
// wall - and would smear the painted colour top_shell_layers deep down the whole wall.
// Today's erosion annihilates that band at the very first descent step and the descent then
// breaks. That protection must survive, and it does: see the "[paintdepth] a steep painted
// surface gains no deep full-width claim (anti-smear guard)" test.
//
// THE SPLIT. The dangerous case and the user's case are separated by exactly one quantity -
// how far the projected patch extends away from the object that sits above it (below it, for
// bottom claims). This function returns the part of `projected_patch` that is more than one
// wall stack clear of `input_expolygons[reference_layer_idx]`, i.e. the part that belongs to
// a genuinely EXPOSED, near-horizontal surface; the descent loop propagates that part at full
// width and leaves everything else to the untouched legacy eroded term.
//   - A flat top face (a prism cap, the user's 8mm painted feature, an interior island): the
//     layer above is empty or far away, so the whole patch is returned and descends at full
//     width. No footprint is lost, and no base strip exists to be slivered - the claim either
//     covers the cross-section out to its contour or is bounded by its own interior edge.
//   - A steep surface: the whole band lies within one wall stack of the layer above, so this
//     returns EMPTY and the descent is byte-identical to before the taper bound.
//   - A patch that is flat in the middle and rolls over to steep at its rim (an organic
//     model - the actual user scenario): split pointwise, correctly, with no threshold to
//     tune.
// Expressed as a slope, the criterion is `layer_height / tan(slope) >= wall + spacing`, i.e.
// the same wall-stack yardstick the erosion itself uses, derived from the quantities already
// in hand instead of an invented angle constant, and adapting automatically to layer height
// and extrusion width. It also preserves the erosion's actual invariant rather than a proxy
// for it: wherever the full-width term contributes, the base material left at that layer's
// perimeter is either nothing at all or at least one wall stack wide - never a sliver.
//
// WAVE B / OPTION N: this is now the LEGACY path only - it runs where the normal-thickness
// shell is off (unlimited mode, or a depth below one wall stack), and there it is byte-identical
// to before. Where the shell is on, fix-wave F1's per-step
// offset_ex(input_expolygons[last_idx], -wall_stack) enforces the same invariant POINTWISE on the
// deposit layer's own contour, which is strictly better: this function's criterion is measured
// against the neighbouring layer and rejects a whole patch, while F1's is measured where the
// claim actually lands and rejects only the part that would sliver. The 6.49-deg (0.1mm layers)
// on/off cliff that criterion produced is precisely what Option N exists to remove - see
// segmentation_top_and_bottom_layers's header for the derivation.
static inline ExPolygons exposed_surface_part(const ExPolygons              &projected_patch,
                                              const std::vector<ExPolygons> &input_expolygons,
                                              size_t                         reference_layer_idx,
                                              size_t                         num_layers,
                                              float                          wall_stack_width)
{
    // reference_layer_idx is layer_idx + 1 for top claims and layer_idx - 1 for bottom claims
    // (the same neighbours the occlusion trim above uses). Both can run off the object - and
    // layer_idx - 1 wraps to SIZE_MAX at layer 0 - which is precisely the "nothing above/below
    // this surface" case: the whole patch is exposed.
    if (reference_layer_idx >= num_layers || input_expolygons[reference_layer_idx].empty() || wall_stack_width <= 0.f)
        return projected_patch;
    return diff_ex(projected_patch, offset_ex(input_expolygons[reference_layer_idx], wall_stack_width));
}

// FLAT-TOP CAP FIX WAVE (.superpowers/sdd/2026-08-31-paint-depth/flat-top-cap-review.md, I1/I2):
// exposed_surface_part()'s wall-stack yardstick is POINTWISE - it measures each point's distance
// to the reference layer's own contour, not the local slope of the patch the point belongs to.
// That misclassifies two related cases the same way, because both put a point within one wall
// stack of a neighbour's contour even though the PATCH is not locally steep there:
//   I1  a genuinely FLAT ledge that merely sits beside a taller riser (a shelf beside a boss, a
//       tier of a stepped part) - the wall-stack-wide band of the ledge nearest the riser reads
//       as "sloped" even though its own local slope is 0, so it never gets capped and keeps a
//       full-D painted ring there forever (measured: 15 painted layers / 73mm2 on a slab+tower
//       fixture beside the riser, saving zero tool changes - the cap's whole purpose).
//   I2  a genuine shallow SLOPE whose per-layer staircase run r is only a little above one wall
//       stack - the yardstick keeps the INNER wall-stack-wide band (nearest the layer above) and
//       caps the OUTER r - wall_stack remainder, the wrong way round for a dome (the inner band
//       is the flatter part) - producing a bullseye of alternating capped/uncapped rings that the
//       absorb then silently re-annexes, or not, depending on gap_infill_speed.
//
// Fix: widen the pointwise verdict into a PER-COMPONENT one, using the SAME wall-stack yardstick
// exposed_surface_part() already computes - no new angle constant, just applied to the
// COMPONENT'S OWN WIDTH instead of to each point of it in isolation. exposed_surface_part()'s
// output (the part of the patch already known to be farther than one wall stack from the
// reference contour) is opened at wall_stack_width, so a component survives only if it has an
// actual core at least 2*wall_stack_width wide (a ring or ledge at least 3 wall stacks total,
// ~2.6mm / <= ~2.2deg at 0.1mm layers) - then the survivor is dilated back out by wall_stack_width
// plus the SAME erosion exposed_surface_part() already applied (2*wall_stack_width total), and
// clipped to the patch:
//   - A component that survives the opening (I1's ledge: 10mm wide, far past the 2.6mm floor) is
//     capped WHOLE, rim included - the dilate-back specifically restores the near-riser band the
//     pointwise test used to exclude, so the ledge is one decision, not two.
//   - A component that does not survive (I2's 3/4/5deg staircase rings: r - wall_stack =
//     1.03/0.55/0.26mm, all under 2*wall_stack_width = 1.757mm) returns EMPTY - the whole ring
//     stays wholly at D, exactly like any slope above the classic ~6.49deg cliff, never partially
//     capped into a bullseye.
//   - The topmost/bottommost origin's own half-ring (a patch with no reference layer at all is
//     wholly "exposed" by exposed_surface_part()'s own early return, :1502-1503) is narrower
//     than a full ring at the SAME run r (its own slab is half a layer's worth, width r/2 against
//     a full ring's r) - see the Minor-1 fix note just below the survival-threshold asymmetry
//     this halving used to cause, and how it is closed.
//
// Fix-wave 3 (flat-top-cap-fixwave-review.md Minor 1): opening the half-width apex component at
// the SAME wall_stack_width the full-width case uses makes it survive only past r > 4*
// wall_stack_width (r/2 > 2*ws), stricter than the r > 3*wall_stack_width (r > 2*ws after the
// erosion, i.e. the width itself exceeding 2*ws) every full ring below it uses - so a slope with
// run r in (3ws, 4ws] capped every ring but its own apex, leaving a hidden painted half-ring
// "fin" inside an otherwise-printable base annulus at the topmost reach (measured: a 2 mm2-scale
// annulus of 271.8mm2 at 2.0deg / 248.5mm2 at 2.15deg, walls-only paint with an unpainted crown).
// Opening the half-width component at 0.75*wall_stack_width instead makes it survive iff
// r/2 > 1.5*ws <=> r > 3*ws - exactly the full-ring rule, so the apex origin's verdict always
// agrees with its neighbours' and the asymmetry closes. The dilate-back stays 2*wall_stack_width
// (unchanged, same as the full-ring case) either way - only the SURVIVAL test's own opening
// delta narrows for this one case. No effect on a genuine free-standing top (also reached via
// this same early return): F1's own
// offset_ex(contour, -wall_stack) already leaves nothing sub-surface to claim once a
// free-standing top is narrower than 2*wall_stack_width, the threshold most real tops meet
// regardless of which opening delta this function uses.
static inline ExPolygons flat_cap_component_ex(const ExPolygons              &projected_patch,
                                                const std::vector<ExPolygons> &input_expolygons,
                                                size_t                         reference_layer_idx,
                                                size_t                         num_layers,
                                                float                          wall_stack_width)
{
    const ExPolygons exposed = exposed_surface_part(projected_patch, input_expolygons, reference_layer_idx, num_layers, wall_stack_width);
    if (exposed.empty())
        return ExPolygons{};
    // Mirrors exposed_surface_part()'s own early-return condition (:1503) exactly - true here
    // means THAT branch is what produced `exposed` (the whole patch, unopened), i.e. this is the
    // topmost/bottommost origin's own half-ring/free-standing-top case, not a normal mid-descent
    // ring with a real reference layer.
    const bool has_no_reference_layer = reference_layer_idx >= num_layers || input_expolygons[reference_layer_idx].empty() || wall_stack_width <= 0.f;
    const float opening_delta = has_no_reference_layer ? 0.75f * wall_stack_width : wall_stack_width;
    return intersection_ex(projected_patch, offset_ex(opening_ex(exposed, opening_delta), 2.f * wall_stack_width));
}

// WAVE B / OPTION N (.superpowers/sdd/2026-08-31-paint-depth/curved-gap-design.md): the painted
// claim is a CONSTANT-THICKNESS SHELL measured NORMAL to the painted surface - "a certain amount
// of inward taper and normal projection so the coloured section becomes its own object" (the
// user's own framing). Two edits below deliver it, and the derivation is worth stating once
// because it is the reason no slope measurement, no pointwise normal and no variable-radius
// offset is needed anywhere:
//
//   On a silhouette of slope theta the layer contours step inward by r = layer_height/tan(theta)
//   per layer. The descent loop deposits the surface layer j's painted ring onto layer j-m at
//   lateral inset [m*r, (m+1)*r] measured from THAT layer's own contour (the ring is the annulus
//   between contour(j) and contour(j+1), and contour(j) is m*r inside contour(j-m)). Union over
//   the M surface layers that can reach a given layer therefore covers lateral inset [0, M*r],
//   whose thickness measured along the surface normal is
//
//       M * r * sin(theta)  ==  M * layer_height * cos(theta).
//
//   So a descent of M = ceil(D / layer_height) layers delivers a normal thickness of D*cos(theta)
//   for free, at every slope, and the lateral band (cut_segmented_layers) delivers D*sin(theta)
//   on the same geometry. Their union is D * max(cos, sin) - never below D/sqrt(2).
//
// What was in the way was not the mechanism but two bounds on it:
//   N1  exposed_surface_part() switched the full-width term OFF entirely once the per-layer run
//       r fell below one wall stack - atan(layer_height/(w+s)) = 6.49 deg at 0.1mm layers. That
//       gate is a PROXY for "this claim would sliver the perimeter"; fix-wave F1's
//       offset_ex(input_expolygons[last_idx], -wall_stack) enforces the real invariant pointwise
//       on the deposit layer's own contour, so the proxy is redundant where F1 runs and its only
//       remaining effect was to throw away the slope-correct claim the descent had computed.
//   N2  the descent was bounded by the solid-shell layer count, so the depth was
//       "top_shell_layers deep" rather than "D thick". It is now
//       max(effective shell layers, layers within D), keeping the earlier shell-coverage wave's
//       contract (never leave base-coloured SOLID SHELL under painted skin) when D < shell.
//
// GATED on `paint_depth_normal_mm >= wall_stack` (and hence on the depth being bounded at all -
// unlimited mode passes 0). Below one wall stack the lateral band reaches only D while the
// F1-inset descent starts at wall_stack, so the base region would be left holding a closed ring
// of width wall_stack - D on every sub-surface layer: a new sliver class, and the exact defect
// Wave A's classic band floor closes from the other side. At paint_depth_walls = 1 that floor
// puts band(1) AT wall_stack on the classic generator (the two meet with zero width between
// them) while Arachne's unfloored band(1) = 0.578595 stays below it and keeps today's behaviour.
//
// SELF-LIMITING at steep slopes, without an angle constant: the full-width term at descent step m
// is non-empty only where (m+1)*r > wall_stack, so the whole extension is inert when
// M*r <= wall_stack, i.e. above atan(D/(w+s)) = 58.5 deg at stock flows, independently of layer
// height. In practice the ring is erased even earlier, by the opening_ex(top_ex,
// small_region_threshold) thin-projection filter below (a ring narrower than 2*threshold =
// 0.225mm at a 0.45mm outer wall cannot survive it), which binds first at any layer height under
// 0.359mm - so the reach of this change is theta < atan(layer_height/0.225) = 23.96 deg at 0.1mm
// layers. That covers the shallow dead band this exists to close; it does not reach 24-45 deg,
// where the claim stays the lateral band alone. Widening it would mean lowering the #7104
// sliver guard, which the design deliberately did not propose.
//
// Fix-wave 3 (absorb-tail-fixwave2-review.md I-1): per-(layer, colour) statistics, used both by
// segmentation_top_and_bottom_layers's own top/bottom descent (the erosion widths below) and, as
// of this fix-wave, by segmentation_by_painting's own interior inter-claim absorb kill-width
// construction - the SAME struct, filled by the SAME function (compute_layer_color_stat below),
// so the two can never resolve a different answer for the same (layer, colour) again. Hoisted to
// file scope (used to be a local struct + lambda inside segmentation_top_and_bottom_layers alone)
// for exactly that reason - segmentation_by_painting needs it too, and duplicating the resolution
// logic there would reopen the very defect this fix-wave closes (two separately-approximated
// per-layer signals - the descent's own erosion width and the absorb's kill width - that could
// silently drift apart, as I-1 measured: 8.14 / 6.01 mm2 base slivers at the sphere fixture's cap
// boundary with a gap-fill-off PARAMETER_MODIFIER at print z 2-3, where the pre-fixwave-2 parent
// commit absorbed them).
struct LayerColorStat {
    // Number of regions for a queried color.
    int     num_regions             { 0 };
    // Maximum perimeter extrusion width for a queried color.
    float   extrusion_width         { 0.f };
    // Minimum radius of a region to be printable. Used to filter regions by morphological opening.
    float   small_region_threshold  { 0.f };
    // Maximum number of top layers for a queried color.
    int     top_shell_layers        { 0 };
    // Maximum number of bottom layers for a queried color.
    int     bottom_shell_layers     { 0 };
    // WAVE B / Option N: total depth (surface layer included) the top/bottom descent runs to -
    // max(the solid shell above, however many layers the normal thickness D spans). Kept
    // SEPARATE from *_shell_layers, which stays the pure solid-shell answer, because the
    // "is anything claimed at all" gates below are C1 contracts about the SHELL (a zero shell
    // count claims nothing, not even the painted surface facet) and must not be deepened.
    int     top_descent_layers      { 0 };
    int     bottom_descent_layers   { 0 };
    // WAVE B / Option N: whether this colour's descent is a normal-thickness shell on this
    // layer - i.e. it is a PAINTED colour (not the base) and the bounded depth D is at least
    // one wall stack. False restores legacy behaviour verbatim: the exposed_surface_part()
    // slope gate, the shell-count depth bound and the eager break.
    bool    normal_shell            { false };
    //BBS: spacing according to width and layer height
    float   extrusion_spacing{ 0.f };
    // Fix-wave 3 (I-1): true iff the region that actually won small_region_threshold /
    // extrusion_spacing below (colour > 0 only - color_idx == 0's aggregate "don't know" case
    // never sets this) has gap_infill_speed <= 0, i.e. small_region_threshold is genuinely this
    // LAYER's gap-fill-disabled (wider) arm, not the ordinary one. Lets segmentation_by_
    // painting's absorb kill-width construction key off exactly the same per-layer resolution
    // this struct's own small_region_threshold already uses for the descent's own erosion,
    // instead of maintaining a second, separately-approximated signal.
    bool    small_region_threshold_gapfill_off { false };
};

// Fix-wave 3 (absorb-tail-fixwave2-review.md I-1): a painted colour (color_idx > 0) can have MORE
// THAN ONE PrintRegion matching wall_filament == color_idx on the very same layer - one inherited
// from the object's own base geometry, another auto-created from a PARAMETER_MODIFIER overriding
// e.g. gap_infill_speed over a confined Z range (PrintApply.cpp:1082-1123 creates one auto-created
// painted PrintRegion per distinct parent config). PrintObjectSlice.cpp:5199-5208 gives every
// layer a LayerRegion for EVERY PrintRegion on the object regardless of whether it has any
// geometry here, and - the fact that makes "just check the candidate's own slices" unusable - an
// auto-created painted PrintRegion's OWN LayerRegion::slices is unconditionally EMPTY at this
// point in the pipeline: apply_mm_segmentation, which writes it, runs strictly after this whole
// segmentation (see compute_layer_color_stat's own N1 comment below for the same fact, cited
// there for the same reason). So "does this candidate's own geometry cover this layer" has to be
// asked of whichever VOLUME actually produced the candidate, which the PrintObjectRegions graph
// (PrintObjectRegions::PaintedRegion::parent -> LayerRangeRegions::volume_regions[parent],
// PrintApply.cpp:1116) still remembers, and which - unlike the painted region itself - IS an
// ordinarily-sliced region with real per-layer slices.
//
// Returns true when `candidate` should be treated as applicable on `layer`. For an auto-created
// painted region this is resolved via its parent's own slices; for anything else (a plain body or
// modifier directly configured with this wall_filament, never routed through the paint gizmo, so
// never entered into painted_regions at all) the candidate's own slices already answer this
// correctly - the same per-layer discipline compute_layer_color_stat's own shell-depth block
// already relies on for every region type.
static bool layer_color_stat_variant_applies_here(const PrintObject &print_object, const Layer &layer, const LayerRegion *candidate)
{
    const PrintRegion        *candidate_region = &candidate->region();
    const PrintObjectRegions *shared           = print_object.shared_regions();
    if (shared != nullptr)
        for (const PrintObjectRegions::LayerRangeRegions &layer_range : shared->layer_ranges) {
            if (! (layer_range.layer_height_range.first <= layer.print_z && layer.print_z <= layer_range.layer_height_range.second))
                continue;
            auto it = std::find_if(layer_range.painted_regions.begin(), layer_range.painted_regions.end(),
                                    [candidate_region](const PrintObjectRegions::PaintedRegion &pr) { return pr.region == candidate_region; });
            if (it != layer_range.painted_regions.end() && it->parent >= 0 && size_t(it->parent) < layer_range.volume_regions.size())
                if (const PrintRegion *parent_region = layer_range.volume_regions[size_t(it->parent)].region) {
                    const int parent_id = parent_region->print_object_region_id();
                    if (parent_id >= 0 && size_t(parent_id) < layer.regions().size())
                        return ! layer.regions()[size_t(parent_id)]->slices.empty();
                }
            break; // found the layer's own range; whether or not it resolved a parent, do not scan the others
        }
    // Not a recognised auto-created painted region (or the graph did not resolve it) - the
    // candidate's own slices are the correct signal, same as every other per-layer gate here.
    return ! candidate->slices.empty();
}

static LayerColorStat compute_layer_color_stat(const ConstLayerPtrsAdaptor &layers, const PrintObject &print_object, const float paint_depth_normal_mm, const size_t layer_idx, const size_t color_idx)
{
    LayerColorStat out;
    const Layer &layer = *layers[layer_idx];
    // Fix-wave 3 (I-1): resolved from ONLY the candidate(s) that actually apply here (colour > 0)
    // when that can be determined at all; `fallback_*` reproduces the OLD unconditional
    // last-wins (kept as a safety net for when layer_color_stat_variant_applies_here cannot
    // resolve anything - shared_regions() unavailable - so behaviour degrades to exactly what it
    // was before this fix rather than a zeroed-out threshold).
    float fallback_small_region_threshold = 0.f, fallback_extrusion_spacing = 0.f;
    bool  fallback_gapfill_off = false;
    float applicable_small_region_threshold = 0.f, applicable_extrusion_spacing = 0.f;
    bool  have_applicable = false, applicable_gapfill_off = false;
    // Paint penetration (top_color_penetration_layers / bottom_color_penetration_layers, Bambu
    // Studio's keys): a region with a value >= 1 sets its paint depth in layers directly, the
    // surface layer included, instead of deriving it from its shell settings; 0 (the default)
    // keeps the shell-derived depth below untouched. Maxed over the regions present on this
    // layer exactly like the shell depth, and applied after the Wave B deepening further down
    // so an explicit depth is never deepened by the paint-depth normal thickness.
    int top_penetration_layers = 0, bottom_penetration_layers = 0;
    for (const LayerRegion *region : layer.regions()) {
        const PrintRegionConfig &config = region->region().config();
        // Fix-wave I2 (.superpowers/sdd/2026-08-31-paint-depth/vertical-depth-fix-review.md):
        // PrintObjectSlice.cpp:5199-5208 gives every layer a LayerRegion for EVERY
        // PrintRegion on the object, whether or not that region has any geometry on this
        // particular layer. Without this guard, a region confined to another part of the
        // object (a modifier, or a Z-stacked volume) still contributes its shell settings
        // to the max below, inflating the claim depth on layers it never actually touches.
        // region->slices is populated per-layer at PrintObjectSlice.cpp:5229-5235, before
        // segmentation runs at :5267, so this is valid and free.
        //
        // Fix-wave N1 (.superpowers/sdd/2026-08-31-paint-depth/vertical-depth-fixwave-
        // rereview.md): this guard must scope ONLY the shell-depth max just below, never
        // the per-colour extrusion-stat block that follows. The auto-created painted
        // region (the ONLY region with wall_filament == a painted color_idx,
        // PrintApply.cpp:1088-1090) has empty slices on EVERY layer at this point in the
        // pipeline - its geometry isn't written until apply_mm_segmentation() runs, AFTER
        // this segmentation (PrintObjectSlice.cpp:5230 -> :5267 -> :5275). Skipping it
        // unconditionally (i.e. gating the per-colour block on it too) would zero
        // num_regions/extrusion_width/extrusion_spacing/small_region_threshold for EVERY
        // painted colour on EVERY layer, silently disabling the lateral inward taper
        // (:1542/:1562 below) and the #7104 thin-projection filter, and re-arming
        // assert(out.num_regions > 0) below. The shell-depth max loses nothing by keeping
        // the guard scoped to just this block: a painted region always shares its parent
        // volume region's top/bottom_shell_layers/thickness (PrintApply.cpp:1088-1090
        // only overrides the filament ids), and the parent DOES have slices on every layer
        // it occupies, so the max already sees that depth via the parent.
        if (! region->slices.empty()) {
            // Vertical paint-depth alignment fix, shell-coverage-investigation.md fix (2): the
            // solid shell a painted claim must cover is whichever region reaches deepest on
            // THIS layer, not only the region(s) that happen to carry this paint color right
            // now - a painted patch can span regions with different shell settings (e.g. a
            // modifier with its own top_shell_layers), and the shell the base material builds
            // underneath does not care which color is painted above it. So these two are
            // maxed over ALL regions on the layer, unconditionally - unlike extrusion_width /
            // small_region_threshold / extrusion_spacing below, which stay scoped to color_idx
            // (they size the lateral taper of THIS color's own claim, untouched by this fix).
            // Effective count = max(configured layer count, however many layers this object's
            // REAL layer heights need to cover the configured thickness) - see
            // effective_shell_layers_by_thickness() above, which mirrors discover_vertical_
            // shells / discover_horizontal_shells exactly, including variable layer height.
            if (config.top_color_penetration_layers.value > 0)
                top_penetration_layers = std::max(top_penetration_layers, config.top_color_penetration_layers.value);
            else
                out.top_shell_layers    = std::max(out.top_shell_layers,
                                                    effective_shell_layers_by_thickness(layers, layer_idx, true,  config.top_shell_layers.value,    config.top_shell_thickness.value));
            if (config.bottom_color_penetration_layers.value > 0)
                bottom_penetration_layers = std::max(bottom_penetration_layers, config.bottom_color_penetration_layers.value);
            else
                out.bottom_shell_layers = std::max(out.bottom_shell_layers,
                                                    effective_shell_layers_by_thickness(layers, layer_idx, false, config.bottom_shell_layers.value, config.bottom_shell_thickness.value));
        }
        if (// color_idx == 0 means "don't know" extruder aka the underlying extruder.
            // As this region may split existing regions, we collect statistics over all regions for color_idx == 0.
            color_idx == 0 || config.wall_filament == int(color_idx)) {
            //BBS: the extrusion line width is outer wall rather than inner wall
            const double nozzle_diameter = print_object.print()->config().nozzle_diameter.get_at(0);
            double outer_wall_line_width = config.get_abs_value("outer_wall_line_width", nozzle_diameter);
            out.extrusion_width     = std::max<float>(out.extrusion_width, outer_wall_line_width);
            const bool  gapfill_off = ! (config.gap_infill_speed.values.front() > 0.f);
            float small_region_threshold = config.gap_infill_speed.values.front() > 0 ?
                                         // Gap fill enabled. Enable a single line of 1/2 extrusion width.
                                         0.5f * outer_wall_line_width :
                                         // Gap fill disabled. Enable two lines slightly overlapping.
                                         outer_wall_line_width + 0.7f * Flow::rounded_rectangle_extrusion_spacing(outer_wall_line_width, float(layer.height));
            small_region_threshold = scaled<float>(small_region_threshold * 0.5f);
            const float extrusion_spacing = Flow::rounded_rectangle_extrusion_spacing(float(outer_wall_line_width), float(layer.height));
            // Fix-wave 3 (I-1): last-region-wins used to overwrite small_region_threshold /
            // extrusion_spacing here unconditionally, so whichever candidate happened to be
            // iterated last (region order is print_object_region_id() / creation order, NOT
            // z-relevance) silently applied its own gap_infill_speed on EVERY layer, not only
            // the ones its own geometry covers. Now only a candidate that actually applies here
            // may win; color_idx == 0's aggregate case is untouched (every region always
            // "applies" to it, same as the shell-depth block above).
            fallback_small_region_threshold = small_region_threshold;
            fallback_extrusion_spacing      = extrusion_spacing;
            fallback_gapfill_off            = gapfill_off;
            if (color_idx == 0 || layer_color_stat_variant_applies_here(print_object, layer, region)) {
                applicable_small_region_threshold = small_region_threshold;
                applicable_extrusion_spacing      = extrusion_spacing;
                applicable_gapfill_off            = gapfill_off;
                have_applicable                   = true;
            }
            ++ out.num_regions;
        }
    }
    if (have_applicable) {
        out.small_region_threshold             = applicable_small_region_threshold;
        out.extrusion_spacing                  = applicable_extrusion_spacing;
        out.small_region_threshold_gapfill_off = applicable_gapfill_off;
    } else {
        // Unresolvable (see header comment) - reproduce the pre-fix unconditional last-wins
        // exactly, rather than a zeroed-out threshold.
        out.small_region_threshold             = fallback_small_region_threshold;
        out.extrusion_spacing                  = fallback_extrusion_spacing;
        out.small_region_threshold_gapfill_off = fallback_gapfill_off;
    }
    assert(out.num_regions > 0);
    // WAVE B / Option N: deepen the descent from "the solid shell" to "whatever covers a
    // normal thickness of paint_depth_normal_mm", reusing effective_shell_layers_by_thickness
    // with a layer count of 1 so the thickness half of its walk runs against this object's
    // REAL print_z / bottom_z values - variable layer height handled exactly, and the same
    // "< thickness - EPSILON" boundary the solid-shell generators use, rather than a D /
    // layer_height division that would silently assume uniform layers (design section 6
    // hazard 5). max() with the shell count keeps the earlier shell-coverage wave's contract
    // intact when a user sets D below their solid shell; gating on the shell count being
    // nonzero keeps C1's "no shell means no claim" intact when they set it to zero.
    //
    // PAINTED COLOURS ONLY (color_idx > 0). color_idx 0 is not a paint claim at all - it is
    // the object's base filament, and its top/bottom claim exists for one reason: to stop a
    // neighbouring painted colour smearing across the SOLID SHELL under an UNPAINTED top or
    // bottom face. That contract is written in shell terms (top_shell_layers /
    // top_shell_thickness) and "how thick is the paint" says nothing about it.
    //
    // Deepening it is not merely pointless, it inverts the feature. merge_segmented_layers
    // trims EVERY extruder's lateral claim by EVERY extruder's top/bottom claim (see its
    // diff_ex over top_and_bottom_layers), so a base-colour claim that descended D deep would
    // cut the painted lateral band back to one wall stack on every layer beneath any
    // unpainted cap. Measured, not reasoned: a 40x40x6mm box with one side painted and
    // paint_depth_mm = 6 loses its 6mm band down to 0.857mm the moment the unpainted top
    // cap's base claim is given the same normal thickness. Paint depth bounds paint.
    //
    // The same colour test gates N1 and N3 below, for the same reason and with the same
    // effect: color_idx 0's descent stays byte-identical to legacy, gate and break included.
    out.extrusion_width = scaled<float>(out.extrusion_width);
    out.extrusion_spacing = scaled<float>(out.extrusion_spacing);
    // The D >= wall_stack gate (design section 5). Below one wall stack the lateral band
    // reaches only D while the F1-inset descent starts at wall_stack, leaving the base region
    // a closed ring of width wall_stack - D sandwiched between two painted annuli on every
    // sub-surface layer: a new sliver class. SCALED_EPSILON of slack because on the CLASSIC
    // generator Wave A's band floor makes D and wall_stack the SAME quantity by construction
    // (band(1) is floored at ext_perimeter_width + ext_perimeter_spacing) and the two reach
    // here down different float paths - a rounding ULP must not decide whether a user gets a
    // normal-thickness shell. On Arachne band(1) = 0.578595 stays genuinely below the floor
    // and keeps today's behaviour, which is the intent.
    // Loose end 2 (interclaim-sliver-investigation.md section 7): guard against the
    // degenerate layer_color_stat path where no region on this layer carries wall_filament
    // == color_idx (out.num_regions == 0, assert-only in debug - see the N1 comment above).
    // extrusion_width/extrusion_spacing then stay 0, so wall_stack == 0 and F1's
    // offset_ex(contour, -0) below is a no-op - but without this term normal_shell still
    // evaluated true (scaled(D) + eps >= 0 always holds), which would let the claim reach
    // the contour with no F1 clearance at all, exactly the exterior bleed F1 exists to stop.
    // Unreachable for a real object per N1, but cheap to close and free of any behavior
    // change on the reachable path (wall_stack > 0 there always).
    out.normal_shell = paint_depth_normal_mm > 0.f && color_idx > 0 &&
                       (out.extrusion_spacing + out.extrusion_width) > 0.f &&
                       scaled<float>(paint_depth_normal_mm) + float(SCALED_EPSILON) >= out.extrusion_spacing + out.extrusion_width;
    out.top_descent_layers    = out.top_shell_layers;
    out.bottom_descent_layers = out.bottom_shell_layers;
    if (out.normal_shell) {
        if (out.top_shell_layers > 0)
            out.top_descent_layers    = std::max(out.top_shell_layers,
                                                 effective_shell_layers_by_thickness(layers, layer_idx, true,  1, double(paint_depth_normal_mm)));
        if (out.bottom_shell_layers > 0)
            out.bottom_descent_layers = std::max(out.bottom_shell_layers,
                                                 effective_shell_layers_by_thickness(layers, layer_idx, false, 1, double(paint_depth_normal_mm)));
    }
    // Paint penetration override (see the loop above). It stands in for the shell depth in
    // every role *_shell_layers plays here - the "is anything claimed" gate, the legacy shadow
    // and the flat-top cap threshold - and it is the descent depth itself, so a flat painted top
    // with a penetration of P is claimed exactly P layers deep whatever the shell settings and
    // the paint-depth mode say (descent == shell leaves the flat-top cap inactive). Unlike a
    // shell count it claims even where the region's own shell count is 0: that is what the
    // setting asks for, and what Bambu Studio does with it. With every region at 0 both fields
    // keep the values computed above, so existing projects slice exactly as before.
    if (top_penetration_layers > 0) {
        out.top_shell_layers   = std::max(out.top_shell_layers,   top_penetration_layers);
        out.top_descent_layers = std::max(out.top_descent_layers, top_penetration_layers);
    }
    if (bottom_penetration_layers > 0) {
        out.bottom_shell_layers   = std::max(out.bottom_shell_layers,   bottom_penetration_layers);
        out.bottom_descent_layers = std::max(out.bottom_descent_layers, bottom_penetration_layers);
    }
    return out;
}

// Returns segmentation of top and bottom layers based on painting in segmentation gizmos.
// Fix-wave (wave-b-review.md Important 2): legacy_top_and_bottom_layers_out is filled with the
// SAME per-colour top/bottom claims, but each one stopped at top_shell_layers/bottom_shell_layers
// (the pre-Wave-B solid-shell depth) instead of the possibly-deepened top_descent_layers/
// bottom_descent_layers. merge_segmented_layers uses it to stop a painted colour's DEEPENED reach
// from silently annexing a neighbouring painted colour's own lateral claim any deeper than it did
// before Wave B, while leaving the deepened reach fully intact against the base colour (Wave B's
// own fix) and for a colour's own claim. Always sized [num_facets_states][num_layers] whenever
// the main return value is (i.e. whenever this function does any work at all) so the two stay
// index-compatible; left empty (default-constructed) by any early return.
static inline std::vector<std::vector<ExPolygons>> segmentation_top_and_bottom_layers(const PrintObject                                               &print_object,
                                                                                      const std::vector<ExPolygons>                                   &input_expolygons,
                                                                                      const std::function<ModelVolumeFacetsInfo(const ModelVolume &)> &extract_facets_info,
                                                                                      const size_t                                                     num_facets_states,
                                                                                      const float                                                      paint_depth_normal_mm,
                                                                                      std::vector<std::vector<ExPolygons>>                            &legacy_top_and_bottom_layers_out,
                                                                                      const std::function<void()>                                     &throw_on_cancel_callback)
{
    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - Segmentation of top and bottom layers in parallel - Begin";
    const size_t num_layers    = input_expolygons.size();
    const ConstLayerPtrsAdaptor layers = print_object.layers();

    // Maximum number of top / bottom layers accounts for maximum overlap of one thread group into a neighbor thread group.
    //
    // Vertical paint-depth alignment fix: top_shell_thickness / bottom_shell_thickness can
    // demand more layers than top_shell_layers / bottom_shell_layers alone (see the
    // effective_shell_layers_by_thickness() comment above) - but ONLY when the corresponding
    // layer count is itself nonzero (fix-wave C1: a zero count means no shell at all, and
    // thickness is dead config in that case - see effective_shell_layers_by_thickness()'s early
    // return and PrintObject.cpp:1965/:1994/:4124). top_layers_eff / bottom_layers_eff below
    // gate on the configured count being > 0 before consulting thickness, matching that helper
    // exactly. max_top_layers / max_bottom_layers / granularity must still account for the
    // thickness-driven deepening in the nonzero case, both so the projection gate right below
    // opens whenever there is a real (count- or thickness-driven) shell to claim, and so the
    // TBB double-buffer parity trick (layer_idx_offset, further below) keeps enough overlap
    // margin for the deeper per-layer descent. This is a conservative *upper bound*
    // only (sizing/gating, not the actual per-layer claim depth, which layer_color_stat()
    // computes exactly below via effective_shell_layers_by_thickness() against each layer's
    // real height): it uses the thinnest layer anywhere in the object, so a uniform-height
    // estimate can only overestimate the layers a given thickness needs, never underestimate
    // (every real layer's height is >= min_layer_height, so its real cumulative height after k
    // layers is >= k * min_layer_height - reaching `thickness` at least as fast).
    double min_layer_height = 0.;
    for (const Layer *layer : layers)
        if (layer->height > EPSILON && (min_layer_height <= 0. || layer->height < min_layer_height))
            min_layer_height = layer->height;
    auto layers_for_thickness = [&min_layer_height, num_layers](double thickness) -> int {
        if (thickness <= 0.)
            return 0;
        if (min_layer_height <= EPSILON)
            return int(num_layers); // Defensive fallback; real layers always have height > 0.
        return std::min(int(num_layers), int(thickness / min_layer_height) + 1);
    };
    // WAVE B / Option N, hazard 2 (curved-gap-design.md section 6): `granularity` is not a
    // performance knob, it is the CORRECTNESS margin of the TBB double-buffer parity trick below.
    // Each thread group writes its descent output into
    // shell_triangles_by_color_*[last_idx + layer_idx_offset] with layer_idx_offset chosen from
    // (range.begin() / granularity) & 1, so two groups that are two apart share a buffer; a layer
    // at the start of a group reaches back `descent depth - 1` layers, and that must stay inside
    // the immediately preceding group (opposite parity). Sizing granularity from the SHELL count
    // while the descent is bounded by a normal thickness D that is deeper than the shell (15
    // layers against 6 at stock defaults / 0.1mm layers) would let a group write into a buffer a
    // same-parity group is concurrently writing - a data race, not a slowdown. So the sizing and
    // the descent bound must be computed from the same quantity, which is what *_descent_eff is.
    int max_top_layers = 0;
    int max_bottom_layers = 0;
    int granularity = 1;
    const int paint_depth_normal_layers = layers_for_thickness(double(paint_depth_normal_mm));
    for (size_t i = 0; i < print_object.num_printing_regions(); ++ i) {
        const PrintRegionConfig &config = print_object.printing_region(i).config();
        // Fix-wave C1: gate on the configured count being nonzero before consulting thickness -
        // a zero count means the generators never build that shell at all (see above), so
        // thickness must not resurrect it here either.
        const int top_layers_eff    = config.top_shell_layers.value > 0
            ? std::max(config.top_shell_layers.value, layers_for_thickness(config.top_shell_thickness.value)) : 0;
        const int bottom_layers_eff = config.bottom_shell_layers.value > 0
            ? std::max(config.bottom_shell_layers.value, layers_for_thickness(config.bottom_shell_thickness.value)) : 0;
        // Wave B: the normal-thickness bound deepens an EXISTING shell, it never creates one -
        // C1's "a zero shell count claims nothing at all" is unchanged, and so is the meaning of
        // max_top_layers / max_bottom_layers as the gate that decides whether slice_mesh_slabs
        // runs at all (both stay zero exactly when they were zero before).
        // Paint penetration: a region with an explicit depth descends exactly that far (see
        // compute_layer_color_stat()), so that depth is its bound instead of the shell's.
        const int top_penetration    = config.top_color_penetration_layers.value;
        const int bottom_penetration = config.bottom_color_penetration_layers.value;
        const int top_descent_eff    = top_penetration > 0 ? top_penetration :
                                       top_layers_eff    > 0 ? std::max(top_layers_eff,    paint_depth_normal_layers) : 0;
        const int bottom_descent_eff = bottom_penetration > 0 ? bottom_penetration :
                                       bottom_layers_eff > 0 ? std::max(bottom_layers_eff, paint_depth_normal_layers) : 0;
        max_top_layers    = std::max(max_top_layers, top_descent_eff);
        max_bottom_layers = std::max(max_bottom_layers, bottom_descent_eff);
        granularity       = std::max(granularity, std::max(top_descent_eff, bottom_descent_eff) - 1);
    }

    // Project upwards pointing painted triangles over top surfaces,
    // project downards pointing painted triangles over bottom surfaces.
    std::vector<std::vector<Polygons>> top_raw(num_facets_states), bottom_raw(num_facets_states);
    std::vector<float> zs = zs_from_layers(layers);
    Transform3d        object_trafo = print_object.trafo_centered();

#ifdef MM_SEGMENTATION_DEBUG_TOP_BOTTOM
    static int iRun = 0;
#endif // MM_SEGMENTATION_DEBUG_TOP_BOTTOM

    if (max_top_layers > 0 || max_bottom_layers > 0) {
        for (const ModelVolume *mv : print_object.model_object()->volumes)
            if (mv->is_model_part()) {
                const Transform3d volume_trafo = object_trafo * mv->get_matrix();
                for (size_t extruder_idx = 0; extruder_idx < num_facets_states; ++extruder_idx) {
                    const indexed_triangle_set painted = extract_facets_info(*mv).facets_annotation.get_facets_strict(*mv, EnforcerBlockerType(extruder_idx));
#ifdef MM_SEGMENTATION_DEBUG_TOP_BOTTOM
                    {
                        static int iRun = 0;
                        its_write_obj(painted, debug_out_path("mm-painted-patch-%d-%d.obj", iRun ++, extruder_idx).c_str());
                    }
#endif // MM_SEGMENTATION_DEBUG_TOP_BOTTOM
                    if (! painted.indices.empty()) {
                        std::vector<Polygons> top, bottom;
                        if (!zs.empty() && is_volume_sinking(painted, volume_trafo)) {
                            std::vector<float> zs_sinking = {0.f};
                            Slic3r::append(zs_sinking, zs);
                            slice_mesh_slabs(painted, zs_sinking, volume_trafo, max_top_layers > 0 ? &top : nullptr, max_bottom_layers > 0 ? &bottom : nullptr, nullptr, throw_on_cancel_callback);

                            MeshSlicingParams slicing_params;
                            slicing_params.trafo = volume_trafo;
                            Polygons bottom_slice = slice_mesh(painted, zs[0], slicing_params);

                            top.erase(top.begin());
                            bottom.erase(bottom.begin());

                            bottom[0] = union_(bottom[0], bottom_slice);
                        } else
                            slice_mesh_slabs(painted, zs, volume_trafo, max_top_layers > 0 ? &top : nullptr, max_bottom_layers > 0 ? &bottom : nullptr, nullptr, throw_on_cancel_callback);
                        auto merge = [](std::vector<Polygons> &&src, std::vector<Polygons> &dst) {
                            auto it_src = find_if(src.begin(), src.end(), [](const Polygons &p){ return ! p.empty(); });
                            if (it_src != src.end()) {
                                if (dst.empty()) {
                                    dst = std::move(src);
                                } else {
                                    assert(src.size() == dst.size());
                                    auto it_dst = dst.begin() + (it_src - src.begin());
                                    for (; it_src != src.end(); ++ it_src, ++ it_dst)
                                        if (! it_src->empty()) {
                                            if (it_dst->empty())
                                                *it_dst = std::move(*it_src);
                                            else
                                                append(*it_dst, std::move(*it_src));
                                        }
                                }
                            }
                        };
                        merge(std::move(top),    top_raw[extruder_idx]);
                        merge(std::move(bottom), bottom_raw[extruder_idx]);
                    }
                }
            }
    }

    auto filter_out_small_polygons = [&num_facets_states, &num_layers](std::vector<std::vector<Polygons>> &raw_surfaces, double min_area) -> void {
        for (size_t extruder_idx = 0; extruder_idx < num_facets_states; ++extruder_idx)
            if (!raw_surfaces[extruder_idx].empty())
                for (size_t layer_idx = 0; layer_idx < num_layers; ++layer_idx)
                    if (!raw_surfaces[extruder_idx][layer_idx].empty())
                        remove_small(raw_surfaces[extruder_idx][layer_idx], min_area);
    };

    // Filter out polygons less than 0.01mm^2, because they are unprintable and causing dimples on outer primers (#7104).
    // (Loose end 1, .superpowers/sdd/2026-08-31-paint-depth/interclaim-sliver-investigation.md
    // section 7: this comment previously said "0.1mm^2" - off by 100x. The argument below is
    // sqr(scale_(0.1f)), i.e. the square of a 0.1mm LENGTH, which is 0.01mm^2 of AREA. Harmless,
    // was misleading.)
    filter_out_small_polygons(top_raw, Slic3r::sqr(scale_(0.1f)));
    filter_out_small_polygons(bottom_raw, Slic3r::sqr(scale_(0.1f)));

#ifdef MM_SEGMENTATION_DEBUG_TOP_BOTTOM
    {
        const char* colors[] = { "aqua", "black", "blue", "fuchsia", "gray", "green", "lime", "maroon", "navy", "olive", "purple", "red", "silver", "teal", "yellow" };
        static int iRun = 0;
        for (size_t layer_id = 0; layer_id < zs.size(); ++layer_id) {
            std::vector<std::pair<Slic3r::ExPolygons, SVG::ExPolygonAttributes>> svg;
            for (size_t extruder_idx = 0; extruder_idx < num_extruders; ++ extruder_idx) {
                if (! top_raw[extruder_idx].empty() && ! top_raw[extruder_idx][layer_id].empty())
                    if (ExPolygons expoly = union_ex(top_raw[extruder_idx][layer_id]); ! expoly.empty()) {
                        const char *color = colors[extruder_idx];
                        svg.emplace_back(expoly, SVG::ExPolygonAttributes{ format("top%d", extruder_idx), color, color, color });
                    }
                if (! bottom_raw[extruder_idx].empty() && ! bottom_raw[extruder_idx][layer_id].empty())
                    if (ExPolygons expoly = union_ex(bottom_raw[extruder_idx][layer_id]); ! expoly.empty()) {
                        const char *color = colors[extruder_idx + 8];
                        svg.emplace_back(expoly, SVG::ExPolygonAttributes{ format("bottom%d", extruder_idx), color, color, color });
                    }
            }
            SVG::export_expolygons(debug_out_path("mm-segmentation-top-bottom-%d-%d-%lf.svg", iRun, layer_id, zs[layer_id]), svg);
        }
        ++ iRun;
    }
#endif // MM_SEGMENTATION_DEBUG_TOP_BOTTOM

    // When the upper surface of an object is occluded, it should no longer be considered the upper surface
    {
        for (size_t extruder_idx = 0; extruder_idx < num_facets_states; ++extruder_idx) {
            for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
                if (!top_raw[extruder_idx].empty() && !top_raw[extruder_idx][layer_idx].empty() && layer_idx + 1 < layers.size()) {
                    top_raw[extruder_idx][layer_idx] = diff(top_raw[extruder_idx][layer_idx], input_expolygons[layer_idx + 1]);
                }
                if (!bottom_raw[extruder_idx].empty() && !bottom_raw[extruder_idx][layer_idx].empty() && layer_idx > 0) {
                    bottom_raw[extruder_idx][layer_idx] = diff(bottom_raw[extruder_idx][layer_idx], input_expolygons[layer_idx - 1]);
                }
            }
        }
    }

    std::vector<std::vector<ExPolygons>> triangles_by_color_bottom(num_facets_states);
    std::vector<std::vector<ExPolygons>> triangles_by_color_top(num_facets_states);
    triangles_by_color_bottom.assign(num_facets_states, std::vector<ExPolygons>(num_layers * 2));
    triangles_by_color_top.assign(num_facets_states, std::vector<ExPolygons>(num_layers * 2));

    // BBS: use shell_triangles_by_color_bottom & shell_triangles_by_color_top to save the top and bottom embedded layers's color information
    std::vector<std::vector<ExPolygons>> shell_triangles_by_color_bottom(num_facets_states);
    std::vector<std::vector<ExPolygons>> shell_triangles_by_color_top(num_facets_states);
    shell_triangles_by_color_bottom.assign(num_facets_states, std::vector<ExPolygons>(num_layers * 2));
    shell_triangles_by_color_top.assign(num_facets_states, std::vector<ExPolygons>(num_layers * 2));

    // Fix-wave (wave-b-review.md Important 2): shadow of shell_triangles_by_color_top/bottom,
    // holding only the descent steps within top_shell_layers/bottom_shell_layers of the surface -
    // i.e. exactly what this same walk would deposit with paint_depth_normal_mm forced to 0. Same
    // double-buffer sizing/parity as the arrays above, since it is written at the identical
    // [color_idx][last_idx + layer_idx_offset] slots, just conditionally (a subset of the same
    // writes) - so the §C race-freedom argument for the arrays above extends to these unchanged.
    std::vector<std::vector<ExPolygons>> legacy_shell_triangles_by_color_bottom(num_facets_states);
    std::vector<std::vector<ExPolygons>> legacy_shell_triangles_by_color_top(num_facets_states);
    legacy_shell_triangles_by_color_bottom.assign(num_facets_states, std::vector<ExPolygons>(num_layers * 2));
    legacy_shell_triangles_by_color_top.assign(num_facets_states, std::vector<ExPolygons>(num_layers * 2));

    // Fix-wave 3 (absorb-tail-fixwave2-review.md I-1): LayerColorStat and its resolution logic
    // are now file-scope (compute_layer_color_stat, above) so segmentation_by_painting's own
    // absorb kill-width construction can call the exact same function - this is now a thin
    // capture-adapting wrapper, not the computation itself.
    auto layer_color_stat = [&layers = std::as_const(layers), &print_object, paint_depth_normal_mm](const size_t layer_idx, const size_t color_idx) -> LayerColorStat {
        return compute_layer_color_stat(layers, print_object, paint_depth_normal_mm, layer_idx, color_idx);
    };

    // WAVE B, hazard 2 completed. Widening `granularity` (above) is necessary for the double-buffer
    // parity trick but it is NOT sufficient, because the trick also assumes each TBB chunk begins
    // on a multiple of `granularity` - which blocked_range does not provide. blocked_range splits
    // at MIDPOINTS until a chunk is no larger than the grainsize, so its chunks are sized in
    // (granularity/2, granularity] and start wherever the halving lands: blocked_range(0, 32, 14)
    // yields [0,8) [8,16) [16,24) [24,32), and `range.begin() / granularity` labels the first TWO
    // of those as group 0. They are ADJACENT and same-parity, so layer 8's descent writes into
    // slots 0..7 of the very buffer the [0,8) chunk is concurrently writing - a data race, and one
    // that predates this wave (at the old grainsize of shell-1 = 5 the chunks come out size 4 and
    // collide the same way). Widening the write-back distance from 5 layers to 15 would have made
    // it far likelier to bite, so this wave closes it rather than inheriting it.
    //
    // Fix: iterate over the GROUPS themselves, so a group's layer range is exactly
    // [g*granularity, (g+1)*granularity) by construction. TBB may still hand several consecutive
    // groups to one task - that is sequential within the task and therefore safe - and the
    // invariant that matters holds unconditionally: a layer's descent reaches back at most
    // granularity layers, i.e. never past the previous group, which always has the OPPOSITE
    // parity. Same work, same per-layer body, same partition sizes; only the boundaries move.
    const size_t num_groups = (num_layers + size_t(granularity) - 1) / size_t(granularity);
    tbb::parallel_for(size_t(0), num_groups, [&granularity, &num_layers, &num_facets_states, &layer_color_stat, &top_raw, &triangles_by_color_top,
                                              &throw_on_cancel_callback, &input_expolygons, &bottom_raw, &triangles_by_color_bottom,
                                              &shell_triangles_by_color_top, &shell_triangles_by_color_bottom,
                                              &legacy_shell_triangles_by_color_top, &legacy_shell_triangles_by_color_bottom](const size_t group_idx) {
        size_t layer_idx_offset = (group_idx & 1) * num_layers;
        const size_t group_end  = std::min(num_layers, (group_idx + 1) * size_t(granularity));
        for (size_t layer_idx = group_idx * size_t(granularity); layer_idx < group_end; ++ layer_idx) {
            for (size_t color_idx = 0; color_idx < num_facets_states; ++color_idx) {
                throw_on_cancel_callback();
                LayerColorStat stat = layer_color_stat(layer_idx, color_idx);
                if (std::vector<Polygons> &top = top_raw[color_idx]; ! top.empty() && ! top[layer_idx].empty())
                    if (ExPolygons top_ex = union_ex(top[layer_idx]); ! top_ex.empty()) {
                        // Clean up thin projections. They are not printable anyways.
                        top_ex = opening_ex(top_ex, stat.small_region_threshold);
                        // Fix-wave N2 (.superpowers/sdd/2026-08-31-paint-depth/vertical-depth-
                        // fixwave-rereview.md): top_raw having geometry here only proves SOME
                        // region on the object has a nonzero top shell (the object-wide
                        // max_top_layers gate above that decides whether to run
                        // slice_mesh_slabs at all) - not that the region(s) actually present
                        // on THIS layer do. stat.top_shell_layers is the correct, layer-local
                        // answer (computed above from exactly the regions with real geometry
                        // on this layer); C1's contract is that a zero shell count claims
                        // nothing at all, not even the immediately-painted surface facet
                        // itself (LayerRegion.cpp:1025-1036 demotes it away from stTop). The
                        // descent loop below was already a no-op whenever
                        // stat.top_shell_layers == 0 (its own bound collapses to
                        // last_idx > layer_idx); this gate makes the surface claim just above
                        // it consistent with that same zero.
                        if (! top_ex.empty() && stat.top_shell_layers > 0) {
                            append(triangles_by_color_top[color_idx][layer_idx + layer_idx_offset], top_ex);
                            // TAPER BOUND (user decision; .superpowers/sdd/2026-08-31-paint-depth/
                            // taper-bound-report.md): "painted top/bottom claims keep FULL WIDTH for
                            // the solid-shell depth". See the exposed_surface_part() comment above
                            // for the erosion's verified purpose and why this split preserves it -
                            // in one line: the erosion is a perimeter-safety margin on INFERRED
                            // claims, and it is only load-bearing where the projected patch hugs its
                            // layer's contour, i.e. where the painted surface is steep.
                            //
                            // WAVE B / Option N (N1): where the claim is a normal-thickness shell,
                            // that split is retired - the full-width term becomes unconditional and
                            // F1's inset below is the sole enforcer of the perimeter invariant. See
                            // the function header for why the two are not both needed: the gate is a
                            // proxy ("reject steep surfaces") applied to the whole patch, F1 is the
                            // real invariant applied pointwise on the deposit layer's own contour,
                            // and F1 is what makes the extension self-suppress on steep geometry
                            // anyway. Keeping the proxy would only discard the slope-correct claim
                            // the descent already computes. Legacy behaviour is preserved verbatim
                            // wherever the extension is off (see normal_shell just below).
                            const float      wall_stack   = stat.extrusion_spacing + stat.extrusion_width;
                            const bool       normal_shell = stat.normal_shell;
                            const ExPolygons top_exposed_ex = normal_shell ? top_ex
                                                                           : exposed_surface_part(top_ex, input_expolygons, layer_idx + 1, num_layers, wall_stack);
                            // FLAT-TOP CAP (user decision 2026-09-01, .superpowers/sdd/
                            // 2026-08-31-paint-depth/flat-top-cap-report.md): on a FLAT (or
                            // near-flat) painted top the normal-thickness shell claims D deep
                            // (15 layers at stock defaults / 0.1mm layers) but only the solid
                            // top shell (stat.top_shell_layers, 6 layers at stock defaults) is
                            // ever VISIBLE - everything past it is hidden sparse infill of the
                            // SAME colour either way (paint_infill_override). Cap the FLAT part
                            // of the claim there; SLOPES AND WALLS MUST KEEP THE FULL D BOUND.
                            //
                            // Discriminator (FLAT-TOP CAP FIX WAVE, .superpowers/sdd/
                            // 2026-08-31-paint-depth/flat-top-cap-review.md I1/I2): a
                            // PER-COMPONENT verdict built on exposed_surface_part()'s own
                            // wall-stack yardstick - see flat_cap_component_ex()'s header above
                            // for the full derivation - computed fresh here UNCONDITIONALLY of
                            // normal_shell so it never touches top_exposed_ex itself (which N1
                            // deliberately made unconditional so slopes still reach D). A
                            // component whose flat core is at least 3 wall stacks wide (a ledge, a
                            // cap, a shallow dome crown) is capped WHOLE, rim included, whatever
                            // touches it (I1); a component that never had a core that wide (every
                            // slope this wave's T1/T2/T3 pin, 10-63deg, and every near-flat ring
                            // below the SAME ~2.2deg floor at 0.1mm layers) is EMPTY and the cap
                            // below is a no-op by construction on the WHOLE component, never a
                            // partial per-point split (I2) - no new angle constant either way. On
                            // a patch that is flat in the middle and rolls over to steep at its
                            // rim (a dome crown), the split is still POINTWISE between components,
                            // exactly like exposed_surface_part's own documented behaviour: the
                            // crown is capped, the rim (rolling into the flank) is not.
                            //
                            // Only worth computing when D actually reaches deeper than the shell
                            // - if it doesn't, there is nothing beyond stat.top_shell_layers to
                            // cap and this would be wasted Clipper work.
                            const bool       top_cap_active = normal_shell && stat.top_descent_layers > stat.top_shell_layers;
                            const ExPolygons top_flat_cap_ex = top_cap_active
                                ? flat_cap_component_ex(top_ex, input_expolygons, layer_idx + 1, num_layers, wall_stack)
                                : ExPolygons{};
                            float offset = 0.f;
                            // Option N (N3): on a slope the full-width term is empty for the NEAR
                            // descent steps - the ring deposited m layers down sits at inset
                            // [m*r, (m+1)*r] and F1 holds it one wall stack clear, so nothing
                            // survives until (m+1)*r > wall_stack (m >= 2 at 15 deg / 0.1mm layers).
                            // The legacy eroded term is empty there too. So the loop's
                            // `if (last.empty()) break;` would fire at step 1 and this whole change
                            // would be a silent no-op. Instead: skip empty steps until the descent
                            // has actually deposited something, then break at the first empty step
                            // as before. The reach (m+1)*r grows monotonically with m, so "empty
                            // before, non-empty after" is the only ordering that occurs and no
                            // productive step is ever skipped; termination is the loop bound either
                            // way. Deliberately keyed on "deposited" (post-opening) rather than on
                            // the raw term, so the marginal first surviving strip being eaten by
                            // opening_ex does not truncate the descent one step early.
                            bool deposited = false;
                            ExPolygons layer_slices_trimmed = input_expolygons[layer_idx];
                            for (int last_idx = int(layer_idx) - 1; last_idx > std::max(int(layer_idx - stat.top_descent_layers), int(0)); --last_idx) {
                                //BBS: offset width should be 2*spacing to avoid too narrow area which has overlap of wall line
                                //offset -= stat.extrusion_width ;
                                offset -= (stat.extrusion_spacing + stat.extrusion_width);
                                layer_slices_trimmed = intersection_ex(layer_slices_trimmed, input_expolygons[last_idx]);
                                ExPolygons last = intersection_ex(top_ex, offset_ex(layer_slices_trimmed, offset));
                                if (! top_exposed_ex.empty()) {
                                    // The near-horizontal part of the patch descends at FULL width -
                                    // trimmed by the running intersection of the layer outlines (the
                                    // containment guard) and by ONE wall stack of clearance from this
                                    // descent layer's own contour, never by the growing `offset`.
                                    //
                                    // FIX-WAVE F1 (.superpowers/sdd/2026-08-31-paint-depth/
                                    // outward-bleed-investigation.md, section 4 Option A). Without
                                    // the offset_ex() below this term is bounded only by a layer
                                    // OUTLINE, so wherever the painted patch reaches the silhouette -
                                    // every painted flat or chamfered cap, because
                                    // exposed_surface_part()'s early return (:1331-1332) hands such a
                                    // patch back with no clearance test at all - the claim reaches
                                    // the contour EXACTLY on every sub-surface shell layer. That
                                    // paints the exterior perimeter of a 0.7-1.0mm ring of side wall
                                    // the user never painted, on every painted flat-topped object.
                                    // It is a regression this branch introduced in 65d17c964f:
                                    // 3448111acd:1570-1578 and the pre-feature merge-base
                                    // f1e9f78696:1388-1396 are byte-identical to each other and inset
                                    // the claim by k * wall_stack unconditionally, so an exterior
                                    // perimeter could never be painted from an INFERRED layer.
                                    //
                                    // The inset is measured from input_expolygons[last_idx] - the
                                    // descent layer's own contour - not from the patch, and it is
                                    // CONSTANT rather than growing with depth. Three consequences,
                                    // all wanted:
                                    //   - an interior painted feature (a raised boss, a flat shelf,
                                    //     the user's 8mm eye/cheek) sits well inside the silhouette,
                                    //     so this never clips it: it keeps its ENTIRE footprint at
                                    //     every shell layer, which is what 65d17c964f was for;
                                    //   - it is still strictly more generous than legacy, whose inset
                                    //     grew as k * wall_stack (4.4mm by depth 5 at 0.1mm layers,
                                    //     against a constant 0.88mm here);
                                    //   - measuring at last_idx rather than at layer_idx is what
                                    //     makes it correct for objects that NARROW downward (an
                                    //     undercut, a waist, an overhang below a painted top), where
                                    //     a patch clear of the surface layer's contour can still land
                                    //     on a lower layer's contour.
                                    //
                                    // The surface layer itself is untouched: it is appended
                                    // separately above with zero margin, which is right, because that
                                    // is the facet the user actually painted.
                                    //
                                    // This also RETIRES the I1 sub-wall-stack absorb that used to sit
                                    // here (and at the bottom twin), which existed to enforce
                                    // "base material at the contour is either nothing or at least one
                                    // wall stack wide" by ABSORBING thin base rings into the claim.
                                    // With this inset that invariant holds by construction - `last`
                                    // is a subset of the contour eroded by one wall stack on both the
                                    // legacy term (inset k * wall_stack, k >= 1) and this one - so
                                    // the absorb's own diff_ex(base_rest, opening_ex(base_rest,
                                    // 0.5 * wall_stack)) is empty by construction. It is not merely
                                    // redundant but actively unsafe to keep: it would sit exactly on
                                    // the knife-edge where Clipper's arc approximation decides
                                    // whether a precisely-one-wall-stack ring survives its opening,
                                    // and any residue it found would be appended straight back onto
                                    // the contour - the very bleed this fix removes. One invariant,
                                    // one mechanism.
                                    const ExPolygons reachable = intersection_ex(top_exposed_ex, layer_slices_trimmed);
                                    // Option N: layer_slices_trimmed only ever shrinks, and under
                                    // the extension top_exposed_ex IS top_ex, so once the patch has
                                    // no overlap with the running intersection of the layer outlines
                                    // nothing at this depth or below can be claimed by either term -
                                    // the one place the relaxed break above can still terminate
                                    // early, and the reason a tall object does not pay M-1 empty
                                    // Clipper rounds once the descent has run off its own geometry.
                                    if (normal_shell && reachable.empty())
                                        break;
                                    append(last, intersection_ex(reachable, offset_ex(input_expolygons[last_idx], -wall_stack)));
                                    last = union_ex(last);
                                }
                                // FLAT-TOP CAP: once this descent step is deeper than the legacy
                                // solid-shell bound, the FLAT portion of the whole deposit (both
                                // the eroded term above and the exposed/full-width term just
                                // appended - subtracting from `last` as a whole, not merely from
                                // the full-width contribution, is what stops a WIDE flat cap's
                                // eroded term from continuing to shrink-and-deposit all the way to
                                // the D-driven depth) is removed. The sloped remainder (if any - a
                                // rolled-over rim) is untouched and keeps descending to the full D
                                // bound below, exactly like today.
                                if (top_cap_active && ! top_flat_cap_ex.empty() && int(layer_idx) - last_idx >= stat.top_shell_layers)
                                    last = diff_ex(last, top_flat_cap_ex);
                                // ITEM 2 (interclaim-absorb-report.md / interclaim-sliver-
                                // investigation.md section 5, Option 2): legacy behaviour
                                // (normal_shell == false, which is EVERY layer/colour in unlimited
                                // mode - paint_depth_normal_mm == 0 there - and any layer/colour
                                // below the D >= wall_stack gate even in bounded mode) keeps the
                                // PER-STEP opening here, byte-identical to before. Under the WAVE B
                                // extension (normal_shell == true) the opening moves to the
                                // ACCUMULATED band in the merge loop below (shell_triangles_by_
                                // color_top's consumer, the ITEM 2 site there) instead: rings
                                // deposited by DIFFERENT originating surface layers can land in the
                                // SAME destination slot at adjacent insets and form a contiguous
                                // band that is never actually thin, even where any one ring alone
                                // is - opening each ring here individually would punch holes in
                                // that band before it ever gets to overlap. Termination re-keys onto
                                // the RAW (un-opened) term below in this branch - deliberately, see
                                // the investigation's "why ranked second" - `deposited` still guards
                                // the initial F1 warm-up region exactly as before either way.
                                if (! normal_shell)
                                    last = opening_ex(last, stat.small_region_threshold);
                                if (last.empty()) {
                                    if (normal_shell && ! deposited)
                                        continue;
                                    break;
                                }
                                deposited = true;
                                // Fix-wave (wave-b-review.md Important 2): this step is also within
                                // the LEGACY (pre-Wave-B) shell depth whenever it is fewer than
                                // top_shell_layers steps from the surface - copy it (not move; the
                                // move below still needs it) into the legacy shadow before it goes.
                                if (int(layer_idx) - last_idx < stat.top_shell_layers)
                                    append(legacy_shell_triangles_by_color_top[color_idx][last_idx + layer_idx_offset], last);
                                append(shell_triangles_by_color_top[color_idx][last_idx + layer_idx_offset], std::move(last));
                            }
                        }
                    }
                if (std::vector<Polygons> &bottom = bottom_raw[color_idx]; ! bottom.empty() && ! bottom[layer_idx].empty())
                    if (ExPolygons bottom_ex = union_ex(bottom[layer_idx]); ! bottom_ex.empty()) {
                        // Clean up thin projections. They are not printable anyways.
                        bottom_ex = opening_ex(bottom_ex, stat.small_region_threshold);
                        // TAPER BOUND, symmetry fix: the vertical-depth fix wave gated the TOP
                        // surface claim on stat.top_shell_layers > 0 (fix-wave N2, see its comment
                        // above) but deliberately left this, the symmetric BOTTOM site, ungated and
                        // recorded it as a known residual. The reasoning is identical in every
                        // respect - bottom_raw having geometry here only proves SOME region on the
                        // object has a nonzero bottom shell (the object-wide max_bottom_layers gate),
                        // not that the region(s) actually present on THIS layer do;
                        // stat.bottom_shell_layers is the layer-local answer, and C1's contract is
                        // that a zero shell count claims nothing at all, not even the
                        // immediately-painted surface facet (LayerRegion.cpp:1025-1036 demotes
                        // stBottom to stInternal/stInternalVoid exactly as it demotes stTop). The
                        // descent loop below was likewise already a no-op at zero (its bound
                        // collapses to last_idx < layer_idx). Top and bottom are now symmetric.
                        if (! bottom_ex.empty() && stat.bottom_shell_layers > 0) {
                            append(triangles_by_color_bottom[color_idx][layer_idx + layer_idx_offset], bottom_ex);
                            // TAPER BOUND: mirror of the top claim above - see that comment and
                            // exposed_surface_part(). The reference layer is the one BELOW
                            // (layer_idx - 1), the same one the bottom occlusion trim uses.
                            // WAVE B / Option N, mirrored - see the top loop for the reasoning behind
                            // every one of these (N1's retired gate, the D >= wall_stack condition,
                            // N3's relaxed break and its `reachable` early-out).
                            const float      wall_stack   = stat.extrusion_spacing + stat.extrusion_width;
                            const bool       normal_shell = stat.normal_shell;
                            const ExPolygons bottom_exposed_ex = normal_shell ? bottom_ex
                                                                              : exposed_surface_part(bottom_ex, input_expolygons, layer_idx - 1, num_layers, wall_stack);
                            // FLAT-TOP CAP, mirrored - see the top loop's comment above for the
                            // full reasoning (flat_cap_component_ex()'s per-component I1/I2 fix).
                            const bool       bottom_cap_active = normal_shell && stat.bottom_descent_layers > stat.bottom_shell_layers;
                            const ExPolygons bottom_flat_cap_ex = bottom_cap_active
                                ? flat_cap_component_ex(bottom_ex, input_expolygons, layer_idx - 1, num_layers, wall_stack)
                                : ExPolygons{};
                            float offset = 0.f;
                            bool  deposited = false;
                            ExPolygons layer_slices_trimmed = input_expolygons[layer_idx];
                            for (size_t last_idx = layer_idx + 1; last_idx < std::min(layer_idx + stat.bottom_descent_layers, num_layers); ++last_idx) {
                                //BBS: offset width should be 2*spacing to avoid too narrow area which has overlap of wall line
                                //offset -= stat.extrusion_width;
                                offset -= (stat.extrusion_spacing + stat.extrusion_width);
                                layer_slices_trimmed = intersection_ex(layer_slices_trimmed, input_expolygons[last_idx]);
                                ExPolygons last = intersection_ex(bottom_ex, offset_ex(layer_slices_trimmed, offset));
                                if (! bottom_exposed_ex.empty()) {
                                    // FIX-WAVE F1, mirrored - see the top loop's comment above for the
                                    // full reasoning, and note the same site swap: the one-wall-stack
                                    // clearance from this descent layer's own contour REPLACES the I1
                                    // absorb that used to follow, which the clearance makes empty by
                                    // construction.
                                    const ExPolygons reachable = intersection_ex(bottom_exposed_ex, layer_slices_trimmed);
                                    if (normal_shell && reachable.empty())
                                        break;
                                    append(last, intersection_ex(reachable, offset_ex(input_expolygons[last_idx], -wall_stack)));
                                    last = union_ex(last);
                                }
                                // FLAT-TOP CAP, mirrored - see the top loop's comment above.
                                if (bottom_cap_active && ! bottom_flat_cap_ex.empty() && last_idx - layer_idx >= size_t(stat.bottom_shell_layers))
                                    last = diff_ex(last, bottom_flat_cap_ex);
                                // ITEM 2, mirrored - see the top loop's comment above.
                                if (! normal_shell)
                                    last = opening_ex(last, stat.small_region_threshold);
                                if (last.empty()) {
                                    if (normal_shell && ! deposited)
                                        continue;
                                    break;
                                }
                                deposited = true;
                                // Fix-wave (wave-b-review.md Important 2), mirrored - see the top
                                // loop's comment above.
                                if (last_idx - layer_idx < size_t(stat.bottom_shell_layers))
                                    append(legacy_shell_triangles_by_color_bottom[color_idx][last_idx + layer_idx_offset], last);
                                append(shell_triangles_by_color_bottom[color_idx][last_idx + layer_idx_offset], std::move(last));
                            }
                        }
                    }
            }
        }
    });

    std::vector<std::vector<ExPolygons>> triangles_by_color_merged(num_facets_states);
    triangles_by_color_merged.assign(num_facets_states, std::vector<ExPolygons>(num_layers));
    // Fix-wave (wave-b-review.md Important 2): sized identically to triangles_by_color_merged, and
    // filled below from the SAME per-colour arrays, just before the moves that consume the surface
    // ones - see the function header comment.
    legacy_top_and_bottom_layers_out.assign(num_facets_states, std::vector<ExPolygons>(num_layers));
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&triangles_by_color_merged, &triangles_by_color_bottom, &triangles_by_color_top, &num_layers, &throw_on_cancel_callback,
                                                                  &shell_triangles_by_color_top, &shell_triangles_by_color_bottom,
                                                                  &legacy_shell_triangles_by_color_top, &legacy_shell_triangles_by_color_bottom,
                                                                  &legacy_top_and_bottom_layers_out, &layer_color_stat](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
            throw_on_cancel_callback();
            // Fix-wave (wave-b-review.md Important 2): capture the legacy-bounded reach BEFORE the
            // moves below consume triangles_by_color_top/bottom (the surface-layer contributions,
            // shared identically between legacy and full - see the function header comment). Not
            // yet trimmed against other colours: merge_segmented_layers intersects/diffs it against
            // the fully-resolved claim itself, so no priority resolution needs to be repeated here.
            for (size_t color_idx = 0; color_idx < legacy_top_and_bottom_layers_out.size(); ++color_idx) {
                ExPolygons &reach = legacy_top_and_bottom_layers_out[color_idx][layer_idx];
                append(reach, triangles_by_color_top[color_idx][layer_idx]);
                append(reach, triangles_by_color_top[color_idx][layer_idx + num_layers]);
                append(reach, triangles_by_color_bottom[color_idx][layer_idx]);
                append(reach, triangles_by_color_bottom[color_idx][layer_idx + num_layers]);
                append(reach, legacy_shell_triangles_by_color_top[color_idx][layer_idx]);
                append(reach, legacy_shell_triangles_by_color_top[color_idx][layer_idx + num_layers]);
                append(reach, legacy_shell_triangles_by_color_bottom[color_idx][layer_idx]);
                append(reach, legacy_shell_triangles_by_color_bottom[color_idx][layer_idx + num_layers]);
                if (! reach.empty()) {
                    reach = union_ex(reach);
                    // Fix-wave 2 (absorb-tail-fixwave2-review.md I-A): the legacy_shell_triangles_
                    // by_color_top/bottom contributions folded in above are the RAW `last` term
                    // from the descent loop whenever normal_shell is true (that loop skips its own
                    // per-step opening_ex exactly then, deferring to the band-level opening below
                    // instead - see this function's ITEM 2 comment at the band-merge site) - so
                    // `reach` can carry un-opened, sub-small_region_threshold ring fragments that
                    // `full`'s own band-level opening already filters out of the claim proper.
                    // merge_segmented_layers's cross-colour clip only ever SUBTRACTS from `full`
                    // (`full \ (excess ∩ other)`), so it cannot itself manufacture new area - but
                    // the region it deliberately EXEMPTS from that subtraction is `full ∩ legacy`,
                    // and where `legacy` still carries one of these un-opened raw fingers, that
                    // exempted intersection can still be a thin sliver of `full`'s own (already-
                    // wide) shape, cut out by the OTHER colour's boundary crossing through it.
                    // Opening `reach` here, with the SAME small_region_threshold `full`'s own
                    // band-level opening already applies (both are per LAYER and per COLOUR, so
                    // this is the identical quantity, not merely a similar one), stops the shadow
                    // from ever carrying that raw finger in the first place - it can no longer
                    // shape the final claim at all, not merely no longer add to it. A no-op
                    // wherever normal_shell is false (every layer/colour in unlimited mode, and
                    // color_idx 0 always - LayerColorStat's own gate), so legacy behaviour is
                    // untouched there.
                    if (const LayerColorStat stat = layer_color_stat(layer_idx, color_idx); stat.normal_shell)
                        reach = opening_ex(reach, stat.small_region_threshold);
                }
            }
            ExPolygons painted_exploys;
            for (size_t color_idx = 0; color_idx < triangles_by_color_merged.size(); ++color_idx) {
                auto &self = triangles_by_color_merged[color_idx][layer_idx];
                append(self, std::move(triangles_by_color_bottom[color_idx][layer_idx]));
                append(self, std::move(triangles_by_color_bottom[color_idx][layer_idx + num_layers]));
                append(self, std::move(triangles_by_color_top[color_idx][layer_idx]));
                append(self, std::move(triangles_by_color_top[color_idx][layer_idx + num_layers]));
                self = union_ex(self);

                append(painted_exploys, self);
            }

            painted_exploys = union_ex(painted_exploys);

            //BBS: merge the top and bottom shell layers
            for (size_t color_idx = 0; color_idx < triangles_by_color_merged.size(); ++color_idx) {
                auto &self = triangles_by_color_merged[color_idx][layer_idx];

                ExPolygons top_band    = union_ex(shell_triangles_by_color_top[color_idx][layer_idx],
                                                  shell_triangles_by_color_top[color_idx][layer_idx + num_layers]);
                ExPolygons bottom_band = union_ex(shell_triangles_by_color_bottom[color_idx][layer_idx],
                                                  shell_triangles_by_color_bottom[color_idx][layer_idx + num_layers]);

                // ITEM 2 (interclaim-absorb-report.md / interclaim-sliver-investigation.md
                // section 5, Option 2): this destination layer's accumulated band, unioned across
                // every originating surface layer that deposited into it - the counterpart to the
                // descent loops above skipping their PER-STEP opening whenever normal_shell is
                // true. Recomputed here (not carried down from the descent loops) because
                // small_region_threshold/normal_shell are per LAYER *and* per COLOUR, and this is
                // the destination layer, generally a different layer than any one contribution's
                // origin. Gated on the SAME normal_shell condition the rest of Wave B/Option N
                // uses (paint_depth_normal_mm > 0 && color_idx > 0 && D >= wall_stack): false for
                // EVERY layer/colour in unlimited mode (paint_depth_normal_mm == 0), so this is a
                // no-op there and the legacy per-step-opened contributions pass through unchanged,
                // exactly byte-identical to before. Where it IS true, the per-step opening was
                // skipped above, so this is the only point these contributions are filtered at
                // all - band-level, once, instead of once per contributing origin layer.
                const LayerColorStat stat = layer_color_stat(layer_idx, color_idx);
                if (stat.normal_shell) {
                    top_band    = opening_ex(top_band, stat.small_region_threshold);
                    bottom_band = opening_ex(bottom_band, stat.small_region_threshold);
                }

                auto top_area    = diff_ex(top_band, painted_exploys);
                auto bottom_area = diff_ex(bottom_band, painted_exploys);

                append(self, top_area);
                append(self, bottom_area);
                self = union_ex(self);
            }
            // Trim one region by the other if some of the regions overlap.
            ExPolygons painted_regions;
            for (size_t color_idx = 1; color_idx < triangles_by_color_merged.size(); ++color_idx) {
                triangles_by_color_merged[color_idx][layer_idx] = diff_ex(triangles_by_color_merged[color_idx][layer_idx], painted_regions);
                append(painted_regions, triangles_by_color_merged[color_idx][layer_idx]);
            }
            triangles_by_color_merged[0][layer_idx] = diff_ex(triangles_by_color_merged[0][layer_idx], painted_regions);
        }
    });
    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - Segmentation of top and bottom layers in parallel - End";

    return triangles_by_color_merged;
}

// For every ColoredLine in lines_colored_out, assign the index of the polygon to which belongs and also the index of this line inside of the polygon.
static inline void init_polygon_indices(const MMU_Graph &graph, const std::vector<std::vector<ColoredLine>> &color_poly, std::vector<ColoredLine> &lines_colored_out)
{
    size_t poly_idx = 0;
    for (const std::vector<ColoredLine> &color_lines : color_poly) {
        size_t line_idx = 0;
        for (size_t color_line_idx = 0; color_line_idx < color_lines.size(); ++color_line_idx) {
            size_t from_idx                            = graph.get_global_index(poly_idx, line_idx);
            lines_colored_out[from_idx].poly_idx       = int(poly_idx);
            lines_colored_out[from_idx].local_line_idx = int(line_idx);
            ++line_idx;
        }
        ++poly_idx;
    }
}

static inline bool line_intersection_with_epsilon(const Line &line_to_extend, const Line &other, Point *intersection)
{
    Line extended_line = line_to_extend;
    extended_line.extend(15 * SCALED_EPSILON);
    return extended_line.intersection(other, intersection);
}

static inline void mark_processed(const voronoi_diagram<double>::const_edge_iterator &edge_iterator)
{
    edge_iterator->color(true);
    edge_iterator->twin()->color(true);
}

static inline bool is_point_closer_to_beginning_of_line(const Line &line, const Point &p)
{
    return (p - line.a).cast<double>().squaredNorm() < (p - line.b).cast<double>().squaredNorm();
}

static inline Line clip_finite_voronoi_edge(const Voronoi::VD::edge_type &edge, const BoundingBoxf &bbox)
{
    assert(edge.is_finite());
    Vec2d v0          = mk_vec2(edge.vertex0());
    Vec2d v1          = mk_vec2(edge.vertex1());
    bool  contains_v0 = bbox.contains(v0);
    bool  contains_v1 = bbox.contains(v1);
    if ((contains_v0 && contains_v1) || (!contains_v0 && !contains_v1)) return {mk_point(edge.vertex0()), mk_point(edge.vertex1())};

    Vec2d vector = (v1 - v0).normalized() * bbox.size().norm();
    if (!contains_v0)
        v0 = (v1 - vector);
    else
        v1 = (v0 + vector);

    return {v0.cast<coord_t>(), v1.cast<coord_t>()};
}

static inline bool has_same_color(const ColoredLine &cl1, const ColoredLine &cl2) { return cl1.color == cl2.color; }

static MMU_Graph build_graph(size_t layer_idx, const std::vector<std::vector<ColoredLine>> &color_poly)
{
    const Polygons color_poly_tmp = colored_points_to_polygon(color_poly);
    const Points   points         = to_points(color_poly_tmp);
    const Lines    lines          = to_lines(color_poly_tmp);

    // The algorithm adds edges to the graph that are between two different colors.
    // If a polygon is colored entirely with one color, we need to add at least one edge from that polygon artificially.
    // Adding this edge is necessary for cases where the expolygon has an outer contour colored whole with one color
    // and a hole colored with a different color. If an edge wasn't added to the graph,
    // the entire expolygon would be colored with single random color instead of two different.
    std::vector<bool> force_edge_adding(color_poly.size());

    // For each polygon, check if it is all colored with the same color. If it is, we need to force adding one edge to it.
    for (const std::vector<ColoredLine> &c_poly : color_poly) {
        bool force_edge = true;
        for (const ColoredLine &c_line : c_poly)
            if (c_line.color != c_poly.front().color) {
                force_edge = false;
                break;
            }
        force_edge_adding[&c_poly - &color_poly.front()] = force_edge;
    }

    ColoredLines       lines_colored = to_lines(color_poly);
    const ColoredLines colored_lines = lines_colored;

    Voronoi::VD vd;
    vd.construct_voronoi(colored_lines.begin(), colored_lines.end());
    // boost::polygon::construct_voronoi(lines_colored.begin(), lines_colored.end(), &vd);
    MMU_Graph graph;
    graph.nodes.reserve(points.size() + vd.vertices().size());
    for (const Point &point : points) graph.nodes.push_back({Vec2d(double(point.x()), double(point.y()))});

    graph.add_contours(color_poly);
    init_polygon_indices(graph, color_poly, lines_colored);

    assert(graph.nodes.size() == lines_colored.size());
    BoundingBox bbox = get_extents(color_poly_tmp);
    graph.append_voronoi_vertices(vd, color_poly_tmp, bbox);

    auto get_prev_contour_line = [&lines_colored, &color_poly, &graph](const voronoi_diagram<double>::const_edge_iterator &edge_it) -> ColoredLine {
        size_t contour_line_local_idx = lines_colored[edge_it->cell()->source_index()].local_line_idx;
        size_t contour_line_size      = color_poly[lines_colored[edge_it->cell()->source_index()].poly_idx].size();
        size_t contour_prev_idx       = graph.get_global_index(lines_colored[edge_it->cell()->source_index()].poly_idx,
                                                         (contour_line_local_idx > 0) ? contour_line_local_idx - 1 : contour_line_size - 1);
        return lines_colored[contour_prev_idx];
    };

    auto get_next_contour_line = [&lines_colored, &color_poly, &graph](const voronoi_diagram<double>::const_edge_iterator &edge_it) -> ColoredLine {
        size_t contour_line_local_idx = lines_colored[edge_it->cell()->source_index()].local_line_idx;
        size_t contour_line_size      = color_poly[lines_colored[edge_it->cell()->source_index()].poly_idx].size();
        size_t contour_next_idx       = graph.get_global_index(lines_colored[edge_it->cell()->source_index()].poly_idx, (contour_line_local_idx + 1) % contour_line_size);
        return lines_colored[contour_next_idx];
    };

    bbox.offset(scale_(10.));
    const BoundingBoxf bbox_clip(bbox.min.cast<double>(), bbox.max.cast<double>());
    const double       bbox_dim_max = double(std::max(bbox.size().x(), bbox.size().y()));

    // Make a copy of the input segments with the double type.
    std::vector<Voronoi::Internal::segment_type> segments;
    for (const Line &line : lines)
        segments.emplace_back(Voronoi::Internal::point_type(double(line.a(0)), double(line.a(1))), Voronoi::Internal::point_type(double(line.b(0)), double(line.b(1))));

    for (auto edge_it = vd.edges().begin(); edge_it != vd.edges().end(); ++edge_it) {
        // Skip second half-edge
        if (edge_it->cell()->source_index() > edge_it->twin()->cell()->source_index() || edge_it->color()) continue;

        if (edge_it->is_infinite() && (edge_it->vertex0() != nullptr || edge_it->vertex1() != nullptr)) {
            // Infinite edge is leading through a point on the counter, but there are no Voronoi vertices.
            // So we could fix this case by computing the intersection between the contour line and infinity edge.
            std::vector<Voronoi::Internal::point_type> samples;
            Voronoi::Internal::clip_infinite_edge(points, segments, *edge_it, bbox_dim_max, &samples);
            if (samples.empty()) continue;

            const Line         edge_line(mk_point(samples[0]), mk_point(samples[1]));
            const ColoredLine &contour_line = lines_colored[edge_it->cell()->source_index()];
            Point              contour_intersection;

            if (line_intersection_with_epsilon(contour_line.line, edge_line, &contour_intersection)) {
                const MMU_Graph::Arc &graph_arc = graph.get_border_arc(edge_it->cell()->source_index());
                const size_t          from_idx  = (edge_it->vertex1() != nullptr) ? edge_it->vertex1()->color() : edge_it->vertex0()->color();
                size_t                to_idx    = ((contour_line.line.a - contour_intersection).cast<double>().squaredNorm() <
                                 (contour_line.line.b - contour_intersection).cast<double>().squaredNorm()) ?
                                                      graph_arc.from_idx :
                                                      graph_arc.to_idx;
                if (from_idx != to_idx && from_idx < graph.nodes_count() && to_idx < graph.nodes_count()) {
                    graph.append_edge(from_idx, to_idx);
                    mark_processed(edge_it);
                }
            }
        } else if (edge_it->is_finite()) {
            // Both points are on contour, so skip them. In cases of duplicate Voronoi vertices, skip edges between the same two points.
            if (graph.is_edge_connecting_two_contour_vertices(edge_it) || (edge_it->vertex0()->color() == edge_it->vertex1()->color())) continue;

            const Line        edge_line         = clip_finite_voronoi_edge(*edge_it, bbox_clip);
            const Line        contour_line      = lines_colored[edge_it->cell()->source_index()].line;
            const ColoredLine colored_line      = lines_colored[edge_it->cell()->source_index()];
            const ColoredLine contour_line_prev = get_prev_contour_line(edge_it);
            const ColoredLine contour_line_next = get_next_contour_line(edge_it);

            if (edge_it->vertex0()->color() >= graph.nodes_count() || edge_it->vertex1()->color() >= graph.nodes_count()) {
                enum class Vertex { VERTEX0, VERTEX1 };
                auto append_edge_if_intersects_with_contour = [&graph, &lines_colored, &edge_line,
                                                               &contour_line](const voronoi_diagram<double>::const_edge_iterator &edge_iterator, const Vertex vertex) {
                    Point intersection;
                    Line  contour_line_twin = lines_colored[edge_iterator->twin()->cell()->source_index()].line;
                    if (line_intersection_with_epsilon(contour_line_twin, edge_line, &intersection)) {
                        const MMU_Graph::Arc &graph_arc = graph.get_border_arc(edge_iterator->twin()->cell()->source_index());
                        const size_t          to_idx_l  = is_point_closer_to_beginning_of_line(contour_line_twin, intersection) ? graph_arc.from_idx : graph_arc.to_idx;
                        graph.append_edge(vertex == Vertex::VERTEX0 ? edge_iterator->vertex0()->color() : edge_iterator->vertex1()->color(), to_idx_l);
                    } else if (line_intersection_with_epsilon(contour_line, edge_line, &intersection)) {
                        const MMU_Graph::Arc &graph_arc = graph.get_border_arc(edge_iterator->cell()->source_index());
                        const size_t          to_idx_l  = is_point_closer_to_beginning_of_line(contour_line, intersection) ? graph_arc.from_idx : graph_arc.to_idx;
                        graph.append_edge(vertex == Vertex::VERTEX0 ? edge_iterator->vertex0()->color() : edge_iterator->vertex1()->color(), to_idx_l);
                    }
                    mark_processed(edge_iterator);
                };

                if (edge_it->vertex0()->color() < graph.nodes_count() && !graph.is_vertex_on_contour(edge_it->vertex0()))
                    append_edge_if_intersects_with_contour(edge_it, Vertex::VERTEX0);

                if (edge_it->vertex1()->color() < graph.nodes_count() && !graph.is_vertex_on_contour(edge_it->vertex1()))
                    append_edge_if_intersects_with_contour(edge_it, Vertex::VERTEX1);
            } else if (graph.is_edge_attach_to_contour(edge_it)) {
                mark_processed(edge_it);
                // Skip edges witch connection two points on a contour
                if (graph.is_edge_connecting_two_contour_vertices(edge_it)) continue;

                const size_t from_idx = edge_it->vertex0()->color();
                const size_t to_idx   = edge_it->vertex1()->color();
                if (graph.is_vertex_on_contour(edge_it->vertex0())) {
                    if (is_point_closer_to_beginning_of_line(contour_line, edge_line.a)) {
                        if ((!has_same_color(contour_line_prev, colored_line) || force_edge_adding[colored_line.poly_idx]) &&
                            points_inside(contour_line_prev.line, contour_line, edge_line.b)) {
                            graph.append_edge(from_idx, to_idx);
                            force_edge_adding[colored_line.poly_idx] = false;
                        }
                    } else {
                        if ((!has_same_color(contour_line_next, colored_line) || force_edge_adding[colored_line.poly_idx]) &&
                            points_inside(contour_line, contour_line_next.line, edge_line.b)) {
                            graph.append_edge(from_idx, to_idx);
                            force_edge_adding[colored_line.poly_idx] = false;
                        }
                    }
                } else {
                    assert(graph.is_vertex_on_contour(edge_it->vertex1()));
                    if (is_point_closer_to_beginning_of_line(contour_line, edge_line.b)) {
                        if ((!has_same_color(contour_line_prev, colored_line) || force_edge_adding[colored_line.poly_idx]) &&
                            points_inside(contour_line_prev.line, contour_line, edge_line.a)) {
                            graph.append_edge(from_idx, to_idx);
                            force_edge_adding[colored_line.poly_idx] = false;
                        }
                    } else {
                        if ((!has_same_color(contour_line_next, colored_line) || force_edge_adding[colored_line.poly_idx]) &&
                            points_inside(contour_line, contour_line_next.line, edge_line.a)) {
                            graph.append_edge(from_idx, to_idx);
                            force_edge_adding[colored_line.poly_idx] = false;
                        }
                    }
                }
            } else if (Point intersection; line_intersection_with_epsilon(contour_line, edge_line, &intersection)) {
                mark_processed(edge_it);
                Vec2d real_v0_double = graph.nodes[edge_it->vertex0()->color()].point;
                Vec2d real_v1_double = graph.nodes[edge_it->vertex1()->color()].point;
                Point real_v0        = Point(coord_t(real_v0_double.x()), coord_t(real_v0_double.y()));
                Point real_v1        = Point(coord_t(real_v1_double.x()), coord_t(real_v1_double.y()));

                if (is_point_closer_to_beginning_of_line(contour_line, intersection)) {
                    Line first_part(intersection, real_v0);
                    Line second_part(intersection, real_v1);

                    if (!has_same_color(contour_line_prev, colored_line)) {
                        if (points_inside(contour_line_prev.line, contour_line, first_part.b))
                            graph.append_edge(edge_it->vertex0()->color(), graph.get_border_arc(edge_it->cell()->source_index()).from_idx);

                        if (points_inside(contour_line_prev.line, contour_line, second_part.b))
                            graph.append_edge(edge_it->vertex1()->color(), graph.get_border_arc(edge_it->cell()->source_index()).from_idx);
                    }
                } else {
                    const size_t int_point_idx    = graph.get_border_arc(edge_it->cell()->source_index()).to_idx;
                    const Vec2d  int_point_double = graph.nodes[int_point_idx].point;
                    const Point  int_point        = Point(coord_t(int_point_double.x()), coord_t(int_point_double.y()));

                    const Line first_part(int_point, real_v0);
                    const Line second_part(int_point, real_v1);

                    if (!has_same_color(contour_line_next, colored_line)) {
                        if (points_inside(contour_line, contour_line_next.line, first_part.b)) graph.append_edge(edge_it->vertex0()->color(), int_point_idx);

                        if (points_inside(contour_line, contour_line_next.line, second_part.b)) graph.append_edge(edge_it->vertex1()->color(), int_point_idx);
                    }
                }
            }
        }
    }

    for (auto edge_it = vd.edges().begin(); edge_it != vd.edges().end(); ++edge_it) {
        // Skip second half-edge and processed edges
        if (edge_it->cell()->source_index() > edge_it->twin()->cell()->source_index() || edge_it->color()) continue;

        if (edge_it->is_finite() && !bool(edge_it->color()) && edge_it->vertex0()->color() < graph.nodes_count() && edge_it->vertex1()->color() < graph.nodes_count()) {
            // Skip cases, when the edge is between two same vertices, which is in cases two near vertices were merged together.
            if (edge_it->vertex0()->color() == edge_it->vertex1()->color()) continue;

            size_t from_idx = edge_it->vertex0()->color();
            size_t to_idx   = edge_it->vertex1()->color();
            graph.append_edge(from_idx, to_idx);
        }
        mark_processed(edge_it);
    }

    graph.remove_nodes_with_one_arc();
    return graph;
}

static std::vector<std::vector<std::pair<size_t, size_t>>> get_all_segments(const std::vector<std::vector<ColoredLine>> &color_poly)
{
    std::vector<std::vector<std::pair<size_t, size_t>>> all_segments(color_poly.size());
    for (size_t poly_idx = 0; poly_idx < color_poly.size(); ++poly_idx) {
        const std::vector<ColoredLine> &c_polygon = color_poly[poly_idx];
        all_segments[poly_idx]                    = get_segments(c_polygon);
    }
    return all_segments;
}

static inline double compute_edge_length(const MMU_Graph &graph, const size_t start_idx, const size_t &start_arc_idx)
{
    assert(start_arc_idx < graph.arcs.size());
    std::vector<bool> used_arcs(graph.arcs.size(), false);

    used_arcs[start_arc_idx]                = true;
    const MMU_Graph::Arc *arc               = &graph.arcs[start_arc_idx];
    size_t                idx               = start_idx;
    double                line_total_length = (graph.nodes[arc->to_idx].point - graph.nodes[idx].point).norm();
    while (graph.nodes[arc->to_idx].arc_idxs.size() == 2) {
        bool found = false;
        for (const size_t &arc_idx : graph.nodes[arc->to_idx].arc_idxs) {
            if (const MMU_Graph::Arc &arc_n = graph.arcs[arc_idx]; arc_n.type == MMU_Graph::ARC_TYPE::NON_BORDER && !used_arcs[arc_idx] && arc_n.to_idx != idx) {
                Linef first_line(graph.nodes[idx].point, graph.nodes[arc->to_idx].point);
                Linef second_line(graph.nodes[arc->to_idx].point, graph.nodes[arc_n.to_idx].point);

                Vec2d  first_line_vec    = (first_line.a - first_line.b);
                Vec2d  second_line_vec   = (second_line.b - second_line.a);
                Vec2d  first_line_vec_n  = first_line_vec.normalized();
                Vec2d  second_line_vec_n = second_line_vec.normalized();
                double angle             = ::acos(std::clamp(first_line_vec_n.dot(second_line_vec_n), -1.0, 1.0));
                if (Slic3r::cross2(first_line_vec_n, second_line_vec_n) < 0.0) angle = 2.0 * (double) PI - angle;

                if (std::abs(angle - PI) >= (PI / 12)) continue;

                idx = arc->to_idx;
                arc = &arc_n;

                line_total_length += (graph.nodes[arc->to_idx].point - graph.nodes[idx].point).norm();
                used_arcs[arc_idx] = true;
                found              = true;
                break;
            }
        }
        if (!found) break;
    }

    return line_total_length;
}

static void remove_multiple_edges_in_vertices(MMU_Graph &graph, const std::vector<std::vector<ColoredLine>> &color_poly)
{
    std::vector<std::vector<std::pair<size_t, size_t>>> colored_segments = get_all_segments(color_poly);
    for (const std::vector<std::pair<size_t, size_t>> &colored_segment_p : colored_segments) {
        size_t poly_idx = &colored_segment_p - &colored_segments.front();
        for (const std::pair<size_t, size_t> &colored_segment : colored_segment_p) {
            size_t first_idx  = graph.get_global_index(poly_idx, colored_segment.first);
            size_t second_idx = graph.get_global_index(poly_idx, (colored_segment.second + 1) % graph.polygon_sizes[poly_idx]);
            Linef  seg_line(graph.nodes[first_idx].point, graph.nodes[second_idx].point);

            if (graph.nodes[first_idx].arc_idxs.size() >= 3) {
                std::vector<std::pair<MMU_Graph::Arc *, double>> arc_to_check;
                for (const size_t &arc_idx : graph.nodes[first_idx].arc_idxs) {
                    MMU_Graph::Arc &n_arc = graph.arcs[arc_idx];
                    if (n_arc.type == MMU_Graph::ARC_TYPE::NON_BORDER) {
                        double total_len = compute_edge_length(graph, first_idx, arc_idx);
                        arc_to_check.emplace_back(&n_arc, total_len);
                    }
                }
                std::sort(arc_to_check.begin(), arc_to_check.end(),
                          [](std::pair<MMU_Graph::Arc *, double> &l, std::pair<MMU_Graph::Arc *, double> &r) -> bool { return l.second > r.second; });

                while (arc_to_check.size() > 1) {
                    graph.remove_edge(first_idx, arc_to_check.back().first->to_idx);
                    arc_to_check.pop_back();
                }
            }
        }
    }
}

// ITEM 1 (interclaim-absorb-report.md / interclaim-sliver-investigation.md section 5,
// Option 1): given a thin, fully-interior base island and the final per-colour claims on its
// layer, returns the 1-based colour index that should absorb the island - the painted claim
// (index >= 1; index 0, the base colour, is never a candidate) with the LARGEST shared area
// against the island dilated by `eps`, ties broken by the LOWEST colour index. Returns 0 if no
// painted claim touches the (dilated) island at all - the caller must not orphan the island onto
// an arbitrary colour when that happens.
//
// Determinism (this codebase has been bitten by this before; corrected by absorb-tail-review.md
// Minor 1 - this comment previously and WRONGLY called ExPolygon::area() an "exact integer"
// shoelace sum): ExPolygon::area() is a DOUBLE computation - Polygon::area() (Polygon.cpp)
// casts each Point to Vec2d and accumulates via cross2(), ExPolygon::area() (ExPolygon.cpp) sums
// the contour and subtracts each hole, all in double. Individual terms are exact only while
// |coordinate| stays under ~2^26.5 scaled units (~95mm from the object's own origin - the
// operands here, island/claim intersections a few mm across, are nowhere near that), and the
// running sum is order-dependent, so this is NOT bit-for-bit "the same as integer arithmetic";
// it is deterministic for a REASON one level up: the SAME inputs, summed in the SAME order, by
// the SAME binary, produce the SAME double every run (no thread/iteration-order ambiguity - the
// candidate loop below is sequential, not parallel), so `area > best_area` (a plain double `>`)
// picks the same argmax every time, and the ascending scan with a STRICT `>` is what makes the
// LOWEST colour index win a genuine tie (two areas that compare bit-identical, e.g. exact
// mirror-symmetric geometry - the unit test below pins exactly this case), matching the
// precedence already established at the painted-colour trim loop further down (colour 1 beats
// colour 2 beats ...). A NEAR-tie on real (non-symmetric) geometry is decided by whichever
// double compares larger, i.e. by rounding, not by index - a real but Minor distinction from
// what "ties broken by lowest index" might suggest, since only a GENUINE (bit-exact) tie reaches
// the index rule at all. (Making this genuinely int64-exact was considered and rejected as not
// actually cheap: Clipper's own vendored Area(const Path&) - deps_src/clipper/clipper.cpp:167 -
// is ALSO double, computed the same way, so it is not a drop-in fix; a real fix would mean
// hand-rolling a new int64 doubled-shoelace accumulator, with its own overflow ceiling on
// pathological geometry, disproportionate to what is a comment-accuracy finding, not a
// behavioural one - the double comparison above IS deterministic, just not integer-exact.)
size_t interclaim_absorb_winner(const ExPolygons &island, const std::vector<ExPolygons> &painted_claims, float eps)
{
    const ExPolygons dilated = offset_ex(island, eps);
    size_t           best_color = 0;
    double           best_area  = 0.;
    for (size_t color_idx = 1; color_idx < painted_claims.size(); ++color_idx) {
        if (painted_claims[color_idx].empty())
            continue;
        const ExPolygons shared = intersection_ex(dilated, painted_claims[color_idx]);
        if (shared.empty())
            continue;
        double area = 0.;
        for (const ExPolygon &e : shared)
            area += e.area();
        if (best_color == 0 || area > best_area) {
            best_area  = area;
            best_color = color_idx;
        }
    }
    return best_color;
}

// Fix-wave (absorb-tail-review.md Minor 4 / M4) - see the header declaration for the full
// rationale. MAX over only the colours whose (eps-dilated, same adjacency test
// interclaim_absorb_winner uses) footprint actually touches this specific island; a colour
// that cannot raise the running max is skipped without paying the geometry test at all.
float interclaim_absorb_effective_claim_width(const ExPolygons &island, const std::vector<ExPolygons> &painted_claims,
                                               const std::vector<float> &claim_width_gapfill_off_by_color,
                                               float min_claim_width, float eps)
{
    ExPolygons dilated;
    bool       have_dilated = false;
    float      widened      = 0.f;
    for (size_t color_idx = 1; color_idx < painted_claims.size(); ++color_idx) {
        if (color_idx >= claim_width_gapfill_off_by_color.size())
            continue;
        const float candidate = claim_width_gapfill_off_by_color[color_idx];
        if (candidate <= widened || painted_claims[color_idx].empty())
            continue; // cannot raise the max even if it borders -> skip the geometry test
        if (! have_dilated) {
            dilated      = offset_ex(island, eps);
            have_dilated = true;
        }
        if (! intersection_ex(dilated, painted_claims[color_idx]).empty())
            widened = candidate;
    }
    return std::max(min_claim_width, widened);
}

static std::vector<std::vector<ExPolygons>> merge_segmented_layers(const std::vector<std::vector<ExPolygons>> &segmented_regions,
                                                                   std::vector<std::vector<ExPolygons>>      &&top_and_bottom_layers,
                                                                   std::vector<std::vector<ExPolygons>>      &&legacy_top_and_bottom_layers,
                                                                   const size_t                                num_facets_states,
                                                                   // ITEM 1 plumbing (interclaim-absorb-report.md): the pre-segmentation layer
                                                                   // contour (needed to compute the base residue against the FINAL claims,
                                                                   // not the lateral-only lslices), the narrowest painted claim width
                                                                   // (halved it becomes the absorb's opening delta, so the absorb's KILL
                                                                   // WIDTH is min_claim_width itself - see the threshold comment at the
                                                                   // absorb site), the F1 wall-stack band width (the interior guard), and
                                                                   // whether depth is bounded at all (segmentation_normal_depth > 0.f at
                                                                   // the call site - the SAME condition the rest of the feature gates on,
                                                                   // so paint_depth_mode == unlimited stays byte-identical: the entire
                                                                   // absorb stage is skipped, not merely a no-op inside it). ITEM 2
                                                                   // plumbing added below: min_claim_width_gapfill_off_by_layer_color.
                                                                   const std::vector<ExPolygons>              &input_expolygons,
                                                                   const float                                  min_claim_width,
                                                                   // Fix-wave (absorb-tail-review.md Minor 4): PER-COLOUR (indexed by
                                                                   // extruder id, i.e. the SAME indexing as segmented_regions'
                                                                   // inner vector) gap-fill-disabled kill width (mm, 0.f for a colour
                                                                   // with no gap-fill-disabled region of its own), replacing the old
                                                                   // single object-wide MAX. An island's effective widened threshold
                                                                   // is now resolved from only the colours that actually border IT
                                                                   // (see the absorb loop below) - an unrelated gap-fill-disabled
                                                                   // region elsewhere on the object can no longer widen the kill
                                                                   // width for an island none of whose real neighbours have gap fill
                                                                   // off. Entry 0 (base) is never consulted - the absorb never hands
                                                                   // an island to colour 0. Sized generously by the caller; an
                                                                   // out-of-range colour index (or an empty vector, the fuzzy-skin
                                                                   // caller's value) is treated as 0.f, matching "no gap-fill-off
                                                                   // region for that colour" - min_claim_width alone applies, byte-
                                                                   // identical to an object with gap fill enabled everywhere.
                                                                   //
                                                                   // Fix-wave 2 (absorb-tail-fixwave2-review.md I-C): OUTER index is now
                                                                   // the LAYER, not the colour - i.e. this is PER LAYER *and* PER COLOUR.
                                                                   // Without this, a gap-fill-disabled PARAMETER_MODIFIER confined to a
                                                                   // small Z range widened the absorb's kill width for EVERY layer of
                                                                   // that colour, including layers nowhere near the modifier where every
                                                                   // region actually present has gap fill ON - see the absorb loop below
                                                                   // for how a layer index out of range (or an empty outer vector, the
                                                                   // fuzzy-skin caller's value) degrades to "no gap-fill-off data for
                                                                   // this layer", byte-identical to an object with gap fill on
                                                                   // everywhere.
                                                                   //
                                                                   // Fix-wave 3 (absorb-tail-fixwave2-review.md I-1): fix-wave 2's own
                                                                   // per-layer resolution ("the regions of that colour with actual
                                                                   // geometry on this layer", via a SEPARATE (a)+(b) approximation built
                                                                   // in segmentation_by_painting) could disagree with what the descent's
                                                                   // OWN erosion in segmentation_top_and_bottom_layers actually used,
                                                                   // because that erosion read layer_color_stat's per-colour block,
                                                                   // which stayed last-region-wins and slice-ungated - so a gap-fill-off
                                                                   // modifier anywhere on the object could erode the descent claim wide
                                                                   // on layers this array correctly left narrow, leaving genuine
                                                                   // (0.45, 0.75]mm base slivers this array's narrower value then failed
                                                                   // to catch (measured: 8.14 / 6.01 mm2 at the sphere fixture's cap
                                                                   // boundary). Now built directly from compute_layer_color_stat's own
                                                                   // small_region_threshold - the SAME per-layer resolution the erosion
                                                                   // sites themselves use - so this array can no longer disagree with
                                                                   // the population it exists to catch.
                                                                   const std::vector<std::vector<float>>       &min_claim_width_gapfill_off_by_layer_color,
                                                                   const float                                  wall_stack,
                                                                   const bool                                   bounded_mode,
                                                                   // Fix-wave (absorb-tail-review.md Minor 3): one ExPolygons per
                                                                   // layer, the F2 ladder's own deliberately-preserved base residue
                                                                   // (paint_depth_clamp_keep_core, via cut_segmented_layers's new
                                                                   // out-parameter) - never absorbed below, regardless of the F1
                                                                   // wall-stack guard's own verdict. Empty (the fuzzy-skin caller's
                                                                   // value, and any layer cut_segmented_layers did not compute one
                                                                   // for) is a no-op: this exemption never fires and the absorb is
                                                                   // byte-identical to before this fix.
                                                                   const std::vector<ExPolygons>                &keep_core_by_layer,
                                                                   const std::function<void()>                &throw_on_cancel_callback)
{
    const size_t                         num_layers = segmented_regions.size();
    std::vector<std::vector<ExPolygons>> segmented_regions_merged(num_layers);
    segmented_regions_merged.assign(num_layers, std::vector<ExPolygons>(num_facets_states));
    assert(!top_and_bottom_layers.size() || num_facets_states == top_and_bottom_layers.size());
    assert(!legacy_top_and_bottom_layers.size() || num_facets_states == legacy_top_and_bottom_layers.size());

    // Fix-wave (wave-b-review.md Important 2): a painted colour's DEEPENED (beyond-legacy-shell)
    // top/bottom reach must not silently annex a NEIGHBOURING PAINTED colour's own lateral claim -
    // merge_segmented_layers's own §2.2 fix already stops a base (color_idx 0) claim from doing
    // this to a painted lateral band, but nothing stops one PAINTED colour's claim from doing the
    // same to another's, and Wave B deepened that reach 2.5x (6 -> 15 layers at stock defaults).
    // Bounding the trim loop below to the legacy reach alone is NOT enough by itself: this
    // colour's own excess is still appended into ITS OWN region unconditionally further down, so
    // skipping the trim would leave the SAME area double-claimed - a real overlap, not merely an
    // unfixed asymmetry. So clip the excess itself here, against every OTHER painted colour's own
    // (raw, pre-trim) lateral claim, and rebuild this colour's top_and_bottom claim as
    // legacy U clipped-excess before the merge loop ever sees it - one self-consistent claim that
    // the trim loop AND the append-back both keep using completely unmodified below. Base colour
    // (color_idx 0) never deepens (segmentation_top_and_bottom_layers's own "PAINTED COLOURS ONLY"
    // gate: color_idx 0's top_descent_layers == top_shell_layers always), so its excess is empty
    // by construction and this loop starting at color_idx 1 is correct, not an oversight.
    if (!top_and_bottom_layers.empty() && !legacy_top_and_bottom_layers.empty()) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&segmented_regions, &top_and_bottom_layers, &legacy_top_and_bottom_layers, &num_facets_states, &throw_on_cancel_callback](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
                throw_on_cancel_callback();
                for (size_t color_idx = 1; color_idx < num_facets_states; ++color_idx) {
                    ExPolygons &full = top_and_bottom_layers[color_idx][layer_idx];
                    if (full.empty())
                        continue;
                    const ExPolygons &legacy = legacy_top_and_bottom_layers[color_idx][layer_idx];
                    ExPolygons        excess = legacy.empty() ? full : diff_ex(full, legacy);
                    if (excess.empty())
                        continue;
                    ExPolygons other_painted_laterals;
                    for (size_t other_idx = 1; other_idx < num_facets_states; ++other_idx)
                        if (other_idx != color_idx && !segmented_regions[layer_idx][other_idx].empty())
                            append(other_painted_laterals, segmented_regions[layer_idx][other_idx]);
                    if (other_painted_laterals.empty())
                        continue;
                    // Fix-wave (absorb-tail-review.md I1): do NOT rebuild `full` from `legacy`
                    // verbatim (the pre-fix `combined = legacy U excess_clipped` below). `legacy`
                    // is fed from the descent loop's RAW `last` whenever normal_shell is true
                    // (segmentation_top_and_bottom_layers :2110/:2179 skip their per-step
                    // opening_ex exactly then, deferring to the band-level opening at
                    // :2268-2269 instead - see that site's Item 2 comment), and the shadow-
                    // building loop above (:2216-2228) never opens it either - so `legacy` can
                    // carry un-opened, sub-threshold ring fragments that `full`'s own band-level
                    // opening already filtered out. Unioning `legacy` back in wholesale would
                    // reintroduce exactly those fragments into the final claim on every layer
                    // this loop actually modifies (i.e. whenever other_painted_laterals is
                    // non-empty) - the #7104 filter silently stops applying there, and the
                    // reintroduced fragments then trim the NEIGHBOUR's own lateral band too (the
                    // second parallel_for below subtracts every colour's `full` from every OTHER
                    // colour's lateral claim).
                    //
                    // Fix: never reference `legacy`'s contents directly - only ever SUBTRACT from
                    // the already-correctly-opened `full` computed above. The real correctness
                    // argument (fix-wave 2, absorb-tail-fixwave2-review.md Minor / m1 - the
                    // previous wording here, "identical to the old formula whenever legacy is a
                    // subset of full", was WRONG and is corrected below): `full = diff_ex(full, X)`
                    // for ANY X can only ever SHRINK `full` or leave it unchanged - subtracting
                    // can never add area back - so this rewrite can never reintroduce a fragment
                    // `full`'s own band-level opening already removed, regardless of what
                    // `legacy`/`excess` happen to contain. It does NOT depend on `legacy` being a
                    // subset of `full`: `legacy` is by design never priority-trimmed against a
                    // lower-index colour's claim, nor clipped by another colour's own surface
                    // contribution, while `full` is both, so that subset relationship does not
                    // generally hold on a layer where this colour's legacy band overlaps either -
                    // Item 2 or not. Where it DID happen to hold, the two formulas agree (both
                    // reduce to `full \ (excess ∩ other)`, since `full = legacy ∪ excess`
                    // disjointly there); where it did not, the OLD formula's unconditional
                    // `legacy ∪ ...` resurrected exactly that already-trimmed geometry as a real
                    // double claim, and this one does not - strictly better everywhere, not merely
                    // "identical outside one narrow bug". Pairwise painted-claim disjointness
                    // (measured zero overlap on every layer of a two-painted-colour fixture) is
                    // pinned directly by a dedicated test (m1) rather than relied on as an
                    // unstated consequence.
                    //
                    // Fix-wave 2 (I-A): the shadow-building site above (where `reach`/`legacy` is
                    // assembled) now ALSO opens it with the same per-layer/per-colour
                    // small_region_threshold `full`'s own band-level opening uses, whenever
                    // normal_shell. A previous wave considered and rejected doing exactly this as
                    // an ALTERNATIVE to the subtract-only rewrite above; it turned out the two are
                    // complementary, not alternatives: subtract-only stops this clip from ADDING
                    // area, but the region it deliberately EXEMPTS from the clip (`full ∩ legacy`)
                    // can still be a thin sliver of `full`'s own (already wide) shape wherever
                    // `legacy` still carries one of these un-opened raw fingers and another
                    // colour's boundary happens to cut through it - opening `reach` at its own
                    // build site stops it from ever carrying that raw finger, so it can no longer
                    // shape the final claim that way either. See has_painted_unopened_fragment's
                    // own header comment (test file) for the measured before/after.
                    full = diff_ex(full, intersection_ex(excess, other_painted_laterals));
                }
            }
        });
    }

    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - Merging segmented layers in parallel - Begin";
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&segmented_regions, &top_and_bottom_layers, &segmented_regions_merged, &num_facets_states, &throw_on_cancel_callback](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            assert(segmented_regions[layer_idx].size() == num_facets_states);
            for (size_t extruder_id = 0; extruder_id < num_facets_states; ++extruder_id) {
                throw_on_cancel_callback();
                if (!segmented_regions[layer_idx][extruder_id].empty()) {
                    ExPolygons segmented_regions_trimmed = segmented_regions[layer_idx][extruder_id];
                    if (!top_and_bottom_layers.empty()) {
                        for (const std::vector<ExPolygons> &top_and_bottom_by_extruder : top_and_bottom_layers) {
                            if (!top_and_bottom_by_extruder[layer_idx].empty() && !segmented_regions_trimmed.empty()) {
                                segmented_regions_trimmed = diff_ex(segmented_regions_trimmed, top_and_bottom_by_extruder[layer_idx]);
                            }
                        }
                    }

                    segmented_regions_merged[layer_idx][extruder_id] = std::move(segmented_regions_trimmed);
                }

                if (!top_and_bottom_layers.empty() && !top_and_bottom_layers[extruder_id][layer_idx].empty()) {
                    bool was_top_and_bottom_empty = segmented_regions_merged[layer_idx][extruder_id].empty();
                    append(segmented_regions_merged[layer_idx][extruder_id], top_and_bottom_layers[extruder_id][layer_idx]);

                    // Remove dimples (#7235) appearing after merging side segmentation of the model with tops and bottoms painted layers.
                    if (!was_top_and_bottom_empty)
                        segmented_regions_merged[layer_idx][extruder_id] = offset2_ex(union_ex(segmented_regions_merged[layer_idx][extruder_id]), float(SCALED_EPSILON), -float(SCALED_EPSILON));
                }
            }
        }
    }); // end of parallel_for
    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - Merging segmented layers in parallel - End";

    // ITEM 1 (interclaim-absorb-report.md / interclaim-sliver-investigation.md section 5,
    // Option 1): interior inter-claim absorb, a third stage after the merge above. This is the
    // only place every colour's FINAL lateral+top/bottom claim and the layer contour are all in
    // hand simultaneously. Reclaims what each colour's own private thin-projection filter
    // (opening_ex(., small_region_threshold) in segmentation_top_and_bottom_layers) eroded off
    // ITS OWN claim and handed to nobody: the claims are never re-unioned or re-partitioned
    // across colours, so the strip left between two independently-eroded neighbouring claims
    // falls through to the base residue and prints as base wall_filament between two painted
    // features - the round-3 GUI defect this fix targets. See the threshold comment below for
    // how wide that strip actually gets (measured, not assumed).
    if (bounded_mode && min_claim_width > 0.f && wall_stack > 0.f) {
        BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - Interior inter-claim absorb - Begin";
        // Threshold. `t` is an opening_ex DELTA, so the width it annihilates is 2t, not t:
        // t = min_claim_width / 2 = 0.225mm at stock flows kills components narrower than
        // 2t = min_claim_width = 0.45mm. That is the correct quantity to key on, and it is not
        // the same number as "what the per-colour filter deleted":
        //   * min_claim_width is already this codebase's "narrowest painted claim that may be
        //     emitted" (Wave A / C-1, plumbed and unit-tested). An interior base island narrower
        //     than ONE bead cannot be printed as base in any useful way - Classic emits nothing
        //     for it, Arachne widens it into an over-extruded bead of the WRONG colour - so
        //     handing it to a printable neighbouring claim is strictly better than keeping it.
        //   * interclaim-sliver-investigation.md section 2 bounds the sliver population at
        //     2 * small_region_threshold = 0.225mm, reasoning from ONE colour's opening deleting
        //     one strip. That bound is too tight on curved geometry and is corrected here by
        //     measurement: on the sphere fixture (test_paint_depth_clamp.cpp, the Item 1
        //     RED/GREEN pin) the real inter-claim annuli are ~0.34mm wide, because BOTH
        //     neighbouring colours' claims are eroded independently and the miter-join corner
        //     truncation adds to the gap. A t of small_region_threshold (kill width 0.225mm)
        //     would leave every one of them behind; t = min_claim_width / 2 catches them.
        //
        // ITEM 2 (interclaim-sliver-investigation.md loose end 3, corrected the same way as the
        // above): min_claim_width alone stops covering the sliver population once gap fill is
        // disabled for at least one region. With gap_infill_speed == 0, small_region_threshold's
        // OWN kill width triples to ~0.75mm at stock flows (the OTHER arm of the same ternary
        // this comment's first bullet already reasons about) - wider than min_claim_width's
        // 0.45mm - so a sliver in that wider range has a "printable core" under a t = min_claim_
        // width/2 opening and is (wrongly) left as genuine base.
        //
        // Fix-wave (absorb-tail-review.md Minor 4 / M4): this USED to be a single object-wide
        // MAX (min_claim_width_gapfill_off, computed once here, outside the per-island loop),
        // which meant a gap-fill-disabled region ANYWHERE on the object widened the kill width
        // for EVERY island, even one whose actual neighbours all have gap fill on - over-
        // absorbing a genuine 0.45-0.75mm base gap in a gap-fill-ON region purely because some
        // unrelated region elsewhere on the object has gap fill off. Resolved PER ISLAND now,
        // inside the loop below, via interclaim_absorb_effective_claim_width() - MAX only over
        // the colours whose claim actually borders THIS island (min_claim_width_gapfill_off_by_
        // color[color] is 0.f for any colour with no gap-fill-disabled region of its own, and an
        // empty array - the fuzzy-skin caller's value - makes every island fall back to plain
        // min_claim_width, byte-identical to before this fix ever existed).
        const float wall_stack_scaled = scaled<float>(wall_stack);
        // eps = 2*SCALED_EPSILON (the investigation's own choice): just enough to make a
        // zero-area touch (shared boundary only) register as an overlap for
        // interclaim_absorb_winner (and, since this fix-wave, the same-shaped adjacency test in
        // interclaim_absorb_effective_claim_width), without meaningfully changing which
        // neighbour has the larger share.
        const float eps = float(2 * SCALED_EPSILON);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&segmented_regions_merged, &input_expolygons, min_claim_width, &min_claim_width_gapfill_off_by_layer_color, &keep_core_by_layer, wall_stack_scaled, eps, &throw_on_cancel_callback](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
                throw_on_cancel_callback();
                if (layer_idx >= input_expolygons.size() || input_expolygons[layer_idx].empty())
                    continue;
                std::vector<ExPolygons> &merged = segmented_regions_merged[layer_idx];

                ExPolygons painted;
                for (size_t color_idx = 1; color_idx < merged.size(); ++color_idx)
                    append(painted, merged[color_idx]);
                painted = union_ex(painted);
                if (painted.empty())
                    continue; // nothing painted on this layer - no inter-claim boundary can exist

                const ExPolygons base_area = diff_ex(input_expolygons[layer_idx], painted);
                if (base_area.empty())
                    continue;
                // F1 guard (interior): the wall-stack band at the contour is F1's own territory -
                // never a candidate here, so the exterior bleed F1 closed cannot return through
                // this stage.
                const ExPolygons interior = offset_ex(input_expolygons[layer_idx], -wall_stack_scaled);
                // Fix-wave (absorb-tail-review.md Minor 3 / M3): the F2 degradation ladder's own
                // deliberately-preserved base residue for THIS layer (paint_depth_clamp_keep_core,
                // via cut_segmented_layers - empty if that stage never ran, or never computed one
                // for this layer). Never absorbed below, regardless of the F1 wall-stack guard's
                // own verdict: keep_core can sit deeper than wall_stack from the contour (its own
                // erosion depth is `band`, which is >= wall_stack by construction - see
                // paint_depth_clamp_keep_core's header), so F1 alone does not reach it.
                const bool has_keep_core_layer = layer_idx < keep_core_by_layer.size() && ! keep_core_by_layer[layer_idx].empty();
                // Fix-wave 2 (absorb-tail-fixwave2-review.md I-C): this layer's own per-colour
                // gap-fill-off widths (resolved from the regions actually present ON THIS LAYER -
                // see multi_material_segmentation_by_painting's build site). Out-of-range (or the
                // fuzzy-skin caller's empty outer vector) degrades to a static empty inner vector,
                // which interclaim_absorb_effective_claim_width already treats as "no gap-fill-off
                // data" (every colour falls back to min_claim_width) - same degradation keep_core_
                // by_layer's own bounds check above uses.
                static const std::vector<float> no_gapfill_off_data_for_layer;
                const std::vector<float> &min_claim_width_gapfill_off_by_color =
                    layer_idx < min_claim_width_gapfill_off_by_layer_color.size() ? min_claim_width_gapfill_off_by_layer_color[layer_idx]
                                                                                   : no_gapfill_off_data_for_layer;

                // Batched per colour and applied once after the scan below, so which component is
                // decided first never changes another component's own (independent) decision.
                std::vector<ExPolygons> absorbed_into(merged.size());
                ExPolygons               absorbed_from_base;

                // PER CONNECTED COMPONENT - base_area is already exactly that (diff_ex/union_ex
                // output never merges disjoint regions into one ExPolygon, and always splits
                // disjoint pieces into separate ones), so iterating its elements directly IS the
                // per-component decomposition this absorb requires. Never
                // diff_ex(base_area, opening_ex(base_area, t)) as a single whole-area operation -
                // that would strip the interlocking notch tooth (and any other thin finger still
                // attached to a genuine base core) right along with real slivers, because a tooth
                // attached to a wide core reads as "thin" only in isolation, never as part of its
                // own component.
                for (const ExPolygon &island : base_area) {
                    const ExPolygons single{island};
                    // Fix-wave (Minor 4 / M4): resolved PER ISLAND, from only the colours that
                    // actually border it - see interclaim_absorb_effective_claim_width's header.
                    const float effective_claim_width = interclaim_absorb_effective_claim_width(
                        single, merged, min_claim_width_gapfill_off_by_color, min_claim_width, eps);
                    const float t = scaled<float>(effective_claim_width * 0.5f);
                    if (! opening_ex(single, t).empty())
                        continue; // has a printable core of its own -> genuine base, leave it
                    if (! diff_ex(single, interior).empty())
                        continue; // touches/extends into the F1 wall-stack band -> F1 owns it
                    // Fix-wave (Minor 3 / M3): F2's own keep-core -> never absorbed, but ONLY the
                    // THIN residue keep_core degenerates to on a two-face-painted wall in
                    // (2*band, 2*band + effective_claim_width] - "hairline-to-kill-width" per M3's
                    // own wording, i.e. it vanishes under THIS island's own `t` opening. keep_core
                    // is NOT always that: cut_segmented_layers pays paint_depth_clamp_keep_core
                    // unconditionally whenever a painted claim needs clamping at all, so on a
                    // thick/healthy object (this file's own sphere fixture included) it is simply
                    // the whole deep interior beyond `band` from the nearest surface - a single
                    // connected region that can span most of the cross-section. Testing "does the
                    // island touch/lie inside keep_core" against THAT is testing the wrong thing:
                    // a genuine interclaim sliver (which sits near the surface, well inside
                    // `band`) can perfectly well touch or even sit inside such a large region
                    // without being any part of the degenerate residue M3 describes - and MUST
                    // still be absorbed there (M4's own gap-fill-off widening exists to do exactly
                    // that). So find the SPECIFIC keep_core connected component this island
                    // actually overlaps (per-component, same discipline as base_area's own
                    // decomposition above - never test the whole array at once, which would let a
                    // thin residue's width be masked by an unrelated wide component elsewhere on
                    // the same layer) and test THAT component's own width, not the island's.
                    //
                    // Fix-wave 2 (absorb-tail-fixwave2-review.md I-D): re-use the island's own
                    // `t` (computed just above from effective_claim_width, which already accounts
                    // for M4's per-island gap-fill-off widening) rather than recomputing a
                    // SEPARATE, narrower t_keep_core from the un-widened min_claim_width alone.
                    // The two used to disagree whenever gap fill is off for a colour bordering
                    // this island: the OUTER test above already treats a keep-core component up
                    // to effective_claim_width wide as having "no printable core" (candidate for
                    // absorb), but the OLD narrower re-test here only recognised it as the
                    // EXEMPT thin residue up to the smaller min_claim_width - so a component in
                    // between the two (a genuine printable core under the outer test's own
                    // widened delta) fell through BOTH exemptions and got absorbed anyway,
                    // alternating per layer wherever the default interlock notch pushed the
                    // core's own width across that gap on one parity but not the other. Identical
                    // to the pre-fix value whenever gap fill is on everywhere (effective_claim_
                    // width == min_claim_width there, so t == the old t_keep_core exactly).
                    bool in_thin_keep_core_residue = false;
                    if (has_keep_core_layer) {
                        const float t_keep_core = t;
                        for (const ExPolygon &core_component : keep_core_by_layer[layer_idx]) {
                            const ExPolygons core_single{core_component};
                            if (intersection_ex(single, core_single).empty())
                                continue; // this component of keep_core has nothing to do with this island
                            if (opening_ex(core_single, t_keep_core).empty()) {
                                in_thin_keep_core_residue = true;
                                break;
                            }
                        }
                    }
                    if (in_thin_keep_core_residue)
                        continue;
                    const size_t winner = interclaim_absorb_winner(single, merged, eps);
                    if (winner == 0)
                        continue; // no painted neighbour touches it -> never orphan it
                    append(absorbed_into[winner], single);
                    absorbed_from_base.push_back(island);
                }

                if (! absorbed_from_base.empty()) {
                    for (size_t color_idx = 1; color_idx < merged.size(); ++color_idx)
                        if (! absorbed_into[color_idx].empty()) {
                            append(merged[color_idx], std::move(absorbed_into[color_idx]));
                            merged[color_idx] = union_ex(merged[color_idx]);
                        }
                    merged[0] = diff_ex(merged[0], absorbed_from_base);
                }
            }
        });
        BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - Interior inter-claim absorb - End";
    }

    return segmented_regions_merged;
}

#ifdef MM_SEGMENTATION_DEBUG_REGIONS
static void export_regions_to_svg(const std::string &path, const std::vector<ExPolygons> &regions, const ExPolygons &lslices)
{
    const std::vector<std::string> colors       = {"blue", "cyan", "red", "orange", "magenta", "pink", "purple", "yellow"};
    coordf_t                       stroke_width = scale_(0.05);
    BoundingBox                    bbox         = get_extents(lslices);
    bbox.offset(scale_(1.));
    ::Slic3r::SVG svg(path.c_str(), bbox);

    svg.draw_outline(lslices, "green", "lime", stroke_width);
    for (const ExPolygons &by_extruder : regions) {
        size_t extrude_idx = &by_extruder - &regions.front();
        if (extrude_idx < int(colors.size()))
            svg.draw(by_extruder, colors[extrude_idx]);
        else
            svg.draw(by_extruder, "black");
    }
}
#endif // MM_SEGMENTATION_DEBUG_REGIONS

#ifdef MM_SEGMENTATION_DEBUG_INPUT
void export_processed_input_expolygons_to_svg(const std::string &path, const LayerRegionPtrs &regions, const ExPolygons &processed_input_expolygons)
{
    coordf_t    stroke_width = scale_(0.05);
    BoundingBox bbox         = get_extents(regions);
    bbox.merge(get_extents(processed_input_expolygons));
    bbox.offset(scale_(1.));
    ::Slic3r::SVG svg(path.c_str(), bbox);

    for (LayerRegion *region : regions)
        for (const Surface &surface : region->slices.surfaces)
            svg.draw_outline(surface, "blue", "cyan", stroke_width);

    svg.draw_outline(processed_input_expolygons, "red", "pink", stroke_width);
}
#endif // MM_SEGMENTATION_DEBUG_INPUT

#ifdef MM_SEGMENTATION_DEBUG_PAINTED_LINES
static void export_painted_lines_to_svg(const std::string &path, const std::vector<std::vector<PaintedLine>> &all_painted_lines, const ExPolygons &lslices)
{
    const std::vector<std::string> colors       = {"blue", "cyan", "red", "orange", "magenta", "pink", "purple", "yellow"};
    coordf_t                       stroke_width = scale_(0.05);
    BoundingBox                    bbox         = get_extents(lslices);
    bbox.offset(scale_(1.));
    ::Slic3r::SVG svg(path.c_str(), bbox);

    for (const Line &line : to_lines(lslices))
        svg.draw(line, "green", stroke_width);

    for (const std::vector<PaintedLine> &painted_lines : all_painted_lines)
        for (const PaintedLine &painted_line : painted_lines)
            svg.draw(painted_line.projected_line, painted_line.color < int(colors.size()) ? colors[painted_line.color] : "black", stroke_width);
}
#endif // MM_SEGMENTATION_DEBUG_PAINTED_LINES

#ifdef MM_SEGMENTATION_DEBUG_COLORIZED_POLYGONS
static void export_colorized_polygons_to_svg(const std::string &path, const std::vector<ColoredLines> &colorized_polygons, const ExPolygons &lslices)
{
    const std::vector<std::string> colors       = {"blue", "cyan", "red", "orange", "magenta", "pink", "purple", "green", "yellow"};
    coordf_t                       stroke_width = scale_(0.05);
    BoundingBox                    bbox         = get_extents(lslices);
    bbox.offset(scale_(1.));
    ::Slic3r::SVG svg(path.c_str(), bbox);

    for (const ColoredLines &colorized_polygon : colorized_polygons)
        for (const ColoredLine &colorized_line : colorized_polygon)
            svg.draw(colorized_line.line, colorized_line.color < int(colors.size())? colors[colorized_line.color] : "black", stroke_width);
}
#endif // MM_SEGMENTATION_DEBUG_COLORIZED_POLYGONS

// Check if all ColoredLine representing a single layer uses the same color.
static bool has_layer_only_one_color(const std::vector<ColoredLines> &colored_polygons)
{
    assert(!colored_polygons.empty());
    assert(!colored_polygons.front().empty());
    int first_line_color = colored_polygons.front().front().color;
    for (const ColoredLines &colored_polygon : colored_polygons)
        for (const ColoredLine &colored_line : colored_polygon)
            if (first_line_color != colored_line.color)
                return false;

    return true;
}

std::vector<std::vector<ExPolygons>> segmentation_by_painting(const PrintObject                                               &print_object,
                                                              const std::function<ModelVolumeFacetsInfo(const ModelVolume &)> &extract_facets_info,
                                                              const size_t                                                     num_facets_states,
                                                              const float                                                      segmentation_max_width,
                                                              const float                                                      segmentation_interlocking_depth,
                                                              const float                                                      segmentation_min_claim_width,
                                                              const float                                                      segmentation_normal_depth,
                                                              // ITEM 1 (interclaim-absorb-report.md): the F1 wall-stack band width
                                                              // (ext_perimeter_width + ext_perimeter_spacing, mm, max across the
                                                              // object's printing regions - the SAME conservative direction
                                                              // segmentation_min_claim_width already takes), plumbed through to
                                                              // merge_segmented_layers's interior inter-claim absorb. 0.f (the fuzzy
                                                              // skin caller's value) disables the absorb, matching its 0.f
                                                              // segmentation_normal_depth.
                                                              const float                                                      segmentation_wall_stack,
                                                              // ITEM 2 (interclaim-sliver-investigation.md loose end 3 / shell-
                                                              // setting-and-gapfill-report.md): the WIDER kill width the #7104
                                                              // thin-projection filter uses when gap_infill_speed == 0 for at
                                                              // least one of the object's regions (mm), plumbed through to
                                                              // merge_segmented_layers's interior inter-claim absorb so its own
                                                              // kill width can track it instead of silently under-covering the
                                                              // wider sliver population that configuration produces. Fix-wave
                                                              // (absorb-tail-review.md Minor 4 / M4): PER COLOUR (indexed by
                                                              // extruder id), not a single object-wide max - see
                                                              // interclaim_absorb_effective_claim_width. Still OBJECT-WIDE per
                                                              // colour (not per layer) at THIS parameter - only ever consulted here
                                                              // as a cheap "does the object have a gap-fill-off region ANYWHERE"
                                                              // pre-check (see this function's body): when it is entirely empty
                                                              // (the fuzzy skin caller's value, paired with segmentation_wall_stack
                                                              // == 0.f, where the absorb never runs regardless) the real per-layer
                                                              // construction below is skipped outright rather than computed and
                                                              // discarded. The per-layer values themselves (fix-wave 2, absorb-
                                                              // tail-fixwave2-review.md I-C) come from compute_layer_color_stat
                                                              // directly as of fix-wave 3 (I-1) - not from narrowing this array -
                                                              // so its own per-region values are otherwise unused.
                                                              const std::vector<float>                                        &segmentation_claim_width_gapfill_off_by_color,
                                                              const bool                                                       segmentation_interlocking_beam,
                                                              const IncludeTopAndBottomLayers                                  include_top_and_bottom_layers,
                                                              const std::function<void()>                                     &throw_on_cancel_callback)
{
    const size_t                          num_layers    = print_object.layers().size();
    std::vector<std::vector<ExPolygons>>  segmented_regions(num_layers);
    segmented_regions.assign(num_layers, std::vector<ExPolygons>(num_facets_states));
    std::vector<std::vector<PaintedLine>> painted_lines(num_layers);
    std::array<std::mutex, 64>            painted_lines_mutex;
    std::vector<EdgeGrid::Grid>           edge_grids(num_layers);
    const ConstLayerPtrsAdaptor           layers = print_object.layers();
    std::vector<ExPolygons>               input_expolygons(num_layers);

    throw_on_cancel_callback();

#ifdef MM_SEGMENTATION_DEBUG
    static int iRun = 0;
#endif // MM_SEGMENTATION_DEBUG

    // Merge all regions and remove small holes
    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - Slices preprocessing in parallel - Begin";
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&layers, &input_expolygons, &throw_on_cancel_callback](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            throw_on_cancel_callback();
            ExPolygons ex_polygons;
            for (LayerRegion *region : layers[layer_idx]->regions())
                for (const Surface &surface : region->slices.surfaces)
                    Slic3r::append(ex_polygons, offset_ex(surface.expolygon, float(10 * SCALED_EPSILON)));
            // All expolygons are expanded by SCALED_EPSILON, merged, and then shrunk again by SCALED_EPSILON
            // to ensure that very close polygons will be merged.
            ex_polygons = union_ex(ex_polygons);
            // Remove all expolygons and holes with an area less than 0.1mm^2
            remove_small_and_small_holes(ex_polygons, Slic3r::sqr(scale_(0.1f)));
            // Occasionally, some input polygons contained self-intersections that caused problems with Voronoi diagrams
            // and consequently with the extraction of colored segments by function extract_colored_segments.
            // Calling simplify_polygons removes these self-intersections.
            // Also, occasionally input polygons contained several points very close together (distance between points is 1 or so).
            // Such close points sometimes caused that the Voronoi diagram has self-intersecting edges around these vertices.
            // This consequently leads to issues with the extraction of colored segments by function extract_colored_segments.
            // Calling expolygons_simplify fixed these issues.
            input_expolygons[layer_idx] = remove_duplicates(expolygons_simplify(offset_ex(ex_polygons, -10.f * float(SCALED_EPSILON)), 5 * SCALED_EPSILON), scaled<coord_t>(0.01), PI/6);

#ifdef MM_SEGMENTATION_DEBUG_INPUT
            export_processed_input_expolygons_to_svg(debug_out_path("mm-input-%d-%d.svg", layer_idx, iRun), layers[layer_idx]->regions(), input_expolygons[layer_idx]);
#endif // MM_SEGMENTATION_DEBUG_INPUT
        }
    }); // end of parallel_for
    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - Slices preprocessing in parallel - End";

    std::vector<BoundingBox> layer_bboxes(num_layers);
    for (size_t layer_idx = 0; layer_idx < num_layers; ++layer_idx) {
        throw_on_cancel_callback();
        layer_bboxes[layer_idx] = get_extents(layers[layer_idx]->regions());
        layer_bboxes[layer_idx].merge(get_extents(input_expolygons[layer_idx]));
    }

    for (size_t layer_idx = 0; layer_idx < num_layers; ++layer_idx) {
        throw_on_cancel_callback();
        BoundingBox bbox = layer_bboxes[layer_idx];
        // Projected triangles could, in rare cases (as in GH issue #7299), belongs to polygons printed in the previous or the next layer.
        // Let's merge the bounding box of the current layer with bounding boxes of the previous and the next layer to ensure that
        // every projected triangle will be inside the resulting bounding box.
        if (layer_idx > 1) bbox.merge(layer_bboxes[layer_idx - 1]);
        if (layer_idx < num_layers - 1) bbox.merge(layer_bboxes[layer_idx + 1]);
        // Projected triangles may slightly exceed the input polygons.
        bbox.offset(20 * SCALED_EPSILON);
        edge_grids[layer_idx].set_bbox(bbox);
        edge_grids[layer_idx].create(input_expolygons[layer_idx], coord_t(scale_(10.)));
    }

    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - Projection of painted triangles - Begin";
    // Deft: a fixed, geometry-independent index for each volume - loop position, not the
    // ModelVolume pointer (which is not stable between two runs) - fed into PaintedLine::vol_idx
    // below so the sort comparator in post_process_painted_lines can finish an order that would
    // otherwise depend on which of two colour-tied facets a thread happened to reach first.
    size_t volume_idx = 0;
    for (const ModelVolume *mv : print_object.model_object()->volumes) {
        const ModelVolumeFacetsInfo facets_info = extract_facets_info(*mv);
        tbb::parallel_for(tbb::blocked_range<size_t>(1, num_facets_states), [&mv, &print_object, &facets_info, &layers, &edge_grids, &painted_lines, &painted_lines_mutex, &input_expolygons, &throw_on_cancel_callback, volume_idx](const tbb::blocked_range<size_t> &range) {
            for (size_t extruder_idx = range.begin(); extruder_idx < range.end(); ++extruder_idx) {
                throw_on_cancel_callback();
                const indexed_triangle_set custom_facets = facets_info.facets_annotation.get_facets(*mv, EnforcerBlockerType(extruder_idx));
                if (!mv->is_model_part() || custom_facets.indices.empty())
                    continue;

                const Transform3f tr = print_object.trafo().cast<float>() * mv->get_matrix().cast<float>();
                tbb::parallel_for(tbb::blocked_range<size_t>(0, custom_facets.indices.size()), [&tr, &custom_facets, &print_object, &layers, &edge_grids, &input_expolygons, &painted_lines, &painted_lines_mutex, &extruder_idx, volume_idx](const tbb::blocked_range<size_t> &range) {
                    for (size_t facet_idx = range.begin(); facet_idx < range.end(); ++facet_idx) {
                        float min_z = std::numeric_limits<float>::max();
                        float max_z = std::numeric_limits<float>::lowest();

                        std::array<Vec3f, 3> facet;
                        for (int p_idx = 0; p_idx < 3; ++p_idx) {
                            facet[p_idx] = tr * custom_facets.vertices[custom_facets.indices[facet_idx](p_idx)];
                            max_z        = std::max(max_z, facet[p_idx].z());
                            min_z        = std::min(min_z, facet[p_idx].z());
                        }

                        if (is_equal(min_z, max_z))
                            continue;

                        // Sort the vertices by z-axis for simplification of projected_facet on slices
                        std::sort(facet.begin(), facet.end(), [](const Vec3f &p1, const Vec3f &p2) { return p1.z() < p2.z(); });

                        // Find lowest slice not below the triangle.
                        auto first_layer = std::upper_bound(layers.begin(), layers.end(), float(min_z - EPSILON),
                                                            [](float z, const Layer *l1) { return z < l1->slice_z; });
                        auto last_layer  = std::upper_bound(layers.begin(), layers.end(), float(max_z + EPSILON),
                                                           [](float z, const Layer *l1) { return z < l1->slice_z; });
                        --last_layer;

                        for (auto layer_it = first_layer; layer_it != (last_layer + 1); ++layer_it) {
                            const Layer *layer     = *layer_it;
                            size_t       layer_idx = layer_it - layers.begin();
                            if (input_expolygons[layer_idx].empty() || is_less(layer->slice_z, facet[0].z()) || is_less(facet[2].z(), layer->slice_z))
                                continue;

                            // https://kandepet.com/3d-printing-slicing-3d-objects/
                            float t            = (float(layer->slice_z) - facet[0].z()) / (facet[2].z() - facet[0].z());
                            Vec3f line_start_f = facet[0] + t * (facet[2] - facet[0]);
                            Vec3f line_end_f;

                            // BBS: When one side of a triangle coincides with the slice_z.
                            if ((is_equal(facet[0].z(), facet[1].z()) && is_equal(facet[1].z(), layer->slice_z))
                                || (is_equal(facet[1].z(), facet[2].z()) && is_equal(facet[1].z(), layer->slice_z))) {
                                line_end_f = facet[1];
                            }
                            else if (facet[1].z() > layer->slice_z) {
                                // [P0, P2] and [P0, P1]
                                float t1   = (float(layer->slice_z) - facet[0].z()) / (facet[1].z() - facet[0].z());
                                line_end_f = facet[0] + t1 * (facet[1] - facet[0]);
                            } else {
                                // [P0, P2] and [P1, P2]
                                float t2   = (float(layer->slice_z) - facet[1].z()) / (facet[2].z() - facet[1].z());
                                line_end_f = facet[1] + t2 * (facet[2] - facet[1]);
                            }

                            Line line_to_test(Point(scale_(line_start_f.x()), scale_(line_start_f.y())),
                                              Point(scale_(line_end_f.x()), scale_(line_end_f.y())));
                            line_to_test.translate(-print_object.center_offset());

                            // BoundingBoxes for EdgeGrids are computed from printable regions. It is possible that the painted line (line_to_test) could
                            // be outside EdgeGrid's BoundingBox, for example, when the negative volume is used on the painted area (GH #7618).
                            // To ensure that the painted line is always inside EdgeGrid's BoundingBox, it is clipped by EdgeGrid's BoundingBox in cases
                            // when any of the endpoints of the line are outside the EdgeGrid's BoundingBox.
                            BoundingBox edge_grid_bbox = edge_grids[layer_idx].bbox();
                            edge_grid_bbox.offset(10 * scale_(EPSILON));
                            if (!edge_grid_bbox.contains(line_to_test.a) || !edge_grid_bbox.contains(line_to_test.b)) {
                                // If the painted line (line_to_test) is entirely outside EdgeGrid's BoundingBox, skip this painted line.
                                if (!edge_grid_bbox.overlap(BoundingBox(Points{line_to_test.a, line_to_test.b})) ||
                                    !line_to_test.clip_with_bbox(edge_grid_bbox))
                                    continue;
                            }

                            size_t mutex_idx = layer_idx & 0x3F;
                            assert(mutex_idx < painted_lines_mutex.size());

                            PaintedLineVisitor visitor(edge_grids[layer_idx], painted_lines[layer_idx], painted_lines_mutex[mutex_idx], 16);
                            visitor.line_to_test = line_to_test;
                            visitor.color        = int(extruder_idx);
                            visitor.vol_idx      = volume_idx;
                            visitor.facet_idx    = facet_idx;
                            edge_grids[layer_idx].visit_cells_intersecting_line(line_to_test.a, line_to_test.b, visitor);
                        }
                    }
                }); // end of parallel_for
            }
        }); // end of parallel_for
        ++volume_idx;
    }
    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - projection of painted triangles - end";
    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - painted layers count: "
                             << std::count_if(painted_lines.begin(), painted_lines.end(), [](const std::vector<PaintedLine> &pl) { return !pl.empty(); });

    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - layers segmentation in parallel - begin";
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&edge_grids, &input_expolygons, &painted_lines, &segmented_regions, &num_facets_states, &throw_on_cancel_callback](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            throw_on_cancel_callback();
            if (!painted_lines[layer_idx].empty()) {
#ifdef MM_SEGMENTATION_DEBUG_PAINTED_LINES
                export_painted_lines_to_svg(debug_out_path("0-mm-painted-lines-%d-%d.svg", layer_idx, iRun), {painted_lines[layer_idx]}, input_expolygons[layer_idx]);
#endif // MM_SEGMENTATION_DEBUG_PAINTED_LINES

                std::vector<std::vector<PaintedLine>> post_processed_painted_lines = post_process_painted_lines(edge_grids[layer_idx].contours(), std::move(painted_lines[layer_idx]));

#ifdef MM_SEGMENTATION_DEBUG_PAINTED_LINES
                export_painted_lines_to_svg(debug_out_path("1-mm-painted-lines-post-processed-%d-%d.svg", layer_idx, iRun), post_processed_painted_lines, input_expolygons[layer_idx]);
#endif // MM_SEGMENTATION_DEBUG_PAINTED_LINES

                std::vector<ColoredLines> color_poly = colorize_contours(edge_grids[layer_idx].contours(), post_processed_painted_lines);

#ifdef MM_SEGMENTATION_DEBUG_COLORIZED_POLYGONS
                export_colorized_polygons_to_svg(debug_out_path("2-mm-colorized_polygons-%d-%d.svg", layer_idx, iRun), color_poly, input_expolygons[layer_idx]);
#endif // MM_SEGMENTATION_DEBUG_COLORIZED_POLYGONS

                assert(!color_poly.empty());
                assert(!color_poly.front().empty());
                if (has_layer_only_one_color(color_poly)) {
                    // If the whole layer is painted using the same color, it is not needed to construct a Voronoi diagram for the segmentation of this layer.
                    segmented_regions[layer_idx][size_t(color_poly.front().front().color)] = input_expolygons[layer_idx];
                } else {
                    MMU_Graph graph = build_graph(layer_idx, color_poly);
                    remove_multiple_edges_in_vertices(graph, color_poly);
                    graph.remove_nodes_with_one_arc();
                    segmented_regions[layer_idx] = extract_colored_segments(graph, num_facets_states);
                    //segmented_regions[layer_idx] = extract_colored_segments(color_poly, num_extruders, layer_idx);
                }

#ifdef MM_SEGMENTATION_DEBUG_REGIONS
                export_regions_to_svg(debug_out_path("3-mm-regions-sides-%d-%d.svg", layer_idx, iRun), segmented_regions[layer_idx], input_expolygons[layer_idx]);
#endif // MM_SEGMENTATION_DEBUG_REGIONS
            }
        }
    }); // end of parallel_for
    BOOST_LOG_TRIVIAL(debug) << "Print object segmentation - layers segmentation in parallel - end";
    throw_on_cancel_callback();

    // Fix-wave 3 (absorb-tail-fixwave2-review.md I-1): derive this directly from
    // compute_layer_color_stat's own small_region_threshold - now correctly resolved PER LAYER
    // (see layer_color_stat_variant_applies_here) - instead of narrowing a SEPARATE, coarser
    // object-wide-per-colour signal with the (a)+(b) approximation fix-wave 2 used here. That
    // approximation was the bug: the top/bottom descent's own openings in
    // segmentation_top_and_bottom_layers resolve their erosion width from layer_color_stat's
    // per-colour block, which - unlike this site - stayed LAST-REGION-WINS and slice-ungated, so
    // a gap-fill-off PARAMETER_MODIFIER anywhere on the object could erode the descent at the
    // wide (gap-off) width on layers this site correctly judged "no gap-fill-off region nearby"
    // and left narrow - measured: 8.14 / 6.01 mm2 base slivers at the sphere fixture's cap
    // boundary (layers 149/150) with a modifier at print z 2-3, where the pre-fix-wave-2 parent
    // commit absorbed them (both sides tracked the SAME object-wide signal back then). Now that
    // compute_layer_color_stat itself resolves the applicable variant PER LAYER (via the
    // variant's PARENT volume region's own slices, not the auto-created painted region's own
    // - which is unconditionally empty here regardless of Z, see that function's own N1 comment),
    // calling it again here for the SAME (layer, colour) can never disagree with what the descent
    // already eroded there - by construction, not by keeping two approximations in sync by hand.
    //
    // The old segmented_regions[layer_idx][color_idx].empty() presence gate is dropped too
    // (review point 3): a colour with nothing resolved here already leaves small_region_
    // threshold_gapfill_off false (no entry below), and interclaim_absorb_effective_claim_
    // width's own per-island adjacency test (intersection_ex(dilated, painted_claims[color_idx]))
    // independently covers "is this colour's FINAL claim actually near this island", regardless
    // of what this construction resolves - the gate was redundant with it, not a second line of
    // defence (fix-wave2-review.md I-1, "theoretical, no bite measured").
    std::vector<std::vector<float>> claim_width_gapfill_off_by_layer_color(num_layers);
    if (! segmentation_claim_width_gapfill_off_by_color.empty()) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&layers, &print_object, segmentation_normal_depth, segmentation_min_claim_width, num_facets_states, &claim_width_gapfill_off_by_layer_color, &throw_on_cancel_callback](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
                throw_on_cancel_callback();
                std::vector<float> &by_color = claim_width_gapfill_off_by_layer_color[layer_idx];
                for (size_t color_idx = 1; color_idx < num_facets_states; ++color_idx) {
                    const LayerColorStat stat = compute_layer_color_stat(layers, print_object, segmentation_normal_depth, layer_idx, color_idx);
                    if (! stat.small_region_threshold_gapfill_off)
                        continue; // this layer's applicable variant has gap fill ON - min_claim_width alone applies, unchanged
                    if (color_idx >= by_color.size())
                        by_color.resize(color_idx + 1, 0.f);
                    // 2 * unscaled(...): the gap-off arm is exactly ext_perimeter_width +
                    // 0.7 * spacing (small_region_threshold halves and scales it - see
                    // compute_layer_color_stat) - i.e. the SAME quantity the erosion sites in
                    // segmentation_top_and_bottom_layers already used to build this layer's
                    // claim. Floored at segmentation_min_claim_width defensively (interclaim_
                    // absorb_effective_claim_width's own final std::max already guarantees this
                    // floor regardless, since `widened` there starts at 0.f).
                    by_color[color_idx] = std::max(segmentation_min_claim_width, 2.f * unscaled<float>(stat.small_region_threshold));
                }
            }
        });
    }

    // Fix-wave (absorb-tail-review.md Minor 3 / M3): the F2 ladder's own deliberately-preserved
    // base residue, one ExPolygons per layer - stays empty (the absorb's new exemption is then a
    // no-op everywhere) unless cut_segmented_layers actually runs below.
    std::vector<ExPolygons> keep_core_by_layer;
    if ((segmentation_max_width > 0.f || segmentation_interlocking_depth > 0.f) && !segmentation_interlocking_beam) {
        cut_segmented_layers(input_expolygons, segmented_regions, float(scale_(segmentation_max_width)), float(scale_(segmentation_interlocking_depth)),
                             float(scale_(segmentation_min_claim_width)), keep_core_by_layer, throw_on_cancel_callback);
        throw_on_cancel_callback();
    }

    // The first index is extruder number (includes default extruder), and the second one is layer number
    std::vector<std::vector<ExPolygons>> top_and_bottom_layers;
    // Fix-wave (wave-b-review.md Important 2): the legacy-depth shadow of top_and_bottom_layers -
    // see segmentation_top_and_bottom_layers's header comment and merge_segmented_layers.
    std::vector<std::vector<ExPolygons>> legacy_top_and_bottom_layers;
    if (include_top_and_bottom_layers == IncludeTopAndBottomLayers::Yes) {
        top_and_bottom_layers = segmentation_top_and_bottom_layers(print_object, input_expolygons, extract_facets_info, num_facets_states, segmentation_normal_depth, legacy_top_and_bottom_layers, throw_on_cancel_callback);
        throw_on_cancel_callback();
    }

    std::vector<std::vector<ExPolygons>> segmented_regions_merged = merge_segmented_layers(segmented_regions, std::move(top_and_bottom_layers), std::move(legacy_top_and_bottom_layers), num_facets_states,
                                                                                            input_expolygons, segmentation_min_claim_width, claim_width_gapfill_off_by_layer_color, segmentation_wall_stack, segmentation_normal_depth > 0.f,
                                                                                            keep_core_by_layer, throw_on_cancel_callback);
    throw_on_cancel_callback();

#ifdef MM_SEGMENTATION_DEBUG_REGIONS
    for (size_t layer_idx = 0; layer_idx < print_object.layers().size(); ++layer_idx)
        export_regions_to_svg(debug_out_path("4-mm-regions-merged-%d-%d.svg", layer_idx, iRun), segmented_regions_merged[layer_idx], input_expolygons[layer_idx]);
#endif // MM_SEGMENTATION_DEBUG_REGIONS

#ifdef MM_SEGMENTATION_DEBUG
    ++iRun;
#endif // MM_SEGMENTATION_DEBUG

    return segmented_regions_merged;
}

// Returns multi-material segmentation based on painting in multi-material segmentation gizmo
std::vector<std::vector<ExPolygons>> multi_material_segmentation_by_painting(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback) {
    const size_t num_physical_filaments = print_object.print()->config().filament_colour.size();
    const size_t num_total_filaments    = print_object.print()->mixed_filament_manager().total_filaments(num_physical_filaments);
    const size_t num_facets_states      = num_total_filaments + 1;

    // Paint Depth Stage 1, plan Task 2 item 1 (docs/superpowers/plans/2026-08-31-paint-depth.md,
    // docs/superpowers/specs/2026-08-31-paint-depth-design.md): mmu_segmented_region_max_width is
    // now legacy-parse-only (Task 1) - the runtime clamp width is derived from paint_depth_mode
    // via paint_depth_band_mm. We derive the flow (external-perimeter width / perimeter spacing)
    // that feeds walls-mode the same way fuzzy_skin_segmentation_by_painting derives its clamp
    // width below (:2237-2253 in this file - "limit the depth ... by the maximal extrusion width
    // of external perimeters"): from every PrintRegion on the object, not only the specific
    // region(s) that happen to carry paint. We don't have a cheap paint-facet -> PrintRegion
    // mapping at this call site (that mapping lives in PrintObjectSlice.cpp's
    // PrintObjectRegions::LayerRangeRegions, a different translation unit built later in the
    // pipeline), so - mirroring that precedent exactly - every printing region's flow is a
    // candidate. When regions differ (multiple materials/wall widths on one object), we take the
    // MAX per-region band across all of them: a narrower region's thinner walls must never be
    // allowed to under-clamp a wider region's paint claim, so the widest region's band wins
    // (documented conservative choice per the plan).
    const PaintDepthMode paint_depth_mode  = print_object.config().paint_depth_mode.value;
    const int            paint_depth_walls = print_object.config().paint_depth_walls.value;
    const double         paint_depth_mm    = print_object.config().paint_depth_mm.value;
    float                max_width         = 0.f;
    // Fix-wave F4: the interlocking notch is clamped against a perimeter spacing too (see
    // paint_depth_interlocking_depth_mm), and there the conservative direction is the
    // OPPOSITE of max_width's: the notch must stay inside the count-window margin of the
    // NARROWEST-walled region on the object, since that region's margin is the smallest in
    // absolute millimetres. So track the min spacing alongside the max band.
    float                min_perimeter_spacing = 0.f;
    // Wave A / C-1: the widest external extrusion on the object, which is the narrowest painted
    // claim the band clamp's degradation ladder is allowed to emit. MAX (not min) across regions
    // is the conservative direction here: a strip that is printable in the object's narrowest
    // region but not in its widest one must still be refused, because segmentation cannot tell
    // which region the strip will land in.
    float                max_ext_perimeter_width = 0.f;
    // Wave A / item 8: the classic wall generator cannot render a painted band narrower than two
    // properly-spaced lines - offset_ex() on a strip always returns both of its boundaries - so
    // the narrowest honest classic band is ext_perimeter_width + ext_perimeter_spacing, i.e. one
    // `wall_stack`, the same quantity F1 insets its top/bottom claim by. See
    // paint_depth_band_classic_floor_mm for the three defects that floor closes.
    const bool           wall_generator_classic = print_object.config().wall_generator.value == PerimeterGeneratorType::Classic;
    // Wave A fix-wave / I-1: the widest wall_stack across the object's classic-generated
    // regions, tracked alongside the per-region floor above so it is available once
    // interlocking_depth is known below - see paint_depth_classic_notch_cap_mm.
    float                max_wall_stack = 0.f;
    // ITEM 1 (interclaim-absorb-report.md): F1's OWN wall_stack quantity
    // (ext_perimeter_width + ext_perimeter_spacing), tracked UNCONDITIONALLY - unlike
    // max_wall_stack above, which only accumulates on the classic generator - because F1's inset
    // in segmentation_top_and_bottom_layers applies on every generator, so the absorb's interior
    // guard must match it there too. MAX across regions is the same conservative direction
    // max_ext_perimeter_width already takes: a larger wall_stack erodes the interior guard
    // MORE, so it can only make the absorb LESS willing to touch a component near the contour,
    // never more.
    float                max_wall_stack_absorb = 0.f;
    // ITEM 2 (interclaim-sliver-investigation.md loose end 3 / interclaim-absorb-report.md
    // "still open", shell-setting-and-gapfill-report.md): when gap_infill_speed == 0,
    // layer_color_stat's small_region_threshold (below) takes its "gap fill disabled" arm -
    // ext_perimeter_width + 0.7 * that region's extrusion spacing, instead of half the width -
    // which widens the #7104 thin-projection filter's kill width from 0.225mm to ~0.75mm at
    // stock flows (interclaim-sliver-investigation.md section 2 "Config sensitivity"). That
    // widens the population of inter-claim base slivers past the absorb's own kill width
    // (min_claim_width, 0.45mm - see the absorb's threshold comment in merge_segmented_layers),
    // so a sliver in that gap survives the absorb and prints body-coloured. Track the SAME
    // quantity here so the absorb can be told about it: MAX across regions where gap fill is
    // actually disabled (0.f if none, so an object with gap fill enabled everywhere - today's
    // default - is byte-identical: the absorb then falls back to min_claim_width exactly as
    // before this fix). Deliberately UNCONDITIONAL on wall_generator (unlike max_wall_stack
    // above, which is classic-only) - the sliver is created by segmentation, upstream of and
    // identical for both perimeter generators; only whether it PRINTS differs (Classic starts
    // emitting an external loop once a strip clears roughly one bead; Arachne widens ANY strip
    // past min_feature_size, 0.1mm, into a min_bead_width, 0.34mm, bead) - at ~0.75mm both
    // generators clear their own threshold and print it, so both are affected by the defect
    // this is fixing.
    // Fix-wave (absorb-tail-review.md Minor 4 / M4): this USED to be a single float, the MAX of
    // the term below across every region on the object regardless of which colour it belongs to
    // - so ANY region with gap fill disabled anywhere on the object widened the absorb's kill
    // width for EVERY colour's boundary, including ones whose own regions all have gap fill on
    // (M4's over-absorption). Tracked PER COLOUR now (indexed by extruder id, wall_filament -
    // the SAME discriminator layer_color_stat's own per-colour block uses,
    // MultiMaterialSegmentation.cpp), grown lazily; merge_segmented_layers's absorb resolves the
    // effective threshold for each island from only the colours that actually border it (see
    // interclaim_absorb_effective_claim_width). Colour 0 (base) is never populated - the absorb
    // never hands an island to it, matching layer_color_stat's own "PAINTED COLOURS ONLY" scope.
    //
    // Fix-wave 2 (absorb-tail-fixwave2-review.md I-C): this per-colour MAX is still OBJECT-WIDE
    // (every region of a colour anywhere on the object, regardless of which layer that region
    // actually has geometry on) - so on its own, a gap-fill-disabled PARAMETER_MODIFIER confined
    // to a small Z range still widened the absorb's kill width for every layer of that colour.
    // segmentation_by_painting (this function's own tail call, which has `segmented_regions` -
    // the real per-layer paint presence this function does NOT have access to) NARROWS this
    // object-wide-per-colour vector down to per-LAYER-per-colour before handing it to
    // merge_segmented_layers - see that narrowing's own comment for why it could not be done
    // here (the auto-created painted PrintRegion's own LayerRegion::slices is empty on every
    // layer at THIS point in the pipeline, so a naive per-layer check here would silently zero
    // this out entirely - caught by this fix-wave's own testing, not merely reasoned about).
    std::vector<float>  claim_width_gapfill_off_by_color;
    for (size_t region_idx = 0; region_idx < print_object.num_printing_regions(); ++region_idx) {
        const PrintRegion &region                 = print_object.printing_region(region_idx);
        const float         ext_perimeter_width   = region.flow(print_object, frExternalPerimeter, print_object.config().layer_height).width();
        const float         ext_perimeter_spacing = region.flow(print_object, frExternalPerimeter, print_object.config().layer_height).spacing();
        const float         perimeter_spacing     = region.flow(print_object, frPerimeter, print_object.config().layer_height).spacing();
        float               region_band           = paint_depth_band_mm(paint_depth_mode, paint_depth_walls, paint_depth_mm, ext_perimeter_width, ext_perimeter_spacing, perimeter_spacing);
        if (wall_generator_classic) {
            region_band    = paint_depth_band_classic_floor_mm(region_band, ext_perimeter_width, ext_perimeter_spacing);
            max_wall_stack = std::max(max_wall_stack, ext_perimeter_width + ext_perimeter_spacing);
        }
        max_wall_stack_absorb   = std::max(max_wall_stack_absorb, ext_perimeter_width + ext_perimeter_spacing);
        max_width = std::max(max_width, region_band);
        max_ext_perimeter_width = std::max(max_ext_perimeter_width, ext_perimeter_width);
        if (perimeter_spacing > 0.f)
            min_perimeter_spacing = min_perimeter_spacing > 0.f ? std::min(min_perimeter_spacing, perimeter_spacing) : perimeter_spacing;
        const int wall_filament_color = region.config().wall_filament.value;
        if (wall_filament_color >= 1 && region.config().gap_infill_speed.values.front() <= 0.f) {
            const float claim_width = ext_perimeter_width + 0.7f * Flow::rounded_rectangle_extrusion_spacing(ext_perimeter_width, float(print_object.config().layer_height.value));
            if (size_t(wall_filament_color) >= claim_width_gapfill_off_by_color.size())
                claim_width_gapfill_off_by_color.resize(size_t(wall_filament_color) + 1, 0.f);
            claim_width_gapfill_off_by_color[size_t(wall_filament_color)] = std::max(claim_width_gapfill_off_by_color[size_t(wall_filament_color)], claim_width);
        }
    }
    // Beam-interlocking mutual exclusion (:2169 below) is unchanged, but the interlocking
    // sub-band must only ever be active when depth is actually bounded (spec Stage 1, "Interlocking
    // ... only active when depth bounded, as upstream"): mmu_segmented_region_interlocking_depth's
    // default is 0.1 (lowered from 0.3 by fix-wave F4), so without this mode gate, pdmUnlimited would still cut a
    // 0.3mm interlocking band via the `segmentation_interlocking_depth > 0.f` half of the OR at
    // :2169 - breaking the "unlimited mode = legacy behavior, bit-identical" requirement. Gating
    // it on paint_depth_mode here (rather than editing the :2169 OR itself) keeps that gate's
    // existing shape/semantics untouched, as the plan requires.
    //
    // Fix-wave F4: the configured depth additionally goes through
    // paint_depth_interlocking_depth_mm, which caps it at a quarter of one perimeter spacing -
    // the exact count-window margin paint_depth_band_mm builds into the band - so the notch
    // can never move Arachne's strip thickness across a bead-count boundary and cost the
    // painted region a wall loop on even layers (the 3/2/3/2 alternation the user reported).
    // Wave A / I-3: that cap is walls-mode-only, which is why the mode is passed in - see
    // paint_depth_interlocking_depth_mm's header comment.
    float        interlocking_depth = paint_depth_mode != pdmUnlimited
                                        ? paint_depth_interlocking_depth_mm(paint_depth_mode, print_object.config().mmu_segmented_region_interlocking_depth.value, min_perimeter_spacing)
                                        : 0.f;
    // Wave A fix-wave / I-1 (.superpowers/sdd/2026-08-31-paint-depth/wave-a-review.md): the
    // per-region classic floor above ensures the ODD-layer band (cut_width, passed through
    // untouched by cut_segmented_layers) reaches wall_stack. EVEN layers additionally subtract
    // interlocking_depth from that same band, which this floor could not account for above - it
    // runs before interlocking_depth is known, since that value depends on min_perimeter_spacing
    // from every region in the SAME loop. Cap the notch itself here, now that both are in hand, at
    // whatever slack the (already classic-floored) band has above wall_stack, so the EVEN-layer
    // effective band (max_width - interlocking_depth) can never drop below wall_stack - closing
    // the classic floor's void ring on both parities instead of odd ones only. Capping the notch
    // rather than raising max_width leaves the ODD-layer band (which never subtracts the notch at
    // all) untouched, so this cannot widen the odd-layer band beyond what the floor above already
    // promises - e.g. at paint_depth_walls = 1, where the floored band already equals wall_stack
    // exactly (zero slack), the notch is capped to 0: a mechanical interlocking tooth is not
    // printable at all in the one configuration where the band itself is already at its own floor.
    if (wall_generator_classic)
        interlocking_depth = paint_depth_classic_notch_cap_mm(interlocking_depth, max_width, max_wall_stack);
    const bool   interlocking_beam  = print_object.config().interlocking_beam.value;

    size_t max_painted_state = 0;
    for (const ModelVolume *mv : print_object.model_object()->volumes) {
        if (!mv->is_model_part())
            continue;
        const auto &used_states = mv->mmu_segmentation_facets.get_data().used_states;
        for (size_t state_idx = static_cast<size_t>(EnforcerBlockerType::Extruder1); state_idx < used_states.size(); ++state_idx) {
            if (used_states[state_idx])
                max_painted_state = std::max(max_painted_state, state_idx);
        }
    }
    if (max_painted_state >= num_facets_states) {
        BOOST_LOG_TRIVIAL(warning) << "multi_material_segmentation_by_painting dropping painted states above segmentation range"
                                   << " max_painted_state=" << max_painted_state
                                   << " num_facets_states=" << num_facets_states
                                   << " physical_filaments=" << num_physical_filaments
                                   << " total_filaments=" << num_total_filaments;
    } else {
        BOOST_LOG_TRIVIAL(debug) << "multi_material_segmentation_by_painting state range check"
                                 << " max_painted_state=" << max_painted_state
                                 << " num_facets_states=" << num_facets_states
                                 << " physical_filaments=" << num_physical_filaments
                                 << " total_filaments=" << num_total_filaments;
    }

    const auto extract_facets_info = [](const ModelVolume &mv) -> ModelVolumeFacetsInfo {
        return {mv.mmu_segmentation_facets, mv.is_mm_painted(), false};
    };

    // WAVE B / Option N (.superpowers/sdd/2026-08-31-paint-depth/curved-gap-design.md): the same
    // band, handed to the top/bottom descent as a NORMAL THICKNESS rather than as a lateral bound.
    // That is the whole semantic change - "how deep the paint goes measured perpendicular to the
    // painted surface", one number that degenerates to the lateral band on a vertical wall and to
    // a layer-count shell on a flat top, and covers everything in between continuously. The band
    // handed over here is the same max-over-regions (and classic-floored) value the lateral clamp
    // gets, so the two halves of the claim are bounded by ONE quantity and their union is
    // D * max(cos, sin) at every slope.
    //
    // Zero in unlimited mode: paint_depth_band_mm already returns 0 there and the classic floor
    // passes a non-positive band through untouched, so max_width is 0 anyway - the explicit mode
    // test is belt-and-braces for the "unlimited mode is bit-identical to legacy" requirement,
    // which is a contract worth reading at the call site rather than deriving.
    const float paint_depth_normal_mm = paint_depth_mode != pdmUnlimited ? max_width : 0.f;

    return segmentation_by_painting(print_object, extract_facets_info, num_facets_states, max_width, interlocking_depth, max_ext_perimeter_width, paint_depth_normal_mm, max_wall_stack_absorb, claim_width_gapfill_off_by_color, interlocking_beam, IncludeTopAndBottomLayers::Yes, throw_on_cancel_callback);
}

// Returns fuzzy skin segmentation based on painting in fuzzy skin segmentation gizmo
std::vector<std::vector<ExPolygons>> fuzzy_skin_segmentation_by_painting(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback) {
    const size_t num_facets_states = 2; // Unpainted facets and facets painted with fuzzy skin.

    const auto extract_facets_info = [](const ModelVolume &mv) -> ModelVolumeFacetsInfo {
        return {mv.fuzzy_skin_facets, mv.is_fuzzy_skin_painted(), false};
    };

    // Because we apply fuzzy skin just on external perimeters, we limit the depth of fuzzy skin
    // by the maximal extrusion width of external perimeters.
    float max_external_perimeter_width = 0.;
    for (size_t region_idx = 0; region_idx < print_object.num_printing_regions(); ++region_idx) {
        const PrintRegion &region = print_object.printing_region(region_idx);
        max_external_perimeter_width = std::max<float>(max_external_perimeter_width, region.flow(print_object, frExternalPerimeter, print_object.config().layer_height).width());
    }

    // Wave A / C-1: fuzzy skin's own clamp width IS one external perimeter width, so that is also
    // its minimum printable claim - i.e. its ladder never runs, and geometry too thin to carry a
    // full-width fuzzy band keeps the pre-degradation no-op, byte-identical to upstream.
    // Wave B / Option N: 0 - fuzzy skin has no top/bottom claim at all (IncludeTopAndBottomLayers::No
    // below), so there is no descent for a normal thickness to bound.
    // ITEM 1: 0.f for segmentation_wall_stack too - paired with segmentation_normal_depth = 0.f
    // just above, this keeps merge_segmented_layers's bounded_mode gate false, so the interior
    // inter-claim absorb never runs on the fuzzy skin path (which has no notion of "painted
    // colour claims" for it to absorb between in the first place).
    // ITEM 2: an empty segmentation_claim_width_gapfill_off_by_color too, for the same reason -
    // the absorb never runs on this path regardless, so there is no threshold for it to widen.
    return segmentation_by_painting(print_object, extract_facets_info, num_facets_states, max_external_perimeter_width, 0.f, max_external_perimeter_width, 0.f, 0.f, std::vector<float>{}, false, IncludeTopAndBottomLayers::No, throw_on_cancel_callback);
}

} // namespace Slic3r
