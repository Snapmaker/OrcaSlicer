// BatchMatchCoordinator — 混色批量匹配编排服务（阶段 B3）。
// 承接：Sidebar「Color Mixing Match」按钮 handler 编排段 + cleanup_unused_filaments_after_batch_match。
// 依赖规则：本 hpp 零宿主 include（仅前向声明）；cpp 级允许 Sidebar/Plater/GUI_App（B2/B1 cpp-only 先例，
// 该服务是 Sidebar 侧编排物，G1 再随耗材域进一步收编）。
#pragma once

#include <functional>

namespace Slic3r {
namespace GUI {

class Sidebar;
struct BatchMatchResult;

namespace app {

class BatchMatchCoordinator {
public:
    explicit BatchMatchCoordinator(Sidebar& sidebar);

    // 原 Sidebar 构造函数内 m_btn_batch_match 的 Bind lambda 体（整体迁入）
    void run_batch_match();

    // 原 Sidebar::cleanup_unused_filaments_after_batch_match（Sidebar 公有门面一行转发）
    void cleanup_unused_filaments_after_batch_match(const BatchMatchResult& match_result,
                                                    std::function<void(int /*current*/, int /*total*/)> on_progress = nullptr);

private:
    Sidebar& m_sidebar;
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
