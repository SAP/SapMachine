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

## [](#Downloads) Download and Installation

You can check for all available [releases](https://github.com/SAP/SapMachine/releases) (including nightly snapshots).
The latest release for any SapMachine major version can be found at `https://sap.github.io/SapMachine/latest/#MAJOR` (e.g. [SapMachine 11](latest/11)). Or you can download binary archives here:

<select id="sapmachine_imagetype_select" class="download_select">
</select>

<select id="sapmachine_os_select" class="download_select">
</select>

<select id="sapmachine_version_select" class="download_select">
</select>

<button id="sapmachine_download_button" type="button" class="download_button">Download</button>

### [](#Linux) Installation on Linux

Unpack the archives to an arbitrary location in the file system and set `JAVA_HOME` / `PATH` environment variables accordingly.

#### [](#Debian) Debian/ Ubuntu
Alternatively, you can use our `.deb` packages if you're on Debian or Ubuntu:

```
sudo bash
wget -q -O - https://dist.sapmachine.io/debian/sapmachine.key | apt-key add -
echo "deb http://dist.sapmachine.io/debian/amd64/ ./" >> /etc/apt/sources.list
apt-get update
apt-get install sapmachine-11-jre
```

### [](#macOS) Installation on macOS
Unpack the archive (double click in finder) to an arbitrary location in the file system. You may want to move the resulting directory to `/Library/Java/JavaVirtualMachines` (admin privileges required). If you do so, `/usr/libexec/java_home -V` will show SapMashine. Moreover, if SapMachine is the most recent JDK, the `java` command in the shell will use it. You can try this with `java -version`.

If you prefer not to have SapMachine integrated in macOS' Java Framework, you can set `JAVA_HOME` / `PATH` environment variables accordingly and run it from an arbitrary location in the file system.

### [](#Windows) Installation on Windows
SapMachine currently doesn't provied an installer on Windows. Unpack the archive and set `JAVA_HOME` / `PATH` environment variables accordingly.

### [](#Docker) Docker
Finally, we also provide [Docker images](https://hub.docker.com/r/sapmachine) for various versions of the SapMachine.

#### [](#Debian) Debian / Ubuntu
```
docker pull sapmachine/jdk11:latest
docker run -it sapmachine/jdk11:latest java -version
```

## [](#Documentation) Documentation
We have a [Wiki](https://github.com/SAP/SapMachine/wiki) with various information about:

* [Supported Platforms](https://github.com/SAP/SapMachine/wiki/Supported-platforms)
* [Certifications](https://github.com/SAP/SapMachine/wiki/Certification-and-Java-Compatibility)
* [Differences between SapMachine and OpenJDK](https://github.com/SAP/SapMachine/wiki/Differences-between-SapMachine-and-OpenJDK)
* [Features Contributed by SAP](https://github.com/SAP/SapMachine/wiki/Features-Contributed-by-SAP)
* [SapMachine Development Process](https://github.com/SAP/SapMachine/wiki/SapMachine-Development-Process)

## [](#Support) How to obtain support
Please create a [new issue](https://github.com/SAP/SapMachine/issues/new) if you find any problems.

### [](#Building) Building the project from source
If you want to build the project yourself, please follow the instructions in [building.md](https://github.com/SAP/SapMachine/blob/jdk/jdk/doc/building.md).

## [](#Repository) Repository setup
This repository contains several branches. The default *master* branch only contains this README file. The *jdk/...* branches are direct mirrors of the corresponding OpenJDK Mercurial repositories (e.g. the *jdk/jdk* branch is a mirror of *http://hg.openjdk.java.net/jdk/jdk*). Finally, the *sapmachine/...* branches are the actual source of the SapMachine releases with specific bug fixes and enhancements. We regularly (usually on a weekly base) merge the *jdk/* branches into the corresponding *sapmachine/* branches.

## [](#Contributing) Contributing
We currently do not accept external contributions for this project. If you want to improve the code or fix a bug please consider contributing directly to the upstream [OpenJDK](http://openjdk.java.net/contribute/) project. Our repositories will be regularly synced with the upstream project so any improvements in the upstream OpenJDK project will directly become visible in our project as well.

## [](#License) License
This project is run under the same licensing terms as the upstream OpenJDK project. Please see the [LICENSE](LICENSE) file in the top-level directory for more information.
