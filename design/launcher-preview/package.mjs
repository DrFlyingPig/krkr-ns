import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const directory = path.dirname(fileURLToPath(import.meta.url));
const [html, css, js, logo, mockArt] = await Promise.all(['index.html', 'style.css', 'app.js', 'logo-artwork.svg', 'mock-art.js'].map(name => readFile(path.join(directory, name), 'utf8')));
const inlineLogo = logo.replace('<svg ', '<svg class="brand-mark" aria-hidden="true" ');
const inlineScript = source => `<script>\n${source.replace(/<\/script/gi, '<\\/script')}\n</script>`;
const bundled = html.replace(/<svg class="brand-mark"[\s\S]*?<\/svg>/, () => inlineLogo).replace('<link rel="stylesheet" href="style.css">', () => `<style>\n${css}\n</style>`).replace('<script src="mock-art.js"></script>', () => inlineScript(mockArt)).replace('<script src="app.js"></script>', () => inlineScript(js));
const output = path.join(directory, 'KRKR-ns-launcher-review.html');
await writeFile(output, bundled, 'utf8');
await writeFile(path.join(directory, 'logo-review.svg'), logo.replace('width="56" height="56"', 'width="336" height="336"'), 'utf8');
console.log(`Built standalone review: ${output}`);
