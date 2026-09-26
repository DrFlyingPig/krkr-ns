/* Local visual review. Selection and preferences stay in memory; no game I/O. */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const library = [
    { title: '风在信纸上停留', palette: 0 },
    { title: '雨后，第七封来信', palette: 1 },
    { title: '黄昏邮局', palette: 2 },
    { title: '月亮收藏室', palette: 3 },
    { title: '薄荷色的远行', palette: 4 },
    { title: '未完成的花园', palette: 5 },
    { title: '星星落在旧车站', palette: 3 },
    { title: '雾里的海岸线', palette: 4 },
    { title: '冬日的纸飞机', palette: 0 },
    { title: '一朵云的来访', palette: 1 },
    { title: '在所有季节都结束以前写给你的那封很长很长的信', palette: 2 },
    { title: '玻璃森林的回声', palette: 5 },
    { title: '今天，也有好天气', palette: 2 },
    { title: '微光与回家的路', palette: 3 }
  ];
  const palettes = [
    { bg: '#E9E6F1', mint: '#C9D6C7', peach: '#E9C4B3', cream: '#F3E3B3', ink: '#8B7EAB', thumb: '#D9D0E9' },
    { bg: '#E6EDE6', mint: '#C1D3C3', peach: '#E7CEC1', cream: '#F0E2B6', ink: '#7F9587', thumb: '#CDDDCF' },
    { bg: '#F1E8DE', mint: '#CED7C5', peach: '#E9C5B3', cream: '#F3E1B4', ink: '#B48B79', thumb: '#EDD1BC' },
    { bg: '#E6E5F0', mint: '#C8D5D2', peach: '#DDC7DC', cream: '#F2E3BD', ink: '#8B82AD', thumb: '#D2CBE9' },
    { bg: '#E3ECE7', mint: '#BCD3C5', peach: '#EACBB9', cream: '#EDE2BC', ink: '#7C9A8D', thumb: '#C6DDD3' },
    { bg: '#EDE8EB', mint: '#CCD7C4', peach: '#E8C0BD', cream: '#F0E0B8', ink: '#9D879A', thumb: '#E1CCDA' }
  ];
  const state = { selected: 0, page: 0, empty: new URLSearchParams(location.search).get('empty') === '1', motion: !matchMedia('(prefers-reduced-motion: reduce)').matches, mode: 'library', launchFiles: {}, imageSettings: {}, imageGame: null, imageRole: 'preview', imageDraft: null };
  let toastTimer;
  let artTimer;
  let lastFocus;
  let scanTimer;
  let uploadSequence = 0;
  const uploadedURLs = new Set();
  const escapeHTML = value => value.replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[char]));
  const star = (x, y, size, fill, rotate = 0) => `<path transform="translate(${x} ${y}) rotate(${rotate}) scale(${size})" d="M0-1C.17-.23.23-.17 1 0 .23.17.17.23 0 1-.17.23-.23.17-1 0-.23-.17-.17-.23 0-1Z" fill="${fill}"/>`;
  const flower = (x, y, fill, size = 1) => `<g transform="translate(${x} ${y}) scale(${size})">${Array.from({ length: 8 }, (_, i) => `<ellipse cx="0" cy="-32" rx="14" ry="31" transform="rotate(${i * 45})" fill="${fill}"/>`).join('')}<circle r="13" fill="#FFFDFA"/></g>`;
  function artwork(game, index) {
    const p = palettes[game.palette];
    const angle = [-8, 7, -5, 9, -7, 5][game.palette];
    const first = escapeHTML(game.title[0]);
    return `<svg viewBox="0 0 860 358" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="纸片、星轨与花朵组成的${first}字插画">
      <defs><pattern id="paper-grain" width="12" height="12" patternUnits="userSpaceOnUse"><circle cx="1" cy="1" r=".5" fill="${p.ink}" opacity=".035"/></pattern><filter id="paper-shadow" x="-25%" y="-25%" width="160%" height="160%"><feDropShadow dx="0" dy="9" stdDeviation="9" flood-color="${p.ink}" flood-opacity=".08"/></filter></defs>
      <circle cx="785" cy="92" r="209" fill="${p.mint}" opacity=".56"/>
      <path d="M-40 267c74-94 193-108 282-52 37 24 56 85 18 143H-40Z" fill="${p.mint}" opacity=".78"/>
      <path d="M-21 183c60-14 133-7 168 39 38 51 27 113-6 157" stroke="#FFFDFA" stroke-width="1.1" fill="none" opacity=".65"/>
      <path d="M-8 170c65-11 130 7 162 54 38 56 19 115-10 150" stroke="#FFFDFA" stroke-width="1.1" fill="none" opacity=".47"/>
      <ellipse cx="452" cy="181" rx="306" ry="108" transform="rotate(-17 452 181)" stroke="#FFFDFA" stroke-width="1.2" fill="none" opacity=".8"/>
      <ellipse cx="452" cy="181" rx="306" ry="108" transform="rotate(19 452 181)" stroke="${p.ink}" stroke-width=".7" fill="none" opacity=".24"/>
      <circle cx="211" cy="247" r="42" fill="${p.peach}"/>
      <circle cx="211" cy="247" r="30" stroke="#FFFDFA" stroke-width=".85" fill="none" opacity=".6"/>
      <path d="M198 247h26m-13-13v26" stroke="#FFFDFA" stroke-width="1.1" opacity=".8"/>
      <g transform="translate(339 59) rotate(${angle} 152 130)" filter="url(#paper-shadow)">
        <rect x="9" y="13" width="288" height="255" rx="4" fill="${p.peach}" opacity=".44"/>
        <rect width="288" height="255" rx="4" fill="#FFFDFA"/>
        <rect width="288" height="255" rx="4" fill="url(#paper-grain)"/>
        <path d="M240 0h48v46Z" fill="${p.cream}" opacity=".85"/>
        <path d="M240 0v46h48" fill="#F4EFE5"/>
        <path d="M25 27h94" stroke="${p.ink}" stroke-width="1.1" opacity=".18"/>
        <text x="144" y="175" text-anchor="middle" fill="${p.ink}" font-size="123" font-family="Noto Serif SC,Songti SC,SimSun,serif" font-weight="400">${first}</text>
        <path d="M32 223h85m54 0h85" stroke="${p.ink}" stroke-width=".8" opacity=".3"/>
        ${star(144, 223, 5, p.peach, 8)}
      </g>
      ${flower(738, 248, p.cream, .77)}
      ${flower(141, 126, p.peach, .35)}
      ${star(268, 96, 19, '#FFFDFA', 14)}
      ${star(677, 87, 10, p.ink, 13)}
      ${star(623, 299, 12, '#FFFDFA', 0)}
      ${star(101, 258, 9, p.ink, 15)}
      ${star(791, 172, 8, '#FFFDFA', 0)}
      <circle cx="296" cy="285" r="5" fill="${p.cream}"/><circle cx="689" cy="164" r="4" fill="#FFFDFA"/><circle cx="183" cy="86" r="2.5" fill="${p.ink}" opacity=".4"/><circle cx="284" cy="188" r="3" fill="#FFFDFA"/>
      <path d="m724 41 8 9-9 8-8-9Z" fill="${p.peach}" opacity=".7"/>
    </svg>`;
  }
  function emptyArtwork() {
    return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 390 235" aria-hidden="true"><ellipse cx="194" cy="213" rx="102" ry="9" fill="#EAE5EE"/><circle cx="210" cy="112" r="89" fill="#E8E3F2"/><ellipse cx="191" cy="114" rx="146" ry="63" transform="rotate(-17 191 114)" fill="none" stroke="#C7BEDE" stroke-width=".8"/><path d="m112 160 88-15 87 21-87 19Z" fill="#D9CFEB"/><path d="m112 160 1 32 86 23v-30Z" fill="#C7BDD8"/><path d="m199 185 88-19-1 33-87 16Z" fill="#DED7EC"/><path d="M117 151V72l75-10 8 80Z" fill="#FFFDFA"/><path d="m200 142-8-80 78 18 6 77Z" fill="#F5F2EC"/><path d="m133 88 43-6m-41 19 43-6m-41 19 30-5m50-14 43 10m-42 2 43 10m-42 2 27 6" stroke="#D7CDDF" stroke-width="1"/><path d="m199 141 1 41m-81-26 80 26 78-19" stroke="#BDB2D1" fill="none" stroke-width="1"/>${flower(315, 147, '#EBC8B5', .32)}${star(90, 75, 14, '#CFDBC7', 13)}${star(286, 38, 13, '#C6BAE0', 12)}${star(57, 144, 7, '#DACAA2', 0)}<circle cx="298" cy="92" r="6" fill="#F0DEAF"/><circle cx="99" cy="203" r="3" fill="#DDD1E9"/></svg>`;
  }
  function fitScreen() {
    const scale = Math.min(innerWidth / 1280, innerHeight / 720);
    document.querySelector('.console').style.transform = `scale(${scale})`;
  }
  function renderLibrary(animate = false) {
    const totalPages = Math.ceil(library.length / 6);
    $('library-count').textContent = state.empty ? '0 部作品' : `${library.length} 部作品`;
    $('detail').hidden = state.empty;
    $('empty-state').hidden = !state.empty;
    $('pagination').hidden = state.empty;
    $('selection-pill').hidden = state.empty;
    if (state.empty) {
      $('game-rows').innerHTML = '<div class="empty-list"><svg viewBox="0 0 40 40" aria-hidden="true"><path d="m5 11 14-3 15 4v22l-15-4-14 3Z"/><path d="M19 8v22M9 17l6-1m-6 6 6-1m9-5 6 2m-6 4 6 2"/></svg><span>暂无游戏</span></div>';
      $('empty-art').innerHTML = emptyArtwork();
      return;
    }
    state.page = Math.floor(state.selected / 6);
    const start = state.page * 6;
    $('game-rows').innerHTML = library.slice(start, start + 6).map((game, offset) => {
      const index = start + offset;
      const p = palettes[game.palette];
      const avatar = state.imageSettings[index]?.avatar;
      const thumb = avatar ? `<img src="${escapeHTML(avatar.url)}" alt="${escapeHTML(game.title)}的头像" data-image-id="${avatar.id}">` : `<span>${escapeHTML(game.title[0])}</span>`;
      return `<button class="game-row" role="option" aria-selected="${index === state.selected}" aria-label="${escapeHTML(game.title)}" data-game="${index}" title="${escapeHTML(game.title)}"><span class="game-thumb${avatar ? ' has-image' : ''}" style="background:${p.thumb};color:${p.ink}">${thumb}</span><span class="game-copy"><strong>${escapeHTML(game.title)}</strong><small>本地游戏</small></span><svg class="row-arrow" viewBox="0 0 14 18" aria-hidden="true"><path d="m4 4 5 5-5 5"/></svg></button>`;
    }).join('');
    $('selection-pill').style.transform = `translateY(${(state.selected % 6) * 76 + 2}px)`;
    $('page-label').innerHTML = `${String(state.page + 1).padStart(2, '0')} <span>/ ${String(totalPages).padStart(2, '0')}</span>`;
    $('prev-page').disabled = state.page === 0;
    $('next-page').disabled = state.page === totalPages - 1;
    renderDetail(animate);
  }
  function renderDetail(animate) {
    const game = library[state.selected];
    $('game-title').textContent = game.title;
    $('game-title').title = game.title;
    $('hero-number').textContent = String(state.selected + 1).padStart(2, '0');
    $('hero').style.background = palettes[game.palette].bg;
    const selectedIndex = state.selected;
    const preview = state.imageSettings[selectedIndex]?.preview;
    const art = preview ? `<img class="hero-selected-image" src="${escapeHTML(preview.url)}" alt="${escapeHTML(game.title)}的预览图" data-image-id="${preview.id}">` : artwork(game, selectedIndex);
    clearTimeout(artTimer);
    if (animate && state.motion) {
      $('hero-art').classList.add('changing');
      artTimer = setTimeout(() => {
        $('hero-art').innerHTML = art;
        $('hero-art').classList.remove('changing');
      }, 80);
    } else {
      $('hero-art').innerHTML = art;
      $('hero-art').classList.remove('changing');
    }
  }
  function selectGame(index) {
    if (state.empty || state.mode !== 'library') return;
    state.selected = (index + library.length) % library.length;
    renderLibrary(true);
  }
  function turnPage(direction) {
    if (state.empty) return;
    const nextPage = Math.max(0, Math.min(Math.ceil(library.length / 6) - 1, state.page + direction));
    if (nextPage !== state.page) selectGame(Math.min(nextPage * 6 + state.selected % 6, library.length - 1));
  }
  function toast(message) {
    clearTimeout(toastTimer);
    $('toast').textContent = message;
    $('toast').classList.add('show');
    toastTimer = setTimeout(() => $('toast').classList.remove('show'), 2000);
  }
  function refresh() {
    closeMenu();
    toast(state.empty ? '暂无游戏。' : `游戏库已刷新 · ${library.length} 部作品`);
  }
  function closeMenu() {
    $('view-menu').hidden = true;
    $('view-button').setAttribute('aria-expanded', 'false');
  }
  function setView(empty) {
    state.empty = empty;
    state.selected = 0;
    state.page = 0;
    closeMenu();
    renderLibrary();
  }
  function openOptions() {
    if (state.empty || state.mode !== 'library') return;
    closeMenu();
    lastFocus = document.activeElement;
    state.mode = 'options';
    $('options-story').textContent = library[state.selected].title;
    $('options-backdrop').hidden = false;
    const selectedFile = state.launchFiles[state.selected] || 'data.xp3';
    document.querySelectorAll('.file-choice').forEach(choice => choice.setAttribute('aria-selected', String(choice.dataset.file === selectedFile)));
    document.querySelector('.file-choice[aria-selected="true"]').focus();
  }
  function closeOptions() {
    $('options-backdrop').hidden = true;
    state.mode = 'library';
    lastFocus?.isConnected ? lastFocus.focus() : $('launch-button').focus();
  }
  function imageSettings(gameIndex) {
    return state.imageSettings[gameIndex] ||= { preview: null, avatar: null, candidates: [...window.KRKRPreviewCandidates] };
  }
  function imageCard(id) {
    return [...$('image-candidates').querySelectorAll('[data-image-id]')].find(card => card.dataset.imageId === id);
  }
  function openImages() {
    if (state.empty || state.mode !== 'library') return;
    closeMenu();
    lastFocus = document.activeElement;
    state.imageGame = state.selected;
    state.imageRole = 'preview';
    const settings = imageSettings(state.imageGame);
    const first = settings.candidates[0]?.id || null;
    state.imageDraft = { preview: settings.preview?.id || first, avatar: settings.avatar?.id || first };
    state.mode = 'images';
    $('images-game-title').textContent = library[state.imageGame].title;
    $('images-backdrop').hidden = false;
    renderImages();
    imageCard(state.imageDraft.preview)?.focus();
    simulateImageScan();
  }
  function closeImages() {
    clearInterval(scanTimer);
    $('image-progress-track').hidden = true;
    $('image-scan').disabled = false;
    $('images-backdrop').hidden = true;
    state.imageDraft = null;
    state.imageGame = null;
    state.mode = 'library';
    lastFocus?.isConnected ? lastFocus.focus() : $('images-button').focus();
  }
  function renderImages() {
    if (state.mode !== 'images') return;
    const settings = imageSettings(state.imageGame);
    const role = state.imageRole;
    const selectedId = state.imageDraft[role];
    const selected = settings.candidates.find(image => image.id === selectedId);
    $('image-tab-preview').setAttribute('aria-selected', String(role === 'preview'));
    $('image-tab-avatar').setAttribute('aria-selected', String(role === 'avatar'));
    $('image-content').setAttribute('aria-labelledby', `image-tab-${role}`);
    $('image-role-label').textContent = role === 'preview' ? '右侧大图' : '列表头像';
    $('image-count').textContent = `${settings.candidates.length} 张图片`;
    $('image-candidates').innerHTML = settings.candidates.map((image, index) => `<button class="image-candidate" role="option" aria-selected="${image.id === selectedId}" data-image-id="${image.id}" title="${escapeHTML(image.name)}"><img src="${escapeHTML(image.url)}" alt="${escapeHTML(image.name)}"><span class="image-candidate-check">${image.id === selectedId ? '✓' : ''}</span><span class="image-candidate-caption"><strong>${escapeHTML(image.name)}</strong><small>${settings[role]?.id === image.id ? '正在使用' : index === 0 ? '推荐' : image.kind === 'local' ? '本地' : '示例'}</small></span></button>`).join('');
    $('image-selection-preview').className = `image-selection-preview${role === 'avatar' ? ' avatar' : ''}${!selected && role === 'avatar' ? ' default-avatar' : ''}`;
    $('image-selection-preview').innerHTML = selected ? `<img src="${escapeHTML(selected.url)}" alt="所选${role === 'preview' ? '大图' : '头像'}">` : role === 'preview' ? artwork(library[state.imageGame], state.imageGame) : escapeHTML(library[state.imageGame].title[0]);
    $('image-selection-name').textContent = selected?.name || (role === 'preview' ? '默认大图' : '默认头像');
    $('image-role-description').textContent = role === 'preview' ? '应用后显示在右侧大图。' : '应用后显示在列表头像。';
    $('image-apply').disabled = !selected;
  }
  function switchImageRole(role) {
    if (state.mode !== 'images' || !['preview', 'avatar'].includes(role)) return;
    state.imageRole = role;
    renderImages();
    (imageCard(state.imageDraft[role]) || $('image-candidates').querySelector('button') || $(`image-tab-${role}`)).focus();
  }
  function chooseImage(id) {
    if (state.mode !== 'images' || !imageSettings(state.imageGame).candidates.some(image => image.id === id)) return;
    state.imageDraft[state.imageRole] = id;
    renderImages();
    imageCard(id)?.focus();
  }
  function applyImage() {
    if (state.mode !== 'images') return;
    const settings = imageSettings(state.imageGame);
    const image = settings.candidates.find(candidate => candidate.id === state.imageDraft[state.imageRole]);
    if (!image) return;
    const role = state.imageRole;
    settings[role] = image;
    renderLibrary();
    closeImages();
    toast(role === 'preview' ? '大图已更新。' : '头像已更新。');
  }
  function resetImage() {
    if (state.mode !== 'images') return;
    imageSettings(state.imageGame)[state.imageRole] = null;
    state.imageDraft[state.imageRole] = null;
    renderLibrary();
    renderImages();
    $('image-reset').focus();
    toast(state.imageRole === 'preview' ? '已恢复默认大图。' : '已恢复默认头像。');
  }
  function simulateImageScan() {
    if (state.mode !== 'images') return;
    clearInterval(scanTimer);
    let progress = 0;
    $('image-scan').disabled = true;
    $('image-progress-track').hidden = false;
    const showProgress = () => {
      $('image-progress').style.width = `${progress}%`;
      $('image-progress').setAttribute('aria-valuenow', String(progress));
      $('image-status').textContent = `模拟扫描 · ${progress}%（不访问游戏内容）`;
    };
    showProgress();
    scanTimer = setInterval(() => {
      if (state.mode !== 'images') { clearInterval(scanTimer); return; }
      progress = Math.min(100, progress + 20);
      showProgress();
      if (progress === 100) {
        clearInterval(scanTimer);
        $('image-scan').disabled = false;
        $('image-progress-track').hidden = true;
        $('image-status').textContent = '已生成示例候选图片，不访问真实游戏内容。';
      }
    }, 160);
  }
  async function importImage(file) {
    if (!file || state.mode !== 'images') return;
    clearInterval(scanTimer);
    $('image-progress-track').hidden = true;
    $('image-scan').disabled = false;
    const validType = ['image/png', 'image/jpeg'].includes(file.type) || !file.type && /\.(png|jpe?g)$/i.test(file.name);
    if (!validType) { $('image-status').textContent = '请选择 PNG 或 JPEG 图片。'; return; }
    const gameIndex = state.imageGame;
    const role = state.imageRole;
    const draft = state.imageDraft;
    const url = URL.createObjectURL(file);
    const image = new Image();
    $('image-status').textContent = '正在读取本地图片…';
    image.src = url;
    try { await image.decode(); }
    catch { URL.revokeObjectURL(url); if (state.imageGame === gameIndex) $('image-status').textContent = '无法读取这张图片，请选择其他图片。'; return; }
    uploadedURLs.add(url);
    const candidate = { id: `local-${++uploadSequence}`, name: file.name, url, kind: 'local' };
    imageSettings(gameIndex).candidates.push(candidate);
    if (state.mode === 'images' && state.imageGame === gameIndex && state.imageDraft === draft) {
      state.imageDraft[role] = candidate.id;
      renderImages();
      if (state.imageRole === role) imageCard(candidate.id)?.focus();
      $('image-status').textContent = '本地图片已加入候选，仅保留在浏览器内存中。';
    }
  }
  function moveImageFocus(direction) {
    const controls = [...$('image-candidates').querySelectorAll('button')];
    if (!controls.length) return;
    let index = controls.indexOf(document.activeElement);
    if (index < 0) index = controls.findIndex(card => card.dataset.imageId === state.imageDraft[state.imageRole]);
    const delta = direction === 'up' ? -3 : direction === 'down' ? 3 : direction === 'left' ? -1 : 1;
    index = Math.max(0, Math.min(controls.length - 1, Math.max(0, index) + delta));
    chooseImage(controls[index].dataset.imageId);
    imageCard(state.imageDraft[state.imageRole])?.scrollIntoView({ block: 'nearest' });
  }
  function confirmImageAction() {
    const focused = document.activeElement;
    if (focused?.matches('.image-candidate')) { chooseImage(focused.dataset.imageId); applyImage(); }
    else if (focused?.tagName === 'BUTTON' && focused.closest('#images-dialog') && focused.id !== 'image-apply') focused.click();
    else applyImage();
  }
  function requestLaunch() {
    if (state.empty || state.mode !== 'library') return;
    closeMenu();
    launch();
  }
  function launch() {
    $('launch-title').textContent = library[state.selected].title;
    $('launch-state').hidden = false;
    state.mode = 'launch';
    $('return-library').focus();
  }
  function returnLibrary() {
    $('launch-state').hidden = true;
    state.mode = 'library';
    $('launch-button').focus();
  }
  function back() {
    if (state.mode === 'options') closeOptions();
    else if (state.mode === 'images') closeImages();
    else if (state.mode === 'launch') returnLibrary();
    else if (!$('view-menu').hidden) closeMenu();
    else toast('已在游戏库。');
  }
  $('game-rows').addEventListener('click', event => {
    const row = event.target.closest('[data-game]');
    if (row) selectGame(Number(row.dataset.game));
  });
  $('prev-page').addEventListener('click', () => turnPage(-1));
  $('next-page').addEventListener('click', () => turnPage(1));
  $('launch-button').addEventListener('click', requestLaunch);
  $('footer-launch').addEventListener('click', requestLaunch);
  $('footer-back').addEventListener('click', back);
  $('options-button').addEventListener('click', openOptions);
  $('detail-options-button').addEventListener('click', openOptions);
  $('images-button').addEventListener('click', openImages);
  $('footer-images').addEventListener('click', openImages);
  $('close-images').addEventListener('click', closeImages);
  $('images-backdrop').addEventListener('click', event => { if (event.target === $('images-backdrop')) closeImages(); });
  $('image-tab-preview').addEventListener('click', () => switchImageRole('preview'));
  $('image-tab-avatar').addEventListener('click', () => switchImageRole('avatar'));
  $('image-candidates').addEventListener('click', event => { const card = event.target.closest('[data-image-id]'); if (card) chooseImage(card.dataset.imageId); });
  $('image-scan').addEventListener('click', simulateImageScan);
  $('image-upload').addEventListener('click', () => $('image-file').click());
  $('image-file').addEventListener('change', event => { const file = event.target.files[0]; event.target.value = ''; importImage(file); });
  $('image-apply').addEventListener('click', applyImage);
  $('image-reset').addEventListener('click', resetImage);
  window.addEventListener('unload', () => uploadedURLs.forEach(url => URL.revokeObjectURL(url)));
  $('footer-refresh').addEventListener('click', refresh);
  $('refresh-button').addEventListener('click', refresh);
  $('empty-refresh').addEventListener('click', refresh);
  $('view-button').addEventListener('click', () => {
    $('view-menu').hidden = !$('view-menu').hidden;
    $('view-button').setAttribute('aria-expanded', String(!$('view-menu').hidden));
  });
  $('sample-view').addEventListener('click', () => setView(false));
  $('empty-view').addEventListener('click', () => setView(true));
  $('close-options').addEventListener('click', closeOptions);
  $('done-options').addEventListener('click', closeOptions);
  $('options-backdrop').addEventListener('click', event => { if (event.target === $('options-backdrop')) closeOptions(); });
  document.querySelector('.console').classList.toggle('no-motion', !state.motion);
  $('file-options').addEventListener('click', event => {
    const choice = event.target.closest('[data-file]');
    if (!choice) return;
    state.launchFiles[state.selected] = choice.dataset.file;
    closeOptions();
    toast('启动文件已选择。');
  });
  $('return-library').addEventListener('click', returnLibrary);
  document.addEventListener('click', event => {
    if (!event.target.closest('.view-menu') && !event.target.closest('#view-button')) closeMenu();
  });
  document.addEventListener('keydown', event => {
    const key = event.key.toLowerCase();
    if (key === 'tab' && (state.mode === 'options' || state.mode === 'images' || state.mode === 'launch')) {
      const dialog = state.mode === 'options' ? $('options-backdrop') : state.mode === 'images' ? $('images-dialog') : $('launch-state');
      const focusable = [...dialog.querySelectorAll('button:not(:disabled)')];
      if (event.shiftKey && document.activeElement === focusable[0]) { event.preventDefault(); focusable.at(-1).focus(); }
      if (!event.shiftKey && document.activeElement === focusable.at(-1)) { event.preventDefault(); focusable[0].focus(); }
      return;
    }
    if (key === 'b' || key === 'escape') { event.preventDefault(); back(); return; }
    if (key === 'a' || key === 'enter') {
      event.preventDefault();
      if (event.repeat) return;
      if (state.mode === 'images') confirmImageAction();
      else if (state.mode === 'options') {
        if (document.activeElement?.tagName === 'BUTTON') document.activeElement.click();
      } else if (state.mode === 'launch') returnLibrary();
      else if (!$('view-menu').hidden && document.activeElement.closest('.view-menu')) document.activeElement.click();
      else if (state.empty) refresh();
      else requestLaunch();
      return;
    }
    if (state.mode === 'images') {
      if (['arrowup', 'arrowdown', 'arrowleft', 'arrowright', 'q', 'e', 'y'].includes(key)) event.preventDefault();
      if (key.startsWith('arrow')) moveImageFocus(key.slice(5));
      if (key === 'q' || key === 'e') switchImageRole(state.imageRole === 'preview' ? 'avatar' : 'preview');
      if (key === 'y' && !event.repeat) simulateImageScan();
      return;
    }
    if (state.mode === 'options') {
      if (['arrowup', 'arrowdown', 'arrowleft', 'arrowright'].includes(key)) {
        event.preventDefault();
        const scope = $('options-backdrop');
        const controls = [...scope.querySelectorAll('button')];
        const next = (controls.indexOf(document.activeElement) + (key === 'arrowup' || key === 'arrowleft' ? -1 : 1) + controls.length) % controls.length;
        controls[next].focus();
      }
      return;
    }
    if (state.mode !== 'library') return;
    if (['arrowup', 'arrowdown', 'arrowleft', 'arrowright', 'x', 'y', '-', 'i'].includes(key)) event.preventDefault();
    if (key === 'arrowup') selectGame(state.selected - 1);
    if (key === 'arrowdown') selectGame(state.selected + 1);
    if (key === 'arrowleft') turnPage(-1);
    if (key === 'arrowright') turnPage(1);
    if (key === 'x' && !event.repeat) openOptions();
    if (key === 'y' && !event.repeat) refresh();
    if ((key === '-' || key === 'i') && !event.repeat) openImages();
  });
  // Standard Gamepad mapping: physical right=A, bottom=B, top=X, left=Y.
  let previousButtons = [];
  let previousDirection = '';
  let nextRepeat = 0;
  function pollGamepad(timestamp) {
    const pad = navigator.getGamepads?.().find?.(gamepad => gamepad && gamepad.connected);
    if (pad) {
      const current = pad.buttons.map(button => button.pressed);
      const press = index => current[index] && !previousButtons[index];
      if (press(1)) {
        if (state.mode === 'images') confirmImageAction();
        else if (state.mode === 'options') document.activeElement?.click();
        else if (state.mode === 'launch') returnLibrary();
        else if (state.empty) refresh();
        else requestLaunch();
      }
      if (press(0)) back();
      if (press(3)) openOptions();
      if (press(2) && state.mode === 'library') refresh();
      if (press(2) && state.mode === 'images') simulateImageScan();
      if (press(8)) openImages();
      if ((press(4) || press(5)) && state.mode === 'images') switchImageRole(state.imageRole === 'preview' ? 'avatar' : 'preview');
      const direction = current[12] || pad.axes[1] < -.55 ? 'up' : current[13] || pad.axes[1] > .55 ? 'down' : current[14] || pad.axes[0] < -.55 ? 'left' : current[15] || pad.axes[0] > .55 ? 'right' : '';
      if (direction && (direction !== previousDirection || timestamp >= nextRepeat)) {
        if (state.mode === 'library') {
          if (direction === 'up') selectGame(state.selected - 1);
          else if (direction === 'down') selectGame(state.selected + 1);
          else turnPage(direction === 'left' ? -1 : 1);
        } else if (state.mode === 'images') moveImageFocus(direction);
        else if (state.mode === 'options') {
          const scope = $('options-backdrop');
          const controls = [...scope.querySelectorAll('button')];
          const next = (controls.indexOf(document.activeElement) + (direction === 'up' || direction === 'left' ? -1 : 1) + controls.length) % controls.length;
          controls[next].focus();
        }
        nextRepeat = timestamp + (direction !== previousDirection ? 320 : 160);
      }
      previousDirection = direction;
      previousButtons = current;
    } else { previousButtons = []; previousDirection = ''; }
    requestAnimationFrame(pollGamepad);
  }
  // Touch swipes change the selected story without launching it.
  let touchStart;
  document.querySelector('.library').addEventListener('touchstart', event => {
    const point = event.changedTouches[0]; touchStart = { x: point.clientX, y: point.clientY };
  }, { passive: true });
  document.querySelector('.library').addEventListener('touchend', event => {
    if (!touchStart) return;
    const point = event.changedTouches[0];
    const dx = point.clientX - touchStart.x, dy = point.clientY - touchStart.y;
    if (Math.max(Math.abs(dx), Math.abs(dy)) > 36) {
      if (Math.abs(dx) > Math.abs(dy)) turnPage(dx < 0 ? 1 : -1);
      else selectGame(state.selected + (dy < 0 ? 1 : -1));
    }
    touchStart = null;
  }, { passive: true });
  window.addEventListener('resize', fitScreen);
  fitScreen();
  renderLibrary();
  requestAnimationFrame(pollGamepad);
})();
