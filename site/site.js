// The site's texts in both languages and the language choice: ?lang= first, then the visitor's own pick (the
// switch in the header), then the browser's languages; Hebrew lays the page out right to left.
(function () {
  var GH = 'https://github.com/israel2176/win.apk';
  var T = {
    en: {
      title: 'Win.apk: run Android apps on Windows',
      description: 'Run Android apps on Windows like any other program: their own window, Start menu entry and notifications. No emulator, no virtual machine. Free and open source.',
      nav_features: 'Features', nav_how: 'Compared with an emulator', nav_install: 'Download',
      star: 'GitHub', lang_switch: 'עברית', lang_label: 'Switch to Hebrew',
      badge: 'Free · Open source · Public preview',
      hero_title: 'Android apps on your Windows PC, like any other program',
      hero_sub: 'No emulator and no virtual machine. Every app opens in its own window, with its own Start menu entry, taskbar icon and Windows notifications.',
      download: 'Download for Windows',
      download_small: 'Windows 10 and 11 · 64-bit · Free',
      source: 'View on GitHub',
      portable: 'Portable ZIP', all_releases: 'All releases', whats_new: 'What\'s new',
      stat1_n: '1', stat1: 'window per app, with its own taskbar icon',
      stat2_n: '0', stat2: 'emulators or virtual machines',
      stat3_n: '4', stat3: 'package formats: APK, APKM, XAPK, APKS',
      features_title: 'What you get',
      f1_t: 'Apps that feel like Windows programs', f1_d: 'Each app gets its own resizable window, a Start menu shortcut and a taskbar icon. Its notifications appear as Windows notifications.',
      f2_t: 'Install with a double-click', f2_d: 'Open an APK file, or a split bundle (APKM, XAPK or APKS), or drag it onto the window. Or pick from the built-in F-Droid store.',
      f3_t: 'Uses your PC\'s hardware', f3_d: 'Webcam, microphone, location, paired Bluetooth devices, Xbox controllers, the clipboard, and your Downloads and Pictures folders.',
      f4_t: 'ARM-only apps too', f4_d: 'Apps built only for phone processors are translated for your PC in the background, at the lowest priority.',
      f5_t: 'Sign in with Google', f5_d: 'Google sign-in is built in. Google\'s own Play services and Play Store can be set up too (experimental).',
      f6_t: 'English and Hebrew', f6_d: 'Light and dark themes. In Hebrew the whole program, its installer included, is laid out right to left.',
      how_title: 'How is it different from an emulator?',
      how_sub: 'An emulator starts a whole Android phone inside your PC and runs apps in it. Win.apk recreates only what each app needs, on top of Windows itself.',
      col_emu: 'Emulator', col_win: 'Win.apk',
      r1: 'Starting up', r1_e: 'Boots a whole Android system', r1_w: 'Nothing to boot; the app just opens',
      r2: 'Memory in the background', r2_e: 'Android keeps running', r2_w: 'Only the apps you have open',
      r3: 'Virtualization (Hyper-V, BIOS)', r3_e: 'Usually required', r3_w: 'Not needed; no administrator rights either',
      r4: 'Windows', r4_e: 'Every app inside one big window', r4_w: 'A window per app: Alt+Tab, Snap, the taskbar',
      r5: 'Hardware', r5_e: 'Emulated devices', r5_w: 'Your PC\'s own, through Windows',
      r6: 'How many apps run', r6_e: 'More, today', r6_w: 'Many, and growing: see the compatibility table',
      honest: 'To be fair: an emulator runs more apps today. Win.apk is young; check whether your app works before you rely on it.',
      compat_link: 'Compatibility table',
      shots_title: 'A look inside',
      shot_library: 'Your apps, in the library', shot_store: 'The built-in F-Droid store', shot_dark: 'Dark theme',
      install_title: 'Get started in three steps',
      s1_t: 'Download', s1_d: 'Win.apk-Setup.exe, with the button above.',
      s2_t: 'Install', s2_d: 'Windows SmartScreen may warn, because the preview isn\'t signed yet: click More info, then Run anyway.',
      s3_t: 'Add apps', s3_d: 'Install one from the Store, or double-click an APK you downloaded.',
      req_title: 'You need',
      req: 'Windows 10 (version 1903 or later) or Windows 11, 64-bit, with an Intel or AMD processor; about 500 MB of disk space; a graphics card with Direct3D 11. No administrator rights.',
      preview: 'Public preview: many apps run well, some only in part, some not yet.',
      d_start: 'Getting started', d_compat: 'Compatibility', d_faq: 'FAQ', d_trouble: 'Troubleshooting', d_report: 'Report a problem',
      footer_1: 'Free and open-source software. An unofficial Windows port of', license: 'License: AGPL-3.0',
      footer_2: 'Android is a trademark of Google LLC. Win.apk is not affiliated with Google.'
    },
    he: {
      title: 'Win.apk: אפליקציות אנדרואיד ב-Windows',
      description: 'מריצים אפליקציות אנדרואיד ב-Windows כמו כל תוכנה אחרת: חלון משלהן, קיצור דרך בתפריט התחל והתראות. בלי אמולטור ובלי מכונה וירטואלית. חינם וקוד פתוח.',
      nav_features: 'מה יש בו', nav_how: 'לעומת אמולטור', nav_install: 'הורדה',
      star: 'GitHub', lang_switch: 'English', lang_label: 'Switch to English',
      badge: 'חינם · קוד פתוח · גרסת תצוגה',
      hero_title: 'אפליקציות אנדרואיד במחשב, כמו כל תוכנה אחרת',
      hero_sub: 'בלי אמולטור ובלי מכונה וירטואלית. כל אפליקציה נפתחת בחלון משלה, עם קיצור דרך בתפריט התחל, אייקון בשורת המשימות והתראות של Windows.',
      download: 'הורדה ל-Windows',
      download_small: 'Windows 10 ו-11 · 64 ביט · חינם',
      source: 'לעמוד ב-GitHub',
      portable: 'גרסה ניידת (ZIP)', all_releases: 'כל הגרסאות', whats_new: 'מה חדש',
      stat1_n: '1', stat1: 'חלון לכל אפליקציה, עם אייקון משלה בשורת המשימות',
      stat2_n: '0', stat2: 'אמולטורים או מכונות וירטואליות',
      stat3_n: '4', stat3: 'סוגי קבצים: APK, APKM, XAPK, APKS',
      features_title: 'מה מקבלים',
      f1_t: 'אפליקציות שמרגישות כמו תוכנות Windows', f1_d: 'לכל אפליקציה חלון משלה שאפשר לשנות את גודלו, קיצור דרך בתפריט התחל ואייקון בשורת המשימות. ההתראות שלה מופיעות כהתראות של Windows.',
      f2_t: 'התקנה בלחיצה כפולה', f2_d: 'פותחים קובץ APK, או חבילה מפוצלת מסוג APKM, XAPK או APKS, או גוררים אותו לחלון. או בוחרים מחנות F-Droid המובנית.',
      f3_t: 'משתמש בחומרה של המחשב', f3_d: 'מצלמת רשת, מיקרופון, מיקום, התקני Bluetooth מצומדים, בקרי Xbox, העתק-הדבק, ותיקיות ההורדות והתמונות שלכם.',
      f4_t: 'גם אפליקציות ARM', f4_d: 'אפליקציות שנבנו רק למעבדים של טלפונים מתורגמות למחשב שלכם ברקע, בעדיפות הנמוכה ביותר.',
      f5_t: 'כניסה עם Google', f5_d: 'כניסה עם חשבון Google מובנית. אפשר גם להגדיר את שירותי Play וחנות Play המקוריים של Google (ניסיוני).',
      f6_t: 'עברית ואנגלית', f6_d: 'מצב בהיר וכהה. בעברית כל התוכנה, כולל ההתקנה, מסודרת מימין לשמאל.',
      how_title: 'במה זה שונה מאמולטור?',
      how_sub: 'אמולטור מפעיל טלפון אנדרואיד שלם בתוך המחשב ומריץ בו את האפליקציות. Win.apk בונה רק את מה שכל אפליקציה צריכה, על גבי Windows עצמו.',
      col_emu: 'אמולטור', col_win: 'Win.apk',
      r1: 'הפעלה', r1_e: 'מאתחל מערכת אנדרואיד שלמה', r1_w: 'אין מה לאתחל, האפליקציה פשוט נפתחת',
      r2: 'זיכרון ברקע', r2_e: 'אנדרואיד ממשיך לרוץ', r2_w: 'רק האפליקציות שפתוחות',
      r3: 'וירטואליזציה (Hyper-V, BIOS)', r3_e: 'בדרך כלל נדרשת', r3_w: 'לא צריך, וגם לא הרשאות מנהל',
      r4: 'חלונות', r4_e: 'כל האפליקציות בתוך חלון אחד גדול', r4_w: 'חלון לכל אפליקציה: Alt+Tab, הצמדה, שורת המשימות',
      r5: 'חומרה', r5_e: 'התקנים מדומים', r5_w: 'החומרה של המחשב עצמו, דרך Windows',
      r6: 'כמה אפליקציות עובדות', r6_e: 'יותר, נכון להיום', r6_w: 'הרבה, והרשימה גדלה: ראו את טבלת התאימות',
      honest: 'ובהגינות: אמולטור מריץ היום יותר אפליקציות. Win.apk עוד צעיר, וכדאי לבדוק שהאפליקציה שלכם עובדת לפני שסומכים עליה.',
      compat_link: 'טבלת התאימות',
      shots_title: 'הצצה פנימה',
      shot_library: 'האפליקציות שלכם, בספרייה', shot_store: 'חנות F-Droid המובנית', shot_dark: 'מצב כהה',
      install_title: 'מתחילים בשלושה צעדים',
      s1_t: 'מורידים', s1_d: 'את Win.apk-Setup.exe, בכפתור למעלה.',
      s2_t: 'מתקינים', s2_d: 'ייתכן ש-SmartScreen יזהיר, כי גרסת התצוגה עוד לא חתומה: לוחצים "מידע נוסף" ואז "הפעל בכל זאת".',
      s3_t: 'מוסיפים אפליקציות', s3_d: 'מהחנות המובנית, או בלחיצה כפולה על קובץ APK שהורדתם.',
      req_title: 'מה צריך',
      req: 'Windows 10 (גרסה 1903 ומעלה) או Windows 11, ‏64 ביט, עם מעבד Intel או AMD; כ-500MB פנויים בדיסק; כרטיס גרפי עם Direct3D 11. לא צריך הרשאות מנהל.',
      preview: 'גרסת תצוגה ציבורית: הרבה אפליקציות עובדות טוב, חלקן רק בחלקן, וחלקן עוד לא.',
      d_start: 'מדריך התחלה', d_compat: 'תאימות', d_faq: 'שאלות נפוצות', d_trouble: 'פתרון בעיות', d_report: 'דיווח על בעיה',
      footer_1: 'תוכנה חופשית בקוד פתוח. הסבה לא רשמית ל-Windows של', license: 'רישיון: AGPL-3.0',
      footer_2: 'Android הוא סימן מסחרי של Google LLC. ‏Win.apk אינו קשור ל-Google.'
    }
  };
  // A link to a document goes to its Hebrew page where one exists.
  var DOCS = {
    en: { d_start: 'docs/getting-started.md', d_compat: 'docs/APPS.md', d_faq: 'docs/faq.md', d_trouble: 'docs/troubleshooting.md' },
    he: { d_start: 'docs/he/getting-started.md', d_compat: 'docs/APPS.md', d_faq: 'docs/he/faq.md', d_trouble: 'docs/he/troubleshooting.md' }
  };

  // Storage can throw (private windows, blocked site data); the page works without it.
  function saved(v) {
    try { if (v) localStorage.setItem('winapk-lang', v); else return localStorage.getItem('winapk-lang'); } catch (e) {}
    return null;
  }

  function pick() {
    var q = new URLSearchParams(location.search).get('lang');
    if (q === 'he' || q === 'en') return q;
    var s = saved();
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

  function apply() {
    var t = T[lang];
    var root = document.documentElement;
    root.lang = lang;
    root.dir = lang === 'he' ? 'rtl' : 'ltr';
    document.title = t.title;
    var meta = document.querySelector('meta[name=description]');
    if (meta) meta.content = t.description;
    document.querySelectorAll('[data-i18n]').forEach(function (el) { el.textContent = t[el.dataset.i18n]; });
    document.querySelectorAll('[data-i18n-label]').forEach(function (el) { el.setAttribute('aria-label', t[el.dataset.i18nLabel]); });
    document.querySelectorAll('img[data-shot]').forEach(function (el) {
      var name = el.dataset.shot;
      el.onerror = lang === 'he' ? function () { el.onerror = null; el.src = 'images/' + name; } : null;
      el.src = (lang === 'he' ? 'images/he/' : 'images/') + name;
    });
    document.querySelectorAll('[data-doc]').forEach(function (el) { el.href = GH + '/blob/main/' + DOCS[lang][el.dataset.doc]; });
  }

  document.addEventListener('click', function (e) {
    if (!e.target.closest('[data-lang-toggle]')) return;
    lang = lang === 'he' ? 'en' : 'he';
    saved(lang);
    apply();
  });

  apply();
})();
