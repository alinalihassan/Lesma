# Check which OS the user has
case $(uname | tr '[:upper:]' '[:lower:]') in
  linux*)
    export OS_NAME=linux
    export FILE_NAME="browser_download_url.*Linux.tar.gz"
    ;;
  darwin*)
    export OS_NAME=osx
    export FILE_NAME="browser_download_url.*Darwin.tar.gz"
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
curl -s https://api.github.com/repos/alinalihassan/Lesma/releases/latest \
| grep $FILE_NAME \
| cut -d : -f 2,3 \
| tr -d \" \
| wget -qi -

# Create Directory
mkdir -p "$HOME/.lesma"

# Untar in .lesma folder
for z in Lesma-*.tar.gz; do tar -xf $z --strip-components=1 -C "$HOME/.lesma"; done

# Remove tmp files
for z in Lesma-*.tar.gz; do rm $z; done