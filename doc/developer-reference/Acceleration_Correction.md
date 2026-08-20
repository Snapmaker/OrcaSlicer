# 问题1纠正：E最大加速度 vs 挤出最大加速度

## 我之前的错误分析

我之前说"会使用5000"是**错误的**！感谢你的测试发现了问题。

## 正确的理解

### 代码逻辑重新分析

**位置**: `src/libslic3r/GCode/GCodeProcessor.cpp`

#### 步骤1: 初始化加速度（Line 770-773）

```cpp
// 从machine_max_acceleration_extruding读取
float max_acceleration = get_option_value(
    m_time_processor.machine_limits.machine_max_acceleration_extruding, i);

// 设置为machines的加速度
m_time_processor.machines[i].max_acceleration = max_acceleration;
m_time_processor.machines[i].acceleration = max_acceleration;
```

**关键点**:
- `machines[i].acceleration` = `machine_max_acceleration_extruding`
- 场景1: acceleration = **20000**
- 场景2: acceleration = **5000**

#### 步骤2: 获取打印移动的加速度（Line 2827-2831）

```cpp
float acceleration = get_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i));
// 返回 machines[i].acceleration
```

**结果**:
- 场景1: acceleration = **20000**（从extruding配置）
- 场景2: acceleration = **5000**（从extruding配置）

#### 步骤3: 检查各轴加速度限制（Line 2834-2838）

```cpp
for (unsigned char a = X; a <= E; ++a) {
    float axis_max_acceleration = get_axis_max_acceleration(..., static_cast<Axis>(a));
    // 对E轴：axis_max_acceleration = 5000

    // 检查这个轴的加速度分量是否超过限制
    if (acceleration * std::abs(delta_pos[a]) * inv_distance > axis_max_acceleration)
        acceleration = axis_max_acceleration / (std::abs(delta_pos[a]) * inv_distance);
}
```

### 关键区别理解

**`machine_max_acceleration_extruding`（挤出最大加速度）**:
- **主要限制**：打印移动的整体加速度上限
- 直接决定`acceleration`的初始值

**`machine_max_acceleration_e`（E轴最大加速度）**:
- **次要检查**：确保E轴的加速度分量不超限
- 只在E轴分量超限时才降低整体加速度

### 实际计算示例

**假设打印移动**：100mm距离，XY=99mm，E=5mm

**场景1：extruding=20000, e=5000**
```
1. acceleration = 20000（从extruding配置）

2. 检查E轴：
   - E轴比例 = 5/100 = 0.05
   - E轴加速度分量 = 20000 * 0.05 = 1000 mm/s²
   - E轴限制 = 5000 mm/s²
   - 1000 < 5000 ✓ 不超限

3. 最终使用：acceleration = 20000 mm/s²
```

**场景2：extruding=5000, e=5000**
```
1. acceleration = 5000（从extruding配置）

2. 检查E轴：
   - E轴比例 = 5/100 = 0.05
   - E轴加速度分量 = 5000 * 0.05 = 250 mm/s²
   - E轴限制 = 5000 mm/s²
   - 250 < 5000 ✓ 不超限

3. 最终使用：acceleration = 5000 mm/s²
```

**结果对比**：
- 场景1用20000，加速更快，**时间更短** ✓
- 场景2用5000，加速更慢，**时间更长** ✓

这与你的测试结果一致！

### E轴限制什么时候生效？

只有当**E轴占比很大**时，E轴限制才会降低整体加速度。

**示例：E占比50%**（100mm移动，XY=50mm，E=50mm）

**场景1：extruding=20000, e=5000**
```
1. acceleration = 20000

2. 检查E轴：
   - E轴比例 = 50/100 = 0.5
   - E轴加速度分量 = 20000 * 0.5 = 10000 mm/s²
   - E轴限制 = 5000 mm/s²
   - 10000 > 5000 ✗ 超限！

3. 调整加速度：
   - acceleration = 5000 / 0.5 = 10000 mm/s²
   - 验证：10000 * 0.5 = 5000 ✓

4. 最终使用：acceleration = 10000 mm/s²（被E轴限制降低了）
```

**场景2：extruding=5000, e=5000**
```
1. acceleration = 5000

2. 检查E轴：
   - E轴加速度分量 = 5000 * 0.5 = 2500 mm/s²
   - E轴限制 = 5000 mm/s²
   - 2500 < 5000 ✓ 不超限

3. 最终使用：acceleration = 5000 mm/s²
```

**结果对比**：
- 场景1用10000（被E轴限制从20000降到10000）
- 场景2用5000
- 场景1仍然更快

## 正确的结论

### 主从关系

1. **`machine_max_acceleration_extruding`是主要限制**
   - 决定了打印移动的基础加速度
   - **直接影响时间估算**
   - 设置20000 vs 5000会导致显著的时间差异

2. **`machine_max_acceleration_e`是辅助检查**
   - 只检查E轴分量是否超限
   - 对于正常打印（E占比通常5-20%），**很少触发**
   - 只在E占比>25%时才可能降低加速度

### 你的测试结果解释

```
测试1：E最大=5000，挤出最大=20000
→ 大部分移动使用20000加速度
→ 时间较短

测试2：E最大=5000，挤出最大=5000
→ 大部分移动使用5000加速度
→ 时间较长（约4倍加速时间）

结论：挤出最大加速度是主要决定因素 ✓
```

### 时间差异计算示例

**假设**：100mm移动，从0加速到100 mm/s

**加速度20000 mm/s²**:
- 加速时间 = 100 / 20000 = 0.005秒
- 加速距离 = 0.5 * 100 * 0.005 = 0.25mm

**加速度5000 mm/s²**:
- 加速时间 = 100 / 5000 = 0.02秒
- 加速距离 = 0.5 * 100 * 0.02 = 1mm

**时间差**：0.02 - 0.005 = **0.015秒**（每次加速多3倍时间）

对于一个典型的打印任务（10000次移动块），累积时间差可能达到**2-5分钟**！

## 修正后的理解

| 参数 | 作用 | 影响程度 |
|-----|------|---------|
| `machine_max_acceleration_extruding` | 打印时整体加速度 | ⭐⭐⭐⭐⭐ 主要 |
| `machine_max_acceleration_e` | E轴分量限制 | ⭐⭐ 次要（通常不触发） |

**优先级**：extruding > e（对正常打印）

## 我之前错误的原因

我错误地认为"E轴限制会把加速度直接降到5000"，但实际上：
- E轴限制只检查**E轴分量**
- 如果E占比小（5-20%），E轴分量 = 加速度 × 5-20%
- 即使加速度是20000，E轴分量只有1000-4000，不超过5000
- 所以E轴限制**不会触发**

只有E占比>25%时，E轴限制才会降低加速度，但仍然不是降到5000，而是降到满足"E轴分量=5000"的程度。

## 感谢你的测试！

你的实际测试证明了：
- **extruding（挤出最大加速度）是关键参数**
- 改变它会显著影响打印时间
- 我之前的分析是错误的

正确答案是：**会使用20000**（对于正常E占比的打印移动）
