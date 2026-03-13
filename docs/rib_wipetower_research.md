# Rib擦除塔优化调研报告

## 调研目标
对比BambuStudio、SoftFever/OrcaSlicer和当前OrcaSlicer的WipeTower实现，找出rib擦除塔的问题根源并制定修复方案。

## 执行摘要

### 当前问题
1. **边界溢出**: rib模式下某些参数触发超出擦除塔边界的挤出线
2. **形状不正方**: 擦除塔无法保持正方形
3. **内部设计差异**: 与BambuStudio的实现存在关键差异

### 根本原因
1. **缺少rib_offset补偿**: WipeTower2注释掉了rib偏移计算，始终返回零向量
2. **y_shift不一致**: 对角线应用了y_shift，但基础矩形没有
3. **边界检查缺失**: `line.extend()`扩展对角线时没有边界约束
4. **正方形维护算法缺失**: 没有实现BambuStudio的`sqrt(depth × width)`公式

---

## 一、架构对比

### 1.1 类层次结构

| 特性 | BambuStudio WipeTower | SoftFever WipeTower2 | 当前 WipeTower2 |
|------|----------------------|---------------------|----------------|
| 代码行数 | 4,468 | 2,625 | 2,663 |
| 目标平台 | Bambu Lab打印机 | 通用MMU/换刀器 | 通用MMU/换刀器 |
| Writer类 | WipeTowerWriter | WipeTowerWriter2 | WipeTowerWriter2 |
| 状态管理 | m_outer_wall map | m_first_layer_bbx cache | m_first_layer_bbx cache |
| rib_offset | 计算并使用 | 注释掉，返回零 | 注释掉，返回零 |

### 1.2 构造函数差异

**BambuStudio:**
```cpp
WipeTower(const PrintConfig& config,
          int plate_idx, Vec3d plate_origin,
          size_t initial_tool,
          const float wipe_tower_height,
          const std::vector<unsigned int>& slice_used_filaments);
```

**WipeTower2:**
```cpp
WipeTower2(const PrintConfig& config,
          const PrintRegionConfig& default_region_config,
          int plate_idx, Vec3d plate_origin,
          const std::vector<std::vector<float>>& wiping_matrix,
          size_t initial_tool);
```

**关键差异:**
- WT2直接接收`PrintRegionConfig`和`wiping_matrix`
- WT接收`wipe_tower_height`和`slice_used_filaments`
- 初始化策略不同

---

## 二、Rib实现详细对比

### 2.1 BambuStudio的Rib实现（参考标准）

#### 几何结构 (`rib_section()` - lines 1481-1521)
创建16点多边形截面：
- 基础矩形: width × depth
- 两条对角线肋: 从角到角交叉
- 肋宽度: 可配置 (m_rib_width)
- 肋长度: 超出对角线 `(rib_length - diagonal) / 2`
- 可选圆角: 平滑拐角

**关键算法:**
```cpp
// 计算对角线方向
Vec2f diag_dir = Vec2f{width, depth}.normalized();
Vec2f diag_dir_perp = diag_dir.perpendicular();

// 扩展肋条垂直于对角线
// 创建4个角区域（每个4点）带肋扩展
// 应用rounding_polygon()如果启用圆角
```

#### 正方形维护 (`plan_tower_new()` - lines 4117-4199)

**关键公式 (line 4125):**
```cpp
float square_width = align_ceil(
    std::sqrt(max_depth * m_wipe_tower_width * m_extra_spacing),
    m_perimeter_width
);
m_wipe_tower_width = square_width;
```

这确保塔保持**正方形**通过：
- 计算所有清洗操作所需的总深度
- 计算宽度使得 `width² ≈ width × depth`（正方形面积）
- 对齐到周长宽度以获得干净的挤出线

#### Rib多边形生成 (`generate_rib_polygon()` - lines 4645-4675)

**动态Rib锥化:**
```cpp
auto get_current_layer_rib_len = [](float cur_height, float max_height, float max_len) -> float {
    return std::abs(max_height - cur_height) / max_height * max_len;
};
```

- 肋条在**底部最长，顶部最短**
- 线性锥化: `rib_length = (剩余高度 / 总高度) × 最大肋长度`
- 创建两条对角线，按锥化长度扩展
- 在对角线周围生成矩形，宽度为`m_rib_width/2`
- 与基础矩形合并形成完整多边形

#### 边界检查与挤出

**WipeTowerWriter类 (lines 523-1356):**
- 跟踪当前位置 (`m_current_pos`)
- 应用围绕塔中心的内部旋转
- 计算挤出流量: `e = length × m_extrusion_flow`
- 宽度修正: `width = e × filament_area / (length × layer_height) + layer_height × (1 - π/4)`

**边界管理:**
- 使用`box_coordinates`结构体 (lines 126-155) 表示塔边界
- `cleaning_box`定义塔内清洗区域
- 挤出线通过坐标检查保持在边界内
- `wipe_tower_wall_infill_overlap`允许与周长轻微重叠

### 2.2 当前WipeTower2的Rib实现

#### 实现细节 (lines 2478-2511)

**关键算法:**
```cpp
// 1. 创建两条对角线
Line line_1(Point{0,0}, Point{width, depth});  // 左下到右上
Line line_2(Point{width,0}, Point{0, depth});  // 右下到左上

// 2. 计算扩展长度（随高度锥化）
float diagonal_extra_length = max(0, m_rib_length - line_1.length()) / 2;
diagonal_extra_length = get_current_layer_rib_len(z_pos, height, diagonal_extra_length);

// 3. 扩展线条并应用y_shift
line_1.extend(diagonal_extra_length);
line_2.extend(diagonal_extra_length);
line_1.translate(-y_shift);  // ⚠️ 问题：只对角线移位
line_2.translate(-y_shift);

// 4. 在线条周围创建矩形（宽度 = m_rib_width/2）
Polygon poly_1 = generate_rectange(line_1, m_rib_width/2);
Polygon poly_2 = generate_rectange(line_2, m_rib_width/2);

// 5. 与基础矩形合并
Polygon base = {ld, rd, ru, lu};  // ⚠️ 问题：基础矩形未移位
return union_({poly_1, poly_2, base}).front();
```

**锥化函数:**
```cpp
get_current_layer_rib_len = [](float cur_height, float max_height, float max_len) {
    return abs(max_height - cur_height) / max_height * max_len;
};
```
创建**金字塔效果** - 肋条在底部延伸最远，在顶部锥化为零。

---

## 三、已识别问题详细分析

### 问题1: 挤出线超出塔边界 ⚠️ 严重

**根本原因:** `line.extend()`操作在`diagonal_extra_length > 0`时将对角线扩展**超出塔占地面积**。

**问题位置:** `WipeTower2.cpp` lines 2492-2493
```cpp
line_1.extend(double(diagonal_extra_length));  // 扩展两端
line_2.extend(double(diagonal_extra_length));
```

**触发条件:**
- `m_rib_length`（计算为`sqrt(width² + depth²) + m_extra_rib_length`）超过自然对角线长度
- `m_extra_rib_length`为正（用户可配置，默认0）

**影响:**
1. 扩展的对角线创建的矩形（`poly_1`, `poly_2`）突出到基础矩形外
2. `union_()`后，生成的多边形有超出预期塔边界的顶点
3. 边缘生成（line 2143）使用`offset(poly, spacing)`进一步扩展这些越界顶点
4. 导致挤出路径超出可打印区域

**证据:** Line 2393显示rib_length计算：
```cpp
m_rib_length = max(m_rib_length, sqrt(depth² + width²));
m_rib_length += m_extra_rib_length;  // 可能推出边界
```

### 问题2: 正方形形状未维护 ⚠️ 严重

**根本原因:** `y_shift`平移（lines 2494-2495）在线条扩展**之后**但在与基础矩形合并**之前**应用。

**问题位置:** `WipeTower2.cpp` lines 2490-2495
```cpp
Point y_shift{0, scaled(this->m_y_shift)};
line_1.extend(diagonal_extra_length);
line_2.extend(diagonal_extra_length);
line_1.translate(-y_shift);  // 移位对角线
line_2.translate(-y_shift);
// 但基础矩形（lines 2500-2503）未移位
```

**影响:**
- 对角线肋在Y方向移位
- 基础矩形保持在原始位置
- 合并创建不对称多边形
- 塔在预览/打印中显示倾斜或非正方形

**y_shift存在原因:** Lines 2422-2423显示当塔深度缩小时使用：
```cpp
if (m_layer_info->depth < m_wipe_tower_depth - m_perimeter_width)
    m_y_shift = (m_wipe_tower_depth - m_layer_info->depth - m_perimeter_width) / 2.f;
```
这在换刀次数减少导致所需深度减小时使塔居中。

### 问题3: 缺少rib_offset补偿 ⚠️ 中等

**根本原因:** WipeTower2注释掉了rib偏移计算。

**BambuStudio实现 (WipeTower.cpp:4322-4324):**
```cpp
if (m_cur_layer_id == 0) {
    BoundingBox bbox = get_extents(result_wall);
    m_rib_offset = Vec2f(-unscaled<float>(bbox.min.x()),
                         -unscaled<float>(bbox.min.y()));
}
```
- 在第一层计算`m_rib_offset`以补偿rib几何
- 通过`get_rib_offset()`方法暴露
- 用于精确定位调整

**WipeTower2实现 (WipeTower2.cpp:2509-2512):**
```cpp
//if (m_cur_layer_id == 0) {  // 注释掉
//    BoundingBox bbox = get_extents(result_wall);
//    m_rib_offset = Vec2f(-unscaled<float>(bbox.min.x()),
//                         -unscaled<float>(bbox.min.y()));
//}

// WipeTower2.hpp:74
Vec2f get_rib_offset() const { return Vec2f::Zero(); }
```
- **不计算rib偏移**（代码被注释）
- 始终返回`Vec2f::Zero()`以保持API一致性
- 注释说明："WT2当前不像WipeTower那样计算rib原点补偿"

**影响:**
- 可能导致轻微的定位不准确
- 原始WipeTower计算此值以实现精确对齐

### 问题4: 内部设计不一致 ⚠️ 中等

**问题:** rib多边形生成有冲突的目标：
1. **稳定性增强**需要肋条超出基础（更宽的占地面积）
2. **边界约束**需要所有几何保持在塔占地面积内
3. **锥化逻辑**在更高层减少肋扩展，但基础计算（line 2393）确保最小扩展

**表现:**
- Lines 2396-2397尝试限制肋宽度：
```cpp
m_rib_width = min(m_rib_width, min(m_wipe_tower_depth, m_wipe_tower_width) / 2.f);
```
但这只限制**宽度**，不限制肋的**长度**。

- 没有对应的`m_rib_length`或`diagonal_extra_length`限制
- 注释说"确保擦除塔的肋壁附着到填充"但不防止边界违规

### 问题5: 注释掉的验证 ⚠️ 低

**证据:** Lines 2508-2509
```cpp
/*if (p_1_2.front().points.size() != 16)
    std::cout << "error " << std::endl;*/
```

**含义:**
- 开发者期望正好16个顶点（4个基础角 + 12个来自对角线矩形）
- 验证被禁用，表明实现不一致地产生这个
- 当`union_()`合并重叠几何时，顶点数可能变化
- 没有运行时检查畸形多边形

---

## 四、修复方案

### 优先级1: 边界约束（问题1）⚠️ 必须修复

**文件:** `src/libslic3r/GCode/WipeTower2.cpp`
**函数:** `generate_rib_polygon()`
**行号:** 2478-2511

**方案A - 限制扩展（推荐）:**
```cpp
// 在line 2488之后，添加边界检查：
float max_safe_extension = std::min(
    (m_wipe_tower_width - line_1.length()) / 2.0f,
    (m_wipe_tower_depth - line_1.length()) / 2.0f
);
diagonal_extra_length = std::min(diagonal_extra_length, max_safe_extension);
```

**方案B - 合并后裁剪:**
```cpp
// 在line 2505之后，裁剪到边界框：
Polygon boundary = generate_rectange_polygon(wt_box.ld, wt_box.ru);
Polygons clipped = intersection_({p_1_2.front(), boundary});
return clipped.empty() ? p_1_2.front() : clipped.front();
```

**推荐:** 方案A，因为它在源头防止问题，而不是事后修复。

### 优先级2: Y-Shift一致性（问题2）⚠️ 必须修复

**文件:** `src/libslic3r/GCode/WipeTower2.cpp`
**行号:** 2500-2503

**修复:** 也对基础矩形应用y_shift：
```cpp
poly.points.push_back(Point::new_scale(wt_box.ld) - y_shift);
poly.points.push_back(Point::new_scale(wt_box.rd) - y_shift);
poly.points.push_back(Point::new_scale(wt_box.ru) - y_shift);
poly.points.push_back(Point::new_scale(wt_box.lu) - y_shift);
```

### 优先级3: 实现rib_offset补偿（问题3）⚠️ 应该修复

**文件:** `src/libslic3r/GCode/WipeTower2.cpp`
**行号:** 2509-2512

**修复:** 取消注释并启用rib_offset计算：
```cpp
if (m_cur_layer_id == 0) {
    BoundingBox bbox = get_extents(p_1_2.front());
    m_rib_offset = Vec2f(-unscaled<float>(bbox.min.x()),
                         -unscaled<float>(bbox.min.y()));
}
```

**同时更新:** `src/libslic3r/GCode/WipeTower2.hpp` line 74
```cpp
Vec2f get_rib_offset() const { return m_rib_offset; }
```

### 优先级4: 添加验证（问题5）⚠️ 建议

**文件:** `src/libslic3r/GCode/WipeTower2.cpp`
**位置:** line 2510之后

**添加:**
```cpp
// 验证结果
BoundingBox bbox = get_extents(p_1_2.front());
BoundingBox expected = get_extents(poly);
if (!expected.contains(bbox)) {
    BOOST_LOG_TRIVIAL(warning) << "Rib polygon exceeds tower boundaries at layer "
                               << m_cur_layer_id;
}

// 可选：验证顶点数
if (p_1_2.front().points.size() != 16) {
    BOOST_LOG_TRIVIAL(debug) << "Rib polygon has " << p_1_2.front().points.size()
                             << " vertices (expected 16)";
}

return p_1_2.front();
```

### 优先级5: 实现正方形维护算法（问题2扩展）⚠️ 可选

**文件:** `src/libslic3r/GCode/WipeTower2.cpp`
**函数:** `generate()` 或新函数 `plan_tower_square()`

**参考BambuStudio实现:**
```cpp
// 在计算塔尺寸时（大约line 2393附近）
float max_depth = /* 计算所有层的最大深度 */;
float square_width = std::ceil(
    std::sqrt(max_depth * m_wipe_tower_width * m_extra_spacing) / m_perimeter_width
) * m_perimeter_width;
m_wipe_tower_width = square_width;
```

**注意:** 这需要更深入的架构更改，可能影响现有配置文件和用户设置。

---

## 五、测试要求

### 5.1 单元测试
1. **边界测试**
   - `wipe_tower_extra_rib_length` = 0, 10, 50, 100
   - 验证所有顶点在边界内

2. **尺寸测试**
   - 小塔: 40x40mm
   - 中塔: 60x60mm
   - 大塔: 100x100mm

3. **深度变化测试**
   - 多材料，不同换刀次数
   - 验证y_shift正确应用

4. **边缘生成测试**
   - 验证边缘不超出床边界
   - 检查边缘与塔的附着

### 5.2 集成测试
1. **G-code预览**
   - 检查视觉伪影
   - 验证塔形状正方/对称

2. **实际打印**
   - 测试不同打印机配置
   - 验证结构稳定性

3. **回归测试**
   - 确保修复不破坏现有功能
   - 测试所有墙类型（矩形、锥形、肋）

### 5.3 性能测试
1. **切片时间**
   - 确保修复不显著增加切片时间

2. **内存使用**
   - 监控多边形操作的内存分配

---

## 六、实现路线图

### 阶段1: 关键修复（1-2天）
- [ ] 修复边界约束（优先级1）
- [ ] 修复y_shift一致性（优先级2）
- [ ] 添加基本验证（优先级4）
- [ ] 单元测试

### 阶段2: 完整对齐（3-5天）
- [ ] 实现rib_offset补偿（优先级3）
- [ ] 集成测试
- [ ] G-code预览验证
- [ ] 文档更新

### 阶段3: 高级功能（可选，5-7天）
- [ ] 实现正方形维护算法（优先级5）
- [ ] 性能优化
- [ ] 实际打印测试
- [ ] 用户文档

---

## 七、风险评估

### 高风险
- **配置兼容性**: 修改可能影响现有用户配置文件
- **打印质量**: 错误的修复可能导致打印失败

### 中风险
- **性能影响**: 额外的边界检查可能增加切片时间
- **回归**: 可能影响非rib墙类型

### 低风险
- **代码复杂度**: 修复相对简单，风险可控

### 缓解策略
1. **渐进式部署**: 先修复关键问题，再添加高级功能
2. **充分测试**: 每个阶段都进行完整测试
3. **版本控制**: 使用feature分支，便于回滚
4. **用户反馈**: Beta测试收集用户反馈

---

## 八、参考资料

### 代码位置
- **BambuStudio**: `D:\work\Projects\BambuStudio\src\libslic3r\GCode\WipeTower.cpp`
- **SoftFever**: `D:\work\Projects\orcaslicer\OrcaSlicer\src\libslic3r\GCode\WipeTower2.cpp`
- **当前**: `C:\WorkCode\orca2.2222222\OrcaSlicer\src\libslic3r\GCode\WipeTower2.cpp`

### 相关文档
- `docs/WipeTower_OutOfBounds_Fix_Analysis.md` - 之前的越界问题分析
- `docs/wipe_tower_brim_chamfer_implementation.md` - 边缘倒角功能

### Git提交历史
- `db1fe73e85` - 修复裙边对齐
- `96be9f94ef` - 修复SEMM的Y偏移
- `ed3f0e2898` - 修复擦除塔耗材选择
- `b9cd05dc86` - 修复负rib长度崩溃
- `715a8a8518` - 修复rib墙宽度计算

---

## 九、结论

当前WipeTower2的rib实现与BambuStudio存在关键差异，主要问题是：
1. 缺少边界约束导致挤出线溢出
2. y_shift应用不一致导致形状不对称
3. 缺少rib_offset补偿导致定位不准确

通过实施上述修复方案，可以使OrcaSlicer的rib擦除塔表现和功能与BambuStudio完全一致。建议采用渐进式实施策略，先修复关键问题，再添加高级功能。

---

**文档版本**: 1.0
**创建日期**: 2026-03-11
**作者**: Claude (基于三个代码库的并行调研)
