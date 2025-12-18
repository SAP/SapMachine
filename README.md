[![GitHub release (latest by date)](https://img.shields.io/github/downloads/sap/sapmachine/latest/total?label=Downloads%20of%20Latest%20Release)](https://sapmachine.io/#download) [![DockerPulls](https://img.shields.io/docker/pulls/_/sapmachine?label=Docker%20Pulls)](https://hub.docker.com/_/sapmachine)

<img align="right" width=350 src="https://sapmachine.io/assets/images/logo_circular.svg">

# [](#SapMachine) SapMachine
SapMachine is a downstream fork of the [OpenJDK](https://openjdk.org/) project. Its purpose is to build and support a binary distribution of OpenJDK for SAP customers and partners.

While maintaining SapMachine, SAP is committed to ensuring the continued success of the Java platform and the OpenJDK project and therefore works in an OpenJDK-upstream-first model. To learn more about our engagement in the OpenJDK, visit [this site](https://sapmachine.io/docs/sap-in-openjdk).

More details about SapMachine, such as *installation instructions*, *frequently asked questions*, *the maintenance and support statement*, and more are available in the [documentation pages](https://sapmachine.io/docs).

## Have an issue?
If it's SapMachine specific please let us know by filing a [new issue](https://github.com/SAP/SapMachine/issues/new).

General JVM/JDK bugs are maintained directly in the [OpenJDK Bug System](https://bugs.openjdk.org/). You can open a SapMachine issue with a reference to an open or resolved OpenJDK bug if you want us to resolve the issue or downport the fix to a specific SapMachine version. If you find a general JVM/JDK bug in SapMachine and don't have editor access to the OpenJDK Bug System you can open an issue here and we'll take care to open a corresponding OpenJDK bug for it.

Since SapMachine tracks the OpenJDK, every SapMachine release contains all the fixes/changes of the corresponding OpenJDK release it is based on.

## Contributing
We currently do not accept external contributions for this project. If you want to improve the code or fix a bug please consider contributing directly to the upstream [OpenJDK](https://openjdk.org/contribute/) project. Our repositories will be regularly synced with OpenJDK, so any improvements in upstream will become effective in SapMachine as well.

## License
This project is run under the same licensing terms as the upstream OpenJDK project. Please see the [LICENSE](LICENSE) file in the top-level directory for more information.
