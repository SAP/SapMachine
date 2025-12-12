[![GitHub release (latest by date)](https://img.shields.io/github/downloads/sap/sapmachine/latest/total?label=Downloads%20of%20Latest%20Release)](https://sapmachine.io/#download) [![DockerPulls](https://img.shields.io/docker/pulls/_/sapmachine?label=Docker%20Pulls)](https://hub.docker.com/_/sapmachine)

<img align="right" width=350 src="https://sapmachine.io/assets/images/logo_circular.svg">

# [](#SapMachine) SapMachine
This project contains a downstream version of the [OpenJDK](https://openjdk.org/) project. It is used to build and maintain a SAP supported version of OpenJDK for SAP customers and partners who wish to use OpenJDK to run their applications.

We want to stress that this is clearly a "*friendly fork*". SAP is committed to ensuring the continued success of the Java platform. 

More details about the *SAP’s Engagement in the OpenJDK project*, *installation instructions*, *frequently asked questions*, *the maintenance and support statement*, and more are available in the [documentation](https://sapmachine.io/docs).

## Have an issue?
If it's SapMachine specific please let us know by filing a [new issue](https://github.com/SAP/SapMachine/issues/new).

Please notice that the SapMachine [issue tracker](https://github.com/SAP/SapMachine/issues) is mainly used internally by the SapMachine team to organize its work (i.e. sync with upstream, downporting fixes, add SapMachine specific features, etc.).

General VM/JDK bugs are maintained directly in the [OpenJDK Bug System](https://bugs.openjdk.java.net/). You can open a SapMachine issue with a reference to an open or resolved OpenJDK bug if you want us to resolve the issue or downport the fix to a specific SapMachine version. If you find a general VM/JDK bug in SapMachine and don't have write access to the OpenJDK Bug System you can open an issue here and we'll take care to open a corresponding OpenJDK bug for it.

Every SapMachine release contains at least all the fixes of the corresponding OpenJDK release it is based on.

## Contributing
We currently do not accept external contributions for this project. If you want to improve the code or fix a bug please consider contributing directly to the upstream [OpenJDK](http://openjdk.java.net/contribute/) project. Our repositories will be regularly synced with the upstream project so any improvements in the upstream OpenJDK project will directly become visible in our project as well.

## License
This project is run under the same licensing terms as the upstream OpenJDK project. Please see the [LICENSE](LICENSE) file in the top-level directory for more information.
