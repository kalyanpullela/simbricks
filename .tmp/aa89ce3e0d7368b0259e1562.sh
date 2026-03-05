
# Server installation script

TMP_DIR="${XDG_RUNTIME_DIR:-"/tmp"}"

DISTRO_VSCODE_VERSION="1.107.0"
DISTRO_IDE_VERSION="1.16.5"
DISTRO_COMMIT="1504c8cc4b34dbfbb4a97ebe954b3da2b5634516"
DISTRO_ID="1504c8cc4b34dbfbb4a97ebe954b3da2b5634516"
DISTRO_QUALITY="stable"
DISTRO_VSCODIUM_RELEASE=""

SERVER_APP_NAME="antigravity-server"
SERVER_INITIAL_EXTENSIONS=""
SERVER_LISTEN_FLAG="--port=0"
SERVER_DATA_DIR="$HOME/.antigravity-server"
SERVER_DIR="$SERVER_DATA_DIR/bin/$DISTRO_IDE_VERSION-$DISTRO_ID"
# Temporarily remove -insiders suffix from SERVER_APP_NAME to point insider cloudtop to stable
SERVER_SCRIPT="$SERVER_DIR/bin/$SERVER_APP_NAME"
SERVER_LOGFILE="$SERVER_DATA_DIR/.$DISTRO_ID.log"
SERVER_PIDFILE="$SERVER_DATA_DIR/.$DISTRO_ID.pid"
SERVER_TOKENFILE="$SERVER_DATA_DIR/.$DISTRO_ID.token"
SERVER_SSH_AGENT_SOCKET="$SERVER_DATA_DIR/.$DISTRO_ID-ssh-auth.sock"
SERVER_ARCH=
SERVER_CONNECTION_TOKEN=
SERVER_DOWNLOAD_URL=

LISTENING_ON=
OS_RELEASE_ID=
ARCH=
PLATFORM=
SERVER_PID=

GLIBC_VERSION_GOOD=

# Add lock mechanism
LOCK_FILE="$SERVER_DATA_DIR/.installation_lock"

# Function to acquire lock
acquire_lock() {
	exec 200>$LOCK_FILE
	echo "Waiting for lock..."
	flock 200
	echo "Lock acquired, proceeding with installation."
}

# Function to release lock
release_lock() {
	flock -u 200
	exec 200>&-
}

trap release_lock EXIT INT TERM

# Mimic output from logs of remote-ssh extension
print_install_results_and_exit() {
	if [[ $1 -eq 1 ]]; then
		echo ""
		echo "Error: installation failed."
		if [[ -f "$SERVER_LOGFILE" ]]; then
			echo "Server log:
 $(cat "$SERVER_LOGFILE")
"
		fi
	fi
	if [[ "$GLIBC_VERSION_GOOD" = "false" ]]; then
		echo "Warning: valid glibc version not found. Antigravity only supports remote connections with glibc >= 2.28, such as Ubuntu 20.04, Debian 10, or CentOS 8."
		echo ""
	fi
	echo "aa89ce3e0d7368b0259e1562: start"
	echo "exitCode==$1=="
	echo "listeningOn==$LISTENING_ON=="
	echo "connectionToken==$SERVER_CONNECTION_TOKEN=="
	echo "logFile==$SERVER_LOGFILE=="
	echo "osReleaseId==$OS_RELEASE_ID=="
	echo "arch==$ARCH=="
	echo "platform==$PLATFORM=="
	echo "tmpDir==$TMP_DIR=="
	
	echo "aa89ce3e0d7368b0259e1562: end"

	exit 0
}

print_install_results_and_wait() {
	# Check server is indeed running
	if [ -f $SERVER_PIDFILE ]; then
		SERVER_PID="$(cat $SERVER_PIDFILE)"
	else
		print_install_results_and_exit 1
	fi

	echo "aa89ce3e0d7368b0259e1562: start"
	# pretend exit code is 0
	echo "exitCode=0"
	echo "listeningOn==$LISTENING_ON=="
	echo "connectionToken==$SERVER_CONNECTION_TOKEN=="
	echo "logFile==$SERVER_LOGFILE=="
	echo "osReleaseId==$OS_RELEASE_ID=="
	echo "arch==$ARCH=="
	echo "platform==$PLATFORM=="
	echo "tmpDir==$TMP_DIR=="
	
	echo "aa89ce3e0d7368b0259e1562: end"

	release_lock

	# Wait for server to exit
	while ps -p $SERVER_PID >/dev/null 2>&1
	do
		sleep 10
	done
}

# Check if platform is supported
KERNEL="$(uname -s)"
case $KERNEL in
	Darwin)
		PLATFORM="darwin"
		;;
	Linux)
		PLATFORM="linux"
		;;
	FreeBSD)
		PLATFORM="freebsd"
		;;
	DragonFly)
		PLATFORM="dragonfly"
		;;
	*)
		echo "Error platform not supported: $KERNEL"
		print_install_results_and_exit 1
		;;
esac

# Check machine architecture
ARCH="$(uname -m)"
case $ARCH in
	x86_64 | amd64)
		SERVER_ARCH="x64"
		;;
	armv7l | armv8l)
		SERVER_ARCH="armhf"
		;;
	arm64 | aarch64)
		SERVER_ARCH="arm64"
		;;
	ppc64le)
		SERVER_ARCH="ppc64le"
		;;
	riscv64)
		SERVER_ARCH="riscv64"
		;;
	*)
		echo "Error architecture not supported: $ARCH"
		print_install_results_and_exit 1
		;;
esac

# https://www.freedesktop.org/software/systemd/man/os-release.html
OS_RELEASE_ID="$(grep -i '^ID=' /etc/os-release 2>/dev/null | sed 's/^ID=//gi' | sed 's/"//g')"
if [ -z $OS_RELEASE_ID ]; then
	OS_RELEASE_ID="$(grep -i '^ID=' /usr/lib/os-release 2>/dev/null | sed 's/^ID=//gi' | sed 's/"//g')"
	if [ -z $OS_RELEASE_ID ]; then
		OS_RELEASE_ID="unknown"
	fi
fi

# Create installation folder
if [ ! -d $SERVER_DIR ]; then
	mkdir -p $SERVER_DIR
	if (( $? > 0 )); then
		echo "Error creating server install directory"
		print_install_results_and_exit 1
	fi
fi

# Acquire lock at the beginning of the script
acquire_lock

# Add trap to release lock on exit
trap release_lock EXIT


if [ -n "$SSH_AUTH_SOCK" ]; then
	ln -s -f $SSH_AUTH_SOCK $SERVER_SSH_AGENT_SOCKET
fi
export SSH_AUTH_SOCK=$SERVER_SSH_AGENT_SOCKET

if [[ "$SERVER_ARCH" == "arm64" ]]; then
	SERVER_ARCH="arm"
fi
if [[ "$DISTRO_QUALITY" == "insider" ]]; then
	SERVER_DOWNLOAD_URL="$DISTRO_QUALITY/$DISTRO_IDE_VERSION-$DISTRO_COMMIT/$PLATFORM-$SERVER_ARCH/Antigravity - Insiders-reh.tar.gz"
else
	SERVER_DOWNLOAD_URL="$DISTRO_QUALITY/$DISTRO_IDE_VERSION-$DISTRO_COMMIT/$PLATFORM-$SERVER_ARCH/Antigravity-reh.tar.gz"
fi

if [[ "$PLATFORM" == "linux" ]]; then
	# Check ldd version based on format "ldd (.*) 2.28"
	version=$(ldd --version | head -n 1 | grep -oE '[0-9]+.[0-9]+$')
	if (( $? > 0 )); then
		echo "Warning: ldd not found"
		GLIBC_VERSION_GOOD="false"
	else
		major=$(echo "$version" | cut -d '.' -f 1)
		minor=$(echo "$version" | cut -d '.' -f 2)

		if [[ "$major" -eq 2 && "$minor" -ge 28 ]]; then
			GLIBC_VERSION_GOOD="true"
		else
			GLIBC_VERSION_GOOD="false"
		fi
	fi

	if [[ "$GLIBC_VERSION_GOOD" = "false" ]]; then
		echo "Warning: valid glibc version not found. Antigravity only supports remote connections with glibc >= 2.28, such as Ubuntu 20.04, Debian 10, or CentOS 8."
	fi
fi

# Check if server script is already installed
if [ ! -f $SERVER_SCRIPT ]; then
	if [ "$PLATFORM" != "darwin" ] && [ "$PLATFORM" != "linux" ]; then
		echo "Error "$PLATFORM" needs manual installation of remote extension host"
		print_install_results_and_exit 1
	fi

	pushd $SERVER_DIR > /dev/null

	temp_file=$(mktemp)

	DOWNLOAD_URLS=(
		"https://edgedl.me.gvt1.com/edgedl/release2/j0qc3/antigravity/$SERVER_DOWNLOAD_URL"
		"https://redirector.gvt1.com/edgedl/release2/j0qc3/antigravity/$SERVER_DOWNLOAD_URL"
		"https://edgedl.me.gvt1.com/edgedl/antigravity/$SERVER_DOWNLOAD_URL"
		"https://redirector.gvt1.com/edgedl/antigravity/$SERVER_DOWNLOAD_URL"
	)

	download_success=0
	for url in "${DOWNLOAD_URLS[@]}"; do
		if [ ! -z $(which wget) ]; then
			wget --tries=3 --timeout=10 --continue --quiet -O $temp_file "$url"
		else
			echo "Error need wget to download server binary"
			print_install_results_and_exit 1
		fi

		if (( $? == 0 )); then
			download_success=1
			break
		else
			echo "Download failed from $url, trying next URL..."
		fi
	done

	if (( download_success == 0 )); then
		echo "Error downloading server from all URLs"
		print_install_results_and_exit 1
	fi

	mv $temp_file vscode-server.tar.gz

	tar -xf vscode-server.tar.gz --strip-components 1
	if (( $? > 0 )); then
		echo "Error while extracting server contents"
		print_install_results_and_exit 1
	fi

	if [ ! -f $SERVER_SCRIPT ]; then
		echo "Error server contents are corrupted"
		print_install_results_and_exit 1
	fi

	rm -f vscode-server.tar.gz

	popd > /dev/null
else
	echo "Server script already installed in $SERVER_SCRIPT"
fi

# Try to find if server is already running
if [ -f $SERVER_PIDFILE ]; then
	SERVER_PID="$(cat $SERVER_PIDFILE)"
	SERVER_RUNNING_PROCESS="$(ps -o pid,args -p $SERVER_PID | grep $SERVER_SCRIPT)"
else
	SERVER_RUNNING_PROCESS="$(ps -o pid,args -A | grep $SERVER_SCRIPT | grep -v grep)"
	if [ -z $SERVER_RUNNING_PROCESS ]; then
		SERVER_PID=
	else
		SERVER_PID="$(echo $SERVER_RUNNING_PROCESS | cut -d ' ' -f 1 | head -n 1)"
	fi
fi

if [ -z "$SERVER_RUNNING_PROCESS" ]; then
	if [ -f "$SERVER_LOGFILE" ]; then
		rm $SERVER_LOGFILE
	fi
	if [ -f "$SERVER_TOKENFILE" ]; then
		rm $SERVER_TOKENFILE
	fi

	touch $SERVER_TOKENFILE
	chmod 600 $SERVER_TOKENFILE
	SERVER_CONNECTION_TOKEN="844538400488"
	echo $SERVER_CONNECTION_TOKEN > $SERVER_TOKENFILE

	export REMOTE_CONTAINERS=true

	$SERVER_SCRIPT --start-server --host=127.0.0.1 $SERVER_LISTEN_FLAG $SERVER_INITIAL_EXTENSIONS --connection-token-file $SERVER_TOKENFILE --telemetry-level off --enable-remote-auto-shutdown --accept-server-license-terms &> $SERVER_LOGFILE & disown
	echo $! > $SERVER_PIDFILE
else
	echo "Server script is already running $SERVER_SCRIPT"
fi

if [ -f $SERVER_TOKENFILE ]; then
	SERVER_CONNECTION_TOKEN="$(cat $SERVER_TOKENFILE)"
else
	echo "Error server token file not found $SERVER_TOKENFILE"
	print_install_results_and_exit 1
fi

if [ -f "$SERVER_LOGFILE" ]; then
	for i in {1..5}; do
		LISTENING_ON="$(cat $SERVER_LOGFILE | grep -E 'Extension host agent listening on .+' | sed 's/Extension host agent listening on //')"
		if [ -n "$LISTENING_ON" ]; then
			break
		fi
		sleep 0.5
	done

	if [ -z "$LISTENING_ON" ]; then
		echo "Error server did not start sucessfully"
		print_install_results_and_exit 1
	fi
else
	echo "Error server log file not found $SERVER_LOGFILE"
	print_install_results_and_exit 1
fi

# Finish server setup
print_install_results_and_exit 0
