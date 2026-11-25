# Apps running in Win.apk

Real screenshots of apps running in Win.apk, shown in the web page's "Israeli apps" gallery (`site/index.html`,
`.github/workflows/pages.yml` copies this folder to `images/apps/`). Only unedited screenshots of an app working:
no error, force-update or blank screens, no mock-ups, no logos. Cropping to the app's window is fine.

| File | App | What it shows | Taken |
|---|---|---|---|
| `waze.png` | Waze (`com.waze`) | The location request, in Hebrew, in its own window | On Windows 11, November 2026 |
| `moovit.png` | Moovit (`com.tranzmate`) | The home screen, in Hebrew, in its own window | On Windows 11, November 2026 |
| `maccabi.png` | Maccabi (`com.ideomobile.maccabi`) | The start screen, in Hebrew (the app's frame) | Win.apk's own frame capture |
| `hapoalim.png` | Bank Hapoalim (`com.ideomobile.hapoalim`) | The sign-in screen, in Hebrew (the app's frame) | Win.apk's own frame capture |

To add one: put `<name>.png` here (a phone-shaped window, about 360 px wide), add a `<figure>` for it to the gallery
in `site/index.html` and its caption to both languages in `site/site.js`. A different screenshot for visitors reading
in Hebrew may go in `docs/images/he/apps/<same name>`; without one they see this folder's.
