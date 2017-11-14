# SapMachine
This project contains a downstream version of the [OpenJDK](http://openjdk.java.net/) project. It is used to build and maintain a SAP supported version of OpenJDK for SAP customers who wish to use OpenJDK in their production environments.

We want to stress the fact that this is clearly a *friendly fork*. One reason why we need this project is the need to quickly react on customer problems with new and fixed versions without having to wait on the upstream project or other distributors/packagers. The second reason for the existence of this project is to showcase and bring over features from our commercially licensed, closed source SAP JVM into the OpenJDK which can not be integrated up-stream in the short-term.

SAP is one of the biggest external contributors to the OpenJDK project and will remain fully committed to the OpenJDK. Our intention is to bring as many features as possible into the up-stream project and keep the diff of this project as small as possible.

## Requirements
Currently this project will be only supported on Linux/x86_64 platforms

## Download and Installation
You can get the latest binary builds from the [releases](https://github.com/SAP/SapMachine) area of the project. Unpack the archives and set `JAVA_HOME` / `PATH` environment variables accordingly.

If you want to build the project yourself, please follow the instructions in [`common/doc/building.html`]() or [`common/doc/building.md`]().

## How to obtain support
Please create a new issue if you find any problems.

## Contributing
We currently do not accept external contributions for this project. If you want to improve the code or fix a bug please consider contributing directly to the upstream [OpenJDK](http://openjdk.java.net/contribute/) project. Our repositories will be regularly synced with the upstream project so any improvements in the upstream OpenJDK project will directly become visible in our project as well.

## License
This project is run under the same licensing terms as the upstream OpenJDK project. Please see the [LICENSE]() file in the top-level directory for more information.
