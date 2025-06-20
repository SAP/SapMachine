import jdk.test.lib.process.OutputAnalyzer;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class VitalsUtils {

    static String outputDir;
    static File outputDirAsFile;

    static {
        outputDir = System.getProperty("user.dir", ".");
        outputDirAsFile = new File(outputDir);
    }

    static File createSubTestDir(String name, boolean create) {
        File subdir = new File(outputDir, name);
        if (subdir.exists()) {
            throw new RuntimeException("test dir already exists: " + subdir);
        }
        if (create) {
            subdir.mkdirs();
            if (!subdir.exists()) {
                throw new RuntimeException("Cannot create test dir at " + subdir);
            }
        }
        return subdir;
    }

    static void fileShouldExist(File f) {
        if (!f.exists()) {
            throw new RuntimeException("expected but does not exist: " + f);
        }
    }

    static void fileShouldNotExist(File f) {
        if (f.exists()) {
            throw new RuntimeException("expected not to exist, but exists: " + f);
        }
    }

    static String[] stderrAsLines(OutputAnalyzer output) {
        return output.getStderr().split("\\R");
    }
    static String[] stdoutAsLines(OutputAnalyzer output) {
        return output.getStdout().split("\\R");
    }

    /**
     * Look in lines for a number of subsequent matches. Start looking at startAtLine. Return line number of last
     * match. Throws RuntimeException if not all regexes where matching.
     * @param lines
     * @param startAtLine
     * @param regexes
     * @return
     */
    static int matchPatterns(String[] lines, int startAtLine, String[] regexes) {
        int nextToMatch = 0;
        int nLine = startAtLine;
        while (nLine < lines.length) {
            if (lines[nLine].matches(regexes[nextToMatch])) {
                System.out.println("Matched \"" + regexes[nextToMatch] +"\" at line " + nLine + "(\"" + lines[nLine] + "\")");
                nextToMatch++;
                if (nextToMatch == regexes.length) {
                    break;
                }
            }
            nLine ++;
        }
        if (nextToMatch < regexes.length) {
            throw new RuntimeException("Not all matches found. First missing pattern " + nextToMatch + ":" + regexes[nextToMatch]);
        }
        return nLine;
    }

    static int outputStderrMatchesPatterns(OutputAnalyzer output, String[] regexes) {
        return matchPatterns(stderrAsLines(output), 0, regexes);
    }

    static int outputStdoutMatchesPatterns(OutputAnalyzer output, String[] regexes) {
        return matchPatterns(stdoutAsLines(output), 0, regexes);
    }

    static String [] fileAsLines(File f) throws IOException {
        Path path = Paths.get(f.getAbsolutePath());
        return Files.readAllLines(path).toArray(new String[0]);
    }

    static void assertFileExists(File f) {
        if (!f.exists()) {
            throw new RuntimeException("File " + f.getAbsolutePath() + " does not exist");
        } else {
            System.out.println("File " + f.getAbsolutePath() + " exists - ok!");
        }
    }

    static void assertFileExists(String filename) {
        File f = new File(filename);
        assertFileExists(f);
    }

    static void assertFileisEmpty(File f) {
        if (f.length() > 0) {
            throw new RuntimeException("File " + f.getAbsolutePath() + " expected to be empty, but is not empty.");
        } else {
            System.out.println("File " + f.getAbsolutePath() + " has zero size - ok!");
        }
    }

    static void assertFileisEmpty(String filename) {
        File f = new File(filename);
        assertFileisEmpty(f);
    }

    static void assertFileContentMatches(File f, String[] regexes) throws IOException {
        String[] lines = fileAsLines(f);
        matchPatterns(lines, 0, regexes);
        System.out.println("File " + f.getAbsolutePath() + " matches " + regexes[0] + ", ... etc - ok!");
    }

    static File findFirstMatchingFileInDirectory(File dir, String regex) {
        File[] files = dir.listFiles();
        for (File f : files) {
            if (f.getName().matches(regex)) {
                System.out.println("Found required file " + f.getAbsolutePath() + " - ok!");
                return f;
            }
        }
        throw new RuntimeException("Could not find file matching \"" + regex + "\" inside " + dir.getAbsolutePath());
    }

    // Extract pid of target process (first line of output shall contain "<pid>:\n"
    private static Pattern patPidInJcmdOutput = Pattern.compile("^(\\d+):\r");
    static long pidFromJcmdOutput(OutputAnalyzer output) {
        String s = output.getOutput();
        Matcher m = patPidInJcmdOutput.matcher(s);
        if (m.matches()) {
            return Long.parseLong(m.group(1));
        }
        throw new RuntimeException("Cannot find pid in jcmd output");
    }
}
