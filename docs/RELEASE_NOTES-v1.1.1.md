# ChapterUploader v1.1.1

修两个「进书了却读不到目录 / 有几章被莫名跳过」的问题。定位过程都写进了 README 的
「读不到章节目录 / `__yw.chapterList is not a function`」小节，这里只记结论。

## 1. 进书之后仍然提示「读不到这本书的章节目录」

**现象**：客户端已经停在写作页，侧栏也有「草稿 / 已发布」，点「开始定时发布」还是弹这句提示，
日志里带着 `__yw.chapterList is not a function`。

**原因**：注入到页面里的是**上一版脚本**。exe 首次运行会把内置脚本导出成
`exe目录/js/site.js`（本意是让人现场改选择器不用重编），升级 exe 之后这份旧副本仍在，
并且优先于 exe 里编译进去的新脚本被读走 —— 旧脚本里根本没有 `chapterList`。

**修法**：

- `js/site.js` 文件头加版本标记 `// @sitejs 26`，`mainwindow.cpp` 里加 `kSiteJsVer = 26`
- `siteScript()` 读 exe 目录副本时先认标记：标记一致 → 当作用户自定义直接用；
  标记是旧版本 → 判定为升级残留，**就地用内置脚本覆盖**（日志打「已自动更新到 v26」）；
  没有标记 → 本次改用内置脚本，并提示怎么恢复
- 注入后加一次自检：把 `__yw.ver` / `chapterList` / `setScheduleDate` / `location.hash` 打进日志，
  连错页面（停在书架）或缺函数都会当场说清楚

## 2. 回收站里的删稿副本把正文章挤掉

**现象**：第 `173/174/175/176/180` 章明明是草稿，排期时却被当成「回收站」跳过。

**原因**：客户端 `chapterStore` 里「回收站」是一整卷，删掉的旧章连同编号一起留着 ——
同一个「第173章」有两条，一条 `status=1`（正文卷），一条 `status=7`（回收站）。
脚本原先照单全收，C++ 侧按章号建 `QHash`，后写的那条（回收站）把前一条盖掉。

**修法**：`_chapterIndex()` 与 `chapterList()` 都排除 `status=7` 和整个「回收站」卷。
本书实测：目录 257 条 → 250 条，重复章号 0，草稿 79 章，正确识别 `172~249`。

## 3. 其他

- 报错弹窗分两种：脚本旧了就说「删掉 js\site.js 再断开重连」，单纯没进书才说「先进入作品」
- 新增 `__yw.storeProbe()`：读不到目录时自动把 `hasApp / hasPinia / stores / listLen / vols`
  打进日志，省得靠猜
- 顶部版本号 v1.1.1

## 4. 定时发布填不上日期/时间（`日历面板没弹出来`）

**现象**：弹窗能打开、开关也能勾上，但日志里每章都是
`⚠ 日期/时间没填对（… 实际「 」：日历面板没弹出来）`，3 章全灭。

**原因**：客户端这个 ant-design-vue Picker 是「`mouseup` 开面板、`click` 关面板」的切换语义。

- 只发 `input.click()` → 只有 click，没有 mousedown/mouseup → 面板不开（原来的实现就是这样，`focus()` 只让容器多了个 `ant-picker-focused`）
- 把 `mousedown`+`mouseup`+`click` 全发一遍 → 打开后立刻又被 click 关掉
- 旧面板关闭后仍留在 DOM 里（带 `ant-slide-up-leave`），按老选择器能查到「假面板」，点格子当然没反应，最后点「确定」提交的是组件里的旧时间

**修法**：

| 位置 | 做法 |
|---|---|
| `_openPicker` | 只发 `pointerdown → mousedown → pointerup → mouseup`，**不发 click**；每次点击后轮询面板是否真出现；开之前先点弹窗标题把残影关掉 |
| `_pickerDropdown` | 过滤 `ant-picker-dropdown-hidden` 与 `-leave`，且要求真的可见；日期/时间按各自 panel 精确匹配，不再「拿不到就退回第一个」 |
| 兜底 1 | 调组件 `onUpdate:value`（就是 `:value` + `@update:value` 那对，等价于替用户选了一次）—— 实测该构建下 `__vueParentComponent` 不可用，保留作兼容 |
| 兜底 2 | 临时放开 `readonly`，`execCommand('insertText')` 键入 + 回车提交；已验证会让组件状态一起变（再开面板时选中的就是键入值） |
| 复核 | `_timeByPanel` 点完格子先确认「选中态」是目标时分，再点「确定」，最后比对输入框显示值 |

真机连测四组（`2026-11-06 19:00`、`2026-12-01 08:05`、`2027-01-02 23:55`、`2026-11-14 19:30`，
含跨月/跨年翻页）全部一次填对，`via` 显示 `calendar / panel+ok` 或 `calendar / typing`。

> 排障工具同步升级：`tools/wpage.py <CBID> "<js>"` 可对指定作品写作页注入脚本并执行；
> 两个脚本都会先 `delete window.__yw` 再注入，避免被 `ver >= N` 的缓存守卫挡住（调试时踩过）。
