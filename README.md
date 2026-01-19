[![GitHub release (latest by date)](https://img.shields.io/github/downloads/sap/sapmachine/latest/total?label=Downloads%20of%20Latest%20Release)](https://sapmachine.io/#download) [![DockerPulls](https://img.shields.io/docker/pulls/_/sapmachine?label=Docker%20Pulls)](https://hub.docker.com/_/sapmachine)

<img align="right" width=350 src="https://sapmachine.io/assets/images/logo_circular.svg">

# [](#SapMachine) SapMachine
SapMachine is a downstream fork of the [OpenJDK](https://openjdk.org/) project, aimed at providing a binary distribution of OpenJDK for SAP customers and partners.

SAP is committed to the ongoing success of the Java platform and the OpenJDK project, maintaining SapMachine in an OpenJDK-upstream-first model. Additional information regarding SAP's engagement in OpenJDK is available on the [SAP OpenJDK Engagement page](https://sapmachine.io/docs/sap-in-openjdk).

Further details about SapMachine, including *installation instructions*, *frequently asked questions*, *the maintenance and support statement*, can be found on the [documentation pages](https://sapmachine.io/docs).

## Issues
For SapMachine-specific concerns, please file a [new issue](https://github.com/SAP/SapMachine/issues/new).

General JVM/JDK bugs are managed in the [OpenJDK Bug System](https://bugs.openjdk.org/). A SapMachine issue can be opened with reference to an existing OpenJDK bug for requesting resolution or backporting the fix to a specific SapMachine version. If a general JVM/JDK bug is found in SapMachine without editor access to the OpenJDK Bug System, an issue can be opened here, and a corresponding OpenJDK bug will be filed by us.

Since SapMachine tracks the OpenJDK, every SapMachine release contains all the fixes/changes of the corresponding OpenJDK release on which it is based.

## Contributing
External contributions to this project are currently not accepted. For code improvements or bug fixes, contributions should be directed to the upstream [OpenJDK](https://openjdk.org/contribute/) project. Repositories are regularly synced with OpenJDK, ensuring that upstream improvements are realized in SapMachine.

## License
This project is run under the same licensing terms as the upstream OpenJDK project. Additional information is available in the [LICENSE](LICENSE) file in the top-level directory.


