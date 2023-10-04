/**
 * @test
 * @summary Test functionality of the malloc hooks library.
 * @author Ralf Schmelter
 *
 * @run main/othervm/native -XX:+UseMallocHooks MallocHooksTest
 */

 public class MallocHooksTest {
     static native boolean hasActiveHooks();
     static native String testNoRecursiveCalls();

     public static void main(String args[]) {
         if (!hasActiveHooks()) {
             String result = testNoRecursiveCalls();

             if (result != null) {
                 throw new RuntimeException("Failed no recursive calls test: " + result);
             }
         }
     }

     static {
         System.loadLibrary("TestMallocHooks");
     }
 }
