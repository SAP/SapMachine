---
layout: default
title: SapMachine
---

**SapMachine** is a free, open-source, non-commercial, cross-platform, and fully-functional production-grade version of the Open Java Development Kit (OpenJDK).
Built and supported by SAP, it provides a robust and reliable Java environment.

<img align="left" width="240" src="assets/images/logo_circular.svg" alt="Logo of SapMachine">

Following the release cadence of the OpenJDK, a new feature release is shipped every six months, with one designated as a long-term support release every two years.
Update releases for active versions are provided every quarter. So SapMachine offers long-term support, regular performance improvements and timely security updates.

For SAP, the default JDK is SapMachine. It is the engine of countless applications and services, in cloud deployments as well as standalone on-premise installations at both, SAP and its customers.

SapMachine is available on a variety of Operating System/CPU architecture combinations, and it is certified to adhere to the Java SE standard.
It can serve as a drop-in replacement for any JDK in Java-based applications, ranging from small desktop applications to high-performance, large-scale server applications.

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

Check out our [FAQ's](https://github.com/SAP/SapMachine/wiki/Frequently-Asked-Questions) and [wikipages](https://github.com/SAP/SapMachine/wiki) for information about:

* [Installation](https://github.com/SAP/SapMachine/wiki/Installation) and [Docker Images](https://github.com/SAP/SapMachine/wiki/Docker-Images)
* [Certifications and Java Compatibility](https://github.com/SAP/SapMachine/wiki/Certification-and-Java-Compatibility)
* [Features contributed by SAP to OpenJDK](https://github.com/SAP/SapMachine/wiki/Features-Contributed-by-SAP)
* [Differences between SapMachine and OpenJDK](https://github.com/SAP/SapMachine/wiki/Differences-between-SapMachine-and-OpenJDK).

## License

This project is OpenSource and as an OpenJDK-distribution, SapMachine is free to use for everyone accepting the [GPLv2 license](https://github.com/SAP/SapMachine/blob/sapmachine/LICENSE).

<hr>

2017-2024 by [SAP SE](https://www.sap.com)
