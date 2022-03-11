# Contributing

It's good to hear that you want to contribute to Lesma! Below are some guidelines, tips and tricks to do just that!

## Getting Started

To get started, if you plan to make changes related to parts other than documentation, we strongly suggest to visit the
[Getting Started](https://lesma.alinalihassan.com/docs/introduction/getting-started) part of the documentation to install
the prerequisites.

## Feature Request

For any feature requests or enhancements to Lesma, [suggest your change](https://github.com/alinalihassan/Lesma/discussions/new) and write in detail the changes and the benefits of such a change.

## Bug Report

Please [search existing issues](https://github.com/alinalihassan/lesma/issues) to make sure your issue hasn't already been reported. If you cannot find a suitable issue, [create a new one](https://github.com/alinalihassan/lesma/issues/new).

Provide the following details:

* short summary of what you was trying to achieve,
* a code causing the bug,
* expected result,
* actual results and
* environment details: at least operating system and compiler version (`lesma -v`).

If possible, try to isolate the problem and provide just enough code to demonstrate it. Add any related information which might help to fix the issue.

## How To Contribute

We use a fairly standard GitHub pull request workflow. If you have already contributed to a project via GitHub pull request, you can skip this section and proceed to the [specific details of what we ask for in a pull request](#pull-request). If this is your first time contributing to a project via GitHub, read on.

Here is the basic GitHub workflow:

1. Fork the Lesma repo. you can do this via the GitHub website. This will result in you having your own copy of the Lesma repo under your GitHub account.
2. Clone your Lesma repo to your local machine.
3. Make a branch for your change.
4. Make your change on that branch.
5. Push your change to your repo.
6. Use the GitHub website to open a PR.

Some things to note that aren't immediately obvious to folks just starting out:

1. Your fork doesn't automatically stay up to date with change in the main repo.
2. Any changes you make on your branch that you used for the PR will automatically appear in the PR so if you have more than 1 PR, be sure to always create different branches for them.
3. Weird things happen with commit history if you don't create your PR branches off of main so always make sure you have the main branch checked out before creating a branch for a PR

You can read more about GitHub via [the official documentation](https://help.github.com/). Some highlights include:

* [Fork A Repo](https://help.github.com/articles/fork-a-repo/)
* [Creating a pull request](https://help.github.com/articles/creating-a-pull-request/)
* [Syncing a fork](https://help.github.com/articles/syncing-a-fork/)

## Pull Request

Before making a pull request, please check how commit messages should be structured, since we're using [Semantic Release](https://semantic-release.gitbook.io/semantic-release/) in our CI based on [Conventional Commits](https://www.conventionalcommits.org/).
Our [CHANGELOG](CHANGELOG.md) with all the changes in Lesma is based on Semantic Release and the commit messages in Git.

Please get familiar with the commit messaging style to avoid having to rename commits during a Pull Request.

* [Write a good commit message](http://chris.beams.io/posts/git-commit/).
* [Use Conventional Commits](https://www.conventionalcommits.org/).
* Issue 1 Pull Request per feature. Don't lump unrelated changes together.