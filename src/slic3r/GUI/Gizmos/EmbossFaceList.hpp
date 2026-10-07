#pragma once
// Bookkeeping for the Text (emboss) tool's font list: the list of loadable faces and the parallel
// list of their UTF-8 names that the combo's search box filters.
//
// The search box (ImGuiWrapper::bbl_combo_with_filter) matches against the names list and hands back
// indices into it; the gizmo uses those indices to pick rows out of the faces list. That only works
// while the two lists hold the same fonts in the same order. The font-list cache path filled the
// faces but not the names, so after the first run every search matched nothing (blank dropdown).
// These helpers keep the two lists together; they are plain templates so they can be unit tested
// without wxWidgets or ImGui.

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace emboss_face_list {

// Make `names` the UTF-8 name of each face in `faces`, in the same order.
// Returns true when `names` had to change.
template<typename Face, typename NameOf>
bool sync_names(const std::vector<Face> &faces, std::vector<std::string> &names, NameOf name_of)
{
    bool same = names.size() == faces.size();
    for (size_t i = 0; same && i < faces.size(); ++i)
        same = names[i] == name_of(faces[i]);
    if (same)
        return false;
    names.clear();
    names.reserve(faces.size());
    for (const Face &face : faces)
        names.push_back(name_of(face));
    return true;
}

// Rows the font combo shows, as indices into the faces list, in display order.
// Without a filter that is every face; with one it is the filter's result, minus any index the
// faces list does not have (a stale result must never index past the end).
inline std::vector<int> visible_rows(bool is_filtered, const std::vector<int> &filtered_idx, size_t face_count)
{
    std::vector<int> rows;
    if (!is_filtered) {
        rows.reserve(face_count);
        for (size_t i = 0; i < face_count; ++i)
            rows.push_back(static_cast<int>(i));
        return rows;
    }
    rows.reserve(filtered_idx.size());
    for (int idx : filtered_idx)
        if (idx >= 0 && static_cast<size_t>(idx) < face_count)
            rows.push_back(idx);
    return rows;
}

// Remove face `idx` and its name together. Returns false (and changes nothing) for an index the
// faces list does not have. A names list that is out of step is rebuilt by the caller's next
// sync_names(); this only drops the matching entry when there is one.
template<typename Face>
bool erase_face(std::vector<Face> &faces, std::vector<std::string> &names, size_t idx)
{
    if (idx >= faces.size())
        return false;
    const bool in_step = names.size() == faces.size();
    faces.erase(faces.begin() + idx);
    if (in_step)
        names.erase(names.begin() + idx);
    else
        names.clear(); // let the next sync_names() rebuild it from the faces
    return true;
}

}}} // namespace Slic3r::GUI::emboss_face_list
