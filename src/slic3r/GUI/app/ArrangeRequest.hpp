#pragma once

namespace Slic3r {
namespace GUI {
namespace app {

struct ArrangeRequest {
    enum class Scope { AllPlates, CurrentPlate };   // 全部盘（原 DEFAULT） / 当前盘（原 MENU）
    Scope scope{Scope::AllPlates};
    static ArrangeRequest all_plates();
    static ArrangeRequest current_plate();
};

struct OrientRequest {
    enum class Scope { Selection, CurrentPlate };   // 选中对象（原 DEFAULT） / 当前盘（原 MENU）——与 Arrange 异构，禁统一
    Scope scope{Scope::Selection};
    static OrientRequest selection();
    static OrientRequest current_plate();
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
