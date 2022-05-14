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

    // JVM section: this one is valid on all platforms
    // (Note: no compressed class space on 32bit platforms)
    public static final String jvm_header_line1_textmode = ".*heap[ -]+meta[ -]+nmt[ -]+jthr[ -]+cldg[ -]+cls.*";
    public static final String jvm_header_line2_textmode = ".*comm[ -]+used[ -]+comm[ -]+used[ -]+.*gctr[ -]+code[ -]+mlc[ -]+map[ -]+num[ -]+nd[ -]+cr[ -]+st[ -]+num[ -]+anon[ -]+num[ -]+ld[ -]+uld.*";
    public static final String jvm_header_line_csvmode = ".*jvm-heap-comm,jvm-heap-used,jvm-meta-comm,jvm-meta-used.*jvm-meta-gctr,jvm-code,jvm-nmt-mlc,jvm-nmt-map,jvm-jthr-num,jvm-jthr-nd,jvm-jthr-cr,jvm-jthr-st,jvm-cldg-num,jvm-cldg-anon,jvm-cls-num,jvm-cls-ld,jvm-cls-uld,";

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
            jvm_header_line1_textmode, jvm_header_line2_textmode, sample_line_regex_minimal_textmode
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
    public static CSVParser.CSV parseAndCheckCSV(OutputAnalyzer output, boolean outputIsRaw) throws CSVParser.CSVParseException {
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

        if (outputIsRaw) {
            csvSanityChecks(csv);
        }

        return csv;
    }

    /**
     * does some more extensive tests on a csv raw output
     * @param csv
     */
    static void csvSanityChecks(CSVParser.CSV csv) throws CSVParser.CSVParseException {

        // The following columns are allowed to be empty (they are delta columns, or their values depend on runtime
        // conditions not always given)
        String colsThatCanBeEmpty =
                  "|jvm-jthr-cr"  // delta column
                + "|syst-si|syst-so" // deltas
                + "|jvm-cls-ld" // delta
                + "|jvm-cls-uld" // delta
                + "|jvm-nmt.*" // only available if nmt is on
                ;

        String colsThatCanBeEmpty_WINDOWS =
                  "|jvm-jthr-st"; // NMT stack size display switched off deliberately on Windows

        String colsThatCanBeEmpty_OSX =
                  "|jvm-jthr-st"; // NMT stack size display switched off deliberately on Mac

        String colsThatCanBeEmpty_LINUX =
                  "|syst-avail" // depends on kernel version
                + "|syst-cpu.*" // CPU values may be omitted in containers; also they are all deltas
                + "|syst-cgr.*" // Cgroup values may be omitted in root cgroup
                + "|proc-chea-usd|proc-chea-free" // cannot be shown if RSS is > 4g and glibc is too old
                + "|proc-rss-anon|proc-rss-file|proc-rss-shm" // depend on kernel
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
                String val = line.at(i);
                if (val.equals("?")) {
                    // aka empty
                    if (!canBeEmptyPattern.matcher(col).matches()) {
                        throw new CSVParser.CSVParseException("Column " + col + " must not have empty value.", lineno + 1);
                    }
                } else {
                    long l = 0;
                    try {
                        l = Long.parseLong(val);
                    } catch (NumberFormatException e) {
                        throw new CSVParser.CSVParseException("Column " + col + ": cannot parse value as long (" + val + ")", lineno + 1);
                    }
                    long highestReasonableRawValue = 0x00800000_00000000l;
                    if (l < 0 || l > highestReasonableRawValue) {
                        throw new CSVParser.CSVParseException("Column " + col + ": Suspiciously high or low value:" + val, lineno + 1);
                    }
                }
            }
        }
    }
}
