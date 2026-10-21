#include "TriangleMeshDeal.hpp"

#include <igl/loop.h>
#include <assert.h>
#include <boost/log/trivial.hpp>

namespace Slic3r {
TriangleMesh TriangleMeshDeal::smooth_triangle_mesh(const TriangleMesh& mesh, bool& ok)
{
    if (mesh.its.vertices.empty() || mesh.its.indices.empty())
    {
        ok = false;
        return TriangleMesh();
    }

    using namespace igl;
    typedef Eigen::Matrix<float, Eigen::Dynamic, 3, Eigen::DontAlign | Eigen::RowMajor> RowMatrixX3f;
    typedef Eigen::Matrix<int, Eigen::Dynamic, 3, Eigen::DontAlign | Eigen::RowMajor>   RowMatrixX3i;

    auto vertices_count = mesh.its.vertices.size();
    auto indices_count  = mesh.its.indices.size();
    // Use Map to map the vertices and indicies into Matrixes without requiring a copy.
    const Eigen::Map<const RowMatrixX3f> OV(mesh.its.vertices[0].data(), vertices_count, 3);
    const Eigen::Map<const RowMatrixX3i> OF(mesh.its.indices[0].data(), indices_count, 3);
    Eigen::MatrixX3f                     V;
    Eigen::MatrixX3i                     F;

    // igl::loop takes Eigen::PlainObjectBase parameters, which Eigen::Map is not,
    // so materialize the mapped views into plain matrices (this copies the data).
    //orca Eigen version 5.0.1  snap Eigen version 3.3.7
    RowMatrixX3f OV_mat = OV;
    RowMatrixX3i OF_mat = OF;

    ok = true;
    // TODO: add validation checks for the input mesh? Is this really necessary?
    // if ( <not OK> ) {
    //    ok = false;
    //    return TriangleMesh();
    // }
    loop(OV_mat, OF_mat, V, F);

    indexed_triangle_set its;

    auto vert_num = V.rows();
    auto face_num = F.rows();
    its.vertices.reserve(vert_num);
    its.indices.reserve(face_num);

    for (int i = 0; i < vert_num; i++)
    {
        // its.vertices holds 3x1 column vectors (stl_vertex), while V.row(i) is a 1x3
        // row expression; transpose keeps the conversion dimensions exact instead of
        // relying on Eigen's runtime-only size check (an eigen_assert in debug builds).
        its.vertices.push_back(V.row(i).transpose());
    }

    for (int i = 0; i < face_num; i++)
    {
        // Same as above: stl_triangle_vertex_indices is a 3x1 column vector.
        its.indices.push_back(F.row(i).transpose());
    }

    TriangleMesh result_mesh(its);
    return result_mesh;
    
}
} // namespace Slic3r
