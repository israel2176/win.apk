// NotificationActionsTest -- what HostNotifications makes of a notification's actions for the host's
// buttons (windows/host/toast_xml.h): which are shown, which have a text field, and the intent an
// action is sent with when the user typed a reply. A plain JVM, no host native.
//
//   javac -cp <framework jars> -d out windows/test/java/com/macapk/loader/NotificationActionsTest.java
//   MACAPK_LOG_LEVEL=7 java -cp <framework jars>:out com.macapk.loader.NotificationActionsTest
package com.macapk.loader;

import android.app.Notification;
import android.app.PendingIntent;
import android.app.RemoteInput;
import android.content.Intent;
import android.os.Bundle;

import java.util.List;

public final class NotificationActionsTest {
    private static int failures;

    private static void check(String what, boolean ok, Object got) {
        System.out.println("  [" + (ok ? "PASS" : "FAIL") + "] " + what + (ok ? "" : ": " + got));
        if (!ok) failures++;
    }

    private static PendingIntent intent(String action) {
        return PendingIntent.getBroadcast(null, 0, new Intent(action), PendingIntent.FLAG_MUTABLE);
    }

    public static void main(String[] args) {
        RemoteInput input = new RemoteInput.Builder("key_text_reply").setLabel("Message").build();
        Notification n = new Notification();
        n.actions = new Notification.Action[] {
            new Notification.Action.Builder(0, "Suggested", intent("s")).setContextual(true).build(),
            new Notification.Action.Builder(0, "Reply", intent("r")).addRemoteInput(input).build(),
            new Notification.Action.Builder(0, "", intent("e")).build(),
            new Notification.Action.Builder(0, "No intent", null).build(),
            new Notification.Action.Builder(0, "Mark as read", intent("m")).build(),
            new Notification.Action.Builder(0, "Archive", intent("a")).build(),
            new Notification.Action.Builder(0, "Fourth", intent("f")).build(),
        };
        List<Notification.Action> shown = HostNotifications.shownActions(n);
        check("three shown, as in the shade", shown.size() == 3, shown.size());
        check("not the contextual one, nor one with no title or intent, in order",
                shown.size() == 3 && "Reply".contentEquals(shown.get(0).title)
                        && "Mark as read".contentEquals(shown.get(1).title)
                        && "Archive".contentEquals(shown.get(2).title), shown);
        check("the reply action has a field", HostNotifications.freeFormInput(shown.get(0)) == input, null);
        check("the others none", HostNotifications.freeFormInput(shown.get(1)) == null, null);
        RemoteInput choices = new RemoteInput.Builder("pick").setAllowFreeFormInput(false)
                .setChoices(new CharSequence[] {"Yes", "No"}).build();
        Notification.Action choiceOnly = new Notification.Action.Builder(0, "Answer", intent("c"))
                .addRemoteInput(choices).build();
        check("choices without free-form text: no field", HostNotifications.freeFormInput(choiceOnly) == null, null);

        Intent fill = HostNotifications.replyIntent(shown.get(0), "On my way");
        Bundle results = RemoteInput.getResultsFromIntent(fill);
        check("the reply under the RemoteInput's key, where getResultsFromIntent finds it",
                results != null && "On my way".contentEquals(results.getCharSequence("key_text_reply")), results);
        check("no reply, no fill-in", HostNotifications.replyIntent(shown.get(0), null) == null, null);
        check("an action without a field adds nothing", HostNotifications.replyIntent(shown.get(1), "x") == null, null);

        Notification none = new Notification();
        check("no actions: none shown", HostNotifications.shownActions(none).isEmpty(), null);

        System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
