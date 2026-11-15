---
name: "FengquDB 业务性能实测"
description: "可核对的技术报告：以清楚的文字、统一图表和原始证据解释同一业务的测量结果。"
colors:
  green: "#126247"
  mysql-bar: "#799198"
  redis-bar: "#6e8594"
  hybrid-bar: "#788b7c"
  sqlite-bar: "#647e80"
  focus: "#175d8d"
  paper: "#f7f8f4"
  surface: "#fff"
  ink: "#152a2d"
  muted: "#506264"
  line: "#cbd4cf"
  soft: "#e9f1eb"
typography:
  display:
    fontFamily: 'system-ui, -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif'
    fontSize: "clamp(34px, 3.4vw, 46px)"
    fontWeight: 750
    lineHeight: 1.22
    letterSpacing: "-0.03em"
  headline:
    fontFamily: 'system-ui, -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif'
    fontSize: "28px"
    fontWeight: 700
    lineHeight: 1.4
    letterSpacing: "-0.02em"
  title:
    fontFamily: 'system-ui, -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif'
    fontSize: "21px"
    fontWeight: 700
    lineHeight: 1.45
    letterSpacing: "-0.02em"
  body:
    fontFamily: 'system-ui, -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif'
    fontSize: "16px"
    fontWeight: 400
    lineHeight: 1.7
  label:
    fontFamily: 'system-ui, -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif'
    fontSize: "13px"
    fontWeight: 600
    lineHeight: 1.7
  chart-value:
    fontFamily: 'system-ui, -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif'
    fontSize: "12px"
    fontWeight: 650
  comparison:
    fontFamily: 'system-ui, -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif'
    fontSize: "20px"
    fontWeight: 700
    lineHeight: 1.5
rounded:
  control: "6px"
spacing:
  tight: "8px"
  compact: "12px"
  paragraph: "16px"
  group: "20px"
  section: "24px"
  open: "36px"
  page: "clamp(24px, 5vw, 72px)"
  page-mobile: "22px"
components:
  navigation-link:
    textColor: "{colors.green}"
  report-select:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    padding: "10px 30px 10px 12px"
  report-notice:
    backgroundColor: "{colors.soft}"
    padding: "16px 22px"
  linear-chart:
    width: "100%"
  data-disclosure-summary:
    textColor: "{colors.green}"
    padding: "10px 0"
---

# Design System: FengquDB 业务性能实测

## Overview

**Creative North Star: "可核对的技术报告"**

这套视觉系统服务于 `docs/benchmarks` 的静态报告，范围与 [PRODUCT.md](PRODUCT.md) 一致。它以浅色纸面、深色正文和细线组织证据，让业务说明、测量、局限与复现入口保持清楚的阅读关系。它不为数据库 SDK 定义界面。

系统的表达来自准确的排版、稳定的图表结构和克制的色彩。页面使用系统字体、真实文本和 SVG 测量几何；现有实现没有位图、装饰插画或动效。页面任务、内容排序和 Read 模式由 [SURFACE.md](docs/benchmarks/SURFACE.md) 约束；本文件记录已经实现的视觉语言。

**Key Characteristics:**

- 浅色纸面与细线分隔，内容沿同一阅读轴展开。
- 深色正文、清楚的数字与稳定的五库颜色对应。
- 原生筛选和展开控件，证据就在对应图表之后。
- 正文适应窄屏，宽图表和逐轮表格在各自区域内滚动。

## Colors

前置令牌是精确色值的规范来源。CSS 自定义属性保留原值，图表色来自 `report.js`；这里按用途解释它们。

### Primary

- **报告深绿（green）**：链接、展开入口、FengquDB 柱形与标签、相对数值说明共用的强调色。

### Secondary

- **灰蓝（mysql-bar）**：MySQL 数据。
- **蓝灰（redis-bar）**：Redis 数据。
- **灰绿（hybrid-bar）**：MySQL+Redis 数据。
- **青灰（sqlite-bar）**：SQLite 数据。
- **焦点蓝（focus）**：键盘焦点轮廓，不表示测量结果或优劣。

### Neutral

- **浅色纸面（paper）**：整页背景与桌面固定筛选区。
- **白色表面（surface）**：原生选择框、数据表与跳转链接的背景。
- **深色墨迹（ink）**：正文、标题、数值和方法区的强分隔线。
- **次级墨迹（muted）**：环境说明、图例、轴标、方法正文和页脚。
- **分隔灰（line）**：页眉、筛选、业务行和页脚的细线。
- **浅绿底（soft）**：持久化设置提示带。

**The Stable Identity Rule.** 同一数据库在所有图表中保持同一颜色；颜色说明身份，数值和坐标说明结果。

## Typography

全页共用前置令牌中的系统无衬线字体栈，不加载外部字体。中英文使用同一层级；标题依靠字重和紧凑字距建立重点，正文保留舒展行距。

- **Display**：开场标题采用流体字号、紧凑行高与平衡换行。
- **Headline**：结果区与方法区的二级标题。
- **Title**：每个业务场景的三级标题；窄屏降为（20px）。方法小标题另用（18px）。
- **Body**：开场解释与基础阅读文字，段落宽度上限为（72ch）。业务说明和方法文字使用（14px），图例和展开入口使用（13px）。
- **Label**：筛选标签；窄屏降为（12px）。原生选择框文字在桌面使用（14px），窄屏使用（13px）。
- **Chart value**：图中数值使用较重字重；数值和表格采用等宽数字。窄屏将柱顶数值与数据库标签放大为（16px），轴刻度保留原字号。
- **Comparison**：业务说明旁的比值。窄屏降为（17px），与解释文字同行。

这是一套按阅读任务分配的离散字号，不宣称统一的数学比例。字标使用（24px）、字重（750），附属说明使用（13px）；窄屏隐藏附属说明。

**The Visible Number Rule.** 图中数值和单位必须保持为可读文本；表格使用等宽数字，正文保留自然的中文换行。

## Layout

页面主容器居中，最大宽度（1280px），两侧留白取前置 `spacing.page`。页眉以字标和锚点导航横向分布。开场为等宽双栏，栏间留白（8%），主标题与解释在底部对齐。提示带的说明栏宽（190px），右侧文字随容器伸展。

桌面筛选采用四栏比例（1 : 0.9 : 1.5 : 1.3），与纸面同色，滚动时固定在顶部。业务行以（230px）说明栏配合弹性图表栏，间距（40px），用顶部细线分开；方法说明采用双栏网格。段落、标签与区块使用前置间距档位，避免以独立卡片重复包裹每项结果。

在（900px）及以下，筛选变为两栏，业务说明栏缩为（185px）、间距为（24px）；提示带改为纵向排列。在（620px）及以下，页面留白改用 `spacing.page-mobile`，开场、业务行和方法区均为单栏，筛选保持两栏并取消固定定位，结果说明与页脚纵向排列。开场标题最大宽度为（9ch）。桌面锚点滚动留出（130px），窄屏改为（24px）。

图表 SVG 使用（800 × 324）坐标空间，显示宽度随栏伸展，但最小宽度保持（610px）。窄屏的图表区域向右扩到页面边缘，保留横向滚动条、提示文字和键盘焦点。逐轮表格在自己的容器中滚动。正文、页眉与筛选仍适应页面宽度。

保留宽图表与表格的局部滚动，阅读正文时不需要横向移动整页。

## Elevation & Depth

现有系统没有阴影、渐变、毛玻璃或浮起的容器。层次来自纸面与白色控件的轻微对比、浅绿提示带、细线和排版。方法区的上边线加重为（2px），形成长报告中的段落转折。桌面筛选区只通过固定位置和不透明纸面保持可读，不增加投影。

**The Flat Evidence Rule.** 用文字层级、留白和分隔线组织证据，保持既有的平面材质。

## Shapes

图表柱形、提示带和内容区保持直角。仅原生选择框使用前置 `rounded.control`，边线为（1px）。内容分隔线为（1px）；图表的网格线和零轴也使用细线，范围须线稍重（1.4px）。系统没有胶囊标签或装饰图标体系。

## Components

### Report navigation

字标与三个真实链接组成页眉，链接有下划线；两个链接跳到报告内部，另一个下载数据。导航文字为（14px），窄屏为（12px）；点击目标最小高度（44px）。悬停加深绿色，键盘焦点使用焦点蓝轮廓。跳过导航的链接平时移出视区，获得焦点时出现。

### Native filters

四组标签与原生 `select` 控件对应简介长度、调用线程、比较指标和业务场景。选择框采用白底、深色文字、细边线与轻微圆角，最小高度（44px）。不替换系统的展开、键盘选择和焦点行为；选择变化后从附带样本重新生成结果，业务选项来自实际场景数据，当前条件通过状态文本报告。

### Durability notice

这是与正文同宽的浅绿提示带，没有阴影。标题以深绿文字加重，解释使用正常字重。它与方法区共同解释确认时机和持久化差异，始终保持为文字内容。

### Scenario and linear chart

每项业务由标题、业务解释、相对数值说明、SVG 柱状图和逐轮数据入口组成。当前场景序列将读取、保存、新增、删除和字段修改放在前面，其余混合、批量与事务场景跟随数据顺序。

图中顺序固定为 MySQL、Redis、MySQL+Redis、SQLite、FengquDB。每张图独立确定线性刻度，以零为基线；柱形是当前筛选下各轮的中位数，须线是最小至最大值。柱顶显示数值，底部显示数据库名称。缺失测量显示“缺少数据”。SVG 有标题和可访问名称，外层滚动区可获得键盘焦点。

相对数值说明仅在五库都有测量时出现：吞吐比较 FengquDB 与其他最高值，P99 比较其他最低值与 FengquDB。文字明确分子、分母和指标方向，不依赖颜色表达结果。

### Raw measurement disclosure

原生 `details` / `summary` 在图下展开逐轮表格。入口使用绿色文字与系统展开指示，悬停增加下划线。表格有标题和列标题，首列左对齐，其余数字右对齐；白底、细行线、轻微的行悬停底色。宽表在可聚焦的独立区域内滚动。

### Method and evidence links

方法文字、环境、持久化差异和证据下载沿正文网格排布。下载与跳转使用普通链接，不伪装成按钮；序列化补充只在附带数据存在时显示。数据缺失时给出文字说明和 CSV 入口；无脚本时保留下载路径。

所有链接、选择框、展开入口与滚动区共用清楚的键盘焦点（3px 轮廓，4px 偏移）。现有页面没有动画或过渡，滚动为即时行为。

## Do's and Don'ts

### Do:

- **Do** 保持前置令牌与报告源码一致，让业务说明、图表和逐轮证据相邻。
- **Do** 在窄屏保留原生筛选、可聚焦的局部滚动区域和横向查看提示。
- **Do** 为数值保留单位、指标方向、样本含义与持久化设置说明。
- **Do** 沿用系统字体、平面背景、细线与真实文本。

### Don't:

- **Don't** 按当前胜负重排数据库、截断零基线或把线性刻度改为对数刻度。
- **Don't** 将缺失值补为零，隐藏落后结果，或把不同持久化窗口包装成相同可靠性。
- **Don't** 以装饰图片替代图表文字、实际测量几何或原始证据入口。
- **Don't** 将本报告的视觉约定扩展为数据库 SDK 的产品界面规格。
