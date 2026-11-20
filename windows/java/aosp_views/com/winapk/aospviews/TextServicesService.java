// ITextServicesManager as TextServicesManagerService answers an app when no spell checker is
// enabled (docs/research/aosp-views.md, step V2): no current spell checker or subtype, spell
// checking off, none enabled, and a session request that never binds (the service only calls the
// listener back once a spell checker's service connects). An EditText asks this when it gets
// focus (TextView.updateTextServicesLocaleLocked) and when it starts its SpellChecker; the host
// has no spell checker to offer yet.
package com.winapk.aospviews;

import android.os.Bundle;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.SpellCheckerSubtype;

import com.android.internal.textservice.ISpellCheckerSessionListener;
import com.android.internal.textservice.ITextServicesSessionListener;

public final class TextServicesService {
    public SpellCheckerInfo getCurrentSpellChecker(int userId, String locale) { return null; }

    public SpellCheckerSubtype getCurrentSpellCheckerSubtype(int userId, boolean allowImplicitlySelectedSubtype) {
        return null;
    }

    public void getSpellCheckerService(int userId, String sciId, String locale, ITextServicesSessionListener tsListener,
            ISpellCheckerSessionListener scListener, Bundle bundle, int supportedAttributes) {}

    public void finishSpellCheckerService(int userId, ISpellCheckerSessionListener listener) {}

    public boolean isSpellCheckerEnabled(int userId) { return false; }

    public SpellCheckerInfo[] getEnabledSpellCheckers(int userId) { return null; }
}
