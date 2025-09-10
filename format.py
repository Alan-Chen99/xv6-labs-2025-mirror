#!/usr/bin/env python

"""
use `uv sync` to install dependencies to use this script
"""
import functools
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Annotated

import cappa
from cappa import Arg
from pygit2 import GIT_OBJECT_BLOB
from pygit2 import Blob
from pygit2 import Commit
from pygit2 import Oid
from pygit2 import Repository
from pygit2 import Tree

sys.setrecursionlimit(2000)

REPO_DIR = Path(__file__).parent.resolve()
repo = Repository(REPO_DIR / ".git")

verbose = 0


def run_command(*args: str, stdin: bytes):
    if verbose >= 1:
        print("++", shlex.join(args))
    return subprocess.run(
        args,
        input=stdin,
        stdout=subprocess.PIPE,
        check=True,
        cwd=REPO_DIR,
    ).stdout


@functools.cache
def format_one(path: Path, content: bytes) -> bytes:
    ext = path.suffix

    if ext in [".c", ".h"]:
        content = run_command("clang-format", "-assume-filename", str(path), stdin=content)

        # writing of this script causes this to be discovered
        # https://github.com/llvm/llvm-project/issues/157976
        while True:
            ans = run_command("clang-format", "-assume-filename", str(path), stdin=content)
            if ans == content:
                return ans
            content = ans

    if ext in [".py"]:
        content = run_command("black", "--stdin-filename", str(path), "-q", "-", stdin=content)
        content = run_command("isort", "--filename", str(path), "-q", "-", stdin=content)
        return content

    if ext in [".toml"]:
        content = run_command("taplo", "fmt", "--stdin-filepath", str(path), "-", stdin=content)

    if verbose >= 1:
        print(f"unchanged: {path}")
    return content


def _map_tree(tree: Tree, current_path: Path) -> Oid:
    tree_builder = repo.TreeBuilder()
    for entry in tree:
        assert entry.name is not None
        entry_path = current_path / entry.name
        if isinstance(entry, Blob):
            content = entry.data
            new_content = format_one(entry_path, content)
            new_blob_id = repo.write(GIT_OBJECT_BLOB, new_content)
            tree_builder.insert(entry.name, new_blob_id, entry.filemode)
        elif isinstance(entry, Tree):
            new_subtree_id = _map_tree(entry, entry_path)
            tree_builder.insert(entry.name, new_subtree_id, entry.filemode)
        else:
            tree_builder.insert(entry.name, entry.id, entry.filemode)
    return tree_builder.write()


@dataclass
class Worktree:
    """
    format the current worktree
    """

    def call(self) -> None:
        global verbose
        verbose = 1

        for fpath_b in subprocess.run(
            ["git", "ls-files", "--exclude-standard"],
            stdout=subprocess.PIPE,
            check=True,
            cwd=REPO_DIR,
        ).stdout.splitlines():
            path = Path(fpath_b.decode()).resolve()
            src = path.read_bytes()
            res = format_one(path, src)
            if res != src:
                path.write_bytes(res)


@dataclass
class History:
    """
    format the entire git history, and write to OUT_REF; does not modify the worktree

    configuration (.clang-format, etc) from the current worktree is used.

    example:
    `./format.py history -i refs/remotes/origin/riscv -o refs/heads/riscv-formatted`
    """

    in_ref: Annotated[str, Arg(short="-i", long="--in-ref")] = "HEAD"
    out_ref: Annotated[str, Arg(short="-o", long="--out-ref")] = "refs/heads/autoformat-output"
    force: bool = False

    def call(self) -> None:
        in_ref = repo.references.get(self.in_ref)
        if in_ref is None:
            print(f"input reference {self.in_ref} does not exist")
            sys.exit(1)
        in_commit = in_ref.peel(Commit)

        prev_out_ref = repo.references.get(self.out_ref)

        if not self.force and prev_out_ref:
            print(f"reference {self.out_ref} already exist!")
            print("note: use --force to overwrite the existing reference")
            sys.exit(2)

        @functools.cache
        def handle_one(cur: Commit) -> Oid:
            parents = [handle_one(x) for x in cur.parents]

            print(f"processing: {cur.short_id} {cur.message}")

            tree = cur.tree
            new_tree = _map_tree(tree, REPO_DIR)
            new_commit_id = repo.create_commit(
                None,
                cur.author,
                cur.committer,
                cur.message,
                new_tree,
                parents,
            )
            return new_commit_id

        out_commit = handle_one(in_commit)

        repo.create_reference(self.out_ref, out_commit, force=self.force)
        print(f"success! output written to {self.out_ref}")


@dataclass
class CheckHistory:
    ref: str = "HEAD"

    def call(self) -> None:

        ref = repo.references.get(self.ref)
        if ref is None:
            print(f"{self.ref} does not exist")
            sys.exit(1)
        commit = ref.peel(Commit)

        @functools.cache
        def handle_one(cur: Commit):
            print(f"processing: {cur.short_id} {cur.message}")

            tree = cur.tree
            correct_tree = _map_tree(tree, REPO_DIR)

            if tree.id != correct_tree:
                raise RuntimeError(f"format mismatch for commit {cur.short_id}")

            for x in cur.parents:
                handle_one(x)

        handle_one(commit)


@dataclass
class GithubActions:
    """
    for use on github actions only
    """

    def call(self) -> None:
        if os.getenv("GITHUB_ACTIONS") != "true":
            sys.exit("Aborting: not running in GitHub Actions.")

        repo_name = os.environ["GITHUB_REPOSITORY"]
        if repo_name != "Alan-Chen99/xv6-labs-2025-mirror":
            sys.exit("forks most likely do not intend to run this actions")

        #####

        pull_remote = repo.remotes.create("_scripted_pull", "git://g.csail.mit.edu/xv6-labs-2025")
        pull_remote.fetch()

        pull_remote_prefix = "refs/remotes/_scripted_pull/"

        #####

        token = os.environ["GITHUB_TOKEN"]
        push_url = f"https://x-access-token:{token}@github.com/{repo_name}.git"
        push_remote = repo.remotes.create_anonymous(push_url)

        #####

        specs: list[str] = []

        for ref in repo.references:
            if ref.startswith(pull_remote_prefix):
                branch = ref.split(pull_remote_prefix)[-1]

                print()
                print(f"PROCESSING BRANCH {branch}")
                print()

                History(ref, f"refs/heads/{branch}").call()

                specs.append(f"refs/heads/{branch}:refs/heads/{branch}")

        push_remote.push(specs)


@cappa.command(name=Path(__file__).name)
@dataclass
class Cli:
    cmd: cappa.Subcommands[Worktree | History | CheckHistory | GithubActions]

    def call(self) -> None:
        self.cmd.call()


if __name__ == "__main__":
    cli = cappa.parse(Cli)
    cli.call()
