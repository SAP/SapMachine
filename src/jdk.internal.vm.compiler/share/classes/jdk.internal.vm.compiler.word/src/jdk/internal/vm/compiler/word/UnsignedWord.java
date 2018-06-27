/*
 * Copyright (c) 2012, 2012, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
package jdk.internal.vm.compiler.word;

/**
 * Represents an unsigned word-sized value.
 *
 * @since 1.0
 */
public interface UnsignedWord extends ComparableWord {

    /**
     * Returns a Unsigned whose value is {@code (this + val)}.
     *
     * @param val value to be added to this Unsigned.
     * @return {@code this + val}
     *
     * @since 1.0
     */
    UnsignedWord add(UnsignedWord val);

    /**
     * Returns a Unsigned whose value is {@code (this - val)}.
     *
     * @param val value to be subtracted from this Unsigned.
     * @return {@code this - val}
     *
     * @since 1.0
     */
    UnsignedWord subtract(UnsignedWord val);

    /**
     * Returns a Unsigned whose value is {@code (this * val)}.
     *
     * @param val value to be multiplied by this Unsigned.
     * @return {@code this * val}
     *
     * @since 1.0
     */
    UnsignedWord multiply(UnsignedWord val);

    /**
     * Returns a Unsigned whose value is {@code (this / val)}.
     *
     * @param val value by which this Unsigned is to be divided.
     * @return {@code this / val}
     *
     * @since 1.0
     */
    UnsignedWord unsignedDivide(UnsignedWord val);

    /**
     * Returns a Unsigned whose value is {@code (this % val)}.
     *
     * @param val value by which this Unsigned is to be divided, and the remainder computed.
     * @return {@code this % val}
     *
     * @since 1.0
     */
    UnsignedWord unsignedRemainder(UnsignedWord val);

    /**
     * Returns a Unsigned whose value is {@code (this << n)}.
     *
     * @param n shift distance, in bits.
     * @return {@code this << n}
     *
     * @since 1.0
     */
    UnsignedWord shiftLeft(UnsignedWord n);

    /**
     * Returns a Unsigned whose value is {@code (this >>> n)}. No sign extension is performed.
     *
     * @param n shift distance, in bits.
     * @return {@code this >> n}
     *
     * @since 1.0
     */
    UnsignedWord unsignedShiftRight(UnsignedWord n);

    /**
     * Returns a Unsigned whose value is {@code (this & val)}.
     *
     * @param val value to be AND'ed with this Unsigned.
     * @return {@code this & val}
     *
     * @since 1.0
     */
    UnsignedWord and(UnsignedWord val);

    /**
     * Returns a Unsigned whose value is {@code (this | val)}.
     *
     * @param val value to be OR'ed with this Unsigned.
     * @return {@code this | val}
     *
     * @since 1.0
     */
    UnsignedWord or(UnsignedWord val);

    /**
     * Returns a Unsigned whose value is {@code (this ^ val)}.
     *
     * @param val value to be XOR'ed with this Unsigned.
     * @return {@code this ^ val}
     *
     * @since 1.0
     */
    UnsignedWord xor(UnsignedWord val);

    /**
     * Returns a Unsigned whose value is {@code (~this)}.
     *
     * @return {@code ~this}
     *
     * @since 1.0
     */
    UnsignedWord not();

    /**
     * Compares this Unsigned with the specified value.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this == val}
     *
     * @since 1.0
     */
    boolean equal(UnsignedWord val);

    /**
     * Compares this Unsigned with the specified value.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this != val}
     *
     * @since 1.0
     */
    boolean notEqual(UnsignedWord val);

    /**
     * Compares this Unsigned with the specified value.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this < val}
     *
     * @since 1.0
     */
    boolean belowThan(UnsignedWord val);

    /**
     * Compares this Unsigned with the specified value.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this <= val}
     *
     * @since 1.0
     */
    boolean belowOrEqual(UnsignedWord val);

    /**
     * Compares this Unsigned with the specified value.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this > val}
     *
     * @since 1.0
     */
    boolean aboveThan(UnsignedWord val);

    /**
     * Compares this Unsigned with the specified value.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this >= val}
     *
     * @since 1.0
     */
    boolean aboveOrEqual(UnsignedWord val);

    /**
     * Returns a Unsigned whose value is {@code (this + val)}.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to be added to this Unsigned.
     * @return {@code this + val}
     *
     * @since 1.0
     */
    UnsignedWord add(int val);

    /**
     * Returns a Unsigned whose value is {@code (this - val)}.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to be subtracted from this Unsigned.
     * @return {@code this - val}
     *
     * @since 1.0
     */
    UnsignedWord subtract(int val);

    /**
     * Returns a Unsigned whose value is {@code (this * val)}.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to be multiplied by this Unsigned.
     * @return {@code this * val}
     *
     * @since 1.0
     */
    UnsignedWord multiply(int val);

    /**
     * Returns a Unsigned whose value is {@code (this / val)}.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value by which this Unsigned is to be divided.
     * @return {@code this / val}
     *
     * @since 1.0
     */
    UnsignedWord unsignedDivide(int val);

    /**
     * Returns a Unsigned whose value is {@code (this % val)}.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value by which this Unsigned is to be divided, and the remainder computed.
     * @return {@code this % val}
     *
     * @since 1.0
     */
    UnsignedWord unsignedRemainder(int val);

    /**
     * Returns a Unsigned whose value is {@code (this << n)}.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param n shift distance, in bits.
     * @return {@code this << n}
     *
     * @since 1.0
     */
    UnsignedWord shiftLeft(int n);

    /**
     * Returns a Unsigned whose value is {@code (this >>> n)}. No sign extension is performed.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param n shift distance, in bits.
     * @return {@code this >> n}
     *
     * @since 1.0
     */
    UnsignedWord unsignedShiftRight(int n);

    /**
     * Returns a Unsigned whose value is {@code (this & val)}.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to be AND'ed with this Unsigned.
     * @return {@code this & val}
     *
     * @since 1.0
     */
    UnsignedWord and(int val);

    /**
     * Returns a Unsigned whose value is {@code (this | val)}.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to be OR'ed with this Unsigned.
     * @return {@code this | val}
     *
     * @since 1.0
     */
    UnsignedWord or(int val);

    /**
     * Returns a Unsigned whose value is {@code (this ^ val)}.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to be XOR'ed with this Unsigned.
     * @return {@code this ^ val}
     *
     * @since 1.0
     */
    UnsignedWord xor(int val);

    /**
     * Compares this Unsigned with the specified value.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this == val}
     *
     * @since 1.0
     */
    boolean equal(int val);

    /**
     * Compares this Unsigned with the specified value.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this != val}
     *
     * @since 1.0
     */
    boolean notEqual(int val);

    /**
     * Compares this Unsigned with the specified value.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this < val}
     *
     * @since 1.0
     */
    boolean belowThan(int val);

    /**
     * Compares this Unsigned with the specified value.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this <= val}
     *
     * @since 1.0
     */
    boolean belowOrEqual(int val);

    /**
     * Compares this Unsigned with the specified value.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this > val}
     *
     * @since 1.0
     */
    boolean aboveThan(int val);

    /**
     * Compares this Unsigned with the specified value.
     * <p>
     * Note that the right operand is a signed value, while the operation is performed unsigned.
     * Therefore, the result is only well-defined for positive right operands.
     *
     * @param val value to which this Unsigned is to be compared.
     * @return {@code this >= val}
     *
     * @since 1.0
     */
    boolean aboveOrEqual(int val);
}
