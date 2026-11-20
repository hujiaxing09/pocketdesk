// 键盘：隐藏的输入框负责唤起 iPhone 键盘（含中文输入法），上面一排是 Windows 特殊键。
// 文字（包括输入法上屏的中文）整段发给电脑；退格、回车等转成按键。
// Ctrl/Alt/Shift/⊞ 点一下锁定，下一个按键或字母会带上它们（例如 Ctrl 然后 c = Ctrl+C）；
// 锁定状态下再点一次同一个键，就单独按一下它（例如点两下 ⊞ 打开开始菜单）。

const VK = { BACK: 8, TAB: 9, ENTER: 13, SHIFT: 16, CTRL: 17, ALT: 18, ESC: 27, DEL: 46, WIN: 91 };
const SENTINEL = ' '; // 留一个字符，退格才会产生 input 事件

function charToVk(ch) {
  if (ch.length !== 1) return 0;
  const c = ch.toUpperCase().charCodeAt(0);
  if ((c >= 65 && c <= 90) || (c >= 48 && c <= 57)) return c;
  if (ch === ' ') return 32;
  return 0;
}

export class Keyboard {
  constructor({ input, bar, send, onChange = () => {} }) {
    this.input = input;
    this.bar = bar;
    this.send = send;
    this.onChange = onChange;
    this.isOpen = false;
    this.composing = false;
    this.mods = new Set();

    input.addEventListener('compositionstart', () => { this.composing = true; });
    input.addEventListener('compositionend', (e) => {
      this.composing = false;
      if (e.data) this.typeText(e.data);
      this.reset();
    });
    input.addEventListener('input', (e) => this.onInput(e));
    input.addEventListener('keydown', (e) => this.onKeyDown(e));
    input.addEventListener('blur', () => {
      // 用户点了键盘上的“完成”或别处，键盘收起
      if (this.isOpen) setTimeout(() => { if (document.activeElement !== this.input) this.close(); }, 50);
    });

    for (const b of bar.querySelectorAll('button')) {
      // 用 pointerdown 并阻止默认行为，输入框不会失去焦点，键盘不会收起
      b.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        if (b.id === 'kb-close') { this.close(); return; }
        if (b.dataset.mod) this.toggleMod(Number(b.dataset.mod));
        else if (b.dataset.vk) this.press(Number(b.dataset.vk));
      });
    }

    const vv = window.visualViewport;
    const place = () => {
      if (!vv) return;
      const bottom = Math.max(0, window.innerHeight - vv.height - vv.offsetTop);
      document.documentElement.style.setProperty('--kb-bottom', `${bottom}px`);
    };
    vv?.addEventListener('resize', place);
    vv?.addEventListener('scroll', place);
  }

  // 必须在点击/触摸结束的事件处理里同步调用，iPhone 才会弹出键盘
  open() {
    this.isOpen = true;
    this.bar.hidden = false;
    this.reset();
    this.input.focus({ preventScroll: true });
    this.onChange(true);
  }

  close() {
    if (!this.isOpen) return;
    this.isOpen = false;
    this.bar.hidden = true;
    this.mods.clear();
    this.renderMods();
    this.input.blur();
    document.documentElement.style.setProperty('--kb-bottom', '0px');
    this.onChange(false);
  }

  toggle() {
    if (this.isOpen) this.close();
    else this.open();
  }

  reset() {
    this.input.value = SENTINEL;
    try { this.input.setSelectionRange(1, 1); } catch { /* 输入框还没获得焦点 */ }
  }

  onInput(e) {
    if (this.composing || e.isComposing) return;
    const type = e.inputType || '';
    if (type === 'deleteContentBackward') this.press(VK.BACK);
    else if (type === 'deleteContentForward') this.press(VK.DEL);
    else if (type === 'insertLineBreak' || type === 'insertParagraph') this.press(VK.ENTER);
    else if (type.startsWith('insert')) {
      const text = e.data ?? this.input.value.replace(SENTINEL, '');
      if (text) this.typeText(text);
    }
    this.reset();
  }

  onKeyDown(e) {
    if (this.composing || e.isComposing || e.keyCode === 229) return;
    const map = { Enter: VK.ENTER, Backspace: VK.BACK, Tab: VK.TAB, Escape: VK.ESC,
      ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Delete: VK.DEL };
    const vk = map[e.key];
    if (!vk) return;
    e.preventDefault(); // 不再产生 input 事件，避免发两次
    this.press(vk);
  }

  typeText(text) {
    // 锁定了修饰键时，单个字母/数字当作快捷键（Ctrl+C 等）
    if (this.mods.size && charToVk(text)) {
      this.press(charToVk(text));
      return;
    }
    this.send({ t: 'text', s: text });
    this.clearMods();
  }

  press(vk) {
    this.send({ t: 'combo', vks: [...this.mods, vk] });
    this.clearMods();
  }

  toggleMod(vk) {
    if (this.mods.has(vk)) {
      // 再点一次：单独按下这个键（例如 ⊞ 打开开始菜单）
      this.mods.delete(vk);
      this.press(vk);
      return;
    }
    this.mods.add(vk);
    this.renderMods();
  }

  clearMods() {
    if (!this.mods.size) return;
    this.mods.clear();
    this.renderMods();
  }

  renderMods() {
    for (const b of this.bar.querySelectorAll('button[data-mod]')) {
      b.classList.toggle('on', this.mods.has(Number(b.dataset.mod)));
    }
  }
}
