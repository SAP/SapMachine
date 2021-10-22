(c) SAP 2021

# The SapMachine MallocTrace facility

## Preface

When analyzing native OOMs resulting from C-heap exhaustion, we have two facilities built into the VM:

- NMT (available in OpenJDK, SapMachine, and SAP JVM)
- the SAP JVM malloc statistics (as the name indicates, SAP JVM only)

Both facilities have pros and cons, but they share one significant disadvantage: they require the malloc sites to be instrumented at compile time. Therefore they are unsuited for analyzing situations where outside code within the VM process allocates C-heap.

In the past, if we were facing a suspected C-heap leak from code outside the VM (outside the coverage of NMT or SAP JVM malloc statistics), we were helpless. In these situations, one would typically use tools like perf or Valgrind, but that is seldom an option for us.

### mtrace?

A straightforward possibility to do this would be the Glibc-internal trace. Using `mtrace(3)`, one can force the Glibc to write a trace file for all malloc calls. Unfortunately, it is very costly. In my experiments, VM slowed down by factor 8-10. It also has to be enabled at the start of the VM (when the VM still happens to be single-threaded). That usually rules it out in production scenarios.

## The SapMachine MallocTrace facility

The new Linux-only malloc trace facility in the SapMachine uses Glibc malloc hooks to hook into the allocation process. In that way, it resembles the Glibc-internal `mtrace(3)`. But unlike `mtrace(3)`, it accumulates data in memory and provides a condensed report upon request, making it a lot faster. Moreover, it does not have to be started at VM startup (though it can), but one can switch it on when needed.

### Usage via jcmd

#### Switch trace on:

```
thomas@starfish$ jcmd AllocCHeap System.malloctrace on
268112:
Tracing activated
```

#### Switch trace off:
```
thomas@starfish$ jcmd AllocCHeap System.malloctrace off
268112:
Tracing deactivated
```

#### Print a SapMachine MallocTrace report:

Two options exist:
- a full report which can be lengthy but will show all call sites.
- (default) an abridged report which only shows the ten "hottest" call sites.

```
jcmd (VM) System.malloctrace print [all]
```

Example:

```
thomas@starfish$ jcmd AllocCHeap System.malloctrace print
268112:
---- 10 hottest malloc sites: ----
---- 0 ----
Invocs: 2813 (+0)
Alloc Size Range: 8 - 312
[0x00007fd04159f3d0] sap::my_malloc_hook(unsigned long, void const*)+192 in libjvm.so
[0x00007fd040e0a004] AllocateHeap(unsigned long, MEMFLAGS, AllocFailStrategy::AllocFailEnum)+68 in libjvm.so
[0x00007fd041891eb2] SymbolTable::allocate_symbol(char const*, int, bool)+226 in libjvm.so
[0x00007fd041895c94] SymbolTable::do_add_if_needed(char const*, int, unsigned long, bool)+116 in libjvm.so
[0x00007fd04189669f] SymbolTable::new_symbols(ClassLoaderData*, constantPoolHandle const&, int, char const**, int*, int*, unsigned int*)+95 in libjvm.so
[0x00007fd040fc0042] ClassFileParser::parse_constant_pool_entries(ClassFileStream const*, ConstantPool*, int, JavaThread*)+3026 in libjvm.so
[0x00007fd040fc0282] ClassFileParser::parse_constant_pool(ClassFileStream const*, ConstantPool*, int, JavaThread*)+34 in libjvm.so
[0x00007fd040fc1c9a] ClassFileParser::ClassFileParser(ClassFileStream*, Symbol*, ClassLoaderData*, ClassLoadInfo const*, ClassFileParser::Publicity, JavaThread*)+938 in libjvm.so
[0x00007fd04149dd3e] KlassFactory::create_from_stream(ClassFileStream*, Symbol*, ClassLoaderData*, ClassLoadInfo const&, JavaThread*)+558 in libjvm.so
[0x00007fd0418a3310] SystemDictionary::resolve_class_from_stream(ClassFileStream*, Symbol*, Handle, ClassLoadInfo const&, JavaThread*)+496 in libjvm.so
[0x00007fd041357bce] jvm_define_class_common(char const*, _jobject*, signed char const*, int, _jobject*, char const*, JavaThread*) [clone .constprop.285]+510 in libjvm.so
[0x00007fd041357d06] JVM_DefineClassWithSource+134 in libjvm.so
[0x00007fd0402cf6d2] Java_java_lang_ClassLoader_defineClass1+450 in libjava.so
[0x00007fd0254b453a] 0x00007fd0254b453aBufferBlob (0x00007fd0254afb10) used for Interpreter
---- 1 ----
Invocs: 2812 (+0)
Alloc Size: 16
[0x00007fd04159f3d0] sap::my_malloc_hook(unsigned long, void const*)+192 in libjvm.so
[0x00007fd040e0a004] AllocateHeap(unsigned long, MEMFLAGS, AllocFailStrategy::AllocFailEnum)+68 in libjvm.so
[0x00007fd041895cd6] SymbolTable::do_add_if_needed(char const*, int, unsigned long, bool)+182 in libjvm.so
[0x00007fd04189669f] SymbolTable::new_symbols(ClassLoaderData*, constantPoolHandle const&, int, char const**, int*, int*, unsigned int*)+95 in libjvm.so
[0x00007fd040fc0042] ClassFileParser::parse_constant_pool_entries(ClassFileStream const*, ConstantPool*, int, JavaThread*)+3026 in libjvm.so
[0x00007fd040fc0282] ClassFileParser::parse_constant_pool(ClassFileStream const*, ConstantPool*, int, JavaThread*)+34 in libjvm.so
[0x00007fd040fc1c9a] ClassFileParser::ClassFileParser(ClassFileStream*, Symbol*, ClassLoaderData*, ClassLoadInfo const*, ClassFileParser::Publicity, JavaThread*)+938 in libjvm.so
[0x00007fd04149dd3e] KlassFactory::create_from_stream(ClassFileStream*, Symbol*, ClassLoaderData*, ClassLoadInfo const&, JavaThread*)+558 in libjvm.so
[0x00007fd0418a3310] SystemDictionary::resolve_class_from_stream(ClassFileStream*, Symbol*, Handle, ClassLoadInfo const&, JavaThread*)+496 in libjvm.so
[0x00007fd041357bce] jvm_define_class_common(char const*, _jobject*, signed char const*, int, _jobject*, char const*, JavaThread*) [clone .constprop.285]+510 in libjvm.so
[0x00007fd041357d06] JVM_DefineClassWithSource+134 in libjvm.so
[0x00007fd0402cf6d2] Java_java_lang_ClassLoader_defineClass1+450 in libjava.so
[0x00007fd0254b453a] 0x00007fd0254b453aBufferBlob (0x00007fd0254afb10) used for Interpreter
...
<snip>
...
Table size: 8171, num_entries: 3351, used slots: 519, longest chain: 5, invocs: 74515, lost: 0, collisions: 5844
Malloc trace on.
 (method: nmt-ish)

74515 captures (0 without stack).
```

#### Reset the call site table:

It is possible to reset the call site table.

```
jcmd (VM) System.malloctrace reset
```

This command will clear the table but not affect any running trace (if active) - the table will repopulate.


### Usage via command line

One can switch on tracing at VM startup using the switch `-XX:+EnableMallocTrace`. A final report is printed upon VM shutdown to stdout via `-XX:+PrintMallocTraceAtExit`.

Both options are diagnostic; one needs to unlock them with `-XX:+UnlockDiagnosticVMOptions` in release builds.

Note: Starting the trace at VM startup is certainly possible but may not be the best option;  the internal call site table will fill with many one-shot call sites that are only relevant during VM startup. A too-full call site table may slow down subsequent tracing.

### Memory costs

The internal data structures cost about ~5M. This memory is limited, and it will not grow - if we hit the limit, we won't register new call sites we encounter (but will continue to account for old call sites).

Note that the call site table holds 32K call sites. That far exceeds the usual number of malloc call sites in the VM, so we should typically never hit this limit.

### Performance costs

In measurements, the MallocTrace increased VM startup time by about 6%. The slowdown highly depends on the frequency of malloc calls, though: a thread continuously doing malloc in a tight loop may slow down by factor 2-3.

### Limitations

This facility uses Glibc malloc hooks. 

Glibc malloc hooks are decidedly thread-unsafe, and we use them in a multithreaded context. Therefore what we can do with these hooks is very restricted.

1) This is **not** a complete leak analysis tool! All we see with this facility is the "hotness" of malloc call sites. These may be innocuous; e.g., a malloc call site may be followed by a free call right away - it still would show up as a hot call site in the MallocTrace report.
2) **Not every allocation** will be captured since there are small-time windows where the hooks need to be disabled.

One should use the MallocTrace tool to analyze suspected C-heap leaks when NMT/SAP JVM malloc statistics show up empty. It shows you which malloc sites are hot; nothing more. 

It works with third-party code, even with code that just happens to run in the VM process, e.g., system libraries.
