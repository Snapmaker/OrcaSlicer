#include "StepMesh.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#include <boost/filesystem.hpp>

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressRange.hxx>
#include <Message_ProgressScope.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

namespace Slic3r {
namespace Library {

namespace {

// Lets OCCT's reader and mesher see the scan's cancel flag.
class CancelIndicator : public Message_ProgressIndicator
{
public:
    explicit CancelIndicator(const std::atomic<bool>* cancel) : m_cancel(cancel) {}
    Standard_Boolean UserBreak() override { return m_cancel != nullptr && m_cancel->load(); }
    void             Show(const Message_ProgressScope&, const Standard_Boolean) override {}

private:
    const std::atomic<bool>* m_cancel;
};

// The Library scans on one thread at a time; this also keeps two previews from building OCCT's
// STEP translator tables at once.
std::mutex s_step_mutex;

} // namespace

bool read_step(const std::string& path, Triangles& out, const MeshLimits& limits, const std::atomic<bool>* cancel)
{
    out.clear();
    boost::system::error_code ec;
    const auto size = boost::filesystem::file_size(boost::filesystem::path(path), ec);
    if (ec || size == 0 || size > limits.max_bytes)
        return false;

    std::lock_guard<std::mutex> lock(s_step_mutex);
    try {
        Handle(CancelIndicator) indicator = new CancelIndicator(cancel);
        Message_ProgressScope   scope(indicator->Start(), "", 2);

        STEPControl_Reader reader;
        if (reader.ReadFile(path.c_str()) != IFSelect_RetDone || (cancel != nullptr && cancel->load()))
            return false;
        reader.TransferRoots(scope.Next());
        if (!scope.More() || (cancel != nullptr && cancel->load()))
            return false;
        const TopoDS_Shape shape = reader.OneShape();
        if (shape.IsNull())
            return false;

        Bnd_Box box;
        BRepBndLib::Add(shape, box);
        if (box.IsVoid())
            return false;
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        const double diag = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
        if (!(diag > 0.) || !std::isfinite(diag))
            return false;

        // About 1/400 of the part: smooth at 256 px, and far quicker than an import's tolerance.
        IMeshTools_Parameters params;
        params.Deflection = diag / 400.;
        params.Angle      = 0.5;
        params.InParallel = false; // the scan is already off the GUI thread; leave the cores to the app
        BRepMesh_IncrementalMesh mesher(shape, params, scope.Next());
        if (cancel != nullptr && cancel->load())
            return false;

        for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
            const TopoDS_Face&         face = TopoDS::Face(it.Current());
            TopLoc_Location            loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull())
                continue;
            const gp_Trsf trsf     = loc.Transformation();
            const bool    reversed = face.Orientation() == TopAbs_REVERSED;
            const int     n        = tri->NbTriangles();
            if (out.size() / 9 + size_t(n) > limits.max_triangles) {
                out.clear();
                return false;
            }
            for (int i = 1; i <= n; ++i) {
                int a, b, c;
                tri->Triangle(i).Get(a, b, c);
                if (reversed)
                    std::swap(b, c);
                for (int k : {a, b, c}) {
                    const gp_Pnt p = tri->Node(k).Transformed(trsf);
                    out.push_back(float(p.X()));
                    out.push_back(float(p.Y()));
                    out.push_back(float(p.Z()));
                }
            }
        }
    } catch (const Standard_Failure&) {
        out.clear();
    } catch (const std::exception&) {
        out.clear();
    }
    return !out.empty();
}

} // namespace Library
} // namespace Slic3r
