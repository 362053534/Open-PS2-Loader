#!/bin/bash

## Download lwNBD
REPO_URL="https://github.com/362053534/lwNBD.git"
REPO_FOLDER="modules/network/lwNBD"
COMMIT="f3f96d007c497a85467583101945e8c806e5135d"
if test ! -d "$REPO_FOLDER"; then
  git clone $REPO_URL "$REPO_FOLDER" || { exit 1; }
  (cd $REPO_FOLDER && git checkout "$COMMIT" && cd -) || { exit 1; }
else
  (cd "$REPO_FOLDER" && git fetch origin && git checkout "$COMMIT" && cd - )|| exit 1
fi
