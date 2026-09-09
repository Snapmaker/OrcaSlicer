#ifndef slic3r_PathData_hpp_
#define slic3r_PathData_hpp_

#include "BufferTexture.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {

class PathLayerStack;

// View type indices; must stay in sync with GCodeViewer::EViewType.
// Kept as plain integers to avoid a header dependency on the viewer class.
namespace PathViewType
{
    constexpr unsigned int FEATURE_TYPE   = 0;  // extrusion role colors
    constexpr unsigned int HEIGHT         = 1;
    constexpr unsigned int WIDTH          = 2;
    constexpr unsigned int FEEDRATE       = 3;
    constexpr unsigned int FAN_SPEED      = 4;
    constexpr unsigned int TEMPERATURE    = 5;
    constexpr unsigned int VOLUMETRIC_RATE = 6;
    constexpr unsigned int TOOL           = 7;
    constexpr unsigned int COLOR_PRINT    = 8;
    constexpr unsigned int FILAMENT_ID    = 9;
    constexpr unsigned int LAYER_TIME     = 10;
    constexpr unsigned int LAYER_TIME_LOG = 11;
}

// One recorded toolpath point: world position plus the index of the move
// group it belongs to (the group index doubles as the per-move attribute
// index sampled by the shaders).
struct PathNode
{
    Vec3f position{ Vec3f::Zero() };
    uint32_t groupIndex{ 0 };
};

// All nodes produced by one move. A straight move contributes a single node;
// an arc move contributes its interpolation-point chain followed by the end
// position. Wipe moves lift their nodes by half the wipe height.
struct MoveNodeGroup
{
    uint32_t moveIndex{ 0 };             // index into GCodeProcessorResult::moves
    std::vector<uint32_t> nodeIndices{}; // indices into PathLayerData nodes
};

// One drawable step of a path: the node groups it connects plus the
// attributes of its ending move. firstSid/secondSid are sequential ids in
// the same space as GCodeViewer::SequentialView::gcode_ids (move indices
// with seam moves skipped).
struct PathStep
{
    uint32_t firstGroup{ 0 };
    uint32_t secondGroup{ 0 };
    uint32_t firstSid{ 0 };
    uint32_t secondSid{ 0 };
    EMoveType type{ EMoveType::Noop };
    ExtrusionRole role{ erNone };
    uint16_t extruderId{ 0 };
};

// Data tables of one printable layer: the raw path data (nodes, per-move
// groups, steps), the visibility-filtered GPU step records, and the buffer
// textures they get uploaded into. No geometry is ever generated here; the
// prism vertices are rebuilt in the vertex shader at draw time.
class PathLayerData
{
public:
    float Z() const { return _z; }
    uint32_t FirstSid() const { return _firstSid; }
    uint32_t LastSid() const { return _lastSid; }

    const std::vector<PathNode>& Nodes() const { return _nodes; }
    const std::vector<MoveNodeGroup>& Groups() const { return _groups; }
    const std::vector<PathStep>& Steps() const { return _steps; }

    // Number of path / marker step records currently stored (one vec4 each).
    size_t PathStepRecordCount() const { return _pathStepRecords.size() / 4; }
    size_t MarkerStepRecordCount() const { return _markerStepRecords.size() / 4; }
    // Number of path step records whose sid is <= sidLast. The records are
    // ordered by sid, so this is the instance count for the sequential
    // playback window (a binary search, no table rebuild).
    unsigned int PathStepCountUpTo(uint32_t sidLast) const;
    unsigned int MarkerStepCountUpTo(uint32_t sidLast) const;

    const BufferTexture& NodeTable() const { return _nodeTable; }
    const BufferTexture& WidthHeightTable() const { return _widthHeightTable; }
    const BufferTexture& AttributeTable() const { return _attributeTable; }
    const BufferTexture& PathStepTable() const { return _pathStepTable; }
    const BufferTexture& MarkerStepTable() const { return _markerStepTable; }

    // Rebuilds the visible step list and the GPU step records for the given
    // visibility state (move types, roles, filament, view type).
    void RefreshVisibleSteps(const PathLayerStack& stack);
    // Rebuilds the per-move attribute table for the current view type.
    void RefreshMoveAttributes(const GCodeProcessorResult& result, unsigned int viewType);
    // Uploads dirty tables; must be called with an active GL context.
    void UploadTables(const GCodeProcessorResult& result);

    void Reset();

private:
    // only the stack assembles layer content
    friend class PathLayerStack;

    uint32_t AddMoveNodeGroup(uint32_t moveIndex, const GCodeProcessorResult::MoveVertex& move);
    void AppendStep(const PathStep& step) { _steps.push_back(step); }
    uint32_t AddNode(const Vec3f& position, float zOffset, MoveNodeGroup& group);

    float _z{ 0.0f };
    uint32_t _firstSid{ 0 };
    uint32_t _lastSid{ 0 };

    std::vector<PathNode> _nodes;
    std::vector<MoveNodeGroup> _groups;
    std::vector<PathStep> _steps;

    // visibility-filtered step records, packed as vec4s:
    // path:  (startNode, endNode, hasPrevStep, prevStartNode)
    // marker: (node, node, 0, 0)
    std::vector<float> _pathStepRecords;
    std::vector<float> _markerStepRecords;
    // sid of each step record (records are ordered by sid), used to clip the
    // instanced draws to the sequential playback window
    std::vector<uint32_t> _pathRecordSids;
    std::vector<uint32_t> _markerRecordSids;
    // per-group attributes, packed as vec4:
    // (moveType, viewValue, deltaExtruder or role for FilamentId view, 0)
    std::vector<float> _attributeRecords;

    BufferTexture _nodeTable;        // RGBA32F: xyz + group index
    BufferTexture _widthHeightTable; // RG32F: extrusion width + height, per group
    BufferTexture _attributeTable;   // RGBA32F: per-move attributes, per group
    BufferTexture _pathStepTable;    // RGBA32F: path step records
    BufferTexture _markerStepTable;  // RGBA32F: marker step records

    bool _nodesDirty{ true };
    bool _widthHeightDirty{ true };
    bool _attributesDirty{ true };
    bool _stepTablesDirty{ true };
};

// The layers of one print with the current visibility windows and dirty
// flags. Owns the per-layer data tables and provides the sequential-playback
// move window (in sid space) used to clip the top layer.
class PathLayerStack
{
public:
    enum class EDirtyFlag : unsigned char
    {
        Layers     = 1 << 0, // layer set rebuilt or layer window changed
        Visibility = 1 << 1, // move-type / role / filament visibility changed
        MoveWindow = 1 << 2, // sequential move range changed (top layer clip)
        ViewType   = 1 << 3  // view type changed (attribute tables rebuild)
    };

    PathLayerStack() = default;

    PathLayerStack(const PathLayerStack&) = delete;
    PathLayerStack& operator=(const PathLayerStack&) = delete;

    // Splits the gcode result into layers and assembles nodes/groups/steps.
    void BuildFromResult(const GCodeProcessorResult& result);
    void Reset();

    size_t LayerCount() const { return _layers.size(); }
    const PathLayerData& Layer(size_t index) const { return *_layers[index]; }
    PathLayerData& Layer(size_t index) { return *_layers[index]; }
    // The sequential playback window only clips the top layer of the visible
    // range; lower layers always render in full.
    bool IsTopLayer(const PathLayerData& layer) const {
        return !_layers.empty() && &layer == _layers[_layerWindow.second].get();
    }

    // visibility windows
    void SetLayerWindow(uint32_t first, uint32_t last);
    void SetMoveWindow(uint32_t first, uint32_t last);
    std::pair<uint32_t, uint32_t> LayerWindow() const { return _layerWindow; }
    std::pair<uint32_t, uint32_t> MoveWindow() const { return _moveWindow; }

    // move-type visibility (defaults: Extrude and Seam visible, rest hidden,
    // matching the legacy pipeline)
    void SetMoveTypeVisible(EMoveType type, bool visible);
    bool IsMoveTypeVisible(EMoveType type) const;
    // extrusion-role visibility flags (all bits set = all visible)
    void SetRoleVisibilityFlags(uint32_t flags);
    uint32_t RoleVisibilityFlags() const { return _roleVisibilityFlags; }
    bool IsRoleVisible(ExtrusionRole role) const;
    void SetFilamentVisible(const std::vector<bool>& flags);
    const std::vector<bool>& FilamentVisible() const { return _filamentVisible; }

    // view type (a PathViewType index)
    void SetViewType(unsigned int viewType);
    unsigned int ViewType() const { return _viewType; }

    // Rebuilds the visible step lists of all layers when visibility or view
    // type changed. Cheap no-op otherwise.
    void RefreshVisibleSteps();
    // Rebuilds the per-move attribute tables of all layers when the view
    // type changed. Cheap no-op otherwise.
    void RefreshMoveAttributes(const GCodeProcessorResult& result);

    bool IsDirty(EDirtyFlag flag) const { return (_dirtyMask & static_cast<unsigned char>(flag)) != 0; }
    void MarkDirty(EDirtyFlag flag) { _dirtyMask |= static_cast<unsigned char>(flag); }
    void ClearDirty(EDirtyFlag flag) { _dirtyMask &= ~static_cast<unsigned char>(flag); }

    // raw move index of a sid (0 when out of range)
    uint32_t MoveIndexOfSid(uint32_t sid) const {
        return (sid < _sidToMoveIndex.size()) ? _sidToMoveIndex[sid] : 0;
    }

private:
    void BuildLayers(const GCodeProcessorResult& result);
    void AssembleSteps(const GCodeProcessorResult& result);

    std::vector<std::unique_ptr<PathLayerData>> _layers;
    std::pair<uint32_t, uint32_t> _layerWindow{ 0, 0 };
    std::pair<uint32_t, uint32_t> _moveWindow{ 0, 0 };

    uint32_t _moveTypeVisibilityFlags{ 0 };
    uint32_t _roleVisibilityFlags{ 0 };
    std::vector<bool> _filamentVisible;

    unsigned int _viewType{ PathViewType::FEATURE_TYPE };
    unsigned char _dirtyMask{ 0xff };
    // number of sids (move indices with seams skipped) in the current result
    uint32_t _sidCount{ 1 };

    // transient lookup tables used while assembling, freed afterwards:
    // sid -> move index, and the seam moves sorted by their sid
    std::vector<uint32_t> _sidToMoveIndex;
    std::vector<std::pair<uint32_t, uint32_t>> _seamMovesBySid;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_PathData_hpp_
