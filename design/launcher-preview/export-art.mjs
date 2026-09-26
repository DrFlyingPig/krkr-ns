import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const directory = path.dirname(fileURLToPath(import.meta.url));
const template = await readFile(path.join(directory, 'hero-artwork.svg'), 'utf8');
const sourceColors = ['#E9E6F1', '#C9D6C7', '#E9C4B3', '#F3E3B3', '#FFFDFA'];
const variants = [
  { name: 'lavender', colors: ['#E9E6F1', '#C9D6C7', '#E9C4B3', '#F3E3B3', '#FFFDFA'], angle: -8, ink: '#8B7EAB' },
  { name: 'sage', colors: ['#E6EDE6', '#C1D3C3', '#E7CEC1', '#F0E2B6', '#FFFDFA'], angle: 7, ink: '#7F9587' },
  { name: 'peach', colors: ['#F1E8DE', '#CED7C5', '#E9C5B3', '#F3E1B4', '#FFFDFA'], angle: -5, ink: '#B48B79' },
  { name: 'moon', colors: ['#E6E5F0', '#C8D5D2', '#DDC7DC', '#F2E3BD', '#FFFDFA'], angle: 9, ink: '#8B82AD' },
  { name: 'mint', colors: ['#E3ECE7', '#BCD3C5', '#EACBB9', '#EDE2BC', '#FFFDFA'], angle: -7, ink: '#7C9A8D' }
];
const coordinates = [];
for (const [index, variant] of variants.entries()) {
  const replacement = Object.fromEntries(sourceColors.map((color, offset) => [color, variant.colors[offset]]));
  const svg = template.replace(/#[0-9A-F]{6}/g, color => replacement[color] || color).replace('rotate(-8 152 130)', `rotate(${variant.angle} 152 130)`);
  const file = `hero-artwork-${String(index + 1).padStart(2, '0')}.svg`;
  await writeFile(path.join(directory, file), svg, 'utf8');
  const angle = variant.angle * Math.PI / 180;
  const initial = { centerX: +(491 - 8 * Math.cos(angle) - 45 * Math.sin(angle)).toFixed(2), baselineY: +(189 - 8 * Math.sin(angle) + 45 * Math.cos(angle)).toFixed(2), size: 123, angle: variant.angle, color: variant.ink };
  coordinates.push({ file, name: variant.name, width: 860, height: 358, colors: variant.colors, initial });
}
await writeFile(path.join(directory, 'hero-artwork-coordinates.json'), `${JSON.stringify(coordinates, null, 2)}\n`, 'utf8');
console.log(`Exported ${variants.length} original hero backgrounds and initial coordinates.`);
