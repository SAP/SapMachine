<img align="right" src="https://sap.github.io/SapMachine/logo_title.png">

# SapMachine
This project contains a downstream version of the [OpenJDK](http://openjdk.java.net/) project. It is used to build and maintain a SAP supported version of OpenJDK for SAP customers who wish to use OpenJDK in their production environments.

We want to stress the fact that this is clearly a *friendly fork*. One reason why we need this project is the need to quickly react on customer problems with new and fixed versions without having to wait on the upstream project or other distributors/packagers. The second reason for the existence of this project is to showcase and bring over features from our commercially licensed, closed source SAP JVM into the OpenJDK which can not be integrated upstream in the short-term.

SAP is committed to ensuring the continued success of the Java platform. We are members of the [JCP Executive committee](https://jcp.org/en/participation/committee) since 2001 and recently served in the [JSR 379 (Java SE 9)](https://www.jcp.org/en/jsr/detail?id=379) and [JSR 383 (Java SE 18.3)](https://www.jcp.org/en/jsr/detail?id=383) Expert Groups. SAP is also one of the biggest external contributors to the OpenJDK project (currently leading the [PowerPC/AIX](http://openjdk.java.net/projects/ppc-aix-port/) and [s390](http://openjdk.java.net/projects/s390x-port/) porting projects) and will remain fully committed to the OpenJDK. Our intention is to bring as many features as possible into the upstream project and keep the diff of this project as small as possible.

## Requirements
Currently this project only supports Linux/x86_64.

## Download and Installation
Download the [latest tagged version](https://github.com/SAP/SapMachine/releases/latest) or check for all available [builds](https://github.com/SAP/SapMachine/releases) (including nightly snapshots) in the release section of the project.

Unpack the archives and set `JAVA_HOME` / `PATH` environment variables accordingly.

If you want to build the project yourself, please follow the instructions in [`building.md`](https://github.com/SAP/SapMachine/blob/jdk/jdk/doc/building.md).

## How to obtain support
Please create a [new issue](https://github.com/SAP/SapMachine/issues/new) if you find any problems.

## Contributing
We currently do not accept external contributions for this project. If you want to improve the code or fix a bug please consider contributing directly to the upstream [OpenJDK](http://openjdk.java.net/contribute/) project. Our repositories will be regularly synced with the upstream project so any improvements in the upstream OpenJDK project will directly become visible in our project as well.

## License
This project is run under the same licensing terms as the upstream OpenJDK project. Please see the [LICENSE](LICENSE) file in the top-level directory for more information.
