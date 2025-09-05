/*
 * Copyright (c) 2025 SAP SE. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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

package jdk.tools.jlink.internal.plugins;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

import jdk.tools.jlink.internal.ExecutableImage;
import jdk.tools.jlink.internal.Platform;
import jdk.tools.jlink.internal.PostProcessor;
import jdk.tools.jlink.plugin.PluginException;
import jdk.tools.jlink.plugin.ResourcePool;
import jdk.tools.jlink.plugin.ResourcePoolBuilder;

/**
 * Adds tools that are SapMachine specific
 */
public class AddSapMachineTools extends AbstractPlugin implements PostProcessor {

    public AddSapMachineTools() {
        super("add-sapmachine-tools");
    }

    @Override
    public Category getType() {
        return Category.ADDER;
    }

    @Override
    public boolean hasArguments() {
        return false;
    }

    @Override
    public boolean hasRawArgument() {
        return false;
    }

    private final String[] tools = {
            "bin/asprof",
            "lib/" + System.mapLibraryName("asyncProfiler"),
            "lib/async-profiler.jar",
            "lib/converter.jar",
            "legal/async/CHANGELOG.md",
            "legal/async/LICENSE",
            "legal/async/README.md"
    };

    @Override
    public List<String> process(ExecutableImage image) {
        var targetPlatform = image.getTargetPlatform();
        var runtimePlatform = Platform.runtime();

        if (!targetPlatform.equals(runtimePlatform)) {
            throw new PluginException("Cannot add SapMachine tools: target image platform " +
                    targetPlatform.toString() + " is different from runtime platform " +
                    runtimePlatform.toString());
        }

        var sourceJavaHome = Path.of(System.getProperty("java.home"));
        var targetJavaHome = image.getHome();

        for (String tool : tools) {
            var path = sourceJavaHome.resolve(tool);
            var target = targetJavaHome.resolve(tool);
            if (Files.exists(path)) {
                try {
                    Files.createDirectories(target.getParent());
                    Files.copy(path, target);
                } catch (IOException e) {
                    throw new UncheckedIOException(e);
                }
            }
        }

        return null;
    }

    @Override
    public ResourcePool transform(ResourcePool in, ResourcePoolBuilder out) {
        return in;
    }
}
