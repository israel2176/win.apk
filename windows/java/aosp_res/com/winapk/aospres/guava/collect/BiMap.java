// The members of Guava's types that Robolectric's libandroidfw port calls, and nothing more
// (AospResRewrite.java maps the port's com.google.common names here). Same behaviour as Guava's
// for those calls.
package com.winapk.aospres.guava.collect;

import java.util.Map;

public interface BiMap<K, V> extends Map<K, V> {
    BiMap<V, K> inverse();
}
