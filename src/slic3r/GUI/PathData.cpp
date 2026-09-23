#include "PathData.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace GUI {

namespace {

constexpr uint32_t INVALID_INDEX = 0xFFFFFFFFu;
// z tolerance used to detect layer boundaries, matching GCodeViewer
constexpr float LAYER_Z_EPSILON = 1e-4f;
// travel moves carry no real width/height; keep a minimal size so the
// generated prism stays visible
constexpr float TRAVEL_SIZE = 0.1f;

// exact copy of GCodeViewer's round_to_bin (the legacy path grouping bins
// heights/widths before comparing them)
float RoundToBin(const float value)
{
    constexpr float const scale    [5] = { 100.f,  1000.f,  10000.f,  100000.f,  1000000.f };
    constexpr float const invscale [5] = { 0.01f,  0.001f,  0.0001f,  0.00001f,  0.000001f };
    constexpr float const threshold[5] = { 0.095f, 0.0095f, 0.00095f, 0.000095f, 0.0000095f };
    int i = 0;
    for (; value < threshold[i] && i < 4; ++i);
    return std::round(value * scale[i]) * invscale[i];
}

// Path::matches() against a record's stored (run-start) attributes
bool LegacyPathMatches(const LegacyPathRecord& path, const GCodeProcessorResult::MoveVertex& move)
{
    switch (move.type)
    {
    case EMoveType::Travel:
        return path.feedrate == move.feedrate
            && path.extruderId == move.extruder_id
            && path.cpColorId == move.cp_color_id;
    case EMoveType::Extrude:
        return path.extruderId == move.extruder_id
            && path.cpColorId == move.cp_color_id
            && path.role == move.extrusion_role
            && move.position.z() <= path.zRef
            && path.feedrate == move.feedrate
            && path.fanSpeed == move.fan_speed
            && path.heightBin == RoundToBin(move.height)
            && path.widthBin == RoundToBin(move.width)
            && std::abs(move.volumetric_rate() - path.volumetricRate) / path.volumetricRate <= 0.05f
            && path.layerTime == move.layer_duration;
    default:
        return false;
    }
}

// moves rendered as diamond markers (options), everything else as prisms
bool IsOptionMove(EMoveType type)
{
    switch (type) {
    case EMoveType::Tool_change:
    case EMoveType::Color_change:
    case EMoveType::Pause_Print:
    case EMoveType::Custom_GCode:
    case EMoveType::Retract:
    case EMoveType::Unretract:
    case EMoveType::Seam:
        return true;
    default:
        return false;
    }
}

// value of the current view type for a move (drives the color ramp lookup)
float ViewValueOfMove(unsigned int viewType, const GCodeProcessorResult::MoveVertex& move)
{
    switch (viewType) {
    case PathViewType::HEIGHT:          return move.height;
    case PathViewType::WIDTH:           return move.width;
    case PathViewType::FEEDRATE:        return move.feedrate;
    case PathViewType::FAN_SPEED:       return move.fan_speed;
    case PathViewType::TEMPERATURE:     return move.temperature;
    case PathViewType::VOLUMETRIC_RATE: return move.volumetric_rate();
    case PathViewType::LAYER_TIME:      return move.layer_duration;
    case PathViewType::LAYER_TIME_LOG:  return move.layer_duration;
    case PathViewType::FEATURE_TYPE:    return float(move.extrusion_role);
    case PathViewType::TOOL:            return float(move.extruder_id);
    case PathViewType::COLOR_PRINT:     return float(move.cp_color_id);
    case PathViewType::FILAMENT_ID:     return float(move.extruder_id);
    default:                            return 0.0f;
    }
}

// Appends the GPU step records of one path step. Consecutive connected body
// steps share their junction node, so the previous record's end node equals
// the current record's start node; that links the pair for the miter fill
// computed in the vertex shader. Arc chains additionally emit one record per
// interpolation segment.
void AppendPathStepRecords(const PathStep& step,
                           const std::vector<MoveNodeGroup>& groups,
                           std::vector<float>& records,
                           std::vector<uint32_t>& recordSids,
                           uint32_t& prevRecordIndex)
{
    const auto& firstGroup = groups[step.firstGroup];
    const auto& secondGroup = groups[step.secondGroup];
    const uint32_t firstNode = firstGroup.nodeIndices.back();

    bool hasPrev = false;
    uint32_t prevFirstNode = 0;
    if (prevRecordIndex != INVALID_INDEX) {
        const uint32_t prevEndNode = static_cast<uint32_t>(records[prevRecordIndex * 4 + 1]);
        if (prevEndNode == firstNode) {
            hasPrev = true;
            prevFirstNode = static_cast<uint32_t>(records[prevRecordIndex * 4 + 0]);
        }
    }

    const auto appendRecord = [&](uint32_t startNode, uint32_t endNode, bool linked, uint32_t prevNode) {
        records.push_back(float(startNode));
        records.push_back(float(endNode));
        records.push_back(linked ? 1.0f : 0.0f);
        records.push_back(float(prevNode));
        recordSids.push_back(step.secondSid);
    };

    appendRecord(firstNode, secondGroup.nodeIndices.front(), hasPrev, prevFirstNode);
    prevRecordIndex = static_cast<uint32_t>(records.size() / 4 - 1);

    for (size_t k = 1; k < secondGroup.nodeIndices.size(); ++k) {
        const uint32_t prevRecordFirst = static_cast<uint32_t>(records[prevRecordIndex * 4 + 0]);
        appendRecord(secondGroup.nodeIndices[k - 1], secondGroup.nodeIndices[k], hasPrev, prevRecordFirst);
        ++prevRecordIndex;
    }
}

void AppendMarkerStepRecords(const PathStep& step,
                             const std::vector<MoveNodeGroup>& groups,
                             std::vector<float>& records,
                             std::vector<uint32_t>& recordSids)
{
    const uint32_t node = groups[step.secondGroup].nodeIndices.front();
    records.push_back(float(node));
    records.push_back(float(node));
    records.push_back(0.0f);
    records.push_back(0.0f);
    recordSids.push_back(step.secondSid);
}

} // namespace

// ----------------------------------------------------------------------------
// PathLayerData
// ----------------------------------------------------------------------------

uint32_t PathLayerData::AddNode(const Vec3f& position, float zOffset, MoveNodeGroup& group)
{
    PathNode node;
    node.position = position;
    node.position.z() += zOffset;
    node.groupIndex = static_cast<uint32_t>(_groups.size());
    _nodes.push_back(node);
    group.nodeIndices.push_back(static_cast<uint32_t>(_nodes.size() - 1));
    return group.nodeIndices.back();
}

uint32_t PathLayerData::AddMoveNodeGroup(uint32_t moveIndex, const GCodeProcessorResult::MoveVertex& move)
{
    MoveNodeGroup group;
    group.moveIndex = moveIndex;

    // wipe blobs are lifted above the path by half their height
    const float zOffset = (move.type == EMoveType::Wipe) ? 0.5f * GCodeProcessor::Wipe_Height : 0.0f;

    if (move.is_arc_move_with_interpolation_points())
        for (const Vec3f& point : move.interpolation_points)
            AddNode(point, zOffset, group);

    AddNode(move.position, zOffset, group);

    _groups.push_back(std::move(group));
    return static_cast<uint32_t>(_groups.size() - 1);
}

void PathLayerData::RefreshVisibleSteps(const PathLayerStack& stack)
{
    _pathStepRecords.clear();
    _markerStepRecords.clear();
    _pathRecordSids.clear();
    _markerRecordSids.clear();
    uint32_t prevRecordIndex = INVALID_INDEX;

    const std::vector<bool>& filamentVisible = stack.FilamentVisible();
    const bool filterByFilament = (stack.ViewType() == PathViewType::COLOR_PRINT)
        && !filamentVisible.empty();

    for (const PathStep& step : _steps) {
        if (!stack.IsMoveTypeVisible(step.type))
            continue;
        if (step.type == EMoveType::Extrude && !stack.IsRoleVisible(step.role))
            continue;
        if (filterByFilament && step.extruderId < filamentVisible.size()
            && !filamentVisible[step.extruderId])
            continue;

        if (IsOptionMove(step.type))
            AppendMarkerStepRecords(step, _groups, _markerStepRecords, _markerRecordSids);
        else
            AppendPathStepRecords(step, _groups, _pathStepRecords, _pathRecordSids, prevRecordIndex);
    }
    _stepTablesDirty = true;
}

unsigned int PathLayerData::PathStepCountUpTo(uint32_t sidLast) const
{
    // records are ordered by sid: count the prefix up to sidLast
    const auto it = std::upper_bound(_pathRecordSids.begin(), _pathRecordSids.end(), sidLast);
    return static_cast<unsigned int>(it - _pathRecordSids.begin());
}

unsigned int PathLayerData::MarkerStepCountUpTo(uint32_t sidLast) const
{
    const auto it = std::upper_bound(_markerRecordSids.begin(), _markerRecordSids.end(), sidLast);
    return static_cast<unsigned int>(it - _markerRecordSids.begin());
}

void PathLayerData::RefreshMoveAttributes(const GCodeProcessorResult& result, unsigned int viewType)
{
    _attributeRecords.assign(_groups.size() * 4, 0.0f);
    const bool filamentIdView = (viewType == PathViewType::FILAMENT_ID);

    // ColorPlay view: legacy collects the zs of Pause_Print / Custom_GCode
    // moves (options_zs) and inverts cp_color_id on the extrude paths of
    // those layers (always 0 -> 255 in this fork), which makes them render
    // gray (the id then exceeds the tool-color count). Replicate by forcing
    // the view value to 255 for the extrusions of such layers -- the gray
    // fallback already exists in gpu_path.fs.
    const bool colorPlayGray = (viewType == PathViewType::COLOR_PRINT);
    bool layerHasOptionMove = false;
    if (colorPlayGray) {
        for (const MoveNodeGroup& group : _groups) {
            const EMoveType type = result.moves[group.moveIndex].type;
            if (type == EMoveType::Pause_Print || type == EMoveType::Custom_GCode) {
                layerHasOptionMove = true;
                break;
            }
        }
    }

    for (size_t groupIndex = 0; groupIndex < _groups.size(); ++groupIndex) {
        const GCodeProcessorResult::MoveVertex& move = result.moves[_groups[groupIndex].moveIndex];
        float* record = &_attributeRecords[groupIndex * 4];
        record[0] = float(move.type);
        record[1] = ViewValueOfMove(viewType, move);
        if (layerHasOptionMove && move.type == EMoveType::Extrude)
            record[1] = 255.0f;
        // the FilamentId debug view encodes the role in the third channel
        record[2] = filamentIdView ? float(move.extrusion_role) : move.delta_extruder;
        record[3] = 0.0f;
    }
    _attributesDirty = true;
}

void PathLayerData::UploadTables(const GCodeProcessorResult& result)
{
    // node table: xyz + group index (the group index is what the shaders use
    // to sample the width/height and attribute tables)
    if (_nodesDirty && !_nodes.empty()) {
        std::vector<float> data;
        data.reserve(_nodes.size() * 4);
        for (const PathNode& node : _nodes) {
            data.push_back(node.position.x());
            data.push_back(node.position.y());
            data.push_back(node.position.z());
            data.push_back(float(node.groupIndex));
        }
        _nodeTable.Upload(data, BufferTexture::EFormat::RGBA32F);
        _nodesDirty = false;
    }

    // width/height table, per group
    if (_widthHeightDirty && !_groups.empty()) {
        std::vector<float> data;
        data.reserve(_groups.size() * 2);
        for (const MoveNodeGroup& group : _groups) {
            const GCodeProcessorResult::MoveVertex& move = result.moves[group.moveIndex];
            const bool isTravel = (move.type == EMoveType::Travel);
            data.push_back(isTravel ? TRAVEL_SIZE : move.width);
            data.push_back(isTravel ? TRAVEL_SIZE : move.height);
        }
        _widthHeightTable.Upload(data, BufferTexture::EFormat::RG32F);
        _widthHeightDirty = false;
    }

    if (_attributesDirty && !_attributeRecords.empty()) {
        _attributeTable.Upload(_attributeRecords, BufferTexture::EFormat::RGBA32F);
        _attributesDirty = false;
    }

    if (_stepTablesDirty) {
        if (!_pathStepRecords.empty())
            _pathStepTable.Upload(_pathStepRecords, BufferTexture::EFormat::RGBA32F);
        if (!_markerStepRecords.empty())
            _markerStepTable.Upload(_markerStepRecords, BufferTexture::EFormat::RGBA32F);
        _stepTablesDirty = false;
    }
}

void PathLayerData::Reset()
{
    _z = 0.0f;
    _firstSid = 0;
    _lastSid = 0;
    _nodes.clear();
    _groups.clear();
    _steps.clear();
    _pathStepRecords.clear();
    _markerStepRecords.clear();
    _pathRecordSids.clear();
    _markerRecordSids.clear();
    _attributeRecords.clear();
    _nodeTable.Reset();
    _widthHeightTable.Reset();
    _attributeTable.Reset();
    _pathStepTable.Reset();
    _markerStepTable.Reset();
    _nodesDirty = true;
    _widthHeightDirty = true;
    _attributesDirty = true;
    _stepTablesDirty = true;
}

// ----------------------------------------------------------------------------
// PathLayerStack
// ----------------------------------------------------------------------------

void PathLayerStack::BuildFromResult(const GCodeProcessorResult& result)
{
    Reset();
    _result = &result;
    BuildLayers(result);
    if (_layers.empty())
        return;

    AssembleSteps(result);

    // the sid -> move index map, the seam list and the legacy path records
    // are kept for sequential-view queries (marker position, current move,
    // slider endpoints)
    BuildLegacyPaths(result);

    _layerWindow = { 0, static_cast<uint32_t>(_layers.size() - 1) };
    _moveWindow = { 0, static_cast<uint32_t>(_sidCount - 1) };
}

void PathLayerStack::BuildLayers(const GCodeProcessorResult& result)
{
    const size_t moveCount = result.moves.size();
    if (moveCount == 0)
        return;

    // sids are move indices with seam moves skipped, matching the space of
    // GCodeViewer::SequentialView::gcode_ids
    _sidToMoveIndex.clear();
    _sidToMoveIndex.reserve(moveCount);
    // compact seam list: (sid, raw move index), naturally sorted by sid
    // (a per-move vector-of-vectors would allocate ~24 bytes per move)
    _seamMovesBySid.clear();
    _sidCount = 0;

    uint32_t seamsCount = 0;
    uint32_t lastTravelSid = 0;
    for (size_t i = 0; i < moveCount; ++i) {
        const GCodeProcessorResult::MoveVertex& move = result.moves[i];

        if (move.type == EMoveType::Seam) {
            // a seam attaches to the sid of the move that follows it
            ++seamsCount;
            _seamMovesBySid.emplace_back(static_cast<uint32_t>(i - seamsCount), static_cast<uint32_t>(i));
            continue;
        }

        const uint32_t sid = static_cast<uint32_t>(i - seamsCount);
        _sidToMoveIndex.push_back(static_cast<uint32_t>(i));
        _sidCount = sid + 1;

        if (move.type == EMoveType::Extrude) {
            const float z = move.position.z();
            const bool needsNewLayer = _layers.empty()
                || std::abs(_layers.back()->_z - z) > LAYER_Z_EPSILON;
            if (needsNewLayer) {
                auto layer = std::make_unique<PathLayerData>();
                layer->_z = z;
                layer->_firstSid = lastTravelSid;
                layer->_lastSid = sid;
                _layers.push_back(std::move(layer));
            }
            else {
                _layers.back()->_lastSid = sid;
            }
        }
        else if (move.type == EMoveType::Travel) {
            // every travel extends the layer (the fork's
            // extract_layer_metadata uses gap > 0): trailing travel chains
            // (e.g. z-hop: lift, xy move, lower) belong to the layer and the
            // slider endpoints use the same range
            if (!_layers.empty())
                _layers.back()->_lastSid = sid;
            lastTravelSid = sid;
        }
    }

    // spiral vase prints define their own layer boundaries
    if (!result.spiral_vase_layers.empty()) {
        _layers.clear();
        for (const auto& entry : result.spiral_vase_layers) {
            auto layer = std::make_unique<PathLayerData>();
            layer->_z = entry.first;
            layer->_firstSid = static_cast<uint32_t>(entry.second.first);
            layer->_lastSid = static_cast<uint32_t>(entry.second.second);
            _layers.push_back(std::move(layer));
        }
    }
}

void PathLayerStack::AssembleSteps(const GCodeProcessorResult& result)
{
    for (auto& layer : _layers) {
        if (layer->_lastSid < layer->_firstSid || layer->_lastSid >= _sidToMoveIndex.size())
            continue;

        const uint32_t sidSpan = layer->_lastSid - layer->_firstSid + 1;
        std::vector<uint32_t> sidToGroup(sidSpan, INVALID_INDEX);
        std::vector<std::vector<uint32_t>> seamGroupsOfSid(sidSpan);

        // the compact (sid, move) seam list is sorted: walk it with a cursor
        // while the sids ascend
        size_t seamCursor = std::lower_bound(_seamMovesBySid.begin(), _seamMovesBySid.end(),
            std::make_pair(layer->_firstSid, 0u)) - _seamMovesBySid.begin();

        // node groups, in sid order; the seam groups of a sid follow its own
        for (uint32_t sid = layer->_firstSid, offset = 0; sid <= layer->_lastSid; ++sid, ++offset) {
            const uint32_t moveIndex = _sidToMoveIndex[sid];
            sidToGroup[offset] = layer->AddMoveNodeGroup(moveIndex, result.moves[moveIndex]);
            while (seamCursor < _seamMovesBySid.size() && _seamMovesBySid[seamCursor].first == sid) {
                const uint32_t seamMoveIndex = _seamMovesBySid[seamCursor].second;
                const GCodeProcessorResult::MoveVertex& seamMove = result.moves[seamMoveIndex];
                seamGroupsOfSid[offset].push_back(layer->AddMoveNodeGroup(seamMoveIndex, seamMove));
                ++seamCursor;
            }
        }

        // steps connect consecutive sids; the step takes the attributes of
        // its ending move. The seam steps of a sid follow the step ending at
        // that sid.
        for (uint32_t sid = layer->_firstSid + 1, offset = 1; sid <= layer->_lastSid; ++sid, ++offset) {
            const uint32_t endGroupIndex = sidToGroup[offset];
            const GCodeProcessorResult::MoveVertex& move = result.moves[layer->_groups[endGroupIndex].moveIndex];

            PathStep step;
            step.firstGroup = sidToGroup[offset - 1];
            step.secondGroup = endGroupIndex;
            step.firstSid = sid - 1;
            step.secondSid = sid;
            step.type = move.type;
            step.role = move.extrusion_role;
            step.extruderId = move.extruder_id;
            layer->AppendStep(step);

            for (const uint32_t seamGroupIndex : seamGroupsOfSid[offset]) {
                const GCodeProcessorResult::MoveVertex& seamMove = result.moves[layer->_groups[seamGroupIndex].moveIndex];
                PathStep seamStep;
                seamStep.firstGroup = seamGroupIndex;
                seamStep.secondGroup = seamGroupIndex;
                seamStep.firstSid = sid;
                seamStep.secondSid = sid;
                seamStep.type = seamMove.type;
                seamStep.role = seamMove.extrusion_role;
                seamStep.extruderId = seamMove.extruder_id;
                layer->AppendStep(seamStep);
            }
        }

        layer->_nodesDirty = true;
        layer->_widthHeightDirty = true;
        layer->_attributesDirty = true;
        layer->_stepTablesDirty = true;
    }
}

void PathLayerStack::Reset()
{
    _layers.clear();
    _layerWindow = { 0, 0 };
    _moveWindow = { 0, 0 };
    _sidCount = 1;
    _anyVisibleSteps = false;

    // defaults matching the legacy pipeline: extrusions and seams visible,
    // travel/wipe/options hidden; all roles visible
    _moveTypeVisibilityFlags = 0;
    _moveTypeVisibilityFlags |= 1u << static_cast<uint32_t>(EMoveType::Extrude);
    _moveTypeVisibilityFlags |= 1u << static_cast<uint32_t>(EMoveType::Seam);
    _roleVisibilityFlags = 0;
    for (uint32_t role = 0; role < static_cast<uint32_t>(erCount); ++role)
        _roleVisibilityFlags |= 1u << role;
    _filamentVisible.clear();
    _viewType = PathViewType::FEATURE_TYPE;
    _dirtyMask = 0xff;
    _sidToMoveIndex.clear();
    _seamMovesBySid.clear();
    _legacyPaths.clear();
    _travelPathIndices.clear();
    _result = nullptr;
}

void PathLayerStack::SetLayerWindow(uint32_t first, uint32_t last)
{
    if (_layers.empty()) {
        _layerWindow = { 0, 0 };
        return;
    }
    first = std::min(first, static_cast<uint32_t>(_layers.size() - 1));
    last = std::min(last, static_cast<uint32_t>(_layers.size() - 1));
    if (first > last)
        std::swap(first, last);
    // pure window change: the render loop only iterates the window, no
    // per-layer table needs rebuilding
    _layerWindow = { first, last };
}

void PathLayerStack::SetMoveWindow(uint32_t first, uint32_t last)
{
    if (first > last)
        std::swap(first, last);
    if (_moveWindow != std::make_pair(first, last)) {
        _moveWindow = { first, last };
        MarkDirty(EDirtyFlag::MoveWindow);
    }
}

void PathLayerStack::SetMoveTypeVisible(EMoveType type, bool visible)
{
    const uint32_t bit = 1u << static_cast<uint32_t>(type);
    const bool changed = ((_moveTypeVisibilityFlags & bit) != 0) != visible;
    if (visible)
        _moveTypeVisibilityFlags |= bit;
    else
        _moveTypeVisibilityFlags &= ~bit;
    if (changed)
        MarkDirty(EDirtyFlag::Visibility);
}

bool PathLayerStack::IsMoveTypeVisible(EMoveType type) const
{
    return (_moveTypeVisibilityFlags & (1u << static_cast<uint32_t>(type))) != 0;
}

void PathLayerStack::SetRoleVisibilityFlags(uint32_t flags)
{
    if (_roleVisibilityFlags != flags) {
        _roleVisibilityFlags = flags;
        MarkDirty(EDirtyFlag::Visibility);
    }
}

bool PathLayerStack::IsRoleVisible(ExtrusionRole role) const
{
    return role < erCount && (_roleVisibilityFlags & (1u << static_cast<uint32_t>(role))) != 0;
}

void PathLayerStack::SetFilamentVisible(const std::vector<bool>& flags)
{
    if (_filamentVisible != flags) {
        _filamentVisible = flags;
        MarkDirty(EDirtyFlag::Visibility);
    }
}

void PathLayerStack::SetViewType(unsigned int viewType)
{
    if (_viewType != viewType) {
        _viewType = viewType;
        MarkDirty(EDirtyFlag::ViewType);
    }
}

// Builds the legacy render-path records with exactly load_toolpaths'
// grouping: a run opens when (no open run) || (previous RAW move type
// differs, so seams and option moves break runs) || (!Path::matches), and
// records start = (first move sid - 1), end = (last move sid). Option moves
// form no paths (legacy renders them as instances only).
void PathLayerStack::BuildLegacyPaths(const GCodeProcessorResult& result)
{
    _legacyPaths.clear();
    _travelPathIndices.clear();
    const auto& moves = result.moves;
    if (moves.size() < 2)
        return;

    uint32_t seamsCount = 0;
    // legacy counts the seam of the very first move too (its seam counting
    // runs before the i == 0 skip); missing it shifts every record sid by 1
    if (moves[0].type == EMoveType::Seam)
        ++seamsCount;
    for (size_t i = 1; i < moves.size(); ++i) { // legacy skips the first move
        const GCodeProcessorResult::MoveVertex& move = moves[i];
        if (move.type == EMoveType::Seam) {
            ++seamsCount;
            continue;
        }
        const uint32_t sid = static_cast<uint32_t>(i - seamsCount);
        const GCodeProcessorResult::MoveVertex& prev = moves[i - 1];

        const bool opensNewRun = _legacyPaths.empty()
            || prev.type != move.type
            || !LegacyPathMatches(_legacyPaths.back(), move);

        if (opensNewRun) {
            if (IsOptionMove(move.type) || move.type == EMoveType::Noop)
                continue; // option/noop moves form no legacy paths

            LegacyPathRecord record;
            record.firstSid = (sid > 0) ? sid - 1 : 0;
            record.lastSid = sid;
            record.type = move.type;
            record.role = move.extrusion_role;
            record.extruderId = move.extruder_id;
            record.cpColorId = move.cp_color_id;
            record.feedrate = move.feedrate;
            record.fanSpeed = move.fan_speed;
            record.heightBin = RoundToBin(move.height);
            record.widthBin = RoundToBin(move.width);
            record.volumetricRate = move.volumetric_rate();
            record.layerTime = move.layer_duration;
            record.zRef = prev.position.z();
            record.startPosition = prev.position;
            record.endPosition = move.position;
            if (move.type == EMoveType::Travel)
                _travelPathIndices.push_back(static_cast<uint32_t>(_legacyPaths.size()));
            _legacyPaths.push_back(record);
        }
        else {
            LegacyPathRecord& open = _legacyPaths.back();
            open.lastSid = sid;
            open.endPosition = move.position;
        }
    }
}

// Sequential-slider endpoints of a layer, replicating the legacy first-pass
// selection: min/max over the layer's visible legacy paths (travels gated
// by the position-connected chain overlap, everything else by both sids
// inside the legacy layer range) plus visible option/seam instances.
std::pair<uint32_t, uint32_t> PathLayerStack::ComputeSliderEndpoints(uint32_t layerIndex) const
{
    if (layerIndex >= _layers.size())
        return { 0, 0 };

    const uint32_t layerFirst = _layers[layerIndex]->FirstSid();
    const uint32_t layerLast = _layers[layerIndex]->LegacyLastSid();

    const bool filterFilament = (_viewType == PathViewType::COLOR_PRINT) && !_filamentVisible.empty();
    uint32_t bestFirst = INVALID_INDEX;
    uint32_t bestLast = 0;

    for (size_t i = 0; i < _legacyPaths.size(); ++i) {
        const LegacyPathRecord& path = _legacyPaths[i];
        if (!IsMoveTypeVisible(path.type))
            continue;
        if (path.type == EMoveType::Extrude && !IsRoleVisible(path.role))
            continue;
        if (filterFilament && path.extruderId < _filamentVisible.size()
            && !_filamentVisible[path.extruderId])
            continue;

        bool inRange;
        if (path.type == EMoveType::Travel) {
            // position-connected chain, ported from is_travel_in_layers_range
            // (chains run through the travel paths only, comparing shared
            // end positions)
            size_t chainIdx = i;
            while (chainIdx > 0) {
                // the previous travel record in move order
                size_t prev = chainIdx - 1;
                while (prev != 0 && _legacyPaths[prev].type != EMoveType::Travel)
                    --prev;
                if (_legacyPaths[prev].type != EMoveType::Travel)
                    break;
                if (!_legacyPaths[chainIdx].startPosition.isApprox(_legacyPaths[prev].endPosition))
                    break;
                chainIdx = prev;
            }
            size_t chainEnd = i;
            while (chainEnd + 1 < _legacyPaths.size()) {
                size_t next = chainEnd + 1;
                while (next + 1 < _legacyPaths.size() && _legacyPaths[next].type != EMoveType::Travel)
                    ++next;
                if (_legacyPaths[next].type != EMoveType::Travel)
                    break;
                if (!_legacyPaths[chainEnd].endPosition.isApprox(_legacyPaths[next].startPosition))
                    break;
                chainEnd = next;
            }
            const uint32_t chainFirstSid = _legacyPaths[chainIdx].firstSid;
            const uint32_t chainLastSid = _legacyPaths[chainEnd].lastSid;
            // legacy uses AND: BOTH chain endpoints must lie inside the
            // layer range (this fork changed upstream's OR check)
            inRange = (layerFirst <= chainFirstSid && chainFirstSid <= layerLast)
                && (layerFirst <= chainLastSid && chainLastSid <= layerLast);
        }
        else {
            // is_in_layers_range: both recorded sids inside the layer range
            inRange = layerFirst <= path.firstSid && path.lastSid <= layerLast;
        }

        if (!inRange)
            continue;
        if (path.firstSid < bestFirst) bestFirst = path.firstSid;
        if (path.lastSid > bestLast) bestLast = path.lastSid;
    }

    // visible option-marker instances (their own sids; a seam carries the
    // sid of the move that follows it)
    if (_result != nullptr) {
        for (uint32_t sid = layerFirst; sid <= layerLast && sid < _sidToMoveIndex.size(); ++sid) {
            const uint32_t moveIndex = _sidToMoveIndex[sid];
            if (moveIndex >= _result->moves.size())
                break;
            const GCodeProcessorResult::MoveVertex& move = _result->moves[moveIndex];
            if (move.type != EMoveType::Seam && IsOptionMove(move.type) && IsMoveTypeVisible(move.type)) {
                if (sid < bestFirst) bestFirst = sid;
                if (sid > bestLast) bestLast = sid;
            }
        }
        if (IsMoveTypeVisible(EMoveType::Seam)) {
            for (const auto& seam : _seamMovesBySid) {
                if (seam.first < layerFirst)
                    continue;
                if (seam.first > layerLast)
                    break; // sorted by sid
                if (seam.first < bestFirst) bestFirst = seam.first;
                if (seam.first > bestLast) bestLast = seam.first;
            }
        }
    }

    if (bestFirst == INVALID_INDEX)
        return { 0, 0 };
    return { bestFirst, bestLast };
}

void PathLayerStack::RefreshVisibleSteps()
{
    const bool needsRefresh = IsDirty(EDirtyFlag::Layers)
        || IsDirty(EDirtyFlag::Visibility)
        || IsDirty(EDirtyFlag::ViewType);
    if (!needsRefresh)
        return;

    for (auto& layer : _layers)
        layer->RefreshVisibleSteps(*this);

    // maintained here (inside the dirty guard) so it costs nothing per
    // frame: "any layer holds a visible path or marker record"
    _anyVisibleSteps = false;
    for (const auto& layer : _layers) {
        if (layer->PathStepRecordCount() > 0 || layer->MarkerStepRecordCount() > 0) {
            _anyVisibleSteps = true;
            break;
        }
    }

    ClearDirty(EDirtyFlag::Layers);
    ClearDirty(EDirtyFlag::Visibility);
    ClearDirty(EDirtyFlag::MoveWindow);
    // ViewType stays set until RefreshMoveAttributes() consumed it
}

void PathLayerStack::RefreshMoveAttributes(const GCodeProcessorResult& result)
{
    if (!IsDirty(EDirtyFlag::ViewType))
        return;

    for (auto& layer : _layers)
        layer->RefreshMoveAttributes(result, _viewType);

    ClearDirty(EDirtyFlag::ViewType);
}

} // namespace GUI
} // namespace Slic3r
