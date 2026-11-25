// The site's texts in both languages and the language choice: ?lang= first, then the visitor's own pick (the
// switch in the header), then the browser's languages; Hebrew lays the page out right to left. Then the download
// buttons: GitHub's public releases list names the newest installer, so the buttons work while every release is
// a pre-release (releases/latest skips those and would answer 404).
(function () {
  var REPO = 'israel2176/win.apk';
  var GH = 'https://github.com/' + REPO;
  var T = {
    en: {
      title: 'Win.apk: run Android apps on Windows',
      description: 'Run Android apps on Windows like any other program: their own window, Start menu entry and notifications. No emulator, no virtual machine. Free and open source.',
      skip: 'Skip to content', nav_label: 'Sections',
      nav_examples: 'What it does', nav_apps: 'Israeli apps', nav_start: 'Get started', nav_faq: 'FAQ',
      lang_switch: 'עברית', lang_label: 'עברית: switch the page to Hebrew',
      badge: 'Free · Open source · Public preview',
      hero_title: 'Android apps on your Windows PC, like any other program',
      hero_sub: 'No emulator, no virtual machine. Every app gets its own window, Start menu entry and notifications.',
      download: 'Download for Windows',
      meta_default: 'Windows 10 and 11 · 64-bit · Free',
      meta_req: 'Windows 10 and 11, 64-bit', meta_pre: 'preview',
      all_releases: 'All releases', portable: 'Portable ZIP', whats_new: 'What\'s new',
      ex_title: 'What you can do',
      e1_t: 'Install an APK with a double-click', e1_d: 'APK, APKM, XAPK or APKS: open the file, or drag it onto the window.',
      e2_t: 'Apps like Windows programs', e2_d: 'Each in its own window, in the Start menu and the taskbar, with Windows notifications.',
      e3_t: 'A built-in store', e3_d: 'Thousands of free apps from F-Droid, a click away.',
      e4_t: 'Google Play (experimental)', e4_d: 'Sign in with Google, and set up Google\'s own Play services and Play Store.',
      e5_t: 'Apps built for phones\' processors', e5_d: 'ARM-only apps are translated for your PC in the background.',
      apps_title: 'Israeli apps on Win.apk',
      apps_sub: 'Real screenshots, taken in Win.apk on Windows.',
      cap_waze: 'Waze', cap_moovit: 'Moovit', cap_maccabi: 'Maccabi', cap_hapoalim: 'Bank Hapoalim',
      alt_waze: 'Waze in its own window on Windows, asking in Hebrew for the precise location',
      alt_moovit: 'Moovit\'s home screen in Hebrew, in its own window on Windows',
      alt_maccabi: 'The Maccabi app\'s start screen in Hebrew, with its sign-in button',
      alt_hapoalim: 'Bank Hapoalim\'s app in Hebrew, with its sign-in button',
      apps_note: 'Not every app works yet. Check yours before you rely on it.',
      compat_link: 'Compatibility table',
      shots_title: 'The program',
      shot_library: 'Your apps, in the library', shot_store: 'The built-in F-Droid store', shot_dark: 'Dark theme',
      alt_library: 'The Win.apk library with installed Android apps',
      alt_store: 'The Store tab, listing free apps from F-Droid',
      alt_dark: 'The Win.apk library in the dark theme',
      start_title: 'Get started in three steps',
      s1_t: 'Download', s1_d: 'Win.apk-Setup.exe, with the button.',
      s2_t: 'Install', s2_d: 'If SmartScreen warns (the preview isn\'t signed yet), click More info, then Run anyway.',
      s3_t: 'Add apps', s3_d: 'From the Store, or double-click an APK you downloaded.',
      req_title: 'You need:',
      req: 'Windows 10 (1903 or later) or 11, 64-bit, Intel or AMD; about 500 MB of disk; Direct3D 11. No administrator rights.',
      guide_link: 'Step-by-step guide',
      faq_title: 'Questions',
      q1: 'Is it an emulator?',
      a1: 'No. An emulator boots a whole Android phone inside your PC. Win.apk runs each app directly on Windows, recreating only what the app needs, so there is nothing to boot and no virtualization to turn on.',
      q2: 'Is it free? Is it open source?',
      a2: 'Yes to both. Win.apk is free, with no ads, and its source code is on GitHub under the AGPL-3.0 license.',
      q3: 'Which apps work?',
      a3: 'Many, and more with every release; some only in part, some not yet. The compatibility table lists what has been tried.',
      q4: 'Is it a Google Play certified device?',
      a4: 'No, and it won\'t pretend to be. Some Google apps may say the device isn\'t certified; you can register Win.apk\'s device ID with Google (Settings > Google account shows it), as on any uncertified device.',
      google_link: 'Google services',
      q5: 'Is it safe? Why does Windows warn?',
      a5: 'The preview installer isn\'t code-signed yet, so SmartScreen doesn\'t know it. It needs no administrator rights and installs only for you. Apps ask for permissions as on Android.',
      faq_link: 'More questions',
      f_github: 'GitHub', f_license: 'License: AGPL-3.0', f_report: 'Report a problem', f_trouble: 'Troubleshooting',
      f_marks: 'Android is a trademark of Google LLC. App names and screenshots belong to their owners; Win.apk is not affiliated with them.'
    },
    he: {
      title: 'Win.apk: אפליקציות אנדרואיד ב-Windows',
      description: 'מריצים אפליקציות אנדרואיד ב-Windows כמו כל תוכנה אחרת: חלון משלהן, קיצור דרך בתפריט התחל והתראות. בלי אמולטור ובלי מכונה וירטואלית. חינם וקוד פתוח.',
      skip: 'דילוג לתוכן', nav_label: 'חלקי העמוד',
      nav_examples: 'מה הוא עושה', nav_apps: 'אפליקציות ישראליות', nav_start: 'איך מתחילים', nav_faq: 'שאלות',
      lang_switch: 'English', lang_label: 'English: הצגת העמוד באנגלית',
      badge: 'חינם · קוד פתוח · גרסת תצוגה',
      hero_title: 'אפליקציות אנדרואיד במחשב, כמו כל תוכנה אחרת',
      hero_sub: 'בלי אמולטור ובלי מכונה וירטואלית. לכל אפליקציה חלון משלה, קיצור דרך בתפריט התחל והתראות.',
      download: 'הורדה ל-Windows',
      meta_default: 'Windows 10 ו-11 · ‏64 ביט · חינם',
      meta_req: 'Windows 10 ו-11, ‏64 ביט', meta_pre: 'גרסת תצוגה',
      all_releases: 'כל הגרסאות', portable: 'גרסה ניידת (ZIP)', whats_new: 'מה חדש',
      ex_title: 'מה אפשר לעשות',
      e1_t: 'התקנת APK בלחיצה כפולה', e1_d: 'APK,‏ APKM,‏ XAPK או APKS: פותחים את הקובץ, או גוררים אותו לחלון.',
      e2_t: 'אפליקציות כמו תוכנות Windows', e2_d: 'כל אחת בחלון משלה, בתפריט התחל ובשורת המשימות, עם התראות של Windows.',
      e3_t: 'חנות מובנית', e3_d: 'אלפי אפליקציות חינמיות מ-F-Droid, בלחיצה.',
      e4_t: 'Google Play (ניסיוני)', e4_d: 'כניסה עם חשבון Google, והגדרה של שירותי Play וחנות Play המקוריים של Google.',
      e5_t: 'אפליקציות למעבדים של טלפונים', e5_d: 'אפליקציות שנבנו רק ל-ARM מתורגמות למחשב שלכם ברקע.',
      apps_title: 'אפליקציות ישראליות ב-Win.apk',
      apps_sub: 'צילומי מסך אמיתיים, שצולמו ב-Win.apk על Windows.',
      cap_waze: 'וייז', cap_moovit: 'מוביט', cap_maccabi: 'מכבי', cap_hapoalim: 'בנק הפועלים',
      alt_waze: 'וייז בחלון משלו ב-Windows, מבקש בעברית גישה למיקום המדויק',
      alt_moovit: 'מסך הבית של מוביט בעברית, בחלון משלו ב-Windows',
      alt_maccabi: 'מסך הפתיחה של אפליקציית מכבי בעברית, עם כפתור הכניסה',
      alt_hapoalim: 'אפליקציית בנק הפועלים בעברית, עם כפתור הכניסה לחשבון',
      apps_note: 'עוד לא כל אפליקציה עובדת. כדאי לבדוק את שלכם לפני שסומכים עליה.',
      compat_link: 'טבלת התאימות',
      shots_title: 'התוכנה',
      shot_library: 'האפליקציות שלכם, בספרייה', shot_store: 'חנות F-Droid המובנית', shot_dark: 'מצב כהה',
      alt_library: 'הספרייה של Win.apk עם אפליקציות אנדרואיד מותקנות',
      alt_store: 'לשונית החנות, עם אפליקציות חינמיות מ-F-Droid',
      alt_dark: 'הספרייה של Win.apk במצב כהה',
      start_title: 'מתחילים בשלושה צעדים',
      s1_t: 'מורידים', s1_d: 'את Win.apk-Setup.exe, בכפתור ההורדה.',
      s2_t: 'מתקינים', s2_d: 'אם SmartScreen מזהיר (גרסת התצוגה עוד לא חתומה), לוחצים "מידע נוסף" ואז "הפעל בכל זאת".',
      s3_t: 'מוסיפים אפליקציות', s3_d: 'מהחנות, או בלחיצה כפולה על קובץ APK שהורדתם.',
      req_title: 'מה צריך:',
      req: 'Windows 10 (גרסה 1903 ומעלה) או 11, ‏64 ביט, מעבד Intel או AMD; כ-500MB בדיסק; Direct3D 11. לא צריך הרשאות מנהל.',
      guide_link: 'מדריך צעד אחר צעד',
      faq_title: 'שאלות',
      q1: 'זה אמולטור?',
      a1: 'לא. אמולטור מפעיל טלפון אנדרואיד שלם בתוך המחשב. Win.apk מריץ כל אפליקציה ישירות על Windows ובונה רק את מה שהיא צריכה, כך שאין מה לאתחל ואין וירטואליזציה להפעיל.',
      q2: 'זה בחינם? זה קוד פתוח?',
      a2: 'כן ושוב כן. Win.apk חינמי, בלי פרסומות, וקוד המקור שלו פתוח ב-GitHub ברישיון AGPL-3.0.',
      q3: 'אילו אפליקציות עובדות?',
      a3: 'הרבה, ועוד עם כל גרסה; חלקן רק בחלקן, וחלקן עוד לא. בטבלת התאימות רשום מה נבדק.',
      q4: 'המכשיר מאושר על ידי Google Play?',
      a4: 'לא, והוא גם לא מתחזה לאחד כזה. חלק מאפליקציות Google עלולות לכתוב שהמכשיר לא מאושר; אפשר לרשום אצל Google את מזהה המכשיר של Win.apk (מופיע בהגדרות > חשבון Google), כמו בכל מכשיר לא מאושר.',
      google_link: 'שירותי Google',
      q5: 'זה בטוח? למה Windows מזהיר?',
      a5: 'קובץ ההתקנה של גרסת התצוגה עוד לא חתום, ולכן SmartScreen לא מכיר אותו. הוא לא צריך הרשאות מנהל ומותקן רק למשתמש שלכם. האפליקציות מבקשות הרשאות כמו באנדרואיד.',
      faq_link: 'עוד שאלות',
      f_github: 'GitHub', f_license: 'רישיון: AGPL-3.0', f_report: 'דיווח על בעיה', f_trouble: 'פתרון בעיות',
      f_marks: 'Android הוא סימן מסחרי של Google LLC. שמות האפליקציות וצילומי המסך שייכים לבעליהם; ל-Win.apk אין קשר אליהם.'
    }
  };
  // A link to a document goes to its Hebrew page where one exists.
  var DOCS = {
    en: { start: 'docs/getting-started.md', compat: 'docs/APPS.md', faq: 'docs/faq.md', trouble: 'docs/troubleshooting.md', google: 'docs/google-services.md' },
    he: { start: 'docs/he/getting-started.md', compat: 'docs/APPS.md', faq: 'docs/he/faq.md', trouble: 'docs/he/troubleshooting.md', google: 'docs/he/google-services.md' }
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
    document.querySelectorAll('img[data-shot]').forEach(function (el) {
      var name = el.dataset.shot;
      el.onerror = function () {
        el.onerror = null;
        var fig = el.closest('figure');
        if (fig) fig.hidden = true;
        var sec = el.closest('section');
        if (sec && !sec.querySelector('figure:not([hidden])')) sec.hidden = true;
      };
      var want = (lang === 'he' && heShots.indexOf(name) >= 0 ? 'images/he/' : 'images/') + name;
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

  function point(c) {
    if (!c) return;
    release = c;
    document.querySelectorAll('[data-dl=setup]').forEach(function (el) { el.href = c.setup; });
    document.querySelectorAll('[data-dl=notes]').forEach(function (el) { el.href = c.notes; });
    if (c.zip) document.querySelectorAll('[data-dl=zip]').forEach(function (el) { el.href = c.zip; });
    meta();
  }

  var cached = store('sessionStorage', 'winapk-release');
  if (cached) {
    try { point(JSON.parse(cached)); return; } catch (e) {}
  }
  if (!window.fetch) return;
  fetch('https://api.github.com/repos/' + REPO + '/releases?per_page=30', { headers: { Accept: 'application/vnd.github+json' } })
    .then(function (r) { return r.ok ? r.json() : []; })
    .then(function (list) {
      var c = choose(Array.isArray(list) ? list : []);
      if (c) store('sessionStorage', 'winapk-release', JSON.stringify(c));
      point(c);
    })
    .catch(function () { /* The buttons keep the releases page. */ });
})();
