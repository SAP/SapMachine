---
layout: default
title: SapMachine
---

**SapMachine** is the free, multiplatform, production-ready, and [Java SE certified](https://github.com/SAP/SapMachine/wiki/Certification-and-Java-Compatibility) [OpenJDK](https://openjdk.org/) (Open Java Development Kit) distribution by [SAP](https://sapmachine.io). It serves as the default JDK for SAP's countless applications and services.

<img align="left" width="240" src="assets/images/logo_circular.svg" alt="Logo of SapMachine">

This Distribution supports all major Operation Systems and CPU architectures, including PowerPC and AIX.
SapMachine comes with long-term support releases that include bug fixes and performance updates; you can learn more about support and maintenance in our [wiki](https://github.com/SAP/SapMachine/wiki/Maintenance-and-Support).

Our goal is to keep SapMachine as close to OpenJDK as possible,
only adding additional features when absolutely necessary; you can find a list of these
in the [wiki](https://github.com/SAP/SapMachine/wiki/Differences-between-SapMachine-and-OpenJDK). One major difference is that we started including [async-profiler](https://github.com/jvm-profiling-tools/async-profiler) in our distribution.

Our team's many contributions include the [PowerPC/AIX support](http://openjdk.java.net/projects/ppc-aix-port/), [helpful NullPointerExceptions](https://openjdk.org/jeps/358), a [website for JFR events](https://sap.github.io/SapMachine/jfrevents/), our own [IntelliJ Profiler plugin](https://plugins.jetbrains.com/plugin/20937-java-jfr-profiler), and more.

## Download

<select id="sapmachine_major_select" class="download_select">
</select>

<select id="sapmachine_imagetype_select" class="download_select">
</select>

<select id="sapmachine_os_select" class="download_select">
</select>

<select id="sapmachine_version_select" class="download_select">
</select>

<button id="sapmachine_download_button" type="button" class="download_button">Download</button>

<div class="download_label_section">
  <div id="download_label" class="download_label"></div>
  <button id="sapmachine_copy_button" type="button" class="download_button">Copy Download URL</button>
</div>

<div class="download_filter">
  <input type="checkbox" id="sapmachine_lts_checkbox" name="lts" checked>
  <label for="lts">Long Term Support Releases (LTS)</label>

  <input type="checkbox" id="sapmachine_nonlts_checkbox" name="nonlts" checked>
  <label for="nonlts">Short Term Support Releases</label>

  <input type="checkbox" id="sapmachine_ea_checkbox" name="ea">
  <label for="ea">Pre-Releases</label>
</div>

## Documentation

Check out our [FAQ's](https://github.com/SAP/SapMachine/wiki/Frequently-Asked-Questions) and [wikipages](https://github.com/SAP/SapMachine/wiki) for more information, including:

* [Installation](https://github.com/SAP/SapMachine/wiki/Installation) and [Docker Images](https://github.com/SAP/SapMachine/wiki/Docker-Images)
* [Maintenance and Support](https://github.com/SAP/SapMachine/wiki/Maintenance-and-Support)
* [Certifications and Java Compatibility](https://github.com/SAP/SapMachine/wiki/Certification-and-Java-Compatibility)
* [Differences between SapMachine and OpenJDK](https://github.com/SAP/SapMachine/wiki/Differences-between-SapMachine-and-OpenJDK)
* [License](https://github.com/SAP/SapMachine/blob/sapmachine/LICENSE)

<hr>

2017-2024 by [SAP SE](https://www.sap.com)
