---
layout: default
title: SapMachine
---

<img align="right" width="350" src="assets/images/logo_circular.png">

# [](#SapMachine) SapMachine
This project contains a downstream version of the [OpenJDK](http://openjdk.java.net/) project. It is used to build and maintain a SAP supported version of OpenJDK for SAP customers and partners who wish to use OpenJDK to run their applications.

We want to stress that this is clearly a "*friendly fork*". SAP is committed to ensuring the continued success of the Java platform:
* We are members of the [JCP Executive committee](https://jcp.org/en/participation/committee) since 2001 and recently served in the [JSR 379 (Java SE 9)](https://www.jcp.org/en/jsr/detail?id=379) and [JSR 383 (Java SE 18.3)](https://www.jcp.org/en/jsr/detail?id=383) Expert Groups. 
* SAP is among the [biggest external contributors](https://blogs.oracle.com/java-platform-group/building-jdk-11-together) to the OpenJDK project (currently leading the [PowerPC/AIX](http://openjdk.java.net/projects/ppc-aix-port/) and [s390](http://openjdk.java.net/projects/s390x-port/) porting projects)

* We intend to bring as many features as possible into the upstream project and keep the diff of this project as small as possible.

## [](#Download) Download

You can check for all available [releases](https://github.com/SAP/SapMachine/releases) (including nightly snapshots).
The latest release for any SapMachine major version can be found at `https://sap.github.io/SapMachine/latest/#MAJOR` (e.g. [SapMachine 11](latest/11)). Or you can download binary archives here:

<select id="sapmachine_imagetype_select" class="download_select">
</select>

<select id="sapmachine_os_select" class="download_select">
</select>

<select id="sapmachine_version_select" class="download_select">
</select>

<button id="sapmachine_download_button" type="button" class="download_button">Download</button>

## [](#Documentation) Documentation
Check out our a [wikipages](https://github.com/SAP/SapMachine/wiki) with various information about:

* [Installation](https://github.com/SAP/SapMachine/wiki/Installation)
* [Docker Images](https://github.com/SAP/SapMachine/wiki/Docker-Images)
* [Supported Platforms](https://github.com/SAP/SapMachine/wiki/Supported-platforms)
* [Certifications and Java Compatibility](https://github.com/SAP/SapMachine/wiki/Certification-and-Java-Compatibility)
* [Differences between SapMachine and OpenJDK](https://github.com/SAP/SapMachine/wiki/Differences-between-SapMachine-and-OpenJDK)
* [Features Contributed by SAP](https://github.com/SAP/SapMachine/wiki/Features-Contributed-by-SAP)
* [SapMachine Development Process](https://github.com/SAP/SapMachine/wiki/SapMachine-Development-Process)

## [](#Have) Have an issue or question...? 
Please file a [new issue](https://github.com/SAP/SapMachine/issues/new) and let us know about.

## [](#License) License
This project is run under the same licensing terms as the upstream OpenJDK project. Please see the [LICENSE](LICENSE) file in the top-level directory for more information.
