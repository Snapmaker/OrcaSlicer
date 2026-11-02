// ArrangeRequest/OrientRequest — 摆放前置语义的显式传递值（阶段 F3，手册《Plater阶段F3-ArrangeRequest显式化实施方案》§2.4）。
// 取代 Plater 旧 prepare_state 隐式全局状态（DEFAULT/MENU 两态枚举已删）：语义随调用显式传递。
// 两类型异构禁统一：Arrange 缺省=全部盘（原 DEFAULT），Orient 缺省=选中对象（原 DEFAULT）——统一会把键盘 A（全部盘）与 Q（选中对象）混同（路线指引红线）。
// 纯值类型头：wx 零依赖、零 include（不拖宿主头，hpp 侧禁 include 本头见 Plater.hpp 前置声明先例）。
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
