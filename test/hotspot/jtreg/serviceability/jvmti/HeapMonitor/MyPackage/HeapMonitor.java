/*
 * Copyright (c) 2018, Google and/or its affiliates. All rights reserved.
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

package MyPackage;

import java.util.ArrayList;
import java.util.List;

/** API for handling the underlying heap sampling monitoring system. */
public class HeapMonitor {
  private static int[][] arrays;
  private static int allocationIterations = 1000;

  static {
    try {
      System.loadLibrary("HeapMonitorTest");
    } catch (UnsatisfiedLinkError ule) {
      System.err.println("Could not load HeapMonitor library");
      System.err.println("java.library.path: " + System.getProperty("java.library.path"));
      throw ule;
    }
  }

  /** Set a specific sampling rate, 0 samples every allocation. */
  public native static void setSamplingRate(int rate);
  public native static void enableSamplingEvents();
  public native static boolean enableSamplingEventsForTwoThreads(Thread firstThread, Thread secondThread);
  public native static void disableSamplingEvents();

  /**
   * Allocate memory but first create a stack trace.
   *
   * @return list of frames for the allocation.
   */
  public static List<Frame> allocate() {
    int sum = 0;
    List<Frame> frames = new ArrayList<Frame>();
    allocate(frames);
    frames.add(new Frame("allocate", "()Ljava/util/List;", "HeapMonitor.java", 58));
    return frames;
  }

  private static void allocate(List<Frame> frames) {
    int sum = 0;
    for (int j = 0; j < allocationIterations; j++) {
      sum += actuallyAllocate();
    }
    frames.add(new Frame("actuallyAllocate", "()I", "HeapMonitor.java", 93));
    frames.add(new Frame("allocate", "(Ljava/util/List;)V", "HeapMonitor.java", 66));
  }

  public static List<Frame> repeatAllocate(int max) {
    List<Frame> frames = null;
    for (int i = 0; i < max; i++) {
      frames = allocate();
    }
    frames.add(new Frame("repeatAllocate", "(I)Ljava/util/List;", "HeapMonitor.java", 75));
    return frames;
  }

  private static int actuallyAllocate() {
    int sum = 0;

    // Let us assume that a 1-element array is 24 bytes of memory and we want
    // 2MB allocated.
    int iterations = (1 << 19) / 6;

    if (arrays == null) {
      arrays = new int[iterations][];
    }

    for (int i = 0; i < iterations; i++) {
      int tmp[] = new int[1];
      // Force it to be kept and, at the same time, wipe out any previous data.
      arrays[i] = tmp;
      sum += arrays[0][0];
    }
    return sum;
  }

  public static int allocateSize(int totalSize) {
    int sum = 0;

    // Let us assume that a 1-element array is 24 bytes.
    int iterations = totalSize / 24;

    if (arrays == null) {
      arrays = new int[iterations][];
    }

    System.out.println("Allocating for " + iterations);
    for (int i = 0; i < iterations; i++) {
      int tmp[] = new int[1];

      // Force it to be kept and, at the same time, wipe out any previous data.
      arrays[i] = tmp;
      sum += arrays[0][0];
    }

    return sum;
  }

  /** Remove the reference to the global array to free data at the next GC. */
  public static void freeStorage() {
    arrays = null;
  }

  public static int[][][] sampleEverything() {
    enableSamplingEvents();
    setSamplingRate(0);

    // Loop around an allocation loop and wait until the tlabs have settled.
    final int maxTries = 10;
    int[][][] result = new int[maxTries][][];
    for (int i = 0; i < maxTries; i++) {
      final int maxInternalTries = 400;
      result[i] = new int[maxInternalTries][];

      resetEventStorage();
      for (int j = 0; j < maxInternalTries; j++) {
        final int size = 1000;
        result[i][j] = new int[size];
      }

      int sampledEvents = sampledEvents();
      if (sampledEvents == maxInternalTries) {
        return result;
      }
    }

    throw new RuntimeException("Could not set the sampler");
  }

  public native static int sampledEvents();
  public native static boolean obtainedEvents(Frame[] frames);
  public native static boolean garbageContains(Frame[] frames);
  public native static boolean eventStorageIsEmpty();
  public native static void resetEventStorage();
  public native static int getEventStorageElementCount();
  public native static void forceGarbageCollection();
  public native static boolean enableVMEvents();

  public static boolean statsHaveExpectedNumberSamples(int expected, int acceptedErrorPercentage) {
    double actual = getEventStorageElementCount();
    double diffPercentage = Math.abs(actual - expected) / expected;
    return diffPercentage < acceptedErrorPercentage;
  }

  public static void setAllocationIterations(int iterations) {
    allocationIterations = iterations;
  }
}
