/*
 * Copyright (c) 2016, 2017, Oracle and/or its affiliates. All rights reserved.
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
package requires;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Callable;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import sun.hotspot.cpuinfo.CPUInfo;
import sun.hotspot.gc.GC;
import sun.hotspot.WhiteBox;
import jdk.test.lib.Platform;

/**
 * The Class to be invoked by jtreg prior Test Suite execution to
 * collect information about VM.
 * Do not use any API's that may not be available in all target VMs.
 * Properties set by this Class will be available in the @requires expressions.
 */
public class VMProps implements Callable<Map<String, String>> {

    private static final WhiteBox WB = WhiteBox.getWhiteBox();

    /**
     * Collects information about VM properties.
     * This method will be invoked by jtreg.
     *
     * @return Map of property-value pairs.
     */
    @Override
    public Map<String, String> call() {
        Map<String, String> map = new HashMap<>();
        map.put("vm.flavor", vmFlavor());
        map.put("vm.compMode", vmCompMode());
        map.put("vm.bits", vmBits());
        map.put("vm.flightRecorder", vmFlightRecorder());
        map.put("vm.simpleArch", vmArch());
        map.put("vm.debug", vmDebug());
        map.put("vm.jvmci", vmJvmci());
        map.put("vm.emulatedClient", vmEmulatedClient());
        map.put("vm.cpu.features", cpuFeatures());
        map.put("vm.rtm.cpu", vmRTMCPU());
        map.put("vm.rtm.os", vmRTMOS());
        map.put("vm.aot", vmAOT());
        // vm.cds is true if the VM is compiled with cds support.
        map.put("vm.cds", vmCDS());
        map.put("vm.cds.custom.loaders", vmCDSForCustomLoaders());
        // vm.graal.enabled is true if Graal is used as JIT
        map.put("vm.graal.enabled", isGraalEnabled());
        map.put("docker.support", dockerSupport());
        vmGC(map); // vm.gc.X = true/false

        VMProps.dump(map);
        return map;
    }

    /**
     * Prints a stack trace before returning null.
     * Used by the various helper functions which parse information from
     * VM properties in the case where they don't find an expected property
     * or a propoerty doesn't conform to an expected format.
     *
     * @return null
     */
    private String nullWithException(String message) {
        new Exception(message).printStackTrace();
        return null;
    }

    /**
     * @return vm.simpleArch value of "os.simpleArch" property of tested JDK.
     */
    protected String vmArch() {
        String arch = System.getProperty("os.arch");
        if (arch.equals("x86_64") || arch.equals("amd64")) {
            return "x64";
        }
        else if (arch.contains("86")) {
            return "x86";
        } else {
            return arch;
        }
    }



    /**
     * @return VM type value extracted from the "java.vm.name" property.
     */
    protected String vmFlavor() {
        // E.g. "Java HotSpot(TM) 64-Bit Server VM"
        String vmName = System.getProperty("java.vm.name");
        if (vmName == null) {
            return nullWithException("Can't get 'java.vm.name' property");
        }

        Pattern startP = Pattern.compile(".* (\\S+) VM");
        Matcher m = startP.matcher(vmName);
        if (m.matches()) {
            return m.group(1).toLowerCase();
        }
        return nullWithException("Can't get VM flavor from 'java.vm.name'");
    }

    /**
     * @return VM compilation mode extracted from the "java.vm.info" property.
     */
    protected String vmCompMode() {
        // E.g. "mixed mode"
        String vmInfo = System.getProperty("java.vm.info");
        if (vmInfo == null) {
            return nullWithException("Can't get 'java.vm.info' property");
        }
        if (vmInfo.toLowerCase().indexOf("mixed mode") != -1) {
            return "Xmixed";
        } else if (vmInfo.toLowerCase().indexOf("compiled mode") != -1) {
            return "Xcomp";
        } else if (vmInfo.toLowerCase().indexOf("interpreted mode") != -1) {
            return "Xint";
        } else {
            return nullWithException("Can't get compilation mode from 'java.vm.info'");
        }
    }

    /**
     * @return VM bitness, the value of the "sun.arch.data.model" property.
     */
    protected String vmBits() {
        String dataModel = System.getProperty("sun.arch.data.model");
        if (dataModel != null) {
            return dataModel;
        } else {
            return nullWithException("Can't get 'sun.arch.data.model' property");
        }
    }

    /**
     * @return "true" if Flight Recorder is enabled, "false" if is disabled.
     */
    protected String vmFlightRecorder() {
        Boolean isUnlockedCommercialFatures = WB.getBooleanVMFlag("UnlockCommercialFeatures");
        Boolean isFlightRecorder = WB.getBooleanVMFlag("FlightRecorder");
        String startFROptions = WB.getStringVMFlag("StartFlightRecording");
        if (isUnlockedCommercialFatures != null && isUnlockedCommercialFatures) {
            if (isFlightRecorder != null && isFlightRecorder) {
                return "true";
            }
            if (startFROptions != null && !startFROptions.isEmpty()) {
                return "true";
            }
        }
        return "false";
    }

    /**
     * @return debug level value extracted from the "jdk.debug" property.
     */
    protected String vmDebug() {
        String debug = System.getProperty("jdk.debug");
        if (debug != null) {
            return "" + debug.contains("debug");
        } else {
            return nullWithException("Can't get 'jdk.debug' property");
        }
    }

    /**
     * @return true if VM supports JVMCI and false otherwise
     */
    protected String vmJvmci() {
        // builds with jvmci have this flag
        return "" + (WB.getBooleanVMFlag("EnableJVMCI") != null);
    }

    /**
     * @return true if VM runs in emulated-client mode and false otherwise.
     */
    protected String vmEmulatedClient() {
        String vmInfo = System.getProperty("java.vm.info");
        if (vmInfo == null) {
            return "false";
        }
        return "" + vmInfo.contains(" emulated-client");
    }

    /**
     * @return supported CPU features
     */
    protected String cpuFeatures() {
        return CPUInfo.getFeatures().toString();
    }

    /**
     * For all existing GC sets vm.gc.X property.
     * Example vm.gc.G1=true means:
     *    VM supports G1
     *    User either set G1 explicitely (-XX:+UseG1GC) or did not set any GC
     * @param map - property-value pairs
     */
    protected void vmGC(Map<String, String> map){
        GC currentGC = GC.current();
        boolean isByErgo = GC.currentSetByErgo();
        List<GC> supportedGC = GC.allSupported();
        for (GC gc: GC.values()) {
            boolean isSupported = supportedGC.contains(gc);
            boolean isAcceptable = isSupported && (gc == currentGC || isByErgo);
            map.put("vm.gc." + gc.name(), "" + isAcceptable);
        }
    }

    /**
     * @return true if VM runs RTM supported OS and false otherwise.
     */
    protected String vmRTMOS() {
        boolean isRTMOS = true;

        if (Platform.isAix()) {
            // Actually, this works since AIX 7.1.3.30, but os.version property
            // is set to 7.1.
            isRTMOS = (Platform.getOsVersionMajor()  > 7) ||
                      (Platform.getOsVersionMajor() == 7 && Platform.getOsVersionMinor() > 1);

        } else if (Platform.isLinux()) {
            if (Platform.isPPC()) {
                isRTMOS = (Platform.getOsVersionMajor()  > 4) ||
                          (Platform.getOsVersionMajor() == 4 && Platform.getOsVersionMinor() > 1);
            }
        }
        return "" + isRTMOS;
    }

    /**
     * @return true if VM runs RTM supported CPU and false otherwise.
     */
    protected String vmRTMCPU() {
        boolean vmRTMCPU = (Platform.isPPC() ? CPUInfo.hasFeature("tcheck") : CPUInfo.hasFeature("rtm"));

        return "" + vmRTMCPU;
    }

    /**
     * @return true if VM supports AOT and false otherwise
     */
    protected String vmAOT() {
        // builds with aot have jaotc in <JDK>/bin
        Path bin = Paths.get(System.getProperty("java.home"))
                        .resolve("bin");
        Path jaotc;
        if (Platform.isWindows()) {
            jaotc = bin.resolve("jaotc.exe");
        } else {
            jaotc = bin.resolve("jaotc");
        }
        return "" + Files.exists(jaotc);
    }

    /**
     * Check for CDS support.
     *
     * @return true if CDS is supported by the VM to be tested.
     */
    protected String vmCDS() {
        if (WB.isCDSIncludedInVmBuild()) {
            return "true";
        } else {
            return "false";
        }
    }

    /**
     * Check for CDS support for custom loaders.
     *
     * @return true if CDS is supported for customer loader by the VM to be tested.
     */
    protected String vmCDSForCustomLoaders() {
        if (vmCDS().equals("true") && Platform.areCustomLoadersSupportedForCDS()) {
            return "true";
        } else {
            return "false";
        }
    }

    /**
     * Check if Graal is used as JIT compiler.
     *
     * @return true if Graal is used as JIT compiler.
     */
    protected String isGraalEnabled() {
        // Graal is enabled if following conditions are true:
        // - we are not in Interpreter mode
        // - UseJVMCICompiler flag is true
        // - jvmci.Compiler variable is equal to 'graal'
        // - TieredCompilation is not used or TieredStopAtLevel is greater than 3

        Boolean useCompiler = WB.getBooleanVMFlag("UseCompiler");
        if (useCompiler == null || !useCompiler)
            return "false";

        Boolean useJvmciComp = WB.getBooleanVMFlag("UseJVMCICompiler");
        if (useJvmciComp == null || !useJvmciComp)
            return "false";

        // This check might be redundant but let's keep it for now.
        String jvmciCompiler = System.getProperty("jvmci.Compiler");
        if (jvmciCompiler == null || !jvmciCompiler.equals("graal")) {
            return "false";
        }

        Boolean tieredCompilation = WB.getBooleanVMFlag("TieredCompilation");
        Long compLevel = WB.getIntxVMFlag("TieredStopAtLevel");
        // if TieredCompilation is enabled and compilation level is <= 3 then no Graal is used
        if (tieredCompilation != null && tieredCompilation && compLevel != null && compLevel <= 3)
            return "false";

        return "true";
    }


   /**
     * A simple check for docker support
     *
     * @return true if docker is supported in a given environment
     */
    protected String dockerSupport() {
        // currently docker testing is only supported for Linux-x64
        if (! ( Platform.isLinux() && Platform.isX64() ) )
            return "false";

        boolean isSupported;
        try {
            isSupported = checkDockerSupport();
        } catch (Exception e) {
            isSupported = false;
        }

        return (isSupported) ? "true" : "false";
    }

    private boolean checkDockerSupport() throws IOException, InterruptedException {
        ProcessBuilder pb = new ProcessBuilder("docker", "ps");
        Process p = pb.start();
        p.waitFor(10, TimeUnit.SECONDS);

        return (p.exitValue() == 0);
    }



    /**
     * Dumps the map to the file if the file name is given as the property.
     * This functionality could be helpful to know context in the real
     * execution.
     *
     * @param map
     */
    protected static void dump(Map<String, String> map) {
        String dumpFileName = System.getProperty("vmprops.dump");
        if (dumpFileName == null) {
            return;
        }
        List<String> lines = new ArrayList<>();
        map.forEach((k, v) -> lines.add(k + ":" + v));
        try {
            Files.write(Paths.get(dumpFileName), lines, StandardOpenOption.APPEND);
        } catch (IOException e) {
            throw new RuntimeException("Failed to dump properties into '"
                    + dumpFileName + "'", e);
        }
    }

    /**
     * This method is for the testing purpose only.
     * @param args
     */
    public static void main(String args[]) {
        Map<String, String> map = new VMProps().call();
        map.forEach((k, v) -> System.out.println(k + ": '" + v + "'"));
    }
}
