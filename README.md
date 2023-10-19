[![GitHub release (latest by date)](https://img.shields.io/github/downloads/sap/sapmachine/latest/total?label=Downloads%20of%20Latest%20Release)](https://sap.github.io/SapMachine/#download) [![DockerPulls](https://img.shields.io/docker/pulls/_/sapmachine?label=Docker%20Pulls)](https://hub.docker.com/_/sapmachine)

<img align="right" width=350 src="https://sap.github.io/SapMachine/assets/images/logo_circular.svg">

# [](#SapMachine) SapMachine
This project contains a downstream version of the [OpenJDK](http://openjdk.java.net/) project. It is used to build and maintain a SAP supported version of OpenJDK for SAP customers and partners who wish to use OpenJDK to run their applications.

We want to stress that this is clearly a "*friendly fork*". SAP is committed to ensuring the continued success of the Java platform. SAP is: 

* A member of the [JCP Executive committee](https://jcp.org/en/participation/committee) since 2001 and recently served in the [JSR 379 (Java SE 9)](https://www.jcp.org/en/jsr/detail?id=379), [JSR 383 (Java SE 18.3)](https://www.jcp.org/en/jsr/detail?id=383), [JSR 384 (Java SE 11)](https://www.jcp.org/en/jsr/detail?id=384), [JSR 386 (Java SE 12)](https://www.jcp.org/en/jsr/detail?id=386), [JSR 388 (Java SE 13)](https://www.jcp.org/en/jsr/detail?id=388), [JSR 389 (Java SE 14)](https://www.jcp.org/en/jsr/detail?id=389), [JSR 390 (Java SE 15)](https://www.jcp.org/en/jsr/detail?id=390), [JSR 391 (Java SE 16)](https://www.jcp.org/en/jsr/detail?id=391), [JSR 392 (Java SE 17)](https://www.jcp.org/en/jsr/detail?id=392), [JSR 393 (Java SE 18)](https://www.jcp.org/en/jsr/detail?id=393), [JSR 394 (Java SE 19)](https://www.jcp.org/en/jsr/detail?id=394), [JSR 395 (Java SE 20)](https://www.jcp.org/en/jsr/detail?id=395), [JSR 396 (Java SE 21)](https://www.jcp.org/en/jsr/detail?id=396) and [JSR 397 (Java SE 22)](https://www.jcp.org/en/jsr/detail?id=397) Expert Groups.

* Among the biggest external contributors to the OpenJDK project (see fix ratio for OpenJDK [11](https://blogs.oracle.com/java-platform-group/building-jdk-11-together), [12](https://blogs.oracle.com/java-platform-group/the-arrival-of-java-12), [13](https://blogs.oracle.com/java-platform-group/the-arrival-of-java-13), [14](https://blogs.oracle.com/java-platform-group/the-arrival-of-java-14), [15](https://blogs.oracle.com/java-platform-group/the-arrival-of-java-15), [16](https://inside.java/2021/03/16/the-arrival-of-java16/), [17](https://inside.java/2021/09/14/the-arrival-of-java17/), [18](https://inside.java/2022/03/22/the-arrival-of-java18/), [19](https://inside.java/2022/09/20/the-arrival-of-java-19/), [20](https://inside.java/2023/03/21/the-arrival-of-java-20/), [21](https://inside.java/2023/09/19/the-arrival-of-java-21/)).

* Leading the [OpenJDK 17 updates project](https://wiki.openjdk.java.net/display/JDKUpdates/JDK+17u) and heavily supporting the [OpenJDK 11 updates project](https://wiki.openjdk.java.net/display/JDKUpdates/JDK11u).

* Leading the [PowerPC/AIX porting project](http://openjdk.java.net/projects/ppc-aix-port/).

* Contributing as many of our features as possible to the OpenJDK project and keep the diff of this project as small as possible.

* Creating tools for developers
    * [JFR Event Collection](https://sapmachine.io/jfrevents/): Information on all JFR events for a specific JDK
    * [AP-Loader](https://github.com/jvm-profiling-tools/ap-loader): AsyncProfiler in a single cross-platform JAR


## Downloads

Check out the [Download](https://sap.github.io/SapMachine/#download) section on [https://sapmachine.io](https://sapmachine.io).

## Documentation
Check out our [FAQs](https://github.com/SAP/SapMachine/wiki/Frequently-Asked-Questions) and [wikipages](https://github.com/SAP/SapMachine/wiki) for information about:
* [Installation](https://github.com/SAP/SapMachine/wiki/Installation) and [Docker Images](https://github.com/SAP/SapMachine/wiki/Docker-Images)
* [Certifications and Java Compatibility](https://github.com/SAP/SapMachine/wiki/Certification-and-Java-Compatibility)
* [SapMachine Development Process](https://github.com/SAP/SapMachine/wiki/SapMachine-Development-Process)

## Have an issue?
If it's SapMachine specific please let us know by filing a [new issue](https://github.com/SAP/SapMachine/issues/new).

Please notice that the SapMachine [issue tracker](https://github.com/SAP/SapMachine/issues) is mainly used internally by the SapMachine team to organize its work (i.e. sync with upstream, downporting fixes, add SapMachine specific features, etc.).

General VM/JDK bugs are maintained directly in the [OpenJDK Bug System](https://bugs.openjdk.java.net/). You can open a SapMachine issue with a reference to an open or resolved OpenJDK bug if you want us to resolve the issue or downport the fix to a specific SapMachine version. If you find a general VM/JDK bug in SapMachine and don't have write access to the OpenJDK Bug System you can open an issue here and we'll take care to open a corresponding OpenJDK bug for it.

Every SapMachine release contains at least all the fixes of the corresponding OpenJDK release it is based on. You can easily find the OpenJDK base version by looking at the [SapMachine version string](https://github.com/SAP/SapMachine/wiki/Differences-between-SapMachine-and-OpenJDK#version-numbers).

You can find the [Differences between SapMachine and OpenJDK](https://github.com/SAP/SapMachine/wiki/Differences-between-SapMachine-and-OpenJDK) and the [Features Contributed by SAP](https://github.com/SAP/SapMachine/wiki/Features-Contributed-by-SAP) in the [SapMachine Wiki](https://github.com/SAP/SapMachine/wiki).

## Contributing
We currently do not accept external contributions for this project. If you want to improve the code or fix a bug please consider contributing directly to the upstream [OpenJDK](http://openjdk.java.net/contribute/) project. Our repositories will be regularly synced with the upstream project so any improvements in the upstream OpenJDK project will directly become visible in our project as well.

## License
This project is run under the same licensing terms as the upstream OpenJDK project. Please see the [LICENSE](LICENSE) file in the top-level directory for more information.
