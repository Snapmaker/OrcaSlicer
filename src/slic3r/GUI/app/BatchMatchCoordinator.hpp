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

    void run_batch_match();

    void cleanup_unused_filaments_after_batch_match(const BatchMatchResult& match_result,
                                                    std::function<void(int /*current*/, int /*total*/)> on_progress = nullptr);

private:
    Sidebar& m_sidebar;
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
