package com.macapk.loader;

import android.app.Application;
import android.app.LocaleConfig;
import android.app.LocaleManager;
import android.os.LocaleList;

import com.macapk.loader.res.ResBackend;
import java.util.ArrayList;
import java.util.List;

/**
 * Backs the runtime menu bar's Language menu. "System Default" clears the app's
 * application locales so the effective list is the global Mac.apk override or,
 * absent that, macOS's preferred languages (LocaleList.macapkGetSystemLocales).
 * Any other pick is LocaleManager.setApplicationLocales, the same call Android 13+
 * Settings > App languages makes, so the app sees a normal Configuration/LocaleList
 * change and re-resolves its resources.
 *
 * Offered languages: the manifest's android:localeConfig when declared (what AOSP's
 * own picker lists), else the locales the app ships resources for.
 */
public final class AppLanguages {
    private AppLanguages() {}

    /** BCP-47 tags to offer, sorted; empty when the app is single-language. */
    public static String[] supportedTags() {
        List<String> out = new ArrayList<>();
        try {
            Application app = com.macapk.loader.app.Apps.current();
            if (app != null) {
                LocaleList declared = new LocaleConfig(app).getSupportedLocales();
                if (declared != null && declared.size() > 0) {
                    for (int i = 0; i < declared.size(); i++) out.add(declared.get(i).toLanguageTag());
                }
            }
            if (out.isEmpty()) {
                Object index = ResBackend.get().resourceIndex();
                if (ResBackend.get().isIndex(index)) {
                    out.addAll(ResBackend.get().indexResourceLocaleTags(index));
                }
            }
        } catch (Throwable t) {
            // The menu degrades to "System Default" only.
        }
        java.util.Collections.sort(out);
        return out.toArray(new String[0]);
    }

    /** The app's chosen tag, or "" for System Default. */
    public static String selectedTag() {
        try {
            return new LocaleManager().getApplicationLocales().toLanguageTags();
        } catch (Throwable t) {
            return "";
        }
    }

    /** Null or "" selects System Default. */
    public static void select(String tag) {
        try {
            LocaleList list = (tag == null || tag.isEmpty())
                    ? LocaleList.getEmptyLocaleList() : LocaleList.forLanguageTags(tag);
            new LocaleManager().setApplicationLocales(list);
        } catch (Throwable t) {
            NativeLog.w("AppLanguages", "select(" + tag + ") failed: " + t);
        }
    }
}
