Here is an updated version of your `CONTRIBUTING.md` file with some changes to improve clarity and organization:

# Contributing

Thank you for your interest in contributing to Lesma! In this document, you'll find guidelines, tips, and tricks to help
you contribute effectively.

## Getting Started

Before making any code changes, we strongly recommend visiting
the [Getting Started](https://lesma.org/docs/introduction/getting-started) section of the documentation to install
the necessary prerequisites.

## Feature Requests

If you have an idea for a new feature or an enhancement to
Lesma, [start a discussion](https://github.com/alinalihassan/Lesma/discussions/new) and provide detailed information
about the proposed changes and their benefits.

## Bug Reports

Before reporting a bug, please [search existing issues](https://github.com/alinalihassan/lesma/issues) to ensure it
hasn't already been reported. If you can't find a suitable
issue, [create a new one](https://github.com/alinalihassan/lesma/issues/new) and provide the following details:

* A short summary of what you were trying to achieve
* The code causing the bug
* The expected result
* The actual results
* Environment details, such as operating system and compiler version (`lesma -v`)

If possible, isolate the problem and provide just enough code to demonstrate it. Include any related information that
might help to fix the issue.

## Contributing Code

We use a standard GitHub pull request workflow. If you have already contributed to a project via GitHub pull request,
you can skip this section and proceed to the [specific details of what we ask for in a pull request](#pull-requests). If
this is your first time contributing to a project via GitHub, follow these steps:

1. Fork the Lesma repo using the GitHub website. This will create a copy of the Lesma repo under your GitHub account.
2. Clone your Lesma repo to your local machine.
3. Create a branch for your changes.
4. Make your changes on that branch.
5. Push your changes to your repo.
6. Use the GitHub website to open a pull request (PR).

Keep these points in mind:

1. Your fork doesn't automatically stay up to date with changes in the main repo.
2. Any changes you make on your PR branch will automatically appear in the PR, so always create different branches for
   multiple PRs.
3. To avoid commit history issues, always create your PR branches from the main branch.

Refer to the [official GitHub documentation](https://help.github.com/) for more information. Some useful resources
include:

* [Forking a Repo](https://help.github.com/articles/fork-a-repo/)
* [Creating a Pull Request](https://help.github.com/articles/creating-a-pull-request/)
* [Syncing a Fork](https://help.github.com/articles/syncing-a-fork/)

## Pull Requests

We use [Semantic Release](https://semantic-release.gitbook.io/semantic-release/) in our CI, which relies
on [Conventional Commits](https://www.conventionalcommits.org/) for commit messages. Our [CHANGELOG](CHANGELOG.md) with
all the changes in Lesma is generated based on Semantic Release and Git commit messages.

Please familiarize yourself with the commit message style to avoid having to rename commits during a PR:

* [Writing a Good Commit Message](http://chris.beams.io/posts/git-commit/)
* [Using Conventional Commits](https://www.conventionalcommits.org/)

When submitting a PR, remember to:

* Create one PR per feature. Don't combine unrelated changes.
* Ensure your PR is based on the latest main