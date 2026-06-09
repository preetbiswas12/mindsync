const fs = require('fs');
const vm = require('vm');
const html = fs.readFileSync('C:\\Users\\preet\\Downloads\\mindsync\\dashboard.html', 'utf-8');
const m = html.match(/<script>([\s\S]+?)<\/script>/);
if (!m) { console.log('No script found'); process.exit(1); }
try {
  new vm.Script(m[1], { filename: 'dashboard.js' });
  console.log('PASS: JS parses OK - no syntax errors');
} catch(e) {
  console.log('FAIL:', e.message);
  const lines = m[1].split('\n');
  const match = e.stack.match(/dashboard\.js:(\d+)/);
  if (match) {
    const ln = parseInt(match[1]);
    console.log('Error at script line', ln);
    for (let i = Math.max(0,ln-5); i < Math.min(lines.length, ln+5); i++) {
      console.log((i+1) + ': ' + lines[i]);
    }
  }
}
