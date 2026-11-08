# Frequently asked questions

**🇮🇱 [לדף הזה בעברית — לחצו כאן](he/faq.md)**

**Is Win.apk an emulator?**
No. No Android system runs: each app runs as its own Windows program, on a
re-creation of what Android apps need. [How it works](how-it-works.md)

**Is it free? Is it safe?**
It is free and open source (AGPL-3.0). The preview builds are not code-signed
yet, so Windows warns when you run the installer. Apps run with your Windows
account's rights and there is no sandbox between them, so install only apps
you trust. Win.apk has no telemetry.

**Which apps work?**
In the last automated run, 90 of 92 open-source F-Droid apps of every kind
started and drew their first screen. That says they start, not that every
screen works, and big commercial apps have been tried far less. [Compatibility](APPS.md) lists what works and what does not.

**Where do I get apps?**
The built-in Store (F-Droid) is the easiest. Files from APKMirror, APKPure or
an app's own site work too: `.apk`, `.apkm`, `.xapk` and `.apks`. Google Play
works experimentally ([Google services](google-services.md)).

**Which file should I download from APKMirror?**
The `x86_64` variant, or `noarch`; else `arm64-v8a`. Never one that has only
`armeabi-v7a`. [Which build to install](arm-apps.md#which-build-to-install)

**Why is an app slow the first time?**
Its code is converted once on install. An app with only ARM code also runs on
a slower JIT until its translation for your processor is ready.

**Can I sign in with my Google account?**
Yes: Settings > Google account. The sign-in happens on Google's own page;
Win.apk never sees your password.

**Do in-app purchases work? Banking apps?**
No. In-app purchases and Play Integrity are not available, and Win.apk will
not fake them. Apps that require Play Integrity (many banking and payment apps)
will refuse to run or will limit what they do.

**Do games work?**
Many do: games on OpenGL ES (libGDX, SDL, most engines) draw through ANGLE,
and Xbox controllers work. Games that need Vulkan, OpenSL ES sound or tilt
sensors do not yet. Multi-finger gestures cannot be made with a mouse.

**Does Win.apk need Hyper-V or virtualization?**
No. Nothing to turn on in the BIOS or in Windows Features, and it can run next
to other virtual machines.

**Does it run on Windows on ARM, 32-bit Windows, or Windows 7/8?**
It needs 64-bit Windows 10 (1903 or later) or 11 on an Intel or AMD processor.
Windows on ARM has not been tried.

**Where are my apps' files? Can apps see my files?**
Each app's private data is in `%LOCALAPPDATA%\Win.apk\android`. The shared
folders an app sees (Download, Pictures, Documents, Music, Movies) are your
own Windows folders, behind the storage permission; Settings > Apps > Use my
Windows folders turns that off.

**How do I report a problem?**
Use **Report a problem** in Win.apk, or open an issue with the app's log.
[Troubleshooting](troubleshooting.md#reporting-a-problem). Hebrew is welcome.

**What is the relation to Mac.apk?**
Win.apk is an unofficial port of [Mac.apk](https://github.com/kksimp/Mac.apk)
by Kaleb Simpson, which runs Android apps on Apple Silicon Macs. Its Android
framework comes from there. Win.apk is not affiliated with or endorsed by the
Mac.apk project; report Win.apk problems here, not there.

**How can I help?**
Try apps and report what you find, add a row to the
[compatibility list](APPS.md#adding-a-row), or send code
([CONTRIBUTING.md](../CONTRIBUTING.md)). A star on GitHub helps others find
the project.
