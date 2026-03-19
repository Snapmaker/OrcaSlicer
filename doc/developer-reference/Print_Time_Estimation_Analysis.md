# 打印时间预估分析文档

## 概述

本文档详细分析了OrcaSlicer中所有影响打印时间预估的工艺配置项，以及温度相关的预估逻辑。

## 时间预估核心机制

时间预估的核心实现在 `src/libslic3r/GCode/GCodeProcessor.cpp` 中的 `TimeMachine` 类。它通过以下方式计算时间：

1. **梯形速度曲线（Trapezoid）**：每个移动块（TimeBlock）被分解为加速、巡航、减速三个阶段
2. **加速度限制**：根据配置的加速度值计算加速/减速所需的时间和距离
3. **速度限制**：根据配置的速度值计算巡航阶段的时间
4. **同步等待**：某些命令（如M400、G4、M191等）会调用 `simulate_st_synchronize()` 添加固定等待时间

## 影响时间预估的配置项

### 1. 速度相关配置（Speed Settings）

这些配置直接影响移动速度，从而影响时间预估：

| 配置名称 | 配置解释 | 代码位置 | 如何影响时间预估 |
|---------|---------|---------|----------------|
| `initial_layer_speed` | 首层打印速度 | `GCodeProcessor::process_line_G1()` | 首层所有移动使用此速度，速度越低时间越长 |
| `initial_layer_infill_speed` | 首层填充速度 | `GCodeProcessor::process_line_G1()` | 首层填充移动使用此速度 |
| `outer_wall_speed` | 外壁速度 | `GCodeProcessor::process_line_G1()` | 外壁移动使用此速度，影响外壁打印时间 |
| `inner_wall_speed` | 内壁速度 | `GCodeProcessor::process_line_G1()` | 内壁移动使用此速度，影响内壁打印时间 |
| `top_surface_speed` | 顶面速度 | `GCodeProcessor::process_line_G1()` | 顶面移动使用此速度 |
| `internal_solid_infill_speed` | 内部实心填充速度 | `GCodeProcessor::process_line_G1()` | 内部实心填充移动使用此速度 |
| `sparse_infill_speed` | 稀疏填充速度 | `GCodeProcessor::process_line_G1()` | 稀疏填充移动使用此速度 |
| `gap_infill_speed` | 间隙填充速度 | `GCodeProcessor::process_line_G1()` | 间隙填充移动使用此速度 |
| `travel_speed` | 空走速度 | `GCodeProcessor::process_line_G1()` | 空走移动使用此速度，影响空走时间 |
| `small_perimeter_speed` | 小周长速度 | `GCodeProcessor::process_line_G1()` | 小周长移动使用此速度 |

**代码级描述**：
- 在 `GCodeProcessor::process_line_G1()` 中，根据移动类型（ExtrusionRole）选择对应的速度配置
- 速度值直接用于计算 `TimeBlock` 的 `cruise_feedrate`
- 时间计算公式：`time = acceleration_time + cruise_time + deceleration_time`
- 其中 `cruise_time = cruise_distance / cruise_feedrate`

### 2. 加速度相关配置（Acceleration Settings）

这些配置影响加速/减速过程，从而影响时间预估：

| 配置名称 | 配置解释 | 代码位置 | 如何影响时间预估 |
|---------|---------|---------|----------------|
| `default_acceleration` | 默认加速度 | `GCodeProcessor::apply_config()` | 作为默认加速度值，影响所有移动的加速/减速时间 |
| `initial_layer_acceleration` | 首层加速度 | `GCodeProcessor::process_line_G1()` | 首层移动使用此加速度，加速度越低加速/减速时间越长 |
| `outer_wall_acceleration` | 外壁加速度 | `GCodeProcessor::process_line_G1()` | 外壁移动使用此加速度 |
| `inner_wall_acceleration` | 内壁加速度 | `GCodeProcessor::process_line_G1()` | 内壁移动使用此加速度 |
| `top_surface_acceleration` | 顶面加速度 | `GCodeProcessor::process_line_G1()` | 顶面移动使用此加速度 |
| `internal_solid_infill_acceleration` | 内部实心填充加速度 | `GCodeProcessor::process_line_G1()` | 内部实心填充移动使用此加速度 |
| `sparse_infill_acceleration` | 稀疏填充加速度 | `GCodeProcessor::process_line_G1()` | 稀疏填充移动使用此加速度 |
| `travel_acceleration` | 空走加速度 | `GCodeProcessor::set_travel_acceleration()` | 空走移动使用此加速度 |
| `bridge_acceleration` | 桥接加速度 | `GCodeProcessor::process_line_G1()` | 桥接移动使用此加速度 |

**代码级描述**：
- 在 `GCodeProcessor::apply_config()` 中，加速度值被设置到 `TimeMachine::acceleration`
- 在 `TimeBlock::calculate_trapezoid()` 中，使用加速度计算加速/减速距离：
  ```cpp
  float accelerate_distance = estimated_acceleration_distance(entry_feedrate, cruise_feedrate, acceleration);
  float decelerate_distance = estimated_acceleration_distance(cruise_feedrate, exit_feedrate, -acceleration);
  ```
- 加速度越低，加速/减速距离越长，巡航距离越短，但总时间可能更长（取决于移动距离）

### 3. 机器限制配置（Machine Limits）

这些配置限制了速度和加速度的上限：

| 配置名称 | 配置解释 | 代码位置 | 如何影响时间预估 |
|---------|---------|---------|----------------|
| `machine_max_acceleration_extruding` | 挤出时最大加速度 | `GCodeProcessor::apply_config()` | 限制所有挤出移动的加速度上限 |
| `machine_max_acceleration_retracting` | 回抽时最大加速度 | `GCodeProcessor::apply_config()` | 限制回抽移动的加速度上限 |
| `machine_max_acceleration_travel` | 空走时最大加速度 | `GCodeProcessor::apply_config()` | 限制空走移动的加速度上限 |
| `machine_max_speed_x` | X轴最大速度 | `GCodeProcessor::process_M203()` | 限制X轴移动速度上限 |
| `machine_max_speed_y` | Y轴最大速度 | `GCodeProcessor::process_M203()` | 限制Y轴移动速度上限 |
| `machine_max_speed_z` | Z轴最大速度 | `GCodeProcessor::process_M203()` | 限制Z轴移动速度上限 |
| `machine_max_speed_e` | E轴最大速度 | `GCodeProcessor::process_M203()` | 限制挤出速度上限 |

**代码级描述**：
- 在 `GCodeProcessor::apply_config()` 中，机器限制被设置到 `TimeMachine::max_acceleration`
- 在 `TimeBlock::calculate_trapezoid()` 中，加速度会被限制在最大值内
- 在 `GCodeProcessor::process_M203()` 中，速度限制被应用到各轴

### 4. 温度相关配置（Temperature Settings）

#### 4.1 预热配置

| 配置名称 | 配置解释 | 代码位置 | 如何影响时间预估 |
|---------|---------|---------|----------------|
| `preheat_time` | 预热时间 | `GCodeProcessor::apply_config()` | **不直接影响时间预估**，仅用于计算何时插入M104预热命令 |
| `delta_temperature` | 预热温度差 | `GCodeProcessor::apply_config()` | **不直接影响时间预估**，仅用于计算预热温度 |
| `preheat_steps` | 预热步数 | `GCodeProcessor::apply_config()` | **不直接影响时间预估**，用于预热回溯的步数 |

**代码级描述**：
- 在 `GCodeProcessor::apply_config()` 中，这些值被存储为成员变量
- 在 `GCodeProcessor::process_line_T()` 中，使用 `preheat_time` 和 `preheat_steps` 计算何时插入M104命令
- **重要**：预热时间本身**不**被添加到时间预估中，因为M104命令是异步的（不等待）

#### 4.2 温度等待命令

| G代码命令 | 配置解释 | 代码位置 | 如何影响时间预估 |
|---------|---------|---------|----------------|
| `M109` | 设置温度并等待 | `GCodeProcessor::process_M109()` | **当前实现有问题**：只更新温度值，**没有添加等待时间** |
| `M190` | 等待热床温度 | `GCodeProcessor::process_M190()` | **当前实现有问题**：只更新温度值，**没有添加等待时间** |
| `M191` | 等待腔室温度 | `GCodeProcessor::process_M191()` | 如果温度>40°C，添加**硬编码的720秒**等待时间 |

**代码级描述**：

**M109问题**（`src/libslic3r/GCode/GCodeProcessor.cpp:3640-3655`）：
```cpp
void GCodeProcessor::process_M109(const GCodeReader::GCodeLine& line)
{
    float new_temp;
    if (line.has_value('R', new_temp)) {
        // ... 更新温度值 ...
    }
    else if (line.has_value('S', new_temp))
        m_extruder_temps[m_extruder_id] = new_temp;
    // ❌ 问题：没有调用 simulate_st_synchronize() 添加等待时间
}
```

**M190问题**（`src/libslic3r/GCode/GCodeProcessor.cpp:3696-3701`）：
```cpp
void GCodeProcessor::process_M190(const GCodeReader::GCodeLine& line)
{
    float new_temp;
    if (line.has_value('S', new_temp))
        m_highest_bed_temp = m_highest_bed_temp < (int)new_temp ? (int)new_temp : m_highest_bed_temp;
    // ❌ 问题：没有调用 simulate_st_synchronize() 添加等待时间
}
```

**M191实现**（`src/libslic3r/GCode/GCodeProcessor.cpp:3703-3710`）：
```cpp
void GCodeProcessor::process_M191(const GCodeReader::GCodeLine& line)
{
    float chamber_temp = 0;
    const float wait_chamber_temp_time = 720.0; // 硬编码720秒
    if (line.has_value('S', chamber_temp) && chamber_temp > 40)
        simulate_st_synchronize(wait_chamber_temp_time); // ✅ 正确添加了等待时间
}
```

**问题分析**：
- **所有M109命令都没有计算等待时间**：`process_M109()` 只更新温度值，没有调用 `simulate_st_synchronize()`
- **所有M190命令都没有计算等待时间**：`process_M190()` 只更新温度值，没有调用 `simulate_st_synchronize()`
- 这导致时间预估**显著偏短**，因为忽略了所有温度等待时间
- 实际打印中，M109/M190会阻塞直到温度达到，但预估中没有计算这部分时间
- 影响最严重的情况：
  - **首层打印前**：如果开始G-code中有M109，等待时间可能达到30-60秒
  - **工具切换时**：如果切换工具需要加热，等待时间可能达到20-40秒
  - **温度变化大时**：从低温（如150°C）加热到高温（如280°C），等待时间可能达到50-80秒

### 5. 工具切换相关配置（Tool Change Settings）

| 配置名称 | 配置解释 | 代码位置 | 如何影响时间预估 |
|---------|---------|---------|----------------|
| `machine_tool_change_time` | 工具切换时间 | `GCodeProcessor::apply_config()` | 在 `process_T()` 中，每次工具切换添加此时间 |
| `machine_load_filament_time` | 加载耗材时间 | `GCodeProcessor::apply_config()` | 在 `process_T()` 中，工具切换时添加此时间 |
| `machine_unload_filament_time` | 卸载耗材时间 | `GCodeProcessor::apply_config()` | 在 `process_M702()` 中，卸载时添加此时间 |

**代码级描述**：
- 在 `GCodeProcessor::process_T()` 中（`src/libslic3r/GCode/GCodeProcessor.cpp:3964-4012`）：
  ```cpp
  float extra_time = 0.0f;
  if (m_time_processor.extruder_unloaded) {
      m_time_processor.extruder_unloaded = false;
      extra_time += get_filament_load_time(static_cast<size_t>(m_extruder_id));
      extra_time += m_time_processor.machine_tool_change_time;
      simulate_st_synchronize(extra_time);
  }
  ```

### 6. 延迟/等待命令（Delay/Wait Commands）

| G代码命令 | 配置解释 | 代码位置 | 如何影响时间预估 |
|---------|---------|---------|----------------|
| `G4` | 延迟命令 | `GCodeProcessor::process_G4()` | 添加指定的延迟时间（S参数为秒，P参数为毫秒） |
| `M1` | 暂停等待 | `GCodeProcessor::process_M1()` | 添加同步等待时间（无限等待，但预估中可能不计算） |
| `M400` | 等待移动完成 | `GCodeProcessor::process_M400()` | 添加同步等待时间 |

**代码级描述**：
- `G4` 命令（`src/libslic3r/GCode/GCodeProcessor.cpp:3457-3462`）：
  ```cpp
  float value_s = 0.0;
  float value_p = 0.0;
  if (line.has_value('S', value_s) || line.has_value('P', value_p)) {
      value_s += value_p * 0.001; // P参数转换为秒
      simulate_st_synchronize(value_s);
  }
  ```

### 7. 其他影响时间预估的因素

| 配置/因素 | 配置解释 | 代码位置 | 如何影响时间预估 |
|---------|---------|---------|----------------|
| `slow_down_layer_time` | 层冷却时间 | `CoolingBuffer` | 如果层时间太短，会降低速度以增加层时间 |
| `fan_cooling_layer_time` | 风扇冷却层时间 | `CoolingBuffer` | 影响冷却逻辑，可能降低速度 |
| `G29` (自动调平) | 自动调平 | `GCodeProcessor::process_G29()` | 硬编码添加**260秒**等待时间 |
| `retraction_speed` | 回抽速度 | `GCodeProcessor::process_line_G1()` | 影响回抽移动的时间 |
| `deretraction_speed` | 回退速度 | `GCodeProcessor::process_line_G1()` | 影响回退移动的时间 |

## 温度相关时间预估问题总结

### 问题1：M109没有添加等待时间

**位置**：`src/libslic3r/GCode/GCodeProcessor.cpp:3640-3655`

**问题**：M109命令应该等待温度达到目标值，但当前实现只更新了温度值，没有添加等待时间。

**影响**：时间预估会**偏短**，因为忽略了温度等待时间。对于需要从低温加热到高温的情况（如从室温到220°C），实际等待时间可能达到30-60秒，但预估中没有计算。

**建议修复**：
```cpp
void GCodeProcessor::process_M109(const GCodeReader::GCodeLine& line)
{
    float new_temp;
    float current_temp = m_extruder_temps[m_extruder_id];
    
    if (line.has_value('R', new_temp)) {
        // ... 处理R参数 ...
    }
    else if (line.has_value('S', new_temp)) {
        m_extruder_temps[m_extruder_id] = new_temp;
    }
    
    // 计算等待时间：根据温度差估算
    if (new_temp > current_temp) {
        float temp_diff = new_temp - current_temp;
        // 假设加热速度约为 2-3°C/秒（可根据实际情况调整）
        float wait_time = temp_diff / 2.5f; // 保守估计
        // 最小等待时间5秒，最大等待时间120秒
        wait_time = std::clamp(wait_time, 5.0f, 120.0f);
        simulate_st_synchronize(wait_time);
    }
}
```

### 问题2：M190没有添加等待时间

**位置**：`src/libslic3r/GCode/GCodeProcessor.cpp:3696-3701`

**问题**：M190命令应该等待热床温度达到目标值，但当前实现只更新了温度值，没有添加等待时间。

**影响**：时间预估会**偏短**，特别是首层打印前的热床预热时间。

**建议修复**：
```cpp
void GCodeProcessor::process_M190(const GCodeReader::GCodeLine& line)
{
    float new_temp;
    if (line.has_value('S', new_temp)) {
        float current_bed_temp = m_highest_bed_temp;
        m_highest_bed_temp = m_highest_bed_temp < (int)new_temp ? (int)new_temp : m_highest_bed_temp;
        
        // 计算等待时间：热床加热速度较慢，约为 0.5-1°C/秒
        if (new_temp > current_bed_temp) {
            float temp_diff = new_temp - current_bed_temp;
            float wait_time = temp_diff / 0.75f; // 保守估计
            // 最小等待时间10秒，最大等待时间300秒
            wait_time = std::clamp(wait_time, 10.0f, 300.0f);
            simulate_st_synchronize(wait_time);
        }
    }
}
```

### 问题3：M191使用硬编码时间

**位置**：`src/libslic3r/GCode/GCodeProcessor.cpp:3703-3710`

**问题**：M191使用硬编码的720秒（12分钟）等待时间，没有根据实际温度差计算。

**影响**：对于不同的温度目标，等待时间可能不准确。

**建议修复**：
```cpp
void GCodeProcessor::process_M191(const GCodeReader::GCodeLine& line)
{
    float chamber_temp = 0;
    if (line.has_value('S', chamber_temp) && chamber_temp > 40) {
        // 腔室加热速度很慢，约为 0.1-0.2°C/秒
        // 假设从室温（~25°C）加热到目标温度
        float temp_diff = chamber_temp - 25.0f;
        float wait_time = temp_diff / 0.15f; // 保守估计
        // 最小等待时间60秒，最大等待时间1800秒（30分钟）
        wait_time = std::clamp(wait_time, 60.0f, 1800.0f);
        simulate_st_synchronize(wait_time);
    }
}
```

### 问题4：预热时间不参与时间预估

**位置**：`src/libslic3r/GCode/GCodeProcessor.cpp:4594-4668`

**问题**：`preheat_time` 配置仅用于计算何时插入M104命令，但不参与时间预估。

**影响**：如果预热时间设置较长，实际打印中工具切换时的等待时间可能被缩短，但预估中没有考虑这个优化。

**分析**：
- 预热机制的目的是**减少**工具切换时的等待时间
- 如果预热成功，M109的等待时间应该**减少**（因为工具已经预热）
- 当前实现中，预热时间不参与预估是**合理的**，因为：
  1. M104是异步命令，不阻塞打印
  2. 预热是否成功取决于实际打印进度
  3. 如果预热失败，仍然需要等待M109

**建议**：保持当前实现，但可以考虑在M109中添加逻辑，如果检测到之前有M104预热命令，可以减少等待时间。

## 补充问题解答

### 1. 打印准备时间（prepare_time）是怎么来的？

**位置**：`src/libslic3r/GCode/GCodeProcessor.cpp:375-376, 2774, 3228`

**机制**：
1. **标记准备阶段**：在 `process_line_G1()` 中，当处理G1/G0命令时，会创建 `TimeBlock` 并设置：
   ```cpp
   block.flags.prepare_stage = m_processing_start_custom_gcode;
   ```

2. **设置准备阶段标志**：`m_processing_start_custom_gcode` 在 `process_gcode_line()` 中设置：
   ```cpp
   // 当遇到自定义G代码（erCustom）且是第一条G1命令时
   m_processing_start_custom_gcode = (m_extrusion_role == erCustom && m_g1_line_id == 0);
   ```
   这意味着**开始G代码（start_gcode）中的移动**会被标记为准备阶段。

3. **累加准备时间**：在 `TimeMachine::calculate_time()` 中：
   ```cpp
   if (block.flags.prepare_stage)
       prepare_time += block_time;
   ```

**包含的内容**：
- 开始G代码（`machine_start_gcode`）中的所有移动时间
- 开始G代码中的温度设置、回零等操作时间
- 首层打印前的所有准备操作时间

**注意**：准备阶段中的空走（Travel）移动**不计入**空走时间统计，但**计入**准备时间：
```cpp
//BBS: don't calculate travel of start gcode into travel time
if (!block.flags.prepare_stage || block.move_type != EMoveType::Travel)
    moves_time[static_cast<size_t>(block.move_type)] += block_time;
```

**用途**：
- 在UI中显示"首层打印时间"时，会从总时间中减去准备时间
- 用于区分"准备时间"和"实际打印时间"

### 2. simulate_st_synchronize 是干什么的？

**位置**：`src/libslic3r/GCode/GCodeProcessor.cpp:257-263`

**功能**：模拟固件（如Marlin）的 `st_synchronize()` 函数调用，表示**等待所有移动完成并添加额外时间**。

**实现**：
```cpp
void GCodeProcessor::TimeMachine::simulate_st_synchronize(float additional_time)
{
    if (!enabled)
        return;
    
    calculate_time(0, additional_time);
}
```

**作用**：
1. **处理时间块队列**：调用 `calculate_time()` 处理 `blocks` 队列中累积的移动块
2. **添加额外时间**：将 `additional_time` 添加到第一个待处理块的时间中
3. **更新统计**：累加到总时间、层时间、角色时间等统计中

**使用场景**：
- **G4延迟命令**：`process_G4()` 调用 `simulate_st_synchronize(value_s)` 添加延迟时间
- **M400等待完成**：`process_M400()` 调用 `simulate_st_synchronize()` 等待移动完成
- **M1暂停**：`process_M1()` 调用 `simulate_st_synchronize()` 添加暂停时间
- **M191等待腔室温度**：`process_M191()` 调用 `simulate_st_synchronize(720.0)` 添加等待时间
- **工具切换**：`process_T()` 调用 `simulate_st_synchronize(extra_time)` 添加切换和加载时间
- **G29自动调平**：`process_G29()` 调用 `simulate_st_synchronize(260.0)` 添加调平时间

**为什么需要**：
- 固件的 `st_synchronize()` 会阻塞直到所有移动完成
- 某些命令（如M109、M190）需要等待操作完成
- 需要在时间预估中反映这些等待时间

**问题**：当前实现中，M109和M190**没有调用** `simulate_st_synchronize()`，导致等待时间没有被计算。

### 3. 算时间是所有G-code都生成了之后后处理的还是生成一条G-code就算一行？

**答案**：**混合模式** - 大部分是**实时处理**，最终计算是**后处理**。

**详细流程**：

#### 3.1 实时处理阶段（G-code生成/解析时）

**位置**：`src/libslic3r/GCode/GCodeProcessor.cpp:1550-2074` (`process_gcode_line`)

**过程**：
1. **解析G-code行**：每解析一行G-code，调用 `process_gcode_line()`
2. **处理G1/G0命令**：遇到G1/G0命令时，调用 `store_move_vertex()` → `process_line_G1()`
3. **创建时间块**：在 `process_line_G1()` 中创建 `TimeBlock` 并添加到 `blocks` 队列：
   ```cpp
   TimeBlock block;
   block.move_type = type;
   block.role = m_extrusion_role;
   block.distance = distance;
   // ... 设置其他属性 ...
   machine.blocks.push_back(block);
   ```
4. **立即计算（部分）**：某些命令会立即调用 `simulate_st_synchronize()`：
   - G4延迟：立即添加延迟时间
   - M400等待：立即处理队列
   - 工具切换：立即添加切换时间

**特点**：
- 时间块（TimeBlock）是**实时创建**的
- 但**不立即计算**最终时间（因为需要前后块的信息来优化速度曲线）

#### 3.2 后处理阶段（所有G-code处理完成后）

**位置**：`src/libslic3r/GCode/GCodeProcessor.cpp:1309-1361` (`finalize`)

**过程**：
1. **最终计算**：在 `finalize()` 中调用 `calculate_time()`：
   ```cpp
   for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
       TimeMachine& machine = m_time_processor.machines[i];
       machine.calculate_time(); // 处理所有剩余的blocks
   }
   ```

2. **速度曲线优化**：`calculate_time()` 执行：
   - **前向传递（forward_pass）**：从前往后优化入口速度
   - **反向传递（reverse_pass）**：从后往前优化出口速度
   - **重新计算梯形**：根据优化后的速度重新计算加速/巡航/减速阶段

3. **累加统计**：计算每个块的时间并累加到：
   - `time`：总时间
   - `prepare_time`：准备时间
   - `layers_time`：每层时间
   - `roles_time`：每种角色时间
   - `moves_time`：每种移动类型时间

**为什么需要后处理**：
- **速度曲线优化**：需要知道后续块的速度才能优化当前块的出口速度
- **加速度限制**：需要确保相邻块之间的速度转换符合加速度限制
- **全局优化**：需要反向传递来确保从后往前的速度优化

#### 3.3 混合模式的原因

**实时处理**：
- 创建时间块（TimeBlock）
- 处理立即生效的命令（G4、M400等）
- 累积到队列中

**后处理**：
- 优化速度曲线（需要全局信息）
- 计算最终时间（需要所有块的信息）
- 更新统计信息

**性能考虑**：
- 如果每条G-code都完整计算，性能会很差（需要重新计算所有块）
- 采用批量处理 + 最终优化的方式，平衡了实时性和准确性

## 总结

影响时间预估的主要因素：

1. **速度配置**：直接影响移动时间
2. **加速度配置**：影响加速/减速时间
3. **机器限制**：限制速度和加速度上限
4. **温度等待**：**当前实现有问题**，M109/M190没有添加等待时间
5. **工具切换**：添加固定的切换和加载时间
6. **延迟命令**：G4、M400等添加固定延迟
7. **准备时间**：开始G代码中的操作时间，单独统计
8. **时间计算模式**：混合模式 - 实时创建时间块，后处理优化和计算

**最严重的问题**：M109和M190没有添加等待时间，这会导致时间预估**显著偏短**，特别是对于需要从低温加热到高温的打印任务。

**时间计算流程**：
1. **实时**：解析G-code → 创建TimeBlock → 添加到队列
2. **实时（部分）**：某些命令立即调用 `simulate_st_synchronize()` 处理队列
3. **后处理**：所有G-code处理完成后，调用 `finalize()` → `calculate_time()` 优化速度曲线并计算最终时间

