---
layout: default
title: SapMachine
---

**SapMachine** is a distribution of the [OpenJDK](https://openjdk.org/) maintained by <a href="https://sap.com">SAP</a>. It is designed to be free, cross-platform, production-ready, and [Java SE certified](https://github.com/SAP/SapMachine/wiki/Certification-and-Java-Compatibility). This distribution serves as the default Java Runtime for SAP's numerous applications and services.

<img align="left" width="240" src="assets/images/logo_circular.svg" alt="Logo of SapMachine">

SapMachine supports all major operating systems.
It comes with long-term support releases that include bug fixes and performance updates; you can learn more about support and maintenance in our [Wiki](https://github.com/SAP/SapMachine/wiki/Maintenance-and-Support).

Our goal is to keep SapMachine as close to OpenJDK as possible,
only adding additional features when absolutely necessary; you can find a list of these in the [Wiki](https://github.com/SAP/SapMachine/wiki/Differences-between-SapMachine-and-OpenJDK).

Our team's many contributions to the OpenJDK and the ecosystem include the [PowerPC/AIX support](http://openjdk.java.net/projects/ppc-aix-port/), [elastic Metaspace](https://openjdk.org/jeps/387),
 [helpful NullPointerExceptions](https://openjdk.org/jeps/358), a [website for JFR events](https://sap.github.io/SapMachine/jfrevents/) and more.

## Download

In the following, you find downloads of SapMachine and our build of [JDK Mission Control (JMC)](https://openjdk.org/projects/jmc/):

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

Check out our [FAQ's](https://github.com/SAP/SapMachine/wiki/Frequently-Asked-Questions) and [Wiki pages](https://github.com/SAP/SapMachine/wiki) for more information, including:

* [Installation](https://github.com/SAP/SapMachine/wiki/Installation) and [Docker Images](https://github.com/SAP/SapMachine/wiki/Docker-Images)
* [Maintenance and Support](https://github.com/SAP/SapMachine/wiki/Maintenance-and-Support)
* [Certifications and Java Compatibility](https://github.com/SAP/SapMachine/wiki/Certification-and-Java-Compatibility)
* [Differences between SapMachine and OpenJDK](https://github.com/SAP/SapMachine/wiki/Differences-between-SapMachine-and-OpenJDK)
