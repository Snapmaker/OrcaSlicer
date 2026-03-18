#ifndef slic3r_OrcaLocalHandler_hpp_
#define slic3r_OrcaLocalHandler_hpp_

#include <wx/webview.h>
#include <wx/filesys.h>

namespace Slic3r {
namespace GUI {

/**
 * Custom scheme handler: orca://app/path
 * Serves files from a local directory on-demand (no memory buffering).
 * - orca://app/xxx         -> rootPath/xxx (Flutter build)
 * - orca://app/user_assets/xxx -> userAssetsRoot/xxx (用户选择的文件，复制到 temp)
 */
class OrcaLocalHandler : public wxWebViewHandler {
public:
    OrcaLocalHandler(const wxString& scheme, const wxString& rootPath,
                     const wxString& userAssetsRoot = wxEmptyString);

    wxFSFile* GetFile(const wxString& uri) override;

private:
    wxString m_root;
    wxString m_userAssetsRoot;
};

} // namespace GUI
} // namespace Slic3r

#endif /* slic3r_OrcaLocalHandler_hpp_ */
