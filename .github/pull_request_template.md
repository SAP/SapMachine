<!-- General instructions for SapMachine PRs:

1. The title of a PR should be "SapMachine (<Release>) #<Issue Number>: <Description>"
2. A PR needs to refer to an issue in the project by adding the issue number in the last line of the PR body in the form of 'fixes #<Issue Number>
3. When integrating a PR, please make sure you:
- Create a merge commit when merging an OpenJDK upstream PR
- Use Rebase & Merge when your PR only contains commits with meaningful commit message, e.g. of the form `SapMachine #<Issue Number>: <Description>` for SapMachine specific changes or the original commit messages for cherry-picks from OpenJDK
- Use Squash and Merge when there are several commits on the PR. Update the commit message to `SapMachine #<Issue Number>: <Description>` and remove unnecessary commit messages from sub-commits
 -->

<!-- Replace the following line with a description of this pull request -->
Description

<!-- replace #Issue with the issue number that the PR is referring to. Otherwise PR testing will fail. -->
fixes #Issue
