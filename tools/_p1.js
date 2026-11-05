(async () => {
  const inp = document.querySelector('.publish_time .ant-picker input');
  const wrap = inp.closest('.ant-picker');
  const fire = (el, type) => el.dispatchEvent(new MouseEvent(type, {bubbles: true, cancelable: true, view: window, button: 0}));
  const state = () => ({ cls: wrap.className, dd: document.querySelectorAll('.ant-picker-dropdown').length,
                         panels: document.querySelectorAll('[class*="picker-panel"]').length });
  const before = state();
  wrap.scrollIntoView({block: 'center'});
  fire(wrap, 'mousedown'); fire(wrap, 'mouseup'); fire(wrap, 'click');
  inp.focus(); fire(inp, 'mousedown'); fire(inp, 'mouseup'); fire(inp, 'click');
  await new Promise(r => setTimeout(r, 400));
  const after = state();
  return JSON.stringify({ before, after, wrapCls: wrap.className });
})()
