const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const context=vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(root,'assets/font-data.js'),'utf8'),context);
const html=fs.readFileSync(path.join(root,'index.html'),'utf8');
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
vm.runInContext(script.split('const cv=')[0],context);
const font=vm.runInContext('CLOCK_FONT',context);
for(let h=0;h<24;h++)for(let m=0;m<60;m++) {
  const text=context.clockDigits(h,m),width=context.textWidth(text,true);
  assert.ok(width<=304);
  const x=Math.floor((320-width)/2);
  const glyphs=context.layout(text,true,x,18);
  for(const p of glyphs) assert.ok(p.x>=8 && p.x+p.g.w<=312 && p.y+p.g.h<=172);
  assert.equal(x,320-(glyphs.at(-1).x+glyphs.at(-1).g.w),'Centered time');
}
assert.equal(context.clockDigits(0,0),'12:00');assert.equal(context.clockDigits(12,0),'12:00');
assert.equal(context.clockDigits(9,7),'9:07');assert.equal(context.textWidth('--:--',true),304);
for(const day of ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'])
  for(const month of ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'])
    for(let n=1;n<=31;n++) assert.ok(12+context.textWidth(`${day}, ${month} ${n}`,false)+20<=308-context.textWidth('AM',false));
const header=fs.readFileSync(path.join(root,'src/clock_font.h'),'utf8');
const bytes=header.split('CLOCK_ALPHA[] PROGMEM = {')[1].split('};')[0].match(/\d+/g).map(Number);
const records=[...header.split('CLOCK_GLYPHS[] = {')[1].matchAll(/\{([^}]+)\}/g)].map(m=>m[1].split(',').map(Number));
let i=0;
for(const [name,g] of Object.entries(font)) {
  const data=Buffer.from(g.data,'base64'),[off,w,h,advance]=records[i++];
  assert.equal(data.length,g.w*g.h);assert.equal(g.w,w);assert.equal(g.h,h);assert.equal(g.advance,advance);
  assert.deepEqual(data,Buffer.from(bytes.slice(off,off+w*h)),`${name}: firmware parity`);
  assert.ok(data.every(a=>a<=15));
  if(name.startsWith('large_')) assert.ok(data.some(a=>a>0 && a<15),'Antialiased curves');
}
console.log('PASS: 1,440 centered times; all dates fit; midnight/noon; placeholder; exact firmware/preview mask parity; antialiasing.');
