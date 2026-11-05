// 起点作家助手（阅文 Electron 客户端 5.x）页面侧适配脚本
// 由 ChapterUploader 通过 DevTools 协议注入，暴露 window.__yw 供 C++ 侧调用。
// 选择器全部来自客户端 authorwrite_5.24.0 的渲染代码：
//   侧栏        .action-btn（新建章 / 新建卷）
//   校对面板    .correct-content / .check-result / .correct-tips
//   病句列表    ul.correc-list > li.correc-item，条目里 button#check_modify_<idx>.btn.primary
//   检测按钮    button.check.dscheck（文案「检测」/「检测中」）
//   一键改全部  button.modify-all
//   编辑器      TinyMCE（inline），window.tinymce.activeEditor
(function () {
  if (window.__yw && window.__yw.ver >= 25) return 'cached';

  const txt = el => (el ? (el.innerText || el.textContent || '').replace(/\s+/g, ' ').trim() : '');

  function visible(el) {
    if (!el || !el.getBoundingClientRect) return false;
    const r = el.getBoundingClientRect();
    if (r.width < 2 || r.height < 2) return false;
    const st = getComputedStyle(el);
    return st.visibility !== 'hidden' && st.display !== 'none' && st.opacity !== '0';
  }

  function depth(el) { let d = 0; while (el) { d++; el = el.parentElement; } return d; }

  function findByText(re, opt) {
    opt = opt || {};
    const nodes = [...document.querySelectorAll('button,a,div,span,li,i,em')];
    const cands = [];
    for (const el of nodes) {
      if (!visible(el)) continue;
      const t = txt(el);
      if (!t || t.length > 24) continue;
      if (!re.test(t)) continue;
      const r = el.getBoundingClientRect();
      if (r.width > (opt.maxWidth || 320) || r.height > (opt.maxHeight || 64)) continue;
      cands.push(el);
    }
    if (!cands.length) return null;
    cands.sort((a, b) => depth(b) - depth(a));   // 取最里层那个，避免点到外层容器
    return cands[0];
  }

  function click(el) {
    if (!el) return false;
    const r = el.getBoundingClientRect();
    const o = {
      bubbles: true, cancelable: true, view: window, button: 0,
      clientX: Math.round(r.left + r.width / 2), clientY: Math.round(r.top + r.height / 2),
    };
    el.dispatchEvent(new MouseEvent('mousedown', o));
    el.dispatchEvent(new MouseEvent('mouseup', o));
    el.dispatchEvent(new MouseEvent('click', o));
    return true;
  }

  // 编辑器集合里有些实例是上一章留下的「僵尸」：DOM 已经不在文档里，
  // 但 tinymce.activeEditor 可能还指着它 —— 那样 setContent 写进去页面上也看不见。
  function editorList() {
    const tm = window.tinymce;
    if (!tm) return [];
    try { return Array.prototype.slice.call(tm.editors || []); } catch (e) { return []; }
  }

  function editorLive(ed) {
    try {
      const el = (ed.getElement && ed.getElement()) || (ed.getBody && ed.getBody());
      return !!el && document.contains(el) && visible(el);
    } catch (e) { return false; }
  }

  function editor() {
    const list = editorList();
    const live = list.filter(editorLive);
    const pool = live.length ? live : list;
    if (!pool.length) return null;
    // 1) activeEditor 还活着 → 光标真在正文里，最可信
    const tm = window.tinymce;
    if (tm.activeEditor && pool.indexOf(tm.activeEditor) >= 0) return tm.activeEditor;
    // 2) 名字里带 chapter/content 的那个
    const hit = pool.find(e => /chapter|content/i.test(
      (e.id || '') + ' ' + (((e.getElement && e.getElement()) || {}).className || '')));
    return hit || pool[0];
  }

  // 面积最大的可见 contenteditable（没有 tinymce 时的退路）
  function biggestEditable() {
    const cands = [...document.querySelectorAll('[contenteditable="true"],[contenteditable=""]')]
      .filter(visible)
      .map(el => { const r = el.getBoundingClientRect(); return { el, a: r.width * r.height }; })
      .sort((a, b) => b.a - a.a);
    return cands.length ? cands[0].el : null;
  }

  function visibleTexts() {
    return [...document.querySelectorAll('button,.action-btn,.menu-item')]
      .filter(visible).map(txt).filter(t => t && t.length <= 12).slice(0, 40);
  }

  // ---------- 校对面板：错别字 / 病句 是两个独立的检测块 ----------
  // 面板结构（实测）：一块 = 标题行「错 错别字：检测完成」+ 说明行「没有任何问题，不愧是你!」+ 一个「检测」按钮。
  // 注意 ① 状态词在标题那一行里，不能只取最里层那个「错别字」小标签；
  //      ② button.check.dscheck 会匹配到 3 个（错别字 / 病句 / 风险段落「20墨水检测」），
  //         不能靠「离哪个标题近」去猜，否则第三块会粘到病句上。
  // 所以改成：先拿标题行，再按垂直区间给每块分配它下面第一个按钮。

  function checkButtons() {
    let list = [...document.querySelectorAll('button.check.dscheck')].filter(visible);
    if (!list.length) {
      list = [...document.querySelectorAll('button')]
        .filter(b => visible(b) && /检测/.test(txt(b)));
    }
    return list.map(b => ({ el: b, text: txt(b), rect: b.getBoundingClientRect() }))
               .sort((a, b) => a.rect.top - b.rect.top);
  }

  // 标题行候选：文字里只出现「错别字」或只出现「病句」，尺寸像一行
  function checkHeadLines() {
    const out = [];
    for (const el of document.querySelectorAll('div,span,p,li,label,h3,h4')) {
      if (!visible(el)) continue;
      const t = txt(el);
      if (!t || t.length > 40) continue;
      const hasS = t.indexOf('病句') >= 0;
      const hasT = t.indexOf('错别字') >= 0;
      if (hasS === hasT) continue;                 // 两种都有（外层容器）或都没有
      const r = el.getBoundingClientRect();
      if (r.width > 480 || r.height > 70) continue;
      out.push({ el: el, kind: hasS ? 'sentence' : 'typo', text: t, rect: r });
    }
    // 同一行里嵌套的只留「文字最长」的那个 —— 它才带着『：检测完成 / ：未检测』
    out.sort((a, b) => a.rect.top - b.rect.top);
    const kept = [];
    for (const h of out) {
      const dup = kept.find(k => k.kind === h.kind && Math.abs(k.rect.top - h.rect.top) < 12);
      if (!dup) { kept.push(h); continue; }
      if (h.text.length > dup.text.length) kept[kept.indexOf(dup)] = h;
    }
    return kept.sort((a, b) => a.rect.top - b.rect.top);
  }

  // 每一块 = 标题行 + 该行到下一个标题行之间、第一个「检测」按钮
  function checkBlocks() {
    const heads = checkHeadLines();
    const btns = checkButtons();
    const blocks = heads.map((h, i) => {
      const nextTop = (i + 1 < heads.length) ? heads[i + 1].rect.top : Infinity;
      const band = btns.filter(b => b.rect.top >= h.rect.top - 6 && b.rect.top < nextTop - 2);
      return {
        kind: h.kind, header: h.text, headEl: h.el, rect: h.rect, nextTop: nextTop,
        btn: band.length ? band[0].el : null,
        btnText: band.length ? band[0].text : '',
      };
    }).filter(b => b.btn);                          // 没有按钮的标题（说明文字/重复标题）不算一块

    // 没有标题可用时，退化成「按按钮顺序」：第一个错别字、第二个病句
    if (!blocks.length) {
      return btns.slice(0, 2).map((b, i) => ({
        kind: i === 0 ? 'typo' : 'sentence', header: '', headEl: null,
        rect: b.rect, nextTop: Infinity, btn: b.el, btnText: b.text,
      }));
    }
    return blocks;
  }

  function findBlock(kind) {
    const list = checkBlocks();
    if (!kind) return list[0] || null;
    return list.find(b => b.kind === kind) || null;
  }

  function kindLabel(kind) {
    return kind === 'sentence' ? '病句' : (kind === 'typo' ? '错别字' : String(kind || ''));
  }

  // 这一块垂直区间里的文字（到下一个标题行为止，绝不掺隔壁那块）
  function blockText(b) {
    if (!b) return '';
    const cx = b.rect.left + b.rect.width / 2;
    const seen = {};
    const parts = [];
    for (const el of document.querySelectorAll('div,span,p,li,button,i,b,em,strong')) {
      if (!visible(el)) continue;
      const r = el.getBoundingClientRect();
      if (r.height < 2 || r.width < 2) continue;
      if (r.top < b.rect.top - 2) continue;
      if (b.nextTop !== Infinity && r.top > b.nextTop - 2) continue;
      if (Math.abs((r.left + r.width / 2) - cx) > 280) continue;
      const t = txt(el);
      if (!t || t.length > 60 || seen[t]) continue;
      seen[t] = 1;
      parts.push(t);
      if (parts.join(' ').length > 200) break;
    }
    return parts.join(' | ');
  }

  // busy（检测中）/ done（出结果）/ idle（未检测）/ unknown
  // 先判「出结果」：用户确认过「没有任何问题，不愧是你！」就是通过，不能被别处的「检测中」拖住
  const RE_DONE = /检测完成|没有任何问题|没有问题|发现|疑似病句|检测失败|请重试/;
  const RE_BUSY = /检测中|正在检测/;

  function blockState(b) {
    if (!b || !b.btn) return 'unknown';
    const head = (b.header || '').trim();
    const btn = txt(b.btn);
    if (RE_DONE.test(head)) return 'done';
    if (RE_BUSY.test(btn) || RE_BUSY.test(head)) return 'busy';
    if (/未检测/.test(head)) return 'idle';
    // 标题里没带状态（有的版本状态另起一行）→ 扫这一块的垂直区间
    const zone = blockText(b);
    if (RE_DONE.test(zone)) return 'done';
    if (RE_BUSY.test(zone)) return 'busy';
    if (/未检测/.test(zone)) return 'idle';
    return 'unknown';
  }

  // ---------- 正文里的红线 ----------
  // 病句/错别字在正文里都是自定义元素 <el-word>：
  //   病句：<el-word data-grammaridx=… data-origin-word="原句" data-correct-word="建议句">
  //           <span class="grammar-mark">原句</span></el-word>
  //   错别字：<el-word data-dsidx=… data-origin-word="原词" data-correct-word="建议词">原词</el-word>
  // 应用在 #editor-body 的 mouseover 上出「修改建议」框（.context-menu.correc-context），
  // 框里 .context-primary > label 是蓝色的建议，点它才会真正改正文并抹掉红线。
  // 之前按「列表里的『修改』按钮」去点 —— 病句那块根本没有那个按钮（只有「忽略」），一条也改不动。

  function redWords() {
    return [...document.querySelectorAll('.mce-content-body el-word')].filter(el => {
      const d = el.dataset;
      if (!(d.grammaridx || d.dsidx || d.proidx || d.readeridx || d.customidx)) return false;
      if (d.grammaridx && d.grammarIgnore === 'true') return false;
      if (d.dsidx && d.dsIgnore === 'true') return false;
      return true;
    });
  }

  const grammarWords = () => redWords().filter(el => el.getAttribute('data-grammaridx'));

  function tipBox() {
    const m = document.querySelector('.context-menu.correc-context');
    if (!m) return null;
    const r = m.getBoundingClientRect();
    return (r.left > -1000 && r.width > 20) ? m : null;
  }

  // 框里的蓝色建议（可能有多条：一句话里多处 diff 各给一条）
  function tipLabels() {
    const m = tipBox();
    return m ? [...m.querySelectorAll('.context-primary label')] : [];
  }

  const plain = s => (s || '').replace(/\s+/g, '').trim();

  // 从多条建议里挑和这条红线最匹配的那条（防止拿到上一处残留的框）
  function pickLabel(labels, correct) {
    if (!labels.length) return null;
    const want = plain(correct);
    if (want) {
      const same = labels.find(l => plain(l.textContent) === want);
      if (same) return same;
      const near = labels.find(l => {
        const t = plain(l.textContent);
        return t && want && (t.indexOf(want) >= 0 || want.indexOf(t) >= 0);
      });
      if (near) return near;
    }
    return labels.length === 1 ? labels[0] : null;
  }

  // 错别字那一块的条目（带「修改」按钮的），只认可见的列表
  function typoItems() {
    return [...document.querySelectorAll('.correc-list .correc-item')]
      .filter(li => visible(li) && (li.querySelector('button.btn.primary')
                                    || li.querySelector('[id^="check_modify_"]')));
  }

  const sleepMs = ms => new Promise(r => setTimeout(r, ms));

  const api = {
    ver: 25,
    _volCount: 0,        // 上次「全展开」时数到的分卷数
    _volAllOpen: false,  // 分卷是否已全部展开（避免每次都收起再展开，白折腾侧栏）

    info() {
      return { url: location.href, title: document.title, tinymce: !!window.tinymce, where: api.where() };
    },

    // 当前在哪种页面：写作页才有 #chapter-title；ready 表示标题框和章节侧栏都渲染好了
    where() {
      const t = document.querySelector('#chapter-title');
      const items = api._chapterItems().length;
      return {
        page: (t || location.hash.indexOf('/chapter/') >= 0) ? 'write' : 'other',
        ready: !!t && items > 0,
        items: items,
        hash: location.hash,
      };
    },

    // 书架卡片：.book_list .item_container[data-book-id]，结构 .item > .cover_area > img
    _bookCards() {
      return [...document.querySelectorAll('.book_list .item_container')];
    },

    // 书名可能在 .book_title / 别的 title 类节点 / 封面 img 的 alt 上，逐层兜底
    _bookTitle(card) {
      if (!card) return '';
      for (const sel of ['.book_title', '.book_name', '[class*="title"]', '[class*="name"]']) {
        const t = txt(card.querySelector(sel));
        if (t && t.length <= 40) return t;
      }
      const img = card.querySelector('.cover_area img') || card.querySelector('img');
      if (img && img.alt) return img.alt.trim();
      const lines = (card.innerText || '').split('\n').map(x => x.trim()).filter(Boolean);
      return lines.length ? lines[0].slice(0, 40) : '';
    },

    // 书架上的全部作品：[{ cbid, title }]，供程序侧下拉候选 + 直接开书
    listBooks() {
      return api._bookCards()
        .map(c => ({ cbid: c.getAttribute('data-book-id') || '', title: api._bookTitle(c) }))
        .filter(b => b.title);
    },

    // ---------- 打开作品 ----------
    // arg: {cbid, title} 或直接书名/CBID 字符串
    // 首选 window.browserWindowAPI.openBook（客户端自己的预加载 API，点封面最终也走这里），
    // 拿不到 API 才退回点封面。
    openBook(arg) {
      const q = (typeof arg === 'object' && arg) ? arg : { title: String(arg || '') };
      const wantId = String(q.cbid || '').trim();
      const wantName = String(q.title || '').trim();
      const cards = api._bookCards();

      let card = null;
      if (wantId)
        card = cards.find(c => c.getAttribute('data-book-id') === wantId) || null;
      if (!card && wantName) {
        card = cards.find(c => {
          const t = api._bookTitle(c);
          return t && (t.indexOf(wantName) >= 0 || wantName.indexOf(t) >= 0);
        }) || null;
      }
      if (!card && !wantId && !wantName)
        card = cards[0] || null;

      const cbid = wantId || (card ? (card.getAttribute('data-book-id') || '') : '');
      const title = card ? api._bookTitle(card) : wantName;

      if (!cbid && !card)
        return { ok: false, why: '书架里没有匹配「' + (wantName || wantId) + '」的作品',
                 titles: cards.map(api._bookTitle).filter(Boolean) };

      // 1) 预加载 API（最稳，不依赖封面是否渲染）
      try {
        const api2 = window.browserWindowAPI;
        if (api2 && typeof api2.openBook === 'function' && cbid) {
          api2.openBook({ CBID: cbid });
          return { ok: true, opened: title || cbid, cbid: cbid, via: 'browserWindowAPI' };
        }
      } catch (e) { /* 落到点击兜底 */ }

      // 2) 兜底：点封面 img → cover_area → 卡片
      if (!card)
        return { ok: false, why: '拿不到作品卡片，且页面没有 browserWindowAPI.openBook',
                 titles: cards.map(api._bookTitle).filter(Boolean) };
      const target = card.querySelector('.cover_area img') || card.querySelector('img')
                  || card.querySelector('.cover_area') || card.querySelector('.item') || card;
      if (!target)
        return { ok: false, why: '作品卡片里找不到可点击的元素',
                 html: (card.outerHTML || '').slice(0, 600),
                 titles: cards.map(api._bookTitle).filter(Boolean) };
      click(target);
      return { ok: true, opened: title, cbid: cbid,
               via: 'click:' + (target.className || target.tagName) };
    },

    // ---------- 侧栏章节列表 ----------
    // 侧栏条目：<a id="chapter_<CCID>" class="item" href="#/chapter/<CBID>/<CCID>">第172章 172西走定荆</a>
    // 注意：列表容器本身 id="chapter_list" 也以 chapter_ 开头，且它包含所有条目文字，必须排掉
    _chapterItems() {
      return [...document.querySelectorAll('[id^="chapter_"]')]
        .filter(el => visible(el) && el.id !== 'chapter_list'
                      && /^第\s*\d+\s*章/.test(txt(el)));
    },

    // 侧栏是 ant-design 虚拟树：真正滚动的是 .ant-tree-list-holder，#chapter_list 自己不滚
    _listScroller() {
      const box = document.querySelector('#chapter_list');
      if (!box) return null;
      const cands = [box, ...box.querySelectorAll('div')];
      return cands.find(e => e.scrollHeight > e.clientHeight + 20) || box;
    },

    // 侧栏是「分卷 → 章节」两级树：分卷是原生 <details id="volume_…">，
    // 卷内章节只有点开 summary 才会被渲染成树节点。第二卷之后的卷默认是收起的，
    // 只滚列表永远找不到里面的章节，所以找章前必须先展开。
    _volumeNodes() {
      return [...document.querySelectorAll('details[id^="volume_"]')]
        .filter(d => visible(d))
        .map(d => {
          const sum = d.querySelector('summary');
          const tEl = d.querySelector('summary .title') || sum;
          const m = (txt(d.querySelector('.chapter-info')) || '').match(/(\d+)\s*章/);
          return { open: d.open, summary: sum, name: txt(tEl).slice(0, 20),
                   count: m ? parseInt(m[1], 10) : -1 };
        });
    },

    // 侧栏列表的总高度（虚拟列表的撑高 div）——分卷展开后它会变，用它判断「重渲染完了」
    _listHeight() {
      const box = document.querySelector('#chapter_list');
      if (!box) return 0;
      let h = 0;
      for (const e of [box, ...box.querySelectorAll('div')]) h = Math.max(h, e.scrollHeight || 0);
      return h;
    },

    // 等到 cond() 为真或超时（Vue 重渲染要几百毫秒，纯微任务等不到）
    async _waitFor(cond, deadlineMs) {
      const t0 = Date.now();
      while (!cond() && Date.now() - t0 < deadlineMs)
        await new Promise(r => setTimeout(r, 60));
      return cond();
    },

    // 把所有分卷都展开（先全摊开，后面按号找就不会漏章）。
    // 要点：卷头只有渲染出来才点得到 —— 先全部收起，卷头会连续挤在列表末尾，
    // 一屏就能读全，再一次性点开；总高度变化即代表重排完成。
    async _expandAllVolumes() {
      const flush = async () => { await null; await null; await null; };
      const all = () => [...document.querySelectorAll('details[id^="volume_"]')];
      const names = [];
      if (api._volAllOpen && all().length && all().length === api._volCount
          && all().every(d => d.open)) return names;       // 已经全开，不折腾

      for (let pass = 0; pass < 3; pass++) {
        // 1) 收起已展开的卷：卷头随即挤到列表末尾
        const opened = all().filter(d => d.open);
        if (opened.length) {
          const h0 = api._listHeight();
          opened.forEach(d => d.querySelector('summary').dispatchEvent(
            new MouseEvent('click', { bubbles: true, cancelable: true, view: window })));
          await flush();
          await api._waitFor(() => api._listHeight() !== h0, 4000);
          await flush();
        }
        // 2) 滚到底，卷头全在视口里
        const box = api._listScroller();
        if (box) {
          box.scrollTop = box.scrollHeight;
          box.dispatchEvent(new Event('scroll', { bubbles: true }));
          await flush();
        }
        // 3) 全部点开
        const closed = api._volumeNodes().filter(v => !v.open && v.summary);
        if (!closed.length) break;
        const h1 = api._listHeight();
        closed.forEach(v => {
          names.push(v.name);
          v.summary.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, view: window }));
        });
        await flush();
        await api._waitFor(() => api._listHeight() !== h1, 4000);   // 卷内章节要等 Vue 重排
        await flush();
        api._volCount = all().length;
        if (all().length && all().every(d => d.open)) break;
      }
      api._volCount = all().length;
      api._volAllOpen = all().length > 0 && all().every(d => d.open);
      return names;
    },

    // 定位侧栏里第 N 章的条目：就地找 → 先把分卷全展开 → 滚虚拟列表逼近。
    // 客户端窗口被挡在后面时页面是 hidden，setTimeout 会被限流到 1 秒，
    // 所以滚动/重排只在必要时等，其余用「微任务」推进（Vue 刷新走 Promise 微任务）。
    async _locateChapter(no) {
      const re = new RegExp('^第\\s*' + no + '\\s*章');
      const items = () => api._chapterItems();
      const hit = () => items().find(el => re.test(txt(el)));
      const flush = async () => { await null; await null; await null; };

      let target = hit();
      if (target) return { target: target, expanded: [] };

      const expanded = await api._expandAllVolumes();  // 先全部展开，避免漏掉收起分卷里的章
      target = hit();
      if (target) return { target: target, expanded: expanded };

      const scroll5 = async () => {
        const list0 = items();
        if (!list0.length) return false;
        const box = api._listScroller();
        if (!box) return false;
        let rowH = 30;
        if (list0.length >= 2) {
          const a = list0[0].getBoundingClientRect().top;
          const b = list0[list0.length - 1].getBoundingClientRect().top;
          const h = Math.abs(b - a) / (list0.length - 1);
          if (h > 8 && h < 200) rowH = h;
        }
        // 相对锚点：用窗口里第一条章节号做参照，比 (no-1)×行高 准得多（中间还夹着卷头行）
        const m0 = txt(list0[0]).match(/第\s*(\d+)\s*章/);
        const base = m0 ? parseInt(m0[1], 10) : 1;
        const maxTop = Math.max(0, box.scrollHeight - box.clientHeight);
        box.scrollTop = Math.min(maxTop, Math.max(0,
          box.scrollTop + (no - base) * rowH - box.clientHeight * 0.35));
        box.dispatchEvent(new Event('scroll', { bubbles: true }));
        await flush();
        target = hit();
        for (let i = 0; !target && i < 30; i++) {       // 没中按「窗口首章号」判方向小步逼近
          const list = items();
          if (!list.length) break;
          const m = txt(list[0]).match(/第\s*(\d+)\s*章/);
          const firstNo = m ? parseInt(m[1], 10) : 0;
          const dir = (firstNo && firstNo > no) ? -1 : 1;
          const before = box.scrollTop;
          box.scrollTop = before + dir * rowH * 8;
          box.dispatchEvent(new Event('scroll', { bubbles: true }));
          await flush();
          if (box.scrollTop === before) break;
          target = hit();
        }
        return !!target;
      };

      if (await scroll5()) return { target: target, expanded: expanded };

      // 还没中（列表太长、卷头没读全）→ 再展开一轮重试
      expanded = expanded.concat(await api._expandAllVolumes());
      if (hit() || await scroll5()) return { target: target, expanded: expanded };
      return { target: null, expanded: expanded };
    },

    // ---------- 章节索引：直接读 Pinia 的 chapterStore，绕开侧栏 DOM ----------
    // 侧栏是「分卷 → 章节」两级虚拟树，收起的分卷、没渲染的章节都不在 DOM 里；
    // 而 chapterStore.ChapterState.list 是全量目录（分卷数组，每章带 CCID/CBID）。
    // 有了 CCID 就能直接改路由跳章，和点侧栏条目等效，但完全不受展开/滚动影响。
    _chapterIndex() {
      try {
        const app = document.querySelector('#app');
        const pinia = app && app.__vue_app__ && app.__vue_app__.config.globalProperties.$pinia;
        const st = pinia && pinia.state && pinia.state.value;
        const cs = st && st.chapterStore && st.chapterStore.ChapterState;
        const list = cs && cs.list;
        if (!Array.isArray(list)) return null;
        const out = [];
        list.forEach(v => (v.chapters || []).forEach(c => {
          const t = String(c.chaptertitle || '').trim();
          const m = t.match(/^第\s*(\d+)\s*章/);
          if (!m) return;
          out.push({ no: parseInt(m[1], 10), title: t,
                     ccid: String(c.CCID || ''), cbid: String(c.CBID || v.CBID || ''),
                     words: parseInt(c.actualwords || c.originalwords || 0, 10) || 0,
                     volume: String(v.volumename || '') });
        }));
        return out.length ? out : null;
      } catch (e) { return null; }
    },

    // 按章节号改路由跳章（侧栏条目的 href 就是 #/chapter/<CBID>/<CCID>）
    async _jumpToChapter(no) {
      const idx = api._chapterIndex();
      if (!idx) return { ok: false, why: '读不到章节索引（chapterStore 结构可能变了）' };
      const c = idx.find(x => x.no === no);
      if (!c) {
        const nos = idx.map(x => x.no);
        return { ok: false, definitive: true,
                 why: '书稿目录里没有第 ' + no + ' 章（目录共 ' + idx.length + ' 章，第 '
                      + Math.min.apply(null, nos) + '~' + Math.max.apply(null, nos) + ' 章）',
                 count: idx.length };
      }
      if (!c.ccid || !c.cbid) return { ok: false, why: '第 ' + no + ' 章缺 CCID/CBID' };
      const hash = '#/chapter/' + c.cbid + '/' + c.ccid;
      if (location.hash !== hash) location.hash = hash;
      await null; await null; await null;             // 路由切过去；正文由外层 draftReady 轮询确认
      return { ok: true, no: no, via: 'route',
               text: c.title + (c.words ? ' ' + c.words : ''),
               ccid: c.ccid, volume: c.volume, words: c.words };
    },

    // 「新建章」插在当前选中章之后，所以先点中上一章。
    // 优先点侧栏条目（客户端原本的行为）；条目没渲染出来才改路由跳。
    async selectChapter(no) {
      const tEl = document.querySelector('#chapter-title');
      const before = tEl ? tEl.value : '';
      const re = new RegExp('^第\\s*' + no + '\\s*章');
      const direct = api._chapterItems().find(el => re.test(txt(el)));
      if (direct) {
        click(direct);
        return { ok: true, no: no, text: txt(direct).slice(0, 40), before: before, via: 'click' };
      }

      const jump = await api._jumpToChapter(no);
      if (jump.ok) return { ok: true, no: no, text: jump.text, before: before, via: 'route' };
      if (jump.definitive) return { ok: false, why: jump.why };

      // 索引不可用 → 退回「全展开 + 滚列表」找条目
      const r = await api._locateChapter(no);
      if (!r.target) {
        const items = api._chapterItems();
        return { ok: false, scrolled: true, expanded: r.expanded,
                 why: jump.why + '（侧栏里也没有）', count: items.length,
                 sample: items.slice(0, 6).map(txt) };
      }
      click(r.target);
      return { ok: true, no: no, text: txt(r.target).slice(0, 40), before: before,
               expanded: r.expanded, via: 'click' };
    },

    // 书稿里第 N 章的状态（以 chapterStore 的全量目录为准，收起的分卷也算）。
    // 注意：客户端里会有「第243章」这种只有编号、0 字的空占位章（chapters 里先建了壳）。
    // 这种章不能算「已存在」——否则上传时会被当成已上传而跳过，那一章永远是空的。
    // 所以判定口径是「正文非空（actualwords > 0）才算存在」；标题只有「第N章」也一并报给外层。
    async hasChapter(no) {
      const bare = t => /^第\s*\d+\s*章\s*$/.test(String(t || '').trim());
      const idx = api._chapterIndex();
      if (idx) {
        const c = idx.find(x => x.no === no);
        if (!c) return { has: false, empty: false, via: 'index', count: idx.length };
        const words = c.words || 0;
        return { has: words > 0, empty: !(words > 0), words: words,
                 title: c.title, titleBare: bare(c.title), via: 'index', count: idx.length };
      }
      // 索引读不到 → 退回侧栏 DOM：条目文字只有「第N章 」（后面没有标题和字数）就是空占位章
      const re = new RegExp('^第\\s*' + no + '\\s*章');
      const find = () => api._chapterItems().filter(el => re.test(txt(el)));
      let hits = find();
      if (!hits.length) {
        const opened = await api._expandAllVolumes();
        return { has: false, empty: false, expanded: opened, via: 'dom' };
      }
      const t = txt(hits[0]).trim();
      const empty = /^第\s*\d+\s*章\s*$/.test(t);
      return { has: !empty, empty: empty, title: t, words: 0, titleBare: empty, via: 'dom' };
    },

    // 草稿校对用：切到第 N 章（跳路由；索引不可用才退回点侧栏）
    async pickChapter(no) {
      const jump = await api._jumpToChapter(no);
      if (jump.ok) return jump;
      if (jump.definitive) return jump;                // 目录里确实没这一章，别白折腾侧栏

      const r = await api._locateChapter(no);
      if (!r.target) {
        const items = api._chapterItems();
        const vols = api._volumeNodes().map(v => v.name + (v.open ? '(已展开)' : '(收起)'));
        return { ok: false, why: jump.why + '；侧栏里也没有第 ' + no + ' 章',
                 expanded: r.expanded, volumes: vols, count: items.length,
                 sample: items.slice(0, 5).map(txt) };
      }
      click(r.target);
      await null; await null; await null;             // 等路由切过去，正文由外层轮询确认
      return { ok: true, no: no, text: txt(r.target).slice(0, 40),
               expanded: r.expanded, via: 'click' };
    },


    // 草稿章节是否已经加载好：标题是「第N章…」且正文非空
    // ready / ok 两个字段都给：早期 C++ 侧读的是 ready，只返回 ok 会让轮询永远为假、白等到超时
    draftReady(no) {
      const tEl = document.querySelector('#chapter-title');
      const title = (tEl ? tEl.value : '').trim();
      const okTitle = new RegExp('^第\\s*' + no + '\\s*章').test(title);
      const chars = api.bodyChars();
      const ready = okTitle && chars > 0;
      return { ok: ready, ready: ready, title: title, chars: chars, no: no,
               active: txt(document.querySelector('[id^="chapter_"].router-link-active')) };
    },

    // 当前选中的是哪一章（标题输入框 + 侧栏高亮条）
    currentTitle() {
      const tEl = document.querySelector('#chapter-title');
      const active = document.querySelector('[id^="chapter_"].router-link-active')
                  || document.querySelector('[id^="chapter_"].active')
                  || document.querySelector('[id^="chapter_"].current');
      return {
        title: tEl ? tEl.value : '',
        active: txt(active).slice(0, 40),
      };
    },

    // ---------- 新建章 ----------
    clickNewChapter() {
      const el = findByText(/^新建(章|章节)$/);
      if (!el) return { ok: false, why: '没找到「新建章」按钮', buttons: visibleTexts() };
      click(el);
      return { ok: true, text: txt(el) };
    },

    // ---------- 标题 / 正文 ----------
    editables() {
      const out = [];
      document.querySelectorAll('[contenteditable="true"],[contenteditable=""],textarea,input[type=text]').forEach(el => {
        if (!visible(el)) return;
        const r = el.getBoundingClientRect();
        out.push({
          tag: el.tagName, cls: (el.className || '').toString().slice(0, 50), id: el.id,
          top: Math.round(r.top), h: Math.round(r.height), w: Math.round(r.width),
          text: txt(el).slice(0, 40),
        });
      });
      return out;
    },

    setTitle(title) {
      // 客户端里章节标题是 <textarea id="chapter-title">（旁边还有个 #chapter-title-hightlight 高亮层）
      const el = document.querySelector('.side-editor-box #chapter-title')
              || document.querySelector('#chapter-title');
      if (!el) return { ok: false, why: '找不到 #chapter-title', editables: api.editables() };

      el.focus();
      el.click();
      const proto = el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
      Object.getOwnPropertyDescriptor(proto, 'value').set.call(el, title);
      // 客户端是 Vue：v-model 只听 input，部分地方还听 compositionend
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      try {
        el.dispatchEvent(new CompositionEvent('compositionend', { bubbles: true, data: title }));
      } catch (e) {
        el.dispatchEvent(new Event('compositionend', { bubbles: true }));
      }
      el.dispatchEvent(new KeyboardEvent('keydown', { bubbles: true, key: 'End' }));
      el.dispatchEvent(new KeyboardEvent('keyup', { bubbles: true, key: 'End' }));

      // 关键：失焦。标题框的提交多写在 blur 里，不失焦的话
      // 客户端还认为标题在编辑中，随后写正文会被当成上一章的内容。
      el.dispatchEvent(new FocusEvent('blur'));
      el.blur();
      return { ok: el.value === title, got: el.value, via: '#chapter-title' };
    },

    // 把焦点挪进正文区（模拟真人点一下「请输入正文」）。
    // 不做这一步，tinymce.activeEditor 可能还挂着上一章的编辑器，
    // setContent 写进去页面上根本看不见 —— 表现为「正文上传失败」。
    focusBody() {
      const ed = editor();
      const dom = biggestEditable();
      // 编辑器实例不在文档里（上一章留下的僵尸）就改走 DOM
      if (ed && (editorLive(ed) || !dom)) {
        const bodyFocused = () => {
          try {
            const b = ed.getBody && ed.getBody();
            if (!b) return false;
            const root = b.ownerDocument;
            return document.activeElement === b
                   || (root && root.activeElement === b)
                   || document.activeElement === (ed.getElement && ed.getElement());
          } catch (e) { return false; }
        };

        try { ed.focus(); } catch (e) { }
        try {
          const b = ed.getBody && ed.getBody();
          if (b) {
            // 再补一次原生 focus：tinymce.focus() 在某些版本里只设置内部状态
            if (b.focus) b.focus({ preventScroll: true });
            try { ed.selection.select(b, true); ed.selection.collapse(false); } catch (e) { }
          }
        } catch (e) { }
        try { ed.nodeChanged && ed.nodeChanged(); } catch (e) { }

        let focused = bodyFocused();
        if (!focused) {
          // 还没进去就真的点一下正文区（跟真人点「请输入正文」一样）
          try {
            const b = ed.getBody && ed.getBody();
            if (b) click(b);
          } catch (e) { }
          focused = bodyFocused();
        }
        return { ok: true, via: 'tinymce', id: ed.id || '', focused: focused, chars: api.bodyChars() };
      }

      const el = dom;
      if (!el) return { ok: false, why: '页面上找不到正文编辑区', editables: api.editables() };
      el.focus();
      try { el.click(); } catch (e) { }
      return { ok: true, via: 'contenteditable', focused: document.activeElement === el,
               chars: api.bodyChars() };
    },

    // 正文区当前状态（校验用）：哪个编辑器在收内容、多少字、标题是什么
    bodyState() {
      const list = editorList();
      const ed = editor();
      const tEl = document.querySelector('#chapter-title');
      let focused = false;
      try {
        const b = ed && ed.getBody && ed.getBody();
        focused = !!b && (document.activeElement === b
                          || (b.ownerDocument && b.ownerDocument.activeElement === b));
      } catch (e) { }
      return {
        count: list.length,
        ids: list.map(e => e.id || '?'),
        editor: ed ? (ed.id || '?') : '',
        focused: focused,
        chars: api.bodyChars(),
        title: tEl ? tEl.value : '',
      };
    },

    setBody(html) {
      const ed = editor();
      const dom = biggestEditable();
      // 编辑器实例已不在文档里（上一章的僵尸）就直接改 DOM，别往看不见的地方写
      if (ed && ed.setContent && (editorLive(ed) || !dom)) {
        // 先聚焦：一是让标题框真正失焦提交，二是把 activeEditor 认到这个实例上
        try { ed.focus(); } catch (e) { }
        ed.setContent(html);
        // 光标落到末尾，跟真人打完字一样，客户端才认这是「正在编辑」的章
        try {
          const b = ed.getBody();
          ed.selection.select(b, true);
          ed.selection.collapse(false);
          ed.nodeChanged && ed.nodeChanged();
        } catch (e) { }
        try { ed.undoManager && ed.undoManager.add(); } catch (e) { }
        try { ed.fire('input'); ed.fire('change'); } catch (e) { }
        // 补一发原生 input：客户端的 v-model / 自动保存监听的是 DOM 事件
        try {
          const b = ed.getBody && ed.getBody();
          if (b) {
            let ev = null;
            try { ev = new InputEvent('input', { bubbles: true, inputType: 'insertText' }); }
            catch (e) { ev = new Event('input', { bubbles: true }); }
            b.dispatchEvent(ev);
          }
        } catch (e) { }
        return { ok: true, via: 'tinymce', id: ed.id || '', chars: api.bodyChars() };
      }
      // 退路：页面上面积最大的 contenteditable
      const el = dom;
      if (!el) return { ok: false, why: '找不到正文编辑区', editables: api.editables() };
      el.focus();
      try { el.click(); } catch (e) { }
      document.execCommand('selectAll', false, null);
      document.execCommand('insertHTML', false, html);
      let n = txt(el).replace(/\s/g, '').length;
      if (n < 10) {
        // execCommand 在没真正获得焦点时是空操作，直接写 innerHTML 兜底
        el.innerHTML = html;
        try { el.dispatchEvent(new Event('input', { bubbles: true })); } catch (e) { }
        n = txt(el).replace(/\s/g, '').length;
      }
      return { ok: n > 0, via: 'contenteditable', chars: n };
    },

    bodyChars() {
      const ed = editor();
      // 优先看「页面上真能看见」的那个编辑区，僵尸实例的字符数不算数
      const dom = biggestEditable();
      if (dom) return txt(dom).replace(/\s/g, '').length;
      if (ed && ed.getContent)
        return (ed.getContent({ format: 'text' }) || '').replace(/\s/g, '').length;
      return 0;
    },

    // ---------- 校对 ----------
    async openProofread() {
      const dsVisible = () => [...document.querySelectorAll('button.check.dscheck')].filter(visible);
      const entry = () => findByText(/^校对$/, { maxWidth: 140 });

      // 面板已经展开：绝不能再点「校对」，那是开关，再点一下就收起来了
      if (dsVisible().length) {
        const ks = checkBlocks().map(b => ({ kind: b.kind, state: blockState(b) }));
        const stale = ks.some(k => k.state === 'done' || k.state === 'busy');
        if (!stale)
          return { ok: true, already: true, blocks: ks.length };
        // 面板里还挂着上一章的检测结果：点一下收起、再点开，拿回干净的「未检测」
        const close = entry();
        if (!close)
          return { ok: true, already: true, blocks: ks.length, stale: true };
        click(close);
        await sleepMs(600);
        const reopen = entry();
        if (reopen) {
          click(reopen);
          await sleepMs(600);
        }
        return { ok: true, reset: true, blocks: dsVisible().length };
      }

      const el = entry();
      if (!el) return { ok: false, why: '没找到右栏「校对」入口', buttons: visibleTexts() };
      click(el);
      return { ok: true };
    },

    // 面板上有几块检测（错别字 / 病句），各自的标题、按钮文案和当前状态
    checkKinds() {
      return checkBlocks().map(b => ({
        kind: b.kind, label: kindLabel(b.kind),
        header: b.header, button: b.btnText, state: blockState(b),
      }));
    },

    // kind 省略 = 第一块（错别字），'sentence' = 病句
    checkState(kind) {
      const b = findBlock(kind);
      return {
        kind: b ? b.kind : (kind || ''),
        label: kindLabel(b ? b.kind : kind),
        found: !!(b && b.btn),
        header: b ? b.header : '',
        sectionText: blockState(b) === 'unknown' ? blockText(b).slice(0, 200) : '',
        buttonText: b ? b.btnText : '',
        state: blockState(b),
        tips: [...document.querySelectorAll('.correct-tips')].map(txt).slice(0, 4),
        result: [...document.querySelectorAll('.check-result')].map(txt).join(' | ').slice(0, 300),
        suggestions: document.querySelectorAll('.correc-list .correc-item').length,
        kinds: checkBlocks().map(x => ({ kind: x.kind, header: x.header, button: x.btnText,
                                         state: blockState(x) })),
      };
    },

    clickCheck(kind) {
      const b = findBlock(kind);
      if (!b || !b.btn) {
        return { ok: false, why: '没找到「' + kindLabel(kind) + '」的检测按钮（校对面板没展开？）',
                 kinds: checkBlocks().map(x => ({ kind: x.kind, header: x.header })) };
      }
      if (/检测中/.test(txt(b.btn)))
        return { ok: true, already: true, kind: b.kind, header: b.header };
      try {
        const r = b.btn.getBoundingClientRect();
        if (r.top < 60 || r.bottom > (window.innerHeight - 40))
          b.btn.scrollIntoView({ block: 'center' });     // 病句那块可能在面板下方
      } catch (err) { }
      click(b.btn);
      return { ok: true, kind: b.kind, header: b.header, text: b.btnText };
    },

    // 还剩多少活：正文里的红线（错别字 data-dsidx / 病句 data-grammaridx），外加列表里的「修改」条目
    fixInfo() {
      const ws = redWords();
      const marks = ws.filter(w => w.getAttribute('data-grammaridx')).length;
      const typos = ws.filter(w => w.getAttribute('data-dsidx')).length;
      return {
        marks: marks,
        typos: typos,
        red: ws.length,
        total: ws.length + typoItems().length,
        first: ws[0] ? (ws[0].getAttribute('data-origin-word') || '').slice(0, 30) : '',
      };
    },

    // 改一条红线（错别字/病句同一套）：滚到红线 → mouseover 唤出修改框 → 点框里的蓝字
    // 返回 { done, left, applied, kind, origin, sug, via }；卡住时 stuck=true + why
    async fixOne() {
      const list = redWords();
      if (!list.length) return { done: true, left: 0, typos: typoItems().length };

      const w      = list[0];
      const before = list.length;
      const origin = (w.getAttribute('data-origin-word') || '').trim();
      const right  = (w.getAttribute('data-correct-word') || '').trim();
      const kind   = w.getAttribute('data-grammaridx') ? 'sentence' : 'typo';

      try { w.scrollIntoView({ block: 'center' }); } catch (e) { }
      await sleepMs(280);

      // 唤出修改框：mouseover 打在这条红线上（病句是里面的 .grammar-mark，错别字直接是 el-word）
      const hover = () => {
        const inner = w.querySelector('.grammar-mark') || w;
        inner.dispatchEvent(new MouseEvent('mouseover', { bubbles: true, cancelable: true, view: window }));
      };

      const readTip = () => {
        const labs = tipLabels();
        const lab = pickLabel(labs, right);
        return { n: labs.length, label: lab, sug: lab ? (lab.textContent || '').trim() : '' };
      };

      hover();
      await sleepMs(320);
      let s = readTip();
      if (!s.label) {                     // 第一下可能被上一次的 hover 状态吞掉
        hover();
        await sleepMs(320);
        s = readTip();
      }

      if (s.label && s.sug) {
        s.label.click();
        await sleepMs(700);
        let left = redWords().length;
        if (left < before)
          return { done: left === 0, left: left, applied: true, via: 'label',
                   kind: kind, origin: origin, sug: s.sug };
        // 点了没生效：重开框再点一次
        hover();
        await sleepMs(320);
        const again = pickLabel(tipLabels(), right);
        if (again) { again.click(); await sleepMs(700); }
        left = redWords().length;
        if (left < before)
          return { done: left === 0, left: left, applied: true, via: 'label-retry',
                   kind: kind, origin: origin, sug: s.sug };
      }

      // 修改框没给建议：按 data-correct-word 直接替换（退路，正文照样是改对的）
      if (right) {
        w.replaceWith(document.createTextNode(right));
        await sleepMs(150);
        const left = redWords().length;
        if (left < before)
          return { done: left === 0, left: left, applied: true, via: 'direct',
                   kind: kind, origin: origin, sug: right };
      }

      // 实在没法改：点「忽略」把这条红线去掉，别让整章卡死
      const menu = tipBox();
      const ign = menu ? [...menu.querySelectorAll('label')]
                           .find(l => /^忽略$/.test((l.textContent || '').trim())) : null;
      if (ign) {
        ign.click();
        await sleepMs(500);
        const left = redWords().length;
        if (left < before)
          return { done: left === 0, left: left, ignored: true, kind: kind, origin: origin };
      }

      return {
        done: false, left: before, stuck: true, kind: kind, origin: origin,
        why: (tipBox() ? (tipLabels().length ? '修改框有建议但点了没反应' : '修改框里没有蓝色建议')
                       : '红线没响应 hover，修改框没弹出'),
      };
    },

    // 点第一条建议的「修改」（错别字那块才有这个按钮），返回剩余条数
    applyOne() {
      const items = typoItems();
      if (!items.length) return { done: true, remaining: 0 };
      const li = items[0];
      const before = txt(li).slice(0, 90);
      const btn = li.querySelector('button.btn.primary') || li.querySelector('[id^="check_modify_"]');
      if (!btn) return { done: false, remaining: items.length, why: '条目里没有「修改」按钮' };
      click(btn);
      return { done: false, remaining: typoItems().length, item: before };
    },

    applyAll() {
      const b = document.querySelector('button.modify-all');
      if (!b) return { ok: false, why: '没有「全部修改」按钮' };
      click(b);
      return { ok: true };
    },

    // ---------- 收尾 ----------
    finish(mode) {
      if (mode === 'none') return { ok: true, skipped: true };
      const re = mode === 'publish' ? /^发布$/ : /^保存$/;
      const el = findByText(re, { maxWidth: 200 });
      if (!el) return { ok: false, why: '没找到「' + (mode === 'publish' ? '发布' : '保存') + '」按钮', buttons: visibleTexts() };
      click(el);
      return { ok: true, text: txt(el) };
    },

    confirmPublish() {
      const el = findByText(/^(确定|确认|立即发布|确认发布)$/, { maxWidth: 220 });
      if (!el) return { ok: false };
      click(el);
      return { ok: true, text: txt(el) };
    },

    // ---------- 定时发布 ----------
    // 流程（客户端 5.24.0 实测）：
    //   左侧「草稿」tab（button.action_text2）→ 点开某章 → 右上角 #btn-group-chapter 的「发布」
    //   → 弹窗「确认章节信息」→ 打开「定时发布」开关 → 日期/时间 → 点「定时发布」
    // 要点：日期、时间都必须在弹出的面板里点选才生效；
    //   直接往输入框里写 value 只会改显示，组件内部状态不变，提交时还是旧时间（用户实测）。

    // 侧栏「草稿 / 已发布」两个 tab
    draftTab() {
      const tabs = [...document.querySelectorAll('button.action_text2')];
      const d = tabs.find(b => txt(b) === '草稿');
      const p = tabs.find(b => txt(b) === '已发布');
      return { found: !!d,
               active: d ? d.getAttribute('data-active') === 'true' : false,
               publishedActive: p ? p.getAttribute('data-active') === 'true' : false };
    },

    // 切到「草稿」列表：发布前必须，否则侧栏里是已发布章
    async ensureDraftTab() {
      const st = api.draftTab();
      if (!st.found) return { ok: false, why: '侧栏里没有「草稿」标签' };
      if (st.active) return { ok: true, active: true, already: true };
      const d = [...document.querySelectorAll('button.action_text2')].find(b => txt(b) === '草稿');
      click(d);
      await api._waitFor(() => api.draftTab().active, 3000);
      const now = api.draftTab();
      return { ok: now.active, active: now.active, already: false };
    },

    // 全量目录 + 状态（不依赖侧栏渲染）。
    // status：1=草稿 2=已发布 5=已定时发布 7=回收站
    chapterList() {
      try {
        const app = document.querySelector('#app');
        const pinia = app && app.__vue_app__ && app.__vue_app__.config.globalProperties.$pinia;
        const st = pinia && pinia.state && pinia.state.value;
        const cs = st && st.chapterStore && st.chapterStore.ChapterState;
        const list = cs && cs.list;
        if (!Array.isArray(list)) return null;
        const out = [];
        list.forEach(v => (v.chapters || []).forEach(c => {
          const t = String(c.chaptertitle || '').trim();
          const m = t.match(/^第\s*(\d+)\s*章/);
          if (!m) return;
          out.push({
            no: parseInt(m[1], 10), title: t,
            status: String(c.status === undefined ? '' : c.status),
            words: parseInt(c.actualwords || c.originalwords || 0, 10) || 0,
            ccid: String(c.CCID || ''), volume: String(v.volumename || ''),
          });
        }));
        out.sort((a, b) => a.no - b.no);
        return out.length ? out : null;
      } catch (e) { return null; }
    },

    chapterStatus(no) {
      const list = api.chapterList() || [];
      const c = list.find(x => x.no === no);
      return c ? { found: true, status: c.status, title: c.title, words: c.words }
               : { found: false };
    },

    // 写作页右上角的「发布」（#btn-group-chapter 里的第一个「发布」）。
    // 已发布章这里是「修改/删除/重置」，所以找不到就说明当前章不是草稿。
    _publishBtn() {
      const box = document.querySelector('#btn-group-chapter') || document;
      return [...box.querySelectorAll('button')].find(b => visible(b) && txt(b) === '发布') || null;
    },

    openPublishDialog() {
      const b = api._publishBtn();
      if (!b) return { ok: false, why: '右上角没有「发布」按钮（这一章可能不是草稿）',
                       buttons: visibleTexts() };
      if (b.disabled || b.classList.contains('ant-btn-disabled'))
        return { ok: false, why: '「发布」按钮是灰的（章节正在保存，或已不是草稿）' };
      click(b);
      return { ok: true, text: txt(b) };
    },

    _publishModal() {
      const wraps = [...document.querySelectorAll('.ant-modal-wrap')].filter(visible);
      return wraps.find(w => /确认章节信息/.test(txt(w)))
             || wraps.find(w => w.querySelector('.publish-item-time')) || null;
    },

    publishDialog() {
      const m = api._publishModal();
      return { open: !!m, title: m ? txt(m.querySelector('.ant-modal-title')) : '' };
    },

    _schedSwitch() { return document.querySelector('.publish-item-time input[ui="switch"]'); },

    _schedInputs() {
      const box = document.querySelector('.publish-item-time');
      if (!box) return {};
      const ins = [...box.querySelectorAll('.publish_time .ant-picker input')];
      return { date: ins[0] || null, time: ins[1] || null };
    },

    // 面板：日期/时间共用 .ant-picker-dropdown，隐藏时带 ant-picker-dropdown-hidden
    _pickerDropdown(kind) {
      const list = [...document.querySelectorAll('.ant-picker-dropdown')]
        .filter(d => visible(d) && !/ant-picker-dropdown-hidden/.test(d.className));
      if (!kind) return list[0] || null;
      const want = kind === 'time' ? '.ant-picker-time-panel' : '.ant-picker-date-panel';
      return list.find(d => d.querySelector(want)) || list[0] || null;
    },

    // 打开某个输入框的面板。已经开着就直接用（上一次尝试可能没关干净）
    async _openPicker(inp, kind) {
      let dd = api._pickerDropdown(kind);
      if (dd) return dd;
      inp.focus();
      try { inp.click(); } catch (e) { }
      for (let i = 0; i < 6 && !dd; i++) {
        await sleepMs(120);
        dd = api._pickerDropdown(kind);
      }
      return dd;
    },

    // 日期：必须是「弹出日历 → 点日期格子」
    async setScheduleDate(text) {
      const ins = api._schedInputs();
      const inp = ins.date;
      if (!inp) return { ok: false, why: '找不到日期输入框' };
      const m = String(text || '').match(/^(\d{4})-(\d{2})-(\d{2})$/);
      if (!m) return { ok: false, why: '日期格式必须是 YYYY-MM-DD（收到 ' + text + '）' };

      const dd = await api._openPicker(inp, 'date');
      if (!dd) return { ok: false, why: '日历面板没弹出来' };

      // 翻月：面板头写着「2026年 11月」
      const want = new Date(parseInt(m[1], 10), parseInt(m[2], 10) - 1, parseInt(m[3], 10));
      const tgt = want.getFullYear() * 12 + want.getMonth();
      let cur = -1;
      for (let i = 0; i < 72; i++) {
        const view = dd.querySelector('.ant-picker-header-view');
        const vm = (txt(view) || '').match(/(\d{4})\D+(\d{1,2})/);
        if (!vm) return { ok: false, why: '读不到日历当前月份（面板结构变了？）' };
        cur = parseInt(vm[1], 10) * 12 + (parseInt(vm[2], 10) - 1);
        if (cur === tgt) break;
        // 超过两个月就点「上/下一年」，否则逐月点，避免来回翻
        const far = Math.abs(cur - tgt) > 2;
        const sel = cur < tgt
          ? (far ? '.ant-picker-header-super-next-btn' : '.ant-picker-header-next-btn')
          : (far ? '.ant-picker-header-super-prev-btn' : '.ant-picker-header-prev-btn');
        const btn = dd.querySelector(sel);
        if (!btn) return { ok: false, why: '日历没有翻页按钮' };
        click(btn);
        await sleepMs(90);
      }
      if (cur !== tgt) return { ok: false, why: '翻不到 ' + text + ' 所在的月份' };

      const same = [...dd.querySelectorAll('td[title]')]
        .filter(td => td.getAttribute('title') === text);
      // 月视图里会顺带渲染上/下月的日子（同一个 title 可能出现两次），优先取当月的
      const cell = same.find(td => td.classList.contains('ant-picker-cell-in-view')) || same[0];
      if (!cell) return { ok: false, why: '日历里没有 ' + text + ' 这一天' };
      click(cell.querySelector('.ant-picker-cell-inner') || cell);
      await sleepMs(200);
      const got = (inp.value || '').trim();
      return { ok: got === text, via: 'calendar', got: got,
               why: got === text ? '' : '点了日期但值变成「' + got + '」' };
    },

    // 时间：必须在面板里选小时、分钟，然后点「确定」
    async setScheduleTime(text) {
      const ins = api._schedInputs();
      const inp = ins.time;
      if (!inp) return { ok: false, why: '找不到时间输入框' };
      const m = String(text || '').match(/^(\d{1,2}):(\d{2})$/);
      if (!m) return { ok: false, why: '时间格式必须是 HH:mm（收到 ' + text + '）' };

      const dd = await api._openPicker(inp, 'time');
      if (!dd) return { ok: false, why: '时间面板没弹出来' };

      const cols = [...dd.querySelectorAll('.ant-picker-time-panel-column')];
      if (cols.length < 2) return { ok: false, why: '时间面板列数不对（' + cols.length + '）' };

      const pick = (col, val) => {
        const cells = [...col.querySelectorAll('.ant-picker-time-panel-cell')];
        const want = String(parseInt(val, 10));
        const hit = cells.find(c => {
          const t = txt(c.querySelector('.ant-picker-time-panel-cell-inner') || c);
          return t === want || t === val;
        });
        if (!hit) return false;
        click(hit.querySelector('.ant-picker-time-panel-cell-inner') || hit);
        return true;
      };

      if (!pick(cols[0], m[1])) return { ok: false, why: '时间面板里没有 ' + m[1] + ' 点' };
      await sleepMs(120);
      if (!pick(cols[1], m[2])) return { ok: false, why: '时间面板里没有 ' + m[2] + ' 分' };
      await sleepMs(120);

      // 面板底部的「确定」——不点它时间和日期都不生效
      const ok = dd.querySelector('.ant-picker-ok button')
              || [...dd.querySelectorAll('button')].find(b => /^(确定|OK)$/.test(txt(b)));
      if (!ok) return { ok: false, why: '时间面板没有「确定」按钮' };
      click(ok);
      await sleepMs(220);

      const got = (inp.value || '').trim();
      return { ok: got === text, via: 'panel+ok', got: got,
               why: got === text ? '' : '确定后时间变成「' + got + '」' };
    },

    // 开关 + 日期 + 时间一次做完；返回值交给 C++ 复核（绝不带着未确认的时间提交）
    async prepareSchedule(dateStr, timeStr) {
      const sw = api._schedSwitch();
      if (!sw) return { ok: false, why: '发布弹窗里没有「定时发布」开关' };
      if (!sw.checked) {
        sw.click();
        await sleepMs(150);
      }
      if (!api._schedSwitch().checked) return { ok: false, why: '「定时发布」开关没打开' };

      const rd = await api.setScheduleDate(dateStr);
      if (!rd.ok) return { ok: false, step: 'date', why: rd.why, date: rd.got };

      const rt = await api.setScheduleTime(timeStr);
      if (!rt.ok) return { ok: false, step: 'time', why: rt.why, time: rt.got };

      const ins = api._schedInputs();
      return { ok: true, checked: !!api._schedSwitch().checked,
               date: ins.date ? ins.date.value : '', time: ins.time ? ins.time.value : '',
               via: rd.via + ' / ' + rt.via };
    },

    submitSchedule() {
      const m = api._publishModal();
      if (!m) return { ok: false, why: '发布弹窗不在了' };
      const btns = [...m.querySelectorAll('button')].filter(visible);
      const hit = btns.find(b => /^(定时发布|确认发布|立即发布)$/.test(txt(b)));
      if (!hit) return { ok: false, why: '弹窗里没有「定时发布」按钮',
                         buttons: btns.map(txt).filter(Boolean).slice(0, 12) };
      if (hit.disabled || hit.classList.contains('ant-btn-disabled'))
        return { ok: false, why: '「' + txt(hit) + '」按钮是灰的（日期时间可能还没选好）' };
      click(hit);
      return { ok: true, text: txt(hit) };
    },

    publishState() {
      const m = api._publishModal();
      const msgs = [...document.querySelectorAll('.ant-message-notice-content,.ant-notification-notice-message')]
        .filter(visible).map(txt).filter(Boolean);
      return { open: !!m, msgs: msgs.slice(-4) };
    },

    // 个别情况下提交后会多弹一个「确定」确认框，点掉它
    clickConfirmIfAsked() {
      const cand = [...document.querySelectorAll('.ant-modal-confirm-btns .ant-btn-primary, .ant-modal-wrap button')]
        .filter(visible)
        .find(b => /^(确定|确认|继续|知道了)$/.test(txt(b)) && !b.closest('.confirm-dialog'));
      if (!cand) return { ok: false };
      click(cand);
      return { ok: true, text: txt(cand) };
    },

    closePublishDialog() {
      const m = api._publishModal();
      if (!m) return { ok: true, already: true };
      const x = m.querySelector('.ant-modal-close');
      if (x) { click(x); return { ok: true }; }
      return { ok: false, why: '弹窗没有关闭按钮' };
    },

    // 调试：把页面结构抓下来，方便对选择器
    dump() {
      return {
        url: location.href,
        title: document.title,
        buttons: visibleTexts(),
        editables: api.editables(),
        checkState: api.checkState(),
        html: document.body ? document.body.outerHTML.slice(0, 60000) : '',
      };
    },
  };

  window.__yw = api;
  return 'installed:' + api.ver;
})()
