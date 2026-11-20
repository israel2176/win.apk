// Guava's Iterators.peekingIterator.
package com.winapk.aospres.guava.collect;

import java.util.Iterator;

public final class Iterators {
    private Iterators() {}

    public static <T> PeekingIterator<T> peekingIterator(Iterator<? extends T> it) {
        return new PeekingIterator<T>() {
            private boolean peeked;
            private T next;

            @Override public boolean hasNext() { return peeked || it.hasNext(); }

            @Override public T peek() {
                if (!peeked) {
                    next = it.next();
                    peeked = true;
                }
                return next;
            }

            @Override public T next() {
                if (!peeked) return it.next();
                T r = next;
                peeked = false;
                next = null;
                return r;
            }

            @Override public void remove() {
                if (peeked) throw new IllegalStateException("Can't remove after you've peeked at next");
                it.remove();
            }
        };
    }
}
