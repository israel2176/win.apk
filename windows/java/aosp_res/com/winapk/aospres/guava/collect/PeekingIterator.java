// Guava's PeekingIterator (ConfigDescription's qualifier parser).
package com.winapk.aospres.guava.collect;

import java.util.Iterator;

public interface PeekingIterator<E> extends Iterator<E> {
    E peek();
}
