// Guava's ImmutableMap, as far as the port uses it (copyOf, get, and the type).
package com.winapk.aospres.guava.collect;

import java.util.AbstractMap;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;

public final class ImmutableMap<K, V> extends AbstractMap<K, V> {
    private final Map<K, V> map;

    private ImmutableMap(Map<K, V> map) { this.map = map; }

    public static <K, V> ImmutableMap<K, V> copyOf(Map<? extends K, ? extends V> m) {
        if (m instanceof ImmutableMap) {
            @SuppressWarnings("unchecked") ImmutableMap<K, V> same = (ImmutableMap<K, V>) m;
            return same;
        }
        return new ImmutableMap<>(Collections.unmodifiableMap(new LinkedHashMap<>(m)));
    }

    @Override public V get(Object key) { return map.get(key); }

    @Override public Set<Entry<K, V>> entrySet() { return map.entrySet(); }
}
