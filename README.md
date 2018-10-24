<img align="right" width=350 src="https://sap.github.io/SapMachine/assets/images/logo_circular.png">

# SapMachine
This project contains a downstream version of the [OpenJDK](http://openjdk.java.net/) project. It is used to build and maintain a SAP supported version of OpenJDK for SAP customers who wish to use OpenJDK in their production environments.

We want to stress the fact that this is clearly a *friendly fork*. One reason why we need this project is the need to quickly react on customer problems with new and fixed versions without having to wait on the upstream project or other distributors/packagers. The second reason for the existence of this project is to showcase and bring over features from our commercially licensed, closed source SAP JVM into the OpenJDK which can not be integrated upstream in the short-term.

SAP is committed to ensuring the continued success of the Java platform. We are members of the [JCP Executive committee](https://jcp.org/en/participation/committee) since 2001 and recently served in the [JSR 379 (Java SE 9)](https://www.jcp.org/en/jsr/detail?id=379) and [JSR 383 (Java SE 18.3)](https://www.jcp.org/en/jsr/detail?id=383) Expert Groups. SAP is also one of the biggest external contributors to the OpenJDK project (currently leading the [PowerPC/AIX](http://openjdk.java.net/projects/ppc-aix-port/) and [s390](http://openjdk.java.net/projects/s390x-port/) porting projects) and will remain fully committed to the OpenJDK. Our intention is to bring as many features as possible into the upstream project and keep the diff of this project as small as possible.

## [](#Documentation) Documentation
We have a [Wiki](https://github.com/SAP/SapMachine/wiki) with various information about:

* [Certifications](https://github.com/SAP/SapMachine/wiki/Certification-and-Java-Compatibility)
* [Differences between SapMachine and OpenJDK](https://github.com/SAP/SapMachine/wiki/Differences-between-SapMachine-and-OpenJDK)
* [Features Contributed by SAP](https://github.com/SAP/SapMachine/wiki/Features-Contributed-by-SAP)
* [SapMachine Development Process](https://github.com/SAP/SapMachine/wiki/SapMachine-Development-Process)

## Requirements
Currently this project supports the following operating systems/CPU architectures:

* Linux/x86_64
* Linux/ppc64
* Linux/ppc64le
* Windows/x86_64
* MacOS/x64

## Download and Installation
Download the [latest released version](https://github.com/SAP/SapMachine/releases/latest) or check all available [builds](https://github.com/SAP/SapMachine/releases) (including nightly snapshots) in the release section of the project. Unpack the archives and set `JAVA_HOME` / `PATH` environment variables accordingly.

Alternatively, you can use our `.deb` packages if you're on Debian or Ubuntu:

```
sudo bash
wget -q -O - https://dist.sapmachine.io/debian/sapmachine.key | apt-key add -
echo "deb http://dist.sapmachine.io/debian/amd64/ ./" >> /etc/apt/sources.list
apt-get update
apt-get install sapmachine-11-jre
```

Finally, we also provide Docker images for various versions of the SapMachine at https://hub.docker.com/r/sapmachine

##### [](#Debian) Debian / Ubuntu

```
docker pull sapmachine/jdk11:latest
docker run -it sapmachine/jdk11:latest java -version
```

## Repository setup

This repository contains several branches. The default *master* branch only contains this README file. The *jdk/...* branches are direct mirrors of the corresponding OpenJDK Mercurial repositories (e.g. the *jdk/jdk* branch is a mirror of *http://hg.openjdk.java.net/jdk/jdk*). Finally, the *sapmachine/...* branches are the actual source of the SapMachine releases with specific bug fixes and enhancements. We regularly (usually on a weekly base) merge the *jdk/* branches into the corresponding *sapmachine/* branches.

## How to obtain support
Please create a [new issue](https://github.com/SAP/SapMachine/issues/new) if you find any problems.

## Contributing
We currently do not accept external contributions for this project. If you want to improve the code or fix a bug please consider contributing directly to the upstream [OpenJDK](http://openjdk.java.net/contribute/) project. Our repositories will be regularly synced with the upstream project so any improvements in the upstream OpenJDK project will directly become visible in our project as well.

## License
This project is run under the same licensing terms as the upstream OpenJDK project. Please see the [LICENSE](LICENSE) file in the top-level directory for more information.
