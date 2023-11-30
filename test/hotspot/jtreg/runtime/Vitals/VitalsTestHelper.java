/*
 * Copyright (c) 2022, SAP SE. All rights reserved.
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

import jdk.test.lib.Platform;
import jdk.test.lib.process.OutputAnalyzer;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.regex.Pattern;


public class VitalsTestHelper {

//    Last 60 minutes:
//                                  ---------------------------------------system---------------------------------------- -------------------------process-------------------------- ----------------------------------------jvm-----------------------------------------
//                                                                        ------cpu------ ------------cgroup-------------      -------rss--------      -cheap-- -cpu- ----io----     --heap--- ----------meta----------      --nmt--- ----jthr----- --cldg-- ----cls-----
//                                  avail comm  crt swap si so p t  pr pb us sy id  st gu lim  limsw slim usg  usgsw kusg virt all  anon file shm swdo usd free us sy of rd   wr thr comm used comm used csc  csu  gctr code mlc map  num nd cr st  num anon num  ld  uld
//            2022-05-14 14:52:36   54.4g 21.7g  65   0k  0  0 2 22  2  0  3  0  96  0  0 8.0g 16.0g      118m  118m   2m 5.1g 109m  72m  38m  0k   0k 44m   7m  0  0  4  11k 0k  21 130m   7m   3m   3m 320k 224k  21m   8m 43m 193m  12  1  0 20m  34   31 1330   0   0
//            2022-05-14 14:52:26   54.4g 21.9g  65   0k  0  0 2 22  4  1  0  0  99  0  0 8.0g 16.0g      118m  118m   2m 5.1g 109m  72m  38m  0k   0k 44m   7m  0  0  4  11k 0k  21 130m   7m   3m   3m 320k 224k  21m   8m 43m 193m  12  1  0 20m  34   31 1330   0   0
//            2022-05-14 14:52:16   54.4g 21.8g  65   0k  0  0 2 22  3  0  0  0  99  0  0 8.0g 16.0g      118m  118m   2m 5.1g 109m  72m  38m  0k   0k 44m   7m  0  0  4  11k 0k  21 130m   7m   3m   3m 320k 224k  21m   8m 43m 193m  12  1  0 20m  34   31 1330   0   0
//            2022-05-14 14:52:06   54.4g 21.8g  65   0k  0  0 2 22  1  0  0  0  99  0  0 8.0g 16.0g      118m  118m   2m 5.1g 109m  72m  38m  0k   0k 44m   7m  0  0  4  11k 0k  21 130m   7m   3m   3m 320k 224k  21m   8m 43m 193m  12  1  0 20m  34   31 1330   0   0
//            2022-05-14 14:51:56   54.4g 21.7g  64   0k  0  0 2 22  2  0  0  0  99  0  0 8.0g 16.0g      118m  118m   2m 5.1g 109m  72m  38m  0k   0k 44m   7m  0  0  4  11k 0k  21 130m   7m   3m   3m 320k 224k  21m   8m 43m 193m  12  1  0 20m  34   31 1330   0   0
//            2022-05-14 14:51:46   54.4g 21.5g  64   0k  0  0 2 22  1  0  0  0  99  0  0 8.0g 16.0g      118m  118m   2m 5.1g 109m  72m  38m  0k   0k 44m   7m  0  0  4  11k 0k  21 130m   7m   3m   3m 320k 224k  21m   8m 43m 193m  12  1  0 20m  34   31 1330   0   0
//            2022-05-14 14:51:36   54.4g 21.5g  64   0k  0  0 2 22  1  0  1  0  99  0  0 8.0g 16.0g      118m  118m   2m 5.1g 109m  72m  38m  0k   0k 44m   7m  0  0  4  11k 0k  21 130m   7m   3m   3m 320k 224k  21m   8m 43m 193m  12  1  0 20m  34   31 1330   0   0

    // Header regex matcher in text mode. These should catch on all platforms, therefore they only check JVM columns,
    // and only those columns that are unconditionally available (or should be)
    public static final String jvm_header_line0_textmode = ".*---jvm---.*";
    public static final String jvm_header_line1_textmode = ".*-heap-.*-meta-.*-jthr-.*-cldg-.*-cls-.*";
    public static final String jvm_header_line2_textmode = ".*comm.*used.*comm.*used.*gctr.*code.*num.*nd.*cr.*num.*ld.*uld.*";

    // This is supposed to match a header in csv mode. Here I am rather lenient, because analysing regex mismatches is a
    // pain. We later do more strict sanity checks where we check most of the fields anyway.
    public static final String jvm_header_line_csvmode = ".*jvm-heap-comm,jvm-heap-used,jvm-meta-comm,jvm-meta-used.*";

    public static final String timestamp_regex = "\\d{4}+-\\d{2}+-\\d{2}.*\\d{2}:\\d{2}:\\d{2}";

    // sample line: a timestamp, followed by some numbers
    public static final String sample_line_regex_minimal_textmode = timestamp_regex + ".*\\d+.*\\d+.*\\d+.*\\d+.*";
    public static final String sample_line_regex_minimal_csvmode = "\"" + timestamp_regex + "\".*\"\\d+\".*";

    private static void printLinesWithLineNo(String[] lines) {
        int lineno = 0;
        for (String s : lines) {
            System.err.println(lineno + ": " + s);
            lineno++;
        }
    }

    static final boolean alwaysPrint = true;

    private static boolean findMatchesInStrings(String[] lines, String[] regexes) {
        boolean success = false;
        Pattern[] pat = new Pattern[regexes.length];
        for (int i = 0; i < regexes.length; i ++) {
            pat[i] = Pattern.compile(regexes[i]);
        }
        int numMatches = 0;
        int lineNo = 0;
        while (lineNo < lines.length && numMatches < pat.length) {
            if (pat[numMatches].matcher(lines[lineNo]).matches()) {
                numMatches ++;
            }
            lineNo ++;
        }
        success = (numMatches == pat.length);
        if (!success) {
            System.err.println("Matched " + numMatches + "/" + pat.length + " pattern. First unmatched: " + pat[numMatches]);
        } else {
            System.err.println("Matched " + numMatches + "/" + pat.length + " pattern. OK!");
        }
        if (!success || alwaysPrint) {
            System.err.println("Lines: ");
            printLinesWithLineNo(lines);
            System.err.println("Pattern: ");
            printLinesWithLineNo(regexes);
        }
        return success;
    }

    static final String[] expected_output_textmode = new String[] {
            jvm_header_line0_textmode, jvm_header_line1_textmode, jvm_header_line2_textmode, sample_line_regex_minimal_textmode
    };

    static final String[] expected_output_csvmode = new String[] {
            jvm_header_line_csvmode, sample_line_regex_minimal_csvmode
    };

    static void fileShouldMatch(File f, String[] expected) throws IOException {
        Path path = Paths.get(f.getAbsolutePath());
        String[] lines = Files.readAllLines(path).toArray(new String[0]);
        if (!findMatchesInStrings(lines, expected)) {
            throw new RuntimeException("Expected output not found (see error output)");
        }
    }

    public static void fileMatchesVitalsTextMode(File f) throws IOException {
        fileShouldMatch(f, expected_output_textmode);
    }

    public static void fileMatchesVitalsCSVMode(File f) throws IOException {
        fileShouldMatch(f, expected_output_csvmode);
    }

    public static void outputMatchesVitalsTextMode(OutputAnalyzer output) {
        String[] lines = output.asLines().toArray(new String[0]);
        if (!findMatchesInStrings(lines, expected_output_textmode)) {
            throw new RuntimeException("Expected output not found (see error output)");
        }
    }

    public static void outputMatchesVitalsCSVMode(OutputAnalyzer output) {
        String[] lines = output.asLines().toArray(new String[0]);
        if (!findMatchesInStrings(lines, expected_output_csvmode)) {
            throw new RuntimeException("Expected output not found (see error output)");
        }
    }

    // Some more extensive sanity checks are possible in CSV mode
    public static CSVParser.CSV parseCSV(OutputAnalyzer output) throws CSVParser.CSVParseException {
        String[] lines = output.asLines().toArray(new String[0]);

        // Search for the beginning of the CSV output
        int firstline = -1;
        int lastline = -1;
        Pattern headerLinePattern = Pattern.compile(jvm_header_line_csvmode);
        Pattern csvDataLinePattern = Pattern.compile(sample_line_regex_minimal_csvmode);
        for (int lineno = 0; lineno < lines.length && firstline == -1 && lastline == -1; lineno ++) {
            String line = lines[lineno];
            if (firstline == -1) {
                if (headerLinePattern.matcher(line).matches()) {
                    firstline = lineno;
                }
            } else {
                if (headerLinePattern.matcher(line).matches()) {
                    throw new CSVParser.CSVParseException("Found header twice", lineno);
                }
                if (!csvDataLinePattern.matcher(line).matches()) {
                    lastline = lineno - 1;
                    break;
                }
            }
        }
        if (lastline == -1) {
            lastline = lines.length - 1;
        }

        if (firstline == -1) {
            throw new CSVParser.CSVParseException("Could not find CSV header line");
        }

        String [] csvlines = Arrays.copyOfRange(lines, firstline, lastline + 1);

        CSVParser.CSV csv;
        csv = CSVParser.parseCSV(csvlines);

        return csv;
    }

    /**
     * Does some more extensive tests on a csv raw output. Requires output to be done with scale=1 or raw mode
     * @param csv
     */
    static public void simpleCSVSanityChecks(CSVParser.CSV csv) throws CSVParser.CSVParseException {

        // The following columns are allowed to be empty (column shown but values missing), e.g.
        // for delta columns
        // Note: data that are always missing (e.g. because of linux kernel version) should have
        // their columns hidden instead, see vitals.cpp)
        String colsThatCanBeEmpty =
                  "|jvm-jthr-cr"  // delta column
                + "|syst-si|syst-so" // deltas
                + "|jvm-cls-ld" // delta
                + "|jvm-cls-uld" // delta
                ;

        String colsThatCanBeEmpty_WINDOWS = "";

        String colsThatCanBeEmpty_OSX = "";

        String colsThatCanBeEmpty_LINUX =
                  "|syst-avail" // Older kernels < 3.14 miss this value
                + "|syst-cpu.*" // CPU values may be omitted in containers; also they are all deltas
                + "|syst-cgr.*" // Cgroup values may be omitted in root cgroup
                + "|proc-chea-usd|proc-chea-free" // cannot be shown if RSS is > 4g and glibc is too old
                + "|proc-io-rd|proc-io-wr" // deltas
                + "|proc-cpu-us|proc-cpu-sy" // deltas
                ;

        String regexCanBeEmpty = "(x" + colsThatCanBeEmpty;
        if (Platform.isLinux()) {
            regexCanBeEmpty += colsThatCanBeEmpty_LINUX;
        } else if (Platform.isWindows()) {
            regexCanBeEmpty += colsThatCanBeEmpty_WINDOWS;
        } else if (Platform.isOSX()) {
            regexCanBeEmpty += colsThatCanBeEmpty_OSX;
        }
        regexCanBeEmpty += ")";

        System.out.println("Columns allowed to be empty: " + regexCanBeEmpty);
        Pattern canBeEmptyPattern = Pattern.compile(regexCanBeEmpty);

        for (int lineno = 0; lineno < csv.lines.length; lineno ++) {
            CSVParser.CSVDataLine line = csv.lines[lineno];
            // Iterate through all columns and do some basic checks.
            // In raw mode, all but the first column are longs. The first column is a time stamp.
            for (int i = 1; i < csv.header.size(); i ++) {
                String col = csv.header.at(i);
                if (line.isEmpty(i)) {
                    // aka empty
                    if (!canBeEmptyPattern.matcher(col).matches()) {
                        throw new CSVParser.CSVParseException("Column " + col + " must not have empty value.", lineno + 1);
                    }
                } else {
                    long l = 0;
                    try {
                        l = line.numberAt(i);
                    } catch (NumberFormatException e) {
                        throw new CSVParser.CSVParseException("Column " + col + ": cannot parse value as long (" + l + ")", lineno + 1);
                    }
                    long highestReasonableRawValue = 0x00800000_00000000l;
                    if (l < 0 || l > highestReasonableRawValue) {
                        throw new CSVParser.CSVParseException("Column " + col + ": Suspiciously high or low value:" + l, lineno + 1);
                    }
                }
            }
        }
    }
}
