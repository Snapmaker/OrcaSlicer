# Klipper vs OrcaSlicer：速度限制方式的重要差异

## 你发现的关键问题

**Klipper固件配置**:
```ini
[printer]
max_velocity: 300        # 限制合成速度
max_z_velocity: 10       # 单独限制Z轴
```

**OrcaSlicer配置**:
```
machine_max_speed_x: 500  # X轴速度限制
machine_max_speed_y: 500  # Y轴速度限制
machine_max_speed_z: 12   # Z轴速度限制
machine_max_speed_e: 120  # E轴速度限制
```

## 核心矛盾

### Klipper的限制方式（固件实际执行）

**max_velocity** 限制的是**合成速度**，不是分量！

```
例子：G1 X100 Y100 F6000 (合成100 mm/s)

Klipper检查：
- 合成速度 = √(Vx² + Vy²) = 100 mm/s
- max_velocity = 300 mm/s
- 100 < 300 ✓ 不降速

即使：
- X轴分量 = 70.7 mm/s
- Y轴分量 = 70.7 mm/s
Klipper不关心分量，只看合成速度！
```

**max_z_velocity** 是特例，限制Z轴分量：

```
因为Z轴通常很慢（丝杠驱动），单独限制
```

### OrcaSlicer的时间估算方式

**检查每个轴的分量**（我之前解释的方式）：

```cpp
// 检查X轴分量
if (X轴分量 > machine_max_speed_x) 降速
// 检查Y轴分量
if (Y轴分量 > machine_max_speed_y) 降速
// 检查Z轴分量
if (Z轴分量 > machine_max_speed_z) 降速
```

## 问题：时间估算可能不准确！

### 场景1：高速对角线移动

**G-code**: `G1 X200 Y200 F18000` (合成300 mm/s)

**Klipper固件**:
```
合成速度 = 300 mm/s
max_velocity = 300 mm/s
300 = 300 ✓ 允许执行，不降速
```

**OrcaSlicer估算**:
```
X轴分量 = 300 × (200/282.8) = 212 mm/s
Y轴分量 = 212 mm/s

如果 machine_max_speed_x = 500:
212 < 500 ✓ OrcaSlicer认为不降速

结果：估算认为可以跑300 mm/s ✓ 与Klipper一致
```

**这个场景没问题！**

### 场景2：三轴斜向移动

**G-code**: `G1 X100 Y100 Z20 F18000` (合成300 mm/s)

**Klipper固件**:
```
距离 = √(100² + 100² + 20²) = 144.6mm
合成速度 = 300 mm/s
max_velocity = 300 mm/s

但Z轴分量 = 300 × (20/144.6) = 41.5 mm/s
max_z_velocity = 10 mm/s
41.5 > 10 ✗ 需要降速！

降速因子 = 10 / 41.5 = 0.241
实际合成速度 = 300 × 0.241 = 72.3 mm/s
```

**OrcaSlicer估算**:
```
Z轴分量 = 300 × (20/144.6) = 41.5 mm/s
machine_max_speed_z = 12 mm/s
41.5 > 12 ✗ 需要降速

降速因子 = 12 / 41.5 = 0.289
实际合成速度 = 300 × 0.289 = 86.7 mm/s
```

**结果对比**:
- Klipper实际: 72.3 mm/s (被max_z_velocity=10限制)
- OrcaSlicer估算: 86.7 mm/s (被machine_max_speed_z=12限制)

**问题**: 如果machine_max_speed_z设置不等于max_z_velocity，时间估算会偏差！

### 场景3：纯XY高速移动（最大问题）

**G-code**: `G1 X200 Y0 F30000` (500 mm/s)

**Klipper固件**:
```
合成速度 = 500 mm/s
max_velocity = 300 mm/s
500 > 300 ✗ 降速到300 mm/s
```

**OrcaSlicer估算**:
```
X轴分量 = 500 mm/s
machine_max_speed_x = 500 mm/s
500 = 500 ✓ 不降速，认为可以跑500 mm/s
```

**严重偏差**！
- Klipper实际: 300 mm/s
- OrcaSlicer估算: 500 mm/s
- **时间估算偏短约40%**

## 为什么会有这个差异？

### Marlin固件的限制方式

Marlin（OrcaSlicer最初针对的固件）使用**per-axis限制**：

```c
// Marlin固件代码（伪代码）
for (axis in XYZE) {
    if (axis_velocity[axis] > max_speed[axis])
        降速;
}
```

这正是OrcaSlicer时间估算的逻辑！

### Klipper的不同设计哲学

Klipper使用**合成速度限制**:

```python
# Klipper固件代码（伪代码）
velocity = sqrt(vx² + vy² + vz²)
if velocity > max_velocity:
    降速
```

**原因**：
- Klipper的运动规划更先进
- 考虑的是打印头的实际移动速度
- 而不是单个电机的速度

## 如何配置才能准确？

### 方法1：保守配置（推荐）

对于Klipper打印机，在OrcaSlicer中：

```
假设Klipper配置：
max_velocity = 300 mm/s
max_z_velocity = 10 mm/s

OrcaSlicer配置（保守）：
machine_max_speed_x = 300  # 不是500！
machine_max_speed_y = 300  # 不是500！
machine_max_speed_z = 10   # 匹配max_z_velocity
machine_max_speed_e = 120  # E轴通常单独限制
```

**原理**: 将XY的限制设为max_velocity，这样：
- 纯X移动: 300 mm/s（正确）
- 纯Y移动: 300 mm/s（正确）
- 对角线XY: 会被降速到212 mm/s（**偏保守**）

**缺点**: 对角线移动会略微高估时间

### 方法2：激进配置（更准确但复杂）

```
OrcaSlicer配置：
machine_max_speed_x = 424  # 300 × √2
machine_max_speed_y = 424  # 300 × √2
machine_max_speed_z = 10
```

**原理**: 对角线移动时，分量 = 300/√2 ≈ 212，需要轴限制 = 300×√2 ≈ 424

**缺点**:
- 对于纯X/Y移动，会高估速度
- 计算复杂

### 方法3：接受偏差（实用）

```
OrcaSlicer配置：
machine_max_speed_x = 500  # 电机物理限制
machine_max_speed_y = 500
machine_max_speed_z = 10
```

**接受**：
- 高速纯X/Y移动时，时间估算会偏短
- 但大部分打印是复杂路径，影响不大
- 用户了解这个限制即可

## OrcaSlicer是否应该改进？

### 理想方案：添加Klipper模式

在时间估算中添加两种模式：

```cpp
if (m_flavor == gcfKlipper) {
    // Klipper模式：检查合成速度
    float composite_velocity = sqrt(vx² + vy² + vz²);
    if (composite_velocity > max_velocity)
        降速;

    // Z轴单独检查
    if (vz > max_z_velocity)
        降速;
} else {
    // Marlin模式：检查各轴分量（当前逻辑）
    for each axis:
        if (axis_velocity > axis_max)
            降速;
}
```

### 需要添加的配置

```cpp
def = this->add("machine_max_velocity", coFloat);
def->label = L("Maximum velocity");
def->tooltip = L("Maximum toolhead velocity (Klipper max_velocity)");
def->sidetext = L("mm/s");
def->mode = comAdvanced;
def->set_default_value(new ConfigOptionFloat(300.0));
```

## 实际影响评估

### 对U1打印机的影响

U1使用什么固件？
- 如果是Marlin系：当前逻辑完全正确 ✓
- 如果是Klipper：可能有偏差

### 典型打印任务的偏差

**正常打印**（大部分是中低速、复杂路径）:
- 偏差 < 5%（可接受）

**高速打印**（直线多、速度高）:
- 偏差可能达到20-30%

**首层/慢速打印**:
- 几乎无偏差（速度远低于限制）

## 总结

### 你的观察非常重要！

发现了OrcaSlicer（Marlin-style）和Klipper的限制方式差异：

| 固件 | 限制方式 | OrcaSlicer估算 | 匹配度 |
|-----|---------|---------------|-------|
| Marlin | Per-axis分量 | Per-axis分量 | ✓ 完美 |
| Klipper | 合成速度 + Z轴 | Per-axis分量 | ⚠️ 有偏差 |

### 实用建议

1. **了解固件类型**
2. **Marlin打印机**: 当前配置完全准确
3. **Klipper打印机**:
   - 保守: 将XY限制设为max_velocity
   - 激进: 接受偏差
4. **Z轴**: 始终匹配max_z_velocity

### 长期改进方向

OrcaSlicer可以：
1. 检测gcfKlipper flavor
2. 添加max_velocity配置
3. 实现Klipper风格的速度限制检查
4. 提供更准确的Klipper时间估算

---

**你的问题触及了一个真正的设计差异！** 这解释了为什么某些Klipper用户可能会发现时间估算不够准确。
