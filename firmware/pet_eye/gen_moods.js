// 从 eyes_v14.html（找不到就依次退到 v12 / v10）里读出 MOODS 定义，生成 moods.h
// 用法：node gen_moods.js
// 原理：直接抓网页版里那段形状代码（常量 inW / mk / solid / wedge / MOODS…）原样执行，
//       所以网页改了形状，跑一次这个脚本就同步了，不存在"两边各写一份"的问题。

const fs = require('fs');
const path = require('path');

const cands = ['../../web/eyes.html'];
const htmlPath = cands.map(c => path.join(__dirname, c)).find(p => fs.existsSync(p));
const outPath = path.join(__dirname, 'moods.h');

if (!fs.existsSync(htmlPath)) {
  console.error('找不到 ../../web/eyes.html');
  console.error('这个脚本要求网页版和 esp32/pet_eye/ 保持原来的目录关系。');
  process.exit(1);
}

const lines = fs.readFileSync(htmlPath, 'utf8').split('\n');
const start = lines.findIndex(l => l.startsWith('const inW'));
if (start < 0) { console.error('网页版里没找到 const inW，结构变了？'); process.exit(1); }

const moodStart = lines.findIndex((l, i) => i > start && l.startsWith('const MOODS'));
if (moodStart < 0) { console.error('没找到 const MOODS'); process.exit(1); }

const end = lines.findIndex((l, i) => i > moodStart && l.trim() === '};');
if (end < 0) { console.error('没找到 MOODS 的结尾 };'); process.exit(1); }

const code = 'const COLS=12;\n' + lines.slice(start, end + 1).join('\n') + '\nreturn MOODS;';
const MOODS = new Function(code)();

// 顺序即 enum 值，**改顺序会改变串口号**，不要动
// v14 的 8 个新表情一律追加在末尾（网页里也是追加在表末，两边编号就对得上）
const order = ['normal', 'happy', 'smug', 'despise', 'blink', 'wink', 'peek',
               'surprise', 'angry', 'cry', 'dizzy', 'love', 'speechless',
               'think', 'sleep', 'shy', 'confused', 'rainbow',
               // ↓ v14 追加（18~24）
               'startle', 'angry2', 'nono', 'nonoBlink', 'dead',
               'nod', 'glitch'];

for (const n of order) {
  if (!MOODS[n]) { console.error('网页里少了表情: ' + n); process.exit(1); }
}

const decorMap = {
  null: 'DECOR_NONE', smug: 'DECOR_SMUG', wink: 'DECOR_WINK', peek: 'DECOR_PEEK',
  tear: 'DECOR_TEAR', shake: 'DECOR_SHAKE', hearts: 'DECOR_HEARTS',
  sweat: 'DECOR_SWEAT', swap: 'DECOR_SWAP', zzz: 'DECOR_ZZZ',
  shy: 'DECOR_SHY', confused: 'DECOR_CONFUSED', rainbow: 'DECOR_RAINBOW',
  // v14 追加
  jblob: 'DECOR_JBLOB',       // 被吓一跳：果冻拉长 + 感叹号
  // 生气三级已删（用户 11-13：删掉）。原来这里挂的是强抖 + 青筋泛红，整套一起拆了。
  nono: 'DECOR_NONO',         // 摇头（眯眼）
  nonoBlink: 'DECOR_NONO_BLINK',  // 摇头（闭眼）
  dead: 'DECOR_DEAD',         // 死鱼眼：X
  nod: 'DECOR_NOD',           // 点头（纵向，一次性）
  glitch: 'DECOR_GLITCH'      // 故障：画面撕裂
};

const f = v => {
  const s = v.toFixed(2).replace(/\.00$/, '').replace(/(\.\d)0$/, '$1');
  return s === '-0' ? '0' : s;
};
const arr = a => '{' + a.map(f).join(',') + '}';

const out = [];
out.push('// 自动生成：node gen_moods.js > moods.h   请勿手改');
out.push('// 来源：eyes_v14.html 的 MOODS 定义');
out.push('#pragma once');
out.push('#include <stdint.h>');
out.push('');
out.push('#define EYE_COLS 12');
out.push('');
out.push('enum Decor : uint8_t {');
out.push('  DECOR_NONE=0, DECOR_SMUG, DECOR_WINK, DECOR_PEEK, DECOR_TEAR, DECOR_SHAKE,');
out.push('  DECOR_HEARTS, DECOR_SWEAT, DECOR_SWAP, DECOR_ZZZ, DECOR_SHY,');
out.push('  DECOR_CONFUSED, DECOR_RAINBOW,');
out.push('  // v14 追加（务必加在末尾，值要跟下面的 MOOD_TABLE 一致）');
out.push('  DECOR_JBLOB, DECOR_NONO, DECOR_NONO_BLINK,');
out.push('  DECOR_DEAD, DECOR_NOD, DECOR_GLITCH');
out.push('};');
out.push('');
out.push('enum MoodId : uint8_t {');
order.forEach((n, i) => out.push(`  M_${n.toUpperCase()} = ${i},`));
out.push('  MOOD_COUNT');
out.push('};');
out.push('');
out.push('struct EyeShape { float top[EYE_COLS]; float bot[EYE_COLS]; };');
out.push('struct MoodDef {');
out.push('  EyeShape L, R;');
out.push('  float dx, dy, lx, rx, ldy, rdy;');
out.push('  Decor decor;');
out.push('};');
out.push('');
out.push('static const MoodDef MOOD_TABLE[MOOD_COUNT] = {');
order.forEach((n, i) => {
  const m = MOODS[n];
  out.push(`  // ${i}: ${n}`);
  out.push(`  { {${arr(m.L.top)}, ${arr(m.L.bot)}},`);
  out.push(`    {${arr(m.R.top)}, ${arr(m.R.bot)}},`);
  out.push(`    ${f(m.dx)}, ${f(m.dy)}, ${f(m.lx)}, ${f(m.rx)}, ${f(m.ldy)}, ${f(m.rdy)}, ${decorMap[m.decor] || 'DECOR_NONE'} },`);
});
out.push('};');
out.push('');
out.push('static const char* const MOOD_NAMES[MOOD_COUNT] = {');
out.push('  ' + order.map(n => `"${n}"`).join(', '));
out.push('};');
out.push('');

fs.writeFileSync(outPath, out.join('\n'), 'utf8');
console.log('已生成 ' + outPath + '（' + order.length + ' 个表情）');
