// The site's texts in both languages and the language choice: ?lang= first, then the visitor's own pick (the
// switch in the header), then the browser's languages; Hebrew lays the page out right to left. Then the download
// buttons: GitHub's public releases list names the newest installer, so the buttons work while every release is
// a pre-release (releases/latest skips those and would answer 404).
(function () {
  var REPO = 'israel2176/win.apk';
  var GH = 'https://github.com/' + REPO;
  var T = {
    en: {
      title: 'Win.apk: Android apps on Windows',
      description: 'Install an Android app and it runs like any Windows program. No emulator. Free and open source.',
      skip: 'Skip to content', footer_label: 'Links',
      lang_switch: 'עברית', lang_label: 'עברית: switch the page to Hebrew',
      hero_title: 'Android apps on Windows',
      hero_sub: 'Install an app and it runs like any Windows program. No emulator.',
      download: 'Download for Windows',
      meta_default: 'Windows 10 and 11, 64-bit',
      meta_req: 'Windows 10 and 11, 64-bit', meta_pre: 'preview',
      star: 'Star us on GitHub', star_short: 'Star us',
      free: 'Free and open source.', portable_zip: 'Portable ZIP', other_versions: 'Other versions',
      alt_library: 'The Win.apk library with installed Android apps',
      f1_t: 'Double-click to install', f1_d: 'APK files and app bundles install like any Windows program.',
      f2_t: 'A window of its own', f2_d: 'Each app is in the Start menu and on the taskbar, with Windows notifications.',
      f3_t: 'In Hebrew too', f3_d: 'The whole program, installer included, reads right to left.',
      apps_title: 'Apps', apps_sub: 'Real screenshots from Win.apk.',
      cap_waze: 'Waze', cap_moovit: 'Moovit', cap_maccabi: 'Maccabi', cap_hapoalim: 'Bank Hapoalim',
      alt_waze: 'Waze in its own window on Windows, asking in Hebrew for the precise location',
      alt_moovit: 'Moovit\'s home screen in Hebrew, in its own window on Windows',
      alt_maccabi: 'The Maccabi app\'s start screen in Hebrew, with its sign-in button',
      alt_hapoalim: 'Bank Hapoalim\'s app in Hebrew, with its sign-in button',
      apps_note: 'Not every app works yet.', compat_link: 'Compatibility list',
      start_title: 'Three steps',
      s1: 'Download the installer.',
      s2: 'Run it. If SmartScreen warns: More info, then Run anyway.',
      s3: 'Open an APK, or pick an app in the Store.',
      l_releases: 'Releases', l_docs: 'Getting started', l_faq: 'FAQ', l_compat: 'Compatibility list',
      l_report: 'Report a problem', l_license: 'License (AGPL-3.0)'
    },
    he: {
      title: 'Win.apk: אפליקציות אנדרואיד ב-Windows',
      description: 'מתקינים אפליקציית אנדרואיד, והיא עובדת כמו כל תוכנה במחשב. בלי אמולטור. חינם וקוד פתוח.',
      skip: 'דילוג לתוכן', footer_label: 'קישורים',
      lang_switch: 'English', lang_label: 'English: הצגת העמוד באנגלית',
      hero_title: 'אפליקציות אנדרואיד ב-Windows',
      hero_sub: 'מתקינים אפליקציה, והיא עובדת כמו כל תוכנה במחשב. בלי אמולטור.',
      download: 'הורדה ל-Windows',
      meta_default: 'Windows 10 ו-11, ‏64 ביט',
      meta_req: 'Windows 10 ו-11, ‏64 ביט', meta_pre: 'גרסת תצוגה',
      star: 'פרגנו בכוכב ב-GitHub', star_short: 'פרגנו בכוכב',
      free: 'חינם וקוד פתוח.', portable_zip: 'גרסה ניידת (ZIP)', other_versions: 'גרסאות נוספות',
      alt_library: 'הספרייה של Win.apk עם אפליקציות אנדרואיד מותקנות',
      f1_t: 'לחיצה כפולה ומתקינים', f1_d: 'קובצי APK וחבילות אפליקציה מותקנים כמו כל תוכנה.',
      f2_t: 'חלון משלה', f2_d: 'כל אפליקציה בתפריט התחל ובשורת המשימות, עם התראות של Windows.',
      f3_t: 'גם בעברית', f3_d: 'כל התוכנה, כולל ההתקנה, מימין לשמאל.',
      apps_title: 'אפליקציות', apps_sub: 'צילומי מסך אמיתיים מתוך Win.apk.',
      cap_waze: 'וייז', cap_moovit: 'מוביט', cap_maccabi: 'מכבי', cap_hapoalim: 'בנק הפועלים',
      alt_waze: 'וייז בחלון משלו ב-Windows, מבקש בעברית גישה למיקום המדויק',
      alt_moovit: 'מסך הבית של מוביט בעברית, בחלון משלו ב-Windows',
      alt_maccabi: 'מסך הפתיחה של אפליקציית מכבי בעברית, עם כפתור הכניסה',
      alt_hapoalim: 'אפליקציית בנק הפועלים בעברית, עם כפתור הכניסה לחשבון',
      apps_note: 'עוד לא כל אפליקציה עובדת.', compat_link: 'רשימת התאימות',
      start_title: 'שלושה צעדים',
      s1: 'מורידים את קובץ ההתקנה.',
      s2: 'מריצים אותו. אם SmartScreen מזהיר: "מידע נוסף", ואז "הפעל בכל זאת".',
      s3: 'פותחים קובץ APK, או בוחרים אפליקציה בחנות.',
      l_releases: 'גרסאות', l_docs: 'מדריך התחלה', l_faq: 'שאלות נפוצות', l_compat: 'רשימת התאימות',
      l_report: 'דיווח על בעיה', l_license: 'רישיון (AGPL-3.0)'
    }
  };
  // A link to a document goes to its Hebrew page where one exists.
  var DOCS = {
    en: { start: 'docs/getting-started.md', compat: 'docs/APPS.md', faq: 'docs/faq.md' },
    he: { start: 'docs/he/getting-started.md', compat: 'docs/APPS.md', faq: 'docs/he/faq.md' }
  };

  // Storage can throw (private windows, blocked site data); the page works without it.
  function store(kind, key, v) {
    try {
      var s = window[kind];
      if (v === undefined) return s.getItem(key);
      s.setItem(key, v);
    } catch (e) {}
    return null;
  }

  function pick() {
    var q = new URLSearchParams(location.search).get('lang');
    if (q === 'he' || q === 'en') return q;
    var s = store('localStorage', 'winapk-lang');
    if (s === 'he' || s === 'en') return s;
    var langs = navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language || 'en'];
    for (var i = 0; i < langs.length; i++) {
      var l = String(langs[i]).toLowerCase();
      if (l.indexOf('he') === 0 || l.indexOf('iw') === 0) return 'he';
      if (l.indexOf('en') === 0) return 'en';
    }
    return 'en';
  }

  var lang = pick();
  var dark = window.matchMedia ? matchMedia('(prefers-color-scheme: dark)') : { matches: false };
  var heShots = [];   // the screenshots the deploy has in Hebrew, from images/he/shots.json
  var release = null;  // { v, size, pre } of the installer the buttons point at, once GitHub has answered

  // The installer's line: version, size, what it runs on. Each part is isolated, so a Latin version and size keep
  // their order inside a Hebrew line; a tag such as v0.3.0-preview already says that it is a preview.
  function meta() {
    var el = document.querySelector('[data-dl-meta]');
    if (!el || !release) return;
    var t = T[lang];
    var dir = lang === 'he' ? 'rtl' : 'ltr';
    var parts = [[release.v, 'ltr'], [release.size + ' MB', 'ltr'], [t.meta_req, dir]];
    if (release.pre && release.v.indexOf('-') < 0) parts.splice(1, 0, [t.meta_pre, dir]);
    el.textContent = '';
    parts.forEach(function (p, i) {
      if (i) el.appendChild(document.createTextNode(' · '));
      var b = document.createElement('bdi');
      b.dir = p[1];
      b.textContent = p[0];
      el.appendChild(b);
    });
  }

  function apply() {
    var t = T[lang];
    var root = document.documentElement;
    root.lang = lang;
    root.dir = lang === 'he' ? 'rtl' : 'ltr';
    document.title = t.title;
    var desc = document.querySelector('meta[name=description]');
    if (desc) desc.content = t.description;
    var ogTitle = document.querySelector('meta[property="og:title"]');
    if (ogTitle) ogTitle.content = t.title;
    var ogDesc = document.querySelector('meta[property="og:description"]');
    if (ogDesc) ogDesc.content = t.description;
    // A Hebrew prefix letter before a Latin word ("ב-Win.apk") takes a non-breaking hyphen, so a line never ends
    // on the letter alone.
    var text = lang === 'he' ? function (s) { return s.replace(/(^|[\s(])([בהוכלמש])-(?=[A-Za-z0-9])/g, '$1$2‑'); }
                             : function (s) { return s; };
    document.querySelectorAll('[data-i18n]').forEach(function (el) { el.textContent = text(t[el.dataset.i18n]); });
    document.querySelectorAll('[data-i18n-label]').forEach(function (el) { el.setAttribute('aria-label', t[el.dataset.i18nLabel]); });
    document.querySelectorAll('[data-i18n-alt]').forEach(function (el) { el.alt = t[el.dataset.i18nAlt]; });
    // The switch names the other language in that language.
    var sw = document.querySelector('[data-lang-toggle] span');
    if (sw) sw.lang = lang === 'he' ? 'en' : 'he';
    // A Hebrew screenshot (images/he/<name>) when the deploy has one (it lists them in images/he/shots.json, so
    // no picture is asked for in vain), else the English one. A picture missing altogether hides its figure, and
    // a gallery left with none hides its section.
    var SHOT_SIZES = { 'images/library.png': [972, 686], 'images/dark.png': [972, 686], 'images/he/library.png': [1053, 717] };
    document.querySelectorAll('img[data-shot]').forEach(function (el) {
      // The program's own dark theme on a page shown dark.
      var name = el.dataset.shotDark && dark.matches ? el.dataset.shotDark : el.dataset.shot;
      var fig = el.closest('figure');
      var sec = el.closest('section');
      if (fig) fig.hidden = false;
      if (sec) sec.hidden = false;
      el.onerror = function () {
        el.onerror = null;
        if (fig) fig.hidden = true;
        if (sec && !sec.querySelector('figure:not([hidden])')) sec.hidden = true;
      };
      var want = (lang === 'he' && heShots.indexOf(name) >= 0 ? 'images/he/' : 'images/') + name;
      if (SHOT_SIZES[want]) { el.width = SHOT_SIZES[want][0]; el.height = SHOT_SIZES[want][1]; }
      if (el.getAttribute('src') !== want) el.src = want;
    });
    document.querySelectorAll('[data-doc]').forEach(function (el) { el.href = GH + '/blob/main/' + DOCS[lang][el.dataset.doc]; });
    meta();
  }

  document.querySelector('[data-lang-toggle]').addEventListener('click', function () {
    lang = lang === 'he' ? 'en' : 'he';
    store('localStorage', 'winapk-lang', lang);
    // A ?lang= in the address would win over the choice on the next visit; keep the address in step.
    var u = new URL(location.href);
    if (u.searchParams.has('lang')) { u.searchParams.set('lang', lang); history.replaceState(null, '', u); }
    apply();
  });

  apply();
  if (dark.addEventListener) dark.addEventListener('change', apply);
  if (window.fetch) {
    fetch('images/he/shots.json').then(function (r) { return r.ok ? r.json() : []; }).then(function (list) {
      heShots = Array.isArray(list) ? list : [];
      if (heShots.length && lang === 'he') apply();
    }).catch(function () {});
  }

  // The newest release that has the installer: a full release when there is one, else the newest pre-release
  // (as the launcher's update check picks). Remembered for the tab's session, so reloading does not ask again
  // (GitHub allows 60 unauthenticated requests an hour).
  function choose(list) {
    var best = null;
    for (var i = 0; i < list.length; i++) {
      var r = list[i];
      if (r.draft) continue;
      var setup = null, zip = null;
      (r.assets || []).forEach(function (a) {
        if (a.name === 'Win.apk-Setup.exe') setup = a;
        if (a.name === 'Win.apk-windows-x64.zip') zip = a;
      });
      if (!setup) continue;
      var c = { v: r.tag_name, pre: !!r.prerelease, notes: r.html_url, setup: setup.browser_download_url,
                zip: zip ? zip.browser_download_url : null, size: Math.round(setup.size / 1048576) };
      if (!c.pre) return c;
      if (!best) best = c;
    }
    return best;
  }

  function safeGhUrl(u) {
    return typeof u === 'string' && /^https:\/\/github\.com\/israel2176\/win\.apk\//.test(u) ? u : null;
  }

  function point(c) {
    if (!c) return;
    release = c;
    var setupUrl = safeGhUrl(c.setup);
    var notesUrl = safeGhUrl(c.notes);
    var zipUrl = safeGhUrl(c.zip);
    if (setupUrl) document.querySelectorAll('[data-dl=setup]').forEach(function (el) { el.href = setupUrl; });
    if (notesUrl) document.querySelectorAll('[data-dl=notes]').forEach(function (el) { el.href = notesUrl; });
    if (zipUrl) document.querySelectorAll('[data-dl=zip]').forEach(function (el) { el.href = zipUrl; });
    meta();
  }

  var CACHE_KEY = 'winapk-release-v1';
  var TTL_MS = 15 * 60 * 1000;
  var cached = store('localStorage', CACHE_KEY);
  if (cached) {
    try {
      var parsed = JSON.parse(cached);
      if (parsed && parsed.ts && (Date.now() - parsed.ts < TTL_MS) && parsed.data) {
        point(parsed.data);
        return;
      }
    } catch (e) {}
  }
  if (!window.fetch) return;
  fetch('https://api.github.com/repos/' + REPO + '/releases?per_page=30', { headers: { Accept: 'application/vnd.github+json' } })
    .then(function (r) { return r.ok ? r.json() : []; })
    .then(function (list) {
      var c = choose(Array.isArray(list) ? list : []);
      if (c) store('localStorage', CACHE_KEY, JSON.stringify({ ts: Date.now(), data: c }));
      point(c);
    })
    .catch(function () { /* The buttons keep the releases page. */ });
})();
