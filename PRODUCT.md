# FengquDB 性能报告页面

此文件约束 `docs/benchmarks` 静态报告，不代替数据库产品规格。

## Platform

web，包含桌面和移动浏览器。

## Stack

用户确认使用零依赖静态 HTML / CSS / JavaScript，可直接打开或发布到 GitHub Pages。

读者是准备接入嵌入式数据库的开发者。页面帮助读者比较相同业务在 MySQL、Redis、MySQL+Redis、SQLite 和 FengquDB 上的吞吐与延迟，并下载原始测量及复现材料。

用户确认：零依赖静态 HTML，可直接打开，可发布到 GitHub Pages。柱子顺序固定为 MySQL、Redis、MySQL+Redis、SQLite、FengquDB；线性刻度、零基线，常用增删改查业务优先。只使用实测数据，不遗漏失败或落后结果。清楚说明性能模式的确认时机和各数据库的持久化差异。

图表使用真实文本、SVG 几何和本地数据；不需要位图素材。报告准备好后由用户决定公开发布。
