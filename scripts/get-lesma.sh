#!/bin/bash

# Parse command-line flags
NON_INTERACTIVE=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --non-interactive)
      NON_INTERACTIVE=1
      shift
      ;;
    *)
      echo "Unknown flag: $1"
      exit 1
      ;;
  esac
done

ask() {
  if [ "$NON_INTERACTIVE" -eq 1 ]; then
    return 0
  fi

  while true; do
    read -rp "$1 [Y/n] " answer
    case $(echo "$answer" | tr "[A-Z]" "[a-z]") in
      y|yes|"" ) return 0;;
      n|no     ) return 1;;
    esac
  done
}

# Exit if any command fails
set -e

# Check which OS the user has
case $(uname | tr '[:upper:]' '[:lower:]') in
  linux*)
    export OS_NAME=linux
    export FILE_NAME="browser_download_url.*Linux-x86_64.tar.gz"
    ;;
  darwin*)
    export OS_NAME=osx
    export FILE_NAME="browser_download_url.*Darwin-x86_64.tar.gz"
    ;;
  msys*)
    export OS_NAME=windows
    ;;
  *)
    export OS_NAME=notset
    ;;
esac

# Stop installation if it's an unsupported OS
if [[ "$OS_NAME" == windows || "$OS_NAME" == notset ]]; then
  echo "Lesma is not yet supported on Windows or other operating systems."
  exit 1
fi

# Download latest asset for the platform
echo "Downloading the latest Lesma release"
curl -s https://api.github.com/repos/alinalihassan/Lesma/releases/latest \
| grep "$FILE_NAME" \
| cut -d : -f 2,3 \
| tr -d \" \
| wget -qi -

# Create Directory
echo "Creating directory at $HOME/.lesma"
mkdir -p "$HOME/.lesma"

# Untar in .lesma folder and remove tmp files
echo "Unzipped binary and standard library files"
for z in Lesma-*.tar.gz; do
  tar -xf "$z" --strip-components=1 -C "$HOME/.lesma"
  rm "$z"
done

# Add directory to PATH
if [[ ":$PATH:" == *":$HOME/.lesma/bin:"* ]]; then
  echo "Lesma already in PATH"
else
  if ask "Would you like to add Lesma to PATH?" >/dev/null; then
    if [ -f "$HOME/.bashrc" ]; then
      echo "export PATH=\$PATH:\$HOME/.lesma/bin" >> "$HOME/.bashrc"
      echo "Added Lesma to PATH in .bashrc, please run 'source ~/.bashrc' to reload shell"
    fi

    if [ -f "$HOME/.zshrc" ]; then
      echo "export PATH=\$PATH:\$HOME/.lesma/bin" >> "$HOME/.zshrc"
      echo "Added Lesma to PATH in .zshrc, please run 'source ~/.zshrc' to reload shell"
    fi

    if [[ ! -f "$HOME/.zshrc" && ! -f "$HOME/.bashrc" ]]; then
      echo "No suitable shell found, please add $HOME/.lesma to PATH"
    fi
  fi
fi
