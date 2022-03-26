import jdk.test.lib.process.OutputAnalyzer;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.regex.Pattern;


public class VitalsTestHelper {


    // Example output:
    // Last 60 minutes:
    //                      ---------------------------system--------------------------- ------------------------process------------------------ --------------------------------------jvm---------------------------------------
    //                                                                -------cpu--------       -------rss--------          -cpu- ----io-----     --heap--- ----------meta----------           ----jthr----- --cldg-- ----cls-----
    //                      avail comm  crt swap si so p   t    pr pb us sy id  wa st gu virt  all  anon file shm swdo hp  us sy of rd   wr  thr comm used comm used csc  csu  gctr code mlc  num nd cr st  num anon num  ld  uld
    //2022-03-10 07:41:27   55,0g 11,5g  34   0k  0  0 538 1269  1  0  0  0 100  0  0  0 23,2g 3,6g 3,6g  36m  0k   0k 92k  0  0  4 128k  0k  38 1,0g 531m   3m   2m 320k 222k  21m   8m 1,5g  12  1  0 37m  27   24 1286   0   0
    //2022-03-10 07:41:17   55,0g 11,5g  34   0k  0  0 538 1267  1  0  1  0  99  0  0  0 23,2g 3,6g 3,6g  36m  0k   0k 92k  0  0  4 128k 21k  38 1,0g 531m   3m   2m 320k 222k  21m   8m 1,5g  12  1  0 37m  27   24 1286   0   0
    //2022-03-10 07:41:07   55,0g 11,5g  34   0k  0  0 538 1268  1  0  3  0  96  0  0  0 23,2g 3,6g 3,6g  36m  0k   0k 92k  3  0  4 128k  8k  38 1,0g 531m   3m   2m 320k 222k  21m   8m 1,5g  12  1  1 37m  27   24 1286   0   0
    //2022-03-10 07:40:57   56,9g 11,6g  34   0k  0  0 539 1279  4  0  2  0  98  0  0  0 21,2g 1,7g 1,7g  36m  0k   0k 92k  2  0  3 131k <1k  38 1,0g 530m   3m   2m 320k 222k  21m   8m 604m  12  1  1 37m  27   24 1286   0   0
    //2022-03-10 07:40:47   58,0g 11,5g  34   0k  0  0 538 1267  1  0  0  0 100  0  0  0 20,2g 642m 606m  36m  0k   0k 92k  0  0  3 128k  0k  37 1,0g 530m   3m   2m 320k 222k  21m   8m  43m  11  1  0 36m  27   24 1286   0   0
    //2022-03-10 07:40:37   58,0g 11,5g  34   0k  0  0 538 1267  1  0  0  0 100  0  0  0 20,2g 642m 606m  36m  0k   0k 92k  0  0  3 128k  0k  37 1,0g 530m   3m   2m 320k 222k  21m   8m  43m  11  1  0 36m  27   24 1286   0   0
    //2022-03-10 07:40:27   58,0g 11,5g  34   0k  0  0 538 1269  1  0  0  0  99  0  0  0 20,2g 642m 606m  36m  0k   0k 92k  0  0  3 998k <1k  37 1,0g 530m   3m   2m 320k 222k  21m   8m  43m  11  1  5 36m  27   24 1286 842   0
    //2022-03-10 07:40:17   58,5g 11,5g  34   0k       537 1254  4  0                    18,9g  95m  65m  31m  0k   0k 92k        2           18 1,0g  10m 128k  55k  64k   2k  21m   7m  42m   9  1    16m   3    0  444

    // Note: all platforms should have the --jvm-- section. Some platforms have more. Above printout is from linux.
    // For now, we just test for the stuff which is in all platforms.
    public static final String jvm_header_line1_textmode = ".*heap.*meta.*";
    public static final String jvm_header_line2_textmode = ".*comm.*used.*comm.*used.*";

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
}
