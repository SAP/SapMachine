---
layout: default
title: SapMachine
---

<img align="right" width="350" src="assets/images/logo_circular.svg">

# SapMachine
This project contains a downstream version of the [OpenJDK](http://openjdk.java.net/) project. It is used to build and maintain a SAP supported version of OpenJDK for SAP customers and partners who wish to use OpenJDK to run their applications.

We want to stress that this is clearly a "*friendly fork*". SAP is committed to ensuring the continued success of the Java platform. SAP is: 

* A member of the [JCP Executive committee](https://jcp.org/en/participation/committee) since 2001 and recently served in the [JSR 379 (Java SE 9)](https://www.jcp.org/en/jsr/detail?id=379), [JSR 383 (Java SE 18.3)](https://www.jcp.org/en/jsr/detail?id=383), [JSR 384 (Java SE 11)](https://www.jcp.org/en/jsr/detail?id=384), [JSR 386 (Java SE 12)](https://www.jcp.org/en/jsr/detail?id=386), [JSR 388 (Java SE 13)](https://www.jcp.org/en/jsr/detail?id=388), [JSR 389 (Java SE 14)](https://www.jcp.org/en/jsr/detail?id=389), [JSR 390 (Java SE 15)](https://www.jcp.org/en/jsr/detail?id=390), [JSR 391 (Java SE 16)](https://www.jcp.org/en/jsr/detail?id=391), [JSR 392 (Java SE 17)](https://www.jcp.org/en/jsr/detail?id=392), [JSR 393 (Java SE 18)](https://www.jcp.org/en/jsr/detail?id=393), [JSR 394 (Java SE 19)](https://www.jcp.org/en/jsr/detail?id=394), [JSR 395 (Java SE 20)](https://www.jcp.org/en/jsr/detail?id=395), [JSR 396 (Java SE 21)](https://www.jcp.org/en/jsr/detail?id=396) and [JSR 397 (Java SE 22)](https://www.jcp.org/en/jsr/detail?id=397) Expert Groups.

* Among the biggest external contributors to the OpenJDK project (see fix ratio for OpenJDK [11](https://blogs.oracle.com/java-platform-group/building-jdk-11-together), [12](https://blogs.oracle.com/java-platform-group/the-arrival-of-java-12), [13](https://blogs.oracle.com/java-platform-group/the-arrival-of-java-13), [14](https://blogs.oracle.com/java-platform-group/the-arrival-of-java-14), [15](https://blogs.oracle.com/java-platform-group/the-arrival-of-java-15), [16](https://inside.java/2021/03/16/the-arrival-of-java16/), [17](https://inside.java/2021/09/14/the-arrival-of-java17/), [18](https://inside.java/2022/03/22/the-arrival-of-java18/), [19](https://inside.java/2022/09/20/the-arrival-of-java-19/), [20](https://inside.java/2023/03/21/the-arrival-of-java-20/), [21](https://inside.java/2023/09/19/the-arrival-of-java-21/)).

* Leading the [OpenJDK 17 updates project](https://wiki.openjdk.java.net/display/JDKUpdates/JDK+17u) and heavily supporting the [OpenJDK 11](https://wiki.openjdk.java.net/display/JDKUpdates/JDK11u) and [OpenJDK 21](https://wiki.openjdk.java.net/display/JDKUpdates/JDK+21u) updates projects.

* Leading the [PowerPC/AIX porting project](http://openjdk.java.net/projects/ppc-aix-port/).

* Contributing many new features inspired by Java stakeholders within SAP to the OpenJDK project. This ensures such features are available in long reach and for everybody. Rarely we add such features to SapMachine directly to keep the diff of this project as small as possible. 

* Creating tools for developers
    * [JFR Event Collection](https://sapmachine.io/jfrevents/): Information on all JFR events for a specific JDK
    * [AP-Loader](https://github.com/jvm-profiling-tools/ap-loader): AsyncProfiler in a single cross-platform JAR
* [Blogging](https://github.com/SAP/SapMachine/wiki/Blogs) and [Presenting](https://github.com/SAP/SapMachine/wiki/Presentations) at Java conferences all over the globe 
    

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
  <input type="checkbox" id="sapmachine_lts_checkbox" name="lts"
         checked>
  <label for="lts">Long Term Support Releases (LTS)</label>

  <input type="checkbox" id="sapmachine_nonlts_checkbox" name="nonlts"
         checked>
  <label for="nonlts">Short Term Support Releases</label>

  <input type="checkbox" id="sapmachine_ea_checkbox" name="ea">
  <label for="ea">Pre-Releases</label>
</div>

## Releases

All [releases](https://github.com/SAP/SapMachine/releases), including nightly snapshots, are available on GitHub.
The latest release for any SapMachine major version can be found at `https://sap.github.io/SapMachine/latest/#MAJOR` (e.g. [SapMachine 21](latest/21)).

## Documentation
Check out our [FAQ's](https://github.com/SAP/SapMachine/wiki/Frequently-Asked-Questions) and [wikipages](https://github.com/SAP/SapMachine/wiki) for information about:
* [Installation](https://github.com/SAP/SapMachine/wiki/Installation) and [Docker Images](https://github.com/SAP/SapMachine/wiki/Docker-Images)
* [Certifications and Java Compatibility](https://github.com/SAP/SapMachine/wiki/Certification-and-Java-Compatibility)

## License
This project is run under the same licensing terms as the upstream OpenJDK project. Please see the [LICENSE](https://github.com/SAP/SapMachine/blob/sapmachine/LICENSE) file in the top-level directory for more information.
