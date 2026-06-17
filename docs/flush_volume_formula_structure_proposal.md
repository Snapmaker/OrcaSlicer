# 冲刷量颜色距离公式结构调整方案

## 背景

当前冲刷量公式已经针对 `PLA Snapspeed冲刷矩阵.xlsx` 做过多轮参数调优。最新提交版本在总体指标上有改善：

- MAE：18.75 -> 16.45
- RMSE：29.07 -> 22.31
- LowMAE：12.98
- HighMAE：35.90

但从误差分组看，仍存在一个系统性风险：

```text
高饱和源色 -> 低饱和、中高亮目标色
```

这类转换经常低估冲刷量，典型样本包括：

- 蓝色 -> 珍珠白
- 橄榄绿 -> 珍珠白
- 橄榄绿 -> 灰色
- 品红色 -> 灰色
- 黄色 -> 珍珠白

同时，不能简单地对所有“高饱和 -> 低饱和”转换加冲刷，因为粉色、奶油白、浅暖色目标存在过估风险。

## 为什么要改公式结构

现有方案主要是在基础颜色距离公式上叠加几个经验补偿项：

```text
white_risk
red_residue
gray_residue
```

这种方式短期可以压低部分误差，但长期存在两个问题。

第一，补偿项偏向局部样本。比如 `red_residue` 和 `gray_residue` 明显围绕红色/品红色源色设计，容易把“高色度源色到低色度高亮目标更难清洗”的通用现象，拟合成“红色特别难清洗”。

第二，当前公式没有把“高冲刷区更需要准确”显式写入结构。用户原则是：

```text
冲刷量越小，允许误差越大。
```

这反过来意味着：

```text
冲刷量越大，误差越应该收敛。
```

所以下一步不建议继续堆针对某个颜色名称或某个 hue 窗口的补丁，而应该把公式结构改成连续、低维、可解释的“残留风险模型”。

## 建议的新公式结构

建议把最终冲刷量拆成两层：

```text
V = V_base * (1 + A_high * R_stain * K)
```

其中：

- `V_base`：当前基础颜色距离公式给出的冲刷量。
- `A_high`：高冲刷激活项，用来体现“低冲刷少干预，高冲刷更严格”。
- `R_stain`：染色残留风险项。
- `K`：整体补偿强度。

### 高冲刷激活项

```text
A_high = smoothstep(V_low, V_high, V_base)
```

含义：

- `V_base` 很小时，`A_high` 接近 0，基本不补偿。
- `V_base` 中等时，补偿平滑进入。
- `V_base` 较高时，补偿充分生效。

这样可以避免低冲刷样本被整体抬高，也符合“冲刷量越小，允许误差越大”的原则。

### 染色残留风险项

建议定义为：

```text
R_stain =
  G_src_chroma
* G_chroma_drop
* G_target_light
* G_target_neutral
* G_not_warm_pastel
```

各项含义：

```text
G_src_chroma
```

源色越饱和、色度越高，残留风险越高。

```text
G_chroma_drop
```

从高色度源色冲到低色度目标时，残留更容易显现。

```text
G_target_light
```

目标越亮，残留越容易被观察到。

```text
G_target_neutral
```

目标越接近白色、灰色这类中性低饱和颜色，补偿越强。

```text
G_not_warm_pastel
```

对奶油白、粉色、浅暖色目标做连续阻尼，避免误判为纯白/灰。

## 推荐使用的颜色空间

优先建议使用 `OKLab / OKLCH` 的 `L/C/h` 来表达门控：

```text
L_s, C_s, h_s = source OKLCH
L_t, C_t, h_t = target OKLCH
```

相比 HSV，OKLCH 的亮度和色度更接近感知语义，更适合判断：

- 源色是否高色度。
- 目标是否低色度。
- 目标是否高亮。
- 目标是否接近中性白/灰。

如果短期不引入 OKLCH，也可以继续用 HSV/RGB 近似，但结构上仍应保持上述门控逻辑。

## 关键概念定义

本方案中的“高色度源色”和“低色度、高亮、近中性色”不建议用颜色名称定义，而应使用连续颜色空间变量定义。

### 高色度源色

使用 OKLCH 时，定义为源色 `C_s` 较高：

```text
G_src_chroma = smoothstep(C_s0, C_s1, C_s)
```

建议初始阈值：

```text
C_s0 = 0.08
C_s1 = 0.16
```

含义：

- `C_s < 0.08`：不认为是高色度源色。
- `C_s > 0.16`：高色度风险充分激活。
- 中间区间平滑过渡。

如果短期继续使用 HSV/RGB，可以用 `S * V` 近似源色色度：

```text
src_chroma = S_s * V_s
G_src_chroma = smoothstep(0.35, 0.65, src_chroma)
```

### 低色度目标

使用 OKLCH 时，定义为目标色度 `C_t` 较低：

```text
G_target_low_chroma = 1 - smoothstep(C_t0, C_t1, C_t)
```

建议初始阈值：

```text
C_t0 = 0.025
C_t1 = 0.080
```

含义：

- `C_t < 0.025`：非常接近白色或灰色。
- `C_t > 0.080`：已经有明显色彩，不再按低色度目标强补偿。
- 中间区间平滑过渡。

HSV/RGB 近似：

```text
G_target_low_chroma = 1 - smoothstep(0.08, 0.25, S_t)
```

### 高亮目标

使用 OKLCH 时，定义为目标亮度 `L_t` 较高：

```text
G_target_light = smoothstep(L_t0, L_t1, L_t)
```

建议初始阈值：

```text
L_t0 = 0.70
L_t1 = 0.88
```

含义：

- `L_t < 0.70`：不按高亮目标处理。
- `L_t > 0.88`：高亮风险充分激活。
- 中间区间平滑过渡。

HSV/RGB 近似可继续使用当前亮度公式：

```text
target_luminance = 0.3 * R + 0.59 * G + 0.11 * B
G_target_light = smoothstep(0.70, 0.88, target_luminance)
```

### 近中性色

近中性色不应只等同于“低饱和”。粉色、奶油白、浅暖色也可能低饱和，但它们仍有明确色相，不能完全按白色或灰色处理。

OKLCH 版本可以先用目标色度低来近似：

```text
G_target_neutral = 1 - smoothstep(0.025, 0.080, C_t)
```

如果继续用 RGB/HSV，建议增加 RGB 通道差辅助判断：

```text
rgb_spread = max(R, G, B) - min(R, G, B)
G_rgb_neutral = 1 - smoothstep(0.04, 0.14, rgb_spread)
```

最终可以组合为：

```text
G_target_neutral =
  G_target_low_chroma
* G_rgb_neutral
```

这样可以避免把低饱和但有明确暖色或粉色倾向的目标完全误判为白/灰。

### 色度下降

色度下降用于描述“高色度源色冲到低色度目标”的风险：

```text
G_chroma_drop = smoothstep(0.06, 0.14, C_s - C_t)
```

HSV/RGB 近似：

```text
src_chroma = S_s * V_s
dst_chroma = S_t * V_t
G_chroma_drop = smoothstep(0.25, 0.55, src_chroma - dst_chroma)
```

### 风险项完整示例

OKLCH 版本：

```text
G_src_chroma       = smoothstep(0.08, 0.16, C_s)
G_chroma_drop      = smoothstep(0.06, 0.14, C_s - C_t)
G_target_light     = smoothstep(0.70, 0.88, L_t)
G_target_neutral   = 1 - smoothstep(0.025, 0.080, C_t)

R_stain =
  G_src_chroma
* G_chroma_drop
* G_target_light
* G_target_neutral
```

HSV/RGB 近似版本：

```text
src_chroma = S_s * V_s
dst_chroma = S_t * V_t
dst_lumi = 0.3 * R_t + 0.59 * G_t + 0.11 * B_t
rgb_spread = max(R_t, G_t, B_t) - min(R_t, G_t, B_t)

G_src_chroma     = smoothstep(0.35, 0.65, src_chroma)
G_chroma_drop    = smoothstep(0.25, 0.55, src_chroma - dst_chroma)
G_target_light   = smoothstep(0.70, 0.88, dst_lumi)
G_target_neutral = 1 - smoothstep(0.04, 0.14, rgb_spread)

R_stain =
  G_src_chroma
* G_chroma_drop
* G_target_light
* G_target_neutral
```

一句话定义：

```text
高色度源色 = 源色 C 足够高，或 HSV 中 S * V 足够高。
低色度、高亮、近中性色 = 目标 C 足够低、L 足够高、RGB 通道差足够小。
```

## 粉色和奶油白的保护方式

不建议写成硬规则：

```text
if target == 粉色 then reduce
if target == 奶油白 then reduce
```

建议写成连续阻尼：

```text
G_warm_pastel =
  G_target_light
* G_target_has_some_chroma
* H_warm_or_pink

G_not_warm_pastel = 1 - damping * G_warm_pastel
```

含义：

- 目标很亮，但不是完全中性。
- 目标有一定色度。
- 目标 hue 偏暖、偏黄、偏粉、偏橙。

这种目标虽然低饱和，但视觉上允许带底色，不应该按纯白/灰目标强补偿。

## 当前方案的好处

### 1. 更符合问题本质

现有主要风险不是“红色难冲”，而是：

```text
高色度源色 -> 低色度、高亮、近中性目标
```

新结构直接表达这个条件，能覆盖蓝、紫、绿、黄、红等不同高饱和源色。

### 2. 减少对具体颜色名称的过拟合

新方案不依赖：

- 源色名称
- 目标色名称
- 某几个固定样本

而是依赖连续颜色空间变量，因此更适合作为默认公式结构。

### 3. 低冲刷区更稳定

`A_high` 会让低 `V_base` 区域的补偿自动减弱，避免为了修高风险样本而把大量低冲刷样本推高。

### 4. 可解释性更强

每个门控项都有明确含义：

- 源色是否容易残留。
- 目标是否容易显残留。
- 是否从高色度掉到低色度。
- 是否属于暖白/粉色等不应强补偿的目标。

后续调参时可以按分组误差定位，而不是逐格猜测。

## 当前方案的风险

### 1. OKLCH 引入成本

如果代码中目前没有 OKLab/OKLCH 转换，需要新增颜色空间转换逻辑。虽然公式更合理，但实现成本和测试成本会增加。

### 2. 门控仍可能过拟合当前矩阵

即使不用颜色名称，只要阈值调得太窄，仍可能只命中当前 `PLA Snapspeed` 色卡。因此阈值应保持平滑、宽泛，并通过分组指标审核。

### 3. 暖色阻尼可能压低真实需要补偿的样本

奶油白、粉色、浅黄色目标确实有过估风险，但其中部分转换也可能需要较高冲刷。阻尼不能做成硬排除，只能做连续衰减。

### 4. 高冲刷激活可能漏掉低基准但高实测的异常点

如果 `V_base` 本身严重低估，`A_high` 可能激活不足。因此 `A_high` 不应只看 `V_base`，可以考虑同时参考 `R_stain`：

```text
A_high = max(
  smoothstep(V_low, V_high, V_base),
  risk_boost * R_stain
)
```

这样可以避免高风险但基础公式偏低时完全补不上。

### 5. 仍需要外部材料验证

当前数据只覆盖一个 PLA Snapspeed 矩阵。新结构更通用，但不能证明对所有材料都成立。提交前最好至少用另一组材料或人工挑选色卡做 sanity check。

## 对抗审核中明确反对的方向

不建议采用以下结构：

```text
if sourceName == "红色" add X
if targetName in ["珍珠白", "冷白色", "奶油白"] add X
if targetName == "粉色" subtract X
红色 -> 珍珠白单独加 140
蓝色 -> 珍珠白单独加 90
```

这些方案可能降低当前矩阵误差，但泛化性弱，本质是记忆样本。

## 下一轮验证指标

下一轮不应只看总 MAE。建议增加高冲刷加权指标：

```text
WeightedMAE_ByWash = sum(abs(error) * weight) / sum(weight)
```

权重建议：

```text
wash <= 0.25: weight = 0.5
0.25 < wash <= 0.50: weight = 1.0
0.50 < wash <= 0.75: weight = 1.5
wash > 0.75: weight = 2.5
```

或者连续形式：

```text
weight = 0.5 + 2.0 * wash^1.5
```

## 放行标准

下一轮候选公式建议至少满足：

```text
MAE <= 16.75
RMSE <= 22.81
LowMAE <= 13.98
HighMAE <= 34.50
```

并且风险分组必须改善：

```text
high_saturation_source -> low_saturation_mid/high_light_target:
  MAE 下降
  Bias 绝对值下降
  UnderRate 下降
```

同时保护粉色和奶油白：

```text
pink:
  OverRate 不上升
  Bias 不恶化

cream_white:
  OverRate 不上升
  Bias 不恶化
```

如果一个候选只降低总 MAE，但高冲刷区、粉色、奶油白或高饱和到低饱和风险组变差，应判定失败。

## 建议实施顺序

1. 先保留当前 `V_base`，不要同时改基础距离公式。
2. 新增通用 `R_stain` 风险项，替代长期依赖 `red_residue / gray_residue` 的方向。
3. 加入 `A_high`，确保低冲刷区不会被过度抬高。
4. 用分组报表调参，而不是只看总 MAE。
5. 通过对抗审核后，再考虑是否移除或弱化现有红色专用补偿。

## 结论

建议下一轮公式结构从“局部颜色补丁”转向“通用残留风险模型”：

```text
高色度源色
+ 色度大幅下降
+ 高亮低色度近中性目标
+ 高冲刷激活
- 暖白/粉色连续阻尼
```

这个结构更符合当前误差分布，也更符合“冲刷量越小，允许误差越大；冲刷量越大，越需要准确”的产品原则。
