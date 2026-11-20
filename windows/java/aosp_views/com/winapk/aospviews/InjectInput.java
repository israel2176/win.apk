// MACAPK_INJECT_FILE's input verbs with AOSP's views (TouchDispatcher.macapkDispatchInjectLine
// asks ViewHost.injectInput first): a harness's taps, swipes, keys and text, made by AospInput as
// the host's own would be and routed to the windows the same way, so a scripted check crosses the
// same path a user's input does. Surface pixels, as the driver's other verbs take them. Runs on
// the inject driver's thread (its sleeps are a finger's hold); the events go to the main looper.
// Verbs that post real host input (hosttap, hostswipe, hostwheel...) and sleep are left to the
// driver: they come back through AospInput's host entries.
package com.winapk.aospviews;

import android.view.KeyEvent;
import android.view.MotionEvent;

final class InjectInput {
    private InjectInput() {}

    static boolean run(String verb, String[] p, String line) {
        AospInput in = AospInput.sInstance;
        if (in == null) return false;
        try {
            switch (verb) {
                case "tap":
                case "hosttappx": {
                    float x = Float.parseFloat(p[1]), y = Float.parseFloat(p[2]);
                    long hold = p.length >= 4 && verb.equals("hosttappx") ? Long.parseLong(p[3]) : 60L;
                    in.touchAt(MotionEvent.ACTION_DOWN, x, y);
                    Thread.sleep(Math.max(0L, hold));
                    in.touchAt(MotionEvent.ACTION_UP, x, y);
                    return true;
                }
                case "swipe": {
                    float x0 = Float.parseFloat(p[1]), y0 = Float.parseFloat(p[2]);
                    float x1 = Float.parseFloat(p[3]), y1 = Float.parseFloat(p[4]);
                    long ms = Math.max(32L, Math.min(p.length >= 6 ? Long.parseLong(p[5]) : 150L, 2000L));
                    int steps = (int) Math.max(2L, ms / 16L);
                    in.touchAt(MotionEvent.ACTION_DOWN, x0, y0);
                    for (int i = 1; i <= steps; i++) {
                        Thread.sleep(ms / steps);
                        in.touchAt(MotionEvent.ACTION_MOVE, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
                    }
                    in.touchAt(MotionEvent.ACTION_UP, x1, y1);
                    return true;
                }
                case "key":
                case "hostkey": {
                    int code = Integer.parseInt(p[1]);
                    in.dispatchHostKey(KeyEvent.ACTION_DOWN, code, 0, 0, false);
                    in.dispatchHostKey(KeyEvent.ACTION_UP, code, 0, 0, false);
                    return true;
                }
                case "hostkeydown":
                    in.dispatchHostKey(KeyEvent.ACTION_DOWN, Integer.parseInt(p[1]), 0, 0, false);
                    return true;
                case "hostkeyup":
                    in.dispatchHostKey(KeyEvent.ACTION_UP, Integer.parseInt(p[1]), 0, 0, false);
                    return true;
                case "text": {
                    // As a keyboard types it: one key per character, a gap between them.
                    String text = line.substring(line.indexOf(' ') + 1);
                    for (int i = 0; i < text.length(); i++) {
                        char c = text.charAt(i);
                        in.dispatchHostKey(KeyEvent.ACTION_DOWN, 0, c, 0, false);
                        in.dispatchHostKey(KeyEvent.ACTION_UP, 0, c, 0, false);
                        Thread.sleep(30L);
                    }
                    return true;
                }
                default:
                    return false;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return true;
        } catch (RuntimeException e) {
            System.err.println(AospWindows.TAG + "inject '" + line + "': " + e);
            return true;
        }
    }
}
