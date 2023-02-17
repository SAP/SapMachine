import java.io.File;
import java.io.IOException;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

class ValuesFromProcFS {

    // from proc meminfo
    public long MemAvail = -1;
    public long Committed_AS = -1;
    public long SwapTotal = -1;
    public long SwapFree = -1;

    private static Pattern patMemAvail = Pattern.compile("MemAvail: *(\\d+) *kB");
    private static Pattern patCommitted_AS = Pattern.compile("Committed_AS: *(\\d+) *kB");
    private static Pattern patSwapTotal = Pattern.compile("SwapTotal: *(\\d+) *kB");
    private static Pattern patSwapFree = Pattern.compile("SwapFree: *(\\d+) *kB");

    // from proc pid status
    public long VmRSS = -1;
    public long VmSwap = -1;
    public long VmSize = -1;

    private static Pattern patVmRSS = Pattern.compile("VmRSS: *(\\d+) *kB");
    private static Pattern patVmSize = Pattern.compile("VmSize: *(\\d+) *kB");
    private static Pattern patVmSwap = Pattern.compile("VmSwap: *(\\d+) *kB");

    /**
     * Retrieve data from proc fs
     *
     * @param pid if != -1, return some data for the process too
     * @return
     * @throws IOException
     */
    public static ValuesFromProcFS retrieveForProcess(long pid) throws IOException {
        ValuesFromProcFS v = new ValuesFromProcFS();

        if (pid != -1) {
            String lines[] = VitalsUtils.fileAsLines(new File("/proc/" + pid + "/status"));
            for (String s : lines) {
                Matcher m = patVmRSS.matcher(s);
                if (m.matches()) {
                    v.VmRSS = Long.parseLong(m.group(1)) * 1024;
                }
                m = patVmSize.matcher(s);
                if (m.matches()) {
                    v.VmSize = Long.parseLong(m.group(1)) * 1024;
                }
                m = patVmSwap.matcher(s);
                if (m.matches()) {
                    v.VmSwap = Long.parseLong(m.group(1)) * 1024;
                }
            }
        }

        String lines[] = VitalsUtils.fileAsLines(new File("/proc/meminfo"));
        for (String s : lines) {
            Matcher m = patMemAvail.matcher(s);
            if (m.matches()) {
                v.MemAvail = Long.parseLong(m.group(1)) * 1024;
            }
            m = patCommitted_AS.matcher(s);
            if (m.matches()) {
                v.Committed_AS = Long.parseLong(m.group(1)) * 1024;
            }
            m = patSwapTotal.matcher(s);
            if (m.matches()) {
                v.SwapTotal = Long.parseLong(m.group(1)) * 1024;
            }
            m = patSwapFree.matcher(s);
            if (m.matches()) {
                v.SwapFree = Long.parseLong(m.group(1)) * 1024;
            }
        }

        return v;
    }
}
