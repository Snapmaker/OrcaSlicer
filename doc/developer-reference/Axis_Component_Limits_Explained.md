# 轴向速度和加速度限制详解：斜向移动如何计算

## 核心问题

对于一条斜向G-code命令，如 `G1 X100 Y100 F6000`，如何应用各轴的速度/加速度限制？

## 关键概念：分量 vs 合成

### 1. 速度的分解

**位置**: `src/libslic3r/GCode/GCodeProcessor.cpp:2804-2824`

```cpp
// 计算每个轴的速度分量（feedrate）
for (unsigned char a = X; a <= E; ++a) {
    curr.axis_feedrate[a] = curr.feedrate * delta_pos[a] * inv_distance;
    // 这里计算的是每个轴的速度分量

    curr.abs_axis_feedrate[a] = std::abs(curr.axis_feedrate[a]);

    if (curr.abs_axis_feedrate[a] != 0.0f) {
        float axis_max_feedrate = get_axis_max_feedrate(..., static_cast<Axis>(a));
        if (axis_max_feedrate != 0.0f)
            min_feedrate_factor = std::min<float>(min_feedrate_factor,
                axis_max_feedrate / curr.abs_axis_feedrate[a]);
    }
}

// 如果有轴超限，按比例降低整体速度
curr.feedrate *= min_feedrate_factor;
```

## 详细示例：斜向移动

### 示例1: 45度对角线移动

**G-code命令**:
```gcode
G1 X100 Y100 F6000
```

**物理参数**:
```
起点: (0, 0)
终点: (100, 100)
delta_X = 100mm
delta_Y = 100mm
距离 = √(100² + 100²) = 141.42mm
目标速度 = 6000 mm/min = 100 mm/s
```

### 步骤1: 计算各轴速度分量

```cpp
inv_distance = 1 / 141.42 = 0.00707

// X轴速度分量
axis_feedrate[X] = 100 mm/s × 100mm × 0.00707
                 = 100 × 0.707
                 = 70.7 mm/s

// Y轴速度分量
axis_feedrate[Y] = 100 mm/s × 100mm × 0.00707
                 = 70.7 mm/s

// Z轴速度分量
axis_feedrate[Z] = 0 mm/s (无Z移动)

// E轴速度分量（假设挤出10mm）
axis_feedrate[E] = 100 mm/s × 10mm × 0.00707
                 = 7.07 mm/s
```

**物理意义**:
- 打印头沿对角线以100 mm/s移动
- X轴方向的速度分量是70.7 mm/s
- Y轴方向的速度分量是70.7 mm/s
- 验证: √(70.7² + 70.7²) = 100 mm/s ✓

### 步骤2: 检查各轴速度限制

**假设配置**:
```
machine_max_speed_x = 500 mm/s
machine_max_speed_y = 500 mm/s
machine_max_speed_z = 12 mm/s
machine_max_speed_e = 120 mm/s
```

**检查过程**:
```cpp
min_feedrate_factor = 1.0

// X轴检查
abs_axis_feedrate[X] = 70.7 mm/s
axis_max_feedrate[X] = 500 mm/s
70.7 < 500 ✓ 不需要降速
factor = 500 / 70.7 = 7.07
min_feedrate_factor = min(1.0, 7.07) = 1.0

// Y轴检查
abs_axis_feedrate[Y] = 70.7 mm/s
axis_max_feedrate[Y] = 500 mm/s
70.7 < 500 ✓ 不需要降速
min_feedrate_factor = min(1.0, 7.07) = 1.0

// E轴检查
abs_axis_feedrate[E] = 7.07 mm/s
axis_max_feedrate[E] = 120 mm/s
7.07 < 120 ✓ 不需要降速
min_feedrate_factor = min(1.0, 16.97) = 1.0
```

**结果**: `min_feedrate_factor = 1.0`，所有轴都不超限，保持100 mm/s

### 步骤3: 应用降速（如果需要）

```cpp
curr.feedrate *= min_feedrate_factor;  // 100 × 1.0 = 100 mm/s
```

## 示例2: Z轴限制导致降速

### 场景: 斜向上移动

**G-code命令**:
```gcode
G1 X100 Y100 Z20 F6000
```

**物理参数**:
```
delta_X = 100mm
delta_Y = 100mm
delta_Z = 20mm
距离 = √(100² + 100² + 20²) = 144.57mm
目标速度 = 100 mm/s
```

### 计算各轴速度分量

```cpp
inv_distance = 1 / 144.57 = 0.00692

axis_feedrate[X] = 100 × 100 × 0.00692 = 69.2 mm/s
axis_feedrate[Y] = 100 × 100 × 0.00692 = 69.2 mm/s
axis_feedrate[Z] = 100 × 20 × 0.00692 = 13.84 mm/s  ⚠️
```

### 检查Z轴限制

```cpp
// Z轴检查
abs_axis_feedrate[Z] = 13.84 mm/s
axis_max_feedrate[Z] = 12 mm/s
13.84 > 12 ✗ 超限！

// 计算需要降速的因子
factor = 12 / 13.84 = 0.867
min_feedrate_factor = min(1.0, 0.867) = 0.867
```

### 应用降速

```cpp
// 降低整体速度
curr.feedrate *= 0.867
curr.feedrate = 100 × 0.867 = 86.7 mm/s

// 重新计算各轴速度分量
axis_feedrate[X] = 69.2 × 0.867 = 60.0 mm/s
axis_feedrate[Y] = 69.2 × 0.867 = 60.0 mm/s
axis_feedrate[Z] = 13.84 × 0.867 = 12.0 mm/s  ✓ 刚好等于限制

// 验证
√(60² + 60² + 12²) = 86.7 mm/s ✓
```

**结果**: 由于Z轴限制12 mm/s，整体速度从100降到86.7 mm/s

## 加速度的分量计算（完全相同的逻辑）

**位置**: `GCodeProcessor.cpp:2834-2838`

```cpp
for (unsigned char a = X; a <= E; ++a) {
    float axis_max_acceleration = get_axis_max_acceleration(..., static_cast<Axis>(a));

    // 计算这个轴的加速度分量
    float axis_acceleration_component = acceleration × |delta_pos[a]| × inv_distance;

    if (axis_acceleration_component > axis_max_acceleration)
        // 降低整体加速度以满足这个轴的限制
        acceleration = axis_max_acceleration / (|delta_pos[a]| × inv_distance);
}
```

### 示例3: E轴加速度限制

**场景**: 高挤出率的打印移动

```gcode
G1 X50 Y50 E25 F3000
```

**参数**:
```
delta_X = 50mm
delta_Y = 50mm
delta_E = 25mm
距离 = √(50² + 50² + 25²) = 75mm
速度 = 50 mm/s
初始加速度 = 20000 mm/s²
E轴最大加速度 = 5000 mm/s²
```

### 计算E轴加速度分量

```cpp
inv_distance = 1/75 = 0.01333

// E轴加速度分量
E_accel_component = 20000 × 25 × 0.01333
                  = 20000 × 0.3333
                  = 6666.7 mm/s²

// 检查E轴限制
6666.7 > 5000 ✗ 超限！

// 调整整体加速度
acceleration = 5000 / (25 × 0.01333)
             = 5000 / 0.3333
             = 15000 mm/s²
```

**结果**:
- 初始加速度20000被降到15000
- E轴分量刚好是5000 mm/s²
- X、Y轴分量相应降低

## 可视化理解

### 速度矢量分解

```
对于 G1 X100 Y100 F6000 (100 mm/s)

         Y轴
         ↑
         │ 70.7 mm/s
         │
         │     ╱
         │   ╱ 合成速度
         │ ╱   100 mm/s
         │╱
    ─────┼──────────→ X轴
         │  70.7 mm/s
         │

合成速度 = √(Vx² + Vy²) = √(70.7² + 70.7²) = 100 mm/s
```

### 如果X轴限制为50 mm/s

```
原始:
Vx = 70.7 mm/s  > 50 ✗ 超限！

降速因子:
factor = 50 / 70.7 = 0.707

降速后:
Vx = 70.7 × 0.707 = 50 mm/s   ✓
Vy = 70.7 × 0.707 = 50 mm/s   ✓
合成 = √(50² + 50²) = 70.7 mm/s

         Y轴
         ↑
         │ 50 mm/s
         │
         │   ╱
         │ ╱ 70.7 mm/s (降速后)
         │╱
    ─────┼──────────→ X轴
         │ 50 mm/s
```

## 关键理解

### 1. 限制的是分量

**轴向速度限制**检查的是：
- 每个轴的速度分量
- **不是**合成速度

**例子**:
```
G1 X100 Y100 F10000 (合成速度166.7 mm/s)

即使合成速度很高，但如果：
- X轴分量 = 118 mm/s < 500 (X轴限制)
- Y轴分量 = 118 mm/s < 500 (Y轴限制)

则不会降速！
```

### 2. 保持方向不变

降速时：
- 所有轴按**相同比例**降速
- 保持移动**方向不变**
- 只改变**速度大小**

```
原始: (Vx, Vy, Vz) = (70.7, 70.7, 13.84)
降速: (Vx, Vy, Vz) = (60.0, 60.0, 12.0)  ← 都乘以0.867
方向: Vx:Vy:Vz = 70.7:70.7:13.84 = 60:60:12  ✓ 方向不变
```

### 3. 找最严格的限制

```cpp
min_feedrate_factor = 1.0

for each axis:
    if (轴分量 > 轴限制)
        factor = 轴限制 / 轴分量
        min_feedrate_factor = min(min_feedrate_factor, factor)

// 使用最小的factor（最严格的限制）
速度 *= min_feedrate_factor
```

## 实际例子：打印一条线

### G-code
```gcode
G1 X0 Y0 Z0.2 E0
G1 X100 Y50 Z0.2 E5 F3000
```

### 第二条命令分析

**移动参数**:
```
delta_X = 100mm
delta_Y = 50mm
delta_Z = 0mm
delta_E = 5mm
距离 = √(100² + 50²) = 111.8mm
目标速度 = 3000 mm/min = 50 mm/s
```

**速度分量**:
```
inv_distance = 1/111.8 = 0.00894

Vx = 50 × 100 × 0.00894 = 44.7 mm/s
Vy = 50 × 50 × 0.00894 = 22.35 mm/s
Vz = 0 mm/s
Ve = 50 × 5 × 0.00894 = 2.24 mm/s
```

**检查限制**（假设标准配置）:
```
Vx = 44.7 < 500 ✓
Vy = 22.35 < 500 ✓
Vz = 0 < 12 ✓
Ve = 2.24 < 120 ✓

所有轴都不超限，保持50 mm/s
```

**如果降低Z轴限制到1 mm/s**（极端例子）:
```
即使Vz = 0，也不受影响（因为没有Z移动）
只有当有Z移动时，Z轴限制才起作用
```

## 总结

### 关键点

1. **限制的是分量**，不是合成速度
2. **分量 = 合成速度 × (轴移动量 / 总距离)**
3. **降速保持方向**，所有轴同比例降速
4. **最严格限制**决定最终速度

### 计算公式

```
轴速度分量 = 合成速度 × (delta_轴 / 总距离)

如果 轴速度分量 > 轴限制:
    降速因子 = 轴限制 / 轴速度分量
    合成速度 *= 降速因子

同样适用于加速度！
```

### 物理意义

这个设计确保：
- 每个轴的电机不超过物理限制
- 移动方向始终正确
- 在满足所有限制的前提下尽可能快

这就是为什么即使是斜向移动，各轴的速度/加速度限制仍然有效！
