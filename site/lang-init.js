(function () {
  var lang = 'en';
  try {
    var q = new URLSearchParams(location.search).get('lang');
    if (q === 'he' || q === 'en') {
      lang = q;
    } else {
      var s = localStorage.getItem('winapk-lang');
      if (s === 'he' || s === 'en') {
        lang = s;
      } else {
        var langs = navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language || 'en'];
        for (var i = 0; i < langs.length; i++) {
          var l = String(langs[i]).toLowerCase();
          if (l.indexOf('he') === 0 || l.indexOf('iw') === 0) { lang = 'he'; break; }
          if (l.indexOf('en') === 0) { lang = 'en'; break; }
        }
      }
    }
  } catch (e) {}
  var root = document.documentElement;
  root.lang = lang;
  root.dir = lang === 'he' ? 'rtl' : 'ltr';
})();
