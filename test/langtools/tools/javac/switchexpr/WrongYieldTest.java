/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
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

/*
 * @test
 * @bug 8223305
 * @summary Ensure proper errors are returned for yields.
 * @compile/fail/ref=WrongYieldTest.out --enable-preview -source ${jdk.version} -XDrawDiagnostics -XDshould-stop.at=ATTR WrongYieldTest.java
 */

package t;

//ERROR - type called yield:
import t.WrongYieldTest.yield;

public class WrongYieldTest {

    // ERROR -  class called yield
    class yield { }

    // OK to have fields called yield
    String[] yield = null;

    // ERROR - field of type yield
    yield y;

    // OK to have methods called yield
    // Nullary yield method
    String[] yield() {
        return null;
    }
    // Unary yield method
    String[] yield(int i) {
        return null;
    }
    // Binary yield method
    String[] yield(int i, int j) {
        return null;
    }

    // OK to declare a local called yield
    void LocalDeclaration1() {
       int yield;
    }
    // OK to declare and initialise a local called yield
    void LocalDeclaration2() {
        int yield = 42;
    }
    // ERROR can't refer to a local called yield in the initialiser
    void LocalDeclaration3() {
        int yield = yield + 1;
    }

    // OK yield gets interpreted as an identifier in a local declaration
    void LocalDeclaration4(int i) {
        int local = switch (i) {
            default -> {
                int yield = yield + 1;
                yield 42;
            }
        };
    }
    // OK - yield a local called yield
    void LocalDeclaration5(int i) {
        int yield = 42;
        int temp = switch (i) {
            default -> {
                yield yield;
            }
        };
    }

    void YieldTypedLocals(int i) {
        // ERROR - Parsed as yield statement, and y1 is unknown
        yield y1 = null;
        // ..whereas..
        // ERROR - parsed as yield statement, which has no switch target
        Object y1;
        yield y1 = null;

        // ERROR - Parsed as yield statement, and y2 is unknown
        yield y2 = new yield();

        // OK - Parsed as yield statement that assigns local y
        Object y;
        Object o = switch (i) {
            default :
                yield y = null;
        };

        // ERROR - Parsed as yield statement that assigns local y,
        //but the initializer refers to restricted identifier:
        Object y2;
        Object o2 = switch (i) {
            default :
                yield y2 = new yield();
        };

        // ERROR - can not create an yield-valued local of type Object
        Object y3 = new yield();

        // ERROR - can not create a final yield-valued local of type yield
        final yield y4 = new yield();

        // ERROR - can create a non-final local of type yield using qualified typename
        WrongYieldTest.yield y5 = new yield();

    }

    void MethodInvocation(int i) {

        // OK - can access a field called yield
        String[] x = this.yield;

        // ERROR - calling nullary yield method using simple name parsed as yield statement
        yield();
        // OK - can call nullary yield method using qualified name
        this.yield();

        // ERROR - Calling unary yield method using simple name is parsed as yield statement
        yield(2);
        // OK - calling unary yield method using qualified name
        this.yield(2);

        // ERROR - Calling binary yield method using simple name is parsed as yield statement
        yield(2, 2); //error
        // OK - calling binary yield method using qualified name
        this.yield(2, 2);

        // ERROR - nullary yield method as receiver is parsed as yield statement
        yield().toString();
        // OK - nullary yield method as receiver using qualified name
        this.yield().toString();

        // ERROR - unary yield method as receiver is parsed as yield statement
        yield(2).toString();
        // OK - unary yield method as receiver using qualified name
        this.yield(2).toString();

        // ERROR - binary yield method as receiver is parsed as yield statement
        yield(2, 2).toString();
        // OK - binary yield method as receiver using qualified name
        this.yield(2, 2).toString();

        // OK - yield method call is in an expression position
        String str = yield(2).toString();



        //OK - yield is a variable
        yield.toString();

        // OK - parsed as method call (with qualified local yield as receiver)
        this.yield.toString();

        yield[0].toString(); //error

        // OK - parsed as yield statement in switch expression
        int j = switch (i) {
            default:
                yield(2);
        };

        // ERROR - second yield is an unreachable statement.
        x = switch (i) {
            default: {
                yield x = null;
                yield null;
            }
        };
    }

    private void yieldLocalVar1(int i) {
        int yield = 0;

        //OK - yield is a variable:
        yield++;
        yield--;

        //ERROR - yield is a statement, but no enclosing switch expr:
        yield ++i;
        yield --i;

        //OK - yield is a variable:
        yield = 3;

        //OK - yield is a variable:
        for (int j = 0; j < 3; j++)
            yield += 1;

        //OK - yield is a variable and not at the beginning of the statement:
        yieldLocalVar1(yield);

        //ERROR - unqualified yield method invocation:
        yieldLocalVar1(yield().length);
        yieldLocalVar1(yield.class.getModifiers());
    }

    private void yieldLocalVar2(int i) {
        int[] yield = new int[1];

        //OK - yield is a variable:
        yield[0] = 5;
    }
}
