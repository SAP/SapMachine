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

import java.util.ArrayList;
import java.util.Hashtable;
import java.util.List;

public class CSVParser {

    public final static class CSVHeader {
        final List<String> columns = new ArrayList<>();
        Hashtable<String, Integer> columnPositions = new Hashtable<>();

        int size() {
            return columns.size();
        }

        String at(int position) {
            return columns.get(position);
        }

        int findColumn(String name) {
            Integer i = columnPositions.get(name);
            return (i == null) ? -1 : i;
        }

        boolean hasColumn(String name) {
            return findColumn(name) != -1;
        }

        void addColumn(String name) {
            if (columnPositions.containsKey(name)) {
                throw new RuntimeException("Already have column " + name);
            }
            columns.add(name);
            columnPositions.put(name, columns.size() - 1);
        }

        @Override
        public String toString() {
            StringBuilder bld = new StringBuilder();
            for (String s : columns) {
                bld.append(s);
                bld.append(",");
            }
            return bld.toString();
        }
    }

    public final static class CSVDataLine {
        ArrayList<String> data = new ArrayList<>();

        int size() {
            return data.size();
        }

        String at(int position) {
            return data.get(position);
        }

        boolean isEmpty(int position) {
            String s = at(position);
            return s == null || s.isEmpty() || s.equals("?");
        }

        long numberAt(int position) throws NumberFormatException {
            if (isEmpty(position)) {
                throw new RuntimeException("no data at position " + position);
            }
            return Long.parseLong(at(position));
        }

        void addData(String s) {
            // If data was surrounded by quotes, remove quotes
            if (s.startsWith("\"") && s.endsWith("\"")) {
                s = s.substring(1, s.length() - 1);
            }
            s = s.trim();
            data.add(s);
        }

        @Override
        public String toString() {
            StringBuilder bld = new StringBuilder();
            for (String s : data) {
                bld.append(s);
                bld.append(",");
            }
            return bld.toString();
        }
    }

    public final static class CSV {
        public CSVHeader header;
        public CSVDataLine[] lines;

        // Convenience function. Given a column name and a data line number, return value for that column in that line
        String getContentOfCell(String columnname, int lineno) {
            return lines[lineno].at(header.findColumn(columnname));
        }

        long getContentOfCellAsNumber(String columnname, int lineno) {
            return Long.parseLong(getContentOfCell(columnname, lineno));
        }

        @Override
        public String toString() {
            StringBuilder builder = new StringBuilder();
            builder.append("CSV: " + header.size() + " columns, " + lines.length + " data lines.\n");
            builder.append(header);
            builder.append("\n");
            for (CSVDataLine line : lines) {
                builder.append(line);
                builder.append("\n");
            }
            return builder.toString();
        }
    }

    public static class CSVParseException extends Exception {
        public CSVParseException(String message) {
            super("CSV parse error " + ": " + message);
        }
        public CSVParseException(String message, int errorLine) {
            super("CSV parse error at line " + errorLine + ": " + message);
        }
    }

    /**
     * Parses the given lines as CSV. The first lines must be the header, all subsequent lines
     * valid data.
     * @param lines
     * @return
     */
    public static final CSV parseCSV(String [] lines) throws CSVParseException {

        if (lines.length < 2) {
            throw new CSVParseException("Not enough data", -1);
        }

        System.out.println("--- CSV parser input: ---");
        for (String s : lines) {
            System.out.println(s);
        }
        System.out.println("--- /CSV parser input: ---");

        int lineno = 0;
        CSVHeader header = new CSVHeader();
        ArrayList<CSVDataLine> datalines = new ArrayList<>();

        try {
            // Parse header line
            String[] parts = lines[lineno].split(",");
            for (String s : parts) {
                header.addColumn(s);
            }

            lineno ++;

            // Parse Data
            while (lineno < lines.length) {
                CSVDataLine dataLine = new CSVDataLine();
                parts = lines[lineno].split(",");
		if (parts.length == 1 && parts[0].isEmpty()) {
			// We start a new section here. Skip the rest.
			break;
		}
                for (String s : parts) {
                    dataLine.addData(s);
                }
                if (dataLine.size() != header.size()) {
                    // We have more or less data than columns. Print some helpful message to stderr, then abort.
                    String s = "Line " + lineno + ": expected " + header.size() + " entries, found " + dataLine.size() + ".";
                    System.err.println(s);
                    System.err.println("Header: " + header.toString());
                    System.err.println("Data: " + dataLine.toString());
                    for (int i = 0; i < header.size() || i < dataLine.size(); i ++) {
                        String col = (i < header.size() ? header.at(i) : "<NULL>");
                        String dat = (i < dataLine.size() ? dataLine.at(i) : "<NULL>");
                        System.err.println("pos: " + i + " column: " + col + " data: " + dat);
                    }
                    throw new CSVParseException(s, lineno);
                }
                datalines.add(dataLine);
                lineno ++;
            }

        } catch (Exception e) {
            System.err.println("--- CSV parse error : " + e.getMessage() + "---");
            e.printStackTrace();
            System.err.println("--- /CSV parse error : " + e.getMessage() + "---");
            throw new CSVParseException(e.getMessage(), lineno);
        }

        CSV csv = new CSV();
        csv.header = header;
        CSVDataLine[] arr = new CSVDataLine[datalines.size()];
        csv.lines = datalines.toArray(arr);

        System.out.println("---- parsed ----");
        System.out.println(csv);
        System.out.println("---- /parsed ----");

        return csv;

    }

}
