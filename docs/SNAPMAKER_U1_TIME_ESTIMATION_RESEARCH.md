# Snapmaker U1 打印时间预估系统 - 综合调研报告

## 调研目标
明确OrcaSlicer中预估打印时间计算的完整逻辑，识别所有影响时间预估的因子，为后续针对Snapmaker U1的时间预估优化提供基础。

---

## 一、核心时间计算架构

### 1.1 主要文件和类
| 文件路径 | 主要功能 |
|---------|---------|
| `src/libslic3r/GCode/GCodeProcessor.hpp` | 时间处理核心数据结构定义（1012行）|
| `src/libslic3r/GCode/GCodeProcessor.cpp` | 时间计算核心实现（5601行）|
| `src/libslic3r/GCode.hpp/cpp` | G-code生成器主类（6946行）|
| `src/libslic3r/PrintConfig.cpp` | 所有配置参数定义 |
| `src/libslic3r/GCodeWriter.cpp` | G-code输出（M73进度命令）|

### 1.2 核心数据结构

**PrintEstimatedStatistics** - 顶层时间统计结构
- `time`: 总打印时间（秒）
- `prepare_time`: 准备时间（开始G-code）
- `custom_gcode_times`: 自定义G-code时间（M109预热、G28归零、换头等）
- `moves_times`: 各类型移动的时间（Travel, Extrude, Retract等）
- `roles_times`: 各挤出角色的时间（外墙、内墙、填充、支撑等）
- `layers_times`: 各层的时间
- 支持Normal和Stealth两种模式并行计算

**TimeBlock** - 单个移动命令的时间块
- `distance`: 移动距离（mm）
- `acceleration`: 加速度（mm/s²）
- `feedrate_profile`: 进给速率配置（entry, cruise, exit速度）
- `trapezoid`: 梯形速度曲线（accelerate_until, decelerate_after, cruise_feedrate）
- `time()`: 计算总时间 = 加速时间 + 恒速时间 + 减速时间

**TimeMachine** - 时间计算引擎
- 固件规划器仿真（前向通过、反向通过、梯形重算）
- 队列大小：64块（模拟固件队列）
- 刷新阈值：256块（4倍队列大小）

### 1.3 时间计算流程

```
G1命令解析 → 创建TimeBlock → 累积到blocks队列 → 固件规划器仿真 → 时间累加
```

---

## 二、影响时间预估的因子分类（50+个因子）

### 2.1 基于速度和距离的计算因子（引擎直接计算）

#### 核心打印速度参数（mm/s）
| 参数名称 | U1基础值 | U1 0.20层高优化值 |
|---------|----------|------------------|
| `outer_wall_speed` | 120 | 200 |
| `inner_wall_speed` | 40 | 300 |
| `sparse_infill_speed` | 50 | 270 |
| `internal_solid_infill_speed` | 40 | 250 |
| `gap_infill_speed` | 30 | 250 |
| `top_surface_speed` | 30 | 200 |
| `initial_layer_speed` | 20 | 50 |
| `travel_speed` | 400 | 500 |
| `bridge_speed` | 25 | 25 |
| `support_speed` | 40 | 40 |

**时间影响**: `时间 = 距离 / 速度`（线性关系）

#### 加速度参数（mm/s²）
| 参数名称 | U1值 |
|---------|------|
| `default_acceleration` | 10000 |
| `outer_wall_acceleration` | 5000 |
| `initial_layer_acceleration` | 500 |
| `top_surface_acceleration` | 2000 |

**时间影响**:
- 加速时间 = `(v_cruise - v_entry) / acceleration`
- 减速时间 = `(v_cruise - v_exit) / acceleration`

#### 梯形速度曲线计算（核心算法）
```
总时间 = t_加速 + t_恒速 + t_减速
```
**关键代码**: GCodeProcessor.cpp 第151-214行

#### 机器速度/加速度限制
- `machine_max_speed_x/y/z/e`
- `machine_max_acceleration_x/y/z/e`
- `machine_max_acceleration_extruding` (M204 P)
- `machine_max_acceleration_retracting` (M204 R)

#### 回抽参数
| 参数 | U1值 |
|------|------|
| `retraction_length` | 0.8 mm |
| `retraction_speed` | 30 mm/s |
| `deretraction_speed` | 30 mm/s |

**每次回抽增加约 0.027秒**

---

### 2.2 配置参数因子（固定时间增量）

#### ⚠️ 工具更换相关时间（**U1关键因子**）

**配置参数**（PrintConfig.cpp 第2047-2073行）：
| 参数名称 | 默认值 | U1配置 | 说明 |
|---------|--------|--------|------|
| `machine_tool_change_time` | 0秒 | **未配置** | 工具更换时间 |
| `machine_load_filament_time` | 0秒 | 0 | 耗材装载时间 |
| `machine_unload_filament_time` | 0秒 | 0 | 耗材卸载时间 |

**时间计算逻辑**（GCodeProcessor.cpp 第4416-4420行）：
```cpp
// T命令处理
float extra_time = 0.0f;
extra_time += get_filament_unload_time(m_last_extruder_id);
extra_time += get_filament_load_time(m_extruder_id);
extra_time += m_time_processor.machine_tool_change_time;  // ← 这里是0！
simulate_st_synchronize(extra_time);
```

**重要发现**：
- U1是工具更换器机器（5个独立挤出头）
- **每次T命令（换头）都会增加固定时间**
- **`machine_tool_change_time` 未配置，这是潜在的主要偏差来源！**

#### 温度等待时间（硬编码）

- **M191 腔室温度**: 硬编码720秒（12分钟）
- **M190 床温**: 通过固件仿真估算
- **M109 挤出机温度**: 通过固件仿真估算

#### 特殊G-code命令时间

- **G4 延迟**: 直接累加S参数（秒）+ P参数（毫秒）
- **G29 床面探测**: 硬编码260秒
- **G28 归零**: 转换为等效G1命令处理

#### 开始G-code时间（prepare_time）

**U1特殊逻辑**：
- 预热所有使用的挤出头（M104 T0/T1/T2/T3/T4）
- 非初始挤出头需要预冲洗（M109 + G1移动 + 挤出）
- 最多5次预冲洗操作
- **这是U1的重要时间开销**

---

### 2.3 间接影响因子

#### 层冷却减速
- `slow_down_layer_time`: 触发减速的最小层时间
- 如果层时间 < 设定值，降低速度延长冷却时间

#### 体积流量限制
- `filament_max_volumetric_speed`: 最大体积流量（mm³/s）
- 如果 `(线宽 × 层高 × 速度) > 限制`，自动降速

#### 离心加速度限制
- 急转弯（角度 < 28.96度）自动降速
- `V_max = sqrt(centripetal_accel × turn_radius)`

---

## 三、Snapmaker U1特定因子分析

### 3.1 工具更换器架构影响

**U1配置特点**：
- 5个独立挤出头（T0-T4）
- `gcode_flavor`: Klipper
- `inherits`: "fdm_toolchanger"

**关键时间因子**：
1. **工具更换机械时间**: `machine_tool_change_time` **← 未配置！**
2. **多头预冲洗时间**: 开始G-code中的重要开销

### 3.2 高速配置影响

**U1 0.20层高优化配置**：
- 外墙：200 mm/s（vs 基础120）
- 内墙：300 mm/s（vs 基础40）
- 填充：270 mm/s（vs 基础50）
- 空跑：500 mm/s（vs 基础400）

**需要验证加速度配置能否支撑这些高速**

### 3.3 Klipper固件特性

- `accel_to_decel_enable`: true
- `accel_to_decel_factor`: 50%
- `max_accel_to_decel` = `max_accel × 50%`

---

## 四、潜在时间偏差来源

### 4.1 ⚠️ 工具更换时间未配置（**最可能的主要原因**）

**问题**：
- U1配置中**未找到** `machine_tool_change_time` 参数
- 默认值为 0秒
- 实际U1换头需要机械动作时间（估计2-5秒/次）

**影响示例**：
```
使用3个挤出头的打印任务
换头次数：50次
时间偏差 = 50 × 3秒 = 150秒 = 2.5分钟
```

**解决方案**：
1. 实际测量U1换头时间
2. 在 `resources/profiles/Snapmaker/machine/fdm_U1.json` 中添加：
   ```json
   "machine_tool_change_time": "3.0"
   ```
3. 重新切片验证

### 4.2 开始G-code时间估算不准确

- 多挤出头预冲洗时间
- M109温度等待时间（环境依赖）
- 准备时间偏差可能达到1-3分钟

### 4.3 加速度限制导致无法达到配置速度

**验证方法**：
```
10mm短距离移动，配置速度200mm/s，加速度10000mm/s²
达到巡航速度需要距离：v²/(2a) = 200²/(2×10000) = 2mm
实际：2mm加速 + 6mm巡航 + 2mm减速
```

如果加速度更低，巡航段会更短或不存在

### 4.4 回抽次数估算

- U1配置：`retraction_minimum_travel` = 1mm
- 每次回抽约0.027秒
- 1000次回抽 = 27秒偏差

---

## 五、调试建议和行动计划

### 5.1 短期行动（立即可做）

**1. 配置工具更换时间**（最重要！）
   - 测量方法：打印简单双色模型，计时实际换头时间
   - 配置文件：`resources/profiles/Snapmaker/machine/fdm_U1.json`
   - 添加参数：`"machine_tool_change_time": "3.0"`

**2. 验证开始G-code时间**
   - 记录从开始到首层的时间
   - 对比切片预估的 `prepare_time`

**3. 检查加速度配置合理性**
   - 验证 `default_acceleration: 10000` 是否过高
   - 计算实际能达到巡航速度的最短距离

### 5.2 中期行动（需要测试验证）

**4. 建立时间预估验证流程**
   - 选择代表性测试模型（单色、双色、多色）
   - 切片并记录预估时间
   - 实际打印并记录真实时间
   - 建立偏差数据库

**5. 分析不同打印模式的时间偏差**
   - 单挤出头打印
   - 双挤出头打印
   - 多挤出头打印
   - 识别换头次数与偏差的关系

**6. 优化高速配置的时间预估**
   - 测试0.20层高配置的实际速度达成率
   - 调整速度/加速度平衡

### 5.3 长期优化（代码层面）

7. 增强工具更换时间的细粒度控制
8. 改进开始G-code时间估算
9. 添加实时学习功能

---

## 六、完整因子清单（调试检查表）

### A. 引擎直接计算因子（45个）

**速度参数** (14个)：
- [ ] outer_wall_speed, inner_wall_speed, sparse_infill_speed
- [ ] internal_solid_infill_speed, gap_infill_speed, top_surface_speed
- [ ] initial_layer_speed, initial_layer_infill_speed
- [ ] travel_speed, travel_speed_z
- [ ] bridge_speed, support_speed, support_interface_speed
- [ ] 其他特殊速度

**加速度参数** (8个)：
- [ ] default_acceleration, outer_wall_acceleration
- [ ] inner_wall_acceleration, travel_acceleration
- [ ] initial_layer_acceleration, top_surface_acceleration
- [ ] bridge_acceleration, 各填充类型加速度

**Jerk参数** (6个)：
- [ ] outer_wall_jerk, inner_wall_jerk, infill_jerk
- [ ] travel_jerk, initial_layer_jerk, top_surface_jerk

**机器限制** (12个)：
- [ ] machine_max_speed_x/y/z/e
- [ ] machine_max_acceleration_x/y/z/e
- [ ] machine_max_acceleration_extruding/retracting
- [ ] machine_min_extruding_rate, machine_min_travel_rate

**回抽参数** (5个)：
- [ ] retraction_length, retraction_speed
- [ ] deretraction_speed, wipe_distance
- [ ] retraction_minimum_travel

### B. 配置固定时间因子（10个）

**工具更换** (3个) - **U1关键**：
- [ ] `machine_tool_change_time` ⚠️ **未配置，主要偏差来源**
- [ ] machine_load_filament_time
- [ ] machine_unload_filament_time

**特殊G-code时间** (4个)：
- [ ] G4延迟命令
- [ ] G28归零时间
- [ ] G29床探测时间（260秒）
- [ ] M191腔室温度等待（720秒）

**开始G-code时间** (3个)：
- [ ] M190床预热时间
- [ ] M104/M109挤出机预热时间
- [ ] 多挤出头预冲洗时间（U1特定）

### C. 间接影响因子（7个）

- [ ] slow_down_layer_time（层冷却减速）
- [ ] filament_max_volumetric_speed（体积流量限制）
- [ ] 离心加速度限制（自动计算）
- [ ] resonance_avoidance（避震共振）
- [ ] max_volumetric_extrusion_rate_slope
- [ ] slow_down_min_speed
- [ ] dont_slow_down_outer_wall

---

## 七、关键文件路径参考

### 核心代码文件
- `src/libslic3r/GCode/GCodeProcessor.hpp`: 第45-105行（PrintEstimatedStatistics）
- `src/libslic3r/GCode/GCodeProcessor.cpp`:
  - 第151-214行：梯形时间计算
  - 第335-389行：固件规划器仿真
  - 第2901行：G1命令处理
  - 第4376-4428行：T命令（换头）处理
  - 第4118行：M191腔室温度等待
- `src/libslic3r/PrintConfig.cpp`:
  - 第2047行：machine_load_filament_time
  - 第2056行：machine_unload_filament_time
  - 第2065行：machine_tool_change_time

### U1配置文件
- `resources/profiles/Snapmaker/machine/fdm_U1.json`: U1机器基础配置
- `resources/profiles/Snapmaker/process/fdm_process_U1.json`: U1工艺基础配置
- `resources/profiles/Snapmaker/process/fdm_process_U1_0.20.json`: 0.20层高优化配置
- `resources/profiles/Snapmaker/process/fdm_process_U1_common.json`: U1通用工艺配置

---

## 八、总结

### 主要发现

1. **⚠️ 工具更换时间未配置是最可能的主要偏差来源**
   - U1作为5头工具更换器，每次换头需要机械时间
   - `machine_tool_change_time` 参数在配置中缺失或为0
   - 估计每次换头2-5秒，累积偏差可能达到数分钟

2. **时间计算系统本身是完善的**
   - 基于梯形速度曲线的准确计算
   - 完整的固件规划器仿真（前向/反向通过）
   - 支持所有主要时间因子

3. **U1高速配置需要验证**
   - 配置速度很高（200-300 mm/s）
   - 需要确认加速度配置能否支撑
   - 短距离移动可能无法达到巡航速度

### 下一步行动优先级

**高优先级**：
1. ⚠️ 测量并配置 `machine_tool_change_time`
2. 验证多挤出头打印的时间偏差
3. 检查开始G-code的实际执行时间

**中优先级**：
4. 建立时间预估验证流程
5. 优化高速配置的参数平衡
6. 分析不同换头次数的影响

**低优先级**：
7. 长期代码优化
8. 添加学习功能
9. 细化时间估算模型

### 预期改进效果

配置正确的 `machine_tool_change_time` 后：
- **单色打印**：时间预估应该基本准确
- **双色打印**（假设30次换头）：偏差从90-150秒降至±30秒内
- **多色打印**（假设100次换头）：偏差从300-500秒降至±60秒内

---

## 附录：时间计算核心算法详解

### 梯形速度曲线数学模型

```
对于单个移动块：
1. 加速阶段：v(t) = v_entry + a*t
   距离：s_accel = (v_cruise² - v_entry²) / (2*a)
   时间：t_accel = (v_cruise - v_entry) / a

2. 恒速阶段：v(t) = v_cruise
   距离：s_cruise = total_distance - s_accel - s_decel
   时间：t_cruise = s_cruise / v_cruise

3. 减速阶段：v(t) = v_cruise - a*t
   距离：s_decel = (v_cruise² - v_exit²) / (2*a)
   时间：t_decel = (v_cruise - v_exit) / a

总时间 = t_accel + t_cruise + t_decel
```

### 固件规划器仿真算法

**前向通过**（Forward Pass）：
- 从第一个块开始，计算每个块的最大可达入口速度
- 考虑前一个块的出口速度和加速度限制

**反向通过**（Reverse Pass）：
- 从最后一个块开始，反向调整速度
- 确保每个块可以在距离内减速到下一块的入口速度

**梯形重算**（Recalculate Trapezoids）：
- 根据最终确定的入口/出口速度
- 重新计算每个块的加速/恒速/减速段

这个算法完全模拟了Marlin/Klipper固件的运动规划器，确保时间估算的准确性。
