// Guava's HashBiMap, as far as the port uses it (NativeObjRegistry: put, get, remove, keySet, clear,
// inverse). As Guava's, putting a value another key already has throws IllegalArgumentException.
package com.winapk.aospres.guava.collect;

import java.util.AbstractMap;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

public final class HashBiMap<K, V> extends AbstractMap<K, V> implements BiMap<K, V> {
    private final Map<K, V> forward;
    private final Map<V, K> backward;
    private final HashBiMap<V, K> inverse;

    private HashBiMap(Map<K, V> forward, Map<V, K> backward, HashBiMap<V, K> inverse) {
        this.forward = forward;
        this.backward = backward;
        this.inverse = inverse != null ? inverse : new HashBiMap<>(backward, forward, this);
    }

    public static <K, V> HashBiMap<K, V> create() { return new HashBiMap<>(new HashMap<>(), new HashMap<>(), null); }

    @Override public V put(K key, V value) {
        K other = backward.get(value);
        if (other != null && !other.equals(key)) throw new IllegalArgumentException("value already present: " + value);
        V old = forward.put(key, value);
        if (old != null) backward.remove(old);
        backward.put(value, key);
        return old;
    }

    @Override public V get(Object key) { return forward.get(key); }

    @Override public boolean containsKey(Object key) { return forward.containsKey(key); }

    @Override public boolean containsValue(Object value) { return backward.containsKey(value); }

    @Override public V remove(Object key) {
        V old = forward.remove(key);
        if (old != null) backward.remove(old);
        return old;
    }

    @Override public void clear() {
        forward.clear();
        backward.clear();
    }

    @Override public int size() { return forward.size(); }

    @Override public Set<K> keySet() { return java.util.Collections.unmodifiableSet(forward.keySet()); }

    @Override public Set<Entry<K, V>> entrySet() { return java.util.Collections.unmodifiableMap(forward).entrySet(); }

    @Override public BiMap<V, K> inverse() { return inverse; }
}
